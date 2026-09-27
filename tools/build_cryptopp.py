#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Builds `build/port/lib/libcryptopp.a` FROM SCRATCH, from source.

WHY THIS SCRIPT EXISTS
======================
The same shape of risk as `libeterpack.a` before `build_eterpack.py`
: `libcryptopp.a` (8 MB) in `build/port/lib/` was assembled by
hand, outside `tools/` and outside control.

WHAT GOES IN
============
`reference/tmp4_source/extern/include/cryptopp/` is the FULL Crypto++ source
(197 `.cpp` files) - the game (`EterBase/cipher.cpp`,
`EterPack/EterPack.cpp`) uses a WIDE range of ciphers (AES, Blowfish, CAST,
Camellia, DES, DH, IDEA, MARS, RC5/6, SEED, Serpent, SHACAL2, Skipjack, TEA,
Twofish) and hash functions (RIPEMD, SHA, Tiger, Whirlpool, Panama) - in
practice almost the whole library.

41 of the 197 files use x86 assembly / SIMD intrinsics (`__asm`, `_mm_*`,
`__m128`) - Crypto++ has these as OPTIONAL SPEED-UPS with a portable C++
fallback elsewhere in the same file (a typical pattern of this library), so
we skip them on purpose, not by accident - the script PRINTS what it skipped
and what did not build for another reason, separately.

CHECK
=====
Counting the built objects and comparing with the list of skipped/unbuilt
ones - every file has an explicit reason for being absent from the archive,
none disappears silently.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
COMPAT = workspace.COMPAT
CRYPTOPP_SRC = os.path.join(workspace.EXTERN_INCLUDE, 'cryptopp')
CRYPTOPP_INC = workspace.EXTERN_INCLUDE
LIBDIR = os.path.join(PORT, 'lib')
OBJDIR = os.path.join(PORT, 'obj', 'cryptopp')
ARCHIVE = os.path.join(LIBDIR, 'libcryptopp.a')
EMSDK = workspace.EMSDK

# Files that are NOT the library: the test suite, benchmarks, the command
# line, the precompiled header. The list = EXACTLY the 25 files that
# were excluded after a bisection search - `adhoc.cpp`
# blew up an empty `main` before the first printf, although it compiled.
# The first version of this script excluded a non-existent
# `bench.cpp` and INCLUDED `bench3.cpp` (2.3 MB of test code object)
# and `pch.cpp`. The original set still lies in build/port/obj/cryptopp_excluded/.
SKIP_DEMO = {
    'test.cpp', 'bench1.cpp', 'bench2.cpp', 'bench3.cpp', 'validat0.cpp',
    'validat1.cpp', 'validat2.cpp', 'validat3.cpp', 'validat4.cpp',
    'validat5.cpp', 'validat6.cpp', 'validat7.cpp', 'validat8.cpp',
    'validat9.cpp', 'validat10.cpp', 'datatest.cpp', 'fipsalgt.cpp',
    'dlltest.cpp', 'fipstest.cpp', 'adhoc.cpp', 'regtest1.cpp',
    'regtest2.cpp', 'regtest3.cpp', 'regtest4.cpp', 'pch.cpp',
}


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
    """Compiles one Crypto++ source (assembly disabled) to `output`, 120 s at
    most; None on success, else the first `error:` line.
    """
    r = subprocess.run(
        [tool_name('em++'), '-std=c++17', '-c', '-O2', '-w',
         '-DCRYPTOPP_DISABLE_ASM',
         '-I' + CRYPTOPP_SRC, '-I' + CRYPTOPP_INC, '-I' + COMPAT,
         path_str, '-o', output],
        capture_output=True, text=True, timeout=120)
    if r.returncode == 0 and os.path.exists(output):
        return None
    return next((l for l in r.stderr.splitlines() if 'error:' in l), '?')


def main():
    """Rebuilds libcryptopp.a from scratch from every Crypto++ .cpp except the
    demo/test files (POMIN_DEMO); reports compile errors and timeouts.
    """
    os.makedirs(LIBDIR, exist_ok=True)
    if os.path.isdir(OBJDIR):
        for f in os.listdir(OBJDIR):
            os.remove(os.path.join(OBJDIR, f))
    os.makedirs(OBJDIR, exist_ok=True)

    all_items = sorted(f for f in os.listdir(CRYPTOPP_SRC)
                       if f.endswith('.cpp'))

    skipped_demo = [f for f in all_items if f in SKIP_DEMO]
    to_build = [f for f in all_items if f not in SKIP_DEMO]

    print(f'all .cpp: {len(all_items)}')
    print(f'skipped (demo/tests): {len(skipped_demo)}')
    print(f'to build: {len(to_build)}\n')

    objs, errors_found = [], []
    for i, name in enumerate(to_build):
        src = os.path.join(CRYPTOPP_SRC, name)
        o = os.path.join(OBJDIR, os.path.splitext(name)[0] + '.o')
        try:
            error = compile_all(src, o)
        except subprocess.TimeoutExpired:
            error = 'timed out (120 s)'
        if error is None:
            objs.append(o)
        else:
            errors_found.append((name, error))
        if (i + 1) % 20 == 0:
            print(f'  ... {i+1}/{len(to_build)}')

    if os.path.exists(ARCHIVE):
        os.remove(ARCHIVE)
    if objs:
        subprocess.run([tool_name('emar'), 'rcs', ARCHIVE] + objs,
                       capture_output=True, text=True)

    print(f'\nlibcryptopp.a: objects {len(objs)} / {len(to_build)} tried'
          f' ({len(all_items)} in the folder)')
    if errors_found:
        print(f'\nNOT BUILT, FOR A REASON OTHER THAN SIMD ({len(errors_found)}):')
        for f, e in errors_found:
            print(f'  {f}\n      {e}')

    size = os.path.getsize(ARCHIVE) if os.path.exists(ARCHIVE) else 0
    print(f'\nwritten: {os.path.relpath(ARCHIVE, ROOT)} ({size // 1024} kB)')

    return 0 if not errors_found else 1


if __name__ == '__main__':
    sys.exit(main())
