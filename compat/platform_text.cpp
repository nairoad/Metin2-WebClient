// SPDX-License-Identifier: GPL-2.0-or-later
// platform_text.cpp - the GDI text functions `EterLib/GrpFontTexture.cpp`
// draws its glyph atlas with (CreateCompatibleDC, CreateDIBSection,
// CreateFontIndirect, SelectObject, GetCharABCWidthsFloatW, TextOutW...):
// baked GDI glyph bitmaps as the main road, a 2D canvas (runtime.js,
// m2w.text*) as the fallback, both ending in the DIB pixels TMP4 reads.

// Design:
// What the caller does: memory DC, 32-bit DIB with direct pixel access,
// font from a `LOGFONT`, ABC widths per character, `TextOutW` per glyph -
// and then it READS THE PIXELS under the pointer `CreateDIBSection`
// returned and uploads them as a texture. Whatever stands in for GDI must
// leave the pixels under that pointer.
//
// Baked GDI fonts are the main road: the canvas
// draws anti-aliased letters larger than GDI (canvas "12px" is the em
// height, GDI `lfHeight=12` the whole cell), and the engine binarises every
// touched pixel - a blob out of a smoothed letter. Real GDI uses the
// READY BITMAPS from the Tahoma file at these sizes, hence the crisp
// pixel look of the original; measured on the same string on the user's
// machine, the difference is qualitative, not a threshold. So
// tools/bake_fonts.py performs on Windows EXACTLY the GDI calls the
// client makes (same LOGFONT, ABC, TextOutW into a DIB) and stores glyphs
// as 1-bit bitmaps with metrics in `data/fonts/<face>_<h>_<i|n>_<b|n>.bin`;
// here `TextOutW`, `GetCharABCWidthsFloatW` and `GetTextExtentPoint32W`
// return those numbers and paste those bits - bit for bit what the Windows
// client sees. With the GUI scale glyphs are baked
// at `lfHeight * scale` and drawn into a PHYSICAL shadow of the DIB that
// gl_device.cpp swaps into the texture (`M2W_FontAtlasHiRes`).
//
// The canvas fallback (no font file, or a character outside the bake,
// e.g. Korean in a file name) draws for real, only with another font
// engine: `measureText` measures, `fillText` draws, `getImageData` gives
// the pixels. It does not reproduce sub-pixel placement and hinting, nor
// GDI's exact `ABC` triple (derived from the bounding box - an
// approximation noted at the function), nor font enumeration: the face
// name goes to CSS and the browser decides. The scars of the fallback (atlas
// cells too low for descenders, neighbours overwritten, anti-aliasing
// halo, kerning) are documented at m2w.textReadRows and friends in
// runtime.js.

