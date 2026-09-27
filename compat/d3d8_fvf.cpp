// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8_fvf.cpp - decomposition of an FVF code and of a `D3DVSD_*`
// declaration into a vertex layout. See d3d8_fvf.h.

// Design: the component order is imposed by the game files (category A)
// and goes exactly as Direct3D laid it out. Reasons at each case.

#include "d3d8_fvf.h"

namespace
{

/// The component count of one texture coordinate set.
///
/// FVF keeps it in the UPPER bits of the code, two bits per set, from bit
/// 16. And those two bits DO NOT mean the count directly - there is a
/// table: 0 is two components (the most common case, so it got zero), 1 is
/// three, 2 is four, 3 is one. Reading them as a number would give two
/// components where there are four, and the other way round - and that is
/// a shift of the WHOLE rest of the vertex.
int SetComponents(DWORD dwFVF, int iSet)
{
    const DWORD dwTwoBits = (dwFVF >> (16 + iSet * 2)) & 0x3u;
    switch (dwTwoBits)
    {
        case D3DFVF_TEXTUREFORMAT1: return 1;
        case D3DFVF_TEXTUREFORMAT2: return 2;
        case D3DFVF_TEXTUREFORMAT3: return 3;
        case D3DFVF_TEXTUREFORMAT4: return 4;
        default:                    return 2;
    }
}

}  // namespace

namespace
{

/// A layout with nothing in it: every offset -1, no texture sets, known.
TVertexLayout EmptyLayout()
{
    TVertexLayout u;
    u.uStride = 0;
    u.iPosition = -1;
    u.iPositionComponents = 0;
    // Only `D3DFVF_XYZRHW` makes a layout transformed; a declaration never
    // does (declarations describe vertices YET to be transformed).
    u.bTransformed = false;
    u.iNormal = -1;
    u.iPointSize = -1;
    u.iDiffuse = -1;
    u.iSpecular = -1;
    u.iTexCoordSets = 0;
    u.bKnown = true;

    for (int i = 0; i < M2W_MAX_TEXCOORD_SETS; ++i)
    {
        u.aiTexCoords[i] = -1;
        u.aiTexCoordComponents[i] = 0;
    }
    return u;
}

/// The position part of FVF code `dwFVF`: puts it at `riOffset` in `ru`
/// and moves `riOffset` past it (none, XYZ, XYZRHW, or XYZ with bone
/// weights - the last one clears `ru.bKnown`).
void PlaceFvfPosition(TVertexLayout& ru, DWORD dwFVF, int& riOffset)
{
    // Mind the values: `D3DFVF_XYZRHW` is 0x0004 and `D3DFVF_XYZB1` is
    // 0x0006, i.e. XYZ|XYZRHW. Testing with `&` alone in the wrong order
    // would confuse them - so `XYZRHW`, the special case, is asked first
    // and `XYZ` only after.
    const DWORD dwPosition = dwFVF & 0x000Eu;

    if (dwPosition == D3DFVF_XYZRHW)
    {
        // Four numbers: X, Y, Z AND W. The vertex is already transformed,
        // the coordinates are screen pixels.
        ru.iPosition = riOffset;
        ru.iPositionComponents = 4;
        ru.bTransformed = true;
        riOffset += 4 * static_cast<int>(sizeof(float));
    }
    else if (dwPosition == D3DFVF_XYZ)
    {
        ru.iPosition = riOffset;
        ru.iPositionComponents = 3;
        riOffset += 3 * static_cast<int>(sizeof(float));
    }
    else if (dwPosition != 0)
    {
        // `XYZB1`, `XYZB2`, `XYZB3` - position plus bone blend weights. The
        // client does not use them (Granny blends on the CPU), but should
        // it start, the layout would be shifted by the weights. Saying "I
        // do not know" outright, instead of counting the XYZ and keeping
        // quiet.
        ru.iPosition = riOffset;
        ru.iPositionComponents = 3;
        riOffset += 3 * static_cast<int>(sizeof(float));
        ru.bKnown = false;
    }
}

}  // namespace

