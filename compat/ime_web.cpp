// SPDX-License-Identifier: GPL-2.0-or-later
// ime_web.cpp - the client's text field without IMM32: the body of our
// `eterLib/IME.h`, replacing `eterLib/IME.cpp` (2302 lines, most
// of them Windows input-method plumbing that the browser does itself).

// Design:
// WHAT IS REPLACED. `eterLib/IME.cpp` is mostly IMM32: `ImmLockIMC` pulled
// out of `imm32.dll` by `GetProcAddress`, Chinese IMEs recognised by
// `ms_adwId`, the "UI-less" mode through TSF, the candidate window, the
// reading window, keyboard-layout tracking. In the browser there is NOTHING
// of that to port, and it is not a gap: the browser composes the
// characters, draws the candidate list and shows the reading hint; the
// program receives FINISHED text.
//
// WHAT STAYS - and it is the right part. The heart of `CIME` has nothing to
// do with IMM32: it is the TEXT FIELD BUFFER - `m_wText` of 1024 wide
// characters, the caret (`ms_curpos`), the end of the string (`ms_lastpos`),
// the length limits (`m_max` / `m_userMax`), the digits-only mode, the list
// of forbidden characters. Every field of the client - login, chat,
// character name, shop amount - is this one buffer. That part is copied
// line for line, colour tags included: the caret in Metin2 does not move by
// one character but jumps over a WHOLE `|cFFFFFFFF` tag, or the player would
// edit the middle of a tag. `FindColorTagEndPosition` and its sisters from
// `eterLib/TextTag.h` compile unchanged.
//
// HOW CHARACTERS ARRIVE (checked with git grep, correcting the
// comments this file carried):
// `m2w.eventsStart` (runtime.js) turns every `keydown` with a one-character
// `e.key` into `WM_CHAR` carrying the UTF-16 code unit; `WM_CHAR` reaches
// `WMChar` through `PythonApplicationProcedure`. TMP4 took `wParam & 0xff`
// - ONE byte - and put it through `MultiByteToWideChar` in the input code
// page; the browser gives a finished Unicode character. So BOTH forms are
// accepted: `wParam` up to 0xFF goes the old way (the client code that
// calls it that way keeps working), above 0xFF it is taken as the
// character. No `WM_IME_*` message is ever posted in the port, no
// `compositionstart`/`compositionend` or `paste` listener exists, and the
// element `m2w_ime` that `EnableIME` would focus is created by no page:
// the `M2W_Ime*` exports below are the DESIGNED entry for such listeners
// (`Module.ccall`) and today have no caller. Consequences to fix in a
// behaviour commit, not here: composed (CJK) text and Ctrl+V do not reach
// the field (Ctrl+V posts `WM_CHAR` 'v' like a plain key).
//
// PASTE. `PasteTextFromClipBoard` read the clipboard IN PLACE on Windows.
// In the browser a clipboard read is asynchronous and needs consent, but
// need not be asked for: on Ctrl+V the browser sends a `paste` event with
// the text. The source of truth is therefore the event (`M2W_ImePaste`),
// and `PasteTextFromClipBoard` is DELIBERATELY empty - reading the
// clipboard here too would paste the same text twice once the listener
// exists.
//
// CANDIDATES AND READING. `GetCandidateCount`, `GetCandidate`,
// `GetCandidateSelection`, `GetCandidatePageCount`, `GetReading`,
// `GetReadingError` return "nothing". That is the TRUE answer, not a stub:
// this data served to draw the character-choice window, which the system
// draws in the browser; filled in, the client would draw a SECOND one next
// to the real one.
//
// CODE PAGE OF THE FIELD. In the original both code pages were
// set by `OnInputLanguageChange` from the Windows keyboard layout
// (`GetLocaleInfoA(..., LOCALE_IDEFAULTANSICODEPAGE)`). The browser has no
// keyboard layout, so earlier both stayed ZERO - and zero is `CP_ACP`,
// which `platform_codec.cpp` treats as UTF-8: "zazolc" typed in the chat
// came out as two junk characters per letter (every Polish letter as two
// UTF-8 bytes, read back in the game's code page - 1250 for the Polish
// localisation, the one scripts get from `app.GetDefaultCodePage()`). The
// right page is the page of the GAME LOCALISATION - `LocaleService_GetCodePage`
// from `UserInterface/Locale.cpp`, in the same binary (`ImeCodePage`).

