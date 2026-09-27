// SPDX-License-Identifier: GPL-2.0-or-later
// granny_web.cpp - the Granny file API on our `.gr2` reader: loading
// (`GrannyReadEntireFileFromMemory`), access to meshes, materials, textures
// and bones, copying vertices and indices into the layout the caller asks
// for. Poses, mesh bindings and deformation are in granny_pose.cpp,
// animation control in granny_control.cpp.

// Design:
// THE RULE CHANGE - the same as for SpeedTree. Earlier the
// rule here was: "a Granny function may be added to this file ONLY when its
// absence changes no data; everything that reads a `.gr2`, computes poses,
// binds meshes or deforms vertices stays a stub - until the day a real
// reader exists". The reason was countable: a `GrannyGetMeshVertices` that
// "politely" returns zero vertices gives a world without characters and NOT
// ONE MESSAGE. Measurement showed the rule cost not "a world without
// characters" but THE WHOLE WORLD:
//     no .gr2 reader
//       -> LoadMotionData(.msa) ERROR   (for every animation separately)
//         -> SetMotionRandomWeight finds no motion 1
//           -> LoadGameData stops at step 50
//             -> loading window: "The file is damaged. Please install new."
// The client does not enter the world AT ALL, so neither the terrain nor
// the network nor the game interface - the whole rest of the port, which
// has nothing to do with Granny - can be checked. The new rule keeps both
// reasons, as with SpeedTree: every function has a body, so the client
// runs - but the ones still missing REPORT ONCE, BY NAME (`M2W_STUB`). The
// absence is not silent.
//
// WHAT THEY RETURN: null pointers, zero counts - the truth about "there is
// no model". The caller checks those results, because Granny could fail
// for real too. ONE DELIBERATE EXCEPTION lives in granny_pose.cpp:
// `GrannyGetWorldPose4x4` and `GrannyGetWorldPoseComposite4x4` return an
// IDENTITY MATRIX, not NULL - the caller reads sixteen numbers out of them
// and multiplies vertices by them, so a null pointer would turn a missing
// model into a crash. Identity means "a bone without a transform" and is
// the only value that invents nothing.
//
// STATE: the `.gr2` reader exists and loading
// here is REAL; the remaining loud stubs are `GrannySetLogCallback`,
// `GrannyConvertSingleObject`, `GrannyFindMatchingMember` and
// `GrannyFreeFileSection` (which deliberately frees nothing - see there).
//
// A LOADED FILE: `granny_file` is an EXPOSED struct in `granny.h`, so the
// address of a real `granny_file` is returned - and our own things are
// kept RIGHT BEHIND IT (`TLoadedFile`). TMP4 does not read the fields of
// that struct, it only passes the pointer on, but should it ever start, it
// gets a struct, not somebody else's object.

#include <cstdio>
#include <vector>
#include "stubs.h"

#include <cstring>

#include "win32_compat.h"
#include <granny.h>

#include "gr2_to_granny.h"
#include "gr2_file.h"
#include "frame_stats.h"

namespace
{

/// A loaded `.gr2` file; `kPublic` MUST be first - that address is returned.
struct TLoadedFile
{
    granny_file kPublic;
    m2wgr2::CFile* pFile;
    granny_file_info* pInfo;
};

/// The opaque `granny_file*` handed out by `GrannyReadEntireFileFromMemory`
/// is really our `TLoadedFile*`; this is the one place that casts it back.
TLoadedFile* OurFile(granny_file* pFile)
{
    return reinterpret_cast<TLoadedFile*>(pFile);
}

/// Does the vertex layout carry bone weights? A mesh without them is RIGID
/// - drawn directly, without deformation by the pose.
bool HasBoneWeights(const granny_data_type_definition* c_pType)
{
    if (!c_pType)
        return false;
    for (const granny_data_type_definition* p = c_pType;
         p->Type != GrannyEndMember; ++p)
    {
        if (p->Name && std::strstr(p->Name, "BoneWeights"))
            return true;
    }
    return false;
}

}  // namespace

