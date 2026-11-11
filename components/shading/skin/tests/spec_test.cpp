// Offline test of the specular replacement programs (tools\gen_spec_table.py -> <repo>\out\gen\skin\spec_cases\): for each of VaM's
// skin pixel programs, the original and its replacement draw the same 32 x 32 grid of random inputs (the case's
// vertex shader), with the same random constant buffers and random textures (mip chains; single-channel float where a
// comparison sampler reads) into a float target. Mode 0 (b12 = 0) must give the original's result; mode 2 (VaM's own
// specular through the view and light vectors and the specular normal the replacement rebuilds, normal-map bias 0) must
// give it within 1e-3 (relative, or absolute below 1); mode 1 (skin GGX with the default look) must change pixels and
// give no NaN or infinity where the original has none. The diffuse the same way (0.6.0): scatCtl.x = 2 (VaM's own
// diffuse factor from the captured N.L and _SubdermisColor) within 1e-3, 1 (the skin wrap, middle look) changed and
// finite. Teeth and tongue (0.8.0), with _SubdermisColor set to 0.6 and a constant occlusion texture at t16: their own
// Subsurface Color 0.6 must give the original within 1e-3, the occlusion the original times its known value within
// 1e-3, and the default teeth look must change pixels and stay finite. Eyes (0.9.0), with an eye rig at t17 around the
// test's positions: the eyelid occlusion with its points far away must give the original, with a degenerate polyline
// at the eye's centre and a huge width the original times 1 - its upper-lid strength (within 1e-3); the catchlight
// alone must change pixels, never darken one and stay finite. The debug layer (when installed) checks that
// every program links with the vertex shader and that the bindings are valid.
//
// usage: spec_test <spec_cases dir> [hash ...]
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
static const int kRounds = 4;  // random input sets per program
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
        // the subsurface outputs (SV_Target1..3) without targets bound: their writes are dropped, as in the game
        if (m->ID == D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET) continue;
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            if (errors < 4) printf("    debug layer (%s): %.*s\n", when, (int)m->DescriptionByteLength, m->pDescription);
            errors++;
        }
    }
    ID3D11InfoQueue_ClearStoredMessages(info);
    return errors;
}

struct Case {
    std::string hash, target;
    std::vector<std::pair<int, int>> cbs;  // slot, rows
    int eyeSlot = -1, eyeRow = 0, eyeComp = 0;
    struct Tex {
        int slot;
        std::string dim;
        int cmp;
    };
    std::vector<Tex> tex;
    std::vector<std::pair<int, int>> smp;  // slot, comparison
    int subdSlot = -1, subdRow = 0;        // _SubdermisColor
    int envSlot = -1, iblRow = 0, lmRow = 0;
    int filterSlot = -1, filterRow = 0, filterComp = 0;
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
    in >> t;
    if (t != "subd") return false;
    in >> t;
    if (sscanf(t.c_str(), "%d:%d", &c.subdSlot, &c.subdRow) != 2) return false;
    size_t at=line.find(" aoenv ");
    if(at==std::string::npos)return false;
    std::istringstream env(line.substr(at+7));std::string ibl,lm;env>>ibl>>lm;
    if(ibl!="-") {
        int slot;
        if(sscanf(ibl.c_str(),"%d:%d",&c.envSlot,&c.iblRow)!=2 ||
           sscanf(lm.c_str(),"%d:%d",&slot,&c.lmRow)!=2 || slot!=c.envSlot)return false;
    }
    at=line.find(" aofilter ");
    if(at==std::string::npos)return false;
    std::istringstream af(line.substr(at+10));af>>t;
    if(t!="-" && sscanf(t.c_str(),"%d:%d:%d",&c.filterSlot,&c.filterRow,&c.filterComp)!=3)return false;
    return true;
}

