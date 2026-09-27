// SPDX-License-Identifier: GPL-2.0-or-later
// jpeg_decode.h - JPEG decoding on libjpeg, in a translation unit kept
// SEPARATE from the Win32 compatibility headers.

// Design:
// TGA and BMP in d3d8_image.cpp are read by hand because they are byte
// layouts. JPEG is an algorithm - DCT, quantisation, Huffman, chroma
// subsampling - so it comes from Emscripten's libjpeg port
// (`-sUSE_LIBJPEG=1`) instead of a home-made decoder. The client loads
// `.jpg` rarely but visibly: `ymir work/special/spheremap.jpg` is the
// reflection map, and without it the shiny `.sub` items had nothing to
// reflect and syserr said only "CreateFromMemoryFile: Cannot create texture"
//
// Why a separate file: `jpeglib.h` and our Win32 layer cannot share one
// translation unit -
//     jmorecfg.h:309: typedef int boolean;
//     compat/rpcndr.h:30: typedef unsigned char boolean;
//     error: typedef redefinition with different types
// Both are right. Defining `HAVE_BOOLEAN` to silence it would be WORSE than
// the error: `jpeg_decompress_struct` has a dozen fields of that type, so a
// library compiled with `int` and our code compiled with `unsigned char`
// would see the same struct differently, with no message - the image would
// just come out random. Hence this header pulls in no Windows type at all,
// only `int` and `std::vector`.

#pragma once

#include <vector>

/// Decodes a JPEG from memory to `D3DFMT_A8R8G8B8` (B, G, R, A in memory).
///
/// Returns `false` when the file cannot be read, including when it is
/// corrupt. libjpeg by default would end the whole program there; here only
/// this function ends.
bool M2W_DecodeJpeg(const unsigned char* c_pBytes, unsigned uBytes,
                    int* piWidth, int* piHeight,
                    std::vector<unsigned char>* pkBGRA);
