// SPDX-License-Identifier: GPL-2.0-or-later
// process.h - Win32 threads. The web port has NO threads.
//
// Pulled in by `eterLib/StdAfx.h`. `GameLib/AreaLoaderThread.cpp` calls
// `_beginthreadex` to load map pieces in the background.
//
// DECLARED, BUT NOT DEFINED - and that is a choice, not an unfinished job.
// So the file **parses** (it does not block the rest of the measurement),
// and linking stops loudly at the missing symbol. A stub returning zero
// would be worse: the code would believe the thread started and would wait
// for data that never comes.
//
// In the end, background loading in the browser is done differently -
// through asynchronous requests in the main loop or a Web Worker. That
// will be a separate decision when `AreaLoaderThread` is ported.

#pragma once

#include "win32_compat.h"

/// The thread entry point type of `_beginthreadex`.
typedef unsigned (__stdcall *M2W_THREAD_START)(void*);

/// Declared only - no body (see the note at the top of the file).
uintptr_t _beginthreadex(void* security, unsigned stack_size,
                         unsigned (__stdcall *start_address)(void*),
                         void* arglist, unsigned initflag, unsigned* thrdaddr);
/// Declared only - no body.
void      _endthreadex(unsigned retval);
