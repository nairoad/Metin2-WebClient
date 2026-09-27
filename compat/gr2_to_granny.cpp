// SPDX-License-Identifier: GPL-2.0-or-later
// gr2_to_granny.cpp - one pass over an unpacked `.gr2`: every object the
// client uses is COPIED into its `granny.h` layout, members picked by name.
// See gr2_to_granny.h.

// Design: a converted source object is remembered by address, so that the
// mesh a model points at IS THE SAME object as the mesh in the mesh list -
// TMP4 compares those pointers. Large number runs (indices, knots) are not
// copied but pointed at inside the file's sections.

#include "gr2_to_granny.h"

#include <string.h>

#include <map>

namespace m2wgr2 {

namespace {

/// One pass over the file. Remembers what is already converted, so that
/// the mesh a model points at IS THE SAME object as the mesh in the mesh
/// list - TMP4 compares those pointers.
class CConverter
{
public:
    /// A converter over one unpacked file; the converted objects are allocated
    /// in (and live as long as) that `CFile`.
    explicit CConverter(CFile& rFile) : m_rFile(rFile) {}

    /// Converts the root object into a `granny_file_info`: file name and the
    /// Textures, Materials, Skeletons, Meshes, Models and Animations tables;
    /// NULL when the file has no root.
    granny_file_info* FileInfo();

private:
    /// `uCount` zeroed objects of type `T` from the file's memory (NULL when
    /// allocation fails).
    template <class T>
    T* New(size_t uCount = 1)
    {
        return (T*)m_rFile.Allocate(sizeof(T) * uCount);
    }

    /// Was this source object converted already?
    void* Known(const void* c_pvSource) const
    {
        std::map<const void*, void*>::const_iterator it = m_mapKnown.find(c_pvSource);
        return it == m_mapKnown.end() ? NULL : it->second;
    }

    /// Records that source object `c_pvSource` became `pvTarget` (see `Known`).
    void Remember(const void* c_pvSource, void* pvTarget)
    {
        m_mapKnown[c_pvSource] = pvTarget;
    }

    /// Texture: file name, type, size, encoding, sub-format and pixel layout.
    /// Like every `Convert*`: NULL source -> NULL; a source met before -> the
    /// SAME target; otherwise a new object, remembered before its members are
    /// copied by name.
    granny_texture* ConvertTexture(granny_data_type_definition* pType, void* pvSrc);
    /// Material: name, texture and maps (usage + the map's material, converted
    /// recursively). NULL/met-before as in `ConvertTexture`.
    granny_material* ConvertMaterial(granny_data_type_definition* pType, void* pvSrc);
    /// Skeleton: name and bones (name, parent index, local transform, inverse
    /// world transform). NULL/met-before as in `ConvertTexture`.
    granny_skeleton* ConvertSkeleton(granny_data_type_definition* pType, void* pvSrc);
    /// Vertex data: the vertex array is NOT copied - it points into the file,
    /// with the file's own vertex type (TMP4 reads it by member name).
    /// NULL/met-before as in `ConvertTexture`.
    granny_vertex_data* ConvertVertexData(granny_data_type_definition* pType, void* pvSrc);
    /// Triangle topology: material groups (material, first triangle, count),
    /// 32- or 16-bit indices and the triangle-to-bone table, the runs pointed at
    /// in the file. NULL/met-before as in `ConvertTexture`.
    granny_tri_topology* ConvertTopology(granny_data_type_definition* pType, void* pvSrc);
    /// Mesh: name, primary vertex data and topology, material bindings and bone
    /// bindings (bone name, bounding box, triangle indices). NULL/met-before as
    /// in `ConvertTexture`.
    granny_mesh* ConvertMesh(granny_data_type_definition* pType, void* pvSrc);
    /// Model: name, initial placement, skeleton and mesh bindings.
    /// NULL/met-before as in `ConvertTexture`.
    granny_model* ConvertModel(granny_data_type_definition* pType, void* pvSrc);
    /// Animation: name, duration, time step, oversampling (default 1) and its
    /// track groups. NULL/met-before as in `ConvertTexture`.
    granny_animation* ConvertAnimation(granny_data_type_definition* pType, void* pvSrc);
    /// Track group: name, flags (`AccumulationFlags` in the file), placement,
    /// loop translation, transform tracks (position, orientation and scale-shear
    /// curves) and text tracks. NULL/met-before as in `ConvertTexture`.
    granny_track_group* ConvertTrackGroup(granny_data_type_definition* pType, void* pvSrc);
    void ConvertCurve(granny_data_type_definition* pType, void* pvSrc,
                const char* c_szName, granny_curve2& rTarget);

