// SPDX-License-Identifier: GPL-2.0-or-later
// grpdevice_gl.cpp - `CGraphicDevice` on the WebGL device: the life cycle
// of the device, the shared buffers and the vertex declarations that the
// whole drawing layer of the client relies on.

// Design:
// WHAT IT REPLACES. `eterLib/GrpDevice.cpp` - creation of the Direct3D
// device: enumerating graphics cards, picking a display mode, a driver
// black list, checking the hardware capabilities, choosing the depth buffer
// format.
//
// In the browser **there is nothing to enumerate**. There is one card - the
// one the browser supplies - no display mode is chosen, and there is no
// driver to black-list. Only the life cycle remains.
//
// HOW MUCH OF THE CLASS IS NEEDED AT ALL. **Twelve symbols** - that is what
// the link contract listed. All the rest of `GrpDevice.h`,
// together with `__CreateDefaultIndexBufferList` and the four functions
// creating stream declarations, is not used outside `GrpDevice.cpp`
// itself. So the **life cycle** is reproduced, not the drawing layer - that
// one sits in the device (`gl_device.cpp`) and in `CStateManager`, which
// compiles unchanged.
//
// THE FOUR FIELDS OF `CGraphicBase` THAT MUST BE FILLED. `GrpBase.cpp`
// compiles here and defines all the static fields. `Create` has to FILL
// them, though, because the rest of the client reads them directly:
//
//   `ms_lpd3dDevice`         - the device; without it every drawing is silent,
//   `ms_d3dPresentParameter` - `GrpScreen.cpp` takes the screen size from here,
//   `ms_d3dCaps`             - `GrpDetector` and the texture quality choice,
//   `ms_iWidth` / `ms_iHeight`.

#include "win32_compat.h"

#include "d3d8_gl.h"

#include "eterLib/GrpDevice.h"
#include "eterLib/StateManager.h"

#include <cstdio>

namespace {

/// The canvas in the document tree. Emscripten creates `#canvas` by
/// default and the client has no say in it - in the browser the window is
/// created by the page, not by the program.
const char* const c_szCanvas = "#canvas";

}  // namespace

CGraphicDevice::CGraphicDevice()
{
    m_uBackBufferCount = 1;
    m_pStateManager = NULL;

    // The tables are static, so they start zeroed anyway - these two calls
    // are here because `__Initialize()` in the original does exactly this.
    // Equivalence with `GrpDevice.cpp` is cheaper than the reasoning "this
    // is redundant anyway".
    __InitializePDTVertexBufferList();
    __InitializeDefaultIndexBufferList();
}

CGraphicDevice::~CGraphicDevice()
{
    Destroy();
}

void CGraphicDevice::InitBackBufferCount(UINT uBackBufferCount)
{
    // In Direct3D the number of back buffers affected smoothness and
    // latency. The browser governs this itself - the swap chain is its own
    // and there is no call that would change it. The value is stored so
    // that `GetDeviceCaps` and `GrpScreen` see what the client set.
    m_uBackBufferCount = uBackBufferCount ? uBackBufferCount : 1;
}

