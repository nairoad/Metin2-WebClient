// SPDX-License-Identifier: GPL-2.0-or-later
// gl_device.cpp - the Direct3D 8 device on WebGL 2: its life cycle
// (constructor, destructor, Present, Reset, TestCooperativeLevel), the
// hardware capabilities shared with the factory, and the M2W_* entry points
// (create/destroy the device, road B, program cache size, DXT switch).
// The rest of `CGlDevice` lives in gl_textures.cpp, gl_buffers.cpp,
// gl_shaders.cpp, gl_render_states.cpp and gl_render_target.cpp; the class is declared in gl_internal.h.
//
// The reasons stand next to the code. Up here only what concerns the whole.
//
// ===========================================================================
// PRINCIPLE ONE: `Lock` HANDS OUT OUR BUFFER, NOT DRIVER MEMORY
// ===========================================================================
// In Direct3D `LockRect` and `Lock` returned a pointer to card memory. The
// client wrote there directly, and then `Unlock` gave it back to the card.
// OpenGL has no such mapping in version ES 3 (`glMapBufferRange` exists,
// but not for textures, and not in WebGL).
//
// So every resource keeps **its own copy in wasm memory**, `Lock` hands out
// a pointer to it, and `Unlock` sends it to OpenGL. This is NOT a
// workaround: the client kept its textures in the `D3DPOOL_MANAGED` pool
// anyway (21 of 31 pool uses), which by definition means "the driver keeps a
// copy in system memory and restores the resource from it after the device
// is lost". We do exactly the same, only explicitly.
//
// The gain on the side is that **loss of the WebGL context** - an ordinary
// thing in a browser, when the card sleeps or the graphics card is switched
// - can be handled, because all the data is on our side.
//
// ===========================================================================
// PRINCIPLE TWO: DEPTH. ONE PLACE, ONE FIX
// ===========================================================================
// Direct3D projects depth into **[0,1]**, OpenGL into **[-1,1]**. `d3dx8.h`
// says so explicitly at `D3DXMatrixPerspectiveFovRH` and refers "to one
// place in the graphics layer". This is that place.
//
// The fix is a multiplication of the projection matrix by the transform
// `z' = 2z - w`. It does not touch X or Y, so **it does not reverse the face
// winding** - and that is the assumption `M2W_CullMode` in
// `d3d8_states.cpp` stands on. If someone ever handled depth differently,
// that would have to be recomputed.
//
// ===========================================================================
// PRINCIPLE THREE: STATE IS APPLIED AT DRAW TIME, NOT WHEN IT IS SET
// ===========================================================================
// Direct3D's rule is "set the states, then draw". The client sets dozens of
// them between one draw and the next, often the same ones repeatedly.
// `CStateManager` already filters repetitions, but still: applying state on
// every `SetRenderState` would mean hundreds of OpenGL calls per frame with
// no effect.
//
// So we record the state into arrays, and tell OpenGL about it once - at
// draw time. Only then is it known which program is needed, too.
//
// ===========================================================================
// WHAT IS NOT HERE YET - SAID PLAINLY
// ===========================================================================
// **Real shaders.** `GrpVertexShader.cpp` and `GrpPixelShader.cpp` load
// compiled Direct3D 8 assembly from files. We do not translate it: drawing
// goes through the programs composed in gl_shaders.cpp instead. That is
// separate work, not a gap in this file.
//
// Until then `CreateVertexShader` with a real body hands out a handle, and
// drawing with such a handle is **skipped with one warning**. Skipping is
// better here than pretending: an image without the effect is visible at
// once, an image with the effect computed any old way - is not.
//
// Translated (the orphaned notes about DXT support and the draw
// census, left here by earlier moves, removed - their text lives
// at the code in gl_internal.h and gl_buffers.cpp).

#include <emscripten/emscripten.h>

#include <map>
#include "stubs.h"
#include <set>
#include <string>
#include <vector>

#include "win32_compat.h"

