#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Gate on the LINKED client, where the object-level baseline is blind.

  python tools/gates/check_link.py [--map identifiers.tsv --done B1,B3] [--record | --compare]

Why: tools/link_client.py links with -sERROR_ON_UNDEFINED_SYMBOLS=0, so a
symbol nobody defines any more becomes a JS stub (`missing function: X`)
and the game aborts at run time instead of the link failing. Phase B1
(reviewer review) produced exactly that: build/port/lib/libeterbase.a and
libeterpack.a are built by separate scripts, were not rebuilt, and kept
defining `tmp4_random`/`tmp4_srandom` while gamelib already called
`m2w_random`. Three green gates missed it because none of them looked at
the linked client.wasm.

Checks (exit 1 on any failure):
  1. IMPORT section of build/port/client.wasm, for the steps listed in
     --done (the ones already applied): no import may be an OLD name of
     those steps, nor a NEW name of a C/C++ symbol (kinds other than EM_JS /
     JS global / env var) - those must be defined, never imported. Names of
     steps not yet applied (EM_JS still under old names) are not judged.
  2. `missing function:` stubs in build/port/client.js: with --record the
     list is saved to tools/gates/link_baseline.txt; with --compare any
     new stub (not in the baseline) fails. Stubs that disappear are fine.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import PORT, wasm_sections, read_text  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
LINK_BASELINE = os.path.join(HERE, 'link_baseline.txt')


def import_names(wasm_path):
    """Names of all imports of the wasm file (the field of each IMPORT entry) -
    the symbols the linker left to JavaScript.
    """
    with open(wasm_path, 'rb') as f:
        data = f.read()
    from common import _leb
    for name, payload in wasm_sections(data):
        if name != 'IMPORT':
            continue
        count, i = _leb(payload, 0)
        out = []
        for _ in range(count):
            n, i = _leb(payload, i); i += n                     # module
            n, i = _leb(payload, i); field = payload[i:i + n]; i += n
            out.append(field.decode('utf-8', 'replace'))
            kind = payload[i]; i += 1
            if kind == 0:
                _v, i = _leb(payload, i)
            elif kind == 1:
                i += 1; flags = payload[i]; i += 1; _v, i = _leb(payload, i)
                if flags & 1: _v, i = _leb(payload, i)
            elif kind == 2:
                flags = payload[i]; i += 1; _v, i = _leb(payload, i)
                if flags & 1: _v, i = _leb(payload, i)
            elif kind == 3:
                i += 2
            elif kind == 4:
                i += 1; _v, i = _leb(payload, i)
        return out
    return []


def export_names(wasm_path):
    """Names in the EXPORT section (functions, globals, tables, memories)."""
    from common import _leb
    with open(wasm_path, 'rb') as f:
        data = f.read()
    for name, payload in wasm_sections(data):
        if name != 'EXPORT':
            continue
        count, i = _leb(payload, 0)
        out = []
        for _ in range(count):
            n, i = _leb(payload, i); out.append(payload[i:i + n].decode('utf-8', 'replace')); i += n
            i += 1                                   # kind
            _v, i = _leb(payload, i)                 # index
        return out
    return []


def load_table(path):
    """Rows (old, new, step, kind) of a tab-separated table; blank and `#` lines
    skipped.
    """
    rows = []
    for line in read_text(path).splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        parts = line.split('\t')
        rows.append((parts[0], parts[1], parts[2] if len(parts) > 2 else '', parts[3] if len(parts) > 3 else ''))
    return rows


def main(argv):
    """Checks the linked client: imports of old table names (`--map`), stubs in
    client.js and wasm exports against the baseline (`--compare`, `--record`).
    1 on any failure.
    """
    wasm = os.path.join(PORT, 'client.wasm')
    glue = os.path.join(PORT, 'client.js')
    imports = import_names(wasm)
    failures = 0
    if '--done' in argv and '--map' not in argv:
        raise SystemExit('--done only makes sense with --map')
    if '--map' in argv:
        rows = load_table(argv[argv.index('--map') + 1])
        if '--done' not in argv:
            raise SystemExit('--map needs --done <steps already applied>, e.g. --done B1')
        done = set(argv[argv.index('--done') + 1].split(','))
        rows = [r for r in rows if r[2] in done]
        old = {r[0] for r in rows}
        c_new = {r[1] for r in rows if not r[3].startswith(('EM_JS', 'JS global', 'env var'))}
        def mentions(n, ident):
            """True when import `n` is identifier `ident` itself or contains it
            Itanium-mangled (its decimal length followed by the name).
            """
            # plain C symbol, or Itanium-mangled C++ where the identifier is
            # preceded by its decimal length (`_Z10m2w_randomv`); reviewer:
            # the first version only looked for `_ident` and missed exactly
            # the three mangled symbols that were the B1 defect
            return n == ident or (str(len(ident)) + ident) in n
        bad_old = sorted(n for n in imports if any(mentions(n, o) for o in old))
        bad_new = sorted(n for n in imports if any(mentions(n, c) for c in c_new))
        for n in bad_old:
            print('FAIL import of OLD table name: ' + n)
        for n in bad_new:
            print('FAIL import of a C/C++ symbol that must be defined: ' + n)
        failures += len(bad_old) + len(bad_new)
        print('imports: %d, judged steps: %s, old-name imports: %d, C-symbol imports: %d' % (len(imports), ','.join(sorted(done)), len(bad_old), len(bad_new)))
    stubs = sorted(set(re.findall(r"missing function: ([A-Za-z_][A-Za-z0-9_]*)", read_text(glue))))
    exports = sorted(set(export_names(wasm)))
    print('stubs in client.js: %d, wasm exports: %d' % (len(stubs), len(exports)))
    # 3. exports of the linked wasm = the names the page/JS may call; a rewrite
    #    (phase C) must not drop or add one unnoticed - exports_baseline.txt
    exports_path = LINK_BASELINE.replace('link_baseline', 'exports_baseline')
    if '--record' in argv:
        with open(LINK_BASELINE, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(stubs) + '\n')
        with open(exports_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(exports) + '\n')
        print('recorded ' + LINK_BASELINE + ' and exports_baseline.txt')
    elif '--compare' in argv:
        base = set(read_text(LINK_BASELINE).split()) if os.path.exists(LINK_BASELINE) else set()
        new = [s for s in stubs if s not in base]
        gone = [s for s in base if s not in stubs]
        for s in new:
            print('FAIL new stub (symbol nobody defines): ' + s)
        print('stubs vs baseline: new %d, gone %d' % (len(new), len(gone)))
        failures += len(new)
        if os.path.exists(exports_path):
            base_e = set(read_text(exports_path).split())
            ren = {r[0]: r[1] for r in (load_table(argv[argv.index('--map') + 1]) if '--map' in argv else [])}
            base_e = {ren.get(e, e) for e in base_e}                 # declared renames are not failures
            missing, added = sorted(base_e - set(exports)), sorted(set(exports) - base_e)
            for e in missing:
                print('FAIL export gone: ' + e)
            for e in added:
                print('FAIL export added: ' + e)
            print('exports vs baseline: gone %d, added %d' % (len(missing), len(added)))
            failures += len(missing) + len(added)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
