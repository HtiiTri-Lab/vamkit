"""VaM's clothing pixel programs with the cloth shading controls -> <repo>/out/gen/cloth-shading/cloth_programs.h (and
test cases for tests/cloth_test.cpp in <repo>/out/gen/cloth-shading/cloth_cases/).

Every ForwardBase / ForwardAdd pixel program of Custom/Subsurface/TransparentGlossNMNoCullSeparateAlpha(ComputeBuff)
(the one shader all custom clothing uses) is lifted to HLSL instruction by instruction and compiled
again with a mode chosen by a constant buffer at b12 (clothCtl.x):

  0  the original program's result (tests only: in the game a mode-0 draw keeps VaM's own program).
  1  cloth: VaM's specular lobe (normalised Blinn-Phong, n from Gloss and the gloss map) replaced by
       - GGX with alpha^2 = 2 / (n + 2) (the same peak as VaM's lobe), Smith height-correlated visibility, times N.L,
         times N.V (VaM's lobe's light over the hemisphere falls as N.V, GGX's does not: the author's strength kept at
         every view angle, tools/brdf_energy.py);
       - Charlie sheen (Estevez & Kulla 2017, alpha = sqrt(alpha^2)) with Neubelt visibility, the cloth lobe of
         Filament / Unreal / HDRP Fabric, in place of GGX as the material gets rough (weight 0 at alpha clothSheen.x,
         1 at clothSheen.y);
       - specular anti-aliasing: alpha^2 widened by the screen-space variance of the specular normal (Tokuyoshi &
         Kaplanyan 2021: + min(2 sigma^2 (|ddx n|^2 + |ddy n|^2), kappa)); the base pass's environment cube mip moves
         with it (VaM's mip = 8 - log2(n)).
     VaM's specular colour S (spec map x _SpecColor x its Fresnel curve on N.V x _SpecInt) stays the colour of both
     lobes; clothCtl.w scales them (2 = VaM's peak at normal incidence: VaM's lobe is S (n + 2) / 4pi there, ours S
     gain (n + 2) / 8pi), clothSheen.z the sheen (2: about VaM's light at N.V 0.7, less facing, more at grazing).
     The diffuse (VaM's wrap with _SubdermisColor), the alpha, the decal and the environment light are VaM's.
(Skipping the 25 shadow taps where a light's attenuation is 0 was tried on 10-26 and saved nothing measurable in the
game; removed.)

How it is put in: the skin plugin's generator (components/shading/skin/tools/gen_spec_table.py,
imported, not changed) finds the same places by data flow, because the clothing programs share the skin programs'
lighting code: the exponent (n), the N.H power A, the specular normal W, the diffuse N.L D and its light vector L, the
view vector V, the light product, the IBL mip Y. Mode 1 overwrites A after VaM computes it with
f N.L gain / (min(10 D, 1) 0.5 (n + 2) / 2pi), so that VaM's own multiplications after it give f N.L gain S.
Only difference to the skin programs the search has to know: VaM's clothing multiplies the spec map by the alpha
("mul_sat T, alpha, T" after "add T, map, _SpecOffset"); the search sees through that one instruction.

    python -I gen_cloth_programs.py   (VAMKIT_VAM, VAMKIT_FXC)
"""
import hashlib
import json
import os
import sys

from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = next(p for p in Path(__file__).resolve().parents if (p / "components").is_dir())
GEN = REPO / "out" / "gen" / "cloth-shading"
sys.path.insert(0, str(REPO / "components" / "shading" / "skin" / "tools"))
import asm_lift as A  # noqa: E402
import skin_shaders as S  # noqa: E402
import gen_spec_table as G  # noqa: E402

FAMILY = r"^Custom/Subsurface/TransparentGlossNMNoCullSeparateAlpha(ComputeBuff)?$"
CTL_SLOT = 12

