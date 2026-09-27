// SPDX-License-Identifier: GPL-2.0-or-later
// speedtree_web.cpp - trees from a BAKE: the reader of the
// `TMP4SPT1` blocks that `tools/speedtree_bake` produces with the real SpeedTree
// DLL, plus the book-keeping around them (instances, LOD, camera) and the
// one thing computed live - the leaf cards facing the camera.

// Design:
// WHERE THE TREES COME FROM. `CSpeedTreeRT` is a procedural engine: a
// `.spt` file carries a RECIPE (Bezier curves, seed, branching parameters)
// and the engine produces the mesh at `Compute`. There is no engine source
// - there is `SpeedTreeRT.dll` from the Windows client. Under wasm that
// library cannot run, but on the machine that BUILDS it can:
// `tools/bake_speedtree.py` builds the native `tools/speedtree_bake/bake_spt.cpp`,
// which for every `.spt` makes EXACTLY the calls `CSpeedTreeWrapper` makes
// in the client and writes what the client then reads from `GetGeometry`.
// The baked files go into the pack under the same `.spt` names
// (`build_client_data.py`, `copy_baked`) and are recognised here by magic.
// So this file is the READER of the bake and the book-keeping around it:
// instances (`MakeInstance` shares the mesh, has its own position), levels
// of detail (the wrapper always asks for 1.0 - the highest), the camera.
//
// LEAF CARDS - the one thing computed live. Leaves are cards turned to the
// camera. SpeedTree's `SetCamera` rotates the cluster table; measured on
// the DLL and checked on 8 cameras with error
// < 0.001:
//     corner = Rz(az + rho_k) * Ry(-el) * L_k,r
//     az = atan2(dir.y, dir.x)      el = asin(dir.z)      (dir NOT normalised)
// where `L_k,r` (the canonical corner) and `rho_k` (the constant angle of
// the cluster) are in the bake. The table is computed once per camera
// change, not per tree (camera generation number).
//
// WHAT IS NOT HERE (deliberately, measured):
//  * leaf rocking (`SetLeafRockingState` + `SetTime`): at wind 0.2 it
//    gives a few units on a 140-unit card - to be added separately;
//  * lower leaf LODs: the wrapper keeps LOD 1.0; there is one cluster
//    table (LOD 0), the other leaf LODs get the same table;
//  * billboards: inactive at LOD 1.0, the wrapper does not draw them.
// A raw `.spt` (not baked) is REJECTED with a message - a silent lack of
// trees cost once already.
//
// `?forest=N` - diagnostic switches, each isolating
// one factor so that "no trees visible" need not be guessed:
//   1  the forest draws ALL instances without the visibility test (isShow)
//      - a stage_port patch in SpeedTreeForestDirectX8::Render;
//   2  as 1, and trees drawn without alpha test, fog and face culling;
//   3  as 2, without depth test, vertex colour only (NOTE: the states leak
//      onto the terrain - an experiment, not a game mode);
//   4  trees drawn AFTER the terrain (MapOutdoorRender, stage_port patch);
//   5  every tree 25 m in front of the camera - does the drawing road give
//      an image at all. This mode settled it: it does (a tree in the
//      middle of the screen), and "no trees" was no trees IN FRAME.
// With `forest>=1` gl_device.cpp keeps the log `/las.txt` (vertex
// projection, sample queries) - see `ReportForestDraw` there.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "SpeedTreeRT.h"
#include <emscripten/emscripten.h>

/// `?forest=N` as an integer, 0 when absent.
EM_JS(int, m2w_forest_mode, (void), { return parseInt(m2w.options.get('forest') || '0', 10) || 0; });

/// The `?forest` switch, read once; gl_device.cpp reads it for its log and
/// for modes 2, 3 and 5.
int M2W_ForestMode()
{
    static int s_i = -1;
    if (s_i < 0) s_i = m2w_forest_mode();
    return s_i;
}

/// Mode 1 and up: the patched `SpeedTreeForestDirectX8::Render` draws every
/// instance, visible or not (`tools/stage_port.py`).
extern "C" int M2W_ForestNoClip()
{
    return M2W_ForestMode() >= 1;
}

// ---------------------------------------------------------------------------
// Tree data - the definition of the type the header only announces.
// `CTreeEngine` is here the baked mesh, `STreeInstanceData` the position of
// an instance. The header keeps pointers to them, so the class layout
// agrees without touching the header.
// ---------------------------------------------------------------------------

/// The position of one instance.
struct STreeInstanceData
{
    float afPosition[3];
};

/// The baked mesh of one `.spt`, shared by its instances (`uRefs`).
class CTreeEngine
{
public:
    unsigned uRefs;                         // how many CSpeedTreeRT share this mesh

    float afBounds[6];
    float fSize, fVariance;
    float afBranchMaterial[13], afFrondMaterial[13], afLeafMaterial[13];
    float fLeafLightingAdjustment;

    std::string strBranchTexture, strComposite, strSelfShadow;
    std::vector<std::string> vecLeafTextures, vecFrondTextures;
    std::vector<const char*> vecLeafTexturePtrs, vecFrondTexturePtrs;

    struct TCollision { unsigned uType; float afPosition[3], afSize[3]; };
    std::vector<TCollision> vecCollisions;

