// SPDX-License-Identifier: GPL-2.0-or-later
// granny_pose.cpp - poses, mesh binding and vertex deformation: what happens
// EVERY FRAME for every character (granny_web.cpp is what happens once, at
// load; curve sampling and controls are in granny_control.cpp).

// Design:
// THE MATRIX CONVENTION - easy to get wrong, so written down. Granny and
// Direct3D multiply VECTOR BY MATRIX (`v * M`), not the other way round.
// Consequences to keep in mind:
//   - the translation sits in the FOURTH ROW, not the fourth column;
//   - a child composes as `local * parent`, not `parent * local`;
//   - the rotation matrix from a quaternion is the transpose of the
//     "column" formulas found on the net.
// A mistake in any of these gives a character tipped over or torn apart,
// not a message.
//
// MOTION PATHS. Earlier `GrannySampleModelAnimations`
// composed the REST pose and the character stood still. Curve sampling and
// controls are in `granny_control.cpp`; here remain the world pose and the
// deformation.
//
// `?skinshortcut=0`: switches the full-weight
// shortcut in `GrannyDeformVertices` off, so that both roads can be
// MEASURED in the same machine state with one page reload (two
// separate runs had different loop regimes, 51% vs 99% busy, so absolute
// times said nothing about the code; the spread of the measurement itself
// on three runs of one binary was 407.1 / 417.0 / 414.6 ms, i.e. 2.5%).

#include <math.h>
#include <string.h>

#include <vector>
#include <set>
#include <string>
#include <cstdio>

#include <emscripten/emscripten.h>

#include "win32_compat.h"
#include <granny.h>

#include "gr2_file.h"
#include "granny_internal.h"
#include "frame_stats.h"

/// `?skinshortcut=0` -> 0, anything else -> 1.
EM_JS(int, m2w_skin_shortcut, (void), { return m2w.options.get('skinshortcut') === '0' ? 0 : 1; });

