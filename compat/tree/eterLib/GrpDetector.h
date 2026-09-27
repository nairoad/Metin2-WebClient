// eterLib/GrpDetector.h - OUR header in place of the TMP4 one.
//
// WHAT THE ORIGINAL DOES: enumerates graphics cards, their display modes and capabilities
// (`D3DCAPS8`), to choose a Direct3D device and weed out blacklisted
// drivers. 165 lines of questions asked of Windows.
//
// WHY WE REPLACE IT instead of porting it:
//
//  1. **In the browser there is nothing to detect.** The browser chooses the card,
//     the canvas imposes the display mode, and WebGL reports hardware capabilities through
//     its own queries - not through `D3DCAPS8`.
//  2. `D3DCAPS8` is about 90 fields describing hardware from 2001. For this
//     TMP4 header to parse, I would have to **make them all up** -
//     and that would be guessing with no way of checking, because nothing in the port
//     reads those fields.
//  3. **`GameLib` never reaches in here** - checked with grep: zero occurrences
//     of `GrpDetector`, `D3D_CAdapterInfo`, `D3D_CDisplayModeAutoDetector`.
//     The header lands in the include tree only because `GrpBase.h`
//     pulls it in on its first line.
//
// So **only the names** stay, because `GrpBase.h` holds `D3D_CDisplayModeAutoDetector`
// as a field (not a pointer), so the type must be complete. There are no bodies: if
// anyone really wanted to detect the device, the compiler or the linker would
// stop them, instead of silently returning zero.

#pragma once

#include <string>

#include "StdAfx.h"

/// Pointer to the device check. In the original called while enumerating
/// cards; kept here only for the declaration in `GrpDevice.h` to match.
typedef BOOL (*PFNCONFIRMDEVICE)(D3DCAPS8& rkD3DCaps, UINT uBehavior, D3DFORMAT eD3DFmt);

// The methods are **declared without bodies** - the same rule as for
// threads and texture loading. They appeared here because `GrpScreen.cpp`
// and `GrpDevice.cpp` really call them, not because we detect anything:
// in the browser there is nothing to detect. Signatures copied to the character from the original,
// so that the TMP4 code needs not a single change.

struct D3D_SModeInfo
{
    UINT      m_uWidth;
    UINT      m_uHeight;
    UINT      m_uDepthBits;
    D3DFORMAT m_eD3DFmtPixel;
};

class D3D_CDeviceInfo
{
public:
    VOID GetString(std::string* pstEnumList);
};

class D3D_CAdapterDisplayModeList
{
public:
    VOID Build(IDirect3D8& rkD3D, D3DFORMAT eD3DFmtDefault, UINT iAdapter);
    UINT GetDisplayModeNum();
    VOID GetString(std::string* pstEnumList);
};

class D3D_CAdapterInfo
{
public:
    BOOL Build(IDirect3D8& rkD3D, UINT iAdapter, PFNCONFIRMDEVICE pfnConfirmDevice);
    BOOL Find(UINT uScrWidth, UINT uScrHeight, UINT uScrDepthBits, BOOL isWindowed,
              UINT* piD3DModeInfo, UINT* piD3DDevInfo);
    VOID GetString(std::string* pstEnumList);

    D3DADAPTER_IDENTIFIER8& GetIdentifier();
    D3DDISPLAYMODE&         GetDesktopD3DDisplayModer();
};

class D3D_CDisplayModeAutoDetector
{
public:
    BOOL Build(IDirect3D8& rkD3D, PFNCONFIRMDEVICE pfnConfirmDevice);
    BOOL Find(UINT uScrWidth, UINT uScrHeight, UINT uScrDepthBits, BOOL isWindowed,
              UINT* piD3DModeInfo, UINT* piD3DDevInfo, UINT* piD3DAdapterInfo);
    VOID GetString(std::string* pstEnumList);

    D3D_CAdapterInfo* GetD3DAdapterInfop(UINT iD3DAdapterInfo);
};