    CFile& m_rFile;
    std::map<const void*, void*> m_mapKnown;
};

/// Pointer to the run of numbers under an array member - WITHOUT COPYING.
///
/// Triangle indices run to tens of thousands per mesh and lie in memory
/// exactly as `granny.h` expects them: a run of numbers. Copying them would
/// double the largest part of the model for no gain. The ELEMENT SIZE is
/// checked instead - should a file from another exporter have something
/// other than four bytes there, the array stays empty rather than read
/// askew.
void* NumberRun(const granny_data_type_definition* c_pType, void* pvSrc,
                const char* c_szName, uint32_t uElementSize,
                granny_int32* piCount)
{
    void* pvArray = NULL;
    granny_data_type_definition* pElementType = NULL;
    bool bPointers = false;
    const int32_t iCount = MemberArray(c_pType, pvSrc, c_szName, &pvArray,
                                     &pElementType, &bPointers);
    if (iCount <= 0 || bPointers || TypeSize(pElementType) != uElementSize)
    {
        *piCount = 0;
        return NULL;
    }
    *piCount = iCount;
    return pvArray;
}

/// Empties a variant (no type, no object) - how every `ExtendedData` is left.
void ZeroVariant(granny_variant& rVariant)
{
    rVariant.Type = NULL;
    rVariant.Object = NULL;
}

/// A bone transform: 68 bytes of the same layout in the file and in the
/// header (flags, position, orientation, scale and shear).
void CopyTransform(const granny_data_type_definition* c_pType,
                             void* pvSrc, const char* c_szName,
                             granny_transform& rTarget)
{
    memset(&rTarget, 0, sizeof(rTarget));
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, pvSrc, &pby);
    if (c_pMember && c_pMember->Type == GrannyTransformMember)
        memcpy(&rTarget, pby, sizeof(rTarget));
}

granny_texture* CConverter::ConvertTexture(granny_data_type_definition* pType, void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_texture*)pv;

    granny_texture* pTarget = New<granny_texture>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);

    pTarget->FromFileName = MemberString(pType, pvSrc, "FromFileName");
    pTarget->TextureType = MemberInt32(pType, pvSrc, "TextureType");
    pTarget->Width = MemberInt32(pType, pvSrc, "Width");
    pTarget->Height = MemberInt32(pType, pvSrc, "Height");
    pTarget->Encoding = MemberInt32(pType, pvSrc, "Encoding");
    pTarget->SubFormat = MemberInt32(pType, pvSrc, "SubFormat");

    memset(&pTarget->Layout, 0, sizeof(pTarget->Layout));
    granny_data_type_definition* pLayoutType = NULL;
    const uint8_t* pbyLayout = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(pType, "Layout", pvSrc, &pbyLayout);
    if (c_pMember && c_pMember->Type == GrannyInlineMember)
    {
        pLayoutType = c_pMember->ReferenceType;
        pTarget->Layout.BytesPerPixel = MemberInt32(pLayoutType, pbyLayout, "BytesPerPixel");
        // The colour components are arrays of four - with another length
        // in the file zeros stay rather than reading beside them.
        const uint8_t* pbyMember = NULL;
        const granny_data_type_definition* c_pShift =
            FindMember(pLayoutType, "ShiftForComponent", pbyLayout, &pbyMember);
        if (c_pShift && c_pShift->ArrayWidth == 4)
            memcpy(pTarget->Layout.ShiftForComponent, pbyMember, 16);
        c_pShift = FindMember(pLayoutType, "BitsForComponent", pbyLayout, &pbyMember);
        if (c_pShift && c_pShift->ArrayWidth == 4)
            memcpy(pTarget->Layout.BitsForComponent, pbyMember, 16);
    }

    // Images stay empty. In the corpus models point at textures BY FILE
    // (`.dds` beside them) and carry no pixels - `Images` was empty in
    // every file checked. When a file with pixels inside turns up, this
    // branch is the place to copy them.
    pTarget->ImageCount = 0;
    pTarget->Images = NULL;
    ZeroVariant(pTarget->ExtendedData);
    return pTarget;
}

