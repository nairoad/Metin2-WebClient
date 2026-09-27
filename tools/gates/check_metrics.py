#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Readability metrics for compat (plan phase C gate).

  python tools/gates/check_metrics.py [--gate]

Reports, per file: line count, functions longer than 80 and 120 lines (with
names), EM_JS/EM_ASM blocks and how many have bodies longer than 3 lines.
With `--gate` the targets from the plan apply and the exit code is 1 when
any is missed:
  * no function > 120 lines, at most 10 > 80 lines in the whole layer,
  * no file > 1500 lines except win32_compat.h (SDK-emulating header),
  * no EM_JS/EM_ASM body > 3 lines (JS lives in runtime.js).

Function detection is regex + brace matching (no preprocessor), which is
exact enough for our own code; it is a counter, not a parser.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import COMPAT, read_text, rel  # noqa: E402

SIG = re.compile(r'^\s*(?!(if|for|while|switch|return|else|catch|EM_JS|EM_ASM)\b)'
                 r'[\w:<>\*&~,\s]+?\s\**&?[\w:~]+\([^;{}]*\)\s*(const)?\s*(override)?\s*\{?\s*$')
# Anywhere in a code line, not only at its start: `s_iStan = EM_ASM_INT({`
# has an assignment before it (four such multi-line blocks in
# gl_buffers.cpp/gl_device.cpp were invisible to this count until then).
EM = re.compile(r'^(?!\s*//).*?\bEM_(JS|ASM[A-Z_]*)\(')
SIZE_EXEMPT = ('win32_compat.h',)


def functions(lines):
    """(length in lines, signature) of every braced function definition found by
    the SIG pattern.
    """
    i, out = 0, []
    while i < len(lines):
        l = lines[i]
        if SIG.match(l) and (l.rstrip().endswith('{') or (i + 1 < len(lines) and lines[i + 1].strip() == '{')):
            j = i if l.rstrip().endswith('{') else i + 1
            d, k = 0, j
            while k < len(lines):
                d += lines[k].count('{') - lines[k].count('}')
                if d <= 0:
                    break
                k += 1
            out.append((k - i + 1, l.strip()[:70]))
            i = k + 1
            continue
        i += 1
    return out


def em_blocks(lines):
    """[(kind, body_lines)] for EM_JS / EM_ASM blocks."""
    out = []
    for i, l in enumerate(lines):
        m = EM.match(l)
        if not m:
            continue
        d, k = 0, i
        started = False
        while k < len(lines):
            d += lines[k].count('{') - lines[k].count('}')
            if lines[k].count('{'):
                started = True
            if started and d <= 0:
                break
            k += 1
        out.append((m.group(1), max(0, k - i - 1)))
    return out


def main(argv):
    """Per compat file: lines, functions over 80/120 lines, EM_JS/EM_ASM blocks;
    prints the top 15 and the gate totals (functions > 120, EM bodies > 3
    lines, files > 1500 lines except SIZE_EXEMPT).
    """
    rows, long80, long120, em_total, em_long, big_files = [], [], [], 0, 0, []
    for f in sorted(os.listdir(COMPAT)):
        if not f.endswith(('.cpp', '.h')):
            continue
        p = os.path.join(COMPAT, f)
        lines = read_text(p).split('\n')
        fs = functions(lines)
        ems = em_blocks(lines)
        l80 = [x for x in fs if x[0] > 80]
        l120 = [x for x in fs if x[0] > 120]
        long80 += [(n, f, s) for n, s in l80]
        long120 += [(n, f, s) for n, s in l120]
        em_total += len(ems)
        em_long += sum(1 for _k, n in ems if n > 3)
        if len(lines) > 1500 and f not in SIZE_EXEMPT:
            big_files.append((len(lines), f))
        rows.append((len(lines), len(l80), len(l120), len(ems), f))
    rows.sort(reverse=True)
    print(' lines  >80  >120  EM_*  file')
    for r in rows[:15]:
        print('%6d  %3d  %4d  %4d  %s' % r)
    print('functions > 80 lines: %d (limit 10); > 120: %d (limit 0)' % (len(long80), len(long120)))
    for n, f, s in sorted(long120, reverse=True):
        print('  %4d  %-24s %s' % (n, f, s))
    print('EM_JS/EM_ASM blocks: %d, with body > 3 lines: %d (limit 0)' % (em_total, em_long))
    print('files > 1500 lines (except %s): %s' % (', '.join(SIZE_EXEMPT), big_files or 'none'))
    if '--gate' in argv:
        return 1 if (long120 or len(long80) > 10 or em_long or big_files) else 0
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
