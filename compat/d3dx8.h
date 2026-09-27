// SPDX-License-Identifier: GPL-2.0-or-later
// d3dx8.h - the D3DX8 MATHS layer for the port.
//
// THE MEASUREMENT behind this file: the whole of `GameLib` (59 files)
// uses **six types, one constant and 24 functions** of D3DX - nothing more.
// `D3DXVECTOR3` alone occurs 448 times, `D3DXMATRIX` 68. This is category B
// (algorithms and constants): it can be written from scratch and **checked by
// its properties**, without knowing Microsoft's original code.
//
// -------------------------------------------------------------------------
// CONVENTION - this is where a transposition mistake is easy to make
// -------------------------------------------------------------------------
// D3DX computes with **row vectors**: v' = v * M. The translation therefore
// sits in the FOURTH ROW (`_41 _42 _43`), not in the fourth column.
//
// The measurement confirms it: in `GameLib` `_41` occurs 39 times, `_42` 39,
// `_43` 29 - almost exclusively translation. TMP4 code **reads these fields
// directly**, so the convention must not change.
//
// A LUCKY COINCIDENCE worth writing down, so that nobody "fixes" it later:
// the MEMORY layout of a D3DX (row-major) matrix is **the same** as that of an
// OpenGL (column-major) matrix. D3DX stores `_11 _12 _13 _14 _21 ...`, so
// `_41` sits at index 12 - exactly where OpenGL keeps the X translation. The
// difference between `v*M` and `M*v` is purely NOTATIONAL. A D3DX matrix can
// be handed to GL without transposing it.
//
// -------------------------------------------------------------------------
// WHAT IS NOT HERE
// -------------------------------------------------------------------------
// Loading of textures, meshes, fonts and shaders (`D3DXCreateTexture*`,
// `D3DXLoadSurfaceFrom*`, `D3DXAssembleShader`). `GameLib` does not call
// them - checked with grep, zero occurrences. When they showed up with the
// next layer they became a separate decision, because that is no longer
// maths (see the declarations at the end of the file).

#pragma once

#include <cmath>

#include "win32_compat.h"
#include "d3d8.h"

#ifndef FLOAT
typedef float FLOAT;
#endif
#ifndef CONST
#define CONST const
#endif

#define D3DX_PI    (3.14159265358979323846f)
#define D3DX_1BYPI (0.318309886183790671538f)

/// Degrees to radians (`degree * pi / 180`), as a float expression.
#define D3DXToRadian(degree) ((degree) * (D3DX_PI / 180.0f))
/// Radians to degrees (`radian * 180 / pi`), as a float expression.
#define D3DXToDegree(radian) ((radian) * (180.0f / D3DX_PI))

// ===========================================================================
// Types
// ===========================================================================

/// Two-component float vector with the D3DX operators (component-wise
/// add/subtract, scaling by a scalar, exact comparison). Converts to
/// `float*` pointing at `x`.
struct D3DXVECTOR2
{
    float x, y;

    /// Uninitialised, as in D3DX (no zeroing - the caller fills it).
    D3DXVECTOR2() {}
    /// From the components, in field order.
    D3DXVECTOR2(float fx, float fy) : x(fx), y(fy) {}
    /// From 2 floats at `p`.
    D3DXVECTOR2(CONST float* p) : x(p[0]), y(p[1]) {}

    /// The components as a float array (pointer to the first field).
    operator float*()             { return &x; }
    /// The components as a read-only float array.
    operator CONST float*() const { return &x; }

    /// Component-wise sum.
    D3DXVECTOR2 operator+(CONST D3DXVECTOR2& v) const { return D3DXVECTOR2(x + v.x, y + v.y); }
    /// Component-wise difference.
    D3DXVECTOR2 operator-(CONST D3DXVECTOR2& v) const { return D3DXVECTOR2(x - v.x, y - v.y); }
    /// Every component times `s`.
    D3DXVECTOR2 operator*(float s)              const { return D3DXVECTOR2(x * s, y * s); }
    /// Every component divided by `s` (no check for zero).
    D3DXVECTOR2 operator/(float s)              const { return D3DXVECTOR2(x / s, y / s); }
    /// Every component negated.
    D3DXVECTOR2 operator-()                     const { return D3DXVECTOR2(-x, -y); }
    /// Adds `v` component-wise.
    D3DXVECTOR2& operator+=(CONST D3DXVECTOR2& v) { x += v.x; y += v.y; return *this; }
    /// Subtracts `v` component-wise.
    D3DXVECTOR2& operator-=(CONST D3DXVECTOR2& v) { x -= v.x; y -= v.y; return *this; }
    /// Multiplies every component by `s`.
    D3DXVECTOR2& operator*=(float s) { x *= s; y *= s; return *this; }
    /// Divides every component by `s` (no check for zero).
    D3DXVECTOR2& operator/=(float s) { x /= s; y /= s; return *this; }
    /// EXACT comparison of every component (no tolerance).
    bool operator==(CONST D3DXVECTOR2& v) const { return x == v.x && y == v.y; }
    /// The negation of `==`.
    bool operator!=(CONST D3DXVECTOR2& v) const { return !(*this == v); }
};

/// `s * v` - the same as `v * s`.
inline D3DXVECTOR2 operator*(float s, CONST D3DXVECTOR2& v) { return v * s; }

/// Derives from `D3DVECTOR` in `d3d8.h`, as in the original. Without that,
/// TMP4 code could not pass a `D3DVECTOR` where a `D3DXVECTOR3` is expected
/// - and it does (`EterGrnLib` passes Granny's vertices directly).
/// It adds no fields of its own, so it is still three `float`s.
struct D3DXVECTOR3 : public D3DVECTOR
{
    /// Uninitialised, as in D3DX (no zeroing - the caller fills it).
    D3DXVECTOR3() {}
    /// From the components, in field order.
    D3DXVECTOR3(float fx, float fy, float fz) { x = fx; y = fy; z = fz; }
    /// From a `D3DVECTOR` (same three components).
    D3DXVECTOR3(CONST D3DVECTOR& v) { x = v.x; y = v.y; z = v.z; }
    /// From 3 floats at `p`.
    D3DXVECTOR3(CONST float* p) { x = p[0]; y = p[1]; z = p[2]; }

    /// The components as a float array (pointer to the first field).
    operator float*()             { return &x; }
    /// The components as a read-only float array.
    operator CONST float*() const { return &x; }

