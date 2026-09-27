// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_states.cpp - translation of Direct3D 8 constants into OpenGL ES 3.
// See d3d8_states.h for the three traps; the reasons sit at the cases.

#include "d3d8_states.h"

// Compressed formats are not part of core OpenGL ES 3 - they come with the
// `WEBGL_compressed_texture_s3tc` extension. The emscripten header does not
// declare them, so the values stand here, under the extension's names.
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT   0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT  0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3
#endif

TTextureFormat M2W_TextureFormat(D3DFORMAT eFormat)
{
    TTextureFormat f;
    f.eInternalFormat = GL_RGBA8;
    f.eFormat = GL_RGBA;
    f.eType = GL_UNSIGNED_BYTE;
    f.eSwizzle = SWIZZLE_NONE;
    f.uBytesPerPixel = 4;
    f.bCompressed = false;
    f.bKnown = true;

    switch (eFormat)
    {
        // --- eight bits per component ------------------------------------
        // The Direct3D name reads from the MOST SIGNIFICANT byte of the
        // 32-bit word. In little-endian memory that gives B, G, R, A - and
        // OpenGL ES 3 has no BGRA format. Hence the swap.
        case D3DFMT_A8R8G8B8:
            f.eSwizzle = SWIZZLE_BGRA_TO_RGBA;
            break;

        case D3DFMT_X8R8G8B8:
            // `X` = an unused byte; Direct3D treated it as full opacity. Two
            // traps (the layer audit): (1) `GL_RGB8` with `GL_RGBA`
            // is not in ES 3's CLOSED table of legal (internal format,
            // format, type) triples - `glTexImage2D` fails with
            // `GL_INVALID_OPERATION` and creates NO texture, without a word;
            // (2) the X byte copied into alpha makes a pixel with a zero there
            // TRANSPARENT. Hence an internal format with alpha and a swizzle
            // that WRITES 255 - one alpha byte per pixel of card memory for
            // Direct3D's semantics to the letter.
            f.eInternalFormat = GL_RGBA8;
            f.eFormat = GL_RGBA;
            f.eSwizzle = SWIZZLE_BGRX_TO_RGBA;
            break;

        case D3DFMT_R8G8B8:
            // Three bytes per pixel, no alpha, no padding. Rare, but the
            // client uses it (3 occurrences).
            f.eInternalFormat = GL_RGB8;
            f.eFormat = GL_RGB;
            f.eSwizzle = SWIZZLE_BGR_TO_RGB;
            f.uBytesPerPixel = 3;
            break;

        // --- sixteen bits per pixel ---------------------------------------
        // NO byte swap here, and it is not an oversight: these formats pack
        // the components into ONE sixteen-bit word, and OpenGL reads it
        // with the same `GL_UNSIGNED_SHORT_*` type of the same bit layout.
        // Swapping the bytes would break them.
        case D3DFMT_R5G6B5:
            f.eInternalFormat = GL_RGB565;
            f.eFormat = GL_RGB;
            f.eType = GL_UNSIGNED_SHORT_5_6_5;
            f.uBytesPerPixel = 2;
            break;

        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
            // NOTE: Direct3D has alpha on the TOP bit (A1R5G5B5), OpenGL on
            // the BOTTOM one (RGB5_A1). Not the same layout - the word has
            // to be rotated on upload. Marked as a swizzle, because the
            // uploading layer has to walk the buffer anyway.
            f.eInternalFormat = GL_RGB5_A1;
            f.eFormat = GL_RGBA;
            f.eType = GL_UNSIGNED_SHORT_5_5_5_1;
            f.eSwizzle = SWIZZLE_ARGB1555;
            f.uBytesPerPixel = 2;
            break;

        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
            // The same note: A4R4G4B4 has alpha at the top, RGBA4 at the bottom.
            f.eInternalFormat = GL_RGBA4;
            f.eFormat = GL_RGBA;
            f.eType = GL_UNSIGNED_SHORT_4_4_4_4;
            f.eSwizzle = SWIZZLE_ARGB4444;
            f.uBytesPerPixel = 2;
            break;

        // --- compressed ----------------------------------------------------
        // DXT goes to the card WITHOUT decompression - and that is its whole
        // value. Decompressing in wasm would take memory and time exactly
        // where they are short.
        case D3DFMT_DXT1:
            f.eInternalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            f.bCompressed = true;
            f.uBytesPerPixel = 0;   // 4x4 blocks, a pixel has no size
            break;
        case D3DFMT_DXT3:
            f.eInternalFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
            f.bCompressed = true;
            f.uBytesPerPixel = 0;   // 4x4 blocks, a pixel has no size
            break;
        case D3DFMT_DXT5:
            f.eInternalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            f.bCompressed = true;
            f.uBytesPerPixel = 0;   // 4x4 blocks, a pixel has no size
            break;

        // --- depth ---------------------------------------------------------
        case D3DFMT_D16:
            f.eInternalFormat = GL_DEPTH_COMPONENT16;
            f.eFormat = GL_DEPTH_COMPONENT;
            f.eType = GL_UNSIGNED_SHORT;
            f.uBytesPerPixel = 2;
            break;

        default:
            // No pretending. The caller has the right to know that this
            // format will not be uploaded - otherwise it would get a
            // texture full of random content and look for the bug
            // elsewhere.
            f.bKnown = false;
            break;
    }

    return f;
}

