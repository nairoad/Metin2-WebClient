// SPDX-License-Identifier: GPL-2.0-or-later
// win32_compat.cpp - the body of the compatibility layer: files and
// mappings on POSIX, the clock, `Sleep` as a deadline for the main loop,
// directories, the clipboard, global memory, GDI leftovers. See the header.

// Design:
// Everything here has an EXACT POSIX counterpart or is documented as
// deliberately empty (the clipboard, critical sections, the console). What
// needs a decision does not get a body here - `win32_compat.h` explains
// which declarations stay without one, and why. `Sleep` is the one
// exception with its own design, described at its body.

#include "win32_compat.h"
#include "stubs.h"

#include <cctype>
#include <cstdio>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include <chrono>
#include <string>
#include <vector>

namespace win32compat {
namespace {

/// The list of live mappings. Win32 `UnmapViewOfFile` takes the address
/// alone, POSIX `munmap` also needs the length - hence the list. The client
/// has a handful of mappings (the data packs), so a linear search is the
/// right thing here, not a compromise.
std::vector<Mapping>& Mappings()
{
    static std::vector<Mapping> v;
    return v;
}

}  // namespace

/// The live mapping at `addr`, or null.
Mapping* FindMapping(const void* addr)
{
    for (Mapping& m : Mappings()) {
        if (m.addr == addr) return &m;
    }
    return nullptr;
}

/// Records a mapping after `mmap`, so that `UnmapViewOfFile` knows its length.
void RememberMapping(void* addr, size_t length)
{
    Mappings().push_back(Mapping{addr, length});
}

/// Drops the record before `munmap`.
void ForgetMapping(const void* addr)
{
    std::vector<Mapping>& v = Mappings();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].addr == addr) {
            v[i] = v.back();
            v.pop_back();
            return;
        }
    }
}

}  // namespace win32compat

// --- files ------------------------------------------------------------------

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD /*share*/, void* /*sec*/,
                   DWORD creation, DWORD /*flags*/, HANDLE /*tmpl*/)
{
    int oflag = 0;
    const bool wantRead  = (access & GENERIC_READ)  != 0;
    const bool wantWrite = (access & GENERIC_WRITE) != 0;

    if (wantRead && wantWrite)      oflag = O_RDWR;
    else if (wantWrite)             oflag = O_WRONLY;
    else                            oflag = O_RDONLY;

    if (creation == CREATE_ALWAYS)  oflag |= O_CREAT | O_TRUNC;

    // THE PATH GOES THROUGH THE LAYER, not straight to `open`.
    //
    // MOST of the game's resources pass here: `CMappedFile` opens files
    // with `CreateFile`, and through `CMappedFile` reads
    // `CEterPackManager` - textures, models, effects and maps.
    //
    // Earlier this line called `::open(name, ...)` directly, so it
    // bypassed everything the path layer knows: cutting the drive letter
    // (`d:/ymir work/...`), turning backslashes, resolving letter case. The
    // symptom was misleading - the log of missing files showed THREE
    // entries although the client failed to find thousands. It failed
    // silently, because this road did not lead through the log.
    //
    // On WRITE only the slashes are straightened: a file that does not
    // exist yet cannot be looked up by another letter case, and the attempt
    // would hit someone else's file and overwrite it.
    const char* c_szPath = wantWrite ? M2W_PosixPath(name)
                                     : M2W_ResolvePathCase(name);

    const int fd = ::open(c_szPath, oflag, 0644);
    if (fd < 0) {
        return INVALID_HANDLE_VALUE;
    }
    return win32compat::FdToHandle(fd);
}

BOOL CloseHandle(HANDLE h)
{
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return FALSE;
    return ::close(win32compat::HandleToFd(h)) == 0 ? TRUE : FALSE;
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD toRead, DWORD* read, void*)
{
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    const ssize_t n = ::read(win32compat::HandleToFd(h), buf, toRead);
    if (n < 0) {
        if (read) *read = 0;
        return FALSE;
    }
    if (read) *read = static_cast<DWORD>(n);
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD toWrite, DWORD* written, void*)
{
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    const ssize_t n = ::write(win32compat::HandleToFd(h), buf, toWrite);
    if (n < 0) {
        if (written) *written = 0;
        return FALSE;
    }
    if (written) *written = static_cast<DWORD>(n);
    return TRUE;
}

