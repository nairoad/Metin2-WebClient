// SPDX-License-Identifier: GPL-2.0-or-later
// keyboard_web.cpp - `CInputKeyboard` / `CInputDevice` (eterLib/Input.h) on
// browser key events: a 256-entry DirectInput scan-code table kept in
// JavaScript (m2w.dik, runtime.js) and copied into wasm once per frame.

// Design:
// `eterLib/Input.cpp` asks for DirectInput - device, data format,
// cooperative level, `GetDeviceState` every frame - none of which exists
// in the browser. `DirectInput8Create` honestly refuses
// (platform_none.cpp), but `Input.cpp` reads the refusal as a failure and
// `CPythonApplication::Create` then aborts the whole game WITHOUT a log
// line (its only silent `return false`). A file whose content is entirely
// about Windows is replaced, not patched - the same case as sound (part
// 176) and the text field.
//
// Scan codes, not virtual keys: input_web.cpp already tracks the keyboard
// in `VK_*`, because that is what `GetAsyncKeyState` asks for. This file
// needs `DIK_*`: `UpdateKeyboard` walks 256 DirectInput scan codes and the
// client describes key bindings with them. `VK_A` is 0x41, `DIK_A` is 0x1E
// - two numberings of the same keyboard; using one for the other would
// give scrambled keys, not missing ones.
//
// One boundary crossing per frame, not 256: the
// first version asked JS about every key separately, every frame - 15 360
// crossings a second at 60 FPS for data that already lies in JS as one
// `Uint8Array(256)`. DirectInput's `GetDeviceState` copied the whole
// buffer in one call, and so does `m2w_dik_copy_state` (`HEAPU8.set`, a
// block copy). This does NOT apply to `GetAsyncKeyState`, which asks about
// single keys.

#include <cstring>

#include "win32_compat.h"
// Path with directory, because the compatibility layer has no `eterLib`
// on its include path - and should not. This file is the exception: it
// implements a class FROM THE GAME TREE, so it must see its declaration.
// `dinput.h` BEFORE `Input.h`: that one uses `LPDIRECTINPUT8` and
// `LPDIRECTINPUTDEVICE8` without including them (the game's `StdAfx.h`
// did, and the compatibility layer has none).
#include "dinput.h"
#include "eterLib/Input.h"

#include <emscripten/emscripten.h>

// Static members of `CInputDevice` and `CInputKeyboard` - here because
// this file replaces `Input.cpp` entirely.
LPDIRECTINPUT8       CInputDevice::ms_lpDI = NULL;
LPDIRECTINPUTDEVICE8 CInputKeyboard::ms_lpKeyboard = NULL;
bool                 CInputKeyboard::ms_bPressedKey[256] = { false };
char                 CInputKeyboard::ms_diks[256] = { 0 };

/// Installs the key listeners and the scan-code table (m2w.dikStart).
EM_JS(void, m2w_dik_start, (void), { m2w.dikStart(); });

/// Whether the key with this scan code is down (0/1). No caller today
/// (git grep) - `UpdateKeyboard` copies the whole table instead.
EM_JS(int, m2w_dik_pressed, (int iDik), {
    var state = m2w.dik;
    if (!state || iDik < 0 || iDik > 255) return 0;
    return state[iDik] ? 1 : 0;
});

/// Copies the whole 256-byte table into `pTarget` in ONE crossing (zeros
/// when the listeners are not installed yet).
EM_JS(void, m2w_dik_copy_state, (void* pTarget), {
    var state = m2w.dik;
    if (!state) { HEAPU8.fill(0, pTarget, pTarget + 256); return; }
    HEAPU8.set(state, pTarget);
});

CInputDevice::CInputDevice()
{
}

CInputDevice::~CInputDevice()
{
}

HRESULT CInputDevice::CreateDevice(HWND)
{
    // Nothing to create - `InitializeKeyboard` installs the listeners.
    // SUCCESS, because to the caller the input device is ready. This is
    // not pretending DirectInput: DirectInput still refuses, nobody asks
    // it any more.
    return S_OK;
}

CInputKeyboard::CInputKeyboard()
{
}

CInputKeyboard::~CInputKeyboard()
{
}

bool CInputKeyboard::InitializeKeyboard(HWND)
{
    m2w_dik_start();
    ResetKeyboard();
    return true;
}

void CInputKeyboard::ResetKeyboard()
{
    // Releases every key THROUGH `KeyUp`, not by zeroing the table: `KeyUp`
    // calls `OnKeyUp`, so the game learns of the release. Zeroing alone
    // would leave it believing the key is still down.
    for (int i = 0; i < 256; ++i)
        if (ms_bPressedKey[i])
            KeyUp(i);

    std::memset(ms_diks, 0, sizeof(ms_diks));
}

void CInputKeyboard::UpdateKeyboard()
{
    // The same loop as the original - only the state comes from the
    // browser instead of `GetDeviceState`. The comparison with the previous
    // frame stays: it is what turns STATE into EVENTS (`OnKeyDown`,
    // `OnKeyUp`).
    unsigned char auState[256];
    m2w_dik_copy_state(auState);

    for (int i = 0; i < 256; ++i)
    {
        const bool bNow = auState[i] != 0;
        ms_diks[i] = bNow ? static_cast<char>(0x80) : 0;

        if (bNow)
        {
            if (!IsPressed(i))
                KeyDown(i);
        }
        else if (IsPressed(i))
        {
            KeyUp(i);
        }
    }
}

void CInputKeyboard::KeyDown(int iIndex)
{
    ms_bPressedKey[iIndex] = true;
    OnKeyDown(iIndex);
}

void CInputKeyboard::KeyUp(int iIndex)
{
    ms_bPressedKey[iIndex] = false;
    OnKeyUp(iIndex);
}

bool CInputKeyboard::IsPressed(int iIndex)
{
    return ms_bPressedKey[iIndex];
}
