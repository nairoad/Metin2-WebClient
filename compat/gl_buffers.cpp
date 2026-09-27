// SPDX-License-Identifier: GPL-2.0-or-later
// gl_buffers.cpp - vertex and index buffers and DRAWING: the resource
// objects `CGlBuffer`, `CGlVertexBuffer`, `CGlIndexBuffer`, and
// the drawing half of `CGlDevice` - buffer creation,
// streams, every Draw* call, the preparation of a draw
// (`PrepareDrawFromBuffer`: state, key, program, uniforms, textures,
// vertex layout), road B (`DrawCustom`) and the forest/terrain reports.

// Design:
// SAME FIRST PRINCIPLE AS gl_textures.cpp: `Lock` returns OUR buffer, not
// driver memory. Every vertex/index buffer keeps its own copy in wasm
// memory (`m_kData`); `Lock` hands out a pointer into it, `Unlock` sends it
// to OpenGL (`UnlockData`). Both buffer kinds share the copy-and-upload
// machinery (`CGlBuffer`) - only the GL binding target and the wrapping
// D3D8 interface differ.
//
// Split out of d3d8_gl.cpp (the second module of group 7,
// after gl_textures.cpp) - a pure code move, the classes
// declared in gl_internal.h. No method changed its content. The drawing of
// CGlDevice came from gl_device.cpp, together with the draw
// census (`TDrawCensus`) and the forest flags - these methods are their
// only users; bodies unchanged (check_moves.py). Translated;
// the address switches `?nocustom`, `?nocull`, `?shadow` moved into
// m2w.options at the same time.

#include "gl_internal.h"
#include "custom_draw.h"
#include "frame_stats.h"

#include <emscripten/emscripten.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// the flag from speedtree_web.cpp - "the next draw is a tree".
extern int g_iM2wForestNext;
extern int M2W_ForestMode();
extern float g_afM2wForestEye[3], g_afM2wForestDir[3];

/// `?nocustom=1`: characters go
/// through the fixed-function pipeline instead of road B. 1 = off.
EM_JS(int, m2w_no_custom, (void), {
    return m2w.options.get('nocustom') === '1' ? 1 : 0; });

/// `?nocull=1`: no face culling on
/// road B. 1 = no culling.
EM_JS(int, m2w_no_cull, (void), {
    return m2w.options.get('nocull') === '1' ? 1 : 0; });

/// `?shadow=N`: -1 absent or not 0..3,
/// 0 skip the terrain shadow pass, 1/2/3 pretend there is no texture on
/// stage 0/1/both in that pass.
EM_JS(int, m2w_shadow_mode, (void), {
    var v = m2w.options.get('shadow'); if (v === null) return -1;
    var n = parseInt(v, 10); return (n >= 0 && n <= 3) ? n : -1; });

namespace {

// ---------------------------------------------------------------------------
// DRAW CENSUS - so that "the model is not visible" stops being a sentence
// and becomes a number.
// ---------------------------------------------------------------------------
// Three questions it answers without entering the game:
//   - do model draws REACH the layer at all,
//   - do they reach it but get SKIPPED (and for which of four reasons),
//   - are they drawn, just not visible on the screen.
//
// Draws split into FLAT (`XYZRHW` - the interface and terrain drawn in
// software) and SPATIAL (everything that goes through the matrices) - a
// character model is spatial.
//
// The report goes EVERY 100 000 DRAWS (every thousand earlier - see
// `Count`), not every frame, because in this
// port `Present` is NEVER called - checked: 3000 loop turns, zero calls.
// So there is no frame boundary to hook it to.
struct TDrawCensus
{
    long lFlat = 0;
    long lSpatial = 0;
    long lSkippedShader = 0;
    long lSkippedDeclaration = 0;
    long lSkippedNoPosition = 0;
    long lSkippedNoStream = 0;

    /// Once the counters reach 100 000 draws, prints them as one census line and
    /// starts again from zero; below that it does nothing.
    void Count()
    {
        const long lTotal = lFlat + lSpatial + lSkippedShader +
                            lSkippedDeclaration + lSkippedNoPosition +
                            lSkippedNoStream;
        // Every 100 000, not every 1000: at a thousand draws per frame this
        // line went EVERY FRAME and pushed everything else out of the
        // console buffer.
        if (lTotal < 100000)
            return;
        std::printf("m2w census: %ld draws - flat %ld, spatial %ld"
                    " | skipped: shader %ld, declaration %ld,"
                    " no position %ld, no stream %ld\n",
                    lTotal, lFlat, lSpatial, lSkippedShader,
                    lSkippedDeclaration, lSkippedNoPosition,
                    lSkippedNoStream);
        *this = TDrawCensus();
    }
};
TDrawCensus g_kCensus;

}  // namespace

CGlBuffer::CGlBuffer(UINT uLength, GLenum eTarget)
    : m_kData(uLength ? uLength : 1, 0), m_eTarget(eTarget), m_uGlName(0),
      m_uDirtyStart(0), m_uDirtyLength(0)
{
    glGenBuffers(1, &m_uGlName);
}

CGlBuffer::~CGlBuffer()
{
    if (m_uGlName)
        glDeleteBuffers(1, &m_uGlName);
}

