// SPDX-License-Identifier: GPL-2.0-or-later
// sound_web.cpp - the client's sound on Web Audio instead of Miles Sound
// System: the body of our `milesLib/SoundManager.h` and `SoundData.h`
// replacing the nine `milesLib` files that stand on `MSS.H`.

// Design:
// Everything that is ARITHMETIC - the volume curve, the distance scales,
// the music state machine, slot allocation, the rate limit of character
// sounds - is copied from `milesLib/SoundManager.cpp` number for number.
// Those constants are audible: the player has a volume slider and a habit.
// Our own decisions are only where Miles ended and the sound card began;
// the browser side is `m2w.sound*` in runtime.js.
//
// WHICH SPATIAL SOUND IT WAS. `SoundManager3D.cpp` walks the list of Miles
// providers; seven are commented out (`Full HRTF`, `Dolby Surround`...) and
// ONE is chosen: `"Miles Fast 2D Positional Audio"`. That is not
// head-filtered 3D sound but PAN AND VOLUME - left/right plus fading with
// distance - exactly what the browser's `PannerNode` does in `equalpower`
// mode. Choosing that mode is not a simplification, it REPRODUCES what the
// player heard.
//
// COORDINATES. Miles got every coordinate from TMP4 with Z NEGATED: the
// listener (`AIL_set_3D_position(m_pListener, fX, fY, -fZ)`), the sources
// (`CSoundInstance3D::SetPosition`) and the view direction. The same rigid
// transform applied to the listener AND the sources keeps the relative
// geometry whole, so there is no need to know whether "up" is Y or Z in
// Metin2's world; the port negates Z in the same three places.
//
// DISTANCE SCALE. TMP4 sets Miles neither a rolloff nor a minimum
// distance; instead it DIVIDES positions by `m_fSoundScale` (200) before
// passing them, so 200 world units land at distance 1.0 - the unit distance
// from which Miles starts fading. Translated directly: `distanceModel =
// 'inverse'`, `refDistance = 1`, `rolloffFactor = 1` (volume 1 up to
// distance 1, then 1/d); the constant 200 stays where it was, in the C++
// division. What is NOT recovered: the exact Miles curve beyond the unit
// distance, which lived in Miles defaults TMP4 never writes down. `1/d` is
// the honest choice (same shape, same reference distance) but a CHOICE -
// if something in the game turns out too quiet or too loud with distance,
// this is the place.
//
// THREE THINGS THE BROWSER IMPOSES THAT MILES DID NOT HAVE:
// 1. Decoding is asynchronous (`decodeAudioData` returns a promise); Miles
//    read the file and played it in the same call. The first play of an
//    unknown file is DELAYED by the decode (milliseconds), every later one
//    is immediate because the buffer is cached. Better than losing the
//    first sound, which a literal `SetInstance -> -1` would do.
// 2. Autoplay is blocked until the first gesture: the `AudioContext` stays
//    `suspended` until the player clicks or presses something. A one-shot
//    listener on the first pointer or keyboard event resumes it.
// 3. Sources are single-use: an `AudioBufferSourceNode` is disposable after
//    `stop`, a Miles `HSAMPLE` could be restarted. So a slot is a permanent
//    PAIR of nodes (gain + panner) and a source is created at every play.
//
// Known differences from TMP4 that were already in the port and
// stay: one allocation counter shared by the 2D and 3D managers (TMP4 had
// one `static DWORD k` per manager) and no `start > 50000` guard of
// `CSoundManager3D::GetInstance`; `m2w.sound` keeps one buffer per file
// while Miles kept the file bytes for the whole game.

#include "win32_compat.h"

#include <emscripten.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "milesLib/SoundManager.h"
#include "EterPack/EterPackManager.h"
#include "eterBase/MappedFile.h"
#include "eterBase/Timer.h"
#include "eterBase/Debug.h"
#include "eterBase/Filename.h"
#include "eterBase/Utils.h"
#include "eterBase/CRC32.h"

// ---------------------------------------------------------------------------
// Bridges to m2w.sound* (runtime.js)
// ---------------------------------------------------------------------------

/// Creates the AudioContext and `iSlots` node pairs; a second call is a no-op.
EM_JS(void, m2w_sound_init, (int iSlots), { m2w.soundStart(iSlots); });