DWORD GetFileSize(HANDLE h, DWORD* hi)
{
    if (h == INVALID_HANDLE_VALUE) return INVALID_FILE_SIZE;
    struct stat st;
    if (::fstat(win32compat::HandleToFd(h), &st) != 0) return INVALID_FILE_SIZE;
    if (hi) *hi = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    return static_cast<DWORD>(st.st_size & 0xFFFFFFFFu);
}

DWORD SetFilePointer(HANDLE h, LONG dist, LONG* distHi, DWORD method)
{
    if (h == INVALID_HANDLE_VALUE) return INVALID_SET_FILE_POINTER;
    int whence = SEEK_SET;
    if (method == FILE_CURRENT) whence = SEEK_CUR;
    if (method == FILE_END)     whence = SEEK_END;

    int64_t offset = dist;
    if (distHi) {
        offset = (static_cast<int64_t>(*distHi) << 32) | static_cast<uint32_t>(dist);
    }
    const off_t r = ::lseek(win32compat::HandleToFd(h), static_cast<off_t>(offset), whence);
    if (r == static_cast<off_t>(-1)) return INVALID_SET_FILE_POINTER;
    if (distHi) *distHi = static_cast<LONG>(static_cast<uint64_t>(r) >> 32);
    return static_cast<DWORD>(static_cast<uint64_t>(r) & 0xFFFFFFFFu);
}

/// Win32 separates "create the mapping" from "show a piece". POSIX has only
/// `mmap`. So the file handle is passed on and all the work is done in
/// `MapViewOfFile` - no state has to be remembered between the calls.
HANDLE CreateFileMappingA(HANDLE file, void*, DWORD, DWORD, DWORD, LPCSTR)
{
    // Failure is signalled by NULL, not INVALID_HANDLE_VALUE - as in Win32.
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    return file;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD, DWORD offHi, DWORD offLo, size_t bytes)
{
    if (mapping == nullptr || mapping == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = win32compat::HandleToFd(mapping);

    size_t length = bytes;
    if (length == 0) {
        struct stat st;
        if (::fstat(fd, &st) != 0) return nullptr;
        length = static_cast<size_t>(st.st_size);
    }
    if (length == 0) return nullptr;

    const off_t off = static_cast<off_t>((static_cast<uint64_t>(offHi) << 32) | offLo);
    void* p = ::mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, off);
    if (p == MAP_FAILED) return nullptr;

    win32compat::RememberMapping(p, length);
    return p;
}

BOOL UnmapViewOfFile(LPCVOID addr)
{
    win32compat::Mapping* m = win32compat::FindMapping(addr);
    if (!m) return FALSE;
    const size_t length = m->length;
    win32compat::ForgetMapping(addr);
    return ::munmap(const_cast<void*>(addr), length) == 0 ? TRUE : FALSE;
}

// --- time -------------------------------------------------------------------