granny_material* CConverter::ConvertMaterial(granny_data_type_definition* pType, void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_material*)pv;

    granny_material* pTarget = New<granny_material>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);

    pTarget->Name = MemberString(pType, pvSrc, "Name");

    granny_data_type_definition* pTextureType = NULL;
    void* pvTexture = MemberReference(pType, pvSrc, "Texture", &pTextureType);
    pTarget->Texture = ConvertTexture(pTextureType, pvTexture);

    void* pvMaps = NULL;
    granny_data_type_definition* pMapType = NULL;
    bool bPointers = false;
    const int32_t iMaps = MemberArray(pType, pvSrc, "Maps", &pvMaps, &pMapType,
                                     &bPointers);
    pTarget->MapCount = 0;
    pTarget->Maps = NULL;
    if (iMaps > 0)
    {
        granny_material_map* pMaps = New<granny_material_map>(iMaps);
        if (pMaps)
        {
            for (int32_t i = 0; i < iMaps; ++i)
            {
                void* pvMap = ArrayElement(pvMaps, pMapType, bPointers, i);
                pMaps[i].Usage = MemberString(pMapType, pvMap, "Usage");
                // In the file the member is called `Map`, in the header `Material`.
                granny_data_type_definition* pTargetType = NULL;
                void* pvTargetMap = MemberReference(pMapType, pvMap, "Map", &pTargetType);
                if (!pvTargetMap)
                    pvTargetMap = MemberReference(pMapType, pvMap, "Material", &pTargetType);
                pMaps[i].Material = ConvertMaterial(pTargetType, pvTargetMap);
            }
            pTarget->MapCount = iMaps;
            pTarget->Maps = pMaps;
        }
    }
    ZeroVariant(pTarget->ExtendedData);
    return pTarget;
}

granny_skeleton* CConverter::ConvertSkeleton(granny_data_type_definition* pType, void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_skeleton*)pv;

    granny_skeleton* pTarget = New<granny_skeleton>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);

    pTarget->Name = MemberString(pType, pvSrc, "Name");
    pTarget->LODType = 0;
    ZeroVariant(pTarget->ExtendedData);

    void* pvBones = NULL;
    granny_data_type_definition* pBoneType = NULL;
    bool bPointers = false;
    const int32_t iBones = MemberArray(pType, pvSrc, "Bones", &pvBones, &pBoneType,
                                       &bPointers);
    pTarget->BoneCount = 0;
    pTarget->Bones = NULL;
    if (iBones <= 0)
        return pTarget;

    granny_bone* pBones = New<granny_bone>(iBones);
    if (!pBones)
        return pTarget;
    for (int32_t i = 0; i < iBones; ++i)
    {
        void* pvBone = ArrayElement(pvBones, pBoneType, bPointers, i);
        pBones[i].Name = MemberString(pBoneType, pvBone, "Name");
        pBones[i].ParentIndex = MemberInt32(pBoneType, pvBone, "ParentIndex", -1);
        CopyTransform(pBoneType, pvBone, "Transform",
                                pBones[i].LocalTransform);
        memset(&pBones[i].InverseWorld4x4, 0, sizeof(pBones[i].InverseWorld4x4));
        MemberReal32Array(pBoneType, pvBone, "InverseWorldTransform",
                          &pBones[i].InverseWorld4x4[0][0], 16);
        pBones[i].LODError = 0.0f;
        ZeroVariant(pBones[i].ExtendedData);
    }
    pTarget->BoneCount = iBones;
    pTarget->Bones = pBones;
    return pTarget;
}

