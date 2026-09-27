// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8types.h - a redirect to our layer.
//
// The EterLib headers pull in `<d3d8types.h>` separately. Without this file
// they would hit Microsoft's original in `extern/include`, which would
// redefine structures `d3d8.h` already provides - and the compiler would
// rightly report it.

#pragma once

#include "d3d8.h"
