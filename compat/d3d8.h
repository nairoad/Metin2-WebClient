// SPDX-License-Identifier: GPL-2.0-or-later
// d3d8.h - the Direct3D 8 CONSTANTS AND TYPES layer for the port.
//
// MEASUREMENT: `GameLib` uses **116 constant names, 6 structure
// types and 6 interface names** of Direct3D - and not a single call of a
// device method. That is no accident: `GameLib` does not draw itself, it sets
// state through `CStateManager` from `EterLib`. `eterLib/StateManager.cpp`
// is compiled as it is, and the device it talks to is the port's own GL
// layer - `CGlDevice` and the other `CGl*` classes in gl_internal.h.
//
// -------------------------------------------------------------------------
// WHAT THESE NUMBERS MEAN, AND WHAT THEY DO NOT
// -------------------------------------------------------------------------
// I give the **real Direct3D 8 values**. But one has to know what for:
//
//  - Inside the port the values **do not have to** be the same. `GameLib`
//    passes them to OUR state manager, which interprets them. It would be
//    enough for them to be DIFFERENT within one family.
//  - The exception that matters: the **bit** families (`D3DFVF_*`,
//    `D3DCLEAR_*`, `D3DUSAGE_*`, `D3DLOCK_*`) are combined with `|`. There
//    the values must be disjoint bits, otherwise flags silently merge.
//  - Second exception: `D3DFMT_*` describes a pixel layout that **is imposed
//    from outside** by the data files (category A). The meaning must match,
//    even if the number itself need not.
//
// I take the original values, because it costs nothing and removes a whole
// class of questions of the kind "did the value happen to matter here".
//
// -------------------------------------------------------------------------
// WHAT IS NOT HERE
// -------------------------------------------------------------------------
// The `IDirect3DDevice8` interface with its hundred methods. `GameLib` does
// not call it. Resource names (`LPDIRECT3DTEXTURE8` and relatives) are
// **opaque** here - they occur in the code only as handles passed along. If
// some file did reach for a method, the compiler would report it and that
// would be a separate decision, not a silent stub.
// (Note added when translating: superseded by the sections below
// - the file declares the methods TMP4 calls, as pure
// virtual interfaces, and the port implements them in gl_*.cpp.)

#pragma once

#include "win32_compat.h"

typedef DWORD D3DCOLOR;
typedef float D3DVALUE;

// Forward declarations - used in the declarations below, defined further on.
struct _D3DCAPS8;
typedef struct _D3DCAPS8 D3DCAPS8;
struct _RGNDATA;
typedef struct _RGNDATA RGNDATA;
struct _D3DPRESENT_PARAMETERS_;
typedef struct _D3DPRESENT_PARAMETERS_ D3DPRESENT_PARAMETERS;
struct _D3DMATRIX;
typedef struct _D3DMATRIX D3DMATRIX;

/// Packs four 0-255 channels into a `D3DCOLOR` (A8R8G8B8); each argument is
/// masked to its low 8 bits.
#define D3DCOLOR_ARGB(a, r, g, b) \
    ((D3DCOLOR)((((a) & 0xFFu) << 24) | (((r) & 0xFFu) << 16) | \
                (((g) & 0xFFu) << 8) | ((b) & 0xFFu)))
/// `D3DCOLOR_ARGB` with the arguments in r, g, b, a order.
#define D3DCOLOR_RGBA(r, g, b, a) D3DCOLOR_ARGB(a, r, g, b)
/// `D3DCOLOR_ARGB` with alpha fixed at 0xFF (opaque).
#define D3DCOLOR_XRGB(r, g, b)    D3DCOLOR_ARGB(0xFFu, r, g, b)

// ===========================================================================
// Structure types
// ===========================================================================

typedef struct _D3DVECTOR {
    float x, y, z;
} D3DVECTOR;

typedef struct _D3DCOLORVALUE {
    float r, g, b, a;
} D3DCOLORVALUE;

typedef enum _D3DLIGHTTYPE {
    D3DLIGHT_POINT       = 1,
    D3DLIGHT_SPOT        = 2,
    D3DLIGHT_DIRECTIONAL = 3,
} D3DLIGHTTYPE;

typedef struct _D3DLIGHT8 {
    D3DLIGHTTYPE  Type;
    D3DCOLORVALUE Diffuse;
    D3DCOLORVALUE Specular;
    D3DCOLORVALUE Ambient;
    D3DVECTOR     Position;
    D3DVECTOR     Direction;
    float         Range;
    float         Falloff;
    float         Attenuation0;
    float         Attenuation1;
    float         Attenuation2;
    float         Theta;
    float         Phi;
} D3DLIGHT8;

typedef struct _D3DMATERIAL8 {
    D3DCOLORVALUE Diffuse;
    D3DCOLORVALUE Ambient;
    D3DCOLORVALUE Specular;
    D3DCOLORVALUE Emissive;
    float         Power;
} D3DMATERIAL8;

typedef struct _D3DVIEWPORT8 {
    DWORD X, Y;
    DWORD Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT8;

typedef struct _D3DLOCKED_RECT {
    int   Pitch;
    void* pBits;
} D3DLOCKED_RECT;

// ===========================================================================
// Pixel formats - CATEGORY A: meaning imposed by the data files
// ===========================================================================

typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN   =  0,
    D3DFMT_R8G8B8    = 20,
    D3DFMT_A8R8G8B8  = 21,
    D3DFMT_X8R8G8B8  = 22,
    D3DFMT_R5G6B5    = 23,
    D3DFMT_X1R5G5B5  = 24,
    D3DFMT_A1R5G5B5  = 25,
    D3DFMT_A4R4G4B4  = 26,
    D3DFMT_A8        = 28,
    D3DFMT_R3G3B2    = 27,
    D3DFMT_A8R3G3B2  = 29,
    D3DFMT_X4R4G4B4  = 30,
    D3DFMT_A2B10G10R10 = 31,
    D3DFMT_DXT1      = 0x31545844,  // 'DXT1'
    D3DFMT_DXT3      = 0x33545844,
    D3DFMT_DXT5      = 0x35545844,
    D3DFMT_D16       = 80,
    D3DFMT_D24S8     = 75,
    D3DFMT_INDEX16   = 101,
    D3DFMT_INDEX32   = 102,
} D3DFORMAT;

typedef struct _D3DSURFACE_DESC {
    D3DFORMAT Format;
    DWORD     Type;
    DWORD     Usage;
    DWORD     Pool;
    UINT      Size;
    DWORD     MultiSampleType;
    UINT      Width;
    UINT      Height;
} D3DSURFACE_DESC;

typedef enum _D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0,
} D3DMULTISAMPLE_TYPE;

typedef enum _D3DBACKBUFFER_TYPE {
    D3DBACKBUFFER_TYPE_MONO  = 0,
    D3DBACKBUFFER_TYPE_LEFT  = 1,
    D3DBACKBUFFER_TYPE_RIGHT = 2,
} D3DBACKBUFFER_TYPE;

