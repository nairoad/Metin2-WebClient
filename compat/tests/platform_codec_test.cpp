// platform_codec_test.cpp - `MultiByteToWideChar` and `WideCharToMultiByte`
// on the code pages Metin2 really sends.
//
// ===========================================================================
// WHAT IS CHECKED HERE, AND WHAT DOES NOT NEED CHECKING
// ===========================================================================
// The TABLES themselves are covered by `zlecenia_gemini/zadanie_B_strony_kodowe`
// (not published) - that set checks all 768 assignments against reference data
// and there is no point repeating it here.
//
// This file checks the LAYER ABOVE the tables, i.e. the Win32 semantics that
// TMP4 code uses and relies on:
//
//   - a NEGATIVE length means "the string ends with a zero", and the zero BELONGS to
//     the string and is converted too (hence seven, not six);
//   - `room == 0` means "do not write, tell me how much is needed" - and then
//     the buffer must not be touched;
//   - a buffer too small is ZERO, not a string cut in half;
//   - a character the page does not have becomes `?`, and the conversion goes
//     on - one exotic character must not wipe out a whole sentence;
//   - a surrogate pair is ONE character, so also one question mark.
//
// And one thing apart: `CP949` has to keep returning ZERO. That is the true
// answer - we do not have the Korean table - and the test guards it, so that
// nobody ever silently replaces it with "I converted something".

#include <cstdio>
#include <cstring>

#include "win32_compat.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-62s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

