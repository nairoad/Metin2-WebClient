// SPDX-License-Identifier: GPL-2.0-or-later
// win32_compat.h - the Win32 -> POSIX/emscripten compatibility layer for the
// TMP4 port.
//
// WHAT FOR: `EterBase` (and the other TMP4 layers) is VS6-era code that
// reaches for `windows.h`. Instead of rewriting calls in hundreds of places,
// I provide the **twenty** functions the code really uses.
//
// The number is measured, not guessed - a `grep` for Win32 symbols in the
// whole of `EterBase` gives 20 different names, mostly file and time ones.
//
// WHAT IS NOT HERE AND WILL NOT BE:
//   * `StackWalk` / `SymInitialize` - stack dumps from `Debug.cpp`. In the
//     browser there is no stack to walk; that file gets a separate, explicit
//     stub.
//   * `MessageBox` - the browser has no system modal windows. It returns a
//     fixed result and logs.
//
// RULE: every function has to reproduce the **Win32 semantics**, not just the
// signature. Different error returns (`INVALID_HANDLE_VALUE` versus `NULL`)
// matter here, because the calling code checks them.
//
// This header imitates the Windows SDK, so it is exempt from the "file over
// 1500 lines" gate (its length is the length of the emulated API, not our
// choice - the renaming plan, group 8). SDK names (types, functions,
// macros) are the contract with TMP4 and are not renamed.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// --- types -------------------------------------------------------------------

typedef unsigned char      BYTE;
typedef unsigned short     WORD;

/// **`unsigned long`, NOT `unsigned int`** - that is how Win32 defines it.
///
/// On wasm32 both are 32 bits, so the ABI is identical and for a long time
/// nothing happens. But they are **different types** for overloading:
/// `lzo.cpp` calls `tea_encrypt(DWORD*, ...)`, and `tea.h` declares
/// `unsigned long*`. With `unsigned int` the match fails and the compiler
/// reports "no matching function".
///
/// Caught by a compile attempt, not by reading the header - and that is an
/// argument for the compatibility layer reproducing **the exact types**, not
/// ones that "have the same width".
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               LONG;
typedef unsigned int       UINT;
typedef unsigned long      ULONG;
typedef char*              LPSTR;
typedef const char*        LPCSTR;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              HANDLE;
typedef void*              HWND;
typedef void*              HMODULE;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define WINAPI
#define APIENTRY
#define CALLBACK
/// MSVC attributes - dropped.
#define __declspec(x)

#define MAX_PATH 260

/// **Watch the trap:** in Win32 `CreateFile` returns `INVALID_HANDLE_VALUE`
/// (i.e. `-1`) on error, while `CreateFileMapping` returns `NULL`. TMP4 code
/// checks both, so the two values must differ.
#define INVALID_HANDLE_VALUE  ((HANDLE)(intptr_t)-1)

#define GENERIC_READ        0x80000000u
#define GENERIC_WRITE       0x40000000u
#define FILE_SHARE_READ     0x00000001u
#define FILE_SHARE_WRITE    0x00000002u
#define CREATE_ALWAYS       2
#define OPEN_EXISTING       3
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_BEGIN          0
#define FILE_CURRENT        1
#define FILE_END            2
#define PAGE_READONLY       0x02u
#define FILE_MAP_READ       0x0004u
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_SIZE        ((DWORD)-1)

#define MB_OK               0x0u
#define MB_ICONERROR        0x10u

namespace win32compat {

/// A file handle. Win32 passes a `HANDLE`, POSIX a descriptor - I keep the
/// descriptor **shifted by one**, so that `fd == 0` does not collide with
/// `NULL`.
inline int HandleToFd(HANDLE h)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(h)) - 1;
}

/// The inverse of `HandleToFd`.
inline HANDLE FdToHandle(int fd)
{
    return reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd) + 1);
}

/// Remembered mappings, so that `UnmapViewOfFile` knows the length. Win32
/// does not require it, POSIX `munmap` does - a real difference between the
/// interfaces, not decoration.
struct Mapping {
    void*  addr;
    size_t length;
};

/// The remembered mapping starting at `addr`, or NULL.
Mapping* FindMapping(const void* addr);
/// Remembers a mapping made by `MapViewOfFile`.
void     RememberMapping(void* addr, size_t length);
/// Forgets the mapping at `addr` (after `munmap`).
void     ForgetMapping(const void* addr);

}  // namespace win32compat

// --- files -------------------------------------------------------------------

/// POSIX `open`: GENERIC_READ/WRITE -> O_RDONLY/O_WRONLY/O_RDWR,
/// CREATE_ALWAYS -> O_CREAT|O_TRUNC; the path goes through
/// `M2W_ResolvePathCase` (read) or `M2W_PosixPath` (write).
/// INVALID_HANDLE_VALUE on error.
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, void* sec,
                   DWORD creation, DWORD flags, HANDLE tmpl);
#define CreateFile CreateFileA

/// `close` of the file descriptor; FALSE for INVALID_HANDLE_VALUE or NULL.
BOOL  CloseHandle(HANDLE h);
/// `read` up to `toRead` bytes; `*read` = bytes read (0 on error).
BOOL  ReadFile(HANDLE h, LPVOID buf, DWORD toRead, DWORD* read, void* ov);
/// `write` of `toWrite` bytes; `*written` = bytes written (0 on error).
BOOL  WriteFile(HANDLE h, LPCVOID buf, DWORD toWrite, DWORD* written, void* ov);
/// `fstat` size - low 32 bits returned, high 32 in `*hi`; INVALID_FILE_SIZE
/// on error.
DWORD GetFileSize(HANDLE h, DWORD* hi);
/// `lseek` (FILE_BEGIN/CURRENT/END); a 64-bit offset when `distHi` is given;
/// INVALID_SET_FILE_POINTER on error.
DWORD SetFilePointer(HANDLE h, LONG dist, LONG* distHi, DWORD method);

HANDLE CreateFileMappingA(HANDLE file, void* sec, DWORD protect,
                          DWORD sizeHi, DWORD sizeLo, LPCSTR name);
#define CreateFileMapping CreateFileMappingA

/// Read-only private `mmap` of the file (length = `bytes`, or the whole file
/// when 0); remembered for `UnmapViewOfFile`. NULL on error.
LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHi, DWORD offLo, size_t bytes);
/// `munmap` of a mapping made by `MapViewOfFile` (its length looked up);
/// FALSE for an unknown address.
BOOL   UnmapViewOfFile(LPCVOID addr);

// --- time --------------------------------------------------------------------

/// Milliseconds of a monotonic clock.
DWORD GetTickCount(void);
/// The same as `GetTickCount`.
DWORD timeGetTime(void);
BOOL  QueryPerformanceCounter(int64_t* out);
/// Always 1e9 - the counter counts nanoseconds.
BOOL  QueryPerformanceFrequency(int64_t* out);
/// Does not block (the browser thread must not): stores a deadline that the
/// main loop honours (`M2W_LoopStillSleeping`); logs sleeps
/// of 20 ms or more (first 20).
void  Sleep(DWORD ms);
/// Whether the main loop should skip this turn: a wait requested by `Sleep`
/// is still running, or the artificial frame limit `?fps=N` says
/// less than 1000/N ms (minus 1 ms of slack) passed since the last turn
bool  M2W_LoopStillSleeping();

// --- diagnostics -------------------------------------------------------------

/// `errno` of the last failed POSIX call.
DWORD GetLastError(void);
/// Writes the text to stderr (the browser console).
void  OutputDebugStringA(LPCSTR text);
#define OutputDebugString OutputDebugStringA

/// No modal window: logs "[MessageBox] caption: text" to stderr and returns
/// IDOK (1).
int   MessageBoxA(HWND wnd, LPCSTR text, LPCSTR caption, UINT type);
#define MessageBox MessageBoxA

/// The conventional name "/metin2.wasm" (there is no executable path); 0 if
/// it does not fit.
DWORD GetModuleFileNameA(HMODULE mod, LPSTR buf, DWORD size);
#define GetModuleFileName GetModuleFileNameA

// --- names from Microsoft's CRT ----------------------------------------------

#ifndef _stricmp
#define _stricmp  strcasecmp
#endif
#ifndef stricmp
#define stricmp   strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif
#ifndef strnicmp
#define strnicmp  strncasecmp
#endif

// ---------------------------------------------------------------------------
// ADDITIONS - names found by a COMPILE ATTEMPT, not guessed.
//
// The list came about like this: compiling the whole of `EterBase`, collecting
// every "unknown type name" and "undeclared identifier" error, grouping them.
// So the layer has exactly what the code uses - no less, no more.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <dirent.h>

// --- type aliases ------------------------------------------------------------

typedef void               VOID;
typedef char               CHAR;
typedef char*              PCHAR;
typedef void*              PVOID;
typedef unsigned char*     PBYTE;
typedef int                INT;
typedef unsigned short     USHORT;
typedef long long          LONGLONG;

/// Win32 `LARGE_INTEGER` is a union; TMP4 code reaches for `.QuadPart`.
union LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
};

// --- Microsoft compiler keywords and macros ----------------------------------

#ifndef __forceinline
#define __forceinline inline
#endif

#ifndef _vsnprintf
#define _vsnprintf vsnprintf
#endif
#ifndef _snprintf
#define _snprintf snprintf
#endif

/// Win32 defines `min`/`max` as MACROS. TMP4 code relies on them. I define
/// them the same way, because switching to `std::min` would mean touching the
/// sources.
#ifndef min
/// Win32 `min` MACRO (TMP4 relies on it being a macro).
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
/// Win32 `max` MACRO.
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef _S_IWRITE
#define _S_IWRITE 0200
#endif
#ifndef _S_IREAD
#define _S_IREAD  0400
#endif

#define OPEN_ALWAYS              4
#define FILE_ATTRIBUTE_DIRECTORY 0x10u