HRESULT CGlBuffer::LockData(UINT uOffset, UINT uLength, BYTE** ppbData)
{
    if (!ppbData || uOffset > m_kData.size())
        return E_FAIL;

    // Direct3D: a length of zero means "to the end of the buffer".
    m_uDirtyStart = uOffset;
    m_uDirtyLength = uLength ? uLength
                             : static_cast<UINT>(m_kData.size()) - uOffset;

    // A LOCK BEYOND THE BUFFER - we refuse and say so ONCE.
    //
    // Earlier it passed silently: we handed out a pointer to the
    // start, and `UnlockData` sent `m_uDirtyLength` bytes FROM THAT PLACE to
    // the card - i.e. read past the end of the vector. That is a read of
    // someone else's memory, which need not show at once or in the same
    // place.
    //
    // The caller has the right to get a refusal and deal with it:
    // `CGraphicVertexBuffer::LockRange` then returns `false`, and
    // `ModelInstanceUpdate.cpp` reports "GRANNY DEFORM DYNAMIC BUFFER
    // LOCK ERROR". A loud error is better than a silent read of memory that
    // is not ours.
    if ((size_t)m_uDirtyStart + m_uDirtyLength > m_kData.size())
    {
        if (!m_bWarnedRange)
        {
            m_bWarnedRange = true;
            std::printf("m2w buffer: lock %u+%u beyond a buffer of %u bytes"
                        " - refused\n",
                        (unsigned)m_uDirtyStart,
                        (unsigned)m_uDirtyLength,
                        (unsigned)m_kData.size());
        }
        m_uDirtyLength = 0;
        return E_FAIL;
    }

    *ppbData = &m_kData[uOffset];
    return D3D_OK;
}

HRESULT CGlBuffer::UnlockData()
{
    glBindBuffer(m_eTarget, m_uGlName);
    // The first time the whole buffer has to be allocated; after that it is
    // enough to replace the written piece. The client often locks a small
    // slice (`D3DLOCK_NOOVERWRITE` on dynamic buffers), so sending the whole
    // thing every time would be a cost for no reason.
    if (!m_bAllocated)
    {
        glBufferData(m_eTarget, static_cast<GLsizeiptr>(m_kData.size()),
                     &m_kData[0], GL_DYNAMIC_DRAW);
        m_bAllocated = true;
    }
    else if (m_uDirtyLength > 0)
    {
        glBufferSubData(m_eTarget, static_cast<GLintptr>(m_uDirtyStart),
                        static_cast<GLsizeiptr>(m_uDirtyLength),
                        &m_kData[m_uDirtyStart]);
    }
    m_uDirtyLength = 0;
    return D3D_OK;
}

// ===========================================================================
// CGlDevice methods (from gl_device.cpp)
// ===========================================================================

HRESULT CGlDevice::CreateVertexBuffer(UINT Length, DWORD /*Usage*/, DWORD FVF,
                                      D3DPOOL /*Pool*/,
                                      IDirect3DVertexBuffer8** ppVertexBuffer)
{
    if (!ppVertexBuffer)
        return E_FAIL;
    *ppVertexBuffer = new CGlVertexBuffer(Length, FVF);
    return D3D_OK;
}

HRESULT CGlDevice::CreateIndexBuffer(UINT Length, DWORD /*Usage*/, D3DFORMAT Format,
                                     D3DPOOL /*Pool*/,
                                     IDirect3DIndexBuffer8** ppIndexBuffer)
{
    if (!ppIndexBuffer)
        return E_FAIL;
    *ppIndexBuffer = new CGlIndexBuffer(Length, Format);
    return D3D_OK;
}

HRESULT CGlDevice::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8* pStreamData,
                                   UINT Stride)
{
    if (StreamNumber != 0)
    {
        // The second stream is used by shaders, which we do not compose
        // anyway.
        return D3D_OK;
    }

    // THE DEVICE HOLDS A REFERENCE - and so did Direct3D.
    //
    // A FIX FROM A MEASUREMENT IN THE BROWSER: the first run
    // printed twenty times
    //     "vertexAttribPointer: no ARRAY_BUFFER is bound"
    // because the test released the buffer and then went on drawing.
    // Without our own reference `m_pStream` pointed to FREED MEMORY, and the
    // buffer's name in OpenGL was already deleted.
    //
    // In Direct3D this was safe, because `SetStreamSource` increased the
    // reference count. We do the same - and it becomes impossible for the
    // client to release something the device is standing on.
    if (pStreamData)
        pStreamData->AddRef();
    if (m_pStream)
        m_pStream->Release();

    m_pStream = static_cast<CGlVertexBuffer*>(pStreamData);
    m_uStride = Stride;
    return D3D_OK;
}

HRESULT CGlDevice::SetIndices(IDirect3DIndexBuffer8* pIndexData,
                              UINT BaseVertexIndex)
{
    // The same as for the vertex stream - see the note there.
    if (pIndexData)
        pIndexData->AddRef();
    if (m_pIndices)
        m_pIndices->Release();

    m_pIndices = static_cast<CGlIndexBuffer*>(pIndexData);
    m_uBaseVertex = BaseVertexIndex;
    return D3D_OK;
}

// -----------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------

HRESULT CGlDevice::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                 UINT PrimitiveCount)
{
    ReportForestDraw("plain", PrimitiveType, PrimitiveCount);
    if (!PrepareDraw())
        return D3D_OK;

    const UINT uVertices =
        M2W_VerticesFromPrimitives(PrimitiveType, PrimitiveCount);
    if (!uVertices)
        return D3D_OK;

    glDrawArrays(M2W_PrimitiveMode(PrimitiveType),
                 static_cast<GLint>(StartVertex),
                 static_cast<GLsizei>(uVertices));
    return D3D_OK;
}

void CGlDevice::ForestLog(const char* c_szFormat, ...)
{
    char sz[1024];
    va_list ap; va_start(ap, c_szFormat); vsnprintf(sz, sizeof(sz), c_szFormat, ap); va_end(ap);
    std::printf("%s", sz);
    if (std::FILE* f = std::fopen("las.txt", "a")) { std::fputs(sz, f); std::fclose(f); }
}

