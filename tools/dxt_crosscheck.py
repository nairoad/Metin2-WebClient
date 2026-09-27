# -*- coding: utf-8 -*-
"""CROSS-check of DXT decompression with an independent decoder (Pillow).

What for: our own test checks what it computed itself. With DATA
- and the colour table of a DXT block is half data - that is not enough.
This script takes RANDOM blocks, passes them through our function and
through the Pillow decoder, and compares pixel by pixel.

Result: 46 080 pixels, three formats, agreement TO THE BIT.

Not part of the project's test suite, because it needs Pillow.
Run by hand:

    python tools/dxt_crosscheck.py

It needs the dump tool built first:

    em++ -std=c++17 -O1 -w -I<directory with dxt.h>         tools/dxt_crosscheck_dump.cpp <dxt.cpp>         -o <tmp>/dxtz.js -s NODERAWFS=1
"""
import io, os, random, struct, subprocess, sys, tempfile
from PIL import Image

NODE = "node"
# The dump tool built as in the docstring; its directory from M2W_DXT_TMP,
# else a folder in the system temp directory.
TMP = os.environ.get("M2W_DXT_TMP") or os.path.join(tempfile.gettempdir(), "m2w_dxt")
SCRIPT = os.path.join(TMP, "dxtz.js")
os.makedirs(TMP, exist_ok=True)

def dds_header(width, height, fourcc, data_size):
    """A minimal DDS header (FOURCC, one level) for `width` x `height` pixels and
    `data_size` bytes of blocks.
    """
    h = b"DDS "
    h += struct.pack("<I", 124)                      # dwSize
    kind_flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000       # CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE
    h += struct.pack("<I", kind_flags)
    h += struct.pack("<I", height)
    h += struct.pack("<I", width)
    h += struct.pack("<I", data_size)           # pitchOrLinearSize
    h += struct.pack("<I", 0)                        # depth
    h += struct.pack("<I", 0)                        # mipMapCount
    h += b"\0" * 44                                  # reserved
    h += struct.pack("<I", 32)                       # pf.dwSize
    h += struct.pack("<I", 0x4)                      # pf.flags = FOURCC
    h += fourcc
    h += b"\0" * 20
    h += struct.pack("<I", 0x1000)                   # caps = TEXTURE
    h += b"\0" * 16
    return h

FORMATS = {1: (b"DXT1", 8), 3: (b"DXT3", 16), 5: (b"DXT5", 16)}

def our_files(fmt, width, height, data):
    """Decodes the blocks with OUR DXT decoder (node script); ARGB per pixel, or
    None when it fails.
    """
    p = os.path.join(TMP, "blocks.bin")
    io.open(p, "wb").write(data)
    r = subprocess.run([NODE, SCRIPT, str(fmt), str(width), str(height)],
                       stdin=io.open(p, "rb"), capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return [int(x, 16) for x in r.stdout.split()]

def pillow(fmt, width, height, data):
    """Decodes the same blocks with Pillow (wrapped in a DDS file); ARGB per
    pixel.
    """
    fourcc, _ = FORMATS[fmt]
    p = os.path.join(TMP, "sample.dds")
    io.open(p, "wb").write(dds_header(width, height, fourcc, len(data)) + data)
    im = Image.open(p).convert("RGBA")
    pixels = list(im.getdata())
    return [(a << 24) | (r << 16) | (g << 8) | b for (r, g, b, a) in pixels]

random.seed(20260905)
WIDTH, HEIGHT = 16, 16
block_count = (WIDTH // 4) * (HEIGHT // 4)

for fmt in (1, 3, 5):
    _, byte_count = FORMATS[fmt]
    bad_hard, bad_mild, total_count = 0, 0, 0
    examples = []
    for sample_set in range(60):
        data = bytes(random.randrange(256) for _ in range(block_count * byte_count))
        a = our_files(fmt, WIDTH, HEIGHT, data)
        b = pillow(fmt, WIDTH, HEIGHT, data)
        if a is None:
            print("  our function REFUSED - this should not happen"); break
        for i, (x, y) in enumerate(zip(a, b)):
            total_count += 1
            aa = [(x >> s) & 255 for s in (24, 16, 8, 0)]
            bb = [(y >> s) & 255 for s in (24, 16, 8, 0)]
            extension = max(abs(p - q) for p, q in zip(aa, bb))
            if extension == 0:
                continue
            if extension <= 4 and aa[0] == bb[0]:
                bad_mild += 1
            else:
                bad_hard += 1
                if len(examples) < 5:
                    examples.append((sample_set, i, x, y))
    print("DXT%d: pixels %d | bit-exact %d | difference <=4 %d | DIVERGENT %d"
          % (fmt, total_count, total_count - bad_mild - bad_hard,
             bad_mild, bad_hard))
    for right_vec, i, x, y in examples:
        print("    sample %d pixel %d: ours %08X, Pillow %08X" % (right_vec, i, x, y))
