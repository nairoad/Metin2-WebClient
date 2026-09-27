// d3d8_pixels_test.cpp - test of converting pixels between formats.
//
// WHY: because an error here gives a picture that draws and is wrong. The most common
// of them - unpacking bits by a shift instead of by replication - makes
// white stop being white and the whole picture dim. Nobody will name it,
// everybody will say "somehow dark".
//
// Running:
//   em++ -std=c++17 -O1 -Icompat compat/tests/d3d8_pixels_test.cpp \
//        compat/d3d8_pixels.cpp compat/win32_compat.cpp \
//        -o d3d8_pixels_test.js && node d3d8_pixels_test.js

#include <cstdio>
#include <vector>

#include "d3d8_pixels.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-62s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("\n=== converting pixels between Direct3D formats ===\n\n");

    // -----------------------------------------------------------------
    std::printf("[white MUST stay white - replication, not a shift]\n");
    {
        // This is the whole point of this file. `0xF` unpacked by `<< 4` gives
        // 240 instead of 255; the picture dims and darkens with every pass.
        unsigned char b16[2];
        M2W_ArgbToPixel(D3DFMT_A4R4G4B4, 0xFFFFFFFFu, b16);
        const DWORD wWhite4 = M2W_PixelToArgb(D3DFMT_A4R4G4B4, b16);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(0x%08lX)",
                      static_cast<unsigned long>(wWhite4));
        Check("A4R4G4B4: white there and back stays white",
                wWhite4 == 0xFFFFFFFFu, buf);

        M2W_ArgbToPixel(D3DFMT_A1R5G5B5, 0xFFFFFFFFu, b16);
        const DWORD wWhite5 = M2W_PixelToArgb(D3DFMT_A1R5G5B5, b16);
        std::snprintf(buf, sizeof(buf), "(0x%08lX)",
                      static_cast<unsigned long>(wWhite5));
        Check("A1R5G5B5: white there and back stays white",
                wWhite5 == 0xFFFFFFFFu, buf);

        M2W_ArgbToPixel(D3DFMT_R5G6B5, 0xFFFFFFFFu, b16);
        const DWORD wWhite565 = M2W_PixelToArgb(D3DFMT_R5G6B5, b16);
        Check("R5G6B5: white there and back stays white",
                wWhite565 == 0xFFFFFFFFu);

        // OPPOSITE control: if the unpacking were a shift,
        // 0xF0 = 240 would come out. I check directly that it did NOT.
        Check("surely not 240 instead of 255",
                ((wWhite4 >> 16) & 0xFF) != 0xF0);
    }

    // -----------------------------------------------------------------
    std::printf("\n[black stays black]\n");
    {
        unsigned char b[2];
        M2W_ArgbToPixel(D3DFMT_A4R4G4B4, 0x00000000u, b);
        Check("A4R4G4B4: transparent black",
                M2W_PixelToArgb(D3DFMT_A4R4G4B4, b) == 0x00000000u);

        M2W_ArgbToPixel(D3DFMT_A1R5G5B5, 0x00000000u, b);
        Check("A1R5G5B5: transparent black",
                M2W_PixelToArgb(D3DFMT_A1R5G5B5, b) == 0x00000000u);
    }

    // -----------------------------------------------------------------
    std::printf("[the order of the components - B and R not swapped]\n");
    {
        // The Direct3D pixel 0xAARRGGBB = 0xFF102030 lies as B G R A.
        const unsigned char source[4] = { 0x30, 0x20, 0x10, 0xFF };
        const DWORD w = M2W_PixelToArgb(D3DFMT_A8R8G8B8, source);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(0x%08lX)", static_cast<unsigned long>(w));
        Check("A8R8G8B8 reads as 0xFF102030", w == 0xFF102030u, buf);

        unsigned char target[4] = { 0, 0, 0, 0 };
        M2W_ArgbToPixel(D3DFMT_A8R8G8B8, 0xFF102030u, target);
        Check("and writes back the same way",
                target[0] == 0x30 && target[1] == 0x20 && target[2] == 0x10 && target[3] == 0xFF);
    }

    // -----------------------------------------------------------------
    std::printf("\n[X means full opacity, not garbage]\n");
    {
        // In `X8R8G8B8` the fourth byte is unused. Direct3D read it as
        // 255, not as whatever happened to lie there - otherwise pictures without alpha
        // would be randomly transparent.
        const unsigned char source[4] = { 0x30, 0x20, 0x10, 0x07 };
        const DWORD w = M2W_PixelToArgb(D3DFMT_X8R8G8B8, source);
        Check("X8R8G8B8 gives alpha 255, despite garbage in the byte",
                ((w >> 24) & 0xFF) == 0xFF);

        const unsigned char z2[2] = { 0x00, 0x00 };
        Check("X1R5G5B5 gives alpha 255 too",
                ((M2W_PixelToArgb(D3DFMT_X1R5G5B5, z2) >> 24) & 0xFF) == 0xFF);
    }

    // -----------------------------------------------------------------
    std::printf("\n[one-bit alpha: the threshold at the half]\n");
    {
        unsigned char b[2];
        M2W_ArgbToPixel(D3DFMT_A1R5G5B5, 0x80FFFFFFu, b);
        Check("alpha 128 -> visible",
                ((M2W_PixelToArgb(D3DFMT_A1R5G5B5, b) >> 24) & 0xFF) == 0xFF);

        M2W_ArgbToPixel(D3DFMT_A1R5G5B5, 0x7FFFFFFFu, b);
        Check("alpha 127 -> invisible",
                ((M2W_PixelToArgb(D3DFMT_A1R5G5B5, b) >> 24) & 0xFF) == 0x00);
    }

    // -----------------------------------------------------------------
    std::printf("\n[pixel sizes]\n");
    {
        Check("A8R8G8B8 is four bytes",
                M2W_BytesPerPixel(D3DFMT_A8R8G8B8) == 4);
        Check("R8G8B8 is three",
                M2W_BytesPerPixel(D3DFMT_R8G8B8) == 3);
        Check("A4R4G4B4 is two",
                M2W_BytesPerPixel(D3DFMT_A4R4G4B4) == 2);
        // DXT has no fixed pixel size - and that must be visible.
        Check("DXT1 has no pixel size",
                M2W_BytesPerPixel(D3DFMT_DXT1) == 0);
        Check("DXT is not convertible",
                !M2W_FormatConvertible(D3DFMT_DXT1));
    }

    // -----------------------------------------------------------------
    std::printf("\n[copying a rectangle without scaling]\n");
    {
        // 2x2 in A8R8G8B8 -> 2x2 in A4R4G4B4.
        unsigned char source[2 * 2 * 4];
        const DWORD colors[4] = { 0xFF000000u, 0xFFFF0000u,
                                  0xFF00FF00u, 0xFF0000FFu };
        for (int i = 0; i < 4; ++i)
            M2W_ArgbToPixel(D3DFMT_A8R8G8B8, colors[i], source + i * 4);

        unsigned char target[2 * 2 * 2];
        const bool bOk = M2W_ConvertPixelRect(
            D3DFMT_A8R8G8B8, source, 2 * 4, 2, 2,
            D3DFMT_A4R4G4B4, target, 2 * 2, 2, 2, false);
        Check("the conversion succeeded", bOk);

        const DWORD wRed = M2W_PixelToArgb(D3DFMT_A4R4G4B4, target + 2);
        Check("red stayed red", wRed == 0xFFFF0000u);
        const DWORD wBlue = M2W_PixelToArgb(D3DFMT_A4R4G4B4, target + 6);
        Check("blue stayed blue", wBlue == 0xFF0000FFu);
    }

    // -----------------------------------------------------------------
    std::printf("\n[halving - two filters]\n");
    {
        // 2x2: black, white / white, black. Averaging gives grey 128.
        unsigned char source[2 * 2 * 4];
        const DWORD colors[4] = { 0xFF000000u, 0xFFFFFFFFu,
                                  0xFFFFFFFFu, 0xFF000000u };
        for (int i = 0; i < 4; ++i)
            M2W_ArgbToPixel(D3DFMT_A8R8G8B8, colors[i], source + i * 4);

        unsigned char target[4];
        M2W_ConvertPixelRect(D3DFMT_A8R8G8B8, source, 2 * 4, 2, 2,
                               D3DFMT_A8R8G8B8, target, 4, 1, 1, true);
        const DWORD w = M2W_PixelToArgb(D3DFMT_A8R8G8B8, target);
        const DWORD r = (w >> 16) & 0xFF;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(%lu, expected about 128)",
                      static_cast<unsigned long>(r));
        Check("linear: a checkerboard gives grey", r > 120 && r < 136, buf);

        // And the point filter takes ONE pixel - so the result is extreme,
        // not in between. This distinction is the whole difference between
        // `D3DX_FILTER_NONE` and `D3DX_FILTER_LINEAR`.
        M2W_ConvertPixelRect(D3DFMT_A8R8G8B8, source, 2 * 4, 2, 2,
                               D3DFMT_A8R8G8B8, target, 4, 1, 1, false);
        const DWORD w2 = M2W_PixelToArgb(D3DFMT_A8R8G8B8, target);
        const DWORD r2 = (w2 >> 16) & 0xFF;
        Check("point: takes one pixel, does not average",
                r2 == 0x00 || r2 == 0xFF);
    }

    // -----------------------------------------------------------------
    std::printf("\n[a format we cannot handle SAYS so]\n");
    {
        unsigned char source[16] = { 0 };
        unsigned char target[16] = { 0 };
        // A compressed source - unpacking DXT is separate work.
        // A silent no-op would leave a black rectangle.
        Check("DXT as the source -> refused, not silence",
                !M2W_ConvertPixelRect(D3DFMT_DXT1, source, 8, 4, 4,
                                        D3DFMT_A8R8G8B8, target, 16, 4, 4, false));
        Check("unknown target format -> refused",
                !M2W_ConvertPixelRect(D3DFMT_A8R8G8B8, source, 16, 2, 2,
                                        D3DFMT_UNKNOWN, target, 16, 2, 2, false));
    }

    // -----------------------------------------------------------------
    std::printf("\n[OPPOSITE controls]\n");
    {
        // If `M2W_PixelToArgb` always returned the same, the tests above would have
        // zero value.
        const unsigned char a[4] = { 1, 2, 3, 4 };
        const unsigned char b[4] = { 9, 8, 7, 6 };
        Check("different pixels give different results",
                M2W_PixelToArgb(D3DFMT_A8R8G8B8, a) !=
                M2W_PixelToArgb(D3DFMT_A8R8G8B8, b));

        // And that the 32 -> 16 conversion REALLY loses precision - otherwise
        // it would mean that somewhere along the way nothing happens.
        unsigned char c16[2];
        M2W_ArgbToPixel(D3DFMT_A4R4G4B4, 0xFF123456u, c16);
        Check("32 bits -> 16 bits loses precision (as it should)",
                M2W_PixelToArgb(D3DFMT_A4R4G4B4, c16) != 0xFF123456u);
    }

    std::printf("\n=== %s ===\n",
                errors == 0 ? "PIXEL CONVERSION WORKS" : "TEST FAILED");
    return errors == 0 ? 0 : 1;
}
