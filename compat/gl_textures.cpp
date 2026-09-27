// SPDX-License-Identifier: GPL-2.0-or-later
// gl_textures.cpp - `CGlTexture`, `CGlSurface`, `CGlLevelSurface`:
// the resource objects behind `IDirect3DTexture8`/`IDirect3DSurface8`, and
// the texture methods of `CGlDevice` - `CreateTexture`,
// `SetTexture`/`GetTexture` and the per-draw binding `ApplyTextures`.

// Design:
// PRINCIPLE ONE FROM THE gl_device.cpp HEADER LIVES HERE: `Lock` HANDS OUT
// OUR BUFFER, NOT DRIVER MEMORY. Every texture keeps its own copy in wasm
// memory (`m_akLevels`), `LockRect` hands out a pointer into it,
// `UnlockRect` sends it to OpenGL (`UploadLevel`). The reason is the same
// as for the whole device: OpenGL ES 3 has no mapping of card memory to a
// CPU pointer for textures, and the client kept them in `D3DPOOL_MANAGED`
// (a system-side copy) anyway in 21 of 31 pool uses.
//
// Split out of d3d8_gl.cpp (the first module of the split,
// the renaming plan) - a pure code move, the classes declared in
// gl_internal.h. No method changed its content. The texture methods of
// CGlDevice came from gl_device.cpp, also unchanged.
// Translated.

#include "gl_internal.h"

#include "win32_compat.h"
#include "d3d8_states.h"
#include "dxt.h"
#include "frame_stats.h"

#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include <cstdio>
#include <cstring>
#include <cstdint>

// ===========================================================================
// Can the browser do compressed textures - definitions (declarations in
// gl_internal.h)
// ===========================================================================

int g_iGpuHasDxt = -1;
bool g_bForcedDxtDecode = false;

// The flag from gl_internal.h (described there). Written by `UploadLevel`
// and `CreateTexture` (render target) in this file and by
// `M2W_ForgetGlState` (gl_render_states.cpp); read and cleared by
// `ApplyTextures` (this file). External linkage, when
// `UploadLevel` left d3d8_gl.cpp; defined here.
bool g_bTextureBindingsUnknown = true;

// ===========================================================================
// CGlTexture
// ===========================================================================

CGlTexture::CGlTexture(UINT uWidth, UINT uHeight, UINT uLevels, D3DFORMAT eFormat)
    : m_uWidth(uWidth), m_uHeight(uHeight),
      m_uLevels(uLevels ? uLevels : 1), m_eFormat(eFormat),
      m_uGlName(0), m_bUploaded(false)
{
    m_kFormat = M2W_TextureFormat(eFormat);
    m_akLevels.resize(m_uLevels);

    // The copy in wasm memory - see principle one at the top of the file.
    UINT uW = uWidth, uH = uHeight;
    for (UINT i = 0; i < m_uLevels; ++i)
    {
        const UINT uBytes = m_kFormat.bCompressed
            // DXT is counted in 4x4 blocks: DXT1 eight bytes per block,
            // DXT3 and DXT5 sixteen.
            ? (((uW + 3) / 4) * ((uH + 3) / 4) *
               ((eFormat == D3DFMT_DXT1) ? 8u : 16u))
            : (uW * uH * m_kFormat.uBytesPerPixel);

        m_akLevels[i].assign(uBytes ? uBytes : 1, 0);
        m_akLevels[i].shrink_to_fit();
        if (uW > 1) uW /= 2;
        if (uH > 1) uH /= 2;
    }

    glGenTextures(1, &m_uGlName);
}

CGlTexture::~CGlTexture()
{
    if (m_uFramebuffer)
        glDeleteFramebuffers(1, &m_uFramebuffer);
    if (m_uDepthBuffer)
        glDeleteRenderbuffers(1, &m_uDepthBuffer);
    if (m_uGlName)
        glDeleteTextures(1, &m_uGlName);
}

