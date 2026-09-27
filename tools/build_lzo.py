#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Builds `build/port/lib/liblzo.a` FROM SCRATCH, from the LZO 2.10 sources.

WHY THIS SCRIPT EXISTS
======================
The same shape of risk as `libeterpack.a` before `build_eterpack.py`
: `liblzo.a` in `build/port/lib/` was assembled by hand,
outside `tools/` and outside control. "What nobody builds does not exist
for the measurement."

FROM WHICH SOURCES - AND WHY IT MATTERS
=======================================
The TMP4 repository has TWO versions of LZO (section DV):
the headers `extern/include/lzo/` are 2.10, and the sources `DumpProto/lzo/`
are 1.08 - a leftover of another tool. The first version of this script
 built the library from 1.08 and stitched it to the 2.10 headers
with a bridge `__lzo_init_v2 -> __lzo_init2`. A warning predicted the effect
literally ("silent errors, with unusual data"): the ninth argument of
`lzo_init()` means `sizeof(lzo_callback_t)` in 2.10, and
`sizeof(lzo_compress_t)` in 1.08 - `lzo_init()` returned an error, `CLZO`
was left without a work buffer, and the first compression (the guild mark,
after logging in) wrote to address 0 (caught by -sSAFE_HEAP=2).

So we build from the REAL 2.10 sources - `build/port/vendor/lzo-2.10`
(tar.gz from oberhumer.com, sha256 in the README, as) - with
THEIR OWN headers, and without any bridge. The whole `src/` (67 files):
LZO 2.10 builds under emscripten without patches, and picking
files "by eye" is also a needless risk.

CHECK
=====
1. `__lzo_init_v2` must be DEFINED in the archive (letter `T`), not
   undefined - exactly the symbol whose absence in 1.08 forced the bridge.
   If it is not there, the wrong version is being built.
2. The four functions the game code uses (`EterBase/lzo.cpp`,
   `MarkImage.cpp`): `lzo1x_1_compress`, `lzo1x_999_compress`,
   `lzo1x_decompress`, `lzo1x_decompress_safe` - also defined.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
LZO_ROOT = os.path.join(PORT, 'vendor', 'lzo-2.10')
LZO_SRC = os.path.join(LZO_ROOT, 'src')
LZO_INC = os.path.join(LZO_ROOT, 'include')
LIBDIR = os.path.join(PORT, 'lib')
OBJDIR = os.path.join(PORT, 'obj', 'lzo')
ARCHIVE = os.path.join(LIBDIR, 'liblzo.a')
EMSDK = workspace.EMSDK

# The source archive and its checksum (earlier the value was only in a
# private README). The tool verifies the archive before unpacking it.
LZO_URL = 'https://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz'
LZO_TAR = os.path.join(PORT, 'vendor', 'lzo-2.10.tar.gz')
LZO_SHA256 = 'c0f892943208266f9b6543b3ae308fab6284c5c90e627931446fb49b4221a072'

REQUIRED = ['__lzo_init_v2', 'lzo1x_1_compress', 'lzo1x_999_compress',
            'lzo1x_decompress', 'lzo1x_decompress_safe']


def tool_name(name):
    """Path of an emsdk tool: from PATH, else from the EMSDK directory; exits when
    it is nowhere.
    """
    p = shutil.which(name)
    if p:
        return p
    for directory in (os.path.join(EMSDK, 'upstream', 'bin'),
                os.path.join(EMSDK, 'upstream', 'emscripten')):
        for ext in ('', '.exe', '.bat'):
            k = os.path.join(directory, name + ext)
            if os.path.exists(k):
                return k
    raise SystemExit(f'{name} not found - add emsdk to PATH')


