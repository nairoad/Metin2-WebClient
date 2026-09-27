// SPDX-License-Identifier: GPL-2.0-or-later
// gr2_file.cpp - the `.gr2` reader: header, section table, Oodle1 sections,
// relocations, and the type-tree walkers. See gr2_file.h.

// Design: transcribed from `tools/gr2.py`, which walks the same files and
// serves as the measure. The one difference follows from the language:
// Python kept the sections apart and a pointer was a pair (section,
// offset). Here the relocations are written in as REAL pointers - exactly
// what the original loader does - so the tree is walked with a plain `->`.

#include "gr2_file.h"
#include <cstdint>

#include <stdlib.h>
#include <string.h>

#include "gr2_oodle1.h"

namespace m2wgr2 {

namespace {

/// Granny file magic, little-endian 32-bit variant. The corpus has no other
/// (400 of 400), but a mismatch has to give an understandable refusal.
const uint8_t c_abyMagicLE32[16] = {
    0xb8, 0x67, 0xb0, 0xca, 0xf8, 0x6d, 0xb1, 0x0f,
    0x84, 0x72, 0x8c, 0x7e, 0x5e, 0x19, 0x00, 0x1e };

const uint32_t c_uSectionEntrySize = 44;
const uint32_t c_uRelocationSize = 12;

/// A uint32 from an unaligned address, in host order (`memcpy`; the files and
/// wasm are both little-endian).
uint32_t ReadU32(const uint8_t* c_pby)
{
    uint32_t u;
    memcpy(&u, c_pby, 4);
    return u;
}

}  // namespace

// ---------------------------------------------------------------------------
// SIZES
// ---------------------------------------------------------------------------

uint32_t MemberSize(const granny_data_type_definition* c_pMember)
{
    uint32_t uOne = 0;
    switch (c_pMember->Type)
    {
        case GrannyInlineMember:
            uOne = c_pMember->ReferenceType ? TypeSize(c_pMember->ReferenceType) : 0;
            break;
        case GrannyReferenceMember:
        case GrannyEmptyReferenceMember:
        case GrannyStringMember:
            uOne = 4;
            break;
        case GrannyReferenceToArrayMember:
        case GrannyArrayOfReferencesMember:
        case GrannyVariantReferenceMember:
            uOne = 8;
            break;
        case GrannyReferenceToVariantArrayMember:
            uOne = 12;
            break;
        case GrannyTransformMember:
            // flags + position(3) + orientation(4) + scale and shear(9)
            uOne = 4 + 3 * 4 + 4 * 4 + 9 * 4;
            break;
        case GrannyReal32Member:
        case GrannyInt32Member:
        case GrannyUInt32Member:
            uOne = 4;
            break;
        case GrannyInt16Member:
        case GrannyUInt16Member:
        case GrannyBinormalInt16Member:
        case GrannyNormalUInt16Member:
        case GrannyReal16Member:
            uOne = 2;
            break;
        case GrannyInt8Member:
        case GrannyUInt8Member:
        case GrannyBinormalInt8Member:
        case GrannyNormalUInt8Member:
            uOne = 1;
            break;
        default:
            uOne = 0;
            break;
    }
    const int32_t iWidth = c_pMember->ArrayWidth > 1 ? c_pMember->ArrayWidth : 1;
    return uOne * (uint32_t)iWidth;
}

namespace
{

/// TYPE SIZE CACHE (found by the layer audit).
///
/// `TypeSize` walks the whole type description and, for inline members,
/// descends recursively into nested types. The result depends **only on
/// the pointer to the type description**, and type descriptions lie in
/// the `.gr2` file and do not change.
///
/// It used to be recomputed at EVERY array index: `ArrayElement` needs only
/// a constant stride, yet it is called in index loops in eleven places of
/// `gr2_to_granny.cpp` - bones, material maps, triangle groups, material
/// bindings, bone bindings, mesh bindings and six loops in `BuildFileInfo`.
/// The largest is bones, hundreds per skeleton.
///
/// The cache is a **direct map** on the low bits of the pointer, not a
/// single cell: `TypeSize` recurses, so one cell would be clobbered by its
/// own nested call. Sixteen slots suffice - a file has a handful of vertex
/// and skeleton types.
///
/// No invalidation: type descriptions live as long as the loaded file, and
/// if a new file gets the same address it gets it ONLY after the old one
/// is freed - and then carries the same layout, being the same Granny
/// type. Should that ever stop being true, this cache is the one place to
/// touch.
const int c_iTypeCacheSize = 16;
const granny_data_type_definition* s_apcTypes[c_iTypeCacheSize] = { 0 };
uint32_t s_auSizes[c_iTypeCacheSize] = { 0 };

}  // namespace

uint32_t TypeSize(const granny_data_type_definition* c_pType)
{
    // The sum of member sizes WITHOUT any alignment. Not an oversight: the
    // exporter lays the members out so that alignment comes out by itself,
    // and the sum agrees with the `GrannyTypeSizeCheck` assertions of the
    // header - they check exactly the sum of the component sizes.
    if (!c_pType)
        return 0;

    const int iSlot =
        (int)(((uintptr_t)c_pType >> 4) & (uintptr_t)(c_iTypeCacheSize - 1));
    if (s_apcTypes[iSlot] == c_pType)
        return s_auSizes[iSlot];

    uint32_t uSum = 0;
    for (const granny_data_type_definition* p = c_pType; p->Type != GrannyEndMember; ++p)
        uSum += MemberSize(p);

    s_apcTypes[iSlot] = c_pType;
    s_auSizes[iSlot] = uSum;
    return uSum;
}

// ---------------------------------------------------------------------------
// LOOKING MEMBERS UP BY NAME
// ---------------------------------------------------------------------------

const granny_data_type_definition* FindMember(
    const granny_data_type_definition* c_pType, const char* c_szName,
    const void* c_pvObject, const uint8_t** ppbyMember)
{
    if (!c_pType || !c_pvObject)
        return NULL;
    const uint8_t* pby = (const uint8_t*)c_pvObject;
    for (const granny_data_type_definition* p = c_pType; p->Type != GrannyEndMember; ++p)
    {
        if (p->Name && strcmp(p->Name, c_szName) == 0)
        {
            if (ppbyMember)
                *ppbyMember = pby;
            return p;
        }
        pby += MemberSize(p);
    }
    return NULL;
}

const char* MemberString(const granny_data_type_definition* c_pType,
                         const void* c_pvObject, const char* c_szName)
{
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, c_pvObject, &pby);
    if (!c_pMember || c_pMember->Type != GrannyStringMember)
        return NULL;
    const char* c_szResult = NULL;
    memcpy(&c_szResult, pby, sizeof(c_szResult));
    return c_szResult;
}

