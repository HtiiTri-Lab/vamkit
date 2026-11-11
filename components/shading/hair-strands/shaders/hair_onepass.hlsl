// VaM hair one-pass lighting: GPUTools/MeshedVR/HairOpt's ForwardBase pass and its ForwardAdd passes (one per further
// pixel light) in one draw. The original draws the whole hair (VS -> HS isolines -> DS strand point and lighting ->
// GS quad per segment -> PS) once per light; here the strand point, the tessellation, the quads and the rasterisation
// are done once, the DS lights the point with the main light and every further light, and the PS adds each light's
// attenuation and shadow.
//
// Compiled per light count (HOP_LIGHTS 0..8) and with / without the screen position interpolant (HOP_SCREEN, for the
// main light's screen-space shadows and directional lights with screen shadows). Everything comes from HopItem
// (filled by the plugin from the original draws' constants) and the original resources: the DS stage keeps the
// original DS's texture / buffer / sampler slots; the PS slots are this shader's own.
//
// The arithmetic follows the original DXBC instruction by instruction (the strand point and shading code is
// OpenOVR\Compositor\hair\hair_native.hlsl's, checked against the original by tools\hair-compare there), so a light's
// contribution is the original pass's up to float rounding; the original also rounds every pass to the 8-bit target.
// One deliberate difference: shadow-map coordinates (spot) and light vectors (point shadows) are computed per pixel
// from the interpolated world position instead of per vertex; both are affine in the position.
//
// HOP_BASE 0: the further lights only (what the plugin draws): Unity's ForwardBase draw stays as it is, and this draw
// replaces the item's ForwardAdd draws where they were, with their state (additive, depth equal); each light limited to
// its ForwardAdd draw's scissor rectangle (HopLight.kind.zw), the draw's own being their union. HOP_BASE 1: ambient,
// main light and lights in one draw (the offline comparison with the original passes).

#ifndef HOP_LIGHTS
#define HOP_LIGHTS 0
#endif
#ifndef HOP_SCREEN
#define HOP_SCREEN 0
#endif
#ifndef HOP_BASE
#define HOP_BASE 1
#endif
#define HOP_MAX_LIGHTS 8

// HopLight.kind.x
#define HOP_LIGHT_DIRECTIONAL 0
#define HOP_LIGHT_POINT 1
#define HOP_LIGHT_SPOT 2
// HopLight.kind.y
#define HOP_SHADOW_NONE 0
#define HOP_SHADOW_DEPTH 1
#define HOP_SHADOW_DEPTH_SOFT 2
#define HOP_SHADOW_CUBE 3
#define HOP_SHADOW_CUBE_SOFT 4
#define HOP_SHADOW_SCREEN 5

// One further light: what its ForwardAdd pass had in $Globals / UnityLighting / UnityShadows. Matrices as stored by
// Unity (one register per column).
struct HopLight {
    float4 pos;               // _WorldSpaceLightPos0
    float4 color;             // _LightColor0
    float4 worldToLight[4];   // unity_WorldToLight
    float4 worldToShadow[4];  // unity_WorldToShadow[0]
    float4 shadowData;        // _LightShadowData
    float4 positionRange;     // _LightPositionRange
    float4 projectionParams;  // _LightProjectionParams
    uint4 kind;               // x HOP_LIGHT_*, y HOP_SHADOW_*, z left | right << 16, w top | bottom << 16 (HOP_BASE 0)
};

// One hair item (eye independent). Layout shared with the plugin (hop_item.h).
cbuffer HopItem : register(b0) {
    float4 _TessFactor;
    float4 _Length;
    float4 hMat0;  // _MaxSpread _RandomBarycentric _StandWidth _Diffuse
    float4 hMat1;  // _FresnelPower _FresnelAtten _SpecularShift _PrimarySpecular
    float4 hMat2;  // _SecondarySpecular _RandomTexColorPower _RandomTexColorOffset _IBLFactor
    float4 _SpecularColor;
    float4 _ExposureIBL;
    float4 _ExposureLM;
    float4 hSkyMatrix[4];
    float4 hSH[9];
    float4 glstate_lightmodel_ambient;
    float4 _WorldSpaceLightPos0;  // the main light (ForwardBase)
    float4 _LightColor0;
    float4 hObjectToWorld[4];
    float4 hWorldToObject[4];
    float4 hStereoMatrixVP[8];
    float4 hStereoCameraPos[2];
    float4 hStereoScaleOffset[2];
    float4 _ProjectionParams;
    uint4 hFlags;  // x lights, y 1 = the main light has screen-space shadows
    HopLight hLights[HOP_MAX_LIGHTS];
};
cbuffer HopEye : register(b1) {
    uint4 hEye;  // x eye index
};

