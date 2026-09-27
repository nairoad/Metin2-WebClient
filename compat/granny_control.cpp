// SPDX-License-Identifier: GPL-2.0-or-later
// granny_control.cpp - ANIMATION CONTROLS and curve sampling: the
// `GrannyXxxControl` API, the clock and weight of a control, sampling of
// the old-format curves and blending of the controls into a local pose.

// Design:
// Earlier every `GrannyXxxControl` was a stub: characters stood in the
// rest pose and the stub registry counted 2520 `GrannyPlayControlledAnimation`
// calls per period. This file replaces them.
//
// WHAT THE DATA CARRIES - MEASURED, NOT ASSUMED (tools/animation_review.py),
// 152 assassin animations, 12047 tracks:
//   * EVERY curve is in Granny's OLD format: `Degree`, `Knots`, `Controls` as
//     plain floats. None of the nineteen packed `granny_curve_data_*`
//     variants of the header occurs in the corpus;
//   * degrees: 0 (constant) and 2 (quadratic). No degree 1 or 3;
//   * dimensions: position 3, orientation 4 (quaternion), scale 9 (3x3);
//   * an EMPTY curve (`KnotCount == 0`) means "no change" - the bone takes
//     that component from the rest pose;
//   * one track group per animation (150 of 152; two have two);
//   * group flags 2 or 3: tracks SORTED by name, root motion EXTRACTED from
//     the animation (`AccumulationExtracted`). The character walks on the
//     spot and the game moves it through the world - see `M2W_RootMotion`.
//
// HOW A DEGREE-2 CURVE IS EVALUATED - and what is an approximation here.
// Granny keeps as many knots as control points. For degree 0 those are
// plain key frames; for degree 2 the original evaluates a B-spline. Here
// neighbouring points are interpolated LINEARLY (quaternions: with
// hemisphere choice and normalisation). At 21 or more knots per second the
// difference from the quadratic curve is below one pixel of motion; a
// DELIBERATE, WRITTEN-DOWN approximation, to be measured at the "animation
// quality" item of the plan, not hidden.
//
// CLOCKS. An instance has a model clock (`GrannySetModelClock`, seconds).
// A control remembers when it started and computes its own clock:
//     raw = (model_clock - start) * speed + offset
// Loops: 0 means forever (`fmod`), N means N times and stop at the end.
// Ease-in and ease-out are weight curves in the MODEL clock - TMP4 gives
// them in the same time `SetModelClock` gets.
//
// FREEING - why not deleted at once. TMP4 keeps `m_pgrnCtrl` and asks it
// `GrannyControlIsComplete` ALSO after `GrannyFreeCompletedModelControls`
// was allowed to free it (`ModelInstanceMotion.cpp:42`). With the real
// library that got away; here `delete` plus a read of that memory are
// non-deterministic bugs. So freeing DETACHES the control from the
// instance and puts it into a waiting room; the memory dies only when the
// room overflows (by then TMP4 has long had a new pointer). Every entry
// checks the pointer against the REGISTRY of live controls - an unknown
// pointer is a no-op, not a crash. AN ADDRESS IS NOT AN IDENTITY (found in
// review): a registry by pointer alone guarded against reading freed
// memory, but not against `new` handing the same address to a NEW control
// of another character - and TMP4 keeps the old `m_pgrnCtrl` indefinitely
// while a character stands. Then the old pointer "comes alive" and its
// `SetControlSpeed` breaks somebody else's animation. So an address
// deleted from the waiting room is remembered as BURNED for the rest of
// the session and never reused: a `new` that repeats a burned address is
// refused and allocates again.

#include <math.h>
#include <string.h>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <stdio.h>
#include <unistd.h>
#include <emscripten/emscripten.h>

#include "granny_internal.h"
#include "frame_stats.h"
#include "stubs.h"

extern unsigned long g_ulM2wFrames;  // frame_stats.cpp (pose probe, file /m2w_pose_log)

GRANNY_NAMESPACE_BEGIN;

/// An ease-in or ease-out weight curve: cubic Hermite from `fA` to `fD`
/// between the clocks `fFrom` and `fTo` (MODEL clock).
struct weight_curve
{
    float fFrom, fTo;
    float fA, fB, fC, fD;
};

/// One animation playing on an instance.
struct control
{
    model_instance* pInstance;
    granny_animation const* c_pAnimation;
    granny_track_group const* c_pGroup;
    /// Track number for every skeleton bone, -1 when the bone has no track.
    /// Shared through the cache of (group, skeleton) pairs.
    const std::vector<int>* c_pvecTracks;

    float fStart;            ///< model clock at start
    float fSpeed;
    int iLoops;              ///< 0 = forever
    float fOffset;           ///< `SetControlRawLocalClock`
    bool bCompletionSet;     ///< `CompleteControlAt`
    float fCompletionClock;  ///< model clock after which the control is complete
    bool bEaseIn, bEaseOut;
    bool bEaseInCurve, bEaseOutCurve;  ///< whether `SetControlEase*Curve` set a curve at all
    weight_curve kEaseIn, kEaseOut;
    bool bFreeOnceUnused;
    bool bAlive;
};

GRANNY_NAMESPACE_END;

