#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers for the quality gates (tools/gates).

Everything here is deliberately dependency-free: repository root discovery,
the list of directories that are NOT our layer (and must never be touched or
scanned), file iteration, and a minimal WebAssembly section parser used to
compare object files section by section.
"""
import hashlib
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, 'tools')
sys.path.insert(0, TOOLS)
import workspace                                     # input paths, emsdk
COMPAT = workspace.COMPAT
PORT = os.path.join(ROOT, 'build', 'port')
OBJDIR = os.path.join(PORT, 'obj', 'gamelib')
# libeterbase.a / libeterpack.a are built by tools/build_eterbase.py and
# tools/build_eterpack.py from these object trees; the phase B1 review found
# them stale and unmeasured, so the baseline covers them too.
OBJDIRS = (('gamelib', OBJDIR), ('eterbase', os.path.join(PORT, 'obj', 'eterbase')),
           ('eterpack', os.path.join(PORT, 'obj', 'eterpack')))
STAGE = os.path.join(PORT, 'stage')
EXTERN = workspace.EXTERN_INCLUDE
EMSDK = workspace.EMSDK

# Not our layer, or generated: never scanned, never renamed.
EXCLUDED_DIRS = (
    '.git', 'dist/site/', '__pycache__', 'node_modules',
    os.path.join('build', 'port', 'stage'), os.path.join('build', 'port', 'obj'),
    os.path.join('build', 'port', 'lib'),
    os.path.join('build', 'port', 'private'), os.path.join('build', 'port', 'data'),
    os.path.join('build', 'port', 'client_files'), os.path.join('build', 'port', 'corpus'),
    os.path.join('build', 'port', 'bake'), os.path.join('build', 'port', 'repaired_gr2'),
    os.path.join('build', 'port', 'repair'),
    os.path.join('tools', 'gates'),    # the name and language gates talk about old names by design
)

# Documents whose old names are part of the record; generated from the code
# by reference.py - regenerated, never edited.
HISTORICAL_DOCS = ('docs/REFERENCE.md',)

# Tracked files that are not published (repository-relative prefixes, `/`).
UNPUBLISHED = ()

# The working repository adds its own private directories, historical
# documents and unpublished files (tools/gates/private_scope.py, not published).
try:
    import private_scope
    EXCLUDED_DIRS += private_scope.EXCLUDED_DIRS
    HISTORICAL_DOCS += private_scope.HISTORICAL_DOCS
    UNPUBLISHED += private_scope.UNPUBLISHED
except ImportError:
    pass

TEXT_EXT = ('.cpp', '.c', '.h', '.py', '.js', '.html', '.md', '.txt', '.bat', '.tsv', '.json', '.cfg')


def rel(path):
    """`path` relative to the repository root, with `/` separators."""
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


def is_excluded(relpath):
    """True when a repo-relative path lies in one of EXCLUDED_DIRS (private,
    generated or third-party directories).
    """
    p = relpath.replace('/', os.sep)
    for d in EXCLUDED_DIRS:
        if p == d or p.startswith(d + os.sep):
            return True
    return False


def git_tracked():
    """Relative paths of files tracked by git (the published layer is exactly
    this set; everything else under build/ is generated or foreign)."""
    import subprocess
    r = subprocess.run(['git', 'ls-files', '-z'], cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        return None
    return [p.decode('utf-8', 'replace') for p in r.stdout.split(b'\0') if p]


def iter_files(roots=None, exts=TEXT_EXT, include_historical=False):
    """Yield absolute paths of our layer's text files under `roots` (default:
    whole repo). Scope = files tracked by git (reviewer, phase 0 review: a
    plain os.walk visited ~8000 untracked files under build/ and vendor
    trees), minus EXCLUDED_DIRS and, unless asked, the historical documents.
    Falls back to os.walk when git is unavailable."""
    roots = roots or [ROOT]
    root_rels = [os.path.relpath(r, ROOT).replace(os.sep, '/') for r in roots]
    tracked = git_tracked()
    if tracked is None:
        for root in roots:
            for dirpath, dirnames, filenames in os.walk(root):
                r = os.path.relpath(dirpath, ROOT)
                r = '' if r == '.' else r
                dirnames[:] = sorted(d for d in dirnames if not is_excluded(os.path.join(r, d) if r else d))
                for f in sorted(filenames):
                    if f.endswith(exts):
                        p = os.path.join(dirpath, f)
                        if include_historical or rel(p) not in HISTORICAL_DOCS:
                            yield p
        return
    for rp in sorted(tracked):
        if not rp.endswith(exts) or is_excluded(rp):
            continue
        if not include_historical and rp in HISTORICAL_DOCS:
            continue
        if not any(rr == '.' or rp == rr or rp.startswith(rr + '/') for rr in root_rels):
            continue
        p = os.path.join(ROOT, rp.replace('/', os.sep))
        if os.path.isfile(p):
            yield p


def read_text(path):
    """The whole file as text (UTF-8, errors replaced)."""
    with io.open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def tool(name):
    """Locate an emsdk tool the same way tools/build_gamelib.py does."""
    import shutil
    p = shutil.which(name)
    if p:
        return p
    for d in (os.path.join(EMSDK, 'upstream', 'bin'), os.path.join(EMSDK, 'upstream', 'emscripten')):
        for ext in ('', '.exe', '.bat'):
            k = os.path.join(d, name + ext)
            if os.path.exists(k):
                return k
    raise SystemExit('tool not found: ' + name + ' (add emsdk to PATH)')


# --- minimal WebAssembly section parser -----------------------------------

def _leb(data, i):
    """Decodes an unsigned LEB128 number at `data[i]`; returns (value, next index)."""
    result, shift = 0, 0
    while True:
        b = data[i]
        i += 1
        result |= (b & 0x7f) << shift
        if not (b & 0x80):
            return result, i
        shift += 7


SECTION_NAMES = {1: 'TYPE', 2: 'IMPORT', 3: 'FUNCTION', 4: 'TABLE', 5: 'MEMORY', 6: 'GLOBAL',
                 7: 'EXPORT', 8: 'START', 9: 'ELEMENT', 10: 'CODE', 11: 'DATA', 12: 'DATACOUNT',
                 13: 'TAG'}


def wasm_sections(data):
    """Return [(name, payload_bytes)] for a .wasm / .o file. Custom sections are
    named by their embedded name (`linking`, `reloc.CODE`, `producers`, `name`,
    `.rodata..L.str`, ...), standard ones by SECTION_NAMES."""
    assert data[:4] == b'\0asm', 'not a wasm file'
    i = 8
    out = []
    while i < len(data):
        sid = data[i]
        i += 1
        size, i = _leb(data, i)
        payload = data[i:i + size]
        i += size
        if sid == 0:
            nlen, j = _leb(payload, 0)
            name = payload[j:j + nlen].decode('utf-8', 'replace')
            payload = payload[j + nlen:]
        else:
            name = SECTION_NAMES.get(sid, 'SECTION%d' % sid)
        out.append((name, payload))
    return out


# `__TIMESTAMP__` expands to the source file's modification time ("Sat Sep 19
# 21:06:15 2026"); stage_port.py rewrites the staged TMP4 sources on every
# run, so UserInterface.o and PythonNetworkStreamPhaseGame.o would differ in
# DATA on every build (measured: the 24-character string is the only thing
# that changes - 4 differing bytes between two builds minutes apart). The
# same-length constant keeps offsets intact. Expected hits: 2 in 340 objects -
# re-measure after each phase; more would mean the pattern masks a literal.
import re
_TIMESTAMP = re.compile(rb'[A-Z][a-z]{2} [A-Z][a-z]{2} [ \d]\d \d\d:\d\d:\d\d \d{4}')


def normalise_payload(name, payload):
    """The DATA section with every `__TIMESTAMP__` string replaced by a
    same-length constant (other sections unchanged).
    """
    if name == 'DATA':
        return _TIMESTAMP.sub(b'Xxx Xxx 00 00:00:00 0000', payload)
    return payload


def section_hashes(path):
    """{section name: sha256 of payload} for one object/wasm file. Duplicate
    custom section names (per-function data segments) get a numeric suffix.
    DATA is normalised (see normalise_payload) before hashing."""
    with open(path, 'rb') as f:
        data = f.read()
    hashes = {}
    seen = {}
    for name, payload in wasm_sections(data):
        payload = normalise_payload(name, payload)
        n = seen.get(name, 0)
        seen[name] = n + 1
        key = name if n == 0 else '%s#%d' % (name, n)
        hashes[key] = hashlib.sha256(payload).hexdigest()
    return hashes
