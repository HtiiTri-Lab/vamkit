"""Extract compiled D3D11 programs (DXBC) of named shaders from a Unity 2018.1
serialized file (resources.assets etc.), without type trees.

usage: python -I shader_extract.py <assets file> <shader-name regex> <out dir>
Writes <out>/<shader>/<index>_<stage>.dxbc plus index.txt (stage, size, the
keyword strings found just before each program in Unity's sub-program blob).
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundle_audit as B  # noqa: E402

STAGES = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs"}


def find_blob(obj):
    """Locate platforms/offsets/compressedLengths/decompressedLengths/compressedBlob."""
    n = len(obj)
    for p in range(0, n - 40, 4):
        k = struct.unpack_from("<i", obj, p)[0]
        if not 1 <= k <= 8:
            continue
        q = p + 4
        try:
            plats = struct.unpack_from("<%di" % k, obj, q); q += 4 * k
            if any(not 0 <= x <= 30 for x in plats):
                continue
            arrays = []
            for _ in range(3):
                if struct.unpack_from("<i", obj, q)[0] != k:
                    raise ValueError
                q += 4
                arrays.append(struct.unpack_from("<%dI" % k, obj, q)); q += 4 * k
            offs, clen, dlen = arrays
            L = struct.unpack_from("<i", obj, q)[0]; q += 4
            if L <= 0 or q + L > n or sum(clen) != L:
                continue
            if any(o + c > L for o, c in zip(offs, clen)):
                continue
            return plats, offs, clen, dlen, obj[q:q + L]
        except (struct.error, ValueError):
            continue
    return None


def dxbc_containers(data):
    i = 0
    while True:
        i = data.find(b"DXBC", i)
        if i < 0:
            return
        if i + 32 <= len(data):
            size = struct.unpack_from("<I", data, i + 24)[0]
            if 32 < size <= len(data) - i:
                yield i, data[i:i + size]
                i += size
                continue
        i += 4


def stage_of(dx):
    count = struct.unpack_from("<I", dx, 28)[0]
    for c in range(count):
        off = struct.unpack_from("<I", dx, 32 + 4 * c)[0]
        tag = dx[off:off + 4]
        if tag in (b"SHEX", b"SHDR"):
            ver = struct.unpack_from("<I", dx, off + 8)[0]
            return STAGES.get(ver >> 16, "?%d" % (ver >> 16))
    return "unknown"


def keywords_before(data, pos):
    window = data[max(0, pos - 600):pos]
    return sorted(set(w.decode() for w in re.findall(rb"[A-Z][A-Z0-9_]{3,40}", window)
                      if not w.startswith(b"DXBC")))


def main():
    path, want, out = sys.argv[1], re.compile(sys.argv[2]), sys.argv[3]
    data = open(path, "rb").read()
    E, types, objs = B.read_serialized(data)
    for pid, start, size, tidx in objs:
        cid, _ = types[tidx]
        if cid != 48:
            continue
        obj = data[start:start + size]
        # The shader name sits in the parsed form, after properties and passes:
        # take the length-prefixed strings with a '/' and keep the matching one.
        name = None
        for m in re.finditer(rb"[A-Za-z][A-Za-z0-9 _()\-]*/[A-Za-z0-9 _/()\-]+", obj):
            s = m.start()
            if s >= 4 and struct.unpack_from("<I", obj, s - 4)[0] == len(m.group(0)):
                cand = m.group(0).decode("latin1")
                if want.search(cand):
                    name = cand
                    break
        if name is None:
            continue
        found = find_blob(obj)
        if not found:
            print("no blob in", name)
            continue
        plats, offs, clen, dlen, blob = found
        sdir = os.path.join(out, re.sub(r"[^A-Za-z0-9_.-]", "_", name))
        os.makedirs(sdir, exist_ok=True)
        lines = []
        for plat, o, c, d in zip(plats, offs, clen, dlen):
            raw = B.decompress(blob[o:o + c], 2, d)
            for n, (pos, dx) in enumerate(dxbc_containers(raw)):
                st = stage_of(dx)
                fn = "%03d_p%d_%s.dxbc" % (n, plat, st)
                with open(os.path.join(sdir, fn), "wb") as f:
                    f.write(dx)
                lines.append("%s\t%d bytes\t%s" % (fn, len(dx), " ".join(keywords_before(raw, pos))))
        with open(os.path.join(sdir, "index.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print("%s: platforms %s, %d programs -> %s" % (name, list(plats), len(lines), sdir))


if __name__ == "__main__":
    main()
