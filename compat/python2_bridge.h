// SPDX-License-Identifier: GPL-2.0-or-later
// python2_bridge.h - the bridge from the Python 2 C API to Python 3: module
// registration, integers, strings in the game's code page, `Py_BuildValue`,
// the opaque frame object and the `.pyc` header - exactly what TMP4 uses.

// Design:
// The MEASUREMENT behind this file: of the 60 `UserInterface`
// files that did not compile, 23 were blocked by ONE pattern -
// `Py_InitModule`; the rest of the Python 2 names occur 25 times in all of
// TMP4. So this is not a "just in case" compatibility layer: it contains
// what the TMP4 code uses and nothing more.
//
// THREE DIFFERENCES OF MEANING the bridge does NOT hide:
// 1. `Py_InitModule` put the module into `sys.modules`; `PyModule_Create`
//    DOES NOT. Without that the code would compile and run, and only
//    `import ui` in a game script would say "no such module". The bridge
//    inserts explicitly - its main job, not the renaming.
// 2. `PyModuleDef` must OUTLIVE the module: in Python 2 the method table
//    was enough, in 3 the module keeps the definition by pointer. It is
//    allocated on the heap and never freed - the game modules are never
//    unloaded - a deliberate leak of a few dozen structs, not an oversight.
// 3. `PyString` is BYTES, `PyUnicode` is CHARACTERS. Metin2 sends text in
//    code pages (`CodePageId.h` from `EterLocale`), not UTF-8, so
//    substituting `PyUnicode_FromString` for `PyString_FromString` CHANGES
//    THE MEANING of everything beyond ASCII: 3 demands valid UTF-8 and
//    raises on a bad byte instead of passing it on. SETTLED BY A
//    MEASUREMENT IN THE GAME: the C side keeps text in the GAME
//    CODE PAGE (1250 for Polish) as the Windows client did - the server,
//    the language files and the drawing (`GetDefaultCodePage`) expect it -
//    and Python 3 keeps characters, so the bridge translates at the border:
//    C -> Python, code-page bytes -> str (`M2W_PyString`); Python -> C,
//    str -> code-page bytes (`M2W_PyBytes`). With the plain UTF-8
//    substitution two things broke at once: `imeGetText` and
//    `GetTextFileLine` returned 1250 bytes that Python rejected as bad
//    UTF-8 (Polish chat stopped at the first accented letter, loading step
//    98 failed), and language strings went to the drawing as UTF-8 and came
//    out as two junk characters per letter.

#pragma once

#include <Python.h>
#include <frameobject.h>

// ---------------------------------------------------------------------------
// Execution frame - `PyFrameObject`
// ---------------------------------------------------------------------------
// In Python 2 `PyFrameObject` was a plain struct and TMP4 reaches into its
// fields directly: `f->f_code->co_name`, `f->f_lineno`, `f->f_lasti`. In 3
// the struct is OPAQUE and the same things come from functions. THE TRAP:
// `PyFrame_GetCode` returns a NEW REFERENCE, not a borrowed one. Rewriting
// `f->f_code` as `PyFrame_GetCode(f)` one to one would compile and work -
// and leak a code object at EVERY traced call; in trace mode that is a leak
// on every line of script. So the bridge gives no macro but a small guard:
// it takes the reference once, returns it in the destructor, and computes
// the line number on the way (`PyFrame_GetLineNumber` does what TMP4
// computed by hand with `PyCode_Addr2Line(f->f_code, f->f_lasti)`).

/// Guard over the frame's code object; used by the trace hook in
/// `ScriptLib/PythonLauncher.cpp` (patched in `tools/stage_port.py`).
class CM2wPythonFrame
{
public:
    /// Takes a new reference to the frame's code object and reads its current
    /// line; a NULL frame gives no code and line 0.
    explicit CM2wPythonFrame(PyFrameObject* pFrame)
        : m_pCode(pFrame ? PyFrame_GetCode(pFrame) : NULL),
          m_iLineNumber(pFrame ? PyFrame_GetLineNumber(pFrame) : 0)
    {
    }

    /// Releases the code object reference.
    ~CM2wPythonFrame()
    {
        Py_XDECREF(m_pCode);
    }

    /// The frame's code object. The reference belongs to the guard - do
    /// NOT release it.
    PyCodeObject* Code() const { return m_pCode; }

    /// The line being executed.
    int LineNumber() const { return m_iLineNumber; }

    /// File name and function name as plain strings. They return "?"
    /// instead of a null pointer: these go straight into `snprintf` in the
    /// exception trace, and a null pointer would turn the error report
    /// into a second error.
    const char* FileName() const { return Utf8(m_pCode ? m_pCode->co_filename : NULL); }
    /// The function name, or "?" (see `FileName`).
    const char* FunctionName() const { return Utf8(m_pCode ? m_pCode->co_name : NULL); }

private:
    /// Not copyable: the guard owns one reference.
    CM2wPythonFrame(const CM2wPythonFrame&);
    /// Not assignable (declared, never defined).
    CM2wPythonFrame& operator=(const CM2wPythonFrame&);