    /// One LOD of branches or fronds: triangle strips into the part's vertices.
    struct TLod
    {
        float fAlphaTest;
        std::vector<unsigned short> vecStripLengths;
        std::vector< std::vector<unsigned short> > vecStrips;
        std::vector<const unsigned short*> vecStripPtrs;
    };
    /// Branches or fronds: vertices plus LODs.
    struct TPart
    {
        unsigned uVertices;
        std::vector<float> vecCoords, vecTex0, vecTex1;
        std::vector<unsigned long> vecColors;
        std::vector<TLod> vecLods;
    } kBranches, kFronds;

    /// One leaf LOD: `uCount` cards.
    struct TLeaves
    {
        bool bActive;
        float fAlphaTest;
        unsigned uCount;
        std::vector<float> vecCenters, vecTexCoords;
        std::vector<unsigned long> vecColors;
        std::vector<unsigned char> vecMapIndices, vecClusterIndices;
        std::vector<const float*> vecTexCoordPtrs, vecCardPtrs;
    };
    std::vector<TLeaves> vecLeafLods;

    unsigned uClusters;
    std::vector<float> vecRho;              // uClusters
    std::vector<float> vecCanonicalCorners; // uClusters * 4 * 3
    std::vector<float> vecCardTable;        // uClusters * 4 * 4 - (x,y,z,0) after the camera
    std::vector<float> vecLeafLodScales;    // 1.0 per leaf LOD
    unsigned uCameraGeneration;

    /// An empty mesh with one reference, zero bounds and materials, and a camera
    /// generation that forces the first leaf-card update.
    CTreeEngine() : uRefs(1), fSize(0), fVariance(0), fLeafLightingAdjustment(0), uClusters(0), uCameraGeneration(~0u)
    {
        std::memset(afBounds, 0, sizeof(afBounds));
        std::memset(afBranchMaterial, 0, sizeof(afBranchMaterial));
        std::memset(afFrondMaterial, 0, sizeof(afFrondMaterial));
        std::memset(afLeafMaterial, 0, sizeof(afLeafMaterial));
        kBranches.uVertices = kFronds.uVertices = 0;
    }
};

namespace
{

// --- shared state (the static SDK methods) ----------------------------------
char s_szError[256] = "";
unsigned s_uInstances = 0, s_uPositions = 0; float s_afLastPosition[3] = { 0, 0, 0 };

}  // namespace

/// Flag for gl_device.cpp (`ReportForestDraw`): "the next draw is a tree".
int g_iM2wForestNext = 0;
/// Eye and direction of the last SetCamera - for the `?forest=5` experiment in gl_device.cpp.
float g_afM2wForestEye[3] = { 0, 0, 0 }, g_afM2wForestDir[3] = { 0, 1, 0 };

namespace
{

float s_fAzimuth = 0.0f, s_fElevation = 0.0f;
unsigned s_uCameraGeneration = 0;

/// Stores the text `CSpeedTreeRT::GetCurrentError` returns (cut to 255
/// characters).
void SetError(const char* c_sz)
{
    std::strncpy(s_szError, c_sz, sizeof(s_szError) - 1);
    s_szError[sizeof(s_szError) - 1] = 0;
}

/// Reports about rejected files - up to ten, to keep the log readable.
int s_iReports = 0;
/// Prints `c_szWhat` as a `m2w speedtree:` line - only the first ten
/// reports, later ones are dropped.
void Report(const char* c_szWhat)
{
    if (s_iReports < 10)
    {
        ++s_iReports;
        std::printf("m2w speedtree: %s\n", c_szWhat);
    }
}

// --- bake reader: every read checks the block bounds -----------------------

struct TReader
{
    const unsigned char* p;
    size_t n, i;
    bool bError;
    /// Reads the `uN`-byte block at `c_p` from the start; no error yet.
    TReader(const unsigned char* c_p, size_t uN) : p(c_p), n(uN), i(0), bError(false) {}

