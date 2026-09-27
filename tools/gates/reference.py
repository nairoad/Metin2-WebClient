#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build docs/REFERENCE.md from the code (plan phase W) and gate on missing docs.

  python tools/gates/reference.py            # write docs/REFERENCE.md
  python tools/gates/reference.py --check    # list undocumented public symbols, exit 1 if any
  python tools/gates/reference.py --check --file stubs.cpp   # gate for ONE rewritten file (phase C)
  python tools/gates/reference.py --stats    # counts only

The document is generated, never edited by hand. Sources of truth:
  * each compat file's header comment (first comment block) -> file paragraph,
  * `///` comment lines directly above a public C++ symbol -> one line per
    symbol: EM_JS functions, `extern "C"` / non-static free functions at file
    scope, classes and structs,
  * `?param` reads (`get("name")` in EM_JS) -> URL parameter table,
  * `globalThis.<name>` / `window.<name>` assignments -> JS globals table,
  * `///` above each `m2w.x = function` in compat/runtime.js -> runtime.js table,
  * module docstrings of every tool (tools/**/*.py tracked by git) -> tools
    table, and each tool's function docstrings -> one table per tool
    (first paragraph of each; - before, top-level scripts and
    their module docstrings only).
`//` blocks are NOT treated as documentation: the gate is what forces every
public symbol to get a `///` line during the rewrite.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, COMPAT, TOOLS, UNPUBLISHED, read_text, rel  # noqa: E402

OUT = os.path.join(ROOT, 'docs', 'REFERENCE.md')
SYMBOL = re.compile(r'^(?:extern\s+"C"\s+)?(?!static\b|return\b|else\b|typedef\b|using\b|namespace\b|#)'
                    r'(?:[\w:<>\*&~]+\s+)+\**&?([A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const)?\s*\{?\s*$')
EMJS = re.compile(r'^EM_JS\(\s*([\w\*\s]+?)\s*,\s*(\w+)\s*,')
CLASS = re.compile(r'^(class|struct|enum(?:\s+class)?)\s+([A-Za-z_]\w*)\s*(?::[^{]*)?\{?\s*$')
PARAM = re.compile(r'\.get\(\s*["\']([A-Za-z_]+)["\']\s*\)')
GLOBAL = re.compile(r'\b(?:globalThis|window)\.([A-Za-z_]\w*)\s*=')


def header_comment(lines):
    """The first `//` comment block of a file as one line (SPDX and ruler lines
    dropped), at most 400 characters.
    """
    out = []
    for l in lines[:80]:
        s = l.strip()
        if s.startswith('//'):
            if 'SPDX-License-Identifier' in s:
                continue
            out.append(s.lstrip('/').strip())
        elif s == '' and not out:
            continue
        elif out:
            break
    return ' '.join(x for x in out if x and not set(x) <= set('=-'))[:400]


def code_part(line, in_block=False):
    """The line without string literals and comments, plus whether a `/* */`
    comment is still open at its end. Strings go first: `"ws://"` is not a
    comment. Block comments are tracked across lines: an unbalanced `(` in
    `/* see Reset( */` would otherwise start a fold that swallows every
    following declaration in the file (reviewer review)."""
    line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    out = ''
    i = 0
    while i < len(line):
        if in_block:
            j = line.find('*/', i)
            if j < 0:
                return out, True
            i, in_block = j + 2, False
        elif line.startswith('/*', i):
            in_block = True
            i += 2
        elif line.startswith('//', i):
            break
        else:
            out += line[i]
            i += 1
    return out, in_block


def join_continuations(lines):
    """Fold a declaration whose parameter list spans several lines into ONE
    line, in place of its first line; the continuation lines become empty so
    the line count and the `///` block above are kept. Without this
    `int M2W_ToUtf16(...,\n ...);` was invisible and codepages.h counted
    3 of 5 functions as "all documented"."""
    out, start, buf, depth = [], None, '', 0
    in_macro = False                     # previous line ended with `\\`
    in_block = False                     # inside a `/* */` comment
    for raw in lines:
        s = raw.strip()
        code, in_block = code_part(s, in_block)   # parentheses in comments/strings do not count
        was_macro, in_macro = in_macro, s.endswith('\\')
        if start is None:
            if was_macro or s.startswith(('//', '#')) or code.count('(') <= code.count(')'):
                out.append(raw)
                continue
            start, buf, depth = len(out), code.rstrip(), code.count('(') - code.count(')')
            out.append('')
            continue
        buf += ' ' + code.strip()
        depth += code.count('(') - code.count(')')
        out.append('')
        if depth <= 0:
            out[start] = buf
            start = None
    if start is not None:
        out[start] = buf
    return out


