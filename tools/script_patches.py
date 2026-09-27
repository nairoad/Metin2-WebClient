#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""script_patches.py - what `lib2to3` will NOT do.

`lib2to3` rewrites **syntax**. What remains are differences that are
changes in the behaviour of the standard library or of the language itself -
and no tool detects those, because syntactically the code is correct. They
come out only when run, one at a time.

Every patch below comes from an **observed error** in the running client,
not from a list of "what changed in Python 3". Next to each it is written
how it showed up.

THE RULE THAT APPLIES HERE
==========================
Every patch MUST hit. If a pattern is not found, the script stops - the
same as `PATCHES` in `stage_port.py`. A silent patch that did not hit is
worse than none: it looks done.
"""

import io
import os
import sys

NL = chr(10)
TAB = chr(9)

OLD_IMPORT_EXCEPTIONS = '\t\timport exception\n\t\timport exceptions\n'

NEW_IMPORT_EXCEPTIONS = '\t\timport exception\n\t\t# PORT: `exceptions` was a BUILT-IN module of Python 2.\n\t\t#\n\t\t# It held `Exception`, `IOError`, `RuntimeError` and the rest -\n\t\t# the same names that were visible anyway without an import.\n\t\t# In Python 3 it is gone, because it was redundant already then.\n\t\t#\n\t\t# In this function nobody refers to it even once -\n\t\t# checked, one occurrence in the whole file, this one.\n\t\t# So there is nothing to replace, only something to remove.\n\t\t#\n\t\t# Symptom: ModuleNotFoundError: No module named "exceptions"\n\t\t# at the first dialog window the client tried\n\t\t# to build from a script (UIScript/PopupDialog.py).\n'

OLD_LOADSCRIPT_EXEC = '\t\t\texec(compile(open(FileName, "rb").read(), FileName, \'exec\'), self.ScriptDictionary)'

NEW_LOADSCRIPT_EXEC = '\t\t\t# PORT: LETTER CASE IN THE FILE NAME.\n\t\t\t#\n\t\t\t# The game scripts ask for the same window descriptions in two\n\t\t\t# spellings - "UIScript/TaskBar.py" and\n\t\t\t# "uiscript/questiondialog.py". On Windows that was the same\n\t\t\t# and nobody had to watch it; in the browser file\n\t\t\t# system these are two different paths and one of them\n\t\t\t# does not exist.\n\t\t\t#\n\t\t\t# Fixing the spelling in dozens of places in the game code\n\t\t\t# would be fixing the SYMPTOM. The cause is one: these names\n\t\t\t# were written for a file system that did not distinguish\n\t\t\t# letter case. So I restore that property - here, in the only\n\t\t\t# place through which these files are opened.\n\t\t\t#\n\t\t\t# I build the path part by part: each I check directly,\n\t\t\t# and when it is missing - I look in the directory for a name differing only\n\t\t\t# in letter case. When there is none either, I leave the name\n\t\t\t# as it was, so that the error says what it should: "no such\n\t\t\t# file", not something about a directory.\n\t\t\tif not os.path.exists(FileName):\n\t\t\t\t_p_joined = ""\n\t\t\t\tfor _p_part in FileName.replace("\\\\", "/").split("/"):\n\t\t\t\t\t_p_step = _p_joined + _p_part if not _p_joined else _p_joined + "/" + _p_part\n\t\t\t\t\tif os.path.exists(_p_step):\n\t\t\t\t\t\t_p_joined = _p_step\n\t\t\t\t\t\tcontinue\n\t\t\t\t\ttry:\n\t\t\t\t\t\t_p_listing = os.listdir(_p_joined if _p_joined else ".")\n\t\t\t\t\texcept OSError:\n\t\t\t\t\t\t_p_listing = []\n\t\t\t\t\t_p_matches = [_p_n for _p_n in _p_listing if _p_n.lower() == _p_part.lower()]\n\t\t\t\t\tif not _p_matches:\n\t\t\t\t\t\t# No such part in any spelling - I leave the\n\t\t\t\t\t\t# name UNTOUCHED. Gluing half of a resolved\n\t\t\t\t\t\t# path to an unresolved rest would give a name\n\t\t\t\t\t\t# nobody wrote.\n\t\t\t\t\t\t_p_joined = FileName\n\t\t\t\t\t\tbreak\n\t\t\t\t\t_p_joined = (_p_matches[0] if not _p_joined else _p_joined + "/" + _p_matches[0])\n\t\t\t\tFileName = _p_joined\n\t\t\t\tprint("===== Load Script File (by letter case) : %s" % (FileName))\n\n\t\t\texec(compile(open(FileName, "rb").read(), FileName, \'exec\'), self.ScriptDictionary)'

OLD_LOCALE_ERROR = '\t\timport locale\n\t\tif locale.error:\n\t\t\tmsg = locale.error.get(msg, msg)'

NEW_LOCALE_ERROR = '\t\timport locale\n\t\t# PORT: ERROR HANDLING THAT OVERTURNED ITSELF.\n\t\t#\n\t\t# `locale.error` was meant to be a DICTIONARY of message translations\n\t\t# - that shows from `.get(msg, msg)` one line below. Such a dictionary\n\t\t# could come only from an own `locale.py` of the game, and there is no such\n\t\t# file in the packs (checked: 54955 names in the corpus,\n\t\t# none is `locale.py`). So `import locale` reaches the\n\t\t# standard library, which has `Error` - a class, not\n\t\t# a dictionary - and no `error`.\n\t\t#\n\t\t# This is not a difference between Python 2 and 3. It is a bug\n\t\t# that was the same in the original - only it sits in the place\n\t\t# where it hurts most: IN ERROR HANDLING. Instead of showing\n\t\t# the `RuntimeError` that got here, the client showed\n\t\t#\n\t\t#     AttributeError: module "locale" has no attribute "error"\n\t\t#\n\t\t# i.e. it reported a failure of its own failure report. The same\n\t\t# shape as `Traceback()`.\n\t\t#\n\t\t# `getattr` instead of cutting it all out, because if someone ever\n\t\t# added a `locale.py` with that dictionary, the translation is to work.\n\t\terror_lookup = getattr(locale, "error", None)\n\t\tif error_lookup:\n\t\t\tmsg = error_lookup.get(msg, msg)'

# (file, pattern, replacement, why)
PATCHES_SCRIPTS = [
    # -------------------------------------------------------------------
    # SYMPTOM:
    #     AttributeError: 'TraceFile' object has no attribute 'flush'
    #     Exception ignored on flushing sys.stdout
    #
    # Python 3 flushes `sys.stdout` and `sys.stderr` at shutdown, so it
    # requires them to have a `flush` method. Python 2 did not do that.
    #
    # The effect was worse than the message itself: THE TRACEBACK OF THE REAL ERROR
    # could not be printed, because printing went through the same objects.
    # The log was left with an empty "Traceback:" and nothing more - i.e. the
    # failure hid its own cause.
    (
        'system.py',
        'class TraceFile:' + NL +
        TAB + 'def write(self, msg):' + NL +
        TAB + TAB + 'dbg.Trace(msg)',

        'class TraceFile:' + NL +
        TAB + 'def write(self, msg):' + NL +
        TAB + TAB + 'dbg.Trace(msg)' + NL + NL +
        TAB + '# PORT: Python 3 flushes sys.stdout at shutdown and requires' + NL +
        TAB + '# a `flush` method. Without it the traceback of the real error could' + NL +
        TAB + '# not be printed - the failure hid its own cause.' + NL +
        TAB + 'def flush(self):' + NL +
        TAB + TAB + 'pass',

        'sys.stdout must have flush in Python 3',
    ),
    (
        'system.py',
        'class TraceErrorFile:' + NL +
        TAB + 'def write(self, msg):' + NL +
        TAB + TAB + 'dbg.TraceError(msg)' + NL +
        TAB + TAB + 'dbg.RegisterExceptionString(msg)',

        'class TraceErrorFile:' + NL +
        TAB + 'def write(self, msg):' + NL +
        TAB + TAB + 'dbg.TraceError(msg)' + NL +
        TAB + TAB + 'dbg.RegisterExceptionString(msg)' + NL + NL +
        TAB + '# PORT: as above, for sys.stderr.' + NL +
        TAB + 'def flush(self):' + NL +
        TAB + TAB + 'pass',

        'sys.stderr must have flush in Python 3',
    ),
    (
        'system.py',
        TAB + 'def show(self):' + NL +
        TAB + TAB + 'dbg.LogBox(self.msg,"Error")',

        TAB + 'def show(self):' + NL +
        TAB + TAB + 'dbg.LogBox(self.msg,"Error")' + NL + NL +
        TAB + '# PORT: `LogBoxFile` also ends up as sys.stderr.' + NL +
        TAB + 'def flush(self):' + NL +
        TAB + TAB + 'pass',

        'LogBoxFile is also put in place of sys.stderr',
    ),

    # -------------------------------------------------------------------
    # The `imp` module disappeared in Python 3.12. Both uses have exact
    # equivalents, so this is not an approximation:
    #
    #     imp.new_module(name)   ->  types.ModuleType(name)
    #     imp.get_magic()        ->  importlib.util.MAGIC_NUMBER
    #
    # `MAGIC_NUMBER` is the same four-byte marker that
    # stands at the start of a `.pyc` file - so the comparison in `system.py`
    # keeps its meaning.
    (
        'system.py',
        'import marshal' + NL + 'import imp' + NL + 'import pack',
        'import marshal' + NL +
        '# PORT: the `imp` module is gone in Python 3.12. Both its uses' + NL +
        '# in this file have exact equivalents - see below.' + NL +
        'import types' + NL +
        'import importlib.util' + NL +
        'import pack',
        'the imp module removed in Python 3.12',
    ),
    (
        'system.py',
        'module = imp.new_module(fqname)',
        'module = types.ModuleType(fqname)',
        'imp.new_module -> types.ModuleType',
    ),
    (
        'system.py',
        'if kFile.read(4)!=imp.get_magic():',
        'if kFile.read(4)!=importlib.util.MAGIC_NUMBER:',
        'imp.get_magic -> importlib.util.MAGIC_NUMBER',
    ),
    (
        'system.py',
        'def __pack_import(name,globals=None,locals=None,fromlist=None):',
        '# PORT: Python 3 calls `__import__` with FIVE arguments - it added\n# `level`, which says whether the import is relative (`from . import x`).\n# Python 2 had four.\n#\n# The symptom was clear only after fixing `Traceback()`:\n#     TypeError: __pack_import() takes from 1 to 4 positional\n#     arguments but 5 were given\n#\n# `level` has to be PASSED ON, not just accepted.\n#\n# The first version of this patch only accepted it, reasoning\n# that the game scripts have no relative imports. That was true of\n# the game scripts and UNTRUE of the whole: this hook intercepts EVERY\n# import, also those from the standard library. And `re/__init__.py` does\n# `from . import _compiler, _parser`.\n#\n# Symptom: ValueError: Empty module name - because a relative import has an empty\n# name and all of its content sits in `level`, which we lost on the way.\ndef __pack_import(name,globals=None,locals=None,fromlist=None,level=0):',
        'Python 3 calls __import__ with five arguments (level was added)',
    ),
    (
        'system.py',
        '\t\treturn old_import(name,globals,locals,fromlist)',
        '\t\t# PORT: `level` MUST go on - see the note at `__pack_import`.\n\t\treturn old_import(name,globals,locals,fromlist,level)',
        'level has to be passed to old_import, otherwise a relative import (from . import x) ends with "Empty module name"',
    ),
    (
        'system.py',
        'def __pack_import(name,globals=None,locals=None,fromlist=None,level=0):\n\tif name in sys.modules:',
        'def __pack_import(name,globals=None,locals=None,fromlist=None,level=0):\n\t# PORT: DOTTED NAMES AND RELATIVE IMPORTS ARE LEFT TO CPYTHON.\n\t#\n\t# The game pack holds only FLAT names - `game.py`, `uiTooltip.py`,\n\t# no packages. A dot in a name therefore always means the standard\n\t# library, and there this hook has nothing to look for.\n\t#\n\t# And it can do harm. `__import__` has a rule this hook\n\t# did not reproduce: with an empty `fromlist` it returns the TOP-LEVEL\n\t# module, not the one after the dot. For `import collections.abc`\n\t# `collections` is to come out, and `collections.abc` came out -\n\t# and the standard library got a different module than it asked for:\n\t#\n\t#     AttributeError: module "collections.abc" has no attribute\n\t#     "namedtuple"\n\t#\n\t# Reproducing that rule here would be rewriting a piece\n\t# of CPython. It is cheaper and safer to ask it to do it.\n\tif level or "." in name:\n\t\treturn old_import(name,globals,locals,fromlist,level)\n\n\tif name in sys.modules:',
        'dotted and relative imports left to CPython - the hook did not reproduce the top-level module rule',
    ),
    (
        'system.py',
        'class pack_file_iterator(object):\n\tdef __init__(self, packfile):\n\t\tself.pack_file = packfile',
        "class pack_file_iterator(object):\n\tdef __init__(self, packfile):\n\t\tself.pack_file = packfile\n\n\t# PORT: in Python 3 an ITERATOR must also have `__iter__`.\n\t#\n\t# Python 2 was content with `next`. Python 3 checks the full protocol\n\t# and without this method `for x in file` ends with\n\t#     TypeError: 'pack_file_iterator' object is not iterable\n\t#\n\t# Returning `self` is not a trick, but the content of the protocol:\n\t# an iterator IS its own source.\n\tdef __iter__(self):\n\t\treturn self",
        'in Python 3 an iterator must also have __iter__ - without it the loop says it is not iterable',
    ),
    # -------------------------------------------------------------------
    # SYMPTOM:
    #     ui.py(line:2747) LoadScriptFile - import exceptions
    #     <class 'ModuleNotFoundError'>:No module named 'exceptions'
    #
    # It stopped the client at the FIRST dialog window - and so at
    # the whole interface, because every window goes through `LoadScriptFile`.
    (
        'ui.py',
        OLD_IMPORT_EXCEPTIONS,
        NEW_IMPORT_EXCEPTIONS,
        'the exceptions module was built into Python 2 and is gone in 3; this function never used it',
    ),
    # -------------------------------------------------------------------
    # SYMPTOM:
    #     Failed to load script file : UIScript/PopupDialog.py
    #     [Errno 44] No such file or directory: 'UIScript/PopupDialog.py'
    #
    # The file WAS there - as `uiscript3/UIScript/PopupDialog.py`.
    # What did not match was the case of another call, which asks
    # for `uiscript/popupdialog.py`. Both spellings are in the game code.
    (
        'ui.py',
        OLD_LOADSCRIPT_EXEC,
        NEW_LOADSCRIPT_EXEC,
        'the game scripts ask for window descriptions in two spellings; Windows did not distinguish letter case, wasm does',
    ),
    # -------------------------------------------------------------------
    # SYMPTOM:
    #     AttributeError: module 'locale' has no attribute 'error'.
    #     Did you mean: 'Error'?
    #
    # It came out only when the first real `RuntimeError` reached `RunMainScript`
    # (a missing interface texture).
    (
        'system.py',
        OLD_LOCALE_ERROR,
        NEW_LOCALE_ERROR,
        'locale.error was meant to be a translation dictionary from an own locale.py of the game, which the packs do not have - the error handling overturned itself',
    ),
    # -------------------------------------------------------------------
    # SYMPTOM (the user's report: "after closing a quest a window
    # remains that cannot be closed"):
    #     File "uiQuest.py", line 342, in CloseSelf
    #     File "uiQuest.py", line 352, in Destroy
    #     File "uiQuest.py", line 410, in <lambda>
    #     NameError: name 'apply' is not defined
    #
    # `apply` is a built-in of Python 2, removed in Python 3. `lib2to3`
    # has a fixer for it, but only for the CALL `apply(f, args)`;
    # here `apply` is passed as a VALUE to `map`, so it stayed.
    # The lambda is meant to call all the collected close functions in turn
    # without arguments - exactly what the list expression does.
    (
        'uiQuest.py',
        TAB + TAB + TAB + 'self.OnCloseEvent = lambda z=[self.OnCloseEvent, f]:list(map(apply,z))',
        TAB + TAB + TAB + 'self.OnCloseEvent = lambda z=[self.OnCloseEvent, f]:[fn() for fn in z]  # PORT: apply does not exist in Python 3',
        'apply passed as a value to map - lib2to3 does not rewrite that; the quest window could not be closed',
    ),
    # -------------------------------------------------------------------
    # SYMPTOM (after a map change: the picture freezes, syserr every frame):
    #     File "uiChat.py", line 104, in OnRender
    #     TypeError: '>=' not supported between instances of 'NoneType' and 'int'
    #
    # `ChatButton.__init__` sets `self.state = None`, and `OnRender`
    # compares `self.state >= BUTTON_STATE_OVER`. In Python 2 `None >= 1`
    # is False (like the UP state); in Python 3 it is a TypeError every frame, until
    # the mouse moves over the button. The initial state = UP gives the same
    # as Python 2 computed for None.
    (
        'uiChat.py',
        TAB + TAB + 'self.state = None\n' + TAB + TAB + 'self.buttonText = None',
        TAB + TAB + 'self.state = self.BUTTON_STATE_UP  # PORT: in Py2 None >= int was False, in Py3 it is a TypeError\n' + TAB + TAB + 'self.buttonText = None',
        'None >= int raises TypeError in Python 3 - the chat button overturned the interface render every frame',
    ),
    # -------------------------------------------------------------------
    # syserr: `localeInfo.py:34: SyntaxWarning: invalid escape
    # sequence '\['` - in Py2 an unknown escape stayed literal, in Py3 it is a
    # warning at every start. The same string in raw form.
    (
        'localeInfo.py',
        "VIRTUAL_KEY_NUMBERS    = \"1234567890-=\\[];',./`\"",
        "VIRTUAL_KEY_NUMBERS    = r\"1234567890-=\\[];',./`\"  # PORT: a raw string, no SyntaxWarning",
        'an unknown escape in a string - SyntaxWarning in Python 3 at every start',
    ),
]


# ---------------------------------------------------------------------------
# GENERAL RULES - mechanical replacements, not point patches
# ---------------------------------------------------------------------------
# The PATCHES_SCRIPTS table describes places; these rules describe NAMES. Only
# replacements that are unambiguous in the whole language and need no reading
# of the context belong here - otherwise they would go into the table.
#
# (old, new, why)
GENERAL_RULES = [
    (
        'time.clock()',
        'time.perf_counter()',
        'time.clock() REMOVED IN PYTHON 3.8. The documentation names '
        'perf_counter() as the replacement for measuring elapsed time, and every '
        'use in the game scripts measures exactly that (the countdown in the login '
        'window, fading the quest curtain). Symptom: '
        'AttributeError: module "time" has no attribute "clock" when '
        'opening the quest window - and a dead main loop, because the exception '
        'interrupted drawing the interface.',
    ),
]


def general_rules(directory):
    """Applies the mechanical replacements to ALL scripts. Returns the count."""
    how_many = 0
    for old_ones, new_ones, why_needed in GENERAL_RULES:
        file_count_n = 0
        occurrences = 0
        for root_node, _dir, file_list in os.walk(directory):
            for name in file_list:
                if not name.endswith('.py'):
                    continue
                path_str = os.path.join(root_node, name)
                content = io.open(path_str, encoding='utf-8', newline='').read()
                n = content.count(old_ones)
                if not n:
                    continue
                io.open(path_str, 'w', encoding='utf-8', newline='').write(
                    content.replace(old_ones, new_ones))
                file_count_n += 1
                occurrences += n
        if occurrences:
            print('  rule: %s -> %s  (%d occurrences in %d files) <- %s'
                  % (old_ones, new_ones, occurrences, file_count_n, why_needed.split('.')[0]))
            how_many += occurrences
    return how_many


def apply_all(directory):
    """Applies the patches. Returns how many were applied, or stops."""
    how_many = general_rules(directory)
    for name, old_ones, new_ones, why_needed in PATCHES_SCRIPTS:
        path_str = os.path.join(directory, name)
        if not os.path.exists(path_str):
            raise SystemExit('a patch for a file that does not exist: ' + name)

        content = io.open(path_str, encoding='utf-8', newline='').read()

        # LINE ENDINGS ARE MIXED AND THAT IS NOT OUR BUSINESS.
        #
        # The game scripts came from Windows and some of them have CRLF, and some
        # LF only - `system.py` one, `ui.py` the other. The patterns below are written
        # in LF, because that is how they are read.
        #
        # Instead of requiring the patch author to know which file it is,
        # the pattern is matched TO THE FILE. The replacement too - otherwise a patch
        # would insert lines with a different ending than the rest into the middle of a file.
        if old_ones not in content and (chr(13) + NL) in content:
            old_ones = old_ones.replace(NL, chr(13) + NL)
            new_ones = new_ones.replace(NL, chr(13) + NL)

        if new_ones in content:
            print('  already there: %s <- %s' % (name, why_needed))
            continue

        if old_ones not in content:
            raise SystemExit(
                'PATCH DID NOT HIT: %s <- %s%s'
                'The pattern does not occur in the file. The source has changed or'
                ' the patch is no longer needed.' % (name, why_needed, NL))

        content = content.replace(old_ones, new_ones, 1)
        io.open(path_str, 'w', encoding='utf-8', newline='').write(content)
        print('  patch: %s <- %s' % (name, why_needed))
        how_many += 1

    return how_many


if __name__ == '__main__':
    directory = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        'build', 'port', 'scripts3')
    print('patches applied: %d' % apply_all(directory))
