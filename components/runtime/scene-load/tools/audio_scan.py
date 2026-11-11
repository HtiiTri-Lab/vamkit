"""AudioClips in UnityFS bundle files (VaM Scene Load's cache files, or any bundle with stored / LZ4 blocks): for each
serialized file of the archive, its AudioClip objects read with Unity 2018.1's layout (checked: the read ends exactly at
the object's size), the type hash of class 83, and which clips a 'load in background' rewrite would change
(DecompressOnLoad + PreloadAudioData + not LoadInBackground).
    python -I audio_scan.py <file or dir> [--list] [--json out.json]"""
import collections, json, os, struct, sys

LOAD = {0: 'DecompressOnLoad', 1: 'CompressedInMemory', 2: 'Streaming'}
FMT = {0: 'PCM', 1: 'Vorbis', 2: 'ADPCM', 3: 'MP3', 4: 'VAG', 5: 'HEVAG', 6: 'XMA', 7: 'AAC', 8: 'GCADPCM', 9: 'ATRAC9'}


def lz4_block(src, out_size):
    dst = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        dst += src[i:i + lit]; i += lit
        if i >= n: break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:
            for k in range(ml):
                dst.append(dst[start + k])
    if len(dst) != out_size:
        raise ValueError('lz4 size')
    return bytes(dst)


class Archive:
    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(4096)
        if head[:8] != b'UnityFS\0':
            raise ValueError('not UnityFS')
        p = 12
        for _ in range(2):
            p = head.index(b'\0', p) + 1
        self.size, csz, usz, self.flags = struct.unpack('>qIII', head[p:p + 20])
        p += 20
        if self.flags & 0x80:
            self.f.seek(self.size - csz); raw = self.f.read(csz); self.data_start = p
        else:
            self.f.seek(p); raw = self.f.read(csz); self.data_start = p + csz
        comp = self.flags & 0x3F
        info = raw if comp == 0 else lz4_block(raw, usz) if comp in (2, 3) else None
        if info is None:
            raise ValueError('blocks info compression %d' % comp)
        self.hash = info[:16]
        q = 16
        n = struct.unpack('>i', info[q:q + 4])[0]; q += 4
        self.blocks = []
        uo = co = 0
        for _ in range(n):
            u, c, fl = struct.unpack('>IIH', info[q:q + 10]); q += 10
            self.blocks.append((uo, u, co, c, fl & 0x3F))
            uo += u; co += c
        m = struct.unpack('>i', info[q:q + 4])[0]; q += 4
        self.nodes = []
        for _ in range(m):
            off, sz, fl = struct.unpack('>qqI', info[q:q + 20]); q += 20
            e = info.index(b'\0', q); name = info[q:e].decode('utf-8', 'replace'); q = e + 1
            self.nodes.append((off, sz, fl, name))

    def read(self, off, size):
        out = bytearray()
        for uo, u, co, c, comp in self.blocks:
            if uo + u <= off or uo >= off + size:
                continue
            self.f.seek(self.data_start + co)
            raw = self.f.read(c)
            if comp == 0:
                d = raw
            elif comp in (2, 3):
                d = lz4_block(raw, u)
            else:
                raise ValueError('block compression %d' % comp)
            a = max(off, uo) - uo
            b = min(off + size, uo + u) - uo
            out += d[a:b]
        return bytes(out)


