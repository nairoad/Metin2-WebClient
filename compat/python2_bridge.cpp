// SPDX-License-Identifier: GPL-2.0-or-later
// python2_bridge.cpp - the bridge functions that do something at run time:
// module registration in `sys.modules`, strings across the code-page
// border, `Py_BuildValue`, and `PyErr_Print` that survives `SystemExit`.

// Design: see python2_bridge.h (the three differences of meaning) - this
// file only carries what each function adds to them.

#include "python2_bridge.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

/// `UserInterface/Locale.cpp`: the code page of the game localisation.
unsigned int LocaleService_GetCodePage();

/// `platform_none.cpp`: ends the main loop.
void PostQuitMessage(int);

namespace
{

/// Module definitions must live as long as the module is in `sys.modules`
/// - the whole life of the program. Kept here instead of freed; a
/// deliberate decision, difference 2 in the header.
std::vector<PyModuleDef*>& ModuleDefs()
{
    static std::vector<PyModuleDef*> s_vecDefs;
    return s_vecDefs;
}

/// The codec name for the border code page, `"cp1250"`; recomputed only
/// when the page changes.
const char* EncodingName()
{
    static char s_szName[16];
    static unsigned s_uLast = 0;
    const unsigned u = M2W_BridgeCodePage();
    if (u != s_uLast)
    {
        s_uLast = u;
        std::snprintf(s_szName, sizeof(s_szName), "cp%u", u);
    }
    return s_szName;
}

/// Byte buffers handed to C by `M2W_PyBytes` must live while the caller
/// uses them. TMP4 uses them at once (copies into a std::string or passes
/// them on), so a ring of the last 256 is enough with margin.
std::deque<PyObject*>& BytesRing()
{
    static std::deque<PyObject*> s_ring;
    return s_ring;
}

/// One format element. Returns a NEW reference, or NULL with an exception.
PyObject* BuildOne(const char** ppFormat, va_list* pArgs);

/// Tuple or list: collects elements up to the closing bracket `cClose`.
PyObject* BuildSequence(const char** ppFormat, va_list* pArgs, char cClose)
{
    std::vector<PyObject*> vec;
    for (;;)
    {
        const char c = **ppFormat;
        if (c == 0 || c == cClose)
            break;
        if (c == ' ' || c == ',' || c == ':' || c == '\t')
        {
            ++*ppFormat;
            continue;
        }
        PyObject* p = BuildOne(ppFormat, pArgs);
        if (!p)
        {
            for (size_t i = 0; i < vec.size(); ++i) Py_DECREF(vec[i]);
            return NULL;
        }
        vec.push_back(p);
    }
    PyObject* pResult = (cClose == ']') ? PyList_New((Py_ssize_t)vec.size())
                                        : PyTuple_New((Py_ssize_t)vec.size());
    if (!pResult)
    {
        for (size_t i = 0; i < vec.size(); ++i) Py_DECREF(vec[i]);
        return NULL;
    }
    for (size_t i = 0; i < vec.size(); ++i)
    {
        if (cClose == ']') PyList_SET_ITEM(pResult, (Py_ssize_t)i, vec[i]);
        else PyTuple_SET_ITEM(pResult, (Py_ssize_t)i, vec[i]);
    }
    return pResult;
}

PyObject* BuildOne(const char** ppFormat, va_list* pArgs)
{
    const char c = *(*ppFormat)++;
    switch (c)
    {
        case '(': { PyObject* p = BuildSequence(ppFormat, pArgs, ')'); if (**ppFormat == ')') ++*ppFormat; return p; }
        case '[': { PyObject* p = BuildSequence(ppFormat, pArgs, ']'); if (**ppFormat == ']') ++*ppFormat; return p; }
        case 'i': case 'b': case 'h': return PyLong_FromLong(va_arg(*pArgs, int));
        case 'B': case 'H': case 'I': return PyLong_FromUnsignedLong(va_arg(*pArgs, unsigned int));
        case 'l': return PyLong_FromLong(va_arg(*pArgs, long));
        case 'k': return PyLong_FromUnsignedLong(va_arg(*pArgs, unsigned long));
        case 'L': return PyLong_FromLongLong(va_arg(*pArgs, long long));
        case 'K': return PyLong_FromUnsignedLongLong(va_arg(*pArgs, unsigned long long));
        case 'n': return PyLong_FromSsize_t(va_arg(*pArgs, Py_ssize_t));
        case 'f': case 'd': return PyFloat_FromDouble(va_arg(*pArgs, double));
        case 'c': { char szChar[2] = { (char)va_arg(*pArgs, int), 0 }; return M2W_PyStringN(szChar, 1); }
        case 'O': case 'S':
        {
            PyObject* p = va_arg(*pArgs, PyObject*);
            if (**ppFormat == '&')
            {
                // `O&` - a converter; TMP4 does not use it (measured), but
                // honestly: no pretending to.
                ++*ppFormat;
                (void)va_arg(*pArgs, void*);
                PyErr_SetString(PyExc_SystemError, "M2W_BuildValue: O& not supported");
                return NULL;
            }
            if (!p) { PyErr_SetString(PyExc_SystemError, "M2W_BuildValue: NULL for O"); return NULL; }
            Py_INCREF(p);
            return p;
        }
        case 'N':
        {
            PyObject* p = va_arg(*pArgs, PyObject*);
            if (!p) { PyErr_SetString(PyExc_SystemError, "M2W_BuildValue: NULL for N"); return NULL; }
            return p;
        }
        case 's': case 'z': case 'y': case 'U':
        {
            const char* c_sz = va_arg(*pArgs, const char*);
            Py_ssize_t n = -1;
            if (**ppFormat == '#')
            {
                ++*ppFormat;
                n = va_arg(*pArgs, Py_ssize_t);
            }
            if (!c_sz)
            {
                Py_INCREF(Py_None);
                return Py_None;
            }
            return M2W_PyStringN(c_sz, n);
        }
        default:
        {
            static int s_iTimes = 0;
            if (s_iTimes++ < 10)
                std::printf("m2w bridge: unknown Py_BuildValue format letter: '%c'\n", c);
            PyErr_Format(PyExc_SystemError, "M2W_BuildValue: unknown format '%c'", c);
            return NULL;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

PyObject* M2W_InitModule(const char* name, PyMethodDef* methods, const char* doc)
{
    // `PyModuleDef_HEAD_INIT` initialises the object header; the other
    // fields must be zero, hence the value-initialising `new PyModuleDef()`.
    PyModuleDef* pDef = new PyModuleDef();
    pDef->m_base = PyModuleDef_HEAD_INIT;
    pDef->m_name = name;
    pDef->m_doc  = doc;
    // `-1` means "the module does not support sub-interpreters and keeps
    // its state globally" - exactly how Python 2 modules worked.
    pDef->m_size    = -1;
    pDef->m_methods = methods;

    ModuleDefs().push_back(pDef);

    PyObject* pModule = PyModule_Create(pDef);
    if (!pModule) {
        return NULL;
    }

    // THE HEART OF THE BRIDGE. `Py_InitModule` from 2 put the module into
    // `sys.modules`; `PyModule_Create` from 3 DOES NOT. Without this line
    // everything would compile and run, and `import ui` in a game script
    // would be the first to say the module does not exist.
    PyObject* pModules = PyImport_GetModuleDict();
    if (!pModules || PyDict_SetItemString(pModules, name, pModule) < 0) {
        Py_DECREF(pModule);
        return NULL;
    }

    // A BORROWED reference is returned, as the original did: `sys.modules`
    // holds one count and the count from `PyModule_Create` is never
    // released, so the object cannot vanish; TMP4 never calls `Py_DECREF`
    // on it - with 2 it did not have to.
    return pModule;
}

// ---------------------------------------------------------------------------
// The code page at the Python border - see the header
// ---------------------------------------------------------------------------

unsigned M2W_BridgeCodePage()
{
    const unsigned u = LocaleService_GetCodePage();
    return u ? u : 1252;
}

PyObject* M2W_PyStringN(const char* c_sz, Py_ssize_t n)
{
    if (!c_sz)
        return PyUnicode_FromStringAndSize(NULL, n);
    if (n < 0)
        n = (Py_ssize_t)std::strlen(c_sz);
    // Pure ASCII - no codec; the vast majority of calls.
    bool bAscii = true;
    for (Py_ssize_t i = 0; i < n; ++i)
        if ((unsigned char)c_sz[i] >= 0x80) { bAscii = false; break; }
    if (bAscii)
        return PyUnicode_FromStringAndSize(c_sz, n);
    return PyUnicode_Decode(c_sz, n, EncodingName(), "replace");
}

PyObject* M2W_PyString(const char* c_sz)
{
    return M2W_PyStringN(c_sz, -1);
}

char* M2W_PyBytes(PyObject* o)
{
    if (!o)
        return NULL;
    if (PyBytes_Check(o))
        return PyBytes_AsString(o);
    if (!PyUnicode_Check(o))
        return NULL;
    // Pure ASCII - the object's UTF-8 buffer is then the same as the code
    // page, and it lives with the object.
    if (PyUnicode_IS_ASCII(o))
        return const_cast<char*>(PyUnicode_AsUTF8(o));

    PyObject* pBytes = PyUnicode_AsEncodedString(o, EncodingName(), "replace");
    if (!pBytes)
    {
        PyErr_Clear();
        return const_cast<char*>(PyUnicode_AsUTF8(o));
    }
    std::deque<PyObject*>& q = BytesRing();
    q.push_back(pBytes);
    while (q.size() > 256)
    {
        Py_DECREF(q.front());
        q.pop_front();
    }
    return PyBytes_AsString(pBytes);
}

PyObject* M2W_BuildValue(const char* c_szFormat, ...)
{
    va_list args;
    va_start(args, c_szFormat);
    const char* p = c_szFormat ? c_szFormat : "";

    // `Py_BuildValue("None")` in `PythonSkill.cpp:1625` - in CPython 'N'
    // would eat a random pointer. Here it means what TMP4's author meant.
    if (std::strcmp(p, "None") == 0)
    {
        va_end(args);
        Py_INCREF(Py_None);
        return Py_None;
    }

    // As in CPython: one element without brackets is itself, zero elements
    // is None, more than one a tuple.
    std::vector<PyObject*> vec;
    PyObject* pResult = NULL;
    bool bError = false;
    while (*p)
    {
        if (*p == ' ' || *p == ',' || *p == ':' || *p == '\t') { ++p; continue; }
        PyObject* pElement = BuildOne(&p, &args);
        if (!pElement) { bError = true; break; }
        vec.push_back(pElement);
    }
    va_end(args);

    if (bError)
    {
        for (size_t i = 0; i < vec.size(); ++i) Py_DECREF(vec[i]);
        return NULL;
    }
    if (vec.empty())
    {
        Py_INCREF(Py_None);
        return Py_None;
    }
    if (vec.size() == 1)
        return vec[0];
    pResult = PyTuple_New((Py_ssize_t)vec.size());
    if (!pResult)
    {
        for (size_t i = 0; i < vec.size(); ++i) Py_DECREF(vec[i]);
        return NULL;
    }
    for (size_t i = 0; i < vec.size(); ++i)
        PyTuple_SET_ITEM(pResult, (Py_ssize_t)i, vec[i]);
    return pResult;
}

// ---------------------------------------------------------------------------
// Python error after a callback - see the header
// ---------------------------------------------------------------------------

void M2W_PrintPythonError()
{
    if (PyErr_ExceptionMatches(PyExc_SystemExit))
    {
        PyErr_Clear();
        std::printf("m2w: SystemExit from a script - ending the main loop instead of the interpreter\n");
        PostQuitMessage(0);
        return;
    }
    PyErr_Print();
}