namespace
{

typedef granny::control TControl;
typedef granny::model_instance TInstance;

// --- registry of live controls and the waiting room ------------------------

/// Every live control (created and not yet released); the check behind
/// `Alive`.
std::set<TControl*>& Registry()
{
    static std::set<TControl*> s_setRegistry;
    return s_setRegistry;
}

/// Released controls kept undeleted (up to WAITING_ROOM_MAX) so a stale
/// pointer held by TMP4 still points at a control, not at freed memory.
std::deque<TControl*>& WaitingRoom()
{
    static std::deque<TControl*> s_dequeWaitingRoom;
    return s_dequeWaitingRoom;
}

enum { WAITING_ROOM_MAX = 256 };

/// Addresses deleted from the waiting room - never reused (see "Design").
std::set<const void*>& Burned()
{
    static std::set<const void*> s_setBurned;
    return s_setBurned;
}

/// Allocates a control at an address that was never burned (see `Burned`);
/// the caller registers it.
TControl* NewControl()
{
    // A burned address = somebody may still hold it. Keep allocating until
    // a fresh one comes; the burned ones are freed only after that.
    std::vector<TControl*> vecRejected;
    TControl* p = new TControl();
    while (Burned().count(p))
    {
        vecRejected.push_back(p);
        p = new TControl();
    }
    for (size_t i = 0; i < vecRejected.size(); ++i)
        delete vecRejected[i];
    return p;
}

/// The control behind an outside pointer, or NULL when it is unknown or
/// released.
TControl* Alive(granny_control const* c_pControl)
{
    TControl* p = const_cast<TControl*>(c_pControl);
    if (!p || Registry().find(p) == Registry().end() || !p->bAlive)
        return NULL;
    return p;
}

/// Removes the control from its model instance's control list and clears
/// its instance; nothing when it has none.
void Detach(TControl* pControl)
{
    if (!pControl->pInstance)
        return;
    std::vector<TControl*>& v = pControl->pInstance->vecControls;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == pControl)
        {
            v.erase(v.begin() + (long)i);
            break;
        }
    }
    pControl->pInstance = NULL;
}

/// Release = detach + waiting room. See "Design".
void Release(TControl* pControl)
{
    Detach(pControl);
    Registry().erase(pControl);
    pControl->bAlive = false;
    std::deque<TControl*>& q = WaitingRoom();
    q.push_back(pControl);
    while (q.size() > WAITING_ROOM_MAX)
    {
        Burned().insert(q.front());
        delete q.front();
        q.pop_front();
    }
}

// --- binding tracks to bones - once per (group, skeleton) pair --------------
// Tracks are named like bones. Matching by name costs O(bones * log tracks)
// and is the same for every character with this skeleton playing this
// animation - so it is computed once.

typedef std::pair<granny_track_group const*, granny_skeleton const*> TBindingPair;

/// For every bone of the skeleton, the index of the group's transform track
/// with the same name (-1 when there is none); computed once per (group,
/// skeleton) pair and cached.
const std::vector<int>* TracksForSkeleton(granny_track_group const* c_pGroup,
                                          granny_skeleton const* c_pSkeleton)
{
    static std::map<TBindingPair, std::vector<int> > s_mapCache;
    const TBindingPair k(c_pGroup, c_pSkeleton);
    std::map<TBindingPair, std::vector<int> >::iterator it = s_mapCache.find(k);
    if (it != s_mapCache.end())
        return &it->second;

    std::vector<int>& v = s_mapCache[k];
    v.assign((size_t)(c_pSkeleton->BoneCount > 0 ? c_pSkeleton->BoneCount : 0), -1);

    std::map<std::string, int> mapByName;
    for (granny_int32 i = 0; i < c_pGroup->TransformTrackCount; ++i)
    {
        const char* c_sz = c_pGroup->TransformTracks[i].Name;
        if (c_sz)
            mapByName[c_sz] = (int)i;
    }
    for (granny_int32 i = 0; i < c_pSkeleton->BoneCount; ++i)
    {
        const char* c_sz = c_pSkeleton->Bones[i].Name;
        if (!c_sz)
            continue;
        std::map<std::string, int>::const_iterator itN = mapByName.find(c_sz);
        if (itN != mapByName.end())
            v[(size_t)i] = itN->second;
    }
    return &v;
}

// --- clock and weight -------------------------------------------------------

/// The control's local clock before loops: (model clock - start) * speed +
/// offset; a control without an instance uses its start time.
float RawClock(const TControl& c_rControl)
{
    const float fModel = c_rControl.pInstance ? c_rControl.pInstance->fClock : c_rControl.fStart;
    return (fModel - c_rControl.fStart) * c_rControl.fSpeed + c_rControl.fOffset;
}

/// The animation's duration, 0 when there is no animation or it is not
/// positive.
float AnimationDuration(const TControl& c_rControl)
{
    const float f = c_rControl.c_pAnimation ? c_rControl.c_pAnimation->Duration : 0.0f;
    return f > 0.0f ? f : 0.0f;
}

/// Time within the animation after the loops. `pbPastEnd` says whether the
/// finished loops have already passed.
float LocalTime(const TControl& c_rControl, bool* pbPastEnd)
{
    const float fLength = AnimationDuration(c_rControl);
    float fRaw = RawClock(c_rControl);
    *pbPastEnd = false;
    if (fLength <= 0.0f)
        return 0.0f;
    if (fRaw < 0.0f)
        fRaw = 0.0f;
    if (c_rControl.iLoops > 0 && fRaw >= fLength * (float)c_rControl.iLoops)
    {
        *pbPastEnd = true;
        return fLength;
    }
    const float f = fmodf(fRaw, fLength);
    return f;
}

