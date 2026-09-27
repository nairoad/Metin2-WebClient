// SPDX-License-Identifier: GPL-2.0-or-later
// granny.h - choosing the platform branch for the Granny 3D SDK.
//
// WHAT BREAKS WITHOUT THIS FILE: `extern/include/granny.h` starts with a
// ladder of platform detection - `_XENON`, `_GAMECUBE`, `NN_PLATFORM_CTR`,
// `__CELLOS_LV2__`, `_PSX2`, `_MSC_VER`... Emscripten is not on that list
// (the SDK is older), so **no branch fires** and the macros `GRANNY_DYNIMP`,
// `GRANNY_CALLBACK`, `granny_uint64x` stay undefined. The result: 126
// "unknown type name GRANNY_DYNIMP" errors in every `GameLib` file.
//
// THIS IS NOT A BUG of the SDK nor ours - it is a header from 2005 that
// could not know a 2015 target.
//
// CHOOSING A BRANCH, not a workaround
// ---------------------------------------------------------------------------
// `NN_PLATFORM_CTR` describes a profile that **matches wasm32 point for
// point**:
//
//   | property             | CTR branch   | wasm32       |
//   |----------------------|--------------|--------------|
//   | calling convention   | none         | none         |
//   | pointer              | 32-bit       | 32-bit       |
//   | byte order           | little       | little       |
//   | 64-bit type          | `long long`  | `long long`  |
//
// So I substitute exactly that one, instead of adding my own macro
// definitions - fewer things to get wrong, and it is visible what we stand
// on.
//
// NOTE: this settles PARSING, not linking. Granny is a commercial library
// and its `.lib` is not in the repository - the first `GrannyXxx` call will
// stop the linker and will be a separate decision. The `.gr2` format is
// still an open question in the project; this file does NOT settle it. It
// only gives that the header stops blocking the rest of the tree.
// (Note added when translating: that decision has since been
// made - the port implements the Granny API itself on its own `.gr2`
// reader: granny_web.cpp, granny_pose.cpp, granny_control.cpp, gr2_*.cpp.)

#pragma once

#if !defined(NN_PLATFORM_CTR)
#define NN_PLATFORM_CTR
#endif

#include_next <granny.h>
