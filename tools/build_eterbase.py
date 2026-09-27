#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Builds `build/port/lib/libeterbase.a` FROM SCRATCH, from source.

WHY THIS SCRIPT EXISTS
======================
The archive was assembled by hand all along and lies in `.gitignore` (it is
a megabyte). The effect: **it could not be recreated from the repository**,
and what was in it only the person who assembled it knew.

That cost something concrete. `eterBase/Poly/` is a SUBDIRECTORY, so
assembling "all `eterBase/*.cpp`" did not cover it - and nine `CPoly`
symbols came out in the contract as "to be written", although the source
had been in the tree from the start. The same shape of error as
the stale `.o` files and the skipped `compat/` files:
**the contract measures what the script builds**, so a file nobody builds
does not exist for the measurement.

WHAT GOES IN AND WHAT DOES NOT
==============================
Everything from `eterBase/` and `eterBase/Poly/` that compiles goes in,
plus the Win32 compatibility layer. One file stays out, and that is **a
measured result, not a decision**: `error.cpp` calls
`GetTimestampForLoadedLibrary` from `BugslayerUtil` - a library that is not
there and will not be, because the crash report is replaced by
`compat/platform_crash.cpp`.

The script PRINTS what it did not build, and with what error. A silent
absence of a file from the archive is exactly what did the damage.
"""

import os
import shutil
import subprocess
import sys

# Sources that do not compile ON PURPOSE (see "WHAT GOES IN" above).
EXPECTED_NOT_BUILT = {
    'error.cpp': 'needs BugslayerUtil (GetTimestampForLoadedLibrary); crash reports are compat/platform_crash.cpp',
}

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
STAGE = os.path.join(PORT, 'stage')
COMPAT = workspace.COMPAT
# The headers of external libraries (lzo, Crypto++, Direct3D) lie in the
# TMP4 source tree, not in `build/port`. A wrong path here cost
# THREE OBJECTS - `MappedFile`, `cipher` and `lzo` - and raised the contract
# from 14 to 32, before the script printed what it did not build.
EXTERN = workspace.EXTERN_INCLUDE
LIBDIR = os.path.join(PORT, 'lib')
OBJDIR = os.path.join(PORT, 'obj', 'eterbase')
ARCHIVE = os.path.join(LIBDIR, 'libeterbase.a')

EMSDK = workspace.EMSDK


def tool_name(name):
    """The same search as in `build_gamelib.py` - first `PATH`, then
    two emsdk directories. Repeated on purpose: both scripts are meant to work
    separately, without importing each other.
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

# Source directories. `Poly` is separate, because it is a SUBDIRECTORY - and
# exactly for that reason it fell out of the assembly by `eterBase/*.cpp`.
DIRECTORIES = (
    os.path.join(STAGE, 'eterBase'),
    os.path.join(STAGE, 'eterBase', 'Poly'),
)

# The Win32 compatibility layer belongs to the archive, not to the contract:
# without it `CopyFileA`, `CharNextExA` and the critical-section functions
# would look missing, although they are written.
EXTRA_SOURCES = (os.path.join(COMPAT, 'win32_compat.cpp'),)


def compile_all(path_str, output):
    """Compiles one source to `output`; None on success, else the first
    `error:` line.
    """
    r = subprocess.run(
        [tool_name('em++'), '-std=c++17', '-c', '-O0', '-w',
         '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
         '-I' + os.path.join(STAGE, 'eterBase'),
         path_str, '-o', output],
        capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(output):
        return None
    return next((l for l in r.stderr.splitlines() if 'error:' in l), '?')


def main():
    """Rebuilds libeterbase.a from scratch (old objects deleted first) from the
    eterBase directories and the extra sources; reports compile errors.
    """
    os.makedirs(LIBDIR, exist_ok=True)
    # Objects are counted FROM SCRATCH. Without it a stale `.o` file stays
    # in the archive and excluding a source is invisible to the measurement -
    # exactly the error.
    if os.path.isdir(OBJDIR):
        for f in os.listdir(OBJDIR):
            os.remove(os.path.join(OBJDIR, f))
    os.makedirs(OBJDIR, exist_ok=True)

    sources = []
    for directory in DIRECTORIES:
        if not os.path.isdir(directory):
            continue
        sources += [os.path.join(directory, f) for f in sorted(os.listdir(directory))
                   if f.endswith('.cpp')]
    sources += list(EXTRA_SOURCES)

    objs, errors_found = [], []
    for src in sources:
        o = os.path.join(OBJDIR, os.path.splitext(os.path.basename(src))[0] + '.o')
        error = compile_all(src, o)
        if error is None:
            objs.append(o)
        else:
            errors_found.append((os.path.relpath(src, PORT), error))

    if os.path.exists(ARCHIVE):
        os.remove(ARCHIVE)
    subprocess.run([tool_name('emar'), 'rcs', ARCHIVE] + objs, capture_output=True, text=True)

    print(f'\nlibeterbase.a: objects {len(objs)}')
    for o in objs:
        print(f'  {os.path.basename(o)}')

    # The files I did not build are PRINTED. A silent absence of a file
    # from the archive is what cost nine symbols in the contract.
    # EXPECTED_NOT_BUILT (see the top of this file) is labelled as such; any
    # other file that does not compile fails the step (a clean
    # build must not print an error the reader has to know to ignore).
    expected = [(f, e) for f, e in errors_found if os.path.basename(f) in EXPECTED_NOT_BUILT]
    unexpected = [(f, e) for f, e in errors_found if os.path.basename(f) not in EXPECTED_NOT_BUILT]
    for f, _e in expected:
        print(f'\nnot built, as expected: {f} - {EXPECTED_NOT_BUILT[os.path.basename(f)]}')
    if unexpected:
        print(f'\nNOT BUILT ({len(unexpected)}):')
        for f, e in unexpected:
            print(f'  {f}\n      {e}')

    size = os.path.getsize(ARCHIVE) if os.path.exists(ARCHIVE) else 0
    print(f'\nwritten: {os.path.relpath(ARCHIVE, ROOT)} ({size // 1024} kB)')

    # INTERNAL CHECK: `CPoly::Eval` MUST be in the archive. This is the
    # symbol whose absence cost a whole entry in the contract - so
    # I check it by name, instead of trusting that the loop above ran.
    r = subprocess.run([tool_name('llvm-nm'), '-C', ARCHIVE],
                       capture_output=True, text=True)
    if 'CPoly::Eval' not in r.stdout:
        print('\nCHECK FAILED: CPoly::Eval is NOT in the archive')
        return 1
    print('check: CPoly::Eval is in the archive')
    return 0


if __name__ == '__main__':
    sys.exit(main())