    /// Copies `uCount` bytes and advances; past the end (or after an earlier
    /// error) sets `bError` and copies nothing.
    bool Bytes(void* pvTarget, size_t uCount)
    {
        if (bError || i + uCount > n) { bError = true; return false; }
        if (uCount) std::memcpy(pvTarget, p + i, uCount);
        i += uCount;
        return true;
    }
    /// One byte (0 on error).
    unsigned U8()  { unsigned char c = 0; Bytes(&c, 1); return c; }
    /// A 16-bit value, host order (0 on error).
    unsigned U16() { unsigned short c = 0; Bytes(&c, 2); return c; }
    /// A 32-bit value, host order (0 on error).
    unsigned U32() { unsigned c = 0; Bytes(&c, 4); return c; }
    /// A 32-bit float (0 on error).
    float F()      { float f = 0; Bytes(&f, 4); return f; }
    /// Reads `uCount` elements of `T` into `v` (resized); sets `bError` and
    /// leaves `v` untouched when they do not fit.
    template <class T> void Array(std::vector<T>& v, size_t uCount)
    {
        if (bError || i + uCount * sizeof(T) > n) { bError = true; return; }
        v.resize(uCount);
        if (uCount) std::memcpy(&v[0], p + i, uCount * sizeof(T));
        i += uCount * sizeof(T);
    }
    /// A string prefixed by its 16-bit length; sets `bError` when it does not fit.
    void String(std::string& str)
    {
        const unsigned uLength = U16();
        if (bError || i + uLength > n) { bError = true; return; }
        str.assign((const char*)p + i, uLength);
        i += uLength;
    }
};

/// Reads one indexed part (branches or fronds): vertex count, positions,
/// colours, two texture-coordinate sets, then up to 64 LODs of up to 4096
/// triangle strips each; larger counts set `bError`.
void ReadPart(TReader& r, CTreeEngine::TPart& k)
{
    k.uVertices = r.U16();
    r.Array(k.vecCoords, 3 * k.uVertices);
    r.Array(k.vecColors, k.uVertices);
    r.Array(k.vecTex0, 2 * k.uVertices);
    r.Array(k.vecTex1, 2 * k.uVertices);
    const unsigned uLods = r.U16();
    if (r.bError || uLods > 64) { r.bError = true; return; }
    k.vecLods.resize(uLods);
    for (unsigned l = 0; l < uLods; ++l)
    {
        CTreeEngine::TLod& p = k.vecLods[l];
        p.fAlphaTest = r.F();
        const unsigned uStrips = r.U16();
        if (r.bError || uStrips > 4096) { r.bError = true; return; }
        p.vecStripLengths.resize(uStrips);
        p.vecStrips.resize(uStrips);
        p.vecStripPtrs.resize(uStrips);
        for (unsigned s = 0; s < uStrips; ++s)
        {
            const unsigned uLength = r.U16();
            r.Array(p.vecStrips[s], uLength);
            if (r.bError) return;
            p.vecStripLengths[s] = (unsigned short)uLength;
            p.vecStripPtrs[s] = uLength ? &p.vecStrips[s][0] : NULL;
        }
    }
}

/// Parses a `TMP4SPT1` block (the format `tools/speedtree_bake/bake_spt.cpp`
/// writes) into `d`. False on a short, oversized or trailing block.
bool ReadBakedTree(const unsigned char* c_pBlock, size_t uBytes, CTreeEngine& d)
{
    TReader r(c_pBlock, uBytes);
    char abyMagic[8];
    if (!r.Bytes(abyMagic, 8) || std::memcmp(abyMagic, "TMP4SPT1", 8) != 0)
        return false;
    if (r.U32() != 1) { SetError("baked tree: unknown version"); return false; }
    for (int i = 0; i < 6; ++i) d.afBounds[i] = r.F();
    d.fSize = r.F(); d.fVariance = r.F();
    for (int i = 0; i < 13; ++i) d.afBranchMaterial[i] = r.F();
    for (int i = 0; i < 13; ++i) d.afFrondMaterial[i] = r.F();
    for (int i = 0; i < 13; ++i) d.afLeafMaterial[i] = r.F();
    d.fLeafLightingAdjustment = r.F();

    r.String(d.strBranchTexture); r.String(d.strComposite); r.String(d.strSelfShadow);
    unsigned u = r.U16();
    if (r.bError || u > 64) return false;
    d.vecLeafTextures.resize(u);
    for (unsigned i = 0; i < u; ++i) r.String(d.vecLeafTextures[i]);
    u = r.U16();
    if (r.bError || u > 64) return false;
    d.vecFrondTextures.resize(u);
    for (unsigned i = 0; i < u; ++i) r.String(d.vecFrondTextures[i]);
    d.vecLeafTexturePtrs.resize(d.vecLeafTextures.size());
    for (size_t i = 0; i < d.vecLeafTextures.size(); ++i) d.vecLeafTexturePtrs[i] = d.vecLeafTextures[i].c_str();
    d.vecFrondTexturePtrs.resize(d.vecFrondTextures.size());
    for (size_t i = 0; i < d.vecFrondTextures.size(); ++i) d.vecFrondTexturePtrs[i] = d.vecFrondTextures[i].c_str();

    u = r.U32();
    if (r.bError || u > 1024) return false;
    d.vecCollisions.resize(u);
    for (unsigned i = 0; i < u; ++i)
    {
        d.vecCollisions[i].uType = r.U32();
        for (int k = 0; k < 3; ++k) d.vecCollisions[i].afPosition[k] = r.F();
        for (int k = 0; k < 3; ++k) d.vecCollisions[i].afSize[k] = r.F();
    }

    ReadPart(r, d.kBranches);
    ReadPart(r, d.kFronds);
    if (r.bError) return false;

    u = r.U16();
    if (r.bError || u > 64) return false;
    d.vecLeafLods.resize(u);
    for (unsigned l = 0; l < u; ++l)
    {
        CTreeEngine::TLeaves& s = d.vecLeafLods[l];
        s.bActive = r.U8() != 0;
        s.fAlphaTest = r.F();
        s.uCount = r.U16();
        r.Array(s.vecCenters, 3 * s.uCount);
        r.Array(s.vecColors, s.uCount);
        r.Array(s.vecMapIndices, s.uCount);
        r.Array(s.vecClusterIndices, s.uCount);
        r.Array(s.vecTexCoords, 8 * s.uCount);
        if (r.bError) return false;
        s.vecTexCoordPtrs.resize(s.uCount);
        for (unsigned i = 0; i < s.uCount; ++i) s.vecTexCoordPtrs[i] = &s.vecTexCoords[8 * i];
    }
    d.vecLeafLodScales.assign(u ? u : 1, 1.0f);

    d.uClusters = r.U16();
    if (r.bError || d.uClusters > 256) return false;
    d.vecRho.resize(d.uClusters);
    d.vecCanonicalCorners.resize(d.uClusters * 12);
    for (unsigned k = 0; k < d.uClusters; ++k)
    {
        d.vecRho[k] = r.F();
        for (int j = 0; j < 12; ++j) d.vecCanonicalCorners[k * 12 + j] = r.F();
    }
    if (r.bError) return false;
    d.vecCardTable.assign(d.uClusters * 16, 0.0f);

    // card pointers: every leaf points at the table entry of its cluster
    for (size_t l = 0; l < d.vecLeafLods.size(); ++l)
    {
        CTreeEngine::TLeaves& s = d.vecLeafLods[l];
        s.vecCardPtrs.resize(s.uCount);
        for (unsigned i = 0; i < s.uCount; ++i)
        {
            const unsigned k = s.vecClusterIndices[i] < d.uClusters ? s.vecClusterIndices[i] : 0;
            s.vecCardPtrs[i] = d.uClusters ? &d.vecCardTable[16 * k] : NULL;
        }
    }
    if (r.i != r.n) { SetError("baked tree: parsing does not end at the end of the file"); return false; }
    return true;
}

/// Cluster table for the current camera: corner = Rz(az + rho) * Ry(-el) * L.
void UpdateCardTable(CTreeEngine& d)
{
    if (d.uCameraGeneration == s_uCameraGeneration)
        return;
    d.uCameraGeneration = s_uCameraGeneration;
    const float cE = std::cos(-s_fElevation), sE = std::sin(-s_fElevation);
    for (unsigned k = 0; k < d.uClusters; ++k)
    {
        const float fAngle = s_fAzimuth + d.vecRho[k];
        const float cA = std::cos(fAngle), sA = std::sin(fAngle);
        for (int r = 0; r < 4; ++r)
        {
            const float* L = &d.vecCanonicalCorners[k * 12 + r * 3];
            // Ry(-el): x' = cE*x + sE*z ; y' = y ; z' = -sE*x + cE*z
            const float x1 = cE * L[0] + sE * L[2];
            const float y1 = L[1];
            const float z1 = -sE * L[0] + cE * L[2];
            // Rz(angle): x'' = cA*x - sA*y ; y'' = sA*x + cA*y
            float* w = &d.vecCardTable[k * 16 + r * 4];
            w[0] = cA * x1 - sA * y1;
            w[1] = sA * x1 + cA * y1;
            w[2] = z1;
            w[3] = 0.0f;
        }
    }
}

/// Points the SDK geometry struct at part `k`'s arrays and at the strips of
/// LOD `iLod` (no normals, tangents or wind); an LOD out of range leaves the
/// vertices but no strips and level -1.
void FillPart(CSpeedTreeRT::SGeometry::SIndexed& s, const CTreeEngine::TPart& k, int iLod)
{
    s.m_usVertexCount = (unsigned short)k.uVertices;
    s.m_pCoords = k.uVertices ? &k.vecCoords[0] : NULL;
    s.m_pColors = k.uVertices ? &k.vecColors[0] : NULL;
    s.m_pTexCoords0 = k.uVertices ? &k.vecTex0[0] : NULL;
    s.m_pTexCoords1 = k.uVertices ? &k.vecTex1[0] : NULL;
    s.m_pNormals = s.m_pBinormals = s.m_pTangents = NULL;
    s.m_pWindWeights = NULL; s.m_pWindMatrixIndices = NULL;
    if (iLod < 0 || iLod >= (int)k.vecLods.size())
    {
        s.m_nDiscreteLodLevel = -1;
        s.m_usNumStrips = 0; s.m_pStripLengths = NULL; s.m_pStrips = NULL;
        return;
    }
    const CTreeEngine::TLod& p = k.vecLods[iLod];
    s.m_nDiscreteLodLevel = iLod;
    s.m_usNumStrips = (unsigned short)p.vecStripLengths.size();
    s.m_pStripLengths = p.vecStripLengths.empty() ? NULL : &p.vecStripLengths[0];
    s.m_pStrips = p.vecStripPtrs.empty() ? NULL : const_cast<const unsigned short**>(&p.vecStripPtrs[0]);
}

/// Points the SDK leaf struct at leaf LOD `iLod` (map/cluster indices,
/// centres, card texture coordinates and corners, colours; no normals or
/// wind); an LOD out of range marks the leaves inactive and empty.
void FillLeaves(CSpeedTreeRT::SGeometry::SLeaf& s, const CTreeEngine& d, int iLod)
{
    if (iLod < 0 || iLod >= (int)d.vecLeafLods.size())
    {
        s.m_bIsActive = false; s.m_nDiscreteLodLevel = -1; s.m_usLeafCount = 0;
        s.m_fAlphaTestValue = 0.0f;
        s.m_pLeafMapIndices = s.m_pLeafClusterIndices = NULL;
        s.m_pCenterCoords = NULL; s.m_pLeafMapTexCoords = NULL; s.m_pLeafMapCoords = NULL;
        s.m_pColors = NULL; s.m_pNormals = s.m_pBinormals = s.m_pTangents = NULL;
        s.m_pWindWeights = NULL; s.m_pWindMatrixIndices = NULL;
        return;
    }
    const CTreeEngine::TLeaves& l = d.vecLeafLods[iLod];
    s.m_bIsActive = true;
    s.m_fAlphaTestValue = l.fAlphaTest;
    s.m_nDiscreteLodLevel = iLod;
    s.m_usLeafCount = (unsigned short)l.uCount;
    s.m_pLeafMapIndices = l.uCount ? &l.vecMapIndices[0] : NULL;
    s.m_pLeafClusterIndices = l.uCount ? &l.vecClusterIndices[0] : NULL;
    s.m_pCenterCoords = l.uCount ? &l.vecCenters[0] : NULL;
    s.m_pLeafMapTexCoords = l.uCount ? const_cast<const float**>(&l.vecTexCoordPtrs[0]) : NULL;
    s.m_pLeafMapCoords = l.uCount ? const_cast<const float**>(&l.vecCardPtrs[0]) : NULL;
    s.m_pColors = l.uCount ? &l.vecColors[0] : NULL;
    s.m_pNormals = s.m_pBinormals = s.m_pTangents = NULL;
    s.m_pWindWeights = NULL; s.m_pWindMatrixIndices = NULL;
}

const float c_afZeros[16] = { 0.0f };

}  // namespace

