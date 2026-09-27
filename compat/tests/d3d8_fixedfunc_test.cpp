// d3d8_fixedfunc_test.cpp - test of the shader builder for the fixed-function pipeline.
//
// WHY: this is the only part of the drawing layer that is **pure
// computation** - numbers in, a string of GLSL code out. Everything
// further needs a graphics card and eyes; this one can be checked by a program.
//
// And it has to be checked, because an error here has the worst possible shape:
// the picture draws, nothing blows up, only the trees have black foliage or
// the interface is upside down. No assertion will catch that.
//
// The tests are of three kinds, as with the D3DX8 math layer:
//   1. Direct3D RULES that must be reproduced to the letter
//      (the chain of stages, `CURRENT` at stage zero, the order of the alpha test),
//   2. PROPERTIES of the resulting string (brackets, declared variables),
//   3. OPPOSITE CONTROLS - whether the test detects anything at all.
//
// Running:
//   em++ -std=c++17 -O1 -Icompat compat/tests/d3d8_fixedfunc_test.cpp \
//        compat/d3d8_fixedfunc.cpp compat/win32_compat.cpp \
//        -o d3d8_fixedfunc_test.js && node d3d8_fixedfunc_test.js

#include <cstdio>
#include <string>

#include "d3d8_fixedfunc.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-62s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

/// Whether `s` contains `what`.
bool Has(const std::string& s, const char* what)
{
    return s.find(what) != std::string::npos;
}

/// Whether the brackets are balanced. A simple test, but it catches exactly the error
/// that gluing strings together makes most often.
bool Balanced(const std::string& s, char openChar, char closeChar)
{
    int n = 0;
    for (char c : s)
    {
        if (c == openChar) ++n;
        else if (c == closeChar) { --n; if (n < 0) return false; }
    }
    return n == 0;
}

