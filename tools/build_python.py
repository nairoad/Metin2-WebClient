#!/usr/bin/env python3
"""build_python.py - CPython 3.13 for wasm32-emscripten, the game's script engine.

The client embeds CPython (the game's UI and logic are Python scripts). This
tool builds, from the official sources, the three archives the link needs and
the headers of the SAME build (a desktop Python's headers do not fit: pointer
size 8 instead of 4):

    build/port/lib/libpython3.13.a, libmpdec.a, libHacl_Hash_SHA2.a
    build/port/python313/Include/  (the source Include/ plus the build's pyconfig.h)

THE RECIPE (reproduced - the notes did not keep it):
the standard cross build of Tools/wasm/README.md (`--host=wasm32-unknown-
emscripten --with-emscripten-target=browser`, CONFIG_SITE of Tools/wasm) plus
`--disable-ipv6`, and five modules the game does not use left out through
Modules/Setup.local. Measured against the library the client was built with
until then: pyconfig.h byte-identical, the same 221 objects, the CODE section
of every object identical (only debug paths and the build date differ).

TWO WAYS:

  * DEFAULT - the prebuilt archive: the same three archives and headers,
    built by this recipe with Emscripten 6.0.8 and published as a release of
    the project (PREBUILT_URL). Downloaded once, checked against PREBUILT_SHA256
    (pinned here, so a changed file is refused) and installed. Needs nothing
    but this script.
  * `--from-source` - the recipe above. Needs Python 3.13 running this script
    (a cross build runs a native Python of the same version), emsdk, a POSIX
    `sh` (on Windows: Git for Windows) and GNU `make` (on Windows e.g.
    `winget install ezwinports.make`). Downloads the source archive from
    python.org once and checks its SHA-256.

The archives are object code for Emscripten: the prebuilt ones match the
Emscripten version they were built with (BUILD-INFO.txt inside). With a
different Emscripten the tool warns; if the link then fails, install that
version (`emsdk install 6.0.8`) or build from source.

    python tools/build_python.py                    # prebuilt: download (once), check, install
    python tools/build_python.py --archive FILE     # prebuilt from a file you downloaded yourself
    M2W_CPYTHON_URL=<url> python tools/build_python.py   # prebuilt from another address (same SHA-256 check)
    python tools/build_python.py --from-source      # download the sources (once), build, install
    python tools/build_python.py --from-source --no-install   # build only
    python tools/build_python.py --package          # pack the INSTALLED build as the release archive
"""

import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import workspace                                            # noqa: E402

VERSION = '3.13.15'
URL = 'https://www.python.org/ftp/python/%s/Python-%s.tar.xz' % (VERSION, VERSION)
SHA256 = '1e66a7945a48390ee4c2a4268a0e4185884059a13c4aab6d148aa208deea4a76'

SRC_ROOT = os.path.join(workspace.ROOT, 'build', 'python-src')
ARCHIVE = os.path.join(SRC_ROOT, 'Python-%s.tar.xz' % VERSION)
SOURCE = os.path.join(SRC_ROOT, 'Python-%s' % VERSION)
BUILD = os.path.join(SOURCE, 'builddir', 'emscripten-browser')

CONFIGURE_ARGS = ['--host=wasm32-unknown-emscripten', '--build=x86_64-pc-linux-gnu',
                  '--with-emscripten-target=browser', '--disable-ipv6']

# The prebuilt archive: this recipe's result, packed by `--package`
# and published as a release of the project. The SHA-256 is pinned here - the
# download is refused unless it matches byte for byte.
PREBUILT_EMSCRIPTEN = '6.0.8'
PREBUILT_NAME = 'cpython-%s-wasm32-emscripten-%s.tar.gz' % (VERSION, PREBUILT_EMSCRIPTEN)
PREBUILT_URL = ('https://github.com/nairoad/Metin2-WebClient/releases/download/cpython-%s-emscripten-%s/%s'
                % (VERSION, PREBUILT_EMSCRIPTEN, PREBUILT_NAME))
