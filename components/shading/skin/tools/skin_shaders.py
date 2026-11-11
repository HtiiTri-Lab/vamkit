"""VaM's skin shaders (Custom/Subsurface/*) read straight from VaM_Data\\globalgamemanagers.assets: every pass with its
sub-programs (keywords, constant buffer layouts, texture and sampler slots, as Unity serialised them) and each
sub-program's D3D11 bytecode (DXBC), by blob index.

Uses the readers in components\\shared\\shader-lift (bundle_audit, parse_shader, shader_extract) and the Shader type
tree they read out of VaM (find_shader_tt.py; generated on first use into <repo>\\out\\gen\\shader-lift, not kept in
the repo). VaM is found through the environment variable VAMKIT_VAM (its install folder; only its
files are read), the HLSL compiler (d3dcompiler_47.dll) through VAMKIT_FXC (the Windows SDK bin\\<version>\\x64 folder,
or fxc.exe in it). What the generators make from VaM's programs goes to <repo>\\out\\gen\\skin (gen_dir), never into
the source tree.
"""
import os
import re
import struct
import sys
from pathlib import Path

REPO = next(p for p in Path(__file__).resolve().parents if (p / "components").is_dir())
sys.path.insert(0, str(REPO / "components" / "shared" / "shader-lift"))
import bundle_audit as B  # noqa: E402
import parse_shader as P  # noqa: E402
import shader_extract as X  # noqa: E402

D3D11 = 4  # platform id of the D3D11 programs in Unity 2018.1's shader blob


def vam_root():
    """VaM's install folder, from VAMKIT_VAM"""
    v = os.environ.get("VAMKIT_VAM")
    if not v:
        raise SystemExit("set VAMKIT_VAM to the VaM install folder (the one holding VaM_Data)")
    if not os.path.isdir(os.path.join(v, "VaM_Data")):
        raise SystemExit("VAMKIT_VAM=%s has no VaM_Data folder" % v)
    return v


def gen_dir(*sub):
    """<repo>\\out\\gen\\skin (or a folder in it), created"""
    d = os.path.join(str(REPO), "out", "gen", "skin", *sub)
    os.makedirs(d, exist_ok=True)
    return d


def shader_name(obj):
    for m in re.finditer(rb"[A-Za-z][A-Za-z0-9 _()\-]*/[A-Za-z0-9 _/()\-]+", obj):
        s = m.start()
        if s >= 4 and struct.unpack_from("<I", obj, s - 4)[0] == len(m.group(0)):
            return m.group(0).decode("latin1")
    return None


def first_dxbc(raw):
    i = raw.find(b"DXBC")
    if i < 0:
        return None
    size = struct.unpack_from("<I", raw, i + 24)[0]
    return raw[i:i + size]


def summarize(pf):
    """parse_shader's JSON summary, built from the parsed form."""
    out = {"name": pf["m_Name"], "subshaders": []}
    for ss in pf["m_SubShaders"]:
        passes = []
        for ps in ss["m_Passes"]:
            names = {idx: nm for nm, idx in ((d["first"], d["second"]) for d in ps["m_NameIndices"])}
            tags = {}
            st_ = ps.get("m_State", {})
            if isinstance(st_.get("m_Tags"), dict):
                tags = {t["first"]: t["second"] for t in st_["m_Tags"].get("tags", [])}
            stages = {}
            for st in ("progVertex", "progFragment", "progGeometry", "progHull", "progDomain"):
                subs = []
                for sp in ps[st]["m_SubPrograms"]:
                    cbs = []
                    for cb in sp.get("m_ConstantBuffers", []):
                        members = [(names.get(m["m_NameIndex"], m["m_NameIndex"]), m["m_Index"], m["m_ArraySize"],
                                    ("mat%d" % m["m_RowCount"]) if "m_RowCount" in m else ("vec%d" % m["m_Dim"]))
                                   for m in cb["m_VectorParams"] + cb["m_MatrixParams"]]
                        cbs.append({"name": names.get(cb["m_NameIndex"], cb["m_NameIndex"]), "size": cb["m_Size"],
                                    "members": {m[0]: (m[1], m[2], m[3]) for m in members}})
                    bind = {names.get(b["m_NameIndex"], b["m_NameIndex"]): b["m_Index"] for b in sp.get("m_ConstantBufferBindings", [])}
                    tex = {names.get(t["m_NameIndex"], t["m_NameIndex"]): (t["m_Index"], t["m_SamplerIndex"], t["m_Dim"]) for t in sp["m_TextureParams"]}
                    buf = {names.get(b["m_NameIndex"], b["m_NameIndex"]): b["m_Index"] for b in sp["m_BufferParams"]}
                    subs.append({"blob": sp["m_BlobIndex"], "type": sp["m_GpuProgramType"],
                                 "keywords": sorted(names.get(k, "#%d" % k) for k in sp["m_KeywordIndices"]),
                                 "cbuffers": cbs, "cbBind": bind, "textures": tex, "buffers": buf})
                stages[st] = subs
            passes.append({"name": st_.get("m_Name", ""), "tags": tags, "stages": stages})
        out["subshaders"].append({"passes": passes})
    return out


