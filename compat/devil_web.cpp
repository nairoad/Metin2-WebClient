// SPDX-License-Identifier: GPL-2.0-or-later
// devil_web.cpp - DevIL, but only as much as the client really calls: the
// fourteen `il*` functions the guild-mark code uses, over the image reader
// of d3d8_image.cpp and a 32-bit TGA writer.

// Design:
// DevIL is free (LGPL), so building it under emscripten would be allowed -
// and disproportionate: dozens of formats, its own dependencies (libpng,
// libjpeg, libtiff, libmng) pulled into the binary. MEASURED, all five
// call sites concern GUILD MARKS and nothing else: `MarkManager.cpp` (the
// mark set), `MarkImage.cpp` (one mark image, 4x4 blocks),
// `GuildMarkUploader.cpp` (uploading one's own mark),
// `PythonApplicationModule.cpp`, `UserInterface.cpp` (`ilInit` only).
// Fourteen functions, images always BGRA with eight bits per component,
// saving always TGA (`ilSave(IL_TGA, ...)`) - a closed subset that fits in
// one file, half of which d3d8_image.cpp already has because the whole
// interface stands on TGA.
//
// What it cannot do, it says loudly: formats `M2W_LoadImage` does not
// read make `ilLoad` return `IL_FALSE` rather than fake success - and
// `CGuildMarkImage::Load` answers IL_FALSE by building an empty mark, so
// the fallback EXISTS in the game and leads somewhere sensible.
// `ilConvertImage` accepts only `IL_BGRA` + bytes, because that is all the
// client asks for; anything else is IL_FALSE plus a console line, not
// pixels silently handed over in the wrong layout.
//
// The image origin is the one thing easy to get wrong here: the client
// calls `ilEnable(IL_ORIGIN_SET)` and `ilOriginFunc(IL_ORIGIN_UPPER_LEFT)`
// - the first row shall be the TOP one. TGA keeps the image bottom-up by
// default, so without that marks would be upside down and nothing would
// crash. `M2W_LoadImage` already returns top-down; the requested origin
// is kept and CHECKED, so a request for `IL_ORIGIN_LOWER_LEFT` gets a
// console line, not a silently flipped image.

#include <cstdio>
#include "stubs.h"
#include <cstring>
#include <map>
#include <vector>

#include "win32_compat.h"
#include "d3d8_image.h"

#include <IL/il.h>

namespace
{

/// One image in the store. Pixels ALWAYS BGRA, one byte per component,
/// top row first - as `M2W_LoadImage` returns them and the client asks.
struct TImageSlot
{
    int iWidth;
    int iHeight;
    std::vector<unsigned char> vecPixels;

