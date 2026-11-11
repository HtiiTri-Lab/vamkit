"""Static survey of a scene's CustomUnityAsset bundles (no VaM): for every CUA atom its on state, bundle and prefab; for
every distinct bundle the .var it resolves to, the zip entry (stored / deflated, sizes) and the UnityFS header + block
table (block compression: 0 none, 1 LZMA, 2 LZ4, 3 LZ4HC; total uncompressed size). Prints the queue VaM would build
(scene order) and totals.
    VAMKIT_VAM=<VaM folder> python -I scene_bundles.py <scene .var or folder> <scene json inside> [--json out.json]"""
import collections, io, json, lzma, os, re, struct, sys, zipfile
from pathlib import Path

VAM = Path(os.environ['VAMKIT_VAM']) if os.environ.get('VAMKIT_VAM') else Path.cwd()   # the VaM folder (set VAMKIT_VAM)


def lz4_block(src, size):
    out = bytearray(); i = 0
    while i < len(src):
        tok = src[i]; i += 1
        n = tok >> 4
        if n == 15:
            while True:
                b = src[i]; i += 1; n += b
                if b != 255: break
        out += src[i:i + n]; i += n
        if i >= len(src): break
        off = src[i] | (src[i + 1] << 8); i += 2
        m = tok & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255: break
        m += 4
        start = len(out) - off
        for k in range(m): out.append(out[start + k])
    return bytes(out[:size])


def unity_lzma(src, size):
    props = src[:5]
    lc = props[0] % 9; rest = props[0] // 9; lp = rest % 5; pb = rest // 5
    dict_size = struct.unpack('<I', props[1:5])[0]
    f = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'lc': lc, 'lp': lp, 'pb': pb, 'dict_size': dict_size}])
    return f.decompress(src[5:], max_length=size)


def cstr(b, i):
    j = b.index(b'\0', i); return b[i:j].decode('latin-1'), j + 1


def unityfs(head, read_tail):
    """head: first bytes of the bundle; read_tail(n): last n bytes (blocks info at the end when flag 0x80)."""
    if not head.startswith(b'UnityFS\0'):
        return {'format': head[:8].split(b'\0')[0].decode('latin-1', 'replace')}
    i = 8
    ver = struct.unpack('>I', head[i:i + 4])[0]; i += 4
    player, i = cstr(head, i); engine, i = cstr(head, i)
    size, csz, usz, flags = struct.unpack('>QIII', head[i:i + 20]); i += 20
    if ver >= 7: i = (i + 15) // 16 * 16
    info = read_tail(csz) if flags & 0x80 else head[i:i + csz]
    ct = flags & 0x3F
    info = info if ct == 0 else unity_lzma(info, usz) if ct == 1 else lz4_block(info, usz)
    n = struct.unpack('>I', info[16:20])[0]; j = 20; blocks = collections.Counter(); ub = cb = 0
    for _ in range(n):
        u, c, fl = struct.unpack('>IIH', info[j:j + 10]); j += 10
        blocks[fl & 0x3F] += 1; ub += u; cb += c
    return {'format': 'UnityFS', 'engine': engine, 'info_comp': ct, 'blocks': dict(blocks), 'nblocks': n, 'uncompressed': ub, 'compressed': cb}


def var_index():
    idx = collections.defaultdict(dict)
    for root, _, files in os.walk(VAM / 'AddonPackages'):
        for f in files:
            m = re.match(r'^(.+)\.(\d+)\.var$', f)
            if m: idx[m.group(1)][int(m.group(2))] = os.path.join(root, f)
    return idx


def resolve(idx, pkg):
    m = re.match(r'^(.+)\.(latest|\d+)$', pkg)
    name, v = m.group(1), m.group(2)
    vs = idx.get(name, {})
    if not vs: return None, None
    if v == 'latest': return vs[max(vs)], max(vs)
    v = int(v)
    if v in vs: return vs[v], v
    return vs[max(vs)], max(vs)          # VaM falls back to the newest one it has


def main():
    src, inner = sys.argv[1], sys.argv[2]
    z = zipfile.ZipFile(src)
    j = json.loads(z.read(inner).decode('utf-8-sig'))
    idx = var_index()
    cuas = []
    for a in j['atoms']:
        if a['type'] != 'CustomUnityAsset': continue
        st = {s['id']: s for s in a['storables']}
        asset = st.get('asset', {})
        cuas.append({'id': a['id'], 'on': a.get('on'), 'url': asset.get('assetUrl'), 'name': asset.get('assetName'), 'dll': asset.get('loadDll')})
    bundles = {}
    for c in cuas:
        u = c['url']
        if not u or u in bundles: continue
        rec = {'url': u}
        if ':/' in u:
            pkg, path = u.split(':/', 1)
            if pkg == 'SELF': var = src; ver = None
            else: var, ver = resolve(idx, pkg)
            rec['var'] = var and os.path.relpath(var, VAM); rec['ver'] = ver
            if var:
                zz = zipfile.ZipFile(var)
                try:
                    zi = zz.getinfo(path)
                except KeyError:
                    names = {n.lower(): n for n in zz.namelist()}
                    zi = zz.getinfo(names[path.lower()]) if path.lower() in names else None
                if zi:
                    rec.update(zip_method=zi.compress_type, zip_size=zi.compress_size, size=zi.file_size)
                    data = zz.read(zi)
                    rec.update(unityfs(data, lambda n: data[-n:]))
                else:
                    rec['missing'] = True
        bundles[u] = rec
    order = []; seen = set()
    for c in cuas:
        if c['url']: order.append((c['url'] in seen, c)); seen.add(c['url'])
    tot = collections.Counter(); comp = collections.Counter(); compsz = collections.Counter()
    for b in bundles.values():
        tot['bundles'] += 1; tot['size'] += b.get('size', 0); tot['unc'] += b.get('uncompressed', 0)
        kind = 'missing' if b.get('missing') or not b.get('var') else ('LZMA' if b.get('blocks', {}).get(1) else 'LZ4' if (b.get('blocks', {}).get(2) or b.get('blocks', {}).get(3)) else 'none' if b.get('format') == 'UnityFS' else b.get('format', '?'))
        b['kind'] = kind; comp[kind] += 1; compsz[kind] += b.get('size', 0)
    on = collections.Counter(c['on'] for c in cuas)
    print('CUAs', len(cuas), 'on', dict(on), 'with url', sum(1 for c in cuas if c['url']), 'loadDll', sum(1 for c in cuas if c['dll'] == 'true'))
    print('distinct bundles', tot['bundles'], 'file MB %.1f' % (tot['size'] / 1e6), 'uncompressed MB %.1f' % (tot['unc'] / 1e6))
    for k in comp: print('  %-8s %3d bundles  %.1f MB' % (k, comp[k], compsz[k] / 1e6))
    big = sorted(bundles.values(), key=lambda b: -b.get('size', 0))[:15]
    for b in big: print('  %7.1f MB %-6s %s' % (b.get('size', 0) / 1e6, b['kind'], b['url'][:110]))
    print('queue: %d requests, %d first-time, %d repeats' % (len(order), sum(1 for r, _ in order if not r), sum(1 for r, _ in order if r)))
    if '--json' in sys.argv:
        Path(sys.argv[sys.argv.index('--json') + 1]).write_text(json.dumps({'cuas': cuas, 'bundles': bundles}, ensure_ascii=False, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
