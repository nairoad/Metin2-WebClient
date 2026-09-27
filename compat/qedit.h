// SPDX-License-Identifier: GPL-2.0-or-later
// qedit.h - DirectShow Editing Services (grabbing frames from a film).
//
// The original in `extern/include` is 10 thousand lines of COM declarations
// that need the whole of `oaidl.h` and a newer `rpcndr.h`. In the browser
// films are played by the <video> tag, and a frame is taken from the canvas
// - none of this is needed.

#pragma once

#include "win32_compat.h"

// Grabbing frames from the graph (`ISampleGrabber`) - opaque, like the rest
// of DirectShow. `MovieMan` takes a frame with it to hand it to a texture;
// in the browser `drawImage(video, ...)` on the canvas does that.
struct ISampleGrabber;
struct ISampleGrabberCB;
struct IMediaDet;
