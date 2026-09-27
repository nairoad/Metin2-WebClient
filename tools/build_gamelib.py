#!/usr/bin/env python
"""build_gamelib.py - builds `GameLib` under emscripten and prints the CONTRACT.

It does three things I used to do by hand:
  1. assembles the tree (`tools/stage_port.py`),
  2. compiles all the `GameLib` files to objects,
  3. collects the UNRESOLVED symbols and writes `build/port/gamelib_contract.md`.

The third point is the most important here. "It compiles" does not mean "it
links" - the project learned that three times (link_test, Crypto++,
LZO). The list of missing symbols is the only thing that
says HOW MUCH really remains, and it is computed, not estimated.

The symbols fall into three kinds and mixing them would inflate the count:
the runtime environment (supplied by the toolchain), our deliberate non-definitions
(meant to stop the linker) and the actual engine-layer contract.

Running:
    python tools/build_gamelib.py
Requires `em++` and `llvm-nm` in PATH (or emsdk in C:\\emsdk).
"""

import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
STAGE = os.path.join(ROOT, 'build', 'port', 'stage')
EXTERN = workspace.EXTERN_INCLUDE
COMPAT = workspace.COMPAT
OBJDIR = os.path.join(ROOT, 'build', 'port', 'obj', 'gamelib')
LIBDIR = os.path.join(ROOT, 'build', 'port', 'lib')
CONTRACT = os.path.join(ROOT, 'build', 'port', 'gamelib_contract.md')   # generated - not in the repository

EMSDK = workspace.EMSDK

# OPTIMIZATION LEVEL OF THE WHOLE TREE
# ==============================================
# Earlier the whole game source went at `-O0`, with the justification "the build is to be
# fast, and the speed of the game code has not been a bottleneck so far". The justification
# was true exactly as long as the client did not have a single
# model: `CGrannyModelInstance` and the whole rest of EterGrnLib exited
# at once through `IsEmpty()`, so there was nothing to execute.
#
# Since the models are in the package and drawn in the game, the same
# path runs on every frame for every character - and the client dropped
# to one frame per a dozen seconds. That is NOT the fault of route B: it is
# the game computing skinning, matrices and bone trees with unoptimized code.
#
# A playable client has to be built with optimization - otherwise it has no way
# to run.
#
# `--o0` stays for times of digging, when build time matters, not
# the speed of the result.
OPT_LEVEL = '-O0' if '--o0' in sys.argv else '-O2'

# How many compilations at once. As many as cores - each sits in a separate
# `em++` subprocess, so the threads only wait for them.
CORES = max(2, (os.cpu_count() or 4))

