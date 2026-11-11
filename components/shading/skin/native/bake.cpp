// Thickness bake and teeth attributes (see bake.h).
#include "bake.h"

#include <math.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <queue>
#include <functional>

namespace {

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Normalize(V3 a) {
    float l = sqrtf(Dot(a, a));
    return l > 0 ? a * (1.0f / l) : V3{0, 0, 1};
}
inline float Get(const V3 &v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---- BVH over the occluding triangles ---------------------------------------------------------------------------

struct Tri {
    V3 a, e1, e2, n;  // n: outward geometric normal (unnormalised)
};

struct Node {
    V3 lo, hi;
    int left, count;  // count > 0: leaf with triangles [left, left + count); else children left, left + 1
};

struct Bvh {
    std::vector<Tri> tris;
    std::vector<Node> nodes;

    void Build(std::vector<Tri> &&t) {
        tris = std::move(t);
        std::vector<int> order(tris.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        std::vector<V3> centre(tris.size());
        for (size_t i = 0; i < tris.size(); i++) {
            const Tri &r = tris[i];
            centre[i] = r.a + (r.e1 + r.e2) * (1.0f / 3.0f);
        }
        nodes.clear();
        nodes.reserve(tris.size() * 2);
        nodes.push_back(Node());
        BuildNode(0, order, centre, 0, (int)order.size());
        std::vector<Tri> sorted(tris.size());
        for (size_t i = 0; i < order.size(); i++) sorted[i] = tris[order[i]];
        tris.swap(sorted);
    }

    void BuildNode(int ni, std::vector<int> &order, const std::vector<V3> &centre, int begin, int end) {
        V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, clo = lo, chi = hi;
        for (int i = begin; i < end; i++) {
            const Tri &t = tris[order[i]];
            V3 p[3] = {t.a, t.a + t.e1, t.a + t.e2};
            for (V3 q : p) {
                lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
            }
            V3 c = centre[order[i]];
            clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
            chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
        }
        nodes[ni].lo = lo;
        nodes[ni].hi = hi;
        int n = end - begin;
        V3 ext = chi - clo;
        int axis = ext.x > ext.y && ext.x > ext.z ? 0 : ext.y > ext.z ? 1 : 2;
        if (n <= 4 || Get(ext, axis) <= 0) {
            nodes[ni].left = begin;
            nodes[ni].count = n;
            return;
        }
        int mid = (begin + end) / 2;
        std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                         [&](int a, int b) { return Get(centre[a], axis) < Get(centre[b], axis); });
        int l = (int)nodes.size();
        nodes.push_back(Node());
        nodes.push_back(Node());
        nodes[ni].left = l;
        nodes[ni].count = 0;
        BuildNode(l, order, centre, begin, mid);
        BuildNode(l + 1, order, centre, mid, end);
    }

    static bool HitBox(const Node &nd, V3 o, V3 inv, float tmax) {
        float t0 = 0, t1 = tmax;
        for (int a = 0; a < 3; a++) {
            float lo = (Get(nd.lo, a) - Get(o, a)) * Get(inv, a), hi = (Get(nd.hi, a) - Get(o, a)) * Get(inv, a);
            if (lo > hi) std::swap(lo, hi);
            t0 = std::max(t0, lo);
            t1 = std::min(t1, hi);
            if (t0 > t1) return false;
        }
        return true;
    }

    // Nearest hit beyond tmin: distance and the triangle, or -1.
    int Nearest(V3 o, V3 d, float tmin, float tmax, float &tHit) const {
        V3 inv{1.0f / (d.x != 0 ? d.x : 1e-20f), 1.0f / (d.y != 0 ? d.y : 1e-20f), 1.0f / (d.z != 0 ? d.z : 1e-20f)};
        int stack[64], sp = 0, best = -1;
        float bestT = tmax;
        stack[sp++] = 0;
        while (sp) {
            const Node &nd = nodes[stack[--sp]];
            if (!HitBox(nd, o, inv, bestT)) continue;
            if (nd.count) {
                for (int i = nd.left; i < nd.left + nd.count; i++) {
                    const Tri &t = tris[i];
                    V3 p = Cross(d, t.e2);
                    float det = Dot(t.e1, p);
                    if (fabsf(det) < 1e-12f) continue;
                    float id = 1.0f / det;
                    V3 s = o - t.a;
                    float u = Dot(s, p) * id;
                    if (u < 0 || u > 1) continue;
                    V3 q = Cross(s, t.e1);
                    float v = Dot(d, q) * id;
                    if (v < 0 || u + v > 1) continue;
                    float tt = Dot(t.e2, q) * id;
                    if (tt > tmin && tt < bestT) {
                        bestT = tt;
                        best = i;
                    }
                }
            } else if (sp < 62) {
                stack[sp++] = nd.left;
                stack[sp++] = nd.left + 1;
            }
        }
        tHit = bestT;
        return best;
    }
};

// Directions in a cone around +z, cosine-weighted (Fibonacci spiral, so the same for every bake).
std::vector<V3> ConeDirections(int count, float coneDegrees) {
    std::vector<V3> dirs;
    float sinMax2 = sinf(coneDegrees * 3.14159265f / 180.0f);
    sinMax2 *= sinMax2;
    const float golden = 2.39996323f;
    for (int i = 0; i < count; i++) {
        float r2 = (i + 0.5f) / count * sinMax2;  // sin^2(theta), uniform -> cosine-weighted
        float s = sqrtf(r2), c = sqrtf(1 - r2), phi = i * golden;
        dirs.push_back({s * cosf(phi), s * sinf(phi), c});
    }
    return dirs;
}

// Sideways spread (bake.h): the light that gets through, exp(-d / 3.67 mm), averaged with a Gaussian over the
// receiving vertices nearby that face the same way. UV seams split vertices; the copies at one position count once.
void SmoothThickness(const BakeInput &in, const std::vector<uint8_t> &need, std::vector<float> &d) {
    const float lambda = 3.67f, sigma = in.smoothMM, reach = 3.0f * sigma;
    int nv = in.vertexCount;
    // one point per position
    std::vector<int> rep(nv, -1);
    std::vector<V3> pos, nrm;
    std::vector<float> T;
    std::vector<int> copies;
    {
        std::unordered_map<uint64_t, int> at;
        for (int i = 0; i < nv; i++) {
            if (!need[i]) continue;
            V3 p{in.pos[3 * i] * 1000.0f, in.pos[3 * i + 1] * 1000.0f, in.pos[3 * i + 2] * 1000.0f};
            uint64_t key = 0;
            for (float c : {p.x, p.y, p.z}) key = key * 0x100000001B3ull ^ (uint64_t)(int64_t)llroundf(c * 1000.0f);
            auto it = at.find(key);
            float t = d[i] >= kNoThickness ? 0.0f : expf(-d[i] / lambda);
            V3 n = Normalize(V3{in.nrm[3 * i], in.nrm[3 * i + 1], in.nrm[3 * i + 2]});
            if (it == at.end()) {
                at[key] = rep[i] = (int)pos.size();
                pos.push_back(p);
                nrm.push_back(n);
                T.push_back(t);
                copies.push_back(1);
            } else {
                int k = rep[i] = it->second;
                nrm[k] = nrm[k] + n;
                T[k] += t;
                copies[k]++;
            }
        }
    }
    int np = (int)pos.size();
    for (int k = 0; k < np; k++) {
        nrm[k] = Normalize(nrm[k]);
        T[k] /= copies[k];
    }
    // grid of cells `reach` wide
    std::unordered_map<uint64_t, std::vector<int>> grid;
    auto cell = [&](V3 p, int dx, int dy, int dz) {
        int64_t x = (int64_t)floorf(p.x / reach) + dx, y = (int64_t)floorf(p.y / reach) + dy, z = (int64_t)floorf(p.z / reach) + dz;
        return ((uint64_t)(x & 0x1fffff) << 42) | ((uint64_t)(y & 0x1fffff) << 21) | (uint64_t)(z & 0x1fffff);
    };
    for (int k = 0; k < np; k++) grid[cell(pos[k], 0, 0, 0)].push_back(k);
    std::vector<float> Ts(np);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma), reach2 = reach * reach;
    for (int k = 0; k < np; k++) {
        double sw = 0, st = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    auto g = grid.find(cell(pos[k], dx, dy, dz));
                    if (g == grid.end()) continue;
                    for (int j : g->second) {
                        V3 e = pos[j] - pos[k];
                        float r2 = Dot(e, e);
                        if (r2 > reach2) continue;
                        float facing = std::min(1.0f, std::max(0.0f, (Dot(nrm[j], nrm[k]) - 0.2f) / 0.6f));
                        float w = expf(-r2 * inv2s2) * facing;
                        sw += w;
                        st += w * T[j];
                    }
                }
        Ts[k] = sw > 0 ? (float)(st / sw) : T[k];
    }
    for (int i = 0; i < nv; i++) {
        if (rep[i] < 0) continue;
        float t = Ts[rep[i]];
        d[i] = t > 1e-6f ? std::min(kNoThickness, -lambda * logf(t)) : kNoThickness;
    }
}

