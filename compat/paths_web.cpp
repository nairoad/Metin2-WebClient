// SPDX-License-Identifier: GPL-2.0-or-later
// paths_web.cpp - the file-path seam between the Windows-shaped client and
// the POSIX file system of wasm: backslashes, drive letters, case-insensitive
// names, directories created on write, the corpus consulted on a miss, and
// the log of files nobody could find.

// Design:
// Backslashes: with the game data in place the client still
// died with "FATAL ERROR!! Python Library file not exist!". The files were
// there; `UserInterface.cpp` builds `"lib\\"` and on POSIX a backslash is an
// ORDINARY character of a name, so `lib\os.pyc` is one oddly named file,
// not `os.pyc` in `lib`. There are a couple of dozen such paths in the
// `.cpp` tree (24 counted; a grep of string literals today
// gives 27 path-like ones); fixing them one by one would be that many
// edits to TMP4 code, and this difference between Windows and the target
// system is exactly what the compatibility layer is for. The conversion goes one way
// only (backslash -> slash), never touches file contents or names inside
// packs, and a drive letter (`d:/ymir work/`, left in the data from the
// authors' machines) is cut off, since no drive means anything here.
//
// Case: "ModuleNotFoundError: No module named 'debugInfo'" for
// `debuginfo.py` - Windows ignores case, wasm does not, and sixty imports
// in the scripts alone differ from their file names, plus textures, icons
// and sounds. Not a data defect but a difference between systems: the
// path is tried as given first (the common case, free), and only on a miss
// rebuilt member by member from a directory scan, with the result cached.
//
// Corpus: a file missing locally may be streamed
// (webfs_web.cpp). This is the one place to hook it, because `fopen`,
// `CreateFile` and `_access` all come through here. Local data (our
// Python 3 scripts, the standard library, settings) wins over the corpus.
// NOT every miss may be cached (rule, recorded
// VF): when the corpus
// KNOWS the name but did not deliver it, that is a transient failure, and
// caching it turned one stumble into a permanent absence - the item icons
// (`icon/item/13000.tga` is in the manifest with 1707 others) were blank
// for a whole session.
//
// Directories on write: the client assumes its own directories
// exist (the installer made them). `CGuildMarkImage::Load` builds and
// saves the file on a miss and calls itself again - with a failing save a
// recursion without bottom ("Maximum call stack size exceeded" on the
// character screen). The cause is fixed (the directory exists), not the
// symptom, because a missing write directory would sooner or later also
// break screenshots, logs and settings.

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <map>
#include <sys/stat.h>

#include "win32_compat.h"

#include <unistd.h>

#include <emscripten/emscripten.h>

// Streaming of the game data - webfs_web.cpp.
bool M2W_CorpusGet(const char* c_szPath, const char* c_szWhere);
bool M2W_CorpusKnows(const char* c_szPath);
bool M2W_CorpusStarted();

