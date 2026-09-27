#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""oodle1.py - unpacking `.gr2` sections packed with the Oodle1 codec.

ORIGIN OF THE DESCRIPTION
=========================
Written from the OPEN SPECIFICATION of the algorithm:

    liboodle - "The Oodle1 Compression Scheme"
    https://github.com/LunaticInAHat/liboodle
    Licence: Unlicense (PUBLIC DOMAIN, no obligations)

Oodle1 is RAD Game Tools' own codec from the Granny 2 era, NOT the modern
"Oodle". It is in no system library and cannot be bypassed:
in the corpus seven of eight sections of every `.gr2` are packed with it
(measured on 400 files).

WHY PYTHON FIRST
================
This is a RESEARCH tool. The reader is ultimately to be in C++ and in the client -
but the format is unknown to us, and with an unknown format what counts is the number
of attempts per hour. In Python an attempt takes seconds, in C++ five minutes of building.
When it works, we will rewrite it in C++ HAVING SOMETHING TO COMPARE THE RESULT WITH - and that
is the real value of this file, not the unpacked byte itself.

THREE LAYERS
============
    bit reader     - yields a fixed-point number in the range [0, 1)
    symbol coder   - an adaptive arithmetic coder, THREE alphabets
    LZ layer       - dictionary-based, LZSS style, with 327 separate coders

The constant 0x4000 is the representation of 1.0. There are no floating-point numbers anywhere.

A DISCREPANCY IN THE SPECIFICATION, RESOLVED
============================================
The pseudocode gives the size of the "1k" alphabet as `(window / 4) + 1`, and the text next to it
as `(window / 1024) + 1`. The reference implementation says `/1024` and so does the text -
I take `/1024`.

DELIBERATE DEPARTURES FROM THE REFERENCE
========================================
1. The reference implementation keeps symbols in `uint8_t`. The "1k" alphabet with
   a 256 KiB window reaches the value 256, however, which does NOT fit in a byte.
   Here the numbers are full - copying someone else's overflow would be faithfulness
   to a bug, not to the format.
2. The coder's tables grow when the active alphabet outgrows the alphabet - see
   `_ensure_room`. The reference then writes past the table.
3. Length coder number 64 gets its symbol count from the FOURTH group.
   The reference shifts the header by a byte too many and gives it zero, which means
   nothing. Checked on 400 files: both versions give the same result.

WHAT REMAINS UNSOLVED, AND WHY IT IS NOT OUR FAULT
============================================================
On 400 random corpus files FOUR fall apart in the middle of one
section: `sura_fencing1_germany_lod_01`, `sura_capoeira1_lod_02`,
`hair_16_1` (the Sura version) and `dungeon_c1`. The rest - 396 - come out to
the byte, matching the size from the section table.

Before I took this as my own mistake, I BUILT THE REFERENCE IMPLEMENTATION
(`liboodle`, emscripten, run under node) and ran it on the same
section. Result: byte for byte the same as mine, with the same overshoot
of 8 bytes. So this port is FAITHFUL to the description, and the description itself falls short.

What else I measured to narrow it down:
  - The divergence is total, not local: from some point (in `hair_16_1`
    byte ~10532 of 12160) everything further is garbage, and the input ends
    prematurely (9691 of 9921 bytes).
  - Up to that point the data is flawless. The measure is THE NORMAL'S LENGTH:
    in good files zero vertices have a normal different from unit length,
    here the first such appears exactly past the point of divergence.
  - Three fixes checked and REJECTED: the length coder group boundaries
    from the header description (0-15, 16-23, 24-31, 32-64) break files that come out
    today; breaking the tie in `split_range` with `>=` instead of `>` too;
    zero versus the fourth group for coder 64 changes nothing.
  - None of the rare events (an empty span, no hit in the active
    alphabet, a zeroed modulus) falls near the divergence.

