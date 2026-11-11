// The one-pass hair shader's constants (shaders/hair_onepass.hlsl, cbuffer HopItem / HopEye), and which Unity shader
// constant fills which field: the plugin copies them from the original draws' constant buffers by name (member offsets
// from hair_programs.h), the offline test from its own values.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define HOP_MAX_LIGHTS 8

enum { HOP_LIGHT_DIRECTIONAL = 0, HOP_LIGHT_POINT = 1, HOP_LIGHT_SPOT = 2 };
enum { HOP_SHADOW_NONE = 0, HOP_SHADOW_DEPTH = 1, HOP_SHADOW_DEPTH_SOFT = 2, HOP_SHADOW_CUBE = 3, HOP_SHADOW_CUBE_SOFT = 4, HOP_SHADOW_SCREEN = 5 };

typedef struct { float v[4]; } HopReg;

typedef struct {
    HopReg pos, color, worldToLight[4], worldToShadow[4], shadowData, positionRange, projectionParams;
    uint32_t kind[4];  // HOP_LIGHT_*, HOP_SHADOW_*
} HopLightData;

typedef struct {
    HopReg tessFactor, length, mat0, mat1, mat2, specularColor, exposureIBL, exposureLM;
    HopReg skyMatrix[4], sh[9], ambient, mainLightPos, mainLightColor;
    HopReg objectToWorld[4], worldToObject[4], stereoMatrixVP[8], stereoCameraPos[2], stereoScaleOffset[2], projectionParams;
    uint32_t flags[4];  // lights, main light screen shadows
    HopLightData lights[HOP_MAX_LIGHTS];
} HopItemData;

#ifdef __cplusplus
static_assert(sizeof(HopLightData) == 14 * 16, "HopLight layout");
static_assert(sizeof(HopItemData) == 46 * 16 + HOP_MAX_LIGHTS * 14 * 16, "HopItem layout");
#endif

// Unity constant -> field. bytes: how much of the field to fill, copied register by register from the constant's offset.
typedef struct { const char *name; uint16_t offset, bytes; } HopField;

#define HOP_ITEM(f) (uint16_t) offsetof(HopItemData, f)
static const HopField kHopItemFields[] = {
    {"_TessFactor", HOP_ITEM(tessFactor), 16},
    {"_Length", HOP_ITEM(length), 16},
    {"_MaxSpread", HOP_ITEM(mat0) + 0, 4},
    {"_RandomBarycentric", HOP_ITEM(mat0) + 4, 4},
    {"_StandWidth", HOP_ITEM(mat0) + 8, 4},
    {"_Diffuse", HOP_ITEM(mat0) + 12, 4},
    {"_FresnelPower", HOP_ITEM(mat1) + 0, 4},
    {"_FresnelAtten", HOP_ITEM(mat1) + 4, 4},
    {"_SpecularShift", HOP_ITEM(mat1) + 8, 4},
    {"_PrimarySpecular", HOP_ITEM(mat1) + 12, 4},
    {"_SecondarySpecular", HOP_ITEM(mat2) + 0, 4},
    {"_RandomTexColorPower", HOP_ITEM(mat2) + 4, 4},
    {"_RandomTexColorOffset", HOP_ITEM(mat2) + 8, 4},
    {"_IBLFactor", HOP_ITEM(mat2) + 12, 4},
    {"_SpecularColor", HOP_ITEM(specularColor), 16},
    {"_ExposureIBL", HOP_ITEM(exposureIBL), 16},
    {"_ExposureLM", HOP_ITEM(exposureLM), 8},
    {"_SkyMatrix", HOP_ITEM(skyMatrix), 64},
    {"_SH0", HOP_ITEM(sh[0]), 12},
    {"_SH1", HOP_ITEM(sh[1]), 12},
    {"_SH2", HOP_ITEM(sh[2]), 12},
    {"_SH3", HOP_ITEM(sh[3]), 12},
    {"_SH4", HOP_ITEM(sh[4]), 12},
    {"_SH5", HOP_ITEM(sh[5]), 12},
    {"_SH6", HOP_ITEM(sh[6]), 12},
    {"_SH7", HOP_ITEM(sh[7]), 12},
    {"_SH8", HOP_ITEM(sh[8]), 12},
    {"glstate_lightmodel_ambient", HOP_ITEM(ambient), 16},
    {"_WorldSpaceLightPos0", HOP_ITEM(mainLightPos), 16},
    {"_LightColor0", HOP_ITEM(mainLightColor), 16},
    {"unity_ObjectToWorld", HOP_ITEM(objectToWorld), 64},
    {"unity_WorldToObject", HOP_ITEM(worldToObject), 64},
    {"unity_StereoMatrixVP", HOP_ITEM(stereoMatrixVP), 128},
    {"unity_StereoWorldSpaceCameraPos", HOP_ITEM(stereoCameraPos), 28},  // float3[2]: register stride
    {"unity_StereoScaleOffset", HOP_ITEM(stereoScaleOffset), 32},
    {"_ProjectionParams", HOP_ITEM(projectionParams), 16},
    {"unity_MatrixVP", HOP_ITEM(stereoMatrixVP), 64},
    {"_WorldSpaceCameraPos", HOP_ITEM(stereoCameraPos), 12},
};
#undef HOP_ITEM
static const int kHopItemFieldCount = (int)(sizeof kHopItemFields / sizeof kHopItemFields[0]);

#define HOP_LIGHT(f) (uint16_t) offsetof(HopLightData, f)
static const HopField kHopLightFields[] = {
    {"_WorldSpaceLightPos0", HOP_LIGHT(pos), 16},
    {"_LightColor0", HOP_LIGHT(color), 16},
    {"unity_WorldToLight", HOP_LIGHT(worldToLight), 64},
    {"unity_WorldToShadow", HOP_LIGHT(worldToShadow), 64},  // the first of four
    {"_LightShadowData", HOP_LIGHT(shadowData), 16},
    {"_LightPositionRange", HOP_LIGHT(positionRange), 16},
    {"_LightProjectionParams", HOP_LIGHT(projectionParams), 16},
};
#undef HOP_LIGHT
static const int kHopLightFieldCount = (int)(sizeof kHopLightFields / sizeof kHopLightFields[0]);
