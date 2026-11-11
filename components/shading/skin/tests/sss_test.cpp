// Offline test of the subsurface scattering (0.10.0).
//
// Part A, the replacement programs' extra outputs (<repo>\out\gen\skin\spec_cases\, as spec_test): each program with subsurface
// outputs draws the case's grid of random inputs in mode 0 (VaM's result), and again with the [Diffuse] Medium look on
// (scatCtl 1, the middle look's wrap), with four float targets and a D32 depth buffer,
// _SpecColor 0, once with the case's _LightColor0 and once with it 0. The colour difference must be albedo (SV_Target2)
// times light (SV_Target1.rgb), the light 0 without _LightColor0, the albedo the same in both, SV_Target3 the depth
// buffer's value, the distance (SV_Target1.a) positive and finite.
//
// Part B, native\sss.cpp on a synthetic eye camera (both eyes side by side, and one eye): a skin base draw (a light
// edge in each eye, the light dimmer lower down, part of it 5 mm farther), an additive light draw (garbage distance,
// albedo and depth that must not land) through SssBeforeDraw / SssAfterDraw, a non-skin square in front, then SssEnd.
// The colour must match a CPU reference of the passes (the same taps, the 16-bit float intermediates, the 8-bit sRGB
// albedo) within one 8-bit step (sRGB) or 4e-3 (16-bit float), the light must not cross between the eyes, red must spread
// farther than blue into the shadow, the non-skin square and the background must stay exactly as drawn, and every
// pipeline state the module touches must be put back.
//
// usage: sss_test <spec_cases dir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>
#include "..\native\sss.h"
void SssTestTargets(ID3D11Texture2D **out);  // sss.cpp built with SSS_TEST

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11InfoQueue *info;

static std::vector<char> ReadFile(const std::string &p) {
    std::vector<char> d;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    return d;
}

struct Rng {
    uint64_t s;
    uint32_t Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 11);
    }
    float Uniform(float a, float b) { return a + (b - a) * (float)(Next() & 0xffffff) / 16777215.0f; }
};

static int DrainMessages(const char *when) {
    if (!info) return 0;
    int errors = 0;
    UINT64 n = ID3D11InfoQueue_GetNumStoredMessages(info);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        ID3D11InfoQueue_GetMessage(info, i, NULL, &len);
        std::vector<char> buf(len);
        D3D11_MESSAGE *m = (D3D11_MESSAGE *)buf.data();
        ID3D11InfoQueue_GetMessage(info, i, m, &len);
        if (m->ID == D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET) continue;
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            if (errors < 4) printf("    debug layer (%s): %.*s\n", when, (int)m->DescriptionByteLength, m->pDescription);
            errors++;
        }
    }
    ID3D11InfoQueue_ClearStoredMessages(info);
    return errors;
}

static ID3D11Buffer *MakeCb(const void *data, UINT bytes) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (bytes + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {data, 0, 0};
    ID3D11Buffer *b = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, data ? &sd : NULL, &b);
    return b;
}

static ID3D11Texture2D *Tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.Format = f, d.SampleDesc.Count = 1, d.Usage = usage;
    d.BindFlags = bind;
    if (usage == D3D11_USAGE_STAGING) d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &t);
    return t;
}

// a texture's rows of bytes (staging copy)
static std::vector<uint8_t> ReadBack(ID3D11Texture2D *t, UINT texel) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    ID3D11Texture2D *s = Tex(d.Width, d.Height, d.Format, 0, D3D11_USAGE_STAGING);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)t);
    std::vector<uint8_t> out((size_t)d.Width * d.Height * texel);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; y++) memcpy(&out[(size_t)y * d.Width * texel], (uint8_t *)m.pData + (size_t)y * m.RowPitch, d.Width * texel);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s, 0);
    }
    ID3D11Texture2D_Release(s);
    return out;
}

