#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""spt.py - parsing `.spt` files, i.e. the SpeedTree TREE RECIPE.

WHY THIS EXISTS
===============
Trees are **a third of the objects** on outdoor maps (measured:
map a1 - 368 of 975) and **there is no other road to them** than
SpeedTree: among 1083 objects of type `Building` there is practically no
vegetation. To draw them, this format first has to be read.

WHAT THIS FILE DOES NOT DO - and this is the most important sentence in the header
===================================================================================
`.spt` **does not contain a tree**. It contains **a recipe**: Bezier curves,
a random seed and branching parameters. The mesh is produced only by the engine -
SpeedTree's `CTreeEngine`, `CFrondEngine`, `CWindEngine` and `CLightingEngine`.

So this parser is **the first of two steps**, not half the work:
first read the recipe, then write a geometry generator. Whoever confuses
the two will estimate the cost several times too low - as I did,
when I wrote "about ten rounds, as with `.gr2`". `.gr2` **contains
a mesh**; `.spt` does not.

THE FORMAT - read from the file, not guessed
============================================
A stream of tags, **without word alignment**. A tag is a 32-bit
little-endian number; after it a value whose SIZE depends on
WHICH tag it is. The file is guarded by the string `__IdvSpt_02_` at the start.

The start of `b1_baobab_rt.spt`, byte by byte:

    0x00  1000  length 12, content "__IdvSpt_02_"
    0x14  1002  (no value)
    0x18  2000  length 28, "D:\\Tree\\...BaobabBark.tga"
    0x3c  2001  1100.0        (four bytes)
    0x44  2002  0x00          (ONE byte - hence no alignment)
    0x49  2003  100.0
    0x51  2005  840158
    0x59  2006  640.0
    0x61  2007  5.0
    0x69  1014  4
    0x71  1016  length 104, content "BezierSpline 0..."

The curves are written **as text**: `BezierSpline`, then lines
`0 1 0.714831 -0.699297 0.079604`.

THE METHOD - FIRST WE LEARN, THEN WE READ
=========================================
The size of a value cannot be inferred from the tag alone without a description
of the format, which we do not have. But it can be **measured**, and that is the whole
trick of this file:

  STEP 1 (learning). For every occurrence of a tag we check which
         value size makes ANOTHER credible tag stand RIGHT AFTER IT.
         Votes are collected from ALL files at once. A tag
         that once comes out as one byte and once as four is disputed
         and gets recorded, not passed over in silence.

  STEP 2 (reading). With the table of sizes, we read the file directly - without
         guessing, without backtracking.

  STEP 3 (check). `the parse ends EXACTLY at the end of the file`.

The check is strict, because an error of one byte shifts everything after it,
and the parse either falls out of the file or ends too early. With 87
files of different sizes an accidental hit is ruled out.
It is the same check that proved itself with `manifest.bin`.

STATE - UNFINISHED, and it is written here plainly
================================================================
**The check does NOT PASS: 0 of 87 files parse to the end.**

That is not a failure of the tool but its result: the model of the format I
wrote down here is **incomplete**, and I know it because the check fails -
not because something seems so to me.

WHAT IS ESTABLISHED (measured, not guessed):
  * a stream of tags, **without alignment** to a word;
  * the header `__IdvSpt_02_`, tags from 1000 upwards;
  * tag 2000 is **the bark texture name** - one in each
    of the 87 files, e.g. `D:\\Tree\\...\\BaobabBark.tga`;
  * curves written **as text**: `BezierSpline` and lines of coordinates.
    696 curves in 87 files;
  * some tags have a **one-byte** value (6015, 6016, 20003,
    20004) - hence no alignment;
  * some tags carry **arrays of numbers**: at the end of `b1_baobab_rt.spt`
    stands 20005, after it EIGHT numbers in a row, and the file ends with the bare
    tag 20001 without a value.

WHAT I CANNOT DO (and this is the next step):
  Greedy reading gets to about byte 1130 of 6689 and drifts.
  Parsing with backtracking, requiring it to reach EXACTLY the end, finds
  **no** solution with the model {none, byte, multiple of
  four, length plus content}. So there is something in the file that is not
  a tag-value pair: most likely a nested block with its own length
  or a repeat counter. It has to be looked for in the area after the first
  `BezierSpline`, because up to that point greedy reading goes correctly.