// --- directories and files ---------------------------------------------------

/// Only what `FindFirstFileA`/`FindNextFileA` fill: attributes and the name.
struct WIN32_FIND_DATA {
    DWORD dwFileAttributes;
    char  cFileName[MAX_PATH];
};

/// `opendir` of the pattern's directory (the mask is ignored - TMP4 uses only
/// `*` and `*.*`) and the first entry; INVALID_HANDLE_VALUE on error or an
/// empty directory.
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATA* data);
/// The next `readdir` entry: name and FILE_ATTRIBUTE_DIRECTORY; FALSE at the
/// end.
BOOL   FindNextFileA(HANDLE h, WIN32_FIND_DATA* data);
/// `closedir` and frees the search state.
BOOL   FindClose(HANDLE h);
#define FindFirstFile FindFirstFileA
#define FindNextFile  FindNextFileA

/// `unlink`.
BOOL  DeleteFileA(LPCSTR name);
/// `mkdir` with mode 0755.
BOOL  CreateDirectoryA(LPCSTR name, void* sec);
/// `rmdir`.
BOOL  RemoveDirectoryA(LPCSTR name);
/// "/tmp/"; returns the size needed when the buffer is too small.
DWORD GetTempPathA(DWORD size, LPSTR buf);
/// `path` + `prefix` + 4 hex digits + ".tmp" (a counter when `unique` is 0);
/// does not create the file.
UINT  GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR buf);
#define DeleteFile      DeleteFileA
#define CreateDirectory CreateDirectoryA
#define RemoveDirectory RemoveDirectoryA
#define GetTempPath     GetTempPathA
#define GetTempFileName GetTempFileNameA

#ifndef _getcwd
#define _getcwd getcwd
#endif

// --- clipboard ---------------------------------------------------------------
//
// In the browser the clipboard is ASYNCHRONOUS and needs a user gesture, so the
// synchronous Win32 API cannot be reproduced. These functions **fail
// quietly** and are documented as such - better than pretending they work.

/// Always FALSE - the browser clipboard is asynchronous (see the section
/// note).
BOOL  OpenClipboard(HWND wnd);
/// Always FALSE (no clipboard).
BOOL  CloseClipboard(void);
/// Always FALSE (no clipboard).
BOOL  EmptyClipboard(void);
/// Always NULL (no clipboard).
HANDLE SetClipboardData(UINT format, HANDLE mem);
/// Always NULL (no clipboard).
HANDLE GetClipboardData(UINT format);
/// Always 0 (no clipboard).
UINT  RegisterClipboardFormatA(LPCSTR name);
#define RegisterClipboardFormat RegisterClipboardFormatA

// --- Nanomite protection markers ---------------------------------------------
//
// `Timer.cpp` and other TMP4 files wrap bodies in `NANOBEGIN` / `NANOEND`.
// These are markers for **Nanomite** - an anti-piracy tool that replaces the
// marked code after compilation. The TMP4 author's comment next to one of
// them says plainly: *"if z powodu nanomite"* ("if because of nanomite") -
// hence the odd `if (this)` in the code, which checks nothing.
//
// In the web port there is nothing to protect and nothing to protect it
// with - I define them as **empty**. This is not a stub for something
// missing; it is the removal of a layer that makes no sense here.
#ifndef NANOBEGIN
#define NANOBEGIN
#define NANOEND
#endif

// --- system information ------------------------------------------------------

/// The three fields `GetSystemInfo` fills.
struct SYSTEM_INFO {
    DWORD dwPageSize;
    DWORD dwNumberOfProcessors;
    DWORD dwAllocationGranularity;
};
/// Page size from `sysconf`, ONE processor, allocation granularity 65536 (a
/// wasm memory page).
void GetSystemInfo(SYSTEM_INFO* info);

// --- console and SEH exception handling --------------------------------------
//
// `Debug.cpp` and `error.cpp` are Windows crash-dump infrastructure: a
// diagnostic console and an SEH filter with a stack walk. The browser has
// neither - these functions **do nothing** and are documented as such.

typedef char* PSTR;
struct _EXCEPTION_POINTERS { void* ExceptionRecord; void* ContextRecord; };
typedef struct _EXCEPTION_POINTERS EXCEPTION_POINTERS;

/// Always FALSE - no console window in the browser.
BOOL AllocConsole(void);
/// Always FALSE.
BOOL FreeConsole(void);

// --- the rest of Microsoft's CRT ---------------------------------------------

#ifndef _chmod
#define _chmod chmod
#endif
#ifndef _ecvt
#define _ecvt ecvt
#endif

/// From `mmsystem.h`. Packs a four-character type code into a `DWORD` - TMP4
/// code uses it for signatures in pack file headers.
#ifndef MAKEFOURCC
/// Packs four characters into a DWORD, first char in the low byte.
#define MAKEFOURCC(a, b, c, d)                     \
    ((DWORD)(BYTE)(a)        | ((DWORD)(BYTE)(b) <<  8) | \
    ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#endif

#ifndef _MAX_PATH
#define _MAX_PATH MAX_PATH
#endif
#ifndef _MAX_FNAME
#define _MAX_FNAME 256
#endif
#ifndef _MAX_EXT
#define _MAX_EXT 256
#endif
#ifndef _MAX_DIR
#define _MAX_DIR 256
#endif
#ifndef _MAX_DRIVE
#define _MAX_DRIVE 3
#endif

typedef BYTE               BOOLEAN;
typedef unsigned char*     LPBYTE;
typedef DWORD*             LPDWORD;
typedef void*              HGLOBAL;
typedef void*              HLOCAL;

/// Process and thread handles. In the browser there is nothing to identify -
/// I return fixed, invalid values, so that diagnostic code has something to
/// pass on, not so that anything works.
HANDLE GetCurrentProcess(void);
/// The pseudo-handle -2, as in Win32.
HANDLE GetCurrentThread(void);
/// Always 1.
DWORD  GetCurrentProcessId(void);
/// Always 1 (one thread).
DWORD  GetCurrentThreadId(void);

// --- Win32 global memory -----------------------------------------------------
//
// `GlobalAlloc` / `GlobalLock` is a relic of 16-bit Windows: allocation
// returned a HANDLE that had to be locked to get a pointer. Since Win32 the
// handle and the pointer are the same thing, so `GlobalLock` is the identity -
// and that is how it is reproduced here.

#define GMEM_FIXED    0x0000u
#define GMEM_MOVEABLE 0x0002u
#define GMEM_ZEROINIT 0x0040u
#define GHND          (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR          (GMEM_FIXED | GMEM_ZEROINIT)

/// `calloc` with GMEM_ZEROINIT, else `malloc`; the handle IS the pointer.
HGLOBAL GlobalAlloc(UINT flags, size_t bytes);
/// `free`; returns NULL (success).
HGLOBAL GlobalFree(HGLOBAL mem);
/// The identity - the handle is the pointer.
LPVOID  GlobalLock(HGLOBAL mem);
/// Always TRUE.
BOOL    GlobalUnlock(HGLOBAL mem);
/// Always 0 - the size is not tracked.
size_t  GlobalSize(HGLOBAL mem);

/// Always NULL.
HMODULE GetModuleHandleA(LPCSTR name);
#define GetModuleHandle GetModuleHandleA

typedef WORD*              LPWORD;
typedef unsigned char*     PUCHAR;
typedef unsigned char      UCHAR;
typedef unsigned long*     PULONG;
typedef unsigned int*      PUINT;
typedef long*              PLONG;
typedef const char*        PCSTR;
typedef char*              LPTSTR;
typedef const char*        LPCTSTR;

// --- name clash with POSIX ---------------------------------------------------
//
// TMP4's `Random.cpp` defines its own `unsigned long random()` and
// `void srandom(unsigned long)`. POSIX `<stdlib.h>` declares
// `long random(void)` and `void srandom(unsigned int)` - the same names,
// different return types. The compiler rightly refuses: "functions that differ
// only in their return type".
//
// On Windows there was no problem, because POSIX `random` simply does not
// exist there.
//
// I solve it with a **rename on the layer's side**, not a patch in the TMP4
// source: the macro acts on the definition and on all calls at once, because
// every TMP4 file pulls this header in through `windows.h`. So the rule "we do
// not touch TMP4 sources" holds.
//
// **This is NOT a stub** - the TMP4 generator keeps working and gives the same
// numbers. Only the symbol's name changes.
#ifndef random
#define random   m2w_random
#define srandom  m2w_srandom
#endif

// --- SAL annotations and size types ------------------------------------------
//
// `IN` and `OUT` are **parameter direction annotations** from Microsoft's
// headers - they carry no meaning for the compiler, they serve only the reader
// and analysis tools. I define them as empty, because that is what they are.
#ifndef IN
#define IN
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif

typedef size_t             SIZE_T;
typedef ptrdiff_t          SSIZE_T;

// --- critical sections -------------------------------------------------------
//
// wasm in this client is **single-threaded** (see `GetSystemInfo` above, where
// I declare one processor). A critical section has nothing to protect, so
// these functions are empty - but I keep the TYPE, so that TMP4 code can go on
// declaring its fields.
//
// **Should the port ever turn threads on** (`-pthread`), these four functions
// will have to be implemented on `pthread_mutex_t`, not left like this. I write
// it down here, because an empty synchronisation implementation is silent and
// dangerous.
struct CRITICAL_SECTION { int unused; };
typedef CRITICAL_SECTION* LPCRITICAL_SECTION;

/// Zeroes the placeholder - single-threaded wasm (see the section note).
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
/// Does nothing (single-threaded).
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
/// Does nothing (single-threaded).
void EnterCriticalSection(LPCRITICAL_SECTION cs);
/// Does nothing (single-threaded).
void LeaveCriticalSection(LPCRITICAL_SECTION cs);

// --- a minimum of COM --------------------------------------------------------
//
// `GameLib` pulls in `objbase.h` for **GUIDs**, not for COM itself - it uses
// them as identifiers of resource types. So I provide the type and the
// comparison, not the whole object model.
/// A 16-byte GUID; compared byte-wise (`operator==`).
struct GUID {
    DWORD Data1;
    WORD  Data2;
    WORD  Data3;
    BYTE  Data4[8];
};
typedef GUID  IID;
typedef GUID  CLSID;
typedef GUID* REFGUID;
typedef const GUID& REFIID;
typedef long  HRESULT;

#define S_OK      ((HRESULT)0)
#define S_FALSE   ((HRESULT)1)
#define E_FAIL    ((HRESULT)0x80004005L)
/// HRESULT >= 0.
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
/// HRESULT < 0.
#define FAILED(hr)    ((HRESULT)(hr) <  0)

/// Byte-wise GUID equality (GUIDs have no padding).
inline bool operator==(const GUID& a, const GUID& b)
{
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}
/// Negation of `operator==`.
inline bool operator!=(const GUID& a, const GUID& b) { return !(a == b); }

// --- Win32 handles and geometry types ----------------------------------------
//
// `GameLib` needs them for **field declarations**, not to call Windows. I
// provide the types so that the headers parse; there is no implementation and
// there is not meant to be. (The graphics layer is our own GL layer, gl_*.cpp.)
/// Declares a handle type as `void*`.
#define DECLARE_HANDLE(name) typedef void* name

DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HCURSOR);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HGDIOBJ);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HPALETTE);

