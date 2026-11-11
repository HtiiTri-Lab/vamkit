// Offline check of VaMToneMappingNative (out/tone-mapping/VaMToneMappingNative.dll) on a synthetic two-eye FP16 target:
//   1. away from anything bright: style 0, pixels at or below the knee come back unchanged (FP16-exact); style 1, every
//      pixel is the film curve of its input (Film below, the shader's formula on the CPU);
//   2. a flat area above the knee follows the shoulder 1 - (1 - k) exp((k - x) / (1 - k)) (style 0) or the film curve
//      (style 1), plus its own small bloom;
//   3. a bright patch in the left eye next to the seam puts no light into the right eye;
//   4. NaN and infinity in the target stay where they are (finite output) and do not spread through the bloom;
//   5. the render target bound before the event is bound after it.
// usage: tm_test <VaMToneMappingNative.dll> [--style 0|1] [--dump <file>]   (--dump writes the output texels, FP16)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static float H2F(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static uint16_t F2H(float f) {
    if (std::isnan(f)) return 0x7e00;
    if (std::isinf(f)) return f > 0 ? 0x7c00 : 0xfc00;
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000;
    int e = (int)((x >> 23) & 255) - 127 + 15;
    uint32_t m = x & 0x7fffff;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7c00);
    uint32_t h = s | (e << 10) | (m >> 13);
    if ((m >> 12) & 1) h++;  // round half up (test values only)
    return (uint16_t)h;
}