#include "win32_compat.h"
#include "stubs.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace
{

// ---------------------------------------------------------------------------
// The GDI model TMP4 expects
// ---------------------------------------------------------------------------

/// One glyph of a baked GDI font (tools/bake_fonts.py).
struct TGlyph
{
    float A = 0, B = 0, C = 0;    ///< ABCFLOAT from GetCharABCWidthsFloatW
    int   iAdvance = 0;           ///< GetCharWidth32W (the whole advance)
    int   iInkX = 0;              ///< first ink column relative to the origin
    int   iWidth = 0, iHeight = 0;
    size_t uBits = 0;             ///< first row in TBakedFont::vecBits
};

/// A baked font: line height plus glyphs by character code.
struct TBakedFont
{
    int iLineHeight = 0;
    std::unordered_map<unsigned, TGlyph> kGlyphs;
    std::vector<unsigned char> vecBits;
};

std::unordered_map<std::string, TBakedFont*> g_kBakedFonts;   ///< key -> font (NULL = no file)

/// Reads `/fonts/<key>.bin` from the start-up package. A missing file is
/// NULL, remembered so the question is not asked twice.
TBakedFont* LoadBakedFont(const std::string& c_rkKey)
{
    auto it = g_kBakedFonts.find(c_rkKey);
    if (it != g_kBakedFonts.end()) return it->second;
    TBakedFont* pFont = nullptr;
    const std::string kPath = "/fonts/" + c_rkKey + ".bin";
    if (std::FILE* f = std::fopen(kPath.c_str(), "rb"))
    {
        std::vector<unsigned char> d;
        unsigned char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
        std::fclose(f);
        if (d.size() >= 14 && std::memcmp(&d[0], "TMP4FNT1", 8) == 0)
        {
            pFont = new TBakedFont;
            unsigned short cy; unsigned uCount;
            std::memcpy(&cy, &d[8], 2); std::memcpy(&uCount, &d[10], 4);
            pFont->iLineHeight = cy;
            size_t o = 14;
            for (unsigned i = 0; i < uCount && o + 24 <= d.size(); ++i)
            {
                unsigned uCode; float A, B, C; short sAdvance, sInkX; unsigned short usWidth, usHeight;
                std::memcpy(&uCode, &d[o], 4); std::memcpy(&A, &d[o + 4], 4);
                std::memcpy(&B, &d[o + 8], 4); std::memcpy(&C, &d[o + 12], 4);
                std::memcpy(&sAdvance, &d[o + 16], 2); std::memcpy(&sInkX, &d[o + 18], 2);
                std::memcpy(&usWidth, &d[o + 20], 2); std::memcpy(&usHeight, &d[o + 22], 2);
                o += 24;
                const size_t uBytes = static_cast<size_t>((usWidth + 7) / 8) * usHeight;
                if (o + uBytes > d.size()) break;
                TGlyph g; g.A = A; g.B = B; g.C = C; g.iAdvance = sAdvance; g.iInkX = sInkX;
                g.iWidth = usWidth; g.iHeight = usHeight;
                g.uBits = pFont->vecBits.size();
                pFont->vecBits.insert(pFont->vecBits.end(), d.begin() + o, d.begin() + o + uBytes);
                pFont->kGlyphs[uCode] = g;
                o += uBytes;
            }
            std::printf("m2w fonts: %s - line %d px, %u glyphs\n", c_rkKey.c_str(), pFont->iLineHeight,
                        (unsigned)pFont->kGlyphs.size());
        }
    }
    else
    {
        std::printf("m2w fonts: no %s - this font goes through the canvas\n", kPath.c_str());
    }
    g_kBakedFonts[c_rkKey] = pFont;
    return pFont;
}

struct TFont
{
    std::string kCss;                 ///< ready value for the canvas `font` property
    int iHeightPx = 12;
    TBakedFont* pBaked = nullptr;     ///< GDI baked offline or NULL
    float fScale = 1.0f;              ///< the bake is at `lfHeight * fScale`
};

struct TBitmap
{
    int    iWidth = 0;
    int    iHeight = 0;
    void*  pvPixels = nullptr;        ///< the buffer TMP4 received
    size_t uBytes = 0;
    /// SHADOW IN PHYSICAL PIXELS (GUI scale): the same picture as
    /// `pvPixels`, but glyphs baked at `lfHeight * scale`, placed at
    /// (x * scale, y * scale). Format as the engine's texture: 0xFFFF / 0.
    std::vector<unsigned short> vecShadow;
    int iShadowWidth = 0, iShadowHeight = 0;
};

struct TContext
{
    int      iCanvas = -1;            ///< canvas index on the JS side
    HFONT    hFont = nullptr;
    HBITMAP  hBitmap = nullptr;
    COLORREF dwTextColour = 0x00FFFFFF;   // BGR: white
    COLORREF dwBackColour = 0x00000000;
    int      iBackMode = TRANSPARENT;
};

std::unordered_map<HDC, TContext>     g_kContexts;
std::unordered_map<HFONT, TFont>      g_kFonts;
std::unordered_map<HBITMAP, TBitmap>  g_kBitmaps;
uintptr_t g_uNextHandle = 1;

/// A fresh fake GDI handle of type `T` (a counter from 1, never reused) - the
/// handles only have to be distinct and non-NULL.
template <typename T>
T NewHandle()
{
    return reinterpret_cast<T>(static_cast<uintptr_t>(g_uNextHandle++));
}

/// The character a glyph is looked up by: backspace draws as a space.
unsigned GlyphCode(WCHAR c)
{
    return (c == 0x08) ? 0x20u : static_cast<unsigned>(c);
}

/// Whether every character of the string is in the bake - then the GDI
/// road, otherwise the canvas.
bool AllBaked(const TBakedFont* c_pFont, const WCHAR* c_pText, int iCount)
{
    if (!c_pFont) return false;
    for (int i = 0; i < iCount; ++i)
        if (!c_pFont->kGlyphs.count(GlyphCode(c_pText[i]))) return false;
    return true;
}

/// Width of the background rectangle for a string on the GDI road: the sum
/// of advances PLUS the tail of the last cell by the engine's formula
/// (`GrpFontTexture.cpp:220`: B + ceil(A>0) + ceil(C>0) + 1) - the engine
/// reads that many columns from the DIB, and after the atlas wraps those
/// columns held bits of another character (reviewer: in 100 % of glyphs
/// size.cx > advance, by 1-5 px).
int BakedRunWidth(const TBakedFont& c_rkFont, const WCHAR* c_pText, int iCount)
{
    int iTotal = 0, iTail = 0;
    for (int i = 0; i < iCount; ++i)
    {
        const TGlyph& g = c_rkFont.kGlyphs.find(GlyphCode(c_pText[i]))->second;
        iTotal += g.iAdvance;
        int cx = static_cast<int>(g.B);
        if (g.A > 0.0f) cx += static_cast<int>(std::ceil(g.A));
        if (g.C > 0.0f) cx += static_cast<int>(std::ceil(g.C));
        cx += 1;
        iTail = (cx > g.iAdvance) ? cx - g.iAdvance : 0;
    }
    return iTotal + iTail;
}

/// GDI road at scale 1: background rectangle for the whole string FIRST
/// (as GDI: one rectangle, then glyphs - or the second character's
/// background would erase the first one's ink where it overhangs its
/// advance, C < 0), then the 1-bit glyphs at their ink offset.
void DrawBaked(TBitmap& rBitmap, const TBakedFont& c_rkFont, int x, int y,
               const WCHAR* c_pText, int iCount, bool bOpaque, unsigned uText, unsigned uBack)
{
    unsigned* puPixels = static_cast<unsigned*>(rBitmap.pvPixels);
    if (bOpaque)
    {
        const int iRun = BakedRunWidth(c_rkFont, c_pText, iCount);
        for (int yy = y; yy < y + c_rkFont.iLineHeight && yy < rBitmap.iHeight; ++yy)
            for (int xx = x; xx < x + iRun && xx < rBitmap.iWidth; ++xx)
                if (yy >= 0 && xx >= 0) puPixels[yy * rBitmap.iWidth + xx] = uBack;
    }
    int px = x;
    for (int i = 0; i < iCount; ++i)
    {
        const TGlyph& g = c_rkFont.kGlyphs.find(GlyphCode(c_pText[i]))->second;
        const int iRowBytes = (g.iWidth + 7) / 8;
        for (int yy = 0; yy < g.iHeight; ++yy)
        {
            const int Y = y + yy;
            if (Y < 0 || Y >= rBitmap.iHeight) continue;
            const unsigned char* c_pRow = &c_rkFont.vecBits[g.uBits + static_cast<size_t>(yy) * iRowBytes];
            for (int xx = 0; xx < g.iWidth; ++xx)
            {
                if (!(c_pRow[xx >> 3] & (0x80 >> (xx & 7)))) continue;
                const int X = px + g.iInkX + xx;
                if (X < 0 || X >= rBitmap.iWidth) continue;
                puPixels[Y * rBitmap.iWidth + X] = uText;
            }
        }
        px += g.iAdvance;
    }
}

/// GDI road AT GUI SCALE: glyphs from the `lfHeight * scale`
/// bake go into the DIB's SHADOW (physical px) at (x * scale, y * scale),
/// and the logical DIB gets their coarse image (every physical pixel lights
/// the logical pixel above it) - the engine reads the DIB only to binarise
/// and copies it into the texture, which gl_device.cpp swaps for the shadow.
void DrawBakedScaled(TBitmap& rBitmap, const TBakedFont& c_rkFont, float fScale, int x, int y,
                     const WCHAR* c_pText, int iCount, bool bOpaque, unsigned uText, unsigned uBack)
{
    if (rBitmap.vecShadow.empty())
    {
        rBitmap.iShadowWidth  = static_cast<int>(rBitmap.iWidth  * fScale + 0.5f);
        rBitmap.iShadowHeight = static_cast<int>(rBitmap.iHeight * fScale + 0.5f);
        rBitmap.vecShadow.assign(static_cast<size_t>(rBitmap.iShadowWidth) * rBitmap.iShadowHeight, 0);
    }
    unsigned* puPixels = static_cast<unsigned*>(rBitmap.pvPixels);
    const int fx = static_cast<int>(x * fScale + 0.5f);
    const int fy = static_cast<int>(y * fScale + 0.5f);
    const unsigned short usText = (uText & 0x00FFFFFFu) ? 0xFFFFu : 0u;
    const unsigned short usBack = (uBack & 0x00FFFFFFu) ? 0xFFFFu : 0u;

    // The logical cell the engine reads is `+1` wider - `scale` physical
    // pixels - so the tail in the shadow is widened by that much.
    const int iShadowRun = BakedRunWidth(c_rkFont, c_pText, iCount) + static_cast<int>(fScale + 1.0f);
    if (bOpaque)
    {
        for (int yy = fy; yy < fy + c_rkFont.iLineHeight && yy < rBitmap.iShadowHeight; ++yy)
            for (int xx = fx; xx < fx + iShadowRun && xx < rBitmap.iShadowWidth; ++xx)
                if (yy >= 0 && xx >= 0) rBitmap.vecShadow[static_cast<size_t>(yy) * rBitmap.iShadowWidth + xx] = usBack;
        const int iLogicalRun = static_cast<int>(iShadowRun / fScale) + 1;
        const int iLogicalHeight = static_cast<int>(c_rkFont.iLineHeight / fScale) + 1;
        for (int yy = y; yy < y + iLogicalHeight && yy < rBitmap.iHeight; ++yy)
            for (int xx = x; xx < x + iLogicalRun && xx < rBitmap.iWidth; ++xx)
                if (yy >= 0 && xx >= 0) puPixels[yy * rBitmap.iWidth + xx] = uBack;
    }
    int px = fx;
    for (int i = 0; i < iCount; ++i)
    {
        const TGlyph& g = c_rkFont.kGlyphs.find(GlyphCode(c_pText[i]))->second;
        const int iRowBytes = (g.iWidth + 7) / 8;
        for (int yy = 0; yy < g.iHeight; ++yy)
        {
            const int Y = fy + yy;
            if (Y < 0 || Y >= rBitmap.iShadowHeight) continue;
            const unsigned char* c_pRow = &c_rkFont.vecBits[g.uBits + static_cast<size_t>(yy) * iRowBytes];
            for (int xx = 0; xx < g.iWidth; ++xx)
            {
                if (!(c_pRow[xx >> 3] & (0x80 >> (xx & 7)))) continue;
                const int X = px + g.iInkX + xx;
                if (X < 0 || X >= rBitmap.iShadowWidth) continue;
                rBitmap.vecShadow[static_cast<size_t>(Y) * rBitmap.iShadowWidth + X] = usText;
                const int LX = static_cast<int>(X / fScale), LY = static_cast<int>(Y / fScale);
                if (LX >= 0 && LX < rBitmap.iWidth && LY >= 0 && LY < rBitmap.iHeight)
                    puPixels[LY * rBitmap.iWidth + LX] = uText;
            }
        }
        px += g.iAdvance;
    }
}

/// `COLORREF` is BGR, not RGB - the trap noted at the `RGB` macro in
/// win32_compat.h. Swapping red and blue breaks nothing; it gives strings
/// in the wrong colour.
std::string ToCss(COLORREF dwColour)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgb(%u,%u,%u)",
                  static_cast<unsigned>(GetRValue(dwColour)),
                  static_cast<unsigned>(GetGValue(dwColour)),
                  static_cast<unsigned>(GetBValue(dwColour)));
    return buf;
}

