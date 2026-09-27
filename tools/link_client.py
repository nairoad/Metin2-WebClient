#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""link_client.py - THE FIRST LINK OF THE WHOLE.

So far the project measured progress with the contract: unresolved symbols
minus what our libraries provide. Now the contract has been
**empty**. But "it compiles" is not "it links" - the project learned that
three times (`link_test`, Crypto++, LZO).

This script asks the question the contract cannot ask: **can these
317 objects and four archives be put together into one `.wasm`.**

WHY `-sERROR_ON_UNDEFINED_SYMBOLS=0` IS NOT CHEATING HERE
=========================================================
Usually it would be. Here it is not, for a concrete reason: emscripten in
this mode **does not pretend the symbol exists**. It puts a stub in its
place, which on a call aborts the program and **prints the name of the
missing function**. That is exactly the stub this project is about: loud,
not silent.

Thanks to that one can get to a picture with Granny 3D and SpeedTree still
unresolved - and when the game touches a character model, the console says
`GrannyGetFileInfo` instead of showing an empty board without explanation.

The script **prints all** unresolved symbols, grouped the same way as the
contract, so the list of stubs is explicit, not hidden in a key.
"""

import io
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
OBJDIR = os.path.join(PORT, 'obj', 'gamelib')
LIBDIR = os.path.join(PORT, 'lib')
COMPAT = workspace.COMPAT
STAGE = os.path.join(PORT, 'stage')
EXTERN = workspace.EXTERN_INCLUDE
MEASURING_SHELL = os.path.join(workspace.SITE, 'measure_shell.html')
# The GLUE, not the page. The page is a separate file - see the note at
# `--shell-file` below.
OUTPUT = os.path.join(PORT, 'client.js')
PAGE = os.path.join(PORT, 'client.html')
DATA = os.path.join(PORT, 'data')

EMSDK = workspace.EMSDK

# The same families as in `build_gamelib.py`. Repeated, not imported:
# both scripts are meant to work separately.
FAMILIES = (
    ('Granny 3D',        re.compile(r'^Granny[A-Z]')),
    ('SpeedTree',        re.compile(r'^_?ZN12CSpeedTreeRT|CSpeedTreeRT')),
    ('CPython',          re.compile(r'^_?Py[A-Z_]|^_?_Py')),
    ('DevIL / images',   re.compile(r'^_?il[A-Z]|^_?ilu[A-Z]')),
    ('libjpeg',          re.compile(r'^_?jpeg_')),
    # The names here are DECORATED (`_Z16ReleaseSemaphorePvlPl`), because these
    # are C++ functions, not C. The first version of this pattern did not catch
    # them and one of our deliberate non-definitions came out as "UNEXPECTED".
    ('threads and semaphores', re.compile(r'CreateSemaphore|ReleaseSemaphore|'
                                    r'WaitForSingleObject|SetThreadPriority|'
                                    r'_beginthreadex|_endthreadex')),
)


def tool_name(name):
    """Path of an emsdk tool: from PATH, else from the EMSDK directory; exits when
    it is nowhere.
    """
    p = shutil.which(name)
    if p:
        return p
    for directory in (os.path.join(EMSDK, 'upstream', 'bin'),
                os.path.join(EMSDK, 'upstream', 'emscripten')):
        for ext in ('', '.exe', '.bat'):
            k = os.path.join(directory, name + ext)
            if os.path.exists(k):
                return k
    raise SystemExit(f'{name} not found - add emsdk to PATH')


def collect_objects():
    """Every .o under obj/gamelib (sorted per directory)."""
    objs = []
    for root, _d, files in os.walk(OBJDIR):
        objs += [os.path.join(root, f) for f in sorted(files) if f.endswith('.o')]
    return objs


def strip_machine_paths(path):
    """Removes the paths of this machine from the JS glue: emscripten
    writes the data package under its full path (`PACKAGE_NAME`, a key used
    consistently, so replacing every occurrence keeps it working) and
    `// include:` comments naming the project and the temp folder."""
    import tempfile
    with open(path, encoding='utf-8') as f:
        text = f.read()
    before = text
    for folder, name in ((ROOT, '.'), (tempfile.gettempdir(), '<tmp>')):
        for spelling in sorted({folder, folder.replace(os.sep, '/')}, key=len, reverse=True):
            text = text.replace(spelling, name)
    if text != before:
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(text)


def compile_entry(empp):
    """The objects that belong to the link but not to the contract: `main_web.cpp`
    (the entry point) and `link_data.cpp` (data symbols the linker needs,
    e.g. `GrannyPNT332VertexType`). They are not in `build_gamelib.py`,
    because the contract asks what is MISSING, and neither provides anything
    to the game code.
    """
    result = []
    for name in ('main_web', 'link_data'):
        o = os.path.join(OBJDIR, name + '.o')
        r = subprocess.run(
            [empp, '-std=c++17', '-c', '-O0', '-w',
             '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
             '-I' + os.path.join(STAGE, 'eterBase'),
             os.path.join(COMPAT, name + '.cpp'), '-o', o],
            capture_output=True, text=True)
        if not os.path.exists(o):
            print(f'{name}.cpp DID NOT COMPILE:')
            for l in r.stderr.splitlines():
                if 'error:' in l:
                    print('  ' + l)
            return None
        result.append(o)
    return result


def copy_site_scripts():
    """Copies sw.js and preload.js from site/ next to the page."""
    for name in ('sw.js', 'preload.js'):
        source = os.path.join(workspace.SITE, name)
        if os.path.isfile(source):
            with io.open(source, encoding='utf-8') as f:
                content = f.read()
            with io.open(os.path.join(PORT, name), 'w', encoding='utf-8', newline='\n') as f:
                f.write(content)


def main():
    """Links the whole client (objects + archives + the entry point) into
    client.wasm/client.js with a fixed hash seed, lists the stubs from the JS
    glue and the unexpected ones, then writes the page, the measuring shell
    and the crash reporter next to it. 1 on a failed compile or link.
    """
    # LINKING WITH A FIXED SEED (naming plan Phase 0).
    # Observed ONCE: two links in a row gave a different client.wasm (a different
    # order of EM_JS imports, further shifted function indices); with
    # PYTHONHASHSEED=0 identical. The reviewer did NOT reproduce the difference
    # without the seed (5 links, all identical), so the attribution "set
    # iteration in emscripten" is unconfirmed - the first two links may have had
    # different input. The seed stays as a safeguard: it costs nothing, and the
    # test "a rename does not change the binary" needs repeatability.
    os.environ['PYTHONHASHSEED'] = '0'
    empp = tool_name('em++')

    objs = collect_objects()
    if not objs:
        print('no objects in obj/gamelib - run first '
              'tools/build_gamelib.py')
        return 1

    input_data = compile_entry(empp)
    if input_data is None:
        return 1
    for o in input_data:
        if o not in objs:
            objs.append(o)

    libs = []
    if os.path.isdir(LIBDIR):
        libs = [os.path.join(LIBDIR, f) for f in sorted(os.listdir(LIBDIR))
                if f.endswith('.a')]

    print(f'\nobjects {len(objs)} | archives {len(libs)}')

    command = [
        empp, '-O1',
        # Unresolved symbols become stubs that on a call
        # ABORT and give the name. See the note at the top of the file.
        '-sERROR_ON_UNDEFINED_SYMBOLS=0',
        '-sFULL_ES3=1', '-sMAX_WEBGL_VERSION=2', '-sMIN_WEBGL_VERSION=2',
        '-sALLOW_MEMORY_GROWTH=1',
        '-sINITIAL_MEMORY=268435456',
        '-sSTACK_SIZE=5242880',
        # NO EXIT FROM THE RUNTIME. With `EXIT_RUNTIME=1`
        # `emscripten_cancel_main_loop` (our counterpart of WM_QUIT after
        # `app.Exit()`/`app.Abort()`) ends the runtime: static destructors run,
        # and `CDynamicPool::~CDynamicPool` has `assert(empty())`,
        # which in the original (Release, NDEBUG) never ran - here it
        # gave a crash window INSTEAD OF a calm "the client has finished".
        # The page lives on anyway; after the loop ends there is nothing to
        # clean up, a reload is enough.
        '-sEXIT_RUNTIME=0',
        '-sASSERTIONS=1',
        # FUNCTION NAMES IN THE CALL TRACE.
        #
        # Without `-g2` the browser shows `wasm-function[35462]` and nothing more -
        # and with a "memory access out of bounds" error that is as good as
        # nothing. The binary grows, but size is not what costs now:
        # searching blindly is. The same lesson as with Traceback()
        # - diagnostics first, the rest after.
        '-g2',
        # compat/runtime.js: the JS side of the compatibility layer (m2w.options,
        # screen measures, end of game). The pre-js loads at the glue module level,
        # BEFORE HEAPU8/FS - the file only defines functions (Phase C, group 2).
        '--pre-js', os.path.join(COMPAT, 'runtime.js'),
        # M2W_SAFE_HEAP=1 in the environment: every memory read/write is
        # checked, and a write to NULL (address 0) stops AT THE PLACE of the
        # write with a call trace - instead of "corrupted heap (address zero)"
        # detected only at the next callback, far from the culprit.
        # Mode 2 = without alignment checks (wasm allows unaligned
        # accesses, and the original x86 code relies on them, e.g. tea_code).
        # Slow (2-3x), so only on request.
        *(['-sSAFE_HEAP=2'] if os.environ.get('M2W_SAFE_HEAP') == '1' else []),
        # `FS` in the JS glue. Without it one cannot look into syserr.txt, i.e.
        # into the client's OWN log - the only place where the game
        # says what went wrong. Without it only guessing remains.
        '-sEXPORTED_RUNTIME_METHODS=FS,callMain',
        # libjpeg is a ready emscripten port, so there is no reason
        # to write it. The versions agree: the client has the libjpeg-9a header
        # (JPEG_LIB_VERSION 90), the port has jpeg-9f - the same major version,
        # the same ABI. With libjpeg this is no detail: between version 6b
        # and 9 the layout of `jpeg_compress_struct` changed, so a mismatch
        # would give no link error, just writes to the wrong fields.
        '-sUSE_LIBJPEG=1',
        # zlib is a port too. CPython's `binascii` needs it, which
        # calls `crc32` - and that is the only trace of zlib left after
        # disabling the `zlib` module. The game does not use the module itself; the
        # checksum function it does.
        '-sUSE_ZLIB=1',

        # `wasmBinary` MUST BE LISTED, so that it can be supplied.
        #
        # Emscripten strips from the glue the handling of those `Module` fields that
        # are not on this list - and does it SILENTLY at build time, loudly only
        # in the browser:
        #
        #     Aborted("Module.wasmBinary" was supplied but "wasmBinary"
        #             not included in INCOMING_MODULE_JS_API)
        #
        # The list below is emscripten's default list (src/settings.js:1008)
        # plus `wasmBinary`. Giving it explicitly REPLACES the default one, so
        # shortening it "because we don't use it" would cut off fields the glue
        # itself uses.
        '-sINCOMING_MODULE_JS_API=ENVIRONMENT,arguments,canvas,'
        'dynamicLibraries,elementPointerLock,instantiateWasm,locateFile,'
        'monitorRunDependencies,noExitRuntime,noInitialRun,onAbort,onExit,'
        'onRuntimeInitialized,postRun,preInit,preRun,print,printErr,'
        'setStatus,statusMessage,stderr,stdin,stdout,thisProgram,wasm,'
        'websocket,wasmBinary',
    ]

    # WITHOUT `--shell-file`.
    #
    # Until then emscripten assembled the page itself: it took our shell, put
    # `{{{ SCRIPT }}}` into it and produced `client.html`. Better: the page is a
    # PLAIN FILE, written by hand, and emscripten produces only the `client.js`
    # glue.
    #
    # The difference is not cosmetic. A hand-written page can DOWNLOAD THE
    # BINARY ITSELF - with a stream, a megabyte counter and retries when the
    # server stutters - and only then hand the finished bytes to the glue through
    # `Module.wasmBinary`. With `--shell-file` the download belongs to
    # emscripten, which at the first network error leaves an empty page.

    # GAME DATA IN THE WASM FILE SYSTEM.
    #
    # `--preload-file DIRECTORY@/` puts the directory's content into the root
    # of the virtual file system, so the client sees `lib/os.pyc`
    # and `locale.cfg` exactly where it looks for them - without changing
    # a single path in the game code.
    #
    # The set is assembled by `tools/build_client_data.py` and is SMALL (about 1.5 MB):
    # start-up files only, no packs. The packs are 1.4 GB and would have to be
    # downloaded BEFORE the first frame - the real port fetches them as
    # needed, so putting them here would be a step in the wrong direction.
    if os.path.isdir(DATA):
        command += ['--preload-file', DATA + '@/']
    else:
        print('WARNING: missing ' + os.path.relpath(DATA, ROOT) +
              ' - run tools/build_client_data.py')
        print('         the client will link, but will stop at its own')
        print('         check "Python Library file not exist"')

    command += objs + libs + ['-o', OUTPUT]

    # THE OUTPUT IS DELETED BEFORE THE RUN. The first version of this script did
    # not do that and considered the link successful, because a
    # `.wasm` from a PREVIOUS, partial attempt lay in the directory. The same shape
    # of error as the stale `.o` files: the measurement looked at a
    # file, not at the result.
    for extension_name in ('.wasm', '.js', '.html'):
        old_one = os.path.splitext(OUTPUT)[0] + extension_name
        if os.path.exists(old_one):
            os.remove(old_one)

    # A RESPONSE FILE, not the command line.
    #
    # The first attempt passed all 319 paths directly and Windows
    # refused: "CreateProcess failed (206)", i.e. a command line longer
    # than 32767 characters. The error said not a word about length - it looked
    # like a failure of emscripten itself.
    rsp = os.path.join(PORT, 'obj', 'konsolidacja.rsp')
    with open(rsp, 'w', encoding='utf-8') as f:
        for a in command[1:]:
            # Windows backslashes in the response file are treated as escape
            # characters, so I replace them with plain slashes - clang takes both.
            f.write('"' + a.replace(chr(92), '/') + '"' + chr(10))

    print('linking...')
    r = subprocess.run([command[0], '@' + rsp], capture_output=True, text=True)

    # STUBS ARE COUNTED FROM THE JS GLUE, NOT FROM THE LINKER WARNINGS.
    #
    # The first version read `undefined symbol` from stderr - and on the SECOND
    # run showed ZERO stubs, although the binary was identical.
    # Emscripten has its own cache and on a repeated link simply does not
    # print these warnings. So the measurement depended on
    # whether something was in the cache before.
    #
    # The JS glue contains for every stub a line `missing function: NAME`,
    # because that is the message the user will see. Reading the
    # OUTPUT instead of the log is immune to the cache.
    def stubs_from_glue():
        """Names of the stubs (unresolved symbols) as the JS glue lists them
        (`missing function: NAME`) - read from the output, not the linker log.
        """
        js = os.path.splitext(OUTPUT)[0] + '.js'
        if not os.path.exists(js):
            return set()
        with open(js, encoding='utf-8', errors='replace') as f:
            return set(re.findall(r'missing function: ([A-Za-z_][A-Za-z0-9_]*)',
                                  f.read()))

    # The linker prints unresolved symbols in its warnings.
    unresolved = set()
    for l in r.stderr.splitlines():
        m = re.search(r'undefined symbol: ([^\s(]+)', l)
        if m:
            unresolved.add(m.group(1))

    # `-sERROR_ON_UNDEFINED_SYMBOLS=0` turns only FUNCTIONS into stubs.
    # On unresolved DATA the linker stops anyway - and that is
    # right: a function can get a body that shouts, but for data
    # nothing can be put in that would not lie.
    for l in r.stderr.splitlines():
        m = re.search(r'wasm-ld: error: .*undefined symbol: ([^\s(]+)', l)
        if m:
            unresolved.add(m.group(1))

    wasm = os.path.splitext(OUTPUT)[0] + '.wasm'
    # BOTH THE EXIT CODE AND THE FILE. The existence of the file alone lied.
    succeeded = (r.returncode == 0) and os.path.exists(wasm)

    if not succeeded:
        print('\nLINK FAILED. First errors:')
        for l in r.stderr.splitlines():
            if 'error' in l.lower():
                print('  ' + l[:200])
        # Even on an error the list of unresolved symbols is worth showing.
        if unresolved:
            summarize(unresolved)
        return 1

    strip_machine_paths(OUTPUT)
    print(f'\nLINKED: {os.path.relpath(wasm, ROOT)} '
          f'({os.path.getsize(wasm) // 1024} kB)')

    # THE MAIN PAGE: `client.html` from the template `site/client.html` - a
    # minimal shell WITHOUT instrumentation. A lesson learned the hard way
    # a GL call counter in the page slowed the client down
    # sixtyfold, and the cause sat in the page, not in the client. The
    # measuring shell stays next to it as `client_measure.html` and is
    # switched on deliberately when an on-screen counter is needed.
    version = str(int(os.path.getmtime(os.path.join(PORT, 'client.wasm')))) if os.path.exists(os.path.join(PORT, 'client.wasm')) else '0'
    kTemplate = os.path.join(workspace.SITE, 'client.html')
    with open(kTemplate, encoding='utf-8') as f:
        kPage = f.read().replace('__VERSION__', version)
    # the default bridge address (wss://game.example.com) from outside git -
    # M2W_BRIDGE, [server] bridge in webclient.toml or private/bridge.txt
    sBridge = workspace.bridge_address()
    kPage = kPage.replace('__BRIDGE__', sBridge.replace('"', ''))
    print('bridge in the page: ' + (sBridge or '(the same server as the page)'))
    with open(PAGE, 'w', encoding='utf-8', newline='\n') as f:
        f.write(kPage)
    copy_site_scripts()
    print(f'page:    {os.path.relpath(PAGE, ROOT)} (our template site/client.html)')

    # MEASURING SHELL - alongside, on request.
    if not page_parses(MEASURING_SHELL):
        return 1

    measurement = os.path.join(PORT, 'client_measure.html')
    shutil.copyfile(MEASURING_SHELL, measurement)
    print(f'measure: {os.path.relpath(measurement, ROOT)} (on-screen counter)')

    summarize(stubs_from_glue())
    return 0



def page_parses(path_str):
    """Does the JavaScript in the shell parse at all?

    WHY THIS STANDS IN THE LINK AND NOT IN MY HEAD
    ==============================================
    Seven times in this project the same error: `\\n` in a string, typed through
    a shell heredoc into a script that writes code. The shell eats one backslash
    and a REAL line end lands in the file. The string breaks off, the file stops
    parsing.

    The symptom is the same every time and misleads every time: the page looks
    stuck ("preparing", "measurement starting"), because the first script block
    died and defined nothing the second needs. Nothing points at a typo -
    everything points at the client.

    The rule "mind the backslashes" has been in the notes and did
    not work even once. So instead of being more careful, a machine checks it:
    every `<script>` block goes to `node --check`. That costs a fraction of a
    second and closes this whole family of errors.
    """
    content = io.open(path_str, encoding='utf-8', errors='replace').read()
    blocks = re.findall(r'<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>',
                       content, re.S)
    if not blocks:
        print('WARNING: the shell has not a single <script> block to '
              'check - is this really the right page?')
        return True

    node = tool_name('node')
    for i, block in enumerate(blocks):
        temporary = os.path.join(PORT, '.shell_check.js')
        io.open(temporary, 'w', encoding='utf-8').write(block)
        result = subprocess.run([node, '--check', temporary],
                               capture_output=True, text=True)
        os.remove(temporary)
        if result.returncode != 0:
            print('\nTHE SHELL DOES NOT PARSE (block %d):' % (i + 1))
            for row in (result.stderr or '').splitlines()[:8]:
                print('  ' + row)
            print('  file: ' + os.path.relpath(path_str, ROOT))
            return False
    return True


def summarize(unresolved):
    """Prints the stubs GROUPED. The number alone says nothing - the difference
    between "Granny is missing" and "something I did not know about is missing"
    is the whole content of this measurement.
    """
    print(f'\nstubs (unresolved symbols): {len(unresolved)}')
    if not unresolved:
        print('  NONE - everything resolved')
        return

    remaining = set(unresolved)
    for name, formula in FAMILIES:
        matches = sorted(s for s in remaining if formula.search(s))
        if matches:
            print(f'\n  {name}: {len(matches)}')
            for s in matches[:6]:
                print(f'      {s}')
            if len(matches) > 6:
                print(f'      ... i {len(matches) - 6} wiecej')
        remaining -= set(matches)

    # THIS IS THE MOST IMPORTANT PART OF THE OUTPUT. The families above are known
    # and expected. This list is the things I did not know about -
    # and only for these was linking worth it.
    if remaining:
        print(f'\n  UNEXPECTED: {len(remaining)}')
        for s in sorted(remaining):
            print(f'      {s}')
    else:
        print('\n  UNEXPECTED: none')


if __name__ == '__main__':
    sys.exit(main())