#include "d3d8_gl.h"
#include "gl_internal.h"
#include "d3d8_fixedfunc.h"
#include "d3d8_fvf.h"
#include "dxt.h"
#include <cstdarg>

#include "d3d8_states.h"
#include "custom_draw.h"
#include "frame_stats.h"

/// `?tnl=0` - back to software vertex transformation. 1 = forced.
EM_JS(int, m2w_software_tnl, (void), {
    return m2w.options.get('tnl') === '0' ? 1 : 0; });

/// `?tnl=0` - go back to software vertex transformation. The switch exists
/// so that both terrain roads can be compared WITHOUT a rebuild, in the same
/// session. See the note at `VertexShaderVersion` in `M2W_GlCapabilities`.
static bool M2W_ForcedSoftwareTnl()
{
    static int s_iMode = -1;
    if (s_iMode < 0)
        s_iMode = m2w_software_tnl();
    return s_iMode != 0;
}

// ===========================================================================
// Hardware capabilities - ONE answer, two askers
// ===========================================================================
// Both the device (`IDirect3DDevice8::GetDeviceCaps`) and the factory
// (`IDirect3D8::GetDeviceCaps`) ask the same thing. In Direct3D both answered
// the same, because the question is about the CARD, not about the object
// asked.
//
// So I keep it in one place. Two copies of this function would differ from
// the first day someone fixed only one - and the client would get two
// different answers depending on whom it asked.
HRESULT M2W_GlCapabilities(D3DCAPS8* pCaps)
{
    if (!pCaps)
        return E_FAIL;

    std::memset(pCaps, 0, sizeof(*pCaps));

    GLint iMax = 2048;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &iMax);
    pCaps->MaxTextureWidth = static_cast<DWORD>(iMax);
    pCaps->MaxTextureHeight = static_cast<DWORD>(iMax);

    GLint iUnits = 8;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &iUnits);
    pCaps->MaxSimultaneousTextures = static_cast<DWORD>(iUnits);
    pCaps->MaxTextureBlendStages = static_cast<DWORD>(iUnits);

    pCaps->MaxAnisotropy = 1;

    // HARDWARE VERTEX TRANSFORMATION - WE TELL THE TRUTH.
    //
    // Earlier the whole `D3DCAPS8` was zeroed, so `VertexShaderVersion`
    // was 0. The game reads it in `CGraphicBase::IsFastTNL` (GrpBase.cpp:113)
    // and chooses the TERRAIN DRAWING ROAD by it:
    //
    //     IsFastTNL() == false  ->  ReserveSoftwareTilingEnable(true)
    //                           ->  the terrain goes the SOFTWARE road (STP)
    //
    // Measured in the user's game (read from the console):
    //     terrain: frame 153000 - 48 patches, software road (STP)
    //
    // And that is untrue about our device: in WebGL 2 the CARD transforms the
    // vertices. The software road computes them on the CPU and passes them
    // already in screen coordinates (`D3DFVF_XYZRHW`) - i.e. a completely
    // different path in our layer, one we have not written properly.
    //
    // WHY THIS IS SAFE - checked, not assumed. `VertexShaderVersion` has
    // **one** live use in the whole compiled tree: `IsFastTNL` itself. The
    // second occurrence sits in `eterLib/GrpDevice.cpp`, which we do not
    // compile (it is on the replaced list). Raising this number therefore
    // does NOT switch on any Direct3D shader path - and our layer SKIPS such
    // paths at draw time, so a mistake here would be expensive.
    //
    // `?tnl=0` goes back to the software road, should a comparison be needed.
    if (!M2W_ForcedSoftwareTnl())
    {
        pCaps->VertexShaderVersion = D3DVS_VERSION(1, 1);
        pCaps->MaxVertexShaderConst = 96;
    }
    return D3D_OK;
}

// Matrices - `D3DXMatrixMultiply` and `D3DXMatrixIdentity`.
#include "d3dx8.h"

#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include <cstdio>
#include <cstring>

// ===========================================================================
// The device
// ===========================================================================

