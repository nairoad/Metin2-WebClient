// SPDX-License-Identifier: GPL-2.0-or-later
// custom_draw.cpp - road B: the shader program, the state memory, the draw
// calls, and two probes that run without a server (model probe with the
// canvas-hash gate, texture probe). See custom_draw.h.

// Design: see custom_draw.h for why the road exists; the notes at each
// block say what was measured (GL call counts, the depth fix, the alpha
// mask of armour textures, fog) and why the probe does per-frame work.

#include "custom_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <emscripten.h>
#include <unistd.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include <string>
#include <vector>

#include <granny.h>

#include "gr2_to_granny.h"
#include "gr2_file.h"
#include "frame_stats.h"

// Models sit in the game packs - see `LoadModelFile`.
#include "eterPack/EterPackManager.h"

// The texture probe takes the same road as the game - see `M2W_TextureProbe`.
#include "eterLib/ResourceManager.h"
#include <dirent.h>
#include <stdlib.h>

/// The compatibility layer keeps its own memory of the OpenGL state. We draw
/// OUTSIDE it, so after our work that memory lies and has to be cleared.
extern void M2W_ForgetGlState();

namespace
{

// ---------------------------------------------------------------------------
// The shader program
// ---------------------------------------------------------------------------
// One program, three inputs, one texture. The light is there so that the
// SHAPE can be seen - a flat body in one colour looks like a blot and says
// nothing about whether the mesh is right.

const char* c_szVertexShader =
    "#version 300 es\n"
    "in vec3 aPosition;\n"
    "in vec3 aNormal;\n"
    "in vec2 aTexCoord;\n"
    "uniform mat4 uWorld;\n"
    "uniform mat4 uViewProjection;\n"
    "uniform mat4 uView;\n"
    "out vec3 vNormal;\n"
    "out float vDistance;\n"
    "out vec2 vTexCoord;\n"
    "void main()\n"
    "{\n"
    // MATRIX TIMES VECTOR, although the matrices are ROW-MAJOR (Direct3D).
    //
    // The reason is in how they are uploaded: WITHOUT transposing, and GLSL
    // reads the array by COLUMNS - i.e. transposes them itself. `M * v` in
    // GLSL therefore gives exactly what `v * M` gives in Direct3D. Writing
    // `v * M` here transposes once too often and the model comes out as two
    // huge triangles across the screen - measured.
    "    vec4 worldPos = uWorld * vec4(aPosition, 1.0);\n"
    "    gl_Position = uViewProjection * worldPos;\n"
    "    vNormal = (uWorld * vec4(aNormal, 0.0)).xyz;\n"
    "    vTexCoord = aTexCoord;\n"
    // FOG: the distance from the eye as with D3DRS_RANGEFOGENABLE
    // (the original, MapManager.cpp:271), not the depth alone.
    "    vDistance = length((uView * worldPos).xyz);\n"
    "}\n";

const char* c_szFragmentShader =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 vNormal;\n"
    "in vec2 vTexCoord;\n"
    "in float vDistance;\n"
    // FOG: uFog = (mode, start, end, density); mode as D3DFOG_*:
    // 0 none, 1 EXP, 2 EXP2, 3 LINEAR. Earlier road B had no fog at
    // all - buildings and piers in the distance were fully visible while
    // the terrain (fixed-function pipeline, software fog) was already
    // sinking into the fog.
    "uniform vec4 uFog;\n"
    "uniform vec3 uFogColor;\n"
    "uniform sampler2D uTexture;\n"
    "uniform int uHasTexture;\n"
    // THE ALPHA TEST REFERENCE from the Direct3D state, 0..1; read
    // only when uAlphaFunc != 0 (below). Earlier this was a hard-coded
    // `< 0.05 discard`, and in Metin2 the alpha channel of an ARMOUR
    // texture is a SPECULAR MASK, not transparency: `warrior_nahan.dds` has
    // 54% texels with alpha 0 - and that whole part of the character
    // (thighs, cloth) vanished, the metal boots remained. The
    // fixed-function pipeline drew it right, because D3DRS_ALPHATESTENABLE
    // was off. Measured in the game with the `?nocustom=1` switch.
    "uniform float uAlphaRef;\n"
    // The D3DCMP_* comparison (1..8), 0 = test off. The same table as
    // `OdrzucenieAlfy` in d3d8_fixedfunc.cpp - one concept, one formula.
    "uniform int uAlphaFunc;\n"
    "out vec4 outColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 n = normalize(vNormal);\n"
    "    vec3 toLight = normalize(vec3(0.4, -0.7, 0.6));\n"
    "    float brightness = 0.35 + 0.65 * max(dot(n, toLight), 0.0);\n"
    "    vec4 texel = (uHasTexture != 0)\n"
    "        ? texture(uTexture, vTexCoord)\n"
    "        : vec4(0.8, 0.75, 0.7, 1.0);\n"
    "    if (uAlphaFunc != 0) {\n"
    "        bool p = (uAlphaFunc == 2) ? (texel.a <  uAlphaRef)\n"
    "               : (uAlphaFunc == 3) ? (texel.a == uAlphaRef)\n"
    "               : (uAlphaFunc == 4) ? (texel.a <= uAlphaRef)\n"
    "               : (uAlphaFunc == 5) ? (texel.a >  uAlphaRef)\n"
    "               : (uAlphaFunc == 6) ? (texel.a != uAlphaRef)\n"
    "               : (uAlphaFunc == 7) ? (texel.a >= uAlphaRef)\n"
    "               : (uAlphaFunc == 8);\n"
    "        if (!p) discard;\n"
    "    }\n"
    "    vec3 color = texel.rgb * brightness;\n"
    "    if (uFog.x > 0.5) {\n"
    "        float f = (uFog.x < 1.5) ? exp(-uFog.w * vDistance)\n"
    "                : (uFog.x < 2.5) ? exp(-pow(uFog.w * vDistance, 2.0))\n"
    "                : (uFog.z - vDistance) / max(uFog.z - uFog.y, 0.0001);\n"
    "        color = mix(uFogColor, color, clamp(f, 0.0, 1.0));\n"
    "    }\n"
    "    outColor = vec4(color, texel.a);\n"
    "}\n";

GLuint s_uProgram = 0;
GLint s_iWorld = -1;
GLint s_iViewProjection = -1;
GLint s_iTexture = -1;
GLint s_iHasTexture = -1;
GLint s_iAlphaRef = -1;
GLint s_iAlphaFunc = -1;
GLint s_iView = -1;
GLint s_iFog = -1;
GLint s_iFogColor = -1;
GLint s_iPosition = -1;
GLint s_iNormal = -1;
GLint s_iTexCoord = -1;
GLuint s_uVertexBuffer = 0;
GLuint s_uIndexBuffer = 0;
GLuint s_uVertexArray = 0;
bool s_bBuilt = false;
bool s_bFailed = false;

// ---------------------------------------------------------------------------
// State memory of the custom road
// ---------------------------------------------------------------------------
// The same lesson as, only on our side. Every draw uploaded the
// same program, the same view matrices, the same depth and blend switches
// - and the same attribute pointers. Measured with twenty characters: 1500
// GL CALLS PER FRAME for sixty meshes, twenty-five per mesh.
//
// That is exactly the number that gave 0.9 frames per second.
// There the work was on the card's side and no stopwatch in the code saw
// it; the only thing that showed it was the call counter.
//
// So only what CHANGED is uploaded. The initial values mean "unknown" and
// are ones no real draw passes, so the first draw after a reset sets
// everything: `0xFFFFFFFF` for GL names, `-1` for switches and the stride,
// `-2.0f` for the alpha reference (`-1.0f` is a REAL value - the
// M2W_ALPHA_REF_UNUSED that M2W_DrawMesh passes), `-1` fog mode.
struct TCustomState
{
    GLuint uProgram = 0;
    GLuint uTexture = 0xFFFFFFFFu;
    GLuint uVertexBuffer = 0xFFFFFFFFu;
    GLuint uIndexBuffer = 0xFFFFFFFFu;
    uintptr_t uBase = (uintptr_t)-1;
    GLsizei iStride = -1;
    int iBlend = -1;
    int iCull = -1;
    int iHasTexture = -1;
    float fAlphaRef = -2.0f;
    int iAlphaFunc = -1;
    float afFog[4] = { -1.0f, 0.0f, 0.0f, 0.0f };
    float afFogColor[3] = { -1.0f, 0.0f, 0.0f };
    float afViewProjection[16] = { 0.0f };
    bool bViewProjectionKnown = false;
    bool bDepthSet = false;
};
TCustomState s_kState;
float s_afFogRequested[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
float s_afFogColorRequested[3] = { 0.0f, 0.0f, 0.0f };

}  // namespace - briefly, for the public functions

// Clears our state memory. The compatibility layer calls this before it
// draws anything its own way - because then the OpenGL state is no longer
// ours. The same contract as `M2W_ForgetGlState`, the other way round: two
// layers draw in one context, so each has to be able to tell the other
// "from now on you do not know what is set".
void M2W_ForgetCustomState()
{
    s_kState = TCustomState();
}

// Fog for road B - the Direct3D state copied by the layer before
// drawing: D3DFOG_* mode (0 = off), start/end (LINEAR), density
// (EXP/EXP2), colour.
void M2W_SetCustomFog(int iMode, float fStart, float fEnd, float fDensity,
                      const float* c_afColor)
{
    s_afFogRequested[0] = (float)iMode; s_afFogRequested[1] = fStart;
    s_afFogRequested[2] = fEnd; s_afFogRequested[3] = fDensity;
    s_afFogColorRequested[0] = c_afColor[0]; s_afFogColorRequested[1] = c_afColor[1];
    s_afFogColorRequested[2] = c_afColor[2];
}

namespace
{

/// Compiles one GLSL shader; on failure prints the info log, deletes it and
/// returns 0.
GLuint Compile(GLenum eKind, const char* c_szSource)
{
    const GLuint u = glCreateShader(eKind);
    glShaderSource(u, 1, &c_szSource, NULL);
    glCompileShader(u);
    GLint iOk = 0;
    glGetShaderiv(u, GL_COMPILE_STATUS, &iOk);
    if (!iOk)
    {
        char szLog[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(u, sizeof(szLog), &n, szLog);
        std::printf("m2w custom draw: shader did not compile: %s\n", szLog);
        glDeleteShader(u);
        return 0;
    }
    return u;
}

/// Compiles and links the program once, looks the locations up, and builds
/// our own vertex array.
bool Build()
{
    if (s_bBuilt)
        return true;
    if (s_bFailed)
        return false;

    const GLuint uVertex = Compile(GL_VERTEX_SHADER, c_szVertexShader);
    const GLuint uFragment = Compile(GL_FRAGMENT_SHADER, c_szFragmentShader);
    if (!uVertex || !uFragment)
    {
        s_bFailed = true;
        return false;
    }

    s_uProgram = glCreateProgram();
    glAttachShader(s_uProgram, uVertex);
    glAttachShader(s_uProgram, uFragment);
    glLinkProgram(s_uProgram);
    glDeleteShader(uVertex);
    glDeleteShader(uFragment);

    GLint iOk = 0;
    glGetProgramiv(s_uProgram, GL_LINK_STATUS, &iOk);
    if (!iOk)
    {
        char szLog[1024];
        GLsizei n = 0;
        glGetProgramInfoLog(s_uProgram, sizeof(szLog), &n, szLog);
        std::printf("m2w custom draw: program did not link: %s\n",
                    szLog);
        s_bFailed = true;
        return false;
    }

    s_iWorld = glGetUniformLocation(s_uProgram, "uWorld");
    s_iViewProjection = glGetUniformLocation(s_uProgram, "uViewProjection");
    s_iTexture = glGetUniformLocation(s_uProgram, "uTexture");
    s_iHasTexture = glGetUniformLocation(s_uProgram, "uHasTexture");
    s_iAlphaRef = glGetUniformLocation(s_uProgram, "uAlphaRef");
    s_iAlphaFunc = glGetUniformLocation(s_uProgram, "uAlphaFunc");
    s_iView = glGetUniformLocation(s_uProgram, "uView");
    s_iFog = glGetUniformLocation(s_uProgram, "uFog");
    s_iFogColor = glGetUniformLocation(s_uProgram, "uFogColor");
    s_iPosition = glGetAttribLocation(s_uProgram, "aPosition");
    s_iNormal = glGetAttribLocation(s_uProgram, "aNormal");
    s_iTexCoord = glGetAttribLocation(s_uProgram, "aTexCoord");

    glGenBuffers(1, &s_uVertexBuffer);
    glGenBuffers(1, &s_uIndexBuffer);

    // OUR OWN VERTEX ARRAY.
    //
    // It holds the attribute switches and pointers, so
    // `glEnableVertexAttribArray` goes once for the program's lifetime, not
    // three times per mesh. And our settings do not mix with the
    // compatibility layer's array - each has its own.
    glGenVertexArrays(1, &s_uVertexArray);
    glBindVertexArray(s_uVertexArray);
    if (s_iPosition >= 0)
        glEnableVertexAttribArray((GLuint)s_iPosition);
    if (s_iNormal >= 0)
        glEnableVertexAttribArray((GLuint)s_iNormal);
    if (s_iTexCoord >= 0)
        glEnableVertexAttribArray((GLuint)s_iTexCoord);
    glBindVertexArray(0);

    s_bBuilt = true;
    std::printf("m2w custom draw: program ready\n");
    return true;
}

// ---------------------------------------------------------------------------
// Matrices - the same the rest of the port uses (row-major)
// ---------------------------------------------------------------------------

/// Writes the 4x4 identity into `m`.
void Identity(float m[16])
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/// `w = a * b` for row-major 4x4 matrices; `w` may alias `a` or `b`.
void Multiply(const float a[16], const float b[16], float w[16])
{
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] +
                           a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(w, t, sizeof(t));
}

/// The "eye to target" view matrix, row convention, LEFT-HANDED - as Metin2
/// uses (Z up, Y into the screen).
void LookAt(const float afEye[3], const float afTarget[3], float m[16])
{
    float z[3] = { afTarget[0] - afEye[0], afTarget[1] - afEye[1], afTarget[2] - afEye[2] };
    float d = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (d < 1e-6f)
        d = 1.0f;
    z[0] /= d; z[1] /= d; z[2] /= d;

    const float g[3] = { 0.0f, 0.0f, 1.0f };
    float x[3] = { g[1] * z[2] - g[2] * z[1],
                   g[2] * z[0] - g[0] * z[2],
                   g[0] * z[1] - g[1] * z[0] };
    d = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (d < 1e-6f)
        d = 1.0f;
    x[0] /= d; x[1] /= d; x[2] /= d;

    const float y[3] = { z[1] * x[2] - z[2] * x[1],
                         z[2] * x[0] - z[0] * x[2],
                         z[0] * x[1] - z[1] * x[0] };

    m[0] = x[0]; m[1] = y[0]; m[2] = z[0]; m[3] = 0.0f;
    m[4] = x[1]; m[5] = y[1]; m[6] = z[1]; m[7] = 0.0f;
    m[8] = x[2]; m[9] = y[2]; m[10] = z[2]; m[11] = 0.0f;
    m[12] = -(afEye[0] * x[0] + afEye[1] * x[1] + afEye[2] * x[2]);
    m[13] = -(afEye[0] * y[0] + afEye[1] * y[1] + afEye[2] * y[2]);
    m[14] = -(afEye[0] * z[0] + afEye[1] * z[1] + afEye[2] * z[2]);
    m[15] = 1.0f;
}

/// Left-handed perspective projection with OpenGL depth [-1, 1]
/// (`fFieldOfView` is the whole vertical angle, in radians).
void Perspective(float fFieldOfView, float fAspect, float fNear, float fFar,
                 float m[16])
{
    memset(m, 0, sizeof(float) * 16);
    const float f = 1.0f / tanf(fFieldOfView * 0.5f);
    m[0] = f / fAspect;
    m[5] = f;
    // Depth in OpenGL goes from -1 to 1, not from 0 to 1 as in Direct3D.
    m[10] = (fFar + fNear) / (fFar - fNear);
    m[11] = 1.0f;
    m[14] = -2.0f * fFar * fNear / (fFar - fNear);
}

}  // namespace

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

/// Uploads `c_afVertices` / `c_puIndices` into our own buffers and draws them
/// through `M2W_DrawMeshFromBuffers`: no blending, no culling, no alpha test.
void M2W_DrawMesh(const TVertexPNT* c_pVertices,
                  uint32_t uVertices,
                  const uint16_t* c_puIndices, uint32_t uIndices,
                  uint32_t uTexture,
                  const float* c_afWorld, const float* c_afView,
                  const float* c_afProjection)
{
    if (!c_pVertices || !c_puIndices || !uVertices || !uIndices)
        return;
    if (!Build())
        return;

    glBindBuffer(GL_ARRAY_BUFFER, s_uVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(uVertices * sizeof(TVertexPNT)),
                 c_pVertices, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_uIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(uIndices * sizeof(uint16_t)),
                 c_puIndices, GL_DYNAMIC_DRAW);

    M2W_DrawMeshFromBuffers(s_uVertexBuffer,
                            (uint32_t)sizeof(TVertexPNT), 0,
                            s_uIndexBuffer, GL_UNSIGNED_SHORT, 0, uIndices,
                            uTexture, c_afWorld, c_afView, c_afProjection,
                            M2W_BLEND_OFF, M2W_CULL_NONE,
                            M2W_ALPHA_TEST_OFF, M2W_ALPHA_REF_UNUSED);
}

namespace
{

// The steps of `M2W_DrawMeshFromBuffers`, in the order it calls them. Each
// one sends OpenGL only what differs from `s_kState` (see "State memory of
// the custom road" above) and updates `s_kState`.

/// The view-projection uniform: `view * projection * depth and pixel-centre
/// fix`, uploaded only when it differs from the last one.
///
/// THE DEPTH FIX - THE SAME AS IN THE D3D8 PIPELINE. Direct3D
/// projects depth to [0, 1], OpenGL expects [-1, 1]. The D3D8 pipeline
/// (`gl_device.cpp`, `BuildFinalMatrix`) adds `z' = 2z - w`; this road did
/// NOT, so every mesh drawn here (character skin) landed in depth [0.5, 1] -
/// always "farther" than it really was. The symptom (user,):
/// "the visibility of the character depends on the camera angle" - from
/// above the terrain behind the character is near, the character loses the
/// depth test and VANISHES, a shadow and a name remain. Two depth
/// conventions in one buffer - and a test between them means nothing.
void UploadViewProjection(const float* c_afView, const float* c_afProjection)
{
    float matViewProjectionRaw[16];
    Multiply(c_afView, c_afProjection, matViewProjectionRaw);
    // The pixel-centre fix of the D3D8 road (`BuildFinalMatrix`):
    // x' = x + w/W, y' = y - w/H - without it the character would sit half a
    // pixel off the terrain it stands on.
    float fHalfX, fHalfY;
    M2W_ClipHalfPixel(&fHalfX, &fHalfY);
    const float c_afDepth[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 2, 0,
        fHalfX, -fHalfY, -1, 1 };
    float matViewProjection[16];
    Multiply(matViewProjectionRaw, c_afDepth, matViewProjection);
    if (!s_kState.bViewProjectionKnown ||
        memcmp(s_kState.afViewProjection, matViewProjection, sizeof(matViewProjection)) != 0)
    {
        memcpy(s_kState.afViewProjection, matViewProjection, sizeof(matViewProjection));
        s_kState.bViewProjectionKnown = true;
        glUniformMatrix4fv(s_iViewProjection, 1, GL_FALSE, matViewProjection);
    }
}

/// Binds `uTexture` on unit 0 and tells the shader whether there is one
/// (0 = none: the shader uses a neutral grey instead of sampling).
void BindTexture(uint32_t uTexture)
{
    if (s_kState.uTexture != (GLuint)uTexture)
    {
        s_kState.uTexture = (GLuint)uTexture;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)uTexture);
    }
    const int iHasTexture = uTexture ? 1 : 0;
    if (s_kState.iHasTexture != iHasTexture)
    {
        s_kState.iHasTexture = iHasTexture;
        glUniform1i(s_iTexture, 0);
        glUniform1i(s_iHasTexture, iHasTexture);
    }
}