// Cosine-weighted ambient visibility of the actual surface. A finite 8 mm horizon
// gives contact occlusion in the cavity; distant geometry fades continuously out.
float Smooth01(float x) {
    x = std::max(0.0f, std::min(1.0f, x));
    return x*x*(3.0f-2.0f*x);
}
float AmbientVisibility(V3 p, V3 n, const Bvh &bvh) {
    static const std::vector<V3> directions = ConeDirections(128,90.0f);
    n=Normalize(n);
    V3 up=fabsf(n.z)<0.9f?V3{0,0,1}:V3{1,0,0};
    V3 tx=Normalize(Cross(up,n)),ty=Cross(n,tx),origin=p+n*0.08f;
    double sum=0;
    for(V3 c:directions) {
        float t;
        V3 d=tx*c.x+ty*c.y+n*c.z;
        int hit=bvh.Nearest(origin,d,0.15f,8.0f,t);
        sum+=hit<0?1.0f:Smooth01(t/8.0f);
    }
    return (float)(sum/directions.size());
}

// ---- UV-space textures -------------------------------------------------------------------------------------------

// One submesh's triangles drawn in UV space (texel centres), its per-vertex values (channels per vertex) interpolated;
// where triangles overlap, the smaller first channel wins (thickness: the thin part). A texel is empty while its first
// channel is negative (values extrapolated just past a triangle's edge can be). The islands are then grown by the
// border (empty texels take the mean of their filled neighbours); texels still empty get `empty`.
BakeTexture RasterSub(const BakeInput &in, int s, float density, const float *values, int channels, float empty,
                      const Bvh *ambient = nullptr, const std::vector<float> *noseDistance = nullptr) {
    const BakeSub &sb = in.subs[s];
    float u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
    for (int k = sb.indexStart; k < sb.indexStart + sb.indexCount; k++) {
        int i = in.indices[k];
        u0 = std::min(u0, in.uv[2 * i]);
        u1 = std::max(u1, in.uv[2 * i]);
        v0 = std::min(v0, in.uv[2 * i + 1]);
        v1 = std::max(v1, in.uv[2 * i + 1]);
    }
    float span = std::max(u1 - u0, v1 - v0);
    if (span * density + 8 > in.maxSize) density = (in.maxSize - 8) / std::max(span, 1e-6f);
    const int border = 3;
    float du = 1.0f / density;
    BakeTexture tex;
    tex.sub = s;
    tex.channels = channels;
    tex.width = std::max(4, (int)ceilf((u1 - u0) * density) + 2 * border);
    tex.height = std::max(4, (int)ceilf((v1 - v0) * density) + 2 * border);
    tex.uvMin[0] = u0 - border * du;
    tex.uvMin[1] = v0 - border * du;
    tex.uvMax[0] = tex.uvMin[0] + tex.width * du;
    tex.uvMax[1] = tex.uvMin[1] + tex.height * du;
    int W = tex.width, H = tex.height, C = channels;
    std::vector<float> img((size_t)W * H * C, -1.0f);
    for (int k = sb.indexStart; k + 2 < sb.indexStart + sb.indexCount; k += 3) {
        int id[3] = {in.indices[k], in.indices[k + 1], in.indices[k + 2]};
        float x[3], y[3];
        for (int j = 0; j < 3; j++) {
            x[j] = (in.uv[2 * id[j]] - tex.uvMin[0]) * density - 0.5f;  // texel-centre coordinates
            y[j] = (in.uv[2 * id[j] + 1] - tex.uvMin[1]) * density - 0.5f;
        }
        float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (fabsf(area) < 1e-12f) continue;
        int xa = std::max(0, (int)floorf(std::min({x[0], x[1], x[2]})));
        int xb = std::min(W - 1, (int)ceilf(std::max({x[0], x[1], x[2]})));
        int ya = std::max(0, (int)floorf(std::min({y[0], y[1], y[2]})));
        int yb = std::min(H - 1, (int)ceilf(std::max({y[0], y[1], y[2]})));
        const float eps = -0.02f;
        for (int py = ya; py <= yb; py++)
            for (int px = xa; px <= xb; px++) {
                float w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
                float w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
                float w2 = 1 - w0 - w1;
                if (w0 < eps || w1 < eps || w2 < eps) continue;
                size_t t = (size_t)py * W + px;
                float v = w0 * values[C * id[0]] + w1 * values[C * id[1]] + w2 * values[C * id[2]];
                if (!(img[t * C] < 0 || v < img[t * C])) continue;
                img[t * C] = v;
                for (int c = 1; c < C; c++) img[t * C + c] = w0 * values[C * id[0] + c] + w1 * values[C * id[1] + c] + w2 * values[C * id[2] + c];
                if (ambient && noseDistance && C == 2) {
                    const float weights[3]={w0,w1,w2};
                    float distance=0;
                    for(int j=0;j<3;j++)distance+=weights[j]*(*noseDistance)[id[j]];
                    img[t*C+1]=1.0f;
                    if(distance<8.0f) {
                        V3 p{0,0,0},n{0,0,0};
                        for(int j=0;j<3;j++) {
                            int i=id[j];
                            p=p+V3{in.pos[3*i],in.pos[3*i+1],in.pos[3*i+2]}*(weights[j]*1000.0f);
                            n=n+V3{in.nrm[3*i],in.nrm[3*i+1],in.nrm[3*i+2]}*weights[j];
                        }
                        float local=1.0f-Smooth01((distance-3.0f)/5.0f);
                        img[t*C+1]=1.0f-(1.0f-AmbientVisibility(p,n,*ambient))*local;
                    }
                }
            }
    }
    // grow the islands by the border: empty texels take the mean of their filled neighbours
    for (int pass = 0; pass < border; pass++) {
        std::vector<float> src = img;
        for (int py = 0; py < H; py++)
            for (int px = 0; px < W; px++) {
                if (src[((size_t)py * W + px) * C] >= 0) continue;
                float sum[4] = {0, 0, 0, 0};
                int cnt = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int qx = px + dx, qy = py + dy;
                        if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
                        size_t q = (size_t)qy * W + qx;
                        if (!(src[q * C] >= 0)) continue;
                        for (int c = 0; c < C; c++) sum[c] += src[q * C + c];
                        cnt++;
                    }
                if (!cnt) continue;
                size_t t = (size_t)py * W + px;
                for (int c = 0; c < C; c++) img[t * C + c] = sum[c] / cnt;
            }
    }
    tex.texels.resize((size_t)W * H * C);
    for (size_t t = 0; t < (size_t)W * H; t++)
        for (int c = 0; c < C; c++) tex.texels[t * C + c] = FloatToHalf(img[t * C] < 0 ? (ambient && c == 1 ? 1.0f : empty) : img[t * C + c]);
    return tex;
}