A SIDE NOTE: `corpus_files` holds **87** `.spt` files, while the corpus
manifest knows **118**. The difference is not investigated - the extractor may not
have extracted them. Check before taking the set as complete.
"""

import os
import struct
import sys


FILE_TAG = b'__IdvSpt_02_'

# The range of numbers we take as credible tags.
#
# THE LOWER BOUND IS AN ASSUMPTION AND IS WRITTEN DOWN HERE AS SUCH. All the tags
# seen in these files lie from 1000 upwards (1000, 1002, 1014, 1016,
# 2000-2007...). Without a lower bound the learning took A STRING LENGTH for a tag:
# at `1002` (a tag without a value) four bytes further stands the number 28,
# i.e. the length of the texture name - and that looked like a credible tag.
#
# The assumption need not be taken on faith: if it is false, the check
# "the parse ends exactly at the end of the file" will not pass.
SMALLEST_TAG = 1000
LARGEST_TAG = 60000

# The value sizes we try during learning. The order matters
# only on a tie of votes.
SIZE_NONE = 0
SIZE_BYTE = 1
SIZE_WORD = 4
SIZE_STRING = -1   # four bytes of length, then the content


def _u32(data, pos):
    """Little-endian uint32 at `pos`."""
    return struct.unpack_from('<I', data, pos)[0]


def _f32(data, pos):
    """Little-endian float at `pos`."""
    return struct.unpack_from('<f', data, pos)[0]


def _credible_tag(data, pos):
    """Whether something that looks like a tag stands at `pos`."""
    if pos + 4 > len(data):
        return False
    z = _u32(data, pos)
    return SMALLEST_TAG <= z <= LARGEST_TAG


def _end(data, pos):
    """True when `pos` is exactly the end of the data."""
    return pos == len(data)


def _string_length(data, pos):
    """If a length followed by content stands at `pos` - returns it."""
    if pos + 4 > len(data):
        return None
    name_len = _u32(data, pos)
    if 0 < name_len <= 65536 and pos + 4 + name_len <= len(data):
        return name_len
    return None


def _parse_with_backtracking(data):
    """Finds a parse that ends EXACTLY at the end of the file.

    Greedy reading is not enough: at tag 6015 the value has one
    byte, but a four-byte read also gives something credible-looking
    at this place - and the whole further parse shifts by three bytes. The error
    shows up only a few hundred bytes further, so the decision cannot be
    made locally.

    That is why we compute BACKWARDS: `reachable[p]` says whether from offset `p` one can
    reach the end of the file. Then at every place we choose only those
    possibilities that lead to the end - and there is no drift.

    Returns a list of `(tag, size, value_offset)` or `None`.
    """
    n = len(data)
    reachable = [None] * (n + 1)     # None = not computed, False/tuple = result
    reachable[n] = ()                # end of file: reachable, without a move

    # We compute from the end, so every `p` already sees the finished results for larger ones.
    for p in range(n - 1, -1, -1):
        reachable[p] = False
        if p + 4 > n:
            continue
        z = _u32(data, p)
        if not (SMALLEST_TAG <= z <= LARGEST_TAG):
            continue
        after = p + 4

        # The order of attempts does not affect correctness - each has to
        # reach the end of the file anyway - but it affects which of several
        # correct parses we choose. The string first, because it is
        # the most characteristic: it carries its own length.
        name_len = _string_length(data, after)
        if name_len is not None and after + 4 + name_len <= n and reachable[after + 4 + name_len]:
            reachable[p] = (z, SIZE_STRING, after, after + 4 + name_len)
            continue

        # ARRAYS OF NUMBERS, not just single values.
        #
        # At the end of `b1_baobab_rt.spt` stands tag 20005, and after it EIGHT
        # numbers in a row and only then the next tag. The model "tag
        # plus one value" had no way to read that, and that is why NONE
        # of the 87 files parsed to the end.
        #
        # So we allow multiples of four bytes. The choice is still not
        # guessing: every possibility has to lead to THE END OF THE FILE,
        # and the consistency of the size of the same tag across 87 files we check
        # separately.
        found = False
        for uCount in (4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 64):
            if after + uCount <= n and reachable[after + uCount]:
                reachable[p] = (z, uCount, after, after + uCount)
                found = True
                break
        if found:
            continue

        if after + 1 <= n and reachable[after + 1]:
            reachable[p] = (z, SIZE_BYTE, after, after + 1)
            continue
        if reachable[after]:
            reachable[p] = (z, SIZE_NONE, after, after)
            continue

    header = struct.pack('<II', 1000, len(FILE_TAG)) + FILE_TAG
    if not data.startswith(header):
        return None
    p = len(header)
    if not reachable[p]:
        return None

    result = [(1000, SIZE_STRING, 4)]
    while p < n:
        z, size, value_pos, next_one = reachable[p]
        result.append((z, size, value_pos))
        p = next_one
    return result


def learn_sizes(file_list):
    """STEP 1: learns the value size of each tag.

    For each file it finds a parse reaching exactly the end (see
    `_parse_with_backtracking`) and counts what size came out for each
    tag. A tag that once comes out as one byte and once as four
    is **disputed** and gets recorded, not passed over in silence - because either
    the format has something there we do not understand, or our parse chose
    one of several correct ones.
    """
    votes = {}
    without_parse = []
    for path_str in file_list:
        with open(path_str, 'rb') as f:
            data = f.read()
        r = _parse_with_backtracking(data)
        if r is None:
            without_parse.append(path_str)
            continue
        for z, size, _ in r:
            votes.setdefault(z, {}).setdefault(size, 0)
            votes[z][size] += 1

    array = {}
    disputed = {}
    for z, counter in votes.items():
        array[z] = max(counter, key=lambda r: counter[r])
        if len(counter) > 1:
            disputed[z] = dict(counter)
    return array, disputed, without_parse


class Parse(object):
    def __init__(self, path_str=None):
        """An empty parse result for `path_str`: no fields, textures or curves yet."""
        self.path_str = path_str
        self.field_list = []          # (tag, value)
        self.textures = []
        self.curve_count = 0
        self.to_end = False
        self.error = None


def parse_data(data, array):
    """STEP 2: reads the file directly, with the table of sizes."""
    w = Parse()
    header = struct.pack('<II', 1000, len(FILE_TAG)) + FILE_TAG
    if not data.startswith(header):
        w.error = 'no %s tag' % FILE_TAG.decode('ascii')
        return w
    w.field_list.append((1000, FILE_TAG.decode('ascii')))
    pos = len(header)

    while pos < len(data):
        if pos + 4 > len(data):
            w.error = 'a tail of %d bytes, too short for a tag' % (len(data) - pos)
            return w
        z = _u32(data, pos)
        pos += 4
        size = array.get(z)
        if size is None:
            w.error = 'tag %d unknown (offset %d)' % (z, pos - 4)
            return w

        if size == SIZE_NONE:
            w.field_list.append((z, None))
        elif size == SIZE_BYTE:
            if pos + 1 > len(data):
                w.error = 'no value byte for tag %d' % z
                return w
            w.field_list.append((z, data[pos]))
            pos += 1
        elif size > 0 and size % 4 == 0:
            if pos + size > len(data):
                w.error = 'no value for tag %d' % z
                return w
            numbers = [_f32(data, pos + i) for i in range(0, size, 4)]
            w.field_list.append((z, numbers[0] if len(numbers) == 1 else numbers))
            pos += size
        else:
            if pos + 4 > len(data):
                w.error = 'no string length for tag %d' % z
                return w
            name_len = _u32(data, pos)
            pos += 4
            if pos + name_len > len(data):
                w.error = 'the string of tag %d goes past the end of the file' % z
                return w
            content = data[pos:pos + name_len].decode('latin-1')
            pos += name_len
            w.field_list.append((z, content))
            if content.lower().endswith(('.tga', '.dds', '.jpg')):
                w.textures.append(content)
            if 'BezierSpline' in content:
                w.curve_count += content.count('BezierSpline')

    w.to_end = (pos == len(data))
    if not w.to_end:
        w.error = 'the parse ended at %d of %d bytes' % (pos, len(data))
    return w


def parse_file(path_str, array):
    """Parses the .spt file at `path_str` with the learned tag table."""
    with open(path_str, 'rb') as f:
        data = f.read()
    w = parse_data(data, array)
    w.path_str = path_str
    return w


def find_files(directory):
    """Every .spt file under `directory`, sorted."""
    file_list = []
    for r, _, fs in os.walk(directory):
        for n in fs:
            if n.lower().endswith('.spt'):
                file_list.append(os.path.join(r, n))
    file_list.sort()
    return file_list


def main():
    """Learns the tag table from every .spt in the directory, then parses each
    and checks the parse ends exactly at the end of the file; prints the
    totals, unparsed files and ambiguous tags. 1 when any file failed.
    """
    directory = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        'build', 'port', 'corpus_files', 'ymir work', 'tree')

    file_list = find_files(directory)
    if not file_list:
        print('not a single .spt file found in %s' % directory)
        return 1

    array, disputed, without_parse = learn_sizes(file_list)
    print('.spt files: %d' % len(file_list))
    print('tags learned: %d (disputed: %d)'
          % (len(array), len(disputed)))
    if without_parse:
        print('WITHOUT A PARSE REACHING THE END: %d files' % len(without_parse))

    to_end = 0
    bad = []
    fields = texture_count = curve_count = 0
    for p in file_list:
        w = parse_file(p, array)
        fields += len(w.field_list)
        texture_count += len(w.textures)
        curve_count += w.curve_count
        if w.to_end:
            to_end += 1
        else:
            bad.append((p, w.error))

    print()
    print('CHECK: the parse ends EXACTLY at the end of the file: %d of %d'
          % (to_end, len(file_list)))
    print('fields in total %d | texture names %d | Bezier curves %d'
          % (fields, texture_count, curve_count))

    if bad:
        print()
        print('NOT PARSED (%d):' % len(bad))
        for p, b in bad[:12]:
            print('   %-42s %s' % (os.path.basename(p), b))

    if disputed:
        print()
        print('DISPUTED TAGS - different sizes in different places:')
        for z in sorted(disputed)[:12]:
            print('   %6d: %s' % (z, disputed[z]))

    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