static std::vector<char> Compile(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "sss_test.hlsl", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    std::vector<char> out;
    if (FAILED(hr)) {
        printf("compile %s: %s\n", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
    } else {
        const char *p = (const char *)ID3D10Blob_GetBufferPointer(code);
        out.assign(p, p + ID3D10Blob_GetBufferSize(code));
    }
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    return out;
}

// ---- part A ----------------------------------------------------------------------------------------------------------

struct Case {
    std::string hash, target;
    std::vector<std::pair<int, int>> cbs;
    int eyeSlot = -1, eyeRow = 0, eyeComp = 0;
    struct Tex {
        int slot;
        std::string dim;
        int cmp;
    };
    std::vector<Tex> tex;
    std::vector<std::pair<int, int>> smp;
    int sss = 0, lightSlot = -1, lightRow = 0, specSlot = -1, specRow = 0;
};

static bool Parse(const std::string &line, Case &c) {
    std::istringstream in(line);
    std::string t;
    int n;
    in >> c.hash >> c.target >> t >> n;
    if (t != "cb") return false;
    for (int i = 0; i < n; i++) {
        in >> t;
        int a, b;
        if (sscanf(t.c_str(), "%d:%d", &a, &b) != 2) return false;
        c.cbs.push_back({a, b});
    }
    in >> t >> t;
    if (t != "-" && sscanf(t.c_str(), "%d:%d:%d", &c.eyeSlot, &c.eyeRow, &c.eyeComp) != 3) return false;
    in >> t >> n;
    for (int i = 0; i < n; i++) {
        in >> t;
        char dim[32];
        Case::Tex x;
        if (sscanf(t.c_str(), "%d:%31[a-z0-9]:%d", &x.slot, dim, &x.cmp) != 3) return false;
        x.dim = dim;
        c.tex.push_back(x);
    }
    in >> t >> n;
    for (int i = 0; i < n; i++) {
        in >> t;
        int a, b;
        if (sscanf(t.c_str(), "%d:%d", &a, &b) != 2) return false;
        c.smp.push_back({a, b});
    }
    in >> t >> t;  // subd
    in >> t >> c.sss;
    if (t != "sss") return false;
    in >> t >> t;
    if (sscanf(t.c_str(), "%d:%d", &c.lightSlot, &c.lightRow) != 2) return false;
    in >> t >> t;
    return sscanf(t.c_str(), "%d:%d", &c.specSlot, &c.specRow) == 2;
}

static ID3D11ShaderResourceView *MakeTexture(const Case::Tex &x, Rng &r) {
    bool cube = x.dim == "texturecube", vol = x.dim == "texture3d";
    DXGI_FORMAT fmt = x.cmp ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    int size = vol ? 16 : cube ? 32 : 64;
    int mips = x.cmp || vol ? 1 : (int)log2((double)size) + 1;
    int faces = cube ? 6 : 1, depth = vol ? size : 1;
    std::vector<std::vector<uint8_t>> data;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    for (int f = 0; f < faces; f++)
        for (int m = 0; m < mips; m++) {
            int s = std::max(1, size >> m), d = vol ? std::max(1, depth >> m) : 1;
            data.emplace_back((size_t)s * s * d * 4);
            std::vector<uint8_t> &v = data.back();
            if (x.cmp)
                for (size_t i = 0; i < v.size() / 4; i++) ((float *)v.data())[i] = r.Uniform(0, 1);
            else
                for (auto &b : v) b = (uint8_t)(r.Next() & 0xff);
            D3D11_SUBRESOURCE_DATA sd = {NULL, (UINT)(s * 4), (UINT)(s * s * 4)};
            init.push_back(sd);
        }
    for (size_t i = 0; i < data.size(); i++) init[i].pSysMem = data[i].data();
    ID3D11ShaderResourceView *srv = NULL;
    if (vol) {
        D3D11_TEXTURE3D_DESC td = {};
        td.Width = td.Height = td.Depth = size;
        td.MipLevels = mips;
        td.Format = fmt;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture3D *t = NULL;
        if (FAILED(ID3D11Device_CreateTexture3D(dev, &td, init.data(), &t))) return NULL;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &srv);
        ID3D11Texture3D_Release(t);
    } else {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = size;
        td.MipLevels = mips;
        td.ArraySize = faces;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ID3D11Texture2D *t = NULL;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, init.data(), &t))) return NULL;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &srv);
        ID3D11Texture2D_Release(t);
    }
    return srv;
}

