// SPDX-License-Identifier: GPL-2.0-or-later
// imm.h - IME (Asian text input) in Windows.
//
// In the browser the browser itself handles IME - the text arrives ready
// through the `compositionend` and `input` events. There is no Win32 layer
// to reproduce, so the header is EMPTY. Should `EterLib` reach for the
// `Imm*` functions, the compiler will report it and it will be a separate
// decision.

#pragma once

#include "win32_compat.h"

// ---------------------------------------------------------------------------
// IME structures - from measuring `UserInterface`
// ---------------------------------------------------------------------------
// At first this file was empty, because `GameLib` does not touch IME.
// `EterLib` and `UserInterface` do: 22 files were blocked on
// `COMPOSITIONFORM`. The same mistake as with `dinput.h` - a substitute for
// one client.
//
// The structures are reproduced, because TMP4 code reads their fields. The
// `Imm*` functions DO NOT EXIST: composing Asian characters is done in the
// browser by the browser itself (the `compositionstart` /
// `compositionupdate` / `compositionend` events), so it is a DECISION about
// translating events, not a missing function.

DECLARE_HANDLE(HIMC);
DECLARE_HANDLE(HIMCC);

/// Where the composition window goes (style, point, area).
typedef struct tagCOMPOSITIONFORM {
    DWORD dwStyle;
    POINT ptCurrentPos;
    RECT  rcArea;
} COMPOSITIONFORM, *PCOMPOSITIONFORM, *LPCOMPOSITIONFORM;

/// Where a candidate window goes.
typedef struct tagCANDIDATEFORM {
    DWORD dwIndex;
    DWORD dwStyle;
    POINT ptCurrentPos;
    RECT  rcArea;
} CANDIDATEFORM, *PCANDIDATEFORM, *LPCANDIDATEFORM;

/// A candidate list header (offsets to the strings follow).
typedef struct tagCANDIDATELIST {
    DWORD dwSize;
    DWORD dwStyle;
    DWORD dwCount;
    DWORD dwSelection;
    DWORD dwPageStart;
    DWORD dwPageSize;
    DWORD dwOffset[1];
} CANDIDATELIST, *PCANDIDATELIST, *LPCANDIDATELIST;

/// An IME style entry.
typedef struct tagSTYLEBUFW {
    DWORD dwStyle;
    WCHAR szDescription[32];
} STYLEBUFW, *LPSTYLEBUFW;

#define CFS_DEFAULT       0x0000
#define CFS_RECT          0x0001
#define CFS_POINT         0x0002
#define CFS_FORCE_POSITION 0x0020
#define CFS_CANDIDATEPOS  0x0040
#define CFS_EXCLUDE       0x0080

#define GCS_COMPSTR       0x0008
#define GCS_COMPATTR      0x0010
#define GCS_CURSORPOS     0x0080
#define GCS_RESULTSTR     0x0800

#define IMN_OPENCANDIDATE   0x0005
#define IMN_CLOSECANDIDATE  0x0004
#define IMN_CHANGECANDIDATE 0x0003

/// The input context. TMP4 code reads **one field** from it - `hPrivate`
/// (checked with grep: six occurrences of `lpIC->hPrivate`, and nothing
/// more). The rest of the original structure serves the system itself.
///
/// The fields before `hPrivate` are here so that **the offset matches**
/// Win32 - an exception to the rule of `D3DCAPS8`, where I did not reproduce
/// the layout. The difference: `D3DCAPS8` is filled by our layer, while
/// `INPUTCONTEXT` would be filled by the system. In the web port there is no
/// system, so in practice it does not matter - but should someone ever hook
/// up a real IME, layout compatibility is already taken care of.
typedef struct tagINPUTCONTEXT {
    HWND            hWnd;
    BOOL            fOpen;
    POINT           ptStatusWndPos;
    POINT           ptSoftKbdPos;
    DWORD           fdwConversion;
    DWORD           fdwSentence;
    union { LOGFONTA A; LOGFONTW W; } lfFont;
    COMPOSITIONFORM cfCompForm;
    CANDIDATEFORM   cfCandForm[4];
    HIMCC           hCompStr;
    HIMCC           hCandInfo;
    HIMCC           hGuideLine;
    HIMCC           hPrivate;
    DWORD           dwNumMsgBuf;
    HIMCC           hMsgBuf;
    DWORD           fdwInit;
    DWORD           dwReserve[3];
} INPUTCONTEXT, *LPINPUTCONTEXT;

// IME constants `eterLib/IME.h` uses. Values from Win32 - they cost nothing,
// and they remove the question whether this particular number mattered.
#define KL_NAMELENGTH 9

#define IMC_GETCANDIDATEPOS   0x0007
#define IMC_SETCANDIDATEPOS   0x0008
#define IMC_GETCOMPOSITIONFONT 0x0009
#define IMC_SETCOMPOSITIONFONT 0x000A
#define IMC_GETCOMPOSITIONWINDOW 0x000B
#define IMC_SETCOMPOSITIONWINDOW 0x000C
#define IMC_GETOPENSTATUS     0x0005
#define IMC_SETOPENSTATUS     0x0006

#define NI_COMPOSITIONSTR     0x0015
#define CPS_CANCEL            0x0004
#define CPS_COMPLETE          0x0001

#define ATTR_INPUT            0x00
#define ATTR_TARGET_CONVERTED 0x01
#define ATTR_CONVERTED        0x02
#define ATTR_TARGET_NOTCONVERTED 0x03

#define IMR_COMPOSITIONWINDOW 0x0001
#define IMR_CANDIDATEWINDOW   0x0002
#define IMR_QUERYCHARPOSITION 0x0006

#define GCL_REVERSECONVERSION 0x0002
#define GCS_COMPREADSTR       0x0001

// The `Imm*` functions - **without bodies**. The browser itself composes
// characters; translating its events into these calls is a decision of the
// input layer, not a missing system function.
/// Declared only - no body (see the note above).
HIMC  ImmGetContext(HWND hwnd);
/// Declared only - no body (see the note above).
BOOL  ImmReleaseContext(HWND hwnd, HIMC himc);
/// Declared only - no body (see the note above).
LONG  ImmGetCompositionStringA(HIMC himc, DWORD index, void* buf, DWORD len);
/// Declared only - no body (see the note above).
LONG  ImmGetCompositionStringW(HIMC himc, DWORD index, void* buf, DWORD len);
/// Declared only - no body (see the note above).
BOOL  ImmSetCompositionWindow(HIMC himc, LPCOMPOSITIONFORM form);
/// Declared only - no body (see the note above).
BOOL  ImmSetCandidateWindow(HIMC himc, LPCANDIDATEFORM form);
/// Declared only - no body (see the note above).
BOOL  ImmNotifyIME(HIMC himc, DWORD action, DWORD index, DWORD value);
/// Declared only - no body (see the note above).
DWORD ImmGetCandidateListW(HIMC himc, DWORD index, LPCANDIDATELIST list, DWORD len);
/// Declared only - no body (see the note above).
BOOL  ImmGetOpenStatus(HIMC himc);
/// Declared only - no body (see the note above).
BOOL  ImmSetOpenStatus(HIMC himc, BOOL open);
/// Declared only - no body (see the note above).
BOOL  ImmGetConversionStatus(HIMC himc, DWORD* conversion, DWORD* sentence);
/// Declared only - no body (see the note above).
BOOL  ImmSetConversionStatus(HIMC himc, DWORD conversion, DWORD sentence);
