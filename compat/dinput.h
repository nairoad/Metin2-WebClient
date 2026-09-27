// SPDX-License-Identifier: GPL-2.0-or-later
// dinput.h - DirectInput key codes.
//
// THE HISTORY OF THIS FILE is a lesson in itself. At first I made it
// **empty** and wrote that this was "from measurement, not an unfinished
// job" - because `GameLib` does not use DirectInput even once. That was true
// about `GameLib`.
//
// Then it turned out that `UserInterface` uses **118 different
// `DIK_*` codes**, in 236 places, and that 20 files do not compile without
// them. The measurement was correct, but **the conclusion too broad**:
// "GameLib does not use it" does not mean "the client does not use it".
// The same mistake as with `GrpDetector.h` - a substitute
// written for one client.
//
// ===========================================================================
// WHY THE VALUES MATTER
// ===========================================================================
// `DIK_*` are **keyboard scan codes** (PS/2 set 1), not arbitrary numbers.
// The client saves key bindings to the settings file in exactly these
// numbers, so they are **imposed from outside** (category A): changing the
// values would remap players' keys.
//
// The browser gives `KeyboardEvent.code` - names like `"KeyW"`,
// `"ArrowUp"` - not scan codes. Translating one into the other is the job of
// the port's input layer (`CanvasInput`), and that is the right place for
// that table. Here are the numbers alone, matching DirectInput.

#pragma once

#include "win32_compat.h"

#define DIRECTINPUT_VERSION 0x0800

// --- the digit row ---------------------------------------------------------
#define DIK_ESCAPE          0x01
#define DIK_1               0x02
#define DIK_2               0x03
#define DIK_3               0x04
#define DIK_4               0x05
#define DIK_5               0x06
#define DIK_6               0x07
#define DIK_7               0x08
#define DIK_8               0x09
#define DIK_9               0x0A
#define DIK_0               0x0B
#define DIK_MINUS           0x0C
#define DIK_EQUALS          0x0D
#define DIK_BACK            0x0E
#define DIK_TAB             0x0F

// --- letters, in keyboard order, not alphabetical ------------------------
#define DIK_Q               0x10
#define DIK_W               0x11
#define DIK_E               0x12
#define DIK_R               0x13
#define DIK_T               0x14
#define DIK_Y               0x15
#define DIK_U               0x16
#define DIK_I               0x17
#define DIK_O               0x18
#define DIK_P               0x19
#define DIK_LBRACKET        0x1A
#define DIK_RBRACKET        0x1B
#define DIK_RETURN          0x1C
#define DIK_LCONTROL        0x1D
#define DIK_A               0x1E
#define DIK_S               0x1F
#define DIK_D               0x20
#define DIK_F               0x21
#define DIK_G               0x22
#define DIK_H               0x23
#define DIK_J               0x24
#define DIK_K               0x25
#define DIK_L               0x26
#define DIK_SEMICOLON       0x27
#define DIK_APOSTROPHE      0x28
#define DIK_GRAVE           0x29
#define DIK_LSHIFT          0x2A
#define DIK_BACKSLASH       0x2B
#define DIK_Z               0x2C
#define DIK_X               0x2D
#define DIK_C               0x2E
#define DIK_V               0x2F
#define DIK_B               0x30
#define DIK_N               0x31
#define DIK_M               0x32
#define DIK_COMMA           0x33
#define DIK_PERIOD          0x34
#define DIK_SLASH           0x35
#define DIK_RSHIFT          0x36
#define DIK_MULTIPLY        0x37
#define DIK_LMENU           0x38
#define DIK_SPACE           0x39
#define DIK_CAPITAL         0x3A

// --- function keys ---------------------------------------------------------
#define DIK_F1              0x3B
#define DIK_F2              0x3C
#define DIK_F3              0x3D
#define DIK_F4              0x3E
#define DIK_F5              0x3F
#define DIK_F6              0x40
#define DIK_F7              0x41
#define DIK_F8              0x42
#define DIK_F9              0x43
#define DIK_F10             0x44
#define DIK_NUMLOCK         0x45
#define DIK_SCROLL          0x46

// --- the numeric keypad ----------------------------------------------------
#define DIK_NUMPAD7         0x47
#define DIK_NUMPAD8         0x48
#define DIK_NUMPAD9         0x49
#define DIK_SUBTRACT        0x4A
#define DIK_NUMPAD4         0x4B
#define DIK_NUMPAD5         0x4C
#define DIK_NUMPAD6         0x4D
#define DIK_ADD             0x4E
#define DIK_NUMPAD1         0x4F
#define DIK_NUMPAD2         0x50
#define DIK_NUMPAD3         0x51
#define DIK_NUMPAD0         0x52
#define DIK_DECIMAL         0x53
#define DIK_F11             0x57
#define DIK_F12             0x58

