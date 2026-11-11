// VaM skin transmission (BepInEx 5, Unity 2018.1, C# 3).
//
// VaM's skin shaders light skin from the front only: an ear, a hand or a nostril with a light behind it stays dark.
// VaMSkinTransmissionNative.bin adds the light that comes through thin skin: every skin lighting draw (one per pixel
// light) is followed by a draw of the same geometry that adds that light's transmission, from a thickness texture
// baked per character. This plugin loads the native module early (it recognises VaM's skin shaders as the game
// creates them), sees every character's skin (a Harmony postfix on DAZSkinV2.DrawMeshGPU, the skin's own per-frame
// draw: no scene scans), and two seconds after a skin first draws hands its bind-pose mesh (current morphs) to the
// native bake, which runs on a worker thread; a skin that goes away is dropped again.
//
// Materials: all of the skin's materials let light through except the eyes, the inside of the mouth, the teeth,
// the eyelashes and the nails. Only spot and point lights with shadows add transmission: their shadow map tells
// how much skin the light crosses to get there (0.7.0: through an ear's rim, yes; through the head to an eyelid or
// a lip, through other fingers into a fist, through a nose bone: no), never less than the baked thickness; it falls
// off like a Gaussian per colour (orange-red through thin skin, deep red through thicker, nothing past ~8 mm) and
// only light from behind counts.
//
// Specular ([Specular] Look, 0.4.0): the same skin materials can be drawn through a replacement of VaM's own skin
// pixel program whose highlight is skin-like instead of VaM's single orange-tinted Blinn-Phong lobe: two GGX lobes
// (roughness from the skin's own gloss map), 2.8% reflectance facing the eye rising with Fresnel toward grazing angles,
// fading with N.L, white, single pores not catching the light (the normal map at a mip bias for the highlight, the
// spec map as cavity). The looks differ in roughness only. VaM = VaM's own program. Skins whose bake has not finished
// keep VaM's program until it has.
//
// 0.11.0, the same for every skin preset: presets set VaM's gloss for VaM's own lobe (Gloss Texture Offset 0.1 .. 0.84),
// which made the GGX lobes up to 3x smoother on some presets than on the person the looks were picked on (highlights
// burnt out to white sheets). With [Specular] on, every skin gets the looks' roughness at its mean gloss (that
// person's: alpha^2 0.155), its own gloss map's variation kept, and the highlight's strength from its spec map over that
// map's own mean (Specular Intensity only fades highlights out below 0.25); the cavity's spec map is filtered to 1.2 mm.
// With [Diffuse] on, the normal map's pore-sized detail is left out of the diffuse light (normal map filtered to 1.2 mm:
// light under skin spreads about that far, and DLSS's mip bias otherwise draws pores at the output pixel, coarser than
// they are) and the ambient is lit with that diffuse normal (VaM lights it with the specular normal, bent up to 2.2x).
// 0.12.0, every skin in the same state inside too: most presets' gloss maps are near black under a big Gloss Texture
// Offset, so VaM's gloss hardly varies across the face (lips, nose and cheeks alike) and 0.11.0's even roughness made
// every face one matte sheet. With [Specular] Normalize on, each gloss map's own p50 (typical skin) gets the test
// person's typical roughness (alpha^2 0.2) and its p90 (T-zone, nose; lips beyond) 1.7x smoother alpha: each map is
// stretched as far as it needs (Feifei's near-flat map ~38x, Anj ~6.5x, Qis ~1.5x, yingying ~1x in log alpha) from
// its own p50 .. p90, which the native module reads once per texture; clamped 0.12 .. 2x.
// 0.13.0, four steps per skin instead of one state (user: Normalize and one 4-step choice of how glossy and how much
// spread, each skin's starting step computed, 0.12.0's state = step 3; head and body together under one choice; the
// steps are one absolute scale, natural matte .. dewy, never VaM's own look; a step picked is the user's taste): 1
// natural matte, 2 soft, 3 satin (0.12.0), 4 dewy, the same on every skin (TierRows). A skin starts at its author's
// style, read from its face material through VaM's own formula (SkinGloss: from the face gloss, spec and normal maps'
// stats the native module reads once, and the skin sliders); a step chosen in the VR menu is kept for that skin
// (BepInEx\config\local.vam.skintransmission.tiers.txt). Every gloss map of the character gets its step's row (the
// face's maps the face rows, the others the body rows, whose nearly flat gloss maps are not stretched into glints);
// the controls: [Specular] Normalize (off = VaM's own highlight) and the steps (VR menu, SkinTiers / SetSkinTier).
// 0.18.0, one continuous level per skin instead of four steps, face and body balanced (their gloss differed more than
// looked right; a slider, and 0 is no highlight or a very low one): the level runs
// 0 .. 4, 1 .. 4 the four steps and between them the rows in between, below 1 step 1 with its highlight fading to none at 0.
// The face and body maps were each anchored at their whole texture's p50 and their spec map's whole mean (UV padding,
// scalp, nails, unused space: Red's torso highlight came out ~3x her cheeks', her limbs' ~0.4x; TEST's face p50 was the
// black padding): now each gloss map gets a row of its own (ST_SetTierRow) with its p50 / p90 and its spec map's mean
// over its skin texels only (the UV coverage of the face map's Face material, of a body map's skin materials; native
// ST_TexStatsMasked), so a level's typical skin is the same on face and body. The body's rows spread their map half as
// far as the face's (R's square root): a body map's p50 .. p90 is mostly paint and noise, which made chests up to four
// reading steps glossier than backs and thighs (investigations\skin-gloss-slider-20261019). A skin starts at the level
// its author's style reads (SkinGloss, the same reading as 0.13.0, now not rounded to a step); a level set in the VR menu
// is kept for that skin (tiers.txt "level<TAB>key"; 0.13.0's steps read as levels 1-4). VR menu: SkinGlossLevels /
// SetSkinGlossLevel.
//
// 0.5.0: the light that gets through is spread sideways like in real skin (smoothed over the surface in the bake, a
// wider weighted shadow kernel): no blotches or hard edges on ears.
//
// Diffuse (0.6.0 Medium as the base of [Diffuse] OverallScattering since 0.17.2): the same replacement programs redo
// VaM's diffuse falloff. VaM's lit side stays (its own factor, whatever the skin's subdermis colour), turning slightly
// warmer toward the terminator; where VaM's N.L stops at a flat floor and then cuts off where the shadow starts, the
// light now runs on past the terminator and fades out, red farthest and blue least (red's wrap 1.6x green's). This is
// OverallScattering 0; 0.5 (default since 0.20.0) and 1 blend the reviewed warm-gray broad response over it (by the
// regional weight). Three steps since 0.20.0 (the user's pick, 0.5 best); 0.19's 0-2 range went with its 2.
// 11-10: the setting is removed; always 0, the Medium base alone.
// One diffuse model, one setting: the former [Diffuse] Look (VaM / Subtle / Medium / Strong) is gone.
//
// Teeth ([Teeth] Look, 0.8.0): VaM lights teeth and tongue as if nothing were around them (back teeth as bright as the
// front ones, no shadow from lips or cheeks) and the teeth at twice the diffuse of skin (their Subsurface Color is 0).
// The bake also measures the mouth: along the teeth from the front teeth to the last molars, and on each tooth from
// its biting edge to its gum end. Even = the teeth at 1x diffuse; Natural / Deep = that, and teeth and tongue darker
// toward the back of the mouth and toward the gums. VaM = VaM's own.
//
// Eyes ([Eyes] Look, 0.9.0): VaM lights the eyeballs as if the eyelids were not there (the white of the eye as bright
// under the upper lid as in the middle) and its cornea reflection is too weak to show a catchlight. The bake also finds
// each eyeball (a sphere fitted to the sclera) and the line where the eyelids touch it; the eye draws then darken the
// eyeball toward the lids (a shadow under the upper lid, a little at the lower one, following blinks and the gaze) and
// add each light's catchlight on the cornea. Subtle / Natural / Deep = how much. VaM = VaM's own. A character wearing
// eye overlay items of its own (clothing that sits on the eyeball: shadow layers, reflection layers) keeps those: dark
// ones turn the eyelid shadow off for that character, shiny ones the catchlight (checked when clothing is switched
// on or off, from the items' own materials, no scene scans).
// Subsurface scattering ([Subsurface] Strength, 0.10.0): light that falls on skin comes out again a few millimetres
// further on, red farthest (d'Eon & Luebke's measured skin profile): lip edges, nostrils, eyelid rims and the edge of a
// shadow on skin glow softly red instead of ending hard. Done on the eye camera's picture after its opaque pass, from the
// direct diffuse light the skin draws also write; specular and ambient light stay. 0 = off.
//
// Test API (VaM test agent): Status() (JSON), SetEnabled(on), SetParams(strength, wrap, thicknessScale, maxMM, depthR,
// depthG, depthB) (0.7.0: depths = Gaussian sigmas, mm), SetShadow(insetMM, radiusMM, falloffMM), SetBakeSmooth(mm), SetLook(old look name), SetHighlight(normalize), SetSpecular(on, mipBias,
// pore, f0, roughness, sharp, wide, sharpWeight), SetDiffuseLook(name), SetScatter(on, aR, aG, aB, pR, pG, pB),
// SetTeethLook(name), SetTeeth(on, subsurface, gain, backStrength, backStart, gumStrength, gumPower), SetEyesLook(name),
// SetEyes(occOn, upper, upperWidth, lower, lowerWidth, catchOn, strength, alpha, f0, cornea), SetEyeSkin(id, allow),
// SetSubsurface(strength, width), SetSkinDetail(refA2, lo, hi, intensity, diffuseMM, ambient, cavityMM, tiers), SetRtTest(layers),
// SetShadowMapSizeTest(size), SetDiag(on), Rebake(),
// ResetTiming(), ForceLevel(level), SetOwnRows(on), SetTierRow(row, a2, r, lo, hi, bias, gain, spread); for the VR menu SkinGlossLevels(),
// SetSkinGlossLevel(uid, level).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