int CGraphicDevice::Create(HWND hWnd, int hres, int vres, bool /*Windowed*/,
                           int /*bit*/, int /*ReflashRate*/)
{
    ms_hWnd = hWnd;
    ms_iWidth = hres;
    ms_iHeight = vres;

    // Diagnostic line: the canvas size once arrived as ZERO and there was
    // no telling whether reading metin2.cfg failed or passing the value on
    // did. One line settles it without guessing.
    std::printf("m2w grp: creating the device %d x %d\n", hres, vres);

    // THE FACTORY MUST EXIST BEFORE THE DEVICE.
    //
    // `CGraphicBase::ms_lpd3d` is `IDirect3D8` - the object that answers
    // the questions about display modes. It draws nothing, so for thirteen
    // parts nobody needed it; it became needed when the client reached
    // `CPythonSystem::GetDisplaySettings`, which read through a null
    // pointer and ended in "memory access out of bounds".
    if (!ms_lpd3d)
        ms_lpd3d = M2W_CreateGlFactory();

    ms_lpd3dDevice = M2W_CreateGlDevice(c_szCanvas, hres, vres);
    if (!ms_lpd3dDevice)
    {
        // `CREATE_NO_DIRECTX` meant "nothing to draw with" - and here it
        // means exactly the same, only about WebGL 2. The client then shows
        // a message instead of entering the game, which is the right thing.
        return CREATE_NO_DIRECTX;
    }

    // --- presentation parameters ------------------------------------------
    // `GrpScreen.cpp` reads the back buffer size from here when clearing
    // the screen in shop mode. They have to agree with the canvas.
    D3DPRESENT_PARAMETERS& r = ms_d3dPresentParameter;
    memset(&r, 0, sizeof(r));
    r.BackBufferWidth = static_cast<UINT>(hres);
    r.BackBufferHeight = static_cast<UINT>(vres);
    r.BackBufferFormat = D3DFMT_A8R8G8B8;
    r.BackBufferCount = m_uBackBufferCount;
    r.Windowed = TRUE;
    r.EnableAutoDepthStencil = TRUE;
    r.AutoDepthStencilFormat = D3DFMT_D16;
    r.SwapEffect = D3DSWAPEFFECT_DISCARD;

    // --- hardware capabilities --------------------------------------------
    ms_lpd3dDevice->GetDeviceCaps(&ms_d3dCaps);

    // THE VERTEX PROCESSING MODE - the second condition of `IsFastTNL`
    //
    // `CGraphicBase::IsFastTNL` (GrpBase.cpp:113) asks TWO things at once:
    // about `VertexShaderVersion` (returned by `M2W_GlCapabilities`) AND
    // whether the device was created with HARDWARE processing. The second
    // condition was never set - `ms_dwD3DBehavior` stayed zero, because the
    // original `GrpDevice.cpp`, which we do not compile, filled it.
    //
    // In the browser the card transforms the vertices, so hardware
    // processing is the TRUE answer here, not a convenient one. Without
    // this line the game picks the software road for drawing the terrain
    // and hands the vertices already in screen coordinates.
    ms_dwD3DBehavior = D3DCREATE_HARDWARE_VERTEXPROCESSING;

    // --- viewport ---------------------------------------------------------
    D3DVIEWPORT8 kViewport;
    kViewport.X = 0;
    kViewport.Y = 0;
    kViewport.Width = static_cast<DWORD>(hres);
    kViewport.Height = static_cast<DWORD>(vres);
    kViewport.MinZ = 0.0f;
    kViewport.MaxZ = 1.0f;
    ms_lpd3dDevice->SetViewport(&kViewport);
    ms_Viewport = kViewport;

    // --- matrix stack -----------------------------------------------------
    // `CGraphicBase::PushMatrix` and its siblings rely on the stack existing.
    D3DXCreateMatrixStack(0, &ms_lpd3dMatStack);
    ms_lpd3dMatStack->LoadIdentity();

    // --- VERTEX LAYOUT DECLARATIONS -----------------------------
    // THE THIRD THING LOST WHEN `GrpDevice.cpp` WAS REPLACED.
    // After the matrices and the buffers - these three
    // handles.
    //
    // `ms_pnt2VS` describes the layout POSITION + NORMAL + TWO SETS OF
    // TEXTURE COORDINATES, i.e. exactly the TERRAIN layout. It stayed zero,
    // so `SetVertexShader(ms_pnt2VS)` told the layer NOTHING and the layer
    // read the vertices by the last known description - and the last one
    // came from the interface (`XYZRHW`, vertices ALREADY transformed).
    //
    // The symptom was misleading to the end: both layouts are 40 BYTES
    // each, so nothing went out of step or crashed. The terrain went to the
    // card as shapes in screen space, with the normals read as colour
    // (208, 201, 201, alpha 3). The measurement from the browser showed it
    // directly: in the whole frame there was NOT ONE draw with the terrain
    // layout.
    ms_ptVS = CreatePTStreamVertexShader();
    ms_pntVS = CreatePNTStreamVertexShader();
    ms_pnt2VS = CreatePNT2StreamVertexShader();

    // --- STATIC MATRICES ----------------------------------------
    // THIS WAS THE BLACK SCREEN. Measurement from the browser: for 400
    // consecutive draw calls `uWorldViewProjection` was ALL ZEROS. A vertex
    // multiplied by a zero matrix gives the point (0,0,0,0) - every
    // triangle is degenerate, so not a single pixel is produced. No OpenGL
    // error, `getError()` = 0, 22 thousand draw calls a second and a black
    // picture.
    //
    // The source: `GrpScreen.cpp:835` sends `ms_matIdentity` as the world
    // matrix for the whole interface. `ms_matIdentity` is a STATIC MEMBER,
    // so it starts zeroed - and the only place that gave it a value was
    // `GrpDevice.cpp:541`. That is, the file we REPLACED.
    //
    // This is the SECOND thing lost in that replacement (the first is the
    // buffers below). The lesson is more general than these two fixes: by
    // replacing the file we took over its API, but not its FOUNDING
    // DUTIES. It did not show for thirty parts because, without a single
    // drawn frame, nobody read those values.
    D3DXMatrixIdentity(&ms_matIdentity);
    D3DXMatrixIdentity(&ms_matView);
    D3DXMatrixIdentity(&ms_matProj);
    D3DXMatrixIdentity(&ms_matInverseView);
    D3DXMatrixIdentity(&ms_matInverseViewYAxis);
    D3DXMatrixIdentity(&ms_matScreen0);
    D3DXMatrixIdentity(&ms_matScreen1);
    D3DXMatrixIdentity(&ms_matScreen2);

    // The triple `ms_matScreen*` takes a point from clip space to screen
    // coordinates: flip the Y axis, move the origin to the corner, scale to
    // half the picture size. The values are from `GrpDevice.cpp` to the
    // sign - `ProjectPosition` uses them, so a mistake would give no
    // error, only the cursor and the character names in wrong places.
    ms_matScreen0._11 = 1;
    ms_matScreen0._22 = -1;

    ms_matScreen1._41 = 1;
    ms_matScreen1._42 = 1;

    ms_matScreen2._11 = static_cast<float>(hres) / 2;
    ms_matScreen2._22 = static_cast<float>(vres) / 2;

    // --- state manager ----------------------------------------------------
    // The heart of the client's drawing layer: it caches states and passes
    // only the changes to the device. Compiles here without a single fix.
    m_pStateManager = new CStateManager(ms_lpd3dDevice);
    STATEMANAGER.SetDefaultState();

    // --- shared buffers -----------------------------------------
    // WHY THEY WERE MISSING FOR THIRTY PARTS: nobody ever drew a single
    // frame. The client considered itself minimised (see
    // `platform_none.cpp`, the message loop fix), so the whole interface
    // drawing path was dead and this absence had no way to show.
    //
    // As soon as `WM_SIZE` started arriving, it showed at once:
    //
    //   CGraphicBase::SetPDTStream  <-  memory access out of bounds
    //   CScreen::RenderBar3d
    //   UI::CBar::OnRender
    //
    // `SetPDTStream` takes `ms_alpd3dPDTVB[n]` and calls `Lock` on it. The
    // table is static, so it was full of zeros - and a virtual call on a
    // null pointer is, in wasm, exactly that message.
    //
    // These three lists were created by `GrpDevice.cpp`, which we REPLACED
    //. The device came over in that replacement; its buffers did
    // not.
    if (!__CreateDefaultIndexBufferList())
        return CREATE_NO_DIRECTX;

    if (!__CreatePDTVertexBufferList())
        return CREATE_NO_DIRECTX;

    return CREATE_OK;
}

