// SPDX-License-Identifier: GPL-2.0-or-later
// gl_internal.h - the ABI BETWEEN the `gl_*.cpp` files that together make
// up the WebGL device (`d3d8_gl.cpp` split into modules).

// Design:
// WHO THIS IS FOR. `d3d8_gl.h` is the PUBLIC interface of the device -
// `M2W_CreateGlDevice`, `M2W_GlCapabilities`, the free functions other
// compat files call. This header is the opposite: the resource classes
// (`CGlResource`, `CGlTexture`, `CGlSurface`, `CGlLevelSurface`) and
// the few globals that more than one `gl_*.cpp` file needs to see -
// `CGlDevice` (declared at the end of this header, methods spread over
// the gl_*.cpp files) creates these resources and calls their methods.
// Nothing outside `gl_*.cpp` includes this header.
//
// WHY A HEADER AND NOT ONE FILE. Earlier every one of these classes
// lived inline, inside `d3d8_gl.cpp`, next to `CGlDevice` that uses
// them - one 3796-line file. Splitting it needs the class DECLARED once,
// visible to every `.cpp` that touches it, with the method BODIES living
// in the `.cpp` file that owns the concern - the same shape as
// `d3d8_states.h`/`.cpp` or `d3d8_fvf.h`/`.cpp`, only for classes instead
// of free functions. `CGlTexture::GetSurfaceLevel` already worked this
// way inside one file (declared in the class, defined below it, because it
// returns `CGlLevelSurface`, a class the compiler has not seen yet);
// this header extends the same idea across files.
//
// WHAT STAYED INLINE. Pure single-return accessors with no side effect
// (`AddRef`, `Release`, `GlName`, `GetLevelCount`, `IsTarget`,
// `Framebuffer`, `GetWidth`, `GetHeight`, `Format`) are still defined right
// in the class body, as before - moving one-line getters out of line would
// only add risk of a transcription slip for no readability gain.

#pragma once

#include "d3d8.h"
#include "d3d8_fixedfunc.h"
#include "d3d8_fvf.h"
#include "d3d8_states.h"
#include "d3dx8.h"

#include <map>
#include <set>
#include <string>
#include <vector>

// ===========================================================================
// Resources (moved unchanged from d3d8_gl.cpp)
// ===========================================================================

/// The common part: a reference count. Direct3D counts them itself, and the
/// client relies on it - `Release()` appears 211 times in its code.
class CGlResource
{
public:
    /// Starts with one reference - the one the creating call hands out.
    CGlResource() : m_iReferences(1) {}
    /// Virtual, so `ReleaseReference` deletes the whole derived object.
    virtual ~CGlResource() {}

    /// Adds a reference; returns the new count.
    ULONG AddReference() { return static_cast<ULONG>(++m_iReferences); }

    /// Returns the count AFTER the decrement, like `IUnknown::Release`;
    /// deletes the object at zero.
    ULONG ReleaseReference()
    {
        --m_iReferences;
        if (m_iReferences <= 0)
        {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(m_iReferences);
    }

protected:
    int m_iReferences;
};

// ===========================================================================
// Can the browser do compressed textures (moved unchanged)
// ===========================================================================
// `WEBGL_compressed_texture_s3tc` is NOT mandatory. On desktops it is
// almost always there, on phones often missing - and Metin2 keeps most of
// its textures in DXT.
//
// Without the extension `glCompressedTexImage2D` returns `GL_INVALID_ENUM`
// and draws nothing. No exception, no message - the textures are simply
// black. So we ask ONCE, when the device is created (gl_device.cpp,
// `M2W_CreateGlDevice`), and remember the answer here, because
// `UploadLevel` (gl_textures.cpp) is on the other side of that boundary.

/// -1 = not asked yet, 0 = absent, 1 = present. Set in
/// `M2W_CreateGlDevice` (gl_device.cpp).
extern int g_iGpuHasDxt;

/// Set by `M2W_ForceDxtDecode` (gl_device.cpp) - for measurement only.
extern bool g_bForcedDxtDecode;

/// The hardware's real answer (`g_iGpuHasDxt`), unless the measurement hook
/// `M2W_ForceDxtDecode` has overridden it (the hook has no caller today; it
/// is kept for measurements).
inline bool GpuHasDxt()
{
    return g_iGpuHasDxt == 1 && !g_bForcedDxtDecode;
}

/// Raised by ANYONE who binds a texture outside the state layer.
///
/// The state memory is safe only as long as nobody changes the state behind
/// its back. `UploadLevel` has to bind the texture to send the pixels, and
/// after that the binding memory lies. Instead of pretending that does not
/// happen, it raises this flag - and the state layer
/// (`CGlDevice::ApplyTextures`, gl_textures.cpp) forgets all bindings at the
/// next draw and sets them again. Defined in gl_textures.cpp;
/// `M2W_ForgetGlState` (gl_render_states.cpp) raises it too.
extern bool g_bTextureBindingsUnknown;

/// Tells the layer that somebody drew OUTSIDE it and its state memory no
/// longer matches what is really set in OpenGL.
///
/// Called by `custom_draw.cpp` - the own drawing road for 3D content
/// (user decision, "road B"). Without it the state memory
/// would skip calls that are needed again after our work - and the image
/// would lose transparency or depth in a way impossible to trace.
/// Body in gl_render_states.cpp; `CGlDevice::SetRenderTarget`
/// calls it too.
void M2W_ForgetGlState();

/// Raised by `M2W_ForgetGlState`: the state memory (`m_kGlCache`) no
/// longer matches OpenGL. Cleared - and the memory thrown away - at the
/// start of `PrepareDrawFromBuffer`, before the program check.
extern bool g_bGlStateUnknown;

// --- texture ---------------------------------------------------------------

class CGlLevelSurface;

/// `IDirect3DTexture8` on WebGL 2: every level keeps its own copy in wasm
/// memory (principle one, gl_device.cpp), sent to the card on `UnlockRect`
/// (`UploadLevel`).
class CGlTexture : public IDirect3DTexture8, public CGlResource
{
public:
    /// Allocates the wasm copy of every level (`uLevels` 0 is treated as 1,
    /// see `UploadLevel` about the chain) and a GL texture name.
    CGlTexture(UINT uWidth, UINT uHeight, UINT uLevels, D3DFORMAT eFormat);
    /// Deletes the framebuffer, depth buffer and texture it owns.
    ~CGlTexture() override;