HEADER = """cbuffer ClothCtl : register(b%d) {
    float4 clothCtl;    // x mode (0 VaM, 1 cloth), y anti-aliasing filter variance (0 = off), z its cap on alpha^2,
                        // w specular gain (2 = VaM's peak)
    float4 clothSheen;  // x alpha where the sheen begins, y alpha where it is whole, z sheen gain, w unused
};
static const float CLOTH_PI = 3.14159265;
float ClothGGX(float NoH, float a2) {
    float d = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (CLOTH_PI * d * d);
}
float ClothSmith(float NoV, float NoL, float a2) {  // height-correlated, includes 1 / (4 N.L N.V)
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
float ClothCharlie(float NoH, float a) {
    float inv = 1.0 / a;
    float s2 = max(1.0 - NoH * NoH, 0.0078125);
    return (2.0 + inv) * pow(s2, inv * 0.5) / (2.0 * CLOTH_PI);
}
float ClothNeubelt(float NoV, float NoL) {
    return saturate(1.0 / (4.0 * (NoL + NoV - NoL * NoV)));
}
// the direct specular, f N.L times the gain. GGX times N.V: VaM's lobe gives about 2 N.V of light over the hemisphere
// (S = 1) where GGX gives about 2 at every angle; times N.V keeps the strength the author set at every view angle
// (tools\brdf_energy.py), so the highlight changes its shape, not its brightness
float ClothDirect(float a2, float3 n, float3 l, float3 v) {
    float3 h = normalize(l + v);
    float NoL = saturate(dot(n, l)), NoV = max(dot(n, v), 1e-4), NoH = saturate(dot(n, h));
    float a = sqrt(a2);
    float spec = ClothGGX(NoH, a2) * ClothSmith(NoV, NoL, a2) * NoV;
    float sheen = ClothCharlie(NoH, a) * ClothNeubelt(NoV, NoL) * clothSheen.z;
    float w = saturate((a - clothSheen.x) / max(clothSheen.y - clothSheen.x, 1e-4));
    return lerp(spec, sheen, w) * NoL * clothCtl.w;
}
float ClothA2(float n, float3 nw) {  // VaM's n -> GGX alpha^2, widened by the normal's screen-space variance
    float a2 = 2.0 / (n + 2.0);
    if (clothCtl.y > 0.0) {
        float3 du = ddx(nw), dv = ddy(nw);
        a2 = saturate(a2 + min(2.0 * clothCtl.y * (dot(du, du) + dot(dv, dv)), clothCtl.z));
    }
    return a2;
}
float ClothMip(float a2) { return 8.0 - log2(max(2.0 / a2 - 2.0, 1.0)); }  // VaM: mip = 8 - log2(n)
""" % CTL_SLOT


def find_anchors(p, meta):
    """gen_spec_table.find_anchors, with VaM's clothing alpha product of the spec map ("add T, map, _SpecOffset ;
    mul_sat T, alpha, T") read as the skin programs' "add_sat T, map, _SpecOffset". p is a copy only searched (main
    lifts the program twice): the add is marked saturated in it. Indices are the program's either way."""
    real = G.def_of

    def def_of(p_, k, o, n):
        j = real(p_, k, o, n)
        m = p_.instrs[j] if j is not None else None
        if m is not None and m.op == "mul" and m.sat and len(m.ops[0].mask) == 3:
            same = [x for x in m.ops[1:] if x.kind == "r" and x.index == m.ops[0].index and len(set(x.sel)) > 1]
            if len(same) == 1:
                j2 = real(p_, j, same[0], n)
                if j2 is not None and p_.instrs[j2].op == "add" and any(x.kind == "cb" for x in p_.instrs[j2].ops[1:]):
                    p_.instrs[j2].sat = True
                    return j2
        return j
    G.def_of = def_of
    try:
        return G.find_anchors(p, meta)
    finally:
        G.def_of = real


