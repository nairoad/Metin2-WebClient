// platform_crash_test.cpp - test of the crash handling.
//
// THE DIFFICULTY: this layer **ends the program** by design, so it cannot be
// checked with an ordinary sequence of assertions - the first crash would take
// the whole test with it.
//
// Solution: the test has two modes.
//   * without an argument - checks what can be checked WITHOUT a crash
//     (whether the handlers are installed, whether `SetEterExceptionHandler` is safe
//     when repeated);
//   * with the argument `crash` - **deliberately causes a crash**, to see
//     the message. That is run separately and the output is looked at.
//
// Running:
//   node platform_crash_test.js            <- assertions
//   node platform_crash_test.js crash      <- shows the message

#include <csignal>
#include <cstdio>
#include <cstring>

#include "win32_compat.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-56s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

}  // namespace

/// Without arguments: the checks that need no crash (exit 0 when all passed);
/// with `crash`: raises SIGABRT to show the crash message.
int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "crash") == 0) {
        std::printf("=== a deliberate crash: raising SIGABRT ===\n");
        std::fflush(stdout);
        UiPlatform_CrashHandlerInstall();
        std::raise(SIGABRT);
        std::printf("FAIL: the program survived SIGABRT\n");
        return 1;
    }

    std::printf("=== crash handling: what can be checked without a crash ===\n\n");

    Check("before installing, the handlers are NOT installed",
            !UiPlatform_CrashHandlerInstalled());

    UiPlatform_CrashHandlerInstall();
    Check("after installing, the handlers are installed",
            UiPlatform_CrashHandlerInstalled());

    // Repeating must be safe: `SetEterExceptionHandler` is called
    // from TMP4 code at an unknown moment, perhaps many times.
    UiPlatform_CrashHandlerInstall();
    SetEterExceptionHandler();
    SetEterExceptionHandler();
    Check("installing again breaks nothing",
            UiPlatform_CrashHandlerInstalled());

    // The handler must be OURS, not the default - otherwise a crash would pass
    // without a message. I check it by asking the system for the current handler:
    // `signal` returns the PREVIOUS one, so after reading it I put it back.
    {
        void (*previous)(int) = std::signal(SIGSEGV, SIG_DFL);
        const bool ours = (previous != SIG_DFL && previous != SIG_ERR);
        std::signal(SIGSEGV, previous);   // restoring the state
        Check("SIGSEGV has a handler other than the default", ours);
    }
    {
        void (*previous)(int) = std::signal(SIGFPE, SIG_DFL);
        const bool ours = (previous != SIG_DFL && previous != SIG_ERR);
        std::signal(SIGFPE, previous);
        Check("SIGFPE has a handler other than the default", ours);
    }

    std::printf("\n=== %s ===\n",
                errors == 0 ? "CRASH HANDLERS INSTALLED"
                           : "TEST FAILED");
    std::printf("(to see the crash message: node platform_crash_test.js crash)\n");
    return errors == 0 ? 0 : 1;
}
