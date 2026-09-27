// SPDX-License-Identifier: GPL-2.0-or-later
// webfs_web.cpp - GAME DATA FETCHED ON DEMAND, not up front: the corpus
// (manifest.bin + 4 MB chunks named by their hash) is read through the
// M2WF parser of webfs.h, chunks come from the page memory / Cache Storage
// / a synchronous XHR (runtime.js, m2w.chunk*), and a fetched file lands
// in MEMFS under the name the client asked for.

// Design:
// Earlier the whole game went in through `--preload-file`: one
// 1.86 GB file that must download BEFORE the first frame and sits whole on
// the heap: measured on the login screen, heap 2018 MB of 4096. Streaming
// does not KEEP the data: a chunk is fetched when something reads from it.
// A half-full heap is not a failure
// by itself, but garbage collection stops being short there, and a long
// pause looks exactly like the stutter the user reported.
//
// The corpus is `manifest.bin` plus a few hundred chunks of 4 MB named by
// the hash of their content (the format: webfs.h). This file adds the chunk
// SOURCE (synchronous XMLHttpRequest - m2w.fetchSync) and the BRIDGE TO THE
// FILE SYSTEM: the rest of
// the port knows nothing - `fopen`, `CreateFile` and `_access` go through
// `M2W_ResolvePathCase` (paths_web.cpp), which calls here only when the
// file really is not there. What stays in the package: our Python 3
// scripts, CPython's standard library and settings - the corpus does not
// know them; game content (models, textures, maps, sounds) is streamed.
//
// Two layers of chunk memory, page side first (300w - the sliding
// prefetch window and the in-flight limits are explained in runtime.js),
// then a small LRU of sixteen chunks in C++ (a file can cross a chunk
// boundary and neighbours lie in the same chunk, so without it one model
// would cost several 4 MB fetches).

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include "webfs.h"
#include "frame_stats.h"

namespace
{

/// Synchronous download into a `malloc` buffer (m2w.fetchSync); length
/// under `piLength`, NULL on failure. The caller frees it.
EM_JS(char*, m2w_fetch, (const char* c_szUrl, int* piLength, int iExpected), {
    return m2w.heapBytes(m2w.fetchSync(UTF8ToString(c_szUrl), iExpected), piLength);
});

/// Page-side chunk memory with a budget in MB (m2w.chunkCacheStart).
EM_JS(void, m2w_chunk_cache_start, (int iBudgetMB), { m2w.chunkCacheStart(iBudgetMB); });

/// Chunk from the page memory into a `malloc` buffer, or NULL
/// (m2w.chunkFromCache).
EM_JS(char*, m2w_chunk_from_cache, (const char* c_szName, int* piLength), {
    return m2w.heapBytes(m2w.chunkFromCache(UTF8ToString(c_szName)), piLength);
});

/// Puts a chunk fetched blocking into the page memory (m2w.chunkCachePut).
EM_JS(void, m2w_chunk_cache_put, (const char* c_szName, const char* c_pData, int iLength), {
    if (iLength <= 0) return;
    m2w.chunkCachePut(UTF8ToString(c_szName), HEAPU8.slice(c_pData, c_pData + iLength));
});

/// Background fetch of a chunk (m2w.chunkInBackground).
EM_JS(void, m2w_chunk_in_background, (const char* c_szUrl, const char* c_szName), {
    m2w.chunkInBackground(UTF8ToString(c_szUrl), UTF8ToString(c_szName));
});

/// Prefetch in the recorded order (m2w.prefetchStart).
EM_JS(void, m2w_prefetch_start, (const char* c_szBase), { m2w.prefetchStart(UTF8ToString(c_szBase)); });

/// Hands the page the list of all chunk hashes (m2w.chunkList).
EM_JS(void, m2w_chunk_list, (const char* c_szList, const char* c_szBase), {
    m2w.chunkList(UTF8ToString(c_szList), UTF8ToString(c_szBase));
});

/// Counters for the report (m2w.chunkCounter). No caller today (git grep)
/// - kept for the F12 console.
EM_JS(int, m2w_chunk_cache_counter, (int iWhat), { return m2w.chunkCounter(iWhat); });

/// Chunk memory budget in MB (m2w.corpusBudget).
EM_JS(int, m2w_corpus_budget, (void), { return m2w.corpusBudget(); });

/// Corpus address into a `malloc` string (m2w.corpusUrl).
EM_JS(char*, m2w_corpus_url, (void), { return m2w.heapString(m2w.corpusUrl()); });

/// Records the first use of a chunk (m2w.chunkUsed).
EM_JS(void, m2w_chunk_used, (int iChunk), { m2w.chunkUsed(iChunk); });

// ---------------------------------------------------------------------------
// Chunk source
// ---------------------------------------------------------------------------

/// Chunks kept in C++ - at 4 MB each a few dozen megabytes.
const size_t c_uChunksInMemory = 16;

struct TChunk
{
    std::vector<unsigned char> vecData;
    unsigned long ulLastUsed = 0;
};

/// `IChunkSource` over the page memory and the network, with an LRU of
/// `c_uChunksInMemory` chunks.
class CNetworkChunkSource : public webfs::IChunkSource
{
public:
    /// A source reading chunks from `c_rkBase` (the corpus URL prefix); no chunk
    /// table until `SetHashes`.
    explicit CNetworkChunkSource(const std::string& c_rkBase) : m_kBase(c_rkBase) {}