namespace
{

// --- matrices ---------------------------------------------------------------

/// Writes the 4x4 identity into `mat`.
void Identity(granny_matrix_4x4 mat)
{
    memset(mat, 0, sizeof(granny_matrix_4x4));
    mat[0][0] = mat[1][1] = mat[2][2] = mat[3][3] = 1.0f;
}

/// `result = a * b` in the row convention: `v * (a * b) == (v * a) * b`.
void Multiply(const float a[4][4], const float b[4][4], float result[4][4])
{
    float t[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] +
                      a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(result, t, sizeof(t));
}

/// A bone transform (flags, position, orientation, scale and shear) to 4x4.
void TransformToMatrix(const granny_transform& c_rTransform, float mat[4][4])
{
    Identity(mat);

    if (c_rTransform.Flags & GrannyHasOrientation)
    {
        const float x = c_rTransform.Orientation[0];
        const float y = c_rTransform.Orientation[1];
        const float z = c_rTransform.Orientation[2];
        const float w = c_rTransform.Orientation[3];
        // ROW form - the transpose of the column formula.
        mat[0][0] = 1.0f - 2.0f * (y * y + z * z);
        mat[0][1] = 2.0f * (x * y + z * w);
        mat[0][2] = 2.0f * (x * z - y * w);
        mat[1][0] = 2.0f * (x * y - z * w);
        mat[1][1] = 1.0f - 2.0f * (x * x + z * z);
        mat[1][2] = 2.0f * (y * z + x * w);
        mat[2][0] = 2.0f * (x * z + y * w);
        mat[2][1] = 2.0f * (y * z - x * w);
        mat[2][2] = 1.0f - 2.0f * (x * x + y * y);
    }

    if (c_rTransform.Flags & GrannyHasScaleShear)
    {
        // Scale and shear goes BEFORE the rotation: `v * S * R`.
        //
        // AND IT IS STORED TRANSPOSED, like the rotation matrix. With a
        // purely diagonal scale this is invisible, because a diagonal
        // matrix is its own transpose - which is exactly why it was hard
        // to notice. It shows only on bones that have SHEAR: in `hair_10_1`
        // the bone `lh_w_01` has the scale
        //     0.9986  0.0529 -0.0081
        //     0.0529  0.9984 -0.0209
        //     0.0081  0.0209 -0.9998
        // i.e. asymmetric, and read as is its whole chain is off by 6.39.
        //
        // SETTLED BY MEASUREMENT, four settings on 3 models with such bones
        // (281 bones in all):
        //     S * R  not transposed:   9 bones wrong, worst 6.3920
        //     S * R  transposed:       0 bones wrong, worst 0.0001
        //     R * S  not transposed:  39 bones wrong, worst 254.25
        //     R * S  transposed:      39 bones wrong, worst 254.25
        float s[4][4];
        Identity(s);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                s[i][j] = c_rTransform.ScaleShear[j][i];
        Multiply(s, mat, mat);
    }

    if (c_rTransform.Flags & GrannyHasPosition)
    {
        mat[3][0] = c_rTransform.Position[0];
        mat[3][1] = c_rTransform.Position[1];
        mat[3][2] = c_rTransform.Position[2];
    }
}

// --- vertex components - computed ONCE, when the deformer is created ---------
// Deformation walks every vertex of every character in every frame. Looking
// components up by name each time would be where it costs most - so the
// offsets are computed once, in `GrannyNewMeshDeformer`.

enum ERole { ROLE_PLAIN = 0, ROLE_POSITION = 1, ROLE_NORMAL = 2 };

struct TComponent
{
    unsigned uOutput;     ///< offset in the output vertex
    int iInput;           ///< offset in the input vertex, -1 when absent
    unsigned uSize;
    int eRole;
};

/// Byte offset of member `c_szName` in the vertex type (its size in
/// `*puSize`), or -1 when the type has no such member.
int ComponentOffset(const granny_data_type_definition* c_pType,
                    const char* c_szName, unsigned* puSize)
{
    if (!c_pType || !c_szName)
        return -1;
    unsigned u = 0;
    for (const granny_data_type_definition* p = c_pType;
         p->Type != GrannyEndMember; ++p)
    {
        const unsigned uBytes = m2wgr2::MemberSize(p);
        if (p->Name && strcmp(p->Name, c_szName) == 0)
        {
            if (puSize)
                *puSize = uBytes;
            return (int)u;
        }
        u += uBytes;
    }
    return -1;
}

/// The `?skinshortcut` switch, read once (see "Design").
bool SkinShortcutEnabled()
{
    static int s_iEnabled = -1;
    if (s_iEnabled < 0)
        s_iEnabled = m2w_skin_shortcut();
    return s_iEnabled != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// The structs `granny.h` leaves OPAQUE - the model instance and the controls
// are in `granny_internal.h` (shared with `granny_control.cpp` since cz.
// 277); poses, bindings and the deformer here.
// ---------------------------------------------------------------------------

GRANNY_NAMESPACE_BEGIN;

/// One transform per bone.
struct local_pose
{
    granny_int32x iBones;
    std::vector<granny_transform> vecTransforms;
};

/// World and composite matrices per bone, FLAT, sixteen floats per bone:
/// `granny_matrix_4x4` is a bare `float[4][4]`, which cannot go into a
/// `std::vector` - an array has neither the constructor nor the destructor
/// the container demands. The memory layout is identical, so the cast to
/// `granny_matrix_4x4*` is honest, not a trick.
struct world_pose
{
    granny_int32x iBones;
    std::vector<float> vecWorld;
    std::vector<float> vecComposite;

    /// Bone `i`'s world matrix (16 floats).
    float* World(granny_int32x i) { return &vecWorld[(size_t)i * 16]; }
    /// Bone `i`'s composite matrix, inverse bind (InverseWorld4x4) * world - what
    /// skinning multiplies vertices by (16 floats).
    float* Composite(granny_int32x i) { return &vecComposite[(size_t)i * 16]; }
    /// Bone `i`'s world matrix, read-only.
    const float* World(granny_int32x i) const { return &vecWorld[(size_t)i * 16]; }
    /// Bone `i`'s composite matrix, read-only.
    const float* Composite(granny_int32x i) const { return &vecComposite[(size_t)i * 16]; }
};

/// Which skeleton bone each mesh binding maps to.
struct mesh_binding
{
    granny_mesh const* c_pMesh;
    /// The skeleton bone number for every mesh binding. ALWAYS at least one
    /// entry: TMP4 reads `*boneIndices` for rigid meshes too, which have no
    /// bindings at all.
    std::vector<granny_int32x> vecToBone;
};

/// Input and output vertex layouts with the component offsets precomputed.
struct mesh_deformer
{
    granny_data_type_definition const* c_pInputType;
    granny_data_type_definition const* c_pOutputType;
    unsigned uInputSize;
    unsigned uOutputSize;
    int iWeightsOffset;   ///< offset of `BoneWeights` in the input, -1 when absent
    int iIndicesOffset;   ///< offset of `BoneIndices`, -1 when absent
    std::vector<TComponent> vecComponents;
};

GRANNY_NAMESPACE_END;

extern "C" {

// ---------------------------------------------------------------------------
// Model instance
// ---------------------------------------------------------------------------

/// A new instance with clock 0, no controls, the initial placement as a
/// matrix.
granny_model_instance* GrannyInstantiateModel(granny_model const* c_pModel)
{
    if (!c_pModel)
        return NULL;
    granny_model_instance* pInstance = new granny_model_instance();
    pInstance->c_pModel = c_pModel;
    pInstance->fClock = 0.0f;
    pInstance->dMeasureWall = 0.0;
    pInstance->fMeasureClock = 0.0f;
    pInstance->uMeasureSamples = 0;
    pInstance->uMeasureClocks = 0;
    pInstance->ulMeasureFrame = 0;
    TransformToMatrix(c_pModel->InitialPlacement, pInstance->matInitialPlacement);
    return pInstance;
}

/// The controls point at the instance - detached BEFORE it is freed.
void GrannyFreeModelInstance(granny_model_instance* pInstance)
{
    M2W_DetachControls(pInstance);
    delete pInstance;
}

/// The skeleton of the instance's model.
granny_skeleton* GrannyGetSourceSkeleton(granny_model_instance const* c_pInstance)
{
    if (!c_pInstance || !c_pInstance->c_pModel)
        return NULL;
    return c_pInstance->c_pModel->Skeleton;
}

/// Sets the instance clock (counted for the clock-rate measurement).
void GrannySetModelClock(granny_model_instance const* c_pInstance, granny_real32 fTime)
{
    if (c_pInstance)
    {
        const_cast<granny_model_instance*>(c_pInstance)->fClock = fTime;
        ++const_cast<granny_model_instance*>(c_pInstance)->uMeasureClocks;
    }
}

/// ROOT MOTION. Earlier the matrix passed through untouched
/// - and the character RAN ON THE SPOT: `CActorInstance::__AccumulationMovement`
/// passes the character's rotation matrix here and reads the translation
/// (`_41, _42, _43`) out of the result as movement in the world. The
/// translation in model space comes from the controls (`M2W_RootMotion`,
/// from the animation's `LoopTranslation`); here it is rotated by the
/// source matrix (row times matrix, as everywhere in Granny) and added to
/// its translation.
void GrannyUpdateModelMatrix(granny_model_instance const* c_pInstance,
                             granny_real32 fSeconds,
                             granny_real32 const* c_pafSource,
                             granny_real32* pafTarget, bool)
{
    if (!c_pafSource || !pafTarget)
        return;
    if (c_pafSource != pafTarget)
        memcpy(pafTarget, c_pafSource, sizeof(float) * 16);

    float afMotion[3];
    if (!M2W_RootMotion(c_pInstance, fSeconds, afMotion))
        return;

    const float* m = c_pafSource;
    for (int k = 0; k < 3; ++k)
        pafTarget[12 + k] += afMotion[0] * m[0 + k] + afMotion[1] * m[4 + k] + afMotion[2] * m[8 + k];
}

// ---------------------------------------------------------------------------
// Poses
// ---------------------------------------------------------------------------

/// A local pose of `iBones` zeroed transforms.
granny_local_pose* GrannyNewLocalPose(granny_int32x iBones)
{
    if (iBones <= 0)
        return NULL;
    granny_local_pose* pPose = new granny_local_pose();
    pPose->iBones = iBones;
    pPose->vecTransforms.resize(iBones);
    memset(&pPose->vecTransforms[0], 0,
           sizeof(granny_transform) * (size_t)iBones);
    return pPose;
}

/// See `GrannyNewLocalPose`.
void GrannyFreeLocalPose(granny_local_pose* pPose)
{
    delete pPose;
}

/// All matrices start as identity.
granny_world_pose* GrannyNewWorldPose(granny_int32x iBones)
{
    if (iBones <= 0)
        return NULL;
    granny_world_pose* pPose = new granny_world_pose();
    pPose->iBones = iBones;
    pPose->vecWorld.assign((size_t)iBones * 16, 0.0f);
    pPose->vecComposite.assign((size_t)iBones * 16, 0.0f);
    for (granny_int32x i = 0; i < iBones; ++i)
    {
        Identity((float(*)[4])pPose->World(i));
        Identity((float(*)[4])pPose->Composite(i));
    }
    return pPose;
}

/// See `GrannyNewWorldPose`.
void GrannyFreeWorldPose(granny_world_pose* pPose)
{
    delete pPose;
}

/// The world matrix of a bone; IDENTITY (not NULL) for a missing pose or
/// bone - the caller reads sixteen numbers out of it and multiplies
/// vertices by them, so a null pointer would turn a missing model into a
/// crash. Identity means "a bone without a transform" and invents nothing.
granny_real32* GrannyGetWorldPose4x4(granny_world_pose const* c_pPose,
                                     granny_int32x iBone)
{
    static granny_matrix_4x4 s_matIdentity = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    if (!c_pPose || iBone < 0 || iBone >= c_pPose->iBones)
        return &s_matIdentity[0][0];
    return const_cast<granny_real32*>(c_pPose->World(iBone));
}

/// The composite (inverse-world * world) matrix of a bone; identity as above.
granny_real32* GrannyGetWorldPoseComposite4x4(granny_world_pose const* c_pPose,
                                              granny_int32x iBone)
{
    static granny_matrix_4x4 s_matIdentity = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    if (!c_pPose || iBone < 0 || iBone >= c_pPose->iBones)
        return &s_matIdentity[0][0];
    return const_cast<granny_real32*>(c_pPose->Composite(iBone));
}

/// All world matrices, contiguous; NULL for an empty pose.
granny_matrix_4x4* GrannyGetWorldPose4x4Array(granny_world_pose const* c_pPose)
{
    if (!c_pPose || c_pPose->vecWorld.empty())
        return NULL;
    return (granny_matrix_4x4*)const_cast<float*>(c_pPose->World(0));
}

/// All composite matrices, contiguous; NULL for an empty pose.
granny_matrix_4x4* GrannyGetWorldPoseComposite4x4Array(
    granny_world_pose const* c_pPose)
{
    if (!c_pPose || c_pPose->vecComposite.empty())
        return NULL;
    return (granny_matrix_4x4*)const_cast<float*>(c_pPose->Composite(0));
}

}  // extern "C" - the internal function has ordinary C++ linkage

granny_transform* M2W_PoseTransforms(granny_local_pose* pPose,
                                     granny_int32x* piBones)
{
    if (!pPose || pPose->vecTransforms.empty())
    {
        if (piBones) *piBones = 0;
        return NULL;
    }
    if (piBones) *piBones = pPose->iBones;
    return &pPose->vecTransforms[0];
}

extern "C" {

/// Composes the pose from the animations playing on the instance
/// (`granny_control.cpp`). A bone without a track gets the rest pose (once
/// ALL of them did, because the controls were stubs).
void GrannySampleModelAnimations(granny_model_instance const* c_pInstance,
                                 granny_int32x iFirstBone,
                                 granny_int32x iBones,
                                 granny_local_pose* pResult)
{
    m2wstats::TStopwatch kStopwatch(m2wstats::g_kPose);
    M2W_SampleControls(c_pInstance, iFirstBone, iBones, pResult);
}

/// World matrices `local * parent` down the chain (the root gets the
/// caller's offset), and composite = inverse-world * world, which the
/// vertices are multiplied by: first from the rest pose into the bone's
/// space, then to where the bone is now.
void GrannyBuildWorldPose(granny_skeleton const* c_pSkeleton,
                          granny_int32x iFirstBone, granny_int32x iBones,
                          granny_local_pose const* c_pLocalPose,
                          granny_real32 const* c_pafOffset,
                          granny_world_pose* pResult)
{
    m2wstats::TStopwatch kStopwatch(m2wstats::g_kPose);

    if (!c_pSkeleton || !c_pSkeleton->Bones || !c_pLocalPose || !pResult)
        return;

    float matOffset[4][4];
    if (c_pafOffset)
        memcpy(matOffset, c_pafOffset, sizeof(matOffset));
    else
        Identity(matOffset);

    for (granny_int32x i = iFirstBone; i < iFirstBone + iBones; ++i)
    {
        if (i < 0 || i >= c_pSkeleton->BoneCount || i >= pResult->iBones ||
            i >= c_pLocalPose->iBones)
            continue;

        float matLocal[4][4];
        TransformToMatrix(c_pLocalPose->vecTransforms[i], matLocal);

        float (*pmatWorld)[4] = (float(*)[4])pResult->World(i);
        const granny_int32 iParent = c_pSkeleton->Bones[i].ParentIndex;
        if (iParent >= 0 && iParent < i && iParent < pResult->iBones)
            Multiply(matLocal, (const float(*)[4])pResult->World(iParent), pmatWorld);
        else
            Multiply(matLocal, matOffset, pmatWorld);

        Multiply(c_pSkeleton->Bones[i].InverseWorld4x4, pmatWorld,
                 (float(*)[4])pResult->Composite(i));
    }

}

/// The "accelerated" form: sample and compose in one pass. TMP4 calls this
/// one and left the other commented out beside it.
///
/// WITHOUT THE MODEL'S INITIAL PLACEMENT - that was a CRUTCH.
/// Earlier the root bone sat on `Model->InitialPlacement`, because
/// without animation the player model's rest pose LAY DOWN: its `Bip01` has
/// an identity local transform and the height and rotation come only with
/// the ANIMATION TRACK (measured in the game: rest `(0 0 0)`, the `wait`
/// animation gives `(2.2 -2.2 91.4)` with a rotation). The crutch added
/// that height a second time - and the character hung a metre above the
/// ground. Granny's `BuildWorldPose` composes ONLY the caller's offset;
/// TMP4 never reads `InitialPlacement`. The same here: no offset =
/// identity. ATTACHED things (weapon, hair) get the parent bone's matrix
/// and that is the start of their chain.
void GrannySampleModelAnimationsAccelerated(granny_model_instance const* c_pInstance,
                                            granny_int32x iBones,
                                            granny_real32 const* c_pafOffset,
                                            granny_local_pose* pScratch,
                                            granny_world_pose* pResult)
{
    if (!c_pInstance || !c_pInstance->c_pModel)
        return;
    GrannySampleModelAnimations(c_pInstance, 0, iBones, pScratch);
    GrannyBuildWorldPose(c_pInstance->c_pModel->Skeleton, 0, iBones, pScratch,
                         c_pafOffset, pResult);
}

// ---------------------------------------------------------------------------
// Binding a mesh to a skeleton
// ---------------------------------------------------------------------------

/// Maps every bone binding of the mesh to a bone of `c_pToSkeleton` by
/// name. THE TABLE IS LONGER THAN NEEDED - ON PURPOSE.
///
/// A vertex holds binding numbers in BYTES, so it can give any number from
/// 0 to 255. Granny's contract does not give the deformer the length of
/// this table, so there is nothing to check there - the only place where
/// that hole can be closed is HERE: the table always has at least 256
/// entries, and the surplus ones point at the root bone.
///
/// That this is not excess caution: measured on 400 corpus models -
/// `triton_boss_lod_01` has 2641 vertices with a NON-ZERO weight at
/// binding numbers 155-255, with 86 bindings. That file is damaged (44% of
/// its normals are not unit length - see the open Oodle1 defect in
/// `gr2_oodle1.h`), but the conclusion is general: DATA CAN BE BAD, and a
/// reader in the client has no right to die of it. Without this padding
/// it was "memory access out of bounds" and the client crashed.
granny_mesh_binding* GrannyNewMeshBinding(granny_mesh const* c_pMesh,
                                          granny_skeleton const*,
                                          granny_skeleton const* c_pToSkeleton)
{
    if (!c_pMesh)
        return NULL;
    granny_mesh_binding* pBinding = new granny_mesh_binding();
    pBinding->c_pMesh = c_pMesh;

    const granny_int32 iBindings = c_pMesh->BoneBindingCount;
    const size_t uLength = (iBindings > 256) ? (size_t)iBindings : 256;
    pBinding->vecToBone.assign(uLength, 0);

    for (granny_int32 i = 0; i < iBindings; ++i)
    {
        const char* c_szName = c_pMesh->BoneBindings[i].BoneName;
        granny_int32x iFound = 0;
        bool bFound = false;
        if (c_szName && c_pToSkeleton)
            bFound = GrannyFindBoneByName(c_pToSkeleton, c_szName, &iFound);
        if (!bFound)
        {
            // Earlier this was SILENT: a bone the target skeleton
            // lacks got index 0 (the root) and a whole part of the mesh
            // (e.g. the boots) followed the root instead of the leg. One
            // report per pair.
            static std::set<std::string> s_setReported;
            const std::string strKey = std::string(c_pMesh->Name ? c_pMesh->Name : "?") +
                                       "|" + (c_szName ? c_szName : "(null)");
            if (s_setReported.insert(strKey).second)
                std::printf("m2w granny: mesh [%s] binds bone [%s], which is NOT in the skeleton"
                            " (%d bones) - follows the root\n",
                            c_pMesh->Name ? c_pMesh->Name : "?",
                            c_szName ? c_szName : "(null)",
                            c_pToSkeleton ? (int)c_pToSkeleton->BoneCount : -1);
            iFound = 0;
        }
        pBinding->vecToBone[i] = iFound;
    }
    return pBinding;
}

/// See `GrannyNewMeshBinding`.
void GrannyFreeMeshBinding(granny_mesh_binding* pBinding)
{
    delete pBinding;
}

/// The binding -> skeleton bone table (at least 256 entries, see above).
granny_int32x const* GrannyGetMeshBindingToBoneIndices(
    granny_mesh_binding const* c_pBinding)
{
    if (!c_pBinding || c_pBinding->vecToBone.empty())
        return NULL;
    return &c_pBinding->vecToBone[0];
}

// ---------------------------------------------------------------------------
// Deformation
// ---------------------------------------------------------------------------

/// Precomputes the component offsets of both layouts and the roles
/// (position, normal) that get transformed.
granny_mesh_deformer* GrannyNewMeshDeformer(
    granny_data_type_definition const* c_pInputType,
    granny_data_type_definition const* c_pOutputType,
    granny_deformation_type, granny_deformer_tail_flags)
{
    if (!c_pInputType || !c_pOutputType)
        return NULL;

    granny_mesh_deformer* pDeformer = new granny_mesh_deformer();
    pDeformer->c_pInputType = c_pInputType;
    pDeformer->c_pOutputType = c_pOutputType;
    pDeformer->uInputSize = m2wgr2::TypeSize(c_pInputType);
    pDeformer->uOutputSize = m2wgr2::TypeSize(c_pOutputType);
    pDeformer->iWeightsOffset = ComponentOffset(c_pInputType, "BoneWeights", NULL);
    pDeformer->iIndicesOffset = ComponentOffset(c_pInputType, "BoneIndices", NULL);

    unsigned uOffset = 0;
    for (const granny_data_type_definition* p = c_pOutputType;
         p->Type != GrannyEndMember; ++p)
    {
        TComponent kComponent;
        kComponent.uOutput = uOffset;
        kComponent.uSize = m2wgr2::MemberSize(p);
        kComponent.iInput = ComponentOffset(c_pInputType, p->Name, NULL);
        kComponent.eRole = ROLE_PLAIN;
        if (p->Name && strcmp(p->Name, GrannyVertexPositionName) == 0 &&
            kComponent.uSize >= 12)
            kComponent.eRole = ROLE_POSITION;
        else if (p->Name && strcmp(p->Name, GrannyVertexNormalName) == 0 &&
                 kComponent.uSize >= 12)
            kComponent.eRole = ROLE_NORMAL;
        pDeformer->vecComponents.push_back(kComponent);
        uOffset += kComponent.uSize;
    }
    return pDeformer;
}

/// See `GrannyNewMeshDeformer`.
void GrannyFreeMeshDeformer(granny_mesh_deformer* pDeformer)
{
    delete pDeformer;
}

/// Vertices through the bones - LINEAR BLENDING OF WEIGHTS.
///
/// `c_piMatrixIndices` says which SKELETON BONE corresponds to which mesh
/// binding; a vertex holds binding numbers, not bones. That one indirection
/// is the whole difference between "the mesh of the model" and "the mesh on
/// this skeleton" - thanks to it the same mesh fits another skeleton, as
/// long as the bones have the same names.
void GrannyDeformVertices(granny_mesh_deformer const* c_pDeformer,
                          granny_int32x const* c_piMatrixIndices,
                          granny_real32 const* c_pafMatrices,
                          granny_int32x iVertices,
                          void const* c_pvSource, void* pvTarget)
{
    m2wstats::TStopwatch kStopwatch(m2wstats::g_kSkinning);
    if (!c_pDeformer || !c_pvSource || !pvTarget || iVertices <= 0)
        return;
    if (c_pDeformer->uInputSize == 0 || c_pDeformer->uOutputSize == 0)
        return;

    const bool bWeighted = (c_pDeformer->iWeightsOffset >= 0 && c_pDeformer->iIndicesOffset >= 0 &&
                            c_pafMatrices && c_piMatrixIndices);


    // ON THE SAFETY OF READING `c_piMatrixIndices` - see the note at
    // `GrannyNewMeshBinding`. The caller gives no length for this table
    // (Granny's contract), so it is padded where it is made, to the full
    // byte range. Nothing left to check here.

    for (granny_int32x w = 0; w < iVertices; ++w)
    {
        const unsigned char* c_pbySource =
            (const unsigned char*)c_pvSource + (size_t)w * c_pDeformer->uInputSize;
        unsigned char* pbyTarget =
            (unsigned char*)pvTarget + (size_t)w * c_pDeformer->uOutputSize;

        // TWO SAVINGS, NEITHER CHANGING THE RESULT
        // ===================================================
        // Skinning was the largest single item of the frame budget in the
        // game: 195.5 ms of 500 ms, nearly two fifths of all the work. The
        // work is inherently large - every vertex of every character in
        // every frame - so every multiplication removed counts.
        //
        // 1. ONE BONE WITH FULL WEIGHT. When a weight is 255 the rest MUST
        //    be zero (weights sum to 255), and blending reduces to copying
        //    that bone's matrix. It is taken directly instead of multiplying
        //    sixteen numbers by one and adding to zeros. This is the case of
        //    MOST vertices of a character - joints have several bones, the
        //    surfaces between them do not.
        //
        //    The condition is DELIBERATELY strict (exactly 255, not
        //    "nearly"): at weight 254 the original scales the matrix by
        //    0.996 and so it stays, because nothing here normalises the sum
        //    of weights.
        //
        // 2. TWELVE NUMBERS INSTEAD OF SIXTEEN. The fourth column of the
        //    matrix is never read below - the transform takes `m[i*4+j]`
        //    only for `j` from zero to two. Blending it was computing
        //    something headed straight for the bin.
        float matBlended[4][4];
        const float* c_pafM = &matBlended[0][0];

        if (bWeighted)
        {
            const unsigned char* c_pbyWeights = c_pbySource + c_pDeformer->iWeightsOffset;
            const unsigned char* c_pbyIndices = c_pbySource + c_pDeformer->iIndicesOffset;

            // The full weight is looked for in ALL four, not only the
            // first. The first version checked `c_pbyWeights[0] == 255`,
            // assuming the weights are sorted descending - an assumption
            // with no backing in the format or the measurement. Measured on
            // 120 characters: the shortcut hit so rarely that the whole fix
            // gave 11 percent instead of the expected half.
            //
            // Four byte comparisons cost next to nothing against the sixteen
            // multiplications they remove.
            int iFull = -1;
            if (SkinShortcutEnabled())
            {
                for (int k = 0; k < 4; ++k)
                    if (c_pbyWeights[k] == 255)
                    {
                        iFull = k;
                        break;
                    }
            }

            if (iFull >= 0)
            {
                c_pafM = c_pafMatrices +
                         (size_t)c_piMatrixIndices[c_pbyIndices[iFull]] * 16;
            }
            else
            {
                memset(matBlended, 0, sizeof(matBlended));
                float fSum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    const float fWeight = c_pbyWeights[k] / 255.0f;
                    if (fWeight <= 0.0f)
                        continue;
                    const granny_int32x iBone =
                        c_piMatrixIndices[c_pbyIndices[k]];
                    const float* c_pafBone = c_pafMatrices + (size_t)iBone * 16;
                    for (int i = 0; i < 4; ++i)
                    {
                        matBlended[i][0] += fWeight * c_pafBone[i * 4 + 0];
                        matBlended[i][1] += fWeight * c_pafBone[i * 4 + 1];
                        matBlended[i][2] += fWeight * c_pafBone[i * 4 + 2];
                    }
                    fSum += fWeight;
                }
                if (fSum <= 0.0f)
                    Identity(matBlended);
            }
        }
        else
        {
            Identity(matBlended);
        }

        for (size_t s = 0; s < c_pDeformer->vecComponents.size(); ++s)
        {
            const TComponent& c_rComponent = c_pDeformer->vecComponents[s];
            unsigned char* pbyTo = pbyTarget + c_rComponent.uOutput;
            if (c_rComponent.iInput < 0)
            {
                // A component absent from the source cannot be computed.
                // Zero is honest here - somebody else's bytes are not.
                memset(pbyTo, 0, c_rComponent.uSize);
                continue;
            }
            const unsigned char* c_pbyFrom = c_pbySource + c_rComponent.iInput;

            if (c_rComponent.eRole == ROLE_POSITION || c_rComponent.eRole == ROLE_NORMAL)
            {
                float af[3];
                memcpy(af, c_pbyFrom, sizeof(af));
                // A position has a fourth component of 1 (translation
                // applies), a normal 0 (translation does NOT, rotation does).
                const float fW = (c_rComponent.eRole == ROLE_POSITION) ? 1.0f : 0.0f;
                float afResult[3];
                for (int j = 0; j < 3; ++j)
                    afResult[j] = af[0] * c_pafM[0 * 4 + j] +
                                  af[1] * c_pafM[1 * 4 + j] +
                                  af[2] * c_pafM[2 * 4 + j] +
                                  fW * c_pafM[3 * 4 + j];
                memcpy(pbyTo, afResult, sizeof(afResult));
                if (c_rComponent.uSize > 12)
                    memset(pbyTo + 12, 0, c_rComponent.uSize - 12);
            }
            else
            {
                memcpy(pbyTo, c_pbyFrom, c_rComponent.uSize);
            }
        }
    }

}

}  // extern "C"