GLenum M2W_BlendFactor(DWORD dwBlend)
{
    switch (dwBlend)
    {
        case D3DBLEND_ZERO:            return GL_ZERO;
        case D3DBLEND_ONE:             return GL_ONE;
        case D3DBLEND_SRCCOLOR:        return GL_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR:     return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA:        return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA:     return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA:       return GL_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA:    return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR:       return GL_DST_COLOR;
        case D3DBLEND_INVDESTCOLOR:    return GL_ONE_MINUS_DST_COLOR;
        case D3DBLEND_SRCALPHASAT:     return GL_SRC_ALPHA_SATURATE;
        default:                       return GL_ONE;
    }
}

GLenum M2W_BlendOp(DWORD dwOp)
{
    switch (dwOp)
    {
        case D3DBLENDOP_ADD:         return GL_FUNC_ADD;
        case D3DBLENDOP_SUBTRACT:    return GL_FUNC_SUBTRACT;
        case D3DBLENDOP_REVSUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case D3DBLENDOP_MIN:         return GL_MIN;
        case D3DBLENDOP_MAX:         return GL_MAX;
        // Zero lands here too - an unset state. Add is Direct3D's initial
        // value, so this is the right answer, not sweeping under the rug.
        default:                     return GL_FUNC_ADD;
    }
}

GLenum M2W_CompareFunc(DWORD dwFunc)
{
    switch (dwFunc)
    {
        case D3DCMP_NEVER:        return GL_NEVER;
        case D3DCMP_LESS:         return GL_LESS;
        case D3DCMP_EQUAL:        return GL_EQUAL;
        case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
        case D3DCMP_GREATER:      return GL_GREATER;
        case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
        case D3DCMP_ALWAYS:       return GL_ALWAYS;
        default:                  return GL_LEQUAL;
    }
}

void M2W_CullMode(DWORD dwCull, bool* pbEnabled, GLenum* peSide)
{
    if (!pbEnabled || !peSide)
        return;

    // ======================================================================
    // THE MAPPING REVERSED - CORRECTED BY A MEASUREMENT IN THE BROWSER
    // ======================================================================
    // Earlier the cross mapping (`CW` -> `GL_FRONT`) stood here, with
    // a note that it was the one place in this file reason alone does not
    // settle. It was the other way round, and the second-frame test
    // measured it.
    //
    // The reasoning that agrees with the measurement - on the triangle
    // (160,60), (260,180), (60,180), i.e. VISUALLY CLOCKWISE:
    //
    //   * in screen coordinates (y DOWN) the signed area comes out
    //     positive, which with the Y axis down means "clockwise";
    //   * converted to OpenGL's window (y UP) the same area comes out
    //     NEGATIVE - and with the Y axis up negative also means
    //     "clockwise".
    //
    // In other words: FLIPPING THE Y AXIS AND FLIPPING ITS SENSE CANCEL OUT.
    // A triangle Direct3D sees as CW, OpenGL sees as CW too - i.e. as a
    // BACK face (because OpenGL's default front is CCW).
    //
    //     D3DCULL_CW  -> GL_BACK
    //     D3DCULL_CCW -> GL_FRONT
    //
    // My earlier reasoning had one step too many: I noticed that flipping
    // the Y axis flips the winding, and forgot that Direct3D counts it in a
    // frame that ALREADY has Y down. No reading of the code settled it -
    // one triangle on the screen did.
    //
    // The measurement also checked the more important thing: BOTH ROADS
    // BEHAVE THE SAME. A triangle given in pixels (`XYZRHW`) and the same
    // triangle given through matrices are culled at the same setting. So
    // it was in Direct3D, because what counted was the look on the screen,
    // not the road there.
    //
    // A mistake crashes nothing: the front face vanishes instead of the
    // back, i.e. characters and buildings are seen from inside. It looks
    // like holes in the models.
    switch (dwCull)
    {
        case D3DCULL_NONE:
            *pbEnabled = false;
            *peSide = GL_BACK;
            break;
        case D3DCULL_CW:
            *pbEnabled = true;
            *peSide = GL_BACK;
            break;
        case D3DCULL_CCW:
            *pbEnabled = true;
            *peSide = GL_FRONT;
            break;
        default:
            *pbEnabled = false;
            *peSide = GL_BACK;
            break;
    }
}

