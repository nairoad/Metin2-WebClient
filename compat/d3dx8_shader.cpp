// SPDX-License-Identifier: GPL-2.0-or-later
// d3dx8_shader.cpp - `D3DXAssembleShaderFromFileA` and `D3DXAssembleShader`,
// i.e. an HONEST REFUSAL.

// Design:
// WHY THIS IS NOT A DUMMY RETURNING SUCCESS. These functions translated a
// shader in Direct3D 8 ASSEMBLY (`vs.1.1`, `dp3 r0, v3, c8`) into byte code
// for the driver. To really write them one would need an assembler of that
// language - and then a translator of its byte code into GLSL, because
// WebGL knows nothing of Direct3D.
//
// We have neither, and **neither is needed**. The port's drawing layer
// composes GLSL from the FIXED-FUNCTION PIPELINE (`d3d8_fixedfunc.cpp`),
// and the device's `CreateVertexShader` (gl_device.cpp) is a deliberate
// nothing: it hands out a handle and creates no program.
//
// So there are TWO possible answers, and the difference between them is
// the whole content of this file:
//
//   THE WRONG ONE: return `D3D_OK` and an empty buffer. Then
//                  `CVertexShader::CreateFromDiskFile` goes on, calls
//                  `CreateVertexShader` with empty code, gets `D3D_OK`
//                  (our device always returns it) and **reports success**.
//                  The client believes it has a working shader, sets it
//                  and draws - and the screen shows nothing or garbage.
//                  Without a single error.
//
//   THE RIGHT ONE: return an error. `CreateFromDiskFile` returns `false`
//                  and the caller has the chance to take a fallback - and
//                  knows there IS NO shader.
//
// The second is chosen. It is the same kind of decision as for
// `EnumFontFamiliesExA`: a stub that claims "it worked" costs
// days of searching in the wrong place later.
//
// WHO USES IT. Two callers in the tree: `GrpVertexShader.cpp` and
// `GrpPixelShader.cpp` (the file variant) - both `CreateFromDiskFile`
// methods have NO caller in our composition (git grep). The memory
// variant is called by SpeedTree's `VertexShaders.h`, compiled since cz.
// 192d - and there the refusal is exactly what makes those files
// compile and run without a change: see the note above
// `D3DXAssembleShader`.
//
// Measured: the client uses four real shaders
// (`CreateVertexShader` three occurrences, `CreatePixelShader` one). The
// rest of the picture goes through the fixed-function pipeline.

#include <cstdio>
#include <cstring>

#include "d3d8.h"
#include "d3dx8.h"

namespace {

/// The D3DX buffer for the error message. The client wraps it in
/// `CDirect3DXBuffer`, whose destructor calls `Release` - so the reference
/// count must be real, not pretended.
class CMessageBuffer : public ID3DXBuffer
{
public:
    /// A buffer holding a copy of `c_szText` (up to 255 characters), one
    /// reference.
    explicit CMessageBuffer(const char* c_szText)
        : m_uRefs(1)
    {
        std::snprintf(m_szText, sizeof(m_szText), "%s", c_szText);
    }

    /// Adds a reference; returns the new count.
    ULONG AddRef() override { return ++m_uRefs; }

    /// Drops a reference and deletes the buffer at zero; returns the new count.
    ULONG Release() override
    {
        const ULONG u = --m_uRefs;
        if (u == 0)
            delete this;
        return u;
    }

    /// The message text.
    void* GetBufferPointer() override { return m_szText; }

    /// Length of the text including the terminating zero.
    DWORD GetBufferSize() override
    {
        // With the terminating zero - the client prints it as a string.
        return static_cast<DWORD>(std::strlen(m_szText) + 1);
    }

private:
    /// Private: deleted only by `Release`.
    ~CMessageBuffer() {}

    ULONG m_uRefs;
    char  m_szText[256];
};

}  // namespace

/// The file variant: refuses, says so once per file name, and hands the
/// message back in `ppCompilationErrors`.
HRESULT D3DXAssembleShaderFromFileA(LPCSTR filename, DWORD,
                                    LPD3DXBUFFER* ppConstants,
                                    LPD3DXBUFFER* ppCompiledShader,
                                    LPD3DXBUFFER* ppCompilationErrors)
{
    // The output pointers are zeroed FIRST. The client keeps them on the
    // stack UNINITIALISED (see `CVertexShader::CreateFromDiskFile`) and
    // does not touch them on error - but should it ever touch them, it
    // gets zero, not stack garbage.
    if (ppConstants)
        *ppConstants = NULL;
    if (ppCompiledShader)
        *ppCompiledShader = NULL;

    char szMessage[256];
    std::snprintf(szMessage, sizeof(szMessage),
                  "m2w: no Direct3D 8 shader assembler - file '%s' skipped. "
                  "The picture goes through the fixed-function pipeline.",
                  filename ? filename : "(no name)");

    // Said ONCE per file, not on every call: should something try to load
    // a shader in the drawing loop, the console would clog so that nothing
    // could be found in it.
    static const char* s_szLast = NULL;
    if (s_szLast != filename)
    {
        std::printf("%s\n", szMessage);
        s_szLast = filename;
    }

    if (ppCompilationErrors)
        *ppCompilationErrors = new CMessageBuffer(szMessage);

    return E_FAIL;
}

// The same assembler, but from MEMORY. SpeedTree does not load shaders
// from files - it has them pasted into `SpeedTreeLib/VertexShaders.h` as
// strings in Direct3D 8 assembly.
//
// HERE THE REFUSAL HAS AN EFFECT THAT THE ONE ABOVE DOES NOT.
// `VertexShaders.h` checks the result and on error SHOWS A MESSAGE BOX
// (`MessageBoxA` of win32_compat.cpp: a line on stderr), then draws the
// trees without a shader - through the fixed-function pipeline. That is
// TMP4's OWN fallback, not one we invented: the authors provided for cards
// that could not do shaders. We take exactly that road.
//
// That is why the refusal must be a refusal. Were this function to return
// `D3D_OK` with an empty buffer, SpeedTree would create a shader out of
// nothing, set it and draw the trees with NOTHING - instead of taking the
// road it has prepared.
HRESULT D3DXAssembleShader(LPCSTR, UINT, DWORD,
                           LPD3DXBUFFER* ppConstants,
                           LPD3DXBUFFER* ppCompiledShader,
                           LPD3DXBUFFER* ppCompilationErrors)
{
    if (ppConstants)
        *ppConstants = NULL;
    if (ppCompiledShader)
        *ppCompiledShader = NULL;

    static const char c_szMessage[] =
        "m2w: SpeedTree shader in Direct3D 8 assembly - skipped. "
        "The trees take TMP4's own fallback, without a shader.";

    // Once per run: `VertexShaders.h` assembles several shaders in a row
    // and each of them would come here separately.
    static bool s_bSaid = false;
    if (!s_bSaid)
    {
        std::printf("%s\n", c_szMessage);
        s_bSaid = true;
    }

    if (ppCompilationErrors)
        *ppCompilationErrors = new CMessageBuffer(c_szMessage);

    return E_FAIL;
}