    /// An empty image: 0x0, no pixels.
    TImageSlot() : iWidth(0), iHeight(0) {}
};

std::map<ILuint, TImageSlot> g_kStore;
ILuint g_uNextId = 1;        ///< zero is DevIL's "no image"
ILuint g_uBound = 0;
bool   g_bOriginSet = false;
ILenum g_eOrigin = IL_ORIGIN_UPPER_LEFT;

/// The bound image or NULL. A separate function because every DevIL call
/// starts the same way, and "forgot to check" would mean writing to a
/// non-existent image.
TImageSlot* Bound()
{
    if (!g_uBound)
        return NULL;
    std::map<ILuint, TImageSlot>::iterator it = g_kStore.find(g_uBound);
    return (it == g_kStore.end()) ? NULL : &it->second;
}

/// Whether the request is for the layout this file handles. `IL_BYTE` is
/// accepted together with `IL_UNSIGNED_BYTE`: `GuildMarkUploader.cpp`
/// passes `IL_BYTE` for the same data - a client-side imprecision, not a
/// different memory layout.
bool LayoutKnown(ILenum eFormat, ILenum eType, const char* c_szCaller)
{
    const bool bOk = (eFormat == IL_BGRA) &&
                     (eType == IL_UNSIGNED_BYTE || eType == IL_BYTE);
    if (!bOk)
    {
        std::printf("m2w devil: %s asks for layout 0x%X/0x%X, only "
                    "IL_BGRA + byte is supported - refused\n",
                    c_szCaller, static_cast<unsigned>(eFormat),
                    static_cast<unsigned>(eType));
    }
    return bOk;
}

/// Reads a whole file. An empty vector when the file does not exist - a
/// normal answer, not a failure: `CGuildMarkImage::Load` builds an empty
/// mark then.
std::vector<unsigned char> ReadWholeFile(const char* c_szName)
{
    std::vector<unsigned char> vecData;
    if (!c_szName)
        return vecData;

    FILE* pFile = M2W_Fopen(c_szName, "rb");
    if (!pFile)
        return vecData;

    std::fseek(pFile, 0, SEEK_END);
    const long lLength = std::ftell(pFile);
    std::fseek(pFile, 0, SEEK_SET);

    if (lLength > 0)
    {
        vecData.resize(static_cast<size_t>(lLength));
        if (std::fread(&vecData[0], 1, vecData.size(), pFile) != vecData.size())
            vecData.clear();
    }
    std::fclose(pFile);
    return vecData;
}

/// Writes a 32-bit UNCOMPRESSED TGA with the top-down bit. Bit 5 of the
/// descriptor (0x20) tells the reader the first row is the TOP one;
/// without it a mark saved by the client and read back would be flipped -
/// a bug that shows only after a full write-read round trip, the hardest
/// to connect to its cause.
bool WriteTga(const char* c_szName, const TImageSlot& c_rkImage)
{
    if (!c_szName || c_rkImage.vecPixels.empty())
        return false;

    FILE* pFile = M2W_Fopen(c_szName, "wb");
    if (!pFile)
        return false;

    unsigned char aHeader[18];
    std::memset(aHeader, 0, sizeof(aHeader));
    aHeader[2] = 2;                                     // true colour, no map
    aHeader[12] = static_cast<unsigned char>(c_rkImage.iWidth & 0xFF);
    aHeader[13] = static_cast<unsigned char>((c_rkImage.iWidth >> 8) & 0xFF);
    aHeader[14] = static_cast<unsigned char>(c_rkImage.iHeight & 0xFF);
    aHeader[15] = static_cast<unsigned char>((c_rkImage.iHeight >> 8) & 0xFF);
    aHeader[16] = 32;                                   // bits per pixel
    aHeader[17] = 0x20 | 8;                             // top-down, 8 alpha bits

    std::fwrite(aHeader, 1, sizeof(aHeader), pFile);
    // 32-bit TGA keeps a pixel as B, G, R, A - exactly the store's layout.
    std::fwrite(&c_rkImage.vecPixels[0], 1, c_rkImage.vecPixels.size(), pFile);
    std::fclose(pFile);
    return true;
}

}  // namespace

// ===========================================================================
// Life cycle
// ===========================================================================

/// DevIL: resets the store.
ILvoid ilInit(ILvoid)
{
    g_kStore.clear();
    g_uNextId = 1;
    g_uBound = 0;
    g_bOriginSet = false;
    g_eOrigin = IL_ORIGIN_UPPER_LEFT;
}

/// DevIL: frees every image.
ILvoid ilShutDown(ILvoid)
{
    g_kStore.clear();
    g_uBound = 0;
}

/// DevIL: allocates `Num` image ids.
ILvoid ilGenImages(ILsizei Num, ILuint* Images)
{
    if (!Images)
        return;
    for (ILsizei i = 0; i < Num; ++i)
    {
        Images[i] = g_uNextId++;
        g_kStore[Images[i]] = TImageSlot();
    }
}

/// DevIL: selects the image the following calls act on.
ILvoid ilBindImage(ILuint Image)
{
    g_uBound = Image;
}

/// DevIL: frees images; a bound one is unbound.
ILvoid ilDeleteImages(ILsizei Num, const ILuint* Images)
{
    if (!Images)
        return;
    for (ILsizei i = 0; i < Num; ++i)
    {
        g_kStore.erase(Images[i]);
        if (g_uBound == Images[i])
            g_uBound = 0;
    }
}

// ===========================================================================
// Image origin
// ===========================================================================

