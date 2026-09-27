// SPDX-License-Identifier: GPL-2.0-or-later
// SpeedTreeRT.h - OUR declaration of the `CSpeedTreeRT` interface:
// the contract between Ymir's `SpeedTreeLib` and our reader of baked trees
// (speedtree_web.cpp), written from USE alone - no SDK file is needed to
// build the client.

// Design:
// Earlier this file was a 16-line `#include_next` shim over the
// SpeedTree RT 1.6 SDK header (IDV, under NDA, lying in `reference/`). The
// web client needed it only so that Ymir's code
// (`SpeedTreeLib/SpeedTreeWrapper.cpp`, `SpeedTreeForest*.cpp`) and our
// reader talk about the same class. This file is that contract rewritten:
//   * every method below is called by Ymir's code in `stage/SpeedTreeLib`
//     or defined in `speedtree_web.cpp` - nothing beyond that;
//   * the fields of `SGeometry`/`STextures` are the ones the Ymir wrapper
//     reaches for (`pBranches->m_pCoords[i * 3]`, `pLeaf->m_pLeafMapTexCoords`
//     ...) - the names are part of the interface, the layout is OURS (both
//     sides compile with this file, so it only has to agree with itself);
//   * the private fields are ours (`CTreeEngine` is the baked mesh, see
//     `speedtree_web.cpp`) and have nothing to do with the SDK layout.
// The SDK (header, `.lib`, `.dll` from the Windows client) is needed ONLY
// by the baker `tools/speedtree_bake/bake_spt.cpp`, built separately
// (`build/port/bake/build.bat`) against the SDK header in `reference/`.
// Enum values (`WIND_NONE` = 0 etc.) are arbitrary - only the agreement
// between the wrapper and the reader matters, both on our side.

#pragma once

#include <cstddef>

/// `GetGeometry` masks - Ymir's code ORs them.
enum
{
    SpeedTree_BranchGeometry    = 1 << 0,
    SpeedTree_FrondGeometry     = 1 << 1,
    SpeedTree_LeafGeometry      = 1 << 2,
    SpeedTree_BillboardGeometry = 1 << 3,
    SpeedTree_AllGeometry       = 0xF
};

class CTreeEngine;          // the baked mesh (speedtree_web.cpp)
struct STreeInstanceData;   // the position of one instance (speedtree_web.cpp)

/// The SpeedTree RT 1.6 engine as Ymir's wrapper uses it: a baked mesh
/// shared by the instances (`MakeInstance`), one position per instance,
/// LOD always 1.0, no wind. See speedtree_web.cpp.
class CSpeedTreeRT
{
public:
    enum EWindMethod            { WIND_NONE, WIND_CPU, WIND_GPU };
    enum ELightingMethod        { LIGHT_STATIC, LIGHT_DYNAMIC };
    enum EStaticLightingStyle   { SLS_BASIC, SLS_USE_LIGHT_SOURCES, SLS_SIMULATE_SHADOWS };
    enum ECollisionObjectType   { CO_SPHERE, CO_CYLINDER, CO_BOX };

    /// Geometry handed to the wrapper. The pointers point INTO
    /// `CTreeEngine` - valid as long as the source tree lives.
    struct SGeometry
    {
        /// Branches or fronds: indexed triangle strips.
        struct SIndexed
        {
            /// All zero, LOD level -1 (nothing to draw).
            SIndexed();
            int                     m_nDiscreteLodLevel;
            unsigned short          m_usVertexCount;
            const float*            m_pCoords;          // 3 per vertex
            const unsigned long*    m_pColors;          // BGRA per vertex
            const float*            m_pTexCoords0;      // 2 per vertex
            const float*            m_pTexCoords1;      // 2 per vertex (self-shadow)
            const float*            m_pNormals;
            const float*            m_pBinormals;
            const float*            m_pTangents;
            const float*            m_pWindWeights;
            const unsigned char*    m_pWindMatrixIndices;
            unsigned short          m_usNumStrips;
            const unsigned short*   m_pStripLengths;
            const unsigned short**  m_pStrips;
        };
        /// Leaves: camera-facing cards, one per leaf.
        struct SLeaf
        {
            /// All zero, LOD level -1 (inactive).
            SLeaf();
            bool                    m_bIsActive;
            int                     m_nDiscreteLodLevel;
            float                   m_fAlphaTestValue;
            unsigned short          m_usLeafCount;
            const unsigned char*    m_pLeafMapIndices;
            const unsigned char*    m_pLeafClusterIndices;
            const float*            m_pCenterCoords;    // 3 per leaf
            const float**           m_pLeafMapTexCoords;// 8 per leaf
            const float**           m_pLeafMapCoords;   // 16 per leaf (4 corners x,y,z,0)
            const unsigned long*    m_pColors;
            const float*            m_pNormals;
            const float*            m_pBinormals;
            const float*            m_pTangents;
            const float*            m_pWindWeights;
            const unsigned char*    m_pWindMatrixIndices;
        };
        /// Billboards: never active at LOD 1.0.
        struct SBillboard
        {
            /// All zero (inactive).
            SBillboard();
            bool                    m_bIsActive;
            const float*            m_pCoords;
            const float*            m_pTexCoords;
            float                   m_fAlphaTestValue;
        };

