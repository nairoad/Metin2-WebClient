// SPDX-License-Identifier: GPL-2.0-or-later
// webfs.h - the M2WF corpus: parser of `manifest.bin` and assembly of a
// file from one or more chunks, with the chunk source injected
// (`IChunkSource`) so the same code runs under a local test and in the
// browser (webfs_web.cpp).

// Design:
// WRITTEN FROM THE FORMAT: the acceptance criterion is "open every file of
// the corpus and produce its bytes exactly" (tools/check_corpus.py checks
// all of them). The manifest format (tools/build_corpus.py writes it):
//
//     header, 28 B:
//       0   char[4]  'M2WF'
//       4   uint32   version (must be 1)
//       8   uint32   unused
//       12  uint32   chunkCount
//       16  uint32   packCount
//       20  uint32   fileCount
//       24  uint32   bootChunks
//     28                    chunk table: chunkCount x (16 B hash + uint32 size)
//     28 + 20*chunkCount    pack table:  packCount  x (uint16 len + name)
//     then                  file table:  fileCount  x (uint16 pack, uint32 chunk,
//                                        uint32 off, uint32 size,
//                                        uint16 namelen + name)
//
// Chunk data is RAW: a file is the slice [off, off+size) of the chunk whose
// index the record names - no client-side compression, no `offset /
// chunkSize` arithmetic (an assumption of README 1.3, refuted). A file may
// CROSS a chunk boundary; the rest lies from offset 0 in the chunks with
// the following indices (confirmed by `chunk_span()` in the working
// extractor). The error messages were read from the binary (`func_11166`)
// and are part of the observed contract.
//
// What is a design choice (not recoverable): the C++ API. In the binary
// `WebFs.cpp` is two functions - the parser (called once from `main()`)
// and a `getFile` bridge to JS with no caller in the code section - so the
// call surface on the C++ side could not be recovered; if it ever is, only
// this layer changes.
//
// Case: lookup MUST ignore case, or names must be
// lowered before asking. Proof on the data: `map_a2/setting.txt` says
// `TextureSet textureset\metin2_A2.txt` with a capital A2, the manifest has
// `textureset/metin2_a2.txt`, and of 54 955 names only 31 contain any
// capitals - all garbled Korean, none ASCII. So `Find()` with a name
// straight from a data file finds nothing. The rule for a data-file path:
// cut the drive prefix, `\` -> `/`, lower case. `Find()` stays EXACT
// (cheaper, predictable); the caller passes the converted name, and
// `FindNoCase()` remains as a safety net.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace webfs {

/// A chunk: 16-byte hash (the file on the server is `<hash in hex>.bin`)
/// and size.
struct ChunkInfo {
    uint8_t  hash[16];
    uint32_t size;
};

/// A file record. `chunk`/`offset` point at the START; a file crossing the
/// chunk boundary continues from offset 0 in the following chunks.
struct FileEntry {
    std::string name;
    uint16_t    pack;
    uint32_t    chunk;
    uint32_t    offset;
    uint32_t    size;
};

/// Where chunk content comes from. The local implementation reads
/// `<hash>.bin` from disk (acceptance test); the browser one goes through
/// the JS bridge.
class IChunkSource {
public:
    /// Virtual, so a source can be deleted through the interface.
    virtual ~IChunkSource() {}
    /// Writes `length` bytes of chunk `chunkIndex`, starting at
    /// `offsetInChunk`, to `dest`. False when the chunk cannot be delivered.
    virtual bool ReadChunk(uint32_t chunkIndex, uint32_t offsetInChunk,
                           uint32_t length, void* dest) = 0;
};

/// The parsed manifest and file assembly.
class WebFs {
public:
    /// An empty corpus (version 0, no chunks/packs/files) until `ParseManifest`.
    WebFs();

    /// Parses the manifest. On error returns false and puts the reason in
    /// `error`. Every read is length-checked BEFORE it happens - the
    /// manifest comes from the network, so a truncated file is a normal
    /// case, not an exception.
    bool ParseManifest(const void* data, size_t length, std::string* error);

    /// File index or -1. EXACT comparison (see the note on case).
    int  Find(const char* name) const;
    /// Case-insensitive variant - explicit, since it is not known which is
    /// the right one.
    int  FindNoCase(const char* name) const;

    /// Assembles a file from one or more chunks; `out` receives exactly
    /// `size` bytes.
    bool ReadFile(int fileIndex, IChunkSource& source, std::vector<uint8_t>* out) const;

    /// Manifest format version from the header (1 after a successful parse).
    uint32_t Version()    const { return m_version; }
    /// Header field @24 `bootChunks`, stored as read; nothing in the port uses it.
    uint32_t BootChunks() const { return m_bootChunks; }

    /// The chunk table (hash and size per chunk), in manifest order.
    const std::vector<ChunkInfo>&  Chunks() const { return m_chunks; }
    /// The pack names, in manifest order (`FileEntry::pack` indexes this).
    const std::vector<std::string>& Packs() const { return m_packs; }
    /// The file table, in manifest order (the index `Find` returns).
    const std::vector<FileEntry>&  Files()  const { return m_files; }

    /// The chunk hash as 32 hex characters - the file on the server is
    /// `<this>.bin`.
    static std::string HashToHex(const uint8_t hash[16]);

private:
    uint32_t                 m_version;
    uint32_t                 m_bootChunks;
    std::vector<ChunkInfo>   m_chunks;
    std::vector<std::string> m_packs;
    std::vector<FileEntry>   m_files;
};

} // namespace webfs