/// The alpha test uniforms: `iAlphaFunc` D3DCMP_* (1..8) or
/// `M2W_ALPHA_TEST_OFF`, `fAlphaRef` the reference 0..1.
void UploadAlphaTest(int iAlphaFunc, float fAlphaRef)
{
    if (s_kState.iAlphaFunc != iAlphaFunc || s_kState.fAlphaRef != fAlphaRef)
    {
        s_kState.iAlphaFunc = iAlphaFunc;
        s_kState.fAlphaRef = fAlphaRef;
        glUniform1i(s_iAlphaFunc, iAlphaFunc);
        glUniform1f(s_iAlphaRef, fAlphaRef);
    }
}

/// The fog uniforms set by `M2W_SetCustomFog`, and the view matrix the
/// shader measures the fog distance with (uploaded every draw).
void UploadFog(const float* c_afView)
{
    if (memcmp(s_kState.afFog, s_afFogRequested, sizeof(s_afFogRequested)) != 0 ||
        memcmp(s_kState.afFogColor, s_afFogColorRequested, sizeof(s_afFogColorRequested)) != 0)
    {
        memcpy(s_kState.afFog, s_afFogRequested, sizeof(s_afFogRequested));
        memcpy(s_kState.afFogColor, s_afFogColorRequested, sizeof(s_afFogColorRequested));
        glUniform4fv(s_iFog, 1, s_afFogRequested);
        glUniform3fv(s_iFogColor, 1, s_afFogColorRequested);
    }
    if (s_iView >= 0)
        glUniformMatrix4fv(s_iView, 1, GL_FALSE, c_afView);
}