int32_t MemberInt32(const granny_data_type_definition* c_pType,
                    const void* c_pvObject, const char* c_szName,
                    int32_t iDefault)
{
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, c_pvObject, &pby);
    if (!c_pMember)
        return iDefault;
    switch (c_pMember->Type)
    {
        case GrannyInt32Member:
        case GrannyUInt32Member:
        {
            int32_t i;
            memcpy(&i, pby, 4);
            return i;
        }
        case GrannyInt16Member:
        case GrannyUInt16Member:
        {
            int16_t i;
            memcpy(&i, pby, 2);
            return i;
        }
        case GrannyInt8Member:
        case GrannyUInt8Member:
            return (int32_t)*pby;
        default:
            return iDefault;
    }
}

float MemberReal32(const granny_data_type_definition* c_pType,
                   const void* c_pvObject, const char* c_szName, float fDefault)
{
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, c_pvObject, &pby);
    if (!c_pMember || c_pMember->Type != GrannyReal32Member)
        return fDefault;
    float f;
    memcpy(&f, pby, 4);
    return f;
}

bool MemberReal32Array(const granny_data_type_definition* c_pType,
                       const void* c_pvObject, const char* c_szName,
                       float* pafTarget, uint32_t uCount)
{
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, c_pvObject, &pby);
    if (!c_pMember || c_pMember->Type != GrannyReal32Member)
        return false;
    const uint32_t uHas = (uint32_t)(c_pMember->ArrayWidth > 1 ? c_pMember->ArrayWidth : 1);
    if (uHas != uCount)
        return false;
    memcpy(pafTarget, pby, uCount * 4);
    return true;
}

