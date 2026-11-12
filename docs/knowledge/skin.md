# Skin shading (raster route)

VaM lights skin with one Blinn-Phong lobe, a flat diffuse falloff that leaves a floor of light on every back-facing
surface, and no light through thin parts, no soft scattering under the surface, no eyelid occlusion on the eyeballs and
no occlusion inside the mouth. Skin presets also set the gloss so differently that some burn out to white sheets and
others are matte everywhere. The skin component replaces VaM's own skin pixel programs with ones that keep VaM's inputs
(textures, light constants, shadow maps) and change the lighting model. A player sees skin that reads as skin: matte
cheeks, a controlled highlight on nose and lips, soft red at lip edges, nostrils and shadow edges, ears that glow when a
light is behind them, teeth that are not piano keys, eyes with a catchlight and an eyelid shadow, a mouth that gets dark
toward the throat.

This document covers the route with path tracing off (`[RayTracing] Renderer = Legacy`): VaM's own raster lighting and
shadow maps, our shading on top. Path-traced skin (its own BSDF, thin-part light and screen-space scattering inside the
tracer) is in [path-tracing.md](path-tracing.md); the settings that both routes read are marked below.

## How it works

Data flow, from VaM's shaders to the screen. All paths are under `components/shading/skin/`.

1. **Lifting VaM's programs (on the user's machine).** `tools/skin_shaders.py` reads VaM's skin shaders
   (`Custom/Subsurface/*`) out of `VaM_Data/globalgamemanagers.assets` through `components/shared/shader-lift`
   (`bundle_audit.py`, `parse_shader.py`, `shader_extract.py`, `find_shader_tt.py`): each pass with its keywords,
   constant-buffer layouts, slots and the D3D11 bytecode of every sub-program. It needs `VAMKIT_VAM` (the VaM folder) and
   `VAMKIT_FXC` (the Windows SDK x64 folder holding `d3dcompiler_47.dll`). Everything generated goes to `out/gen/skin/`
   and is never committed: it is VaM's own code in edited form.
2. **`tools/asm_lift.py`** turns a DXBC pixel program into equivalent HLSL instruction by instruction (disassembly with
   hex literals so constants keep their bits; registers as float4 locals, integer instructions through
   `asint`/`asfloat`; inputs declared in the original's signature order). `lift(dx).hlsl(insert=...)` accepts text to
   insert before or after any instruction index. `tools/vs_emu.py` runs a vertex program's disassembly on one vertex
   with known values to find which output registers carry world position, normal and UV.
3. **`tools/gen_spec_table.py`** (about 2000 lines, the core) lifts every `MARMO_LINEAR` ForwardBase and ForwardAdd pixel
   program of the Gloss/GlossNM, Cull/NoCull and tessellated-mapped skin shaders (ComputeBuff or not: same pixel
   bytecode), finds the places to change by data flow, and compiles each again with a replacement model selected by a
   constant buffer at `b12` (`specCtl`, `specLobe`, `scatCtl`, `teethCtl`, `eyeCtl`, `skinCtl`, `skinNorm`, `skinLight`,
   and more; 92 floats, `kSpecFloats` in `native/skin_transmission.cpp`). Mode 0 returns the original program's result,
   mode 1 the new model, mode 2 (tests only) VaM's own specular rebuilt from the vectors this code reconstructs. The
   places found by data flow include the specular exponent, the N.H power, the specular normal, the diffuse N.L, the
   view vector and S (Subsurface Color). A program where any is not found exactly once stops the generator: this is
   deliberate, a silent mismatch would patch the wrong instruction. Output: `out/gen/skin/spec_programs.h`, plus
   `spec_cases/` for the offline tests.
4. **`tools/gen_skin_table.py`** finds, for each ForwardAdd pixel program of the opaque skin shaders for a light with a
   shadow map (spot, point, point cookie), the light kind, the shadow slots, where the vertex program puts world
   position, normal and UV, and the slots/offsets of `_LightColor0`, `_WorldSpaceLightPos0`, `unity_WorldToLight`,
   `_Color`, `_MainTex`. Programs with identical answers share one transmission shader
   (`shaders/transmission.hlsli`), whose input signature is checked register for register against the original so it
   links with the skin draw's vertex shader. Output: `skin_programs.h`, `variants/`, `skin_table.json`.
5. **`tools/gen_sss_shaders.py`** and **`tools/gen_mouth.py`** compile `shaders/sss.hlsl` and `shaders/mouth.hlsl` into
   `sss_shaders.h` and `mouth_shaders.h`. Order and the rest of the build: `manifest/components.toml`, component `skin`.
   Any change to the program-table structure requires re-running the generators and rebuilding the native library. A
   stale table once read out of bounds in a texture slot and crashed scene load inside d3d11; `static_assert`s and the
   sorted-hash check in `ST_Init` now catch part of this.
6. **Swapping at draw time (`native/skin_transmission.cpp`).** The module hooks the D3D11 device's `CreatePixelShader`
   (`hCreatePS`) and the context's `PSSetShader` (`hPSSetShader`) and draw calls (`hDrawIndexed*`, ...), chained so it
   works with the hair module whichever patches first. A pixel shader is a skin program when the SHA-1 of its bytecode
   (a hash truncated to 8 bytes) is in `kSkinPrograms`; for each such program a replacement is created next to the
   original (`kSpecPrograms`, binary-searched by hash). Which character and material a draw belongs to is known from the
   draw's index buffer and first index, registered per character by the plugin (`VaMSkinTransmission.cs`, found through
   a Harmony postfix on `DAZSkinV2.DrawMeshGPU`, so no scene scans). For a registered draw the module binds the
   replacement and the `b12` constants for that draw only and puts both back. If any step fails (no replacement, bake
   not done, unknown layout), the draw keeps VaM's program: the failure mode is always "VaM's look".
