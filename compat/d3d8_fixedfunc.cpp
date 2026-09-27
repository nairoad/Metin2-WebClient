// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_fixedfunc.cpp - composing GLSL from the Direct3D 8 fixed-function
// pipeline: the rules below are the DIRECT3D 8 SPECIFICATION rewritten in
// GLSL, every blend operation with the formula it used. See d3d8_fixedfunc.h.

// Design: category B - the algorithm survives, no binary layout
// is involved. The vertex program is composed in five parts (declarations,
// position, vertex colour, texture coordinates, fog), the pixel program in
// four (declarations, samples, the stage chain, the finish); the GLSL text
// for a fixed set of 1141 keys is the gate `tools/gates/check_glsl.py`.

#include "d3d8_fixedfunc.h"

#include <cstdio>

namespace
{

// ---------------------------------------------------------------------------
// Arguments - `D3DTA_*`
// ---------------------------------------------------------------------------
// The low three bits say WHERE to take the value from; two more are
// modifiers:
//   `D3DTA_COMPLEMENT`     (0x10) - take 1-x
//   `D3DTA_ALPHAREPLICATE` (0x20) - spread alpha over every component
// The order is defined by Direct3D: FIRST the complement, THEN the alpha
// replication. The other way round would give another result for
// `COMPLEMENT|ALPHAREPLICATE`.

/// The GLSL variable holding the given source.
/// NOTE stage 0: in Direct3D `D3DTA_CURRENT` on the first stage means the
/// same as `D3DTA_DIFFUSE` - there is no "previous" result yet. The client
/// relies on it.
std::string Source(DWORD dwArg, int iStage)
{
    switch (dwArg & 0x07)
    {
        case D3DTA_DIFFUSE:  return "vertexColor";
        case D3DTA_CURRENT:  return (iStage == 0) ? "vertexColor" : "current";
        case D3DTA_TEXTURE:  return "sample" + std::to_string(iStage);
        case D3DTA_TFACTOR:  return "uTextureFactor";
        case D3DTA_SPECULAR: return "specularColor";
        case D3DTA_TEMP:     return "temp";
        default:             return "vertexColor";
    }
}

/// The full argument, modifiers included.
std::string Argument(DWORD dwArg, int iStage)
{
    std::string s = Source(dwArg, iStage);

    if (dwArg & D3DTA_COMPLEMENT)
        s = "(vec4(1.0) - " + s + ")";

    if (dwArg & D3DTA_ALPHAREPLICATE)
        s = "vec4(" + s + ".a)";

    return s;
}

// ---------------------------------------------------------------------------
// Operations - `D3DTOP_*`
// ---------------------------------------------------------------------------
// `bAlpha` says whether the colour or the alpha channel is composed. Most
// operations compute the same for both; only the ones that by design MIX
// colour with alpha (`MODULATEALPHA_ADDCOLOR` and siblings) differ - they
// make sense for colour only and for alpha reduce to the first argument.
/// GLSL expression of texture-stage operation `dwOp` on the argument
/// expressions `a1`, `a2`; an unknown operation yields `a1`.
std::string Operation(DWORD dwOp, const std::string& a1, const std::string& a2,
                      bool bAlpha)
{
    switch (dwOp)
    {
        case D3DTOP_SELECTARG1:  return a1;
        case D3DTOP_SELECTARG2:  return a2;

        case D3DTOP_MODULATE:    return "(" + a1 + " * " + a2 + ")";
        case D3DTOP_MODULATE2X:  return "(" + a1 + " * " + a2 + " * 2.0)";
        case D3DTOP_MODULATE4X:  return "(" + a1 + " * " + a2 + " * 4.0)";

        case D3DTOP_ADD:         return "(" + a1 + " + " + a2 + ")";
        // ADDSIGNED shifts the second argument by -0.5 so that it can subtract.
        case D3DTOP_ADDSIGNED:   return "(" + a1 + " + " + a2 + " - 0.5)";
        case D3DTOP_SUBTRACT:    return "(" + a1 + " - " + a2 + ")";

        // Blending by weight - the weight is the alpha of the given source.
        case D3DTOP_BLENDDIFFUSEALPHA:
            return "mix(" + a2 + ", " + a1 + ", vertexColor.a)";
        case D3DTOP_BLENDTEXTUREALPHA:
            return "mix(" + a2 + ", " + a1 + ", SAMPLE_OF_THIS_STAGE.a)";
        case D3DTOP_BLENDFACTORALPHA:
            return "mix(" + a2 + ", " + a1 + ", uTextureFactor.a)";

        // Four operations joining colour with alpha. Formulas straight from
        // the specification:
        //   MODULATEALPHA_ADDCOLOR:    arg1.rgb + arg1.a * arg2.rgb
        //   MODULATECOLOR_ADDALPHA:    arg1.rgb * arg2.rgb + arg1.a
        //   MODULATEINVALPHA_ADDCOLOR: (1-arg1.a) * arg2.rgb + arg1.rgb
        //   MODULATEINVCOLOR_ADDALPHA: (1-arg1.rgb) * arg2.rgb + arg1.a
        case D3DTOP_MODULATEALPHA_ADDCOLOR:
            if (bAlpha) return a1;
            return "vec4(" + a1 + ".rgb + " + a1 + ".a * " + a2 + ".rgb, 1.0)";
        case D3DTOP_MODULATECOLOR_ADDALPHA:
            if (bAlpha) return a1;
            return "vec4(" + a1 + ".rgb * " + a2 + ".rgb + " + a1 + ".a, 1.0)";
        case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
            if (bAlpha) return a1;
            return "vec4((1.0 - " + a1 + ".a) * " + a2 + ".rgb + " + a1 + ".rgb, 1.0)";
        case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
            if (bAlpha) return a1;
            return "vec4((vec3(1.0) - " + a1 + ".rgb) * " + a2 + ".rgb + " + a1 + ".a, 1.0)";

        default:
            // `D3DTOP_DISABLE` does not get here - the caller handles it,
            // because disabling a stage means "break the chain", not
            // "compute".
            return a1;
    }
}

/// Substitutes the name of this stage's sample where it is needed.
std::string SubstituteSample(std::string s, int iStage)
{
    const std::string strPlaceholder = "SAMPLE_OF_THIS_STAGE";
    const std::string strName = "sample" + std::to_string(iStage);
    size_t uPos;
    while ((uPos = s.find(strPlaceholder)) != std::string::npos)
        s.replace(uPos, strPlaceholder.size(), strName);
    return s;
}

/// The comparison of the alpha test - `D3DCMP_*`. Returns the condition
/// for REJECTING the pixel, i.e. the negation of the pass condition.
std::string AlphaRejection(DWORD dwFunc)
{
    switch (dwFunc)
    {
        case D3DCMP_NEVER:        return "true";
        case D3DCMP_LESS:         return "!(result.a <  uAlphaRef)";
        case D3DCMP_EQUAL:        return "!(result.a == uAlphaRef)";
        case D3DCMP_LESSEQUAL:    return "!(result.a <= uAlphaRef)";
        case D3DCMP_GREATER:      return "!(result.a >  uAlphaRef)";
        case D3DCMP_NOTEQUAL:     return "!(result.a != uAlphaRef)";
        case D3DCMP_GREATEREQUAL: return "!(result.a >= uAlphaRef)";
        case D3DCMP_ALWAYS:       return "false";
        default:                  return "false";
    }
}

// ---------------------------------------------------------------------------
// The vertex program, in parts (pure code motion; the text
// comes out the same, check_glsl.py)
// ---------------------------------------------------------------------------

/// Inputs, uniforms and outputs of the vertex program.
void VertexDeclarations(const TPipelineKey& c_rKey, std::string& s)
{
    s += "#version 300 es\n";
    s += "precision highp float;\n";

    s += "in vec4 aPosition;\n";
    if (c_rKey.bNormal)  s += "in vec3 aNormal;\n";
    // NOTE `.bgra` at EVERY use of these two inputs. `D3DFVF_DIFFUSE` and
    // `D3DFVF_SPECULAR` are `D3DCOLOR` words (0xAARRGGBB), which in
    // little-endian memory lie as B, G, R, A. The attribute goes to OpenGL
    // WITHOUT conversion - there are orders of magnitude more vertices
    // than textures and walking them in wasm would be waste - so the
    // shader fixes the order. It costs nothing.
    //
    // Without it a character's skin would be blue and the sky red. The
    // image draws, nothing blows up. The same trap as `A8R8G8B8` in
    // `d3d8_states.cpp`, only settled differently - and precisely because
    // differently, easy to forget.
    if (c_rKey.bDiffuse)   s += "in vec4 aColor;\n";
    if (c_rKey.bSpecular)  s += "in vec4 aSpecular;\n";
    for (int i = 0; i < c_rKey.iTexCoordSets; ++i)
        s += "in vec4 aTexCoord" + std::to_string(i) + ";\n";

    s += "uniform mat4 uWorldViewProjection;\n";
    s += "uniform mat4 uWorldView;\n";
    s += "uniform mat4 uTextureMatrix[2];\n";
    // The orthographic projection of ALREADY transformed vertices
    // (`XYZRHW`) - then the coordinates come in screen pixels.
    s += "uniform vec2 uTargetSize;\n";
    s += "uniform float uHalfPixel;\n";  // 0.5 / GUI scale
    s += "uniform float uFlipY;\n";  // -1 when drawing to a texture
    s += "uniform vec4 uLightDirection;\n";
    s += "uniform vec4 uLightColor;\n";
    s += "uniform vec4 uLightAmbient;\n";
    s += "uniform vec4 uMaterialDiffuse;\n";
    s += "uniform vec4 uMaterialEmissive;\n";
    s += "uniform vec4 uMaterialAmbient;\n";
    s += "uniform vec4 uGlobalAmbient;\n";
    s += "uniform vec2 uFogRange;\n";       // start, end
    s += "uniform float uFogDensity;\n";

    s += "out vec4 vColor;\n";
    s += "out vec4 vSpecular;\n";
    s += "out vec2 vTexCoord0;\n";
    s += "out vec2 vTexCoord1;\n";
    s += "out float vFog;\n";
}

/// `gl_Position` and `viewDistance` - the pixel road for `XYZRHW`, the
/// matrix road otherwise.
void VertexPosition(const TPipelineKey& c_rKey, std::string& s)
{
    if (c_rKey.bTransformed)
    {
        // `D3DFVF_XYZRHW`: the coordinates are already screen pixels, from
        // the TOP LEFT corner. OpenGL counts from the bottom LEFT and wants
        // the range [-1,1], hence the flipped Y axis. The same conversion
        // the driver itself did in Direct3D.
        // HALF A PIXEL: in Direct3D 8/9 the pixel
        // centre lies on the INTEGER coordinate (0,0 = the centre of the
        // top-left pixel), in OpenGL on (0.5, 0.5). The game code computes
        // for D3D (e.g. draws text at `fCurX - 0.5f`), so without this
        // offset every UI quad lies half a pixel off the grid and the
        // bilinear filter blends neighbouring texels: blurred letters,
        // "torn" window frames (measured 6x: grey, doubled glyph edges).
        s += "  vec2 p = (aPosition.xy + uHalfPixel) / uTargetSize;\n";
        s += "  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, aPosition.z, 1.0);\n";
        s += "  float viewDistance = aPosition.w;\n";
    }
    else
    {
        s += "  gl_Position = uWorldViewProjection * vec4(aPosition.xyz, 1.0);\n";
        s += "  float viewDistance = -(uWorldView * vec4(aPosition.xyz, 1.0)).z;\n";
    }
    s += "  gl_Position.y *= uFlipY;\n";
}

/// `vColor` and `vSpecular`: without lighting the colour comes straight
/// from the vertex (or white when there is none); with lighting the
/// Lambert model with one directional light and an ambient term - as much
/// as the client sets (`SetLight` occurs eight times, `LightEnable` five).
void VertexColor(const TPipelineKey& c_rKey, std::string& s)
{
    if (c_rKey.bLighting && c_rKey.bNormal)
    {
        s += "  vec3 n = normalize(mat3(uWorldView) * aNormal);\n";
        s += "  float lam = max(dot(n, -normalize(uLightDirection.xyz)), 0.0);\n";
        s += "  vec4 base = " +
             std::string(c_rKey.bDiffuse && c_rKey.bColorVertex
                         ? "aColor.bgra" : "uMaterialDiffuse") + ";\n";
        // THE FULL DIRECT3D 8 FORMULA - not part of it.
        //
        // Earlier ONLY `diffuse * (light_ambient + light_colour *
        // angle)` was computed. Direct3D 8 computes:
        //
        //     emissive
        //   + material_ambient * global_ambient          (D3DRS_AMBIENT)
        //   + material_diffuse * light_diffuse * angle
        //   + material_ambient * light_ambient
        //
        // EMISSIVE and MATERIAL AMBIENT were missing. Not a detail: the map
        // environment in Metin2 sets emissive to 0.8 by default
        // (GameLib/MapUtil.cpp:26), so in Direct3D the terrain has
        // brightness 0.8 REGARDLESS of the angle of incidence. Here, when
        // the angle came out zero, black remained - and the terrain shadow
        // pass multiplies the screen by it. Hence the black surroundings of
        // the character with a correct distance.
        //
        // Found after two missed fixes and one experiment (`?shadow=0`), which
        // pointed at the right pass. The formula compared with the game
        // code, not guessed.
        s += "  vec3 lighting = uMaterialEmissive.rgb"
             " + uMaterialAmbient.rgb * uGlobalAmbient.rgb"
             " + base.rgb * uLightColor.rgb * lam"
             " + uMaterialAmbient.rgb * uLightAmbient.rgb;\n";
        s += "  vColor = vec4(clamp(lighting, 0.0, 1.0), base.a);\n";
    }
    else if (c_rKey.bDiffuse)
    {
        s += "  vColor = aColor.bgra;\n";
    }
    else
    {
        s += "  vColor = vec4(1.0);\n";
    }

    s += c_rKey.bSpecular ? "  vSpecular = aSpecular.bgra;\n"
                          : "  vSpecular = vec4(0.0);\n";
}

/// `vTexCoordN` for every stage: a coordinate set of the vertex or
/// coordinates GENERATED from the position, the normal or the reflection
/// vector (`D3DTSS_TCI_*`), through the texture matrix when
/// `D3DTSS_TEXTURETRANSFORMFLAGS` says so.
void VertexTexCoords(const TPipelineKey& c_rKey, std::string& s)
{
    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        const TTextureStage& r = c_rKey.stage[i];
        const DWORD dwTci = r.dwTexCoordIndex & 0xFFFF0000u;
        const int iSet = static_cast<int>(r.dwTexCoordIndex & 0x0000FFFFu);

        std::string strSource;
        switch (dwTci)
        {
            case D3DTSS_TCI_CAMERASPACEPOSITION:
                // Coordinates from the POSITION in camera space - Metin2
                // does environment projection and terrain shadows with it
                // (11 uses).
                strSource = "vec4((uWorldView * vec4(aPosition.xyz, 1.0)).xyz, 1.0)";
                break;
            case D3DTSS_TCI_CAMERASPACENORMAL:
                strSource = c_rKey.bNormal
                            ? "vec4(mat3(uWorldView) * aNormal, 1.0)"
                            : "vec4(0.0, 0.0, 1.0, 1.0)";
                break;
            case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:
                strSource = c_rKey.bNormal
                            ? "vec4(reflect(normalize((uWorldView * vec4(aPosition.xyz, 1.0)).xyz),"
                              " normalize(mat3(uWorldView) * aNormal)), 1.0)"
                            : "vec4(0.0, 0.0, 1.0, 1.0)";
                break;
            default:
                strSource = (iSet < c_rKey.iTexCoordSets)
                            ? ("aTexCoord" + std::to_string(iSet))
                            : "vec4(0.0)";
                break;
        }

        const bool bTransform = (r.dwTextureTransformFlags & 0xFF) != D3DTTFF_DISABLE;
        const std::string strExpr = bTransform
            ? ("(uTextureMatrix[" + std::to_string(i) + "] * " + strSource + ").xy")
            : (strSource + ".xy");

        s += "  vTexCoord" + std::to_string(i) + " = " + strExpr + ";\n";
    }
}

