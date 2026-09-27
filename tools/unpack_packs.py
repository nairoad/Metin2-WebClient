#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""unpack_packs.py - extracts the scripts and data from the client packs.

Builds `tools/unpack_pack.cpp` for wasm and runs it under node with
access to the real files (`-sNODERAWFS=1`). The reader is the client's own
code - see the note in that file.

WHICH PACKS AND WHY THESE
=========================
  root        game scripts: `system.py`, `prototype.py`, all the interface windows
  locale_en   language files: `locale/en/locale_game.txt` and window scripts

This is the MINIMUM needed for the client to get as far as creating the
window. The other packs are textures, models and maps - 1.4 GB in total,
which there is no point extracting to disk, because they are meant to be
loaded on demand.

WHAT HAPPENS TO THE RESULT
==========================
It goes to `build/port/scripts`, where `tools/rewrite_scripts.py` takes it
and converts it to Python 3. The split is deliberate: extracting and
rewriting are two different activities, so with a wrong result it is
immediately clear which one failed.
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
PACKS = workspace.PACKS
TARGET = os.path.join(PORT, 'scripts')

EMSDK = workspace.EMSDK

# Pack names - without extension. `CEterPack` adds `.eix` and `.epk` itself.
# `locale_pl` - the language of the user's server (`?deflang=pl`). Without
# this pack, choosing Polish ended in a crash on `locale/pl/ui/LoginWindow.py`.
TO_UNPACK = ('root', 'locale_en', 'locale_pl')


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
    raise SystemExit(name + ' not found - add emsdk to PATH')


def build(empp, result):
    """Builds the node unpacker (unpack_pack.cpp + the eterPack/eterBase/LZO/
    Crypto++ archives, compat/paths_web.cpp and the runtime.js pre-js) into
    `result`; False when it did not link.
    """
    r = subprocess.run(
        [empp, '-std=c++17', '-O1', '-w',
         '-I' + workspace.COMPAT,
         '-I' + os.path.join(PORT, 'stage'),
         '-I' + os.path.join(PORT, 'stage', 'eterBase'),
         '-I' + os.path.join(PORT, 'stage', 'eterPack'),
         '-I' + workspace.EXTERN_INCLUDE,
         os.path.join(ROOT, 'tools', 'unpack_pack.cpp'),
         # `win32_compat.cpp` (in libeterbase) calls M2W_ResolvePathCase,
         # which lives in paths_web.cpp since the Phase A split, and the EM_JS
         # code needs the `m2w` runtime - without both the unpacker linked
         # (undefined symbols allowed) and aborted on the first pack.
         os.path.join(workspace.COMPAT, 'paths_web.cpp'),
         '--pre-js', os.path.join(workspace.COMPAT, 'runtime.js'),
         os.path.join(PORT, 'lib', 'libeterpack.a'),
         os.path.join(PORT, 'lib', 'libeterbase.a'),
         os.path.join(PORT, 'lib', 'liblzo.a'),
         os.path.join(PORT, 'lib', 'libcryptopp.a'),
         '-sNODERAWFS=1', '-sALLOW_MEMORY_GROWTH=1', '-sEXIT_RUNTIME=1',
         '-sERROR_ON_UNDEFINED_SYMBOLS=0',
         '-o', result],
        capture_output=True, text=True)

    if not os.path.exists(result):
        print('unpack_pack.cpp DID NOT BUILD:')
        for l in r.stderr.splitlines():
            if 'error' in l.lower():
                print('  ' + l[:180])
        return False
    return True


def main():
    """Unpacks every client pack with the node tool into CEL and checks that the
    key files (system.py, prototype.py, locale_game.txt) came out.
    """
    if not os.path.isdir(PACKS):
        print('missing ' + os.path.relpath(PACKS, ROOT))
        return 1

    node = shutil.which('node')
    if not node:
        print('node not found')
        return 1

    empp = tool_name('em++')
    js_tool = os.path.join(PORT, 'obj', 'unpack_pack.js')
    os.makedirs(os.path.dirname(js_tool), exist_ok=True)

    if not build(empp, js_tool):
        return 1

    os.makedirs(TARGET, exist_ok=True)

    all_items = 0
    for name in TO_UNPACK:
        # `CEterPack::CreateIndexFile` calls `fopen` on the BARE file name,
        # without a path - so the tool has to start in the packs directory.
        # That is not our decision, just the behaviour of the client's reader.
        r = subprocess.run([node, js_tool, '.', name, TARGET],
                           cwd=PACKS, capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode != 0:
            print('  pack "%s" FAILED' % name)
            return 1
        for l in r.stdout.splitlines():
            # `extracted N | ...` (`wyjete` earlier)
            if l.startswith(('extracted ', 'wyjete ')):
                all_items += int(l.split()[1])

    print('\nfiles extracted in total: %d' % all_items)
    print('in: ' + os.path.relpath(TARGET, ROOT))

    # CHECK BY NAME. These two files are the entry point of the whole game -
    # without them the rest does not matter, so I check them directly,
    # instead of trusting that "a lot came out".
    for required_one in ('system.py', 'prototype.py',
                     os.path.join('locale', 'en', 'locale_game.txt')):
        if not os.path.exists(os.path.join(TARGET, required_one)):
            print('CHECK FAILED - missing ' + required_one)
            return 1
    print('check: system.py, prototype.py and locale_game.txt are in place')
    return 0


if __name__ == '__main__':
    sys.exit(main())
