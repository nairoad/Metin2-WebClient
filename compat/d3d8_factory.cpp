// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_factory.cpp - `IDirect3D8`, i.e. THE QUESTIONS ABOUT THE HARDWARE:
// adapters, display modes, format support, capabilities - answered for a
// browser window, truthfully.

// Design:
// WHAT WAS MISSING. The port's drawing layer had its own `IDirect3DDevice8`
// - enough for drawing. But Direct3D 8 has TWO objects, not
// one:
//   `IDirect3D8`        THE FACTORY. Answers "which cards are in the
//                       computer", "which display modes they support", "does
//                       this texture format work".
//   `IDirect3DDevice8`  THE DEVICE. Draws.
// There was no factory and `CGraphicBase::ms_lpd3d` stayed null. The
// symptom came only when the client got that far:
//     RuntimeError: memory access out of bounds
//       at CPythonSystem::GetDisplaySettings()
//       at CPythonApplication::Create(...)
// `GetDisplaySettings` calls `lpD3D->GetAdapterIdentifier(...)` on a null
// pointer. Not a failure of the drawing layer - a hole in its picture of
// what the client asks. The call trace came from `-g2` and from catching
// rejected promises in the measurement page; without those two it was
// `wasm-function[30218]`.
//
// WHAT IS ANSWERED, and why exactly that. The browser has no notion of a
// "display mode". The canvas can have any size and one line changes it -
// there is no list of modes the card "supports", because there is no
// screen switching. But the client NEEDS a list: it builds the display
// settings menu out of it (`GetDisplaySettings` fills `m_ResolutionList`),
// and an empty menu means "no hardware" to it. So a list of sizes that
// REALLY could be set in the browser window is given - with the current
// window size first. Not invention: every one of them works if the user
// picks it. Sizes LARGER than the window are rejected: the client would not
// show them anyway (`GetDisplaySettings` sieves below 800x600), and picking
// a size larger than the window would give a cropped image - worse than no
// choice.

#include <cstdio>
#include <cstring>
#include <vector>

#include "win32_compat.h"
#include "d3d8.h"
#include "d3d8_gl.h"

#include <emscripten/emscripten.h>

namespace
{

/// The window size in LOGICAL pixels: the same measure as
/// `GetSystemMetrics` (platform_none.cpp, `m2w.windowSize`), or the game
/// would see two different screen sizes from two APIs.
EM_JS(int, m2w_display_mode_size, (int bHeight), { return m2w.windowSize(bHeight); });

/// The sizes offered as "display modes". Chosen to cover what the client
/// uses anyway: the settings menu shows modes from 800x600 up only.
const struct { UINT uWidth; UINT uHeight; } c_aModes[] = {
    {  800,  600 },
    { 1024,  768 },
    { 1280,  720 },
    { 1280, 1024 },
    { 1366,  768 },
    { 1600,  900 },
    { 1920, 1080 },
};

const int C_MODES = static_cast<int>(sizeof(c_aModes) / sizeof(c_aModes[0]));

/// The one adapter: WebGL 2, with the window's size as its desktop mode.
class CGlFactory : public IDirect3D8
{
public:
    /// One reference; builds the display-mode list from the current window.
    CGlFactory() : m_uRefs(1) { BuildModes(); }

    /// Adds a reference; returns the new count.
    ULONG AddRef() override { return ++m_uRefs; }

    /// Drops a reference and deletes the factory at zero; returns the new count.
    ULONG Release() override
    {
        const ULONG u = --m_uRefs;
        if (u == 0)
            delete this;
        return u;
    }

    /// Always one adapter - the browser's WebGL 2.
    UINT GetAdapterCount() override { return 1; }

    /// Driver "WebGL 2", a description naming the browser, WHQL level 1, the
    /// rest zero; E_FAIL for NULL.
    HRESULT GetAdapterIdentifier(UINT, DWORD,
                                 D3DADAPTER_IDENTIFIER8* pIdentifier) override
    {
        if (!pIdentifier)
            return E_FAIL;

        std::memset(pIdentifier, 0, sizeof(*pIdentifier));
        // The name goes to the "about" window and to the log. The truth:
        // WebGL 2 draws, not an invented card.
        std::snprintf(pIdentifier->Driver, sizeof(pIdentifier->Driver),
                      "%s", "WebGL 2");
        std::snprintf(pIdentifier->Description,
                      sizeof(pIdentifier->Description),
                      "%s", "browser (WebGL 2 through emscripten)");
        pIdentifier->WHQLLevel = 1;
        return D3D_OK;
    }

    /// Number of modes built by `BuildModes`.
    UINT GetAdapterModeCount(UINT) override
    {
        return static_cast<UINT>(m_vecModes.size());
    }

    /// Mode `uMode` of the list; E_FAIL for NULL or an index out of range.
    HRESULT EnumAdapterModes(UINT, UINT uMode, D3DDISPLAYMODE* pMode) override
    {
        if (!pMode || uMode >= m_vecModes.size())
            return E_FAIL;
        *pMode = m_vecModes[uMode];
        return D3D_OK;
    }

