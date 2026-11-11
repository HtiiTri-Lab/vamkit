// FSB5 Vorbis -> int16, with libvorbis 1.3.7 (third_party). FMOD's FSB5 keeps only the Vorbis audio packets (16-bit
// size + packet) and names the stripped setup header by CRC32; the id header is rebuilt from channels / rate / block
// sizes. Setup headers come from libvorbis's own encoder (vorbis_encode_init_vbr at the channels / rate / quality in
// vorbis_setup_index.h reproduces FMOD's bytes: same CRC32), made once per CRC and kept.
#include "vorbisdec.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

#include "vorbis_setup_index.h"

namespace vd {

static uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t Le64(const uint8_t* p) { return Le32(p) | (uint64_t)Le32(p + 4) << 32; }

static const int kFsbRates[] = { 0, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };

bool ParseFsb(const uint8_t* p, size_t n, Fsb& f)
{
    if (n < 68 || memcmp(p, "FSB5", 4) != 0) return false;
    f.version = (int)Le32(p + 4);
    uint32_t samples = Le32(p + 8), shsz = Le32(p + 12), nmsz = Le32(p + 16), dsz = Le32(p + 20);
    f.mode = (int)Le32(p + 24);
    size_t hdr = f.version == 1 ? 60 : 64;
    if (samples != 1 || hdr + shsz + nmsz + dsz > n || shsz < 8) return false;
    uint64_t sh = Le64(p + hdr);
    int fi = (int)(sh >> 1 & 15);
    f.rate = fi < (int)(sizeof kFsbRates / sizeof kFsbRates[0]) ? kFsbRates[fi] : 0;
    f.channels = (sh >> 5 & 1) ? 2 : 1;
    uint64_t dataOff = (sh >> 6 & ((1ull << 28) - 1)) * 32;
    f.frames = sh >> 34;
    f.setupCrc = 0;
    size_t q = hdr + 8, end = hdr + shsz;
    bool more = sh & 1;
    while (more) {
        if (q + 4 > end) return false;
        uint32_t c = Le32(p + q); q += 4;
        more = c & 1;
        uint32_t size = c >> 1 & 0xFFFFFF, type = c >> 25;
        if (q + size > end) return false;
        if (type == 1 && size >= 1) f.channels = p[q];                 // channel count chunk
        else if (type == 2 && size >= 4) f.rate = (int)Le32(p + q);    // frequency chunk
        else if (type == 11 && size >= 4) f.setupCrc = Le32(p + q);    // VORBISDATA: setup CRC (+ seek table)
        q += size;
    }
    size_t ds = hdr + shsz + nmsz + dataOff;
    if (dataOff > dsz) return false;
    f.data = p + ds;
    f.dataSize = dsz - dataOff;
    return f.rate > 0;
}

struct Setup {
    std::vector<uint8_t> id, setup;   // the encoder's id header (block sizes) and the setup header
};
static std::mutex gSetupLock;
static std::map<uint32_t, std::shared_ptr<Setup>> gSetups;   // null: tried, cannot be made

static uint32_t Crc32(const uint8_t* p, size_t n)
{
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    });
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static std::shared_ptr<Setup> Make(uint32_t crc)
{
    const VorbisSetupIndex* e = nullptr;
    for (const auto& x : kVorbisSetups) if (x.crc == crc) { e = &x; break; }
    if (!e) return nullptr;
    float q;
    memcpy(&q, &e->qbits, 4);
    std::shared_ptr<Setup> out;
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, e->channels, e->rate, q) == 0) {
        vorbis_dsp_state d;
        if (vorbis_analysis_init(&d, &vi) == 0) {
            vorbis_comment vc;
            vorbis_comment_init(&vc);
            ogg_packet id, comm, code;
            if (vorbis_analysis_headerout(&d, &vc, &id, &comm, &code) == 0 && Crc32(code.packet, (size_t)code.bytes) == crc) {
                out = std::make_shared<Setup>();
                out->id.assign(id.packet, id.packet + id.bytes);
                out->setup.assign(code.packet, code.packet + code.bytes);
            }
            vorbis_comment_clear(&vc);
            vorbis_dsp_clear(&d);
        }
    }
    vorbis_info_clear(&vi);
    return out;
}

static std::shared_ptr<Setup> GetSetup(uint32_t crc)
{
    {
        std::lock_guard<std::mutex> g(gSetupLock);
        auto it = gSetups.find(crc);
        if (it != gSetups.end()) return it->second;
    }
    std::shared_ptr<Setup> s = Make(crc);   // a few ms; two threads may both make it, the result is the same
    std::lock_guard<std::mutex> g(gSetupLock);
    return gSetups.emplace(crc, s).first->second;
}

bool HaveSetup(uint32_t crc, int, int)
{
    for (const auto& x : kVorbisSetups) if (x.crc == crc) return true;
    return false;
}

// float -> int16. 0: what FMOD does for a DecompressOnLoad Vorbis clip (checked against Unity's GetData on 1310 clips:
// (int)(x * 32767), truncated toward zero, x clamped to [-1, 1] first: a clipped sample is -32767, never
// -32768, as the game's own decode shows); the others only for the offline test.
static inline int16_t Convert(float x, int variant)
{
    float s;
    long v;
    switch (variant) {
    case 1: s = x * 32768.f; v = lrintf(s); break;                      // round to nearest even, x 32768 (vorbisfile)
    case 2: s = x * 32767.f; v = lrintf(s); break;                      // round, x 32767
    case 3: s = x * 32768.f; v = (long)s; break;                        // truncate, x 32768
    case 4: s = x * 32768.f; v = (long)floorf(s + 0.5f); break;         // round half up, x 32768
    default: s = x * 32767.f; v = (long)s; return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);   // FMOD
    }
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

