// rtc_omm.hlsli: the micro-triangle order of an opacity micromap (VK_EXT_opacity_micromap's and NVAPI's: the "bird
// curve" of the spec's index-to-barycentrics appendix). Used by path_omm.hlsl (PathOmmBake).
#ifndef RTC_OMM_HLSLI
#define RTC_OMM_HLSLI

uint OmmEvenBits(uint x) {
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return x;
}
uint OmmPrefixXor(uint x) {
    x ^= x >> 1; x ^= x >> 2; x ^= x >> 4; x ^= x >> 8;
    return x;
}

// Micro-triangle `index` of a level-`level` OMM (4^level of them): its corners as hit barycentrics (the weights of the
// triangle's vertices 1 and 2, CandidateTriangleBarycentrics' order).
void OmmMicroTriangle(uint index, uint level, out float2 c0, out float2 c1, out float2 c2) {
    if (level == 0) { c0 = float2(0, 0); c1 = float2(1, 0); c2 = float2(0, 1); return; }
    uint b0 = OmmEvenBits(index), b1 = OmmEvenBits(index >> 1);
    uint fx = OmmPrefixXor(b0), fy = OmmPrefixXor(b0 & ~b1);
    uint t = fy ^ b1;
    uint mask = (1u << level) - 1;
    uint u = ((fx & ~t) | (b0 & ~t) | (~b0 & ~fx & t)) & mask;
    uint v = (fy ^ b0) & mask;
    uint w = ((~fx & ~t) | (b0 & ~t) | (~b0 & fx & t)) & mask;
    bool upright = ((u ^ v ^ w) & 1u) != 0;
    if (!upright) { u++; v++; }
    float s = 1.0 / (float)(1u << level);
    float2 p = float2(u, v) * s;
    c0 = p;
    c1 = upright ? p + float2(s, 0) : p - float2(s, 0);
    c2 = upright ? p + float2(0, s) : p - float2(0, s);
}

#endif
