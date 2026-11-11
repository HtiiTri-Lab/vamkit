// Light that passes through thin skin (ears, nostrils, fingers, toes) from a light behind it: one light, added on top
// of what VaM's own skin shader drew for that light (blend one + one, depth test equal, the skin draw's own vertex
// shader). tools\gen_skin_table.py writes, per layout of VaM's skin pixel programs, the declarations, PathMM() for the
// light's kind and a main() that reads the world position, normal and UV from wherever that layout keeps them.
//
// How much skin the light really crosses (0.7.0, as Jimenez 2010, FaceWorks deep scatter, UE / HDRP thick objects):
// the light's own shadow map holds where the light first enters the body; PathMM() turns that depth into the
// distance from there to this point (moved a little inward along the normal, so the surface itself never counts as
// the entry). Whatever lies on the way (the rest of the head, an eyeball, teeth, other fingers, a wide nose bone)
// makes that path long and nothing comes through. The baked thickness (bind pose, rays along the inverted normal,
// smoothed over the surface: native\bake.cpp) is a lower bound: thick parts stay dark at their silhouettes, and a
// shadow map whose bias puts the entry too close cannot make skin thinner than it is.
// Through skin, light falls off with distance like a Gaussian per colour (FaceWorks: Gaussian, 2.7 mm in theory,
// 5-6 mm looks better), red widest: thin parts come through orange-red, thicker ones deep red, a centimetre and more
// not at all. Only light from behind counts (back irradiance max(N.-L, 0), no wrap): light grazing a thin rim from
// the side does not draw a red line along it. 13 samples across the light direction (the point, 6 at half the
// radius, 6 at the radius), each with its own path, weighted by how far red light spreads: soft, no hard edges. A ring
// sample the light reaches without crossing anything is outside the body (in the light beside a thin edge or in front
// of a curve) and does not count.

cbuffer SkinTransmission : register(b13)
{
    float4 stUV;      // xy scale, zw offset: the shader's UV (with _MainTex_ST) -> thickness texture UV
    float4 stSigma;   // xyz 1 / Gaussian sigma per colour (per mm), w strength
    float4 stParams;  // x wrap (0), y thickness scale, z largest thickness that counts (mm), w unused
    float4 stShadow;  // x inward offset of the shading point (m), y kernel radius (m), z weight of an inner-ring tap,
                      // w weight of an outer-ring tap (the centre tap weighs 1)
};
Texture2D<float2> stThickness : register(t16);
SamplerState stSampler : register(s15);

float ThicknessMM(float2 uvMain)
{
    return stThickness.SampleLevel(stSampler, uvMain * stUV.xy + stUV.zw, 0).x * stParams.y;
}

// unit vectors across the light direction
void ShadowBasis(float3 L, out float3 t1, out float3 t2)
{
    float3 up = abs(L.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    t1 = normalize(cross(up, L));
    t2 = cross(L, t1);
}

// tap k (0..5) of the inner ring (half the radius) and of the outer ring (the radius, turned by 30 degrees)
float3 InnerTap(int k, float3 t1, float3 t2)
{
    float a = k * 1.04719755;
    return (t1 * cos(a) + t2 * sin(a)) * (0.5 * stShadow.y);
}

float3 OuterTap(int k, float3 t1, float3 t2)
{
    float a = k * 1.04719755 + 0.52359878;
    return (t1 * cos(a) + t2 * sin(a)) * stShadow.y;
}

float Facing(float3 n, float3 L)
{
    return saturate(stParams.x + dot(-n, L));
}

// what comes through skin `mm` thick, per colour
float3 Transmittance(float mm)
{
    float3 x = mm * stSigma.xyz;
    return exp(-0.5 * x * x);
}

float3 Transmit(float3 T, float facing, float3 lightColor, float atten, float3 albedo)
{
    // 0.22.5: perceptual adaptation for VaM's LDR output. Use incident light before
    // thickness/facing/albedo: a strong backlight must still shine through a dark ear.
    // C1 continuous at black and at full response; no scene-wide brightness estimate.
    float3 incident = max(lightColor * atten, 0.0);
    float lowLight = smoothstep(0.0, 0.06, dot(incident, float3(0.2126, 0.7152, 0.0722)));
    return incident * (lowLight * facing * stSigma.w) * T * albedo;
}