    ULONG AddRef() override  { return AddReference(); }
    ULONG Release() override { return ReleaseReference(); }

    /// A new `CGlLevelSurface` viewing level `Level` (defined in
    /// gl_textures.cpp because it returns that class).
    HRESULT GetSurfaceLevel(UINT Level, IDirect3DSurface8** ppSurfaceLevel) override;

    /// Hands out a pointer into OUR copy of the level (principle one) and
    /// its pitch - for DXT the pitch of a row of 4x4 blocks.
    HRESULT LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect,
                     CONST RECT* pRect, DWORD Flags) override;
    /// Sends the level to OpenGL (`UploadLevel`).
    HRESULT UnlockRect(UINT Level) override;
    /// Format and size of level `Level` (halved per level, never below 1).
    HRESULT GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override;

    DWORD GetLevelCount() override { return m_uLevels; }

    /// The OpenGL texture name (generated in the constructor).
    GLuint GlName() const { return m_uGlName; }

    // -----------------------------------------------------------------------
    // DRAWING TO A TEXTURE
    // -----------------------------------------------------------------------
    // The client asks for it in four places and all of them are real:
    //
    //     GrpShadowTexture.cpp            character shadow
    //     MapOutdoorCharacterShadow.cpp   shadow map on the terrain
    //     SnowEnvironment.cpp (x2)        snow blur
    //
    // Earlier the layer REFUSED - deliberately and completely (see
    // the note at `SetRenderTarget`, gl_render_target.cpp). The refusal was
    // right then, because a partial refusal cleared the real screen to
    // white. But it cost two things at once:
    //
    //   * `syserr.txt` grew by 25 kB PER SECOND - the client reports the
    //     failed shadow-map clear in EVERY frame ("Unable to Clear");
    //   * the shadow map stayed as it was after creation, and the terrain
    //     multiplies its colour by it (`SetTexture(1, m_lpCharacterShadowMapTexture)`
    //     and `D3DTOP_MODULATE` in `MapOutdoorRenderSTP.cpp`). Multiplying
    //     by black gives black - and that is one of the reasons for the dark
    //     ground.
    //
    // In OpenGL this is a FRAMEBUFFER OBJECT: the texture as the colour
    // attachment plus a depth buffer. We create it only when the client
    // asks for a texture with `D3DUSAGE_RENDERTARGET` - ordinary textures
    // pay nothing for it.
    /// True when the texture was prepared as a render target (it has a
    /// complete framebuffer).
    bool IsTarget() const { return m_uFramebuffer != 0; }
    /// The framebuffer object drawing into this texture (0 when not a target).
    GLuint Framebuffer() const { return m_uFramebuffer; }
    /// Width of level 0 in pixels.
    UINT GetWidth() const { return m_uWidth; }
    /// Height of level 0 in pixels.
    UINT GetHeight() const { return m_uHeight; }

    /// Prepares the texture as a render target: one RGBA level, depth
    /// renderbuffer, framebuffer (left at 0 if incomplete). Called once, at
    /// creation.
    void PrepareAsTarget();

    /// Does any sampling parameter differ from the one already sent?
    bool SamplerChanged(GLint iMinFilter, GLint iMagFilter,
                        GLint iWrapS, GLint iWrapT) const;

    /// Sets the sampling - but ONLY what changed (the texture must be bound).
    void SetSampler(GLint iMinFilter, GLint iMagFilter,
                    GLint iWrapS, GLint iWrapT);

    /// Sends a level to OpenGL. This is where the pixel reordering of
    /// `d3d8_states.cpp` happens - and only here, once per upload, not every
    /// frame.
    void UploadLevel(UINT uLevel);

    /// The Direct3D format given at creation.
    D3DFORMAT Format() const { return m_eFormat; }

private:
    /// The sampling parameters sent last - see `SetSampler`.
    /// `-1` means "we do not know", so the first time always goes to the card.
    GLint m_aiSampler[4] = { -1, -1, -1, -1 };

    std::vector<unsigned char> m_kHiResBuffer;   ///< font atlas at the GUI scale

    /// The fallback for browsers without `WEBGL_compressed_texture_s3tc`:
    /// decoding the blocks in wasm and sending plain pixels.
    ///
    /// Runs ONCE per texture level, at load time - not on the hot path. It
    /// costs four times more card memory and there is no way round that:
    /// if the card cannot read blocks, it has to get pixels.
    void UploadDecodedDxt(UINT uLevel, UINT uWidth, UINT uHeight,
                          const std::vector<unsigned char>& c_rData);

    UINT m_uWidth;
    UINT m_uHeight;
    UINT m_uLevels;
    D3DFORMAT m_eFormat;
    TTextureFormat m_kFormat;

    GLuint m_uGlName;
    bool m_bUploaded;

    /// Framebuffer and depth - only for textures that are render TARGETS.
    /// Zero means "an ordinary texture" and costs nothing.
    GLuint m_uFramebuffer = 0;
    GLuint m_uDepthBuffer = 0;

    /// The highest level of detail that really went to the card.
    /// See `UploadLevel` - whether the texture is COMPLETE depends on it.
    UINT m_uHighestUploaded = 0;

    std::vector<std::vector<unsigned char> > m_akLevels;
    std::vector<unsigned char> m_kUploadBuffer;
};

// --- surface ---------------------------------------------------------------

/// A surface is a **description** here, not memory. The client uses it
/// only to ask for the size of the screen buffer (`GetBackBuffer`,
/// `GetDepthStencilSurface`) and for comparisons. Nobody writes to it.
class CGlSurface : public IDirect3DSurface8, public CGlResource
{
public:
    /// A surface description of the given size and format (no memory behind it).
    CGlSurface(UINT uWidth, UINT uHeight, D3DFORMAT eFormat)
        : m_uWidth(uWidth), m_uHeight(uHeight), m_eFormat(eFormat) {}

    ULONG AddRef() override  { return AddReference(); }
    ULONG Release() override { return ReleaseReference(); }

    /// Format and size given at creation.
    HRESULT GetDesc(D3DSURFACE_DESC* pDesc) override;