// ---------------------------------------------------------------------------
// Static SDK fields
// ---------------------------------------------------------------------------
bool CSpeedTreeRT::m_bTextureFlip = false;
bool CSpeedTreeRT::m_bDropToBillboard = false;

// ---------------------------------------------------------------------------
// Global settings
// ---------------------------------------------------------------------------
void CSpeedTreeRT::SetNumWindMatrices(unsigned int) {}
void CSpeedTreeRT::SetWindMatrix(unsigned int, const float*) {}
void CSpeedTreeRT::SetTime(float) {}
void CSpeedTreeRT::SetLightState(unsigned int, bool) {}
void CSpeedTreeRT::SetLightAttributes(unsigned int, const float*) {}
void CSpeedTreeRT::SetTextureFlip(bool bFlip) { m_bTextureFlip = bFlip; }
void CSpeedTreeRT::SetDropToBillboard(bool bFlag) { m_bDropToBillboard = bFlag; }

/// Azimuth and elevation of the view direction, as the DLL: WITHOUT
/// normalisation. The one difference: asin outside [-1, 1] would give NaN
/// and vanishing leaves - here it is clamped. A changed angle bumps the
/// camera generation, which makes every tree recompute its card table.
void CSpeedTreeRT::SetCamera(const float* c_pafEye, const float* c_pafDirection)
{
    if (!c_pafDirection)
        return;
    if (c_pafEye) { g_afM2wForestEye[0] = c_pafEye[0]; g_afM2wForestEye[1] = c_pafEye[1]; g_afM2wForestEye[2] = c_pafEye[2]; }
    g_afM2wForestDir[0] = c_pafDirection[0]; g_afM2wForestDir[1] = c_pafDirection[1]; g_afM2wForestDir[2] = c_pafDirection[2];
    static unsigned s_uCalls = 0;
    if ((++s_uCalls % 6000) == 1)
        std::printf("m2w speedtree: SetCamera #%u eye (%.0f, %.0f, %.0f) dir (%.3f, %.3f, %.3f); instances %u, positions %u, last (%.0f, %.0f, %.0f)\n", s_uCalls,
                    c_pafEye ? c_pafEye[0] : 0.0f, c_pafEye ? c_pafEye[1] : 0.0f, c_pafEye ? c_pafEye[2] : 0.0f,
                    c_pafDirection[0], c_pafDirection[1], c_pafDirection[2], s_uInstances, s_uPositions, s_afLastPosition[0], s_afLastPosition[1], s_afLastPosition[2]);
    float fZ = c_pafDirection[2];
    if (fZ > 1.0f) fZ = 1.0f;
    if (fZ < -1.0f) fZ = -1.0f;
    const float fAzimuth = std::atan2(c_pafDirection[1], c_pafDirection[0]);
    const float fElevation = std::asin(fZ);
    if (fAzimuth == s_fAzimuth && fElevation == s_fElevation)
        return;
    s_fAzimuth = fAzimuth; s_fElevation = fElevation;
    ++s_uCameraGeneration;
}

