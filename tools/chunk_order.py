#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""chunk_order.py - the order in which corpus chunks are prefetched.

On first entry the client reaches for ~150 chunks (600 MB) with a
synchronous XHR, frame by frame - the picture stands still. The page can
already have some of them in memory if it knows the ORDER (measured:
155 -> 115 XHR, a sliding window in `m2w_prefetch_start`). That order comes
from a recording in the game: the client writes every first reach for a
chunk into `m2w.corpus.order` (`compat/webfs_web.cpp`, `Daj`).

The permanent record is FILE NAMES (`tools/data/file_order.txt`): for
each chunk the first file that starts in it. Chunk numbers and hashes
change with every corpus rebuild, file names do not. `--save` translates
the names to hashes by the current manifest and writes
`corpus/order.txt` (one hash per line) - that is what the page
reads.

Usage:
  python tools/chunk_order.py --record file.json     # from the console:
        JSON.stringify([...new Set(m2w.corpus.order)])
  python tools/chunk_order.py --write                # after a corpus rebuild
"""
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, 'build', 'port', 'corpus')
PERMANENT = os.path.join(ROOT, 'tools', 'data', 'file_order.txt')

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_corpus import read_corpus_manifest  # noqa: E402


def first_files(file_list):
    """chunk -> name of the first file starting in it."""
    result = {}
    for name, ch, off, _sz in sorted(file_list, key=lambda e: (e[1], e[2])):
        result.setdefault(ch, name)
    return result


def from_recording(path_str):
    """Turns a recorded list of chunk numbers into the file names that start
    those chunks (first appearance) and stores them as the permanent order.
    """
    chunks, file_list = read_corpus_manifest(os.path.join(CORPUS, 'manifest.bin'))
    numbers_list = json.load(io.open(path_str, encoding='utf-8'))
    first_ones = first_files(file_list)
    names = []
    for n in numbers_list:
        if n in first_ones and first_ones[n] not in names:
            names.append(first_ones[n])
    os.makedirs(os.path.dirname(PERMANENT), exist_ok=True)
    io.open(PERMANENT, 'w', encoding='utf-8', newline='\n').write('\n'.join(names) + '\n')
    print('recording: %d chunks -> %d file names in %s' % (len(numbers_list), len(names), os.path.relpath(PERMANENT, ROOT)))


def save():
    """Rewrites the permanent file-name order into chunk hashes of the CURRENT
    corpus (the order file the prefetch reads); 1 without the order.
    """
    chunks, file_list = read_corpus_manifest(os.path.join(CORPUS, 'manifest.bin'))
    if not os.path.exists(PERMANENT):
        print('missing %s - run --record first' % os.path.relpath(PERMANENT, ROOT))
        return 1
    name_to_chunk = {}
    for name, ch, _off, _sz in file_list:
        name_to_chunk[name] = ch
    digests = []
    missing = 0
    for line_text in io.open(PERMANENT, encoding='utf-8'):
        name = line_text.strip()
        if not name:
            continue
        ch = name_to_chunk.get(name)
        if ch is None:
            missing += 1
            continue
        digest = chunks[ch][0]
        if digest not in digests:
            digests.append(digest)
    io.open(os.path.join(CORPUS, 'order.txt'), 'w', encoding='ascii', newline='\n').write('\n'.join(digests) + '\n')
    print('order.txt: %d chunks (%d names without a file in the corpus)' % (len(digests), missing))
    return 0


def main():
    """`--record FILE` (record, then write) or `--write` (write); prints the
    usage otherwise.
    """
    if '--record' in sys.argv:
        from_recording(sys.argv[sys.argv.index('--record') + 1])
        return save()
    if '--write' in sys.argv:
        return save()
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())
