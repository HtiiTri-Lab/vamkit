// FSB5 Vorbis (how Unity 2018.1 stores a Vorbis AudioClip) decoded to the int16 samples FMOD keeps for a
// DecompressOnLoad clip, on any thread; and the PCM16 FSB5 that holds them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vd {

struct Fsb {
    int version = 0, mode = 0, channels = 0, rate = 0;     // mode 15 Vorbis, 2 PCM16
    uint64_t frames = 0;
    uint32_t setupCrc = 0;                                  // Vorbis: CRC32 of the setup header (VORBISDATA chunk)
    const uint8_t* data = nullptr;                          // the sample's data (Vorbis: u16 size + packet, ...)
    size_t dataSize = 0;
};

// One clip's FSB5 bytes (one sample) -> fields; false when it is not that.
bool ParseFsb(const uint8_t* p, size_t n, Fsb& out);

// Whether a setup header for this CRC can be made (libvorbis's encoder, see vorbis_setup_index.h). Thread-safe.
bool HaveSetup(uint32_t crc, int channels, int rate);

// Decodes a Vorbis FSB to frames * channels int16, interleaved. variant: how a float sample becomes int16 (0 = FMOD's;
// others for the offline test). Returns 0, or < 0: -1 not Vorbis / bad header, -2 no setup, -3 a
// packet libvorbis rejects, -4 fewer frames than the header says. *decoded: frames libvorbis gave (before trimming).
int Decode(const Fsb& f, int16_t* out, int variant, uint64_t* decoded);
int DecodeImpl(const Fsb& f, int16_t* out, int variant, uint64_t* decoded, std::vector<float>* allFloats);
int DecodeFloat(const Fsb& f, std::vector<float>& all);   // test: every frame libvorbis gives, as float, interleaved

// Unity's PCM16 FSB5 for frames * channels int16 at rate (1 or 2 channels, a rate FSB5 has an index for): its size, and
// its 96 header bytes (hash24: the 24 header bytes 36..59 to carry over, or null). The data follows at 96.
size_t PcmFsbSize(int channels, uint64_t frames);
int FsbRateIndex(int rate);   // 0: none
size_t PcmFsbHeader(int channels, int rate, uint64_t frames, const uint8_t* hash24, uint8_t* out);

}  // namespace vd
