// SimXR capture: copies a rectangle of a submitted eye image into the capture
// target (same size: point sampling = exact pixels; smaller: bilinear), opaque.
cbuffer CaptureConstants : register(b0)
{
    float2 sourceOffset; // normalized texture coordinates of the rectangle's corner
    float2 sourceExtent; // normalized size of the rectangle
    uint flipY;
    uint3 padding;
};

SamplerState captureSampler : register(s0);
Texture2D<float4> sourceColor : register(t0);

float4 main(in float4 position : SV_Position, in float2 uv : TEXCOORD0) : SV_Target
{
    float2 coord = sourceOffset + uv * sourceExtent;
    if (flipY != 0)
    {
        coord.y = 1.0 - coord.y;
    }
    return float4(sourceColor.SampleLevel(captureSampler, coord, 0).rgb, 1.0);
}