HRESULT CGlTexture::LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect,
                             CONST RECT* /*pRect*/, DWORD /*Flags*/)
{
    if (!pLockedRect || Level >= m_uLevels)
        return E_FAIL;

    UINT uW = m_uWidth;
    for (UINT i = 0; i < Level; ++i)
        if (uW > 1) uW /= 2;

    // `Pitch` is the distance between ROWS in bytes. For compressed formats
    // Direct3D gave the distance between ROWS OF BLOCKS here - and a client
    // that does not know that does not write to compressed textures anyway.
    pLockedRect->Pitch = static_cast<INT>(
        m_kFormat.bCompressed
        ? (((uW + 3) / 4) * ((m_eFormat == D3DFMT_DXT1) ? 8u : 16u))
        : (uW * m_kFormat.uBytesPerPixel));
    pLockedRect->pBits = m_akLevels[Level].empty()
                         ? NULL : &m_akLevels[Level][0];
    return D3D_OK;
}

HRESULT CGlTexture::UnlockRect(UINT Level)
{
    UploadLevel(Level);
    return D3D_OK;
}

HRESULT CGlTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc)
{
    if (!pDesc)
        return E_FAIL;

    UINT uW = m_uWidth, uH = m_uHeight;
    for (UINT i = 0; i < Level; ++i)
    {
        if (uW > 1) uW /= 2;
        if (uH > 1) uH /= 2;
    }

    std::memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Format = m_eFormat;
    pDesc->Width = uW;
    pDesc->Height = uH;
    return D3D_OK;
}

void CGlTexture::PrepareAsTarget()
{
    glBindTexture(GL_TEXTURE_2D, m_uGlName);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(m_uWidth),
                 static_cast<GLsizei>(m_uHeight), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    // No filtering between levels - there is only one level.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_aiSampler[0] = GL_LINEAR;
    m_aiSampler[1] = GL_LINEAR;
    m_aiSampler[2] = GL_CLAMP_TO_EDGE;
    m_aiSampler[3] = GL_CLAMP_TO_EDGE;
    m_bUploaded = true;

    // A RENDER TARGET HAS ONE LEVEL - AND MUST TELL THE CARD SO.
    //
    // This was the cause of the BLACK TERRAIN AROUND THE CHARACTER. The
    // terrain multiplies its colour by the character shadow map (stage 1),
    // and `StateManager.cpp:302` sets `MIPFILTER = LINEAR` on that stage -
    // i.e. sampling with levels of detail. The target texture has only level
    // 0, and `GL_TEXTURE_MAX_LEVEL` stayed at its default (1000): for OpenGL
    // the chain was INCOMPLETE and every sample returned black - even though
    // `glReadPixels` from its framebuffer showed white.
    //
    // The shape of the symptom fitted to a T: black everywhere the shadow
    // pass draws, white in the distance, because that pass has its fog colour
    // set to white. Three lighting fixes missed, because the vertex colour
    // was never black (emissive measured at 0.32). The `?shadow=2` experiment
    // decided it, not reasoning.
    //
    // `UploadLevel` has done the same for ordinary textures
    // - the same bug, a second road that had escaped it.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glGenRenderbuffers(1, &m_uDepthBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_uDepthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          static_cast<GLsizei>(m_uWidth),
                          static_cast<GLsizei>(m_uHeight));
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &m_uFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_uFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_uGlName, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_uDepthBuffer);

    const GLenum eStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (eStatus != GL_FRAMEBUFFER_COMPLETE)
    {
        // We do not pretend it worked. An incomplete framebuffer is worse
        // than none: drawing to it is undefined.
        std::printf("m2w target: framebuffer %ux%u incomplete (0x%x)\n",
                    (unsigned)m_uWidth, (unsigned)m_uHeight,
                    (unsigned)eStatus);
        glDeleteFramebuffers(1, &m_uFramebuffer);
        m_uFramebuffer = 0;
    }
}

bool CGlTexture::SamplerChanged(GLint iMinFilter, GLint iMagFilter,
                                GLint iWrapS, GLint iWrapT) const
{
    return m_aiSampler[0] != iMinFilter ||
           m_aiSampler[1] != iMagFilter ||
           m_aiSampler[2] != iWrapS ||
           m_aiSampler[3] != iWrapT;
}

