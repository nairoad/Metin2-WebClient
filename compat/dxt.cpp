// SPDX-License-Identifier: GPL-2.0-or-later
// dxt.cpp - DXT1 / DXT3 / DXT5 block decoding (see dxt.h for the design).

#include "dxt.h"

#include <cstddef>

namespace
{

/// R5G6B5 -> 0x00RRGGBB. Bits are replicated, not shifted, so that the
/// largest 5-bit value (31) gives 255 rather than 248 - white stays white.
uint32_t Expand565(uint16_t c)
{
    const uint32_t r = (c >> 11) & 0x1F;
    const uint32_t g = (c >>  5) & 0x3F;
    const uint32_t b = (c >>  0) & 0x1F;

    const uint32_t r8 = (r << 3) | (r >> 2);
    const uint32_t g8 = (g << 2) | (g >> 4);
    const uint32_t b8 = (b << 3) | (b >> 2);

    return (r8 << 16) | (g8 << 8) | b8;
}

/// 0x00RRGGBB + alpha -> 0xAARRGGBB.
uint32_t MakePixel(uint32_t rgb, uint32_t a)
{
    return (a << 24) | (rgb & 0x00FFFFFFu);
}

/// Decodes the 8-byte colour block shared by DXT1/3/5 into four 0x00RRGGBB
/// colours. `bDxt1` enables the three-colour mode (`c0 <= c1`); DXT3/DXT5
/// are always four-colour. Returns true when index 3 means transparent.
bool DecodeColourBlock(const uint8_t* c_pBlock, uint32_t aRgb[4], bool bDxt1)
{
    const uint16_t c0raw = static_cast<uint16_t>(c_pBlock[0]) |
                           (static_cast<uint16_t>(c_pBlock[1]) << 8);
    const uint16_t c1raw = static_cast<uint16_t>(c_pBlock[2]) |
                           (static_cast<uint16_t>(c_pBlock[3]) << 8);

    const uint32_t c0 = Expand565(c0raw);
    const uint32_t c1 = Expand565(c1raw);

    aRgb[0] = c0;
    aRgb[1] = c1;
    aRgb[2] = 0;
    aRgb[3] = 0;

    if (!bDxt1 || c0raw > c1raw)
    {
        // Four colours: c2 = (2*c0+c1)/3, c3 = (c0+2*c1)/3, per channel
        // after the expansion to 8 bits.
        for (int ch = 0; ch < 3; ++ch)
        {
            const int shift = ch * 8;
            const uint32_t v0 = (c0 >> shift) & 0xFF;
            const uint32_t v1 = (c1 >> shift) & 0xFF;
            aRgb[2] |= ((2 * v0 + v1) / 3) << shift;
            aRgb[3] |= ((v0 + 2 * v1) / 3) << shift;
        }
        return false;
    }

    // Three colours (DXT1, c0 <= c1): c2 = (c0+c1)/2 per channel, c3 is
    // fully transparent and handled by the caller. Mutation-tested: `>=`
    // here survives random data and dies only on a `c0 == c1` sample
    for (int ch = 0; ch < 3; ++ch)
    {
        const int shift = ch * 8;
        const uint32_t v0 = (c0 >> shift) & 0xFF;
        const uint32_t v1 = (c1 >> shift) & 0xFF;
        aRgb[2] |= ((v0 + v1) / 2) << shift;
    }
    return true;
}

/// Writes the 16 pixels of one block into the image. `c_pIndices` are the
/// four index bytes of the colour block (bits 0-1 = leftmost pixel of the
/// row); `bTransparent3` makes index 3 a fully transparent 0x00000000.
void WriteBlock(const uint32_t aRgb[4], const uint8_t aAlpha[16],
                const uint8_t* c_pIndices, bool bTransparent3,
                uint32_t* pTarget, int iBlockX, int iBlockY, int iWidth)
{
    for (int row = 0; row < 4; ++row)
    {
        const uint8_t rowByte = c_pIndices[row];
        for (int col = 0; col < 4; ++col)
        {
            const int idx = (rowByte >> (col * 2)) & 0x3;
            const int pixel = (iBlockY * 4 + row) * iWidth + (iBlockX * 4 + col);

            if (bTransparent3 && idx == 3)
                pTarget[pixel] = 0x00000000u;
            else
                pTarget[pixel] = MakePixel(aRgb[idx], aAlpha[row * 4 + col]);
        }
    }
}

/// DXT1: one 8-byte block.
void DecodeDxt1Block(const uint8_t* c_pBlock, uint32_t* pTarget,
                     int iBlockX, int iBlockY, int iWidth)
{
    uint32_t aRgb[4] = {};
    const bool bTransparent3 = DecodeColourBlock(c_pBlock, aRgb, true);

    uint8_t aAlpha[16];
    for (int i = 0; i < 16; ++i)
        aAlpha[i] = 255;

    WriteBlock(aRgb, aAlpha, c_pBlock + 4, bTransparent3, pTarget, iBlockX, iBlockY, iWidth);
}

/// DXT3: one 16-byte block.
void DecodeDxt3Block(const uint8_t* c_pBlock, uint32_t* pTarget,
                     int iBlockX, int iBlockY, int iWidth)
{
    // Bytes 0-7: 4-bit alpha per pixel, low nibble = left pixel. Expanded by
    // replication, `(a << 4) | a`, so 0xF becomes 255, not 240.
    uint8_t aAlpha[16];
    for (int i = 0; i < 8; ++i)
    {
        const uint8_t b = c_pBlock[i];
        const uint8_t a0 = b & 0x0F;
        const uint8_t a1 = (b >> 4) & 0x0F;
        aAlpha[i * 2 + 0] = (a0 << 4) | a0;
        aAlpha[i * 2 + 1] = (a1 << 4) | a1;
    }

    // Bytes 8-15: colour block, always four colours.
    uint32_t aRgb[4] = {};
    DecodeColourBlock(c_pBlock + 8, aRgb, false);

    WriteBlock(aRgb, aAlpha, c_pBlock + 8 + 4, false, pTarget, iBlockX, iBlockY, iWidth);
}

/// DXT5: one 16-byte block.
void DecodeDxt5Block(const uint8_t* c_pBlock, uint32_t* pTarget,
                     int iBlockX, int iBlockY, int iWidth)
{
    const uint8_t a0 = c_pBlock[0];
    const uint8_t a1 = c_pBlock[1];

    // The eight alpha values: two endpoints, six interpolated (or four
    // interpolated plus 0 and 255 when a0 <= a1).
    uint8_t aAlphaTable[8];
    aAlphaTable[0] = a0;
    aAlphaTable[1] = a1;

    if (a0 > a1)
    {
        for (int i = 2; i <= 7; ++i)
            aAlphaTable[i] = static_cast<uint8_t>(((8 - i) * (int)a0 + (i - 1) * (int)a1) / 7);
    }
    else
    {
        for (int i = 2; i <= 5; ++i)
            aAlphaTable[i] = static_cast<uint8_t>(((6 - i) * (int)a0 + (i - 1) * (int)a1) / 5);
        aAlphaTable[6] = 0;
        aAlphaTable[7] = 255;
    }

    // Bytes 2-4 index pixels 0-7, bytes 5-7 pixels 8-15: 3-bit indices,
    // tightly packed, LSB first. Each three bytes are glued into a 24-bit
    // number (lowest byte at bit 0) and the triples pulled out at j*3.
    // This is where an off-by-one-bit slip shows as stripes in the
    // transparency, not as a crash.
    uint8_t aAlpha[16];
    for (int half = 0; half < 2; ++half)
    {
        const uint8_t* c_pSrc = c_pBlock + 2 + half * 3;
        const uint32_t bits = (uint32_t)c_pSrc[0]
                            | ((uint32_t)c_pSrc[1] << 8)
                            | ((uint32_t)c_pSrc[2] << 16);
        for (int j = 0; j < 8; ++j)
            aAlpha[half * 8 + j] = aAlphaTable[(bits >> (j * 3)) & 0x7];
    }

    // Bytes 8-15: colour block, always four colours.
    uint32_t aRgb[4] = {};
    DecodeColourBlock(c_pBlock + 8, aRgb, false);

    WriteBlock(aRgb, aAlpha, c_pBlock + 8 + 4, false, pTarget, iBlockX, iBlockY, iWidth);
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

unsigned int M2W_DxtByteCount(EM2wDxtFormat eFormat, int iWidth, int iHeight)
{
    if (iWidth <= 0 || iHeight <= 0)
        return 0;
    if ((iWidth % 4) != 0 || (iHeight % 4) != 0)
        return 0;

    unsigned int uBytesPerBlock;
    switch (eFormat)
    {
        case M2W_DXT1: uBytesPerBlock =  8; break;
        case M2W_DXT3: uBytesPerBlock = 16; break;
        case M2W_DXT5: uBytesPerBlock = 16; break;
        default: return 0;
    }

    const int iBlocksX = iWidth / 4;
    const int iBlocksY = iHeight / 4;
    return static_cast<unsigned int>(iBlocksX * iBlocksY) * uBytesPerBlock;
}

bool M2W_DecodeDxt(EM2wDxtFormat eFormat, const void* c_pvBlocks,
                   unsigned int uBytes, int iWidth, int iHeight,
                   uint32_t* pauTarget)
{
    if (c_pvBlocks == nullptr || pauTarget == nullptr)
        return false;
    if (iWidth <= 0 || iHeight <= 0)
        return false;
    if ((iWidth % 4) != 0 || (iHeight % 4) != 0)
        return false;

    const unsigned int uRequired = M2W_DxtByteCount(eFormat, iWidth, iHeight);
    if (uRequired == 0)
        return false;   // unknown format
    if (uBytes < uRequired)
        return false;   // buffer too small - reading past it would be a crash

    const auto* p = static_cast<const uint8_t*>(c_pvBlocks);
    const int iBlocksX = iWidth / 4;
    const int iBlocksY = iHeight / 4;

    for (int by = 0; by < iBlocksY; ++by)
        for (int bx = 0; bx < iBlocksX; ++bx)
        {
            switch (eFormat)
            {
                case M2W_DXT1:
                    DecodeDxt1Block(p, pauTarget, bx, by, iWidth);
                    p += 8;
                    break;
                case M2W_DXT3:
                    DecodeDxt3Block(p, pauTarget, bx, by, iWidth);
                    p += 16;
                    break;
                case M2W_DXT5:
                    DecodeDxt5Block(p, pauTarget, bx, by, iWidth);
                    p += 16;
                    break;
                default:
                    return false;
            }
        }

    return true;
}
