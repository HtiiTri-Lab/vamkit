// path_omm.hlsl: the path tracer's opacity micromaps (rtcore.cpp "PT opacity micromaps", path_omm.inl). As the removed
// atlas bake was - level 4, 4-state, one thread per 16 micro-triangles - but from the PT material's own alpha map (its original
// resolution and address modes, path_trace.hlsl PtAlphaAt / PtRead) and the PT's coverage rules: a micro-triangle is
// transparent only where every PT ray skips each hit in it (PtHitTracked, PtVisibility), opaque only where every ray
// takes it, else unknown - the shaders' own test, as before. The rules that apply to the material come from the host.
#include "rtc_common.hlsli"
#include "rtc_omm.hlsli"

#define PT_OMM_LEVEL 4
#define PT_OMM_WORDS 16      // rtcore.cpp kOmmWords
#define PT_OMM_BYTES 64
#define PT_OMM_FORMAT 2u     // OC1_4_STATE
#define PT_OMM_TRANSPARENT 0u
#define PT_OMM_OPAQUE 1u
#define PT_OMM_UNKNOWN 2u    // unknown-transparent: a candidate for the shaders
// rules (path_omm.inl PtOmmRules)
#define PT_OMM_CUT 1u        // coverage below `cutoff` is skipped (cutout, alpha-tested families)
#define PT_OMM_ZERO 2u       // coverage 0 is skipped (stochastic coverage that attenuates shadow rays by 1 - coverage)
#define PT_OMM_ANY 4u        // a hit past the cutoff is taken by every ray, whatever its coverage
#define PT_OMM_ONE 8u        // a hit of coverage 1 is taken by every ray
#define PT_OMM_MARGIN 0.002  // alpha kept off a bound unless it is exact (0 or 1 texels; R8 steps are 0.0039)
#define PT_OMM_SPAN 64       // texels per axis a micro-triangle may read before it is left unknown

struct PtOmmParams {
    uint texture, sampling, rules; float cutoff;  // sampling: PtMaterial.sampling1.w (PtRead's mode)
    float4 st;                                    // PtMaterial.alphaST
    float adjust, base, offset, pad;              // PtAlphaAt's bump.w; PtOpacity's base.a and post-colour offset
};

// The texels one axis of a uv interval can read through PtRead's address mode and the bilinear filter (a superset: the
// filter's reach and a texel of margin). False: too many (the micro-triangle stays unknown).
bool PtOmmAxis(float lo, float hi, uint mode, int size, out int i0, out int i1) {
    i0 = 0; i1 = -1;
    if (!(abs(lo) < 256 && abs(hi) < 256)) return false;  // NaN fails too; float steps stay well under a texel
    if (mode == 2) {         // mirror: 1 - |frac(u / 2) 2 - 1|, 0 at even integers, 1 at odd ones
        float fa = 1 - abs(frac(lo * .5) * 2 - 1), fb = 1 - abs(frac(hi * .5) * 2 - 1);
        float a = min(fa, fb), b = max(fa, fb);
        if (hi - lo >= 2) { a = 0; b = 1; }
        else [loop] for (float k = ceil(lo); k <= floor(hi); k++) {
            if (fmod(abs(k), 2) == 0) a = 0; else b = 1;
        }
        lo = a; hi = b;
    } else if (mode == 3) {  // mirror once: saturate(|u|)
        float a = lo <= 0 && hi >= 0 ? 0 : min(abs(lo), abs(hi)), b = max(abs(lo), abs(hi));
        lo = saturate(a); hi = saturate(b);
    }
    i0 = (int)floor(lo * size - .5) - 1;
    i1 = (int)floor(hi * size - .5) + 2;
    if (i1 - i0 > PT_OMM_SPAN) return false;
    if (mode != 0) { i0 = clamp(i0, 0, size - 1); i1 = clamp(i1, 0, size - 1); }  // clamp samplers (2, 3 land in [0, 1] first)
    return true;
}

// PtOpacity(PtAlphaAt) at an alpha bound, before its saturate: saturate(alpha + adjust) base + offset. `slack`: 0 where
// that a is exactly 0 or 1 (a base is then exact, so fused or not every shader rounds it alike: the filter returns
// exactly 0 / 1 when every texel is), else 1e-6 kept off any line the coverage is compared with.
float PtOmmRaw(PtOmmParams p, float alpha, out float slack) {
    float a = saturate(alpha + p.adjust);
    slack = a == 0 || a == 1 ? 0 : 1e-6;
    return a * p.base + p.offset;
}

