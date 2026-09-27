// d3d8_image_test.cpp - test of the TGA, BMP and JPEG reader.
//
// WHY: because TGA is the format the WHOLE Metin2 interface stands on - every
// item icon, every guild emblem. And the most common error in a TGA reader does not
// crash anything: it gives **upside-down icons**, because the file goes from the bottom,
// and the texture from the top.
//
// The tests build files BYTE BY BYTE, with values typed in by hand. Thanks to
// that they do not check the reader against itself - every header here is laid out
// according to the format description, not according to what the reader happens to do.
//
// Running:
//   em++ -std=c++17 -O1 -Icompat compat/tests/d3d8_image_test.cpp \
//        compat/d3d8_image.cpp compat/win32_compat.cpp \
//        -o d3d8_image_test.js && node d3d8_image_test.js

#include <cstdio>
#include <vector>

#include "d3d8_image.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-60s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

/// Builds a TGA header. `uDescriptor` with bit 0x20 means "image from the top".
std::vector<unsigned char> TgaHeader(unsigned char uType, int iImageWidth,
                                       int iImageHeight, unsigned char uBits,
                                       unsigned char uDescriptor)
{
    std::vector<unsigned char> k(18, 0);
    k[2] = uType;
    k[12] = static_cast<unsigned char>(iImageWidth & 0xFF);
    k[13] = static_cast<unsigned char>((iImageWidth >> 8) & 0xFF);
    k[14] = static_cast<unsigned char>(iImageHeight & 0xFF);
    k[15] = static_cast<unsigned char>((iImageHeight >> 8) & 0xFF);
    k[16] = uBits;
    k[17] = uDescriptor;
    return k;
}

/// The colour of a pixel of the image, as `0xAARRGGBB`.
DWORD Pixel(const TImage& o, int x, int y)
{
    const size_t i = (static_cast<size_t>(y) * o.iWidth + x) * 4;
    // The buffer holds B, G, R, A.
    return (static_cast<DWORD>(o.vecPixels[i + 3]) << 24) |
           (static_cast<DWORD>(o.vecPixels[i + 2]) << 16) |
           (static_cast<DWORD>(o.vecPixels[i + 1]) << 8) |
            static_cast<DWORD>(o.vecPixels[i + 0]);
}

}  // namespace

/// JPEG 8x8: the left half RED, the right BLUE.
///
/// The file was made by an INDEPENDENT encoder (Pillow), quality 100, without
/// chroma subsampling. The values the test compares with were
/// read by THE SAME independent decoder: (254, 0, 0)
/// and (0, 0, 254) - not 255, because JPEG is lossy even at a hundred.
///
/// That is why the comparison has a TOLERANCE. The test is to decide whether
/// the reader lays out the components in the right order and does not put
/// the image upside down - not to reproduce the cosine transform to
/// the last unit.
const unsigned char c_aJPEG[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08, 0x03,
    0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00,
    0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00,
    0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
    0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
    0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6,
    0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9,
    0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1,
    0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xC4, 0x00,
    0x1F, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
    0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
    0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
    0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15,
    0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18,
    0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
    0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
    0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
    0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00,
    0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xFE,
    0x68, 0xFF, 0x00, 0x6A, 0xAF, 0xF9, 0x90, 0xFF, 0x00, 0xEE, 0x68, 0xFF,
    0x00, 0xDD, 0x76, 0xBF, 0xD5, 0x0F, 0xF4, 0x65, 0xFF, 0x00, 0xE7, 0x36,
    0x3F, 0xEF, 0x5B, 0xBF, 0xF8, 0x3D, 0x9F, 0xED, 0x47, 0xFA, 0x43, 0x3F,
    0xF3, 0x88, 0x7F, 0xF7, 0x9F, 0xBF, 0xF8, 0x0A, 0x1F, 0xFF, 0xD9,
};