typedef enum _D3DPOOL {
    D3DPOOL_DEFAULT   = 0,
    D3DPOOL_MANAGED   = 1,
    D3DPOOL_SYSTEMMEM = 2,
    D3DPOOL_SCRATCH   = 3,
} D3DPOOL;

// ===========================================================================
// Kind of primitives drawn
// ===========================================================================

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST     = 1,
    D3DPT_LINELIST      = 2,
    D3DPT_LINESTRIP     = 3,
    D3DPT_TRIANGLELIST  = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN   = 6,
} D3DPRIMITIVETYPE;

// ===========================================================================
// Device state
// ===========================================================================

typedef enum _D3DRENDERSTATETYPE {
    D3DRS_SHADEMODE                 = 9,
    D3DRS_ZENABLE                   = 7,
    D3DRS_FILLMODE                  = 8,
    D3DRS_ZWRITEENABLE              = 14,
    D3DRS_ALPHATESTENABLE           = 15,
    D3DRS_SRCBLEND                  = 19,
    D3DRS_DESTBLEND                 = 20,
    D3DRS_CULLMODE                  = 22,
    D3DRS_ZFUNC                     = 23,
    D3DRS_ALPHAREF                  = 24,
    D3DRS_ALPHAFUNC                 = 25,
    D3DRS_ALPHABLENDENABLE          = 27,
    D3DRS_FOGENABLE                 = 28,
    D3DRS_SPECULARENABLE            = 29,
    D3DRS_FOGCOLOR                  = 34,
    D3DRS_FOGTABLEMODE              = 35,
    D3DRS_FOGSTART                  = 36,
    D3DRS_FOGEND                    = 37,
    D3DRS_FOGDENSITY                = 38,
    D3DRS_RANGEFOGENABLE            = 48,
    D3DRS_TEXTUREFACTOR             = 60,
    D3DRS_LIGHTING                  = 137,
    D3DRS_FOGVERTEXMODE             = 140,
    D3DRS_COLORVERTEX               = 141,
    D3DRS_DIFFUSEMATERIALSOURCE     = 145,
    D3DRS_SOFTWAREVERTEXPROCESSING  = 153,
    D3DRS_BLENDOP                   = 171,

    // The rest of the state that `CStateManager` caches by name, although
    // the game itself does not set it. They must be ENUMERATORS, not
    // macros: `SetRenderState` takes `D3DRENDERSTATETYPE`, so an `int` will
    // not convert. The compiler caught it - I first wrote them
    // as `#define` and the type protested at once.
    D3DRS_LINEPATTERN               = 10,
    D3DRS_LASTPIXEL                 = 16,
    D3DRS_DITHERENABLE              = 26,
    D3DRS_ZVISIBLE                  = 30,
    D3DRS_EDGEANTIALIAS             = 40,
    D3DRS_ZBIAS                     = 47,
    D3DRS_STENCILENABLE             = 52,
    D3DRS_STENCILWRITEMASK          = 58,
    D3DRS_WRAP0                     = 128,
    D3DRS_WRAP1                     = 129,
    D3DRS_WRAP2                     = 130,
    D3DRS_WRAP3                     = 131,
    D3DRS_WRAP4                     = 132,
    D3DRS_WRAP5                     = 133,
    D3DRS_WRAP6                     = 134,
    D3DRS_WRAP7                     = 135,
    D3DRS_CLIPPING                  = 136,
    D3DRS_AMBIENT                   = 139,
    D3DRS_LOCALVIEWER               = 142,
    D3DRS_NORMALIZENORMALS          = 143,
    D3DRS_SPECULARMATERIALSOURCE    = 146,
    D3DRS_AMBIENTMATERIALSOURCE     = 147,
    D3DRS_EMISSIVEMATERIALSOURCE    = 148,
    D3DRS_VERTEXBLEND               = 151,
    D3DRS_CLIPPLANEENABLE           = 152,
    D3DRS_POINTSIZE                 = 154,
    D3DRS_MULTISAMPLEANTIALIAS      = 161,
    D3DRS_MULTISAMPLEMASK           = 162,
    D3DRS_INDEXEDVERTEXBLENDENABLE  = 167,
    D3DRS_COLORWRITEENABLE          = 168,
} D3DRENDERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP               =  1,
    D3DTSS_COLORARG1             =  2,
    D3DTSS_COLORARG2             =  3,
    D3DTSS_ALPHAOP               =  4,
    D3DTSS_ALPHAARG1             =  5,
    D3DTSS_ALPHAARG2             =  6,
    D3DTSS_TEXCOORDINDEX         = 11,
    D3DTSS_ADDRESSU              = 13,
    D3DTSS_ADDRESSV              = 14,
    D3DTSS_BORDERCOLOR           = 15,
    D3DTSS_MAGFILTER             = 16,
    D3DTSS_MINFILTER             = 17,
    D3DTSS_MIPFILTER             = 18,
    D3DTSS_MIPMAPLODBIAS         = 19,
    D3DTSS_MAXMIPLEVEL           = 20,
    D3DTSS_MAXANISOTROPY         = 21,
    D3DTSS_TEXTURETRANSFORMFLAGS = 24,
    D3DTSS_ADDRESSW              = 25,
    D3DTSS_COLORARG0             = 26,
    D3DTSS_ALPHAARG0             = 27,
    D3DTSS_RESULTARG             = 28,
} D3DTEXTURESTAGESTATETYPE;

/// Source of texture coordinates. This is a FLAG ORed onto the coordinate
/// set number with `|`, not an ordinary constant - hence the high value.
#define D3DTSS_TCI_PASSTHRU                     0x00000
#define D3DTSS_TCI_CAMERASPACENORMAL            0x10000
#define D3DTSS_TCI_CAMERASPACEPOSITION          0x20000
#define D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR  0x30000

typedef enum _D3DTEXTUREOP {
    D3DTOP_DISABLE            =  1,
    D3DTOP_SELECTARG1         =  2,
    D3DTOP_SELECTARG2         =  3,
    D3DTOP_MODULATE           =  4,
    D3DTOP_MODULATE2X         =  5,
    D3DTOP_MODULATE4X         =  6,
    D3DTOP_ADD                =  7,
    D3DTOP_ADDSIGNED          =  8,
    D3DTOP_SUBTRACT           = 10,
    D3DTOP_BLENDDIFFUSEALPHA  = 12,
    D3DTOP_BLENDTEXTUREALPHA  = 13,
    D3DTOP_BLENDFACTORALPHA   = 14,
    D3DTOP_MODULATEALPHA_ADDCOLOR    = 18,
    D3DTOP_MODULATECOLOR_ADDALPHA    = 19,
    D3DTOP_MODULATEINVALPHA_ADDCOLOR = 20,
    D3DTOP_MODULATEINVCOLOR_ADDALPHA = 21,
} D3DTEXTUREOP;

