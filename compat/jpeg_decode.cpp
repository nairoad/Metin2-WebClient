// SPDX-License-Identifier: GPL-2.0-or-later
// jpeg_decode.cpp - JPEG decoding on libjpeg (see jpeg_decode.h for why this
// is its own translation unit).

#include "jpeg_decode.h"

#include <csetjmp>
#include <cstddef>
#include <cstdio>

extern "C" {
#include <jpeglib.h>
}

namespace
{

/// libjpeg's default `error_exit` calls `exit(1)`: in the browser one corrupt
/// `.jpg` would kill the whole client without an explanation. This error
/// manager `longjmp`s back instead, and the error becomes `false` - the same
/// answer as "format not supported".
struct TErrorManager
{
    jpeg_error_mgr kStandard;
    std::jmp_buf kJump;
};

/// libjpeg error handler: jumps back to `M2W_DecodeJpeg` (`longjmp`) instead
/// of libjpeg's default `exit()`, so a bad file fails one decode, not the
/// client.
void AbortInsteadOfExit(j_common_ptr pInfo)
{
    std::longjmp(reinterpret_cast<TErrorManager*>(pInfo->err)->kJump, 1);
}

/// Warnings are dropped too: a corrupt file can emit thousands, and the
/// client has one log stream shared with the game.
void Silent(j_common_ptr) {}

}  // namespace

bool M2W_DecodeJpeg(const unsigned char* c_pBytes, unsigned uBytes,
                    int* piWidth, int* piHeight,
                    std::vector<unsigned char>* pkBGRA)
{
    if (!c_pBytes || !uBytes || !piWidth || !piHeight || !pkBGRA)
        return false;

    jpeg_decompress_struct kInfo;
    TErrorManager kError;

    kInfo.err = jpeg_std_error(&kError.kStandard);
    kError.kStandard.error_exit = AbortInsteadOfExit;
    kError.kStandard.output_message = Silent;

    if (setjmp(kError.kJump))
    {
        jpeg_destroy_decompress(&kInfo);
        pkBGRA->clear();
        *piWidth = 0;
        *piHeight = 0;
        return false;
    }

    jpeg_create_decompress(&kInfo);
    jpeg_mem_src(&kInfo, const_cast<unsigned char*>(c_pBytes),
                 static_cast<unsigned long>(uBytes));

    if (jpeg_read_header(&kInfo, TRUE) != JPEG_HEADER_OK)
    {
        jpeg_destroy_decompress(&kInfo);
        return false;
    }

    // Ask for RGB whatever the file holds. libjpeg converts YCbCr, greyscale
    // and CMYK itself, so there is one copy loop instead of four variants of
    // which three would never be exercised.
    kInfo.out_color_space = JCS_RGB;

    if (!jpeg_start_decompress(&kInfo))
    {
        jpeg_destroy_decompress(&kInfo);
        return false;
    }

    const int iWidth = static_cast<int>(kInfo.output_width);
    const int iHeight = static_cast<int>(kInfo.output_height);

    if (iWidth <= 0 || iHeight <= 0 || kInfo.output_components != 3)
    {
        jpeg_finish_decompress(&kInfo);
        jpeg_destroy_decompress(&kInfo);
        return false;
    }

    pkBGRA->assign(static_cast<size_t>(iWidth) * iHeight * 4, 0);

    std::vector<unsigned char> kRow(static_cast<size_t>(iWidth) * 3);
    unsigned char* pRow = &kRow[0];

    while (kInfo.output_scanline < kInfo.output_height)
    {
        const int y = static_cast<int>(kInfo.output_scanline);
        jpeg_read_scanlines(&kInfo, &pRow, 1);

        unsigned char* pTarget = &(*pkBGRA)[static_cast<size_t>(y) * iWidth * 4];
        for (int x = 0; x < iWidth; ++x)
        {
            // libjpeg gives R, G, B - the texture wants B, G, R, A. The
            // easiest mistake in this file and the hardest to notice: the
            // picture looks fine, only the sky is red.
            pTarget[x * 4 + 0] = pRow[x * 3 + 2];
            pTarget[x * 4 + 1] = pRow[x * 3 + 1];
            pTarget[x * 4 + 2] = pRow[x * 3 + 0];
            pTarget[x * 4 + 3] = 0xFF;    // JPEG has no transparency
        }
    }

    jpeg_finish_decompress(&kInfo);
    jpeg_destroy_decompress(&kInfo);

    // JPEG rows run top-down like the texture - no flip, the only one of the
    // three formats in this reader that needs none.
    *piWidth = iWidth;
    *piHeight = iHeight;
    return true;
}
