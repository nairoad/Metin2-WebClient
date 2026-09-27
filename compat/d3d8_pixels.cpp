// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_pixels.cpp - pixel packing, unpacking and resampling between
// Direct3D formats (see d3d8_pixels.h for the bit-expansion rule).

#include "d3d8_pixels.h"

#include <cstring>

namespace
{

/// Expands `iBits` bits to eight by REPLICATION, not by shifting: `0xF`
/// must give `0xFF`, not `0xF0` (see the header).
inline DWORD Expand(DWORD dwValue, int iBits)
{
    switch (iBits)
    {
        case 8: return dwValue & 0xFF;
        // 4 bits: 0xF -> 0xFF, the nibble replicated.
        case 4: return (dwValue << 4) | dwValue;
        // 5 bits: 0x1F -> 0xFF. The three HIGHEST bits are appended at the
        // bottom - a multiplication by 255/31 to within one level, for the
        // price of two shifts.
        case 6: return (dwValue << 2) | (dwValue >> 4);
        case 5: return (dwValue << 3) | (dwValue >> 2);
        // 1 bit: 0 or a full 255. One-bit alpha is there or not - there
        // were no intermediate values and there is nowhere to take them from.
        case 1: return dwValue ? 0xFFu : 0u;
        default: return dwValue;
    }
}

/// Reduces an 8-bit channel to its `iBits` highest bits (drops the low ones).
inline DWORD Truncate(DWORD dwEightBits, int iBits)
{
    return (dwEightBits & 0xFF) >> (8 - iBits);
}

/// Packs four 8-bit channels into A8R8G8B8 (each masked to 8 bits).
inline DWORD PackArgb(DWORD a, DWORD r, DWORD g, DWORD b)
{
    return ((a & 0xFF) << 24) | ((r & 0xFF) << 16) |
           ((g & 0xFF) << 8) | (b & 0xFF);
}

/// The source pixel nearest to the source-space point (fSx, fSy).
DWORD SampleNearest(D3DFORMAT eFormat, const unsigned char* c_pSource, int iPitch,
                    int iWidth, int iHeight, UINT uBytes, float fSx, float fSy)
{
    int sx = static_cast<int>(fSx + 0.5f);
    int sy = static_cast<int>(fSy + 0.5f);
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= iWidth) sx = iWidth - 1;
    if (sy >= iHeight) sy = iHeight - 1;
    return M2W_PixelToArgb(eFormat, c_pSource + static_cast<size_t>(sy) * iPitch + sx * uBytes);
}

/// Bilinear average of the four source pixels around (fSx, fSy), per
/// component, rounded.
DWORD SampleBilinear(D3DFORMAT eFormat, const unsigned char* c_pSource, int iPitch,
                     int iWidth, int iHeight, UINT uBytes, float fSx, float fSy)
{
    int sx0 = static_cast<int>(fSx);
    int sy0 = static_cast<int>(fSy);
    if (fSx < 0.0f) sx0 = 0;
    if (fSy < 0.0f) sy0 = 0;
    int sx1 = sx0 + 1;
    int sy1 = sy0 + 1;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 >= iWidth) sx1 = iWidth - 1;
    if (sy1 >= iHeight) sy1 = iHeight - 1;
    if (sx0 >= iWidth) sx0 = iWidth - 1;
    if (sy0 >= iHeight) sy0 = iHeight - 1;

    const float fUx = fSx - static_cast<float>(sx0);
    const float fUy = fSy - static_cast<float>(sy0);

    const DWORD a00 = M2W_PixelToArgb(eFormat, c_pSource + static_cast<size_t>(sy0) * iPitch + sx0 * uBytes);
    const DWORD a10 = M2W_PixelToArgb(eFormat, c_pSource + static_cast<size_t>(sy0) * iPitch + sx1 * uBytes);
    const DWORD a01 = M2W_PixelToArgb(eFormat, c_pSource + static_cast<size_t>(sy1) * iPitch + sx0 * uBytes);
    const DWORD a11 = M2W_PixelToArgb(eFormat, c_pSource + static_cast<size_t>(sy1) * iPitch + sx1 * uBytes);

    DWORD adwComponents[4];
    for (int i = 0; i < 4; ++i)
    {
        const int iShift = i * 8;
        const float f00 = static_cast<float>((a00 >> iShift) & 0xFF);
        const float f10 = static_cast<float>((a10 >> iShift) & 0xFF);
        const float f01 = static_cast<float>((a01 >> iShift) & 0xFF);
        const float f11 = static_cast<float>((a11 >> iShift) & 0xFF);

        const float fTop = f00 + (f10 - f00) * fUx;
        const float fBottom = f01 + (f11 - f01) * fUx;
        const float f = fTop + (fBottom - fTop) * fUy;

        adwComponents[i] = static_cast<DWORD>(f + 0.5f) & 0xFF;
    }

    return (adwComponents[3] << 24) | (adwComponents[2] << 16) |
           (adwComponents[1] << 8) | adwComponents[0];
}

}  // namespace