struct RECT  { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct SIZE  { LONG cx, cy; };
typedef RECT*  LPRECT;
typedef POINT* LPPOINT;
typedef RECT   RECTL;

typedef unsigned int   UINT_PTR;
typedef int            INT_PTR;
typedef long           LPARAM;
typedef unsigned int   WPARAM;
typedef long           LRESULT;
typedef DWORD          COLORREF;
typedef float          FLOAT;
typedef wchar_t        WCHAR;
typedef WCHAR*         LPWSTR;
typedef const WCHAR*   LPCWSTR;

// --- COM interface declaration macros ----------------------------------------
//
// `interface` in Microsoft's headers is **a plain `struct`**. The rest is the
// casing of virtual method declarations. None of this implements anything - the
// point is only that the DirectX headers can be parsed well enough to see what
// in `GameLib` really depends on them.
#ifndef interface
#define interface struct
#endif
#define STDMETHODCALLTYPE
/// A virtual method returning HRESULT.
#define STDMETHOD(m)            virtual HRESULT m
/// A virtual method returning `t`.
#define STDMETHOD_(t, m)        virtual t m
#define PURE                    = 0
#define THIS_
#define THIS
/// An interface is a `struct`.
#define DECLARE_INTERFACE(i)    interface i
/// An interface deriving from `b`.
#define DECLARE_INTERFACE_(i, b) interface i : public b

// ---------------------------------------------------------------------------
// GDI fonts - from measurement, not from a wish list
// ---------------------------------------------------------------------------
// `eterLib/Util.h` declares `EnumFontFamExProc`, i.e. the callback for
// enumerating system fonts. In the browser there is nothing to enumerate, but
// **the declaration has to parse**, because the header pulls in the whole of
// `GameLib`.
//
// The `LOGFONT` fields are real, because they cost nothing and remove the
// question whether this particular field mattered. The names `LF_FACESIZE`
// and `LF_FULLFACESIZE` come from Win32 too.
#define LF_FACESIZE     32
#define LF_FULLFACESIZE 64

typedef struct tagLOGFONTA {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    char lfFaceName[LF_FACESIZE];
} LOGFONTA, *PLOGFONTA, *LPLOGFONTA;

typedef LOGFONTA LOGFONT;
typedef PLOGFONTA PLOGFONT;
typedef LPLOGFONTA LPLOGFONT;

typedef struct tagTEXTMETRICA {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    BYTE tmFirstChar;
    BYTE tmLastChar;
    BYTE tmDefaultChar;
    BYTE tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} TEXTMETRICA, *PTEXTMETRICA, *LPTEXTMETRICA;

typedef TEXTMETRICA TEXTMETRIC;
typedef PTEXTMETRICA PTEXTMETRIC;
typedef LPTEXTMETRICA LPTEXTMETRIC;

// ---------------------------------------------------------------------------
// TCHAR and ZeroMemory
// ---------------------------------------------------------------------------
// In Win32 `TCHAR` is `char` or `wchar_t` depending on `UNICODE`. The TMP4
// client builds in the NARROW (single-byte) variant - visible from the fact
// that it calls the `...A` variants explicitly everywhere (`MessageBoxA`,
// `FindFirstFileA`). So `TCHAR` is `char`, not `wchar_t`: the other choice
// would silently double the size of every string in the structures.
typedef char      TCHAR;
typedef char*     LPTSTR;
typedef const char* LPCTSTR;

#ifndef _T
/// Narrow build: the literal unchanged.
#define _T(x)   x
#endif
#ifndef TEXT
/// Narrow build: the literal unchanged.
#define TEXT(x) x
#endif

#ifndef ZeroMemory
/// `memset(dst, 0, len)`.
#define ZeroMemory(dst, len) memset((dst), 0, (len))
#endif
#ifndef CopyMemory
/// `memcpy(dst, src, len)`.
#define CopyMemory(dst, src, len) memcpy((dst), (src), (len))
#endif
#ifndef FillMemory
/// `memset(dst, val, len)` - note Win32's argument order.
#define FillMemory(dst, len, val) memset((dst), (val), (len))
#endif

// Floating-point constants (`FLT_EPSILON` in `GameLib/MapUtil.cpp`) and string
// functions - Win32 pulls them in through `windows.h`, POSIX needs them
// explicitly.
#include <cfloat>
#include <climits>

/// Copying a file. `bFailIfExists` kept exactly as in Win32: when true, an
/// existing target file is **not** overwritten and the function returns false.
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL bFailIfExists);
#ifndef CopyFile
#define CopyFile CopyFileA
#endif

// Thread priorities. `AreaLoaderThread` itself gets a different solution in the
// port anyway (the main loop or a Web Worker), but the constants have to exist
// so that the file parses and what is still left can be measured.
#define THREAD_PRIORITY_IDLE          (-15)
#define THREAD_PRIORITY_LOWEST         (-2)
#define THREAD_PRIORITY_BELOW_NORMAL   (-1)
#define THREAD_PRIORITY_NORMAL           0
#define THREAD_PRIORITY_ABOVE_NORMAL     1
#define THREAD_PRIORITY_HIGHEST          2
#define THREAD_PRIORITY_TIME_CRITICAL   15

/// Declared only - no body in compat/ (see the section note above).
BOOL SetThreadPriority(HANDLE thread, int priority);

// ---------------------------------------------------------------------------
// Semaphores and waiting on a handle - DECLARED, NOT DEFINED
// ---------------------------------------------------------------------------
// The same rule as for `_beginthreadex` in `process.h`: the declaration lets
// the file parse and does not hide the rest of the measurement, and the missing
// definition will stop the linker loudly when someone really tries to use it.
//
// A stub would be WRONG here: the `AreaLoaderThread` code waits on a semaphore
// for data from the loading thread. A successful "ready" answer without a
// thread would mean the game reads a map that was not loaded.
#define INFINITE      0xFFFFFFFFul
#define WAIT_OBJECT_0 0x00000000ul
#define WAIT_TIMEOUT  0x00000102ul

/// Declared only - no body in compat/ (see the section note above).
HANDLE CreateSemaphoreA(void* sec, LONG initialCount, LONG maxCount, LPCSTR name);
#ifndef CreateSemaphore
#define CreateSemaphore CreateSemaphoreA
#endif
/// Declared only - the one unresolved symbol of the link
/// (tools/gates/link_baseline.txt); see the section note.
BOOL   ReleaseSemaphore(HANDLE sem, LONG releaseCount, LONG* previousCount);
/// Declared only - no body in compat/ (see the section note above).
DWORD  WaitForSingleObject(HANDLE h, DWORD milliseconds);

// `CONST` is the upper-case counterpart of `const` from the Win32/DirectX
// headers. It stands here, not in `d3dx8.h`, because both files of the
// graphics layer use it.
#ifndef CONST
#define CONST const
#endif
#ifndef FLOAT
typedef float FLOAT;
#endif

// ---------------------------------------------------------------------------
// GDI bitmaps (`BITMAPINFO` and family)
// ---------------------------------------------------------------------------
// They blocked SEVEN `EterLib` files at once. They are pure
// structures describing the pixel layout in a BMP file - no Windows code stands
// behind them, so reproducing them is reproducing a FORMAT (category A), not the
// system.
//
// `#pragma pack(2)` matters: `BITMAPFILEHEADER` is 14 bytes, not 16. Without
// two-byte alignment the compiler would add padding and reading a BMP file
// would shift by two bytes - a bug that gives an image, only a wrong one.
typedef struct tagRGBQUAD {
    BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved;
} RGBQUAD;

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;

#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
    WORD  bfType;       // 'BM'
    DWORD bfSize;
    WORD  bfReserved1;
    WORD  bfReserved2;
    DWORD bfOffBits;
} BITMAPFILEHEADER, *PBITMAPFILEHEADER, *LPBITMAPFILEHEADER;
#pragma pack(pop)

#define BI_RGB       0
#define BI_RLE8      1
#define BI_RLE4      2
#define BI_BITFIELDS 3

// GDI character sets - `EterLib` passes them to font selection.
#define ANSI_CHARSET        0
#define DEFAULT_CHARSET     1
#define SYMBOL_CHARSET      2
#define SHIFTJIS_CHARSET  128
#define HANGUL_CHARSET    129
#define GB2312_CHARSET    134
#define CHINESEBIG5_CHARSET 136
#define RUSSIAN_CHARSET   204
#define EASTEUROPE_CHARSET 238