void CGlDevice::ReportForestDraw(const char* c_szWhat, D3DPRIMITIVETYPE eType, UINT uPrimitives)
{
    if (!g_iM2wForestNext)
        return;
    g_iM2wForestNext = 0;
    // Only tree layouts (branches/fronds XYZ|DIFFUSE|TEX2, leaves XYZ|DIFFUSE|TEX1):
    // the flag can be left over after a GetGeometry the wrapper draws nothing after.
    if (m_dwFVF != 0x242 && m_dwFVF != 0x142)
        return;
    // IN THE FRAME? For the first 3 000 000 tree draws (the code limit; the
    // Polish original said 30000): project ALL vertices of
    // this draw and count those inside the view volume. A line only when
    // something is in the frame - that decides between "the camera does not
    // look at the trees" and "a tree is in the frame, yet not visible".
    {
        static int s_iExamined = 0;
        if (M2W_ForestMode() >= 1 && s_iExamined < 3000000 && m_pStream && m_pStream->Data() && m_uStride >= 12)
        {
            ++s_iExamined;
            D3DXMATRIX kM; BuildFinalMatrix(&kM);
            const unsigned uN = m_pStream->Size() / m_uStride;
            unsigned uInFrame = 0; float fMinX = 1e9f, fMaxX = -1e9f, fMinY = 1e9f, fMaxY = -1e9f, fMinZ = 1e9f, fMaxZ = -1e9f;
            for (unsigned i = 0; i < uN; ++i)
            {
                float af[3]; std::memcpy(af, m_pStream->Data() + i * m_uStride, 12);
                D3DXVECTOR4 v; D3DXVec3Transform(&v, reinterpret_cast<const D3DXVECTOR3*>(af), &kM);
                if (v.w <= 0.0f) continue;
                const float x = v.x / v.w, y = v.y / v.w, z = v.z / v.w;
                if (x < fMinX) fMinX = x; if (x > fMaxX) fMaxX = x; if (y < fMinY) fMinY = y; if (y > fMaxY) fMaxY = y; if (z < fMinZ) fMinZ = z; if (z > fMaxZ) fMaxZ = z;
                if (x > -1 && x < 1 && y > -1 && y < 1 && z > -1 && z < 1) ++uInFrame;
            }
            static int s_iInFrameReports = 0;
            if (uInFrame > 0 && ++s_iInFrameReports <= 30)
                ForestLog("m2w forest IN FRAME %d: %s primitives %u, world (%.0f %.0f %.0f), vertices %u in frame %u, ndc x [%.2f %.2f] y [%.2f %.2f] z [%.3f %.3f], textures %u %u, alpha %lu/%lu, z %lu\n",
                          s_iInFrameReports, c_szWhat, (unsigned)uPrimitives, m_matWorld._41, m_matWorld._42, m_matWorld._43, uN, uInFrame,
                          fMinX, fMaxX, fMinY, fMaxY, fMinZ, fMaxZ,
                          m_apTextures[0] ? (unsigned)m_apTextures[0]->GlName() : 0u, m_apTextures[1] ? (unsigned)m_apTextures[1]->GlName() : 0u,
                          (unsigned long)m_adwRenderState[D3DRS_ALPHATESTENABLE], (unsigned long)m_adwRenderState[D3DRS_ALPHAREF], (unsigned long)m_adwRenderState[D3DRS_ZENABLE]);
        }
    }
    // EXPERIMENT `?forest=2`: draw the tree without the alpha test, fog and
    // face culling - which of the three puts the image out.
    {
        if (M2W_ForestMode() == 2 || M2W_ForestMode() == 3)
        {
            m_adwRenderState[D3DRS_ALPHATESTENABLE] = 0;
            m_adwRenderState[D3DRS_FOGENABLE] = 0;
            m_adwRenderState[D3DRS_CULLMODE] = D3DCULL_NONE;
        }
        // `?forest=5`: EVERY tree drawn 25 m IN FRONT OF THE CAMERA (the world
        // translation replaced). If the drawing path works, the trees must
        // be in the middle of the screen - wherever they stand on the map.
        if (M2W_ForestMode() == 5)
        {
            m_matWorld._41 = g_afM2wForestEye[0] + g_afM2wForestDir[0] * 2500.0f;
            m_matWorld._42 = g_afM2wForestEye[1] + g_afM2wForestDir[1] * 2500.0f;
            m_matWorld._43 = g_afM2wForestEye[2] - 900.0f;
        }
        // `?forest=3`: on top of that no depth test - does the tree hit the
        // screen at all.
        if (M2W_ForestMode() == 3)
        {
            m_adwRenderState[D3DRS_ZENABLE] = 0;
            m_adwRenderState[D3DRS_ALPHABLENDENABLE] = 0;
            m_aadwStageState[0][D3DTSS_COLOROP] = D3DTOP_SELECTARG2;   // the vertex colour alone
            m_aadwStageState[1][D3DTSS_COLOROP] = D3DTOP_DISABLE;
        }
    }
    static int s_iPrinted = 0;
    static int s_iLater = 0;
    if (s_iPrinted >= 8 && (++s_iLater % 1500) != 0)
        return;
    ++s_iPrinted;
    const TVertexLayout kLayout = M2W_VertexLayout(m_dwFVF);
    ForestLog("m2w forest %d: %s type %d primitives %u FVF %08lx shader %08lx stride %u"
              " | position %d colour %d sets %d transformed %d"
              " | world (%.0f %.0f %.0f) | textures %u %u | alpha test %lu func %lu ref %lu"
              " | cull %lu z %lu z write %lu | fog %lu | lighting %lu\n",
              s_iPrinted, c_szWhat, (int)eType, (unsigned)uPrimitives,
              (unsigned long)m_dwFVF, (unsigned long)m_dwVertexShader, (unsigned)m_uStride,
              kLayout.iPosition, kLayout.iDiffuse, kLayout.iTexCoordSets, kLayout.bTransformed ? 1 : 0,
              m_matWorld._41, m_matWorld._42, m_matWorld._43,
              m_apTextures[0] ? (unsigned)m_apTextures[0]->GlName() : 0u,
              m_apTextures[1] ? (unsigned)m_apTextures[1]->GlName() : 0u,
              (unsigned long)m_adwRenderState[D3DRS_ALPHATESTENABLE],
              (unsigned long)m_adwRenderState[D3DRS_ALPHAFUNC],
              (unsigned long)m_adwRenderState[D3DRS_ALPHAREF],
              (unsigned long)m_adwRenderState[D3DRS_CULLMODE],
              (unsigned long)m_adwRenderState[D3DRS_ZENABLE],
              (unsigned long)m_adwRenderState[D3DRS_ZWRITEENABLE],
              (unsigned long)m_adwRenderState[D3DRS_FOGENABLE],
              (unsigned long)m_adwRenderState[D3DRS_LIGHTING]);
    if (m_pStream && m_pStream->Data() && m_pStream->Size() >= 24)
    {
        float af[3]; DWORD dw; float afUV[2];
        std::memcpy(af, m_pStream->Data(), 12);
        std::memcpy(&dw, m_pStream->Data() + 12, 4);
        std::memcpy(afUV, m_pStream->Data() + 16, 8);
        ForestLog("m2w forest %d: first vertex (%.1f %.1f %.1f) colour %08lx uv (%.3f %.3f), buffer %u B\n",
                  s_iPrinted, af[0], af[1], af[2], (unsigned long)dw, afUV[0], afUV[1], (unsigned)m_pStream->Size());
    }
    // Project the first three vertices with the same matrix that goes to the
    // shader - do they hit the view volume at all.
    if (m_pStream && m_pStream->Data() && m_pStream->Size() >= 3 * m_uStride && m_uStride >= 12)
    {
        D3DXMATRIX kM; BuildFinalMatrix(&kM);
        for (int i = 0; i < 3; ++i)
        {
            float af[3]; std::memcpy(af, m_pStream->Data() + i * m_uStride, 12);
            D3DXVECTOR4 v;
            D3DXVec3Transform(&v, reinterpret_cast<const D3DXVECTOR3*>(af), &kM);
            ForestLog("m2w forest %d: vertex %d (%.1f %.1f %.1f) -> clip (%.2f %.2f %.2f w %.2f) ndc (%.3f %.3f %.3f)\n",
                      s_iPrinted, i, af[0], af[1], af[2], v.x, v.y, v.z, v.w,
                      v.w != 0.0f ? v.x / v.w : 0.0f, v.w != 0.0f ? v.y / v.w : 0.0f, v.w != 0.0f ? v.z / v.w : 0.0f);
        }
    }
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR)
        ForestLog("m2w forest %d: GL error %x before the draw\n", s_iPrinted, (unsigned)e);
}