/// UTF-16 -> UTF-8. TMP4 calls the `...W` variants; the canvas takes UTF-8.
std::string ToUtf8(const WCHAR* c_pText, int iCount)
{
    std::string kOut;
    if (!c_pText) return kOut;
    for (int i = 0; i < iCount; ++i)
    {
        unsigned int c = static_cast<unsigned int>(c_pText[i]);
        if (c < 0x80)
        {
            kOut += static_cast<char>(c);
        }
        else if (c < 0x800)
        {
            kOut += static_cast<char>(0xC0 | (c >> 6));
            kOut += static_cast<char>(0x80 | (c & 0x3F));
        }
        else
        {
            kOut += static_cast<char>(0xE0 | (c >> 12));
            kOut += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            kOut += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return kOut;
}

}  // namespace

// ---------------------------------------------------------------------------
// Browser side (m2w.text*, runtime.js)
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__

/// A 2D canvas of the given size; returns its index (m2w.textCanvasCreate).
EM_JS(int, m2w_canvas_create, (int iWidth, int iHeight), {
    return m2w.textCanvasCreate(iWidth, iHeight);
});

/// Sets the canvas font from a CSS string (m2w.textCanvasFont).
EM_JS(void, m2w_canvas_font, (int iId, const char* c_szCss), {
    m2w.textCanvasFont(iId, UTF8ToString(c_szCss));
});

/// Width of the string in the canvas font (m2w.textMeasure).
EM_JS(double, m2w_canvas_measure, (int iId, const char* c_szText), {
    return m2w.textMeasure(iId, UTF8ToString(c_szText));
});

/// Width, left and right bounding box into `pdOut[3]` - one crossing
/// instead of three; `GetCharABCWidthsFloatW` asks for whole
/// character ranges.
EM_JS(void, m2w_canvas_measure_three, (int iId, const char* c_szText, double* pdOut), {
    m2w.heapDoubles(pdOut, m2w.textMeasureThree(iId, UTF8ToString(c_szText)));
});

/// Cumulative widths of the prefixes of the string (real kerning).
EM_JS(int, m2w_canvas_measure_prefixes, (int iId, const char* c_szText, double* pdOut, int iCapacity), {
    var a = m2w.textMeasurePrefixes(iId, UTF8ToString(c_szText), iCapacity);
    for (var i = 0; i < a.length; ++i) HEAPF64[(pdOut >> 3) + i] = a[i];
    return a.length;
});

/// Right edge of the string's ink (m2w.textBboxRight).
EM_JS(double, m2w_canvas_bbox_right, (int iId, const char* c_szText), {
    return m2w.textBboxRight(iId, UTF8ToString(c_szText));
});

/// Ink height of the string (m2w.textHeight).
EM_JS(double, m2w_canvas_height, (int iId, const char* c_szText), {
    return m2w.textHeight(iId, UTF8ToString(c_szText));
});

/// Full line height of the font, not of a particular string.
EM_JS(double, m2w_canvas_line_height, (int iId), {
    return m2w.textLineHeight(iId);
});

/// Draws the string (m2w.textDraw); opaque fills the background first.
EM_JS(void, m2w_canvas_draw, (int iId, int x, int y, const char* c_szText,
                              const char* c_szColour, const char* c_szBack, int bOpaque), {
    m2w.textDraw(iId, x, y, UTF8ToString(c_szText), UTF8ToString(c_szColour),
                 UTF8ToString(c_szBack), bOpaque);
});

/// Copies the rectangle [x0, x1) x [y0, y1) of the canvas into the DIB.
EM_JS(void, m2w_canvas_read_rows,
      (int iId, void* pvTarget, int iStride, int x0, int x1, int y0, int y1), {
    m2w.textReadRows(iId, pvTarget, iStride, x0, x1, y0, y1);
});

#else   // building outside emscripten (syntax-only unit tests)
static int    m2w_canvas_create(int, int) { return -1; }
static void   m2w_canvas_font(int, const char*) {}
static double m2w_canvas_measure(int, const char*) { return 0.0; }
static void   m2w_canvas_measure_three(int, const char*, double* w)
{ if (w) { w[0] = 0.0; w[1] = 0.0; w[2] = 0.0; } }
static int    m2w_canvas_measure_prefixes(int, const char*, double*, int) { return 0; }
static double m2w_canvas_bbox_right(int, const char*) { return 0.0; }
static double m2w_canvas_height(int, const char*) { return 0.0; }
static double m2w_canvas_line_height(int) { return 0.0; }
static void   m2w_canvas_draw(int, int, int, const char*, const char*, const char*, int) {}
static void   m2w_canvas_read_rows(int, void*, int, int, int, int, int) {}
#endif

// ---------------------------------------------------------------------------
// The functions TMP4 calls
// ---------------------------------------------------------------------------

/// GDI: a memory device context.
HDC CreateCompatibleDC(HDC /*hdc*/)
{
    const HDC hContext = NewHandle<HDC>();
    g_kContexts[hContext] = TContext();
    return hContext;
}

/// GDI: a 32-bit bitmap with direct pixel access (`*ppvBits`).
HBITMAP CreateDIBSection(HDC /*hdc*/, CONST BITMAPINFO* c_pBmi, UINT /*uUsage*/,
                         void** ppvBits, HANDLE /*hSection*/, DWORD /*dwOffset*/)
{
    if (!c_pBmi || !ppvBits) return nullptr;

    TBitmap m;
    m.iWidth = static_cast<int>(c_pBmi->bmiHeader.biWidth);
    // In BMP a positive height means bottom-up. GDI is usually given a
    // NEGATIVE one here so that rows run top-down - as on the canvas.
    m.iHeight = c_pBmi->bmiHeader.biHeight < 0
                ? -static_cast<int>(c_pBmi->bmiHeader.biHeight)
                :  static_cast<int>(c_pBmi->bmiHeader.biHeight);
    if (m.iWidth <= 0 || m.iHeight <= 0) return nullptr;

    m.uBytes = static_cast<size_t>(m.iWidth) * m.iHeight * 4;
    m.pvPixels = std::calloc(1, m.uBytes);
    if (!m.pvPixels) return nullptr;

    const HBITMAP hBitmap = NewHandle<HBITMAP>();
    g_kBitmaps[hBitmap] = m;
    *ppvBits = m.pvPixels;
    return hBitmap;
}

/// GDI: a font from a `LOGFONT` - the CSS string for the canvas and, when
/// the package has it, the baked GDI font.
HFONT CreateFontIndirectA(CONST LOGFONTA* c_pLogFont)
{
    if (!c_pLogFont) return nullptr;

    TFont kFont;
    // `lfHeight` in GDI: NEGATIVE is the character height, positive the
    // whole cell with leading. CSS `px` matches the first, so the absolute
    // value is taken; with a positive value the string comes out slightly
    // larger than on Windows.
    kFont.iHeightPx = c_pLogFont->lfHeight < 0 ? -c_pLogFont->lfHeight : c_pLogFont->lfHeight;
    if (kFont.iHeightPx <= 0) kFont.iHeightPx = 12;

    std::string kCss;
    if (c_pLogFont->lfItalic) kCss += "italic ";
    if (c_pLogFont->lfWeight >= FW_BOLD) kCss += "bold ";
    kCss += std::to_string(kFont.iHeightPx);
    kCss += "px \"";
    kCss += c_pLogFont->lfFaceName[0] ? c_pLogFont->lfFaceName : "sans-serif";
    // The fallback family matters: font names from the game files may not
    // exist in the browser, and without it the string vanishes instead of
    // being substituted.
    kCss += "\", sans-serif";
    kFont.kCss = kCss;

    // Baked GDI. Empty face name: GDI in the original takes the
    // face from `GetFontFaceFromCodePage` (CTextBar) - "Arial" for CP 1250.
    std::string kFace = c_pLogFont->lfFaceName[0] ? c_pLogFont->lfFaceName : "Arial";
    for (size_t i = 0; i < kFace.size(); ++i)
        kFace[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(kFace[i])));
    const std::string kSuffix = std::string("_") + (c_pLogFont->lfItalic ? "i" : "n") + "_" +
                                (c_pLogFont->lfWeight >= FW_BOLD ? "b" : "n");
    // GUI scale: first the bake at lfHeight * scale (letters
    // sharp in physical pixels), then the plain one (the GPU then enlarges
    // them with a filter - softly).
    const float fScale = M2W_UiScale();
    if (fScale != 1.0f)
    {
        const int iPhysical = static_cast<int>(kFont.iHeightPx * fScale + 0.5f);
        kFont.pBaked = LoadBakedFont(kFace + "_" + std::to_string(iPhysical) + kSuffix);
        if (kFont.pBaked) kFont.fScale = fScale;
    }
    if (!kFont.pBaked)
        kFont.pBaked = LoadBakedFont(kFace + "_" + std::to_string(kFont.iHeightPx) + kSuffix);

    const HFONT hFont = NewHandle<HFONT>();
    g_kFonts[hFont] = kFont;
    return hFont;
}