7. **Bake (`native/bake.cpp`, worker thread).** Two seconds after a skin first draws, the plugin hands its bind-pose mesh
   to the bake: thickness (rays along the inverted normal, smoothed 3 mm), the mouth measurements, the eyeball spheres
   and eyelid contact lines. Textures are created on the render thread.

### Layer stack (in render order)

- **Diffuse.** In this repository's raster route the diffuse is VaM's own factor (so every skin's Subsurface Color still
  decides its lit side) times a per-channel wrap past the terminator, `((N.L + a) / (1 + a))^p` over green's, with red
  wrapping farthest (`DiffuseA`/`DiffuseP`, look "Medium", always on: `BaseDiffuse` in the plugin; applied through
  `ST_SetScatter`). The normal map is filtered to 1.2 mm for the diffuse (light spreads about that far under skin and a
  DLSS mip bias otherwise draws pores at output resolution), and the ambient light uses that diffuse normal (VaM lights
  it with the specular normal bent up to 2.2x). Two further diffuse models exist in the generated programs
  (`scatCtl.x = 3`: shadowed irradiance with a Burley 2012 grazing factor; `SkinGrazing`) but the plugin in this
  repository never selects mode 3: it belonged to the texture-space ray-tracing route, which is not in the repository
  (see "Not in this repository"). The grazing term is therefore dormant here.
- **Regional scattering.** A hand-built, anatomy-guided weight map (mean about 0.8, range 0.56 to 1.02) multiplies the
  SSS radius by its square root. It is an artistic map, not a measured thickness. Female skin in the standard
  24928-vertex layout only; any other layout, or no map in the build, falls back to a uniform response
  (`RegionalSurface.cs`). The map is not in the repository (below).
