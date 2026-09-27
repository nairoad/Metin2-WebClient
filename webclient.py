#!/usr/bin/env python3
"""webclient.py - one command for the whole WebClient build.

    python webclient.py doctor               what is missing, and how to fix it
    python webclient.py steps                the build steps, in order
    python webclient.py build                every step, stopping at the first failure
    python webclient.py build --from data    from one step to the end
    python webclient.py build --only link    one step
    python webclient.py build --skip trees   leave optional steps out
    python webclient.py check                the quality gates against the recorded baselines
    python webclient.py check --record       record the baselines (once, from a build you trust)
    python webclient.py serve [--port 8731]  the built client on http://127.0.0.1:<port>/client.html,
                                             with the WebSocket bridge (--no-bridge: without)
    python webclient.py bridge               only the bridge (bridge/, allowlist from serverInfo.py)
    python webclient.py package              dist/site/ - the files to upload (tools/package.py)

Every step runs one of the existing tools in tools/ - this file only orders
them, reads their exit codes and output, and explains a failure in one
sentence. The paths come from webclient.toml (see webclient.example.toml and
`python tools/workspace.py`). The full output of `build` also goes to
build/webclient-build.log.
"""

import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(ROOT, 'tools')
sys.path.insert(0, TOOLS)
import workspace                                             # noqa: E402

LOG = os.path.join(ROOT, 'build', 'webclient-build.log')

# Output lines that mean a step failed even when its exit code says otherwise
# (a compile error in one file, a patch that no longer matches the sources).
FAILURE = re.compile(r'Traceback \(most recent call last\)|DID NOT HIT|AMBIGUOUS|'
                     r'\berrors:? [1-9]|NOT BUILT \(|BUILD FAILED')

# What to do when a step fails, by the text of its output.
HINTS = [
    (re.compile(r'Traceback \(most recent call last\)'),
     'a tool stopped with a Python error - the last line of the traceback says what, the line above it where'),
    (re.compile(r'DID NOT HIT|AMBIGUOUS'),
     'a source patch does not match your client sources - tools/stage_port.py names the patch '
     'and the file; adapt the patch to your fork (the patch text says what it fixes and why)'),
    (re.compile(r'not found - add emsdk|em\+\+.*not found|emcc.*not found', re.I),
     'emsdk is not where [tools] emsdk says - run `python webclient.py doctor`'),
    (re.compile(r'no Python with lib2to3|No module named .lib2to3'),
     'the Python 2 -> 3 rewrite needs Python 3.12 or older - set [tools] python2to3'),
    (re.compile(r'\berrors:? [1-9]|NOT BUILT \(|error:'),
     'a source file does not compile - the lines with `error:` above name the file and the line'),
]


# ---------------------------------------------------------------------------
# doctor: every input and tool, with one sentence on how to fix what is missing
# ---------------------------------------------------------------------------

def run_quietly(args):
    """Runs a command and returns (exit code, output); (None, reason) when it cannot start."""
    try:
        r = subprocess.run(args, capture_output=True, text=True, errors='replace', timeout=60)
        return r.returncode, r.stdout + r.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)


def emsdk_tool(name):
    """The path of an emsdk tool (em++, emar, node) or None."""
    candidates = [os.path.join(workspace.EMSDK, 'upstream', 'emscripten', name + ext)
                  for ext in ('.exe', '.bat', '')]
    if name == 'node':
        node_root = os.path.join(workspace.EMSDK, 'node')
        if os.path.isdir(node_root):
            for d in sorted(os.listdir(node_root), reverse=True):
                candidates.append(os.path.join(node_root, d, 'bin', 'node.exe' if os.name == 'nt' else 'node'))
    for p in candidates:
        if os.path.isfile(p):
            return p
    return shutil.which(name)


def check_python():
    """This interpreter: 3.11+ for tomllib."""
    ok = sys.version_info[:2] >= (3, 11)
    return ok, 'Python %d.%d.%d' % sys.version_info[:3], 'install Python 3.11 or newer'


