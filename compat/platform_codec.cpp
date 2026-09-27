// SPDX-License-Identifier: GPL-2.0-or-later
// platform_codec.cpp - `MultiByteToWideChar` / `WideCharToMultiByte` for
// UTF-8 and the single-byte pages CP1250 / CP1252 / CP874, with the Windows
// contract the client relies on (negative length = zero-terminated including
// the zero; capacity 0 = return the size needed; 0 = failure).

// Design:
// `EterLocale/StringCodec.cpp` converts game strings between bytes and
// `wchar_t` through `MultiByteToWideChar` with a CODE PAGE number - and the
// code page is the whole problem. Metin2 sends text in code pages: Korean
// in CP949, European in CP1250/1252, Thai in CP874 (`EterLocale/
// CodePageId.h` lists them; the client picks one by the server language).
//
// Supported: UTF-8, CP1250, CP1252, CP874. The tables are DATA in
// codepages.cpp - no `<locale>`, no dependence on the machine's settings,
// the same result everywhere. `CP_ACP` ("the system default page") is
// treated as UTF-8, because that is the browser's default - platform
// conformance, not a shortcut.
//
// Not supported: CP949 (Korean, double-byte, about 17 000 mappings) - a
// separate job of another size and risk; for it this layer returns 0, an
// honest "not converted".
//
// The tables are checked the hard way because the first CP1250 delivered
// had SEVEN wrong mappings and passed 48 green checks: a round trip over all
// 256 bytes proves a table is CONSISTENT, not that it is TRUE (found in an
// audit). Today's values come from
// CPython's codecs queried byte by byte, and the test carries all 768
// mappings as data.
//
// Code pages could also be converted THROUGH PYTHON's codecs, but these
// tables are needed either way - the Win32 layer has to work before Python
// starts.

#include "win32_compat.h"
#include "codepages.h"

#include <cstring>

namespace
{

/// One character from UTF-8. Returns the number of bytes consumed, 0 on a
/// malformed lead byte or a truncated sequence.
int FromUtf8(const unsigned char* p, int iAvailable, unsigned int* puChar)
{
    if (iAvailable <= 0) return 0;
    if (p[0] < 0x80) { *puChar = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && iAvailable >= 2)
    {
        *puChar = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && iAvailable >= 3)
    {
        *puChar = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && iAvailable >= 4)
    {
        *puChar = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12)
                | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        return 4;
    }
    return 0;
}

/// Whether this code page can be converted at all.
bool Supported(UINT uCodePage)
{
    return uCodePage == CP_UTF8 || uCodePage == CP_ACP ||
           M2W_CodePageKnown(static_cast<int>(uCodePage));
}

/// Whether this page is SINGLE-BYTE (CP1250/1252/874) - a table lookup
/// rather than UTF-8 decoding.
bool SingleByte(UINT uCodePage)
{
    return uCodePage != CP_UTF8 && uCodePage != CP_ACP &&
           M2W_CodePageKnown(static_cast<int>(uCodePage));
}

}  // namespace

/// Win32: bytes in `uCodePage` -> UTF-16. Returns units written, or the
/// units needed when `iCapacity` is 0; 0 = unsupported page, malformed
/// UTF-8 or buffer too small.
int MultiByteToWideChar(UINT uCodePage, DWORD /*dwFlags*/, LPCSTR c_szInput, int iBytes,
                        WCHAR* pOutput, int iCapacity)
{
    if (!c_szInput) return 0;
    if (!Supported(uCodePage)) return 0;   // see the design note

    const int iLength = (iBytes < 0)
                        ? static_cast<int>(std::strlen(c_szInput)) + 1
                        : iBytes;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(c_szInput);

    // --- single-byte page ---------------------------------------------------
    // One byte is exactly one character, always in the basic plane - no
    // surrogate pairs and no length pass: characters needed = bytes.
    if (SingleByte(uCodePage))
    {
        if (iCapacity > 0 && iLength > iCapacity)
            return 0;

        if (iCapacity > 0)
            for (int i = 0; i < iLength; ++i)
                pOutput[i] = static_cast<WCHAR>(
                    M2W_ByteToChar(static_cast<int>(uCodePage), p[i]));

        // An unassigned byte becomes the replacement character (0xFFFD)
        // and that is NOT an error - Windows behaves the same with
        // `MB_ERR_INVALID_CHARS` off, the mode Metin2 calls it in.
        return iLength;
    }

    // --- UTF-8 --------------------------------------------------------------
    int iWritten = 0;
    int i = 0;
    while (i < iLength)
    {
        unsigned int uChar = 0;
        const int iConsumed = FromUtf8(p + i, iLength - i, &uChar);
        if (iConsumed == 0) return 0;      // malformed UTF-8: say it cannot be done
        i += iConsumed;

        // Outside the basic plane a surrogate pair is needed - `wchar_t` is
        // 16 bits here, as on Windows.
        const int iNeeded = (uChar > 0xFFFFu) ? 2 : 1;
        if (iCapacity > 0)
        {
            if (iWritten + iNeeded > iCapacity) return 0;
            if (iNeeded == 1)
            {
                pOutput[iWritten] = static_cast<WCHAR>(uChar);
            }
            else
            {
                const unsigned int uRest = uChar - 0x10000u;
                pOutput[iWritten]     = static_cast<WCHAR>(0xD800u + (uRest >> 10));
                pOutput[iWritten + 1] = static_cast<WCHAR>(0xDC00u + (uRest & 0x3FFu));
            }
        }
        iWritten += iNeeded;
    }
    // With `iCapacity == 0` Windows returns the size NEEDED, not an error -
    // TMP4 asks for buffer lengths that way.
    return iWritten;
}