#define _MaxSpread hMat0.x
#define _RandomBarycentric hMat0.y
#define _StandWidth hMat0.z
#define _Diffuse hMat0.w
#define _FresnelPower hMat1.x
#define _FresnelAtten hMat1.y
#define _SpecularShift hMat1.z
#define _PrimarySpecular hMat1.w
#define _SecondarySpecular hMat2.x
#define _RandomTexColorPower hMat2.y
#define _RandomTexColorOffset hMat2.z
#define _IBLFactor hMat2.w

struct TessRenderParticle {
    float3 Position;
    float3 Velocity;
    float3 LightCenter;
    float3 Color;
    float Interpolation;
    int RootIndex;
};

// DS stage: the original ForwardBase DS's slots.
Texture2D<float4> _NoiseTex : register(t0);
Texture2D<float4> _ColorTex : register(t1);
StructuredBuffer<TessRenderParticle> _Particles : register(t2);
StructuredBuffer<float3> _Barycentrics : register(t3);
SamplerState sampler_ColorTex : register(s0);
SamplerState sampler_NoiseTex : register(s1);

// PS stage.
Texture2D<float4> gRotation : register(t0);       // unity_RandomRotation16
Texture2D<float4> gScreenShadow : register(t1);   // the main light's screen-space shadows
Texture2D<float4> gCookie[HOP_MAX_LIGHTS] : register(t2);      // spot cookie (_LightTexture0)
Texture2D<float4> gAtten[HOP_MAX_LIGHTS] : register(t10);      // attenuation (point _LightTexture0, spot _LightTextureB0)
Texture2D<float4> gShadow[HOP_MAX_LIGHTS] : register(t18);     // spot shadow map / directional screen-space shadows
TextureCube<float4> gShadowCube[HOP_MAX_LIGHTS] : register(t26);
SamplerState sRotation : register(s0);
SamplerState sScreenShadow : register(s1);
SamplerState sCookie : register(s2);
SamplerState sAtten : register(s3);
SamplerComparisonState sShadowCompare : register(s4);
SamplerState sShadowRaw : register(s5);
SamplerState sDirectionalScreen : register(s6);

// The shadow filters' sample offsets (xy blocker search, zw filter), bit for bit the original's.
static const float4 kHopPoisson[25] = {
    float4(0.123298101, -0.0392337516, -0.635181785, 0.217271104),
    float4(-0.56253773, -0.360242814, -0.149960607, 0.232067496),
    float4(0.640371919, 0.0682112277, -0.679780006, 0.688492417),
    float4(0.281338692, -0.588158786, -0.77586472, -0.253408998),
    float4(-0.573121786, 0.270057201, -0.473191589, -0.283272296),
    float4(0.203316599, 0.419773906, -0.333007902, 0.643005908),
    float4(0.846795797, -0.354558408, -0.138415098, -0.0983022526),
    float4(-0.423045099, -0.797441006, -0.818232715, -0.564593911),
    float4(0.719025314, 0.569357514, -0.91984719, 0.0654980168),
    float4(0.0381546803, -0.99141711, -0.142208502, -0.4872109),
    float4(-0.223626494, 0.502861381, -0.498083293, -0.588559926),
    float4(0.172225401, 0.983663023, -0.332615912, -0.849614799),
    float4(-0.291246414, 0.898051202, 0.306673586, -0.140199706),
    float4(-0.898414791, -0.0876278579, 0.114831701, 0.374455005),
    float4(-0.699508488, 0.673418522, -0.0388568006, 0.8071329),
    float4(-0.293195993, -0.0628911927, 0.410288513, 0.696029484),
    float4(0, 0, 0.556387722, 0.337537706),
    float4(0, 0, -0.0178657603, -0.887376487),
    float4(0, 0, 0.234990999, -0.455843806),
    float4(0, 0, 0.620677471, -0.155100495),
    float4(0, 0, 0.664064229, -0.569142699),
    float4(0, 0, 0.731272578, 0.583016813),
    float4(0, 0, 0.887970686, 0.0571521297),
    float4(0, 0, 0.312829614, -0.830802977),
    float4(0, 0, 0.868976414, -0.339797288),
};