def patch(an):
    r = lambda rc: "r%d.%s" % (rc[0], "xyzw"[rc[1]])
    X, Y, Aa, D = r(an["X"]), r(an["Y"]), r(an["A"]), r(an["D"])
    ins = G.Inserts()
    ins.add(an["kv"], after="float3 clothV = normalize(%s);" % an["V"])
    nm = an["nmap"]
    if nm:
        ins.add(nm["klast"], after="float3 clothNw = %s;  // the specular normal before VaM normalises it" % an["W"])
        pre = []
    else:
        pre = ["float3 clothNw = %s;" % an["W"]]
    code = pre + [
        "float clothNn = %s;  // VaM's n" % X,
        "float3 clothN = normalize(clothNw);",
        "float clothAa = ClothA2(clothNn, clothN);"]
    if an["X"] != an["Y"]:
        code.append("if (clothCtl.x == 1.0) %s = ClothMip(clothAa);  // the environment cube's mip" % Y)
    ins.add(an["exp"] + 1, after="\n".join(code))
    ins.add(an["nl"], before="float3 clothL = %s;" % an["L"], after="float clothNdL = %s;" % D)
    ins.add(an["nh_exp"], after="\n".join([
        "if (clothCtl.x == 1.0) {",
        "    float clothM = min(clothNdL * 10.0, 1.0) * 0.5 * mad(clothNn, 0.15915494, 0.31830987);",
        "    %s = clothM > 0.0 ? ClothDirect(clothAa, clothN, clothL, clothV) / clothM : 0.0;" % Aa,
        "}"]))
    return ins


def programs():
    progs = {}
    for name, summ, blobs, pid in S.load(FAMILY):
        for ss in summ["subshaders"]:
            for ps in ss["passes"]:
                lm = ps["tags"].get("LIGHTMODE")
                if lm not in ("FORWARDBASE", "FORWARDADD"):
                    continue
                for e in ps["stages"].get("progFragment", []):
                    dx = blobs[e["blob"]]
                    h = hashlib.sha1(dx).hexdigest()[:16]
                    progs.setdefault(h, {"dx": dx, "e": e, "lm": lm, "names": set()})["names"].add(name.split("/")[-1])
    return progs


def meta_of(e):
    meta = {"lightpos": G.member(e, "UnityLighting", "_WorldSpaceLightPos0")[:2],
            "lightcolor": G.member(e, "$Globals", "_LightColor0")[:2],
            "subdermis": G.member(e, "$Globals", "_SubdermisColor")[:2],
            "speccolor": G.member(e, "$Globals", "_SpecColor")[:2],
            "specint": G.member(e, "$Globals", "_SpecInt"),
            "glossoff": G.member(e, "$Globals", "_GlossOffset"),
            "shininess": G.member(e, "$Globals", "_Shininess"),
            "maintex": e["textures"]["_MainTex"][0],
            "campos": (G.member(e, "UnityPerCamera", "_WorldSpaceCameraPos") or (None, None))[:2],
            "stereocampos": (G.member(e, "UnityStereoGlobals", "unity_StereoWorldSpaceCameraPos") or (None, None))[:2]}
    if meta["campos"][0] is None:
        meta["campos"] = None
    if meta["stereocampos"][0] is None:
        meta["stereocampos"] = None
    return meta


