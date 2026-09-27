#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""repair_gr2.py - the .gr2 files our Oodle1 does not unpack, saved with raw
sections by the real granny2.dll.

Our Oodle1 decoder (tools/oodle1.py == compat/gr2_oodle1.cpp) is faithful to
the algorithm description and agrees with the open reference
implementation, but ~1% of the corpus files break down in the middle of a
section (the description is incomplete). The client rejects such a
file: a missing building (c1-013-6tower.gr2 - the house by Octavio in
Jinno), missing animations (dance, sad, jumagap...).

This script walks every .gr2 in the corpus work directory, tries each with
our decoder and passes the failing ones through
`build/port/repair/repair_gr2.exe` (x86, `tools/gr2_repair/repair_gr2.cpp`,
`GrannyDecompressData` from the client's granny2.dll). The result lands in
`build/port/repaired_gr2/<path>` - `build_corpus.py` takes these files
instead of the originals. The list of checked files is remembered
(`repaired_gr2/checked.txt`), so later runs are fast.

Usage: python tools/repair_gr2.py [--fresh]
"""
import io
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
WORK_DIR = os.path.join(PORT, 'client_files')
TARGET = os.path.join(PORT, 'repaired_gr2')
EXE = os.path.join(PORT, 'repair', 'repair_gr2.exe')
DLL = os.path.join(workspace.CLIENT, 'granny2.dll')
CHECKED = os.path.join(TARGET, 'checked.txt')

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gr2
import oodle1


def falls(path_str):
    """True when our decoder cannot unpack one of the sections."""
    d = io.open(path_str, 'rb').read()
    p = gr2.GrannyFile(d)
    if p.error:
        return False          # not a .gr2 in our sense - left alone
    for i, s in enumerate(p.sections):
        if s.unpacked_size == 0 or s.compression == 0:
            continue
        if s.compression != 2:
            return False
        try:
            oodle1.unpack_section(p.raw_section(i), s.first16, s.first8, s.unpacked_size)
        except oodle1.Oodle1Error:
            return True
        except Exception:
            return True
    return False


def run_helper(args, wait_seconds=60):
    """Runs repair_gr2.exe, retrying while Windows refuses to start it: a freshly
    built executable is held by the antivirus scan (WinError 32) - measured in
    the clean-clone trials for more than 10 s, so it waits up to `wait_seconds`
    with growing pauses before giving up."""
    import time
    started, pause, announced = time.time(), 0.5, False
    while time.time() - started < wait_seconds:
        if not os.path.isfile(args[0]):
            break
        try:
            return subprocess.run(args, capture_output=True, text=True)
        except PermissionError:
            if not announced:
                print('    the helper is held (antivirus scan of a new program?) - waiting up to %d s'
                      % wait_seconds)
                announced = True
            time.sleep(pause)
            pause = min(pause * 1.5, 5.0)
        except FileNotFoundError:          # removed between the check and the start
            break
    # Measured in the clean-clone trials: the antivirus holds the new
    # executable for a while, and sometimes REMOVES it (only the .obj stays).
    gone = not os.path.isfile(args[0])
    sys.exit('%s cannot be started%s - %s. Allow %s (or the build directory) in the antivirus and run '
             'the step again (python webclient.py build --from gr2); without it ~1%% of the models '
             'stay missing (the step is optional: --skip gr2).'
             % (os.path.relpath(args[0], ROOT),
                ' (the file is gone)' if gone else '',
                'your antivirus removed the freshly built helper' if gone
                else 'it is still held, most likely by the antivirus scan of a new program',
                               os.path.relpath(os.path.dirname(args[0]), ROOT)))


def build_helper():
    """Builds repair_gr2.exe with tools/gr2_repair/build.bat in the MSVC x86
    environment of `[tools] vcvars32`; False when MSVC is missing or
    the build fails.
    """
    if not os.path.isfile(workspace.VCVARS32):
        print('missing %s - install MSVC Build Tools 2022 (x86) or set [tools] vcvars32' % workspace.VCVARS32)
        return False
    bat = os.path.join(ROOT, 'tools', 'gr2_repair', 'build.bat')
    r = subprocess.run(['cmd', '/c', bat], env=dict(os.environ, M2W_VCVARS32=workspace.for_batch(workspace.VCVARS32)),
                       capture_output=True, text=True, errors='replace')
    if r.returncode != 0 or not os.path.isfile(EXE):
        print((r.stdout + r.stderr)[-2000:])
        return False
    print('built ' + os.path.relpath(EXE, ROOT))
    return True


def main():
    """Re-saves the .gr2 files our Oodle1 decoder cannot unpack through the real
    granny2.dll (raw sections) into the repaired directory; files already
    checked are skipped unless `--fresh`.
    """
    if not os.path.isfile(EXE) and not build_helper():
        return 1
    if not os.path.isfile(DLL):
        print('granny2.dll missing in ' + workspace.CLIENT)
        return 1
    os.makedirs(TARGET, exist_ok=True)
    checked = set()
    # Polish flag names from earlier still work.
    sys.argv = [{'--od-nowa': '--fresh'}.get(a, a) for a in sys.argv]
    if '--fresh' not in sys.argv and os.path.isfile(CHECKED):
        checked = set(l.strip() for l in io.open(CHECKED, encoding='utf-8') if l.strip())
    all_items = []
    for root_node, _k, names in os.walk(WORK_DIR):
        for n in names:
            if n.lower().endswith('.gr2'):
                all_items.append(os.path.relpath(os.path.join(root_node, n), WORK_DIR).replace(os.sep, '/'))
    all_items.sort()
    new_ones = [w for w in all_items if w not in checked]
    print('.gr2 files: %d, to check: %d' % (len(all_items), len(new_ones)))
    repaired, errors_found = 0, 0
    with io.open(CHECKED, 'a', encoding='utf-8', newline='\n') as index_list:
        for i, w in enumerate(new_ones):
            z = os.path.join(WORK_DIR, w.replace('/', os.sep))
            if falls(z):
                target = os.path.join(TARGET, w.replace('/', os.sep))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                r = run_helper([EXE, DLL, z, target])
                if r.returncode == 0:
                    repaired += 1
                    print('  repaired: %s (%d -> %d B)' % (w, os.path.getsize(z), os.path.getsize(target)))
                else:
                    # not written to the checked list: the next run tries it
                    # again instead of skipping a failure
                    errors_found += 1
                    print('  FAILED: %s - %s' % (w, (r.stderr or r.stdout).strip()[:200]))
                    continue
            index_list.write(w + '\n')
            if (i + 1) % 500 == 0:
                print('  ... %d / %d' % (i + 1, len(new_ones)))
    how_many = sum(len(f) for _r, _d, f in os.walk(TARGET)) - 1
    print('repaired now: %d, errors: %d, files in repaired_gr2: %d' % (repaired, errors_found, how_many))
    return 1 if errors_found else 0


if __name__ == '__main__':
    sys.exit(main())
