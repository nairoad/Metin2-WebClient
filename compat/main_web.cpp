// SPDX-License-Identifier: GPL-2.0-or-later
// main_web.cpp - the entry point: emscripten's linker wants `main`, the
// client has `WinMain` (UserInterface/UserInterface.cpp). `main` sets the
// locale from the page address and calls `WinMain` with an empty command line.

// Design:
// The command line is empty ON PURPOSE. `WinMain` reads `--hackshield`,
// `--openid-test` and a language version from it; none exists in the
// browser and faking them would be guessing. If they are ever needed they
// come from the page address, not from here.
//
// `WinMain` ends in a loop that never returns - the Windows message pump.
// In a browser that would hang the tab, so the loop runs on
// `emscripten_set_main_loop` (decided).
// Earlier `main` entered `WinMain` only on a flag in the address, a
// leftover from when the loop was an open question - the page looked broken
// to anyone who did not know the flag. Now it runs by default and `?run=0`
// asks for the link check alone (does everything link and reach `main`?)
// without the question about the loop.
//
// Linkage trap: the first version declared `WinMain` as `extern "C"` and the
// link "passed" with 263 kB and ONE unresolved symbol. `WinMain` is a plain
// C++ function with a mangled name; `extern "C"` looked for another one, and
// since `main` reached nothing, the linker dropped the WHOLE game. A green
// measurement of emptiness.

#include <cstdio>
#include <cstring>

#include "win32_compat.h"

#include <emscripten/emscripten.h>

/// Whether the page address asks to run (`?run=` other than
/// `0`; m2w.options in runtime.js). Asked of JavaScript because there is no
/// command line in the browser and the address is the only place the user
/// can say something before start.
EM_JS(int, m2w_should_run, (), { return m2w.shouldRun(); });

/// TMP4's entry point (`UserInterface/UserInterface.cpp`), called by `main`
/// below.
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPSTR lpCmdLine, int nCmdShow);

/// Entry point: link banner, optional `?run=0` exit, locale, `WinMain`.
int main(int, char**)
{
    std::printf("=== TMP4 client - link reached main() ===\n");

    if (!m2w_should_run())
    {
        // Not an error: the answer to "did it link" asked without the second
        // question, "does the main loop run".
        std::printf("link check only (?run=0) - exiting\n");
        return 0;
    }

    // Language and code page from the page address, BEFORE WinMain reads
    // locale.cfg.
    extern void M2W_SetLocale();
    M2W_SetLocale();

    char szEmpty[] = "";
    return WinMain(NULL, NULL, szEmpty, 0);
}
