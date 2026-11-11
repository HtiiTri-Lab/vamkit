"""Offline check of VaM Scene Load 1.5.0's in-memory asset bundles (no VaM): every bundle a scene loads goes through the
native library as in the game (SL_BundleSubmit2 reads the .var entry, the background job builds the bundle) and is read
back through SL_BundleRead the way Unity reads its Stream, then compared with the original entry decoded here by Python:
  - as it is (2): the bytes = the entry's bytes;
  - rebuilt (1): header strings, size field = length, flags 0x43, LZ4 blocks info smaller than the info, the original
    hash, stored blocks covering exactly the data; the directory has the same nodes (names, flags) in the same order;
    every node's bytes equal the original node's, except in a serialized file the 20 bytes of each PCM16 clip's
    m_Resource offset / size / m_CompressionFormat (and the m_Source characters of a clip in a further node), and a
    .resource node that continues past its original bytes with the PCM16 FSB5s; further resource nodes after the
    original ones (same flags, a name of the same length as a .resource node), no node with PCM16 clips over 1 GB;
  - each PCM16 clip (audio_scan reads the rebuilt serialized file): offset / size inside the node, FSB5 PCM16 with the
    original frames / channels / Hz, its samples = SL_DecodeFsb of the original Vorbis FSB5 (hash), the padding zeros;
    against the game's own decode (AudioTruthProbe lines, --truth) the first / last 8 values and min / max within 1;
  - a clip read twice (decoded again after its buffer went) gives the same bytes.
    python -I mem_bundle_check.py <dll> <bundles.json | urls.txt> [...] [--truth truth.tsv ...] [--only substring]"""
import array, collections, ctypes, hashlib, json, os, re, struct, sys, time, zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scene_bundles as sb      # noqa: E402
import audio_scan as A          # noqa: E402
import fsb5                     # noqa: E402

args = sys.argv[1:]
dll = ctypes.CDLL(args[0])
truth_files = [args[i + 1] for i, a in enumerate(args) if a == '--truth']
only = args[args.index('--only') + 1] if '--only' in args else None
srcs = [a for i, a in enumerate(args[1:], 1) if not a.startswith('--') and args[i - 1] not in ('--truth', '--only')]
dll.SL_BundleSubmit2.restype = ctypes.c_int
dll.SL_BundleSubmit2.argtypes = [ctypes.c_wchar_p, ctypes.c_int64, ctypes.c_int64, ctypes.c_int, ctypes.c_int64, ctypes.c_int]
dll.SL_BundleResult2.restype = ctypes.c_int
dll.SL_BundleResult2.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_int64), ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_int)]
dll.SL_BundleRead.restype = ctypes.c_int
dll.SL_BundleRead.argtypes = [ctypes.c_int, ctypes.c_int64, ctypes.c_void_p, ctypes.c_int]
dll.SL_BundleFree.argtypes = [ctypes.c_int]
dll.SL_AudioStats.argtypes = [ctypes.POINTER(ctypes.c_double), ctypes.c_int]
dll.SL_DecodeFsb.restype = ctypes.c_int
dll.SL_DecodeFsb.argtypes = [ctypes.c_char_p, ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64]

urls = []
var_of = {}
for src in srcs:
    if src.endswith('.json'):
        bj = json.load(open(src, encoding='utf-8'))['bundles']
        urls += list(bj)
        if isinstance(bj, dict):
            for k, v in bj.items():
                if isinstance(v, dict) and v.get('var'): var_of[k] = str(sb.VAM / v['var'])
    else:
        for line in open(src, encoding='utf-8'):
            m = re.search(r'([^\t]+:/[^\t]+\.(?:assetbundle|uibundle|cambundle|fontbundle|audiobundle|unity3d|scene))\s*$', line.strip(), re.I)
            if m: urls.append(m.group(1))
seen = set(); urls = [u for u in urls if not (u in seen or seen.add(u))]
if only: urls = [u for u in urls if only.lower() in u.lower()]
truth = collections.defaultdict(list)
for t in truth_files:
    for line in open(t, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) > 16 and f[1] == 'DecompressOnLoad' and f[2] == 'Loaded':
            truth[(f[0], int(f[3]), int(f[4]), int(f[5]))].append(f)