    /// Always fails - see the note inside.
    HRESULT LockRect(D3DLOCKED_RECT*, CONST RECT*, DWORD) override
    {
        // Reading the screen buffer is `glReadPixels` in OpenGL, which
        // **stalls the pipeline**. The client does it for a screenshot and
        // nowhere else. It stays undefined until it is needed - and then
        // with full awareness of the cost.
        return E_FAIL;
    }

    HRESULT UnlockRect() override { return E_FAIL; }

private:
    UINT m_uWidth;
    UINT m_uHeight;
    D3DFORMAT m_eFormat;
};

// --- texture level surface -------------------------------------------------

/// THE TABLE OF SURFACES THAT ARE VIEWS OF A TEXTURE.
///
/// `SetRenderTarget` (gl_render_target.cpp) gets an `IDirect3DSurface8*` and
/// has to decide whether it is a view of a texture (then we bind its
/// framebuffer) or the screen buffer (then we go back to the screen). Both
/// classes derive from the same interface, so a blind cast would be a guess
/// - and a wrong guess writes to random memory.
///
/// The table is small: the client creates four render targets for the whole
/// game. Defined in gl_textures.cpp (together with `CGlLevelSurface`, which
/// fills it).
extern std::map<IDirect3DSurface8*, CGlTexture*> g_kTextureSurfaces;

/// A view of ONE level of detail of a texture.
///
/// The client uses it in one place and for one thing: `GrpImageTexture.cpp`
/// takes the source and target surfaces and has `D3DXLoadSurfaceFromSurface`
/// copy one into the other - when shrinking textures and converting to a
/// smaller format.
///
/// It is a **view**, not a copy: `LockRect` goes straight to the texture. A
/// copy would mean that writing to the surface never reaches the texture -
/// and the whole copy would silently do nothing.
///
/// The surface HOLDS a reference to the texture. Without it a client that
/// releases the texture before the surface - which is exactly what
/// `GrpImageTexture.cpp` does - would be left with a pointer to freed memory.
class CGlLevelSurface : public IDirect3DSurface8, public CGlResource
{
public:
    /// Takes a reference to `pOwner` and registers itself in
    /// `g_kTextureSurfaces`.
    CGlLevelSurface(CGlTexture* pOwner, UINT uLevel);

    ULONG AddRef() override  { return CGlResource::AddReference(); }
    ULONG Release() override { return CGlResource::ReleaseReference(); }

    /// The texture this surface looks at.
    ///
    /// Needed by `SetRenderTarget`: Direct3D names the RENDER TARGET as a
    /// surface, while OpenGL binds a framebuffer set up on the TEXTURE.
    /// Without this step there is no way from one to the other.
    CGlTexture* Texture() const { return m_pTexture; }

    /// The level's description (`CGlTexture::GetLevelDesc`).
    HRESULT GetDesc(D3DSURFACE_DESC* pDesc) override;
    /// Locks the level in the texture itself (`CGlTexture::LockRect`).
    HRESULT LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect,
                     DWORD Flags) override;
    /// Unlocks - and so uploads - the level (`CGlTexture::UnlockRect`).
    HRESULT UnlockRect() override;

private:
    /// Unregisters from `g_kTextureSurfaces` and releases the texture.
    ~CGlLevelSurface() override;

    CGlTexture* m_pTexture;
    UINT m_uLevel;
};

// --- buffers ---------------------------------------------------------------

/// Common to the vertex and index buffer: a copy in wasm memory, sent to
/// OpenGL on `Unlock`.
class CGlBuffer : public CGlResource
{
public:
    /// A wasm copy of `uLength` bytes (at least 1) and a GL buffer name for
    /// `eTarget` (GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER).
    CGlBuffer(UINT uLength, GLenum eTarget);
    /// Deletes the GL buffer.
    ~CGlBuffer() override;

    /// Hands out a pointer into the wasm copy at `uOffset` and remembers
    /// the range to upload (`uLength` 0 = to the end); a range beyond the
    /// buffer is refused with E_FAIL and one log line.
    HRESULT LockData(UINT uOffset, UINT uLength, BYTE** ppbData);
    /// Uploads: the whole buffer the first time, then only the locked range.
    HRESULT UnlockData();

    /// The OpenGL buffer name.
    GLuint GlName() const { return m_uGlName; }
    /// Size of the buffer in bytes (at least 1).
    UINT Size() const { return static_cast<UINT>(m_kData.size()); }

    /// The content's copy in wasm memory. For MEASUREMENT, not for drawing -
    /// drawing goes from the buffer on the card.
    const unsigned char* Data() const
    {
        return m_kData.empty() ? NULL : &m_kData[0];
    }

protected:
    std::vector<unsigned char> m_kData;
    GLenum m_eTarget;
    GLuint m_uGlName;
    UINT m_uDirtyStart;
    UINT m_uDirtyLength;
    bool m_bAllocated = false;
    bool m_bWarnedRange = false;
};

/// `IDirect3DVertexBuffer8` on `CGlBuffer`. `FVF()` returns the vertex
/// layout code given to `CreateVertexBuffer` - today with no caller in
/// compat/ (the port's `IDirect3DVertexBuffer8` has no `GetFVF` in its
/// interface, d3d8.h:529-535); the layout for drawing takes a separate
/// path, through `CGlDevice::m_dwFVF`/`SetVertexShader`.
class CGlVertexBuffer : public IDirect3DVertexBuffer8, public CGlBuffer
{
public:
    /// A vertex buffer of `uLength` bytes; `dwFVF` is only remembered.
    CGlVertexBuffer(UINT uLength, DWORD dwFVF)
        : CGlBuffer(uLength, GL_ARRAY_BUFFER), m_dwFVF(dwFVF) {}

    ULONG AddRef() override  { return AddReference(); }
    ULONG Release() override { return ReleaseReference(); }

    HRESULT Lock(UINT uOffset, UINT uLength, BYTE** ppbData, DWORD) override
    {
        return LockData(uOffset, uLength, ppbData);
    }
    HRESULT Unlock() override { return UnlockData(); }

    /// The vertex format given to `CreateVertexBuffer` (see the class note: no
    /// caller today).
    DWORD FVF() const { return m_dwFVF; }

private:
    DWORD m_dwFVF;
};