def check_source():
    """The client sources: source/<library> and extern/include."""
    need = [os.path.join(workspace.SOURCE_LIBRARIES, d) for d in ('GameLib', 'UserInterface', 'EterPack')]
    need.append(os.path.join(workspace.EXTERN_INCLUDE, 'cryptopp'))
    missing = [p for p in need if not os.path.isdir(p)]
    return (not missing, workspace.SOURCE if not missing else 'missing ' + missing[0],
            'set [inputs] source to the directory with source/ and extern/ of your client sources')


def check_packs():
    """The game packs (.eix/.epk)."""
    n = len([f for f in os.listdir(workspace.PACKS) if f.lower().endswith('.eix')]) if os.path.isdir(workspace.PACKS) else 0
    return n > 0, '%d packs in %s' % (n, workspace.PACKS), 'set [inputs] client (or [inputs] packs) to your installed game client'


def check_emsdk():
    """Emscripten: em++ and the node that comes with it."""
    empp, node = emsdk_tool('em++'), emsdk_tool('node')
    if not empp:
        return False, 'no em++ under ' + workspace.EMSDK, 'install emsdk (emscripten.org) and set [tools] emsdk'
    code, out = run_quietly([empp, '--version'])
    version = out.splitlines()[0] if code == 0 and out else '?'
    return bool(node), version + ('' if node else ' - but no node'), 'run `emsdk install latest && emsdk activate latest`'


def check_lzo():
    """The LZO 2.10 sources: present, or downloaded by the `lzo` step."""
    import build_lzo
    ok = os.path.isdir(build_lzo.LZO_SRC) or os.path.isfile(build_lzo.LZO_TAR)
    return ok, 'LZO 2.10 ' + ('present' if ok else 'not here yet'), \
        'the lzo step downloads it (%s) - needs the network once' % build_lzo.LZO_URL


def check_python_wasm():
    """CPython 3.13 built for wasm: the libraries and the headers of the same build."""
    need = [os.path.join(workspace.LIBDIR, n) for n in ('libpython3.13.a', 'libmpdec.a', 'libHacl_Hash_SHA2.a')]
    need.append(os.path.join(workspace.PORT, 'python313', 'Include', 'Python.h'))
    missing = [p for p in need if not os.path.isfile(p)]
    return (not missing, 'CPython 3.13 for wasm ' + ('present' if not missing else 'missing: ' + os.path.relpath(missing[0], ROOT)),
            'the `python` build step downloads it (a prebuilt archive, SHA-256 checked) - '
            'see docs/BUILDING.md section 5')


def check_python_build():
    """What `tools/build_python.py --from-source` needs: Python 3.13, sh, make."""
    import build_python
    ok, problems = build_python.requirements()
    return ok, 'can build CPython for wasm from source' if ok else problems[0], \
        'only for building it yourself (--from-source); the `python` step downloads a prebuilt one'


def check_stdlib():
    """The CPython 3.13 standard library packed into the client data."""
    import build_client_data
    p = build_client_data.STDLIB_SOURCE
    ok = os.path.isfile(os.path.join(p, 'encodings', '__init__.py'))
    return ok, p, 'run this with Python 3.13, or set M2W_PYTHON_LIB to a CPython 3.13 Lib/ directory'


def check_2to3():
    """A Python that still has lib2to3 (for the Python 2 game scripts)."""
    p = workspace.PYTHON_2TO3
    if not os.path.isfile(p):
        return False, 'missing ' + p, 'install Python 3.12 or older and set [tools] python2to3'
    code, out = run_quietly([p, '-c', 'import lib2to3, sys; print(sys.version.split()[0])'])
    return code == 0, (p + ' ' + out.strip()) if code == 0 else 'no lib2to3 in ' + p, 'point [tools] python2to3 at Python 3.12 or older'


def check_pillow():
    """Pillow, for the mouse cursors (tools/cursors.py)."""
    try:
        import PIL
        return True, 'Pillow ' + PIL.__version__, ''
    except ImportError:
        return False, 'missing', 'pip install pillow'


def check_font():
    """The UI font registered as Tahoma."""
    ok = bool(workspace.FONT) and os.path.isfile(workspace.FONT)
    return ok, workspace.FONT or '(none)', 'set [inputs] font to a tahoma.ttf'


def check_server():
    """The game server address (written into the client's serverInfo.py)."""
    ok = bool(workspace.server_address())
    return ok, 'set' if ok else 'not set', 'set [server] address in webclient.toml'


