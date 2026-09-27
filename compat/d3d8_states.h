// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_states.h - translation of Direct3D 8 constants into OpenGL ES 3
// constants: texture formats (with the byte swizzle they need), blend
// factors and equations, compare functions, culling, filters, wrap modes,
// primitive counts and the `0xAARRGGBB` colour. Pure - testable.

// Design:
// WHY A SEPARATE, PURE FILE. This looks like the dullest part of the
// drawing layer: a lookup table from one family of constants to another.
// That is exactly why it gets its own file and its own test
// (d3d8_states_test.cpp). A mistake in such a table DOES NOT CRASH the
// program. It gives an image that draws and is wrong: walls seen from
// inside, transparency the other way round, textures blue instead of red
// - exactly the faults that in a finished game look like "something is
// off with the graphics" and nobody can point at. Not one OpenGL call
// happens here - numbers in, numbers out - so a program can check it.
//
// THREE TRAPS THIS FILE SETTLES ONCE:
// 1. A8R8G8B8 IS BGRA IN MEMORY. The Direct3D format name describes the
//    32-bit word from the most significant byte (0xAARRGGBB), not the byte
//    order. On a little-endian machine - and wasm is little-endian - the
//    bytes lie B, G, R, A. OpenGL ES 3 HAS NO BGRA format. They have to be
//    swapped. The same trap as `RGB()` in `win32_compat.h`, written down
//    there once already.
// 2. FACE WINDING. Direct3D 8 worked LEFT-HANDED and "front" meant
//    something else than in OpenGL. `D3DCULL_CCW` - the most common in
//    Direct3D games - culls the faces whose vertices go COUNTER-clockwise
//    seen in Direct3D's frame, i.e. clockwise in OpenGL's; settled by a
//    measurement, see `M2W_CullMode`.
// 3. A PRIMITIVE COUNT IS NOT A VERTEX COUNT. `DrawPrimitive` takes the
//    number of TRIANGLES, `glDrawArrays` the number of VERTICES. For a list
//    that is times three, for a strip or fan plus two. A mistake gives a
//    missing or an extra triangle in every draw.

#pragma once

#include "win32_compat.h"
#include "d3d8.h"

#include <GLES3/gl3.h>

/// How texture memory is converted from the Direct3D layout to OpenGL's.
///
/// DESIGN FIX: this used to be one boolean `bSwapBGRA`. Too
/// little, and it showed only when the layer using it was written:
/// eight-bit formats need a BYTE SWAP (B and R exchanged), sixteen-bit
/// formats a ROTATION OF BIT FIELDS (alpha moves from the top bit to the
/// bottom). Two entirely different operations, and one flag made them
/// confusable.
enum EPixelSwizzle
{
    /// Memory goes to OpenGL unchanged.
    SWIZZLE_NONE = 0,

    /// Four bytes per pixel, B and R exchanged.
    SWIZZLE_BGRA_TO_RGBA,

    /// The same, but THE FOURTH BYTE IS DROPPED AND REPLACED BY FULL
    /// OPACITY. For formats with `X` instead of `A`, where that byte means
    /// nothing - copied as is it would mean "invisible" whenever a zero
    /// happened to lie there.
    SWIZZLE_BGRX_TO_RGBA,

    /// Three bytes per pixel, B and R exchanged.
    SWIZZLE_BGR_TO_RGB,

    /// 16-bit word: `ARRRRRGG GGGBBBBB` -> `RRRRRGGG GGBBBBBA`. Alpha
    /// travels from bit 15 to bit 0.
    SWIZZLE_ARGB1555,

    /// 16-bit word: `AAAARRRR GGGGBBBB` -> `RRRRGGGG BBBBAAAA`. A rotation
    /// by four bits to the left.
    SWIZZLE_ARGB4444,
};

/// The description of a texture format on the OpenGL side.
struct TTextureFormat
{
    GLenum eInternalFormat;     ///< `internalformat` for `glTexImage2D`
    GLenum eFormat;             ///< `format`
    GLenum eType;               ///< `type`

    /// What to do with the memory before uploading. See trap 1 above.
    EPixelSwizzle eSwizzle;

