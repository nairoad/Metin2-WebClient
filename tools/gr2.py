#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gr2.py - parsing a Granny `.gr2` file.

WHY PYTHON FIRST
================
This is a RESEARCH tool, not the final data path. The `.gr2` reader
is ultimately to be written in C++ and sit in the client (the user's decision:
"ease of replacing data" - servers with their own models distribute them as
`.gr2`, so a reader inside means "you replace the pack and it works").

But the format is UNKNOWN to us, and with an unknown format what counts is the number
of attempts per hour. One attempt in Python is seconds; one attempt in C++ is
a five-minute build. Once we can unpack and describe the file,
we will rewrite this in C++ - already having something to compare the result with.

WHAT WE KNOW FROM MEASUREMENT
=======================================
400 examined files from the corpus, all the same:

    magic          b8 67 b0 ca f8 6d b1 0f 84 72 8c 7e 5e 19 00 1e
                   (Granny little-endian 32-bit)
    format version 6
    header         uncompressed (format 0)
    sections       8, of which SEVEN packed with `Oodle1`

Oodle1 is Granny's own codec, not the modern one from RAD.

THE FORMAT DESCRIBES ITSELF
=====================================
This is Granny's whole trick and the reason one reader
will handle models from ANY server, including ones we have never
seen. There is no "character structure" hard-coded in the file.
There is a TYPE TREE: an array of field descriptions (`granny_data_type_definition`),
and each description gives the kind of field, its name and - for compound fields -
a pointer to the description of the type it is made of. The file's root is a pair
(pointer to type, pointer to object), so reading starts with
reading the DESCRIPTION, not with an assumption.

The practical effect: we need not know `granny.h` to extract the skeleton.
We have it and compare the names with it - but as a CHECK, not as a
source. A model exported with a newer exporter will add a field and our reader
will see it, instead of going out of alignment by four bytes.
"""

import io
import os
import struct
import sys

# Granny magics. We are interested in the first - the rest is here so that
# a file in another variant gives an understandable message, not "unknown magic".
MAGICS = {
    bytes.fromhex('b867b0caf86db10f84728c7e5e19001e'):
        'little-endian 32-bit',
    bytes.fromhex('0e11958654a2c9dd8fbe6d0b6d70c0f4'):
        'big-endian 32-bit',
    bytes.fromhex('e57b0efcfd48c4b5b17048f9c1f78bb0'):
        'little-endian 32-bit (variant 2)',
    bytes.fromhex('dfeacd4dbfe1a4e05a24c6b3fef5a5a1'):
        'little-endian 64-bit',
}

COMPRESSION_NAME = {
    0: 'none',
    1: 'Oodle0',
    2: 'Oodle1',
    3: 'BitKnit',
    4: 'BitKnit2',
}

# ---------------------------------------------------------------------------
# FIELD KINDS
# ---------------------------------------------------------------------------
# The order is taken from `granny_member_type` in
# reference/tmp4_source/extern/include/granny.h - it is an enumeration
# NUMBERED FROM ZERO, so the numbers in the file are indices into this list.
# I shorten the names (without the `Granny` prefix), because they appear in every
# line of the dump.

END = 0
INLINE = 1               # Inline - an object written in place
REFERENCE = 2             # Reference - a pointer
REFERENCE_TO_ARRAY = 3  # ReferenceToArray - a count + a pointer to a sequence
ARRAY_OF_REFERENCES = 4       # ArrayOfReferences - a count + a pointer to pointers
VARIANT_REFERENCE = 5     # VariantReference - (type, object)
UNSUPPORTED = 6
REFERENCE_TO_VARIANT_ARRAY = 7   # (type, count, pointer)
STRING = 8
TRANSFORM = 9       # Transform - 68 bytes
REAL32 = 10
INT8 = 11
UINT8 = 12
INT8_BINORMAL = 13
UINT8_NORMAL = 14
INT16 = 15
UINT16 = 16
INT16_BINORMAL = 17
UINT16_NORMAL = 18
INT32 = 19
UINT32 = 20
REAL16 = 21
EMPTY_REFERENCE = 22

KIND_NAME = {
    END: 'End',
    INLINE: 'Inline',
    REFERENCE: 'Reference',
    REFERENCE_TO_ARRAY: 'RefToArray',
    ARRAY_OF_REFERENCES: 'ArrayOfRefs',
    VARIANT_REFERENCE: 'VariantRef',
    UNSUPPORTED: 'Unsupported',
    REFERENCE_TO_VARIANT_ARRAY: 'RefToVariantArray',
    STRING: 'String',
    TRANSFORM: 'Transform',
    REAL32: 'Real32',
    INT8: 'Int8',
    UINT8: 'UInt8',
    INT8_BINORMAL: 'BinormalInt8',
    UINT8_NORMAL: 'NormalUInt8',
    INT16: 'Int16',
    UINT16: 'UInt16',
    INT16_BINORMAL: 'BinormalInt16',
    UINT16_NORMAL: 'NormalUInt16',
    INT32: 'Int32',
    UINT32: 'UInt32',
    REAL16: 'Real16',
    EMPTY_REFERENCE: 'EmptyReference',
}

# The size of ONE element of a field in bytes, in a 32-bit file.
# `Inline` has no entry here, because its size is computed from the description of the type
# it points to - and that is the only place where the arithmetic is
# recursive.
KIND_SIZE = {
    END: 0,
    REFERENCE: 4,
    REFERENCE_TO_ARRAY: 8,
    ARRAY_OF_REFERENCES: 8,
    VARIANT_REFERENCE: 8,
    UNSUPPORTED: 0,
    REFERENCE_TO_VARIANT_ARRAY: 12,
    STRING: 4,
    TRANSFORM: 68,
    REAL32: 4,
    INT8: 1,
    UINT8: 1,
    INT8_BINORMAL: 1,
    UINT8_NORMAL: 1,
    INT16: 2,
    UINT16: 2,
    INT16_BINORMAL: 2,
    UINT16_NORMAL: 2,
    INT32: 4,
    UINT32: 4,
    REAL16: 2,
    EMPTY_REFERENCE: 4,
}

# A field description in the file: kind, pointer to the name, pointer to the type,
# array width, three extra fields, one ignored.
FIELD_DESC_SIZE = 32


class Section(object):
    """One section of the file - a description from the section table.

    NOTE ON `first16Bit` and `first8Bit`. These are not offsets in the file,
    but boundaries IN THE UNPACKED data. Oodle1 splits the stream into THREE
    parts - 32-, 16- and 8-bit values - because separately they pack better
    than mixed. The decompressor must know these boundaries.
    """

    SIZE_OF = 44

    def __init__(self, data, offset):
        """Reads one 44-byte section entry at `offset`: compression, data
        offset, packed/unpacked size, alignment, the two Oodle1 split points,
        relocation and marshalling tables.
        """
        field_list = struct.unpack_from('<11I', data, offset)
        (self.compression,
         self.data_offset,
         self.packed_size,
         self.unpacked_size,
         self.alignment,
         self.first16,
         self.first8,
         self.relocation_offset,
         self.relocation_count,
         self.marshalling_offset,
         self.marshalling_count) = field_list

    @property
    def compression_name(self):
        """Name of the section's compression (e.g. Oodle1), or "unknown N"."""
        return COMPRESSION_NAME.get(self.compression, 'unknown %d' % self.compression)

    def description(self):
        """One line: compression, packed -> unpacked size, the 16/8 split points and
        the relocation count.
        """
        return ('%-8s packed %8d -> %8d B   16/8 boundaries: %8d %8d   '
                'relocations %5d' %
                (self.compression_name, self.packed_size,
                 self.unpacked_size, self.first16, self.first8,
                 self.relocation_count))