/// `IDirect3DIndexBuffer8` on `CGlBuffer`; `GlType()`/`BytesPerIndex()` carry
/// the index width (16 or 32 bits) chosen at creation.
class CGlIndexBuffer : public IDirect3DIndexBuffer8, public CGlBuffer
{
public:
    /// An index buffer of `uLength` bytes; D3DFMT_INDEX32 gives 32-bit indices,
    /// anything else 16-bit.
    CGlIndexBuffer(UINT uLength, D3DFORMAT eFormat)
        : CGlBuffer(uLength, GL_ELEMENT_ARRAY_BUFFER),
          m_eGlType((eFormat == D3DFMT_INDEX32) ? GL_UNSIGNED_INT
                                               : GL_UNSIGNED_SHORT),
          m_uBytesPerIndex((eFormat == D3DFMT_INDEX32) ? 4u : 2u) {}

    ULONG AddRef() override  { return AddReference(); }
    ULONG Release() override { return ReleaseReference(); }

    HRESULT Lock(UINT uOffset, UINT uLength, BYTE** ppbData, DWORD) override
    {
        return LockData(uOffset, uLength, ppbData);
    }
    HRESULT Unlock() override { return UnlockData(); }

    /// GL_UNSIGNED_INT or GL_UNSIGNED_SHORT, for `glDrawElements`.
    GLenum GlType() const { return m_eGlType; }
    /// 4 or 2.
    UINT BytesPerIndex() const { return m_uBytesPerIndex; }

private:
    GLenum m_eGlType;
    UINT m_uBytesPerIndex;
};

// --- program (a shader pair plus input locations) --------------------------

/// A compiled program: the GL handle plus the locations of every input
/// `CGlDevice` may need to send. `-1` (what `glGetUniformLocation`/
/// `glGetAttribLocation` return for a name they do not find) means "this
/// program does not have it" - a shader may leave out inputs it does
/// not use.
struct TProgram
{
    GLuint uProgram;

    GLint iPosition;
    GLint iNormal;
    GLint iColor;
    GLint iSpecular;
    GLint aiTexCoords[M2W_TEXTURE_STAGES];

    GLint iWorldViewProjection;
    GLint iWorldView;
    GLint iTargetSize;
    GLint iHalfPixel;
    GLint iFlipY;
    GLint iTextureMatrix;
    GLint iLightDirection;
    GLint iLightColor;
    GLint iLightAmbient;
    GLint iMaterialDiffuse;
    GLint iMaterialEmissive;
    GLint iMaterialAmbient;
    GLint iGlobalAmbient;
    GLint iFogRange;
    GLint iFogDensity;
    GLint iTexture0;
    GLint iTexture1;
    GLint iTextureFactor;
    GLint iFogColor;
    GLint iAlphaRef;
};

/// Compiles one shader (vertex or pixel) from GLSL source composed by
/// `d3d8_fixedfunc.cpp`. Returns 0 and prints the whole source together
/// with the error when the driver refuses - `FindOrBuildProgram`
/// (gl_shaders.cpp) checks it explicitly
/// (`if (!uVertexShader || !uPixelShader) return NULL;`) before
/// `glAttachShader`, so a zero never goes on silently.
GLuint CompileShader(GLenum eKind, const std::string& c_rSource);

// ===========================================================================
// The device (declared here; method bodies in gl_*.cpp - split
// out of gl_device.cpp module by module)
// ===========================================================================

/// `IDirect3DDevice8` on WebGL 2 - the whole device the game draws on.
/// The Direct3D state is recorded in arrays and applied to OpenGL only at
/// draw time (principle three, gl_device.cpp header).
class CGlDevice : public IDirect3DDevice8, public CGlResource
{
public:
    /// Sets Direct3D's meaningful initial states, identity matrices, and
    /// creates the vertex array and the two scratch buffers. `iWidth`/
    /// `iHeight` are the LOGICAL screen size.
    CGlDevice(int iWidth, int iHeight);

    /// Deletes cached programs and GL objects, releases held references.
    ~CGlDevice() override;

    ULONG AddRef() override  { return CGlResource::AddReference(); }
    ULONG Release() override { return CGlResource::ReleaseReference(); }

    /// Number of programs in the cache, over all key-hash buckets.
    int CachedProgramCount() const;

    // -----------------------------------------------------------------------
    // Creating resources
    // -----------------------------------------------------------------------