    /// Component-wise sum.
    D3DXVECTOR3 operator+(CONST D3DXVECTOR3& v) const { return D3DXVECTOR3(x + v.x, y + v.y, z + v.z); }
    /// Component-wise difference.
    D3DXVECTOR3 operator-(CONST D3DXVECTOR3& v) const { return D3DXVECTOR3(x - v.x, y - v.y, z - v.z); }
    /// Every component times `s`.
    D3DXVECTOR3 operator*(float s)              const { return D3DXVECTOR3(x * s, y * s, z * s); }
    /// Every component divided by `s` (no check for zero).
    D3DXVECTOR3 operator/(float s)              const { return D3DXVECTOR3(x / s, y / s, z / s); }
    /// An unchanged copy (unary plus).
    D3DXVECTOR3 operator+()                     const { return *this; }
    /// Every component negated.
    D3DXVECTOR3 operator-()                     const { return D3DXVECTOR3(-x, -y, -z); }
    /// Adds `v` component-wise.
    D3DXVECTOR3& operator+=(CONST D3DXVECTOR3& v) { x += v.x; y += v.y; z += v.z; return *this; }
    /// Subtracts `v` component-wise.
    D3DXVECTOR3& operator-=(CONST D3DXVECTOR3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    /// Multiplies every component by `s`.
    D3DXVECTOR3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    /// Divides every component by `s` (no check for zero).
    D3DXVECTOR3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
    /// EXACT comparison of every component (no tolerance).
    bool operator==(CONST D3DXVECTOR3& v) const { return x == v.x && y == v.y && z == v.z; }
    /// The negation of `==`.
    bool operator!=(CONST D3DXVECTOR3& v) const { return !(*this == v); }
};

/// `s * v` - the same as `v * s`.
inline D3DXVECTOR3 operator*(float s, CONST D3DXVECTOR3& v) { return v * s; }

/// Four-component float vector with the D3DX operators (component-wise
/// add/subtract, scaling, exact comparison; no `/=`). Converts to `float*`
/// pointing at `x`.
struct D3DXVECTOR4
{
    float x, y, z, w;

    /// Uninitialised, as in D3DX (no zeroing - the caller fills it).
    D3DXVECTOR4() {}
    /// From the components, in field order.
    D3DXVECTOR4(float fx, float fy, float fz, float fw) : x(fx), y(fy), z(fz), w(fw) {}
    /// From 4 floats at `p`.
    D3DXVECTOR4(CONST float* p) : x(p[0]), y(p[1]), z(p[2]), w(p[3]) {}

    /// The components as a float array (pointer to the first field).
    operator float*()             { return &x; }
    /// The components as a read-only float array.
    operator CONST float*() const { return &x; }

    /// Component-wise sum.
    D3DXVECTOR4 operator+(CONST D3DXVECTOR4& v) const { return D3DXVECTOR4(x + v.x, y + v.y, z + v.z, w + v.w); }
    /// Component-wise difference.
    D3DXVECTOR4 operator-(CONST D3DXVECTOR4& v) const { return D3DXVECTOR4(x - v.x, y - v.y, z - v.z, w - v.w); }
    /// Every component times `s`.
    D3DXVECTOR4 operator*(float s)              const { return D3DXVECTOR4(x * s, y * s, z * s, w * s); }
    /// Every component divided by `s` (no check for zero).
    D3DXVECTOR4 operator/(float s)              const { return D3DXVECTOR4(x / s, y / s, z / s, w / s); }
    /// Every component negated.
    D3DXVECTOR4 operator-()                     const { return D3DXVECTOR4(-x, -y, -z, -w); }
    /// Adds `v` component-wise.
    D3DXVECTOR4& operator+=(CONST D3DXVECTOR4& v) { x += v.x; y += v.y; z += v.z; w += v.w; return *this; }
    /// Subtracts `v` component-wise.
    D3DXVECTOR4& operator-=(CONST D3DXVECTOR4& v) { x -= v.x; y -= v.y; z -= v.z; w -= v.w; return *this; }
    /// Multiplies every component by `s`.
    D3DXVECTOR4& operator*=(float s) { x *= s; y *= s; z *= s; w *= s; return *this; }
    /// EXACT comparison of every component (no tolerance).
    bool operator==(CONST D3DXVECTOR4& v) const { return x == v.x && y == v.y && z == v.z && w == v.w; }
    /// The negation of `==`.
    bool operator!=(CONST D3DXVECTOR4& v) const { return !(*this == v); }
};

/// Quaternion. Field order `x y z w` - imaginary part BEFORE the real one,
/// as in D3DX. TMP4 code builds quaternions with an initialiser list, so the
/// reverse order would silently twist the rotations.
struct D3DXQUATERNION
{
    float x, y, z, w;

    /// Uninitialised, as in D3DX (no zeroing - the caller fills it).
    D3DXQUATERNION() {}
    /// From the components, in field order.
    D3DXQUATERNION(float fx, float fy, float fz, float fw) : x(fx), y(fy), z(fz), w(fw) {}
    /// From 4 floats at `p`.
    D3DXQUATERNION(CONST float* p) : x(p[0]), y(p[1]), z(p[2]), w(p[3]) {}

    /// The components as a float array (pointer to the first field).
    operator float*()             { return &x; }
    /// The components as a read-only float array.
    operator CONST float*() const { return &x; }

    /// Component-wise sum.
    D3DXQUATERNION operator+(CONST D3DXQUATERNION& q) const { return D3DXQUATERNION(x + q.x, y + q.y, z + q.z, w + q.w); }
    /// Component-wise difference.
    D3DXQUATERNION operator-(CONST D3DXQUATERNION& q) const { return D3DXQUATERNION(x - q.x, y - q.y, z - q.z, w - q.w); }
    /// Every component (w included) times `s` - a scaling, NOT the quaternion product (that is `D3DXQuaternionMultiply`).
    D3DXQUATERNION operator*(float s)                 const { return D3DXQUATERNION(x * s, y * s, z * s, w * s); }
    /// Every component negated.
    D3DXQUATERNION operator-()                        const { return D3DXQUATERNION(-x, -y, -z, -w); }
    /// EXACT comparison of every component (no tolerance).
    bool operator==(CONST D3DXQUATERNION& q) const { return x == q.x && y == q.y && z == q.z && w == q.w; }
    /// The negation of `==`.
    bool operator!=(CONST D3DXQUATERNION& q) const { return !(*this == q); }
};

/// Derives from `D3DMATRIX` in `d3d8.h`, as in the original. It adds no
/// fields of its own, so the memory layout is the same, and passing a
/// `D3DXMATRIX` where a `D3DMATRIX` is expected costs nothing.
struct D3DXMATRIX : public D3DMATRIX
{
    /// Uninitialised, as in D3DX (no zeroing - the caller fills it).
    D3DXMATRIX() {}
    /// From 16 floats at `p`.
    D3DXMATRIX(CONST float* p)
    {
        for (int i = 0; i < 16; ++i) (&_11)[i] = p[i];
    }
    /// From a `D3DMATRIX` (same layout).
    D3DXMATRIX(CONST D3DMATRIX& o) : D3DMATRIX(o) {}
    /// From sixteen elements, row by row (`f41..f43` is the translation).
    D3DXMATRIX(float f11, float f12, float f13, float f14,
               float f21, float f22, float f23, float f24,
               float f31, float f32, float f33, float f34,
               float f41, float f42, float f43, float f44)
    {
        _11 = f11; _12 = f12; _13 = f13; _14 = f14;
        _21 = f21; _22 = f22; _23 = f23; _24 = f24;
        _31 = f31; _32 = f32; _33 = f33; _34 = f34;
        _41 = f41; _42 = f42; _43 = f43; _44 = f44;
    }

    /// Element at row `r`, column `c` (0-based).
    float& operator()(unsigned r, unsigned c)             { return m[r][c]; }
    /// Element at row `r`, column `c` (0-based), by value.
    float  operator()(unsigned r, unsigned c) const       { return m[r][c]; }