granny_vertex_data* CConverter::ConvertVertexData(
    granny_data_type_definition* pType, void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_vertex_data*)pv;

    granny_vertex_data* pTarget = New<granny_vertex_data>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);

    // Vertices are an array whose TYPE IS STORED BESIDE IT - Granny's whole
    // idea stands on that. The type description stays the one FROM THE
    // FILE, because TMP4 walks it by member name to compose the FVF.
    void* pvVertices = NULL;
    granny_data_type_definition* pVertexType = NULL;
    const int32_t iCount = MemberVariantArray(pType, pvSrc, "Vertices", &pvVertices,
                                            &pVertexType);
    pTarget->VertexType = pVertexType;
    pTarget->VertexCount = iCount;
    pTarget->Vertices = (granny_uint8*)pvVertices;
    pTarget->VertexComponentNameCount = 0;
    pTarget->VertexComponentNames = NULL;
    pTarget->VertexAnnotationSetCount = 0;
    pTarget->VertexAnnotationSets = NULL;
    return pTarget;
}

granny_tri_topology* CConverter::ConvertTopology(granny_data_type_definition* pType,
                                             void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_tri_topology*)pv;

    granny_tri_topology* pTarget = New<granny_tri_topology>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);
    memset(pTarget, 0, sizeof(*pTarget));

    void* pvGroups = NULL;
    granny_data_type_definition* pGroupType = NULL;
    bool bPointers = false;
    const int32_t iGroups = MemberArray(pType, pvSrc, "Groups", &pvGroups, &pGroupType,
                                      &bPointers);
    if (iGroups > 0)
    {
        granny_tri_material_group* pGroups = New<granny_tri_material_group>(iGroups);
        if (pGroups)
        {
            for (int32_t i = 0; i < iGroups; ++i)
            {
                void* pvGroup = ArrayElement(pvGroups, pGroupType, bPointers, i);
                pGroups[i].MaterialIndex = MemberInt32(pGroupType, pvGroup, "MaterialIndex");
                pGroups[i].TriFirst = MemberInt32(pGroupType, pvGroup, "TriFirst");
                pGroups[i].TriCount = MemberInt32(pGroupType, pvGroup, "TriCount");
            }
            pTarget->GroupCount = iGroups;
            pTarget->Groups = pGroups;
        }
    }

    pTarget->Indices = (granny_int32*)NumberRun(pType, pvSrc, "Indices", 4,
                                             &pTarget->IndexCount);
    pTarget->Indices16 = (granny_uint16*)NumberRun(pType, pvSrc, "Indices16", 2,
                                                &pTarget->Index16Count);
    pTarget->BonesForTriangle = (granny_int32*)NumberRun(
        pType, pvSrc, "BonesForTriangle", 4, &pTarget->BonesForTriangleCount);
    pTarget->TriangleToBoneIndices = (granny_int32*)NumberRun(
        pType, pvSrc, "TriangleToBoneIndices", 4, &pTarget->TriangleToBoneCount);
    return pTarget;
}

