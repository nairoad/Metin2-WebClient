// SPDX-License-Identifier: GPL-2.0-or-later
// d3dtypes.h - a redirect to our layer (like d3d8types.h).
//
// A header from the DirectDraw days, pulled in by `EterImageLib`. The
// original in `extern/include` refers to `LPDDSURFACEDESC`, i.e. to the
// whole DirectDraw layer - while all that is needed from it are the
// pixel-format constants, which we have in `d3d8.h`.

#pragma once

#include "d3d8.h"
