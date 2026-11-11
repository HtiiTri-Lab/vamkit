// Offline test of the clothing replacement programs (tools/gen_cloth_programs.py -> <repo>/out/gen/cloth-shading/cloth_cases/), after the skin
// plugin's spec_test: for each of VaM's clothing pixel programs, the original and its replacement draw the same 32 x 32
// grid of random inputs (the case's vertex shader) with the same random constant buffers and textures into a float
// target. Mode 0 (b12 = 0) must give the original's result; mode 1 (cloth, the plugin's defaults) must change pixels
// and give no NaN or infinity where the original has none. The debug layer (when installed) checks that every program
// links with the vertex shader and that the bindings are valid.
//
// usage: cloth_test <generated folder, <repo>/out/gen/cloth-shading> [hash ...]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

static const int W = 256, H = 256;
static const int kRounds = 4;
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
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            if (errors < 4) printf("    debug layer (%s): %.*s\n", when, (int)m->DescriptionByteLength, m->pDescription);
            errors++;
        }
    }
    ID3D11InfoQueue_ClearStoredMessages(info);
    return errors;
}

struct Case {
    std::string hash, target, pass;
    std::vector<std::pair<int, int>> cbs;  // slot, rows
    int eyeSlot = -1, eyeRow = 0, eyeComp = 0;
    struct Tex {
        int slot;
        std::string dim;
        int cmp;
    };
    std::vector<Tex> tex;
    std::vector<std::pair<int, int>> smp;  // slot, comparison
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
    in >> t >> c.pass;
    return t == "pass";
}

static ID3D11ShaderResourceView *MakeTexture(const Case::Tex &x, Rng &r) {
    bool cube = x.dim == "texturecube", vol = x.dim == "texture3d";
    DXGI_FORMAT fmt = x.cmp ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const UINT texel = 4;
    int size = vol ? 16 : cube ? 32 : 64;
    int mips = x.cmp || vol ? 1 : (int)log2((double)size) + 1;
    int faces = cube ? 6 : 1, depth = vol ? size : 1;
    std::vector<std::vector<uint8_t>> data;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    for (int f = 0; f < faces; f++)
        for (int m = 0; m < mips; m++) {
            int s = std::max(1, size >> m), d = vol ? std::max(1, depth >> m) : 1;
            data.emplace_back((size_t)s * s * d * texel);
            std::vector<uint8_t> &v = data.back();
            if (x.cmp)
                for (size_t i = 0; i < v.size() / 4; i++) ((float *)v.data())[i] = r.Uniform(0, 1);
            else
                for (auto &b : v) b = (uint8_t)(r.Next() & 0xff);
        }
    for (int f = 0, i = 0; f < faces; f++)
        for (int m = 0; m < mips; m++, i++) {
            int s = std::max(1, size >> m);
            D3D11_SUBRESOURCE_DATA sd = {data[i].data(), (UINT)(s * texel), (UINT)(s * s * texel)};
            init.push_back(sd);
        }
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

static ID3D11Buffer *MakeCb(const void *data, UINT bytes) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (bytes + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {data, 0, 0};
    ID3D11Buffer *b = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, &sd, &b);
    return b;
}

static std::vector<float> Draw(ID3D11VertexShader *vs, ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, ID3D11Texture2D *rt,
                               ID3D11Texture2D *staging) {
    const FLOAT clear[4] = {-7, -7, -7, -7};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 32 * 32 * 6, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    std::vector<float> out((size_t)W * H * 4);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < H; y++) memcpy(&out[(size_t)y * W * 4], (uint8_t *)m.pData + (size_t)y * m.RowPitch, W * 16);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    return out;
}

