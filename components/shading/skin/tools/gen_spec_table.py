"""VaM's skin pixel programs with their specular changed -> <repo>\\out\\gen\\skin\\spec_programs.h (and test cases for
tests\\spec_test.cpp in out\\gen\\skin\\spec_cases\\, and the BRDF alone in spec_cases\\spec_brdf.hlsl).

Every MARMO_LINEAR ForwardBase / ForwardAdd pixel program of Custom/Subsurface/GlossNMCull, GlossNMNoCull, GlossCull,
GlossNoCull and GlossNMTessMappedFixed (ComputeBuff or not: they share pixel bytecode) is lifted to HLSL instruction by
instruction (asm_lift.py) and compiled again with a second specular, chosen by a constant buffer at b12 (specCtl.x:
0 = the original program's result, 1 = skin GGX, 2 = test):

  VaM: one normalised Blinn-Phong lobe, n = 2^lerp(_Shininess, 1, (1 - gloss)^2), colour S = spec map * _SpecColor *
  a Fresnel curve on N.V (~0.19 facing for the test skin), no N.L, the specular normal bent 1.37x by the normal map.
  Skin GGX: two GGX lobes (perceptual roughness sqrt(alpha) * specLobe.y / .z, weights specLobe.w / 1 - specLobe.w;
  alpha^2 = 2 / (n + 2) from VaM's own n, so each skin's gloss map, _Shininess and _GlossOffset still set it), Smith
  height-correlated visibility, Schlick Fresnel on V.H from F0 = specCtl.w, N.L, white. The specular normal is the
  normal map sampled with a mip bias (specCtl.y), so single pores do not catch the light; the normal variance the bias
  removed widens the lobes (specCtl.z, Toksvig-like). The skin's spec map stays as cavity: its value over its own mean
  (the texture's last mip), times saturate(_SpecInt). The IBL keeps VaM's cube, with the mip of the GGX lobes and their
  directional albedo (Karis' fit) in place of S; its reflection direction is VaM's (about W) or, with eyeGeo.y, about the
  highlight's own normal (0.22.12 candidate).
  Candidate (eyeGeo.z, skin with the irradiance diffuse): each light's diffuse times Burley's 2012 grazing factor at the
  skin's own perceptual roughness - retroreflection where light and view are close, the diffuse Fresnel shadow where
  they are apart, 1 at normal incidence (SkinGrazing); the 0.20.0 translucency is still measured against the factor
  without it.

How it is put in: S (the instruction multiplying "spec map * _SpecColor" by the Fresnel scalar) becomes
cavity * directional albedo, white; the N.H power A ("dp3_sat A, N, H" -> log -> mul by n -> exp) becomes
pi * f * N.L / (min(10 N_d.L, 1) * 0.5 * (n + 2) / 2pi * directional albedo), so that VaM's own multiplications after
it (checked in every program: A * min(10 N_d.L, 1) * 0.5, then "mad A, S * (n + 2) / 2pi [* exposure], diffuse") give
pi * f * N.L * cavity times the light. The places are found by data flow in each program: the exponent ("add X, -Y,
l(8.0)" + "exp X, X"; Y = the IBL mip), the N.H power, the specular normal W (three dp3 of the TBN rows with VaM's
tangent normal; its normal-map sample and _SpecularBumpiness from that normal's slice), the diffuse N.L (the dp3_sat
feeding VaM's wrap "mad_sat ..., NdotL, 1 - S/2, S/2" with S = _SubdermisColor; L = its operand reaching
_WorldSpaceLightPos0), the view vector (camera position minus position) and S. A program where any of them is not
found exactly once, or where S or A is used anywhere else, stops the generator. Mode 2 (test only) runs VaM's own
specular through the vectors and normal this code rebuilds (normal-map sample at bias 0): it must give the original
result within rounding.

Teeth and tongue (0.8.0; their draws keep modes 0): on teeth (teethCtl.w = 1) VaM's diffuse factor F computed again
with their own Subsurface Color teethCtl.y in place of _SubdermisColor (VaM's teeth have 0, which makes diffuse 2x
where it faces the light; 1 gives 1x), times teethCtl.z; on both (teethCtl.x = 1) the program's result times the
mouth occlusion: a two-channel
texture over the main texture's UV (back of the mouth 0..1, gum end of a tooth 0..1; native\bake.cpp) at t16 / s15,
the transmission's slots (their draws get no transmission draw), darkens toward the back and toward the gums.

Eyes (0.9.0; their draws keep modes 0): the sclera, iris and pupil draws (pupils: Custom/Subsurface/Cull and NoCull,
lifted the same way) get, before their return, the cornea catchlight (eyeCtl.y) added and the eyelid occlusion
(eyeCtl.x) multiplied in. Both read the eye rig at t17, a Buffer<float4> the module fills once a frame per character
with the eye gather compute shader (kEyeGatherCS) from the skin's own vertex buffer: [0] / [2] each eye's centre
(xyz, world) and its scale, [1] / [3] its gaze and radius, then the eyelid contact points (xyz on the eyeball's sphere,
w = upper-lid share + 2 where a polyline starts), eye 0's eyeCtl.z of them, then eye 1's eyeCtl.w. The pixel goes
with the nearer eye (nothing farther than 1.3 radii from it: a vertex buffer not in world space draws as VaM's).
Occlusion: the pixel moved onto the eyeball's sphere, its distance d to the nearest point of each lid's polylines,
1 - s exp(-d^2 / w^2) per lid (upper and lower strength and width, widths in eyeball radii). Catchlight: one GGX lobe
(alpha eyeCatch.y, Schlick from F0 eyeCatch.z) on the cornea's normal - a sphere of eyeCatch.w radii through the
eyeball's front, inside the sclera's front ring (eyeGeo.x radii from the gaze axis), the eyeball's own sphere outside
it, blended across 4% of the radius - lit by the pass's light (the program's light colour times attenuation and
shadow, captured where VaM multiplies its result by it), so it follows each light and each eye's view.

Subsurface scattering (0.10.0): every replacement also writes three more targets, which only the skin draws of the eye
camera's opaque pass bind (native\sss.cpp; unbound targets drop the writes): SV_Target1 = the direct diffuse light
without the albedo, E = F * 2 * L (F the diffuse factor as the program ends up using it, L the light scale the lit
result is multiplied by: attenuation, shadow, _LightColor0), and in w the distance to the camera (|camera - position|,
from the view vector); SV_Target2 = the albedo (the factor F is multiplied by); SV_Target3 = SV_Position.z (which pixels
are still skin when the opaque pass ends). Found by data flow: the albedo product is F's one use; the diffuse operand of
"mad A, Sn, diffuse" is that product or "add X, P, P" of it (the 2); L is the other operand of the one mul reading
that mad's result. albedo * E is then exactly the light's diffuse in the colour (tests\sss_test.cpp checks it with
_LightColor0 on and off). A program where these are not found keeps one target and gets sss 0 in spec_programs.h.

SKIN DETAIL (0.11.0 / 0.12.0 / 0.13.0; b12 skinCtl / skinNorm, all 0 = 0.10.0; t18, t19): skin presets set VaM's gloss (_GlossOffset 0.1 .. 0.84,
_Shininess) for VaM's own lobe, so the GGX lobes made from n were up to 3x smoother on some presets than on the test
person the looks were picked on (a ~10x brighter peak, burnt out to white). skinCtl.x > 0: alpha^2 = skinCtl.x at the
skin's mean gloss (the gloss map's last mip with _GlossOffset, as VaM's n: specNMean), a pixel's own alpha^2 over that
one's kept (clamped skinCtl.y .. z). skinNorm.w != 0 (0.12.0) spreads a skin's own roughness instead: most presets'
gloss maps are near black (p50 0.01 .. 0.03) under a big _GlossOffset, so VaM's n hardly varies across the face (Feifei
alpha 0.133 .. 0.137). t18 holds this draw's gloss map's own p50 and 1 / (p90 - p50) (the module reads them from the
map's <= 256 px mip once per texture): alpha^2 = skinCtl.x at the map's p50, times 2^(skinNorm.w (t - p50) / (p90 -
p50)) (t = the raw gloss texel; -2 log2 R: R times smoother alpha at p90), clamped; until the stats are there (t18 .z
0) the 0.11.0 ratio. 0.13.0: skinNorm.w 1 = those from the draw's tier row at t19 (the skin's tier, face or body, set
per gloss map by the plugin): x alpha^2 at p50, y -2 log2 R, z / w the alpha^2 floor / ceiling; [1].x the gloss map's
mip bias, [1].y the highlight's gain (0 = 1), [1].z the least p90 - p50 the spread is divided by (the body rows: a nearly
flat gloss map's noise not stretched into glints). 0.18.0: [2].w 1 = the row is this gloss map's own (one per skin and
map, the plugin's continuous level): [2].x / .y its p50 / p90 - p50 over the skin texels only (the UV coverage of the
map's skin materials; the whole map's held padding, scalp and nails, so face and body maps were anchored at different
skin), [2].z the cavity's divisor over the same texels (3 x saturate(the spec map's mean there + _SpecOffset)), and
[1].y the gain as it is (0 = no highlight). The normal slope compute shader (kNormalSlopeCS) reads a face
normal map's pore-scale slope for the plugin's starting step. skinCtl.w: the cavity over the mean of the spec map with _SpecOffset, times
saturate(4 _SpecInt) (_SpecInt only fades highlights out below 0.25). Pores: VaM lights the ambient (both SH) with the
specular normal (bent by _SpecularBumpiness, 1 .. 2.2 on presets) and the diffuse with the full normal map, the cavity
with the full spec map (DLSS's mip bias sharpens all of them to the output pixel): skinNorm.x / .z > 0 sample the
normal map / spec map with gradients scaled so the footprint is at least that many metres (skinPix = metres per pixel
from the position's derivatives), the diffuse normal (_DiffuseBumpiness) replaced before its three dp3; skinNorm.y the
ambient on that diffuse normal: W written after the IBL cube sample (W then feeds only the SH and VaM's N.H, which mode
1 replaces). Found by data flow: the gloss sample = the one 2-D sample in the IBL mip's slice (with _GlossOffset and
_Shininess in it), the diffuse normal = the diffuse N.L's operand without the light position, TBN rows as the specular
normal's, its tangent normal from the same normal-map sample and used only by the three dp3. Programs without them
(the pupils' Cull / NoCull) keep 0.10.0's.

    python -I gen_spec_table.py   (VAMKIT_VAM, VAMKIT_FXC)
"""
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import asm_lift as A  # noqa: E402
import skin_shaders as S  # noqa: E402

FAMILIES = r"^Custom/Subsurface/(GlossNMCull|GlossNMNoCull|GlossCull|GlossNoCull|GlossNMTessMappedFixed|Cull|NoCull)(ComputeBuff)?$"
CTL_SLOT = 12
EIGHT = 0x41000000