GLenum M2W_MinFilter(DWORD dwMin, DWORD dwMip)
{
    // In Direct3D the MINIFICATION filter and the MIP filter are two
    // separate states. In OpenGL they are one - which is why this function
    // takes both at once. Separating them would give four combinations, two
    // of which could not arise.
    const bool bLinearMin = (dwMin == D3DTEXF_LINEAR ||
                             dwMin == D3DTEXF_ANISOTROPIC);

    switch (dwMip)
    {
        case D3DTEXF_NONE:
            // No mip levels - a plain filter, without `MIPMAP`.
            return bLinearMin ? GL_LINEAR : GL_NEAREST;
        case D3DTEXF_POINT:
            return bLinearMin ? GL_LINEAR_MIPMAP_NEAREST
                              : GL_NEAREST_MIPMAP_NEAREST;
        case D3DTEXF_LINEAR:
        case D3DTEXF_ANISOTROPIC:
        default:
            return bLinearMin ? GL_LINEAR_MIPMAP_LINEAR
                              : GL_NEAREST_MIPMAP_LINEAR;
    }
}

GLenum M2W_MagFilter(DWORD dwMag)
{
    // Mip levels play no part in magnification - OpenGL accepts only
    // `GL_NEAREST` or `GL_LINEAR` here.
    return (dwMag == D3DTEXF_LINEAR || dwMag == D3DTEXF_ANISOTROPIC)
           ? GL_LINEAR : GL_NEAREST;
}

GLenum M2W_WrapMode(DWORD dwAddress)
{
    switch (dwAddress)
    {
        case D3DTADDRESS_WRAP:   return GL_REPEAT;
        case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
        case D3DTADDRESS_CLAMP:  return GL_CLAMP_TO_EDGE;
        case D3DTADDRESS_BORDER:
            // OpenGL ES 3 HAS NO wrapping to a border colour. The nearest
            // is clamping to the edge - and that is how it behaves in most
            // uses, because the border tends to be transparent anyway.
            //
            // The difference shows where the border colour differs from the
            // texture's edge (`D3DTSS_BORDERCOLOR`, 4 uses). Recorded as a
            // KNOWN difference, not pretended away.
            return GL_CLAMP_TO_EDGE;
        default:
            return GL_REPEAT;
    }
}

GLenum M2W_PrimitiveMode(D3DPRIMITIVETYPE eType)
{
    switch (eType)
    {
        case D3DPT_POINTLIST:     return GL_POINTS;
        case D3DPT_LINELIST:      return GL_LINES;
        case D3DPT_LINESTRIP:     return GL_LINE_STRIP;
        case D3DPT_TRIANGLELIST:  return GL_TRIANGLES;
        case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
        case D3DPT_TRIANGLEFAN:   return GL_TRIANGLE_FAN;
        default:                  return GL_TRIANGLES;
    }
}

UINT M2W_VerticesFromPrimitives(D3DPRIMITIVETYPE eType, UINT uPrimitives)
{
    if (uPrimitives == 0)
        return 0;

    switch (eType)
    {
        case D3DPT_POINTLIST:     return uPrimitives;
        case D3DPT_LINELIST:      return uPrimitives * 2;
        case D3DPT_LINESTRIP:     return uPrimitives + 1;
        case D3DPT_TRIANGLELIST:  return uPrimitives * 3;
        case D3DPT_TRIANGLESTRIP: return uPrimitives + 2;
        case D3DPT_TRIANGLEFAN:   return uPrimitives + 2;
        default:
            // Zero means "do not draw". Guessing a vertex count for an
            // unknown type would have OpenGL read memory past the end of
            // the buffer - and that is no longer a bad image but a crash.
            return 0;
    }
}

