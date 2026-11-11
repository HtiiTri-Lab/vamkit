// VaM tone mapping 0.2.1: bloom and a highlight shoulder on the eye camera's HDR target (OnRenderImage's source),
// compute only (native\tonemap.cpp). The target may hold both eyes side by side (single-pass stereo): every read is
// clamped to the eye it belongs to, so neither the blur nor the bilinear reads cross the seam.
//
// Bloom (Jimenez 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare", as Unity's PostProcessing
// v2): a mip chain of 13-tap downsamples (the first one with Karis' 1 / (1 + luma) weights against single bright
// pixels, then the threshold with a soft knee: only light above 1.0, the old clip point), tent upsamples summed back
// up, the sum over the levels added tinted (film halation, warm). Tone: per channel, unchanged up to the knee, an
// exponential shoulder above it that reaches white smoothly (the shoulder of Uchimura's Gran Turismo curve without its
// toe), so everything VaM drew below the knee keeps its value and what it clipped keeps its gradation.
//
// 0.2.0, the film style (CSCompositeFilm, in place of the shoulder): a whole characteristic curve - Lottes' 2016
// operator x^a / (x^(a d) b + c) ("Advanced Techniques and Optimization of HDR Color Pipelines"), a toe that deepens the
// shadows, more contrast through the mids, mid grey 0.18 lifted to 0.21, white reached at 3x VaM's old clip point - on
// the brightest channel, the colour kept as its ratios to it; those ratios a little more saturated in the mids and
// running to white as the curve reaches it (dye crosstalk: bright colours bleach to white instead of staying flat
// primaries); then a print's split tone, the shadows a little cyan, the highlights a little warm, at equal luminance.

cbuffer TmPass : register(b0) {
    float4 gSrc;    // the texture read (t0): x eye width, y height, z 1 / full width, w 1 / height
    float4 gAdd;    // the second texture read (t1), as gSrc
    float4 gDst;    // the texture written (u0): x eye width, y height, z eyes
    float4 gParam;  // x knee, y threshold, z soft knee, w bloom intensity / levels
    float4 gTint;   // rgb the bloom's tint
    float4 gFilm;   // the film curve: x contrast a, y shoulder d, z b, w c (native FilmCurve)
    float4 gLook;   // x the ratios' saturation, y the crosstalk's exponent, z the split tone's strength
    float4 gCool;   // rgb the shadows' tint (luminance 1)
    float4 gWarm;   // rgb the highlights' tint (luminance 1)
};

Texture2D<float4> tSrc : register(t0);
Texture2D<float4> tAdd : register(t1);
SamplerState sLinear : register(s0);
RWTexture2D<float4> uDst : register(u0);

// a bilinear read at p (texels of one eye of texture t whose sizes are s), kept inside that eye
float3 Read(Texture2D<float4> t, float4 s, float eye, float2 p) {
    p = clamp(p, 0.5, s.xy - 0.5);
    return t.SampleLevel(sLinear, float2((eye * s.x + p.x) * s.z, p.y * s.w), 0).rgb;
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// the thread's pixel of the written texture: false outside it; its eye and, in the texture read, its centre
bool Locate(uint2 id, float4 read, out float eye, out float2 p) {
    eye = 0.0;
    p = 0.0;
    if (id.x >= (uint)(gDst.x * gDst.z) || id.y >= (uint)gDst.y) return false;
    eye = floor((id.x + 0.5) / gDst.x);
    p = (float2(id.x - eye * gDst.x, id.y) + 0.5) * read.xy / gDst.xy;
    return true;
}

float3 Box4(float3 a, float3 b, float3 c, float3 d) { return (a + b + c + d) * 0.25; }

// 13 bilinear reads: the inner box (weight 1/2) and four corner boxes (1/8 each); karis: each box over 1 + its luma
float3 Down13(float eye, float2 p, bool karis) {
    float3 A = Read(tSrc, gSrc, eye, p + float2(-2, -2)), B = Read(tSrc, gSrc, eye, p + float2(0, -2));
    float3 C = Read(tSrc, gSrc, eye, p + float2(2, -2)), D = Read(tSrc, gSrc, eye, p + float2(-1, -1));
    float3 E = Read(tSrc, gSrc, eye, p + float2(1, -1)), F = Read(tSrc, gSrc, eye, p + float2(-2, 0));
    float3 G = Read(tSrc, gSrc, eye, p), H = Read(tSrc, gSrc, eye, p + float2(2, 0));
    float3 I = Read(tSrc, gSrc, eye, p + float2(-1, 1)), J = Read(tSrc, gSrc, eye, p + float2(1, 1));
    float3 K = Read(tSrc, gSrc, eye, p + float2(-2, 2)), L = Read(tSrc, gSrc, eye, p + float2(0, 2));
    float3 M = Read(tSrc, gSrc, eye, p + float2(2, 2));
    float3 b0 = Box4(D, E, I, J), b1 = Box4(A, B, F, G), b2 = Box4(B, C, G, H), b3 = Box4(F, G, K, L), b4 = Box4(G, H, L, M);
    float4 w = float4(0.125, 0.125, 0.125, 0.125);
    float w0 = 0.5;
    if (karis) {
        w0 /= 1.0 + Luma(b0);
        w /= 1.0 + float4(Luma(b1), Luma(b2), Luma(b3), Luma(b4));
    }
    return (b0 * w0 + b1 * w.x + b2 * w.y + b3 * w.z + b4 * w.w) / (w0 + dot(w, 1.0));
}

// the light above the threshold, with a quadratic soft knee below it
float3 Prefilter(float3 c) {
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - gParam.y + gParam.z, 0.0, 2.0 * gParam.z);
    soft = soft * soft / (4.0 * gParam.z + 1e-5);
    return c * (max(soft, br - gParam.y) / max(br, 1e-5));
}