void* MemberReference(const granny_data_type_definition* c_pType,
                      const void* c_pvObject, const char* c_szName,
                      granny_data_type_definition** ppElementType)
{
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, c_pvObject, &pby);
    if (!c_pMember)
        return NULL;
    if (c_pMember->Type == GrannyReferenceMember ||
        c_pMember->Type == GrannyEmptyReferenceMember)
    {
        void* pv = NULL;
        memcpy(&pv, pby, sizeof(pv));
        if (ppElementType)
            *ppElementType = c_pMember->ReferenceType;
        return pv;
    }
    if (c_pMember->Type == GrannyVariantReferenceMember)
    {
        granny_data_type_definition* pType = NULL;
        void* pv = NULL;
        memcpy(&pType, pby, sizeof(pType));
        memcpy(&pv, pby + 4, sizeof(pv));
        if (ppElementType)
            *ppElementType = pType;
        return pv;
    }
    return NULL;
}

int32_t MemberArray(const granny_data_type_definition* c_pType,
                    const void* c_pvObject, const char* c_szName,
                    void** ppvArray, granny_data_type_definition** ppElementType,
                    bool* pbPointers)
{
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, c_pvObject, &pby);
    if (!c_pMember)
        return 0;
    if (c_pMember->Type != GrannyReferenceToArrayMember &&
        c_pMember->Type != GrannyArrayOfReferencesMember)
        return 0;

    int32_t iCount = 0;
    void* pv = NULL;
    memcpy(&iCount, pby, 4);
    memcpy(&pv, pby + 4, sizeof(pv));
    if (iCount <= 0 || !pv)
        return 0;

    if (ppvArray)
        *ppvArray = pv;
    if (ppElementType)
        *ppElementType = c_pMember->ReferenceType;
    if (pbPointers)
        *pbPointers = (c_pMember->Type == GrannyArrayOfReferencesMember);
    return iCount;
}

void* ArrayElement(void* pvArray, granny_data_type_definition* pElementType,
                   bool bPointers, int32_t iIndex)
{
    if (!pvArray)
        return NULL;
    if (bPointers)
    {
        void* pv = NULL;
        memcpy(&pv, (uint8_t*)pvArray + (size_t)iIndex * 4, sizeof(pv));
        return pv;
    }
    const uint32_t uSize = TypeSize(pElementType);
    if (uSize == 0)
        return NULL;
    return (uint8_t*)pvArray + (size_t)iIndex * uSize;
}

int32_t MemberVariantArray(const granny_data_type_definition* c_pType,
                           const void* c_pvObject, const char* c_szName,
                           void** ppvArray,
                           granny_data_type_definition** ppElementType)
{
    const uint8_t* pby = NULL;
    const granny_data_type_definition* c_pMember =
        FindMember(c_pType, c_szName, c_pvObject, &pby);
    if (!c_pMember || c_pMember->Type != GrannyReferenceToVariantArrayMember)
        return 0;

    // HERE IS GRANNY'S WHOLE IDEA FOR VERTICES: the array has no type up
    // front - the type is STORED BESIDE IT. That is why the same reader
    // handles a vertex with bone weights and without, with one set of
    // texture coordinates and with three.
    granny_data_type_definition* pType = NULL;
    int32_t iCount = 0;
    void* pv = NULL;
    memcpy(&pType, pby, sizeof(pType));
    memcpy(&iCount, pby + 4, 4);
    memcpy(&pv, pby + 8, sizeof(pv));
    if (iCount <= 0 || !pv || !pType)
        return 0;
    if (ppvArray)
        *ppvArray = pv;
    if (ppElementType)
        *ppElementType = pType;
    return iCount;
}