// ---- strand point (as the original DS) --------------------------------------------------------------------------------

struct HairStrandPoint {
    float3 pos;
    float3 tangent;
    float3 normal;  // from the strand's light centre
    float3 color;   // particle colour before the noise gradient
};

float3 HairSpread(float3 root, float3 p) {
    float3 d = p - root;
    float len = sqrt(dot(d, d));
    return d * (min(len, _MaxSpread) / max(len, 0.0001)) + root;
}

float3 HairInterpolate(int i0, int i1, int i2, float3 b, float interpolation) {
    float3 p0 = _Particles[i0].Position;
    float3 q1 = HairSpread(p0, _Particles[i1].Position);
    float3 q2 = HairSpread(p0, _Particles[i2].Position);
    float3 bary = q1 * b.y;
    bary = p0 * b.x + bary;
    bary = q2 * b.z + bary;
    float3 sum = q1 + p0;
    sum = sum + q2;
    return interpolation * (sum * (1.0 / 3.0) - bary) + bary;
}

HairStrandPoint HairComputePoint(uint3 guides, float u, float v) {
    float segments = _TessFactor.y;  // particles per guide strand
    float t = (segments - 0.001) * u;
    float3 b0 = _Barycentrics[(uint)(v * 64.0)];
    t = t * dot(_Length.xyz, b0);
    float segment = trunc(t);
    int seg = (int)t;
    int i0 = (int)((float)guides.x * segments + segment);
    int i1 = (int)((float)guides.y * segments + segment);
    int i2 = (int)((float)guides.z * segments + segment);
    TessRenderParticle root = _Particles[i0];
    float3 b1 = _Barycentrics[(int)(((float)root.RootIndex + v) * 64.0)];
    float3 b = _RandomBarycentric * (b1 - b0) + b0;
    HairStrandPoint s;
    s.pos = HairInterpolate(i0, i1, i2, b, root.Interpolation);
    // Tangent toward the tip from the neighbouring particle (the next one at the root).
    bool first = seg == max(seg - 1, 0);
    int step = first ? 1 : -1;
    float3 n = HairInterpolate(i0 + step, i1 + step, i2 + step, b, _Particles[i0 + step].Interpolation);
    s.tangent = normalize(s.pos - n) * (first ? -1.0 : 1.0);
    float3 c = _Particles[i1].Color * b0.y;
    c = root.Color * b0.x + c;
    c = _Particles[i2].Color * b0.z + c;
    s.color = root.Interpolation * (root.Color - c) + c;
    s.normal = normalize(s.pos - root.LightCenter);
    return s;
}

// ---- shading (DS) --------------------------------------------------------------------------------------------------------

float3 HairNoiseColor(float3 color, float v) {
    float3 g = _NoiseTex.SampleLevel(sampler_NoiseTex, float2(0.5, v), 0).xyz;
    g = g + _RandomTexColorOffset;
    g = g + -1.0;
    g = _RandomTexColorPower * g + 1.0;
    return color * g;
}

// Direct light: two shifted Kajiya-Kay lobes, wrapped diffuse and fresnel. toCamera * invDistance + light is the
// original's unnormalised half vector; shift is the _ColorTex sample (the same for every light).
float3 HairDirect(HairStrandPoint s, float3 color, float3 toCamera, float invDistance, float3 view, float3 light, float3 lightColor,
                  float u, float shift) {
    float3 c = light.zxy * s.tangent - s.tangent.zxy * light;
    float3 ns = s.tangent.yzx * c.yzx - s.tangent.zxy * c;
    float3 t1 = (shift + _SpecularShift) * ns + s.tangent;
    float3 t2 = (shift - _SpecularShift) * ns + s.tangent;
    float nsl = saturate(dot(ns, light));
    t2 = normalize(t2);
    float3 h = normalize(toCamera * invDistance + light);
    float d2 = dot(t2, h);
    float specular2 = exp2(log2(sqrt(-d2 * d2 + 1.0)) * _PrimarySpecular);
    t1 = normalize(t1);
    float d1 = dot(t1, h);
    float specular1 = exp2(log2(sqrt(-d1 * d1 + 1.0)) * _SecondarySpecular);
    float3 specular = min(specular1 + specular2, 1.0) * _SpecularColor.xyz;
    float nl = saturate(dot(s.normal, light));
    float nv = saturate(dot(s.normal, view));
    specular = specular * nl;
    specular = nv * specular;
    float fresnel = exp2(log2(1.0 - nv) * _FresnelPower);
    specular = specular * saturate(u + u);
    float wrap = 1.0 - _Diffuse;
    nsl = saturate(nsl * wrap + _Diffuse);
    float diffuse = saturate(nl * wrap + _Diffuse);
    diffuse = saturate(fresnel * _FresnelAtten + diffuse);
    float3 lit = diffuse * lightColor;
    float3 base = nsl * color + specular;
    return lit * base * _ExposureIBL.w;
}