namespace
{

/// Creates the context's canvas once its size is known (from the bitmap).
void EnsureCanvas(TContext& rContext)
{
    if (rContext.iCanvas >= 0 || !rContext.hBitmap) return;
    const TBitmap& m = g_kBitmaps[rContext.hBitmap];
    rContext.iCanvas = m2w_canvas_create(m.iWidth, m.iHeight);
    if (rContext.hFont && rContext.iCanvas >= 0)
        m2w_canvas_font(rContext.iCanvas, g_kFonts[rContext.hFont].kCss.c_str());
}

/// Canvas road of `TextOutW`: draws, then copies back ONLY the touched
/// rectangle: four rows of slack above `y` for
/// accents; below, the ink height plus eight rows with a TRANSPARENT
/// background, or to the bottom of the canvas with an OPAQUE one
/// (`m2w.textDraw` fills down to the bottom). Columns: NEVER left of `x`
/// (that is the previous, already stored character's cell),
/// to the right only this character's own ink with a
/// transparent background, the full width with an opaque one.
void DrawCanvas(TContext& rContext, const TBitmap& c_rkBitmap, int x, int y,
                const WCHAR* c_pText, int iCount)
{
    const std::string kUtf8 = ToUtf8(c_pText, iCount);
    const bool bOpaque = (rContext.iBackMode == OPAQUE);
    m2w_canvas_draw(rContext.iCanvas, x, y, kUtf8.c_str(),
                    ToCss(rContext.dwTextColour).c_str(),
                    ToCss(rContext.dwBackColour).c_str(),
                    bOpaque ? 1 : 0);

    const int iInkHeight = static_cast<int>(m2w_canvas_height(rContext.iCanvas, kUtf8.c_str()));

    int y0 = y - 4;
    if (y0 < 0)
        y0 = 0;
    int y1 = bOpaque ? c_rkBitmap.iHeight : (y + iInkHeight + 8);
    if (y1 > c_rkBitmap.iHeight)
        y1 = c_rkBitmap.iHeight;

    int x0, x1;
    if (bOpaque)
    {
        x0 = 0;
        x1 = c_rkBitmap.iWidth;
    }
    else
    {
        const double dRight = m2w_canvas_bbox_right(rContext.iCanvas, kUtf8.c_str());
        x0 = x;
        x1 = x + static_cast<int>(dRight + 0.999) + 1;
    }

    m2w_canvas_read_rows(rContext.iCanvas, c_rkBitmap.pvPixels, c_rkBitmap.iWidth, x0, x1, y0, y1);
}

}  // namespace

