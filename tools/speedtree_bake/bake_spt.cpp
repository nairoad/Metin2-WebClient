// bake_spt.cpp - BAKING SPEEDTREE TREES INTO A FINISHED MESH.
//
// SpeedTree RT is not a reader but a generator: `.spt` carries a recipe
// (curves, seed, parameters), and the mesh is produced by the `CSpeedTreeRT`
// engine, whose sources we do not have - we have the Windows `SpeedTreeRT.dll`
// from the client and the `.lib` for it. Under wasm they cannot be used, but on this machine THEY CAN.
//
// So: this program (x86, MSVC) loads every `.spt` with EXACTLY the same
// sequence of calls that `CSpeedTreeWrapper::LoadTree` performs
// in the client (the same lighting and wind methods, seed 1, size
// from the file), and writes what the client later reads from `GetGeometry`:
// branch and frond vertices with static lighting colours,
// the index strips of every level of detail, the leaves (centres, colours,
// texture coordinates, clusters) and the leaf cluster tables in
// canonical form. In the client `speedtree_web.cpp` reads this record instead of
// computing the tree.
//
// LEAF CARDS (measured): `SetCamera` rotates every cluster
// by the formula  corner = Rz(az + rho_k) * Ry(-el) * Rz(-rho_k) * corner(az=0,el=0),
// where az = atan2(dir.y, dir.x), el = asin(dir.z), and rho_k is the cluster's
// constant angle. We store L_k = Rz(-rho_k) * corner(0,0) and rho_k; the client assembles
// the rest. Rho_k is determined by measurement: for el = +-90 the component a = (v(90)+v(-90))/2
// lies on the rotation axis (in the XY plane). The program ITSELF CHECKS the formula on
// a third camera (az 45, el 45) and refuses to write when the error > 0.01.
//
// Leaf rocking (`SetLeafRockingState(true)` + `SetTime`) is NOT
// baked - we bake with rocking off; it is a measured, small
// difference (a few units on a 140-unit card) to be added separately.
//
// LOD: with `SetLodLevel(1.0)` the DLL returns `m_nDiscreteLodLevel == 0` for
// branches, fronds and leaves (measured, measurements/probe2.cpp) -
// so index 0 = the highest detail, and that is what the client gets by default.
// The measurements the card formula came from: the `measurements/` directory (probe2.cpp
// builds with the same build.bat as the baker; model.py checks the formula).
//
// Usage:  bake_spt.exe <file.spt> <output.file>
//          bake_spt.exe --dir <directory_with_spt> <output_directory>
// Output format: see `speedtree_web.cpp` (the reader).

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <windows.h>
#include "SpeedTreeRT.h"