    /// The components as a float array (pointer to the first field).
    operator float*()             { return &_11; }
    /// The components as a read-only float array.
    operator CONST float*() const { return &_11; }

    /// Matrix product `*this * o` (row convention: first this, then `o`).
    D3DXMATRIX operator*(CONST D3DXMATRIX& o) const
    {
        D3DXMATRIX r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += m[i][k] * o.m[k][j];
                r.m[i][j] = s;
            }
        }
        return r;
    }
    /// `*this = *this * o`.
    D3DXMATRIX& operator*=(CONST D3DXMATRIX& o) { *this = *this * o; return *this; }
    /// EXACT comparison of every component (no tolerance).
    bool operator==(CONST D3DXMATRIX& o) const
    {
        for (int i = 0; i < 16; ++i) if ((&_11)[i] != (&o._11)[i]) return false;
        return true;
    }
    /// The negation of `==`.
    bool operator!=(CONST D3DXMATRIX& o) const { return !(*this == o); }
};

/// Plane `a*x + b*y + c*z + d = 0`, stored as four floats in that order.
/// Only construction, `float*` conversion and exact comparison - the maths
/// is in the `D3DXPlane*` functions below.
struct D3DXPLANE
{
    float a, b, c, d;

    /// Uninitialised, as in D3DX (no zeroing - the caller fills it).
    D3DXPLANE() {}
    /// From the components, in field order.
    D3DXPLANE(float fa, float fb, float fc, float fd) : a(fa), b(fb), c(fc), d(fd) {}
    /// From 4 floats at `p`.
    D3DXPLANE(CONST float* p) : a(p[0]), b(p[1]), c(p[2]), d(p[3]) {}

    /// The components as a float array (pointer to the first field).
    operator float*()             { return &a; }
    /// The components as a read-only float array.
    operator CONST float*() const { return &a; }

    /// EXACT comparison of every component (no tolerance).
    bool operator==(CONST D3DXPLANE& p) const { return a == p.a && b == p.b && c == p.c && d == p.d; }
    /// The negation of `==`.
    bool operator!=(CONST D3DXPLANE& p) const { return !(*this == p); }
};

