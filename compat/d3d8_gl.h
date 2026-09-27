// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_gl.h - the Direct3D 8 device built on WebGL 2: its public entry points
// (create/destroy, capabilities, factory, measurement switches). The device
// itself is `CGlDevice` (gl_internal.h, gl_*.cpp).
//
// ===========================================================================
// WHAT THIS IS
// ===========================================================================
// The body of the `IDirect3DDevice8` interface that `d3d8.h` describes from
// measurement. The client calls these methods through
// `CStateManager`; here they turn into OpenGL ES 3 calls, i.e. WebGL 2.
//
// Three pure layers it stands on are already written and tested:
//
//   * `d3d8_fixedfunc.*` - composes GLSL from the fixed-function pipeline,
//   * `d3d8_states.*`     - translates constants and texture memory (183),
//   * `d3d8_fvf.*`       - decodes an FVF code into a vertex layout.
//
// The device uses them and adds what cannot be tested in any way without a
// graphics card: the OpenGL calls themselves.
//
// ===========================================================================
// WHY A DEVICE, AND NOT A REWRITE OF THE CLIENT
// ===========================================================================
// Because the client calls the device in one narrow place - `CStateManager`
// - and everything above it is game logic. Replacing the device leaves that
// logic untouched, so every bug in the image has one place to check.
// Rewriting the client would spread the same bug over a hundred files.

#pragma once

#include "win32_compat.h"
#include "d3d8.h"

/// Hardware capabilities. Both the device and the factory call it - see the
/// note at the definition (gl_device.cpp).
HRESULT M2W_GlCapabilities(D3DCAPS8* pCaps);

/// The `IDirect3D8` factory - answers questions about display modes.
/// `CGraphicBase::ms_lpd3d` has to get it, otherwise
/// `CPythonSystem::GetDisplaySettings` reads from a null pointer.
IDirect3D8* M2W_CreateGlFactory();

/// Creates the device bound to the page's canvas.
///
/// `c_szCanvas` is a selector like `"#canvas"`. Returns NULL when WebGL 2
/// is not available - and that is a true answer, not a crash: there are
/// browsers and machines without it.
IDirect3DDevice8* M2W_CreateGlDevice(const char* c_szCanvas,
                                     int iWidth, int iHeight);

/// Destroys the device and everything that belongs to it.
void M2W_DestroyGlDevice(IDirect3DDevice8* pDevice);

/// How many programs sit in the cache. For measurement and tests - the
/// measurement says there should be a few dozen keys, not
/// thousands. If this number grows without end, the key contains something
/// that should not tell programs apart.
int M2W_ProgramCacheSize(IDirect3DDevice8* pDevice);

/// Forces the FALLBACK road for DXT textures - decoding in wasm instead of
/// handing the blocks to the card.
///
/// It exists so that this road can be MEASURED where the browser has
/// `WEBGL_compressed_texture_s3tc` and would never reach for it by itself.
/// Without this switch the fallback would be code that runs only for people
/// for whom nobody checks it - exactly where untested code must not be.
///
/// Called with `false` it hands the decision back to the browser.
/// No caller in the repository (the test that used it was
/// removed) - kept as a measurement hook.
void M2W_ForceDxtDecode(bool bForce);
