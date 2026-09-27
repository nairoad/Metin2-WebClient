// SPDX-License-Identifier: GPL-2.0-or-later
// frame_stats.h - where the time of one frame goes: per-job stopwatches and
// a per-period report (frames, FPS, worst frame, work vs waiting) whose
// parts must add up to the whole.

// Design:
// The client dropped to one frame per several seconds exactly when models
// appeared in the package. Four suspects at once: the custom draw path,
// pipeline assembly in the compatibility layer, skinning, and unoptimised
// game code. Each sounded plausible and each led to different work. The
// same shape as the mistake, where a 1 FPS drop was declared
// "panel throttling" because the frame itself computed in under a
// millisecond - the wrong thing was being measured (corrected).
// So this file does not guess: it measures and prints an account in which
// the sum of the parts has to match the total.
//
// Usage:
//     {
//         m2wstats::TStopwatch kStopwatch(m2wstats::g_kCustomDraw);
//         ...the work to measure...
//     }
// and once per main-loop iteration `M2W_BeginFrame()` / `M2W_MeasureFrame()`.
// The report goes out every 60 frames or every 5 s, whichever comes first,
// so that at one frame per several seconds the first line does not take a
// quarter of an hour.
//
// Cost: one clock read on entry and one on exit. `emscripten_get_now` is
// `performance.now()`, a JS call - too expensive to wrap around a single
// vertex, cheap enough to be invisible around a whole mesh. Hence the
// stopwatches sit around WHOLE jobs, never inside inner loops.

#pragma once

namespace m2wstats
{

/// One measured kind of work.
struct TCounter
{
    double dTime = 0.0;   ///< accumulated milliseconds
    long lCalls = 0;      ///< how many times it ran
    long lItems = 0;      ///< how many things it processed (vertices, meshes...)
};

/// High-resolution clock, milliseconds.
double Now();

extern TCounter g_kCustomDraw;       ///< our custom draw path (path B)
extern TCounter g_kPipelineSetup;    ///< pipeline assembly in the compatibility layer
extern TCounter g_kSkinning;         ///< skin: vertices through bones
extern TCounter g_kPose;             ///< skeleton pose assembly
extern TCounter g_kGr2Load;          ///< reading `.gr2` files
extern TCounter g_kShaderPrograms;   ///< shader program assembly
extern TCounter g_kTextureUploads;   ///< texture uploads to the GPU
extern TCounter g_kCorpusFetch;      ///< fetching files from the corpus (network)

/// Measures the time from construction to destruction and adds it to a counter.
class TStopwatch
{
public:
    /// Starts timing for `rCounter`.
    explicit TStopwatch(TCounter& rCounter)
        : m_rCounter(rCounter), m_dStart(Now()) {}

    /// Adds the elapsed time to the counter and counts one call.
    ~TStopwatch()
    {
        m_rCounter.dTime += Now() - m_dStart;
        ++m_rCounter.lCalls;
    }

private:
    TCounter& m_rCounter;
    double m_dStart;

    /// Not copyable: one stopwatch, one measurement.
    TStopwatch(const TStopwatch&);
    /// Not assignable (declared, never defined).
    TStopwatch& operator=(const TStopwatch&);
};

}  // namespace m2wstats

/// Starts a frame. Called at the BEGINNING of a main-loop iteration.
///
/// Why a separate begin when there is an end: without it the account
/// measured WALL time, report to report. At 60 FPS most of that is WAITING
/// for the next frame, not work, and the "REST" line came out huge (923 ms
/// of 1000 ms measured with twenty characters on screen) - looking like a
/// large unknown cost in the game, while it was mostly idleness. Same family
/// of mistake as above: a true number about the wrong thing. Now WORK is
/// measured - time spent inside the iteration - and the waiting share is
/// shown separately.
void M2W_BeginFrame();

/// Ends a frame: every 60th (or every 5 s) prints the account and resets the
/// counters. Called at the end of a main-loop iteration.
void M2W_MeasureFrame();