/// The Direct3D default pipeline key (`M2W_DefaultPipelineKey`).
TPipelineKey Default()
{
    TPipelineKey k;
    M2W_DefaultPipelineKey(&k);
    return k;
}

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("\n=== shader builder for the fixed-function pipeline ===\n\n");

    // -----------------------------------------------------------------
    std::printf("[the Direct3D initial state]\n");
    {
        const TPipelineKey k = Default();
        Check("stage 0 is MODULATE", k.stage[0].dwColorOp == D3DTOP_MODULATE);
        Check("stage 1 is DISABLED", k.stage[1].dwColorOp == D3DTOP_DISABLE);
        Check("lighting ON by default", k.bLighting);

        const std::string ps = M2W_BuildPixelProgram(k);
        // MODULATE(TEXTURE, CURRENT), and `CURRENT` at stage zero is
        // the vertex colour - hence the sample multiplied by the colour.
        Check("stage 0 multiplies the sample by the vertex colour",
                Has(ps, "sample0 * vertexColor"));
        Check("stage 1 is not in the code", !Has(ps, "color1 ="));
    }

    // -----------------------------------------------------------------
    std::printf("\n[the chain rule: DISABLE breaks it]\n");
    {
        TPipelineKey k = Default();
        k.stage[0].dwColorOp = D3DTOP_DISABLE;
        k.stage[1].dwColorOp = D3DTOP_MODULATE;   // deliberately SET
        const std::string ps = M2W_BuildPixelProgram(k);

        // Earlier the test demanded that a colour DISABLE break the WHOLE chain
        // (stage 1 with an operation set "does not count"). The terrain overturned that
        // `MapOutdoorRenderSTP.cpp` disables the COLOUR of stage 1, but
        // leaves its ALPHA (the splat map) - colour and alpha are two separate
        // chains, and a stage drops out only when BOTH are disabled.
        Check("a disabled stage-0 colour does not sample - it copies current",
                Has(ps, "color0 = current"));
        Check("stage 1 with an operation set DOES COUNT",
                Has(ps, "color1 ="));
        Check("stage 1 multiplies (MODULATE)", Has(ps, "color1 = (sample1 * current)"));

        TPipelineKey kBoth = Default();
        kBoth.stage[0].dwColorOp = D3DTOP_DISABLE;
        kBoth.stage[0].dwAlphaOp = D3DTOP_DISABLE;
        kBoth.stage[1].dwColorOp = D3DTOP_MODULATE;   // behind the broken chain
        const std::string psBoth = M2W_BuildPixelProgram(kBoth);
        Check("colour AND alpha disabled - chain broken, no stage 1",
                !Has(psBoth, "color1 ="));
        // `vec4 current = vertexColor;` stands in EVERY program (initialisation),
        // so the proof of the break at stage 0 is the absence of its own line
        Check("no stage 0 either - the result is the vertex colour alone",
                !Has(psBoth, "color0 ="));
    }

    // -----------------------------------------------------------------
    std::printf("\n[CURRENT at stage zero is DIFFUSE]\n");
    {
        TPipelineKey k = Default();
        k.stage[0].dwColorOp = D3DTOP_SELECTARG1;
        k.stage[0].dwColorArg1 = D3DTA_CURRENT;
        k.stage[1].dwColorOp = D3DTOP_SELECTARG1;
        k.stage[1].dwColorArg1 = D3DTA_CURRENT;
        const std::string ps = M2W_BuildPixelProgram(k);

        // At the first stage there is no "previous result", so
        // Direct3D substitutes the vertex colour. At the second - the result of the first.
        Check("stage 0: CURRENT = the vertex colour",
                Has(ps, "color0 = vertexColor"));
        Check("stage 1: CURRENT = the result of the previous one",
                Has(ps, "color1 = current"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[argument modifiers and their ORDER]\n");
    {
        TPipelineKey k = Default();
        k.stage[0].dwColorOp = D3DTOP_SELECTARG1;
        k.stage[0].dwColorArg1 = D3DTA_TEXTURE | D3DTA_COMPLEMENT;
        std::string ps = M2W_BuildPixelProgram(k);
        Check("COMPLEMENT gives 1-x", Has(ps, "(vec4(1.0) - sample0)"));

        k.stage[0].dwColorArg1 = D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE;
        ps = M2W_BuildPixelProgram(k);
        Check("ALPHAREPLICATE replicates alpha", Has(ps, "vec4(sample0.a)"));

        // Direct3D sets the order: FIRST the complement, THEN the alpha
        // replication. The other way round would give a different, visible result.
        k.stage[0].dwColorArg1 = D3DTA_TEXTURE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE;
        ps = M2W_BuildPixelProgram(k);
        Check("order: complement BEFORE alpha replication",
                Has(ps, "vec4((vec4(1.0) - sample0).a)"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[the operations the client really uses]\n");
    {
        struct { DWORD op; const char* looking_for; const char* name; } cases[] = {
            { D3DTOP_SELECTARG1, "color0 = sample0",              "SELECTARG1" },
            { D3DTOP_SELECTARG2, "color0 = vertexColor",         "SELECTARG2" },
            { D3DTOP_MODULATE,   "sample0 * vertexColor",        "MODULATE" },
            { D3DTOP_ADD,        "sample0 + vertexColor",        "ADD" },
            { D3DTOP_ADDSIGNED,  "- 0.5",                          "ADDSIGNED" },
            { D3DTOP_MODULATE2X, "* 2.0",                          "MODULATE2X" },
            { D3DTOP_BLENDDIFFUSEALPHA, "vertexColor.a)",         "BLENDDIFFUSEALPHA" },
        };
        for (const auto& p : cases)
        {
            TPipelineKey k = Default();
            k.stage[0].dwColorOp = p.op;
            k.stage[0].dwColorArg1 = D3DTA_TEXTURE;
            k.stage[0].dwColorArg2 = D3DTA_DIFFUSE;
            const std::string ps = M2W_BuildPixelProgram(k);
            Check(p.name, Has(ps, p.looking_for));
        }
    }

    // -----------------------------------------------------------------
    std::printf("\n[BLENDTEXTUREALPHA takes the sample of ITS OWN stage]\n");
    {
        TPipelineKey k = Default();
        k.stage[0].dwColorOp = D3DTOP_SELECTARG1;
        k.stage[0].dwColorArg1 = D3DTA_TEXTURE;
        k.stage[1].dwColorOp = D3DTOP_BLENDTEXTUREALPHA;
        k.stage[1].dwColorArg1 = D3DTA_TEXTURE;
        k.stage[1].dwColorArg2 = D3DTA_CURRENT;
        const std::string ps = M2W_BuildPixelProgram(k);

        // The weight has to come from the texture of THIS stage (sample1), not from stage
        // zero. The name substitution is done by `SubstituteSample`; if it failed,
        // a placeholder string would stay in the code.
        Check("the weight is sample1, not sample0", Has(ps, "sample1.a)"));
        Check("no placeholder string left", !Has(ps, "SAMPLE_OF_THIS_STAGE"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[a stage without a texture gives WHITE, not zero]\n");
    {
        TPipelineKey k = Default();
        k.stage[0].bTextureBound = false;
        std::string ps = M2W_BuildPixelProgram(k);
        // Direct3D treated `D3DTA_TEXTURE` without a bound texture as white.
        // Zero would multiply the whole picture by nothing - a black screen instead of
        // drawing with the vertex colour alone.
        Check("without a texture sample0 = white", Has(ps, "vec4 sample0 = vec4(1.0)"));

        k.stage[0].bTextureBound = true;
        ps = M2W_BuildPixelProgram(k);
        Check("with a texture sample0 = a read", Has(ps, "texture(uTexture0"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[the alpha test before the fog]\n");
    {
        TPipelineKey k = Default();
        k.bAlphaTest = true;
        k.dwAlphaFunc = D3DCMP_GREATEREQUAL;
        k.bFog = true;
        k.dwFogMode = D3DFOG_LINEAR;
        const std::string ps = M2W_BuildPixelProgram(k);

        const size_t pos_discard = ps.find("discard");
        // I look for the MIXING with the fog, not the name `uFogColor` - that one also appears
        // in the uniform declaration at the top of the file, i.e. ALWAYS before
        // the discard. The first version of this test went red because of that
        // with correct code: it measured the position of the declaration.
        const size_t pos_fog = ps.find("mix(uFogColor");
        Check("there is a pixel discard", pos_discard != std::string::npos);
        Check("there is mixing with the fog", pos_fog != std::string::npos);
        // The order matters: fog does not change alpha, so the test has to
        // look at the alpha FROM THE STAGES. The other way round - tree foliage would stop
        // being transparent where the fog brightens it.
        Check("discard BEFORE the fog", pos_discard < pos_fog);

        Check("GREATEREQUAL discards the negation of the condition",
                Has(ps, "!(result.a >= uAlphaRef)"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[vertices ALREADY transformed - XYZRHW]\n");
    {
        TPipelineKey k = Default();
        k.bTransformed = true;
        const std::string vs = M2W_BuildVertexProgram(k);

        // The user interface gives coordinates in PIXELS, counting from
        // the top left corner. OpenGL counts from the bottom - hence the Y flip.
        // A mistake gives an upside-down interface and nothing more; it compiles.
        Check("does not multiply by the world-view-projection matrix",
                !Has(vs, "gl_Position = uWorldViewProjection"));
        // the half pixel `uHalfPixel` (0.5 / GUI scale) added BEFORE
        // the division - otherwise with a GUI scale > 1 text and frames are blurred.
        Check("divides by the target size (with the half pixel)",
                Has(vs, "(aPosition.xy + uHalfPixel) / uTargetSize"));
        Check("FLIPS the Y axis", Has(vs, "1.0 - p.y * 2.0"));

        TPipelineKey k2 = Default();
        const std::string vs2 = M2W_BuildVertexProgram(k2);
        Check("without XYZRHW multiplies by the matrix",
                Has(vs2, "gl_Position = uWorldViewProjection"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[the vertex colour is BGRA, not RGBA]\n");
    {
        TPipelineKey k = Default();
        k.bDiffuse = true;
        k.bSpecular = true;
        k.bLighting = false;
        const std::string vs = M2W_BuildVertexProgram(k);

        // `D3DFVF_DIFFUSE` is a `D3DCOLOR` word (0xAARRGGBB), in memory
        // little-endian: B, G, R, A. The attribute goes to OpenGL without
        // reordering - there are too many vertices to walk through them -
        // so the shader straightens the order with one word.
        //
        // Without it the characters' skin would be blue and the sky red.
        // The picture draws, nothing blows up.
        Check("diffuse colour read through .bgra",
                Has(vs, "vColor = aColor.bgra"));
        Check("the specular colour too", Has(vs, "vSpecular = aSpecular.bgra"));

        // OPPOSITE control: if someone ever "simplified" it, a bare
        // `aColor` without the suffix would remain.
        Check("no bare aColor left", !Has(vs, "vColor = aColor;"));

        // The same rule with lighting - the base of the colouring also comes
        // from the vertex and also needs straightening.
        TPipelineKey k2 = Default();
        k2.bDiffuse = true;
        k2.bNormal = true;
        k2.bLighting = true;
        k2.bColorVertex = true;
        Check("with lighting base also through .bgra",
                Has(M2W_BuildVertexProgram(k2), "base = aColor.bgra"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[fog - three modes, three formulas]\n");
    {
        TPipelineKey k = Default();
        k.bFog = true;

        k.dwFogMode = D3DFOG_LINEAR;
        Check("LINEAR", Has(M2W_BuildVertexProgram(k), "uFogRange.y -"));

        k.dwFogMode = D3DFOG_EXP;
        const std::string e1 = M2W_BuildVertexProgram(k);
        Check("EXP", Has(e1, "exp(-uFogDensity") && !Has(e1, "pow("));

        k.dwFogMode = D3DFOG_EXP2;
        Check("EXP2 squares",
                Has(M2W_BuildVertexProgram(k), "pow(uFogDensity"));

        k.bFog = false;
        Check("without fog the factor is 1",
                Has(M2W_BuildVertexProgram(k), "vFog = 1.0"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[computing texture coordinates - D3DTSS_TCI_*]\n");
    {
        TPipelineKey k = Default();
        k.bNormal = true;
        k.stage[0].dwTexCoordIndex = D3DTSS_TCI_CAMERASPACEPOSITION;
        Check("from the position in camera space",
                Has(M2W_BuildVertexProgram(k), "uWorldView * vec4(aPosition.xyz"));

        k.stage[0].dwTexCoordIndex = D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR;
        Check("from the reflection vector",
                Has(M2W_BuildVertexProgram(k), "reflect("));

        // Without a normal the reflection vector cannot be computed. The generator has
        // to give something sensible, not refer to an undeclared input
        // - that would be a shader compile error during the game.
        k.bNormal = false;
        const std::string vs = M2W_BuildVertexProgram(k);
        Check("without a normal does NOT refer to aNormal",
                !Has(vs, "aNormal"));
    }

    // -----------------------------------------------------------------
    std::printf("\n[properties of the resulting string]\n");
    {
        // Walks over many different keys and checks that each gives
        // code with closed brackets and without placeholder strings.
        int examined = 0;
        bool all_ok = true;

        const DWORD operations[] = {
            D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE,
            D3DTOP_MODULATE2X, D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED,
            D3DTOP_SUBTRACT, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDTEXTUREALPHA,
            D3DTOP_BLENDFACTORALPHA, D3DTOP_MODULATEALPHA_ADDCOLOR,
            D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR,
            D3DTOP_MODULATEINVCOLOR_ADDALPHA
        };
        const DWORD arguments[] = {
            D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_TFACTOR,
            D3DTA_TEXTURE | D3DTA_COMPLEMENT,
            D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE
        };

        for (DWORD op0 : operations)
        for (DWORD op1 : operations)
        for (DWORD a : arguments)
        {
            TPipelineKey k = Default();
            k.stage[0].dwColorOp = op0;
            k.stage[0].dwAlphaOp = op0;
            k.stage[0].dwColorArg1 = a;
            k.stage[0].dwColorArg2 = D3DTA_CURRENT;
            k.stage[1].dwColorOp = op1;
            k.stage[1].dwAlphaOp = op1;
            k.stage[1].dwColorArg1 = D3DTA_TEXTURE;
            k.stage[1].dwColorArg2 = a;
            k.stage[0].bTextureBound = true;
            k.stage[1].bTextureBound = true;

            const std::string ps = M2W_BuildPixelProgram(k);
            ++examined;

            if (!Balanced(ps, '(', ')') || !Balanced(ps, '{', '}') ||
                Has(ps, "SAMPLE_OF_THIS_STAGE"))
            {
                all_ok = false;
                break;
            }
        }

        char buf[64];
        std::snprintf(buf, sizeof(buf), "(%d keys)", examined);
        Check("every key gives closed code without placeholder strings",
                all_ok && examined == 16 * 16 * 6, buf);
    }

    // -----------------------------------------------------------------
    std::printf("\n[the key hash]\n");
    {
        TPipelineKey a = Default();
        TPipelineKey b = Default();
        Check("the same keys - the same hash",
                M2W_KeyHash(a) == M2W_KeyHash(b));

        b.stage[1].dwColorOp = D3DTOP_ADD;
        Check("different keys - different hash",
                M2W_KeyHash(a) != M2W_KeyHash(b));
    }

    // -----------------------------------------------------------------
    std::printf("\n[OPPOSITE controls - does the test detect anything]\n");
    {
        // If `Has` always returned true, all the tests above would be
        // worthless. This pair checks that.
        const std::string ps = M2W_BuildPixelProgram(Default());
        Check("searching finds what is there", Has(ps, "void main()"));
        Check("searching does NOT find what is not there",
                !Has(ps, "ThisStringIsSurelyNotThere"));

        std::string broken = "((";
        Check("the bracket check detects unclosed ones",
                !Balanced(broken, '(', ')'));
    }

    std::printf("\n=== %s ===\n",
                errors == 0 ? "SHADER BUILDER WORKS"
                           : "TEST FAILED");
    return errors == 0 ? 0 : 1;
}
