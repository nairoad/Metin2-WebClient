// SPDX-License-Identifier: GPL-2.0-or-later
// platform_none.cpp - Win32 calls whose TRUE answer in the browser is "there
// is no such thing" (processes, windows, display modes, resources), plus the
// thin window layer the client cannot do without: the message queue carrying
// WM_SIZE and input, the canvas handle, screen metrics and the quit path.

// Design:
// Now the project keeps the rule "a body when the equivalent is
// exact; a declaration without a body when a choice has to be made". This
// file is the third case: a function whose real answer here is "nothing".
// `Process32First` returning FALSE does not lie - a web page really has no
// other processes to list; `FindWindow` returning NULL does not lie - there
// are no Windows windows. Unlike a stub, which says "done" having done
// nothing, these say "not done" or "empty" - which is what is. Hence the
// file name.
//
// Not here: `CreateFontIndirectA`, `TextOutW`, `D3DXCreateTexture`. The game
// USES their result, so "nothing" would be a lie that surfaces on screen;
// they live in the drawing layer.
//
// Lessons this file carries (each cost a black screen or a hang; details in
// the chronicle parts named at the functions): a zero screen size is read
// by TMP4 as a screen of width zero, not as "unknown"; an empty
// message queue is read as "nothing ever changed", so WM_SIZE must be
// carried or the client thinks it is minimised forever;
// `ShowCursor` is a counter the client LOOPS on, not a constant;
// `GetCursorPos` returning FALSE hides the pointer from the whole UI (part
// 205); `CreateWindowExA` returning NULL means "failed", not "not mine"
// The JavaScript behind the measures is in runtime.js.

#include <cstring>
#include <cstdio>

#include "win32_compat.h"

#include <emscripten/emscripten.h>
#include "dinput.h"
#include "tlhelp32.h"

// ---------------------------------------------------------------------------
// Other processes and modules - the anti-cheat layer
// ---------------------------------------------------------------------------
// A page does not see the operating system; the browser withholds other
// processes on purpose. Said plainly: THIS PROTECTION DOES NOT WORK in the
// port. Enumeration returns empty, so the scanners find nothing - foreign or
// own - and no function pretends to have checked.

/// Win32: no process snapshot in a browser - `INVALID_HANDLE_VALUE`.
HANDLE CreateToolhelp32Snapshot(DWORD /*dwFlags*/, DWORD /*th32ProcessID*/)
{
    // `INVALID_HANDLE_VALUE` is how Win32 says "no snapshot"; TMP4 tests
    // for it, so it gets an answer it can handle.
    return INVALID_HANDLE_VALUE;
}

BOOL Process32First(HANDLE /*hSnapshot*/, LPPROCESSENTRY32 /*lppe*/) { return FALSE; }
BOOL Process32Next(HANDLE /*hSnapshot*/, LPPROCESSENTRY32 /*lppe*/)  { return FALSE; }
BOOL Module32First(HANDLE /*hSnapshot*/, LPMODULEENTRY32 /*lpme*/)   { return FALSE; }
BOOL Module32Next(HANDLE /*hSnapshot*/, LPMODULEENTRY32 /*lpme*/)    { return FALSE; }

/// Win32: no other processes - NULL.
HANDLE OpenProcess(DWORD /*dwAccess*/, BOOL /*bInherit*/, DWORD /*dwProcessId*/)
{
    return NULL;
}

/// Win32: FALSE, zero bytes read.
BOOL ReadProcessMemory(HANDLE /*hProcess*/, const void* /*c_pvAddress*/, void* /*pvBuffer*/,
                       size_t /*uSize*/, size_t* puRead)
{
    // Win32 stores the bytes read even on failure; zeroing is part of the
    // contract, not politeness.
    if (puRead) *puRead = 0;
    return FALSE;
}

// ---------------------------------------------------------------------------
// Windows and resources
// ---------------------------------------------------------------------------

/// Win32: NULL - there are no Windows windows to find.
HWND FindWindowA(LPCSTR /*c_szClassName*/, LPCSTR /*c_szWindowName*/)
{
    return NULL;    // no Windows windows to find
}

/// Win32: NULL - the page's `<link rel="icon">` is the window icon.
HICON LoadIconA(HINSTANCE /*hInstance*/, LPCSTR /*c_szName*/)
{
    return NULL;    // the page's `<link rel="icon">` is the window icon
}