// Ambient: order-2 SH in _SkyMatrix space plus the fixed-function ambient.
float3 HairAmbient(float3 normal, float3 color) {
    float3 sy = normal.y * float3(hSkyMatrix[0].y, hSkyMatrix[1].y, hSkyMatrix[2].y);
    float3 sz = normal.z * float3(hSkyMatrix[0].z, hSkyMatrix[1].z, hSkyMatrix[2].z);
    float3 sx = normal.x * float3(hSkyMatrix[0].x, hSkyMatrix[1].x, hSkyMatrix[2].x);
    float3 n = sy + sz;
    n = n + sx;
    float3 sh = hSH[1].xyz * n.y + hSH[0].xyz;
    sh = hSH[2].xyz * n.z + sh;
    sh = hSH[3].xyz * n.x + sh;
    float3 q = n.xzx * n.yyz;
    sh = hSH[4].xyz * q.x + sh;
    sh = hSH[5].xyz * q.y + sh;
    sh = hSH[7].xyz * q.z + sh;
    float c8 = n.x * n.x - n.y * n.y;
    float c6 = n.z * n.z * 3.0 - 1.0;
    sh = hSH[6].xyz * c6 + sh;
    sh = hSH[8].xyz * c8 + sh;
    float3 fixedAmbient = (glstate_lightmodel_ambient.xyz + glstate_lightmodel_ambient.xyz) * _ExposureLM.x;
    sh = abs(sh) * (_ExposureIBL.x * _IBLFactor) + fixedAmbient;
    return color * sh * _ExposureIBL.w;
}

// ---- pipeline ----------------------------------------------------------------------------------------------------------

struct HopControlPoint {
    uint id : TEXCOORD0;
};
struct HopPatch {
    float f[2] : SV_TessFactor;
};

HopControlPoint HopVS(uint id : SV_VertexID) {
    HopControlPoint o;
    o.id = id;
    return o;
}

HopPatch HopPatchConstants(InputPatch<HopControlPoint, 3> p) {
    HopPatch o;
    o.f[0] = _TessFactor.x;
    o.f[1] = _TessFactor.y;
    return o;
}

[domain("isoline")]
[partitioning("integer")]
[outputtopology("line")]
[outputcontrolpoints(3)]
[patchconstantfunc("HopPatchConstants")]
HopControlPoint HopHS(InputPatch<HopControlPoint, 3> p, uint i : SV_OutputControlPointID) {
    return p[i];
}

struct HopPoint {
    float4 pos : SV_POSITION;  // world position (as the original DS)
    float3 side : TEXCOORD0;
#if HOP_BASE
    float3 ambient : TEXCOORD1;
    float3 direct : TEXCOORD2;
#endif
#if HOP_LIGHTS > 0
    float3 light[HOP_LIGHTS] : TEXCOORD3;
#endif
};