// Nostrils locates the cavity. Face shares its inner wall; both use continuous surface visibility.
// Graph distance bounds the calculation; it does not prescribe an aperture or a dark silhouette.
void NoseVisibility(const BakeInput &in, const Bvh &bvh, std::vector<float> &visibility, std::vector<float> &noseDistance) {
    visibility.assign(in.vertexCount, 1.0f);
    noseDistance.assign(in.vertexCount, 1e3f);
    if (bvh.tris.empty()) return;
    std::vector<int> vertex(in.vertexCount, -1), copies;
    std::vector<V3> pos, nrm;
    std::vector<uint8_t> seed;
    std::unordered_map<uint64_t, int> weld;
    std::unordered_map<uint64_t, int> edges, noseEdges;
    auto addVertex = [&](int i) {
        if (vertex[i] >= 0) return vertex[i];
        V3 p{in.pos[3*i]*1000, in.pos[3*i+1]*1000, in.pos[3*i+2]*1000};
        uint64_t key = 0;
        for (float c : {p.x, p.y, p.z}) key = key * 0x100000001B3ull ^ (uint64_t)(int64_t)llroundf(c * 1000);
        auto it = weld.find(key);
        int j;
        if (it == weld.end()) {
            j = (int)pos.size(); weld[key] = j;
            pos.push_back(p); nrm.push_back({0,0,0}); seed.push_back(0);
        } else j = it->second;
        nrm[j] = nrm[j] + V3{in.nrm[3*i], in.nrm[3*i+1], in.nrm[3*i+2]};
        vertex[i] = j; copies.push_back(i);
        return j;
    };
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sub = in.subs[s];
        if (!(sub.flags & (BAKE_NOSTRILS | BAKE_FACE)) || !(sub.flags & BAKE_RECEIVER)) continue;
        for (int k = sub.indexStart; k + 2 < sub.indexStart + sub.indexCount; k += 3) {
            int v[3] = {addVertex(in.indices[k]), addVertex(in.indices[k+1]), addVertex(in.indices[k+2])};
            for (int e = 0; e < 3; e++) {
                if (sub.flags & BAKE_NOSTRILS) seed[v[e]] = 1;
                int a = std::min(v[e], v[(e+1)%3]), b = std::max(v[e], v[(e+1)%3]);
                if (a == b) continue;
                uint64_t key = ((uint64_t)a << 32) | (uint32_t)b;
                edges[key]++;
                if (sub.flags & BAKE_NOSTRILS) noseEdges[key]++;
            }
        }
    }
    bool open = false;
    for (auto edge : noseEdges) open |= edge.second == 1;
    if (!open) return;
    std::vector<std::vector<std::pair<int,float>>> graph(pos.size());
    for (auto &kv : edges) {
        int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
        V3 d = pos[a] - pos[b]; float len = sqrtf(Dot(d,d));
        graph[a].push_back({b,len}); graph[b].push_back({a,len});
    }
    typedef std::pair<float,int> Item;
    typedef std::priority_queue<Item, std::vector<Item>, std::greater<Item>> Queue;
    Queue queue;
    std::vector<float> distance(pos.size(), 1e30f);
    for (size_t j = 0; j < pos.size(); j++) if (seed[j]) distance[j] = 0, queue.push({0.0f,(int)j});
    while (!queue.empty()) {
        Item item = queue.top(); queue.pop();
        if (item.first > distance[item.second]) continue;
        for (auto edge : graph[item.second]) {
            float d = item.first + edge.second;
            if (d < distance[edge.first] && d < 12.0f) distance[edge.first] = d, queue.push({d,edge.first});
        }
    }
    for(int i:copies) {
        int j=vertex[i];
        noseDistance[i]=std::min(distance[j],1e3f);
        if(distance[j]<8.0f) {
            float local=1.0f-Smooth01((distance[j]-3.0f)/5.0f);
            visibility[i]=1.0f-(1.0f-AmbientVisibility(pos[j],nrm[j],bvh))*local;
        }
    }
}