// Order matters - see the note in `eterLib/IME.h`: `win32_compat.h` turns
// `min` and `max` into macros, so the standard library must come first.
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "win32_compat.h"

#include <emscripten.h>

#include "eterLib/IME.h"
#include "eterLib/TextTag.h"

/// Free functions of TMP4: `GetDefaultCodePage` lives in `eterLib/Util.cpp`,
/// `FindToken` and `ReadToken` in `eterLib/GrpTextInstance.cpp`.
extern DWORD GetDefaultCodePage();
/// TMP4 (`GrpTextInstance.cpp`): the first code-page token (`@` and four
/// digits) in [begin, end), or `end` when there is none.
extern const char* FindToken(const char* begin, const char* end);
/// TMP4 (`GrpTextInstance.cpp`): the code page named by the token at `token`
/// (its four digits; 9999 means CP_UTF8).
extern int ReadToken(const char* token);

/// `UserInterface/Locale.cpp`: the code page of the game localisation.
extern "C++" unsigned int LocaleService_GetCodePage();

// ---------------------------------------------------------------------------
// Bridge to m2w.imeEnable (runtime.js)
// ---------------------------------------------------------------------------

/// Focuses (1) or blurs (0) the hidden text element `m2w_ime` - which no
/// page creates today, so this is a no-op until one does (see "Design").
EM_JS(void, m2w_ime_enable, (int iEnabled), { m2w.imeEnable(iEnabled); });

// ---------------------------------------------------------------------------
// Static fields - the same ones TMP4 defines at the top of `IME.cpp`
// ---------------------------------------------------------------------------

int CIME::ms_compLen;
int CIME::ms_curpos;
int CIME::ms_lastpos;
int CIME::ms_ulbegin;
int CIME::ms_ulend;

wchar_t CIME::m_wText[CIME::IMESTR_MAXLEN];

bool CIME::ms_bInitialized = false;
bool CIME::ms_bDisableIMECompletely = false;
bool CIME::ms_bImeEnabled = false;
bool CIME::ms_bCaptureInput = false;

HWND CIME::ms_hWnd;

bool  CIME::ms_bCandidateList;
DWORD CIME::ms_dwCandidateCount;
bool  CIME::ms_bVerticalCandidate;
int   CIME::ms_iCandListIndexBase;
WCHAR CIME::ms_wszCandidate[CIME::MAX_CANDLIST][CIME::MAX_CANDIDATE_LENGTH];
DWORD CIME::ms_dwCandidateSelection;
DWORD CIME::ms_dwCandidatePageSize;

bool CIME::ms_bReadingInformation;
int  CIME::ms_iReadingError = 0;
bool CIME::ms_bHorizontalReading;
std::vector<wchar_t> CIME::ms_wstrReading;

wchar_t* CIME::ms_wszCurrentIndicator;

IIMEEventSink* CIME::ms_pEvent;

UINT CIME::ms_uOutputCodePage = 0;
UINT CIME::ms_uInputCodePage = 0;

namespace
{

/// The code page to use: the one set explicitly, else the localisation's,
/// else 1250 (see "Design").
UINT ImeCodePage(UINT uSet)
{
    if (uSet != 0)
        return uSet;
    const unsigned int u = LocaleService_GetCodePage();
    return u ? u : 1250;
}

/// The only live `CIME` in the client. Needed because composition events
/// come from the browser, not through an object - exactly as on Windows
/// they came to the window, not to the class. (The composition buffer is a
/// NON-static field in TMP4 and `FinalizeString` is static and does not
/// touch it; that stays.)
CIME* s_pIME = NULL;

}  // namespace

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

CIME::CIME()
{
    ms_hWnd = NULL;

    ms_bCandidateList = false;
    ms_bReadingInformation = false;

    Clear();

    m_max = 0;
    m_userMax = 0;

    m_bOnlyNumberMode = FALSE;

    m_bEnablePaste = false;
    m_bUseDefaultIME = false;

    m_wszComposition[0] = 0;

    s_pIME = this;
}