// Arguments of texture operations.
#define D3DTA_DIFFUSE   0x00000000
#define D3DTA_CURRENT   0x00000001
#define D3DTA_TEXTURE   0x00000002
#define D3DTA_TFACTOR   0x00000003
#define D3DTA_SPECULAR  0x00000004
#define D3DTA_TEMP      0x00000005
#define D3DTA_COMPLEMENT 0x00000010
#define D3DTA_ALPHAREPLICATE 0x00000020

typedef enum _D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP       = 1,
    D3DTADDRESS_MIRROR     = 2,
    D3DTADDRESS_CLAMP      = 3,
    D3DTADDRESS_BORDER     = 4,
    D3DTADDRESS_MIRRORONCE = 5,
} D3DTEXTUREADDRESS;

typedef enum _D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE           = 0,
    D3DTEXF_POINT          = 1,
    D3DTEXF_LINEAR         = 2,
    D3DTEXF_ANISOTROPIC    = 3,
    D3DTEXF_FLATCUBIC      = 4,
    D3DTEXF_GAUSSIANCUBIC  = 5,
} D3DTEXTUREFILTERTYPE;

typedef enum _D3DTEXTURETRANSFORMFLAGS {
    D3DTTFF_DISABLE   = 0,
    D3DTTFF_COUNT1    = 1,
    D3DTTFF_COUNT2    = 2,
    D3DTTFF_COUNT3    = 3,
    D3DTTFF_COUNT4    = 4,
    D3DTTFF_PROJECTED = 256,
} D3DTEXTURETRANSFORMFLAGS;

typedef enum _D3DBLEND {
    D3DBLEND_ZERO            =  1,
    D3DBLEND_ONE             =  2,
    D3DBLEND_SRCCOLOR        =  3,
    D3DBLEND_INVSRCCOLOR     =  4,
    D3DBLEND_SRCALPHA        =  5,
    D3DBLEND_INVSRCALPHA     =  6,
    D3DBLEND_DESTALPHA       =  7,
    D3DBLEND_INVDESTALPHA    =  8,
    D3DBLEND_DESTCOLOR       =  9,
    D3DBLEND_INVDESTCOLOR    = 10,
    D3DBLEND_SRCALPHASAT     = 11,
} D3DBLEND;

typedef enum _D3DBLENDOP {
    D3DBLENDOP_ADD         = 1,
    D3DBLENDOP_SUBTRACT    = 2,
    D3DBLENDOP_REVSUBTRACT = 3,
    D3DBLENDOP_MIN         = 4,
    D3DBLENDOP_MAX         = 5,
} D3DBLENDOP;

// Channel write mask, `D3DRS_COLORWRITEENABLE`. The four lowest bits; the
// rest of the word means nothing, which is why the client writes
// `0xFFFFFFFF` there and it comes to the same as `0xF`.
#define D3DCOLORWRITEENABLE_RED   (1L << 0)
#define D3DCOLORWRITEENABLE_GREEN (1L << 1)
#define D3DCOLORWRITEENABLE_BLUE  (1L << 2)
#define D3DCOLORWRITEENABLE_ALPHA (1L << 3)

typedef enum _D3DCMPFUNC {
    D3DCMP_NEVER        = 1,
    D3DCMP_LESS         = 2,
    D3DCMP_EQUAL        = 3,
    D3DCMP_LESSEQUAL    = 4,
    D3DCMP_GREATER      = 5,
    D3DCMP_NOTEQUAL     = 6,
    D3DCMP_GREATEREQUAL = 7,
    D3DCMP_ALWAYS       = 8,
} D3DCMPFUNC;

typedef enum _D3DCULL {
    D3DCULL_NONE = 1,
    D3DCULL_CW   = 2,
    D3DCULL_CCW  = 3,
} D3DCULL;

typedef enum _D3DFILLMODE {
    D3DFILL_POINT     = 1,
    D3DFILL_WIREFRAME = 2,
    D3DFILL_SOLID     = 3,
} D3DFILLMODE;

typedef enum _D3DFOGMODE {
    D3DFOG_NONE   = 0,
    D3DFOG_EXP    = 1,
    D3DFOG_EXP2   = 2,
    D3DFOG_LINEAR = 3,
} D3DFOGMODE;

typedef enum _D3DMATERIALCOLORSOURCE {
    D3DMCS_MATERIAL = 0,
    D3DMCS_COLOR1   = 1,
    D3DMCS_COLOR2   = 2,
} D3DMATERIALCOLORSOURCE;

typedef enum _D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW       = 2,
    D3DTS_PROJECTION = 3,
    D3DTS_TEXTURE0   = 16,
    D3DTS_TEXTURE1   = 17,
    D3DTS_TEXTURE2   = 18,
    D3DTS_TEXTURE3   = 19,
    D3DTS_WORLD      = 256,
} D3DTRANSFORMSTATETYPE;

// ===========================================================================
// BIT families - here the values really must be disjoint bits
// ===========================================================================

#define D3DCLEAR_TARGET   0x00000001l
#define D3DCLEAR_ZBUFFER  0x00000002l
#define D3DCLEAR_STENCIL  0x00000004l

#define D3DUSAGE_RENDERTARGET       0x00000001l
#define D3DUSAGE_DEPTHSTENCIL       0x00000002l
#define D3DUSAGE_WRITEONLY          0x00000008l
#define D3DUSAGE_SOFTWAREPROCESSING 0x00000010l
#define D3DUSAGE_DONOTCLIP          0x00000020l
#define D3DUSAGE_POINTS             0x00000040l
#define D3DUSAGE_RTPATCHES          0x00000080l
#define D3DUSAGE_NPATCHES           0x00000100l
#define D3DUSAGE_DYNAMIC            0x00000200l

#define D3DLOCK_READONLY     0x00000010l
#define D3DLOCK_NOSYSLOCK    0x00000800l
#define D3DLOCK_NOOVERWRITE  0x00001000l
#define D3DLOCK_DISCARD      0x00002000l

// Vertex description. The bits must match in LAYOUT too, because `GameLib`
// builds constants such as `D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1` out
// of them and compares the whole.
#define D3DFVF_RESERVED0  0x0001
#define D3DFVF_XYZ        0x0002
#define D3DFVF_XYZRHW     0x0004
#define D3DFVF_XYZB1      0x0006
#define D3DFVF_XYZB2      0x0008
#define D3DFVF_XYZB3      0x000a
#define D3DFVF_NORMAL     0x0010
#define D3DFVF_PSIZE      0x0020
#define D3DFVF_DIFFUSE    0x0040
#define D3DFVF_SPECULAR   0x0080
#define D3DFVF_TEX0       0x0000
#define D3DFVF_TEX1       0x0100
#define D3DFVF_TEX2       0x0200
#define D3DFVF_TEX3       0x0300
#define D3DFVF_TEX4       0x0400
#define D3DFVF_TEX8       0x0800
#define D3DFVF_TEXCOUNT_MASK  0x0f00
#define D3DFVF_TEXCOUNT_SHIFT 8