/// GDI: binds a font or bitmap to the context and returns the PREVIOUS one
/// - TMP4 restores it after drawing.
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ hObject)
{
    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end()) return nullptr;
    TContext& k = it->second;

    if (g_kFonts.count(reinterpret_cast<HFONT>(hObject)))
    {
        const HGDIOBJ hPrevious = k.hFont;
        k.hFont = reinterpret_cast<HFONT>(hObject);
        if (k.iCanvas >= 0)
            m2w_canvas_font(k.iCanvas, g_kFonts[k.hFont].kCss.c_str());
        return hPrevious;
    }
    if (g_kBitmaps.count(reinterpret_cast<HBITMAP>(hObject)))
    {
        const HGDIOBJ hPrevious = k.hBitmap;
        k.hBitmap = reinterpret_cast<HBITMAP>(hObject);
        EnsureCanvas(k);
        return hPrevious;
    }
    return nullptr;
}

/// GDI: frees a bitmap (and its pixels) or a font.
BOOL DeleteObject(HGDIOBJ hObject)
{
    auto m = g_kBitmaps.find(reinterpret_cast<HBITMAP>(hObject));
    if (m != g_kBitmaps.end())
    {
        std::free(m->second.pvPixels);
        g_kBitmaps.erase(m);
        return TRUE;
    }
    g_kFonts.erase(reinterpret_cast<HFONT>(hObject));
    return TRUE;
}