/// The window procedure. In the browser there is no window - events go through
/// `CanvasInput`. The name has to exist, because `EterLib` declares it in a
/// header.
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

/// `GetCurrentTime` is an old Win32 alias of `GetTickCount`.
#define GetCurrentTime GetTickCount

/// `chdir`.
int _chdir(const char* path);

/// The next character in a multibyte string. `EterLib` uses it to walk text in
/// Asian encodings.
LPSTR CharNextExA(WORD codePage, LPCSTR current, DWORD flags);

// ---------------------------------------------------------------------------
// Sockets: Winsock names on top of POSIX sockets
// ---------------------------------------------------------------------------
// Emscripten provides POSIX sockets and **itself replaces** `connect()` with a
// WebSocket connection (the mechanism the bridge stands on). Winsock
// differs from POSIX mainly in NAMES, so the layer is aliases, not a new
// implementation.
//
// `WSAStartup` and `WSACleanup` are **really** nothing on POSIX - initialising
// the network stack is a Windows requirement, not a Unix one. Empty bodies are
// not a stub hiding something missing here, but the correct translation.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)

typedef struct sockaddr     SOCKADDR,    *PSOCKADDR,    *LPSOCKADDR;
typedef struct sockaddr_in  SOCKADDR_IN, *PSOCKADDR_IN, *LPSOCKADDR_IN;
typedef struct in_addr      IN_ADDR,     *PIN_ADDR,     *LPIN_ADDR;
typedef struct hostent      HOSTENT,     *PHOSTENT,     *LPHOSTENT;

typedef struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
} WSADATA, *LPWSADATA;

/// Two bytes into a WORD, `a` in the low byte.
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | (((WORD)((BYTE)(b))) << 8)))

/// No network stack to start on POSIX - always succeeds.
inline int WSAStartup(WORD, LPWSADATA) { return 0; }
/// Nothing to clean up - always succeeds.
inline int WSACleanup(void)            { return 0; }
/// Winsock's `closesocket` is POSIX `close`.
inline int closesocket(SOCKET s)       { return close(s); }

// More GDI and window constants and handles - from measuring the EterLib sources.
#define GREEK_CHARSET   161
#define TURKISH_CHARSET 162
#define ARABIC_CHARSET  178
#define BALTIC_CHARSET  186
#define THAI_CHARSET    222

#define FW_DONTCARE   0
#define FW_NORMAL   400
#define FW_BOLD     700

#define BLACK_BRUSH  4
#define WHITE_BRUSH  0
#define NULL_BRUSH   5

#define MB_TOPMOST     0x00040000

HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
BOOL    DeleteObject(HGDIOBJ obj);
/// Always NULL - no GDI objects.
HGDIOBJ GetStockObject(int object);

/// The previous character in a multibyte string - `CharNextExA` the other way.
LPSTR CharPrevExA(WORD codePage, LPCSTR start, LPCSTR current, DWORD flags);

// The last batch from measuring the EterLib sources.
#include <sys/select.h>

/// The socket error is `errno`.
#define WSAGetLastError() (errno)

/// Packing a GDI colour. NOTE: `COLORREF` is **BGR**, the reverse of
/// `D3DCOLOR`, which is ARGB. Swapping red and blue is a bug that gives an
/// image - only in the wrong colours.
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | (((WORD)(BYTE)(g)) << 8) | \
                                 (((DWORD)(BYTE)(b)) << 16)))
/// Red from a BGR COLORREF (low byte).
#define GetRValue(c) ((BYTE)(c))
/// Green from a COLORREF.
#define GetGValue(c) ((BYTE)(((WORD)(c)) >> 8))
/// Blue from a COLORREF (third byte).
#define GetBValue(c) ((BYTE)((c) >> 16))

#define HEBREW_CHARSET 177
#define MAC_CHARSET     77
#define OEM_CHARSET    255

#define OUT_DEFAULT_PRECIS   0
#define OUT_TT_PRECIS        4
#define OUT_TT_ONLY_PRECIS   7
#define CLIP_DEFAULT_PRECIS  0
#define DEFAULT_QUALITY      0
#define ANTIALIASED_QUALITY  4
#define DEFAULT_PITCH        0
#define FF_DONTCARE          0

// Window styles. In the browser there is no window, but `EterLib` passes these
// flags on and the declarations have to exist.
#define WS_OVERLAPPED       0x00000000ul
#define WS_CAPTION          0x00C00000ul
#define WS_SYSMENU          0x00080000ul
#define WS_THICKFRAME       0x00040000ul
#define WS_MINIMIZEBOX      0x00020000ul
#define WS_MAXIMIZEBOX      0x00010000ul
#define WS_POPUP            0x80000000ul
#define WS_VISIBLE          0x10000000ul
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

BOOL DeleteDC(HDC dc);

/// `__min` and `__max` are MSVC macros. The parentheses around the arguments
/// are necessary, otherwise `__min(a + 1, b)` would compute differently from
/// how it looks.
#ifndef __min
/// MSVC `__min`.
#define __min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef __max
/// MSVC `__max`.
#define __max(a, b) (((a) > (b)) ? (a) : (b))
#endif

// `VOID` - the upper-case `void` from the Win32 headers.
#ifndef VOID
#define VOID void
#endif

// ---------------------------------------------------------------------------
// GDI: the device context and text drawing - DECLARATIONS WITHOUT BODIES
// ---------------------------------------------------------------------------
// `GrpFontTexture` and `GrpDib` rasterise text through GDI: they create a
// memory context, select a font, ask for character widths and draw them into a
// bitmap, which then becomes a texture.
//
// In the browser this is done DIFFERENTLY - through `canvas` and `measureText`
// - and that is a **design decision**, not a missing system function. So the
// declarations are here, the bodies are not: the `EterLib` files parse and are
// visible in the contract, and the linker stops loudly at the place where the
// decision has to be made.
//
// The names are narrow (`...A`) and wide (`...W`), because TMP4 calls both
// variants: Latin text goes as bytes, Asian text as two bytes per character.
#define TRANSPARENT      1
#define OPAQUE           2
#define DIB_RGB_COLORS   0
#define DIB_PAL_COLORS   1

typedef struct _ABCFLOAT {
    float abcfA;
    float abcfB;
    float abcfC;
} ABCFLOAT, *LPABCFLOAT;

/// The dummy handle 1 (text is drawn by platform_text.cpp, not GDI).
HDC     GetDC(HWND wnd);
/// Always 1.
int     ReleaseDC(HWND wnd, HDC dc);
HDC     CreateCompatibleDC(HDC dc);
/// Declared only - no body in compat/ (see the section note above).
HBITMAP CreateCompatibleBitmap(HDC dc, int cx, int cy);
HBITMAP CreateDIBSection(HDC dc, CONST BITMAPINFO* bmi, UINT usage,
                         void** bits, HANDLE section, DWORD offset);

COLORREF SetBkColor(HDC dc, COLORREF color);
int      SetBkMode(HDC dc, int mode);
COLORREF SetTextColor(HDC dc, COLORREF color);

BOOL TextOutA(HDC dc, int x, int y, LPCSTR text, int count);
BOOL TextOutW(HDC dc, int x, int y, const WCHAR* text, int count);
#ifndef TextOut
#define TextOut TextOutA
#endif

BOOL GetTextExtentPoint32W(HDC dc, const WCHAR* text, int count, SIZE* size);
BOOL GetTextExtentPoint32A(HDC dc, LPCSTR text, int count, SIZE* size);
BOOL GetCharABCWidthsFloatW(HDC dc, UINT first, UINT last, LPABCFLOAT abcf);

// One real Win32 API for measuring characters IN THE CONTEXT of
// the whole word (not in isolation) - `lpDx[i]` is the width of the i-th
// character AS measured in the presence of its neighbours (real kerning).
// Implemented here is ONLY this one piece of the contract (`lpDx`) - the other
// `GCP_RESULTSW` fields are not needed by us (yet), so they are zeroed instead
// of guessed.
#define GCP_MAXEXTENT 0x00100000
typedef struct _GCP_RESULTSW {
    DWORD  lStructSize;
    LPWSTR lpOutString;
    UINT*  lpOrder;
    int*   lpDx;
    int*   lpCaretPos;
    LPSTR  lpClass;
    WCHAR* lpGlyphs;
    UINT   nGlyphs;
    int    nMaxFit;
} GCP_RESULTSW, *LPGCP_RESULTSW;

DWORD GetCharacterPlacementW(HDC dc, const WCHAR* text, int count,
                             int maxExtent, LPGCP_RESULTSW results,
                             DWORD flags);

HFONT CreateFontIndirectA(CONST LOGFONTA* lf);
#ifndef CreateFontIndirect
#define CreateFontIndirect CreateFontIndirectA
#endif

// The last batch from measuring `EterLib`.
#define VIETNAMESE_CHARSET 163
#define JOHAB_CHARSET      130

// Non-blocking sockets. On POSIX this is set through `fcntl`, but the Winsock
// names have to exist, because TMP4 code uses them directly.
#ifndef FIONBIO
#define FIONBIO 0x8004667Eul
#endif
#define WSAEWOULDBLOCK  EWOULDBLOCK
#define WSAEINPROGRESS  EINPROGRESS
#define WSAECONNRESET   ECONNRESET
#define WSAENOTCONN     ENOTCONN

/// Only FIONBIO: sets or clears O_NONBLOCK through `fcntl`; -1 for anything
/// else.
int ioctlsocket(SOCKET s, long cmd, unsigned long* argp);

/// Declared only - no body in compat/ (see the section note above).
BOOL GetTextExtentPoint32A_(HDC dc, LPCSTR text, int count, SIZE* size);
#ifndef GetTextExtentPoint32
#define GetTextExtentPoint32 GetTextExtentPoint32A
#endif