// ---------------------------------------------------------------------------
// Vertex layout declarations
// ---------------------------------------------------------------------------
// Content moved FROM `GrpDevice.cpp` UNCHANGED - pure Direct3D 8. The
// register names are part of the CONTRACT with the drawing code: 0 is the
// position, 3 the normal, 7 the first set of texture coordinates. Changing
// any number would give no error, only a picture with the fields mixed up.

DWORD CGraphicDevice::CreatePNTStreamVertexShader()
{
    DWORD declVector[] =
    {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(3, D3DVSDT_FLOAT3),
        D3DVSD_REG(7, D3DVSDT_FLOAT2),
        D3DVSD_END()
    };

    DWORD ret;
    if (FAILED(ms_lpd3dDevice->CreateVertexShader(&declVector[0], NULL, &ret, 0)))
        return 0;
    return ret;
}

DWORD CGraphicDevice::CreatePNT2StreamVertexShader()
{
    DWORD declVector[] =
    {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(3, D3DVSDT_FLOAT3),
        D3DVSD_REG(7, D3DVSDT_FLOAT2),
        D3DVSD_REG(D3DVSDE_TEXCOORD1, D3DVSDT_FLOAT2),
        D3DVSD_END()
    };

    DWORD ret;
    if (FAILED(ms_lpd3dDevice->CreateVertexShader(&declVector[0], NULL, &ret, 0)))
        return 0;
    return ret;
}

