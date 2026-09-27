#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""uiscript_from_client.py - the interface window descriptions, extracted and rewritten.

WHAT THE `UIScript/*.py` FILES ARE
==================================
This is not game code but its **window layout**, written as a Python
dictionary: name, position, size, children, button images. The client
executes such a file and reads one dictionary `window` from it.

Every interface window goes this way, so without these 93 files there is
neither a login screen nor anything after it. The client stops at the
first of them - `UIScript/PopupDialog.py` - before it shows anything.

THREE THINGS HAVE TO BE DONE WITH THEM
======================================
1. **Extract them from the client.** They lie in the packs (unpacked by
   `build_corpus.py --unpack-only`).

2. **Rewrite them to Python 3** - `lib2to3`, the same as the game scripts.

3. **DECIDE THE DIVISION.** And that really matters here, unlike in the
   game scripts. Every coordinate in these files is computed:

       "x": SCREEN_WIDTH/2 - 250,

   In Python 2 that gives `150`. In Python 3 `150.0` - and
   `PyTuple_GetInteger` on the other side of the bridge does not accept a
   fraction. The window would not be off by half a pixel; it would not be
   placed at all.

   `tools/division.py` decides, by the direct operands. Not every division
   goes to `//`: the authors wrapped some in `float(...)` themselves, and
   those stay.

NAME SPELLING
=============
The game scripts ask for these files in TWO spellings -
`UIScript/TaskBar.py` and `uiscript/questiondialog.py` - because on Windows
it was the same. It cannot be solved on the data side: `UIScript` and
`uiscript` are ONE directory on the disk where I assemble this data.