/// `vFog`: factor 1 means "the object's full colour", 0 "fog alone". The
/// formulas are the ones Direct3D used.
void VertexFog(const TPipelineKey& c_rKey, std::string& s)
{
    if (c_rKey.bFog && c_rKey.bTransformed)
    {
        // VERTICES ALREADY TRANSFORMED: there is NO distance from the camera.
        //
        // `aPosition.w` is with `D3DFVF_XYZRHW` the reciprocal of `w`, not
        // a distance - exactly that used to stand here and would give a
        // meaningless number. Direct3D computed nothing in this case: it
        // took THE FOG FACTOR FROM THE SPECULAR ALPHA, which the program
        // had to write itself. Without `D3DFVF_SPECULAR` there was no fog
        // for such vertices at all.
        //
        // In today's Metin2 no place switches this road on (the
        // software-transformed terrain blends fog in a separate pass with
        // `D3DRS_FOGENABLE` at `FALSE`), so this is not a fix of something
        // visible - it closes a road that used to return garbage.
        s += c_rKey.bSpecular ? "  vFog = aSpecular.a;\n" : "  vFog = 1.0;\n";
        s += "  vFog = clamp(vFog, 0.0, 1.0);\n";
    }
    else if (c_rKey.bFog)
    {
        switch (c_rKey.dwFogMode)
        {
            case D3DFOG_EXP:
                s += "  vFog = exp(-uFogDensity * viewDistance);\n";
                break;
            case D3DFOG_EXP2:
                s += "  vFog = exp(-pow(uFogDensity * viewDistance, 2.0));\n";
                break;
            case D3DFOG_LINEAR:
            default:
                s += "  vFog = (uFogRange.y - viewDistance)"
                     " / max(uFogRange.y - uFogRange.x, 0.0001);\n";
                break;
        }
        s += "  vFog = clamp(vFog, 0.0, 1.0);\n";
    }
    else
    {
        s += "  vFog = 1.0;\n";
    }
}