/// Derives from `D3DCOLORVALUE`, as in the original.
struct D3DXCOLOR : public D3DCOLORVALUE
{
    /// Uninitialised, as in D3DX (no zeroing - the caller fills it).
    D3DXCOLOR() {}
    /// From the components, in field order.
    D3DXCOLOR(float fr, float fg, float fb, float fa) { r = fr; g = fg; b = fb; a = fa; }
    /// From a `D3DCOLORVALUE` (same four channels).
    D3DXCOLOR(CONST D3DCOLORVALUE& c) { r = c.r; g = c.g; b = c.b; a = c.a; }
    /// From 4 floats at `p`.
    D3DXCOLOR(CONST float* p) { r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
    /// From a packed `D3DCOLOR` (A8R8G8B8).
    D3DXCOLOR(DWORD argb)
    {
        r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
        g = static_cast<float>((argb >>  8) & 0xFFu) / 255.0f;
        b = static_cast<float>((argb      ) & 0xFFu) / 255.0f;
        a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    }

    /// Back to a packed `D3DCOLOR`. TMP4 code assigns a `D3DXCOLOR` directly
    /// to a `DWORD` field (a vertex colour), so this conversion has to exist
    /// - and it has to clamp, because multiplying colours can leave [0,1].
    operator DWORD() const
    {
        const float cr = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        const float cg = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
        const float cb = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
        const float ca = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        return (static_cast<DWORD>(ca * 255.0f + 0.5f) << 24)
             | (static_cast<DWORD>(cr * 255.0f + 0.5f) << 16)
             | (static_cast<DWORD>(cg * 255.0f + 0.5f) <<  8)
             |  static_cast<DWORD>(cb * 255.0f + 0.5f);
    }

    /// The components as a float array (pointer to the first field).
    operator float*()             { return &r; }
    /// The components as a read-only float array.
    operator CONST float*() const { return &r; }

    /// Component-wise sum.
    D3DXCOLOR operator+(CONST D3DXCOLOR& c) const { return D3DXCOLOR(r + c.r, g + c.g, b + c.b, a + c.a); }
    /// Component-wise difference.
    D3DXCOLOR operator-(CONST D3DXCOLOR& c) const { return D3DXCOLOR(r - c.r, g - c.g, b - c.b, a - c.a); }
    /// Every channel (alpha included) times `s`.
    D3DXCOLOR operator*(float s)            const { return D3DXCOLOR(r * s, g * s, b * s, a * s); }
    /// EXACT comparison of every component (no tolerance).
    bool operator==(CONST D3DXCOLOR& c) const { return r == c.r && g == c.g && b == c.b && a == c.a; }
    /// The negation of `==`.
    bool operator!=(CONST D3DXCOLOR& c) const { return !(*this == c); }
};

// ===========================================================================
// Vectors
// ===========================================================================

/// Dot product of two 2D vectors.
inline FLOAT D3DXVec2Dot(CONST D3DXVECTOR2* v1, CONST D3DXVECTOR2* v2)
{
    return v1->x * v2->x + v1->y * v2->y;
}

/// Writes `v / |v|` to `out` and returns `out`; a zero vector gives zero.
inline D3DXVECTOR2* D3DXVec2Normalize(D3DXVECTOR2* out, CONST D3DXVECTOR2* v)
{
    const float len = std::sqrt(v->x * v->x + v->y * v->y);
    // At zero length D3DX returns the zero vector, not NaN. TMP4 code
    // normalises directions that can be zero (a character standing still),
    // so this case is NOT theoretical.
    if (len == 0.0f) { out->x = 0.0f; out->y = 0.0f; return out; }
    out->x = v->x / len;
    out->y = v->y / len;
    return out;
}

/// `out = v1 + v2`; returns `out`.
inline D3DXVECTOR3* D3DXVec3Add(D3DXVECTOR3* out, CONST D3DXVECTOR3* v1, CONST D3DXVECTOR3* v2)
{
    out->x = v1->x + v2->x;
    out->y = v1->y + v2->y;
    out->z = v1->z + v2->z;
    return out;
}

/// `out = v1 - v2`; returns `out`.
inline D3DXVECTOR3* D3DXVec3Subtract(D3DXVECTOR3* out, CONST D3DXVECTOR3* v1, CONST D3DXVECTOR3* v2)
{
    out->x = v1->x - v2->x;
    out->y = v1->y - v2->y;
    out->z = v1->z - v2->z;
    return out;
}

/// Dot product of two 3D vectors.
inline FLOAT D3DXVec3Dot(CONST D3DXVECTOR3* v1, CONST D3DXVECTOR3* v2)
{
    return v1->x * v2->x + v1->y * v2->y + v1->z * v2->z;
}

/// `out = v1 x v2` (cross product); `out` may alias either input.
inline D3DXVECTOR3* D3DXVec3Cross(D3DXVECTOR3* out, CONST D3DXVECTOR3* v1, CONST D3DXVECTOR3* v2)
{
    const D3DXVECTOR3 r(v1->y * v2->z - v1->z * v2->y,
                        v1->z * v2->x - v1->x * v2->z,
                        v1->x * v2->y - v1->y * v2->x);
    *out = r;  // through a temporary: `out` may be the same object as `v1`
    return out;
}

/// Squared length `x*x + y*y + z*z` (no square root).
inline FLOAT D3DXVec3LengthSq(CONST D3DXVECTOR3* v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

/// Euclidean length of a 3D vector.
inline FLOAT D3DXVec3Length(CONST D3DXVECTOR3* v)
{
    return std::sqrt(D3DXVec3LengthSq(v));
}

/// Writes `v / |v|` to `out` and returns `out`; a zero vector gives zero,
/// as in `D3DXVec2Normalize`.
inline D3DXVECTOR3* D3DXVec3Normalize(D3DXVECTOR3* out, CONST D3DXVECTOR3* v)
{
    const float len = D3DXVec3Length(v);
    if (len == 0.0f) { out->x = out->y = out->z = 0.0f; return out; }
    out->x = v->x / len;
    out->y = v->y / len;
    out->z = v->z / len;
    return out;
}

/// `out = v * s`; returns `out`.
inline D3DXVECTOR3* D3DXVec3Scale(D3DXVECTOR3* out, CONST D3DXVECTOR3* v, FLOAT s)
{
    out->x = v->x * s;
    out->y = v->y * s;
    out->z = v->z * s;
    return out;
}

/// Linear interpolation `out = v1 + s * (v2 - v1)`; `s` is not clamped.
inline D3DXVECTOR3* D3DXVec3Lerp(D3DXVECTOR3* out, CONST D3DXVECTOR3* v1, CONST D3DXVECTOR3* v2, FLOAT s)
{
    out->x = v1->x + s * (v2->x - v1->x);
    out->y = v1->y + s * (v2->y - v1->y);
    out->z = v1->z + s * (v2->z - v1->z);
    return out;
}

/// Full transform: `(v, 1) * M`, WITHOUT dividing by w.
inline D3DXVECTOR4* D3DXVec3Transform(D3DXVECTOR4* out, CONST D3DXVECTOR3* v, CONST D3DXMATRIX* mtx)
{
    const D3DXVECTOR4 r(
        v->x * mtx->_11 + v->y * mtx->_21 + v->z * mtx->_31 + mtx->_41,
        v->x * mtx->_12 + v->y * mtx->_22 + v->z * mtx->_32 + mtx->_42,
        v->x * mtx->_13 + v->y * mtx->_23 + v->z * mtx->_33 + mtx->_43,
        v->x * mtx->_14 + v->y * mtx->_24 + v->z * mtx->_34 + mtx->_44);
    *out = r;
    return out;
}

/// Point: `(v, 1) * M`, divided by w.
inline D3DXVECTOR3* D3DXVec3TransformCoord(D3DXVECTOR3* out, CONST D3DXVECTOR3* v, CONST D3DXMATRIX* mtx)
{
    D3DXVECTOR4 t;
    D3DXVec3Transform(&t, v, mtx);
    const float w = (t.w != 0.0f) ? t.w : 1.0f;
    out->x = t.x / w;
    out->y = t.y / w;
    out->z = t.z / w;
    return out;
}

/// Direction: `(v, 0) * M` - the translation does NOT apply.
inline D3DXVECTOR3* D3DXVec3TransformNormal(D3DXVECTOR3* out, CONST D3DXVECTOR3* v, CONST D3DXMATRIX* mtx)
{
    const D3DXVECTOR3 r(
        v->x * mtx->_11 + v->y * mtx->_21 + v->z * mtx->_31,
        v->x * mtx->_12 + v->y * mtx->_22 + v->z * mtx->_32,
        v->x * mtx->_13 + v->y * mtx->_23 + v->z * mtx->_33);
    *out = r;
    return out;
}

// ===========================================================================
// Matrices
// ===========================================================================

/// Sets `out` to the 4x4 identity matrix; returns `out`.
inline D3DXMATRIX* D3DXMatrixIdentity(D3DXMATRIX* out)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out->m[i][j] = (i == j) ? 1.0f : 0.0f;
    return out;
}

/// `out = m1 * m2`. In the row convention this means "first m1, then m2".
inline D3DXMATRIX* D3DXMatrixMultiply(D3DXMATRIX* out, CONST D3DXMATRIX* m1, CONST D3DXMATRIX* m2)
{
    const D3DXMATRIX r = (*m1) * (*m2);
    *out = r;  // temporary, because `out` is sometimes the same object as `m1` or `m2`
    return out;
}

/// `out = transpose(mtx)`; `out` may alias `mtx`.
inline D3DXMATRIX* D3DXMatrixTranspose(D3DXMATRIX* out, CONST D3DXMATRIX* mtx)
{
    D3DXMATRIX r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = mtx->m[j][i];
    *out = r;
    return out;
}

/// Translation matrix: identity with `(x, y, z)` in the fourth row.
inline D3DXMATRIX* D3DXMatrixTranslation(D3DXMATRIX* out, FLOAT x, FLOAT y, FLOAT z)
{
    D3DXMatrixIdentity(out);
    out->_41 = x;  // fourth ROW - the row-vector convention
    out->_42 = y;
    out->_43 = z;
    return out;
}

/// Scaling matrix: identity with `(sx, sy, sz)` on the diagonal.
inline D3DXMATRIX* D3DXMatrixScaling(D3DXMATRIX* out, FLOAT sx, FLOAT sy, FLOAT sz)
{
    D3DXMatrixIdentity(out);
    out->_11 = sx;
    out->_22 = sy;
    out->_33 = sz;
    return out;
}

/// Rotation by `angle` radians about the X axis (row-vector form).
inline D3DXMATRIX* D3DXMatrixRotationX(D3DXMATRIX* out, FLOAT angle)
{
    const float s = std::sin(angle), c = std::cos(angle);
    D3DXMatrixIdentity(out);
    out->_22 =  c; out->_23 = s;
    out->_32 = -s; out->_33 = c;
    return out;
}

/// Rotation by `angle` radians about the Y axis (row-vector form).
inline D3DXMATRIX* D3DXMatrixRotationY(D3DXMATRIX* out, FLOAT angle)
{
    const float s = std::sin(angle), c = std::cos(angle);
    D3DXMatrixIdentity(out);
    out->_11 = c; out->_13 = -s;
    out->_31 = s; out->_33 =  c;
    return out;
}

/// Rotation by `angle` radians about the Z axis (row-vector form).
inline D3DXMATRIX* D3DXMatrixRotationZ(D3DXMATRIX* out, FLOAT angle)
{
    const float s = std::sin(angle), c = std::cos(angle);
    D3DXMatrixIdentity(out);
    out->_11 =  c; out->_12 = s;
    out->_21 = -s; out->_22 = c;
    return out;
}

/// Quaternion -> rotation matrix in the ROW convention (i.e. the transpose
/// of the textbook rotation matrix, which assumes a column vector).
inline D3DXMATRIX* D3DXMatrixRotationQuaternion(D3DXMATRIX* out, CONST D3DXQUATERNION* q)
{
    const float x = q->x, y = q->y, z = q->z, w = q->w;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    D3DXMatrixIdentity(out);
    out->_11 = 1.0f - 2.0f * (yy + zz);
    out->_12 =        2.0f * (xy + wz);
    out->_13 =        2.0f * (xz - wy);

    out->_21 =        2.0f * (xy - wz);
    out->_22 = 1.0f - 2.0f * (xx + zz);
    out->_23 =        2.0f * (yz + wx);

    out->_31 =        2.0f * (xz + wy);
    out->_32 =        2.0f * (yz - wx);
    out->_33 = 1.0f - 2.0f * (xx + yy);
    return out;
}

/// Forward declaration - defined below with the quaternions.
inline D3DXQUATERNION* D3DXQuaternionRotationYawPitchRoll(D3DXQUATERNION* out, FLOAT yaw, FLOAT pitch, FLOAT roll);

/// Rotation matrix from yaw/pitch/roll, built through the quaternion of
/// `D3DXQuaternionRotationYawPitchRoll` so the two always agree.
inline D3DXMATRIX* D3DXMatrixRotationYawPitchRoll(D3DXMATRIX* out, FLOAT yaw, FLOAT pitch, FLOAT roll)
{
    D3DXQUATERNION q;
    D3DXQuaternionRotationYawPitchRoll(&q, yaw, pitch, roll);
    return D3DXMatrixRotationQuaternion(out, &q);
}

/// Right-handed view matrix. `zaxis` points FROM the target TO the eye -
/// hence `eye - at`.
inline D3DXMATRIX* D3DXMatrixLookAtRH(D3DXMATRIX* out, CONST D3DXVECTOR3* eye,
                                      CONST D3DXVECTOR3* at, CONST D3DXVECTOR3* up)
{
    D3DXVECTOR3 zaxis, xaxis, yaxis, diff;
    D3DXVec3Subtract(&diff, eye, at);
    D3DXVec3Normalize(&zaxis, &diff);
    D3DXVec3Cross(&xaxis, up, &zaxis);
    D3DXVec3Normalize(&xaxis, &xaxis);
    D3DXVec3Cross(&yaxis, &zaxis, &xaxis);

    D3DXMatrixIdentity(out);
    out->_11 = xaxis.x; out->_12 = yaxis.x; out->_13 = zaxis.x;
    out->_21 = xaxis.y; out->_22 = yaxis.y; out->_23 = zaxis.y;
    out->_31 = xaxis.z; out->_32 = yaxis.z; out->_33 = zaxis.z;
    out->_41 = -D3DXVec3Dot(&xaxis, eye);
    out->_42 = -D3DXVec3Dot(&yaxis, eye);
    out->_43 = -D3DXVec3Dot(&zaxis, eye);
    return out;
}

/// Right-handed orthographic projection. The depth range is [0,1]
/// (Direct3D), NOT [-1,1] (OpenGL) - a difference that the move to WebGL
/// has to account for in ONE place, and not here.
inline D3DXMATRIX* D3DXMatrixOrthoRH(D3DXMATRIX* out, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf)
{
    D3DXMatrixIdentity(out);
    out->_11 = 2.0f / w;
    out->_22 = 2.0f / h;
    out->_33 = 1.0f / (zn - zf);
    out->_43 = zn / (zn - zf);
    return out;
}

// ===========================================================================
// Quaternions
// ===========================================================================

/// MIND THE ORDER. D3DX documents this as `out = q2 * q1` in mathematical
/// notation, which means "first rotation q1, then q2". That way the argument
/// order is THE SAME as for `D3DXMatrixMultiply`.
///
/// This is not a guess - `d3dx8_math_test.cpp` checks that both paths AGREE:
/// `R(QuaternionMultiply(q1,q2))` must equal `MatrixMultiply(R(q1),R(q2))`.
/// The reverse order fails that test.
inline D3DXQUATERNION* D3DXQuaternionMultiply(D3DXQUATERNION* out,
                                              CONST D3DXQUATERNION* q1,
                                              CONST D3DXQUATERNION* q2)
{
    const D3DXQUATERNION r(
        q2->w * q1->x + q2->x * q1->w + q2->y * q1->z - q2->z * q1->y,
        q2->w * q1->y - q2->x * q1->z + q2->y * q1->w + q2->z * q1->x,
        q2->w * q1->z + q2->x * q1->y - q2->y * q1->x + q2->z * q1->w,
        q2->w * q1->w - q2->x * q1->x - q2->y * q1->y - q2->z * q1->z);
    *out = r;
    return out;
}

/// Sets `out` to the identity quaternion `(0, 0, 0, 1)`; returns `out`.
inline D3DXQUATERNION* D3DXQuaternionIdentity(D3DXQUATERNION* out)
{
    out->x = out->y = out->z = 0.0f;
    out->w = 1.0f;
    return out;
}

/// Length of the quaternion as a four-component vector.
inline FLOAT D3DXQuaternionLength(CONST D3DXQUATERNION* q)
{
    return std::sqrt(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
}

/// Writes `q / |q|` to `out`; a zero quaternion gives the IDENTITY (not
/// zero, unlike the vector normalisers).
inline D3DXQUATERNION* D3DXQuaternionNormalize(D3DXQUATERNION* out, CONST D3DXQUATERNION* q)
{
    const float len = D3DXQuaternionLength(q);
    if (len == 0.0f) return D3DXQuaternionIdentity(out);
    out->x = q->x / len; out->y = q->y / len;
    out->z = q->z / len; out->w = q->w / len;
    return out;
}

/// The axis does NOT have to be normalised - D3DX normalises it itself.
inline D3DXQUATERNION* D3DXQuaternionRotationAxis(D3DXQUATERNION* out,
                                                  CONST D3DXVECTOR3* axis, FLOAT angle)
{
    D3DXVECTOR3 n;
    D3DXVec3Normalize(&n, axis);
    const float s = std::sin(angle * 0.5f);
    out->x = n.x * s;
    out->y = n.y * s;
    out->z = n.z * s;
    out->w = std::cos(angle * 0.5f);
    return out;
}

/// Quaternion from yaw (about Y), pitch (about X) and roll (about Z), in
/// radians, composed in the order roll -> pitch -> yaw.
inline D3DXQUATERNION* D3DXQuaternionRotationYawPitchRoll(D3DXQUATERNION* out,
                                                          FLOAT yaw, FLOAT pitch, FLOAT roll)
{
    // Yaw about Y, pitch about X, roll about Z; composed in the order
    // roll -> pitch -> yaw.
    const float sy = std::sin(yaw   * 0.5f), cy = std::cos(yaw   * 0.5f);
    const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
    const float sr = std::sin(roll  * 0.5f), cr = std::cos(roll  * 0.5f);

    out->x = cy * sp * cr + sy * cp * sr;
    out->y = sy * cp * cr - cy * sp * sr;
    out->z = cy * cp * sr - sy * sp * cr;
    out->w = cy * cp * cr + sy * sp * sr;
    return out;
}

// ===========================================================================
// Planes
// ===========================================================================

/// Product with a POINT: `a*x + b*y + c*z + d`. The sign says which side of
/// the plane the point lies on - view culling in `GameLib` rests on this.
inline FLOAT D3DXPlaneDotCoord(CONST D3DXPLANE* p, CONST D3DXVECTOR3* v)
{
    return p->a * v->x + p->b * v->y + p->c * v->z + p->d;
}

/// Product with a DIRECTION: without the constant term.
inline FLOAT D3DXPlaneDotNormal(CONST D3DXPLANE* p, CONST D3DXVECTOR3* v)
{
    return p->a * v->x + p->b * v->y + p->c * v->z;
}

/// Scales the WHOLE quadruple so that `(a,b,c)` has length 1. Normalising
/// `(a,b,c)` alone, without `d`, would move the plane - a typical mistake.
inline D3DXPLANE* D3DXPlaneNormalize(D3DXPLANE* out, CONST D3DXPLANE* p)
{
    const float len = std::sqrt(p->a * p->a + p->b * p->b + p->c * p->c);
    if (len == 0.0f) { *out = *p; return out; }
    out->a = p->a / len;
    out->b = p->b / len;
    out->c = p->c / len;
    out->d = p->d / len;
    return out;
}

// ===========================================================================
// D3DX names that appear in the EterLib headers as HANDLES
// ===========================================================================
// D3DX buffer and mesh. Like the Direct3D resources - opaque here, because
// in the headers they only appear as pointers passed along. Their full
// definitions are at the end of the file.
struct ID3DXBuffer;
struct ID3DXMesh;
typedef ID3DXBuffer* LPD3DXBUFFER;
typedef ID3DXMesh*   LPD3DXMESH;

// Filter used when resizing a texture.
#define D3DX_FILTER_NONE     0x00000001
#define D3DX_FILTER_POINT    0x00000002
#define D3DX_FILTER_LINEAR   0x00000003
#define D3DX_FILTER_TRIANGLE 0x00000004
#define D3DX_FILTER_BOX      0x00000005

/// Forward declaration of the D3DX matrix stack; the interface is defined
/// below and implemented in `d3dx8_matrixstack.cpp`.
struct ID3DXMatrixStack;
typedef ID3DXMatrixStack* LPD3DXMATRIXSTACK;

// ===========================================================================
// Third batch - from measuring the EterLib sources
// ===========================================================================

/// Quaternion conjugate: a rotation by the same angle the OTHER way. For a
/// unit quaternion it is also the inverse.
inline D3DXQUATERNION* D3DXQuaternionConjugate(D3DXQUATERNION* out, CONST D3DXQUATERNION* q)
{
    out->x = -q->x;
    out->y = -q->y;
    out->z = -q->z;
    out->w =  q->w;
    return out;
}

/// Determinant of a 4x4 matrix, expanded along the first row.
inline FLOAT D3DXMatrixDeterminant(CONST D3DXMATRIX* mtx)
{
    const float (*m)[4] = mtx->m;

    // 2x2 determinants of the two bottom rows - each used twice, so
    // computed once.
    const float s0 = m[2][0] * m[3][1] - m[2][1] * m[3][0];
    const float s1 = m[2][0] * m[3][2] - m[2][2] * m[3][0];
    const float s2 = m[2][0] * m[3][3] - m[2][3] * m[3][0];
    const float s3 = m[2][1] * m[3][2] - m[2][2] * m[3][1];
    const float s4 = m[2][1] * m[3][3] - m[2][3] * m[3][1];
    const float s5 = m[2][2] * m[3][3] - m[2][3] * m[3][2];

    return m[0][0] * (m[1][1] * s5 - m[1][2] * s4 + m[1][3] * s3)
         - m[0][1] * (m[1][0] * s5 - m[1][2] * s2 + m[1][3] * s1)
         + m[0][2] * (m[1][0] * s4 - m[1][1] * s2 + m[1][3] * s0)
         - m[0][3] * (m[1][0] * s3 - m[1][1] * s1 + m[1][2] * s0);
}

/// `EterLib` calls this name with a lower-case `f` - that is how it is in
/// the D3DX8 headers.
inline FLOAT D3DXMatrixfDeterminant(CONST D3DXMATRIX* mtx)
{
    return D3DXMatrixDeterminant(mtx);
}

/// Orthographic projection given by its edges rather than width and height -
/// allows a view volume that is NOT symmetric about the view axis. Depth
/// lands in [0,1], as in Direct3D.
inline D3DXMATRIX* D3DXMatrixOrthoOffCenterRH(D3DXMATRIX* out, FLOAT l, FLOAT r,
                                              FLOAT b, FLOAT t, FLOAT zn, FLOAT zf)
{
    D3DXMatrixIdentity(out);
    out->_11 = 2.0f / (r - l);
    out->_22 = 2.0f / (t - b);
    out->_33 = 1.0f / (zn - zf);
    out->_41 = (l + r) / (l - r);
    out->_42 = (t + b) / (b - t);
    out->_43 = zn / (zn - zf);
    return out;
}

/// Vertex size in bytes from its `FVF` description.
///
/// The number of texture coordinate sets sits in bits 8-11, and the size of
/// EACH set - in the bits from 16 upwards, two per set. The default `00`
/// means TWO numbers (8 bytes), not zero - this is where it is easy to count
/// half as much as needed.
inline UINT D3DXGetFVFVertexSize(DWORD fvf)
{
    UINT size = 0;

    if (fvf & D3DFVF_XYZRHW)   size += 4 * sizeof(float);
    else if (fvf & D3DFVF_XYZ) size += 3 * sizeof(float);

    if (fvf & D3DFVF_NORMAL)   size += 3 * sizeof(float);
    if (fvf & D3DFVF_PSIZE)    size += sizeof(float);
    if (fvf & D3DFVF_DIFFUSE)  size += sizeof(DWORD);
    if (fvf & D3DFVF_SPECULAR) size += sizeof(DWORD);

    const UINT texSets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (UINT i = 0; i < texSets; ++i) {
        switch ((fvf >> (16 + i * 2)) & 0x3) {
            case D3DFVF_TEXTUREFORMAT1: size += 1 * sizeof(float); break;
            case D3DFVF_TEXTUREFORMAT2: size += 2 * sizeof(float); break;
            case D3DFVF_TEXTUREFORMAT3: size += 3 * sizeof(float); break;
            case D3DFVF_TEXTUREFORMAT4: size += 4 * sizeof(float); break;
        }
    }
    return size;
}

/// Right-handed perspective projection. `fovY` is the **whole** vertical
/// angle, not half of it - hence `1 / tan(fovY / 2)`. Depth lands in [0,1]
/// (Direct3D), not [-1,1] (OpenGL); the conversion for WebGL is done in one
/// place in the graphics layer, not here.
inline D3DXMATRIX* D3DXMatrixPerspectiveFovRH(D3DXMATRIX* out, FLOAT fovY, FLOAT aspect,
                                              FLOAT zn, FLOAT zf)
{
    const float h = 1.0f / std::tan(fovY * 0.5f);
    const float w = h / aspect;

    for (int i = 0; i < 16; ++i) (&out->_11)[i] = 0.0f;
    out->_11 = w;
    out->_22 = h;
    out->_33 = zf / (zn - zf);
    out->_34 = -1.0f;            // carries -z into w, hence the perspective divide
    out->_43 = zn * zf / (zn - zf);
    return out;
}

/// Matrix inverse by cofactors.
///
/// `pDeterminant` may be `NULL` - as in D3DX. For a singular matrix it
/// returns `NULL` and **does not touch** `out`; TMP4 code checks the result,
/// so silently returning garbage would be worse than having no function.
inline D3DXMATRIX* D3DXMatrixInverse(D3DXMATRIX* out, FLOAT* pDeterminant,
                                     CONST D3DXMATRIX* mtx)
{
    const float* m = &mtx->_11;
    float inv[16];

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (pDeterminant) *pDeterminant = det;
    if (det == 0.0f) return NULL;

    const float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) (&out->_11)[i] = inv[i] * invDet;
    return out;
}

/// Rotation matrix about an arbitrary axis. Built through a quaternion so
/// that it is **by definition consistent** with `D3DXQuaternionRotationAxis`
/// - computing it separately with Rodrigues' formula would let a convention
/// mismatch show up only in the game.
inline D3DXMATRIX* D3DXMatrixRotationAxis(D3DXMATRIX* out, CONST D3DXVECTOR3* axis, FLOAT angle)
{
    D3DXQUATERNION q;
    D3DXQuaternionRotationAxis(&q, axis, angle);
    return D3DXMatrixRotationQuaternion(out, &q);
}

/// Full transform of a four-component vector: `v * M`, without the divide.
/// `EterGrnLib` passes vertices with bone weights this way.
inline D3DXVECTOR4* D3DXVec4Transform(D3DXVECTOR4* out, CONST D3DXVECTOR4* v,
                                      CONST D3DXMATRIX* mtx)
{
    const D3DXVECTOR4 r(
        v->x * mtx->_11 + v->y * mtx->_21 + v->z * mtx->_31 + v->w * mtx->_41,
        v->x * mtx->_12 + v->y * mtx->_22 + v->z * mtx->_32 + v->w * mtx->_42,
        v->x * mtx->_13 + v->y * mtx->_23 + v->z * mtx->_33 + v->w * mtx->_43,
        v->x * mtx->_14 + v->y * mtx->_24 + v->z * mtx->_34 + v->w * mtx->_44);
    *out = r;
    return out;
}

// ===========================================================================
// Fourth batch
// ===========================================================================

#define D3DX_DEFAULT ((UINT)-1)

/// Euclidean length of a 2D vector.
inline FLOAT D3DXVec2Length(CONST D3DXVECTOR2* v)
{
    return std::sqrt(v->x * v->x + v->y * v->y);
}

/// Cross product in the plane: positive when v2 lies to the left of v1.
/// The name comes from "counter-clockwise" - the sign tells the winding.
inline FLOAT D3DXVec2CCW(CONST D3DXVECTOR2* v1, CONST D3DXVECTOR2* v2)
{
    return v1->x * v2->y - v1->y * v2->x;
}

/// Projects a point from the world to the screen. Mind y: on the screen it
/// grows DOWNWARDS, hence the subtraction instead of addition.
inline D3DXVECTOR3* D3DXVec3Project(D3DXVECTOR3* out, CONST D3DXVECTOR3* v,
                                    CONST D3DVIEWPORT8* viewport,
                                    CONST D3DXMATRIX* projection,
                                    CONST D3DXMATRIX* view,
                                    CONST D3DXMATRIX* world)
{
    D3DXMATRIX m;
    D3DXMatrixIdentity(&m);
    if (world)      D3DXMatrixMultiply(&m, &m, world);
    if (view)       D3DXMatrixMultiply(&m, &m, view);
    if (projection) D3DXMatrixMultiply(&m, &m, projection);

    D3DXVECTOR3 p;
    D3DXVec3TransformCoord(&p, v, &m);

    out->x = viewport->X + (1.0f + p.x) * 0.5f * viewport->Width;
    out->y = viewport->Y + (1.0f - p.y) * 0.5f * viewport->Height;
    out->z = viewport->MinZ + p.z * (viewport->MaxZ - viewport->MinZ);
    return out;
}

/// The reverse path. For a singular matrix it leaves the result UNTOUCHED -
/// just like D3DXMatrixInverse.
inline D3DXVECTOR3* D3DXVec3Unproject(D3DXVECTOR3* out, CONST D3DXVECTOR3* v,
                                      CONST D3DVIEWPORT8* viewport,
                                      CONST D3DXMATRIX* projection,
                                      CONST D3DXMATRIX* view,
                                      CONST D3DXMATRIX* world)
{
    D3DXMATRIX m, inverse;
    D3DXMatrixIdentity(&m);
    if (world)      D3DXMatrixMultiply(&m, &m, world);
    if (view)       D3DXMatrixMultiply(&m, &m, view);
    if (projection) D3DXMatrixMultiply(&m, &m, projection);
    if (!D3DXMatrixInverse(&inverse, NULL, &m)) return out;

    D3DXVECTOR3 p;
    p.x = 2.0f * (v->x - viewport->X) / viewport->Width - 1.0f;
    p.y = 1.0f - 2.0f * (v->y - viewport->Y) / viewport->Height;
    p.z = (v->z - viewport->MinZ) / (viewport->MaxZ - viewport->MinZ);

    D3DXVec3TransformCoord(out, &p, &inverse);
    return out;
}

// ---------------------------------------------------------------------------
// This is NO LONGER maths - the D3DX resource functions
// ---------------------------------------------------------------------------
// Loading textures, building meshes and assembling shaders belong to the
// port's graphics layer, not to the compatibility layer. They are declared
// here so that EterLib parses; the bodies that exist live in
// `d3dx8_textures.cpp`, `d3dx8_shader.cpp` and `d3dx8_matrixstack.cpp`.
// What has NO body is left undefined on purpose, so that the linker stops
// loudly at the place where a decision has to be made.
//
// The same rule as for threads in process.h. A stub returning
// D3D_OK and an empty handle would be the worst option here: the game would
// draw into a non-existent texture and not know it.

/// D3DX matrix stack: push/pop plus the D3DX transform operations on the
/// top matrix. Implemented in `d3dx8_matrixstack.cpp`; the destructor is
/// protected, so it is released only through `Release`.
struct ID3DXMatrixStack
{
    /// Adds a reference; returns the new count.
    virtual ULONG   AddRef() = 0;
    /// Drops a reference, deleting the stack at zero; returns the new count.
    virtual ULONG   Release() = 0;
    /// Duplicates the top matrix onto the stack.
    virtual HRESULT Push() = 0;
    /// Removes the top matrix (in the port the bottom one is never removed).
    virtual HRESULT Pop() = 0;
    /// Replaces the top with the identity.
    virtual HRESULT LoadIdentity() = 0;
    /// Replaces the top with `*pM`.
    virtual HRESULT LoadMatrix(CONST D3DXMATRIX* pM) = 0;
    /// `top = top * M` - M acts AFTER the current transform (world frame).
    virtual HRESULT MultMatrix(CONST D3DXMATRIX* pM) = 0;
    /// `top = M * top` - M acts BEFORE the current transform (the object's own
    /// frame).
    virtual HRESULT MultMatrixLocal(CONST D3DXMATRIX* pM) = 0;
    /// `MultMatrix` with a rotation by `angle` radians about axis `pV`.
    virtual HRESULT RotateAxis(CONST D3DXVECTOR3* pV, FLOAT angle) = 0;
    /// `MultMatrixLocal` with a rotation by `angle` radians about axis `pV`.
    virtual HRESULT RotateAxisLocal(CONST D3DXVECTOR3* pV, FLOAT angle) = 0;
    /// `MultMatrix` with a yaw/pitch/roll rotation.
    virtual HRESULT RotateYawPitchRoll(FLOAT yaw, FLOAT pitch, FLOAT roll) = 0;
    /// `MultMatrixLocal` with a yaw/pitch/roll rotation.
    virtual HRESULT RotateYawPitchRollLocal(FLOAT yaw, FLOAT pitch, FLOAT roll) = 0;
    /// `MultMatrixLocal` with a scaling.
    virtual HRESULT ScaleLocal(FLOAT x, FLOAT y, FLOAT z) = 0;
    /// `MultMatrixLocal` with a translation.
    virtual HRESULT TranslateLocal(FLOAT x, FLOAT y, FLOAT z) = 0;
    /// `MultMatrix` with a scaling.
    virtual HRESULT Scale(FLOAT x, FLOAT y, FLOAT z) = 0;
    /// `MultMatrix` with a translation.
    virtual HRESULT Translate(FLOAT x, FLOAT y, FLOAT z) = 0;
    /// The top matrix (writable; valid until the next Push/Pop).
    virtual D3DXMATRIX* GetTop() = 0;
protected:
    /// Protected: the stack goes away through `Release`.
    ~ID3DXMatrixStack() {}
};

typedef struct _D3DXIMAGE_INFO {
    UINT      Width;
    UINT      Height;
    UINT      Depth;
    UINT      MipLevels;
    D3DFORMAT Format;
    DWORD     ResourceType;
    DWORD     ImageFileFormat;
} D3DXIMAGE_INFO;

HRESULT D3DXCreateMatrixStack(DWORD flags, LPD3DXMATRIXSTACK* ppStack);

HRESULT D3DXCreateTexture(IDirect3DDevice8* pDevice, UINT width, UINT height,
                          UINT mipLevels, DWORD usage, D3DFORMAT format,
                          D3DPOOL pool, LPDIRECT3DTEXTURE8* ppTexture);

HRESULT D3DXCreateTextureFromFileInMemoryEx(
    IDirect3DDevice8* pDevice, LPCVOID pSrcData, UINT srcDataSize,
    UINT width, UINT height, UINT mipLevels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, DWORD filter, DWORD mipFilter, D3DCOLOR colorKey,
    D3DXIMAGE_INFO* pSrcInfo, void* pPalette, LPDIRECT3DTEXTURE8* ppTexture);

HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface8* pDestSurface,
                                   CONST void* pDestPalette, CONST RECT* pDestRect,
                                   IDirect3DSurface8* pSrcSurface,
                                   CONST void* pSrcPalette, CONST RECT* pSrcRect,
                                   DWORD filter, D3DCOLOR colorKey);

