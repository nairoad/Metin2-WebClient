// SPDX-License-Identifier: GPL-2.0-or-later
// imagehlp.h - a REDIRECT for the TMP4 port.
//
// TMP4 code includes this header directly. Instead of rewriting hundreds
// of #include directives, I provide a file with this name that pulls in
// our compatibility layer. That keeps the TMP4 sources UNTOUCHED - and the
// less we touch them, the easier it will be to take in fixes from upstream.
#pragma once
#include "win32_compat.h"