// ---------------------------------------------------------------------------
// The pixel program, in parts (pure code motion)
// ---------------------------------------------------------------------------

/// Inputs, uniforms and the output.
void PixelDeclarations(std::string& s)
{
    s += "#version 300 es\n";
    s += "precision highp float;\n";

    s += "in vec4 vColor;\n";
    s += "in vec4 vSpecular;\n";
    s += "in vec2 vTexCoord0;\n";
    s += "in vec2 vTexCoord1;\n";
    s += "in float vFog;\n";

    s += "uniform sampler2D uTexture0;\n";
    s += "uniform sampler2D uTexture1;\n";
    s += "uniform vec4 uTextureFactor;\n";
    s += "uniform vec4 uFogColor;\n";
    s += "uniform float uAlphaRef;\n";

    s += "out vec4 outColor;\n";
}

/// The three working colours and `sampleN` for every stage.
void PixelSamples(const TPipelineKey& c_rKey, std::string& s)
{
    s += "  vec4 vertexColor = vColor;\n";
    s += "  vec4 specularColor = vSpecular;\n";
    s += "  vec4 temp = vec4(0.0);\n";

    // Samples. A stage without a bound texture gives WHITE - Direct3D
    // behaved so and the client relies on it, drawing with the vertex
    // colour alone.
    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        s += "  vec4 sample" + std::to_string(i) + " = ";
        s += c_rKey.stage[i].bTextureBound
             ? ("texture(uTexture" + std::to_string(i) + ", vTexCoord" + std::to_string(i) + ");\n")
             : "vec4(1.0);\n";
    }
}