// ---- teeth and tongue (0.8.0) ------------------------------------------------------------------------------------

// Per vertex of the teeth and tongue submeshes: back (0 at the front of the front teeth, 1 at the back of the last
// molars, along the forward axis) and, on teeth, gum (0 at the tooth's biting edge, 1 at its gum end). See bake.h.
void TeethAttributes(const BakeInput &in, std::vector<float> &back, std::vector<float> &gum, TeethReport &rep) {
    int nv = in.vertexCount;
    back.assign(nv, 0.0f);
    gum.assign(nv, 0.0f);
    auto P = [&](int i) { return V3{in.pos[3 * i], in.pos[3 * i + 1], in.pos[3 * i + 2]}; };
    std::vector<uint8_t> kind(nv, 0), lip(nv, 0);  // kind 1 teeth, 2 tongue
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sb = in.subs[s];
        for (int k = sb.indexStart; k < sb.indexStart + sb.indexCount; k++) {
            int i = in.indices[k];
            if (sb.flags & BAKE_TEETH) kind[i] = 1;
            else if ((sb.flags & (BAKE_TONGUE|BAKE_MOUTH)) && !kind[i]) kind[i] = 2;
            if (sb.flags & BAKE_LIPS) lip[i] = 1;
        }
    }
    V3 tc{0, 0, 0}, lc{0, 0, 0};
    int nt = 0, nl = 0;
    for (int i = 0; i < nv; i++) {
        if (kind[i] == 1) tc = tc + P(i), nt++;
        if (lip[i]) lc = lc + P(i), nl++;
    }
    if (!nt) return;
    tc = tc * (1.0f / nt);
    V3 f{0, 0, 1};
    if (nl) {
        V3 d = lc * (1.0f / nl) - tc;
        d.y = 0;
        if (Dot(d, d) > 1e-8f) f = Normalize(d);
    }
    rep.forward[0] = f.x, rep.forward[1] = f.y, rep.forward[2] = f.z;
    // the teeth: parts connected by triangles or by vertices at the same place (UV seams split vertices)
    std::vector<int> parent(nv);
    for (int i = 0; i < nv; i++) parent[i] = i;
    auto find = [&](int a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    };
    auto unite = [&](int a, int b) {
        a = find(a), b = find(b);
        if (a != b) parent[a] = b;
    };
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sb = in.subs[s];
        if (!(sb.flags & BAKE_TEETH)) continue;
        for (int k = sb.indexStart; k + 2 < sb.indexStart + sb.indexCount; k += 3) {
            unite(in.indices[k], in.indices[k + 1]);
            unite(in.indices[k], in.indices[k + 2]);
        }
    }
    std::unordered_map<uint64_t, int> at;  // position (0.01 mm grid) -> a vertex there
    for (int i = 0; i < nv; i++) {
        if (kind[i] != 1) continue;
        V3 q = P(i);
        uint64_t key = ((uint64_t)(int64_t)lroundf(q.x * 1e5f) & 0x1fffff) | (((uint64_t)(int64_t)lroundf(q.y * 1e5f) & 0x1fffff) << 21) |
                       (((uint64_t)(int64_t)lroundf(q.z * 1e5f) & 0x1fffff) << 42);
        auto it = at.find(key);
        if (it == at.end()) at[key] = i;
        else unite(i, it->second);
    }
    struct Part {
        double f = 0, y = 0;
        float ylo = 1e30f, yhi = -1e30f;
        int n = 0;
        bool upper = false;
    };
    std::unordered_map<int, Part> parts;
    float fFront = -1e30f, fBack = 1e30f;
    for (int i = 0; i < nv; i++) {
        if (kind[i] != 1) continue;
        V3 q = P(i);
        float fi = Dot(q, f);
        Part &pt = parts[find(i)];
        pt.f += fi, pt.y += q.y, pt.n++;
        pt.ylo = std::min(pt.ylo, q.y), pt.yhi = std::max(pt.yhi, q.y);
        fFront = std::max(fFront, fi), fBack = std::min(fBack, fi);
    }
    // upper or lower: above or below the line through the parts' centres along the arch (y = a + b f, least squares;
    // the biting plane rises toward the back)
    double sf = 0, sy = 0, sff = 0, sfy = 0;
    for (auto &kv : parts) {
        Part &pt = kv.second;
        pt.f /= pt.n, pt.y /= pt.n;
        sf += pt.f, sy += pt.y, sff += pt.f * pt.f, sfy += pt.f * pt.y;
    }
    double m = (double)parts.size(), det = m * sff - sf * sf;
    double b = parts.size() > 1 && fabs(det) > 1e-12 ? (m * sfy - sf * sy) / det : 0.0, a = (sy - b * sf) / std::max(m, 1.0);
    for (auto &kv : parts) {
        Part &pt = kv.second;
        pt.upper = pt.y > a + b * pt.f;
        (pt.upper ? rep.upper : rep.lower)++;
    }
    rep.teeth = (int)parts.size();
    float depth = std::max(fFront - fBack, 1e-3f);
    rep.frontMM = fFront * 1000.0f;
    rep.depthMM = depth * 1000.0f;
    for (int i = 0; i < nv; i++) {
        if (!kind[i]) continue;
        V3 q = P(i);
        back[i] = std::min(1.0f, std::max(0.0f, (fFront - Dot(q, f)) / depth));
        if (kind[i] != 1) continue;
        const Part &pt = parts[find(i)];
        float h = std::max(pt.yhi - pt.ylo, 5e-4f);
        gum[i] = std::min(1.0f, std::max(0.0f, pt.upper ? (q.y - pt.ylo) / h : (pt.yhi - q.y) / h));
    }
}