/// Win32: cursors come from cursor_web.cpp; every other resource is NULL.
void* LoadImageA(HINSTANCE /*hInstance*/, LPCSTR c_szName, UINT uType,
                 int /*cx*/, int /*cy*/, UINT /*uLoad*/)
{
    // CURSORS ARE THE EXCEPTION. The rest of the `.rc` resources do not
    // exist in the port - interface images come through the pack and the
    // texture layer - but cursors are a PROPERTY OF THE CANVAS the browser
    // has built in (cursor_web.cpp). Returning NULL meant "failed to load"
    // and stopped the start: `CreateCursors` loads fifteen and aborts on
    // the first miss.
    if (uType == IMAGE_CURSOR)
    {
        // `MAKEINTRESOURCE` stuffs the resource number into the pointer -
        // a Win32 custom, not ours. Take it back out.
        const uintptr_t uNumber = reinterpret_cast<uintptr_t>(c_szName);
        return M2W_CursorHandle(static_cast<int>(uNumber & 0xFFFF));
    }
    return NULL;
}

/// Win32: TRUE - nothing was created, but the clean-up "succeeded".
BOOL DestroyCursor(HCURSOR /*hCursor*/)
{
    return TRUE;    // nothing was created, but to the caller the clean-up succeeded
}

/// Win32: FALSE - a page does not change system settings (sticky keys).
BOOL SystemParametersInfoA(UINT /*uAction*/, UINT /*uParam*/, void* /*pvData*/,
                           UINT /*uWinIni*/)
{
    // The client turns off "sticky keys" for the game. A page does not
    // change system settings and should not - FALSE says nothing changed.
    return FALSE;
}

// ---------------------------------------------------------------------------
// Floating-point unit
// ---------------------------------------------------------------------------

/// MSVC runtime: no x87 control word in WebAssembly - returns 0.
unsigned int _controlfp(unsigned int /*uNewValue*/, unsigned int /*uMask*/)
{
    // On x87 this set precision and rounding. WebAssembly has IEEE-754
    // arithmetic with fixed behaviour - there is no control word to change.
    // The same function settled that `fistp` in PRTerrainLib
    // ROUNDS rather than truncates: its only call in TMP4 sets precision,
    // not rounding.
    return 0;
}

/// Win32: TRUE - MEMFS has no read-only attribute to clear.
BOOL SetFileAttributesA(LPCSTR /*c_szName*/, DWORD /*dwAttributes*/)
{
    // TMP4 clears the read-only attribute before writing. Emscripten's file
    // system has no such attribute, so there is nothing to clear and the
    // write succeeds anyway.
    return TRUE;
}

// ---------------------------------------------------------------------------
// Screen measures - JavaScript in runtime.js, bridges here
// ---------------------------------------------------------------------------

/// GUI scale (`?scale=`), snapped to a baked font scale. See m2w.uiScale.
EM_JS(double, m2w_ui_scale, (void), { return m2w.uiScale(); });

/// Device pixel ratio capped so the buffer stays within `?max=` (default
/// 1920x1080). See m2w.effectiveDpr.
EM_JS(double, m2w_effective_dpr, (void), { return m2w.effectiveDpr(); });

/// Logical pixels per CSS pixel: effective dpr / GUI scale.
EM_JS(double, m2w_css_to_buffer_ratio, (void), { return m2w.cssToBufferRatio(); });

/// `?cursor=`: 1 software, 2 hardware, 0 not given.
EM_JS(int, m2w_cursor_mode, (void), { return m2w.cursorMode(); });

/// Canvas width (0), height (1) in logical pixels, or hidden-tab flag (2).
EM_JS(int, m2w_canvas_state, (int iWhat), { return m2w.canvasState(iWhat); });

/// Window width (0) or height (1) in logical pixels.
EM_JS(int, m2w_window_size, (int bHeight), { return m2w.windowSize(bHeight); });

/// The page's reaction to the end of the game (close, return, message).
EM_JS(void, m2w_quit_page, (void), { m2w.quit(); });

/// Whether the game draws its own cursor: `?cursor=` overrides
/// `bFromConfig` (SOFTWARE_CURSOR in metin2.cfg). Called from
/// `CPythonSystem::IsSoftwareCursor` (tools/stage_port.py).
extern "C" int M2W_UseSoftwareCursor(int bFromConfig)
{
    const int iMode = m2w_cursor_mode();
    const int bSoftware = (iMode == 1) ? 1 : (iMode == 2) ? 0 : bFromConfig;
    static bool s_bPrinted = false;
    if (!s_bPrinted)
    {
        s_bPrinted = true;
        std::printf("m2w cursor: %s (%s)\n",
                    bSoftware ? "software - drawn by the game" : "hardware - CSS",
                    iMode ? "from ?cursor=" : "from metin2.cfg");
    }
    if (bSoftware) EM_ASM({ m2w.cursor.software = true; });
    return bSoftware;
}