const char* CSpeedTreeRT::GetCurrentError(void)
{
    return s_szError;
}

// ---------------------------------------------------------------------------
// Object lifetime and instances
// ---------------------------------------------------------------------------
CSpeedTreeRT::CSpeedTreeRT()
{
    std::memset((void*)&m_pEngine, 0, (char*)(this + 1) - (char*)&m_pEngine);
    m_pInstanceData = new STreeInstanceData;
    std::memset(m_pInstanceData, 0, sizeof(STreeInstanceData));
}

/// An instance: shares the source's mesh (reference counted), owns its position.
CSpeedTreeRT::CSpeedTreeRT(const CSpeedTreeRT* c_pSource)
{
    std::memset((void*)&m_pEngine, 0, (char*)(this + 1) - (char*)&m_pEngine);
    m_pEngine = c_pSource->m_pEngine;
    if (m_pEngine)
        ++m_pEngine->uRefs;
    m_pInstanceData = new STreeInstanceData;
    std::memset(m_pInstanceData, 0, sizeof(STreeInstanceData));
    m_bTreeComputed = c_pSource->m_bTreeComputed;
}

CSpeedTreeRT::~CSpeedTreeRT()
{
    if (m_pEngine && --m_pEngine->uRefs == 0)
        delete m_pEngine;
    delete m_pInstanceData;
}


CSpeedTreeRT* CSpeedTreeRT::MakeInstance(void)
{
    if (!m_pEngine)
        return NULL;
    ++s_uInstances;
    return new CSpeedTreeRT(this);
}

CSpeedTreeRT* CSpeedTreeRT::Clone(float, float, float, unsigned int) const
{
    return NULL;
}

const CSpeedTreeRT* CSpeedTreeRT::InstanceOf(void) const
{
    return NULL;
}

