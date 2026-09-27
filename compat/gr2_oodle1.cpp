// SPDX-License-Identifier: GPL-2.0-or-later
// gr2_oodle1.cpp - the Oodle1 decoder, transcribed faithfully from
// `tools/oodle1.py`, which agrees byte for byte with the reference
// implementation on 400 corpus files. See gr2_oodle1.h.

// Design: the field names stay as in the description of the algorithm
// (LS, SW, LSW, TLW, HLS, HLSN, NRW, DT, RRI, RI) so that it can be read
// side by side with the specification text - the one place in this tree
// where abbreviations win over descriptive names.

#include "gr2_oodle1.h"

#include <string.h>

#include <vector>

namespace m2wgr2 {

namespace {

const uint32_t ONE = 0x4000;   // fixed-point representation of 1.0

const char* s_c_szError = "";

/// Repeat length code -> the actual length.
/// Codes 1..60 are lengths 2..61; the last four jump.
uint32_t RepeatLength(uint32_t uCode)
{
    static uint32_t s_auTable[65];
    static bool s_bReady = false;
    if (!s_bReady)
    {
        s_auTable[0] = 0;
        for (uint32_t u = 1; u <= 60; ++u)
            s_auTable[u] = u + 1;
        s_auTable[61] = 128;
        s_auTable[62] = 192;
        s_auTable[63] = 256;
        s_auTable[64] = 512;
        s_bReady = true;
    }
    return s_auTable[uCode];
}

/// The smaller of two unsigned values.
uint32_t Min(uint32_t a, uint32_t b) { return a < b ? a : b; }
/// The larger of two unsigned values.
uint32_t Max(uint32_t a, uint32_t b) { return a > b ? a : b; }

// ---------------------------------------------------------------------------
// Bit reader - the "7+1" split
// ---------------------------------------------------------------------------
// Every input byte enters the register with its seven high bits; its lowest
// bit waits OUTSIDE the register and enters only with the next byte.
class CBitReader
{
public:
    /// Starts reading the `uCount`-byte stream at byte `uStart`: the first byte
    /// is split into its seven high bits (register) and the low bit (waiting).
    CBitReader(const uint8_t* c_pby, uint32_t uCount, uint32_t uStart)
        : m_c_pby(c_pby), m_uCount(uCount), m_uI(uStart)
    {
        m_uMid = m_uI < m_uCount ? (m_c_pby[m_uI] >> 1) : 0;
        m_byLsb = m_uI < m_uCount ? (m_c_pby[m_uI] & 1) : 0;
        m_uModulus = 0x80;
        ++m_uI;
    }

    /// Shifts whole bytes in until the modulus exceeds 0x800000 (24 bits of
    /// precision); past the end of the data it feeds zeros.
    void FillRegister()
    {
        while (m_uModulus <= 0x800000)
        {
            m_uMid = (m_uMid << 1) | m_byLsb;
            // The stream is sometimes read a few bytes past the end of the
            // data - intended, because the decompressor stops by the NUMBER
            // OF OUTPUT BYTES, not by an end marker. Zeros are the safe
            // answer here.
            const uint8_t by = (m_uI < m_uCount) ? m_c_pby[m_uI] : 0;
            ++m_uI;
            m_uMid = (m_uMid << 7) | (by >> 1);
            m_byLsb = by & 1;
            m_uModulus <<= 8;
        }
    }

    /// The next value on the scale [0, uOne) without consuming it (clamped to
    /// uOne - 1; 0 when the modulus is smaller than the scale).
    uint32_t Peek(uint32_t uOne)
    {
        FillRegister();
        const uint32_t uScale = m_uModulus / uOne;
        if (uScale == 0)
            return 0;
        const uint32_t uZ = m_uMid / uScale;
        return uZ < uOne - 1 ? uZ : uOne - 1;
    }

    /// Consumes the interval [uMinZ, uMinZ + uRange) of the scale `uOne` that the
    /// last `Peek` fell into; the top interval takes the whole remaining modulus.
    void Consume(uint32_t uMinZ, uint32_t uRange, uint32_t uOne)
    {
        const uint32_t uScale = m_uModulus / uOne;
        const uint32_t uSz = uMinZ * uScale;
        m_uMid -= uSz;
        if (uMinZ < (uOne - uRange))
            m_uModulus = uRange * uScale;
        else
            m_uModulus -= uSz;
    }