class GrannyFile(object):
    """The header of a `.gr2` file together with the section table."""

    def __init__(self, data):
        """Parses the file header: magic (variant), header format (packed ones are
        refused), version, sizes, root type and root object pointers, and the
        section table; on a problem sets `error` instead of raising.
        """
        self.data = data
        self.error = None

        if len(data) < 32:
            self.error = 'file shorter than the header'
            return

        magic = data[:16]
        self.variant = MAGICS.get(magic)
        if not self.variant:
            self.error = 'unknown magic: ' + magic.hex()
            return

        # After the magic: header size, header format, two reserved fields.
        (self.header_size, self.header_format,
         _r0, _r1) = struct.unpack_from('<4I', data, 16)

        if self.header_format != 0:
            # The header can be packed too. In the corpus it is not even
            # once (400 of 400), but I do not assume that in the code.
            self.error = 'packed header (format %d)' % self.header_format
            return

        # The file header starts RIGHT AFTER the magic and the four fields, i.e.
        # at offset 32. All offsets in the section table
        # are counted FROM EXACTLY THIS PLACE, not from the start of the file.
        self.base = 32
        (self.version, self.file_size, self.crc,
         self.section_offset, self.section_count) = struct.unpack_from(
            '<5I', data, self.base)

        # THE ROOT. Two (section, offset) pairs: the first points to the TYPE
        # DESCRIPTION of the main object, the second to the object itself. All reading
        # starts from this pair - the rest of the file is reachable only through it.
        (type_section, type_offset, object_section, object_offset) = struct.unpack_from(
            '<4I', data, self.base + 20)
        self.root_type = Pointer(type_section, type_offset)
        self.root_object = Pointer(object_section, object_offset)
        self.type_tag = struct.unpack_from('<I', data, self.base + 36)[0]

        self.sections = []
        for i in range(self.section_count):
            o = self.base + self.section_offset + i * Section.SIZE_OF
            if o + Section.SIZE_OF > len(data):
                self.error = 'the section table goes past the end of the file'
                return
            self.sections.append(Section(data, o))

    def raw_section(self, i):
        """The section's bytes AS THEY LIE IN THE FILE - still packed.

        NOTE ON TWO KINDS OF OFFSETS. The offset of the SECTION TABLE is counted
        from `base` (32), but the DATA offset of each section is
        ABSOLUTE - from the start of the file. A mistake gives no error, it just
        reads random bytes as an Oodle1 header and nonsensical alphabet sizes
        come out of them (window 721409, literal alphabet 1).
        """
        s = self.sections[i]
        p = s.data_offset
        return self.data[p:p + s.packed_size]