int SetDIBitsToDevice(HDC dc, int xDest, int yDest, DWORD w, DWORD h,
                      int xSrc, int ySrc, UINT startScan, UINT lines,
                      CONST void* bits, CONST BITMAPINFO* bmi, UINT colorUse);

// Calling-convention macros from RPC/COM. In Win32 they expand to `__stdcall`
// or to nothing; under wasm there is one convention, so they are **empty**.
// `eterLib/DIMM.h` (the IME interface) declares its memory allocation functions
// with them.
#ifndef __RPC_USER
#define __RPC_USER
#endif
#ifndef __RPC_STUB
#define __RPC_STUB
#endif
#ifndef __RPC_FAR
#define __RPC_FAR
#endif
#ifndef RPC_ENTRY
#define RPC_ENTRY
#endif
typedef long RPC_STATUS;

// RPC interface handles from `DIMM.h` (IME). In the browser the browser itself
// handles IME - these names only have to exist.
typedef void* RPC_IF_HANDLE;
typedef void* handle_t;
typedef struct _MIDL_STUB_MESSAGE MIDL_STUB_MESSAGE;

// ---------------------------------------------------------------------------
// Wide-character variants and small things from measuring `UserInterface`
//
// ---------------------------------------------------------------------------
typedef struct tagLOGFONTW {
    LONG  lfHeight;
    LONG  lfWidth;
    LONG  lfEscapement;
    LONG  lfOrientation;
    LONG  lfWeight;
    BYTE  lfItalic;
    BYTE  lfUnderline;
    BYTE  lfStrikeOut;
    BYTE  lfCharSet;
    BYTE  lfOutPrecision;
    BYTE  lfClipPrecision;
    BYTE  lfQuality;
    BYTE  lfPitchAndFamily;
    WCHAR lfFaceName[LF_FACESIZE];
} LOGFONTW, *PLOGFONTW, *LPLOGFONTW;

typedef struct tagTEXTMETRICW {
    LONG  tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    LONG  tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang;
    LONG  tmDigitizedAspectX, tmDigitizedAspectY;
    WCHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE  tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} TEXTMETRICW, *PTEXTMETRICW, *LPTEXTMETRICW;

// Splitting and packing words. The parentheses are necessary - without them
// `HIWORD(a + b)` would compute differently from how it looks.
#ifndef MAKEWORD
/// Two bytes into a WORD, `a` in the low byte.
#define MAKEWORD(a, b)  ((WORD)(((BYTE)(a)) | (((WORD)((BYTE)(b))) << 8)))
#endif
/// Two WORDs into a LONG, `a` in the low half.
#define MAKELONG(a, b)  ((LONG)(((WORD)(a)) | (((DWORD)((WORD)(b))) << 16)))
/// The low 16 bits.
#define LOWORD(l)       ((WORD)((DWORD)(l) & 0xFFFF))
/// The high 16 bits.
#define HIWORD(l)       ((WORD)(((DWORD)(l) >> 16) & 0xFFFF))
/// The low 8 bits.
#define LOBYTE(w)       ((BYTE)((DWORD)(w) & 0xFF))
/// Bits 8-15.
#define HIBYTE(w)       ((BYTE)(((DWORD)(w) >> 8) & 0xFF))

/// `strcmpi` is an MSVC name; POSIX has `strcasecmp`.
#ifndef strcmpi
#define strcmpi  strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif
#ifndef stricmp
#define stricmp  strcasecmp
#endif

/// Ending the message loop. In the browser the loop is in
/// `requestAnimationFrame` and ends differently - a declaration without a body.
void PostQuitMessage(int exitCode);

typedef unsigned long ULONG_PTR;
typedef long LONG_PTR;

/// `EXTERN_C` from the Win32 headers - in C++ it means C linkage.
#ifndef EXTERN_C
#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif
#endif

// Reading another process's memory - the anti-cheat layer. In the browser there
// are no other processes. A declaration without a body, so it is visible that
// this protection does not work in the port.
#define PROCESS_VM_READ           0x0010
#define PROCESS_QUERY_INFORMATION 0x0400
BOOL ReadProcessMemory(HANDLE process, const void* address, void* buffer,
                       size_t size, size_t* bytesRead);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD processId);

/// `MIDL_INTERFACE("...")` in Win32 is a `struct` with the `uuid` and
/// `novtable` attributes. The attributes are hints for Microsoft's compiler
/// and do not change the object layout, so a plain `struct` remains.
#ifndef MIDL_INTERFACE
/// A plain `struct` (the uuid/novtable attributes dropped).
#define MIDL_INTERFACE(x) struct
#endif

// ---------------------------------------------------------------------------
// `IUnknown` - the root of the COM model
// ---------------------------------------------------------------------------
// The IME headers (`eterLib/DIMM.h`) declare interfaces deriving from
// `IUnknown` - 22 `UserInterface` files were blocked by the missing class
// The same wall the measurement hit, only from the
// other side.
//
// Three methods, as in the original. Nothing is faked here: it is simply the
// declaration of the root of the hierarchy TMP4 uses for IME and the
// clipboard.
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif

/// The COM root: QueryInterface/AddRef/Release, all pure virtual.
struct IUnknown
{
    /// COM: asks the object for interface `riid`. No class of the port derives
    /// from `IUnknown` - the type is here for TMP4's declarations.
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const IID& riid, void** ppvObject) = 0;
    /// COM: adds a reference; returns the new count.
    virtual ULONG   STDMETHODCALLTYPE AddRef() = 0;
    /// COM: drops a reference, deleting the object at zero; returns the new count.
    virtual ULONG   STDMETHODCALLTYPE Release() = 0;
protected:
    /// Protected and non-virtual: a COM object goes away through `Release`.
    ~IUnknown() {}
};

typedef IUnknown* LPUNKNOWN;

// Language and keyboard layout identifiers - `eterLib/IME.h` asks the system
// for them to know which language the user types in. In the browser this
// corresponds to `navigator.language` and the composition events.
typedef WORD  LANGID;
typedef DWORD LCID;
DECLARE_HANDLE(HKL);

/// A language id from primary and sub-language.
#define MAKELANGID(p, s)       ((((WORD)(s)) << 10) | (WORD)(p))
/// The primary language (low 10 bits).
#define PRIMARYLANGID(lgid)    ((WORD)((lgid) & 0x3FF))
/// The sub-language (bits 10-15).
#define SUBLANGID(lgid)        ((WORD)((lgid) >> 10))

#define LANG_NEUTRAL   0x00
#define LANG_ENGLISH   0x09
#define LANG_KOREAN    0x12
#define LANG_JAPANESE  0x11
#define LANG_CHINESE   0x04
#define LANG_POLISH    0x15

/// Declared only - no body in compat/ (see the section note above).
LANGID GetSystemDefaultLangID(void);
/// Declared only - no body in compat/ (see the section note above).
LANGID GetUserDefaultLangID(void);
/// Declared only - no body in compat/ (see the section note above).
HKL    GetKeyboardLayout(DWORD idThread);

// Pointer aliases from `windef.h` that `eterLib/IME.h` uses.
typedef int*           PINT;
typedef unsigned int*  PUINT;
typedef DWORD*         PDWORD;
typedef BYTE*          PBYTE;
typedef WORD*          PWORD;
typedef BOOL*          PBOOL;
typedef char*          PCHAR;
typedef WCHAR*         PWCHAR;

/// The system version. The client asks for it to tell Windows 9x from NT
/// (different IME handling). In the browser the answer is single and fixed, so
/// the function gets a body only in the port's platform layer.
typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    char  szCSDVersion[128];
} OSVERSIONINFOA, *POSVERSIONINFOA, *LPOSVERSIONINFOA;

typedef OSVERSIONINFOA OSVERSIONINFO;
typedef LPOSVERSIONINFOA LPOSVERSIONINFO;

#define VER_PLATFORM_WIN32s        0
#define VER_PLATFORM_WIN32_WINDOWS 1
#define VER_PLATFORM_WIN32_NT      2

/// Declared only - no body in compat/ (see the section note above).
BOOL GetVersionExA(LPOSVERSIONINFOA info);
#ifndef GetVersionEx
#define GetVersionEx GetVersionExA
#endif

// ---------------------------------------------------------------------------
// Virtual key codes (`VK_*`)
// ---------------------------------------------------------------------------
// This is SOMETHING ELSE than `DIK_*` from `dinput.h`, and it is worth keeping
// the two families apart in one's head:
//
//   `DIK_*` - SCAN codes, i.e. the physical position of the key. The client
//             stores its control bindings in them.
//   `VK_*`  - VIRTUAL codes, i.e. the key after translation through the
//             keyboard layout. They arrive in window messages.
//
// The same letter has a DIFFERENT number in the two families (`VK_Z` is 0x5A,
// `DIK_Z` is 0x2C) - mixing them up gives controls that work, only not where
// they should.
//
// In the browser `VK_*` corresponds to `KeyboardEvent.key`/`keyCode`, and
// `DIK_*` to `KeyboardEvent.code`. Two translation tables, not one.
#define VK_LBUTTON  0x01
#define VK_RBUTTON  0x02
#define VK_CANCEL   0x03
#define VK_MBUTTON  0x04
#define VK_BACK     0x08
#define VK_TAB      0x09
#define VK_CLEAR    0x0C
#define VK_RETURN   0x0D
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_PAUSE    0x13
#define VK_CAPITAL  0x14
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_PRIOR    0x21
#define VK_NEXT     0x22
#define VK_END      0x23
#define VK_HOME     0x24
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_INSERT   0x2D
#define VK_DELETE   0x2E
#define VK_Z        0x5A
#define VK_F1       0x70
#define VK_F2       0x71
#define VK_F3       0x72
#define VK_F4       0x73
#define VK_F5       0x74
#define VK_F6       0x75
#define VK_F7       0x76
#define VK_F8       0x77
#define VK_F9       0x78
#define VK_F10      0x79
#define VK_F11      0x7A
#define VK_F12      0x7B
#define VK_NUMLOCK  0x90
#define VK_SCROLL   0x91