    /// Peek and consume in one - for values of uniform distribution.
    uint32_t Take(uint32_t uOne)
    {
        if (uOne <= 1)
            return 0;
        FillRegister();
        const uint32_t uScale = m_uModulus / uOne;
        if (uScale == 0)
            return 0;
        uint32_t uZ = m_uMid / uScale;
        if (uZ > uOne - 1)
            uZ = uOne - 1;
        const uint32_t uSz = uZ * uScale;
        m_uMid -= uSz;
        if (uZ < uOne - 1)
            m_uModulus = uScale;
        else
            m_uModulus -= uSz;
        return uZ;
    }

private:
    const uint8_t* m_c_pby;
    uint32_t m_uCount;
    uint32_t m_uI;
    uint32_t m_uMid;
    uint32_t m_uModulus;
    uint8_t m_byLsb;
};

// ---------------------------------------------------------------------------
// Symbol coder - an adaptive arithmetic coder with THREE alphabets
// ---------------------------------------------------------------------------
// active         - learned symbols, with assigned ranges
// conditional    - learned since the last normalisation, equal odds
// proportional   - every possible symbol, equal odds
//
// Symbol 0 is the ESCAPE - never a result, it only switches the alphabet.
class CSymbolCoder
{
public:
    /// DOES NOT ALLOCATE (found by the layer audit).
    ///
    /// This used to be `Setup(2, 0)`, i.e. three `assign`s on three vectors.
    /// `CDecompressor` has **327 coders** (`m_akLiteral[4]`, `m_akLength[65]`,
    /// `m_ak4b[256]`, `m_k1b`, `m_k1k`), so merely constructing it made
    /// **about 981 allocations** - and its constructor then calls `Setup` on
    /// EACH of the 327 with a larger size, allocating a second time. The
    /// first 981 went straight to waste: not one coder keeps the values of
    /// the default constructor.
    ///
    /// And a `CDecompressor` is built THREE TIMES PER SECTION of a `.gr2`
    /// (`DecompressSection`, the loop over three streams) - about 2900
    /// allocate/free pairs per loaded model, multiplied by the number of
    /// models when monsters come into view.
    ///
    /// Safety: `CDecompressor::CDecompressor` calls `Setup` on all 327
    /// members before any is used - checked. Outside `CDecompressor` this
    /// class is used nowhere.
    CSymbolCoder() {}

    /// Resets the coder for an alphabet of `uAlphabetSize` values of which
    /// `uUniqueSymbols` can occur: only the escape is known, and the decay and
    /// renormalisation thresholds follow from the alphabet size.
    void Setup(uint32_t uAlphabetSize, uint32_t uUniqueSymbols)
    {
        const size_t n = uAlphabetSize + 2;   // escape (0.0) and "1" (1.0)
        m_US = uUniqueSymbols;
        m_LS.assign(n, 0);
        m_SW.assign(n, ONE);
        m_LSW.assign(n, 0);
        m_SW[0] = 0;
        m_LSW[0] = 4;
        m_TLW = 4;
        m_HLS = 0;
        m_HLSN = 0;
        m_NRW = 8;
        m_DT = Max(256, Min((uAlphabetSize - 1) * 32, 15160));
        m_RRI = 4;
        m_RI = Max(128, Min((uAlphabetSize - 1) * 2, (m_DT / 2) - 32));
    }

    /// Decodes one symbol and updates the adaptive weights: renormalises (with
    /// decay) when due, then either a known symbol or, after the escape, one
    /// from the not-yet-normalised tail or a brand new one read uniformly.
    uint32_t Decode(CBitReader& rBs, uint32_t uAlphabetSize);

private:
    /// Adds room when the active alphabet has outgrown the alphabet.
    ///
    /// The escape dies only when the last symbol is learned, but its range
    /// vanishes only at the NEXT normalisation. Between those moments the
    /// encoder may still send an escape and the decoder has to accept it -
    /// both sides look at the same table. The reference implementation
    /// then writes past the table; here the tables grow.
    void EnsureRoom()
    {
        while (m_LS.size() <= (size_t)m_HLS + 1)
        {
            m_LS.push_back(0);
            m_SW.push_back(ONE);
            m_LSW.push_back(0);
        }
    }