// Sampling parameters belong to the TEXTURE, not to the draw, so it is
// enough to send them when they change. The layer used to send them on
// EVERY draw: four calls per bound texture, measured 183 per frame at 50
// draws.
void CGlTexture::SetSampler(GLint iMinFilter, GLint iMagFilter,
                            GLint iWrapS, GLint iWrapT)
{
    if (m_aiSampler[0] != iMinFilter)
    {
        m_aiSampler[0] = iMinFilter;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, iMinFilter);
    }
    if (m_aiSampler[1] != iMagFilter)
    {
        m_aiSampler[1] = iMagFilter;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, iMagFilter);
    }
    if (m_aiSampler[2] != iWrapS)
    {
        m_aiSampler[2] = iWrapS;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, iWrapS);
    }
    if (m_aiSampler[3] != iWrapT)
    {
        m_aiSampler[3] = iWrapT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, iWrapT);
    }
}

namespace
{

/// FONT ATLAS AT THE GUI SCALE. The engine packs
/// glyphs into an A4R4G4B4 texture by copying its DIB 1:1; at a GUI scale
/// `platform_text.cpp` keeps for every DIB a SHADOW in physical pixels
/// (glyphs baked at lfHeight * scale). When `c_rData` (`ruW` x `ruH`) is a
/// copy of such a DIB, copies the shadow into `rOut`, sets `ruW`/`ruH` to its
/// size and returns true - the bigger texture goes to the card instead.
/// Texture coordinates are normalised, so the engine notices nothing, and the
/// UI quad and the texel are 1:1 again - sharp letters.
bool FontAtlasShadow(const std::vector<unsigned char>& c_rData, UINT& ruW, UINT& ruH,
                     std::vector<unsigned char>& rOut)
{
    unsigned uHiW = 0, uHiH = 0;
    const unsigned short* pHiRes = M2W_FontAtlasHiRes(&c_rData[0], ruW, ruH, &uHiW, &uHiH);
    if (!pHiRes)
        return false;
    rOut.assign(reinterpret_cast<const unsigned char*>(pHiRes),
                reinterpret_cast<const unsigned char*>(pHiRes) + (size_t)uHiW * uHiH * 2);
    ruW = uHiW;
    ruH = uHiH;
    return true;
}

}  // namespace

void CGlTexture::UploadLevel(UINT uLevel)
{
    if (uLevel >= m_uLevels || !m_kFormat.bKnown)
        return;

    // Sending a texture to the card - including DXT decoding when the card
    // cannot do it itself. This is work that does NOT come evenly: entering
    // new terrain brings a great deal of it at once.
    m2wstats::TStopwatch kStopwatch(m2wstats::g_kTextureUploads);

    UINT uW = m_uWidth, uH = m_uHeight;
    for (UINT i = 0; i < uLevel; ++i)
    {
        if (uW > 1) uW /= 2;
        if (uH > 1) uH /= 2;
    }

    glBindTexture(GL_TEXTURE_2D, m_uGlName);
    g_bTextureBindingsUnknown = true;

    const std::vector<unsigned char>* pData = &m_akLevels[uLevel];
    if (pData->empty())
        return;

    if (uLevel == 0 && m_eFormat == D3DFMT_A4R4G4B4 && M2W_UiScale() != 1.0f &&
        FontAtlasShadow(*pData, uW, uH, m_kHiResBuffer))
        pData = &m_kHiResBuffer;
    const std::vector<unsigned char>& rData = *pData;

    // THE HIGHEST LEVEL WE REALLY SENT. An incomplete mip chain
    // samples BLACK in OpenGL, with no error, whenever the minification
    // filter asks for levels. Direct3D's `Levels = 0` means "the full chain",
    // while the constructor keeps one level (`uLevels ? uLevels : 1`) - so
    // the magnified UI worked and the strongly minified ground (texture
    // coordinates up to (50, 55) on one patch) was black. `GL_TEXTURE_MAX_LEVEL`
    // at the highest level really sent makes the texture complete.
    if (uLevel >= m_uHighestUploaded)
    {
        m_uHighestUploaded = uLevel;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                        static_cast<GLint>(m_uHighestUploaded));
    }

    if (m_kFormat.bCompressed && GpuHasDxt())
    {
        // The normal road: the blocks go to the card AS THEY ARE. That is the
        // whole value of compression - the texture takes a quarter of the
        // card memory it would take decoded.
        glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(uLevel),
                               m_kFormat.eInternalFormat,
                               static_cast<GLsizei>(uW), static_cast<GLsizei>(uH),
                               0, static_cast<GLsizei>(rData.size()), &rData[0]);
    }
    else if (m_kFormat.bCompressed)
    {
        UploadDecodedDxt(uLevel, uW, uH, rData);
    }
    else if (m_kFormat.eSwizzle == SWIZZLE_NONE)
    {
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(uLevel),
                     static_cast<GLint>(m_kFormat.eInternalFormat),
                     static_cast<GLsizei>(uW), static_cast<GLsizei>(uH), 0,
                     m_kFormat.eFormat, m_kFormat.eType, &rData[0]);
    }
    else
    {
        // An intermediate buffer. The source is left alone - it belongs to
        // the client and may be needed after a context loss.
        m_kUploadBuffer.resize(rData.size());
        M2W_SwizzlePixels(m_kFormat, &rData[0], &m_kUploadBuffer[0], uW * uH);
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(uLevel),
                     static_cast<GLint>(m_kFormat.eInternalFormat),
                     static_cast<GLsizei>(uW), static_cast<GLsizei>(uH), 0,
                     m_kFormat.eFormat, m_kFormat.eType, &m_kUploadBuffer[0]);
    }

    m_bUploaded = true;
}

