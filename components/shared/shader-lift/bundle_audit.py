"""Minimal UnityFS (Unity 2018.1) asset-bundle reader: lists render-relevant
components (Light, ReflectionProbe, Camera, renderers ...) with key fields.

usage: python -I bundle_audit.py <file.var|file.assetbundle> [inner-path-substring]
Reads only; no Unity install needed. Type trees must be present (normal for bundles).
"""
import collections
import io
import lzma
import struct
import sys
import zipfile

CLASS = {20: "Camera", 108: "Light", 215: "ReflectionProbe", 23: "MeshRenderer",
         137: "SkinnedMeshRenderer", 198: "ParticleSystem", 199: "ParticleSystemRenderer",
         114: "MonoBehaviour", 33: "MeshFilter", 1: "GameObject", 4: "Transform",
         104: "RenderSettings", 157: "LightmapSettings", 196: "NavMeshSettings",
         258: "LightProbes", 21: "Material", 28: "Texture2D", 89: "Cubemap", 43: "Mesh",
         48: "Shader", 84: "RenderTexture", 205: "LODGroup", 212: "SpriteRenderer",
         96: "TrailRenderer", 120: "LineRenderer", 1001: "Prefab", 115: "MonoScript"}


def lz4_block(src, usize):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += 4
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:
            for k in range(ml):
                dst.append(dst[start + k])
    return bytes(dst)


def decompress(data, kind, usize):
    if kind == 0:
        return data
    if kind == 1:
        props = data[:5]
        lc = props[0] % 9; r = props[0] // 9; lp = r % 5; pb = r // 5
        dict_size = struct.unpack("<I", props[1:5])[0]
        filt = [{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}]
        return lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt).decompress(data[5:], usize)
    if kind in (2, 3):
        return lz4_block(data, usize)
    raise ValueError("compression %d" % kind)


def cstr(f):
    b = bytearray()
    while True:
        c = f.read(1)
        if not c or c == b"\0":
            return b.decode("utf-8", "replace")
        b += c