Until this is solved, `unpack_section` prefers to SHOUT rather than return
damaged data: every stream must end to the byte at its
boundary, otherwise `Oodle1Error` is raised.
"""

import io
import os
import struct
import sys

class Oodle1Error(Exception):
    """The section cannot be unpacked according to the algorithm's description.

    A separate exception, so that the caller can TELL "this file is beyond the
    reach of today's Oodle1 description" from an ordinary bug in the code.
    """


ONE = 0x4000          # fixed-point representation of 1.0
MASK32 = 0xFFFFFFFF

# Repeat length code -> real length.
# Codes 1..60 are lengths 2..61; the last four are stepped.
LENGTH_TABLE = (
    [0] + list(range(2, 62)) + [128, 192, 256, 512]
)
assert len(LENGTH_TABLE) == 65


class BitReader(object):
    """The "7+1" split.

    Every input byte enters the register with its seven high bits,
    and its lowest bit waits outside the register and enters only with the
    next byte. Thanks to that the lowest bit read does not affect the
    register's value until it becomes certain.
    """

    def __init__(self, data, start_pos=0):
        """Starts reading `data` at byte `start_pos`: the first byte split into its
        seven high bits (register) and the low bit (waiting outside).
        """
        self.d = data
        self.i = start_pos
        self.mean = self.d[self.i] >> 1
        self.lsb = self.d[self.i] & 1
        self.module = 0x80
        self.i += 1

    def _byte(self):
        """The next input byte, or 0 past the end (the decoder stops by output size
        and may read a few bytes beyond the data).
        """
        # The stream is sometimes read a few bytes past the end of the data - that is
        # intended: the decompressor ends by the NUMBER OF OUTPUT BYTES, not
        # by an end marker, so the last reads may go outside the buffer.
        # Zeros are the safe answer here (the specification anyway says to
        # pad the input with zeros to a multiple of four).
        if self.i < len(self.d):
            b = self.d[self.i]
        else:
            b = 0
        self.i += 1
        return b

    def load_register(self):
        """Shifts whole bytes in ("7+1" split) until the modulus exceeds 0x800000."""
        while self.module <= 0x800000:
            self.mean = ((self.mean << 1) | self.lsb) & MASK32
            b = self._byte()
            self.mean = ((self.mean << 7) | (b >> 1)) & MASK32
            self.lsb = b & 1
            self.module = (self.module << 8) & MASK32

    def peek(self, one_step):
        """The next value on the scale [0, jeden) without consuming it (clamped to
        jeden - 1; 0 when the modulus is smaller than the scale).
        """
        self.load_register()
        scale = self.module // one_step
        if scale == 0:
            return 0
        z = self.mean // scale
        return z if z < one_step - 1 else one_step - 1

    def consume(self, min_z_value, span, one_step):
        """Consumes the interval [min_z, min_z + span) of the scale `one_step`;
        the top interval takes the whole remaining modulus.
        """
        scale = self.module // one_step
        sz = min_z_value * scale
        self.mean = (self.mean - sz) & MASK32
        if min_z_value < (one_step - span):
            self.module = (span * scale) & MASK32
        else:
            self.module = (self.module - sz) & MASK32

    def take(self, one_step):
        """A peek combined with consumption - for a uniformly distributed value."""
        if one_step <= 1:
            return 0
        self.load_register()
        scale = self.module // one_step
        if scale == 0:
            return 0
        z = self.mean // scale
        if z > one_step - 1:
            z = one_step - 1
        sz = z * scale
        self.mean = (self.mean - sz) & MASK32
        if z < one_step - 1:
            self.module = scale & MASK32
        else:
            self.module = (self.module - sz) & MASK32
        return z


class SymbolCoder(object):
    """An adaptive arithmetic coder with THREE alphabets.

    active        - learned symbols, with assigned spans
    conditional   - learned since the last normalization, with equal chances
    proportional  - all possible ones, with equal chances

    Symbol 0 is the ESCAPE - it is never a result, it only switches the alphabet.
    """

    def __init__(self, alphabet_size, unique_symbols):
        """An adaptive coder for `alphabet_size` values of which
        `unique_symbols` can occur: only the escape known, decay and
        renormalisation thresholds from the alphabet size.
        """
        n = alphabet_size + 2      # room for the escape (0.0) and "1" (1.0)
        self.US = unique_symbols
        self.LS = [0] * n             # symbol values
        self.SW = [ONE] * n         # cumulative weights (thresholds in the range)
        self.LSW = [0] * n            # freshness - how often the symbol occurs
        self.SW[0] = 0
        self.LSW[0] = 4               # escape weight
        self.TLW = 4
        self.HLS = 0                  # highest learned
        self.HLSN = 0                 # highest at the last normalization
        self.NRW = 8                  # the weight at which we normalize
        self.DT = max(256, min((alphabet_size - 1) * 32, 15160))
        self.RRI = 4                  # fast normalization interval
        self.RI = max(128, min((alphabet_size - 1) * 2, (self.DT // 2) - 32))

    def _ensure_room(self):
        """Adds room in the tables when the active alphabet has outgrown the alphabet.

        WHY THIS CAN HAPPEN AT ALL (measured). The escape goes out
        only when the last symbol is learned (`HLS == US`) - but its
        span in the range disappears only at the NEXT normalization.
        Between these two moments the coder still has a non-zero escape
        range, so it can send it, and the decoder MUST accept it: both
        sides look at the same table and stay in agreement. Then it learns
        a symbol it already knows - a costly, but correct, repetition.

        The reference implementation (`liboodle`) allocates exactly
        `alphabet + 2` positions and at such a moment WRITES PAST THE TABLE. In C++
        that is silent memory corruption, in Python an exception - and that is exactly
        why it came to light: 4 files of 400 (`sura_capoeira1_lod_02`,
        `hair_16_1`, `sura_fencing1_germany_lod_01`, `dungeon_c1`).
        Instead of copying someone else's overflow, the tables grow.
        """
        while len(self.LS) <= self.HLS + 1:
            self.LS.append(0)
            self.SW.append(ONE)
            self.LSW.append(0)

    def split_range(self):
        """Reduces the weights of rarely used symbols until they drop out of the alphabet."""
        self.LSW[0] //= 2
        self.TLW = self.LSW[0]
        largest = 0
        largest_index = 0
        i = 1
        while i <= self.HLS:
            while self.LSW[i] <= 1:
                if i >= self.HLS:
                    self.LSW[i] = 0
                    self.HLS -= 1
                    break
                # The highest learned symbol takes the place of the one dropping out -
                # the alphabet shrinks by one, without holes.
                self.LSW[i] = self.LSW[self.HLS]
                self.LSW[self.HLS] = 0
                self.LS[i] = self.LS[self.HLS]
                self.HLS -= 1
            if not self.LSW[i]:
                break
            self.LSW[i] //= 2
            self.TLW += self.LSW[i]
            if self.LSW[i] > largest:
                largest = self.LSW[i]
                largest_index = i
            i += 1

        if largest and largest_index != self.HLS:
            self.LSW[self.HLS], self.LSW[largest_index] = self.LSW[largest_index], self.LSW[self.HLS]
            self.LS[self.HLS], self.LS[largest_index] = self.LS[largest_index], self.LS[self.HLS]

        # The escape cannot drop out until we know all the symbols.
        if self.HLS != self.US and not self.LSW[0]:
            self.LSW[0] = 1
            self.TLW += 1

        for j in range(self.HLS + 1, len(self.SW)):
            self.SW[j] = ONE

    def normalize(self):
        """Divides the range [0,1) among the learned symbols."""
        # The quantum is computed from 0x20000, not from 0x4000, and divided by 8 at
        # the end - that gives three more bits of precision when truncating.
        quantum = 0x20000 // self.TLW
        self.SW[0] = 0
        total_sum = (self.LSW[0] * quantum) // 8
        for i in range(1, self.HLS + 1):
            self.SW[i] = total_sum
            total_sum += (self.LSW[i] * quantum) // 8

        if (self.RRI * 2) < self.RI:
            self.RRI *= 2
            self.NRW = self.TLW + self.RRI
        else:
            self.NRW = self.TLW + self.RI

        self.HLSN = self.HLS
        for j in range(self.HLS + 1, len(self.SW)):
            self.SW[j] = ONE

    def decode_symbol(self, bs, alphabet_size):
        """Decodes one symbol and updates the weights: renormalises (with decay)
        when due, then a known symbol or, after the escape, one from the
        not-yet-normalised tail or a brand new one read uniformly.
        """
        if self.TLW >= self.NRW:
            if self.TLW >= self.DT:
                self.split_range()
            self.normalize()

        z = bs.peek(ONE)

        i = 0
        while i <= self.HLSN:
            if self.SW[i + 1] > z:
                break
            i += 1

        bs.consume(self.SW[i], self.SW[i + 1] - self.SW[i], ONE)
        self.LSW[i] += 1
        self.TLW += 1

        if i:
            return self.LS[i]

        # ESCAPE: either a symbol from the conditional alphabet, or an entirely new one.
        if self.HLS != self.HLSN:
            if bs.take(2):
                i = bs.take(self.HLS - self.HLSN) + self.HLSN + 1
                self.LSW[i] += 2
                self.TLW += 2
                return self.LS[i]

        self.HLS += 1
        self._ensure_room()
        symbol = bs.take(alphabet_size)
        self.LS[self.HLS] = symbol
        self.LSW[self.HLS] += 2
        self.TLW += 2

        # Once we know all the symbols, the escape is no longer needed.
        if self.HLS == self.US:
            self.TLW -= self.LSW[0]
            self.LSW[0] = 0

        return symbol


class Decompressor(object):
    """The LZ layer - 327 separate coders, depending on context."""

    # Which of the four header bytes the coder with a given code belongs to.
    # There are 65 codes and four bytes - the split is by sixteen, and code 64
    # joins the last group.
    @staticmethod
    def code_group(code):
        """Which of the four header bytes covers length code `code` (groups of 16,
        code 64 in the last).
        """
        return min(code // 16, 3)

    def __init__(self, bs, header):
        """Sets up the LZ decoder from the three header words: window, literal
        alphabet and the 4 literal, 65 length and distance coders.
        """
        self.bs = bs
        self.window = header[0] >> 9
        self.LAS = header[0] & 0x1FF
        unique_literals = header[1] & 0x1FF
        largest_1k = header[1] >> 19

        # Four literal coders, chosen by the output position modulo 4.
        # With four-byte data (RGBA colours, floating-point numbers)
        # each then sees only one component, which changes slowly.
        self.literal_code = [SymbolCoder(self.LAS, unique_literals)
                          for _ in range(4)]

        # 65 length coders, chosen by the PREVIOUS length code.
        #
        # THE SPECIFICATION CONTRADICTS ITSELF, RESOLVED BY MEASUREMENT.
        # The header carries four bytes "how many different length codes occurred
        # in this context", one per group. The header description gives
        # the group boundaries as 0-15, 16-23, 24-31, 32-64, but both the pseudocode and the
        # reference implementation divide the coders into four equal sixteens.
        # I checked both versions on the corpus: with the boundaries from the description
        # sections fall apart that come out to the byte with sixteens
        # (among others `dungeon_c1` sections 1 and 6). Sixteens stay - the header
        # description is inaccurate at this point.
        rl = [(header[2] >> 24) & 0xFF, (header[2] >> 16) & 0xFF,
              (header[2] >> 8) & 0xFF, header[2] & 0xFF]
        self.length_code = [SymbolCoder(65, rl[self.code_group(code)])
                             for code in range(65)]

        self.O1AS = min(4, self.window + 1)
        o4as = min(256, (self.window // 4) + 1)
        o1024as = (self.window // 1024) + 1

        self.code_1b = SymbolCoder(self.O1AS, self.O1AS)
        self.code_4b = [SymbolCoder(o4as, o4as) for _ in range(256)]
        self.code_1k = SymbolCoder(o1024as, largest_1k + 1)

        self.byte_count = 0
        self.last_code = 0

    def step(self, output, pos):
        """One call: a literal or a repeat. Returns the number of bytes."""
        code = self.length_code[self.last_code].decode_symbol(self.bs, 65)
        self.last_code = code

        if not code:
            lit = self.literal_code[self.byte_count & 3].decode_symbol(self.bs, self.LAS)
            output[pos] = lit & 0xFF
            self.byte_count += 1
            return 1

        length = LENGTH_TABLE[code]
        active_window = min(self.window, self.byte_count)

        o1b = self.code_1b.decode_symbol(self.bs, self.O1AS) + 1
        o1k = self.code_1k.decode_symbol(self.bs, (active_window // 1024) + 1)
        o4b = self.code_4b[o1k].decode_symbol(
            self.bs, min(256, (active_window // 4) + 1))
        gap = (o1k * 1024) + (o4b * 4) + o1b

        # Copying BYTE BY BYTE, not as a block. With a distance smaller
        # than the length, the repeat reads its own fresh output - and that is how it should be,
        # because that is how LZ writes repeating patterns.
        source = pos - gap
        if source < 0:
            raise Oodle1Error('gap %d reaches before the start (pos %d)'
                             % (gap, pos))
        if pos + length > len(output):
            raise Oodle1Error('a repeat of %d B at %d goes outside the section (%d B)'
                             % (length, pos, len(output)))
        for k in range(length):
            output[pos + k] = output[source + k]
        self.byte_count += length
        return length


def unpack_section(packed, first16, first8, unpacked_size):
    """Unpacks one `.gr2` section - up to THREE streams in turn.

    The block starts with three headers of 12 bytes (36 in total). All
    three streams share ONE bit reader - only the decoder changes,
    because each stream has its own alphabet sizes.

    The boundaries come from the section table: `first16` and `first8` are the
    places where the unpacked data passes from 32-bit values
    to 16-, then to 8-bit ones. Separately they pack better than mixed.
    """
    if unpacked_size == 0:
        return bytearray()
    if len(packed) < 36:
        raise Oodle1Error('section shorter than three headers')

    headers = struct.unpack_from('<9I', packed, 0)
    bs = BitReader(packed, 36)
    output = bytearray(unpacked_size)

    boundaries = [first16, first8, unpacked_size]
    pos = 0
    for nr in range(3):
        if pos >= unpacked_size:
            break
        d = Decompressor(bs, headers[nr * 3:nr * 3 + 3])
        while pos < boundaries[nr]:
            pos += d.step(output, pos)
        if pos != boundaries[nr]:
            # EVERY STREAM MUST COME OUT TO THE BYTE. There is no end marker -
            # the end is recognized by the number of bytes - so overshooting the
            # boundary is the only visible trace of a divergence. Without this
            # check the first stream would overwrite the start of the second
            # and nobody would find out.
            raise Oodle1Error('stream %d overshot its boundary: %d instead of %d'
                             % (nr, pos, boundaries[nr]))

    return output


def main():
    """`oodle1.py FILE [SECTION]`: decompresses the Oodle1 sections of a .gr2
    (disk or corpus) and prints the size and first 16 bytes of each.
    """
    if len(sys.argv) < 2:
        print('usage: oodle1.py <.gr2 file from the corpus or a path> [section number]')
        return 1

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gr2

    name = sys.argv[1]
    if os.path.exists(name):
        data = io.open(name, 'rb').read()
    else:
        import webfs
        data = webfs.Corpus().read_file(name)

    p = gr2.GrannyFile(data)
    if p.error:
        print('ERROR: ' + p.error)
        return 1

    which = [int(sys.argv[2])] if len(sys.argv) > 2 else range(p.section_count)
    for i in which:
        s = p.sections[i]
        if s.unpacked_size == 0:
            print('[%d] pusta' % i)
            continue
        if s.compression == 0:
            print('[%d] uncompressed, %d B' % (i, s.unpacked_size))
            continue
        if s.compression != 2:
            print('[%d] compression %s - unsupported' % (i, s.compression_name))
            continue
        try:
            result = unpack_section(p.raw_section(i), s.first16,
                                    s.first8, s.unpacked_size)
            print('[%d] UNPACKED %d -> %d B   first 16 bytes: %s'
                  % (i, s.packed_size, len(result),
                     bytes(result[:16]).hex(' ')))
        except Exception as e:
            print('[%d] ERROR: %s' % (i, e))
    return 0


if __name__ == '__main__':
    sys.exit(main())
