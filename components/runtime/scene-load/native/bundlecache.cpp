// VaMSceneLoadNative.bin, part 2 (VaM Scene Load 1.5.0): asset bundles of .var packages prepared in memory for
// AssetBundle.LoadFromStream, plus DecompressOnLoad Vorbis clips decoded in parallel.
//
// VaM loads every asset bundle of a .var package by inflating the zip entry into a managed byte[] on a thread of its own
// and handing it to AssetBundle.LoadFromMemoryAsync; Unity then decompresses an LZMA bundle as a whole on its one
// loading thread, one bundle after another (VaM's queue). 1.3.x wrote, once per bundle, a file for LoadFromFile; since
// 1.5.0: nothing is written: a background job reads the entry and
// keeps, in memory, what Unity reads through a Stream:
//   - LZMA bundles (every block LZMA or stored): the same UnityFS archive uncompressed (header flags 0x43: blocks info
//     LZ4-compressed, smaller than the info, as Unity 2018.1 refuses one that is not; one stored block per original
//     block, block flags 0x40), the same data hash and directory, so the archive's files are byte for byte what Unity
//     would have decompressed;
//   - bundles with AudioClips that are DecompressOnLoad + Vorbis (FMOD decodes those on Unity's main thread when the
//     clip loads: a scene's voice pack, 371 clips, 24 s in one frame): each such
//     clip points at a PCM16 FSB5 of its samples instead (m_Resource offset / size, m_CompressionFormat PCM), placed
//     after the .resource node, each on a 128 KB block boundary, stored blocks from the first one on; no node over
//     1 GB (FMOD refuses every clip of a larger resource file), further ones get a name of the same length and the
//     clips there m_Source pointed at it. Those regions are
//     not kept: the first read of any of them decodes all the bundle's clips on CPU Boost (vorbisdec.cpp, libvorbis;
//     FMOD's own int16 rounding: about 1 value in 5000 differs by 1 from FMOD's float
//     arithmetic), a clip not done yet is decoded by the reader itself, and a clip's buffer goes 2 s after its last
//     byte was read (Unity reads most clips twice in a row; decoded again if read later), the decoded clips of a bundle
//     nobody reads for 10 s too; decoded-unread bytes stop the background at a cap;
//   - every other bundle (LZ4, uncompressed): the entry's bytes as they are.
// A bundle's memory goes when the plugin frees it (its AssetBundle unloaded). Jobs run on CPU Boost's shared workers
// (task "scene load bundles", priority 0 = background) when it is loaded: one thread of ours hands every job queued so
// far to CB_Run, one chunk per job in submission order, repeats for the jobs queued meanwhile, then each bundle whose
// clips are being read, one chunk per clip; without CPU Boost, that thread runs them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "third_party/miniz/miniz.h"
#include "vorbisdec.h"

extern "C" int64_t SL_ReadZipEntry(const wchar_t* zipPath, int64_t headerOffset, int64_t compSize, int method, uint8_t* dst, int64_t dstLen);

// ---------------------------------------------------------------------------------------------------------------------
// LZMA (the LZMA SDK's decoding algorithm, as specified by its reference decoder LzmaSpec.cpp; written for this file).
// Decodes exactly outSize bytes from a raw LZMA stream (5 property bytes first), the way Unity decodes an LZMA block
// of known size. Any malformed input (bad properties, distance beyond the output, reading past the input) fails.

namespace lz {

struct RangeDec {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t range, code;
    bool bad;
    inline uint32_t Next() { if (p < end) return *p++; bad = true; return 0; }
    bool Init()
    {
        range = 0xFFFFFFFFu; code = 0; bad = false;
        uint32_t b = Next();
        for (int i = 0; i < 4; i++) code = (code << 8) | Next();
        return b == 0 && code != range && !bad;
    }
    inline void Normalize() { if (range < (1u << 24)) { range <<= 8; code = (code << 8) | Next(); } }
    inline unsigned Bit(uint16_t* prob)
    {
        unsigned v = *prob;
        uint32_t bound = (range >> 11) * v;
        unsigned s;
        if (code < bound) { v += ((1u << 11) - v) >> 5; range = bound; s = 0; }
        else { v -= v >> 5; code -= bound; range -= bound; s = 1; }
        *prob = (uint16_t)v;
        Normalize();
        return s;
    }
    inline uint32_t Direct(unsigned n)
    {
        uint32_t res = 0;
        do {
            range >>= 1;
            code -= range;
            uint32_t t = 0u - (code >> 31);
            code += range & t;
            if (code == range) bad = true;
            Normalize();
            res <<= 1;
            res += t + 1;
        } while (--n);
        return res;
    }
};

template <unsigned N> static inline unsigned Tree(uint16_t* probs, RangeDec& rc)
{
    unsigned m = 1;
    for (unsigned i = 0; i < N; i++) m = (m << 1) + rc.Bit(&probs[m]);
    return m - (1u << N);
}

static inline unsigned TreeReverse(uint16_t* probs, unsigned n, RangeDec& rc)
{
    unsigned m = 1, sym = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = rc.Bit(&probs[m]);
        m = (m << 1) + bit;
        sym |= bit << i;
    }
    return sym;
}

struct LenDec {
    uint16_t choice, choice2, low[16][8], mid[16][8], high[256];
    void Init()
    {
        choice = choice2 = 1024;
        for (auto& a : low) for (auto& x : a) x = 1024;
        for (auto& a : mid) for (auto& x : a) x = 1024;
        for (auto& x : high) x = 1024;
    }
    inline unsigned Decode(RangeDec& rc, unsigned posState)
    {
        if (rc.Bit(&choice) == 0) return Tree<3>(low[posState], rc);
        if (rc.Bit(&choice2) == 0) return 8 + Tree<3>(mid[posState], rc);
        return 16 + Tree<8>(high, rc);
    }
};

