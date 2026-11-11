// SimXR capture: a full-screen triangle over the capture target.
float4 main(in uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position
{
    uv = float2((id == 1) ? 2.0 : 0.0, (id == 2) ? 2.0 : 0.0);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
