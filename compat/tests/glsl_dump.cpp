// SPDX-License-Identifier: GPL-2.0-or-later
// glsl_dump.cpp - prints the GLSL the fixed-function composer
// (compat/d3d8_fixedfunc.cpp) generates for a fixed SET OF PIPELINE KEYS:
// the behaviour gate of Phase C groups 6-7 (`tools/gates/check_glsl.py`).
//
// The set is not "every combination" (that would be millions) but every
// FACTOR varied on its own from two base keys (the Direct3D default and a
// two-stage lit key), plus the full product of stage-0 colour operation x
// argument pair, which is where most of the composer's branches live. A
// rewrite that changes any line of the composer changes at least one of
// these programs.
//
// Build and run as the tests (tools/gates/run_tests.py):
//   em++ -std=c++17 -O1 -Icompat compat/tests/glsl_dump.cpp compat/d3d8_fixedfunc.cpp
//        compat/win32_compat.cpp -o glsl_dump.js && node glsl_dump.js

#include <cstdio>
#include <string>

#include "d3d8_fixedfunc.h"

namespace {

int g_iKeys = 0;

/// Prints the vertex and pixel program built for `c_rKey`, headed by a
/// running number, `c_szName` and the key hash (check_glsl.py compares this).
void Dump(const char* c_szName, const TPipelineKey& c_rKey)
{
    ++g_iKeys;
    std::printf("=== key %d: %s hash=%08lx ===\n", g_iKeys, c_szName, (unsigned long)M2W_KeyHash(c_rKey));
    std::printf("%s", M2W_BuildVertexProgram(c_rKey).c_str());
    std::printf("--- pixel ---\n");
    std::printf("%s", M2W_BuildPixelProgram(c_rKey).c_str());
}

/// The Direct3D default pipeline key (`M2W_DefaultPipelineKey`).
TPipelineKey Default()
{
    TPipelineKey k;
    M2W_DefaultPipelineKey(&k);
    return k;
}

/// Lit, two texture stages, two coordinate sets, fog and alpha test - the
/// shape of a terrain or model key.
TPipelineKey Rich()
{
    TPipelineKey k = Default();
    k.stage[0].bTextureBound = true;
    k.stage[1].bTextureBound = true;
    k.stage[1].dwColorOp = D3DTOP_MODULATE;
    k.stage[1].dwColorArg1 = D3DTA_TEXTURE;
    k.stage[1].dwColorArg2 = D3DTA_CURRENT;
    k.stage[1].dwAlphaOp = D3DTOP_SELECTARG2;
    k.stage[1].dwAlphaArg2 = D3DTA_CURRENT;
    k.stage[1].dwTexCoordIndex = 1;
    k.bNormal = true;
    k.bDiffuse = true;
    k.iTexCoordSets = 2;
    k.bLighting = true;
    k.bFog = true;
    k.dwFogMode = D3DFOG_LINEAR;
    k.bAlphaTest = true;
    k.dwAlphaFunc = D3DCMP_GREATEREQUAL;
    return k;
}

const DWORD c_adwOps[] = {
    D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE,
    D3DTOP_MODULATE2X, D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED,
    D3DTOP_SUBTRACT, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDTEXTUREALPHA,
    D3DTOP_BLENDFACTORALPHA, D3DTOP_MODULATEALPHA_ADDCOLOR,
    D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR,
    D3DTOP_MODULATEINVCOLOR_ADDALPHA, 9, 11, 15, 16, 17, 22, 23, 24, 25, 26 };
const DWORD c_adwArgs[] = { D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTA_SPECULAR, D3DTA_TEMP };

}  // namespace

