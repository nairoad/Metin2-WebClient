// SPDX-License-Identifier: GPL-2.0-or-later
// granny_internal.h - the structs `granny.h` leaves OPAQUE, shared between
// granny_pose.cpp (poses, deformation) and granny_control.cpp (animation
// controls, curve sampling): the model instance and the bridge functions
// between the two files.

// Design:
// Earlier the model instance was defined in `granny_pose.cpp` and had
// no list of controls - there was nothing to control. Animations need the
// instance to know its controls and a control its instance; that is this
// header.
//
// NAMESPACE. In C++ mode `granny.h` assumes the real definition sits in
// `namespace granny` and `granny_model_instance` is only an alias
// (`typedef struct granny::model_instance`). Defining in the global
// namespace gives "definition conflicts with typedef" - so the definitions
// go where the header calls for them, with its own macros.

#pragma once

#include "win32_compat.h"
#include <granny.h>

#include <vector>

GRANNY_NAMESPACE_BEGIN;

struct control;

/// A model instance: the model, its clock, the controls playing on it and
/// the clock-rate measurement.
struct model_instance
{
    granny_model const* c_pModel;
    granny_real32 fClock;
    /// THE MODEL'S INITIAL PLACEMENT, already as a matrix.
    ///
    /// Not decoration - the missing start of the bone chain. Measured on
    /// `warrior_novice`: the root bone `Bip01` has an identity LOCAL
    /// transform while its inverse-world matrix carries a -90 degree
    /// rotation and a 99.44 lift. The difference sits in
    /// `Model->InitialPlacement` - it is what raises the skeleton from
    /// lying to standing. Without it the composite matrices do not come
    /// out as identity and the character is off by its whole height.
    /// (Computed in `GrannyInstantiateModel`; NOT applied in
    /// `GrannySampleModelAnimationsAccelerated` - see there.)
    float matInitialPlacement[4][4];

    /// Controls playing on this instance, in start order. Usually one, two
    /// during a transition between motions (the old one fades out, the new
    /// one in). The pointers belong to the registry in `granny_control.cpp`.
    std::vector<control*> vecControls;

    /// Clock-rate measurement: the last wall-clock and model-clock
    /// readings and a sample counter - per instance.
    double dMeasureWall;
    float fMeasureClock;
    unsigned uMeasureSamples;
    unsigned uMeasureClocks;      ///< `SetModelClock` calls since the last report
    unsigned long ulMeasureFrame; ///< loop frame number at the last report
};

GRANNY_NAMESPACE_END;

/// Composes the local pose of bones `[iFirst, iFirst + iCount)` from the
/// animations playing on the instance. A bone no control touches gets the
/// skeleton's rest pose. Called from `GrannySampleModelAnimations`.
void M2W_SampleControls(granny_model_instance const* c_pInstance,
                        granny_int32x iFirst, granny_int32x iCount,
                        granny_local_pose* pResult);

/// Detaches every control from the instance - before it is freed.
void M2W_DetachControls(granny_model_instance* pInstance);

/// ROOT MOTION: how far the animations playing on the instance
/// move the model in ITS OWN space over `fSeconds`. A weighted sum over the
/// controls, only for track groups with the root motion EXTRACTED from the
/// curves (`LoopTranslation` per loop). False when nothing moves.
bool M2W_RootMotion(granny_model_instance const* c_pInstance, float fSeconds,
                    float afTranslation[3]);

/// The transforms of a local pose - `granny_pose.cpp` keeps them in a vector.
granny_transform* M2W_PoseTransforms(granny_local_pose* pPose,
                                     granny_int32x* piBones);