/// Binds the index buffer and points the three attributes (PNT layout) at
/// `uVertexBuffer`, shifted by `uBaseVertex` vertices.
///
/// Direct3D 8's `BaseVertexIndex` is added to EVERY index, and OpenGL ES 3
/// has nothing to do that with (`glDrawElementsBaseVertex` only comes in ES
/// 3.2). An offset of the attribute pointers handles it - the same as the
/// compatibility layer does.
void BindBuffers(uint32_t uVertexBuffer, uint32_t uStride, uint32_t uBaseVertex,
                 uint32_t uIndexBuffer)
{
    if (s_kState.uIndexBuffer != (GLuint)uIndexBuffer)
    {
        s_kState.uIndexBuffer = (GLuint)uIndexBuffer;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)uIndexBuffer);
    }

    const uintptr_t uBase = (uintptr_t)uBaseVertex * uStride;
    const GLsizei iStride = (GLsizei)uStride;

    if (s_kState.uVertexBuffer != (GLuint)uVertexBuffer ||
        s_kState.uBase != uBase || s_kState.iStride != iStride)
    {
        s_kState.uVertexBuffer = (GLuint)uVertexBuffer;
        s_kState.uBase = uBase;
        s_kState.iStride = iStride;

        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)uVertexBuffer);
        if (s_iPosition >= 0)
            glVertexAttribPointer((GLuint)s_iPosition, 3, GL_FLOAT, GL_FALSE,
                                  iStride, (const void*)uBase);
        if (s_iNormal >= 0)
            glVertexAttribPointer((GLuint)s_iNormal, 3, GL_FLOAT, GL_FALSE,
                                  iStride,
                                  (const void*)(uBase + sizeof(float) * 3));
        if (s_iTexCoord >= 0)
            glVertexAttribPointer((GLuint)s_iTexCoord, 2, GL_FLOAT, GL_FALSE,
                                  iStride,
                                  (const void*)(uBase + sizeof(float) * 6));
    }
}