static float Diff(float a, float b) {  // relative difference (absolute below 1); NaN against NaN is 0
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b) ? 0.0f : INFINITY;
    if (a == b) return 0.0f;
    return fabsf(a - b) / std::max(1.0f, fabsf(a));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cloth_test <generated folder, out/gen/cloth-shading> [hash ...]\n");
        return 2;
    }
    std::string root = argv[1], dir = root + "\\cloth_cases\\";
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

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL, *staging = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (FLOAT)W, (FLOAT)H, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
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
    // b12: clothCtl (mode, AA variance, AA cap, gain), clothSheen (alpha begin, alpha whole, gain); mode 1 = the
    // plugin's defaults
    const float ctl0[8] = {0}, ctl1[8] = {1, 0.15915494f, 0.18f, 2, 0.25f, 0.5f, 2, 0};
    ID3D11Buffer *cb0 = MakeCb(ctl0, sizeof ctl0), *cb1 = MakeCb(ctl1, sizeof ctl1);

    std::vector<char> man = ReadFile(dir + "manifest.txt");
    std::istringstream lines(std::string(man.begin(), man.end()));
    std::string line;
    int cases = 0, failed = 0;
    float worst0 = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        Case c;
        if (!Parse(line, c)) {
            printf("bad manifest line: %s\n", line.c_str());
            failed++;
            continue;
        }
        bool want = argc <= 2;
        for (int i = 2; i < argc; i++) want |= c.hash == argv[i];
        if (!want) continue;
        cases++;
        std::vector<char> o = ReadFile(dir + c.hash + ".orig.dxbc"), rp = ReadFile(dir + c.hash + ".repl.dxbc"),
                          v = ReadFile(dir + c.hash + ".vs.dxbc");
        ID3D11VertexShader *vs = NULL;
        ID3D11PixelShader *pso = NULL, *psr = NULL;
        if (FAILED(ID3D11Device_CreateVertexShader(dev, v.data(), v.size(), NULL, &vs)) ||
            FAILED(ID3D11Device_CreatePixelShader(dev, o.data(), o.size(), NULL, &pso)) ||
            FAILED(ID3D11Device_CreatePixelShader(dev, rp.data(), rp.size(), NULL, &psr))) {
            printf("%s: cannot create shaders\n", c.hash.c_str());
            failed++;
            continue;
        }
        Rng r = {0x9e3779b97f4a7c15ull ^ std::hash<std::string>()(c.hash)};
        float d0 = 0;
        long badFinite = 0, changed = 0, pixels = 0;
        int debug = 0;
        for (int round = 0; round < kRounds; round++) {
            std::vector<ID3D11Buffer *> cbs;
            for (auto &cb : c.cbs) {
                std::vector<float> d((size_t)cb.second * 4);
                for (auto &x : d) x = r.Uniform(0.0f, 1.0f);
                if (cb.first == c.eyeSlot) ((uint32_t *)d.data())[c.eyeRow * 4 + c.eyeComp] = 0;
                ID3D11Buffer *b = MakeCb(d.data(), (UINT)(d.size() * 4));
                ID3D11DeviceContext_PSSetConstantBuffers(ctx, cb.first, 1, &b);
                cbs.push_back(b);
            }
            std::vector<ID3D11ShaderResourceView *> srvs;
            for (auto &x : c.tex) {
                ID3D11ShaderResourceView *s = MakeTexture(x, r);
                ID3D11DeviceContext_PSSetShaderResources(ctx, x.slot, 1, &s);
                srvs.push_back(s);
            }
            for (auto &s : c.smp) ID3D11DeviceContext_PSSetSamplers(ctx, s.first, 1, s.second ? &sCmp : &sLin);
            std::vector<float> ref = Draw(vs, pso, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cb0);
            std::vector<float> m0 = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cb1);
            std::vector<float> m1 = Draw(vs, psr, rtv, rt, staging);
            debug += DrainMessages(c.hash.c_str());
            for (size_t i = 0; i < ref.size(); i++) {
                if (ref[i] == -7.0f) continue;  // not drawn (discarded)
                pixels++;
                d0 = std::max(d0, Diff(ref[i], m0[i]));
                if (isfinite(ref[i]) && !isfinite(m1[i])) badFinite++;
                if (Diff(ref[i], m1[i]) > 1e-3f) changed++;
            }
            for (auto b : cbs) ID3D11Buffer_Release(b);
            for (auto s : srvs)
                if (s) ID3D11ShaderResourceView_Release(s);
        }
        worst0 = std::max(worst0, d0);
        bool ok = d0 <= 1e-5f && badFinite == 0 && changed > 0 && debug == 0;
        printf("%s %-4s: mode 0 %.1e, mode 1 changed %ld of %ld values, non-finite %ld%s\n", c.hash.c_str(), c.pass.c_str(), d0,
               changed, pixels, badFinite, ok ? "" : "  FAILED");
        if (!ok) failed++;
        ID3D11VertexShader_Release(vs);
        ID3D11PixelShader_Release(pso);
        ID3D11PixelShader_Release(psr);
    }
    printf("%d cases, %d failed, worst mode 0 %.2e: %s\n", cases, failed, worst0, failed || !cases ? "FAILED" : "PASSED");
    return failed || !cases ? 1 : 0;
}
