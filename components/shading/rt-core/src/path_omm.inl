// PT opacity micromaps (API 20; shaders\path_omm.hlsl, frame-rate study 2026-11-02).
// Every cutout / translucent triangle a PT ray passes asked the shaders whether to take it (PtHitTracked, PtVisibility:
// the material, the uvs, a lod-0 alpha read), also where its alpha map is plainly 0 or 1 - hair cards' empty space,
// most of a garment's cloth. 0.22.9's micromaps are the atlas route's (its alpha slices and rules) and stay off here.
// These follow the PT's own rules, so a ray decided in hardware ends as the shaders would have ended it:
//   transparent: skipped by every PT ray - coverage under the cutoff (alpha-tested families: both shaders `continue`
//     before anything else), or coverage 0 where it is the stochastic kind whose shadow factor is 1 - coverage;
//   opaque: taken by every ray - past the cutoff where both shaders then commit / stop whatever the coverage (cutout;
//     volume glass), or coverage 1 where a hit is committed and a shadow ray gets 1 - 1 = 0;
//   anything else - thin sheets (2048), blend-state sheets (2097152), skin (the subsurface probe reads every skin hit
//   on its instance) - stays with the shaders. PtHitTracked counts only coverage strictly between 0 and 1 as varying.
// A material's micromaps wait until its coverage inputs and alpha map (PathMaterial::alphaVersion, 0: none) have
// stayed the same for kPathOmmSettle frames: Record builds before the frame's SetPathMaterial, so a change is one frame
// late, and an animated alpha (a strip fade) keeps the shaders' test instead of baking every frame.
constexpr uint32_t kPathOmmLayer = 524288u;  // test bit 19 (rtcore.h SetTestLayers)
constexpr uint64_t kPathOmmSettle = 8;
constexpr uint32_t kPtOmmCut = 1, kPtOmmZero = 2, kPtOmmAny = 4, kPtOmmOne = 8;  // path_omm.hlsl PT_OMM_*

uint32_t PtFlags(const PtMaterialGpu &r) { uint32_t f; memcpy(&f, &r.params[3], 4); return f; }

// What the coverage of a material's hits reads (PtAlphaAt, PtOpacity and the shaders' branches on it).
uint64_t PtOmmKey(const PtMaterialGpu &r, uint64_t alphaVersion) {
    uint64_t k = Mix(Mix(Mix(0x50744f6d6dull, r.extra[3]), alphaVersion), r.sampling[7]);
    k = Mix(Mix(Mix(k, r.alphaST, 4), r.bump + 3, 1), r.base + 3, 1);
    k = Mix(Mix(k, r.materialControls + 2, 2), r.params + 2, 1);
    return Mix(k, PtFlags(r));
}

// The submesh's micromaps in the PT (see above). `p`: its bake's inputs when it is baked; `sig` takes what it read.
SubOmm PathOmm(const SubGpu &sr, const MatGpu &mr, MaterialKind kind, const GeoGpu &geo, uint64_t frame, PtOmmParamsGpu &p,
               uint64_t &sig) {
    SubOmm so = {0, 0, 0, 0};
    p = {};
    if (sr.blasTess || (kind != MaterialKind::Cutout && kind != MaterialKind::Translucent)) return so;
    auto it = g->pathMaterials.find(sr.material);
    if (it == g->pathMaterials.end() || frame < it->second.ommSince + kPathOmmSettle) return so;
    const PtMaterialGpu &r = it->second.gpu;
    const uint32_t flags = PtFlags(r);
    const float transmission = r.params[2];
    if (flags & 8u) return so;  // skin
    const bool cut = kind == MaterialKind::Cutout || (flags & 8388608u);
    const float cutoff = kind == MaterialKind::Cutout ? mr.color[3] : r.materialControls[3];
    const bool stochastic = kind == MaterialKind::Translucent && !(flags & (1048576u | 2097152u)) &&
                            (transmission < .999f || (flags & 16384u));
    // PtVisibility skips a cosmetic layer's hits (kNoOcclude) whatever their coverage
    const bool shadowSkip = (geo.flags & kNoOcclude) && (transmission < .999f || (flags & 1048576u));
    uint32_t rules = cut ? kPtOmmCut : 0;
    // API 26: a sheer scene object Unity's shadow map takes as a cutout blocks light where its coverage is 0 as well.
    const bool solidShadow = (mr.hasAlpha & 4u) && (g->pathEnvironment.authorRules & kPathAuthorSolid);
    if (stochastic && transmission < .999f && !(flags & 2048u) && !solidShadow) rules |= kPtOmmZero;
    if (!(flags & (2048u | 2097152u)) && !shadowSkip) {
        if (kind == MaterialKind::Cutout && !(flags & 1048576u)) rules |= kPtOmmAny;
        else if ((flags & 1048576u) || stochastic) rules |= kPtOmmOne;
        else rules |= kPtOmmAny;  // translucent volume glass: committed, and a shadow ray stops at it
    }
    if (!rules) return so;
    const float base = r.base[3];
    const float offset = (flags & 32768u) ? r.materialControls[2] : 0.0f;
    if (!std::isfinite(base) || !std::isfinite(offset) || !std::isfinite(cutoff) || !(base >= 0)) return so;
    sig = Mix(Mix(Mix(Mix(sig, 0x5054ull), it->second.ommKey), (uint64_t)kind), rules);
    sig = Mix(Mix(sig, mr.color + 3, 1), (uint64_t)(geo.flags & kNoOcclude));
    const uint32_t opaque = (uint32_t)NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE,
                   clear = (uint32_t)NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT;
    if (r.extra[3] == kNone || geo.uvBuf == kNone) {
        // PtAlphaAt is exactly 1 for every hit: one coverage, 1 x base exact
        const float raw = base + offset, coverage = std::min(std::max(raw, 0.0f), 1.0f);
        if ((cut && coverage < cutoff) || ((rules & kPtOmmZero) && raw <= 0)) so = {1, clear, 0, 0};
        else if ((!cut || coverage >= cutoff) && ((rules & kPtOmmAny) || ((rules & kPtOmmOne) && raw >= 1))) so = {1, opaque, 0, 0};
        return so;
    }
    if (!it->second.alphaVersion) return so;  // a map that may change any frame
    so.mode = 2;
    p.texture = r.extra[3]; p.sampling = r.sampling[7]; p.rules = rules; p.cutoff = cutoff;
    memcpy(p.st, r.alphaST, 16);
    p.adjust = r.bump[3]; p.base = base; p.offset = offset;
    return so;
}
