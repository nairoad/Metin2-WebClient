// SPDX-License-Identifier: GPL-2.0-or-later
// webfs.cpp - M2WF manifest parser and file assembly (see webfs.h).

#include "webfs.h"

#include <cstring>
#include <cstdio>
#include <cstdarg>

namespace webfs {

namespace {

/// Bounds-checked reader. The manifest comes from the network, so every
/// read must be checked - a truncated file is a normal case. The class
/// exists so that the check cannot be skipped by accident.
class Reader {
public:
    /// Reads the `n` bytes at `p`, starting at offset 0.
    Reader(const uint8_t* p, size_t n) : m_p(p), m_n(n), m_o(0) {}

    /// True when `count` more bytes are left. The readers below do NOT check -
    /// call this first.
    bool Need(size_t count) const { return m_o + count <= m_n; }
    /// Current read offset from the start of the buffer.
    size_t Offset() const { return m_o; }

    /// Little-endian uint16 at the current offset; advances by 2 (unchecked).
    uint16_t U16() {
        uint16_t v = static_cast<uint16_t>(m_p[m_o]) |
                     static_cast<uint16_t>(m_p[m_o + 1]) << 8;
        m_o += 2;
        return v;
    }
    /// Little-endian uint32 at the current offset; advances by 4 (unchecked).
    uint32_t U32() {
        uint32_t v = static_cast<uint32_t>(m_p[m_o]) |
                     static_cast<uint32_t>(m_p[m_o + 1]) << 8 |
                     static_cast<uint32_t>(m_p[m_o + 2]) << 16 |
                     static_cast<uint32_t>(m_p[m_o + 3]) << 24;
        m_o += 4;
        return v;
    }
    /// Copies `count` bytes to `dest` and advances (unchecked).
    void Bytes(void* dest, size_t count) {
        std::memcpy(dest, m_p + m_o, count);
        m_o += count;
    }
    /// Advances by `count` bytes without reading (unchecked).
    void Skip(size_t count) { m_o += count; }

private:
    const uint8_t* m_p;
    size_t         m_n;
    size_t         m_o;
};

/// `printf` into a std::string; the result is cut at 255 characters (the
/// error messages of the parser are short).
std::string Format(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

/// Lower-cases A-Z only; every other byte (including non-ASCII) unchanged.
char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

WebFs::WebFs() : m_version(0), m_bootChunks(0) {}

std::string WebFs::HashToHex(const uint8_t hash[16]) {
    static const char* kHex = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[i * 2]     = kHex[hash[i] >> 4];
        out[i * 2 + 1] = kHex[hash[i] & 0x0f];
    }
    return out;
}

namespace {

/// Reads `packCount` pack names (u16 length + bytes each) into `packs`;
/// false with the "truncated in the pack table" message when the data ends.
bool ReadPackTable(Reader& r, uint32_t packCount, std::vector<std::string>& packs,
                   std::string* error) {
    packs.resize(packCount);
    for (uint32_t i = 0; i < packCount; ++i) {
        if (!r.Need(2)) {
            if (error) {
                *error = Format("WebFs: manifest truncated in the pack table at %u of %u.",
                                i, packCount);
            }
            return false;
        }
        const uint16_t nameLength = r.U16();
        if (!r.Need(nameLength)) {
            if (error) {
                *error = Format("WebFs: manifest truncated in the pack table at %u of %u.",
                                i, packCount);
            }
            return false;
        }
        packs[i].resize(nameLength);
        if (nameLength) r.Bytes(&packs[i][0], nameLength);
    }
    return true;
}

/// Reads `fileCount` file entries (pack u16, chunk, offset, size u32, name
/// u16 length + bytes) into `files`; false with the "truncated in the file
/// table" message when the data ends.
bool ReadFileTable(Reader& r, uint32_t fileCount, std::vector<FileEntry>& files,
                   std::string* error) {
    files.resize(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i) {
        if (!r.Need(16)) {
            if (error) {
                *error = Format("WebFs: manifest truncated in the file table at %u of %u.",
                                i, fileCount);
            }
            return false;
        }
        FileEntry& f = files[i];
        f.pack   = r.U16();
        f.chunk  = r.U32();
        f.offset = r.U32();
        f.size   = r.U32();
        const uint16_t nameLength = r.U16();
        if (!r.Need(nameLength)) {
            if (error) {
                *error = Format("WebFs: manifest truncated in the file table at %u of %u.",
                                i, fileCount);
            }
            return false;
        }
        f.name.resize(nameLength);
        if (nameLength) r.Bytes(&f.name[0], nameLength);
    }
    return true;
}

} // namespace

bool WebFs::ParseManifest(const void* data, size_t length, std::string* error) {
    m_chunks.clear();
    m_packs.clear();
    m_files.clear();
    m_version = 0;
    m_bootChunks = 0;

    const uint8_t* p = static_cast<const uint8_t*>(data);
    Reader r(p, length);

    if (!r.Need(28)) {
        if (error) *error = "WebFs: manifest shorter than the 28-byte header.";
        return false;
    }
    if (std::memcmp(p, "M2WF", 4) != 0) {
        if (error) *error = "WebFs: manifest magic is not 'M2WF'.";
        return false;
    }
    r.Skip(4);

    m_version = r.U32();
    if (m_version != 1) {
        if (error) *error = Format("WebFs: unsupported manifest version %u.", m_version);
        return false;
    }
    r.Skip(4);                              // field @8 - unused
    const uint32_t chunkCount = r.U32();
    const uint32_t packCount  = r.U32();
    const uint32_t fileCount  = r.U32();
    m_bootChunks              = r.U32();

    // --- chunk table: fixed 20 B records, one check for the whole ---
    if (!r.Need(static_cast<size_t>(chunkCount) * 20)) {
        if (error) {
            *error = Format("WebFs: manifest truncated in the chunk table "
                            "(%u chunks declared).", chunkCount);
        }
        return false;
    }
    m_chunks.resize(chunkCount);
    for (uint32_t i = 0; i < chunkCount; ++i) {
        r.Bytes(m_chunks[i].hash, 16);
        m_chunks[i].size = r.U32();
    }

    // --- pack table: VARIABLE-length records, a check at each ---
    if (!ReadPackTable(r, packCount, m_packs, error))
        return false;

    // --- file table: 16 B fixed part + name ---
    if (!ReadFileTable(r, fileCount, m_files, error))
        return false;

    return true;
}

int WebFs::Find(const char* name) const {
    for (size_t i = 0; i < m_files.size(); ++i) {
        if (m_files[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int WebFs::FindNoCase(const char* name) const {
    for (size_t i = 0; i < m_files.size(); ++i) {
        const std::string& candidate = m_files[i].name;
        size_t k = 0;
        for (; k < candidate.size() && name[k]; ++k) {
            if (LowerAscii(candidate[k]) != LowerAscii(name[k])) break;
        }
        if (k == candidate.size() && name[k] == '\0') return static_cast<int>(i);
    }
    return -1;
}

bool WebFs::ReadFile(int fileIndex, IChunkSource& source, std::vector<uint8_t>* out) const {
    if (fileIndex < 0 || static_cast<size_t>(fileIndex) >= m_files.size()) return false;
    const FileEntry& f = m_files[fileIndex];

    out->resize(f.size);
    if (f.size == 0) return true;

    // A file may reach beyond one chunk - the rest starts at offset 0 in the
    // chunk with the next index. The loop does NOT assume one hop suffices.
    uint32_t remaining = f.size;
    uint32_t chunkIndex = f.chunk;
    uint32_t offsetInChunk = f.offset;
    uint8_t* dest = &(*out)[0];

    while (remaining > 0) {
        if (chunkIndex >= m_chunks.size()) return false;
        const uint32_t chunkSize = m_chunks[chunkIndex].size;
        if (offsetInChunk > chunkSize) return false;

        uint32_t take = chunkSize - offsetInChunk;
        if (take > remaining) take = remaining;
        // Zero progress would be an endless loop - an error beats a hang.
        if (take == 0) return false;

        if (!source.ReadChunk(chunkIndex, offsetInChunk, take, dest)) return false;

        dest      += take;
        remaining -= take;
        chunkIndex += 1;
        offsetInChunk = 0;
    }
    return true;
}

} // namespace webfs
