#!/usr/bin/env python
"""stage_port.py - assembles the port's source tree from two parts.

WHY this is needed:
`GameLib` pulls in its dependencies with RELATIVE paths (`../eterLib/Pool.h`),
which resolve relative to the directory of the including file. No order
of `-I` overrides that. To substitute our own headers for `EterLib`,
our directories have to **lie next to** `GameLib` in one tree.

That is why the tree is ASSEMBLED, not kept in the repository:

    reference/tmp4_source/source/<lib>   ->  copied unchanged
    compat/tree/<lib>                    ->  OURS, covers the copy

The repository holds only what we wrote ourselves. We do not duplicate
or edit the TMP4 sources - both rules from `build/port/README.md` stay.

Running:
    python tools/stage_port.py [target_directory]
"""

import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import workspace                                     # input paths, emsdk
TMP4 = workspace.SOURCE_LIBRARIES
OURS = workspace.TREE
DEFAULT_STAGE = os.path.join(ROOT, 'build', 'port', 'stage')

# Everything comes from TMP4 first, and our directories COVER single
# files. Thanks to that the repository holds only the DIFFERENCE, and its size is
# a measurement in itself: how many `EterLib` headers really had to be replaced.
#
# `EterBase` and `EterPack` **run** under emscripten,
# so they have nothing to cover.
FROM_TMP4 = [
    'GameLib', 'eterBase', 'EterPack', 'eterLocale',
    'eterLib', 'eterGrnLib', 'milesLib', 'effectLib',
    'PRTerrainLib', 'SpeedTreeLib', 'EterImageLib', 'SphereLib', 'ScriptLib',
    'UserInterface', 'EterPythonLib', 'CWebBrowser', 'Discord',
]


# ---------------------------------------------------------------------------
# POINT PATCHES
# ---------------------------------------------------------------------------
# There are things that CANNOT be handled by the header layer or by covering a
# file: they sit in the content of a TMP4 header, which `#pragma once` makes
# impossible to replace from outside. Instead of pulling the whole file into the repository, I keep
# **only the difference** here - visible, short and justified.
#
# Every patch MUST hit. If the `old` pattern is not found, the script
# stops - better to stop loudly than to silently assemble a tree that
# is not what I think it is.
OLD_PY_BUILDEXCEPTION = '\t\tPyErr_SetString(PyExc_RuntimeError, szErrBuf);\n\t}\n\n\treturn Py_BuildNone();\n\t//return NULL;'

NEW_PY_BUILDEXCEPTION = '\t\tPyErr_SetString(PyExc_RuntimeError, szErrBuf);\n\n\t\t// PORT: A FUNCTION THAT SET AN EXCEPTION MUST RETURN NULL.\n\t\t//\n\t\t// That is what the source said - with `return NULL;` hidden in a comment\n\t\t// right next to it. The authors knew it was not right, and under\n\t\t// Python 2 it got through: an exception set with a non-null result was\n\t\t// deferred there and blew up later, somewhere unrelated to the cause.\n\t\t//\n\t\t// Python 3 does not allow it and says so plainly:\n\t\t//\n\t\t//     SystemError: <built-in function Generate> returned a result\n\t\t//                  with an exception set\n\t\t//\n\t\t// Fifteen such lines in the log came from ONE missing\n\t\t// cursor texture - the message said nothing about the cursor or\n\t\t// the texture, only about the function that happened to be returning.\n\t\t//\n\t\t// After this change `grpImage.Generate` raises `RuntimeError`, and\n\t\t// `mouseModule.CursorImage.LoadImage` has a ready `except` for it\n\t\t// and sets the handle to zero. That is how it was meant to work.\n\t\treturn NULL;\n\t}\n\n\treturn Py_BuildNone();'

OLD_GETINTEGER = 'bool PyTuple_GetInteger(PyObject* poArgs, int pos, int* ret)\n{\n\tif (pos >= PyTuple_Size(poArgs))\n\t\treturn false;\n\n\tPyObject* poItem = PyTuple_GetItem(poArgs, pos);\n\t\n\tif (!poItem)\n\t\treturn false;\n\t\n\t*ret = PyLong_AsLong(poItem);\n\treturn true;\n}'

NEW_GETINTEGER = 'bool PyTuple_GetInteger(PyObject* poArgs, int pos, int* ret)\n{\n\tif (pos >= PyTuple_Size(poArgs))\n\t\treturn false;\n\n\tPyObject* poItem = PyTuple_GetItem(poArgs, pos);\n\t\n\tif (!poItem)\n\t\treturn false;\n\n\t// PORT: IN PYTHON 2 THIS BRIDGE ACCEPTED A FRACTION. IT STILL DOES.\n\t//\n\t// `PyInt_AsLong` in Python 2 took any object with an `__int__` method\n\t// - `float` too - and TRUNCATED it. `PyLong_AsLong` in Python 3 does not\n\t// do that: it sets `TypeError` and returns -1.\n\t//\n\t// And this function DID NOT CHECK the error and reported SUCCESS. The caller\n\t// went on with the value -1 and an exception set, and Python\n\t// reported it only on return from the binding:\n\t//\n\t//     SystemError: <built-in function SetWindowPosition> returned\n\t//                  a result with an exception set\n\t//\n\t// The real cause was two layers lower:\n\t//\n\t//     textLine.SetPosition(self.GetWidth()/2, self.GetHeight()/2)\n\t//\n\t// i.e. a DIVISION, which in Python 3 gives a fraction.\n\t//\n\t// The divisions themselves are settled in the scripts\n\t// (`tools/division.py`, 215 turned into `//`), but that rule does not\n\t// settle everything and cannot. So the bridge\n\t// behaves as it did in Python 2: it TRUNCATES the fraction.\n\t// That is not leniency, but reproducing the behaviour\n\t// of the platform this code was written for.\n\t//\n\t// And a second change, independent of the first: when the conversion\n\t// FAILS, the function says `false`. Reporting success with\n\t// an exception set was the same error as\n\t// `Py_BuildException` returning `Py_None`.\n\tif (PyFloat_Check(poItem))\n\t{\n\t\t*ret = static_cast<int>(PyFloat_AsDouble(poItem));\n\t\treturn true;\n\t}\n\n\t*ret = PyLong_AsLong(poItem);\n\tif (*ret == -1 && PyErr_Occurred())\n\t{\n\t\tPyErr_Clear();\n\t\treturn false;\n\t}\n\treturn true;\n}'

OLD_GETLONG = 'bool PyTuple_GetLong(PyObject* poArgs, int pos, long* ret)\n{\n\tif (pos >= PyTuple_Size(poArgs))\n\t\treturn false;\n\n\tPyObject* poItem = PyTuple_GetItem(poArgs, pos);\n\n\tif (!poItem)\n\t\treturn false;\n\n\t*ret = PyLong_AsLong(poItem);\n\treturn true;\n}'

NEW_GETLONG = 'bool PyTuple_GetLong(PyObject* poArgs, int pos, long* ret)\n{\n\tif (pos >= PyTuple_Size(poArgs))\n\t\treturn false;\n\n\tPyObject* poItem = PyTuple_GetItem(poArgs, pos);\n\n\tif (!poItem)\n\t\treturn false;\n\n\t// The same bridge, the same change - the reasoning is at\n\t// `PyTuple_GetInteger` below. I do not leave the non-robust\n\t// variant here just because the symptom happened to show on that one:\n\t// both functions are called by the same bindings, for the same\n\t// coordinates.\n\tif (PyFloat_Check(poItem))\n\t{\n\t\t*ret = static_cast<long>(PyFloat_AsDouble(poItem));\n\t\treturn true;\n\t}\n\n\t*ret = PyLong_AsLong(poItem);\n\tif (*ret == -1 && PyErr_Occurred())\n\t{\n\t\tPyErr_Clear();\n\t\treturn false;\n\t}\n\treturn true;\n}'

REASON_SELECT = (
    (
    'THE FIRST ARGUMENT OF `select` - WINSOCK IGNORES IT, POSIX DOES NOT. '
    'Microsoft keeps `nfds` only for compatibility with Berkeley and never '
    'reads it, so the TMP4 code passes ZERO there. In POSIX this number means'
    ' "check descriptors 0 to nfds-1", so zero means CHECK NONE. Symptom (cz.'
    ' 207): the connection to the game server IS ESTABLISHED - listening on '
    'the WebSocket shows an open socket to the bridge - but `FD_ISSET(m_sock,'
    ' &fdsSend)` is never true, so `CNetworkStream::Process` waits until '
    '`m_connectLimitTime` runs out and reports `OnConnectFailure`. The user '
    'sees "Error while connecting to the server", and `syserr.txt` has NO '
    'error at all - because formally nothing broke: nobody asked about '
    'anything. I fix it at the call site, not with a stub for `select` in the'
    ' compatibility layer, because CPython uses `select` too - an own '
    'implementation of this primitive would reach the whole binary, while the'
    ' fault reaches two lines.'
)
)