// ===========================================================================
// Resource handles - OPAQUE
// ===========================================================================
// `GameLib` passes them along and never reaches for a method. A declaration
// without a definition is enough and has the advantage that **every attempt
// to call a method stops the compiler**, instead of hitting a stub that
// pretends to be an implementation.
// (Note added when translating: superseded by the next section -
// the resources are full interfaces with pure virtual methods now.)

// ===========================================================================
// WHERE `GameLib` TOUCHES THE DEVICE - measured, not guessed
// ===========================================================================
// An earlier note said that `GameLib` "does not call a single device method".
// **That was too strong.** Counted exactly, the whole of `GameLib`
// calls:
//
//   on the device (12 methods):  CreateTexture 5, SetRenderTarget 4,
//     CreateVertexBuffer 3, CreateDepthStencilSurface 3, SetViewport 2,
//     LightEnable 2, GetRenderTarget 2, GetDepthStencilSurface 2, Clear 2,
//     SetStreamSource 1, GetViewport 1, CreateIndexBuffer 1
//
//   on resources (7 methods):  Lock 3, Unlock 3, GetSurfaceLevel 3, Release 2,
//     LockRect 1, UnlockRect 1, GetDesc 1
//
// That is the **whole** conversation of `GameLib` with low-level graphics -
// 33 calls in four files out of 59. The rest goes through `CStateManager`.
//
// THE METHODS ARE PURE VIRTUAL, on purpose:
//   - the classes become abstract, so nobody creates one by accident;
//   - there are no bodies, so there is no stub that could silently return
//     "success" and let the game draw into a non-existent texture;
//   - this is an **interface to be implemented by the port's GL layer**,
//     not a DirectX dummy. I keep the signatures as in the original, so that
//     `GameLib` needs not a single change. The implementation: `CGlDevice`,
//     `CGlTexture`, `CGlSurface`, `CGlLevelSurface`, `CGlVertexBuffer`,
//     `CGlIndexBuffer` in gl_internal.h.

typedef struct _D3DRECT {
    LONG x1, y1, x2, y2;
} D3DRECT;

/// Base of every Direct3D resource: reference counting only. The destructor
/// is protected - a resource goes away through `Release`.
struct IDirect3DResource8
{
    /// D3D8 contract: adds a reference; returns the new count.
    virtual ULONG AddRef() = 0;
    /// D3D8 contract: drops a reference, destroying the resource at zero;
    /// returns the new count.
    virtual ULONG Release() = 0;
protected:
    /// Protected: a resource goes away through `Release`.
    ~IDirect3DResource8() {}
};

/// A surface (render target, depth buffer or one mip level of a texture):
/// description and CPU locking. Implemented by `CGlSurface` and
/// `CGlLevelSurface` (gl_internal.h).
struct IDirect3DSurface8 : public IDirect3DResource8
{
    /// D3D8 contract: the surface's format, usage, pool, size and dimensions.
    virtual HRESULT GetDesc(D3DSURFACE_DESC* pDesc) = 0;
    /// D3D8 contract: maps the surface (or the rectangle `pRect`) for CPU
    /// access; `pLockedRect` receives the pointer and the row pitch.
    virtual HRESULT LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) = 0;
    /// D3D8 contract: ends the CPU access started by `LockRect`.
    virtual HRESULT UnlockRect() = 0;
};

/// Hierarchy kept as in the original: `EffectLib` passes a texture where the
/// base type is expected.
struct IDirect3DBaseTexture8 : public IDirect3DResource8
{
};

/// A 2D texture with mip levels: per-level surfaces, locking and
/// descriptions. Implemented by `CGlTexture` (gl_internal.h).
struct IDirect3DTexture8 : public IDirect3DBaseTexture8
{
    /// D3D8 contract: mip level `Level` as a surface (a new reference).
    virtual HRESULT GetSurfaceLevel(UINT Level, IDirect3DSurface8** ppSurfaceLevel) = 0;
    /// D3D8 contract: maps mip level `Level` (or a rectangle of it) for CPU
    /// access.
    virtual HRESULT LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect,
                             CONST RECT* pRect, DWORD Flags) = 0;
    /// D3D8 contract: ends the CPU access to mip level `Level`.
    virtual HRESULT UnlockRect(UINT Level) = 0;
    /// D3D8 contract: the description of mip level `Level`.
    virtual HRESULT GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) = 0;
    /// Added after measuring. My list of methods was
    /// INCOMPLETE: the grep went by variable names, and `GetLevelCount` is
    /// called on a variable that did not match the pattern. Only the
    /// compiler found it - and that is the right order.
    virtual DWORD GetLevelCount() = 0;
};

/// A vertex buffer: lock a byte range for writing, unlock to hand it to the
/// GPU. Implemented by `CGlVertexBuffer` (gl_internal.h).
struct IDirect3DVertexBuffer8 : public IDirect3DResource8
{
    /// `ppbData` is `BYTE**`, not `void**` - as in D3D8. `GameLib` casts the
    /// result to its vertex type and the signature has to match.
    virtual HRESULT Lock(UINT OffsetToLock, UINT SizeToLock, BYTE** ppbData, DWORD Flags) = 0;
    /// D3D8 contract: ends the lock; the written bytes go to the GPU.
    virtual HRESULT Unlock() = 0;
};

/// An index buffer, with the same lock/unlock contract as the vertex
/// buffer. Implemented by `CGlIndexBuffer` (gl_internal.h).
struct IDirect3DIndexBuffer8 : public IDirect3DResource8
{
    /// D3D8 contract: maps `SizeToLock` bytes from `OffsetToLock` (0, 0 = the
    /// whole buffer) for writing; `D3DLOCK_*` flags.
    virtual HRESULT Lock(UINT OffsetToLock, UINT SizeToLock, BYTE** ppbData, DWORD Flags) = 0;
    /// D3D8 contract: ends the lock; the written indices go to the GPU.
    virtual HRESULT Unlock() = 0;
};

/// The Direct3D 8 device: the methods TMP4 calls (measured and
/// 155), pure virtual. Implemented by `CGlDevice` (gl_internal.h, gl_*.cpp);
/// the destructor is protected - it goes away through `Release`.
struct IDirect3DDevice8
{
    /// D3D8 contract: adds a reference to the device.
    virtual ULONG AddRef() = 0;
    /// D3D8 contract: drops a reference to the device.
    virtual ULONG Release() = 0;