DWORD CGraphicDevice::CreatePTStreamVertexShader()
{
    DWORD declVector[] =
    {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_STREAM(1),
        D3DVSD_REG(7, D3DVSDT_FLOAT2),
        D3DVSD_END()
    };

    DWORD ret;
    if (FAILED(ms_lpd3dDevice->CreateVertexShader(&declVector[0], NULL, &ret, 0)))
        return 0;
    return ret;
}

// ---------------------------------------------------------------------------
// Buffers shared by the whole drawing layer
// ---------------------------------------------------------------------------
// Content moved FROM `GrpDevice.cpp` UNCHANGED. It is pure Direct3D 8 - not
// one Windows call - so there is nothing to translate or decide here. The
// index values are part of the CONTRACT with the drawing code: `RenderBar2d`
// relies on `DEFAULT_IB_FILL_RECT` being six indices in the order 0,2,1,
// 2,3,1 - another order would give triangles facing backwards, culled by
// `CULL_CCW`.

void CGraphicDevice::__InitializePDTVertexBufferList()
{
    for (UINT i = 0; i < PDT_VERTEXBUFFER_NUM; ++i)
        ms_alpd3dPDTVB[i] = NULL;
}

void CGraphicDevice::__DestroyPDTVertexBufferList()
{
    for (UINT i = 0; i < PDT_VERTEXBUFFER_NUM; ++i)
    {
        if (ms_alpd3dPDTVB[i])
        {
            ms_alpd3dPDTVB[i]->Release();
            ms_alpd3dPDTVB[i] = NULL;
        }
    }
}

bool CGraphicDevice::__CreatePDTVertexBufferList()
{
    for (UINT i = 0; i < PDT_VERTEXBUFFER_NUM; ++i)
    {
        if (FAILED(
            ms_lpd3dDevice->CreateVertexBuffer(
                sizeof(TPDTVertex) * PDT_VERTEX_NUM,
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                D3DPOOL_SYSTEMMEM,
                &ms_alpd3dPDTVB[i])
        ))
            return false;
    }
    return true;
}

void CGraphicDevice::__InitializeDefaultIndexBufferList()
{
    for (UINT i = 0; i < DEFAULT_IB_NUM; ++i)
        ms_alpd3dDefIB[i] = NULL;
}

void CGraphicDevice::__DestroyDefaultIndexBufferList()
{
    for (UINT i = 0; i < DEFAULT_IB_NUM; ++i)
    {
        if (ms_alpd3dDefIB[i])
        {
            ms_alpd3dDefIB[i]->Release();
            ms_alpd3dDefIB[i] = NULL;
        }
    }
}

