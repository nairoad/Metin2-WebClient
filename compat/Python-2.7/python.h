// SPDX-License-Identifier: GPL-2.0-or-later
// Python-2.7/python.h - A REDIRECT TO PYTHON 3.
//
// WHY under this name: `ScriptLib/StdAfx.h` and `EterPythonLib` pull in
// `<Python-2.7/python.h>` hard-coded. Instead of rewriting dozens of
// directives in the TMP4 sources, I substitute a file with this name - the
// same trick as with `windows.h`.
//
// WHY to three: the client embeds CPython 3 built for wasm;
// the `Python-2.7` headers from `extern/include` are a dead end.
//
// NOTE: THIS IS MEASUREMENT SCAFFOLDING, not the port's final layer. It
// serves to count HOW MANY places in TMP4 need rewriting from two to
// three. In the end the port will build against CPython compiled for
// emscripten, with its own `pyconfig.h`.
//
// SECOND NOTE, taken from the mistake: on Windows `python.h`
// and `Python.h` are THE SAME FILE. A separate upper-case redirect **must
// not** be created here - it overwrites this one and gives an empty
// include.

#pragma once

#include <Python.h>

// The bridge from the two API to the three - see `python2_bridge.h` for the
// three differences in meaning this bridge does NOT hide.
#include "python2_bridge.h"