static int PartA(const std::string &root) {
    const int W = 256, H = 256;
    std::string dir = root + "\\";
    ID3D11Texture2D *rt[4];
    ID3D11RenderTargetView *rtv[4];
    for (int i = 0; i < 4; i++) {
        rt[i] = Tex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt[i], NULL, &rtv[i]);
    }
    ID3D11Texture2D *ds = Tex(W, H, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL);
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ds, NULL, &dsv);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState *dss = NULL;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &dss);
    D3D11_VIEWPORT vp = {0, 0, (FLOAT)W, (FLOAT)H, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = NULL;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    D3D11_SAMPLER_DESC sdLin = {}, sdCmp = {};
    sdLin.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdLin.AddressU = sdLin.AddressV = sdLin.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sdLin.MaxLOD = D3D11_FLOAT32_MAX;
    sdCmp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sdCmp.AddressU = sdCmp.AddressV = sdCmp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdCmp.ComparisonFunc = D3D11_COMPARISON_GREATER_EQUAL;
    sdCmp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *sLin = NULL, *sCmp = NULL;
    ID3D11Device_CreateSamplerState(dev, &sdLin, &sLin);
    ID3D11Device_CreateSamplerState(dev, &sdCmp, &sCmp);
    const float ctlOff[92] = {0};  // b12: every mode 0 (kSpecFloats 92 since 0.22.20)
    // the same with the diffuse look Medium (scatCtl.x 1, scatA, scatP: VaMSkinTransmission.cs DiffuseA / DiffuseP [2])
    const float ctlMed[92] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1.3808f, 0.8630f, 0.5394f, 0, 3.9291f, 2.9640f, 2.3583f, 0};
    ID3D11Buffer *cbOff = MakeCb(ctlOff, 368), *cbMed = MakeCb(ctlMed, 368);
    float ctlTrans[92];memcpy(ctlTrans,ctlMed,sizeof ctlTrans);ctlTrans[9]=2.0f;
    ID3D11Buffer *cbTrans=MakeCb(ctlTrans,368);
    // the teeth occlusion's texture and sampler (t16, s15), unused in mode 0 but declared by the programs
    {
        ID3D11Texture2D *t = Tex(4, 4, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        ID3D11ShaderResourceView *srv = NULL;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &srv);
        ID3D11Texture2D_Release(t);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 16, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(ctx, 15, 1, &sLin);
    }
    DrainMessages("part A setup");

    std::vector<char> man = ReadFile(dir + "manifest.txt");
    std::istringstream lines(std::string(man.begin(), man.end()));
    std::string line;
    int cases = 0, failed = 0;
    double worstAll = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        Case c;
        if (!Parse(line, c)) {
            printf("bad manifest line: %s\n", line.c_str());
            failed++;
            continue;
        }
        if (!c.sss) continue;
        cases++;
        std::vector<char> rp = ReadFile(dir + c.hash + ".repl.dxbc"), v = ReadFile(dir + c.hash + ".vs.dxbc");
        ID3D11VertexShader *vs = NULL;
        ID3D11PixelShader *ps = NULL;
        if (FAILED(ID3D11Device_CreateVertexShader(dev, v.data(), v.size(), NULL, &vs)) ||
            FAILED(ID3D11Device_CreatePixelShader(dev, rp.data(), rp.size(), NULL, &ps))) {
            printf("%s: cannot create shaders\n", c.hash.c_str());
            failed++;
            continue;
        }
        int drawn = 0, diffOff = 0, lightLeft = 0, albedoOff = 0, zOff = 0, distBad = 0, dbg = 0, lit = 0, medOff = 0, medChanged = 0;
        double worst = 0, worstMed = 0;
        std::vector<float> plain;  // round 0's colour in mode 0 (rounds 4..7 repeat rounds 0..3 with Medium on)
        for (int round = 0; round < 12; round++) {
            bool med = round >= 4;
            Rng r = {(std::stoull(c.hash, NULL, 16) + 0x9e3779b97f4a7c15ull * (round % 4)) | 1};
            std::vector<std::vector<float>> data;
            for (auto &cb : c.cbs) {
                std::vector<float> d((size_t)cb.second * 4);
                for (auto &x : d) x = r.Uniform(0.0f, 1.0f);
                if (cb.first == c.eyeSlot) ((uint32_t *)d.data())[c.eyeRow * 4 + c.eyeComp] = 0;
                if (cb.first == c.specSlot)
                    for (int k = 0; k < 3; k++) d[(size_t)c.specRow * 4 + k] = 0;
                data.push_back(d);
            }
            std::vector<ID3D11ShaderResourceView *> srvs;
            for (auto &x : c.tex) {
                ID3D11ShaderResourceView *s = MakeTexture(x, r);
                ID3D11DeviceContext_PSSetShaderResources(ctx, x.slot, 1, &s);
                srvs.push_back(s);
            }
            for (auto &s : c.smp) ID3D11DeviceContext_PSSetSamplers(ctx, s.first, 1, s.second ? &sCmp : &sLin);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, round >= 8 ? &cbTrans : med ? &cbMed : &cbOff);
            std::vector<std::vector<uint8_t>> out[2];
            std::vector<uint8_t> depth;
            for (int pass = 0; pass < 2; pass++) {
                std::vector<ID3D11Buffer *> cbs;
                for (size_t i = 0; i < c.cbs.size(); i++) {
                    std::vector<float> d = data[i];
                    if (pass == 1 && c.cbs[i].first == c.lightSlot)
                        for (int k = 0; k < 3; k++) d[(size_t)c.lightRow * 4 + k] = 0;
                    ID3D11Buffer *b = MakeCb(d.data(), (UINT)(d.size() * 4));
                    ID3D11DeviceContext_PSSetConstantBuffers(ctx, c.cbs[i].first, 1, &b);
                    cbs.push_back(b);
                }
                const FLOAT clear[4] = {-7, -7, -7, -7};
                for (int i = 0; i < 4; i++) ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv[i], clear);
                ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, rtv, dsv);
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, dss, 0);
                ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
                ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
                ID3D11DeviceContext_Draw(ctx, 32 * 32 * 6, 0);
                ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
                for (int i = 0; i < 4; i++) out[pass].push_back(ReadBack(rt[i], 16));
                if (pass == 0) depth = ReadBack(ds, 4);
                for (auto b : cbs) ID3D11Buffer_Release(b);
            }
            dbg += DrainMessages(c.hash.c_str());
            const float *c1 = (const float *)out[0][0].data(), *e1 = (const float *)out[0][1].data(), *a1 = (const float *)out[0][2].data(),
                        *z1 = (const float *)out[0][3].data(), *c2 = (const float *)out[1][0].data(), *e2 = (const float *)out[1][1].data(),
                        *a2 = (const float *)out[1][2].data(), *dz = (const float *)depth.data();
            for (int p = 0; p < W * H; p++) {
                if (c1[p * 4] == -7.0f) continue;
                drawn++;
                bool off = false, left = false, aoff = false, any = false;
                for (int k = 0; k < 3; k++) {
                    float x = c1[p * 4 + k], y = c2[p * 4 + k], al = a1[p * 4 + k], e = e1[p * 4 + k];
                    if (!isfinite(x) || !isfinite(y)) continue;
                    double want = (double)al * e, got = (double)x - y;
                    double d = fabs(got - want) / std::max(1.0, (double)fabsf(x));
                    (med ? worstMed : worst) = std::max(med ? worstMed : worst, d);
                    if (d > 1e-4) off = true;
                    if (med && round == 4 && x != plain[(size_t)p * 4 + k]) medChanged++;
                    if (e2[p * 4 + k] != 0.0f) left = true;
                    if (memcmp(&a1[p * 4 + k], &a2[p * 4 + k], 4)) aoff = true;
                    if (e > 0) any = true;
                }
                if (off) (med ? medOff : diffOff)++;
                if (left) lightLeft++;
                if (aoff) albedoOff++;
                if (any) lit++;
                if (fabsf(z1[p * 4] - dz[p]) > 1e-6f) zOff++;
                if (!(e1[p * 4 + 3] > 0) || !isfinite(e1[p * 4 + 3])) distBad++;
            }
            if (round == 0) plain.assign(c1, c1 + (size_t)W * H * 4);
            for (auto x : srvs)
                if (x) ID3D11ShaderResourceView_Release(x);
        }
        // Medium must change the colour (else this checked mode 0 twice)
        bool ok = drawn > 0 && diffOff == 0 && medOff == 0 && medChanged > 0 && lightLeft == 0 && albedoOff == 0 && zOff == 0 && distBad == 0 &&
                  dbg == 0 && lit > 0;
        if (!ok) failed++;
        worstAll = std::max(worstAll, std::max(worst, worstMed));
        printf("A %s %s %-6s drawn %d lit %5.1f%%  colour - albedo*light off %d (worst %.1e), with Medium off %d (worst %.1e, colour "
               "changed %d)  light without _LightColor0 %d  albedo changed %d  depth off %d  distance bad %d  debug %d\n",
               c.hash.c_str(), c.target.c_str(), ok ? "ok" : "FAILED", drawn, 100.0 * lit / std::max(1, drawn), diffOff, worst, medOff, worstMed,
               medChanged, lightLeft, albedoOff, zOff, distBad, dbg);
        ID3D11VertexShader_Release(vs);
        ID3D11PixelShader_Release(ps);
    }
    printf("part A: %d programs, %d failed, worst %.1e: %s\n", cases, failed, worstAll, failed || !cases ? "FAILED" : "PASSED");
    for (int i = 0; i < 4; i++) ID3D11RenderTargetView_Release(rtv[i]), ID3D11Texture2D_Release(rt[i]);
    ID3D11DepthStencilView_Release(dsv);
    ID3D11Texture2D_Release(ds);
    return failed || !cases ? 1 : 0;
}