# Libraries built next to `GameLib`. The same list serves later to
# recognize which files did NOT compile - so that compilation
# and classification look at exactly the same set.
# Files we do NOT compile - and that is a decision, not a backlog.
#
# We replaced them at the HEADER level (compat/tree/), so their bodies
# implement classes that no longer exist. Counting them as waiting would be
# untrue: they will never compile and do not need to.
#
# This is the third category next to waiting and writing: REPLACED.
NOT_COMPILED = {
    # --- the process scanner (anti-cheat) -----------------------------------
    # The only trace of this module in the whole client is a COMMENTED-OUT line
    # in `UserInterface.cpp`:
    #
    #     //ProcessScanner_ReleaseQuitEvent();
    #
    # Nobody calls either `ProcessScanner_Create` or `ProcessScanner_Destroy`.
    # This scanner is dead ALREADY IN TMP4 - and its only effect on the port
    # was to demand three Windows thread symbols (`_beginthread`,
    # `CreateEventA`, `SetEvent`) for browsing the process list, which
    # does not exist in the browser.
    'UserInterface/ProcessScanner.cpp':
        'dead already in TMP4 - its only call is commented out, and a process '
        'list does not exist in the browser',

    # --- marshal: a copy of marshal.c from CPython 2 ------------------------
    # `ScriptLib/PythonMarshal.cpp` is CPython 2's `marshal.c` pasted into the Metin2
    # tree, with an underscore added to the names. It reads Python 2
    # structures: the sixteen-bit `argcount` of a code object, `ob_size` and `ob_digit`
    # of an integer, `PyEval_GetRestricted`.
    #
    # Earlier it lay on the "does not compile" list and looked like ordinary
    # work: five errors, all of the kind "this name is called differently in Python 3".
    # Fixing them would have been the WORST way out - the file would
    # compile, link, run and read Python 2 data into
    # Python 3 structures.
    #
    # The port embeds CPython 3, which has its OWN marshal.
    # The three functions `ScriptLib/StdAfx.h` demands are exactly three
    # CPython functions with a leading underscore - they are provided by
    # `compat/python_marshal_web.cpp`.
    'ScriptLib/PythonMarshal.cpp':
        'a copy of marshal.c from CPython 2 - the port embeds CPython 3, '
        'which has its own marshal; replaced by '
        'compat/python_marshal_web.cpp',

    'eterLib/GrpDetector.cpp':
        'replaced by our tree/eterLib/GrpDetector.h - graphics card '
        'detection, which has nothing to detect in the browser',
    'eterLib/IME.cpp':
        'replaced by our tree/eterLib/IME.h and compat/ime_web.cpp - '
        'of the 2302 lines of the original most are IMM32, and composing Asian '
        'characters is done in the browser by the browser itself, through the '
        'compositionstart/update/end events',

    # --- sound: the whole library stands on the Miles Sound System ----------
    # We already replaced its HEADERS (`SoundManager.h`, `SoundData.h`), because Miles
    # sat in them only in the private part. The bodies of these files call Miles
    # DIRECTLY - `HPROVIDER`, `AIL_*` - and no compatibility layer will remove that.
    # So the sound layer is our own (compat/sound_web.cpp), not a port of milesLib.
    'milesLib/SoundBase.cpp':            'Miles - replaced by the port sound layer',
    'milesLib/SoundData.cpp':            'Miles - replaced by the port sound layer',
    'milesLib/SoundInstance2D.cpp':      'Miles - replaced by the port sound layer',
    'milesLib/SoundInstance3D.cpp':      'Miles - replaced by the port sound layer',
    'milesLib/SoundInstanceStream.cpp':  'Miles - replaced by the port sound layer',
    'milesLib/SoundManager.cpp':         'Miles - replaced by the port sound layer',
    'milesLib/SoundManager2D.cpp':       'Miles - replaced by the port sound layer',
    'milesLib/SoundManager3D.cpp':       'Miles - replaced by the port sound layer',
    'milesLib/SoundManagerStream.cpp':   'Miles - replaced by the port sound layer',

    # --- the intro movie: a whole DirectShow filter graph -------------------
    # In the browser it corresponds to ONE <video> tag, and grabbing a frame
    # is drawImage onto a canvas.
    'UserInterface/MovieMan.cpp':
        'a DirectShow filter graph - in the browser one <video> tag',
    'UserInterface/PythonApplicationLogo.cpp':
        'playing the intro movie through the same filter graph',

    # --- real-money shop (ItemShop) ------------------------------------------
    'CWebBrowser/CWebBrowser.c':
        'an embedded Internet Explorer window through OLE. In the browser ItemShop '
        'is a plain HTML window or frame - the COM layer has nothing '
        'to reproduce',

    # --- SpeedTree trees: NO LONGER EXCLUDED ---------------------
    # Earlier both files stood here with the reason "they require
    # D3DXAssembleShader, i.e. the Direct3D shader assembler". The reason was
    # true, but the conclusion wrong.
    #
    # `VertexShaders.h` CHECKS the assembler's result and on an error draws
    # the trees without a shader. That is TMP4'S OWN fallback path - the authors
    # anticipated cards that could not do shaders. So it was enough for
    # `D3DXAssembleShader` to REFUSE HONESTLY (compat/d3dx8_shader.cpp),
    # and both files compile without a single change in the source.
    #
    # The second blocking error was on OUR side: we had `D3DXVECTOR3(CONST float*)`
    # as `explicit`, and in real D3DX8 it is not. SpeedTree
    # assigns directly the result of `GetTreePosition()`, which is `const float*`.
    # Fixed in the header - for all seven types, not just this one.

    # --- keyboard: the whole file is about DirectInput ----------------------
    # `Input.cpp` creates a DirectInput device, sets the data format,
    # the cooperative level and calls `GetDeviceState` every frame. In the browser
    # there is not one of these.
    #
    # `DirectInput8Create` in our layer refuses honestly, but
    # `Input.cpp` reads that refusal as a FAILURE and returns `false` - and
    # `CPythonApplication::Create` stops the whole game on it, without a single
    # log entry, because that is the only `return false` in this function that
    # prints nothing. Found only after fixing `Traceback()`.
    #
    # Replaced by `compat/keyboard_web.cpp`, which takes the state
    # from `KeyboardEvent.code` - and that is the same concept as the DirectInput
    # scan code, so the mapping is literal, not approximate.
    'eterLib/Input.cpp':
        'the whole file is about DirectInput; replaced by compat/keyboard_web.cpp '
        'on KeyboardEvent.code events',

    'eterLib/GrpDevice.cpp':
        'creating the Direct3D device. Replaced by the port drawing layer; '
        'this file refers to classes from the replaced GrpDetector.h',
}

LIBRARIES = ('eterLib', 'eterGrnLib', 'effectLib', 'PRTerrainLib',
              'SphereLib', 'SpeedTreeLib', 'EterImageLib', 'UserInterface',
              'EterPythonLib', 'ScriptLib', 'milesLib', 'CWebBrowser',
              'eterLocale')

# CPython headers. THIS IS MEASUREMENT SCAFFOLDING: ultimately the port
# builds against a CPython compiled for emscripten, with its own
# `pyconfig.h`. Here they serve to count how much TMP4 code is ready.
# CPython headers FROM THE SAME BUILD as `lib/libpython3.13.a`.
#
# Earlier they pointed at the Python installed in the system
# and were SCAFFOLDING: they served to count how much TMP4 code is
# ready, not to build anything that works.
#
# Now there is a real `libpython` for wasm and the headers MUST
# come from the same build. That is not order for order's sake:
# the Windows `pyconfig.h` says `SIZEOF_VOID_P 8`, and the one from the wasm
# build says `4`. The difference goes through `Py_ssize_t` into EVERY
# CPython structure - `PyObject` would have a different size on the two
# sides of the link. It would give no error, just reading fields
# from shifted places.
PYTHON_INC = os.path.join(ROOT, 'build', 'port', 'python313', 'Include')

