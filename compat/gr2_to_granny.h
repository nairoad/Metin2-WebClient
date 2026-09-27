// SPDX-License-Identifier: GPL-2.0-or-later
// gr2_to_granny.h - conversion of an unpacked `.gr2` image into the
// `granny.h` structures TMP4 compiles against: members picked BY NAME,
// never by casting the buffer.

// Design:
// WHY CONVERSION AND NOT A CAST. It was tempting to take the unpacked
// buffer and cast it straight to `granny_mesh` or `granny_skeleton`: wasm32
// has four-byte pointers, exactly the ones in a 32-bit file, so "everything
// would fit". It does not. Measured (file layouts against the header): the corpus
// files came out of Granny 2.4.0.7 and `granny.h`, which TMP4 compiles
// against, is from a later SDK with fields the files do not have:
//   granny_skeleton  header: Name BoneCount Bones LODType ExtendedData
//                    file:   Name BoneCount Bones
//   granny_bone      header: ... InverseWorld4x4 LODError ExtendedData
//                    file:   ... InverseWorldTransform LightInfo CameraInfo
//                            ExtendedData
// A cast would read `LODError` out of `LightInfo` - and nobody would find
// out, because those are numbers, not pointers. The same shape of fault as
// in the whole chain: a constant where the contract
// carries meaning. So members are picked BY NAME, as
// `GrannyConvertSingleObject` does. The side effect is what the user asked
// for: a file from another server, another exporter, with an extra member
// - passes. A missing member stays zero, an extra one is skipped.

#pragma once

#include "gr2_file.h"

namespace m2wgr2 {

/// Builds a `granny_file_info` in the `granny.h` layout out of `rFile`'s
/// memory. NULL when the file has no root or memory runs out.
granny_file_info* BuildFileInfo(CFile& rFile);

/// How many entries a type description has, INCLUDING the closing one.
/// TMP4 divides the result of `GrannyGetTotalTypeSize` by 32 and walks the
/// rows of the description, so this is what it really uses.
int32_t TypeEntryCount(const granny_data_type_definition* c_pType);

}  // namespace m2wgr2
