// SPDX-License-Identifier: GPL-2.0-or-later
// webbrowser_web.cpp - the browser window inside the client (the item shop):
// `CWebBrowser/CWebBrowser.h` on an `<iframe>` over the canvas
// (m2w.webbrowser*, runtime.js) instead of an embedded Internet Explorer.

// Design:
// `CWebBrowser/CWebBrowser.c` embeds Internet Explorer through OLE
// (`OleCreate`, `IOleObject`, `IOleInPlaceSite`, `IWebBrowser2`); in the
// browser one `<iframe>` element does the job. The window serves one thing
// in Metin2 - the shop for real money - and both notes below follow from
// that.
//
// The whole dance around the Direct3D device disappears:
// `CGraphicDevice::EnableWebBrowserMode` in TMP4 switches `SwapEffect` to
// `D3DSWAPEFFECT_COPY`, `BackBufferCount` to 1 and calls `Reset`, because a
// child window (the IE control) over a Direct3D swap chain needs copying
// instead of swapping, or it flickers. `GrpScreen.cpp` additionally clears
// the screen with FOUR RECTANGLES around the browser window under
// `g_isBrowserMode`. The iframe sits above the canvas in the document tree
// and the page compositor composes it; it cannot be painted over.
// `g_isBrowserMode` and `g_rcBrowser` stay ALIVE nevertheless: the
// four-rectangle clear is still correct (and cheaper), and GrpScreen.cpp
// should behave as before.
//
// A limitation that must not be worked around: the embedded IE control was
// not subject to `X-Frame-Options` or `Content-Security-Policy:
// frame-ancestors`; an `<iframe>` is. A shop sending `DENY` or `SAMEORIGIN`
// shows EMPTY - and that is right: it is the shop's protection against
// being displayed in someone else's frame, the very protection that guards
// the player while paying. The proper answer then is a real tab
// (`window.open`), not a way around the header. Hence `WebBrowser_Show`
// reports only whether the frame exists; `PythonApplicationWebPage.cpp`
// prints `CREATE_WEBBROWSER_ERROR` on 0.

#include <string>

#include "win32_compat.h"

#include <emscripten.h>

// Both lived in `eterLib/GrpDevice.cpp`, which is replaced;
// `GrpScreen.cpp` reads them through `extern` to clear around the shop.
bool g_isBrowserMode = false;
RECT g_rcBrowser = { 0, 0, 0, 0 };

namespace
{

bool g_bVisible = false;

}  // namespace

/// Creates/places the iframe and loads the address; 1 when the frame exists.
EM_JS(int, m2w_webbrowser_show, (const char* c_szUrl, int l, int t, int r, int b), {
    return m2w.webbrowserShow(UTF8ToString(c_szUrl), l, t, r, b);
});

/// Moves the iframe (CSS pixels).
EM_JS(void, m2w_webbrowser_move, (int l, int t, int r, int b), {
    m2w.webbrowserMove(l, t, r, b);
});

/// Hides the iframe and unloads its page.
EM_JS(void, m2w_webbrowser_hide, (void), { m2w.webbrowserHide(); });

/// Removes the iframe from the document.
EM_JS(void, m2w_webbrowser_destroy, (void), { m2w.webbrowserDestroy(); });

// ===========================================================================
// The interface the client uses - signatures from `CWebBrowser/CWebBrowser.h`
// ===========================================================================
// The TMP4 header is not included on purpose: it starts with
// `#include <windows.h>`. Definitions are in a different order than there,
// hence the forward declaration.
extern "C" {

void WebBrowser_Destroy();

/// TMP4 called `OleInitialize` and registered a window class here. Nothing
/// to prepare: the frame is created when first shown.
int WebBrowser_Startup(HINSTANCE)
{
    return 1;
}

/// Same as `WebBrowser_Destroy`.
void WebBrowser_Cleanup()
{
    WebBrowser_Destroy();
}

/// Removes the frame; browser mode off.
void WebBrowser_Destroy()
{
    m2w_webbrowser_destroy();
    g_bVisible = false;
    g_isBrowserMode = false;
}

/// Shows the shop at `rcWebBrowser`; 0 when there is nothing to show in.
int WebBrowser_Show(HWND, const char* c_szAddress, const RECT* c_prcWebBrowser)
{
    if (!c_szAddress || !c_prcWebBrowser)
        return 0;

    const int iOk = m2w_webbrowser_show(c_szAddress,
                                        static_cast<int>(c_prcWebBrowser->left),
                                        static_cast<int>(c_prcWebBrowser->top),
                                        static_cast<int>(c_prcWebBrowser->right),
                                        static_cast<int>(c_prcWebBrowser->bottom));
    if (!iOk)
        return 0;

    g_rcBrowser = *c_prcWebBrowser;
    g_isBrowserMode = true;
    g_bVisible = true;
    return 1;
}

/// Hides the frame; browser mode off.
void WebBrowser_Hide()
{
    m2w_webbrowser_hide();
    g_bVisible = false;
    g_isBrowserMode = false;
}

/// Moves the frame and updates `g_rcBrowser`.
void WebBrowser_Move(const RECT* c_prcWebBrowser)
{
    if (!c_prcWebBrowser)
        return;

    g_rcBrowser = *c_prcWebBrowser;
    m2w_webbrowser_move(static_cast<int>(c_prcWebBrowser->left),
                        static_cast<int>(c_prcWebBrowser->top),
                        static_cast<int>(c_prcWebBrowser->right),
                        static_cast<int>(c_prcWebBrowser->bottom));
}

/// Asked EVERY frame (`PythonApplicationProcedure.cpp`), so answered from
/// C++ state - a JS crossing per frame would be a cost without reason.
int WebBrowser_IsVisible()
{
    return g_bVisible ? 1 : 0;
}

/// The rectangle last shown or moved to.
const RECT& WebBrowser_GetRect()
{
    return g_rcBrowser;
}

}  // extern "C"
