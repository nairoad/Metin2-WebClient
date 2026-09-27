// repair_gr2.cpp - UNPACKING .gr2 SECTIONS WITH THE REAL granny2.dll.
//
// Our Oodle1 reader (compat/gr2_oodle1.cpp, tools/oodle1.py) is faithful
// to the algorithm's description and agrees byte for byte with the open
// reference implementation - and yet ~1% of the corpus files fall apart in the middle of a section
// (the description is incomplete, not us). Among them the building
// `zone/c/building/c1-013-6tower.gr2` (the house by Octavio in Jinno) and a few
// animations (dance_1/4/6, sad, jumagap, pabeop, damage).
//
// This program does with such files what the baker does with trees: on
// a machine with the Windows client it calls `GrannyDecompressData` from granny2.dll
// for every packed section and writes a file with RAW sections
// (Format 0). The file layout, the section table, relocations and types stay - only
// the Format/DataOffset/DataSize of the sections and the file size in the header change.
// The client reads Format 0 without any code change.
//
// Usage: repair_gr2.exe <granny2.dll> <input.gr2> <output.gr2>
// Exit code: 0 = written, 2 = the file had no packed sections,
//              1 = error (message on stderr).
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

typedef int (__stdcall *FnPadding)(int Format);
typedef bool (__stdcall *FnDecompress)(int Format, bool FileIsByteReversed,
                                       int CompressedBytesSize, void* CompressedBytes,
                                       int Stop0, int Stop1, int Stop2, void* DecompressedBytes);

/// Little-endian u32 at `p`.
static unsigned U32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
/// Stores `v` little-endian at `p`.
static void PutU32(unsigned char* p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }

/// Re-saves one .gr2 with every packed section decompressed by granny2.dll
/// (see the header for the exit codes).
int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: repair_gr2 <granny2.dll> <in.gr2> <out.gr2>\n"); return 1; }
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    FnPadding fnPad = (FnPadding)GetProcAddress(h, "_GrannyGetCompressedBytesPaddingSize@4");
    FnDecompress fnDec = (FnDecompress)GetProcAddress(h, "_GrannyDecompressData@32");
    if (!fnPad || !fnDec) { fprintf(stderr, "no Granny exports in the dll\n"); return 1; }

    FILE* f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    std::vector<unsigned char> inBytes;
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) inBytes.insert(inBytes.end(), buf, buf + n);
    fclose(f);
    if (inBytes.size() < 32 + 44) { fprintf(stderr, "too short\n"); return 1; }

    const unsigned uBase = 32;
    const unsigned uHeaderFormat = U32(&inBytes[20]);
    if (uHeaderFormat != 0) { fprintf(stderr, "packed header - not supported\n"); return 1; }
    const unsigned uSectionOffset = U32(&inBytes[uBase + 12]);
    const unsigned uSections = U32(&inBytes[uBase + 16]);
    if (uSectionOffset > inBytes.size()) { fprintf(stderr, "section table outside the file\n"); return 1; }
    const unsigned uTable = uBase + uSectionOffset;
    // Every size from the file is checked in 64 bits before use (this is a 32-bit program: size_t would wrap too;
    // security audit): a .gr2 may come from the internet, and 32-bit sums of
    // values taken from it wrap around.
    if ((unsigned long long)uTable + (unsigned long long)uSections * 44 > inBytes.size()) { fprintf(stderr, "section table outside the file\n"); return 1; }

    // Is there anything to do?
    bool bPacked = false;
    for (unsigned i = 0; i < uSections; ++i)
        if (U32(&inBytes[uTable + i * 44]) != 0 && U32(&inBytes[uTable + i * 44 + 12]) != 0) bPacked = true;
    if (!bPacked) return 2;

    // New file: header + section table (up to the end of the table), then the
    // section data and their relocation tables, in section order.
    std::vector<unsigned char> outBytes(inBytes.begin(), inBytes.begin() + uTable + uSections * 44);
    for (unsigned i = 0; i < uSections; ++i)
    {
        unsigned char* pDescIn = &inBytes[uTable + i * 44];
        const unsigned uFormat = U32(pDescIn + 0);
        const unsigned uDataOff = U32(pDescIn + 4);
        const unsigned uDataSize = U32(pDescIn + 8);
        const unsigned uExpanded = U32(pDescIn + 12);
        const unsigned uAlign = U32(pDescIn + 16);
        const unsigned uFirst16 = U32(pDescIn + 20);
        const unsigned uFirst8 = U32(pDescIn + 24);
        const unsigned uFixOff = U32(pDescIn + 28), uFixCnt = U32(pDescIn + 32);
        const unsigned uMixOff = U32(pDescIn + 36), uMixCnt = U32(pDescIn + 40);

        if (uFixCnt && (unsigned long long)uFixOff + (unsigned long long)uFixCnt * 12 > inBytes.size()) { fprintf(stderr, "section %u: fixups outside the file\n", i); return 1; }
        if (uMixCnt && (unsigned long long)uMixOff + (unsigned long long)uMixCnt * 16 > inBytes.size()) { fprintf(stderr, "section %u: marshalling outside the file\n", i); return 1; }
        // An expanded section over 256 MB, or an alignment that is not a power
        // of two up to 4096, is not a model - refused instead of allocated.
        if (uExpanded > (256u << 20)) { fprintf(stderr, "section %u: %u bytes expanded - refused\n", i, uExpanded); return 1; }
        if (uAlign && (uAlign > 4096 || (uAlign & (uAlign - 1)))) { fprintf(stderr, "section %u: alignment %u - refused\n", i, uAlign); return 1; }

        std::vector<unsigned char> sectionBytes;
        if (uExpanded)
        {
            if ((unsigned long long)uDataOff + uDataSize > inBytes.size()) { fprintf(stderr, "section %u outside the file\n", i); return 1; }
            if (uFormat == 0)
                sectionBytes.assign(inBytes.begin() + uDataOff, inBytes.begin() + uDataOff + uDataSize);
            else
            {
                // sizes handed to the dll as int must stay int; the padding
                // comes from the dll and is bounded too
                if (uDataSize > 0x7fffffffu || uExpanded > 0x7fffffffu) { fprintf(stderr, "section %u too large\n", i); return 1; }
                const int iPad = fnPad((int)uFormat);
                if (iPad < 0 || iPad > (1 << 20)) { fprintf(stderr, "section %u: padding %d - refused\n", i, iPad); return 1; }
                std::vector<unsigned char> packedBytes(inBytes.begin() + uDataOff, inBytes.begin() + uDataOff + uDataSize);
                packedBytes.resize(packedBytes.size() + (iPad > 0 ? iPad : 0) + 16, 0);
                sectionBytes.assign(uExpanded, 0);
                if (!fnDec((int)uFormat, false, (int)uDataSize, &packedBytes[0], (int)uFirst16, (int)uFirst8, (int)uExpanded, &sectionBytes[0]))
                { fprintf(stderr, "GrannyDecompressData: section %u (format %u) refused\n", i, uFormat); return 1; }
            }
        }
        // alignment of the section data
        const unsigned uAlignment = uAlign ? uAlign : 4;
        while (outBytes.size() % uAlignment) outBytes.push_back(0);
        const unsigned uNewOff = (unsigned)outBytes.size();
        outBytes.insert(outBytes.end(), sectionBytes.begin(), sectionBytes.end());
        while (outBytes.size() % 4) outBytes.push_back(0);
        unsigned uNewFix = 0, uNewMix = 0;
        if (uFixCnt) { uNewFix = (unsigned)outBytes.size(); outBytes.insert(outBytes.end(), inBytes.begin() + uFixOff, inBytes.begin() + uFixOff + (size_t)uFixCnt * 12); }
        if (uMixCnt) { uNewMix = (unsigned)outBytes.size(); outBytes.insert(outBytes.end(), inBytes.begin() + uMixOff, inBytes.begin() + uMixOff + (size_t)uMixCnt * 16); }

        unsigned char* pDescOut = &outBytes[uTable + i * 44];
        PutU32(pDescOut + 0, 0);
        PutU32(pDescOut + 4, uExpanded ? uNewOff : 0);
        PutU32(pDescOut + 8, uExpanded);
        PutU32(pDescOut + 12, uExpanded);
        PutU32(pDescOut + 28, uFixCnt ? uNewFix : 0);
        PutU32(pDescOut + 36, uMixCnt ? uNewMix : 0);
    }
    PutU32(&outBytes[uBase + 4], (unsigned)outBytes.size());   // file size in the header

    FILE* g = fopen(argv[3], "wb");
    if (!g) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
    fwrite(&outBytes[0], 1, outBytes.size(), g);
    fclose(g);
    printf("%s: %u sections, %u -> %u B\n", argv[2], uSections, (unsigned)inBytes.size(), (unsigned)outBytes.size());
    return 0;
}