/// Depth test (once per state reset), blending and culling.
/// `iBlend` non-zero = alpha blending without depth writes; `iCull` one of
/// `M2W_CULL_NONE` / `M2W_CULL_CW` / `M2W_CULL_CCW`.
void ApplyRasterState(int iBlend, int iCull)
{
    if (!s_kState.bDepthSet)
    {
        s_kState.bDepthSet = true;
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }

    if (s_kState.iBlend != iBlend)
    {
        s_kState.iBlend = iBlend;
        if (iBlend)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            // Blended parts of a character (hair, capes) must not close the
            // depth in front of what is behind them.
            glDepthMask(GL_FALSE);
        }
        else
        {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
    }

    if (s_kState.iCull != iCull)
    {
        s_kState.iCull = iCull;
        if (iCull == M2W_CULL_NONE)
        {
            glDisable(GL_CULL_FACE);
        }
        else
        {
            glEnable(GL_CULL_FACE);
            // Direct3D `D3DCULL_CW` culls the clockwise faces, i.e. the
            // front ones are COUNTER-clockwise (= the default GL_CCW, back
            // culled) - and the other way round. WITHOUT `glFrontFace`
            // that is GLOBAL state and the fixed-function road
            // does not set it (assumes CCW); a `glFrontFace(GL_CW)` left
            // here after a shadow pass with the swapped side then culled
            // the WHOLE terrain, sky and UI.
            glCullFace((iCull == M2W_CULL_CW) ? GL_BACK : GL_FRONT);
        }
    }
}

}  // namespace