namespace
{

/// Rotating buffers. The caller gets a `const char*` and uses it AT ONCE,
/// usually as the argument of the next call - so the memory must outlive
/// the call, but not longer. Several buffers, because
/// `rename(Posix(a), Posix(b))` evaluates both arguments BEFORE `rename`
/// runs; one buffer would give the same path twice, a bug that shows only
/// when a file is moved - rarely and far from its cause.
const int c_iBuffers = 8;
const int c_iLength = 1024;

char g_aszBuffers[c_iBuffers][c_iLength];
int  g_iNextBuffer = 0;

/// Whether the path starts with a drive letter (`d:/`, `C:\\`).
bool StartsWithDrive(const char* c_sz)
{
    return c_sz[0] && c_sz[1] == ':' &&
           (c_sz[2] == '/' || c_sz[2] == '\\') &&
           ((c_sz[0] >= 'a' && c_sz[0] <= 'z') ||
            (c_sz[0] >= 'A' && c_sz[0] <= 'Z'));
}

/// Resolved paths: key = the path asked for, value = the one that really
/// exists. An empty value means "checked, absent", so the directories are
/// not scanned again for the same miss.
std::map<std::string, std::string> g_kResolved;

/// True when `stat` succeeds for the path (file or directory).
bool Exists(const std::string& c_rstPath)
{
    struct stat kStat;
    return stat(c_rstPath.c_str(), &kStat) == 0;
}

/// A name in the directory differing from `c_rstWanted` only in case, or
/// an empty string.
std::string FindIgnoringCase(const std::string& c_rstDirectory,
                             const std::string& c_rstWanted)
{
    DIR* pDir = opendir(c_rstDirectory.empty() ? "." : c_rstDirectory.c_str());
    if (!pDir)
        return std::string();

    std::string kFound;
    struct dirent* pEntry;

    while ((pEntry = readdir(pDir)) != NULL)
    {
        const std::string kName(pEntry->d_name);
        if (kName.size() != c_rstWanted.size())
            continue;

        size_t i = 0;
        for (; i < kName.size(); ++i)
        {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(kName[i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(c_rstWanted[i])));
            if (a != b)
                break;
        }

        if (i == kName.size())
        {
            kFound = kName;
            break;
        }
    }

    closedir(pDir);
    return kFound;
}

/// Whether `?missing=1` asks for the log of missing files.
EM_JS(int, m2w_missing_log_enabled, (void), {
    return m2w.options.get('missing') === '1' ? 1 : 0;
});

/// Whether `?nomodels=1` hides every `.gr2`.
EM_JS(int, m2w_no_models, (void), {
    return m2w.options.get('nomodels') === '1' ? 1 : 0;
});

/// NEGATIVE CONTROL: the user reported that the
/// stutter began together with `.gr2` support, and "fix the gr2 layer,
/// ask if it helped" cannot tell "the fix missed" from "the cause is
/// elsewhere". With this flag model files stop existing - as before part
/// 225 - on the SAME binary, in the same session: one reload and a
/// comparison. (Result: `.gr2` was innocent.)
bool NoModels()
{
    static int s_iWanted = -1;
    if (s_iWanted < 0)
        s_iWanted = m2w_no_models();
    return s_iWanted != 0;
}

/// Whether the name ends in `.gr2` (any case).
bool IsModel(const char* c_szName)
{
    const size_t uLength = c_szName ? std::strlen(c_szName) : 0;
    if (uLength < 4)
        return false;
    const char* c = c_szName + uLength - 4;
    return c[0] == '.' &&
           (c[1] == 'g' || c[1] == 'G') &&
           (c[2] == 'r' || c[2] == 'R') &&
           c[3] == '2';
}

/// Creates the missing directories on the way to a file about to be
/// written. `EEXIST` is the NORMAL outcome for most of them.
void CreateDirectoriesFor(const char* c_szPath)
{
    if (!c_szPath || !*c_szPath)
        return;

    char szBuffer[512];
    std::snprintf(szBuffer, sizeof(szBuffer), "%s", c_szPath);

    for (char* p = szBuffer + 1; *p; ++p)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(szBuffer, 0777);
        *p = '/';
    }
}

}  // namespace

/// Backslashes -> slashes, drive letter removed. Returns the input pointer
/// when nothing had to change, otherwise one of eight rotating buffers.
const char* M2W_PosixPath(const char* c_szPath)
{
    if (!c_szPath)
        return NULL;

    // The drive letter goes: `EterPackManager.cpp` even has
    // `PATH_ABSOLUTE_YMIRWORK1 "d:/ymir work/"` as a constant. The pack
    // manager strips it before looking into a pack, but `GetFromFile`
    // passes it as is. Which letter does not matter - there are none.
    if (StartsWithDrive(c_szPath))
        c_szPath += 3;

    // A path without a backslash returns WITHOUT copying - not for speed
    // but for correctness: the fewer paths pass through the rotating
    // buffers, the smaller the chance one is overwritten before use.
    if (!std::strchr(c_szPath, '\\'))
        return c_szPath;

    char* pTarget = g_aszBuffers[g_iNextBuffer];
    g_iNextBuffer = (g_iNextBuffer + 1) % c_iBuffers;

    int i = 0;
    for (; c_szPath[i] && i < c_iLength - 1; ++i)
        pTarget[i] = (c_szPath[i] == '\\') ? '/' : c_szPath[i];
    pTarget[i] = '\0';

    return pTarget;
}

/// The `/missing.txt` log of files nobody could find (`?missing=1`), or NULL.
/// It lives in the in-memory file system; read it in the browser console with
/// `Module.FS.readFile('/missing.txt', {encoding: 'utf8'})` (stdio buffers it, so the
/// last few names may appear only after more are written).
std::FILE* M2W_MissingFileLog()
{
    // Opened on request only: writing thousands of names on every start
    // would cost memory and give nothing until somebody reads it.
    static std::FILE* s_pFile = NULL;
    static bool s_bChecked = false;

    if (!s_bChecked)
    {
        s_bChecked = true;
        if (m2w_missing_log_enabled())
        {
            s_pFile = std::fopen("/missing.txt", "wb");
            if (s_pFile)
                std::printf("m2w paths: writing missing files to /missing.txt\n");
        }
    }
    return s_pFile;
}