/// GUI scale for C++ (platform_text.cpp, gl_device.cpp), read once.
extern "C" float M2W_UiScale()
{
    static float s_fScale = 0.0f;
    if (s_fScale <= 0.0f)
    {
        s_fScale = static_cast<float>(m2w_ui_scale());
        if (s_fScale < 1.0f) s_fScale = 1.0f;
    }
    return s_fScale;
}

// ---------------------------------------------------------------------------
// Message queue - added, corrected
// ---------------------------------------------------------------------------
// The browser has no message loop: input comes from DOM listeners and
// frames from requestAnimationFrame. Earlier the conclusion drawn
// from that was "an empty queue is the true answer", and it cost a black
// screen: the client does not read an empty queue as "nothing to report"
// but as "NOTHING EVER CHANGED". `WM_SIZE` is the only way it learns it is
// not minimised (`m_isMinimizedWnd` starts true in PythonApplication.cpp
// and is cleared only in the WM_SIZE handler), so without it the client
// called `SkipRenderBuffering(3000)` forever: 3000 loop iterations, ZERO
// WebGL calls, a live context, clean syserr. Perfect silence.
//
// So this layer does not fake the full Win32 queue - input still comes from
// DOM events (events_web.cpp collects them, this file turns them into
// window messages) - but it carries the messages that have NO OTHER
// CARRIER: the window state. Untouched TMP4 code does the rest:
// `MSWindowProcedure` finds the window through `GWL_USERDATA`, `OnSize`
// sets `m_isActive` and `m_isVisible`. One seam fixes four fields.
//
// The state is POLLED, not listened for: `PeekMessage` runs once per frame
// anyway (`CMSApplication::IsMessage`), so comparing the state costs the
// same as a listener and needs no exported function nor listener lifetime.
// The first iteration produces the first WM_SIZE by itself, because the
// "previous state" starts as impossible values.

bool M2W_PollWindowEvent(UINT* puMessage, WPARAM* pwParam, LPARAM* plParam);
void M2W_MousePosition(int* piX, int* piY);
void M2W_MouseSet(int iX, int iY);