BRDF = """cbuffer SpecCtl : register(b%d) {
    float4 specCtl;   // x mode (0 VaM, 1 skin GGX, 2 test), y normal-map mip bias, z pore widening, w F0
    float4 specLobe;  // x roughness scale, y sharp-lobe roughness factor, z wide-lobe factor, w sharp-lobe weight
    float4 scatCtl;   // x diffuse mode (0 VaM, 1 skin wrap, 2 test, 3 the irradiance, 0.19.0), y overall scattering 0..2
    float4 scatA;     // xyz per-channel wrap a: light reaches N.L = -a
    float4 scatP;     // xyz per-channel power p
    float4 teethCtl;  // x 1 = mouth occlusion (teeth, tongue), y Subsurface Color in VaM's diffuse factor, z diffuse
                      // gain, w 1 = the diffuse factor with y and z (teeth)
    float4 teethOcc;  // x darkening at the back of the mouth, y where it starts (0 front .. 1 back), z darkening at
                      // the gum end of a tooth, w its power
    float4 teethUV;   // xy scale, zw offset: the main texture's UV (with _MainTex_ST) -> the occlusion texture's UV
    float4 eyeCtl;    // x 1 = eyelid occlusion, y 1 = cornea catchlight, z / w eyelid contact points of eye 0 / eye 1
    float4 eyeOcc;    // x upper-lid darkening, y its width, z lower-lid darkening, w its width (widths in eyeball radii)
    float4 eyeCatch;  // x strength, y GGX alpha, z F0, w the cornea's radius in eyeball radii
    float4 eyeGeo;    // x the sclera's front ring (where the cornea ends) in eyeball radii from the gaze axis; y (skin) 1 =
                      // the IBL reflected about the highlight's normal (0 = about VaM's W); z (skin) 1 = the diffuse's
                      // grazing factor (SkinGrazing); w unused (eyes: no E1 on ray-traced draws, removed)
    float4 skinCtl;   // x the lobes' alpha^2 at the skin's mean gloss (0 = the skin's own n), y / z the clamp of a pixel's
                      // alpha^2 over the mean gloss's, w 1 = intensity: spec map (with _SpecOffset) over its own mean,
                      // times saturate(4 _SpecInt)
    float4 skinNorm;  // x diffuse normal filter width (metres; 0 = VaM's diffuse normal), y 1 = the ambient (SH) on the
                      // diffuse normal (VaM: the specular one), z cavity filter width (metres; 0 = VaM's spec map sample),
                      // w 1: each skin's tier row at t19 (0.13.0)
    float4 noseUV;    // main UV -> the nasal cavity texture
    float4 noseCtl;   // x baked AO; y amount; z Nostrils baseline override; w this character's Face IBL filter
    float4 mouthCtl; // x visibility amount, y tongue wet surface, z use dynamic geometry (0 legacy)
    float4 mouthUV;
    float4 bodyCtl;  // unused (0.19.0's body environment visibility, removed); kept for the layout
    float4 bodyUV;   // unused (its lookup map's UV)
    float4 shadowCtl;  // x 1 = spot / point shadow softness from the blocker distance (0.19.0), y the light's radius (m),
                       // z unused (0.19.0: the least radius in the skin's mesh edges, from the removed body
                       // environment's edge buffer; the least radius is now ShadowMinTexels), w tests only
    float4 shadowCtl2; // 0.19.1 (all 0 = 0.19.0): x the least radius in shadow map texels (0 = 1.5), y 1 = the filters'
                       // rotation from the skin's UV and 64 taps in the penumbra, z 1 = the 32-tap weighted blocker search
                       // (continuous depth, soft blocker test), w the rotation's cells per UV unit (0 = 192)
    float4 skinLight;  // 0.22.20 (all 0 = 0.22.19): x the vellus sheen's weight (SkinSheen; 0 on lips and all but skin),
                       // y 1 = each light's specular from its disc (SpecDirect: the lobes widened by the sine of shadowCtl.y)
};
Texture2D<uint4> mouthMap : register(t20);
Buffer<float> mouthAO : register(t21);
float MouthTexel(int2 p,uint2 size) {
    uint4 q=mouthMap.Load(int3(clamp(p,int2(0,0),int2(size)-1),0));
    if(q.x==0xffffffffu)return 1.0;
    float2 w=float2(q.w&65535u,q.w>>16)/65535.0;
    return dot(float3(w,max(0,1-w.x-w.y)),float3(mouthAO.Load(q.x),mouthAO.Load(q.y),mouthAO.Load(q.z)));
}
float MouthVisibility(float2 uv) {
    if(mouthCtl.z==0.0 || mouthCtl.x==0.0)return 1.0;
    uint w,h;mouthMap.GetDimensions(w,h);if(w==0||h==0)return 1.0;
    float2 p=(uv*mouthUV.xy+mouthUV.zw)*float2(w,h)-.5;int2 q=int2(floor(p));float2 f=frac(p);
    float v=lerp(lerp(MouthTexel(q,uint2(w,h)),MouthTexel(q+int2(1,0),uint2(w,h)),f.x),lerp(MouthTexel(q+int2(0,1),uint2(w,h)),MouthTexel(q+int2(1,1),uint2(w,h)),f.x),f.y);
    return saturate(v);
}
Texture2D<float2> teethTex : register(t16);  // teeth: back/gum; Nostrils: thickness/visibility
SamplerState teethSmp : register(s15);
// 0.19.0 direct light shadows on female skin (spot and point lights, VaM's hard shadows). The light is a disc of radius
// shadowCtl.y metres. What is right: (1) skin with nothing between it and the light is not darkened by its own shadow
// map; (2) the terminator is the skin's diffuse model's (VaM's factor with our wrap past N.L 0 = light that went in on
// the lit side and scattered out), the shadow map takes no part in it; (3) only other surfaces cast shadows: full dark
// inside, penumbra light radius x (receiver - blocker) / blocker; (4) nothing follows the mesh's triangles or the radius.
// VaM's skin programs break (1) and (3): they blur every shadow by one radius from the light's Shadow Strength (0.025 x
// strength in shadow map UV for a spot, 0.1 m x strength at the receiver for a point light), with every tap compared with
// the pixel's one depth - slanted lit skin then shades itself, more the larger the radius. Here VaM's loop is skipped and
// its sum replaced: 16 taps over VaM's radius find the blockers, radius = the penumbra, between the least radius and
// VaM's; then VaM's own 25-tap pattern and rotation at that radius, each tap compared with the receiver's plane from the
// vertex normal (one-sided: only where the plane is nearer the light, less an allowance for the mesh's facets, 0.3 x the
// tap's offset), the normal turned toward the light up to N.L 0.1 at least (~84 deg): the plane never turns away, both
// sides of the outline get the same, and past it the body's own front counts only where it stands well in front (about
// 10 x the tap's offset; there the diffuse model has faded). Tried and wrong: VaM's radius over vertex N.L 0.45 .. 0.15
// with VaM's compare (a hard line around the chin: the self-shading changed with the radius, SimXR 20261020-002709 /
// -004414), the receiver's plane from ddx / ddy (the mesh's triangles, -011727), the darker of VaM's and the penumbra
// filter (VaM's self-shading kept on lit skin, -012853), a disc-horizon factor with the pixel's one depth over vertex
// N.L sin a .. 0 (two drops in one narrow band: lines along every terminator, -014505). The least radius, shadowCtl.z metres at the receiver (1.5 texels at least):
// where a body part shades the skin a few centimetres away (the shoulder into the collarbone's hollow, the arm onto the
// chest) the shadow map's outline is the caster mesh's corners, a sharp penumbra draws them as a polygon (SimXR
// 20261020-000014, the hard view). Their size goes with the mesh's: the least radius is shadowCtl.z x the skin's mesh edge
// under the pixel (per vertex the mean edge of the welded triangles around it, interpolated: Face ~4 mm, Torso ~13,
// Shoulders ~15, Legs ~18), the face keeps its sharp contact shadows and the body loses the corners (SimXR
// 20261020-000759: one least radius for all, 6 mm left the shoulder's corners, 12 mm washed out the face;
// 20261020-001941: one per submesh drew its seams across a shadow). That mesh edge came from the body environment's
// edge buffer, removed with it: the least radius is ShadowMinTexels alone (as it was where that buffer was unbound).
// shadowCtl.w, tests only: 1 every radius the least (the shadow map's own edges), 2 the radius over VaM's as grey (light
// passes), 3 the penumbra from the blocker nearest the light instead of the blockers' mean
static const float SHADOW_GOLDEN = 2.39996323;
static const float2 SHADOW_TAPS[25] = {  // VaM's (its programs' icb)
    float2(-0.6351818, 0.2172711), float2(-0.1499606, 0.2320675), float2(-0.67978, 0.6884924), float2(-0.7758647, -0.253409),
    float2(-0.4731916, -0.2832723), float2(-0.3330079, 0.6430059), float2(-0.1384151, -0.09830225), float2(-0.8182327, -0.5645939),
    float2(-0.9198472, 0.06549802), float2(-0.1422085, -0.4872109), float2(-0.4980833, -0.5885599), float2(-0.3326159, -0.8496148),
    float2(0.3066736, -0.1401997), float2(0.1148317, 0.374455), float2(-0.0388568, 0.8071329), float2(0.4102885, 0.6960295),
    float2(0.5563877, 0.3375377), float2(-0.01786576, -0.8873765), float2(0.234991, -0.4558438), float2(0.6206775, -0.1551005),
    float2(0.6640642, -0.5691427), float2(0.7312726, 0.5830168), float2(0.8879707, 0.05715213), float2(0.3128296, -0.830803),
    float2(0.8689764, -0.3397973)};
static float gShadowR = 0.0, gShadowOurs = -1.0;  // the radius over VaM's; the sum that replaces VaM's (-1: VaM's own)
float ShadowDone(float r, float hi) { gShadowR = r / hi; return r; }
float ShadowNoise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
// 0.19.1: nothing in the filters changes with DLSS's jitter. VaM rotates its 25 taps per pixel by a hash of a noise
// texture read at the shadow map UV x 10 (spot) or at the screen position (point), 0.19.0's blocker search by a hash
// of the shadow map position: each frame's sub-pixel jitter gives every pixel a new rotation. DLSS averages that away
// only where its history holds, and VaM's people have no motion vectors of their own (the bridge sends rigid objects
// only): on a moving body every frame's noise and every jump of the penumbra radius reaches the eye, and the generated
// frames move it with the camera's vectors. Now: (1) one rotation for both, a smooth value noise over the skin's own
// UV (shadowCtl2.w cells per UV unit, 192: ~1.3 mm on the face, ~6 mm on the body), so it moves with the skin and a
// sub-pixel shift changes it a little, not at random; (2) a pixel in a penumbra (0 < its 25 taps < 25) takes 39 more,
// a golden-angle spiral (each tap 1/64 of the result: the steps of a fixed pattern stay under 2%%); (3) the blocker
// search reads the shadow map bilinearly and counts a blocker by how far it stands in front (a ramp over the tolerance),
// 32 taps weighted 1 - (r / R)^2, the blockers' share below 15%% of the weight taking the radius back toward the least:
// the radius is a continuous function of the position (with 16 equal point taps a thin blocker near the edge of the
// search was found by some pixels and missed by their neighbours, a broken fringe).
float ShadowMinTexels() { return shadowCtl2.x > 0.0 ? shadowCtl2.x : 1.5; }
int ShadowSearchTaps() { return shadowCtl2.z != 0.0 ? 32 : 16; }
float ShadowSearchWeight(float q2) { return shadowCtl2.z != 0.0 ? 1.0 - q2 : 1.0; }
float ShadowBlocker(float zb, float edge, float tol) { return shadowCtl2.z != 0.0 ? saturate((edge - zb) / tol) : (zb < edge ? 1.0 : 0.0); }
float ShadowRadiusFrom(float cnt, float wall, float rb, float lo) {
    return shadowCtl2.z != 0.0 ? lerp(lo, rb, saturate(cnt / (0.15 * wall))) : rb;
}
bool ShadowStable() { return shadowCtl2.y != 0.0; }
float ShadowHash(float2 i) {
    uint2 q = asuint(int2(i));
    uint h = (q.x * 1597334677u) ^ (q.y * 3812015801u);
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return float(h & 0xffffffu) * (1.0 / 16777216.0);
}
float ShadowSkinAngle(float2 uv) {  // radians
    float2 p = uv * (shadowCtl2.w > 0.0 ? shadowCtl2.w : 192.0), i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(ShadowHash(i), ShadowHash(i + float2(1, 0)), f.x), lerp(ShadowHash(i + float2(0, 1)), ShadowHash(i + float2(1, 1)), f.x), f.y) * 6.28318531;
}
static const int SHADOW_MORE = 39;  // the penumbra's extra taps
// c: shadow map UV and NDC depth, w: the receiver's clip w (metres along the light's axis), m0..m3: unity_WorldToShadow,
// n: the vertex normal, toL: light - position, (sn, cs): VaM's tap rotation; returns VaM's radius (its loop is then skipped),
// the sum that replaces its loop's in gShadowOurs
float ShadowSpotRadius(Texture2D sm, SamplerComparisonState sc, float3 c, float w, float vamR, float4 m0, float4 m1, float4 m2, float4 m3,
                       float3 n, float3 toL, float sn, float cs, float2 uv) {
    gShadowOurs = -1.0;
    if (shadowCtl.x == 0.0 || !(w > 0.0)) return vamR;
    uint sw, sh;
    sm.GetDimensions(sw, sh);
    if (sw == 0 || sh == 0) return vamR;
    float k = length(float3(m0.x, m1.x, m2.x)), um = w / k;  // shadow map UV per metre at w = 1; metres per UV here
    float lo = ShadowMinTexels() / sw, hi = max(vamR, lo), r = lo;
    float3 L = toL / max(length(toL), 1e-6);
    n = normalize(n);
    n = normalize(n + L * max(0.1 - dot(n, L), 0.0));  // never past ~84 deg from the light
    float4 r2 = float4(m0.z, m1.z, m2.z, m3.z), r3 = float4(m0.w, m1.w, m2.w, m3.w);
    float a = dot(r2.xyz, r3.xyz) / max(dot(r3.xyz, r3.xyz), 1e-12), b = r2.w - a * r3.w;  // NDC depth = a + b / w
    // the receiver's plane from the vertex normal: w over the shadow map UV (up to ~84 deg from facing the light)
    float3 t1 = normalize(cross(n, abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0))), t2 = cross(n, t1);
    float4 d1 = m0 * t1.x + m1 * t1.y + m2 * t1.z, d2 = m0 * t2.x + m1 * t2.y + m2 * t2.z;
    float2 j1 = (d1.xy - c.xy * d1.w) / w, j2 = (d2.xy - c.xy * d2.w) / w;  // UV per metre along t1, t2
    float det = j1.x * j2.y - j2.x * j1.y;
    float2 g = abs(det) > 1e-12 ? float2(d1.w * j2.y - d2.w * j1.y, d2.w * j1.x - d1.w * j2.x) / det : float2(0, 0);
    float gl = length(g), gmax = 10.0 * um;
    if (gl > gmax) g *= gmax / gl;
    float th = ShadowStable() ? ShadowSkinAngle(uv) : 0.0;
    if (ShadowStable()) sn = sin(th), cs = cos(th);
    if (shadowCtl.w != 1.0) {
        float tol = 0.002 * w + 0.003, sum = 0, cnt = 0, wall = 0, zmin = 1e20;
        float rot = ShadowStable() ? th + 1.0 : ShadowNoise(c.xy * float2(sw, sh)) * 6.28318531;
        int ns = ShadowSearchTaps();
        [loop] for (int i = 0; i < ns; i++) {
            float t = i * SHADOW_GOLDEN + rot, q2 = (i + 0.5) / ns, wt = ShadowSearchWeight(q2);
            float2 o = float2(cos(t), sin(t)) * (sqrt(q2) * hi);
            int2 q = clamp(int2(floor((c.xy + o) * float2(sw, sh))), int2(0, 0), int2(sw, sh) - 1);
            float den = (shadowCtl2.z != 0.0 ? sm.SampleLevel(teethSmp, c.xy + o, 0).x : sm.Load(int3(q, 0)).x) - a;
            float zb = abs(den) > 1e-24 ? b / den : 1e20;
            float bt = zb > 0.0 ? ShadowBlocker(zb, w + min(dot(g, o), 0.0) - tol, tol) : 0.0;
            wall += wt;
            if (bt > 0.0) { sum += zb * wt * bt; cnt += wt * bt; zmin = min(zmin, zb); }
        }
        if (cnt > 0.0) {
            float zb = shadowCtl.w == 3.0 ? zmin : sum / cnt;
            r = ShadowRadiusFrom(cnt, wall, clamp(shadowCtl.y * (w - zb) / zb * k / w, lo, hi), lo);
        }
    }
    float s = 0.0;
    [unroll] for (int i = 0; i < 25; i++) {
        float2 t = SHADOW_TAPS[i], o = float2(t.y * sn + cs * t.x, t.x * (-sn) + cs * t.y) * r;
        float wr = max(w + min(dot(g, o), 0.0) - 0.3 * length(o) * um, 0.5 * w);
        s += sm.SampleCmpLevelZero(sc, c.xy + o, c.z + b * (1.0 / wr - 1.0 / w)).x;
    }
    if (ShadowStable() && s > 0.0 && s < 25.0) {  // a penumbra: 39 more taps
        float s2 = 0.0;
        [loop] for (int i = 0; i < SHADOW_MORE; i++) {
            float t = th + 0.5 + i * SHADOW_GOLDEN;
            float2 o = float2(cos(t), sin(t)) * (sqrt((i + 0.5) / SHADOW_MORE) * r);
            float wr = max(w + min(dot(g, o), 0.0) - 0.3 * length(o) * um, 0.5 * w);
            s2 += sm.SampleCmpLevelZero(sc, c.xy + o, c.z + b * (1.0 / wr - 1.0 / w)).x;
        }
        s = (s + s2) * (25.0 / (25.0 + SHADOW_MORE));
    }
    gShadowOurs = s;
    ShadowDone(r, hi);
    return vamR;
}
// v: position - light (world); proj: _LightProjectionParams (stored depth = 1 - (proj.y / z - proj.x), z the distance
// along the cube face's axis); n: the vertex normal; f1, f2: VaM's tap frame (the pattern's x, y); radii in metres at
// the receiver, as VaM's offsets
float ShadowPointRadius(TextureCube sm, SamplerComparisonState sc, float3 v, float vamR, float4 proj, float3 n, float3 f1, float3 f2,
                        float2 uv) {
    gShadowOurs = -1.0;
    if (shadowCtl.x == 0.0) return vamR;
    uint sw, sh, mips;
    sm.GetDimensions(0, sw, sh, mips);
    float3 av = abs(v);
    float zr = max(av.x, max(av.y, av.z)), dist = length(v);
    if (sw == 0 || !(zr > 0.0)) return vamR;
    float lo = 2.0 * ShadowMinTexels() * zr / sw, hi = max(vamR, lo), r = lo;  // a face spans 2 z
    n = normalize(n);
    n = normalize(n - v / dist * max(0.1 + dot(n, v) / dist, 0.0));  // never past ~84 deg from the light
    float nv = dot(n, v);
    float3 l = v / dist, tx = normalize(cross(abs(l.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), l)), ty = cross(l, tx);
    float th = ShadowStable() ? ShadowSkinAngle(uv) : 0.0;
    if (ShadowStable()) f1 = tx * cos(th) + ty * sin(th), f2 = ty * cos(th) - tx * sin(th);  // VaM's frame: screen noise
    if (shadowCtl.w != 1.0) {
        float tol = 0.002 * zr + 0.003, sum = 0, cnt = 0, wall = 0, zmin = 1e20;
        float rot = ShadowStable() ? th + 1.0 : ShadowNoise(float2(dot(v, float3(1.0, 0.71, 0.29)), dot(v, float3(0.23, 1.0, 0.61))) * 2000.0) * 6.28318531;
        int ns = ShadowSearchTaps();
        [loop] for (int i = 0; i < ns; i++) {
            float t = i * SHADOW_GOLDEN + rot, q2 = (i + 0.5) / ns, wt = ShadowSearchWeight(q2);
            float3 o = (tx * cos(t) + ty * sin(t)) * (sqrt(q2) * hi), q = v + o, aq = abs(q);
            float zq = max(aq.x, max(aq.y, aq.z)), dq = dot(n, q), least = max(1.0 - 10.0 * length(o) / length(q), 0.5);
            float k = dq * nv > 0.0 ? clamp(nv / dq, least, 1.0) : least;  // the receiver's plane along this tap's ray, nearer only
            float zb = proj.y / max(1.0 - sm.SampleLevel(teethSmp, q, 0).x + proj.x, 1e-6);
            float bt = ShadowBlocker(zb, k * zq - tol, tol);
            wall += wt;
            if (bt > 0.0) { sum += zb * zr / zq * wt * bt; cnt += wt * bt; zmin = min(zmin, zb * zr / zq); }
        }
        if (cnt > 0.0) {
            float zb = shadowCtl.w == 3.0 ? zmin : sum / cnt;
            r = ShadowRadiusFrom(cnt, wall, clamp(shadowCtl.y * (zr - zb) / zb, lo, hi), lo);
        }
    }
    float s = 0.0, bias = max(proj.z, 0.005);
    [unroll] for (int i = 0; i < 25; i++) {
        float3 o = f2 * (SHADOW_TAPS[i].y * r) + f1 * (SHADOW_TAPS[i].x * r), q = v + o;
        float dq = dot(n, q), lq = length(q), least = max(1.0 - 10.0 * length(o) / lq, 0.5);
        float k = dq * nv > 0.0 ? clamp(nv / dq, least, 1.0) : least;
        q *= k - 0.3 * length(o) / lq;
        float3 aq = abs(q), m = max(aq - rsqrt(dot(aq, aq)) * aq * proj.z, 0.0);
        float z = max(max(m.y, m.x), m.z) - bias;
        s += sm.SampleCmpLevelZero(sc, q, 1.0 - (proj.y / max(z, 0.0) - proj.x)).x;
    }
    if (ShadowStable() && s > 0.0 && s < 25.0) {  // a penumbra: 39 more taps
        float s2 = 0.0;
        [loop] for (int i = 0; i < SHADOW_MORE; i++) {
            float t = 0.5 + i * SHADOW_GOLDEN, rr = sqrt((i + 0.5) / SHADOW_MORE) * r;
            float3 o = (f1 * cos(t) + f2 * sin(t)) * rr, q = v + o;
            float dq = dot(n, q), lq = length(q), least = max(1.0 - 10.0 * length(o) / lq, 0.5);
            float k = dq * nv > 0.0 ? clamp(nv / dq, least, 1.0) : least;
            q *= k - 0.3 * length(o) / lq;
            float3 aq = abs(q), m = max(aq - rsqrt(dot(aq, aq)) * aq * proj.z, 0.0);
            float z = max(max(m.y, m.x), m.z) - bias;
            s2 += sm.SampleCmpLevelZero(sc, q, 1.0 - (proj.y / max(z, 0.0) - proj.x)).x;
        }
        s = (s + s2) * (25.0 / (25.0 + SHADOW_MORE));
    }
    gShadowOurs = s;
    ShadowDone(r, hi);
    return vamR;
}
Buffer<float4> eyePts : register(t17);  // the eye rig this frame (see the module's eye gather)
Buffer<float4> skinGloss : register(t18);  // this draw's gloss map: x its p50, y p90 - p50, z 1 = there
Buffer<float4> skinTier : register(t19);   // its tier row: (alpha^2 at p50, -2 log2 R, alpha^2 floor, ceiling),
                                           // (gloss map mip bias, highlight gain (0 = 1), least spread, 0),
                                           // 0.18.0 (skin texels' p50, p90 - p50, cavity divisor, 1 = there)
static const float SPEC_PI_EYE = 3.14159265;
// what light reaches the eyeball at P under the eyelids: the polylines of eye points [first, first + n)
float EyeOcclusion(float3 P, float3 C, float R, uint first, uint n) {
    float3 q = C + R * normalize(P - C);
    float wu = eyeOcc.y * R, wl = eyeOcc.w * R;
    float du = 0.0, dl = 0.0;
    float4 a = eyePts.Load(first);
    for (uint k = 1; k < n; k++) {
        float4 b = eyePts.Load(first + k);
        if (b.w < 2.0) {
            float3 ab = b.xyz - a.xyz;
            float t = saturate(dot(q - a.xyz, ab) / max(dot(ab, ab), 1e-20));
            float3 d = q - (a.xyz + t * ab);
            float d2 = dot(d, d);
            float u = lerp(a.w >= 2.0 ? a.w - 2.0 : a.w, b.w, t);
            du = max(du, u * exp(-d2 / max(wu * wu, 1e-20)));
            dl = max(dl, (1.0 - u) * exp(-d2 / max(wl * wl, 1e-20)));
        }
        a = b;
    }
    return (1.0 - eyeOcc.x * du) * (1.0 - eyeOcc.z * dl);
}
// the surface the catchlight is on: the cornea inside the sclera's front ring, the eyeball outside it
float3 EyeCatchNormal(float3 P, float3 C, float3 g, float R) {
    float3 q = P - C;
    float3 lat = q - g * dot(q, g);
    float k = smoothstep(eyeGeo.x - 0.04, eyeGeo.x + 0.04, length(lat) / R);
    float3 nc = normalize(P - (C + g * (R - eyeCatch.w * R)));
    return normalize(lerp(nc, normalize(q), k));
}
// pi * f * N.L of the catchlight's lobe (one GGX lobe, Smith height-correlated, Schlick)
float EyeCatch(float3 n, float3 l, float3 v) {
    float3 hv = l + v;
    float hh = dot(hv, hv);
    if (!(hh > 1e-12)) return 0.0;
    float3 h = hv * rsqrt(hh);
    float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-4), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float a2 = eyeCatch.y * eyeCatch.y;
    float d = (nh * a2 - nh) * nh + 1.0;
    float D = a2 / (SPEC_PI_EYE * d * d);
    float vis = 0.5 / max(nv * sqrt(nl * nl * (1.0 - a2) + a2) + nl * sqrt(nv * nv * (1.0 - a2) + a2), 1e-6);
    float x = 1.0 - vh, x2 = x * x;
    return eyeCatch.x * SPEC_PI_EYE * D * vis * (eyeCatch.z + (1.0 - eyeCatch.z) * x2 * x2 * x) * nl;
}
// what light reaches inside the mouth: less toward the back and toward the gums
float TeethOcclusion(float2 uvMain) {
    float2 bg = teethTex.SampleLevel(teethSmp, uvMain * teethUV.xy + teethUV.zw, 0);
    return (1.0 - teethOcc.x * smoothstep(teethOcc.y, 1.0, saturate(bg.x))) * (1.0 - teethOcc.z * pow(saturate(bg.y), teethOcc.w));
}
float NoseVisibility(float2 uvMain) {
    return noseCtl.x != 0.0 ? lerp(1.0, saturate(teethTex.SampleLevel(teethSmp, uvMain * noseUV.xy + noseUV.zw, 0).y), noseCtl.y) : 1.0;
}
// skin diffuse per channel, 1 facing the light: ((N.L + a) / (1 + a))^p, smooth down to 0 at N.L = -a
float3 ScatterWrap(float x) {
    return pow(saturate((x + scatA.xyz) / (1.0 + scatA.xyz)), scatP.xyz);
}
// what VaM's own diffuse factor (any Subsurface Color; flat below N.L 0) is multiplied by: each channel's wrap over the
// neutral (green) wrap at VaM's saturated N.L - 1 at N.L 0.5 and 1, red above and blue below near the terminator,
// past it down to 0 at N.L = -a instead of VaM's flat floor
// 0.19.0 female skin with the blocker-distance shadows: the diffuse factor is the irradiance from the light, a disc of
// shadowCtl.y metres (the shadows' light): N.L where the whole disc is above the horizon, (N.L + s)^2 / 4s across it
// (s = the disc's angular radius, sine), 0 below; times VaM's own facing factor (1 - Subsurface Color / 2). No light past
// the horizon from N.L: the light under the skin past the terminator and at shadow edges is the subsurface pass's
// (d'Eon's skin profile in millimetres over this shadowed irradiance). VaM's own factor keeps 1/4 of (1 - S/2) S/2 on
// every side turned away and our wrap continued it past N.L 0: both light skin no light reaches and only a 3 cm blurred
// shadow hid that; a sharper shadow cut it into lines (SimXR 20261020-002709 .. -014505).
float SkinLightSine(float4 lp, float3 P) {
    return lp.w == 0.0 ? 0.0 : saturate(shadowCtl.y / max(length(lp.xyz - P), 1e-3));
}
float SkinIrradiance(float x, float s) {
    return s <= 1e-4 ? saturate(x) : x >= s ? x : x <= -s ? 0.0 : (x + s) * (x + s) / (4.0 * s);
}
// candidate (eyeGeo.z): Burley's 2012 diffuse factor over Lambert, fitted to MERL - per side 1 + (F90 - 1)(1 - cos)^5,
// F90 = 0.5 + 2 r (L.H)^2: light and view close (L.H 1) brighten the grazing edge (retroreflection, up to 0.5 + 2r), far
// apart (L.H 0) darken it (diffuse Fresnel, 0.5); 1 at normal incidence. r = the skin's perceptual roughness, the lobes'
// alpha^2 blended as for the IBL; n the diffuse normal (VaM's N.L operand)
float SkinGrazing(float3 n, float3 l, float3 v, float2 a2) {
    n = normalize(n);
    l = normalize(l);
    float3 hv = l + v;
    float hh = dot(hv, hv);
    float lh = hh > 1e-12 ? dot(l, hv) * rsqrt(hh) : 0.0;
    float r = sqrt(sqrt(max(lerp(a2.y, a2.x, specLobe.w), 1e-8)));
    float f90 = 0.5 + 2.0 * r * lh * lh;
    float ql = 1.0 - saturate(dot(n, l)), qv = 1.0 - saturate(dot(n, v));
    float ql2 = ql * ql, qv2 = qv * qv;
    return (1.0 + (f90 - 1.0) * ql2 * ql2 * ql) * (1.0 + (f90 - 1.0) * qv2 * qv2 * qv);
}
float3 ScatterTint(float x) {
    float n = pow((saturate(x) + scatA.y) / (1.0 + scatA.y), scatP.y);
    return ScatterWrap(x) / max(n, 1e-6);
}
static const float SPEC_PI = 3.14159265;
// Artistic full-skin translucency approximation, independent of whole-body thickness.
// Blend toward a broad, near-neutral directional diffuse response. At N.L=1 the value is
// exactly the existing facing-light factor; at N.L=-1 it is zero. The original
// light attenuation and shadow multiply it afterwards, before the existing SSS.
// It is not a thickness-based transmission model and is not a physical percentage.
Texture2D<float> regionalTex : register(t22);
float RegionalResponse(float2 uv) {
    if(scatCtl.z == 0.0) return 1.0;
    float w=clamp(regionalTex.SampleLevel(teethSmp,uv*teethUV.xy+teethUV.zw,0),.5,1.08);
    return scatCtl.z == 2.0 ? w : lerp(1.0,w,saturate(scatCtl.y));
}
float3 OverallTranslucency(float3 current, float x, float3 subdermis, float regional) {
    float amount = clamp(scatCtl.y * regional, 0.0, 2.0);
    float3 a = float3(0.75, 0.50, 0.35);
    // 0..1 retains the reviewed blend; 1..2 widens the angular brightness
    // response without extrapolating RGB (which could go negative or tint it).
    float3 profile = pow(saturate((x + a) / (1.0 + a)), float3(1.3, 1.4, 1.5) / max(1.0, amount));
    float3 facing = (1.0 - 0.5 * subdermis) * saturate(0.5 * subdermis + (1.0 - 0.5 * subdermis));
    // Keep the reviewed angular brightness, but replace its orange/red tint.
    // Only the added scattering profile is neutralized; skin/albedo, lighting
    // and the facing-light material response retain their original colour.
    // Warm-gray ceiling: R:G:B = 1.02:1:0.98, independent of strength, fading
    // to neutral at N.L=1. Luminance normalization avoids a brightness change.
    float3 luma = float3(0.2126, 0.7152, 0.0722);
    float3 tint = lerp(float3(1.0, 1.0, 1.0), float3(1.02, 1.0, 0.98), 1.0 - saturate(x));
    float3 basis = facing * tint;
    float brightness = dot(facing * profile, luma);
    float3 target = basis * (brightness / max(dot(basis, luma), 1e-6));
    return lerp(current, target, saturate(amount));
}
// 0.20.0 candidate, female skin with the blocker-distance shadows (scatCtl.x 3): the translucency the overall layer gave
// (its angular response at [Diffuse] OverallScattering = scatCtl.w), as the part above the shadowed irradiance only:
// that excess times the light's attenuation and sharp shadow goes to its own target (SV_Target4), the subsurface pass
// blurs it over centimetres and adds it. The irradiance keeps the sharp shadows; the translucent glow gets soft edges, so
// where it meets a shadow (the neck under the chin) it fades instead of being cut into a line. scatA.w: its colour, 0 the
// reviewed warm gray, 1 the response's own per-channel colour (red reaches farthest past the terminator).
float3 TranslucencyExcess(float3 current, float x, float3 subdermis) {
    if (scatCtl.x != 3.0 || !(scatCtl.w > 0.0)) return 0.0;
    float amount = min(scatCtl.w, 2.0);
    float3 a = float3(0.75, 0.50, 0.35);
    float3 profile = pow(saturate((x + a) / (1.0 + a)), float3(1.3, 1.4, 1.5) / max(1.0, amount));
    float3 facing = 1.0 - 0.5 * subdermis;
    float3 luma = float3(0.2126, 0.7152, 0.0722);
    float3 basis = facing * lerp(float3(1.0, 1.0, 1.0), float3(1.02, 1.0, 0.98), 1.0 - saturate(x));
    float brightness = dot(facing * profile, luma);
    float3 gray = basis * (brightness / max(dot(basis, luma), 1e-6));
    float3 target = lerp(gray, facing * profile, saturate(scatA.w));
    return saturate(amount) * max(target - current, 0.0);
}
// VaM's tangent-space specular normal from a normal-map texel: Unity's UnpackNormalmapRGorAG, bent by _SpecularBumpiness
float3 SpecTangentNormal(float4 t, float bump) {
    float2 xy = float2(t.x * t.w, t.y) * 2.0 - 1.0;
    float3 n = normalize(float3(xy, sqrt(1.0 - min(dot(xy, xy), 1.0))));
    return normalize(bump * (n - float3(0.0, 0.0, 1.0)) + float3(0.0, 0.0, 1.0));
}
// GGX alpha^2 of the two lobes: VaM's Blinn-Phong n as Beckmann-equivalent alpha^2 = 2 / (n + 2), plus the normal
// variance the mip bias took out, then as perceptual roughness sqrt(alpha) scaled per lobe
float2 SpecLobeA2B(float a2, float variance) {
    float r = sqrt(sqrt(min(a2 + variance, 1.0))) * specLobe.x;
    float2 rr = clamp(r * specLobe.yz, 0.2, 1.0);
    rr *= rr;
    return rr * rr;
}
float2 SpecLobeA2(float n, float variance) {
    return SpecLobeA2B(2.0 / (n + 2.0), variance);
}
// the pixel's alpha^2 before the lobes: VaM's n as 2 / (n + 2); with skinCtl.x the same skin roughness for every skin
// preset: skinCtl.x at the skin's mean gloss (nMean = VaM's n there), the gloss map's own variation kept (clamped to
// skinCtl.y .. z times); with skinNorm.w and a tier row (ta, tb) the row's alpha^2 at the map's p50 (stats in t18; the
// row's own over the skin texels, tc, 0.18.0) and its texels (t, raw) spread by their own p90 - p50 (at least the row's
// tb.z), clamped to the row's floor .. ceiling
float SpecBaseA2(float n, float nMean, float t, float4 ta, float4 tb, float4 tc) {
    float a2 = 2.0 / (n + 2.0);
    if (skinCtl.x > 0.0) {
        float4 gs = tc.w > 0.0 ? float4(tc.xy, 1.0, 0.0) : skinGloss.Load(0);
        float r = a2 * (nMean + 2.0) * 0.5;  // over the mean gloss's (0.11.0)
        if (skinNorm.w != 0.0 && ta.x > 0.0)
            a2 = clamp(ta.x * (gs.z > 0.0 ? exp2(ta.y * (t - gs.x) / max(gs.y, max(tb.z, 0.01))) : r), ta.z, ta.w);
        else
            a2 = skinCtl.x * clamp(r, skinCtl.y, skinCtl.z);
    }
    return a2;
}
// the filtered sample's gradient scale: the sample's footprint at least w metres (pix = metres per pixel)
float SkinFilter(float w, float pix) {
    return max(1.0, w / max(pix, 1e-7));
}
float SpecD(float a2, float nh) {
    float d = (nh * a2 - nh) * nh + 1.0;
    return a2 / (SPEC_PI * d * d);
}
// height-correlated Smith G / (4 N.L N.V)
float SpecVis(float a2, float nl, float nv) {
    float l = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    float v = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    return 0.5 / max(l + v, 1e-6);
}
float SpecF(float vh) {
    float x = 1.0 - vh, x2 = x * x;
    return specCtl.w + (1.0 - specCtl.w) * x2 * x2 * x;
}
// pi * f * N.L of the two lobes (Unity's convention, in which Lambert diffuse is albedo * N.L, as VaM's is facing a light)
// 0.22.20 (skinLight.y): the light is the disc the diffuse and the shadows use (s = the sine of its angular radius,
// SkinLightSine), its highlight the lobes convolved with it - each lobe's alpha^2 widened as Unreal's sphere lights
// (Karis 2013; a2 + s (3 sqrt(a2) + s) / 4 V.H), the normalised lobe keeping its energy. s 0 (directional lights,
// shadowCtl.y 0) = the point light of 0.22.19.
float SpecDirect(float2 a2, float3 n, float3 l, float3 v, float s) {
    float3 hv = l + v;
    float hh = dot(hv, hv);
    if (!(hh > 1e-12)) return 0.0;
    float3 h = hv * rsqrt(hh);
    float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-4), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    if (skinLight.y != 0.0 && s > 0.0) a2 = min(a2 + 0.25 * s * (3.0 * sqrt(a2) + s) / (vh + 0.001), 1.0);
    float lobes = specLobe.w * SpecD(a2.x, nh) * SpecVis(a2.x, nl, nv) + (1.0 - specLobe.w) * SpecD(a2.y, nh) * SpecVis(a2.y, nl, nv);
    return SPEC_PI * lobes * SpecF(vh) * nl;
}
// 0.22.20 (skinLight.x): the skin's vellus hair as a sheen lobe - Estevez & Kulla's 2017 "Charlie" distribution
// (2 + 1/a) sin(theta_h)^(1/a) / 2pi, a = SKIN_SHEEN_A, with Neubelt's visibility 1 / 4 (N.L + N.V - N.L N.V). Nothing
// where the skin faces the light and the eye; a soft glow where the half vector grazes it - the rim of skin lit from
// behind or the side. White, times the weight; pi * f * N.L as SpecDirect (at most weight / 2 of the light).
static const float SKIN_SHEEN_A = 0.5;
float SkinSheen(float3 n, float3 l, float3 v) {
    float3 hv = l + v;
    float hh = dot(hv, hv);
    if (!(hh > 1e-12)) return 0.0;
    float nh = saturate(dot(n, hv * rsqrt(hh))), nl = saturate(dot(n, l)), nv = saturate(dot(n, v));
    float d = (2.0 + 1.0 / SKIN_SHEEN_A) * pow(max(1.0 - nh * nh, 0.0), 0.5 / SKIN_SHEEN_A) * (0.5 / SPEC_PI);
    return skinLight.x * SPEC_PI * d * nl / (4.0 * max(nl + nv - nl * nv, 1e-4));
}
// directional albedo of one GGX lobe for F0 = specCtl.w (Karis' fit of the split-sum table; r = perceptual roughness)
float SpecEnv1(float r, float nv) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 q = r * c0 + c1;
    float a004 = min(q.x * q.x, exp2(-9.28 * nv)) * q.x + q.y;
    float2 ab = float2(-1.04, 1.04) * a004 + q.zw;
    return specCtl.w * ab.x + ab.y;
}
float SpecEnv(float2 a2, float nv) {
    float2 r = sqrt(sqrt(a2));
    return specLobe.w * SpecEnv1(r.x, nv) + (1.0 - specLobe.w) * SpecEnv1(r.y, nv);
}
// 0.22.6: LDR look adaptation, driven by arriving light, never the BRDF peak,
// albedo or the selected gloss level. A strong source in a dark scene keeps its
// reflection; weak sources fade smoothly without changing roughness or hue.
float SkinSpecLowLight(float3 incident) {
    return smoothstep(0.0, 0.06, dot(max(incident, 0.0), float3(0.2126, 0.7152, 0.0722)));
}
// Skyshop's cube mip for the lobes: VaM samples mip 8 - log2 n for Blinn-Phong n; alpha^2 as n = 2 / alpha^2 - 2
float SpecMip(float2 a2) {
    float2 m = 8.0 - log2(max(2.0 / a2 - 2.0, 1.0));
    return specLobe.w * m.x + (1.0 - specLobe.w) * m.y;
}
""" % CTL_SLOT
HEADER = BRDF
SSS_HEADER = """struct SssOut {
    float4 c : SV_Target0;
    float4 e : SV_Target1;  // direct diffuse light without the albedo; w distance to the camera
    float4 a : SV_Target2;  // albedo
    float z : SV_Target3;   // SV_Position.z
    float4 t : SV_Target4;  // 0.20.0: the translucency (TranslucencyExcess times the light), without the albedo; w distance
};
SssOut SssOutput(float4 c, float3 e, float3 t, float3 albedo, float dist, float z, float regional) {
    SssOut o;
    o.c = c;
    o.e = float4(e, dist);
    o.a = float4(albedo, regional);
    o.z = z;
    o.t = float4(t, dist);
    return o;
}
"""