namespace
{

/// Rebuilds the absolute or relative path `c_rAsked` member by member into
/// `rBuilt`: each member as given when it exists, else the entry of that
/// directory that matches it ignoring case (FindIgnoringCase). False as
/// soon as a member has no match - then `rBuilt` holds the part found.
bool RebuildIgnoringCase(const std::string& c_rAsked, std::string& rBuilt)
{
    rBuilt.clear();
    size_t uStart = 0;

    if (!c_rAsked.empty() && c_rAsked[0] == '/')
    {
        rBuilt = "/";
        uStart = 1;
    }

    while (uStart <= c_rAsked.size())
    {
        size_t uEnd = c_rAsked.find('/', uStart);
        if (uEnd == std::string::npos)
            uEnd = c_rAsked.size();

        const std::string kMember = c_rAsked.substr(uStart, uEnd - uStart);
        if (kMember.empty())
        {
            uStart = uEnd + 1;
            continue;
        }

        std::string kTry = rBuilt;
        if (!kTry.empty() && kTry[kTry.size() - 1] != '/')
            kTry += "/";
        kTry += kMember;

        if (Exists(kTry))
        {
            rBuilt = kTry;
        }
        else
        {
            const std::string kOther = FindIgnoringCase(rBuilt, kMember);
            if (kOther.empty())
            {
                return false;
            }
            if (!rBuilt.empty() && rBuilt[rBuilt.size() - 1] != '/')
                rBuilt += "/";
            rBuilt += kOther;
        }

        if (uEnd == c_rAsked.size())
            break;
        uStart = uEnd + 1;
    }
    return true;
}

}  // namespace

/// The path that really exists for `c_szPath`: as given, or with members
/// re-cased, or fetched from the corpus; the input (POSIX form) when none.
const char* M2W_ResolvePathCase(const char* c_szPath)
{
    if (!c_szPath)
        return NULL;

    if (NoModels() && IsModel(c_szPath))
        return c_szPath;

    // Slashes first - otherwise the split would take `lib\os.pyc` for one
    // member.
    const char* c_szPosix = M2W_PosixPath(c_szPath);

    if (Exists(c_szPosix))
        return c_szPosix;

    const std::string kAsked(c_szPosix);

    std::map<std::string, std::string>::const_iterator it = g_kResolved.find(kAsked);
    if (it != g_kResolved.end())
        return it->second.empty() ? c_szPosix : it->second.c_str();

    // Rebuild member by member.
    std::string kBuilt;
    const bool bFound = RebuildIgnoringCase(kAsked, kBuilt);

    // The corpus is asked ONLY after the local search failed (see the
    // design note).
    if (!bFound && M2W_CorpusGet(kAsked.c_str(), kAsked.c_str()))
    {
        g_kResolved[kAsked] = kAsked;
        return g_kResolved[kAsked].c_str();
    }

    // A known-but-undelivered name is a transient failure, not an absence:
    // do not cache it. Reported ONCE per name, since this
    // branch runs only on the first question - later the cache answers.
    if (!bFound && M2W_CorpusKnows(kAsked.c_str()))
    {
        std::printf("m2w paths: the corpus KNOWS [%s] but did not deliver it - "
                    "not caching the miss, will retry\n",
                    kAsked.c_str());
        return c_szPosix;
    }

    // Misses are cached too - the client asks for missing files often and
    // on purpose (is there a localised version of this resource?).
    g_kResolved[kAsked] = bFound ? kBuilt : std::string();

    // The log of missing files: the client asks for thousands of files and
    // never says which it did not get - it draws without them or skips the
    // effect. Every name once (the cache above guarantees this branch runs
    // once per name; a missing effect in the draw loop would otherwise fill
    // the log in seconds).
    if (!bFound && M2W_MissingFileLog())
        std::fprintf(M2W_MissingFileLog(), "%s\n", kAsked.c_str());

    if (!bFound)
        return c_szPosix;

    return g_kResolved[kAsked].c_str();
}

/// `_access` of win32_compat.h: `access` on the resolved path.
int M2W_Access(const char* c_szPath, int iMode)
{
    // Works only when EVERY object in the link went through the `_access`
    // macro of win32_compat.h: a hand-built `libeterpack.a`
    // called libc's bare `access`, `CEterPackManager::isExist` bypassed the
    // corpus and item icons were blank. Since then tools/build_eterpack.py
    // builds the archive and tools/gates/check_link.py checks the symbol.
    return access(M2W_ResolvePathCase(c_szPath), iMode);
}

/// `fopen` of win32_compat.h: resolves case when reading, creates the
/// directories when writing.
FILE* M2W_Fopen(const char* c_szPath, const char* c_szMode)
{
    if (!c_szPath || !c_szMode)
        return NULL;

    // Case is resolved ONLY when reading. When writing, the file does not
    // exist yet by assumption, so a case search would find nothing or -
    // worse - hit somebody else's file and overwrite it.
    const bool bRead = (c_szMode[0] == 'r');

    if (bRead)
        return std::fopen(M2W_ResolvePathCase(c_szPath), c_szMode);

    // When writing, the directory may not exist yet. `M2W_PosixPath`
    // returns a rotating buffer, which `CreateDirectoriesFor` copies
    // before anything else can reuse it.
    const char* c_szPosix = M2W_PosixPath(c_szPath);
    CreateDirectoriesFor(c_szPosix);
    return std::fopen(c_szPosix, c_szMode);
}
