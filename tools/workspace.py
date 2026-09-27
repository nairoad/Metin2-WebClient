#!/usr/bin/env python3
"""workspace.py - where the build finds its inputs and puts its outputs.

Every tool takes its paths from here instead of spelling them itself, so a
different client (another fork of the sources, another pack set, emsdk in
another place) is one edit of `webclient.toml`, not a search through thirty
scripts.

Each setting is looked up in this order:
  1. an environment variable (for one-off runs and CI),
  2. `webclient.toml` in the repository root (the user's file, git-ignored;
     `webclient.example.toml` describes every key),
  3. the default - the layout of the repository this layer was built in.

Relative paths in `webclient.toml` are relative to the repository root.
Private values (the server address, the bridge address) can also stay in
`build/port/private/` as before; neither `webclient.toml` nor `private/` is
tracked by git.

Running it prints every resolved setting:
    python tools/workspace.py
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# NO PATHS OF THIS MACHINE IN THE CLIENT. The compiler writes source
# paths into what it builds - `__FILE__` in the engine's messages, debug info
# in the libraries - and a published client.wasm would carry the builder's
# user name and folders (measured: 138 in client.wasm). Every tool imports
# this module and every compile goes through emcc, which appends EMCC_CFLAGS:
# the project folder is mapped to `.` in both spellings Windows gives it.
def _map_project_paths():
    """Adds `-ffile-prefix-map=<project>=.` to EMCC_CFLAGS (once)."""
    import shlex
    flags = os.environ.get('EMCC_CFLAGS', '')
    for spelling in sorted({ROOT, ROOT.replace(os.sep, '/')}):
        flag = '-ffile-prefix-map=%s=.' % spelling
        if flag not in flags:
            flags = (flags + ' ' + shlex.quote(flag)).strip()
    os.environ['EMCC_CFLAGS'] = flags


_map_project_paths()
CONFIG_FILE = os.path.join(ROOT, 'webclient.toml')


def load_config(path=CONFIG_FILE):
    """Reads `webclient.toml` into a dict of sections; {} when the file is absent."""
    if not os.path.exists(path):
        return {}
    try:
        import tomllib
    except ImportError:
        sys.exit('ERROR: reading %s needs Python 3.11 or newer (tomllib)' % path)
    with open(path, 'rb') as f:
        try:
            return tomllib.load(f)
        except tomllib.TOMLDecodeError as e:
            sys.exit('ERROR: %s is not valid TOML: %s' % (path, e))


CONFIG = load_config()


def setting(section, key, env=None, default=None):
    """The value of `[section] key`: the environment variable `env` first, then
    webclient.toml, then `default`."""
    if env and os.environ.get(env, '').strip():
        return os.environ[env].strip()
    value = CONFIG.get(section, {}).get(key)
    if value is None or value == '':
        return default
    return value


def path_setting(section, key, env=None, default=None):
    """Like `setting`, for a path: a relative one is taken from the repository root."""
    value = setting(section, key, env, default)
    if value is None:
        return None
    value = os.path.expanduser(str(value))
    return os.path.normpath(value if os.path.isabs(value) else os.path.join(ROOT, value))


# --- inputs: what the user brings -------------------------------------------
# The client sources (TMP4 or another fork): `source/<library>` with the engine,
# `extern/include` and `extern/library` with its third-party headers and libraries.
SOURCE = path_setting('inputs', 'source', 'M2W_SOURCE', 'reference/tmp4_source')
SOURCE_LIBRARIES = os.path.join(SOURCE, 'source')
EXTERN_INCLUDE = os.path.join(SOURCE, 'extern', 'include')
EXTERN_LIBRARY = os.path.join(SOURCE, 'extern', 'library')

# The installed game client: the packs, and the Windows DLLs used only by the
# offline bakers (SpeedTreeRT.dll, granny2.dll).
CLIENT = path_setting('inputs', 'client', 'M2W_CLIENT', 'reference/Client')
PACKS = path_setting('inputs', 'packs', 'M2W_PACKS', os.path.join(CLIENT, 'pack'))

# The UI font registered as "Tahoma" in the browser (compat/locale_web.cpp).
# It is not in the packs; the default is the system font of a Windows machine.
FONT = path_setting('inputs', 'font', 'M2W_FONT',
                    os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'tahoma.ttf')
                    if os.name == 'nt' else None)

# --- tools ------------------------------------------------------------------
EMSDK = path_setting('tools', 'emsdk', 'EMSDK', r'C:\emsdk' if os.name == 'nt' else '~/emsdk')

# A Python that still has `lib2to3` (3.12 or older) - rewrites the game's
# Python 2 scripts (rewrite_scripts.py, uiscript_from_client.py).
PYTHON_2TO3 = path_setting('tools', 'python2to3', 'M2W_PYTHON_2TO3',
                           os.path.join('~', 'AppData', 'Local', 'Programs', 'Python', 'Python310', 'python.exe'))

# MSVC x86 environment (Build Tools 2022) - only for the offline Windows tools:
# the SpeedTree baker and the .gr2 repair.
VCVARS32 = path_setting('tools', 'vcvars32', 'M2W_VCVARS32', r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat')

# --- our layer and the work directory ---------------------------------------
PORT = os.path.join(ROOT, 'build', 'port')     # outputs: stage, obj, lib, data, client.*
COMPAT = os.path.join(ROOT, 'compat')          # the compatibility layer (C++/JS)
COMPAT_TESTS = os.path.join(COMPAT, 'tests')   # its unit tests (tools/gates/run_tests.py)
TREE = os.path.join(COMPAT, 'tree')            # our headers covering the sources
STAGE = os.path.join(PORT, 'stage')            # the assembled source tree
LIBDIR = os.path.join(PORT, 'lib')
OBJDIR = os.path.join(PORT, 'obj')
PRIVATE = os.path.join(PORT, 'private')        # outside git and outside publication
GAME_FILES = os.path.join(PORT, 'client_files')  # the user's packs unpacked (build_corpus.py)
BAKED = os.path.join(PORT, 'baked')            # trees baked by bake_speedtree.py
CORPUS = os.path.join(PORT, 'corpus')          # the streamed corpus (build_corpus.py)
SITE = os.path.join(ROOT, 'site')              # the page template


def private_line(name):
    """The content of `build/port/private/<name>` stripped, '' when the file is absent."""
    path = os.path.join(PRIVATE, name)
    if not os.path.isfile(path):
        return ''
    with io.open(path, encoding='utf-8') as f:
        return f.read().strip()


def for_batch(path):
    """`path` unchanged when it can be written between double quotes in a cmd
    batch file; exits with an explanation when it holds a character cmd would
    act on (`"`, `%`, a line break) - a path from webclient.toml must not be
    able to end the quoting and add a command (security audit)."""
    bad = [c for c in ('"', '%', '\r', '\n') if c in path]
    if bad:
        sys.exit('this path cannot be used in a cmd batch file (it contains %s): %s'
                 % (' '.join(repr(c) for c in bad), path))
    return path


# --- the server -------------------------------------------------------------
def server_address():
    """The game server address written into serverInfo.py: M2W_SERVER, then
    `[server] address`, then private/server.txt; '' when none is set."""
    if os.environ.get('M2W_SERVER', '').strip():
        return os.environ['M2W_SERVER'].strip()
    return setting('server', 'address') or private_line('server.txt')


def bridge_address():
    """The default bridge address put into the page: M2W_BRIDGE, then
    `[server] bridge`, then private/bridge.txt; '' means the page's own host."""
    return setting('server', 'bridge', 'M2W_BRIDGE') or private_line('bridge.txt')


def describe():
    """Prints every resolved path and whether it exists; the server and bridge
    only as set / not set (their values are private)."""
    print('config  %s' % (CONFIG_FILE if CONFIG else '(no webclient.toml - defaults)'))
    for name, path in (('source', SOURCE), ('client', CLIENT), ('packs', PACKS), ('emsdk', EMSDK),
                       ('font', FONT), ('2to3', PYTHON_2TO3), ('vcvars', VCVARS32)):
        print('%-7s %s%s' % (name, path, '' if path and os.path.exists(path) else '  (MISSING)'))
    print('%-7s %s' % ('server', 'set' if server_address() else '(not set)'))
    print('%-7s %s' % ('bridge', 'set' if bridge_address() else "(not set - the page's own host)"))


if __name__ == '__main__':
    describe()