def check_windows_gdi():
    """Windows GDI, for the pixel-exact UI fonts (tools/bake_fonts.py)."""
    return sys.platform == 'win32', sys.platform, 'bake the fonts on a Windows machine (without them text is drawn by the browser)'


def check_msvc():
    """MSVC x86, for the SpeedTree baker and the .gr2 repair."""
    ok = os.path.isfile(workspace.VCVARS32)
    return ok, workspace.VCVARS32 if ok else 'missing', 'install Visual Studio Build Tools 2022 (C++, x86) or set [tools] vcvars32'


def check_speedtree():
    """The SpeedTree RT SDK (library and DLL) from your client, for baking the trees."""
    import bake_speedtree
    missing = [p for p in (bake_speedtree.LIB, bake_speedtree.DLL) if not os.path.isfile(p)]
    return not missing, 'present' if not missing else 'missing ' + missing[0], \
        'SpeedTreeRT.lib (extern/library) and SpeedTreeRT.dll (client) - without them no trees'


def check_granny():
    """granny2.dll from your client, for re-saving the .gr2 files our decoder rejects."""
    import repair_gr2
    ok = os.path.isfile(repair_gr2.DLL)
    return ok, repair_gr2.DLL, 'copy granny2.dll into your client directory - without it ~1% of models stay missing'


def check_bridge():
    """node and the bridge's one dependency (the `ws` library), for serve/bridge."""
    node = emsdk_tool('node')
    ws = os.path.isfile(os.path.join(ROOT, 'bridge', 'node_modules', 'ws', 'package.json'))
    if not node:
        return False, 'no node', 'install emsdk (it brings node) or Node.js 20+'
    return ws, 'bridge ready' if ws else 'bridge/node_modules missing', \
        'cd bridge && npm ci   (installs the WebSocket library pinned in package-lock.json)'


