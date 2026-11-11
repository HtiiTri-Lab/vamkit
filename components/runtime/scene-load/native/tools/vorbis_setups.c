/* Vorbis setup headers as libvorbis 1.3.7's encoder makes them, for every (channels, rate, VBR quality) on a fine
   grid: writes each distinct setup header once (CRC32 of its bytes = how FMOD's FSB5 names it).
   Output file: records { uint32 crc; int32 channels; int32 rate; float q; uint32 size; uint8 data[size] }.
   Usage: vorbis_setups.exe <out.bin> [qstep]                                                                  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisenc.h>

static unsigned crcTable[256];
static void CrcInit(void)
{
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[i] = c;
    }
}
static unsigned Crc32(const unsigned char* p, long n)
{
    unsigned c = 0xFFFFFFFFu;
    for (long i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

#define MAXSEEN 200000
static unsigned seen[MAXSEEN];
static int nSeen = 0;
static int Seen(unsigned c)
{
    for (int i = 0; i < nSeen; i++) if (seen[i] == c) return 1;
    if (nSeen < MAXSEEN) seen[nSeen++] = c;
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vorbis_setups out.bin [qstep]\n"); return 2; }
    float step = argc > 2 ? (float)atof(argv[2]) : 0.001f;
    FILE* f = fopen(argv[1], "wb");
    if (!f) return 1;
    CrcInit();
    static const int rates[] = { 8000, 11000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000 };
    int written = 0, tried = 0;
    for (int ch = 1; ch <= 8; ch++) {
        for (int r = 0; r < (int)(sizeof rates / sizeof rates[0]); r++) {
            for (int k = 0;; k++) {
                float q = -0.1f + k * step;
                if (q > 1.0001f) break;
                vorbis_info vi;
                vorbis_info_init(&vi);
                tried++;
                if (vorbis_encode_init_vbr(&vi, ch, rates[r], q) != 0) { vorbis_info_clear(&vi); continue; }
                vorbis_comment vc; vorbis_comment_init(&vc);
                vorbis_dsp_state vd;
                if (vorbis_analysis_init(&vd, &vi) != 0) { vorbis_comment_clear(&vc); vorbis_info_clear(&vi); continue; }
                ogg_packet id, comm, code;
                if (vorbis_analysis_headerout(&vd, &vc, &id, &comm, &code) == 0) {
                    unsigned c = Crc32(code.packet, code.bytes);
                    if (!Seen(c)) {
                        int sz = (int)code.bytes;
                        fwrite(&c, 4, 1, f); fwrite(&ch, 4, 1, f); fwrite(&rates[r], 4, 1, f); fwrite(&q, 4, 1, f); fwrite(&sz, 4, 1, f);
                        fwrite(code.packet, 1, sz, f);
                        written++;
                    }
                }
                vorbis_dsp_clear(&vd);
                vorbis_comment_clear(&vc);
                vorbis_info_clear(&vi);
            }
        }
        fprintf(stderr, "channels %d: %d distinct setups so far (%d tried)\n", ch, written, tried);
    }
    fclose(f);
    printf("distinct setups %d of %d encoder setups\n", written, tried);
    return 0;
}