/// Draws `uIndices` indices from buffers already on the card with our own
/// program - see the declaration in custom_draw.h for every parameter.
/// Sends OpenGL only what changed since the last draw (`s_kState`), then
/// tells the compatibility layer its state memory is stale.
void M2W_DrawMeshFromBuffers(uint32_t uVertexBuffer, uint32_t uStride,
                             uint32_t uBaseVertex,
                             uint32_t uIndexBuffer, uint32_t uIndexType,
                             uint32_t uFirstIndex, uint32_t uIndices,
                             uint32_t uTexture,
                             const float* c_afWorld, const float* c_afView,
                             const float* c_afProjection,
                             int iBlend, int iCull,
                             int iAlphaFunc, float fAlphaRef)
{
    if (!uVertexBuffer || !uIndexBuffer || !uIndices || !uStride)
        return;
    if (!Build())
        return;

    m2wstats::TStopwatch kStopwatch(m2wstats::g_kCustomDraw);

    glBindVertexArray(s_uVertexArray);

    if (s_kState.uProgram != s_uProgram)
    {
        s_kState.uProgram = s_uProgram;
        glUseProgram(s_uProgram);
    }

    // The world matrix changes with every mesh - no point remembering it.
    glUniformMatrix4fv(s_iWorld, 1, GL_FALSE, c_afWorld);

    UploadViewProjection(c_afView, c_afProjection);
    BindTexture(uTexture);
    UploadAlphaTest(iAlphaFunc, fAlphaRef);
    UploadFog(c_afView);
    BindBuffers(uVertexBuffer, uStride, uBaseVertex, uIndexBuffer);
    ApplyRasterState(iBlend, iCull);

    const uint32_t uBytesPerIndex =
        (uIndexType == GL_UNSIGNED_INT) ? 4u : 2u;
    glDrawElements(GL_TRIANGLES, (GLsizei)uIndices, (GLenum)uIndexType,
                   (const void*)((uintptr_t)uFirstIndex * uBytesPerIndex));

    glBindVertexArray(0);

    // We drew outside the compatibility layer's state memory - it must know.
    M2W_ForgetGlState();
}

// ---------------------------------------------------------------------------
// The probe - and why it now does more than it did
// ---------------------------------------------------------------------------
// Earlier the probe loaded one model, composed the pose ONCE and
// deformed the skin ONCE, and then only drew the finished vertices. So it
// showed that the chain works - and nothing more.
//
// That turned out to be too little. The user reports stutter in the GAME,
// and the game does every frame something that probe never did once: for
// EVERY character it samples the animation, composes the skeleton pose and
// puts every vertex through the bones. Measuring that on the login screen
// made no sense, because there is no character there - and exactly so I
// "verified the fix" twice and was wrong twice.
//
// So the probe now does what the game does: keeps N instances of the model
// and every frame composes their pose, deforms the skin and draws. It does
// not replace a measurement in the game - the game code has its own work
// that is not here - but it lets OUR part be measured without a server and
// without waiting for the user.
//
//     ?modelprobe=1     one character  (as before)
//     ?modelprobe=20    twenty         - as many as the town shows
//
// The frame budget of `frame_stats.h` then splits it into pose, skin and
// draw.