# Symbols supplied by the runtime environment at link time.
#
# The pattern alone is not enough: about fifty names got into the "contract"
# like `strlen`, `fopen`, `sinf`, `socket`, `htonl` - i.e. plain
# libc, which emscripten adds by itself. The count was **inflated** by that.
# That is why besides the pattern we ask **emscripten's system libraries** what
# they really supply - that is a measurement, not a list one has to remember.
RUNTIME = re.compile(r'^(std::|__cxa_|__gxx_|__dso_handle$|__assert_fail$'
                     r'|__stack_chk|__stack_pointer$|__indirect_function_table$'
                     r'|__dynamic_cast$|_Unwind|operator new|operator delete'
                     # `emscripten_*` are in the toolchain's JS library and are added
                     # at link time - the same as libc.
                     r'|emscripten_'
                     # `exit` is added by emscripten's glue, not by libc.a
                     r'|^exit$'
                     # THE SEVENTH FAMILY THAT LOOKS LIKE A GAP.
                     #
                     # `setjmp`/`longjmp` are not an instruction in wasm,
                     # but a rewrite of the whole function: emscripten
                     # turns the calls into `invoke_*` and adds
                     # `__wasm_setjmp`, `__THREW__`, `getTempRet0`.
                     # These names are exposed by the LINKER, not by any
                     # `.a` - so `llvm-nm` sees them as
                     # unresolved and they really are unresolved,
                     # just not for us.
                     #
                     # They came in with `compat/jpeg_decode.cpp`: `libjpeg` error
                     # handling goes through `longjmp`, because the default one
                     # ends the program.
                     r'|^__wasm_setjmp|^__THREW__$|^__threwValue$'
                     r'|^invoke_[vijfd]+$|^[gs]etTempRet0$'
                     r'|typeinfo|vtable for __cxxabi)')

SYSROOT = os.path.join(EMSDK, 'upstream', 'emscripten', 'cache', 'sysroot',
                       'lib', 'wasm32-emscripten')
SYSLIBS = ('libc.a', 'libc++.a', 'libc++abi.a', 'libcompiler_rt.a',
           'libc-mt.a', 'libdlmalloc.a', 'libsockets.a', 'libGL.a')

# Granny 3D is a COMMERCIAL library. We do not "write" its symbols - that is a separate
# decision (our own `.gr2` reader or a licence), so they count separately.
GRANNY = re.compile(r'^Granny[A-Z]')

# SpeedTree is the SECOND commercial library in this tree - and we learned
# that by MEASUREMENT, not from documentation.
#
# Earlier both SpeedTree wrapper files were excluded with the reason
# "they require the shader assembler", and the contract showed 13 symbols of
# `CSpeedTreeWrapper` and `CSpeedTreeForestDirectX8` as OUR work.
# When the assembler started refusing honestly, both files compiled without
# a single change - and the contract jumped to 50, entirely `CSpeedTreeRT`.
#
# That was not a regression, but an uncovering. Those ten symbols were never
# to be written: they were wrappers for an SDK we do not have. Counting
# them as our work UNDERSTATED the real dependency and inflated progress.
SPEEDTREE = re.compile(r'^CSpeedTreeRT::')

# Our own functions written in JavaScript through `EM_JS`. The linker
# sees them as unresolved, because their bodies are in the JS glue, not in the wasm -
# but they ARE WRITTEN. It is the sixth time the same shape of count,
# only this time the unresolved symbol is our own.
OUR_JS = re.compile(r'^(tmp4|m2w)_')   # EM_JS names move to m2w_ (Phase B2)

# Emscripten's OpenGL layer - `glDrawArrays`, `glBindTexture`,
# `emscripten_webgl_create_context` and the rest.
#
# THE MEASUREMENT that settled it: `libGL.a` from the emscripten sysroot
# has **3610 bytes and defines ONE symbol**. There is no
# `glDrawArrays` there. The bodies of these functions sit in the JAVASCRIPT GLUE
# (`library_webgl.js`) and come in only at link time, with the switch
# `-sMAX_WEBGL_VERSION=2`.
#
# This is exactly the same shape of count as with `EM_JS` above - a symbol
# unresolved, yet not missing - only this time not ours, but
# emscripten's. For the seventh time the same mistake lurked in the same place:
# adding them to "to be written" would have meant writing OpenGL.
GL_LAYER = re.compile(r'^(gl[A-Z]|emscripten_webgl_|emscripten_glGet)')

# EXTERNAL libraries we simply do not link yet. This is NOT
# work to be written - it is a missing `-l`.
#
# They got into the count for the THIRD time for the same reason (libc,
# CPython and DevIL): an unresolved symbol looks the same regardless
# of who is to supply it. That is why every such family must be
# named EXPLICITLY, not fall into the "free functions" bag.
EXTERNAL = re.compile(r'^(Py[A-Z_]|_Py|il[A-Z]|ilu[A-Z]|ilut[A-Z]|jpeg_)')
# `jpeg_*` came, when `JpegFile.cpp` started to compile.
# Every time an unblocked file pulls in a new library, its
# symbols look like a gap - and every time it has to be NAMED.

# Our deliberate non-definitions - see `compat/process.h` and `win32_compat.h`.
OUR_DECISIONS = {'_beginthreadex', '_endthreadex', 'CreateSemaphoreA',
                 'ReleaseSemaphore', 'WaitForSingleObject', 'SetThreadPriority'}


