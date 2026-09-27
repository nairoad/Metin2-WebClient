#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Find old names anywhere in our layer (plan phase A/B/E gate).

Usage:
  python tools/gates/check_names.py --files            # old compat file base names (phase A)
  python tools/gates/check_names.py --prefix           # tmp4_ / Tmp4_ / TMP4_ identifiers
  python tools/gates/check_names.py --map names.tsv    # every `old` column of a TSV
  python tools/gates/check_names.py ... --expect-zero  # exit 1 on any hit (gate)
  python tools/gates/check_names.py ... --expect-all   # exit 1 if any pattern has NO hit
                                                        # (negative control before renaming)

Scans the whole repository except what is not ours (reference/, recovered/,
stage/, generated output) and except historical documents. Also scans the
generated build/port/client.js when present: EM_JS functions are called by
name inside JavaScript text, which the compiler never checks.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, PORT, iter_files, read_text, rel  # noqa: E402

# Old base names of compat files that must not come back; the list lives in
# the working repository only (tools/gates/private_scope.py).
try:
    from private_scope import OLD_FILE_BASENAMES, BARE_SKIP
except ImportError:
    OLD_FILE_BASENAMES, BARE_SKIP = [], ()


BARE_IN = ('.py', '.bat', '.cpp', '.h', '.md', '.html', '.js')


def load_ceiling(key):
    """`# ceiling <key> N` line in allow_names.txt, or None."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'allow_names.txt')
    if not os.path.exists(path):
        return None
    for line in read_text(path).splitlines():
        m = re.match(r'#\s*ceiling\s+(.+?)\s+(\d+)\s*$', line)
        if m and m.group(1).strip() == key:
            return int(m.group(2))
    return None


def load_allow(args):
    """Known, justified leftovers: `tools/gates/allow_names.txt`, one
    `file<TAB>pattern<TAB>reason` per line. A hit that matches an allowed
    (file, pattern) pair is reported but does not fail --expect-zero."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'allow_names.txt')
    allow = set()
    if os.path.exists(path):
        for line in read_text(path).splitlines():
            if line.strip() and not line.startswith('#'):
                f, pat = line.split('\t')[:2]
                allow.add((f.strip(), pat.strip()))
    return allow


def patterns_from_args(args):
    """The (label, regex) list for the chosen checks: `--files` old compat file
    names, `--prefix` identifiers containing tmp4/Tmp4/TMP4, `--map` old names
    from the table.
    """
    pats = []
    if '--files' in args:
        # In C++/docs the name appears with an extension; build scripts list
        # compat files as bare identifiers (`nazwa + '.cpp'`, reviewer P1), so
        # for .py/.bat the bare word counts too (see BARE_IN).
        # `name.(cpp|h|o)`, `name.*` (docs shorthand) and - in C++/markdown -
        # the bare word (reviewer, phase A review D1: 5 comment references
        # slipped through). The bare word is skipped for BARE_SKIP: names that
        # are also ordinary words in prose.
        pats += [(b, re.compile(r'\b' + re.escape(b) + r'(\.(cpp|h|o)\b|\.\*)')) for b in OLD_FILE_BASENAMES]
    if '--prefix' in args:
        # Anywhere in an identifier, not only as a prefix (reviewer, phase B
        # review P-4: `tmp4Chunks`, `g_ulTmp4Frames`, `CTmp4PythonFrame`);
        # no leading \b, so glue-file exports `_Tmp4_IME_*` and
        # `__em_js__tmp4_*` count too (P-3). The bare word "tmp4" in prose
        # and log prefixes ("tmp4 net:") has no identifier characters
        # attached and is not matched.
        pats += [('tmp4_', re.compile(r'\w*tmp4_\w+')), ('Tmp4_', re.compile(r'\w*Tmp4_\w+')),
                 ('TMP4_', re.compile(r'\w*TMP4_[A-Z0-9_]+')),
                 ('tmp4Camel', re.compile(r'\w*tmp4[A-Z]\w*')), ('Tmp4Camel', re.compile(r'\w+Tmp4[A-Z]\w*'))]
    if '--map' in args:
        tsv = args[args.index('--map') + 1]
        for line in read_text(tsv).splitlines():
            if not line.strip() or line.startswith('#'):
                continue
            old = line.split('\t')[0].strip()
            pats.append((old, re.compile(r'\b' + re.escape(old) + r'\b')))
    if not pats:
        raise SystemExit(__doc__)
    return pats


def main(argv):
    """Counts every pattern in the repository (and client.js), minus the allowed
    (file, label) pairs; prints per label and file. 1 with `--expect-zero` and
    hits.
    """
    pats = patterns_from_args(argv)
    files = list(iter_files())
    glue = os.path.join(PORT, 'client.js')
    if os.path.exists(glue):
        files.append(glue)
    hits = {}          # pattern label -> {file: count}
    allow = load_allow(argv)
    allowed_hits = 0
    allow_used = {}
    for path in files:
        text = read_text(path)
        bare = path.endswith(BARE_IN) and '--files' in argv
        for label, rx in pats:
            # `reference/tmp4_source` is the name of the TMP4 source directory,
            # not one of our identifiers - never counted
            n = len([m for m in rx.findall(text) if m != 'tmp4_source'])
            if bare and label in OLD_FILE_BASENAMES and label not in BARE_SKIP:
                n += len(re.findall(r'(?<![\w./])' + re.escape(label) + r'(?![\w.])', text))
            if n and (rel(path), label) in allow:
                allowed_hits += n
                allow_used[(rel(path), label)] = allow_used.get((rel(path), label), 0) + n
                continue
            if n:
                hits.setdefault(label, {})[rel(path)] = n
    total = 0
    for label, _rx in pats:
        per = hits.get(label, {})
        n = sum(per.values())
        total += n
        if n:
            print('%-28s %5d  in %d files: %s' % (label, n, len(per),
                  ', '.join('%s(%d)' % (f, c) for f, c in sorted(per.items())[:6]) + (' ...' if len(per) > 6 else '')))
    print('patterns: %d, with hits: %d, total hits: %d (allowed, not counted: %d), files scanned: %d' %
          (len(pats), len(hits), total, allowed_hits, len(files)))
    # The allow list is coarse - (file, pattern), not literal - so it is kept
    # honest two ways (reviewer, phase B4 review P-2): entries with no hit are
    # reported as dead, and the allowed-hit count must not grow above the
    # recorded ceiling in allow_names.txt (`# ceiling <pattern-set> N`).
    dead = [(f, pat) for (f, pat) in allow if (f, pat) not in allow_used and any(pat == lab for lab, _ in pats)]
    for f, pat in sorted(dead):
        print('DEAD allow entry: %s / %s (no hits)' % (f, pat))
    ceiling = load_ceiling(' '.join(sorted(lab for lab, _ in pats if not lab in OLD_FILE_BASENAMES)) or 'files')
    if ceiling is not None and allowed_hits > ceiling:
        print('FAIL allowed hits grew: %d > ceiling %d - a new leftover hides behind an old allow entry' % (allowed_hits, ceiling))
        return 1
    if '--expect-zero' in argv and total:
        return 1
    if '--expect-all' in argv:
        missing = [label for label, _ in pats if label not in hits]
        if missing:
            print('NO HITS for: ' + ', '.join(missing))
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