int Decode(const Fsb& f, int16_t* out, int variant, uint64_t* decoded) { return DecodeImpl(f, out, variant, decoded, nullptr); }

int DecodeFloat(const Fsb& f, std::vector<float>& all) { uint64_t d; return DecodeImpl(f, nullptr, 0, &d, &all); }

int DecodeImpl(const Fsb& f, int16_t* out, int variant, uint64_t* decoded, std::vector<float>* allFloats)
{
    if (decoded) *decoded = 0;
    if (f.mode != 15) return -1;
    std::shared_ptr<Setup> s = GetSetup(f.setupCrc);
    if (!s) return -2;
    // id header: the encoder's (block sizes), with this clip's channels and rate
    std::vector<uint8_t> id = s->id;
    if (id.size() < 30) return -1;
    id[11] = (uint8_t)f.channels;
    uint32_t r = (uint32_t)f.rate;
    id[12] = (uint8_t)r; id[13] = (uint8_t)(r >> 8); id[14] = (uint8_t)(r >> 16); id[15] = (uint8_t)(r >> 24);
    static const uint8_t comment[] = { 3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1 };

    vorbis_info vi;
    vorbis_comment vc;
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    int rc = 0;
    ogg_packet op = {};
    op.packet = id.data(); op.bytes = (long)id.size(); op.b_o_s = 1; op.granulepos = -1; op.packetno = 0;
    if (vorbis_synthesis_headerin(&vi, &vc, &op) != 0) rc = -1;
    op.packet = (unsigned char*)comment; op.bytes = sizeof comment; op.b_o_s = 0; op.packetno = 1;
    if (!rc && vorbis_synthesis_headerin(&vi, &vc, &op) != 0) rc = -1;
    op.packet = s->setup.data(); op.bytes = (long)s->setup.size(); op.packetno = 2;
    if (!rc && vorbis_synthesis_headerin(&vi, &vc, &op) != 0) rc = -1;
    if (rc) { vorbis_comment_clear(&vc); vorbis_info_clear(&vi); return rc; }

    vorbis_dsp_state d;
    vorbis_block b;
    vorbis_synthesis_init(&d, &vi);
    vorbis_block_init(&d, &b);
    const int ch = f.channels;
    const uint64_t want = f.frames;
    uint64_t got = 0, all = 0;
    size_t q = 0;
    long pno = 3;
    while (q + 2 <= f.dataSize) {
        unsigned n = f.data[q] | f.data[q + 1] << 8;
        q += 2;
        if (n == 0 || q + n > f.dataSize) break;
        op.packet = (unsigned char*)f.data + q; op.bytes = n; op.b_o_s = 0; op.packetno = pno++; op.granulepos = -1;
        q += n;
        op.e_o_s = 0;
        if (vorbis_synthesis(&b, &op) != 0) { rc = -3; break; }
        vorbis_synthesis_blockin(&d, &b);
        float** pcm;
        int k;
        while ((k = vorbis_synthesis_pcmout(&d, &pcm)) > 0) {
            if (allFloats)
                for (int i = 0; i < k; i++) for (int c = 0; c < ch; c++) allFloats->push_back(pcm[c][i]);
            all += (uint64_t)k;
            uint64_t take = !out ? 0 : got + (uint64_t)k <= want ? (uint64_t)k : want - got;
            for (uint64_t i = 0; i < take; i++)
                for (int c = 0; c < ch; c++) out[(got + i) * ch + c] = Convert(pcm[c][i], variant);
            got += take;
            vorbis_synthesis_read(&d, k);
        }
    }
    vorbis_block_clear(&b);
    vorbis_dsp_clear(&d);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    if (decoded) *decoded = all;
    if (rc) return rc;
    if (!out) return 0;
    if (got < want) {
        memset(out + got * ch, 0, (size_t)((want - got) * ch) * 2);
        return -4;
    }
    return 0;
}

// Unity's own PCM16 FSB5 (a CompressedInMemory PCM clip of the cache): header 60 bytes (FSB5, version 1,
// one sample, sample headers 36: the 8-byte sample header padded so the data starts at 96, no names, data size, mode 2,
// 8 zero bytes, 24 bytes of hash), then the data: frames * channels int16, size rounded up to 32 plus 32 (zeros).
size_t PcmFsbSize(int channels, uint64_t frames)
{
    size_t data = (size_t)(frames * channels * 2);
    return 96 + ((data + 31) & ~(size_t)31) + 32;
}

int FsbRateIndex(int rate)
{
    for (int i = 1; i < (int)(sizeof kFsbRates / sizeof kFsbRates[0]); i++) if (kFsbRates[i] == rate) return i;
    return 0;
}

size_t PcmFsbHeader(int channels, int rate, uint64_t frames, const uint8_t* hash24, uint8_t* o)
{
    memset(o, 0, 96);
    size_t dsz = PcmFsbSize(channels, frames) - 96;
    auto put32 = [&](size_t at, uint32_t v) { o[at] = (uint8_t)v; o[at + 1] = (uint8_t)(v >> 8); o[at + 2] = (uint8_t)(v >> 16); o[at + 3] = (uint8_t)(v >> 24); };
    memcpy(o, "FSB5", 4);
    put32(4, 1); put32(8, 1); put32(12, 36); put32(16, 0); put32(20, (uint32_t)dsz); put32(24, 2);
    if (hash24) memcpy(o + 36, hash24, 24);
    uint64_t sh = (frames << 34) | ((uint64_t)(channels == 2) << 5) | ((uint64_t)FsbRateIndex(rate) << 1);
    for (int i = 0; i < 8; i++) o[60 + i] = (uint8_t)(sh >> (8 * i));
    return 96;
}

}  // namespace vd
