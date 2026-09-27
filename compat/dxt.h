// SPDX-License-Identifier: GPL-2.0-or-later
// dxt.h - decoding of DXT1 / DXT3 / DXT5 compressed textures to 0xAARRGGBB
// pixels, for the texture upload path when the GPU has no S3TC
// (`CGlTexture::UploadDecodedDxt` in gl_textures.cpp).

// Design:
// The browser's WebGL has no guaranteed S3TC support, so compressed
// textures from the game data are decoded on the CPU. Decoding is exact,
// not approximate: audited against Pillow on 259 120 pixels bit for bit,
// with mutation tests killing every deliberately broken copy.
// The lesson recorded there: the three-colour mode condition (`c0 <= c1`)
// survives random data because equal colours practically never occur in
// noise - a test needs a sample that forces it.
//
// The caller passes the byte count of the block buffer and the function
// never reads past it; on any failure it writes nothing that could pass for
// an image, so the caller learns of the failure instead of chasing a black
// rectangle through the drawing code.

#ifndef M2W_DXT_H
#define M2W_DXT_H

#include <cstdint>

/// The three compression formats the game uses.
///
/// Named as in Direct3D. OpenGL calls them `BC1`, `BC2`, `BC3`; this project
/// keeps the old names.
enum EM2wDxtFormat
{
    M2W_DXT1 = 1,   ///< 8 bytes per 4x4 block, one-bit alpha or none
    M2W_DXT3 = 3,   ///< 16 bytes per block, explicit 4-bit alpha
    M2W_DXT5 = 5,   ///< 16 bytes per block, interpolated alpha
};

/// Decodes a whole texture to `0xAARRGGBB` pixels.
///
/// `c_pvBlocks` - compressed data, blocks left to right, then down (like
///                the pixels).
/// `uBytes`     - size of the block buffer; the function MUST NOT read past it.
/// `iWidth`, `iHeight` - in pixels; both must be multiples of 4.
/// `pauTarget`  - room for `iWidth * iHeight` 32-bit words, tightly packed,
///                first image row first.
///
/// Returns `false` when something cannot be done - bad format, bad size,
/// block buffer too small, null pointer. Nothing plausible-looking is
/// written to the target in that case.
bool M2W_DecodeDxt(EM2wDxtFormat eFormat, const void* c_pvBlocks,
                   unsigned int uBytes, int iWidth, int iHeight,
                   std::uint32_t* pauTarget);

/// Bytes a texture of this format and size occupies. Zero for a bad format
/// or a size that is not a multiple of 4.
///
/// Separate because the caller needs the number BEFORE allocating - and the
/// code that later reads the buffer had better be the code that sized it.
unsigned int M2W_DxtByteCount(EM2wDxtFormat eFormat, int iWidth, int iHeight);

#endif  // M2W_DXT_H
