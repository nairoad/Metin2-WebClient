// d3d8_fvf_test.cpp - test of splitting an FVF code into a vertex layout.
//
// WHY: the order of the components in a vertex is IMPOSED by the game files
// (category A). A mistake in an offset does not crash the program - it gives a model
// that reads the texture coordinates as the position and sprawls across the whole
// screen. Or, worse, an offset by four bytes: an almost right picture,
// with a slight skew nobody can name.
//
// The strongest test in this file is a COMPARISON OF TWO INDEPENDENT ROUTES:
// the vertex size computed by `M2W_VertexLayout` (by summing
// offsets) must agree with `D3DXGetFVFVertexSize` from `d3dx8.h`, which
// was written separately and earlier. Agreement of two separately written functions
// is evidence that neither of them gives alone.
//
// Running:
//   em++ -std=c++17 -O1 -Icompat compat/tests/d3d8_fvf_test.cpp \
//        compat/d3d8_fvf.cpp compat/win32_compat.cpp \
//        -o d3d8_fvf_test.js && node d3d8_fvf_test.js

#include <cstdio>

#include "d3d8_fvf.h"
#include "d3dx8.h"

namespace {

int errors = 0;

/// Prints one check line (`OK` / `FAIL` and the detail) and counts failures.
void Check(const char* name, bool ok, const char* detail = "")
{
    std::printf("  %-62s %s %s\n", name, ok ? "OK  " : "FAIL", detail);
    if (!ok) ++errors;
}

}  // namespace