/// 1 = buffer ready, 0 = decoding, -1 = cannot. `slice` COPIES: the decoder
/// detaches the buffer it gets and the wasm heap must never be detached.
EM_JS(int, m2w_sound_load, (unsigned int uCrc, const void* c_pvData, int iBytes), {
    return m2w.soundLoad(uCrc, HEAPU8.slice(c_pvData, c_pvData + iBytes).buffer);
});

/// `iLoops` as Miles counted: 0 forever, 1 once, n times; `iSpatial` = 1
/// routes through the panner.
EM_JS(void, m2w_sound_play, (int iSlot, unsigned int uCrc, int iLoops, int iSpatial), {
    m2w.soundPlay(iSlot, uCrc, iLoops, iSpatial);
});

/// Stops the slot; a decode it waited for no longer starts it.
EM_JS(void, m2w_sound_stop, (int iSlot), { m2w.soundStop(iSlot); });

/// 1 when nothing plays in the slot and nothing waits for a decode.
EM_JS(int, m2w_sound_slot_free, (int iSlot), { return m2w.soundSlotFree(iSlot); });

/// Gain of the slot, 0..1 (the curve is applied in C++).
EM_JS(void, m2w_sound_volume, (int iSlot, float fVolume), { m2w.soundVolume(iSlot, fVolume); });

/// Source position relative to the listener, already divided by the scale
/// and with Z negated.
EM_JS(void, m2w_sound_position, (int iSlot, float fX, float fY, float fZ), {
    m2w.soundPosition(iSlot, fX, fY, fZ);
});

/// Listener position alone: TMP4 has it apart from the direction
/// (`SetListenerPosition` vs `SetListenerDirection`) and calls them from
/// different places, so here too they are two functions.
EM_JS(void, m2w_sound_listener_position, (float fX, float fY, float fZ), {
    m2w.soundListenerPosition(fX, fY, fZ);
});

/// Listener direction alone - the counterpart of `AIL_set_3D_orientation`.
EM_JS(void, m2w_sound_listener_direction, (float fFwdX, float fFwdY, float fFwdZ,
                                           float fUpX, float fUpY, float fUpZ), {
    m2w.soundListenerDirection(fFwdX, fFwdY, fFwdZ, fUpX, fUpY, fUpZ);
});

