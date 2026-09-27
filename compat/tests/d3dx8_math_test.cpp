// d3dx8_math_test.cpp - test of the PROPERTIES of the D3DX8 math layer.
//
// WHY: I wrote `d3dx8.h` from memory of the conventions, not from Microsoft's original.
// The matrix convention is exactly the kind of thing that **compiles,
// runs and gives a wrong picture** - a transpose is not an error, just a silent
// twisting of the scene. So every function has to pass a test that cannot
// be fooled.
//
// The tests are of three kinds:
//   1. REFERENCE VALUES computed by hand (a 90-degree rotation, etc.),
//   2. PROPERTIES (composition, inversion, agreement of two routes),
//   3. OPPOSITE CONTROLS - a check that the test detects anything at all.
//
// Running:
//   em++ -std=c++17 -O1 -Icompat compat/tests/d3dx8_math_test.cpp compat/win32_compat.cpp \
//        -o d3dx8_math_test.js && node d3dx8_math_test.js

#include <cstdio>
#include <cmath>

#include "d3dx8.h"

namespace {

int failures = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-58s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++failures;
}

/// Floats equal within `eps`.
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

/// Vector `v` equal to (x, y, z) within `eps`.
bool NearVec(const D3DXVECTOR3& v, float x, float y, float z, float eps = 1e-4f)
{
    return Near(v.x, x, eps) && Near(v.y, y, eps) && Near(v.z, z, eps);
}

