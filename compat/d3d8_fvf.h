// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_fvf.h - the vertex layout from an FVF code or a `D3DVSD_*`
// declaration: offset, component count and type of every vertex input,
// computed the way Direct3D 8 laid vertices out. Pure - testable.

// Design:
// WHAT FVF IS. Direct3D 8 described the vertex format with ONE NUMBER - a
// set of bits `D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1`. The driver worked
// out from it what lies where in memory. OpenGL wants the same, only spelt
// out: for every shader input the offset, the component count and the
// type. This file does that computation. It is PURE - a number in, a struct
// of offsets out - so it can be checked by a test (d3d8_fvf_test.cpp).
//
// CATEGORY A - IMPOSED FROM OUTSIDE. The order of the components in vertex
// memory is NOT our choice. It is set by the model and map files in the
// game packs, which are immutable. Direct3D always laid them out the same:
//     position -> [blend weights] -> normal -> point size ->
//     diffuse colour -> specular colour -> texture coordinates
// A mistake in that order gives an image that draws and makes no sense -
// texture coordinates read as a position, a model smeared across the
// screen.
//
// THE TRAP: THE VERTEX COLOUR IS BGRA, NOT RGBA. `D3DFVF_DIFFUSE` is one
// `D3DCOLOR` word, `0xAARRGGBB`; in little-endian memory the bytes lie
// B, G, R, A - the same trap as with `A8R8G8B8` textures. Memory
// is NOT converted here: there are orders of magnitude more vertices than
// textures and walking them in wasm would be waste. The attribute goes to
// OpenGL AS IS and the shader fixes the order with one word: `aColor.bgra`.
// It costs nothing. So `M2W_BuildVertexProgram` MUST read the colour
// through `.bgra` - there is a test case for it, because without it the
// characters would have red and blue swapped.

#pragma once

#include "win32_compat.h"
#include "d3d8.h"

/// The most texture coordinate sets an FVF can describe.
enum { M2W_MAX_TEXCOORD_SETS = 8 };

/// Where everything lies in one vertex. Offset -1 means "this component is
/// absent" - not "lies at the start"; zero would be ambiguous here.
struct TVertexLayout
{
    /// Size of the whole vertex in bytes - the stride.
    UINT uStride;

    int iPosition;             ///< offset of the position
    int iPositionComponents;   ///< 3 for `XYZ`, 4 for `XYZRHW`

    /// Whether the vertices are ALREADY transformed (`D3DFVF_XYZRHW`) - then
    /// they go straight to the screen, in pixels, and are not multiplied by
    /// the matrices.
    bool bTransformed;

    int iNormal;               ///< offset of the normal or -1
    int iPointSize;            ///< offset of `PSIZE` or -1
    int iDiffuse;              ///< offset of the diffuse colour or -1
    int iSpecular;             ///< offset of the specular colour or -1

    int aiTexCoords[M2W_MAX_TEXCOORD_SETS];          ///< offsets
    int aiTexCoordComponents[M2W_MAX_TEXCOORD_SETS]; ///< 1..4 per set
    int iTexCoordSets;         ///< how many sets the vertex carries

    /// Whether the FVF code was understood in full. `false` means there is
    /// something in the code we do not handle (bone blend weights, for
    /// instance) - and the caller has the right to know instead of getting
    /// a layout that looks right and is shifted.
    bool bKnown;
};

/// Decomposes an FVF code into a vertex layout.
TVertexLayout M2W_VertexLayout(DWORD dwFVF);

/// The same from a `D3DVSD_*` DECLARATION instead of an FVF code.
///
/// WHY: Direct3D 8 had two ways to describe a vertex - one number (FVF) or
/// a run of `D3DVSD_*` words, which could also split the data over several
/// streams. `CGraphicDevice` uses the latter for CHARACTER MODELS
/// (`CreatePNTStreamVertexShader` and three sisters), and `SetVertexShader`
/// then gets only the handle. Earlier our layer THREW those
/// declarations AWAY: `CreateVertexShader` returned a handle with the top
/// bit set, `SetVertexShader` saw that bit and left the remembered FVF code
/// alone - so a character model drew with whatever layout was left over
/// from something else. Symptom: characters smeared across the screen,
/// without one OpenGL error.
///
/// WHAT IT UNDERSTANDS. Stream ZERO. A `D3DVSD_STREAM(n)` word with `n != 0`
/// switches to a stream we never supply - its data is skipped and `bKnown`
/// becomes `false`, so the caller knows the layout is incomplete. Silent
/// consent would be worse: the layout would look right with wrong offsets.
///
/// `c_pDeclaration` may be `NULL` - then `iPosition` comes out -1.
TVertexLayout M2W_LayoutFromDeclaration(const DWORD* c_pDeclaration);
