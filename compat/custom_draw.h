// SPDX-License-Identifier: GPL-2.0-or-later
// custom_draw.h - DRAWING PAST THE FIXED-FUNCTION PIPELINE ("road B"): one
// shader program, own buffers and own state for the spatial content
// (models), while the interface stays on the Direct3D 8 emulation.

// Design:
// WHY A SEPARATE ROAD (the user's decision, "road B"). The `gl_device.cpp`
// layer pretends to be Direct3D 8 with its fixed-function pipeline: eight
// texture stages, `D3DTOP_*` operations, lighting, fog. For the INTERFACE
// that works - login, inventory, minimap and chat draw correctly. For the
// spatial content it works worse: the terrain was black and
// the characters invisible, and every time the search for the
// cause ended in the same place - in the composition of the pipeline,
// where nothing shouts, a black pixel just comes out.
//
// So the split here is: the
// interface STAYS on the emulation, because it works. The spatial content
// - models, later the terrain - gets its own road: own shader program, own
// buffers, own state.
//
// IN PRACTICE this file pretends nothing. It gets vertices, indices, a
// texture and matrices - and draws. No texture stages and no Direct3D
// states here; when something is wrong it shows on screen or in a report
// instead of vanishing between eight stages.

#pragma once

#include <stddef.h>
#include <stdint.h>

/// The vertex the models are drawn with. EXACTLY the `PNT332` layout TMP4
/// uses for characters - position, normal, texture coordinates.
struct TVertexPNT
{
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

/// Draws one mesh with the custom shader program.
///
/// @param c_pVertices   an array of `uVertices` vertices
/// @param c_puIndices   triangle indices (three per triangle)
/// @param uTexture      OpenGL texture name; 0 means "no texture"
/// @param c_afWorld     4x4 world matrix, row-major (as in Direct3D)
/// @param c_afView      4x4 view matrix
/// @param c_afProjection 4x4 projection matrix
void M2W_DrawMesh(const TVertexPNT* c_pVertices,
                  uint32_t uVertices,
                  const uint16_t* c_puIndices, uint32_t uIndices,
                  uint32_t uTexture,
                  const float* c_afWorld, const float* c_afView,
                  const float* c_afProjection);

/// `iCull` values of `M2W_DrawMeshFromBuffers` - the Direct3D cull mode.
enum
{
    M2W_CULL_NONE = 0,   ///< D3DCULL_NONE: nothing culled
    M2W_CULL_CW = 1,     ///< D3DCULL_CW: clockwise faces culled (GL_BACK)
    M2W_CULL_CCW = 2     ///< D3DCULL_CCW: counter-clockwise faces culled (GL_FRONT)
};
/// `iBlend` = 0 of `M2W_DrawMeshFromBuffers`: opaque, depth written.
const int M2W_BLEND_OFF = 0;
/// `iAlphaFunc` = 0 of `M2W_DrawMeshFromBuffers`: no alpha test.
const int M2W_ALPHA_TEST_OFF = 0;
/// `fAlphaRef` when the alpha test is off - never compared with anything.
const float M2W_ALPHA_REF_UNUSED = -1.0f;

/// Draws a mesh from BUFFERS already on the card.
///
/// Direct3D 8 buffers in this port ARE OpenGL buffers (`CGlBuffer` keeps the
/// GL name beside the copy in memory). So when the game has already
/// prepared the vertices and indices there is nothing to copy - bind and
/// draw.
///
/// @param uStride        distance between vertices; MUST be the PNT layout
/// @param uBaseVertex    `BaseVertexIndex` from `SetIndices` (in vertices)
/// @param uIndexType     `GL_UNSIGNED_SHORT` or `GL_UNSIGNED_INT`
/// @param uFirstIndex    number of the first index (in indices, not bytes)
/// @param iBlend         non-zero when drawing with alpha blending (then no
///                       depth writes); M2W_BLEND_OFF = opaque
/// @param iCull          M2W_CULL_NONE / M2W_CULL_CW / M2W_CULL_CCW
/// @param iAlphaFunc     D3DCMP_* (1..8), M2W_ALPHA_TEST_OFF = no alpha test
/// @param fAlphaRef      the alpha test reference, 0..1; ignored when the
///                       test is off (M2W_ALPHA_REF_UNUSED)
void M2W_DrawMeshFromBuffers(uint32_t uVertexBuffer, uint32_t uStride,
                             uint32_t uBaseVertex,
                             uint32_t uIndexBuffer, uint32_t uIndexType,
                             uint32_t uFirstIndex, uint32_t uIndices,
                             uint32_t uTexture,
                             const float* c_afWorld, const float* c_afView,
                             const float* c_afProjection,
                             int iBlend, int iCull,
                             int iAlphaFunc, float fAlphaRef);

/// ENTRY FOR THE GAME: draws what the game has just asked to draw - but by
/// our road, not the fixed-function pipeline.
///
/// Takes EVERYTHING from the current device state (stream, indices, the
/// texture of stage zero, world, view and projection matrices), so at the
/// call site it is enough to replace `DrawIndexedPrimitive` with this.
/// Returns `false` when the state is not fit for drawing our way - then the
/// caller draws the old way.
///
/// Defined in `gl_device.cpp`, because the device state lives there.
bool M2W_DrawModelCustom(uint32_t uFirstIndex, uint32_t uTriangles);

/// Half a physical pixel of the device's current viewport in clip units -
/// the pixel-centre fix the D3D8 road puts into its matrix, so
/// that meshes drawn here stay on the same pixels as the terrain. The y
/// component is mirrored on a flipped render target, because this road's
/// projection arrives already flipped. Zeros without a device. Defined in
/// `gl_device.cpp`.
void M2W_ClipHalfPixel(float* pfX, float* pfY);

/// Fog of road B: D3DFOG_* mode (0 off), start, end, density,
/// RGB colour 0..1.
void M2W_SetCustomFog(int iMode, float fStart, float fEnd, float fDensity,
                      const float* c_afColor);

/// Tells the custom road that the OpenGL state changed outside it. The
/// compatibility layer calls this before its own drawing - see the body.
void M2W_ForgetCustomState();

/// PROBE: loads one `.gr2` model and draws N animated instances of it in rows
/// of ten, with the camera orbiting them (it also runs the texture probe,
/// `M2W_TextureProbe`, which has its own address switch).
///
/// Why: the whole road - reader, pose, deformation, drawing - can then be
/// checked WITHOUT A SERVER, on the login screen. When the character
/// appears the chain is whole and only the hook into the place where the
/// game draws characters remains. When it does not, the fault is in the
/// chain, not in what cannot be seen.
///
/// Switched on with `?modelprobe=N` in the page
/// address, N = the number of characters - pose and skin are computed for
/// them EVERY FRAME, as in the game. `?probehash=F` prints a hash of the
/// canvas after the F-th probe frame (the gate `tools/gates/check_probe.py`).
void M2W_ModelProbe();

/// TEXTURE PROBE: loads a whole directory of images by the road the game
/// takes and measures how long it takes. Answers whether the multi-second
/// pauses come from uploading textures to the card.
///
/// Switched on with `?textureprobe=<directory>` or `?textureprobe=1`
void M2W_TextureProbe();