namespace
{

// --- slots: numbers copied from the milesLib headers ------------------------
// `CSoundManager3D::INSTANCE_MAX_COUNT` = 32, `CSoundManager2D` = 4,
// `CSoundManagerStream::MUSIC_INSTANCE_MAX_NUM` = 3. These numbers are
// AUDIBLE: the thirty-third spatial sound in the original simply did not
// play.
const int c_iSlots3D    = 32;
const int c_iSlots2D    = 4;
const int c_iSlotsMusic = 3;

const int c_iBase3D     = 0;
const int c_iBase2D     = c_iBase3D + c_iSlots3D;
const int c_iBaseMusic  = c_iBase2D + c_iSlots2D;
const int c_iSlotsTotal = c_iBaseMusic + c_iSlotsMusic;

// --- file cache: the counterpart of `CSoundBase::ms_dataMap` ---------------
// The original keeps a map CRC of the file name -> `CSoundData`. Exactly
// that stays, with the same hash (`GetCRC32` of the name), because it
// decides whether two spellings of the same name hit the same buffer.

/// One file the browser has been asked to decode.
struct TSoundFile
{
    std::string strName;
    int         iState;   // -1 cannot, 0 decoding, 1 ready
};

std::map<DWORD, TSoundFile> s_mapFiles;

/// `CSoundData::SetPackMode` was called - a record of intent, see there.
bool s_bPackMode = false;

/// Reads a file and hands it to the browser for decoding.
/// Returns its CRC, or 0 when the file does not exist.
DWORD LoadFile(const char* c_szName)
{
    const DWORD dwCrc = GetCRC32(c_szName, static_cast<int>(strlen(c_szName)));

    std::map<DWORD, TSoundFile>::iterator it = s_mapFiles.find(dwCrc);
    if (it != s_mapFiles.end())
        return (it->second.iState < 0) ? 0 : dwCrc;

    // The original read through Miles callbacks that pointed either at the
    // pack or at the disk (`SetPackMode`). Here both roads go through
    // `CEterPackManager::Get`, which chooses by itself - see `SetPackMode`.
    CMappedFile kFile;
    LPCVOID pvData = NULL;
    if (!CEterPackManager::Instance().Get(kFile, c_szName, &pvData))
    {
        TraceError("CSoundManager: no sound file %s", c_szName);
        TSoundFile kBad;
        kBad.strName = c_szName;
        kBad.iState = -1;
        s_mapFiles[dwCrc] = kBad;
        return 0;
    }

    TSoundFile kNew;
    kNew.strName = c_szName;
    kNew.iState = m2w_sound_load(dwCrc, pvData, static_cast<int>(kFile.Size()));
    s_mapFiles[dwCrc] = kNew;

    // `kFile` dies here, as it should: `m2w_sound_load` copied the bytes to
    // the browser, so our copy is no longer needed. Miles kept it for the
    // whole game - this is a saving, not a loss.
    return (kNew.iState < 0) ? 0 : dwCrc;
}

// --- slots on the C++ side --------------------------------------------------
// Only what the browser does not know: which file plays in which slot. The
// rest (whether it plays, how loud) lives on the other side.
DWORD s_adwSlotCrc[c_iSlotsTotal];

/// The counterpart of `CSoundManager3D::SetInstance` and
/// `CSoundManager2D::GetInstance`: a rotating counter `k`, the first FREE
/// slot, and when there is none the sound is dropped - as in the original
/// (with the two differences named in the header).
int AllocateSlot(int iBase, int iCount)
{
    static DWORD k = 0;

    DWORD dwStart = k++;
    const DWORD dwEnd = dwStart + static_cast<DWORD>(iCount);

    while (dwStart < dwEnd)
    {
        const int iSlot = iBase + static_cast<int>(dwStart % static_cast<DWORD>(iCount));
        if (m2w_sound_slot_free(iSlot))
            return iSlot;
        ++dwStart;
    }

    return -1;
}

// --- manager state ----------------------------------------------------------
// The header has no fields - a decision: in the port the object
// layout is ours, nobody outside writes it. So the state lives here, with
// the initial values copied from the TMP4 constructor.

BOOL  s_bSoundDisable        = FALSE;
float s_fxPosition           = 0.0f;
float s_fyPosition           = 0.0f;
float s_fzPosition           = 0.0f;
float s_fSoundScale          = 200.0f;    // TMP4: m_fSoundScale
float s_fAmbienceSoundScale  = 1000.0f;   // TMP4: m_fAmbienceSoundScale
float s_fSoundVolume         = 1.0f;
float s_fMusicVolume         = 1.0f;
float s_fBackupSoundVolume   = 0.0f;
float s_fBackupMusicVolume   = 0.0f;

enum EMusicState
{
    MUSIC_STATE_OFF,
    MUSIC_STATE_PLAY,
    MUSIC_STATE_FADE_IN,
    MUSIC_STATE_FADE_OUT,
    MUSIC_STATE_FADE_LIMIT_OUT,
};

/// TMP4: `CSoundManager::TMusicInstance`, field for field.
struct TMusicInstance
{
    DWORD       dwMusicFileNameCRC;
    EMusicState MusicState;
    float       fVolume;
    float       fVolumeSpeed;
    float       fLimitVolume;
};

TMusicInstance s_akMusic[c_iSlotsMusic];

std::map<std::string, float> s_mapHistory;   // TMP4: m_PlaySoundHistoryMap

/// TMP4: `__ConvertRatioVolumeToApplyVolume`, copied to the character -
/// this is the curve of the volume slider the player has in their fingers.
float RatioToVolume(float fRatio)
{
    if (0.1f > fRatio)
        return fRatio;

    return static_cast<float>(pow(10.0f, (-1.0f + fRatio)));
}

/// TMP4: `__ConvertGradeVolumeToApplyVolume`. Grades are 0..5.
float GradeToVolume(int iGrade)
{
    return RatioToVolume(iGrade / 5.0f);
}

void SetMusicVolumeInternal(float fVolume);
void StopMusic(DWORD dwIndex);
void StartMusic(DWORD dwIndex, const char* c_szFileName, float fVolume,
                float fVolumeSpeed);
BOOL FindMusic(const char* c_szFileName, DWORD* pdwIndex);

}  // namespace

// ---------------------------------------------------------------------------
// CSoundData
// ---------------------------------------------------------------------------