/// TMP4 frees `imm32.dll` and the current IME library here; nothing to
/// free - neither was loaded.
CIME::~CIME()
{
    if (s_pIME == this)
        s_pIME = NULL;
}

/// TMP4 checks the OS version here, loads `imm32.dll`, pulls four functions
/// out of it and disables the "text frame service". None of that has a
/// browser counterpart, but the RESULT is the same: composition works, only
/// somebody else does it. Called from `PythonIMEModule` (`ime.Initialize`).
bool CIME::Initialize(HWND hWnd)
{
    if (ms_bInitialized)
        return true;

    ms_hWnd = hWnd;

    ms_bDisableIMECompletely = false;
    ms_bInitialized = true;

    return true;
}

void CIME::Uninitialize()
{
    if (!ms_bInitialized)
        return;

    ms_hWnd = NULL;
    ms_bInitialized = false;
}

void CIME::Clear()
{
    ms_lastpos = 0;
    ms_curpos = 0;

    ms_compLen = 0;
    ms_ulbegin = 0;
    ms_ulend = 0;
}

// ---------------------------------------------------------------------------
// Text buffer - copied from TMP4 line for line
// ---------------------------------------------------------------------------

void CIME::SetMax(int iMax)      { m_max = iMax; }
void CIME::SetUserMax(int iMax)  { m_userMax = iMax; }

/// Replaces the buffer with `szText` in the input code page. The string may
/// CHANGE code page in the middle - the `@nnnn` marker written by
/// `GetCodePageText`; five bytes is the length of that marker.
void CIME::SetText(const char* szText, int len)
{
    ms_compLen = 0;
    ms_ulbegin = 0;
    ms_ulend = 0;

    const char* begin = szText;
    const char* end = begin + len;
    const char* iter = FindToken(begin, end);

    const int m_wTextLen = sizeof(m_wText) / sizeof(wchar_t);

    ms_lastpos = MultiByteToWideChar(ImeCodePage(ms_uInputCodePage), 0, begin,
                                     static_cast<int>(iter - begin),
                                     m_wText, m_wTextLen);

    if (iter < end)
        ms_lastpos += MultiByteToWideChar(ReadToken(iter), 0, (iter + 5),
                                          static_cast<int>(end - (iter + 5)),
                                          m_wText + ms_lastpos,
                                          m_wTextLen - ms_lastpos);

    ms_curpos = min(ms_curpos, ms_lastpos);
}

/// Appends the field's text in the output code page (Vietnamese, 1268:
/// the data is UTF-8 although the page says otherwise - as in TMP4). The
/// string has THREE parts: text before the caret, the string being
/// composed, text after the caret - the composition sits IN THE MIDDLE,
/// which is why the field shows it where the caret stands.
int CIME::GetText(std::string& rstrText, bool /*addCodePage*/)
{
    const int outCodePage = static_cast<int>(ImeCodePage(ms_uOutputCodePage));
    int dataCodePage;
    switch (outCodePage)
    {
        case 1268:
            dataCodePage = CP_UTF8;
            break;
        default:
            dataCodePage = outCodePage;
    }

    int len = 0;
    char text[IMESTR_MAXLEN];

    len += WideCharToMultiByte(dataCodePage, 0, m_wText, ms_curpos,
                               text, static_cast<int>(sizeof(text)) - len, NULL, NULL);
    len += WideCharToMultiByte(dataCodePage, 0, m_wszComposition, ms_compLen,
                               text + len, static_cast<int>(sizeof(text)) - len, NULL, NULL);
    len += WideCharToMultiByte(dataCodePage, 0, m_wText + ms_curpos,
                               ms_lastpos - ms_curpos,
                               text + len, static_cast<int>(sizeof(text)) - len, NULL, NULL);

    rstrText.append(text, text + len);

    return static_cast<int>(rstrText.size());
}

/// `@nnnn` when the output page differs from the game's default, else "".
const char* CIME::GetCodePageText()
{
    static char szCodePage[16];

    const int defCodePage = static_cast<int>(GetDefaultCodePage());
    const int outCodePage = static_cast<int>(ImeCodePage(ms_uOutputCodePage));

    if (outCodePage != defCodePage)
        sprintf(szCodePage, "@%04d", outCodePage);
    else
        szCodePage[0] = 0;

    return szCodePage;
}

