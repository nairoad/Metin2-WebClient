// SPDX-License-Identifier: GPL-2.0-or-later
// platform_crash.cpp - crash reporting without SEH: five POSIX signals plus
// `std::terminate`, each printing a call stack to stderr and then ending the
// program the way it should end.

// Design:
// `EterBase/error.cpp` in TMP4 stands on Windows structured exception
// handling - `SetUnhandledExceptionFilter` catches the crash, `StackWalk64`
// from `imagehlp` walks the stack. Neither exists outside Windows; it was
// one of the two files the project kept as "needs a replacement, not a stub"
// (the other being `CPostIt.cpp`, the clipboard).
//
// The equivalent this platform has is the POSIX one: five signals plus
// `std::terminate`, with the stack from `emscripten_get_callstack`, which
// names the functions itself. Note that names appear only in builds with `-g` /
// `-gsource-map`; without them the trace shows `wasm-function[14]`
//
// Where the handlers get installed: there is no separate start-up code
// doing it before `WinMain`. The install point is
// `SetEterExceptionHandler()`, called from the `CPythonApplication`
// constructor (PythonApplication.cpp) - the same call TMP4 made to install
// SEH. `UiPlatform_CrashHandlerInstall` is the implementation behind it and
// is what compat/tests/platform_crash_test.cpp exercises; installing twice is
// harmless.

#include "win32_compat.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace
{

/// Whether the handlers are installed. Installing twice does no harm, but
/// remembering lets `UiPlatform_CrashHandlerInstalled` tell the truth.
bool g_bInstalled = false;

/// Human-readable name of a crash signal (SIGSEGV, SIGABRT, SIGFPE, SIGILL,
/// SIGBUS where it exists); "unknown signal" otherwise.
const char* SignalName(int iSignal)
{
    switch (iSignal)
    {
        // In wasm a real out-of-bounds access is a JS RuntimeError trap,
        // not a signal - SIGSEGV arrives only when something raises it.
        case SIGSEGV: return "SIGSEGV (memory access out of bounds)";
        case SIGABRT: return "SIGABRT (program aborted)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS (bad memory access)";
#endif
        default:      return "unknown signal";
    }
}

/// Writes the C call stack to stderr (`emscripten_get_callstack`); a one-line
/// note instead when it is unavailable or outside emscripten.
void PrintCallStack()
{
#ifdef __EMSCRIPTEN__
    // `EM_LOG_FUNC_PARAMS` is deprecated, so only the C stack is requested.
    char szBuffer[8 * 1024];
    const int n = emscripten_get_callstack(EM_LOG_C_STACK, szBuffer, sizeof(szBuffer));
    if (n > 0)
    {
        std::fputs(szBuffer, stderr);
        std::fputc('\n', stderr);
    }
    else
    {
        std::fputs("  (call stack unavailable)\n", stderr);
    }
#else
    std::fputs("  (call stack only under emscripten)\n", stderr);
#endif
}

/// Signal handler: prints the crash banner, the signal name and the call
/// stack to stderr, then restores the default action and re-raises, so the
/// process ends with the real cause.
void OnSignal(int iSignal)
{
    // Very little is allowed inside a signal handler. `fputs` and `write`
    // are safe, `printf` with formatting is not necessarily, so the message
    // is assembled from ready pieces.
    std::fputs("\n=== CLIENT CRASH ===\n", stderr);
    std::fputs("signal: ", stderr);
    std::fputs(SignalName(iSignal), stderr);
    std::fputc('\n', stderr);
    PrintCallStack();
    std::fputs("=== end ===\n", stderr);

    // Restore the default action and raise again, so the program ends THE
    // WAY IT SHOULD. A plain `exit` would misreport the cause - to outside
    // tools it would look like a clean exit.
    std::signal(iSignal, SIG_DFL);
    std::raise(iSignal);
}

/// `std::terminate` handler: prints the crash banner and, when an exception
/// is in flight, its `what()` message; then the call stack, then `abort()`.
void OnTerminate()
{
    std::fputs("\n=== CLIENT CRASH ===\n", stderr);
    std::fputs("unhandled C++ exception (std::terminate)\n", stderr);

    // If the end comes from an exception it can still be caught and its
    // message shown - usually the most valuable line of the whole dump.
    if (std::exception_ptr pCurrent = std::current_exception())
    {
        try
        {
            std::rethrow_exception(pCurrent);
        }
        catch (const std::exception& e)
        {
            std::fputs("message: ", stderr);
            std::fputs(e.what(), stderr);
            std::fputc('\n', stderr);
        }
        catch (...)
        {
            std::fputs("message: (exception not derived from std::exception)\n", stderr);
        }
    }

    PrintCallStack();
    std::fputs("=== end ===\n", stderr);
    std::abort();
}

}  // namespace

/// Installs the five signal handlers and the terminate handler; a no-op
/// when already installed.
void UiPlatform_CrashHandlerInstall()
{
    if (g_bInstalled)
        return;
    std::signal(SIGSEGV, OnSignal);
    std::signal(SIGABRT, OnSignal);
    std::signal(SIGFPE,  OnSignal);
    std::signal(SIGILL,  OnSignal);
#ifdef SIGBUS
    std::signal(SIGBUS,  OnSignal);
#endif
    std::set_terminate(OnTerminate);
    g_bInstalled = true;
}

/// Whether `UiPlatform_CrashHandlerInstall` has run (platform_crash_test.cpp).
bool UiPlatform_CrashHandlerInstalled()
{
    return g_bInstalled;
}

/// TMP4's install call for SEH (`CPythonApplication` constructor); here it
/// installs the POSIX handlers. May be called any number of times.
void SetEterExceptionHandler()
{
    UiPlatform_CrashHandlerInstall();
}
