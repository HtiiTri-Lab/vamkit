// The isoline tessellator's domain coordinates, read back through stream-out (the native side does it once on the game's
// device): a single patch with the factors in b0 (x density, y detail), every output vertex's SV_DomainLocation.
cbuffer F : register(b0) { float4 factors; };
struct CP { float4 p : POS; };
CP VS(uint id : SV_VertexID) { CP o; o.p = float4(id, 0, 0, 0); return o; }
struct PC { float f[2] : SV_TessFactor; };
PC PCF(InputPatch<CP, 3> p) { PC o; o.f[0] = factors.x; o.f[1] = factors.y; return o; }
[domain("isoline")] [partitioning("integer")] [outputtopology("line")] [outputcontrolpoints(3)] [patchconstantfunc("PCF")]
CP HS(InputPatch<CP, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }
struct DO { float4 uv : UV; };
[domain("isoline")] DO DS(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<CP, 3> p) { DO o; o.uv = float4(uv, 0, 1); return o; }