const UINT CP_1250 = 1250;
const UINT CP_1252 = 1252;
const UINT CP_874 = 874;
const UINT CP_949 = 949;

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("\n=== code pages in the Win32 layer ===\n\n");

    char buf[64];

    // -----------------------------------------------------------------
    std::printf("[CP1250 - Polish letters there and back]\n");
    {
        // "ZOLW" ("tortoise") in Polish, in CP1250: Z with a dot, o with an acute, l with a stroke, w.
        const char aBytes[] = { '\xAF', '\xF3', '\xB3', 'w', 0 };

        WCHAR aWide[16];
        const int iChars = MultiByteToWideChar(CP_1250, 0, aBytes, -1,
                                                aWide, 16);
        std::snprintf(buf, sizeof(buf), "(returned %d)", iChars);
        // Four letters PLUS the terminating zero - a negative length counts it.
        Check("a negative length counts the terminating zero too", iChars == 5, buf);

        Check("0xAF is Z with a dot (U+017B)", aWide[0] == 0x017B);
        Check("0xF3 is o with an acute (U+00F3)", aWide[1] == 0x00F3);
        Check("0xB3 is l with a stroke (U+0142)", aWide[2] == 0x0142);
        Check("'w' stays 'w'", aWide[3] == L'w');

        char aBack[16];
        const int iBytes = WideCharToMultiByte(CP_1250, 0, aWide, iChars,
                                                aBack, 16, NULL, NULL);
        std::snprintf(buf, sizeof(buf), "(returned %d)", iBytes);
        Check("back comes out the same number of bytes", iBytes == 5, buf);
        Check("and they are THE SAME bytes",
                std::memcmp(aBack, aBytes, 5) == 0);
    }

    // -----------------------------------------------------------------
    std::printf("\n[the same number, a DIFFERENT page - a different character]\n");
    {
        // This is the check that the page number really changes something.
        // Without it the whole set would pass even if the layer
        // used one table for all pages.
        const char aOne[] = { '\xF3', 0 };
        WCHAR a1250[4], a1252[4], a874[4];

        MultiByteToWideChar(CP_1250, 0, aOne, 1, a1250, 4);
        MultiByteToWideChar(CP_1252, 0, aOne, 1, a1252, 4);
        MultiByteToWideChar(CP_874, 0, aOne, 1, a874, 4);

        std::snprintf(buf, sizeof(buf), "(1250:%04X 1252:%04X 874:%04X)",
                      static_cast<unsigned>(a1250[0]),
                      static_cast<unsigned>(a1252[0]),
                      static_cast<unsigned>(a874[0]));

        // In CP1250 and CP1252 the byte 0xF3 happens to be the same character
        // (o with an acute), but in CP874 it is a Thai letter - and that is enough.
        Check("0xF3 in CP874 means something else than in CP1250",
                a874[0] != a1250[0], buf);
        Check("and it is a Thai character from the U+0E00 block",
                a874[0] >= 0x0E00 && a874[0] <= 0x0E5B, buf);
    }

    // -----------------------------------------------------------------
    std::printf("\n[the Win32 semantics TMP4 code relies on]\n");
    {
        const char aBytes[] = { '\xAF', '\xF3', '\xB3', 'w', 0 };

        // `room == 0` - a question about the size, without writing.
        WCHAR aGuard[8];
        for (int i = 0; i < 8; ++i)
            aGuard[i] = 0xBEEF;
        const int iNeeded = MultiByteToWideChar(CP_1250, 0, aBytes, -1,
                                                  aGuard, 0);
        std::snprintf(buf, sizeof(buf), "(returned %d)", iNeeded);
        Check("zero room: returns the NEEDED size", iNeeded == 5, buf);
        Check("zero room: does NOT touch the buffer", aGuard[0] == 0xBEEF);

        // A buffer too small is zero, not a truncated string.
        WCHAR aSmall[2];
        const int iTooSmall = MultiByteToWideChar(CP_1250, 0, aBytes, -1,
                                                aSmall, 2);
        std::snprintf(buf, sizeof(buf), "(returned %d)", iTooSmall);
        Check("a buffer too small is ZERO, not a truncated string",
                iTooSmall == 0, buf);

        // An explicitly given length does NOT add a zero.
        const int iExplicit = MultiByteToWideChar(CP_1250, 0, aBytes, 4,
                                                aGuard, 8);
        std::snprintf(buf, sizeof(buf), "(returned %d)", iExplicit);
        Check("an explicit length does not add a zero", iExplicit == 4, buf);
    }

    // -----------------------------------------------------------------
    std::printf("\n[a character the page does not have]\n");
    {
        // A Chinese character that is in none of our three pages.
        const WCHAR aWide[] = { L'a', 0x4E00, L'b', 0 };
        char aBytes[16];
        BOOL bUsedDefault = FALSE;

        const int iBytes = WideCharToMultiByte(CP_1250, 0, aWide, 3,
                                                aBytes, 16, NULL,
                                                &bUsedDefault);
        std::snprintf(buf, sizeof(buf), "(returned %d, middle '%c')",
                      iBytes, iBytes >= 2 ? aBytes[1] : '?');

        // THE MOST IMPORTANT SENTENCE OF THIS SECTION: the conversion has to go ON.
        // Breaking the string on one exotic character would wipe out the whole
        // sentence - and in the game that looks like a lost message, not like an encoding
        // error.
        Check("an unknown character is '?', and the string goes on",
                iBytes == 3 && aBytes[0] == 'a' && aBytes[1] == '?' &&
                aBytes[2] == 'b', buf);
        Check("and the layer SAYS it used the default character",
                bUsedDefault == TRUE);

        // A surrogate pair: ONE question mark, not two. Counting it
        // as two shifts the whole rest of the string.
        const WCHAR aPair[] = { L'a', 0xD83D, 0xDE00, L'b', 0 };
        const int iPair = WideCharToMultiByte(CP_1250, 0, aPair, 4,
                                              aBytes, 16, NULL, NULL);
        std::snprintf(buf, sizeof(buf), "(returned %d)", iPair);
        Check("a surrogate pair gives ONE question mark",
                iPair == 3 && aBytes[1] == '?' && aBytes[2] == 'b', buf);
    }

    // -----------------------------------------------------------------
    std::printf("\n[UTF-8 STILL works - this could not break]\n");
    {
        // OPPOSITE control on the whole rework: adding the tables must not
        // touch the route that worked before.
        const char c_szUtf8[] = "a\xC5\xBC" "b";   // 'a', z with a dot, 'b'
        WCHAR aWide[8];
        const int iChars = MultiByteToWideChar(CP_UTF8, 0, c_szUtf8, -1,
                                                aWide, 8);
        std::snprintf(buf, sizeof(buf), "(returned %d, middle %04X)",
                      iChars, iChars >= 2 ? static_cast<unsigned>(aWide[1]) : 0);
        Check("UTF-8 splits as before",
                iChars == 4 && aWide[1] == 0x017C, buf);

        char aBack[16];
        const int iBytes = WideCharToMultiByte(CP_UTF8, 0, aWide, iChars,
                                                aBack, 16, NULL, NULL);
        Check("and composes back",
                iBytes == 5 && std::memcmp(aBack, c_szUtf8, 4) == 0);
    }

    // -----------------------------------------------------------------
    std::printf("\n[CP949 - a refusal, and it has to stay one]\n");
    {
        // The Korean page is DOUBLE-BYTE and has about 17000 assignments.
        // We do not have it. Zero means "I did not convert" and is the TRUE
        // answer - this case stands here so that nobody ever silently
        // replaces it with "I converted something".
        const char aBytes[] = { '\xB0', '\xA1', 0 };
        WCHAR aWide[8];
        const int iChars = MultiByteToWideChar(CP_949, 0, aBytes, -1,
                                                aWide, 8);
        std::snprintf(buf, sizeof(buf), "(returned %d)", iChars);
        Check("CP949 still honestly refuses", iChars == 0, buf);

        const WCHAR aTest[] = { L'a', 0 };
        char aResult[8];
        Check("and the other way too",
                WideCharToMultiByte(CP_949, 0, aTest, 2, aResult, 8,
                                    NULL, NULL) == 0);
    }

    // -----------------------------------------------------------------
    std::printf("\n[a full pass 0x20-0xFF through the Win32 layer]\n");
    {
        // Every byte this page knows has to come back to itself THROUGH THE WHOLE
        // LAYER - not through the table alone. That catches an error that does not
        // concern the table: a lost cast, a wrong length, a mixed-up page
        // number in one of the two functions.
        const UINT aPages[3] = { CP_1250, CP_1252, CP_874 };
        const char* aNames[3] = { "CP1250", "CP1252", "CP874" };

        for (int s = 0; s < 3; ++s)
        {
            int iChecked = 0;
            bool bAll = true;

            for (int b = 0x20; b <= 0xFF; ++b)
            {
                const char aOne[1] = { static_cast<char>(b) };
                WCHAR kWide[2] = { 0, 0 };
                if (MultiByteToWideChar(aPages[s], 0, aOne, 1,
                                        kWide, 2) != 1)
                {
                    bAll = false;
                    break;
                }

                // An unassigned byte gives the replacement character - there is nothing to
                // convert it back to, and that is not an error.
                if (kWide[0] == 0xFFFD)
                    continue;

                char kBack[2] = { 0, 0 };
                const int iCount = WideCharToMultiByte(aPages[s], 0, kWide, 1,
                                                     kBack, 2, NULL, NULL);
                if (iCount != 1 ||
                    static_cast<unsigned char>(kBack[0]) !=
                        static_cast<unsigned char>(b))
                {
                    bAll = false;
                    break;
                }
                ++iChecked;
            }

            std::snprintf(buf, sizeof(buf), "(bytes %d)", iChecked);
            char kName[64];
            std::snprintf(kName, sizeof(kName),
                          "%s: every assigned byte comes back to itself", aNames[s]);
            Check(kName, bAll && iChecked > 180, buf);
        }
    }

    std::printf("\n=== %s ===\n",
                errors == 0 ? "CODE PAGES WORK" : "TEST FAILED");
    return errors == 0 ? 0 : 1;
}
