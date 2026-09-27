// SPDX-License-Identifier: GPL-2.0-or-later
// gl_shaders.cpp - programs: `CompileShader` (one GLSL source composed by
// `d3d8_fixedfunc.cpp` -> an OpenGL shader object) and the program half of
// `CGlDevice` - the Direct3D 8 shader handles and constants, the
// pipeline key and the program cache (`BuildKey`, `FindOrBuildProgram`),
// and the per-draw uniforms (`ApplyUniforms`).

// Design:
// A draw needs a program for the current `TPipelineKey`: `BuildKey`
// builds the key from the recorded state, `FindOrBuildProgram` finds or
// compiles the program (cache per key hash, plus the last result),
// `ApplyUniforms` sends the uniforms. Real Direct3D 8 shaders are not
// translated - see "WHAT IS NOT HERE YET" at the top of gl_device.cpp.
//
// `CompileShader` split out of gl_device.cpp, the CGlDevice
// methods (group 7) - a pure code move, no body changed its
// content (checked token by token). `ApplyUniforms` split into stages
//. Translated.

#include "gl_internal.h"
#include "frame_stats.h"
#include "stubs.h"

#include <cstdio>
#include <cstring>

GLuint CompileShader(GLenum eKind, const std::string& c_rSource)
{
    const GLuint u = glCreateShader(eKind);
    const char* p = c_rSource.c_str();
    glShaderSource(u, 1, &p, NULL);
    glCompileShader(u);

    GLint iOk = 0;
    glGetShaderiv(u, GL_COMPILE_STATUS, &iOk);
    if (!iOk)
    {
        // Prints the WHOLE source together with the error. The shader is
        // composed by machine, so the driver's message alone ("error in line
        // 34") says nothing - one has to see what stands there.
        char szLog[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(u, sizeof(szLog), &n, szLog);
        std::printf("m2w: shader did not compile: %s\n---\n%s\n---\n",
                    szLog, c_rSource.c_str());
        glDeleteShader(u);
        return 0;
    }
    return u;
}

// ===========================================================================
// CGlDevice methods (from gl_device.cpp)
// ===========================================================================

int CGlDevice::CachedProgramCount() const
{
    int n = 0;
    for (std::map<DWORD, std::vector<TProgramCacheEntry> >::const_iterator it =
             m_kProgramCache.begin(); it != m_kProgramCache.end(); ++it)
        n += static_cast<int>(it->second.size());
    return n;
}

// -----------------------------------------------------------------------
// Shaders - see the note at the top of gl_device.cpp
// -----------------------------------------------------------------------

HRESULT CGlDevice::CreateVertexShader(CONST DWORD* pDeclaration, CONST DWORD* pFunction,
                                      DWORD* pHandle, DWORD)
{
    if (!pHandle)
        return E_FAIL;

    // Without a function body this is only a vertex format DECLARATION -
    // Direct3D 8's trick for multiple streams. With a body it is a real
    // assembly shader, which we cannot translate.
    m_dwNextShaderHandle += 1;
    *pHandle = m_dwNextShaderHandle | SHADER_HANDLE_BIT;
    if (pFunction)
        m_kShadersWithCode.insert(*pHandle);

    // AN EARLIER BUG, FIXED. The first argument here had NO NAME, so the
    // declaration went into the bin. `SetVertexShader` then saw only the
    // handle with the top bit set and - correctly - did not take it for an
    // FVF code, so the draw used the vertex layout LEFT BY THE PREVIOUS
    // call.
    //
    // That hit the character models: `CGraphicDevice` describes them
    // exactly by a declaration (`CreatePNTStreamVertexShader`), and
    // `ModelInstanceRender.cpp` draws through `SetVertexShader(ms_pntVS)`
    // in six places. Characters would read positions from offsets
    // belonging to something else entirely - without a single OpenGL
    // error, because from the card's point of view everything was fine.
    if (pDeclaration)
        m_kDeclarationLayouts[*pHandle] = M2W_LayoutFromDeclaration(pDeclaration);

    // The container changed - the last-result memory is lost.
    ForgetPipelineCache();
    return D3D_OK;
}

HRESULT CGlDevice::SetVertexShader(DWORD Handle)
{
    // In Direct3D 8 the same method takes an FVF CODE or a shader handle.
    // A bit tells them apart - handles have the top one set.
    m_dwVertexShader = Handle;
    if (!(Handle & SHADER_HANDLE_BIT))
        m_dwFVF = Handle;
    return D3D_OK;
}

HRESULT CGlDevice::GetVertexShader(DWORD* pHandle)
{
    if (!pHandle)
        return E_FAIL;
    *pHandle = m_dwVertexShader;
    return D3D_OK;
}

HRESULT CGlDevice::DeleteVertexShader(DWORD Handle)
{
    m_kShadersWithCode.erase(Handle);
    m_kDeclarationLayouts.erase(Handle);

    // The deleted handle may have been the remembered one - the memory is
    // lost.
    ForgetPipelineCache();
    return D3D_OK;
}

// THE SHADER CONSTANTS ROAD - silent earlier.
//
// Drawing with a real shader is SKIPPED here, and one warning in
// `PrepareDrawFromBuffer` says so. But the shader constants themselves went
// in complete silence: the client set them, we returned `D3D_OK`, and
// nothing in any log said that this road was used at all.
//
// Now they report themselves through the registry, so one screenshot shows
// whether the game really tries to draw with its own shaders - and how often.
HRESULT CGlDevice::SetVertexShaderConstant(DWORD, CONST void*, DWORD)
{
    M2W_STUB("D3D8::SetVertexShaderConstant");
    return D3D_OK;
}

HRESULT CGlDevice::CreatePixelShader(CONST DWORD*, DWORD* pHandle)
{
    // We hand out a NON-ZERO HANDLE to a shader that will never work - on
    // purpose, because a refusal would stop the client already at creation.
    // But that means the client leaves here convinced of success, so the
    // report matters all the more here.
    M2W_STUB("D3D8::CreatePixelShader (handle without a shader)");
    if (!pHandle)
        return E_FAIL;
    m_dwNextShaderHandle += 1;
    *pHandle = m_dwNextShaderHandle | SHADER_HANDLE_BIT;
    return D3D_OK;
}

HRESULT CGlDevice::SetPixelShader(DWORD Handle)
{
    m_dwPixelShader = Handle;
    return D3D_OK;
}

HRESULT CGlDevice::DeletePixelShader(DWORD)
{
    M2W_STUB("D3D8::DeletePixelShader");
    return D3D_OK;
}

HRESULT CGlDevice::SetPixelShaderConstant(DWORD, CONST void*, DWORD)
{
    M2W_STUB("D3D8::SetPixelShaderConstant");
    return D3D_OK;
}

// --- building the key from the current state ----------------------------

void CGlDevice::BuildKey(const TVertexLayout& c_rLayout)
{
    M2W_DefaultPipelineKey(&m_kKey);

    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        TTextureStage& r = m_kKey.stage[i];
        const DWORD* a = m_aadwStageState[i];

        r.dwColorOp = a[D3DTSS_COLOROP] ? a[D3DTSS_COLOROP] : D3DTOP_DISABLE;
        r.dwColorArg1 = a[D3DTSS_COLORARG1];
        r.dwColorArg2 = a[D3DTSS_COLORARG2];
        r.dwAlphaOp = a[D3DTSS_ALPHAOP] ? a[D3DTSS_ALPHAOP] : D3DTOP_DISABLE;
        r.dwAlphaArg1 = a[D3DTSS_ALPHAARG1];
        r.dwAlphaArg2 = a[D3DTSS_ALPHAARG2];
        r.dwTexCoordIndex = a[D3DTSS_TEXCOORDINDEX];
        r.dwTextureTransformFlags = a[D3DTSS_TEXTURETRANSFORMFLAGS];
        r.bTextureBound = (m_apTextures[i] != NULL);

        // `?shadow=1|2|3` - IN THE SHADOW PASS PRETEND THERE IS NO TEXTURE on
        // stage 0 / 1 / both. Direct3D without a texture takes
        // white, so that stage stops multiplying. An experiment, not a fix:
        // the character shadow map measured white, emissive measured 0.32 -
        // and the pass still gives black. What remains is stage 0 (the
        // static shadow map, DXT1 with nine levels) or the vertex colour
        // alone. The switch decides between them in one reload, instead of a
        // fourth blind fix.
        {
            const int iMode = ShadowExperimentMode();
            if (iMode > 0 && IsShadowPass() &&
                (iMode == 3 || iMode == i + 1))
                r.bTextureBound = false;
        }
    }

    m_kKey.bTransformed = c_rLayout.bTransformed;
    m_kKey.bNormal = (c_rLayout.iNormal >= 0);
    m_kKey.bDiffuse = (c_rLayout.iDiffuse >= 0);
    m_kKey.bSpecular = (c_rLayout.iSpecular >= 0);
    m_kKey.iTexCoordSets = c_rLayout.iTexCoordSets;

    // Lighting makes no sense for already transformed vertices - Direct3D
    // skipped it then too.
    m_kKey.bLighting =
        (m_adwRenderState[D3DRS_LIGHTING] != 0) && !c_rLayout.bTransformed;
    m_kKey.bColorVertex = (m_adwRenderState[D3DRS_COLORVERTEX] != 0);
    m_kKey.bFog = (m_adwRenderState[D3DRS_FOGENABLE] != 0);
    m_kKey.dwFogMode = m_adwRenderState[D3DRS_FOGVERTEXMODE];
    m_kKey.bAlphaTest = (m_adwRenderState[D3DRS_ALPHATESTENABLE] != 0);
    m_kKey.dwAlphaFunc = m_adwRenderState[D3DRS_ALPHAFUNC];

    // `D3DFVF_SPECULAR` alone is not enough - see `d3d8_fixedfunc.h`.
    m_kKey.bSpecularEnable =
        (m_adwRenderState[D3DRS_SPECULARENABLE] != 0);
}