// the film style with the native defaults (tonemap.cpp FilmCurve / Tint, tonemap.hlsl Film)
static void Film(const float *x, float *out) {
    const double a = 1.5, d = 1.0, mi = 0.18, mo = 0.21, hm = 3.0, sat = 1.15, xt = 4.0, split = 2.0;
    double m = std::pow(mi, a), h = std::pow(hm, a);
    double b = (h - m / mo) / (std::pow(h, d) - std::pow(m, d)), c = h - std::pow(h, d) * b;
    double peak = std::fmax(x[0], std::fmax(x[1], x[2]));
    if (!(peak > 1e-6)) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    double z = std::pow(peak, a), y = std::fmin(z / (std::pow(z, d) * b + c), 1.0), w = std::pow(y, xt), col[3];
    for (int i = 0; i < 3; i++) col[i] = y * (std::pow(x[i] / peak, sat) * (1.0 - w) + w);
    double l = 0.2126 * col[0] + 0.7152 * col[1] + 0.0722 * col[2], v = std::sqrt(std::fmin(std::fmax(l, 0.0), 1.0));
    const double cool[3] = {0.92, 1.0, 1.10}, warm[3] = {1.06, 1.0, 0.90};
    double lc = 0.2126 * cool[0] + 0.7152 * cool[1] + 0.0722 * cool[2], lw = 0.2126 * warm[0] + 0.7152 * warm[1] + 0.0722 * warm[2];
    double ws = split * (1.0 - v) * (1.0 - v), wh = split * v * v;
    for (int i = 0; i < 3; i++) {
        double t = (1.0 + (cool[i] / lc - 1.0) * ws) * (1.0 + (warm[i] / lw - 1.0) * wh);
        out[i] = (float)std::fmin(std::fmax(col[i] * t, 0.0), 1.0);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tm_test <VaMToneMappingNative.dll> [--style 0|1] [--dump <file>]\n");
        return 2;
    }
    int style = 0;
    const char *dump = nullptr;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--style")) style = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--dump")) dump = argv[i + 1];
    }
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        fprintf(stderr, "no device\n");
        return 2;
    }
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    auto init = (int(__cdecl *)(void *))GetProcAddress(m, "TM_Init");
    auto func = (void *(__cdecl *)())GetProcAddress(m, "TM_RenderEventFunc");
    auto read = (int(__cdecl *)(char *, int))GetProcAddress(m, "TM_Read");
    auto setStyle = (void(__cdecl *)(int))GetProcAddress(m, "TM_SetStyle");
    if (!init || !func || !read) return 2;
    if (style != 0 && !setStyle) {
        fprintf(stderr, "this DLL has no TM_SetStyle\n");
        return 2;
    }
    if (setStyle) setStyle(style);

    const UINT EW = 640, W = EW * 2, H = 600;
    std::vector<uint16_t> px((size_t)W * H * 4);
    auto set = [&](UINT x, UINT y, float r, float g, float b) {
        uint16_t *p = &px[((size_t)y * W + x) * 4];
        p[0] = F2H(r), p[1] = F2H(g), p[2] = F2H(b), p[3] = F2H(1.0f);
    };
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++) set(x, y, 0.05f + 0.5f * x / W, 0.3f, 0.6f * y / H);  // all at or below the knee
    for (UINT y = 100; y < 160; y++)  // left eye, against the seam: bright
        for (UINT x = EW - 40; x < EW; x++) set(x, y, 8.0f, 6.0f, 4.0f);
    for (UINT y = 400; y < 560; y++)  // right eye: a flat area above the knee, below the bloom threshold
        for (UINT x = EW + 300; x < EW + 460; x++) set(x, y, 0.9f, 0.75f, 0.62f);
    set(200, 300, NAN, NAN, NAN);  // left eye: NaN and infinity
    set(210, 300, INFINITY, INFINITY, INFINITY);

    D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {px.data(), W * 8, 0};
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) return 2;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(tex, &rd, &rtv))) return 2;
    if (init(tex) != 0) {
        fprintf(stderr, "TM_Init failed\n");
        return 2;
    }
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ((void(__stdcall *)(int))func())((1 << 24) | (2 << 16));
    ID3D11RenderTargetView *after = nullptr;
    ctx->OMGetRenderTargets(1, &after, nullptr);
    bool restored = after == rtv;
    if (after) after->Release();

    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    dev->CreateTexture2D(&td, nullptr, &st);
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE mp;
    ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp);
    auto get = [&](UINT x, UINT y, int c) { return H2F(((const uint16_t *)((const uint8_t *)mp.pData + (size_t)y * mp.RowPitch))[x * 4 + c]); };
    auto in = [&](UINT x, UINT y, int c) { return H2F(px[((size_t)y * W + x) * 4 + c]); };
    // what the pixel would be with no bloom: style 0 the input (at or below the knee), style 1 its film curve
    auto want = [&](UINT x, UINT y, int c) {
        if (style == 0) return in(x, y, c);
        float v[3] = {in(x, y, 0), in(x, y, 1), in(x, y, 2)}, o[3];
        Film(v, o);
        return o[c];
    };
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            for (UINT y = 0; y < H; y++) fwrite((const uint8_t *)mp.pData + (size_t)y * mp.RowPitch, 8, W, f);
            fclose(f);
        }
    }

    char status[1024];
    read(status, sizeof status);
    printf("native: %s\nstyle %d\n", status, style);

    // 1. far from the bright patch, the flat area and the NaN
    size_t same = 0, total = 0;
    float worst = 0;
    for (UINT y = 0; y < H; y += 3)
        for (UINT x = 0; x < W; x += 3) {
            // the bloom's widest level reaches a few hundred pixels: the bright patch's eye down to y 520, the flat area +-260
            bool nearBright = x < EW && y < 520;
            bool nearFlat = x + 260 > EW + 300 && x < EW + 720 && y + 260 > 400;
            bool nearNan = x > 150 && x < 260 && y > 250 && y < 350;
            if (nearBright || nearFlat || nearNan) continue;
            for (int c = 0; c < 3; c++) {
                total++;
                float d = std::fabs(get(x, y, c) - want(x, y, c));
                if (style == 0 ? d == 0 : d < 2e-3f) same++;
                if (d > worst) worst = d;
            }
        }
    printf("1. away from light: %zu of %zu channels as expected (%s), largest difference %.6f\n", same, total,
           style == 0 ? "unchanged" : "film curve within 0.002", worst);
    bool ok1 = same == total;

    // 2. the flat area's centre against the shoulder or the film curve
    const float k = 0.6f;
    float curveErr = 0;
    float flat[3] = {in(EW + 380, 480, 0), in(EW + 380, 480, 1), in(EW + 380, 480, 2)}, flatFilm[3];
    Film(flat, flatFilm);
    for (int c = 0; c < 3; c++) {
        float x = flat[c], w = style == 1 ? flatFilm[c] : x > k ? 1.0f - (1.0f - k) * std::exp((k - x) / (1.0f - k)) : x;
        float got = get(EW + 380, 480, c);
        printf("2. channel %d: in %.4f -> %.4f (%s %.4f)\n", c, x, got, style == 1 ? "film" : "shoulder", w);
        curveErr = std::fmax(curveErr, std::fabs(got - w));
    }
    bool ok2 = curveErr < 3e-3f;

    // 3. the right eye's first columns next to the patch: no light
    float leak = 0;
    for (UINT y = 60; y < 200; y++)
        for (UINT x = EW; x < EW + 60; x++)
            for (int c = 0; c < 3; c++) leak = std::fmax(leak, std::fabs(get(x, y, c) - want(x, y, c)));
    float glow = 0;
    for (int c = 0; c < 3; c++) glow = std::fmax(glow, get(EW - 60, 130, c) - want(EW - 60, 130, c));
    printf("3. right eye beside the patch: largest difference %.6f; left eye 20 px from it: bloom +%.4f\n", leak, glow);
    bool ok3 = (style == 0 ? leak == 0 : leak < 2e-3f) && glow > 0;

    // 4. NaN / infinity
    bool finite = true;
    for (int c = 0; c < 3; c++) finite = finite && std::isfinite(get(200, 300, c)) && std::isfinite(get(210, 300, c));
    float spread = 0;
    for (UINT y = 260; y < 340; y++)
        for (UINT x = 160; x < 250; x++)
            if (!(x == 200 && y == 300) && !(x == 210 && y == 300))
                for (int c = 0; c < 3; c++) spread = std::fmax(spread, std::fabs(get(x, y, c) - want(x, y, c)));
    printf("4. NaN -> %.3f %.3f %.3f, inf -> %.3f %.3f %.3f; largest change around them %.4f\n", get(200, 300, 0), get(200, 300, 1),
           get(200, 300, 2), get(210, 300, 0), get(210, 300, 1), get(210, 300, 2), spread);
    bool ok4 = finite;

    printf("5. render target restored: %s\n", restored ? "yes" : "no");
    bool ok = ok1 && ok2 && ok3 && ok4 && restored;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
