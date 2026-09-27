// SPDX-License-Identifier: GPL-2.0-or-later
// frame_stats.cpp - frame accounting (see frame_stats.h for the design).

#include "frame_stats.h"
#include "stubs.h"

#include <cstdio>

#include <emscripten/emscripten.h>

namespace m2wstats
{

double Now()
{
    return emscripten_get_now();
}

TCounter g_kCustomDraw;
TCounter g_kPipelineSetup;
TCounter g_kSkinning;
TCounter g_kPose;
TCounter g_kGr2Load;
TCounter g_kShaderPrograms;
TCounter g_kTextureUploads;
TCounter g_kCorpusFetch;

}  // namespace m2wstats

namespace
{
/// When the current loop iteration began. Zero means "not begun".
double g_dWorkStart = 0.0;

/// Time spent INSIDE loop iterations in the current period.
double g_dWork = 0.0;

/// Longest single loop iteration in the current period.
double g_dLongestWork = 0.0;
}

unsigned long g_ulM2wFrames = 0;

void M2W_BeginFrame()
{
    ++g_ulM2wFrames;
    g_dWorkStart = m2wstats::Now();
}

void M2W_MeasureFrame()
{
    using namespace m2wstats;

    if (g_dWorkStart > 0.0)
    {
        const double dThisWork = Now() - g_dWorkStart;
        g_dWork += dThisWork;
        if (dThisWork > g_dLongestWork)
            g_dLongestWork = dThisWork;
        g_dWorkStart = 0.0;
    }

    static double s_dPeriodStart = 0.0;
    static long s_lFrames = 0;

    static double s_dPrevious = 0.0;
    static double s_dWorst = 0.0;

    const double dNow = Now();
    if (s_dPeriodStart == 0.0)
    {
        s_dPeriodStart = dNow;
        s_dPrevious = dNow;
        return;
    }

    // THE WORST FRAME, not only the average.
    //
    // Stutter is not a low average - it is ONE frame that took twenty
    // seconds, hidden among sixty sixteen-millisecond ones. The average of
    // such a period comes out acceptable and lies about how it plays.
    const double dThisFrame = dNow - s_dPrevious;
    s_dPrevious = dNow;
    if (dThisFrame > s_dWorst)
        s_dWorst = dThisFrame;

    // EVERY 60 FRAMES OR EVERY 5 SECONDS - whichever comes first.
    //
    // The frame count alone is not enough: at one frame per several seconds
    // the first line would arrive after a quarter of an hour, when there is
    // long nothing left to measure. Time alone is not enough either: at
    // fifty frames per second a report every five seconds would lose what
    // only the per-frame account shows.
    ++s_lFrames;
    const double dPeriod = dNow - s_dPeriodStart;
    if (s_lFrames < 60 && dPeriod < 5000.0)
        return;

    const double dPerFrame = dPeriod / (double)s_lFrames;
    const double dMeasured = g_kCustomDraw.dTime + g_kPipelineSetup.dTime +
                             g_kSkinning.dTime + g_kPose.dTime +
                             g_kGr2Load.dTime + g_kShaderPrograms.dTime +
                             g_kTextureUploads.dTime + g_kCorpusFetch.dTime;

    std::printf(
        "m2w frame: %ld frames in %.0f ms = %.1f ms/frame (%.1f FPS), "
        "WORST %.1f ms\n"
        "    WORK %.0f ms of %.0f ms (%.0f%%), longest iteration %.1f ms - "
        "the rest is waiting for the next frame\n",
        s_lFrames, dPeriod, dPerFrame,
        (dPerFrame > 0.0) ? (1000.0 / dPerFrame) : 0.0, s_dWorst,
        g_dWork, dPeriod, (dPeriod > 0.0) ? (100.0 * g_dWork / dPeriod) : 0.0,
        g_dLongestWork);
    std::printf(
        "    custom draw  %8.1f ms / %6ld calls\n"
        "    D3D8 pipeline %7.1f ms / %6ld calls\n"
        "    skinning     %8.1f ms / %6ld calls\n"
        "    pose         %8.1f ms / %6ld calls\n"
        "    gr2 load     %8.1f ms / %6ld files\n"
        "    programs     %8.1f ms / %6ld builds\n"
        "    textures     %8.1f ms / %6ld uploads\n"
        "    corpus       %8.1f ms / %6ld fetches\n"
        "    REST OF WORK %8.1f ms  <- game, scripts, everything else\n",
        g_kCustomDraw.dTime, g_kCustomDraw.lCalls,
        g_kPipelineSetup.dTime, g_kPipelineSetup.lCalls,
        g_kSkinning.dTime, g_kSkinning.lCalls,
        g_kPose.dTime, g_kPose.lCalls,
        g_kGr2Load.dTime, g_kGr2Load.lCalls,
        g_kShaderPrograms.dTime, g_kShaderPrograms.lCalls,
        g_kTextureUploads.dTime, g_kTextureUploads.lCalls,
        g_kCorpusFetch.dTime, g_kCorpusFetch.lCalls,
        g_dWork - dMeasured);

    // WHAT IS MISSING - in the same rhythm as the time account. The account
    // says where the time goes; this line says what the game reached for in
    // vain. Without it "test everything at the end" ends with "something
    // does not work" and no pointer to what (see stubs.h).
    M2W_StubsPrint();

    g_kCustomDraw = TCounter();
    g_kPipelineSetup = TCounter();
    g_kSkinning = TCounter();
    g_kPose = TCounter();
    g_kGr2Load = TCounter();
    g_kShaderPrograms = TCounter();
    g_kTextureUploads = TCounter();
    g_kCorpusFetch = TCounter();
    s_lFrames = 0;
    s_dPeriodStart = dNow;
    s_dWorst = 0.0;
    g_dWork = 0.0;
    g_dLongestWork = 0.0;
}