int CIME::GetCodePage()
{
    return static_cast<int>(ImeCodePage(ms_uOutputCodePage));
}

void CIME::SetNumberMode() { m_bOnlyNumberMode = TRUE; }
void CIME::SetStringMode() { m_bOnlyNumberMode = FALSE; }

void CIME::AddExceptKey(wchar_t key) { m_exceptKey.push_back(key); }
void CIME::ClearExceptKey()          { m_exceptKey.clear(); }

bool CIME::__IsWritable(wchar_t key)
{
    return m_exceptKey.end() == std::find(m_exceptKey.begin(), m_exceptKey.end(), key);
}

/// Whether `len` more characters would exceed the buffer, `m_max` (bytes in
/// the output page) or `m_userMax` - the second limit counts WITHOUT colour
/// tags, i.e. what the player sees.
bool CIME::IsMax(const wchar_t* wInput, int len)
{
    if (ms_lastpos + len > IMESTR_MAXLEN)
        return true;

    const int textLen = WideCharToMultiByte(ImeCodePage(ms_uOutputCodePage), 0, m_wText,
                                            ms_lastpos, 0, 0, NULL, NULL);
    const int inputLen = WideCharToMultiByte(ImeCodePage(ms_uOutputCodePage), 0, wInput,
                                             len, 0, 0, NULL, NULL);

    if (textLen + inputLen > m_max)
        return true;

    if (m_userMax != 0 && m_max != m_userMax)
    {
        const std::wstring str = GetTextTagOutputString(m_wText, ms_lastpos);
        const std::wstring input = GetTextTagOutputString(wInput, len);
        const int t = WideCharToMultiByte(ImeCodePage(ms_uOutputCodePage), 0, str.c_str(),
                                          static_cast<int>(str.length()), 0, 0, NULL, NULL);
        const int i = WideCharToMultiByte(ImeCodePage(ms_uOutputCodePage), 0, input.c_str(),
                                          static_cast<int>(input.length()), 0, 0, NULL, NULL);
        return t + i > m_userMax;
    }

    return false;
}

void CIME::InsertString(wchar_t* wString, int iSize)
{
    if (IsMax(wString, iSize))
        return;

    if (ms_curpos < ms_lastpos)
        memmove(m_wText + ms_curpos + iSize, m_wText + ms_curpos,
                sizeof(wchar_t) * (ms_lastpos - ms_curpos));

    memcpy(m_wText + ms_curpos, wString, sizeof(wchar_t) * iSize);

    ms_curpos += iSize;
    ms_lastpos += iSize;
}

void CIME::OnChar(wchar_t c)
{
    if (m_bOnlyNumberMode)
        if (!iswdigit(static_cast<wint_t>(c)))
            return;

    if (!__IsWritable(c))
        return;

    InsertString(&c, 1);
}

// --- caret: jumps over a WHOLE colour tag, not one character ----------------

void CIME::IncCurPos()
{
    if (ms_curpos < ms_lastpos)
    {
        const int pos = FindColorTagEndPosition(m_wText + ms_curpos,
                                                ms_lastpos - ms_curpos);
        if (pos > 0)
            ms_curpos = min(ms_lastpos, max(0, ms_curpos + (pos + 1)));
        else
            ++ms_curpos;
    }
}

void CIME::DecCurPos()
{
    if (ms_curpos > 0)
    {
        const int pos = FindColorTagStartPosition(m_wText + ms_curpos - 1, ms_curpos);

        if (pos > 0)
            ms_curpos = min(ms_lastpos, max(0, ms_curpos - (pos + 1)));
        else
            --ms_curpos;
    }
}

/// The caret as the player SEES it, i.e. with the tags subtracted
/// (`GrpTextInstance.cpp` draws it there).
int CIME::GetCurPos()
{
    return GetTextTagOutputLen(m_wText, ms_curpos);
}

/// The inverse of `GetCurPos`: the player points at a place on screen and
/// the buffer has more characters than are visible.
void CIME::SetCurPos(int offset)
{
    if (offset < 0 || offset > ms_lastpos)
    {
        ms_curpos = ms_lastpos;
        return;
    }

    ms_curpos = min(ms_lastpos,
                    GetTextTagInternalPosFromRenderPos(m_wText, ms_lastpos, offset));
}