namespace
{

/// Address of this variable serves as the window handle: any non-zero,
/// unique value will do - `HWND` is opaque to the program on Win32 too.
char g_cPageCanvas = 0;

/// The handle of our one window (the page's canvas).
HWND CanvasHandle()
{
    return reinterpret_cast<HWND>(&g_cPageCanvas);
}

/// Window state LAST REPORTED to the client. Deliberately impossible
/// initial values: the first poll always counts as a change.
int  g_iReportedWidth  = -1;
int  g_iReportedHeight = -1;
int  g_bReportedHidden = -1;

/// Window procedure remembered at `RegisterClass` - on Win32 the window
/// class holds it and `DispatchMessage` takes it from there; same here.
WNDPROC g_pfnWindowProc = NULL;

/// `GWL_USERDATA` of our one window: `CMSWindow::Create` stores `this`,
/// `MSWindowProcedure` reads it back. Without it a message would reach the
/// trampoline and fall into `DefWindowProc` - nowhere.
LONG g_lWindowUserData = 0;

/// The pending message, if any.
bool   g_bMessagePending = false;
UINT   g_uMessage = 0;
WPARAM g_wMessage = 0;
LPARAM g_lMessage = 0;

/// Window that captured the mouse (see `SetCapture`).
HWND g_hCapture = NULL;

/// `PostQuitMessage` raises this; `GetMessage` then returns
/// FALSE (= WM_QUIT) and `MessageProcess` ends the main loop.
bool g_bQuitRequested = false;

/// Compares the window state with the last reported one and, when it
/// differs, composes WM_SIZE. Returns true when a message is pending.
bool RefreshWindowState()
{
    if (g_bMessagePending)
        return true;

    // The C++ reference keeps the function in the link; earlier
    // d3d8_factory.cpp called it by its JS name from another EM_JS (now
    // through `m2w.windowSize`) - kept so that the export set stays.
    (void)m2w_css_to_buffer_ratio();
    const int iWidth  = m2w_canvas_state(0);
    const int iHeight = m2w_canvas_state(1);
    const int bHidden = m2w_canvas_state(2);

    // Zero means "not known yet", not "a window of zero pixels" - reporting
    // it would repeat the part-199 mistake.
    if (iWidth <= 0 || iHeight <= 0)
        return false;

    if (iWidth == g_iReportedWidth && iHeight == g_iReportedHeight && bHidden == g_bReportedHidden)
        return false;

    g_iReportedWidth  = iWidth;
    g_iReportedHeight = iHeight;
    g_bReportedHidden = bHidden;

    // A hidden tab really IS minimised - skipping frames is then correct,
    // so the truth is carried both ways, not a constant "restored". The
    // line goes to the log and the on-screen counter: `IsActive()`
    // decides whether the game does anything at all and is set ONLY from
    // this message, so "the loop runs 60 times a second and does 0 % work"
    // had to be checkable from one screenshot.
    std::printf("    WINDOW %dx%d %s\n", iWidth, iHeight,
                bHidden ? "HIDDEN (game pauses)" : "visible");

    g_uMessage = WM_SIZE;
    g_wMessage = bHidden ? SIZE_MINIMIZED : SIZE_RESTORED;
    g_lMessage = static_cast<LPARAM>((iHeight << 16) | (iWidth & 0xFFFF));
    g_bMessagePending = true;
    return true;
}

/// Whether any message is pending - window state or page input. ORDER
/// MATTERS: window state first, because WM_SIZE takes the client out of the
/// "minimised" state; a click delivered before it would hit an interface
/// that has not been drawn yet.
bool HasMessage()
{
    if (g_bQuitRequested || g_bMessagePending)
        return true;
    if (RefreshWindowState())
        return true;

    UINT   uMessage = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    if (!M2W_PollWindowEvent(&uMessage, &wParam, &lParam))
        return false;

    g_uMessage = uMessage;
    g_wMessage = wParam;
    g_lMessage = lParam;
    g_bMessagePending = true;
    return true;
}

/// Copies the pending message (set by `HasMessage`) into `pMsg`, addressed to
/// the canvas window; does nothing for NULL.
void FillMessage(LPMSG pMsg)
{
    if (!pMsg)
        return;
    std::memset(pMsg, 0, sizeof(*pMsg));
    pMsg->hwnd    = CanvasHandle();
    pMsg->message = g_uMessage;
    pMsg->wParam  = g_wMessage;
    pMsg->lParam  = g_lMessage;
}

}  // namespace

/// Win32: reports the pending message (window state first, then page
/// input); `PM_REMOVE` consumes it.
BOOL PeekMessageA(LPMSG pMsg, HWND, UINT, UINT, UINT uRemove)
{
    if (!HasMessage())
    {
        if (pMsg) std::memset(pMsg, 0, sizeof(*pMsg));
        return FALSE;
    }

    FillMessage(pMsg);

    // `PM_NOREMOVE` = look but leave. `CMSApplication::IsMessage` calls it
    // that way and `MessageProcess` then takes the message with
    // `GetMessage` - removing here would lose it before delivery.
    if (uRemove & PM_REMOVE)
        g_bMessagePending = false;

    return TRUE;
}

/// Win32: the pending message, or FALSE (= WM_QUIT) - never waits.
BOOL GetMessageA(LPMSG pMsg, HWND, UINT, UINT)
{
    // Win32 `GetMessage` WAITS. Waiting here would hang the tab (one
    // thread, one event loop), so it answers with what there is: a message
    // or FALSE. FALSE means WM_QUIT and `MessageProcess` ends the main
    // loop, so it may be returned ONLY when nobody asked `IsMessage` - and
    // that is the case: the loop calls `GetMessage` only after
    // `PeekMessage` said yes.
    if (g_bQuitRequested || !HasMessage())
    {
        if (pMsg) std::memset(pMsg, 0, sizeof(*pMsg));
        return FALSE;
    }

    FillMessage(pMsg);
    g_bMessagePending = false;
    return TRUE;
}

/// Win32: FALSE - characters come from DOM events, nothing to translate.
BOOL TranslateMessage(const MSG*)
{
    // WM_KEYDOWN -> WM_CHAR. Characters come from DOM events, so there is
    // nothing to translate; FALSE is the truth.
    return FALSE;
}

/// Win32: calls the window procedure remembered by `RegisterClassA`.
LRESULT DispatchMessageA(const MSG* pMsg)
{
    // Win32 takes the procedure from the window class; so do we, hence the
    // requirement that `RegisterClass` remembers it.
    if (!pMsg || !g_pfnWindowProc)
        return 0;
    return g_pfnWindowProc(pMsg->hwnd, pMsg->message, pMsg->wParam, pMsg->lParam);
}