/// In TMP4 this installed four Miles callbacks so that it read from the
/// game pack instead of the disk. Here the road to a file is ONE -
/// `CEterPackManager`, which knows by itself whether a thing sits in a pack
/// or lies loose - so only the record of intent remains. Not a "does
/// nothing" stub: should the two roads ever be split again, this flag says
/// the client asked for it.
void CSoundData::SetPackMode()
{
    s_bPackMode = true;
}

// ---------------------------------------------------------------------------
// CSoundManager - arithmetic copied from milesLib/SoundManager.cpp
// ---------------------------------------------------------------------------

CSoundManager::CSoundManager()
{
    s_bSoundDisable       = FALSE;
    s_fxPosition          = 0.0f;
    s_fyPosition          = 0.0f;
    s_fzPosition          = 0.0f;
    s_fSoundScale         = 200.0f;
    s_fAmbienceSoundScale = 1000.0f;
    s_fSoundVolume        = 1.0f;
    s_fMusicVolume        = 1.0f;
    s_fBackupMusicVolume  = 0.0f;
    s_fBackupSoundVolume  = 0.0f;

    for (int i = 0; i < c_iSlotsMusic; ++i)
    {
        s_akMusic[i].dwMusicFileNameCRC = 0;
        s_akMusic[i].MusicState = MUSIC_STATE_OFF;
        s_akMusic[i].fVolume = 0.0f;
        s_akMusic[i].fVolumeSpeed = 0.0f;
        s_akMusic[i].fLimitVolume = 0.0f;
    }

    for (int i = 0; i < c_iSlotsTotal; ++i)
        s_adwSlotCrc[i] = 0;
}

CSoundManager::~CSoundManager()
{
}

/// Creates the browser side (`m2w.soundStart`). Always succeeds: with no
/// Web Audio the game runs silent, as it did with no sound card.
BOOL CSoundManager::Create()
{
    m2w_sound_init(c_iSlotsTotal);
    return TRUE;
}

void CSoundManager::Destroy()
{
    for (int i = 0; i < c_iSlotsTotal; ++i)
        m2w_sound_stop(i);

    s_mapFiles.clear();
    s_mapHistory.clear();   // TMP4 has the same line here, marked "Fix"
}

/// Listener position, kept in C++: sources are passed RELATIVE to it (see
/// `PlaySound3D`) while the browser listener sits at the origin.
void CSoundManager::SetPosition(float fx, float fy, float fz)
{
    s_fxPosition = fx;
    s_fyPosition = fy;
    s_fzPosition = fz;
}

/// The same sign flip on Z that TMP4 did on the way into Miles.
void CSoundManager::SetDirection(float fxDir, float fyDir, float fzDir,
                                 float fxUp, float fyUp, float fzUp)
{
    m2w_sound_listener_direction(fxDir, fyDir, -fzDir,
                                 fxUp,  fyUp,  -fzUp);
}

/// Once per frame: the listener pinned to the origin (TMP4 does that EVERY
/// frame and passes sources relatively; otherwise the division by the
/// scale would have to change too) and the music fades. The direction is
/// NOT touched: `SetDirection` sets it separately, and TMP4 called only
/// `SetListenerPosition` here.
void CSoundManager::Update()
{
    m2w_sound_listener_position(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < c_iSlotsMusic; ++i)
    {
        TMusicInstance& r = s_akMusic[i];
        if (MUSIC_STATE_OFF == r.MusicState)
            continue;

        switch (r.MusicState)
        {
            case MUSIC_STATE_FADE_IN:
                r.fVolume += r.fVolumeSpeed;
                if (r.fVolume >= GetMusicVolume())
                {
                    r.fVolume = GetMusicVolume();
                    r.fVolumeSpeed = 0.0f;
                    r.MusicState = MUSIC_STATE_PLAY;
                }
                m2w_sound_volume(c_iBaseMusic + i, r.fVolume);
                break;

            case MUSIC_STATE_FADE_LIMIT_OUT:
                r.fVolume -= r.fVolumeSpeed;
                if (r.fVolume <= r.fLimitVolume)
                {
                    r.fVolume = r.fLimitVolume;
                    r.fVolumeSpeed = 0.0f;
                    r.MusicState = MUSIC_STATE_PLAY;
                }
                m2w_sound_volume(c_iBaseMusic + i, r.fVolume);
                break;

            case MUSIC_STATE_FADE_OUT:
                r.fVolume -= r.fVolumeSpeed;
                if (r.fVolume <= 0.0f)
                {
                    r.fVolume = 0.0f;
                    r.fVolumeSpeed = 0.0f;
                    r.MusicState = MUSIC_STATE_OFF;
                    StopMusic(static_cast<DWORD>(i));
                }
                m2w_sound_volume(c_iBaseMusic + i, r.fVolume);
                break;

            default:
                break;
        }
    }
}