    /// `PyUnicode_AsUTF8` sets an exception on failure; a trace is no
    /// place to set a second one, so it is cleared and "?" returned.
    static const char* Utf8(PyObject* pObject)
    {
        if (!pObject)
            return "?";
        const char* c_sz = PyUnicode_AsUTF8(pObject);
        if (!c_sz)
        {
            PyErr_Clear();
            return "?";
        }
        return c_sz;
    }

    PyCodeObject* m_pCode;
    int m_iLineNumber;
};

// ---------------------------------------------------------------------------
// `.pyc` header
// ---------------------------------------------------------------------------

/// Words before the code object in a `.pyc`: TWO in Python 2 (magic
/// number, source mtime), FOUR since 3.7 (magic, bit field, mtime, source
/// size). `CPythonLauncher::RunCompiledFile` read two - in 3 it would start
/// unmarshalling eight bytes early and marshal would report "bad marshal
/// data" with nothing pointing at the header.
#define M2W_PYC_HEADER_WORDS 4

// ---------------------------------------------------------------------------
// Integers
// ---------------------------------------------------------------------------
// In 2 `int` and `long` were separate types; in 3 one remains. The
// substitution is exact, no traps.
#ifndef PyInt_FromLong
#define PyInt_FromLong   PyLong_FromLong
#define PyInt_AsLong     PyLong_AsLong
#define PyInt_Check      PyLong_Check
#define PyInt_CheckExact PyLong_CheckExact
#define PyInt_AS_LONG    PyLong_AsLong
#endif

// ---------------------------------------------------------------------------
// Strings - SEE DIFFERENCE 3 IN THE HEADER
// ---------------------------------------------------------------------------
// `Py_BuildValue` with `s` goes through `M2W_BuildValue` - the same format,
// only strings decoded in the code page. Every format TMP4 uses is handled
// (measured with grep: i b h l k n f d s z s# O N c and tuple/list
// brackets); an unknown format letter is an error printed once.
#ifndef PyString_FromString
/// Code-page bytes -> str (`PyString_FromString`).
PyObject* M2W_PyString(const char* c_sz);
/// Code-page bytes of length `n` (-1 = NUL-terminated) -> str.
PyObject* M2W_PyStringN(const char* c_sz, Py_ssize_t n);
/// str -> code-page bytes (`PyString_AsString`); the buffer lives in a
/// ring of the last 256 conversions - TMP4 uses it at once.
char* M2W_PyBytes(PyObject* o);
/// `Py_BuildValue` with strings decoded in the code page.
PyObject* M2W_BuildValue(const char* c_szFormat, ...);
/// The code page of the border: the game localisation's, else 1252.
unsigned M2W_BridgeCodePage();

#define PyString_FromString        M2W_PyString
#define PyString_FromStringAndSize M2W_PyStringN
#define PyString_InternFromString  PyUnicode_InternFromString
#define PyString_Check             PyUnicode_Check
#define PyString_CheckExact        PyUnicode_CheckExact
/// str -> code-page bytes; see `M2W_PyBytes` for the buffer's lifetime.
#define PyString_AsString(o)       M2W_PyBytes(o)
/// The unchecked form of Python 2 - the same here.
#define PyString_AS_STRING(o)      M2W_PyBytes(o)
#define Py_BuildValue              M2W_BuildValue
#endif

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

/// The counterpart of `Py_InitModule4` from Python 2: creates the module,
/// puts it into `sys.modules` and returns it as a BORROWED reference, as
/// the original did (the reference from `PyModule_Create` is never
/// released, so the module survives even a cleared `sys.modules`, and TMP4
/// never calls `Py_DECREF` on the result - with 2 it did not have to).
PyObject* M2W_InitModule(const char* name, PyMethodDef* methods, const char* doc);

#ifndef Py_InitModule
/// The Python 2 forms, all through `M2W_InitModule` (the `self` and
/// `apiver` of `Py_InitModule4` have no Python 3 counterpart).
#define Py_InitModule(name, methods)       M2W_InitModule((name), (methods), NULL)
/// See `Py_InitModule`.
#define Py_InitModule3(name, methods, doc) M2W_InitModule((name), (methods), (doc))
/// See `Py_InitModule`.
#define Py_InitModule4(name, methods, doc, self, apiver) \
    M2W_InitModule((name), (methods), (doc))
#endif

/// `PyErr_Print` that survives `SystemExit`: for `SystemExit`
/// CPython's `PyErr_Print` calls `Py_Exit` - finalises the interpreter and
/// ends the process. In the browser the "process" is the frame loop, which
/// goes on: the window objects are dead, `UI::CWindow::m_poHandler` dangles
/// and the next mouse move is `memory access out of bounds` (seen after
/// `exception.Abort()` -> `sys.exit()` after a missing locale
/// key). Here `SystemExit` ends the main loop through `PostQuitMessage`
/// (as `CPythonApplication::Abort` does); every other error prints as
/// before. Called from `ScriptLib/PythonUtils.cpp` (patched).
void M2W_PrintPythonError();