    /// Halves the escape weight, then walks the learned symbols: one with
    /// weight 1 or less is dropped (the highest learned one moves into its
    /// place), any other has its weight halved. Afterwards the heaviest symbol
    /// moves to the top and the escape stays alive while symbols are unknown.
    void Decay();
    /// Rebuilds the cumulative thresholds from the weights (about 0x4000 in total),
    /// schedules the next renormalisation and fills the unused tail with ONE.
    void Normalise();

    uint32_t m_US;
    std::vector<uint32_t> m_LS;    // symbol values
    std::vector<uint32_t> m_SW;    // cumulative weights (thresholds in the interval)
    std::vector<uint32_t> m_LSW;   // recency - how often a symbol comes up
    uint32_t m_TLW;
    uint32_t m_HLS;                // highest learned
    uint32_t m_HLSN;               // highest at the last normalisation
    uint32_t m_NRW;
    uint32_t m_DT;
    uint32_t m_RRI;
    uint32_t m_RI;
};

void CSymbolCoder::Decay()
{
    m_LSW[0] /= 2;
    m_TLW = m_LSW[0];
    uint32_t uBest = 0;
    uint32_t uBestI = 0;
    uint32_t i = 1;
    while (i <= m_HLS)
    {
        while (m_LSW[i] <= 1)
        {
            if (i >= m_HLS)
            {
                m_LSW[i] = 0;
                --m_HLS;
                break;
            }
            // The highest learned symbol takes the place of the one that
            // drops out - the alphabet shrinks by one, without holes.
            m_LSW[i] = m_LSW[m_HLS];
            m_LSW[m_HLS] = 0;
            m_LS[i] = m_LS[m_HLS];
            --m_HLS;
        }
        if (!m_LSW[i])
            break;
        m_LSW[i] /= 2;
        m_TLW += m_LSW[i];
        if (m_LSW[i] > uBest)
        {
            uBest = m_LSW[i];
            uBestI = i;
        }
        ++i;
    }

    if (uBest && uBestI != m_HLS)
    {
        const uint32_t uW = m_LSW[m_HLS];
        m_LSW[m_HLS] = m_LSW[uBestI];
        m_LSW[uBestI] = uW;
        const uint32_t uS = m_LS[m_HLS];
        m_LS[m_HLS] = m_LS[uBestI];
        m_LS[uBestI] = uS;
    }

    // The escape cannot drop out until every symbol is known.
    if (m_HLS != m_US && !m_LSW[0])
    {
        m_LSW[0] = 1;
        ++m_TLW;
    }

    // THE TAIL IS NOT CLOSED HERE (found by the layer audit).
    //
    // There used to be a loop `for (j = m_HLS + 1; j < m_SW.size(); ++j)
    // m_SW[j] = ONE;` - **exactly the same** one `Normalise` runs on the
    // same range. `Decay` has ONE caller in the whole file (`Decode`) and
    // is called there **always directly before** `Normalise`, which does
    // not change `m_HLS` and does not read `m_SW` at all - checked line by
    // line. So that loop was one hundred percent redundant.
    //
    // Scale: for `m_ak4b` the table has up to 258 entries, for the length
    // coders 67. `Decode` is called once per EVERY SYMBOL of the stream,
    // i.e. in the inner loop of decompression. It was the one place in
    // Oodle1 where work grew with the alphabet size, not with the number
    // of bits.
}

void CSymbolCoder::Normalise()
{
    // The quantum is taken from 0x20000, not 0x4000, and divided by 8 at
    // the end - three more bits of precision when truncating.
    const uint32_t uQuantum = 0x20000 / m_TLW;
    m_SW[0] = 0;
    uint32_t uSum = (m_LSW[0] * uQuantum) / 8;
    for (uint32_t i = 1; i <= m_HLS; ++i)
    {
        m_SW[i] = uSum;
        uSum += (m_LSW[i] * uQuantum) / 8;
    }

    if ((m_RRI * 2) < m_RI)
    {
        m_RRI *= 2;
        m_NRW = m_TLW + m_RRI;
    }
    else
    {
        m_NRW = m_TLW + m_RI;
    }

    m_HLSN = m_HLS;
    for (size_t j = m_HLS + 1; j < m_SW.size(); ++j)
        m_SW[j] = ONE;
}

uint32_t CSymbolCoder::Decode(CBitReader& rBs, uint32_t uAlphabetSize)
{
    if (m_TLW >= m_NRW)
    {
        if (m_TLW >= m_DT)
            Decay();
        Normalise();
    }

    const uint32_t uZ = rBs.Peek(ONE);

    uint32_t i = 0;
    while (i <= m_HLSN)
    {
        if (m_SW[i + 1] > uZ)
            break;
        ++i;
    }

    rBs.Consume(m_SW[i], m_SW[i + 1] - m_SW[i], ONE);
    ++m_LSW[i];
    ++m_TLW;

    if (i)
        return m_LS[i];

    // ESCAPE: either a symbol from the conditional alphabet or a brand new one.
    if (m_HLS != m_HLSN)
    {
        if (rBs.Take(2))
        {
            i = rBs.Take(m_HLS - m_HLSN) + m_HLSN + 1;
            m_LSW[i] += 2;
            m_TLW += 2;
            return m_LS[i];
        }
    }

    ++m_HLS;
    EnsureRoom();
    const uint32_t uSymbol = rBs.Take(uAlphabetSize);
    m_LS[m_HLS] = uSymbol;
    m_LSW[m_HLS] += 2;
    m_TLW += 2;

    // Once every symbol is known the escape is no longer needed.
    if (m_HLS == m_US)
    {
        m_TLW -= m_LSW[0];
        m_LSW[0] = 0;
    }

    return uSymbol;
}

// ---------------------------------------------------------------------------
// LZ layer - 327 separate coders, chosen by context
// ---------------------------------------------------------------------------
class CDecompressor
{
public:
    /// Sets up the LZ decoder from the three header words of a block: window
    /// size, literal alphabet and the 4 + 65 + 1 + 256 + 1 symbol coders.
    CDecompressor(CBitReader& rBs, const uint32_t* c_puHeader)
        : m_rBs(rBs), m_bError(false)
    {
        m_uWindow = c_puHeader[0] >> 9;
        m_LAS = c_puHeader[0] & 0x1FF;
        const uint32_t uUniqueLiterals = c_puHeader[1] & 0x1FF;
        const uint32_t uLargest1k = c_puHeader[1] >> 19;

        // Four literal coders, chosen by the output position modulo 4. With
        // four-byte data (RGBA colours, floats) each then sees only one
        // component, which changes slowly.
        for (int i = 0; i < 4; ++i)
            m_akLiteral[i].Setup(m_LAS, uUniqueLiterals);

        // 65 length coders, chosen by the PREVIOUS length code. Four header
        // bytes say how many distinct codes occur in a group; the split is
        // by sixteen, and code 64 joins the last group.
        const uint32_t auRl[4] = {
            (c_puHeader[2] >> 24) & 0xFF, (c_puHeader[2] >> 16) & 0xFF,
            (c_puHeader[2] >> 8) & 0xFF, c_puHeader[2] & 0xFF };
        for (uint32_t uCode = 0; uCode < 65; ++uCode)
        {
            const uint32_t uGroup = uCode / 16 < 3 ? uCode / 16 : 3;
            m_akLength[uCode].Setup(65, auRl[uGroup]);
        }

        m_O1AS = Min(4, m_uWindow + 1);
        const uint32_t u4As = Min(256, (m_uWindow / 4) + 1);
        const uint32_t u1kAs = (m_uWindow / 1024) + 1;

        m_k1b.Setup(m_O1AS, m_O1AS);
        for (int i = 0; i < 256; ++i)
            m_ak4b[i].Setup(u4As, u4As);
        m_k1k.Setup(u1kAs, uLargest1k + 1);

        m_uBytes = 0;
        m_uLastCode = 0;
    }