/// A HERMITE curve, not Bezier (reviewer). The header names the
/// parameters `StartValue, StartTangent, EndTangent, EndValue` - value and
/// TANGENT at both ends. The first version took them for four Bezier
/// control points; the ends came out the same, the middle of a transition
/// had another shape. There is no Granny source, so this is a reading of
/// the header's names (certainty: names, not code) - written down, not
/// hidden.
float Hermite(const granny::weight_curve& c_rCurve, float fClock)
{
    if (fClock <= c_rCurve.fFrom)
        return c_rCurve.fA;
    if (fClock >= c_rCurve.fTo || c_rCurve.fTo <= c_rCurve.fFrom)
        return c_rCurve.fD;
    const float t = (fClock - c_rCurve.fFrom) / (c_rCurve.fTo - c_rCurve.fFrom);
    const float t2 = t * t, t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return h00 * c_rCurve.fA + h10 * c_rCurve.fB + h11 * c_rCurve.fC + h01 * c_rCurve.fD;
}

/// AN UNSET CURVE DOES NOT ATTENUATE. `CopyMotion` (LOD change)
/// and `SetMotionPointer` with blendTime 0 call `GrannySetControlEaseIn(true)`
/// WITHOUT `SetControlEaseInCurve`. Earlier the curve was then zeros
/// (`fFrom = fTo = 0`) and `Hermite` for `fClock >= fTo` returned the end
/// value 0 -> weight 0 FOREVER: a mob that came close to the camera (LOD
/// thresholds 500/1500/2500) stood in the rest pose without root motion
/// while the server had it hit the player from a distance (measured:
/// "ZERO active: clock 2.178 controls 1: [run start 0.017 weight 0.000 in
/// 1(0.00..0.00)]"). The curve counts only when somebody set it - an
/// explicit flag from `SetControlEase*Curve`, NOT a test of the values: an
/// ease-out curve with blendTime 0 (`(t, t, 1,1,0,0)`, the default of
/// `SetMotionPointer`) is legal and has to give weight 0 at once.
float Weight(const TControl& c_rControl)
{
    const float fClock = c_rControl.pInstance ? c_rControl.pInstance->fClock : c_rControl.fStart;
    float f = 1.0f;
    if (c_rControl.bEaseIn && c_rControl.bEaseInCurve)
        f *= Hermite(c_rControl.kEaseIn, fClock);
    if (c_rControl.bEaseOut && c_rControl.bEaseOutCurve)
        f *= Hermite(c_rControl.kEaseOut, fClock);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

/// COMPLETION IS EXPLICIT. Earlier a control was also
/// "complete" once its loops ran out. Measured (bone 0 probe, space
/// attack): after the last frame of the attack `SetMotionPointer(wait)` ->
/// `GrannyFreeControlIfComplete` freed the attack AT ONCE, and the new
/// `wait` has weight exactly 0 in its start frame (the ease-in curve starts
/// from 0) -> ONE FRAME WITHOUT ANY CONTROL = pure rest pose (T-pose, root
/// at z 0 = 87 cm below ground). User: "after an animation ends the
/// character sinks under the ground for a split second and T-poses".
///
/// The original (`ModelInstanceMotion.cpp:67-76`) gives the old control an
/// EASE-OUT curve 1->0 over blendTime and ONLY THEN `CompleteControlAt(+blend)`
/// - which makes sense only when a finished one-shot animation is NOT
/// complete by itself but stands on its last frame (`LocalTime` clamps it
/// so). `granny.h` has an explicit flag for that
/// (`GrannyGetControlCompletionCheckFlag`, `GrannyGetControlCompletionClock`,
/// set by `CompleteControlAt`). So: complete = the clock passed an
/// explicitly set end. Nothing more. The game relies on nothing else (it
/// measures the end of a motion with its own `fEndTime`; `IsMotionPlaying`
/// only in `CopyMotion` for attached parts).
bool Completed(const TControl& c_rControl)
{
    return c_rControl.bCompletionSet && c_rControl.pInstance && c_rControl.pInstance->fClock >= c_rControl.fCompletionClock;
}

// --- curves -----------------------------------------------------------------

/// The `granny_old_curve` that `gr2_to_granny.cpp` stored in the curve's
/// variant (NULL when the curve is empty).
const granny_old_curve* OldCurve(const granny_curve2& c_rCurve)
{
    return (const granny_old_curve*)c_rCurve.CurveData.Object;
}

/// Samples a curve of `iDimension` components at time `t`. False when the
/// curve is empty (the component is to stay at rest).
bool Sample(const granny_old_curve* c_pCurve, int iDimension, float t, float* pafResult,
            bool bQuaternion)
{
    if (!c_pCurve || c_pCurve->KnotCount <= 0 || !c_pCurve->Controls ||
        c_pCurve->ControlCount < iDimension)
        return false;

    // THERE MUST BE POINTS FOR EVERY KNOT (reviewer). The equality
    // `ControlCount == KnotCount * dimension` is MEASURED on the corpus, not
    // ENFORCED - and the corpus has damaged files (`triton_boss_lod_01`). A
    // shorter point table = a read past it. Take as many knots as there
    // really are points for.
    int n = c_pCurve->KnotCount;
    if (c_pCurve->ControlCount < n * iDimension)
        n = c_pCurve->ControlCount / iDimension;
    if (n <= 0)
        return false;
    if (n == 1 || c_pCurve->Degree == 0 || !c_pCurve->Knots)
    {
        // Constant - or key frames without interpolation (degree 0).
        int i = 0;
        if (n > 1 && c_pCurve->Knots)
        {
            while (i + 1 < n && c_pCurve->Knots[i + 1] <= t)
                ++i;
        }
        if ((i + 1) * iDimension > c_pCurve->ControlCount)
            i = c_pCurve->ControlCount / iDimension - 1;
        memcpy(pafResult, c_pCurve->Controls + (size_t)i * iDimension,
               sizeof(float) * (size_t)iDimension);
        return true;
    }

    // The knot on the left: the largest i with Knots[i] <= t (binary search).
    const float* k = c_pCurve->Knots;
    if (t <= k[0])
    {
        memcpy(pafResult, c_pCurve->Controls, sizeof(float) * (size_t)iDimension);
        return true;
    }
    if (t >= k[n - 1])
    {
        memcpy(pafResult, c_pCurve->Controls + (size_t)(n - 1) * iDimension,
               sizeof(float) * (size_t)iDimension);
        return true;
    }
    int lo = 0, hi = n - 1;
    while (hi - lo > 1)
    {
        const int mid = (lo + hi) / 2;
        if (k[mid] <= t) lo = mid; else hi = mid;
    }
    const float fSpan = k[hi] - k[lo];
    const float u = fSpan > 0.0f ? (t - k[lo]) / fSpan : 0.0f;
    const float* a = c_pCurve->Controls + (size_t)lo * iDimension;
    const float* b = c_pCurve->Controls + (size_t)hi * iDimension;

    float fSign = 1.0f;
    if (bQuaternion)
    {
        // The same hemisphere: q and -q are the same rotation, but the
        // interpolation between them passes through zero.
        const float fDot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (fDot < 0.0f)
            fSign = -1.0f;
    }
    for (int i = 0; i < iDimension; ++i)
        pafResult[i] = a[i] + (fSign * b[i] - a[i]) * u;
    if (bQuaternion)
    {
        const float fLength = sqrtf(pafResult[0] * pafResult[0] + pafResult[1] * pafResult[1] +
                                    pafResult[2] * pafResult[2] + pafResult[3] * pafResult[3]);
        if (fLength > 1e-6f)
            for (int i = 0; i < 4; ++i)
                pafResult[i] /= fLength;
    }
    return true;
}

/// The bone transform of one track at time `t`; components the track does
/// not carry come from the rest pose.
void SampleTrack(const granny_transform_track& c_rTrack, float t,
                 const granny_transform& c_rRest, granny_transform* pResult)
{
    *pResult = c_rRest;
    float af[9];
    if (Sample(OldCurve(c_rTrack.PositionCurve), 3, t, af, false))
    {
        memcpy(pResult->Position, af, sizeof(float) * 3);
        pResult->Flags |= GrannyHasPosition;
    }
    if (Sample(OldCurve(c_rTrack.OrientationCurve), 4, t, af, true))
    {
        memcpy(pResult->Orientation, af, sizeof(float) * 4);
        pResult->Flags |= GrannyHasOrientation;
    }
    if (Sample(OldCurve(c_rTrack.ScaleShearCurve), 9, t, af, false))
    {
        memcpy(pResult->ScaleShear, af, sizeof(float) * 9);
        pResult->Flags |= GrannyHasScaleShear;
    }
}

/// `result = result * (1 - w) + b * w` component by component; the
/// quaternion with hemisphere choice. For blending two motions at a
/// transition.
void Blend(granny_transform* pResult, const granny_transform& c_rB, float w)
{
    for (int i = 0; i < 3; ++i)
        pResult->Position[i] += (c_rB.Position[i] - pResult->Position[i]) * w;
    float fSign = 1.0f;
    float fDot = 0.0f;
    for (int i = 0; i < 4; ++i)
        fDot += pResult->Orientation[i] * c_rB.Orientation[i];
    if (fDot < 0.0f)
        fSign = -1.0f;
    float fLength = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        pResult->Orientation[i] += (fSign * c_rB.Orientation[i] - pResult->Orientation[i]) * w;
        fLength += pResult->Orientation[i] * pResult->Orientation[i];
    }
    fLength = sqrtf(fLength);
    if (fLength > 1e-6f)
        for (int i = 0; i < 4; ++i)
            pResult->Orientation[i] /= fLength;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            pResult->ScaleShear[i][j] += (c_rB.ScaleShear[i][j] - pResult->ScaleShear[i][j]) * w;
    pResult->Flags |= c_rB.Flags;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pose sampling - called from `granny_pose.cpp`
// ---------------------------------------------------------------------------

void M2W_SampleControls(granny_model_instance const* c_pInstance,
                        granny_int32x iFirst, granny_int32x iCount,
                        granny_local_pose* pResult)
{
    if (!c_pInstance || !c_pInstance->c_pModel || !pResult)
        return;
    const granny_skeleton* c_pSkeleton = c_pInstance->c_pModel->Skeleton;
    if (!c_pSkeleton || !c_pSkeleton->Bones)
        return;
    granny_int32x iPoseBones = 0;
    granny_transform* pafPose = M2W_PoseTransforms(pResult, &iPoseBones);
    if (!pafPose)
        return;

    // The controls with a non-zero weight and their times - computed once
    // per pose, not once per bone.
    struct TActive { const TControl* c_pControl; float fWeight; float fTime; };
    TActive aActive[8];
    int iActive = 0;
    float fWeightSum = 0.0f;
    const std::vector<TControl*>& v = c_pInstance->vecControls;
    for (size_t i = 0; i < v.size() && iActive < 8; ++i)
    {
        const TControl* c_pControl = v[i];
        if (!c_pControl->c_pGroup || !c_pControl->c_pvecTracks)
            continue;
        const float fWeight = Weight(*c_pControl);
        if (fWeight <= 0.0f)
            continue;
        bool bPastEnd = false;
        aActive[iActive].c_pControl = c_pControl;
        aActive[iActive].fWeight = fWeight;
        aActive[iActive].fTime = LocalTime(*c_pControl, &bPastEnd);
        ++iActive;
        fWeightSum += fWeight;
    }

    // MEASUREMENT: clock rate and root. The user sees the
    // character in the air and the animations "very sped up" - before
    // anything is fixed, measure: how much the model clock grows per second
    // of wall time (1.0 = good) and the rest and animated position of the
    // first bone with a track. Every 600 samples, not to flood the console.
    {
        static unsigned s_uSamples = 0;
        // The clock rate is measured on ONE instance (the first that got a
        // control) - there are many instances and they do not sample in
        // turn.
        TInstance* pInstance = const_cast<TInstance*>(c_pInstance);
        if ((pInstance->uMeasureSamples++ % 1800) == 0)
        {
            const double dNow = emscripten_get_now() / 1000.0;
            extern unsigned long g_ulM2wFrames;
            static double s_dTick = 0.0;
            const double dTick = timeGetTime() / 1000.0;
            if (pInstance->dMeasureWall > 0.0 && dNow > pInstance->dMeasureWall)
                printf("m2w anim: model clock %.3f/s of wall time, "
                       "controls %u, active %d, weight %.2f | SetModelClock %u times "
                       "in %lu loop frames | timeGetTime %.3f/s\n",
                       (c_pInstance->fClock - pInstance->fMeasureClock) / (dNow - pInstance->dMeasureWall),
                       (unsigned)v.size(), iActive, fWeightSum,
                       pInstance->uMeasureClocks, g_ulM2wFrames - pInstance->ulMeasureFrame,
                       (dTick - s_dTick) / (dNow - pInstance->dMeasureWall));
            s_dTick = dTick;
            pInstance->dMeasureWall = dNow; pInstance->fMeasureClock = c_pInstance->fClock;
            pInstance->uMeasureClocks = 0; pInstance->ulMeasureFrame = g_ulM2wFrames;
        }
        if ((s_uSamples++ % 60000) == 0 && iActive > 0)
        {

            const TActive& c_rActive = aActive[0];
            printf("m2w anim: '%s' duration %.2f time %.3f speed %.2f loops %d\n",
                   c_rActive.c_pControl->c_pAnimation->Name ? c_rActive.c_pControl->c_pAnimation->Name : "?",
                   c_rActive.c_pControl->c_pAnimation->Duration, c_rActive.fTime,
                   c_rActive.c_pControl->fSpeed, c_rActive.c_pControl->iLoops);
            for (granny_int32 b = 0; b < c_pSkeleton->BoneCount; ++b)
            {
                const int iTrack = (*c_rActive.c_pControl->c_pvecTracks)[(size_t)b];
                if (iTrack < 0) continue;
                const granny_transform& r = c_pSkeleton->Bones[b].LocalTransform;
                granny_transform kAnimated;
                SampleTrack(c_rActive.c_pControl->c_pGroup->TransformTracks[iTrack],
                            c_rActive.fTime, r, &kAnimated);
                printf("m2w anim: bone %d '%s' parent %d rest flags %u "
                       "(%.2f %.2f %.2f) q(%.2f %.2f %.2f %.2f) | anim flags %u "
                       "(%.2f %.2f %.2f) q(%.2f %.2f %.2f %.2f)\n",
                       (int)b, c_pSkeleton->Bones[b].Name, (int)c_pSkeleton->Bones[b].ParentIndex,
                       (unsigned)r.Flags, r.Position[0], r.Position[1], r.Position[2],
                       r.Orientation[0], r.Orientation[1], r.Orientation[2], r.Orientation[3],
                       (unsigned)kAnimated.Flags, kAnimated.Position[0], kAnimated.Position[1], kAnimated.Position[2],
                       kAnimated.Orientation[0], kAnimated.Orientation[1], kAnimated.Orientation[2], kAnimated.Orientation[3]);
                break;
            }
        }
    }

    for (granny_int32x i = iFirst; i < iFirst + iCount; ++i)
    {
        if (i < 0 || i >= c_pSkeleton->BoneCount || i >= iPoseBones)
            continue;
        const granny_transform& c_rRest = c_pSkeleton->Bones[i].LocalTransform;
        granny_transform& rResult = pafPose[i];

        // Blending: the first active control gives the base, every next one
        // blends in with the relative weight `w_k / (w_1 + ... + w_k)`.
        // After all of them that is the weighted mean - without a separate
        // division.
        float fSoFar = 0.0f;
        bool bAny = false;
        int iCovering = 0;  // MEASUREMENT - how many controls have a track for THIS bone.
        for (int k = 0; k < iActive; ++k)
        {
            const TActive& c_rActive = aActive[k];
            const int iTrack = (*c_rActive.c_pControl->c_pvecTracks)[(size_t)i];
            if (iTrack < 0)
                continue;
            ++iCovering;
            granny_transform kThis;
            SampleTrack(c_rActive.c_pControl->c_pGroup->TransformTracks[iTrack],
                        c_rActive.fTime, c_rRest, &kThis);
            if (!bAny)
            {
                rResult = kThis;
                bAny = true;
                fSoFar = c_rActive.fWeight;
            }
            else
            {
                fSoFar += c_rActive.fWeight;
                Blend(&rResult, kThis, c_rActive.fWeight / fSoFar);
            }
        }

        if (!bAny)
        {
            rResult = c_rRest;
        }
        // TEMPORARY MEASUREMENT: bone 0 (Bip01) - total weight
        // < 0.95 = a deficit, which earlier got the rest pose blended
        // in. Switched on by the file /m2w_pose_log (JS: Module.FS.writeFile).
        if (i <= 1)
        {
            static int s_iFlag = -1;
            static unsigned long s_ulFlagFrame = 0;
            extern unsigned long g_ulM2wFrames;
            if (s_iFlag < 0 || s_ulFlagFrame != g_ulM2wFrames)
            { s_iFlag = (access("/m2w_pose_log", F_OK) == 0) ? 1 : 0; s_ulFlagFrame = g_ulM2wFrames; }
            if (s_iFlag == 1)
            {
                FILE* pf = fopen("/m2w_poses.txt", "a");
                if (pf)
                {
                    fprintf(pf, "frame %lu inst %p model [%s] bones %d bone %d [%s] active %d covering %d weight_sum %.3f ", g_ulM2wFrames, (const void*)c_pInstance, c_pInstance->c_pModel && c_pInstance->c_pModel->Name ? c_pInstance->c_pModel->Name : "?", (int)c_pSkeleton->BoneCount, (int)i, c_pSkeleton->Bones[i].Name ? c_pSkeleton->Bones[i].Name : "?", iActive, iCovering, fSoFar);
                    for (int k = 0; k < iActive; ++k)
                        fprintf(pf, "[%s w %.3f t %.3f] ", aActive[k].c_pControl->c_pAnimation && aActive[k].c_pControl->c_pAnimation->Name ? aActive[k].c_pControl->c_pAnimation->Name : "?", aActive[k].fWeight, aActive[k].fTime);
                    fprintf(pf, "result T(%.1f %.1f %.1f) rest T(%.1f %.1f %.1f)\n", rResult.Position[0], rResult.Position[1], rResult.Position[2], c_rRest.Position[0], c_rRest.Position[1], c_rRest.Position[2]);
                    if (i == 0 && iActive == 0)
                    {
                        fprintf(pf, "  ZERO active: clock %.3f controls %u:", c_pInstance->fClock, (unsigned)v.size());
                        for (size_t k = 0; k < v.size(); ++k)
                        {
                            const TControl* c_pControl = v[k];
                            fprintf(pf, " [%s start %.3f weight %.3f in %d(%.2f..%.2f) out %d(%.2f..%.2f) end %d/%.3f group %p tracks %p]", c_pControl->c_pAnimation && c_pControl->c_pAnimation->Name ? strrchr(c_pControl->c_pAnimation->Name, 92) ? strrchr(c_pControl->c_pAnimation->Name, 92) + 1 : c_pControl->c_pAnimation->Name : "?", c_pControl->fStart, Weight(*c_pControl), (int)c_pControl->bEaseIn, c_pControl->kEaseIn.fFrom, c_pControl->kEaseIn.fTo, (int)c_pControl->bEaseOut, c_pControl->kEaseOut.fFrom, c_pControl->kEaseOut.fTo, (int)c_pControl->bCompletionSet, c_pControl->fCompletionClock, (const void*)c_pControl->c_pGroup, (const void*)c_pControl->c_pvecTracks);
                        }
                        fprintf(pf, "\n");
                    }
                    fclose(pf);
                }
            }
        }
        // THE REST-POSE FILL THRESHOLD - as Granny.
        //
        // Earlier this read: one control with weight < 1 -> blend the
        // rest pose into the whole missing part (`1 - sum`). Measured (the
        // probe below, space attack): after a finished attack the old
        // control is already released (`GrannyFreeCompletedModelControls` in
        // `UpdateTime` - the same in TMP4, `IsMotionPlaying` asks
        // `GrannyControlIsComplete`), and `wait` comes in ALONE with a 0.1 s
        // curve: weights 0.05, 0.18, 0.38, 0.58, 0.81. The player model's
        // `Bip01` rest is (0 0 0) (measured - the height comes
        // with the animation track), so 95% rest = the character 83 cm
        // below ground in a T-pose for 5 frames. User: "after an animation
        // ends the character sinks under the ground for a split second and
        // T-poses".
        //
        // Granny does it differently: `granny.h` defines
        // `GrannyDefaultLocalPoseFillThreshold 0.2f` and
        // `GrannySetLocalPoseFillThreshold` (TMP4 never calls it - the
        // default threshold). A bone with total weight W < threshold gets
        // the rest pose in the share `(threshold - W) / threshold`; at
        // W >= threshold the result is the pure weighted mean of the
        // controls. The formula is a reading of the Granny documentation
        // from memory (certainty: the name and value of the constant from
        // the header - yes; the exact shape of the formula - no), written
        // down, not hidden. Effect for the attack: rest only in the first
        // 1-2 frames (W 0.05 -> 73%, W 0.18 -> 9%), then pure animation - as
        // in the original.
        if (bAny && fSoFar < GrannyDefaultLocalPoseFillThreshold)
        {
            Blend(&rResult, c_rRest,
                  (GrannyDefaultLocalPoseFillThreshold - fSoFar) / GrannyDefaultLocalPoseFillThreshold);
        }
    }
}

bool M2W_RootMotion(granny_model_instance const* c_pInstance, float fSeconds,
                    float afTranslation[3])
{
    afTranslation[0] = afTranslation[1] = afTranslation[2] = 0.0f;
    if (!c_pInstance || fSeconds <= 0.0f)
        return false;

    // Granny: when the root motion is EXTRACTED from the curves
    // (`GrannyAccumulationExtracted`), its linear part lies in
    // `LoopTranslation` - the translation over ONE loop of the animation.
    // Over `fSeconds` of model clock the control advances `fSeconds * speed`
    // seconds of animation, i.e. the fraction `/ Duration` of a loop.
    // Blending: the mean weighted by the controls' weights, as for the pose.
    float fWeightSum = 0.0f;
    bool bAny = false;
    const std::vector<TControl*>& v = c_pInstance->vecControls;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const TControl* c_pControl = v[i];
        if (!c_pControl->c_pGroup || !c_pControl->c_pAnimation)
            continue;
        const float fWeight = Weight(*c_pControl);
        if (fWeight <= 0.0f)
            continue;
        fWeightSum += fWeight;
        if (!(c_pControl->c_pGroup->Flags & GrannyAccumulationExtracted))
            continue;
        const float fLength = AnimationDuration(*c_pControl);
        if (fLength <= 0.0f)
            continue;
        // After the last loop of a finished control nothing flows any more.
        bool bPastEnd = false;
        LocalTime(*c_pControl, &bPastEnd);
        if (bPastEnd)
            continue;
        const float fFraction = fSeconds * c_pControl->fSpeed / fLength;
        for (int k = 0; k < 3; ++k)
            afTranslation[k] += fWeight * fFraction * c_pControl->c_pGroup->LoopTranslation[k];
        bAny = true;
    }
    if (!bAny)
        return false;
    if (fWeightSum > 0.0f && fWeightSum != 1.0f)
        for (int k = 0; k < 3; ++k)
            afTranslation[k] /= fWeightSum;
    return true;
}

