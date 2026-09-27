#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""cursors.py - the game's mouse cursors (`.cur` from the TMP4 resources) as
PNG with their hotspot, for the client data package (`data/cursors/`).

In Windows the cursors were resources of the executable (`UserInterface.rc`:
`IDC_CURSOR_NORMAL CURSOR "Cursors\cursor.cur"`). The browser can show its
own cursor through `canvas.style.cursor = url(...) x y` - it needs a PNG and
the hotspot. This script reads the `.cur` directly: the ICONDIR header
(hotspot in bytes 10-13 of the entry), the XOR image through PIL, the AND
mask by hand (PIL loses it), and assembles RGBA.

Output: `data/cursors/<resource number>.png` + `data/cursors/index.txt`
(`number hotspotX hotspotY`). Used by `compat/cursor_web.cpp`.
"""
import io
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
SOURCE = os.path.join(workspace.SOURCE_LIBRARIES, 'UserInterface', 'Cursors')

# From `UserInterface/resource.h` + `UserInterface.rc` (resource number -> file).
RESOURCES = {
    102: 'cursor.cur',
    104: 'cursor_attack.cur',
    105: 'cursor_chair.cur',
    106: 'cursor_door.cur',
    107: 'cursor_no.cur',
    108: 'cursor_pick.cur',
    109: 'cursor_talk.cur',
    110: 'cursor_buy.cur',
    111: 'cursor_sell.cur',
    114: 'cursor_pick.cur',      # IDC_CURSOR_PICKUP - no file of its own in the .rc
    139: 'cursor_camera_rotate.cur',
    140: 'cursor_hsize.cur',
    141: 'cursor_vsize.cur',
    142: 'cursor_hvsize.cur',
}


def load_cur(path_str):
    """Returns (PIL RGBA image, hotspotX, hotspotY)."""
    from PIL import Image
    b = io.open(path_str, 'rb').read()
    how_many = struct.unpack_from('<H', b, 4)[0]
    if how_many < 1:
        raise ValueError('pusty .cur')
    w, h, _c, _r, hx, hy, size, off = struct.unpack_from('<BBBBHHII', b, 6)
    w = w or 256
    h = h or 256
    im = Image.open(path_str).convert('RGBA')
    # AND mask: after the DIB header (40 B), the palette, the XOR pixels.
    bi_size, bi_w, bi_h2, planes, bpp = struct.unpack_from('<IiiHH', b, off)
    clr_used = struct.unpack_from('<I', b, off + 32)[0]
    palette_size = (clr_used or (1 << bpp if bpp <= 8 else 0)) * 4
    xor_row = ((bi_w * bpp + 31) // 32) * 4
    and_row = ((bi_w + 31) // 32) * 4
    and_start = off + bi_size + palette_size + xor_row * h
    px = im.load()
    for y in range(h):
        row = b[and_start + (h - 1 - y) * and_row:]
        for x in range(w):
            if row[x >> 3] & (0x80 >> (x & 7)):
                r, g, bb, _a = px[x, y]
                px[x, y] = (r, g, bb, 0)
    return im, hx, hy


def build(target):
    """Writes the PNGs and the index to `target`. Returns the number of cursors."""
    os.makedirs(target, exist_ok=True)
    rows_list = []
    for number_of, file_path in sorted(RESOURCES.items()):
        path_str = os.path.join(SOURCE, file_path)
        if not os.path.exists(path_str):
            print('  missing %s' % file_path)
            continue
        im, hx, hy = load_cur(path_str)
        im.save(os.path.join(target, '%d.png' % number_of))
        rows_list.append('%d %d %d' % (number_of, hx, hy))
    io.open(os.path.join(target, 'index.txt'), 'w', encoding='ascii', newline='\n').write('\n'.join(rows_list) + '\n')
    return len(rows_list)


def main():
    """Builds the cursor PNGs (with hotspots) and index.txt into
        build/port/data/cursors and prints how many.

    """
    target = os.path.join(ROOT, 'build', 'port', 'data', 'cursors')
    n = build(target)
    print('cursors: %d -> %s' % (n, os.path.relpath(target, ROOT)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
