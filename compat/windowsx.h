// SPDX-License-Identifier: GPL-2.0-or-later
// windowsx.h - helper macros for handling window messages.
//
// `MSWindow.cpp` pulls this header in for macros like `GET_X_LPARAM`. In
// the browser there is no message loop - events come from the canvas - so
// the file is empty, and the macros themselves I will add when the
// compiler asks for them (and it is known which are really used).

#pragma once

#include "win32_compat.h"