// a 4 x 4 linear system (Gaussian elimination, partial pivoting); false when singular
bool Solve4(double m[4][5], double x[4]) {
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
        if (fabs(m[p][c]) < 1e-30) return false;
        for (int k = 0; k < 5; k++) std::swap(m[c][k], m[p][k]);
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = m[r][c] / m[c][c];
            for (int k = c; k < 5; k++) m[r][k] -= f * m[c][k];
        }
    }
    for (int c = 0; c < 4; c++) x[c] = m[c][4] / m[c][c];
    return true;
}

// polylines through a graph of nodes with at most two neighbours each: open chains first (from their ends), then loops
std::vector<std::vector<uint64_t>> Chains(const std::unordered_map<uint64_t, std::vector<uint64_t>> &adj) {
    std::vector<std::vector<uint64_t>> out;
    std::unordered_map<uint64_t, bool> used;
    auto walk = [&](uint64_t s) {
        std::vector<uint64_t> c{s};
        used[s] = true;
        uint64_t prev = s, cur = s;
        bool first = true;
        for (;;) {
            auto it = adj.find(cur);
            uint64_t next = 0;
            bool found = false;
            for (uint64_t w : it->second)
                if (!used[w] && (first || w != prev)) {
                    next = w;
                    found = true;
                    break;
                }
            if (!found) break;
            used[next] = true;
            c.push_back(next);
            prev = cur, cur = next, first = false;
        }
        out.push_back(c);
    };
    for (auto &kv : adj)
        if (kv.second.size() == 1 && !used[kv.first]) walk(kv.first);
    for (auto &kv : adj)
        if (!used[kv.first]) walk(kv.first);
    return out;
}

}  // namespace

// ---- eyes (0.9.0) ------------------------------------------------------------------------------------------------