namespace {
/// The device the game draws on. The port creates it once and never changes
/// it, so one pointer is enough - and `M2W_DrawModelCustom` has to get the
/// state from somewhere, because it is called from game code that does not
/// see the device.
CGlDevice* g_pCurrentDevice = NULL;

}  // namespace

// `CGlDevice` is declared in gl_internal.h; here the bodies of its
// life-cycle methods. Section banners stand in both places, `///`
// descriptions at the declaration, reasons `//` at the body.

CGlDevice::CGlDevice(int iWidth, int iHeight)
    : m_iWidth(iWidth), m_iHeight(iHeight)
{
    M2W_DefaultPipelineKey(&m_kKey);

    std::memset(m_adwRenderState, 0, sizeof(m_adwRenderState));
    std::memset(m_apTextures, 0, sizeof(m_apTextures));

    // Direct3D's initial values that MATTER. The rest is zero, and that
    // matches the truth.
    m_adwRenderState[D3DRS_ZENABLE] = TRUE;
    m_adwRenderState[D3DRS_ZWRITEENABLE] = TRUE;
    m_adwRenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    m_adwRenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
    m_adwRenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    m_adwRenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    m_adwRenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    m_adwRenderState[D3DRS_LIGHTING] = TRUE;
    m_adwRenderState[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    m_adwRenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    // Four bits, i.e. "write all channels". Zero would mean "write nothing"
    // - and that is a black screen without a single error.
    m_adwRenderState[D3DRS_COLORWRITEENABLE] =
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;

    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        m_aadwStageState[i][D3DTSS_MINFILTER] = D3DTEXF_POINT;
        m_aadwStageState[i][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
        m_aadwStageState[i][D3DTSS_MIPFILTER] = D3DTEXF_NONE;
        m_aadwStageState[i][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
        m_aadwStageState[i][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    }

    D3DXMatrixIdentity(&m_matWorld);
    D3DXMatrixIdentity(&m_matView);
    D3DXMatrixIdentity(&m_matProjection);
    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
        D3DXMatrixIdentity(&m_amatTexture[i]);

    std::memset(&m_kMaterial, 0, sizeof(m_kMaterial));
    std::memset(m_akLights, 0, sizeof(m_akLights));
    for (int i = 0; i < LIGHT_COUNT; ++i)
        m_abLightEnabled[i] = false;

    glGenVertexArrays(1, &m_uVertexArray);
    glBindVertexArray(m_uVertexArray);
    glGenBuffers(1, &m_uScratchBuffer);
    glGenBuffers(1, &m_uScratchIndexBuffer);
}

CGlDevice::~CGlDevice()
{
    for (std::map<DWORD, std::vector<TProgramCacheEntry> >::iterator it =
             m_kProgramCache.begin(); it != m_kProgramCache.end(); ++it)
        for (size_t i = 0; i < it->second.size(); ++i)
            glDeleteProgram(it->second[i].kProgram.uProgram);

    // Give back the references the device held.
    if (m_pStream) m_pStream->Release();
    if (m_pIndices) m_pIndices->Release();
    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
        if (m_apTextures[i]) m_apTextures[i]->Release();

    if (m_pTargetDesc) m_pTargetDesc->Release();
    if (m_pDepthDesc) m_pDepthDesc->Release();

    if (m_uScratchBuffer) glDeleteBuffers(1, &m_uScratchBuffer);
    if (m_uScratchIndexBuffer) glDeleteBuffers(1, &m_uScratchIndexBuffer);
    if (m_uVertexArray) glDeleteVertexArrays(1, &m_uVertexArray);
}

HRESULT CGlDevice::Present(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*)
{
    // In the browser the image reaches the screen when the frame callback
    // returns to the browser. There is nothing to "present" - and that is not
    // a stub, but the truth about this platform.
    return D3D_OK;
}

HRESULT CGlDevice::Reset(D3DPRESENT_PARAMETERS* pParams)
{
    std::printf("m2w grp: Reset %u x %u\n", pParams ? (unsigned)pParams->BackBufferWidth : 0u,
                pParams ? (unsigned)pParams->BackBufferHeight : 0u);
    if (pParams)
    {
        m_iWidth = static_cast<int>(pParams->BackBufferWidth);
        m_iHeight = static_cast<int>(pParams->BackBufferHeight);
        // The drawing buffer size has to follow what the client considers the
        // screen - see the note at device creation.
        emscripten_set_canvas_element_size("#canvas", ToPhysical(m_iWidth),
                                           ToPhysical(m_iHeight));
        // After `Reset` Direct3D has the viewport equal to the whole new
        // buffer. Without this the viewport computed at the OLD
        // height stayed (`SetViewport` subtracts from the target height) -
        // after shrinking the window from 1278 to 600 px the image lay 678 px
        // above the canvas: a black screen (measured: glViewport
        // (0,678,900,600)).
        m_kViewport.X = 0; m_kViewport.Y = 0;
        m_kViewport.Width = static_cast<DWORD>(m_iWidth);
        m_kViewport.Height = static_cast<DWORD>(m_iHeight);
        m_kViewport.MinZ = 0.0f; m_kViewport.MaxZ = 1.0f;
        if (!m_pTargetTexture)
            glViewport(0, 0, ToPhysical(m_iWidth), ToPhysical(m_iHeight));
    }
    return D3D_OK;
}

HRESULT CGlDevice::TestCooperativeLevel()
{
    // A lost device in Direct3D corresponds to a lost WebGL context - and
    // ONLY to that. Earlier a heuristic stood here: "depth test off
    // and some GL error queued = device lost"; one GL 0x502 (measured:
    // `forest 1: GL error 502 before drawing`) in a frame with the depth
    // test off gave DEVICELOST, `RestoreDevice` returned FALSE and the game
    // stopped drawing FOR GOOD (a black screen, the loop at 60 FPS,
    // `potok D3D8 0 wywolan`).
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE hContext = emscripten_webgl_get_current_context();
    const bool bLost = hContext && emscripten_is_webgl_context_lost(hContext);
    static int s_iReports = 0;
    if (bLost && s_iReports < 5)
    {
        ++s_iReports;
        std::printf("m2w grp: TestCooperativeLevel -> DEVICELOST (context %d)\n", (int)hContext);
    }
    return bLost ? D3DERR_DEVICELOST : D3D_OK;
}

HRESULT CGlDevice::GetDeviceCaps(D3DCAPS8* pCaps)
{
    // One place for this answer - see `M2W_GlCapabilities`.
    return M2W_GlCapabilities(pCaps);
}

UINT CGlDevice::GetAvailableTextureMem()
{
    // OpenGL does not answer this question and there is no way to guess.
    // The client uses it to choose texture quality; we return a value that
    // means "plenty", instead of a zero meaning "nothing will fit".
    return 256u * 1024u * 1024u;
}

int CGlDevice::ToPhysical(int iLogical)
{
    return static_cast<int>(iLogical * M2W_UiScale() + 0.5f);
}

// ===========================================================================
// Creation
// ===========================================================================

void M2W_ForceDxtDecode(bool bForce)
{
    g_bForcedDxtDecode = bForce;
}

IDirect3DDevice8* M2W_CreateGlDevice(const char* c_szCanvas,
                                     int iWidth, int iHeight)
{
    EmscriptenWebGLContextAttributes kAttributes;
    emscripten_webgl_init_context_attributes(&kAttributes);
    kAttributes.majorVersion = 2;         // WebGL 2, i.e. OpenGL ES 3
    kAttributes.minorVersion = 0;
    kAttributes.depth = EM_TRUE;
    kAttributes.stencil = EM_TRUE;
    kAttributes.antialias = EM_FALSE;     // the client smooths itself, if it wants
    kAttributes.alpha = EM_FALSE;         // an opaque canvas - cheaper
    kAttributes.preserveDrawingBuffer = EM_FALSE;

    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE hContext =
        emscripten_webgl_create_context(c_szCanvas ? c_szCanvas : "#canvas",
                                        &kAttributes);
    if (hContext <= 0)
    {
        // A true answer: there are machines and browsers without WebGL 2.
        std::printf("m2w: no WebGL 2 - the device cannot be created\n");
        return NULL;
    }

    emscripten_webgl_make_context_current(hContext);

    // Extensions in WebGL have to be ENABLED, not just present. Emscripten
    // does not do it itself - and that is a trap, because
    // `glGetString(GL_EXTENSIONS)` can list an extension whose calls will
    // not work anyway until it is enabled.
    //
    // The returned value is the only answer that can be trusted here.
    g_iGpuHasDxt =
        emscripten_webgl_enable_extension(hContext,
                                          "WEBGL_compressed_texture_s3tc")
        ? 1 : 0;
    if (!g_iGpuHasDxt)
        std::printf("m2w: no WEBGL_compressed_texture_s3tc - DXT textures "
                    "will be decoded at load time\n");

    // THE CANVAS SIZE HAS TO BE SET.
    //
    // Emscripten creates the canvas at the default size - **300 by 150** - and
    // does not change it because we asked for an 800 by 600 device. The
    // drawing buffer then has a different size than what the client
    // considers the screen.
    //
    // The symptom is nasty because it is PARTIAL: the middle of the image
    // draws correctly, and everything outside the 300x150 rectangle does not
    // exist. The second-frame test showed it as "the top half of the texture
    // black" - and for a while it looked like a texture sampling bug, not the
    // canvas size.
    // the game gives the LOGICAL size, the canvas gets the
    // PHYSICAL one.
    emscripten_set_canvas_element_size(c_szCanvas ? c_szCanvas : "#canvas",
                                       CGlDevice::ToPhysical(iWidth), CGlDevice::ToPhysical(iHeight));

    CGlDevice* pDevice = new CGlDevice(iWidth, iHeight);
    g_pCurrentDevice = pDevice;
    return pDevice;
}

void M2W_DestroyGlDevice(IDirect3DDevice8* pDevice)
{
    if (pDevice == g_pCurrentDevice)
        g_pCurrentDevice = NULL;
    if (pDevice)
        pDevice->Release();
}

bool M2W_DrawModelCustom(uint32_t uFirstIndex, uint32_t uTriangles)
{
    if (!g_pCurrentDevice)
        return false;
    return g_pCurrentDevice->DrawCustom(uFirstIndex, uTriangles);
}

void CGlDevice::ClipHalfPixel(float* pfX, float* pfY, bool bFlipAlreadyApplied) const
{
    // The viewport is kept in LOGICAL px on the screen (GUI scale) and in
    // texels on a texture target - the same split as SetViewport.
    const bool bScreen = !m_pTargetTexture;
    const int iW = bScreen ? ToPhysical(static_cast<int>(m_kViewport.Width)) : static_cast<int>(m_kViewport.Width);
    const int iH = bScreen ? ToPhysical(static_cast<int>(m_kViewport.Height)) : static_cast<int>(m_kViewport.Height);
    *pfX = iW > 0 ? 1.0f / static_cast<float>(iW) : 0.0f;
    *pfY = iH > 0 ? 1.0f / static_cast<float>(iH) : 0.0f;
    // Road A shifts, then the shader flips (`gl_Position.y *= uFlipY`) - the
    // shift is mirrored with the geometry. Road B flips the projection first
    // (gl_buffers.cpp, TargetFlipped) and shifts after, so its shift must be
    // mirrored here to land on the same pixels (reviewer).
    if (bFlipAlreadyApplied && TargetFlipped())
        *pfY = -*pfY;
}

void M2W_ClipHalfPixel(float* pfX, float* pfY)
{
    if (!g_pCurrentDevice)
    {
        *pfX = 0.0f;
        *pfY = 0.0f;
        return;
    }
    g_pCurrentDevice->ClipHalfPixel(pfX, pfY, true);
}

int M2W_ProgramCacheSize(IDirect3DDevice8* pDevice)
{
    if (!pDevice)
        return 0;
    return static_cast<CGlDevice*>(pDevice)->CachedProgramCount();
}
