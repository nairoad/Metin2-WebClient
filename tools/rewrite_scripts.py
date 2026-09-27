#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rewrite_scripts.py - the game scripts from Python 2 to Python 3.

WHY AT ALL
==========
The game scripts in the `root.epk` pack are written in Python 2: `print "..."`
occurs 142 times, `print(` not once. The port embeds CPython 3, which will
not run that - the client stops at `RunMain Error`, because `system.py`
cannot even be parsed.

WHY `lib2to3` AND NOT OUR OWN RULES
===================================
It was tempting to write a dozen regular expressions - the incompatibilities
are few and known. But `print` in Python 2 is **a statement with a grammar**,
not a function: `print >>file, x`, `print a,` with a trailing comma, `print`
alone. A regular expression will hit the easy cases and silently break the
hard ones - and a broken game script is not a crash, just a missing window.

`lib2to3` parses code **with the Python 2 grammar** and rewrites the tree.
It is the tool half the world moved to Python 3 with.

It is not in Python 3.13 (removed in 3.13), but it is in 3.10 - and this
script calls THAT Python as a separate process. Hence `PYTHON_2TO3`.

WHAT WE DO **NOT** REWRITE - and this is the most important part of this file
=============================================================================
`lib2to3` has a separate `division` fixer, which **I do not enable**, and that
is a decision, not an oversight.

In Python 2 `7 / 2` gives `3`. In Python 3 it gives `3.5`. Replacing `/` with
`//` everywhere would break every division that really was meant to give a
fraction; not replacing leaves other numbers where the code relied on
truncation. **It cannot be decided statically which is which** - one would
have to know the types, and Python does not declare them.

So instead of guessing, this script **counts** the occurrences and prints them
as a known, open point. `tokenize` counts them, not a regular expression,
so slashes in strings (`"icon/item/%d.tga"`) and in comments do not
count. Without that distinction the number would be ten times larger
and useless.