// --- sound nodes in animations ---------------------------------------------
// NOTE: both forms of `UpdateSoundData` in TMP4 are `assert(!"...")` - DEAD
// code that would kill the program if called. Nobody calls it: the
// animation road goes through `UpdateSoundInstance` (ActorInstanceMotionEvent,
// EffectInstance). They stay empty instead of asserting: an assertion in
// the browser has nobody to tell, and such a call would be one that has not
// happened in twenty years.

void CSoundManager::UpdateSoundData(DWORD, const NSound::TSoundDataVector*)
{
}

void CSoundManager::UpdateSoundData(float, float, float, DWORD,
                                    const NSound::TSoundDataVector*)
{
}

/// Plays every sound of the animation frame `dwcurFrame` at the actor.
void CSoundManager::UpdateSoundInstance(float fx, float fy, float fz,
                                        DWORD dwcurFrame,
                                        const NSound::TSoundInstanceVector* c_pVector,
                                        BOOL bCheckFrequency)
{
    for (DWORD i = 0; i < c_pVector->size(); ++i)
    {
        const NSound::TSoundInstance& c_r = c_pVector->at(i);
        if (c_r.dwFrame == dwcurFrame)
            PlayCharacterSound3D(fx, fy, fz, c_r.strSoundFileName.c_str(),
                                 bCheckFrequency);
    }
}

/// The 2D form (effects without a position).
void CSoundManager::UpdateSoundInstance(DWORD dwcurFrame,
                                        const NSound::TSoundInstanceVector* c_pVector)
{
    for (DWORD i = 0; i < c_pVector->size(); ++i)
    {
        const NSound::TSoundInstance& c_r = c_pVector->at(i);
        if (c_r.dwFrame == dwcurFrame)
            PlaySound2D(c_r.strSoundFileName.c_str());
    }
}

// --- scales and volumes -----------------------------------------------------

float CSoundManager::GetSoundScale()               { return s_fSoundScale; }
void  CSoundManager::SetSoundScale(float f)        { s_fSoundScale = f; }
void  CSoundManager::SetAmbienceSoundScale(float f){ s_fAmbienceSoundScale = f; }
float CSoundManager::GetSoundVolume()              { return s_fSoundVolume; }
float CSoundManager::GetMusicVolume()              { return s_fMusicVolume; }

/// Clamped to 0..1; while muted (`SaveVolume`) only the backup is written.
void CSoundManager::SetSoundVolume(float fVolume)
{
    if (s_bSoundDisable)
    {
        s_fBackupSoundVolume = fVolume;
        return;
    }

    fVolume = fMAX(fVolume, 0.0f);
    fVolume = fMIN(fVolume, 1.0f);
    s_fSoundVolume = fVolume;
    s_fBackupSoundVolume = fVolume;
}

namespace
{

/// TMP4: `__SetMusicVolume` - also applies the volume to every music slot
/// that is not off and not fading out.
void SetMusicVolumeInternal(float fVolume)
{
    if (s_bSoundDisable)
    {
        s_fBackupMusicVolume = fVolume;
        return;
    }

    fVolume = fMAX(fVolume, 0.0f);
    fVolume = fMIN(fVolume, 1.0f);
    s_fMusicVolume = fVolume;
    s_fBackupMusicVolume = fVolume;

    for (int i = 0; i < c_iSlotsMusic; ++i)
    {
        TMusicInstance& r = s_akMusic[i];
        if (MUSIC_STATE_OFF == r.MusicState)      continue;
        if (MUSIC_STATE_FADE_OUT == r.MusicState) continue;

        r.fVolume = fVolume;
        m2w_sound_volume(c_iBaseMusic + i, fVolume);
    }
}

}  // namespace

void CSoundManager::SetMusicVolume(float fVolume)
{
    SetMusicVolumeInternal(fVolume);
}

