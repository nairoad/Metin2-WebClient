// eterLib/StdAfx.h - OUR header, substituted in place of the TMP4 one.
//
// The first file of the layer replacing `EterLib`. It differs from the original
// **only in what the measurement told us to change**:
//
//  - `<d3d8.h>` and `<d3dx8.h>` stay, because they now land in `compat`
//    (constants and math) - `GameLib` uses 116 + 30 names from them;
//  - `<dinput.h>` stays, because ours is EMPTY by measurement: `GameLib` does not use
//    DirectInput even once;
//  - the `#pragma comment(lib, ...)` lines go - they are instructions for the
//    Microsoft linker, which mean nothing in emscripten;
//  - the `#pragma warning` lines go - they concern MSVC warnings.
//
// The rest of the include chain stays untouched, so that the difference between our
// layer and the original is as small as possible and easy to review.

#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <d3d8.h>
#include <d3dx8.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <mmsystem.h>
#include <process.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <direct.h>
#include <malloc.h>

#include "../eterBase/StdAfx.h"
#include "../eterBase/Debug.h"
#include "../eterLocale/CodePageId.h"

#include <winsock.h>