/// DevIL: `IL_ORIGIN_SET` and `IL_FILE_OVERWRITE` are honoured; any other
/// mode is acknowledged and reported (a refusal would stop the caller).
ILboolean ilEnable(ILenum Mode)
{
    if (Mode == IL_ORIGIN_SET)
    {
        g_bOriginSet = true;
        return IL_TRUE;
    }

    // FILE OVERWRITE - the client DOES ask for it (`GuildMarkUploader.cpp:38`,
    // `MarkImage.cpp:56`; earlier this comment claimed it did not).
    // IL_TRUE is TRUE here, not convenient: `WriteTga` opens with
    // `M2W_Fopen(..., "wb")`, which always overwrites.
    if (Mode == IL_FILE_OVERWRITE)
        return IL_TRUE;

    M2W_STUB("DevIL::ilEnable (unknown mode)");
    std::printf("m2w devil: ilEnable(0x%X) - unknown mode; acknowledged, "
                "because a refusal would stop the caller\n",
                static_cast<unsigned>(Mode));
    return IL_TRUE;
}

/// DevIL: the reader always returns top-down; any other origin is reported
/// and refused rather than silently flipped.
ILboolean ilOriginFunc(ILenum Mode)
{
    g_eOrigin = Mode;

    if (Mode != IL_ORIGIN_UPPER_LEFT)
    {
        std::printf("m2w devil: origin 0x%X requested, but the reader "
                    "always returns the TOP row first\n",
                    static_cast<unsigned>(Mode));
        return IL_FALSE;
    }
    return IL_TRUE;
}

// ===========================================================================
// Load and save
// ===========================================================================

/// DevIL: loads a file into the bound image. The type argument is
/// `IL_TYPE_UNKNOWN` in all five client calls ("detect yourself");
/// `M2W_LoadImage` detects by content, so it is unnamed.
ILboolean ilLoad(ILenum, const ILstring FileName)
{
    TImageSlot* pImage = Bound();
    if (!pImage)
        return IL_FALSE;

    const std::vector<unsigned char> vecFile = ReadWholeFile(reinterpret_cast<const char*>(FileName));
    if (vecFile.empty())
        return IL_FALSE;

    const TImage kResult = M2W_LoadImage(&vecFile[0], static_cast<UINT>(vecFile.size()));
    if (!kResult.bOk)
    {
        std::printf("m2w devil: unsupported file format '%s' (TGA and BMP are supported)\n",
                    reinterpret_cast<const char*>(FileName));
        return IL_FALSE;
    }

    pImage->iWidth = kResult.iWidth;
    pImage->iHeight = kResult.iHeight;
    pImage->vecPixels = kResult.vecPixels;
    return IL_TRUE;
}

/// DevIL: saves the bound image - TGA only, as both `ilSave` calls in the
/// tree pass `IL_TGA`; another format is refused, not written as TGA under
/// a foreign name.
ILboolean ilSave(ILenum Type, const ILstring FileName)
{
    TImageSlot* pImage = Bound();
    if (!pImage)
        return IL_FALSE;

    if (Type != IL_TGA)
    {
        std::printf("m2w devil: only TGA can be saved, 0x%X requested\n",
                    static_cast<unsigned>(Type));
        return IL_FALSE;
    }

    return WriteTga(reinterpret_cast<const char*>(FileName), *pImage) ? IL_TRUE : IL_FALSE;
}

// ===========================================================================
// Pixels
// ===========================================================================

/// DevIL: (re)allocates the bound image and copies `Data` in when given.
ILboolean ilTexImage(ILuint Width, ILuint Height, ILuint, ILubyte,
                     ILenum Format, ILenum Type, ILvoid* Data)
{
    TImageSlot* pImage = Bound();
    if (!pImage)
        return IL_FALSE;
    if (!LayoutKnown(Format, Type, "ilTexImage"))
        return IL_FALSE;

    pImage->iWidth = static_cast<int>(Width);
    pImage->iHeight = static_cast<int>(Height);
    pImage->vecPixels.assign(static_cast<size_t>(Width) * Height * 4, 0);

    if (Data && !pImage->vecPixels.empty())
        std::memcpy(&pImage->vecPixels[0], Data, pImage->vecPixels.size());

    return IL_TRUE;
}