def scan_file(path):
    """Scans one compat file: its public symbols (EM_JS, free functions at file
    or `extern "C"`/named-namespace scope, classes) with their `///` text, the
    URL parameters read and the JS globals assigned.
    """
    text = read_text(path)
    lines = join_continuations(text.split('\n'))
    is_header = path.endswith('.h')
    symbols, params, globs = [], set(), set()
    depth = 0
    extern_level = None       # depth inside an `extern "C" {` block (reviewer P3)
    doc = []
    for i, raw in enumerate(lines):
        l = raw.rstrip()
        s = l.strip()
        # `extern "C" {` and `namespace name {` open a scope whose members
        # are still public symbols (reviewer: frame_stats.h had 2 of
        # 12 symbols counted); anonymous namespaces stay private
        if (s.startswith('extern "C"') or re.match(r'^namespace\s+\w+\s*\{?$', s)) and s.endswith('{'):
            extern_level = depth + 1
            depth += 1
            continue
        if extern_level is not None and depth < extern_level:
            extern_level = None
        at_top = depth == 0 or depth == extern_level
        if s.startswith('///'):
            doc.append(s[3:].strip())
        elif s == 'EMSCRIPTEN_KEEPALIVE':
            # an attribute on its own line between the `///` and the
            # function keeps the doc (ime_web.cpp: 4 exports read
            # as undocumented)
            continue
        elif at_top and s and not s.startswith('//'):
            m = EMJS.match(s) or (SYMBOL.match(s.rstrip(';')) if not s.startswith(('EM_ASM', 'if', 'for', 'while', ':')) else None)
            c = CLASS.match(s)
            # function-like only when `(` follows the name directly (C rule); with a
            # space it is an object-like constant, e.g. `#define X ((DWORD)-1)`
            # (20+ constants were listed as undocumented macros)
            mac = re.match(r'^#define\s+([A-Za-z_]\w*)\(', s)
            # in a header the DECLARATION (ending with `;`) is the documented
            # site; in a .cpp only definitions count (a `;` line there is a
            # forward declaration)
            is_decl = s.endswith(';')
            if m and 'EM_JS' in s:
                symbols.append(('EM_JS', m.group(2), ' '.join(doc)))
            elif m and '(' in s and (is_header or not is_decl) and not s.startswith(('#', '}')):
                symbols.append(('function', m.group(1), ' '.join(doc)))
            elif c:
                symbols.append((c.group(1), c.group(2), ' '.join(doc)))
            elif mac and is_header:
                symbols.append(('macro', mac.group(1), ' '.join(doc)))
            doc = []
        elif s and not s.startswith('//'):
            doc = []
        depth += l.count('{') - l.count('}')
    # parameters and globals are read from the ORIGINAL text: folding blanks
    # string literals, and `.get("cursor")` lives in one
    for p in PARAM.findall(text):
        params.add(p)
    for g in GLOBAL.findall(text):
        globs.add(g)
    return header_comment(lines), symbols, params, globs


def selftest():
    """`--selftest`: the folding cases the reviewer review named
    (multi-line declaration, `\\`-continued macro, unbalanced parenthesis
    inside a comment before a documented declaration) plus `//` inside a
    string literal, which cost network_web.cpp three symbols."""
    import tempfile
    src = """// SPDX-License-Identifier: GPL-2.0-or-later
// selftest.h - fixture.
#define D3DCOLOR_ARGB(a,r,g,b) \\
    ((D3DCOLOR)((((a)&0xff)<<24)|(((r)&0xff)<<16)| \\
    (((g)&0xff)<<8)|((b)&0xff)))
// see Reset( below - unbalanced on purpose
/// Two-line declaration.
int M2W_Folded(int a,
               int b);
/// One-line declaration.
void M2W_Plain(void);
/// An enum.
enum EM2wKind
{
    M2W_A = 1,
};
/// A string with `//` in it is not a comment.
EM_JS(void, m2w_url, (void), { console.log("ws://" + (1 ? "a" : "b")); });
/// After it.
int M2W_After(void);
/* see Reset( for details - block comment, unbalanced on purpose */
/* a block comment
   spanning lines, with ( inside
   and // too */
/// After the block comments.
int M2W_AfterBlock(int a /* inline ( */,
                   int b);
/// Exported, attribute on its own line.
EMSCRIPTEN_KEEPALIVE
void M2W_Kept(void);
/// A constructor whose initialiser list starts a line: `m_pFirst` is not a
/// symbol (gr2_file.cpp).
CM2wThing::CM2wThing()
    : m_pFirst(NULL), m_iSecond(0)
{
}
"""
    with tempfile.NamedTemporaryFile('w', suffix='.h', delete=False, encoding='utf-8') as f:
        f.write(src)
    names = [(k, n, bool(d)) for k, n, d in scan_file(f.name)[1]]
    os.remove(f.name)
    expected = [('macro', 'D3DCOLOR_ARGB', False), ('function', 'M2W_Folded', True),
                ('function', 'M2W_Plain', True), ('enum', 'EM2wKind', True),
                ('EM_JS', 'm2w_url', True), ('function', 'M2W_After', True),
                ('function', 'M2W_AfterBlock', True), ('function', 'M2W_Kept', True)]
    ok = names == expected
    print(('selftest ok: ' if ok else 'selftest FAILED: ') + repr(names))
    return 0 if ok else 1


