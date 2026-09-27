// SPDX-License-Identifier: GPL-2.0-or-later
// gl_render_states.cpp - the render-state half of `CGlDevice`: the
// Direct3D 8 Set*/Get* for render states, texture-stage states, transforms,
// material, lights and the gamma ramp, and the per-draw application of the
// recorded state to OpenGL (`ApplyRenderStates`).

// Design:
// PRINCIPLE THREE from the gl_device.cpp header lives here: Set* only
// RECORDS the state in the device's arrays, and OpenGL learns about it once,
// at draw time (`ApplyRenderStates`, called from `PrepareDrawFromBuffer`),
// through the state memory (`TGlStateCache`, `SetEnabled`), which skips calls
// that would change nothing. `M2W_ForgetGlState` raises `g_bGlStateUnknown`
// (callers: road B in custom_draw.cpp, and SetRenderTarget); its only
// reader is `PrepareDrawFromBuffer` in gl_buffers.cpp, which throws the
// memory away BEFORE its program check (until then the reader
// was `ApplyRenderStates`, called after that check).
//
// Split out of gl_device.cpp (group 7) - a pure code move:
// method bodies unchanged, the class declared in gl_internal.h.
// Translated.

#include "win32_compat.h"
#include "gl_internal.h"
#include "stubs.h"

#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstring>

// Declared in gl_internal.h (read by PrepareDrawFromBuffer
// in gl_buffers.cpp, before the program check).
bool g_bGlStateUnknown = false;

void M2W_ForgetGlState()
{
    g_bGlStateUnknown = true;
    g_bTextureBindingsUnknown = true;
}

HRESULT CGlDevice::LightEnable(DWORD Index, BOOL Enable)
{
    if (Index >= LIGHT_COUNT)
        return D3D_OK;
    m_abLightEnabled[Index] = (Enable != FALSE);
    return D3D_OK;
}

// --- gamma ramp -----------------------------------------------------------
//
// Image brightness through the card's gamma ramp. In the browser this
// corresponds to a filter on the canvas, and **we have not written it
// yet** - the brightness slider in the game settings does nothing.
//
// Earlier both functions were EMPTY AND SILENT, and `GetGammaRamp`
// did not even touch the caller's structure - so it handed back
// **uninitialised memory** as the result. The same shape that cost
// `GetClientRect`: the caller got garbage and took it for an
// answer.
//
// Today: the ramp is REMEMBERED and handed back, so the "set, read" cycle
// is consistent; the missing filter reports itself once through the stub
// registry, so it is no longer invisible.
//
// `PythonGraphic.cpp:154` (`SetGammaRamp`) is today the only caller in
// the whole tree - `GetGammaRamp` has none. The fix is therefore
// preventive, and that is how it should be: a latent bug is not left in
// place just because nobody happens to walk into it.
void CGlDevice::SetGammaRamp(DWORD, CONST D3DGAMMARAMP* pRamp)
{
    M2W_STUB("D3D8::SetGammaRamp (the brightness slider does nothing)");
    if (pRamp)
    {
        m_kGammaRamp = *pRamp;
        m_bGammaRampKnown = true;
    }
}

void CGlDevice::GetGammaRamp(D3DGAMMARAMP* pRamp)
{
    if (!pRamp)
        return;

    if (m_bGammaRampKnown)
    {
        *pRamp = m_kGammaRamp;
        return;
    }

    // The identity ramp - the one the card has before any change. Eight
    // bits are stretched to sixteen by multiplying by 257 (so 0xFF ->
    // 0xFFFF), not by shifting by eight, which would leave the brightest
    // value at 0xFF00.
    for (int i = 0; i < 256; ++i)
    {
        const WORD w = static_cast<WORD>(i * 257);
        pRamp->red[i] = w;
        pRamp->green[i] = w;
        pRamp->blue[i] = w;
    }
}

// -----------------------------------------------------------------------
// States - recorded, applied at draw time
// -----------------------------------------------------------------------

HRESULT CGlDevice::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    if (static_cast<DWORD>(State) < RENDER_STATE_COUNT)
        m_adwRenderState[State] = Value;
    return D3D_OK;
}

HRESULT CGlDevice::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
{
    if (!pValue)
        return E_FAIL;
    *pValue = (static_cast<DWORD>(State) < RENDER_STATE_COUNT)
              ? m_adwRenderState[State] : 0;
    return D3D_OK;
}