[domain("isoline")]
HopPoint HopDS(HopPatch pc, float2 uv : SV_DomainLocation, const OutputPatch<HopControlPoint, 3> p) {
    float u = uv.x, v = uv.y;
    HairStrandPoint s = HairComputePoint(uint3(p[0].id, p[1].id, p[2].id), u, v);
    HopPoint o;
    o.pos = float4(s.pos, 1.0);
    float3 toCamera = hStereoCameraPos[hEye.x].xyz - s.pos;
    float invDistance = rsqrt(dot(toCamera, toCamera));
    float3 view = invDistance * toCamera;
    o.side = normalize(cross(s.tangent, view)) * ((1.0 - saturate(u - 0.9) * 10.0) * _StandWidth);
    float3 color = HairNoiseColor(s.color, v);
    float shift = saturate(_ColorTex.SampleLevel(sampler_ColorTex, float2(v, u), 0).x - 0.5);
#if HOP_BASE
    o.direct = HairDirect(s, color, toCamera, invDistance, view, _WorldSpaceLightPos0.xyz, _LightColor0.xyz, u, shift);
    o.ambient = HairAmbient(s.normal, color);
#endif
#if HOP_LIGHTS > 0
    [unroll] for (int k = 0; k < HOP_LIGHTS; k++) {
        // Point and spot lights from the point, directional lights by direction (both normalised, as ForwardAdd).
        float3 l = hLights[k].kind.x == HOP_LIGHT_DIRECTIONAL ? hLights[k].pos.xyz : hLights[k].pos.xyz - s.pos;
        o.light[k] = HairDirect(s, color, toCamera, invDistance, view, normalize(l), hLights[k].color.xyz, u, shift);
    }
#endif
    return o;
}

struct HopVertex {
    float4 pos : SV_POSITION;
#if HOP_BASE
    float3 ambient : TEXCOORD0;
    float3 direct : TEXCOORD1;
#endif
    float3 world : TEXCOORD2;
#if HOP_SCREEN
    float4 screen : TEXCOORD3;
#endif
#if HOP_LIGHTS > 0
    float3 light[HOP_LIGHTS] : TEXCOORD4;
#endif
};

float4 HopViewProjection(float4 world) {
    int eye = (int)hEye.x;
    float4 c = hStereoMatrixVP[eye * 4 + 1] * world.y;
    c = hStereoMatrixVP[eye * 4 + 0] * world.x + c;
    c = hStereoMatrixVP[eye * 4 + 2] * world.z + c;
    return hStereoMatrixVP[eye * 4 + 3] * world.w + c;
}

// UnityObjectToClipPos(mul(unity_WorldToObject, world)), as the ForwardBase GS.
float4 HopClip(float3 wp) {
    float3 o = hWorldToObject[1].xyz * wp.y;
    o = hWorldToObject[0].xyz * wp.x + o;
    o = hWorldToObject[2].xyz * wp.z + o;
    o = o + hWorldToObject[3].xyz;
    float4 w = hObjectToWorld[1] * o.y;
    w = hObjectToWorld[0] * o.x + w;
    w = hObjectToWorld[2] * o.z + w;
    w = w + hObjectToWorld[3];
    return HopViewProjection(w);
}

#if HOP_SCREEN
float4 HopScreenPos(float4 clip) {
    float3 a = float3(0.5, _ProjectionParams.x, 0.5) * clip.xyw;
    float2 p = a.zz + float2(a.x, a.y * 0.5);
    float4 so = hStereoScaleOffset[hEye.x];
    p = p * so.xy + clip.ww * so.zw;
    return float4(p, clip.zw);
}
#endif

HopVertex HopEmit(HopPoint d, float sideSign) {
    HopVertex o;
    float3 wp = sideSign > 0.0 ? d.pos.xyz + d.side : d.pos.xyz - d.side;
    o.pos = HopClip(wp);
#if HOP_BASE
    o.ambient = d.ambient;
    o.direct = d.direct;
#endif
    o.world = wp;
#if HOP_SCREEN
    o.screen = HopScreenPos(o.pos);
#endif
#if HOP_LIGHTS > 0
    [unroll] for (int k = 0; k < HOP_LIGHTS; k++) o.light[k] = d.light[k];
#endif
    return o;
}

// One quad per segment, as the original: (p0 +side, p1 +side, p0 -side, p1 -side).
[maxvertexcount(4)]
void HopGS(line HopPoint p[2], inout TriangleStream<HopVertex> strip) {
    strip.Append(HopEmit(p[0], 1.0));
    strip.Append(HopEmit(p[1], 1.0));
    strip.Append(HopEmit(p[0], -1.0));
    strip.Append(HopEmit(p[1], -1.0));
}

// ---- lights (PS) ----------------------------------------------------------------------------------------------------------

