// SPDX-License-Identifier: GPL-2.0-or-later
// gr2_oodle1.h - decompression of `.gr2` sections with the Oodle1 codec:
// RAD Game Tools' own codec from the Granny 2 era - NOT today's "Oodle" -
// found in no system library and impossible to bypass (seven of eight
// sections of every `.gr2` in the corpus use it, measured on 400 files).

// Design:
// ORIGIN OF THE DESCRIPTION. Written from the open `liboodle` specification
// (https://github.com/LunaticInAHat/liboodle, Unlicense = public domain),
// first in Python (`tools/oodle1.py`) and only then transcribed here. The
// reason for that order: with an unknown format what counts is attempts
// per hour, and an attempt in Python takes seconds instead of a
// five-minute build. The Python version stays as the MEASURE - this file's
// output has to agree with it byte for byte.
//
// THE LIMIT OF KNOWLEDGE, written down honestly. On 400 random corpus
// files FOUR fall apart in the middle of one section. A reference
// implementation run on the same section gives byte for byte the same
// output, with the same overrun - so this code is FAITHFUL to the
// description and the description itself is lacking. Until that is
// explained `DecompressSection` prefers to REFUSE rather than return
// damaged data - see the boundary check.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace m2wgr2 {

/// Decompresses one `.gr2` section.
///
/// @param c_pbyPacked      the section's bytes as they lie in the file
///                         (with the three 12-byte headers at the start)
/// @param uPacked          how many of them
/// @param uFirst16         boundary of stream 0 (from the section table)
/// @param uFirst8          boundary of stream 1
/// @param pbyOutput        buffer for `uUnpacked` bytes
/// @return false when the section cannot be decompressed per the description.
bool DecompressSection(const uint8_t* c_pbyPacked, uint32_t uPacked,
                       uint32_t uFirst16, uint32_t uFirst8,
                       uint8_t* pbyOutput, uint32_t uUnpacked);

/// The reason of the last refusal - for the log. Never NULL.
const char* LastError();

}  // namespace m2wgr2
