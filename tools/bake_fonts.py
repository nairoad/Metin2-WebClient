#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bake_fonts.py - baking the UI fonts with the real GDI.

The Windows client draws text through GDI: `CreateFontIndirect(LOGFONT)`,
`GetCharABCWidthsFloatW`, `TextOutW` into a DIB, and the engine
(`GrpFontTexture.cpp`) binarises every pixel with the test "blue != 0".
Tahoma at these sizes has READY pixel BITMAPS inside and GDI uses them -
hence the sharp, "pixel" look of Metin's text. The browser has no access to
them: the canvas draws letters anti-aliased and larger, and after the
binarisation a blob comes out (measured: the same text GDI vs canvas).

This script does with fonts what `bake_spt` does with trees: on a Windows
machine it makes EXACTLY the GDI calls the client makes, and stores the
result - for every character A/B/C, the advance width and a 1-bit bitmap.
`compat/platform_text.cpp` pastes these bitmaps into the DIB instead of
drawing on the canvas. The canvas stays as the fallback for characters
outside the bake.

Needs Windows (ctypes + gdi32). On another system it does nothing - the
client uses the canvas as before.

Format of `fonts/<face>_<height>_<i|n>_<b|n>.bin` (little-endian):
  8 B  "TMP4FNT1"
  u16  row height (GetTextExtentPoint32W .cy)
  u32  number of glyphs
  per glyph: u32 code, f32 A, f32 B, f32 C, i16 advance (GetCharWidth32W),
             i16 x0 (first ink column relative to the drawing point),
             u16 width, u16 height, then height * ceil(width/8) bytes of bits (MSB first)