// Win32 error codes used by the anti-cheat layer and the events.
#define ERROR_PARTIAL_COPY   299
#define ERROR_ACCESS_DENIED    5
#define ERROR_FILE_NOT_FOUND   2

// Synchronisation events - like semaphores, DECLARED WITHOUT BODIES.
/// Declared only - no body in compat/ (see the section note above).
HANDLE CreateEventA(void* sec, BOOL manualReset, BOOL initialState, LPCSTR name);
#ifndef CreateEvent
#define CreateEvent CreateEventA
#endif
/// Declared only - no body in compat/ (see the section note above).
BOOL SetEvent(HANDLE event);
/// Declared only - no body in compat/ (see the section note above).
BOOL ResetEvent(HANDLE event);

// Win16 leftovers: `FAR` and `NEAR` described the kind of pointer in the
// segmented model. In Win32 they are **empty** and so they are here -
// `MovieMan.h` writes `const GUID FAR clsidSplitter`, which does not parse at
// all without this macro.
#ifndef FAR
#define FAR
#endif
#ifndef NEAR
#define NEAR
#endif
#ifndef PASCAL
#define PASCAL
#endif

// ---------------------------------------------------------------------------
// Starting COM - DECLARATIONS WITHOUT BODIES
// ---------------------------------------------------------------------------
// `MovieMan` (film playback through DirectShow) and the built-in browser of
// the **ItemShop** start COM. In the browser there is nothing to start: films
// are played by `<video>`, and the shop will be an ordinary HTML window.
//
// The declarations are here, the bodies are not - the linker is to stop where
// the decision to replace these two mechanisms has to be made.
#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED     0x0

#define CLSCTX_INPROC_SERVER  0x1
#define CLSCTX_LOCAL_SERVER   0x4
#define CLSCTX_ALL            (CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER)

/// Always S_OK - nothing to start.
HRESULT CoInitialize(void* reserved);
/// Declared only - no body in compat/ (see the section note above).
HRESULT CoInitializeEx(void* reserved, DWORD coInit);
/// Does nothing.
void    CoUninitialize(void);
/// Declared only - no body in compat/ (see the section note above).
HRESULT CoCreateInstance(const CLSID& rclsid, IUnknown* pUnkOuter, DWORD dwClsContext,
                         const IID& riid, void** ppv);
/// Declared only - no body in compat/ (see the section note above).
void*   CoTaskMemAlloc(size_t cb);
/// Declared only - no body in compat/ (see the section note above).
void    CoTaskMemFree(void* pv);

// ---------------------------------------------------------------------------
// Window, messages and key state - DECLARATIONS WITHOUT BODIES
// ---------------------------------------------------------------------------
// The last tail of `UserInterface`. In the browser the message loop
// does not exist: events come from `canvas` and from `document`. The names have
// to exist so that the files parse, and the linker is to stop where they have
// to be translated into events.
#define WM_CHAR         0x0102
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_SYSKEYDOWN   0x0104
#define WM_SYSKEYUP     0x0105
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION   0x010E
#define WM_IME_COMPOSITION      0x010F
#define WM_IME_NOTIFY           0x0282
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_RBUTTONDOWN  0x0204
#define WM_RBUTTONUP    0x0205
#define WM_MOUSEWHEEL   0x020A
#define WM_QUIT         0x0012
#define WM_CLOSE        0x0010
#define WM_ACTIVATE     0x0006

#define SM_CXSCREEN        0
#define SM_CYSCREEN        1
#define SM_CXFULLSCREEN   16
#define SM_CYFULLSCREEN   17

/// A resource number passed as a name pointer.
#define MAKEINTRESOURCE(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))

typedef short SHORT;

int   GetSystemMetrics(int index);
SHORT GetKeyState(int vKey);
SHORT GetAsyncKeyState(int vKey);
/// `rename`, but - as in Win32 - FALSE when the target already exists (POSIX
/// would overwrite it).
BOOL  MoveFileA(LPCSTR from, LPCSTR to);
#ifndef MoveFile
#define MoveFile MoveFileA
#endif

/// Control of the x87 floating-point unit. `_PC_24` set single precision - in
/// wasm there is nothing to set, but the name has to exist. (The same
/// `_controlfp` that settled that `fistp` rounds rather than
/// truncates.)
#define _MCW_PC 0x00030000
#define _PC_24  0x00020000
#define _PC_53  0x00010000
#define _PC_64  0x00000000
unsigned int _controlfp(unsigned int newValue, unsigned int mask);

/// The simpler variant of `_beginthreadex` - the same rule: no body.
uintptr_t _beginthread(void (__cdecl *start_address)(void*), unsigned stack_size,
                       void* arglist);

// ---------------------------------------------------------------------------
// Cursor, icons, system settings - THE LAST TAIL OF `UserInterface`
// ---------------------------------------------------------------------------
// Listed with ONE grep over all the remaining files at once, not one name per
// run - the lesson.
//
// Everything below is DECLARATIONS WITHOUT BODIES. In the browser the cursor is
// set with the CSS `cursor` property, the icon comes from `<link rel=icon>`, and
// system settings (like "sticky keys") the page does not see and should not.
DECLARE_HANDLE(HCURSOR);
DECLARE_HANDLE(HICON);

#define IMAGE_BITMAP    0
#define IMAGE_ICON      1
#define IMAGE_CURSOR    2
#define LR_DEFAULTCOLOR 0x0000
#define LR_VGACOLOR     0x0080
#define LR_LOADFROMFILE 0x0010

#define MB_ICONSTOP        0x00000010
#define MB_ICONWARNING     0x00000030
#define MB_YESNO           0x00000004
#define IDYES              6
#define IDNO               7

#define HWND_TOP        ((HWND)0)
#define HWND_TOPMOST    ((HWND)-1)
#define SWP_SHOWWINDOW  0x0040
#define SWP_NOSIZE      0x0001
#define SWP_NOMOVE      0x0002

#define CP_ACP          0
#define CP_UTF8         65001

// "Sticky keys" - Windows offers them after five presses of Shift and can
// interrupt the game with that. The client switches them off for the duration
// of play. In the browser there is nothing to switch off.
#define SPI_GETSTICKYKEYS 0x003A
#define SPI_SETSTICKYKEYS 0x003B
#define SKF_STICKYKEYSON  0x00000001
#define SKF_AVAILABLE     0x00000002
#define SKF_HOTKEYACTIVE  0x00000004
#define SKF_CONFIRMHOTKEY 0x00000008

typedef struct tagSTICKYKEYS {
    UINT  cbSize;
    DWORD dwFlags;
} STICKYKEYS, *LPSTICKYKEYS;

typedef struct tagSECURITY_ATTRIBUTES {
    DWORD  nLength;
    void*  lpSecurityDescriptor;
    BOOL   bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

// Resolution of the multimedia timer. In the browser time comes from
// `performance.now()`, whose resolution cannot be changed anyway.
typedef struct tagTIMECAPS {
    UINT wPeriodMin;
    UINT wPeriodMax;
} TIMECAPS, *LPTIMECAPS;

#define TIMERR_NOERROR 0
/// Always TIMERR_NOERROR - the browser timer resolution cannot be changed.
UINT timeBeginPeriod(UINT period);
/// Always TIMERR_NOERROR.
UINT timeEndPeriod(UINT period);
UINT timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc);

#define WM_APP 0x8000

int     ShowCursor(BOOL show);
/// Forwards to `M2W_SetCursor` (cursor_web.cpp).
HCURSOR SetCursor(HCURSOR cursor);

/// Cursors - see compat/cursor_web.cpp. The handle carries the RESOURCE
/// NUMBER, because the client keeps these handles and passes them back on every
/// change of cursor shape. A meaningless handle would give one shape for
/// everything.
void* M2W_CursorHandle(int iResource);
/// Sets the cursor shape carried by `hCursor`; returns the previous handle.
HCURSOR M2W_SetCursor(HCURSOR hCursor);

/// Shows or hides the cursor on the canvas. Called by `ShowCursor`, which
/// keeps a show counter according to the Win32 contract.
void M2W_CursorVisible(int bVisible);
/// Moves the port's mouse position (`M2W_MouseSet`); the real pointer cannot
/// be moved by a page.
BOOL    SetCursorPos(int x, int y);
BOOL    GetCursorPos(POINT* pt);
BOOL    DestroyCursor(HCURSOR cursor);
/// Always TRUE, the point unchanged - the canvas is the only "screen".
BOOL    ClientToScreen(HWND wnd, POINT* pt);
/// Always TRUE, the point unchanged.
BOOL    ScreenToClient(HWND wnd, POINT* pt);
/// Always FALSE - the page owns the canvas.
BOOL    SetWindowPos(HWND wnd, HWND after, int x, int y, int cx, int cy, UINT flags);

HICON   LoadIconA(HINSTANCE inst, LPCSTR name);
#ifndef LoadIcon
#define LoadIcon LoadIconA
#endif
int     LoadStringA(HINSTANCE inst, UINT id, LPSTR buffer, int size);
#ifndef LoadString
#define LoadString LoadStringA
#endif
HWND    FindWindowA(LPCSTR className, LPCSTR windowName);
#ifndef FindWindow
#define FindWindow FindWindowA
#endif
void*   LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT load);
#ifndef LoadImage
#define LoadImage LoadImageA
#endif

DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR defaultValue,
                               LPSTR buffer, DWORD size, LPCSTR file);
#ifndef GetPrivateProfileString
#define GetPrivateProfileString GetPrivateProfileStringA
#endif