namespace {

/// Nanoseconds of `std::chrono::steady_clock` - the one clock behind
/// `GetTickCount`, `timeGetTime`, `QueryPerformanceCounter` and the `Sleep` deadline.
int64_t NowNanoseconds()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

DWORD GetTickCount(void)
{
    return static_cast<DWORD>(NowNanoseconds() / 1000000);
}

DWORD timeGetTime(void)
{
    return GetTickCount();
}

/// Win32 gives the counter and the frequency separately. Nanoseconds and a
/// frequency of `1e9` are given, so the quotient comes out in seconds - as
/// the calling code expects.
BOOL QueryPerformanceCounter(int64_t* out)
{
    if (!out) return FALSE;
    *out = NowNanoseconds();
    return TRUE;
}

BOOL QueryPerformanceFrequency(int64_t* out)
{
    if (!out) return FALSE;
    *out = 1000000000LL;
    return TRUE;
}

// THE GAME'S FRAME LIMITER - see `Sleep` below.
static double g_dM2wSleepDeadline = 0.0;

#ifdef __EMSCRIPTEN__
/// `?fps=N` - an artificial frame limit for tests:
/// loop turns faster than 1000/N ms are skipped, as on a slow monitor.
EM_JS(double, m2w_fps_limit, (void), { return m2w.fpsLimit(); });
#else
static double m2w_fps_limit(void) { return 0.0; }
#endif

#ifdef __EMSCRIPTEN__
/// `?events=loop` - the old road of the event
/// frame counter (one event frame per loop turn), for A/B (stage_port.py).
EM_JS(int, m2w_events_from_loop, (void), { return m2w.eventsFromLoop(); });
#else
static int m2w_events_from_loop(void) { return 0; }
#endif

/// 1 = movement events counted as in the original (one frame per
/// loop turn) instead of from time - the A/B switch, read once.
extern "C" int M2W_EventsFromLoop()
{
    static int s_i = -1;
    if (s_i < 0)
        s_i = m2w_events_from_loop();
    return s_i;
}

bool M2W_LoopStillSleeping()
{
    static double s_dLimitMs = -1.0;
    static double s_dLast = 0.0;
    if (s_dLimitMs < 0.0)
    {
        const double f = m2w_fps_limit();
        s_dLimitMs = (f > 0.0) ? 1000.0 / f : 0.0;
        if (s_dLimitMs > 0.0)
            std::printf("m2w loop: artificial limit %.0f FPS (?fps=)\n", f);
    }
    const double dNow = NowNanoseconds() / 1e6;
    if (s_dLimitMs > 0.0)
    {
        if (dNow - s_dLast < s_dLimitMs - 1.0)
            return true;
        s_dLast = dNow;
    }
    if (g_dM2wSleepDeadline <= 0.0)
        return false;
    if (dNow < g_dM2wSleepDeadline)
        return true;
    g_dM2wSleepDeadline = 0.0;
    return false;
}

void Sleep(DWORD ms)
{
    // SLEEP IS A LIMITER, NOT A VOID - BUT IT NO LONGER LIMITS THE GAME'S
    // PACE.
    //
    // History: earlier the game had its OWN TIME
    // (`CTimer::UseCustomTime()`, a fixed 16-17 ms per loop turn), and the
    // pace of 60 turns/s was held by `Sleep(rest)` in
    // `PythonApplication.cpp` (~line 896). On a 117 Hz monitor, without
    // that sleep, the world ran 1.95 times too fast (measured:
    // model clock 1.92-1.98 s per second of wall time). The game was switched to REAL TIME: at 60 Hz the fixed step was
    // shorter than a turn, the game was "forever late" and skipped drawing
    // every frame. With real time `rest = s_uiNextFrameTime - now` is
    // always <= 0 (the next frame == the moment of `Advance()`, and `now`
    // is read after the frame's work), so THAT `Sleep(rest)` path is dead;
    // the animation pace follows the time delta, not the turn count.
    //
    // The deadline mechanism stays for the OTHER callers (e.g.
    // `Sleep(g_iLoadingDelayTime)` in AreaLoaderThread, `app.Sleep` from
    // Python): the thread must not block, so the DEADLINE is stored and the
    // loop turn checks it on entry and yields at once before the deadline
    // (`M2W_LoopStillSleeping`).
    if (ms > 0)
        g_dM2wSleepDeadline = NowNanoseconds() / 1e6 + (double)ms;
    static int s_iReports = 0;
    if (ms >= 20 && s_iReports < 20)
    {
        ++s_iReports;
        std::printf("m2w Sleep(%u) - the loop sleeps\n", (unsigned)ms);
    }
    // In the browser the main thread must not block. This stays an
    // **explicitly empty** operation, not usleep - blocking would hang the
    // tab.
    //
    // THIS IS NOT A STUB IN THE BAD SENSE - it is the right answer. But it
    // has a SIDE EFFECT one has to know about, and earlier nobody
    // knew, because the function was silent.
    //
    // (The note, kept as history above
    // are the sequel.) `PythonApplication.cpp` uses `Sleep(rest)` as the
    // FRAME LIMITER: it counts how much time is left until the next frame
    // and sleeps that long. Here it does not sleep, so **the game's limiter
    // is dead**. The pace is set solely by the browser's
    // `requestAnimationFrame`.
    //
    // On a sixty-hertz screen it comes to the same. On a faster one the
    // game loop runs more often than the game intended - the effects of
    // which we DID NOT MEASURE then. Written down as an open question, not
    // as "works".

    (void)ms;
}

// --- diagnostics ------------------------------------------------------------

DWORD GetLastError(void)
{
    return static_cast<DWORD>(errno);
}

void OutputDebugStringA(LPCSTR text)
{
    if (text) std::fputs(text, stderr);
}

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT)
{
    std::fprintf(stderr, "[MessageBox] %s: %s\n",
                 caption ? caption : "", text ? text : "");
    return 1;   // IDOK
}

DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD size)
{
    if (!buf || size == 0) return 0;
    // In the browser there is no path of the executable. A conventional
    // name is returned, so that code building paths relative to it gets
    // something predictable.
    const char* kFake = "/metin2.wasm";
    const size_t n = std::strlen(kFake);
    if (n + 1 > size) return 0;
    std::memcpy(buf, kFake, n + 1);
    return static_cast<DWORD>(n);
}

// ---------------------------------------------------------------------------
// SUPPLEMENT - see the header.
// ---------------------------------------------------------------------------

#include <dirent.h>

namespace {

/// The state of a directory walk. Win32 hides it behind a `HANDLE`, POSIX
/// has `DIR*`.
struct FindState {
    DIR*        dir;
    std::string prefix;   ///< the directory, so an entry can be checked for being one
};

}  // namespace

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATA* data)
{
    if (!pattern || !data) return INVALID_HANDLE_VALUE;

    // Win32 takes a pattern (`dir\*.*`); POSIX opens the directory itself.
    // So the part before the last separator is taken and the mask IGNORED
    // - TMP4 code uses only `*` and `*.*`, so no behaviour is lost.
    std::string p(pattern);
    const size_t cut = p.find_last_of("/\\");
    const std::string dirPath = (cut == std::string::npos) ? "." : p.substr(0, cut);

    DIR* d = ::opendir(dirPath.c_str());
    if (!d) return INVALID_HANDLE_VALUE;

    FindState* st = new FindState{d, dirPath};
    if (!FindNextFileA(reinterpret_cast<HANDLE>(st), data)) {
        ::closedir(d);
        delete st;
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(st);
}

BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATA* data)
{
    if (h == INVALID_HANDLE_VALUE || !data) return FALSE;
    FindState* st = reinterpret_cast<FindState*>(h);

    struct dirent* e = ::readdir(st->dir);
    if (!e) return FALSE;

    std::snprintf(data->cFileName, sizeof(data->cFileName), "%s", e->d_name);

    struct stat sb;
    const std::string full = st->prefix + "/" + e->d_name;
    data->dwFileAttributes = 0;
    if (::stat(full.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
        data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    return TRUE;
}

BOOL FindClose(HANDLE h)
{
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    FindState* st = reinterpret_cast<FindState*>(h);
    ::closedir(st->dir);
    delete st;
    return TRUE;
}

BOOL DeleteFileA(LPCSTR name)      { return ::unlink(name) == 0 ? TRUE : FALSE; }
BOOL CreateDirectoryA(LPCSTR n, void*) { return ::mkdir(n, 0755) == 0 ? TRUE : FALSE; }
BOOL RemoveDirectoryA(LPCSTR name) { return ::rmdir(name) == 0 ? TRUE : FALSE; }

DWORD GetTempPathA(DWORD size, LPSTR buf)
{
    const char* kTmp = "/tmp/";
    const size_t n = std::strlen(kTmp);
    if (!buf || size < n + 1) return static_cast<DWORD>(n + 1);
    std::memcpy(buf, kTmp, n + 1);
    return static_cast<DWORD>(n);
}

UINT GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR buf)
{
    if (!buf) return 0;
    static UINT counter = 0;
    const UINT id = unique ? unique : ++counter;
    std::snprintf(buf, MAX_PATH, "%s%s%04x.tmp",
                  path ? path : "/tmp/", prefix ? prefix : "tmp", id & 0xFFFF);
    return id;
}

// --- clipboard: fails SILENTLY, and that is documented ----------------------

BOOL   OpenClipboard(HWND)              { return FALSE; }
BOOL   CloseClipboard(void)             { return FALSE; }
BOOL   EmptyClipboard(void)             { return FALSE; }
HANDLE SetClipboardData(UINT, HANDLE)   { return nullptr; }
HANDLE GetClipboardData(UINT)           { return nullptr; }
UINT   RegisterClipboardFormatA(LPCSTR) { return 0; }

void GetSystemInfo(SYSTEM_INFO* info)
{
    if (!info) return;
    const long page = ::sysconf(_SC_PAGESIZE);
    info->dwPageSize = static_cast<DWORD>(page > 0 ? page : 4096);
    // In wasm there is one thread running the game's code; no more is guessed.
    info->dwNumberOfProcessors = 1;
    info->dwAllocationGranularity = 65536;   // the size of a wasm memory page
}

BOOL AllocConsole(void) { return FALSE; }
BOOL FreeConsole(void)  { return FALSE; }

HANDLE GetCurrentProcess(void)   { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)); }
HANDLE GetCurrentThread(void)    { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2)); }
DWORD  GetCurrentProcessId(void) { return 1; }
DWORD  GetCurrentThreadId(void)  { return 1; }

