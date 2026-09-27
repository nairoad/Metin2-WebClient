// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_fixedfunc.h - the Direct3D 8 fixed-function pipeline as a shader
// generator: the INPUT (a pipeline key - the set of states that determines
// a program) and the two functions that compose GLSL from it. Pure -
// testable (d3d8_fixedfunc_test.cpp, tools/gates/check_glsl.py).

// Design:
// WHY. Direct3D 8 had a FIXED-FUNCTION PIPELINE: no shaders were written,
// states were set (`D3DRS_*`, `D3DTSS_*`) and the driver composed a program
// out of them. WebGL 2 has no such pipeline. Whoever takes this layer on has to write that composer. This
// header describes its INPUT: the set of states that uniquely determines a
// program. The rest of the drawing layer fills that set in and asks for
// the finished program.
//
// WHY A SEPARATE, PURE FILE. Not one OpenGL call, not one `EM_JS` here.
// Numbers in, two strings of GLSL out. So it CAN BE CHECKED BY A TEST,
// without a graphics card and without a browser - the only part of the
// drawing layer that is pure arithmetic. Category B of the split from cz.
// 137: an algorithm and constants that survive as a SPECIFICATION to be
// rewritten. No binary layout here.
//
// THE SIZE OF THE JOB - FROM A MEASUREMENT. Metin2 uses TWO
// texture stages (numbers 0 and 1), not the eight Direct3D 8 offered. It
// uses eight blend operations, of which four are 219 of all 226
// occurrences, and four argument sources. The program key is therefore
// small and countable. The generator nevertheless handles the WHOLE set of
// operations from `d3d8.h`, because the cost is one line per operation and
// a missing operation is a silent image error in one place of the game
// that nobody will find.

#pragma once

#include "win32_compat.h"
#include "d3d8.h"

#include <string>

/// How many texture stages the generator composes. Two - see the
/// measurement above. Raising it needs no change to the composing code,
/// only a bigger array: `Argument` and `Operation` do not know which stage
/// they serve.
enum { M2W_TEXTURE_STAGES = 2 };

/// The state of one texture stage - the subset of `D3DTSS_*` that AFFECTS
/// the program. Filtering and wrapping (`MINFILTER`, `ADDRESSU`) are not
/// here: in OpenGL they belong to the texture or the sampler object, not
/// to the program - a filter change has no right to rebuild a shader.
struct TTextureStage
{
    DWORD dwColorOp;
    DWORD dwColorArg1;
    DWORD dwColorArg2;
    DWORD dwAlphaOp;
    DWORD dwAlphaArg1;
    DWORD dwAlphaArg2;

    /// `D3DTSS_TEXCOORDINDEX`: the low bits are the coordinate set number,
    /// the high ones (`D3DTSS_TCI_*`) say the coordinates are to be
    /// GENERATED from the position, the normal or the reflection vector.
    DWORD dwTexCoordIndex;

    /// `D3DTSS_TEXTURETRANSFORMFLAGS` - how many components to take after
    /// the texture matrix (`D3DTTFF_COUNT2` and so on).
    DWORD dwTextureTransformFlags;

    /// Whether a texture is bound to this stage at all. Direct3D treated
    /// `D3DTA_TEXTURE` without a texture as white - and that has to be
    /// reproduced, because the client relies on it when drawing with the
    /// vertex colour alone.
    bool bTextureBound;
};

/// Everything that determines a program. Two equal keys must give the same
/// code - the program cache in the layer above stands on that.
struct TPipelineKey
{
    TTextureStage stage[M2W_TEXTURE_STAGES];

    // --- vertex format (from the FVF code) ---
    /// `D3DFVF_XYZRHW` - the vertices are ALREADY transformed, in screen
    /// pixels. The whole user interface uses this.
    bool bTransformed;
    bool bNormal;
    bool bDiffuse;
    bool bSpecular;
    /// How many texture coordinate sets the vertex carries (0..3).
    int  iTexCoordSets;

    // --- render states that change the PROGRAM ---
    bool  bLighting;             // D3DRS_LIGHTING
    bool  bColorVertex;          // D3DRS_COLORVERTEX
    bool  bFog;                  // D3DRS_FOGENABLE
    DWORD dwFogMode;             // D3DFOG_* (vertex or pixel fog)
    bool  bAlphaTest;            // D3DRS_ALPHATESTENABLE
    DWORD dwAlphaFunc;           // D3DCMP_*

    /// `D3DRS_SPECULARENABLE`. `D3DFVF_SPECULAR` in the vertex format alone
    /// is NOT ENOUGH for the highlight to reach the image - Direct3D added
    /// it only with this state, and it is OFF by default.
    ///
    /// Not a detail. The one place in Metin2 that carries `D3DFVF_SPECULAR`
    /// is the software-transformed terrain (`MapOutdoorRenderSTP.cpp`), and
    /// it writes the FOG COLOUR there (`dwSpecular = dwFog`) - no highlight
    /// at all. `StateManager` sets `D3DRS_SPECULARENABLE` to `FALSE` once,
    /// at start, and never undoes it, so Direct3D SIMPLY SKIPPED that field
    /// and the terrain blended its fog in a separate pass by itself. Adding
    /// the highlight unconditionally would brighten the whole terrain by
    /// the fog colour - the image would draw, nothing would blow up, only
    /// the world would be washed out.
    bool  bSpecularEnable;
};

/// Resets the key to Direct3D 8's INITIAL state - not to plain zeros. The
/// defaults are part of the contract: after `Reset` the driver had stage 0
/// at `MODULATE(TEXTURE, DIFFUSE)` and every later stage disabled.
void M2W_DefaultPipelineKey(TPipelineKey* pKey);

/// Composes the vertex program. GLSL ES 3.00, i.e. WebGL 2.
std::string M2W_BuildVertexProgram(const TPipelineKey& c_rKey);

/// Composes the pixel program.
std::string M2W_BuildPixelProgram(const TPipelineKey& c_rKey);

/// A number that describes the key - for the cache of finished programs.
/// Different keys may give the same hash; the cache must compare keys, not
/// trust the hash.
DWORD M2W_KeyHash(const TPipelineKey& c_rKey);
