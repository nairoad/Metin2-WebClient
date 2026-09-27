// Reads DXT blocks from stdin (raw bytes), decompresses them and prints
// the ARGB pixels in hex. Used for comparison with an INDEPENDENT decoder.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "dxt.h"

/// `<format> <width> <height>` < blocks: prints one `AARRGGBB` line per pixel,
/// or `REFUSED` (exit 1) when the decoder rejects the input; exit 2 on bad usage.
int main(int argc, char** argv)
{
    if (argc < 4) return 2;
    const int iFormat = std::atoi(argv[1]);
    const int iWidth = std::atoi(argv[2]);
    const int iHeight = std::atoi(argv[3]);

    std::vector<unsigned char> kBlocks;
    int c;
    while ((c = std::getchar()) != EOF)
        kBlocks.push_back((unsigned char)c);

    std::vector<std::uint32_t> kPixels((size_t)iWidth * iHeight, 0);
    if (!M2W_DecodeDxt((EM2wDxtFormat)iFormat, kBlocks.data(),
                          (unsigned)kBlocks.size(), iWidth, iHeight, kPixels.data()))
    {
        std::printf("REFUSED\n");
        return 1;
    }
    for (size_t i = 0; i < kPixels.size(); ++i)
        std::printf("%08X\n", (unsigned)kPixels[i]);
    return 0;
}
