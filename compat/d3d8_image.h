// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_image.h - decoding an image file from memory (TGA, BMP, JPEG,
// uncompressed DDS) into `D3DFMT_A8R8G8B8` pixels, for
// `D3DXCreateTextureFromFileInMemoryEx` and the DevIL layer.

// Design:
// `D3DXCreateTextureFromFileInMemoryEx` turns the bytes of an image file
// into a texture; the client calls it for everything that is not a DDS -
// item icons, interface images, guild marks. The port binary is full of
// `.tga` paths (`icon/item/%05d.tga` for EVERY item icon, `mark/%s_%lu.tga`,
// `d:/ymir work/ui/game/client_origin.tga`); `.jpg` appears four times and
// `.png` never. TGA is the format the whole interface stands on - and the
// one the browser cannot decode (`createImageBitmap` knows PNG, JPEG, WebP,
// GIF, BMP). Handing it to the browser would be asynchronous, while this
// call must return a finished texture, and would not work for the format
// there is most of. So the reader is here, in C++, and returns at once.
// (The port binary contains the string `stb_image`, but next to `EXR`,
// `WebP`, `AVIF`, `BaseExceptionGroup` - Pillow messages from the embedded
// CPython, not the game's texture reader.)
//
// The trap with a second bottom: TGA keeps the image BOTTOM-UP by default
// (the first row of the file is the bottom row) and a Direct3D texture
// top-down; bit 5 of the image descriptor says the file is top-down. A
// program that ignores it gets icons upside down and nothing crashes.
// Every reader here returns rows top-down.

#pragma once

#include "win32_compat.h"
#include "d3d8.h"

#include <vector>

/// What was read.
struct TImage
{
    int iWidth;
    int iHeight;

    /// Pixels always in `D3DFMT_A8R8G8B8`, i.e. B, G, R, A in memory. One
    /// output format instead of several: the caller converts further anyway
    /// (d3d8_pixels.cpp), and every extra variant is a path nobody tests.
    std::vector<unsigned char> vecPixels;

    /// Whether the file could be read. `false` means "unknown format" or
    /// "corrupt file" - not "empty image".
    bool bOk;
};

/// Decodes by content, not by file name - there is no name here.
TImage M2W_LoadImage(const void* c_pvData, UINT uBytes);