static ID3D11ShaderResourceView *MakeTexture(const Case::Tex &x, Rng &r) {
    bool cube = x.dim == "texturecube", vol = x.dim == "texture3d";
    DXGI_FORMAT fmt = x.cmp ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT texel = x.cmp ? 4 : 4;
    int size = vol ? 16 : cube ? 32 : 64;
    // SPEC_TEST_ONE_MIP=1: no mip chains (takes implicit-derivative LOD choice out of the comparison)
    int mips = x.cmp || vol || getenv("SPEC_TEST_ONE_MIP") ? 1 : (int)log2((double)size) + 1;
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
    for (auto &v : data) {
        D3D11_SUBRESOURCE_DATA sd = {v.data(), 0, 0};
        init.push_back(sd);
    }
    // pitches
    for (int f = 0, i = 0; f < faces; f++)
        for (int m = 0; m < mips; m++, i++) {
            int s = std::max(1, size >> m);
            init[i].SysMemPitch = s * texel;
            init[i].SysMemSlicePitch = s * s * texel;
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

// one draw of the grid with this pixel shader; the target's floats
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

static bool Same(float a, float b) {
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    if (a == b) return true;
    return fabsf(a - b) <= 1e-5f * std::max(1.0f, fabsf(a));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: spec_test <spec_cases dir> [hash ...]\n");
        return 2;
    }
    std::string dir = std::string(argv[1]) + "\\";
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
    // b12: specCtl (mode, normal-map mip bias, pore widening, F0), specLobe (roughness scale, sharp / wide factors, weight),
    // scatCtl (diffuse mode), scatA / scatP (per-channel wrap and power; the middle look), teethCtl (on, Subsurface
    // Color, gain), teethOcc (back strength, back start, gum strength, gum power), teethUV (scale, offset)
#define SCAT_MID 1.3808f, 0.8630f, 0.5394f, 0, 3.9291f, 2.9640f, 2.3583f, 0
#define SPEC_SCAT_OFF 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    const float ctlOff[92] = {0}, ctlTest[92] = {2, 0, 1, 0.028f, 1, 0.75f, 1.3f, 0.85f, 0, 0, 0, 0, SCAT_MID},
                ctlScatTest[92] = {0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, SCAT_MID},
                ctlScat[92] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, SCAT_MID},
                ctlSkin[92] = {1, 2.5f, 1, 0.028f, 1, 0.75f, 1.3f, 0.85f, 1, 0, 0, 0, SCAT_MID},
                // skin GGX + wrap with the 0.11.0 skin detail (skinCtl, skinNorm: one roughness for every skin, intensity over
                // its own mean, diffuse normal and cavity filtered to 2 mm, the ambient on the diffuse normal; 0.13.0's
                // tier row at t19 with the gloss map's stats at t18, the row alone, and neither)
                ctlDetail[92] = {1, 2.5f, 1, 0.028f, 1, 0.75f, 1.3f, 0.85f, 1, 0, 0, 0, SCAT_MID, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2f, 0.12f, 2, 1, 0.002f, 1, 0.002f, 1},
                // every channel green's wrap: VaM's diffuse where N.L >= 0, less (no floor) where it is below
                ctlScatNeutral[92] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0.863f, 0.863f, 0.863f, 0, 2.964f, 2.964f, 2.964f, 0},
                // teeth: Subsurface Color 0.6 (= the test's _SubdermisColor), no occlusion; then the occlusion alone (as on
                // the tongue: diffuse factor untouched, Subsurface Color 0 ignored), (1 - 0.5 smoothstep(0.5, 1, 0.75))
                // (1 - 0.4 * 0.5^2) = 0.675 on the constant texture; then a teeth look
                ctlTeethSame[92] = {SPEC_SCAT_OFF, 1, 0.6f, 1, 1, 0, 0.5f, 0, 1, 1, 1, 0, 0},
                ctlTeethOcc[92] = {SPEC_SCAT_OFF, 1, 0, 0, 0, 0.5f, 0.5f, 0.4f, 2, 1, 1, 0, 0},
                ctlTeethLook[92] = {SPEC_SCAT_OFF, 1, 1, 1, 1, 0.6f, 0.3f, 0.5f, 1.5f, 1, 1, 0, 0},
                // eyes (eyeCtl, eyeOcc, eyeCatch, eyeGeo): the eyelid occlusion alone, eye 0 with 2 points, upper lid 0.5 with
                // a width of 100 radii, no lower lid; the catchlight alone (Natural)
                ctlEyeOcc[92] = {SPEC_SCAT_OFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 0.5f, 100, 0, 1, 0, 0, 0, 0, 0.4f},
                ctlEyeCatch[92] = {SPEC_SCAT_OFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 1, 0.05f, 0.025f, 0.6f, 0.4f};
    float ctlTrans[92];memcpy(ctlTrans,ctlScat,sizeof ctlTrans);ctlTrans[9]=0.5f;
    ID3D11Buffer *cbTransHalf=MakeCb(ctlTrans,sizeof ctlTrans);
    ctlTrans[9]=1.0f;ID3D11Buffer *cbTransFull=MakeCb(ctlTrans,sizeof ctlTrans);
    ctlTrans[9]=2.0f;ID3D11Buffer *cbTransMax=MakeCb(ctlTrans,sizeof ctlTrans);
    float ctlRegional[92];memcpy(ctlRegional,ctlScat,sizeof ctlRegional);ctlRegional[9]=1;ctlRegional[10]=1;ctlRegional[28]=ctlRegional[29]=1;
    ID3D11Buffer *cbRegional=MakeCb(ctlRegional,sizeof ctlRegional);
    ctlRegional[9]=0;ID3D11Buffer *cbRegionalOff=MakeCb(ctlRegional,sizeof ctlRegional);
    D3D11_TEXTURE2D_DESC rtDesc={};rtDesc.Width=rtDesc.Height=rtDesc.MipLevels=rtDesc.ArraySize=rtDesc.SampleDesc.Count=1;
    rtDesc.Format=DXGI_FORMAT_R32_FLOAT;rtDesc.Usage=D3D11_USAGE_IMMUTABLE;rtDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    float rw=.5f;D3D11_SUBRESOURCE_DATA rinit={&rw,4,0};ID3D11Texture2D *rtex=nullptr;ID3D11ShaderResourceView *regionalSrv=nullptr;
    ID3D11Device_CreateTexture2D(dev,&rtDesc,&rinit,&rtex);ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)rtex,nullptr,&regionalSrv);ID3D11Texture2D_Release(rtex);
    ID3D11DeviceContext_PSSetShaderResources(ctx,22,1,&regionalSrv);
    float ctlMouth[92]={};ctlMouth[64]=ctlMouth[66]=ctlMouth[68]=ctlMouth[69]=1;
    ID3D11Buffer *cbMouth=MakeCb(ctlMouth,sizeof ctlMouth);
    uint32_t lookup[4]={0,0,0,65535};float visibility=.5f;
    D3D11_TEXTURE2D_DESC md={};md.Width=md.Height=md.MipLevels=md.ArraySize=md.SampleDesc.Count=1;md.Format=DXGI_FORMAT_R32G32B32A32_UINT;md.Usage=D3D11_USAGE_IMMUTABLE;md.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA mi={lookup,16,0};ID3D11Texture2D *mt=nullptr;ID3D11ShaderResourceView *mv=nullptr,*av=nullptr;
    ID3D11Device_CreateTexture2D(dev,&md,&mi,&mt);ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)mt,nullptr,&mv);ID3D11Texture2D_Release(mt);
    D3D11_BUFFER_DESC ad={};ad.ByteWidth=4;ad.Usage=D3D11_USAGE_IMMUTABLE;ad.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA ai={&visibility,0,0};ID3D11Buffer *ab=nullptr;
    ID3D11Device_CreateBuffer(dev,&ad,&ai,&ab);D3D11_SHADER_RESOURCE_VIEW_DESC avd={};avd.Format=DXGI_FORMAT_R32_FLOAT;avd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;avd.Buffer.NumElements=1;
    ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)ab,&avd,&av);ID3D11Buffer_Release(ab);
    ID3D11DeviceContext_PSSetShaderResources(ctx,20,1,&mv);ID3D11DeviceContext_PSSetShaderResources(ctx,21,1,&av);
    const float kTeethOcc = 0.675f, kSubd = 0.6f;
    float ctlNose[92] = {0};
    ctlNose[56] = ctlNose[57] = ctlNose[60] = ctlNose[61] = 1.0f;
    ID3D11Buffer *cbNose = MakeCb(ctlNose, sizeof ctlNose);
    ctlNose[62]=1.0f;ctlNose[63]=0.17f;
    ID3D11Buffer *cbNoseFilter=MakeCb(ctlNose,sizeof ctlNose);
    ctlNose[61]=0.0f;
    ID3D11Buffer *cbNoseFilterOff=MakeCb(ctlNose,sizeof ctlNose);
    ID3D11Buffer *cbOff = MakeCb(ctlOff, 368), *cbTest = MakeCb(ctlTest, 368), *cbScatTest = MakeCb(ctlScatTest, 368),
                 *cbScat = MakeCb(ctlScat, 368), *cbSkin = MakeCb(ctlSkin, 368), *cbDetail = MakeCb(ctlDetail, 368), *cbScatNeutral = MakeCb(ctlScatNeutral, 368),
                 *cbTeethSame = MakeCb(ctlTeethSame, 368), *cbTeethOcc = MakeCb(ctlTeethOcc, 368), *cbTeethLook = MakeCb(ctlTeethLook, 368),
                 *cbEyeOcc = MakeCb(ctlEyeOcc, 368), *cbEyeCatch = MakeCb(ctlEyeCatch, 368);
    // the eye rigs (t17): eye 0 at (0.5, 0.5, 0.5), radius 1 (every test position within its 1.3 radii), eye 1 far away;
    // eye 0's two points 10^4 away (none within the 100-radii width: no occlusion anywhere) or both at its centre (every pixel 1 radius from them: with the
    // 100-radii width the upper lid's full darkening, x (1 - 0.5 exp(-1e-4)))
    ID3D11ShaderResourceView *eyeFar = NULL, *eyeCentre = NULL;
    for (int k = 0; k < 2; k++) {
        float pts[6][4] = {{0.5f, 0.5f, 0.5f, 1}, {0, 0, 1, 1}, {100, 100, 100, 1}, {0, 0, 1, 1}, {0.5f, 0.5f, 0.5f, 3}, {0.5f, 0.5f, 0.5f, 1}};
        if (k == 0) pts[4][0] = pts[4][1] = pts[4][2] = 1e4f, pts[5][0] = 1.0001e4f, pts[5][1] = pts[5][2] = 1e4f;
        D3D11_BUFFER_DESC eb = {};
        eb.ByteWidth = sizeof pts;
        eb.Usage = D3D11_USAGE_IMMUTABLE;
        eb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA ei = {pts, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC ev = {};
        ev.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ev.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        ev.Buffer.NumElements = 6;
        ID3D11Buffer *b = NULL;
        ID3D11Device_CreateBuffer(dev, &eb, &ei, &b);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)b, &ev, k == 0 ? &eyeFar : &eyeCentre);
        ID3D11Buffer_Release(b);
    }
    ID3D11ShaderResourceView *noSrv = NULL;
    // a gloss map's stats (t18): p50 0.3, p90 0.5
    ID3D11ShaderResourceView *glossStats = NULL;
    {
        float gs[4] = {0.3f, 0.2f, 1, 0.1f};  // p50, p90 - p50, there, p10
        D3D11_BUFFER_DESC gb = {};
        gb.ByteWidth = sizeof gs;
        gb.Usage = D3D11_USAGE_IMMUTABLE;
        gb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA gi = {gs, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC gv = {};
        gv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        gv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        gv.Buffer.NumElements = 1;
        ID3D11Buffer *b = NULL;
        ID3D11Device_CreateBuffer(dev, &gb, &gi, &b);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)b, &gv, &glossStats);
        ID3D11Buffer_Release(b);
    }
    // a tier row (t19): alpha^2 0.2 at p50, R 1.7 (-2 log2 1.7), floor 0.024, ceiling 0.4; the gloss map 0.5 mips
    // blurred, the highlight x 1.2, the spread at least 0.03 (a body row)
    ID3D11ShaderResourceView *tierRow = NULL;
    {
        float tr[8] = {0.2f, -1.5310f, 0.024f, 0.4f, 0.5f, 1.2f, 0.03f, 0};
        D3D11_BUFFER_DESC tb = {};
        tb.ByteWidth = sizeof tr;
        tb.Usage = D3D11_USAGE_IMMUTABLE;
        tb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA ti = {tr, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC tv = {};
        tv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        tv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        tv.Buffer.NumElements = 2;
        ID3D11Buffer *b = NULL;
        ID3D11Device_CreateBuffer(dev, &tb, &ti, &b);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)b, &tv, &tierRow);
        ID3D11Buffer_Release(b);
    }
    // 0.18.0 rows of a gloss map's own (three float4): the row above with its own skin stats (p50 0.3, spread 0.2 = t18's)
    // and the cavity's divisor 3; then gain 0.5, gain 0 (no highlight), divisor 1.5, p50 0.6
    ID3D11ShaderResourceView *ownRow[5] = {};
    {
        const float rows[5][12] = {{0.2f, -1.5310f, 0.024f, 0.4f, 0.5f, 1.0f, 0.03f, 0, 0.3f, 0.2f, 3.0f, 1},
                                   {0.2f, -1.5310f, 0.024f, 0.4f, 0.5f, 0.5f, 0.03f, 0, 0.3f, 0.2f, 3.0f, 1},
                                   {0.2f, -1.5310f, 0.024f, 0.4f, 0.5f, 0.0f, 0.03f, 0, 0.3f, 0.2f, 3.0f, 1},
                                   {0.2f, -1.5310f, 0.024f, 0.4f, 0.5f, 1.0f, 0.03f, 0, 0.3f, 0.2f, 1.5f, 1},
                                   {0.2f, -1.5310f, 0.024f, 0.4f, 0.5f, 1.0f, 0.03f, 0, 0.6f, 0.2f, 3.0f, 1}};
        for (int k = 0; k < 5; k++) {
            D3D11_BUFFER_DESC tb = {};
            tb.ByteWidth = sizeof rows[k];
            tb.Usage = D3D11_USAGE_IMMUTABLE;
            tb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA ti = {rows[k], 0, 0};
            D3D11_SHADER_RESOURCE_VIEW_DESC tv = {};
            tv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            tv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            tv.Buffer.NumElements = 3;
            ID3D11Buffer *b = NULL;
            ID3D11Device_CreateBuffer(dev, &tb, &ti, &b);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)b, &tv, &ownRow[k]);
            ID3D11Buffer_Release(b);
        }
    }
    int ownBad = 0, ownSpec = 0, ownP50 = 0;
    // the teeth occlusion texture (t16, s15): back 0.75, gum 0.5 everywhere
    {
        uint16_t texels[4 * 4 * 2];
        for (int i = 0; i < 16; i++) texels[2 * i] = 0x3a00, texels[2 * i + 1] = 0x3800;  // half 0.75, 0.5
        D3D11_TEXTURE2D_DESC tt = {};
        tt.Width = tt.Height = 4;
        tt.MipLevels = tt.ArraySize = 1;
        tt.Format = DXGI_FORMAT_R16G16_FLOAT;
        tt.SampleDesc.Count = 1;
        tt.Usage = D3D11_USAGE_IMMUTABLE;
        tt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {texels, 4 * 4, 0};
        ID3D11Texture2D *t = NULL;
        ID3D11ShaderResourceView *srv = NULL;
        ID3D11Device_CreateTexture2D(dev, &tt, &init, &t);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &srv);
        ID3D11Texture2D_Release(t);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 16, 1, &srv);
        D3D11_SAMPLER_DESC sc = sdLin;
        sc.AddressU = sc.AddressV = sc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ID3D11SamplerState *sClamp = NULL;
        ID3D11Device_CreateSamplerState(dev, &sc, &sClamp);
        ID3D11DeviceContext_PSSetSamplers(ctx, 15, 1, &sClamp);
    }
    DrainMessages("setup");

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
        if (argc > 2) {
            bool want = false;
            for (int i = 2; i < argc; i++) want |= c.hash == argv[i];
            if (!want) continue;
        }
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
        // several rounds of random inputs; mode 0 must match the original in every round, mode 2 within 1e-3, mode 1
        // must change some pixels and stay finite where the original is
        int mismatch = 0, exact = 0, nan = 0, testOff = 0, changedSkin = 0, badSkin = 0, drawn = 0, dbg = 0, px = 0;
        int scatTestOff = 0, changedScat = 0, badScat = 0, neutralAbove = 0, neutralSame = 0;
        int teethOff = 0, occOff = 0, changedTeeth = 0, badTeeth = 0;
        int eyeFarOff = 0, eyeCentreOff = 0, catchBelow = 0, catchChanged = 0, catchBad = 0;
        int changedDetail = 0, badDetail = 0, noseOff = 0;
        int transBad=0,transChanged=0,transMaxChanged=0;
        double worst = 0, worstTest = 0, worstScatTest = 0, worstTeeth = 0, worstOcc = 0, worstEye = 0;
        for (int round = 0; round < kRounds; round++) {
            Rng r = {(std::stoull(c.hash, NULL, 16) + 0x9e3779b97f4a7c15ull * round) | 1};
            std::vector<ID3D11Buffer *> cbs;
            ID3D11Buffer *subdCb = NULL;  // the same with _SubdermisColor 0.6 (teeth)
            ID3D11Buffer *envCb = NULL, *envOriginal = NULL;
            for (auto &cb : c.cbs) {
                std::vector<float> d((size_t)cb.second * 4);
                for (auto &x : d) x = r.Uniform(0.0f, 1.0f);
                if (cb.first == c.eyeSlot) ((uint32_t *)d.data())[c.eyeRow * 4 + c.eyeComp] = 0;
                ID3D11Buffer *b = MakeCb(d.data(), (UINT)(d.size() * 4));
                ID3D11DeviceContext_PSSetConstantBuffers(ctx, cb.first, 1, &b);
                cbs.push_back(b);
                if(cb.first==c.envSlot) {
                    std::vector<float> dark=d;
                    dark[c.iblRow*4]=dark[c.iblRow*4+1]=dark[c.lmRow*4]=0;
                    envCb=MakeCb(dark.data(),(UINT)(dark.size()*4));envOriginal=b;
                }
                if (cb.first == c.subdSlot) {
                    for (int k = 0; k < 3; k++) d[(size_t)c.subdRow * 4 + k] = kSubd;
                    subdCb = MakeCb(d.data(), (UINT)(d.size() * 4));
                }
            }
            std::vector<ID3D11ShaderResourceView *> srvs;
            for (auto &x : c.tex) {
                ID3D11ShaderResourceView *s = MakeTexture(x, r);
                ID3D11DeviceContext_PSSetShaderResources(ctx, x.slot, 1, &s);
                srvs.push_back(s);
            }
            for (auto &s : c.smp) ID3D11DeviceContext_PSSetSamplers(ctx, s.first, 1, s.second ? &sCmp : &sLin);

            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbOff);
            std::vector<float> a = Draw(vs, pso, rtv, rt, staging);
            std::vector<float> b = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbNose);
            std::vector<float> nose = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbMouth);
            std::vector<float> mouth=Draw(vs,psr,rtv,rt,staging),mouthNoEnv=mouth;
            std::vector<float> noEnv=b, noseNoEnv=nose;
            std::vector<float> filterNoEnv=b, filterReference=b;
            if(envCb) {
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,c.envSlot,1,&envCb);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbOff);
                noEnv=Draw(vs,psr,rtv,rt,staging);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbMouth);
                mouthNoEnv=Draw(vs,psr,rtv,rt,staging);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbNose);
                noseNoEnv=Draw(vs,psr,rtv,rt,staging);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbNoseFilter);
                filterNoEnv=Draw(vs,psr,rtv,rt,staging);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,c.envSlot,1,&envOriginal);
                ID3D11Buffer_Release(envCb);
            }
            // Compare the new override to VaM's actual shader with the authored
            // face filter, rather than validating against the replacement itself.
            if(c.filterSlot>=0) {
                D3D11_BUFFER_DESC fd={};ID3D11Buffer_GetDesc(envOriginal,&fd);
                D3D11_BUFFER_DESC filterReadDesc=fd;filterReadDesc.Usage=D3D11_USAGE_STAGING;filterReadDesc.BindFlags=0;filterReadDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ID3D11Buffer *read=NULL;ID3D11Device_CreateBuffer(dev,&filterReadDesc,NULL,&read);
                ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)read,(ID3D11Resource*)envOriginal);
                D3D11_MAPPED_SUBRESOURCE mapped={};
                ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)read,0,D3D11_MAP_READ,0,&mapped);
                std::vector<float> ref((float*)mapped.pData,(float*)mapped.pData+fd.ByteWidth/4);
                ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)read,0);ID3D11Buffer_Release(read);
                ref[c.filterRow*4+c.filterComp]=0.17f;
                ID3D11Buffer *refCb=MakeCb(ref.data(),fd.ByteWidth);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,c.filterSlot,1,&refCb);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbOff);
                filterReference=Draw(vs,pso,rtv,rt,staging);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx,c.filterSlot,1,&envOriginal);
                ID3D11Buffer_Release(refCb);
            }
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbNoseFilter);
            std::vector<float> filterAO=Draw(vs,psr,rtv,rt,staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbNoseFilterOff);
            std::vector<float> filterOff=Draw(vs,psr,rtv,rt,staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbTest);
            std::vector<float> te = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbSkin);
            std::vector<float> sk = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbDetail);
            std::vector<float> d0 = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &tierRow);
            std::vector<float> dr = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 18, 1, &glossStats);
            std::vector<float> dt = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &ownRow[0]);
            std::vector<float> nAt = Draw(vs, psr, rtv, rt, staging);   // t18 bound too: the row's own stats win
            ID3D11DeviceContext_PSSetShaderResources(ctx, 18, 1, &noSrv);
            std::vector<float> nA = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &ownRow[1]);
            std::vector<float> nH = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &ownRow[2]);
            std::vector<float> nZ = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &ownRow[3]);
            std::vector<float> nD = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &ownRow[4]);
            std::vector<float> nP = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 18, 1, &noSrv);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &noSrv);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbScatTest);
            std::vector<float> st = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbScat);
            std::vector<float> sc = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbTransHalf);
            std::vector<float> th=Draw(vs,psr,rtv,rt,staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbTransFull);
            std::vector<float> tf=Draw(vs,psr,rtv,rt,staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbTransMax);
            std::vector<float> tm=Draw(vs,psr,rtv,rt,staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbRegional);
            std::vector<float> regional=Draw(vs,psr,rtv,rt,staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx,12,1,&cbRegionalOff);
            std::vector<float> regionalOff=Draw(vs,psr,rtv,rt,staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbScatNeutral);
            std::vector<float> sn = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbEyeOcc);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 17, 1, &eyeFar);
            std::vector<float> ef = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 17, 1, &eyeCentre);
            std::vector<float> ec = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbEyeCatch);
            std::vector<float> ek = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 17, 1, &noSrv);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, c.subdSlot, 1, &subdCb);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbOff);
            std::vector<float> a6 = Draw(vs, pso, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbTeethSame);
            std::vector<float> ts = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbTeethOcc);
            std::vector<float> to = Draw(vs, psr, rtv, rt, staging);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 12, 1, &cbTeethLook);
            std::vector<float> tl = Draw(vs, psr, rtv, rt, staging);
            dbg += DrainMessages(c.hash.c_str());

            for (int p = 0; p < W * H; p++, px++) {
                bool same = true, ex = true, off = false, ch = false, bad = false, soff = false, sch = false, sbad = false;
                bool nup = false, neq = true;
                if (a[p * 4] == -7.0f) continue;
                drawn++;
                for (int k = 0; k < 3; k++) {
                    float x = a[p * 4 + k], y = b[p * 4 + k], t = te[p * 4 + k], s = sk[p * 4 + k];
                    float u = st[p * 4 + k], w = sc[p * 4 + k], q = sn[p * 4 + k];
                    if(isfinite(w)) {
                        float h=th[p*4+k],f=tf[p*4+k];
                        float tol=1e-4f*std::max(1.0f,std::max(fabsf(w),fabsf(f)));
                        if(!isfinite(h)||!isfinite(f)||fabsf(h-(w+f)*.5f)>tol)transBad++;
                        if(!isfinite(regional[p*4+k])||fabsf(regional[p*4+k]-h)>tol)transBad++;
                        if(!isfinite(regionalOff[p*4+k])||fabsf(regionalOff[p*4+k]-w)>tol)transBad++;
                        if(isfinite(f)&&fabsf(f-w)>tol)transChanged++;
                        if(!isfinite(tm[p*4+k]))transBad++;
                        else if(fabsf(tm[p*4+k]-f)>tol)transMaxChanged++;
                    }
                    if (isfinite(x)) {
                        float nv = nose[p * 4 + k];
                        // Independent light separation: with environment off AO must do nothing;
                        // with environment on it may only change the environment contribution.
                        float direct=noEnv[p*4+k], directAO=noseNoEnv[p*4+k];
                        float want=direct+0.5f*(x-direct);
                        float atol=1e-3f*std::max(1.0f,fabsf(x));
                        if(!isfinite(nv)||fabsf(nv-want)>atol||!isfinite(directAO)||fabsf(directAO-direct)>atol)noseOff++;
                        // Mouth cavity intentionally covers direct AND indirect light.
                        // Nasal AO above retains the older environment-only requirement.
                        float cavityT=(.5f-.18f)/(.62f-.18f),cavity=.06f+.94f*cavityT*cavityT*(3-2*cavityT);
                        if(!isfinite(mouth[p*4+k])||fabsf(mouth[p*4+k]-x*cavity)>atol||fabsf(mouthNoEnv[p*4+k]-direct*cavity)>atol)noseOff++;
                        float fw=c.filterSlot>=0 ? direct+(filterReference[p*4+k]-direct)*0.5f : x;
                        if(!isfinite(filterAO[p*4+k]) || fabsf(filterAO[p*4+k]-fw)>atol ||
                           fabsf(filterOff[p*4+k]-x)>atol || fabsf(filterNoEnv[p*4+k]-direct)>atol)noseOff++;
                        double tol = 1e-4 * std::max(1.0f, fabsf(x));
                        if (!isfinite(q) || q > x + tol) nup = true;
                        if (!isfinite(q) || fabsf(q - x) > tol) neq = false;
                    }
                    if (isnan(x)) nan++;
                    if (!Same(x, y)) same = false;
                    if (memcmp(&x, &y, 4)) ex = false;
                    if (!isnan(x) && !isnan(y)) worst = std::max(worst, (double)fabsf(x - y) / std::max(1.0f, fabsf(x)));
                    if (isfinite(x)) {
                        double d = !isfinite(t) ? 1e30 : (double)fabsf(x - t) / std::max(1.0f, fabsf(x));
                        worstTest = std::max(worstTest, d);
                        if (d > 1e-3) off = true;
                        if (!isfinite(s)) bad = true;
                        double e = !isfinite(u) ? 1e30 : (double)fabsf(x - u) / std::max(1.0f, fabsf(x));
                        worstScatTest = std::max(worstScatTest, e);
                        if (e > 1e-3) soff = true;
                        if (!isfinite(w)) sbad = true;
                    }
                    if (!Same(x, s)) ch = true;
                    if (!Same(x, w)) sch = true;
                }
                bool toff = false, ooff = false, tch = false, tbad = false;
                for (int k = 0; k < 3; k++) {
                    float x = a6[p * 4 + k], y = ts[p * 4 + k], z = to[p * 4 + k], w = tl[p * 4 + k];
                    if (!isfinite(x)) continue;
                    double d = !isfinite(y) ? 1e30 : (double)fabsf(x - y) / std::max(1.0f, fabsf(x));
                    double e = !isfinite(z) ? 1e30 : (double)fabsf(x * kTeethOcc - z) / std::max(1.0f, fabsf(x));
                    worstTeeth = std::max(worstTeeth, d);
                    worstOcc = std::max(worstOcc, e);
                    if (d > 1e-3) toff = true;
                    if (e > 1e-3) ooff = true;
                    if (!isfinite(w)) tbad = true;
                    if (!Same(x, w)) tch = true;
                }
                bool efo = false, eco = false, kb = false, kc = false, kbad = false;
                for (int k = 0; k < 3; k++) {
                    float x = a[p * 4 + k], f = ef[p * 4 + k], g = ec[p * 4 + k], h = ek[p * 4 + k];
                    if (!isfinite(x)) continue;
                    double tol = std::max(1.0f, fabsf(x));
                    if (!Same(x, f)) efo = true;
                    double d = !isfinite(g) ? 1e30 : fabs(x * 0.5 - g) / tol;
                    worstEye = std::max(worstEye, d);
                    if (d > 1e-3) eco = true;
                    if (!isfinite(h)) kbad = true;
                    else if (h < x - 1e-5 * tol) kb = true;
                    if (!Same(x, h)) kc = true;
                }
                bool dch = false, dbad = false;
                for (int k = 0; k < 3; k++) {
                    float x = a[p * 4 + k], s = sk[p * 4 + k], dv = dt[p * 4 + k];
                    if (!isfinite(x)) continue;
                    if (!isfinite(dv) || !isfinite(d0[p * 4 + k]) || !isfinite(dr[p * 4 + k])) dbad = true;
                    if (!Same(s, dv)) dch = true;
                }
                if (dch) changedDetail++;
                if (dbad) badDetail++;
                // 0.18.0's own rows: t18 ignored; the highlight linear in the gain (0 = none) and in 1 / divisor; the
                // row's p50 used
                bool spec = false, p50 = false;
                for (int k = 0; k < 3; k++) {
                    float x = a[p * 4 + k];
                    if (!isfinite(x)) continue;
                    float A = nA[p * 4 + k], At = nAt[p * 4 + k], Hh = nH[p * 4 + k], Z = nZ[p * 4 + k], D = nD[p * 4 + k], P = nP[p * 4 + k];
                    if (!isfinite(A) || !isfinite(At) || !isfinite(Hh) || !isfinite(Z) || !isfinite(D) || !isfinite(P)) { ownBad++; continue; }
                    float tol = 2e-4f * std::max(1.0f, std::max(fabsf(A), fabsf(D)));
                    if (!Same(A, At)) ownBad++;
                    if (fabsf((A - Z) - 2.0f * (Hh - Z)) > tol) ownBad++;
                    if (fabsf((D - Z) - 2.0f * (A - Z)) > tol) ownBad++;
                    if (fabsf(A - Z) > tol) spec = true;
                    if (!Same(P, A)) p50 = true;
                }
                if (spec) ownSpec++;
                if (p50) ownP50++;
                if (efo) eyeFarOff++;
                if (eco) eyeCentreOff++;
                if (kb) catchBelow++;
                if (kc) catchChanged++;
                if (kbad) catchBad++;
                if (toff) teethOff++;
                if (ooff) occOff++;
                if (tch) changedTeeth++;
                if (tbad) badTeeth++;
                if (!same) mismatch++;
                if (ex) exact++;
                if (off) testOff++;
                if (ch) changedSkin++;
                if (bad) badSkin++;
                if (soff) scatTestOff++;
                if (sch) changedScat++;
                if (sbad) badScat++;
                if (nup) neutralAbove++;
                if (neq) neutralSame++;
            }
            for (auto x : cbs) ID3D11Buffer_Release(x);
            if (subdCb) ID3D11Buffer_Release(subdCb);
            for (auto x : srvs)
                if (x) ID3D11ShaderResourceView_Release(x);
        }
        worstAll = std::max(worstAll, worst);
        printf("own rows (0.18.0): failures %d, pixels with a highlight %d, changed by the row's p50 %d\n", ownBad, ownSpec, ownP50);
        bool ownOk = ownBad == 0 && (changedDetail == 0 || (ownSpec > 0 && ownP50 > 0));   // no tier rows without the skin detail
        ownBad = ownSpec = ownP50 = 0;
        bool ok = ownOk && mismatch == 0 && dbg == 0 && drawn == px && testOff == 0 && changedSkin > 0 && badSkin == 0 &&
                  scatTestOff == 0 && changedScat > 0 && badScat == 0 && neutralAbove == 0 && neutralSame > 0 &&
                  c.subdSlot >= 0 && teethOff == 0 && occOff == 0 && changedTeeth > 0 && badTeeth == 0 && eyeFarOff == 0 &&
                  eyeCentreOff == 0 && catchBelow == 0 && catchChanged > 0 && catchBad == 0 && badDetail == 0 && noseOff == 0 && transBad == 0 && transChanged > 0 && transMaxChanged > 0;
        printf("overall translucency: regional half-map / zero-off / interpolation failures %d, changed channels %d, 1-to-2 changed %d\n",transBad,transChanged,transMaxChanged);
        if (noseOff) printf("nasal/mouth environment separation mismatch: %d\n", noseOff);
        if (!ok) failed++;
        printf("%s %s %-6s drawn %d  bit-exact %5.1f%%  mismatches %d  worst rel %.2e  nan %d  | rebuilt: off %d worst %.1e"
               "  | skin GGX: changed %5.1f%%  non-finite %d  | diffuse rebuilt: off %d worst %.1e  wrap: changed %5.1f%%"
               "  non-finite %d  neutral: above VaM %d, same %5.1f%%  | teeth: off %d worst %.1e  occlusion off %d worst %.1e"
               "  look changed %5.1f%% non-finite %d  | eyes: far off %d, centre off %d worst %.1e, catchlight below %d"
               " changed %5.1f%% non-finite %d  | detail changed %5.1f%% non-finite %d  debug %d\n",
               c.hash.c_str(), c.target.c_str(), ok ? "ok" : "FAILED", drawn, 100.0 * exact / std::max(1, drawn), mismatch, worst, nan,
               testOff, worstTest, 100.0 * changedSkin / std::max(1, drawn), badSkin, scatTestOff, worstScatTest,
               100.0 * changedScat / std::max(1, drawn), badScat, neutralAbove, 100.0 * neutralSame / std::max(1, drawn), teethOff,
               worstTeeth, occOff, worstOcc, 100.0 * changedTeeth / std::max(1, drawn), badTeeth, eyeFarOff, eyeCentreOff, worstEye,
               catchBelow, 100.0 * catchChanged / std::max(1, drawn), catchBad, 100.0 * changedDetail / std::max(1, drawn), badDetail, dbg);
        ID3D11VertexShader_Release(vs);
        ID3D11PixelShader_Release(pso);
        ID3D11PixelShader_Release(psr);
    }
    printf("%d cases, %d failed, worst relative difference %.2e: %s\n", cases, failed, worstAll, failed || !cases ? "FAILED" : "PASSED");
    return failed || !cases ? 1 : 0;
}