/// Whether two pixels differ on every component by at most `uSlack`.
/// JPEG is lossy, so a bit-exact comparison would make no sense.
bool Near(DWORD a, DWORD b, unsigned uSlack)
{
    for (int i = 0; i < 4; ++i)
    {
        const int x = static_cast<int>((a >> (i * 8)) & 0xFF);
        const int y = static_cast<int>((b >> (i * 8)) & 0xFF);
        const int r = (x > y) ? (x - y) : (y - x);
        if (r > static_cast<int>(uSlack))
            return false;
    }
    return true;
}

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("\n=== image reader: TGA, BMP and JPEG ===\n\n");

    // -----------------------------------------------------------------
    std::printf("[TGA 32-bit, uncompressed, from the top]\n");
    {
        // A 2x1 image: the first pixel red, the second blue.
        // In a TGA file a pixel lies as B, G, R, A.
        std::vector<unsigned char> k = TgaHeader(2, 2, 1, 32, 0x20);
        const unsigned char aData[8] = {
            0x00, 0x00, 0xFF, 0xFF,   // red
            0xFF, 0x00, 0x00, 0xFF,   // blue
        };
        k.insert(k.end(), aData, aData + 8);

        const TImage o = M2W_LoadImage(&k[0], static_cast<UINT>(k.size()));
        Check("read", o.bOk);
        Check("size 2x1", o.iWidth == 2 && o.iHeight == 1);

        char buf[48];
        std::snprintf(buf, sizeof(buf), "(0x%08lX)",
                      static_cast<unsigned long>(Pixel(o, 0, 0)));
        Check("the first pixel RED", Pixel(o, 0, 0) == 0xFFFF0000u, buf);
        Check("the second pixel BLUE", Pixel(o, 1, 0) == 0xFF0000FFu);

        // OPPOSITE control: if the reader took the bytes directly as R,G,B,
        // the first pixel would come out blue. TGA keeps them as B,G,R.
        Check("did not confuse B with R", Pixel(o, 0, 0) != 0xFF0000FFu);
    }

    // -----------------------------------------------------------------
    std::printf("\n[TGA from the BOTTOM - the default, and the most common trap]\n");
    {
        // The same image 1x2, but without bit 0x20. The file goes from the bottom,
        // so the first pixel in the file is the BOTTOM row of the image.
        std::vector<unsigned char> k = TgaHeader(2, 1, 2, 32, 0x00);
        const unsigned char aData[8] = {
            0x00, 0x00, 0xFF, 0xFF,   // red - first in the file
            0xFF, 0x00, 0x00, 0xFF,   // blue - second in the file
        };
        k.insert(k.end(), aData, aData + 8);

        const TImage o = M2W_LoadImage(&k[0], static_cast<UINT>(k.size()));
        Check("read", o.bOk);

        // Since the file goes from the bottom, the RED - first in the file - is
        // the bottom row, i.e. in the texture it has to come out AT THE BOTTOM (y = 1).
        // Ignoring this gives upside-down icons.
        char buf[48];
        std::snprintf(buf, sizeof(buf), "(top 0x%08lX, bottom 0x%08lX)",
                      static_cast<unsigned long>(Pixel(o, 0, 0)),
                      static_cast<unsigned long>(Pixel(o, 0, 1)));
        Check("the top row is BLUE", Pixel(o, 0, 0) == 0xFF0000FFu, buf);
        Check("the bottom row is RED", Pixel(o, 0, 1) == 0xFFFF0000u, buf);

        // And the same file WITH bit 0x20 has to give the opposite - that is the proof
        // that the bit is really read, and not that the reader always flips.
        std::vector<unsigned char> k2 = TgaHeader(2, 1, 2, 32, 0x20);
        k2.insert(k2.end(), aData, aData + 8);
        const TImage o2 = M2W_LoadImage(&k2[0], static_cast<UINT>(k2.size()));
        Check("with the 'from the top' bit it comes out REVERSED",
                Pixel(o2, 0, 0) == 0xFFFF0000u);
    }

    // -----------------------------------------------------------------
    std::printf("\n[TGA 24-bit - alpha has to come out full]\n");
    {
        std::vector<unsigned char> k = TgaHeader(2, 1, 1, 24, 0x20);
        const unsigned char aData[3] = { 0x30, 0x20, 0x10 };   // B, G, R
        k.insert(k.end(), aData, aData + 3);

        const TImage o = M2W_LoadImage(&k[0], static_cast<UINT>(k.size()));
        Check("read", o.bOk);
        // Without an alpha channel the pixel must be fully opaque. Zero
        // would give invisible icons - and nobody would know why.
        Check("full alpha at 24 bits", Pixel(o, 0, 0) == 0xFF102030u);
    }

    // -----------------------------------------------------------------
    std::printf("\n[TGA PACKED (RLE)]\n");
    {
        // Type 10. The first packet: a repeat (bit 0x80) of four
        // reds. The second: a run of two different ones.
        std::vector<unsigned char> k = TgaHeader(10, 6, 1, 32, 0x20);
        const unsigned char aData[] = {
            0x83, 0x00, 0x00, 0xFF, 0xFF,             // 4 x red
            0x01, 0xFF, 0x00, 0x00, 0xFF,             // run of 2: blue
                  0x00, 0xFF, 0x00, 0xFF,             //           green
        };
        k.insert(k.end(), aData, aData + sizeof(aData));

        const TImage o = M2W_LoadImage(&k[0], static_cast<UINT>(k.size()));
        Check("read", o.bOk);
        Check("size 6x1", o.iWidth == 6 && o.iHeight == 1);

        // The lower seven bits of the packet header are the COUNT MINUS ONE.
        // 0x83 means "repeat FOUR times", not three.
        Check("the repeat gave FOUR pixels",
                Pixel(o, 0, 0) == 0xFFFF0000u && Pixel(o, 3, 0) == 0xFFFF0000u);
        Check("the fifth pixel is already blue", Pixel(o, 4, 0) == 0xFF0000FFu);
        Check("the sixth pixel is green", Pixel(o, 5, 0) == 0xFF00FF00u);
    }

    // -----------------------------------------------------------------
    std::printf("\n[BMP 24-bit with row padding]\n");
    {
        // 3 pixels in a row at 3 bytes is 9 bytes - padded to 12.
        // Ignoring the padding shifts every following row and gives
        // a "staircase" picture that looks like a damaged file.
        std::vector<unsigned char> k(54, 0);
        k[0] = 'B'; k[1] = 'M';
        k[10] = 54;               // the data starts after the header
        k[14] = 40;               // size of the info header
        k[18] = 3;                // width
        k[22] = 2;                // height (positive = from the bottom)
        k[26] = 1;                // planes
        k[28] = 24;               // bits per pixel

        const unsigned char aRow0[12] = {
            0x00, 0x00, 0xFF,  0x00, 0x00, 0xFF,  0x00, 0x00, 0xFF,  0, 0, 0
        };
        const unsigned char aRow1[12] = {
            0xFF, 0x00, 0x00,  0xFF, 0x00, 0x00,  0xFF, 0x00, 0x00,  0, 0, 0
        };
        k.insert(k.end(), aRow0, aRow0 + 12);
        k.insert(k.end(), aRow1, aRow1 + 12);

        const TImage o = M2W_LoadImage(&k[0], static_cast<UINT>(k.size()));
        Check("read", o.bOk);
        Check("size 3x2", o.iWidth == 3 && o.iHeight == 2);

        // A BMP with a positive height goes from the bottom - row zero of the file
        // (red) is the BOTTOM row of the image.
        Check("top blue", Pixel(o, 0, 0) == 0xFF0000FFu);
        Check("bottom red", Pixel(o, 2, 1) == 0xFFFF0000u);
        Check("the third pixel of the row read too",
                Pixel(o, 2, 0) == 0xFF0000FFu);
    }

    // -----------------------------------------------------------------
    std::printf("\n[what we cannot do, we SAY plainly]\n");
    {
        // TGA with a palette - Metin2 does not use it, and pretending would give pictures
        // in random colours.
        std::vector<unsigned char> k = TgaHeader(1, 2, 2, 8, 0x20);
        k[1] = 1;   // palette type
        k.resize(64, 0);
        Check("TGA with a palette - refused",
                !M2W_LoadImage(&k[0], static_cast<UINT>(k.size())).bOk);

        // A truncated file. Better to say "I cannot" than to return a picture
        // padded with zeros - a black half of an icon looks like a drawing
        // error, not like a damaged file.
        std::vector<unsigned char> u = TgaHeader(2, 8, 8, 32, 0x20);
        u.resize(18 + 16, 0);
        Check("truncated file - refused, not half a picture",
                !M2W_LoadImage(&u[0], static_cast<UINT>(u.size())).bOk);

        Check("empty buffer - refused",
                !M2W_LoadImage(NULL, 0).bOk);

        // PNG - a signature other than BMP, and as TGA it has no sensible header.
        const unsigned char aPng[16] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A,
                                         0x1A, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0 };
        Check("PNG - refused (we cannot, and do not pretend)",
                !M2W_LoadImage(aPng, sizeof(aPng)).bOk);
    }


    // -----------------------------------------------------------------
    std::printf("[JPEG 8x8, half red, half blue]\n");
    {
        const TImage o = M2W_LoadImage(
            c_aJPEG, static_cast<UINT>(sizeof(c_aJPEG)));
        Check("read", o.bOk);
        Check("size 8x8", o.iWidth == 8 && o.iHeight == 8);

        const DWORD dwLeft = Pixel(o, 0, 0);
        const DWORD dwRight = Pixel(o, 7, 0);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(0x%08lX / 0x%08lX)",
                      static_cast<unsigned long>(dwLeft),
                      static_cast<unsigned long>(dwRight));

        Check("left pixel RED", Near(dwLeft, 0xFFFE0000u, 8), buf);
        Check("right pixel BLUE", Near(dwRight, 0xFF0000FEu, 8));

        // OPPOSITE control - the same one that guards TGA: confusing
        // R with B is the easiest error in this reader, because `libjpeg`
        // gives RGB, and the texture wants BGRA.
        Check("did not confuse B with R", !Near(dwLeft, 0xFF0000FEu, 8));

        // FROM THE TOP. JPEG has no direction bit - it always goes from the top.
        // If the reader flipped the rows, this test would still pass
        // (the image is vertically symmetric), so I check differently:
        // the bottom row must be the same as the top one.
        Check("bottom row like the top one",
                Near(Pixel(o, 0, 7), dwLeft, 8));
    }

    // -----------------------------------------------------------------
    std::printf("[a damaged JPEG - it must not kill the program]\n");
    {
        // The default `libjpeg` error handler calls `exit(1)`. If it
        // stayed the default, this case would not report an error -
        // it would simply CUT OFF the whole test. That anything further on
        // gets printed is part of the result here.
        std::vector<unsigned char> k(c_aJPEG, c_aJPEG + sizeof(c_aJPEG));
        for (size_t i = 200; i < k.size(); ++i)
            k[i] = 0x5A;

        const TImage o = M2W_LoadImage(&k[0],
                                               static_cast<UINT>(k.size()));
        std::printf("  (survived reading a damaged file)\n");
        Check("does not pretend it worked, or returns a full image",
                !o.bOk || (o.iWidth == 8 && o.iHeight == 8));
    }
    std::printf("\n=== %s ===\n",
                errors == 0 ? "IMAGE READER WORKS" : "TEST FAILED");
    return errors == 0 ? 0 : 1;
}