    /// D3D8 contract: creates a texture (`Levels` 0 = the full mip chain).
    virtual HRESULT CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                  D3DFORMAT Format, D3DPOOL Pool,
                                  IDirect3DTexture8** ppTexture) = 0;
    /// D3D8 contract: creates a vertex buffer of `Length` bytes.
    virtual HRESULT CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                       IDirect3DVertexBuffer8** ppVertexBuffer) = 0;
    /// D3D8 contract: creates an index buffer of `Length` bytes, 16- or 32-bit
    /// indices (`Format`).
    virtual HRESULT CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                      IDirect3DIndexBuffer8** ppIndexBuffer) = 0;
    /// D3D8 contract: creates a depth/stencil surface.
    virtual HRESULT CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                              D3DMULTISAMPLE_TYPE MultiSample,
                                              IDirect3DSurface8** ppSurface) = 0;

    /// D3D8 contract: the current render target (a new reference).
    virtual HRESULT GetRenderTarget(IDirect3DSurface8** ppRenderTarget) = 0;
    /// D3D8 contract: the current depth/stencil surface (a new reference).
    virtual HRESULT GetDepthStencilSurface(IDirect3DSurface8** ppZStencilSurface) = 0;
    /// D3D8 contract: draws from now on into `pRenderTarget` with depth buffer
    /// `pNewZStencil`; also resets the viewport to the whole target.
    virtual HRESULT SetRenderTarget(IDirect3DSurface8* pRenderTarget,
                                    IDirect3DSurface8* pNewZStencil) = 0;

    /// D3D8 contract: the current viewport.
    virtual HRESULT GetViewport(D3DVIEWPORT8* pViewport) = 0;
    /// D3D8 contract: sets the viewport (rectangle and depth range).
    virtual HRESULT SetViewport(CONST D3DVIEWPORT8* pViewport) = 0;

    /// D3D8 contract: clears the target (colour), depth and/or stencil
    /// (`D3DCLEAR_*`) in the viewport or in `Count` rectangles.
    virtual HRESULT Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags,
                          D3DCOLOR Color, float Z, DWORD Stencil) = 0;
    /// D3D8 contract: switches fixed-function light `Index` on or off.
    virtual HRESULT LightEnable(DWORD Index, BOOL Enable) = 0;
    /// Gamma ramp - `PythonGraphic` sets the image brightness with it.
    /// In the browser the counterpart is a CSS filter or a shader correction.
    /// (Note added when translating: neither exists yet -
    /// `CGlDevice::SetGammaRamp` remembers the ramp and reports itself once
    /// as a stub, gl_render_states.cpp.)
    virtual void SetGammaRamp(DWORD Flags, CONST D3DGAMMARAMP* pRamp) = 0;
    /// D3D8 contract: the current gamma ramp.
    virtual void GetGammaRamp(D3DGAMMARAMP* pRamp) = 0;
    /// D3D8 contract: back buffer `BackBuffer` as a surface (a new reference).
    virtual HRESULT GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type,
                                  IDirect3DSurface8** ppBackBuffer) = 0;
    /// D3D8 contract: binds a vertex buffer with vertex size `Stride` to stream
    /// `StreamNumber`.
    virtual HRESULT SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8* pStreamData,
                                    UINT Stride) = 0;

    // -----------------------------------------------------------------------
    // The second half of the contact - from measuring `EterLib`
    // -----------------------------------------------------------------------
    // `GameLib` called the device in 33 places. `EterLib` does it more
    // widely, because it is the drawing layer - and `CStateManager` is its
    // heart: it caches state and passes on only the changes.
    //
    // Everything below came from TWO greps: over calls on the device pointer
    // in `eterLib`, `eterGrnLib` and `effectLib` (25 names) and over the body
    // of `StateManager.cpp` itself (19 names). Nothing here is "just in case".

    /// D3D8 contract: starts the drawing of a frame.
    virtual HRESULT BeginScene() = 0;
    /// D3D8 contract: ends the drawing of a frame.
    virtual HRESULT EndScene() = 0;
    /// D3D8 contract: shows the finished frame (flips the back buffer).
    virtual HRESULT Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                            HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) = 0;
    /// D3D8 contract: recreates the swap chain with new parameters (size,
    /// format) after a mode change or a lost device.
    virtual HRESULT Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) = 0;
    /// D3D8 contract: D3D_OK, or D3DERR_DEVICELOST / DEVICENOTRESET when the
    /// device was lost.
    virtual HRESULT TestCooperativeLevel() = 0;
    /// D3D8 contract: the device capabilities.
    virtual HRESULT GetDeviceCaps(D3DCAPS8* pCaps) = 0;
    /// D3D8 contract: an estimate of free texture memory, in bytes.
    virtual UINT    GetAvailableTextureMem() = 0;

    /// D3D8 contract: sets one render state (`D3DRS_*`).
    virtual HRESULT SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) = 0;
    /// D3D8 contract: reads one render state.
    virtual HRESULT GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) = 0;
    /// D3D8 contract: sets one state (`D3DTSS_*`) of texture stage `Stage`.
    virtual HRESULT SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                         DWORD Value) = 0;
    /// D3D8 contract: reads one state of texture stage `Stage`.
    virtual HRESULT GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                         DWORD* pValue) = 0;
    /// D3D8 contract: binds `pTexture` (or NULL) to stage `Stage`.
    virtual HRESULT SetTexture(DWORD Stage, IDirect3DBaseTexture8* pTexture) = 0;
    /// D3D8 contract: the texture bound to stage `Stage` (a new reference).
    virtual HRESULT GetTexture(DWORD Stage, IDirect3DBaseTexture8** ppTexture) = 0;

    /// D3D8 contract: sets the world, view, projection or texture matrix.
    virtual HRESULT SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) = 0;
    /// D3D8 contract: reads a transform matrix.
    virtual HRESULT GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) = 0;
    /// D3D8 contract: sets the fixed-function material.
    virtual HRESULT SetMaterial(CONST D3DMATERIAL8* pMaterial) = 0;
    /// D3D8 contract: reads the material.
    virtual HRESULT GetMaterial(D3DMATERIAL8* pMaterial) = 0;
    /// D3D8 contract: sets the parameters of fixed-function light `Index`.
    virtual HRESULT SetLight(DWORD Index, CONST D3DLIGHT8* pLight) = 0;
    /// D3D8 contract: reads the parameters of light `Index`.
    virtual HRESULT GetLight(DWORD Index, D3DLIGHT8* pLight) = 0;

    /// `BaseVertexIndex` is an offset ADDED to every index - a D3D8 quirk
    /// that newer versions do not have.
    virtual HRESULT SetIndices(IDirect3DIndexBuffer8* pIndexData, UINT BaseVertexIndex) = 0;

    /// In D3D8 a shader is a number (a handle), not an object.
    /// `SetVertexShader` takes the same parameter as an `FVF` description -
    /// if the value fits in the `FVF` bits, it means "fixed pipeline", not a
    /// shader.
    virtual HRESULT CreateVertexShader(CONST DWORD* pDeclaration, CONST DWORD* pFunction,
                                       DWORD* pHandle, DWORD Usage) = 0;
    /// D3D8 contract: selects a vertex shader handle - or, when `Handle` fits
    /// the FVF bits, the fixed pipeline with that vertex format.
    virtual HRESULT SetVertexShader(DWORD Handle) = 0;
    /// D3D8 contract: the current vertex shader handle (or FVF).
    virtual HRESULT GetVertexShader(DWORD* pHandle) = 0;
    /// D3D8 contract: frees a handle from `CreateVertexShader`.
    virtual HRESULT DeleteVertexShader(DWORD Handle) = 0;
    /// D3D8 contract: writes `ConstantCount` four-float constants from register
    /// `Register` on.
    virtual HRESULT SetVertexShaderConstant(DWORD Register, CONST void* pConstantData,
                                            DWORD ConstantCount) = 0;
    /// D3D8 contract: creates a pixel shader from assembled tokens; returns a
    /// handle.
    virtual HRESULT CreatePixelShader(CONST DWORD* pFunction, DWORD* pHandle) = 0;
    /// D3D8 contract: selects a pixel shader (0 = the fixed texture stages).
    virtual HRESULT SetPixelShader(DWORD Handle) = 0;
    /// D3D8 contract: frees a pixel shader handle.
    virtual HRESULT DeletePixelShader(DWORD Handle) = 0;
    /// D3D8 contract: writes `ConstantCount` four-float pixel shader constants.
    virtual HRESULT SetPixelShaderConstant(DWORD Register, CONST void* pConstantData,
                                           DWORD ConstantCount) = 0;

    /// D3D8 contract: draws `PrimitiveCount` primitives from the bound stream,
    /// starting at vertex `StartVertex`.
    virtual HRESULT DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                  UINT PrimitiveCount) = 0;
    /// D3D8 contract: draws `PrimitiveCount` indexed primitives from index
    /// `StartIndex` of the bound index buffer (plus the `SetIndices` base).
    virtual HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                         UINT NumVertices, UINT StartIndex,
                                         UINT PrimitiveCount) = 0;
    /// D3D8 contract: draws from vertices in user memory ("UP"), not from a
    /// buffer.
    virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                    CONST void* pVertexStreamZeroData,
                                    UINT VertexStreamZeroStride) = 0;
    /// D3D8 contract: draws indexed primitives with both indices and vertices in
    /// user memory.
    virtual HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                           UINT MinVertexIndex, UINT NumVertices,
                                           UINT PrimitiveCount, CONST void* pIndexData,
                                           D3DFORMAT IndexDataFormat,
                                           CONST void* pVertexStreamZeroData,
                                           UINT VertexStreamZeroStride) = 0;