void EyeRigOf(const BakeInput &in, EyeRig &rig) {
    rig = EyeRig();
    int nv = in.vertexCount;
    auto P = [&](int i) { return V3{in.pos[3 * i], in.pos[3 * i + 1], in.pos[3 * i + 2]}; };
    // vertices at the same place (UV seams split them) are one: the first vertex there stands for all
    std::vector<int> weld(nv);
    {
        std::unordered_map<uint64_t, int> at;
        at.reserve(nv);
        for (int i = 0; i < nv; i++) {
            V3 q = P(i);
            uint64_t key = ((uint64_t)(int64_t)lroundf(q.x * 1e5f) & 0x1fffff) | (((uint64_t)(int64_t)lroundf(q.y * 1e5f) & 0x1fffff) << 21) |
                           (((uint64_t)(int64_t)lroundf(q.z * 1e5f) & 0x1fffff) << 42);
            auto it = at.find(key);
            weld[i] = it == at.end() ? (at[key] = i) : it->second;
        }
    }
    std::vector<uint8_t> kind(nv, 0);  // 1 sclera, 2 iris
    double mx = 0;
    int ns = 0;
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sb = in.subs[s];
        for (int k = sb.indexStart; k < sb.indexStart + sb.indexCount; k++) {
            int i = in.indices[k];
            if ((sb.flags & BAKE_SCLERA) && kind[i] != 1) kind[i] = 1, mx += P(i).x, ns++;
            else if ((sb.flags & BAKE_IRIS) && !kind[i]) kind[i] = 2;
        }
    }
    if (ns < 32) {
        rig.error = "no sclera";
        return;
    }
    mx /= ns;
    auto sideOf = [&](int i) { return P(i).x >= mx ? 0 : 1; };  // eye 0 on the +x side
    V3 C[2], G[2];
    float R[2];
    for (int e = 0; e < 2; e++) {
        // the eyeball: least-squares sphere through the sclera, |p|^2 = 2 c.p + (r^2 - |c|^2)
        double m[4][5] = {};
        int n = 0;
        for (int i = 0; i < nv; i++) {
            if (kind[i] != 1 || weld[i] != i || sideOf(i) != e) continue;
            V3 q = P(i);
            double row[5] = {2.0 * q.x, 2.0 * q.y, 2.0 * q.z, 1.0, (double)Dot(q, q)};
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 5; b++) m[a][b] += row[a] * row[b];
            n++;
        }
        double x[4];
        if (n < 16 || !Solve4(m, x)) {
            rig.error = "cannot fit the eyeball";
            return;
        }
        C[e] = V3{(float)x[0], (float)x[1], (float)x[2]};
        double r2 = x[3] + x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
        if (!(r2 > 0)) {
            rig.error = "cannot fit the eyeball";
            return;
        }
        R[e] = (float)sqrt(r2);
    }
    for (int e = 0; e < 2; e++) {
        // the gaze: from the centre to the iris's centre (iris vertices nearer this eye)
        V3 ic{0, 0, 0};
        int n = 0;
        for (int i = 0; i < nv; i++) {
            if (kind[i] != 2 || weld[i] != i) continue;
            V3 q = P(i);
            V3 d0 = q - C[0], d1 = q - C[1];
            if ((Dot(d0, d0) <= Dot(d1, d1) ? 0 : 1) != e) continue;
            ic = ic + q, n++;
        }
        if (!n) {
            rig.error = "no iris";
            return;
        }
        G[e] = Normalize(ic * (1.0f / n) - C[e]);
    }
    auto edgeKey = [](int a, int b) { return a < b ? ((uint64_t)a << 32) | (uint32_t)b : ((uint64_t)b << 32) | (uint32_t)a; };
    for (int e = 0; e < 2; e++) {
        V3 c = C[e], g = G[e];
        float r = R[e];
        V3 h = Normalize(V3{1, 0, 0} - g * g.x), v = Cross(g, h);  // across the eye (+x), up
        // the sclera's front ring: the boundary loop of this eye's sclera that lies most to the front
        std::unordered_map<uint64_t, int> count;
        for (int s = 0; s < in.subCount; s++) {
            const BakeSub &sb = in.subs[s];
            if (!(sb.flags & BAKE_SCLERA)) continue;
            for (int k = sb.indexStart; k + 2 < sb.indexStart + sb.indexCount; k += 3) {
                int w[3] = {weld[in.indices[k]], weld[in.indices[k + 1]], weld[in.indices[k + 2]]};
                if (sideOf(w[0]) != e || sideOf(w[1]) != e || sideOf(w[2]) != e) continue;
                for (int q = 0; q < 3; q++) count[edgeKey(w[q], w[(q + 1) % 3])]++;
            }
        }
        std::unordered_map<uint64_t, std::vector<uint64_t>> adj;
        for (auto &kv : count) {
            if (kv.second != 1) continue;
            uint64_t a = kv.first >> 32, b = kv.first & 0xffffffffu;
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
        std::vector<uint64_t> front;
        float frontAxial = -1e30f;
        for (auto &ch : Chains(adj)) {
            if (ch.size() < 8) continue;
            float ax = 0;
            for (uint64_t i : ch) ax += Dot(P((int)i) - c, g);
            ax /= ch.size();
            if (ax > frontAxial) frontAxial = ax, front = ch;
        }
        if (front.empty()) {
            rig.error = "no front ring on the sclera";
            return;
        }
        float lim = 0;
        int pick[4] = {-1, -1, -1, -1};
        float best[4] = {1e30f, 1e30f, 1e30f, 1e30f};
        for (uint64_t i : front) {
            V3 d = P((int)i) - c;
            V3 lat = d - g * Dot(d, g);
            lim += sqrtf(Dot(lat, lat));
            float ang = atan2f(Dot(lat, v), Dot(lat, h));
            for (int k = 0; k < 4; k++) {
                float want = k * 1.5707963f, diff = fabsf(remainderf(ang - want, 6.2831853f));
                if (diff < best[k]) best[k] = diff, pick[k] = (int)i;
            }
        }
        rig.limbus[e] = lim / front.size();
        if (pick[0] == pick[2] || pick[1] == pick[3]) {
            rig.error = "sclera front ring too coarse";
            return;
        }
        V3 r0 = P(pick[0]), r1 = P(pick[1]), r2 = P(pick[2]), r3 = P(pick[3]);
        V3 ez = Normalize(Cross(r0 - r2, r1 - r3));
        if (Dot(ez, g) < 0) {
            std::swap(pick[1], pick[3]);
            std::swap(r1, r3);
            ez = Normalize(Cross(r0 - r2, r1 - r3));
        }
        V3 o = (r0 + r1 + r2 + r3) * 0.25f, ex = Normalize(r0 - r2), ey = Cross(ez, ex);
        V3 dc = c - o;
        float loc[6] = {Dot(dc, ex), Dot(dc, ey), Dot(dc, ez), Dot(g, ex), Dot(g, ey), Dot(g, ez)};
        memcpy(rig.local[e], loc, sizeof loc);
        V3 span = r0 - r2;
        rig.span[e] = sqrtf(Dot(span, span));
        for (int k = 0; k < 4; k++) rig.ring[e][k] = pick[k];
        rig.centre[e][0] = c.x, rig.centre[e][1] = c.y, rig.centre[e][2] = c.z;
        rig.gaze[e][0] = g.x, rig.gaze[e][1] = g.y, rig.gaze[e][2] = g.z;
        rig.radius[e] = r;

        // the eyelid contact: where the eyelid skin crosses the sphere 2% outside the eyeball (0.3 mm on VaM's), in front
        // of the centre; one point per crossed edge, one segment per crossed triangle
        float rc = r * 1.02f, reach = r * 1.7f;
        std::unordered_map<uint64_t, float> cross;  // edge -> fraction from its lower to its higher vertex index
        std::unordered_map<uint64_t, std::vector<uint64_t>> link;
        auto f = [&](int i) { V3 d = P(i) - c; return sqrtf(Dot(d, d)) - rc; };
        for (int s = 0; s < in.subCount; s++) {
            const BakeSub &sb = in.subs[s];
            if (!(sb.flags & BAKE_RECEIVER)) continue;
            for (int k = sb.indexStart; k + 2 < sb.indexStart + sb.indexCount; k += 3) {
                int w[3] = {weld[in.indices[k]], weld[in.indices[k + 1]], weld[in.indices[k + 2]]};
                float fv[3];
                bool near = true;
                for (int q = 0; q < 3; q++) {
                    fv[q] = f(w[q]);
                    near = near && fv[q] < reach - rc;
                }
                if (!near) continue;
                uint64_t keys[2];
                int nk = 0;
                bool ahead = true;
                for (int q = 0; q < 3 && nk < 3; q++) {
                    int a = w[q], b = w[(q + 1) % 3];
                    float fa = fv[q], fb = fv[(q + 1) % 3];
                    if ((fa < 0) == (fb < 0) || a == b) continue;
                    float t = fa / (fa - fb);  // from a to b
                    uint64_t key = edgeKey(a, b);
                    float tl = a < b ? t : 1.0f - t;
                    V3 pa = P(std::min(a, b)), pb = P(std::max(a, b));
                    if (Dot(pa + (pb - pa) * tl - c, g) <= 0) ahead = false;
                    if (nk < 2) keys[nk] = key;
                    nk++;
                    cross[key] = tl;
                }
                if (nk != 2 || !ahead) continue;
                link[keys[0]].push_back(keys[1]);
                link[keys[1]].push_back(keys[0]);
            }
        }
        // the polylines: drop stubs, longest first, as many as fit
        std::vector<std::vector<uint64_t>> chains;
        for (auto &ch : Chains(link))
            if (ch.size() >= 4) chains.push_back(ch);
        std::sort(chains.begin(), chains.end(), [](const std::vector<uint64_t> &a, const std::vector<uint64_t> &b) { return a.size() > b.size(); });
        std::vector<EyePoint> pts;
        for (auto &ch : chains) {
            std::vector<uint64_t> use = ch;
            if (pts.empty() && (int)use.size() > kEyePoints) {  // evenly along it, both ends kept
                use.clear();
                for (int k = 0; k < kEyePoints; k++) use.push_back(ch[(size_t)k * (ch.size() - 1) / (kEyePoints - 1)]);
            }
            if ((int)(pts.size() + use.size()) > kEyePoints) continue;
            for (size_t k = 0; k < use.size(); k++) {
                EyePoint p;
                p.a = (int)(use[k] >> 32), p.b = (int)(use[k] & 0xffffffffu);
                p.t = cross[use[k]];
                p.start = k == 0;
                pts.push_back(p);
            }
            rig.chains[e]++;
        }
        if (pts.size() < 4) {
            rig.error = "no eyelid contact";
            return;
        }
        // upper or lower lid: above or below the line through the two corners (the points farthest along the eye), soft
        // over 4% of the radius
        std::vector<float> px(pts.size()), py(pts.size());
        int lo = 0, hi = 0;
        for (size_t k = 0; k < pts.size(); k++) {
            V3 pa = P(pts[k].a), pb = P(pts[k].b), d = pa + (pb - pa) * pts[k].t - c;
            px[k] = Dot(d, h), py[k] = Dot(d, v);
            if (px[k] < px[lo]) lo = (int)k;
            if (px[k] > px[hi]) hi = (int)k;
        }
        float soft = 0.04f * r, yMin = 1e30f, yMax = -1e30f;
        for (size_t k = 0; k < pts.size(); k++) {
            float line = py[lo] + (py[hi] - py[lo]) * (px[k] - px[lo]) / std::max(px[hi] - px[lo], 1e-9f);
            float x = std::min(1.0f, std::max(0.0f, (py[k] - line + soft) / (2.0f * soft)));
            pts[k].upper = x * x * (3.0f - 2.0f * x);
            yMin = std::min(yMin, py[k]), yMax = std::max(yMax, py[k]);
        }
        rig.widthMM[e] = (px[hi] - px[lo]) * 1000.0f;
        rig.heightMM[e] = (yMax - yMin) * 1000.0f;
        rig.points[e] = pts;
    }
    rig.eyes = 2;
}