// --- extended (the 0xE0 prefix in hardware) --------------------------------
#define DIK_NUMPADEQUALS    0x8D
#define DIK_PLAYPAUSE       0x99
#define DIK_NUMPADCOMMA     0xB3
#define DIK_MEDIASTOP       0xA4
#define DIK_MUTE            0xA0
#define DIK_CALCULATOR      0xA1
#define DIK_NEXTTRACK       0x99
#define DIK_VOLUMEDOWN      0xAE
#define DIK_VOLUMEUP        0xB0
#define DIK_WEBHOME         0xB2
#define DIK_NUMPADENTER     0x9C
#define DIK_RCONTROL        0x9D
#define DIK_DIVIDE          0xB5
#define DIK_SYSRQ           0xB7
#define DIK_RMENU           0xB8
#define DIK_PAUSE           0xC5
#define DIK_HOME            0xC7
#define DIK_UP              0xC8
#define DIK_PRIOR           0xC9
#define DIK_LEFT            0xCB
#define DIK_RIGHT           0xCD
#define DIK_END             0xCF
#define DIK_DOWN            0xD0
#define DIK_NEXT            0xD1
#define DIK_INSERT          0xD2
#define DIK_DELETE          0xD3
#define DIK_LWIN            0xDB
#define DIK_RWIN            0xDC
#define DIK_APPS            0xDD

// --- alternative names TMP4 uses interchangeably --------------------------
#define DIK_ESC             DIK_ESCAPE
#define DIK_LALT            DIK_LMENU
#define DIK_RALT            DIK_RMENU
#define DIK_PGUP            DIK_PRIOR
#define DIK_PGDN            DIK_NEXT

// ===========================================================================
// Devices - DECLARATIONS WITHOUT BODIES
// ===========================================================================
// Capturing the keyboard and mouse through DirectInput has no counterpart in
// the browser: events come from the `canvas`, and "exclusivity" is handled
// by the pointer lock (`requestPointerLock`). This is a DECISION, not a gap,
// so the interfaces are declared and have no bodies. The factory
// `DirectInput8Create` does have one (`platform_none.cpp`): it refuses.

#define DISCL_EXCLUSIVE     0x00000001
#define DISCL_NONEXCLUSIVE  0x00000002
#define DISCL_FOREGROUND    0x00000004
#define DISCL_BACKGROUND    0x00000008

#define DIERR_NOTACQUIRED       ((HRESULT)0x8007000CL)
#define DIERR_OTHERAPPHASPRIO   ((HRESULT)0x80070005L)
#define DIERR_INPUTLOST         ((HRESULT)0x8007001EL)

/// The device data format description. `Input.cpp` passes its address to
/// `SetDataFormat`, but nobody looks inside - the device is never created
/// anyway.
struct DIDATAFORMAT { DWORD dwSize; };
extern const DIDATAFORMAT c_dfDIKeyboard;
extern const DIDATAFORMAT c_dfDIMouse;
extern const DIDATAFORMAT c_dfDIMouse2;

/// A DirectInput device - interface only, no implementation (see above).
struct IDirectInputDevice8A
{
    /// DirectInput contract: adds a reference. Never called in the port - no
    /// device is ever created.
    virtual ULONG   AddRef() = 0;
    /// DirectInput contract: drops a reference (never called, as above).
    virtual ULONG   Release() = 0;
    /// DirectInput contract: the report format (keyboard, mouse) of the device.
    virtual HRESULT SetDataFormat(const DIDATAFORMAT* lpdf) = 0;
    /// DirectInput contract: exclusive/shared and foreground/background access
    /// (`DISCL_*`).
    virtual HRESULT SetCooperativeLevel(HWND hwnd, DWORD dwFlags) = 0;
    /// DirectInput contract: starts receiving input.
    virtual HRESULT Acquire() = 0;
    /// DirectInput contract: stops receiving input.
    virtual HRESULT Unacquire() = 0;
    /// DirectInput contract: copies the current state (`cbData` bytes) to
    /// `lpvData`.
    virtual HRESULT GetDeviceState(DWORD cbData, void* lpvData) = 0;
protected:
    /// Protected: goes away through `Release`.
    ~IDirectInputDevice8A() {}
};

/// The DirectInput object - interface only, no implementation.
struct IDirectInput8A
{
    /// DirectInput contract: adds a reference (never called - the object is
    /// never created, `DirectInput8Create` fails).
    virtual ULONG   AddRef() = 0;
    /// DirectInput contract: drops a reference (never called, as above).
    virtual ULONG   Release() = 0;
    /// DirectInput contract: creates the device `rguid` (keyboard or mouse).
    virtual HRESULT CreateDevice(const GUID& rguid, IDirectInputDevice8A** lplpDevice,
                                 void* pUnkOuter) = 0;
protected:
    /// Protected: goes away through `Release`.
    ~IDirectInput8A() {}
};

typedef IDirectInput8A*       LPDIRECTINPUT8;
typedef IDirectInputDevice8A* LPDIRECTINPUTDEVICE8;
typedef IDirectInput8A        IDirectInput8;
typedef IDirectInputDevice8A  IDirectInputDevice8;

/// Body in `platform_none.cpp`: sets `*ppvOut` to NULL and returns
/// `E_FAIL` - input comes from DOM events, and an error (not an empty
/// object) keeps TMP4 from calling methods on nothing.
HRESULT DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, const GUID& riidltf,
                           void** ppvOut, void* punkOuter);