void CSoundManager::SetSoundVolumeRatio(float fRatio)
{
    SetSoundVolume(RatioToVolume(fRatio));
}

void CSoundManager::SetSoundVolumeGrade(int iGrade)
{
    SetSoundVolume(GradeToVolume(iGrade));
}

/// TMP4 DECLARES these two in `SoundManager.h` but defines neither:
/// `SetMusicVolumeGrade` is commented out in the `.cpp`, `SetMusicVolumeRatio`
/// is absent. Nothing calls them, so the TMP4 linker never tripped. They are
/// written here because a header promising a method without a body is a
/// lying header; the body is the commented-out TMP4 line - what TMP4's author
/// meant before switching it off.
void CSoundManager::SetMusicVolumeRatio(float fRatio)
{
    SetMusicVolumeInternal(RatioToVolume(fRatio));
}

/// See `SetMusicVolumeRatio`.
void CSoundManager::SetMusicVolumeGrade(int iGrade)
{
    SetMusicVolumeInternal(GradeToVolume(iGrade));
}

/// Mutes everything and remembers the volumes (window loses focus,
/// PythonApplicationProcedure). TMP4: a second save must do nothing, so as
/// not to overwrite the backup with zeros.
void CSoundManager::SaveVolume()
{
    if (s_bSoundDisable)
        return;

    const float fBackupMusicVolume = s_fMusicVolume;
    const float fBackupSoundVolume = s_fSoundVolume;
    SetMusicVolumeInternal(0.0f);
    SetSoundVolume(0.0f);
    s_fBackupMusicVolume = fBackupMusicVolume;
    s_fBackupSoundVolume = fBackupSoundVolume;
    s_bSoundDisable = TRUE;
}

/// Undoes `SaveVolume`.
void CSoundManager::RestoreVolume()
{
    s_bSoundDisable = FALSE;
    SetMusicVolumeInternal(s_fBackupMusicVolume);
    SetSoundVolume(s_fBackupSoundVolume);
}

// --- sound ------------------------------------------------------------------

/// One play, no position, in one of the 4 2D slots.
void CSoundManager::PlaySound2D(const char* c_szFileName)
{
    if (0.0f == GetSoundVolume())
        return;

    const DWORD dwCrc = LoadFile(c_szFileName);
    if (!dwCrc)
        return;

    const int iSlot = AllocateSlot(c_iBase2D, c_iSlots2D);
    if (iSlot < 0)
        return;

    s_adwSlotCrc[iSlot] = dwCrc;
    m2w_sound_volume(iSlot, GetSoundVolume());
    m2w_sound_play(iSlot, dwCrc, 1, 0);
}

/// Position relative to the listener, divided by the sound scale, Z
/// negated - the three things TMP4 did on the way into Miles.
void CSoundManager::PlaySound3D(float fx, float fy, float fz,
                                const char* c_szFileName, int iPlayCount)
{
    if (0.0f == GetSoundVolume())
        return;

    const DWORD dwCrc = LoadFile(c_szFileName);
    if (!dwCrc)
        return;

    const int iSlot = AllocateSlot(c_iBase3D, c_iSlots3D);
    if (iSlot < 0)
        return;

    s_adwSlotCrc[iSlot] = dwCrc;
    m2w_sound_position(iSlot,
                       (fx - s_fxPosition) / s_fSoundScale,
                       (fy - s_fyPosition) / s_fSoundScale,
                       -((fz - s_fzPosition) / s_fSoundScale));
    m2w_sound_volume(iSlot, GetSoundVolume());
    m2w_sound_play(iSlot, dwCrc, iPlayCount, 1);
}

/// Like `PlaySound3D` with the ambience scale (1000). Returns the SLOT
/// NUMBER as TMP4 did - `GameLib/Area.cpp` keeps it to call `StopSound3D`
/// and `SetSoundVolume3D` later, so the numbering has to be the same: the
/// base is subtracted.
int CSoundManager::PlayAmbienceSound3D(float fx, float fy, float fz,
                                       const char* c_szFileName, int iPlayCount)
{
    if (0.0f == GetSoundVolume())
        return -1;

    const DWORD dwCrc = LoadFile(c_szFileName);
    if (!dwCrc)
        return -1;

    const int iSlot = AllocateSlot(c_iBase3D, c_iSlots3D);
    if (iSlot < 0)
        return -1;

    s_adwSlotCrc[iSlot] = dwCrc;
    m2w_sound_position(iSlot,
                       (fx - s_fxPosition) / s_fAmbienceSoundScale,
                       (fy - s_fyPosition) / s_fAmbienceSoundScale,
                       -((fz - s_fzPosition) / s_fAmbienceSoundScale));
    m2w_sound_volume(iSlot, GetSoundVolume());
    m2w_sound_play(iSlot, dwCrc, iPlayCount, 1);

    return iSlot - c_iBase3D;
}