HRESULT CGlDevice::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                        DWORD Value)
{
    if (Stage < M2W_TEXTURE_STAGES && static_cast<DWORD>(Type) < STAGE_STATE_COUNT)
        m_aadwStageState[Stage][Type] = Value;
    return D3D_OK;
}

HRESULT CGlDevice::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                        DWORD* pValue)
{
    if (!pValue)
        return E_FAIL;
    *pValue = (Stage < M2W_TEXTURE_STAGES &&
               static_cast<DWORD>(Type) < STAGE_STATE_COUNT)
              ? m_aadwStageState[Stage][Type] : 0;
    return D3D_OK;
}

HRESULT CGlDevice::SetTransform(D3DTRANSFORMSTATETYPE State,
                                CONST D3DMATRIX* pMatrix)
{
    if (!pMatrix)
        return E_FAIL;

    switch (State)
    {
        case D3DTS_WORLD:      m_matWorld = *pMatrix; break;
        case D3DTS_VIEW:       m_matView = *pMatrix; break;
        case D3DTS_PROJECTION: m_matProjection = *pMatrix; break;
        case D3DTS_TEXTURE0:   m_amatTexture[0] = *pMatrix; break;
        case D3DTS_TEXTURE1:   m_amatTexture[1] = *pMatrix; break;
        default: break;
    }
    return D3D_OK;
}

HRESULT CGlDevice::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
{
    if (!pMatrix)
        return E_FAIL;

    switch (State)
    {
        case D3DTS_WORLD:      *pMatrix = m_matWorld; break;
        case D3DTS_VIEW:       *pMatrix = m_matView; break;
        case D3DTS_PROJECTION: *pMatrix = m_matProjection; break;
        case D3DTS_TEXTURE0:   *pMatrix = m_amatTexture[0]; break;
        case D3DTS_TEXTURE1:   *pMatrix = m_amatTexture[1]; break;
        default: D3DXMatrixIdentity(static_cast<D3DXMATRIX*>(pMatrix)); break;
    }
    return D3D_OK;
}

HRESULT CGlDevice::SetMaterial(CONST D3DMATERIAL8* pMaterial)
{
    if (pMaterial)
        m_kMaterial = *pMaterial;
    return D3D_OK;
}

HRESULT CGlDevice::GetMaterial(D3DMATERIAL8* pMaterial)
{
    if (!pMaterial)
        return E_FAIL;
    *pMaterial = m_kMaterial;
    return D3D_OK;
}

HRESULT CGlDevice::SetLight(DWORD Index, CONST D3DLIGHT8* pLight)
{
    if (!pLight || Index >= LIGHT_COUNT)
        return D3D_OK;
    m_akLights[Index] = *pLight;
    return D3D_OK;
}

HRESULT CGlDevice::GetLight(DWORD Index, D3DLIGHT8* pLight)
{
    if (!pLight || Index >= LIGHT_COUNT)
        return E_FAIL;
    *pLight = m_akLights[Index];
    return D3D_OK;
}

void CGlDevice::SetEnabled(GLenum eCap, int& riCached, bool bEnabled)
{
    const int iWanted = bEnabled ? 1 : 0;
    if (riCached == iWanted)
        return;
    riCached = iWanted;
    if (bEnabled)
        glEnable(eCap);
    else
        glDisable(eCap);
}