def clips(s):
    """(type hash of class 83 or None, [clip dict]) for one serialized file."""
    meta_size, file_size, version, data_off = struct.unpack('>IIII', s[:16])
    if version != 17:
        return ('version %d' % version, [])
    e = '>' if s[16] else '<'
    q = 20
    k = s.index(b'\0', q); q = k + 1
    q += 4
    tt = s[q]; q += 1
    ntypes = struct.unpack(e + 'i', s[q:q + 4])[0]; q += 4
    types = []
    for _ in range(ntypes):
        cid = struct.unpack(e + 'i', s[q:q + 4])[0]; q += 4
        q += 1
        q += 2
        if cid == 114:
            q += 16
        h = s[q:q + 16]; q += 16
        if tt:
            nn, sb = struct.unpack(e + 'ii', s[q:q + 8]); q += 8
            q += nn * 24 + sb
        types.append((cid, h))
    nobj = struct.unpack(e + 'i', s[q:q + 4])[0]; q += 4
    out = []
    th = None
    for _ in range(nobj):
        q = (q + 3) & ~3
        pid, start, sz, ti = struct.unpack(e + 'qIIi', s[q:q + 20]); q += 20
        cid, h = types[ti]
        if cid != 83:
            continue
        th = h.hex()
        b = s[data_off + start:data_off + start + sz]
        r = 0
        n = struct.unpack(e + 'i', b[r:r + 4])[0]; r += 4
        name = b[r:r + n].decode('utf-8', 'replace'); r += n; r = (r + 3) & ~3
        lt, ch, fr, bits = struct.unpack(e + 'iiii', b[r:r + 16]); r += 16
        length = struct.unpack(e + 'f', b[r:r + 4])[0]; r += 4
        r += 2; r = (r + 3) & ~3
        r += 4
        pre, bg = b[r], b[r + 1]
        bg_at = data_off + start + r + 1
        r += 3; r = (r + 3) & ~3
        n = struct.unpack(e + 'i', b[r:r + 4])[0]; r += 4
        src = b[r:r + n].decode('utf-8', 'replace'); r += n; r = (r + 3) & ~3
        res_at = data_off + start + r
        roff, rsz = struct.unpack(e + 'QQ', b[r:r + 16]); r += 16
        fmt = struct.unpack(e + 'i', b[r:r + 4])[0]; r += 4
        out.append({'name': name, 'load': LOAD.get(lt, lt), 'format': FMT.get(fmt, fmt), 'preload': pre, 'background': bg,
                    'length': length, 'bytes': rsz, 'channels': ch, 'hz': fr, 'exact': r == sz, 'bg_at': bg_at,
                    'source': src, 'offset': roff, 'res_at': res_at, 'path_id': pid, 'bits': bits,
                    'change': lt == 0 and pre == 1 and bg == 0})
    return (th, out)


def scan(path):
    a = Archive(path)
    res = {'file': os.path.basename(path), 'hash': a.hash.hex(), 'blocks': collections.Counter(b[4] for b in a.blocks), 'serialized': []}
    for off, sz, fl, name in a.nodes:
        if not fl & 4:
            continue
        s = a.read(off, sz)
        th, cl = clips(s)
        res['serialized'].append({'node': name, 'type_hash': th, 'clips': cl})
    return res


def main():
    target = sys.argv[1]
    files = [os.path.join(target, f) for f in sorted(os.listdir(target)) if f.endswith('.bundle')] if os.path.isdir(target) else [target]
    tot = collections.Counter(); hashes = collections.Counter(); inexact = 0; out = []
    secs = 0.0
    for f in files:
        try:
            r = scan(f)
        except Exception as ex:
            tot['unreadable'] += 1
            print('unreadable', os.path.basename(f), ex)
            continue
        out.append(r)
        n = sum(len(s['clips']) for s in r['serialized'])
        if not n:
            continue
        tot['bundles with clips'] += 1
        for s in r['serialized']:
            hashes[s['type_hash']] += len(s['clips'])
            for c in s['clips']:
                tot['clips'] += 1
                inexact += not c['exact']
                tot[(c['load'], c['format'], 'preload' if c['preload'] else 'no preload', 'bg' if c['background'] else 'main')] += 1
                if c['change']:
                    tot['to change'] += 1; secs += c['length']
        if '--list' in sys.argv:
            ch = [c for s in r['serialized'] for c in s['clips'] if c['change']]
            print('%s  clips %d  change %d (%.0f s audio)  blocks %s  hash %s' % (r['file'], n, len(ch), sum(c['length'] for c in ch), dict(r['blocks']), r['hash']))
    print('files', len(files), dict(tot))
    print('clips read not ending at the object size:', inexact)
    print('class 83 type hashes:', dict(hashes))
    print('audio to change: %.0f s' % secs)
    if '--json' in sys.argv:
        open(sys.argv[sys.argv.index('--json') + 1], 'w', encoding='utf-8').write(json.dumps(out, default=lambda o: dict(o) if isinstance(o, collections.Counter) else str(o), indent=1))


if __name__ == '__main__':
    main()