void CIME::DelCurPos()
{
    if (ms_curpos < ms_lastpos)
    {
        const int eraseCount =
            FindColorTagEndPosition(m_wText + ms_curpos, ms_lastpos - ms_curpos) + 1;
        wcscpy(m_wText + ms_curpos, m_wText + ms_curpos + eraseCount);
        ms_lastpos -= eraseCount;
        ms_curpos = min(ms_lastpos, ms_curpos);
    }
}

int CIME::GetCompLen() { return ms_compLen; }
int CIME::GetULBegin() { return ms_ulbegin; }
int CIME::GetULEnd()   { return ms_ulend; }

// ---------------------------------------------------------------------------
// Paste
// ---------------------------------------------------------------------------

void CIME::EnablePaste(bool bFlag) { m_bEnablePaste = bFlag; }

/// Inserts `str` (input code page) at the caret. NOTE a naming trap in
/// TMP4: its version declares a LOCAL `wchar_t m_wText[IMESTR_MAXLEN]` that
/// SHADOWS the static field of the same name - it works, because it is only
/// a scratch buffer, but reads like a bug; the buffer is named differently
/// here.
void CIME::PasteString(const char* str)
{
    const char* begin = str;
    const char* end = str + strlen(str);

    wchar_t awText[IMESTR_MAXLEN];
    const int wstrLen = MultiByteToWideChar(ImeCodePage(ms_uInputCodePage), 0, begin,
                                            static_cast<int>(end - begin),
                                            awText, IMESTR_MAXLEN);
    InsertString(awText, wstrLen);

    if (ms_pEvent)
        ms_pEvent->OnUpdate();
}

/// Inserts UTF-8 text at the caret - the road of `M2W_ImeCompositionEnd`
/// and `M2W_ImePaste` (the browser gives UTF-8, not the input page).
void CIME::PasteUtf8(const char* szUtf8)
{
    if (!szUtf8 || !szUtf8[0])
        return;

    wchar_t awText[IMESTR_MAXLEN];
    const int wstrLen = MultiByteToWideChar(CP_UTF8, 0, szUtf8,
                                            static_cast<int>(strlen(szUtf8)),
                                            awText, IMESTR_MAXLEN);
    if (wstrLen <= 0)
        return;

    InsertString(awText, wstrLen);

    if (ms_pEvent)
        ms_pEvent->OnUpdate();
}

/// DELIBERATELY EMPTY - see "Paste" in the header. Not a gap to fill: an
/// on-demand clipboard read is asynchronous in the browser and needs its
/// own consent, so a literal translation would be not only hard but worse.
void CIME::PasteTextFromClipBoard()
{
}

// ---------------------------------------------------------------------------
// Enabling, capturing, input mode
// ---------------------------------------------------------------------------

void CIME::UseDefaultIME()        { m_bUseDefaultIME = true; }
bool CIME::IsIMEEnabled()         { return ms_bImeEnabled; }
void CIME::DisableIME()           { EnableIME(false); }
void CIME::EnableCaptureInput()   { ms_bCaptureInput = true; }
void CIME::DisableCaptureInput()  { ms_bCaptureInput = false; }
bool CIME::IsCaptureEnabled()     { return ms_bCaptureInput; }

/// The browser composes when a text element has focus; `m2w_ime_enable`
/// says whether the hidden one should. Before `Initialize` nothing happens
/// (TMP4 has the same guard: `ime.EnableIME` from Python may come first).
void CIME::EnableIME(bool bEnable)
{
    if (!ms_bInitialized || !ms_hWnd)
        return;

    if (ms_bDisableIMECompletely)
        bEnable = false;

    m2w_ime_enable(bEnable ? 1 : 0);
    ms_bImeEnabled = bEnable;

    if (bEnable)
        CheckToggleState();
}

/// TMP4 asked IMM32 whether the current keyboard layout IS an input method
/// and whether it is on - to draw the mode indicator. The browser takes no
/// such question: that a composition is running is only known from
/// `compositionstart`. So the indicator stays empty - "don't know" said
/// plainly, not a guessed "off".
void CIME::CheckToggleState()
{
    ms_wszCurrentIndicator = NULL;
}