The same goes for `has_key`: `lib2to3` rewrites `d.has_key(k)` to `k in d`
and that replacement is safe, so I leave it enabled.
"""

import io
import os
import shutil
import subprocess
import sys
import tokenize

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # noqa: E402 - [tools] python2to3
SOURCE = os.path.join(ROOT, 'build', 'port', 'scripts')
TARGET = os.path.join(ROOT, 'build', 'port', 'scripts3')

# A Python in which `lib2to3` still exists. Removed in 3.13.
PYTHON_2TO3 = workspace.PYTHON_2TO3    # [tools] python2to3 / M2W_PYTHON_2TO3


def count_divisions(path_str):
    """How many times the `/` operator occurs in the file - counting TOKENS, not text.

    A slash in a string and in a comment is not an operator. A regular
    expression does not tell them apart and gave a tenfold inflated
    number - and an inflated measurement is worse than none, because it looks
    like knowledge.
    """
    how_many = 0
    try:
        with open(path_str, 'rb') as f:
            for tok in tokenize.tokenize(f.readline):
                if tok.type == tokenize.OP and tok.string == '/':
                    how_many += 1
    except Exception:
        # A file `tokenize` does not get through simply does not enter
        # the count. I return -1, so that it shows, rather than looks
        # like zero.
        return -1
    return how_many


def parses_in_python3(path_str):
    """True when the file parses as Python 3."""
    import ast
    try:
        ast.parse(io.open(path_str, encoding='utf-8', errors='replace').read())
        return True
    except SyntaxError:
        return False




def to_utf8(file_list):
    """Converts files from CP949 to UTF-8. Returns the number converted.

    WHY THIS IS NEEDED AT ALL
    -------------------------
    `lib2to3` stopped on `UnicodeDecodeError` in 34 files. That is not
    damage: the game scripts have **the authors' Korean comments**
    written in CP949, because Metin2 was made in Korea. Bytes like
    `Å×½ºÆ®` are simply the word "test".

    Python 3 reads sources as UTF-8, so these files have to be converted.
    All 34 decode as CP949 **without a single error** - and that is
    stronger evidence than the code page name alone: if it
    were something else, decoding would trip on some byte.

    The order matters: UTF-8 first, only then CP949. A file
    that already is valid UTF-8 stays untouched - because CP949
    would take its bytes as something else and silently change the content.
    """
    how_many = 0
    for path_str in file_list:
        data_bytes = io.open(path_str, 'rb').read()

        try:
            data_bytes.decode('utf-8')
            continue          # already fine, not touching it
        except UnicodeDecodeError:
            pass

        try:
            content = data_bytes.decode('cp949')
        except UnicodeDecodeError:
            # I do not guess further. The file stays as it was, and `lib2to3`
            # will say so loudly - better than converting it with
            # a code page that only "looks close".
            print('  CANNOT READ: ' + os.path.basename(path_str))
            continue

        io.open(path_str, 'w', encoding='utf-8', newline='').write(content)
        how_many += 1
    return how_many




def match_names(directory):
    """Renames files to the spelling the scripts really import.

    SYMPTOM:
        ModuleNotFoundError: No module named 'debugInfo'

    while the file is called `debuginfo.py`. Windows does not distinguish case
    in file names, so for twenty years this bothered nobody.
    The wasm file system does distinguish it.

    WHY HERE AND NOT IN THE COMPATIBILITY LAYER
    -------------------------------------------
    The compatibility layer (`compat/paths_web.cpp`) resolves it too and is
    needed there - for textures, icons and sounds the game's C++ code
    reaches for. But **a Python import does not touch it**: CPython looks for
    modules with its own mechanism and its own system calls.

    For imports the only place where this can be handled without digging
    in CPython is the file name.

    THE MEASUREMENT THAT MAKES THIS SAFE
    ------------------------------------
    A rename is unambiguous only when every file is
    imported with ONE spelling. Checked: 66 imported files,
    **zero** with more than one spelling. If one had two, this
    function stops - because then there is no good name and a decision is
    needed, not guessing.
    """
    import collections
    import re

    names = [n for n in os.listdir(directory) if n.endswith('.py')]
    by_lowercase = {n.lower(): n for n in names}

    usages = collections.defaultdict(set)
    for n in names:
        content = io.open(os.path.join(directory, n), encoding='utf-8',
                        errors='replace').read()
        for m in re.finditer(r'^\s*(?:import|from)\s+([A-Za-z_][\w]*)',
                             content, re.M):
            module = m.group(1)
            key_name = (module + '.py').lower()
            if key_name in by_lowercase:
                usages[key_name].add(module)

    disputed = {k: v for k, v in usages.items() if len(v) > 1}
    if disputed:
        print('DISPUTED NAMES - the same file imported in several spellings:')
        for k, v in sorted(disputed.items()):
            print('  %s -> %s' % (k, sorted(v)))
        raise SystemExit('not guessing which spelling is the right one')

    how_many = 0
    for key_name, variants in usages.items():
        module = list(variants)[0]
        old_value = by_lowercase[key_name]
        new_value = module + '.py'
        if old_value == new_value:
            continue
        os.rename(os.path.join(directory, old_value),
                  os.path.join(directory, new_value))
        how_many += 1

    return how_many


def is_referenced(path_str, file_list):
    """Does any other script mention this file's name (without `.py`)? The game
    loads window descriptions and modules by name, so a name no script mentions
    is a file nothing loads.
    """
    stem = os.path.splitext(os.path.basename(path_str))[0].lower()
    for other in file_list:
        if other == path_str:
            continue
        with io.open(other, encoding='utf-8', errors='replace') as f:
            if stem in f.read().lower():
                return True
    return False


def main():
    """Rewrites the game scripts from Python 2 to 3 (lib2to3 without the
    division fixer, then our script patches and the division pass) into a
    clean target, and reports files that still do not parse.
    """
    if not os.path.isdir(SOURCE):
        print('missing ' + os.path.relpath(SOURCE, ROOT))
        print('unpack the pack first - see '
              'tools/unpack_pack.cpp')
        return 1

    if not os.path.exists(PYTHON_2TO3):
        print('no Python with lib2to3 found: ' + PYTHON_2TO3)
        print('set M2W_PYTHON_2TO3 to a python.exe of version 3.9-3.12')
        return 1

    # Target from scratch - otherwise a file removed from the source stays in the result
    # and the measurement lies (the same shape as the stale `.o`).
    if os.path.isdir(TARGET):
        shutil.rmtree(TARGET)
    shutil.copytree(SOURCE, TARGET)

    file_list = []
    for root_node, _k, names in os.walk(TARGET):
        file_list += [os.path.join(root_node, n) for n in sorted(names)
                  if n.endswith('.py')]


    utf8_count = to_utf8(file_list)
    print('converted from CP949 to UTF-8: %d' % utf8_count)

    before_bad = sum(0 if parses_in_python3(p) else 1 for p in file_list)
    print('.py files: %d | NOT parsing before the rewrite: %d'
          % (len(file_list), before_bad))

    # `-w` writes in place, `-n` leaves no `.bak` copy,
    # `--no-diffs` prints no differences (there are tens of thousands
    # of lines of them and they add nothing to the result).
    r = subprocess.run(
        [PYTHON_2TO3, '-m', 'lib2to3', '-w', '-n', '--no-diffs', TARGET],
        capture_output=True, text=True)

    # `lib2to3` reports a rewrite with the line 'RefactoringTool: Refactored <file>'.
    # The first version counted 'Writing converted' and always gave zero -
    # the measurement said nothing happened, although the tool did not
    # get to work at all (it stopped on the encoding).
    changed = sum(1 for l in r.stderr.splitlines()
                    if 'Refactored ' in l)
    print('files rewritten by lib2to3: %d' % changed)

    # --- patches lib2to3 will NOT do -----------------------------------
    # Semantic differences: syntactically the code is correct, so no
    # tool will detect them. Each comes from an OBSERVED error
    # in the running client - see tools/script_patches.py.
    import script_patches
    print('semantic patches:')
    script_patches.apply_all(TARGET)

    not_parsing = [p for p in file_list if not parses_in_python3(p)]
    # A file that does not parse is an error only when a script refers to it
    # the packs can carry dead files broken at the source - measured:
    # `locale/*/ui/taskbar_haloween.py` has a string split across two lines
    # already in the pack, and nothing loads it. Those are listed, not fatal.
    after_bad = [p for p in not_parsing if is_referenced(p, file_list)]
    unused = [p for p in not_parsing if p not in after_bad]
    print('NOT parsing after the rewrite: %d' % len(after_bad))
    for p in after_bad[:10]:
        print('   ' + os.path.relpath(p, TARGET))
    if unused:
        print('not parsing, but no script refers to them (unused, broken in the pack): %d' % len(unused))
        for p in unused[:10]:
            print('   ' + os.path.relpath(p, TARGET))


    name_count = match_names(TARGET)
    print('names matched to the spelling of imports: %d' % name_count)

    # --- division -------------------------------------------------------
    # Earlier there was an OPEN POINT here: 305 `/` operators and the sentence
    # that it cannot be decided statically which of them was meant to truncate.
    #
    # The sentence was too strong. One need not know WHAT TYPE the result is -
    # it is enough to answer the reverse question: can a fraction occur in this
    # division AT ALL. If it cannot, then in Python 2 it was
    # an integer and `//` reproduces it exactly. The rule and its limits
    # are described in `tools/division.py`.
    #
    # FROM GUESSING TO MEASUREMENT: the client stopped at
    #
    #     ui.py(984) SetText - textLine.SetPosition(self.GetWidth()/2, ...)
    #     SystemError: <built-in function SetWindowPosition> returned
    #                  a result with an exception set
    #
    # `GetWidth()/2` gives `400.0` in Python 3, and `PyTuple_GetInteger` on
    # the other side of the bridge does not accept a fraction. That was not an open
    # point to think over - it was a wall.
    import division
    integer_total, kept_total, unknown = 0, 0, 0
    operand_names = set()
    for p in file_list:
        content = io.open(p, encoding='utf-8', errors='replace',
                        newline='').read()
        try:
            new_value, integer_count, kept, names = division.convert_divisions(content)
        except Exception:
            # A file `tokenize` does not get through stays as it was.
            # I count it separately, so that it does not look like zero.
            unknown += 1
            continue
        integer_total += integer_count
        kept_total += kept
        operand_names |= names
        if integer_count:
            io.open(p, 'w', encoding='utf-8', newline='').write(new_value)

    print('\nDIVISION')
    print('  to `//` (integer, as in Python 2): %d' % integer_total)
    print('  kept as `/` (a fraction already in Python 2): %d' % kept_total)
    if unknown:
        print('  files tokenize could not get through: %d' % unknown)
    print('  names used as an operand of integer division: %d'
          % len(operand_names))

    # THE SECOND HALF OF THE ANSWER is in C, not here: `PyTuple_GetInteger`
    # accepts a `float` and truncates, as `PyInt_AsLong` did
    # in Python 2. Thanks to that a division this rule does NOT decide
    # gives at worst a different rounding, not a killed client.

    print('\nresult in: ' + os.path.relpath(TARGET, ROOT))
    return 0 if not after_bad else 1


if __name__ == '__main__':
    sys.exit(main())