class Fail(Exception):
    pass


def writes(ins):
    """[(reg, comp)] the instruction writes"""
    out = []
    dests = ins.ops[:2] if ins.op == "sincos" else ins.ops[:1]
    if ins.op in ("if_nz", "if_z", "else", "endif", "loop", "endloop", "breakc_nz", "breakc_z", "break", "ret", "discard_nz", "discard_z"):
        return out
    for d in dests:
        if d.kind == "r":
            out.extend((d.index, c) for c in d.mask)
    return out


def last_def(p, k, reg, comp):
    for j in range(k - 1, -1, -1):
        if (reg, comp) in writes(p.instrs[j]):
            return j
    return None


def sources(ins):
    if ins.op in ("discard_nz", "discard_z"):  # its one operand is read (clothing programs)
        return ins.ops[:1]
    return ins.ops[2:] if ins.op == "sincos" else ins.ops[1:]


def src_comps(ins, op_index, comps):
    """components of source operand op_index that dest components comps read"""
    o = ins.ops[op_index]
    if ins.op in ("dp2", "dp3", "dp4"):
        comps = list(range(int(ins.op[2])))
    if ins.op in ("sample", "sample_l", "sample_c_lz"):
        comps = [0, 1, 2] if op_index == 1 else [0]
    return sorted({"xyzw".index(o.sel[c]) for c in comps}) if o.kind in ("r", "v", "cb", "icb") else []


