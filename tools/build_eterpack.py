#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Builds `build/port/lib/libeterpack.a` FROM SCRATCH, from source.

WHY THIS SCRIPT EXISTS
======================
The archive had lain in `build/port/lib/` since 4 September, assembled by
hand, outside `tools/` and outside the repository. Nobody knew which headers
it was built with - and that cost the ITEM ICONS.

`CEterPackManager::isExist` checks a file through `_access`. In the current
compatibility layer `_access` is a macro for `M2W_Access`, which asks the
streamed corpus (`compat/paths_web.cpp`). But the old archive was built
BEFORE that macro existed: `llvm-nm` shows in `EterPackManager.o` the
undefined symbol `access` - the bare `access` of libc, which sees only the
wasm file system. File reading went through `fopen` (intercepted), so the
data loaded; only the EXISTENCE CHECK bypassed the corpus,
`CItemData::__SetIconImage` got "no such file" and the icon stayed empty -
although the file is in the corpus and the TGA reader decodes it.

The same shape of error as `libeterbase.a` before `build_eterbase.py`:
**what nobody builds does not exist for the measurement** - and here even
worse, because it existed in a version that silently bypassed our layer.

WHAT GOES IN
============
Everything from `stage/eterPack/` (`.cpp` and `md5.c`). The script PRINTS
what it did not build. Internal check: the archive must have `M2W_Access`
as an undefined symbol (i.e. `_access` went through the macro), and must not
have a bare `access`.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
STAGE = os.path.join(PORT, 'stage')
COMPAT = workspace.COMPAT
EXTERN = workspace.EXTERN_INCLUDE
LIBDIR = os.path.join(PORT, 'lib')
OBJDIR = os.path.join(PORT, 'obj', 'eterpack')
ARCHIVE = os.path.join(LIBDIR, 'libeterpack.a')
EMSDK = workspace.EMSDK


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


DIRECTORY = os.path.join(STAGE, 'eterPack')


def compile_all(path_str, output):
    """Compiles one C or C++ source of eterPack to `output`; None on success,
    else the first `error:` line.
    """
    compiler = 'emcc' if path_str.endswith('.c') else 'em++'
    std = ['-std=c11'] if path_str.endswith('.c') else ['-std=c++17']
    r = subprocess.run(
        [tool_name(compiler)] + std + ['-c', '-O1', '-w',
         '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
         '-I' + os.path.join(STAGE, 'eterBase'),
         '-I' + os.path.join(STAGE, 'eterPack'),
         path_str, '-o', output],
        capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(output):
        return None
    return next((l for l in r.stderr.splitlines() if 'error:' in l), '?')


def main():
    """Rebuilds libeterpack.a from scratch from every eterPack source; reports
    compile errors and checks the archive.
    """
    os.makedirs(LIBDIR, exist_ok=True)
    if os.path.isdir(OBJDIR):
        for f in os.listdir(OBJDIR):
            os.remove(os.path.join(OBJDIR, f))
    os.makedirs(OBJDIR, exist_ok=True)

    sources = [os.path.join(DIRECTORY, f) for f in sorted(os.listdir(DIRECTORY))
              if f.endswith('.cpp') or f.endswith('.c')]

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
    subprocess.run([tool_name('emar'), 'rcs', ARCHIVE] + objs,
                   capture_output=True, text=True)

    print(f'\nlibeterpack.a: objects {len(objs)}')
    for o in objs:
        print(f'  {os.path.basename(o)}')
    if errors_found:
        print(f'\nNOT BUILT ({len(errors_found)}):')
        for f, e in errors_found:
            print(f'  {f}\n      {e}')

    size = os.path.getsize(ARCHIVE) if os.path.exists(ARCHIVE) else 0
    print(f'\nwritten: {os.path.relpath(ARCHIVE, ROOT)} ({size // 1024} kB)')

    # CHECK: `isExist` must go through our layer, not through libc.
    r = subprocess.run([tool_name('llvm-nm'),
                        os.path.join(OBJDIR, 'EterPackManager.o')],
                       capture_output=True, text=True)
    symbol_list = r.stdout
    bare = any(l.strip().endswith(' access') for l in symbol_list.splitlines())
    ours = 'M2W_Access' in symbol_list
    if bare or not ours:
        print('\nCHECK FAILED: EterPackManager.o %s' %
              ('calls the bare access from libc' if bare else 'does not call M2W_Access'))
        return 1
    print('check: EterPackManager.o checks files through M2W_Access')
    return 0


if __name__ == '__main__':
    sys.exit(main())