// ---- part B ----------------------------------------------------------------------------------------------------------

static const char *kSceneHlsl = R"(
cbuffer P : register(b0) { float4 rect; float4 p; };  // rect: x0 y0 x1 y1 in pixels; p: x z, y mode (0 base, 1 add), zw size
float4 VS(uint i : SV_VertexID) : SV_Position {
    float2 c = float2((i == 1 || i == 4 || i == 5) ? rect.z : rect.x, (i == 2 || i == 3 || i == 5) ? rect.w : rect.y);
    return float4(c.x / p.z * 2 - 1, 1 - c.y / p.w * 2, p.x, 1);
}
struct O { float4 c : SV_Target0; float4 e : SV_Target1; float4 a : SV_Target2; float z : SV_Target3; };
O PSSkin(float4 pos : SV_Position) {
    uint2 q = uint2(pos.xy);
    float3 alb = float3(0.8, 0.55, 0.45);
    uint eyeW = p.z > p.w * 1.5 ? (uint)p.z / 2 : (uint)p.z;
    float l = (q.x % eyeW) >= eyeW / 2 ? 1.0 : 0.0;
    l *= q.y < 128 ? 1.0 : 0.5;
    float3 e = LIGHT_SCALE * l * float3(1.0, 0.9, 0.8);
    float dist = q.y >= 192 ? 0.105 : 0.1;
    O o;
    if (p.y == 0) {
        o.c = float4(alb * e + 0.05 * alb, 1);
        o.e = float4(e, dist);
        o.a = float4(alb, REGIONAL && q.y < 128 ? .6 : 1.0);
        o.z = pos.z;
    } else {
        float3 e2 = LIGHT_SCALE * ((q.y % 64) < 32 ? 0.25 : 0.0);
        o.c = float4(alb * e2, 0);
        o.e = float4(e2, 777);
        o.a = float4(0, 0, 1, 1);
        o.z = 0.123;
    }
    return o;
}
float4 PSFlat(float4 pos : SV_Position) : SV_Target { return float4(0.1, 0.9, 0.1, 1); }
)";

static float FromHalf(uint16_t h) { return DirectX::PackedVector::XMConvertHalfToFloat(h); }
static float Half(float x) { return FromHalf(DirectX::PackedVector::XMConvertFloatToHalf(x)); }
static float SrgbDecode(uint8_t b) {
    float c = b / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static float SrgbEncode(float l) {
    l = std::min(1.0f, std::max(0.0f, l));
    return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1 / 2.4f) - 0.055f;
}
static float Q8(float l) { return SrgbDecode((uint8_t)lrintf(SrgbEncode(l) * 255.0f)); }  // through an 8-bit sRGB target

// CPU taps, as sss.cpp FillCb
struct Taps {
    double o[21], I[21][6], invTwoVar[6], w[3][6];
};
static Taps MakeTaps(double width) {
    static const double kVar[6] = {0.0064, 0.0484, 0.187, 0.567, 1.99, 7.41};
    static const double kW[3][6] = {{0.233, 0.100, 0.118, 0.113, 0.358, 0.078},
                                    {0.455, 0.336, 0.198, 0.007, 0.004, 0.000},
                                    {0.649, 0.344, 0.000, 0.007, 0.000, 0.000}};
    Taps t;
    double sig[6], maxS = 0;
    for (int k = 0; k < 6; k++) {
        double v = kVar[k] * width * width;
        sig[k] = sqrt(v), maxS = std::max(maxS, sig[k]);
        t.invTwoVar[k] = (float)(1.0 / (2.0 * v));
        for (int ch = 0; ch < 3; ch++) t.w[ch][k] = (float)kW[ch][k];
    }
    for (int i = 0; i < 21; i++) {
        double s = (i - 10) / 10.0;
        t.o[i] = (float)(3.0 * maxS * s * fabs(s));
    }
    auto Phi = [](double x) { return 0.5 * erfc(-x / sqrt(2.0)); };
    for (int i = 0; i < 21; i++) {
        double lo = i == 0 ? -1e30 : 0.5 * (t.o[i - 1] + t.o[i]), hi = i == 20 ? 1e30 : 0.5 * (t.o[i] + t.o[i + 1]);
        for (int k = 0; k < 6; k++) t.I[i][k] = (float)(Phi(hi / sig[k]) - Phi(lo / sig[k]));
    }
    return t;
}

