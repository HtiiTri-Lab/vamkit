"""Builds <repo>\\out\\gen\\skin\\skin_programs.h: which of VaM's pixel programs are skin lighting passes, and one transmission pixel
shader (shaders\\transmission.hlsli) per layout they come in.

For every ForwardAdd pixel program of the opaque compute-buffer skin shaders (Custom/Subsurface/*, RenderType
OpaqueDynamicVertices, no tessellation) for a light with a shadow map (SPOT + SHADOWS_DEPTH, POINT / POINT_COOKIE +
SHADOWS_CUBE: the shadow map tells how much skin the light crosses; other lights get no transmission) it finds:
  - the light kind and its shadow map's slots (the program's keywords and Unity's binding tables),
  - where its inputs hold the world position, the world normal and _MainTex's UV: by running the paired vertex
    program (same pass, same keywords, same output signature) on one vertex with known values (vs_emu.py),
  - the slots and offsets of _LightColor0, _WorldSpaceLightPos0, unity_WorldToLight, _Color, _MainTex, the light
    textures and their samplers (Unity's own binding tables).
Programs with the same answers share one transmission shader, compiled here (d3dcompiler_47) and checked: its input
signature must be the original's, register for register, so it links with the skin draw's vertex shader.

usage: python -I gen_skin_table.py   (VAMKIT_VAM, VAMKIT_FXC; also writes out\\gen\\skin\\variants\\, skin_table.json)
"""
import hashlib
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skin_shaders as S  # noqa: E402
import vs_emu as V  # noqa: E402

KINDS = ["DIRECTIONAL", "POINT", "SPOT", "POINT_COOKIE", "DIRECTIONAL_COOKIE"]
OWN_T, OWN_S, OWN_B = 16, 15, 13   # the transmission shader's own slots (transmission.hlsli)

# test vertex
P = np.array([0.31, 1.27, -0.43], np.float32)
N = np.array([0.2, 0.3, 0.93], np.float32)
T4 = np.array([0.8, -0.1, 0.2, 1.0], np.float32)
UV = np.array([0.37, 0.61], np.float32)
MAIN_ST = np.array([1.5, 2.5, 0.125, 0.375], np.float32)


def rot(ax, a):
    c, s = np.cos(a), np.sin(a)
    m = np.eye(4)
    i, j = [(1, 2), (0, 2), (0, 1)][ax]
    m[i, i], m[i, j], m[j, i], m[j, j] = c, -s, s, c
    return m


M = rot(0, 0.3) @ rot(1, -0.7) @ rot(2, 0.2)
M[:3, 3] = [0.5, -0.2, 0.8]
M = M.astype(np.float32)
MINV = np.linalg.inv(M.astype(np.float64)).astype(np.float32)
WP = (M @ np.append(P, 1))[:3]
NW = (MINV[:3, :3].T @ N)
NW = NW / np.linalg.norm(NW)
UVM = UV * MAIN_ST[:2] + MAIN_ST[2:]