/// Win32: only `GWL_USERDATA` has content; everything else is 0.
LONG GetWindowLongA(HWND, int nIndex)
{
    // The only field with content is `GWL_USERDATA` (`MSWindowProcedure`
    // reads the `CMSWindow` pointer from it). Styles and instance handles
    // belong to a window that does not exist; zero is the truth there.
    return (nIndex == GWL_USERDATA) ? g_lWindowUserData : 0;
}

/// Win32: stores `GWL_USERDATA`, returns the previous value.
LONG SetWindowLongA(HWND, int nIndex, LONG lValue)
{
    if (nIndex != GWL_USERDATA)
        return 0;
    const LONG lPrevious = g_lWindowUserData;
    g_lWindowUserData = lValue;
    return lPrevious;
}

/// Win32: the display counter - TRUE +1, FALSE -1, visible while >= 0.
int ShowCursor(BOOL bShow)
{
    // A DISPLAY COUNTER, NOT A CONSTANT. An earlier version returned zero
    // ("CSS drives the cursor, no counter needed") and the client hung on
    // the first main-loop iteration in `OnUIUpdate`, because it LOOPS on
    // the value: `do { n = ShowCursor(FALSE); } while (n >= 0);`
    // (PythonApplicationCursor.cpp, found by breaking execution and reading
    // the stack). Win32 contract, verbatim: TRUE increments,
    // FALSE decrements, the NEW value is returned, the cursor is visible
    // while the counter is non-negative; it starts at zero = visible.
    static int s_iCounter = 0;
    s_iCounter += bShow ? 1 : -1;
    M2W_CursorVisible(s_iCounter >= 0 ? 1 : 0);
    return s_iCounter;
}

HCURSOR SetCursor(HCURSOR hCursor)    { return M2W_SetCursor(hCursor); }

/// The game moves the cursor back to the anchor point while turning the
/// camera; done on the virtual position under pointer lock
/// (events_web.cpp).
BOOL SetCursorPos(int iX, int iY)     { M2W_MouseSet(iX, iY); return TRUE; }

/// Win32: the pointer position measured by events_web.cpp; canvas = screen.
BOOL GetCursorPos(POINT* pPoint)
{
    // Corrected. It used to return FALSE and leave the point
    // untouched - but mouse moves do not reach the game any other way, so
    // "keeps what it had" meant "never learns", `OnMouseMove` was
    // skipped and the UI did not know where the pointer was - and
    // `UI::CWindowManager` picks the window under the CURSOR, not under
    // the click. The browser cursor exists and can be measured; the canvas
    // is the screen (`ClientToScreen` is the identity).
    if (!pPoint)
        return FALSE;

    // Through intermediates: `POINT::x` is `LONG`, not `int` - same width,
    // different type (the `DWORD` lesson).
    int iX = 0;
    int iY = 0;
    M2W_MousePosition(&iX, &iY);
    pPoint->x = iX;
    pPoint->y = iY;
    return TRUE;
}

BOOL ClientToScreen(HWND, POINT*) { return TRUE; }  // the canvas IS the screen
BOOL ScreenToClient(HWND, POINT*) { return TRUE; }
BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return FALSE; }

// MOUSE CAPTURE stopped being decoration:
// `PythonApplicationProcedure.cpp` calls `OnMouseLeftButtonUp` ONLY when
// `hWnd == GetCapture()`, and a click in TMP4 completes on the RELEASE.
// There is nothing to capture at system level (and no need: `mouseup` is
// caught on the window, so a release outside the canvas arrives too); only
// the bookkeeping remains, and it has to be true.
/// Win32: clears the captured window (bookkeeping only).
BOOL ReleaseCapture(void)
{
    g_hCapture = NULL;
    return TRUE;
}

/// Win32: records the captured window, returns the previous one.
HWND SetCapture(HWND hWindow)
{
    const HWND hPrevious = g_hCapture;
    g_hCapture = hWindow;
    return hPrevious;
}

/// Win32: 1 = "list exhausted, font not found" - a page cannot enumerate
/// system fonts (see the body).
int EnumFontFamiliesExA(HDC, LOGFONTA*, FONTENUMPROCA, LPARAM, DWORD)
{
    // A page cannot list the system's fonts; the callback is never called.
    // Return value corrected in
    // Win32 returns the LAST value the callback returned, and a
    // callback returning zero means "stop, found it" - so zero told the
    // only caller, `eterLib/Util.cpp` (`if (EnumFontFamiliesEx(...) == 0)
    // return fontFace;`), that EVERY font in its Windows 9x table exists.
    // Non-zero means "the whole list went by, nobody stopped" = not found,
    // and only then Util.cpp falls back to `GetDefaultFontFace()`. That is
    // the true answer: we found no font, because we cannot search.
    return 1;
}

