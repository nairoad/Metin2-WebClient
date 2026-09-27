#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every function of the port layer has a `///` description - checked by the compiler.

  python tools/gates/check_docs.py                 # counts per file
  python tools/gates/check_docs.py --list          # also every undocumented function
  python tools/gates/check_docs.py --file gl_device.cpp --list   # one translation unit
  python tools/gates/check_docs.py --expect-zero   # exit 1 if anything is undocumented
  python tools/gates/check_docs.py --record        # store the undocumented list (docs_baseline.txt)
  python tools/gates/check_docs.py --compare       # exit 1 if a function is undocumented that
                                                    # the baseline did not list (a NEW gap)

reference.py only sees PUBLIC symbols (free functions, EM_JS, classes) by
regex. The rule of the plan (§4 point 3b) is wider: EVERY function - class
methods, `static` helpers, anonymous-namespace functions, constructors -
has a `///` line saying what it does. This tool asks clang itself: each
compat/*.cpp is run through em++ with `-Xclang -ast-dump` (the
same include paths as the build) and the text AST is streamed. A function
counts as documented when clang attached a doc comment (`///`, `/** */`) to
ANY of its declarations - the rule "the description sits in one place,
at the declaration when there is a header". Plain `//` blocks
do not count; that is clang's own rule for doc comments.