    /// One call: a literal or a repeat. Returns the number of bytes.
    uint32_t Step(uint8_t* pbyOutput, uint32_t uPos, uint32_t uSize);

    /// True once a step wrote past the output or referred before its start - the
    /// block is corrupt.
    bool Error() const { return m_bError; }

private:
    CBitReader& m_rBs;
    CSymbolCoder m_akLiteral[4];
    CSymbolCoder m_akLength[65];
    CSymbolCoder m_k1b;
    CSymbolCoder m_ak4b[256];
    CSymbolCoder m_k1k;

    uint32_t m_uWindow;
    uint32_t m_LAS;
    uint32_t m_O1AS;
    uint32_t m_uBytes;
    uint32_t m_uLastCode;
    bool m_bError;
};

uint32_t CDecompressor::Step(uint8_t* pbyOutput, uint32_t uPos, uint32_t uSize)
{
    const uint32_t uCode = m_akLength[m_uLastCode].Decode(m_rBs, 65);
    m_uLastCode = uCode;

    if (!uCode)
    {
        const uint32_t uLit = m_akLiteral[m_uBytes & 3].Decode(m_rBs, m_LAS);
        if (uPos >= uSize)
        {
            m_bError = true;
            s_c_szError = "literal past the end of the section";
            return 0;
        }
        pbyOutput[uPos] = (uint8_t)(uLit & 0xFF);
        ++m_uBytes;
        return 1;
    }

    const uint32_t uLength = RepeatLength(uCode);
    const uint32_t uActiveWindow = Min(m_uWindow, m_uBytes);

    const uint32_t u1b = m_k1b.Decode(m_rBs, m_O1AS) + 1;
    const uint32_t u1k = m_k1k.Decode(m_rBs, (uActiveWindow / 1024) + 1);
    const uint32_t u4b = m_ak4b[u1k < 256 ? u1k : 255].Decode(
        m_rBs, Min(256, (uActiveWindow / 4) + 1));
    const uint32_t uDistance = (u1k * 1024) + (u4b * 4) + u1b;

    if (uDistance > uPos)
    {
        m_bError = true;
        s_c_szError = "distance before the start of the section";
        return 0;
    }
    if (uPos + uLength > uSize)
    {
        m_bError = true;
        s_c_szError = "repeat past the end of the section";
        return 0;
    }

    // Copying BYTE BY BYTE, not as a block. With a distance shorter than
    // the length the repeat reads its own fresh output - as it should,
    // because that is how LZ writes repeating patterns.
    const uint32_t uSource = uPos - uDistance;
    for (uint32_t k = 0; k < uLength; ++k)
        pbyOutput[uPos + k] = pbyOutput[uSource + k];

    m_uBytes += uLength;
    return uLength;
}

}  // namespace

const char* LastError()
{
    return s_c_szError;
}

bool DecompressSection(const uint8_t* c_pbyPacked, uint32_t uPacked,
                       uint32_t uFirst16, uint32_t uFirst8,
                       uint8_t* pbyOutput, uint32_t uUnpacked)
{
    if (uUnpacked == 0)
        return true;
    if (uPacked < 36)
    {
        s_c_szError = "section shorter than three headers";
        return false;
    }

    uint32_t auHeaders[9];
    memcpy(auHeaders, c_pbyPacked, sizeof(auHeaders));

    // All three streams share ONE bit reader - only the decompressor
    // changes, because every stream has its own alphabet sizes.
    CBitReader kBs(c_pbyPacked, uPacked, 36);

    const uint32_t auBounds[3] = { uFirst16, uFirst8, uUnpacked };
    uint32_t uPos = 0;
    for (int nr = 0; nr < 3; ++nr)
    {
        if (uPos >= uUnpacked)
            break;
        CDecompressor kD(kBs, auHeaders + nr * 3);
        while (uPos < auBounds[nr])
        {
            const uint32_t uCount = kD.Step(pbyOutput, uPos, uUnpacked);
            if (kD.Error())
                return false;
            uPos += uCount;
        }
        if (uPos != auBounds[nr])
        {
            // EVERY STREAM HAS TO COME OUT TO THE BYTE. There is no end
            // marker - the end is known by the byte count - so overshooting
            // the boundary is the only visible trace of a divergence.
            // Without this check stream one would overwrite the start of
            // stream two and nobody would know.
            s_c_szError = "stream overshot its boundary";
            return false;
        }
    }

    return true;
}

}  // namespace m2wgr2