/// GDI: frees a memory context.
BOOL DeleteDC(HDC hdc)
{
    g_kContexts.erase(hdc);
    return TRUE;
}

/// GDI: text colour (BGR), returns the previous one.
COLORREF SetTextColor(HDC hdc, COLORREF dwColour)
{
    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end()) return 0;
    const COLORREF dwPrevious = it->second.dwTextColour;
    it->second.dwTextColour = dwColour;
    return dwPrevious;
}

/// GDI: background colour (BGR), returns the previous one.
COLORREF SetBkColor(HDC hdc, COLORREF dwColour)
{
    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end()) return 0;
    const COLORREF dwPrevious = it->second.dwBackColour;
    it->second.dwBackColour = dwColour;
    return dwPrevious;
}

/// GDI: OPAQUE / TRANSPARENT background, returns the previous mode.
int SetBkMode(HDC hdc, int iMode)
{
    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end()) return 0;
    const int iPrevious = it->second.iBackMode;
    it->second.iBackMode = iMode;
    return iPrevious;
}

/// GDI: draws the string into the DIB - baked glyphs when every character
/// is baked (GDI road; at GUI scale into the shadow too),
/// otherwise through the canvas. DIB pixels are BGRA; the engine reads
/// blue (`& 0xff`), so the text colour goes in as the COLORREF (BGR).
BOOL TextOutW(HDC hdc, int x, int y, const WCHAR* c_pText, int iCount)
{
    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end() || !c_pText || iCount <= 0) return FALSE;
    TContext& k = it->second;
    EnsureCanvas(k);
    if (k.iCanvas < 0 || !k.hBitmap) return FALSE;

    const TBakedFont* c_pBaked = k.hFont ? g_kFonts[k.hFont].pBaked : nullptr;
    if (AllBaked(c_pBaked, c_pText, iCount))
    {
        TBitmap& m = g_kBitmaps[k.hBitmap];
        const unsigned uText = 0xFF000000u | (static_cast<unsigned>(k.dwTextColour) & 0x00FFFFFFu);
        const unsigned uBack = static_cast<unsigned>(k.dwBackColour) & 0x00FFFFFFu;
        const bool bOpaque = (k.iBackMode == OPAQUE);
        const float fScale = g_kFonts[k.hFont].fScale;
        if (fScale != 1.0f)
            DrawBakedScaled(m, *c_pBaked, fScale, x, y, c_pText, iCount, bOpaque, uText, uBack);
        else
            DrawBaked(m, *c_pBaked, x, y, c_pText, iCount, bOpaque, uText, uBack);
        return TRUE;
    }

    DrawCanvas(k, g_kBitmaps[k.hBitmap], x, y, c_pText, iCount);
    return TRUE;
}