- **Screen-space subsurface scattering** (`[Subsurface] Strength`, 0 to 1; `native/sss.cpp`, `shaders/sss.hlsl`). The
  replacement programs write extra targets that only the skin draws of the eye camera's opaque pass bind: SV_Target1 =
  direct diffuse light without albedo (E) and in w the distance to the camera, SV_Target2 = the albedo, SV_Target3 =
  SV_Position.z. After the opaque pass: `SssPrep` keeps E where the depth buffer still shows that skin, `SssBlur` runs
  twice (across, down) a six-Gaussian d'Eon and Luebke profile on 21 taps placed in millimetres on the skin, per eye,
  depth-aware (a tap at another depth counts less by the same Gaussians); `SssComp` adds `albedo * strength * (blur - E)`
  where the pixel is skin. Only direct diffuse moves; specular and ambient stay. The width is fixed at 1 = measured
  skin (`sssWidth`); wider looked wrong. Strength above 1 produces dark rims next to shadows and is not offered.
  Effect: lip edges, nostrils, eyelid rims and shadow edges glow softly red. The same setting also drives the path
  tracer's own screen-space scattering ([path-tracing.md](path-tracing.md)).
- **Light through thin parts** (`[General] Strength`; both routes read it). Per skin draw lit by a spot or point light
  with a shadow map, a second draw of the same geometry (same vertex shader, blend one+one, depth test equal, no depth
  write; `shaders/transmission.hlsli`) adds that light's transmission. Thickness = the larger of the baked thickness and
  the path the light crosses according to its own shadow map (so light does not come through the head to an eyelid, nor
  through a nose bone); Gaussian transmittance per colour (sigma 2.5, 1.45, 0.85 mm for R, G, B: orange-red through thin
  skin, deep red through thicker, nothing past a centimetre or so); only light from behind counts; 13 samples across
  the light direction, each with its own path, so edges are soft. Coverage: non-tessellated skin materials (face, ears,
  lips, nostrils, neck, hands, forearms, feet). Torso and legs are tessellated materials and at least 14 mm thick, so
  they are not covered. Directional lights and lights without shadows add nothing. In dim light the extra transmission
  and the scattering fade out continuously (`smoothstep(0, .06, Y)` in linear shader units), so skin does not glow red
  in the dark; this is a look adaptation, not an absorption change.
- **Sheen.** A white Charlie sheen lobe (alpha 0.5, Neubelt visibility, weight 0.15, zero on lips;
  `SkinSheen` in `gen_spec_table.py`, `ST_SetSkinFuzz`) models vellus hair: nothing facing light and eye, a soft glow
  where the half vector grazes, i.e. the rim of skin lit from behind or the side. It is added to the specular in the
  same replacement and is active in this route.

### Specular

- **Model.** Two white GGX lobes, Smith height-correlated visibility, Schlick Fresnel on V.H with F0 0.028, times N.L.
  The roughness comes from VaM's own exponent (`alpha^2 = 2 / (n + 2)`), so each skin's gloss map still shapes it. The
  specular normal is the normal map sampled with mip bias 2.5 so single pores do not catch the light; the variance the
  bias removed widens the lobes. The specular map is kept as a cavity term: its value over its own mean (its last mip),
  times `saturate(_SpecInt)`. `[Specular] Normalize = false` returns VaM's program unchanged; this is the first
  thing to switch when a highlight looks wrong.
- **Gloss scale.** One continuous level per skin, 0 to 4: 0 no skin highlight, 1 natural matte, 2 soft, 3 satin, 4 dewy.
  The four anchor rows (`TierRows` in the plugin) are alpha^2 at the gloss map's median 0.38 / 0.28 / 0.20 / 0.175,
  with matching R, floor/ceiling and gain; levels between interpolate (`LevelRow`: alpha^2, R and bounds geometric, the
  rest linear), below 1 the highlight fades to none at 0. Each gloss map gets its own row (`ST_SetTierRow`) carrying its
  p50 and p90 and its spec map's mean measured **only over the texels of its skin materials** (`ST_TexStatsMasked` with a
  256x256 UV mask the plugin rasterises: the face map takes Face, body maps their skin materials), because whole-texture
  statistics included UV padding, scalp and nails and made torsos several times glossier than cheeks. The body rows
  spread their map half as far (R of the body row is the square root of the face's) because a body map's p50-to-p90
  range is mostly paint and noise.
- **Starting level.** Computed from the author's own settings, not chosen by hand: VaM's own highlight formula on a
  standard sphere, one light 50 degrees off the view, at the face gloss map's p50 and p90, the lobe widened by the
  normal map's pore-scale slope variance (measured by `ST_NormalSlope`); `SkinGloss()` reads log2 of the shown
  highlight's spread; `level = clamp(1 + (reading + 0.675) / 0.75, 1, 4)`. A level set in the menu is kept per skin in
  `BepInEx/config/local.vam.skintransmission.tiers.txt`, one line `level<TAB>key`, key = face diffuse path | normal map
  path. Only the skin currently drawing binds its gloss map's row (after a scene change VaM keeps the previous scene's
  skin in the same cached textures and would otherwise win).
