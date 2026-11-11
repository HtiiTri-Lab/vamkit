// Local VDXR patch: blend only the inner border of the existing NR rectangle.
// Original and NR images retain their original resolution and eye mapping.
Texture2D<float4> originalImage : register(t0);
cbuffer FeatherConstants : register(b0) {
    int2 inputBase;
    int2 outputBase;
    int2 regionSize;
    float featherPixels;
    float padding;
};

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    int2 localPixel = int2(position.xy) - outputBase;
    float edge = min(min(localPixel.x, localPixel.y),
                     min(regionSize.x - 1 - localPixel.x, regionSize.y - 1 - localPixel.y));
    float nrWeight = smoothstep(0.0, featherPixels, edge);
    // RGB alpha blending over the NR target; the target alpha is preserved.
    return float4(originalImage.Load(int3(inputBase + localPixel, 0)).rgb, 1.0 - nrWeight);
}