    /// A new `CGlTexture`; an unknown format fails with NULL (and a log line);
    /// `D3DUSAGE_RENDERTARGET` also gets a framebuffer (`PrepareAsTarget`).
    HRESULT CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                          D3DFORMAT Format, D3DPOOL /*Pool*/,
                          IDirect3DTexture8** ppTexture) override;

    /// A new `CGlVertexBuffer` of `Length` bytes.
    HRESULT CreateVertexBuffer(UINT Length, DWORD /*Usage*/, DWORD FVF,
                               D3DPOOL /*Pool*/,
                               IDirect3DVertexBuffer8** ppVertexBuffer) override;

    /// A new `CGlIndexBuffer` of `Length` bytes, 16- or 32-bit indices.
    HRESULT CreateIndexBuffer(UINT Length, DWORD /*Usage*/, D3DFORMAT Format,
                              D3DPOOL /*Pool*/,
                              IDirect3DIndexBuffer8** ppIndexBuffer) override;

    /// A depth surface is only a description here (size and format) - the
    /// real depth buffer belongs to the canvas or to a target texture.
    HRESULT CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                      D3DMULTISAMPLE_TYPE,
                                      IDirect3DSurface8** ppSurface) override;

    /// Description of the render target or depth surface - ONE OBJECT, not a
    /// new one per query. Returns it with a reference added for
    /// the caller.
    ///
    /// Layer audit: `MapOutdoorCharacterShadow.cpp:111` and `:117` ask for
    /// the target and the depth TWICE EVERY FRAME, and `GrpShadowTexture.cpp`
    /// and `SnowEnvironment.cpp` add theirs - at least four heap allocations
    /// and four frees per frame for objects that carry only a SIZE and a
    /// FORMAT.
    ///
    /// The size can change when the window does, so the cached object is
    /// valid only while the dimensions match - otherwise it is made anew.
    CGlSurface* SurfaceDescription(CGlSurface*& rpCached,
                                     D3DFORMAT eFormat);

    /// The current render target's description (`SurfaceDescription`).
    HRESULT GetRenderTarget(IDirect3DSurface8** ppRenderTarget) override;

    /// The depth surface's description (`SurfaceDescription`, D3DFMT_D16).
    HRESULT GetDepthStencilSurface(IDirect3DSurface8** ppZStencilSurface) override;

    /// A view of a render-target texture binds its framebuffer; any other
    /// surface (the back buffer) or NULL returns to the screen; a texture
    /// without a framebuffer is refused completely (`m_bTargetRefused`).
    HRESULT SetRenderTarget(IDirect3DSurface8* pTarget, IDirect3DSurface8*) override;

    /// The viewport as last set, in Direct3D terms (logical px on screen).
    HRESULT GetViewport(D3DVIEWPORT8* pViewport) override;

    /// Converts the Direct3D viewport (top-left origin) to glViewport
    /// (bottom-left) against the CURRENT target's height; GUI scale on screen.
    HRESULT SetViewport(CONST D3DVIEWPORT8* pViewport) override;

    /// Clears colour/depth/stencil of the current target (depth writes forced
    /// on for the clear); refused while `m_bTargetRefused`.
    HRESULT Clear(DWORD, CONST D3DRECT*, DWORD Flags, D3DCOLOR Color,
                  float Z, DWORD Stencil) override;

    /// Switches light `Index` (0..LIGHT_COUNT-1) on or off; others ignored.
    HRESULT LightEnable(DWORD Index, BOOL Enable) override;

    // --- gamma ramp ---------------------------------------------------------
    /// Remembers the ramp (no canvas filter yet - reported once as a stub).
    void SetGammaRamp(DWORD, CONST D3DGAMMARAMP* pRamp) override;

    /// The remembered ramp, or the identity ramp (i * 257) before any Set.
    void GetGammaRamp(D3DGAMMARAMP* pRamp) override;

    /// The screen buffer - the same description as `GetRenderTarget`.
    HRESULT GetBackBuffer(UINT, D3DBACKBUFFER_TYPE,
                          IDirect3DSurface8** ppBackBuffer) override;

    /// Stream 0 only (others ignored): holds a reference to the buffer
    /// and remembers the stride.
    HRESULT SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8* pStreamData,
                            UINT Stride) override;

    /// No-ops: WebGL has no scene brackets.
    HRESULT BeginScene() override { return D3D_OK; }
    HRESULT EndScene() override { return D3D_OK; }

    /// Nothing to do - the browser shows the frame when the callback returns.
    HRESULT Present(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) override;

    /// Takes the new back-buffer size: resizes the canvas (physical px) and
    /// resets the viewport to the whole buffer.
    HRESULT Reset(D3DPRESENT_PARAMETERS* pParams) override;

    /// DEVICELOST only when the WebGL context is really lost.
    HRESULT TestCooperativeLevel() override;

    /// The shared answer of `M2W_GlCapabilities`.
    HRESULT GetDeviceCaps(D3DCAPS8* pCaps) override;

    /// A fixed 256 MB - OpenGL cannot tell, and zero would mean "nothing fits".
    UINT GetAvailableTextureMem() override;

    // -----------------------------------------------------------------------
    // States - recorded, applied at draw time
    // -----------------------------------------------------------------------

    /// Records a render state; OpenGL gets it at the next draw.
    HRESULT SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;

    /// The recorded render state (0 outside the table).
    HRESULT GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override;

    /// Records a texture-stage state; it enters the pipeline key at draw time.
    HRESULT SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                 DWORD Value) override;

    /// The recorded texture-stage state (0 outside the table).
    HRESULT GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                 DWORD* pValue) override;

    /// Puts a texture on a stage, holding our own reference to it.
    HRESULT SetTexture(DWORD Stage, IDirect3DBaseTexture8* pTexture) override;

    /// The texture on a stage (no reference added - as earlier).
    HRESULT GetTexture(DWORD Stage, IDirect3DBaseTexture8** ppTexture) override;

    /// Records the world, view, projection or texture 0/1 matrix; others ignored.
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE State,
                         CONST D3DMATRIX* pMatrix) override;

    /// The recorded matrix; identity for any other transform.
    HRESULT GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override;

    /// Records the material (sent as uniforms at draw time).
    HRESULT SetMaterial(CONST D3DMATERIAL8* pMaterial) override;

    /// The recorded material.
    HRESULT GetMaterial(D3DMATERIAL8* pMaterial) override;

    /// Records light `Index` - lights are NUMBERED (see m_akLights).
    HRESULT SetLight(DWORD Index, CONST D3DLIGHT8* pLight) override;

    /// The recorded light `Index`.
    HRESULT GetLight(DWORD Index, D3DLIGHT8* pLight) override;

    /// Holds a reference to the index buffer; remembers BaseVertexIndex.
    HRESULT SetIndices(IDirect3DIndexBuffer8* pIndexData,
                       UINT BaseVertexIndex) override;

    // -----------------------------------------------------------------------
    // Shaders - see the note at the top of gl_device.cpp
    // -----------------------------------------------------------------------

    /// A new handle (top bit set); a declaration becomes a vertex layout
    /// (`m_kDeclarationLayouts`), a function body marks it as real code
    /// (such draws are skipped - see `PrepareDrawFromBuffer`).
    HRESULT CreateVertexShader(CONST DWORD* pDeclaration, CONST DWORD* pFunction,
                               DWORD* pHandle, DWORD) override;

    /// Records an FVF code or a shader handle (told apart by the top bit).
    HRESULT SetVertexShader(DWORD Handle) override;

    /// The FVF code or handle set last.
    HRESULT GetVertexShader(DWORD* pHandle) override;

    /// Forgets the handle's layout and code mark.
    HRESULT DeleteVertexShader(DWORD Handle) override;

    /// Ignored; reported once as a stub.
    HRESULT SetVertexShaderConstant(DWORD, CONST void*, DWORD) override;

    /// A non-zero handle to a shader that never runs (reported as a stub).
    HRESULT CreatePixelShader(CONST DWORD*, DWORD* pHandle) override;

    /// Records the handle (not used for drawing).
    HRESULT SetPixelShader(DWORD Handle) override;

    /// Ignored; reported once as a stub.
    HRESULT DeletePixelShader(DWORD) override;

    /// Ignored; reported once as a stub.
    HRESULT SetPixelShaderConstant(DWORD, CONST void*, DWORD) override;

    // -----------------------------------------------------------------------
    // Drawing
    // -----------------------------------------------------------------------

    /// glDrawArrays from the current stream (after `PrepareDraw`).
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                          UINT PrimitiveCount) override;

    /// The forest log: to the console AND to the file `/las.txt` (the
    /// browser console loses the first lines in a flood; the file survives
    /// - like syserr.txt).
    static void ForestLog(const char* c_szFormat, ...);

    /// MEASURING TREE DRAWING. `speedtree_web.cpp` raises
    /// `g_iM2wForestNext` in every `GetGeometry`, i.e. right before every
    /// draw of branches/fronds/leaves. The first eight such draws get a full
    /// description of the state here: layout, world translation, textures,
    /// alpha test, first vertex - and the GL error after the draw. With
    /// `?forest=N` also the in-frame count and the N experiments (see the
    /// `forest` row of m2w.options).
    void ReportForestDraw(const char* c_szWhat, D3DPRIMITIVETYPE eType, UINT uPrimitives);

    /// A ONE-OFF REPORT ABOUT THE FIRST TERRAIN PATCH.
    ///
    /// The ground is black, while the sky and distant mountains draw
    /// correctly. The terrain goes the STP road (`MapOutdoorRenderSTP.cpp`),
    /// i.e. with vertices ALREADY TRANSFORMED on the CPU - Direct3D does not
    /// light those at all, so the colour comes from only two things: the
    /// VERTEX COLOUR and the texture stages.
    ///
    /// This report decides which of them fails, and does it with one entry
    /// into the game instead of another round of guessing:
    ///
    ///     vertex colour black   -> the game computes it, the cause is above
    ///     texture 0 equal zero  -> we did not bind it, the cause is here
    ///
    /// The vertex layout is given by `D3DFVF_XYZRHW|DIFFUSE|SPECULAR|TEX2`
    /// (ibid., line 46): 4 numbers, two colours, two coordinate pairs.
    /// (Now it prints the first eight DIFFERENT layouts instead -
    /// see the note in the body.)
    void ReportTerrain();

    /// glDrawElements from the current stream and index buffer, with the
    /// tree reports and the samples query around it.
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT,
                                 UINT, UINT StartIndex,
                                 UINT PrimitiveCount) override;

    /// Draws from client memory through the scratch vertex buffer.
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                            CONST void* pVertexStreamZeroData,
                            UINT VertexStreamZeroStride) override;

    /// Draws from client memory through the scratch vertex and index buffers.
    HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT,
                                   UINT NumVertices, UINT PrimitiveCount,
                                   CONST void* pIndexData,
                                   D3DFORMAT IndexDataFormat,
                                   CONST void* pVertexStreamZeroData,
                                   UINT VertexStreamZeroStride) override;

    // -----------------------------------------------------------------------
    // ROAD B - drawing that bypasses the fixed-function pipeline
    // -----------------------------------------------------------------------

    /// The alpha test for our own drawing road - from the Direct3D state
    /// THE SAME D3DCMP_* table as `AlphaRejection` in
    /// d3d8_fixedfunc.cpp. 0 = test off. Earlier our own road had a
    /// hard-coded `discard < 0.05`, which cut the gloss mask out of armours.
    int AlphaTestFunc() const;
    /// D3DRS_ALPHAREF as 0..1.
    float AlphaTestRef() const;

    /// Draws what the game has just asked to draw, but by OUR OWN road.
    ///
    /// All input comes from the device's current state, because the game has
    /// set it already: the vertex stream, the indices, the stage-zero texture
    /// and three matrices. Nothing is copied - Direct3D 8 buffers in this
    /// port ARE OpenGL buffers.
    ///
    /// Returns `false` when the state does not suit our road (or `?nocustom=1`).
    /// Then the caller has to draw the old way, not draw nothing.
    bool DrawCustom(UINT uFirstIndex, UINT uTriangles);