// --- device context and COM ------------------------------------------------
// `GetDC` has ONE use in the client - Util.cpp, as the first argument of the
// font enumeration above. The handle is never read, so it is non-zero: NULL
// would mean "cannot draw", which is false (platform_text.cpp draws).
HDC GetDC(HWND) { return reinterpret_cast<HDC>(1); }
int ReleaseDC(HWND, HDC) { return 1; }

// COM does not exist. `UserInterface.cpp` calls these on entry and exit and
// checks only for success - and nothing that needed COM was left undone.
HRESULT CoInitialize(void*) { return S_OK; }
void    CoUninitialize(void) {}

/// Win32: FALSE, buffer untouched - no special folders.
BOOL SHGetSpecialFolderPathA(HWND, LPSTR pszPath, int, BOOL)
{
    // No Windows special folders. FALSE, buffer untouched - the caller
    // then uses its fallback path.
    (void)pszPath;
    return FALSE;
}

/// Win32: FALSE - no display modes; the window is the canvas.
BOOL EnumDisplaySettingsA(LPCSTR, DWORD, LPDEVMODEA)
{
    return FALSE;   // no display modes to list: the game's window is the canvas
}

/// Win32: not `DISP_CHANGE_SUCCESSFUL` - the mode did not change.
LONG ChangeDisplaySettingsA(LPDEVMODEA, DWORD)
{
    // Anything but `DISP_CHANGE_SUCCESSFUL`: the mode really did not
    // change, and claiming success would make the game think it is
    // full-screen.
    return 1;
}

// Window procedure: no window, nothing to handle - zero is what Win32
// returns for messages of no consequence.
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }

/// DirectInput interface id - the real value, since it costs nothing and
/// must match if ever compared.
const GUID IID_IDirectInput8A = {
    0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 }
};

BOOL ShowWindow(HWND, int) { return FALSE; }   // the window belongs to the page
BOOL IsIconic(HWND)        { return FALSE; }   // a tab is not "minimised"; nobody asks anyway

const GUID GUID_SysKeyboard = {
    0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
};
const GUID GUID_SysMouse = {
    0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
};

// Region invalidation: a frame is drawn whole on every requestAnimationFrame,
// so there is no "region to refresh".
BOOL InvalidateRect(HWND, const RECT*, BOOL) { return TRUE; }
BOOL UpdateWindow(HWND)                      { return TRUE; }

// ---------------------------------------------------------------------------
// The window belongs to the PAGE, not to the game
// ---------------------------------------------------------------------------
// The page owns the canvas: a window that is never closed, and a page
// cannot intercept its own tab closing. Earlier `CreateWindowExA`
// returned NULL for that reason - right reason, wrong answer: NULL means
// "failed to create", and `CMSWindow::Create` aborted the whole game
// ("CMSWindow::Create failed / Metin2.CREATE_WINDOW"). So it returns a
// NOMINAL HANDLE: one constant value meaning "this page's canvas",
// comparable and non-zero. `IsWindow` is TRUE for it and FALSE for anything
// else; `DestroyWindow` is FALSE, because a page cannot close itself - the
// user closes the tab.

/// Win32: the nominal handle of the page's canvas (never NULL).
HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                     HWND, void*, HINSTANCE, void*)
{
    return CanvasHandle();
}

BOOL DestroyWindow(HWND)              { return FALSE; }

/// Win32: TRUE only for the canvas handle.
BOOL IsWindow(HWND hWindow)
{
    return (hWindow == CanvasHandle()) ? TRUE : FALSE;
}
BOOL MoveWindow(HWND, int, int, int, int, BOOL) { return FALSE; }
BOOL SetWindowTextA(HWND, LPCSTR)     { return FALSE; }
void* GetMenu(HWND)                   { return NULL; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return NULL; }
HWND GetCapture(void)                 { return g_hCapture; }

