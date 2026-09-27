// SPDX-License-Identifier: GPL-2.0-or-later
// gr2_file.h - reader of a Granny `.gr2` file: sections, relocations and
// the type tree; the self-describing format walked by member NAME, so a
// file from an exporter never seen before still reads.

// Design:
// WHY THE READER LIVES IN THE CLIENT, not beside it: the user's decision,
// for ease of swapping data. Servers with their own models hand them out as
// `.gr2`, so a reader inside means "swap the pack and it works". Converting
// outside the client would mean putting every new model through a tool
// first.
//
// THE FORMAT DESCRIBES ITSELF - and all generality stands on that. The
// file has no "character structure" baked in. It has a TYPE TREE: a run of
// member descriptions (`granny_data_type_definition`), and the file root is
// a pair (pointer to a type description, pointer to an object). Reading
// therefore starts by reading the DESCRIPTION, not from an assumption -
// which is why the same reader handles a file from an exporter we have
// never seen.
//
// THIS IS NOT THE SAME AS `granny.h`, and that matters. Measured
// (file layouts compared with the header): the layout in the FILE differs from
// `granny.h`, which TMP4 compiles against. The corpus files came out of
// Granny 2.4.0.7 and the header is from a later SDK with fields the files
// do not have:
//   granny_skeleton  header: Name BoneCount Bones LODType ExtendedData
//                    file:   Name BoneCount Bones
//   granny_bone      header: Name ParentIndex LocalTransform InverseWorld4x4
//                            LODError ExtendedData
//                    file:   Name ParentIndex Transform InverseWorldTransform
//                            LightInfo CameraInfo ExtendedData
// Had the reader simply CAST the unpacked buffer to the header structs
// (tempting, because wasm32 has four-byte pointers, exactly like the file),
// `LODError` would read `LightInfo` and nobody would find out. So the reader
// COPIES the objects, picking the members BY NAME - exactly as
// `GrannyConvertSingleObject` does. The same road gives resilience to files
// from other servers: a missing member stays zero, an extra one is skipped.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include <granny.h>

namespace m2wgr2 {

/// An unpacked `.gr2` file: sections in memory with the pointers already
/// written in.
class CFile
{
public:
    /// An empty file: no sections, no root, no error.
    CFile();
    /// Frees the unpacked sections and every block from `Allocate`.
    ~CFile();

    /// Unpacks the whole file. The input buffer is not kept.
    bool Load(const uint8_t* c_pbyFile, uint32_t uSize);

    /// Type description of the root object - or NULL.
    granny_data_type_definition* RootType() const { return m_pRootType; }

    /// The root object - or NULL.
    void* RootObject() const { return m_pvRootObject; }

    /// Memory living as long as the file. Zeroed.
    void* Allocate(size_t uBytes);

    /// The reason of the last refusal. Never NULL.
    const char* Error() const { return m_c_szError; }

private:
    /// Not copyable: the file owns raw memory blocks.
    CFile(const CFile&);
    /// Not assignable (declared, never defined).
    CFile& operator=(const CFile&);

    /// Unpacks the `uSections` sections described by the table at
    /// `c_pbyTable` (absolute data offsets into `c_pbyFile`) into
    /// `m_vecSections`; false with `m_c_szError` on a bad entry.
    bool LoadSections(const uint8_t* c_pbyFile, uint32_t uSize,
                      const uint8_t* c_pbyTable, uint32_t uSections);

    /// Writes the real addresses into the unpacked sections from each
    /// section's relocation table; false with `m_c_szError` on a bad one.
    bool ApplyRelocations(const uint8_t* c_pbyFile, uint32_t uSize,
                          const uint8_t* c_pbyTable, uint32_t uSections);

    std::vector<uint8_t*> m_vecSections;
    std::vector<uint32_t> m_vecSectionSizes;
    std::vector<uint8_t*> m_vecBlocks;     // memory for the copied objects
    granny_data_type_definition* m_pRootType;
    void* m_pvRootObject;
    const char* m_c_szError;
};

// ---------------------------------------------------------------------------
// WALKING THE SOURCE TYPE TREE
// ---------------------------------------------------------------------------
// After the relocations the type descriptions in the file hold REAL
// pointers, so they can be walked like ordinary structs. Note: these are
// the descriptions FROM THE FILE, not from `granny.h` - see above.

/// How many bytes ONE element of a member of this kind takes. For an
/// inline member (`GrannyInlineMember`) computed recursively from the
/// component's type description.
uint32_t MemberSize(const granny_data_type_definition* c_pMember);

/// How many bytes the whole object described by this type takes.
uint32_t TypeSize(const granny_data_type_definition* c_pType);

/// Looks a member up BY NAME. Returns its description and sets `*ppbyMember`
/// to its start in the object. NULL when there is no such member - an
/// ordinary state, not an error: a file from another exporter may lack it.
const granny_data_type_definition* FindMember(
    const granny_data_type_definition* c_pType, const char* c_szName,
    const void* c_pvObject, const uint8_t** ppbyMember);

/// The string under member `c_szName` - or NULL.
const char* MemberString(const granny_data_type_definition* c_pType,
                         const void* c_pvObject, const char* c_szName);

/// The integer under a member - or `iDefault`.
int32_t MemberInt32(const granny_data_type_definition* c_pType,
                    const void* c_pvObject, const char* c_szName,
                    int32_t iDefault = 0);

/// The float under a member - or `fDefault`.
float MemberReal32(const granny_data_type_definition* c_pType,
                   const void* c_pvObject, const char* c_szName,
                   float fDefault = 0.0f);

/// Copies `uCount` floats out of a member. False when the member is absent
/// or has another size - `pafTarget` is then left untouched.
bool MemberReal32Array(const granny_data_type_definition* c_pType,
                       const void* c_pvObject, const char* c_szName,
                       float* pafTarget, uint32_t uCount);

/// The object a `Reference` member points at - or NULL. Sets
/// `*ppElementType` to that object's type description.
void* MemberReference(const granny_data_type_definition* c_pType,
                      const void* c_pvObject, const char* c_szName,
                      granny_data_type_definition** ppElementType);

/// The array under a member. Handles BOTH kinds of array at once:
///   `ReferenceToArray`   - a run of objects, `*ppvArray` is the first
///   `ArrayOfReferences`  - a run of POINTERS to objects
/// `*pbPointers` says which case it is. Returns the element count.
int32_t MemberArray(const granny_data_type_definition* c_pType,
                    const void* c_pvObject, const char* c_szName,
                    void** ppvArray, granny_data_type_definition** ppElementType,
                    bool* pbPointers);

/// The i-th element of an array read by `MemberArray`.
void* ArrayElement(void* pvArray, granny_data_type_definition* pElementType,
                   bool bPointers, int32_t iIndex);

/// An array with its type stored beside it (`ReferenceToVariantArray`) -
/// that is how vertices are stored. Returns the element count and sets the
/// type.
int32_t MemberVariantArray(const granny_data_type_definition* c_pType,
                           const void* c_pvObject, const char* c_szName,
                           void** ppvArray,
                           granny_data_type_definition** ppElementType);

}  // namespace m2wgr2