/// GDI: `TextOutW` for a byte string.
BOOL TextOutA(HDC hdc, int x, int y, LPCSTR c_szText, int iCount)
{
    if (!c_szText || iCount <= 0) return FALSE;
    std::vector<WCHAR> vecWide(static_cast<size_t>(iCount));
    for (int i = 0; i < iCount; ++i)
        vecWide[i] = static_cast<unsigned char>(c_szText[i]);
    return TextOutW(hdc, x, y, vecWide.data(), iCount);
}

/// GDI: width of the string and the LINE height of the font (not of these
/// characters: "___" and "ABC" must give the same height).
BOOL GetTextExtentPoint32W(HDC hdc, const WCHAR* c_pText, int iCount, SIZE* pSize)
{
    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end() || !pSize) return FALSE;
    TContext& k = it->second;
    EnsureCanvas(k);
    if (k.iCanvas < 0) return FALSE;

    const TBakedFont* c_pBaked = k.hFont ? g_kFonts[k.hFont].pBaked : nullptr;
    if (AllBaked(c_pBaked, c_pText, iCount))
    {
        const float fScale = g_kFonts[k.hFont].fScale;
        LONG cx = 0;
        for (int i = 0; i < iCount; ++i)
            cx += c_pBaked->kGlyphs.find(GlyphCode(c_pText[i]))->second.iAdvance;
        // GUI scale: the engine counts in logical px - physical metrics /
        // scale, rounded up (the logical cell * scale must hold the glyph).
        pSize->cx = static_cast<LONG>(std::ceil(cx / fScale - 1e-4f));
        pSize->cy = static_cast<LONG>(std::ceil(c_pBaked->iLineHeight / fScale - 1e-4f));
        return TRUE;
    }
    const std::string kUtf8 = ToUtf8(c_pText, iCount);
    pSize->cx = static_cast<LONG>(m2w_canvas_measure(k.iCanvas, kUtf8.c_str()) + 0.5);
    // The real line height of the font, not the nominal size:
    // without room for descenders the atlas cells were too low.
    pSize->cy = k.hFont
        ? static_cast<LONG>(m2w_canvas_line_height(k.iCanvas) + 0.5)
        : 0;
    return TRUE;
}

/// GDI: `GetTextExtentPoint32W` for a byte string.
BOOL GetTextExtentPoint32A(HDC hdc, LPCSTR c_szText, int iCount, SIZE* pSize)
{
    if (!c_szText || iCount < 0) return FALSE;
    std::vector<WCHAR> vecWide(static_cast<size_t>(iCount));
    for (int i = 0; i < iCount; ++i)
        vecWide[i] = static_cast<unsigned char>(c_szText[i]);
    return GetTextExtentPoint32W(hdc, vecWide.data(), iCount, pSize);
}

/// GDI: the `ABC` triple per character - exact from the bake; from the
/// canvas an APPROXIMATION derived from the total width and the bounding
/// box, a fraction of a pixel off in the spacing, not an error.
BOOL GetCharABCWidthsFloatW(HDC hdc, UINT uFirst, UINT uLast, LPABCFLOAT pAbc)
{
    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end() || !pAbc || uLast < uFirst) return FALSE;
    TContext& k = it->second;
    EnsureCanvas(k);
    if (k.iCanvas < 0) return FALSE;

    const TBakedFont* c_pBaked = k.hFont ? g_kFonts[k.hFont].pBaked : nullptr;
    for (UINT uChar = uFirst, i = 0; uChar <= uLast; ++uChar, ++i)
    {
        if (c_pBaked)
        {
            auto g = c_pBaked->kGlyphs.find(uChar == 0x08 ? 0x20u : uChar);
            if (g != c_pBaked->kGlyphs.end())
            {
                const float fScale = g_kFonts[k.hFont].fScale;
                pAbc[i].abcfA = g->second.A / fScale;
                pAbc[i].abcfB = g->second.B / fScale;
                pAbc[i].abcfC = g->second.C / fScale;
                continue;
            }
        }
        const WCHAR aOne[1] = { static_cast<WCHAR>(uChar) };
        const std::string kUtf8 = ToUtf8(aOne, 1);

        double adOut[3] = { 0.0, 0.0, 0.0 };
        m2w_canvas_measure_three(k.iCanvas, kUtf8.c_str(), adOut);

        const double dWidth = adOut[0];
        const double dLeft  = adOut[1];
        const double dRight = adOut[2];

        pAbc[i].abcfA = static_cast<float>(-dLeft);
        pAbc[i].abcfB = static_cast<float>(dRight + dLeft);
        pAbc[i].abcfC = static_cast<float>(dWidth - (dRight + dLeft) + dLeft);
    }
    return TRUE;
}

