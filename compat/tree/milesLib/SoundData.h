// milesLib/SoundData.h - OUR header in place of the TMP4 one.
//
// ===========================================================================
// WHAT CHANGED AND WHY
// ===========================================================================
// The original pulls in `<mss.h>` - the header of **Miles Sound System**, a commercial
// library, which under emscripten ends with `#error ... did not detect
// your platform`. The same reason for which we already replaced
// `milesLib/SoundManager.h`.
//
// Miles sat in the PRIVATE part of this class: four callbacks
// (`open_callback`, `close_callback`, `seek_callback`, `read_callback`),
// through which Miles read sound from the game pack instead of from disk. The public
// interface is **entirely free** of Miles - it is an ordinary handle to
// a loaded sound file.
//
// ===========================================================================
// WHY THIS FILE IS NEEDED AT ALL
// ===========================================================================
// `UserInterface/UserInterface.cpp` calls exactly ONE thing:
//
//     CSoundData::SetPackMode();   // Miles - setting the callbacks
//
// One line in the whole client. In the port it means: "sound is to be read
// from the pack, not from disk" - and that sentence stays true regardless of
// whether Miles or Web Audio plays the sound. Only whom we tell it
// changes.
//
// I keep the private fields **without the Miles callbacks** and without their types
// (`U32`, `S32`, `AILCALLBACK`). The object layout is our business in the port,
// because nobody outside writes it.

#pragma once

#include "../eterBase/MappedFile.h"

class CSoundData
{
public:
    enum
    {
        FLAG_DATA_SIZE     = 1,
        SOUND_FILE_MAX_NUM = 5,
    };

public:
    /// Switches sound loading to the game pack instead of the file system.
    /// In the original it set the Miles callbacks; in the port it does the same
    /// for the browser sound layer.
    static void SetPackMode();

    CSoundData();
    virtual ~CSoundData();

    void         Assign(const char* filename);
    LPVOID       Get();
    ULONG        GetSize();
    void         Release();
    DWORD        GetAccessTime();
    const char*  GetFileName();

    void  SetPlayTime(DWORD dwPlayTime);
    DWORD GetPlayTime();

protected:
    bool ReadFromDisk();
    void Destroy();

protected:
    char   m_filename[128];
    int    m_iRefCount;
    DWORD  m_dwAccessTime;
    DWORD  m_dwPlayTime;
    ULONG  m_size;
    LPVOID m_data;
    long   m_flag;
    bool   m_assigned;
};
