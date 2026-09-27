// SPDX-License-Identifier: GPL-2.0-or-later
// movie_web.cpp - the intro movie through a `<video>` element, replacing
// `UserInterface/MovieMan.cpp` (a DirectShow filter graph on a DirectDraw 7
// surface, hand-picked codecs MP43/MP4V/MP3, frames to screen by `BitBlt`).

// Design:
// A difference that cannot be hidden: the original BLOCKED.
// `CMovieMan::PlayMovie` spun its own message loop and returned only when
// the movie ended or the player skipped it; the rest of the client stood
// still. In the browser the main thread MUST NOT block - the picture, the
// events and the very playback would stop with it - so `PlayLogo` starts an
// overlay and RETURNS AT ONCE. Code that called `PlayLogo` expecting the
// movie to be OVER on return goes on immediately. For the logo that does
// not matter: the overlay lies over the canvas and removes itself. Should a
// movie ever need something to follow it, the right road is a callback on
// `ended`, not waiting.
//
// What the browser may not play: TMP4 chose codecs by COM class id because
// the files are in formats of their day. The browser plays what it can -
// today MP4/H.264, WebM, Ogg. An old container (e.g. WMV) shows NOTHING
// and the overlay ends at once, saying so in the console. The right answer
// is transcoding when the package is built, not a workaround here.

#include <string>

#include "win32_compat.h"

#include <emscripten.h>

#include "eterBase/Debug.h"
#include "EterPack/EterPackManager.h"
#include "eterBase/MappedFile.h"
#include "UserInterface/MovieMan.h"

/// Puts a `<video>` overlay over the page and plays the given bytes.
EM_JS(void, m2w_movie_play, (const char* c_pData, int iBytes, const char* c_szName), {
    m2w.moviePlay(HEAPU8.slice(c_pData, c_pData + iBytes), UTF8ToString(c_szName));
});

void CMovieMan::PlayLogo(const char* pcszName)
{
    if (!pcszName)
        return;

    CMappedFile kFile;
    LPCVOID pData = NULL;
    if (!CEterPackManager::Instance().Get(kFile, pcszName, &pData))
    {
        TraceError("CMovieMan::PlayLogo - no file %s", pcszName);
        return;
    }

    m2w_movie_play(static_cast<const char*>(pData),
                   static_cast<int>(kFile.Size()),
                   pcszName);

    // Returns AT ONCE - see the design note.
}