[BepInPlugin("local.vam.skintransmission", "VaM Skin Transmission", "0.23.9")]
public sealed class VaMSkinTransmission : BaseUnityPlugin
{
    private const string Native = "VaMSkinTransmissionNative";
    private const int ApiVersion = 45;  // API 45: scene record 23, Unity's reflection probe. API 44: ST_SetPathAuthor's background distances. API 43: ST_SetPathAuthor, RtLightRec.shadows bits, RT_DRAW_NO_CAST, draw kind 0x400, path mode 65536; 0.23.5: RT_DRAW_CAMERA_ONLY (RtScene.CameraOnly); 0.23.4: ST_GetPathObservedV1 (RtScene author shaders); 0.23.3: ST_SetPathUiTexturesV1 (PathCanvasUi)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate ulong BufferBytesFn(IntPtr buffer);
    private static BufferBytesFn stPathBufferBytes;
    internal static ulong PathBufferBytes(IntPtr buffer) { return stPathBufferBytes!=null?stPathBufferBytes(buffer):0; }
    private const int OpInstall = 1, OpBegin = 2, OpEnd = 3, OpFrame = 4;
    private const int Receiver = 1, Occluder = 2, Teeth = 4, Tongue = 8, Lips = 16, Sclera = 32, Iris = 64, Pupil = 128, Nostrils = 256, Face = 512, Mouth = 1024, SurfaceOnly = 2048, MouthAO = 4096,
                      RtOnly = 32768, RtPartShift = 16;
    // 0.22.0: the lighting atlas's parts (RtCore rtc::Part; texel sizes: eyes 0.5 mm, face and mouth 1 mm, hands 1.5 mm,
    // the body 2 mm)
    private const int PartBody = 0, PartFace = 1, PartHand = 2, PartMouth = 3, PartEye = 4;
    private const float SettleSeconds = 2f;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    // Bound with GetProcAddress: VaM's Mono looks for [DllImport] libraries in VaM_Data\Plugins only.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PtrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void SetFn(int value);
    private static SetFn stPathTracing;
    private static IntFn stPathSceneActive;
    private static IntFn stPathRenderingActive;
    private static IntFn stPathRequested;
    internal static bool PathSceneActive { get { return stPathSceneActive!=null && stPathSceneActive()!=0; } }
    internal static bool PathRenderingActive { get { return rtRenderer!=null && rtRenderer.Value=="PathTracing" && stPathRenderingActive!=null && stPathRenderingActive()!=0; } }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn(IntPtr resource);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadFn(byte[] buf, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void VoidFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void PtrArgFn(IntPtr p);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int BakeFn(ref BakeDesc desc);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ShadowFn(float exitM, float radiusM, float falloffM);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FloatFn(float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void Float2Fn(float a, float b);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void Float3Fn(float a, float b, float c);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void SpecFn(int on, float mipBias, float pore, float f0, float roughness,
                                                                                      float sharp, float wide, float sharpWeight);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ParamsFn(float strength, float wrap, float thicknessScale, float maxMM,
                                                                                        float depthR, float depthG, float depthB);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ScatFn(int on, float aR, float aG, float aB, float pR, float pG, float pB);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void DetailFn(float refA2, float lo, float hi, int intensity, float diffuseMM, int ambient, float cavityMM, int tiers);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void TiersFn(float[] rows, int n, int defaultRow);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void GlossTierFn(IntPtr tex, int row);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int TexStatsFn(IntPtr tex);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int TexStatsMaskedFn(IntPtr tex, byte[] mask, int mw, int mh);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void TierRowFn(int row, float[] v);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int TexResultFn(int id, [Out] float[] v);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void TeethFn(int on, float subsurface, float gain, float backStrength,
                                                                                       float backStart, float gumStrength, float gumPower);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void EyesFn(int occOn, float upper, float upperWidth, float lower,
                                                                                      float lowerWidth, int catchOn, float strength, float alpha,
                                                                                      float f0, float cornea);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void EyeSkinFn(int skin, int allow);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void NoseFilterFn(int skin, float faceFilter);

    // native StBakeDesc: every field 8 bytes
    [StructLayout(LayoutKind.Sequential)]
    private struct BakeDesc
    {
        public long version;
        public IntPtr indexBuffer;
        public long skin;
        public long vertexCount;
        public IntPtr pos, nrm, uv;
        public long indexCount;
        public IntPtr indices;
        public long subCount;
        public IntPtr subIndexStart, subIndexCount, subFlags, subGpuStart;
        public IntPtr subST;
        public IntPtr regional;
    }

    private static IntFn stApiVersion, stInstalled, stRtAvailable;
    private static PtrFn stRenderEventFunc, stRenderEventAndDataFunc;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void RtTestFn(uint layers);
    private static RtTestFn stSetRtTest;
    private static SetFn stSetPathBounces;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void PathAuthorFn(uint rules, float ambient, float contact, float ambientRange, float emitter, float backgroundNear, float backgroundFar);
    private static PathAuthorFn stSetPathAuthor;
    private static ConfigEntry<int> rtDetail;
    private static ConfigEntry<string> rtRenderer;
    private static FloatFn stSetRtDetail;
    private static SetFn stSetEnvSpecNormal, stSetSkinGrazing, stSetSpecDisc;
    private static FloatFn stSetSkinFuzz;
    private static SetFn stSetEnabled;
    private static InitFn stInit;
    private static ReadFn stRead, stLastError, stPathNotice;
    private static readonly byte[] pathNoticeBuffer = new byte[256];
    private static VoidFn stResetTiming;
    private static PtrArgFn stUnregister;
    private static BakeFn stBake;
    private static ParamsFn stSetParams;
    private static ShadowFn stSetShadow;
    private static FloatFn stSetBake, stSetNose, stSetOverallTranslucency;
    private static SetFn stSetRegional;
    private static NoseFilterFn stSetNoseFilter;
    private static SpecFn stSetSpecular;
    private static ScatFn stSetScatter;
    private static DetailFn stSetSkinDetail;
    private static TiersFn stSetTiers;
    private static GlossTierFn stSetGlossTier;
    private static TexStatsFn stTexStats, stNormalSlope;
    private static TexStatsMaskedFn stTexStatsMasked;
    private static TierRowFn stSetTierRow;
    private static TexResultFn stTexStatsResult;
    private static TeethFn stSetTeeth;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void MouthFn(float ao, float wet);
    private static MouthFn stSetMouth;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void TongueHeightFn(int skin,int height);
    private static TongueHeightFn stSetTongueHeight;
    private static Float2Fn stSetSubsurface;
    private static FloatFn stSetSubsurfaceView;
    private static EyesFn stSetEyes;
    private static EyeSkinFn stSetEyeSkin;
    private static SetFn stSetDiag;
    private static Float3Fn stSetTranslucency;

    private static IntPtr renderEvent;
    private static string loadError;
    private static VaMSkinTransmission instance;
    private static ConfigEntry<float> strength, sssStrength;
    private static float sssWidth = 1f;   // measured skin (user 2026-10-18: back from 3 to 1)
    private static ConfigEntry<string> teethLook, eyesLook;
    private const string BaseDiffuse = "Medium";   // OverallScattering 0 (see the header)
    private static ConfigEntry<bool> specNormalize;
    private static ConfigEntry<bool> mouthEnabled, tongueMaterial;
    private static ConfigEntry<float> mouthStrength;
    private static ConfigEntry<int> rtPathBounces;
    // PT reads a scene's lights the way its author saw them in Unity (docs\pt-author-intent-20261109, rtcore path_tracing.h kPathAuthor*):
    // near plane, shadowless lights, no-receive surfaces, ambient and emitters, cast-off objects, solid shadows of see-through
    // casters, shadow edges by strength on what is not the person, bounces under a low ambient, Unity's spot, panorama spheres.
    private const uint PathAuthorRules = 1u | 2u | 8u | 16u | 32u | 64u | 128u | 256u | 1024u | 2048u | 4096u | 8192u | 65536u;
    // 11-09 (docs\pt-speed-20261109; user: mainly VR, far things have no visual weight): [RayTracing] PathBackground, where what is not the person is traced
    // for less. Off: nowhere. Far: past 8 m from the camera, whole past 16 m. All: everywhere. Each display mode's preset keeps its own value.
    private const uint PathBackgroundRules = 16384u | 32768u;
    private static ConfigEntry<string> rtPathBackground;
    private static float pathBackgroundNear, pathBackgroundFar;  // test: SetPathBackgroundTest
    private const float PathAuthorAmbient = .5f, PathAuthorContact = .6f, PathAuthorAmbientRange = 1f, PathAuthorEmitter = .25f;
    private static bool pathAuthorTest;
    private static ConfigEntry<int> shadowMapSizeCfg;
    private static bool tongueSurfaceOn = true;

    // [Specular] Normalize (0.12.0; until 0.11.0 one Look VaM / Soft / Natural / Moist, carried over once): the skin GGX's
    // perceptual roughness scale 1.2 (the lobes' mean alpha at ~0.34 on step 3, measured faces' Beckmann m 0.35-0.4;
    // 0.12.0's Gloss 100), normal map 2.5 mips blurred for the highlight, pore widening 1, F0 0.028, lobes at 0.75x and
    // 1.3x the roughness, the sharp lobe's weight 0.85
    private static readonly string[] OldLookNames = { "VaM", "Soft", "Natural", "Moist" };
    // the filter widths (mm, 0.11.0); for a gloss map without a step row (none on the programs without skin detail): the
    // 0.11.0 ratio around 0.12.0's alpha^2 0.2, clamped 0.12 .. 2x
    private const float DetailFilterMM = 1.2f, DetailMedA2 = 0.2f, DetailStretchLo = 0.12f, DetailStretchHi = 2f;
    // 0.13.0 steps: per row alpha^2 at the gloss map's own p50, R (alpha at p50 over alpha at p90: the map's texels spread
    // by its own p50 .. p90), alpha^2 floor, ceiling, the gloss map's mip bias, the highlight's gain, the least p90 - p50
    // the spread is divided by, 0; rows 0-3 the face's steps 1-4, rows 4-7 the body's (the same, the spread at least
    // 0.03: Red's body map has p50 0.054, p90 0.063). Step 3 = 0.12.0's state (alpha^2 0.2, R 1.7, 0.024 .. 0.4). How much
    // the highlight stands out (tier_match.py's reading on Red): 0.12, 0.43, 0.72, 0.79; step 1 back past 0.13.0's first
    // candidates' (0.39), step 4 half their step above 3 (step 1 was to go back; step 4 was too much to be of use).
    // 0.18.0: the body rows' R is the face's square root (1.14, 1.225, 1.304, 1.342).
    private static readonly float[] TierRows = {
        0.38f, 1.3f, 0.06f, 0.55f, 0f, 0.9f, 0.01f, 0f,
        0.28f, 1.5f, 0.035f, 0.47f, 0f, 0.95f, 0.01f, 0f,
        0.20f, 1.7f, 0.024f, 0.40f, 0f, 1f, 0.01f, 0f,
        0.175f, 1.8f, 0.02f, 0.37f, 0f, 1f, 0.01f, 0f,
        0.38f, 1.1402f, 0.06f, 0.55f, 0f, 0.9f, 0.03f, 0f,
        0.28f, 1.2247f, 0.035f, 0.47f, 0f, 0.95f, 0.03f, 0f,
        0.20f, 1.3038f, 0.024f, 0.40f, 0f, 1f, 0.03f, 0f,
        0.175f, 1.3416f, 0.02f, 0.37f, 0f, 1f, 0.03f, 0f };
    private const int TierCount = 4, TierDefaultRow = 2;   // until a skin's level is known: the face's step 3
    // 0.18.0's levels: 0 .. 4 (1 .. 4 = the steps); a skin's starting level from SkinGloss: 1 at -0.675 (Qis, the
    // roughest gloss VaM has, n ~2: the middle of step 1), one more every 0.75 (the test person, the wettest preset, at
    // 1.54: 3.95), 1 .. 4 (0.13.0's cuts -0.3 / 0.45 / 1.2 lie halfway: the rounded levels are its steps)
    private const float LevelMax = 4f, LevelFrom = -0.675f, LevelPerStep = 0.75f;
    private const float SlopeUnknown = 0.15f;   // a normal map whose slope cannot be read (presets: 0.06 .. 0.32)
    private const float TierCheckSeconds = 0.5f;
    private static readonly float[] tierRowsNow = (float[])TierRows.Clone();
    private static float forcedLevel = -1f;   // test: every skin at this level (< 0 = each its own)
    // 0.18.0: the gloss maps' own rows, 8 .. 255 (0-7: the steps' rows, for maps without one of their own)
    private const int FirstOwnRow = 8, RowCount = 256;
    private static readonly bool[] rowUsed = new bool[RowCount];
    private static bool ownRows = true;   // test: false = every map on the steps' rows with its whole map's stats (0.17.1)
    private const int MaskSize = 256;   // the UV coverage masks (ST_TexStatsMasked reads the <= 256 px mip)
    private static bool specOnNow, diffOnNow;   // the looks as applied (config or test API)

    // the diffuse base (and the test API's choices): VaM's own diffuse factor (any subdermis colour) times each channel's wrap ((N.L + a) / (1 + a))^p over
    // green's at VaM's saturated N.L. Green = VaM's lit-side curve for its default subdermis colour (a 0.863, p 2.964);
    // red's a = green's x k, blue's = green's / k, each p so that N.L 0.5 gives green's value (VaM's at N.L 0.5 and 1);
    // k = 1.3 / 1.6 / 2.0.
    private static readonly string[] DiffuseNames = { "VaM", "Subtle", "Medium", "Strong" };
    private static readonly float[][] DiffuseA = {
        null, new[] { 1.1219f, 0.8630f, 0.6638f }, new[] { 1.3808f, 0.8630f, 0.5394f }, new[] { 1.7260f, 0.8630f, 0.4315f } };
    private static readonly float[][] DiffuseP = {
        null, new[] { 3.4470f, 2.9640f, 2.5916f }, new[] { 3.9291f, 2.9640f, 2.3583f }, new[] { 4.5712f, 2.9640f, 2.1557f } };

    // [Teeth] Look: the teeth's Subsurface Color in VaM's diffuse factor (1 = 1x facing the light; VaM's teeth 0 = 2x)
    // and the mouth occlusion: darkening at the last molars and where it starts along the teeth (0 front teeth .. 1 last
    // molars; canines ~0.2), darkening at a tooth's gum end and its power (higher = closer to the gum)
    private static readonly string[] TeethNames = { "VaM", "Even", "Natural", "Deep" };
    private static readonly float[] TeethBack = { 0f, 0f, 0.6f, 0.85f }, TeethBackStart = { 0f, 0f, 0.15f, 0.1f };
    private static readonly float[] TeethGum = { 0f, 0f, 0.35f, 0.55f }, TeethGumPower = { 2f, 2f, 2f, 1.5f };

    // [Eyes] Look: eyelid occlusion (upper lid darkening and width, lower lid darkening and width; widths in eyeball radii:
    // Natural 2.2 mm under the upper lid, 1 mm at the lower one on VaM's 15 mm eyeball) and the catchlight's strength (one
    // GGX lobe, alpha 0.05, F0 0.025, on a cornea of 0.6 eyeball radii)
    private static readonly string[] EyesNames = { "VaM", "Subtle", "Natural", "Deep" };
    private static readonly float[] EyeUpper = { 0f, 0.35f, 0.55f, 0.75f }, EyeUpperWidth = { 0.146f, 0.12f, 0.146f, 0.18f };
    private static readonly float[] EyeLower = { 0f, 0.2f, 0.3f, 0.45f }, EyeLowerWidth = { 0.066f, 0.05f, 0.066f, 0.08f };
    private static readonly float[] EyeCatch = { 0f, 0.6f, 1f, 1.3f };

    private static readonly Regex TeethName = new Regex("^teeth", RegexOptions.IgnoreCase), TongueName = new Regex("^tongue", RegexOptions.IgnoreCase),
                                  LipsName = new Regex("^lips", RegexOptions.IgnoreCase), ScleraName = new Regex("^sclera", RegexOptions.IgnoreCase),
                                  IrisName = new Regex("^iris", RegexOptions.IgnoreCase), PupilName = new Regex("^pupil", RegexOptions.IgnoreCase);
    private static readonly Regex NotReceiver = new Regex("cornea|eyereflection|eyelash|gums|innermouth|iris|lacrimal|pupil|sclera|tear|teeth|tongue|nail|defaultmat",
                                                          RegexOptions.IgnoreCase);
    private static readonly Regex NotOccluder = new Regex("cornea|eyereflection|eyelash|tear|defaultmat", RegexOptions.IgnoreCase);
    private static readonly Regex FaceName = new Regex("^face$", RegexOptions.IgnoreCase);
    // 0.22.0 lighting atlas: parts by material name; nails and the genital graft drawn ray-traced only
    private static readonly Regex EyePart = new Regex("^sclera|^iris|^pupil", RegexOptions.IgnoreCase),
                                  MouthPart = new Regex("^teeth|^tongue|gums|innermouth", RegexOptions.IgnoreCase),
                                  FacePart = new Regex("^face$|^lips|^nostrils|^ears|lacrimal", RegexOptions.IgnoreCase),
                                  HandPart = new Regex("^hands|finger", RegexOptions.IgnoreCase),
                                  RtOnlyName = new Regex("nail|^defaultmat$", RegexOptions.IgnoreCase);

    private static int PartOf(string name)
    {
        if (EyePart.IsMatch(name)) return PartEye;
        if (MouthPart.IsMatch(name)) return PartMouth;
        if (FacePart.IsMatch(name)) return PartFace;
        if (HandPart.IsMatch(name)) return PartHand;
        return PartBody;
    }
    // levels set in the VR menu, per skin key (BepInEx\config\local.vam.skintransmission.tiers.txt: "level<TAB>key")
    private static readonly Dictionary<string, float> levelChosen = new Dictionary<string, float>();
    private static string tierFile;
    private static float tierNextCheck;
    private static bool levelsDirty;   // to be saved at the next check
    private static Type tAtom;
    private static MemberInfo mAtomUid;

    // ---- skins -------------------------------------------------------------------------------------------------------

    private static FieldInfo fMesh, fDazMesh, fGpuMaterials;
    private static Type tCharacter;
    private static FieldInfo fCharacterIsMale;
    private static PropertyInfo pMorphedVerts, pMorphedNormals, pMaterialNames;
    // eye overlay items: DAZSkinWrap's skin, mesh, materials; DAZMesh's base-space UV-mapped mesh and vertices
    private static FieldInfo fWrapSkin, fWrapDazMesh, fWrapMaterials, fWrapEnabled, fUvMappedMesh;
    private static PropertyInfo pUVVertices;
    private static string eyeAsideError;

    private sealed class Skin
    {
        public Component component;
        public int id;
        public float firstSeen;
        public Mesh bakedMesh;
        public IntPtr indexBuffer;
        public int job;
        public float noseFaceFilter = float.NaN;
        public Texture tongueNormal;
        public readonly TongueSurface.Binding tongueSurface = new TongueSurface.Binding();
        public string state = "waiting";
        public string mouthEligibility = "unknown";
        // the eyeballs in the base mesh's space (eye overlay items are made in it), 0 radius until baked
        public Vector3[] eyeCentre = new Vector3[2];
        public float eyeRadius;
        public int eyeAllow = 3;      // bit 0 eyelid occlusion, bit 1 catchlight
        public string eyeItems = "";  // the overlay materials found (status)
        // its highlight level (0.13.0 steps, 0.18.0 levels): the character, its face's textures as last seen, the stats
        // asked for (ST_TexStats / ST_NormalSlope ids), the skin's key for a level set, its reading, starting level and
        // level (< 0 = not known yet), each gloss map
        public string uid = "", key = "";
        public Texture faceGloss, faceSpec, faceMain, faceNormal;
        // 0.23.1: CheckTier's tongue and face submeshes, found again only when the mesh's name list is a new one
        // (Mono's Regex.IsMatch makes garbage at each call; this ran for every name of every skin twice a second)
        public string[] tierNames;
        public int tierFace = -1;
        public readonly List<int> tierTongues = new List<int>();
        public int glossQuery, specQuery, normalQuery;
        public float glossP50 = -1f, glossP90 = -1f, specMean = -1f, slope = -1f, gloss = float.NaN;
        public float shininess, glossOffset, specInt, specOffset, fresnel, specBump, specColour;
        public float autoLevel = -1f, level = -1f;
        public readonly Dictionary<Texture, GlossMap> maps = new Dictionary<Texture, GlossMap>();
    }

    // A skin's gloss map (0.18.0): its own row and its skin texels' stats (ST_TexStatsMasked ids; p50 < 0 = not read)
    private sealed class GlossMap
    {
        public Texture spec;
        public string name = "";
        public bool face;             // the face's map (its Face material), else the body's
        public int row = -1;          // its own row, -1 = none (the steps' rows)
        public int bound = -2;        // the row the texture was last given
        public int glossQuery, specQuery;
        public float p50 = -1f, spread, specMean = -1f, specOffset;
        public bool failed;           // not read: the steps' row of its level, t18's stats
        public float written = float.NaN;   // the level its row holds
    }
    private static readonly Dictionary<int, Skin> skins = new Dictionary<int, Skin>();
    private static readonly List<int> gone = new List<int>();
    // clothing wraps seen switching on (DAZSkinWrap.OnEnable); classified again a second after any clothing switches
    private static readonly List<Component> wraps = new List<Component>();
    private static float eyesDirtyAt = -1f;
    private const float EyeSettleSeconds = 1f;

    private sealed class Hooked
    {
        public Camera camera;
        public CommandBuffer begin, end;
    }
    private static readonly List<Hooked> hooked = new List<Hooked>();

    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception(Native + " has no " + name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    private static FieldInfo Field(Type t, string name)
    {
        FieldInfo f = t.GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        if (f == null) throw new MissingFieldException(t.Name, name);
        return f;
    }

    private static PropertyInfo Property(Type t, string name)
    {
        PropertyInfo p = t.GetProperty(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        if (p == null) throw new MissingMemberException(t.Name, name);
        return p;
    }

    private void Awake()
    {
        instance = this;
        bool enabled = Config.Bind("General", "Enabled", true,
            "The plugin: light through thin skin (ears, hands, nostrils) and the [Specular] / [Diffuse] / [Teeth] changes. Off: nothing is loaded, VaM is unchanged. Read at startup.").Value;
        strength = Config.Bind("General", "Strength", 1.0f, new ConfigDescription("How much light comes through thin skin (1 = as measured for skin; 0 = none, no extra draws).",
            new AcceptableValueRange<float>(0f, 4f)));
        // 0.12.0: the former [Specular] Look carried over once into Normalize, then dropped from the file; 0.13.0: Gloss
        // dropped (each skin's step instead)
        string oldLook = Config.Bind("Specular", "Look", "", "until 0.11.0").Value;
        Config.Remove(new ConfigDefinition("Specular", "Look"));
        Config.Bind("Specular", "Gloss", 100, "until 0.12.0");
        Config.Remove(new ConfigDefinition("Specular", "Gloss"));
        specNormalize = Config.Bind("Specular", "Normalize", true,
            "Skin highlights. Off = VaM's own (each skin preset's gloss as its author set it: one oily, orange-tinted lobe; single " +
            "pores glittering; some presets burn out to white sheets, others are matte all over). On = skin-like highlights: two " +
            "GGX lobes, 2.8% reflectance facing you rising toward the edges, white, no single pores glittering; cheeks matte, " +
            "T-zone and nose smoother, lips smoothest, at one level per skin from 0 (no highlight) through 1 natural matte, " +
            "2 soft, 3 satin to 4 dewy (how glossy and how much spread together; face and body alike, each measured on its skin " +
            "only). A skin starts at the level its own VaM highlight suggests; a level set in the VR menu is kept for that skin.");
        int old = Array.IndexOf(OldLookNames, oldLook);
        if (old >= 0) specNormalize.Value = old > 0;
        Config.Save();
        tierFile = System.IO.Path.Combine(Paths.ConfigPath, "local.vam.skintransmission.tiers.txt");
        LoadTiers();
        // 11-10, removed: [Diffuse] OverallScattering (整体散射 / 整体透光,
        // the existing lighting's translucency layer and broad response) is gone; the native amount stays at its off step, 0.
        teethLook = Config.Bind("Teeth", "Look", "Natural", new ConfigDescription(
            "Teeth and tongue. VaM = VaM's own (teeth twice as bright as skin in the same light, back teeth as bright as front ones). " +
            "Even = the teeth as bright as skin. Natural / Deep = that, and the inside of the mouth darker toward the back and " +
            "toward the gums, a little / more (only where the geometric [Mouth] occlusion does not run: it replaces this).", new AcceptableValueList<string>(TeethNames)));
        eyesLook = Config.Bind("Eyes", "Look", "Natural", new ConfigDescription(
            "Eyes. VaM = VaM's own (the eyeball as bright under the eyelids as in the middle, no catchlight). Subtle / Natural / Deep = " +
            "the eyeball darker toward the eyelids (mostly under the upper lid, following blinks and the gaze) and a catchlight of " +
            "each light on the cornea, a little / as in life / more. Characters wearing eye overlay items keep their own: dark ones " +
            "turn the eyelid shadow off for them, shiny ones the catchlight.", new AcceptableValueList<string>(EyesNames)));
        mouthEnabled = Config.Bind("Mouth", "Enabled", true, "Geometry-driven oral cavity occlusion (female characters), including direct light and highlights. The only mouth occlusion where it runs: [Teeth] Look's back/gum darkening is not added on top.");
        mouthStrength = Config.Bind("Mouth", "Strength", .95f, new ConfigDescription("Oral occlusion strength; .95 is the approved contrast.", new AcceptableValueRange<float>(0f, 1f)));
        tongueMaterial = Config.Bind("Mouth", "TongueMaterial", true, "Approved tongue normal/gloss and wet tip on compatible tongue UV layouts. Original appearance textures are preserved.");
        rtDetail = Config.Bind("RayTracing", "RRDetail", 75, new ConfigDescription("RR material-detail retention: 0 = reconstructed image; 100 = full current-frame deterministic material detail.", new AcceptableValueRange<int>(0, 100)));
        rtRenderer=Config.Bind("RayTracing","Renderer","Legacy",new ConfigDescription("Lighting renderer, followed while playing: PathTracing = the scene path traced; Legacy = VaM's own lighting and shadow maps with this plugin's skin shading, nothing traced.",new AcceptableValueList<string>("Legacy","PathTracing")));
        rtPathBounces = Config.Bind("RayTracing", "PathBounces", 0, new ConfigDescription("Path tracing: how many times light " +
            "bounces off diffuse surfaces (skin, walls, clothing) on its way to the eye. 0 = the lights and the occluded " +
            "sky alone, as the installed route's Ambient 1: the modelling the scene's author lit for; 3 = the room's full " +
            "bounce light. Mirrors, glass, gloss and hair keep their reflections at every value.",
            new AcceptableValueRange<int>(0, 3)));
        rtPathBackground = Config.Bind("RayTracing", "PathBackground", "Far", new ConfigDescription("Path tracing: where what is not the person has no diffuse " +
            "bounce and broad reflections read the sky's cube. Off = nowhere; Far = past 8 m from the camera, whole past 16 m; All = everywhere.",
            new AcceptableValueList<string>("Off", "Far", "All")));
        shadowMapSizeCfg = Config.Bind("Shadows", "ShadowMapSize", 2048, new ConfigDescription("Shadow map size of VaM's spot and point " +
            "lights with shadows. Unity's own size follows the light's share of the screen and can be 256 (about 2 cm a texel at " +
            "2.5 m: the sharp contact shadows show the texels as blocks); its largest at VaM's quality is 2048. 0 = Unity's own.",
            new AcceptableValueList<int>(0, 2048, 4096)));
        sssStrength = Config.Bind("Subsurface", "Strength", 1.0f, new ConfigDescription(
            "Light scattering under the skin: light that falls on skin comes out again a few millimetres further on, red farthest " +
            "(as far as in measured skin): lip edges, nostrils, eyelid rims and shadow edges glow softly red. 1 = full; " +
            "0 = off.", new AcceptableValueRange<float>(0f, 1f)));
        if (!enabled)
        {
            loadError = "disabled ([General] Enabled = false)";
            Logger.LogInfo("VaM skin transmission: disabled in the config; nothing loaded");
            return;
        }
        try
        {
            if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11) throw new Exception("not Direct3D 11");
            // .bin, not .dll: BepInEx reads every .dll under plugins as a .NET assembly
            string dll = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(typeof(VaMSkinTransmission).Assembly.Location), Native + ".bin");
            IntPtr m = LoadLibraryW(dll);
            if (m == IntPtr.Zero) throw new Exception("cannot load " + dll + " (error " + Marshal.GetLastWin32Error() + ")");
            stApiVersion = Bind<IntFn>(m, "ST_ApiVersion");
            stPathBufferBytes=Bind<BufferBytesFn>(m,"ST_PathBufferBytesV1");
            stPathTracing = Bind<SetFn>(m, "ST_SetPathTracingV1");
            stPathSceneActive = Bind<IntFn>(m,"ST_PathSceneActiveV1");
            stPathRenderingActive=Bind<IntFn>(m,"ST_PathRenderingActiveV1");
            stPathRequested=Bind<IntFn>(m,"ST_PathTracingRequestedV1");
            stPathNotice=Bind<ReadFn>(m,"ST_PathNoticeV1");
            RtScene.stObserved=Bind<RtScene.ObservedFn>(m,"ST_GetPathObservedV1");
            stSetOverallTranslucency = Bind<FloatFn>(m, "ST_SetOverallTranslucency");
            stSetTranslucency = Bind<Float3Fn>(m, "ST_SetTranslucency");
            stSetRegional = Bind<SetFn>(m, "ST_SetRegional");
            if (stApiVersion() != ApiVersion) throw new Exception("native API " + stApiVersion() + ", plugin expects " + ApiVersion);
            stInstalled = Bind<IntFn>(m, "ST_Installed");
            stRenderEventFunc = Bind<PtrFn>(m, "ST_RenderEventFunc");
            stRenderEventAndDataFunc = Bind<PtrFn>(m, "ST_RenderEventAndDataFunc");
            stRtAvailable = Bind<IntFn>(m, "ST_RtAvailable");
            stSetRtTest = Bind<RtTestFn>(m, "ST_SetRtTest");
            stSetPathBounces = Bind<SetFn>(m, "ST_SetPathBounces");
            stSetPathAuthor = Bind<PathAuthorFn>(m, "ST_SetPathAuthor");
            stSetRtDetail = Bind<FloatFn>(m, "ST_SetRtDetail");
            stSetEnvSpecNormal = Bind<SetFn>(m, "ST_SetEnvSpecNormal");
            stSetSkinGrazing = Bind<SetFn>(m, "ST_SetSkinGrazing");
            stSetSkinFuzz = Bind<FloatFn>(m, "ST_SetSkinFuzz");
            stSetSpecDisc = Bind<SetFn>(m, "ST_SetSpecDisc");
            stSetEnabled = Bind<SetFn>(m, "ST_SetEnabled");
            stInit = Bind<InitFn>(m, "ST_Init");
            stRead = Bind<ReadFn>(m, "ST_Read");
            stLastError = Bind<ReadFn>(m, "ST_LastError");
            stResetTiming = Bind<VoidFn>(m, "ST_ResetTiming");
            stUnregister = Bind<PtrArgFn>(m, "ST_Unregister");
            stBake = Bind<BakeFn>(m, "ST_Bake");
            stSetParams = Bind<ParamsFn>(m, "ST_SetParams");
            stSetShadow = Bind<ShadowFn>(m, "ST_SetShadow");
            stSetBake = Bind<FloatFn>(m, "ST_SetBake");
            stSetNose = Bind<FloatFn>(m, "ST_SetNose");
            stSetNoseFilter = Bind<NoseFilterFn>(m, "ST_SetNoseFilter");
            stSetSpecular = Bind<SpecFn>(m, "ST_SetSpecular");
            stSetScatter = Bind<ScatFn>(m, "ST_SetScatter");
            stSetSkinDetail = Bind<DetailFn>(m, "ST_SetSkinDetail");
            stSetTiers = Bind<TiersFn>(m, "ST_SetTiers");
            stSetGlossTier = Bind<GlossTierFn>(m, "ST_SetGlossTier");
            stTexStats = Bind<TexStatsFn>(m, "ST_TexStats");
            stTexStatsMasked = Bind<TexStatsMaskedFn>(m, "ST_TexStatsMasked");
            stSetTierRow = Bind<TierRowFn>(m, "ST_SetTierRow");
            stNormalSlope = Bind<TexStatsFn>(m, "ST_NormalSlope");
            stTexStatsResult = Bind<TexResultFn>(m, "ST_TexStatsResult");
            stSetTeeth = Bind<TeethFn>(m, "ST_SetTeeth");
            stSetMouth = Bind<MouthFn>(m, "ST_SetMouth");
            stSetTongueHeight = Bind<TongueHeightFn>(m,"ST_SetTongueHeight");
            stSetSubsurface = Bind<Float2Fn>(m, "ST_SetSubsurface");
            stSetSubsurfaceView = Bind<FloatFn>(m, "ST_SetSubsurfaceView");
            stSetEyes = Bind<EyesFn>(m, "ST_SetEyes");
            stSetEyeSkin = Bind<EyeSkinFn>(m, "ST_SetEyeSkin");
            stSetDiag = Bind<SetFn>(m, "ST_SetDiag");

            Type skin = Type.GetType("DAZSkinV2, Assembly-CSharp", true);
            Type dazMesh = Type.GetType("DAZMesh, Assembly-CSharp", true);
            tCharacter = Type.GetType("DAZCharacter, Assembly-CSharp", true);
            fCharacterIsMale = Field(tCharacter, "isMale");
            fMesh = Field(skin, "mesh");
            fDazMesh = Field(skin, "dazMesh");
            fGpuMaterials = Field(skin, "GPUmaterials");
            pMorphedVerts = Property(dazMesh, "morphedUVVertices");
            pMorphedNormals = Property(dazMesh, "morphedUVNormals");
            pMaterialNames = Property(dazMesh, "materialNames");
            MethodInfo draw = skin.GetMethod("DrawMeshGPU", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, Type.EmptyTypes, null);
            if (draw == null) throw new MissingMethodException("DAZSkinV2", "DrawMeshGPU");

            // now, before any scene: the game creates skin shaders later and they are recognised as they are created
            int r = stInit(Texture2D.whiteTexture.GetNativeTexturePtr());
            if (r != 0) throw new Exception("ST_Init " + r + ": " + NativeError());
            renderEvent = stRenderEventFunc();
            ApplyParams(strength.Value, 0f, 1f, 20f, 2.5f, 1.45f, 0.85f);
            strength.SettingChanged += delegate { ApplyParams(strength.Value, 0f, 1f, 20f, 2.5f, 1.45f, 0.85f); };
            stSetTiers(tierRowsNow, TierRows.Length / 8, TierDefaultRow);
            for (int k = 0; k < FirstOwnRow; k++) rowUsed[k] = true;
            ApplySpecular(specNormalize.Value);
            specNormalize.SettingChanged += delegate { ApplySpecular(specNormalize.Value); };
            ApplyDiffuse(BaseDiffuse);
            stSetOverallTranslucency(0f);   // [Diffuse] OverallScattering removed (11-10): its off step
            ApplySubsurface(sssStrength.Value, sssWidth);
            sssStrength.SettingChanged += delegate { ApplySubsurface(sssStrength.Value, sssWidth); };
            ApplyTeeth(teethLook.Value);
            teethLook.SettingChanged += delegate { ApplyTeeth(teethLook.Value); };
            ApplyEyes(eyesLook.Value);
            eyesLook.SettingChanged += delegate { ApplyEyes(eyesLook.Value); };
            ApplyMouth();
            mouthEnabled.SettingChanged += delegate { ApplyMouth(); };
            mouthStrength.SettingChanged += delegate { ApplyMouth(); };
            tongueMaterial.SettingChanged += delegate { ApplyMouth(); };
            ApplyEnv();
            rtDetail.SettingChanged += delegate { ApplyEnv(); };
            rtRenderer.SettingChanged += delegate { ApplyEnv(); };
            rtPathBounces.SettingChanged += delegate { ApplyEnv(); };
            rtPathBackground.SettingChanged += delegate { ApplyEnv(); };
            shadowMapSizeCfg.SettingChanged += delegate { ApplyEnv(); };
            // The nasal trial has separate approval history; this release installs the mouth.
            stSetNose(0f);
            Harmony harmony = new Harmony("local.vam.skintransmission");
            harmony.Patch(draw, null,
                new HarmonyMethod(typeof(VaMSkinTransmission).GetMethod("SkinDrawn", BindingFlags.Public | BindingFlags.Static)), null, null);
            PatchEyeItems(harmony);
            PatchLights(harmony);
            RtScene.Setup(harmony, stRenderEventAndDataFunc(), shadowLights);
            if (!RtScene.Ready) Logger.LogWarning("VaM skin transmission: the scene for ray tracing unavailable: " + RtScene.Status());
            try { PathCanvasUi.Setup(harmony, Bind<PathCanvasUi.SetFn>(m, "ST_SetPathUiTexturesV1"), Bind<PathCanvasUi.DumpFn>(m, "ST_DumpPathOverlayV1")); }
            catch (Exception e) { Logger.LogWarning("VaM skin transmission: PT canvas UI unavailable: " + e.Message); }
            Camera.onPreCull += PreCull;
            Camera.onPostRender += EyeRendered;
            StartCoroutine(FrameEvents());
            Logger.LogInfo("VaM skin transmission: loaded");
        }
        catch (Exception e)
        {
            loadError = e.Message;
            Logger.LogError("VaM skin transmission: " + e.Message);
        }
    }

    private static void ApplyParams(float s, float wrap, float scale, float maxMM, float r, float g, float b)
    {
        stSetParams(s, wrap, scale, maxMM, r, g, b);
    }

    private static void ApplyMouth()
    {
        if (stSetMouth == null) return;
        SetMouth(mouthEnabled.Value ? mouthStrength.Value : -1f, tongueMaterial.Value ? 1f : 0f);
    }

    private static void ApplyEnv()
    {
        if(stPathTracing!=null)stPathTracing(rtRenderer.Value=="PathTracing"?1:0);
        if (stSetRtDetail != null) stSetRtDetail(rtDetail.Value / 100f);
        if (stSetPathBounces != null) stSetPathBounces(rtPathBounces.Value);
        if (stSetPathAuthor != null && !pathAuthorTest)
        {
            string background = rtPathBackground.Value;
            bool far = background == "Far";
            stSetPathAuthor(background == "Off" ? PathAuthorRules : PathAuthorRules | PathBackgroundRules, PathAuthorAmbient, PathAuthorContact, PathAuthorAmbientRange, PathAuthorEmitter, far ? 8f : 0f, far ? 16f : 0f);
        }
        SetShadowMapSize(shadowMapSizeCfg.Value);
    }

    private static void ApplySpecular(bool normalize)
    {
        stSetSpecular(normalize ? 1 : 0, 2.5f, 1f, 0.028f, 1.2f, 0.75f, 1.3f, 0.85f);
        specOnNow = normalize;
        ApplyDetail();
    }

    // skin detail from the two settings: [Specular] Normalize on = each skin's roughness from its step's row (0.13.0),
    // intensity over the spec map's own mean, the cavity filtered; [Diffuse] on = the diffuse normal filtered, the ambient
    // on it
    private static void ApplyDetail()
    {
        bool spec = specOnNow, diff = diffOnNow;
        stSetSkinDetail(spec ? DetailMedA2 : 0f, DetailStretchLo, DetailStretchHi, spec ? 1 : 0,
                        diff ? DetailFilterMM : 0f, diff ? 1 : 0, spec ? DetailFilterMM : 0f, spec ? 1 : 0);
    }

    // ---- highlight levels (0.13.0 steps, 0.18.0 levels) --------------------------------------------------------------

    private static void LoadTiers()
    {
        levelChosen.Clear();
        try
        {
            if (!System.IO.File.Exists(tierFile)) return;
            foreach (string line in System.IO.File.ReadAllLines(tierFile, Encoding.UTF8))
            {
                int tab = line.IndexOf('\t');
                float t;
                if (tab > 0 && float.TryParse(line.Substring(0, tab), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out t) &&
                    t >= 0f && t <= LevelMax) levelChosen[line.Substring(tab + 1)] = t;
            }
        }
        catch (Exception e)
        {
            Debug.LogWarning("VaM skin transmission: cannot read " + tierFile + ": " + e.Message);
        }
    }

    private static void SaveTiers()
    {
        try
        {
            List<string> lines = new List<string>();
            foreach (KeyValuePair<string, float> kv in levelChosen)
                lines.Add(kv.Value.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "\t" + kv.Key);
            lines.Sort(StringComparer.Ordinal);
            System.IO.File.WriteAllLines(tierFile, lines.ToArray(), new UTF8Encoding(false));
        }
        catch (Exception e)
        {
            Debug.LogWarning("VaM skin transmission: cannot write " + tierFile + ": " + e.Message);
        }
    }

    // The style a skin's author gave its face, read through VaM's own formula (investigations\skin-detail-20261018\
    // tier_match.py, the same): VaM's highlight on a sphere seen head on, one light 50 deg off the view (light 1.2, albedo
    // 0.4, shown in sRGB), at the face gloss map's p50 (cheeks, 0.7) and p90 (T-zone, 0.3), its lobe widened by half the
    // pore-scale slope's variance of its bent normals (n' = 1 / (1/n + (Specular Bumpiness x slope)^2 / 4)); the reading is
    // log2 of the shown highlight's spread (std) over the sphere: a broad even sheen reads matte, a highlight that stands
    // out glossy. Eight appearances read -0.64, 0.18, 0.38, 0.39, 0.88, 0.91, 0.91 and 1.54 (ranked as their VaM
    // pictures).
    private static float SkinGloss(Skin s)
    {
        const int Res = 101;
        float la = 50f * Mathf.Deg2Rad;
        Vector3 L = new Vector3(-Mathf.Sin(la) * 0.8f, Mathf.Sin(la) * 0.6f, Mathf.Cos(la)).normalized, H = (L + Vector3.forward).normalized;
        float bs = s.specBump * s.slope, F = s.fresnel, S0 = Mathf.Clamp01(s.specMean + s.specOffset) * s.specColour;
        float[] n = new float[2];
        for (int w = 0; w < 2; w++)
        {
            float g = Mathf.Clamp01((w == 0 ? s.glossP50 : s.glossP90) + s.glossOffset), u = (1f - g) * (1f - g);
            float n0 = Mathf.Pow(2f, Mathf.Lerp(s.shininess, 1f, u));
            n[w] = 1f / (1f / n0 + bs * bs / 4f);
        }
        double[] sum = new double[2], sq = new double[2];
        int count = 0;
        for (int j = 0; j < Res; j++)
            for (int i = 0; i < Res; i++)
            {
                float x = -1f + 2f * i / (Res - 1), y = -1f + 2f * j / (Res - 1);
                if (x * x + y * y >= 0.995f) continue;
                Vector3 N = new Vector3(x, y, Mathf.Sqrt(1f - x * x - y * y));
                float nl = Mathf.Clamp01(Vector3.Dot(N, L)), nv = Mathf.Clamp01(N.z), nh = Mathf.Clamp01(Vector3.Dot(N, H));
                float c = 1f - nv, fr = (1f - F) * (1f - F) + 2f * F * (1f - F) * c + F * F * c * c * c;
                float xs = s.specInt * (0.05f + 0.95f * fr);
                float k = Mathf.Sqrt(Mathf.Max(xs, 0f)) * (1f - 0.922444f) + xs * 0.922444f;
                float diffuse = 1.2f * 0.4f * nl, base0 = Shown(diffuse);
                for (int w = 0; w < 2; w++)
                {
                    float hl = 1.2f * S0 * k * (n[w] + 2f) / (2f * Mathf.PI) * 0.5f * Mathf.Pow(nh, n[w]) * Mathf.Min(10f * nl, 1f);
                    double d = Shown(diffuse + hl) - base0;
                    sum[w] += d;
                    sq[w] += d * d;
                }
                count++;
            }
        double spread = 0;
        for (int w = 0; w < 2; w++)
        {
            double m = sum[w] / count;
            spread += (w == 0 ? 0.7 : 0.3) * Math.Sqrt(Math.Max(sq[w] / count - m * m, 0.0));
        }
        return spread > 1e-6 ? (float)Math.Log(spread, 2.0) : -20f;
    }

    // linear light as shown, 0 .. 255 (sRGB)
    private static float Shown(float lin)
    {
        lin = Mathf.Min(lin, 1f);
        return (lin <= 0.0031308f ? 12.92f * lin : 1.055f * Mathf.Pow(lin, 1f / 2.4f) - 0.055f) * 255f;
    }

    // a skin's starting level from its reading (two decimals)
    private static float LevelOf(float gloss)
    {
        return Mathf.Round(Mathf.Clamp(1f + (gloss - LevelFrom) / LevelPerStep, 1f, LevelMax) * 100f) / 100f;
    }

    // A level's row (8 floats as ST_SetTiers takes them): between the steps' rows (face rows 0-3, body 4-7) alpha^2, R,
    // floor and ceiling geometric, the rest linear; below step 1 step 1's row, its gain going down to 0 at level 0.
    private static void LevelRow(float level, bool body, float[] o)
    {
        level = Mathf.Clamp(level, 0f, LevelMax);
        int i = Mathf.Clamp(Mathf.FloorToInt(level), 1, TierCount - 1);
        float f = Mathf.Clamp01(level - i);
        int a = ((body ? TierCount : 0) + i - 1) * 8, b = a + 8;
        for (int k = 0; k < 8; k++)
        {
            float x = tierRowsNow[a + k], y = tierRowsNow[b + k];
            o[k] = k < 4 && x > 0f && y > 0f ? x * Mathf.Pow(y / x, f) : Mathf.Lerp(x, y, f);
        }
        if (level < 1f) o[5] *= level;
    }

    private static float MatFloat(Material m, string name, float fallback)
    {
        return m.HasProperty(name) ? m.GetFloat(name) : fallback;
    }

    private static Texture MatTex(Material m, string name)
    {
        return RtScene.Declared(m, name) ? m.GetTexture(name) : null;   // 0.23.2: carried-over properties log errors
    }

    // the character (its atom's uid) a skin belongs to
    private static string AtomUid(Component c)
    {
        try
        {
            if (tAtom == null)
            {
                tAtom = Type.GetType("Atom, Assembly-CSharp", true);
                mAtomUid = (MemberInfo)tAtom.GetProperty("uid", BindingFlags.Public | BindingFlags.Instance) ??
                           tAtom.GetField("uid", BindingFlags.Public | BindingFlags.Instance);
            }
            Component a = c.GetComponentInParent(tAtom);
            if (a == null) return c.name;
            object v = mAtomUid is PropertyInfo ? ((PropertyInfo)mAtomUid).GetValue(a, null) : mAtomUid is FieldInfo ? ((FieldInfo)mAtomUid).GetValue(a) : null;
            return v as string ?? a.name;
        }
        catch (Exception)
        {
            return c.name;
        }
    }

    // A skin's face textures (a new preset or texture: its stats asked for, its starting step again), its step once known,
    // and every gloss map of it on its step's row (the face's maps the face rows, the others the body rows).
    private static bool HeightTexture(Texture texture)
    {
        if(texture==null)return false;
        RenderTexture rt=null,old=RenderTexture.active;Texture2D sample=null;
        try {
            rt=RenderTexture.GetTemporary(32,32,0,RenderTextureFormat.ARGB32,RenderTextureReadWrite.Linear);
            Graphics.Blit(texture,rt);RenderTexture.active=rt;
            sample=new Texture2D(32,32,TextureFormat.RGBA32,false,true);
            sample.ReadPixels(new Rect(0,0,32,32),0,0);sample.Apply();
            Color[] pixels=sample.GetPixels();int grey=0;float lo=1,hi=0;
            foreach(Color c in pixels) {
                if(Mathf.Abs(c.r-c.g)<.025f&&Mathf.Abs(c.r-c.b)<.025f)grey++;
                lo=Mathf.Min(lo,c.r);hi=Mathf.Max(hi,c.r);
            }
            return grey>pixels.Length*.97f&&hi-lo>.025f;
        } finally {
            RenderTexture.active=old;if(rt!=null)RenderTexture.ReleaseTemporary(rt);if(sample!=null)UnityEngine.Object.Destroy(sample);
        }
    }

    private static void CheckTier(Skin s)
    {
        object dm = fDazMesh.GetValue(s.component);
        string[] names = dm != null ? pMaterialNames.GetValue(dm, null) as string[] : null;
        Material[] mats = fGpuMaterials.GetValue(s.component) as Material[];
        if (names == null || mats == null) return;
        if (!ReferenceEquals(names, s.tierNames))
        {
            s.tierNames = names; s.tierFace = -1; s.tierTongues.Clear();
            for (int k = 0; k < names.Length; k++)
            {
                if (names[k] == null) continue;
                if (TongueName.IsMatch(names[k])) s.tierTongues.Add(k);
                if (s.tierFace < 0 && FaceName.IsMatch(names[k])) s.tierFace = k;
            }
        }
        for(int t=0;t<s.tierTongues.Count;t++) {
            int k=s.tierTongues[t];if(k>=mats.Length)continue;
            string before = s.tongueSurface.State;
            if (tongueSurfaceOn) s.tongueSurface.Update(mats[k], fMesh.GetValue(s.component) as Mesh, k);
            else s.tongueSurface.Restore();
            if (before != s.tongueSurface.State && instance != null) instance.Logger.LogInfo("Tongue material: " + s.tongueSurface.State);
            Texture normal=MatTex(mats[k],"_BumpMap");
            if(normal!=s.tongueNormal) {
                s.tongueNormal=normal;
                bool height=HeightTexture(normal);
                stSetTongueHeight(s.id,height?1:0);
                if(instance!=null)instance.Logger.LogInfo("Tongue surface: "+(height?"height gradients":"normal map")+" / "+(normal!=null?normal.name:"none"));
            }
        }
        Material face = s.tierFace >= 0 && s.tierFace < mats.Length ? mats[s.tierFace] : null;
        if (face == null) return;
        // Read the existing face baseline; never write the material or the saved appearance.
        // The same registered-material check already follows skin/preset changes.
        float faceFilter = Mathf.Clamp01(MatFloat(face, "_IBLFilter", 0f));
        if (faceFilter != s.noseFaceFilter) {
            s.noseFaceFilter = faceFilter;
            // Keep the 0.13 nasal baseline; the mouth release does not enable the nasal trial.
        }
        Texture fg = MatTex(face, "_GlossTex"), fs = MatTex(face, "_SpecTex"), fm = MatTex(face, "_MainTex"), fn = MatTex(face, "_BumpMap");
        if (fg != s.faceGloss || fs != s.faceSpec || fm != s.faceMain || fn != s.faceNormal || s.uid.Length == 0)
        {
            s.faceGloss = fg; s.faceSpec = fs; s.faceMain = fm; s.faceNormal = fn;
            s.uid = AtomUid(s.component);
            string a = fm != null ? fm.name : "", b = fg != null ? fg.name : "";
            s.key = a.Length > 0 || b.Length > 0 ? a + "|" + b : "skin:" + s.uid;
            s.glossQuery = fg != null ? stTexStats(fg.GetNativeTexturePtr()) : 0;
            s.specQuery = fs != null ? stTexStats(fs.GetNativeTexturePtr()) : 0;
            s.normalQuery = fn != null ? stNormalSlope(fn.GetNativeTexturePtr()) : 0;
            s.shininess = MatFloat(face, "_Shininess", 7f);
            s.glossOffset = MatFloat(face, "_GlossOffset", 0f);
            s.specInt = MatFloat(face, "_SpecInt", 1f);
            s.specOffset = MatFloat(face, "_SpecOffset", 0f);
            s.fresnel = MatFloat(face, "_Fresnel", 0f);
            s.specBump = MatFloat(face, "_SpecularBumpiness", 1f);
            Color sc = face.HasProperty("_SpecColor") ? face.GetColor("_SpecColor").linear : Color.white;   // as the shader gets it
            s.specColour = 0.2126f * sc.r + 0.7152f * sc.g + 0.0722f * sc.b;
            s.autoLevel = -1f;
            s.gloss = float.NaN;
        }
        if (s.autoLevel < 0f)
        {
            float[] g = new float[4], sp = new float[4], nm = new float[4];
            int ga = s.glossQuery > 0 ? stTexStatsResult(s.glossQuery, g) : -1, sa = s.specQuery > 0 ? stTexStatsResult(s.specQuery, sp) : -1;
            int na = s.normalQuery > 0 ? stTexStatsResult(s.normalQuery, nm) : -2;
            if (ga == 0 || sa == 0 || na == 0) return;   // not read yet
            s.glossP50 = ga == 1 ? g[1] : 0f;
            s.glossP90 = ga == 1 ? g[2] : 0f;
            s.specMean = sa == 1 ? sp[3] : 0.25f;
            s.slope = na == 1 ? nm[3] : na == -2 ? 0f : SlopeUnknown;   // no normal map: no bent normals
            s.gloss = SkinGloss(s);
            s.autoLevel = LevelOf(s.gloss);
        }
        float chosen;
        s.level = forcedLevel >= 0f ? forcedLevel : levelChosen.TryGetValue(s.key, out chosen) ? chosen : s.autoLevel;
        UpdateGlossMaps(s, names, mats, fg);
    }

    // 0.18.0: each gloss map of the skin with a row of its own at the skin's level: its skin texels' stats asked for once
    // (the UV coverage of the materials sampling it: the face map's Face material, a body map's skin materials, or all of
    // them when none is skin), the row written again when the level or the stats change. Maps the skin no longer uses
    // give their rows back.
    private static readonly float[] rowValues = new float[12];

    private static readonly List<Texture> glossSeen = new List<Texture>();
    private static void UpdateGlossMaps(Skin s, string[] names, Material[] mats, Texture faceGloss)
    {
        // 0.18.1: only a skin that is drawn binds its gloss maps. VaM keeps a character's previous skin (inactive) with
        // the same cached textures and the previous scene's material values; binding last, its level won the shared
        // textures (seen in a test: three appearances drawn on the inactive skin's rows). It binds again once
        // it is drawn.
        Behaviour drawn = s.component as Behaviour;
        if (drawn == null || !drawn.isActiveAndEnabled)
        {
            foreach (GlossMap g0 in s.maps.Values) g0.bound = -2;
            return;
        }
        List<Texture> seen = glossSeen;   // 0.23.1: one list, not a new one at each check
        seen.Clear();
        for (int k = 0; k < mats.Length; k++)
        {
            Texture t = MatTex(mats[k], "_GlossTex");
            if (t == null) continue;
            if (seen.Contains(t)) continue;
            seen.Add(t);
            GlossMap g;
            if (!s.maps.TryGetValue(t, out g))
            {
                g = NewGlossMap(s, names, mats, t, t == faceGloss);
                s.maps[t] = g;
            }
            if (g.p50 < 0f && !g.failed) ReadGlossMap(g);
            int step = (g.face ? 0 : TierCount) + Mathf.Clamp(Mathf.RoundToInt(s.level), 1, TierCount) - 1;
            int want = ownRows && g.row >= FirstOwnRow ? g.row : step;
            if (g.bound != want)
            {
                stSetGlossTier(t.GetNativeTexturePtr(), want);
                g.bound = want;
            }
            if (g.written == s.level || want != g.row) continue;
            LevelRow(s.level, !g.face, rowValues);
            bool own = g.p50 >= 0f;
            if (own)
            {
                rowValues[8] = g.p50; rowValues[9] = g.spread;
                rowValues[10] = Mathf.Max(3f * Mathf.Clamp01(g.specMean + g.specOffset), 0.15f);
                rowValues[11] = 1f;
            }
            else
            {
                rowValues[8] = rowValues[9] = rowValues[10] = rowValues[11] = 0f;
                rowValues[5] = Mathf.Max(rowValues[5], 1e-6f);   // without its own stats a gain of 0 would read as 1
            }
            stSetTierRow(g.row, rowValues);
            // a map still being read is written again once read
            if (own || g.failed) g.written = s.level;
        }
        if (s.maps.Count > seen.Count)
        {
            List<Texture> drop = new List<Texture>();
            foreach (KeyValuePair<Texture, GlossMap> kv in s.maps)
                if (!seen.Contains(kv.Key)) drop.Add(kv.Key);
            foreach (Texture t in drop)
            {
                FreeGlossMap(s.maps[t], t);
                s.maps.Remove(t);
            }
        }
    }

    private static GlossMap NewGlossMap(Skin s, string[] names, Material[] mats, Texture t, bool face)
    {
        GlossMap g = new GlossMap();
        g.face = face;
        g.name = t.name;
        List<int> all = new List<int>(), chosen = new List<int>();
        for (int k = 0; k < mats.Length; k++)
            if (MatTex(mats[k], "_GlossTex") == t) all.Add(k);
        foreach (int k in all)
        {
            string n = names != null && k < names.Length && names[k] != null ? names[k] : "";
            if (face ? FaceName.IsMatch(n) : n.Length > 0 && !NotReceiver.IsMatch(n)) chosen.Add(k);
        }
        if (chosen.Count == 0) chosen = all;
        Material m = mats[chosen[0]];
        g.spec = MatTex(m, "_SpecTex");
        g.specOffset = MatFloat(m, "_SpecOffset", 0f);
        for (int r = FirstOwnRow; r < RowCount && g.row < 0; r++)
            if (!rowUsed[r]) { rowUsed[r] = true; g.row = r; }
        byte[] mask = UvMask(s, chosen, m);
        if (mask != null)
        {
            g.glossQuery = stTexStatsMasked(t.GetNativeTexturePtr(), mask, MaskSize, MaskSize);
            g.specQuery = g.spec != null ? stTexStatsMasked(g.spec.GetNativeTexturePtr(), mask, MaskSize, MaskSize) : 0;
        }
        if (g.glossQuery <= 0) g.failed = true;
        return g;
    }

    private static void ReadGlossMap(GlossMap g)
    {
        float[] v = new float[4];
        int ga = stTexStatsResult(g.glossQuery, v);
        if (ga == 0) return;
        if (ga < 0) { g.failed = true; return; }
        float p50 = v[1], spread = Mathf.Max(v[2] - v[1], 0f);
        float specMean = 0.25f;   // no spec map: VaM's default grey
        if (g.specQuery > 0)
        {
            int sa = stTexStatsResult(g.specQuery, v);
            if (sa == 0) return;
            if (sa < 0) { g.failed = true; return; }
            specMean = v[3];
        }
        g.spread = spread;
        g.specMean = specMean;
        g.p50 = p50;
    }

    // a gloss map's row back; the texture (when it is still there and no other skin's map has it) back on the steps' rows
    private static void FreeGlossMap(GlossMap g, Texture t)
    {
        if (g.row >= FirstOwnRow) rowUsed[g.row] = false;
        GlossMap other = null;
        foreach (Skin o in skins.Values)
            if (o.maps.TryGetValue(t, out other) && other != g) break;
            else other = null;
        if (t == null) return;
        if (other != null) stSetGlossTier(t.GetNativeTexturePtr(), other.bound >= 0 ? other.bound : -1);
        else stSetGlossTier(t.GetNativeTexturePtr(), -1);
    }

    // The UV coverage of these materials of the skin's mesh (MaskSize^2, rows bottom up as Unity's UV v; the materials'
    // _MainTex scale and offset): every texel whose centre a triangle covers, and the texels of its corners.
    private static byte[] UvMask(Skin s, List<int> subs, Material m)
    {
        Mesh mesh = fMesh.GetValue(s.component) as Mesh;
        if (mesh == null) return null;
        Vector2[] uv = mesh.uv;
        if (uv == null || uv.Length != mesh.vertexCount) return null;
        Vector2 scale = Vector2.one, offset = Vector2.zero;
        if (m != null && m.HasProperty("_MainTex")) { scale = m.GetTextureScale("_MainTex"); offset = m.GetTextureOffset("_MainTex"); }
        byte[] mask = new byte[MaskSize * MaskSize];
        int set = 0;
        foreach (int k in subs)
        {
            if (k >= mesh.subMeshCount || mesh.GetTopology(k) != MeshTopology.Triangles) continue;
            int[] tri = mesh.GetTriangles(k);
            for (int i = 0; i + 2 < tri.Length; i += 3)
            {
                Vector2 a = Vector2.Scale(uv[tri[i]], scale) + offset, b = Vector2.Scale(uv[tri[i + 1]], scale) + offset,
                        c = Vector2.Scale(uv[tri[i + 2]], scale) + offset;
                a *= MaskSize; b *= MaskSize; c *= MaskSize;
                int x0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(a.x, Mathf.Min(b.x, c.x)))), x1 = Mathf.Min(MaskSize - 1, Mathf.FloorToInt(Mathf.Max(a.x, Mathf.Max(b.x, c.x))));
                int y0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(a.y, Mathf.Min(b.y, c.y)))), y1 = Mathf.Min(MaskSize - 1, Mathf.FloorToInt(Mathf.Max(a.y, Mathf.Max(b.y, c.y))));
                float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++)
                    {
                        float px = x + 0.5f, py = y + 0.5f;
                        float w0 = (b.x - px) * (c.y - py) - (b.y - py) * (c.x - px);
                        float w1 = (c.x - px) * (a.y - py) - (c.y - py) * (a.x - px);
                        float w2 = (a.x - px) * (b.y - py) - (a.y - py) * (b.x - px);
                        bool inside = area > 0f ? w0 >= 0f && w1 >= 0f && w2 >= 0f : w0 <= 0f && w1 <= 0f && w2 <= 0f;
                        if (inside && mask[y * MaskSize + x] == 0) { mask[y * MaskSize + x] = 1; set++; }
                    }
                foreach (Vector2 q in new Vector2[] { a, b, c })
                {
                    int x = Mathf.FloorToInt(q.x), y = Mathf.FloorToInt(q.y);
                    if (x >= 0 && y >= 0 && x < MaskSize && y < MaskSize && mask[y * MaskSize + x] == 0) { mask[y * MaskSize + x] = 1; set++; }
                }
            }
        }
        return set >= 64 ? mask : null;
    }

    // the diffuse look
    private static bool ApplyDiffuse(string name)
    {
        int i = Array.IndexOf(DiffuseNames, name);
        if (i < 0) return false;
        if (i == 0) stSetScatter(0, 0.863f, 0.863f, 0.863f, 2.964f, 2.964f, 2.964f);
        else stSetScatter(1, DiffuseA[i][0], DiffuseA[i][1], DiffuseA[i][2], DiffuseP[i][0], DiffuseP[i][1], DiffuseP[i][2]);
        diffOnNow = i > 0;
        ApplyDetail();
        return true;
    }

    private static void ApplySubsurface(float s, float width)
    {
        stSetSubsurface(s, width);
    }

    private static bool ApplyTeeth(string name)
    {
        int i = Array.IndexOf(TeethNames, name);
        if (i < 0) return false;
        stSetTeeth(i == 0 ? 0 : 1, 1f, 1f, TeethBack[i], TeethBackStart[i], TeethGum[i], TeethGumPower[i]);
        return true;
    }

    private static bool ApplyEyes(string name)
    {
        int i = Array.IndexOf(EyesNames, name);
        if (i < 0) return false;
        stSetEyes(i == 0 ? 0 : 1, EyeUpper[i], EyeUpperWidth[i], EyeLower[i], EyeLowerWidth[i], i == 0 ? 0 : 1, EyeCatch[i], 0.05f, 0.025f, 0.6f);
        return true;
    }

    // Eye overlay items: every clothing wrap that switches on is noted (no scene scans), and a second after any clothing
    // item switches on or off the wraps are looked at again. Without these members of VaM's types the eyes simply never
    // step aside (Status: eye_aside_error).
    private static void PatchEyeItems(Harmony harmony)
    {
        try
        {
            Type wrap = Type.GetType("DAZSkinWrap, Assembly-CSharp", true);
            Type item = Type.GetType("DAZDynamicItem, Assembly-CSharp", true);
            Type dazMesh = Type.GetType("DAZMesh, Assembly-CSharp", true);
            fWrapSkin = Field(wrap, "skin");
            fWrapDazMesh = Field(wrap, "dazMesh");
            fWrapMaterials = Field(wrap, "GPUmaterials");
            fWrapEnabled = Field(wrap, "materialsEnabled");
            fUvMappedMesh = Field(dazMesh, "_uvMappedMesh");
            pUVVertices = Property(dazMesh, "UVVertices");
            BindingFlags all = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
            MethodInfo wrapOn = wrap.GetMethod("OnEnable", all, null, Type.EmptyTypes, null);
            MethodInfo itemOn = item.GetMethod("OnEnable", all, null, Type.EmptyTypes, null);
            MethodInfo itemOff = item.GetMethod("OnDisable", all, null, Type.EmptyTypes, null);
            if (wrapOn == null || itemOn == null || itemOff == null) throw new MissingMethodException("DAZSkinWrap / DAZDynamicItem", "OnEnable / OnDisable");
            harmony.Patch(wrapOn, null, new HarmonyMethod(typeof(VaMSkinTransmission).GetMethod("WrapEnabled", BindingFlags.Public | BindingFlags.Static)), null, null);
            HarmonyMethod toggled = new HarmonyMethod(typeof(VaMSkinTransmission).GetMethod("ItemToggled", BindingFlags.Public | BindingFlags.Static));
            harmony.Patch(itemOn, null, toggled, null, null);
            harmony.Patch(itemOff, null, toggled, null, null);
        }
        catch (Exception e)
        {
            eyeAsideError = e.Message;
        }
    }

    // 0.19.1 shadow map size: VaM's spot and point lights with shadows get [Shadows] ShadowMapSize (Light.
    // shadowCustomResolution). Unity's own size follows the light's share of the screen: 256 for a spot seen small (~2 cm
    // a texel at 2.5 m), which the contact-sharp penumbra draws as blocks; at VaM's Medium quality its largest is 2048.
    // Every light is noted when VaM sets it up or changes its type, shadows or resolution (Harmony postfixes on
    // AdjustLightV2; no scene scans); a change of the setting goes over the noted ones. 0 = Unity's own.
    private static FieldInfo fAdjustLight;
    private static readonly List<Light> shadowLights = new List<Light>(), raisedLights = new List<Light>();
    private static int shadowMapSize;
    private static string shadowMapError;

    private static void PatchLights(Harmony harmony)
    {
        try
        {
            Type adjust = Type.GetType("AdjustLightV2, Assembly-CSharp", true);
            fAdjustLight = Field(adjust, "_light");
            BindingFlags all = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
            HarmonyMethod changed = new HarmonyMethod(typeof(VaMSkinTransmission).GetMethod("LightChanged", BindingFlags.Public | BindingFlags.Static));
            foreach (string name in new[] { "Init", "SetShadowResolution", "SetLightType", "SyncShadowsOn" })
            {
                MethodInfo m = adjust.GetMethod(name, all);
                if (m == null) throw new MissingMethodException("AdjustLightV2", name);
                harmony.Patch(m, null, changed, null, null);
            }
        }
        catch (Exception e)
        {
            shadowMapError = e.Message;
        }
    }

    // Harmony postfix: VaM set up or changed a light
    public static void LightChanged(object __instance)
    {
        Light l = fAdjustLight != null ? fAdjustLight.GetValue(__instance) as Light : null;
        if (l == null) return;
        if (!shadowLights.Contains(l)) shadowLights.Add(l);
        ApplyShadowMapSize(l);
    }

    private static void ApplyShadowMapSize(Light l)
    {
        if (shadowMapSize > 0 && (l.type == LightType.Spot || l.type == LightType.Point) && l.shadows != LightShadows.None)
        {
            l.shadowCustomResolution = shadowMapSize;
            if (!raisedLights.Contains(l)) raisedLights.Add(l);
        }
        else if (raisedLights.Remove(l)) l.shadowCustomResolution = -1;
    }

    private static void SetShadowMapSize(int size)
    {
        shadowMapSize = size;
        shadowLights.RemoveAll(l => l == null);
        raisedLights.RemoveAll(l => l == null);
        foreach (Light l in shadowLights) ApplyShadowMapSize(l);
    }

    // Harmony postfix: a clothing wrap switched on
    public static void WrapEnabled(object __instance)
    {
        Component c = __instance as Component;
        if (c == null) return;
        if (!wraps.Contains(c)) wraps.Add(c);
        eyesDirtyAt = Time.realtimeSinceStartup;
    }

    // Harmony postfix: a clothing (or hair) item switched on or off
    public static void ItemToggled()
    {
        eyesDirtyAt = Time.realtimeSinceStartup;
    }

    // Per character: its active clothing whose material sits on the eyeballs (at least 80% of its vertices within 0.85 to
    // 1.25 eyeball radii of an eye's centre, in the base mesh's space) is an eye overlay: shiny (specular intensity x colour
    // at least 1.5; VaM's eye reflection is 0.24) = its own catchlight, else dark and visible (diffuse below 0.25, alpha
    // adjust above -0.6) = its own eyelid shadow. Other materials there (coloured lenses) change nothing.
    private static void ClassifyEyeItems()
    {
        eyesDirtyAt = -1f;
        for (int i = wraps.Count - 1; i >= 0; i--)
            if (wraps[i] == null) wraps.RemoveAt(i);
        foreach (Skin s in skins.Values)
        {
            if (s.component == null || s.eyeRadius <= 0f) continue;
            int allow = 3;
            StringBuilder found = new StringBuilder();
            for (int w = 0; w < wraps.Count; w++)
            {
                try { allow &= ~EyeItem(wraps[w], s, found); }
                catch (Exception e) { found.Append(found.Length > 0 ? " " : "").Append("error:").Append(e.Message); }
            }
            s.eyeItems = found.ToString();
            if (allow != s.eyeAllow)
            {
                s.eyeAllow = allow;
                stSetEyeSkin(s.id, allow);
            }
        }
    }

    // what one wrap does to the eyes of skin s (bit 0 its own eyelid shadow, bit 1 its own catchlight)
    private static int EyeItem(Component wrap, Skin s, StringBuilder found)
    {
        Behaviour b = wrap as Behaviour;
        if (b == null || !b.isActiveAndEnabled) return 0;
        // These raster effects do not draw in PT; they must not disable PT's own eyelid band.
        if (RtScene.PtReplacesEnhancedEyes(wrap)) return 0;
        Component skin = fWrapSkin.GetValue(wrap) as Component;
        if (skin == null || skin.GetInstanceID() != s.id) return 0;
        object dm = fWrapDazMesh.GetValue(wrap);
        Mesh mesh = dm != null ? fUvMappedMesh.GetValue(dm) as Mesh : null;
        Vector3[] v = dm != null ? pUVVertices.GetValue(dm, null) as Vector3[] : null;
        if (mesh == null || v == null || v.Length != mesh.vertexCount) return 0;
        // eye items are small and at the eyes: a quick look at the whole mesh first
        float r = s.eyeRadius;
        Bounds bb = mesh.bounds;
        Vector3 size = bb.size;
        if (size.x > 0.14f || size.y > 0.08f || size.z > 0.08f) return 0;
        if (bb.SqrDistance(s.eyeCentre[0]) > 1.6f * r * r && bb.SqrDistance(s.eyeCentre[1]) > 1.6f * r * r) return 0;
        // 0.22.0: every small item at the eyes (makeup on the lids and the eyeball, lenses, lashes) casts no ray-traced
        // shadow: it would darken the eyeball under its own shading (an eye shadow layer half on the lids blackened it)
        RtScene.EyeOverlay(wrap);
        Material[] mats = fWrapMaterials.GetValue(wrap) as Material[];
        bool[] on = fWrapEnabled.GetValue(wrap) as bool[];
        int result = 0;
        for (int k = 0; k < mesh.subMeshCount; k++)
        {
            if (on != null && k < on.Length && !on[k]) continue;
            Material m = mats != null && k < mats.Length ? mats[k] : null;
            if (m == null) continue;
            int[] tri = mesh.GetTriangles(k);
            int near = 0;
            for (int i = 0; i < tri.Length; i++)
            {
                Vector3 p = v[tri[i]];
                float d = Mathf.Min((p - s.eyeCentre[0]).magnitude, (p - s.eyeCentre[1]).magnitude) / r;
                if (d >= 0.85f && d <= 1.25f) near++;
            }
            if (tri.Length < 36 || near < 0.8f * tri.Length) continue;
            Color spec = m.HasProperty("_SpecColor") ? m.GetColor("_SpecColor") : Color.black;
            float specInt = m.HasProperty("_SpecInt") ? m.GetFloat("_SpecInt") : 0f;
            Color diffuse = m.HasProperty("_Color") ? m.GetColor("_Color") : Color.white;
            float alpha = m.HasProperty("_AlphaAdjust") ? m.GetFloat("_AlphaAdjust") : 0f;
            string kind = null;
            if (specInt * Mathf.Max(spec.r, Mathf.Max(spec.g, spec.b)) >= 1.5f)
            {
                result |= 2;
                kind = "reflection";
            }
            else if (Mathf.Max(diffuse.r, Mathf.Max(diffuse.g, diffuse.b)) < 0.25f && alpha > -0.6f)
            {
                result |= 1;
                kind = "shadow";
            }
            if (kind != null)
                found.Append(found.Length > 0 ? " " : "").Append(wrap.name.Replace("\"", "'")).Append(":").Append(m.name.Replace("\"", "'")).Append(":").Append(kind);
        }
        return result;
    }

    // the eyeballs from the base mesh's sclera: per side its bounding box (the sclera is the sphere without its front
    // cap: the box's x and y middle is the centre, its width 2 radii, its back 1 radius behind the centre)
    private static void FindEyes(Skin s, object dm, Mesh mesh, string[] names)
    {
        s.eyeRadius = 0f;
        Vector3[] v = dm != null && pUVVertices != null ? pUVVertices.GetValue(dm, null) as Vector3[] : null;
        if (v == null || v.Length != mesh.vertexCount || names == null) return;
        List<int> sclera = new List<int>();
        for (int k = 0; k < mesh.subMeshCount && k < names.Length; k++)
            if (names[k] != null && ScleraName.IsMatch(names[k])) sclera.AddRange(mesh.GetTriangles(k));
        if (sclera.Count < 96) return;
        float mx = 0f;
        for (int i = 0; i < sclera.Count; i++) mx += v[sclera[i]].x;
        mx /= sclera.Count;
        Vector3[] lo = { Vector3.one * 1e9f, Vector3.one * 1e9f }, hi = { Vector3.one * -1e9f, Vector3.one * -1e9f };
        for (int i = 0; i < sclera.Count; i++)
        {
            Vector3 p = v[sclera[i]];
            int e = p.x >= mx ? 0 : 1;
            lo[e] = Vector3.Min(lo[e], p);
            hi[e] = Vector3.Max(hi[e], p);
        }
        float r = 0.25f * ((hi[0].x - lo[0].x) + (hi[1].x - lo[1].x));
        if (!(r > 0.002f && r < 0.05f)) return;
        for (int e = 0; e < 2; e++)
            s.eyeCentre[e] = new Vector3(0.5f * (lo[e].x + hi[e].x), 0.5f * (lo[e].y + hi[e].y), lo[e].z + r);
        s.eyeRadius = r;
        eyesDirtyAt = Time.realtimeSinceStartup;
    }

    private void OnDestroy()
    {
        RtScene.RestorePathScene();
        if(stPathTracing!=null)stPathTracing(0);
        Camera.onPreCull -= PreCull;
        Camera.onPostRender -= EyeRendered;
        foreach (Hooked h in hooked)
        {
            try
            {
                if (h.camera == null) continue;
                h.camera.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque, h.begin);
                h.camera.RemoveCommandBuffer(CameraEvent.AfterForwardOpaque, h.end);
            }
            catch (Exception) { }
        }
        hooked.Clear();
        foreach (Skin skin in skins.Values) skin.tongueSurface.Restore();
        TongueSurface.Dispose();
        if (stSetMouth != null) stSetMouth(-1f, 0f);
        SetShadowMapSize(0);
        if (stSetEnabled != null) stSetEnabled(0);
        if (stSetSpecular != null) stSetSpecular(0, 2.5f, 1f, 0.028f, 1f, 0.75f, 1.3f, 0.85f);
        if (stSetScatter != null) stSetScatter(0, 0.863f, 0.863f, 0.863f, 2.964f, 2.964f, 2.964f);
        if (stSetOverallTranslucency != null) stSetOverallTranslucency(0f);
        if (stSetRegional != null) stSetRegional(0);
        if (stSetSkinDetail != null) stSetSkinDetail(0f, 0.05f, 4f, 0, 0f, 0, 0f, 0);
        if (stSetTeeth != null) stSetTeeth(0, 1f, 1f, 0f, 0f, 0f, 2f);
        if (stSetSubsurface != null) stSetSubsurface(0f, 1f);
        if (stSetEyes != null) stSetEyes(0, 0f, 0.146f, 0f, 0.066f, 0, 0f, 0.05f, 0.025f, 0.6f);
    }

    // Harmony postfix: the skin's own per-frame draw. Only notes it; the bake starts from Update.
    public static void SkinDrawn(object __instance)
    {
        Component c = __instance as Component;
        if (c == null) return;
        RtScene.SkinDrawn(c);
        int id = c.GetInstanceID();
        if (skins.ContainsKey(id)) return;
        Skin s = new Skin();
        s.component = c;
        s.id = id;
        s.firstSeen = Time.realtimeSinceStartup;
        skins[id] = s;
    }

    private void Update()
    {
        RtScene.UpdateSkyCapture();
        if (loadError != null || skins.Count == 0) return;
        float now = Time.realtimeSinceStartup;
        gone.Clear();
        foreach (KeyValuePair<int, Skin> p in skins)
        {
            Skin s = p.Value;
            if (s.component == null)
            {
                foreach (KeyValuePair<Texture, GlossMap> kv in s.maps) FreeGlossMap(kv.Value, kv.Key);
                s.maps.Clear();
                s.tongueSurface.Restore();
                stSetNoseFilter(s.id, -1f);
                if (s.indexBuffer != IntPtr.Zero) stUnregister(s.indexBuffer);
                gone.Add(p.Key);
                continue;
            }
            Mesh mesh = fMesh.GetValue(s.component) as Mesh;
            if (mesh == null) continue;
            if (s.bakedMesh != null && s.bakedMesh == mesh) continue;
            if (s.bakedMesh != null)
            {
                // the skin made a new mesh: the old one's entries go, and it is baked again
                if (s.indexBuffer != IntPtr.Zero) stUnregister(s.indexBuffer);
                s.bakedMesh = null;
                s.indexBuffer = IntPtr.Zero;
                s.firstSeen = now;
                continue;
            }
            if (now - s.firstSeen < SettleSeconds) continue;
            StartBake(s, mesh);
        }
        for (int i = 0; i < gone.Count; i++) skins.Remove(gone[i]);
        if (eyesDirtyAt >= 0f && now - eyesDirtyAt >= EyeSettleSeconds) ClassifyEyeItems();
        if (now >= tierNextCheck)
        {
            // the registered skins' own materials only (no scene scans): a few texture lookups each
            tierNextCheck = now + TierCheckSeconds;
            foreach (Skin s in skins.Values)
            {
                if (s.component == null) continue;
                try { CheckTier(s); }
                catch (Exception e) { s.key = "error: " + e.Message; }
            }
            if (levelsDirty)
            {
                levelsDirty = false;
                SaveTiers();
            }
        }
    }

    private void StartBake(Skin s, Mesh mesh)
    {
        s.bakedMesh = mesh;  // also when it fails: not tried again every frame
        try
        {
            object dm = fDazMesh.GetValue(s.component);
            int nv = mesh.vertexCount;
            // Classify this skin's actual character once per bake, never from names or UVs.
            // No oral AO resources or per-frame tracing for male/unidentified characters.
            Component character = s.component.GetComponentInParent(tCharacter);
            s.mouthEligibility = character == null ? "unknown" : ((bool)fCharacterIsMale.GetValue(character) ? "male" : "female");
            Vector3[] verts = dm != null ? pMorphedVerts.GetValue(dm, null) as Vector3[] : null;
            Vector3[] normals = dm != null ? pMorphedNormals.GetValue(dm, null) as Vector3[] : null;
            string source = "morphed";
            if (verts == null || normals == null || verts.Length != nv || normals.Length != nv)
            {
                verts = mesh.vertices;
                normals = mesh.normals;
                source = "mesh";
            }
            Vector2[] uvs = mesh.uv;
            if (verts.Length != nv || normals.Length != nv || uvs.Length != nv) throw new Exception("vertex arrays do not match the mesh");
            string regionalState;
            float[] regional = RegionalSurface.ForMesh(uvs, out regionalState);
            string[] names = dm != null ? pMaterialNames.GetValue(dm, null) as string[] : null;
            Material[] materials = fGpuMaterials.GetValue(s.component) as Material[];
            int subs = mesh.subMeshCount;
            float[] pos = new float[nv * 3], nrm = new float[nv * 3], uv = new float[nv * 2];
            for (int i = 0; i < nv; i++)
            {
                pos[3 * i] = verts[i].x; pos[3 * i + 1] = verts[i].y; pos[3 * i + 2] = verts[i].z;
                nrm[3 * i] = normals[i].x; nrm[3 * i + 1] = normals[i].y; nrm[3 * i + 2] = normals[i].z;
                uv[2 * i] = uvs[i].x; uv[2 * i + 1] = uvs[i].y;
            }
            List<int> indices = new List<int>();
            int[] subStart = new int[subs], subCount = new int[subs], subFlags = new int[subs], gpuStart = new int[subs];
            float[] st = new float[subs * 4];
            StringBuilder receivers = new StringBuilder(), mouth = new StringBuilder();
            for (int k = 0; k < subs; k++)
            {
                subStart[k] = indices.Count;
                if (mesh.GetTopology(k) == MeshTopology.Triangles) indices.AddRange(mesh.GetTriangles(k));
                subCount[k] = indices.Count - subStart[k];
                gpuStart[k] = (int)mesh.GetIndexStart(k);
                string name = names != null && k < names.Length && names[k] != null ? names[k] : "";
                int flags = s.mouthEligibility == "female" ? MouthAO : 0;
                if (name.Length > 0 && !NotReceiver.IsMatch(name)) flags |= Receiver;
                // Female pelvic grafts are skin even on layouts without our regional map (e.g. Olympia).
                // Falling through to RtOnly treated these as nails and reset their diffuse/SSS/gloss constants,
                // leaving a dark material boundary against Hips. Deep transmission is still omitted.
                if (s.mouthEligibility == "female" && name.Equals("defaultMat", StringComparison.OrdinalIgnoreCase)) flags |= Receiver | SurfaceOnly;
                if (name.Length > 0 && !NotOccluder.IsMatch(name)) flags |= Occluder;
                if (TeethName.IsMatch(name)) flags |= Teeth;
                if (TongueName.IsMatch(name)) flags |= Tongue;
                if (Regex.IsMatch(name, "gums|innermouth", RegexOptions.IgnoreCase)) flags |= Mouth;
                if (LipsName.IsMatch(name)) flags |= Lips;
                if (ScleraName.IsMatch(name)) flags |= Sclera;
                if (IrisName.IsMatch(name)) flags |= Iris;
                if (PupilName.IsMatch(name)) flags |= Pupil;
                if (name.Equals("Nostrils", StringComparison.OrdinalIgnoreCase)) flags |= Nostrils;
                if (name.Equals("Face", StringComparison.OrdinalIgnoreCase)) flags |= Face;
                // 0.22.0: nails and the genital graft (defaultMat) without an entry of their own: one for the lighting atlas
                // only (VaM's own program until the atlas is ready)
                if (name.Length > 0 && (flags & Receiver) == 0 && RtOnlyName.IsMatch(name)) flags |= Receiver | SurfaceOnly | RtOnly;
                // the lighting atlas's part, for the submeshes the module draws (an entry: skin, nails, teeth, tongue, the
                // eyes; the gums and the inside of the mouth where the geometric mouth occlusion runs)
                if (name.Length > 0 && ((flags & (Receiver | Teeth | Tongue | Sclera | Iris | Pupil)) != 0 || (flags & (Mouth | MouthAO)) == (Mouth | MouthAO)))
                    flags |= (PartOf(name) + 1) << RtPartShift;
                // Match RtScene.SkipSkin: the faces replaced by a graft are never drawn or evaluated by Surface.
                // Baking Hidden as a receiver joined its UVs to Hips, so edge filtering read its zero surface/history
                // and drew a black ambient rim. Leave the hole for dilation from the visible skin instead.
                if (name.IndexOf("hidden", StringComparison.OrdinalIgnoreCase) >= 0) flags = 0;
                subFlags[k] = flags;
                if ((flags & Receiver) != 0) receivers.Append(receivers.Length > 0 ? " " : "").Append(name);
                if ((flags & (Teeth | Tongue)) != 0) mouth.Append(mouth.Length > 0 ? " " : "").Append(name);
                Material mat = materials != null && k < materials.Length ? materials[k] : null;
                Vector2 scale = Vector2.one, offset = Vector2.zero;
                if (mat != null && mat.HasProperty("_MainTex"))
                {
                    scale = mat.GetTextureScale("_MainTex");
                    offset = mat.GetTextureOffset("_MainTex");
                }
                st[4 * k] = scale.x; st[4 * k + 1] = scale.y; st[4 * k + 2] = offset.x; st[4 * k + 3] = offset.y;
            }
            FindEyes(s, dm, mesh, names);
            IntPtr ib = mesh.GetNativeIndexBufferPtr();  // synchronises with the render thread, once per bake
            if (ib == IntPtr.Zero) throw new Exception("no index buffer");
            int[] idx = indices.ToArray();
            Logger.LogInfo("Regional scattering: " + s.component.name + ": " + regionalState);
            GCHandle[] pins = new GCHandle[] {
                GCHandle.Alloc(pos, GCHandleType.Pinned), GCHandle.Alloc(nrm, GCHandleType.Pinned), GCHandle.Alloc(uv, GCHandleType.Pinned),
                GCHandle.Alloc(idx, GCHandleType.Pinned), GCHandle.Alloc(subStart, GCHandleType.Pinned), GCHandle.Alloc(subCount, GCHandleType.Pinned),
                GCHandle.Alloc(subFlags, GCHandleType.Pinned), GCHandle.Alloc(gpuStart, GCHandleType.Pinned), GCHandle.Alloc(st, GCHandleType.Pinned), GCHandle.Alloc(regional, GCHandleType.Pinned) };
            int job;
            try
            {
                BakeDesc d = new BakeDesc();
                d.version = 2;
                d.indexBuffer = ib;
                d.skin = s.id;
                d.vertexCount = nv;
                d.pos = pins[0].AddrOfPinnedObject();
                d.nrm = pins[1].AddrOfPinnedObject();
                d.uv = pins[2].AddrOfPinnedObject();
                d.indexCount = idx.Length;
                d.indices = pins[3].AddrOfPinnedObject();
                d.subCount = subs;
                d.subIndexStart = pins[4].AddrOfPinnedObject();
                d.subIndexCount = pins[5].AddrOfPinnedObject();
                d.subFlags = pins[6].AddrOfPinnedObject();
                d.subGpuStart = pins[7].AddrOfPinnedObject();
                d.subST = pins[8].AddrOfPinnedObject();
                d.regional = regional == null ? IntPtr.Zero : pins[9].AddrOfPinnedObject();
                job = stBake(ref d);  // copies everything
            }
            finally
            {
                for (int i = 0; i < pins.Length; i++) pins[i].Free();
            }
            if (job <= 0) throw new Exception("ST_Bake " + job);
            s.indexBuffer = ib;
            s.job = job;
            s.state = "baking (" + source + " vertices)";
            Logger.LogInfo("VaM skin transmission: baking " + s.component.name + " (" + nv + " vertices, " + subs + " materials, " + source +
                           " vertices; through: " + receivers + "; mouth: " + mouth + ")");
        }
        catch (Exception e)
        {
            s.state = "error: " + e.Message;
            Logger.LogError("VaM skin transmission: cannot bake " + s.component.name + ": " + e.Message);
        }
    }

    private IEnumerator FrameEvents()
    {
        WaitForEndOfFrame end = new WaitForEndOfFrame();
        GL.IssuePluginEvent(renderEvent, OpInstall << 24);
        for (int frame = 0; ; frame++)
        {
            yield return end;
            GL.IssuePluginEvent(renderEvent, OpFrame << 24);
            // 0.22.62: what PT adapted or skipped for one object, and PT pauses on a device failure (once each)
            for (int k = 0; k < 4 && stPathNotice != null; k++)
            {
                int n = stPathNotice(pathNoticeBuffer, pathNoticeBuffer.Length);
                if (n <= 0) break;
                Logger.LogWarning("PT: " + Encoding.ASCII.GetString(pathNoticeBuffer, 0, n));
            }
            if (frame == 10)
            {
                if (stInstalled() != 0) Logger.LogInfo("VaM skin transmission: context hooks installed");
                else Logger.LogError("VaM skin transmission: context hooks not installed: " + NativeError());
            }
        }
    }

    // The eye camera: every camera that renders both eyes in one pass; its opaque pass is timed (GPU) and the subsurface
    // scattering runs at its end (its field of view: the millimetres per pixel). 0.22.0: every camera's PreCull hands the
    // frame's scene to the ray tracing at the first one after the skins have drawn (the eye camera at the latest), with
    // the eye camera's culling mask.
    private static int eyeMask = -1;
    // 0.22.65: RtScene reads what the eye camera drew (a first-person plugin's hidden head) between that camera's
    // onPreRender and onPostRender. Subscribed at load, before any scene plugin, so it runs ahead of their restore
    // (VaM's stereo eye camera calls OnRenderObject after onPostRender, too late).
    private static void EyeRendered(Camera c)
    {
        if (RtScene.IsSkyCaptureCamera(c)) return;
        RtScene.EyeRendered(c);
    }
    private static void PreCull(Camera c)
    {
        if (c == null) return;
        if (RtScene.IsSkyCaptureCamera(c)) return;
        // 11-09: a scene's second stereo camera is not the eye. One stage pack's CameraHook (UI only,
        // depth 1) came last and left its culling mask for the next frame's record, which an earlier camera can start: in VR most of
        // the stage never reached the PT, and the sky capture, the background and Embody's watch followed that camera too.
        Camera center = CameraTarget.centerTarget != null ? CameraTarget.centerTarget.targetCamera : null;
        bool eye = center != null && center.isActiveAndEnabled && center.stereoEnabled ? c == center : (c.stereoEnabled || c == center);
        if (eye) eyeMask = c.cullingMask;
        RtScene.PreCull(c, eye, eyeMask, RtWanted());
        if (!eye) return;
        stSetSubsurfaceView(1f / (c.stereoEnabled ? c.GetStereoProjectionMatrix(Camera.StereoscopicEye.Left) : c.projectionMatrix).m11);
        for (int i = hooked.Count - 1; i >= 0; i--)
        {
            if (hooked[i].camera == c) return;
            if (hooked[i].camera == null) hooked.RemoveAt(i);
        }
        Hooked h = new Hooked();
        h.camera = c;
        h.begin = new CommandBuffer();
        h.begin.name = "skin transmission timing begin";
        h.begin.IssuePluginEvent(renderEvent, (OpBegin << 24) | (c.stereoEnabled ? 2 : 1));
        h.end = new CommandBuffer();
        h.end.name = "skin transmission timing end";
        h.end.IssuePluginEvent(renderEvent, OpEnd << 24);
        c.AddCommandBuffer(CameraEvent.BeforeForwardOpaque, h.begin);
        c.AddCommandBuffer(CameraEvent.AfterForwardOpaque, h.end);
        hooked.Add(h);
    }

    // the scene is handed over only when the native side traces it (D3D11On12 with DXR, [RayTracing] Renderer = PathTracing)
    internal static bool RtWanted()
    {
        return rtRenderer != null && rtRenderer.Value == "PathTracing" && stRtAvailable() != 0;
    }

    private static string NativeError()
    {
        byte[] buf = new byte[512];
        int n = stLastError(buf, buf.Length);
        return Encoding.ASCII.GetString(buf, 0, Math.Max(0, Math.Min(n, buf.Length - 1)));
    }

    // ---- test API (VaM test agent) ------------------------------------------------------------------------------------

    public static string Status()
    {
        if (loadError != null) return "{\"error\":\"" + loadError.Replace("\\", "\\\\").Replace("\"", "'") + "\"}";
        byte[] buf = new byte[1 << 14];
        int n = stRead(buf, buf.Length);
        // the text can grow between two reads (0.22.0: a SimXR run got an empty "native"): room to spare, a few tries
        for (int k = 0; n < 0 && k < 4; k++) { buf = new byte[-n + 4096]; n = stRead(buf, buf.Length); }
        StringBuilder sb = new StringBuilder();
        sb.Append("{\"eye_cameras\":").Append(hooked.Count).Append(",\"eye_aside_error\":\"").Append((eyeAsideError ?? "").Replace("\"", "'"))
          .Append("\",\"wraps\":").Append(wraps.Count)
          .Append(",\"shadow_map\":{\"size\":").Append(shadowMapSize).Append(",\"lights\":").Append(shadowLights.Count)
          .Append(",\"raised\":").Append(raisedLights.Count).Append(",\"error\":\"").Append((shadowMapError ?? "").Replace("\"", "'")).Append("\"}")
          .Append(",\"rt_scene\":").Append(RtScene.Status()).Append(",\"rt_wanted\":").Append(RtWanted() ? 1 : 0)
          .Append(",\"native_error\":\"").Append(NativeError().Replace("\"", "'")).Append("\",\"skins\":[");
        bool first = true;
        foreach (Skin s in skins.Values)
        {
            if (!first) sb.Append(",");
            first = false;
            sb.Append("{\"id\":").Append(s.id).Append(",\"name\":\"").Append(s.component != null ? s.component.name.Replace("\"", "'") : "")
              .Append("\",\"job\":").Append(s.job).Append(",\"state\":\"").Append(s.state.Replace("\"", "'")).Append("\"")
              .Append(",\"tongue_material\":\"").Append(s.tongueSurface.State.Replace("\"", "'")).Append("\"")
              .Append(",\"mouth_ao_eligibility\":\"").Append(s.mouthEligibility).Append("\"")
              .Append(",\"eye_radius_mm\":").Append((s.eyeRadius * 1000f).ToString("0.00")).Append(",\"eye_allow\":").Append(s.eyeAllow)
              .Append(",\"eye_items\":\"").Append(s.eyeItems.Replace("\"", "'")).Append("\"")
              .Append(",\"uid\":\"").Append(s.uid.Replace("\"", "'")).Append("\",\"key\":\"").Append(s.key.Replace("\\", "/").Replace("\"", "'"))
              .Append("\",\"level\":").Append(s.level.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture))
              .Append(",\"auto_level\":").Append(s.autoLevel.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture))
              .Append(",\"gloss\":").Append(float.IsNaN(s.gloss) ? "null" : s.gloss.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture))
              .Append(",\"gloss_p50\":").Append(s.glossP50.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture))
              .Append(",\"gloss_p90\":").Append(s.glossP90.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture))
              .Append(",\"spec_mean\":").Append(s.specMean.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture))
              .Append(",\"normal_slope\":").Append(s.slope.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture))
              .Append(",\"skin_params\":[").Append(s.shininess.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append(",")
              .Append(s.glossOffset.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append(",")
              .Append(s.specInt.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append(",")
              .Append(s.specOffset.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append(",")
              .Append(s.fresnel.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append(",")
              .Append(s.specBump.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append(",")
              .Append(s.specColour.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture)).Append("],\"gloss_maps\":[");
            bool firstMap = true;
            foreach (GlossMap g in s.maps.Values)
            {
                sb.Append(firstMap ? "" : ",").Append("{\"name\":\"").Append(g.name.Replace("\\", "/").Replace("\"", "'")).Append("\",\"face\":").Append(g.face ? 1 : 0)
                  .Append(",\"row\":").Append(g.row).Append(",\"bound\":").Append(g.bound).Append(",\"failed\":").Append(g.failed ? 1 : 0)
                  .Append(",\"p50\":").Append(g.p50.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture))
                  .Append(",\"spread\":").Append(g.spread.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture))
                  .Append(",\"spec_mean\":").Append(g.specMean.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture))
                  .Append(",\"spec_offset\":").Append(g.specOffset.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture))
                  .Append(",\"spec\":\"").Append(g.spec != null ? g.spec.name.Replace("\\", "/").Replace("\"", "'") : "").Append("\"}");
                firstMap = false;
            }
            sb.Append("]");
            AppendMaterials(sb, s);
            sb.Append("}");
        }
        sb.Append("],\"native\":").Append(n > 0 ? Encoding.ASCII.GetString(buf, 0, n) : "null").Append("}");
        return sb.ToString();
    }

    // per submesh: material name, the shader of the skin's GPU material and of the DAZMesh's own material, the GPU
    // material's Subsurface Color (diagnostics)
    private static void AppendMaterials(StringBuilder sb, Skin s)
    {
        if (s.component == null) return;
        Behaviour b = s.component as Behaviour;
        sb.Append(",\"active\":").Append(b != null && b.isActiveAndEnabled ? 1 : 0).Append(",\"materials\":[");
        try
        {
            object dm = fDazMesh.GetValue(s.component);
            string[] names = dm != null ? pMaterialNames.GetValue(dm, null) as string[] : null;
            Material[] gpu = fGpuMaterials.GetValue(s.component) as Material[];
            FieldInfo fm = dm != null ? dm.GetType().GetField("materials", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance) : null;
            Material[] own = fm != null ? fm.GetValue(dm) as Material[] : null;
            int n = Math.Max(names != null ? names.Length : 0, gpu != null ? gpu.Length : 0);
            for (int k = 0; k < n; k++)
            {
                string name = names != null && k < names.Length && names[k] != null ? names[k] : "";
                Material g = gpu != null && k < gpu.Length ? gpu[k] : null;
                Material o = own != null && k < own.Length ? own[k] : null;
                string subd = "";
                if (g != null && g.HasProperty("_SubdermisColor"))
                {
                    Color c = g.GetColor("_SubdermisColor");
                    subd = c.r.ToString("0.###") + " " + c.g.ToString("0.###") + " " + c.b.ToString("0.###");
                }
                sb.Append(k > 0 ? "," : "").Append("[").Append(k).Append(",\"").Append(name.Replace("\"", "'")).Append("\",\"")
                  .Append(g != null && g.shader != null ? g.shader.name : "").Append("\",\"")
                  .Append(o != null && o.shader != null ? o.shader.name : "").Append("\",\"").Append(subd).Append("\"]");
            }
        }
        catch (Exception e)
        {
            sb.Append("[-1,\"").Append(e.Message.Replace("\"", "'")).Append("\",\"\",\"\"]");
        }
        sb.Append("]");
    }

    public static string SetEnabled(bool on)
    {
        if (loadError != null) return "error: " + loadError;
        stSetEnabled(on ? 1 : 0);
        return on ? "on" : "off";
    }

    public static string SetParams(float s, float wrap, float thicknessScale, float maxMM, float depthR, float depthG, float depthB)
    {
        if (loadError != null) return "error: " + loadError;
        ApplyParams(s, wrap, thicknessScale, maxMM, depthR, depthG, depthB);
        return "ok";
    }

    // shadow kernel, millimetres: the shading point moved inward along the normal, kernel radius, falloff of the ring
    // weights (0 = an even inner ring only)
    public static string SetShadow(float insetMM, float radiusMM, float falloffMM)
    {
        if (loadError != null) return "error: " + loadError;
        stSetShadow(insetMM * 0.001f, radiusMM * 0.001f, falloffMM * 0.001f);
        return "ok";
    }

    // sideways spread of the thickness bake (Gaussian sigma, mm; 0 = raw per-vertex values) for the next bakes: Rebake()
    public static string SetBakeSmooth(float mm)
    {
        if (loadError != null) return "error: " + loadError;
        stSetBake(mm);
        return "ok";
    }

    // Test API only: the nasal cavity mask, across surface lighting, transmission and subsurface compositing.
    public static string SetNose(float amount)
    {
        if (stSetNose == null) return "error: not loaded";
        stSetNose(Mathf.Clamp01(amount));
        return "ok";
    }

    // the highlight as [Specular] Normalize (0 / 1); the config's comes back with the next config change
    public static string SetHighlight(int normalize)
    {
        if (loadError != null) return "error: " + loadError;
        ApplySpecular(normalize != 0);
        return "ok";
    }

    // a look by its name until 0.11.0 (tools): VaM = Normalize off, Soft / Natural / Moist = on
    public static string SetLook(string name)
    {
        if (loadError != null) return "error: " + loadError;
        int i = Array.IndexOf(OldLookNames, name);
        if (i < 0) return "error: no look " + name;
        ApplySpecular(i > 0);
        return name;
    }

    // the config's [Specular] and [Diffuse] settings again (after test calls); each skin back at its own level and the
    // build's rows
    public static string ApplyConfig()
    {
        if (loadError != null) return "error: " + loadError;
        stSetOverallTranslucency(0f);
        ApplySpecular(specNormalize.Value);
        ApplyDiffuse(BaseDiffuse);
        Array.Copy(TierRows, tierRowsNow, TierRows.Length);
        ApplyMouth();
        stSetRtTest(0xffffffffu);
        ApplyEnv();
        stSetTiers(tierRowsNow, TierRows.Length / 8, TierDefaultRow);
        ownRows = true;
        foreach (Skin s in skins.Values)
            foreach (GlossMap g in s.maps.Values) g.written = float.NaN;
        return ForceLevel(-1f);
    }

    // For the VR menu: one line per character drawn now, "uid<TAB>starting level<TAB>level" (0.00 .. 4.00; -1 = not
    // known yet)
    public static string SkinGlossLevels()
    {
        if (loadError != null) return "";
        List<string> lines = new List<string>();
        foreach (Skin s in skins.Values)
        {
            Behaviour b = s.component as Behaviour;
            if (b == null || !b.isActiveAndEnabled || s.uid.Length == 0) continue;
            lines.Add(s.uid + "\t" + s.autoLevel.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) + "\t" +
                      s.level.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
        }
        lines.Sort(StringComparer.Ordinal);
        return string.Join("\n", lines.ToArray());
    }

    // a character's level (0 .. 4; its starting level or < 0 = not kept), kept for its skin (saved within half a second);
    // at once
    public static string SetSkinGlossLevel(string uid, float level)
    {
        if (loadError != null) return "error: " + loadError;
        int n = 0;
        foreach (Skin s in skins.Values)
        {
            if (s.component == null || s.uid != uid || s.key.Length == 0) continue;
            if (level < 0f || float.IsNaN(level) || Mathf.Abs(level - s.autoLevel) < 0.005f) levelChosen.Remove(s.key);
            else levelChosen[s.key] = Mathf.Round(Mathf.Clamp(level, 0f, LevelMax) * 100f) / 100f;
            CheckTier(s);
            n++;
        }
        if (n > 0) levelsDirty = true;
        return n > 0 ? "ok" : "error: no character " + uid;
    }

    // test: every skin at this level (0 .. 4), not kept; < 0 = each its own again
    public static string ForceLevel(float level)
    {
        if (loadError != null) return "error: " + loadError;
        forcedLevel = level >= 0f ? Mathf.Min(level, LevelMax) : -1f;
        foreach (Skin s in skins.Values)
            if (s.component != null) CheckTier(s);
        return "level " + forcedLevel.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
    }

    // test: 0 = every gloss map on the steps' rows (the level rounded) with its whole map's stats, as 0.17.1; 1 = each
    // its own row (0.18.0)
    public static string SetOwnRows(int on)
    {
        if (loadError != null) return "error: " + loadError;
        ownRows = on != 0;
        foreach (Skin s in skins.Values)
        {
            foreach (GlossMap g in s.maps.Values) g.written = float.NaN;
            if (s.component != null) CheckTier(s);
        }
        return ownRows ? "own rows" : "step rows";
    }

    // test: one step row (0-3 face, 4-7 body): alpha^2 at p50, R, alpha^2 floor, ceiling, gloss map mip bias, gain, the
    // least spread; every skin's own rows follow
    public static string SetTierRow(int row, float a2, float r, float lo, float hi, float bias, float gain, float spread)
    {
        if (loadError != null) return "error: " + loadError;
        if (row < 0 || row >= TierRows.Length / 8) return "error: no row " + row;
        float[] v = { a2, r, lo, hi, bias, gain, spread, 0f };
        Array.Copy(v, 0, tierRowsNow, row * 8, 8);
        stSetTiers(tierRowsNow, TierRows.Length / 8, TierDefaultRow);
        foreach (Skin s in skins.Values)
        {
            foreach (GlossMap g in s.maps.Values) g.written = float.NaN;
            if (s.component != null) CheckTier(s);
        }
        return "ok";
    }

    // specular replacement, every parameter (on: 0 = VaM's own program, 1 = skin GGX, 2 = the replacement giving VaM's
    // result); the config's look comes back with the next config change
    public static string SetSpecular(int on, float mipBias, float pore, float f0, float roughness, float sharp, float wide, float sharpWeight)
    {
        if (loadError != null) return "error: " + loadError;
        stSetSpecular(on, mipBias, pore, f0, roughness, sharp, wide, sharpWeight);
        return "on " + on;
    }

    // skin detail (0.11.0), every parameter: refA2 > 0 = the same roughness for every skin (alpha^2 at its mean gloss),
    // lo / hi its gloss map's variation kept; intensity 1 = the spec map over its own mean; diffuseMM / cavityMM > 0 =
    // the diffuse normal / the cavity filtered to that width; ambient 1 = the ambient on the diffuse normal; tiers 1 =
    // each skin's step row (0.13.0). The config's choice comes back with the next config change
    public static string SetSkinDetail(float refA2, float lo, float hi, int intensity, float diffuseMM, int ambient, float cavityMM, int tiers)
    {
        if (loadError != null) return "error: " + loadError;
        stSetSkinDetail(refA2, lo, hi, intensity, diffuseMM, ambient, cavityMM, tiers);
        return "ok";
    }

    // test: the diffuse base by name (VaM / Subtle / Medium / Strong); ApplyConfig() puts Medium back
    public static string SetDiffuseLook(string name)
    {
        if (loadError != null) return "error: " + loadError;
        return ApplyDiffuse(name) ? name : "error: no look " + name;
    }

    // diffuse replacement, every parameter (on: 0 = VaM's own diffuse, 1 = per-channel wrap, 2 = the replacement giving
    // VaM's diffuse); the config's look comes back with the next config change
    // Temporary review override 0..2; ApplyConfig restores the saved menu value.
    // Test project only: 0 uniform control, 1 regional shading, 2 UV registration view.
    public static string SetRegional(int mode)
    {
        if (stSetRegional == null) return "error: not loaded";
        stSetRegional(Math.Max(0, Math.Min(2, mode)));
        return "regional mode " + mode;
    }

    public static string SetOverallTranslucency(float amount)
    {
        if (loadError != null) return "error: " + loadError;
        if (stSetOverallTranslucency == null) return "error: not loaded";
        if (float.IsNaN(amount) || float.IsInfinity(amount)) return "error: non-finite amount";
        amount = Mathf.Clamp(amount, 0f, 2f);
        stSetOverallTranslucency(amount);
        return "overall translucency " + amount;
    }

    // 0.20.0 candidate: the translucency on female skin with the blocker-distance shadows (amount = [Diffuse]
    // OverallScattering): its glow's strength (0 = off), its blur's scale (1 = 10 + 22 mm), its colour (0 warm gray ..
    // 1 the angular response's own)
    public static string SetTranslucency(float strength, float width, float tint)
    {
        if (loadError != null) return "error: " + loadError;
        if (stSetTranslucency == null) return "error: not loaded";
        stSetTranslucency(strength, width, tint);
        return "translucency strength " + strength + " width " + width + " tint " + tint;
    }

    public static string SetScatter(int on, float aR, float aG, float aB, float pR, float pG, float pB)
    {
        if (loadError != null) return "error: " + loadError;
        stSetScatter(on, aR, aG, aB, pR, pG, pB);
        return "on " + on;
    }

    // teeth look by name, as [Teeth] Look; the config's look comes back with the next config change
    public static string SetMouth(float ao, float wet) {
        if(stSetMouth==null) return "unavailable";
        tongueSurfaceOn=ao>=0 && wet>0;
        if(!tongueSurfaceOn) foreach(Skin s in skins.Values) s.tongueSurface.Restore();
        stSetMouth(ao,wet); return "mouth AO="+ao+" wet="+wet;
    }

    // test (0.22.12 candidate, SimXR A/B; never in a menu): the skin's IBL reflected about the highlight's normal (1, the
    // default) or about VaM's full-resolution, bumpiness-bent normal (0, before)
    public static string SetEnvSpecNormal(int on)
    {
        if (loadError != null) return "error: " + loadError;
        stSetEnvSpecNormal(on);
        return "env spec normal " + on;
    }

    // test (candidate, SimXR A/B; never in a menu): each light's skin diffuse times Burley's grazing factor at the skin's
    // own roughness (1, the default) or not (0, as 0.22.14)
    public static string SetSkinGrazing(int on)
    {
        if (loadError != null) return "error: " + loadError;
        stSetSkinGrazing(on);
        return "skin grazing " + on;
    }

    // test (0.22.20, SimXR A/B; never in a menu): the vellus sheen on skin (weight 0.15, the default; 0 = as 0.22.19)
    public static string SetSkinFuzz(float weight)
    {
        if (loadError != null) return "error: " + loadError;
        stSetSkinFuzz(weight);
        return "skin fuzz " + weight;
    }

    // test (0.22.20, SimXR A/B; never in a menu): each light's skin specular from its 0.25 m disc (1, the default) or from
    // a point (0, as 0.22.19)
    public static string SetSpecDisc(int on)
    {
        if (loadError != null) return "error: " + loadError;
        stSetSpecDisc(on);
        return "spec disc " + on;
    }

    // test (0.22.1): the hairs RtScene knows, their particles read back (RtScene.HairProbe)
    public static string RtHairProbe()
    {
        return RtScene.HairProbe();
    }

    // The test agent and menu use the same persisted lighting selection.
    public static string PathTracing(bool enabled)
    {
        if(stPathTracing==null)return "native unavailable";
        rtRenderer.Value=enabled?"PathTracing":"Legacy";ApplyEnv();
        return enabled?"path tracing requested":"existing lighting";
    }
    public static string PathMaterials() { return RtScene.PathMaterials(); }
    public static string RendererState() {
        if(rtRenderer==null)return "当前：未加载";
        if(rtRenderer.Value=="Legacy")return "当前：VaM 光照";
        if(PathRenderingActive)return "当前：路径追踪";
        if(stPathRequested!=null && stPathRequested()==0)return "当前：原光照回退（路径追踪不可用）";
        return "当前：准备路径追踪";
    }
    public static string PathSkinFixture(bool enabled) { return RtScene.PathSkinFixture(enabled); }
    // test: the author rules (rtcore path_tracing.h kPathAuthor*), for this run
    // test: where the background's budget starts, for the next SetPathAuthorTest
    public static string SetPathBackgroundTest(float near, float far)
    {
        pathBackgroundNear = near; pathBackgroundFar = far;
        return "background from " + near + " to " + far;
    }
    public static string SetPathAuthorTest(int rules, float ambient, float contact, float ambientRange, float emitter)
    {
        if (loadError != null) return "error: " + loadError;
        pathAuthorTest = true;
        stSetPathAuthor(unchecked((uint)rules), ambient, contact, ambientRange, emitter, pathBackgroundNear, pathBackgroundFar);
        return "author rules 0x" + rules.ToString("x") + " ambient " + ambient + " contact " + contact + " range " + ambientRange + " emitter " + emitter;
    }
    // test (SimXR A/B; never in a menu): RtCore's layers (bits: rtcore.h's test layers; -1 = all); ApplyConfig: all
    public static string SetRtTest(int layers)
    {
        if (loadError != null) return "error: " + loadError;
        stSetRtTest(unchecked((uint)layers));
        return "rt test layers 0x" + unchecked((uint)layers).ToString("x");
    }

    // test (0.19.1): the shadow map size of the noted spot / point lights (0 = Unity's own); ApplyConfig() puts it back
    public static string SetShadowMapSizeTest(int size)
    {
        if (size != 0 && (size < 256 || size > 8192)) return "error: 0 or 256..8192";
        SetShadowMapSize(size);
        return "shadow map size " + size + " on " + raisedLights.Count + " of " + shadowLights.Count + " noted lights";
    }

    public static string SetTeethLook(string name)
    {
        if (loadError != null) return "error: " + loadError;
        return ApplyTeeth(name) ? name : "error: no look " + name;
    }

    // teeth and tongue, every parameter (on: 0 = VaM's own); the config's look comes back with the next config change
    public static string SetTeeth(int on, float subsurface, float gain, float backStrength, float backStart, float gumStrength, float gumPower)
    {
        if (loadError != null) return "error: " + loadError;
        stSetTeeth(on, subsurface, gain, backStrength, backStart, gumStrength, gumPower);
        return "on " + on;
    }

    // eyes look by name, as [Eyes] Look; the config's look comes back with the next config change
    public static string SetEyesLook(string name)
    {
        if (loadError != null) return "error: " + loadError;
        return ApplyEyes(name) ? name : "error: no look " + name;
    }

    // eyes, every parameter (both off: VaM's own); the config's look comes back with the next config change
    public static string SetEyes(int occOn, float upper, float upperWidth, float lower, float lowerWidth, int catchOn, float strength, float alpha,
                                 float f0, float cornea)
    {
        if (loadError != null) return "error: " + loadError;
        stSetEyes(occOn, upper, upperWidth, lower, lowerWidth, catchOn, strength, alpha, f0, cornea);
        return "ok";
    }

    // which eye effects one character gets (bit 0 eyelid occlusion, bit 1 catchlight), until clothing is switched again
    public static string SetEyeSkin(int id, int allow)
    {
        if (loadError != null) return "error: " + loadError;
        Skin s;
        if (skins.TryGetValue(id, out s)) s.eyeAllow = allow & 3;
        stSetEyeSkin(id, allow);
        return "ok";
    }

    // subsurface scattering (strength 0..1, 0 = off; width = the skin profile's scale, 1 = measured skin);
    // the config's strength comes back with its next change
    public static string SetSubsurface(float s, float width)
    {
        if (loadError != null) return "error: " + loadError;
        sssWidth = width;
        ApplySubsurface(s, width);
        return "strength " + s + " width " + width;
    }

    // diagnostics: which pixel shaders draw the registered skin meshes (Status: native.diag_skin_ps)
    public static string SetDiag(bool on)
    {
        if (loadError != null) return "error: " + loadError;
        stSetDiag(on ? 1 : 0);
        return on ? "on" : "off";
    }

    public static string Rebake()
    {
        if (loadError != null) return "error: " + loadError;
        int n = 0;
        foreach (Skin s in skins.Values)
        {
            if (s.component == null || s.bakedMesh == null) continue;
            Mesh mesh = fMesh.GetValue(s.component) as Mesh;
            if (mesh == null) continue;
            instance.StartBake(s, mesh);
            n++;
        }
        return "rebaking " + n;
    }

    public static string ResetTiming()
    {
        if (loadError != null) return "error: " + loadError;
        stResetTiming();
        RtScene.ResetTiming();
        return "ok";
    }

}