    bool ReadChunk(uint32_t uChunk, uint32_t uOffset, uint32_t uLength, void* pvTarget) override
    {
        const std::vector<unsigned char>* c_pData = Get(uChunk);
        if (!c_pData)
            return false;
        if ((size_t)uOffset + uLength > c_pData->size())
            return false;
        std::memcpy(pvTarget, &(*c_pData)[uOffset], uLength);
        return true;
    }

    /// Points the source at the manifest's chunk table (names `<hash>.bin`); the
    /// vector is not copied and must outlive the source.
    void SetHashes(const std::vector<webfs::ChunkInfo>* c_pHashes)
    {
        m_c_pHashes = c_pHashes;
    }

    /// How many chunks this source has fetched from the network so far.
    unsigned long Fetched() const { return m_ulFetched; }

private:
    /// The chunk's bytes: from the LRU, else from the page memory, else
    /// fetched blocking. Also starts the background fetch of the next three
    /// (files lie in the corpus in pack order and the game reads them in
    /// groups - a model, its textures, the neighbouring map chunk; three,
    /// not thirty, because deeper prefetch would evict what the game is
    /// reading now; and at most two of these background fetches are in
    /// flight - `m2w.chunkInBackground` in runtime.js).
    const std::vector<unsigned char>* Get(uint32_t uChunk)
    {
        ++m_ulClock;

        std::map<uint32_t, TChunk>::iterator it = m_kChunks.find(uChunk);
        if (it != m_kChunks.end())
        {
            it->second.ulLastUsed = m_ulClock;
            return &it->second.vecData;
        }

        if (!m_c_pHashes || uChunk >= m_c_pHashes->size())
            return NULL;

        const std::string kName = webfs::WebFs::HashToHex((*m_c_pHashes)[uChunk].hash) + ".bin";
        const std::string kUrl = m_kBase + kName;

        m2w_chunk_used((int)uChunk);

        // Page memory first: a chunk the background fetch already
        // brought costs no round trip - the whole difference between a
        // 16 ms frame and a one-second one.
        int iLength = 0;
        char* pBuffer = m2w_chunk_from_cache(kName.c_str(), &iLength);
        if (!pBuffer || iLength <= 0)
        {
            if (pBuffer)
                free(pBuffer);
            iLength = 0;
            pBuffer = m2w_fetch(kUrl.c_str(), &iLength, (int)(*m_c_pHashes)[uChunk].size);
            if (pBuffer && iLength > 0)
                m2w_chunk_cache_put(kName.c_str(), pBuffer, iLength);
        }
        if (!pBuffer || iLength <= 0)
        {
            if (pBuffer)
                free(pBuffer);
            std::printf("m2w corpus: cannot fetch chunk %u (%s)\n", (unsigned)uChunk, kUrl.c_str());
            return NULL;
        }
        ++m_ulFetched;

        // Room is made BEFORE inserting, so the new chunk is never evicted.
        while (m_kChunks.size() >= c_uChunksInMemory)
            EvictOldest();

        TChunk& rNew = m_kChunks[uChunk];
        rNew.vecData.assign((unsigned char*)pBuffer, (unsigned char*)pBuffer + iLength);
        rNew.ulLastUsed = m_ulClock;
        free(pBuffer);

        for (uint32_t i = 1; i <= 3; ++i)
        {
            const uint32_t uNext = uChunk + i;
            if (uNext >= m_c_pHashes->size())
                break;
            const std::string kNextName = webfs::WebFs::HashToHex((*m_c_pHashes)[uNext].hash) + ".bin";
            m2w_chunk_in_background((m_kBase + kNextName).c_str(), kNextName.c_str());
        }
        return &rNew.vecData;
    }