def read_unityfs(buf):
    f = io.BytesIO(buf)
    sig = cstr(f)
    assert sig == "UnityFS", sig
    ver = struct.unpack(">I", f.read(4))[0]
    cstr(f); cstr(f)
    size, csz, usz, flags = struct.unpack(">qIII", f.read(20))
    if ver >= 7:
        f.seek((f.tell() + 15) // 16 * 16)
    if flags & 0x80:
        info = buf[size - csz:size]
        data_start = f.tell()
    else:
        info = f.read(csz)
        data_start = f.tell()
    info = decompress(info, flags & 0x3F, usz)
    g = io.BytesIO(info)
    g.read(16)
    nb = struct.unpack(">i", g.read(4))[0]
    blocks = [struct.unpack(">IIH", g.read(10)) for _ in range(nb)]
    nn = struct.unpack(">i", g.read(4))[0]
    nodes = []
    for _ in range(nn):
        off, sz, fl = struct.unpack(">qqI", g.read(20))
        nodes.append((off, sz, fl, cstr(g)))
    out = bytearray()
    pos = data_start
    for u, c, fl in blocks:
        out += decompress(buf[pos:pos + c], fl & 0x3F, u)
        pos += c
    return {name: bytes(out[off:off + sz]) for off, sz, fl, name in nodes}


class TT:
    def __init__(self, nodes):
        self.nodes = nodes  # list of (level, type, name, size, meta)


def read_serialized(data):
    msz, fsz, ver, doff = struct.unpack(">IIII", data[:16])
    endian = data[16]
    E = ">" if endian else "<"
    f = io.BytesIO(data)
    f.seek(20)
    cstr(f)
    struct.unpack(E + "i", f.read(4))
    tree = f.read(1)[0]
    ntypes = struct.unpack(E + "i", f.read(4))[0]
    types = []
    for _ in range(ntypes):
        cid = struct.unpack(E + "i", f.read(4))[0]
        f.read(1)
        sidx = struct.unpack(E + "h", f.read(2))[0]
        if ver >= 16 and (cid == 114 or sidx >= 0) or cid < 0:
            f.read(16)
        f.read(16)
        tt = None
        if tree:
            nc, sb = struct.unpack(E + "ii", f.read(8))
            raw = [struct.unpack(E + "HBBIIiiI" if False else E + "HBBIIiii", f.read(24)) for _ in range(nc)]
            strs = f.read(sb)

            def s(o):
                if o & 0x80000000:
                    o &= 0x7FFFFFFF
                    return COMMON.get(o, "?%d" % o)
                e = strs.index(b"\0", o)
                return strs[o:e].decode()
            tt = TT([(r[1], s(r[3]), s(r[4]), r[5], r[7]) for r in raw])
        types.append((cid, tt))
    nobj = struct.unpack(E + "i", f.read(4))[0]
    objs = []
    for _ in range(nobj):
        f.seek((f.tell() + 3) // 4 * 4)
        pid, start, size, tidx = struct.unpack(E + "qIIi", f.read(20))
        objs.append((pid, doff + start, size, tidx))
    return E, types, objs


COMMON_STR = ("AABB Array Base BitField bool char ColorRGBA Component data deque double dynamic_array FastPropertyName "
              "first float Font GameObject Generic Mono GradientNEW GUID GUIStyle int list long long map Matrix4x4f "
              "MdFour MonoBehaviour MonoScript m_ByteSize m_Curve m_EditorClassIdentifier m_EditorHideFlags m_Enabled "
              "m_ExtensionPtr m_GameObject m_Index m_IsArray m_IsStatic m_MetaFlag m_Name m_ObjectHideFlags "
              "m_PrefabInternal m_PrefabParentObject m_Script m_StaticEditorFlags m_Type m_Version Object pair "
              "PPtr<Component> PPtr<GameObject> PPtr<Material> PPtr<MonoBehaviour> PPtr<MonoScript> PPtr<Object> "
              "PPtr<Prefab> PPtr<Sprite> PPtr<TextAsset> PPtr<Texture> PPtr<Texture2D> PPtr<Transform> Prefab Quaternionf "
              "Rectf RectInt RectOffset second set short size SInt16 SInt32 SInt64 SInt8 staticvector string TextAsset "
              "TextMesh Texture Texture2D Transform TypelessData UInt16 UInt32 UInt64 UInt8 unsigned int unsigned long long "
              "unsigned short vector Vector2f Vector3f Vector4f m_ScriptingClassIdentifier Gradient Type*")
# offsets of Unity's common string table (2018.x): computed by cumulative lengths
COMMON = {}
_o = 0
for _w in ["AABB", "AnimationClip", "AnimationCurve", "AnimationState", "Array", "Base", "BitField", "bitset", "bool",
           "char", "ColorRGBA", "Component", "data", "deque", "double", "dynamic_array", "FastPropertyName", "first",
           "float", "Font", "GameObject", "Generic Mono", "GradientNEW", "GUID", "GUIStyle", "int", "list", "long long",
           "map", "Matrix4x4f", "MdFour", "MonoBehaviour", "MonoScript", "m_ByteSize", "m_Curve",
           "m_EditorClassIdentifier", "m_EditorHideFlags", "m_Enabled", "m_ExtensionPtr", "m_GameObject", "m_Index",
           "m_IsArray", "m_IsStatic", "m_MetaFlag", "m_Name", "m_ObjectHideFlags", "m_PrefabInternal",
           "m_PrefabParentObject", "m_Script", "m_StaticEditorFlags", "m_Type", "m_Version", "Object", "pair",
           "PPtr<Component>", "PPtr<GameObject>", "PPtr<Material>", "PPtr<MonoBehaviour>", "PPtr<MonoScript>",
           "PPtr<Object>", "PPtr<Prefab>", "PPtr<Sprite>", "PPtr<TextAsset>", "PPtr<Texture>", "PPtr<Texture2D>",
           "PPtr<Transform>", "Prefab", "Quaternionf", "Rectf", "RectInt", "RectOffset", "second", "set", "short",
           "size", "SInt16", "SInt32", "SInt64", "SInt8", "staticvector", "string", "TextAsset", "TextMesh", "Texture",
           "Texture2D", "Transform", "TypelessData", "UInt16", "UInt32", "UInt64", "UInt8", "unsigned int",
           "unsigned long long", "unsigned short", "vector", "Vector2f", "Vector3f", "Vector4f",
           "m_ScriptingClassIdentifier", "Gradient", "Type*", "int2_storage", "int3_storage", "BoundsInt",
           "m_CorrespondingSourceObject", "m_PrefabInstance", "m_PrefabAsset"]:
    COMMON[_o] = _w
    _o += len(_w) + 1

PRIM = {"bool": "?", "UInt8": "B", "SInt8": "b", "char": "c", "SInt16": "h", "short": "h", "UInt16": "H",
        "unsigned short": "H", "int": "i", "SInt32": "i", "UInt32": "I", "unsigned int": "I", "Type*": "i",
        "float": "f", "double": "d", "SInt64": "q", "long long": "q", "UInt64": "Q", "unsigned long long": "Q",
        "FileSize": "q"}


def parse(tt, E, data, pos):
    nodes = tt.nodes

    def children(i):
        lvl = nodes[i][0]
        j = i + 1
        out = []
        while j < len(nodes) and nodes[j][0] > lvl:
            if nodes[j][0] == lvl + 1:
                out.append(j)
            j += 1
        return out

    def rd(i, p):
        lvl, typ, name, size, meta = nodes[i]
        ch = children(i)
        if typ in PRIM and not ch:
            fmt = PRIM[typ]
            v = struct.unpack_from(E + fmt, data, p)[0]
            p += struct.calcsize(fmt)
        elif typ == "string":
            n = struct.unpack_from(E + "i", data, p)[0]
            v = data[p + 4:p + 4 + n].decode("utf-8", "replace")
            p += 4 + n
            p = (p + 3) // 4 * 4
            return v, p
        elif ch and nodes[ch[0]][1] == "Array":
            arr = ch[0]
            ach = children(arr)
            n = struct.unpack_from(E + "i", data, p)[0]
            p += 4
            et = nodes[ach[1]]
            if et[1] in PRIM and not children(ach[1]):
                fmt = PRIM[et[1]]
                sz = struct.calcsize(fmt)
                v = list(struct.unpack_from(E + fmt * min(n, 64), data, p)) if n else []
                p += sz * n
            else:
                v = []
                for _ in range(n):
                    x, p = rd(ach[1], p)
                    if len(v) < 64:
                        v.append(x)
            if nodes[arr][4] & 0x4000 or meta & 0x4000:
                p = (p + 3) // 4 * 4
            return v, p
        elif typ == "TypelessData":
            n = struct.unpack_from(E + "i", data, p)[0]
            p += 4 + n
            v = "<%d bytes>" % n
        else:
            v = {}
            for c in ch:
                v[nodes[c][2]], p = rd(c, p)
        if meta & 0x4000:
            p = (p + 3) // 4 * 4
        return v, p

    return rd(0, pos)[0]


LIGHT_TYPE = {0: "Spot", 1: "Directional", 2: "Point", 3: "Area"}
SHADOWS = {0: "None", 1: "Hard", 2: "Soft"}
RENDER_MODE = {0: "Auto", 1: "ForcePixel", 2: "ForceVertex"}
PROBE_MODE = {0: "Baked", 1: "Realtime", 2: "Custom"}
PROBE_REFRESH = {0: "OnAwake", 1: "EveryFrame", 2: "ViaScripting"}
PROBE_SLICE = {0: "AllFacesAtOnce", 1: "IndividualFaces", 2: "NoTimeSlicing"}


def audit_serialized(name, data):
    E, types, objs = read_serialized(data)
    counts = collections.Counter()
    gos = {}
    for pid, start, size, tidx in objs:
        cid, tt = types[tidx]
        counts[CLASS.get(cid, cid)] += 1
    print("  [%s] objects: %s" % (name, dict(counts.most_common(18))))
    for pid, start, size, tidx in objs:
        cid, tt = types[tidx]
        if cid == 1 and tt:
            try:
                o = parse(tt, E, data, start)
                gos[pid] = (o.get("m_Name"), o.get("m_IsActive"))
            except Exception:
                pass
    for pid, start, size, tidx in objs:
        cid, tt = types[tidx]
        if cid not in (20, 108, 215) or tt is None:
            continue
        o = parse(tt, E, data, start)
        go = gos.get(o.get("m_GameObject", {}).get("m_PathID"), ("?", "?"))
        en = o.get("m_Enabled")
        if cid == 108:
            sh = o.get("m_Shadows", {})
            print("    Light %-28s active=%s enabled=%s type=%-11s mode=%-10s lightmap=%s shadows=%s res=%s range=%.1f int=%.2f cull=0x%x" % (
                go[0], go[1], en, LIGHT_TYPE.get(o.get("m_Type")), RENDER_MODE.get(o.get("m_RenderMode")),
                o.get("m_Lightmapping", o.get("m_BakingOutput", {}).get("lightmapBakeType")),
                SHADOWS.get(sh.get("m_Type")), sh.get("m_Resolution"), o.get("m_Range", 0), o.get("m_Intensity", 0),
                o.get("m_CullingMask", {}).get("m_Bits", 0)))
        elif cid == 215:
            print("    ReflectionProbe %-18s active=%s enabled=%s mode=%s refresh=%s slicing=%s res=%s hdr=%s shadowDist=%s cull=0x%x" % (
                go[0], go[1], en, PROBE_MODE.get(o.get("m_Mode")), PROBE_REFRESH.get(o.get("m_RefreshMode")),
                PROBE_SLICE.get(o.get("m_TimeSlicingMode")), o.get("m_Resolution"), o.get("m_HDR"),
                o.get("m_ShadowDistance"), o.get("m_CullingMask", {}).get("m_Bits", 0)))
        elif cid == 20:
            print("    Camera %-27s active=%s enabled=%s target=%s depth=%s path=%s cull=0x%x" % (
                go[0], go[1], en, o.get("m_TargetTexture", {}).get("m_PathID"), o.get("m_Depth"),
                o.get("m_RenderingPath"), o.get("m_CullingMask", {}).get("m_Bits", 0)))


def main():
    path = sys.argv[1]
    sub = sys.argv[2] if len(sys.argv) > 2 else ""
    bundles = []
    if path.lower().endswith(".var"):
        z = zipfile.ZipFile(path)
        for n in z.namelist():
            if n.lower().endswith(".assetbundle") and sub in n:
                bundles.append((n, z.read(n)))
    else:
        bundles.append((path, open(path, "rb").read()))
    for n, buf in bundles:
        print("==", n, len(buf))
        files = read_unityfs(buf)
        for fn, data in files.items():
            if fn.endswith(".resS") or fn.endswith(".resource"):
                continue
            try:
                audit_serialized(fn, data)
            except Exception as e:
                print("  [%s] parse error %r" % (fn, e))


if __name__ == "__main__":
    main()