def reaches(p, k, reg, comps, pred, depth=12, seen=None):
    """does register reg (components comps, as read at instruction k) depend on an operand for which pred is true"""
    seen = seen if seen is not None else set()
    if depth == 0:
        return False
    for c in comps:
        j = last_def(p, k, reg, c)
        if j is None or (j, c) in seen:
            continue
        seen.add((j, c))
        ins = p.instrs[j]
        dcomps = [c]
        for i, o in enumerate(ins.ops):
            if i == 0 or (ins.op == "sincos" and i == 1):
                continue
            if pred(o):
                return True
            if o.kind == "r" and reaches(p, j, o.index, src_comps(ins, i, [dcomps[0]] if ins.op not in ("dp2", "dp3", "dp4") else dcomps), pred, depth - 1, seen):
                return True
    return False


def is_lit(o, bits):
    return o.kind == "l" and all(b == bits for b in o.lit)


def scalar_reg(o):
    """(reg, comp) of a register operand read as one component (selects one letter), else None"""
    if o.kind != "r" or len(set(o.sel)) != 1:
        return None
    return o.index, "xyzw".index(o.sel[0])


def find_anchors(p, meta):
    ins = p.instrs
    # 1. exponent: add X, -Y, l(8.0) ; exp X, X
    exp_at = []
    for k in range(len(ins) - 1):
        a, b = ins[k], ins[k + 1]
        if (a.op == "add" and not a.sat and a.ops[0].kind == "r" and len(a.ops[0].swz) == 1 and a.ops[1].neg and
                a.ops[1].kind == "r" and is_lit(a.ops[2], EIGHT) and b.op == "exp" and b.ops[0].text == a.ops[0].text and
                b.ops[1].kind == "r" and scalar_reg(b.ops[1]) == (a.ops[0].index, "xyzw".index(a.ops[0].swz))):
            exp_at.append(k)
    if len(exp_at) != 1:
        raise Fail("exponent sites %s" % exp_at)
    ke = exp_at[0]
    X = (ins[ke].ops[0].index, "xyzw".index(ins[ke].ops[0].swz))
    Y = scalar_reg(ins[ke].ops[1])
    if ins[ke].depth != 0:
        raise Fail("exponent inside flow control")
    # 2. N.H power: dp3_sat A ; log A, A ; mul A, A, X (either order) ; exp A, A
    nh = []
    for k in range(len(ins)):
        m = ins[k]
        if m.op != "mul" or m.ops[0].kind != "r" or len(m.ops[0].swz) != 1:
            continue
        Ar = (m.ops[0].index, "xyzw".index(m.ops[0].swz))
        srcs = [scalar_reg(o) for o in m.ops[1:]]
        if sorted(map(str, srcs)) != sorted(map(str, [Ar, X])):
            continue
        if last_def(p, k, *X) != ke + 1:
            continue
        lg = last_def(p, k, *Ar)
        if lg is None or ins[lg].op != "log" or scalar_reg(ins[lg].ops[1]) != Ar:
            continue
        dp = last_def(p, lg, *Ar)
        if dp is None or ins[dp].op != "dp3" or not ins[dp].sat:
            continue
        ex = k + 1
        if ex >= len(ins) or ins[ex].op != "exp" or scalar_reg(ins[ex].ops[1]) != Ar or ins[ex].ops[0].text != m.ops[0].text:
            continue
        nh.append((dp, ex, Ar))
    if len(nh) != 1:
        raise Fail("N.H sites %s" % nh)
    kdp, kex, Ar = nh[0]
    if ins[kex].depth != 0 or ins[kdp].depth != 0:
        raise Fail("N.H inside flow control")
    # which dp3 operand is N: the one not reaching _WorldSpaceLightPos0
    lslot, lrow = meta["lightpos"]

    def is_lightpos(o):
        return o.kind == "cb" and o.index == lslot and o.row == (None, lrow)
    nops = []
    for i in (1, 2):
        o = ins[kdp].ops[i]
        if o.kind != "r":
            raise Fail("N.H operand %s" % o.text)
        if not reaches(p, kdp, o.index, sorted({"xyzw".index(o.sel[c]) for c in range(3)}), is_lightpos):
            nops.append(o)
    if len(nops) != 1:
        raise Fail("N.H: %d operands without the light position" % len(nops))
    N = nops[0]
    # N = W * rsq(dot(W, W)): find W
    ncomps = ["xyzw".index(N.sel[c]) for c in range(3)]
    jn = last_def(p, kdp, N.index, ncomps[0])
    if jn is None or any(last_def(p, kdp, N.index, c) != jn for c in ncomps) or ins[jn].op != "mul":
        raise Fail("normalised N not one mul")
    wops = [o for o in ins[jn].ops[1:] if len({o.sel[c] for c in ncomps}) == 3]
    if len(wops) != 1:
        raise Fail("N normalise operands")
    W = wops[0]
    if W.kind == "r":
        for c in ncomps:
            d = last_def(p, jn, W.index, "xyzw".index(W.sel[c]))
            if d is None or d >= ke:
                raise Fail("specular normal defined after the exponent (%s)" % d)
    elif W.kind != "v":
        raise Fail("specular normal operand %s" % W.text)
    wexpr = "%s%d.%s" % (W.kind, W.index, "".join(W.sel[c] for c in ncomps))
    if W.neg or W.abs:
        raise Fail("specular normal with modifiers")
    # 3. diffuse N.L: dp3_sat D feeding mad_sat Q, D, P, R with P, R from _SubdermisColor
    gslot, srow = meta["subdermis"]

    def is_subd(o):
        return o.kind == "cb" and o.index == gslot and o.row == (None, srow)
    nl = []
    for k in range(len(ins)):
        m = ins[k]
        if m.op != "mad" or not m.sat:
            continue
        D = scalar_reg(m.ops[1])
        if D is None:
            continue
        ok = True
        for o in m.ops[2:]:
            if is_subd(o):
                continue
            if o.kind != "r" or not reaches(p, k, o.index, sorted({"xyzw".index(o.sel[c]) for c in m.ops[0].mask}), is_subd):
                ok = False
        if not ok:
            continue
        j = last_def(p, k, *D)
        if j is None or ins[j].op != "dp3" or not ins[j].sat:
            continue
        nl.append((j, D, k))
    if len(nl) != 1:
        raise Fail("diffuse N.L sites %s" % nl)
    kdl, D, kq = nl[0]
    if ins[kdl].depth != 0 or kdl > kex:
        raise Fail("diffuse N.L inside flow control or after N.H")
    an = {"exp": ke, "X": X, "Y": Y, "nh_exp": kex, "A": Ar, "W": wexpr, "nl": kdl, "D": D, "kdp": kdp, "jn": jn, "Wkind": W.kind,
          "Wreg": W.index, "Wcomps": ["xyzw".index(W.sel[c]) for c in ncomps],
          "specint": "cb%d[%d].%s" % (meta["specint"][0], meta["specint"][1], "xyzw"[meta["specint"][2]])}
    # 4. L: the diffuse N.L operand reaching _WorldSpaceLightPos0
    lops = [o for o in ins[kdl].ops[1:3] if o.kind == "r" and reaches(p, kdl, o.index, comps3(o), is_lightpos)]
    if len(lops) != 1:
        raise Fail("diffuse N.L: %d operands with the light position" % len(lops))
    an["L"] = p.src(lops[0], [0, 1, 2])
    an["Nd"] = p.src([o for o in ins[kdl].ops[1:3] if o is not lops[0]][0], [0, 1, 2])  # the diffuse normal
    # 5. V: "add V, -P, C" (either order), C = the camera position (per camera or per eye), in the slice of N.H's H
    H = [o for o in ins[kdp].ops[1:3] if o is not N][0]
    hslice = slice_of(p, kdp, H.index, comps3(H))
    cp, scp = meta.get("campos"), meta.get("stereocampos")

    def is_campos(o):
        return o.kind == "cb" and ((cp and o.index == cp[0] and o.row == (None, cp[1])) or
                                   (scp and o.index == scp[0] and o.row[0] is not None and o.row[1] == scp[1]))
    vs = [k for k in hslice if ins[k].op == "add" and len(ins[k].ops[0].swz) == 3 and any(is_campos(o) for o in ins[k].ops[1:])]
    if len(vs) != 1 or ins[vs[0]].depth != 0 or vs[0] > ke:
        raise Fail("view vector sites %s" % vs)
    an["kv"], an["V"] = vs[0], p.dest(ins[vs[0]].ops[0])
    po = [o for o in ins[vs[0]].ops[1:] if not is_campos(o)]
    if len(po) != 1 or not po[0].neg or po[0].abs or po[0].kind not in ("r", "v"):
        raise Fail("view vector: position operand")
    pt = p.src(po[0], [0, 1, 2])
    an["P"] = pt[2:-1] if pt.startswith("(-") and pt.endswith(")") else None
    if not an["P"]:
        raise Fail("view vector: position text %s" % pt)
    # 6. the specular normal's normal map: W = (dp3 TBN row, tangent normal) x 3; in the tangent normal's slice one
    #    texture2d sample and "mad ..., _SpecularBumpiness, n, l(0, 0, 1)"
    an["nmap"] = None
    if W.kind == "r":
        defs = [last_def(p, jn, W.index, "xyzw".index(W.sel[c])) for c in ncomps]
        rows, tn = [], set()
        for j in defs:
            m = ins[j]
            vo = [o for o in m.ops[1:] if o.kind == "v"]
            ro = [o for o in m.ops[1:] if o.kind == "r"]
            if m.op != "dp3" or len(vo) != 1 or len(ro) != 1 or ro[0].neg or ro[0].abs or vo[0].neg or vo[0].abs:
                raise Fail("specular normal not dp3(TBN row, tangent normal): %s" % m.line)
            rows.append(p.src(vo[0], [0, 1, 2]))
            tn.add(p.src(ro[0], [0, 1, 2]))
        if len(tn) != 1 or len(set(defs)) != 3:
            raise Fail("specular normal rows")
        tno = [o for o in ins[min(defs)].ops[1:] if o.kind == "r"][0]
        tslice = slice_of(p, min(defs), tno.index, comps3(tno))
        smp = [k for k in tslice if ins[k].op == "sample" and p.textures[ins[k].ops[2].index] == "texture2d"]
        bump = [k for k in tslice if ins[k].op == "mad" and ins[k].ops[1].kind == "cb" and len(set(ins[k].ops[1].sel)) == 1 and
                ins[k].ops[3].kind == "l" and ins[k].ops[3].lit[:3] == [0, 0, 0x3f800000]]
        if len(smp) != 1 or len(bump) != 1 or any(ins[k].depth for k in tslice):
            raise Fail("specular normal slice: samples %s, bumpiness %s" % (smp, bump))
        s = ins[smp[0]]
        an["nmap"] = {"ksample": smp[0], "tex": s.ops[2].index, "smp": s.ops[3].index, "uv": p.src(s.ops[1], [0, 1]),
                      "bump": p.src(ins[bump[0]].ops[1], [0]), "rows": rows, "tangent": tn.pop(), "kfirst": min(defs),
                      "klast": max(defs), "W": [W.sel[c] for c in ncomps], "Wreg": W.index}
    # 7. S: "mul T2, T, _SpecColor", then "mul S, fresnel, T2" (T2's only use); S unchanged until after the exponent and
    #    used only after it: once by "mul Sn, S, (n + 2) / 2pi" (n = X after the exponent) and at most once more (the IBL)
    scs, scr = meta["speccolor"]
    k1s = [k for k, m in enumerate(ins) if m.op == "mul" and any(o.kind == "cb" and o.index == scs and o.row == (None, scr) for o in m.ops[1:])]
    if len(k1s) != 1:
        raise Fail("_SpecColor products %s" % k1s)
    k1 = k1s[0]
    tops = [o for o in ins[k1].ops[1:] if o.kind == "r"]
    t2 = ins[k1].ops[0]
    u = uses(p, k1, t2.index, t2.mask)
    if len(tops) != 1 or len(t2.swz) != 3 or len(u) != 1 or ins[u[0]].op != "mul" or len(ins[u[0]].ops[0].swz) != 3:
        raise Fail("_SpecColor product and its use")
    k2 = u[0]
    Sd = ins[k2].ops[0]
    if ins[k1].depth or ins[k2].depth or k2 > ke:
        raise Fail("S inside flow control or after the exponent")
    for c in Sd.mask:
        if last_def(p, ke + 2, Sd.index, c) != k2:
            raise Fail("S rewritten before the exponent")
    su = uses(p, k2, Sd.index, Sd.mask)
    if not su or su[0] <= ke + 1:
        raise Fail("S used before the exponent")

    def is_norm(j):
        """mul at j reads (n + 2) / 2pi = mad(n, 1/2pi, 1/pi) with n from the exponent"""
        for o in ins[j].ops[1:]:
            rc = scalar_reg(o)
            if rc is None:
                continue
            d = last_def(p, j, *rc)
            if (d is not None and ins[d].op == "mad" and scalar_reg(ins[d].ops[1]) == X and last_def(p, d, *X) == ke + 1 and
                    is_lit(ins[d].ops[2], 0x3e22f983) and is_lit(ins[d].ops[3], 0x3ea2f983)):
                return True
        return False
    norm = [j for j in su if ins[j].op == "mul" and is_norm(j)]
    others = [j for j in su if j not in norm]
    if len(norm) != 1 or len(others) > 1 or any(ins[j].op != "mul" for j in others):
        raise Fail("S uses %s" % [ins[j].line for j in su])
    an["S"], an["k1"], an["T"] = p.dest(Sd), k1, p.src(tops[0], [0, 1, 2])
    # the spec map: T = saturate(sample + _SpecOffset)
    jt = def_of(p, k1, tops[0], 3)
    ro = [o for o in ins[jt].ops[1:] if o.kind == "r"] if jt is not None else []
    js = def_of(p, jt, ro[0], 3) if jt is not None and ins[jt].op == "add" and ins[jt].sat and len(ro) == 1 else None
    if js is None or ins[js].op != "sample" or ins[js].depth:
        raise Fail("spec map sample")
    s = ins[js]
    an["specmap"] = {"ksample": js, "tex": s.ops[2].index, "smp": s.ops[3].index, "uv": p.src(s.ops[1], [0, 1])}
    so = [o for o in ins[jt].ops[1:] if o.kind == "cb"]
    an["specoff"] = p.src(so[0], [0, 1, 2]) if len(so) == 1 else None
    # 8. A after the power: "mul A, A, M" with M = min(10 D, 1), "mul A, A, 0.5", then "mad o, A, Sn, diffuse" with Sn
    #    = S * (n + 2) / 2pi, or that times one constant (the exposure)
    chain, k, rc = [], kex, Ar
    for step in range(3):
        u = uses(p, k, rc[0], [rc[1]])
        if len(u) != 1:
            raise Fail("N.H power step %d used %d times" % (step, len(u)))
        m = ins[u[0]]
        chain.append(u[0])
        if step < 2:
            if m.op != "mul" or len(m.ops[0].swz) != 1:
                raise Fail("N.H power step %d: %s" % (step, m.line))
            k, rc = u[0], (m.ops[0].index, "xyzw".index(m.ops[0].swz))
    c1, c2, c3 = (ins[j] for j in chain)
    an["c3"] = chain[2]
    mo = [o for o in c1.ops[1:] if scalar_reg(o) != Ar]
    jm = last_def(p, chain[0], *scalar_reg(mo[0])) if len(mo) == 1 and scalar_reg(mo[0]) else None
    jt = last_def(p, jm, *scalar_reg(ins[jm].ops[1])) if jm is not None and ins[jm].op == "min" and is_lit(ins[jm].ops[2], 0x3f800000) and scalar_reg(ins[jm].ops[1]) else None
    if (jt is None or ins[jt].op != "mul" or not any(scalar_reg(o) == D for o in ins[jt].ops[1:]) or
            not any(is_lit(o, 0x41200000) for o in ins[jt].ops[1:]) or last_def(p, jt, *D) != kdl):
        raise Fail("N.H power not times min(10 N.L, 1): %s" % c1.line)
    prev = (c1.ops[0].index, "xyzw".index(c1.ops[0].swz))
    if not (any(scalar_reg(o) == prev for o in c2.ops[1:]) and any(is_lit(o, 0x3f000000) for o in c2.ops[1:])):
        raise Fail("N.H power not times 0.5: %s" % c2.line)
    prev = (c2.ops[0].index, "xyzw".index(c2.ops[0].swz))
    if c3.op != "mad" or scalar_reg(c3.ops[1]) != prev or c3.ops[2].kind != "r":
        raise Fail("N.H power not into mad A, Sn, diffuse: %s" % c3.line)
    sn = {last_def(p, chain[2], c3.ops[2].index, c) for c in src_comps(c3, 2, c3.ops[0].mask)}
    if len(sn) == 1 and sn != set(norm):
        j = sn.pop()
        ro = [o for o in ins[j].ops[1:] if o.kind == "r"]
        sn = {def_of(p, j, ro[0], 3)} if ins[j].op == "mul" and len(ro) == 1 and any(o.kind == "cb" for o in ins[j].ops[1:]) else set()
    if sn != set(norm):
        raise Fail("mad A, Sn, diffuse: Sn is not S * (n + 2) / 2pi")
    # 8b. the pass's light: the one use of that sum, "mul R, sum, Y" with Y made from _LightColor0 (times attenuation
    #     and shadow)
    lcs, lcr = meta["lightcolor"]

    def is_lightcol(o):
        return o.kind == "cb" and o.index == lcs and o.row == (None, lcr)
    so = c3.ops[0]
    lu = uses(p, chain[2], so.index, list(so.mask))
    if len(lu) != 1 or ins[lu[0]].op != "mul" or ins[lu[0]].depth or len(ins[lu[0]].ops[0].mask) != 3:
        raise Fail("light: the specular + diffuse sum's use %s" % [ins[j].line for j in lu])
    ml = ins[lu[0]]
    yo = [o for o in ml.ops[1:] if not (o.kind == "r" and o.index == so.index)]
    if len(yo) != 1 or yo[0].kind != "r" or yo[0].neg or yo[0].abs or not reaches(p, lu[0], yo[0].index, src_comps(ml, ml.ops.index(yo[0]), [0, 1, 2]), is_lightcol):
        raise Fail("light: %s" % ml.line)
    an["klight"], an["light"] = lu[0], p.src(yo[0], [0, 1, 2])
    # 9. the IBL mip Y (when not X): written after the exponent, read once, as the LOD of the cube sample
    if X != Y:
        yu = uses(p, ke, Y[0], [Y[1]])
        if len(yu) != 1 or ins[yu[0]].op != "sample_l" or p.textures[ins[yu[0]].ops[2].index] != "texturecube" or scalar_reg(ins[yu[0]].ops[4]) != Y:
            raise Fail("IBL mip uses %s" % [ins[j].line for j in yu])
        an["kcube"] = yu[0]
    else:
        an["kcube"] = None
    # 10. the diffuse factor F: from VaM's wrap "mad_sat Q, D, 1 - S/2, S/2", the three-component products by factors
    #     made from D or _SubdermisColor (1 - S/2, ((D + 1) / 2)^2), up to the first product with anything else (the
    #     albedo); F = 2 (1 - S/2) saturate(S/2 + (1 - S/2) D) ((D + 1) / 2)^2 / 2, the 2 coming later (checked by mode 2)
    an["dl_ops"] = [p.src(o, [0, 1, 2]) for o in ins[kdl].ops[1:3]]

    def from_ds(j, i):
        """operand i of instruction j is made from D or _SubdermisColor"""
        o = ins[j].ops[i]
        if is_subd(o):
            return True
        if o.kind != "r":
            return False
        sl = slice_of(p, j, o.index, src_comps(ins[j], i, ins[j].ops[0].mask))
        return kdl in sl or any(is_subd(q) for k2 in sl for q in ins[k2].ops[1:])
    kf, fop = kq, ins[kq].ops[0]
    if len(fop.mask) != 3:
        raise Fail("diffuse wrap not three components: %s" % ins[kq].line)
    while True:
        u = uses(p, kf, fop.index, list(fop.mask))
        if len(u) != 1:
            raise Fail("diffuse factor at %d used %d times" % (kf, len(u)))
        m = ins[u[0]]
        if m.op != "mul" or len(m.ops[0].mask) != 3 or m.depth:
            break
        # the operand reading the factor so far (by components: "mul r3.xyz, r3.xxxx, r3.yzwy" reads it as r3.yzw)
        reads_f = [m.ops[i].kind == "r" and m.ops[i].index == fop.index and
                   set(src_comps(m, i, m.ops[0].mask)) <= set(fop.mask) for i in (1, 2)]
        other = [i for i in (1, 2) if not reads_f[i - 1]]
        if len(other) != 1:
            raise Fail("diffuse factor product: %s" % m.line)
        if not from_ds(u[0], other[0]):
            break
        kf, fop = u[0], m.ops[0]
    an["kF"], an["F"] = kf, p.dest(fop)
    an["subd"] = "cb%d[%d].xyz" % (gslot, srow)
    # 11. teeth and tongue: the main texture's UV (an input register: with _MainTex_ST, as the transmission reads it)
    #     and the program's one return, at its end
    mt = [k for k, m in enumerate(ins) if m.op in ("sample", "sample_b", "sample_l") and m.ops[2].index == meta["maintex"]]
    if not mt or ins[mt[0]].depth or ins[mt[0]].ops[1].kind != "v" or len({p.src(ins[k].ops[1], [0, 1]) for k in mt}) != 1:
        raise Fail("main texture samples %s" % [ins[k].line for k in mt])
    an["maintex"] = {"ksample": mt[0], "uv": p.src(ins[mt[0]].ops[1], [0, 1])}
    rets = [k for k, m in enumerate(ins) if m.op == "ret"]
    if rets != [len(ins) - 1] or ins[rets[0]].depth:
        raise Fail("returns %s" % rets)
    an["ret"] = rets[0]
    for k in (an["kv"], k1, kdl, kdp, kex, ke, kf, an["klight"]):
        if ins[k].depth:
            raise Fail("anchor inside flow control")
    return an


