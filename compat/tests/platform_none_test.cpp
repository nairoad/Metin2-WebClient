// platform_none_test.cpp - test of the CONTRACT of the "nothing here" layer.
//
// WHY: these functions compute nothing, so they cannot be checked by
// comparing a result. What can be checked is the **contract**: whether they answer
// exactly as TMP4 code expects, and whether loops written in the Win32 style
// end correctly on them.
//
// That matters, because an error in this layer does not show as a crash. If
// `Process32First` returned TRUE instead of FALSE, the anti-cheat scanner
// would read an **unfilled structure** and report a non-existent process -
// or fall into an endless loop. The test checks exactly that.
//
// Running:
//   em++ -std=c++17 -O1 -Icompat compat/tests/platform_none_test.cpp compat/win32_compat.cpp \
//        compat/platform_none.cpp -o platform_none_test.js && node platform_none_test.js

#include <cstdio>
#include <cstring>

#include "win32_compat.h"
#include "tlhelp32.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-56s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("=== the \"nothing here\" layer: the contract with TMP4 code ===\n\n");

    // --- 1. Process snapshot ------------------------------------------------
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        Check("CreateToolhelp32Snapshot returns INVALID_HANDLE_VALUE",
                snapshot == INVALID_HANDLE_VALUE);

        // A loop written EXACTLY the way `ProcessScanner.cpp`
        // in TMP4 does it. If `Process32First` lies, this loop will either read
        // garbage or never end.
        PROCESSENTRY32 entry;
        std::memset(&entry, 0xCD, sizeof(entry));   // deliberately GARBAGE in the buffer
        entry.dwSize = sizeof(entry);

        int counted = 0;
        if (Process32First(snapshot, &entry)) {
            do { ++counted; } while (Process32Next(snapshot, &entry) && counted < 1000);
        }
        char buf[80];
        std::snprintf(buf, sizeof(buf), "(counted %d)", counted);
        Check("the loop over processes ends immediately", counted == 0, buf);

        // OPPOSITE control to the previous one: the buffer still holds garbage, i.e.
        // none of the functions wrote to it. If one of them partly
        // filled the structure, this test would fail.
        bool untouched = true;
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&entry);
        for (size_t i = sizeof(DWORD); i < sizeof(entry); ++i) {
            if (bytes[i] != 0xCD) { untouched = false; break; }
        }
        Check("the buffer was NOT touched (no partial filling)",
                untouched);
    }

    // --- 2. Modules -----------------------------------------------------------
    {
        MODULEENTRY32 module;
        std::memset(&module, 0, sizeof(module));
        module.dwSize = sizeof(module);
        Check("Module32First returns FALSE", !Module32First(INVALID_HANDLE_VALUE, &module));
        Check("Module32Next returns FALSE",  !Module32Next(INVALID_HANDLE_VALUE, &module));
    }

    // --- 3. Reading another process's memory ----------------------------------
    {
        char target[16];
        std::memset(target, 0x5A, sizeof(target));
        size_t bytesRead = 12345;          // deliberately a NON-ZERO value

        const BOOL result = ReadProcessMemory(NULL, (const void*)0x1000,
                                             target, sizeof(target), &bytesRead);
        Check("ReadProcessMemory returns FALSE", !result);
        // Win32 writes the number of bytes read ALSO on failure -
        // that is part of the contract, not a courtesy.
        Check("and zeroes the count of bytes read", bytesRead == 0);

        bool targetUntouched = true;
        for (size_t i = 0; i < sizeof(target); ++i) {
            if (static_cast<unsigned char>(target[i]) != 0x5A) { targetUntouched = false; break; }
        }
        Check("and does NOT write to the target buffer", targetUntouched);

        Check("OpenProcess returns NULL", OpenProcess(PROCESS_VM_READ, FALSE, 1) == NULL);
    }

    // --- 4. Windows and resources ---------------------------------------------
    {
        Check("FindWindow finds no window", FindWindowA("Class", "Title") == NULL);
        Check("LoadIcon returns no icon", LoadIconA(NULL, MAKEINTRESOURCE(1)) == NULL);
        // Earlier `LoadImageA` returned NULL and the client aborted the start on
        // "CREATE_CURSOR" (CPythonApplication::CreateCursors treats NULL
        // as an error). Now cursors have handles (cursor_web.cpp).
        Check("LoadImage returns a cursor handle",
                LoadImageA(NULL, "cursor", IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR) != NULL);
        Check("DestroyCursor reports a successful cleanup",
                DestroyCursor(NULL) == TRUE);
    }

    // --- 5. Keys and system settings -------------------------------------------
    {
        Check("GetKeyState says 'not pressed'", GetKeyState(VK_SHIFT) == 0);
        Check("GetKeyState does not pretend a toggle", GetKeyState(VK_SCROLL) == 0);

        STICKYKEYS sk;
        std::memset(&sk, 0, sizeof(sk));
        sk.cbSize = sizeof(sk);
        Check("SystemParametersInfo does NOT change system settings",
                SystemParametersInfoA(SPI_SETSTICKYKEYS, sizeof(sk), &sk, 0) == FALSE);
    }

    // --- 6. The rest ------------------------------------------------------------
    {
        Check("_controlfp returns zero (there is no control word)",
                _controlfp(_PC_24, _MCW_PC) == 0u);
        Check("SetFileAttributes reports success (there are no attributes)",
                SetFileAttributesA("file.txt", 0) == TRUE);
    }

    std::printf("\n=== %s ===\n",
                errors == 0 ? "THE \"NOTHING HERE\" LAYER KEEPS ITS CONTRACT"
                           : "TEST FAILED");
    return errors == 0 ? 0 : 1;
}