def check_measure():
    """The measurement harness of the probe/behaviour gates: puppeteer-core
    (tools/browser, `npm ci`) and Microsoft Edge."""
    harness = os.path.join(ROOT, 'tools', 'browser')
    lib = os.path.isfile(os.path.join(harness, 'node_modules', 'puppeteer-core', 'package.json'))
    edge = os.path.isfile(r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')
    if not lib:
        return False, 'tools/browser/node_modules missing', 'cd tools/browser && npm ci'
    return edge, 'harness ready' if edge else 'no Microsoft Edge', 'install Microsoft Edge (the harness drives it)'


# name -> (check, required?, what needs it)
CHECKS = [
    ('python',       check_python,       True,  'every step'),
    ('source',       check_source,       True,  'stage, libraries'),
    ('packs',        check_packs,        True,  'unpack, corpus'),
    ('emsdk',        check_emsdk,        True,  'every compile step'),
    ('lzo',          check_lzo,          False, 'lzo (downloads it when missing)'),
    ('python-wasm',  check_python_wasm,  False, 'engine, link - made by the `python` step'),
    ('python-build', check_python_build, False, 'only build_python.py --from-source'),
    ('stdlib',       check_stdlib,       True,  'data'),
    ('python2to3',   check_2to3,         True,  'scripts, uiscript'),
    ('pillow',       check_pillow,       True,  'data (cursors)'),
    ('font',         check_font,         False, 'data (UI font)'),
    ('server',       check_server,       False, 'data (server address)'),
    ('windows-gdi',  check_windows_gdi,  False, 'data (baked fonts)'),
    ('msvc',         check_msvc,         False, 'trees, gr2'),
    ('speedtree',    check_speedtree,    False, 'trees'),
    ('granny',       check_granny,       False, 'gr2'),
    ('bridge',       check_bridge,       False, 'serve, bridge (logging in to the game)'),
    ('measure',      check_measure,      False, 'check (the probe and behaviour gates)'),
]


def doctor():
    """Prints every check; 1 when a required one fails."""
    print('settings: %s\n' % (workspace.CONFIG_FILE if workspace.CONFIG else
                              'no webclient.toml - defaults (copy webclient.example.toml to change them)'))
    failed = 0
    checked = []
    for name, check, required, used_by in CHECKS:
        try:
            ok, detail, fix = check()
        except Exception as e:                      # a check must never stop the others
            ok, detail, fix = False, '%s: %s' % (type(e).__name__, e), 'see the message'
        checked.append((name, ok))
        mark = 'ok  ' if ok else ('FAIL' if required else 'warn')
        print('  %s %-12s %s' % (mark, name, detail))
        if not ok:
            print('       %s  (needed by: %s)' % (fix, used_by))
            failed += required
    # CPython for wasm is a warning only while the `python` step can make it
    names = {name: ok for name, ok in checked}
    if not names.get('python-wasm') and not names.get('python-build'):
        print('  FAIL CPython for wasm is missing AND cannot be built here (see python-build above)')
        failed += 1
    print('\n%s' % ('ready to build: python webclient.py build' if not failed else
                    '%d required item(s) missing - fix them, then run doctor again' % failed))
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# build: the steps in order
# ---------------------------------------------------------------------------

# (name, tool and arguments, what it does, doctor checks it needs - an optional
#  check that fails makes the step skipped with a warning instead of failing)
STEPS = [
    ('stage',    ['stage_port.py'],                     'client sources + our patches -> build/port/stage', ['source']),
    ('lzo',      ['build_lzo.py'],                      'LZO 2.10 (downloaded once) -> lib/liblzo.a', ['emsdk']),
    ('cryptopp', ['build_cryptopp.py'],                 'Crypto++ from extern/include -> lib/libcryptopp.a', ['emsdk', 'source']),
    ('eterbase', ['build_eterbase.py'],                 'EterBase -> lib/libeterbase.a', ['emsdk']),
    ('eterpack', ['build_eterpack.py'],                 'EterPack -> lib/libeterpack.a', ['emsdk']),
    ('python',   ['build_python.py'],                   'CPython 3.13 for wasm, prebuilt (downloaded once) -> lib/libpython3.13.a (skipped when present)', []),
    ('engine',   ['build_gamelib.py'],                  'the engine and our compat layer -> obj/gamelib', ['emsdk', 'python-wasm']),
    ('unpack',   ['build_corpus.py', '--unpack-only'],  'your packs -> build/port/client_files', ['emsdk', 'packs']),
    ('scripts',  ['unpack_packs.py'],                   'the script packs -> build/port/scripts', ['emsdk', 'packs']),
    ('rewrite',  ['rewrite_scripts.py'],                'game scripts Python 2 -> 3 -> build/port/scripts3', ['python2to3']),
    ('uiscript', ['uiscript_from_client.py'],           'window descriptions -> build/port/uiscript3', ['python2to3']),
    ('trees',    ['bake_speedtree.py'],                 'SpeedTree trees baked to meshes -> build/port/baked', ['msvc', 'speedtree']),
    ('gr2',      ['repair_gr2.py'],                     '.gr2 files our decoder rejects, re-saved by granny2.dll', ['msvc', 'granny']),
    ('corpus',   ['build_corpus.py'],                   'the streamed corpus (4 MB chunks + manifest) -> build/port/corpus', ['emsdk', 'packs']),
    ('data',     ['build_client_data.py'],              'the start-up package (scripts, UI, fonts, cursors) -> build/port/data', ['stdlib', 'pillow']),
    ('link',     ['link_client.py'],                    'client.wasm, client.js, client.data, client.html', ['emsdk', 'python-wasm']),
]


def print_steps():
    """Lists the build steps."""
    for i, (name, args, what, needs) in enumerate(STEPS, 1):
        print('%2d. %-9s %s' % (i, name, what))
        print('              tools/%s   needs: %s' % (' '.join(args), ', '.join(needs)))
    return 0


def select_steps(argv):
    """The steps chosen by --from / --only / --skip, in order; exits on an unknown name."""
    names = [s[0] for s in STEPS]

    def value(flag):
        """The value after `flag` (comma lists allowed), or None."""
        if flag not in argv:
            return None
        i = argv.index(flag)
        if i + 1 >= len(argv):
            sys.exit('%s needs a step name - see `python webclient.py steps`' % flag)
        items = argv[i + 1].split(',')
        for n in items:
            if n not in names:
                sys.exit('unknown step %r - the steps are: %s' % (n, ', '.join(names)))
        return items

    only, start, skip = value('--only'), value('--from'), value('--skip') or []
    chosen = list(STEPS)
    if only:
        chosen = [s for s in STEPS if s[0] in only]
    elif start:
        chosen = STEPS[names.index(start[0]):]
    return [s for s in chosen if s[0] not in skip]


def run_step(name, args, log):
    """Runs one tool, echoing and logging its output; returns (ok, output)."""
    env = dict(os.environ)
    empp = emsdk_tool('em++')
    if empp:                                           # the tools find em++ / node on PATH
        extra = [os.path.dirname(empp)]
        node = emsdk_tool('node')
        if node:
            extra.append(os.path.dirname(node))
        env['PATH'] = os.pathsep.join(extra + [env.get('PATH', '')])
    env['PYTHONUNBUFFERED'] = '1'
    p = subprocess.Popen([sys.executable, os.path.join(TOOLS, args[0])] + args[1:], cwd=ROOT, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    lines = []
    for line in p.stdout:
        lines.append(line)
        log.write(line)
        sys.stdout.write('    ' + line)
    p.wait()
    output = ''.join(lines)
    return p.returncode == 0 and not FAILURE.search(output), output


def explain(output):
    """One sentence on what to do about a failed step, from its output."""
    for pattern, hint in HINTS:
        if pattern.search(output):
            return hint
    return 'read the last lines above (also in %s)' % os.path.relpath(LOG, ROOT)


def build(argv):
    """Runs the chosen steps in order and stops at the first failure; 1 on failure."""
    results = {name: check()[0] for name, check, _r, _u in CHECKS}
    required = {name for name, _c, r, _u in CHECKS if r}
    required.add('python-wasm')       # a warning in doctor (the `python` step makes it), never skippable
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    started = time.time()
    with open(LOG, 'w', encoding='utf-8') as log:
        for name, args, what, needs in select_steps(argv):
            if name == 'python' and results.get('python-wasm') and '--only' not in argv:
                print('\n[python] CPython for wasm is present - skipped (rebuild: --only python)')
                continue
            if name == 'engine' and not results.get('python-wasm'):
                results['python-wasm'] = check_python_wasm()[0]      # the `python` step may have made it
            missing = [n for n in needs if not results.get(n)]
            if [n for n in missing if n in required]:
                print('\n[%s] cannot run - missing: %s. Run `python webclient.py doctor`.' % (name, ', '.join(missing)))
                return 1
            if missing:
                print('\n[%s] SKIPPED - optional input missing: %s (%s)' % (name, ', '.join(missing), what))
                continue
            print('\n[%s] %s' % (name, what))
            log.write('\n===== %s: tools/%s\n' % (name, ' '.join(args)))
            t = time.time()
            ok, output = run_step(name, args, log)
            if not ok:
                print('\n[%s] FAILED after %.0f s: %s' % (name, time.time() - t, explain(output)))
                print('Continue after fixing it: python webclient.py build --from %s' % name)
                return 1
            print('[%s] done in %.0f s' % (name, time.time() - t))
    print('\nbuild finished in %.0f s - next: python webclient.py serve' % (time.time() - started))
    return 0


# ---------------------------------------------------------------------------
# check, serve, package
# ---------------------------------------------------------------------------

GATES = [
    ('link',      'check_link.py'),
    ('glsl',      'check_glsl.py'),
    ('probe',     'check_probe.py'),
    ('behaviour', 'check_behaviour.py'),
    ('tests',     'run_tests.py'),
]


def check(argv):
    """Runs the quality gates in --compare mode (or --record); 1 when one fails."""
    mode = '--record' if '--record' in argv else '--compare'
    failed = []
    harness_ok, harness_detail, harness_fix = check_measure()
    for name, script in GATES:
        if name in ('probe', 'behaviour') and not harness_ok:
            # without the harness these gates measure nothing and report a
            # broken client (no WebGL, 0 frames), clean-clone trial
            print('  FAIL %-10s cannot measure: %s - %s' % (name, harness_detail, harness_fix))
            failed.append(name)
            continue
        r = subprocess.run([sys.executable, os.path.join(TOOLS, 'gates', script), mode], cwd=ROOT,
                           capture_output=True, text=True, errors='replace')
        last = (r.stdout + r.stderr).strip().splitlines()[-1:] or ['']
        print('  %s %-10s %s' % ('ok  ' if r.returncode == 0 else 'FAIL', name, last[0][:110]))
        if r.returncode != 0:                      # say WHAT differed, not only that it did
            for line in (r.stdout + r.stderr).splitlines():
                if line.startswith(('FAIL', 'DIFFERS')):
                    print('         ' + line[:150])
        if r.returncode != 0:
            failed.append(name)
    if mode == '--compare' and failed:
        print('\nA gate differs from its baseline: either the change broke something, or it was meant\n'
              'to change that (then record again: python webclient.py check --record).')
    return 1 if failed else 0


def start_bridge(page_port, foreground=False):
    """Writes the bridge configuration (tools/bridge_config.py) and starts
    bridge/src/index.js with it; the process (or its exit code when
    `foreground`), None when it cannot start."""
    ok, detail, fix = check_bridge()
    if not ok:
        print('bridge: %s - %s' % (detail, fix))
        return None
    import bridge_config
    path, config = bridge_config.write_config(page_port)
    if not config['targets']:
        print('bridge: no targets - build the client data first (serverInfo.py) or set [bridge] targets')
        return None
    print('bridge: ws://%s:%d, allowed: %d hosts of the server list, ports %s'
          % (config['listenHost'], config['listenPort'],
             len({t['host'] for t in config['targets'].values()}),
             bridge_config.port_ranges(t['port'] for t in config['targets'].values())))
    args = [emsdk_tool('node'), os.path.join(ROOT, 'bridge', 'src', 'index.js'), path]
    if foreground:
        return subprocess.call(args, cwd=os.path.join(ROOT, 'bridge'))
    process = subprocess.Popen(args, cwd=os.path.join(ROOT, 'bridge'))
    time.sleep(1.0)
    if process.poll() is not None:
        print('bridge: exited at start (code %s) - is port %d taken (another bridge running)?'
              % (process.returncode, config['listenPort']))
        return None
    return process


def bridge(argv):
    """Runs only the bridge, in the foreground (Ctrl+C stops it)."""
    port = int(argv[argv.index('--port') + 1]) if '--port' in argv else 8731
    code = start_bridge(port, foreground=True)
    return 1 if code is None else code


def serve(argv):
    """Serves the built client (tools/site_server.py: only its files) on 127.0.0.1 with the bridge next to it (unless
    --no-bridge) and prints the client address; Ctrl+C stops both."""
    port = int(argv[argv.index('--port') + 1]) if '--port' in argv else 8731
    if not os.path.isfile(os.path.join(workspace.PORT, 'client.html')):
        print('no build/port/client.html - run: python webclient.py build')
        return 1
    bridge_process = None if '--no-bridge' in argv else start_bridge(port)
    if bridge_process is None and '--no-bridge' not in argv:
        print('without a bridge the client waits at "connecting to the server" (see docs/DEPLOYMENT.md)')
    print('client: http://127.0.0.1:%d/client.html   (Ctrl+C stops the server%s)'
          % (port, ' and the bridge' if bridge_process else ''))
    try:
        # only the client's files - never build/port/private
        return subprocess.call([sys.executable, os.path.join(TOOLS, 'site_server.py'), str(port)],
                               cwd=workspace.PORT)
    except KeyboardInterrupt:
        return 0
    finally:
        if bridge_process:
            bridge_process.terminate()
            try:
                bridge_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                bridge_process.kill()


def package(argv):
    """dist/site/ for upload (tools/package.py --site)."""
    return subprocess.call([sys.executable, os.path.join(TOOLS, 'package.py'), '--site'], cwd=ROOT)


COMMANDS = {
    'doctor': lambda argv: doctor(),
    'steps': lambda argv: print_steps(),
    'build': build,
    'check': check,
    'serve': serve,
    'bridge': bridge,
    'package': package,
}


def main(argv):
    """Dispatches the command line; prints the usage for none or an unknown command."""
    if not argv or argv[0] not in COMMANDS:
        print(__doc__)
        return 0 if not argv else 1
    return COMMANDS[argv[0]](argv[1:])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