struct Px {
    float r, g, b, w;
};

static void BlurCpu(const std::vector<Px> &in, std::vector<Px> &out, int W, int H, int eyes, double radPerPx, const Taps &t, int dx, int dy) {
    out.assign(in.size(), Px{0, 0, 0, 0});
    int eyeW = W / eyes;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const Px &c = in[(size_t)y * W + x];
            if (c.w <= 0) continue;
            int x0 = (x / eyeW) * eyeW, x1 = x0 + eyeW - 1;
            float radius=sqrtf(getenv("REGIONAL_SSS_TEST") && y<128 ? .6f : 1.f);
            float pxPerMM = radius / (c.w * 1000.0f * (float)radPerPx);
            double acc[3] = {0, 0, 0}, ws[3] = {0, 0, 0};
            for (int i = 0; i < 21; i++) {
                float off = (float)t.o[i] * pxPerMM;
                int qx = (int)floorf(x + 0.5f + dx * off), qy = (int)floorf(y + 0.5f + dy * off);
                if (qx < x0 || qx > x1 || qy < 0 || qy > H - 1) continue;
                const Px &s = in[(size_t)qy * W + qx];
                if (s.w <= 0) continue;
                double dz = (s.w - c.w) * 1000.0 / radius, dz2 = dz * dz;
                double f[6];
                for (int k = 0; k < 6; k++) f[k] = t.I[i][k] * exp(-dz2 * t.invTwoVar[k]);
                const float sv[3] = {s.r, s.g, s.b};
                for (int ch = 0; ch < 3; ch++) {
                    double w = 0;
                    for (int k = 0; k < 6; k++) w += f[k] * t.w[ch][k];
                    acc[ch] += w * sv[ch];
                    ws[ch] += w;
                }
            }
            Px &o = out[(size_t)y * W + x];
            o.r = Half((float)(acc[0] / std::max(ws[0], 1e-12)));
            o.g = Half((float)(acc[1] / std::max(ws[1], 1e-12)));
            o.b = Half((float)(acc[2] / std::max(ws[2], 1e-12)));
            o.w = Half(c.w);
        }
}

struct StateSnap {
    ID3D11RenderTargetView *rtv[8];
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *bs;
    FLOAT f[4];
    UINT mask;
    ID3D11DepthStencilState *ds;
    UINT ref;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11ComputeShader *cs;
    ID3D11ShaderResourceView *psSrv[4], *csSrv[3];
    ID3D11UnorderedAccessView *uav;
    ID3D11Buffer *psCb, *csCb;
    ID3D11RasterizerState *rs;
    UINT nvp;
    D3D11_VIEWPORT vp[4];
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11InputLayout *il;
};

static void Snap(StateSnap &s) {
    memset(&s, 0, sizeof s);
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 8, s.rtv, &s.dsv);
    ID3D11DeviceContext_OMGetBlendState(ctx, &s.bs, s.f, &s.mask);
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &s.ds, &s.ref);
    ID3D11DeviceContext_VSGetShader(ctx, &s.vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(ctx, &s.ps, NULL, NULL);
    ID3D11DeviceContext_CSGetShader(ctx, &s.cs, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 4, s.psSrv);
    ID3D11DeviceContext_CSGetShaderResources(ctx, 0, 3, s.csSrv);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(ctx, 0, 1, &s.uav);
    ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &s.psCb);
    ID3D11DeviceContext_CSGetConstantBuffers(ctx, 0, 1, &s.csCb);
    ID3D11DeviceContext_RSGetState(ctx, &s.rs);
    s.nvp = 4;
    ID3D11DeviceContext_RSGetViewports(ctx, &s.nvp, s.vp);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &s.topo);
    ID3D11DeviceContext_IAGetInputLayout(ctx, &s.il);
    // the snapshot only compares pointers: drop the references at once
    IUnknown *all[] = {(IUnknown *)s.dsv, (IUnknown *)s.bs, (IUnknown *)s.ds, (IUnknown *)s.vs, (IUnknown *)s.ps, (IUnknown *)s.cs,
                       (IUnknown *)s.uav, (IUnknown *)s.psCb, (IUnknown *)s.csCb, (IUnknown *)s.rs, (IUnknown *)s.il};
    for (IUnknown *u : all)
        if (u) IUnknown_Release(u);
    for (auto v : s.rtv)
        if (v) IUnknown_Release((IUnknown *)v);
    for (auto v : s.psSrv)
        if (v) IUnknown_Release((IUnknown *)v);
    for (auto v : s.csSrv)
        if (v) IUnknown_Release((IUnknown *)v);
}

static bool SameState(const StateSnap &a, const StateSnap &b, const char *when) {
    bool ok = !memcmp(a.rtv, b.rtv, sizeof a.rtv) && a.dsv == b.dsv && a.bs == b.bs && !memcmp(a.f, b.f, sizeof a.f) && a.mask == b.mask &&
              a.ds == b.ds && a.ref == b.ref && a.vs == b.vs && a.ps == b.ps && a.cs == b.cs && !memcmp(a.psSrv, b.psSrv, sizeof a.psSrv) &&
              !memcmp(a.csSrv, b.csSrv, sizeof a.csSrv) && a.uav == b.uav && a.psCb == b.psCb && a.csCb == b.csCb && a.rs == b.rs &&
              a.nvp == b.nvp && !memcmp(a.vp, b.vp, sizeof(D3D11_VIEWPORT) * a.nvp) && a.topo == b.topo && a.il == b.il;
    if (!ok) printf("    state not put back after %s\n", when);
    return ok;
}