/// The stage chain: `colorN`, `alphaN`, `current`.
void PixelStageChain(const TPipelineKey& c_rKey, std::string& s)
{
    s += "  vec4 current = vertexColor;\n";

    // --- the stage chain ---------------------------------------------------
    // COLOUR AND ALPHA ARE TWO SEPARATE CHAINS (fixed).
    //
    // Earlier this read `if (dwColorOp == D3DTOP_DISABLE) break;` - a
    // disabled colour broke the WHOLE stage, alpha included. It sounded
    // like a Direct3D rule and bothered nobody for thirty parts, because
    // until then no pass computed alpha in a stage with disabled colour.
    //
    // THE TERRAIN DOES. `MapOutdoorRenderSTP.cpp:200` draws the blend layers
    // so:
    //
    //     SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    //     SetTexture(0, ground texture);
    //     SetTexture(1, blend map);
    //
    // The colour of stage 1 stays DISABLED from the previous pass - because
    // that stage adds nothing to the colour. It adds ALPHA, and alpha
    // decides where a given ground layer is visible at all.
    //
    // Under the old rule stage 1 dropped out entirely: the terrain shader
    // had `uTexture0` but NO `uTexture1`. The blend map was bound to the
    // card and never sampled, so alpha came from the vertex - and there it
    // is 3 of 255. The ground layers drew with one percent opacity and only
    // the background remained.
    //
    // Symptom: TERRAIN INVISIBLE. No error, no OpenGL warning - the
    // geometry, the ground texture and the blend map were CORRECT, each
    // checked on its own. Only their combination failed.
    //
    // The chain now ends only when a stage adds NEITHER colour NOR alpha.
    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        const TTextureStage& r = c_rKey.stage[i];

        const bool bColorActive = (r.dwColorOp != D3DTOP_DISABLE);
        const bool bAlphaActive = (r.dwAlphaOp != D3DTOP_DISABLE);

        if (!bColorActive && !bAlphaActive)
            break;

        if (bColorActive)
        {
            const std::string c1 = Argument(r.dwColorArg1, i);
            const std::string c2 = Argument(r.dwColorArg2, i);
            s += "  vec4 color" + std::to_string(i) + " = " +
                 SubstituteSample(Operation(r.dwColorOp, c1, c2, false), i) + ";\n";
        }
        else
        {
            // The stage adds no colour - `current` stays UNTOUCHED. Not the
            // same as black: `D3DTOP_DISABLE` on colour means "skip", not
            // "zero".
            s += "  vec4 color" + std::to_string(i) + " = current;\n";
        }

        if (bAlphaActive)
        {
            const std::string a1 = Argument(r.dwAlphaArg1, i);
            const std::string a2 = Argument(r.dwAlphaArg2, i);
            s += "  float alpha" + std::to_string(i) + " = (" +
                 SubstituteSample(Operation(r.dwAlphaOp, a1, a2, true), i) + ").a;\n";
        }
        else
        {
            // Alpha disabled on its own - the alpha of the previous stage
            // stays, not zero.
            s += "  float alpha" + std::to_string(i) + " = current.a;\n";
        }

        s += "  current = vec4(color" + std::to_string(i) + ".rgb, alpha" +
             std::to_string(i) + ");\n";
    }
}