class Pointer(object):
    """An address inside the UNPACKED file: section number and offset.

    WHY NOT ONE NUMBER. The original loads all sections into
    one memory block and turns relocations into REAL pointers -
    hence `granny_data_type_definition` has `char const *Name` inside.
    We keep the sections separate and a pointer is a pair, because that way
    a mistake by a section comes out as an exception, not as silently read
    neighbouring bytes. The same rule as with `first16Bit`: boundaries are to
    be visible.
    """

    __slots__ = ('section', 'offset')

    def __init__(self, section, offset):
        """A pointer inside the file: section number and offset in it."""
        self.section = section
        self.offset = offset

    def shift_by(self, o):
        """The pointer moved by `o` bytes within the same section."""
        return Pointer(self.section, self.offset + o)

    def __eq__(self, other):
        """Equal when both section and offset are equal."""
        return (isinstance(other, Pointer) and other.section == self.section
                and other.offset == self.offset)

    def __hash__(self):
        """Hash of (section, offset), so pointers can key dicts."""
        return hash((self.section, self.offset))

    def __repr__(self):
        """`section:offset` in hex, e.g. `2:0000a1c0`."""
        return '%d:%08x' % (self.section, self.offset)


class Field(object):
    """One row of a type description - `granny_data_type_definition`."""

    __slots__ = ('kind', 'name', 'type_desc', 'width_value', 'extra')

    def __init__(self, kind, name, type_desc, width_value, extra):
        """One member description of a type: kind, name, sub-type pointer, array
        width and the extra words.
        """
        self.kind = kind
        self.name = name
        self.type_desc = type_desc
        self.width_value = width_value
        self.extra = extra

    @property
    def kind_name(self):
        """Name of the member kind, or "unknown N"."""
        return KIND_NAME.get(self.kind, 'unknown %d' % self.kind)

    @property
    def multiplicity(self):
        """How many elements. Zero and one mean the same: one element."""
        return self.width_value if self.width_value > 1 else 1

    def __repr__(self):
        """`kind name`, with `[width]` for arrays."""
        s = '%s %s' % (self.kind_name, self.name)
        if self.width_value > 1:
            s += '[%d]' % self.width_value
        return s