class Reader:
    """The presented bundle as a file object (audio_scan.Archive reads through seek / read)."""
    def __init__(self, bid, length):
        self.id, self.length, self.pos = bid, length, 0
        self.buf = ctypes.create_string_buffer(1 << 20)
    def seek(self, p, whence=0):
        self.pos = p if whence == 0 else self.length + p if whence == 2 else self.pos + p
    def tell(self):
        return self.pos
    def read(self, n=-1):
        if n < 0: n = self.length - self.pos
        out = bytearray()
        while n > 0:
            k = min(n, 1 << 20)
            got = dll.SL_BundleRead(self.id, self.pos, self.buf, k)
            if got < 0: raise IOError('SL_BundleRead failed at %d' % self.pos)
            if got == 0: break
            out += self.buf.raw[:got]; self.pos += got; n -= got
        return bytes(out)


def original_archive(data):
    """entry bytes -> (header fields, info, nodes, data bytes decoded)."""
    i = 12
    p, i = sb.cstr(data, i); e, i = sb.cstr(data, i)
    size, csz, usz, flags = struct.unpack('>QIII', data[i:i + 20]); end = i + 20
    raw = data[-csz:] if flags & 0x80 else data[end:end + csz]
    ct = flags & 0x3F
    info = raw if ct == 0 else sb.unity_lzma(raw, usz) if ct == 1 else sb.lz4_block(raw, usz)
    n = struct.unpack('>I', info[16:20])[0]; j = 20; blocks = []
    for _ in range(n):
        blocks.append(struct.unpack('>IIH', info[j:j + 10])); j += 10
    m = struct.unpack('>I', info[j:j + 4])[0]; j += 4; nodes = []
    for _ in range(m):
        off, sz, fl = struct.unpack('>qqI', info[j:j + 20]); j += 20
        k = info.index(b'\0', j); nodes.append((off, sz, fl, info[j:k].decode('utf-8', 'replace'))); j = k + 1
    pos = end if flags & 0x80 else end + csz
    out = bytearray()
    for (u, c, fl) in blocks:
        blk = data[pos:pos + c]; pos += c
        t = fl & 0x3F
        out += blk if t == 0 else sb.unity_lzma(blk, u) if t == 1 else sb.lz4_block(blk, u)
    return dict(header=data[:end - 20], flags=flags, hash=info[:16], blocks=blocks, nodes=nodes, data=bytes(out))


def stats():
    v = (ctypes.c_double * 18)()
    dll.SL_AudioStats(v, 18)
    names = ['pcm16 clips', 'kept vorbis', 'bg decodes', 'reader decodes', 'redecodes', 'cap skips', 'freed', 'decode errors',
             'decode ms', 'wait ms', 'frames', 'audio bundles', 'pending bytes', 'peak pending', 'further nodes', 'dropped unread', 'held', 'held peak']
    return dict(zip(names, [round(x, 1) for x in v]))


idx = sb.var_index()
tot = collections.Counter(); bad = []
t_all = time.perf_counter()
jobs = []
for u in urls:
    pkg, path = u.split(':/', 1)
    var = var_of.get(u)
    if not var:
        try: var, _ = sb.resolve(idx, pkg)
        except AttributeError: var = None
    if not var:
        tot['missing'] += 1; continue
    zz = zipfile.ZipFile(var)
    names = {n.lower(): n for n in zz.namelist()}
    if path.lower() not in names:
        tot['no entry'] += 1; continue
    zi = zz.getinfo(names[path.lower()])
    bid = dll.SL_BundleSubmit2(os.path.abspath(var), zi.header_offset, zi.compress_size, zi.compress_type, zi.file_size, 1)
    jobs.append((u, zz, zi, bid))