uint16_t FloatToHalf(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7bff);
    uint32_t h = sign | (exp << 10) | (mant >> 13);
    if (mant & 0x1000) h++;  // round
    return (uint16_t)h;
}

float HalfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
    if (exp == 0) x = sign;
    else x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

bool Bake(const BakeInput &in, BakeOutput &out, std::string &error) {
    const float mm = 1000.0f;
    const float D0 = 2.0f, Lmax = 50.0f, tMin = 0.2f, start = 0.05f;
    int nv = in.vertexCount;
    if (nv <= 0 || !in.pos || !in.nrm || !in.uv || !in.indices) {
        error = "empty mesh";
        return false;
    }
    auto P = [&](int i) { return V3{in.pos[3 * i] * mm, in.pos[3 * i + 1] * mm, in.pos[3 * i + 2] * mm}; };
    auto Nv = [&](int i) { return Normalize(V3{in.nrm[3 * i], in.nrm[3 * i + 1], in.nrm[3 * i + 2]}); };

    // winding: which way the triangles' geometric normals point relative to the vertex normals
    double windSum = 0;
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sb = in.subs[s];
        for (int k = sb.indexStart; k + 2 < sb.indexStart + sb.indexCount; k += 3) {
            int a = in.indices[k], b = in.indices[k + 1], c = in.indices[k + 2];
            if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv) {
                error = "index out of range";
                return false;
            }
            V3 g = Cross(P(b) - P(a), P(c) - P(a));
            windSum += Dot(g, Nv(a) + Nv(b) + Nv(c));
        }
    }
    float wind = windSum >= 0 ? 1.0f : -1.0f;
    out.windingSign = (int)wind;

    double t0 = NowMs();
    std::vector<Tri> tris;
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sb = in.subs[s];
        if (!(sb.flags & BAKE_OCCLUDER)) continue;
        for (int k = sb.indexStart; k + 2 < sb.indexStart + sb.indexCount; k += 3) {
            V3 a = P(in.indices[k]), b = P(in.indices[k + 1]), c = P(in.indices[k + 2]);
            Tri t;
            t.a = a;
            t.e1 = b - a;
            t.e2 = c - a;
            t.n = Cross(t.e1, t.e2) * wind;
            tris.push_back(t);
        }
    }
    out.occluderTriangles = (int)tris.size();
    Bvh bvh;
    bvh.Build(std::move(tris));
    out.msBvh = NowMs() - t0;

    // vertices that need a value: those of receiving submeshes
    std::vector<uint8_t> need(nv, 0);
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sb = in.subs[s];
        if (!(sb.flags & BAKE_RECEIVER) || (sb.flags & BAKE_SURFACE_ONLY)) continue;
        for (int k = sb.indexStart; k < sb.indexStart + sb.indexCount; k++) need[in.indices[k]] = 1;
    }
    out.vertexThickness.assign(nv, kNoThickness);
    std::vector<V3> cone = ConeDirections(std::max(1, in.rays), in.coneDegrees);
    int threads = in.threads > 0 ? in.threads : std::max(1u, std::thread::hardware_concurrency() / 2);
    out.threads = threads;
    std::atomic<int> next(0), dropped(0);
    t0 = NowMs();
    auto work = [&]() {
        for (;;) {
            int begin = next.fetch_add(256);
            if (begin >= nv) break;
            int end = std::min(nv, begin + 256);
            for (int i = begin; i < end; i++) {
                if (!need[i]) continue;
                V3 n = Nv(i), w = n * -1.0f;
                V3 up = fabsf(w.z) < 0.9f ? V3{0, 0, 1} : V3{1, 0, 0};
                V3 tx = Normalize(Cross(up, w)), ty = Cross(w, tx);
                float rot = (float)((i * 2654435761u) >> 8) * (6.2831853f / 16777216.0f);
                float cr = cosf(rot), sr = sinf(rot);
                V3 o = P(i) + w * start;
                double sum = 0;
                int used = 0;
                for (const V3 &c : cone) {
                    float cx = c.x * cr - c.y * sr, cy = c.x * sr + c.y * cr;
                    V3 d = Normalize(tx * cx + ty * cy + w * c.z);
                    float t;
                    int hit = bvh.Nearest(o, d, tMin, Lmax, t);
                    if (hit >= 0 && Dot(bvh.tris[hit].n, d) < 0) {
                        dropped.fetch_add(1);  // met the outside of a surface: started outside the body
                        continue;
                    }
                    used++;
                    if (hit < 0) continue;
                    sum += exp(-(t + start) / D0);
                }
                if (used && sum > 0) out.vertexThickness[i] = (float)std::min<double>(kNoThickness, -D0 * log(sum / used));
            }
        }
    };
    std::vector<std::thread> pool;
    for (int k = 1; k < threads; k++) pool.emplace_back(work);
    work();
    for (auto &th : pool) th.join();
    out.msRays = NowMs() - t0;
    out.droppedRays = dropped.load();
    if (in.smoothMM > 0) SmoothThickness(in, need, out.vertexThickness);
    std::vector<float> noseAmbient;
    std::vector<float> noseDistance;
    NoseVisibility(in, bvh, noseAmbient, noseDistance);
    out.vertexNoseVisibility = noseAmbient;


    // UV-space textures, one per receiving submesh
    t0 = NowMs();
    for (int s = 0; s < in.subCount; s++) {
        const BakeSub &sb = in.subs[s];
        if (!(sb.flags & BAKE_RECEIVER) || sb.indexCount < 3) continue;
        if (sb.flags & (BAKE_NOSTRILS | BAKE_FACE)) {
            std::vector<float> vals(2 * (size_t)nv);
            for (int i = 0; i < nv; i++) vals[2 * i] = out.vertexThickness[i], vals[2 * i + 1] = noseAmbient[i];
            BakeTexture tex = RasterSub(in, s, in.density, vals.data(), 2, kNoThickness,&bvh,&noseDistance);
            out.textures.push_back(std::move(tex));
        } else out.textures.push_back(RasterSub(in, s, in.density, out.vertexThickness.data(), 1, kNoThickness));
    }
    // teeth and tongue: (back, gum) per vertex, a two-channel texture each at half the density (small parts)
    TeethAttributes(in, out.vertexBack, out.vertexGum, out.teeth);
    if (out.teeth.teeth) {
        std::vector<float> vals(2 * (size_t)nv);
        for (int i = 0; i < nv; i++) vals[2 * i] = out.vertexBack[i], vals[2 * i + 1] = out.vertexGum[i];
        for (int s = 0; s < in.subCount; s++) {
            const BakeSub &sb = in.subs[s];
            if (!(sb.flags & (BAKE_TEETH | BAKE_TONGUE | BAKE_MOUTH)) || sb.indexCount < 3) continue;
            BakeTexture tex = RasterSub(in, s, in.density * 0.5f, vals.data(), 2, 0.0f);
            tex.kind = (sb.flags & BAKE_TEETH) ? TEX_TEETH : (sb.flags & BAKE_TONGUE) ? TEX_TONGUE : TEX_MOUTH;
            out.textures.push_back(std::move(tex));
        }
    }
    if (in.regional) for (int s = 0; s < in.subCount; s++) {
        if (!(in.subs[s].flags & BAKE_RECEIVER) || in.subs[s].indexCount == 0) continue;
        BakeTexture tex = RasterSub(in, s, in.density * .5f, in.regional, 1, 1.0f);
        tex.kind = TEX_REGIONAL;
        out.textures.push_back(std::move(tex));
    }
    out.msRaster = NowMs() - t0;
    EyeRigOf(in, out.eyes);
    return true;
}