// 0 ok; negative: -1 properties, -2 range coder start, -3 data (distance / overrun / corrupt), -4 memory
static int Decode(const uint8_t* src, size_t srcLen, uint8_t* out, size_t outSize)
{
    if (srcLen < 5) return -1;
    unsigned d = src[0];
    if (d >= 9 * 5 * 5) return -1;
    unsigned lc = d % 9; d /= 9;
    unsigned lp = d % 5, pb = d / 5;
    uint32_t dictSize = (uint32_t)src[1] | ((uint32_t)src[2] << 8) | ((uint32_t)src[3] << 16) | ((uint32_t)src[4] << 24);
    if (dictSize < 4096) dictSize = 4096;
    size_t nLit = (size_t)0x300 << (lc + lp);
    uint16_t* lit = (uint16_t*)malloc(nLit * sizeof(uint16_t));
    if (!lit) return -4;
    for (size_t i = 0; i < nLit; i++) lit[i] = 1024;
    uint16_t isMatch[12 << 4], isRep[12], isRepG0[12], isRepG1[12], isRepG2[12], isRep0Long[12 << 4];
    uint16_t posSlot[4][64], posDec[1 + 128 - 14], align[16];
    for (auto& x : isMatch) x = 1024;
    for (auto& x : isRep0Long) x = 1024;
    for (int i = 0; i < 12; i++) isRep[i] = isRepG0[i] = isRepG1[i] = isRepG2[i] = 1024;
    for (auto& a : posSlot) for (auto& x : a) x = 1024;
    for (auto& x : posDec) x = 1024;
    for (auto& x : align) x = 1024;
    LenDec lenDec, repLenDec;
    lenDec.Init(); repLenDec.Init();

    RangeDec rc;
    rc.p = src + 5; rc.end = src + srcLen;
    if (!rc.Init()) { free(lit); return -2; }
    const unsigned pbMask = (1u << pb) - 1, lpMask = (1u << lp) - 1;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    unsigned state = 0;
    size_t pos = 0;
    int result = 0;
    while (pos < outSize) {
        unsigned posState = (unsigned)pos & pbMask;
        if (rc.Bit(&isMatch[(state << 4) + posState]) == 0) {
            unsigned prev = pos ? out[pos - 1] : 0;
            uint16_t* probs = &lit[(size_t)0x300 * ((((unsigned)pos & lpMask) << lc) + (prev >> (8 - lc)))];
            unsigned symbol = 1;
            if (state >= 7) {
                unsigned matchByte = out[pos - rep0 - 1];
                do {
                    unsigned matchBit = (matchByte >> 7) & 1;
                    matchByte <<= 1;
                    unsigned bit = rc.Bit(&probs[((1 + matchBit) << 8) + symbol]);
                    symbol = (symbol << 1) | bit;
                    if (matchBit != bit) break;
                } while (symbol < 0x100);
            }
            while (symbol < 0x100) symbol = (symbol << 1) | rc.Bit(&probs[symbol]);
            out[pos++] = (uint8_t)(symbol - 0x100);
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        unsigned len;
        if (rc.Bit(&isRep[state]) != 0) {
            if (pos == 0) { result = -3; break; }
            if (rc.Bit(&isRepG0[state]) == 0) {
                if (rc.Bit(&isRep0Long[(state << 4) + posState]) == 0) {
                    state = state < 7 ? 9 : 11;
                    out[pos] = out[pos - rep0 - 1];
                    pos++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (rc.Bit(&isRepG1[state]) == 0) dist = rep1;
                else {
                    if (rc.Bit(&isRepG2[state]) == 0) dist = rep2;
                    else { dist = rep3; rep3 = rep2; }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = repLenDec.Decode(rc, posState);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2; rep2 = rep1; rep1 = rep0;
            len = lenDec.Decode(rc, posState);
            state = state < 7 ? 7 : 10;
            unsigned lenState = len < 3 ? len : 3;
            unsigned slot = Tree<6>(posSlot[lenState], rc);
            if (slot < 4) rep0 = slot;
            else {
                unsigned nDirect = (slot >> 1) - 1;
                uint32_t dist = (2u | (slot & 1u)) << nDirect;
                if (slot < 14) dist += TreeReverse(posDec + dist - slot, nDirect, rc);
                else {
                    dist += rc.Direct(nDirect - 4) << 4;
                    dist += TreeReverse(align, 4, rc);
                }
                rep0 = dist;
            }
            if (rep0 == 0xFFFFFFFFu) { result = -3; break; }   // end marker before outSize bytes
            if (rep0 >= dictSize || rep0 >= pos) { result = -3; break; }
        }
        len += 2;
        if (len > outSize - pos) { result = -3; break; }   // longer than the block: corrupt
        const uint8_t* from = out + pos - rep0 - 1;
        uint8_t* to = out + pos;
        if (rep0 + 1 >= len) memcpy(to, from, len);
        else for (unsigned i = 0; i < len; i++) to[i] = from[i];
        pos += len;
        if (rc.bad) { result = -3; break; }
    }
    if (result == 0 && rc.bad) result = -3;
    free(lit);
    return result;
}

}  // namespace lz

// LZ4 block (Unity's blocks info and LZ4 blocks): exactly outSize bytes; -1 on any malformed input.
static int Lz4Decode(const uint8_t* src, size_t srcLen, uint8_t* out, size_t outSize)
{
    size_t i = 0, o = 0;
    while (i < srcLen) {
        unsigned tok = src[i++];
        size_t n = tok >> 4;
        if (n == 15) { unsigned b; do { if (i >= srcLen) return -1; b = src[i++]; n += b; } while (b == 255); }
        if (n > srcLen - i || n > outSize - o) return -1;
        memcpy(out + o, src + i, n); i += n; o += n;
        if (i >= srcLen) break;
        if (srcLen - i < 2) return -1;
        size_t off = src[i] | (src[i + 1] << 8); i += 2;
        if (off == 0 || off > o) return -1;
        size_t m = tok & 15;
        if (m == 15) { unsigned b; do { if (i >= srcLen) return -1; b = src[i++]; m += b; } while (b == 255); }
        m += 4;
        if (m > outSize - o) return -1;
        for (size_t k = 0; k < m; k++) out[o + k] = out[o + k - off];
        o += m;
    }
    return o == outSize ? 0 : -1;
}

// LZ4 block of in[0, n), greedy with a 4-byte hash (the blocks info is a few hundred bytes to a few KB). Follows the
// block rules: the last match starts at least 12 bytes before the end, the last 5 bytes are literals, offsets < 64 KB.
// Unity 2018.1 refused an info framed as literals only (bigger than the info itself): "Unable to read header from
// archive file" for every rewritten bundle (first game run); its own uncompressed bundles carry a real LZ4 info.
static void Lz4Encode(const uint8_t* in, size_t n, std::vector<uint8_t>& out)
{
    out.clear();
    auto len = [&](size_t r) { while (r >= 255) { out.push_back(255); r -= 255; } out.push_back((uint8_t)r); };
    auto emit = [&](size_t lit0, size_t litLen, size_t off, size_t mlen) {
        size_t ml = mlen ? mlen - 4 : 0;
        out.push_back((uint8_t)(((litLen >= 15 ? 15 : litLen) << 4) | (mlen ? (ml >= 15 ? 15 : ml) : 0)));
        if (litLen >= 15) len(litLen - 15);
        out.insert(out.end(), in + lit0, in + lit0 + litLen);
        if (!mlen) return;
        out.push_back((uint8_t)(off & 255)); out.push_back((uint8_t)(off >> 8));
        if (ml >= 15) len(ml - 15);
    };
    std::vector<int64_t> table(1u << 12, -1);
    size_t anchor = 0, ip = 0;
    while (n >= 12 && ip + 12 <= n) {
        uint32_t v; memcpy(&v, in + ip, 4);
        uint32_t h = (v * 2654435761u) >> 20;
        int64_t ref = table[h];
        table[h] = (int64_t)ip;
        if (ref >= 0 && ip - (size_t)ref <= 65535 && memcmp(in + ref, in + ip, 4) == 0) {
            size_t m = 4, limit = n - 5;
            while (ip + m < limit && in[ref + m] == in[ip + m]) m++;
            emit(anchor, ip - anchor, ip - (size_t)ref, m);
            ip += m; anchor = ip;
        } else ip++;
    }
    emit(anchor, n - anchor, 0, 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// UnityFS

static inline uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t Be64(const uint8_t* p) { return ((uint64_t)Be32(p) << 32) | Be32(p + 4); }
static inline uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t Le64(const uint8_t* p) { return Le32(p) | ((uint64_t)Le32(p + 4) << 32); }
static inline void Put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
static inline void Put64(std::vector<uint8_t>& v, uint64_t x) { Put32(v, (uint32_t)(x >> 32)); Put32(v, (uint32_t)x); }
static inline void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

static const uint64_t kBlock = 128 * 1024;   // stored blocks after the first converted clip; clips start on one
static inline uint64_t AlignUp(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }
// The largest .resource node a bundle is given. FMOD refuses every clip of a resource file larger than that ("Cannot
// create FMOD::Sound instance for resource archive:/.../....resource, (Unsupported file or audio format.)"; a first
// game run: a voice pack's node grew to 4.24 GB, all 371 clips refused, while every node under
// 0.5 GB worked); its file size is 32 bits there. Clips past it go into further nodes of the same name length (one
// character of the name changed), their m_Source pointed at them.
static const uint64_t kNodeMax = (uint64_t)1 << 30;

// audio: decoded clip bytes not read yet (all bundles), its cap, and counters
static std::atomic<int64_t> gPending{0}, gPendingPeak{0};
static std::atomic<int64_t> gCap{(int64_t)4 << 30};
static std::atomic<int> gAudioOn{1};
static std::atomic<int64_t> sClips{0}, sKept{0}, sEager{0}, sReader{0}, sRedecode{0}, sCapSkip{0}, sFreed{0}, sFail{0},
    sDecodeTicks{0}, sWaitTicks{0}, sFrames{0}, sBundlesAudio{0}, sNodes{0}, sDropped{0};
static std::atomic<int64_t> gHeld{0}, gHeldPeak{0};   // bytes the bundles keep (entries, decompressed data, headers)
// A clip's buffer read to its end stays kKeepMs after its last read: Unity reads most clips twice in a row (a second
// game run: 331 of 440 decoded again on the reading thread, freed after the first read). Decoded
// clips of a bundle nobody reads any more (kIdleMs since its last read) go too.
static const int64_t kKeepMs = 2000, kIdleMs = 10000;
static LARGE_INTEGER gFreq;

struct Seg { uint64_t at, len; const uint8_t* mem; int clip; };   // clip < 0: mem (null: zeros); else that clip's region

struct Clip {
    vd::Fsb fsb;               // the Vorbis FSB5, in the bundle's data
    uint64_t size = 0;         // the PCM16 FSB5 Unity reads instead
    uint8_t hash[24] = {};     // header bytes 36..59 of the Vorbis FSB5, carried over
    int state = 0;             // 0 not decoded, 1 decoding, 2 ready, 3 freed (decoded again when read); under Bundle::lock
    int users = 0;
    uint8_t* pcm = nullptr;
    uint64_t readTo = 0;       // front-to-back progress of the reads of the ready buffer
    bool done = false;         // read to its end
    int64_t lastRead = 0;      // ticks
};

struct Bundle : std::enable_shared_from_this<Bundle> {
    std::vector<uint8_t> head;                    // UnityFS header + LZ4 blocks info
    uint8_t* raw = nullptr;                       // the entry's bytes (kept when the data is read from them)
    uint8_t* data = nullptr;                      // decompressed data
    std::vector<std::vector<uint8_t>> patched;    // serialized files with their AudioClips pointed at PCM16
    std::vector<Seg> segs;
    std::vector<Clip> clips;
    uint64_t length = 0;
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    bool eager = false, capSkipped = false;
    int64_t lastRead = 0;                         // ticks of the last clip read
    uint64_t held = 0;
    void Hold(uint64_t n)
    {
        held = n;
        int64_t h = gHeld += (int64_t)n, peak = gHeldPeak.load();
        while (h > peak && !gHeldPeak.compare_exchange_weak(peak, h)) {}
    }
    ~Bundle()
    {
        for (Clip& c : clips) if (c.pcm) { free(c.pcm); gPending -= (int64_t)c.size; }
        free(raw);
        free(data);
        gHeld -= (int64_t)held;
    }
};

static void QueueDecode(std::shared_ptr<Bundle> b);
static void RegisterAudio(const std::shared_ptr<Bundle>& b);

// ---- serialized files (version 17, Unity 2018.1) and their AudioClips

struct Rd {
    const uint8_t* p; size_t n, q; bool big, bad;
    uint32_t U32() { if (q + 4 > n) { bad = true; q = n; return 0; } uint32_t v = big ? Be32(p + q) : Le32(p + q); q += 4; return v; }
    uint64_t U64() { if (q + 8 > n) { bad = true; q = n; return 0; } uint64_t v = big ? Be64(p + q) : Le64(p + q); q += 8; return v; }
    void Skip(size_t k) { if (k > n - q) { bad = true; q = n; } else q += k; }
    void Align4() { size_t a = (q + 3) & ~(size_t)3; if (a > n) { bad = true; q = n; } else q = a; }
};

// AudioClip's oldTypeHash in 2018.1 (the layout read below; checked against every clip of the cache: the read ends at
// the object's size)
static const uint8_t kClipHash[16] = { 0xb2, 0x34, 0x49, 0x46, 0x5e, 0x37, 0x63, 0xdd, 0x19, 0x33, 0x5a, 0x88, 0xea, 0x09, 0xae, 0x99 };

struct ClipRef {
    size_t at;                 // m_Resource.m_Offset within the serialized file (m_Size at +8, m_CompressionFormat at +16)
    size_t srcAt;              // the characters of m_Resource.m_Source within the serialized file
    bool big;
    int loadType, channels, freq, format;
    std::string source;
    uint64_t offset, size;
};

static void Clips(const uint8_t* s, size_t n, std::vector<ClipRef>& out)
{
    if (n < 24) return;
    uint32_t version = Be32(s + 8), dataOff = Be32(s + 12);
    if (version != 17 || Be32(s + 4) != n || dataOff > n) return;
    Rd r{ s, n, 20, s[16] != 0, false };
    while (r.q < n && s[r.q]) r.q++;
    r.Skip(1 + 4);
    if (r.bad || r.q >= n) return;
    bool tree = s[r.q++] != 0;
    int types = (int)r.U32();
    if (types < 0 || types > 100000) return;
    std::vector<int> cls(types);
    std::vector<bool> clipType(types);
    for (int i = 0; i < types && !r.bad; i++) {
        cls[i] = (int)r.U32();
        r.Skip(3);
        if (cls[i] == 114) r.Skip(16);
        clipType[i] = cls[i] == 83 && r.q + 16 <= n && memcmp(s + r.q, kClipHash, 16) == 0;
        r.Skip(16);
        if (tree) { uint32_t nodes = r.U32(), strings = r.U32(); r.Skip((size_t)nodes * 24 + strings); }
    }
    int objects = (int)r.U32();
    if (r.bad || objects < 0 || objects > 10000000) return;
    for (int i = 0; i < objects && !r.bad; i++) {
        r.Align4();
        r.U64();
        uint32_t start = r.U32(), size = r.U32();
        int ti = (int)r.U32();
        if (r.bad || ti < 0 || ti >= types || !clipType[ti]) continue;
        if ((uint64_t)dataOff + start + size > n) continue;
        Rd o{ s + dataOff + start, size, 0, r.big, false };
        uint32_t nameLen = o.U32();
        o.Skip(nameLen); o.Align4();
        ClipRef c;
        c.big = r.big;
        c.loadType = (int)o.U32(); c.channels = (int)o.U32(); c.freq = (int)o.U32(); o.U32(); o.U32();
        o.Skip(2); o.Align4(); o.U32(); o.Skip(3); o.Align4();
        uint32_t srcLen = o.U32();
        if (o.bad || srcLen > size - o.q) continue;
        c.srcAt = dataOff + start + o.q;
        c.source.assign((const char*)o.p + o.q, srcLen);
        o.Skip(srcLen); o.Align4();
        c.at = dataOff + start + o.q;
        c.offset = o.U64(); c.size = o.U64(); c.format = (int)o.U32();
        if (o.bad || o.q != size) continue;
        out.push_back(c);
    }
}

static void PutField(uint8_t* p, uint64_t v, int bytes, bool big)
{
    for (int i = 0; i < bytes; i++) p[big ? bytes - 1 - i : i] = (uint8_t)(v >> (8 * i));
}

// ---- building

struct Node { uint64_t off, size; uint32_t flags; std::string name; };

// The entry's bytes (raw, owned from here on) -> what Unity is given to read. 1 rebuilt (LZMA -> stored and/or clips
// -> PCM16), 2 the bytes as they are; negative: -10 archive, -11/-12 block, -4 memory, -40 not UnityFS 6 (VaM's own).
static int BuildBundle(uint8_t* raw, size_t size, bool audio, std::shared_ptr<Bundle>& out, int* converted)
{
    // b owns raw only once it is kept (as it is, or its data read from it); on failure the caller frees it
    std::shared_ptr<Bundle> b = std::make_shared<Bundle>();
    *converted = 0;
    auto asIs = [&]() -> int {
        if (b->data) { free(b->data); b->data = nullptr; }
        b->raw = raw;
        b->segs.assign(1, Seg{ 0, size, raw, -1 });
        b->clips.clear(); b->patched.clear(); b->head.clear();
        b->length = size;
        b->Hold(size);
        out = b;
        return 2;
    };
    if (size < 64 || memcmp(raw, "UnityFS\0", 8) != 0 || Be32(raw + 8) != 6) return -40;
    const uint8_t* d = raw;
    size_t i = 12;
    while (i < size && d[i]) i++;
    if (++i >= size) return -10;
    while (i < size && d[i]) i++;
    if (++i + 20 > size) return -10;
    uint64_t total = Be64(d + i);
    uint32_t csz = Be32(d + i + 8), usz = Be32(d + i + 12), flags = Be32(d + i + 16);
    size_t headerEnd = i + 20;
    if (total != size || (flags & ~0xFFu)) return asIs();
    unsigned infoComp = flags & 0x3F;
    bool infoAtEnd = (flags & 0x80) != 0;
    if (csz > size - headerEnd) return -10;
    const uint8_t* infoSrc = infoAtEnd ? d + size - csz : d + headerEnd;
    size_t dataStart = infoAtEnd ? headerEnd : headerEnd + csz;
    std::vector<uint8_t> info(usz);
    if (infoComp == 0) { if (csz != usz) return -10; memcpy(info.data(), infoSrc, usz); }
    else if (infoComp == 1) { if (lz::Decode(infoSrc, csz, info.data(), usz)) return -10; }
    else if (infoComp == 2 || infoComp == 3) { if (Lz4Decode(infoSrc, csz, info.data(), usz)) return -10; }
    else return asIs();
    if (usz < 24) return -10;
    uint32_t nBlocks = Be32(info.data() + 16);
    size_t j = 20;
    if (nBlocks > (usz - 20) / 10) return -10;
    struct Block { uint32_t u, c; uint16_t f; };
    std::vector<Block> blocks(nBlocks);
    bool anyLzma = false, anyLz4 = false, other = false;
    uint64_t uTotal = 0, cTotal = 0;
    for (uint32_t k = 0; k < nBlocks; k++, j += 10) {
        blocks[k].u = Be32(info.data() + j);
        blocks[k].c = Be32(info.data() + j + 4);
        blocks[k].f = (uint16_t)((info[j + 8] << 8) | info[j + 9]);
        unsigned comp = blocks[k].f & 0x3F;
        if (comp == 1) anyLzma = true;
        else if (comp == 2 || comp == 3) anyLz4 = true;
        else if (comp != 0) other = true;
        if (blocks[k].f & ~0x7Fu) other = true;
        uTotal += blocks[k].u; cTotal += blocks[k].c;
    }
    if (j + 4 > usz) return -10;
    uint32_t nNodes = Be32(info.data() + j);
    size_t dirAt = j;
    j += 4;
    std::vector<Node> nodes;
    bool hasResource = false;
    for (uint32_t k = 0; k < nNodes; k++) {
        if (j + 20 > usz) return -10;
        Node nd;
        nd.off = Be64(info.data() + j); nd.size = Be64(info.data() + j + 8); nd.flags = Be32(info.data() + j + 16);
        j += 20;
        size_t e = j;
        while (e < usz && info[e]) e++;
        if (e >= usz) return -10;
        nd.name.assign((const char*)info.data() + j, e - j);
        j = e + 1;
        if (nd.off > uTotal || nd.size > uTotal - nd.off) return -10;
        if (nd.name.size() > 9 && nd.name.compare(nd.name.size() - 9, 9, ".resource") == 0) hasResource = true;
        nodes.push_back(nd);
    }
    size_t dirEnd = j;
    bool wantAudio = audio && hasResource && gAudioOn.load();
    if (other || (!anyLzma && !wantAudio)) return asIs();
    size_t dataEnd = infoAtEnd ? size - csz : size;
    if (cTotal > dataEnd - dataStart) return -10;
    if (uTotal > ((uint64_t)1 << 40)) return -10;

    // the data: decompressed, or the entry's own bytes when every block is stored
    const uint8_t* D;
    if (!anyLzma && !anyLz4) {
        if (cTotal != uTotal) return -10;
        D = d + dataStart;
    } else {
        b->data = (uint8_t*)malloc(uTotal ? (size_t)uTotal : 1);
        if (!b->data) return -4;
        const uint8_t* src = d + dataStart;
        size_t o = 0;
        for (uint32_t k = 0; k < nBlocks; k++) {
            unsigned comp = blocks[k].f & 0x3F;
            int r = 0;
            if (comp == 0) { if (blocks[k].c != blocks[k].u) r = -11; else memcpy(b->data + o, src, blocks[k].u); }
            else if (comp == 1) r = lz::Decode(src, blocks[k].c, b->data + o, blocks[k].u);
            else r = Lz4Decode(src, blocks[k].c, b->data + o, blocks[k].u) ? -12 : 0;
            if (r) return r < -10 ? r : -12;
            src += blocks[k].c; o += blocks[k].u;
        }
        D = b->data;
    }

    // DecompressOnLoad Vorbis clips this decoder can make PCM16 of
    struct Conv { int ser, res; ClipRef ref; vd::Fsb fsb; };
    std::vector<Conv> conv;
    if (wantAudio) {
        for (int n = 0; n < (int)nodes.size(); n++) {
            if (!(nodes[n].flags & 4)) continue;
            std::vector<ClipRef> refs;
            Clips(D + nodes[n].off, (size_t)nodes[n].size, refs);
            for (const ClipRef& c : refs) {
                if (c.loadType != 0 || c.format != 1) continue;
                size_t slash = c.source.rfind('/');
                std::string leaf = slash == std::string::npos ? c.source : c.source.substr(slash + 1);
                int res = -1;
                for (int m = 0; m < (int)nodes.size(); m++) if (nodes[m].name == leaf) { res = m; break; }
                if (res < 0 || c.offset > nodes[res].size || c.size > nodes[res].size - c.offset) continue;
                vd::Fsb f;
                bool ok = vd::ParseFsb(D + nodes[res].off + c.offset, (size_t)c.size, f) && f.mode == 15 && f.frames > 0
                    && f.channels == c.channels && (f.channels == 1 || f.channels == 2) && f.rate == c.freq && vd::FsbRateIndex(f.rate) > 0
                    && f.frames * f.channels * 2 + 256 <= kNodeMax && vd::HaveSetup(f.setupCrc, f.channels, f.rate);
                if (!ok) { sKept++; continue; }
                conv.push_back(Conv{ n, res, c, f });
            }
        }
    }
    if (conv.empty() && !anyLzma) return asIs();
    if (conv.empty()) {
        // LZMA only: the same archive stored, one stored block per block, the same directory (as 1.3.1 wrote it)
        std::vector<uint8_t> ni(info.begin(), info.begin() + 16);
        Put32(ni, nBlocks);
        for (uint32_t k = 0; k < nBlocks; k++) { Put32(ni, blocks[k].u); Put32(ni, blocks[k].u); Put16(ni, (uint16_t)(blocks[k].f & ~0x3Fu)); }
        ni.insert(ni.end(), info.begin() + dirAt, info.end());
        std::vector<uint8_t> nic;
        Lz4Encode(ni.data(), ni.size(), nic);
        std::vector<uint8_t> back(ni.size());
        if (nic.size() >= ni.size() || Lz4Decode(nic.data(), nic.size(), back.data(), back.size()) || back != ni) return -10;
        b->head.assign(d, d + headerEnd - 20);
        Put64(b->head, (uint64_t)headerEnd + nic.size() + uTotal);
        Put32(b->head, (uint32_t)nic.size());
        Put32(b->head, (uint32_t)ni.size());
        Put32(b->head, (flags & ~0xBFu) | 0x40u | 3u);
        b->head.insert(b->head.end(), nic.begin(), nic.end());
        uint64_t h = b->head.size();
        b->segs.push_back(Seg{ 0, h, b->head.data(), -1 });
        b->segs.push_back(Seg{ h, uTotal, D, -1 });
        b->length = h + uTotal;
        free(raw);   // the data is in b->data
        b->Hold(uTotal + h);
        out = b;
        return 1;
    }
    (void)dirEnd;

    // layout: the data as it is, each serialized file with converted clips replaced by a patched copy (same size), and
    // after each .resource node with converted clips their PCM16 FSB5s, each on a kBlock boundary
    std::vector<int> order(nodes.size());
    for (size_t n = 0; n < nodes.size(); n++) order[n] = (int)n;
    std::sort(order.begin(), order.end(), [&](int a, int c) { return nodes[a].off < nodes[c].off; });
    for (size_t k = 1; k < order.size(); k++)
        if (nodes[order[k]].off < nodes[order[k - 1]].off + nodes[order[k - 1]].size) return asIs();   // overlapping nodes
    std::vector<int> patchOf(nodes.size(), -1);
    {
        int count = 0;
        for (const Conv& c : conv) if (patchOf[c.ser] < 0) patchOf[c.ser] = count++;
        b->patched.resize(count);
        for (size_t n = 0; n < nodes.size(); n++)
            if (patchOf[n] >= 0) b->patched[patchOf[n]].assign(D + nodes[n].off, D + nodes[n].off + nodes[n].size);
    }
    b->clips.resize(conv.size());
    std::vector<uint64_t> newOff(nodes.size()), newSize(nodes.size());
    std::vector<Node> extra;                      // further resource nodes (off / size: in the new data)
    std::vector<Seg> ds;
    uint64_t pres = 0, o = 0, firstInsert = UINT64_MAX;
    for (int n : order) {
        const Node& nd = nodes[n];
        if (nd.off > o) { ds.push_back(Seg{ pres, nd.off - o, D + o, -1 }); pres += nd.off - o; o = nd.off; }
        newOff[n] = pres;
        ds.push_back(Seg{ pres, nd.size, patchOf[n] >= 0 ? b->patched[patchOf[n]].data() : D + nd.off, -1 });
        pres += nd.size; o = nd.off + nd.size;
        uint64_t partStart = newOff[n];
        int part = -1;                            // -1: the node itself; else extra[part]
        size_t p = nd.name.size() > 4 && nd.name.compare(0, 4, "CAB-") == 0 ? 4 : 0;
        const char* letters = "ghijklmnopqrstuvwxyz";
        int nextLetter = 0;
        for (size_t k = 0; k < conv.size(); k++) {
            if (conv[k].res != n) continue;
            if (firstInsert == UINT64_MAX) firstInsert = pres;
            Clip& c = b->clips[k];
            c.fsb = conv[k].fsb;
            c.size = vd::PcmFsbSize(c.fsb.channels, c.fsb.frames);
            uint64_t a = AlignUp(pres, kBlock);
            if (a + c.size - partStart > kNodeMax && a > partStart) {
                // the next node: the name with one character changed to a letter no hex name has, unused in the bundle
                if (part >= 0) extra[part].size = pres - extra[part].off;
                else newSize[n] = pres - newOff[n];
                std::string name;
                while (letters[nextLetter]) {
                    name = nd.name;
                    name[p] = letters[nextLetter++];
                    bool used = false;
                    for (const Node& m : nodes) if (m.name == name) used = true;
                    for (const Node& m : extra) if (m.name == name) used = true;
                    if (!used) break;
                    name.clear();
                }
                if (name.empty()) return asIs();
                extra.push_back(Node{ a, 0, nd.flags, name });
                part = (int)extra.size() - 1;
                partStart = a;
            }
            if (a > pres) { ds.push_back(Seg{ pres, a - pres, nullptr, -1 }); pres = a; }
            memcpy(c.hash, D + nodes[n].off + conv[k].ref.offset + 36, 24);
            ds.push_back(Seg{ pres, c.size, nullptr, (int)k });
            uint8_t* ser = b->patched[patchOf[conv[k].ser]].data();
            if (part >= 0) {
                const std::string& src = conv[k].ref.source;
                if (src.size() < nd.name.size()) return asIs();
                memcpy(ser + conv[k].ref.srcAt + src.size() - nd.name.size(), extra[part].name.data(), nd.name.size());
            }
            PutField(ser + conv[k].ref.at, pres - partStart, 8, conv[k].ref.big);
            PutField(ser + conv[k].ref.at + 8, c.size, 8, conv[k].ref.big);
            PutField(ser + conv[k].ref.at + 16, 0, 4, conv[k].ref.big);   // m_CompressionFormat: PCM
            pres += c.size;
        }
        if (part >= 0) extra[part].size = pres - extra[part].off;
        else newSize[n] = pres - newOff[n];
    }
    if (o < uTotal) { ds.push_back(Seg{ pres, uTotal - o, D + o, -1 }); pres += uTotal - o; }
    uint64_t dataTotal = pres;

    // blocks info: the original blocks (stored) up to the first PCM16 clip, then stored kBlock blocks; the directory
    // with the new offsets and sizes
    std::vector<uint8_t> ni(info.begin(), info.begin() + 16);
    std::vector<uint32_t> sizes;
    uint64_t acc = 0;
    for (uint32_t k = 0; k < nBlocks && acc < firstInsert; k++) {
        uint64_t u = blocks[k].u < firstInsert - acc ? blocks[k].u : firstInsert - acc;
        if (u) sizes.push_back((uint32_t)u);
        acc += u;
    }
    if (acc < firstInsert) { sizes.push_back((uint32_t)(firstInsert - acc)); acc = firstInsert; }   // (cannot happen)
    uint64_t a0 = AlignUp(acc, kBlock);
    if (a0 > acc && a0 <= dataTotal) { sizes.push_back((uint32_t)(a0 - acc)); acc = a0; }
    while (acc < dataTotal) { uint64_t u = dataTotal - acc < kBlock ? dataTotal - acc : kBlock; sizes.push_back((uint32_t)u); acc += u; }
    uint16_t bf = nBlocks ? (uint16_t)(blocks[0].f & ~0x3Fu) : 0x40;
    Put32(ni, (uint32_t)sizes.size());
    for (uint32_t u : sizes) { Put32(ni, u); Put32(ni, u); Put16(ni, bf); }
    Put32(ni, (uint32_t)(nodes.size() + extra.size()));
    for (size_t n = 0; n < nodes.size(); n++) {
        Put64(ni, newOff[n]); Put64(ni, newSize[n]); Put32(ni, nodes[n].flags);
        ni.insert(ni.end(), nodes[n].name.begin(), nodes[n].name.end());
        ni.push_back(0);
    }
    for (const Node& e : extra) {
        Put64(ni, e.off); Put64(ni, e.size); Put32(ni, e.flags);
        ni.insert(ni.end(), e.name.begin(), e.name.end());
        ni.push_back(0);
    }
    std::vector<uint8_t> nic;
    Lz4Encode(ni.data(), ni.size(), nic);
    {
        std::vector<uint8_t> back(ni.size());
        if (nic.size() >= ni.size() || Lz4Decode(nic.data(), nic.size(), back.data(), back.size()) || back != ni) return asIs();
    }
    b->head.assign(d, d + headerEnd - 20);
    Put64(b->head, (uint64_t)headerEnd + nic.size() + dataTotal);
    Put32(b->head, (uint32_t)nic.size());
    Put32(b->head, (uint32_t)ni.size());
    Put32(b->head, (flags & ~0xBFu) | 0x40u | 3u);
    b->head.insert(b->head.end(), nic.begin(), nic.end());
    uint64_t h = b->head.size();
    b->segs.push_back(Seg{ 0, h, b->head.data(), -1 });
    for (Seg s : ds) { s.at += h; b->segs.push_back(s); }
    b->length = h + dataTotal;
    if (b->data) free(raw);
    else b->raw = raw;   // stored blocks: the data is read from the entry's bytes
    *converted = (int)conv.size();
    sClips += (int64_t)conv.size();
    sNodes += (int64_t)extra.size();
    sBundlesAudio++;
    {
        uint64_t n = (b->data ? uTotal : size) + h;
        for (const auto& x : b->patched) n += x.size();
        b->Hold(n);
    }
    RegisterAudio(b);
    out = b;
    return 1;
}

// ---- reading

static inline int64_t Ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// One clip's PCM16 FSB5: header + the decoded samples + zero padding. Null without memory.
static uint8_t* DecodeClip(Clip& c)
{
    uint8_t* buf = (uint8_t*)malloc((size_t)c.size);
    if (!buf) return nullptr;
    int64_t t0 = Ticks();
    vd::PcmFsbHeader(c.fsb.channels, c.fsb.rate, c.fsb.frames, c.hash, buf);
    size_t pcm = (size_t)(c.fsb.frames * c.fsb.channels * 2);
    uint64_t got = 0;
    int rc = vd::Decode(c.fsb, (int16_t*)(buf + 96), 0, &got);
    memset(buf + 96 + pcm, 0, (size_t)c.size - 96 - pcm);
    if (rc) sFail++;
    sDecodeTicks += Ticks() - t0;
    sFrames += (int64_t)c.fsb.frames;
    int64_t p = gPending += (int64_t)c.size;
    int64_t peak = gPendingPeak.load();
    while (p > peak && !gPendingPeak.compare_exchange_weak(peak, p)) {}
    return buf;
}

// Background: the clips of a bundle nobody decoded yet (chunk = clip), while the decoded-unread bytes stay under gCap.
static void DecodeSome(void* ctx, int lo, int hi)
{
    Bundle* b = (Bundle*)ctx;
    for (int k = lo; k < hi; k++) {
        Clip& c = b->clips[k];
        AcquireSRWLockExclusive(&b->lock);
        if (c.state != 0) { ReleaseSRWLockExclusive(&b->lock); continue; }
        if (gPending.load() + (int64_t)c.size > gCap.load()) { b->capSkipped = true; ReleaseSRWLockExclusive(&b->lock); sCapSkip++; continue; }
        c.state = 1;
        ReleaseSRWLockExclusive(&b->lock);
        uint8_t* buf = DecodeClip(c);
        AcquireSRWLockExclusive(&b->lock);
        if (buf) { c.pcm = buf; c.readTo = 0; c.done = false; c.lastRead = 0; c.state = 2; sEager++; }
        else c.state = 0;
        ReleaseSRWLockExclusive(&b->lock);
        WakeAllConditionVariable(&b->cv);
    }
}

// [off, off + len) of clip k's PCM16 FSB5 into dst. The first read of any clip of a bundle starts the others in the
// background; a clip not started is decoded right here; a buffer read to its end is freed (read again: decoded again).
static bool ClipRead(Bundle* b, int k, uint64_t off, uint8_t* dst, uint64_t len)
{
    Clip& c = b->clips[k];
    bool kick = false;
    AcquireSRWLockExclusive(&b->lock);
    if (!b->eager) { b->eager = true; kick = true; }
    for (;;) {
        if (c.state == 2) break;
        if (c.state == 0 || c.state == 3) {
            bool again = c.state == 3;
            c.state = 1;
            ReleaseSRWLockExclusive(&b->lock);
            if (kick) { kick = false; QueueDecode(b->shared_from_this()); }
            uint8_t* buf = DecodeClip(c);
            AcquireSRWLockExclusive(&b->lock);
            if (!buf) { c.state = again ? 3 : 0; ReleaseSRWLockExclusive(&b->lock); WakeAllConditionVariable(&b->cv); return false; }
            c.pcm = buf; c.readTo = 0; c.done = false; c.state = 2;
            (again ? sRedecode : sReader)++;
            WakeAllConditionVariable(&b->cv);
            break;
        }
        int64_t t0 = Ticks();
        SleepConditionVariableSRW(&b->cv, &b->lock, INFINITE, 0);
        sWaitTicks += Ticks() - t0;
    }
    c.users++;
    ReleaseSRWLockExclusive(&b->lock);
    if (kick) QueueDecode(b->shared_from_this());
    memcpy(dst, c.pcm + off, (size_t)len);
    AcquireSRWLockExclusive(&b->lock);
    c.users--;
    if (off <= c.readTo && off + len > c.readTo) c.readTo = off + len;
    if (c.readTo >= c.size) c.done = true;
    c.lastRead = b->lastRead = Ticks();
    ReleaseSRWLockExclusive(&b->lock);
    return true;
}

// The sweeper thread (4 times a second while bundles with PCM16 clips exist): frees the buffers read to their end
// kKeepMs ago and the decoded clips of bundles idle for kIdleMs, then restarts bundles whose background stopped at the
// cap.
static SRWLOCK gAudioLock = SRWLOCK_INIT;
static std::vector<std::weak_ptr<Bundle>> gAudio;
static HANDLE gSweepWake = nullptr;

static void Sweep()
{
    std::vector<std::shared_ptr<Bundle>> live;
    AcquireSRWLockExclusive(&gAudioLock);
    for (size_t i = 0; i < gAudio.size();) {
        std::shared_ptr<Bundle> b = gAudio[i].lock();
        if (!b) { gAudio[i] = gAudio.back(); gAudio.pop_back(); continue; }
        live.push_back(std::move(b));
        i++;
    }
    ReleaseSRWLockExclusive(&gAudioLock);
    int64_t now = Ticks(), keep = kKeepMs * gFreq.QuadPart / 1000, idle = kIdleMs * gFreq.QuadPart / 1000;
    for (auto& b : live) {
        AcquireSRWLockExclusive(&b->lock);
        bool bundleIdle = b->lastRead && now - b->lastRead > idle;
        for (Clip& c : b->clips) {
            if (c.state != 2 || c.users) continue;
            if (c.done ? now - c.lastRead <= keep : !bundleIdle) continue;
            (c.done ? sFreed : sDropped)++;
            free(c.pcm); c.pcm = nullptr; c.state = 3; c.readTo = 0; c.done = false;
            gPending -= (int64_t)c.size;
        }
        ReleaseSRWLockExclusive(&b->lock);
    }
    if (gPending.load() < gCap.load() / 2)
        for (auto& b : live) {
            bool re = false;
            AcquireSRWLockExclusive(&b->lock);
            if (b->capSkipped) { b->capSkipped = false; re = true; }
            ReleaseSRWLockExclusive(&b->lock);
            if (re) QueueDecode(b);
        }
}

static DWORD WINAPI Sweeper(void*)
{
    for (;;) {
        AcquireSRWLockShared(&gAudioLock);
        bool any = !gAudio.empty();
        ReleaseSRWLockShared(&gAudioLock);
        if (any) Sleep(250);
        else WaitForSingleObject(gSweepWake, INFINITE);
        Sweep();
    }
}

static void RegisterAudio(const std::shared_ptr<Bundle>& b)
{
    AcquireSRWLockExclusive(&gAudioLock);
    gAudio.push_back(b);
    ReleaseSRWLockExclusive(&gAudioLock);
    if (gSweepWake) SetEvent(gSweepWake);
}

static int64_t BundleRead(Bundle* b, uint64_t pos, uint8_t* dst, int64_t n)
{
    if (n <= 0 || pos >= b->length) return 0;
    if ((uint64_t)n > b->length - pos) n = (int64_t)(b->length - pos);
    size_t lo = 0, hi = b->segs.size();
    while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (b->segs[mid].at <= pos) lo = mid; else hi = mid; }
    int64_t done = 0;
    for (size_t i = lo; done < n && i < b->segs.size(); i++) {
        const Seg& s = b->segs[i];
        uint64_t in = pos + (uint64_t)done - s.at;
        uint64_t take = s.len - in < (uint64_t)(n - done) ? s.len - in : (uint64_t)(n - done);
        if (s.clip >= 0) { if (!ClipRead(b, s.clip, in, dst + done, take)) return -1; }
        else if (s.mem) memcpy(dst + done, s.mem + in, (size_t)take);
        else memset(dst + done, 0, (size_t)take);
        done += (int64_t)take;
    }
    return done;
}

// One bundle: the zip entry (as SL_ReadZipEntry reads it) -> its Bundle. 1 / 2 / negative as BuildBundle; the read's
// codes -1..-7.
static int BuildFromEntry(const wchar_t* zip, int64_t hdr, int64_t csize, int method, int64_t size, bool audio, std::shared_ptr<Bundle>& out, int* converted)
{
    *converted = 0;
    if (size < 0 || size > 0x7fffffff) return -6;
    uint8_t* buf = (uint8_t*)malloc(size ? (size_t)size : 1);
    if (!buf) return -4;
    int64_t r = SL_ReadZipEntry(zip, hdr, csize, method, buf, size);
    if (r != size) { free(buf); return (int)(r < 0 ? r : -6); }
    int result = BuildBundle(buf, (size_t)size, audio, out, converted);
    if (result < 0) free(buf);   // kept bundles own buf (or freed it)
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Jobs

struct Job {
    std::wstring zip;
    int64_t hdr, csize, size;
    int method;
    bool audio;
    volatile LONG state;    // 0 queued, 1 running, 2 done
    int result, clips;
    double ms;
    std::shared_ptr<Bundle> bundle;   // until SL_BundleFree
    bool freed;
};

typedef int (*CbTaskFn)(const char*, int);
typedef void (*CbRunFn)(int, void (*)(void*, int, int), void*, int, int, int);

static SRWLOCK gLock = SRWLOCK_INIT;
static std::vector<Job*> gJobs;
static std::deque<std::shared_ptr<Bundle>> gDecodeQ;
static size_t gNext = 0;
static HANDLE gWake = nullptr;
static HANDLE gThread = nullptr;
static CbRunFn gRun = nullptr;
static int gTask = -1;

static Job* Take()
{
    AcquireSRWLockExclusive(&gLock);
    Job* j = gNext < gJobs.size() ? gJobs[gNext++] : nullptr;
    ReleaseSRWLockExclusive(&gLock);
    return j;
}

static void RunJob(Job* j)
{
    InterlockedExchange(&j->state, 1);
    int64_t a = Ticks();
    std::shared_ptr<Bundle> b;
    int clips = 0;
    int r = BuildFromEntry(j->zip.c_str(), j->hdr, j->csize, j->method, j->size, j->audio, b, &clips);
    AcquireSRWLockExclusive(&gLock);
    j->result = r < 0 ? r : (b ? r : -10);
    j->clips = clips;
    if (r > 0 && !j->freed) j->bundle = b;
    ReleaseSRWLockExclusive(&gLock);
    j->ms = (double)(Ticks() - a) * 1000.0 / (double)gFreq.QuadPart;
    MemoryBarrier();
    InterlockedExchange(&j->state, 2);
}

// One chunk = one job (the next in submission order), so a worker can leave for higher-priority work between jobs.
static void RunSome(void*, int b, int e)
{
    for (int i = b; i < e; i++) {
        Job* j = Take();
        if (!j) return;
        RunJob(j);
    }
}

// Each round hands every job queued so far to CPU Boost as its own chunk; jobs queued meanwhile go in the next round.
// (1.3.0 ran kSlots chunks that each pulled jobs until the queue was empty: requests are queued one by one from the
// main thread, so all but one chunk found it empty at once and left.) Then the bundles whose clips are being read:
// each one's clips as chunks.
static DWORD WINAPI Driver(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WaitForSingleObject(gWake, INFINITE);
        for (;;) {
            AcquireSRWLockExclusive(&gLock);
            size_t waiting = gJobs.size() - gNext;
            std::shared_ptr<Bundle> dec;
            if (!waiting && !gDecodeQ.empty()) { dec = gDecodeQ.front(); gDecodeQ.pop_front(); }
            ReleaseSRWLockExclusive(&gLock);
            if (waiting) {
                int n = (int)(waiting < 4096 ? waiting : 4096);
                if (gRun) gRun(gTask, RunSome, nullptr, 0, n, 1);
                else RunSome(nullptr, 0, n);
                continue;
            }
            if (!dec) break;
            int n = (int)dec->clips.size();
            if (gRun) gRun(gTask, DecodeSome, dec.get(), 0, n, 1);
            else DecodeSome(dec.get(), 0, n);
        }
    }
}

static bool StartDriver()
{
    if (gThread) return true;
    QueryPerformanceFrequency(&gFreq);
    HMODULE cb = GetModuleHandleW(L"VaMCpuBoostNative.bin");
    if (cb) {
        auto api = (int (*)())GetProcAddress(cb, "CB_ApiVersion");
        auto task = (CbTaskFn)GetProcAddress(cb, "CB_Task");
        auto run = (CbRunFn)GetProcAddress(cb, "CB_Run");
        if (api && task && run && api() >= 5) { gTask = task("scene load bundles", 0); gRun = run; }
    }
    gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gSweepWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gWake || !gSweepWake) return false;
    HANDLE sweeper = CreateThread(nullptr, 0, Sweeper, nullptr, 0, nullptr);
    if (!sweeper) return false;
    CloseHandle(sweeper);
    gThread = CreateThread(nullptr, 0, Driver, nullptr, 0, nullptr);
    return gThread != nullptr;
}

static void QueueDecode(std::shared_ptr<Bundle> b)
{
    AcquireSRWLockExclusive(&gLock);
    gDecodeQ.push_back(std::move(b));
    ReleaseSRWLockExclusive(&gLock);
    if (gWake) SetEvent(gWake);
}

static std::shared_ptr<Bundle> Get(int id)
{
    std::shared_ptr<Bundle> b;
    AcquireSRWLockShared(&gLock);
    if (id >= 1 && (size_t)id <= gJobs.size()) b = gJobs[id - 1]->bundle;
    ReleaseSRWLockShared(&gLock);
    return b;
}

extern "C" __declspec(dllexport) int SL_BundleApi() { return 2; }

// Queues one bundle (the arguments as for SL_ReadZipEntry, plus the entry's size; audio: make DecompressOnLoad Vorbis
// clips PCM16). Returns the job id (>= 1), or -1 when the background thread cannot start. Main thread only.
extern "C" __declspec(dllexport) int SL_BundleSubmit2(const wchar_t* zipPath, int64_t headerOffset, int64_t compSize, int method,
                                                      int64_t size, int audio)
{
    if (!StartDriver()) return -1;
    Job* j = new Job();
    j->zip = zipPath;
    j->hdr = headerOffset; j->csize = compSize; j->size = size; j->method = method; j->audio = audio != 0;
    j->state = 0; j->result = 0; j->clips = 0; j->ms = 0; j->freed = false;
    AcquireSRWLockExclusive(&gLock);
    gJobs.push_back(j);
    int id = (int)gJobs.size();
    ReleaseSRWLockExclusive(&gLock);
    SetEvent(gWake);
    return id;
}

// 0 while queued or running; then the job's result (1 rebuilt, 2 as it is, negative failed: nothing kept). length:
// the bytes Unity reads; ms: the job's time; clips: clips made PCM16.
extern "C" __declspec(dllexport) int SL_BundleResult2(int id, int64_t* length, double* ms, int* clips)
{
    AcquireSRWLockShared(&gLock);
    Job* j = id >= 1 && (size_t)id <= gJobs.size() ? gJobs[id - 1] : nullptr;
    ReleaseSRWLockShared(&gLock);
    if (!j) return -30;
    if (j->state != 2) return 0;
    MemoryBarrier();
    std::shared_ptr<Bundle> b = Get(id);
    if (length) *length = b ? (int64_t)b->length : 0;
    if (ms) *ms = j->ms;
    if (clips) *clips = j->clips;
    return j->result;
}

// Bytes [pos, pos + count) of a ready bundle into dst (any thread). The count read (0 at the end), -1 on failure.
extern "C" __declspec(dllexport) int SL_BundleRead(int id, int64_t pos, uint8_t* dst, int count)
{
    std::shared_ptr<Bundle> b = Get(id);
    if (!b || pos < 0 || count < 0) return -1;
    return (int)BundleRead(b.get(), (uint64_t)pos, dst, count);
}

// Its memory goes (once background decodes of it finish); any time, also before the job is done.
extern "C" __declspec(dllexport) void SL_BundleFree(int id)
{
    std::shared_ptr<Bundle> drop;
    AcquireSRWLockExclusive(&gLock);
    if (id >= 1 && (size_t)id <= gJobs.size()) { Job* j = gJobs[id - 1]; j->freed = true; drop.swap(j->bundle); }
    ReleaseSRWLockExclusive(&gLock);
}

// Whether CPU Boost runs the jobs (1) or our one thread does (0); -1 before the first job.
extern "C" __declspec(dllexport) int SL_BundleOnCpuBoost() { return gThread ? (gRun ? 1 : 0) : -1; }

// on: make clips PCM16 at all; cap: decoded bytes not read yet that background decoding stops at.
extern "C" __declspec(dllexport) void SL_AudioConfig(int on, int64_t cap) { gAudioOn = on; if (cap > 0) gCap = cap; }

// out[0..13]: clips made PCM16, kept Vorbis, decoded in the background, decoded by the reader, decoded again after
// being read, background skips at the cap, buffers freed after their last byte was read, decodes with an error,
// decode CPU ms, reader waits ms, frames decoded, bundles with PCM16 clips, decoded-unread bytes now, their peak.
extern "C" __declspec(dllexport) int SL_AudioStats(double* out, int n)
{
    double tick = 1000.0 / (double)(gFreq.QuadPart ? gFreq.QuadPart : 1);
    double v[18] = { (double)sClips, (double)sKept, (double)sEager, (double)sReader, (double)sRedecode, (double)sCapSkip, (double)sFreed,
                     (double)sFail, sDecodeTicks * tick, sWaitTicks * tick, (double)sFrames, (double)sBundlesAudio,
                     (double)gPending.load(), (double)gPendingPeak.load(), (double)sNodes, (double)sDropped,
                     (double)gHeld.load(), (double)gHeldPeak.load() };
    for (int i = 0; i < n && i < 18; i++) out[i] = v[i];
    return 18;
}

// Offline checks: a whole file as an entry's bytes -> a ready bundle id (no background driver needed; reads decode on
// the reading thread, plus the background driver for the rest).
extern "C" __declspec(dllexport) int SL_BundleFromFile(const wchar_t* path, int audio, int* result, int* clips)
{
    if (!StartDriver()) return -1;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -2;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    uint8_t* buf = sz.QuadPart > 0 && sz.QuadPart < 0x7fffffff ? (uint8_t*)malloc((size_t)sz.QuadPart) : nullptr;
    DWORD got = 0;
    bool ok = buf && ReadFile(f, buf, (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart;
    CloseHandle(f);
    if (!ok) { free(buf); return -3; }
    std::shared_ptr<Bundle> b;
    int conv = 0;
    int r = BuildBundle(buf, (size_t)sz.QuadPart, audio != 0, b, &conv);
    if (r < 0) free(buf);
    if (result) *result = r;
    if (clips) *clips = conv;
    Job* j = new Job();
    j->state = 2; j->result = r; j->clips = conv; j->ms = 0; j->freed = false; j->bundle = r > 0 ? b : nullptr;
    AcquireSRWLockExclusive(&gLock);
    gJobs.push_back(j);
    int id = (int)gJobs.size();
    ReleaseSRWLockExclusive(&gLock);
    return id;
}

// Raw LZMA (offline checks): 0 ok, negative failed.
extern "C" __declspec(dllexport) int SL_LzmaDecode(const uint8_t* src, int64_t srcLen, uint8_t* out, int64_t outSize)
{
    return lz::Decode(src, (size_t)srcLen, out, (size_t)outSize);
}

// Offline checks: one FSB5 (Vorbis) -> int16 the way a PCM16 clip is made. 0 ok.
extern "C" __declspec(dllexport) int SL_DecodeFsb(const uint8_t* fsb, int64_t n, int16_t* out, int64_t outValues)
{
    vd::Fsb f;
    if (!vd::ParseFsb(fsb, (size_t)n, f) || (int64_t)(f.frames * f.channels) > outValues) return -1;
    uint64_t got;
    return vd::Decode(f, out, 0, &got);
}