print('submitted', len(jobs), flush=True)
for u, zz, zi, bid in jobs:
    length, ms, clips = ctypes.c_int64(), ctypes.c_double(), ctypes.c_int()
    while True:
        r = dll.SL_BundleResult2(bid, ctypes.byref(length), ctypes.byref(ms), ctypes.byref(clips))
        if r != 0: break
        time.sleep(0.02)
    tot['job ms'] += ms.value
    if r < 0:
        tot['failed %d' % r] += 1; print('FAIL', r, u, flush=True); continue
    orig = zz.read(zi)
    rd = Reader(bid, length.value)
    ok = True
    if r == 2:
        tot['as is'] += 1
        ok = hashlib.sha1(rd.read()).digest() == hashlib.sha1(orig).digest()
    else:
        tot['rebuilt'] += 1
        tot['pcm16 clips (jobs)'] += clips.value
        a = original_archive(orig)
        head = rd.read(1 << 16)
        i = 12
        p_, i = sb.cstr(head, i); e_, i = sb.cstr(head, i)
        size, csz, usz, flags = struct.unpack('>QIII', head[i:i + 20])
        ok &= head[:i] == a['header'] and size == length.value and flags == 0x43 and csz < usz
        b = A.Archive.__new__(A.Archive)   # audio_scan.Archive on our reader instead of a file
        b.f = rd; rd.seek(0); hb = rd.read(4096)
        pp = 12
        for _ in range(2): pp = hb.index(b'\0', pp) + 1
        b.size, c2, u2, b.flags = struct.unpack('>qIII', hb[pp:pp + 20]); pp += 20
        rd.seek(pp); raw = rd.read(c2); b.data_start = pp + c2
        inf = A.lz4_block(raw, u2)
        b.hash = inf[:16]
        q = 16; n = struct.unpack('>i', inf[q:q + 4])[0]; q += 4; b.blocks = []; uo = co = 0
        for _ in range(n):
            uu, cc, fl = struct.unpack('>IIH', inf[q:q + 10]); q += 10
            ok &= uu == cc and fl & 0x3F == 0
            b.blocks.append((uo, uu, co, cc, 0)); uo += uu; co += cc
        m = struct.unpack('>i', inf[q:q + 4])[0]; q += 4; b.nodes = []
        for _ in range(m):
            off, sz, fl = struct.unpack('>qqI', inf[q:q + 20]); q += 20
            k = inf.index(b'\0', q); b.nodes.append((off, sz, fl, inf[q:k].decode('utf-8', 'replace'))); q = k + 1
        ok &= b.hash == a['hash'] and b.data_start + uo == length.value
        no_ = len(a['nodes'])
        ok &= [(x[2], x[3]) for x in b.nodes[:no_]] == [(x[2], x[3]) for x in a['nodes']]
        res_names = [x[3] for x in a['nodes'] if x[3].endswith('.resource')]
        for x in b.nodes[no_:]:
            tot['further nodes'] += 1
            ok &= x[3] not in [y[3] for y in a['nodes']] and any(len(x[3]) == len(r) and x[2] == f for r in res_names for f in [y[2] for y in a['nodes'] if y[3] == r])
        for x, y in zip(b.nodes, a['nodes'] + [None] * (len(b.nodes) - no_)):
            if (y is None or x[1] > y[1]) and x[1] > (1 << 30): ok = False; print('  node over 1 GB', x[3], x[1], flush=True)
        tot['largest node with PCM16 MB'] = max(tot['largest node with PCM16 MB'], max([x[1] >> 20 for x, y in zip(b.nodes, a['nodes'] + [None] * (len(b.nodes) - no_)) if y is None or x[1] > y[1]] or [0]))
        if not ok: print('  header / blocks / directory differ', u, flush=True)
        D = a['data']
        o_names = {x[3] for x in a['nodes']}
        for (no, ns, nf, nn), (oo, os_, of, on) in zip(b.nodes, a['nodes']):
            want = D[oo:oo + os_]
            if nn.endswith('.resource') and ns > os_:
                if b.read(no, os_) != want:
                    ok = False; print('  resource prefix differs', nn, flush=True)
                continue
            got = b.read(no, ns)
            if nf & 4 and got != want:
                diff = [k for k in range(len(want)) if got[k] != want[k]]
                th, cl = A.clips(got)
                allowed = {c['res_at'] + k for c in cl if c['format'] == 'PCM' for k in range(20)}   # m_Offset, m_Size, m_CompressionFormat
                for c in cl:   # m_Source of a clip in a further node
                    if c['format'] == 'PCM' and c['source'].split('/')[-1] not in o_names:
                        start = c['res_at'] - ((len(c['source'].encode('utf-8')) + 3) & ~3)
                        allowed |= set(range(start, start + len(c['source'].encode('utf-8'))))
                if any(k not in allowed for k in diff):
                    ok = False; print('  serialized file differs outside the patched fields', nn, len(diff), flush=True)
            elif got != want:
                ok = False; print('  node differs', nn, flush=True)
        # the clips
        by_name = {x[3]: x for x in b.nodes}
        o_by_name = {x[3]: x for x in a['nodes']}
        for no, ns, nf, nn in b.nodes:
            if not nf & 4: continue
            th, cl = A.clips(b.read(no, ns))
            _, ocl = A.clips(D[o_by_name[nn][0]:o_by_name[nn][0] + o_by_name[nn][1]])
            for c, oc in zip(cl, ocl):
                if c['format'] != 'PCM' or oc['format'] != 'Vorbis':
                    tot['clips kept'] += 1
                    ok &= c['offset'] == oc['offset'] and c['bytes'] == oc['bytes']
                    continue
                tot['pcm16 clips'] += 1
                leaf = c['source'].split('/')[-1]
                rn = by_name[leaf]; orn = o_by_name[oc['source'].split('/')[-1]]
                ok &= c['source'][:-len(leaf)] == oc['source'][:-len(leaf)] and len(leaf) == len(oc['source'].split('/')[-1])
                ok &= c['offset'] + c['bytes'] <= rn[1]
                if leaf not in o_names: tot['pcm16 clips in further nodes'] += 1
                vfsb = D[orn[0] + oc['offset']:orn[0] + oc['offset'] + oc['bytes']]
                pv = fsb5.parse(vfsb)
                pcm_fsb = b.read(rn[0] + c['offset'], c['bytes'])
                pp_ = fsb5.parse(pcm_fsb)
                nval = pv['frames'] * pv['channels']
                exp = (ctypes.c_int16 * nval)()
                rc = dll.SL_DecodeFsb(vfsb, len(vfsb), exp, nval)
                data = pcm_fsb[96:96 + 2 * nval]
                good = (pp_['mode'] == 'PCM16' and pp_['frames'] == pv['frames'] and pp_['channels'] == pv['channels'] and pp_['freq'] == pv['freq']
                        and pp_['data_start'] == 96 and rc == 0 and data == bytes(exp) and not any(pcm_fsb[96 + 2 * nval:])
                        and (rn[0] + c['offset']) % (128 * 1024) == 0)
                if not good:
                    ok = False; tot['pcm16 clip bad'] += 1
                    if tot['pcm16 clip bad'] < 5:
                        print('  clip bad', c['name'], pp_['mode'], pp_['frames'], pv['frames'], pp_['channels'], pv['channels'], pp_['freq'], pv['freq'], pp_['data_start'], rc,
                              'data same', data == bytes(exp), 'pad zero', not any(pcm_fsb[96 + 2 * nval:]), 'aligned', (rn[0] + c['offset']) % (128 * 1024), flush=True)
                tl = truth.get((c['name'], pv['frames'], pv['channels'], pv['freq']))
                if tl:
                    v = array.array('h', data)
                    head8 = list(v[:8]); tail8 = list(v[-8:])
                    close = False
                    for f in tl:
                        th8 = [int(x) for x in f[15].split(',') if x]; tt8 = [int(x) for x in f[16].split(',') if x]
                        mn, mx = round(float(f[13]) * 32768), round(float(f[14]) * 32768)
                        if (all(abs(x - y) <= 1 for x, y in zip(head8, th8)) and all(abs(x - y) <= 1 for x, y in zip(tail8, tt8))
                                and abs(min(v) - mn) <= 1 and abs(max(v) - mx) <= 1):
                            close = True
                    tot['vs game: within 1' if close else 'vs game: FAR'] += 1
                    if not close and tot['vs game: FAR'] < 5: print('  far from the game', c['name'], head8, tl[0][15], flush=True)
        if 'cv_' in u.lower() or clips.value > 300:
            # read one clip twice: the second read decodes it again
            st0 = stats()
            for no, ns, nf, nn in b.nodes:
                if not nf & 4: continue
                th, cl = A.clips(b.read(no, ns))
                pc = [c for c in cl if c['format'] == 'PCM']
                if pc:
                    rn = by_name[pc[0]['source'].split('/')[-1]]
                    x1 = b.read(rn[0] + pc[0]['offset'], pc[0]['bytes']); x2 = b.read(rn[0] + pc[0]['offset'], pc[0]['bytes'])
                    ok &= x1 == x2
                    print('  read twice: same', x1 == x2, 'redecodes', stats()['redecodes'] - st0['redecodes'], flush=True)
                    break
    if not ok:
        tot['BAD'] += 1; bad.append(u); print('BAD', r, u, flush=True)
    dll.SL_BundleFree(bid)
print('bundles', len(urls), dict(tot), 'in %.0f s' % (time.perf_counter() - t_all))
print('audio', stats())
for u in bad: print('bad', u)
