// SPDX-License-Identifier: GPL-2.0-or-later
// codepages.h - conversion between the Windows code pages the client uses
// (1250, 1252, 874) and Unicode. platform_codec.cpp uses the per-character
// functions; the whole-string ones have no caller in the client.

// Design:
// The code page numbers are the ones Windows uses, and that is not our
// choice: the client writes them to its settings files and sends them in
// the protocol, so they are imposed from outside.
//
// Bytes a page does not assign map to U+FFFD (the replacement character),
// never to a made-up value, and U+FFFD itself never maps back to a byte -
// the sentinel is not a character (it used to map to 0x81 in
// all three pages, and a byte going through decode-encode silently changed
// its value; ime_web.cpp does exactly such round trips).
//
// The CP1250 table is NOT typed from memory: the first version had seven
// wrong entries and passed every test because it was internally consistent
// Values were taken byte by byte from
// CPython's `cp1250` codec; all three tables were re-audited against the
// unicode.org mappings.

#ifndef M2W_CODEPAGES_H
#define M2W_CODEPAGES_H

#include <cstdint>

/// Windows code page numbers understood by this layer.
enum EM2wCodePage
{
    M2W_CP_874  = 874,    ///< Thai
    M2W_CP_1250 = 1250,   ///< Central European - Polish, Czech, Hungarian
    M2W_CP_1252 = 1252,   ///< Western European - German, French
};

/// Whether this code page has a table here.
bool M2W_CodePageKnown(int iCodePage);

/// One code-page byte -> one Unicode character.
///
/// Returns `0xFFFD` (the replacement character) for bytes the page does not
/// assign. Such bytes exist in each of the three pages and must not be
/// pretended to mean something. Unknown code page: also `0xFFFD`.
char32_t M2W_ByteToChar(int iCodePage, unsigned char uByte);

/// Unicode character -> one code-page byte.
///
/// Returns `false` when the page has no such character and then leaves
/// `puByte` untouched. That is an ordinary situation: page 1252 has no
/// Polish a with ogonek (U+0105). `0xFFFD` is never found (design note above).
bool M2W_CharToByte(int iCodePage, char32_t cChar, unsigned char* puByte);

// ===========================================================================
// Whole strings - Windows behaviour
// ===========================================================================
// No caller in the client today: platform_codec.cpp implements the string
// logic of MultiByteToWideChar / WideCharToMultiByte itself on the
// per-character functions above, and the only user of these two is the
// offline test in zlecenia_gemini. Kept because they are
// correct and audited. The signatures are simplified (plain C++ types
// instead of Windows ones) but the BEHAVIOUR is the Windows one:
//
//   * `iBytes` / `iUnits` negative -> the string is zero-terminated and the
//                                    zero BELONGS to it, so it is converted too;
//   * `iCapacity` zero             -> write nothing, return the size NEEDED
//                                    (this is how the client asks for a length);
//   * return value zero            -> failure.

/// Code page -> UTF-16. Returns the number of 16-bit units written (or
/// needed), zero on failure.
///
/// Failure means: unknown page, null pointer, buffer too small. An
/// unassigned byte is NOT a failure - it becomes `0xFFFD`.
int M2W_ToUtf16(int iCodePage, const char* c_szInput, int iBytes,
                char16_t* pOutput, int iCapacity);

/// UTF-16 -> code page. Returns the number of bytes written (or needed),
/// zero on failure.
///
/// A character the page does not have becomes a question mark (`'?'`) and
/// the conversion goes on - as Windows did. That is NOT a failure: a string
/// in another language should come out readable where it can.
///
/// Surrogate pairs (characters outside the basic plane) also become a
/// question mark - ONE per pair, not two.
int M2W_FromUtf16(int iCodePage, const char16_t* c_pInput, int iUnits,
                  char* pOutput, int iCapacity);

#endif  // M2W_CODEPAGES_H