bool CGraphicDevice::__CreateDefaultIndexBuffer(UINT eDefIB, UINT uIdxCount,
                                                const WORD* c_awIndices)
{
    assert(ms_alpd3dDefIB[eDefIB] == NULL);

    if (FAILED(
        ms_lpd3dDevice->CreateIndexBuffer(
            sizeof(WORD) * uIdxCount,
            D3DUSAGE_WRITEONLY,
            D3DFMT_INDEX16,
            D3DPOOL_MANAGED,
            &ms_alpd3dDefIB[eDefIB])
    ))
        return false;

    WORD* dstIndices;
    if (FAILED(
        ms_alpd3dDefIB[eDefIB]->Lock(0, 0, (BYTE**)&dstIndices, 0)
    ))
        return false;

    memcpy(dstIndices, c_awIndices, sizeof(WORD) * uIdxCount);

    ms_alpd3dDefIB[eDefIB]->Unlock();

    return true;
}

bool CGraphicDevice::__CreateDefaultIndexBufferList()
{
    static const WORD c_awLineIndices[2] = { 0, 1, };
    static const WORD c_awLineTriIndices[6] = { 0, 1, 0, 2, 1, 2, };
    static const WORD c_awLineRectIndices[8] = { 0, 1, 0, 2, 1, 3, 2, 3, };
    static const WORD c_awLineCubeIndices[24] = {
        0, 1, 0, 2, 1, 3, 2, 3,
        0, 4, 1, 5, 2, 6, 3, 7,
        4, 5, 4, 6, 5, 7, 6, 7,
    };
    static const WORD c_awFillTriIndices[3] = { 0, 1, 2, };
    static const WORD c_awFillRectIndices[6] = { 0, 2, 1, 2, 3, 1, };
    static const WORD c_awFillCubeIndices[36] = {
        0, 1, 2, 1, 3, 2,
        2, 0, 6, 0, 4, 6,
        0, 1, 4, 1, 5, 4,
        1, 3, 5, 3, 7, 5,
        3, 2, 7, 2, 6, 7,
        4, 5, 6, 5, 7, 6,
    };

    if (!__CreateDefaultIndexBuffer(DEFAULT_IB_LINE, 2, c_awLineIndices))
        return false;
    if (!__CreateDefaultIndexBuffer(DEFAULT_IB_LINE_TRI, 6, c_awLineTriIndices))
        return false;
    if (!__CreateDefaultIndexBuffer(DEFAULT_IB_LINE_RECT, 8, c_awLineRectIndices))
        return false;
    if (!__CreateDefaultIndexBuffer(DEFAULT_IB_LINE_CUBE, 24, c_awLineCubeIndices))
        return false;
    if (!__CreateDefaultIndexBuffer(DEFAULT_IB_FILL_TRI, 3, c_awFillTriIndices))
        return false;
    if (!__CreateDefaultIndexBuffer(DEFAULT_IB_FILL_RECT, 6, c_awFillRectIndices))
        return false;
    if (!__CreateDefaultIndexBuffer(DEFAULT_IB_FILL_CUBE, 36, c_awFillCubeIndices))
        return false;

    return true;
}

void CGraphicDevice::Destroy()
{
    __DestroyPDTVertexBufferList();
    __DestroyDefaultIndexBufferList();

    if (m_pStateManager)
    {
        delete m_pStateManager;
        m_pStateManager = NULL;
    }

    if (ms_lpd3dMatStack)
    {
        ms_lpd3dMatStack->Release();
        ms_lpd3dMatStack = NULL;
    }

    if (ms_lpd3dDevice)
    {
        M2W_DestroyGlDevice(ms_lpd3dDevice);
        ms_lpd3dDevice = NULL;
    }

    m_kMap_strWarningMessage.clear();
}