class Image(object):
    """An unpacked file: sections in memory plus resolved pointers.

    WHAT RELOCATION DOES. In the file pointers are ZEROED - one cannot write
    an address that does not exist yet. Instead each section has a table of
    relocations: triples (offset here, target section, offset
    there). The loader writes the real address at the given place. We do not
    write anything - we build a DICTIONARY, and reading a pointer is a look into
    it. The side effect is valuable: a place that has NO entry is
    certainly a null pointer, so one cannot accidentally follow
    garbage.

    The relocation tables lie in the file UNPACKED, at an
    ABSOLUTE offset (the same as the section data - see `raw_section`).
    """

    def __init__(self, file_path, sections_only=None):
        """Unpacks the sections (raw ones padded, Oodle1 ones decompressed; only
        `sections_only` when given, the others left as None) and builds the
        relocation map (section, offset) -> target pointer.
        """
        self.file_path = file_path
        self.memory = []
        self.relocations = {}
        self._types = {}
        self._sizes = {}

        import oodle1

        for i, s in enumerate(file_path.sections):
            if s.unpacked_size == 0:
                self.memory.append(b'')
                continue
            if sections_only is not None and i not in sections_only:
                # Sections with meshes can be hundreds of kilobytes, and when
                # viewing only the structure of the file they are not needed. Unpacking
                # in Python costs seconds, so I give the option to skip them.
                self.memory.append(None)
                continue
            raw_data = file_path.raw_section(i)
            if s.compression == 0:
                data = raw_data + b'\x00' * (s.unpacked_size - len(raw_data))
            elif s.compression == 2:
                data = oodle1.unpack_section(
                    raw_data, s.first16, s.first8, s.unpacked_size)
            else:
                raise ValueError('section %d: unsupported compression %s'
                                 % (i, s.compression_name))
            self.memory.append(bytes(data))

        for i, s in enumerate(file_path.sections):
            for j in range(s.relocation_count):
                o = s.relocation_offset + j * 12
                source_dir, to_section, to_shift = struct.unpack_from(
                    '<3I', file_path.data, o)
                self.relocations[(i, source_dir)] = Pointer(to_section, to_shift)

    # -- raw reading ---------------------------------------------------------

    def data_bytes(self, ptr, how_many):
        """`how_many` bytes at pointer `ptr`; ValueError when the section was not
        unpacked or the read leaves it.
        """
        data = self.memory[ptr.section]
        if data is None:
            raise ValueError('section %d was not unpacked' % ptr.section)
        end = ptr.offset + how_many
        if end > len(data):
            raise ValueError('a read of %d B at %s goes outside the section (%d B)'
                             % (how_many, ptr, len(data)))
        return data[ptr.offset:end]

    def number(self, ptr, format):
        """One `struct` value of format `format` at pointer `ptr`."""
        return struct.unpack(format, self.data_bytes(ptr, struct.calcsize(format)))[0]

    def pointer(self, ptr):
        """The pointer stored at address `ptr` - or None when null."""
        return self.relocations.get((ptr.section, ptr.offset))

    def text_value(self, ptr):
        """The string pointed to from address `ptr` (zero-terminated)."""
        target = self.pointer(ptr)
        if target is None:
            return None
        data = self.memory[target.section]
        if data is None:
            return None
        end = data.find(b'\x00', target.offset)
        if end < 0:
            end = len(data)
        # Granny keeps names as plain bytes. Bone names are sometimes
        # Korean in cp949, but in the corpus they are ASCII; `latin-1` does not
        # raise on anything, so I never lose data.
        return data[target.offset:end].decode('latin-1')

    # -- type tree -------------------------------------------------------

    def type_desc(self, ptr):
        """The list of fields of the type at address `ptr`.

        A type description is a SEQUENCE of 32-byte rows, ended by a row whose
        kind is zero. There is no field count anywhere - the end is recognized
        by the sentinel, as in a C string.
        """
        key_name = (ptr.section, ptr.offset)
        if key_name in self._types:
            return self._types[key_name]

        field_list = []
        self._types[key_name] = field_list          # entry BEFORE reading: types can
        p = ptr                            # refer to themselves
        while True:
            kind = self.number(p, '<I')
            if kind == END:
                break
            if kind > EMPTY_REFERENCE:
                raise ValueError('unknown field kind %d at %s' % (kind, p))
            name = self.text_value(p.shift_by(4))
            type_desc = self.pointer(p.shift_by(8))
            width_value = self.number(p.shift_by(12), '<i')
            extra = struct.unpack('<3i', self.data_bytes(p.shift_by(16), 12))
            field_list.append(Field(kind, name, type_desc, width_value, extra))
            p = p.shift_by(FIELD_DESC_SIZE)
        return field_list

    def type_size(self, ptr, in_build=None):
        """How many bytes an object described by the type at `ptr` takes.

        It sums the field sizes WITHOUT ANY ALIGNMENT. That is not an oversight:
        Granny's exporter arranges the fields so that alignment comes out by itself
        (`Int8` go in fours, `Real16` in twos), and the size computed as a sum
        agrees with `GrannyTypeSizeCheck` from the header - those assertions in
        `granny.h` check exactly the sum of the member sizes.
        """
        key_name = (ptr.section, ptr.offset)
        if key_name in self._sizes:
            return self._sizes[key_name]
        if in_build is None:
            in_build = set()
        if key_name in in_build:
            raise ValueError('type %s contains itself' % ptr)
        in_build.add(key_name)

        total_sum = 0
        for field in self.type_desc(ptr):
            total_sum += self.field_size(field, in_build)

        in_build.discard(key_name)
        self._sizes[key_name] = total_sum
        return total_sum

    def field_size(self, field, in_build=None):
        """Size in bytes of a member: its type's size for inline members, the
        kind's fixed size otherwise, times the array width.
        """
        if field.kind == INLINE:
            if field.type_desc is None:
                raise ValueError('inline field without a type: %r' % field)
            one_step = self.type_size(field.type_desc, in_build)
        else:
            one_step = KIND_SIZE[field.kind]
        return one_step * field.multiplicity

    # -- reading objects --------------------------------------------------

    def obj(self, type_ptr, obj_ptr, depth=3, array_limit=8):
        """The object at `obj_ptr` read according to the type description at `type_ptr`.

        The result is a plain Python dictionary: field name -> value. Compound
        values (references, arrays) descend as long as `depth`
        allows; below that I insert an abbreviation, so that a dump of the whole model can
        be read by eye.
        """
        result = {}
        p = obj_ptr
        for field in self.type_desc(type_ptr):
            try:
                result[field.name] = self.value_of(
                    field, p, depth, array_limit)
            except Exception as e:
                result[field.name] = '<ERROR: %s>' % e
            p = p.shift_by(self.field_size(field))
        return result

    def value_of(self, field, p, depth, array_limit):
        """The value of one member at `p` as plain Python: inline objects and
        references followed up to `depth` levels, arrays cut at
        `array_limit`, strings, transforms and numbers decoded; a placeholder
        string where the depth runs out.
        """
        r = field.kind

        if r == INLINE:
            if depth <= 0:
                return '<inline %s>' % (field.type_desc,)
            size = self.type_size(field.type_desc)
            if field.multiplicity > 1:
                return [self.obj(field.type_desc, p.shift_by(i * size),
                                    depth - 1, array_limit)
                        for i in range(min(field.multiplicity, array_limit))]
            return self.obj(field.type_desc, p, depth - 1, array_limit)

        if r in (REFERENCE, EMPTY_REFERENCE):
            target = self.pointer(p)
            if target is None:
                return None
            if depth <= 0 or field.type_desc is None:
                return '-> %s' % (target,)
            return self.obj(field.type_desc, target, depth - 1, array_limit)

        if r == REFERENCE_TO_ARRAY:
            how_many = self.number(p, '<i')
            target = self.pointer(p.shift_by(4))
            return self.inline_array(field.type_desc, target, how_many,
                                        depth, array_limit)

        if r == ARRAY_OF_REFERENCES:
            how_many = self.number(p, '<i')
            target = self.pointer(p.shift_by(4))
            return self.reference_array(field.type_desc, target, how_many,
                                        depth, array_limit)

        if r == VARIANT_REFERENCE:
            type_desc = self.pointer(p)
            target = self.pointer(p.shift_by(4))
            if type_desc is None or target is None:
                return None
            if depth <= 0:
                return '<variant %s -> %s>' % (type_desc, target)
            return self.obj(type_desc, target, depth - 1, array_limit)

        if r == REFERENCE_TO_VARIANT_ARRAY:
            type_desc = self.pointer(p)
            how_many = self.number(p.shift_by(4), '<i')
            target = self.pointer(p.shift_by(8))
            return self.inline_array(type_desc, target, how_many, depth, array_limit)

        if r == STRING:
            return self.text_value(p)

        if r == TRANSFORM:
            raw_bytes = struct.unpack('<I16f', self.data_bytes(p, 68))
            return {'flags': raw_bytes[0], 'position': raw_bytes[1:4],
                    'orientation': raw_bytes[4:8], 'scale': raw_bytes[8:17]}

        # From here on just numbers. A multiplicity greater than one means a fixed-length
        # array WRITTEN IN PLACE (that is how `Extra[3]`
        # or a 3x3 matrix as nine `Real32` are stored).
        format = {
            REAL32: 'f', INT8: 'b', UINT8: 'B', INT8_BINORMAL: 'b',
            UINT8_NORMAL: 'B', INT16: 'h', UINT16: 'H',
            INT16_BINORMAL: 'h', UINT16_NORMAL: 'H',
            INT32: 'i', UINT32: 'I', REAL16: 'H',
        }.get(r)
        if format is None:
            return '<kind %s>' % field.kind_name
        n = field.multiplicity
        val = struct.unpack('<%d%s' % (n, format),
                             self.data_bytes(p, n * struct.calcsize(format)))
        return val[0] if n == 1 else list(val)

    def inline_array(self, type_desc, target, how_many, depth, array_limit):
        """`how_many` inline objects of type `type_desc` from `target` (at most `array_limit`,
        then a "... N more" marker); a placeholder at depth 0.
        """
        if target is None or how_many <= 0:
            return []
        if depth <= 0 or type_desc is None:
            return '<%d elements at %s>' % (how_many, target)
        size = self.type_size(type_desc)
        result = [self.obj(type_desc, target.shift_by(i * size),
                             depth - 1, array_limit)
                 for i in range(min(how_many, array_limit))]
        if how_many > array_limit:
            result.append('<... %d more>' % (how_many - array_limit))
        return result

    def reference_array(self, type_desc, target, how_many, depth, array_limit):
        """`how_many` references (4-byte pointers) from `target`, each followed to an object
        of type `type_desc` (at most `array_limit`); a placeholder at depth 0.
        """
        if target is None or how_many <= 0:
            return []
        if depth <= 0 or type_desc is None:
            return '<%d references at %s>' % (how_many, target)
        result = []
        for i in range(min(how_many, array_limit)):
            elevation = self.pointer(target.shift_by(i * 4))
            result.append(None if elevation is None
                         else self.obj(type_desc, elevation, depth - 1, array_limit))
        if how_many > array_limit:
            result.append('<... %d more>' % (how_many - array_limit))
        return result