void CGlTexture::UploadDecodedDxt(UINT uLevel, UINT uWidth, UINT uHeight,
                                  const std::vector<unsigned char>& c_rData)
{
    EM2wDxtFormat eFormat;
    switch (m_eFormat)
    {
        case D3DFMT_DXT1: eFormat = M2W_DXT1; break;
        case D3DFMT_DXT3: eFormat = M2W_DXT3; break;
        case D3DFMT_DXT5: eFormat = M2W_DXT5; break;
        default:          return;
    }

    // MIPMAP LEVELS GO BELOW FOUR PIXELS, while a DXT block is always 4x4. A
    // 2x2 level is still ONE whole block - so I decode to whole blocks and
    // crop afterwards.
    //
    // Without that, decoding would refuse the last three levels of every
    // texture: 4x4, 2x2 and 1x1. The symptom would be blurred objects seen
    // from afar, not any error.
    const UINT uBlocksW = (uWidth + 3) / 4;
    const UINT uBlocksH = (uHeight + 3) / 4;
    const UINT uFullWidth = uBlocksW * 4;
    const UINT uFullHeight = uBlocksH * 4;

    std::vector<std::uint32_t> kFull(
        static_cast<size_t>(uFullWidth) * uFullHeight, 0);

    if (!M2W_DecodeDxt(eFormat, &c_rData[0],
                       static_cast<unsigned int>(c_rData.size()),
                       static_cast<int>(uFullWidth),
                       static_cast<int>(uFullHeight),
                       &kFull[0]))
    {
        // We do not pretend. The level stays unsent and the client gets a
        // message - better than a texture made of random memory.
        std::printf("m2w: cannot decode DXT (level %u, %ux%u, "
                    "bytes %u)\n",
                    static_cast<unsigned>(uLevel),
                    static_cast<unsigned>(uWidth),
                    static_cast<unsigned>(uHeight),
                    static_cast<unsigned>(c_rData.size()));
        return;
    }

    // `M2W_DecodeDxt` returns `0xAARRGGBB`, i.e. B, G, R, A in memory.
    // OpenGL ES 3 has no `GL_BGRA`, so we reorder - and crop to the real
    // size of the level on the way.
    m_kUploadBuffer.resize(static_cast<size_t>(uWidth) * uHeight * 4);
    for (UINT y = 0; y < uHeight; ++y)
    {
        for (UINT x = 0; x < uWidth; ++x)
        {
            const std::uint32_t uPixel =
                kFull[static_cast<size_t>(y) * uFullWidth + x];
            unsigned char* q =
                &m_kUploadBuffer[(static_cast<size_t>(y) * uWidth + x) * 4];
            q[0] = static_cast<unsigned char>((uPixel >> 16) & 0xFF);  // R
            q[1] = static_cast<unsigned char>((uPixel >> 8) & 0xFF);   // G
            q[2] = static_cast<unsigned char>(uPixel & 0xFF);          // B
            q[3] = static_cast<unsigned char>((uPixel >> 24) & 0xFF);  // A
        }
    }

    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(uLevel), GL_RGBA8,
                 static_cast<GLsizei>(uWidth), static_cast<GLsizei>(uHeight), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, &m_kUploadBuffer[0]);
}