namespace
{

/// `?probefile=d:/ymir work/pc/warrior/warrior_nahan.gr2` - a model other
/// than the default `warrior_novice` (cz.
/// 300g: the feet of the `nahan` armour off - the pose/skin core, or the
/// joining of models on the game's side).
EM_JS(char*, m2w_probe_file, (void), {
    var w = m2w.options.get('probefile'); if (!w) return 0;
    var n = lengthBytesUTF8(w) + 1, p = _malloc(n); stringToUTF8(w, p, n); return p;
});

/// `?probehash=N`: after the N-th probe frame the canvas pixels
/// are hashed and printed once - the behaviour gate for group 5 of Phase C
/// (`tools/gates/check_probe.py`): the same model, the same N, the same
/// hash before and after a rewrite of the model files.
EM_JS(int, m2w_probe_hash_frame, (void), {
    var n = parseInt(m2w.options.get('probehash') || '', 10);
    return (isFinite(n) && n > 0) ? n : 0;
});

/// FNV-1a 64 over the RGBA bytes of the whole canvas, read back from the
/// default framebuffer at the end of the frame (still valid inside the
/// same requestAnimationFrame callback).
void PrintCanvasHash(int iFrame)
{
    int iWidth = 0;
    int iHeight = 0;
    emscripten_get_canvas_element_size("#canvas", &iWidth, &iHeight);
    if (iWidth <= 0 || iHeight <= 0)
        return;
    std::vector<unsigned char> vecPixels((size_t)iWidth * (size_t)iHeight * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, iWidth, iHeight, GL_RGBA, GL_UNSIGNED_BYTE, &vecPixels[0]);
    unsigned long long ullHash = 14695981039346656037ULL;
    for (size_t i = 0; i < vecPixels.size(); ++i)
    {
        ullHash ^= vecPixels[i];
        ullHash *= 1099511628211ULL;
    }
    std::printf("m2w probe: canvas hash after %d frames = %016llx (%dx%d)\n",
                iFrame, ullHash, iWidth, iHeight);
}

/// `?modelprobe=N` clamped to 1..200; 0 when absent.
EM_JS(int, m2w_probe_count, (void), {
    var w = m2w.options.get('modelprobe'); if (!w) return 0;
    var n = parseInt(w, 10); if (!isFinite(n) || n < 1) n = 1; return n > 200 ? 200 : n;
});

/// One mesh of the model, ready for per-frame deformation.
struct TProbeMesh
{
    granny_mesh* pMesh = NULL;
    granny_mesh_binding* pBinding = NULL;
    granny_mesh_deformer* pDeformer = NULL;
    bool bRigid = false;
    std::vector<TVertexPNT> vecVertices;
    std::vector<uint16_t> vecIndices;
};

/// One model instance - as much state as the game keeps per character.
struct TProbeInstance
{
    granny_model_instance* pInstance = NULL;
    granny_skeleton* pSkeleton = NULL;
    granny_local_pose* pLocalPose = NULL;
    granny_world_pose* pWorldPose = NULL;
    std::vector<TProbeMesh> vecMeshes;
    float fX = 0.0f;
    float fY = 0.0f;
};

std::vector<TProbeInstance> s_vecInstances;
bool s_bProbeLoaded = false;
bool s_bProbeFailed = false;
float s_fRotation = 0.0f;

/// Loads a model file through OUR reader. Returns the file info or NULL.
/// MODEL FILES SIT IN PACKS, not loose - `fopen` does not see them, so the
/// same road as the game: the pack manager.
granny_file_info* LoadModelFile(const char* c_szName)
{
    static CMappedFile s_kMappedFile;
    LPCVOID c_pvData = NULL;
    if (!CEterPackManager::Instance().Get(s_kMappedFile, c_szName, &c_pvData))
        return NULL;

    granny_file* pFile = GrannyReadEntireFileFromMemory(
        (granny_int32x)s_kMappedFile.Size(), (void*)c_pvData);
    if (!pFile)
        return NULL;
    return GrannyGetFileInfo(pFile);
}

/// Builds one instance: pose, bindings and deformers for every mesh.
bool BuildInstance(granny_model* pModel, TProbeInstance& rInstance)
{
    rInstance.pInstance = GrannyInstantiateModel(pModel);
    rInstance.pSkeleton = GrannyGetSourceSkeleton(rInstance.pInstance);
    if (!rInstance.pSkeleton)
        return false;

    rInstance.pLocalPose = GrannyNewLocalPose(rInstance.pSkeleton->BoneCount);
    rInstance.pWorldPose = GrannyNewWorldPose(rInstance.pSkeleton->BoneCount);

    for (granny_int32 mb = 0; mb < pModel->MeshBindingCount; ++mb)
    {
        granny_mesh* pMesh = pModel->MeshBindings[mb].Mesh;
        if (!pMesh)
            continue;
        const granny_int32x iVertices = GrannyGetMeshVertexCount(pMesh);
        const granny_int32x iIndices = GrannyGetMeshIndexCount(pMesh);
        if (iVertices <= 0 || iIndices <= 0)
            continue;

        TProbeMesh kMesh;
        kMesh.pMesh = pMesh;
        kMesh.bRigid = GrannyMeshIsRigid(pMesh) != 0;
        kMesh.vecVertices.resize((size_t)iVertices);
        kMesh.vecIndices.resize((size_t)iIndices);
        GrannyCopyMeshIndices(pMesh, 2, &kMesh.vecIndices[0]);

        if (kMesh.bRigid)
        {
            // A rigid mesh does not change between frames - copied once.
            GrannyCopyMeshVertices(pMesh, GrannyPNT332VertexType,
                                   &kMesh.vecVertices[0]);
        }
        else
        {
            kMesh.pBinding = GrannyNewMeshBinding(pMesh, rInstance.pSkeleton,
                                                  rInstance.pSkeleton);
            kMesh.pDeformer = GrannyNewMeshDeformer(
                GrannyGetMeshVertexType(pMesh), GrannyPNT332VertexType,
                GrannyDeformPositionNormal, GrannyAllowUncopiedTail);
        }
        rInstance.vecMeshes.push_back(kMesh);
    }
    return !rInstance.vecMeshes.empty();
}

}  // namespace