def find_sss(p, an):
    """the albedo product, the factor between it and the diffuse operand, the light scale (see the docstring)"""
    ins = p.instrs
    fop = ins[an["kF"]].ops[0]
    u = uses(p, an["kF"], fop.index, list(fop.mask))
    if len(u) != 1 or ins[u[0]].op != "mul" or len(ins[u[0]].ops[0].mask) != 3 or ins[u[0]].depth:
        raise Fail("albedo product %s" % [ins[j].line for j in u])
    ka = u[0]
    m = ins[ka]
    comps = list(m.ops[0].mask)
    reads_f = [i for i in (1, 2) if m.ops[i].kind == "r" and m.ops[i].index == fop.index and
               set(src_comps(m, i, m.ops[0].mask)) <= set(fop.mask)]
    if len(reads_f) != 1:
        raise Fail("albedo product operands: %s" % m.line)
    fi = reads_f[0]
    out = {"ka": ka, "alb": p.src(m.ops[3 - fi], comps), "F": p.src(m.ops[fi], comps)}
    c3 = ins[an["c3"]]
    dop = c3.ops[3]
    jd = def_of(p, an["c3"], dop, 3)
    pd = m.ops[0]
    if jd == ka:
        out["factor"] = "1.0"
    elif (jd is not None and ins[jd].op == "add" and not ins[jd].sat and ins[jd].ops[1].text == ins[jd].ops[2].text and
          ins[jd].ops[1].kind == "r" and ins[jd].ops[1].index == pd.index and def_of(p, jd, ins[jd].ops[1], 3) == ka and
          not ins[jd].ops[1].neg and not ins[jd].ops[1].abs):
        out["factor"] = "2.0"
    else:
        raise Fail("diffuse operand not the albedo product (x2): %s" % (ins[jd].line if jd is not None else None))
    dst = c3.ops[0]
    u = uses(p, an["c3"], dst.index, list(dst.mask))
    if len(u) != 1 or ins[u[0]].op != "mul" or len(ins[u[0]].ops[0].mask) != 3 or ins[u[0]].depth:
        raise Fail("light scale: uses of the lit result %s" % [ins[j].line for j in u])
    kl = u[0]
    ml = ins[kl]
    lc = list(ml.ops[0].mask)
    reads_r = [i for i in (1, 2) if ml.ops[i].kind == "r" and ml.ops[i].index == dst.index and
               set(src_comps(ml, i, ml.ops[0].mask)) <= set(dst.mask)]
    if len(reads_r) != 1:
        raise Fail("light scale operands: %s" % ml.line)
    out["kl"], out["L"] = kl, p.src(ml.ops[3 - reads_r[0]], lc)
    pos = [i for i, x in enumerate(p.sig) if x[0] == "SV_POSITION"]
    if len(pos) != 1 or p.sig[pos[0]][3] & 4 == 0:
        raise Fail("no SV_POSITION.z input")
    out["posz"] = "i.e%d.z" % pos[0]
    return out


def cb_text(m):
    return "cb%d[%d].%s" % (m[0], m[1], "xyzw"[m[2]]) if m else None


def reads_cb(o, m):
    """operand o reads the cbuffer member m (slot, row, component) as one component"""
    return o.kind == "cb" and o.index == m[0] and o.row == (None, m[1]) and set(o.sel) == {"xyzw"[m[2]]}


def world_normal(p, k, o):
    """the TBN rows and the tangent-space normal of a normalised world normal o read at k:
    o = W * rsq(dot(W, W)), W = (dp3 row_i, t) x 3 -> (rows, tangent operand, the three dp3)"""
    ins = p.instrs
    nc = ["xyzw".index(o.sel[c]) for c in range(3)]
    jn = last_def(p, k, o.index, nc[0])
    if jn is None or any(last_def(p, k, o.index, c) != jn for c in nc) or ins[jn].op != "mul":
        raise Fail("normal not one normalising mul")
    wops = [x for x in ins[jn].ops[1:] if x.kind == "r" and len({x.sel[c] for c in nc}) == 3]
    if len(wops) != 1:
        raise Fail("normal: normalise operands")
    W = wops[0]
    defs = [last_def(p, jn, W.index, "xyzw".index(W.sel[c])) for c in nc]
    rows, tn = [], []
    for j in defs:
        m = ins[j] if j is not None else None
        vo = [x for x in m.ops[1:] if x.kind == "v"] if m else []
        ro = [x for x in m.ops[1:] if x.kind == "r"] if m else []
        if not m or m.op != "dp3" or len(vo) != 1 or len(ro) != 1 or ro[0].neg or ro[0].abs or vo[0].neg or vo[0].abs:
            raise Fail("normal not dp3(TBN row, tangent normal)")
        rows.append(p.src(vo[0], [0, 1, 2]))
        tn.append(ro[0])
    if len({p.src(x, [0, 1, 2]) for x in tn}) != 1 or len(set(defs)) != 3:
        raise Fail("normal rows")
    return rows, tn[0], defs


def find_detail(p, an, meta):
    """where the 0.11.0 detail changes go (see SKIN DETAIL in the docstring); Fail = the program keeps 0.10.0's"""
    ins = p.instrs
    d = {}
    # the gloss map: the one 2-D sample in the slice of the IBL mip Y (= 8 - log2 n), with _GlossOffset and _Shininess
    ke = an["exp"]
    sl = slice_of(p, ke, an["Y"][0], [an["Y"][1]])
    smp = [k for k in sl if ins[k].op == "sample" and p.textures[ins[k].ops[2].index] == "texture2d"]
    go, sh = meta.get("glossoff"), meta.get("shininess")
    if len(smp) != 1 or not go or not sh:
        raise Fail("gloss sample %s" % smp)
    if not any(reads_cb(o, go) for k in sl for o in ins[k].ops[1:]) or not any(reads_cb(o, sh) for k in sl for o in ins[k].ops[1:]):
        raise Fail("gloss offset / shininess not in the exponent's slice")
    s = ins[smp[0]]
    if s.depth:
        raise Fail("gloss sample inside flow control")
    d["gloss"] = {"ksample": smp[0], "tex": s.ops[2].index, "smp": s.ops[3].index, "uv": p.src(s.ops[1], [0, 1]),
                  "off": cb_text(go), "shin": cb_text(sh)}
    nm = an["nmap"]
    if an["specoff"] is None:
        raise Fail("spec offset")
    if an["kv"] > an["specmap"]["ksample"] or (nm and an["kv"] > nm["ksample"]):
        raise Fail("view vector after the spec map / normal map sample")
    d["normal"] = None
    if nm:
        # the diffuse normal: the diffuse N.L's operand without the light position, VaM's tangent normal bent by
        # _DiffuseBumpiness from the same normal-map sample as the specular one
        lslot, lrow = meta["lightpos"]
        kdl = an["nl"]
        no = [o for o in ins[kdl].ops[1:3] if o.kind == "r" and not reaches(
            p, kdl, o.index, comps3(o), lambda x: x.kind == "cb" and x.index == lslot and x.row == (None, lrow))]
        if len(no) != 1:
            raise Fail("diffuse N.L normal operand")
        rows, t, defs = world_normal(p, kdl, no[0])
        k0 = min(defs)
        tslice = slice_of(p, k0, t.index, comps3(t))
        tsm = [k for k in tslice if ins[k].op == "sample"]
        bump = [k for k in tslice if ins[k].op == "mad" and ins[k].ops[1].kind == "cb" and len(set(ins[k].ops[1].sel)) == 1 and
                ins[k].ops[3].kind == "l" and ins[k].ops[3].lit[:3] == [0, 0, 0x3f800000]]
        if tsm != [nm["ksample"]] or len(bump) != 1 or any(ins[k].depth for k in tslice) or ins[k0].depth:
            raise Fail("diffuse normal slice: samples %s, bumpiness %s" % (tsm, bump))
        tc = comps3(t)
        u = uses(p, max(tslice), t.index, tc)
        if sorted(u) != sorted(defs):
            raise Fail("diffuse tangent normal used outside its three dp3: %s" % [ins[j].line for j in u])
        if rows != nm["rows"]:
            raise Fail("diffuse and specular TBN rows differ")
        d["normal"] = {"k0": k0, "t": "r%d.%s" % (t.index, "".join("xyzw"[c] for c in tc)), "bump": p.src(ins[bump[0]].ops[1], [0]),
                       "rows": rows, "raw": "r%d" % ins[nm["ksample"]].ops[0].index}
        # the ambient: after the IBL cube sample, W (VaM's specular normal) feeds only the SH and VaM's N.H
        d["ambient"] = None
        if an["kcube"] is not None:
            kc = an["kcube"]
            wr, wc = nm["Wreg"], ["xyzw".index(c) for c in nm["W"]]
            for c in wc:
                j = last_def(p, kc, wr, c)
                if j is None or not nm["kfirst"] <= j <= nm["klast"] or ins[j].op != "dp3":
                    raise Fail("specular normal rewritten before the cube sample")
            if ins[kc].depth or kc < nm["ksample"]:
                raise Fail("cube sample inside flow control or before the normal map")
            d["ambient"] = {"k": kc, "W": "r%d.%s" % (wr, "".join(nm["W"]))}
    return d