struct Variant {
    const char *name;
    UINT w, h;
    DXGI_FORMAT colorRes, colorRtv, depthRes, depthDsv;
};

static bool regionalTest=false;
static float lightScale=1.0f;
static int PartB() {
    char lightDefine[80];
    sprintf_s(lightDefine, "#define LIGHT_SCALE %.9f\n", lightScale);
    std::string scene=std::string(lightDefine)+(regionalTest?"#define REGIONAL 1\n":"#define REGIONAL 0\n")+std::string(kSceneHlsl);
    std::vector<char> vsc = Compile(scene.c_str(), "VS", "vs_5_0"), skc = Compile(scene.c_str(), "PSSkin", "ps_5_0"),
                      flc = Compile(scene.c_str(), "PSFlat", "ps_5_0");
    if (vsc.empty() || skc.empty() || flc.empty()) return 1;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *psSkin = NULL, *psFlat = NULL;
    ID3D11Device_CreateVertexShader(dev, vsc.data(), vsc.size(), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, skc.data(), skc.size(), NULL, &psSkin);
    ID3D11Device_CreatePixelShader(dev, flc.data(), flc.size(), NULL, &psFlat);
    std::string err;
    if (!SssInit(dev, (SssCreatePSFn)dev->lpVtbl->CreatePixelShader, err)) {
        printf("SssInit: %s\n", err.c_str());
        return 1;
    }
    const float tanHalfY = 1.17f, width = 1.0f, strength = 1.0f;
    SssSetParams(strength, width, tanHalfY);
    // 0.20.0: SSS_TRANSLUCENCY=1 runs the translucency passes too (this scene's skin writes no SV_Target4: nothing may
    // change, the debug layer checks the passes' bindings and the state put back)
    if (getenv("SSS_TRANSLUCENCY")) SssSetTranslucency(1.0f, 1.0f);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11DepthStencilState *dsLess = NULL, *dsEqual = NULL;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &dsLess);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO, dd.DepthFunc = D3D11_COMPARISON_EQUAL;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &dsEqual);
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO, bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState *bsAdd = NULL;
    ID3D11Device_CreateBlendState(dev, &bd, &bsAdd);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = NULL;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
    // the game's other state, which the module must leave alone
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11Texture2D *dummy = Tex(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE),
                    *dummyU = Tex(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS);
    ID3D11ShaderResourceView *dummySrv = NULL;
    ID3D11UnorderedAccessView *dummyUav = NULL;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)dummy, NULL, &dummySrv);
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)dummyU, NULL, &dummyUav);

    const Variant variants[] = {
        {"both eyes, sRGB 8-bit colour, D24S8", 512, 256, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R24G8_TYPELESS,
         DXGI_FORMAT_D24_UNORM_S8_UINT},
        {"both eyes, half-float colour, D32", 512, 256, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_TYPELESS,
         DXGI_FORMAT_D32_FLOAT},
        {"both eyes, half-float colour, D32S8", 512, 256, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
         DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT},
        {"one eye, sRGB 8-bit colour, D24S8", 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_D24_UNORM_S8_UINT,
         DXGI_FORMAT_D24_UNORM_S8_UINT},
    };
    Taps taps = MakeTaps(width);
    int failed = 0;
    for (const Variant &v : variants) {
        const int W = v.w, H = v.h, eyes = W > H * 3 / 2 ? 2 : 1, eyeW = W / eyes;
        bool srgb = v.colorRtv == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        ID3D11Texture2D *color = Tex(W, H, v.colorRes, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D *depth = Tex(W, H, v.depthRes, D3D11_BIND_DEPTH_STENCIL);
        D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
        rvd.Format = v.colorRtv, rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
        dvd.Format = v.depthDsv, dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView *rtv = NULL;
        ID3D11DepthStencilView *dsv = NULL;
        if (!color || !depth || FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)color, &rvd, &rtv)) ||
            FAILED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, &dvd, &dsv))) {
            printf("B %s: cannot make the camera's targets\n", v.name);
            failed++;
            continue;
        }
        float cb[8] = {0, 16, (float)W, 240, 0.6f, 0, (float)W, (float)H};
        ID3D11Buffer *cbSkin = MakeCb(cb, 32), *cbAdd = NULL, *cbOcc = NULL;
        cb[5] = 1;
        cbAdd = MakeCb(cb, 32);
        float co[8] = {40, 40, 60, 60, 0.3f, 0, (float)W, (float)H};
        cbOcc = MakeCb(co, 32);

        // the frame: the game's state
        const FLOAT bg[4] = {0.2f, 0.3f, 0.4f, 1};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, bg);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        D3D11_VIEWPORT vp = {0, 0, (FLOAT)W, (FLOAT)H, 0, 1};
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(ctx, rs);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 1, &dummySrv);
        ID3D11DeviceContext_CSSetShaderResources(ctx, 1, 1, &dummySrv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &dummyUav, NULL);
        ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbOcc);
        bool stateOk = true;

        SssBegin((unsigned)eyes);  // the views side by side (native: the eye camera's view count)
        // not one colour target with depth: refused, nothing changed
        {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            StateSnap s0, s1;
            Snap(s0);
            SssDrawSave sv;
            bool bound = SssBeforeDraw(ctx, sv);
            Snap(s1);
            if (bound) {
                printf("    bound without a depth target\n");
                stateOk = false;
            }
            stateOk &= SameState(s0, s1, "a refused draw");
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
        }
        // the skin's base pass
        ID3D11DeviceContext_PSSetShader(ctx, psSkin, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbSkin);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbSkin);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, dsLess, 0);
        const FLOAT zero[4] = {0, 0, 0, 0};
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, zero, 0xffffffff);
        int bound = 0;
        {
            StateSnap s0, s1;
            Snap(s0);
            SssDrawSave sv;
            if (SssBeforeDraw(ctx, sv)) {
                bound++;
                ID3D11DeviceContext_Draw(ctx, 6, 0);
                SssAfterDraw(ctx, sv);
            }
            Snap(s1);
            stateOk &= SameState(s0, s1, "the base draw");
        }
        // an additive light pass
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbAdd);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbAdd);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, dsEqual, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, bsAdd, zero, 0xffffffff);
        {
            StateSnap s0, s1;
            Snap(s0);
            SssDrawSave sv;
            if (SssBeforeDraw(ctx, sv)) {
                bound++;
                ID3D11DeviceContext_Draw(ctx, 6, 0);
                SssAfterDraw(ctx, sv);
            }
            Snap(s1);
            stateOk &= SameState(s0, s1, "the light draw");
        }
        // something else in front of the skin
        ID3D11DeviceContext_PSSetShader(ctx, psFlat, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbOcc);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbOcc);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, dsLess, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, zero, 0xffffffff);
        ID3D11DeviceContext_Draw(ctx, 6, 0);
        // the colour before the passes
        std::vector<uint8_t> before = ReadBack(color, srgb ? 4 : 8);
        {
            StateSnap s0, s1;
            Snap(s0);
            SssEnd(ctx, (SssSetPSFn)ctx->lpVtbl->PSSetShader);
            Snap(s1);
            stateOk &= SameState(s0, s1, "SssEnd");
        }
        SssFrame();
        std::vector<uint8_t> after = ReadBack(color, srgb ? 4 : 8);
        if (getenv("SSS_DUMP")) {  // the intermediate targets at a few pixels
            ID3D11Texture2D *t[8];
            SssTestTargets(t);
            std::vector<uint8_t> e = ReadBack(t[0], 8), z = ReadBack(t[2], 4), pp = ReadBack(t[3], 8), bb = ReadBack(t[5], 8),
                                 dc = ReadBack(t[7], v.depthRes == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4);
            const int at[][2] = {{128, 16}, {100, 100}, {130, 100}};
            for (auto &q : at) {
                size_t p = (size_t)q[1] * W + q[0];
                const uint16_t *eh = (const uint16_t *)e.data() + p * 4, *ph = (const uint16_t *)pp.data() + p * 4, *bh = (const uint16_t *)bb.data() + p * 4;
                uint32_t draw = v.depthRes == DXGI_FORMAT_R32G8X24_TYPELESS ? ((const uint32_t *)dc.data())[p * 2] : ((const uint32_t *)dc.data())[p];
                float dv;
                memcpy(&dv, &draw, 4);
                printf("    dump (%d,%d): E %.4f %.4f %.4f %.4f  Z %.7f  depth raw %08x (%.7f)  prep %.4f %.4f  blur %.4f %.4f\n", q[0], q[1], FromHalf(eh[0]),
                       FromHalf(eh[1]), FromHalf(eh[2]), FromHalf(eh[3]), ((const float *)z.data())[p], draw, dv, FromHalf(ph[0]), FromHalf(ph[3]),
                       FromHalf(bh[0]), FromHalf(bh[3]));
            }
        }
        int dbg = DrainMessages(v.name);

        // the CPU reference
        auto get = [&](const std::vector<uint8_t> &img, int p, int k) -> float {
            if (srgb) return SrgbDecode(img[(size_t)p * 4 + k]);
            return FromHalf(((const uint16_t *)img.data())[(size_t)p * 4 + k]);
        };
        const float alb[3] = {0.8f, 0.55f, 0.45f}, lc[3] = {1.0f, 0.9f, 0.8f};
        std::vector<Px> E((size_t)W * H, Px{0, 0, 0, 0}), t1, b1;
        std::vector<bool> skin((size_t)W * H, false);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                bool inSkin = y >= 16 && y < 240, inOcc = x >= 40 && x < 60 && y >= 40 && y < 60;
                if (!inSkin || inOcc) continue;
                skin[(size_t)y * W + x] = true;
                float l = (x % eyeW) >= eyeW / 2 ? 1.0f : 0.0f;
                l *= lightScale * (y < 128 ? 1.0f : 0.5f);
                float e2 = lightScale * ((y % 64) < 32 ? 0.25f : 0.0f);
                Px &e = E[(size_t)y * W + x];
                e.r = Half(Half(l * lc[0]) + e2), e.g = Half(Half(l * lc[1]) + e2), e.b = Half(Half(l * lc[2]) + e2);
                e.w = Half(y >= 192 ? 0.105f : 0.1f);
            }
        double radPerPx = 2.0 * tanHalfY / H;
        BlurCpu(E, t1, W, H, eyes, radPerPx, taps, 1, 0);
        BlurCpu(t1, b1, W, H, eyes, radPerPx, taps, 0, 1);
        int off = 0, nonSkinChanged = 0, leak = 0, redVsBlue = 0, redChecks = 0;
        double worst = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int p = y * W + x;
                bool s = skin[p];
                for (int k = 0; k < 3; k++) {
                    float c0 = get(before, p, k), c1 = get(after, p, k);
                    if (!s) {
                        if (memcmp(&before[(size_t)p * (srgb ? 4 : 8)], &after[(size_t)p * (srgb ? 4 : 8)], srgb ? 4 : 8)) nonSkinChanged++;
                        break;
                    }
                    const float bv[3] = {b1[p].r, b1[p].g, b1[p].b}, ev[3] = {E[p].r, E[p].g, E[p].b};
                    float a = Q8(alb[k]);  // the albedo target is 8-bit sRGB
                    float incidentY = .2126f * std::max(bv[0],ev[0]) + .7152f * std::max(bv[1],ev[1]) + .0722f * std::max(bv[2],ev[2]);
                    float fade = std::min(1.f, std::max(0.f, incidentY / .06f));
                    fade = fade * fade * (3.f - 2.f * fade);
                    float want = std::max(0.0f, c0 + a * fade * strength * (regionalTest && y<128 ? .6f : 1.f) * (bv[k] - ev[k]));
                    double d;
                    bool bad;
                    if (srgb) {
                        int wb = (int)lrintf(SrgbEncode(want) * 255.0f), gb = after[(size_t)p * 4 + k];
                        d = abs(wb - gb);
                        bad = d > 1;
                    } else {
                        // 16-bit float: the compute passes' stores may round toward zero (one step, 2^-11 relative, per
                        // stage: blur across, blur down, the colour)
                        d = fabs(c1 - want) / std::max(1.0f, fabsf(want));
                        bad = d > (lightScale < .1f ? 5e-5 : 4e-3);
                    }
                    if (bad && off < 6)
                        printf("    (%d,%d).%d before %.5f after %.5f want %.5f  blur %.5f light %.5f dist %.4f\n", x, y, k, c0, c1, want, bv[k], ev[k],
                               E[p].w);
                    if (bad) off++;
                    worst = std::max(worst, d);
                }
                // the other eye's light must not come over: the first pixels of the right eye (dark there; rows whose
                // neighbours above and below have the same light)
                if (eyes == 2 && s && x >= eyeW && x < eyeW + 12 && y >= 26 && y < 182 && (y % 32) >= 10 && (y % 32) < 22) {
                    for (int k = 0; k < 3; k++)
                        if (get(after, p, k) > get(before, p, k) + (srgb ? 0.0f : 1e-4f)) {
                            leak++;
                            break;
                        }
                }
                // just inside the shadow next to the lit half (no extra light rows, no square): red spreads farther
                // The first pixel in shadow still receives substantial light from
                // the lit side. Far tails are intentionally suppressed in 0.22.5.
                if (lightScale >= .1f && s && y >= 106 && y < 119 && (y % 64) >= 32 && x % eyeW == eyeW / 2 - 1) {
                    redChecks++;
                    float dr = (get(after, p, 0) - get(before, p, 0)) / (alb[0] * lc[0]);
                    float db = (get(after, p, 2) - get(before, p, 2)) / (alb[2] * lc[2]);
                    if (dr > db && dr > 0) redVsBlue++;
                }
            }
        std::string st = SssStatus();
        bool ok = bound == 2 && off == 0 && nonSkinChanged == 0 && leak == 0 && (lightScale < .1f || redChecks > 0) && redVsBlue == redChecks && stateOk && dbg == 0 &&
                  st.find("\"bound\":2") != std::string::npos;
        if (!ok) failed++;
        printf("B %-40s %-6s bound %d  off %d (worst %.3g%s)  non-skin changed %d  other eye's light %d  red farther than blue %d/%d  "
               "state %s  debug %d\n      %s\n",
               v.name, ok ? "ok" : "FAILED", bound, off, worst, srgb ? " steps" : " rel", nonSkinChanged, leak, redVsBlue, redChecks,
               stateOk ? "kept" : "CHANGED", dbg, st.c_str());
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(rtv);
        ID3D11DepthStencilView_Release(dsv);
        ID3D11Texture2D_Release(color);
        ID3D11Texture2D_Release(depth);
        ID3D11Buffer_Release(cbSkin), ID3D11Buffer_Release(cbAdd), ID3D11Buffer_Release(cbOcc);
    }
    printf("part B: %d variants, %d failed: %s\n", (int)(sizeof variants / sizeof variants[0]), failed, failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}

#include "legacy_guides_test.inl"
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: sss_test <spec_cases dir>\n");
        return 2;
    }
    UINT flags = D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        flags = 0;
        if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
            printf("no D3D11 device\n");
            return 1;
        }
    }
    if (flags) ID3D11Device_QueryInterface(dev, IID_ID3D11InfoQueue, (void **)&info);
    printf("debug layer: %s\n", info ? "on" : "not installed");
    regionalTest=getenv("REGIONAL_SSS_TEST")!=nullptr;
    if (getenv("SSS_LIGHT_SCALE")) lightScale=(float)atof(getenv("SSS_LIGHT_SCALE"));
    printf("light scale: %.6f\n", lightScale);
    printf("regional SSS reference: %s\n",regionalTest?"on":"off");
    int b = PartB();
    int c = PartC();
    int a = getenv("SSS_PART_B_ONLY") ? 0 : PartA(argv[1]);
    printf("%s\n", a || b || c ? "FAILED" : "PASSED");
    return a || b || c;
}
