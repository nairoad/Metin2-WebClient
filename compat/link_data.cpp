// SPDX-License-Identifier: GPL-2.0-or-later
// link_data.cpp - data symbols the game references and no library defines:
// today one, `GrannyPNT332VertexType`, the vertex layout the game asks the
// Granny layer to convert meshes into.

// Design:
// The first full link failed on exactly three
// symbols, all of them DATA. `-sERROR_ON_UNDEFINED_SYMBOLS=0` turns a missing
// FUNCTION into a stub that aborts with its own name when called - a loud
// stub. Nothing loud can stand in for missing DATA: data has no call at
// which it could shout, so the linker stops, and rightly so. The choice is
// a true value or a lie; this file exists because a true value is available
// for every symbol it ever held - the layout is spelled out in headers we
// have.
//
// The other two (`_Py_NoneStruct`, `PyExc_RuntimeError`) lived here at first.
// Now the real `libpython3.13.a` built for wasm defines
// them, and our copies had to go: two definitions of one datum are a link
// error, and had ours won, Python's `None` would not have been the `None`
// the rest of CPython knows.
//
// What remains is not a stub. `granny_pnt332_vertex` is `Position[3]`,
// `Normal[3]`, `UV[2]`, all `real32` - a layout imposed from outside, and
// the Granny layer (granny_web.cpp) converts vertices into it BY MEMBER
// NAME, so the names below matter as much as the sizes.

#include <cstddef>

// The struct is repeated from `granny.h` instead of including it, because
// this file must compile WITHOUT the TMP4 source tree on the include path -
// it belongs to the compatibility layer, not to the game. Checked against
// the header:
//
//     GRANNY_STRUCT(struct) granny_data_type_definition
//     {
//         granny_member_type Type;
//         char const * Name;
//         granny_data_type_definition * ReferenceType;
//         granny_int32 ArrayWidth;
//         granny_int32 Extra[3];
//         granny_uintaddrx Ignored_Ignored;
//     };
namespace
{

struct TMemberDefinition
{
    int          eType;
    const char*  c_szName;
    void*        pReferenceType;
    int          iArrayWidth;
    int          aiExtra[3];
    size_t       uIgnored;
};

// `granny_member_type` from `granny.h`: `GrannyEndMember` is first in the
// enumeration (0), `GrannyReal32Member` is 10. Counted from the header
// order, because reading a `.gr2` depends on these numbers.
const int c_iEndMember = 0;
const int c_iReal32Member = 10;

// PNT332 - Position, Normal, TextureCoordinates: three, three, two, hence
// the name. The array MUST end with a `GrannyEndMember` entry: Granny gets
// no length, it looks for the end, and a missing terminator is not a bad
// image but a read past the array.
//
// Granny appends the texture-coordinate set number to the name; the header
// gives only the stem (`GrannyVertexTextureCoordinatesName` is
// "TextureCoordinates"). The `0` is confirmed in OUR data: the vertex type
// of a corpus file (`property/n/obj/snow.m/general_obj_bell.gr2`, dumped
// with `tools/gr2.py --tree`) names its members `Position`, `Normal`,
// `TextureCoordinates0` - and granny_web.cpp matches exactly that name when
// it copies vertices.
TMemberDefinition g_akPNT332[] = {
    { c_iReal32Member, "Position",            NULL, 3, { 0, 0, 0 }, 0 },
    { c_iReal32Member, "Normal",              NULL, 3, { 0, 0, 0 }, 0 },
    { c_iReal32Member, "TextureCoordinates0", NULL, 2, { 0, 0, 0 }, 0 },
    { c_iEndMember,    NULL,                  NULL, 0, { 0, 0, 0 }, 0 },
};

}  // namespace

extern "C" {

/// `granny_pnt332_vertex` layout, the target type custom_draw.cpp passes to
/// `GrannyCopyMeshVertices` / `GrannyNewMeshDeformer`.
void* GrannyPNT332VertexType = g_akPNT332;

}  // extern "C"