void M2W_ColorToFloat(DWORD dwColor, float* pafOut)
{
    if (!pafOut)
        return;

    // A Direct3D colour is 0xAARRGGBB - a WORD, not a run of bytes. Taking
    // it apart with shifts is therefore independent of the machine's byte
    // order and is the right road. Casting to a byte array would be a
    // mistake of the same family as BGRA above.
    pafOut[0] = static_cast<float>((dwColor >> 16) & 0xFF) / 255.0f;  // R
    pafOut[1] = static_cast<float>((dwColor >>  8) & 0xFF) / 255.0f;  // G
    pafOut[2] = static_cast<float>((dwColor      ) & 0xFF) / 255.0f;  // B
    pafOut[3] = static_cast<float>((dwColor >> 24) & 0xFF) / 255.0f;  // A
}

// ---------------------------------------------------------------------------
// Converting texture memory
// ---------------------------------------------------------------------------

void M2W_SwizzlePixels(const TTextureFormat& c_rFormat,
                       const void* c_pvSource, void* pvTarget, UINT uPixels)
{
    if (!c_pvSource || !pvTarget)
        return;

    // DXT blocks go to the card as they are - the whole value of compression.
    if (c_rFormat.bCompressed)
        return;

    const unsigned char* s = static_cast<const unsigned char*>(c_pvSource);
    unsigned char* t = static_cast<unsigned char*>(pvTarget);

    switch (c_rFormat.eSwizzle)
    {
        case SWIZZLE_BGRA_TO_RGBA:
            for (UINT i = 0; i < uPixels; ++i)
            {
                t[i * 4 + 0] = s[i * 4 + 2];   // R <- third byte
                t[i * 4 + 1] = s[i * 4 + 1];   // G stays
                t[i * 4 + 2] = s[i * 4 + 0];   // B <- first byte
                t[i * 4 + 3] = s[i * 4 + 3];   // A stays
            }
            break;

        case SWIZZLE_BGRX_TO_RGBA:
            // As above, but the fourth source byte is UNUSED and may be
            // dropped. Full opacity goes in - see the reason at
            // `D3DFMT_X8R8G8B8`.
            for (UINT i = 0; i < uPixels; ++i)
            {
                t[i * 4 + 0] = s[i * 4 + 2];
                t[i * 4 + 1] = s[i * 4 + 1];
                t[i * 4 + 2] = s[i * 4 + 0];
                t[i * 4 + 3] = 255;
            }
            break;

        case SWIZZLE_BGR_TO_RGB:
            for (UINT i = 0; i < uPixels; ++i)
            {
                t[i * 3 + 0] = s[i * 3 + 2];
                t[i * 3 + 1] = s[i * 3 + 1];
                t[i * 3 + 2] = s[i * 3 + 0];
            }
            break;

        case SWIZZLE_ARGB1555:
            // `ARRRRRGG GGGBBBBB` -> `RRRRRGGG GGBBBBBA`.
            // NOT a byte swap but a rotation of the word by one bit to the
            // left: alpha travels from bit 15 to bit 0, the rest moves one
            // position up. R, G, B keep their order relative to each other
            // - Direct3D and OpenGL store them in the same order, only the
            // place of alpha differs.
            for (UINT i = 0; i < uPixels; ++i)
            {
                const unsigned int w = static_cast<unsigned int>(s[i * 2 + 0]) |
                                      (static_cast<unsigned int>(s[i * 2 + 1]) << 8);
                const unsigned int r = ((w << 1) | (w >> 15)) & 0xFFFFu;
                t[i * 2 + 0] = static_cast<unsigned char>(r & 0xFF);
                t[i * 2 + 1] = static_cast<unsigned char>((r >> 8) & 0xFF);
            }
            break;

        case SWIZZLE_ARGB4444:
            // `AAAARRRR GGGGBBBB` -> `RRRRGGGG BBBBAAAA`: a rotation by four bits.
            for (UINT i = 0; i < uPixels; ++i)
            {
                const unsigned int w = static_cast<unsigned int>(s[i * 2 + 0]) |
                                      (static_cast<unsigned int>(s[i * 2 + 1]) << 8);
                const unsigned int r = ((w << 4) | (w >> 12)) & 0xFFFFu;
                t[i * 2 + 0] = static_cast<unsigned char>(r & 0xFF);
                t[i * 2 + 1] = static_cast<unsigned char>((r >> 8) & 0xFF);
            }
            break;

        case SWIZZLE_NONE:
        default:
        {
            const UINT uBytes = uPixels * c_rFormat.uBytesPerPixel;
            for (UINT i = 0; i < uBytes; ++i)
                t[i] = s[i];
            break;
        }
    }
}
