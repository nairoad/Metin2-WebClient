#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_corpus.py - the streamed corpus from the USER'S DATA.

WHY THIS EXISTS
===============
Now the client fetches game data over the network, from a corpus
in the M2WF format. The corpus must come from the packs of THE CLIENT THE
PLAYER USES - a corpus from any other build diverges before your eyes:

    icon/item/70038.tga - CItemData::__SetIconImage

the icon of an item that exists on the player's server but not in the other
build. Models, maps, texts and everything else the server added or changed
diverge the same way.

This script builds the corpus from `[inputs] packs` (webclient.toml) - the
packs of THIS client.

THE FORMAT
==========
`manifest.bin` plus 4 MB chunks named by the hash of their content.
Described in `compat/webfs.h`, the reader in `tools/webfs.py`,
and our layer reads it in `compat/webfs_web.cpp`.

    header, 28 B:
        0   char[4]  'M2WF'
        4   uint32   version (1)
        8   uint32   chunk size
        12  uint32   chunks
        16  uint32   packs
        20  uint32   files
        24  uint32   boot chunks
    28                      chunk table: N x (16 B hash + uint32 size)
    28 + 20*N               pack table:  M x (uint16 length + name)
    then                    file table:  K x (uint16 pack, uint32 chunk,
                                              uint32 offset, uint32 size,
                                              uint16 length + name)

The chunk hash is **SHA-256 cut to 16 bytes** - established by measurement,
not guessed (MD5 sounded sensible and was wrong).

TWO THINGS THE FORMAT FORCES
============================
1. A file may cross a chunk boundary - the rest lies from offset zero in the
   chunk with the next number. So we pack as a STREAM, not
   file-per-chunk.
2. The names in the manifest are lower case, with a forward slash. The
   reader in the client makes the same conversion before it asks.

Running:
    python tools/build_corpus.py            # the whole client
    python tools/build_corpus.py --check    # only the check of a finished one
"""

import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
PORT = os.path.join(ROOT, 'build', 'port')
CLIENT = workspace.CLIENT
PACKS = workspace.PACKS
WORK_DIR = workspace.GAME_FILES
TARGET = os.path.join(PORT, 'corpus')

CHUNK_SIZE = 4 * 1024 * 1024

# Files from the client's root that the game reads directly, not from a pack.
LOOSE_FILES = ('locale.cfg', 'metin2.cfg', 'mouse.cfg', 'channel.inf')


def pack_names():
    """Pack names without extension - as many as there really are.

    `unpack_packs.py` had a list of TWO names here (`root`, `locale_en`),
    because it was made to extract the scripts. The corpus needs everything, so
    the list comes from the directory, not from code - otherwise the first
    `.epk` file the user added would be invisible and nobody would notice.
    """
    if not os.path.isdir(PACKS):
        return []
    names = set()
    for p in os.listdir(PACKS):
        suffix_part = p.lower()
        if suffix_part.endswith('.eix') or suffix_part.endswith('.epk'):
            names.add(os.path.splitext(p)[0])
    return sorted(names)


def unpack_all():
    """Extracts all packs into the work directory. Returns the number of files."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import unpack_packs

    empp = unpack_packs.tool_name('em++')
    node = shutil.which('node')
    if not node:
        raise SystemExit('node not found')

    js_tool = os.path.join(PORT, 'obj', 'unpack_pack.js')
    os.makedirs(os.path.dirname(js_tool), exist_ok=True)
    if not unpack_packs.build(empp, js_tool):
        raise SystemExit('could not build the pack reader')

    os.makedirs(WORK_DIR, exist_ok=True)

    packs = pack_names()
    print('packs to extract: %d' % len(packs))

    how_many = 0
    for i, name in enumerate(packs, 1):
        r = subprocess.run([node, js_tool, '.', name, WORK_DIR],
                           cwd=PACKS, capture_output=True, text=True)
        extracted = 0
        for l in r.stdout.splitlines():
            if l.startswith(('extracted ', 'wyjete ')):   # `wyjete` earlier
                try:
                    extracted = int(l.split()[1])
                except (IndexError, ValueError):
                    pass
        how_many += extracted
        # A pack that did not open does NOT stop the whole - but it has to
        # be heard. The client copes with one missing pack too.
        if r.returncode != 0:
            print('  [%3d/%3d] %-24s FAILED' % (i, len(packs), name))
        else:
            print('  [%3d/%3d] %-24s %6d files' % (i, len(packs), name,
                                                    extracted))
    return how_many


