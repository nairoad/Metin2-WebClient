// SPDX-License-Identifier: GPL-2.0-or-later
// locale_web.cpp - language and code page: writes `/locale.cfg`
// from the page address before `WinMain` reads it, and registers the
// packaged Tahoma font in the browser.

// Design:
// The game reads its language and code page from `/locale.cfg`. The
// packaged file always said `1252 en` - despite `?deflang=pl` in the
// address: game code page 1252, Polish chat impossible. So the file is written here, in C++ at the entry of `main`,
// from: `?lang=`, then the language remembered in
// `localStorage('m2/lang')`, then `?deflang=` (the site passes the server
// language), then the browser language, finally `en`; the language -> code
// page table is m2w.locales (runtime.js).

#include <stdio.h>
#include <string.h>

#include <emscripten/emscripten.h>

namespace
{

/// Registers `/tahoma.ttf` as the browser font "Tahoma" (m2w.loadFont).
EM_JS(void, m2w_load_font, (void), { m2w.loadFont(); });

/// Language into `pszBuffer`, returns its code page (m2w.browserLanguage).
EM_JS(int, m2w_browser_language, (char* pszBuffer, int iSize), {
    var r = m2w.browserLanguage();
    stringToUTF8(r.lang, pszBuffer, iSize);
    return r.codePage;
});

}  // namespace

/// Writes `/locale.cfg` from the page address. Called from `main`, before
/// `WinMain`.
void M2W_SetLocale()
{
    m2w_load_font();

    char szLang[16] = { 0 };
    const int iCodePage = m2w_browser_language(szLang, sizeof(szLang));

    // `LocaleService_LoadConfig` reads THREE fields, `%d %d %s`: a number
    // (the report port), the code page, the name. The client package has
    // ready `locale_<lang>.cfg` files with the right number; they are used
    // when present, otherwise the English number (10002) with the code page
    // from the m2w.locales table.
    char szLine[64] = { 0 };
    char szName[32];
    snprintf(szName, sizeof(szName), "locale_%s.cfg", szLang);
    FILE* fReady = fopen(szName, "rb");
    if (fReady)
    {
        if (!fgets(szLine, sizeof(szLine), fReady))
            szLine[0] = 0;
        fclose(fReady);
        char* p = szLine + strlen(szLine);
        while (p > szLine && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '))
            *--p = 0;
    }
    if (!szLine[0])
        snprintf(szLine, sizeof(szLine), "10002 %d %s", iCodePage, szLang);

    FILE* f = fopen("locale.cfg", "wb");
    if (!f)
    {
        printf("m2w locale: cannot write locale.cfg - keeping the packaged one\n");
        return;
    }
    fprintf(f, "%s\n", szLine);
    fclose(f);
    printf("m2w locale: locale.cfg = '%s'\n", szLine);
}