void CSpeedTreeRT::DeleteTransientData(void) {}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

/// A `TMP4SPT1` block from the pack. A raw `.spt` (the recipe, which cannot
/// be computed here) is rejected with a report.
bool CSpeedTreeRT::LoadTree(const unsigned char* c_pBlock, unsigned int uBytes)
{
    if (!c_pBlock || uBytes < 8)
    {
        SetError("empty block");
        return false;
    }
    if (std::memcmp(c_pBlock, "TMP4SPT1", 8) != 0)
    {
        SetError("raw .spt without a bake - run tools/bake_speedtree.py "
                 "and tools/build_client_data.py");
        Report("got a raw .spt instead of a bake (tools/bake_speedtree.py)");
        return false;
    }
    CTreeEngine* pNew = new CTreeEngine;
    s_szError[0] = 0;
    if (!ReadBakedTree(c_pBlock, uBytes, *pNew))
    {
        if (!s_szError[0])
            SetError("baked tree: damaged block");
        Report(s_szError);
        delete pNew;
        return false;
    }
    if (m_pEngine && --m_pEngine->uRefs == 0)
        delete m_pEngine;
    m_pEngine = pNew;
    m_bTreeComputed = false;
    // A report for every loaded tree (up to 20) - so that no trees on
    // screen can be told from no trees in the data.
    static int s_iLoaded = 0;
    if (++s_iLoaded <= 20)
        std::printf("m2w speedtree: tree %d loaded - branches %u, fronds %u, leaves %u, clusters %u\n",
                    s_iLoaded, pNew->kBranches.uVertices, pNew->kFronds.uVertices,
                    pNew->vecLeafLods.empty() ? 0u : pNew->vecLeafLods[0].uCount, pNew->uClusters);
    return true;
}

/// The file-name form (the wrapper's fallback when the pack has no block).
bool CSpeedTreeRT::LoadTree(const char* c_szName)
{
    if (!c_szName)
        return false;
    std::FILE* f = std::fopen(c_szName, "rb");
    if (!f)
    {
        SetError("no tree file");
        return false;
    }
    std::vector<unsigned char> v;
    unsigned char aby[16384];
    size_t n;
    while ((n = std::fread(aby, 1, sizeof(aby), f)) > 0)
        v.insert(v.end(), aby, aby + n);
    std::fclose(f);
    return LoadTree(v.empty() ? NULL : &v[0], (unsigned)v.size());
}

/// The bake already IS the computed tree; only the flag is set.
bool CSpeedTreeRT::Compute(const float*, unsigned int, bool)
{
    if (!m_pEngine)
    {
        SetError("Compute without a loaded tree");
        return false;
    }
    m_bTreeComputed = true;
    return true;
}

// ---------------------------------------------------------------------------
// Tree settings - the bake accounts for them already, so only book-keeping
// ---------------------------------------------------------------------------
void CSpeedTreeRT::SetTreeSize(float fSize, float fVariance)
{
    if (m_pEngine) { m_pEngine->fSize = fSize; m_pEngine->fVariance = fVariance; }
}
void CSpeedTreeRT::GetTreeSize(float& fSize, float& fVariance) const
{
    fSize = m_pEngine ? m_pEngine->fSize : 0.0f;
    fVariance = m_pEngine ? m_pEngine->fVariance : 0.0f;
}
unsigned int CSpeedTreeRT::GetSeed() const { return 1; }

void CSpeedTreeRT::SetTreePosition(float x, float y, float z)
{
    m_pInstanceData->afPosition[0] = x; m_pInstanceData->afPosition[1] = y; m_pInstanceData->afPosition[2] = z;
    ++s_uPositions; s_afLastPosition[0] = x; s_afLastPosition[1] = y; s_afLastPosition[2] = z;
}
const float* CSpeedTreeRT::GetTreePosition(void) const { return m_pInstanceData->afPosition; }

void CSpeedTreeRT::SetLodLevel(float) {}
float CSpeedTreeRT::GetLodLevel(void) const { return 1.0f; }
void CSpeedTreeRT::SetLodLimits(float, float) {}
void CSpeedTreeRT::GetLodLimits(float& fNear, float& fFar) const { fNear = 0.0f; fFar = 0.0f; }
void CSpeedTreeRT::ComputeLodLevel(void) {}
unsigned short CSpeedTreeRT::GetDiscreteLeafLodLevel(float) const { return 0; }
void CSpeedTreeRT::SetLocalMatrices(unsigned int, unsigned int) {}
void CSpeedTreeRT::GetLocalMatrices(unsigned int& uFirst, unsigned int& uCount) { uFirst = 0; uCount = 4; }
void CSpeedTreeRT::SetLeafRockingState(bool) {}
bool CSpeedTreeRT::GetLeafRockingState(void) const { return false; }
void CSpeedTreeRT::SetNumLeafRockingGroups(unsigned int) {}
void CSpeedTreeRT::ResetLeafWindState(void) {}
void CSpeedTreeRT::ComputeWindEffects(bool, bool, bool) {}
float CSpeedTreeRT::GetWindStrength(void) const { return 0.0f; }
float CSpeedTreeRT::SetWindStrength(float fNew, float, float) { return fNew; }