        SIndexed    m_sBranches;
        float       m_fBranchAlphaTestValue;
        SIndexed    m_sFronds;
        float       m_fFrondAlphaTestValue;
        SLeaf       m_sLeaves0;
        SLeaf       m_sLeaves1;
        SBillboard  m_sBillboard0;
        SBillboard  m_sBillboard1;
        SBillboard  m_sHorizontalBillboard;
    };

    /// Texture file names from the bake (pointers into `CTreeEngine`).
    struct STextures
    {
        /// All zero (no names).
        STextures();
        const char*         m_pBranchTextureFilename;
        unsigned int        m_uiLeafTextureCount;
        const char**        m_pLeafTextureFilenames;
        unsigned int        m_uiFrondTextureCount;
        const char**        m_pFrondTextureFilenames;
        const char*         m_pCompositeFilename;
        const char*         m_pSelfShadowFilename;
    };

    /// A tree with no mesh yet and its own zeroed instance position.
    CSpeedTreeRT();
    /// Drops this tree's reference to the shared mesh (deleting it at zero) and
    /// frees the instance position.
    ~CSpeedTreeRT();

    // loading and instances
    bool                LoadTree(const char* c_szName);
    bool                LoadTree(const unsigned char* c_pBlock, unsigned int uBytes);
    bool                Compute(const float* c_pafTransform, unsigned int uSeed = 1, bool bCompositeStrips = true);
    /// A new tree sharing this one's mesh, with its own position; NULL when no
    /// mesh is loaded.
    CSpeedTreeRT*       MakeInstance();
    /// Not supported - always NULL (the wrapper uses `MakeInstance`).
    CSpeedTreeRT*       Clone(float x, float y, float z, unsigned int uSeed) const;
    /// Always NULL - an instance does not remember its source.
    const CSpeedTreeRT* InstanceOf() const;
    /// Does nothing - the bake keeps no transient data.
    void                DeleteTransientData();
    /// The last error text set by loading (empty when none).
    static const char*  GetCurrentError();
    /// Always "" - the bake has no user data.
    const char*         GetUserData() const;

    // size, position, bounds
    /// Stores the size and variance in the shared mesh (book-keeping only - the
    /// bake is already sized).
    void                SetTreeSize(float fSize, float fVariance);
    /// The stored size and variance (0, 0 without a mesh).
    void                GetTreeSize(float& fSize, float& fVariance) const;
    /// Always 1.
    unsigned int        GetSeed() const;
    /// Sets this instance's position.
    void                SetTreePosition(float x, float y, float z);
    /// This instance's position (three floats).
    const float*        GetTreePosition() const;
    /// The baked bounding box, six floats (zeros without a mesh; NULL ignored).
    void                GetBoundingBox(float* pafBounds) const;
    /// Number of baked collision objects (0 without a mesh).
    unsigned int        GetCollisionObjectCount();
    /// Collision object `uIndex`: type, position and size; an unknown index gives
    /// a zero-size sphere at the origin.
    void                GetCollisionObject(unsigned int uIndex, ECollisionObjectType& eType,
                                           float* pafPosition, float* pafSize);

    // levels of detail
    /// Ignored - the port draws LOD 1.0 always.
    void                SetLodLevel(float f);
    /// Always 1.0.
    float               GetLodLevel() const;
    /// Ignored.
    void                SetLodLimits(float fNear, float fFar);
    /// Always 0, 0.
    void                GetLodLimits(float& fNear, float& fFar) const;
    /// Does nothing (LOD is fixed).
    void                ComputeLodLevel();
    /// Always 0 - the first leaf LOD.
    unsigned short      GetDiscreteLeafLodLevel(float f) const;
    /// Number of baked branch LODs (0 without a mesh).
    unsigned short      GetNumBranchLodLevels() const;
    /// Number of baked frond LODs (0 without a mesh).
    unsigned short      GetNumFrondLodLevels() const;
    /// Number of baked leaf LODs (0 without a mesh).
    unsigned short      GetNumLeafLodLevels() const;
    /// Per-leaf-LOD size scales from the bake (zeros without a mesh).
    const float*        GetLeafLodSizeAdjustments();
    /// Two triangles per leaf card of the first LOD (`f` ignored).
    unsigned int        GetLeafTriangleCount(float f) const;
    /// Triangles of the first branch strip of LOD 0 (length - 2; `f` ignored).
    unsigned int        GetBranchTriangleCount(float f) const;
    /// Triangles of the first frond strip of LOD 0 (length - 2; `f` ignored).
    unsigned int        GetFrondTriangleCount(float f) const;

