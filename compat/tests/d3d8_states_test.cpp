// d3d8_states_test.cpp - test of the Direct3D 8 -> OpenGL ES 3 translation table.
//
// WHY: because an error in the translation table does not crash the program. It gives
// a picture that draws and is wrong - walls seen from the inside, colours swapped,
// a missing triangle in every draw. In the finished game it looks
// like "something is wrong with the graphics" and nobody can point a finger at it.
//
// Running:
//   em++ -std=c++17 -O1 -Icompat compat/tests/d3d8_states_test.cpp \
//        compat/d3d8_states.cpp compat/win32_compat.cpp \
//        -o d3d8_states_test.js && node d3d8_states_test.js

#include <cstdio>
#include <cmath>

#include "d3d8_states.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-62s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

/// Floats equal within 0.005 (one 8-bit colour step is ~0.004).
bool Near(float a, float b) { return std::fabs(a - b) < 0.005f; }

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("\n=== translation of Direct3D 8 constants -> OpenGL ES 3 ===\n\n");

    // -----------------------------------------------------------------
    std::printf("[the primitive count is NOT the vertex count]\n");
    {
        // `DrawPrimitive` takes the number of TRIANGLES, `glDrawArrays` the number
        // of VERTICES. Values computed by hand.
        struct { D3DPRIMITIVETYPE t; UINT p; UINT expected; const char* n; } cases[] = {
            { D3DPT_TRIANGLELIST,  10, 30, "triangle list: 10 -> 30" },
            { D3DPT_TRIANGLESTRIP, 10, 12, "strip: 10 -> 12" },
            { D3DPT_TRIANGLEFAN,   10, 12, "fan: 10 -> 12" },
            { D3DPT_LINELIST,      10, 20, "line list: 10 -> 20" },
            { D3DPT_LINESTRIP,     10, 11, "polyline: 10 -> 11" },
            { D3DPT_POINTLIST,     10, 10, "points: 10 -> 10" },
            { D3DPT_TRIANGLESTRIP,  1,  3, "strip with one triangle -> 3" },
        };
        char buf[64];
        for (const auto& p : cases)
        {
            const UINT w = M2W_VerticesFromPrimitives(p.t, p.p);
            std::snprintf(buf, sizeof(buf), "(got %u)", w);
            Check(p.n, w == p.expected, buf);
        }

        Check("zero primitives -> zero vertices",
                M2W_VerticesFromPrimitives(D3DPT_TRIANGLELIST, 0) == 0);

        // An unknown kind must give ZERO, not a guess. A guess
        // would make OpenGL read past the end of the buffer - that is no longer
        // a wrong picture, but a crash.
        Check("unknown kind -> zero, not a guess",
                M2W_VerticesFromPrimitives(static_cast<D3DPRIMITIVETYPE>(99), 10) == 0);
    }

    // -----------------------------------------------------------------
    std::printf("\n[the winding direction is REVERSED]\n");
    {
        bool bOn = false;
        GLenum eFace = 0;

        M2W_CullMode(D3DCULL_NONE, &bOn, &eFace);
        Check("NONE culls nothing", !bOn);

        // Values CORRECTED by a measurement in the browser - before that
        // it was the other way round. The reasoning stands by the code in `d3d8_states.cpp`.
        M2W_CullMode(D3DCULL_CW, &bOn, &eFace);
        Check("CW culls GL_BACK", bOn && eFace == GL_BACK);

        M2W_CullMode(D3DCULL_CCW, &bOn, &eFace);
        Check("CCW culls GL_FRONT", bOn && eFace == GL_FRONT);

        // OPPOSITE control: if someone ever "simplified" this function so
        // that both cases give the same, the two tests above would still pass
        // with one value. This one will not.
        GLenum a = 0, b = 0;
        bool x = false;
        M2W_CullMode(D3DCULL_CW, &x, &a);
        M2W_CullMode(D3DCULL_CCW, &x, &b);
        Check("CW and CCW give DIFFERENT faces", a != b);
    }

    // -----------------------------------------------------------------
    std::printf("\n[A8R8G8B8 is BGRA in memory]\n");
    {
        const TTextureFormat a8 = M2W_TextureFormat(D3DFMT_A8R8G8B8);
        Check("A8R8G8B8 is known", a8.bKnown);
        Check("A8R8G8B8 needs bytes B and R swapped",
                a8.eSwizzle == SWIZZLE_BGRA_TO_RGBA);
        Check("A8R8G8B8 is not compressed", !a8.bCompressed);
        Check("A8R8G8B8 goes as RGBA8", a8.eInternalFormat == GL_RGBA8);

        const TTextureFormat x8 = M2W_TextureFormat(D3DFMT_X8R8G8B8);
        // `X` means "unused byte" - Direct3D treated it as full
        // opacity. Earlier the test demanded GL_RGB8 "with alpha from
        // OpenGL"; the layer audit overturned that: the pair GL_RGB8 +
        // GL_RGBA is ILLEGAL in ES 3 (glTexImage2D rejects it silently),
        // and the X byte copied into alpha would give a TRANSPARENT pixel. Today:
        // an internal format with alpha, and the conversion inserts 255.
        Check("X8R8G8B8 goes as RGBA8 (alpha inserted as 255)",
                x8.eInternalFormat == GL_RGBA8);

        // AND HERE the opposite trap: a 16-bit format packs the components into ONE
        // word, read with the same type. Reordering bytes WOULD BREAK it.
        const TTextureFormat r565 = M2W_TextureFormat(D3DFMT_R5G6B5);
        Check("R5G6B5 needs NO reordering at all",
                r565.eSwizzle == SWIZZLE_NONE);
        Check("R5G6B5 is read with type 5_6_5",
                r565.eType == GL_UNSIGNED_SHORT_5_6_5);
    }

    // -----------------------------------------------------------------
    std::printf("\n[compressed formats go without unpacking]\n");
    {
        const TTextureFormat d1 = M2W_TextureFormat(D3DFMT_DXT1);
        const TTextureFormat d3 = M2W_TextureFormat(D3DFMT_DXT3);
        const TTextureFormat d5 = M2W_TextureFormat(D3DFMT_DXT5);
        Check("DXT1 is compressed", d1.bCompressed && d1.bKnown);
        Check("DXT3 is compressed", d3.bCompressed && d3.bKnown);
        Check("DXT5 is compressed", d5.bCompressed && d5.bKnown);
        Check("three DXT are three DIFFERENT formats",
                d1.eInternalFormat != d3.eInternalFormat &&
                d3.eInternalFormat != d5.eInternalFormat);
    }

    // -----------------------------------------------------------------
    std::printf("\n[an unknown format says it is unknown]\n");
    {
        const TTextureFormat nn = M2W_TextureFormat(D3DFMT_UNKNOWN);
        // If it pretended it could, the caller would send garbage and look for the error
        // elsewhere. The same rule as with code pages
        // "I did not convert" is a true answer.
        Check("D3DFMT_UNKNOWN does not pretend to be known", !nn.bKnown);

        const TTextureFormat odd = M2W_TextureFormat(
            static_cast<D3DFORMAT>(31337));
        Check("a format off the list does not pretend either", !odd.bKnown);
    }

    // -----------------------------------------------------------------
    std::printf("\n[the minification filter joins TWO Direct3D states]\n");
    {
        // Direct3D has the minification filter and the mip level filter
        // separately; OpenGL has them in one value.
        Check("LINEAR + no levels = GL_LINEAR",
                M2W_MinFilter(D3DTEXF_LINEAR, D3DTEXF_NONE) == GL_LINEAR);
        Check("POINT + no levels = GL_NEAREST",
                M2W_MinFilter(D3DTEXF_POINT, D3DTEXF_NONE) == GL_NEAREST);
        Check("LINEAR + LINEAR = trilinear",
                M2W_MinFilter(D3DTEXF_LINEAR, D3DTEXF_LINEAR)
                == GL_LINEAR_MIPMAP_LINEAR);
        Check("LINEAR + POINT = bilinear with level selection",
                M2W_MinFilter(D3DTEXF_LINEAR, D3DTEXF_POINT)
                == GL_LINEAR_MIPMAP_NEAREST);
        Check("POINT + POINT = nearest on both",
                M2W_MinFilter(D3DTEXF_POINT, D3DTEXF_POINT)
                == GL_NEAREST_MIPMAP_NEAREST);

        // Anisotropic is a separate SETTING in OpenGL, not a filter -
        // so here it has to behave like linear.
        Check("ANISOTROPIC behaves like linear",
                M2W_MinFilter(D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR)
                == GL_LINEAR_MIPMAP_LINEAR);

        Check("magnification knows only two filters",
                M2W_MagFilter(D3DTEXF_LINEAR) == GL_LINEAR &&
                M2W_MagFilter(D3DTEXF_POINT) == GL_NEAREST);
    }

    // -----------------------------------------------------------------
    std::printf("\n[wrapping - and one KNOWN difference]\n");
    {
        Check("WRAP", M2W_WrapMode(D3DTADDRESS_WRAP) == GL_REPEAT);
        Check("CLAMP", M2W_WrapMode(D3DTADDRESS_CLAMP) == GL_CLAMP_TO_EDGE);
        Check("MIRROR", M2W_WrapMode(D3DTADDRESS_MIRROR) == GL_MIRRORED_REPEAT);
        // OpenGL ES 3 has no wrapping to the border colour. The difference is
        // WRITTEN DOWN in the header, not kept quiet.
        Check("BORDER comes down to CLAMP (difference described in the file)",
                M2W_WrapMode(D3DTADDRESS_BORDER) == GL_CLAMP_TO_EDGE);
    }

    // -----------------------------------------------------------------
    std::printf("\n[blending - the eight the client uses]\n");
    {
        Check("SRCALPHA",     M2W_BlendFactor(D3DBLEND_SRCALPHA) == GL_SRC_ALPHA);
        Check("INVSRCALPHA",  M2W_BlendFactor(D3DBLEND_INVSRCALPHA) == GL_ONE_MINUS_SRC_ALPHA);
        Check("ONE",          M2W_BlendFactor(D3DBLEND_ONE) == GL_ONE);
        Check("ZERO",         M2W_BlendFactor(D3DBLEND_ZERO) == GL_ZERO);
        Check("SRCCOLOR",     M2W_BlendFactor(D3DBLEND_SRCCOLOR) == GL_SRC_COLOR);
        Check("INVDESTCOLOR", M2W_BlendFactor(D3DBLEND_INVDESTCOLOR) == GL_ONE_MINUS_DST_COLOR);
        Check("INVSRCCOLOR",  M2W_BlendFactor(D3DBLEND_INVSRCCOLOR) == GL_ONE_MINUS_SRC_COLOR);
        Check("DESTALPHA",    M2W_BlendFactor(D3DBLEND_DESTALPHA) == GL_DST_ALPHA);

        // OPPOSITE control: if the function returned GL_ONE always, seven
        // of the eight tests above would fail - but let us check it directly.
        Check("does not return the same everywhere",
                M2W_BlendFactor(D3DBLEND_SRCALPHA) != M2W_BlendFactor(D3DBLEND_ZERO));
    }

    // -----------------------------------------------------------------
    std::printf("\n[blend equation - names that LOOK swapped]\n");
    {
        // Instinct suggests that `SUBTRACT` in one interface means what
        // `REVERSE_SUBTRACT` means in the other. It does not. Direct3D computes
        // `D3DBLENDOP_SUBTRACT` as `source - destination` and so does
        // `GL_FUNC_SUBTRACT`. These two checks are here so that this
        // agreement is WRITTEN DOWN, not recalled from memory at every
        // reading of this file.
        Check("ADD",         M2W_BlendOp(D3DBLENDOP_ADD) == GL_FUNC_ADD);
        Check("SUBTRACT is GL_FUNC_SUBTRACT, not REVERSE",
                M2W_BlendOp(D3DBLENDOP_SUBTRACT) == GL_FUNC_SUBTRACT);
        Check("REVSUBTRACT is GL_FUNC_REVERSE_SUBTRACT",
                M2W_BlendOp(D3DBLENDOP_REVSUBTRACT) == GL_FUNC_REVERSE_SUBTRACT);
        Check("MIN",         M2W_BlendOp(D3DBLENDOP_MIN) == GL_MIN);
        Check("MAX",         M2W_BlendOp(D3DBLENDOP_MAX) == GL_MAX);

        // Zero is the UNSET state. Direct3D started with addition, so
        // addition is the right answer here, not a fallback.
        Check("the unset state gives addition",
                M2W_BlendOp(0) == GL_FUNC_ADD);

        // OPPOSITE control: if the function always returned GL_FUNC_ADD,
        // all of the above except two would pass anyway.
        Check("does not return addition everywhere",
                M2W_BlendOp(D3DBLENDOP_MIN) !=
                M2W_BlendOp(D3DBLENDOP_MAX));
    }

    // -----------------------------------------------------------------
    std::printf("\n[splitting the colour 0xAARRGGBB]\n");
    {
        float k[4];
        M2W_ColorToFloat(0xFF804020u, k);
        // A=0xFF=1.0, R=0x80=0.502, G=0x40=0.251, B=0x20=0.125
        Check("R from bits 16-23", Near(k[0], 0.502f));
        Check("G from bits 8-15",  Near(k[1], 0.251f));
        Check("B from bits 0-7",   Near(k[2], 0.125f));
        Check("A from bits 24-31", Near(k[3], 1.0f));

        // OPPOSITE control on the order: if R and B were swapped
        // - the most common error here - this test catches it,
        // because 0x80 and 0x20 are different numbers.
        Check("R and B are NOT swapped", !Near(k[0], k[2]));

        M2W_ColorToFloat(0x00000000u, k);
        Check("transparent black is all zeros",
                Near(k[0], 0.0f) && Near(k[3], 0.0f));

        M2W_ColorToFloat(0xFFFFFFFFu, k);
        Check("opaque white is all ones",
                Near(k[0], 1.0f) && Near(k[3], 1.0f));
    }

    // -----------------------------------------------------------------
    std::printf("\n[reordering texture memory]\n");
    {
        // --- four bytes: swapping B and R ---------------------------------
        {
            const TTextureFormat f = M2W_TextureFormat(D3DFMT_A8R8G8B8);
            // The Direct3D pixel 0xAARRGGBB = 0xFF102030 lies in memory
            // little-endian as the bytes 30 20 10 FF, i.e. B G R A.
            const unsigned char source[4] = { 0x30, 0x20, 0x10, 0xFF };
            unsigned char target[4] = { 0, 0, 0, 0 };
            M2W_SwizzlePixels(f, source, target, 1);
            Check("BGRA -> RGBA: R in first place", target[0] == 0x10);
            Check("BGRA -> RGBA: G stays",              target[1] == 0x20);
            Check("BGRA -> RGBA: B in third place",   target[2] == 0x30);
            Check("BGRA -> RGBA: A stays",              target[3] == 0xFF);

            // OPPOSITE control: if the function only copied, the first
            // byte would still be 0x30.
            Check("it surely changed something", target[0] != source[0]);
        }

        // --- more than one pixel -------------------------------------
        {
            const TTextureFormat f = M2W_TextureFormat(D3DFMT_A8R8G8B8);
            const unsigned char source[8] = { 1, 2, 3, 4,  5, 6, 7, 8 };
            unsigned char target[8] = { 0 };
            M2W_SwizzlePixels(f, source, target, 2);
            Check("the second pixel reordered too",
                    target[4] == 7 && target[5] == 6 && target[6] == 5 && target[7] == 8);
        }

        // --- three bytes ---------------------------------------------------
        {
            const TTextureFormat f = M2W_TextureFormat(D3DFMT_R8G8B8);
            Check("R8G8B8 has three bytes per pixel", f.uBytesPerPixel == 3);
            const unsigned char source[3] = { 0x30, 0x20, 0x10 };
            unsigned char target[3] = { 0, 0, 0 };
            M2W_SwizzlePixels(f, source, target, 1);
            Check("BGR -> RGB", target[0] == 0x10 && target[1] == 0x20 && target[2] == 0x30);
        }

        // --- sixteen bits: a ROTATION, not a byte swap ----------------
        {
            const TTextureFormat f = M2W_TextureFormat(D3DFMT_A1R5G5B5);
            Check("A1R5G5B5 is a rotation by one bit",
                    f.eSwizzle == SWIZZLE_ARGB1555);
            Check("A1R5G5B5 has two bytes per pixel", f.uBytesPerPixel == 2);

            // The word 0x8000 is alpha alone on the top bit. After the rotation
            // 0x0001 has to come out - alpha on the lowest.
            const unsigned char source[2] = { 0x00, 0x80 };
            unsigned char target[2] = { 0, 0 };
            M2W_SwizzlePixels(f, source, target, 1);
            const unsigned int w = target[0] | (target[1] << 8);
            Check("alpha from bit 15 moves to bit 0", w == 0x0001u);

            // Red 0x7C00 (bits 10-14) has to land on bits 11-15.
            const unsigned char source2[2] = { 0x00, 0x7C };
            M2W_SwizzlePixels(f, source2, target, 1);
            const unsigned int w2 = target[0] | (target[1] << 8);
            Check("red moves one bit up", w2 == 0xF800u);
        }

        {
            const TTextureFormat f = M2W_TextureFormat(D3DFMT_A4R4G4B4);
            // 0xF000 is alpha alone. After a rotation by four bits: 0x000F.
            const unsigned char source[2] = { 0x00, 0xF0 };
            unsigned char target[2] = { 0, 0 };
            M2W_SwizzlePixels(f, source, target, 1);
            const unsigned int w = target[0] | (target[1] << 8);
            Check("A4R4G4B4: alpha from the top to the bottom", w == 0x000Fu);

            // 0x0F00 is red. After the rotation: 0xF000.
            const unsigned char source2[2] = { 0x00, 0x0F };
            M2W_SwizzlePixels(f, source2, target, 1);
            const unsigned int w2 = target[0] | (target[1] << 8);
            Check("A4R4G4B4: red from the bottom to the top", w2 == 0xF000u);
        }

        // --- no reordering: a copy -------------------------------------
        {
            const TTextureFormat f = M2W_TextureFormat(D3DFMT_R5G6B5);
            const unsigned char source[4] = { 0xAB, 0xCD, 0xEF, 0x12 };
            unsigned char target[4] = { 0, 0, 0, 0 };
            M2W_SwizzlePixels(f, source, target, 2);
            Check("R5G6B5 copies the bytes unchanged",
                    target[0] == 0xAB && target[1] == 0xCD &&
                    target[2] == 0xEF && target[3] == 0x12);
        }

        // --- compressed: NOTHING ------------------------------------------
        {
            const TTextureFormat f = M2W_TextureFormat(D3DFMT_DXT1);
            Check("DXT has no pixel size", f.uBytesPerPixel == 0);
            const unsigned char source[4] = { 1, 2, 3, 4 };
            unsigned char target[4] = { 9, 9, 9, 9 };
            M2W_SwizzlePixels(f, source, target, 1);
            // The blocks go to the card unchanged - the function has no right to touch them.
            Check("the function does not touch DXT blocks",
                    target[0] == 9 && target[3] == 9);
        }
    }

    // -----------------------------------------------------------------
    std::printf("\n[comparisons for the depth test]\n");
    {
        Check("LESSEQUAL", M2W_CompareFunc(D3DCMP_LESSEQUAL) == GL_LEQUAL);
        Check("GREATER",   M2W_CompareFunc(D3DCMP_GREATER) == GL_GREATER);
        Check("ALWAYS",    M2W_CompareFunc(D3DCMP_ALWAYS) == GL_ALWAYS);
        Check("NEVER",     M2W_CompareFunc(D3DCMP_NEVER) == GL_NEVER);
        Check("LESS and GREATER are not the same",
                M2W_CompareFunc(D3DCMP_LESS) != M2W_CompareFunc(D3DCMP_GREATER));
    }

    std::printf("\n=== %s ===\n",
                errors == 0 ? "CONSTANT TRANSLATION WORKS" : "TEST FAILED");
    return errors == 0 ? 0 : 1;
}
