// SPDX-License-Identifier: GPL-2.0-or-later
// d3dx8_textures.cpp - three D3DX texture functions: `D3DXCreateTexture`,
// `D3DXLoadSurfaceFromSurface` (pixel-format conversion between locked
// surfaces, d3d8_pixels.cpp) and `D3DXCreateTextureFromFileInMemoryEx`
// (image file -> texture with colour key and mip levels, d3d8_image.cpp).

// Design:
// The first two are called only from `eterLib/GrpImageTexture.cpp`, both to
// COPY A TEXTURE INTO A SMALLER FORMAT when the card has little memory or
// the client has image halving on. Neither touches OpenGL: one hands the
// job to the device, the other is pure pixel conversion (tested) wrapped in
// surface locking.
//
// What is NOT here and why it matters: DXT decompression.
// `GrpImageTexture.cpp` has a branch for `ms_bSupportDXT == false` that
// copies a compressed texture through `D3DXLoadSurfaceFromSurface` into
// `A4R4G4B4` or `A1R5G5B5`; `M2W_ConvertPixelRect` refuses a compressed
// source and the refusal is passed on and said ONCE in the console. A
// silent no-op would give a texture of zeros - black rectangles instead of
// images, and a hunt in the drawing code. The branch runs only when the
// browser lacks `WEBGL_compressed_texture_s3tc` - practically every
// desktop browser has it, phones vary.
//
// The colour key must not be overlooked: the client passes `0xffff00ff`,
// bright pink, and pixels of that colour become fully transparent - the
// old trick from the days when formats without alpha were cheaper, still
// used in Metin2. Without it icons have pink backgrounds. It is applied
// BEFORE the format conversion: after 16 bits pink would no longer be
// exactly pink.

#include "win32_compat.h"

#include "d3d8_image.h"
#include "d3d8_pixels.h"
#include "d3dx8.h"

#include <cstdio>
#include <cstring>
#include <vector>

/// D3DX: creates a texture through the device; `D3DX_DEFAULT` mip levels
/// means "down to one pixel", as Direct3D counted it.
HRESULT D3DXCreateTexture(IDirect3DDevice8* pDevice, UINT width, UINT height,
                          UINT mipLevels, DWORD usage, D3DFORMAT format,
                          D3DPOOL pool, LPDIRECT3DTEXTURE8* ppTexture)
{
    if (!pDevice || !ppTexture)
        return E_FAIL;

    if (mipLevels == D3DX_DEFAULT || mipLevels == 0)
    {
        mipLevels = 1;
        UINT s = (width > height) ? width : height;
        while (s > 1)
        {
            s /= 2;
            ++mipLevels;
        }
    }

    return pDevice->CreateTexture(width, height, mipLevels, usage, format,
                                  pool, ppTexture);
}

/// D3DX: copies one whole surface into another with format conversion.
/// Partial rectangles are not used by the client (all six calls in
/// `GrpImageTexture.cpp` pass NULL); a compressed source is refused (see
/// the design note).
HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface8* pDestSurface,
                                   CONST void* /*pDestPalette*/,
                                   CONST RECT* /*pDestRect*/,
                                   IDirect3DSurface8* pSrcSurface,
                                   CONST void* /*pSrcPalette*/,
                                   CONST RECT* /*pSrcRect*/,
                                   DWORD filter, D3DCOLOR /*colorKey*/)
{
    if (!pDestSurface || !pSrcSurface)
        return E_FAIL;

    D3DSURFACE_DESC kSource, kTarget;
    if (FAILED(pSrcSurface->GetDesc(&kSource)) ||
        FAILED(pDestSurface->GetDesc(&kTarget)))
        return E_FAIL;

    D3DLOCKED_RECT kS, kT;
    if (FAILED(pSrcSurface->LockRect(&kS, NULL, D3DLOCK_READONLY)))
        return E_FAIL;

    if (FAILED(pDestSurface->LockRect(&kT, NULL, 0)))
    {
        pSrcSurface->UnlockRect();
        return E_FAIL;
    }

    // `D3DX_FILTER_LINEAR` averages four pixels, `D3DX_FILTER_NONE` takes
    // the nearest. The client uses these two; `BOX` and `TRIANGLE` (also
    // averaging filters in D3DX) are treated as linear.
    const bool bLinear = ((filter & 0x7) == D3DX_FILTER_LINEAR ||
                          (filter & 0x7) == D3DX_FILTER_BOX ||
                          (filter & 0x7) == D3DX_FILTER_TRIANGLE);

    const bool bOk = M2W_ConvertPixelRect(
        kSource.Format, kS.pBits, kS.Pitch,
        static_cast<int>(kSource.Width), static_cast<int>(kSource.Height),
        kTarget.Format, kT.pBits, kT.Pitch,
        static_cast<int>(kTarget.Width), static_cast<int>(kTarget.Height),
        bLinear);

    pDestSurface->UnlockRect();
    pSrcSurface->UnlockRect();

    if (!bOk)
    {
        static bool s_bSaid = false;
        if (!s_bSaid)
        {
            std::printf("m2w d3dx: cannot copy a surface from format %d to %d - "
                        "most likely DXT would have to be decompressed "
                        "(see the note in d3dx8_textures.cpp)\n",
                        static_cast<int>(kSource.Format),
                        static_cast<int>(kTarget.Format));
            s_bSaid = true;
        }
        return E_FAIL;
    }

    return D3D_OK;
}