/// Win32: the canvas size in logical pixels; FALSE while unknown.
BOOL GetClientRect(HWND, RECT* pRect)
{
    // Corrected: it used to return FALSE without touching the
    // rectangle ("zeroing would shrink the drawing area to nothing"), but
    // the caller then reads UNINITIALISED memory - `RECT rcWnd;
    // GetClientRect(&rcWnd); ResizeBackBuffer(...)` in
    // PythonApplicationProcedure.cpp - invisible until WM_SIZE started
    // arriving. The client area is the canvas, and it can be measured.
    if (!pRect)
        return FALSE;

    const int iWidth  = m2w_canvas_state(0);
    const int iHeight = m2w_canvas_state(1);
    if (iWidth <= 0 || iHeight <= 0)
        return FALSE;   // not known yet - and that is what is said

    pRect->left   = 0;
    pRect->top    = 0;
    pRect->right  = iWidth;
    pRect->bottom = iHeight;
    return TRUE;
}

/// Win32: same as `GetClientRect` - no frame, no title bar.
BOOL GetWindowRect(HWND hWindow, RECT* pRect)
{
    // Window and client area are the same thing in the browser: no frame,
    // title bar or border - `GetSystemMetrics` says the same since part
    // 199 (decorations have thickness zero).
    return GetClientRect(hWindow, pRect);
}

BOOL AdjustWindowRect(RECT*, DWORD, BOOL)             { return FALSE; }
BOOL AdjustWindowRectEx(RECT*, DWORD, BOOL, DWORD)    { return FALSE; }

// ---------------------------------------------------------------------------
// DirectInput - devices that do not exist
// ---------------------------------------------------------------------------
// `eterLib/Input.cpp` asks for DirectInput; in the browser input comes from
// DOM events on the canvas (physical keys mapped from KeyboardEvent.code
// to DIK codes). `DirectInput8Create` therefore
// returns an ERROR, not an empty object: an empty object would make TMP4
// call methods on it and fall over a null pointer far from the cause.
/// DirectInput: E_FAIL - input comes from DOM events, not from a device.
HRESULT DirectInput8Create(HINSTANCE, DWORD, const GUID&, void** ppvOut, void*)
{
    if (ppvOut) *ppvOut = NULL;
    return E_FAIL;
}

/// DirectInput data-format descriptors. `Input.cpp` passes their address to
/// `SetDataFormat`, so they must exist, but nobody looks inside - the device
/// is never created. `extern` because a namespace-scope `const` has
/// internal linkage; without it the linker still reported them missing
/// (one build wasted).
extern const DIDATAFORMAT c_dfDIKeyboard = { sizeof(DIDATAFORMAT) };
extern const DIDATAFORMAT c_dfDIMouse    = { sizeof(DIDATAFORMAT) };
extern const DIDATAFORMAT c_dfDIMouse2   = { sizeof(DIDATAFORMAT) };

/// Win32: ends the main loop (`GetMessage` returns WM_QUIT) and lets the
/// page close, return or show a message (m2w.quit).
void PostQuitMessage(int)
{
    // ENDING THE LOOP. Until then this was empty: after
    // `app.Abort()` (missing file, script error) the game went on, and
    // `sys.exit()` from a script finalised the interpreter mid-frame -
    // window objects died, C++ kept dangling handles and the client crashed
    // with "memory access out of bounds". Now: flag ->
    // `GetMessage` returns WM_QUIT -> the main loop ends, and the page shows
    // a readable message instead of a black screen (m2w.quit, runtime.js).
    if (g_bQuitRequested)
        return;
    g_bQuitRequested = true;
    std::printf("m2w: PostQuitMessage - client exiting\n");
    m2w_quit_page();
}

/// Win32: remembers the window procedure; returns non-zero.
ATOM RegisterClassA(const WNDCLASSA* c_pkClass)
{
    // Corrected: there are no window classes, but a class
    // carries the ONE thing needed here, the WINDOW PROCEDURE, which
    // `DispatchMessage` takes from it. `CMSWindow::Create` passes
    // `MSWindowProcedure`, which finds the window object through
    // `GWL_USERDATA` and calls its virtual handler - untouched TMP4 code
    // from there on.
    if (c_pkClass && c_pkClass->lpfnWndProc)
        g_pfnWindowProc = c_pkClass->lpfnWndProc;

    // Non-zero = registered. Zero would be an error, and
    // `CMSWindow::RegisterWindowClass` goes on regardless - so the
    // distinction has to be honest anyway.
    return 1;
}

/// Win32: forgets the window procedure.
BOOL UnregisterClassA(LPCSTR, HINSTANCE)
{
    g_pfnWindowProc = NULL;
    return TRUE;
}