TVertexLayout M2W_VertexLayout(DWORD dwFVF)
{
    TVertexLayout u = EmptyLayout();
    int iOffset = 0;

    PlaceFvfPosition(u, dwFVF, iOffset);

    // --- normal ------------------------------------------------------------
    if (dwFVF & D3DFVF_NORMAL)
    {
        u.iNormal = iOffset;
        iOffset += 3 * static_cast<int>(sizeof(float));
    }

    // --- point size --------------------------------------------------------
    if (dwFVF & D3DFVF_PSIZE)
    {
        u.iPointSize = iOffset;
        iOffset += static_cast<int>(sizeof(float));
    }

    // --- colours -----------------------------------------------------------
    // Both are `D3DCOLOR`, one word 0xAARRGGBB - in memory B, G, R, A. Not
    // converted: the shader reads through `.bgra`. See the header.
    if (dwFVF & D3DFVF_DIFFUSE)
    {
        u.iDiffuse = iOffset;
        iOffset += static_cast<int>(sizeof(DWORD));
    }
    if (dwFVF & D3DFVF_SPECULAR)
    {
        u.iSpecular = iOffset;
        iOffset += static_cast<int>(sizeof(DWORD));
    }

    // --- texture coordinates -----------------------------------------------
    const int iSets = static_cast<int>(
        (dwFVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);

    u.iTexCoordSets = (iSets > M2W_MAX_TEXCOORD_SETS)
                      ? M2W_MAX_TEXCOORD_SETS : iSets;

    if (iSets > M2W_MAX_TEXCOORD_SETS)
        u.bKnown = false;

    for (int i = 0; i < u.iTexCoordSets; ++i)
    {
        const int iComponents = SetComponents(dwFVF, i);
        u.aiTexCoords[i] = iOffset;
        u.aiTexCoordComponents[i] = iComponents;
        iOffset += iComponents * static_cast<int>(sizeof(float));
    }

    u.uStride = static_cast<UINT>(iOffset);
    return u;
}


// ---------------------------------------------------------------------------
// The layout from a `D3DVSD_*` declaration
// ---------------------------------------------------------------------------

namespace
{

/// How many bytes one value of the given data type takes and how many
/// components it has. Returns `false` for a type we do not know - and then
/// does NOT guess the size, because a guessed size shifts everything that
/// lies after it.
bool DataType(DWORD dwType, int* piBytes, int* piComponents)
{
    switch (dwType)
    {
        case D3DVSDT_FLOAT1:   *piBytes =  4; *piComponents = 1; return true;
        case D3DVSDT_FLOAT2:   *piBytes =  8; *piComponents = 2; return true;
        case D3DVSDT_FLOAT3:   *piBytes = 12; *piComponents = 3; return true;
        case D3DVSDT_FLOAT4:   *piBytes = 16; *piComponents = 4; return true;
        // `D3DCOLOR` and `UBYTE4` are one word each - four bytes, four
        // components. They differ only in order and normalisation; that is
        // the business of attribute binding, not of size.
        case D3DVSDT_D3DCOLOR: *piBytes =  4; *piComponents = 4; return true;
        case D3DVSDT_UBYTE4:   *piBytes =  4; *piComponents = 4; return true;
        case D3DVSDT_SHORT2:   *piBytes =  4; *piComponents = 2; return true;
        case D3DVSDT_SHORT4:   *piBytes =  8; *piComponents = 4; return true;
        default:               return false;
    }
}

/// Puts the stream-zero register `dwRegister` (with `iComponents`
/// components) at `iOffset` in `ru`; texture sets raise `riHighestSet`.
/// Registers the layout cannot express clear `ru.bKnown` - the caller still
/// moves the offset on, because they take room in the vertex.
void PlaceRegister(TVertexLayout& ru, DWORD dwRegister, int iOffset,
                   int iComponents, int& riHighestSet)
{
    switch (dwRegister)
    {
        case D3DVSDE_POSITION:
            ru.iPosition = iOffset;
            ru.iPositionComponents = iComponents;
            break;
        case D3DVSDE_NORMAL:
            ru.iNormal = iOffset;
            break;
        case D3DVSDE_PSIZE:
            ru.iPointSize = iOffset;
            break;
        case D3DVSDE_DIFFUSE:
            ru.iDiffuse = iOffset;
            break;
        case D3DVSDE_SPECULAR:
            ru.iSpecular = iOffset;
            break;
        case D3DVSDE_BLENDWEIGHT:
        case D3DVSDE_BLENDINDICES:
            // Bone blending on the card. It takes room in the vertex, so
            // the offset MUST move on - but the component itself cannot
            // be used.
            ru.bKnown = false;
            break;
        default:
            if (dwRegister >= D3DVSDE_TEXCOORD0 &&
                dwRegister <= static_cast<DWORD>(D3DVSDE_TEXCOORD0 +
                              M2W_MAX_TEXCOORD_SETS - 1))
            {
                const int iSet =
                    static_cast<int>(dwRegister) - D3DVSDE_TEXCOORD0;
                ru.aiTexCoords[iSet] = iOffset;
                ru.aiTexCoordComponents[iSet] = iComponents;
                if (iSet > riHighestSet)
                    riHighestSet = iSet;
            }
            else
            {
                // `D3DVSDE_POSITION2`, `NORMAL2` and anything further.
                ru.bKnown = false;
            }
            break;
    }
}

}  // namespace

/// A guard against an unterminated declaration array. The client's
/// declarations are a few words each; 256 is margin beyond measure, and an
/// endless loop would read the memory beside the array - a crash, not a bad
/// image. (d3d8_fvf_test checks that a declaration without an end stops here.)
const int c_iMaxDeclarationWords = 256;

TVertexLayout M2W_LayoutFromDeclaration(const DWORD* c_pDeclaration)
{
    TVertexLayout u = EmptyLayout();

    if (!c_pDeclaration)
    {
        u.bKnown = false;
        return u;
    }

    int iStream = 0;
    int iOffset = 0;
    int iHighestSet = -1;

    for (int i = 0; i < c_iMaxDeclarationWords; ++i)
    {
        const DWORD dwWord = c_pDeclaration[i];
        const DWORD dwKind = (dwWord >> D3DVSD_TOKENTYPESHIFT) & 0x07u;

        if (dwKind == D3DVSD_TOKEN_END)
            break;

        if (dwKind == D3DVSD_TOKEN_STREAM)
        {
            // A new stream counts its offsets FROM ZERO. We supply only
            // stream zero, so further words will be skipped - but the
            // offset is reset anyway, so that a return to zero (which
            // Direct3D did not forbid) comes out right.
            iStream = static_cast<int>(dwWord & 0xFFFFu);
            if (iStream == 0)
                iOffset = 0;
            continue;
        }

        if (dwKind != D3DVSD_TOKEN_STREAMDATA)
        {
            // Words we do not handle: shader constants, tessellator,
            // extensions. No pretending to understand them.
            u.bKnown = false;
            continue;
        }

        const DWORD dwRegister = dwWord & 0xFFFFu;
        const DWORD dwType = (dwWord >> 16) & 0x0Fu;

        int iBytes = 0;
        int iComponents = 0;
        if (!DataType(dwType, &iBytes, &iComponents))
        {
            // An unknown data type ends the trust in all the rest: we do
            // not know how far to move on.
            u.bKnown = false;
            break;
        }

        if (iStream != 0)
        {
            // Data from a further stream. There is nowhere to take it from
            // - our `SetStreamSource` accepts only stream zero - so the
            // layout is INCOMPLETE and the caller has the right to know.
            u.bKnown = false;
            continue;
        }

        PlaceRegister(u, dwRegister, iOffset, iComponents, iHighestSet);
        iOffset += iBytes;
    }

    // The set count is the HIGHEST USED number plus one, not the number of
    // occurrences. A declaration may skip a set in the middle - then the
    // hole stays with offset -1 and the shader gets zeros for it instead
    // of reading somebody else's data.
    u.iTexCoordSets = iHighestSet + 1;

    u.uStride = static_cast<UINT>(iOffset);
    return u;
}
