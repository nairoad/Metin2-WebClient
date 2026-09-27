// SPDX-License-Identifier: GPL-2.0-or-later
// dshow.h - DirectShow. Film playback in the Windows client.
//
// In the browser the <video> tag does that, so the DirectShow layer has
// nothing to play. The header is empty; when code reaches for a specific
// interface, the compiler will report it and it will be a separate
// decision.

#pragma once

#include "win32_compat.h"

// ---------------------------------------------------------------------------
// DirectShow interfaces - OPAQUE
// ---------------------------------------------------------------------------
// `UserInterface/MovieMan.h` plays the intro film through a DirectShow filter
// graph: it builds it from `IGraphBuilder`, hands the frames to a DirectDraw
// surface, the sound to `IBasicAudio`.
//
// In the browser this whole construction is **one `<video>` tag**. The names
// are declared here, but without bodies and without methods: `MovieMan.h`
// keeps only pointers to them, so this is enough for the file to parse, and
// any reach for a method will stop the compiler.
//
// The same shape as `DIMM.h`: a whole Windows subsystem that in
// the browser corresponds to one HTML element.
struct IAMMultiMediaStream;
struct IMultiMediaStream;
struct IMediaStream;
struct IDirectDrawMediaStream;
struct IDirectDrawStreamSample;
struct IGraphBuilder;
struct IMediaControl;
struct IMediaEvent;
struct IMediaSeeking;
struct IBasicAudio;
struct IBasicVideo;
struct IVideoWindow;
struct IBaseFilter;
struct IPin;
struct IDirectDraw;
struct IDirectDrawSurface;
struct IDirectDrawSurface7;

// The rest of the names from `MovieMan` - listed with ONE grep over the whole
// file, instead of adding one per compiler run. The latter way cost me six
// runs before I noticed I could ask for all of them at once.
struct IMediaEventEx;
struct IDMOWrapperFilter;
struct IDirectDrawClipper;
struct IEnumPins;
struct IMoniker;
struct IRunningObjectTable;
struct IEnumMoniker;
struct IBindCtx;
struct ICreateDevEnum;
struct IEnumFilters;
struct IFilterGraph;
struct IPropertyBag;

// Interface identifiers `MovieMan` passes to `QueryInterface`.
extern const IID IID_IAMMultiMediaStream;
extern const IID IID_IMultiMediaStream;
extern const IID IID_IDirectDrawMediaStream;
extern const IID IID_IBaseFilter;
extern const IID IID_IBasicAudio;
extern const IID IID_IDMOWrapperFilter;
extern const CLSID CLSID_AMMultiMediaStream;
extern const CLSID CLSID_DMOWrapperFilter;
extern const GUID  MSPID_PrimaryVideo;
extern const GUID  MSPID_PrimaryAudio;

extern const CLSID CLSID_FilterGraph;
extern const CLSID CLSID_SampleGrabber;
extern const CLSID CLSID_NullRenderer;
extern const GUID  MEDIATYPE_Video;
extern const GUID  MEDIASUBTYPE_RGB32;
extern const GUID  MEDIASUBTYPE_RGB24;
extern const IID   IID_IGraphBuilder;
extern const IID   IID_IMediaControl;
extern const IID   IID_IMediaEventEx;
extern const IID   IID_IVideoWindow;
extern const IID   IID_IBasicVideo;
extern const IID   IID_ISampleGrabber;

#define EC_COMPLETE    0x01
#define EC_USERABORT   0x02
#define EC_ERRORABORT  0x03

typedef long OAHWND;

/// A DirectShow media type description (fields as in Win32).
typedef struct _AM_MEDIA_TYPE {
    GUID   majortype;
    GUID   subtype;
    BOOL   bFixedSizeSamples;
    BOOL   bTemporalCompression;
    ULONG  lSampleSize;
    GUID   formattype;
    IUnknown* pUnk;
    ULONG  cbFormat;
    BYTE*  pbFormat;
} AM_MEDIA_TYPE;
