// SPDX-License-Identifier: GPL-2.0-or-later
// stubs.cpp - registry of unimplemented calls (see stubs.h for the design).

#include "stubs.h"

#include <cstdio>
#include <cstring>

namespace
{

/// How many distinct stubs the registry can hold.
///
/// An array, not `std::map`: the registry sits on the drawing path, and a
/// memory allocation on the drawing path is exactly the kind of thing that
/// later gets described as "stutter of unknown origin". 120 slots for a
/// 49-file compatibility layer leaves headroom.
const int c_iSlots = 120;

TStubEntry g_akEntries[c_iSlots];
int        g_iUsed = 0;

/// Shared slot for everything that did not fit. It exists so that
/// `M2W_StubSlot` NEVER returns null - the macro increments the fields
/// without checking, and a null there would crash in the very place that
/// is meant to prevent silent failures.
TStubEntry g_kOverflow = { "(overflow)", 0, 0 };

char g_szBuffer[4096];

}  // namespace

TStubEntry* M2W_StubSlot(const char* c_szName)
{
    if (c_szName == 0)
        return &g_kOverflow;

    // This loop runs ONCE per call site, on its first hit - not per call.
    // That is why it may be linear and may even compare contents: two
    // literals with the same text can have different addresses in separate
    // translation units, and one stub must not become two counter rows.
    for (int i = 0; i < g_iUsed; ++i)
    {
        if (g_akEntries[i].c_szName == c_szName ||
            std::strcmp(g_akEntries[i].c_szName, c_szName) == 0)
        {
            return &g_akEntries[i];
        }
    }

    if (g_iUsed >= c_iSlots)
        return &g_kOverflow;

    // THE FIRST HIT IS REPORTED ON ITS OWN. The log then shows WHEN the game
    // first reached for something that is not there, and what it was doing
    // at that moment. Once per name, not once per call.
    g_akEntries[g_iUsed].c_szName = c_szName;
    g_akEntries[g_iUsed].lSinceStart = 0;
    g_akEntries[g_iUsed].lInPeriod = 0;
    ++g_iUsed;

    std::printf("m2w stub FIRST HIT: %s\n", c_szName);
    return &g_akEntries[g_iUsed - 1];
}

void M2W_StubsPrint()
{
    long lTotal = 0;
    int iActive = 0;
    for (int i = 0; i < g_iUsed; ++i)
    {
        if (g_akEntries[i].lInPeriod > 0)
        {
            lTotal += g_akEntries[i].lInPeriod;
            ++iActive;
        }
    }
    if (g_kOverflow.lInPeriod > 0)
    {
        lTotal += g_kOverflow.lInPeriod;
        ++iActive;
    }

    // Nothing in this period - do not clutter the counter. The missing line
    // is a result in itself: the game reached for nothing it lacks.
    if (iActive == 0)
        return;

    // The counter gets the FOUR MOST FREQUENT - a screenshot could not show
    // more legibly anyway, and the top four say where the hole is. The full
    // list exists (`M2W_StubsFullList`) but nothing calls it yet.
    std::printf("    STUBS %d kinds %ld calls:", iActive, lTotal);

    for (int iPlace = 0; iPlace < 4; ++iPlace)
    {
        int iTop = -1;
        for (int i = 0; i < g_iUsed; ++i)
        {
            if (g_akEntries[i].lInPeriod <= 0)
                continue;
            if (iTop < 0 || g_akEntries[i].lInPeriod > g_akEntries[iTop].lInPeriod)
                iTop = i;
        }
        if (iTop < 0)
            break;

        std::printf(" %s %ld", g_akEntries[iTop].c_szName,
                    g_akEntries[iTop].lInPeriod);
        g_akEntries[iTop].lInPeriod = -g_akEntries[iTop].lInPeriod;  // mark as printed
    }
    if (g_kOverflow.lInPeriod > 0)
        std::printf(" (+%ld overflow)", g_kOverflow.lInPeriod);
    std::printf("\n");

    for (int i = 0; i < g_iUsed; ++i)
        g_akEntries[i].lInPeriod = 0;
    g_kOverflow.lInPeriod = 0;
}

const char* M2W_StubsFullList()
{
    int iWritten = std::snprintf(g_szBuffer, sizeof(g_szBuffer),
                                 "stubs: %d kinds\n", g_iUsed);

    for (int i = 0; i < g_iUsed && iWritten < (int)sizeof(g_szBuffer) - 80; ++i)
    {
        iWritten += std::snprintf(g_szBuffer + iWritten, sizeof(g_szBuffer) - iWritten,
                                  "  %-40s %8ld\n",
                                  g_akEntries[i].c_szName, g_akEntries[i].lSinceStart);
    }
    return g_szBuffer;
}