class Node(object):
    """A pair (type description, object address) - one object in the file.

    `Image.obj` reads the WHOLE subtree at once and gives a dictionary; that is good
    for viewing, but bad for work, because a model has tens of thousands of
    vertices. A node reads LAZILY: only the field that is asked for.
    All the further steps (skeleton, meshes, poses) rest on this.
    """

    def __init__(self, image, type_desc, ptr):
        """A typed object in the unpacked file: type pointer and object pointer."""
        self.image = image
        self.type_desc = type_desc
        self.ptr = ptr

    def field_list(self):
        """The member list of the node's type."""
        return self.image.type_desc(self.type_desc)

    def where(self, name):
        """(field description, field address) - or an exception when there is no such field."""
        p = self.ptr
        for field in self.field_list():
            if field.name == name:
                return field, p
            p = p.shift_by(self.image.field_size(field))
        raise KeyError('%s has no field %r (it has: %s)'
                       % (self.type_desc, name,
                          ', '.join(str(x.name) for x in self.field_list())))

    def __contains__(self, name):
        """True when the node's type has a member called `name`."""
        return any(x.name == name for x in self.field_list())

    def __getitem__(self, name):
        """The value of a field. Objects and arrays of objects come out as nodes."""
        field, p = self.where(name)
        o = self.image
        r = field.kind

        if r == INLINE:
            size = o.type_size(field.type_desc)
            if field.multiplicity > 1:
                return [Node(o, field.type_desc, p.shift_by(i * size))
                        for i in range(field.multiplicity)]
            return Node(o, field.type_desc, p)

        if r in (REFERENCE, EMPTY_REFERENCE):
            target = o.pointer(p)
            return None if target is None else Node(o, field.type_desc, target)

        if r == REFERENCE_TO_ARRAY:
            how_many = o.number(p, '<i')
            target = o.pointer(p.shift_by(4))
            if target is None or how_many <= 0:
                return []
            size = o.type_size(field.type_desc)
            return [Node(o, field.type_desc, target.shift_by(i * size))
                    for i in range(how_many)]

        if r == ARRAY_OF_REFERENCES:
            how_many = o.number(p, '<i')
            target = o.pointer(p.shift_by(4))
            if target is None or how_many <= 0:
                return []
            result = []
            for i in range(how_many):
                elevation = o.pointer(target.shift_by(i * 4))
                result.append(None if elevation is None else Node(o, field.type_desc, elevation))
            return result

        if r == VARIANT_REFERENCE:
            type_desc = o.pointer(p)
            target = o.pointer(p.shift_by(4))
            if type_desc is None or target is None:
                return None
            return Node(o, type_desc, target)

        if r == REFERENCE_TO_VARIANT_ARRAY:
            # HERE SITS GRANNY'S WHOLE IDEA FOR VERTICES. The array does not
            # have a type in advance - the type is STORED NEXT TO IT. Thanks to that the
            # same reader will handle a vertex with bone weights and without them,
            # with one set of texture coordinates and with three.
            type_desc = o.pointer(p)
            how_many = o.number(p.shift_by(4), '<i')
            target = o.pointer(p.shift_by(8))
            if type_desc is None or target is None or how_many <= 0:
                return VariantArray(o, None, None, 0)
            return VariantArray(o, type_desc, target, how_many)

        return o.value_of(field, p, 0, 8)

    def lookup(self, depth=2, limit=8):
        """The node as a dict (see `Image.obj`), `depth` levels deep, arrays
        cut at `limit`.
        """
        return self.image.obj(self.type_desc, self.ptr, depth, limit)

    def __repr__(self):
        """`Node(type @ pointer)`."""
        return 'Node(%s @ %s)' % (self.type_desc, self.ptr)