bool CGlDevice::KeysEqual(const TPipelineKey& a, const TPipelineKey& b) const
{
    // Comparison FIELD BY FIELD, not `memcmp`. Padding with garbage may lie
    // between the fields of the structure and `memcmp` would call two
    // identical states different - the cache would swell without end and
    // shaders would be composed over and over. The `d3d8_fixedfunc.h`
    // header says so explicitly.
    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        const TTextureStage& x = a.stage[i];
        const TTextureStage& y = b.stage[i];
        if (x.dwColorOp != y.dwColorOp || x.dwColorArg1 != y.dwColorArg1 ||
            x.dwColorArg2 != y.dwColorArg2 || x.dwAlphaOp != y.dwAlphaOp ||
            x.dwAlphaArg1 != y.dwAlphaArg1 || x.dwAlphaArg2 != y.dwAlphaArg2 ||
            x.dwTexCoordIndex != y.dwTexCoordIndex ||
            x.dwTextureTransformFlags != y.dwTextureTransformFlags ||
            x.bTextureBound != y.bTextureBound)
            return false;
    }
    return a.bTransformed == b.bTransformed &&
           a.bNormal == b.bNormal &&
           a.bDiffuse == b.bDiffuse &&
           a.bSpecular == b.bSpecular &&
           a.iTexCoordSets == b.iTexCoordSets &&
           a.bLighting == b.bLighting &&
           a.bColorVertex == b.bColorVertex &&
           a.bFog == b.bFog &&
           a.dwFogMode == b.dwFogMode &&
           a.bAlphaTest == b.bAlphaTest &&
           a.dwAlphaFunc == b.dwAlphaFunc &&
           a.bSpecularEnable == b.bSpecularEnable;
}

