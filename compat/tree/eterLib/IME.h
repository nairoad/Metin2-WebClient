// eterLib/IME.h - OUR header in place of the TMP4 one.
//
// ===========================================================================
// WHY
// ===========================================================================
// The original starts with `#include <imm.h>` and `#pragma comment(lib,
// "imm32.lib")`, and its protected part is built entirely on IMM32:
// `HIMC`, `HIMCC`, `INPUTCONTEXT`, `HKL`, pointers to `ImmLockIMC`
// and `ImmUnlockIMCC` fetched through `GetProcAddress` from `imm32.dll`, the
// system version in `OSVERSIONINFOA`, the identifiers `ms_adwId` used to recognise
// WHICH Chinese IME is switched on.
//
// In the browser there is not a single one of these - and, more importantly, **there is
// nothing to replace**. The browser itself composes Asian characters
// and draws the candidate list itself; the program receives finished text
// in the events `compositionstart` / `compositionupdate` / `compositionend`.
//
// ===========================================================================
// WHAT STAYS AND WHAT GOES
// ===========================================================================
// The whole public part STAYS, to the signature - because `PythonIME.cpp`,
// `PythonIMEModule.cpp`, `PythonApplication.h` and `GrpTextInstance.cpp`
// compile against exactly it today.
//
// The **state of the text field** STAYS too: the buffer `m_wText`, the cursor
// position, the length of the string being composed, the underline. This is the heart of the class and it has
// nothing to do with IMM32 - every text field of the client (login, chat,
// character name) is this same buffer.
//
// ONLY the fields typed with IMM32 and Windows GO:
//   `m_hOrgIMC`, `_ImmLockIMC` and three more pointers, `ms_hklCurrent`,
//   `ms_szKeyboardLayout`, `ms_stOSVI`, `ms_hImm32Dll`, `ms_hCurrentImeDll`,
//   `ms_dwImeState`, `ms_adwId`, `ms_dwIMELevel`, `ms_dwIMELevelSaved`,
//   `ms_bUILessMode`, `ms_bChineseIME`, `ms_bUseIMMCandidate`
// and the protected methods that existed ONLY to serve them
// (`ResultProcess`, `CompositionProcess*`, `AttributeProcess`,
// `CandidateProcess`, `ReadingProcess`, `GetImeId`, `SetupImeApi`,
// `CheckInputLocale`, `SetSupportLevel`, `GetCodePageFromLang`,
// `GetReadingWindowOrientation`).
//
// THE MEASUREMENT that allows it: `CIME::ms_*` does not occur **even once** outside
// `IME.cpp` itself. Checked with grep over the whole tree. So the layout of this class is
// our business in the port - just as with `CSoundManager`.

#pragma once

// The standard library headers MUST come BEFORE `win32_compat.h`:
// the latter defines `min` and `max` as macros, and `<vector>` and `<string>`
// contain `std::min`/`std::max` in a form the macro does not let through.
// TMP4's StdAfx has the same order - there `windows.h` forced it for the
// same reason.
#include <string>
#include <vector>

#include "win32_compat.h"

class IIMEEventSink
{
public:
    virtual bool OnWM_CHAR(WPARAM wParam, LPARAM lParam) = 0;
    virtual void OnUpdate() = 0;

    virtual void OnChangeCodePage() = 0;

    virtual void OnOpenCandidateList() = 0;
    virtual void OnCloseCandidateList() = 0;

    virtual void OnOpenReadingWnd() = 0;
    virtual void OnCloseReadingWnd() = 0;
};

class CIME
{
public:
    enum
    {
        IMEREADING_MAXLEN = 128,
        IMESTR_MAXLEN = 1024,
        IMECANDIDATE_MAXLEN = 32768,
        MAX_CANDLIST = 10,
        MAX_CANDIDATE_LENGTH = 256
    };

public:
    CIME();
    virtual ~CIME();

    bool Initialize(HWND hWnd);
    void Uninitialize(void);

    static void Clear();

    void SetMax(int iMax);
    void SetUserMax(int iMax);
    void SetText(const char* c_szText, int len);
    int  GetText(std::string& rstrText, bool addCodePage = false);
    const char* GetCodePageText();
    int  GetCodePage();