PATCHES = [
    (
        'EterLib/Camera.cpp',
        'void CCamera::BeginDrag(int nMouseX, int nMouseY)\n{\n\tif (IsLock())\n\t\treturn;\n\n\tm_bDrag = true;\n',
        'extern "C" void M2W_CameraDrag(int bOn);  // PORT, compat/events_web.cpp\n\n'
        'void CCamera::BeginDrag(int nMouseX, int nMouseY)\n{\n\tif (IsLock())\n\t\treturn;\n\n\tm_bDrag = true;\n'
        '\t// PORT: the page locks the pointer for a camera drag - and only for\n'
        '\t// one: a right press on an inventory slot is not a turn. Here, not\n'
        '\t// at `SetCursorVisible`, because the callers hide the cursor only in\n'
        '\t// the hardware cursor mode (PythonPlayerInputMouse.cpp).\n'
        '\tM2W_CameraDrag(1);\n',
        'PORT: the page learns when a camera drag begins (pointer lock only '
        'for a real camera turn, not for every right press)',
    ),
    (
        'EterLib/Camera.cpp',
        'bool CCamera::EndDrag()\n{\n\tif (IsLock())\n\t\treturn false;\n\n\tm_bDrag = false;\n',
        'bool CCamera::EndDrag()\n{\n\tif (IsLock())\n\t\treturn false;\n\n\tm_bDrag = false;\n'
        '\tM2W_CameraDrag(0);  // PORT: the drag ended - the page lets the lock go\n',
        'PORT: the page learns when a camera drag ends',
    ),
    (
        'UserInterface/PythonSystem.cpp',
        "bool CPythonSystem::IsSoftwareCursor()\n{\n\treturn m_Config.is_software_cursor;\n}",
        "extern \"C\" int M2W_UseSoftwareCursor(int bFromConfig);  // PORT, compat/platform_none.cpp\n"
        "bool CPythonSystem::IsSoftwareCursor()\n{\n"
        "\t// PORT: `?cursor=software|hardware` in the page URL wins\n"
        "\t// over metin2.cfg. Software cursor = a page has no hardware cursor for\n"
        "\t// the game - the browser\n"
        "\t// cursor hidden for good, the game draws it, so the browser moving\n"
        "\t// the cursor during Pointer Lock has nothing to show.\n"
        "\treturn M2W_UseSoftwareCursor(m_Config.is_software_cursor ? 1 : 0) != 0;\n}",
        'PORT: cursor mode from the page URL (?cursor=software)',
    ),
    (
        'ScriptLib/PythonUtils.cpp',
        "\t\tPyErr_Print();\n",
        "\t\tM2W_PrintPythonError();  // PORT: SystemExit -> PostQuitMessage, not Py_Exit\n",
        'PORT: sys.exit from a script ends the main loop instead of '
        'finalizing the interpreter in the middle of a frame',
        3,
    ),
    (
        'ScriptLib/PythonUtils.cpp',
        "#include \"PythonUtils.h\"\n",
        "#include \"PythonUtils.h\"\nvoid M2W_PrintPythonError();  // PORT, compat/python2_bridge.cpp\n",
        'PORT: declaration of M2W_PrintPythonError',
    ),
    (
        'EterPythonLib/PythonWindowManagerModule.cpp',
        "bool PyTuple_GetWindow(PyObject* poArgs, int pos, UI::CWindow ** ppRetWindow)\n{\n\tint iHandle;\n\tif (!PyTuple_GetInteger(poArgs, pos, &iHandle))\n\t\treturn false;\n\tif (!iHandle)\n\t\treturn false;\n\n\t*ppRetWindow = (UI::CWindow*)iHandle;\n\treturn true;\n}",
        "// PORT: A WINDOW HANDLE IS A NUMBER WITH A GENERATION, NOT A POINTER.\n// The original gave Python a raw pointer as an int, and `ui.Window.__del__`\n// calls `wndMgr.Destroy(hWnd)` also for an already destroyed window -> a second\n// `delete` of the same block (measured: abort() in free from ~CWindow on\n// a map change, monkey dungeon 3). A set of live pointers alone is not enough:\n// after window A is freed the allocator gives that address to window B and a dangling handle A\n// would destroy B (reviewer). Handle = (slot number << 8) | generation;\n// the generation grows on every reuse of a slot, so an old handle\n// never hits a new window. 0 stays as `no window`.\n#include <vector>\nnamespace {\nstruct TWindowSlot { UI::CWindow* pWindow; unsigned uGeneration; };\nstd::vector<TWindowSlot> g_aSlots(1);  // [0] unused - handle 0 = none\nstd::vector<int> g_aFree;\nunsigned g_uRejected = 0;\n}\nint M2W_WindowHandle(UI::CWindow* pWindow)\n{\n\tint i;\n\tif (!g_aFree.empty()) { i = g_aFree.back(); g_aFree.pop_back(); }\n\telse { i = (int)g_aSlots.size(); g_aSlots.push_back(TWindowSlot()); g_aSlots[i].uGeneration = 0; }\n\tg_aSlots[i].pWindow = pWindow;\n\tg_aSlots[i].uGeneration = (g_aSlots[i].uGeneration + 1) & 0xFF;\n\tif (g_aSlots[i].uGeneration == 0) g_aSlots[i].uGeneration = 1;\n\treturn (i << 8) | (int)g_aSlots[i].uGeneration;\n}\nstatic UI::CWindow* M2W_WindowFromHandle(int iHandle)\n{\n\tconst int i = iHandle >> 8; const unsigned uGen = (unsigned)(iHandle & 0xFF);\n\tif (i <= 0 || i >= (int)g_aSlots.size() || !g_aSlots[i].pWindow || g_aSlots[i].uGeneration != uGen)\n\t\treturn NULL;\n\treturn g_aSlots[i].pWindow;\n}\n/// Frees the slot of the handle when a window is destroyed. Returns false when the handle\n/// is already invalid (a double Destroy or a dangling handle) - without an exception,\n/// because that is the normal path of `__del__` after an explicit destruction.\nstatic bool M2W_ReleaseHandle(int iHandle)\n{\n\tconst int i = iHandle >> 8;\n\tif (!M2W_WindowFromHandle(iHandle)) { ++g_uRejected; if (g_uRejected <= 5 || (g_uRejected % 1000) == 0) printf(\"m2w windows: Destroy of an invalid handle %d (rejected %u)\\n\", iHandle, g_uRejected); return false; }\n\tg_aSlots[i].pWindow = NULL; g_aFree.push_back(i);\n\treturn true;\n}\nbool PyTuple_GetWindow(PyObject* poArgs, int pos, UI::CWindow ** ppRetWindow)\n{\n\tint iHandle;\n\tif (!PyTuple_GetInteger(poArgs, pos, &iHandle))\n\t\treturn false;\n\tif (!iHandle)\n\t\treturn false;\n\n\t*ppRetWindow = M2W_WindowFromHandle(iHandle);\n\treturn *ppRetWindow != NULL;\n}",
        'PORT: a window handle = a number with a generation, not a raw'
        ' pointer (double delete on a map change)',
    ),
    (
        'EterPythonLib/PythonWindowManagerModule.cpp',
        "\treturn Py_BuildValue(\"i\", pWindow);",
        "\treturn Py_BuildValue(\"i\", M2W_WindowHandle(pWindow));  // PORT",
        'PORT: Register* return a handle from the table',
        17,
    ),
    (
        'EterPythonLib/PythonWindowManagerModule.cpp',
        "PyObject * wndMgrDestroy(PyObject * poSelf, PyObject * poArgs)\n{\n\tUI::CWindow * pWin;\n\tif (!PyTuple_GetWindow(poArgs, 0, &pWin))\n\t\treturn Py_BuildException();\n\n\tUI::CWindowManager::Instance().DestroyWindow(pWin);\n\treturn Py_BuildNone();\n}",
        "PyObject * wndMgrDestroy(PyObject * poSelf, PyObject * poArgs)\n{\n\t// PORT: invalid handle = already destroyed, silently (see PyTuple_GetWindow).\n\tint iHandle;\n\tif (!PyTuple_GetInteger(poArgs, 0, &iHandle) || !iHandle)\n\t\treturn Py_BuildNone();\n\tUI::CWindow * pWin = M2W_WindowFromHandle(iHandle);\n\tif (!M2W_ReleaseHandle(iHandle) || !pWin)\n\t\treturn Py_BuildNone();\n\n\tUI::CWindowManager::Instance().DestroyWindow(pWin);\n\treturn Py_BuildNone();\n}",
        'PORT: Destroy frees the handle and ignores an invalid one',
    ),
    (
        'UserInterface/InstanceBase.cpp',
        "\t\tm_GraphicThingInstance.SetAlphaValue(0.0f);\n\t\tm_GraphicThingInstance.BlendAlphaValue(1.0f, 0.5f);\n\t}",
        "\t\t// PORT (decision of the user - a departure from 1:1): NO alpha\n\t\t// fade-in when creating characters (own and of others). Original: alpha 0 -> 1\n\t\t// over 0.5 s (`BlendAlphaValue(1.0f, 0.5f)`); the user: \"it looks\n\t\t// better without\". The code kept in a comment, so one can go back:\n\t\t//   m_GraphicThingInstance.SetAlphaValue(0.0f);\n\t\t//   m_GraphicThingInstance.BlendAlphaValue(1.0f, 0.5f);\n\t\tm_GraphicThingInstance.SetAlphaValue(1.0f);\n\t}",
        "PORT: no alpha fade-in when creating characters - the user's "
        'decision',
    ),
    (
        'UserInterface/PythonApplicationEvent.cpp',
        "void CPythonApplication::OnSizeChange(int width, int height)\n{\t\n}",
        "void CPythonApplication::OnSizeChange(int width, int height)\n{\n\t// PORT: EMPTY in the original - a Windows window did not change\n\t// size on the fly. In the browser it does (panel, full screen).\n\t// The buffer and the viewport follow the window through ResizeBackBuffer/Reset; here\n\t// we tell the interface the new screen size (window layers,\n\t// GetScreenWidth/Height for windows created later).\n\tif (width > 0 && height > 0)\n\t{\n\t\tUI::CWindowManager& rkWndMgr = UI::CWindowManager::Instance();\n\t\trkWndMgr.SetResolution(width, height);\n\t\trkWndMgr.SetScreenSize(width, height);\n\t}\n}",
        'OnSizeChange: the interface learns about the new screen size - round '
        '300h',
    ),
    (
        'EterLib/GrpDevice.cpp',
        "\t\t\tSTATEMANAGER.SetDefaultState();\n\t\t}\n\t}\n\n\treturn true;",
        "\t\t\tSTATEMANAGER.SetDefaultState();\n\t\t\t// PORT: the screen size of the graphics layer also follows the buffer\n\t\t\t// (SetOrtho2D in ScreenFilter, proportions) - set once in the original.\n\t\t\tms_iWidth = (int)uWidth;\n\t\t\tms_iHeight = (int)uHeight;\n\t\t}\n\t}\n\n\treturn true;",
        'ResizeBackBuffer: ms_iWidth/ms_iHeight follow the buffer',
    ),
    (
        'EterGrnLib/Thing.cpp',
        "\tm_pgrnFile = GrannyReadEntireFileFromMemory(iSize, (void *) c_pvBuf);\n\n\tif (!m_pgrnFile)\n\t\treturn false;\n",
        "\tm_pgrnFile = GrannyReadEntireFileFromMemory(iSize, (void *) c_pvBuf);\n\n\tif (!m_pgrnFile)\n\t{\n\t\t// PORT: the .gr2 reader reports a refusal, but does not know the file\n\t\t// name - only the resource knows it. Without this one cannot tie\n\t\t// \"rejected\" to a specific model or animation.\n\t\tprintf(\"m2w granny: REJECTED file [%s] (%d B)\\n\", GetFileName(), iSize);\n\t\treturn false;\n\t}\n",
        'name of the .gr2 file when the reader refuses',
    ),
    (
        'UserInterface/Packet.h',
        "typedef struct SPacketGCTime\n{\n    BYTE        bHeader;\n    time_t      time;\n} TPacketGCTime;",
        "typedef struct SPacketGCTime\n{\n    BYTE        bHeader;\n    // PORT: like TPlayerSkill - the original has a 4 B time_t\n    // (_USE_32BIT_TIME_T). Measured: with 8 B the client read 9 instead of 5 B\n    // and got the time -829223660397594794 (4 bytes of the next packet).\n    DWORD       time;\n} TPacketGCTime;",
        'TPacketGCTime: time_t -> DWORD (ABI _USE_32BIT_TIME_T)',
    ),
    (
        'UserInterface/Packet.h',
        "\tBYTE bLevel;\n\ttime_t tNextRead;\n} TPlayerSkill;",
        "\tBYTE bLevel;\n\t// PORT: the original compiles with _USE_32BIT_TIME_T\n\t// (UserInterface/StdAfx.h:14) - time_t has 4 bytes there, so an entry has\n\t// 6 B, and the GC_SKILL_LEVEL_NEW packet 1531 B. In emscripten time_t has 8 B:\n\t// entry 10 B, packet 2551 B - the client ate 1020 B of the NEXT packets,\n\t// and read the levels shifted (measured: only skill 3 \"P\",\n\t// garbage from index 153 = 1530/10). A fixed-width type, as in\n\t// the original ABI.\n\tDWORD tNextRead;\n} TPlayerSkill;",
        "TPlayerSkill: time_t -> DWORD, as with the original's _USE_32BIT_TIME_T "
        '(fixes skill levels)',
    ),

    (
        'GameLib/PropertyManager.cpp',
        'bool CPropertyManager::Get(DWORD dwCRC, CProperty ** ppProperty)\n{\n\tTPropertyCRCMap::iterator itor = m_PropertyByCRCMap.find(dwCRC);\n\n\tif (m_PropertyByCRCMap.end() == itor)\n\t\treturn false;',
        'extern "C" const char* M2W_PropertyFile(unsigned long);\n\nbool CPropertyManager::Get(DWORD dwCRC, CProperty ** ppProperty)\n{\n\tTPropertyCRCMap::iterator itor = m_PropertyByCRCMap.find(dwCRC);\n\n\t// PORT: LAZY REGISTRATION. Nobody lists the property/\n\t// directory at start (1835 files in the streamed corpus);\n\t// instead a CRC -> file index (compat/properties_web.cpp), and a file\n\t// registers at the first question.\n\t//\n\tif (m_PropertyByCRCMap.end() == itor)\n\t{\n\t\tconst char* c_szFile = M2W_PropertyFile(dwCRC);\n\t\tif (c_szFile && Register(c_szFile))\n\t\t\titor = m_PropertyByCRCMap.find(dwCRC);\n\t}\n\n\tif (m_PropertyByCRCMap.end() == itor)\n\t\treturn false;',
        'map objects: Get(CRC) missed 599 times, because nobody registered the '
        'property/ directory at start; lazy registration from the CRC -> file '
        'index',
    ),

    (
        'UserInterface/PythonBackground.cpp',
        'void CPythonBackground::__CreateProperty()\n{\n\tif (CEterPackManager::SEARCH_FILE_FIRST == CEterPackManager::Instance().GetSearchMode() &&\n\t\t_access("property", 0) == 0)\n\t{\n\t\tm_PropertyManager.Initialize(NULL);\n\n\t\tCPropertyLoader PropertyLoader;\n\t\tPropertyLoader.SetPropertyManager(&m_PropertyManager);\n\t\tPropertyLoader.Create("*.*", "Property");\n\t}',
        'extern "C" int M2W_HasPropertyIndex();\n\nvoid CPythonBackground::__CreateProperty()\n{\n\t// PORT: file mode WITHOUT listing the directory - when there is\n\t// a CRC -> file index, properties register lazily in Get(CRC).\n\tif (M2W_HasPropertyIndex())\n\t{\n\t\tm_PropertyManager.Initialize(NULL);\n\t}\n\telse if (CEterPackManager::SEARCH_FILE_FIRST == CEterPackManager::Instance().GetSearchMode() &&\n\t\t_access("property", 0) == 0)\n\t{\n\t\tm_PropertyManager.Initialize(NULL);\n\n\t\tCPropertyLoader PropertyLoader;\n\t\tPropertyLoader.SetPropertyManager(&m_PropertyManager);\n\t\tPropertyLoader.Create("*.*", "Property");\n\t}',
        'properties: without the index the client went to pack/property, which '
        'does not exist; with the index, file mode without listing the directory',
    ),

    (
        'UserInterface/PythonCharacterManagerModule.cpp',
        '\tif (!pRaceData->SetMotionRandomWeight(iMode, iMotion, iSubMotion, iPercentage))\n'
        '\t\tPy_BuildException("Failed to SetMotionRandomWeight");',

        '\tif (!pRaceData->SetMotionRandomWeight(iMode, iMotion, iSubMotion, iPercentage))\n'
        '\t\t// PORT: A MISSING `return`. The third time the same shape.\n'
        '\t\t//\n'
        '\t\t// `Py_BuildException` SETS an exception and returns NULL - but here\n'
        '\t\t// its result was thrown away, and the function went on to\n'
        '\t\t// `return Py_BuildNone()`. So Python got A RESULT\n'
        '\t\t// AND AN EXCEPTION AT ONCE.\n'
        '\t\t//\n'
        '\t\t// Python 2 tolerated it, Python 3 checks strictly:\n'
        '\t\t//\n'
        '\t\t//     SystemError: <built-in function SetMotionRandomWeight>\n'
        '\t\t//                  returned a result with an exception set\n'
        '\t\t//\n'
        '\t\t// and the real message ("Failed to SetMotionRandomWeight")\n'
        '\t\t// was lost. It is the same family as `Py_BuildException` returning\n'
        '\t\t// `Py_None` and `PyTuple_GetInteger` reporting success\n'
        '\t\t// with an exception set.\n'
        '\t\t//\n'
        '\t\t// Symptom: `LoadGameData` aborted entirely\n'
        '\t\t// at step 50, and the loading window showed\n'
        '\t\t// "The file is damaged. Please install new."\n'
        '\t\treturn Py_BuildException("Failed to SetMotionRandomWeight");',

        'A MISSING `return` BEFORE `Py_BuildException`. The function set an '
        'exception, threw its result away and returned `Py_BuildNone()` - i.e. A '
        'RESULT AND AN EXCEPTION AT ONCE. Python 3 checks that strictly and turns'
        ' it into `SystemError`, so the real error message is lost. Every other '
        "branch of this function has a `return`, so the author's intent is beyond"
        ' doubt.',
    ),
    (
        'eterLib/SkyBox.cpp',
        'CSkyObject::~CSkyObject()\n{\n\tDestroy();\n}',

        'CSkyObject::~CSkyObject()\n{\n'
        '\t// PORT: A QUALIFIED CALL, BECAUSE `Destroy` IS PURE\n'
        '\t// VIRTUAL (`virtual void Destroy() = 0;` in the header).\n'
        '\t//\n'
        '\t// Calling a virtual method from a destructor hits the method table\n'
        '\t// of THIS class - and for a pure virtual method it holds\n'
        '\t// `__cxa_pure_virtual`, a function with a DIFFERENT SIGNATURE than a member\n'
        '\t// method. In wasm every indirect call has its type checked,\n'
        '\t// so this ends at once:\n'
        '\t//\n'
        '\t//     RuntimeError: function signature mismatch\n'
        '\t//       CSkyObject::~CSkyObject <- CSkyBox::~CSkyBox\n'
        '\t//       <- CMapOutdoor::~CMapOutdoor <- CPythonBackground::Destroy\n'
        '\t//       <- CPythonNetworkStream::Warp\n'
        '\t//\n'
        '\t// I.e. at ENTERING THE WORLD, when the client cleans up\n'
        '\t// the old map.\n'
        '\t//\n'
        '\t// Why it worked in the original: in a destructor the dynamic type\n'
        '\t// is known, so the compiler with optimization on turns\n'
        '\t// it into a DIRECT call and reaches for `CSkyObject::Destroy`,\n'
        '\t// which has a body (empty) right below. We build with `-O0`, so\n'
        '\t// the indirect call stays.\n'
        '\t//\n'
        '\t// So I write explicitly what happens there anyway. This is NOT\n'
        '\t// a change of behaviour: the derived class already cleaned up its own in\n'
        '\t// its own destructor, which ran EARLIER. Reaching\n'
        '\t// from here to `Destroy` of the derived class would be impossible anyway.\n'
        '\tCSkyObject::Destroy();\n}',

        'A CALL OF A PURE VIRTUAL METHOD FROM A DESTRUCTOR. `Destroy` is declared'
        ' `= 0`, so the method table holds `__cxa_pure_virtual` - a function with'
        ' a different signature than a member method. Wasm checks the type on '
        'every indirect call, so instead of silently working (as with '
        'optimization on, which devirtualizes it) it ends with "function '
        'signature mismatch" and kills the client on ENTERING THE WORLD. I '
        'qualify the call - that is exactly the function that runs on the '
        'platform where this code worked.',
    ),
    (
        'eterGrnLib/LODController.cpp',
        '\t\tassert(!"EMPTY SKELETON(CANNON LINK)");\n\t\treturn;',

        '\t\t// PORT: AN EMPTY SKELETON IS AN EXPECTED STATE FOR US.\n'
        '\t\t//\n'
        '\t\t// As long as there is no `.gr2` reader, NO character model\n'
        '\t\t// loads, so the skeleton is always empty. The TMP4 code already\n'
        '\t\t// handles that - one line below it simply RETURNS. The `assert` here was\n'
        '\t\t// a warning for the programmer and in the `NDEBUG` release, i.e.\n'
        '\t\t// the one people ran, it DISAPPEARED.\n'
        '\t\t//\n'
        '\t\t// Here assertions are on, so the warning turned\n'
        '\t\t// into `abort()` and killed the whole runtime.\n'
        '\t\t// Symptom: login passes, the client enters\n'
        '\t\t// character selection and GOES OUT - a black screen without any error\n'
        '\t\t// in `syserr.txt`, because `abort()` is faster than the write.\n'
        '\t\t//\n'
        '\t\t// I do NOT turn off assertions in the whole build. The rest of them guard\n'
        '\t\t// things that CAN break here and I want to know about them\n'
        '\t\t// at once. This one describes a gap we made ourselves\n'
        '\t\t// deliberately and which is documented.\n'
        '\t\t//\n'
        '\t\t// Reports ONCE. Every character, every hair and every weapon\n'
        '\t\t// passes through here, so without a counter this one gap would flood\n'
        '\t\t// `syserr.txt` with hundreds of identical lines and cover\n'
        '\t\t// everything else.\n'
        '\t\tstatic bool s_bAlreadyReported = false;\n'
        '\t\tif (!s_bAlreadyReported)\n'
        '\t\t{\n'
        '\t\t\ts_bAlreadyReported = true;\n'
        '\t\t\tTraceError("PORT: empty skeleton - there is no .gr2 reader, '
        'so character models do not load. This is a KNOWN gap, "\n'
        '\t\t\t\t"not a failure. Reported once.");\n'
        '\t\t}\n'
        '\t\treturn;',

        'AN ASSERTION THAT KILLED THE CLIENT AFTER LOGIN. `assert` in TMP4 is a '
        'guard for the programmer and disappears in the NDEBUG release - the code'
        ' one line below RETURNS anyway, so the behaviour without the assertion '
        'is the one the authors foresaw. Here assertions are on, so a warning '
        'about a gap we made deliberately (no .gr2 reader) turned into abort() '
        'and killed the whole game. I replace it with a one-time report - I do '
        'NOT turn off assertions in the whole build, because the rest of them '
        'guard things I want to know about at once.',
    ),
    (
        'eterLib/NetStream.cpp',
        'if (select(0, &fdsRecv, &fdsSend, NULL, &delay) == SOCKET_ERROR)',
        'if (select(m_sock + 1, &fdsRecv, &fdsSend, NULL, &delay) == SOCKET_ERROR)',
        REASON_SELECT,
    ),
    (
        'eterLib/NetDatagram.cpp',
        'if (select(0, &m_fdsRecv, &m_fdsSend, NULL, &delay) == SOCKET_ERROR)',
        'if (select(m_sock + 1, &m_fdsRecv, &m_fdsSend, NULL, &delay) == SOCKET_ERROR)',
        REASON_SELECT + ' The same call, the same mistake - I fix BOTH, not only the one where '
            'the symptom showed.',
    ),
    (
        'eterLib/Pool.h',
        'void * operator new(unsigned int /*mem_size*/)',
        'void * operator new(size_t /*mem_size*/)',
        'on wasm32 size_t is unsigned long, not unsigned int - the same width, a '
        'DIFFERENT type. The language requires exactly size_t here. The same '
        'mistake as with DWORD.',
    ),
    (
        'eterLib/Pool.h',
        'void * operator new(unsigned int mem_size)',
        'void * operator new(size_t mem_size)',
        'as above, the second occurrence (CPooledObjectWithSize)',
    ),
    (
        'effectLib/Type.h',
        'typedef std::vector<CTimeEvent<T> >::iterator iterator;',
        'typedef typename std::vector<CTimeEvent<T> >::iterator iterator;',
        'the word typename is missing before a name dependent on a template '
        'parameter. MSVC let it through, the C++ standard requires it - clang is '
        'right',
    ),
    (
        'GameLib/FlyTarget.h',
        'class IFlyTargetableObject',
        'class CFlyTarget;\n\nclass IFlyTargetableObject',
        'a friend class declaration does NOT introduce the name into the '
        'enclosing scope - `std::set<CFlyTarget*>` below does not see it. MSVC '
        'let it through. It blocked 23 of the 59 GameLib files',
    ),
    (
        'PRTerrainLib/TerrainType.h',
        '#define PR_FLOAT_TO_INTASM __asm',
        '#define PR_FLOAT_TO_INTASM PR_ICNV = (long)nearbyintf(PR_FCNV)\n'
        '#define PR_FLOAT_TO_INTASM_ORIGINAL_X87 __asm',
        'an x87 assembler insert (fld/fistp), which wasm does not have. `fistp` '
        'rounds ACCORDING TO THE CURRENT MODE, it does not truncate - by default '
        'to the nearest even. TMP4 never changes that mode (the only `_controlfp`'
        ' sets PRECISION, not rounding), so `nearbyintf` gives the same. I '
        'replace ONLY this one macro, so the fix `PR_ICNV > PR_FCNV ? PR_ICNV-1 :'
        ' PR_ICNV` in `PR_FLOAT_TO_INT` still works and still gives the floor',
    ),
    (
        'effectLib/Type.h',
        'BILLBOARD_TYPE_2FACE, //     / and \\ \n',
        'BILLBOARD_TYPE_2FACE, //     / and (backslash)\n',
        'a `//` comment ended with a BACKSLASH and a space, so the next line was '
        'GLUED to it - and it held BILLBOARD_TYPE_3FACE, which therefore never '
        'came into being. Clang splices lines according to the standard, MSVC was'
        ' lenient'
    ),
    (
        'UserInterface/PythonCharacterManager.cpp',
        'TraceError("CPythonCharacterManager::CreateInstance: VID[%d] - ALREADY EXIST\\n", c_rkCreateData);',
        'TraceError("CPythonCharacterManager::CreateInstance: VID[%d] - ALREADY EXIST\\n", c_rkCreateData.m_dwVID);',
        'A BUG IN TMP4, not a dialect difference: the WHOLE `SCreateData` '
        'structure was passed to `%d`, not a number. On Windows this would print '
        'garbage too - clang just says so loudly. The message text ("VID[%d]") '
        'clearly points at the `m_dwVID` field',
    ),
    (
        'UserInterface/MarkManager.cpp',
        '#if _MSC_VER < 1200',
        '#if defined(_MSC_VER) && _MSC_VER < 1200',
        'under clang `_MSC_VER` does not exist, so in `#if` it is zero - and `0 <'
        ' 1200` is TRUE. So the file took the branch for old MSVC and did not '
        'define `sys_err` or `sys_log`. The author meant "old MSVC", not "every '
        'compiler that is not MSVC"',
    ),
    (
        'UserInterface/PythonPlayerModule.cpp',
        'int pos = 0;\n			PyObject* key, *value;',
        'Py_ssize_t pos = 0;\n			PyObject* key, *value;',
        '`PyDict_Next` takes `Py_ssize_t*` in Python 3, not `int*`. It is the '
        'first boundary of the bridge that cannot be handled by a '
        'rename - the call site has to be touched',
    ),
    (
        'ScriptLib/PythonUtils.cpp',
        '*ret = unsigned char(val);',
        '*ret = (unsigned char)(val);',
        'a function-style cast works only for SINGLE-WORD type names - the same '
        'as in PythonIME.cpp. It occurs TWICE in this file and both times it is '
        'the same mistake',
        2,
    ),
    (
        'UserInterface/MarkImage.cpp',
        '#if !defined(_MSC_VER)',
        '#if defined(__M2_SERVER_BUILD__)',
        'the file is SHARED with the server and picks a branch by whether MSVC '
        'compiles it. Under clang "not MSVC" meant "server" - and we build the '
        'CLIENT. The name __M2_SERVER_BUILD__ says plainly which side is meant, '
        "instead of inferring it from the compiler's identity"
    ),
    (
        'UserInterface/UserInterface.cpp',
        '( MS C++ %d Compiled )", __TIMESTAMP__, _MSC_VER);',
        '( clang %d Compiled )", __TIMESTAMP__, __clang_major__);',
        'the test version message prints the compiler number. Under clang '
        '`_MSC_VER` does not exist. I give the clang version instead of zeroing '
        'the number - the sentence is still to tell the TRUTH about what built '
        'it. It occurs FOUR times: once for every build variant',
        4,
    ),
    (
        'UserInterface/PythonIME.cpp',
        'unsigned char c = unsigned char(wParam & 0xff);',
        'unsigned char c = (unsigned char)(wParam & 0xff);',
        'a function-style cast works only for SINGLE-WORD type names. `unsigned '
        'char(x)` is not valid - MSVC let it through. The meaning does not '
        'change, so the fix is safe',
    ),
    (
        'UserInterface/PythonNetworkStreamPhaseLoading.cpp',
        '(0xfe-0xa1+1)',
        '(0xfe - 0xa1 + 1)',
        'the same expression, the second occurrence',
    ),
    (
        'UserInterface/PythonNetworkStreamPhaseGame.cpp',
        '(0xfe-0xa1+1)',
        '(0xfe - 0xa1 + 1)',
        'WITHOUT A SPACE `0xfe-0xa1` is ONE token. The language rule says a '
        'number takes in `e-` or `E-` (because that is how an exponent is '
        'written), and `0xfe` ends with `e` - so the minus is pulled in and the '
        'whole stops being a valid number. The expression computes the position '
        'of a hangul character in the encoding table',
    ),
    # -----------------------------------------------------------------------
    # PYTHON 3 - three files that did not compile earlier
    # -----------------------------------------------------------------------
    # The port embeds **CPython 3 under wasm32-emscripten** (tools/build_python.py).
    # That settles the question that had hung: the target is Python 3,
    # not Python 2. All the patches below follow from that.
    #
    # The patterns have TABS and trailing spaces exactly as in
    # the TMP4 source - that is why they are written through repr, not by hand.
    (
        'ScriptLib/PythonLauncher.cpp',
        '\tconst char * funcname;\n\tchar szTraceBuffer[128];\n',
        '\tconst char * funcname;\n\tchar szTraceBuffer[128];\n\n\t// PORT: `PyFrameObject` is OPAQUE in Python 3 - it no longer has\n\t// the fields `f_code`, `f_lineno` or `f_lasti`. The guard\n\t// `CM2wPythonFrame` from `python2_bridge.h` takes the code object ONCE\n\t// and releases it in the destructor.\n\t//\n\t// THIS IS THE WHOLE CONTENT OF THIS PATCH: `PyFrame_GetCode` returns a NEW\n\t// reference, not a borrowed one. Rewriting `f->f_code` one to\n\t// one would compile and work - and would leak a code\n\t// object on EVERY traced event, i.e. in tracing mode on\n\t// every line of a game script.\n\tconst CM2wPythonFrame kFrame(f);\n',
        'PyFrameObject is opaque in Python 3, and PyFrame_GetCode returns a NEW '
        'reference - the guard takes it once and releases it in the destructor',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\t\t\tif (Py_OptimizeFlag)\n\t\t\t\tf->f_lineno = PyCode_Addr2Line(f->f_code, f->f_lasti);\n\n\t\t\tfuncname = PyString_AsString(f->f_code->co_name);\n',
        '\t\t\t// The computation `PyCode_Addr2Line(f->f_code, f->f_lasti)` under\n\t\t\t// `Py_OptimizeFlag` is gone: `PyFrame_GetLineNumber` does exactly the\n\t\t\t// same and does it always, so the condition was only a way of\n\t\t\t// saving that computation.\n\t\t\tfuncname = kFrame.FunctionName();\n',
        'PyFrame_GetLineNumber already computes what PyCode_Addr2Line did under '
        'Py_OptimizeFlag',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\t\t\t\t\t  PyString_AsString(f->f_code->co_filename), \n\t\t\t\t\t  f->f_lineno,\n\t\t\t\t\t  funcname);\n',
        '\t\t\t\t\t  kFrame.FileName(),\n\t\t\t\t\t  kFrame.LineNumber(),\n\t\t\t\t\t  funcname);\n',
        'frame fields through the guard, call',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\t\t\tint len;\n\t\t\tconst char * exc_str;\n\t\t\tPyObject_AsCharBuffer(exc_type, &exc_str, &len);\n',
        '\t\t\t// PORT: `PyObject_AsCharBuffer` is gone in Python 3 - and there is\n\t\t\t// nothing to replace, because its result (`exc_str`) WAS NOT USED EVEN\n\t\t\t// ONCE. The message below is built from the frame alone. It was\n\t\t\t// dead code already in TMP4.\n',
        'PyObject_AsCharBuffer is gone in Python 3, and its result was not used '
        'anywhere anyway - dead code already in TMP4',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\t\t\t\t\t  PyString_AS_STRING(f->f_code->co_filename), \n\t\t\t\t\t  f->f_lineno,\n\t\t\t\t\t  PyString_AS_STRING(f->f_code->co_name));\n',
        '\t\t\t\t\t  kFrame.FileName(),\n\t\t\t\t\t  kFrame.LineNumber(),\n\t\t\t\t\t  kFrame.FunctionName());\n',
        'frame fields through the guard, exception',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\tPy_SetProgramName((char*)c_szProgramName);\n',
        '\t// PORT: `Py_SetProgramName` is gone in Python 3.13, and before that it\n\t// took `wchar_t*`. It served to compute the paths of the standard\n\t// library from the location of the executable - and in the browser\n\t// there is no executable.\n\t//\n\t// THIS IS NOT A STUB, BUT A TRANSFER OF RESPONSIBILITY:\n\t// the program name is visible in `sys.argv[0]` and in error\n\t// messages, and in Python 3 it is set through `PyConfig.program_name`\n\t// BEFORE `Py_InitializeFromConfig`. The layer embedding\n\t// CPython does that - and `Py_InitializeFromConfig` is exactly what the\n\t// port binary calls. This function is not that place.\n\t(void)c_szProgramName;\n',
        'Py_SetProgramName is gone in Python 3.13; the program name now belongs '
        'to PyConfig in the embedding layer',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\t_PyMarshal_ReadLongFromFile(fp);\n\tv = _PyMarshal_ReadLastObjectFromFile(fp);\n',
        '\t// PORT: the `.pyc` header grew. In Python 2 the code object was preceded\n\t// by TWO words (magic number and source modification time); since\n\t// Python 3.7 there are FOUR - a bit field and the source size were added.\n\t// Reading the old number of words would start taking the code object apart\n\t// eight bytes too early, and marshal would only say\n\t// "bad marshal data" - a message nobody would connect\n\t// with the header.\n\tfor (int iWord = 1; iWord < M2W_PYC_HEADER_WORDS; ++iWord)\n\t\t_PyMarshal_ReadLongFromFile(fp);\n\n\tv = _PyMarshal_ReadLastObjectFromFile(fp);\n',
        'the .pyc header has four words since Python 3.7, not two',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\tif (Py_FlushLine()) \n\t\tPyErr_Clear();\n',
        '\t// PORT: `Py_FlushLine` is gone in Python 3. It flushed `sys.stdout`\n\t// after the Python 2 `print` statement, which buffered the whole line.\n\t// In Python 3 `print` is a function and decides about flushing itself,\n\t// so there is nothing to call. That is the truth about this version\n\t// of the language, not a stub.\n',
        'Py_FlushLine is gone in Python 3 together with line buffering by the '
        'print statement',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        '\tv = PyEval_EvalCode(co, m_poDic, m_poDic);\n',
        '\t// PORT: in Python 3 `PyEval_EvalCode` takes `PyObject*`, not\n\t// `PyCodeObject*` - that is the only change, the meaning stays.\n\t// The cast is safe: `PyCodeObject` starts with\n\t// `PyObject_HEAD`, so it is a Python object in every respect.\n\tv = PyEval_EvalCode((PyObject *) co, m_poDic, m_poDic);\n',
        'PyEval_EvalCode takes PyObject* in Python 3, not PyCodeObject*',
    ),
    (
        'UserInterface/PythonSkill.cpp',
        '''	return Py_BuildValue("i", c_pSkillData->GradeData[iGradeIndex]);''',
        '''	// A FLAW OF TMP4 ITSELF, not of the port. `GradeData[i]` is the WHOLE structure
	// `TGradeData { std::string strName; CGraphicImage* pImage; WORD
	// wMotionIndex; }`, passed through the ellipsis under the format "i".
	//
	// It was broken already in the original: MSVC pushed the structure onto the stack,
	// and "i" read its FIRST FOUR BYTES - i.e. the start of the internal
	// buffer of `std::string`, in practice the first four characters of the grade
	// name taken as a number. Clang does not allow this at all,
	// because the type is not trivial.
	//
	// The format "i" says that ONE INTEGER was to come out, and the only
	// integer field of this structure is `wMotionIndex` - and that is
	// the reading I put here. I have no evidence for it besides the
	// structure and the format, but any answer here is at least as
	// good as the original one, because the original one was accidental.
	//
	// SECOND EVIDENCE, found in the same file: fifty-eight
	// lines above, `skillGetIconImageNew` does
	// `Py_BuildValue("i", GradeData[i].pImage)` - i.e. the author picks
	// a SINGLE field from this structure, and the image pointer is already
	// handled by a separate function. `strName` would need the format "s".
	// `wMotionIndex` remains.
	//
	// TO CHECK IN THE GAME: whether the scripts call
	// `skill.GetGradeData` at all. If not, this function is dead and the question
	// disappears.
	return Py_BuildValue("i", (int)c_pSkillData->GradeData[iGradeIndex].wMotionIndex);''',
        'Py_BuildValue("i", ...) got the WHOLE TGradeData structure through the '
        'ellipsis - broken already in TMP4; wMotionIndex is the only integer '
        'field, so it is the one that fits the format',
    ),

    (
        'UserInterface/PythonApplication.cpp',
        'void CPythonApplication::Loop()\n{\t\n\twhile (1)\n\t{\t\n\t\tif (IsMessage())\n\t\t{\n\t\t\tif (!MessageProcess())\n\t\t\t\tbreak;\n\t\t}\n\t\telse\n\t\t{\n\t\t\tif (!Process())\n\t\t\t\tbreak;\n\n\t\t\tm_dwLastIdleTime=ELTimer_GetMSec();\n\t\t}\n\t}\n}',
        '/// PORT: true once the browser spins the main loop - before that there\n/// is no game to hand messages to (`M2W_PumpMessagesNow`).\nstatic bool s_bM2WLoopRunning = false;\n\nvoid CPythonApplication::Loop()\n{\n\t// PORT: THE MAIN LOOP HANDED OVER TO THE BROWSER.\n\t//\n\t// The original spun here until the window closed. In the browser\n\t// there is ONE thread and ONE event loop - until we give it\n\t// back, not a single frame is drawn and not a single\n\t// button works. The tab simply freezes.\n\t//\n\t// WHY THIS IS SUCH A SMALL CHANGE: one turn of this loop is\n\t// already exactly ONE FRAME. Nothing has to be split up or\n\t// rearranged - it is enough to give control back after every turn.\n\t// Hence `emscripten_set_main_loop`, not Asyncify: Asyncify\n\t// would double the binary and slow everything down, to buy\n\t// a capability this code does not need.\n\t//\n\t// The second argument (0) means "as many frames per second as the browser\n\t// wants" - i.e. `requestAnimationFrame`. A number of our own\n\t// would be worse: it would not draw in step with the screen refresh.\n\t//\n\t// The third (1) means "do not return from this function". Without it `Loop`\n\t// would return, `WinMain` would go on to clean up and free\n\t// everything the loop is about to use.\n\tstd::printf("m2w loop: handing control to the browser\\n");\n\ts_bM2WLoopRunning = true;\n\temscripten_set_main_loop(CPythonApplication::__LoopTurn, 0, 1);\n}\n\n/// One turn of the old loop. The content rewritten ONE TO ONE\n/// from the original - only `while (1)` is gone, because from now on the\n/// browser spins it.\n///\n/// `break` from the original corresponds here to `emscripten_cancel_main_loop`:\n/// both mean "the end, we leave the loop".\nvoid CPythonApplication::__LoopTurn()\n{\n\t// PORT: FRAME LIMITER. `Sleep(rest)` at the end of\n\t// `Process()` records a deadline; before it a turn is empty.\n\t// Without this the game (own time 16-17 ms per turn) runs as many times\n\t// too fast as the screen refreshes faster than 60 Hz.\n\tif (M2W_LoopStillSleeping())\n\t\treturn;\n\n\t// TURN COUNTER - see the note at `Loop()`.\n\t//\n\t// The first three turns and then every six-hundredth. Three, because\n\t// the most important is the answer to the question \"did it start\n\t// at all\"; every six-hundredth, because ten seconds of silence is to\n\t// mean \"stuck\", not \"nobody is looking\".\n\t// PORT: start of the frame - from here WORK is counted, not\n\t// wall-clock time. See compat/frame_stats.h.\n\tM2W_BeginFrame();\n\n\tstatic unsigned long s_uTurns = 0;\n\t++s_uTurns;\n\tif (s_uTurns <= 3 || s_uTurns % 600 == 0)\n\t\tstd::printf("m2w loop: turn %lu\\n", s_uTurns);\n\n\tCPythonApplication& rkApp = CPythonApplication::Instance();\n\n\t// PORT: ALL messages, THEN the frame. The original did\n\t// in one turn EITHER one message, OR a frame - but it spun this\n\t// loop thousands of times a second, so the messages went through at once.\n\t// Here one turn = one requestAnimationFrame frame, so every\n\t// message ATE a whole frame: a held key (auto-repeat\n\t// ~30/s) = every second frame without drawing - reported as \"stutter while\n\t// holding any key\". The limit protects against an endless loop,\n\t// should messages arrive faster than they can be taken off.\n\tfor (int iMessages = 0; iMessages < 64 && rkApp.IsMessage(); ++iMessages)\n\t{\n\t\tif (!rkApp.MessageProcess())\n\t\t{\n\t\t\temscripten_cancel_main_loop();\n\t\t\treturn;\n\t\t}\n\t}\n\n\tif (!rkApp.Process())\n\t{\n\t\temscripten_cancel_main_loop();\n\t\treturn;\n\t}\n\n\trkApp.m_dwLastIdleTime = ELTimer_GetMSec();\n\n\t// PORT: A TRY AT OUR OWN DRAWING, at the end of the frame.\n\t//\n\t// It draws HERE, because this is the only place where the frame is already\n\t// finished: everything the game was to draw is drawn,\n\t// and the browser has not shown it yet. Earlier, the model\n\t// would be painted over by the interface background.\n\t//\n\t// It is switched on by the marker `?modelprobe=1` in the page URL\n\t// and serves one question: does the whole chain - the .gr2 reader,\n\t// pose, skin, our own shader program - work WITHOUT A SERVER.\n\tM2W_ModelProbe();\n\n\t// PORT: end of the frame - here the time accounting closes.\n\tM2W_MeasureFrame();\n}\n\n/// PORT: hands the queued window messages to the game NOW, from inside a\n/// browser input handler (runtime.js, right and middle press). The game\n/// decides while handling the press whether it turns the camera\n/// (`CCamera::BeginDrag` -> m2w.cameraDrag), and Firefox grants a pointer lock\n/// only while a user input handler runs - in the next frame it is too\n/// late. Between two frames, like the original loop taking a message\n/// between two `Process()` calls. Returns the number of messages taken.\nextern "C" EMSCRIPTEN_KEEPALIVE int M2W_PumpMessagesNow()\n{\n\tif (!s_bM2WLoopRunning)\n\t\treturn 0;\n\n\tCPythonApplication& rkApp = CPythonApplication::Instance();\n\tint iMessages = 0;\n\tfor (; iMessages < 64 && rkApp.IsMessage(); ++iMessages)\n\t{\n\t\tif (!rkApp.MessageProcess())\n\t\t{\n\t\t\ts_bM2WLoopRunning = false;\n\t\t\temscripten_cancel_main_loop();\n\t\t\tbreak;\n\t\t}\n\t}\n\treturn iMessages;\n}',
        'the main loop handed over to the browser - one turn of this loop is '
        'already one frame, so the change is small; without it the tab freezes '
        'and nothing is drawn',
    ),
    (
        'UserInterface/PythonApplication.cpp',
        '\tCTimer::Instance().UseCustomTime();\n\tm_dwWidth = 800;',
        '\t// PORT: REAL TIME instead of a fixed step.\n'
        '\t// The original computed game time with a fixed step of 16/17 ms PER LOOP TURN\n'
        '\t// (`UseCustomTime`), and spun the loop thousands of times a second and added\n'
        '\t// "frame skipping" (s_bFrameSkip) when a turn was late relative to that\n'
        '\t// step. Here one turn = one `requestAnimationFrame` frame:\n'
        '\t// at 60 Hz a turn takes 16.67 ms > the 16.5 ms step, so the game is\n'
        '\t// FOREVER late and skips drawing EVERY frame (measured:\n'
        '\t// black screen, loop at 60 FPS, `potok D3D8 0 wywolan`); at ~117 Hz\n'
        '\t// game time ran ~1.95x faster. Real time (the default\n'
        '\t// in CTimer) removes both; frame skipping makes no sense when the pace\n'
        '\t// is set by the browser - turned off.\n'
        '\tm_isFrameSkipDisable = true;\n\tm_dwWidth = 800;',
        'real game time instead of a fixed 16 ms step per turn; no frame skipping'
        '',
    ),
    (
        'UserInterface/PythonApplication.cpp',
        'void CPythonApplication::SetFrameSkip(bool isEnable)\n{\n\tif (isEnable)\n\t\tm_isFrameSkipDisable=false;\n\telse\n\t\tm_isFrameSkipDisable=true;\n}',
        'void CPythonApplication::SetFrameSkip(bool /*isEnable*/)\n{\n'
        '\t// PORT: `game.py` turns frame skipping on at\n'
        '\t// entering the world (`app.SetFrameSkip(1)`). With real time\n'
        '\t// `s_uiNextFrameTime` == the moment of `Advance()`, and `dwCurrentTime` is read\n'
        '\t// AFTER the update - the frame is ALWAYS "late" by its own work\n'
        '\t// time and EVERY one is skipped (measured: login draws, the world\n'
        '\t// black, `potok D3D8 0 wywolan`). The pace is set by the browser,\n'
        '\t// skipping has nothing to catch up on - turned off for good.\n'
        '\tm_isFrameSkipDisable=true;\n}',
        'frame skipping turned off for good (game.py turns it on when entering '
        'the world)',
    ),
    (
        'UserInterface/PythonApplication.cpp',
        '\t\t\ts_uiNextFrameTime += nAdjustTime; \n\t\t\tprintf("FrameSkip  %d\\n",nAdjustTime);\n\t\t\tCTimer::Instance().Adjust(nAdjustTime);',
        '\t\t\t// PORT: WITHOUT moving the clock and WITHOUT\n'
        '\t\t\t// `s_uiNextFrameTime` running ahead: `+= nAdjustTime` without `Adjust`\n'
        '\t\t\t// gave `rest` = 1.4 s and `Sleep(1386)` every frame (0.7 FPS - "the game\n'
        '\t\t\t// stutters tragically"). Instead, alignment with the clock.\n'
        '\t\t\ts_uiNextFrameTime = dwCurrentTime;\n'
        '\t\t\t// `Adjust`\n'
        '\t\t\t// added `nAdjustTime` to `m_dwCurrentTime`, which with REAL\n'
        '\t\t\t// time gave in the next `Advance()` a NEGATIVE\n'
        '\t\t\t// delta -> the DWORD wraps to ~49 days -> `m_fLocalTime` of the models\n'
        '\t\t\t// runs off into millions of seconds: a character like a ghost, without textures\n'
        '\t\t\t// (measured A/B: with UseCustomTime the character is normal).\n'
        '\t\t\t// With real time there is nothing to "catch up".',
        'FrameSkip does not move the clock (a negative delta with real time)',
    ),
    (
        'EterBase/Timer.cpp',
        '\t\tm_dwElapsedTime = currentTime - m_dwCurrentTime;',
        '\t\tm_dwElapsedTime = currentTime - m_dwCurrentTime;\n'
        '\t\t// PORT: one frame in the browser can last seconds\n'
        '\t\t// (a synchronous chunk download, a hidden tab) - the original with a\n'
        '\t\t// fixed 16 ms step never saw such a delta. We cap it\n'
        '\t\t// at 100 ms, so that animations and clocks do not jump.\n'
        '\t\tif (m_dwElapsedTime > 100)\n'
        '\t\t\tm_dwElapsedTime = 100;',
        'the real-time delta capped at 100 ms per frame',
    ),
    (
        'UserInterface/PythonApplication.cpp',
        '\tif (rest > 0 && !bCurrentLateUpdate )\n\t{\n\t\ts_uiLoad -= rest;',
        '\t// PORT: no sleeping in the loop. `rest` was to hold\n'
        '\t// 60 turns/s with a fixed time step; with real time the pace\n'
        '\t// is set by the browser, and after a long frame (loading 1.4 s)\n'
        '\t// `s_uiNextFrameTime` ran ahead and `Sleep(1400)` went every frame\n'
        '\t// (measured: 0.7 FPS, "the game stutters tragically").\n'
        '\tif (false && rest > 0 && !bCurrentLateUpdate )\n\t{\n\t\ts_uiLoad -= rest;',
        'the loop without Sleep(rest) - real time needs no limiter',
    ),
    (
        'GameLib/ActorInstance.h',
        '\t\t\tDWORD\t\t\tdwcurFrame;\n\t\t\tDWORD\t\t\tdwFrameCount;\n',
        '\t\t\tDWORD\t\t\tdwcurFrame;\n\t\t\tDWORD\t\t\tdwFrameCount;\n'
        '\t\t\tDWORD\t\t\tdwM2wProcessed;   // PORT: event frames computed from time, not from loop turns\n',
        'motion node: the event frame counter computed from time',
    ),
    (
        'GameLib/ActorInstanceMotion.cpp',
        '\tm_kCurMotNode.dwcurFrame = 0;\n\tm_kCurMotNode.dwFrameCount = fDurationTime / (1.0f / g_fGameFPS);\n}',
        '\tm_kCurMotNode.dwcurFrame = 0;\n\tm_kCurMotNode.dwM2wProcessed = 0;\n\tm_kCurMotNode.dwFrameCount = fDurationTime / (1.0f / g_fGameFPS);\n}',
        'SetMotion resets the event frame counter',
    ),
    (
        'GameLib/ActorInstanceMotion.cpp',
        '\telse\n\t{\n\t\tMotionEventProcess();\n\t\tSoundEventProcess(!isPC);\n\n\t\t++m_kCurMotNode.dwcurFrame;\n\t}\n}',
        '\telse\n\t{\n'
        '\t\t// PORT: the original did HERE one event frame\n'
        '\t\t// PER LOOP TURN (assumption: 60 turns/s). In the browser a turn = a monitor\n'
        '\t\t// frame: at 120 Hz steps and movement effects went 2x too fast, at\n'
        '\t\t// 30 Hz 2x too slow. We compute event frames from TIME (like the attack branch),\n'
        '\t\t// exactly 60 per second of animation; after a stutter we do not catch up\n'
        '\t\t// more than 30 frames (0.5 s), so as not to fire a burst of sounds.\n'
        '\t\tif (M2W_EventsFromLoop())\n'
        '\t\t{\n'
        '\t\t\t// ?events=loop - the old path (one event frame per turn), for A/B\n'
        '\t\t\tMotionEventProcess();\n'
        '\t\t\tSoundEventProcess(!isPC);\n'
        '\t\t\t++m_kCurMotNode.dwcurFrame;\n'
        '\t\t\treturn;\n'
        '\t\t}\n'
        '\t\t// EVENT frames are in ANIMATION time - a motion\n'
        '\t\t// plays with `fSpeedRatio` (skills: casting speed), and\n'
        '\t\t// an event `dwFrame` from .msa is an animation frame. A counter without the multiplier\n'
        '\t\t// gave the Aura/Berserk effect with a delay (the user, A/B ?events=loop).\n'
        '\t\t// The attack branch above does exactly the same (`GetAttackingElapsedTime`).\n'
        '\t\tconst float fElapsed = GetLocalTime() - m_kCurMotNode.fStartTime;\n'
        '\t\tconst float fRatio = (m_kCurMotNode.fSpeedRatio > 0.0f) ? m_kCurMotNode.fSpeedRatio : 1.0f;\n'
        '\t\tDWORD dwTarget = (fElapsed > 0.0f) ? DWORD(fElapsed * fRatio * g_fGameFPS) : 0;\n'
        '\t\tif (dwTarget > m_kCurMotNode.dwM2wProcessed + 30)\n'
        '\t\t\tm_kCurMotNode.dwM2wProcessed = dwTarget - 30;\n'
        '\t\tif (dwTarget < m_kCurMotNode.dwM2wProcessed)\n'
        '\t\t\tm_kCurMotNode.dwM2wProcessed = dwTarget;   // local time moved back (motion loop)\n'
        '\t\t// animation length in animation frames (dwFrameCount is in time frames)\n'
        '\t\tconst DWORD dwLength = (MOTION_TYPE_LOOP == m_kCurMotNode.iMotionType && m_kCurMotNode.dwFrameCount > 0)\n'
        '\t\t\t? DWORD(m_kCurMotNode.dwFrameCount * fRatio + 0.5f) : 0;\n'
        '\t\twhile (m_kCurMotNode.dwM2wProcessed < dwTarget)\n'
        '\t\t{\n'
        '\t\t\t// events get the ANIMATION frame through `dwcurFrame` (that is how\n'
        '\t\t\t// MotionEventProcess/SoundEventProcess read it), after which the time frame returns\n'
        '\t\t\tm_kCurMotNode.dwcurFrame = dwLength ? (m_kCurMotNode.dwM2wProcessed % dwLength) : m_kCurMotNode.dwM2wProcessed;\n'
        '\t\t\tMotionEventProcess();\n'
        '\t\t\tSoundEventProcess(!isPC);\n'
        '\t\t\t++m_kCurMotNode.dwM2wProcessed;\n'
        '\t\t}\n'
        '\t\t// the TIME frame (60/s) - that is what CurrentMotionProcess expects (end of a ONCE motion)\n'
        '\t\tm_kCurMotNode.dwcurFrame = (fElapsed > 0.0f) ? DWORD(fElapsed * g_fGameFPS) : 0;\n'
        '\t}\n}',
        'motion events (steps, effects) computed from time, not from the number '
        'of loop turns',
    ),
    (
        'UserInterface/PythonApplication.h',
        '\t\tvoid Loop();',
        '\t\tvoid Loop();\n\t\t/// PORT: one turn of the old main loop.\n\t\t/// Static, because `emscripten_set_main_loop` takes a plain\n\t\t/// function pointer - there is no room for `this` there.\n\t\t/// The class is a singleton, so `Instance()` returns the\n\t\t/// same object that held the state before the change.\n\t\tstatic void __LoopTurn();',
        'declaration of the loop turn - the body is in PythonApplication.cpp',
    ),
    (
        'UserInterface/PythonApplication.cpp',
        '#include "StdAfx.h"',
        '#include "StdAfx.h"\n\n// PORT: the main loop gives control to the browser.\n#include <emscripten/emscripten.h>\n\n// PORT: a try at our own drawing - see the end of the loop turn.\nextern void M2W_ModelProbe();\n\n// PORT: time accounting of one frame - see compat/frame_stats.h.\nextern void M2W_MeasureFrame();\nextern void M2W_BeginFrame();\n\n// PORT: frame limiter - see compat/win32_compat.cpp.\nextern bool M2W_LoopStillSleeping();',
        'the emscripten header for emscripten_set_main_loop',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        'PyObject * builtins = PyImport_ImportModule("__builtin__");',
        '// PORT: in Python 2 the built-in module was called `__builtin__`, in Python 3\n\t// it is called `builtins`. This is NOT cosmetic: `PyImport_ImportModule`\n\t// with the wrong name returns `NULL` and sets an exception, and the next line\n\t// (`PyModule_AddIntConstant`) gets that `NULL` and falls over on it.\n\t//\n\t// The symptom would be misleading: a crash in `PyModule_AddIntConstant`, i.e. ONE\n\t// line AFTER the real cause.\n\tPyObject * builtins = PyImport_ImportModule("builtins");',
        'the built-in module is called `builtins` in Python 3, not `__builtin__` '
        '- without this the import returns NULL and the next line falls over',
    ),
    (
        'ScriptLib/PythonLauncher.cpp',
        'void Traceback()\n{\n\tstd::string str;\n\n\tfor (int i = 0; i < g_nCurTraceN; ++i)\n\t{\n\t\tstr.append(g_stTraceBuffer[i]);\n\t\tstr.append("\\n");\n\t}\n\t\n\tPyObject * exc;\n\tPyObject * v;\n\tPyObject * tb;\n\tconst char * errStr;\n\n\tPyErr_Fetch(&exc, &v, &tb);\n\n\tif (PyString_Check(v))\n\t{\n\t\terrStr = PyString_AS_STRING(v);\n\t\tstr.append("Error: ");\n\t\tstr.append(errStr);\n\n\t\tTracef("%s\\n", errStr);\n\t}\n\tPy_DECREF(exc);\n\tPy_DECREF(v);\n\tPy_DECREF(tb);\n\tLogBoxf("Traceback:\\n\\n%s\\n", str.c_str());\n}',
        'void Traceback()\n{\n\t// PORT: THIS IS THE FUNCTION THAT WAS SUPPOSED TO SHOW ERRORS,\n\t// and it showed an empty string.\n\t//\n\t// The original checked `PyString_Check(v)`. In Python 2 the value\n\t// of an exception was sometimes a string, because Python 2 allowed raising strings.\n\t// In Python 3 `v` is ALWAYS an exception object, so the check\n\t// never passed and the log got only\n\t// "Traceback:" with an empty space.\n\t//\n\t// The effect was worse than not having this function: the client REPORTED an error\n\t// and at the same time hid its content. Every search for a cause\n\t// started with guessing.\n\t//\n\t// Now there are two paths at once and that is deliberate:\n\t//   `PyErr_Print` prints the FULL call trace to sys.stderr,\n\t//      i.e. through `TraceErrorFile` to syserr.txt,\n\t//   `PyObject_Str` gives a short message for the window.\n\t// The first says WHERE, the second WHAT - and neither alone is enough.\n\n\tstd::string str;\n\n\tfor (int i = 0; i < g_nCurTraceN; ++i)\n\t{\n\t\tstr.append(g_stTraceBuffer[i]);\n\t\tstr.append("\\n");\n\t}\n\n\tPyObject * exc = NULL;\n\tPyObject * v = NULL;\n\tPyObject * tb = NULL;\n\n\tPyErr_Fetch(&exc, &v, &tb);\n\n\t// Without this `v` is sometimes an argument tuple instead of an exception object -\n\t// `PyErr_Fetch` returns the raw state, not one ready to be shown.\n\tPyErr_NormalizeException(&exc, &v, &tb);\n\n\tif (v)\n\t{\n\t\tPyObject * pkText = PyObject_Str(v);\n\t\tif (pkText)\n\t\t{\n\t\t\tconst char * c_szText = PyUnicode_AsUTF8(pkText);\n\t\t\tif (c_szText)\n\t\t\t{\n\t\t\t\tstr.append("Error: ");\n\t\t\t\tstr.append(c_szText);\n\t\t\t\tTracef("%s\\n", c_szText);\n\t\t\t}\n\t\t\tPy_DECREF(pkText);\n\t\t}\n\t}\n\n\t// Call trace. `PyErr_Restore` takes ownership of the three references,\n\t// so AFTER IT they must no longer be released - and that is why there is no\n\t// `Py_DECREF` here. The original released them unconditionally, also when they were\n\t// empty; in Python 3 `PyErr_Fetch` without an error returns three zeros\n\t// and such a `Py_DECREF` crashed the program.\n\tif (exc)\n\t{\n\t\tPyErr_Restore(exc, v, tb);\n\t\tPyErr_Print();\n\t}\n\n\tLogBoxf("Traceback:\\n\\n%s\\n", str.c_str());\n}',
        'Traceback() showed an EMPTY message - it checked PyString_Check on the '
        'exception object, which never passes in Python 3; the client reported an'
        ' error and hid its content',
    ),
    (
        'UserInterface/PythonPackModule.cpp',
        '\t\t\t\treturn Py_BuildValue("s#",pData, file.Size());',
        '\t\t\t\t// PORT: "s#" MEANS "DECODE AS UTF-8" IN PYTHON 3.\n\t\t\t\t//\n\t\t\t\t// In Python 2 `str` was a byte sequence and this line simply\n\t\t\t\t// returned them. In Python 3 `str` is text, so `Py_BuildValue`\n\t\t\t\t// has to know IN WHAT ENCODING these bytes are - and it assumes\n\t\t\t\t// UTF-8, because that is the default answer.\n\t\t\t\t//\n\t\t\t\t// And this is not UTF-8 data. The game language files are in the CODE\n\t\t\t\t// PAGE given in `locale.cfg` (here 1252). Symptom:\n\t\t\t\t//\n\t\t\t\t//     UnicodeDecodeError: utf-8 codec cannot decode byte 0x91\n\t\t\t\t//\n\t\t\t\t// 0x91 is the left apostrophe in CP1252 - a byte that in UTF-8 has no\n\t\t\t\t// right to stand alone.\n\t\t\t\t//\n\t\t\t\t// So it decodes with THE page the client itself set at start,\n\t\t\t\t// and does not guess. `errors="replace"` instead of an error, because a single\n\t\t\t\t// unexpected byte in a language file is to give one question\n\t\t\t\t// mark, not to overturn the loading of the whole language.\n\t\t\t\t{\n\t\t\t\tchar szEncoding[16];\n\t\t\t\tsnprintf(szEncoding, sizeof(szEncoding), "cp%u",\n\t\t\t\t\t\t LocaleService_GetCodePage());\n\t\t\t\treturn PyUnicode_Decode(static_cast<const char*>(pData),\n\t\t\t\t\t\t\t\t\t  file.Size(), szEncoding, "replace");\n\t\t\t\t}',
        '"s#" means "decode as UTF-8" in Python 3, and the game language files '
        'are in the code page from locale.cfg - it decodes with the one the '
        'client set',
    ),
    (
        'ScriptLib/PythonUtils.cpp',
        OLD_PY_BUILDEXCEPTION,
        NEW_PY_BUILDEXCEPTION,
        'a function that set an exception returned Py_None instead of NULL. In '
        'Python 2 the exception waited and blew up elsewhere; in Python 3 it is '
        'SystemError "returned a result with an exception set". The authors\' own '
        '`return NULL;` lay there commented out',
    ),
    (
        'ScriptLib/PythonUtils.cpp',
        OLD_GETINTEGER,
        NEW_GETINTEGER,
        'PyInt_AsLong in Python 2 accepted a float and truncated; PyLong_AsLong '
        'in Python 3 sets TypeError, and the function reported success anyway - '
        'hence SystemError at SetWindowPosition',
    ),
    (
        'ScriptLib/PythonUtils.cpp',
        OLD_GETLONG,
        NEW_GETLONG,
        'the same in the long variant - both functions are called by the same '
        'bindings',
    ),

    # -----------------------------------------------------------------------
    # ROUTE B: characters are drawn by their own pipeline
    # -----------------------------------------------------------------------
    (
        'EterGrnLib/ModelInstanceRender.cpp',
        '#include "StdAfx.h"',
        '#include "StdAfx.h"\n'
        '\n'
        '// PORT: ROUTE B - spatial content bypasses the fixed-function\n'
        '// pipeline. See `compat/custom_draw.h`.\n'
        'extern bool M2W_DrawModelCustom(unsigned int uFirstIndex,\n'
        '                                   unsigned int uTriangles);',
        'declaration of our own drawing pipeline - the body is in '
        'compat/gl_device.cpp',
    ),
    (
        'EterGrnLib/ModelInstanceRender.cpp',
        '\t\t\tSTATEMANAGER.DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, '
        'vtxCount, pTriGroupNode->idxPos, pTriGroupNode->triCount);',
        '\t\t\t// PORT: ROUTE B.\n'
        '\t\t\t//\n'
        '\t\t\t// Everything needed to draw this mesh the game\n'
        '\t\t\t// has already set on the device: the vertex stream,\n'
        '\t\t\t// the indices, the texture and three matrices. Our own pipeline takes\n'
        '\t\t\t// it from there and draws with our own shader program,\n'
        '\t\t\t// instead of assembling a fixed-function pipeline of eight texture\n'
        '\t\t\t// stages - in which nothing shouts, just a black pixel\n'
        '\t\t\t// comes out.\n'
        '\t\t\t//\n'
        '\t\t\t// When the state does not suit our route (a vertex layout\n'
        '\t\t\t// other than PNT), it draws the old way. Better a worse picture\n'
        '\t\t\t// than none.\n'
        '\t\t\tif (!M2W_DrawModelCustom(pTriGroupNode->idxPos,\n'
        '\t\t\t                            pTriGroupNode->triCount))\n'
        '\t\t\t\tSTATEMANAGER.DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, '
        'vtxCount, pTriGroupNode->idxPos, pTriGroupNode->triCount);',
        'characters drawn by our own pipeline - four places, because there are '
        'that many variants of drawing model meshes',
        4,
    ),

    # -----------------------------------------------------------------------
    # TERRAIN: a report instead of guessing
    # -----------------------------------------------------------------------
    # The terrain has been black and every time the search for the cause
    # started with guessing where it is: in the data, in the quad
    # tree, in the visibility or only in the drawing. Those are FOUR
    # different jobs and without a measurement it is not known which.
    #
    # `RenderTerrain` has six places where it returns without drawing anything.
    # Each of them now reports itself - once, so as not to flood the log.
    (
        'GameLib/MapOutdoorRender.cpp',
        'void CMapOutdoor::RenderTerrain()\n'
        '{\n'
        '\tif (!IsVisiblePart(PART_TERRAIN))\n'
        '\t\treturn;\n'
        '\n'
        '\tif (!m_bSettingTerrainVisible)\n'
        '\t\treturn;\n'
        '\n'
        '\t// Inserted by levites\n'
        '\tif (!m_pTerrainPatchProxyList)\n'
        '\t\treturn;\n'
        '\n'
        '\tCCamera * pCamera = CCameraManager::Instance().GetCurrentCamera();\n'
        '\tif (!pCamera)\n'
        '\t\treturn;',
        'void CMapOutdoor::RenderTerrain()\n'
        '{\n'
        '\t// PORT: A REPORT OF WHERE TERRAIN DRAWING STOPS.\n'
        '\t//\n'
        '\t// Each of these returns means something else and leads to different\n'
        '\t// work. Without a report "the terrain is black" is an impression,\n'
        '\t// not a measurement - and exactly that mistake cost\n'
        '\t// a dozen rounds of work.\n'
        '\tstatic bool s_abReported[8] = { false };\n'
        '\t#define M2W_TERRAIN_BAIL(nr, reason) \\\n'
        '\t\tdo { if (!s_abReported[nr]) { s_abReported[nr] = true; \\\n'
        '\t\t\tprintf("m2w terrain: not drawing - %s\\n", reason); } \\\n'
        '\t\t\treturn; } while (0)\n'
        '\n'
        '\tif (!IsVisiblePart(PART_TERRAIN))\n'
        '\t\tM2W_TERRAIN_BAIL(0, "the TERRAIN part is turned off");\n'
        '\n'
        '\tif (!m_bSettingTerrainVisible)\n'
        '\t\tM2W_TERRAIN_BAIL(1, "setting: terrain invisible");\n'
        '\n'
        '\t// Inserted by levites\n'
        '\tif (!m_pTerrainPatchProxyList)\n'
        '\t\tM2W_TERRAIN_BAIL(2, "no terrain patch list");\n'
        '\n'
        '\tCCamera * pCamera = CCameraManager::Instance().GetCurrentCamera();\n'
        '\tif (!pCamera)\n'
        '\t\tM2W_TERRAIN_BAIL(3, "no camera");',
        'a report of where terrain drawing stops - six returns, each means '
        'different work',
    ),
    (
        'GameLib/MapOutdoorRender.cpp',
        '\tif (CTerrainPatch::SOFTWARE_TRANSFORM_PATCH_ENABLE)\n\t\t__RenderTerrain_RenderSoftwareTransformPatch();\n\telse\n\t\t__RenderTerrain_RenderHardwareTransformPatch();\n}',
        '\tif (CTerrainPatch::SOFTWARE_TRANSFORM_PATCH_ENABLE)\n\t\t__RenderTerrain_RenderSoftwareTransformPatch();\n\telse\n\t\t__RenderTerrain_RenderHardwareTransformPatch();\n\t#undef M2W_TERRAIN_BAIL\n}',
        'terrain: the end of the report macro above (#undef M2W_TERRAIN_BAIL)',
    ),
    (
        'SpeedTreeLib/SpeedTreeForestDirectX8.cpp',
        '#include "SpeedTreeForestDirectX8.h"',
        '#include "SpeedTreeForestDirectX8.h"\n\n// PORT: switch ?forest=1 - see compat/speedtree_web.cpp.\nextern "C" int M2W_ForestNoClip();',
        'forest: declaration of the ?forest=1 switch',
    ),
    (
        'SpeedTreeLib/SpeedTreeForestDirectX8.cpp',
        '\t\t\t\tif (ppInstances[i]->isShow())\n\t\t\t\t\tppInstances[i]->RenderBranches();',
        '\t\t\t\tif (ppInstances[i]->isShow() || M2W_ForestNoClip())\n\t\t\t\t\tppInstances[i]->RenderBranches();',
        'forest: ?forest=1 draws branches without the visibility test',
    ),
    (
        'SpeedTreeLib/SpeedTreeForestDirectX8.cpp',
        '\t\t\t\tif (ppInstances[i]->isShow())\n\t\t\t\t\tppInstances[i]->RenderFronds();',
        '\t\t\t\tif (ppInstances[i]->isShow() || M2W_ForestNoClip())\n\t\t\t\t\tppInstances[i]->RenderFronds();',
        'forest: ?forest=1 draws fronds without the visibility test',
    ),
    (
        'SpeedTreeLib/SpeedTreeForestDirectX8.cpp',
        '\t\t\t\tif (ppInstances[i]->isShow())\n\t\t\t\t\tppInstances[i]->RenderLeaves();',
        '\t\t\t\tif (ppInstances[i]->isShow() || M2W_ForestNoClip())\n\t\t\t\t\tppInstances[i]->RenderLeaves();',
        'forest: ?forest=1 draws leaves without the visibility test',
    ),
    (
        'GameLib/MapOutdoorRender.cpp',
        '\tSetBlendOperation();\n\tRenderArea();\n\tRenderTree();\n\tif (!m_bEnableTerrainOnlyForHeight)\n\t\tRenderTerrain();\n\tRenderBlendArea();\n#endif',
        '\tSetBlendOperation();\n\tRenderArea();\n\t// PORT: experiment `?forest=4` - trees AFTER the terrain. Decides\n\t// whether trees get lost in the depth test with the terrain (a different way of computing depth).\n\t{\n\t\textern int M2W_ForestMode();\n\t\tif (M2W_ForestMode() != 4)\n\t\t\tRenderTree();\n\t}\n\tif (!m_bEnableTerrainOnlyForHeight)\n\t\tRenderTerrain();\n\t{\n\t\textern int M2W_ForestMode();\n\t\tif (M2W_ForestMode() == 4)\n\t\t\tRenderTree();\n\t}\n\tRenderBlendArea();\n#endif',
        'forest: ?forest=4 draws trees after the terrain - a depth experiment',
    ),
    (
        'EterLib/GrpFontTexture.cpp',
        '\tif (m_x + size.cx >= (width - 1))\n\t{\n\t\tm_y += (m_step + 1);\n\t\tm_step = 0;\n\t\tm_x = 0;\n\n\t\tif (m_y + size.cy >= (height - 1))\n\t\t{\n\t\t\tif (!UpdateTexture())\n\t\t\t{\n\t\t\t\treturn NULL;\n\t\t\t}\n\n\t\t\tif (!AppendTexture())\n\t\t\t\treturn NULL;\n\n\t\t\tm_y = 0;\n\t\t}\n\t}\n',
        "\t// PORT - GUTTER between atlas cells. The original packed\n\t// characters WITHOUT a gap (m_x += size.cx directly) - on our GDI-\n\t// via-Canvas2D layer this measurably contaminated neighbours (a\n\t// measured, fixed case of 'y' overwriting 'L') and, even after\n\t// that fix, leaves a zero gap prone to bleeding\n\t// with linear texture filtering. We add it explicitly.\n\tconst int M2W_GUTTER = 2;\n\n\tif (m_x + size.cx + M2W_GUTTER >= (width - 1))\n\t{\n\t\tm_y += (m_step + 1 + M2W_GUTTER);\n\t\tm_step = 0;\n\t\tm_x = 0;\n\n\t\tif (m_y + size.cy >= (height - 1))\n\t\t{\n\t\t\tif (!UpdateTexture())\n\t\t\t{\n\t\t\t\treturn NULL;\n\t\t\t}\n\n\t\t\tif (!AppendTexture())\n\t\t\t\treturn NULL;\n\n\t\t\tm_y = 0;\n\t\t}\n\t}\n",
        'font atlas: a real gutter between cells',
    ),
    (
        'EterLib/GrpFontTexture.cpp',
        '\tm_x += size.cx;\n',
        '\tm_x += size.cx + M2W_GUTTER;\n',
        'font atlas: the packing step accounts for the gutter',
    ),
    (
        'EterBase/Debug.cpp',
        "\tszBuf[len] = '\\n';\n\tszBuf[len + 1] = '\\0';\n\n\ttime_t ct = time(0);",
        "\tszBuf[len] = '\\n';\n\tszBuf[len + 1] = '\\0';\n\n\t// PORT - SANITIZATION before going out to JS - A SAFEGUARD,\n\t// NOT a fix (the crash \"address zero\" had its source in LZO;\n\t// the canary showed TraceError did not take part in it).\n\t// When a `%s` in the format comes from damaged data,\n\t// the buffer may contain bytes INVALID as UTF-8 (e.g. a lone\n\t// 0xFE), and the emscripten stderr->JS console bridge decodes them as UTF-8\n\t// and reports an error. The cost is national characters in syserr. TraceError\n\t// is called from dozens of places across the code - one cannot guarantee\n\t// in advance that no `%s` argument will ever be broken,\n\t// so we cut it off HERE, in one place: every byte >= 0x80\n\t// we replace with '?' - an error message may come out a bit uglier,\n\t// but it will never again kill the whole game.\n\tfor (int iSanit = 0; iSanit < len; ++iSanit)\n\t{\n\t\tif ((unsigned char)szBuf[iSanit] >= 0x80)\n\t\t\tszBuf[iSanit] = '?';\n\t}\n\n\ttime_t ct = time(0);",
        'TraceError: sanitization of bytes >=0x80 before going out to JS '
        '(A SAFEGUARD, not a crash fix)',
    ),
    (
        'EterBase/Debug.cpp',
        '\t_vsnprintf(szBuf, sizeof(szBuf), c_szFormat, args);\n\tva_end(args);\n\n\ttime_t ct = time(0);\n\tstruct tm ctm = *localtime(&ct);\n\n\tfprintf(stderr, "%02d%02d %02d:%02d:%05d :: %s", \n',
        '\t_vsnprintf(szBuf, sizeof(szBuf), c_szFormat, args);\n\tva_end(args);\n\n\t// PORT - the same sanitization as in TraceError (see the\n\t// justification there) - without it one broken %s argument kills the whole game.\n\tfor (size_t iSanit = 0; iSanit < sizeof(szBuf) && szBuf[iSanit]; ++iSanit)\n\t{\n\t\tif ((unsigned char)szBuf[iSanit] >= 0x80)\n\t\t\tszBuf[iSanit] = \'?\';\n\t}\n\n\ttime_t ct = time(0);\n\tstruct tm ctm = *localtime(&ct);\n\n\tfprintf(stderr, "%02d%02d %02d:%02d:%05d :: %s", \n',
        'TraceErrorWithoutEnter: sanitization of bytes >=0x80 (a '
        'safeguard, not a fix)',
    ),
]