// Rotation of the filter's sample offsets: as UnitySampleShadowmap's noise.
float2 HopRotate(float2 t, float sn, float cs) { return float2(t.y * sn + cs * t.x, t.x * -sn + cs * t.y); }

// Spot light shadow map: 25-tap PCF rotated per pixel (soft: radius from a 16-tap blocker search).
float HopShadowSpot(int k, float3 w, bool soft) {
    float4 sc = hLights[k].worldToShadow[1] * w.y;
    sc = hLights[k].worldToShadow[0] * w.x + sc;
    sc = hLights[k].worldToShadow[2] * w.z + sc;
    sc = sc + hLights[k].worldToShadow[3];
    float3 p = sc.xyz / sc.w;
    float3 n = gRotation.Sample(sRotation, p.xy * 10.0).xyz;
    float a = frac(sin(dot(n, float3(12.9898, 78.233, 45.5432))) * 43758.5469) * 10.0;
    float sn, cs;
    sincos(a, sn, cs);
    float radius = (1.0 - hLights[k].shadowData.x) * (soft ? 0.05 : 0.025);
    [branch] if (soft) {
        float blockers = 0.0;
        [loop] for (int i = 0; i < 16; i++)
            blockers += gShadow[k].SampleLevel(sShadowRaw, HopRotate(kHopPoisson[i].xy, sn, cs) * radius + p.xy, 0).x;
        float mean = blockers * 0.0625;
        radius = radius * ((p.z - blockers * 0.0625) / mean);
    }
    float sum = 0.0;
    [loop] for (int i = 0; i < 25; i++)
        sum += gShadow[k].SampleCmpLevelZero(sShadowCompare, HopRotate(kHopPoisson[i].zw, sn, cs) * radius + p.xy, p.z);
    return sum * 0.04;
}

// Distance to compare in a point light's shadow cube, from the light to the sample direction d.
float HopCubeDepth(int k, float3 d, float nearest) {
    float3 a = abs(d);
    float3 m = max(-(a * rsqrt(dot(a, a))) * hLights[k].projectionParams.z + a, 0.0);
    float z = max(m.z, max(m.y, m.x));
    z = max(z - nearest, 0.0);
    z = hLights[k].projectionParams.y / z;
    z = z - hLights[k].projectionParams.x;
    return 1.0 - z;
}

// Point light shadow cube: 25 taps in a plane across the light direction, rotated per pixel by noise at the
// pixel's screen position (soft: radius from a 16-tap blocker search).
float HopShadowCube(int k, float3 w, bool soft) {
    float3 toLight = w - hLights[k].positionRange.xyz;
    float3 back = toLight + hLights[k].positionRange.xyz;
    int eye = (int)hEye.x;
    float3 c = hStereoMatrixVP[eye * 4 + 1].xyw * back.y;
    c = hStereoMatrixVP[eye * 4 + 0].xyw * back.x + c;
    c = hStereoMatrixVP[eye * 4 + 2].xyw * back.z + c;
    c = c + hStereoMatrixVP[eye * 4 + 3].xyw;
    float2 half_ = float2(c.x * 0.5, (c.y * _ProjectionParams.x) * 0.5);
    float2 uv = c.zz * 0.5 + half_;
    float4 so = hStereoScaleOffset[eye];
    uv = uv * so.xy + c.zz * so.zw;
    uv = uv / c.z;
    float3 n = gRotation.Sample(sRotation, uv).xyz;
    float3 r = frac(sin(cross(n, float3(12.9898, 78.233, 45.5432))) * 43758.5469) * 10.0 + 0.0001;
    float3 t1 = normalize(cross(toLight, r));
    float3 t2 = normalize(cross(toLight, t1));
    float radius = (1.0 - hLights[k].shadowData.x) * (soft ? 0.5 : 0.1);
    t1 = t1 * radius;
    t2 = t2 * radius;
    float nearest = max(hLights[k].projectionParams.z, 0.005);
    float scale = 1.0;
    [branch] if (soft) {
        float centre = HopCubeDepth(k, toLight, nearest);
        float blockers = 0.0;
        [loop] for (int i = 0; i < 16; i++)
            blockers += gShadowCube[k].SampleLevel(sShadowRaw, t2 * kHopPoisson[i].y + t1 * kHopPoisson[i].x + toLight, 0).x;
        float mean = blockers * 0.0625;
        scale = (centre - blockers * 0.0625) / mean;
    }
    float sum = 0.0;
    [loop] for (int i = 0; i < 25; i++) {
        float3 d = soft ? (t2 * kHopPoisson[i].w + t1 * kHopPoisson[i].z) * scale + toLight
                        : t2 * kHopPoisson[i].w + t1 * kHopPoisson[i].z + toLight;
        sum += gShadowCube[k].SampleCmpLevelZero(sShadowCompare, d, HopCubeDepth(k, d, nearest));
    }
    return sum * 0.04;
}