/// Character sounds with the rate limit copied constant for constant: the
/// distance is measured in the PLANE only (no Z) against 5000 units
/// squared (no square root), and the same file plays at most every 0.3 s.
void CSoundManager::PlayCharacterSound3D(float fx, float fy, float fz,
                                         const char* c_szFileName,
                                         BOOL bCheckFrequency)
{
    if (0.0f == GetSoundVolume())
        return;

    if (bCheckFrequency)
    {
        static const float s_fLimitDistance = 5000.0f * 5000.0f;
        const float fdx = (fx - s_fxPosition) * (fx - s_fxPosition);
        const float fdy = (fy - s_fyPosition) * (fy - s_fyPosition);

        if (fdx + fdy > s_fLimitDistance)
            return;

        std::map<std::string, float>::iterator it = s_mapHistory.find(c_szFileName);
        if (s_mapHistory.end() != it)
        {
            if (CTimer::Instance().GetCurrentSecond() - it->second < 0.3f)
                return;
        }

        s_mapHistory[c_szFileName] = CTimer::Instance().GetCurrentSecond();
    }

    const DWORD dwCrc = LoadFile(c_szFileName);
    if (!dwCrc)
        return;

    const int iSlot = AllocateSlot(c_iBase3D, c_iSlots3D);
    if (iSlot < 0)
        return;

    s_adwSlotCrc[iSlot] = dwCrc;
    m2w_sound_position(iSlot,
                       (fx - s_fxPosition) / s_fSoundScale,
                       (fy - s_fyPosition) / s_fSoundScale,
                       -((fz - s_fzPosition) / s_fSoundScale));
    m2w_sound_volume(iSlot, GetSoundVolume());
    m2w_sound_play(iSlot, dwCrc, 1, 1);
}

/// `iIndex` is the number `PlayAmbienceSound3D` returned.
void CSoundManager::StopSound3D(int iIndex)
{
    if (iIndex < 0 || iIndex >= c_iSlots3D)
        return;
    m2w_sound_stop(c_iBase3D + iIndex);
}

/// `iIndex` as in `StopSound3D`.
void CSoundManager::SetSoundVolume3D(int iIndex, float fVolume)
{
    if (iIndex < 0 || iIndex >= c_iSlots3D)
        return;
    m2w_sound_volume(c_iBase3D + iIndex, fVolume);
}

void CSoundManager::StopAllSound3D()
{
    for (int i = 0; i < c_iSlots3D; ++i)
        StopSound3D(i);

    s_mapHistory.clear();
}

// --- music ------------------------------------------------------------------