- **Low-light adaptation.** With Normalize on, the direct highlight fades with the light reaching the pixel (colour,
  attenuation, shadow) and the environment highlight, separated from the SH diffuse, fades with the reflected light
  reaching it, both by `smoothstep(0, .06, Y)`. It never raises anything and does not touch roughness, F0 or the levels.
- **Environment specular** is VaM's cube map times the directional albedo of the GGX lobes at the lobes' mip, sampled
  along the highlight's own normal (`eyeGeo.y`), not VaM's full-resolution normal bent by `_SpecularBumpiness`. Using the
  bent normal counted pore variance twice and made foreheads and noses grainy under an environment key light (23 matched
  head-turn frames, simulated headset: pore-level contrast in highlights about 24 percent lower, brightness within
  1 percent).
- **Light disc.** `SpecDirect` can widen both lobes for a spherical light (Karis' `a2 + s(3 sqrt(a2) + s) / (4 V.H)`,
  `skinLight.y`, `ST_SetSpecDisc`) so the highlight is as large as the source; the disc radius is `shadowCtl.y`. Nothing
  in this repository sets that radius for the raster route (it came with the removed atlas route), so lights are
  highlighted as points here. Not confirmed in the game; treat as dormant.

### Mouth, teeth, eyes

- **Mouth occlusion (female only, `[Mouth] Enabled`, `Strength` default 0.95).** `native/mouth_bake.cpp` builds a BVH of
  the posed mouth geometry, `native/mouth_gpu.cpp` runs `shaders/mouth.hlsl` (`Prepare`, `Refit`, `Trace`) every real
  frame, shared by both eyes: 96 hemisphere rays per receiving point (teeth, tongue, gums, inner wall, lips), range
  scaled with lip width and character scale. The result `lerp(1, cavity, Strength)` darkens **all** light (direct,
  specular, scattering input): this was a decision, not an oversight; AO only on ambient was rejected as far too bright.
  Contrast is concentrated at depth: exposed surfaces stay bright, the incisor mid-gap is occluded only lightly. Gender
  is read from the parent `DAZCharacter.isMale`; male or unknown: no mouth rays.
- **Tongue material (`[Mouth] TongueMaterial`).** The original tongue displacement map is blurry and was being used as a
  normal at runtime. The plugin binds a rebuilt normal and gloss (relief reconstructed by frequency band from a sharper
  texture: an estimate, not measured height; roughness .305 to .385, wetter toward the tip) to the standard tongue UV only;
  other UVs keep the original material. These assets are not in the repository (below).
- **Teeth (`[Teeth] Look`: VaM, Even, Natural, Deep).** VaM's teeth have Subsurface Color 0, so the diffuse factor is 2x
  facing the light and nothing on the back side: under side light the teeth look like piano keys, each fully lit or
  black. Even recomputes the factor with S = 1 for all characters (about 0.92x head on). Natural and Deep also darken
  toward the back of the mouth (back .6, gums .35 for Natural, from a two-channel texture baked along the lip-tooth axis
  and per tooth, 32 teeth by union-find and position welding) but only where the geometric mouth occlusion does not run
  (males, unknown): the geometric occlusion replaces it, they are never stacked.
