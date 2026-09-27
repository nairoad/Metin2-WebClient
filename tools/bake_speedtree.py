# -*- coding: utf-8 -*-
"""Builds and runs the SpeedTree tree baker.

`tools/speedtree_bake/bake_spt.cpp` is a NATIVE program (x86, MSVC), because it
links `reference/tmp4_source/extern/library/SpeedTreeRT.lib` and loads
`reference/Client/SpeedTreeRT.dll` - an engine whose sources we do not have,
and whose output (tree meshes) we want 1:1 as in the client.

Steps:
  1. `cl.exe` from Build Tools 2022 (vcvars32) -> build/port/bake/bake_spt.exe
  2. for every directory with `.spt` in build/port/client_files (the user's
     packs unpacked by build_corpus.py --unpack-only): baking into
     build/port/baked/<the same path>/<name>.spt (the client gets the
     baked file UNDER THE .spt NAME - `LoadTree` recognises it by the magic
     TMP4SPT1). `tools/build_client_data.py` copies this directory into the client
     package (`copy_baked`), because in the package a file takes
     precedence over the streamed corpus - and the corpus carries the
     original `.spt`, which the client cannot compute.

Usage:  python tools/bake_speedtree.py [--build-only]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
SOURCE = os.path.join(ROOT, 'tools', 'speedtree_bake', 'bake_spt.cpp')
OUT_DIR = os.path.join(ROOT, 'build', 'port', 'bake')
EXE = os.path.join(OUT_DIR, 'bake_spt.exe')
HEADERS = workspace.EXTERN_INCLUDE
LIB = os.path.join(workspace.EXTERN_LIBRARY, 'SpeedTreeRT.lib')
DLL = os.path.join(workspace.CLIENT, 'SpeedTreeRT.dll')
VCVARS = workspace.VCVARS32   # [tools] vcvars32 / M2W_VCVARS32
SOURCE_FILES = workspace.GAME_FILES
BAKED = workspace.BAKED


def build():
    """Compiles the native SpeedTree baker with MSVC (vcvars32 + cl) against the
    SDK .lib and copies the DLL next to it; False when an input is missing or
    the build fails.
    """
    for p in (SOURCE, LIB, DLL, VCVARS):
        if not os.path.exists(p):
            print('MISSING: %s' % p)
            return False
    os.makedirs(OUT_DIR, exist_ok=True)
    bat = os.path.join(OUT_DIR, 'build.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        # every path goes between quotes into cmd - checked first
        q = workspace.for_batch
        f.write('call "%s" >nul\r\n' % q(VCVARS))
        f.write('cd /d "%s"\r\n' % q(OUT_DIR))
        f.write('cl /nologo /EHsc /O2 /I "%s" "%s" /link "%s" /OUT:"%s"\r\n' % (q(HEADERS), q(SOURCE), q(LIB), q(EXE)))
    r = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True, errors='replace')
    if r.returncode != 0 or not os.path.exists(EXE):
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        print('BUILD FAILED')
        return False
    import shutil
    shutil.copy(DLL, os.path.join(OUT_DIR, 'SpeedTreeRT.dll'))
    print('built: %s' % EXE)
    return True


def bake_all():
    """Runs the baker on every directory of the unpacked packs with .spt files,
    writing into the baked tree with the same layout; prints the totals, True
    when there were no errors. Refuses input that is already baked.
    """
    directories = set()
    already_baked = []
    for d, _, file_list in os.walk(SOURCE_FILES):
        for p in file_list:
            if p.lower().endswith('.spt'):
                directories.add(d)
                with open(os.path.join(d, p), 'rb') as f:
                    if f.read(8) == b'TMP4SPT1':
                        already_baked.append(os.path.join(d, p))
    if not directories:
        print('no .spt files in %s - run: python tools/build_corpus.py --unpack-only' % SOURCE_FILES)
        return False
    if already_baked:
        # An earlier version once copied baked trees INTO the unpacked files; baking a baked
        # file is not an error the baker reports, it is a wrong tree.
        print('%d .spt files in %s are already baked (e.g. %s) - extract the packs again:'
              % (len(already_baked), SOURCE_FILES, os.path.relpath(already_baked[0], SOURCE_FILES)))
        print('    python tools/build_corpus.py --fresh --unpack-only')
        return False
    ok_count = error_count = 0
    for d in sorted(directories):
        relative_one = os.path.relpath(d, SOURCE_FILES)
        target = os.path.join(BAKED, relative_one)
        os.makedirs(target, exist_ok=True)
        r = subprocess.run([EXE, '--dir', d, target], capture_output=True, text=True, errors='replace')
        # the baker prints `<file>: ERROR: ...` per failed tree and a closing
        # `baked N, errors M`
        for w in r.stdout.splitlines():
            if ': ERROR: ' in w or w.startswith('baked '):
                print('  %s: %s' % (relative_one, w))
        for w in r.stdout.splitlines():
            if w.startswith('baked '):
                parts = w.replace(',', '').split()
                ok_count += int(parts[1]); error_count += int(parts[3])
    print('baked in total: %d, errors: %d' % (ok_count, error_count))
    return error_count == 0


if __name__ == '__main__':
    if not build():
        sys.exit(1)
    if '--build-only' in sys.argv:
        sys.exit(0)
    sys.exit(0 if bake_all() else 1)