class VariantArray(object):
    """An array of objects whose type is stored together with it."""

    def __init__(self, image, type_desc, ptr, how_many):
        """An array of `how_many` elements of type `type_desc` starting at `ptr` (a variant
        array: the type travels with the data).
        """
        self.image = image
        self.type_desc = type_desc
        self.ptr = ptr
        self.how_many = how_many
        self.size = image.type_size(type_desc) if type_desc is not None else 0

    def __len__(self):
        """Number of elements."""
        return self.how_many

    def __getitem__(self, i):
        """Element `i` as a `Node` (negative indices count from the end);
        IndexError out of range.
        """
        if i < 0:
            i += self.how_many
        if not 0 <= i < self.how_many:
            raise IndexError(i)
        return Node(self.image, self.type_desc, self.ptr.shift_by(i * self.size))

    def field_list(self):
        """The members of the element type ([] without a type)."""
        return [] if self.type_desc is None else self.image.type_desc(self.type_desc)

    def __repr__(self):
        """`VariantArray(count x size B, type_desc ...)`."""
        return ('VariantArray(%d x %d B, type %s)'
                % (self.how_many, self.size, self.type_desc))


def root_node(image):
    """The root object of the file as a `Node`."""
    return Node(image, image.file_path.root_type, image.file_path.root_object)


def walk(node, path_str):
    """Navigates the tree by a path string: `Meshes/0/PrimaryVertexData`."""
    current = node
    for step in path_str.split('/'):
        if not step:
            continue
        if step.lstrip('-').isdigit():
            current = current[int(step)]
        else:
            current = current[step]
        if current is None:
            raise ValueError('empty at step %r' % step)
    return current


# ---------------------------------------------------------------------------
# PRINTING
# ---------------------------------------------------------------------------

def print_tree(value_of, indent=0, file_path=sys.stdout):
    """Prints a nested dict/list value as an indented tree to `file_path`."""
    pre = '  ' * indent
    if isinstance(value_of, dict):
        for k, v in value_of.items():
            if isinstance(v, (dict, list)) and v:
                file_path.write('%s%s:\n' % (pre, k))
                print_tree(v, indent + 1, file_path)
            else:
                file_path.write('%s%s: %s\n' % (pre, k, digest(v)))
    elif isinstance(value_of, list):
        for i, v in enumerate(value_of):
            if isinstance(v, (dict, list)) and v:
                file_path.write('%s[%d]\n' % (pre, i))
                print_tree(v, indent + 1, file_path)
            else:
                file_path.write('%s[%d] %s\n' % (pre, i, digest(v)))
    else:
        file_path.write('%s%s\n' % (pre, digest(value_of)))


def digest(v):
    """A short printable form of a leaf value (floats with %g)."""
    if isinstance(v, float):
        return '%g' % v
    if isinstance(v, list) and v and isinstance(v[0], float):
        return '[' + ', '.join('%g' % x for x in v) + ']'
    if isinstance(v, list) and not v:
        return '[]'
    if isinstance(v, dict) and not v:
        return '{}'
    return repr(v) if isinstance(v, str) else str(v)