// --- global memory ----------------------------------------------------------

HGLOBAL GlobalAlloc(UINT flags, size_t bytes)
{
    void* p = (flags & GMEM_ZEROINIT) ? std::calloc(1, bytes) : std::malloc(bytes);
    return p;
}

HGLOBAL GlobalFree(HGLOBAL mem)
{
    std::free(mem);
    return nullptr;
}

/// Since Win32 the handle IS the pointer - locking is the identity.
LPVOID GlobalLock(HGLOBAL mem)   { return mem; }
BOOL   GlobalUnlock(HGLOBAL)     { return TRUE; }

/// Win32 remembers the block size; `malloc` does not expose it portably.
/// Zero is returned and **described as such** - code relying on it has to
/// be changed, not given an invented number.
size_t GlobalSize(HGLOBAL)       { return 0; }

HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }

// --- critical sections: empty, because wasm is single-threaded here ---------
void InitializeCriticalSection(LPCRITICAL_SECTION cs) { if (cs) cs->unused = 0; }
void DeleteCriticalSection(LPCRITICAL_SECTION)        {}
void EnterCriticalSection(LPCRITICAL_SECTION)         {}
void LeaveCriticalSection(LPCRITICAL_SECTION)         {}

BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL bFailIfExists)
{
    if (bFailIfExists) {
        FILE* probe = std::fopen(to, "rb");
        if (probe) { std::fclose(probe); return FALSE; }
    }

    FILE* in = std::fopen(from, "rb");
    if (!in) return FALSE;
    FILE* out = std::fopen(to, "wb");
    if (!out) { std::fclose(in); return FALSE; }

    char buf[16 * 1024];
    size_t n;
    BOOL ok = TRUE;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = FALSE; break; }
    }
    if (std::ferror(in)) ok = FALSE;

    std::fclose(in);
    std::fclose(out);
    return ok;
}

int _chdir(const char* path)
{
    return chdir(path);
}

