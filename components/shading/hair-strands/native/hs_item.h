// Layouts shared with shaders\hair_strands.hlsl (HsLight, HsItem, HsView, the HsDispatch constant buffer).
#pragma once
#include <stdint.h>

#define HS_MAX_LIGHTS 16
#define HS_ITEM_SHIFT 26
#define HS_MAX_ITEMS 63
#define HS_POINT_BYTES 36
static inline uint64_t HsPointBufferBytes(uint64_t points) { return ((points + 31) & ~31ull) * HS_POINT_BYTES; }
#define HS_DOMAIN_VALUES (2144 + 2080)  // u of detail 1..64 (k = 0..S), v of density 1..64 (j = 0..L-1)

enum { HS_LIGHT_DIRECTIONAL = 0, HS_LIGHT_POINT = 1, HS_LIGHT_SPOT = 2 };
enum { HS_SHADOW_NONE = 0, HS_SHADOW_DEPTH = 1, HS_SHADOW_DEPTH_SOFT = 2, HS_SHADOW_CUBE = 3, HS_SHADOW_CUBE_SOFT = 4, HS_SHADOW_SCREEN = 5 };

typedef struct { float v[4]; } HsReg;

typedef struct {
    HsReg pos, color, worldToLight[4], worldToShadow[4], shadowData, positionRange, projectionParams;
    uint32_t kind[4];     // HS_LIGHT_*, HS_SHADOW_*
    uint32_t scissor[4];  // eye 0: left | right << 16, top | bottom << 16; eye 1 the same
} HsLight;

typedef struct {
    // the first 45 registers are HairRender onepass's HopItemData (hop_item.h), field for field
    HsReg tessFactor, length, mat0, mat1, mat2, specularColor, exposureIBL, exposureLM;
    HsReg skyMatrix[4], sh[9], ambient, mainLightPos, mainLightColor;
    HsReg objectToWorld[4], worldToObject[4], stereoMatrixVP[8], stereoCameraPos[2], stereoScaleOffset[2], projectionParams;
    uint32_t counts[4];  // patches, strands per patch, segments per strand, first point
    uint32_t flags[4];   // left light mask; screen/eyes + right light mask in high 16 bits; first index; 16-bit indices
} HsItem;

typedef struct {
    float viewport[4];  // left, top, width, height
    float depth[4];     // min, max
    int32_t clip[4];    // left, top, right, bottom (exclusive)
    uint32_t mode[4];   // D3D11_CULL_MODE, front counter-clockwise, D3D11_COMPARISON_FUNC, eye
} HsView;

typedef struct {
    uint32_t dispatch[4];  // item, view, first thread, threads
    uint32_t resolve[4];   // eye 1's first pixel column
} HsDispatch;

typedef struct {
    uint32_t frame[4];             // items, segments
    uint32_t segStart[HS_MAX_ITEMS + 1];  // item i's first segment; [items] = segments
} HsFrameCb;

#ifdef __cplusplus
static_assert(sizeof(HsLight) == 15 * 16, "HsLight layout");
static_assert(sizeof(HsItem) == 47 * 16, "HsItem layout");
static_assert(sizeof(HsView) == 4 * 16, "HsView layout");
static_assert(sizeof(HsFrameCb) == 17 * 16, "HsFrame layout");
#endif
