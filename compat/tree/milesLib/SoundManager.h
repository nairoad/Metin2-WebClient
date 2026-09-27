// milesLib/SoundManager.h - OUR header in place of the TMP4 one.
//
// WHY at all: the original pulls in `SoundManager2D.h`, `SoundManager3D.h`
// and `SoundManagerStream.h`, and those pull in `MSS.H` - the header of **Miles Sound
// System**, a commercial library. Under emscripten `MSS.H` ends
// outright: `#error MSS.H did not detect your platform`.
//
// THE MEASUREMENT that says this is not a problem:
//   - `GameLib` uses **zero** symbols from Miles. Not `AIL_*`, not `HSAMPLE`,
//     not `HDIGDRIVER` - checked with grep over all 59 files.
//   - Of the whole `CSoundManager`, `GameLib` calls **five methods**:
//     `PlaySound3D`, `PlayAmbienceSound3D`, `SetSoundVolume3D`,
//     `StopSound3D`, `UpdateSoundInstance` - plus `Instance()`.
//
// Miles sat only in the PROTECTED part of the class and in the helper
// headers. The public interface is entirely free of it.
//
// So this file is **the same public interface**, copied
// signature by signature from the original, without the protected part. The inside of the class in the port
// will stand on the browser's sound, not on Miles - and that is exactly why
// there is not a single field here: the object layout is our business.
//
// NOTE on `CSingleton`: in TMP4 it **does not create itself** - `Instance()` kills
// the program with an assertion if the object was not created explicitly. I keep that behaviour,
// because the client's start-up order depends on it.

#pragma once

#include "../eterBase/Singleton.h"

#include "SoundData.h"
#include "Type.h"

// `SoundData.h` came in the original through `SoundManager2D.h` (which
// we replaced), and `UserInterface.cpp` relies on it. I list it
// EXPLICITLY, instead of counting on the chain - the same fix as for
// `PRTerrainLib/StdAfx.h`.

class CSoundManager : public CSingleton<CSoundManager>
{
public:
    CSoundManager();
    virtual ~CSoundManager();

    BOOL Create();
    void Destroy();

    void SetPosition(float fx, float fy, float fz);
    void SetDirection(float fxDir, float fyDir, float fzDir,
                      float fxUp, float fyUp, float fzUp);
    void Update();

    float GetSoundScale();
    void  SetSoundScale(float fScale);
    void  SetAmbienceSoundScale(float fScale);
    void  SetSoundVolume(float fVolume);
    void  SetSoundVolumeRatio(float fRatio);
    void  SetMusicVolume(float fVolume);
    void  SetMusicVolumeRatio(float fRatio);
    void  SetSoundVolumeGrade(int iGrade);
    void  SetMusicVolumeGrade(int iGrade);
    void  SaveVolume();
    void  RestoreVolume();
    float GetSoundVolume();
    float GetMusicVolume();

    // Sound
    void PlaySound2D(const char* c_szFileName);
    void PlaySound3D(float fx, float fy, float fz, const char* c_szFileName,
                     int iPlayCount = 1);
    void StopSound3D(int iIndex);
    int  PlayAmbienceSound3D(float fx, float fy, float fz, const char* c_szFileName,
                             int iPlayCount = 1);
    void PlayCharacterSound3D(float fx, float fy, float fz, const char* c_szFileName,
                              BOOL bCheckFrequency = FALSE);
    void SetSoundVolume3D(int iIndex, float fVolume);
    void StopAllSound3D();

    // Music
    void PlayMusic(const char* c_szFileName);
    void FadeInMusic(const char* c_szFileName, float fVolumeSpeed = 0.016f);
    void FadeOutMusic(const char* c_szFileName, float fVolumeSpeed = 0.016f);
    void FadeLimitOutMusic(const char* c_szFileName, float fLimitVolume,
                           float fVolumeSpeed = 0.016f);
    void FadeOutAllMusic();
    void FadeAll();

    // Sound nodes in animation
    void UpdateSoundData(DWORD dwcurFrame,
                         const NSound::TSoundDataVector* c_pSoundDataVector);
    void UpdateSoundData(float fx, float fy, float fz, DWORD dwcurFrame,
                         const NSound::TSoundDataVector* c_pSoundDataVector);
    void UpdateSoundInstance(float fx, float fy, float fz, DWORD dwcurFrame,
                             const NSound::TSoundInstanceVector* c_pSoundInstanceVector,
                             BOOL bCheckFrequency = FALSE);
    void UpdateSoundInstance(DWORD dwcurFrame,
                             const NSound::TSoundInstanceVector* c_pSoundInstanceVector);
};