def apply_patches(stage):
    """Applies every entry of PATCHES to the staged tree: the pattern must occur
    exactly as many times as declared (default once) or the script stops, so a
    patch can never land in the wrong place.
    """
    for entry in PATCHES:
        # The fifth element is optional: HOW MANY occurrences of the pattern are to be
        # replaced. By default ONE. More has to be declared
        # explicitly - a pattern hit two places, of which the
        # second was something else, and the replacement broke it.
        rel, old, new, why = entry[0], entry[1], entry[2], entry[3]
        expected_values = entry[4] if len(entry) > 4 else 1
        path = os.path.join(stage, rel.replace('/', os.sep))
        with open(path, encoding='utf-8', errors='ignore') as fh:
            text = fh.read()
        # The pattern must occur EXACTLY ONCE. Once the pattern
        # 'int pos = 0;' hit TWO places - one was a
        # `PyDict_Next` loop, the other an ordinary index passed to
        # `PyTuple_GetInteger`. Replacing both broke the second.
        # The compiler caught it at once, but better not to allow it.
        how_many = text.count(old)
        if how_many != expected_values:
            raise SystemExit(
                'AMBIGUOUS PATTERN: ' + rel + '\n'
                '  searched: ' + old + '\n'
                '  occurrences: ' + str(how_many) + ', expected ' + str(expected_values) +
                ' - the patch could land in the wrong place. Lengthen the '
                'pattern or declare the number of occurrences in the fifth field.')
        if old not in text:
            raise SystemExit(
                'PATCH DID NOT HIT: ' + rel + '\n'
                '  searched: ' + old + '\n'
                '  reason for the patch: ' + why + '\n'
                '  The TMP4 source has changed or the patch is no longer needed.')
        with open(path, 'w', encoding='utf-8') as fh:
            fh.write(text.replace(old, new, expected_values))
        print(f'  patch: {rel}  <- {why.split(" - ")[0][:60]}')