def collect_files():
    """A list of (name_in_corpus, path_on_disk), sorted.

    The order matters in practice: files next to each other in the list land
    in the same chunk, and the client usually reads whole directories at once
    (all the textures of one character, all the patches of one map).
    So sorting by path gives fewer downloads than a random order.
    """
    file_list = []
    for root_node, _dir, names in os.walk(WORK_DIR):
        for n in names:
            full = os.path.join(root_node, n)
            relative_path = os.path.relpath(full, WORK_DIR)
            file_list.append((relative_path.replace(os.sep, '/').lower(), full))

    for n in LOOSE_FILES:
        full = os.path.join(CLIENT, n)
        if os.path.exists(full):
            file_list.append((n.lower(), full))

    # MUSIC: `BGM/*.mp3` lies in the client LOOSE, outside the packs
    # (the packs have only 5 of the 25 tracks). Without this, syserr after every
    # entry to a map: "cannot load: BGM/enter_the_east.mp3".
    already = set(n for n, _p in file_list)
    bgm_dir = os.path.join(CLIENT, 'BGM')
    if os.path.isdir(bgm_dir):
        for n in sorted(os.listdir(bgm_dir)):
            name = 'bgm/' + n.lower()
            if name not in already:
                file_list.append((name, os.path.join(bgm_dir, n)))

    # REPAIRED .gr2: the files our Oodle1 does not
    # unpack lie unpacked in build/port/repaired_gr2/ (tools/
    # repair_gr2.py) and REPLACE the originals under the same name.
    overlay(file_list, os.path.join(PORT, 'repaired_gr2'), '.gr2', 'repaired .gr2 (granny2.dll)')
    # BAKED TREES: the client reads a tree from the corpus under its
    # .spt name and recognises the baked mesh by its magic (TMP4SPT1). Once
    # the corpus carried an old bake copied into client_files by hand
    # - the work directory stays a clean unpack of the packs, the
    # baker reads it, and the current bake replaces the originals here.
    overlay(file_list, workspace.BAKED, '.spt', 'baked trees (SpeedTree)')

    file_list.sort()
    return file_list


def overlay(file_list, directory, suffix, label):
    """Replaces entries of `file_list` by the files of `directory` with the same
    corpus name (lower-case relative path) ending in `suffix`; prints the count.
    """
    if not os.path.isdir(directory):
        return
    replacement = {}
    for root_node, _dir, names in os.walk(directory):
        for n in names:
            if n.lower().endswith(suffix):
                full = os.path.join(root_node, n)
                replacement[os.path.relpath(full, directory).replace(os.sep, '/').lower()] = full
    how_many = 0
    for i, (name, full) in enumerate(file_list):
        if name in replacement:
            file_list[i] = (name, replacement[name])
            how_many += 1
    print('%s: %d replaced' % (label, how_many))