def patch_detail(ins, an, d):
    """0.11.0: the same skin roughness for every preset (inserted where the gloss map is sampled: its mean), diffuse and
    ambient normals filtered to a width in metres, the cavity's spec map likewise"""
    g = d["gloss"]
    ins.add(g["ksample"], before="\n".join([
        "float specGlossMean = saturate(t%d.SampleLevel(s%d, %s, 14.0).x + %s);  // the mean gloss: its last mip" % (g["tex"], g["smp"], g["uv"], g["off"]),
        "float specGlossU = 1.0 - specGlossMean;",
        "float specNMean = exp2(lerp(%s, 1.0, specGlossU * specGlossU));  // VaM's n there" % g["shin"],
        "float4 skinTierA = skinTier.Load(0), skinTierB = skinTier.Load(1), skinTierC = skinTier.Load(2);  // this map's row (0.13.0, 0.18.0)",
        "float specGlossT = t%d.SampleBias(s%d, %s, skinTierB.x).x;  // the raw gloss texel (no _GlossOffset)" % (g["tex"], g["smp"], g["uv"])]))
    ins.add(an["kv"], after="float skinPix = max(length(ddx(eyeP)), length(ddy(eyeP)));  // metres per pixel")
    sm = an["specmap"]
    ins.add(sm["ksample"], before="\n".join([
        "float2 skinCavUv = %s;" % sm["uv"],
        "float skinCavK = SkinFilter(skinNorm.z, skinPix);",
        "float3 skinCav = saturate(t%d.SampleGrad(s%d, skinCavUv, ddx(skinCavUv) * skinCavK, ddy(skinCavUv) * skinCavK).xyz + %s);" % (
            sm["tex"], sm["smp"], an["specoff"])]))
    n = d["normal"]
    if n:
        nm = an["nmap"]
        ins.add(nm["ksample"], after="\n".join([
            "float4 skinNRaw = %s;  // VaM's normal-map sample" % n["raw"],
            "float2 skinNUv = %s;" % nm["uv"],
            "float skinNK = SkinFilter(skinNorm.x, skinPix);",
            "float4 skinNTex = skinNorm.x > 0.0 ? t%d.SampleGrad(s%d, skinNUv, ddx(skinNUv) * skinNK, ddy(skinNUv) * skinNK) : skinNRaw;" % (nm["tex"], nm["smp"]),
            "float3 skinDt = SpecTangentNormal(skinNTex, %s);  // the diffuse normal, filtered" % n["bump"]]))
        ins.add(n["k0"], before="if (skinNorm.x > 0.0) %s = skinDt;" % n["t"])
        a = d.get("ambient")
        if a:
            ins.add(a["k"], after="if (skinNorm.y != 0.0) %s = normalize(float3(%s));  // the ambient on the diffuse normal" % (
                a["W"], ", ".join("dot(%s, skinDt)" % r for r in n["rows"])))


def patch_sss(ins, an, ss):
    ins.add(an["kv"], after="float sssDist = length(%s);  // to the camera" % an["V"])
    ins.add(ss["ka"], before="float3 sssAlb = %s;  // albedo\nfloat3 sssF = %s;  // diffuse factor\n"
            "float3 sssTF = TranslucencyExcess(skinDiffuse0, scatX, %s);  // 0.20.0: the translucency over it (before the grazing factor)" % (
                ss["alb"], ss["F"], an["subd"]))
    ins.add(ss["kl"], before="float3 sssE = sssF * %s * (%s);  // direct diffuse light without the albedo\n"
            "float3 sssT = sssTF * %s * (%s);  // the translucency, the same light" % (ss["factor"], ss["L"], ss["factor"], ss["L"]))


def patch_nasal_ambient(p, inserts, an, e, base):
    """AO belongs to the SH + sky reflection term, before it is added to direct lighting.
    Require a unique data-flow match; ForwardAdd contains no environment light and gets no AO.
    """
    if not base:
        return None
    # VaM's default Nostrils IBLFilter=0.5 versus Face=0 creates a material-sized
    # step in environment light. Use this character's face baseline, then geometric
    # AO. Keep the original material and exact original result when amount is zero.
    filt = member(e, "$Globals", "_IBLFilter")
    fs = [k for k, m in enumerate(p.instrs) if m.op == "add" and not m.depth
          and any(reads_cb(o, filt) and o.neg for o in m.ops[1:])
          and any(is_lit(o, 0x3f800000) for o in m.ops[1:])]
    if len(fs) != 1:
        raise Fail("nasal filter: expected one environment filter, got %s" % fs)
    inserts.add(fs[0], after="if (noseCtl.z != 0.0) %s = 1.0 - lerp(%s, noseCtl.w, noseCtl.y);  // continuous environment baseline" %
                (p.dest(p.instrs[fs[0]].ops[0]), cb_text(filt)))
    sh = {member(e, "$Globals", "_SH%d" % i)[:2] for i in range(9)}
    lc = member(e, "$Globals", "_LightColor0")[:2]
    def depends(k, o, bindings):
        pred = lambda x: x.kind == "cb" and x.row[0] is None and (x.index, x.row[1]) in bindings
        return pred(o) or (o.kind == "r" and reaches(p, k, o.index, comps3(o), pred, depth=96))
    found = []
    for k in range(an["klight"] + 1, an["ret"]):
        m = p.instrs[k]
        if m.op != "mad" or m.depth or len(m.ops) != 4:
            continue
        for j in (1, 2):
            o, scale, direct = m.ops[j], m.ops[3-j], m.ops[3]
            if (o.kind == "r" and not o.neg and not o.abs and len(set(o.sel[:3])) == 3 and
                    depends(k, o, sh) and not depends(k, o, {lc}) and
                    scale.kind == "cb" and depends(k, direct, {lc}) and not depends(k, direct, sh)):
                found.append((k, p.src(o, [0, 1, 2])))
    if len(found) != 1:
        raise Fail("nasal AO: expected one environment/direct combine, got %s" % found)
    k, value = found[0]
    inserts.add(k, before="%s *= NoseVisibility(teethUv);  // environment-only occlusion (nasal AO)" % value)
    return {"combine": k, "environment": value, "filter": filt}


def find_shadow(p, e, keywords, an):
    """VaM's hard spot / point shadow filter (0.19.0; VaM forces Hard on both, the SOFT variants are left as they are):
    the radius "mul R, (1 - _LightShadowData.x), l(0.025 | 0.1)" before the 25-tap loop, the loop's shadow map, and the
    spot's shadow coordinates (the div by their w) or the point's light-to-position vector (position - _LightPositionRange)"""
    if "SHADOWS_SOFT" in keywords:
        return None
    spot = "SHADOWS_DEPTH" in keywords and "SPOT" in keywords
    if not spot and "SHADOWS_CUBE" not in keywords:
        return None
    lsd = member(e, "UnityShadows", "_LightShadowData")
    if lsd is None:
        raise Fail("shadow: no _LightShadowData")
    ins = p.instrs
    # the vertex normal (the TBN rows' third column, else the program's normal) and light - position
    rows, lp = (an.get("nmap") or {}).get("rows"), member(e, "UnityLighting", "_WorldSpaceLightPos0")
    n = "float3(%s)" % ", ".join(r.split(".")[0] + ".z" for r in rows) if rows else \
        an.get("W") if (an.get("W") or "").startswith("v") else None
    if n is None or lp is None or not an.get("P"):
        raise Fail("shadow: no vertex normal / light position")
    tol_ = "cb%d[%d].xyz - %s" % (lp[0], lp[1], an["P"])
    lit = 0x3ccccccd if spot else 0x3dcccccd
    muls = []
    for k, m in enumerate(ins):
        if m.op != "mul" or len(m.ops) != 3 or m.ops[0].kind != "r" or len(m.ops[0].mask) != 1:
            continue
        for j in (1, 2):
            o, l = m.ops[j], m.ops[3 - j]
            if not is_lit(l, lit) or scalar_reg(o) is None:
                continue
            d = last_def(p, k, *scalar_reg(o))
            if d is not None and ins[d].op == "add" and any(reads_cb(x, lsd) and x.neg for x in ins[d].ops[1:]) and \
                    any(is_lit(x, 0x3f800000) for x in ins[d].ops[1:]):
                muls.append(k)
    if len(muls) != 1:
        raise Fail("shadow: expected one radius, got %s" % muls)
    k = muls[0]
    samples = [j for j in range(k + 1, len(ins)) if ins[j].op == "sample_c_lz"]
    if not samples:
        raise Fail("shadow: no comparison sample after the radius")
    ks = samples[0]
    tex = ins[ks].ops[2].index
    if p.textures.get(tex) != ("texture2d" if spot else "texturecube"):
        raise Fail("shadow: t%d is %s" % (tex, p.textures.get(tex)))
    radius = p.dest(ins[k].ops[0])
    acc, after, lo = shadow_sum(p, ks)
    if lo < k:
        raise Fail("shadow: the radius is inside VaM's loop")
    smp = "s%d" % ins[ks].ops[3].index
    if spot:
        kd = def_of(p, ks, ins[ks].ops[4], 1)
        if kd is None or ins[kd].op != "div" or kd > k or ins[kd].depth != ins[k].depth:
            raise Fail("shadow: compare depth is not the div of the shadow coordinates")
        dv = ins[kd]
        reg, mask = dv.ops[0].index, dv.ops[0].mask
        cmp_ = ins[ks].ops[4]
        if len(mask) != 3 or cmp_.kind != "r" or "xyzw".index(cmp_.sel[0]) != mask[2] or \
                any((reg, c) in writes(ins[j]) for j in range(kd + 1, k + 1) for c in mask):
            raise Fail("shadow: the shadow coordinates do not reach the radius")
        mat = member(e, "UnityShadows", "unity_WorldToShadow")
        if mat is None or mat[2] != 0:
            raise Fail("shadow: no unity_WorldToShadow")
        mrows = ", ".join("cb%d[%d]" % (mat[0], mat[1] + i) for i in range(4))
        # VaM's tap rotation: the sincos (sin, cos) before the radius, not written again before the loop's sample
        scs = [j for j in range(k) if ins[j].op == "sincos" and ins[j].ops[0].kind == "r" and ins[j].ops[1].kind == "r"]
        if not scs:
            raise Fail("shadow: no tap rotation")
        js = scs[-1]
        rot = [ins[js].ops[0], ins[js].ops[1]]
        if any(len(o.mask) != 1 or any((o.index, o.mask[0]) in writes(ins[j]) for j in range(js + 1, ks)) for o in rot):
            raise Fail("shadow: the tap rotation does not reach the loop")
        return {"kind": "spot", "tex": tex, "radius_at": k, "div": kd, "w": p.src(dv.ops[2], [0]), "acc": acc, "after": after, "loop": lo,
                "call": "%s = ShadowSpotRadius(t%d, %s, %s, shadowW, %s, %s, %s, %s, %s, %s, teethUv);" % (
                    radius, tex, smp, p.dest(dv.ops[0]), radius, mrows, n, tol_, p.dest(rot[0]), p.dest(rot[1]))}
    lpr, proj = member(e, "UnityLighting", "_LightPositionRange"), member(e, "UnityLighting", "_LightProjectionParams")
    if lpr is None or proj is None:
        raise Fail("shadow: no _LightPositionRange / _LightProjectionParams")
    vecs = [j for j in range(k) if ins[j].op == "add" and ins[j].ops[0].kind == "r" and len(ins[j].ops[0].mask) == 3 and
            any(x.kind == "cb" and x.index == lpr[0] and x.row == (None, lpr[1]) and x.neg for x in ins[j].ops[1:])]
    if len(vecs) != 1:
        raise Fail("shadow: expected one light-to-position vector, got %s" % vecs)
    kv = vecs[0]
    reg, mask = ins[kv].ops[0].index, ins[kv].ops[0].mask
    if any((reg, c) in writes(ins[j]) for j in range(kv + 1, k + 1) for c in mask):
        raise Fail("shadow: the light-to-position vector does not reach the radius")
    # VaM's tap frame: the two vectors the radius scales right after it, and which takes the pattern's x / y in the loop
    rr, frame = ins[k].ops[0], {}
    for j in range(k + 1, ks):
        m = ins[j]
        if m.op == "mul" and len(m.ops[0].mask) == 3:
            r_ = [x for x in m.ops[1:3] if x.kind == "r" and x.index == rr.index and set(x.sel[:3]) == {rr.swz[0]}]
            v_ = [x for x in m.ops[1:3] if r_ and x.kind == "r" and x is not r_[0]]
            if r_ and len(v_) == 1 and v_[0].index == m.ops[0].index:
                frame[v_[0].index] = p.src(v_[0], [0, 1, 2])
    axis = {}
    for j in range(k + 1, ks):
        m = ins[j]
        if m.op in ("mul", "mad"):
            ic = [x for x in m.ops[1:3] if x.kind == "icb"]
            ve = [x for x in m.ops[1:3] if x.kind == "r" and x.index in frame]
            if ic and ve:
                axis[ic[0].sel[0]] = frame[ve[0].index]
    if len(frame) != 2 or sorted(axis) != ["x", "y"]:
        raise Fail("shadow: no point tap frame (%s, %s)" % (frame, axis))
    return {"kind": "point", "tex": tex, "radius_at": k, "acc": acc, "after": after, "loop": lo,
            "call": "%s = ShadowPointRadius(t%d, %s, %s, %s, cb%d[%d], %s, %s, %s, teethUv);" % (
                radius, tex, smp, p.dest(ins[kv].ops[0]), radius, proj[0], proj[1], n, axis["x"], axis["y"])}


def shadow_sum(p, ks):
    """VaM's 25-tap sum (the add of each tap's compare result in its loop) and the instruction after the loop"""
    ins, d = p.instrs, p.instrs[ks].ops[0]
    ends = [j for j in range(ks + 1, len(ins)) if ins[j].op == "endloop"]
    adds = [j for j in range(ks + 1, ends[0] if ends else ks + 1) if ins[j].op == "add" and
            any(x.kind == "r" and x.index == d.index and x.sel[0] == d.swz[0] for x in ins[j].ops[1:])]
    if len(adds) != 1 or len(ins[adds[0]].ops[0].mask) != 1:
        raise Fail("shadow: no tap sum in VaM's loop")
    # VaM's loop is skipped when ours ran (its sum is replaced): nothing else it writes may be read after it
    lo = max(j for j in range(ks) if ins[j].op == "loop")
    acc = (ins[adds[0]].ops[0].index, ins[adds[0]].ops[0].mask[0])
    live = {w for j in range(lo + 1, ends[0]) for w in writes(ins[j])} - {acc}
    for j in range(ends[0] + 1, len(ins)):
        m = ins[j]
        reads = m.ops[:1] if m.op in ("if_nz", "if_z", "breakc_nz", "breakc_z") else sources(m)
        hit = [o.index for o in reads if o.kind == "r" and any((o.index, "xyzw".index(c)) in live for c in o.sel)]
        if hit:
            raise Fail("shadow: VaM's loop leaves r%d live" % hit[0])
        if m.depth == ins[ends[0]].depth:
            live -= set(writes(m))
        if not live:
            break
    return p.dest(ins[adds[0]].ops[0]), ends[0] + 1, lo


def patch_shadow(inserts, sh):
    if sh["kind"] == "spot":
        inserts.add(sh["div"], before="float shadowW = %s;  // the shadow coordinates' w (0.19.0)" % sh["w"])
    inserts.add(sh["radius_at"], after=sh["call"] + "  // 0.19.0: the penumbra by the blocker distance, on the receiver's plane")
    inserts.add(sh["loop"], after="if (gShadowOurs >= 0.0) break;  // 0.19.0: ours ran, VaM's 25 taps would be thrown away")
    inserts.add(sh["after"], before="%s = gShadowOurs >= 0.0 ? gShadowOurs : %s;  // 0.19.0: replaces VaM's 25-tap sum" % (
        sh["acc"], sh["acc"]))


# The skin specular's low-light adaptation of the environment reflection (0.22.6, specCtl.x 1; SkinSpecLowLight): in
# the base pass VaM sums the environment's two parts in one instruction, the cube's specular and the SH. The specular
# part is copied before that sum (its operands may be the sum's destination) and its faded share taken off after it.
# Programs with the skin detail must have the split; others without it keep VaM's environment.
def patch_env_lowlight(p, inserts, an, det, ambient, base):
    ins = p.instrs
    if not (base and ambient):
        return
    kc = an["kcube"]
    if kc is None or ins[kc].depth:
        raise Fail("environment: no IBL cube sample at depth 0")
    k = ambient["combine"]
    vo = [o for o in ins[k].ops[1:3] if o.kind == "r" and p.src(o, [0, 1, 2]) == ambient["environment"]]
    j = def_of(p, k, vo[0], 3) if len(vo) == 1 else None
    dmask = ins[j].ops[0].mask if j is not None else []
    split = None
    if j is not None and not ins[j].depth and ins[j].op in ("mad", "add") and j > kc:
        m = ins[j]
        fc = lambda o: o.kind == "r" and kc in slice_of(p, j, o.index, src_comps(m, m.ops.index(o), dmask))
        if m.op == "mad":
            prod, add = [m.ops[1], m.ops[2]], m.ops[3]
            sp = [o for o in prod if fc(o)]
            if len(sp) == 1 and not fc(add):
                split = (sp[0], add)
        else:
            a, b = m.ops[1], m.ops[2]
            if fc(a) != fc(b):
                split = (a, b) if fc(a) else (b, a)
    if split and len(dmask) == 3 and not ins[j].sat:
        m = ins[j]
        cm = list(dmask)
        if m.op == "mad":
            other = [o for o in (m.ops[1], m.ops[2]) if o is not split[0]][0]
            spec = "%s * %s" % (p.src(split[0], cm), p.src(other, cm))
        else:
            spec = p.src(split[0], cm)
        inserts.add(j, before="float3 envSpec = %s;  // the environment's specular part, before VaM's sum" % spec,
                    after="\n".join([
                        "if (specCtl.x == 1.0) {",
                        "    float3 skinEnvSpec = envSpec;",
                        # Recover the arriving, decoded/exposed cube light before the
                        # skin's cavity and directional BRDF. Using the reflected RGB
                        # itself would also dim a normal environment by its small F0.
                        "    float skinEnvFade = SkinSpecLowLight(skinEnvSpec / max(specCav * specE, 1e-6));",
                        "    if (skinEnvFade < 1.0) %s -= skinEnvSpec * (1.0 - skinEnvFade);" % p.dest(m.ops[0]),
                        "}"]))
    elif det:
        raise Fail("skin specular: cannot separate environment reflection from diffuse SH for low-light adaptation")