granny_mesh* CConverter::ConvertMesh(granny_data_type_definition* pType, void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_mesh*)pv;

    granny_mesh* pTarget = New<granny_mesh>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);
    memset(pTarget, 0, sizeof(*pTarget));

    pTarget->Name = MemberString(pType, pvSrc, "Name");

    granny_data_type_definition* pVertexDataType = NULL;
    void* pvVertexData = MemberReference(pType, pvSrc, "PrimaryVertexData", &pVertexDataType);
    pTarget->PrimaryVertexData = ConvertVertexData(pVertexDataType, pvVertexData);

    granny_data_type_definition* pTopologyType = NULL;
    void* pvTopology = MemberReference(pType, pvSrc, "PrimaryTopology", &pTopologyType);
    pTarget->PrimaryTopology = ConvertTopology(pTopologyType, pvTopology);

    void* pvTable = NULL;
    granny_data_type_definition* pElementType = NULL;
    bool bPointers = false;

    const int32_t iMaterials = MemberArray(pType, pvSrc, "MaterialBindings", &pvTable,
                                     &pElementType, &bPointers);
    if (iMaterials > 0)
    {
        granny_material_binding* pBindings = New<granny_material_binding>(iMaterials);
        if (pBindings)
        {
            for (int32_t i = 0; i < iMaterials; ++i)
            {
                void* pvBinding = ArrayElement(pvTable, pElementType, bPointers, i);
                granny_data_type_definition* pMaterialType = NULL;
                void* pvMaterial = MemberReference(pElementType, pvBinding, "Material", &pMaterialType);
                pBindings[i].Material = ConvertMaterial(pMaterialType, pvMaterial);
            }
            pTarget->MaterialBindingCount = iMaterials;
            pTarget->MaterialBindings = pBindings;
        }
    }

    const int32_t iBones = MemberArray(pType, pvSrc, "BoneBindings", &pvTable,
                                       &pElementType, &bPointers);
    if (iBones > 0)
    {
        granny_bone_binding* pBindings = New<granny_bone_binding>(iBones);
        if (pBindings)
        {
            for (int32_t i = 0; i < iBones; ++i)
            {
                void* pvBinding = ArrayElement(pvTable, pElementType, bPointers, i);
                pBindings[i].BoneName = MemberString(pElementType, pvBinding, "BoneName");
                memset(pBindings[i].OBBMin, 0, sizeof(pBindings[i].OBBMin));
                memset(pBindings[i].OBBMax, 0, sizeof(pBindings[i].OBBMax));
                MemberReal32Array(pElementType, pvBinding, "OBBMin", pBindings[i].OBBMin, 3);
                MemberReal32Array(pElementType, pvBinding, "OBBMax", pBindings[i].OBBMax, 3);
                pBindings[i].TriangleIndices = (granny_int32*)NumberRun(
                    pElementType, pvBinding, "TriangleIndices", 4, &pBindings[i].TriangleCount);
            }
            pTarget->BoneBindingCount = iBones;
            pTarget->BoneBindings = pBindings;
        }
    }

    ZeroVariant(pTarget->ExtendedData);
    return pTarget;
}

granny_model* CConverter::ConvertModel(granny_data_type_definition* pType, void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_model*)pv;

    granny_model* pTarget = New<granny_model>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);
    memset(pTarget, 0, sizeof(*pTarget));

    pTarget->Name = MemberString(pType, pvSrc, "Name");
    CopyTransform(pType, pvSrc, "InitialPlacement", pTarget->InitialPlacement);

    granny_data_type_definition* pSkeletonType = NULL;
    void* pvSkeleton = MemberReference(pType, pvSrc, "Skeleton", &pSkeletonType);
    pTarget->Skeleton = ConvertSkeleton(pSkeletonType, pvSkeleton);

    void* pvTable = NULL;
    granny_data_type_definition* pElementType = NULL;
    bool bPointers = false;
    const int32_t iCount = MemberArray(pType, pvSrc, "MeshBindings", &pvTable, &pElementType,
                                     &bPointers);
    if (iCount > 0)
    {
        granny_model_mesh_binding* pBindings =
            New<granny_model_mesh_binding>(iCount);
        if (pBindings)
        {
            for (int32_t i = 0; i < iCount; ++i)
            {
                void* pvBinding = ArrayElement(pvTable, pElementType, bPointers, i);
                granny_data_type_definition* pMeshType = NULL;
                void* pvMesh = MemberReference(pElementType, pvBinding, "Mesh", &pMeshType);
                pBindings[i].Mesh = ConvertMesh(pMeshType, pvMesh);
            }
            pTarget->MeshBindingCount = iCount;
            pTarget->MeshBindings = pBindings;
        }
    }
    ZeroVariant(pTarget->ExtendedData);
    return pTarget;
}

