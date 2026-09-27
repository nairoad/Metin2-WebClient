// eterpack_test.cpp - FUNCTIONAL test of the ported `EterPack`.
//
// Criterion: **write a pack, close it, open it again, read - and compare
// byte for byte.** This is a PROPERTY test, like TEA: I do not need
// to know the format by heart to establish that both sides work - and the test
// cannot be fooled by returning zeros, because zeros will not match the input
// data.
//
// WHY it does not test on a real pack from the server: the data we have is
// in the **WebFs** format (`M2WF` + chunks), not in `EterPack` (`.eix` / `.epk`).
// The port replaced the storage layer, keeping the rest. A check on
// a real file would need an `.eix` pack we do not have - and it is better
// to write that down than to pretend a wider coverage.
//
// Running:
//   node eterpack_test.js

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "EterPack.h"
#include "lzo.h"
#include "EterPackManager.h"
#include "MappedFile.h"

namespace {

int failures = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-52s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++failures;
}

/// Test content: incompressible (rising bytes, interleaved), so that the pass
/// through LZO means anything. A run of zeros alone would pass even with broken
/// compression.
std::vector<BYTE> MakePayload(size_t n)
{
    std::vector<BYTE> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<BYTE>((i * 37u) ^ (i >> 3));
    }
    return v;
}

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("=== EterPack under emscripten: writing and reading a pack ===\n\n");
    char buf[160];

    // `CEterPack` calls `CLZO::Instance()` on every write and read.
    // `CSingleton` in TMP4 **does not create itself** - the `ms_singleton` assertion
    // crashes the program if the object does not exist. A deliberate decision of the authors:
    // the singleton is to be created explicitly, at a known moment.
    //
    // I keep it on the stack of `main`, because that is exactly what the client does: global
    // objects come to life at the start and live to the end.
    CLZO lzo;
    Check("CLZO::Instance() available after creating the object",
          &CLZO::Instance() == &lzo);

    const char* kDbName   = "testpack";
    const char* kPathName = "/tmp/";
    const char* kEntry    = "dir/test_file.bin";
    const std::vector<BYTE> payload = MakePayload(4096);

    // --- write --------------------------------------------------------------
    {
        CEterFileDict dict;
        CEterPack pack;

        // `bReadOnly = false` - otherwise `Put` has nowhere to write.
        const bool created = pack.Create(dict, kDbName, kPathName, /*bReadOnly*/ false);
        Check("CEterPack::Create for writing", created);
        if (!created) {
            std::printf("\n=== TEST FAILED (nothing to read) ===\n");
            return 1;
        }

        const bool put = pack.Put(kEntry, payload.data(),
                                  static_cast<long>(payload.size()), /*packType*/ 1);
        Check("CEterPack::Put wrote the file into the pack", put);

        Check("IsExist sees the file right after Put", pack.IsExist(kEntry));
        pack.Destroy();
    }

    // --- read from a NEW object -------------------------------------------
    // A separate object matters here: if `Get` read from the write
    // cache, the test would pass even with writing to disk not working.
    {
        CEterFileDict dict;
        CEterPack pack;

        const bool opened = pack.Create(dict, kDbName, kPathName, /*bReadOnly*/ true);
        Check("opening the pack again (a new object)", opened);
        if (!opened) {
            std::printf("\n=== TEST FAILED ===\n");
            return 1;
        }

        Check("IsExist sees the file after reopening", pack.IsExist(kEntry));

        // OPPOSITE control: the pack must not "find" something that is not there.
        Check("IsExist does NOT see a file that was not written",
              !pack.IsExist("dir/no_such_file.bin"));

        CMappedFile mapped;
        LPCVOID data = nullptr;
        const bool got = pack.Get(mapped, kEntry, &data);
        Check("CEterPack::Get returned data", got && data != nullptr);

        if (got && data) {
            const size_t size = static_cast<size_t>(mapped.Size());
            std::snprintf(buf, sizeof(buf), "(%zu, expected %zu)", size, payload.size());
            Check("size read = size written", size == payload.size(), buf);

            if (size == payload.size()) {
                const bool same = std::memcmp(data, payload.data(), size) == 0;
                Check("the content matches BYTE FOR BYTE", same);

                // An opposite control to the comparison: if `memcmp` compared
                // anything with anything, this test would not pass.
                std::vector<BYTE> other = payload;
                other[size / 2] ^= 0xFFu;
                Check("the comparison DETECTS a one-byte difference",
                      std::memcmp(data, other.data(), size) != 0);
            }
        }

        std::vector<std::string> names;
        if (pack.GetNames(&names)) {
            std::snprintf(buf, sizeof(buf), "(%zu)", names.size());
            Check("GetNames returns exactly one entry", names.size() == 1, buf);
            if (names.size() == 1) {
                Check("the entry name matches the written one", names[0] == kEntry,
                      names[0].c_str());
            }
        }
        pack.Destroy();
    }

    std::printf("\n=== %s ===\n",
                failures == 0 ? "ETERPACK WORKS UNDER EMSCRIPTEN"
                              : "TEST FAILED");
    return failures == 0 ? 0 : 1;
}