void CGlDevice::ReportTerrain()
{
    if (!m_pStream)
        return;

    // GUESSING THE FORMAT DID NOT WORK - SO I MEASURE IT.
    //
    // The first version of this report waited for `D3DFVF_XYZRHW|DIFFUSE|
    // SPECULAR|TEX2`, because that is what `MapOutdoorRenderSTP.cpp:46`
    // sets. It never spoke up, although the terrain is drawn on the screen
    // and the session ran on the right binary. The instrument's silence is
    // a result in itself then: the format at DRAW time is not the one I saw
    // in the game's code.
    //
    // Instead of fixing the guess, it prints the FIRST EIGHT DIFFERENT
    // layouts that go through indexed drawing - together with the stride,
    // the shader handle and whether the vertices are already transformed.
    // The terrain is among them and it will be visible which one it is.
    static std::set<DWORD> s_kSeen;
    static int s_iPrinted = 0;

    const DWORD dwKey = m_dwFVF ^ (m_dwVertexShader << 8) ^
                        (m_uStride << 20);
    if (s_iPrinted >= 8 || s_kSeen.count(dwKey))
        return;
    s_kSeen.insert(dwKey);
    ++s_iPrinted;

    const TVertexLayout kLayout = M2W_VertexLayout(m_dwFVF);
    std::printf("m2w layout %d: FVF %08lx shader %08lx stride %u"
                " | position %d normal %d colour %d sets %d"
                " | transformed %d | textures %u %u\n",
                s_iPrinted, (unsigned long)m_dwFVF,
                (unsigned long)m_dwVertexShader, (unsigned)m_uStride,
                kLayout.iPosition, kLayout.iNormal, kLayout.iDiffuse, kLayout.iTexCoordSets,
                kLayout.bTransformed ? 1 : 0,
                m_apTextures[0] ? (unsigned)m_apTextures[0]->GlName() : 0u,
                m_apTextures[1] ? (unsigned)m_apTextures[1]->GlName() : 0u);

    const unsigned char* c_pby = m_pStream->Data();
    if (!c_pby || m_pStream->Size() < 40 || m_uStride < 40)
        return;

    float af[4];
    DWORD adw[2];
    float afUV[4];
    std::memcpy(af, c_pby, sizeof(af));
    std::memcpy(adw, c_pby + 16, sizeof(adw));
    std::memcpy(afUV, c_pby + 24, sizeof(afUV));

    std::printf("m2w terrain: first vertex (%.1f %.1f %.1f rhw %.3f)"
                " colour %08lx specular %08lx uv0 (%.3f %.3f) uv1 (%.3f %.3f)\n",
                af[0], af[1], af[2], af[3],
                (unsigned long)adw[0], (unsigned long)adw[1],
                afUV[0], afUV[1], afUV[2], afUV[3]);
    std::printf("m2w terrain: textures %u and %u, stage 0 colour operation = %lu,"
                " arguments %lu and %lu\n",
                m_apTextures[0] ? (unsigned)m_apTextures[0]->GlName() : 0u,
                m_apTextures[1] ? (unsigned)m_apTextures[1]->GlName() : 0u,
                (unsigned long)m_aadwStageState[0][D3DTSS_COLOROP],
                (unsigned long)m_aadwStageState[0][D3DTSS_COLORARG1],
                (unsigned long)m_aadwStageState[0][D3DTSS_COLORARG2]);
}