BOOL  SetFileAttributesA(LPCSTR name, DWORD attributes);
#ifndef SetFileAttributes
#define SetFileAttributes SetFileAttributesA
#endif
/// `getcwd`; Win32 lengths: without the terminator when it fits, the size
/// needed including it when it does not.
DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer);
#ifndef GetCurrentDirectory
#define GetCurrentDirectory GetCurrentDirectoryA
#endif
BOOL  SystemParametersInfoA(UINT action, UINT param, void* data, UINT update);
#ifndef SystemParametersInfo
#define SystemParametersInfo SystemParametersInfoA
#endif
/// Always TRUE (single-threaded).
BOOL  ReleaseMutex(HANDLE mutex);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sec, BOOL initialOwner, LPCSTR name);
#ifndef CreateMutex
#define CreateMutex CreateMutexA
#endif

/// `lstrlen` is Win32's `strlen` - the name comes from the days when there
/// were separate variants for "long" pointers.
#ifndef lstrlen
#define lstrlen  strlen
#endif
#ifndef lstrcpy
#define lstrcpy  strcpy
#endif
#ifndef lstrcmp
#define lstrcmp  strcmp
#endif

/// `_alloca` is MSVC's name for `alloca` from `<alloca.h>`.
#include <alloca.h>
#ifndef _alloca
#define _alloca alloca
#endif

// ---------------------------------------------------------------------------
// Crash handling
// ---------------------------------------------------------------------------
/// TMP4's `EterBase/error.cpp` stands on SEH (`SetUnhandledExceptionFilter`
/// plus `StackWalk64`), which does not exist outside Windows. It is replaced
/// by `compat/platform_crash.cpp`: five POSIX signals plus `std::terminate`.
void SetEterExceptionHandler();

/// Installs the five POSIX signal handlers + std::terminate (platform_crash.cpp).
/// Called through `SetEterExceptionHandler` from the `CPythonApplication`
/// constructor and directly by platform_crash_test.cpp; repeat calls are no-ops.
void UiPlatform_CrashHandlerInstall();
/// Whether `UiPlatform_CrashHandlerInstall` has run.
bool UiPlatform_CrashHandlerInstalled();

// ---------------------------------------------------------------------------
// A batch from measuring 39 files that do not compile yet
// ---------------------------------------------------------------------------
// Listed in ONE pass over all of them, not one error at a time.
typedef BOOL* LPBOOL;
typedef struct timeval TIMEVAL, *PTIMEVAL, *LPTIMEVAL;

/// Window user data. In Win32 an offset into the window structure; here only
/// a name, because there are no windows.
#define GWL_USERDATA  (-21)
#define GWL_STYLE     (-16)
#define GWL_EXSTYLE   (-20)
#define GWL_WNDPROC   (-4)

/// A window message. `MSApplication` keeps it in the main loop; in the browser
/// the loop is in `requestAnimationFrame`, but the type has to exist.
typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG, *PMSG, *LPMSG;

/// The font enumeration callback. See `LOGFONT` above: the page does not
/// enumerate system fonts, so this name exists only so that the declaration
/// in `eterLib/Util.h` parses.
typedef int (*FONTENUMPROCA)(const LOGFONTA*, const TEXTMETRICA*, DWORD, LPARAM);
typedef FONTENUMPROCA FONTENUMPROC;

/// The gamma ramp - 256 values per channel. `PythonGraphic` sets brightness
/// with it.
typedef struct _D3DGAMMARAMP {
    WORD red[256];
    WORD green[256];
    WORD blue[256];
} D3DGAMMARAMP;

HWND SetCapture(HWND wnd);
BOOL ReleaseCapture(void);

/// MSVC names for in-place case conversion.
char* _strupr(char* s);
/// Lower-cases the string in place; returns it.
char* _strlwr(char* s);
#ifndef _unlink
#define _unlink unlink
#endif
// `_mkdir` is NOT defined here. `compat/direct.h` declares it as a function,
// and a macro would shadow that declaration and break POSIX `mkdir` - it
// showed up as 'redefinition of mkdir as different kind of symbol'
// in ten files at once.

// ---------------------------------------------------------------------------
// The second batch of the same round - message loop, display mode, folders
// ---------------------------------------------------------------------------
#define PM_NOREMOVE 0x0000
#define PM_REMOVE   0x0001

BOOL PeekMessageA(LPMSG msg, HWND wnd, UINT filterMin, UINT filterMax, UINT remove);
BOOL GetMessageA(LPMSG msg, HWND wnd, UINT filterMin, UINT filterMax);
BOOL TranslateMessage(const MSG* msg);
LRESULT DispatchMessageA(const MSG* msg);
#ifndef PeekMessage
#define PeekMessage     PeekMessageA
#define GetMessage      GetMessageA
#define DispatchMessage DispatchMessageA
#endif

LONG GetWindowLongA(HWND wnd, int index);
LONG SetWindowLongA(HWND wnd, int index, LONG newValue);
#ifndef GetWindowLong
#define GetWindowLong GetWindowLongA
#define SetWindowLong SetWindowLongA
#endif

int EnumFontFamiliesExA(HDC dc, LOGFONTA* lf, FONTENUMPROCA proc, LPARAM lp, DWORD flags);
#ifndef EnumFontFamiliesEx
#define EnumFontFamiliesEx EnumFontFamiliesExA
#endif

/// The display mode. In the browser the game window size is the canvas size -
/// there is nothing to switch, but `PythonApplicationProcedure` fills this
/// structure.
typedef struct _devicemodeA {
    char  dmDeviceName[32];
    WORD  dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmDisplayFlags;
    DWORD dmDisplayFrequency;
} DEVMODEA, *LPDEVMODEA;
typedef DEVMODEA DEVMODE;
typedef LPDEVMODEA LPDEVMODE;

#define DM_BITSPERPEL       0x00040000ul
#define DM_PELSWIDTH        0x00080000ul
#define DM_PELSHEIGHT       0x00100000ul
#define DM_DISPLAYFREQUENCY 0x00400000ul
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define CDS_FULLSCREEN      0x00000004ul
#define DISP_CHANGE_SUCCESSFUL 0

BOOL EnumDisplaySettingsA(LPCSTR device, DWORD mode, LPDEVMODEA dm);
LONG ChangeDisplaySettingsA(LPDEVMODEA dm, DWORD flags);
#ifndef EnumDisplaySettings
#define EnumDisplaySettings   EnumDisplaySettingsA
#define ChangeDisplaySettings ChangeDisplaySettingsA
#endif

#define CSIDL_PERSONAL 0x0005
#define CSIDL_DESKTOP  0x0000
#define CSIDL_APPDATA  0x001a
BOOL SHGetSpecialFolderPathA(HWND wnd, LPSTR path, int folder, BOOL create);
#ifndef SHGetSpecialFolderPath
#define SHGetSpecialFolderPath SHGetSpecialFolderPathA
#endif

#define D3DCAPS2_FULLSCREENGAMMA 0x00020000ul
#define D3DCAPS2_CANCALIBRATEGAMMA 0x00100000ul

/// Winsock declares the last parameter as `int*`, POSIX as `socklen_t*`. On
/// wasm32 both are 32 bits, but they are **different types** - the same
/// problem as with `DWORD`. An overload removes it without
/// touching the sources.
inline int recvfrom(int sock, void* buffer, size_t length, int flags,
                    struct sockaddr* sender, int* addrLength)
{
    socklen_t tmp = addrLength ? static_cast<socklen_t>(*addrLength) : 0;
    const int result = ::recvfrom(sock, buffer, length, flags, sender,
                                  addrLength ? &tmp : nullptr);
    if (addrLength) *addrLength = static_cast<int>(tmp);
    return result;
}

// ---------------------------------------------------------------------------
// Converting bytes to wide characters - see `platform_codec.cpp`
// ---------------------------------------------------------------------------
// NOTE: only the UTF-8 code page is handled. For the others (CP949 Korean,
// CP1250, CP874...) the functions return 0, i.e. "I did not convert". That is
// one decision to be made - code page tables or moving the data to UTF-8 - not
// several scattered stubs.
int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR input, int bytes,
                        WCHAR* output, int capacity);
int WideCharToMultiByte(UINT codePage, DWORD flags, const WCHAR* input, int chars,
                        LPSTR output, int capacity, LPCSTR defaultChar, BOOL* usedDefault);

// The rest of the small things from measuring 32 files.
#define DISP_CHANGE_RESTART     1
#define DISP_CHANGE_FAILED    (-1)
#define DISP_CHANGE_BADMODE   (-2)

/// Always 0 - there is no window procedure.
LRESULT DefWindowProcA(HWND wnd, UINT message, WPARAM w, LPARAM l);
#ifndef DefWindowProc
#define DefWindowProc DefWindowProcA
#endif

/// The DirectInput interface identifier. `eterLib/Input.cpp` passes it to
/// `DirectInput8Create`; input in the port goes through `canvas` events, so
/// this constant exists so that the file parses.
extern const GUID IID_IDirectInput8A;
#ifndef IID_IDirectInput8
#define IID_IDirectInput8 IID_IDirectInput8A
#endif

/// The DirectDraw surface description - `EterImageLib/DXTCImage.cpp` reads the
/// DDS file header with it. It is a **data format** (category A), so the field
/// layout has to match to the byte.
typedef struct _DDPIXELFORMAT {
    DWORD dwSize, dwFlags, dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask, dwGBitMask, dwBBitMask, dwRGBAlphaBitMask;
} DDPIXELFORMAT;

typedef struct _DDSCAPS2 {
    DWORD dwCaps, dwCaps2, dwCaps3, dwCaps4;
} DDSCAPS2;