CGraphicDevice::EDeviceState CGraphicDevice::GetDeviceState()
{
    if (!ms_lpd3dDevice)
        return DEVICESTATE_NULL;

    // "Device lost" in Direct3D corresponds to LOSING THE WebGL CONTEXT -
    // an ordinary thing in a browser, when the card sleeps or the system
    // switches between the integrated and the discrete card. The client
    // already knows how to cope, because Direct3D lost the device on a
    // display mode change.
    const HRESULT hr = ms_lpd3dDevice->TestCooperativeLevel();
    if (SUCCEEDED(hr))
        return DEVICESTATE_OK;

    return DEVICESTATE_NEEDS_RESET;
}

bool CGraphicDevice::Reset()
{
    if (!ms_lpd3dDevice)
        return false;

    if (FAILED(ms_lpd3dDevice->Reset(&ms_d3dPresentParameter)))
        return false;

    STATEMANAGER.SetDefaultState();
    return true;
}

// ---------------------------------------------------------------------------
// Shop mode (ItemShop)
// ---------------------------------------------------------------------------
// In TMP4 these three methods RECONFIGURED the Direct3D device: `SwapEffect`
// to `D3DSWAPEFFECT_COPY`, `BackBufferCount` to 1, then `Reset`. The reason
// was purely a system one - a Windows child window (the Internet Explorer
// control) above a Direct3D swap chain required copying instead of swapping
// the buffers.
//
// In the browser the `<iframe>` lies above the canvas in the document tree
// and the page compositor combines them. There is nothing to reconfigure.
// Storing the rectangle remains, because `GrpScreen.cpp` clears the screen
// with four rectangles around the shop window - and that is still correct,
// and cheaper besides.
//
// The variables `g_isBrowserMode` and `g_rcBrowser` themselves live in
// `webbrowser_web.cpp`, next to the window they concern.

extern bool g_isBrowserMode;
extern RECT g_rcBrowser;

void CGraphicDevice::EnableWebBrowserMode(const RECT& c_rcWebPage)
{
    g_rcBrowser = c_rcWebPage;
    g_isBrowserMode = true;
}

void CGraphicDevice::DisableWebBrowserMode()
{
    g_isBrowserMode = false;
}

void CGraphicDevice::MoveWebBrowserRect(const RECT& c_rcWebPage)
{
    g_rcBrowser = c_rcWebPage;
}

// ---------------------------------------------------------------------------

bool CGraphicDevice::ResizeBackBuffer(UINT uWidth, UINT uHeight)
{
    if (!ms_lpd3dDevice)
        return false;

    ms_d3dPresentParameter.BackBufferWidth = uWidth;
    ms_d3dPresentParameter.BackBufferHeight = uHeight;
    ms_iWidth = static_cast<int>(uWidth);
    ms_iHeight = static_cast<int>(uHeight);

    // Resizing the canvas belongs to the PAGE, not to the program - the
    // browser window decides how much room there is. The client learns of
    // it here and adjusts the viewport; the canvas itself is not touched.
    D3DVIEWPORT8 kViewport;
    kViewport.X = 0;
    kViewport.Y = 0;
    kViewport.Width = uWidth;
    kViewport.Height = uHeight;
    kViewport.MinZ = 0.0f;
    kViewport.MaxZ = 1.0f;
    ms_lpd3dDevice->SetViewport(&kViewport);
    ms_Viewport = kViewport;

    // `ms_matScreen2` carries HALF THE PICTURE SIZE, so on a resize it
    // stops being true. TMP4 has no such line and did not need one: there a
    // resolution change went through recreating the device, i.e. through
    // `Create`. Here the window resizes LIVE, because that is how a browser
    // tab works - so this place has to close what `Create` closed there.
    // Otherwise `ProjectPosition` would compute screen coordinates by the
    // size from before the change.
    ms_matScreen2._11 = static_cast<float>(uWidth) / 2;
    ms_matScreen2._22 = static_cast<float>(uHeight) / 2;

    return Reset();
}

void CGraphicDevice::RegisterWarningString(UINT uiMsg, const char* c_szString)
{
    if (c_szString)
        m_kMap_strWarningMessage[uiMsg] = c_szString;
}
