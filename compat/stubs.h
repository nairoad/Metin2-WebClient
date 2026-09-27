// SPDX-License-Identifier: GPL-2.0-or-later
// stubs.h - registry of what is NOT implemented: every stub reports itself
// once in the log when first hit and once per statistics period on the
// on-screen counter, at the cost of one pointer load and two increments.

// Design:
// This project paid twice for silent stubs (a `Levels = 0` texture that
// rendered black without an error; `CSpeedTreeRT::LoadTree` returning false
// and a world without trees, not one message) and once for an unattributed
// fix (several things changed at once, the stutter vanished and
// nobody knows which change did it). Hence: loud stubs.
//
// Cost: the first version searched the registry by
// name on EVERY call. The justification given for that being harmless was
// wrong - the quoted call site was unreachable - and the real per-frame path
// was never measured. The current design costs the same as the
// `static bool` it replaced: each call site keeps its own static pointer to
// its registry slot; the first call looks the slot up, every later call is
// one pointer load and two increments. No loop, no strcmp, no allocation.

#pragma once

/// One registry slot. Exposed in the header because the macro increments
/// the fields directly - that is the whole trick that makes counting free.
struct TStubEntry
{
    const char* c_szName;
    long lSinceStart;   ///< calls since the client started
    long lInPeriod;     ///< calls since the last frame-statistics report
};

/// Finds or creates the slot for `c_szName`. Called ONCE per call site, on
/// its first hit. Never returns null: when the registry is full it returns
/// the shared "overflow" slot.
TStubEntry* M2W_StubSlot(const char* c_szName);

/// Prints the `    STUBS ...` line for the on-screen counter and resets the
/// per-period counts. Called from `M2W_MeasureFrame()`, in the same rhythm
/// as the rest of the frame accounting. Prints nothing when no stub was hit
/// in the period - that silence is a result too.
void M2W_StubsPrint();

/// Full list: every name and its calls since start. No caller today (git
/// grep) - kept for the F12 console, where `M2W_StubsPrint` shows
/// only the four most frequent.
const char* M2W_StubsFullList();

/// Use inside a stub:
///
///     void CSpeedTreeRT::SetTreeSize(float, float)
///     {
///         M2W_STUB("SpeedTree::SetTreeSize");
///     }
///
/// The name should carry the FAMILY, not just the method: the counter shows
/// a dozen characters, and "SetTreeSize" alone does not say what is missing.
///
/// After the first call it costs one pointer compare, one load and two
/// increments.
#define M2W_STUB(name)                                              \
    do {                                                            \
        static TStubEntry* s_pEntry = 0;                            \
        if (s_pEntry == 0)                                          \
            s_pEntry = M2W_StubSlot(name);                          \
        ++s_pEntry->lSinceStart;                                    \
        ++s_pEntry->lInPeriod;                                      \
    } while (0)