Categories:
  documented   - a doc comment on some declaration of the function;
  inherited    - no own comment, but it overrides a method that has one
                 (Doxygen's INHERIT_DOCS); listed with --inherited;
  undocumented - neither.
Python (tools/**/*.py tracked by git): every function, method and
nested function needs a docstring (ast; skipped with --file).
JavaScript (compat/*.js): every NAMED function (`m2w.x = function`,
`name: function`, `var x = function`, `function x(`, and the same with an
arrow `(..) =>` / `x =>` as the value) needs `///` on the line
directly above - anonymous callbacks are not counted. Scanned by regex (no
parser), skipped with --file.
Not counted: implicit functions, `= default` / `= delete`, lambdas, and
anything outside compat or inside compat/Python-2.7 (verbatim
copies of CPython headers).

Declarations are keyed by the file and line of their FIRST declaration, so a
header function seen by forty translation units is counted once. Free
functions with external linkage are additionally joined ACROSS translation
units by name and type, so an `extern` declaration in one .cpp shares the
`///` of the definition in another. Limitation of that join: two DIFFERENT
external functions with the same name and type in different files (or
namespaces) would count as one - a review checked the 10 such pairs in
compat are all a declaration and its definition. The
baseline keys are `file:name` without line numbers (lines move with every
edit); a limitation: a second undocumented function of the same name in the
same file (`AddRef` of another interface) is not reported as NEW.
"""
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import COMPAT, STAGE, EXTERN, ROOT, tool  # noqa: E402

FUNC_KINDS = {'FunctionDecl', 'CXXMethodDecl', 'CXXConstructorDecl',
              'CXXDestructorDecl', 'CXXConversionDecl'}
NODE = re.compile(r'^([|`\- ]*)([A-Za-z]\w*)(?: (0x[0-9a-f]+))?(.*)$')
LOCTOK = re.compile(r'(line):(\d+):\d+|col:\d+|'
                    r'((?:[A-Za-z]:\\|/)[^<>,]*?|<built-in>|<scratch space>|<command line>):(\d+):\d+')
SKIP_NAMES = {'used', 'referenced', 'implicit', 'invalid', 'hidden', 'imported',
              'constexpr', 'inline', 'static', 'extern'}
BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'docs_baseline.txt')
EXCLUDED_DIR = os.path.normcase(os.path.join(COMPAT, 'Python-2.7'))
COMPAT_NC = os.path.normcase(COMPAT)


def compile_cmd(src):
    """The em++ command that dumps the text AST of one compat source."""
    python_inc = os.path.join(ROOT, 'build', 'port', 'python313', 'Include')  # = build_gamelib.PYTHON_INC
    return [tool('em++'), '-std=c++17', '-fsyntax-only', '-w', '-sUSE_LIBJPEG=1',
            '-Xclang', '-ast-dump', '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
            '-I' + os.path.join(STAGE, 'eterBase'), '-I' + python_inc, src]


def balanced(s, i):
    """Index just past the `<...>` group starting at s[i] (nested groups allowed)."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '<':
            depth += 1
        elif s[j] == '>':
            depth -= 1
            if depth == 0:
                return j + 1
    return len(s)


class Tracker:
    """Clang prints a file (and line) only when it changes; this follows it."""

    def __init__(self):
        """No file and no line yet - the first location token sets both."""
        self.file, self.line = None, None

    def feed(self, text):
        """Update the current file/line from every location token in `text`."""
        for m in LOCTOK.finditer(text):
            if m.group(1):
                self.line = int(m.group(2))
            elif m.group(3):
                self.file, self.line = m.group(3), int(m.group(4))


def ours(path):
    """True for files of the port layer (compat/, minus the CPython copies)."""
    if not path or path.startswith('<'):
        return False
    p = os.path.normcase(os.path.abspath(path))
    return p.startswith(COMPAT_NC) and not p.startswith(EXCLUDED_DIR)


def scan(src):
    """Run clang on one source; return (decls, error). decls: addr -> dict."""
    proc = subprocess.Popen(compile_cmd(src), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    trk = Tracker()
    decls = {}          # addr -> {file, line, name, doc, prev, overrides, skip, kind}
    stack = []          # (depth, kind, addr)
    for raw in proc.stdout:
        line = raw.decode('utf-8', 'replace').rstrip('\r\n')
        m = NODE.match(line)
        if not m:
            continue
        prefix, kind, addr, rest = m.groups()
        depth = len(prefix) // 2
        while stack and stack[-1][0] >= depth:
            stack.pop()
        rest = rest.lstrip()
        prev = None
        # `parent 0x..` / `prev 0x..` come before the source range
        while True:
            mm = re.match(r'(parent|prev) (0x[0-9a-f]+) ?', rest)
            if not mm:
                break
            if mm.group(1) == 'prev':
                prev = mm.group(2)
            rest = rest[mm.end():]
        if rest.startswith('<'):
            end = balanced(rest, 0)
            trk.feed(rest[:end])
            rest = rest[end:].lstrip()
        dfile = dline = None
        if kind in FUNC_KINDS and rest and not rest.startswith('<invalid'):
            tok = rest.split(' ', 1)[0]
            trk.feed(tok)
            dfile, dline = trk.file, trk.line
            rest = rest[len(tok):].lstrip()
            if rest.startswith('<Spelling='):
                end = balanced(rest, 0)
                trk.feed(rest[:end])
                rest = rest[end:].lstrip()
        else:
            trk.feed(rest.split("'", 1)[0])
        if kind == 'FullComment':
            for d, k, a in reversed(stack):
                if k in FUNC_KINDS or k == 'FunctionTemplateDecl':
                    if a in decls:
                        decls[a]['doc'] = True
                    else:
                        decls.setdefault('tmpl:' + a, {'doc': True})
                    break
                if k not in ('FullComment', 'ParagraphComment', 'TextComment'):
                    break
        if line.lstrip('|`- ').startswith('Overrides: ['):
            for d, k, a in reversed(stack):
                if k in FUNC_KINDS:
                    decls[a]['overrides'] += re.findall(r'0x[0-9a-f]+', line)
                    break
        if kind in FUNC_KINDS and addr:
            head = rest.split("'", 1)[0].split()
            name = next((w for w in reversed(head) if w not in SKIP_NAMES), '?')
            tail = rest.rsplit("'", 1)[-1].split()
            parent_tmpl = stack[-1][2] if stack and stack[-1][1] == 'FunctionTemplateDecl' else None
            quoted = re.search(r"'([^']*)'", rest)
            decls[addr] = {
                'file': dfile, 'line': dline, 'name': name, 'kind': kind,
                'ext': ((name, quoted.group(1)) if kind == 'FunctionDecl' and quoted
                        and 'external-linkage' in tail else None),
                'doc': bool(parent_tmpl and decls.get('tmpl:' + parent_tmpl, {}).get('doc')),
                'prev': prev, 'overrides': [], 'tmpl': parent_tmpl,
                'skip': ('implicit' in head or 'delete' in tail or 'default' in tail
                         or any(k in ('LambdaExpr',) for _, k, _ in stack)),
            }
        if addr:
            stack.append((depth, kind, addr))
    err = proc.stderr.read().decode('utf-8', 'replace')
    rc = proc.wait()
    return decls, (err[-800:] if rc else None)


# a function value: `function`, or an arrow `(a, b) =>` / `a =>` (a review
# noted arrows slipped through)
_JS_FN = r'(?:async\s+)?(?:function\b|\([^()]*\)\s*=>|[\w$]+\s*=>)'
JS_FUNCTION = re.compile(r'^\s*(?:(?:var|let|const)\s+([\w$]+)\s*=\s*' + _JS_FN +
                         r'|([\w$.]+)\s*=\s*' + _JS_FN +
                         r'|([\w$]+)\s*:\s*' + _JS_FN +
                         r'|(?:async\s+)?function\s+([\w$]+)\s*\()')


def tracked(prefixes, suffixes, with_re=False):
    """Repository paths (absolute) tracked by git under `prefixes` with one of
    `suffixes`, without vendored node_modules and - unless `with_re` - without
    tools/re (its C++/JS dumps are not published code; its Python IS checked,
    as it was earlier)."""
    files = subprocess.run(['git', 'ls-files'] + list(prefixes), cwd=ROOT, capture_output=True,
                           text=True).stdout.split()
    return [os.path.join(ROOT, f) for f in files
            if f.endswith(suffixes) and (with_re or not f.startswith('tools/re/')) and 'node_modules' not in f]


def js_functions():
    """Named functions of the published JavaScript - compat/*.js, the page and
    measuring scripts in site/ and tools/browser, the bridge (bridge/src), and inline scripts of
    the tracked pages (client.html, measure_shell.html) - as (file, line,
    name, documented): `m2w.x = function`, `name: function`, `var x =
    function`, `function x(`; documented when the line directly above starts
    with `///`, the same convention as the C++ side. Anonymous callbacks are
    not counted (beyond compat too)."""
    out = []
    paths = sorted(set(glob.glob(os.path.join(COMPAT, '*.js'))) |
                   set(tracked(['site', 'tools/browser', 'bridge/src'], ('.js', '.html'))) |
                   set(tracked(['site/measure_shell.html'], ('.html',))))
    for path in paths:
        with open(path, encoding='utf-8') as fh:
            lines = fh.read().split('\n')
        for i, text in enumerate(lines):
            m = JS_FUNCTION.match(text)
            if m:
                name = next(g for g in m.groups() if g)
                doc = i > 0 and lines[i - 1].strip().startswith('///')
                out.append((os.path.normcase(os.path.abspath(path)), i + 1, name, doc))
    return out


def py_functions():
    """Every function of the Python tools (tools/**/*.py tracked by git) as
    (file, line, qualified name, has a docstring) - the same rule for the
    tools as for compat."""
    import ast
    out = []
    for path in tracked(['tools', 'tests', 'webclient.py'], ('.py',), with_re=True):
        with open(path, encoding='utf-8') as fh:
            tree = ast.parse(fh.read())

        def walk(node, prefix):
            """Collect the functions under `node`, qualifying nested names."""
            for ch in ast.iter_child_nodes(node):
                if isinstance(ch, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    out.append((os.path.normcase(path), ch.lineno, prefix + ch.name,
                                ast.get_docstring(ch) is not None))
                    walk(ch, prefix + ch.name + '.')
                elif isinstance(ch, ast.ClassDef):
                    walk(ch, prefix + ch.name + '.')
                else:
                    walk(ch, prefix)
        walk(tree, '')
    return out


CPP_FUNCTION = re.compile(r'^(?!(?:if|for|while|switch|return|else|do|case|namespace|struct|class|enum|typedef|using)\b)'
                          r'[A-Za-z_][\w:<>,\*&\s]*?[\s\*&]([A-Za-z_~][\w:~]*)\s*\([^;]*$')


def other_cpp_functions():
    """Top-level function definitions of the published C++ OUTSIDE compat
    (the unit-test programs in compat/tests, tests/, the C++ tools) as (file,
    line, name, documented). These files are not compiled with the compat
    flags, so they are read by pattern, not by clang: a definition starts in
    column 0 and its body brace follows on that line or the next;
    documented when the nearest non-blank line above starts with `///`.
    Functions inside class bodies are not seen by this pass."""
    out = []
    paths = tracked(['compat/tests', 'tests', 'tools'], ('.cpp',))
    tests_nc = os.path.normcase(os.path.join(COMPAT, 'tests'))
    paths = [p for p in paths if not (os.path.normcase(p).startswith(COMPAT_NC)
                                      and not os.path.normcase(p).startswith(tests_nc))
             and os.sep + 'tree' + os.sep not in p]
    for path in sorted(paths):
        with open(path, encoding='utf-8', errors='replace') as fh:
            lines = fh.read().split('\n')
        for i, text in enumerate(lines):
            m = CPP_FUNCTION.match(text)
            if not m or text[:1].isspace():
                continue
            j = i
            while j < len(lines) and '{' not in lines[j] and ';' not in lines[j] and j - i < 4:
                j += 1
            if j >= len(lines) or '{' not in lines[j]:
                continue
            k = i - 1
            while k >= 0 and (not lines[k].strip() or lines[k].startswith('template')):
                k -= 1
            doc = k >= 0 and lines[k].strip().startswith('///')
            out.append((os.path.normcase(os.path.abspath(path)), i + 1, m.group(1), doc))
    return out


def resolve(decls):
    """Group redeclarations; return {key: (name, kind, documented, overridden_keys)}."""
    def root(a):
        """The first declaration of `a`, following the `prev` links."""
        seen = set()
        while decls.get(a, {}).get('prev') in decls and a not in seen:
            seen.add(a)
            a = decls[a]['prev']
        return a

    groups = {}
    for a, d in decls.items():
        if a.startswith('tmpl:'):
            continue
        if d['tmpl'] and decls.get('tmpl:' + d['tmpl'], {}).get('doc'):
            d['doc'] = True
        r = root(a)
        groups.setdefault(r, []).append(a)
    out = {}
    for r, members in groups.items():
        rd = decls[r]
        if not ours(rd['file']) or any(decls[m]['skip'] for m in members):
            continue
        key = (os.path.normcase(os.path.abspath(rd['file'])), rd['line'], rd['name'])
        doc = any(decls[m]['doc'] for m in members)
        over = []
        for m in members:
            for o in decls[m]['overrides']:
                if o in decls:
                    od = decls[root(o)]
                    if od['file']:
                        over.append((os.path.normcase(os.path.abspath(od['file'])), od['line'], od['name'],
                                     any(decls[x]['doc'] for x in groups.get(root(o), []))))
        out[key] = (rd['name'], rd['kind'], doc, over, rd['ext'])
    return out


def main(argv):
    """Scan every compat translation unit and report undocumented functions."""
    srcs = sorted(glob.glob(os.path.join(COMPAT, '*.cpp')))
    if '--file' in argv:
        want = argv[argv.index('--file') + 1]
        srcs = [s for s in srcs if os.path.basename(s) == want]
    total, errors = {}, []
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
        for src, (decls, err) in zip(srcs, ex.map(scan, srcs)):
            if err:
                errors.append((os.path.basename(src), err))
                continue
            for k, (name, kind, doc, over, ext) in resolve(decls).items():
                old = total.get(k)
                odoc = any(o[3] for o in over)
                if old:
                    doc = doc or old[2]
                    odoc = odoc or old[3]
                total[k] = (name, kind, doc, odoc, ext)
    # A free function with external linkage may be declared `extern` in one
    # .cpp and defined (with its `///`) in another; clang never sees both in
    # one translation unit, so the two are joined by name and type here.
    ext_doc = set(v[4] for v in total.values() if v[4] and v[2])
    if '--file' not in argv:
        for f, line, name, doc in js_functions():
            total[(f, line, name)] = (name, 'JSFunction', doc, False, None)
        for f, line, name, doc in py_functions():
            total[(f, line, name)] = (name, 'PyFunction', doc, False, None)
        for f, line, name, doc in other_cpp_functions():
            total[(f, line, name)] = (name, 'CppFunction', doc, False, None)
    per_file = {}
    for (f, line, name), (_, kind, doc, odoc, ext) in sorted(total.items()):
        doc = doc or (ext in ext_doc)
        cat = 'documented' if doc else ('inherited' if odoc else 'undocumented')
        # compat files by their name (as before), tools by their repo path
        where = (os.path.relpath(f, COMPAT) if os.path.normcase(f).startswith(COMPAT_NC)
                 else os.path.relpath(f, ROOT))
        per_file.setdefault(where, []).append((line, name, kind, cat))
    counts = {'documented': 0, 'inherited': 0, 'undocumented': 0}
    for f, items in sorted(per_file.items()):
        c = {k: sum(1 for i in items if i[3] == k) for k in counts}
        for k in counts:
            counts[k] += c[k]
        if c['undocumented'] or '--all-files' in argv:
            print('%-28s %4d functions, %4d documented, %3d inherited, %3d undocumented'
                  % (f, len(items), c['documented'], c['inherited'], c['undocumented']))
        if '--list' in argv:
            for line, name, kind, cat in items:
                if cat == 'undocumented' or (cat == 'inherited' and '--inherited' in argv):
                    print('    %s:%d  %s  (%s)%s' % (f, line, name, kind,
                                                    '  [inherits]' if cat == 'inherited' else ''))
    for f, e in errors:
        print('CLANG FAILED on %s:\n%s' % (f, e))
    undocumented = sorted('%s:%s' % (f.replace('\\', '/'), name)
                          for f, items in per_file.items()
                          for line, name, kind, cat in items if cat == 'undocumented')
    rc = 0
    if '--record' in argv and not errors:
        with open(BASELINE, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('# undocumented functions (file:name), tools/gates/check_docs.py --record\n')
            fh.write(''.join(u + '\n' for u in undocumented))
        print('recorded %d undocumented functions in %s' % (len(undocumented), os.path.relpath(BASELINE, ROOT)))
    if '--compare' in argv:
        with open(BASELINE, encoding='utf-8') as fh:
            base = set(l.strip() for l in fh if l.strip() and not l.startswith('#'))
        new = [u for u in undocumented if u not in base]
        gone = len(base - set(undocumented))
        for u in new:
            print('NEW UNDOCUMENTED  ' + u)
        print('vs baseline: %d undocumented now, %d in baseline, %d new, %d documented since'
              % (len(undocumented), len(base), len(new), gone))
        rc = 1 if new or errors else 0
    print('translation units: %d (%d failed); functions: %d - documented %d, inherited %d, undocumented %d'
          % (len(srcs), len(errors), sum(counts.values()), counts['documented'],
             counts['inherited'], counts['undocumented']))
    if '--expect-zero' in argv and (counts['undocumented'] or errors):
        return 1
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
