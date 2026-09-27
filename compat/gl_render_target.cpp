// SPDX-License-Identifier: GPL-2.0-or-later
// gl_render_target.cpp - where `CGlDevice` draws TO: the render target
// and depth surfaces (`SetRenderTarget` binds a texture's framebuffer or
// goes back to the screen), their descriptions (`SurfaceDescription`), the
// viewport, `Clear` and `GetBackBuffer`.

// Design:
// Direct3D 8 names the target of drawing by a SURFACE; OpenGL binds a
// FRAMEBUFFER. `SetRenderTarget` translates one into the other: a surface
// that is a view of a texture (`g_kTextureSurfaces`, gl_textures.cpp)
// binds that texture's framebuffer; a view of a texture WITHOUT one is
// refused (`E_FAIL`, `m_bTargetRefused`); any other surface, or NULL,
// returns to the screen. On the screen viewport sizes are logical pixels
// converted with `ToPhysical` (gl_device.cpp, the GUI scale);
// on a texture they are used as they are.
//
// Split out of gl_device.cpp (the last method move of group
// 7) - a pure code move: method bodies unchanged (checked token by
// token), the class declared in gl_internal.h.
// Translated.

#include "win32_compat.h"
#include "gl_internal.h"

#include <cstdio>

HRESULT CGlDevice::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                             D3DMULTISAMPLE_TYPE,
                                             IDirect3DSurface8** ppSurface)
{
    if (!ppSurface)
        return E_FAIL;
    *ppSurface = new CGlSurface(Width, Height, Format);
    return D3D_OK;
}

CGlSurface* CGlDevice::SurfaceDescription(CGlSurface*& rpCached,
                                          D3DFORMAT eFormat)
{
    const UINT uWidth = static_cast<UINT>(m_iWidth);
    const UINT uHeight  = static_cast<UINT>(m_iHeight);

    if (rpCached)
    {
        D3DSURFACE_DESC kDesc;
        rpCached->GetDesc(&kDesc);
        if (kDesc.Width != uWidth || kDesc.Height != uHeight)
        {
            rpCached->Release();
            rpCached = NULL;
        }
    }
    if (!rpCached)
        rpCached = new CGlSurface(uWidth, uHeight, eFormat);

    // The caller gets a reference and releases it itself - ours stays.
    rpCached->AddRef();
    return rpCached;
}

HRESULT CGlDevice::GetRenderTarget(IDirect3DSurface8** ppRenderTarget)
{
    if (!ppRenderTarget)
        return E_FAIL;
    *ppRenderTarget = SurfaceDescription(m_pTargetDesc, D3DFMT_A8R8G8B8);
    return D3D_OK;
}

HRESULT CGlDevice::GetDepthStencilSurface(IDirect3DSurface8** ppZStencilSurface)
{
    if (!ppZStencilSurface)
        return E_FAIL;
    *ppZStencilSurface = SurfaceDescription(m_pDepthDesc, D3DFMT_D16);
    return D3D_OK;
}

namespace
{

/// DIAGNOSTIC: prints the 2x2 corner of the render-target texture
/// `pTexture` that has just been drawn to - at most three times per session.
///
/// Why here: the terrain multiplies its colour by the shadow map
/// (`D3DTOP_MODULATE` on stage one, `MapOutdoorRenderSTP.cpp`), so a black
/// shadow map means black ground (the measured terrain vertex colour is
/// `ffdadadf` - bright - and stage zero selects the texture alone, so the
/// darkness had to come in later). Reading the texture from the browser
/// console FAILED (`GL_INVALID_OPERATION`) and returned plain zeros - which
/// look exactly like real zeros. Here the framebuffer is still bound and
/// known to be complete, so the read is trustworthy (the GL error is
/// printed with it).
void ReportTargetCorner(const CGlTexture* pTexture)
{
    static int s_iReports = 0;
    if (s_iReports >= 3)
        return;
    ++s_iReports;
    unsigned char abyPixels[16] = { 0 };
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, abyPixels);
    const GLenum eError = glGetError();
    std::printf("m2w target: after drawing to texture %ux%u"
                " corner (%u %u %u %u) error %u\n",
                (unsigned)pTexture->GetWidth(),
                (unsigned)pTexture->GetHeight(),
                abyPixels[0], abyPixels[1], abyPixels[2],
                abyPixels[3], (unsigned)eError);
}

}  // namespace