// ===========================================================================
// CGlSurface
// ===========================================================================

HRESULT CGlSurface::GetDesc(D3DSURFACE_DESC* pDesc)
{
    if (!pDesc)
        return E_FAIL;
    std::memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Format = m_eFormat;
    pDesc->Width = m_uWidth;
    pDesc->Height = m_uHeight;
    return D3D_OK;
}

// ===========================================================================
// CGlLevelSurface
// ===========================================================================

std::map<IDirect3DSurface8*, CGlTexture*> g_kTextureSurfaces;

CGlLevelSurface::CGlLevelSurface(CGlTexture* pOwner, UINT uLevel)
    : m_pTexture(pOwner), m_uLevel(uLevel)
{
    if (m_pTexture)
        m_pTexture->AddRef();
    g_kTextureSurfaces[this] = m_pTexture;
}

CGlLevelSurface::~CGlLevelSurface()
{
    g_kTextureSurfaces.erase(this);
    if (m_pTexture)
        m_pTexture->Release();
}

HRESULT CGlLevelSurface::GetDesc(D3DSURFACE_DESC* pDesc)
{
    if (!m_pTexture)
        return E_FAIL;
    return m_pTexture->GetLevelDesc(m_uLevel, pDesc);
}

HRESULT CGlLevelSurface::LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect,
                                  DWORD Flags)
{
    if (!m_pTexture)
        return E_FAIL;
    return m_pTexture->LockRect(m_uLevel, pLockedRect, pRect, Flags);
}

HRESULT CGlLevelSurface::UnlockRect()
{
    if (!m_pTexture)
        return E_FAIL;
    return m_pTexture->UnlockRect(m_uLevel);
}

HRESULT CGlTexture::GetSurfaceLevel(UINT Level, IDirect3DSurface8** ppSurfaceLevel)
{
    if (!ppSurfaceLevel)
        return E_FAIL;

    if (Level >= m_uLevels)
    {
        *ppSurfaceLevel = NULL;
        return E_FAIL;
    }

    *ppSurfaceLevel = new CGlLevelSurface(this, Level);
    return D3D_OK;
}

// ===========================================================================
// CGlDevice methods (from gl_device.cpp)
// ===========================================================================

// -----------------------------------------------------------------------
// Creating resources
// -----------------------------------------------------------------------

HRESULT CGlDevice::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                 D3DFORMAT Format, D3DPOOL /*Pool*/,
                                 IDirect3DTexture8** ppTexture)
{
    if (!ppTexture)
        return E_FAIL;

    const TTextureFormat kF = M2W_TextureFormat(Format);
    if (!kF.bKnown)
    {
        // We do not pretend. The client gets NULL and has the right to know.
        *ppTexture = NULL;
        std::printf("m2w: unknown texture format %d\n",
                    static_cast<int>(Format));
        return E_FAIL;
    }

    CGlTexture* pOwner = new CGlTexture(Width, Height, Levels, Format);

    // A RENDER TARGET gets a framebuffer - see `PrepareAsTarget`.
    // An ordinary texture pays nothing for it, because it never gets here.
    if (Usage & D3DUSAGE_RENDERTARGET)
    {
        pOwner->PrepareAsTarget();
        // The texture binding changed behind the state memory's back.
        g_bTextureBindingsUnknown = true;
    }

    *ppTexture = pOwner;
    return D3D_OK;
}