/// Dumps the programs of every key combination the gate covers.
int main()
{
    char szName[160];

    Dump("default", Default());
    Dump("rich", Rich());

    // stage 0: every colour op x every argument pair, texture bound
    for (size_t o = 0; o < sizeof(c_adwOps) / sizeof(c_adwOps[0]); ++o)
        for (size_t a = 0; a < sizeof(c_adwArgs) / sizeof(c_adwArgs[0]); ++a)
            for (size_t b = 0; b < sizeof(c_adwArgs) / sizeof(c_adwArgs[0]); ++b)
            {
                TPipelineKey k = Default();
                k.stage[0].bTextureBound = true;
                k.stage[0].dwColorOp = c_adwOps[o];
                k.stage[0].dwColorArg1 = c_adwArgs[a];
                k.stage[0].dwColorArg2 = c_adwArgs[b];
                std::snprintf(szName, sizeof(szName), "stage0 colour op %lu args %lu %lu",
                              (unsigned long)c_adwOps[o], (unsigned long)c_adwArgs[a], (unsigned long)c_adwArgs[b]);
                Dump(szName, k);
            }

    // stage 0: every alpha op with three argument pairs
    for (size_t o = 0; o < sizeof(c_adwOps) / sizeof(c_adwOps[0]); ++o)
        for (int p = 0; p < 3; ++p)
        {
            static const DWORD c_aadwPairs[3][2] = {
                { D3DTA_TEXTURE, D3DTA_DIFFUSE }, { D3DTA_DIFFUSE, D3DTA_CURRENT }, { D3DTA_TFACTOR, D3DTA_TEXTURE } };
            TPipelineKey k = Default();
            k.stage[0].bTextureBound = true;
            k.stage[0].dwAlphaOp = c_adwOps[o];
            k.stage[0].dwAlphaArg1 = c_aadwPairs[p][0];
            k.stage[0].dwAlphaArg2 = c_aadwPairs[p][1];
            std::snprintf(szName, sizeof(szName), "stage0 alpha op %lu pair %d", (unsigned long)c_adwOps[o], p);
            Dump(szName, k);
        }

    // argument modifiers on stage 0 and 1
    for (int s = 0; s < 2; ++s)
        for (int m = 1; m < 4; ++m)
        {
            TPipelineKey k = Rich();
            const DWORD dwMod = (m & 1 ? D3DTA_COMPLEMENT : 0) | (m & 2 ? D3DTA_ALPHAREPLICATE : 0);
            k.stage[s].dwColorArg1 |= dwMod;
            k.stage[s].dwAlphaArg2 |= dwMod;
            std::snprintf(szName, sizeof(szName), "stage%d modifiers 0x%lx", s, (unsigned long)dwMod);
            Dump(szName, k);
        }

    // stage 1: every colour op with (TEXTURE, CURRENT), and the chain rules
    for (size_t o = 0; o < sizeof(c_adwOps) / sizeof(c_adwOps[0]); ++o)
    {
        TPipelineKey k = Rich();
        k.stage[1].dwColorOp = c_adwOps[o];
        std::snprintf(szName, sizeof(szName), "stage1 colour op %lu", (unsigned long)c_adwOps[o]);
        Dump(szName, k);
    }
    {
        TPipelineKey k = Rich();
        k.stage[0].dwColorOp = D3DTOP_DISABLE;
        Dump("stage0 colour disabled, alpha kept", k);
        k.stage[0].dwAlphaOp = D3DTOP_DISABLE;
        Dump("stage0 fully disabled - chain broken", k);
    }

    // texture bound or not, per stage
    for (int b = 0; b < 4; ++b)
    {
        TPipelineKey k = Rich();
        k.stage[0].bTextureBound = (b & 1) != 0;
        k.stage[1].bTextureBound = (b & 2) != 0;
        std::snprintf(szName, sizeof(szName), "textures bound %d", b);
        Dump(szName, k);
    }

    // vertex format
    for (int f = 0; f < 32; ++f)
    {
        TPipelineKey k = Rich();
        k.bTransformed = (f & 1) != 0;
        k.bNormal = (f & 2) != 0;
        k.bDiffuse = (f & 4) != 0;
        k.bSpecular = (f & 8) != 0;
        k.bSpecularEnable = (f & 16) != 0;
        std::snprintf(szName, sizeof(szName), "fvf bits %d", f);
        Dump(szName, k);
    }
    for (int n = 0; n <= 3; ++n)
    {
        TPipelineKey k = Rich();
        k.iTexCoordSets = n;
        std::snprintf(szName, sizeof(szName), "texcoord sets %d", n);
        Dump(szName, k);
    }

    // texture coordinate generation and transform
    for (int t = 0; t < 4; ++t)
        for (int c = 0; c <= 4; ++c)
        {
            TPipelineKey k = Rich();
            k.stage[0].dwTexCoordIndex = (DWORD)t << 16;
            k.stage[0].dwTextureTransformFlags = (DWORD)c;
            std::snprintf(szName, sizeof(szName), "stage0 tci %d ttff %d", t, c);
            Dump(szName, k);
        }
    for (int t = 0; t < 4; ++t)
    {
        TPipelineKey k = Rich();
        k.stage[1].dwTexCoordIndex = ((DWORD)t << 16) | 1;
        k.stage[1].dwTextureTransformFlags = D3DTTFF_COUNT2;
        std::snprintf(szName, sizeof(szName), "stage1 tci %d ttff 2", t);
        Dump(szName, k);
    }
    // generated coordinates WITHOUT a vertex normal (the fallback), and the
    // chain rule on stage 1 (reviewer, review)
    for (int t = 1; t < 4; ++t)
    {
        TPipelineKey k = Rich();
        k.bNormal = false;
        k.stage[0].dwTexCoordIndex = (DWORD)t << 16;
        std::snprintf(szName, sizeof(szName), "stage0 tci %d without normal", t);
        Dump(szName, k);
    }
    {
        TPipelineKey k = Rich();
        k.stage[1].dwColorOp = D3DTOP_DISABLE;
        Dump("stage1 colour disabled, alpha kept", k);
        k = Rich();
        k.stage[1].dwAlphaOp = D3DTOP_DISABLE;
        Dump("stage1 alpha disabled, colour kept", k);
    }

    // render states that change the program
    for (int l = 0; l < 4; ++l)
    {
        TPipelineKey k = Rich();
        k.bLighting = (l & 1) != 0;
        k.bColorVertex = (l & 2) != 0;
        std::snprintf(szName, sizeof(szName), "lighting %d colorvertex %d", l & 1, (l >> 1) & 1);
        Dump(szName, k);
    }
    for (int m = 0; m <= 3; ++m)
    {
        TPipelineKey k = Rich();
        k.bFog = true;
        k.dwFogMode = (DWORD)m;
        std::snprintf(szName, sizeof(szName), "fog mode %d", m);
        Dump(szName, k);
        k.bTransformed = true;
        std::snprintf(szName, sizeof(szName), "fog mode %d transformed", m);
        Dump(szName, k);
    }
    {
        TPipelineKey k = Rich();
        k.bFog = false;
        Dump("fog off", k);
    }
    for (int f = 0; f <= 8; ++f)
    {
        TPipelineKey k = Rich();
        k.bAlphaTest = f != 0;
        k.dwAlphaFunc = (DWORD)f;
        std::snprintf(szName, sizeof(szName), "alpha test func %d", f);
        Dump(szName, k);
    }

    std::printf("=== %d keys ===\n", g_iKeys);
    return 0;
}