/// Win32: UTF-16 -> bytes in `uCodePage`. A character the page lacks
/// becomes `?` and sets `*pbUsedDefault`. Returns bytes written, or the
/// bytes needed when `iCapacity` is 0; 0 = unsupported page or buffer too
/// small.
int WideCharToMultiByte(UINT uCodePage, DWORD /*dwFlags*/, const WCHAR* c_pInput, int iUnits,
                        LPSTR pOutput, int iCapacity, LPCSTR /*c_szDefault*/, BOOL* pbUsedDefault)
{
    if (pbUsedDefault) *pbUsedDefault = FALSE;
    if (!c_pInput) return 0;
    if (!Supported(uCodePage)) return 0;

    const int iLength = (iUnits < 0) ? [&]{
        int n = 0; while (c_pInput[n]) ++n; return n + 1;
    }() : iUnits;

    int iWritten = 0;

    // --- single-byte page ---------------------------------------------------
    if (SingleByte(uCodePage))
    {
        for (int i = 0; i < iLength; ++i)
        {
            unsigned int uChar = static_cast<unsigned int>(c_pInput[i]);

            // A surrogate pair is ONE character on two units. Counting it as
            // two would give two question marks and shift the rest of the
            // string.
            if (uChar >= 0xD800u && uChar <= 0xDBFFu && i + 1 < iLength)
            {
                const unsigned int uSecond = static_cast<unsigned int>(c_pInput[i + 1]);
                if (uSecond >= 0xDC00u && uSecond <= 0xDFFFu)
                {
                    uChar = 0x10000u + ((uChar - 0xD800u) << 10) + (uSecond - 0xDC00u);
                    ++i;
                }
            }

            unsigned char uByte = 0;
            if (!M2W_CharToByte(static_cast<int>(uCodePage),
                                static_cast<char32_t>(uChar), &uByte))
            {
                // The page has no such character. Windows inserts the
                // default character and goes on - stopping would be worse,
                // one exotic character wiping out a whole sentence.
                uByte = '?';
                if (pbUsedDefault)
                    *pbUsedDefault = TRUE;
            }

            if (iCapacity > 0)
            {
                if (iWritten + 1 > iCapacity)
                    return 0;
                pOutput[iWritten] = static_cast<char>(uByte);
            }
            ++iWritten;
        }
        return iWritten;
    }

    // --- UTF-8 --------------------------------------------------------------
    for (int i = 0; i < iLength; ++i)
    {
        unsigned int uChar = static_cast<unsigned int>(c_pInput[i]);

        // Surrogate pair -> one character.
        if (uChar >= 0xD800u && uChar <= 0xDBFFu && i + 1 < iLength)
        {
            const unsigned int uSecond = static_cast<unsigned int>(c_pInput[i + 1]);
            if (uSecond >= 0xDC00u && uSecond <= 0xDFFFu)
            {
                uChar = 0x10000u + ((uChar - 0xD800u) << 10) + (uSecond - 0xDC00u);
                ++i;
            }
        }

        char szBuffer[4];
        int iCount;
        if (uChar < 0x80u)
        {
            szBuffer[0] = static_cast<char>(uChar); iCount = 1;
        }
        else if (uChar < 0x800u)
        {
            szBuffer[0] = static_cast<char>(0xC0u | (uChar >> 6));
            szBuffer[1] = static_cast<char>(0x80u | (uChar & 0x3Fu)); iCount = 2;
        }
        else if (uChar < 0x10000u)
        {
            szBuffer[0] = static_cast<char>(0xE0u | (uChar >> 12));
            szBuffer[1] = static_cast<char>(0x80u | ((uChar >> 6) & 0x3Fu));
            szBuffer[2] = static_cast<char>(0x80u | (uChar & 0x3Fu)); iCount = 3;
        }
        else
        {
            szBuffer[0] = static_cast<char>(0xF0u | (uChar >> 18));
            szBuffer[1] = static_cast<char>(0x80u | ((uChar >> 12) & 0x3Fu));
            szBuffer[2] = static_cast<char>(0x80u | ((uChar >> 6) & 0x3Fu));
            szBuffer[3] = static_cast<char>(0x80u | (uChar & 0x3Fu)); iCount = 4;
        }

        if (iCapacity > 0)
        {
            if (iWritten + iCount > iCapacity) return 0;
            std::memcpy(pOutput + iWritten, szBuffer, static_cast<size_t>(iCount));
        }
        iWritten += iCount;
    }
    return iWritten;
}