namespace
{

/// Links the two compiled shaders into a program and deletes them; the
/// program, or 0 after printing the link log.
GLuint LinkProgram(GLuint uVertexShader, GLuint uPixelShader)
{
    const GLuint uProgram = glCreateProgram();
    glAttachShader(uProgram, uVertexShader);
    glAttachShader(uProgram, uPixelShader);
    glLinkProgram(uProgram);
    glDeleteShader(uVertexShader);
    glDeleteShader(uPixelShader);

    GLint iOk = 0;
    glGetProgramiv(uProgram, GL_LINK_STATUS, &iOk);
    if (!iOk)
    {
        char szLog[1024];
        GLsizei n = 0;
        glGetProgramInfoLog(uProgram, sizeof(szLog), &n, szLog);
        std::printf("m2w: program did not link: %s\n", szLog);
        glDeleteProgram(uProgram);
        return 0;
    }
    return uProgram;
}

/// Fills every attribute and uniform location of `r` from its linked
/// program `r.uProgram` (-1 for names the program does not use).
void LookUpLocations(TProgram& r)
{
    r.iPosition = glGetAttribLocation(r.uProgram, "aPosition");
    r.iNormal  = glGetAttribLocation(r.uProgram, "aNormal");
    r.iColor     = glGetAttribLocation(r.uProgram, "aColor");
    r.iSpecular   = glGetAttribLocation(r.uProgram, "aSpecular");
    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        char szName[16];
        std::snprintf(szName, sizeof(szName), "aTexCoord%d", i);
        r.aiTexCoords[i] = glGetAttribLocation(r.uProgram, szName);
    }

