// eterbase_test.cpp - FUNCTIONAL test of the ported `EterBase`.
//
// WHY, when the files already compile: **compilation does not see a missing
// body** - this project learned that painfully with `link_test.cpp`
// where a texture's `Render` did not exist for three rounds,
// while the header claimed the file was complete.
//
// This test goes a step further than linking: it **executes** TMP4 code under
// `node` and checks the results. Because the compatibility layer can link
// and give wrong numbers - and then the error would show only in the game.
//
// Running:
//   em++ -std=c++17 -O1 -Icompat -I<EterBase> -I<extern/include> \
//        eterbase_test.cpp -Llib -leterbase -lcryptopp -llzo -o eterbase_test.js
//   node eterbase_test.js

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CRC32.h"
#include "tea.h"
#include "Timer.h"

namespace {

int failures = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-52s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++failures;
}

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("=== EterBase under emscripten: functional test ===\n\n");
    char buf[160];

    // --- CRC32 --------------------------------------------------------------
    // Reference values computed INDEPENDENTLY (Python `zlib.crc32`), not
    // taken from the same code - otherwise the test would check itself.
    {
        const char* kText = "123456789";
        const DWORD crc = GetCRC32(kText, std::strlen(kText));
        std::snprintf(buf, sizeof(buf), "(0x%08lX, expected 0xCBF43926)",
                      static_cast<unsigned long>(crc));
        Check("GetCRC32(\"123456789\") = the standard CRC-32 vector", crc == 0xCBF43926u, buf);

        const DWORD empty = GetCRC32("", 0);
        std::snprintf(buf, sizeof(buf), "(0x%08lX)", static_cast<unsigned long>(empty));
        Check("GetCRC32 of empty input = 0", empty == 0u, buf);
    }

    // --- TEA: encryption and decryption are inverse -----------------------
    // This is a PROPERTY test, not a value test: I do not need to know the ciphertext
    // to check that `decrypt(encrypt(x)) == x`. Such a test passes
    // only when both sides work - and it cannot be fooled
    // by returning zeros.
    {
        unsigned long key[4] = { 0x01234567u, 0x89ABCDEFu, 0xFEDCBA98u, 0x76543210u };
        unsigned long plain[8];
        for (int i = 0; i < 8; ++i) plain[i] = 0x11111111u * static_cast<unsigned>(i + 1);

        unsigned long cipher[8] = {0};
        unsigned long back[8]   = {0};

        const int encSize = tea_encrypt(cipher, plain, key, sizeof(plain));
        Check("tea_encrypt returned a non-zero size", encSize > 0);

        const bool changed = std::memcmp(cipher, plain, sizeof(plain)) != 0;
        Check("the ciphertext DIFFERS from the plaintext", changed);

        tea_decrypt(back, cipher, key, static_cast<int>(sizeof(plain)));
        const bool roundtrip = std::memcmp(back, plain, sizeof(plain)) == 0;
        Check("tea_decrypt(tea_encrypt(x)) == x", roundtrip);

        // OPPOSITE control: a wrong key must not give the right result.
        // Without it the test would pass even if both functions were
        // the identity.
        unsigned long badKey[4] = { 1, 2, 3, 4 };
        unsigned long wrong[8]  = {0};
        tea_decrypt(wrong, cipher, badKey, static_cast<int>(sizeof(plain)));
        Check("decryption with a WRONG key does NOT give the plaintext",
              std::memcmp(wrong, plain, sizeof(plain)) != 0);
    }

    // --- Timer --------------------------------------------------------------
    // Checks the compatibility layer from the side that is easy to break: `Timer.cpp`
    // stands on `QueryPerformanceCounter` and `timeGetTime`, which I wrote myself.
    {
        ELTimer_Init();
        const DWORD t0 = ELTimer_GetMSec();
        // Spending time without `Sleep` - in the browser `Sleep` is explicitly empty.
        volatile double acc = 0.0;
        for (int i = 0; i < 3000000; ++i) acc += i * 0.5;
        const DWORD t1 = ELTimer_GetMSec();

        std::snprintf(buf, sizeof(buf), "(%lu -> %lu)",
                      static_cast<unsigned long>(t0), static_cast<unsigned long>(t1));
        Check("ELTimer_GetMSec does not go back", t1 >= t0, buf);
        Check("the clock runs at all (not frozen at zero)", t1 > 0);
        (void)acc;
    }

    std::printf("\n=== %s ===\n",
                failures == 0 ? "ETERBASE WORKS UNDER EMSCRIPTEN"
                              : "TEST FAILED");
    return failures == 0 ? 0 : 1;
}