granny_animation* CConverter::ConvertAnimation(granny_data_type_definition* pType,
                                         void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_animation*)pv;

    granny_animation* pTarget = New<granny_animation>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);
    memset(pTarget, 0, sizeof(*pTarget));

    pTarget->Name = MemberString(pType, pvSrc, "Name");
    pTarget->Duration = MemberReal32(pType, pvSrc, "Duration");
    pTarget->TimeStep = MemberReal32(pType, pvSrc, "TimeStep");
    pTarget->Oversampling = MemberReal32(pType, pvSrc, "Oversampling", 1.0f);

    // TRACK GROUPS - an array of POINTERS to groups.
    void* pvTable = NULL;
    granny_data_type_definition* pElementType = NULL;
    bool bPointers = false;
    const int32_t iGroups = MemberArray(pType, pvSrc, "TrackGroups", &pvTable,
                                      &pElementType, &bPointers);
    pTarget->TrackGroupCount = 0;
    pTarget->TrackGroups = NULL;
    if (iGroups > 0)
    {
        granny_track_group** ppGroups = New<granny_track_group*>(iGroups);
        if (ppGroups)
        {
            for (int32_t i = 0; i < iGroups; ++i)
                ppGroups[i] = ConvertTrackGroup(
                    pElementType, ArrayElement(pvTable, pElementType, bPointers, i));
            pTarget->TrackGroupCount = iGroups;
            pTarget->TrackGroups = ppGroups;
        }
    }
    ZeroVariant(pTarget->ExtendedData);
    return pTarget;
}

/// A CURVE IN THE OLD FORMAT - how the WHOLE corpus is written (measured,
/// `tools/animation_review.py`: 36141 curves, all `Degree/Knots/Controls`).
/// `granny.h` describes a curve as `granny_curve2`, a `(type, object)`
/// variant in one of nineteen packings - but the Metin2 files are older
/// than that header. So the variant gets a `granny_old_curve` with knots and
/// controls pointing STRAIGHT INTO THE FILE (no copy) and the type is left
/// empty: the only reader is `granny_control.cpp`, which knows what lies
/// there.
void CConverter::ConvertCurve(granny_data_type_definition* pType, void* pvSrc,
                          const char* c_szName, granny_curve2& rTarget)
{
    ZeroVariant(rTarget.CurveData);
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember = FindMember(pType, c_szName, pvSrc, &pby);
    if (!c_pMember || c_pMember->Type != GrannyInlineMember || !c_pMember->ReferenceType)
        return;
    granny_data_type_definition* pCurveType = c_pMember->ReferenceType;
    void* pvCurve = const_cast<uint8_t*>(pby);

    granny_old_curve* pCurve = New<granny_old_curve>();
    if (!pCurve)
        return;
    memset(pCurve, 0, sizeof(*pCurve));
    pCurve->Degree = MemberInt32(pCurveType, pvCurve, "Degree");
    pCurve->Knots = (granny_real32*)NumberRun(pCurveType, pvCurve, "Knots", 4, &pCurve->KnotCount);
    pCurve->Controls = (granny_real32*)NumberRun(pCurveType, pvCurve, "Controls", 4,
                                             &pCurve->ControlCount);
    rTarget.CurveData.Object = pCurve;
}