void M2W_DetachControls(granny_model_instance* pInstance)
{
    if (!pInstance)
        return;
    while (!pInstance->vecControls.empty())
    {
        TControl* pControl = pInstance->vecControls.back();
        Release(pControl);
    }
}

// ---------------------------------------------------------------------------
// The `granny.h` contract
// ---------------------------------------------------------------------------

extern "C" {

/// A new control on the instance. The track group: the original looks it
/// up by model name, but in the corpus an animation has one group (150 of
/// 152) and that is the right one. With several the one named like the
/// model is taken, and when there is none - the first.
granny_control* GrannyPlayControlledAnimation(granny_real32 fStart,
                                              granny_animation const* c_pAnimation,
                                              granny_model_instance* pInstance)
{
    if (!c_pAnimation || !pInstance || !pInstance->c_pModel || !pInstance->c_pModel->Skeleton)
        return NULL;

    TControl* pControl = NewControl();
    memset(pControl, 0, sizeof(*pControl));
    pControl->bAlive = true;
    pControl->pInstance = pInstance;
    pControl->c_pAnimation = c_pAnimation;
    pControl->fStart = fStart;
    pControl->fSpeed = 1.0f;
    pControl->iLoops = 0;

    granny_track_group const* c_pGroup = NULL;
    for (granny_int32 i = 0; i < c_pAnimation->TrackGroupCount; ++i)
    {
        granny_track_group const* c_pG = c_pAnimation->TrackGroups[i];
        if (!c_pG)
            continue;
        if (!c_pGroup)
            c_pGroup = c_pG;
        if (c_pG->Name && pInstance->c_pModel->Name &&
            strcmp(c_pG->Name, pInstance->c_pModel->Name) == 0)
        {
            c_pGroup = c_pG;
            break;
        }
    }
    pControl->c_pGroup = c_pGroup;
    pControl->c_pvecTracks = c_pGroup
        ? TracksForSkeleton(c_pGroup, pInstance->c_pModel->Skeleton) : NULL;

    Registry().insert(pControl);
    pInstance->vecControls.push_back(pControl);
    return pControl;
}

/// Releases a live control (an unknown pointer is a no-op - see "Design").
void GrannyFreeControl(granny_control* pControl)
{
    if (TControl* p = Alive(pControl))
        Release(p);
}

/// True when the control is gone or complete (then it is released).
bool GrannyFreeControlIfComplete(granny_control* pControl)
{
    TControl* p = Alive(pControl);
    if (!p)
        return true;
    if (!Completed(*p))
        return false;
    Release(p);
    return true;
}

/// Marks the control for `GrannyFreeCompletedModelControls`.
void GrannyFreeControlOnceUnused(granny_control* pControl)
{
    if (TControl* p = Alive(pControl))
        p->bFreeOnceUnused = true;
}

/// Releases the instance's controls that are marked and complete.
void GrannyFreeCompletedModelControls(granny_model_instance const* c_pInstance)
{
    if (!c_pInstance)
        return;
    TInstance* pInstance = const_cast<TInstance*>(c_pInstance);
    for (size_t i = 0; i < pInstance->vecControls.size();)
    {
        TControl* pControl = pInstance->vecControls[i];
        if (pControl->bFreeOnceUnused && Completed(*pControl))
            Release(pControl);  // removes it from the vector
        else
            ++i;
    }
}

/// True for an unknown pointer too - TMP4 asks after freeing (see "Design").
bool GrannyControlIsComplete(granny_control const* c_pControl)
{
    const TControl* c_p = Alive(c_pControl);
    if (!c_p)
        return true;
    return Completed(*c_p);
}

/// The explicit completion clock - the only way a control completes.
void GrannyCompleteControlAt(granny_control* pControl, granny_real32 fClock)
{
    if (TControl* p = Alive(pControl))
    {
        p->bCompletionSet = true;
        p->fCompletionClock = fClock;
    }
}

/// The animation's duration, 0 for an unknown control.
granny_real32 GrannyGetControlLocalDuration(granny_control const* c_pControl)
{
    const TControl* c_p = Alive(c_pControl);
    return c_p ? AnimationDuration(*c_p) : 0.0f;
}

/// See `GrannySetControlLoopCount`.
granny_int32x GrannyGetControlLoopCount(granny_control const* c_pControl)
{
    const TControl* c_p = Alive(c_pControl);
    return c_p ? c_p->iLoops : 0;
}

/// 0 = forever; negative values clamp to 0.
void GrannySetControlLoopCount(granny_control* pControl, granny_int32x iLoops)
{
    if (TControl* p = Alive(pControl))
        p->iLoops = iLoops < 0 ? 0 : (int)iLoops;
}

/// See `GrannySetControlSpeed`; 1 for an unknown control.
granny_real32 GrannyGetControlSpeed(granny_control const* c_pControl)
{
    const TControl* c_p = Alive(c_pControl);
    return c_p ? c_p->fSpeed : 1.0f;
}

/// A speed change must not make the clock jump: the offset is recomputed so
/// that the raw clock stays where it is.
void GrannySetControlSpeed(granny_control* pControl, granny_real32 fSpeed)
{
    TControl* p = Alive(pControl);
    if (!p)
        return;
    const float fRaw = RawClock(*p);
    p->fSpeed = fSpeed;
    const float fModel = p->pInstance ? p->pInstance->fClock : p->fStart;
    p->fOffset = fRaw - (fModel - p->fStart) * p->fSpeed;
}

/// `(model clock - start) * speed + offset`, 0 for an unknown control.
granny_real32 GrannyGetControlRawLocalClock(granny_control* pControl)
{
    const TControl* c_p = Alive(pControl);
    return c_p ? RawClock(*c_p) : 0.0f;
}

/// Sets the raw clock by recomputing the offset.
void GrannySetControlRawLocalClock(granny_control* pControl, granny_real32 fClock)
{
    TControl* p = Alive(pControl);
    if (!p)
        return;
    const float fModel = p->pInstance ? p->pInstance->fClock : p->fStart;
    p->fOffset = fClock - (fModel - p->fStart) * p->fSpeed;
}

/// Switches the ease-in weight on; it attenuates only once a curve is set.
void GrannySetControlEaseIn(granny_control* pControl, bool bEnable)
{
    if (TControl* p = Alive(pControl))
        p->bEaseIn = bEnable;
}

/// Hermite weight curve from `fA` at clock `fFrom` to `fD` at `fTo`
/// (`fB`, `fC` the tangents); also sets the "curve set" flag.
void GrannySetControlEaseInCurve(granny_control* pControl, granny_real32 fFrom,
                                 granny_real32 fTo, granny_real32 fA,
                                 granny_real32 fB, granny_real32 fC,
                                 granny_real32 fD)
{
    if (TControl* p = Alive(pControl))
    {
        p->kEaseIn.fFrom = fFrom; p->kEaseIn.fTo = fTo; p->bEaseInCurve = true;
        p->kEaseIn.fA = fA; p->kEaseIn.fB = fB;
        p->kEaseIn.fC = fC; p->kEaseIn.fD = fD;
    }
}

/// Switches the ease-out weight on; see `GrannySetControlEaseIn`.
void GrannySetControlEaseOut(granny_control* pControl, bool bEnable)
{
    if (TControl* p = Alive(pControl))
        p->bEaseOut = bEnable;
}

/// See `GrannySetControlEaseInCurve`.
void GrannySetControlEaseOutCurve(granny_control* pControl, granny_real32 fFrom,
                                  granny_real32 fTo, granny_real32 fA,
                                  granny_real32 fB, granny_real32 fC,
                                  granny_real32 fD)
{
    if (TControl* p = Alive(pControl))
    {
        p->kEaseOut.fFrom = fFrom; p->kEaseOut.fTo = fTo; p->bEaseOutCurve = true;
        p->kEaseOut.fA = fA; p->kEaseOut.fB = fB;
        p->kEaseOut.fC = fC; p->kEaseOut.fD = fD;
    }
}

}  // extern "C"
