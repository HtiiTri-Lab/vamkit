/*
* Copyright (c) 2024-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#ifndef _RTXCR_HAIRFARFIELDBCSDF_HLSLI_
#define _RTXCR_HAIRFARFIELDBCSDF_HLSLI_

#include "utils/RtxcrBsdf.hlsli"
#include "HairMaterial.hlsli"
#include "utils/RtxcrHairBsdfHelper.hlsli"

struct RTXCR_FarFieldTrtFit
{
    float peak;
    float weight1;
    float weight2;
    float width1;
    float width2;
};

float RTXCR_FarFieldWrapAngle(const float angle)
{
    float wrapped = fmod(angle + RTXCR_PI, RTXCR_TWO_PI);
    if (wrapped < 0.0f)
    {
        wrapped += RTXCR_TWO_PI;
    }
    return wrapped - RTXCR_PI;
}

// A normal distribution wrapped onto [-pi, pi).  Spatial aliases converge
// quickly for narrow lobes; the Fourier form is stable for broad lobes.
float RTXCR_FarFieldWrappedGaussian(const float angle, const float stddev)
{
    const float sigma = max(stddev, 1e-4f);
    const float delta = RTXCR_FarFieldWrapAngle(angle);

    if (sigma < 1.0f)
    {
        return RTXCR_Gaussian1D(delta - RTXCR_TWO_PI, sigma) +
               RTXCR_Gaussian1D(delta, sigma) +
               RTXCR_Gaussian1D(delta + RTXCR_TWO_PI, sigma);
    }

    float result = 1.0f;
    [unroll]
    for (uint harmonic = 1; harmonic <= 6; ++harmonic)
    {
        const float n = float(harmonic);
        result += 2.0f * exp(-0.5f * sigma * sigma * n * n) * cos(n * delta);
    }
    return max(result * RTXCR_ONE_OVER_TWO_PI, 0.0f);
}

void RTXCR_FarFieldComputeTrtFit(const float thetaI,
                                 const float roughness,
                                 out RTXCR_FarFieldTrtFit fit)
{
    // TRT lobe: custom 3-Gaussian lobe based on fitting to MC simulation
    // The elliptic-hair fit is parameterized by |theta_i|, not theta_d.
    const float ti = abs(thetaI);
    float variance1;
    float variance2;

    if (ti < 0.525f)
    {
        fit.peak = cos(ti) - 0.733f;
        fit.weight1 = (-0.000111282f) * (-0.103125f + ti) * ti + pow(ti, 15.7265f) + 0.00023939f;
        fit.weight2 = 0.000322755f * ((ti - pow(1.80972f * ti, 16.7669f)) * tan(ti) + 0.991977f);
        variance1 = 0.00597578f + 0.000428897f * cos(5.41149f * ti);
        variance2 = 0.0181f * tan(cos(2.121f * ti));
    }
    else if (ti < 1.1f)
    {
        fit.peak = max(0.0f, 0.00493f + 0.579f * ti - 0.775f * ti * ti);
        fit.weight1 = 0.00108f - 0.0014f * ti + 0.0003937f * ti * ti;
        fit.weight2 = -0.00119f + 0.00219f * ti;
        variance1 = 0.0391f - 0.0888f * ti + 0.0581f * ti * ti;
        variance2 = 0.384f - 1.14f * ti + 0.942f * ti * ti;
    }
    else
    {
        fit.peak = 0.0f;
        fit.weight1 = 0.0f;
        fit.weight2 = 0.000239f + 0.00139f * ti * ti * ti - 0.00053124f * ti * ti * ti * ti * ti;
        variance1 = 1.0f;
        variance2 = -1.86f + 2.73f * ti - 0.7437f * ti * ti;
    }

    fit.weight1 = max(fit.weight1, 0.0f);
    fit.weight2 = max(fit.weight2, 0.0f);
    const float widthScale = max(roughness, 1e-4f) / 0.06f;
    fit.width1 = max(widthScale * sqrt(max(variance1, 0.0f)), 1e-4f);
    fit.width2 = max(widthScale * sqrt(max(variance2, 0.0f)), 1e-4f);
}

float RTXCR_FarFieldTrtAzimuth(const float phi,
                               const RTXCR_FarFieldTrtFit fit)
{
    return fit.weight1 * RTXCR_FarFieldWrappedGaussian(phi - fit.peak, fit.width1) +
           fit.weight1 * RTXCR_FarFieldWrappedGaussian(phi + fit.peak, fit.width1) +
           fit.weight2 * RTXCR_FarFieldWrappedGaussian(phi, fit.width2);
}

float RTXCR_FarFieldVariance(const float beta)
{
    // Very large v is already indistinguishable from a uniform elevation
    // distribution, and clamping avoids loss of precision in sinh(1 / v).
    return clamp(beta * beta, 1e-6f, 100.0f);
}

float RTXCR_FarFieldLongitudinal(const float thetaOp,
                                 const float sinThetaI,
                                 const float cosThetaI,
                                 const float beta)
{
    const float sinThetaOp = sin(thetaOp);
    const float cosThetaOp = abs(cos(thetaOp));
    return RTXCR_MP(cosThetaOp, cosThetaI, sinThetaOp, sinThetaI, RTXCR_FarFieldVariance(beta));
}

void RTXCR_FarFieldSampleLongitudinal(const float thetaOp,
                                      const float beta,
                                      const float2 random,
                                      out float sinThetaI,
                                      out float cosThetaI)
{
    const float v = RTXCR_FarFieldVariance(beta);
    const float u = max(random.x, 1e-5f);
    const float cosTheta = clamp(1.0f + v * log(u + (1.0f - u) * exp(-2.0f / v)), -1.0f, 1.0f);
    const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
    const float sinThetaOp = sin(thetaOp);
    const float cosThetaOp = abs(cos(thetaOp));

    sinThetaI = clamp(-cosTheta * sinThetaOp + sinTheta * cos(RTXCR_TWO_PI * random.y) * cosThetaOp, -1.0f, 1.0f);
    cosThetaI = sqrt(max(1.0f - sinThetaI * sinThetaI, 0.0f));
}

void RTXCR_FarFieldCoordinateFrame(in const RTXCR_HairInteractionSurface hairInteractionSurface,
                                   in const float3 wo,
                                   out float3 tangent,
                                   out float3 azimuthOrigin,
                                   out float3 azimuthBitangent,
                                   out float sinThetaO,
                                   out float cosThetaO)
{
    tangent = normalize(hairInteractionSurface.tangent); // tangent of hair
    sinThetaO = clamp(dot(wo, tangent), -1.0f, 1.0f);
    cosThetaO = sqrt(max(1.0f - sinThetaO * sinThetaO, 0.0f));

    float3 projectedWo = wo - sinThetaO * tangent;
    if (dot(projectedWo, projectedWo) < 1e-10f)
    {
        projectedWo = hairInteractionSurface.shadingNormal -
                      dot(hairInteractionSurface.shadingNormal, tangent) * tangent;
        if (dot(projectedWo, projectedWo) < 1e-10f)
        {
            const float3 fallbackAxis = abs(tangent.z) < 0.9f ?
                                        float3(0.0f, 0.0f, 1.0f) :
                                        float3(0.0f, 1.0f, 0.0f);
            projectedWo = fallbackAxis - dot(fallbackAxis, tangent) * tangent;
        }
    }

    azimuthOrigin = normalize(projectedWo);
    azimuthBitangent = normalize(cross(azimuthOrigin, tangent));
}

void RTXCR_FarFieldDirectionCoordinates(in const float3 wi,
                                        in const float3 tangent,
                                        in const float3 azimuthOrigin,
                                        in const float3 azimuthBitangent,
                                        out float sinThetaI,
                                        out float cosThetaI,
                                        out float phi,
                                        out float cosPhi)
{
    sinThetaI = clamp(dot(wi, tangent), -1.0f, 1.0f);
    cosThetaI = sqrt(max(1.0f - sinThetaI * sinThetaI, 0.0f));

    const float3 projectedWi = wi - sinThetaI * tangent;
    const float invCosThetaI = 1.0f / max(cosThetaI, 1e-7f);
    cosPhi = clamp(dot(projectedWi, azimuthOrigin) * invCosThetaI, -1.0f, 1.0f);
    const float sinPhi = clamp(dot(projectedWi, azimuthBitangent) * invCosThetaI, -1.0f, 1.0f);
    phi = abs(RTXCR_Atan2safe(sinPhi, cosPhi));
}

float RTXCR_FarFieldTtAzimuthPdf(const float phi,
                                 const float cosPhi,
                                 const float etaPrimeInverse,
                                 out float hTT)
{
    const float a = clamp(etaPrimeInverse, 0.0f, 1.0f);
    const float supportStart = 2.0f * asin(a);
    if (phi < supportStart)
    {
        hTT = 1.0f;
        return 0.0f;
    }

    // hTT: root of phi(h) for p = 1
    const float numerator = 0.5f + 0.5f * cosPhi;
    const float denominator = 1.0f + a * a -
                              2.0f * a * sqrt(max(0.5f - 0.5f * cosPhi, 0.0f));
    hTT = sqrt(saturate(numerator / max(denominator, 1e-7f)));

    const float dhR = sqrt(max(1.0f - hTT * hTT, 1e-7f));
    const float dhT = sqrt(max(1.0f - a * a * hTT * hTT, 1e-7f));
    const float derivative = abs(-2.0f / dhR + 2.0f * a / dhT);
    return derivative > 1e-7f ? 0.5f / derivative : 0.0f;
}

float4 RTXCR_FarFieldLobeProbabilities(in const RTXCR_HairMaterialInteractionBcsdf material,
                                       const float sinThetaO,
                                       const float cosThetaO)
{
    const float diffuseProbability = saturate(material.diffuseReflectionWeight);
    const float ior = max(material.ior, 1.0001f);
    const float f0 = RTXCR_CalculateBaseReflectivity(1.0f, ior);
    const float fresnel = RTXCR_EvalFresnelSchlick(f0, cosThetaO);
    const float cosThetaT = sqrt(max(1.0f - sinThetaO * sinThetaO / (ior * ior), 1e-7f));
    const float3 transmittance = exp(-2.0f * max(material.absorptionCoefficient, 0.0f) / cosThetaT);

    const float energyR = fresnel * RTXCR_Luminance(saturate(material.reflectionTint));
    const float energyTT = (1.0f - fresnel) * (1.0f - fresnel) * RTXCR_Luminance(transmittance);

    // A2 at h = 0 is a conservative, inexpensive proxy for the fitted TRT
    // lobe.  Unlike the fit itself, it responds correctly to runtime IOR.
    const float energyTRT = (1.0f - fresnel) * (1.0f - fresnel) * fresnel *
                            RTXCR_Luminance(transmittance * transmittance);

    // Keep every non-zero scattering component represented in the proposal.
    float3 specularEnergy = max(float3(energyR, energyTT, energyTRT), 1e-4f.xxx);
    specularEnergy /= specularEnergy.x + specularEnergy.y + specularEnergy.z;
    return float4(diffuseProbability, (1.0f - diffuseProbability) * specularEnergy);
}

// Essential interface functions invoked in generated material code
// Custom far-field BCSDF eval() [Eugene d'Eon - 2022]
//  R lobe: [d'Eon et al. 2014 - SIGGRAPH talk]
//  TT lobe: [Marschner et al. 2003]
//  TRT lobe: custom 3-Gaussian lobe based on fitting to MC simulation
// Returns the CRCF (including the hair integration measure) and
// the full solid-angle PDF used by RTXCR_SampleFarFieldBcsdf.
void RTXCR_HairFarFieldBcsdfEval(in const RTXCR_HairInteractionSurface hairInteractionSurface,
                                 in const RTXCR_HairMaterialInteractionBcsdf hairMaterialInteractionBcsdf,
                                 in const float3 wi,     // pointing to light
                                 in const float3 wo,     // pointing to camera
                                 out float3 bsdf,        // Far-field CRCF for hair lobes (R, TT, TRT)
                                 out float3 bsdfDiffuse, // [optional] The extension hair diffuse CRCF for artificial hair, set diffuseReflectionWeight to 0 to disable this feature
                                 out float pdf)          // PDF for the current sample (used for indirect pass)
{
    float3 tangent;
    float3 azimuthOrigin;
    float3 azimuthBitangent;
    float sinThetaO;
    float cosThetaO;
    RTXCR_FarFieldCoordinateFrame(hairInteractionSurface, wo, tangent,
                                  azimuthOrigin, azimuthBitangent,
                                  sinThetaO, cosThetaO);

    // determine cylindrical coordinates (theta/phi) [Marschner et al. 2003]
    float sinThetaI;
    float cosThetaI;
    float phi;
    float cosPhi;
    RTXCR_FarFieldDirectionCoordinates(wi, tangent, azimuthOrigin, azimuthBitangent, sinThetaI, cosThetaI, phi, cosPhi);

    const float thetaI = asin(sinThetaI);
    const float thetaO = asin(sinThetaO);
    const float thetaD = 0.5f * (thetaO - thetaI);
    const float sinThetaD = sin(thetaD);
    const float cosThetaD = cos(thetaD);

    // load fiber properties
    const float roughness = max(hairMaterialInteractionBcsdf.roughness, 1e-4f);
    const float alpha = hairMaterialInteractionBcsdf.cuticleAngle;
    const float ior = max(hairMaterialInteractionBcsdf.ior, 1.0001f);
    const float iorSqr = ior * ior;
    const float f0 = RTXCR_CalculateBaseReflectivity(1.0f, ior);
    const float3 mua = max(hairMaterialInteractionBcsdf.absorptionCoefficient, 0.0f);

    // Compute R lobe - smooth azimuthal N term, normalized longitudinal M term
    // p(phi) = cos(phi / 2) / 4 is normalized on [-pi, pi].
    const float azimuthR = 0.25f * max(cos(0.5f * phi), 0.0f);
    // tighten the R lobe (or not) with phi - [d'Eon et al. 2014 SIGGRAPH talk]
    const float betaR = sqrt(2.0f) * roughness * max(0.01f, cos(0.5f * phi));
    const float longitudinalR = RTXCR_FarFieldLongitudinal(thetaO + 2.0f * alpha, sinThetaI, cosThetaI, betaR);
    // Attenuation is parameterized by the outgoing ray and cylinder offset,
    // as in Ap.  Keeping it independent of the normalized longitudinal term
    // is what makes the lobe's directional integral conservative.
    // [d'Eon et al. 2011 - (12)], using the outgoing-ray approximation.
    const float fresnelR = RTXCR_EvalFresnelSchlick(f0, saturate(cosThetaO * cos(0.5f * phi)));

    // Compute TT lobe - exact azimuthal Jacobian N term, normalized longitudinal M term
    // Sample/evaluate the exact Jacobian of uniform cylinder offset h.
    const float betaTT = 0.5f * roughness * sqrt(max((iorSqr - 1.0f) / max(cosThetaO * cosThetaO, 1e-7f), 0.0f));
    const float longitudinalTT = RTXCR_FarFieldLongitudinal(thetaO - alpha,
                                                            sinThetaI, cosThetaI, betaTT);
    const float etaPrimeInverse = cosThetaD / // 1.0 / eta_prime
                                  sqrt(max(iorSqr - sinThetaD * sinThetaD, 1e-7f));
    float hTT;
    const float azimuthTT = RTXCR_FarFieldTtAzimuthPdf(phi, cosPhi, etaPrimeInverse, hTT);
    const float cosGammaI = sqrt(max(1.0f - hTT * hTT, 0.0f));
    // [d'Eon et al. 2011 - (14)], using the outgoing-ray approximation.
    const float fresnelTT = RTXCR_EvalFresnelSchlick(f0, saturate(cosThetaO * cosGammaI));
    const float cosThetaT = sqrt(max(1.0f - sinThetaO * sinThetaO / iorSqr, 1e-7f));
    const float etaPrimeInverseO = cosThetaO / sqrt(max(iorSqr - sinThetaO * sinThetaO, 1e-7f));
    const float cosGammaT = sqrt(max(1.0f - hTT * hTT * etaPrimeInverseO * etaPrimeInverseO, 0.0f));
    const float3 attenuationTT = (1.0f - fresnelTT) * (1.0f - fresnelTT) * exp(-2.0f * mua * cosGammaT / cosThetaT); // TODO: absorption with Medulla

    // compute TRT lobe as sum of 3 Gaussians
    // The Gaussians are wrapped periodically and their fit parameters are functions of theta_i.
    const float betaTRT = roughness * (2.0f + pow(abs(thetaO), 1.5f));
    const float longitudinalTRT = RTXCR_FarFieldLongitudinal(thetaO - 3.0f * alpha,
                                                             sinThetaI, cosThetaI, betaTRT);
    RTXCR_FarFieldTrtFit trtFit;
    RTXCR_FarFieldComputeTrtFit(thetaI, roughness, trtFit);
    const float trtAzimuthUnnormalized = RTXCR_FarFieldTrtAzimuth(phi, trtFit);
    const float trtAzimuthIntegral = max(2.0f * trtFit.weight1 + trtFit.weight2, 1e-7f);
    // assume h = 0 for absorption
    const float fresnelSpec = RTXCR_EvalFresnelSchlick(f0, cosThetaO);
    const float3 transmittanceSpec = exp(-2.0f * mua / cosThetaT);
    // The fitted weights define the TRT azimuthal shape.  Give that normalized
    // shape the physical A2 energy at h = 0 so IOR and absorption affect TRT,
    // instead of accidentally counting the fit amplitude as extra energy.
    const float3 attenuationTRT = (1.0f - fresnelSpec) * (1.0f - fresnelSpec) *
                                  fresnelSpec * transmittanceSpec * transmittanceSpec;

    const float diffuseWeight = saturate(hairMaterialInteractionBcsdf.diffuseReflectionWeight);
    const float diffuseAzimuth = max((RTXCR_PI - phi) * cosPhi + sin(phi), 0.0f);
    const float diffusePdf = cosThetaI * (0.25f / RTXCR_PI) * diffuseAzimuth;

    // eval:
    bsdf = max((1.0f - diffuseWeight) *
               (longitudinalR * azimuthR * fresnelR * saturate(hairMaterialInteractionBcsdf.reflectionTint) +
                longitudinalTT * azimuthTT * attenuationTT +
                longitudinalTRT * trtAzimuthUnnormalized / trtAzimuthIntegral * attenuationTRT),
               0.0f);
    bsdfDiffuse = diffuseWeight * diffusePdf * saturate(hairMaterialInteractionBcsdf.diffuseReflectionTint);

    // PDF is the complete normalized mixture used by the sampling function.
    const float4 lobeProbability = RTXCR_FarFieldLobeProbabilities(hairMaterialInteractionBcsdf, sinThetaO, cosThetaO);
    const float proposalR = longitudinalR * azimuthR;
    const float proposalTT = longitudinalTT * azimuthTT;
    const float proposalTRT = longitudinalTRT * trtAzimuthUnnormalized / trtAzimuthIntegral;
    pdf = dot(lobeProbability, float4(diffusePdf, proposalR, proposalTT, proposalTRT));
}

bool RTXCR_SampleFarFieldBcsdf(in const RTXCR_HairInteractionSurface hairInteractionSurface,
                               in const RTXCR_HairMaterialInteractionBcsdf hairMaterialInteractionBcsdf,
                               in const float3 wo,
                               in const float h,
                               in const float3 rand2[2],
                               out float3 wi,
                               out float3 bsdf,
                               out float3 bsdfDiffuse,
                               out float pdf)
{
    float3 tangent;
    float3 azimuthOrigin;
    float3 azimuthBitangent;
    float sinThetaO;
    float cosThetaO;
    RTXCR_FarFieldCoordinateFrame(hairInteractionSurface, wo, tangent,
                                  azimuthOrigin, azimuthBitangent,
                                  sinThetaO, cosThetaO);

    const float thetaO = asin(sinThetaO);
    const float roughness = max(hairMaterialInteractionBcsdf.roughness, 1e-4f);
    const float alpha = hairMaterialInteractionBcsdf.cuticleAngle;
    const float ior = max(hairMaterialInteractionBcsdf.ior, 1.0001f);
    // Select a lobe using outgoing-direction energy proxies at h = 0, then
    // sample its normalized longitudinal and azimuthal proposal.
    const float4 lobeProbability = RTXCR_FarFieldLobeProbabilities(
        hairMaterialInteractionBcsdf, sinThetaO, cosThetaO);

    float sinThetaI;
    float cosThetaI;

    if (rand2[0].z < lobeProbability.x)
    {
        // sample diffuse
        // The marginal of cosine hemispheres over uniformly sampled cylinder
        // offsets is exactly the analytic diffuse BCSDF above.
        const float clampedH = clamp(h, -1.0f, 1.0f);
        const float3 cylinderNormal = sqrt(max(1.0f - clampedH * clampedH, 0.0f)) * azimuthOrigin +
                                      clampedH * azimuthBitangent;
        const float3 cylinderBitangent = normalize(cross(cylinderNormal, tangent));
        float unusedPdf;
        const float3 localWi = RTXCR_SampleHemisphere(rand2[0].xy, unusedPdf);
        wi = localWi.x * tangent + localWi.y * cylinderBitangent +
             localWi.z * cylinderNormal;
    }
    else if (rand2[0].z < lobeProbability.x + lobeProbability.y)
    {
        // sample R
        const float phi = RTXCR_PhiR(clamp(h, -1.0f, 1.0f));
        const float betaR = sqrt(2.0f) * roughness * max(0.01f, cos(0.5f * phi));
        RTXCR_FarFieldSampleLongitudinal(thetaO + 2.0f * alpha, betaR,
                                         rand2[0].xy, sinThetaI, cosThetaI);
        wi = cosThetaI * (cos(phi) * azimuthOrigin + sin(phi) * azimuthBitangent) +
             sinThetaI * tangent;
    }
    else if (rand2[0].z < lobeProbability.x + lobeProbability.y + lobeProbability.z)
    {
        // sample TT
        const float betaTT = 0.5f * roughness * sqrt(max((ior * ior - 1.0f) / max(cosThetaO * cosThetaO, 1e-7f), 0.0f));
        RTXCR_FarFieldSampleLongitudinal(thetaO - alpha, betaTT, rand2[0].xy, sinThetaI, cosThetaI);
        const float thetaI = asin(sinThetaI);
        const float thetaD = 0.5f * (thetaO - thetaI);
        const float etaPrimeInverse = cos(thetaD) / // 1.0 / eta_prime
                                      sqrt(max(ior * ior - sin(thetaD) * sin(thetaD), 1e-7f));
        const float phi = RTXCR_FarFieldWrapAngle(
            RTXCR_PhiTT(clamp(h, -1.0f, 1.0f), etaPrimeInverse));
        wi = cosThetaI * (cos(phi) * azimuthOrigin + sin(phi) * azimuthBitangent) +
             sinThetaI * tangent;
    }
    else
    {
        // sample TRT
        const float betaTRT = roughness * (2.0f + pow(abs(thetaO), 1.5f));
        RTXCR_FarFieldSampleLongitudinal(thetaO - 3.0f * alpha, betaTRT,
                                         rand2[0].xy, sinThetaI, cosThetaI);

        RTXCR_FarFieldTrtFit trtFit;
        RTXCR_FarFieldComputeTrtFit(asin(sinThetaI), roughness, trtFit);
        const float integral = max(2.0f * trtFit.weight1 + trtFit.weight2, 1e-7f);
        const float selector = saturate(0.5f * h + 0.5f) * integral;
        float center;
        float width;
        if (selector < trtFit.weight1)
        {
            center = trtFit.peak;
            width = trtFit.width1;
        }
        else if (selector < 2.0f * trtFit.weight1)
        {
            center = -trtFit.peak;
            width = trtFit.width1;
        }
        else
        {
            center = 0.0f;
            width = trtFit.width2;
        }

        const float gaussian = RTXCR_RandomGaussian1D(rand2[1].x, min(rand2[1].y, 1.0f - 1e-7f));
        const float phi = RTXCR_FarFieldWrapAngle(center + width * gaussian);
        wi = cosThetaI * (cos(phi) * azimuthOrigin + sin(phi) * azimuthBitangent) +
             sinThetaI * tangent;
    }

    RTXCR_HairFarFieldBcsdfEval(hairInteractionSurface,
                                hairMaterialInteractionBcsdf,
                                wi, wo, bsdf, bsdfDiffuse, pdf);
    return pdf > 0.0f;
}

#endif