OPTION_ROW = re.compile(r"\{\s*name:\s*'([A-Za-z_]+)',\s*file:\s*'([^']+)'")


def first_line(text):
    """The first sentence-ish line of a description, `|` escaped for tables."""
    line = ' '.join((text or '').strip().split('\n\n')[0].split())
    return line.replace('|', '\\|')


def runtime_js_section():
    """`## runtime.js`: every top-level `m2w.x = function` with the first
    paragraph of its `///` text."""
    path = os.path.join(COMPAT, 'runtime.js')
    lines = read_text(path).split('\n')
    out = ['', '## `runtime.js` - page-side functions (`m2w.*`)', '',
           '| function | description |', '|---|---|']
    for i, l in enumerate(lines):
        m = re.match(r'^m2w\.([\w$.]+)\s*=\s*(?:async\s+)?function\b', l)
        if not m:
            continue
        doc, j = [], i - 1
        while j >= 0 and lines[j].strip().startswith('///'):
            doc.insert(0, lines[j].strip()[3:].strip())
            j -= 1
        out.append('| `m2w.%s` | %s |' % (m.group(1), first_line(' '.join(doc)) or '_no description_'))
    return out


def tools_sections():
    """`## Tools`: every tool script tracked by git (tools/**/*.py) with the
    first paragraph of its module docstring, then per script its functions
    with the first paragraph of their docstrings."""
    import ast
    import subprocess
    files = [f for f in subprocess.run(['git', 'ls-files', 'tools'], cwd=ROOT, capture_output=True,
                                       text=True).stdout.split()
             if f.endswith('.py') and not f.startswith(UNPUBLISHED)]
    trees = {}
    for f in files:
        trees[f] = ast.parse(read_text(os.path.join(ROOT, f)))
    out = ['', '## Tools (`tools/**/*.py`)', '', '| script | description |', '|---|---|']
    for f in files:
        out.append('| `%s` | %s |' % (f, first_line(ast.get_docstring(trees[f])) or '_no docstring_'))
    out += ['', '## Tool functions', '']
    for f in files:
        rows = []

        def walk(node, prefix):
            """Collect (qualified name, first docstring line) under `node`."""
            for ch in ast.iter_child_nodes(node):
                if isinstance(ch, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    rows.append((prefix + ch.name, first_line(ast.get_docstring(ch))))
                    walk(ch, prefix + ch.name + '.')
                elif isinstance(ch, ast.ClassDef):
                    walk(ch, prefix + ch.name + '.')
                else:
                    walk(ch, prefix)
        walk(trees[f], '')
        if not rows:
            continue
        out += ['### `%s`' % f, '', '| function | description |', '|---|---|']
        out += ['| `%s` | %s |' % (n, d or '_no docstring_') for n, d in rows]
        out.append('')
    return out


def options_table():
    """`m2w.options` rows from compat/runtime.js (and `m2w.options.add`
    calls in other files): (name, file). A file reading an option
    through the table is listed once, even when its EM_JS bridge still
    contains `.get('x')` textually."""
    rows = []
    for f in sorted(os.listdir(COMPAT)):
        if f.endswith('.js'):
            for m in OPTION_ROW.finditer(read_text(os.path.join(COMPAT, f))):
                rows.append(m.groups())
    return rows


def main(argv):
    """Writes docs/REFERENCE.md, or with `--check`/`--stats`/`--file` reports
    undocumented public symbols (1 when there are any with `--check`).
    """
    if '--selftest' in argv:
        return selftest()
    files = sorted(f for f in os.listdir(COMPAT) if f.endswith(('.cpp', '.h')))
    # a symbol documented at its declaration (header) counts as documented at
    # its definition too - the `///` belongs in one place, not both
    documented_anywhere = set()
    documented_where = {}
    documented_text = {}   # name -> the first `///` text found, for definitions without one
    for f in files:
        for k, n, d in scan_file(os.path.join(COMPAT, f))[1]:
            if d:
                documented_anywhere.add(n)
                documented_where.setdefault(n, []).append(f)
                documented_text.setdefault(n, (d, f))
    # inheritance is by bare name: say so when a name is documented twice
    # (reviewer review) - today 0 such names
    for n, where in sorted(documented_where.items()):
        if len(where) > 1 and ('--check' in argv or '--stats' in argv):
            print('warning: %s documented in %s - definitions inherit by bare name' % (n, ', '.join(where)))
    if '--file' in argv:
        only = argv[argv.index('--file') + 1]
        files = [f for f in files if f == only or f == os.path.basename(only)]
        if not files:
            raise SystemExit('no such compat file: ' + only)
    n_sym = n_doc = 0
    undocumented = []
    sections, all_params, all_globs = [], {}, {}
    for f in files:
        head, symbols, params, globs = scan_file(os.path.join(COMPAT, f))
        # a definition without its own `///` shows the declaration's text
        # (fresh-reader test: "(documented at declaration)" sent the
        # reader to another file for every .cpp row)
        symbols = [(k, n, d or ('%s *(from %s)*' % documented_text[n] if n in documented_text else ''))
                   for k, n, d in symbols]
        for p in params:
            all_params.setdefault(p, set()).add(f)
        for g in globs:
            all_globs.setdefault(g, set()).add(f)
        n_sym += len(symbols)
        n_doc += sum(1 for _k, _n, d in symbols if d)
        undocumented += [(f, k, n) for k, n, d in symbols if not d]
        sections.append((f, head, symbols))
    # JS globals set in compat/*.js (runtime.js) count too - the page reads
    # some of them (m2w_all_chunks, m2w_prefetch...); without this the table
    # showed only the .cpp ones and "20 -> 4" looked like all were gone
    for f in sorted(os.listdir(COMPAT)):
        if f.endswith('.js'):
            for g in GLOBAL.findall(read_text(os.path.join(COMPAT, f))):
                all_globs.setdefault(g, set()).add(f)
    for name, f in options_table():
        # a bridge like `m2w.options.get('run')` in main_web.cpp is also
        # caught by PARAM - one row per file (reviewer review)
        all_params.setdefault(name, set()).discard(f)
        all_params[name].add(f + ' (m2w.options)')
    if '--file' in argv and '--check' not in argv and '--stats' not in argv:
        argv = list(argv) + ['--check']          # --file alone means the per-file gate
    if '--stats' in argv or '--check' in argv:
        print('compat files: %d, public symbols: %d, documented (///): %d, undocumented: %d' %
              (len(files), n_sym, n_doc, len(undocumented)))
        print('URL parameters: %d, JS globals: %d' % (len(all_params), len(all_globs)))
        if '--check' in argv:
            for f, k, n in undocumented[:40]:
                print('  %-26s %-8s %s' % (f, k, n))
            if len(undocumented) > 40:
                print('  ... %d more' % (len(undocumented) - 40))
            return 1 if undocumented else 0
        return 0
    out = ['# Reference (generated by tools/gates/reference.py - do not edit)', '',
           'Every file of the browser layer (`compat`), what it is for, and its public symbols. ',
           'Symbols without a description have no `///` comment yet (tools/gates/check_docs.py lists them).', '']
    for f, head, symbols in sections:
        out.append('## `%s`' % f)
        out.append('')
        out.append(head or '_(no header comment)_')
        out.append('')
        if symbols:
            out.append('| kind | symbol | description |')
            out.append('|---|---|---|')
            for k, n, d in symbols:
                out.append('| %s | `%s` | %s |' % (k, n, d.replace('|', '\\|') or '_undocumented_'))
            out.append('')
    out.append('## URL parameters')
    out.append('')
    out.append('| parameter | read in |')
    out.append('|---|---|')
    for p in sorted(all_params):
        out.append('| `?%s` | %s |' % (p, ', '.join(sorted(all_params[p]))))
    if not any('m2w.options' in ', '.join(v) for v in all_params.values()):
        raise SystemExit('reference.py: no m2w.options rows found in compat/runtime.js')
    out.append('')
    out.append('## JavaScript globals')
    out.append('')
    out.append('| global | set in |')
    out.append('|---|---|')
    for g in sorted(all_globs):
        out.append('| `%s` | %s |' % (g, ', '.join(sorted(all_globs[g]))))
    out += runtime_js_section()
    out += tools_sections()
    with open(OUT, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(out) + '\n')
    print('wrote %s: %d files, %d symbols (%d documented), %d params, %d globals' %
          (rel(OUT), len(files), n_sym, n_doc, len(all_params), len(all_globs)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