void CSpeedTreeRT::SetLeafWindMethod(EWindMethod) {}
void CSpeedTreeRT::SetBranchWindMethod(EWindMethod) {}
void CSpeedTreeRT::SetFrondWindMethod(EWindMethod) {}
CSpeedTreeRT::EWindMethod CSpeedTreeRT::GetLeafWindMethod(void) const { return WIND_NONE; }
CSpeedTreeRT::EWindMethod CSpeedTreeRT::GetBranchWindMethod(void) const { return WIND_NONE; }
CSpeedTreeRT::EWindMethod CSpeedTreeRT::GetFrondWindMethod(void) const { return WIND_NONE; }

void CSpeedTreeRT::SetBranchLightingMethod(ELightingMethod) {}
void CSpeedTreeRT::SetLeafLightingMethod(ELightingMethod) {}
void CSpeedTreeRT::SetFrondLightingMethod(ELightingMethod) {}
CSpeedTreeRT::ELightingMethod CSpeedTreeRT::GetBranchLightingMethod(void) const { return LIGHT_STATIC; }
CSpeedTreeRT::ELightingMethod CSpeedTreeRT::GetLeafLightingMethod(void) const { return LIGHT_STATIC; }
CSpeedTreeRT::ELightingMethod CSpeedTreeRT::GetFrondLightingMethod(void) const { return LIGHT_STATIC; }
void CSpeedTreeRT::SetStaticLightingStyle(EStaticLightingStyle) {}
CSpeedTreeRT::EStaticLightingStyle CSpeedTreeRT::GetStaticLightingStyle(void) const { return SLS_BASIC; }
float CSpeedTreeRT::GetLeafLightingAdjustment() const { return m_pEngine ? m_pEngine->fLeafLightingAdjustment : 0.0f; }
void CSpeedTreeRT::SetLeafLightingAdjustment(float f) { if (m_pEngine) m_pEngine->fLeafLightingAdjustment = f; }

const float* CSpeedTreeRT::GetBranchMaterial(void) const { return m_pEngine ? m_pEngine->afBranchMaterial : c_afZeros; }
const float* CSpeedTreeRT::GetLeafMaterial(void) const   { return m_pEngine ? m_pEngine->afLeafMaterial : c_afZeros; }
const float* CSpeedTreeRT::GetFrondMaterial(void) const  { return m_pEngine ? m_pEngine->afFrondMaterial : c_afZeros; }
void CSpeedTreeRT::SetBranchMaterial(const float* p) { if (m_pEngine && p) std::memcpy(m_pEngine->afBranchMaterial, p, 13 * sizeof(float)); }
void CSpeedTreeRT::SetLeafMaterial(const float* p)   { if (m_pEngine && p) std::memcpy(m_pEngine->afLeafMaterial, p, 13 * sizeof(float)); }
void CSpeedTreeRT::SetFrondMaterial(const float* p)  { if (m_pEngine && p) std::memcpy(m_pEngine->afFrondMaterial, p, 13 * sizeof(float)); }

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
unsigned short CSpeedTreeRT::GetNumBranchLodLevels(void) const { return m_pEngine ? (unsigned short)m_pEngine->kBranches.vecLods.size() : 0; }
unsigned short CSpeedTreeRT::GetNumFrondLodLevels(void) const  { return m_pEngine ? (unsigned short)m_pEngine->kFronds.vecLods.size() : 0; }
unsigned short CSpeedTreeRT::GetNumLeafLodLevels(void) const   { return m_pEngine ? (unsigned short)m_pEngine->vecLeafLods.size() : 0; }

void CSpeedTreeRT::GetBoundingBox(float* pfBounds) const
{
    if (!pfBounds) return;
    if (m_pEngine) std::memcpy(pfBounds, m_pEngine->afBounds, sizeof(float) * 6);
    else std::memset(pfBounds, 0, sizeof(float) * 6);
}

unsigned int CSpeedTreeRT::GetCollisionObjectCount(void)
{
    return m_pEngine ? (unsigned)m_pEngine->vecCollisions.size() : 0;
}

void CSpeedTreeRT::GetCollisionObject(unsigned int uIndex, ECollisionObjectType& eType,
                                      float* pfPosition, float* pfSize)
{
    if (!m_pEngine || uIndex >= m_pEngine->vecCollisions.size())
    {
        eType = CO_SPHERE;
        if (pfPosition) std::memset(pfPosition, 0, sizeof(float) * 3);
        if (pfSize)     std::memset(pfSize,     0, sizeof(float) * 3);
        return;
    }
    const CTreeEngine::TCollision& k = m_pEngine->vecCollisions[uIndex];
    eType = (ECollisionObjectType)k.uType;
    if (pfPosition) std::memcpy(pfPosition, k.afPosition, sizeof(float) * 3);
    if (pfSize)     std::memcpy(pfSize,     k.afSize,     sizeof(float) * 3);
}

void CSpeedTreeRT::GetTextures(STextures& s) const
{
    std::memset(&s, 0, sizeof(s));
    if (!m_pEngine) return;
    s.m_pBranchTextureFilename = m_pEngine->strBranchTexture.c_str();
    s.m_pCompositeFilename = m_pEngine->strComposite.empty() ? NULL : m_pEngine->strComposite.c_str();
    s.m_pSelfShadowFilename = m_pEngine->strSelfShadow.empty() ? NULL : m_pEngine->strSelfShadow.c_str();
    s.m_uiLeafTextureCount = (unsigned)m_pEngine->vecLeafTexturePtrs.size();
    s.m_pLeafTextureFilenames = s.m_uiLeafTextureCount ? &m_pEngine->vecLeafTexturePtrs[0] : NULL;
    s.m_uiFrondTextureCount = (unsigned)m_pEngine->vecFrondTexturePtrs.size();
    s.m_pFrondTextureFilenames = s.m_uiFrondTextureCount ? &m_pEngine->vecFrondTexturePtrs[0] : NULL;
}