def main():
    GEN.mkdir(parents=True, exist_ok=True)
    cases_dir = str(GEN / "cloth_cases")
    os.makedirs(cases_dir, exist_ok=True)
    rows, manifest, report = [], [], []
    for h, pr in sorted(programs().items()):
        e, lm = pr["e"], pr["lm"]
        kw = " ".join(e["keywords"])
        meta = meta_of(e)
        found = A.lift(pr["dx"])     # the search's copy (find_anchors may mark the spec map's add saturated)
        p = A.lift(pr["dx"])         # the one compiled
        if max(p.cbs) >= CTL_SLOT:
            raise SystemExit("%s uses constant buffer slot %d" % (h, max(p.cbs)))
        try:
            an = find_anchors(found, meta)
            inserts = patch(an)
        except G.Fail as x:
            raise SystemExit("%s (%s %s): %s" % (h, lm, kw, x))
        src = p.hlsl(inserts, HEADER, precise=p.target == "ps_4_0")
        code = S.compile_hlsl(src, "main", p.target, name="cloth_%s.hlsl" % h)
        if [x[:5] for x in S.signature(code, b"ISGN")] != [x[:5] for x in S.signature(pr["dx"], b"ISGN")]:
            raise SystemExit("%s: input signature differs" % h)
        if len(S.signature(code, b"OSGN")) != 1:
            raise SystemExit("%s: output signature %s" % (h, S.signature(code, b"OSGN")))
        rows.append((h, code, 0 if lm == "FORWARDBASE" else 1))
        open(os.path.join(cases_dir, h + ".orig.dxbc"), "wb").write(pr["dx"])
        open(os.path.join(cases_dir, h + ".repl.dxbc"), "wb").write(code)
        open(os.path.join(cases_dir, h + ".hlsl"), "w", encoding="utf-8").write(src)
        open(os.path.join(cases_dir, h + ".vs.dxbc"), "wb").write(S.compile_hlsl(G.test_vs(p.sig), "main", "vs_4_0", name="vs_%s.hlsl" % h))
        eye = G.member(e, "UnityStereoEyeIndex", "unity_StereoEyeIndex")
        cmp_t = sorted({i.ops[2].index for i in p.instrs if i.op == "sample_c_lz"})
        parts = [h, p.target, "cb", str(len(p.cbs))] + ["%d:%d" % kv for kv in sorted(p.cbs.items())]
        parts += ["eye", "%d:%d:%d" % eye if eye else "-"]
        parts += ["tex", str(len(p.textures))] + ["%d:%s:%d" % (t, d, 1 if t in cmp_t else 0) for t, d in sorted(p.textures.items())]
        parts += ["smp", str(len(p.samplers))] + ["%d:%d" % (s_, 1 if c else 0) for s_, c in sorted(p.samplers.items())]
        parts += ["pass", "base" if lm == "FORWARDBASE" else "add"]
        manifest.append(" ".join(parts))
        report.append({"hash": h, "pass": lm, "keywords": e["keywords"], "shaders": sorted(pr["names"]),
                       "anchors": {k: v for k, v in an.items() if k not in ("specmap", "specoff")}, "instructions": len(p.instrs)})
        print("%s %-11s %s" % (h, lm, kw[:90]))
    rows.sort(key=lambda r_: r_[0])
    open(os.path.join(cases_dir, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
    json.dump(report, open(str(GEN / "cloth_programs.json"), "w"), indent=1)
    out = str(GEN / "cloth_programs.h")
    with open(out, "w") as f:
        f.write("// generated by tools\\gen_cloth_programs.py: VaM's clothing pixel programs with the cloth shading controls\n")
        f.write("// (key = first 8 bytes of the original's SHA-1, value = the replacement's bytecode; b12 = ClothCtl)\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("// add: 1 = a ForwardAdd (per-light) program\n")
        f.write("struct ClothProgram { uint64_t hash; const unsigned char *code; unsigned size; int add; };\n")
        for i, (h, code, add) in enumerate(rows):
            f.write("static const unsigned char kCloth%d[%d] = {%s};\n" % (i, len(code), ",".join(str(b) for b in code)))
        f.write("static const ClothProgram kClothPrograms[] = {\n")
        for i, (h, code, add) in enumerate(rows):
            f.write("    {0x%sull, kCloth%d, %d, %d},\n" % (h, i, len(code), add))
        f.write("};\nstatic const int kClothProgramCount = %d;\n" % len(rows))
    print("%d programs -> %s; test cases in %s" % (len(rows), out, cases_dir))


if __name__ == "__main__":
    main()