HRESULT D3DXAssembleShaderFromFileA(LPCSTR filename, DWORD flags,
                                    LPD3DXBUFFER* ppConstants,
                                    LPD3DXBUFFER* ppCompiledShader,
                                    LPD3DXBUFFER* ppCompilationErrors);
#ifndef D3DXAssembleShaderFromFile
#define D3DXAssembleShaderFromFile D3DXAssembleShaderFromFileA
#endif

/// The same assembler, but from MEMORY, not a file. SpeedTree uses it:
/// its shaders are pasted into `VertexShaders.h` as strings in Direct3D 8
/// assembly. The answer is the same refusal, for the same reason - see
/// `compat/d3dx8_shader.cpp`.
HRESULT D3DXAssembleShader(LPCSTR srcData, UINT srcDataLen, DWORD flags,
                           LPD3DXBUFFER* ppConstants,
                           LPD3DXBUFFER* ppCompiledShader,
                           LPD3DXBUFFER* ppCompilationErrors);

/// Declared only - no body. Its only caller, the original
/// `eterLib/GrpDevice.cpp`, is replaced by `grpdevice_gl.cpp`, which does
/// not create the helper meshes.
HRESULT D3DXCreateSphere(IDirect3DDevice8* pDevice, FLOAT radius, UINT slices,
                         UINT stacks, LPD3DXMESH* ppMesh, LPD3DXBUFFER* ppAdjacency);