private:
    enum
    {
        RENDER_STATE_COUNT = 256,
        STAGE_STATE_COUNT = 32,
        /// The top bit tells a shader handle from an FVF code. An FVF never
        /// has it - its highest used bit would be 31 with the last set of
        /// coordinates, but the client uses three at most.
        SHADER_HANDLE_BIT = 0x80000000u,
    };

    /// One program cache entry: the key it was built for and the program.
    struct TProgramCacheEntry
    {
        TPipelineKey kKey;
        TProgram kProgram;
    };

    // --- building the key from the current state ----------------------------

    /// Fills `m_kKey` from the stage states, bound textures, the vertex
    /// layout and the render states (with the `?shadow=1|2|3` experiment).
    void BuildKey(const TVertexLayout& c_rLayout);

    /// Field-by-field key comparison (not memcmp - padding holds garbage).
    bool KeysEqual(const TPipelineKey& a, const TPipelineKey& b) const;

    /// The program for `m_kKey`: the last result, else the cache bucket by
    /// key hash, else compile and link a new one; NULL if that fails.
    const TProgram* FindOrBuildProgram();

    // --- applying state -------------------------------------------------------
    //
    // THE STATE MEMORY. Direct3D 8 itself took care not to send the same thing
    // twice; our layer sent everything on every draw, because that is the
    // simplest to write. Measured on THE LOGIN SCREEN ALONE: 1875 GL calls
    // per frame at 50 draws - thirty-eight per draw, almost all of them
    // repeating state already set. In the game there are hundreds of draws,
    // and that is the difference between smoothness and stutter.
    //
    // `-1` in every field means "WE DO NOT KNOW". The first use always goes
    // to the card, so the memory never skips something that was not set.
    struct TGlStateCache
    {
        int iDepthTest = -1;
        int iDepthWrite = -1;
        GLenum eDepthFunc = 0;
        int iBlend = -1;
        GLenum eBlendSrc = 0;
        GLenum eBlendDst = 0;
        GLenum eBlendEquation = 0;
        int iColorMask = -1;
        int iCull = -1;
        GLenum eCullFace = 0;
        int iActiveUnit = -1;
        GLuint auBound[M2W_TEXTURE_STAGES] = { 0 };
        int aiBoundKnown[M2W_TEXTURE_STAGES] = { 0 };
        unsigned uEnabledAttribs = 0;
        int iAttribsKnown = 0;
        GLuint uProgram = 0;
        int iProgramKnown = 0;
    };
    TGlStateCache m_kGlCache;

    /// glEnable/glDisable of `eCap` - only when it differs from `riCached`.
    void SetEnabled(GLenum eCap, int& riCached, bool bEnabled);

    /// Applies the recorded depth, blend, colour-mask and cull states to
    /// OpenGL through the state memory (once per draw).
    void ApplyRenderStates();

    /// Binds the stage textures and their sampling through the state memory
    /// (forgotten first if `g_bTextureBindingsUnknown`), then the sampler
    /// uniforms 0 and 1.
    void ApplyTextures(const TProgram& c_rProgram);

    /// World * view * projection with the depth fix - see principle two at
    /// the top of gl_device.cpp: `z' = 2z - w` in Direct3D's matrix
    /// notation (row times matrix) - and the pixel-centre fix
    /// `x' = x + w/W`, `y' = y - w/H` (see ClipHalfPixel).
    void BuildFinalMatrix(D3DXMATRIX* pResult) const;

    /// Sends every uniform of `c_rProgram` from the recorded Direct3D state,
    /// once per draw, in a fixed order (matrices, colour/fog/alpha, material,
    /// light), then the shadow-pass report. Split into stages.
    void ApplyUniforms(const TProgram& c_rProgram);
    /// Final, world-view and texture matrices, render-target size, Y flip
    /// and half-pixel offset.
    void ApplyMatrixUniforms(const TProgram& c_rProgram);
    /// D3DRS_TEXTUREFACTOR, fog colour/range/density, alpha-test reference.
    void ApplyColourUniforms(const TProgram& c_rProgram);
    /// Material diffuse/emissive/ambient and the global ambient (D3DRS_AMBIENT).
    void ApplyMaterialUniforms(const TProgram& c_rProgram);
    /// Direction (moved to view space), colour and ambient of `c_pLight`,
    /// the first enabled light; NULL sends zeros (no light shines).
    void ApplyLightUniforms(const TProgram& c_rProgram, const D3DLIGHT8* c_pLight);
    /// Diagnostic: on every 6000th terrain shadow pass prints the light
    /// and material to the log.
    void ReportShadowPass(const D3DLIGHT8* c_pLight);
    /// Diagnostic, every 10th report: reads back the centre and corner
    /// pixel of the two shadow textures (a pipeline stall - kept rare).
    void ReadShadowTargets();

    /// Reinterprets the bits of a render-state DWORD as a float (fog
    /// start/end/density are passed that way).
    static float BitsToFloat(DWORD dw);

    /// Points the program's attributes at the buffer by `c_rLayout` (shifted
    /// by BaseVertexIndex) and enables only the attributes that changed.
    void ApplyVertexLayout(const TProgram& c_rProgram,
                           const TVertexLayout& c_rLayout,
                           UINT uStride);

    /// The common part of preparing a draw from the current stream. Returns
    /// `false` when drawing is not possible - then the caller must NOT draw,
    /// rather than draw with anything.
    bool PrepareDraw();

    /// Y FLIP WHEN DRAWING TO A TEXTURE. Direct3D puts row 0 of
    /// the target (v = 0) at the TOP of the image, OpenGL at the BOTTOM (the
    /// framebuffer grows upwards). A texture loaded from a file has row 0 at
    /// the top in both, so ordinary textures agree - but one RENDERED to a
    /// texture is flipped vertically relative to Direct3D. The consumer (the
    /// terrain) computes `v` the Direct3D way (`0.5 - y/2550`), so without the
    /// flip the shadow of a point above the map centre lands below it: the
    /// player's shadow (at the centre) almost in place, the mobs' shadows
    /// mirrored to the other side of the player - they "move" with every
    /// step. The flip multiplies clip-space `y` by -1 and SWAPS the cull
    /// side (flipping reverses the vertex order). True when drawing to a
    /// texture unless `?noflip=1` switches it off
    /// for an A/B test.
    bool TargetFlipped() const;

    /// `?shadow=0` - SKIP THE PASS THAT MULTIPLIES THE
    /// SCREEN BY ITSELF.
    ///
    /// Not a fix but a DECIDING EXPERIMENT. The terrain is drawn in two
    /// passes; the second, the shadows, mixes with the screen by
    /// multiplication (`SRCBLEND_ZERO`, `DESTBLEND_SRCCOLOR`) and concerns
    /// the patches near the character. If the black goes away when it is
    /// off - the cause is there and I know what to look for. If it stays -
    /// my diagnosis is wrong and the search goes elsewhere.
    ///
    /// Two rounds of guessing cost more than this one switch. The value of
    /// `?shadow=N`, read once: -1 absent, 0 skip the pass,
    /// 1/2/3 pretend there is no texture on stage 0/1/both (see `BuildKey`).
    static int ShadowExperimentMode();

    /// Is this the terrain shadow pass? It is recognised unambiguously by its
    /// blending: multiplying the screen by itself (`SRCBLEND_ZERO`,
    /// `DESTBLEND_SRCCOLOR`) is done in the whole client only by the
    /// terrain shadow.
    bool IsShadowPass() const;

    /// `?shadow=0` and this is the shadow pass.
    bool SkipShadowPass() const;

    /// The vertex layout of the coming draw into `rLayout`: from the current
    /// shader handle's `D3DVSD_*` declaration, else decoded from the FVF code
    /// (both remembered per change of handle / code). `false` (counted in the
    /// census) for a draw with a real assembly shader or a declaration the
    /// layer cannot put together.
    bool SelectVertexLayout(TVertexLayout& rLayout);

    /// Prepares a draw from GL buffer `uBuffer`: forgets stale state, skips
    /// real-shader draws and unusable layouts (counted in the census), builds
    /// the key, binds the program, applies states, layout, textures and
    /// uniforms. `false` = do not draw.
    bool PrepareDrawFromBuffer(GLuint uBuffer, UINT uStride);

    // --- state ------------------------------------------------------------------

    int m_iWidth;    ///< LOGICAL size (what the game considers the screen)
    int m_iHeight;

