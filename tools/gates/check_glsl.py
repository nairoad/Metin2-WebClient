#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shader-composer gate for Phase C groups 6-7:
compat/tests/glsl_dump.cpp prints the GLSL that compat/d3d8_fixedfunc.cpp
generates for 1141 pipeline keys; this compares it with the recorded dump.

  python tools/gates/check_glsl.py --record
  python tools/gates/check_glsl.py --compare [--strict]

The baseline is tools/gates/glsl_baseline.txt.gz (1.9 MB of text, ~40 KB
gzipped, deterministic: mtime 0). `--compare` first diffs the raw text;
when it differs, the text is normalised (every GLSL
identifier -> ID, whitespace collapsed) and the run passes only when the
normalised programs are identical for every key AND the old->new identifier
map is a bijection (no SPLIT, no MERGE) and no name is renamed onto another
name of the old text (a swap of roles) - a rewrite may rename the GLSL
variables (`kolorWierzch` -> `vertexColor`), not restructure a program;
the key hash in every header must match too.
`--strict` demands the raw text unchanged. Every key that differs is
printed with its first differing line."""
import difflib
import gzip
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, PORT, tool  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, 'glsl_baseline.txt.gz')
IDENT = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')
KEY = re.compile(r'^=== key (\d+): (.*) hash=([0-9a-f]+) ===$', re.M)


def dump():
    """Builds glsl_dump.cpp with the fixed-function composer and returns every
    generated GLSL text (exits on a build or run failure).
    """
    empp, node = tool('em++'), tool('node')
    js = os.path.join(PORT, 'glsl_dump.js')
    cmd = [empp, '-std=c++17', '-O1', '-w', '-Icompat', 'compat/tests/glsl_dump.cpp',
           'compat/d3d8_fixedfunc.cpp', 'compat/win32_compat.cpp', '-o', js]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print('BUILD FAILED\n' + r.stderr[-1500:])
        sys.exit(1)
    r = subprocess.run([node, js], cwd=PORT, capture_output=True, text=True, encoding='utf-8')
    if r.returncode != 0:
        print('DUMP FAILED\n' + r.stderr[-1500:])
        sys.exit(1)
    return r.stdout


def split(text):
    """{key name: (hash, program text)} in dump order."""
    parts = KEY.split(text)
    out = {}
    for i in range(1, len(parts) - 3, 4):
        out[parts[i + 1]] = (parts[i + 2], parts[i + 3])
    return out


def norm(text):
    """`text` with identifiers replaced by `ID` and whitespace collapsed."""
    return ' '.join(IDENT.sub('ID', text).split())


def main(argv):
    """`--record` stores the GLSL dump; `--compare` checks it is identical, or -
    unless `--strict` - equal after mapping renamed identifiers. 1 on a
    difference.
    """
    cur = dump()
    if '--record' in argv:
        with gzip.GzipFile(BASELINE, 'wb', mtime=0) as f:
            f.write(cur.encode('utf-8'))
        print('recorded %d keys, %d bytes of text -> %s' % (len(split(cur)), len(cur), BASELINE))
        return 0
    if '--compare' not in argv:
        print(__doc__)
        return 2
    with gzip.open(BASELINE, 'rb') as f:
        base = f.read().decode('utf-8')
    if cur == base:
        print('glsl vs baseline: identical text, %d keys' % len(split(cur)))
        return 0
    if '--strict' in argv:
        print('FAIL glsl text differs (--strict)')
        return 1
    a, b = split(base), split(cur)
    fails = 0
    o2n, n2o = {}, {}
    if list(a) != list(b):
        print('FAIL key set differs: %d vs %d' % (len(a), len(b)))
        return 1
    renamed = 0
    for name in a:
        ha, ta = a[name]
        hb, tb = b[name]
        if ha != hb:
            # the key hash is a function of the key, not of the program -
            # a rewrite of M2W_KeyHash changes it with identical GLSL
            # (reviewer, review: it slipped through)
            fails += 1
            if fails <= 10:
                print('FAIL %s: key hash %s -> %s' % (name, ha, hb))
        if ta == tb:
            continue
        renamed += 1
        na, nb = norm(ta), norm(tb)
        if na != nb:
            fails += 1
            if fails <= 10:
                da, db = na.split(' '), nb.split(' ')
                sm = difflib.SequenceMatcher(None, da, db, autojunk=False)
                for tag, i1, i2, j1, j2 in sm.get_opcodes():
                    if tag != 'equal':
                        print('FAIL %s: %s %s => %s' % (name, tag, ' '.join(da[max(0, i1 - 6):i2 + 6]),
                                                        ' '.join(db[max(0, j1 - 6):j2 + 6])))
                        break
            continue
        for x, y in zip(IDENT.findall(ta), IDENT.findall(tb)):
            o2n.setdefault(x, set()).add(y)
            n2o.setdefault(y, set()).add(x)
    for x, ys in sorted(o2n.items()):
        if len(ys) > 1:
            fails += 1
            print('FAIL SPLIT %s -> %s' % (x, sorted(ys)))
    for y, xs in sorted(n2o.items()):
        if len(xs) > 1:
            fails += 1
            print('FAIL MERGE %s -> %s' % (sorted(xs), y))
    # a rename ONTO a name that also existed before is a swap of roles
    # (aColor <-> aSpecular), which the bijection alone would pass as two
    # consistent renames (reviewer, review)
    for x, ys in sorted(o2n.items()):
        y = next(iter(ys))
        if y != x and y in o2n:
            fails += 1
            print('FAIL rename onto an existing name (swap?): %s -> %s' % (x, y))
    renames = sorted((x, next(iter(ys))) for x, ys in o2n.items() if next(iter(ys)) != x)
    print('glsl vs baseline: %d keys, %d with changed text, %d renamed identifiers, %d failures' %
          (len(a), renamed, len(renames), fails))
    if renames:
        print('  renames: ' + ', '.join('%s->%s' % r for r in renames[:40]) + (' ...' if len(renames) > 40 else ''))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