Usage:  python tools/bake_fonts.py [target_directory]   (default build/port/data/fonts)
"""
import ctypes
import io
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(ROOT, 'build', 'port', 'data', 'fonts')

# (face, lfHeight, italic, bold) - what the game asks for: `Tahoma:12`,
# `Tahoma:12:Italic`, `Tahoma:14` (locale UI_DEF_FONT), `CTextBar`
# (face from GetFontFaceFromCodePage: Arial; 12 and bold 18).
BASE_FONTS = [
    ('Tahoma', 12, False, False),
    ('Tahoma', 12, True, False),
    ('Tahoma', 14, False, False),
    ('Tahoma', 16, False, False),
    ('Tahoma', 12, False, True),
    ('Arial', 12, False, False),
    ('Arial', 12, False, True),
    ('Arial', 18, False, True),
]
# GUI scale: the client asks for `lfHeight * scale`,
# so that the letters are sharp in physical pixels. We bake every face
# at the sizes for the typical scales.
SCALES = [1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0]
FONTS = []
for _face, _height, _i, _b in BASE_FONTS:
    for _s in SCALES:
        _w = int(_height * _s + 0.5)
        if (_face, _w, _i, _b) not in FONTS:
            FONTS.append((_face, _w, _i, _b))

# Character ranges: ASCII, Latin-1, Latin Extended-A (Polish, Czech,
# Hungarian...), typographic (quotation marks, dashes, ellipsis, euro).
RANGES = [(0x20, 0x7E), (0xA0, 0x17F), (0x192, 0x192), (0x2C6, 0x2C7),
           (0x2D8, 0x2DD), (0x2013, 0x2014), (0x2018, 0x201E), (0x2020, 0x2022),
           (0x2026, 0x2026), (0x2030, 0x2030), (0x2039, 0x203A), (0x20AC, 0x20AC),
           (0x2122, 0x2122)]


def key_name(face, height, italic, bold):
    """The bake key of a font: lower-case face, height, italic and bold flags."""
    return '%s_%d_%s_%s' % (face.lower(), height, 'i' if italic else 'n', 'b' if bold else 'n')


def _gdi():
    """The gdi32 functions with their ctypes signatures set (handles as void
    pointers);
    returns (gdi, wintypes).
    """
    import ctypes.wintypes as w
    gdi = ctypes.windll.gdi32
    V = ctypes.c_void_p
    for f in ('SelectObject', 'DeleteObject', 'CreateCompatibleDC', 'CreateDIBSection', 'CreateFontIndirectW'):
        getattr(gdi, f).restype = V
    gdi.SelectObject.argtypes = [V, V]
    gdi.DeleteObject.argtypes = [V]
    gdi.DeleteDC.argtypes = [V]
    gdi.SetTextColor.argtypes = [V, w.DWORD]
    gdi.SetBkColor.argtypes = [V, w.DWORD]
    gdi.SetBkMode.argtypes = [V, ctypes.c_int]
    gdi.TextOutW.argtypes = [V, ctypes.c_int, ctypes.c_int, w.LPCWSTR, ctypes.c_int]
    gdi.GetCharABCWidthsFloatW.argtypes = [V, w.UINT, w.UINT, V]
    gdi.GetCharWidth32W.argtypes = [V, w.UINT, w.UINT, V]
    gdi.GetTextExtentPoint32W.argtypes = [V, w.LPCWSTR, ctypes.c_int, V]
    gdi.GetTextFaceW.argtypes = [V, ctypes.c_int, V]
    gdi.CreateDIBSection.argtypes = [V, V, w.UINT, V, V, w.DWORD]
    gdi.CreateFontIndirectW.argtypes = [V]
    gdi.CreateCompatibleDC.argtypes = [V]
    return gdi, w


def bake_one(face, height, italic, bold):
    """Returns (cy, list of glyphs), or None when GDI did not give this face."""
    gdi, w = _gdi()

    class LOGFONTW(ctypes.Structure):
        _fields_ = [('lfHeight', w.LONG), ('lfWidth', w.LONG), ('lfEscapement', w.LONG),
                    ('lfOrientation', w.LONG), ('lfWeight', w.LONG), ('lfItalic', w.BYTE),
                    ('lfUnderline', w.BYTE), ('lfStrikeOut', w.BYTE), ('lfCharSet', w.BYTE),
                    ('lfOutPrecision', w.BYTE), ('lfClipPrecision', w.BYTE), ('lfQuality', w.BYTE),
                    ('lfPitchAndFamily', w.BYTE), ('lfFaceName', w.WCHAR * 32)]

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', w.DWORD), ('biWidth', w.LONG), ('biHeight', w.LONG),
                    ('biPlanes', w.WORD), ('biBitCount', w.WORD), ('biCompression', w.DWORD),
                    ('biSizeImage', w.DWORD), ('biXPelsPerMeter', w.LONG), ('biYPelsPerMeter', w.LONG),
                    ('biClrUsed', w.DWORD), ('biClrImportant', w.DWORD)]

    class ABCFLOAT(ctypes.Structure):
        _fields_ = [('abcfA', ctypes.c_float), ('abcfB', ctypes.c_float), ('abcfC', ctypes.c_float)]

    W, H, PAD = 96, 64, 24
    dc = gdi.CreateCompatibleDC(None)
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(bmi)
    bmi.biWidth = W
    bmi.biHeight = -H
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bits = ctypes.c_void_p()
    hbm = gdi.CreateDIBSection(dc, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    gdi.SelectObject(dc, hbm)

    # EXACTLY as GrpFontTexture::GetFont / CTextBar::__SetFont.
    lf = LOGFONTW()
    lf.lfHeight = height
    lf.lfWeight = 700 if bold else 400
    lf.lfItalic = 1 if italic else 0
    lf.lfCharSet = 238            # EASTEUROPE_CHARSET (CP_1250)
    lf.lfQuality = 4              # ANTIALIASED_QUALITY
    lf.lfFaceName = face
    hf = gdi.CreateFontIndirectW(ctypes.byref(lf))
    gdi.SelectObject(dc, hf)
    gdi.SetTextColor(dc, 0xFFFFFF)
    gdi.SetBkColor(dc, 0)

    name = ctypes.create_unicode_buffer(64)
    gdi.GetTextFaceW(dc, 64, name)
    if name.value.lower() != face.lower():
        # A silent face substitution would give the wrong file under the right name - we refuse.
        print('  ERROR: GDI substituted "%s" for "%s" - the font is not installed' % (name.value, face))
        gdi.DeleteObject(hf)
        gdi.DeleteObject(hbm)
        gdi.DeleteDC(dc)
        return None

    abc = ABCFLOAT()
    sz = w.SIZE()
    step = ctypes.c_int()
    gdi.GetTextExtentPoint32W(dc, 'M', 1, ctypes.byref(sz))
    cy = sz.cy
    if cy <= 0 or cy > H:
        return None

    glyphs = []
    zero = b'\x00' * (W * H * 4)
    for start_from, do in RANGES:
        for code in range(start_from, do + 1):
            ch = chr(code)
            if not gdi.GetCharABCWidthsFloatW(dc, code, code, ctypes.byref(abc)):
                continue
            gdi.GetCharWidth32W(dc, code, code, ctypes.byref(step))
            ctypes.memmove(bits, zero, len(zero))
            gdi.TextOutW(dc, PAD, 0, ch, 1)
            buf = ctypes.string_at(bits, W * H * 4)
            columns = [x for x in range(W) if any(buf[(y * W + x) * 4] for y in range(cy))]
            if columns:
                x0, x1 = columns[0], columns[-1] + 1
            else:
                x0 = x1 = PAD
            width = x1 - x0
            rows_list = bytearray()
            byte_count = (width + 7) // 8
            for y in range(cy):
                row_index = bytearray(byte_count)
                for x in range(width):
                    if buf[(y * W + x0 + x) * 4]:
                        row_index[x // 8] |= 0x80 >> (x % 8)
                rows_list += row_index
            glyphs.append((code, abc.abcfA, abc.abcfB, abc.abcfC, step.value, x0 - PAD, width, cy, bytes(rows_list)))

    gdi.DeleteObject(hf)
    gdi.DeleteObject(hbm)
    gdi.DeleteDC(dc)
    return cy, glyphs


def save(path_str, cy, glyphs):
    """Writes one baked font (`TMP4FNT1`, cell height, glyph count, then per
    glyph its ABC widths, advance, box and bits) to `path_str`.
    """
    b = io.BytesIO()
    b.write(b'TMP4FNT1')
    b.write(struct.pack('<HI', cy, len(glyphs)))
    for code, A, B, C, step, x0, width, height, bit_data in glyphs:
        b.write(struct.pack('<IfffhhHH', code, A, B, C, step, x0, width, height))
        b.write(bit_data)
    os.makedirs(os.path.dirname(path_str), exist_ok=True)
    io.open(path_str, 'wb').write(b.getvalue())


def build(target=TARGET):
    """Bakes all the fonts from the list. Returns the number of files (0 outside Windows)."""
    if sys.platform != 'win32':
        print('font baking: Windows only (GDI) - skipping, the client will use the canvas')
        return 0
    how_many = 0
    for face, height, italic, bold in FONTS:
        result = bake_one(face, height, italic, bold)
        if not result:
            print('  %s: GDI gave no font' % key_name(face, height, italic, bold))
            continue
        cy, glyphs = result
        path_str = os.path.join(target, key_name(face, height, italic, bold) + '.bin')
        save(path_str, cy, glyphs)
        print('  %s: line %d px, glyphs %d, %d B' % (key_name(face, height, italic, bold), cy, len(glyphs), os.path.getsize(path_str)))
        how_many += 1
    return how_many


if __name__ == '__main__':
    sys.exit(0 if build(sys.argv[1] if len(sys.argv) > 1 else TARGET) > 0 else 1)