/// Runs every check of this file; exit 0 when all passed, 1 otherwise.
int main()
{
    std::printf("\n=== splitting an FVF code into a vertex layout ===\n\n");

    // -----------------------------------------------------------------
    std::printf("[the client's most common format: XYZ|DIFFUSE|TEX1]\n");
    {
        const TVertexLayout u =
            M2W_VertexLayout(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        char buf[64];

        Check("position at the start", u.iPosition == 0);
        Check("position has three components", u.iPositionComponents == 3);
        Check("it is not transformed", !u.bTransformed);
        Check("colour after the position (12)", u.iDiffuse == 12);
        Check("coordinates after the colour (16)", u.aiTexCoords[0] == 16);
        Check("TWO coordinate components by default", u.aiTexCoordComponents[0] == 2);
        Check("one set", u.iTexCoordSets == 1);

        std::snprintf(buf, sizeof(buf), "(%u)", u.uStride);
        Check("size 12+4+8 = 24", u.uStride == 24, buf);

        // A missing component is MINUS ONE, not zero - zero would mean
        // "lies at the start" and the caller would read the position as the normal.
        Check("no normal is -1, not 0", u.iNormal == -1);
        Check("no specular is -1", u.iSpecular == -1);
        Check("no point size is -1", u.iPointSize == -1);
        Check("an unused set is -1", u.aiTexCoords[1] == -1);
    }

    // -----------------------------------------------------------------
    std::printf("\n[with a normal: XYZ|NORMAL|DIFFUSE|TEX1]\n");
    {
        const TVertexLayout u = M2W_VertexLayout(
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);

        // The order is imposed: position, normal, colour, coordinates.
        // If the colour went before the normal - and that is a natural mistake,
        // because that is how one thinks of them - everything after the position would be shifted.
        Check("normal right after the position (12)", u.iNormal == 12);
        Check("colour after the normal (24)", u.iDiffuse == 24);
        Check("coordinates at the end (28)", u.aiTexCoords[0] == 28);
        Check("size 12+12+4+8 = 36", u.uStride == 36);
    }

    // -----------------------------------------------------------------
    std::printf("\n[user interface: XYZRHW|DIFFUSE|TEX1]\n");
    {
        const TVertexLayout u = M2W_VertexLayout(
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

        Check("XYZRHW has FOUR components", u.iPositionComponents == 4);
        Check("XYZRHW means: already transformed", u.bTransformed);
        Check("colour after four numbers (16)", u.iDiffuse == 16);
        Check("size 16+4+8 = 28", u.uStride == 28);
    }

    // -----------------------------------------------------------------
    std::printf("\n[two coordinate sets - terrain and shadows]\n");
    {
        const TVertexLayout u = M2W_VertexLayout(
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2);

        Check("two sets", u.iTexCoordSets == 2);
        Check("the first set after the normal (24)", u.aiTexCoords[0] == 24);
        Check("the second set after the first (32)", u.aiTexCoords[1] == 32);
        Check("size 12+12+8+8 = 40", u.uStride == 40);
    }

    // -----------------------------------------------------------------
    std::printf("\n[the component count of a set is NOT those two bits]\n");
    {
        // FVF keeps the component count in two bits per set, but through
        // a TABLE: 0 means two components, 1 three, 2 four, 3 one.
        // Reading those bits as a number would give two where there are four.
        const TVertexLayout u4 = M2W_VertexLayout(
            D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
        Check("TEXCOORDSIZE4 gives four components", u4.aiTexCoordComponents[0] == 4);
        Check("size 12+16 = 28", u4.uStride == 28);

        const TVertexLayout u3 = M2W_VertexLayout(
            D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
        Check("TEXCOORDSIZE3 gives three components", u3.aiTexCoordComponents[0] == 3);

        const TVertexLayout u1 = M2W_VertexLayout(
            D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE1(0));
        Check("TEXCOORDSIZE1 gives ONE component", u1.aiTexCoordComponents[0] == 1);

        // OPPOSITE control: if the function read those bits directly as a
        // number, TEXCOORDSIZE1 (bits 11 = 3) would give three, not one.
        Check("the bits are not read directly as a number",
                u1.aiTexCoordComponents[0] != 3);
    }

    // -----------------------------------------------------------------
    std::printf("\n[a format we do not know says so plainly]\n");
    {
        // `XYZB1` is the position plus a bone blend weight. The client does not
        // use it, but if it started to, the layout would be shifted by those weights.
        const TVertexLayout u = M2W_VertexLayout(
            D3DFVF_XYZB1 | D3DFVF_DIFFUSE);
        Check("XYZB1 does not pretend to be understood", !u.bKnown);

        const TVertexLayout ok = M2W_VertexLayout(
            D3DFVF_XYZ | D3DFVF_DIFFUSE);
        Check("an ordinary format IS understood", ok.bKnown);

        // And this is a check that `XYZRHW` (0x0004) was not taken for
        // `XYZB1` (0x0006) - both values share bits and testing
        // them with `&` in the wrong order would confuse them.
        const TVertexLayout rhw = M2W_VertexLayout(D3DFVF_XYZRHW);
        Check("XYZRHW is not taken for XYZB1", rhw.bKnown && rhw.bTransformed);
    }

    // -----------------------------------------------------------------
    std::printf("\n[AGREEMENT WITH A SECOND, SEPARATELY WRITTEN ROUTE]\n");
    {
        // `D3DXGetFVFVertexSize` in `d3dx8.h` computes the vertex size
        // independently - by summing sizes, without computing offsets.
        // Our function reaches the same number another way: through
        // the offset of the last component. Agreement of two separately written
        // functions is evidence that neither of them gives alone.
        const DWORD codes[] = {
            D3DFVF_XYZ,
            D3DFVF_XYZ | D3DFVF_DIFFUSE,
            D3DFVF_XYZ | D3DFVF_NORMAL,
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE,
            D3DFVF_XYZ | D3DFVF_TEX1,
            D3DFVF_XYZ | D3DFVF_TEX2,
            D3DFVF_XYZ | D3DFVF_TEX3,
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2,
            D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE,
            D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0),
            D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0),
        };

        int agreeing = 0;
        bool all = true;
        char buf[96];

        for (DWORD code : codes)
        {
            const TVertexLayout u = M2W_VertexLayout(code);
            const UINT uSecondRoute = D3DXGetFVFVertexSize(code);
            if (u.uStride == uSecondRoute)
            {
                ++agreeing;
            }
            else
            {
                all = false;
                std::snprintf(buf, sizeof(buf),
                              "(FVF 0x%lX: layout %u, d3dx8 %u)",
                              static_cast<unsigned long>(code),
                              u.uStride, uSecondRoute);
                Check("MISMATCH", false, buf);
            }
        }

        std::snprintf(buf, sizeof(buf), "(%d of %d)", agreeing,
                      static_cast<int>(sizeof(codes) / sizeof(codes[0])));
        Check("both routes give the same size", all, buf);
    }

    // -----------------------------------------------------------------
    std::printf("\n[OPPOSITE control on the agreement test itself]\n");
    {
        // If both functions returned zero always, the agreement test above
        // would pass without objection. I check that the sizes differ
        // and are non-zero.
        const UINT a = M2W_VertexLayout(D3DFVF_XYZ).uStride;
        const UINT b = M2W_VertexLayout(D3DFVF_XYZ | D3DFVF_DIFFUSE).uStride;
        Check("the sizes are non-zero", a > 0 && b > 0);
        Check("different formats give different sizes", a != b);
        Check("adding the colour adds four bytes", b == a + 4);
    }

    // -----------------------------------------------------------------
    std::printf("\n[D3DVSD DECLARATIONS - four real ones, from GrpDevice.cpp]\n");
    {
        // These are not examples invented for the test. They are copied
        // word for word from `stage/eterLib/GrpDevice.cpp` - from the same
        // client that is to draw with them later. The character declaration
        // (`CreatePNTStreamVertexShader`) describes the position, the normal
        // and one set of coordinates.
        const DWORD aPNT[] = {
            D3DVSD_STREAM(0),
            D3DVSD_REG(0, D3DVSDT_FLOAT3),
            D3DVSD_REG(3, D3DVSDT_FLOAT3),
            D3DVSD_REG(7, D3DVSDT_FLOAT2),
            D3DVSD_END()
        };

        const TVertexLayout kPNT = M2W_LayoutFromDeclaration(aPNT);
        Check("PNT: position at the start", kPNT.iPosition == 0);
        Check("PNT: position has three components", kPNT.iPositionComponents == 3);
        Check("PNT: normal after twelve bytes", kPNT.iNormal == 12);
        Check("PNT: coordinates after twenty-four",
                kPNT.aiTexCoords[0] == 24 && kPNT.aiTexCoordComponents[0] == 2);
        Check("PNT: one set of coordinates", kPNT.iTexCoordSets == 1);
        Check("PNT: the vertex size is 32 bytes", kPNT.uStride == 32);
        Check("PNT: there is no colour", kPNT.iDiffuse == -1 && kPNT.iSpecular == -1);
        Check("PNT: NOT transformed", !kPNT.bTransformed);
        Check("PNT: understood in full", kPNT.bKnown);

        // THE SAME COMPUTED BY A SECOND ROUTE. The PNT declaration describes exactly
        // the same layout as `D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1` -
        // and both splitters must give the same result. If one of them had
        // a mistake of one field, this equality would show it.
        {
            const TVertexLayout kFVF = M2W_VertexLayout(
                D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
            Check("PNT agrees with the equivalent FVF code",
                    kFVF.uStride == kPNT.uStride &&
                    kFVF.iPosition == kPNT.iPosition &&
                    kFVF.iNormal == kPNT.iNormal &&
                    kFVF.aiTexCoords[0] == kPNT.aiTexCoords[0] &&
                    kFVF.iTexCoordSets == kPNT.iTexCoordSets);
        }

        // A declaration with TWO sets of coordinates.
        const DWORD aPNT2[] = {
            D3DVSD_STREAM(0),
            D3DVSD_REG(0, D3DVSDT_FLOAT3),
            D3DVSD_REG(3, D3DVSDT_FLOAT3),
            D3DVSD_REG(7, D3DVSDT_FLOAT2),
            D3DVSD_REG(D3DVSDE_TEXCOORD1, D3DVSDT_FLOAT2),
            D3DVSD_END()
        };
        const TVertexLayout kPNT2 = M2W_LayoutFromDeclaration(aPNT2);
        Check("PNT2: two sets of coordinates", kPNT2.iTexCoordSets == 2);
        Check("PNT2: the second set after thirty-two",
                kPNT2.aiTexCoords[1] == 32);
        Check("PNT2: the size is 40 bytes", kPNT2.uStride == 40);
        Check("PNT2: understood in full", kPNT2.bKnown);

        // A declaration with a SECOND STREAM. We do not pretend we can do it:
        // stream zero has to be split correctly, but `bKnown` must
        // drop, because there is nowhere to take the second stream from.
        const DWORD aPT[] = {
            D3DVSD_STREAM(0),
            D3DVSD_REG(0, D3DVSDT_FLOAT3),
            D3DVSD_STREAM(1),
            D3DVSD_REG(7, D3DVSDT_FLOAT2),
            D3DVSD_END()
        };
        const TVertexLayout kPT = M2W_LayoutFromDeclaration(aPT);
        Check("PT: stream zero split correctly",
                kPT.iPosition == 0 && kPT.uStride == 12);
        Check("PT: the second stream REPORTED as unsupported",
                !kPT.bKnown);
        Check("PT: coordinates from the second stream do NOT enter stream zero",
                kPT.aiTexCoords[0] == -1);

        // The double stream from GrpDevice - the same pattern, more fields.
        const DWORD aDouble[] = {
            D3DVSD_STREAM(0),
            D3DVSD_REG(0, D3DVSDT_FLOAT3),
            D3DVSD_REG(3, D3DVSDT_FLOAT3),
            D3DVSD_REG(7, D3DVSDT_FLOAT2),
            D3DVSD_STREAM(1),
            D3DVSD_REG(D3DVSDE_POSITION2, D3DVSDT_FLOAT3),
            D3DVSD_REG(D3DVSDE_NORMAL2, D3DVSDT_FLOAT3),
            D3DVSD_REG(D3DVSDE_TEXCOORD1, D3DVSDT_FLOAT2),
            D3DVSD_END()
        };
        const TVertexLayout kDouble = M2W_LayoutFromDeclaration(aDouble);
        Check("double: stream zero the same as in PNT",
                kDouble.uStride == 32 && kDouble.iNormal == 12);
        Check("double: reported as unsupported", !kDouble.bKnown);
    }

    // -----------------------------------------------------------------
    std::printf("\n[declarations - edge cases]\n");
    {
        Check("a null pointer gives no position and bKnown false",
                M2W_LayoutFromDeclaration(NULL).iPosition == -1 &&
                !M2W_LayoutFromDeclaration(NULL).bKnown);

        // The end marker alone - a declaration without a single field.
        const DWORD aEmpty[] = { D3DVSD_END() };
        const TVertexLayout kEmpty = M2W_LayoutFromDeclaration(aEmpty);
        Check("a declaration without fields has no position", kEmpty.iPosition == -1);
        Check("and has size zero", kEmpty.uStride == 0);

        // A GAP IN THE NUMBERING. Set 0 skipped, only 1 used.
        // Direct3D allowed it. The gap has to stay a gap (-1), and the number
        // of sets has to be the HIGHEST number plus one - otherwise the shader
        // would read set 1 as set zero.
        const DWORD aGap[] = {
            D3DVSD_STREAM(0),
            D3DVSD_REG(0, D3DVSDT_FLOAT3),
            D3DVSD_REG(D3DVSDE_TEXCOORD1, D3DVSDT_FLOAT2),
            D3DVSD_END()
        };
        const TVertexLayout kGap = M2W_LayoutFromDeclaration(aGap);
        Check("the skipped set 0 stays a gap", kGap.aiTexCoords[0] == -1);
        Check("set 1 lies where it was given", kGap.aiTexCoords[1] == 12);
        Check("the number of sets is the highest number plus one",
                kGap.iTexCoordSets == 2);

        // Colour as `D3DCOLOR` - four bytes, not sixteen.
        const DWORD aColor[] = {
            D3DVSD_STREAM(0),
            D3DVSD_REG(0, D3DVSDT_FLOAT3),
            D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
            D3DVSD_REG(7, D3DVSDT_FLOAT2),
            D3DVSD_END()
        };
        const TVertexLayout kColor = M2W_LayoutFromDeclaration(aColor);
        Check("D3DCOLOR takes FOUR bytes", kColor.aiTexCoords[0] == 16);
        Check("the colour lies right after the position", kColor.iDiffuse == 12);
        Check("the size with colour is 24 bytes", kColor.uStride == 24);

        // An unknown data type: the splitter has to STOP, not guess
        // the size. A guessed size would shift everything further on.
        const DWORD aBad[] = {
            D3DVSD_STREAM(0),
            D3DVSD_REG(0, D3DVSDT_FLOAT3),
            D3DVSD_REG(7, 0x0Du),          // a type that does not exist
            D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
            D3DVSD_END()
        };
        const TVertexLayout kBad = M2W_LayoutFromDeclaration(aBad);
        Check("an unknown data type reports bKnown false", !kBad.bKnown);
        Check("and does NOT append the fields lying after it", kBad.iDiffuse == -1);

        // A declaration without the end word. It has to end on the reserve,
        // not read the memory next to the array.
        DWORD aNoEnd[300];
        for (int i = 0; i < 300; ++i)
            aNoEnd[i] = D3DVSD_REG(0, D3DVSDT_FLOAT3);
        const TVertexLayout kNoEnd = M2W_LayoutFromDeclaration(aNoEnd);
        Check("a declaration without an end does not loop",
                kNoEnd.uStride == 256u * 12u);
    }

    std::printf("\n=== %s ===\n",
                errors == 0 ? "FVF SPLITTING WORKS" : "TEST FAILED");
    return errors == 0 ? 0 : 1;
}
