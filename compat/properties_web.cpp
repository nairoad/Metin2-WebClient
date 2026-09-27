// SPDX-License-Identifier: GPL-2.0-or-later
// properties_web.cpp - CRC -> map-object property file. Map objects
// (buildings, trees, rocks) are named in `areadata.txt` by the CRC of their
// `.prb/.prt/...` file; this table answers which file that is, so
// `CPropertyManager::Get` can register it lazily.

// Design:
// After entering the world syserr.txt held 599 lines of
// " CArea::LoadObject Property(#) Load ERROR".
// The original resolves a CRC only after REGISTERING every file in
// `property/` at start - by listing the directory
// (`CPropertyLoader::Create("*.*")`) or reading `pack/property`. We have
// neither: no packs, and `property/` is 1835 files scattered across the
// chunks of the streamed corpus, so fetching them all at start would pull
// hundreds of megabytes of other people's chunks. Registration at start
// was therefore empty and every `Get(CRC)` missed.
//
// `tools/build_client_data.py` reads the CRC (second line, after `YPRT`) of
// every `property/` file in the corpus and writes `property/crc_index.txt`
// (`<crc><TAB><path in corpus>`) into the preloaded package. It is read
// here once, lazily; `CPropertyManager::Get(DWORD)` (patched in
// tools/stage_port.py) asks `M2W_PropertyFile(crc)` on a miss and registers
// THAT ONE file, fetched from the corpus on demand like any other.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>

namespace
{

/// The CRC -> path table, loaded on first use.
std::map<unsigned long, std::string>& Index()
{
    static std::map<unsigned long, std::string> s_kIndex;
    static bool s_bLoaded = false;
    if (s_bLoaded)
        return s_kIndex;
    s_bLoaded = true;

    FILE* f = fopen("property/crc_index.txt", "rb");
    if (!f)
    {
        printf("m2w properties: no property/crc_index.txt - map objects will "
               "have no properties (run tools/build_client_data.py)\n");
        return s_kIndex;
    }
    char szLine[1024];
    while (fgets(szLine, sizeof(szLine), f))
    {
        char* pTab = strchr(szLine, '\t');
        if (!pTab)
            continue;
        *pTab = 0;
        char* pPath = pTab + 1;
        size_t n = strlen(pPath);
        while (n && (pPath[n - 1] == '\n' || pPath[n - 1] == '\r'))
            pPath[--n] = 0;
        if (!n)
            continue;
        s_kIndex[strtoul(szLine, NULL, 10)] = pPath;
    }
    fclose(f);
    printf("m2w properties: CRC -> file index: %u entries\n",
           (unsigned)s_kIndex.size());
    return s_kIndex;
}

}  // namespace

/// Path of the property file with this CRC, or NULL when the index has none.
extern "C" const char* M2W_PropertyFile(unsigned long ulCRC)
{
    std::map<unsigned long, std::string>& k = Index();
    std::map<unsigned long, std::string>::const_iterator it = k.find(ulCRC);
    return it == k.end() ? NULL : it->second.c_str();
}

/// Whether the index exists - `CPythonBackground::__CreateProperty` picks
/// the file mode by it (tools/stage_port.py).
extern "C" int M2W_HasPropertyIndex()
{
    return Index().empty() ? 0 : 1;
}