// ---------------------------------------------------------------------------
// LOADING
// ---------------------------------------------------------------------------

CFile::CFile()
    : m_pRootType(NULL), m_pvRootObject(NULL), m_c_szError("")
{
}

CFile::~CFile()
{
    for (size_t i = 0; i < m_vecSections.size(); ++i)
        free(m_vecSections[i]);
    for (size_t i = 0; i < m_vecBlocks.size(); ++i)
        free(m_vecBlocks[i]);
}

void* CFile::Allocate(size_t uBytes)
{
    if (uBytes == 0)
        uBytes = 1;
    void* pv = calloc(1, uBytes);
    if (pv)
        m_vecBlocks.push_back((uint8_t*)pv);
    return pv;
}

bool CFile::LoadSections(const uint8_t* c_pbyFile, uint32_t uSize,
                         const uint8_t* c_pbyTable, uint32_t uSections)
{
    m_vecSections.assign(uSections, NULL);
    m_vecSectionSizes.assign(uSections, 0);

    for (uint32_t i = 0; i < uSections; ++i)
    {
        const uint8_t* c_pbyEntry =
            c_pbyTable + i * c_uSectionEntrySize;
        const uint32_t uCompression = ReadU32(c_pbyEntry + 0);
        const uint32_t uDataOffset = ReadU32(c_pbyEntry + 4);
        const uint32_t uPacked = ReadU32(c_pbyEntry + 8);
        const uint32_t uUnpacked = ReadU32(c_pbyEntry + 12);
        const uint32_t uFirst16 = ReadU32(c_pbyEntry + 20);
        const uint32_t uFirst8 = ReadU32(c_pbyEntry + 24);

        m_vecSectionSizes[i] = uUnpacked;
        if (uUnpacked == 0)
            continue;
        if ((uint64_t)uDataOffset + uPacked > uSize)
        {
            m_c_szError = "section data past the end of the file";
            return false;
        }

        uint8_t* pbyTarget = (uint8_t*)calloc(1, uUnpacked);
        if (!pbyTarget)
        {
            m_c_szError = "out of memory for a section";
            return false;
        }
        m_vecSections[i] = pbyTarget;

        const uint8_t* c_pbySource = c_pbyFile + uDataOffset;
        if (uCompression == 0)
        {
            memcpy(pbyTarget, c_pbySource,
                   uPacked < uUnpacked ? uPacked : uUnpacked);
        }
        else if (uCompression == 2)
        {
            if (!DecompressSection(c_pbySource, uPacked, uFirst16, uFirst8,
                                   pbyTarget, uUnpacked))
            {
                m_c_szError = LastError();
                return false;
            }
        }
        else
        {
            m_c_szError = "unsupported section compression";
            return false;
        }
    }
    return true;
}