PREBUILT_SHA256 = 'c6547939f96fdcc9dced35be088ea5bd1e57f578bab6e10c29e609282bbba150'
PREBUILT_ARCHIVE = os.path.join(SRC_ROOT, PREBUILT_NAME)

# Modules the game never imports - left out, as (smaller client,
# no zlib/bz2/sqlite/expat code to link).
DISABLED_MODULES = ['_bz2', '_sqlite3', '_elementtree', 'pyexpat', 'zlib']

# What the link takes, relative to the build directory -> file name in build/port/lib.
ARCHIVES = {
    'libpython3.13.a': 'libpython3.13.a',
    os.path.join('Modules', '_decimal', 'libmpdec', 'libmpdec.a'): 'libmpdec.a',
    os.path.join('Modules', '_hacl', 'libHacl_Hash_SHA2.a'): 'libHacl_Hash_SHA2.a',
}


def posix(path):
    """A Windows path as the POSIX shell of Git for Windows sees it (C:\\x -> /c/x)."""
    if os.name != 'nt' or len(path) < 2 or path[1] != ':':
        return path
    return '/' + path[0].lower() + path[2:].replace('\\', '/')


def find_sh():
    """A POSIX sh: on PATH, else the one of Git for Windows; None when missing."""
    found = shutil.which('sh')
    if found:
        return found
    for base in (os.environ.get('ProgramFiles', r'C:\Program Files'), os.environ.get('ProgramW6432', '')):
        candidate = os.path.join(base, 'Git', 'usr', 'bin', 'sh.exe')
        if base and os.path.isfile(candidate):
            return candidate
    return None


def requirements():
    """(ok, problems): the interpreter version, sh, make and emsdk."""
    problems = []
    if sys.version_info[:2] != (3, 13):
        problems.append('run this with Python 3.13 (a cross build needs a native Python of the same '
                        'version; this is %d.%d)' % sys.version_info[:2])
    if not find_sh():
        problems.append('no POSIX sh - install Git for Windows (it brings sh.exe)')
    if not shutil.which('make'):
        problems.append('no GNU make - on Windows: winget install ezwinports.make')
    if not os.path.isdir(os.path.join(workspace.EMSDK, 'upstream', 'emscripten')):
        problems.append('no emsdk at %s - set [tools] emsdk' % workspace.EMSDK)
    return not problems, problems


def fetch():
    """Downloads the source archive once and checks its SHA-256; False on a mismatch."""
    os.makedirs(SRC_ROOT, exist_ok=True)
    if not os.path.isfile(ARCHIVE):
        print('downloading %s' % URL)
        urllib.request.urlretrieve(URL, ARCHIVE)
    with open(ARCHIVE, 'rb') as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != SHA256:
        print('%s: sha256 %s, expected %s - delete it and run again' % (ARCHIVE, digest, SHA256))
        return False
    if not os.path.isfile(os.path.join(SOURCE, 'configure')):
        with tarfile.open(ARCHIVE, 'r:xz') as t:
            if hasattr(tarfile, 'data_filter'):
                t.extractall(SRC_ROOT, filter='data')
            else:
                t.extractall(SRC_ROOT)
        print('unpacked %s (sha256 ok)' % os.path.relpath(SOURCE, workspace.ROOT))
    return True


def build_env():
    """The environment for configure and make: emsdk's compilers first on PATH,
    and the directory of sh (make runs its recipes with it)."""
    env = dict(os.environ)
    extra = [os.path.join(workspace.EMSDK, 'upstream', 'emscripten')]
    node_root = os.path.join(workspace.EMSDK, 'node')
    if os.path.isdir(node_root):
        for d in sorted(os.listdir(node_root), reverse=True):
            extra.append(os.path.join(node_root, d, 'bin'))
            break
    extra.append(os.path.dirname(find_sh()))
    env['PATH'] = os.pathsep.join(extra + [env.get('PATH', '')])
    env.update(CONFIG_SITE='../../Tools/wasm/config.site-wasm32-emscripten',
               CC='emcc', CXX='em++', AR='emar', RANLIB='emranlib', READELF='true')
    return env