/// D3DX: image file in memory -> texture. Called for everything that is
/// not a DDS (item icons, interface images, guild marks); the file is read
/// by d3d8_image.cpp (TGA, BMP, JPEG, uncompressed DDS - tested), the rest
/// of the D3DX contract is here: colour key, `D3DX_DEFAULT` size and
/// format, every mip level computed from the ORIGINAL image (not from the
/// previous level - rounding errors through eight halvings show on the
/// smallest levels as a hue shift).
HRESULT D3DXCreateTextureFromFileInMemoryEx(
    IDirect3DDevice8* pDevice, LPCVOID pSrcData, UINT srcDataSize,
    UINT width, UINT height, UINT mipLevels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, DWORD filter, DWORD /*mipFilter*/, D3DCOLOR colorKey,
    D3DXIMAGE_INFO* pSrcInfo, void* /*pPalette*/, LPDIRECT3DTEXTURE8* ppTexture)
{
    if (!pDevice || !pSrcData || !ppTexture)
        return E_FAIL;

    *ppTexture = NULL;

    // NOT `const`: the colour key changes pixels IN PLACE. A
    // full copy of the vector was made here for every image (four
    // megabytes for 1024x1024, allocated and copied for nothing) although
    // only the `colorKey != 0` branch changed it; the image is ours and
    // nobody else looks at it.
    TImage kImage = M2W_LoadImage(pSrcData, srcDataSize);
    if (!kImage.bOk)
    {
        // No pretending: `GrpImageTexture.cpp` prints "Cannot create
        // texture" and goes on - better no image than one from random
        // memory. Which format it was: no name here, but the
        // header says more than "cannot" - the first 20 times, with size
        // and bytes.
        static int s_iSaid = 0;
        if (s_iSaid < 20)
        {
            ++s_iSaid;
            const unsigned char* p = static_cast<const unsigned char*>(pSrcData);
            char szHead[13];
            for (int i = 0; i < 12; ++i)
                szHead[i] = (i < (int)srcDataSize && p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
            szHead[12] = 0;
            std::printf("m2w image: cannot decode (%u B, header"
                        " %02x %02x %02x %02x %02x %02x %02x %02x '%s') - TGA, BMP, JPEG are supported\n",
                        (unsigned)srcDataSize,
                        srcDataSize > 0 ? p[0] : 0, srcDataSize > 1 ? p[1] : 0,
                        srcDataSize > 2 ? p[2] : 0, srcDataSize > 3 ? p[3] : 0,
                        srcDataSize > 4 ? p[4] : 0, srcDataSize > 5 ? p[5] : 0,
                        srcDataSize > 6 ? p[6] : 0, srcDataSize > 7 ? p[7] : 0, szHead);
        }
        return E_FAIL;
    }

    // --- colour key, BEFORE the format conversion ----------------------------
    std::vector<unsigned char>& vecPixels = kImage.vecPixels;
    if (colorKey != 0)
    {
        const size_t uPixels = vecPixels.size() / 4;
        for (size_t i = 0; i < uPixels; ++i)
        {
            const DWORD dwARGB = M2W_PixelToArgb(D3DFMT_A8R8G8B8, &vecPixels[i * 4]);
            if (dwARGB == colorKey)
                vecPixels[i * 4 + 3] = 0;   // alpha only
        }
    }

    // --- size and format ------------------------------------------------------
    // `D3DX_DEFAULT` means "take it from the file"; the client passes it
    // for all three.
    const UINT uWidth = (width == D3DX_DEFAULT || width == 0)
                        ? static_cast<UINT>(kImage.iWidth) : width;
    const UINT uHeight = (height == D3DX_DEFAULT || height == 0)
                         ? static_cast<UINT>(kImage.iHeight) : height;
    const D3DFORMAT eFormat = (format == D3DFMT_UNKNOWN) ? D3DFMT_A8R8G8B8 : format;

    if (pSrcInfo)
    {
        std::memset(pSrcInfo, 0, sizeof(*pSrcInfo));
        pSrcInfo->Width = static_cast<UINT>(kImage.iWidth);
        pSrcInfo->Height = static_cast<UINT>(kImage.iHeight);
        pSrcInfo->Depth = 1;
        pSrcInfo->Format = eFormat;
        // MipLevels is filled AFTER the texture exists - the client reads
        // the level count from here and loops over it.
    }

    IDirect3DTexture8* pTexture = NULL;
    const HRESULT hr = D3DXCreateTexture(pDevice, uWidth, uHeight, mipLevels, usage, eFormat, pool, &pTexture);
    if (FAILED(hr) || !pTexture)
        return E_FAIL;

    const DWORD dwLevels = pTexture->GetLevelCount();
    if (pSrcInfo)
        pSrcInfo->MipLevels = dwLevels;

    // --- fill the levels ------------------------------------------------------
    const bool bLinear = ((filter & 0x7) != D3DX_FILTER_NONE &&
                          (filter & 0x7) != D3DX_FILTER_POINT);

    for (DWORD i = 0; i < dwLevels; ++i)
    {
        D3DSURFACE_DESC kDesc;
        if (FAILED(pTexture->GetLevelDesc(i, &kDesc)))
            break;

        D3DLOCKED_RECT kLocked;
        if (FAILED(pTexture->LockRect(i, &kLocked, NULL, 0)))
            break;

        M2W_ConvertPixelRect(
            D3DFMT_A8R8G8B8, &vecPixels[0], kImage.iWidth * 4,
            kImage.iWidth, kImage.iHeight,
            kDesc.Format, kLocked.pBits, kLocked.Pitch,
            static_cast<int>(kDesc.Width), static_cast<int>(kDesc.Height),
            // Level zero at the same size is a plain copy - a linear filter
            // would blur it for nothing.
            (i == 0 && kDesc.Width == static_cast<UINT>(kImage.iWidth)) ? false : bLinear);

        pTexture->UnlockRect(i);
    }

    *ppTexture = pTexture;
    return D3D_OK;
}
