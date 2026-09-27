// unpack_pack.cpp - extracts files from an `.epk` pack WITH THE CLIENT'S READER.
//
// ===========================================================================
// WHY NOT AN OWN READER IN PYTHON
// ===========================================================================
// The pack format is described in `eterPack/EterPack.h` and could be rewritten:
// the `.eix` index is LZO-packed and encrypted with four key words,
// and every file in the `.epk` has its own compression type (`COMPRESSED_TYPE_*`),
// of which one is encrypted with Panama, and two "hybrid".
//
// Rewriting that would mean writing a SECOND reader of the same format -
// and a second chance for a mistake. And we already have the first one, it is in the tree, it is
// compiled and it passed its tests (`eterpack_test.cpp`).
//
// So this tool DOES NOT READ the format. It calls `CEterPack::Get`, i.e.
// exactly the function the game uses. If it ever turns out that it
// extracts garbage, that means the game reads it too - and that is the right
// conclusion, not "my reader has a bug".
//
// ===========================================================================
// HOW IT RUNS
// ===========================================================================
// This is a wasm program run in node, with `-sNODERAWFS=1` - i.e.
// with access to REAL files, not to an in-memory file system.
// The packs are 1.4 GB and pulling them into browser memory would make no
// sense; here the point is a one-off extraction of the scripts to disk.
//
//     em++ ... unpack_pack.cpp <sources> -sNODERAWFS=1 -o unpack_pack.js
//     node unpack_pack.js <pack_directory> <name> <target_directory>
//
// for example:
//
//     node unpack_pack.js ../../reference/Client/pack root ../../build/port/scripts
//
// ===========================================================================
// WHAT IT DOES NOT DO
// ===========================================================================
// It rewrites nothing and does not change a single byte. The game scripts are written
// in Python 2 and come out that way - rewriting them for Python 3 is a separate step
// and a separate tool. Mixing those two things in one program
// would mean that with a wrong result nobody knows which part failed.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "win32_compat.h"

#include "EterPack.h"
#include "EterPackManager.h"
#include "MappedFile.h"
#include "lzo.h"

#include <cctype>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

/// Creates a directory together with its parents. `mkdir -p` in one function, because
/// the names in the pack contain paths (`uiscript/loginwindow.py`), and
/// the file system does not care that a directory "will be needed in a moment".
bool CreateDirectories(const std::string& c_rstPath)
{
    for (size_t i = 1; i < c_rstPath.size(); ++i)
    {
        if (c_rstPath[i] != '/' && c_rstPath[i] != '\\')
            continue;

        std::string kPart = c_rstPath.substr(0, i);
        mkdir(kPart.c_str(), 0777);   // already exists? fine
    }
    return true;
}

/// Whether one path segment is a Windows device name (CON, PRN, AUX, NUL,
/// COM1-9, LPT1-9 - with any extension): writing to it would go to the device.
bool IsReservedWindowsName(const std::string& c_rstSegment)
{
    std::string kBase = c_rstSegment.substr(0, c_rstSegment.find('.'));
    for (size_t i = 0; i < kBase.size(); ++i)
        kBase[i] = (char)std::toupper((unsigned char)kBase[i]);
    if (kBase == "CON" || kBase == "PRN" || kBase == "AUX" || kBase == "NUL")
        return true;
    return kBase.size() == 4 && (kBase.compare(0, 3, "COM") == 0 || kBase.compare(0, 3, "LPT") == 0) &&
           kBase[3] >= '1' && kBase[3] <= '9';
}

/// Whether a pack entry name (already `/`-separated, drive letter and leading
/// `/` removed) stays inside the target directory: not empty, no `..`
/// segment, no segment ending in a dot or a space (Windows drops them, so
/// `.. ` would become `..`), no device name, no `:` and no control characters.
bool IsSafeRelativeName(const std::string& c_rstName)
{
    if (c_rstName.empty())
        return false;
    size_t uStart = 0;
    while (uStart <= c_rstName.size())
    {
        size_t uEnd = c_rstName.find('/', uStart);
        if (uEnd == std::string::npos)
            uEnd = c_rstName.size();
        const std::string kSegment = c_rstName.substr(uStart, uEnd - uStart);
        if (kSegment == "..")
            return false;
        if (!kSegment.empty() && (kSegment.back() == '.' || kSegment.back() == ' ') && kSegment != ".")
            return false;
        if (IsReservedWindowsName(kSegment))
            return false;
        uStart = uEnd + 1;
    }
    for (size_t i = 0; i < c_rstName.size(); ++i)
    {
        const unsigned char c = (unsigned char)c_rstName[i];
        if (c == ':' || c < 0x20)
            return false;
    }
    return true;
}

}  // namespace

