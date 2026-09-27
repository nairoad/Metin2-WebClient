// SPDX-License-Identifier: GPL-2.0-or-later
// rpcndr.h - a REDIRECT for the TMP4 port (COM headers).
#pragma once
#include "win32_compat.h"

// ---------------------------------------------------------------------------
// `boolean` and the `__RPCNDR_H__` guard
// ---------------------------------------------------------------------------
// FINDING: the copy of `libjpeg-9a/jconfig.h` in the TMP4
// repository is DAMAGED. Someone ran it through a "boolean" -> "unsigned
// char" replacement - comment included:
//
//     /* Define "unsigned char" as unsigned char, not enum, per Windows custom */
//     #ifndef __RPCNDR_H__
//     typedef unsigned char unsigned char;      <- this will not compile
//     #endif
//
// The guard itself shows what the libjpeg authors meant: **skip this typedef
// if `rpcndr.h` already did it**. We do exactly that - we provide `boolean`
// ourselves and set the guard. The damaged line is skipped, and `boolean`
// exists, because `jpeglib.h` uses it.
//
// This is not a workaround for the damage, but **using the back door the
// libjpeg authors left for exactly this case**.
#ifndef __RPCNDR_H__
#define __RPCNDR_H__
#endif

#ifndef _BOOLEAN_DEFINED
#define _BOOLEAN_DEFINED
typedef unsigned char boolean;
#endif
