// SPDX-License-Identifier: GPL-2.0-or-later
// python_marshal_web.cpp - the three `_PyMarshal_*` functions declared in
// `ScriptLib/PythonMarshal.h`, implemented on CPython's own marshal instead
// of the Python 2 `marshal.c` copy that the original client carried.

// Design:
// `stage/ScriptLib/PythonMarshal.cpp` is `marshal.c` from CPython 2 pasted
// into the Metin2 tree, each function renamed with a leading underscore. It
// reads Python 2 structures - a 16-bit `argcount` in the code object,
// `ob_size`/`ob_digit` in integers, `PyEval_GetRestricted` - and builds a
// code object filled with Python 2 BYTECODE. Earlier it sat on the
// "does not compile" list with five errors of the kind "this name is spelled
// differently in Python 3". Fixing those would have been the WORST option:
// the file would compile, link and run - reading Python 2 data into Python
// 3 structures.
//
// The client embeds CPython 3 built for wasm32-emscripten, so it has a
// working marshal of its own, and that is what gets called. Hence
// `PythonMarshal.cpp` is on the list of files replaced ON PURPOSE
// (tools/build_gamelib.py), next to `milesLib` and `GrpDetector.cpp`, not
// on the "to fix" list.
//
// Consequence for the game scripts: Metin2 packages carry `.pyc` compiled
// by Python 2, and no marshal saves them - Python 2 bytecode means nothing
// to Python 3, and `RunCompiledFile` rejects them on the magic number
// anyway. The scripts have to be RECOMPILED, as the port did; that is work
// on the data and a separate decision. This file only provides the three
// functions `PythonLauncher.cpp` calls, and provides them correctly.

#include <Python.h>
#include <marshal.h>

#include <cstdio>

extern "C" {

/// Reads one marshalled object from the current position of `fp`.
PyObject* _PyMarshal_ReadObjectFromFile(FILE* fp)
{
    return PyMarshal_ReadObjectFromFile(fp);
}

/// Reads the LAST object in `fp`: the rest of the file may be slurped into
/// memory in one go instead of byte by byte. CPython makes the same
/// distinction under the same name, so the optimisation stays there.
PyObject* _PyMarshal_ReadLastObjectFromFile(FILE* fp)
{
    return PyMarshal_ReadLastObjectFromFile(fp);
}

/// Reads a 32-bit little-endian integer (the `.pyc` magic number and the
/// time stamp).
long _PyMarshal_ReadLongFromFile(FILE* fp)
{
    return PyMarshal_ReadLongFromFile(fp);
}

}  // extern "C"