uint PtOmmState(PtOmmParams p, Texture2D<float4> t, int2 size, float2 p0, float2 p1, float2 p2) {
    float2 lo = min(p0, min(p1, p2)), hi = max(p0, max(p1, p2));
    int x0, x1, y0, y1;
    if (!PtOmmAxis(lo.x, hi.x, p.sampling & 3, size.x, x0, x1) || !PtOmmAxis(lo.y, hi.y, (p.sampling >> 2) & 3, size.y, y0, y1))
        return PT_OMM_UNKNOWN;
    float mn = 1, mx = 0;
    [loop] for (int y = y0; y <= y1; y++)
        [loop] for (int x = x0; x <= x1; x++) {
            int2 c = int2(x, y) % size;
            c += select(c < 0, size, 0);
            float alpha = t.Load(int3(c, 0)).a;
            mn = min(mn, alpha);
            mx = max(mx, alpha);
        }
    // bounds of the filtered alpha: exact where every texel is 0 (or 1), else a margin past the texels
    float aHi = mx <= 0 ? 0 : min(mx + PT_OMM_MARGIN, 1), aLo = mn >= 1 ? 1 : max(mn - PT_OMM_MARGIN, 0);
    if (!(p.base >= 0)) return PT_OMM_UNKNOWN;  // coverage rises with alpha only for base >= 0 (never below in VaM)
    float sHi, sLo, rHi = PtOmmRaw(p, aHi, sHi), rLo = PtOmmRaw(p, aLo, sLo);
    if ((p.rules & PT_OMM_CUT) && saturate(rHi) + sHi < p.cutoff) return PT_OMM_TRANSPARENT;
    if ((p.rules & PT_OMM_ZERO) && rHi + sHi <= 0) return PT_OMM_TRANSPARENT;
    bool past = !(p.rules & PT_OMM_CUT) || saturate(rLo) - sLo >= p.cutoff;
    if (past && ((p.rules & PT_OMM_ANY) || ((p.rules & PT_OMM_ONE) && rLo - sLo >= 1))) return PT_OMM_OPAQUE;
    return PT_OMM_UNKNOWN;
}

// pA geometry record, pB submesh record, pC micromap data UAV (PT_OMM_BYTES each), pD micromap descs UAV (8 bytes
// each), pE OMM index UAV (uint per triangle), pF the submesh's first micromap, pG its first index entry, pH the
// geometry's PtOmmParams (SRV), pLight this submesh's among them, pCount triangles x PT_OMM_WORDS
[RootSignature(RTC_ROOT)] [numthreads(64, 1, 1)] void PathOmmBake(uint3 dtid : SV_DispatchThreadID) {
    uint id = Linear(dtid);
    if (id >= pCount) return;
    uint tri = id / PT_OMM_WORDS, word = id % PT_OMM_WORDS;
    GeoGpu g = LoadGeo(pA);
    SubGpu s = LoadSub(pB);
    PtOmmParams p = gBuf[pH].Load<PtOmmParams>(pLight * 48);
    Texture2D<float4> t = gTex[p.texture];
    uint w, h, levels;
    t.GetDimensions(0, w, h, levels);
    uint3 v = LoadTriangle(g, s.triStart + tri);
    float2 t0 = LoadFloat2(g.uvBuf, g.uvOffset, g.uvStride, g.uvFormat, v.x);
    float2 t1 = LoadFloat2(g.uvBuf, g.uvOffset, g.uvStride, g.uvFormat, v.y);
    float2 t2 = LoadFloat2(g.uvBuf, g.uvOffset, g.uvStride, g.uvFormat, v.z);
    uint bits = 0;
    [loop] for (uint k = 0; k < 16; k++) {
        float2 c[3];
        OmmMicroTriangle(word * 16 + k, PT_OMM_LEVEL, c[0], c[1], c[2]);
        float2 q[3];
        [unroll] for (uint j = 0; j < 3; j++) q[j] = ((1 - c[j].x - c[j].y) * t0 + c[j].x * t1 + c[j].y * t2) * p.st.xy + p.st.zw;
        bits |= PtOmmState(p, t, int2(w, h), q[0], q[1], q[2]) << (2 * k);
    }
    uint omm = pF + tri;
    gRwBuf[pC].Store(omm * PT_OMM_BYTES + word * 4, bits);
    if (word == 0) {
        gRwBuf[pD].Store2(omm * 8, uint2(omm * PT_OMM_BYTES, PT_OMM_LEVEL | (PT_OMM_FORMAT << 16)));
        gRwBuf[pE].Store((pG + tri) * 4, omm);
    }
}