    // Candidate list - in the browser the browser draws it.
    int  GetCandidateCount();
    int  GetCandidatePageCount();
    int  GetCandidate(DWORD index, std::string& rstrText);
    int  GetCandidateSelection();

    // Reading hint - as above.
    int  GetReading(std::string& rstrText);
    int  GetReadingError();

    void  SetInputMode(DWORD dwMode);
    DWORD GetInputMode();

    bool IsIMEEnabled();
    void EnableIME(bool bEnable = true);
    void DisableIME();

    void EnableCaptureInput();
    void DisableCaptureInput();
    bool IsCaptureEnabled();

    void SetNumberMode();
    void SetStringMode();
    bool __IsWritable(wchar_t key);
    void AddExceptKey(wchar_t key);
    void ClearExceptKey();

    /// OUR ADDITION, which TMP4 did not have. The browser speaks only
    /// UTF-8 - that is how text arrives from character composition and from the
    /// `paste` event. `PasteString` converts according to `ms_uInputCodePage`, so for
    /// these two roads it would give zero. It is the same road, only it is known
    /// up front what the string is in.
    void PasteUtf8(const char* szUtf8);

    void PasteTextFromClipBoard();
    void EnablePaste(bool bFlag);
    void PasteString(const char* str);
    static void FinalizeString(bool bSend = false);

    void UseDefaultIME();

    static int GetCurPos();
    static int GetCompLen();
    static int GetULBegin();
    static int GetULEnd();

    static void CloseCandidateList();
    static void CloseReadingInformation();
    static void ChangeInputLanguage();
    static void ChangeInputLanguageWorker();

    LRESULT WMInputLanguage(HWND hWnd, UINT uiMsg, WPARAM wParam, LPARAM lParam);
    LRESULT WMStartComposition(HWND hWnd, UINT uiMsg, WPARAM wParam, LPARAM lParam);
    LRESULT WMComposition(HWND hWnd, UINT uiMsg, WPARAM wParam, LPARAM lParam);
    LRESULT WMEndComposition(HWND hWnd, UINT uiMsg, WPARAM wParam, LPARAM lParam);
    LRESULT WMNotify(HWND hWnd, UINT uiMsg, WPARAM wParam, LPARAM lParam);
    LRESULT WMChar(HWND hWnd, UINT uiMsg, WPARAM wParam, LPARAM lParam);

protected:
    // They stay PROTECTED, as in TMP4: the derived class calls them
    // (`CPythonIME`), not outside code. Widening the access would break nothing,
    // but would not achieve anything either - so it stays as it was.
    void IncCurPos();
    void DecCurPos();
    void SetCurPos(int offset);
    void DelCurPos();

protected:
    static void CheckToggleState();

    void InsertString(wchar_t* szString, int iSize);
    void OnChar(wchar_t c);
    bool IsMax(const wchar_t* wInput, int len);

protected:
    int m_max;
    int m_userMax;

    BOOL m_bOnlyNumberMode;

    std::vector<wchar_t> m_exceptKey;

    bool m_bEnablePaste;
    bool m_bUseDefaultIME;

public:
    static bool ms_bInitialized;
    static bool ms_bDisableIMECompletely;
    static bool ms_bImeEnabled;
    static bool ms_bCaptureInput;

    static HWND ms_hWnd;

    // Candidate list
    static bool  ms_bCandidateList;
    static DWORD ms_dwCandidateCount;
    static bool  ms_bVerticalCandidate;
    static int   ms_iCandListIndexBase;
    static WCHAR ms_wszCandidate[CIME::MAX_CANDLIST][MAX_CANDIDATE_LENGTH];
    static DWORD ms_dwCandidateSelection;
    static DWORD ms_dwCandidatePageSize;

    // Reading hint
    static bool ms_bReadingInformation;
    static int  ms_iReadingError;
    static bool ms_bHorizontalReading;
    static std::vector<wchar_t> ms_wstrReading;

    static wchar_t* ms_wszCurrentIndicator;

    static IIMEEventSink* ms_pEvent;

    wchar_t        m_wszComposition[IMESTR_MAXLEN];
    static wchar_t m_wText[IMESTR_MAXLEN];

    static int ms_compLen;
    static int ms_curpos;
    static int ms_lastpos;
    static int ms_ulbegin;
    static int ms_ulend;

    static UINT ms_uOutputCodePage;
    static UINT ms_uInputCodePage;
};