protected:
    /// Protected: the device goes away through `Release`.
    ~IDirect3DDevice8() {}
};

struct IDirect3D8;

typedef IDirect3DTexture8*      LPDIRECT3DTEXTURE8;
typedef IDirect3DSurface8*      LPDIRECT3DSURFACE8;
typedef IDirect3DVertexBuffer8* LPDIRECT3DVERTEXBUFFER8;
typedef IDirect3DIndexBuffer8*  LPDIRECT3DINDEXBUFFER8;
typedef IDirect3DDevice8*       LPDIRECT3DDEVICE8;
typedef IDirect3D8*             LPDIRECT3D8;

// Device type and its capabilities. `D3DCAPS8` stays OPAQUE for the same
// reason as the resource handles: in `GameLib` and the `EterLib` headers it
// occurs only as a reference in the `PFNCONFIRMDEVICE` declaration. Querying
// the hardware capabilities is the job of the layer that replaces `EterLib`
// in the port - and it will decide what in this structure makes sense in
// the browser.
// (Note added when translating: superseded - see the
// `D3DCAPS8` section below; the structure is complete and `EterLib` is
// compiled, not replaced.)
typedef enum _D3DDEVTYPE {
    D3DDEVTYPE_HAL = 1,
    D3DDEVTYPE_REF = 2,
    D3DDEVTYPE_SW  = 3,
} D3DDEVTYPE;

// ---------------------------------------------------------------------------
// `D3DCAPS8` - device capabilities
// ---------------------------------------------------------------------------
// Earlier this was an OPAQUE structure, because `GameLib` did not read
// it. `EterLib`, however, holds it BY VALUE (`GrpBase.cpp`), so the type has
// to be complete.
//
// ATTENTION, and this matters: **this is not Microsoft's layout.** The
// original has about ninety fields and recreating them from memory would be
// guessing with no way to check. So I give **the fields TMP4 code really
// reads** (caught by grep over the whole tree: `VertexProcessingCaps`,
// `VertexShaderVersion`, `TextureFilterCaps`, `PrimitiveMiscCaps`, `DevCaps`,
// `Caps2`, `TextureAddressCaps`, `MaxTextureWidth`, `MaxTextureHeight`,
// `MaxAnisotropy`) plus a handful of obvious neighbours.
//
// That is allowed, because in the port this structure is filled by **our**
// layer, not by a driver: nobody from outside writes bytes here, so what
// counts is matching NAMES, not offsets. If code reading a field outside the
// list turned up, the compiler would report it - and then it would be known
// that the field has to be added.
/// Device capabilities - the fields TMP4 reads, not Microsoft's layout (see
/// above). Filled by `M2W_GlCapabilities` (gl_device.cpp), which both
/// `CGlDevice::GetDeviceCaps` and the factory's `GetDeviceCaps` call.
struct _D3DCAPS8 {
    D3DDEVTYPE DeviceType;
    UINT       AdapterOrdinal;

    DWORD Caps;
    DWORD Caps2;
    DWORD Caps3;
    DWORD DevCaps;

    DWORD PrimitiveMiscCaps;
    DWORD RasterCaps;
    DWORD TextureCaps;
    DWORD TextureFilterCaps;
    DWORD TextureAddressCaps;

    DWORD MaxTextureWidth;
    DWORD MaxTextureHeight;
    DWORD MaxTextureAspectRatio;
    DWORD MaxAnisotropy;
    DWORD MaxSimultaneousTextures;
    DWORD MaxTextureBlendStages;

    DWORD VertexProcessingCaps;
    DWORD MaxActiveLights;
    DWORD MaxUserClipPlanes;
    DWORD MaxVertexBlendMatrices;
    DWORD MaxStreams;
    DWORD MaxPrimitiveCount;
    DWORD MaxVertexIndex;

    DWORD VertexShaderVersion;
    DWORD PixelShaderVersion;
    DWORD MaxVertexShaderConst;
    float MaxPointSize;
};

// Capability bits used by TMP4 code in comparisons.
#define D3DPTFILTERCAPS_MAGFANISOTROPIC 0x04000000ul
#define D3DPTFILTERCAPS_MINFANISOTROPIC 0x00000400ul
#define D3DPTADDRESSCAPS_BORDER         0x00000008ul
#define D3DPMISCCAPS_MASKZ              0x00000002ul
#define D3DVTXPCAPS_TEXGEN              0x00000001ul
#define D3DCAPS2_DYNAMICTEXTURES        0x20000000ul
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT  0x00010000ul