    r.iWorldViewProjection = glGetUniformLocation(r.uProgram, "uWorldViewProjection");
    r.iWorldView = glGetUniformLocation(r.uProgram, "uWorldView");
    r.iTargetSize = glGetUniformLocation(r.uProgram, "uTargetSize");
    r.iHalfPixel = glGetUniformLocation(r.uProgram, "uHalfPixel");
    r.iFlipY = glGetUniformLocation(r.uProgram, "uFlipY");
    r.iTextureMatrix = glGetUniformLocation(r.uProgram, "uTextureMatrix");
    r.iLightDirection = glGetUniformLocation(r.uProgram, "uLightDirection");
    r.iLightColor = glGetUniformLocation(r.uProgram, "uLightColor");
    r.iLightAmbient = glGetUniformLocation(r.uProgram, "uLightAmbient");
    r.iMaterialDiffuse = glGetUniformLocation(r.uProgram, "uMaterialDiffuse");
    r.iMaterialEmissive = glGetUniformLocation(r.uProgram, "uMaterialEmissive");
    r.iMaterialAmbient = glGetUniformLocation(r.uProgram, "uMaterialAmbient");
    r.iGlobalAmbient = glGetUniformLocation(r.uProgram, "uGlobalAmbient");
    r.iFogRange = glGetUniformLocation(r.uProgram, "uFogRange");
    r.iFogDensity = glGetUniformLocation(r.uProgram, "uFogDensity");
    r.iTexture0 = glGetUniformLocation(r.uProgram, "uTexture0");
    r.iTexture1 = glGetUniformLocation(r.uProgram, "uTexture1");
    r.iTextureFactor = glGetUniformLocation(r.uProgram, "uTextureFactor");
    r.iFogColor = glGetUniformLocation(r.uProgram, "uFogColor");
    r.iAlphaRef = glGetUniformLocation(r.uProgram, "uAlphaRef");
}

}  // namespace

const TProgram* CGlDevice::FindOrBuildProgram()
{
    // THE LAST RESULT FIRST. Hundreds of draws in a row go with
    // the same key - one structure comparison then saves the hash over a
    // hundred bytes, the tree descent and the bucket walk.
    if (m_bLastProgramKnown && KeysEqual(m_kLastKey, m_kKey))
        return m_pLastProgram;

    const DWORD dwHash = M2W_KeyHash(m_kKey);
    std::vector<TProgramCacheEntry>& rBucket = m_kProgramCache[dwHash];

    for (size_t i = 0; i < rBucket.size(); ++i)
        if (KeysEqual(rBucket[i].kKey, m_kKey))
        {
            m_kLastKey = m_kKey;
            m_pLastProgram = &rBucket[i].kProgram;
            m_bLastProgramKnown = true;
            return m_pLastProgram;
        }

    m2wstats::TStopwatch kStopwatch(m2wstats::g_kShaderPrograms);
    const GLuint uVertexShader = CompileShader(
        GL_VERTEX_SHADER, M2W_BuildVertexProgram(m_kKey));
    const GLuint uPixelShader = CompileShader(
        GL_FRAGMENT_SHADER, M2W_BuildPixelProgram(m_kKey));
    if (!uVertexShader || !uPixelShader)
        return NULL;

    const GLuint uProgram = LinkProgram(uVertexShader, uPixelShader);
    if (!uProgram)
        return NULL;

    TProgramCacheEntry kEntry;
    kEntry.kKey = m_kKey;
    TProgram& r = kEntry.kProgram;
    r.uProgram = uProgram;

    LookUpLocations(r);

    // THE INSERT MAY MOVE THE VECTOR, and then the remembered pointer would
    // point into nothing. So the memory is dropped RIGHT BEFORE the insert
    // and rebuilt on the fresh pointer right after it.
    m_bLastProgramKnown = false;
    rBucket.push_back(kEntry);

    m_kLastKey = m_kKey;
    m_pLastProgram = &rBucket.back().kProgram;
    m_bLastProgramKnown = true;
    return m_pLastProgram;
}

void CGlDevice::BuildFinalMatrix(D3DXMATRIX* pResult) const
{
    D3DXMATRIX kWorldView;
    D3DXMatrixMultiply(&kWorldView,
                       &m_matWorld,
                       &m_matView);

    D3DXMATRIX kFull;
    D3DXMatrixMultiply(&kFull, &kWorldView,
                       &m_matProjection);

    D3DXMATRIX kDepth;
    D3DXMatrixIdentity(&kDepth);
    kDepth._33 = 2.0f;
    kDepth._43 = -1.0f;
    // HALF A PIXEL ON THE MATRIX ROAD. Direct3D 8 puts a pixel
    // centre on the INTEGER coordinate, OpenGL on .5 fixed that
    // only for `XYZRHW` vertices. The interface is NOT drawn that way: the
    // image instances (`GrpImageInstance`, `GrpExpandedImageInstance`) send
    // positions through an orthographic projection, already moved by -0.5
    // for Direct3D. Without this fix their edges land exactly ON pixel
    // centres, where the rasteriser decides by rounding: measured on the
    // 280x105 popup board at scale 1 - the top corner and line (y 404.5 to
    // 436.5 after the -0.5) cover rows 404-435 instead of Direct3D's
    // 405-436, and row 436 (where the centre and the side lines begin) is
    // covered by neither piece:
    // the background shows through as a 1-px line across the board ("the
    // texture splits"), and the edges look uneven. Moving the whole image by
    // half a PHYSICAL pixel of the current viewport, in clip space
    // (`x' = x + w/W`, `y' = y - w/H`, before the render-target flip) is
    // exactly what the Direct3D rasteriser does by convention, so it holds
    // for 3D as well.
    float fHalfX, fHalfY;
    ClipHalfPixel(&fHalfX, &fHalfY);
    kDepth._41 = fHalfX;
    kDepth._42 = -fHalfY;

    D3DXMatrixMultiply(pResult, &kFull, &kDepth);
}

