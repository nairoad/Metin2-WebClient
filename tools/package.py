#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""package.py - the ready directory to upload.

  dist/site/   the full client to upload under https://your.site/metin2/
               (client.html, engine, data, the 2 GB corpus - chunks as hard
               links, so as not to duplicate disk space)

The directory lies in the repository root and is outside git (.gitignore).
It gets a README.txt with instructions.

Usage: python tools/package.py [--site]
"""
import io
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(ROOT, 'build', 'port')
FOR_SITE = os.path.join(ROOT, 'dist', 'site')

SITE_FILES = ['client.html', 'client.js', 'client.wasm', 'client.data', 'sw.js', 'preload.js']

README_SITE = """WHAT IS HERE
============
The full Metin2 client for the browser. Upload THIS WHOLE directory (without
README.txt) to an address, e.g. https://example.com/metin2/ - so that
client.html is at https://example.com/metin2/client.html.

  client.html             the page (ours, minimal)
  client.js, client.wasm  the engine (their URLs carry ?v=..., so after an
                          update nobody gets the old version from a cache)
  client.data             the start-up package (UI scripts, fonts, cursors)
  sw.js, preload.js       Service Worker + the download bar in the corner
  corpus/                 game data (~2 GB, 496 chunks of 4 MB)

SERVER REQUIREMENTS
===================
1. HTTPS (the Service Worker and wss:// need it).
2. The bridge (bridge/ in the repository) running on a machine that sees
   the game server, exposed by nginx under /to/ (step by step:
   docs/DEPLOYMENT.md in the repository). The client builds the address wss://your.site/to/...
   itself. A bridge on another address: [server] bridge in webclient.toml.
3. Headers (recommended):
     corpus/*.bin          Cache-Control: public, max-age=31536000, immutable
     corpus/manifest.bin   Cache-Control: no-cache
     client.html           Cache-Control: no-cache
     *.wasm                Content-Type: application/wasm

THE "PLAY IN THE BROWSER" LINK ON YOUR SITE
===========================================
  <a href="https://example.com/metin2/client.html?deflang=pl&scale=1.5&return=https://example.com" target="_blank">Play</a>

  scale=1|1.25|1.5|1.75|2|2.5|3   interface size (remembered in the browser)
  return=<address>                where "Exit game" goes back to
  deflang=pl                      language

AFTER UPLOADING - CHECK
=======================
  curl -I https://example.com/metin2/client.wasm          -> 200, application/wasm
  curl -I https://example.com/metin2/corpus/manifest.bin  -> 200
  in the game F12 -> console: "[sw] registered", then "[corpus] background: ..."
"""


def link_or_copy(source, target):
    """Puts `source` at `target` as a hard link (no extra space on NTFS), or a copy
    when linking fails.
    """
    if os.path.exists(target):
        os.remove(target)
    try:
        os.link(source, target)          # NTFS: a hard link, zero space
    except OSError:
        shutil.copy2(source, target)


def build(without_private):
    """Runs build_client_data.py (optionally `--no-private`) and link_client.py;
    False when either fails or prints an error line.
    """
    import subprocess
    tools = os.path.dirname(os.path.abspath(__file__))
    extra_args = ['--no-private'] if without_private else []
    for script, args in (('build_client_data.py', extra_args), ('link_client.py', [])):
        r = subprocess.run([sys.executable, os.path.join(tools, script)] + args, capture_output=True, text=True)
        bad = [l for l in (r.stdout + r.stderr).splitlines()
               if 'Traceback' in l or 'Error' in l or 'ERROR' in l]
        if r.returncode != 0 or bad:
            print('ERROR in %s:\n%s' % (script, '\n'.join(bad[:10]) or r.stderr[-800:]))
            return False
    return True


def page():
    """Builds dist/site/: the data package WITHOUT private files (refuses when
    loginInfo.xml is still in it), the page files and the corpus; then
    restores the local package with the private files. 1 on an error.
    """
    if os.path.isdir(FOR_SITE):
        shutil.rmtree(FOR_SITE)
    os.makedirs(FOR_SITE)
    # PUBLIC PACKAGE: without private/ (loginInfo.xml = autologin with a password).
    print('building client.data WITHOUT the private files...')
    if not build(True):
        return 1
    with io.open(os.path.join(PORT, 'client.js'), 'rb') as f:
        if b'loginInfo.xml' in f.read():
            print('ERROR: client.data still contains loginInfo.xml - not packing')
            return 1
    for p in SITE_FILES:
        z = os.path.join(PORT, p)
        if not os.path.isfile(z):
            print('MISSING: build/port/' + p + ' - run python tools/link_client.py first')
            return 1
        shutil.copy2(z, os.path.join(FOR_SITE, p))
    corpus = os.path.join(PORT, 'corpus')
    if not os.path.isdir(corpus):
        print('MISSING: build/port/corpus - run python tools/build_corpus.py first')
        return 1
    target = os.path.join(FOR_SITE, 'corpus')
    os.makedirs(target)
    # Only the chunks of the CURRENT manifest - the work directory also holds
    # the chunks of earlier rebuilds (measured: 1260 files, 5.3 GB, when the
    # manifest has 496).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from check_corpus import read_corpus_manifest
    chunks, _files = read_corpus_manifest(os.path.join(corpus, 'manifest.bin'))
    names = ['manifest.bin', 'order.txt'] + [digest + '.bin' for digest, _size in chunks]
    how_many, byte_count = 0, 0
    for name in names:
        z = os.path.join(corpus, name)
        if not os.path.isfile(z):
            print('MISSING in the corpus: ' + name)
            return 1
        link_or_copy(z, os.path.join(target, name))
        how_many += 1
        byte_count += os.path.getsize(z)
    with io.open(os.path.join(FOR_SITE, 'README.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(README_SITE)
    print('dist/site/: %d page files + corpus %d files (%.1f GB), without loginInfo.xml' % (len(SITE_FILES), how_many, byte_count / 1e9))
    # back to the working version (with autologin) for local tests
    print('restoring client.data with the private files (for tests)...')
    build(False)
    return 0


def main():
    """Builds dist/site/ (`--site`, the default); returns its exit code."""
    return page()


if __name__ == '__main__':
    sys.exit(main())