    // wind (here: accepted, not computed)
    /// Ignored - no wind.
    static void         SetNumWindMatrices(unsigned int u);
    /// Ignored - no wind.
    static void         SetWindMatrix(unsigned int u, const float* pafMatrix);
    /// Ignored - no wind animation.
    static void         SetTime(float f);
    /// Ignored.
    void                SetLocalMatrices(unsigned int uFirst, unsigned int uCount);
    /// Always 0 and 4.
    void                GetLocalMatrices(unsigned int& uFirst, unsigned int& uCount);
    /// Ignored.
    void                SetLeafRockingState(bool b);
    /// Always false.
    bool                GetLeafRockingState() const;
    /// Ignored.
    void                SetNumLeafRockingGroups(unsigned int u);
    /// Does nothing.
    void                ResetLeafWindState();
    /// Does nothing - no wind.
    void                ComputeWindEffects(bool bBranches, bool bLeaves, bool bFronds);
    /// Always 0.
    float               GetWindStrength() const;
    /// Returns `fNew` and stores nothing.
    float               SetWindStrength(float fNew, float fBranches = -1.0f, float fLeaves = -1.0f);
    /// Ignored.
    void                SetLeafWindMethod(EWindMethod e);
    /// Ignored.
    void                SetBranchWindMethod(EWindMethod e);
    /// Ignored.
    void                SetFrondWindMethod(EWindMethod e);
    /// Always WIND_NONE.
    EWindMethod         GetLeafWindMethod() const;
    /// Always WIND_NONE.
    EWindMethod         GetBranchWindMethod() const;
    /// Always WIND_NONE.
    EWindMethod         GetFrondWindMethod() const;

    // lighting and materials (13 numbers: 3x4 colours + shininess, as baked)
    /// Ignored - lighting is done by the GL layer.
    static void         SetLightState(unsigned int u, bool b);
    /// Ignored.
    static void         SetLightAttributes(unsigned int u, const float* paf);
    /// Ignored.
    void                SetBranchLightingMethod(ELightingMethod e);
    /// Ignored.
    void                SetLeafLightingMethod(ELightingMethod e);
    /// Ignored.
    void                SetFrondLightingMethod(ELightingMethod e);
    /// Always LIGHT_STATIC.
    ELightingMethod     GetBranchLightingMethod() const;
    /// Always LIGHT_STATIC.
    ELightingMethod     GetLeafLightingMethod() const;
    /// Always LIGHT_STATIC.
    ELightingMethod     GetFrondLightingMethod() const;
    /// Ignored.
    void                SetStaticLightingStyle(EStaticLightingStyle e);
    /// Always SLS_BASIC.
    EStaticLightingStyle GetStaticLightingStyle() const;
    /// The baked leaf lighting adjustment (0 without a mesh).
    float               GetLeafLightingAdjustment() const;
    /// Overwrites the leaf lighting adjustment in the shared mesh.
    void                SetLeafLightingAdjustment(float f);
    /// The branch material, 13 floats (3x4 colours + shininess; zeros without a
    /// mesh).
    const float*        GetBranchMaterial() const;
    /// The leaf material, 13 floats (zeros without a mesh).
    const float*        GetLeafMaterial() const;
    /// The frond material, 13 floats (zeros without a mesh).
    const float*        GetFrondMaterial() const;
    /// Copies 13 floats into the shared branch material (NULL ignored).
    void                SetBranchMaterial(const float* paf);
    /// Copies 13 floats into the shared leaf material (NULL ignored).
    void                SetLeafMaterial(const float* paf);
    /// Copies 13 floats into the shared frond material (NULL ignored).
    void                SetFrondMaterial(const float* paf);

    // camera, billboards, textures, geometry
    static void         SetCamera(const float* c_pafEye, const float* c_pafDirection);
    /// Stores the flag (read by nobody in the port).
    static void         SetTextureFlip(bool b);
    /// Stores the flag (read by nobody in the port).
    static void         SetDropToBillboard(bool b);
    /// Texture file names from the bake (branch, leaves, fronds, composite,
    /// self-shadow); all zero without a mesh.
    void                GetTextures(STextures& s) const;
    /// The leaf-card corner table for the current camera (recomputed when the
    /// camera changed) and its size; NULL and 0 without one.
    const float*        GetLeafBillboardTable(unsigned int& uEntries) const;
    void                GetGeometry(SGeometry& g, unsigned long ulMask = SpeedTree_AllGeometry,
                                    short sBranchLod = -1, short sFrondLod = -1, short sLeafLod = -1);

private:
    CSpeedTreeRT(const CSpeedTreeRT* c_pSource);
    /// Not copyable (declared, never defined) - instances come from `MakeInstance`.
    CSpeedTreeRT(const CSpeedTreeRT&);
    /// Not assignable (declared, never defined).
    CSpeedTreeRT& operator=(const CSpeedTreeRT&);

    CTreeEngine*        m_pEngine;          // the baked mesh (shared between instances)
    STreeInstanceData*  m_pInstanceData;    // the position of this instance
    bool                m_bTreeComputed;

    static bool         m_bTextureFlip;
    static bool         m_bDropToBillboard;
};
