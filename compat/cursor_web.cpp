// SPDX-License-Identifier: GPL-2.0-or-later
// cursor_web.cpp - mouse cursors as a PROPERTY OF THE CANVAS, not a file
// resource: `LoadImage(IMAGE_CURSOR)` handles that mean something,
// `SetCursor` -> `canvas.style.cursor`, real shapes from the game's `.cur`
// files converted to PNG, with a CSS keyword as fallback.

// Design:
// On Windows cursors were resources in the executable; `.rc` resources do
// not exist in the port. But cursors DO exist - the browser has them built
// in - so the difference is where they come from. Earlier
// `LoadImageA` returned NULL, the client read "failed" and aborted the
// start ("CMSWindow::Cursors Create Error"): `CreateCursors` loads fifteen
// and stops at the first NULL.
//
// A handle has to MEAN something: the client keeps them in
// `m_CursorHandleMap` and passes them back to `SetCursor` when the shape
// changes over an item, a door, while turning the camera. So the handle is
// the resource number plus a constant (only so that none is zero; numbers
// start at 102, a detail of `resource.h` not relied upon).
//
// Shapes: `tools/cursors.py` converts the `.cur` files from the
// TMP4 resources into `build/port/data/cursors/<number>.png` plus `index.txt`
// with the
// hotspots. The first time a number is used the PNG is read from the file
// system, base64-encoded and turned into `url(data:image/png;base64,...)
// x y, <fallback>`; with an old data package (no PNG) the CSS keyword
// chosen by MEANING stays (a sword becomes `crosshair`, "cannot" becomes
// `not-allowed`).
//
// Visibility and the software cursor live in m2w.cursor (runtime.js); the
// camera-turn fallback (cursor stays visible when the pointer lock is
// refused) is explained there.

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "win32_compat.h"

#include <emscripten/emscripten.h>