- **Eyes (`[Eyes] Look`: VaM, Subtle, Natural, Deep).** VaM's cornea has no catchlight worth the name and nothing darkens
  the eyeball under the lids. The bake fits a sphere to each sclera (about 15.1 mm radius on the standard female) and
  finds the eyelid contact points (where skin crosses 1.02 R; at most 64 per eye; they follow blinks). Every frame a
  compute shader (`kEyeGatherCS`) reads the skin's vertex buffer into a small rig buffer at `t17`. **E1** darkens the
  eyeball toward the lids, `1 - s exp(-d^2 / w^2)` per lid. **E2** adds a catchlight: one GGX lobe (alpha 0.05, F0
  0.025) on a corneal sphere of 0.6 R, lit by the pass's light, so it follows each light and each eye's view. Clothing
  that sits on the eyeball (shadow layers, painted catchlights, tear lines; about a third of scenes have some) is
  detected 1 s after it is enabled (80 percent of its vertices within 0.85 to 1.25 R of the eyeball and a small
  bounding box): a bright overlay turns E2 off, a visible dark one turns E1 off (`ST_SetEyeSkin`). Not validated on
  real overlay items. Not done: enamel translucency, tooth wet highlights, tear line, iris depth.

### The graft mesh must count as skin

The standard female mesh carries `defaultMat`, a separate genital graft mesh (2836 triangles) that sits over a hole in the
hips and shares only 69 vertex positions with the body. It is visible skin: shading, occlusion, thickness bake and
previews must treat it as receiver and occluder; without it the crotch is a hole. `Hidden` is the body faces it replaces:
exclude it from both receiving and blocking (it shares the hips' UV island and otherwise paints a black ring). The graft
has its own bone set (hip, pelvis, abdomen, abdomen2 and both thighs appear twice in the merged skin's bone table): when
posing a mesh offline, set every bone with the same name, not the first one (first-only was 81 mm off in a seated pose).
Exported OBJs have graft normals tens of degrees off; the live GPU mesh does not. Material-name rules written for one
region (for example the mouth bake's exclusion of cornea, reflection, lashes, tear, `defaultMat`) must not be copied
elsewhere.

## Settings

All in `BepInEx/config/local.vam.skintransmission.cfg` unless noted. The menu exposes the marked ones
([settings-and-menu.md](settings-and-menu.md)).

| Section / key | Values | What it trades |
|---|---|---|
| `[General] Enabled` | true | read at start-up; false loads nothing, VaM unchanged |
| `[General] Strength` | 0 to 4 in the file, menu 0 to 2, default 1 | light through thin parts (1 = measured skin; 0 = no extra draws); both routes; menu |
| `[Subsurface] Strength` | 0 to 1, default 1 | screen-space scattering; 0 costs nothing; both routes; menu |
| `[Specular] Normalize` | true | false = VaM's own highlight (diagnostic); menu |
| per-skin gloss level | 0 to 4 | in `local.vam.skintransmission.tiers.txt`, set from the menu; `[Specular] Look` and `Gloss` are old keys that are read once for migration and removed |
| `[Mouth] Enabled`, `Strength`, `TongueMaterial` | true, 0.95, true | geometric mouth occlusion; tongue assets (fall back to VaM's tongue when absent) |
| `[Teeth] Look` | Natural | VaM, Even, Natural, Deep |
| `[Eyes] Look` | Natural | VaM, Subtle, Natural, Deep |
| `[Shadows] ShadowMapSize` | 2048 | 0 = Unity's own, 2048, 4096. VaM's size follows the light's share of the screen and can be 256 (about 2 cm a texel at 2.5 m, visible as blocks in contact shadows) |
| `[RayTracing] Renderer`, `RRDetail`, `PathBounces`, `PathBackground` | Legacy / PathTracing, 75, 0 to 3, Off / Far / All | path tracing; see [path-tracing.md](path-tracing.md) |

Removed and not to be re-added without the code that served them: `[RayTracing] Enabled/Ambient/Lights/HairShadows`,
`[EnvShadow] *`, `[Shadows] DistanceSoftness`, `[Diffuse] OverallScattering`. The plugin still holds
`ST_SetOverallTranslucency` and the translucency code in the generated programs, fixed at the off step (0); they are not
reachable from any setting.

## Measured

Numbers are approximate; the notes do not record resolution or headset refresh for most of them. All on the development
machine ([frame-budget.md](frame-budget.md)).

- GGX specular in the opaque stage: about +0.02 to +0.04 ms per frame, the same for every gloss level.
- Screen-space scattering: about 0.2 ms per frame for two eyes, independent of width.
- Mouth occlusion, optimised: about 0.24 ms for one offline gather (3687 receivers x 96 rays). The first version, a
  per-vertex software BVH, cost 21.3 ms per character per real frame and cut a heavy two-character scene from about 34 to 12-14 real frames per second; it was not noticed for a while because both sides of every A/B contained it. Lesson: measure every new
  per-frame GPU feature alone before installing it.

## Pitfalls

- **Hard line under the chin, or lines at shadow edges** -> light on back-facing skin that a sharp shadow no longer hides
  (VaM's factor leaves `(1 - S/2) * S/2 / 4` everywhere; a wrap continued it past N.L 0; a 3 cm blurred shadow used to
  hide it). Do not tune the shadow filter; ask which light should not be there. Any new diffuse term must not
  reintroduce a floor of light on the back side.
- **Highlights look plastic, oily, flat or pore-less** -> start here: `[Specular] Normalize` off to compare, then the
  skin's gloss level. The gloss levels are an absolute style scale, not a translation of VaM's presets.
- **A near-black gloss map makes every level look alike** -> some presets paint the highlight map nearly black on skin
  (a few 8-bit values on the cheeks) and only lips and lids bright; levels barely change them. Statistics-based
  remapping and rank mapping were tried offline and did not help. An idea not built: a standard regional template for
  maps with no real skin information.
- **After a scene change a skin keeps the old gloss row** -> only the skin currently drawing may bind a gloss map's row.
- **Crash at scene load in d3d11 after editing the generators** -> a program table whose layout changed without
  regenerating and rebuilding the native library.
- **Eye or crotch looks different in a diff image** -> an eye-motion plugin (not in this repository) rotates eyeballs even in a frozen scene,
  and the graft mesh is skin ([above](#the-graft-mesh-must-count-as-skin)); neither is the candidate's doing.
- **Dark lower lids / dark eyes with a clothing item** -> an eye-overlay item that is not detected (detection runs only on
  clothing enable/disable, through the item's own materials).

## Tried and dropped

- A medium/strong diffuse base with an outward warm terminator, and a former `[Diffuse] Look` selector: two models doing
  one job, the later overriding the earlier, with no one able to reason about the result. One diffuse model remains.
- Ambient light transmission through the ear rim: +8 levels at the rim, not visible; removed.
- Overall (centimetre-scale) translucency glow outside the shadow: removed as a setting because it was confusing next to
  the thin-part light and does nothing under path tracing. Do not re-add the glow unless asked; the thin-part light is the
  feature that remains.
- SSS width 3 instead of 1: wrong look. SSS strength above 1: dark rims.
- Mouth occlusion as an ambient-only term, and as a final-colour multiply applied to a nose: the first is too bright; the
  second darkened direct light, SSS and transmission and added dark outlines on the nose. Random-grain tongue surface:
  broke the highlight without giving tissue texture.
- Taking a minimum, a plane test, an N.L ramp or a horizon factor on shadow filters to cure the chin line: all kept the light that
  caused it.
- The Burley term and the sheen were judged by simulated-headset side-by-side stills with neural rendering (NR) on; the grazing
  direction (brighter retroreflection or darker Fresnel) cannot be derived from theory for skin, which is why the sheen
  is a separate, explicit term rather than an automatic roughness rule.

## Not in this repository

- **Regional scattering map and tongue surface data** derive from VaM's mesh and textures and have no generator yet.
  `RegionalSurface.cs` and `TongueSurface.cs` look for embedded resources (`Regional.FemaleRegional.bin`, `Mouth.Tongue*.png`,
  `TongueLayout.bin`, `TongueSurface.txt`); when absent the plugin logs once, uses a uniform response and VaM's own
  tongue. A different VaM build or a missing asset changes nothing else.
- **The texture-space ray-tracing atlas** (the former "existing lighting" route: lighting visibility, environment and
  bounce light baked into an atlas) and **EnvShadow** (a BVH ambient occlusion and blocker-distance penumbra for female
  skin) are not here. With them went the irradiance diffuse mode and the penumbra least radius (average triangle edge
  around a vertex, used as the lower bound of the blocker-distance radius, which stopped shoulders from showing mesh
  corners and faces from looking flat). Raster shadows are now VaM's own maps, which VaM blurs by `shadowStrength`.
- The eye-lid occlusion and eye looks above are raster-only. Path tracing has its own eye model.

## Open

- Hard horizontal division inside the nostrils: cause established: VaM's Face material has `_IBLFilter = 0`, Nostrils
  0.5, so ambient light halves across the material boundary (zeroing the ambient removes the line; matching the filter
  also removes it). A fix, matching the nostril filter to the face's and multiplying only ambient light by a geometric AO,
  removed the line but the mesh crease at the nostril's upper edge remained; not installed. Do not darken the final
  colour for this.
- Seams between limbs and torso under particular lights on one family of skin presets (limbs' normal map 4K, torso's 8K,
  so the same UV density gives different bump RMS; this is content and exists in vanilla): not reproduced, and the usual
  causes (gloss and diffuse values across the seam, decals, normal recalculation, regional weights, light choice) were
  excluded. Next step is to reproduce with the exact scene and lights, including environment light, from the back and
  sides.
- The raster diffuse has no grazing term and no light-disc specular (dormant, above); whether to enable either again
  needs the missing diffuse mode 3 path or a replacement.
- Low-light adaptation was verified on GPU composites and in load tests, not in game on a person's eyes.

## Verify

How the look is judged: by a person, on side-by-side stills with neural rendering (NR) on, ideally in the headset. Whole-crop
statistics ("p99 of 5 to 8 levels, barely visible") were wrong more than once: lips, nostrils and mouth corners were
visibly redder and softer. Measure highlights against highlights; decide on pictures. Flicker and cost are guards, not
goals.

Offline tests (Windows, `VAMKIT_VCVARS` = `vcvars64.bat`; they build into `out/skin-tests/`):

- `tests/build_spec_test.cmd`, then `spec_test.exe <out/gen/skin/spec_cases> [hash ...]` (after `tools/gen_spec_table.py`).
  For each lifted program, the original and its replacement draw 32x32 random inputs with random constant buffers and
  textures. Proves: mode 0 equals the original; mode 2 (VaM's specular through the rebuilt vectors) equals it within
  1e-3; mode 1 changes pixels and stays finite; the diffuse modes, teeth (S = 0.6 equals the original, an occlusion
  texture multiplies exactly), eyes (far lid points give the original; a degenerate lid polyline gives the original times
  `1 - upper strength`; catchlight only adds), and the debug layer reports every program linking with its vertex shader.
- `tests/build_sss_test.cmd`, then `sss_test.exe <spec_cases dir>`. Part A: the extra targets carry albedo x light
  exactly (albedo x E equals the light's diffuse in the colour with `_LightColor0` on and off). Part B: `native/sss.cpp`
  on a synthetic eye camera against a CPU reference within one 8-bit step, no light crossing between the eyes, red
  spreading farther than blue, non-skin pixels untouched, pipeline state restored.
- `tests/build_mouth_test.cmd` (needs `tools/gen_mouth.py` first), then `mouth_test.exe`: a posed mouth gives a small
  occlusion near the geometry and full visibility far away, and moving the vertices needs no rebake. It exercises the
  geometry path only, not the look.
- The path-tracing side of skin is covered by the tests in [path-tracing.md](path-tracing.md).

Only the game shows: the overall look, low-light behaviour, eye-overlay clothing, the graft area, anything with a real
person's presets. Run the lab scripts in `tools/lab/README.md` for stills.