void CGlDevice::ApplyUniforms(const TProgram& c_rProgram)
{
    ApplyMatrixUniforms(c_rProgram);
    ApplyColourUniforms(c_rProgram);
    ApplyMaterialUniforms(c_rProgram);

    // The light is chosen BY NUMBER, not "the one set last" - see the note
    // at `m_akLights`. `NULL` means "none shines" and then all three
    // components go as zeros, as they used to with the light off.
    const D3DLIGHT8* c_pLight = FirstEnabledLight();
    ApplyLightUniforms(c_rProgram, c_pLight);
    ReportShadowPass(c_pLight);
}

void CGlDevice::ApplyMatrixUniforms(const TProgram& c_rProgram)
{
    D3DXMATRIX kFinal;
    BuildFinalMatrix(&kFinal);

    // Direct3D matrices are row-major, and GLSL reads column-major. The
    // difference is a transpose - and OpenGL can do it itself, with the
    // third argument. That is cheaper and less error-prone than reordering
    // by hand.
    // THE MATRIX CONVENTION - corrected BY MEASUREMENT.
    //
    // `GL_TRUE` stood here, i.e. "the data is rows, transpose it". It
    // looked like the right answer, because Direct3D matrices ARE row-major
    // and GLSL reads column-major. It was the other way round.
    //
    // Direct3D multiplies VECTOR BY MATRIX (`v * M`), GLSL matrix by vector
    // (`M * v`). For the same result GLSL must get the matrix
    // **transposed**. And passing row data with `GL_FALSE` - i.e. "read it
    // as columns" - gives exactly the transpose. For free.
    //
    // `GL_TRUE` transposed it back, i.e. cancelled what was needed. The
    // symptom was not obvious: with the identity matrix everything looked
    // fine (it is symmetric), but the fourth row took the place of the
    // fourth column and **`w` stopped being one**. With the depth fix that
    // gave `z/w = 2.0`, i.e. outside the view volume - and geometry vanished
    // WITHOUT ANY OpenGL ERROR. No reasoning caught it; reading
    // `glGetUniformfv` back from the running browser did.
    if (c_rProgram.iWorldViewProjection >= 0)
        glUniformMatrix4fv(c_rProgram.iWorldViewProjection, 1, GL_FALSE,
                           reinterpret_cast<const GLfloat*>(&kFinal));

    if (c_rProgram.iWorldView >= 0)
    {
        D3DXMATRIX kWorldView;
        D3DXMatrixMultiply(&kWorldView,
                           &m_matWorld,
                           &m_matView);
        glUniformMatrix4fv(c_rProgram.iWorldView, 1, GL_FALSE,
                           reinterpret_cast<const GLfloat*>(&kWorldView));
    }

    if (c_rProgram.iTextureMatrix >= 0)
        glUniformMatrix4fv(c_rProgram.iTextureMatrix,
                           M2W_TEXTURE_STAGES, GL_FALSE,
                           reinterpret_cast<const GLfloat*>(m_amatTexture));

    if (c_rProgram.iTargetSize >= 0)
        glUniform2f(c_rProgram.iTargetSize,
                    static_cast<GLfloat>(m_pTargetTexture ? (int)m_pTargetTexture->GetWidth() : m_iWidth),
                    static_cast<GLfloat>(m_pTargetTexture ? (int)m_pTargetTexture->GetHeight() : m_iHeight));
    if (c_rProgram.iFlipY >= 0)
        glUniform1f(c_rProgram.iFlipY, TargetFlipped() ? -1.0f : 1.0f);
    // Half a PHYSICAL pixel in target units: on the logical screen (GUI
    // scale) that is 0.5 / scale, on a texture 0.5.
    if (c_rProgram.iHalfPixel >= 0)
        glUniform1f(c_rProgram.iHalfPixel, m_pTargetTexture ? 0.5f : 0.5f / M2W_UiScale());
}