def copy_tree(src, dst):
    """Copies the C/C++ sources and headers (.h .hpp .cpp .c .inl) of `src` into
    `dst`, keeping the layout; returns how many files (0 when `src` is absent).
    """
    if not os.path.isdir(src):
        return 0
    n = 0
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        out = os.path.join(dst, rel) if rel != '.' else dst
        os.makedirs(out, exist_ok=True)
        for f in files:
            if os.path.splitext(f)[1].lower() not in ('.h', '.hpp', '.cpp', '.c', '.inl'):
                continue
            shutil.copy2(os.path.join(root, f), os.path.join(out, f))
            n += 1
    return n


def main():
    """Rebuilds the stage tree from scratch: TMP4 libraries unchanged, then our
    compat/tree overlays, then the point patches.
    """
    stage = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_STAGE
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    print('from TMP4 (unchanged):')
    for lib in FROM_TMP4:
        n = copy_tree(os.path.join(TMP4, lib), os.path.join(stage, lib))
        print(f'  {lib:<14} {n:4d} files')

    print('ours (covering):')
    if not os.path.isdir(OURS):
        print('  (nothing yet)')
    else:
        for lib in sorted(os.listdir(OURS)):
            p = os.path.join(OURS, lib)
            if not os.path.isdir(p):
                continue
            n = copy_tree(p, os.path.join(stage, lib))
            print(f'  {lib:<14} {n:4d} files')

    print('point patches:')
    apply_patches(stage)

    print(f'\ntree assembled in: {stage}')


if __name__ == '__main__':
    main()