// Moves ONE character forward, not one byte. In double-byte encodings
// (Shift-JIS, Big5, GBK, EUC-KR) the first byte of a pair has the top bit
// set; then two bytes are skipped. At the end of the string it stays put -
// as Win32 does, so that the loop `while (*p) p = CharNextExA(...)` does
// not run past the buffer.
LPSTR CharNextExA(WORD /*codePage*/, LPCSTR current, DWORD /*flags*/)
{
    if (!current || *current == '\0') return const_cast<LPSTR>(current);
    if (static_cast<unsigned char>(*current) & 0x80) {
        return const_cast<LPSTR>(current + (current[1] ? 2 : 1));
    }
    return const_cast<LPSTR>(current + 1);
}

// GDI objects. In the browser there is no device context and no brushes -
// the port's graphics layer draws. These functions stay **empty**, because
// `EterLib` calls them when cleaning up and when choosing the system font,
// and neither activity has a wasm counterpart that could be done in place.
// `SelectObject` and `DeleteObject` moved to `platform_text.cpp`:
// there they have a REAL body, because they bind the font and the bitmap
// to the drawing context. Here they were empty until there was something
// to draw with.
HGDIOBJ GetStockObject(int)             { return NULL; }

// Moves ONE character back, not one byte. To tell whether the byte before
// us is the second byte of a pair, the string has to be walked from
// `start` - otherwise the second byte of a pair cannot be told from a
// standalone character.
LPSTR CharPrevExA(WORD codePage, LPCSTR start, LPCSTR current, DWORD flags)
{
    if (!start || !current || current <= start) return const_cast<LPSTR>(start);
    LPCSTR p = start;
    LPCSTR prev = start;
    while (p < current) {
        prev = p;
        p = CharNextExA(codePage, p, flags);
        if (p == prev) break;  // guard against looping
    }
    return const_cast<LPSTR>(prev);
}

// `DeleteDC` - see the note above, the body is in `platform_text.cpp`.

// `ioctlsocket(FIONBIO)` is the only use of this function in TMP4 code, and
// on POSIX it corresponds to `fcntl` with the `O_NONBLOCK` flag. So the
// translation is exact, not approximate - and that is why it has a body,
// unlike the things that need a decision.
int ioctlsocket(SOCKET s, long cmd, unsigned long* argp)
{
    if (cmd != (long)FIONBIO || !argp) return -1;

    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;

    const int newFlags = *argp ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, newFlags) < 0 ? -1 : 0;
}

// Letter case change IN PLACE - the MSVC names. POSIX has only `toupper`
// on a single character, so the loop is the right translation here, not a
// workaround.
char* _strupr(char* s)
{
    if (!s) return s;
    for (char* p = s; *p; ++p) {
        *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    }
    return s;
}

char* _strlwr(char* s)
{
    if (!s) return s;
    for (char* p = s; *p; ++p) {
        *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Things with an EXACT POSIX counterpart - so they have a body
// ---------------------------------------------------------------------------

DWORD GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    // Win32 returns the length WITHOUT the terminator when it fit, and when
    // it did not - the size needed INCLUDING it. This difference is part of
    // the contract and the calling code relies on it.
    char tmp[4096];
    if (!getcwd(tmp, sizeof(tmp))) return 0;

    const size_t length = std::strlen(tmp);
    if (!buf || size < length + 1) {
        return static_cast<DWORD>(length + 1);
    }
    std::memcpy(buf, tmp, length + 1);
    return static_cast<DWORD>(length);
}

BOOL MoveFileA(LPCSTR from, LPCSTR to)
{
    // `rename` on POSIX does exactly the same. There is one difference
    // worth knowing: Win32 REFUSES when the target exists, `rename`
    // overwrites. The Win32 behaviour is reproduced, because TMP4 code
    // checks the result and on success assumes it lost nothing.
    if (!from || !to) return FALSE;
    FILE* probe = std::fopen(to, "rb");
    if (probe) { std::fclose(probe); return FALSE; }
    return std::rename(from, to) == 0 ? TRUE : FALSE;
}