So the solution is on the code side: `ui.py` resolves the case before
opening the file (a patch in `tools/script_patches.py`). Here I write one
spelling - `UIScript/` - and that is all.
"""

import io
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import division                                            # noqa: E402
import workspace                                            # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS3 = os.path.join(ROOT, 'build', 'port', 'scripts3')
TARGET = os.path.join(ROOT, 'build', 'port', 'uiscript3')
DIRECTORY = 'UIScript'

PYTHON_2TO3 = workspace.PYTHON_2TO3    # [tools] python2to3 / M2W_PYTHON_2TO3


def spellings_from_client():
    """Collects the name spelling the game scripts really use.

    The key is the lower-case name, the value - the spelling from the code.
    When the same file occurs in the code in two spellings, either one wins:
    `ui.py` resolves the difference when opening anyway.
    """
    import re
    formula = re.compile(r'[Uu][Ii][Ss]cript/([A-Za-z0-9_]+\.py)')
    found_items = {}
    for root_node, _k, names in os.walk(SCRIPTS3):
        for n in names:
            if not n.endswith('.py'):
                continue
            content = io.open(os.path.join(root_node, n), encoding='utf-8',
                            errors='replace').read()
            for m in formula.finditer(content):
                name = m.group(1)
                found_items.setdefault(name.lower(), name)
    return found_items


def main():
    """Takes the UIScript window descriptions from the user's unpacked packs and
    rewrites them to Python 3 for the data set.
    """
    if not os.path.exists(PYTHON_2TO3):
        print('no Python with lib2to3 found: ' + PYTHON_2TO3)
        return 1

    # SOURCE: THE USER'S CLIENT. Window descriptions from any other
    # build may be CHANGED (e.g. a `systemoptiondialog.py` with a "display mode"
    # section with the keys
    # `localeInfo.OPTION_DISPLAY`, which exist neither in the client's locale nor
    # in our `uiSystemOption.py`) - opening "System options" ended in
    # `exception.Abort()` -> `sys.exit()` -> dangling window handles
    # -> a crash. The game scripts (`scripts3`) are from the user's client, so
    # the window descriptions have to be from there too: one version, not a mix.
    CLIENT_UISCRIPT = os.path.join(workspace.GAME_FILES, 'uiscript')
    if not os.path.isdir(CLIENT_UISCRIPT):
        print('no %s - run: python tools/build_corpus.py --unpack-only' % CLIENT_UISCRIPT)
        return 1
    source_items = ['uiscript/' + n for n in sorted(os.listdir(CLIENT_UISCRIPT))
                    if os.path.isfile(os.path.join(CLIENT_UISCRIPT, n))]
    read_file = lambda n: io.open(os.path.join(CLIENT_UISCRIPT, n.split('/')[-1]), 'rb').read()
    print('source: the user client (%s)' % os.path.relpath(CLIENT_UISCRIPT, ROOT))
    if not source_items:
        print('the source has no uiscript/ directory')
        return 1

    spelling = spellings_from_client()
    print('name spellings found in the game scripts: %d' % len(spelling))

    # Target from scratch - a file removed from the source must not stay in the result.
    if os.path.isdir(TARGET):
        shutil.rmtree(TARGET)
    directory = os.path.join(TARGET, DIRECTORY)
    os.makedirs(directory)

    without_spelling = []
    for name in source_items:
        flat = name.split('/')[-1]
        target_encoding = spelling.get(flat.lower(), flat)
        if flat.lower() not in spelling:
            without_spelling.append(flat)

        data_bytes = read_file(name)

        # NOT EVERYTHING IN THIS DIRECTORY IS CODE.
        #
        # Next to the window descriptions lie the files `936_desc_warrior.txt`,
        # `949_desc_warrior.txt` and so on - character class descriptions, one
        # set PER CODE PAGE (936 is simplified Chinese,
        # 949 Korean). The client reads them as data and knows by itself how to
        # decode them; converting them to UTF-8 would break exactly what
        # once already broke the language files.
        #
        # So they go BYTE FOR BYTE.
        if not flat.lower().endswith('.py'):
            io.open(os.path.join(directory, target_encoding), 'wb').write(data_bytes)
            continue

        try:
            content = data_bytes.decode('utf-8')
        except UnicodeDecodeError:
            # The same order as in rewrite_scripts.py: UTF-8 first,
            # only then CP949. A file that already is UTF-8 stays
            # untouched.
            content = data_bytes.decode('cp949')
        io.open(os.path.join(directory, target_encoding), 'w', encoding='utf-8',
                newline=chr(10)).write(content)

    print('extracted from the corpus: %d files' % len(source_items))
    if without_spelling:
        print('  without a spelling in the code (the corpus one stays): %d'
              % len(without_spelling))

    r = subprocess.run(
        [PYTHON_2TO3, '-m', 'lib2to3', '-w', '-n', '--no-diffs', directory],
        capture_output=True, text=True)
    print('files rewritten by lib2to3: %d'
          % sum(1 for l in r.stderr.splitlines() if 'Refactored ' in l))

    # --- division --------------------------------------------------------
    integer_total, kept_total = 0, 0
    all_names = set()
    for n in sorted(os.listdir(directory)):
        if not n.endswith('.py'):
            continue
        path_str = os.path.join(directory, n)
        content = io.open(path_str, encoding='utf-8', newline='').read()
        new_value, integer_count, kept, names = division.convert_divisions(content)
        integer_total += integer_count
        kept_total += kept
        all_names |= names
        if integer_count:
            io.open(path_str, 'w', encoding='utf-8',
                    newline=chr(10)).write(new_value)

    print('division: %d to `//`, %d left as `/`'
          % (integer_total, kept_total))
    print('  names used as an operand of integer division: %s'
          % ', '.join(sorted(all_names)))

    # --- check ------------------------------------------------------------
    # These files are EXECUTED by the client, so the syntax must be right.
    # I check it here, not in the browser - a syntax error in a window
    # description shows up there as "Failed to load script file" and nothing more.
    bad = []
    for n in sorted(os.listdir(directory)):
        if not n.endswith('.py'):
            continue
        content = io.open(os.path.join(directory, n), encoding='utf-8').read()
        try:
            compile(content, n, 'exec')
        except SyntaxError as e:
            bad.append('%s: %s' % (n, e))

    print('syntax check: %d files, errors %d' % (len(source_items), len(bad)))
    for w in bad[:10]:
        print('  ' + w)

    print('result in: ' + os.path.relpath(TARGET, ROOT))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
