// SPDX-License-Identifier: GPL-2.0-or-later
// tlhelp32.h - a snapshot of processes and modules (ToolHelp32).
//
// The client uses it to detect foreign processes - the anti-cheat layer. In
// the browser there is nothing to look into: page code does not see the
// system. The functions have bodies in `platform_none.cpp` that enumerate
// NOTHING (no snapshot, no first process, no first module), so the
// scanners find nothing - said plainly there: this protection does not
// work in the port.

#pragma once

#include "win32_compat.h"

// The snapshot structures. I do not invent fields - I reproduce the ones TMP4
// code reads.
#define TH32CS_SNAPPROCESS 0x00000002
#define TH32CS_SNAPMODULE  0x00000008
#define TH32CS_SNAPTHREAD  0x00000004
#define MAX_MODULE_NAME32  255

typedef struct tagPROCESSENTRY32 {
    DWORD     dwSize;
    DWORD     cntUsage;
    DWORD     th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD     th32ModuleID;
    DWORD     cntThreads;
    DWORD     th32ParentProcessID;
    LONG      pcPriClassBase;
    DWORD     dwFlags;
    char      szExeFile[260];
} PROCESSENTRY32, *PPROCESSENTRY32, *LPPROCESSENTRY32;

typedef struct tagMODULEENTRY32 {
    DWORD  dwSize;
    DWORD  th32ModuleID;
    DWORD  th32ProcessID;
    DWORD  GlblcntUsage;
    DWORD  ProccntUsage;
    BYTE*  modBaseAddr;
    DWORD  modBaseSize;
    HMODULE hModule;
    char   szModule[MAX_MODULE_NAME32 + 1];
    char   szExePath[260];
} MODULEENTRY32, *PMODULEENTRY32, *LPMODULEENTRY32;

// Bodies in `platform_none.cpp` - see the note at the top of the file.
/// Body in `platform_none.cpp`: always `INVALID_HANDLE_VALUE` (no snapshot).
HANDLE CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
/// Body in `platform_none.cpp`: always `FALSE` (no process).
BOOL   Process32First(HANDLE hSnapshot, LPPROCESSENTRY32 lppe);
/// Body in `platform_none.cpp`: always `FALSE` (no process).
BOOL   Process32Next(HANDLE hSnapshot, LPPROCESSENTRY32 lppe);
/// Body in `platform_none.cpp`: always `FALSE` (no module).
BOOL   Module32First(HANDLE hSnapshot, LPMODULEENTRY32 lpme);
/// Body in `platform_none.cpp`: always `FALSE` (no module).
BOOL   Module32Next(HANDLE hSnapshot, LPMODULEENTRY32 lpme);
