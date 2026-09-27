// SPDX-License-Identifier: GPL-2.0-or-later
// grpdetector_gl.cpp - what is left of graphics-adapter enumeration: the
// two `GrpDetector` methods the client still calls, and the two texture-size
// queries from `eterLib/Util.h`, all answered from the one device WebGL gives.

// Design:
// `eterLib/GrpDetector.cpp` enumerated adapters, their display modes and
// capabilities, then picked the best combination. It is replaced by our own
// `tree/eterLib/GrpDetector.h` (build_gamelib.py), because in the browser
// there is NOTHING to enumerate: one adapter - whatever the browser provides
// - and no display mode to choose. Of the whole family only two methods are
// referenced by code that is still built (`GrpScreen.cpp`, git grep); the
// rest was used only by `GrpDevice.cpp`, which is replaced too.
//
// The texture-size functions changed their question from "what does the
// adapter do according to Direct3D" to "according to OpenGL" - the same
// answer, asked differently, and asked ONCE: the numbers come from the
// device's `D3DCAPS8`, which our device fills from `GL_MAX_TEXTURE_SIZE`,
// not from a second guess here.

#include "win32_compat.h"

#include "eterLib/GrpDevice.h"

namespace
{

/// `ms_d3dCaps` and `ms_d3dPresentParameter` are PROTECTED in
/// `CGraphicBase`. A free function cannot see them; a derived class can, and
/// in C++ that is the proper road, not a trick. TMP4 did it differently:
/// `GrpDevice.cpp` copied the sizes into file statics (`s_MaxTextureWidth`)
/// when the device was created. Inheritance has no SECOND source of truth
/// about the same number - so the two cannot drift apart.
class CCapsAccess : public CGraphicDevice
{
public:
    /// The capabilities `CGraphicDevice` stored at creation (protected there;
    /// this class exists only to read them).
    static const D3DCAPS8& Caps() { return ms_d3dCaps; }
    /// The presentation parameters `CGraphicDevice` stored at creation.
    static const D3DPRESENT_PARAMETERS& PresentParameters()
    {
        return ms_d3dPresentParameter;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Largest texture
// ---------------------------------------------------------------------------

/// Largest texture width the device supports. `GrpFontTexture.cpp` asks when
/// building the glyph atlas: above 512 pixels it takes a larger atlas and
/// rebuilds it less often.
DWORD GetMaxTextureWidth()
{
    return CCapsAccess::Caps().MaxTextureWidth;
}

/// Largest texture height the device supports.
DWORD GetMaxTextureHeight()
{
    return CCapsAccess::Caps().MaxTextureHeight;
}

// ---------------------------------------------------------------------------
// The two methods the client still calls
// ---------------------------------------------------------------------------

/// Desktop display mode. Called from `CScreen::RestoreDevice`
/// (`GrpScreen.cpp`) on the device-lost path, which reads only `Format`.
///
/// In the browser the "desktop" is the page's window, whose size is what the
/// client itself passed when creating the device - so this returns the
/// current back-buffer size and the format we really draw in, not an
/// invented value.
D3DDISPLAYMODE& D3D_CAdapterInfo::GetDesktopD3DDisplayModer()
{
    static D3DDISPLAYMODE s_kMode;
    s_kMode.Width = CCapsAccess::PresentParameters().BackBufferWidth;
    s_kMode.Height = CCapsAccess::PresentParameters().BackBufferHeight;
    s_kMode.RefreshRate = 0;   // "unknown" - the browser does not say
    s_kMode.Format = D3DFMT_A8R8G8B8;
    return s_kMode;
}

/// Adapter description by index. There is one adapter, so every index means
/// the same one - and returning NULL for indices other than zero would break
/// code that simply takes the first available.
D3D_CAdapterInfo* D3D_CDisplayModeAutoDetector::GetD3DAdapterInfop(UINT)
{
    static D3D_CAdapterInfo s_kAdapter;
    return &s_kAdapter;
}