def sss_source(src, ss):
    """the lifted source with the four outputs"""
    a, b = "float4 main(PSIn i) : SV_Target {", "    return o0;\n}\n"
    if src.count(a) != 1 or not src.endswith(b) or src.count("return o0;") != 1:
        raise SystemExit("unexpected lifted source shape")
    return src.replace(a, "SssOut main(PSIn i) {").replace(
        "return o0;", "return SssOutput(o0, sssE * mouthShade, sssT * mouthShade, sssAlb, sssDist, %s, regionalResponse);" % ss["posz"])


def member_any(e, name):
    for cb in e["cbuffers"]:
        if name in cb["members"]:
            off = cb["members"][name][0]
            return e["cbBind"][cb["name"]], off // 16
    return None


def comps3(o):
    return sorted({"xyzw".index(o.sel[c]) for c in range(3)})


def def_of(p, k, o, n):
    """the one instruction that last wrote the first n components register operand o reads at k (None if several)"""
    if o.kind != "r":
        return None
    d = {last_def(p, k, o.index, "xyzw".index(o.sel[c])) for c in range(n)}
    return d.pop() if len(d) == 1 else None


def uses(p, k, reg, comps):
    """instructions after k reading reg (any of comps) before all of them are written again outside flow control"""
    out, live = [], set(comps)
    for j in range(k + 1, len(p.instrs)):
        m = p.instrs[j]
        first = 0 if m.op in ("if_nz", "if_z", "breakc_nz", "breakc_z") else 2 if m.op == "sincos" else 1
        for i in range(first, len(m.ops)):
            o = m.ops[i]
            rc = set()
            if o.kind == "r" and o.index == reg:
                rc = set(src_comps(m, i, m.ops[0].mask)) if first == 1 else {"xyzw".index(c) for c in o.sel}
            if o.kind in ("cb", "icb") and o.row[0] and o.row[0].startswith("r%d." % reg):
                rc.add("xyzw".index(o.row[0][-1]))
            if rc & live:
                out.append(j)
                break
        if m.depth == 0:
            live -= {c for r, c in writes(m) if r == reg}
        if not live:
            break
    return out


def slice_of(p, k, reg, comps):
    """instructions (indices) the components of reg read at k depend on, stopping at samples"""
    seen, out, work = set(), set(), [(k, reg, c) for c in comps]
    while work:
        k0, r, c = work.pop()
        j = last_def(p, k0, r, c)
        if j is None or (j, c) in seen:
            continue
        seen.add((j, c))
        out.add(j)
        m = p.instrs[j]
        if m.op.startswith("sample"):
            continue
        for i, o in enumerate(m.ops[1:], 1):
            if o.kind == "r":
                for cc in src_comps(m, i, [c]):
                    work.append((j, o.index, cc))
            if o.kind in ("cb", "icb") and o.row[0]:
                rr = A.Operand(o.row[0])
                work.append((j, rr.index, "xyzw".index(rr.swz)))
    return sorted(out)


class Inserts(dict):
    def add(self, k, before="", after=""):
        b, a = self.get(k, ("", ""))
        self[k] = ("\n".join(x for x in (b, before) if x), "\n".join(x for x in (a, after) if x))


def patch(p, an, d=None, base=True):
    r = lambda rc: "r%d.%s" % (rc[0], "xyzw"[rc[1]])
    X, Y, Aa, D = r(an["X"]), r(an["Y"]), r(an["A"]), r(an["D"])
    ins = Inserts()
    ins.add(an["kv"], before="float3 eyeP = %s;  // the world position" % an["P"], after="float3 specV = normalize(%s);" % an["V"])
    # Capture the diffuse operand before the direct specular + diffuse MAD can
    # overwrite it; adapt only that MAD's specular share once its light is ready.
    direct_sum = p.instrs[an["c3"]]
    ins.add(an["c3"], before="float3 skinSpecDiffuse = %s;" % p.src(direct_sum.ops[3], direct_sum.ops[0].mask))
    ins.add(an["klight"], before="\n".join([
        "float3 eyeLight = %s;  // the light: colour x attenuation x shadow" % an["light"],
        "if (specCtl.x == 1.0) {",
        "    float skinSpecFade = SkinSpecLowLight(eyeLight);",
        "    if (skinSpecFade < 1.0) %s = lerp(skinSpecDiffuse, %s, skinSpecFade);" % (p.dest(direct_sum.ops[0]), p.dest(direct_sum.ops[0])),
        "}"]))
    sm = an["specmap"]
    ins.add(sm["ksample"], before="float3 specMapMean = t%d.SampleLevel(s%d, %s, 14.0).xyz;  // its last mip" % (sm["tex"], sm["smp"], sm["uv"]))
    ins.add(an["k1"], before="float3 specMap = %s;" % an["T"])
    nm = an["nmap"]
    if nm:
        ins.add(nm["ksample"], before="float4 specNTex = t%d.SampleBias(s%d, %s, specCtl.y);" % (nm["tex"], nm["smp"], nm["uv"]))
        raw=p.dest(p.instrs[nm["ksample"]].ops[0])
        ins.add(nm["ksample"],after="\n".join([
            "if(mouthCtl.y>0.0 && mouthCtl.w>0.0) {",
            "    uint tw,th; t%d.GetDimensions(tw,th); float2 du=float2(1.0/tw,0),dv=float2(0,1.0/th);" % nm["tex"],
            "    float2 huv=%s;" % nm["uv"],
            "    float hx=t%d.SampleBias(s%d,huv-du,.35).r-t%d.SampleBias(s%d,huv+du,.35).r;" % (nm["tex"],nm["smp"],nm["tex"],nm["smp"]),
            "    float hy=t%d.SampleBias(s%d,huv-dv,.35).r-t%d.SampleBias(s%d,huv+dv,.35).r;" % (nm["tex"],nm["smp"],nm["tex"],nm["smp"]),
            "    float3 hn=normalize(float3(float2(hx,hy)*12.0,1.0));",
            "    specNTex=float4(hn*.5+.5,1.0); %s=specNTex;" % raw,
            "}"]))
        ins.add(nm["kfirst"], before="float3 specNt0 = %s;  // VaM's tangent-space specular normal" % nm["tangent"])
        w = "r%d.%s" % (nm["Wreg"], "".join(nm["W"]))
        ins.add(nm["klast"], after="\n".join([
            "float3 specNt = SpecTangentNormal(specNTex, mouthCtl.y>0.0 ? lerp(%s,1.0,mouthCtl.y) : %s);" % (nm["bump"],nm["bump"]),
            "float3 specNw = float3(%s);" % ", ".join("dot(%s, specNt)" % row for row in nm["rows"]),
            "float3 specDn = specNt0 - specNt;",
            "float specVar = specCtl.z * dot(specDn, specDn);",
            "if (specCtl.x == 2.0) %s = specNw;  // test: VaM's specular normal rebuilt" % w]))
        pre = []
    else:
        pre = ["float3 specNw = %s;" % an["W"], "float specVar = 0.0;"]
    s = an["S"]
    si = an["specint"]
    if d:
        cav = "\n".join([
            "float3 specT = skinNorm.z > 0.0 ? skinCav : specMap;",
            "float specCav = skinCtl.w != 0.0 ? (skinTierC.w > 0.0 || skinTierB.y > 0.0 ? skinTierB.y : 1.0) * saturate(4.0 * %s) * min(dot(specT, 1.0) / max(skinTierC.w > 0.0 ? skinTierC.z : dot(saturate(specMapMean + %s), 1.0), 0.15), 3.0)" % (si, an["specoff"]),
            "    : saturate(%s) * min(dot(specT, 1.0) / max(dot(specMapMean, 1.0), 0.15), 3.0);" % si])
    else:
        cav = "float specCav = saturate(%s) * min(dot(specMap, 1.0) / max(dot(specMapMean, 1.0), 0.15), 3.0);" % si
    code = pre + [
        "float3 specN = normalize(specNw);",
        "float2 specA2 = %s;  // %s = n" % ("SpecLobeA2B(SpecBaseA2(%s, specNMean, specGlossT, skinTierA, skinTierB, skinTierC), specVar)" % X if d else "SpecLobeA2(%s, specVar)" % X, X),
        "float specE = SpecEnv(specA2, max(dot(specN, specV), 1e-4));",
        cav,
        "if (specCtl.x == 1.0) {",
        "    %s = (specCav * specE).%s;  // S" % (s, "x" * (len(s) - s.index(".") - 1))]
    if an["X"] != an["Y"]:
        code.append("    %s = SpecMip(specA2);  // IBL mip" % Y)
    code.append("}")
    ins.add(an["exp"] + 1, after="\n".join(code))
    # The IBL about the highlight's normal (specN: the normal map at the mip bias; the variance the bias removed is in the
    # lobes and so in their cube mip) when eyeGeo.y != 0 (skin draws; 0 = VaM's W, as before). VaM reflects about W, the
    # full-resolution normal bent by _SpecularBumpiness (1 .. 2.2 on presets): pores turned the lookup by 10 - 30 degrees
    # on top of the mip that already counts them - single pores caught the environment's light, unlike the light's.
    # W is swapped in before the first read of it that makes the cube's direction and put back after the sample; every
    # read of W in between must make that direction.
    kc = an["kcube"]
    if nm and kc is not None and not p.instrs[kc].depth:
        dop = p.instrs[kc].ops[1]
        sl = set(slice_of(p, kc, dop.index, comps3(dop)))
        wr, wc = an["Wreg"], an["Wcomps"]
        reads_w = lambda m: any(o.kind == "r" and o.index == wr and any("xyzw".index(c) in wc for c in o.sel) for o in sources(m))
        kws = sorted(k for k in sl if reads_w(p.instrs[k]))
        if (not kws or kws[0] <= an["exp"] + 1 or any(p.instrs[k].depth for k in kws) or
                any(reads_w(p.instrs[k]) and k not in sl for k in range(kws[0], kc)) or
                any((wr, c) in writes(p.instrs[k]) for k in range(kws[0], kc + 1) for c in wc)):
            raise Fail("IBL direction: W read outside the cube's direction before the sample")
        ins.add(kws[0], before="float3 specIblW = %s;\nif (specCtl.x == 1.0 && eyeGeo.y != 0.0) %s = specN;  // the IBL about the highlight's normal" % (
            an["W"], an["W"]))
        ins.add(kc, after="%s = specIblW;  // VaM's W again" % an["W"])
    ins.add(an["nl"], before="float3 specL = %s;\nfloat3 skinDn = %s;  // the diffuse normal\nfloat scatX = dot(%s, %s);  // the diffuse N.L before VaM saturates it" % (
        an["L"], an["Nd"], an["dl_ops"][0], an["dl_ops"][1]), after="float specNdotL = %s;" % D)
    sd, F = an["subd"], an["F"]
    sine = "SkinLightSine(%s, eyeP)" % an["lightpos"] if an.get("lightpos") else "0.0"  # eyeP: the world position, kept at the top
    ins.add(an["kF"], after="\n".join([
        "if (scatCtl.x == 3.0) {  // 0.19.0: the irradiance; past the terminator the subsurface pass carries the light",
        "    %s = (1.0 - %s * 0.5) * SkinIrradiance(scatX, %s);" % (F, sd, sine),
        "} else if (scatCtl.x == 1.0) {  // skin wrap: VaM's own factor, tinted and continued past the terminator",
        "    %s = %s * ScatterTint(scatX);" % (F, F),
        "} else if (scatCtl.x == 2.0) {  // test: VaM's own diffuse factor from the captured N.L",
        "    float scatD = saturate(scatX);",
        "    float scatH = (scatD + 1.0) * 0.5;",
        "    %s = (1.0 - %s * 0.5) * saturate(%s * 0.5 + (1.0 - %s * 0.5) * scatD) * (scatH * scatH);" % (F, sd, sd, sd),
        "}",
        "if (scatCtl.y > 0.0) %s = OverallTranslucency(%s, scatX, %s, regionalResponse);" % (F, F, sd),
        "float3 skinDiffuse0 = %s;  // before the grazing factor: the translucency is measured against it" % F,
        "if (eyeGeo.z != 0.0 && scatCtl.x == 3.0 && specCtl.x == 1.0) %s *= SkinGrazing(skinDn, specL, specV, specA2);  // candidate" % F,
        "if (teethCtl.w != 0.0) {  // teeth: VaM's factor with their own Subsurface Color, times a gain",
        "    float teethD = saturate(scatX);",
        "    float teethH = (teethD + 1.0) * 0.5;",
        "    float teethS = teethCtl.y;",
        "    %s = ((1.0 - teethS * 0.5) * saturate(teethS * 0.5 + (1.0 - teethS * 0.5) * teethD) * (teethH * teethH) * teethCtl.z).xxx;" % F,
        "}"]))
    mt = an["maintex"]
    ins.add(mt["ksample"], before="float2 teethUv = %s; float regionalResponse=RegionalResponse(teethUv); // main texture UV" % mt["uv"])
    ins.add(an["ret"], before="\n".join([
        "float mouthShade = teethCtl.x != 0.0 ? TeethOcclusion(teethUv) : 1.0;",
        "if(mouthCtl.z!=0.0) {",
        "    float mouthVis=MouthVisibility(teethUv);",
        "    float cavity=0.06+0.94*smoothstep(0.18,0.62,mouthVis);",
        "    if(teethCtl.w!=0.0) {",
        "        float toothBack=teethTex.SampleLevel(teethSmp,teethUv*teethUV.xy+teethUV.zw,0).x;",
        "        float frontTooth=1.0-smoothstep(0.08,0.30,toothBack);",
        "        float toothContact=0.32+0.68*smoothstep(0.03,0.48,mouthVis);",
        "        cavity=lerp(cavity,toothContact,0.70*frontTooth);",
        "    }",
        "    float exposed=smoothstep(0.30,0.65,mouthVis);",
        "    if(teethCtl.w!=0.0 || mouthCtl.y>0.0) mouthShade=lerp(mouthShade,sqrt(mouthShade),0.65*exposed*mouthCtl.x);",
        "    mouthShade=lerp(mouthShade,min(mouthShade,cavity),mouthCtl.x);",
        "}",
        "o0.xyz *= mouthShade;  // geometric cavity darkening includes all lighting; never stacks the old depth factor twice",
        "if(mouthCtl.z==2.0) {o0.xyz=0.15*MouthVisibility(teethUv).xxx;mouthShade=0.0;} // review-only visibility view",
        ("if(scatCtl.z==2.0) {o0.xyz=saturate((regionalResponse-.5)/.58).xxx;mouthShade=0.0;} // linear scalar preview" if base else
         "if(scatCtl.z==2.0) {o0.xyz=0.0;mouthShade=0.0;} // do not add the preview per light"),
        ("if(shadowCtl.w==2.0) {o0.xyz=0.0;mouthShade=0.0;} // tests: the shadow radius view comes from the light passes" if base else
         "if(shadowCtl.w==2.0) {o0.xyz=0.15*gShadowR.xxx;mouthShade=0.0;} // tests: the shadow radius over VaM's"),

    ]))
    ins.add(an["ret"], before="\n".join([
        "if (eyeCtl.x + eyeCtl.y != 0.0) {  // eyes: cornea catchlight, eyelid occlusion",
        "    float4 eyeC0 = eyePts.Load(0), eyeG0 = eyePts.Load(1), eyeC1 = eyePts.Load(2), eyeG1 = eyePts.Load(3);",
        "    float3 eyeD0 = eyeP - eyeC0.xyz, eyeD1 = eyeP - eyeC1.xyz;",
        "    bool eyeB = dot(eyeD1, eyeD1) < dot(eyeD0, eyeD0);",
        "    float3 eyeC = eyeB ? eyeC1.xyz : eyeC0.xyz, eyeG = eyeB ? eyeG1.xyz : eyeG0.xyz, eyeQ = eyeP - eyeC;",
        "    float eyeR = eyeB ? eyeG1.w : eyeG0.w;",
        "    if (eyeR > 0.0 && dot(eyeQ, eyeQ) < 1.69 * eyeR * eyeR) {",
        "        if (eyeCtl.y != 0.0) o0.xyz += EyeCatch(EyeCatchNormal(eyeP, eyeC, eyeG, eyeR), specL, specV) * eyeLight;",
        "        if (eyeCtl.x != 0.0) o0.xyz *= EyeOcclusion(eyeP, eyeC, eyeR, eyeB ? 4 + (uint)eyeCtl.z : 4, eyeB ? (uint)eyeCtl.w : (uint)eyeCtl.z);",
        "    }",
        "}"]))
    ins.add(an["nh_exp"], after="\n".join([
        "if (specCtl.x == 1.0) {",
        "    float specM = min(specNdotL * 10.0, 1.0) * 0.5 * mad(%s, 0.15915494, 0.31830987) * specE;" % X,
        "    float specS = %s;  // the light's disc (0.22.20)" % sine,
        "    float specF = SpecDirect(specA2, specN, specL, specV, specS) + (skinLight.x > 0.0 ? SkinSheen(specN, specL, specV) : 0.0);",
        "    %s = specM > 0.0 ? specF / specM : 0.0;" % Aa,
        "} else if (specCtl.x == 2.0) {  // test: VaM's N.H power from the rebuilt vectors",
        "    %s = exp2(log2(saturate(dot(specN, normalize(specL + specV)))) * %s);" % (Aa, X),
        "}"]))
    return ins


