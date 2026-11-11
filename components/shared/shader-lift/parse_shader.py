"""Parse GPUTools/MeshedVR/HairOpt (resources.assets, no type trees) with the
Shader type tree taken from a StreamingAssets bundle, and write a JSON summary:
passes -> stages -> sub-programs (blob index, keywords, program type, constant
buffer layouts, texture/buffer bindings).
usage: python -I parse_shader.py <assets> <pathid> <shader_tt.json> <out.json>
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bundle_audit as B  # noqa: E402

PRIM = B.PRIM


def load_type_tree(path=None):
    """The Shader type tree [(level, type, name, size, meta)]. Without a path: the one generated from the user's
    VaM into <repo>/out/gen/shader-lift/shader_tt.json, made now if it is not there (find_shader_tt.py, VAMKIT_VAM)."""
    if path is None:
        import find_shader_tt as F
        path = F.default_out()
        if not os.path.isfile(path):
            F.generate(out=path)
    with open(path, encoding="utf-8") as f:
        return [tuple(n) for n in json.load(f)]


def parse(nodes, data, pos, E="<"):
    kids = [[] for _ in nodes]
    stack = []
    for i, n in enumerate(nodes):
        while stack and nodes[stack[-1]][0] >= n[0]:
            stack.pop()
        if stack:
            kids[stack[-1]].append(i)
        stack.append(i)

    def rd(i, p):
        lvl, typ, name, size, meta = nodes[i]
        ch = kids[i]
        if typ in PRIM and not ch:
            fmt = PRIM[typ]
            v = struct.unpack_from(E + fmt, data, p)[0]
            p += struct.calcsize(fmt)
        elif typ == "string":
            n = struct.unpack_from(E + "i", data, p)[0]
            v = data[p + 4:p + 4 + n].decode("utf-8", "replace")
            p = (p + 4 + n + 3) // 4 * 4
            return v, p
        elif ch and nodes[ch[0]][1] == "Array":
            arr = ch[0]
            et = kids[arr][1]
            n = struct.unpack_from(E + "i", data, p)[0]
            p += 4
            if nodes[et][1] in PRIM and not kids[et]:
                fmt = PRIM[nodes[et][1]]
                v = list(struct.unpack_from(E + fmt * n, data, p))
                p += struct.calcsize(fmt) * n
            else:
                v = []
                for _ in range(n):
                    x, p = rd(et, p)
                    v.append(x)
            if nodes[arr][4] & 0x4000 or meta & 0x4000:
                p = (p + 3) // 4 * 4
            return v, p
        elif typ == "TypelessData":
            n = struct.unpack_from(E + "i", data, p)[0]
            v = data[p + 4:p + 4 + n]
            p += 4 + n
        else:
            v = {}
            for c in ch:
                v[nodes[c][2]], p = rd(c, p)
        if meta & 0x4000:
            p = (p + 3) // 4 * 4
        return v, p

    return rd(0, pos)


def main():
    path, pathid, ttp, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    data = open(path, "rb").read()
    E, types, objs = B.read_serialized(data)
    nodes = load_type_tree(ttp)
    for pid, start, size, tidx in objs:
        if pid != pathid:
            continue
        v, end = parse(nodes, data, start, E)
        print("parsed", end - start, "of", size, "bytes")
        pf = v["m_ParsedForm"]
        summary = {"name": pf["m_Name"], "subshaders": []}
        for ss in pf["m_SubShaders"]:
            passes = []
            for ps in ss["m_Passes"]:
                names = {idx: nm for nm, idx in ((d["first"], d["second"]) for d in ps["m_NameIndices"])}
                tags = {t["first"]: t["second"] for t in ps["m_Tags"]["tags"]} if "m_Tags" in ps else {}
                stages = {}
                for st in ("progVertex", "progFragment", "progGeometry", "progHull", "progDomain"):
                    subs = []
                    for sp in ps[st]["m_SubPrograms"]:
                        cbs = []
                        for cb in sp.get("m_ConstantBuffers", []):
                            members = [(names.get(m["m_NameIndex"], m["m_NameIndex"]), m["m_Index"], m["m_ArraySize"],
                                        ("mat%d" % m["m_RowCount"]) if "m_RowCount" in m else ("vec%d" % m["m_Dim"]))
                                       for m in cb["m_VectorParams"] + cb["m_MatrixParams"]]
                            members += [(names.get(s["m_NameIndex"], s["m_NameIndex"]), s["m_Index"], s["m_ArraySize"], "struct%d" % s["m_StructSize"])
                                        for s in cb["m_StructParams"]]
                            cbs.append({"name": names.get(cb["m_NameIndex"], cb["m_NameIndex"]), "size": cb["m_Size"],
                                        "members": sorted(members, key=lambda m: m[1])})
                        bind = [(names.get(b["m_NameIndex"], b["m_NameIndex"]), b["m_Index"]) for b in sp.get("m_ConstantBufferBindings", [])]
                        tex = [(names.get(t["m_NameIndex"], t["m_NameIndex"]), t["m_Index"], t["m_SamplerIndex"], t["m_Dim"]) for t in sp["m_TextureParams"]]
                        buf = [(names.get(b["m_NameIndex"], b["m_NameIndex"]), b["m_Index"]) for b in sp["m_BufferParams"]]
                        glob = [(names.get(m["m_NameIndex"], m["m_NameIndex"]), m["m_Index"]) for m in sp["m_VectorParams"] + sp["m_MatrixParams"]]
                        subs.append({"blob": sp["m_BlobIndex"], "type": sp["m_GpuProgramType"], "tier": sp["m_ShaderHardwareTier"],
                                     "keywords": sorted(names.get(k, "#%d" % k) for k in sp["m_KeywordIndices"]),
                                     "cbuffers": cbs, "cbBind": bind, "textures": tex, "buffers": buf, "globals": glob,
                                     "samplers": [(s["sampler"], s["bindPoint"]) for s in sp["m_Samplers"]]})
                    stages[st] = subs
                passes.append({"type": ps["m_Type"], "name": ps["m_State"]["m_Name"], "tags": tags,
                               "useName": ps.get("m_UseName"), "programMask": ps.get("m_ProgramMask"), "stages": stages})
            summary["subshaders"].append({"passes": passes})
        json.dump(summary, open(out, "w"), indent=1, default=str)
        return
    print("pathid not found")


if __name__ == "__main__":
    main()