granny_track_group* CConverter::ConvertTrackGroup(granny_data_type_definition* pType,
                                               void* pvSrc)
{
    if (!pvSrc)
        return NULL;
    if (void* pv = Known(pvSrc))
        return (granny_track_group*)pv;

    granny_track_group* pTarget = New<granny_track_group>();
    if (!pTarget)
        return NULL;
    Remember(pvSrc, pTarget);
    memset(pTarget, 0, sizeof(*pTarget));

    pTarget->Name = MemberString(pType, pvSrc, "Name");
    // The file calls this member `AccumulationFlags`, the header `Flags`.
    // Same values (`GrannyAccumulationExtracted`, `GrannyTrackGroupIsSorted`).
    pTarget->Flags = MemberInt32(pType, pvSrc, "AccumulationFlags",
                            MemberInt32(pType, pvSrc, "Flags"));
    CopyTransform(pType, pvSrc, "InitialPlacement", pTarget->InitialPlacement);
    MemberReal32Array(pType, pvSrc, "LoopTranslation", pTarget->LoopTranslation, 3);

    void* pvTable = NULL;
    granny_data_type_definition* pElementType = NULL;
    bool bPointers = false;

    const int32_t iTracks = MemberArray(pType, pvSrc, "TransformTracks", &pvTable,
                                         &pElementType, &bPointers);
    if (iTracks > 0)
    {
        granny_transform_track* pTracks = New<granny_transform_track>(iTracks);
        if (pTracks)
        {
            memset(pTracks, 0, sizeof(*pTracks) * (size_t)iTracks);
            for (int32_t i = 0; i < iTracks; ++i)
            {
                void* pvTrack = ArrayElement(pvTable, pElementType, bPointers, i);
                pTracks[i].Name = MemberString(pElementType, pvTrack, "Name");
                pTracks[i].Flags = 0;
                ConvertCurve(pElementType, pvTrack, "PositionCurve", pTracks[i].PositionCurve);
                ConvertCurve(pElementType, pvTrack, "OrientationCurve", pTracks[i].OrientationCurve);
                ConvertCurve(pElementType, pvTrack, "ScaleShearCurve", pTracks[i].ScaleShearCurve);
            }
            pTarget->TransformTrackCount = iTracks;
            pTarget->TransformTracks = pTracks;
        }
    }

    // TEXT TRACKS - events in time (hit, step). TMP4 reads them through
    // `CGrannyMotion::GetTextTrack`, straight from these members.
    const int32_t iTextTracks = MemberArray(pType, pvSrc, "TextTracks", &pvTable,
                                            &pElementType, &bPointers);
    if (iTextTracks > 0)
    {
        granny_text_track* pTexts = New<granny_text_track>(iTextTracks);
        if (pTexts)
        {
            memset(pTexts, 0, sizeof(*pTexts) * (size_t)iTextTracks);
            for (int32_t i = 0; i < iTextTracks; ++i)
            {
                void* pvText = ArrayElement(pvTable, pElementType, bPointers, i);
                pTexts[i].Name = MemberString(pElementType, pvText, "Name");
                void* pvEntries = NULL;
                granny_data_type_definition* pEntryType = NULL;
                bool bEntryPointers = false;
                const int32_t iEntries = MemberArray(pElementType, pvText, "Entries",
                                                    &pvEntries, &pEntryType, &bEntryPointers);
                if (iEntries <= 0)
                    continue;
                granny_text_track_entry* pEntries = New<granny_text_track_entry>(iEntries);
                if (!pEntries)
                    continue;
                for (int32_t j = 0; j < iEntries; ++j)
                {
                    void* pvBinding = ArrayElement(pvEntries, pEntryType, bEntryPointers, j);
                    pEntries[j].TimeStamp = MemberReal32(pEntryType, pvBinding, "TimeStamp");
                    pEntries[j].Text = MemberString(pEntryType, pvBinding, "Text");
                }
                pTexts[i].EntryCount = iEntries;
                pTexts[i].Entries = pEntries;
            }
            pTarget->TextTrackCount = iTextTracks;
            pTarget->TextTracks = pTexts;
        }
    }

    ZeroVariant(pTarget->ExtendedData);
    return pTarget;
}