def build_the_corpus(file_list):
    """Writes the chunks and the manifest. Returns (chunks, bytes)."""
    os.makedirs(TARGET, exist_ok=True)

    chunks = []          # (hash_hex, size)
    entries = []            # (chunk, offset, size, name)
    buffer = bytearray()
    chunk_number = 0

    def close_chunk():
        """Writes a buffer as a chunk. Its name is the hash of its content."""
        nonlocal buffer, chunk_number
        if not buffer:
            return
        data = bytes(buffer)
        digest = hashlib.sha256(data).hexdigest()[:32]
        path_str = os.path.join(TARGET, digest + '.bin')
        # A content-addressed chunk: if it already exists, it is identical.
        if not os.path.exists(path_str):
            io.open(path_str, 'wb').write(data)
        chunks.append((digest, len(data)))
        buffer = bytearray()
        chunk_number += 1

    # NO CHUNK MAY START WITH A BOM.
    # The client's fallback path reads a chunk with a synchronous XHR as text
    # (`charset=x-user-defined`), and the browser does BOM sniffing anyway:
    # a chunk starting with FE FF / FF FE / EF BB BF is decoded as
    # UTF-16/UTF-8 and returns half the "characters" - every file of that chunk
    # is read from a shifted place (measured: one such chunk
    # in the corpus, 44 .mss files with sound samples instead of text). When
    # the next chunk would start with a BOM, we close the current one a byte or
    # two earlier (the reader uses the chunk sizes from the manifest, so
    # a chunk shorter than nominal is legal).
    BOMS = (bytes([0xFE, 0xFF]), bytes([0xFF, 0xFE]), bytes([0xEF, 0xBB, 0xBF]))

    def starts_with_bom(b):
        """True when `b` starts with a byte order mark (BOMY)."""
        return any(b[:len(x)] == x for x in BOMS)

    def close_avoiding_bom(next_bytes):
        """Closes a full buffer so that the next chunk does not start with a BOM.
        `next_bytes` are the bytes that go into the next chunk right after the
        possibly moved tail of the buffer. Returns the moved tail.
        """
        nonlocal buffer
        cut = len(buffer)
        while cut > 0 and starts_with_bom(bytes(buffer[cut:]) + next_bytes[:3]):
            cut -= 1
        if cut == 0:
            raise SystemExit('a chunk made entirely of BOMs - this cannot happen')
        suffix_part = bytes(buffer[cut:])
        if suffix_part:
            # A file that started in the moved tail now starts
            # in the next chunk.
            for i in range(len(entries) - 1, -1, -1):
                k, shifted, extension, name_part = entries[i]
                if k != chunk_number:
                    break
                if shifted >= cut:
                    entries[i] = (k + 1, shifted - cut, extension, name_part)
            del buffer[cut:]
        close_chunk()
        buffer += suffix_part
        return suffix_part

    def next_start(idx):
        """The first 3 bytes of the next existing file (to peek for a BOM)."""
        for n2, sc2 in file_list[idx + 1:]:
            try:
                with io.open(sc2, 'rb') as fh:
                    return fh.read(3)
            except OSError:
                continue
        return b''

    for idx, (name, path_str) in enumerate(file_list):
        try:
            content = io.open(path_str, 'rb').read()
        except OSError:
            continue

        # A file starts WHERE THERE IS ROOM - and may cross the
        # chunk boundary. That is how the format works and how `WebFs::ReadFile` reads it.
        entries.append((chunk_number, len(buffer), len(content), name))

        rest = content
        while rest:
            place = CHUNK_SIZE - len(buffer)
            buffer += rest[:place]
            rest = rest[place:]
            if len(buffer) >= CHUNK_SIZE:
                close_avoiding_bom(rest[:3] if rest else next_start(idx))

    close_chunk()

    # --- manifest ---------------------------------------------------------
    parts = [b'M2WF']
    parts.append(struct.pack('<6I', 1, CHUNK_SIZE, len(chunks),
                              1, len(entries), min(len(chunks), 24)))
    for digest, size in chunks:
        parts.append(bytes.fromhex(digest))
        parts.append(struct.pack('<I', size))

    # ONE PACK. The format allows pack names (e.g. for a download order);
    # our reader does not use them, and faking a split without a reason would be
    # inventing content we did not measure.
    pack_name = b'client'
    parts.append(struct.pack('<H', len(pack_name)))
    parts.append(pack_name)

    for chunk, shift_by, size, name in entries:
        b = name.encode('utf-8')
        parts.append(struct.pack('<HIII', 0, chunk, shift_by, size))
        parts.append(struct.pack('<H', len(b)))
        parts.append(b)

    io.open(os.path.join(TARGET, 'manifest.bin'), 'wb').write(b''.join(parts))
    return len(chunks), sum(r for _s, r in chunks)