UINT M2W_BytesPerPixel(D3DFORMAT eFormat)
{
    switch (eFormat)
    {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            return 4;
        case D3DFMT_R8G8B8:
            return 3;
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_R5G6B5:
            return 2;
        default:
            // Compressed and unknown formats. Zero means "do not count on
            // it", not "a pixel takes no space".
            return 0;
    }
}

bool M2W_FormatConvertible(D3DFORMAT eFormat)
{
    return M2W_BytesPerPixel(eFormat) != 0;
}

DWORD M2W_PixelToArgb(D3DFORMAT eFormat, const void* c_pvPixel)
{
    if (!c_pvPixel)
        return 0;

    const unsigned char* p = static_cast<const unsigned char*>(c_pvPixel);

    switch (eFormat)
    {
        case D3DFMT_A8R8G8B8:
            // In little-endian memory: B, G, R, A.
            return PackArgb(p[3], p[2], p[1], p[0]);

        case D3DFMT_X8R8G8B8:
            // `X` = an unused byte - Direct3D read it as fully opaque, not
            // as whatever happened to lie there.
            return PackArgb(0xFF, p[2], p[1], p[0]);

        case D3DFMT_R8G8B8:
            return PackArgb(0xFF, p[2], p[1], p[0]);

        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        {
            const DWORD w = static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8);
            const DWORD a = (eFormat == D3DFMT_A4R4G4B4) ? Expand((w >> 12) & 0xF, 4) : 0xFFu;
            return PackArgb(a,
                            Expand((w >> 8) & 0xF, 4),
                            Expand((w >> 4) & 0xF, 4),
                            Expand(w & 0xF, 4));
        }

        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
        {
            const DWORD w = static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8);
            const DWORD a = (eFormat == D3DFMT_A1R5G5B5) ? Expand((w >> 15) & 0x1, 1) : 0xFFu;
            return PackArgb(a,
                            Expand((w >> 10) & 0x1F, 5),
                            Expand((w >> 5) & 0x1F, 5),
                            Expand(w & 0x1F, 5));
        }

        case D3DFMT_R5G6B5:
        {
            const DWORD w = static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8);
            return PackArgb(0xFF,
                            Expand((w >> 11) & 0x1F, 5),
                            Expand((w >> 5) & 0x3F, 6),
                            Expand(w & 0x1F, 5));
        }

        default:
            return 0;
    }
}