extern "C" {

/// The one function that was here earlier. Reads and creates no
/// data - tells the library where to print ITS OWN messages. There is no
/// library, so there is nothing to redirect.
void GrannySetLogCallback(granny_log_callback const *)
{
    M2W_STUB("Granny::SetLogCallback");
}

/// Not written: the reader does its own conversion by name (gr2_to_granny).
void GrannyConvertSingleObject(granny_data_type_definition const *, void const *, granny_data_type_definition const *, void *, granny_conversion_handler *)
{
    M2W_STUB("Granny::GrannyConvertSingleObject");
}

/// Copies the triangle indices into the card's buffer at the requested
/// width. The file may hold them sixteen- or thirty-two-bit and the card
/// wants the one width the caller asks for - so both conversions exist.
void GrannyCopyMeshIndices(granny_mesh const * c_pMesh,
                           granny_int32x iBytesPerIndex, void * pvTarget)
{
    if (!c_pMesh || !c_pMesh->PrimaryTopology || !pvTarget)
        return;
    const granny_tri_topology* c_pTopology = c_pMesh->PrimaryTopology;

    if (c_pTopology->IndexCount && c_pTopology->Indices)
    {
        if (iBytesPerIndex == 4)
        {
            std::memcpy(pvTarget, c_pTopology->Indices,
                        (size_t)c_pTopology->IndexCount * 4);
        }
        else if (iBytesPerIndex == 2)
        {
            granny_uint16* pTarget = (granny_uint16*)pvTarget;
            for (granny_int32 i = 0; i < c_pTopology->IndexCount; ++i)
                pTarget[i] = (granny_uint16)c_pTopology->Indices[i];
        }
        return;
    }

    if (c_pTopology->Index16Count && c_pTopology->Indices16)
    {
        if (iBytesPerIndex == 2)
        {
            std::memcpy(pvTarget, c_pTopology->Indices16,
                        (size_t)c_pTopology->Index16Count * 2);
        }
        else if (iBytesPerIndex == 4)
        {
            granny_int32* pTarget = (granny_int32*)pvTarget;
            for (granny_int32 i = 0; i < c_pTopology->Index16Count; ++i)
                pTarget[i] = c_pTopology->Indices16[i];
        }
    }
}

/// Copies the vertices into the layout the caller asks for - BY COMPONENT
/// NAME, not by offset.
///
/// This is the whole point of a format that carries a type description
/// with its data. A vertex in the file may be `Position BoneWeights
/// BoneIndices Normal UV` (40 bytes) while the card wants `Position Normal
/// UV` (32 bytes) - and what goes where is decided by name. A component the
/// file lacks is zeroed: better zero than somebody else's bytes.
void GrannyCopyMeshVertices(granny_mesh const * c_pMesh,
                            granny_data_type_definition const * c_pTargetType,
                            void * pvTarget)
{
    if (!c_pMesh || !c_pMesh->PrimaryVertexData || !c_pTargetType || !pvTarget)
        return;
    const granny_vertex_data* c_pData = c_pMesh->PrimaryVertexData;
    const granny_data_type_definition* c_pSourceType = c_pData->VertexType;
    if (!c_pSourceType || !c_pData->Vertices)
        return;

    const unsigned int uSourceSize = m2wgr2::TypeSize(c_pSourceType);
    const unsigned int uTargetSize = m2wgr2::TypeSize(c_pTargetType);
    if (uSourceSize == 0 || uTargetSize == 0)
        return;

    if (c_pData->VertexCount <= 0 || !c_pData->Vertices)
        return;

    // THE MEMBER MAP IS COMPUTED ONCE, NOT PER VERTEX.
    //
    // Earlier the inner loop did a full `FindMember` by name for
    // EVERY vertex - a `strcmp` over the members of the source type, plus
    // `MemberSize` for every member passed, which for inline members
    // descends recursively into `TypeSize`. For a mesh of 5000 vertices and
    // eight source members that is of the order of **160 000 name
    // comparisons per mesh** - and the result is EXACTLY THE SAME for every
    // vertex, because the type description does not change.
    //
    // Now the map is built once, on the first vertex, and reduced to three
    // numbers per member: from, to, how many. The vertex loop does only
    // `memcpy` and `memset` at offsets. One allocation per mesh instead of
    // hundreds of thousands of `strcmp` - and it is the only allocation
    // added here.
    struct TMemberPlan
    {
        unsigned int uTargetOffset;   ///< offset in the target vertex
        unsigned int uSourceOffset;   ///< offset in the source vertex
        unsigned int uBytes;
        bool         bCopy;           ///< false = zero, no source
    };

    std::vector<TMemberPlan> vecPlan;
    {
        unsigned int uMembers = 0;
        for (const granny_data_type_definition* p = c_pTargetType;
             p->Type != GrannyEndMember; ++p)
            ++uMembers;
        vecPlan.reserve(uMembers);
    }

    const unsigned char* const c_pbyFirst = c_pData->Vertices;
    unsigned int uOffset = 0;
    for (const granny_data_type_definition* p = c_pTargetType;
         p->Type != GrannyEndMember; ++p)
    {
        const unsigned int uBytes = m2wgr2::MemberSize(p);
        const unsigned char* pbyComponent = NULL;
        const granny_data_type_definition* c_pSource =
            p->Name ? m2wgr2::FindMember(c_pSourceType, p->Name,
                                         c_pbyFirst, &pbyComponent)
                    : NULL;

        TMemberPlan kMember;
        kMember.uTargetOffset = uOffset;
        kMember.uBytes = uBytes;
        kMember.bCopy = (c_pSource && c_pSource->Type == p->Type &&
                         m2wgr2::MemberSize(c_pSource) == uBytes &&
                         pbyComponent != NULL);
        kMember.uSourceOffset = kMember.bCopy
                                ? (unsigned int)(pbyComponent - c_pbyFirst) : 0;
        vecPlan.push_back(kMember);

        uOffset += uBytes;
    }

    for (granny_int32 i = 0; i < c_pData->VertexCount; ++i)
    {
        const unsigned char* c_pbySource = c_pData->Vertices + (size_t)i * uSourceSize;
        unsigned char* pbyTarget = (unsigned char*)pvTarget + (size_t)i * uTargetSize;

        for (size_t j = 0; j < vecPlan.size(); ++j)
        {
            const TMemberPlan& c_kMember = vecPlan[j];
            if (c_kMember.bCopy)
                std::memcpy(pbyTarget + c_kMember.uTargetOffset,
                            c_pbySource + c_kMember.uSourceOffset, c_kMember.uBytes);
            else
                std::memset(pbyTarget + c_kMember.uTargetOffset, 0, c_kMember.uBytes);
        }
    }
}

/// Bone index by name, or false.
bool GrannyFindBoneByName(granny_skeleton const * c_pSkeleton,
                          char const * c_szName, granny_int32x * piIndex)
{
    if (!c_pSkeleton || !c_szName || !c_pSkeleton->Bones)
        return false;
    for (granny_int32 i = 0; i < c_pSkeleton->BoneCount; ++i)
    {
        const char* c_szBone = c_pSkeleton->Bones[i].Name;
        if (c_szBone && std::strcmp(c_szBone, c_szName) == 0)
        {
            if (piIndex)
                *piIndex = i;
            return true;
        }
    }
    return false;
}

/// Not written: reports once, returns false.
bool GrannyFindMatchingMember(granny_data_type_definition const *, void const *, char const *, granny_variant *)
{
    M2W_STUB("Granny::GrannyFindMatchingMember");
    return (bool)0;
}


/// Frees the file - the sections AND the converted objects go with it.
void GrannyFreeFile(granny_file * pFile)
{
    if (!pFile)
        return;
    TLoadedFile* pOurs = OurFile(pFile);
    delete pOurs->pFile;
    delete pOurs;
}

/// TMP4 frees the sections with vertices and indices right after copying
/// them into the card's buffers. The original may - we MUST NOT, and that
/// is deliberate: the converted objects (`Indices`, `Vertices`) POINT INTO
/// those sections instead of copying them. Copying would save memory only
/// apparently, because it would first have to be doubled. So this function
/// does nothing and says so once. Real memory saving comes with data
/// streaming.
void GrannyFreeFileSection(granny_file *, granny_int32x)
{
    M2W_STUB("Granny::GrannyFreeFileSection (not freed - objects point into it)");
}


/// The converted `granny_file_info` of a loaded file.
granny_file_info * GrannyGetFileInfo(granny_file * pFile)
{
    return pFile ? OurFile(pFile)->pInfo : (granny_file_info *)0;
}

/// The texture of a material for the given kind.
///
/// A material holds its texture in two ways: directly (`Texture`) or through
/// named references (`Maps`, each with a `Usage`). In the corpus the models
/// use the first, but the second is in the format and files from other
/// servers may use it - so both are handled.
granny_texture * GrannyGetMaterialTextureByType(
    granny_material const * c_pMaterial, granny_material_texture_type eKind)
{
    if (!c_pMaterial)
        return (granny_texture *)0;

    const char* c_szWanted = NULL;
    switch (eKind)
    {
        case GrannyDiffuseColorTexture: c_szWanted = "diffuse"; break;
        case GrannyOpacityTexture: c_szWanted = "opacity"; break;
        case GrannySpecularColorTexture: c_szWanted = "specular"; break;
        case GrannyBumpHeightTexture: c_szWanted = "bump"; break;
        case GrannySelfIlluminationTexture: c_szWanted = "self"; break;
        default: break;
    }

    if (c_szWanted)
    {
        for (granny_int32 i = 0; i < c_pMaterial->MapCount; ++i)
        {
            const granny_material_map& c_rMap = c_pMaterial->Maps[i];
            if (!c_rMap.Usage || !c_rMap.Material)
                continue;
            // Case-insensitive comparison: exporters write "Diffuse Color",
            // "diffuse", "DiffuseColor".
            bool bMatches = false;
            for (const char* c_szU = c_rMap.Usage; *c_szU && !bMatches; ++c_szU)
            {
                const char* a = c_szU;
                const char* b = c_szWanted;
                while (*b && *a &&
                       ((*a | 0x20) == (*b | 0x20)))
                {
                    ++a;
                    ++b;
                }
                bMatches = (*b == 0);
            }
            if (bMatches)
                return c_rMap.Material->Texture;
        }
    }

    // Without references: the material's own texture is its diffuse colour.
    if (eKind == GrannyDiffuseColorTexture)
        return c_pMaterial->Texture;
    return (granny_texture *)0;
}

/// The file carries indices EITHER thirty-two- OR sixteen-bit.
granny_int32x GrannyGetMeshIndexCount(granny_mesh const * c_pMesh)
{
    if (!c_pMesh || !c_pMesh->PrimaryTopology)
        return 0;
    const granny_tri_topology* c_pTopology = c_pMesh->PrimaryTopology;
    return c_pTopology->IndexCount ? c_pTopology->IndexCount : c_pTopology->Index16Count;
}

/// Material groups of the mesh (one draw call each in TMP4).
granny_int32x GrannyGetMeshTriangleGroupCount(granny_mesh const * c_pMesh)
{
    if (!c_pMesh || !c_pMesh->PrimaryTopology)
        return 0;
    return c_pMesh->PrimaryTopology->GroupCount;
}

/// See `GrannyGetMeshTriangleGroupCount`.
granny_tri_material_group * GrannyGetMeshTriangleGroups(granny_mesh const * c_pMesh)
{
    if (!c_pMesh || !c_pMesh->PrimaryTopology)
        return (granny_tri_material_group *)0;
    return c_pMesh->PrimaryTopology->Groups;
}

/// Vertices of the primary vertex data, 0 without one.
granny_int32x GrannyGetMeshVertexCount(granny_mesh const * c_pMesh)
{
    if (!c_pMesh || !c_pMesh->PrimaryVertexData)
        return 0;
    return c_pMesh->PrimaryVertexData->VertexCount;
}

/// The vertex type description FROM THE FILE (TMP4 composes the FVF from it).
granny_data_type_definition * GrannyGetMeshVertexType(granny_mesh const * c_pMesh)
{
    if (!c_pMesh || !c_pMesh->PrimaryVertexData)
        return (granny_data_type_definition *)0;
    return c_pMesh->PrimaryVertexData->VertexType;
}

/// The raw vertices in the file's layout - pointer into the section.
void * GrannyGetMeshVertices(granny_mesh const * c_pMesh)
{
    if (!c_pMesh || !c_pMesh->PrimaryVertexData)
        return (void *)0;
    return c_pMesh->PrimaryVertexData->Vertices;
}

/// The size of the TYPE DESCRIPTION ITSELF in bytes - not of the object.
/// TMP4 divides the result by 32 (the size of one description row) and
/// walks the component names to compose the FVF. The closing row is
/// counted in, because TMP4 skips rows without a name anyway.
granny_int32x GrannyGetTotalTypeSize(granny_data_type_definition const * c_pType)
{
    return m2wgr2::TypeEntryCount(c_pType) * 32;
}

/// A RIGID mesh is one whose vertices carry no bone weights. TMP4 splits
/// the whole drawing path on that: rigid ones go straight into the card's
/// buffer, deformed ones go through the pose.
bool GrannyMeshIsRigid(granny_mesh const * c_pMesh)
{
    if (!c_pMesh || !c_pMesh->PrimaryVertexData)
        return true;
    return !HasBoneWeights(c_pMesh->PrimaryVertexData->VertexType);
}

/// LOADING A `.gr2` FILE - everything else starts here.
///
/// The input buffer is NOT kept: the sections unpack into their own memory
/// and the relocations are written in as real pointers. The caller may
/// therefore free its buffer right after the return - and TMP4 does.
granny_file * GrannyReadEntireFileFromMemory(granny_int32x iSize,
                                             void const* c_pvData)
{
    m2wstats::TStopwatch kStopwatch(m2wstats::g_kGr2Load);
    if (!c_pvData || iSize <= 0)
        return (granny_file *)0;

    TLoadedFile* pFile = new TLoadedFile();
    std::memset(&pFile->kPublic, 0, sizeof(pFile->kPublic));
    pFile->pFile = new m2wgr2::CFile();
    pFile->pInfo = NULL;

    if (!pFile->pFile->Load((const unsigned char*)c_pvData,
                            (unsigned int)iSize))
    {
        // THE REFUSAL IS LOUD. A silent NULL would look like "empty file"
        // and surface only as a missing character on screen.
        std::printf("m2w granny: .gr2 rejected - %s\n",
                    pFile->pFile->Error());
        delete pFile->pFile;
        delete pFile;
        return (granny_file *)0;
    }

    pFile->pInfo = m2wgr2::BuildFileInfo(*pFile->pFile);
    if (!pFile->pInfo)
    {
        std::printf("m2w granny: .gr2 unpacked, but no file info\n");
        delete pFile->pFile;
        delete pFile;
        return (granny_file *)0;
    }

    // A REPORT THAT ANSWERS ONE QUESTION: did the model really come in, or
    // did it only load. The first file described in full, then only the
    // count every hundred - there are hundreds of models in the game and
    // the log must not become a wall.
    {
        static int s_iLoaded = 0;
        ++s_iLoaded;
        const granny_file_info* c_pInfo = pFile->pInfo;
        if (s_iLoaded == 1 || (s_iLoaded % 100) == 0)
        {
            const char* c_szTexture = "(none)";
            if (c_pInfo->TextureCount > 0 && c_pInfo->Textures[0] &&
                c_pInfo->Textures[0]->FromFileName)
                c_szTexture = c_pInfo->Textures[0]->FromFileName;
            int iVertices = 0;
            for (granny_int32 i = 0; i < c_pInfo->MeshCount; ++i)
                if (c_pInfo->Meshes[i] && c_pInfo->Meshes[i]->PrimaryVertexData)
                    iVertices += c_pInfo->Meshes[i]->PrimaryVertexData->VertexCount;
            std::printf("m2w granny: file %d - models %d, meshes %d, "
                        "vertices %d, bones %d, animations %d, texture %s\n",
                        s_iLoaded, (int)c_pInfo->ModelCount,
                        (int)c_pInfo->MeshCount, iVertices,
                        (c_pInfo->SkeletonCount > 0 && c_pInfo->Skeletons[0])
                            ? (int)c_pInfo->Skeletons[0]->BoneCount : 0,
                        (int)c_pInfo->AnimationCount, c_szTexture);
        }
    }
    return &pFile->kPublic;
}



}  // extern "C"