def load(want=r"^Custom/Subsurface/",data=None):
    """[(name, summary, blobs)] with blobs[i] = DXBC bytes of blob index i (D3D11)."""
    rx = re.compile(want)
    if data is None:data = open(os.path.join(vam_root(), "VaM_Data", "globalgamemanagers.assets"), "rb").read()
    E, types, objs = B.read_serialized(data)
    nodes = P.load_type_tree()
    result = []
    for pid, start, size, tidx in objs:
        if types[tidx][0] != 48:
            continue
        obj = data[start:start + size]
        name = shader_name(obj)
        if not name or not rx.search(name):
            continue
        tree=types[tidx][1]
        v, end = P.parse(tree.nodes if tree else nodes, data, start, E)
        summary = summarize(v["m_ParsedForm"])
        plats, offs, clen, dlen, blob = X.find_blob(obj)
        k = list(plats).index(D3D11)
        raw = B.decompress(blob[offs[k]:offs[k] + clen[k]], 2, dlen[k])
        n = struct.unpack_from("<I", raw, 0)[0]
        blobs = []
        for i in range(n):
            o, s = struct.unpack_from("<II", raw, 4 + 8 * i)
            blobs.append(first_dxbc(raw[o:o + s]))
        result.append((summary["name"], summary, blobs, pid))
    return result


# ---- DXBC signatures ---------------------------------------------------------------------------------------------

def chunks(dx):
    count = struct.unpack_from("<I", dx, 28)[0]
    for c in range(count):
        off = struct.unpack_from("<I", dx, 32 + 4 * c)[0]
        yield dx[off:off + 4], off + 8, struct.unpack_from("<I", dx, off + 4)[0]


def signature(dx, tag):
    """[(semantic, index, register, mask, sysvalue, comptype)] of chunk ISGN / OSGN (or the *1 forms)."""
    for t, off, size in chunks(dx):
        if t != tag:
            continue
        n = struct.unpack_from("<I", dx, off)[0]
        out = []
        for e in range(n):
            nameoff, idx, sysv, ctype, reg, mask, rw, _, _ = struct.unpack_from("<IIIIIBBBB", dx, off + 8 + 24 * e)
            name = dx[off + nameoff:dx.index(b"\0", off + nameoff)].decode()
            out.append((name, idx, reg, mask, sysv, ctype))
        return out
    return None


def stage(dx):
    return X.stage_of(dx)


# ---- disassembly and compiling (the Windows SDK's d3dcompiler_47.dll, VAMKIT_FXC) --------------------------------

_d3dc = None


def _compiler():
    global _d3dc
    if _d3dc is None:
        import ctypes
        v = os.environ.get("VAMKIT_FXC")
        if not v:
            raise SystemExit("set VAMKIT_FXC to the Windows SDK bin\\<version>\\x64 folder (fxc.exe, d3dcompiler_47.dll)")
        d = os.path.dirname(v) if v.lower().endswith(".exe") else v
        dll = os.path.join(d, "d3dcompiler_47.dll")
        if not os.path.isfile(dll):
            raise SystemExit("VAMKIT_FXC=%s: no d3dcompiler_47.dll there" % v)
        _d3dc = ctypes.WinDLL(dll)
    return _d3dc


def _blob_bytes(blob):
    import ctypes
    vt = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    data = ctypes.string_at(get_ptr(blob), get_size(blob))
    release(blob)
    return data


def disassemble(dx):
    import ctypes
    out = ctypes.c_void_p()
    hr = _compiler().D3DDisassemble(dx, len(dx), 0, None, ctypes.byref(out))
    if hr != 0:
        raise RuntimeError("D3DDisassemble 0x%08x" % (hr & 0xffffffff))
    return _blob_bytes(out).decode("latin1").rstrip("\0")


def compile_hlsl(src, entry, target, defines=None, name="gen.hlsl", flags=0):
    """Bytecode, or raises with the compiler's messages."""
    import ctypes

    class Macro(ctypes.Structure):
        _fields_ = [("Name", ctypes.c_char_p), ("Definition", ctypes.c_char_p)]
    ms = None
    if defines:
        arr = (Macro * (len(defines) + 1))()
        for i, (k, v) in enumerate(defines.items()):
            arr[i].Name = k.encode()
            arr[i].Definition = str(v).encode()
        ms = arr
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    s = src.encode()
    # D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15) | D3DCOMPILE_ENABLE_STRICTNESS (1 << 11)
    hr = _compiler().D3DCompile(s, len(s), name.encode(), ms, None, entry.encode(), target.encode(),
                                (1 << 15) | (1 << 11) | flags, 0, ctypes.byref(code), ctypes.byref(err))
    msg = _blob_bytes(err).decode("latin1") if err.value else ""
    if hr != 0:
        raise RuntimeError("D3DCompile 0x%08x:\n%s" % (hr & 0xffffffff, msg))
    return _blob_bytes(code)