/// Win32: screen size = the tab, in logical pixels; decorations = 0.
int GetSystemMetrics(int nIndex)
{
    // SCREEN SIZE: "don't know" was the WRONG answer here. Earlier
    // this returned zero, and TMP4 does not read zero as unknown but as a
    // screen of width zero: `PythonSystem.cpp` clamps the config to it and
    // the device was created 0 x -7 (the log: "tmp4 grp: tworze urzadzenie 0 x -7", today "m2w grp: creating the device").
    // The browser screen exists and can be measured - `innerWidth` is the
    // area the page can draw in, which is what `SM_CXFULLSCREEN` asks. In
    // LOGICAL pixels (dpr / GUI scale) so the buffer matches its
    // size on the page and nothing is stretched.
    switch (nIndex)
    {
        // Whole screen and work area are the same: the tab. No task bar.
        case SM_CXSCREEN:
        case SM_CXFULLSCREEN:
            return m2w_window_size(0);

        case SM_CYSCREEN:
        case SM_CYFULLSCREEN:
            return m2w_window_size(1);

        // Window decorations - frame, title bar, border: the page has none,
        // and zero here means "thickness zero", not "unknown".
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Graphics capability flags that `GrpDevice.cpp` used to set
// ---------------------------------------------------------------------------
// That file is REPLACED and with it went the definitions of four
// flags the drawing code tests in many places. The values are intended:
// WebGL draws lines and shadows without restriction, so both "cannot" flags
// are false; images are not halved, since texture memory is not as tight as
// on 2004 cards; software tiling was a workaround for cards without
// `CLAMP_TO_EDGE`, which WebGL has as standard. If any of this proves
// different in practice, this is the place to change it.
bool GRAPHICS_CAPS_CAN_NOT_DRAW_LINE   = false;
bool GRAPHICS_CAPS_CAN_NOT_DRAW_SHADOW = false;
bool GRAPHICS_CAPS_HALF_SIZE_IMAGE     = false;
bool GRAPHICS_CAPS_SOFTWARE_TILING     = false;

// ---------------------------------------------------------------------------
// A mutex in a SINGLE-THREADED program
// ---------------------------------------------------------------------------
// Not a stub but the correct implementation for this case: with one thread
// the lock is always free and taking it always succeeds. TMP4 uses it to
// ensure a single running copy - and every tab is its own world, so that
// holds too. The handle must differ from NULL: the caller tests it.
/// Win32: a non-NULL handle - the lock is always free (one thread).
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR)
{
    return reinterpret_cast<HANDLE>(1);
}
BOOL ReleaseMutex(HANDLE) { return TRUE; }

// Multimedia timer resolution: `performance.now()` has a fixed resolution
// the page cannot change, so `timeBeginPeriod` has nothing to set and says
// so with `TIMERR_NOERROR` (nothing broke) and no effect.
UINT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
UINT timeEndPeriod(UINT)   { return TIMERR_NOERROR; }

/// winmm: 1 ms both ways - the resolution of `performance.now()`.
UINT timeGetDevCaps(LPTIMECAPS pCaps, UINT)
{
    // 1 ms both ways - the real resolution to expect from
    // `performance.now()` with timing protections on.
    if (pCaps) { pCaps->wPeriodMin = 1; pCaps->wPeriodMax = 1; }
    return TIMERR_NOERROR;
}

/// Win32: empty string - no `.rc` string resources.
int LoadStringA(HINSTANCE, UINT, LPSTR pszBuffer, int iSize)
{
    // No `.rc` string resources in the port; game texts come from the
    // package. The first character is zeroed so the caller gets an empty
    // string, not garbage.
    if (pszBuffer && iSize > 0) pszBuffer[0] = '\0';
    return 0;
}

/// Win32: returns the caller's default - no `.ini` files.
DWORD GetPrivateProfileStringA(LPCSTR, LPCSTR, LPCSTR c_szDefault,
                               LPSTR pszBuffer, DWORD dwSize, LPCSTR)
{
    // No `.ini` files; settings come from `metin2.cfg` through the client's
    // own reader. The DEFAULT the caller supplied is returned - exactly
    // what Win32 does when the file is missing.
    if (!pszBuffer || dwSize == 0) return 0;
    const char* c_szSource = c_szDefault ? c_szDefault : "";
    DWORD i = 0;
    while (c_szSource[i] && i + 1 < dwSize) { pszBuffer[i] = c_szSource[i]; ++i; }
    pszBuffer[i] = '\0';
    return i;
}

// `_fltused` is the marker by which the MSVC linker recognises floating-
// point use. Outside MSVC it means nothing, but code compiled earlier
// references it, so it must exist.
extern "C" int _fltused = 0;