/// `result`: the highlight, the alpha test and the fog.
void PixelFinish(const TPipelineKey& c_rKey, std::string& s)
{
    s += "  vec4 result = current;\n";

    // The highlight is added AFTER the stage chain and only to the colour -
    // alpha stays.
    //
    // A FIX FROM READING THE CLIENT: `bSpecular` alone stood here,
    // i.e. "the vertex format carries a highlight, so add it". Direct3D did
    // not: it added it only with `D3DRS_SPECULARENABLE`, which is OFF by
    // default and Metin2 switches off once more explicitly
    // (`StateManager.cpp:220`), never switching it on.
    //
    // The one place that carries `D3DFVF_SPECULAR` at all is the
    // software-transformed terrain - and it writes the FOG COLOUR there,
    // not a highlight. Without this gate the whole terrain would get the
    // fog colour ADDED on top, i.e. be the brighter the denser the fog. No
    // error, no warning - only a washed-out world.
    if (c_rKey.bSpecular && c_rKey.bSpecularEnable)
        s += "  result = vec4(result.rgb + specularColor.rgb, result.a);\n";

    // --- alpha test --------------------------------------------------------
    // Must be BEFORE the fog: Direct3D rejected the pixel on the alpha
    // computed by the stages, and fog does not change alpha. The order
    // matters for tree foliage and grates, where the rejection is the
    // whole content of the image.
    if (c_rKey.bAlphaTest)
    {
        s += "  if (" + AlphaRejection(c_rKey.dwAlphaFunc) + ") discard;\n";
    }

    if (c_rKey.bFog)
        s += "  result = vec4(mix(uFogColor.rgb, result.rgb, vFog), result.a);\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

void M2W_DefaultPipelineKey(TPipelineKey* pKey)
{
    if (!pKey)
        return;

    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        TTextureStage& r = pKey->stage[i];

        // Direct3D after `Reset`: stage 0 is MODULATE(TEXTURE, DIFFUSE),
        // every later one DISABLED. Not our choice - the client counts on
        // it, setting only the differences from this state.
        r.dwColorOp   = (i == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        r.dwAlphaOp   = (i == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        r.dwColorArg1 = D3DTA_TEXTURE;
        r.dwColorArg2 = D3DTA_CURRENT;
        r.dwAlphaArg1 = D3DTA_TEXTURE;
        r.dwAlphaArg2 = D3DTA_CURRENT;

        r.dwTexCoordIndex = static_cast<DWORD>(i);
        r.dwTextureTransformFlags = D3DTTFF_DISABLE;
        r.bTextureBound = false;
    }

    pKey->bTransformed = false;
    pKey->bNormal = false;
    pKey->bDiffuse = false;
    pKey->bSpecular = false;
    pKey->iTexCoordSets = 0;

    pKey->bLighting = true;         // D3DRS_LIGHTING is ON by default
    pKey->bColorVertex = false;
    pKey->bFog = false;
    pKey->dwFogMode = D3DFOG_NONE;
    pKey->bAlphaTest = false;
    pKey->dwAlphaFunc = D3DCMP_ALWAYS;
    // `D3DRS_SPECULARENABLE` is OFF by default - see the header.
    pKey->bSpecularEnable = false;
}

// ---------------------------------------------------------------------------
// The vertex program
// ---------------------------------------------------------------------------

std::string M2W_BuildVertexProgram(const TPipelineKey& c_rKey)
{
    std::string s;
    VertexDeclarations(c_rKey, s);
    s += "void main() {\n";
    VertexPosition(c_rKey, s);
    VertexColor(c_rKey, s);
    VertexTexCoords(c_rKey, s);
    VertexFog(c_rKey, s);
    s += "}\n";
    return s;
}

// ---------------------------------------------------------------------------
// The pixel program
// ---------------------------------------------------------------------------

std::string M2W_BuildPixelProgram(const TPipelineKey& c_rKey)
{
    std::string s;
    PixelDeclarations(s);
    s += "void main() {\n";
    PixelSamples(c_rKey, s);
    PixelStageChain(c_rKey, s);
    PixelFinish(c_rKey, s);
    s += "  outColor = result;\n";
    s += "}\n";
    return s;
}

// ---------------------------------------------------------------------------
// The key hash
// ---------------------------------------------------------------------------

DWORD M2W_KeyHash(const TPipelineKey& c_rKey)
{
    // Plain FNV-1a over the key's bytes. The struct is all numbers and
    // booleans, no pointers - so its bytes ARE its content.
    //
    // NOTE: the padding between fields may be garbage, so the program cache
    // must compare keys field by field, not trust this hash. The header
    // says so outright.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&c_rKey);
    DWORD h = 2166136261u;
    for (size_t i = 0; i < sizeof(TPipelineKey); ++i)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