void CGlDevice::ApplyColourUniforms(const TProgram& c_rProgram)
{
    if (c_rProgram.iTextureFactor >= 0)
    {
        float k[4];
        M2W_ColorToFloat(m_adwRenderState[D3DRS_TEXTUREFACTOR], k);
        glUniform4fv(c_rProgram.iTextureFactor, 1, k);
    }

    if (c_rProgram.iFogColor >= 0)
    {
        float k[4];
        M2W_ColorToFloat(m_adwRenderState[D3DRS_FOGCOLOR], k);
        glUniform4fv(c_rProgram.iFogColor, 1, k);
    }

    if (c_rProgram.iFogRange >= 0)
    {
        // `D3DRS_FOGSTART` and `FOGEND` are floating-point numbers stored in
        // a `DWORD` - Direct3D 8 passed them by reinterpreting the bits, not
        // by converting to an integer.
        const float fStart = BitsToFloat(m_adwRenderState[D3DRS_FOGSTART]);
        const float fEnd = BitsToFloat(m_adwRenderState[D3DRS_FOGEND]);
        glUniform2f(c_rProgram.iFogRange, fStart, fEnd);
    }

    if (c_rProgram.iFogDensity >= 0)
        glUniform1f(c_rProgram.iFogDensity,
                    BitsToFloat(m_adwRenderState[D3DRS_FOGDENSITY]));

    if (c_rProgram.iAlphaRef >= 0)
        glUniform1f(c_rProgram.iAlphaRef,
                    static_cast<GLfloat>(m_adwRenderState[D3DRS_ALPHAREF])
                    / 255.0f);
}

void CGlDevice::ApplyMaterialUniforms(const TProgram& c_rProgram)
{
    if (c_rProgram.iMaterialDiffuse >= 0)
        glUniform4f(c_rProgram.iMaterialDiffuse,
                    m_kMaterial.Diffuse.r, m_kMaterial.Diffuse.g,
                    m_kMaterial.Diffuse.b, m_kMaterial.Diffuse.a);

    // MATERIAL EMISSIVE AND AMBIENT - see d3d8_fixedfunc.cpp.
    if (c_rProgram.iMaterialEmissive >= 0)
        glUniform4f(c_rProgram.iMaterialEmissive,
                    m_kMaterial.Emissive.r, m_kMaterial.Emissive.g,
                    m_kMaterial.Emissive.b, 1.0f);
    if (c_rProgram.iMaterialAmbient >= 0)
        glUniform4f(c_rProgram.iMaterialAmbient,
                    m_kMaterial.Ambient.r, m_kMaterial.Ambient.g,
                    m_kMaterial.Ambient.b, 1.0f);

    // GLOBAL AMBIENT - the D3DRS_AMBIENT state, not read at all until then.
    // The colour is ARGB, eight bits per component.
    if (c_rProgram.iGlobalAmbient >= 0)
    {
        const DWORD dw = m_adwRenderState[D3DRS_AMBIENT];
        glUniform4f(c_rProgram.iGlobalAmbient,
                    ((dw >> 16) & 0xFF) / 255.0f,
                    ((dw >> 8) & 0xFF) / 255.0f,
                    (dw & 0xFF) / 255.0f, 1.0f);
    }
}

void CGlDevice::ApplyLightUniforms(const TProgram& c_rProgram, const D3DLIGHT8* c_pLight)
{
    // THE LIGHT DIRECTION MUST BE IN THE SAME SPACE AS THE NORMAL
    // - and until this round it WAS NOT.
    //
    // The shading program computes the normal like this:
    //     vec3 n = normalize(mat3(uWorldView) * aNormal);
    // i.e. it moves it to VIEW space. And the light direction went from here
    // **raw, in WORLD space**. The dot product of two vectors from different
    // spaces means nothing - and **it changes as the camera turns**.
    //
    // Exactly what the user reported: "depending on the camera setting the
    // character either sinks into this darkness or is visible" (user
    // report). The symptom looked like a terrain fault and was a mistake in
    // one multiplication.
    //
    // Why it blackened the terrain: the ambient light is zero (measured:
    // `otoczenie 0.00 0.00 0.00`), so the whole colour is
    // `light colour * lambert`. When lambert comes out zero, the vertex
    // colour is black - and the shadow pass mixes it with the screen by
    // MULTIPLICATION. Black times anything gives black.
    //
    // The normal goes through WORLD and VIEW, so the light direction -
    // already in world space - only needs moving by VIEW alone. Without the
    // translation: it is a direction vector, not a point.
    if (c_rProgram.iLightDirection >= 0)
    {
        float fX = 0.0f, fY = 0.0f, fZ = -1.0f;
        if (c_pLight)
        {
            const D3DXMATRIX& m = m_matView;
            const float x = c_pLight->Direction.x;
            const float y = c_pLight->Direction.y;
            const float z = c_pLight->Direction.z;
            fX = x * m._11 + y * m._21 + z * m._31;
            fY = x * m._12 + y * m._22 + z * m._32;
            fZ = x * m._13 + y * m._23 + z * m._33;
        }
        glUniform4f(c_rProgram.iLightDirection, fX, fY, fZ, 0.0f);
    }

    if (c_rProgram.iLightColor >= 0)
        glUniform4f(c_rProgram.iLightColor,
                    c_pLight ? c_pLight->Diffuse.r : 0.0f,
                    c_pLight ? c_pLight->Diffuse.g : 0.0f,
                    c_pLight ? c_pLight->Diffuse.b : 0.0f, 1.0f);

    if (c_rProgram.iLightAmbient >= 0)
        glUniform4f(c_rProgram.iLightAmbient,
                    c_pLight ? c_pLight->Ambient.r : 0.0f,
                    c_pLight ? c_pLight->Ambient.g : 0.0f,
                    c_pLight ? c_pLight->Ambient.b : 0.0f, 1.0f);
}