void M2W_ArgbToPixel(D3DFORMAT eFormat, DWORD dwARGB, void* pvPixel)
{
    if (!pvPixel)
        return;

    unsigned char* p = static_cast<unsigned char*>(pvPixel);

    const DWORD a = (dwARGB >> 24) & 0xFF;
    const DWORD r = (dwARGB >> 16) & 0xFF;
    const DWORD g = (dwARGB >> 8) & 0xFF;
    const DWORD b = dwARGB & 0xFF;

    switch (eFormat)
    {
        case D3DFMT_A8R8G8B8:
            p[0] = static_cast<unsigned char>(b);
            p[1] = static_cast<unsigned char>(g);
            p[2] = static_cast<unsigned char>(r);
            p[3] = static_cast<unsigned char>(a);
            break;

        case D3DFMT_X8R8G8B8:
            p[0] = static_cast<unsigned char>(b);
            p[1] = static_cast<unsigned char>(g);
            p[2] = static_cast<unsigned char>(r);
            p[3] = 0xFF;
            break;

        case D3DFMT_R8G8B8:
            p[0] = static_cast<unsigned char>(b);
            p[1] = static_cast<unsigned char>(g);
            p[2] = static_cast<unsigned char>(r);
            break;

        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        {
            const DWORD w = ((eFormat == D3DFMT_A4R4G4B4 ? Truncate(a, 4) : 0xF) << 12) |
                            (Truncate(r, 4) << 8) | (Truncate(g, 4) << 4) | Truncate(b, 4);
            p[0] = static_cast<unsigned char>(w & 0xFF);
            p[1] = static_cast<unsigned char>((w >> 8) & 0xFF);
            break;
        }

        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
        {
            // One-bit alpha: threshold at half. Direct3D cut it the same
            // way - with one bit there is no other sensible answer.
            const DWORD ab = (eFormat == D3DFMT_A1R5G5B5) ? ((a >= 128) ? 1u : 0u) : 1u;
            const DWORD w = (ab << 15) | (Truncate(r, 5) << 10) |
                            (Truncate(g, 5) << 5) | Truncate(b, 5);
            p[0] = static_cast<unsigned char>(w & 0xFF);
            p[1] = static_cast<unsigned char>((w >> 8) & 0xFF);
            break;
        }

        case D3DFMT_R5G6B5:
        {
            const DWORD w = (Truncate(r, 5) << 11) | (Truncate(g, 6) << 5) | Truncate(b, 5);
            p[0] = static_cast<unsigned char>(w & 0xFF);
            p[1] = static_cast<unsigned char>((w >> 8) & 0xFF);
            break;
        }

        default:
            break;
    }
}

bool M2W_ConvertPixelRect(D3DFORMAT eSourceFormat, const void* c_pvSource,
                          int iSourcePitch, int iSourceWidth, int iSourceHeight,
                          D3DFORMAT eTargetFormat, void* pvTarget,
                          int iTargetPitch, int iTargetWidth, int iTargetHeight,
                          bool bLinear)
{
    if (!c_pvSource || !pvTarget)
        return false;

    // The usual reason: a compressed (DXT) source. Decompressing DXT is a
    // separate job, said plainly instead of a black rectangle and a search
    // elsewhere.
    if (!M2W_FormatConvertible(eSourceFormat) || !M2W_FormatConvertible(eTargetFormat))
        return false;

    if (iSourceWidth <= 0 || iSourceHeight <= 0 || iTargetWidth <= 0 || iTargetHeight <= 0)
        return false;

    const UINT uSourceBytes = M2W_BytesPerPixel(eSourceFormat);
    const UINT uTargetBytes = M2W_BytesPerPixel(eTargetFormat);

    const unsigned char* c_pSource = static_cast<const unsigned char*>(c_pvSource);
    unsigned char* pTarget = static_cast<unsigned char*>(pvTarget);

    for (int y = 0; y < iTargetHeight; ++y)
    {
        unsigned char* pRow = pTarget + static_cast<size_t>(y) * iTargetPitch;

        for (int x = 0; x < iTargetWidth; ++x)
        {
            // Position in the source: the centre of the target pixel mapped
            // onto the source grid. Without the half-pixel offset the image
            // drifts half a cell at every reduction - visible after a few
            // mip levels.
            const float fSx = (static_cast<float>(x) + 0.5f) *
                              static_cast<float>(iSourceWidth) /
                              static_cast<float>(iTargetWidth) - 0.5f;
            const float fSy = (static_cast<float>(y) + 0.5f) *
                              static_cast<float>(iSourceHeight) /
                              static_cast<float>(iTargetHeight) - 0.5f;

            const DWORD dwARGB = bLinear
                ? SampleBilinear(eSourceFormat, c_pSource, iSourcePitch, iSourceWidth, iSourceHeight, uSourceBytes, fSx, fSy)
                : SampleNearest(eSourceFormat, c_pSource, iSourcePitch, iSourceWidth, iSourceHeight, uSourceBytes, fSx, fSy);

            M2W_ArgbToPixel(eTargetFormat, dwARGB, pRow + x * uTargetBytes);
        }
    }

    return true;
}