namespace
{

// The probe scene, in game units (a character is ~180 tall). Chosen so that
// twenty characters fit on the screen; the check_probe gate
// hashes this exact picture, so changing any of them means re-recording
// tools/gates/probe_baseline.json.
const int c_iProbeRow = 10;                 // instances per row
const float c_fProbeSpacingX = 120.0f;      // between instances in a row
const float c_fProbeSpacingY = 140.0f;      // between rows
const float c_fProbeOrbitStep = 0.01f;      // camera turn per frame, radians
const float c_fProbeOrbitBase = 320.0f;     // orbit radius for zero instances...
const float c_fProbeOrbitPerInstance = 40.0f;   // ...plus this per instance
const float c_fProbeEyeHeight = 150.0f;     // camera height
const float c_fProbeTargetHeight = 90.0f;   // looks at mid-body
const float c_fProbeFieldOfView = 1.0f;     // vertical, radians (~57 degrees)
const float c_fProbeNear = 10.0f;
const float c_fProbeFar = 20000.0f;

/// Loads the probe model (`?probefile=` first, then the novice warrior) and
/// builds `iCount` animated instances of it into `s_vecInstances`; false when
/// no model loads or an instance does not build.
bool LoadProbeInstances(int iCount)
{
    static const char* c_aszCandidates[] = {
        "d:/ymir work/pc/warrior/warrior_novice.gr2",
        "D:/Ymir Work/pc/warrior/warrior_novice.gr2",
        "d:/ymir work/pc/warrior/warrior_novice_lod_01.gr2",
    };
    granny_file_info* pInfo = NULL;
    if (char* pszOwn = m2w_probe_file())
    {
        pInfo = LoadModelFile(pszOwn);
        std::printf("m2w probe: model from the address [%s] - %s\n", pszOwn,
                    (pInfo && pInfo->ModelCount > 0) ? "loaded" : "NOT loaded");
        if (!(pInfo && pInfo->ModelCount > 0))
            pInfo = NULL;
        free(pszOwn);
    }
    for (size_t i = 0;
         !pInfo && i < sizeof(c_aszCandidates) / sizeof(c_aszCandidates[0]); ++i)
    {
        pInfo = LoadModelFile(c_aszCandidates[i]);
        if (pInfo && pInfo->ModelCount > 0)
            break;
        pInfo = NULL;
    }
    if (!pInfo)
    {
        std::printf("m2w probe: no model could be loaded\n");
        return false;
    }

    granny_model* pModel = pInfo->Models[0];
    for (int i = 0; i < iCount; ++i)
    {
        TProbeInstance kInstance;
        // Rows of ten, so that all twenty are visible.
        kInstance.fX = (float)((i % c_iProbeRow) - c_iProbeRow / 2) * c_fProbeSpacingX;
        kInstance.fY = (float)(i / c_iProbeRow) * c_fProbeSpacingY;
        if (!BuildInstance(pModel, kInstance))
        {
            std::printf("m2w probe: instance %d did not build\n", i);
            return false;
        }
        s_vecInstances.push_back(kInstance);
    }
    std::printf("m2w probe: built %d instances of %d meshes\n",
                (int)s_vecInstances.size(),
                (int)s_vecInstances[0].vecMeshes.size());
    return true;
}

/// Poses, skins and draws every probe instance with `matView` /
/// `matProjection` - the same per-frame work the game does for a character.
void DrawProbeInstances(const float* matView, const float* matProjection)
{
    for (size_t w = 0; w < s_vecInstances.size(); ++w)
    {
        TProbeInstance& rInstance = s_vecInstances[w];

        // THE SAME THE GAME DOES TO EVERY CHARACTER EVERY FRAME.
        GrannySampleModelAnimationsAccelerated(
            rInstance.pInstance, rInstance.pSkeleton->BoneCount, NULL,
            rInstance.pLocalPose, rInstance.pWorldPose);
        granny_matrix_4x4* pComposite =
            GrannyGetWorldPoseComposite4x4Array(rInstance.pWorldPose);

        float matWorld[16];
        Identity(matWorld);
        matWorld[12] = rInstance.fX;
        matWorld[13] = rInstance.fY;

        for (size_t s = 0; s < rInstance.vecMeshes.size(); ++s)
        {
            TProbeMesh& rMesh = rInstance.vecMeshes[s];
            if (!rMesh.bRigid)
            {
                GrannyDeformVertices(
                    rMesh.pDeformer,
                    GrannyGetMeshBindingToBoneIndices(rMesh.pBinding),
                    (const granny_real32*)pComposite,
                    (granny_int32x)rMesh.vecVertices.size(),
                    GrannyGetMeshVertices(rMesh.pMesh),
                    &rMesh.vecVertices[0]);
            }
            M2W_DrawMesh(&rMesh.vecVertices[0],
                         (uint32_t)rMesh.vecVertices.size(),
                         &rMesh.vecIndices[0], (uint32_t)rMesh.vecIndices.size(),
                         0, matWorld, matView, matProjection);
        }
    }
}

}  // namespace

/// See custom_draw.h: runs the texture probe, then (with `?modelprobe=N`)
/// loads the model once and draws N instances every frame, orbiting camera.
void M2W_ModelProbe()
{
    // The second probe hangs on the same hook, because both serve the same
    // thing: measuring in the client what cannot be seen without a server.
    // Each watches its own address switch, so they do not get in each
    // other's way.
    M2W_TextureProbe();

    static int s_iCount = -1;
    if (s_iCount < 0)
    {
        s_iCount = m2w_probe_count();
        if (s_iCount > 0)
            std::printf("m2w probe: %d model instances, pose and skin "
                        "computed EVERY FRAME\n", s_iCount);
    }
    if (!s_iCount || s_bProbeFailed)
        return;

    if (!s_bProbeLoaded)
    {
        s_bProbeLoaded = true;
        if (!LoadProbeInstances(s_iCount))
        {
            s_bProbeFailed = true;
            return;
        }
    }

    // `?probehash=N` - the gate of Phase C group 5, see m2w_probe_hash_frame.
    // With the hash on, the frame is cleared first: the login screen behind
    // the probe is not deterministic frame for frame (measured:
    // two runs, two hashes), and the gate is about OUR chain only.
    static int s_iHashFrame = -1;
    static int s_iProbeFrames = 0;
    if (s_iHashFrame < 0)
        s_iHashFrame = m2w_probe_hash_frame();
    if (s_iHashFrame > 0)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // The camera circles - a rotation shows the body better than a still.
    s_fRotation += c_fProbeOrbitStep;
    const float fRadius = c_fProbeOrbitBase + c_fProbeOrbitPerInstance * (float)s_vecInstances.size();
    const float afEye[3] = { sinf(s_fRotation) * fRadius,
                             -cosf(s_fRotation) * fRadius, c_fProbeEyeHeight };
    const float afTarget[3] = { 0.0f, 0.0f, c_fProbeTargetHeight };

    float matView[16];
    float matProjection[16];
    LookAt(afEye, afTarget, matView);

    int iWidth = 0;
    int iHeight = 0;
    emscripten_get_canvas_element_size("#canvas", &iWidth, &iHeight);
    const float fAspect = (iHeight > 0)
        ? (float)iWidth / (float)iHeight : 1.0f;
    Perspective(c_fProbeFieldOfView, fAspect, c_fProbeNear, c_fProbeFar, matProjection);

    DrawProbeInstances(matView, matProjection);

    if (s_iHashFrame > 0 && ++s_iProbeFrames == s_iHashFrame)
        PrintCanvasHash(s_iProbeFrames);
}