/// Direct3D names the target of drawing by a surface, OpenGL binds a
/// framebuffer. A view of a `D3DUSAGE_RENDERTARGET` texture binds the
/// framebuffer that texture got when it was created (`PrepareAsTarget`);
/// any other surface - the client passes `GetBackBuffer` to go
/// back - or NULL returns to the screen.
///
/// A render-target texture WITHOUT a framebuffer (creating it failed) is
/// refused COMPLETELY: `E_FAIL` plus `m_bTargetRefused`, which makes `Clear`
/// refuse too. A partial refusal is worse than a complete one:
/// the client checks the result only to skip the shadows, and runs the rest
/// of `BeginRenderCharacterShadowToTexture` anyway - its
/// `Clear(TARGET|ZBUFFER, WHITE)`, meant for the shadow map, then cleared
/// the REAL SCREEN to white twice a frame (measured: 122 `glClear` with mask
/// 16640 and colour (1,1,1,1) per 21127 draw calls). The symptom looked
/// nothing like the cause: black ground, the sky only at some angles.
HRESULT CGlDevice::SetRenderTarget(IDirect3DSurface8* pTarget, IDirect3DSurface8*)
{
    // Surface -> texture. The client also passes the SCREEN BUFFER here
    // (`GetBackBuffer`) to go back to drawing on the screen - and that is a
    // different class than a view of a texture level. The texture-surface
    // table settles it without a blind cast; no entry means "screen".
    CGlTexture* pTexture = NULL;
    {
        const std::map<IDirect3DSurface8*, CGlTexture*>::const_iterator it =
            g_kTextureSurfaces.find(pTarget);
        if (it != g_kTextureSurfaces.end())
            pTexture = it->second;
    }

    if (!pTarget || !pTexture)
    {
        // Back to the screen. Leaving a texture target: one look at what
        // was drawn into it (see ReportTargetCorner).
        if (m_pTargetTexture)
            ReportTargetCorner(m_pTargetTexture);

        m_bTargetRefused = false;
        m_pTargetTexture = NULL;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, ToPhysical(m_iWidth), ToPhysical(m_iHeight));
        m_kViewport.X = 0;
        m_kViewport.Y = 0;
        m_kViewport.Width = static_cast<DWORD>(m_iWidth);
        m_kViewport.Height = static_cast<DWORD>(m_iHeight);
        M2W_ForgetGlState();
        return D3D_OK;
    }

    if (!pTexture->IsTarget())
    {
        // A texture without a framebuffer - because creating it failed.
        // We refuse completely, as earlier.
        m_bTargetRefused = true;
        return E_FAIL;
    }

    m_bTargetRefused = false;
    m_pTargetTexture = pTexture;
    glBindFramebuffer(GL_FRAMEBUFFER, pTexture->Framebuffer());
    glViewport(0, 0, static_cast<GLsizei>(pTexture->GetWidth()),
               static_cast<GLsizei>(pTexture->GetHeight()));
    m_kViewport.X = 0;
    m_kViewport.Y = 0;
    m_kViewport.Width = pTexture->GetWidth();
    m_kViewport.Height = pTexture->GetHeight();

    // The texture we are starting to draw to must not stay bound as a
    // SOURCE. The binding memory does not know that, so we clear it.
    M2W_ForgetGlState();
    return D3D_OK;
}

HRESULT CGlDevice::GetViewport(D3DVIEWPORT8* pViewport)
{
    if (!pViewport)
        return E_FAIL;
    *pViewport = m_kViewport;
    return D3D_OK;
}