typedef struct _DDSURFACEDESC2 {
    DWORD dwSize, dwFlags, dwHeight, dwWidth;
    union { LONG lPitch; DWORD dwLinearSize; };
    DWORD dwBackBufferCount;
    union { DWORD dwMipMapCount; DWORD dwRefreshRate; };
    DWORD dwAlphaBitDepth;
    DWORD dwReserved;
    void* lpSurface;
    DWORD ddckCKDestOverlay[2];
    DWORD ddckCKDestBlt[2];
    DWORD ddckCKSrcOverlay[2];
    DWORD ddckCKSrcBlt[2];
    DDPIXELFORMAT ddpfPixelFormat;
    DDSCAPS2 ddsCaps;
    DWORD dwTextureStage;
} DDSURFACEDESC2;

#define DDSD_CAPS        0x00000001
#define DDSD_HEIGHT      0x00000002
#define DDSD_WIDTH       0x00000004
#define DDSD_PIXELFORMAT 0x00001000
#define DDSD_MIPMAPCOUNT 0x00020000
#define DDSD_LINEARSIZE  0x00080000
#define DDPF_FOURCC      0x00000004
#define DDPF_RGB         0x00000040
#define DDPF_ALPHAPIXELS 0x00000001
#define DDSCAPS_TEXTURE  0x00001000
#define DDSCAPS_MIPMAP   0x00400000

// Window messages and show commands - the complement.
#define WM_CREATE       0x0001
#define WM_DESTROY      0x0002
#define WM_MOVE         0x0003
#define WM_SIZE         0x0005
#define WM_SETFOCUS     0x0007
#define WM_KILLFOCUS    0x0008
#define WM_PAINT        0x000F
#define WM_ERASEBKGND   0x0014
#define WM_SYSCOMMAND   0x0112
#define WM_SETCURSOR    0x0020
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN  0x0207
#define WM_MBUTTONUP    0x0208
#define WM_ACTIVATEAPP  0x001C

#define SIZE_RESTORED   0
#define SIZE_MINIMIZED  1
#define SIZE_MAXIMIZED  2

#define SW_HIDE         0
#define SW_SHOWNORMAL   1
#define SW_MINIMIZE     6
#define SW_RESTORE      9
#define SW_SHOW         5

#define SC_MONITORPOWER 0xF170
#define SC_SCREENSAVE   0xF140

/// Always FALSE - the page owns the canvas.
BOOL ShowWindow(HWND wnd, int command);
/// Always FALSE - the canvas is never minimised.
BOOL IsIconic(HWND wnd);

#define DDSD_PITCH      0x00000008
#define DDSD_PIXELSIZE  0x00000020

/// DirectInput device identifiers - keyboard and mouse.
extern const GUID GUID_SysKeyboard;
extern const GUID GUID_SysMouse;

#define WA_INACTIVE    0
#define WA_ACTIVE      1
#define WA_CLICKACTIVE 2

// The last missing constants from the measurement.
#define DDPF_ALPHAPREMULT 0x00008000
#define DDPF_LUMINANCE    0x00020000
#define DDPF_ALPHA        0x00000002

#define WM_INPUTLANGCHANGE 0x0051
#define WM_IME_SETCONTEXT  0x0281
#define WM_IME_CHAR        0x0286

#define D3DSGR_NO_CALIBRATION  0x00000000ul
#define D3DSGR_CALIBRATE       0x00000001ul

/// Always TRUE - every frame is redrawn anyway.
BOOL InvalidateRect(HWND wnd, const RECT* rect, BOOL erase);
/// Always TRUE.
BOOL UpdateWindow(HWND wnd);

// ---------------------------------------------------------------------------
// Window: registration, creation, rectangles
// ---------------------------------------------------------------------------
// `MSWindow.cpp` creates and handles a Win32 window. In the browser the window
// belongs to the PAGE: it is never closed, and a page cannot intercept its
// own tab closing. These names exist
// so that the file parses; the answers are in `platform_none.cpp`.
typedef struct tagWNDCLASSA {
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HGDIOBJ   hbrBackground;
    LPCSTR    lpszMenuName;
    LPCSTR    lpszClassName;
} WNDCLASSA, *LPWNDCLASSA;
typedef WNDCLASSA WNDCLASS;

#define CS_HREDRAW 0x0002
#define CS_VREDRAW 0x0001
#define CS_OWNDC   0x0020
#define SWP_NOZORDER 0x0004
#define IDI_APPLICATION ((LPCSTR)32512)
#define IDC_ARROW       ((LPCSTR)32512)

HWND    CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style,
                        int x, int y, int width, int height, HWND parent,
                        void* menu, HINSTANCE inst, void* param);
#ifndef CreateWindow
/// `CreateWindowExA` with extended style 0.
#define CreateWindow(cls, title, style, x, y, w, h, parent, menu, inst, param) \
        CreateWindowExA(0, (cls), (title), (style), (x), (y), (w), (h), \
                        (parent), (menu), (inst), (param))
#define CreateWindowEx CreateWindowExA
#endif
/// Always FALSE - a page cannot close its tab.
BOOL    DestroyWindow(HWND wnd);
BOOL    IsWindow(HWND wnd);
BOOL    GetClientRect(HWND wnd, RECT* rect);
BOOL    GetWindowRect(HWND wnd, RECT* rect);
/// Always FALSE - the page owns the canvas.
BOOL    MoveWindow(HWND wnd, int x, int y, int width, int height, BOOL repaint);
/// Always FALSE (the page title is not the window title).
BOOL    SetWindowTextA(HWND wnd, LPCSTR title);
#ifndef SetWindowText
#define SetWindowText SetWindowTextA
#endif
/// Always NULL.
void*   GetMenu(HWND wnd);
/// Always NULL - cursors come from `M2W_CursorHandle`.
HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name);
#ifndef LoadCursor
#define LoadCursor LoadCursorA
#endif
/// The window last passed to `SetCapture` (platform_none.cpp).
HWND    GetCapture(void);

/// `SetRect` fills a rectangle - pure arithmetic, so it **has a body**.
inline BOOL SetRect(RECT* rect, int left, int top, int right, int bottom)
{
    if (!rect) return FALSE;
    rect->left = left;   rect->top = top;
    rect->right = right; rect->bottom = bottom;
    return TRUE;
}

/// GWL_STYLE through `GetWindowLongA`.
#define GetWindowStyle(wnd)   ((DWORD)GetWindowLongA((wnd), GWL_STYLE))
/// GWL_EXSTYLE through `GetWindowLongA`.
#define GetWindowExStyle(wnd) ((DWORD)GetWindowLongA((wnd), GWL_EXSTYLE))

#define WM_EXITSIZEMOVE 0x0232
#define ISC_SHOWUICOMPOSITIONWINDOW   0x80000000ul
#define ISC_SHOWUIALLCANDIDATEWINDOW  0x0000000Ful
/// Always FALSE, the rectangle unchanged - no window frame.
BOOL AdjustWindowRect(RECT* rect, DWORD style, BOOL menu);
/// Always FALSE, the rectangle unchanged.
BOOL AdjustWindowRectEx(RECT* rect, DWORD style, BOOL menu, DWORD exStyle);

typedef WORD ATOM;

ATOM RegisterClassA(const WNDCLASSA* cls);
BOOL UnregisterClassA(LPCSTR name, HINSTANCE inst);
#ifndef RegisterClass
#define RegisterClass   RegisterClassA
#define UnregisterClass UnregisterClassA
#endif

// `boolean` and the `__RPCNDR_H__` guard - see `compat/rpcndr.h`, which
// describes the damage to the `jconfig.h` copy in the TMP4 repository.

// BACKSLASH IN PATHS - see compat/paths_web.cpp.
//
// TMP4 code builds paths the Windows way ("lib\\os.pyc"), and the wasm file
// system is POSIX, where a backslash is an ordinary name character. Without
// the conversion the client does not find its own files and ends with a
// message that tells the truth about something else.

/// `c_szPath` with a drive letter (`d:/`) stripped and backslashes turned
/// into slashes - in one of a few rotating buffers, only when there is a
/// backslash; otherwise the input pointer itself.
const char* M2W_PosixPath(const char* c_szPath);
/// `access()` on the path after `M2W_ResolvePathCase`.
int M2W_Access(const char* c_szPath, int iMode);
/// `fopen()`: case resolved when reading; when writing the path is converted
/// and its directories created first.
FILE* M2W_Fopen(const char* c_szPath, const char* c_szMode);

/// A path that REALLY exists - with the letter case resolved. Windows did not
/// tell cases apart, the wasm file system does, and the imports in the game
/// scripts alone differ in case in 60 places.
const char* M2W_ResolvePathCase(const char* c_szPath);

/// The missing-files log - see compat/paths_web.cpp. `NULL` when it was not
/// asked for with `?missing=1`.
std::FILE* M2W_MissingFileLog();

// `_access` from the Windows C library. It goes through `M2W_Access`, because
// TMP4 code builds paths the Windows way - see compat/paths_web.cpp.
//
// It used to stand in `compat/io.h` and reached some files only INCIDENTALLY,
// through Python's system `pyconfig.h`. When the Python headers changed to the
// ones from the wasm build, the chain disappeared and four files
// stopped compiling. A dependency nobody knew about, because nobody had written
// it down.
#ifndef _access
#define _access M2W_Access
#endif


#include "rpcndr.h"

#ifdef __cplusplus
/// the GUI scale (`?scale=N`) - the game sees a
/// logical screen (physical / N), the buffer stays physical. See
/// platform_none.cpp / gl_device.cpp / platform_text.cpp.
extern "C" float M2W_UiScale();
/// platform_text.cpp: the font-atlas shadow in physical px for the DIB whose
/// (16-bit) content equals `pData` of size w x h. NULL = not this one.
const unsigned short* M2W_FontAtlasHiRes(const void* pData, unsigned w, unsigned h,
                                         unsigned* puWidth, unsigned* puHeight);
#endif

#ifdef __cplusplus
/// `?events=loop` - movement events
/// counted as in the original (one frame per loop turn) instead of from time
/// - an A/B measurement.
extern "C" int M2W_EventsFromLoop();
#endif