/// Declared only - no body; same reason as `D3DXCreateSphere`.
HRESULT D3DXCreateCylinder(IDirect3DDevice8* pDevice, FLOAT radius1, FLOAT radius2,
                           FLOAT length, UINT slices, UINT stacks,
                           LPD3DXMESH* ppMesh, LPD3DXBUFFER* ppAdjacency);

/// D3DX mesh. Five methods - as many as `CScreen::RenderD3DXMesh` calls.
/// `CGraphicBase` holds two such meshes (sphere and cylinder) for helper
/// drawing, so the type has to be complete.
struct ID3DXMesh
{
    /// Adds a reference; returns the new count.
    virtual ULONG   AddRef() = 0;
    /// Drops a reference; returns the new count.
    virtual ULONG   Release() = 0;
    /// Number of triangles.
    virtual DWORD   GetNumFaces() = 0;
    /// Number of vertices.
    virtual DWORD   GetNumVertices() = 0;
    /// The vertex format (`D3DFVF_*` bits).
    virtual DWORD   GetFVF() = 0;
    /// The mesh's vertex buffer (a new reference).
    virtual HRESULT GetVertexBuffer(LPDIRECT3DVERTEXBUFFER8* ppVB) = 0;
    /// The mesh's index buffer (a new reference).
    virtual HRESULT GetIndexBuffer(LPDIRECT3DINDEXBUFFER8* ppIB) = 0;
protected:
    /// Protected: the mesh goes away through `Release`. No class of the port
    /// implements `ID3DXMesh` (see `D3DXCreateSphere`).
    ~ID3DXMesh() {}
};

/// D3DX buffer - a wrapper around a piece of memory returned by the shader
/// assembler and by mesh creation.
struct ID3DXBuffer
{
    /// Adds a reference; returns the new count.
    virtual ULONG AddRef() = 0;
    /// Drops a reference, deleting the buffer at zero; returns the new count.
    virtual ULONG Release() = 0;
    /// The buffer's bytes.
    virtual void* GetBufferPointer() = 0;
    /// The buffer's size in bytes.
    virtual DWORD GetBufferSize() = 0;
protected:
    /// Protected: the buffer goes away through `Release`.
    ~ID3DXBuffer() {}
};

/// Per-component colour product - channel by channel, alpha included.
/// Used when dimming interface elements.
inline D3DXCOLOR* D3DXColorModulate(D3DXCOLOR* out, CONST D3DXCOLOR* c1,
                                    CONST D3DXCOLOR* c2)
{
    out->r = c1->r * c2->r;
    out->g = c1->g * c2->g;
    out->b = c1->b * c2->b;
    out->a = c1->a * c2->a;
    return out;
}