public:
    /// Physical buffer pixels from logical ones (GUI scale).
    static int ToPhysical(int iLogical);

    /// Half a PHYSICAL pixel of the current viewport in clip units (`*pfX`
    /// = 1/width, `*pfY` = 1/height; 0 for an empty viewport): the shift
    /// that puts Direct3D's pixel centres onto OpenGL's. The shift
    /// belongs BEFORE the render-target flip; `bFlipAlreadyApplied` (road B,
    /// whose projection is flipped on the CPU first) negates `*pfY` when the
    /// target is flipped, so that both roads end with the same image.
    void ClipHalfPixel(float* pfX, float* pfY, bool bFlipAlreadyApplied = false) const;
private:

    DWORD m_adwRenderState[RENDER_STATE_COUNT];
    DWORD m_aadwStageState[M2W_TEXTURE_STAGES][STAGE_STATE_COUNT] = {};
    CGlTexture* m_apTextures[M2W_TEXTURE_STAGES];

    // The DERIVED type (`D3DXMATRIX`), not the base `D3DMATRIX`, so the D3DX
    // helpers (`D3DXMatrixMultiply`, `D3DXMatrixIdentity`) take them without
    // a cast. (The original said "see the note above"; no such note existed
    // even in the file's first version.)
    D3DXMATRIX m_matWorld;
    D3DXMATRIX m_matView;
    D3DXMATRIX m_matProjection;
    D3DXMATRIX m_amatTexture[M2W_TEXTURE_STAGES];

    D3DMATERIAL8 m_kMaterial;
    // LIGHTS ARE NUMBERED - and that is no detail.
    //
    // Earlier the layer kept ONE light and ONE switch, and threw away
    // the number the client gave. The client uses at least two:
    //
    //     MapOutdoorRender.cpp:161   SetLight(0, terrain light)
    //     PythonGraphic.cpp:86,94    LightEnable(0, TRUE), LightEnable(1, TRUE)
    //     MapManager.cpp:242,247     LightEnable(0, TRUE) / (0, FALSE)
    //
    // With one slot for all, `SetLight(1, ...)` OVERWROTE the terrain light,
    // and `LightEnable(1, ...)` decided whether the terrain was lit. The call
    // that came last won - so the terrain lighting depended on the ORDER of
    // calls about something else.
    //
    // Eight is what Direct3D 8 guarantees on the simplest hardware, and four
    // times what this client uses.
    enum { LIGHT_COUNT = 8 };
    D3DLIGHT8 m_akLights[LIGHT_COUNT];
    bool m_abLightEnabled[LIGHT_COUNT];

    /// The first enabled light, or `NULL`. The layer's shading program knows
    /// ONE directional light - and the client never lights more at once.
    const D3DLIGHT8* FirstEnabledLight() const;

    D3DVIEWPORT8 m_kViewport = {};

    CGlVertexBuffer* m_pStream = NULL;
    UINT m_uStride = 0;
    CGlIndexBuffer* m_pIndices = NULL;
    UINT m_uBaseVertex = 0;

    DWORD m_dwFVF = 0;
    DWORD m_dwVertexShader = 0;
    DWORD m_dwPixelShader = 0;
    DWORD m_dwNextShaderHandle = 0;
    std::set<DWORD> m_kShadersWithCode;

    /// Vertex layouts described by a `D3DVSD_*` DECLARATION, not by an FVF
    /// code. The key is the handle returned by `CreateVertexShader`.
    std::map<DWORD, TVertexLayout> m_kDeclarationLayouts;
    bool m_bWarnedShaders = false;
    bool m_bWarnedDeclarations = false;
    bool m_bClearForcedDepthWrite = false;

    /// The client asked for an OFF-SCREEN render target, and we do not have
    /// it. While that lasts, everything meant for that target must bypass the
    /// screen - see the note at `SetRenderTarget`.
    bool m_bTargetRefused = false;

    /// The texture we are drawing to now. `NULL` means "to the screen".
    /// `SetRenderTarget` holds it and `SetViewport` reads it - because
    /// Direct3D counts the viewport from the top, OpenGL from the bottom,
    /// and the "top" of a target other than the screen is elsewhere.
    CGlTexture* m_pTargetTexture = NULL;

    TPipelineKey m_kKey;
    std::map<DWORD, std::vector<TProgramCacheEntry> > m_kProgramCache;

    // ---- LAST-RESULT MEMORY ------------------------------------
    //
    // The layer audit showed that on EVERY draw we recomputed four things that
    // almost always come out the same: an FNV-1a hash over about a hundred
    // bytes of key, a descent of the `m_kProgramCache` tree (inserting an
    // empty vector on a miss), a linear walk of the bucket, the decoding of
    // the FVF code and two more trees by shader handle.
    //
    // WHY A LAST-RESULT MEMORY AND NOT A "DIRTY STATE" FLAG. The flag would
    // have to be set in EVERY place that touches state going into the key -
    // `SetRenderState`, `SetTextureStageState`, `SetTexture`, `SetMaterial`,
    // lights, fog, FVF. Missing one of them gives A WRONG SHADER WITHOUT A
    // SINGLE ERROR - exactly the kind of silent failure this project chased
    // for a dozen rounds with the dark ground.
    //
    // The last-result memory is immune to omission by design: it compares the
    // ACTUAL INPUT, not its own idea of when that changed. It costs one
    // structure comparison instead of a hash and two trees.
    //
    // Invalidation is needed only where the CONTAINER changes, not the key:
    // on insertion into the cache (because a `std::vector` may move and
    // invalidate the pointer) and on creating and deleting a shader handle.
    bool         m_bLastProgramKnown = false;
    TPipelineKey m_kLastKey;
    const TProgram* m_pLastProgram = NULL;

    bool  m_bFvfLayoutKnown = false;
    DWORD m_dwLastFvf = 0;
    TVertexLayout m_kLastFvfLayout;

    bool  m_bLastShaderKnown = false;
    DWORD m_dwLastShader = 0;
    bool  m_bLastShaderHasCode = false;
    bool  m_bLastShaderHasDeclaration = false;
    TVertexLayout m_kLastDeclarationLayout;

    /// The last gamma ramp given by the game - see `SetGammaRamp`.
    D3DGAMMARAMP m_kGammaRamp;
    bool         m_bGammaRampKnown = false;

    /// The render target and depth descriptions - see `SurfaceDescription`.
    CGlSurface* m_pTargetDesc = NULL;
    CGlSurface* m_pDepthDesc = NULL;

    /// Drops the last-result memories (program, FVF layout, shader). Called
    /// wherever the CONTAINER they draw from changes - not where the
    /// drawing state changes; the key comparison already covers that.
    void ForgetPipelineCache();

    GLuint m_uVertexArray = 0;
    GLuint m_uScratchBuffer = 0;
    GLuint m_uScratchIndexBuffer = 0;
};