namespace
{

/// Offset added to the resource number so no handle is zero.
const uintptr_t c_uHandleOffset = 0x4355;   // 'CU'

/// Numbers from `UserInterface/resource.h`, repeated because the
/// compatibility layer should not pull in game headers.
const int c_iNormal       = 102;
const int c_iAttack       = 104;
const int c_iChair        = 105;
const int c_iDoor         = 106;
const int c_iNo           = 107;
const int c_iPickUp       = 108;
const int c_iTalk         = 109;
const int c_iBuy          = 110;
const int c_iSell         = 111;
const int c_iPick         = 114;
const int c_iCameraRotate = 139;
const int c_iSizeHoriz    = 140;
const int c_iSizeVert     = 141;
const int c_iSizeBoth     = 142;

/// CSS cursor keyword for a resource number, chosen by MEANING, not look.
const char* CssKeyword(int iResource)
{
    switch (iResource)
    {
        case c_iNormal:         return "default";
        // Combat: the crosshair is the nearest "aim at target".
        case c_iAttack:         return "crosshair";
        // Things that can be used - the browser has one shape for that.
        case c_iChair:
        case c_iDoor:
        case c_iTalk:
        case c_iBuy:
        case c_iSell:           return "pointer";
        // Picking an item up from the ground.
        case c_iPickUp:
        case c_iPick:           return "grab";
        // "Cannot" - the only literal match in the whole table.
        case c_iNo:             return "not-allowed";
        // Turning the camera: dragging the world.
        case c_iCameraRotate:   return "move";
        case c_iSizeHoriz:      return "ew-resize";
        case c_iSizeVert:       return "ns-resize";
        case c_iSizeBoth:       return "nwse-resize";
        default:                return "default";
    }
}

/// Standard base64 with padding.
std::string Base64(const std::vector<unsigned char>& v)
{
    static const char* c_szAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    size_t i = 0;
    for (; i + 2 < v.size(); i += 3)
    {
        const unsigned n = (v[i] << 16) | (v[i + 1] << 8) | v[i + 2];
        s += c_szAlphabet[(n >> 18) & 63]; s += c_szAlphabet[(n >> 12) & 63];
        s += c_szAlphabet[(n >> 6) & 63];  s += c_szAlphabet[n & 63];
    }
    if (i < v.size())
    {
        unsigned n = v[i] << 16;
        if (i + 1 < v.size()) n |= v[i + 1] << 8;
        s += c_szAlphabet[(n >> 18) & 63]; s += c_szAlphabet[(n >> 12) & 63];
        s += (i + 1 < v.size()) ? c_szAlphabet[(n >> 6) & 63] : '=';
        s += '=';
    }
    return s;
}

/// Hotspots from `/cursors/index.txt` (`<number> <x> <y>` per line), read once.
std::map<int, std::pair<int, int> >& Hotspots()
{
    static std::map<int, std::pair<int, int> > s_kHotspots;
    static bool s_bRead = false;
    if (!s_bRead)
    {
        s_bRead = true;
        if (FILE* pf = std::fopen("/cursors/index.txt", "r"))
        {
            int n, x, y;
            while (std::fscanf(pf, "%d %d %d", &n, &x, &y) == 3)
                s_kHotspots[n] = std::make_pair(x, y);
            std::fclose(pf);
        }
    }
    return s_kHotspots;
}

/// Full CSS `cursor` value for a resource number (cached): the PNG as a
/// data URL with its hotspot, then the keyword as fallback - or the keyword
/// alone when the package has no PNG.
const std::string& CssValue(int iResource)
{
    static std::map<int, std::string> s_kCache;
    std::map<int, std::string>::iterator it = s_kCache.find(iResource);
    if (it != s_kCache.end())
        return it->second;

    std::string kValue = CssKeyword(iResource);
    char szPath[64];
    std::snprintf(szPath, sizeof(szPath), "/cursors/%d.png", iResource);
    if (FILE* pf = std::fopen(szPath, "rb"))
    {
        std::vector<unsigned char> v;
        unsigned char ab[4096];
        size_t n;
        while ((n = std::fread(ab, 1, sizeof(ab), pf)) > 0)
            v.insert(v.end(), ab, ab + n);
        std::fclose(pf);
        if (!v.empty())
        {
            std::pair<int, int> kHot(0, 0);
            std::map<int, std::pair<int, int> >::iterator ih = Hotspots().find(iResource);
            if (ih != Hotspots().end())
                kHot = ih->second;
            char szHot[32];
            std::snprintf(szHot, sizeof(szHot), ") %d %d, ", kHot.first, kHot.second);
            kValue = "url(data:image/png;base64," + Base64(v) + szHot + CssKeyword(iResource);
        }
    }
    return s_kCache[iResource] = kValue;
}

}  // namespace

/// Sets the cursor shape on the canvas (m2w.cursorSet). The value is
/// remembered so that showing the cursor again returns to it.
EM_JS(void, m2w_cursor_set, (const char* c_szCss), {
    m2w.cursorSet(UTF8ToString(c_szCss));
});

/// Shows (1) or hides (0) the browser cursor (m2w.cursorVisible).
EM_JS(void, m2w_cursor_visible, (int bVisible), {
    m2w.cursorVisible(bVisible);
});

/// `ShowCursor` (platform_none.cpp) - a thin wrapper, because EM_JS cannot
/// be declared in a header.
void M2W_CursorVisible(int bVisible)
{
    m2w_cursor_visible(bVisible);
}

/// `LoadImage(IMAGE_CURSOR)`: the handle for a resource number.
void* M2W_CursorHandle(int iResource)
{
    return reinterpret_cast<void*>(c_uHandleOffset + static_cast<uintptr_t>(iResource));
}

/// `SetCursor`: applies the shape behind the handle; NULL hides the cursor
/// as on Win32. Returns the previous handle.
HCURSOR M2W_SetCursor(HCURSOR hCursor)
{
    static HCURSOR s_hPrevious = NULL;

    const uintptr_t uValue = reinterpret_cast<uintptr_t>(hCursor);
    if (uValue > c_uHandleOffset)
    {
        const int iResource = static_cast<int>(uValue - c_uHandleOffset);
        m2w_cursor_set(CssValue(iResource).c_str());
    }
    else if (hCursor == NULL)
    {
        m2w_cursor_visible(0);
    }

    const HCURSOR hOld = s_hPrevious;
    s_hPrevious = hCursor;
    return hOld;
}