HRESULT CGlDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT,
                                        UINT, UINT StartIndex,
                                        UINT PrimitiveCount)
{
    ReportTerrain();
    const bool bForest = g_iM2wForestNext != 0;
    bool bForestQuery = bForest;
    ReportForestDraw("indexed", PrimitiveType, PrimitiveCount);

    if (!PrepareDraw() || !m_pIndices)
    {
        if (bForest) ForestLog("m2w forest: indexed draw SKIPPED (prepare %d, indices %d)\n", PrepareDraw() ? 1 : 0, m_pIndices ? 1 : 0);
        return D3D_OK;
    }

    const UINT uIndices =
        M2W_VerticesFromPrimitives(PrimitiveType, PrimitiveCount);
    if (!uIndices)
        return D3D_OK;

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_pIndices->GlName());

    // Direct3D 8's `BaseVertexIndex` is added to EVERY index. OpenGL ES 3
    // has no equivalent (`glDrawElementsBaseVertex` comes only in ES 3.2),
    // so an offset of the attribute pointers handles it - see
    // `ApplyVertexLayout`.
    const void* pOffset = reinterpret_cast<const void*>(
        static_cast<uintptr_t>(StartIndex) * m_pIndices->BytesPerIndex());

    // THE SAMPLES QUERY: did this tree draw produce even one
    // fragment that passed the tests. The previous query's result is read
    // at the next one - without waiting for the GPU.
    static GLuint s_uQuery = 0; static bool s_bQueryRunning = false; static unsigned s_uQueries = 0, s_uWithSamples = 0;
    if (bForest && M2W_ForestMode() >= 1)
    {
        if (!s_uQuery) glGenQueries(1, &s_uQuery);
        if (s_bQueryRunning)
        {
            GLuint uReady = 0; glGetQueryObjectuiv(s_uQuery, GL_QUERY_RESULT_AVAILABLE, &uReady);
            if (uReady)
            {
                GLuint uResult = 0; glGetQueryObjectuiv(s_uQuery, GL_QUERY_RESULT, &uResult);
                ++s_uQueries; if (uResult) ++s_uWithSamples;
                if ((s_uQueries % 500) == 1) ForestLog("m2w forest samples: queries %u, with fragments %u\n", s_uQueries, s_uWithSamples);
                s_bQueryRunning = false;
            }
        }
        if (!s_bQueryRunning) { glBeginQuery(GL_ANY_SAMPLES_PASSED, s_uQuery); s_bQueryRunning = true; }
        else bForestQuery = false;
    }
    glDrawElements(M2W_PrimitiveMode(PrimitiveType),
                   static_cast<GLsizei>(uIndices),
                   m_pIndices->GlType(), pOffset);
    if (bForestQuery && s_bQueryRunning) glEndQuery(GL_ANY_SAMPLES_PASSED);
    if (bForest)
    {
        static int s_i = 0;
        if (++s_i <= 4)
        {
            GLint iProgram = 0, iOk = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &iProgram);
            if (iProgram) glGetProgramiv(iProgram, GL_LINK_STATUS, &iOk);
            GLint iVBO = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &iVBO);
            GLint iIBO = 0; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &iIBO);
            GLint iVAO = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &iVAO);
            GLint aiVp[4] = { 0, 0, 0, 0 }; glGetIntegerv(GL_VIEWPORT, aiVp);
            GLboolean bDepth = glIsEnabled(GL_DEPTH_TEST), bBlend = glIsEnabled(GL_BLEND), bCull = glIsEnabled(GL_CULL_FACE), bScis = glIsEnabled(GL_SCISSOR_TEST);
            GLint iFbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &iFbo);
            ForestLog("m2w forest after draw %d: GL error %x, program %d (link %d), VBO %d IBO %d VAO %d, viewport %d %d %d %d, depth %d blend %d cull %d scissor %d, FBO %d, indices %u\n",
                      s_i, (unsigned)glGetError(), iProgram, iOk, iVBO, iIBO, iVAO, aiVp[0], aiVp[1], aiVp[2], aiVp[3],
                      bDepth, bBlend, bCull, bScis, iFbo, (unsigned)uIndices);
        }
    }
    return D3D_OK;
}

HRESULT CGlDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                   CONST void* pVertexStreamZeroData,
                                   UINT VertexStreamZeroStride)
{
    const UINT uVertices =
        M2W_VerticesFromPrimitives(PrimitiveType, PrimitiveCount);
    if (!uVertices || !pVertexStreamZeroData || !VertexStreamZeroStride)
        return D3D_OK;

    // Drawing from client memory. OpenGL cannot do that - the data has to go
    // through a buffer. There is one scratch buffer and it is overwritten on
    // every such draw; that is fine, because `glDrawArrays` finishes reading
    // before we return.
    glBindBuffer(GL_ARRAY_BUFFER, m_uScratchBuffer);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(uVertices * VertexStreamZeroStride),
                 pVertexStreamZeroData, GL_STREAM_DRAW);

    if (!PrepareDrawFromBuffer(m_uScratchBuffer, VertexStreamZeroStride))
        return D3D_OK;

    glDrawArrays(M2W_PrimitiveMode(PrimitiveType), 0,
                 static_cast<GLsizei>(uVertices));
    return D3D_OK;
}