/// Shader version number, as in D3D8: the kind tag in the high word, the
/// major and minor version in the low one.
#define D3DVS_VERSION(major, minor) (0xFFFE0000ul | ((major) << 8) | (minor))
/// Pixel shader version number: `D3DVS_VERSION` with the pixel kind tag
/// 0xFFFF in the high word.
#define D3DPS_VERSION(major, minor) (0xFFFF0000ul | ((major) << 8) | (minor))

// ===========================================================================
// Second batch - from measuring the EterLib headers
// ===========================================================================
// `GameLib` does not use these names. The `EterLib` headers that `GameLib`
// pulls in need them - and that is enough to stop the compiler.

/// Direct3D matrix. `D3DXMATRIX` from `d3dx8.h` DERIVES from it - as in the
/// original - so the field layout is the same and the conversion costs
/// nothing.
struct _D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
};

typedef struct _D3DDISPLAYMODE {
    UINT      Width;
    UINT      Height;
    UINT      RefreshRate;
    D3DFORMAT Format;
} D3DDISPLAYMODE;

typedef enum _D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD = 1,
    D3DSWAPEFFECT_FLIP    = 2,
    D3DSWAPEFFECT_COPY    = 3,
} D3DSWAPEFFECT;

/// Presentation parameters passed to `CreateDevice` and `Reset`: back
/// buffer size, format and count, swap effect, window, depth buffer.
struct _D3DPRESENT_PARAMETERS_ {
    UINT                BackBufferWidth;
    UINT                BackBufferHeight;
    D3DFORMAT           BackBufferFormat;
    UINT                BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT       SwapEffect;
    HWND                hDeviceWindow;
    BOOL                Windowed;
    BOOL                EnableAutoDepthStencil;
    D3DFORMAT           AutoDepthStencilFormat;
    DWORD               Flags;
    UINT                FullScreen_RefreshRateInHz;
    UINT                FullScreen_PresentationInterval;
};

#define MAX_DEVICE_IDENTIFIER_STRING 512

typedef struct _D3DADAPTER_IDENTIFIER8 {
    char          Driver[MAX_DEVICE_IDENTIFIER_STRING];
    char          Description[MAX_DEVICE_IDENTIFIER_STRING];
    LARGE_INTEGER DriverVersion;
    DWORD         VendorId;
    DWORD         DeviceId;
    DWORD         SubSysId;
    DWORD         Revision;
    GUID          DeviceIdentifier;
    DWORD         WHQLLevel;
} D3DADAPTER_IDENTIFIER8;

typedef IDirect3DBaseTexture8* LPDIRECT3DBASETEXTURE8;

#define D3D_OK 0

// Size of the texture coordinates in a vertex description. The bits sit at
// the top of the `FVF` word, two for each coordinate set.
#define D3DFVF_TEXTUREFORMAT1 3
#define D3DFVF_TEXTUREFORMAT2 0
#define D3DFVF_TEXTUREFORMAT3 1
#define D3DFVF_TEXTUREFORMAT4 2
/// `FVF` bits saying that coordinate set `i` has one float.
#define D3DFVF_TEXCOORDSIZE1(i) (D3DFVF_TEXTUREFORMAT1 << (i * 2 + 16))
/// Two floats for set `i` - the default, whose code is 0, so the macro
/// ignores `i`.
#define D3DFVF_TEXCOORDSIZE2(i) (D3DFVF_TEXTUREFORMAT2)
/// `FVF` bits saying that coordinate set `i` has three floats.
#define D3DFVF_TEXCOORDSIZE3(i) (D3DFVF_TEXTUREFORMAT3 << (i * 2 + 16))
/// `FVF` bits saying that coordinate set `i` has four floats.
#define D3DFVF_TEXCOORDSIZE4(i) (D3DFVF_TEXTUREFORMAT4 << (i * 2 + 16))

// ---------------------------------------------------------------------------
// Vertex stream description (`D3DVSD_*`)
// ---------------------------------------------------------------------------
// In Direct3D 8 the vertex description for a shader is a sequence of 32-bit
// words built with macros. `EterLib` builds arrays out of them - and we
// REALLY READ those arrays (`M2W_LayoutFromDeclaration`), because
// `CGraphicDevice` describes the character models' vertex format with them.
//
// CORRECTION: a sentence here said the numbers only matter in
// so far as they are "different and combinable". That was true while the
// declarations went to the bin. From the moment we take them apart, the
// register numbers have to be **the real ones** - otherwise texture
// coordinates 4-7 would land in the place of the second stream's position.
#define D3DVSD_TOKEN_STREAM   1
#define D3DVSD_TOKEN_STREAMDATA 2
#define D3DVSD_TOKEN_END      7
#define D3DVSD_TOKENTYPESHIFT 29

/// Declaration token: the following registers come from stream `n`.
#define D3DVSD_STREAM(n)   ((DWORD)((D3DVSD_TOKEN_STREAM << D3DVSD_TOKENTYPESHIFT) | (n)))
/// Declaration token: input register `reg` takes data of type `t`
/// (`D3DVSDT_*`) from the current stream.
#define D3DVSD_REG(reg, t) ((DWORD)((D3DVSD_TOKEN_STREAMDATA << D3DVSD_TOKENTYPESHIFT) | \
                                    ((t) << 16) | (reg)))
// In real Direct3D 8 `D3DVSD_END()` is simply `0xFFFFFFFF`.
// The word kind sits in bits 29-31, and all ones put a seven there - the
// same value the shift gives. The parser looks at the word KIND, so it
// accepts both forms.
/// Declaration token that ends the array (`0xFFFFFFFF`, kind 7).
#define D3DVSD_END()       ((DWORD)0xFFFFFFFFul)

// Data types in a stream.
#define D3DVSDT_FLOAT1   0x00
#define D3DVSDT_FLOAT2   0x01
#define D3DVSDT_FLOAT3   0x02
#define D3DVSDT_FLOAT4   0x03
#define D3DVSDT_D3DCOLOR 0x04
#define D3DVSDT_UBYTE4   0x05
#define D3DVSDT_SHORT2   0x06
#define D3DVSDT_SHORT4   0x07

// Vertex shader input register numbers.
#define D3DVSDE_POSITION   0
#define D3DVSDE_BLENDWEIGHT 1
#define D3DVSDE_BLENDINDICES 2
#define D3DVSDE_NORMAL     3
#define D3DVSDE_PSIZE      4
#define D3DVSDE_DIFFUSE    5
#define D3DVSDE_SPECULAR   6
#define D3DVSDE_TEXCOORD0  7
#define D3DVSDE_TEXCOORD1  8
#define D3DVSDE_TEXCOORD2  9
#define D3DVSDE_TEXCOORD3  10
#define D3DVSDE_TEXCOORD4  11
#define D3DVSDE_TEXCOORD5  12
#define D3DVSDE_TEXCOORD6  13
#define D3DVSDE_TEXCOORD7  14