def verify():
    """Reads the finished corpus with OUR reader and compares it with the files on disk.

    The check is here for the same reason as with every other format
    in this project: a file that got written looks the same as a file
    that got written RIGHT. So I check a few dozen random files
    byte for byte, instead of trusting that the loop was correct.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from webfs import Corpus

    k = Corpus(TARGET)
    print('manifest: %d chunks, %d files' % (k.chunk_total, k.file_count_n))

    import random
    sample_set = random.sample(k.order, min(60, len(k.order)))
    sources = dict(collect_files())         # with the overlays - the file that really went in
    bad = 0
    for name in sample_set:
        from_corpus = k.read_file(name)
        path_str = sources.get(name.lower())
        if not path_str or not os.path.exists(path_str):
            continue
        from_disk = io.open(path_str, 'rb').read()
        if from_corpus != from_disk:
            bad += 1
            print('  DIFFERS: %s (%d vs %d bytes)'
                  % (name, len(from_corpus or b''), len(from_disk)))

    print('checked %d files, different: %d' % (len(sample_set), bad))
    return 0 if bad == 0 else 1


def main():
    """Builds the corpus: unpacks the packs (again with `--fresh`; only that with
    `--unpack-only`), collects the files with the repaired .gr2 and the baked
    trees, writes chunks and manifest, rewrites the prefetch order, then
    verifies; `--check` only verifies.
    """
    # Polish flag names from earlier still work.
    if '--check' in sys.argv:
        return verify()

    if not os.path.isdir(PACKS):
        print('missing ' + os.path.relpath(PACKS, ROOT))
        return 1

    if not os.path.isdir(WORK_DIR) or '--fresh' in sys.argv:
        how_many = unpack_all()
        print('\nextracted files: %d' % how_many)
    else:
        print('the work directory exists - `--fresh` extracts the packs again')
    # The bakers (trees, .gr2 repair) and the window descriptions read the
    # unpacked files, and the corpus takes their results - so a full build
    # unpacks first (`--unpack-only`) and makes the corpus after them.
    if '--unpack-only' in sys.argv:
        return 0

    file_list = collect_files()
    print('files for the corpus: %d' % len(file_list))

    chunk_count, byte_count = build_the_corpus(file_list)
    print('\nCORPUS: %d chunks, %.1f MB' % (chunk_count, byte_count / 1048576.0))
    print('in: ' + os.path.relpath(TARGET, ROOT))
    print('\nthe client uses it by default (corpus/ next to the page; ?corpus= for another place)')
    # Prefetch order: the chunk hashes
    # change with every rebuild, the list of file NAMES does not - so
    # we translate it to hashes here, not in the client.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import chunk_order
    chunk_order.save()
    remove_stale_chunks()
    return verify()


def remove_stale_chunks():
    """Deletes the chunks of earlier builds that the new manifest does not list
    (the work corpus had grown to 1712 files / 6.7 GB for a
    499-chunk manifest; package.py already copied only the listed ones)."""
    from check_corpus import read_corpus_manifest
    chunks, _files = read_corpus_manifest(os.path.join(TARGET, 'manifest.bin'))
    keep = {digest + '.bin' for digest, _size in chunks}
    removed = 0
    for name in os.listdir(TARGET):
        if name.endswith('.bin') and name != 'manifest.bin' and name not in keep:
            os.remove(os.path.join(TARGET, name))
            removed += 1
    if removed:
        print('removed %d chunks of earlier builds' % removed)


if __name__ == '__main__':
    sys.exit(main())