def compile_all(path_str, output):
    """Compiles one LZO C source to `output`; None on success, else the first
    `error:` line.
    """
    r = subprocess.run(
        [tool_name('emcc'), '-c', '-O2', '-w',
         '-I' + LZO_INC, '-I' + LZO_SRC,
         path_str, '-o', output],
        capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(output):
        return None
    return next((l for l in r.stderr.splitlines() if 'error:' in l), '?')


def unpack_sources():
    """Downloads the LZO archive once (LZO_URL), checks its SHA-256 and unpacks
    it into build/port/vendor; False when the download fails or the sum does
    not match.
    """
    import hashlib
    import tarfile
    if not os.path.isfile(LZO_TAR):
        # downloaded once, then checked like any archive (the clean-clone
        # trial had to fetch it by hand while build_python.py fetched its own)
        import urllib.request
        os.makedirs(os.path.dirname(LZO_TAR), exist_ok=True)
        print(f'downloading {LZO_URL}')
        try:
            urllib.request.urlretrieve(LZO_URL, LZO_TAR)
        except OSError as e:
            print(f'download failed ({e}) - put the archive at {os.path.relpath(LZO_TAR, ROOT)} by hand')
            return False
    with open(LZO_TAR, 'rb') as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != LZO_SHA256:
        # a broken or truncated download must not block every later run
        os.remove(LZO_TAR)
        print(f'{os.path.relpath(LZO_TAR, ROOT)}: sha256 {digest}, expected {LZO_SHA256} - '
              f'the file was removed (broken download?); run the step again to fetch it anew')
        return False
    with tarfile.open(LZO_TAR, 'r:gz') as t:
        # `filter='data'` (no absolute paths, no links out of the target) exists
        # since 3.11.4 / 3.12; an older interpreter unpacks without it.
        if hasattr(tarfile, 'data_filter'):
            t.extractall(os.path.dirname(LZO_TAR), filter='data')
        else:
            t.extractall(os.path.dirname(LZO_TAR))
    print(f'unpacked {os.path.relpath(LZO_ROOT, ROOT)} (sha256 ok)')
    return os.path.isdir(LZO_SRC)


def main():
    """Rebuilds liblzo.a from scratch from the LZO 2.10 sources (1 when they are
    not unpacked); reports compile errors.
    """
    if not os.path.isdir(LZO_SRC) and not unpack_sources():
        return 1

    os.makedirs(LIBDIR, exist_ok=True)
    if os.path.isdir(OBJDIR):
        for f in os.listdir(OBJDIR):
            os.remove(os.path.join(OBJDIR, f))
    os.makedirs(OBJDIR, exist_ok=True)

    file_list = sorted(f for f in os.listdir(LZO_SRC) if f.endswith('.c'))
    objs, errors_found = [], []
    for name in file_list:
        o = os.path.join(OBJDIR, os.path.splitext(name)[0] + '.o')
        error = compile_all(os.path.join(LZO_SRC, name), o)
        if error is None:
            objs.append(o)
        else:
            errors_found.append((name, error))

    if os.path.exists(ARCHIVE):
        os.remove(ARCHIVE)
    subprocess.run([tool_name('emar'), 'rcs', ARCHIVE] + objs,
                   capture_output=True, text=True)

    print(f'\nliblzo.a: objects {len(objs)} / {len(file_list)} (LZO 2.10, vendor)')
    if errors_found:
        print(f'\nNOT BUILT ({len(errors_found)}):')
        for f, e in errors_found:
            print(f'  {f}\n      {e}')

    size = os.path.getsize(ARCHIVE) if os.path.exists(ARCHIVE) else 0
    print(f'\nwritten: {os.path.relpath(ARCHIVE, ROOT)} ({size // 1024} kB)')

    if errors_found:
        print('\nCHECK FAILED: not every file was built')
        return 1

    r = subprocess.run([tool_name('llvm-nm'), ARCHIVE],
                       capture_output=True, text=True)
    defined_symbols = set()
    for l in r.stdout.splitlines():
        cols = l.split()
        if len(cols) == 3 and cols[1] in ('T', 't', 'D', 'd'):
            defined_symbols.add(cols[2])

    missing = [s for s in REQUIRED if s not in defined_symbols]
    print('\ncheck of the symbols the game needs:')
    for s in REQUIRED:
        print(f'  {"OK     " if s in defined_symbols else "MISSING"} {s}')
    if missing:
        print('\nCHECK FAILED: definitions missing - are these not the 2.10 sources?')
        return 1

    print('\ncheck: built from scratch from the 2.10 sources, symbols defined')
    return 0


if __name__ == '__main__':
    sys.exit(main())