/// `<pack_directory> <name> <target_directory>`: extracts every entry of the
/// pack through `CEterPack::Get`; exit 1 when nothing came out.
int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: %s <pack_directory> <name> <target_directory>\n",
                    argv[0]);
        std::printf("e.g.:  %s pack root scripts\n", argv[0]);
        return 1;
    }

    const char* c_szDirectory = argv[1];
    const char* c_szName = argv[2];
    const char* c_szTarget = argv[3];

    // `CEterPack` calls `CLZO::Instance()` on every read, so the object
    // must exist BEFORE we open the pack. This is not a formality -
    // the singleton registers itself when created, and without it the read fails.
    CLZO kLzo;
    CEterFileDict kDict;
    CEterPack kPack;

    if (!kPack.Create(kDict, c_szName, c_szDirectory, /*bReadOnly*/ true))
    {
        std::printf("cannot open pack '%s' in '%s'\n",
                    c_szName, c_szDirectory);
        return 1;
    }

    TDataPositionMap& rkIndex = kPack.GetIndexMap();
    std::printf("pack '%s': %d entries\n", c_szName,
                static_cast<int>(rkIndex.size()));

    int iExtracted = 0, iEmpty = 0, iFailed = 0;

    for (TDataPositionMap::iterator it = rkIndex.begin();
         it != rkIndex.end(); ++it)
    {
        const TEterPackIndex* c_pEntry = it->second;
        if (!c_pEntry)
            continue;

        const std::string kName(c_pEntry->filename);
        if (kName.empty())
            continue;

        CMappedFile kFile;
        LPCVOID pvData = NULL;
        if (!kPack.Get(kFile, kName.c_str(), &pvData) || !pvData)
        {
            std::printf("  READ ERROR: %s\n", kName.c_str());
            ++iFailed;
            continue;
        }

        const size_t uSize = static_cast<size_t>(kFile.Size());
        if (uSize == 0)
        {
            ++iEmpty;
            continue;
        }

        // Names in the pack may have Windows backslashes. The same problem
        // as in `compat/paths_web.cpp` - only here it concerns the CONTENT
        // of the pack, not a path given by the game code.
        std::string kRelative = kName;
        for (size_t i = 0; i < kRelative.size(); ++i)
            if (kRelative[i] == '\\')
                kRelative[i] = '/';

        // THE DRIVE LETTER MUST GO BEFORE THE PATH IS JOINED.
        //
        // Some packs keep names as FULL Windows paths
        // (`d:/ymir work/pc/assassin/...`). Appending them to the target
        // directory gives `<target>/d:/ymir work/...`, and `d:` is not a legal
        // directory component - every write failed.
        //
        // Measured: `patch1` (92 MB, the server's patch) - 441 write errors
        // and ZERO extracted files. The same with `pc2` and `uiloading`. Three packs
        // out of 108, but exactly those three carry the character models and what the server
        // added after the release - i.e. exactly the content we were missing.
        //
        // The client does the same translation in `M2W_PosixPath`; the corpus has to
        // name the files the same way, or it will not find them by name.
        if (kRelative.size() > 2 && kRelative[1] == ':')
            kRelative = kRelative.substr(2);
        while (!kRelative.empty() && kRelative[0] == '/')
            kRelative = kRelative.substr(1);

        // NO NAME MAY LEAVE THE TARGET DIRECTORY (security audit).
        // A pack is data, and packs are downloaded from the internet: a name
        // like `../../x` would otherwise be written OUTSIDE the target (the
        // file system resolves `..`), a `:` left after the drive letter is an
        // NTFS stream or another drive. Such entries are refused and counted.
        if (!IsSafeRelativeName(kRelative))
        {
            std::printf("  UNSAFE NAME, skipped: %s\n", kName.c_str());
            ++iFailed;
            continue;
        }

        const std::string kTargetPath = std::string(c_szTarget) + "/" + kRelative;

        CreateDirectories(kTargetPath);

        FILE* pOut = std::fopen(kTargetPath.c_str(), "wb");
        if (!pOut)
        {
            std::printf("  WRITE ERROR: %s\n", kTargetPath.c_str());
            ++iFailed;
            continue;
        }

        std::fwrite(pvData, 1, uSize, pOut);
        std::fclose(pOut);
        ++iExtracted;
    }

    std::printf("\nextracted %d | empty %d | failed %d\n",
                iExtracted, iEmpty, iFailed);

    // A check: a pack without a single extracted file means something is
    // wrong - not "the pack was empty". Better to say it here than
    // to let an empty directory look like a result.
    if (iExtracted == 0)
    {
        std::printf("NOTHING CAME OUT - this is not a result, it is an error\n");
        return 1;
    }
    return 0;
}