// Added from measuring the EterLib sources.
#define D3DADAPTER_DEFAULT 0

typedef enum _D3DSHADEMODE {
    D3DSHADE_FLAT    = 1,
    D3DSHADE_GOURAUD = 2,
    D3DSHADE_PHONG   = 3,
} D3DSHADEMODE;

#define D3DPRESENT_INTERVAL_DEFAULT   0x00000000ul
#define D3DPRESENT_INTERVAL_ONE       0x00000001ul
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000ul

// ===========================================================================
// Fourth batch - full device state and error codes
// ===========================================================================
// `CStateManager` caches the WHOLE Direct3D state, so it names all of it -
// including the states the game itself does not set. Hence this long tail.


#define D3DVBF_DISABLE  0
#define D3DVBF_1WEIGHTS 1
#define D3DVBF_2WEIGHTS 2
#define D3DVBF_3WEIGHTS 3

#define D3DFMT_D32     71
#define D3DFMT_D15S1   73
#define D3DFMT_D24X8   77
#define D3DFMT_D24X4S4 79

typedef enum _D3DRESOURCETYPE {
    D3DRTYPE_SURFACE       = 1,
    D3DRTYPE_VOLUME        = 2,
    D3DRTYPE_TEXTURE       = 3,
    D3DRTYPE_VOLUMETEXTURE = 4,
    D3DRTYPE_CUBETEXTURE   = 5,
    D3DRTYPE_VERTEXBUFFER  = 6,
    D3DRTYPE_INDEXBUFFER   = 7,
} D3DRESOURCETYPE;

#define D3DCREATE_FPU_PRESERVE              0x00000002ul
#define D3DCREATE_MULTITHREADED             0x00000004ul
#define D3DCREATE_PUREDEVICE                0x00000010ul
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020ul
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040ul
#define D3DCREATE_MIXED_VERTEXPROCESSING    0x00000080ul

#define D3DENUM_NO_WHQL_LEVEL               0x00000002ul
#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER  0x00000001ul
#define D3DDEVCAPS_PUREDEVICE               0x00100000ul
#define D3DPMISCCAPS_CLIPTLVERTS            0x00000200ul
#define D3DCAPS2_CANRENDERWINDOWED          0x00080000ul
#define D3DVTXPCAPS_DIRECTIONALLIGHTS       0x00000008ul
#define D3DVTXPCAPS_POSITIONALLIGHTS        0x00000010ul
#define D3DLOCK_NO_DIRTY_UPDATE             0x00008000ul
#define D3D_SDK_VERSION                     220
// CORRECTED: this said 11 and 12, the numbers that in Direct3D 8
// belong to TEXCOORD4 and TEXCOORD5. While we did not read declarations it
// made no difference; now it does.
#define D3DVSDE_POSITION2                   15
#define D3DVSDE_NORMAL2                     16

// Error codes. The HRESULT layout matters here: the HIGHEST BIT means
// failure, so FAILED() works on them the same as in Windows.
#define D3DERR_OUTOFVIDEOMEMORY    ((HRESULT)0x8876017Cl)
#define D3DERR_DEVICELOST          ((HRESULT)0x88760868l)
#define D3DERR_DEVICENOTRESET      ((HRESULT)0x88760869l)
#define D3DERR_NOTAVAILABLE        ((HRESULT)0x8876086Al)
#define D3DERR_INVALIDCALL         ((HRESULT)0x8876086Cl)

// ===========================================================================
// `IDirect3D8` - the object that enumerates adapters and creates the device
// ===========================================================================
// Ten methods, all from measuring the calls in `eterLib/GrpScreen.cpp` and
// `GrpDevice.cpp`. In the browser enumerating adapters makes no sense, but
// `CScreen` dereferences this object (`IDirect3D8& rkD3D = *ms_lpd3d;`), so
// the type has to be COMPLETE - a forward declaration is not enough.
//
// Pure virtual, like the device: this is an interface to be implemented by
// the port's GL layer, which answers these questions with what WebGL reports.
// (Implemented by `CGlFactory` in
// d3d8_factory.cpp - one adapter; its `CreateDevice` refuses with E_FAIL,
// because `CGraphicDevice::Create` (grpdevice_gl.cpp) creates the device
// itself.)
/// Direct3D 8 factory: adapter enumeration, format checks, caps and device
/// creation, pure virtual. Implemented by `CGlFactory` (d3d8_factory.cpp).
struct IDirect3D8
{
    /// D3D8 contract: adds a reference to the factory.
    virtual ULONG AddRef() = 0;
    /// D3D8 contract: drops a reference to the factory.
    virtual ULONG Release() = 0;

    /// D3D8 contract: number of adapters (graphics cards).
    virtual UINT    GetAdapterCount() = 0;
    /// D3D8 contract: driver name, description and version of an adapter.
    virtual HRESULT GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                                         D3DADAPTER_IDENTIFIER8* pIdentifier) = 0;
    /// D3D8 contract: number of display modes of an adapter.
    virtual UINT    GetAdapterModeCount(UINT Adapter) = 0;
    /// D3D8 contract: display mode number `Mode`.
    virtual HRESULT EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE* pMode) = 0;
    /// D3D8 contract: the current (desktop) display mode.
    virtual HRESULT GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) = 0;

    /// D3D8 contract: whether a device of this type works with these formats.
    virtual HRESULT CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType,
                                    D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat,
                                    BOOL Windowed) = 0;
    /// D3D8 contract: whether a resource of format `CheckFormat` is supported.
    virtual HRESULT CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType,
                                      D3DFORMAT AdapterFormat, DWORD Usage,
                                      D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) = 0;
    /// D3D8 contract: whether a depth format fits a render-target format.
    virtual HRESULT CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
                                           D3DFORMAT AdapterFormat,
                                           D3DFORMAT RenderTargetFormat,
                                           D3DFORMAT DepthStencilFormat) = 0;
    /// D3D8 contract: the capabilities of a device type on an adapter.
    virtual HRESULT GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8* pCaps) = 0;
    /// D3D8 contract: creates the device.
    virtual HRESULT CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                 DWORD BehaviorFlags,
                                 D3DPRESENT_PARAMETERS* pPresentationParameters,
                                 IDirect3DDevice8** ppReturnedDeviceInterface) = 0;
protected:
    /// Protected: the factory goes away through `Release`.
    ~IDirect3D8() {}
};

/// `Direct3DCreate8` of the original API: the port's factory
/// (d3d8_factory.cpp, `CGlFactory`). `GrpDevice.cpp` called exactly this;
/// the name stays because any file of the tree may ask for it.
IDirect3D8* Direct3DCreate8(UINT SDKVersion);