def fill_cb(m, slot, cb, rng):
    rows = np.zeros((cb["size"] // 16 + 8, 4), np.uint32)
    for name, (off, arr, typ) in cb["members"].items():
        n = max(arr, 1)
        r0 = off // 16
        if typ == "mat4":
            for k in range(n):
                if name == "unity_ObjectToWorld":
                    mat = M
                elif name == "unity_WorldToObject":
                    mat = MINV
                else:
                    mat = rng.uniform(-1, 1, (4, 4)).astype(np.float32)
                for c in range(4):   # column-major: register c holds column c
                    rows[r0 + 4 * k + c] = V.f2u(mat[:, c])
        else:
            for k in range(n):
                if name == "_MainTex_ST":
                    v = MAIN_ST
                elif name == "unity_WorldTransformParams":
                    v = np.array([0, 0, 0, 1], np.float32)
                elif name in ("unity_StereoEyeIndex", "unity_HalfStereoSeparation"):
                    v = np.zeros(4, np.float32)
                else:
                    v = rng.uniform(0.1, 0.9, 4).astype(np.float32)
                rows[r0 + k] = V.f2u(v)
    m.cbs[slot] = rows


def run_vs(summary_vs, dx):
    asm = S.disassemble(dx)
    m = V.Machine()
    rng = np.random.default_rng(7)
    for cb in summary_vs["cbuffers"]:
        fill_cb(m, summary_vs["cbBind"][cb["name"]], cb, rng)
    strides = {int(a): int(b) for a, b in re.findall(r"dcl_resource_structured t(\d+), (\d+)", asm)}
    for name, slot in summary_vs["buffers"].items():
        src = {"verts": P, "normals": N, "tangents": T4}.get(name)
        if src is None:
            raise ValueError("unknown buffer " + name)
        m.bufs[slot] = (strides[slot], V.pack_buffer([src], len(src)).ljust(strides[slot], b"\0"))
    for sem, idx, reg, mask, sysv, ct in S.signature(dx, b"ISGN"):
        r = m.reg("v%d" % reg)
        if sem == "POSITION":
            r[:] = V.f2u([0.11, 0.22, 0.33, 1.0])
        elif sem == "TEXCOORD" and idx == 0:
            r[:] = V.f2u([UV[0], UV[1], 0, 0])
        elif sem == "SV_VertexID":
            r[:] = 0
        elif sem == "NORMAL":
            r[:] = V.f2u(np.append(N, 0))
        elif sem == "TANGENT":
            r[:] = V.f2u(T4)
        else:
            r[:] = V.f2u([0.5, 0.5, 0.5, 0.5])
    V.run(m, asm)
    return {k: v for k, v in m.regs.items() if k.startswith("o")}


def locate(outs, vec):
    """[(register, component)] per component of vec, the first place it is found."""
    res = []
    for x in vec:
        hits = V.find(outs, float(x))
        if not hits:
            return None
        r, c = hits[0]
        res.append((int(r[1:]), c))
    return res


def sig_key(sig):
    return tuple((a, b, r, m, sv) for a, b, r, m, sv, ct in sig)


HLSL_TYPES = {1: "float", 3: "float2", 7: "float3", 15: "float4"}


def gen_hlsl(key):
    k = dict(key)
    out = []
    out.append("// generated by tools\\gen_skin_table.py")
    out.append(open(os.path.join(ROOT, "shaders", "transmission.hlsli"), encoding="utf-8").read())
    out.append("cbuffer UnityGlobals : register(b%d) { float4 cbG[%d]; };" % (k["g_slot"], k["g_rows"]))
    out.append("cbuffer UnityLighting : register(b%d) { float4 cbL[%d]; };" % (k["l_slot"], k["l_rows"]))
    t, s, _ = k["main_tex"]
    out.append("Texture2D _MainTex : register(t%d); SamplerState sMainTex : register(s%d);" % (t, s))
    if k["lt0"]:
        t, s, dim = k["lt0"]
        out.append("%s _LightTexture0 : register(t%d); SamplerState sLight0 : register(s%d);" % ("TextureCube" if dim == 4 else "Texture2D", t, s))
    if k["ltb0"]:
        t, s, _ = k["ltb0"]
        out.append("Texture2D _LightTextureB0 : register(t%d); SamplerState sLightB0 : register(s%d);" % (t, s))
    t, s = k["sh_tex"]
    # PathMM(q, Lq): how far (mm) the light travels from where it first enters the body (the depth in the light's
    # shadow map, read raw with our own linear sampler) to q, along Lq (q toward the light); 0 or less when q is
    # nearer the light than anything in the shadow map (outside the body, lit)
    if k["shadow"] == "depth":
        # spot: unity_WorldToShadow[0] is projective, so along the ray q + t Lq the shadow coordinates are c + t dc
        # and the stored depth D is met where (c.z + t dc.z) / (c.w + t dc.w) = D; the ray goes through the light,
        # so its shadow map texel stays the same
        w = k["s_w2s"]
        out.append("cbuffer UnityShadows : register(b%d) { float4 cbS[%d]; };" % (k["s_slot"], k["s_rows"]))
        out.append("Texture2D<float> _ShadowMapTexture : register(t%d);" % t)
        out.append("float PathMM(float3 q, float3 Lq) {")
        out.append("    float4 c = cbS[%d] * q.x + cbS[%d] * q.y + cbS[%d] * q.z + cbS[%d];" % (w, w + 1, w + 2, w + 3))
        out.append("    float4 dc = cbS[%d] * Lq.x + cbS[%d] * Lq.y + cbS[%d] * Lq.z;" % (w, w + 1, w + 2))
        out.append("    float D = _ShadowMapTexture.SampleLevel(stSampler, c.xy / c.w, 0);")
        out.append("    float den = dc.z - D * dc.w;")
        out.append("    float tt = (D * c.w - c.z) / (abs(den) > 1e-9 ? den : 1e-9);")
        out.append("    return tt * 1000;")
        out.append("}")
    else:
        # point: the depth cube holds 1 - (P.y / dom - P.x) for the first surface at dominant-axis distance dom
        # (reversed hardware depth), so the stored depth gives the entry's dom on the same ray from the light, and
        # distances along one ray scale with dom. VaM's own compare moves the receiver toward the light (P.z, at
        # least 5 mm) against shadow acne; a distance needs the receiver where it is, so none of that here
        out.append("TextureCube<float> _ShadowMapTexture : register(t%d);" % t)
        out.append("float PathMM(float3 q, float3 Lq) {")
        out.append("    float3 v = q - cbL[%d].xyz;" % k["l_lpr"])
        out.append("    float4 P = cbL[%d];" % k["l_lpp"])
        out.append("    float3 a = abs(v);")
        out.append("    float dom = max(max(max(a.x, a.y), a.z), 1e-6);")
        out.append("    float D = _ShadowMapTexture.SampleLevel(stSampler, v, 0);")
        out.append("    float entry = P.y / min(1 - D + P.x, -1e-9);")
        out.append("    return length(v) * (1 - entry / dom) * 1000;")
        out.append("}")
    out.append("struct PSIn {")
    for i, (sem, idx, reg, mask, sv) in enumerate(k["sig"]):
        if mask not in HLSL_TYPES:
            raise ValueError("input mask %d" % mask)
        name = "SV_POSITION" if sem == "SV_POSITION" else "%s%d" % (sem, idx)
        out.append("    %s e%d : %s;" % (HLSL_TYPES[mask], i, name))
    out.append("};")
    regname = {}
    for i, (sem, idx, reg, mask, sv) in enumerate(k["sig"]):
        for c in range(4):
            if mask & (1 << c):
                regname[(reg, c)] = "i.e%d.%s" % (i, "xyzw"[c])

    def vec(roles):
        return ", ".join(regname[(r, c)] for r, c in roles)
    out.append("float4 main(PSIn i) : SV_Target {")
    out.append("    float3 wp = float3(%s);" % vec(k["wp"]))
    out.append("    float3 n = normalize(float3(%s));" % vec(k["n"]))
    out.append("    float2 uv = float2(%s);" % vec(k["uv"]))
    lc, w2l, col = k["g_light_color"], k["g_w2l"], k["g_color"]
    lp = k["l_light_pos"]
    out.append("    float4 lightPos = cbL[%d];" % lp)
    kind = k["kind"]
    out.append("    float3 L = normalize(lightPos.xyz - wp);")
    if w2l is not None:
        out.append("    float4 lc = cbG[%d] * wp.x + cbG[%d] * wp.y + cbG[%d] * wp.z + cbG[%d];" % (w2l, w2l + 1, w2l + 2, w2l + 3))
    if kind == "POINT":
        out.append("    float atten = _LightTexture0.Sample(sLight0, dot(lc.xyz, lc.xyz).xx).x;")
    elif kind == "SPOT":
        out.append("    float atten = (lc.z > 0) * _LightTexture0.Sample(sLight0, lc.xy / lc.w + 0.5).w"
                   " * _LightTextureB0.Sample(sLightB0, dot(lc.xyz, lc.xyz).xx).x;")
    elif kind == "POINT_COOKIE":
        out.append("    float atten = _LightTextureB0.Sample(sLightB0, dot(lc.xyz, lc.xyz).xx).x * _LightTexture0.Sample(sLight0, lc.xyz).w;")
    out.append("    float d = ThicknessMM(uv);")
    out.append("    float facing = Facing(n, L);")
    out.append("    if (d >= stParams.z || atten * facing <= 0) return 0;")
    out.append("    float3 p = wp - n * stShadow.x, t1, t2;")
    out.append("    ShadowBasis(L, t1, t2);")
    out.append("    float3 T = Transmittance(max(d, PathMM(p, L)));")
    out.append("    float wsum = 1;")
    out.append("    [unroll] for (int k = 0; k < 6; k++) {")
    out.append("        float3 qi = p + InnerTap(k, t1, t2), qo = p + OuterTap(k, t1, t2);")
    out.append("        float si = PathMM(qi, normalize(lightPos.xyz - qi)), so = PathMM(qo, normalize(lightPos.xyz - qo));")
    # a ring sample the light reaches without crossing anything lies outside the body (in the light beside a thin
    # edge or in front of a curve): not skin, so it does not count (it would bring the baked thickness's light to
    # places the light only gets to across the whole nose)
    out.append("        float wi = si > 0 ? stShadow.z : 0, wo = so > 0 ? stShadow.w : 0;")
    out.append("        T += wi * Transmittance(max(d, si)) + wo * Transmittance(max(d, so));")
    out.append("        wsum += wi + wo;")
    out.append("    }")
    out.append("    T /= wsum;")
    out.append("    float3 albedo = _MainTex.Sample(sMainTex, uv).rgb * cbG[%d].rgb;" % col)
    out.append("    return float4(Transmit(T, facing, cbG[%d].rgb, atten, albedo), 0);" % lc)
    out.append("}")
    return "\n".join(out) + "\n"


def member(p, cbname, name):
    for cb in p["cbuffers"]:
        if cb["name"] == cbname and name in cb["members"]:
            off, arr, typ = cb["members"][name]
            return p["cbBind"][cbname], off, cb["size"]
    return None


def main():
    global ROOT
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    shaders = S.load()
    keys = {}          # layout key -> variant index
    variants = []      # (key, hlsl, bytecode)
    programs = []      # (hash64, variant, kind, pass, shader, keywords)
    skipped = []
    for name, summ, blobs, pid in shaders:
        for ss in summ["subshaders"]:
            for p in ss["passes"]:
                lm, rt = p["tags"].get("LIGHTMODE"), p["tags"].get("RenderType")
                if lm not in ("FORWARDBASE", "FORWARDADD") or rt != "OpaqueDynamicVertices":
                    continue
                if p["stages"]["progHull"]:
                    continue  # tessellated (…TessMapped…): the pixel inputs come from a domain shader
                vss = p["stages"]["progVertex"]
                for f in p["stages"]["progFragment"]:
                    kws = set(f["keywords"])
                    kind = [k for k in KINDS if k in kws]
                    if len(kind) != 1:
                        skipped.append((name, sorted(kws), "light kind"))
                        continue
                    kind = kind[0]
                    if kind == "SPOT" and "SHADOWS_DEPTH" in kws:
                        shadow = "depth"
                    elif kind in ("POINT", "POINT_COOKIE") and "SHADOWS_CUBE" in kws:
                        shadow = "cube"
                    else:
                        skipped.append((name, sorted(kws), "no shadow map (light without shadows, or directional)"))
                        continue
                    psdx = blobs[f["blob"]]
                    isg = S.signature(psdx, b"ISGN")
                    # paired vertex program: same output signature, keywords as close as possible
                    best = None
                    for v in vss:
                        vdx = blobs[v["blob"]]
                        if sig_key(S.signature(vdx, b"OSGN")) != sig_key(isg):
                            continue
                        vk = set(v["keywords"])
                        score = len(vk & kws) - 2 * len(vk - kws)
                        if best is None or score > best[0]:
                            best = (score, v, vdx)
                    if best is None:
                        skipped.append((name, sorted(kws), "no vertex program with this signature"))
                        continue
                    outs = run_vs(best[1], best[2])
                    wp, n, uv = locate(outs, WP), locate(outs, NW), locate(outs, UVM)
                    if not (wp and n and uv):
                        skipped.append((name, sorted(kws), "roles not found wp=%s n=%s uv=%s" % (wp, n, uv)))
                        continue
                    g = member(f, "$Globals", "_LightColor0")
                    col = member(f, "$Globals", "_Color")
                    w2l = member(f, "$Globals", "unity_WorldToLight")
                    lpos = member(f, "UnityLighting", "_WorldSpaceLightPos0")
                    tex = f["textures"]
                    if not g or not col or not lpos or "_MainTex" not in tex:
                        skipped.append((name, sorted(kws), "missing constants/textures"))
                        continue
                    if kind != "DIRECTIONAL" and not w2l:
                        skipped.append((name, sorted(kws), "no unity_WorldToLight"))
                        continue
                    sh = tex.get("_ShadowMapTexture")
                    if not sh or ("dcl_sampler s%d, mode_comparison" % sh[1]) not in S.disassemble(psdx):
                        skipped.append((name, sorted(kws), "no comparison-sampled _ShadowMapTexture"))
                        continue
                    w2s = lpr = lpp = None
                    if shadow == "depth":
                        w2s = member(f, "UnityShadows", "unity_WorldToShadow")
                        if not w2s:
                            skipped.append((name, sorted(kws), "no unity_WorldToShadow"))
                            continue
                    else:
                        lpr = member(f, "UnityLighting", "_LightPositionRange")
                        lpp = member(f, "UnityLighting", "_LightProjectionParams")
                        if not lpr or not lpp:
                            skipped.append((name, sorted(kws), "no _LightPositionRange / _LightProjectionParams"))
                            continue
                    used_t = {v[0] for v in tex.values()}
                    used_s = {v[1] for v in tex.values()}
                    if OWN_T in used_t or OWN_S in used_s or OWN_B in f["cbBind"].values():
                        skipped.append((name, sorted(kws), "own slots taken"))
                        continue
                    key = (("kind", kind), ("sig", sig_key(isg)), ("wp", tuple(wp)), ("n", tuple(n)), ("uv", tuple(uv)),
                           ("g_slot", g[0]), ("g_rows", (max(g[1], col[1], w2l[1] + 48 if w2l else 0)) // 16 + 1),
                           ("g_light_color", g[1] // 16), ("g_color", col[1] // 16), ("g_w2l", w2l[1] // 16 if w2l else None),
                           ("l_slot", lpos[0]), ("l_rows", max(lpos[1], lpr[1] if lpr else 0, lpp[1] if lpp else 0) // 16 + 1),
                           ("l_light_pos", lpos[1] // 16),
                           ("l_lpr", lpr[1] // 16 if lpr else None), ("l_lpp", lpp[1] // 16 if lpp else None),
                           ("shadow", shadow), ("sh_tex", (sh[0], sh[1])),
                           ("s_slot", w2s[0] if w2s else None), ("s_rows", w2s[1] // 16 + 4 if w2s else None),
                           ("s_w2s", w2s[1] // 16 if w2s else None),
                           ("main_tex", tuple(tex["_MainTex"])),
                           ("lt0", tuple(tex["_LightTexture0"]) if "_LightTexture0" in tex else None),
                           ("ltb0", tuple(tex["_LightTextureB0"]) if "_LightTextureB0" in tex else None))
                    if key not in keys:
                        src = gen_hlsl(key)
                        code = S.compile_hlsl(src, "main", "ps_5_0", name="transmission_%d.hlsl" % len(variants))
                        got = sig_key(S.signature(code, b"ISGN"))
                        if got != sig_key(isg):
                            raise RuntimeError("variant %d: input signature\n%s\nis not the original's\n%s" % (len(variants), got, sig_key(isg)))
                        keys[key] = len(variants)
                        variants.append((key, src, code))
                    h = hashlib.sha1(psdx).digest()
                    programs.append((int.from_bytes(h[:8], "big"), keys[key], KINDS.index(kind), 0 if lm == "FORWARDBASE" else 1,
                                     name, " ".join(sorted(kws))))
    hashes = {}
    for pr in programs:
        if pr[0] in hashes and hashes[pr[0]][1] != pr[1]:
            raise RuntimeError("same bytecode, different layouts: %s / %s" % (hashes[pr[0]], pr))
        hashes[pr[0]] = pr
    uniq = sorted(hashes.values())
    with open(os.path.join(S.gen_dir(), "skin_programs.h"), "w", encoding="utf-8") as f:
        f.write("// generated by tools\\gen_skin_table.py: VaM's skin lighting pixel programs (first 8 bytes of the\n"
                "// bytecode's SHA-1) and the transmission pixel shader for each of their layouts.\n#pragma once\n#include <stdint.h>\n\n")
        f.write("struct SkinProgram { uint64_t hash; uint16_t variant; uint8_t kind; uint8_t pass; };\n")
        f.write("static const SkinProgram kSkinPrograms[] = {\n")
        for h, vi, kind, ps, name, kws in uniq:
            f.write("    {0x%016xull, %d, %d, %d},  // %s %s\n" % (h, vi, kind, ps, name[18:], kws))
        f.write("};\nstatic const int kSkinProgramCount = %d;\n\n" % len(uniq))
        for vi, (key, src, code) in enumerate(variants):
            f.write("static const unsigned char kVariant%d[%d] = {\n" % (vi, len(code)))
            for i in range(0, len(code), 24):
                f.write("    " + ",".join("%d" % b for b in code[i:i + 24]) + ",\n")
            f.write("};\n")
        f.write("// shadowT / shadowS: the light's shadow map and its comparison sampler (the slots VaM's program uses);\n"
                "// shadowCube: a depth cube (point light), else a 2D depth map (spot light)\n")
        f.write("struct SkinVariant { const unsigned char *code; unsigned size; uint8_t shadowT, shadowS, shadowCube; };\n")
        f.write("static const SkinVariant kSkinVariants[] = {\n")
        for vi, (key, src, code) in enumerate(variants):
            k = dict(key)
            f.write("    {kVariant%d, %d, %d, %d, %d},\n" % (vi, len(code), k["sh_tex"][0], k["sh_tex"][1], 1 if k["shadow"] == "cube" else 0))
        f.write("};\nstatic const int kSkinVariantCount = %d;\n" % len(variants))
    build = S.gen_dir("variants")
    for vi, (key, src, code) in enumerate(variants):
        with open(os.path.join(build, "variant%02d.hlsl" % vi), "w", encoding="utf-8") as f:
            f.write(src)
    report = {"programs": len(uniq), "variants": len(variants),
              "by_shader": {}, "skipped": [list(s) for s in skipped]}
    for h, vi, kind, ps, name, kws in uniq:
        report["by_shader"][name] = report["by_shader"].get(name, 0) + 1
    with open(os.path.join(S.gen_dir(), "skin_table.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print("%d skin pixel programs (%d entries), %d transmission variants, %d skipped" % (len(uniq), len(programs), len(variants), len(skipped)))
    for s in skipped[:10]:
        print("  skipped:", s)


if __name__ == "__main__":
    main()
