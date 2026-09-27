#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every `/// Declared only` label in compat/*.h must be true.

  python tools/gates/check_declared.py [--expect-zero]

For each `///` line containing "Declared only", the next non-`///` line is
the declaration; its function name is looked up in every .h/.c/.cpp file
under compat. A name followed by a parameter list and then `{`
(a body, inline or not) means the label lies. Once the label was on six
functions with bodies in platform_none.cpp (ToolHelp32 5,
DirectInput8Create) because the header note it was translated from was
stale; this is the check that would have caught it.

Positive control: `--probe NAME` prints where NAME has a body, so the
search itself can be shown to find bodies (e.g. `--probe Process32First`).
"""
import glob
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COMPAT = os.path.join(ROOT, 'compat')


def load_sources():
    """All compat sources as {path: text}."""
    out = {}
    for f in glob.glob(os.path.join(COMPAT, '**', '*.*'), recursive=True):
        if f.endswith(('.cpp', '.h', '.c')):
            out[f] = io.open(f, encoding='utf-8', errors='replace').read()
    return out


def bodies(name, sources):
    """`file:line` of every definition (parameter list followed by `{`) of `name`."""
    hits = []
    for f, t in sources.items():
        for m in re.finditer(r'\b%s\s*\(' % re.escape(name), t):
            pre = t[t.rfind('\n', 0, m.start()) + 1:m.start()]
            if '//' in pre or re.match(r'\s*(if|return|while|else)\b', pre):
                continue
            depth, j = 0, t.find('(', m.start())
            while j < len(t):
                depth += t[j] == '('
                depth -= t[j] == ')'
                j += 1
                if depth == 0:
                    break
            if re.match(r'\s*(const\s*)?(override\s*)?\{', t[j:j + 80]):
                hits.append('%s:%d' % (os.path.relpath(f, ROOT), t.count('\n', 0, m.start()) + 1))
    return hits


def labels(sources):
    """(header, line, name) for every `/// Declared only` label."""
    out = []
    for f, t in sorted(sources.items()):
        if not f.endswith('.h'):
            continue
        lines = t.split('\n')
        for i, line in enumerate(lines):
            if '///' not in line or 'Declared only' not in line:
                continue
            for k in range(i + 1, min(i + 8, len(lines))):
                if lines[k].strip().startswith('///'):
                    continue
                names = re.findall(r'([A-Za-z_]\w*)\s*\(', lines[k])
                names = [n for n in names if n not in ('WINAPI', '__stdcall', 'APIENTRY')]
                if names:
                    out.append((os.path.relpath(f, ROOT), k + 1, names[0]))
                break
    return out


def main(argv):
    """Report labels that sit on functions with a body; exit 1 with --expect-zero."""
    sources = load_sources()
    if '--probe' in argv:
        name = argv[argv.index('--probe') + 1]
        print(name, '->', ', '.join(bodies(name, sources)) or 'no body')
        return 0
    found = labels(sources)
    bad = 0
    for path, line, name in found:
        b = bodies(name, sources)
        if b:
            bad += 1
            print('LABEL LIES  %s:%d %s has a body at %s' % (path, line, name, ', '.join(b)))
    print('declared-only labels: %d, on functions with a body: %d' % (len(found), bad))
    return 1 if bad and '--expect-zero' in argv else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