void CGlDevice::ApplyRenderStates()
{
    // Drawing outside this layer (road B) is handled by the caller,
    // PrepareDrawFromBuffer, before the program check.

    const DWORD* s = m_adwRenderState;

    SetEnabled(GL_DEPTH_TEST, m_kGlCache.iDepthTest, s[D3DRS_ZENABLE] != 0);

    const int iDepthWrite = s[D3DRS_ZWRITEENABLE] ? 1 : 0;
    if (m_kGlCache.iDepthWrite != iDepthWrite)
    {
        m_kGlCache.iDepthWrite = iDepthWrite;
        glDepthMask(iDepthWrite ? GL_TRUE : GL_FALSE);
    }
    m_bClearForcedDepthWrite = false;

    const GLenum eDepthFunc = M2W_CompareFunc(s[D3DRS_ZFUNC]);
    if (m_kGlCache.eDepthFunc != eDepthFunc)
    {
        m_kGlCache.eDepthFunc = eDepthFunc;
        glDepthFunc(eDepthFunc);
    }

    SetEnabled(GL_BLEND, m_kGlCache.iBlend, s[D3DRS_ALPHABLENDENABLE] != 0);
    if (s[D3DRS_ALPHABLENDENABLE])
    {
        const GLenum eBlendSrc = M2W_BlendFactor(s[D3DRS_SRCBLEND]);
        const GLenum eBlendDst = M2W_BlendFactor(s[D3DRS_DESTBLEND]);
        if (m_kGlCache.eBlendSrc != eBlendSrc || m_kGlCache.eBlendDst != eBlendDst)
        {
            m_kGlCache.eBlendSrc = eBlendSrc;
            m_kGlCache.eBlendDst = eBlendDst;
            glBlendFunc(eBlendSrc, eBlendDst);
        }
        // The blend equation. Metin2 today sets only addition
        // (`StateManager.cpp:204` and `FlyTrace.cpp:144`), so a hard-coded
        // `GL_FUNC_ADD` would give the same image - but the assumption
        // "nobody will ever set anything else" is exactly the kind of
        // assumption that has already outlived its truth several times in
        // this project.
        const GLenum eBlendEquation = M2W_BlendOp(s[D3DRS_BLENDOP]);
        if (m_kGlCache.eBlendEquation != eBlendEquation)
        {
            m_kGlCache.eBlendEquation = eBlendEquation;
            glBlendEquation(eBlendEquation);
        }
    }

    // The channel write mask. The client sets it once, to "all", but it
    // costs four bits and one call, and without it every future use would
    // be silently skipped.
    const DWORD dwMask = s[D3DRS_COLORWRITEENABLE];
    if (m_kGlCache.iColorMask != static_cast<int>(dwMask & 0xF))
    {
        m_kGlCache.iColorMask = static_cast<int>(dwMask & 0xF);
        glColorMask((dwMask & D3DCOLORWRITEENABLE_RED) ? GL_TRUE : GL_FALSE,
                    (dwMask & D3DCOLORWRITEENABLE_GREEN) ? GL_TRUE : GL_FALSE,
                    (dwMask & D3DCOLORWRITEENABLE_BLUE) ? GL_TRUE : GL_FALSE,
                    (dwMask & D3DCOLORWRITEENABLE_ALPHA) ? GL_TRUE : GL_FALSE);
    }

    // WHAT IS NOT HERE AND WILL NOT BE - this is an answer, not an oversight.
    //
    // `D3DRS_FILLMODE`: OpenGL ES 3 has no `glPolygonMode`, so wireframe
    // cannot be switched on. In Metin2 only the terrain preview in the tools
    // asks for it (`MapOutdoorRender.cpp:765` and `801`) and
    // `CScreen::RenderD3DXMesh`; every other place sets `D3DFILL_SOLID`. So
    // we always draw filled. Doing it for real would mean building a
    // separate edge buffer - a big cost, zero gain for the game itself.
    //
    // `D3DRS_SHADEMODE`: `D3DSHADE_FLAT` would need the `flat` qualifier on
    // the variables between the shaders, i.e. A SEPARATE PROGRAM VARIANT for
    // one use (`LensFlare.cpp:203`). The difference is purely visual - the
    // sun flare is shaded smoothly instead of flat.
    //
    // `D3DRS_ZBIAS`, `D3DRS_STENCILENABLE`, `D3DRS_WRAP0..7`,
    // `D3DRS_SOFTWAREVERTEXPROCESSING`, `D3DRS_CLIPPING`: the client sets
    // them once, at start-up, to Direct3D's INITIAL values and never changes
    // them. Applying them would be writing zero over zero.

    bool bCull = false;
    GLenum eCullFace = GL_BACK;
    M2W_CullMode(s[D3DRS_CULLMODE], &bCull, &eCullFace);
    if (TargetFlipped())
        eCullFace = (eCullFace == GL_BACK) ? GL_FRONT : GL_BACK;
    SetEnabled(GL_CULL_FACE, m_kGlCache.iCull, bCull);
    if (bCull && m_kGlCache.eCullFace != eCullFace)
    {
        m_kGlCache.eCullFace = eCullFace;
        glCullFace(eCullFace);
    }
}

/// `?noflip=1` switches the Y flip of
/// drawing to a texture off for an A/B test; typing
/// `window.m2w_no_reflection = 1` (or `= 0`) in the console overrides the
/// address live. Returns 1 = no flip.
EM_JS(int, m2w_no_flip, (void), {
    if (window.m2w_no_reflection !== undefined) return window.m2w_no_reflection ? 1 : 0;
    return m2w.options.get('noflip') === '1' ? 1 : 0; });

bool CGlDevice::TargetFlipped() const
{
    if (!m_pTargetTexture)
        return false;
    // Read on every draw to a target (a few dozen per frame), so the
    // switch works live.
    return !m2w_no_flip();
}