    /// The CURRENT window size (read on every call), 60 Hz, X8R8G8B8; E_FAIL
    /// for NULL.
    HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE* pMode) override
    {
        if (!pMode)
            return E_FAIL;

        // The "desktop mode" is here the current window size. Read EVERY
        // TIME, not from a remembered value - a browser window can be
        // resized at any moment, a Windows desktop cannot.
        pMode->Width = static_cast<UINT>(m2w_display_mode_size(0));
        pMode->Height = static_cast<UINT>(m2w_display_mode_size(1));
        pMode->RefreshRate = 60;
        pMode->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }

    /// Always D3D_OK - there is one device type.
    HRESULT CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT,
                            BOOL) override
    {
        return D3D_OK;
    }

    /// D3D_OK only for the formats the GL layer can translate (A8R8G8B8,
    /// X8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, DXT1/3/5, D16); E_FAIL otherwise.
    HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD,
                              D3DRESOURCETYPE, D3DFORMAT eFormat) override
    {
        // Answered TRUTHFULLY, not "yes" to everything. Formats that
        // `d3d8_states.cpp` cannot translate to WebGL get a refusal - so the
        // client picks another one instead of getting a texture full of
        // garbage.
        switch (eFormat)
        {
            case D3DFMT_A8R8G8B8:
            case D3DFMT_X8R8G8B8:
            case D3DFMT_R5G6B5:
            case D3DFMT_A1R5G5B5:
            case D3DFMT_A4R4G4B4:
            case D3DFMT_DXT1:
            case D3DFMT_DXT3:
            case D3DFMT_DXT5:
            case D3DFMT_D16:
                return D3D_OK;
            default:
                return E_FAIL;
        }
    }

    /// Always D3D_OK.
    HRESULT CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT,
                                   D3DFORMAT) override
    {
        return D3D_OK;
    }

    /// The same capabilities as the device (`M2W_GlCapabilities`); E_FAIL for
    /// NULL.
    HRESULT GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS8* pCaps) override
    {
        if (!pCaps)
            return E_FAIL;
        // The same capabilities the device returns - otherwise the client
        // would get two different answers to one question, depending on
        // whom it asked.
        return M2W_GlCapabilities(pCaps);
    }

    /// Refuses: sets `*ppDevice` to NULL, prints why and returns E_FAIL - the
    /// device is created by `CGraphicDevice::Create` (grpdevice_gl.cpp).
    HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD,
                         D3DPRESENT_PARAMETERS*,
                         IDirect3DDevice8** ppDevice) override
    {
        // The device is created by `CGraphicDevice::Create` directly, because
        // it has to know the canvas selector. This road is not used, and
        // that is said instead of returning something that would not work.
        if (ppDevice)
            *ppDevice = NULL;
        std::printf("m2w d3d8: CreateDevice through the factory is not used - "
                    "CGraphicDevice::Create creates the device\n");
        return E_FAIL;
    }

private:
    /// Private: deleted only by `Release`.
    ~CGlFactory() {}

    /// Builds the mode list: the current window size PLUS those of the
    /// constant table that fit in the window.
    void BuildModes()
    {
        const UINT uWindowWidth = static_cast<UINT>(m2w_display_mode_size(0));
        const UINT uWindowHeight = static_cast<UINT>(m2w_display_mode_size(1));

        D3DDISPLAYMODE kCurrent;
        kCurrent.Width = uWindowWidth;
        kCurrent.Height = uWindowHeight;
        kCurrent.RefreshRate = 60;
        kCurrent.Format = D3DFMT_X8R8G8B8;

        // The current size FIRST - the client takes from the list the one
        // matching the saved settings, and when it finds none, the first
        // one at hand.
        if (kCurrent.Width >= 800 && kCurrent.Height >= 600)
            m_vecModes.push_back(kCurrent);

        for (int i = 0; i < C_MODES; ++i)
        {
            if (c_aModes[i].uWidth > uWindowWidth || c_aModes[i].uHeight > uWindowHeight)
                continue;
            if (c_aModes[i].uWidth == kCurrent.Width &&
                c_aModes[i].uHeight == kCurrent.Height)
                continue;

            D3DDISPLAYMODE kMode;
            kMode.Width = c_aModes[i].uWidth;
            kMode.Height = c_aModes[i].uHeight;
            kMode.RefreshRate = 60;
            kMode.Format = D3DFMT_X8R8G8B8;
            m_vecModes.push_back(kMode);
        }

        // A window smaller than 800x600 leaves the list EMPTY - and that is
        // the truth, not a defect. The client then shows that there is no
        // mode to choose, instead of offering a size that does not fit.
        if (m_vecModes.empty())
        {
            std::printf("m2w d3d8: the window %u x %u is smaller than 800 x 600 - "
                        "the display mode list is empty\n",
                        uWindowWidth, uWindowHeight);
        }
    }

    ULONG m_uRefs;
    std::vector<D3DDISPLAYMODE> m_vecModes;
};

}  // namespace

IDirect3D8* M2W_CreateGlFactory()
{
    return new CGlFactory();
}

// `Direct3DCreate8` of the original API. `GrpDevice.cpp` called exactly
// this - and although that file is replaced here, the name stays, because
// any other file of the tree may ask for it.
IDirect3D8* Direct3DCreate8(UINT)
{
    return M2W_CreateGlFactory();
}