/// DevIL: writes a rectangle into the bound image, row by row with bounds
/// checked on every pixel - marks are assembled from blocks placed into a
/// larger board, so a rectangle outside the image is not unthinkable, and
/// without the check it would be a write into foreign memory.
ILvoid ilSetPixels(ILint XOff, ILint YOff, ILint, ILuint Width, ILuint Height,
                   ILuint, ILenum Format, ILenum Type, ILvoid* Data)
{
    TImageSlot* pImage = Bound();
    if (!pImage || !Data || pImage->vecPixels.empty())
        return;
    if (!LayoutKnown(Format, Type, "ilSetPixels"))
        return;

    const unsigned char* c_pSource = static_cast<const unsigned char*>(Data);

    for (ILuint y = 0; y < Height; ++y)
    {
        const long lY = static_cast<long>(YOff) + static_cast<long>(y);
        if (lY < 0 || lY >= pImage->iHeight)
            continue;

        for (ILuint x = 0; x < Width; ++x)
        {
            const long lX = static_cast<long>(XOff) + static_cast<long>(x);
            if (lX < 0 || lX >= pImage->iWidth)
                continue;

            const size_t uTarget = (static_cast<size_t>(lY) * pImage->iWidth + static_cast<size_t>(lX)) * 4;
            const size_t uSource = (static_cast<size_t>(y) * Width + x) * 4;
            std::memcpy(&pImage->vecPixels[uTarget], c_pSource + uSource, 4);
        }
    }
}

/// DevIL: copies a rectangle out of the bound image; pixels outside the
/// image come out as ZEROS, not memory garbage. Returns the pixels copied.
ILuint ilCopyPixels(ILuint XOff, ILuint YOff, ILuint, ILuint Width,
                    ILuint Height, ILuint, ILenum Format, ILenum Type,
                    ILvoid* Data)
{
    TImageSlot* pImage = Bound();
    if (!pImage || !Data || pImage->vecPixels.empty())
        return 0;
    if (!LayoutKnown(Format, Type, "ilCopyPixels"))
        return 0;

    unsigned char* pTarget = static_cast<unsigned char*>(Data);
    ILuint uCopied = 0;

    for (ILuint y = 0; y < Height; ++y)
    {
        for (ILuint x = 0; x < Width; ++x)
        {
            const size_t uOut = (static_cast<size_t>(y) * Width + x) * 4;
            const long lX = static_cast<long>(XOff) + static_cast<long>(x);
            const long lY = static_cast<long>(YOff) + static_cast<long>(y);

            if (lX < 0 || lX >= pImage->iWidth ||
                lY < 0 || lY >= pImage->iHeight)
            {
                std::memset(pTarget + uOut, 0, 4);
                continue;
            }

            const size_t uFrom = (static_cast<size_t>(lY) * pImage->iWidth + static_cast<size_t>(lX)) * 4;
            std::memcpy(pTarget + uOut, &pImage->vecPixels[uFrom], 4);
            ++uCopied;
        }
    }
    return uCopied;
}

/// DevIL: the store ALWAYS holds `IL_BGRA` bytes (all four client calls
/// ask for that), so no conversion is needed - only the CHECK that the
/// request is for what we have.
ILboolean ilConvertImage(ILenum DestFormat, ILenum DestType)
{
    if (!Bound())
        return IL_FALSE;
    return LayoutKnown(DestFormat, DestType, "ilConvertImage") ? IL_TRUE : IL_FALSE;
}

// ===========================================================================
// Queries
// ===========================================================================

/// DevIL: properties of the bound image. `IL_IMAGE_BITS_PER_PIXEL` returns
/// the number of BYTES, as DevIL misleadingly does - the client relies on
/// DevIL's behaviour, not on the name.
ILint ilGetInteger(ILenum Mode)
{
    const TImageSlot* c_pImage = Bound();

    switch (Mode)
    {
        case IL_IMAGE_WIDTH:
            return c_pImage ? c_pImage->iWidth : 0;
        case IL_IMAGE_HEIGHT:
            return c_pImage ? c_pImage->iHeight : 0;
        case IL_IMAGE_DEPTH:
            return c_pImage ? 1 : 0;
        case IL_IMAGE_BITS_PER_PIXEL:
        case IL_IMAGE_BYTES_PER_PIXEL:
            return c_pImage ? 4 : 0;
        case IL_IMAGE_FORMAT:
            return IL_BGRA;
        case IL_IMAGE_TYPE:
            return IL_UNSIGNED_BYTE;
        default:
            return 0;
    }
}