// ---------------------------------------------------------------------------
// The texture probe
// ---------------------------------------------------------------------------
// Why a separate probe when there is already the frame budget: the budget
// says how much time went into uploading textures, but only when SOMETHING
// uploads them. On the login screen almost nothing does, and in the game -
// a great deal at once, when entering new terrain. Until now that could not
// be checked without a server and without the user.
//
// This probe does what the game does when entering a map: takes a
// DIRECTORY of textures and loads everything in it, one after another, by
// the same road - through `CResourceManager`. It measures the whole, the
// worst single file and the sum of bytes. If loading a hundred textures
// takes seconds, that is the cause of the stutter I am looking for - and it
// shows without entering the game.
//
//     ?textureprobe=d:/ymir work/pc/warrior/
//     ?textureprobe=1     - the default directory (as above)

namespace
{

/// `?textureprobe` as a malloc'd string, 0 when absent.
EM_JS(char*, m2w_texture_dir, (void), {
    var w = m2w.options.get('textureprobe'); if (!w) return 0; if (w === '1') w = 'd:/ymir work/pc/warrior/';
    var n = lengthBytesUTF8(w) + 1, p = _malloc(n); stringToUTF8(w, p, n); return p;
});

/// Does the name end with this extension (case-insensitive)?
bool EndsWith(const char* c_szName, const char* c_szSuffix)
{
    const size_t uN = strlen(c_szName);
    const size_t uS = strlen(c_szSuffix);
    if (uN < uS)
        return false;
    for (size_t i = 0; i < uS; ++i)
    {
        char a = c_szName[uN - uS + i];
        char b = c_szSuffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

}  // namespace

void M2W_TextureProbe()
{
    static int s_iDone = 0;
    if (s_iDone)
        return;
    s_iDone = 1;

    char* pszDirectory = m2w_texture_dir();
    if (!pszDirectory)
        return;

    // The directory in the file system has no drive letter - the game adds
    // it itself and `CResourceManager` strips it. Both forms are needed
    // here: one to list the directory, the other to ask for the resource.
    std::string strWithDrive(pszDirectory);
    free(pszDirectory);
    if (!strWithDrive.empty() && strWithDrive[strWithDrive.size() - 1] != '/')
        strWithDrive += "/";

    std::string strOnDisk = strWithDrive;
    if (strOnDisk.size() > 2 && strOnDisk[1] == ':')
        strOnDisk = "/" + strOnDisk.substr(3);

    DIR* pDirectory = opendir(strOnDisk.c_str());
    if (!pDirectory)
    {
        std::printf("m2w texture probe: no directory %s\n",
                    strOnDisk.c_str());
        return;
    }

    std::vector<std::string> vecNames;
    for (struct dirent* pEntry = readdir(pDirectory); pEntry;
         pEntry = readdir(pDirectory))
    {
        if (EndsWith(pEntry->d_name, ".dds") || EndsWith(pEntry->d_name, ".tga") ||
            EndsWith(pEntry->d_name, ".jpg"))
            vecNames.push_back(pEntry->d_name);
    }
    closedir(pDirectory);

    if (vecNames.empty())
    {
        std::printf("m2w texture probe: no images in %s\n",
                    strOnDisk.c_str());
        return;
    }

    std::printf("m2w texture probe: %d images in %s - loading\n",
                (int)vecNames.size(), strWithDrive.c_str());

    const double dStart = m2wstats::Now();
    double dWorst = 0.0;
    std::string strWorst;
    int iLoaded = 0;

    for (size_t i = 0; i < vecNames.size(); ++i)
    {
        const std::string strFull = strWithDrive + vecNames[i];
        const double dBefore = m2wstats::Now();

        CResource* pResource =
            CResourceManager::Instance().GetResourcePointer(strFull.c_str());
        if (pResource)
        {
            // ONLY `Load()`. The first version also called
            // `CreateDeviceObjects()` to send the texture to the card "for
            // sure" - and the client CRASHED on `assert(m_lpd3dTexture == NULL)`
            // in `GrpImageTexture.cpp:43`. Reason: `Load()` already sent it,
            // and a second upload is in that class a program error, not a
            // repeat. Measured: abort in the first loop iteration.
            pResource->Load();
            ++iLoaded;
        }

        const double dThis = m2wstats::Now() - dBefore;
        if (dThis > dWorst)
        {
            dWorst = dThis;
            strWorst = vecNames[i];
        }
    }

    const double dTotal = m2wstats::Now() - dStart;
    std::printf("m2w texture probe: %d of %d in %.0f ms (%.1f ms per image), "
                "worst %.0f ms - %s\n",
                iLoaded, (int)vecNames.size(), dTotal,
                dTotal / (double)vecNames.size(), dWorst,
                strWorst.c_str());
}
