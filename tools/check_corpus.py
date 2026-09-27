#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_corpus.py - checks the WHOLE client corpus byte for byte.

Reads the manifest `build/port/corpus/manifest.bin`, assembles every
file from the chunks (also across a chunk boundary, like `WebFs::ReadFile`)
and compares it with the source file in `build/port/client_files`. Also: no
chunk may start with a BOM (a synchronous XHR does
BOM sniffing and decodes such a chunk as UTF-16/UTF-8), and the size of
every chunk on disk must agree with the manifest.

`build_corpus.py --check` checks 60 random files; this tool checks all of
them (about 2 GB, a few dozen seconds).
"""
import io
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, 'build', 'port', 'corpus')
WORK_DIR = os.path.join(ROOT, 'build', 'port', 'client_files')
BOMS = (bytes([0xFE, 0xFF]), bytes([0xFF, 0xFE]), bytes([0xEF, 0xBB, 0xBF]))


def read_corpus_manifest(path_str):
    """Parses manifest.bin: (chunks as (hash hex, size), files as (name, chunk,
    offset, size)); exits when it is not M2WF.
    """
    d = io.open(path_str, 'rb').read()
    if d[:4] != b'M2WF':
        raise SystemExit('this is not an M2WF manifest')
    chunk_count, pack_count, file_count, _boot = struct.unpack_from('<4I', d, 12)
    p = 28
    chunks = []
    for _ in range(chunk_count):
        chunks.append((d[p:p + 16].hex(), struct.unpack_from('<I', d, p + 16)[0]))
        p += 20
    for _ in range(pack_count):
        n = struct.unpack_from('<H', d, p)[0]
        p += 2 + n
    file_list = []
    for _ in range(file_count):
        _pk, ch, off, sz = struct.unpack_from('<HIII', d, p)
        p += 14
        n = struct.unpack_from('<H', d, p)[0]
        name = d[p + 2:p + 2 + n].decode('utf-8', 'replace')
        p += 2 + n
        file_list.append((name, ch, off, sz))
    if p != len(d):
        raise SystemExit('manifest: the parse does not end at the end of the file')
    return chunks, file_list


def main():
    """Checks the corpus: every chunk present, of the manifest size and without
    a BOM; every file equal to its source on disk. 0 when all agree.
    """
    chunks, file_list = read_corpus_manifest(os.path.join(CORPUS, 'manifest.bin'))
    print('manifest: %d chunks, %d files' % (len(chunks), len(file_list)))

    errors_found = 0
    for i, (digest, size) in enumerate(chunks):
        sc = os.path.join(CORPUS, digest + '.bin')
        if not os.path.exists(sc):
            print('MISSING chunk %d %s' % (i, digest)); errors_found += 1; continue
        if os.path.getsize(sc) != size:
            print('SIZE of chunk %d: %d, manifest %d' % (i, os.path.getsize(sc), size)); errors_found += 1
        with io.open(sc, 'rb') as fh:
            b = fh.read(3)
        if any(b[:len(x)] == x for x in BOMS):
            print('BOM at the start of chunk %d %s (%s)' % (i, digest, b.hex())); errors_found += 1
    short_ones = sum(1 for _s, r in chunks[:-1] if r != chunks[0][1])
    print('chunks: errors %d, shorter than nominal (except the last): %d' % (errors_found, short_ones))

    memory = {}

    def chunk(i):
        """Bytes of chunk `i` (one chunk kept in memory at a time)."""
        if i not in memory:
            memory.clear()
            memory[i] = io.open(os.path.join(CORPUS, chunks[i][0] + '.bin'), 'rb').read()
        return memory[i]

    # the source of every corpus name as build_corpus collected it - with the
    # repaired .gr2 and the baked trees in place of the unpacked originals
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_corpus
    sources = dict(build_corpus.collect_files())
    checked = across_boundary = distinct = missing = 0
    for name, ch, off, sz in sorted(file_list, key=lambda e: (e[1], e[2])):
        source = sources.get(name.lower(), '')
        if not os.path.exists(source):
            missing += 1
            continue
        parts, remaining, ci, o = [], sz, ch, off
        while remaining > 0:
            data = chunk(ci)
            take = min(len(data) - o, remaining)
            if take <= 0:
                break
            parts.append(data[o:o + take]); remaining -= take; ci += 1; o = 0
        if ci - 1 != ch:
            across_boundary += 1
        checked += 1
        if b''.join(parts) != io.open(source, 'rb').read():
            distinct += 1
            if distinct <= 10:
                print('DIFFERS: %s (chunk %d, offset %d, %d B)' % (name, ch, off, sz))
    print('files checked %d (across a chunk boundary %d), without a source on disk %d, DIFFERENT %d'
          % (checked, across_boundary, missing, distinct))
    return 0 if (errors_found == 0 and distinct == 0) else 1


if __name__ == '__main__':
    sys.exit(main())
