#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_client_data.py - assembles a SMALL data set for the wasm file system.

WHY NOT EVERYTHING
==================
The client in `reference/Client` is 1.7 GB, of which 1.4 GB are the `pack/` packs.
Emscripten can pull them all in through `--preload-file`, but
the result would be a data file the browser has to download **in full
before the first frame**. That is not how the real port works
- it fetches packs as needed.

So this script assembles a MINIMAL set: as much as needed for the client
to pass its own start-up checks and get further. Every file is
here for a reason that can be pointed at in the code:

  lib/*.pyc      `CheckPythonLibraryFilenames` (UserInterface.cpp:80)
                 checks eleven names through `_access`. Without them the client
                 ends with the message "Python Library file not exist".
  locale.cfg     `LocaleService_LoadConfig` reads it right after start -
                 hence the version number, code page and language.
  locale_*.cfg   the same file for the other languages; they cost bytes.
  metin2.cfg     display settings (resolution, depth).
  mouse.cfg      key bindings.
  channel.inf    server list.

A NOTE THAT WILL MATTER LATER
=============================
The `lib/*.pyc` files are **Python 2.7 bytecode** - from the original client.
The port targets CPython 3.14, which will not
load this code: the file header, the magic number and the instruction set itself changed.

Here it does not matter yet, because `CheckPythonLibraryFilenames` calls
`_access`, i.e. asks ONLY whether the file exists - it does not open it. But when
the real CPython comes, the scripts will have to be recompiled from
source. I write it down here so it does not come as a surprise.
"""

import io
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
CLIENT = workspace.CLIENT
TARGET = os.path.join(ROOT, 'build', 'port', 'data')

# Single files from the client's root.
FILES = ('locale.cfg', 'metin2.cfg', 'mouse.cfg', 'channel.inf')

# Directories copied whole. Only small ones - see the note at the top.
DIRECTORIES = ('lib',)




# ===========================================================================
# The Python standard library
# ===========================================================================
# `Py_Initialize` WILL NOT START without it. The symptom is unambiguous and shows
# in the client's `syserr.txt`:
#
#     Fatal Python error: Failed to import encodings module
#     ModuleNotFoundError: No module named 'encodings'
#
# CPython looks for it under `sys.prefix`, which in this build is
# `/usr/local` - hence this path in the virtual file system.
#
# WHAT I THROW OUT AND WHY
# ------------------------
# The whole `Lib/` is 49 MB, and the browser has to download this data BEFORE the first
# frame. I throw out directories the game has no way to use:
#
#   test, tests, idlelib, turtledemo  CPython tools and examples
#   tkinter                           a windowing interface that is not here
#   ensurepip, venv                   installing packages
#   lib2to3                           rewriting code from Python 2 to 3
#   __pycache__                       bytecode that will be created anew anyway
#
# I throw out nothing "by eye from the name". Every item on this list is a
# CPython developer tool, not a part of the language.
# WHERE TO TAKE IT FROM: the Temp directory gets cleaned - on 2026-09-19
# only empty folders were left in it, the script packed "0 .py files" and the client on the site
# died at start with a black screen. Hence the order:
#   1. M2W_PYTHON_LIB (an explicitly given Lib/ directory),
#   2. Lib/ of the CPython sources in Temp (if it EXISTS and has encodings/),
#   3. Lib/ of the Python running this script - provided it is 3.13 (the same
#      series as the one built into client.wasm; the stdlib .py files are compatible within
#      a series).
# An empty result is a build ERROR, not a warning.
def _find_stdlib():
    """The CPython 3.13 `Lib/` to pack: M2W_PYTHON_LIB,
    then the sources in Temp, then this interpreter's stdlib when it is 3.13 -
    the first one with `encodings/`; else the first candidate (the caller then
    reports an empty result as an error).
    """
    import sysconfig
    candidates = []
    if os.environ.get('M2W_PYTHON_LIB'):
        candidates.append(os.environ.get('M2W_PYTHON_LIB'))
    candidates.append(os.path.join(os.path.expanduser('~'), 'AppData', 'Local', 'Temp',
                                  'claude', 'Python-3.13.15', 'Lib'))
    if sys.version_info[:2] == (3, 13):
        candidates.append(sysconfig.get_paths()['stdlib'])
    for k in candidates:
        if os.path.isfile(os.path.join(k, 'encodings', '__init__.py')):
            return k
    return candidates[0]


STDLIB_SOURCE = _find_stdlib()

# Where CPython looks for it - see `sys.prefix` above.
STDLIB_TARGET = os.path.join('usr', 'local', 'lib', 'python3.13')

STDLIB_SKIPPED = ('test', 'tests', 'idlelib', 'turtledemo', 'tkinter',
                   'ensurepip', 'venv', 'lib2to3', '__pycache__',
                   'site-packages', 'distutils')


def copy_stdlib():
    """Returns the number of copied files, or -1 when the source is missing."""
    if not os.path.isdir(STDLIB_SOURCE):
        return -1

    target = os.path.join(TARGET, STDLIB_TARGET)
    how_many = 0

    for directory, subdirs, file_list in os.walk(STDLIB_SOURCE):
        # I cut the skipped ones IN PLACE, so that `os.walk` does not enter them -
        # otherwise it would go through thousands of files only to
        # reject them by name.
        subdirs[:] = [p for p in subdirs if p not in STDLIB_SKIPPED]

        relative_one = os.path.relpath(directory, STDLIB_SOURCE)
        if relative_one == '.':
            relative_one = ''

        target_path = os.path.join(target, relative_one) if relative_one else target
        os.makedirs(target_path, exist_ok=True)

        for name in file_list:
            if not name.endswith('.py'):
                continue
            shutil.copy2(os.path.join(directory, name),
                         os.path.join(target_path, name))
            how_many += 1

    return how_many




# ===========================================================================
# Game scripts, rewritten to Python 3
# ===========================================================================
# Placed as LOOSE FILES, not back into the pack - and that is not
# a shortcut, but using what the client can do anyway.
#
# `CEterPackManager` starts in `SEARCH_FILE_FIRST` mode (see its
# constructor): it looks for a file on disk first, only then in the pack.
# A rewritten `system.py` placed alongside therefore wins over the one from `root.epk`
# without changing a single line in the game code and without repacking.
#
# Thanks to that the original pack stays UNTOUCHED and one can at any moment
# compare what changed.
SCRIPTS = os.path.join(ROOT, 'build', 'port', 'scripts3')


def copy_scripts():
    """Returns the number of copied files, or -1 when the source is missing."""
    if not os.path.isdir(SCRIPTS):
        return -1

    how_many = 0
    for root_node, _dir, file_list in os.walk(SCRIPTS):
        relative_one = os.path.relpath(root_node, SCRIPTS)
        target_path = TARGET if relative_one == '.' else os.path.join(TARGET, relative_one)
        os.makedirs(target_path, exist_ok=True)

        for name in file_list:
            shutil.copy2(os.path.join(root_node, name),
                         os.path.join(target_path, name))
            how_many += 1
    return how_many




# ===========================================================================
# All game files in the package (`--with-corpus`, tests only)
# ===========================================================================
# The user's packs unpacked (`build_corpus.py --unpack-only`). Placed the same
# way as the scripts - as loose files, because `CEterPackManager` starts
# in SEARCH_FILE_FIRST mode.
#
# The set is SMALL and that is intended: it consists of what the client ITSELF
# reported as missing. The compatibility layer can write a log
# (`?missing=1` in the URL creates `/missing.txt`), so guessing what it
# needs was replaced by measurement.
CORPUS = workspace.GAME_FILES


# What we do NOT take into the client package and why. Each item is
# a decision with a reason, not "because it is big" - and each can be undone with one
# deletion.
#
# This came about, when the package grew to 661 MB. The browser
# loads it WHOLE into memory before the first frame, so size is not
# a matter of convenience here, but of whether the client starts at all.
SKIPPED_EXTENSIONS = {
    '.mp3': 'music - no sound in the port yet (80 MB)',
    '.atr': 'terrain attributes (collisions) - the terrain is not drawn yet (74 MB)',
    '.wtr': 'water - as above (18 MB)',
    '.mde': 'map editor helper meshes - the game does not read them (19 MB)',
    '.bmp': 'uncompressed images, every one has a counterpart (12 MB)',
}

# Languages. The client uses one, and the corpus carries a dozen - the text
# images of the foreign versions alone are 114 MB.
LANGUAGES_WE_TAKE = ('locale/en/', 'locale/we/')


# DO WE SKIP ANYTHING AT ALL
# ====================================
# By default NO. The user's decision: "extract the whole game, don't limit it.
# If something is not going to load, it won't load."
#
# The reason is stronger than convenience, and it is the history of this project.
# A missing file does not report itself as missing - it reports itself as an error
# somewhere else: `.gr2` outside the package gave "characters are not visible" and thirty
# rounds of searching in the drawing, `.dds` outside the package gives white
# characters. Every such limitation buys megabytes at the price of
# a false trail, and a false trail costs more.
#
# The lists stay, because they describe what weighs how much. `M2W_LIMIT=1`
# in the environment brings them back into effect.
LIMITED = os.environ.get('M2W_LIMIT') == '1'


def skipping(relative_one):
    """Does this file stay out of the package? Returns the reason or None."""
    if not LIMITED:
        return None
    m = relative_one.replace(os.sep, '/').lower()
    extension_name = '.' + m.rsplit('.', 1)[-1] if '.' in m else ''
    if extension_name in SKIPPED_EXTENSIONS:
        return SKIPPED_EXTENSIONS[extension_name]
    if m.startswith('locale/') and not m.startswith(LANGUAGES_WE_TAKE):
        return 'a language version the client does not use'
    return None


def copy_corpus():
    """Returns the number of copied files, or -1 when the source is missing."""
    if not os.path.isdir(CORPUS):
        return -1

    how_many = 0
    skipped = 0
    overrides = 0
    bytes_skipped = 0
    for root_node, _dir, file_list in os.walk(CORPUS):
        relative_one = os.path.relpath(root_node, CORPUS)
        target_path = TARGET if relative_one == '.' else os.path.join(TARGET, relative_one)
        os.makedirs(target_path, exist_ok=True)
        for name in file_list:
            source = os.path.join(root_node, name)
            relative_file = os.path.relpath(source, CORPUS)
            if skipping(relative_file):
                skipped += 1
                try:
                    bytes_skipped += os.path.getsize(source)
                except OSError:
                    pass
                continue
            target = os.path.join(target_path, name)
            # WHAT WAS PREPARED TAKES PRECEDENCE. The corpus is copied
            # LAST, so without this check it overwrites everything that
            # was created earlier - and among other things the game scripts
            # rewritten to Python 3 were created there.
            #
            # It happened twice. The first time (`locale/en`
            # took the corrected texts and broke logging in), the second:
            # `system.py` came back in the Python 2 version and the client stopped at
            # "SyntaxError: unterminated string literal" - i.e. at something
            # that looks like an error in a script, but is an error of ORDER.
            #
            # The rule now sits in the code, not in memory.
            if os.path.exists(target):
                overrides += 1
                continue
            shutil.copy2(source, target)
            how_many += 1
    if overrides:
        print('the corpus gave way to prepared files: %d times' % overrides)
    if skipped:
        print('deliberately skipped: %d files, %.1f MB'
              % (skipped, bytes_skipped / 1048576.0))
        for e, why_needed in sorted(SKIPPED_EXTENSIONS.items()):
            print('    %-6s %s' % (e, why_needed))
        print('    %-6s %s' % ('locale', 'only ' +
                               ', '.join(LANGUAGES_WE_TAKE)))
    return how_many




# ===========================================================================
# Interface window descriptions
# ===========================================================================
# `tools/uiscript_from_client.py` takes them from the unpacked packs, rewrites them to
# Python 3 and decides the division - without that every window coordinate
# would be a fraction, and the bridge to C accepts only integers.
#
# They go separately from the game scripts, because they come from a different source and a different
# tool. In the data set they land next to each other.
UISCRIPT = os.path.join(ROOT, 'build', 'port', 'uiscript3')


def copy_uiscript():
    """Returns the number of copied files, or -1 when the source is missing."""
    if not os.path.isdir(UISCRIPT):
        return -1

    how_many = 0
    for root_node, _dir, file_list in os.walk(UISCRIPT):
        relative_one = os.path.relpath(root_node, UISCRIPT)
        target_path = TARGET if relative_one == '.' else os.path.join(TARGET, relative_one)
        os.makedirs(target_path, exist_ok=True)
        for name in file_list:
            shutil.copy2(os.path.join(root_node, name),
                         os.path.join(target_path, name))
            how_many += 1
    return how_many


# ---------------------------------------------------------------------------
# THE GAME SERVER ADDRESS - THE ONLY PLACE WHERE IT IS CHANGED
# ---------------------------------------------------------------------------
# `serverInfo.py` carries the address the client was built for. That is a
# DEPLOYMENT setting, not game content - and that is why it is not in
# `script_patches.py`: that table
# describes differences between Python 2 and 3, and every item has a justification
# in the code. An address in someone's home network is not such a justification.
#
# Put your address here. `build_client_data.py` assembles `data/` from scratch on every
# run (`shutil.rmtree`), so a fix written directly into
# `build/port/data/serverInfo.py` would disappear at the next assembly.
#
# NOTE: this is the address of the GAME SERVER, not of the bridge. The bridge (`bridge/`)
# is given in the page URL - `?bridge=host:port` - because it belongs to
# how the server was set up, not to the game data. See `compat/network_web.cpp`.
#
# CONFIGURATION FIELD (E3): the address is NOT written into this tool
# (it is published). It comes, in this order, from:
#   1. `--server ADDRESS` on the command line,
#   2. the environment variable `M2W_SERVER`,
#   3. `[server] address` in webclient.toml (git-ignored),
#   4. the file `build/port/private/server.txt` (a directory outside git and outside
#      publication) - one line with the address.
# Steps 2-4 are `workspace.server_address()`.
# Without any of them `serverInfo.py` stays with the address from the corpus and the tool
# says so plainly.
SERVER_ADDRESS = ''


def server_address():
    """The address from the command line, the environment or the private file - see above."""
    if '--server' in sys.argv:
        i = sys.argv.index('--server')
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1].strip()
    return workspace.server_address() or SERVER_ADDRESS


def set_server_address():
    """Replaces `SERVER_IP` in the already assembled `serverInfo.py`."""
    address = server_address()
    if not address:
        print('WARNING: no server address (--server, M2W_SERVER, [server] address in '
              'webclient.toml or build/port/private/server.txt) - serverInfo.py keeps the '
              'address from the corpus')
        return

    path_str = os.path.join(TARGET, 'serverInfo.py')
    if not os.path.exists(path_str):
        print('WARNING: no serverInfo.py - the server address was NOT set')
        return

    with io.open(path_str, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    new_value, how_many = re.subn(r'^(SERVER_IP\s*=\s*)"[^"]*"',
                        lambda m: m.group(1) + '"' + address + '"',
                        content, flags=re.MULTILINE)

    # A hit count other than one means the file does not look the way
    # I assume - and then we DO NOT WRITE. A silent replacement at zero places
    # would be worse than no fix, because it would look done.
    if how_many != 1:
        print('WARNING: SERVER_IP occurs %d times (should be 1) - '
              'the address was NOT set' % how_many)
        return

    with io.open(path_str, 'w', encoding='utf-8', newline='') as f:
        f.write(new_value)
    print('server address: %s' % address)


# ===========================================================================
# The index of map object properties - `property/crc_index.txt`
# ===========================================================================
# `areadata.txt` describes every map object by a CRC NUMBER, and the number is translated by
# `CPropertyManager` after registering all the `property/` files.
# In the original it does that by listing the directory at start; here those 1835 files
# lie in the streamed corpus and fetching them all would pull down
# hundreds of megabytes of unrelated chunks. Instead, a CRC -> path table,
# read by `compat/properties_web.cpp`; a file is fetched only
# when the object really comes into view.
#
# The CRC is the SECOND line of the file (the first is `YPRT`). I take the names from the corpus
# MANIFEST, not from disk: the directories have Korean names and only the bytes
# from the manifest are sure to match what the client will later ask for.
# It is the corpus the client reads (build_corpus.py), so the index and the
# files it points at are one build.
def property_index():
    """Writes `property/crc_index.txt` (CRC -> path of every `property/` file with
    a YPRT header), with names taken from the corpus manifest; returns the
    number of rows, 0 without a corpus.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import webfs
        k = webfs.Corpus(workspace.CORPUS)
    except Exception as e:
        print('WARNING: without the corpus I cannot build the property index (%s)' % e)
        return 0
    rows_list = []
    for name in sorted(k.file_list):
        if not name.lower().startswith('property/'):
            continue
        try:
            data = k.read_file(name)
        except Exception:
            continue
        lines_list = data.split(b'\n')
        if len(lines_list) < 2 or not lines_list[0].startswith(b'YPRT'):
            continue
        crc = lines_list[1].strip()
        if not crc.isdigit():
            continue
        rows_list.append(crc.decode('ascii') + '\t' + name)
    if not rows_list:
        return 0
    directory = os.path.join(TARGET, 'property')
    os.makedirs(directory, exist_ok=True)
    with io.open(os.path.join(directory, 'crc_index.txt'), 'w',
                 encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(rows_list) + '\n')
    return len(rows_list)


# ===========================================================================
# Private files - `build/port/private/`
# ===========================================================================
# Everything lying in this directory goes into the client's data set AS
# IT IS. The directory is outside the repository (`.gitignore` hides `build/port/*`
# except the listed ones), so this is the place for things that MUST NOT
# be published - above all `loginInfo.xml` from the auto-login built into TMP4
# (`introLogin.py`, `__LoadLoginInfo`):
#
#     addr="<server address>"        # game server, channel 1 (serverInfo.PORT_1)
#     port=13000
#     account_addr="<server address>"  # account server (serverInfo.PORT_AUTH)
#     account_port=11000
#     id="login"
#     pwd="password"
#     slot=0                       # which character from the list
#     autoLogin=1
#     autoSelect=1
#
# The syntax is Python (the file is executed with `exec`), the extension
# `.xml` is the name from TMP4. With this file the client, after loading the page, itself
# connects, logs in and enters the world - every page reload
# (`?v=N`) ends in the game without clicking. That is what it is for: tests after every
# build without manual logging in.
#
# The password does not pass through any tool or conversation - it lies in a file
# the user writes, and goes into the client's DATA PACKAGE on their
# machine. The package (`build/port/data*`) is outside the repository too.
PRIVATE = os.path.join(ROOT, 'build', 'port', 'private')


# ===========================================================================
# Baked trees
# ===========================================================================
# The corpus carries the original `.spt` - recipes from which the mesh is produced by
# the SpeedTree RT engine, whose sources we do not have. `tools/bake_speedtree.py`
# runs that engine NATIVELY (the DLL from the client) and writes the finished meshes
# to `build/port/baked/` under the same `.spt` names. In the package
# a file takes precedence over the streamed corpus, so the client
# gets the baked version, and `speedtree_web.cpp` recognizes it by the TMP4SPT1 magic.
BAKED = os.path.join(ROOT, 'build', 'port', 'baked')


def copy_baked():
    """Copies the baked SpeedTree files into the data set, keeping their paths;
    returns how many, -1 when nothing was baked.
    """
    if not os.path.isdir(BAKED):
        return -1
    how_many = 0
    for root_node, _dir, file_list in os.walk(BAKED):
        relative_one = os.path.relpath(root_node, BAKED)
        target_path = TARGET if relative_one == '.' else os.path.join(TARGET, relative_one)
        os.makedirs(target_path, exist_ok=True)
        for name in file_list:
            shutil.copy2(os.path.join(root_node, name), os.path.join(target_path, name))
            how_many += 1
    return how_many


# ===========================================================================
# UI font
# ===========================================================================
# The game asks for a font BY NAME (`UI_DEF_FONT = Tahoma:12` in
# `locale_game.txt`) - `CreateFontIndirectA`
# (`compat/platform_text.cpp`) builds CSS from that and LEAVES THE CHOICE TO THE BROWSER.
# Without our own font the result depends on what the browser substitutes for
# "Tahoma" - for the user it came out badly ("the font is awful").
#
# The file is `[inputs] font` of webclient.toml (default the Windows
# system tahoma.ttf - the packs do not carry one). We put it into the SMALL
# start-up package, so
# that it is in the virtual file system BEFORE main() starts - `main_web.cpp`
# registers it as a `FontFace` as early as possible (`M2W_SetLocale`).
FONT = workspace.FONT


def copy_font():
    """Copies the UI font (tahoma.ttf) into the data set; 1, or -1 when it is
    missing.
    """
    if not FONT or not os.path.isfile(FONT):
        return -1
    shutil.copy2(FONT, os.path.join(TARGET, 'tahoma.ttf'))
    return 1


def copy_private():
    """Copies into the data set ONLY the private file the client opens:
    loginInfo.xml (auto-login, file name case fixed); returns how many.

    Earlier EVERY file of private/ went in - the clean-clone trial found
    server.txt, forbidden.txt, bridge.json and old library backups inside the
    local client.data. private/ is also where the tools keep their private
    settings; those must not end up in a file the browser downloads.
    """
    if not os.path.isdir(PRIVATE):
        return 0
    how_many = 0
    for name in sorted(os.listdir(PRIVATE)):
        source = os.path.join(PRIVATE, name)
        # The wasm file system DISTINGUISHES case, Windows does not.
        # The client opens exactly `loginInfo.xml`.
        if os.path.isfile(source) and name.lower() == 'logininfo.xml':
            shutil.copy2(source, os.path.join(TARGET, 'loginInfo.xml'))
            how_many += 1
    return how_many


def main():
    """Rebuilds the client data set from scratch: client files, Python stdlib,
    rewritten game scripts, property list, baked trees, fonts, private files,
    cursors, UI scripts and (with `--with-corpus`) the whole corpus; reports
    what is missing. 1 without the client directory.
    """
    if not os.path.isdir(CLIENT):
        print('missing ' + os.path.relpath(CLIENT, ROOT))
        print('put the client there or fix the path in this script')
        return 1

    # The target is deleted from scratch. Without that a file removed from the source stays in
    # the set and the "what is missing" measurement lies - the same shape of error
    # as the stale `.o` files.
    if os.path.isdir(TARGET):
        shutil.rmtree(TARGET)
    os.makedirs(TARGET)

    copied, missing_ones = 0, []

    for name in FILES:
        source = os.path.join(CLIENT, name)
        if os.path.exists(source):
            shutil.copy2(source, os.path.join(TARGET, name))
            copied += 1
        else:
            missing_ones.append(name)

    # SOFTWARE CURSOR: `SOFTWARE_CURSOR 1` in metin2.cfg - a page
    # has no hardware cursor to hand to the game. The browser
    # cursor is hidden for good (compat/cursor_web.cpp), the arrow
    # is drawn by the game at its mouse position - so the browser moving the system
    # cursor during Pointer Lock (Firefox: a jump to the
    # middle of the window when rotating the camera) has nothing to show. From the URL:
    # `?cursor=hardware` restores the CSS cursor (platform_none.cpp).
    cfg = os.path.join(TARGET, 'metin2.cfg')
    if os.path.exists(cfg):
        with io.open(cfg, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
        new_value, how_many = re.subn(r'^(SOFTWARE_CURSOR\s+)\d+', r'\g<1>1', content, flags=re.M)
        if not how_many:
            new_value = content.rstrip('\n') + '\nSOFTWARE_CURSOR\t\t\t1\n'
        with io.open(cfg, 'w', encoding='utf-8', newline='\n') as f:
            f.write(new_value)
        print('metin2.cfg: SOFTWARE_CURSOR 1 (the game draws the cursor)')

    # I take the language files by pattern, because there are a dozen of them and they may
    # differ between clients.
    for name in sorted(os.listdir(CLIENT)):
        if name.startswith('locale_') and name.endswith('.cfg'):
            shutil.copy2(os.path.join(CLIENT, name),
                         os.path.join(TARGET, name))
            copied += 1

    for name in DIRECTORIES:
        source = os.path.join(CLIENT, name)
        if os.path.isdir(source):
            # Windows binaries of the client's Python (pyexpat*.pyd, a .pdb) cannot run
            # in the browser - left out (0.9 MB of the start-up package)
            shutil.copytree(source, os.path.join(TARGET, name),
                            ignore=shutil.ignore_patterns('*.pyd', '*.pdb', '*.dll', '*.exe'))
            copied += sum(len(f) for _r, _d, f in os.walk(source))
        else:
            missing_ones.append(name + '/')


    stdlib_count = copy_stdlib()
    if stdlib_count <= 0:
        print('\nERROR: no sources of the Python standard library (%s)' % STDLIB_SOURCE)
        print('       set M2W_PYTHON_LIB to the Lib/ directory of CPython 3.13')
        print('       without it Py_Initialize ends with')
        print('       "Failed to import encodings module" - a black screen')
        sys.exit(1)
    else:
        print('Python standard library: %d .py files' % stdlib_count)
        copied += stdlib_count


    script_count = copy_scripts()
    if script_count < 0:
        print('\nWARNING: no rewritten game scripts')
        print('         run tools/rewrite_scripts.py')
        print('         without them the client ends at "RunMain Error"')
    else:
        print('game scripts (Python 3): %d files' % script_count)
        copied += script_count



    property_count = property_index()
    if property_count:
        print('object property index (CRC -> file): %d' % property_count)
        copied += 1

    baked_count = copy_baked()
    if baked_count < 0:
        print('WARNING: no baked trees - run tools/bake_speedtree.py')
        print('         without them outdoor maps have no trees (a third of the objects)')
    else:
        print('baked trees (SpeedTree -> meshes): %d' % baked_count)
        copied += baked_count

    font_count = copy_font()
    if font_count < 0:
        print('WARNING: no UI font (%s) - set [inputs] font in webclient.toml; '
              'without it the UI uses the browser fallback font (worse than the '
              'original)' % FONT)
    else:
        print('UI font (tahoma.ttf): copied')
        copied += font_count

    # GDI-BAKED FONTS: glyph bitmaps
    # and metrics from real GDI, `data/fonts/`. Windows only;
    # without them `platform_text.cpp` draws on a canvas (worse than the original).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import bake_fonts
    baked_font_count = bake_fonts.build(os.path.join(TARGET, 'fonts'))
    print('GDI-baked fonts: %d' % baked_font_count)
    copied += baked_font_count

    # `--no-private`: the package for PUBLICATION - without
    # loginInfo.xml (autologin with a password!) and the rest of `private/`. Used by
    # tools/package.py; measured: client.data with autologin ended up in dist/site/.
    private_count = 0 if '--no-private' in sys.argv else copy_private()
    if private_count:
        print('private files (not in the repository): %d' % private_count)
        copied += private_count

    # CURSORS: `.cur` from the TMP4 resources as PNG + hotspot,
    # `data/cursors/`. See tools/cursors.py and compat/cursor_web.cpp.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import cursors
    cursor_count = cursors.build(os.path.join(TARGET, 'cursors'))
    print('cursors: %d' % cursor_count)
    copied += cursor_count + 1

    uiscript_count = copy_uiscript()
    if uiscript_count < 0:
        print('WARNING: no window descriptions - run tools/uiscript_from_client.py')
        print('         without them the client stops at the first window')
    else:
        print('window descriptions (UIScript): %d files' % uiscript_count)
        copied += uiscript_count

    # A PACKAGE WITHOUT THE CORPUS.
    #
    # Since the game data comes over the network as needed (`compat/webfs_web.cpp`),
    # there is no point keeping game content in the preloaded package. Only what
    # the corpus does NOT KNOW stays, because it is created here: the scripts rewritten to Python 3,
    # the CPython standard library, the window descriptions and the settings files.
    #
    # Measured before the change: package 1.86 GB, browser heap 2018 MB
    # of 4096 MB.
    #
    # WITHOUT THE CORPUS BY DEFAULT. Earlier it was the other way round: without
    # a flag the tool copied `corpus_files/` - a directory from a DIFFERENT,
    # older tool, with files with LF line endings - and the package grew
    # to 2 GB, and `property/*.prb` with LF shadowed the correct (CRLF) ones from
    # the network corpus: `CProperty::ReadFromMemory: File format error
    # after FourCC` x1400, ALL BUILDINGS AND TREES DISAPPEARED. One
    # forgotten flag = a broken client for everyone who
    # reloads it. The exceptional mode is to be the one that has to be typed.
    if '--with-corpus' in sys.argv:
        print('mode --with-corpus: ALL game content in the package (2 GB) - for tests only')
        corpus_count = copy_corpus()
    else:
        print('mode without the corpus (default): game content comes over the network, not in the package')
        corpus_count = 0
    if corpus_count < 0:
        print('WARNING: no unpacked packs - run tools/build_corpus.py --unpack-only')
    else:
        print('data from the corpus: %d files' % corpus_count)
        copied += corpus_count

    size = sum(os.path.getsize(os.path.join(r, f))
                  for r, _d, fs in os.walk(TARGET) for f in fs)

    print('\ndata set: %d files, %d kB' % (copied, size // 1024))
    print('in: ' + os.path.relpath(TARGET, ROOT))

    if missing_ones:
        print('\nMISSING (not in the client):')
        for b in missing_ones:
            print('  ' + b)

    # A CHECK BY NAME. The client checks eleven specific files
    # (`sc_apszPythonLibraryFilenames`) and ends when even one is missing.
    # I check them here, to learn about it from this script,
    # not from an error message in the browser.
    required = ('UserDict.pyc', '__future__.pyc', 'copy_reg.pyc',
                'linecache.pyc', 'ntpath.pyc', 'os.pyc', 'site.pyc',
                'stat.pyc', 'string.pyc', 'traceback.pyc', 'types.pyc')
    missing_lib = [n for n in required
                if not os.path.exists(os.path.join(TARGET, 'lib', n))]
    if missing_lib:
        print('\nCHECK FAILED - the client REQUIRES these files:')
        for n in missing_lib:
            print('  lib/' + n)
        return 1

    print('check: all 11 files from the client list are in place')

    set_server_address()
    return 0


if __name__ == '__main__':
    sys.exit(main())
