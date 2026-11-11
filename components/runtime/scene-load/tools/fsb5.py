"""FSB5 (FMOD sound bank, as Unity 2018.1 stores AudioClip data in a bundle's .resource): header, the sample header's
packed fields and extra chunks (Vorbis: CRC32 of the stripped setup header, seek table), packets.
    python -I fsb5.py <bundle file> [how many clips]      (prints the first clips' FSB5 layout)"""
import collections, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_scan as A                                    # noqa: E402

FREQ = {1: 8000, 2: 11000, 3: 11025, 4: 16000, 5: 22050, 6: 24000, 7: 32000, 8: 44100, 9: 48000, 10: 96000}
MODES = {1: 'PCM8', 2: 'PCM16', 3: 'PCM24', 4: 'PCM32', 5: 'PCMFLOAT', 6: 'GCADPCM', 7: 'IMAADPCM', 11: 'MPEG', 15: 'VORBIS'}


def parse(b):
    """b: one clip's FSB5 bytes -> dict (header fields, chunks, data start)."""
    assert b[:4] == b'FSB5', b[:4]
    ver, nsamp, shsz, nmsz, dsz, mode = struct.unpack('<6I', b[4:28])
    hdr = 60 if ver == 1 else 64
    out = {'version': ver, 'samples': nsamp, 'sample_header_size': shsz, 'names': nmsz, 'data_size': dsz,
           'mode': MODES.get(mode, mode), 'raw_after_mode': b[28:hdr].hex(), 'header_size': hdr}
    q = hdr
    sh = struct.unpack('<Q', b[q:q + 8])[0]; q += 8
    out['extra'] = sh & 1
    out['freq_index'] = (sh >> 1) & 15
    out['freq'] = FREQ.get(out['freq_index'], out['freq_index'])
    out['channels'] = 2 if (sh >> 5) & 1 else 1
    out['data_offset'] = ((sh >> 6) & ((1 << 28) - 1)) * 32
    out['frames'] = sh >> 34
    chunks = []
    more = sh & 1
    while more:
        c = struct.unpack('<I', b[q:q + 4])[0]; q += 4
        more = c & 1
        size = (c >> 1) & 0xFFFFFF
        typ = c >> 25
        chunks.append((typ, b[q:q + size]))
        q += size
    out['chunks'] = chunks
    out['data_start'] = hdr + shsz + nmsz + out['data_offset']
    return out


def vorbis_crc(p):
    for typ, d in p['chunks']:
        if typ == 11:
            return struct.unpack('<I', d[:4])[0]
    return None


def packets(b, p):
    """Vorbis packets: 16-bit LE size + data, until data_size (a 0 size ends it)."""
    q = p['data_start']
    end = q + p['data_size']
    out = []
    while q + 2 <= end:
        n = struct.unpack('<H', b[q:q + 2])[0]; q += 2
        if n == 0:
            break
        out.append(b[q:q + n]); q += n
    return out


def clip_fsb(a, nodes_by_name, c):
    node = nodes_by_name[c['source'].split('/')[-1]] if c['source'].split('/')[-1] in nodes_by_name else None
    if node is None:
        return None
    return a.read(node[0] + c['offset'], c['bytes'])


def bundle_clips(path):
    a = A.Archive(path)
    by_name = {x[3]: x for x in a.nodes}
    out = []
    for off, sz, fl, name in a.nodes:
        if not fl & 4:
            continue
        s = a.read(off, sz)
        th, cl = A.clips(s)
        for c in cl:
            out.append((c, clip_fsb(a, by_name, c)))
    return a, out


def main():
    a, cl = bundle_clips(sys.argv[1])
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    crcs = collections.Counter(); modes = collections.Counter(); chunk_types = collections.Counter()
    for i, (c, b) in enumerate(cl):
        if b is None:
            continue
        p = parse(b)
        modes[p['mode']] += 1
        crcs[vorbis_crc(p)] += 1
        for t, d in p['chunks']:
            chunk_types[(t, len(d) if t != 11 else 'seek')] += 1
        if i < want:
            pk = packets(b, p) if p['mode'] == 'VORBIS' else []
            print(c['name'], p['mode'], p['channels'], p['freq'], 'frames', p['frames'], 'clip samples', round(c['length'] * c['hz']),
                  'data', p['data_size'], 'chunks', [(t, len(d)) for t, d in p['chunks']], 'crc', hex(vorbis_crc(p) or 0),
                  'packets', len(pk), 'first sizes', [len(x) for x in pk[:4]], 'after mode', p['raw_after_mode'][:16])
    print('modes', dict(modes)); print('vorbis setup crcs', {hex(k) if k else k: v for k, v in crcs.items()})
    print('chunk types', dict(chunk_types))


if __name__ == '__main__':
    main()
