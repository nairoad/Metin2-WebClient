// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_image.cpp - TGA, BMP, JPEG and uncompressed DDS readers (see
// d3d8_image.h for why they are here). Every reader returns B, G, R, A rows
// top-down; a truncated file is refused rather than padded with zeros - a
// half-black icon looks like a drawing bug, not like a corrupt file.

#include "d3d8_image.h"
#include "jpeg_decode.h"

#include <cstring>

namespace
{

/// Little-endian 16-bit value at `p`.
inline unsigned int Read16(const unsigned char* p)
{
    // Both formats store numbers LEAST significant byte first - a property
    // of the FILE, not of the machine. Assembled by hand so the reader
    // behaves the same wherever it runs.
    return static_cast<unsigned int>(p[0]) |
          (static_cast<unsigned int>(p[1]) << 8);
}

/// Little-endian 32-bit value at `p`.
inline unsigned int Read32(const unsigned char* p)
{
    return static_cast<unsigned int>(p[0]) |
          (static_cast<unsigned int>(p[1]) << 8) |
          (static_cast<unsigned int>(p[2]) << 16) |
          (static_cast<unsigned int>(p[3]) << 24);
}

/// Writes one pixel in `D3DFMT_A8R8G8B8` order: B, G, R, A.
inline void Store(unsigned char* pTarget, unsigned char b, unsigned char g,
                  unsigned char r, unsigned char a)
{
    pTarget[0] = b;
    pTarget[1] = g;
    pTarget[2] = r;
    pTarget[3] = a;
}

/// Reverses the row order in place.
void FlipRows(std::vector<unsigned char>& rPixels, int iWidth, int iHeight)
{
    const size_t uRow = static_cast<size_t>(iWidth) * 4;
    std::vector<unsigned char> kBuffer(uRow);

    for (int y = 0; y < iHeight / 2; ++y)
    {
        unsigned char* pTop = &rPixels[static_cast<size_t>(y) * uRow];
        unsigned char* pBottom = &rPixels[static_cast<size_t>(iHeight - 1 - y) * uRow];
        std::memcpy(&kBuffer[0], pTop, uRow);
        std::memcpy(pTop, pBottom, uRow);
        std::memcpy(pBottom, &kBuffer[0], uRow);
    }
}

/// A 0x0 image with `bOk` false - what every reader returns on failure.
TImage EmptyImage()
{
    TImage o;
    o.iWidth = 0;
    o.iHeight = 0;
    o.bOk = false;
    return o;
}

// ===========================================================================
// TGA
// ===========================================================================
// 18-byte header:
//   0  length of the id field (skipped)
//   1  colour-map type (0 = none)
//   2  IMAGE TYPE: 2 = true colour, 3 = greyscale, 10 = true colour RLE,
//                  11 = greyscale RLE
//   3..7   colour-map description
//   8..11  origin (skipped)
//   12..13 width
//   14..15 height
//   16     bits per pixel
//   17     image descriptor; bit 5 = top-down
//
// Colour maps (types 1 and 9) are not supported: Metin2 does not use them,
// and pretending to would give images in random colours.

/// Stores one TGA pixel read from `c_pSource` (1 byte grey, 3 bytes B G R or
/// 4 bytes B G R A) as B, G, R, A at `pDest`; alpha 255 when the file has none.
void StoreTgaPixel(unsigned char* pDest, const unsigned char* c_pSource,
                   unsigned int uBytesPerPixel)
{
    if (uBytesPerPixel == 1)
        Store(pDest, c_pSource[0], c_pSource[0], c_pSource[0], 0xFF);
    else
        Store(pDest, c_pSource[0], c_pSource[1], c_pSource[2],
              (uBytesPerPixel == 4) ? c_pSource[3] : 0xFF);
}

/// Decodes the pixel data of a TGA from `pData` (up to `pEnd`) into
/// `rvecPixels` in file order; RLE when `bRle`. Returns how many of the
/// `uPixelsTotal` pixels were written - fewer when the file ends early.
size_t DecodeTgaPixels(const unsigned char* pData, const unsigned char* pEnd,
                       unsigned int uBytesPerPixel, bool bRle,
                       std::vector<unsigned char>& rvecPixels, size_t uPixelsTotal)
{
    size_t uWritten = 0;
    while (uWritten < uPixelsTotal)
    {
        if (bRle)
        {
            if (pData >= pEnd)
                break;

            const unsigned int uPacket = *pData++;
            // High bit: 1 = one pixel repeated, 0 = a run of distinct
            // pixels. The low seven bits are the COUNT MINUS ONE - 0 means
            // one pixel, not zero.
            const unsigned int uCount = (uPacket & 0x7F) + 1;

            if (uPacket & 0x80)
            {
                if (pData + uBytesPerPixel > pEnd)
                    break;

                for (unsigned int i = 0; i < uCount && uWritten < uPixelsTotal; ++i)
                    StoreTgaPixel(&rvecPixels[uWritten++ * 4], pData, uBytesPerPixel);
                pData += uBytesPerPixel;
            }
            else
            {
                for (unsigned int i = 0; i < uCount && uWritten < uPixelsTotal; ++i)
                {
                    if (pData + uBytesPerPixel > pEnd)
                        break;
                    StoreTgaPixel(&rvecPixels[uWritten++ * 4], pData, uBytesPerPixel);
                    pData += uBytesPerPixel;
                }
            }
        }
        else
        {
            if (pData + uBytesPerPixel > pEnd)
                break;
            StoreTgaPixel(&rvecPixels[uWritten++ * 4], pData, uBytesPerPixel);
            pData += uBytesPerPixel;
        }
    }
    return uWritten;
}

/// Decodes TGA types 2, 3, 10 and 11 (true colour / greyscale, raw or RLE;
/// 8, 24 or 32 bits) into top-down B, G, R, A; `bOk` false for colour maps, other types or a
/// truncated file.
TImage ReadTga(const unsigned char* p, UINT uBytes)
{
    TImage o = EmptyImage();

    if (uBytes < 18)
        return o;

    const unsigned int uIdLength = p[0];
    const unsigned int uColourMapType = p[1];
    const unsigned int uType = p[2];
    const int iWidth = static_cast<int>(Read16(p + 12));
    const int iHeight = static_cast<int>(Read16(p + 14));
    const unsigned int uBits = p[16];
    const unsigned int uDescriptor = p[17];

    if (uColourMapType != 0)
        return o;   // colour map - see above

    if (uType != 2 && uType != 3 && uType != 10 && uType != 11)
        return o;

    if (iWidth <= 0 || iHeight <= 0)
        return o;

    if (uBits != 8 && uBits != 24 && uBits != 32)
        return o;

    const unsigned int uBytesPerPixel = uBits / 8;
    const unsigned char* pData = p + 18 + uIdLength;
    const unsigned char* pEnd = p + uBytes;

    if (pData > pEnd)
        return o;

    o.iWidth = iWidth;
    o.iHeight = iHeight;
    o.vecPixels.assign(static_cast<size_t>(iWidth) * iHeight * 4, 0);

    const size_t uPixelsTotal = static_cast<size_t>(iWidth) * iHeight;
    const bool bRle = (uType == 10 || uType == 11);
    const size_t uWritten = DecodeTgaPixels(pData, pEnd, uBytesPerPixel, bRle,
                                            o.vecPixels, uPixelsTotal);

    if (uWritten != uPixelsTotal)
    {
        o.bOk = false;   // the file ended half-way (see the file header)
        return o;
    }

    // Descriptor bit 5: top-down. Without it - the default - the file is
    // bottom-up and has to be flipped.
    if (!(uDescriptor & 0x20))
        FlipRows(o.vecPixels, iWidth, iHeight);

    o.bOk = true;
    return o;
}

// ===========================================================================
// BMP
// ===========================================================================
// Uncompressed only, 24 or 32 bits. Metin2 uses BMP rarely, but the reader
// is cheap: the pixel order is the same as in TGA - B, G, R - and it is
// bottom-up too.

/// Decodes an uncompressed 24- or 32-bit BMP (either row order) into
/// top-down B, G, R, A; `bOk` false otherwise.
TImage ReadBmp(const unsigned char* p, UINT uBytes)
{
    TImage o = EmptyImage();

    if (uBytes < 54)
        return o;

    const unsigned int uDataOffset = Read32(p + 10);
    const unsigned int uHeaderSize = Read32(p + 14);
    if (uHeaderSize < 40)
        return o;

    const int iWidth = static_cast<int>(Read32(p + 18));
    int iHeight = static_cast<int>(Read32(p + 22));
    const unsigned int uBits = Read16(p + 28);
    const unsigned int uCompression = Read32(p + 30);

    if (uCompression != 0)
        return o;   // RLE and others - not pretended

    if (uBits != 24 && uBits != 32)
        return o;

    // A negative height in BMP means top-down.
    bool bTopDown = false;
    if (iHeight < 0)
    {
        iHeight = -iHeight;
        bTopDown = true;
    }

    if (iWidth <= 0 || iHeight <= 0 || uDataOffset >= uBytes)
        return o;

    const unsigned int uBytesPerPixel = uBits / 8;
    // BMP rows are padded to a multiple of four bytes.
    const size_t uFileRow = ((static_cast<size_t>(iWidth) * uBytesPerPixel + 3) / 4) * 4;

    if (uDataOffset + uFileRow * iHeight > uBytes)
        return o;

    o.iWidth = iWidth;
    o.iHeight = iHeight;
    o.vecPixels.assign(static_cast<size_t>(iWidth) * iHeight * 4, 0);

    for (int y = 0; y < iHeight; ++y)
    {
        const unsigned char* pRow = p + uDataOffset + static_cast<size_t>(y) * uFileRow;
        unsigned char* pTarget = &o.vecPixels[static_cast<size_t>(y) * iWidth * 4];

        for (int x = 0; x < iWidth; ++x)
            Store(pTarget + x * 4, pRow[x * uBytesPerPixel + 0],
                  pRow[x * uBytesPerPixel + 1], pRow[x * uBytesPerPixel + 2],
                  (uBytesPerPixel == 4) ? pRow[x * uBytesPerPixel + 3] : 0xFF);
    }

    if (!bTopDown)
        FlipRows(o.vecPixels, iWidth, iHeight);

    o.bOk = true;
    return o;
}

// ===========================================================================
// JPEG - the decoder is jpeg_decode.cpp (libjpeg); only the copy into TImage
// is here.
// ===========================================================================

/// Decodes a JPEG through `M2W_DecodeJpeg` (libjpeg) into B, G, R, A.
TImage ReadJpeg(const unsigned char* p, UINT uBytes)
{
    TImage o = EmptyImage();
    o.bOk = M2W_DecodeJpeg(p, static_cast<unsigned>(uBytes), &o.iWidth, &o.iHeight, &o.vecPixels);
    return o;
}

// ===========================================================================
// UNCOMPRESSED DDS
// ===========================================================================
// The client recognises DDS itself (`CDXTCImage::LoadHeaderFromMemory`),
// but only COMPRESSED (DXT) ones; uncompressed ones go to
// `D3DXCreateTexture...`, which in real D3DX read everything. Measured in
// the corpus: 461 `A1R5G5B5` files (16 bpp) - MONSTER TEXTURES
// (`skipia_bowman01.dds`, 512x512) - plus a few 24/32-bit ones (moon,
// shadow). Without this reader monsters walked untextured ("Cannot create
// texture" x18 in syserr). Level 0 is read; the colour masks come from the
// header and are converted GENERALLY (shift and width of each mask), so
// 16/24/32 bits with any RGB(A) layout take the same road. DDS is top-down.

/// The channel selected by `uMask`, scaled to 0-255 (rounded); 255 when the
/// mask is empty (no such channel - e.g. no alpha).
unsigned int ComponentFromMask(unsigned int uPixel, unsigned int uMask)
{
    if (!uMask)
        return 0xFF;
    int iShift = 0;
    while (!((uMask >> iShift) & 1u))
        ++iShift;
    int iBits = 0;
    while (iBits + iShift < 32 && ((uMask >> (iShift + iBits)) & 1u))
        ++iBits;
    const unsigned int uRaw = (uPixel & uMask) >> iShift;
    if (iBits >= 8)
        return uRaw >> (iBits - 8);
    // Exactly: a full mask -> 255. For 1 bit 0 or 255, for 5 bits 0..255
    // evenly. (The first version did `u |= u >> bits` once, which for 1 bit
    // gave 192 - monsters three-quarters visible.)
    const unsigned int uFull = (1u << iBits) - 1u;
    return (uRaw * 255u + uFull / 2) / uFull;
}

/// Decodes an UNCOMPRESSED DDS (RGB flag, 16/24/32 bits, any masks, up to
/// 8192x8192) into B, G, R, A; compressed (FOURCC) files are refused.
TImage ReadDds(const unsigned char* p, UINT uBytes)
{
    TImage o = EmptyImage();

    if (uBytes < 128 || Read32(p + 4) != 124)
        return o;
    const int iHeight = static_cast<int>(Read32(p + 12));
    const int iWidth = static_cast<int>(Read32(p + 16));
    const unsigned int uPfFlags = Read32(p + 80);
    const unsigned int uBits = Read32(p + 88);
    const unsigned int uMaskR = Read32(p + 92);
    const unsigned int uMaskG = Read32(p + 96);
    const unsigned int uMaskB = Read32(p + 100);
    const unsigned int uMaskA = Read32(p + 104);

    // 0x4 = FOURCC (DXT and other compressed) - not here; 0x40 = RGB.
    if ((uPfFlags & 0x4) || !(uPfFlags & 0x40))
        return o;
    if (uBits != 16 && uBits != 24 && uBits != 32)
        return o;
    if (iWidth <= 0 || iHeight <= 0 || iWidth > 8192 || iHeight > 8192)
        return o;

    const unsigned int uBytesPerPixel = uBits / 8;
    const size_t uNeeded = 128 + static_cast<size_t>(iWidth) * iHeight * uBytesPerPixel;
    if (uNeeded > uBytes)
        return o;

    o.iWidth = iWidth;
    o.iHeight = iHeight;
    o.vecPixels.assign(static_cast<size_t>(iWidth) * iHeight * 4, 0);

    const bool bAlpha = (uPfFlags & 0x1) != 0 && uMaskA != 0;
    const unsigned char* pData = p + 128;
    for (size_t i = 0; i < static_cast<size_t>(iWidth) * iHeight; ++i)
    {
        const unsigned char* q = pData + i * uBytesPerPixel;
        unsigned int uPixel = q[0] | (q[1] << 8);
        if (uBytesPerPixel >= 3) uPixel |= q[2] << 16;
        if (uBytesPerPixel >= 4) uPixel |= static_cast<unsigned int>(q[3]) << 24;
        Store(&o.vecPixels[i * 4],
              static_cast<unsigned char>(ComponentFromMask(uPixel, uMaskB)),
              static_cast<unsigned char>(ComponentFromMask(uPixel, uMaskG)),
              static_cast<unsigned char>(ComponentFromMask(uPixel, uMaskR)),
              bAlpha ? static_cast<unsigned char>(ComponentFromMask(uPixel, uMaskA)) : 0xFF);
    }
    o.bOk = true;
    return o;
}

}  // namespace

TImage M2W_LoadImage(const void* c_pvData, UINT uBytes)
{
    TImage o = EmptyImage();

    if (!c_pvData || uBytes < 18)
        return o;

    const unsigned char* p = static_cast<const unsigned char*>(c_pvData);

    // BMP starts with "BM" - the only one of these formats with a signature
    // at the front. TGA has NONE, a property of the format, not an
    // oversight: it is recognised by its header fields making sense.
    if (p[0] == 'B' && p[1] == 'M')
        return ReadBmp(p, uBytes);

    // DDS - "DDS " (with the space). Uncompressed only; DXT the client
    // takes itself before getting here.
    if (p[0] == 'D' && p[1] == 'D' && p[2] == 'S' && p[3] == ' ')
        return ReadDds(p, uBytes);

    // JPEG starts with the SOI marker (`FF D8`) followed by another marker,
    // hence the third `FF`. Two bytes would do in practice, but the third
    // costs nothing, and TGA starts with arbitrary bytes - the more
    // certainty before it, the better.
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        return ReadJpeg(p, uBytes);

    return ReadTga(p, uBytes);
}