// 3x3 tent around p (texels of texture t)
float3 Tent(Texture2D<float4> t, float4 s, float eye, float2 p) {
    float3 c = Read(t, s, eye, p) * 4.0;
    c += (Read(t, s, eye, p + float2(-1, 0)) + Read(t, s, eye, p + float2(1, 0)) + Read(t, s, eye, p + float2(0, -1)) +
          Read(t, s, eye, p + float2(0, 1))) * 2.0;
    c += Read(t, s, eye, p + float2(-1, -1)) + Read(t, s, eye, p + float2(1, -1)) + Read(t, s, eye, p + float2(-1, 1)) +
         Read(t, s, eye, p + float2(1, 1));
    return c * (1.0 / 16.0);
}

// the first level: from the camera's target (NaN, infinity and negative values made 0..64 first)
[numthreads(8, 8, 1)]
void CSDownFirst(uint3 id : SV_DispatchThreadID) {
    float eye;
    float2 p;
    if (!Locate(id.xy, gSrc, eye, p)) return;
    uDst[id.xy] = float4(Prefilter(clamp(Down13(eye, p, true), 0.0, 64.0)), 1.0);
}

[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID) {
    float eye;
    float2 p;
    if (!Locate(id.xy, gSrc, eye, p)) return;
    uDst[id.xy] = float4(Down13(eye, p, false), 1.0);
}

// level k: its own downsample (t1, same size as u0) plus the tent of level k + 1's sum (t0)
[numthreads(8, 8, 1)]
void CSUp(uint3 id : SV_DispatchThreadID) {
    float eye;
    float2 p;
    if (!Locate(id.xy, gSrc, eye, p)) return;
    uDst[id.xy] = float4(tAdd[id.xy].rgb + Tent(tSrc, gSrc, eye, p), 1.0);
}

float3 Shoulder(float3 x) {
    float k = gParam.x;
    float3 s = 1.0 - (1.0 - k) * exp((k - x) / (1.0 - k));
    return x > k ? s : x;
}

// the picture: the camera's target (t0, read texel for texel) plus the tinted bloom (t1, level 1's sum), toned
[numthreads(8, 8, 1)]
void CSComposite(uint3 id : SV_DispatchThreadID) {
    float eye;
    float2 p;
    if (!Locate(id.xy, gAdd, eye, p)) return;
    float4 c = tSrc[id.xy];
    float3 bloom = Tent(tAdd, gAdd, eye, p) * gParam.w;
    uDst[id.xy] = float4(Shoulder(max(c.rgb, 0.0) + gTint.rgb * bloom), c.a);
}

// the film style of the linear colour x (0..64)
float3 Film(float3 x) {
    float peak = max(x.r, max(x.g, x.b));
    if (!(peak > 1e-6)) return 0.0;
    float3 ratio = x / peak;
    float z = pow(peak, gFilm.x);
    float y = min(z / (pow(z, gFilm.y) * gFilm.z + gFilm.w), 1.0);
    ratio = lerp(pow(ratio, gLook.x), 1.0, pow(y, gLook.y));
    float3 c = y * ratio;
    float v = sqrt(saturate(Luma(c)));
    float3 tone = lerp(1.0, gCool.rgb, gLook.z * (1.0 - v) * (1.0 - v)) * lerp(1.0, gWarm.rgb, gLook.z * v * v);
    return saturate(c * tone);
}

// as CSComposite, toned by the film style
[numthreads(8, 8, 1)]
void CSCompositeFilm(uint3 id : SV_DispatchThreadID) {
    float eye;
    float2 p;
    if (!Locate(id.xy, gAdd, eye, p)) return;
    float4 c = tSrc[id.xy];
    float3 bloom = Tent(tAdd, gAdd, eye, p) * gParam.w;
    uDst[id.xy] = float4(Film(min(max(c.rgb, 0.0) + gTint.rgb * bloom, 64.0)), c.a);
}

// 0.2.2: CSComposite and CSCompositeFilm in place, for a target that takes a UAV (OpenComposite's RR image): each
// texel read from u0 itself and written back there, so no texture of its own and no copy back over the target.
[numthreads(8, 8, 1)]
void CSCompositeInPlace(uint3 id : SV_DispatchThreadID) {
    float eye;
    float2 p;
    if (!Locate(id.xy, gAdd, eye, p)) return;
    float4 c = uDst[id.xy];
    float3 bloom = Tent(tAdd, gAdd, eye, p) * gParam.w;
    uDst[id.xy] = float4(Shoulder(max(c.rgb, 0.0) + gTint.rgb * bloom), c.a);
}

[numthreads(8, 8, 1)]
void CSCompositeFilmInPlace(uint3 id : SV_DispatchThreadID) {
    float eye;
    float2 p;
    if (!Locate(id.xy, gAdd, eye, p)) return;
    float4 c = uDst[id.xy];
    float3 bloom = Tent(tAdd, gAdd, eye, p) * gParam.w;
    uDst[id.xy] = float4(Film(min(max(c.rgb, 0.0) + gTint.rgb * bloom, 64.0)), c.a);
}