/// Matrices equal element by element within `eps`.
bool NearMat(const D3DXMATRIX& a, const D3DXMATRIX& b, float eps = 1e-4f)
{
    for (int i = 0; i < 16; ++i) {
        if (!Near((&a._11)[i], (&b._11)[i], eps)) return false;
    }
    return true;
}

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("=== D3DX8: the math layer ===\n\n");
    char buf[200];

    // --- 1. MEMORY LAYOUT -----------------------------------------------------
    // This is the assumption everything else stands on: `_41` must lie at
    // index 12, because that is where OpenGL keeps the X translation. If the field layout were
    // different, a D3DX matrix would have to be transposed before being given to GL.
    {
        Check("D3DXMATRIX has 64 bytes", sizeof(D3DXMATRIX) == 64);
        Check("D3DXVECTOR3 has 12 bytes (no padding)", sizeof(D3DXVECTOR3) == 12);

        D3DXMATRIX m;
        for (int i = 0; i < 16; ++i) (&m._11)[i] = static_cast<float>(i);
        std::snprintf(buf, sizeof(buf), "(_41=%.0f, expected 12)", m._41);
        Check("_41 lies at index 12 - like the X translation in OpenGL",
              m._41 == 12.0f, buf);
        Check("_44 lies at index 15", m._44 == 15.0f);
        Check("m[1][2] is the same as _23", m.m[1][2] == m._23);
    }

    // --- 2. THE TRANSLATION IS IN THE FOURTH ROW -------------------------------
    // Measured: `GameLib` reads `_41 _42 _43` directly, 107 times in total.
    // Putting the translation in the fourth COLUMN would break that code silently.
    {
        D3DXMATRIX t;
        D3DXMatrixTranslation(&t, 10.0f, 20.0f, 30.0f);
        Check("D3DXMatrixTranslation writes to _41.._43",
              t._41 == 10.0f && t._42 == 20.0f && t._43 == 30.0f);
        Check("and does NOT write to _14.._34 (opposite control)",
              t._14 == 0.0f && t._24 == 0.0f && t._34 == 0.0f);

        D3DXVECTOR3 p(1.0f, 2.0f, 3.0f), out;
        D3DXVec3TransformCoord(&out, &p, &t);
        Check("TransformCoord adds the translation to a POINT",
              NearVec(out, 11.0f, 22.0f, 33.0f));

        D3DXVec3TransformNormal(&out, &p, &t);
        Check("TransformNormal IGNORES the translation (a direction)",
              NearVec(out, 1.0f, 2.0f, 3.0f));
    }

    // --- 3. THE ORDER OF MATRIX COMPOSITION -------------------------------------
    // `Multiply(A, B)` in the row convention means "first A, then B".
    // The test is chosen so that both orders give DIFFERENT results - otherwise it
    // would check nothing.
    {
        D3DXMATRIX t, s, ts, st;
        D3DXMatrixTranslation(&t, 10.0f, 0.0f, 0.0f);
        D3DXMatrixScaling(&s, 2.0f, 2.0f, 2.0f);
        D3DXMatrixMultiply(&ts, &t, &s);
        D3DXMatrixMultiply(&st, &s, &t);

        D3DXVECTOR3 p(1.0f, 0.0f, 0.0f), a, b;
        D3DXVec3TransformCoord(&a, &p, &ts);
        D3DXVec3TransformCoord(&b, &p, &st);

        std::snprintf(buf, sizeof(buf), "(%.1f, expected 22.0)", a.x);
        Check("Multiply(T,S): first translation, then scale", Near(a.x, 22.0f), buf);
        std::snprintf(buf, sizeof(buf), "(%.1f, expected 12.0)", b.x);
        Check("Multiply(S,T): first scale, then translation", Near(b.x, 12.0f), buf);
        Check("both orders give DIFFERENT results (the test detects something)",
              a != b);
    }

    // --- 4. `out` may be the same object as the input --------------------------
    // TMP4 code writes `D3DXMatrixMultiply(&m, &m, &n)` and `D3DXVec3Cross(&v,&v,&w)`.
    // Without a temporary inside, the result would be partly overwritten.
    {
        D3DXMATRIX a, b, ref;
        D3DXMatrixTranslation(&a, 1.0f, 2.0f, 3.0f);
        D3DXMatrixScaling(&b, 2.0f, 3.0f, 4.0f);
        D3DXMatrixMultiply(&ref, &a, &b);
        D3DXMatrixMultiply(&a, &a, &b);  // result into the input
        Check("Multiply survives the input being overwritten by the result", NearMat(a, ref));

        D3DXVECTOR3 v(1.0f, 0.0f, 0.0f), w(0.0f, 1.0f, 0.0f), cref;
        D3DXVec3Cross(&cref, &v, &w);
        D3DXVec3Cross(&v, &v, &w);
        Check("Vec3Cross survives the input being overwritten by the result",
              NearVec(v, cref.x, cref.y, cref.z));
        Check("Vec3Cross(X, Y) = Z (right-handed system)", NearVec(cref, 0.0f, 0.0f, 1.0f));
    }

    // --- 5. ROTATIONS: reference values computed by hand ------------------------
    {
        D3DXMATRIX rz;
        D3DXMatrixRotationZ(&rz, D3DXToRadian(90.0f));
        D3DXVECTOR3 x(1.0f, 0.0f, 0.0f), out;
        D3DXVec3TransformCoord(&out, &x, &rz);
        std::snprintf(buf, sizeof(buf), "(%.3f %.3f %.3f)", out.x, out.y, out.z);
        Check("RotationZ(90): (1,0,0) -> (0,1,0)", NearVec(out, 0.0f, 1.0f, 0.0f), buf);

        D3DXMATRIX ry;
        D3DXMatrixRotationY(&ry, D3DXToRadian(90.0f));
        D3DXVec3TransformCoord(&out, &x, &ry);
        std::snprintf(buf, sizeof(buf), "(%.3f %.3f %.3f)", out.x, out.y, out.z);
        Check("RotationY(90): (1,0,0) -> (0,0,-1)", NearVec(out, 0.0f, 0.0f, -1.0f), buf);

        Check("D3DXToRadian(180) = PI", Near(D3DXToRadian(180.0f), D3DX_PI));
        Check("D3DXToDegree(PI) = 180", Near(D3DXToDegree(D3DX_PI), 180.0f));
    }

    // --- 6. QUATERNIONS: agreement of TWO ROUTES ---------------------------------
    // The most important test in this file. The order of quaternion multiplication in D3DX
    // is the reverse of the mathematical notation and easy to get wrong. I check it
    // not by a declaration, but by AGREEMENT: a composition of rotations computed
    // with quaternions must give the same matrix as the composition computed with matrices.
    {
        const D3DXVECTOR3 axisY(0.0f, 1.0f, 0.0f), axisX(1.0f, 0.0f, 0.0f);
        D3DXQUATERNION q1, q2, q12;
        D3DXQuaternionRotationAxis(&q1, &axisY, D3DXToRadian(35.0f));
        D3DXQuaternionRotationAxis(&q2, &axisX, D3DXToRadian(50.0f));
        D3DXQuaternionMultiply(&q12, &q1, &q2);

        D3DXMATRIX m1, m2, mQuat, mMat;
        D3DXMatrixRotationQuaternion(&m1, &q1);
        D3DXMatrixRotationQuaternion(&m2, &q2);
        D3DXMatrixRotationQuaternion(&mQuat, &q12);
        D3DXMatrixMultiply(&mMat, &m1, &m2);

        Check("R(QuatMul(q1,q2)) == MatMul(R(q1),R(q2)) - the order agrees",
              NearMat(mQuat, mMat));

        // OPPOSITE CONTROL: with the reverse order the test MUST fail,
        // otherwise it does not tell the two possibilities apart.
        D3DXQUATERNION q21;
        D3DXQuaternionMultiply(&q21, &q2, &q1);
        D3DXMATRIX mQuatRev;
        D3DXMatrixRotationQuaternion(&mQuatRev, &q21);
        Check("the reverse order gives a DIFFERENT matrix (the test tells them apart)",
              !NearMat(mQuatRev, mMat));

        // A quaternion about the Y axis must give exactly the same matrix as RotationY.
        D3DXMATRIX ry;
        D3DXMatrixRotationY(&ry, D3DXToRadian(35.0f));
        Check("R(Quat about Y) == D3DXMatrixRotationY", NearMat(m1, ry));

        // YawPitchRoll with yaw alone is a rotation about Y.
        D3DXQUATERNION qy;
        D3DXQuaternionRotationYawPitchRoll(&qy, D3DXToRadian(35.0f), 0.0f, 0.0f);
        D3DXMATRIX myp;
        D3DXMatrixRotationQuaternion(&myp, &qy);
        Check("YawPitchRoll(yaw,0,0) is a rotation about Y", NearMat(myp, ry));

        // MatrixRotationYawPitchRoll with roll alone is a rotation about Z.
        D3DXMATRIX mroll, rz;
        D3DXMatrixRotationYawPitchRoll(&mroll, 0.0f, 0.0f, D3DXToRadian(20.0f));
        D3DXMatrixRotationZ(&rz, D3DXToRadian(20.0f));
        Check("MatrixRotationYawPitchRoll(0,0,roll) is a rotation about Z",
              NearMat(mroll, rz));

        Check("a quaternion from RotationAxis has length 1",
              Near(D3DXQuaternionLength(&q1), 1.0f));
    }

    // --- 7. A ROTATION DOES NOT CHANGE LENGTH -------------------------------------
    // A property that catches every sign and scale error at once.
    {
        D3DXQUATERNION q;
        const D3DXVECTOR3 axis(0.3f, -0.5f, 0.8f);
        D3DXQuaternionRotationAxis(&q, &axis, D3DXToRadian(123.0f));
        D3DXMATRIX r;
        D3DXMatrixRotationQuaternion(&r, &q);

        D3DXVECTOR3 v(1.0f, -2.0f, 3.5f), out;
        D3DXVec3TransformNormal(&out, &v, &r);
        std::snprintf(buf, sizeof(buf), "(%.5f vs %.5f)",
                      D3DXVec3Length(&out), D3DXVec3Length(&v));
        Check("a rotation keeps the vector length",
              Near(D3DXVec3Length(&out), D3DXVec3Length(&v), 1e-3f), buf);

        // A rotation matrix is orthogonal: R * R^T = I.
        D3DXMATRIX rt, prod, id;
        D3DXMatrixTranspose(&rt, &r);
        D3DXMatrixMultiply(&prod, &r, &rt);
        D3DXMatrixIdentity(&id);
        Check("a rotation matrix is orthogonal (R*R^T = I)", NearMat(prod, id, 1e-3f));
    }

    // --- 8. VIEW ---------------------------------------------------------------
    // The defining property of a view matrix: the eye lands at the origin.
    {
        const D3DXVECTOR3 eye(10.0f, 5.0f, 20.0f), at(0.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f);
        D3DXMATRIX view;
        D3DXMatrixLookAtRH(&view, &eye, &at, &up);

        D3DXVECTOR3 out;
        D3DXVec3TransformCoord(&out, &eye, &view);
        std::snprintf(buf, sizeof(buf), "(%.4f %.4f %.4f)", out.x, out.y, out.z);
        Check("LookAtRH: the eye goes to the origin",
              NearVec(out, 0.0f, 0.0f, 0.0f, 1e-3f), buf);

        // The target lies IN FRONT of the camera, i.e. on negative Z (right-handed system).
        D3DXVec3TransformCoord(&out, &at, &view);
        std::snprintf(buf, sizeof(buf), "(z=%.3f)", out.z);
        Check("LookAtRH: the target lies on NEGATIVE Z (right-handed)", out.z < 0.0f, buf);
        Check("LookAtRH: the target lies on the view axis",
              Near(out.x, 0.0f, 1e-3f) && Near(out.y, 0.0f, 1e-3f));
    }

    // --- 9. VECTORS --------------------------------------------------------------
    {
        D3DXVECTOR3 a(3.0f, 4.0f, 0.0f), n;
        Check("Vec3Length(3,4,0) = 5", Near(D3DXVec3Length(&a), 5.0f));
        Check("Vec3LengthSq(3,4,0) = 25", Near(D3DXVec3LengthSq(&a), 25.0f));
        D3DXVec3Normalize(&n, &a);
        Check("Vec3Normalize gives length 1", Near(D3DXVec3Length(&n), 1.0f));

        // D3DX on a zero vector returns zero, not NaN. TMP4 code normalizes
        // directions that are sometimes zero - this is not a theoretical case.
        const D3DXVECTOR3 zero(0.0f, 0.0f, 0.0f);
        D3DXVec3Normalize(&n, &zero);
        Check("Vec3Normalize(0) gives zero, not NaN", NearVec(n, 0.0f, 0.0f, 0.0f));

        const D3DXVECTOR3 x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f);
        Check("Vec3Dot of perpendicular = 0", Near(D3DXVec3Dot(&x, &y), 0.0f));
        Check("Vec3Dot of parallel = 1", Near(D3DXVec3Dot(&x, &x), 1.0f));

        D3DXVECTOR3 lerp;
        D3DXVec3Lerp(&lerp, &x, &y, 0.25f);
        Check("Vec3Lerp(v1,v2,0.25)", NearVec(lerp, 0.75f, 0.25f, 0.0f));

        D3DXVECTOR2 v2(3.0f, 4.0f), n2;
        D3DXVec2Normalize(&n2, &v2);
        Check("Vec2Normalize(3,4) = (0.6, 0.8)", Near(n2.x, 0.6f) && Near(n2.y, 0.8f));

        // Operator overloads - TMP4 code uses them a lot.
        const D3DXVECTOR3 sum = x + y;
        const D3DXVECTOR3 sc  = x * 3.0f;
        Check("operators + and * on D3DXVECTOR3",
              NearVec(sum, 1.0f, 1.0f, 0.0f) && NearVec(sc, 3.0f, 0.0f, 0.0f));
        Check("the D3DXVECTOR3 -> float* conversion points at x",
              static_cast<const float*>(x)[0] == 1.0f);
    }

    // --- 10. PLANES --------------------------------------------------------------
    {
        // The plane y = 5, with a deliberately NON-unit normal.
        D3DXPLANE raw(0.0f, 2.0f, 0.0f, -10.0f), p;
        D3DXPlaneNormalize(&p, &raw);
        Check("PlaneNormalize scales d too (not just the normal)",
              Near(p.b, 1.0f) && Near(p.d, -5.0f));

        const D3DXVECTOR3 above(0.0f, 8.0f, 0.0f), below(0.0f, 1.0f, 0.0f), on(0.0f, 5.0f, 0.0f);
        Check("PlaneDotCoord gives the signed DISTANCE",
              Near(D3DXPlaneDotCoord(&p, &above), 3.0f));
        Check("a point on the other side has a negative sign",
              D3DXPlaneDotCoord(&p, &below) < 0.0f);
        Check("a point ON the plane gives zero",
              Near(D3DXPlaneDotCoord(&p, &on), 0.0f));
    }

    // --- 11. COLOUR ----------------------------------------------------------------
    {
        const D3DXCOLOR c(static_cast<DWORD>(0x80FF8000u));
        std::snprintf(buf, sizeof(buf), "(%.3f %.3f %.3f %.3f)", c.r, c.g, c.b, c.a);
        Check("D3DXCOLOR from A8R8G8B8 splits the channels correctly",
              Near(c.r, 1.0f, 0.01f) && Near(c.g, 0.502f, 0.01f) &&
              Near(c.b, 0.0f, 0.01f) && Near(c.a, 0.502f, 0.01f), buf);
    }

    // --- 12. THIRD BATCH --------------------------------------------
    {
        // The conjugate of a unit quaternion is its inverse: composing
        // a rotation with its conjugate must give the identity. This property catches every
        // sign error at once.
        const D3DXVECTOR3 axisQ(0.2f, 0.7f, -0.4f);
        D3DXQUATERNION q, qc, composition;
        D3DXQuaternionRotationAxis(&q, &axisQ, D3DXToRadian(77.0f));
        D3DXQuaternionConjugate(&qc, &q);
        D3DXQuaternionMultiply(&composition, &q, &qc);
        Check("q * conj(q) = identity",
              Near(composition.x, 0.0f) && Near(composition.y, 0.0f) &&
              Near(composition.z, 0.0f) && Near(composition.w, 1.0f));

        // The determinant: the identity gives 1, a scaling the product of the scales, and a rotation
        // does NOT change the determinant, because it is orthogonal.
        D3DXMATRIX id, sk, rot;
        D3DXMatrixIdentity(&id);
        D3DXMatrixScaling(&sk, 2.0f, 3.0f, 4.0f);
        D3DXMatrixRotationQuaternion(&rot, &q);
        std::snprintf(buf, sizeof(buf), "(%.3f)", D3DXMatrixfDeterminant(&sk));
        Check("determinant of the identity = 1", Near(D3DXMatrixfDeterminant(&id), 1.0f));
        Check("determinant of the scaling (2,3,4) = 24",
              Near(D3DXMatrixfDeterminant(&sk), 24.0f, 1e-3f), buf);
        Check("determinant of a rotation = 1", Near(D3DXMatrixfDeterminant(&rot), 1.0f, 1e-3f));

        // Opposite control: a singular matrix must give zero.
        D3DXMATRIX singular;
        D3DXMatrixScaling(&singular, 1.0f, 0.0f, 1.0f);
        Check("determinant of a singular matrix = 0",
              Near(D3DXMatrixfDeterminant(&singular), 0.0f));

        // An off-centre projection: the centre of the frame has to land in the centre of the screen, and a corner
        // in a corner. The frame is deliberately NOT centred on the view axis -
        // with a symmetric one an offset error would be invisible.
        D3DXMATRIX ortho;
        D3DXMatrixOrthoOffCenterRH(&ortho, 10.0f, 30.0f, 5.0f, 25.0f, 1.0f, 100.0f);
        D3DXVECTOR3 centre(20.0f, 15.0f, -1.0f), cornerLB(10.0f, 5.0f, -1.0f), res;
        D3DXVec3TransformCoord(&res, &centre, &ortho);
        std::snprintf(buf, sizeof(buf), "(%.3f %.3f)", res.x, res.y);
        Check("OrthoOffCenterRH: frame centre -> (0,0)",
              Near(res.x, 0.0f) && Near(res.y, 0.0f), buf);
        D3DXVec3TransformCoord(&res, &cornerLB, &ortho);
        std::snprintf(buf, sizeof(buf), "(%.3f %.3f)", res.x, res.y);
        Check("OrthoOffCenterRH: bottom left corner -> (-1,-1)",
              Near(res.x, -1.0f) && Near(res.y, -1.0f), buf);

        // The vertex size. The default bits `00` mean TWO texture
        // coordinates, not zero - here it is easiest to count half too few.
        const UINT size = D3DXGetFVFVertexSize(D3DFVF_XYZ | D3DFVF_NORMAL |
                                                  D3DFVF_DIFFUSE | D3DFVF_TEX1);
        std::snprintf(buf, sizeof(buf), "(%u, expected 36)", size);
        Check("FVF XYZ|NORMAL|DIFFUSE|TEX1 = 12+12+4+8 = 36", size == 36u, buf);

        const UINT noTex = D3DXGetFVFVertexSize(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        std::snprintf(buf, sizeof(buf), "(%u, expected 16)", noTex);
        Check("FVF XYZ|DIFFUSE = 12+4 = 16", noTex == 16u, buf);

        const UINT rhw = D3DXGetFVFVertexSize(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        std::snprintf(buf, sizeof(buf), "(%u, expected 28)", rhw);
        Check("FVF XYZRHW|DIFFUSE|TEX1 = 16+4+8 = 28", rhw == 28u, buf);
    }

    std::printf("\n=== %s ===\n",
                failures == 0 ? "D3DX8 MATH LAYER WORKS"
                              : "TEST FAILED");
    return failures == 0 ? 0 : 1;
}
