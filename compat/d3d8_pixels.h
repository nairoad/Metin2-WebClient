// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_pixels.h - pixel conversion between Direct3D formats (A8R8G8B8,
// X8R8G8B8, R8G8B8, A4R4G4B4, X4R4G4B4, A1R5G5B5, X1R5G5B5, R5G6B5) with
// nearest or bilinear resampling: the arithmetic of
// `D3DXLoadSurfaceFromSurface`, pure buffers in and out, testable.

// Design:
// `eterLib/GrpImageTexture.cpp` loads a texture in one format and then
// COPIES IT INTO ANOTHER - always a smaller one: A8R8G8B8 -> A4R4G4B4,
// X8R8G8B8 -> A1R5G5B5, R8G8B8 -> A1R5G5B5 - with little texture memory
// (`IsLowTextureMemory`) or with image halving on
// (`GRAPHICS_CAPS_HALF_SIZE_IMAGE`). Direct3D did that in
// `D3DXLoadSurfaceFromSurface`, which scaled on the way. This file is its
// arithmetic, without one OpenGL call.
//
// Bit expansion is the one thing with a second bottom: packing eight bits
// into four is obviously `v >> 4`; unpacking would obviously be `v << 4`,
// and that is a visible ERROR - white `0xFF` packs to `0xF` and unpacks
// to `0xF0` = 240, not 255, the whole image dims and darkens further with
// every pass. Right is REPLICATION, `(v << 4) | v` = `0xFF`: the extreme
// stays extreme and a round trip is stable. The same for 5 and 6 bits.

#pragma once

#include "win32_compat.h"
#include "d3d8.h"

/// Unpacks one pixel to `0xAARRGGBB`. A format without alpha gets `0xFF`
/// alpha - as Direct3D did.
DWORD M2W_PixelToArgb(D3DFORMAT eFormat, const void* c_pvPixel);

/// Packs `0xAARRGGBB` into the target format.
void M2W_ArgbToPixel(D3DFORMAT eFormat, DWORD dwARGB, void* pvPixel);

/// Whether this format can be unpacked and packed here.
bool M2W_FormatConvertible(D3DFORMAT eFormat);

/// Bytes per pixel of the format. Zero for formats not handled here or
/// without a fixed size (DXT).
UINT M2W_BytesPerPixel(D3DFORMAT eFormat);

/// Copies a rectangle of pixels with format conversion and scaling.
///
/// `bLinear` chooses between the nearest source pixel and the average of
/// four - `D3DX_FILTER_NONE` and `D3DX_FILTER_LINEAR`, the only two the
/// client uses.
///
/// Returns `false` when either format is not handled - the true answer.
/// A silent no-op would leave the texture full of zeros, a black rectangle
/// instead of the image.
bool M2W_ConvertPixelRect(D3DFORMAT eSourceFormat, const void* c_pvSource,
                          int iSourcePitch, int iSourceWidth, int iSourceHeight,
                          D3DFORMAT eTargetFormat, void* pvTarget,
                          int iTargetPitch, int iTargetWidth, int iTargetHeight,
                          bool bLinear);