/// On Windows this switched IMM32 between native and alphanumeric mode. In
/// the browser the input-method mode belongs to the player's operating
/// system and the program has no way to change it - `document` has no call
/// for that.
void CIME::SetInputMode(DWORD /*dwMode*/)
{
}

DWORD CIME::GetInputMode()
{
    return 0;
}

// ---------------------------------------------------------------------------
// Candidate list and reading hint - "there is none" (see "Design")
// ---------------------------------------------------------------------------

int CIME::GetCandidateCount()      { return 0; }
int CIME::GetCandidatePageCount()  { return 0; }
int CIME::GetCandidateSelection()  { return 0; }

int CIME::GetCandidate(DWORD /*index*/, std::string& /*rstrText*/)
{
    return 0;
}

int CIME::GetReading(std::string& /*rstrText*/)
{
    return 0;
}

int CIME::GetReadingError()
{
    return ms_iReadingError;
}

void CIME::CloseCandidateList()
{
    ms_bCandidateList = false;
    ms_dwCandidateCount = 0;
    memset(&ms_wszCandidate, 0, sizeof(ms_wszCandidate));

    if (ms_pEvent)
        ms_pEvent->OnCloseCandidateList();
}

void CIME::CloseReadingInformation()
{
    ms_bReadingInformation = false;

    if (ms_pEvent)
        ms_pEvent->OnCloseReadingWnd();
}

/// A keyboard-layout change never reaches the page: the browser sends no
/// event for it and does not expose the layout - `KeyboardEvent` already
/// carries the finished character.
void CIME::ChangeInputLanguage()
{
    if (ms_pEvent)
        ms_pEvent->OnChangeCodePage();
}

void CIME::ChangeInputLanguageWorker()
{
    ChangeInputLanguage();
}

/// TMP4 told IMM32 to drop the composed string (`NI_COMPOSITIONSTR`,
/// `CPS_CANCEL`) and close the candidate list. The browser composes now, so
/// what belongs to US remains: resetting the state.
void CIME::FinalizeString(bool /*bSend*/)
{
    ms_compLen = 0;
    ms_ulbegin = 0;
    ms_ulend = 0;

    CloseCandidateList();
}

// ---------------------------------------------------------------------------
// Message handlers (dispatched by `PythonApplicationProcedure`; only
// `WM_CHAR` is ever posted in the port - see "Design")
// ---------------------------------------------------------------------------

LRESULT CIME::WMInputLanguage(HWND, UINT, WPARAM, LPARAM)
{
    ChangeInputLanguage();
    return 0;
}

LRESULT CIME::WMStartComposition(HWND, UINT, WPARAM, LPARAM)
{
    return 1L;
}

/// On Windows `lParam` carried only FLAGS (`GCS_COMPSTR`, `GCS_RESULTSTR`)
/// and the content had to be asked of IMM32. The content now comes
/// directly through `M2W_ImeCompositionUpdate`, so this handler has nothing
/// left to do but refresh the view.
LRESULT CIME::WMComposition(HWND, UINT, WPARAM, LPARAM)
{
    if (ms_pEvent)
        ms_pEvent->OnUpdate();

    return 0;
}

LRESULT CIME::WMEndComposition(HWND, UINT, WPARAM, LPARAM)
{
    ms_compLen = 0;
    ms_ulbegin = 0;
    ms_ulend = 0;

    if (ms_pEvent)
        ms_pEvent->OnUpdate();

    return 0L;
}

/// `WM_IME_NOTIFY` announced the candidate window opening, closing and
/// changing selection. The browser draws those windows now and tells us
/// nothing about them.
LRESULT CIME::WMNotify(HWND, UINT, WPARAM, LPARAM)
{
    return 0;
}