    /// Whether this is a COMPRESSED format (DXT) - then
    /// `glCompressedTexImage2D` is used and `eFormat`/`eType` mean nothing.
    bool bCompressed;

    /// Bytes per pixel. ZERO for compressed formats - a pixel has no fixed
    /// size there, 4x4 blocks count.
    ///
    /// It lives HERE, not in a function of the swizzle kind, and that is a
    /// fix of a same-day mistake: `SWIZZLE_NONE` covers `R5G6B5` (two
    /// bytes) and `D16` (two) - but also any future format of another
    /// size. The swizzle kind does NOT determine the pixel size and the
    /// question was wrongly put.
    UINT uBytesPerPixel;

    /// Whether the format is handled at all. `false` means "not pretending".
    bool bKnown;
};

/// Direct3D pixel format -> description for OpenGL.
TTextureFormat M2W_TextureFormat(D3DFORMAT eFormat);

/// Blend factor `D3DBLEND_*` -> `GL_*`. Returns `GL_ONE` for an unknown
/// one, the value that distorts the image the least.
GLenum M2W_BlendFactor(DWORD dwBlend);

/// Comparison `D3DCMP_*` -> `GL_*` (for the depth test).
GLenum M2W_CompareFunc(DWORD dwFunc);

/// Face culling. Returns through pointers, because in OpenGL these are TWO
/// things - whether to cull and which side - and in Direct3D one.
void M2W_CullMode(DWORD dwCull, bool* pbEnabled, GLenum* peSide);

/// Filtering. `dwMip` may be `D3DTEXF_NONE` - then there are no mip levels
/// and the minification filter is plain, without `MIPMAP`.
GLenum M2W_MinFilter(DWORD dwMin, DWORD dwMip);
/// Magnification: `GL_LINEAR` or `GL_NEAREST`, mip levels play no part.
GLenum M2W_MagFilter(DWORD dwMag);

/// `D3DRS_BLENDOP` -> OpenGL blend equation.
///
/// Checked, because it looked like a trap and is not: Direct3D computes
/// `D3DBLENDOP_SUBTRACT` as `source - destination`, exactly what
/// `GL_FUNC_SUBTRACT` does; `D3DBLENDOP_REVSUBTRACT` is `destination -
/// source`, i.e. `GL_FUNC_REVERSE_SUBTRACT`. The names correspond one to
/// one and must NOT be swapped. Written down precisely because a reflexive
/// "they are probably reversed" would cost one swap too many.
GLenum M2W_BlendOp(DWORD dwOp);

/// Texture coordinate wrapping.
GLenum M2W_WrapMode(DWORD dwAddress);

/// Primitive type -> OpenGL mode.
GLenum M2W_PrimitiveMode(D3DPRIMITIVETYPE eType);

/// Number of PRIMITIVES -> number of VERTICES. See trap 3. Returns 0 for
/// an unknown type - better to draw nothing than garbage from the memory
/// beside the buffer.
UINT M2W_VerticesFromPrimitives(D3DPRIMITIVETYPE eType, UINT uPrimitives);

/// Splits a Direct3D colour (0xAARRGGBB) into four components 0..1.
/// Separate, because `D3DRS_TEXTUREFACTOR` (57 uses) and the fog colour
/// come in that form and the shader wants a `vec4`.
void M2W_ColorToFloat(DWORD dwColor, float* pafOut);

/// Converts a pixel buffer from the Direct3D layout to OpenGL's.
///
/// Source and target are SEPARATE, because the source buffer belongs to
/// the client and must not be damaged - the client keeps textures in the
/// `D3DPOOL_MANAGED` pool and may upload them again after a lost context.
///
/// Takes the WHOLE format description, not the swizzle kind alone: the
/// pixel size is a property of the format, not of the swizzle, and
/// separating them was an occasion for inconsistency.
///
/// For a compressed format does NOTHING - DXT blocks are not converted,
/// they go to the card as they are.
void M2W_SwizzlePixels(const TTextureFormat& c_rFormat,
                       const void* c_pvSource, void* pvTarget, UINT uPixels);