void CGlDevice::ReportShadowPass(const D3DLIGHT8* c_pLight)
{
    // THE LIGHTING COMPONENTS - ONCE, AT THE FIRST DRAW WITH IT.
    //
    // The terrain is drawn in two passes: the first with lighting OFF (the
    // colour comes out white, the texture visible), the second - the
    // shadows - with it ON and blending by MULTIPLICATION
    // (`SRCBLEND_ZERO`, `DESTBLEND_SRCCOLOR`, MapOutdoorRenderHTP.cpp:609).
    //
    // Multiplying by black gives black. So if the lighting colour comes out
    // zero, the second pass **paints black over what the first drew
    // correctly** - and that is exactly what the terrain on the user's
    // screenshot looks like: good geometry, black colour.
    //
    // Any of three things can be zero, and each means different work:
    //   * the material - the game fills it from the map environment data
    //     (`MapManager.cpp:237`), so zero means the environment did not
    //     load;
    //   * no light enabled - `LightEnable(0, TRUE)` happens only when the
    //     environment has the directional light on;
    //   * a light enabled, but with a zero colour.
    //
    // Without this line I would choose between them by tossing a coin.
    // REPEATED, NOT ONE-OFF.
    //
    // The first version reported ONCE. A page that writes the log to
    // the browser console keeps only the latest messages - at fifty
    // thousand lines per session a one-off report falls out of the buffer
    // before anyone reads it.
    //
    // This is no procedural detail: a measurement that cannot be read is
    // the same as no measurement. We repeat at the same rhythm as the
    // terrain report, so the two can be put side by side.
    // ONLY THE TERRAIN SHADOW PASS.
    //
    // The previous version reported at the FIRST lit draw in the period -
    // and that need not be the terrain. The reading "the view matrix is the
    // identity" could then come from drawing the interface or the sky,
    // where the identity is perfectly in place, and would lead astray.
    //
    // This pass is recognised unambiguously by its blending: multiplying
    // the screen by itself (`SRCBLEND_ZERO`, `DESTBLEND_SRCCOLOR`) is done
    // in the whole client only by the terrain shadow.
    const bool bShadowPass =
        m_adwRenderState[D3DRS_SRCBLEND] == D3DBLEND_ZERO &&
        m_adwRenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCCOLOR;

    static unsigned long s_ulDraws = 0;
    if (bShadowPass && (s_ulDraws++ % 6000) == 0)
    {
        // THE DIRECTION AFTER CONVERSION - because it decides the angle of
        // incidence, and the angle of incidence is the only thing left
        // between the white material and the black terrain.
        float fKx = 0.0f, fKy = 0.0f, fKz = -1.0f;
        if (c_pLight)
        {
            const D3DXMATRIX& mv = m_matView;
            const float x = c_pLight->Direction.x;
            const float y = c_pLight->Direction.y;
            const float z = c_pLight->Direction.z;
            fKx = x * mv._11 + y * mv._21 + z * mv._31;
            fKy = x * mv._12 + y * mv._22 + z * mv._32;
            fKz = x * mv._13 + y * mv._23 + z * mv._33;
        }
        std::printf("m2w light: direction WORLD %.2f %.2f %.2f -> "
                    "VIEW %.2f %.2f %.2f\n",
                    c_pLight ? c_pLight->Direction.x : 0.0f,
                    c_pLight ? c_pLight->Direction.y : 0.0f,
                    c_pLight ? c_pLight->Direction.z : 0.0f,
                    fKx, fKy, fKz);

        // THE FULL MATERIAL, not the diffuse alone.
        //
        // Direct3D 8 computes the vertex colour like this:
        //     emissive
        //   + material_ambient * global_ambient
        //   + sum over lights [ material_diffuse * light_diffuse * angle
        //                     + material_ambient * light_ambient ]
        //
        // Our layer computes ONLY the last line, and with the material
        // diffuse in both places. **Emissive** is missing - and it is exactly
        // what gives brightness INDEPENDENT of the angle of incidence, i.e.
        // exactly what is missing where the terrain is black.
        //
        // Before adding it, I check whether the map environment sets
        // emissive at all. Zero means the lead is false.
        std::printf("m2w light: emissive %.2f %.2f %.2f  "
                    "material ambient %.2f %.2f %.2f\n",
                    m_kMaterial.Emissive.r, m_kMaterial.Emissive.g,
                    m_kMaterial.Emissive.b,
                    m_kMaterial.Ambient.r, m_kMaterial.Ambient.g,
                    m_kMaterial.Ambient.b);

        std::printf("m2w light: material %.2f %.2f %.2f  "
                    "light %s  colour %.2f %.2f %.2f  "
                    "ambient %.2f %.2f %.2f\n",
                    m_kMaterial.Diffuse.r, m_kMaterial.Diffuse.g,
                    m_kMaterial.Diffuse.b,
                    c_pLight ? "ON" : "NONE SHINES",
                    c_pLight ? c_pLight->Diffuse.r : 0.0f,
                    c_pLight ? c_pLight->Diffuse.g : 0.0f,
                    c_pLight ? c_pLight->Diffuse.b : 0.0f,
                    c_pLight ? c_pLight->Ambient.r : 0.0f,
                    c_pLight ? c_pLight->Ambient.g : 0.0f,
                    c_pLight ? c_pLight->Ambient.b : 0.0f);

        ReadShadowTargets();
    }
}