HRESULT CGlDevice::SetViewport(CONST D3DVIEWPORT8* pViewport)
{
    if (!pViewport)
        return E_FAIL;
    m_kViewport = *pViewport;

    // Direct3D counts the viewport from the TOP LEFT corner, OpenGL from
    // the bottom. The conversion is here and only here.
    //
    // THE HEIGHT COMES FROM THE CURRENT TARGET, not from the screen (part
    // 240). When drawing to a 512x512 shadow texture in a window 915 high,
    // subtracting from the SCREEN height gives a viewport moved 403 pixels
    // up - i.e. off the texture. The symptom would be an empty or clipped
    // shadow, not an error.
    // A viewport on the SCREEN is in logical px - multiplied by the GUI
    // scale; on a texture unchanged.
    const bool bScreen = !m_pTargetTexture;
    const GLint iTargetHeight = bScreen
        ? ToPhysical(m_iHeight)
        : static_cast<GLint>(m_pTargetTexture->GetHeight());
    const GLint iX = bScreen ? ToPhysical(static_cast<int>(pViewport->X)) : static_cast<GLint>(pViewport->X);
    const GLint iH = bScreen ? ToPhysical(static_cast<int>(pViewport->Height)) : static_cast<GLint>(pViewport->Height);
    const GLint iW = bScreen ? ToPhysical(static_cast<int>(pViewport->Width)) : static_cast<GLint>(pViewport->Width);
    const GLint iY0 = bScreen ? ToPhysical(static_cast<int>(pViewport->Y)) : static_cast<GLint>(pViewport->Y);
    const GLint iY = iTargetHeight - iY0 - iH;
    glViewport(iX, iY, iW, iH);
    return D3D_OK;
}

HRESULT CGlDevice::Clear(DWORD, CONST D3DRECT*, DWORD Flags, D3DCOLOR Color,
                         float Z, DWORD Stencil)
{
    // See `SetRenderTarget`: while the client believes it draws OFF THE
    // SCREEN, its clear has no right to touch the screen. It returns an
    // error, because that is what it is - and
    // `BeginRenderCharacterShadowToTexture` reads and logs that error.
    if (m_bTargetRefused)
        return E_FAIL;

    GLbitfield uBits = 0;

    if (Flags & D3DCLEAR_TARGET)
    {
        float k[4];
        M2W_ColorToFloat(Color, k);
        glClearColor(k[0], k[1], k[2], k[3]);
        uBits |= GL_COLOR_BUFFER_BIT;
    }
    if (Flags & D3DCLEAR_ZBUFFER)
    {
        glClearDepthf(Z);
        // Clearing depth does nothing while writing to the depth buffer is
        // off - an OpenGL behaviour Direct3D did not have. Depth writes are
        // switched on for the clear.
        glDepthMask(GL_TRUE);
        // The state memory MUST know about it: otherwise on the next draw
        // it would think the mask is already right and would not restore
        // it.
        m_kGlCache.iDepthWrite = 1;
        m_bClearForcedDepthWrite = true;
        uBits |= GL_DEPTH_BUFFER_BIT;
    }
    if (Flags & D3DCLEAR_STENCIL)
    {
        glClearStencil(static_cast<GLint>(Stencil));
        uBits |= GL_STENCIL_BUFFER_BIT;
    }

    // Scissors could limit the clear to rectangles, but the client passes
    // them only in shop mode (`g_isBrowserMode`), where in the browser there
    // is nothing to protect anyway - the frame lies ABOVE the canvas.
    glDisable(GL_SCISSOR_TEST);
    glClear(uBits);
    return D3D_OK;
}

HRESULT CGlDevice::GetBackBuffer(UINT, D3DBACKBUFFER_TYPE,
                                 IDirect3DSurface8** ppBackBuffer)
{
    return GetRenderTarget(ppBackBuffer);
}
