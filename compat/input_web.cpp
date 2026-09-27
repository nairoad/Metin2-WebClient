// SPDX-License-Identifier: GPL-2.0-or-later
// input_web.cpp - polled keyboard state: `GetAsyncKeyState` / `GetKeyState`
// answered from a table of virtual-key codes kept in JavaScript and filled
// from `keydown` / `keyup`.

// Design:
// The client asks about keys in TWO different ways: by events (`WM_CHAR`
// into a text field - ime_web.cpp) and by POLLING - `GetAsyncKeyState(
// VK_LSHIFT)` in the middle of drawing (git grep: Area.cpp,
// MapOutdoorRenderHTP.cpp, EffectManager.cpp, ModelInstanceRender.cpp,
// PythonApplication.cpp, PythonApplicationLogo.cpp, MovieMan.cpp). The
// poller wants the state NOW, not a notification of a change, so the
// browser's answer is a state table fed by key events.
//
// Two independent key-code families exist in the client: `VK_*` (Windows
// virtual keys, used by `GetAsyncKeyState`) and `DIK_*` (DirectInput scan
// codes, used by key bindings - keyboard_web.cpp, dinput.h). The browser
// gives neither; `KeyboardEvent.code` is a name like `"ShiftLeft"`. This
// file maps to `VK_*` only, because that is all `GetAsyncKeyState` asks for;
// mixing the two families would repeat a mistake win32_compat.h once
// recorded.
//
// The `GetAsyncKeyState` contract must be kept exactly: Win32 returns a
// `SHORT` whose HIGH bit (0x8000) means "down now" and whose low bit
// (0x0001) means "was pressed since the last query". Callers test it
// differently - Area.cpp `& 0x8001`, MapOutdoorRenderHTP.cpp `& 0x8000`,
// EffectManager.cpp the whole value as a boolean - so returning a bare `1`
// would break the `& 0x8000` checks, which happen to guard the terrain
// preview mode. Hence the full 0x8000.

#include "win32_compat.h"

#include <emscripten.h>

/// Installs the key-state table (`globalThis.m2w_keys`, 256 bytes indexed
/// by VK code) and the `keydown` / `keyup` / `blur` listeners that fill it.
/// Kept in JavaScript because that is where the events arrive; a query from
/// C++ is one boundary crossing per key per frame, and there are few pollers.
EM_JS(void, m2w_keys_start, (void), { if (globalThis.m2w) m2w.keysStart(); });

/// Whether the key with this VK code is down now (0/1).
EM_JS(int, m2w_key_pressed, (int iVk), { return globalThis.m2w ? m2w.keyPressed(iVk) : 0; });

/// Win32: 0x8000 when the key is down now, 0 otherwise (see the design
/// note for the contract and the missing low bit).
SHORT GetAsyncKeyState(int vKey)
{
    // The listeners are installed on the first query. A separate start call
    // is not worth having: the client polls keys only once the page is up.
    static bool s_bStarted = false;
    if (!s_bStarted)
    {
        m2w_keys_start();
        s_bStarted = true;
    }

    // Only the "down now" bit. The "pressed since the last query" bit is NOT
    // emulated: in Win32 reading it CLEARED it, so it depended on who asked
    // last. No caller in TMP4 relies on it - Area.cpp writes `& 0x8001`, but
    // the high bit decides anyway. Faking it would give an order-dependent
    // answer.
    return m2w_key_pressed(vKey) ? static_cast<SHORT>(0x8000) : 0;
}

/// Win32: same answer as `GetAsyncKeyState`; the toggle bit is always 0.
SHORT GetKeyState(int vKey)
{
    // In Win32 this differs from `GetAsyncKeyState` by answering from the
    // message QUEUE rather than the keyboard. There is no queue
    // (platform_none.cpp), so both answers are the same.
    //
    // Remaining limitation: the low bit of `GetKeyState` reports a TOGGLE -
    // whether the Caps Lock or Scroll Lock light is on. A table built from
    // `keydown` / `keyup` sees the key press, not the state it toggled the
    // keyboard into, so that bit stays zero ("toggle off") - and where TMP4
    // used Scroll Lock as a camera modifier (PythonApplicationCamera.cpp,
    // `GetKeyState(VK_SCROLL) & 1`) that mode is unavailable.
    return GetAsyncKeyState(vKey);
}