namespace
{

/// Which music slot plays this file (by the case-insensitive CRC of the
/// normalised path, as TMP4 keys `dwMusicFileNameCRC`).
BOOL FindMusic(const char* c_szFileName, DWORD* pdwIndex)
{
    std::string strFileName;
    StringPath(c_szFileName, strFileName);
    const DWORD dwCRC = GetCaseCRC32(strFileName.c_str(),
                                     static_cast<int>(strFileName.length()));

    for (int i = 0; i < c_iSlotsMusic; ++i)
    {
        const TMusicInstance& c_r = s_akMusic[i];
        if (MUSIC_STATE_OFF != c_r.MusicState && c_r.dwMusicFileNameCRC == dwCRC)
        {
            *pdwIndex = static_cast<DWORD>(i);
            return TRUE;
        }
    }

    return FALSE;
}

/// Stops music slot `dwIndex` at once (no fade) and marks it OFF; an index out
/// of range is ignored.
void StopMusic(DWORD dwIndex)
{
    if (dwIndex >= c_iSlotsMusic)
        return;

    m2w_sound_stop(c_iBaseMusic + static_cast<int>(dwIndex));

    TMusicInstance& r = s_akMusic[dwIndex];
    r.fVolume = 0.0f;
    r.fVolumeSpeed = 0.0f;
    r.MusicState = MUSIC_STATE_OFF;
    r.dwMusicFileNameCRC = 0;
}

/// Starts a file looping in a music slot and puts the slot in FADE_IN.
void StartMusic(DWORD dwIndex, const char* c_szFileName, float fVolume,
                float fVolumeSpeed)
{
    if (dwIndex >= c_iSlotsMusic)
        return;

    const DWORD dwCrc = LoadFile(c_szFileName);
    if (!dwCrc)
    {
        TraceError("CSoundManager::PlayMusic - cannot load: %s", c_szFileName);
        return;
    }

    const int iSlot = c_iBaseMusic + static_cast<int>(dwIndex);
    s_adwSlotCrc[iSlot] = dwCrc;
    m2w_sound_volume(iSlot, fVolume);
    m2w_sound_play(iSlot, dwCrc, 0, 0);   // 0 loops = forever, as `Play(0)` in Miles

    TMusicInstance& r = s_akMusic[dwIndex];
    r.fVolume = fVolume;
    r.fVolumeSpeed = fVolumeSpeed;
    r.MusicState = MUSIC_STATE_FADE_IN;

    std::string strFileName;
    StringPath(c_szFileName, strFileName);
    r.dwMusicFileNameCRC = GetCaseCRC32(strFileName.c_str(),
                                        static_cast<int>(strFileName.length()));
}

}  // namespace

void CSoundManager::PlayMusic(const char* c_szFileName)
{
    StartMusic(0, c_szFileName, GetMusicVolume(), 0.0f);
}

/// A file already playing is only switched back to FADE_IN; otherwise
/// everything else fades out and the first free slot starts it from 0.
/// TMP4: with all three slots busy the music simply does not start (the
/// "evict slot zero" variant is commented out in TMP4).
void CSoundManager::FadeInMusic(const char* c_szFileName, float fVolumeSpeed)
{
    DWORD dwIndex;
    if (FindMusic(c_szFileName, &dwIndex))
    {
        s_akMusic[dwIndex].MusicState = MUSIC_STATE_FADE_IN;
        s_akMusic[dwIndex].fVolumeSpeed = fVolumeSpeed;
        return;
    }

    FadeOutAllMusic();

    for (int i = 0; i < c_iSlotsMusic; ++i)
    {
        if (MUSIC_STATE_OFF != s_akMusic[i].MusicState)
            continue;

        StartMusic(static_cast<DWORD>(i), c_szFileName, 0.0f, fVolumeSpeed);
        return;
    }
}

void CSoundManager::FadeOutMusic(const char* c_szFileName, float fVolumeSpeed)
{
    DWORD dwIndex;
    if (!FindMusic(c_szFileName, &dwIndex))
    {
        Tracenf("FadeOutMusic: %s - ERROR NOT EXIST", c_szFileName);
        return;
    }

    s_akMusic[dwIndex].MusicState = MUSIC_STATE_FADE_OUT;
    s_akMusic[dwIndex].fVolumeSpeed = fVolumeSpeed;
}

/// Fades down to `fLimitVolume` (a slider ratio, put through the curve)
/// and keeps playing there.
void CSoundManager::FadeLimitOutMusic(const char* c_szFileName, float fLimitVolume,
                                      float fVolumeSpeed)
{
    DWORD dwIndex;
    if (!FindMusic(c_szFileName, &dwIndex))
    {
        Tracenf("FadeLimitOutMusic: %s - ERROR NOT EXIST", c_szFileName);
        return;
    }

    TMusicInstance& r = s_akMusic[dwIndex];
    r.MusicState = MUSIC_STATE_FADE_LIMIT_OUT;
    r.fVolumeSpeed = fVolumeSpeed;
    r.fLimitVolume = RatioToVolume(fLimitVolume);
}

void CSoundManager::FadeOutAllMusic()
{
    for (int i = 0; i < c_iSlotsMusic; ++i)
    {
        if (MUSIC_STATE_OFF == s_akMusic[i].MusicState)
            continue;

        s_akMusic[i].MusicState = MUSIC_STATE_FADE_OUT;
        s_akMusic[i].fVolumeSpeed = 0.01f;   // TMP4 has the constant inline here
    }
}

void CSoundManager::FadeAll()
{
    FadeOutAllMusic();
}