HRESULT CGlDevice::SetTexture(DWORD Stage, IDirect3DBaseTexture8* pTexture)
{
    if (Stage >= M2W_TEXTURE_STAGES)
        return D3D_OK;

    // Same here. `GrpImageTexture.cpp` releases the texture on every swap,
    // so without our own reference the stage would point to freed memory
    // until the next `SetTexture`.
    if (pTexture)
        pTexture->AddRef();
    if (m_apTextures[Stage])
        m_apTextures[Stage]->Release();

    m_apTextures[Stage] = static_cast<CGlTexture*>(pTexture);
    return D3D_OK;
}

HRESULT CGlDevice::GetTexture(DWORD Stage, IDirect3DBaseTexture8** ppTexture)
{
    if (!ppTexture)
        return E_FAIL;
    *ppTexture = (Stage < M2W_TEXTURE_STAGES) ? m_apTextures[Stage] : NULL;
    return D3D_OK;
}

void CGlDevice::ApplyTextures(const TProgram& c_rProgram)
{
    // Somebody bound a texture behind the layer's back - see
    // `g_bTextureBindingsUnknown`. Then the binding memory is thrown away.
    if (g_bTextureBindingsUnknown)
    {
        for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
            m_kGlCache.aiBoundKnown[i] = 0;
        m_kGlCache.iActiveUnit = -1;
        g_bTextureBindingsUnknown = false;
    }

    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        const GLuint uName = m_apTextures[i] ? m_apTextures[i]->GlName() : 0;
        if (!m_kGlCache.aiBoundKnown[i] ||
            m_kGlCache.auBound[i] != uName)
        {
            if (m_kGlCache.iActiveUnit != i)
            {
                m_kGlCache.iActiveUnit = i;
                glActiveTexture(GL_TEXTURE0 + i);
            }
            m_kGlCache.aiBoundKnown[i] = 1;
            m_kGlCache.auBound[i] = uName;
            glBindTexture(GL_TEXTURE_2D, uName);
        }

        if (!m_apTextures[i])
            continue;

        const DWORD* a = m_aadwStageState[i];
        const GLint iMinFilter = static_cast<GLint>(
            M2W_MinFilter(a[D3DTSS_MINFILTER], a[D3DTSS_MIPFILTER]));
        const GLint iMagFilter = static_cast<GLint>(
            M2W_MagFilter(a[D3DTSS_MAGFILTER]));
        const GLint iWrapS =
            static_cast<GLint>(M2W_WrapMode(a[D3DTSS_ADDRESSU]));
        const GLint iWrapT =
            static_cast<GLint>(M2W_WrapMode(a[D3DTSS_ADDRESSV]));
        if (m_apTextures[i]->SamplerChanged(iMinFilter, iMagFilter,
                                            iWrapS, iWrapT))
        {
            // The parameters belong to the TEXTURE, so it must be bound -
            // and above we bind it only when it changes.
            if (m_kGlCache.iActiveUnit != i)
            {
                m_kGlCache.iActiveUnit = i;
                glActiveTexture(GL_TEXTURE0 + i);
            }
            glBindTexture(GL_TEXTURE_2D, uName);
            m_apTextures[i]->SetSampler(iMinFilter, iMagFilter,
                                        iWrapS, iWrapT);
        }

        // `D3DTSS_MAXANISOTROPY` is the only stage state the client uses and
        // we do not. Anisotropic filtering needs the
        // `EXT_texture_filter_anisotropic` extension in WebGL 2, which is
        // not mandatory.
        //
        // This is a gap in quality, not in behaviour: `D3DTEXF_ANISOTROPIC`
        // itself is already mapped to linear filtering
        // (`d3d8_states.cpp:253`), so the terrain is smooth, only at very
        // sharp angles less crisp than in the original. The client limits
        // the factor to four anyway (`StateManager.cpp:101`).
    }

    if (c_rProgram.iTexture0 >= 0) glUniform1i(c_rProgram.iTexture0, 0);
    if (c_rProgram.iTexture1 >= 0) glUniform1i(c_rProgram.iTexture1, 1);
}