bool CFile::ApplyRelocations(const uint8_t* c_pbyFile, uint32_t uSize,
                             const uint8_t* c_pbyTable, uint32_t uSections)
{
    // RELOCATIONS. In the file the pointers are ZEROED - an address that
    // does not exist yet cannot be stored. Instead every section has a
    // table of triples (offset in me, target section, offset there), and
    // the loader writes the real address at the indicated place. The
    // tables lie in the file UNCOMPRESSED, at an absolute offset.
    for (uint32_t i = 0; i < uSections; ++i)
    {
        const uint8_t* c_pbyEntry =
            c_pbyTable + i * c_uSectionEntrySize;
        const uint32_t uRelocationOffset = ReadU32(c_pbyEntry + 28);
        const uint32_t uRelocations = ReadU32(c_pbyEntry + 32);
        if (uRelocations == 0)
            continue;
        if ((uint64_t)uRelocationOffset + (uint64_t)uRelocations * c_uRelocationSize
            > uSize)
        {
            m_c_szError = "relocation table past the end of the file";
            return false;
        }
        for (uint32_t j = 0; j < uRelocations; ++j)
        {
            const uint8_t* c_pbyRelocation =
                c_pbyFile + uRelocationOffset + j * c_uRelocationSize;
            const uint32_t uFrom = ReadU32(c_pbyRelocation + 0);
            const uint32_t uToSection = ReadU32(c_pbyRelocation + 4);
            const uint32_t uToOffset = ReadU32(c_pbyRelocation + 8);
            if (uToSection >= uSections || !m_vecSections[i] || !m_vecSections[uToSection])
            {
                m_c_szError = "relocation points at a section that does not exist";
                return false;
            }
            if (uFrom + 4 > m_vecSectionSizes[i] ||
                uToOffset > m_vecSectionSizes[uToSection])
            {
                m_c_szError = "relocation past the end of a section";
                return false;
            }
            uint8_t* pbyTarget = m_vecSections[uToSection] + uToOffset;
            memcpy(m_vecSections[i] + uFrom, &pbyTarget, sizeof(pbyTarget));
        }
    }
    return true;
}

bool CFile::Load(const uint8_t* c_pbyFile, uint32_t uSize)
{
    if (uSize < 64)
    {
        m_c_szError = "file shorter than the header";
        return false;
    }
    if (memcmp(c_pbyFile, c_abyMagicLE32, sizeof(c_abyMagicLE32)) != 0)
    {
        m_c_szError = "not a little-endian 32-bit Granny file";
        return false;
    }

    const uint32_t uHeaderFormat = ReadU32(c_pbyFile + 20);
    if (uHeaderFormat != 0)
    {
        m_c_szError = "the file header is compressed";
        return false;
    }

    // The header proper starts RIGHT AFTER the magic and four fields, i.e.
    // at offset 32. The offset of the SECTION TABLE counts from there - but
    // the DATA offset of every section is ABSOLUTE. Mixing them up gives no
    // error, it just reads random bytes as an Oodle1 header, and nonsense
    // alphabet sizes come out of them.
    const uint32_t uBase = 32;
    const uint32_t uVersion = ReadU32(c_pbyFile + uBase);
    if (uVersion != 6)
    {
        m_c_szError = "unsupported .gr2 format version";
        return false;
    }
    const uint32_t uSectionOffset = ReadU32(c_pbyFile + uBase + 12);
    const uint32_t uSections = ReadU32(c_pbyFile + uBase + 16);
    const uint32_t uTypeSection = ReadU32(c_pbyFile + uBase + 20);
    const uint32_t uTypeOffset = ReadU32(c_pbyFile + uBase + 24);
    const uint32_t uObjectSection = ReadU32(c_pbyFile + uBase + 28);
    const uint32_t uObjectOffset = ReadU32(c_pbyFile + uBase + 32);

    if (uSections == 0 || uSections > 64)
    {
        m_c_szError = "nonsensical section count";
        return false;
    }
    const uint32_t uTableEnd =
        uBase + uSectionOffset + uSections * c_uSectionEntrySize;
    if (uTableEnd > uSize)
    {
        m_c_szError = "section table past the end of the file";
        return false;
    }

    const uint8_t* c_pbyTable = c_pbyFile + uBase + uSectionOffset;
    if (!LoadSections(c_pbyFile, uSize, c_pbyTable, uSections))
        return false;
    if (!ApplyRelocations(c_pbyFile, uSize, c_pbyTable, uSections))
        return false;

    if (uTypeSection >= uSections || uObjectSection >= uSections ||
        !m_vecSections[uTypeSection] || !m_vecSections[uObjectSection])
    {
        m_c_szError = "root points at a section that does not exist";
        return false;
    }
    m_pRootType = (granny_data_type_definition*)
        (m_vecSections[uTypeSection] + uTypeOffset);
    m_pvRootObject = m_vecSections[uObjectSection] + uObjectOffset;
    return true;
}

}  // namespace m2wgr2