def tool_name(name):
    """Path of an emsdk tool (`em++`, `llvm-nm`...): from PATH, else from the
    EMSDK directory; exits when it is nowhere.
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


def main():
    """Stages the tree, compiles every library of LIBRARIES plus the compat
    layer to objects (in parallel), then collects the undefined symbols and
    writes the contract (build/port/gamelib_contract.md).
    """
    subprocess.check_call([sys.executable,
                           os.path.join(ROOT, 'tools', 'stage_port.py')])

    empp = tool_name('em++')
    nm = tool_name('llvm-nm')
    cxxfilt = tool_name('llvm-cxxfilt')

    def compile_all(lib, required):
        """Compiles all the library's `.cpp` files. `required` says whether an error
        is a reason to stop: for `GameLib` yes (it is to go through whole),
        for `EterLib` no - there some files **by design** will not pass,
        because that is exactly the part the graphics layer replaces.
        """
        src_dir = os.path.join(STAGE, lib)
        sources = sorted(f for f in os.listdir(src_dir)
                        if f.endswith('.cpp') or f.endswith('.c'))
        out = os.path.join(OBJDIR, lib)
        os.makedirs(out, exist_ok=True)
        ok, errors_found = 0, []

        def one_step(f):
            """Compiles one file. Separately, because the compilations run IN PARALLEL."""
            return f, subprocess.run(
                [empp, '-std=c++17', '-c', OPT_LEVEL, '-w',
                 '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
                 # `eterBase` on the path, because `UserInterface/MarkImage.cpp`
                 # pulls in "crc32.h" without a directory - that is how the project was set up
                 # in MSVC. Python: see the note at `PYTHON_INC`.
                 '-I' + os.path.join(STAGE, 'eterBase'), '-I' + PYTHON_INC,
                 os.path.join(src_dir, f), '-o',
                 os.path.join(out, os.path.splitext(f)[0] + '.o')],
                capture_output=True, text=True)

        # COMPILATIONS IN PARALLEL.
        #
        # Every file is a separate `em++` call and knows nothing of the others,
        # so the order has no meaning here at all - and at `-O2` the whole
        # serially would take a dozen minutes. Threads are enough, because all the time
        # is spent in the subprocess anyway.
        to_compile = [f for f in sources if lib + '/' + f not in NOT_COMPILED]
        with concurrent.futures.ThreadPoolExecutor(max_workers=CORES) as kPool:
            for f, r in kPool.map(one_step, to_compile):
                if r.returncode == 0:
                    ok += 1
                else:
                    errors_found.append((f, next((l for l in r.stderr.splitlines()
                                           if 'error:' in l), '?')))
        print(f'{lib}: objects {ok} | errors {len(errors_found)}')
        if errors_found and required:
            for f, e in errors_found:
                print(f'  {f}: {e}')
        return ok, errors_found

    # OBJECTS ARE COUNTED FROM SCRATCH, and that is not caution but a fix of a
    # measurement error. Symbols are collected by `os.walk` over the whole `OBJDIR`,
    # so a `.o` file from a PREVIOUS run kept counting - also
    # when its source had just gone to the replaced files.
    # It was detected on `ProcessScanner.cpp`: after listing it as dead
    # the contract did not move by a single symbol, because the old object lay in place
    # and kept supplying what it supplied.
    #
    # Without this deletion every EXCLUSION of a file is invisible in the count,
    # and that is exactly the shape of failure the internal check
    # below is about: the number looks credible and is untrue.
    if os.path.isdir(OBJDIR):
        shutil.rmtree(OBJDIR)
    os.makedirs(OBJDIR, exist_ok=True)

    print()
    _, errors_found = compile_all('GameLib', required=True)
    if errors_found:
        return 1

    # The remaining libraries go whole, as far as possible. The files that do not
    # pass are exactly the graphics layer, input and system fonts
    # - i.e. what the port's engine layer replaces anyway. Every file that PASSES
    # removes an item from the contract.
    #
    # NOTE: the number of files is NOT a measure of progress. Once adding 13
    # files removed four symbols, and one file removed a dozen of them.
    # The measure is the contract, computed anew below.
    for lib in LIBRARIES:
        compile_all(lib, required=False)

    # --- symbole ------------------------------------------------------------
    objs = []
    compat_errors = []
    for root, _d, files in os.walk(OBJDIR):
        objs += [os.path.join(root, f) for f in files if f.endswith('.o')]
    libs = [os.path.join(LIBDIR, f) for f in os.listdir(LIBDIR)] if os.path.isdir(LIBDIR) else []

    def symbol_list(file_list):
        """(undefined, defined) symbol sets of the given objects, read with llvm-nm
        in batches of 100 (the Windows command line limit).
        """
        undef, defined = set(), set()
        # IN PORTIONS, because Windows has a hard limit on command line length
        # (32767 characters). There are over a thousand objects, each with an absolute
        # path - one call exceeds the limit and comes out as
        # "The file name or extension is too long", i.e. a message
        # that says nothing. A hundred files at a time fit with a margin.
        for i in range(0, len(file_list), 100):
            r = subprocess.run([nm] + file_list[i:i + 100],
                               capture_output=True, text=True)
            for line in r.stdout.splitlines():
                cols = line.split()
                if len(cols) >= 2 and cols[-2] == 'U':
                    undef.add(cols[-1])
                elif len(cols) >= 3 and cols[-2] in ('T', 'W', 'D', 'V', 'B'):
                    defined.add(cols[-1])
        return undef, defined

    # The compatibility layer belongs to the objects, not to the contract - without it
    # `CopyFileA`, `CharNextExA` and the critical-section functions would look
    # missing, although they are written.
    compat_o = os.path.join(OBJDIR, 'win32_compat.o')
    r = subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w', '-I' + COMPAT,
                    os.path.join(COMPAT, 'win32_compat.cpp'), '-o', compat_o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(compat_o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(compat_o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(compat_o):
        objs.append(compat_o)

    # The "nothing here" layer - functions whose true
    # answer in the browser is "there is no such thing". It belongs to the objects,
    # because it is WRITTEN, not to the contract.
    none_o = os.path.join(OBJDIR, 'platform_none.o')
    r = subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w', '-I' + COMPAT,
                    os.path.join(COMPAT, 'platform_none.cpp'), '-o', none_o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(none_o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(none_o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(none_o):
        objs.append(none_o)

    # Crash handling - replaces `EterBase/error.cpp`, which is based on SEH.
    crash_o = os.path.join(OBJDIR, 'platform_crash.o')
    r = subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w', '-I' + COMPAT,
                    os.path.join(COMPAT, 'platform_crash.cpp'), '-o', crash_o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(crash_o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(crash_o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(crash_o):
        objs.append(crash_o)

    # Drawing text through a 2D canvas.
    text_o = os.path.join(OBJDIR, 'platform_text.o')
    r = subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w', '-I' + COMPAT,
                    os.path.join(COMPAT, 'platform_text.cpp'), '-o', text_o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(text_o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(text_o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(text_o):
        objs.append(text_o)

    # Converting bytes to wide characters.
    codec_o = os.path.join(OBJDIR, 'platform_codec.o')
    r = subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w', '-I' + COMPAT,
                    os.path.join(COMPAT, 'platform_codec.cpp'), '-o', codec_o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(codec_o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(codec_o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(codec_o):
        objs.append(codec_o)

    # Sound on Web Audio - replaces nine `milesLib` files.
    # As the only file of the compatibility layer it needs paths into the TMP4 tree:
    # it implements `CSoundManager`, so it must see its header, the game's pack
    # and the clock.
    sound_o = os.path.join(OBJDIR, 'sound_web.o')
    r = subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w',
                    '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
                    '-I' + os.path.join(STAGE, 'eterBase'),
                    os.path.join(COMPAT, 'sound_web.cpp'), '-o', sound_o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(sound_o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(sound_o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(sound_o):
        objs.append(sound_o)

    # A text field without IMM32 - replaces `eterLib/IME.cpp`.
    ime_o = os.path.join(OBJDIR, 'ime_web.o')
    r = subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w',
                    '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
                    '-I' + os.path.join(STAGE, 'eterBase'),
                    os.path.join(COMPAT, 'ime_web.cpp'), '-o', ime_o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(ime_o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(ime_o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(ime_o):
        objs.append(ime_o)

    # The shop window, the intro movie and the keyboard state,
    # plus the mouse and characters as window messages.
    for name in ('webbrowser_web', 'movie_web', 'input_web', 'events_web',
                  'network_web'):
        o = os.path.join(OBJDIR, name + '.o')
        subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w',
                        '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
                        '-I' + os.path.join(STAGE, 'eterBase'),
                        os.path.join(COMPAT, name + '.cpp'), '-o', o],
                       capture_output=True, text=True)
        if os.path.exists(o):
            objs.append(o)

    # The drawing layer. Five files, of which THREE are pure
    # and checked by a test without a graphics card:
    #   d3d8_fixedfunc - builds GLSL from the fixed-function pipeline,
    #   d3d8_states     - translates constants and texture memory,
    #   d3d8_fvf       - takes an FVF code apart into a vertex layout,
    # and two add what cannot be checked without a card:
    #   gl_device      - the IDirect3DDevice8 device on WebGL 2 (earlier
    #                    the file d3d8_gl.cpp - git mv),
    #   grpdevice_gl   - CGraphicDevice, i.e. the life cycle.
    # The last two - `dxt` and `codepages` - came from an external
    # agent and are PURE: nothing but the standard library,
    # so they are checked by a test run in node.
    #
    # NOTE ON THE COUNT: as long as they were not here, `M2W_DecodeDxt`
    # and three code page functions came out in the contract as MISSING,
    # although they lie written in `compat/`. The contract measures what the script
    # builds - a file skipped in this list does not exist for the measurement.
    # That is the same shape of error as the stale `.o` files.
    for name in ('d3d8_fixedfunc', 'd3d8_states', 'd3d8_fvf',
                  'gl_device', 'gl_textures', 'gl_buffers', 'gl_shaders',
                  'gl_render_states', 'gl_render_target', 'grpdevice_gl',
                  'd3dx8_matrixstack', 'grpdetector_gl',
                  'd3d8_pixels', 'd3dx8_textures', 'd3d8_image',
                  'dxt', 'jpeg_decode', 'codepages',
                  'd3dx8_shader',
                  'paths_web', 'devil_web', 'granny_web',
                  'gr2_oodle1', 'gr2_file', 'gr2_to_granny', 'granny_pose',
                  'granny_control', 'custom_draw', 'frame_stats', 'webfs_web',
                  'speedtree_web', 'cursor_web', 'd3d8_factory',
                  'keyboard_web', 'stubs', 'properties_web',
                  'locale_web'):
        o = os.path.join(OBJDIR, name + '.o')
        # THE OPTIMIZATION LEVEL DEPENDS ON THE FILE.
        #
        # The whole tree goes at `-O0`, because the build is to be fast, and the speed
        # of the game code has not been a bottleneck so far. The `.gr2` reader is another
        # matter: Oodle1 unpacking is an arithmetic coder that goes
        # BIT BY BIT, and deforming vertices by bones runs
        # on every frame for every character. Measured on 40 corpus
        # models: `-O0` 0.663 s, `-O2` 0.173 s - FOUR TIMES faster.
        # When monsters come into view that is the difference between
        # a stutter and none.
        #
        # THE GL LAYER IS ON THIS LIST FOR ANOTHER REASON AND HONESTLY NOT
        # MEASURED. It runs on every draw, so `-O2` should help -
        # but it cannot be checked locally, because the browser panel throttles
        # `requestAnimationFrame` to about one frame per second. Measured:
        # the frame itself computes in 0.2-0.9 ms, and there are 0.8 calls per second, so
        # a local "drop to 1 FPS" says nothing about the client. A measurement of drawing
        # speed can come only from the user's real browser.
        fast = name in ('gr2_oodle1', 'gr2_file', 'gr2_to_granny',
                            'granny_pose', 'granny_web', 'granny_control',
                            'gl_device', 'gl_textures', 'gl_buffers', 'gl_shaders',
                            'gl_render_states', 'gl_render_target', 'd3d8_fixedfunc', 'd3d8_states',
                            'd3d8_fvf', 'grpdevice_gl', 'd3d8_pixels',
                            'd3dx8_matrixstack')
        # `-sUSE_LIBJPEG=1` is NEEDED here ALREADY AT COMPILE TIME, not
        # only at link time: without this flag emscripten does not
        # expose the port's headers and `#include <jpeglib.h>`
        # in `d3d8_image.cpp` has nothing to find.
        r = subprocess.run([empp, '-std=c++17', '-c',
                            '-O2', '-w',
                            '-sUSE_LIBJPEG=1',
                            '-I' + COMPAT, '-I' + STAGE, '-I' + EXTERN,
                            '-I' + os.path.join(STAGE, 'eterBase'),
                            os.path.join(COMPAT, name + '.cpp'), '-o', o],
                           capture_output=True, text=True)
        # A COMPATIBILITY LAYER FILE THAT DOES NOT COMPILE MUST SHOUT.
        #
        # Earlier the output of `em++` was thrown away here, and the object added "if it
        # exists". The effect measured on ourselves: a typo in `d3d8_gl.cpp`
        # (today `gl_device.cpp` - git mv; a real line end
        # in the middle of a string) made the file
        # NOT compile, the build said "0 errors", the link
        # said "no unexpected ones", and the client crashed only
        # in the browser on "missing function: M2W_CreateGlDevice".
        #
        # Three green lights in a row on a broken build - exactly the
        # same shape as the file skipped in the list and the stale `.o`
        # counted as fresh. An error that does not shout where it arises
        # comes back later disguised as something else.
        if r.returncode != 0:
            compat_errors.append(name)
            print('  COMPILE ERROR compat/' + name + '.cpp:')
            for row in r.stderr.splitlines():
                if 'error:' in row:
                    print('    ' + row.strip())
        if os.path.exists(o):
            objs.append(o)

    if compat_errors:
        raise SystemExit(
            'THE COMPATIBILITY LAYER DID NOT BUILD: ' + ', '.join(compat_errors) +
            ' | a link without these files would give a client that crashes '
            'only in the browser - see the note above.')

    # `WebFs` - the reader of the streamed corpus. Written from scratch
    # from the M2WF format (build_corpus.py);
    # it lies in `compat/` (webfs.h/.cpp), because it is our layer and goes into
    # publication.
    o = os.path.join(OBJDIR, 'webfs.o')
    r = subprocess.run([empp, '-std=c++17', '-c', '-O2', '-w',
                    os.path.join(COMPAT, 'webfs.cpp'),
                    '-o', o],
                   capture_output=True, text=True)
    # THE RESULT MUST SHOUT - see the note at the main list.
    if r.returncode != 0:
        compat_errors.append(os.path.basename(o)[:-2])
        print('  COMPILE ERROR ' + os.path.basename(o)[:-2] + '.cpp:')
        for row in r.stderr.splitlines():
            if 'error:' in row:
                print('    ' + row.strip())
    if os.path.exists(o):
        objs.append(o)

    # The Python 2 -> 3 bridge also belongs to the objects, not to the contract.
    # Together with it the marshal: the three functions `ScriptLib/StdAfx.h` demands,
    # built on CPython's own marshal instead of on the pasted copy of
    # `marshal.c` from Python 2 (see NOT_COMPILED).
    for name in ('python2_bridge', 'python_marshal_web'):
        o = os.path.join(OBJDIR, name + '.o')
        subprocess.run([empp, '-std=c++17', '-c', OPT_LEVEL, '-w', '-I' + COMPAT,
                        '-I' + PYTHON_INC,
                        os.path.join(COMPAT, name + '.cpp'), '-o', o],
                       capture_output=True, text=True)
        if os.path.exists(o):
            objs.append(o)

    u_gl, d_gl = symbol_list(objs)
    _, d_lib = symbol_list(libs) if libs else (set(), set())

    sys_libs = [os.path.join(SYSROOT, f) for f in SYSLIBS
                if os.path.exists(os.path.join(SYSROOT, f))]
    _, d_sys = symbol_list(sys_libs) if sys_libs else (set(), set())
    print(f'emscripten system libraries: {len(sys_libs)} files, '
          f'{len(d_sys)} symbols')

    missing = sorted(u_gl - d_gl - d_lib - d_sys)

    r = subprocess.run([cxxfilt], input='\n'.join(missing), capture_output=True, text=True)
    readable = [l.strip() for l in r.stdout.splitlines() if l.strip()]

    groups = collections.defaultdict(list)
    runtime, decisions, granny, external, our_js = [], [], [], [], []
    speedtree = []
    gl_layer = []
    for l in readable:
        if GL_LAYER.match(l):
            gl_layer.append(l)
            continue
        if RUNTIME.match(l):
            runtime.append(l)
            continue
        if GRANNY.match(l):
            granny.append(l)
            continue
        if SPEEDTREE.match(l):
            speedtree.append(l)
            continue
        if OUR_JS.match(l):
            our_js.append(l)
            continue
        if EXTERNAL.match(l):
            external.append(l)
            continue
        if l.split('(')[0].split(' ')[-1] in OUR_DECISIONS:
            decisions.append(l)
            continue
        m = re.match(r'(?:[\w:<>,\s\*&]+?\s)?([A-Za-z_]\w*)::', l)
        groups[m.group(1) if m else '(free functions and data)'].append(l)

    # -----------------------------------------------------------------------
    # TWO KINDS OF GAP that must not be mixed
    # -----------------------------------------------------------------------
    # "A symbol is missing because a file does not compile" is SOMETHING ELSE than "a symbol
    # is missing because nobody wrote it". The first waits in the queue, the second
    # is work.
    #
    # I decide it by checking whether one of the files that did NOT
    # compile contains definitions of methods of this class (`CIME::`). If so -
    # the code exists and waits.
    waiting, raised = {}, {}

    # I recognize the files that did not compile by the ABSENCE of an object file,
    # not by what the compiling function returned. The difference came out:
    # the version based on the returned list gave zero matches, and the same
    # classification computed separately gave 39. The state of the object directory is
    # harder evidence than my bookkeeping in memory.
    contents = {}
    for lib in LIBRARIES:
        src = os.path.join(STAGE, lib)
        out = os.path.join(OBJDIR, lib)
        if not os.path.isdir(src):
            continue
        done = ({os.path.splitext(f)[0] for f in os.listdir(out)}
                  if os.path.isdir(out) else set())
        for f in os.listdir(src):
            if not f.endswith(('.cpp', '.c')):
                continue
            if os.path.splitext(f)[0] in done:
                continue
            if lib + '/' + f in NOT_COMPILED:
                continue
            try:
                with open(os.path.join(src, f), encoding='utf-8',
                          errors='ignore') as fh:
                    contents[f] = fh.read()
            except OSError:
                pass
    print(f'files that did not compile: {len(contents)}')

    for cls_name, syms in groups.items():
        if cls_name != '(free functions and data)':
            where = [file_name for file_name, text_str in contents.items()
                     if cls_name + '::' in text_str]
            (waiting if where else raised)[cls_name] = (len(syms), where)
            continue

        # Free functions and data have to be considered ONE BY ONE, not in a heap.
        # Once this whole bag went into "to be written", although it contained
        # `LoadMultipleTextData`, `GRAPHICS_CAPS_*` or `g_rcBrowser` -
        # things defined in TMP4 files that simply do not
        # compile. The same counting error as with libc and CPython,
        # only in a third disguise.
        for sym in syms:
            name = sym.split('(')[0].split(' ')[-1]
            where = [file_name for file_name, text_str in contents.items()
                     if name in text_str]
            key_name = f'{name} (free)'
            (waiting if where else raised)[key_name] = (1, where)

    # INTERNAL CHECK.
    #
    # It comes: a regular expression had the byte 0x08 in it
    # (the shell turned `\b` into a backspace character), so it matched
    # nothing. The script kept counting and printed a number that looked
    # credible. The error was INVISIBLE also in `grep` and `sed`.
    #
    # REWRITTEN, and that matters more than the fix itself.
    # The two previous versions POINTED AT a specific symbol:
    # `GRAPHICS_CAPS_CAN_NOT_DRAW_LINE` and `CIME`.
    # Each time the file in which that symbol sat later moved
    # to the REPLACED ones - its symbols rightly landed in "to be written",
    # and the check crashed the program for a correct result. A sentinel tied
    # to one name ages together with the count it guards.
    #
    # This version checks the MECHANISM, not a name, and that is why it will not age.

    # (a) A positive check on the matching itself, on made-up data.
    #     Exactly this was broken by the 0x08 byte: a match that matches
    #     nothing and reports no error.
    if 'CSentinel::' not in 'void CSentinel::Method() {}':
        raise SystemExit(
            'THE CLASSIFIER DOES NOT WORK: matching a class name does not find '
            'it even in a sentence made up right here. Not writing the '
            'contract, because it would be untrue.')

    # (b0) No file LISTED AS REPLACED may have an object.
    #      This is a check on the fix: if the deletion of `OBJDIR`
    #      ever disappeared or stopped working, a stale `.o` would supply
    #      the symbols of a file we deliberately do not compile - and the count
    #      would show it as done.
    for printed in NOT_COMPILED:
        lib, file_path = printed.split('/', 1)
        obj = os.path.join(OBJDIR, lib, os.path.splitext(file_path)[0] + '.o')
        if os.path.exists(obj):
            raise SystemExit(
                f'THE CLASSIFIER DOES NOT WORK: {printed} is listed as '
                f'replaced, but its object lies in {obj}. Not writing the '
                'contract, because it would be untrue.')

    # (b) A check on real data: if there ARE files that did not
    #     compile, at least one symbol must be waiting for them.
    #     An empty "waits for a file" column with a non-empty list of files means
    #     that reading the sources or the matching has stopped working.
    if contents and not waiting:
        raise SystemExit(
            f'THE CLASSIFIER DOES NOT WORK: {len(contents)} files did not '
            'compile, yet no symbol came out as "waits for a file". '
            'Not writing the contract, because it would be untrue.')

    n = sum(len(v) for v in groups.values())
    with open(CONTRACT, 'w', encoding='utf-8') as f:
        f.write('# `GameLib` contract - what the engine layer has to provide\n\n')
        f.write('**Generated by measurement** by `tools/build_gamelib.py`, not by hand.\n')
        f.write('`stage_port.py` assembles the tree, 59 `GameLib` files compile to\n')
        f.write('objects, `llvm-nm` lists the unresolved symbols, and what our libraries\n')
        f.write('already provide is subtracted from them.\n\n')
        f.write('The symbols fall into FOUR kinds and mixing them would inflate the\n')
        f.write('count - it did, by about fifty names from `libc`:\n\n')
        f.write('| kind | count | who provides |\n|---|---|---|\n')
        f.write(f'| **engine-layer contract** | **{n}** | us, in the engine layer |\n')
        f.write(f'| **Granny 3D** | **{len(granny)}** | a COMMERCIAL library - a separate decision |\n')
        f.write(f'| runtime environment | {len(runtime)} | `libc`, `libc++` and the toolchain |\n')
        f.write(f'| our deliberate non-definitions | {len(decisions)} | NOBODY - they are to stop the linker |\n\n')
        f.write('The last row is the threads and semaphores from `compat`, declared without\n')
        f.write('bodies on purpose: `AreaLoaderThread` waits on a semaphore for data\n')
        f.write('from the loading thread, so a stub saying "done" would make the game read\n')
        f.write('a map that is not loaded.\n\n')
        f.write('```\n' + '\n'.join(sorted(decisions)) + '\n```\n\n')
        f.write('### Granny 3D\n\n')
        f.write('This is the **API of a commercial library** whose `.lib` is not in the repository.\n')
        f.write('It is not part of the engine-layer contract, because it is not written - one has\n')
        f.write('either to have a licence or to write an own `.gr2` reader. A separate decision\n')
        f.write('and a separate count.\n\n')
        f.write('```\n' + '\n'.join(sorted(granny)) + '\n```\n\n')
        n_waiting = sum(how_many for how_many, _ in waiting.values())
        n_to_write = sum(how_many for how_many, _ in raised.values())
        f.write(chr(10).join(['---', '', '## REPLACED files - not compiled on purpose', '', 'Each for its own reason, given next to it. Some have our own '
        'header here, so their bodies implement classes that no longer exist; others '
        'stand on a library that is not in the browser and will not be; still '
        'others are dead already in TMP4 itself. Counting them as WAITING would be '
        'untrue - they wait for nothing.', '']))
        for file_path, reason in sorted(NOT_COMPILED.items()):
            f.write('- `' + file_path + '` - ' + reason + chr(10))
        f.write(chr(10))
        f.write('---\n\n## Two kinds of gap\n\n')
        f.write('"A symbol is missing because a file does not compile" is **something else** than\n')
        f.write('"a symbol is missing because nobody wrote it". The first waits in the queue,\n')
        f.write('the second is work. Decided by checking whether one of the files that\n')
        f.write('did not compile contains definitions of methods of that class.\n\n')
        f.write(f'| kind | symbols |\n|---|---|\n')
        f.write(f'| **waits for a file that does not compile** | {n_waiting} |\n')
        f.write(f'| **to be written** | {n_to_write} |\n\n')
        f.write('### Waits for a file\n\n| class | symbols | file |\n|---|---|---|\n')
        for k, (how_many, where) in sorted(waiting.items(), key=lambda x: -x[1][0]):
            f.write(f'| `{k}` | {how_many} | {", ".join(where[:3])} |\n')
        f.write('\n### To be written\n\n| class / group | symbols |\n|---|---|\n')
        for k, (how_many, _g) in sorted(raised.items(), key=lambda x: -x[1][0]):
            f.write(f'| `{k}` | {how_many} |\n')

        f.write('\n---\n\n## engine-layer contract by class\n\n| class / group | symbols |\n|---|---|\n')
        for k, v in sorted(groups.items(), key=lambda x: -len(x[1])):
            f.write(f'| `{k}` | {len(v)} |\n')
        f.write('\n---\n\n## Full list\n')
        for k, v in sorted(groups.items(), key=lambda x: -len(x[1])):
            f.write(f'\n### {k} ({len(v)})\n\n```\n')
            for s in sorted(v):
                f.write(s + '\n')
            f.write('```\n')

    print(f'\ncontract {n} = waits for a file {n_waiting} + to be written {n_to_write}')
    print(f'Granny {len(granny)} | SpeedTree {len(speedtree)} | external {len(external)} | '
          f'our JS {len(our_js)} | '
          f'GL layer {len(gl_layer)} | '
          f'runtime {len(runtime)} | our decisions {len(decisions)}')
    print(f'written: {os.path.relpath(CONTRACT, ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
