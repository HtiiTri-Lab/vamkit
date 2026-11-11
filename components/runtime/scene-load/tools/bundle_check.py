"""Offline check of VaM Scene Load 1.3.0's asset bundle files (no VaM): for every bundle a scene loads, the native
SL_BundleBuildNow writes the file the plugin would load instead of VaM's in-memory bundle, and this compares it with
the original entry decoded here by Python (liblzma for LZMA blocks):
  - LZMA bundles: header strings equal, size field = file size, flags 0x43, the LZ4 blocks info smaller than the
    info (1.3.1: Unity refused a literals-only info), blocks info = the original's hash, one
    stored block per original block (same sizes), the same directory bytes; the data = the original blocks decoded;
  - every other bundle: the file = the entry's bytes.
    python -I bundle_check.py <dll> <workdir> <urls.txt | bundles.json> [...]   (one 'Package.Name.Version:/path' per line)"""
import ctypes, hashlib, json, os, re, struct, sys, time, zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scene_bundles as sb

dll = ctypes.CDLL(sys.argv[1])
work = Path(sys.argv[2]); work.mkdir(parents=True, exist_ok=True)
build = dll.SL_BundleBuildNow
build.restype = ctypes.c_int
build.argtypes = [ctypes.c_wchar_p, ctypes.c_int64, ctypes.c_int64, ctypes.c_int, ctypes.c_int64, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int64)]

urls = []
for src in sys.argv[3:]:
    if src.endswith('.json'):
        urls += list(json.load(open(src, encoding='utf-8'))['bundles'])
    else:
        for line in open(src, encoding='utf-8'):
            m = re.search(r'([^\t]+:/[^\t]+\.(?:assetbundle|uibundle|cambundle|fontbundle|audiobundle|unity3d|scene))\s*$', line.strip(), re.I)
            if m: urls.append(m.group(1))
seen = set(); urls = [u for u in urls if not (u in seen or seen.add(u))]
idx = sb.var_index()


def parse(data):
    i = 8; ver = struct.unpack('>I', data[i:i + 4])[0]; i += 4
    p, i = sb.cstr(data, i); e, i = sb.cstr(data, i)
    size, csz, usz, flags = struct.unpack('>QIII', data[i:i + 20]); end = i + 20
    raw = data[-csz:] if flags & 0x80 else data[end:end + csz]
    ct = flags & 0x3F
    info = raw if ct == 0 else sb.unity_lzma(raw, usz) if ct == 1 else sb.lz4_block(raw, usz)
    n = struct.unpack('>I', info[16:20])[0]; j = 20; blocks = []
    for _ in range(n):
        blocks.append(struct.unpack('>IIH', info[j:j + 10])); j += 10
    start = end if flags & 0x80 else end + csz
    return dict(ver=ver, p=p, e=e, size=size, flags=flags, csz=csz, usz=usz, hash=info[:16], blocks=blocks, dir=info[j:], start=start, header=data[:end - 20])


tot = {'lzma': 0, 'asis': 0, 'fail': 0, 'bad': 0}; native_ms = 0.0; out_mb = 0.0; t_all = time.perf_counter()
for u in urls:
    pkg, path = u.split(':/', 1)
    var, _ = sb.resolve(idx, pkg)
    if not var:
        print('MISSING', u); continue
    zz = zipfile.ZipFile(var)
    names = {n.lower(): n for n in zz.namelist()}
    if path.lower() not in names:
        print('NO ENTRY', u); continue
    zi = zz.getinfo(names[path.lower()])
    orig = zz.read(zi)
    out = work / 'check.bundle'
    nb = ctypes.c_int64(0)
    t = time.perf_counter()
    r = build(os.path.abspath(var), zi.header_offset, zi.compress_size, zi.compress_type, zi.file_size, str(out), ctypes.byref(nb))
    native_ms += (time.perf_counter() - t) * 1000
    if r < 0:
        tot['fail'] += 1; print('FAIL', r, u); continue
    got = out.read_bytes(); out_mb += len(got) / 1e6
    ok = True
    if r == 2:
        tot['asis'] += 1
        ok = got == orig
    else:
        tot['lzma'] += 1
        a, b = parse(orig), parse(got)
        ok &= b['csz'] < b['usz'] and b['header'] == a['header'] and b['size'] == len(got) and b['flags'] == 0x43 and b['hash'] == a['hash'] and b['dir'] == a['dir']
        ok &= len(b['blocks']) == len(a['blocks']) and all(bb == (ab[0], ab[0], ab[2] & ~0x3F) for ab, bb in zip(a['blocks'], b['blocks']))
        pos = a['start']; h = hashlib.sha1()
        for (uu, cc, fl) in a['blocks']:
            blk = orig[pos:pos + cc]; pos += cc
            h.update(sb.unity_lzma(blk, uu) if fl & 0x3F == 1 else blk)
        ok &= hashlib.sha1(got[b['start']:]).digest() == h.digest() and b['start'] + sum(x[0] for x in a['blocks']) == len(got)
    if not ok:
        tot['bad'] += 1; print('MISMATCH', r, u)
    out.unlink()
print('bundles', len(urls), tot, 'native single-thread %.1f s for %.0f MB out' % (native_ms / 1000, out_mb), 'total %.0f s' % (time.perf_counter() - t_all))