def load_from_corpus(name):
    """Extracts a file from the streamed corpus (`build/port/corpus`, tools/webfs.py)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import webfs
    return webfs.Corpus().read_file(name)


def load_file(name):
    """The bytes of `name`: a file on disk if it exists, else the corpus entry."""
    if os.path.exists(name):
        return io.open(name, 'rb').read()
    return load_from_corpus(name)


def open_file(name):
    """(GrannyFile, Image) of `name`; ValueError when the header cannot be parsed."""
    data = load_file(name)
    p = GrannyFile(data)
    if p.error:
        raise ValueError(p.error)
    return p, Image(p)


# ---------------------------------------------------------------------------
# COMMAND LINE
# ---------------------------------------------------------------------------

def cmd_header(name):
    """`gr2.py NAME`: prints the header and the section table; 1 on a bad file."""
    data = load_file(name)
    p = GrannyFile(data)
    if p.error:
        print('ERROR: ' + p.error)
        return 1
    print('%s' % name)
    print('  variant       : %s' % p.variant)
    print('  format version: %d' % p.version)
    print('  file size     : %d (really %d)' % (p.file_size, len(data)))
    print('  sections      : %d' % p.section_count)
    print('  root type     : %s' % p.root_type)
    print('  root object   : %s' % p.root_object)
    for i, s in enumerate(p.sections):
        print('  [%d] %s' % (i, s.description()))
    return 0


def cmd_type(name):
    """`--type NAME`: prints the root type's members with offsets, sizes and
    sub-type member names.
    """
    p, o = open_file(name)
    print('%s - root type at %s' % (name, p.root_type))
    field_list = o.type_desc(p.root_type)
    print('  object size: %d B, fields: %d'
          % (o.type_size(p.root_type), len(field_list)))
    shift = 0
    for field in field_list:
        size = o.field_size(field)
        subtype = ''
        if field.type_desc is not None:
            try:
                subtype = '  {%s}' % ', '.join(
                    x.name or '?' for x in o.type_desc(field.type_desc))
            except Exception as e:
                subtype = '  <ERROR %s>' % e
        print('  +%-5d %-16s %-28s %3d B%s'
              % (shift, field.kind_name,
                 (field.name or '?') + ('[%d]' % field.width_value
                                        if field.width_value > 1 else ''),
                 size, subtype[:120]))
        shift += size
    return 0


def cmd_tree(name, depth, limit):
    """`--tree NAME`: prints the root object as a tree."""
    p, o = open_file(name)
    print('%s' % name)
    print_tree(o.obj(p.root_type, p.root_object, depth, limit))
    return 0


def cmd_path(name, path_str, depth, limit):
    """`--path NAME PATH`: prints the node, variant array or list reached by
    PATH (e.g. Meshes/0/PrimaryVertexData).
    """
    p, o = open_file(name)
    w = walk(root_node(o), path_str)
    print('%s : %s' % (name, path_str))
    if isinstance(w, Node):
        print('  %r' % w)
        print_tree(w.lookup(depth, limit), 1)
    elif isinstance(w, VariantArray):
        print('  %r' % w)
        for field in w.field_list():
            print('    field %-16s %s' % (field.kind_name, field))
        for i in range(min(len(w), limit)):
            print('  [%d]' % i)
            print_tree(w[i].lookup(depth, limit), 2)
    elif isinstance(w, list):
        print('  %d elements' % len(w))
        for i, elevation in enumerate(w[:limit]):
            print('  [%d]' % i)
            if isinstance(elevation, Node):
                print_tree(elevation.lookup(depth, limit), 2)
            else:
                print_tree(elevation, 2)
    else:
        print_tree(w, 1)
    return 0


def layout_signature(array):
    """The layout of one vertex as a tuple - for comparing files.

    This is the SHOPPING LIST for the C++ reader: how many DIFFERENT vertex
    layouts it has to be able to take apart to handle all the models.
    I do not guess it - I count it across the whole corpus.
    """
    return tuple((p.kind_name, p.name, p.multiplicity) for p in array.field_list())


def verify_file(name):
    """Walks the WHOLE file and collects numbers. Raises an exception on a slip."""
    p, o = open_file(name)
    k = root_node(o)
    st = {'bones': 0, 'meshes': 0, 'vertices': 0, 'indices': 0,
          'materials': 0, 'animations': 0, 'tracks': 0, 'layouts': set()}

    for texture in k['Textures']:
        texture['FromFileName']
    for material in k['Materials']:
        material['Name']
        st['materials'] += 1
    for skeleton in k['Skeletons']:
        for bone in skeleton['Bones']:
            bone['Name']
            bone['ParentIndex']
            st['bones'] += 1
    for mesh in k['Meshes']:
        st['meshes'] += 1
        mesh['Name']
        wd = mesh['PrimaryVertexData']
        if wd is not None:
            w = wd['Vertices']
            st['layouts'].add(layout_signature(w))
            st['vertices'] += len(w)
            if len(w):
                # FIRST AND LAST. If the vertex size were computed
                # wrong, the last one would go outside the section - and that is an exception,
                # not silent garbage.
                w[0].lookup(1)
                w[len(w) - 1].lookup(1)
        top = mesh['PrimaryTopology']
        if top is not None:
            i32 = top['Indices']
            i16 = top['Indices16']
            st['indices'] += len(i32) + len(i16)
            for group in top['Groups']:
                group['MaterialIndex']
        for binding in mesh['BoneBindings']:
            binding['BoneName']
    for model in k['Models']:
        model['Name']
        for binding in model['MeshBindings']:
            binding['Mesh']
    for group in k['TrackGroups']:
        group['Name']
        st['tracks'] += len(group['TransformTracks'])
        for path_str in group['TransformTracks']:
            path_str['Name']
    for animation in k['Animations']:
        animation['Name']
        animation['Duration']
        st['animations'] += 1
    return st


def cmd_check(how_many, pattern=None):
    """`--check [N] [PATTERN]`: walks the whole tree of N random corpus .gr2
    files (seed 1), counts objects and vertex layouts, lists every failure;
    1 when any file failed.
    """
    import collections
    import random
    import traceback
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import webfs
    k = webfs.Corpus()
    names = [n for n in k.file_list if n.lower().endswith('.gr2')]
    if pattern:
        names = [n for n in names if pattern.lower() in n.lower()]
    random.seed(1)
    random.shuffle(names)
    names = names[:how_many]

    total_sum = collections.Counter()
    layouts = collections.Counter()
    errors_found = []
    for n in names:
        try:
            st = verify_file(n)
        except Exception as e:
            errors_found.append((n, '%s: %s' % (type(e).__name__, e)))
            if len(errors_found) <= 3:
                traceback.print_exc()
            continue
        for u in st.pop('layouts'):
            layouts[u] += 1
        total_sum.update(st)
        total_sum['files'] += 1

    print('files checked: %d of %d' % (total_sum['files'], len(names)))
    for key_name in ('bones', 'meshes', 'vertices', 'indices',
                  'materials', 'animations', 'tracks'):
        print('  %-14s %d' % (key_name, total_sum[key_name]))
    print('vertex layouts: %d' % len(layouts))
    for u, n in layouts.most_common():
        print('  %4d x  %s' % (n, ' '.join(
            '%s%s:%s' % (nz, '[%d]' % curve if curve > 1 else '', read_data)
            for read_data, nz, curve in u)))
    if errors_found:
        print('ERRORS: %d' % len(errors_found))
        for n, e in errors_found[:20]:
            print('  %s -> %s' % (n, e))
    return 1 if errors_found else 0


def main():
    """Command line: dispatches to the `cmd_*` commands; prints the usage
    without arguments. The Polish flag names from earlier still work.
    """
    if len(sys.argv) < 2:
        print('usage:')
        print('  gr2.py <name>                      - header and sections')
        print('  gr2.py --type <name>               - root type')
        print('  gr2.py --tree <name> [depth] [lim] - root object')
        print('  gr2.py --path <name> <path> [depth] [lim]')
        print('        e.g. --path model.gr2 Meshes/0/PrimaryVertexData')
        print("  gr2.py --check [count] [pattern]   - walk of the whole tree")
        print("  gr2.py --survey [count]            - survey of N files")
        return 1

    if sys.argv[1] == '--check':
        how_many = int(sys.argv[2]) if len(sys.argv) > 2 else 50
        pattern = sys.argv[3] if len(sys.argv) > 3 else None
        return cmd_check(how_many, pattern)

    if sys.argv[1] == '--path':
        depth_left = int(sys.argv[4]) if len(sys.argv) > 4 else 2
        lim = int(sys.argv[5]) if len(sys.argv) > 5 else 8
        return cmd_path(sys.argv[2], sys.argv[3], depth_left, lim)

    if sys.argv[1] == '--survey':
        how_many = int(sys.argv[2]) if len(sys.argv) > 2 else 200
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import webfs
        import collections
        k = webfs.Corpus()
        names = [n for n in k.file_list if n.lower().endswith('.gr2')][:how_many]

        variants = collections.Counter()
        versions = collections.Counter()
        compressions = collections.Counter()
        section_count = collections.Counter()
        errors_found = collections.Counter()
        for n in names:
            p = GrannyFile(k.read_file(n))
            if p.error:
                errors_found[p.error] += 1
                continue
            variants[p.variant] += 1
            versions[p.version] += 1
            section_count[p.section_count] += 1
            for s in p.sections:
                compressions[s.compression_name] += 1

        print('examined   : %d' % len(names))
        print('variants   : %s' % dict(variants))
        print('versions   : %s' % dict(versions))
        print('sections   : %s' % dict(section_count))
        print('compression: %s' % dict(compressions))
        if errors_found:
            print('ERRORS     : %s' % dict(errors_found))
        return 0

    if sys.argv[1] == '--type':
        return cmd_type(sys.argv[2])

    if sys.argv[1] == '--tree':
        depth_left = int(sys.argv[3]) if len(sys.argv) > 3 else 3
        lim = int(sys.argv[4]) if len(sys.argv) > 4 else 8
        return cmd_tree(sys.argv[2], depth_left, lim)

    return cmd_header(sys.argv[1])


if __name__ == '__main__':
    sys.exit(main())