    /// Drops the least recently used chunk from the in-memory LRU (linear scan;
    /// does nothing when it is empty).
    void EvictOldest()
    {
        std::map<uint32_t, TChunk>::iterator itOldest = m_kChunks.end();
        for (std::map<uint32_t, TChunk>::iterator it = m_kChunks.begin(); it != m_kChunks.end(); ++it)
        {
            if (itOldest == m_kChunks.end() || it->second.ulLastUsed < itOldest->second.ulLastUsed)
                itOldest = it;
        }
        if (itOldest != m_kChunks.end())
            m_kChunks.erase(itOldest);
    }

    std::string m_kBase;
    const std::vector<webfs::ChunkInfo>* m_c_pHashes = NULL;
    std::map<uint32_t, TChunk> m_kChunks;
    unsigned long m_ulClock = 0;
    unsigned long m_ulFetched = 0;
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

webfs::WebFs* g_pCorpus = NULL;
CNetworkChunkSource* g_pSource = NULL;
bool g_bTried = false;
unsigned long g_ulDelivered = 0;

/// Index by name, built once from the manifest - `WebFs::FindNoCase` walks
/// all 54 955 entries (half a million character comparisons per question),
/// and the client asks for files by the thousand, including ones that do
/// not exist (localised versions of resources).
std::map<std::string, int> g_kIndex;

/// The name as the corpus spells it: no drive letter, no leading slash,
/// lower case, forward slashes.
std::string CorpusName(const char* c_szPath)
{
    std::string k(c_szPath ? c_szPath : "");
    if (k.size() > 2 && k[1] == ':')
        k = k.substr(3);
    while (!k.empty() && k[0] == '/')
        k = k.substr(1);
    for (size_t i = 0; i < k.size(); ++i)
    {
        if (k[i] == '\\')
            k[i] = '/';
        else if (k[i] >= 'A' && k[i] <= 'Z')
            k[i] = (char)(k[i] - 'A' + 'a');
    }
    return k;
}

/// Creates the directories on the way to a file about to be written.
void CreateDirectories(const std::string& c_rkPath)
{
    for (size_t i = 1; i < c_rkPath.size(); ++i)
    {
        if (c_rkPath[i] != '/')
            continue;
        const std::string kSoFar = c_rkPath.substr(0, i);
        EM_ASM({
            try { FS.mkdir(UTF8ToString($0)); } catch (e) { }
        }, kSoFar.c_str());
    }
}

/// Fetches and parses the manifest on the first call; true when the corpus
/// is available.
bool Started()
{
    if (g_bTried)
        return g_pCorpus != NULL;
    g_bTried = true;

    char* pszBase = m2w_corpus_url();
    const std::string kBase(pszBase ? pszBase : "corpus/");
    if (pszBase)
        free(pszBase);

    // `manifest.bin` is NOT content-addressed (chunks are: name = hash of
    // the content, safely cacheable forever) - one fixed URL whose content
    // changes with every corpus rebuild. Measured: the browser
    // kept an OLD manifest (2 983 523 B) while the disk had a NEW one
    // (2 983 543 B), the client asked for chunks by the old list and got
    // 404. A time stamp keeps THIS ONE address out of the cache; chunks are
    // not affected. (A separate, real caching problem after a rebuild - NOT
    // the cause of the "corrupted heap (address zero)" crash, which was LZO.)
    const std::string kManifest = kBase + "manifest.bin?t=" + std::to_string((long long)time(NULL));
    int iLength = 0;
    char* pBuffer = m2w_fetch(kManifest.c_str(), &iLength, 0);   // size unknown up front
    if (!pBuffer || iLength <= 0)
    {
        if (pBuffer)
            free(pBuffer);
        std::printf("m2w corpus: no %s - streaming disabled\n", kManifest.c_str());
        return false;
    }

    g_pCorpus = new webfs::WebFs();
    std::string kError;
    const bool bOk = g_pCorpus->ParseManifest(pBuffer, (size_t)iLength, &kError);
    free(pBuffer);

    if (!bOk)
    {
        std::printf("m2w corpus: manifest unreadable - %s\n", kError.c_str());
        delete g_pCorpus;
        g_pCorpus = NULL;
        return false;
    }

    // The page memory must exist BEFORE the first chunk is asked for.
    const int iBudgetMB = m2w_corpus_budget();
    m2w_chunk_cache_start(iBudgetMB);
    std::printf("m2w corpus: chunk memory %d MB (change with ?corpusMB=N)\n", iBudgetMB);

    g_pSource = new CNetworkChunkSource(kBase);
    g_pSource->SetHashes(&g_pCorpus->Chunks());
    m2w_prefetch_start(kBase.c_str());

    // All chunk hashes for the page's background download.
    {
        const std::vector<webfs::ChunkInfo>& c_rkChunks = g_pCorpus->Chunks();
        std::string kList;
        kList.reserve(c_rkChunks.size() * 33);
        for (size_t i = 0; i < c_rkChunks.size(); ++i)
        {
            if (i) kList += '\n';
            kList += webfs::WebFs::HashToHex(c_rkChunks[i].hash);
        }
        m2w_chunk_list(kList.c_str(), kBase.c_str());
    }

    const std::vector<webfs::FileEntry>& c_rkFiles = g_pCorpus->Files();
    for (size_t i = 0; i < c_rkFiles.size(); ++i)
    {
        std::string kKey(c_rkFiles[i].name);
        for (size_t j = 0; j < kKey.size(); ++j)
            if (kKey[j] >= 'A' && kKey[j] <= 'Z')
                kKey[j] = (char)(kKey[j] - 'A' + 'a');
        g_kIndex[kKey] = (int)i;
    }

    std::printf("m2w corpus: %u files in %u chunks, address %s\n",
                (unsigned)g_pCorpus->Files().size(),
                (unsigned)g_pCorpus->Chunks().size(), kBase.c_str());
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points for the path layer (paths_web.cpp)
// ---------------------------------------------------------------------------

/// Whether the corpus has this file - answered from the manifest, nothing
/// is fetched. Same as `M2W_CorpusKnows`; no caller today (git grep).
bool M2W_CorpusHas(const char* c_szPath)
{
    if (!Started())
        return false;
    return g_kIndex.find(CorpusName(c_szPath)) != g_kIndex.end();
}

/// Whether the corpus is available (manifest fetched and parsed).
bool M2W_CorpusStarted()
{
    return Started();
}

/// Whether the corpus KNOWS this name - without fetching anything. Needed
/// to tell "the file does not exist" (true, may be cached) from "it is in
/// the index but was not delivered" (a failure that must not be cached) -
/// paths_web.cpp; the rule is and is recorded
/// (item icons).
bool M2W_CorpusKnows(const char* c_szPath)
{
    if (!Started() || !c_szPath)
        return false;
    return g_kIndex.find(CorpusName(c_szPath)) != g_kIndex.end();
}

/// Fetches the file and writes it to the file system under `c_szWhere`.
/// False when the corpus does not have it or the fetch failed.
bool M2W_CorpusGet(const char* c_szPath, const char* c_szWhere)
{
    if (!Started())
        return false;

    const std::string kName = CorpusName(c_szPath);
    const std::map<std::string, int>::const_iterator it = g_kIndex.find(kName);
    if (it == g_kIndex.end())
    {
        // The first twenty unknown names are printed. `icon/item/70038.tga`
        // reported missing looked like a data mismatch between the corpus
        // and the user's server; a corpus built from the
        // user's client had the icon too - hypothesis refuted by
        // measurement. When the file is in the index and the client does
        // not get it, the error is in the NAME it asks by, and only
        // printing the name settles that.
        static int s_iPrinted = 0;
        if (s_iPrinted < 20)
        {
            ++s_iPrinted;
            std::printf("m2w corpus: UNKNOWN [%s] (asked for [%s])\n",
                        kName.c_str(), c_szPath ? c_szPath : "");
        }
        return false;
    }
    const int iFile = it->second;

    m2wstats::TStopwatch kStopwatch(m2wstats::g_kCorpusFetch);

    std::vector<uint8_t> vecContent;
    if (!g_pCorpus->ReadFile(iFile, *g_pSource, &vecContent))
    {
        std::printf("m2w corpus: %s is in the index but cannot be assembled\n", c_szPath);
        return false;
    }

    const std::string kWhere(c_szWhere ? c_szWhere : "");
    CreateDirectories(kWhere);

    std::FILE* pFile = std::fopen(kWhere.c_str(), "wb");
    if (!pFile)
    {
        std::printf("m2w corpus: cannot write %s\n", kWhere.c_str());
        return false;
    }
    if (!vecContent.empty())
        std::fwrite(&vecContent[0], 1, vecContent.size(), pFile);
    std::fclose(pFile);

    ++g_ulDelivered;
    if (g_ulDelivered == 1 || g_ulDelivered % 200 == 0)
        std::printf("m2w corpus: %lu files delivered, %lu chunks fetched\n",
                    g_ulDelivered, g_pSource->Fetched());
    return true;
}