const float* CSpeedTreeRT::GetLeafBillboardTable(unsigned int& uEntries) const
{
    if (!m_pEngine || m_pEngine->vecCardTable.empty()) { uEntries = 0; return NULL; }
    UpdateCardTable(*m_pEngine);
    uEntries = (unsigned)m_pEngine->vecCardTable.size();
    return &m_pEngine->vecCardTable[0];
}

const float* CSpeedTreeRT::GetLeafLodSizeAdjustments(void)
{
    return m_pEngine ? &m_pEngine->vecLeafLodScales[0] : c_afZeros;
}


unsigned int CSpeedTreeRT::GetLeafTriangleCount(float) const
{
    return m_pEngine && !m_pEngine->vecLeafLods.empty() ? 2 * m_pEngine->vecLeafLods[0].uCount : 0;
}
unsigned int CSpeedTreeRT::GetBranchTriangleCount(float) const
{
    if (!m_pEngine || m_pEngine->kBranches.vecLods.empty()) return 0;
    const CTreeEngine::TLod& p = m_pEngine->kBranches.vecLods[0];
    return p.vecStripLengths.empty() ? 0 : (unsigned)(p.vecStripLengths[0] > 2 ? p.vecStripLengths[0] - 2 : 0);
}
unsigned int CSpeedTreeRT::GetFrondTriangleCount(float) const
{
    if (!m_pEngine || m_pEngine->kFronds.vecLods.empty()) return 0;
    const CTreeEngine::TLod& p = m_pEngine->kFronds.vecLods[0];
    return p.vecStripLengths.empty() ? 0 : (unsigned)(p.vecStripLengths[0] > 2 ? p.vecStripLengths[0] - 2 : 0);
}

/// Fills the requested parts with pointers into the bake; leaves get the
/// card table of the current camera. Every call flags the next draw as a
/// tree for gl_device.cpp (`g_iM2wForestNext`).
void CSpeedTreeRT::GetGeometry(SGeometry& g, unsigned long ulMask, short sBranchLod, short sFrondLod, short sLeafLod)
{
    if (!m_pEngine)
        return;
    CTreeEngine& d = *m_pEngine;
    g_iM2wForestNext = 1;
    // Measure: how often the wrapper asks for geometry (= how often it
    // draws). Every 20000 branch queries one line with the last tree
    // position and the camera.
    {
        static unsigned s_uBranches = 0, s_uLeaves = 0;
        if (ulMask & SpeedTree_LeafGeometry) ++s_uLeaves;
        if ((ulMask & SpeedTree_BranchGeometry) && (++s_uBranches % 20000) == 1)
            std::printf("m2w speedtree: branch queries %u, leaf queries %u; tree at (%.0f, %.0f, %.0f), branch vertices %u; camera az %.2f el %.2f\n",
                        s_uBranches, s_uLeaves, m_pInstanceData->afPosition[0], m_pInstanceData->afPosition[1],
                        m_pInstanceData->afPosition[2], d.kBranches.uVertices, s_fAzimuth, s_fElevation);
    }
    if (ulMask & SpeedTree_BranchGeometry)
    {
        FillPart(g.m_sBranches, d.kBranches, sBranchLod >= 0 ? sBranchLod : 0);
        g.m_fBranchAlphaTestValue = g.m_sBranches.m_nDiscreteLodLevel >= 0 ? d.kBranches.vecLods[g.m_sBranches.m_nDiscreteLodLevel].fAlphaTest : 0.0f;
    }
    if (ulMask & SpeedTree_FrondGeometry)
    {
        FillPart(g.m_sFronds, d.kFronds, sFrondLod >= 0 ? sFrondLod : 0);
        g.m_fFrondAlphaTestValue = g.m_sFronds.m_nDiscreteLodLevel >= 0 ? d.kFronds.vecLods[g.m_sFronds.m_nDiscreteLodLevel].fAlphaTest : 0.0f;
    }
    if (ulMask & SpeedTree_LeafGeometry)
    {
        UpdateCardTable(d);
        FillLeaves(g.m_sLeaves0, d, sLeafLod >= 0 ? sLeafLod : 0);
        FillLeaves(g.m_sLeaves1, d, -1);
    }
    if (ulMask & SpeedTree_BillboardGeometry)
    {
        g.m_sBillboard0.m_bIsActive = false;
        g.m_sBillboard1.m_bIsActive = false;
        g.m_sHorizontalBillboard.m_bIsActive = false;
    }
}

const char* CSpeedTreeRT::GetUserData(void) const { return ""; }

// Descriptive structs: the constructors zero them (declared in our SpeedTreeRT.h).
CSpeedTreeRT::SGeometry::SIndexed::SIndexed()     { std::memset(this, 0, sizeof(*this)); m_nDiscreteLodLevel = -1; }
CSpeedTreeRT::SGeometry::SLeaf::SLeaf()           { std::memset(this, 0, sizeof(*this)); m_nDiscreteLodLevel = -1; }
CSpeedTreeRT::SGeometry::SBillboard::SBillboard() { std::memset(this, 0, sizeof(*this)); }
CSpeedTreeRT::STextures::STextures()              { std::memset(this, 0, sizeof(*this)); }