/// Backspace, or one character: a `wParam` above 0xFF is the Unicode
/// character the browser gave (see "Design"), one up to 0xFF is a byte in
/// the input code page as in TMP4. A vertical bar starts a text tag, so one
/// typed by the player is DOUBLED or it would vanish on display - the same
/// rule as in `TextTag.cpp`.
LRESULT CIME::WMChar(HWND, UINT, WPARAM wParam, LPARAM lParam)
{
    const unsigned char c = static_cast<unsigned char>(wParam & 0xff);

    switch (c)
    {
        case 8:   // backspace
            if (ms_bCaptureInput == false)
                return 0;

            if (ms_curpos > 0)
            {
                DecCurPos();
                DelCurPos();
            }

            if (ms_pEvent)
                ms_pEvent->OnUpdate();

            return 0;

        default:
            if (ms_pEvent)
            {
                if (ms_pEvent->OnWM_CHAR(wParam, lParam))
                    break;
            }

            if (ms_bCaptureInput == false)
                return 0;

            {
                wchar_t w = 0;

                if (wParam > 0xFF)
                {
                    w = static_cast<wchar_t>(wParam);
                }
                else
                {
                    const char cByte = static_cast<char>(c);
                    if (MultiByteToWideChar(ImeCodePage(ms_uInputCodePage), 0, &cByte, 1, &w, 1) <= 0)
                        return 0;
                }

                OnChar(w);

                if (w == L'|')
                    OnChar(w);

                if (ms_pEvent)
                    ms_pEvent->OnUpdate();
            }
            break;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Entry points for the browser - the counterparts of the three `WM_IME_*`
// messages and of `paste`. `extern "C"` + KEEPALIVE so that a listener in
// runtime.js can reach them with `Module.ccall`; NO SUCH LISTENER EXISTS
// YET (git grep) - they wait for a behaviour commit.
// ---------------------------------------------------------------------------

extern "C" {

/// `compositionstart`: clears the composed string and refreshes the view.
EMSCRIPTEN_KEEPALIVE
void M2W_ImeCompositionStart()
{
    CIME::ms_compLen = 0;
    CIME::ms_ulbegin = 0;
    CIME::ms_ulend = 0;

    if (CIME::ms_pEvent)
        CIME::ms_pEvent->OnUpdate();
}

/// `compositionupdate`: `szUtf8` is the current form of the composed
/// string, `iUnderlineBegin`/`iUnderlineEnd` the range the input method
/// underlines (on Windows it came as `GCS_COMPATTR` attributes).
EMSCRIPTEN_KEEPALIVE
void M2W_ImeCompositionUpdate(const char* szUtf8, int iUnderlineBegin, int iUnderlineEnd)
{
    if (!s_pIME || !szUtf8)
        return;

    wchar_t* pwszTarget = s_pIME->m_wszComposition;
    const int len = MultiByteToWideChar(CP_UTF8, 0, szUtf8,
                                        static_cast<int>(strlen(szUtf8)),
                                        pwszTarget, CIME::IMESTR_MAXLEN - 1);

    CIME::ms_compLen = (len > 0) ? len : 0;
    pwszTarget[CIME::ms_compLen] = 0;

    CIME::ms_ulbegin = iUnderlineBegin;
    CIME::ms_ulend = iUnderlineEnd;

    if (CIME::ms_pEvent)
        CIME::ms_pEvent->OnUpdate();
}

/// `compositionend`: the finished text enters the buffer as if the player
/// had typed it character by character - which is the point, because only
/// now is it text.
EMSCRIPTEN_KEEPALIVE
void M2W_ImeCompositionEnd(const char* szUtf8)
{
    if (!s_pIME)
        return;

    CIME::ms_compLen = 0;
    CIME::ms_ulbegin = 0;
    CIME::ms_ulend = 0;
    s_pIME->m_wszComposition[0] = 0;

    if (szUtf8 && szUtf8[0] && CIME::ms_bCaptureInput)
        s_pIME->PasteUtf8(szUtf8);
    else if (CIME::ms_pEvent)
        CIME::ms_pEvent->OnUpdate();
}

/// The `paste` event. Apart from composition because it is a different
/// road entirely - see `PasteTextFromClipBoard`.
EMSCRIPTEN_KEEPALIVE
void M2W_ImePaste(const char* szUtf8)
{
    if (!s_pIME || !szUtf8 || !CIME::ms_bCaptureInput)
        return;

    s_pIME->PasteUtf8(szUtf8);
}

}  // extern "C"