def run(args, env, log_name):
    """Runs one build command in the build directory with its output in a log;
    True on success (the tail of the log is printed on failure)."""
    log = os.path.join(SRC_ROOT, log_name)
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        r = subprocess.run(args, cwd=BUILD, env=env, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        with open(log, encoding='utf-8', errors='replace') as f:
            print(''.join(f.readlines()[-25:]))
        print('FAILED: %s (full log: %s)' % (' '.join(args[:2]), os.path.relpath(log, workspace.ROOT)))
    return r.returncode == 0


def build():
    """Configures (once) and makes the three archives; False on a failure."""
    os.makedirs(os.path.join(BUILD, 'Modules'), exist_ok=True)
    env = build_env()
    args = [find_sh(), '../../configure', '-C'] + CONFIGURE_ARGS + [
        '--with-build-python=' + posix(sys.executable)]
    # configure runs once - and again whenever its arguments changed (the
    # stamp holds the last ones; a changed flag must not build silently with
    # the old configuration).
    stamp = os.path.join(BUILD, 'configure-args.txt')
    wanted = '\n'.join(args[1:])
    previous = io.open(stamp, encoding='utf-8').read() if os.path.isfile(stamp) else None
    if not os.path.isfile(os.path.join(BUILD, 'Makefile')) or previous != wanted:
        for stale in ('config.cache', 'Makefile'):
            if os.path.isfile(os.path.join(BUILD, stale)):
                os.remove(os.path.join(BUILD, stale))
        print('configure (a few minutes)...')
        if not run(args, env, 'configure.log'):
            return False
        with io.open(stamp, 'w', encoding='utf-8') as f:
            f.write(wanted)
    with open(os.path.join(BUILD, 'Modules', 'Setup.local'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# tools/build_python.py: modules the game does not use\n*disabled*\n')
        f.write('\n'.join(DISABLED_MODULES) + '\n')
    print('make (several minutes)...')
    return run(['make', '-j%d' % (os.cpu_count() or 4)] + [a.replace(os.sep, '/') for a in ARCHIVES],
               env, 'make.log')


def install():
    """Copies the archives to build/port/lib and the headers of this build to
    build/port/python313/Include."""
    os.makedirs(workspace.LIBDIR, exist_ok=True)
    for source, name in ARCHIVES.items():
        shutil.copy2(os.path.join(BUILD, source), os.path.join(workspace.LIBDIR, name))
        print('  %-24s %d bytes' % (name, os.path.getsize(os.path.join(workspace.LIBDIR, name))))
    include = os.path.join(workspace.PORT, 'python313', 'Include')
    if os.path.isdir(include):
        shutil.rmtree(include)
    shutil.copytree(os.path.join(SOURCE, 'Include'), include)
    shutil.copy2(os.path.join(BUILD, 'pyconfig.h'), os.path.join(include, 'pyconfig.h'))
    print('  headers -> %s' % os.path.relpath(include, workspace.ROOT))


def emscripten_version():
    """The version of the Emscripten in [tools] emsdk ('6.0.8'), '' when unknown."""
    path = os.path.join(workspace.EMSDK, 'upstream', 'emscripten', 'emscripten-version.txt')
    try:
        with io.open(path, encoding='utf-8') as f:
            return f.read().strip().strip('"')
    except OSError:
        return ''


def sha256_of(path):
    """The SHA-256 of a file, as hex."""
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def installed_files():
    """(path on disk, path in the archive) of everything `install()` put in
    place, plus the CPython license texts; the archive paths are sorted."""
    include = os.path.join(workspace.PORT, 'python313', 'Include')
    files = [(os.path.join(workspace.LIBDIR, name), 'lib/' + name) for name in sorted(ARCHIVES.values())]
    for root, _dirs, names in os.walk(include):
        for name in names:
            path = os.path.join(root, name)
            files.append((path, 'include/' + os.path.relpath(path, include).replace(os.sep, '/')))
    files.append((os.path.join(SOURCE, 'LICENSE'), 'LICENSE'))
    files.append((os.path.join(SOURCE, 'Doc', 'license.rst'), 'license-incorporated-software.rst'))
    return sorted(files, key=lambda f: f[1])


def package():
    """Packs the INSTALLED build (build/port/lib + build/port/python313/Include)
    with the CPython license and a BUILD-INFO.txt into build/python-src/
    PREBUILT_NAME - byte for byte the same for the same files (sorted names,
    time 0, no owner) - and prints its SHA-256 to pin in PREBUILT_SHA256."""
    import gzip
    version = emscripten_version()
    if version != PREBUILT_EMSCRIPTEN:
        print('this build uses Emscripten %s, the archive name says %s - change PREBUILT_EMSCRIPTEN first'
              % (version or '?', PREBUILT_EMSCRIPTEN))
        return 1
    files = installed_files()
    missing = [p for p, _a in files if not os.path.isfile(p)]
    if missing:
        print('missing (build and install first, and keep the sources in build/python-src): '
              + ', '.join(os.path.relpath(p, workspace.ROOT) for p in missing[:5]))
        return 1
    info = ['CPython %s for wasm32-emscripten - the script engine of Metin2 WebClient' % VERSION,
            'built by tools/build_python.py --from-source with Emscripten %s' % version,
            'source: %s (sha256 %s)' % (URL, SHA256),
            'configure: ' + ' '.join(CONFIGURE_ARGS),
            'modules left out: ' + ' '.join(DISABLED_MODULES),
            'license: LICENSE (PSF) and license-incorporated-software.rst (libmpdec, HACL* and others)',
            '', 'sha256 of every file:']
    info += ['%s  %s' % (sha256_of(p), a) for p, a in files]
    blob = ('\n'.join(info) + '\n').encode('utf-8')
    os.makedirs(SRC_ROOT, exist_ok=True)
    with open(PREBUILT_ARCHIVE, 'wb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode='w', format=tarfile.PAX_FORMAT) as t:
                def add(name, data):
                    """Adds `data` as the member `name`: a plain file, time 0, no owner."""
                    member = tarfile.TarInfo(name)
                    member.size, member.mtime, member.mode = len(data), 0, 0o644
                    member.uid = member.gid = 0
                    member.uname = member.gname = ''
                    t.addfile(member, io.BytesIO(data))
                add('BUILD-INFO.txt', blob)
                for path, name in files:
                    with open(path, 'rb') as f:
                        add(name, f.read())
    print('%s: %d files, %d bytes' % (os.path.relpath(PREBUILT_ARCHIVE, workspace.ROOT), len(files) + 1,
                                      os.path.getsize(PREBUILT_ARCHIVE)))
    print('sha256 %s  <- PREBUILT_SHA256' % sha256_of(PREBUILT_ARCHIVE))
    return 0


def install_prebuilt(archive):
    """Checks `archive` against PREBUILT_SHA256 and installs its archives and
    headers the way `install()` does; False on a mismatch or a bad member."""
    digest = sha256_of(archive)
    if digest != PREBUILT_SHA256:
        print('%s: sha256 %s, expected %s - not installed (delete the file and run again, '
              'or build from source: --from-source)' % (archive, digest, PREBUILT_SHA256))
        return False
    include = os.path.join(workspace.PORT, 'python313', 'Include')
    with tarfile.open(archive, 'r:gz') as t:
        members = t.getmembers()
        for m in members:
            # only regular files under the three expected places - never an
            # absolute path, `..`, a link or a device
            if not m.isfile() or m.name.startswith('/') or '..' in m.name.split('/') or \
                    not (m.name.startswith(('lib/', 'include/')) or m.name in (
                        'BUILD-INFO.txt', 'LICENSE', 'license-incorporated-software.rst')):
                print('%s: unexpected member %r - not installed' % (archive, m.name))
                return False
        if sorted(m.name for m in members if m.name.startswith('lib/')) != \
                sorted('lib/' + n for n in ARCHIVES.values()):
            print('%s: not the three archives the link needs - not installed' % archive)
            return False
        # Unpacked ASIDE first and moved into place only when complete: an
        # interrupted run (Ctrl+C, a full disk) must not leave the headers
        # deleted and a library half written.
        staging = os.path.join(SRC_ROOT, 'prebuilt-staging')
        if os.path.isdir(staging):
            shutil.rmtree(staging)
        for m in members:
            if not m.name.startswith(('lib/', 'include/')):
                continue
            target = os.path.join(staging, *m.name.split('/'))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with t.extractfile(m) as src, open(target, 'wb') as dst:
                shutil.copyfileobj(src, dst)
    os.makedirs(workspace.LIBDIR, exist_ok=True)
    for name in ARCHIVES.values():
        os.replace(os.path.join(staging, 'lib', name), os.path.join(workspace.LIBDIR, name))
    if os.path.isdir(include):
        shutil.rmtree(include)
    os.makedirs(os.path.dirname(include), exist_ok=True)
    shutil.move(os.path.join(staging, 'include'), include)
    shutil.rmtree(staging)
    for name in sorted(ARCHIVES.values()):
        print('  %-24s %d bytes' % (name, os.path.getsize(os.path.join(workspace.LIBDIR, name))))
    print('  headers -> %s' % os.path.relpath(include, workspace.ROOT))
    return True


def prebuilt(argv):
    """The default way: the prebuilt archive (`--archive FILE`, or downloaded
    once from PREBUILT_URL - or from M2W_CPYTHON_URL when set, e.g. a mirror or
    a `file:///` test copy; the SHA-256 check is the same either way), checked
    and installed; 1 on a failure."""
    if not PREBUILT_SHA256:
        print('no prebuilt archive is published for this version yet - build from source: '
              'python tools/build_python.py --from-source')
        return 1
    version = emscripten_version()
    if version and version != PREBUILT_EMSCRIPTEN:
        print('WARNING: your Emscripten is %s, the prebuilt CPython was built with %s. If the link '
              'fails, install that version (emsdk install %s, emsdk activate %s) or build from source '
              '(--from-source).' % (version, PREBUILT_EMSCRIPTEN, PREBUILT_EMSCRIPTEN, PREBUILT_EMSCRIPTEN))
    if '--archive' in argv:
        i = argv.index('--archive')
        archive = argv[i + 1] if i + 1 < len(argv) else ''
        if not os.path.isfile(archive):
            print('--archive: no file %r' % archive)
            return 1
    else:
        archive = PREBUILT_ARCHIVE
        if not os.path.isfile(archive):
            os.makedirs(SRC_ROOT, exist_ok=True)
            url = os.environ.get('M2W_CPYTHON_URL') or PREBUILT_URL
            print('downloading %s' % url)
            try:
                urllib.request.urlretrieve(url, archive + '.part')
            except Exception as e:
                print('download failed: %s\n  download it yourself and run: python tools/build_python.py '
                      '--archive <file>\n  or build from source: python tools/build_python.py --from-source'
                      % e)
                return 1
            os.replace(archive + '.part', archive)
    if not install_prebuilt(archive):
        return 1
    print('CPython %s for wasm (prebuilt, sha256 ok): done' % VERSION)
    return 0


def main(argv):
    """`--package`, `--from-source` (requirements, fetch, build and, unless
    --no-install, install) or - by default - the prebuilt archive; 1 on a failure."""
    if '--package' in argv:
        return package()
    if '--from-source' not in argv:
        return prebuilt(argv)
    ok, problems = requirements()
    if not ok:
        for p in problems:
            print('missing: ' + p)
        return 1
    if not fetch() or not build():
        return 1
    if '--no-install' not in argv:
        install()
    print('CPython %s for wasm: done' % VERSION)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