EYE_GATHER = """// the eye rig of one character this frame (the module runs it before the character's first eye draw of a frame):
// each eye's frame from four vertices of the sclera's front ring, then the eyelid contact points, moved onto the eyeball
cbuffer EyeRig : register(b0) {
    uint4 rigCount;          // x points of eye 0, y of eye 1, z base vertex
    uint4 rigRing[2];        // the front ring's four vertices per eye
    float4 rigLocal[4];      // per eye: the centre in the ring frame (xyz, bind metres), |r0 - r2| in the bind pose; the gaze
                             // in the frame (xyz), the radius in the bind pose
    float4 rigPoint[%d];    // asuint(x), asuint(y): the edge's vertices, z: fraction along it, w: upper-lid share (+ 2: starts)
};
StructuredBuffer<float3> verts : register(t0);  // the skin's vertices this frame (world space)
RWBuffer<float4> outPts : register(u0);
groupshared float4 frameC[2], frameG[2];

[numthreads(64, 1, 1)]
void main(uint tid : SV_GroupIndex) {
    uint base = rigCount.z;
    if (tid < 2) {
        uint4 r = rigRing[tid];
        float3 r0 = verts[r.x + base], r1 = verts[r.y + base], r2 = verts[r.z + base], r3 = verts[r.w + base];
        float3 d = r0 - r2;
        float span = length(d);
        float3 ex = d / max(span, 1e-20);
        float3 ez = normalize(cross(d, r1 - r3));
        float3 ey = cross(ez, ex);
        float4 lc = rigLocal[2 * tid], lg = rigLocal[2 * tid + 1];
        float s = span / max(lc.w, 1e-20);
        float3 o = (r0 + r1 + r2 + r3) * 0.25;
        frameC[tid] = float4(o + (ex * lc.x + ey * lc.y + ez * lc.z) * s, s);
        frameG[tid] = float4(normalize(ex * lg.x + ey * lg.y + ez * lg.z), lg.w * s);
        outPts[2 * tid] = frameC[tid];
        outPts[2 * tid + 1] = frameG[tid];
    }
    GroupMemoryBarrierWithGroupSync();
    uint n = rigCount.x + rigCount.y;
    for (uint k = tid; k < n; k += 64) {
        float4 q = rigPoint[k];
        uint e = k < rigCount.x ? 0 : 1;
        float3 p = lerp(verts[asuint(q.x) + base], verts[asuint(q.y) + base], q.z);
        float3 c = frameC[e].xyz;
        float3 dp = p - c;
        float l = length(dp);
        outPts[4 + k] = float4(l > 0.0 ? c + dp * (frameG[e].w / l) : p, q.w);
    }
}
"""
EYE_POINTS = 128  # both eyes (bake.h kEyePoints each)

NORMAL_SLOPE = """// a face normal map's pore-scale slope (0.13.0, the plugin's starting step): every texel's tangent-space slope
// (Unity's UnpackNormalmapRGorAG x, y) against its own aligned 8 x 8 block's mean; a thread per block, each group's
// sum of squares (64 blocks) to its own element
cbuffer Slope : register(b0) {
    uint4 slopeSize;  // x, y blocks across / down, z groups across
};
Texture2D<float4> nmap : register(t0);
RWBuffer<float> sums : register(u0);
groupshared float part[64];

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    uint2 blk = gid.xy * 8 + tid.xy;
    float v = 0.0;
    if (blk.x < slopeSize.x && blk.y < slopeSize.y) {
        float2 m = 0.0;
        float q = 0.0;
        for (uint j = 0; j < 64; j++) {
            float4 t = nmap.Load(int3(blk * 8 + uint2(j & 7, j >> 3), 0));
            float2 xy = float2(t.x * t.w, t.y) * 2.0 - 1.0;
            m += xy;
            q += dot(xy, xy);
        }
        v = max(q - dot(m, m) / 64.0, 0.0);
    }
    part[gi] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 32; k > 0; k >>= 1) {
        if (gi < k) part[gi] += part[gi + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) sums[gid.y * slopeSize.z + gid.x] = part[0];
}
"""


def member(e, cbname, name):
    for cb in e["cbuffers"]:
        if cb["name"] == cbname and name in cb["members"]:
            off = cb["members"][name][0]
            return e["cbBind"][cbname], off // 16, (off % 16) // 4
    return None


def main():
    cases_dir = S.gen_dir("spec_cases")
    progs = {}
    for name, summ, blobs, pid in S.load(FAMILIES):
        for ss in summ["subshaders"]:
            for ps in ss["passes"]:
                lm = ps["tags"].get("LIGHTMODE")
                if lm not in ("FORWARDBASE", "FORWARDADD"):
                    continue
                for e in ps["stages"].get("progFragment", []):
                    if "MARMO_LINEAR" not in e["keywords"]:
                        continue
                    dx = blobs[e["blob"]]
                    h = hashlib.sha1(dx).hexdigest()[:16]
                    if h not in progs:
                        progs[h] = {"dx": dx, "e": e, "lm": lm, "names": set()}
                    progs[h]["names"].add(name.split("/")[-1])
    rows, manifest, report = [], [], []
    rr_codes = {}
    for h, pr in sorted(progs.items()):
        e = pr["e"]
        meta = {"lightpos": member(e, "UnityLighting", "_WorldSpaceLightPos0")[:2],
                "lightcolor": member(e, "$Globals", "_LightColor0")[:2],
                "subdermis": member(e, "$Globals", "_SubdermisColor")[:2],
                "speccolor": member(e, "$Globals", "_SpecColor")[:2],
                "specint": member(e, "$Globals", "_SpecInt"),
                "glossoff": member(e, "$Globals", "_GlossOffset"),
                "shininess": member(e, "$Globals", "_Shininess"),
                "maintex": e["textures"]["_MainTex"][0],
                "campos": (member(e, "UnityPerCamera", "_WorldSpaceCameraPos") or (None, None))[:2],
                "stereocampos": (member(e, "UnityStereoGlobals", "unity_StereoWorldSpaceCameraPos") or (None, None))[:2]}
        if meta["campos"][0] is None:
            meta["campos"] = None
        if meta["stereocampos"][0] is None:
            meta["stereocampos"] = None
        p = A.lift(pr["dx"])
        if max(p.cbs) >= CTL_SLOT:
            raise SystemExit("%s uses constant buffer slot %d" % (h, max(p.cbs)))
        if (p.textures and max(p.textures) >= 16) or (p.samplers and max(p.samplers) >= 15):
            raise SystemExit("%s uses texture or sampler slots the teeth occlusion or the eyes need" % h)
        try:
            an = find_anchors(p, meta)
        except Fail as x:
            raise SystemExit("%s (%s %s): %s" % (h, pr["lm"], " ".join(e["keywords"]), x))
        try:
            ss = find_sss(p, an)
        except Fail as x:
            ss = None
            print("%s (%s %s): no subsurface outputs: %s" % (h, pr["lm"], " ".join(e["keywords"]), x))
        try:
            det = find_detail(p, an, meta)
        except Fail as x:
            det = None
            print("%s (%s %s): no skin detail changes: %s" % (h, pr["lm"], " ".join(e["keywords"]), x))
        lp_ = member(e, "UnityLighting", "_WorldSpaceLightPos0")
        an["lightpos"] = "cb%d[%d]" % (lp_[0], lp_[1]) if lp_ else None
        inserts = patch(p, an, det, pr["lm"] == "FORWARDBASE")
        ambient = patch_nasal_ambient(p, inserts, an, e, pr["lm"] == "FORWARDBASE")
        if det:
            patch_detail(inserts, an, det)
        if ss:
            patch_sss(inserts, an, ss)
        try:
            shadow = find_shadow(p, e, e["keywords"], an) if pr["lm"] == "FORWARDADD" else None
        except Fail as x:
            raise SystemExit("%s (%s %s): %s" % (h, pr["lm"], " ".join(e["keywords"]), x))
        if shadow:
            patch_shadow(inserts, shadow)
        try:
            patch_env_lowlight(p, inserts, an, det, ambient, pr["lm"] == "FORWARDBASE")
        except Fail as x:
            raise SystemExit("%s (%s %s): %s" % (h, pr["lm"], " ".join(e["keywords"]), x))
        # precise temporaries stop fxc from reordering arithmetic (VaM's shadow noise hash turns a 1-ulp change into a
        # different PCF rotation); only for ps_4_0, whose bytecode has no precise bit: in ps_5_0 it reaches the driver,
        # which then stops fusing mul + add the way it does for the original
        src = p.hlsl(inserts, HEADER + (SSS_HEADER if ss else ""), precise=p.target == "ps_4_0")
        if ss:
            src = sss_source(src, ss)
        code = S.compile_hlsl(src, "main", p.target, name="spec_%s.hlsl" % h)
        isgn = S.signature(code, b"ISGN")
        if [x[:5] for x in isgn] != [x[:5] for x in S.signature(pr["dx"], b"ISGN")]:
            raise SystemExit("%s: input signature differs" % h)
        if len(S.signature(code, b"OSGN")) != (5 if ss else 1):
            raise SystemExit("%s: output signature %s" % (h, S.signature(code, b"OSGN")))
        if ss:
            # RR uses seven outputs. Detail restoration reuses this draw's
            # unlit albedo; do not shade a second, unoccluded reference image.
            rr = src.replace('float4 t : SV_Target4;', 'float4 n : SV_Target5; float4 s : SV_Target6; float4 t : SV_Target4;')
            rr = rr.replace('float z, float regional) {', 'float z, float regional, float3 normal, float rough, float specular) {')
            rr = rr.replace('o.c = c;', 'o.c = c; o.n = float4(normal, rough); o.s = float4(specular.xxx, 1);')
            tail = rr.rfind('regionalResponse);')
            assert tail >= 0
            rr = rr[:tail] + 'regionalResponse, specN, sqrt(sqrt(max(lerp(specA2.y, specA2.x, specLobe.w), 1e-8))), saturate(specE));' + rr[tail+len('regionalResponse);'):]
            rr_codes[h] = S.compile_hlsl(rr, "main", p.target, name="rr_%s.hlsl" % h)
            assert len(S.signature(rr_codes[h], b"OSGN")) == 7
            open(os.path.join(cases_dir, h + ".rr.hlsl"), "w", encoding="utf-8").write(rr)
        rows.append((h, code, 1 if ss else 0, det["gloss"]["tex"] if det else -1))
        # test case: original, replacement, a vertex shader with the same outputs, the bindings
        open(os.path.join(cases_dir, h + ".orig.dxbc"), "wb").write(pr["dx"])
        open(os.path.join(cases_dir, h + ".repl.dxbc"), "wb").write(code)
        open(os.path.join(cases_dir, h + ".hlsl"), "w", encoding="utf-8").write(src)
        open(os.path.join(cases_dir, h + ".vs.dxbc"), "wb").write(S.compile_hlsl(test_vs(p.sig), "main", "vs_4_0", name="vs_%s.hlsl" % h))
        eye = member(e, "UnityStereoEyeIndex", "unity_StereoEyeIndex")
        cmp_t = sorted({i.ops[2].index for i in p.instrs if i.op == "sample_c_lz"})
        parts = [h, p.target, "cb", str(len(p.cbs))] + ["%d:%d" % kv for kv in sorted(p.cbs.items())]
        parts += ["eye", "%d:%d:%d" % eye if eye else "-"]
        parts += ["tex", str(len(p.textures))] + ["%d:%s:%d" % (t, d, 1 if t in cmp_t else 0) for t, d in sorted(p.textures.items())]
        parts += ["smp", str(len(p.samplers))] + ["%d:%d" % (s_, 1 if c else 0) for s_, c in sorted(p.samplers.items())]
        parts += ["subd", "%d:%d" % meta["subdermis"]]
        # subsurface outputs: 1 / 0, then where _LightColor0 and _SpecColor are (tests\sss_test)
        lc = member_any(e, "_LightColor0")
        parts += ["sss", "1" if ss else "0", "light", "%d:%d" % lc if lc else "-", "speccolor", "%d:%d" % meta["speccolor"]]
        ibl, lm = member_any(e, "_ExposureIBL"), member_any(e, "_ExposureLM")
        parts += ["aoenv", "%d:%d" % ibl if ambient else "-", "%d:%d" % lm if ambient else "-"]
        parts += ["aofilter", "%d:%d:%d" % tuple(ambient["filter"]) if ambient else "-"]
        manifest.append(" ".join(parts))
        report.append({"hash": h, "pass": pr["lm"], "keywords": e["keywords"], "shaders": sorted(pr["names"]),
                       "anchors": an, "sss": ss, "detail": det, "nasal_ambient": ambient,
                       "shadow": shadow["kind"] if shadow else None,
                       "instructions": len(p.instrs)})
    rows.sort(key=lambda r: r[0])
    open(os.path.join(cases_dir, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
    open(os.path.join(cases_dir, "spec_brdf.hlsl"), "w").write(BRDF)
    json.dump(report, open(os.path.join(S.gen_dir(), "spec_table.json"), "w"), indent=1)
    out = os.path.join(S.gen_dir(), "spec_programs.h")
    with open(out, "w") as f:
        f.write("// generated by tools\\gen_spec_table.py: VaM's skin pixel programs with the specular changes\n")
        f.write("// (key = first 8 bytes of the original's SHA-1, value = the replacement's bytecode)\n#pragma once\n#include <stdint.h>\n")
        f.write("// sss: the replacement also writes the subsurface targets (SV_Target1..3); gloss: its gloss map's texture slot\n")
        f.write("// (the skin detail's t18 stats are that texture's; -1 = no skin detail)\n")
        f.write("struct SpecProgram { uint64_t hash; const unsigned char *code; unsigned size; int sss; int gloss; const unsigned char *rr; unsigned rrSize; };\n")
        for i, (h, code, sss, gl) in enumerate(rows):
            f.write("static const unsigned char kSpec%d[%d] = {" % (i, len(code)))
            f.write(",".join(str(b) for b in code))
            f.write("};\n")
        for i, (h, *_) in enumerate(rows):
            if h in rr_codes:
                c = rr_codes[h]
                f.write("static const unsigned char kRr%d[%d] = {%s};\n" % (i, len(c), ",".join(str(b) for b in c)))
        f.write("static const SpecProgram kSpecPrograms[] = {\n")
        for i, (h, code, sss, gl) in enumerate(rows):
            f.write("    {0x%sull, kSpec%d, %d, %d, %d, %s, %d},\n" % (h, i, len(code), sss, gl, "kRr%d" % i if h in rr_codes else "nullptr", len(rr_codes.get(h,b""))))
        f.write("};\nstatic const int kSpecProgramCount = %d;\n" % len(rows))
        cs = S.compile_hlsl(EYE_GATHER % EYE_POINTS, "main", "cs_5_0", name="eye_gather.hlsl")
        f.write("// the eye gather compute shader (cs_5_0); its constant buffer holds %d points\n" % EYE_POINTS)
        f.write("static const int kEyeGatherPoints = %d;\n" % EYE_POINTS)
        f.write("static const unsigned char kEyeGatherCS[%d] = {%s};\n" % (len(cs), ",".join(str(b) for b in cs)))
        cs = S.compile_hlsl(NORMAL_SLOPE, "main", "cs_5_0", name="normal_slope.hlsl")
        f.write("// the normal slope compute shader (cs_5_0, 0.13.0): 8 x 8 groups of 8 x 8 texel blocks\n")
        f.write("static const unsigned char kNormalSlopeCS[%d] = {%s};\n" % (len(cs), ",".join(str(b) for b in cs)))
    print("%d programs (%d with subsurface outputs) -> %s; test cases in %s" % (len(rows), sum(r[2] for r in rows), out, cases_dir))


def test_vs(sig):
    """vertex shader for tests\\spec_test: a 32 x 32 grid of quads over the target; every output a random value per
    grid corner (0.05 .. 0.95), linear across each quad"""
    L = ["struct VSOut {"]
    for i, (sem, idx, reg, mask, sv, ct) in enumerate(sig):
        name = "SV_POSITION" if sem == "SV_POSITION" else "%s%d" % (sem, idx)
        L.append("    %s e%d : %s;" % (A.HLSL_TYPES[mask], i, name))
    L.append("};")
    L.append("float rnd(float2 c, float k) { return 0.05 + 0.9 * frac(sin(dot(c, float2(12.9898, 78.233)) + k * 37.719) * 43758.5453); }")
    L.append("VSOut main(uint id : SV_VertexID) {")
    L.append("    uint q = id / 6, v = id % 6;")
    L.append("    uint2 cell = uint2(q % 32, q / 32);")
    L.append("    static const uint2 corner[6] = {uint2(0,0), uint2(1,0), uint2(0,1), uint2(0,1), uint2(1,0), uint2(1,1)};")
    L.append("    float2 g = float2(cell + corner[v]);")
    L.append("    VSOut o;")
    for i, (sem, idx, reg, mask, sv, ct) in enumerate(sig):
        n = bin(mask).count("1")
        if sem == "SV_POSITION":
            L.append("    o.e%d = float4(g / 16.0 - 1.0, 0.5, 1.0);" % i)
        else:
            vals = ", ".join("rnd(g, %d.0)" % (4 * i + c) for c in range(n))
            L.append("    o.e%d = %s(%s);" % (i, A.HLSL_TYPES[mask], vals))
    L.append("    return o;")
    L.append("}")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    main()