// Light k's attenuation (and shadow) at this pixel: what its ForwardAdd PS multiplies the DS colour with.
float HopAttenuation(int k, HopVertex i) {
    uint kind = hLights[k].kind.x, shadow = hLights[k].kind.y;
    float3 w = i.world;
    float a = 1.0;
    [branch] if (kind == HOP_LIGHT_POINT) {
        float3 lc = hLights[k].worldToLight[1].xyz * w.y;
        lc = hLights[k].worldToLight[0].xyz * w.x + lc;
        lc = hLights[k].worldToLight[2].xyz * w.z + lc;
        lc = lc + hLights[k].worldToLight[3].xyz;
        a = gAtten[k].Sample(sAtten, dot(lc, lc).xx).x;
        [branch] if (shadow == HOP_SHADOW_CUBE || shadow == HOP_SHADOW_CUBE_SOFT)
            a = HopShadowCube(k, w, shadow == HOP_SHADOW_CUBE_SOFT) * a;
    } else if (kind == HOP_LIGHT_SPOT) {
        float4 lc = hLights[k].worldToLight[1] * w.y;
        lc = hLights[k].worldToLight[0] * w.x + lc;
        lc = hLights[k].worldToLight[2] * w.z + lc;
        lc = lc + hLights[k].worldToLight[3];
        float cookie = gCookie[k].Sample(sCookie, lc.xy / lc.w + 0.5).w;
        float front = 0.0 < lc.z ? 1.0 : 0.0;
        float falloff = gAtten[k].Sample(sAtten, dot(lc.xyz, lc.xyz).xx).x;
        a = falloff * (cookie * front);
        [branch] if (shadow == HOP_SHADOW_DEPTH || shadow == HOP_SHADOW_DEPTH_SOFT)
            a = HopShadowSpot(k, w, shadow == HOP_SHADOW_DEPTH_SOFT) * a;
    } else {
#if HOP_SCREEN
        [branch] if (shadow == HOP_SHADOW_SCREEN) a = gShadow[k].Sample(sDirectionalScreen, i.screen.xy / i.screen.w).x;
#endif
    }
    return a;
}

float4 HopPS(HopVertex i) : SV_Target {
#if !HOP_BASE
    // each light where its ForwardAdd draw's scissor let it through (alpha too: those draws write alpha 1). Tested per
    // 2x2 quad first, so the light's texture reads have their derivatives.
    float3 c = 0.0;
    float alpha = 0.0;
#if HOP_LIGHTS > 0
    uint2 p = (uint2)i.pos.xy, q = p & ~1u;
    [unroll] for (int k = 0; k < HOP_LIGHTS; k++) {
        uint2 lo = hLights[k].kind.zw & 0xffff, hi = hLights[k].kind.zw >> 16;
        [branch] if (all(q + 2 > lo) && all(q < hi)) {
            float3 l = HopAttenuation(k, i) * i.light[k];
            bool inside = all(p >= lo) && all(p < hi);
            c += inside ? l : 0.0;
            alpha = inside ? 1.0 : alpha;
        }
    }
#endif
    return float4(c, alpha);
#else
#if HOP_SCREEN
    float3 c;
    [branch] if (hFlags.y) {
        float m = gScreenShadow.Sample(sScreenShadow, i.screen.xy / i.screen.w).x;
        c = i.direct * m + i.ambient;
    } else {
        c = i.ambient + i.direct;
    }
#else
    float3 c = i.ambient + i.direct;
#endif
#if HOP_LIGHTS > 0
    [unroll] for (int k = 0; k < HOP_LIGHTS; k++) c += HopAttenuation(k, i) * i.light[k];
#endif
    return float4(c, 1.0);
#endif
}