granny_file_info* CConverter::FileInfo()
{
    granny_data_type_definition* pType = m_rFile.RootType();
    void* pvSrc = m_rFile.RootObject();
    if (!pType || !pvSrc)
        return NULL;

    granny_file_info* pTarget = New<granny_file_info>();
    if (!pTarget)
        return NULL;
    memset(pTarget, 0, sizeof(*pTarget));
    pTarget->FromFileName = MemberString(pType, pvSrc, "FromFileName");
    ZeroVariant(pTarget->ExtendedData);

    void* pvTable = NULL;
    granny_data_type_definition* pElementType = NULL;
    bool bPointers = false;

    const int32_t iTextures = MemberArray(pType, pvSrc, "Textures", &pvTable, &pElementType,
                                         &bPointers);
    if (iTextures > 0)
    {
        granny_texture** ppTextures = New<granny_texture*>(iTextures);
        for (int32_t i = 0; ppTextures && i < iTextures; ++i)
            ppTextures[i] = ConvertTexture(
                pElementType, ArrayElement(pvTable, pElementType, bPointers, i));
        pTarget->TextureCount = ppTextures ? iTextures : 0;
        pTarget->Textures = ppTextures;
    }

    const int32_t iMaterials = MemberArray(pType, pvSrc, "Materials", &pvTable,
                                            &pElementType, &bPointers);
    if (iMaterials > 0)
    {
        granny_material** ppMaterials = New<granny_material*>(iMaterials);
        for (int32_t i = 0; ppMaterials && i < iMaterials; ++i)
            ppMaterials[i] = ConvertMaterial(
                pElementType, ArrayElement(pvTable, pElementType, bPointers, i));
        pTarget->MaterialCount = ppMaterials ? iMaterials : 0;
        pTarget->Materials = ppMaterials;
    }

    const int32_t iSkeletons = MemberArray(pType, pvSrc, "Skeletons", &pvTable,
                                            &pElementType, &bPointers);
    if (iSkeletons > 0)
    {
        granny_skeleton** ppSkeletons = New<granny_skeleton*>(iSkeletons);
        for (int32_t i = 0; ppSkeletons && i < iSkeletons; ++i)
            ppSkeletons[i] = ConvertSkeleton(
                pElementType, ArrayElement(pvTable, pElementType, bPointers, i));
        pTarget->SkeletonCount = ppSkeletons ? iSkeletons : 0;
        pTarget->Skeletons = ppSkeletons;
    }

    const int32_t iMeshes = MemberArray(pType, pvSrc, "Meshes", &pvTable, &pElementType,
                                        &bPointers);
    if (iMeshes > 0)
    {
        granny_mesh** ppMeshes = New<granny_mesh*>(iMeshes);
        for (int32_t i = 0; ppMeshes && i < iMeshes; ++i)
            ppMeshes[i] = ConvertMesh(
                pElementType, ArrayElement(pvTable, pElementType, bPointers, i));
        pTarget->MeshCount = ppMeshes ? iMeshes : 0;
        pTarget->Meshes = ppMeshes;
    }

    const int32_t iModels = MemberArray(pType, pvSrc, "Models", &pvTable, &pElementType,
                                        &bPointers);
    if (iModels > 0)
    {
        granny_model** ppModels = New<granny_model*>(iModels);
        for (int32_t i = 0; ppModels && i < iModels; ++i)
            ppModels[i] = ConvertModel(
                pElementType, ArrayElement(pvTable, pElementType, bPointers, i));
        pTarget->ModelCount = ppModels ? iModels : 0;
        pTarget->Models = ppModels;
    }

    const int32_t iAnimations = MemberArray(pType, pvSrc, "Animations", &pvTable,
                                          &pElementType, &bPointers);
    if (iAnimations > 0)
    {
        granny_animation** ppAnimations = New<granny_animation*>(iAnimations);
        for (int32_t i = 0; ppAnimations && i < iAnimations; ++i)
            ppAnimations[i] = ConvertAnimation(
                pElementType, ArrayElement(pvTable, pElementType, bPointers, i));
        pTarget->AnimationCount = ppAnimations ? iAnimations : 0;
        pTarget->Animations = ppAnimations;
    }

    return pTarget;
}

}  // namespace

granny_file_info* BuildFileInfo(CFile& rFile)
{
    CConverter kConverter(rFile);
    return kConverter.FileInfo();
}

int32_t TypeEntryCount(const granny_data_type_definition* c_pType)
{
    if (!c_pType)
        return 0;
    int32_t i = 0;
    while (c_pType[i].Type != GrannyEndMember)
        ++i;
    return i + 1;   // with the closing entry
}

}  // namespace m2wgr2