namespace {

const char c_szMagic[8] = { 'T', 'M', 'P', '4', 'S', 'P', 'T', '1' };
const unsigned c_uVersion = 1;

/// Little-endian byte writer of the baked record (`b` raw bytes, `u8/u16/u32`,
/// `f` float, `fs` float array, `text` u16 length + bytes).
struct Writer
{
    std::vector<unsigned char> v;
    // NULL (e.g. normals with static lighting, a second UV set without
    // self-shadow) we write as zeros - the client does not read them anyway.
    void b(const void* p, size_t n) { if (!p) { v.insert(v.end(), n, (unsigned char)0); return; } const unsigned char* c = (const unsigned char*)p; v.insert(v.end(), c, c + n); }
    void u8(unsigned x) { unsigned char c = (unsigned char)x; b(&c, 1); }
    void u16(unsigned x) { unsigned short c = (unsigned short)x; b(&c, 2); }
    void u32(unsigned x) { b(&x, 4); }
    void f(float x) { b(&x, 4); }
    void fs(const float* p, size_t n) { if (n) b(p, n * 4); }
    void text(const char* s) { size_t n = s ? strlen(s) : 0; u16((unsigned)n); if (n) b(s, n); }
};

/// Rotation matrix about Z by `a` radians (3x3, row-major).
void Rz(float a, float m[9])
{
    float c = cosf(a), s = sinf(a);
    m[0] = c; m[1] = -s; m[2] = 0;  m[3] = s; m[4] = c; m[5] = 0;  m[6] = 0; m[7] = 0; m[8] = 1;
}
/// Rotation matrix about Y by `a` radians (3x3, row-major).
void Ry(float a, float m[9])
{
    float c = cosf(a), s = sinf(a);
    m[0] = c; m[1] = 0; m[2] = s;  m[3] = 0; m[4] = 1; m[5] = 0;  m[6] = -s; m[7] = 0; m[8] = c;
}
/// 3x3 matrix product r = a * b.
void Mul(const float a[9], const float b[9], float r[9])
{
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}
/// 3x3 matrix times vector r = m * v.
void MulV(const float m[9], const float v[3], float r[3])
{
    for (int i = 0; i < 3; ++i) r[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
}

/// The cluster table after setting the camera: a copy of `GetLeafBillboardTable`.
std::vector<float> TableAtCamera(CSpeedTreeRT* p, CSpeedTreeRT::SGeometry& g, float dx, float dy, float dz)
{
    float afEye[3] = { 0, 0, 0 }, afDir[3] = { dx, dy, dz };
    CSpeedTreeRT::SetCamera(afEye, afDir);
    p->GetGeometry(g, SpeedTree_LeafGeometry);
    unsigned n = 0; const float* t = p->GetLeafBillboardTable(n);
    return std::vector<float>(t, t + n);
}

const char* c_szError = NULL;

/// Loads one `.spt` exactly as the client wrapper does, bakes it into the
/// TMP4SPT1 record (checking the leaf card formula) and writes `c_szOutput`;
/// false with `c_szError` set on any failure.
bool Bake(const char* c_szSpt, const char* c_szOutput)
{
    FILE* f = fopen(c_szSpt, "rb");
    if (!f) { c_szError = "no such file"; return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> block(n > 0 ? n : 1);
    fread(&block[0], 1, n, f); fclose(f);

    // --- exactly like CSpeedTreeWrapper (constructor + LoadTree) ---
    CSpeedTreeRT* p = new CSpeedTreeRT;
    p->SetWindStrength(1.0f);
    p->SetLocalMatrices(0, 4);
    p->SetTextureFlip(true);
    if (!p->LoadTree(&block[0], (unsigned)n)) { c_szError = CSpeedTreeRT::GetCurrentError(); delete p; return false; }
    p->SetBranchLightingMethod(CSpeedTreeRT::LIGHT_STATIC);
    p->SetLeafLightingMethod(CSpeedTreeRT::LIGHT_STATIC);
    p->SetFrondLightingMethod(CSpeedTreeRT::LIGHT_STATIC);
    p->SetBranchWindMethod(CSpeedTreeRT::WIND_NONE);
    p->SetLeafWindMethod(CSpeedTreeRT::WIND_NONE);
    p->SetFrondWindMethod(CSpeedTreeRT::WIND_NONE);
    p->SetNumLeafRockingGroups(1);
    if (!p->Compute(NULL, 1)) { c_szError = CSpeedTreeRT::GetCurrentError(); delete p; return false; }
    float afBounds[6]; p->GetBoundingBox(afBounds);
    p->SetLeafRockingState(false);            // we bake without rocking (see the header)
    CSpeedTreeRT::SetDropToBillboard(true);
    const float fHeight = afBounds[5] - afBounds[2];
    p->SetLodLimits(fHeight * 2.0f, fHeight * 9.0f);    // c_fNearLodFactor / c_fFarLodFactor from SpeedTreeConfig.h (reviewer)
    p->SetLodLevel(1.0f);
    p->SetWindStrength(0.0f);

    Writer z;
    z.b(c_szMagic, 8); z.u32(c_uVersion);
    z.fs(afBounds, 6);
    float fSize = 0, fVariance = 0; p->GetTreeSize(fSize, fVariance);
    z.f(fSize); z.f(fVariance);
    z.fs(p->GetBranchMaterial(), 13); z.fs(p->GetFrondMaterial(), 13); z.fs(p->GetLeafMaterial(), 13);
    z.f(p->GetLeafLightingAdjustment());

    CSpeedTreeRT::STextures t; p->GetTextures(t);
    z.text(t.m_pBranchTextureFilename); z.text(t.m_pCompositeFilename); z.text(t.m_pSelfShadowFilename);
    z.u16(t.m_uiLeafTextureCount);  for (unsigned i = 0; i < t.m_uiLeafTextureCount; ++i) z.text(t.m_pLeafTextureFilenames[i]);
    z.u16(t.m_uiFrondTextureCount); for (unsigned i = 0; i < t.m_uiFrondTextureCount; ++i) z.text(t.m_pFrondTextureFilenames[i]);

    const unsigned uCollisions = p->GetCollisionObjectCount();
    z.u32(uCollisions);
    for (unsigned i = 0; i < uCollisions; ++i)
    {
        CSpeedTreeRT::ECollisionObjectType e; float afP[3], afDims[3];
        p->GetCollisionObject(i, e, afP, afDims);
        z.u32((unsigned)e); z.fs(afP, 3); z.fs(afDims, 3);
    }

    CSpeedTreeRT::SGeometry g;
    p->GetGeometry(g);

    // --- branches and fronds: shared vertices, strips per LOD ---
    for (int iPart = 0; iPart < 2; ++iPart)
    {
        const unsigned long ulMask = iPart == 0 ? SpeedTree_BranchGeometry : SpeedTree_FrondGeometry;
        CSpeedTreeRT::SGeometry::SIndexed& s = iPart == 0 ? g.m_sBranches : g.m_sFronds;
        const unsigned uLod = iPart == 0 ? p->GetNumBranchLodLevels() : p->GetNumFrondLodLevels();
        const unsigned uW = s.m_usVertexCount;
        z.u16(uW);
        z.fs(s.m_pCoords, 3 * uW);
        z.b(s.m_pColors, 4 * uW);
        z.fs(s.m_pTexCoords0, 2 * uW); z.fs(s.m_pTexCoords1, 2 * uW);
        // we do NOT write normals: the lighting is static (colours),
        // and the wrapper does not read normals - a quarter of the size less.
        z.u16(uLod);
        for (unsigned l = 0; l < uLod; ++l)
        {
            if (iPart == 0) p->GetGeometry(g, ulMask, (short)l);
            else            p->GetGeometry(g, ulMask, -1, (short)l);
            z.f(iPart == 0 ? g.m_fBranchAlphaTestValue : g.m_fFrondAlphaTestValue);
            const unsigned uStrips = uW > 1 ? s.m_usNumStrips : 0;
            z.u16(uStrips);
            for (unsigned k = 0; k < uStrips; ++k)
            {
                z.u16(s.m_pStripLengths[k]);
                z.b(s.m_pStrips[k], 2 * s.m_pStripLengths[k]);
            }
        }
        if (iPart == 0) p->GetGeometry(g, ulMask, 0); else p->GetGeometry(g, ulMask, -1, 0);
    }

    // --- leaves per LOD (the client uses 0, the rest for completeness) ---
    const unsigned uLeafLods = p->GetNumLeafLodLevels();
    z.u16(uLeafLods);
    for (unsigned l = 0; l < uLeafLods; ++l)
    {
        p->GetGeometry(g, SpeedTree_LeafGeometry, -1, -1, (short)l);
        const CSpeedTreeRT::SGeometry::SLeaf& s = g.m_sLeaves0;
        const unsigned uN = s.m_usLeafCount;
        z.u8(s.m_bIsActive ? 1 : 0); z.f(s.m_fAlphaTestValue); z.u16(uN);
        z.fs(s.m_pCenterCoords, 3 * uN);
        z.b(s.m_pColors, 4 * uN);
        z.b(s.m_pLeafMapIndices, uN); z.b(s.m_pLeafClusterIndices, uN);
        for (unsigned i = 0; i < uN; ++i) z.fs(s.m_pLeafMapTexCoords ? s.m_pLeafMapTexCoords[i] : NULL, 8);
    }
    p->GetGeometry(g, SpeedTree_LeafGeometry, -1, -1, 0);

    // --- leaf clusters: canonical table and the rho angle ---
    std::vector<float> t0 = TableAtCamera(p, g, 1, 0, 0);       // az 0, el 0
    std::vector<float> tG = TableAtCamera(p, g, 0, 0, 1);       // el +90
    std::vector<float> tD = TableAtCamera(p, g, 0, 0, -1);      // el -90
    std::vector<float> t45 = TableAtCamera(p, g, 0.5f, 0.5f, 0.70710678f);  // az 45, el 45 - check
    const unsigned uClusters = (unsigned)t0.size() / 16;
    // check: every leaf's card = the table entry of its cluster (by value;
    // the pointers differ - the DLL keeps two copies)
    {
        unsigned n = 0; const float* tab = p->GetLeafBillboardTable(n);
        const CSpeedTreeRT::SGeometry::SLeaf& s = g.m_sLeaves0;
        for (unsigned i = 0; i < s.m_usLeafCount; ++i)
            for (int j = 0; j < 16; ++j)
                if (fabsf(s.m_pLeafMapCoords[i][j] - tab[16 * s.m_pLeafClusterIndices[i] + j]) > 1e-4f)
                { c_szError = "leaf card differs from the cluster table"; delete p; return false; }
    }
    z.u16(uClusters);
    float fMaxError = 0.0f;
    for (unsigned k = 0; k < uClusters; ++k)
    {
        // rotation axis from corner 0: a = (v(90) + v(-90)) / 2, lies in XY
        float a[3];
        for (int i = 0; i < 3; ++i) a[i] = 0.5f * (tG[k * 16 + i] + tD[k * 16 + i]);
        float fLen = sqrtf(a[0] * a[0] + a[1] * a[1]);
        float fRho = 0.0f;
        if (fLen > 1e-3f)
        {
            if (a[1] < 0) { a[0] = -a[0]; a[1] = -a[1]; }
            fRho = atan2f(a[1], a[0]) - 1.57079632679f;
        }
        z.f(fRho);
        float mRzMinus[9]; Rz(-fRho, mRzMinus);
        float L[4][3];
        for (int r = 0; r < 4; ++r) MulV(mRzMinus, &t0[k * 16 + r * 4], L[r]);
        for (int r = 0; r < 4; ++r) z.fs(L[r], 3);
        // check of the formula on the camera az 45 el 45
        const float fAz = atan2f(0.5f, 0.5f), fEl = asinf(0.70710678f);
        float mA[9], mB[9], mM[9]; Rz(fAz + fRho, mA); Ry(-fEl, mB); Mul(mA, mB, mM);
        for (int r = 0; r < 4; ++r)
        {
            float v[3]; MulV(mM, L[r], v);
            for (int i = 0; i < 3; ++i) { float e = fabsf(v[i] - t45[k * 16 + r * 4 + i]); if (e > fMaxError) fMaxError = e; }
        }
    }
    if (fMaxError > 0.01f)
    {
        static char s_ab[128]; snprintf(s_ab, sizeof(s_ab), "leaf card formula disagrees with the DLL (error %.4f)", fMaxError);
        c_szError = s_ab; delete p; return false;
    }

    FILE* w = fopen(c_szOutput, "wb");
    if (!w) { c_szError = "cannot write"; delete p; return false; }
    fwrite(&z.v[0], 1, z.v.size(), w); fclose(w);
    printf("%s: branches %u fronds %u leaves %u clusters %u lod %u/%u/%u -> %u B (card error %.5f)\n", c_szSpt,
           g.m_sBranches.m_usVertexCount, g.m_sFronds.m_usVertexCount, g.m_sLeaves0.m_usLeafCount, uClusters,
           p->GetNumBranchLodLevels(), p->GetNumFrondLodLevels(), uLeafLods, (unsigned)z.v.size(), fMaxError);
    delete p;
    return true;
}

}  // namespace

/// One file (`<file.spt> <output>`) or a whole directory (`--dir <from> <to>`);
/// sets the client light 0 first. Exit 1 on any failed tree, 2 on bad usage.
int main(int argc, char** argv)
{
    if (argc < 3) { printf("bake_spt <file.spt> <output>  |  bake_spt --dir <directory> <output>\n"); return 2; }
    // EXACTLY like CSpeedTreeForestDirectX8::SetRenderingDevice - light 0
    // before computing the trees; the static lighting goes into the vertex
    // colours at Compute, so the layout of this array is part of the
    // bake's content: pos(3) diffuse(3) ambient(3) specular(3) directional flag,
    // attenuation(3). (The first version had a made-up array - the colours
    // came out black.)
    float afLight1[] =
    {
        -0.707f, -0.300f, 0.707f,   // pos (direction)
        1.0f, 1.0f, 1.0f,           // diffuse
        0.5f, 0.5f, 0.5f,           // ambient
        1.0f, 1.0f, 1.0f,           // specular
        0.0f,                       // directional flag
        1.0f, 0.0f, 0.0f            // attenuation
    };
    CSpeedTreeRT::SetNumWindMatrices(4);
    CSpeedTreeRT::SetLightAttributes(0, afLight1);
    CSpeedTreeRT::SetLightState(0, true);

    if (strcmp(argv[1], "--dir") != 0)
    {
        if (!Bake(argv[1], argv[2])) { printf("%s: ERROR: %s\n", argv[1], c_szError ? c_szError : "?"); return 1; }
        return 0;
    }
    if (argc < 4) return 2;
    std::string sFrom = argv[2], sTo = argv[3];
    CreateDirectoryA(sTo.c_str(), NULL);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((sFrom + "\\*.spt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no .spt files in %s\n", sFrom.c_str()); return 1; }
    int iOk = 0, iErrors = 0;
    do
    {
        std::string sIn = sFrom + "\\" + fd.cFileName, sOut = sTo + "\\" + fd.cFileName;
        if (Bake(sIn.c_str(), sOut.c_str())) ++iOk;
        else { ++iErrors; printf("%s: ERROR: %s\n", sIn.c_str(), c_szError ? c_szError : "?"); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("baked %d, errors %d\n", iOk, iErrors);
    return iErrors ? 1 : 0;
}