HRESULT CGlDevice::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT,
                                          UINT NumVertices, UINT PrimitiveCount,
                                          CONST void* pIndexData,
                                          D3DFORMAT IndexDataFormat,
                                          CONST void* pVertexStreamZeroData,
                                          UINT VertexStreamZeroStride)
{
    const UINT uIndices =
        M2W_VerticesFromPrimitives(PrimitiveType, PrimitiveCount);
    if (!uIndices || !pIndexData || !pVertexStreamZeroData ||
        !VertexStreamZeroStride)
        return D3D_OK;

    glBindBuffer(GL_ARRAY_BUFFER, m_uScratchBuffer);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(NumVertices * VertexStreamZeroStride),
                 pVertexStreamZeroData, GL_STREAM_DRAW);

    const UINT uBytesPerIndex = (IndexDataFormat == D3DFMT_INDEX32) ? 4u : 2u;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_uScratchIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(uIndices * uBytesPerIndex),
                 pIndexData, GL_STREAM_DRAW);

    if (!PrepareDrawFromBuffer(m_uScratchBuffer, VertexStreamZeroStride))
        return D3D_OK;

    glDrawElements(M2W_PrimitiveMode(PrimitiveType),
                   static_cast<GLsizei>(uIndices),
                   (uBytesPerIndex == 4) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                   NULL);
    return D3D_OK;
}

// -----------------------------------------------------------------------
// ROAD B - drawing that bypasses the fixed-function pipeline
// -----------------------------------------------------------------------

int CGlDevice::AlphaTestFunc() const
{
    if (!m_adwRenderState[D3DRS_ALPHATESTENABLE])
        return 0;
    const DWORD dwFunc = m_adwRenderState[D3DRS_ALPHAFUNC];
    return (dwFunc >= 1 && dwFunc <= 8) ? (int)dwFunc : 0;
}