void CGlDevice::ReadShadowTargets()
{
    // WHAT REALLY MULTIPLIES THE TERRAIN.
    //
    // The shadow pass multiplies the terrain colour by TWO textures: stage 0
    // is the static terrain shadow map, stage 1 the CHARACTER shadow map - a
    // render target cleared to white and drawn around the character
    // (`MapOutdoorCharacterShadow.cpp:129`). The reach of that texture is
    // exactly "around the character", i.e. where the terrain is black;
    // beyond it clamping (CLAMP) returns the edge. Three lighting fixes did
    // not explain why the black is ONLY nearby - this is the only component
    // of the pass that has that shape.
    //
    // We read pixels from this texture's framebuffer (centre and corner) at
    // the moment the terrain uses it. The read is expensive (a pipeline
    // stall), so it is rare - every tenth report.
    static unsigned s_uReads = 0;
    if ((s_uReads++ % 10) == 0)
    {
        for (int i = 0; i < 2; ++i)
        {
            CGlTexture* pT = m_apTextures[i];
            if (!pT)
            {
                std::printf("m2w shadow: stage %d NO texture\n", i);
                continue;
            }
            if (!pT->IsTarget())
            {
                std::printf("m2w shadow: stage %d texture %ux%u (ordinary)\n",
                            i, (unsigned)pT->GetWidth(), (unsigned)pT->GetHeight());
                continue;
            }
            unsigned char abyCentre[4] = { 0 }, abyCorner[4] = { 0 };
            glBindFramebuffer(GL_READ_FRAMEBUFFER, pT->Framebuffer());
            glReadPixels(static_cast<GLint>(pT->GetWidth() / 2),
                         static_cast<GLint>(pT->GetHeight() / 2), 1, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, abyCentre);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, abyCorner);
            const GLenum eError = glGetError();
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              m_pTargetTexture ? m_pTargetTexture->Framebuffer() : 0);
            std::printf("m2w shadow: stage %d TARGET %ux%u centre (%u %u %u %u)"
                        " corner (%u %u %u %u) error %u\n",
                        i, (unsigned)pT->GetWidth(), (unsigned)pT->GetHeight(),
                        abyCentre[0], abyCentre[1], abyCentre[2], abyCentre[3],
                        abyCorner[0], abyCorner[1], abyCorner[2], abyCorner[3],
                        (unsigned)eError);
        }
    }
}

float CGlDevice::BitsToFloat(DWORD dw)
{
    float f;
    std::memcpy(&f, &dw, sizeof(f));
    return f;
}

const D3DLIGHT8* CGlDevice::FirstEnabledLight() const
{
    for (int i = 0; i < LIGHT_COUNT; ++i)
        if (m_abLightEnabled[i])
            return &m_akLights[i];
    return NULL;
}

void CGlDevice::ForgetPipelineCache()
{
    m_bLastProgramKnown = false;
    m_bFvfLayoutKnown = false;
    m_bLastShaderKnown = false;
}
