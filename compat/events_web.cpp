// SPDX-License-Identifier: GPL-2.0-or-later
// events_web.cpp - mouse and characters as WINDOW MESSAGES: DOM events
// queued in JavaScript (m2w.events, runtime.js) and handed to the message
// queue of platform_none.cpp one per poll, plus the virtual cursor position
// behind `GetCursorPos` / `SetCursorPos`.

// Design:
// The client reads input by TWO different roads, and mixing them would
// cost as much as mixing `VK_*` with `DIK_*`: POLLING (`GetAsyncKeyState`
// in input_web.cpp, DirectInput key state in keyboard_web.cpp - walking and
// shortcuts) and WINDOW MESSAGES (`WM_LBUTTONDOWN`, `WM_CHAR`... - the
// INTERFACE: PythonApplicationProcedure.cpp turns them into
// `OnMouseLeftButtonDown` and `UI::CWindowManager`). This file serves only
// the second road, carried by the message queue, the one that
// carries WM_SIZE.
//
// Why there was no mouse for most of the port: `DirectInput8Create`
// honestly refuses, so `CInputMouse` never exists - as it should. But the
// interface never depended on DirectInput; it depended on window messages
// that the compatibility layer had nothing to send them with. Picture, no
// clicks - and not only buttons: `Process()` calls `GetCursorPos` every
// frame and feeds `OnMouseMove`, and with FALSE the UI never learned where
// the pointer was. Two traps fixed with it: `GetCapture` must return the
// window or `WM_LBUTTONUP` is ignored (a TMP4 click completes on release),
// and coordinates are CLIENT pixels of the canvas, converted from CSS
// pixels by the canvas scale and the GUI scale.
//
// Camera turning uses Pointer Lock and a VIRTUAL position:
// the story and the Firefox quirks are documented at the listeners in
// runtime.js. The lock follows the GAME's camera drag, which it reports
// through `M2W_CameraDrag` (patches on `CCamera::BeginDrag` and
// `EndDrag`, see tools/stage_port.py) - not every right press is a camera turn.

#include <cstring>

#include "win32_compat.h"

#include <emscripten/emscripten.h>

/// Installs the DOM listeners (m2w.eventsStart). Safe to repeat.
EM_JS(void, m2w_events_start, (void), { m2w.eventsStart(); });

/// Takes one event off the queue into `pData` as four ints: message,
/// `wParam`, x, y. Returns 0 when the queue is empty.
EM_JS(int, m2w_event_poll, (int* pData), { return m2w.eventPollInto(pData); });

/// Virtual cursor position: x (0) or y (1), in logical canvas pixels.
EM_JS(int, m2w_mouse_position, (int bY), {
    var state = m2w.events;
    if (!state) return 0;
    return bY ? state.y : state.x;
});

/// `SetCursorPos`: moves the virtual position - under pointer lock exactly
/// what the game calls it for; without a lock it holds until the next real
/// mouse move. With the right button down movement is always relative
/// (runtime.js), so moving back is simply setting the position.
EM_JS(int, m2w_mouse_set, (int x, int y), { return m2w.mouseSet(x, y); });

// ---------------------------------------------------------------------------
// C side
// ---------------------------------------------------------------------------

/// Starts the listeners. Called from every entry point below, so nobody
/// has to decide "who starts this and when": repeating is safe, and when
/// the canvas does not exist yet nothing happens until the next frame.
void M2W_EventsStart()
{
    m2w_events_start();
}

/// Next queued event as a window message, or false when there is none.
bool M2W_PollWindowEvent(UINT* puMessage, WPARAM* pwParam, LPARAM* plParam)
{
    M2W_EventsStart();

    int aiData[4] = { 0, 0, 0, 0 };
    if (!m2w_event_poll(aiData))
        return false;

    *puMessage = static_cast<UINT>(aiData[0]);
    *pwParam = static_cast<WPARAM>(aiData[1]);

    // Coordinates travel in `lParam` as two 16-bit words - `LOWORD` x,
    // `HIWORD` y. The client reads them through `short(...)`, so negative
    // ones (cursor outside the canvas under capture) pass correctly.
    *plParam = static_cast<LPARAM>(((aiData[3] & 0xFFFF) << 16) |
                                    (aiData[2] & 0xFFFF));
    return true;
}

/// The game started (1) or ended (0) a camera drag (m2w.cameraDrag).
EM_JS(void, m2w_camera_drag, (int bOn), {
    if (m2w.cameraDrag) m2w.cameraDrag(bOn);
});

/// Called by the patched `CCamera::BeginDrag` (1) and `CCamera::EndDrag` (0).
extern "C" void M2W_CameraDrag(int bOn)
{
    M2W_EventsStart();
    m2w_camera_drag(bOn);
}

/// `SetCursorPos` (platform_none.cpp).
void M2W_MouseSet(int iX, int iY)
{
    M2W_EventsStart();
    m2w_mouse_set(iX, iY);
}

/// `GetCursorPos` (platform_none.cpp).
void M2W_MousePosition(int* piX, int* piY)
{
    M2W_EventsStart();
    if (piX) *piX = m2w_mouse_position(0);
    if (piY) *piY = m2w_mouse_position(1);
}