float CGlDevice::AlphaTestRef() const
{
    return (float)(m_adwRenderState[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
}

bool CGlDevice::DrawCustom(UINT uFirstIndex, UINT uTriangles)
{
    // A REPORT, so that "the character is not visible" can be turned into
    // a number. The first call and then every hundred thousandth - see
    // `TDrawCensus`.
    static unsigned long s_ulOurs = 0;
    static unsigned long s_ulNoState = 0;
    static unsigned long s_ulBadStride = 0;
    struct TReport
    {
        /// Prints the custom-road counters on the first mesh and every 100 000th.
        static void Say()
        {
            const unsigned long ulTotal =
                s_ulOurs + s_ulNoState + s_ulBadStride;
            if (ulTotal != 1 && ulTotal % 100000 != 0)
                return;
            std::printf("m2w custom road: %lu meshes - our road %lu, "
                        "no state %lu, bad stride %lu\n",
                        ulTotal, s_ulOurs, s_ulNoState, s_ulBadStride);
        }
    };

    if (!m_pStream || !m_pIndices || !uTriangles)
    {
        ++s_ulNoState; TReport::Say();
        return false;
    }

    // EXPERIMENT: `?nocustom=1` - characters go through the
    // fixed-function pipeline instead of road B. Decides whether "the
    // incomplete character in armour" is road B or the data.
    {
        static int s_iNoCustom = -1;
        if (s_iNoCustom < 0)
            s_iNoCustom = m2w_no_custom();
        if (s_iNoCustom)
            return false;
    }

    // Our own program reads the `PNT332` layout - position, normal, one pair
    // of coordinates, 32 bytes in all. When the stride is different, other
    // data lies in the buffer and drawing it with this program would give
    // garbage, not an image. Better to leave it to the pipeline.
    if (m_uStride != 32)
    {
        ++s_ulBadStride; TReport::Say();
        return false;
    }

    ++s_ulOurs; TReport::Say();

    const GLuint uTexture =
        m_apTextures[0] ? m_apTextures[0]->GlName() : 0;

    // Direct3D `D3DCULL_CW` culls faces that are CLOCKWISE.
    const DWORD dwCull = m_adwRenderState[D3DRS_CULLMODE];
    int iCull = (dwCull == D3DCULL_CW)  ? M2W_CULL_CW
              : (dwCull == D3DCULL_CCW) ? M2W_CULL_CCW
                                        : M2W_CULL_NONE;
    // EXPERIMENT: `?nocull=1` switches culling off on our own
    // road - a test whether "the incomplete character in armour" is a
    // reversed vertex order (bones with a negative scale).
    {
        static int s_iNoCull = -1;
        if (s_iNoCull < 0)
            s_iNoCull = m2w_no_cull();
        if (s_iNoCull)
            iCull = M2W_CULL_NONE;
    }

    // FOG: road B gets Direct3D's fog state. A target texture
    // (the shadow map) without fog - as in the original
    // (`BeginRenderCharacterShadow` draws with TFACTOR without fog), hence
    // mode 0 when it is a target.
    {
        const bool bFog = m_adwRenderState[D3DRS_FOGENABLE] != 0 && !m_pTargetTexture;
        float afColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        M2W_ColorToFloat(m_adwRenderState[D3DRS_FOGCOLOR], afColor);
        M2W_SetCustomFog(bFog ? (int)m_adwRenderState[D3DRS_FOGVERTEXMODE] : 0,
                         BitsToFloat(m_adwRenderState[D3DRS_FOGSTART]),
                         BitsToFloat(m_adwRenderState[D3DRS_FOGEND]),
                         BitsToFloat(m_adwRenderState[D3DRS_FOGDENSITY]),
                         afColor);
    }
    // The Y flip on a target texture (see `TargetFlipped`): in Direct3D's
    // row-major projection (`v * M`) the result's `y` is the second column.
    D3DXMATRIX matTargetProjection = m_matProjection;
    if (TargetFlipped())
    {
        matTargetProjection._12 = -matTargetProjection._12; matTargetProjection._22 = -matTargetProjection._22;
        matTargetProjection._32 = -matTargetProjection._32; matTargetProjection._42 = -matTargetProjection._42;
        if (iCull == M2W_CULL_CW) iCull = M2W_CULL_CCW; else if (iCull == M2W_CULL_CCW) iCull = M2W_CULL_CW;
    }
    M2W_DrawMeshFromBuffers(
        m_pStream->GlName(), m_uStride, m_uBaseVertex,
        m_pIndices->GlName(), m_pIndices->GlType(),
        uFirstIndex, uTriangles * 3, uTexture,
        reinterpret_cast<const float*>(&m_matWorld),
        reinterpret_cast<const float*>(&m_matView),
        reinterpret_cast<const float*>(&matTargetProjection),
        m_adwRenderState[D3DRS_ALPHABLENDENABLE] ? 1 : 0,
        iCull, AlphaTestFunc(), AlphaTestRef());
    return true;
}

void CGlDevice::ApplyVertexLayout(const TProgram& c_rProgram,
                                  const TVertexLayout& c_rLayout,
                                  UINT uStride)
{
    const GLsizei iStride = static_cast<GLsizei>(
        uStride ? uStride : c_rLayout.uStride);

    // `BaseVertexIndex` is added to every index - OpenGL ES 3 has no method
    // for it, so we move the start of the attributes.
    const uintptr_t uBase =
        static_cast<uintptr_t>(m_uBaseVertex) *
        static_cast<uintptr_t>(iStride);

    // Instead of switching all eight off and the needed ones on, we collect a
    // MASK and touch only the difference. Measured: 397 switch-offs per frame
    // at 50 draws - eight per draw, while usually none changes.
    unsigned uWanted = 0;

    if (c_rProgram.iPosition >= 0 && c_rLayout.iPosition >= 0)
    {
        uWanted |= 1u << c_rProgram.iPosition;
        glVertexAttribPointer(
            static_cast<GLuint>(c_rProgram.iPosition),
            c_rLayout.iPositionComponents, GL_FLOAT, GL_FALSE, iStride,
            reinterpret_cast<const void*>(uBase + c_rLayout.iPosition));
    }

    if (c_rProgram.iNormal >= 0 && c_rLayout.iNormal >= 0)
    {
        uWanted |= 1u << c_rProgram.iNormal;
        glVertexAttribPointer(
            static_cast<GLuint>(c_rProgram.iNormal), 3, GL_FLOAT, GL_FALSE,
            iStride,
            reinterpret_cast<const void*>(uBase + c_rLayout.iNormal));
    }

    // Colours go as FOUR NORMALISED BYTES, in the order B, G, R, A. The
    // shader straightens it with `.bgra` - see `d3d8_fvf.h`.
    if (c_rProgram.iColor >= 0 && c_rLayout.iDiffuse >= 0)
    {
        uWanted |= 1u << c_rProgram.iColor;
        glVertexAttribPointer(
            static_cast<GLuint>(c_rProgram.iColor), 4, GL_UNSIGNED_BYTE,
            GL_TRUE, iStride,
            reinterpret_cast<const void*>(uBase + c_rLayout.iDiffuse));
    }

    if (c_rProgram.iSpecular >= 0 && c_rLayout.iSpecular >= 0)
    {
        uWanted |= 1u << c_rProgram.iSpecular;
        glVertexAttribPointer(
            static_cast<GLuint>(c_rProgram.iSpecular), 4, GL_UNSIGNED_BYTE,
            GL_TRUE, iStride,
            reinterpret_cast<const void*>(uBase + c_rLayout.iSpecular));
    }

    for (int i = 0; i < M2W_TEXTURE_STAGES; ++i)
    {
        if (c_rProgram.aiTexCoords[i] < 0 || i >= c_rLayout.iTexCoordSets)
            continue;
        uWanted |= 1u << c_rProgram.aiTexCoords[i];
        glVertexAttribPointer(
            static_cast<GLuint>(c_rProgram.aiTexCoords[i]),
            c_rLayout.aiTexCoordComponents[i], GL_FLOAT, GL_FALSE, iStride,
            reinterpret_cast<const void*>(uBase + c_rLayout.aiTexCoords[i]));
    }

    // Only now do we touch the switches - and only those that change.
    const unsigned uHave = m_kGlCache.uEnabledAttribs;
    for (GLuint i = 0; i < 8; ++i)
    {
        const unsigned uBit = 1u << i;
        if (m_kGlCache.iAttribsKnown && (uWanted & uBit) == (uHave & uBit))
            continue;
        if (uWanted & uBit)
            glEnableVertexAttribArray(i);
        else
            glDisableVertexAttribArray(i);
    }
    m_kGlCache.iAttribsKnown = 1;
    m_kGlCache.uEnabledAttribs = uWanted;
}

bool CGlDevice::PrepareDraw()
{
    if (!m_pStream)
    {
        ++g_kCensus.lSkippedNoStream; g_kCensus.Count();
        return false;
    }
    return PrepareDrawFromBuffer(m_pStream->GlName(), m_uStride);
}

int CGlDevice::ShadowExperimentMode()
{
    static int s_iMode = -2;
    if (s_iMode < -1)
        s_iMode = m2w_shadow_mode();
    return s_iMode;
}

bool CGlDevice::IsShadowPass() const
{
    return m_adwRenderState[D3DRS_SRCBLEND] == D3DBLEND_ZERO &&
           m_adwRenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCCOLOR;
}

bool CGlDevice::SkipShadowPass() const
{
    return ShadowExperimentMode() == 0 && IsShadowPass();
}

bool CGlDevice::SelectVertexLayout(TVertexLayout& rLayout)
{
    // TWO TREES PER SHADER HANDLE - once per CHANGE of handle, not per draw
    // The handle changes in `SetVertexShader`, and there are
    // hundreds of draws between one change and the next.
    if (!m_bLastShaderKnown || m_dwLastShader != m_dwVertexShader)
    {
        m_dwLastShader = m_dwVertexShader;
        m_bLastShaderHasCode =
            m_kShadersWithCode.count(m_dwVertexShader) != 0;

        const std::map<DWORD, TVertexLayout>::const_iterator itD =
            m_kDeclarationLayouts.find(m_dwVertexShader);
        m_bLastShaderHasDeclaration = (itD != m_kDeclarationLayouts.end());
        if (m_bLastShaderHasDeclaration)
            m_kLastDeclarationLayout = itD->second;

        m_bLastShaderKnown = true;
    }

    // A draw with a real shader - skipped with one warning. See the note at
    // the top of gl_device.cpp ("WHAT IS NOT HERE YET").
    if (m_bLastShaderHasCode ||
        (m_dwPixelShader & SHADER_HANDLE_BIT))
    {
        if (!m_bWarnedShaders)
        {
            std::printf("m2w: draw with a Direct3D assembly shader - "
                        "skipped (see the note in gl_device.cpp)\n");
            m_bWarnedShaders = true;
        }
        ++g_kCensus.lSkippedShader; g_kCensus.Count();
        return false;
    }

    // The vertex layout can come from TWO sources: from the FVF code or from
    // the `D3DVSD_*` declaration given when the handle was created. The
    // declaration takes precedence, because if the client set exactly this
    // handle, it describes the data that lies in the buffer now.
    if (m_bLastShaderHasDeclaration)
    {
        rLayout = m_kLastDeclarationLayout;

        // An incomplete declaration - most often because of a second stream,
        // which we do not provide. Drawing from it would give an image made
        // of half the data, i.e. something worse than no image: something
        // that looks like a bug elsewhere. We say so once and skip.
        if (!rLayout.bKnown && !m_bWarnedDeclarations)
        {
            std::printf("m2w: a vertex declaration I cannot put together "
                        "in full (a second stream or bone weights) - "
                        "draw skipped\n");
            m_bWarnedDeclarations = true;
        }
        if (!rLayout.bKnown)
        {
            ++g_kCensus.lSkippedDeclaration; g_kCensus.Count();
            return false;
        }
    }
    else
    {
        // DECODING THE FVF CODE - once per CHANGE of the code.
        // `m_dwFVF` changes in `SetVertexShader`, and the decoding has about
        // thirty branches and a loop over the coordinate sets.
        if (!m_bFvfLayoutKnown || m_dwLastFvf != m_dwFVF)
        {
            m_dwLastFvf = m_dwFVF;
            m_kLastFvfLayout = M2W_VertexLayout(m_dwFVF);
            m_bFvfLayoutKnown = true;
        }
        rLayout = m_kLastFvfLayout;
    }
    return true;
}

bool CGlDevice::PrepareDrawFromBuffer(GLuint uBuffer, UINT uStride)
{
    if (SkipShadowPass())
        return false;

    m2wstats::TStopwatch kStopwatch(m2wstats::g_kPipelineSetup);

    // From now on the OpenGL state belongs to us, not to road B.
    M2W_ForgetCustomState();

    // M2W_ForgetGlState was called since our last draw - by road B
    // (custom_draw.cpp), which bound ITS program, or by SetRenderTarget.
    // The memory of which program is bound may be stale. It must
    // be thrown away HERE, before the program check below - earlier
    // it was thrown away in ApplyRenderStates, i.e. AFTER the check had
    // already skipped glUseProgram. The first draw after road B then ran
    // with road B's program, and our uniforms went to it (the WebGL pairs
    // glUniform1i "type does not match" / glUniform1f "size does not
    // match", older than the renaming).
    if (g_bGlStateUnknown)
    {
        m_kGlCache = TGlStateCache();
        g_bGlStateUnknown = false;
    }

    TVertexLayout kLayout;
    if (!SelectVertexLayout(kLayout))
        return false;

    if (kLayout.iPosition < 0)
    {
        ++g_kCensus.lSkippedNoPosition; g_kCensus.Count();
        return false;
    }

    if (kLayout.bTransformed)
        ++g_kCensus.lFlat;
    else
        ++g_kCensus.lSpatial;
    g_kCensus.Count();

    BuildKey(kLayout);

    const TProgram* pProgram = FindOrBuildProgram();
    if (!pProgram)
        return false;

    if (!m_kGlCache.iProgramKnown ||
        m_kGlCache.uProgram != pProgram->uProgram)
    {
        m_kGlCache.iProgramKnown = 1;
        m_kGlCache.uProgram = pProgram->uProgram;
        glUseProgram(pProgram->uProgram);
    }
    glBindBuffer(GL_ARRAY_BUFFER, uBuffer);

    ApplyRenderStates();
    ApplyVertexLayout(*pProgram, kLayout, uStride);
    ApplyTextures(*pProgram);
    ApplyUniforms(*pProgram);
    return true;
}
