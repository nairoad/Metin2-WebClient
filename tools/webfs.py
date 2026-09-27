#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""webfs.py - reads a data corpus in the M2WF format (by default
`build/port/corpus`, written by build_corpus.py; or pass another directory).

WHAT IT IS
==========
The streamed client keeps the game data not in `.epk` packs but in a few
hundred content-addressed chunks of 4 MB. `manifest.bin` says in which chunk
and at what offset each file lies.

The names in the manifest are plain paths (`ymir work/pc/warrior/...`), so
the content of the packs is **already unpacked and decrypted** there. That
means any game file can be reached through this corpus without touching the
`.epk` format.

WHY THIS MATTERS MORE THAN IT LOOKS
===================================
The `CEterPack` reader gives the files pack by pack and requires the
original client to be on disk. The corpus gives two things that road does not:

  1. **A list of everything** - every name with its size, without unpacking.
  2. **THE BOOT SET**: `bootChunks` says how many chunks are needed for the
     game to start at all - the answer to "what to preload".

THE CHECK THAT IS HERE
======================
A chunk is content-addressed, so its name is the hash of its content. The
script **computes and compares** it instead of believing - because a
replaced or incomplete file would look exactly like a good one until the
game loaded garbage from it.
"""

import hashlib
import io
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import workspace
DIST = workspace.CORPUS


class Corpus(object):
    """The manifest plus access to the chunks."""

    def __init__(self, directory=DIST):
        """Loads manifest.bin of the corpus in `directory`: header, chunk table, pack
        names and the file table (keyed by lower-case name, plus the original
        order); exits when it is not M2WF or does not end where it should.
        """
        self.directory = directory
        path_str = os.path.join(directory, 'manifest.bin')
        b = io.open(path_str, 'rb').read()
        if b[:4] != b'M2WF':
            raise SystemExit('this is not a webfs manifest: ' + path_str)

        (self.version, self.chunk_size, self.chunk_total, self.pack_count_n,
         self.file_count_n, self.boot) = struct.unpack_from('<6I', b, 4)

        p = 28
        self.chunk_list = []
        for _ in range(self.chunk_total):
            self.chunk_list.append((b[p:p + 16].hex(),
                                struct.unpack_from('<I', b, p + 16)[0]))
            p += 20

        self.pack_names = []
        for _ in range(self.pack_count_n):
            name_len = struct.unpack_from('<H', b, p)[0]
            p += 2
            self.pack_names.append(b[p:p + name_len].decode('utf-8', 'replace'))
            p += name_len

        self.file_list = {}
        self.order = []
        for _ in range(self.file_count_n):
            pack_name_str, chunk, shift_by, size = struct.unpack_from('<HIII', b, p)
            p += 14
            name_len = struct.unpack_from('<H', b, p)[0]
            p += 2
            name = b[p:p + name_len].decode('utf-8', 'replace')
            p += name_len
            self.file_list[name.lower()] = (pack_name_str, chunk, shift_by, size,
                                         name)
            self.order.append(name)

        if p != len(b):
            raise SystemExit('the manifest does not end where it should')

        self._cached = {}

    def chunk_path(self, iChunk):
        """Path of chunk `iChunk` on disk (`<hash>.bin`)."""
        return os.path.join(self.directory, self.chunk_list[iChunk][0] + '.bin')

    def chunk(self, iChunk):
        """The content of a chunk. Keeps the LAST one in memory - the files of one
        pack lie next to each other, so consecutive reads usually hit the same one.
        """
        if iChunk in self._cached:
            return self._cached[iChunk]

        data = io.open(self.chunk_path(iChunk), 'rb').read()
        # One at a time. Keeping all of them is gigabytes in memory.
        self._cached = {iChunk: data}
        return data

    def read_file(self, name):
        """The content of a game file, or `None` when it is not in the corpus."""
        entry = self.file_list.get(name.lower().replace('\\', '/'))
        if not entry:
            return None
        _pack, iChunk, uShift, uSize, _n = entry
        # A file may cross a chunk boundary - the rest lies from
        # offset 0 in the following chunks (that is how `WebFs::ReadFile` reads).
        # Earlier the reader took only one chunk and `build_corpus.py --check` reported
        # a false "DIFFERS" for files on the boundary.
        parts = []
        remaining = uSize
        while remaining > 0:
            data = self.chunk(iChunk)
            take = min(len(data) - uShift, remaining)
            if take <= 0:
                break
            parts.append(data[uShift:uShift + take])
            remaining -= take
            iChunk += 1
            uShift = 0
        return b''.join(parts)

    def verify_chunk(self, iChunk):
        """Whether the chunk's hash agrees with its name. Returns (ok, reason)."""
        expected_one, size = self.chunk_list[iChunk]
        path_str = self.chunk_path(iChunk)
        if not os.path.exists(path_str):
            return False, 'no such file'

        data = io.open(path_str, 'rb').read()
        if len(data) != size:
            return False, 'size %d, the manifest says %d' % (len(data), size)

        # The file name is the hash of its content: SHA-256 CUT TO 16 BYTES.
        #
        # I first guessed MD5, because it has exactly 16 bytes - and that is
        # reasoning that sounds good and was wrong. A measurement decided:
        # I computed five hash functions on chunk zero
        # and SHA-256 matched to the character.
        computed = hashlib.sha256(data).hexdigest()[:32]
        if computed != expected_one:
            return False, 'hash %s, expected %s' % (computed[:12],
                                                       expected_one[:12])
        return True, 'ok'


def main():
    """Prints the manifest summary, how many chunks are on disk, the hash check
    of the first three chunks and a sample DDS read.
    """
    k = Corpus()
    print('manifest: %d chunks, %d packs, %d files, start-up set %d'
          % (k.chunk_total, k.pack_count_n, k.file_count_n, k.boot))
    print('chunk size: %d B' % k.chunk_size)

    present = sum(1 for i in range(k.chunk_total)
                   if os.path.exists(k.chunk_path(i)))
    print('chunks on disk: %d of %d' % (present, k.chunk_total))

    # HASH CHECK on the first three chunks of the boot set.
    # Not on all of them, because that is 1.7 GB - three are enough to decide
    # whether we understand at all what a file name is.
    print('\nhash check (the first three chunks):')
    for i in range(min(3, k.chunk_total)):
        ok, reason = k.verify_chunk(i)
        print('  chunk %d: %s' % (i, 'MATCHES' if ok else reason))

    # Reading a file and checking that it really is this format.
    print('\nreading a sample file:')
    for name in k.order[:400]:
        if name.lower().endswith('.dds'):
            data = k.read_file(name)
            tag = data[:4] if data else b''
            print('  %s' % name)
            print('  %d bytes, tag %r %s'
                  % (len(data), tag,
                     'this is DDS' if tag == b'DDS ' else 'NOT DDS'))
            break
    return 0


if __name__ == '__main__':
    sys.exit(main())