/// GDI: only `lpDx` is implemented - the width of every character IN THE
/// CONTEXT of the whole string (real kerning), the one field of
/// this API the client uses. The rest of `GCP_RESULTSW` is zeroed: not
/// needed, and faking it without a measurement would be guessing.
DWORD GetCharacterPlacementW(HDC hdc, const WCHAR* c_pText, int iCount,
                             int /*iMaxExtent*/, LPGCP_RESULTSW pResults,
                             DWORD /*dwFlags*/)
{
    if (!pResults) return 0;
    pResults->nGlyphs = 0;
    if (pResults->lpCaretPos)
        for (int i = 0; i < iCount; ++i) pResults->lpCaretPos[i] = 0;

    auto it = g_kContexts.find(hdc);
    if (it == g_kContexts.end() || !c_pText || iCount <= 0 || !pResults->lpDx)
        return 0;
    TContext& k = it->second;
    EnsureCanvas(k);
    if (k.iCanvas < 0) return 0;

    const std::string kUtf8 = ToUtf8(c_pText, iCount);
    std::vector<double> vecCumulative(static_cast<size_t>(iCount), 0.0);
    const int iMeasured = m2w_canvas_measure_prefixes(k.iCanvas, kUtf8.c_str(), vecCumulative.data(), iCount);

    double dPrevious = 0.0;
    for (int i = 0; i < iMeasured; ++i)
    {
        const double dCharWidth = vecCumulative[static_cast<size_t>(i)] - dPrevious;
        pResults->lpDx[i] = static_cast<int>(dCharWidth + 0.5);
        dPrevious = vecCumulative[static_cast<size_t>(i)];
    }
    // Characters beyond what the browser measured (should not happen for
    // ordinary text) - zero rather than garbage.
    for (int i = iMeasured; i < iCount; ++i) pResults->lpDx[i] = 0;

    pResults->nGlyphs = static_cast<UINT>(iCount);
    return static_cast<DWORD>(dPrevious + 0.5);
}

/// GDI: copying a bitmap TO THE SCREEN. The screen is drawn by the graphics
/// layer, not GDI - this road leads nowhere and does not pretend to have
/// drawn; the row count the caller expects is returned so it is not taken
/// for a read error.
int SetDIBitsToDevice(HDC /*hdc*/, int /*xDest*/, int /*yDest*/, DWORD w, DWORD h,
                      int /*xSrc*/, int /*ySrc*/, UINT /*uStartScan*/, UINT uLines,
                      CONST void* /*c_pvBits*/, CONST BITMAPINFO* /*c_pBmi*/, UINT /*uColorUse*/)
{
    M2W_STUB("GDI::SetDIBitsToDevice (draws nothing on screen)");
    (void)w;
    return static_cast<int>(uLines ? uLines : h);
}

/// The physical-pixel shadow of the DIB whose logical content equals the
/// texture `c_pvData` (w x h, 16-bit) the engine is uploading, or NULL
/// (gl_device.cpp swaps the texture for it; the engine copies the DIB to the
/// texture 1:1 - `pwDst[x] = pdwSrc[x]`, the low 16 bits - so equality of
/// the whole content identifies the DIB).
const unsigned short* M2W_FontAtlasHiRes(const void* c_pvData, unsigned w, unsigned h,
                                         unsigned* puWidth, unsigned* puHeight)
{
    const unsigned short* c_pTexture = static_cast<const unsigned short*>(c_pvData);
    for (auto& rPair : g_kBitmaps)
    {
        TBitmap& m = rPair.second;
        if (m.vecShadow.empty() || static_cast<unsigned>(m.iWidth) != w ||
            static_cast<unsigned>(m.iHeight) != h)
            continue;
        const unsigned* c_puPixels = static_cast<const unsigned*>(m.pvPixels);
        const size_t n = static_cast<size_t>(w) * h;
        bool bEqual = true;
        for (size_t i = 0; i < n; ++i)
        {
            if (static_cast<unsigned short>(c_puPixels[i] & 0xFFFFu) != c_pTexture[i]) { bEqual = false; break; }
        }
        if (!bEqual) continue;
        *puWidth  = static_cast<unsigned>(m.iShadowWidth);
        *puHeight = static_cast<unsigned>(m.iShadowHeight);
        return &m.vecShadow[0];
    }
    return nullptr;
}
