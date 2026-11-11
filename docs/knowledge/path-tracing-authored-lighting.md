# Path tracing a scene that was lit for raster

Every VaM scene was lit by its author while looking at Unity 2018.1's forward renderer: lights without size, shadow
maps, a flat ambient colour that reaches everywhere, reflection probes, no bounce light, a post-processing stack on
top. A physically straightforward path tracer shows such a scene differently from what the author composed: fill
lights suddenly cast shadows, a lamp inside its shade is blocked by the shade, a closed room loses its ambient, a
dark stage that was held together by a reflection probe goes black, a silhouette thrown on a far wall melts into a
70 cm penumbra. This document lists how the tracer reads each authored setting so that the author's statement
survives, what it deliberately does not copy, and the rule that decides between the two. A player sees the scene's
composition as authored, with traced shadows, occlusion and reflections in it.

The tracer itself is described in [path-tracing.md](path-tracing.md); VaM's and Unity's lighting facts in
[vam-facts.md](vam-facts.md).

## The standing rule

1. **Fill in what the tracer failed to read from the author; do not converge to the raster picture.** When the
   traced picture differs from stock, sort the difference into one of two classes. *The tracer missed something the
   author wrote* (a probe, a light setting, a shader's meaning): fix it as a general translation rule. *The tracer
   computes real light that raster lacks* (bounce lets a black wall show, a room is airier): keep it; it is the
   point of tracing. Similar brightness is a consequence of reading the rules correctly, never the target.
2. **Rules are general, one bit each, measured in the game.** No per-scene constants. When a look differs, measure
   what Unity does with that setting on a test surface and translate that.
3. **The rules are always on.** There is no player-facing switch for "author rules"; only a test entry
   (`VaMSkinTransmission.SetPathAuthorTest`) changes them for a run. Rule 0 = the physical tracer.
4. **Tracing never falls back to raster** for a scene or an object (see "Objects are adapted" in
   [path-tracing.md](path-tracing.md)).

**Comparison baseline.** The reference is the stock raster picture: the launcher's vanilla mode (nothing of this mod
loaded) from the author's camera position, with the scene's **third-party post-processing disabled on both sides**.
The usual post stack carries a colour LUT that lifts blacks and shifts tone; comparing with it on attributes the
LUT's work to lighting (an early comparison concluded "all pure black gone" from the LUT alone). `Renderer = Legacy`
is not the stock picture either: it has this mod's skin shading. Compare from the author's camera: stages and
panorama spheres are dressed for one view direction only. Check VR separately; a desktop match does not imply a VR
match.

## How it works

Rule bits live in `rtc::PathEnvironment::authorRules` (`components/shading/rt-core/include/path_tracing.h`,
`kPathAuthor*`; mirrored as `PT_AUTHOR_*` in `shaders/path_trace.hlsl`). The plugin sends them every time settings
apply (`ApplyEnv` in `components/shading/skin/VaMSkinTransmission.cs`: `PathAuthorRules`, plus
`PathBackgroundRules` unless `PathBackground = Off`) through `ST_SetPathAuthor` to `RtSetPathAuthor`
(`components/shading/skin/native/rt.cpp`). Parameters: ambient share 0.5, contact range 0.6 m, ambient range 1 m,
emitter scale 0.25.

### Lights

- **Effect lights with a fixed angular size.** A VaM light has no emitter size; Unity shadows point and spot lights
  hard. Authors place lights 1.5-2.3 m from a person and set range for the look: Unity's falloff follows
  `distance / range`, so position is composition, not a fixture. The tracer therefore gives every point and spot
  light a constant apparent radius of 8 degrees seen from the lit point (`RtScene.PtLightAngular` = 0.1396 rad,
  physical radius 0; the sampled disc is `radius + tan(angle) x distance` in `PtAnalyticCandidate`). A directional
  light is the sun: 0.27 degrees. History of the choice, because each alternative fails in a known way: a 0.25 m
  disc washed shadows out; a point source gave knife-edge terminators on arms and hair; a 3 cm emitter is still a
  point source at 2 m; 12 degrees looked flat. 8 degrees was chosen by a person from side-by-side frames
  (3 cm / 4 / 8 / 12 degrees). Think of lights as effect lights when changing anything here, not as fixtures.
- **Falloff.** `1 / (1 + 25 (d/range)^2)`, windowed to zero over the last fifth of the range
  (`smoothstep(1, .64, (d/range)^2)`, `PtLightFalloff`); a hard cut at the range drew a bright ring on walls.
  Colour x intensity is converted to linear as one value (VaM's `lightsUseLinearIntensity` is off).
- **Spot lights** (`kPathAuthorSpot`, `PtSpotUnity`): Unity's forward spot is its default cookie x the falloff above
  with **true distance**, not distance along the axis. The cookie was read from the game: full to 0.63 of the cone's
  tangent radius, 0.52 at 0.82, 0 at 0.99 (`PT_SPOT_COOKIE`), a narrower pool than the cone, which is the circle
  authors compose with. Custom cookies are not applied.
- **Shadow strength** (`kPathAuthorCrisp`, `PtAuthorShadowRadius`). Measured on a Standard wall at strength 0, 0.166
  and 1: VaM reads `shadowStrength` as the **blur radius of the shadow edge on every surface; the shadow is always
  fully dark**. (A scene file without the key means 0.2.) On surfaces that are not a person the light's disc is
  limited to the radius that reproduces that edge about 1 m from the light, so a silhouette thrown on a wall 5 m away
  stays a silhouette. People keep the 8 degree light: their soft self-shadowing was chosen on look.
- **Lights without shadows** (`kPathAuthorNoShadow`): shadows off, or a vertex light. A fill light must not start
  casting. The shadow ray is shortened to a contact range (0.6 m, jittered between half and full so that no edge
  marks its end): shape and contact stay, anything farther lets the light pass. The same shortening applies to
  surfaces that do not receive shadows in Unity (`kPathAuthorNoReceive`, render queue above 2500).
- **Shadow near plane** (`kPathAuthorNear`): nothing nearer to a point or spot light than its shadow near plane
  casts, so a light inside its lamp shade or fixture is not blocked by it.
- **Culling masks and exclusion layers.** A common scene plugin moves excluded atoms to dedicated layers and removes
  those layers from a light's culling mask. In Unity an excluded object is neither lit by that light nor casts its
  shadow. The tracer encodes up to five such classes in instance masks (per-light linking in
  [path-tracing.md](path-tracing.md)). Ambient and emitters ignore the mask, as in Unity.

### Casters

- **Cast shadows Off** (`kPathAuthorCull`, `rtc::kNoCast`): in no shadow map, so it blocks no analytic light;
  environment, bounce and camera rays still see it. Under the same bit a shadow ray culls front faces (Unity's
  shadow map draws the faces looking at the light), so a one-sided shell does not stop a light outside it while a
  closed mesh shadows the same.
- **Shadows Only** (`rtc::kShadowOnly`): blocks analytic lights, invisible to every other ray.
- **Transparent casters** (`kPathAuthorSolid`, `MaterialDesc::solidShadow`): a scene object on VaM's translucent
  skin-family programs falls through the shader fallback chain to a cutout shadow caster with a cutoff near zero, so
  in Unity a sheer object throws a **solid** shadow, and authors use that as a device. Shadow rays to analytic lights
  treat such objects as opaque above the cutoff instead of attenuating by `1 - alpha`. Not applied to a person's
  clothing (rejected on look, see Tried and dropped).
- **Outline shells** (inverted hulls for toon outlines): Unity culls the near half so only the rim shows; traced
  both ways the shell blocks its object's own light. `RtScene.CameraOnly` marks a matched shell `rtc::kCameraOnly`
  (camera rays only). The match is deliberately one known object by name and parent.

### Ambient, exposure and probes

- **Diffuse environment per shader family.** VaM's lit programs read three different things
  (`EnvironmentOf` in `RtScene.cs`, `PtEnvironmentLobes`): the skin family (`Custom/Subsurface/*`, `Custom/Hair/*`,
  skin IBL) = Skyshop SH x `_ExposureIBL.x` + Unity SH x `_ExposureLM.x`; `Marmoset/*` = Skyshop SH + Unity SH;
  everything else (Standard and copies) = Unity SH only. Unity's ambient is scaled by `_ExposureLM.x`
  (`unityAmbientIntensity`) **only for the skin family** (test bit 28). Scaling it for every surface turned scenes
  whose author took the ambient off the person nearly black around her.
- **Skyshop's cube is for reflections only** (`kPathAuthorSkySh`). Diffuse = Skyshop's SH x `.x`; reflection = the
  cube x `_ExposureIBL.y`. The two scales are independent and an author may set `.y` far from `.x` to tame an HDR
  cube; using the cube x `.x` as diffuse made such scenes tens of times too bright.
- **Ambient reaches everywhere** (`kPathAuthorAmbient`, `PtAuthorAmbientShare`): Unity's ambient is unoccluded, in a
  closed room too. At the first surface of a path, half of the diffuse environment is taken with occlusion only
  within 1 m; the other half is fully occluded. Not applied where a scene carries its own indoor probe.
- **A low ambient also lowers bounce** (`kPathAuthorBounce`): where the author scaled the person's Unity ambient
  down, the room's bounce light onto her is scaled the same; to her it is ambient too.
- **Reflection probes** (`kPathAuthorProbe`, `PathMaterial::mode` 262144, `PathProbe` in `RtScene.cs`,
  `PtProbeSpecular`, `PtProbeRadiance`). Unity's Standard family takes its indirect specular from
  `unity_SpecCube0` and nowhere else. Some stages are visible only through it where no light reaches. The tracer
  picks one probe (highest importance, then the one containing the camera; no per-renderer blending). On Standard
  surfaces inside its box: roughness of 0.4 and above adds Unity's term (mip by roughness, HDR decode, box
  projection, `surfaceReduction x FresnelLerp`) and that lobe traces no ray; smoother surfaces are traced as usual
  and read the probe where the ray leaves the scene. Skin and hair do not read the probe in Unity and do not here.
- **Emissive surfaces** (`kPathAuthorEmitter`): seen directly or in a reflection an emitter is as bright as drawn;
  the diffuse light it casts on other surfaces is scaled by 0.25 (authors use emission as a look, not as a lamp).
- **Camera exposure** (`_ExposureIBL.w`) scales the tracer's exposure (`Frame::exposure`).

### Sky and background

- **Skyshop's visible sky** is a mesh that follows the player and is off by default: in most authored scenes the
  background is the camera's clear colour. With it hidden a camera ray that misses shows the clear colour; with it
  shown the background cube is sampled in the sky mesh's own object space (`PathEnvironment::backgroundToSky`,
  `UpdateBackground`). The lighting sky and the visible sky rotate independently.
- **Unity skybox materials and panorama spheres** (`UpdateSkyCapture`, `VisibleSkyRenderer`). Scenes bring skies as
  `Skybox/*` materials, often on a large sphere. Equirectangular panoramas (360 or 180 degrees, mono or stereo
  layouts) are sampled directly; other `Skybox/` materials are captured by a hidden camera into a linear HDR cube
  when the material changes (2048 per face, about 192 MiB plus depth). The sky is seen by camera rays only: it
  blocks no light and is excluded from emitter sampling (otherwise a panorama sphere tints the scene). Reflections
  and indirect light keep the Skyshop environment.
- **Panorama spheres do not hide what lies beyond** (`kPathAuthorPanorama`, `PtHitTracked`): Unity draws such a
  sphere first without depth, so scenery placed past it is visible. A camera ray that hits one continues, and shows
  the sphere only where nothing lies beyond.
- **VaM's own sky dome** takes the environment branch; it was once replayed as UI and covered effect layers.

### Baked-lighting environments and the indoor probe

Environments built with baked lightmaps have their light in textures the tracer does not read; only a few real-time
lights remain. Two consequences. (1) The SH rule is right for people but removes light the cube used to add to
static objects by accident (a baked hotel interior became 17 % darker). (2) Such scenes need light added in the
scene, not constants in the tracer.

The tracer offers one mechanism: a scene that contains an atom with the id `PT_IndoorEnvironment` (`RebuildRigid`)
has the author's daytime HDR reflection probe from the environment asset used as a **local diffuse environment**
(`PtIndoorCandidate`): sampled at the first diffuse vertex only, occluded by geometry within 0.35 m, strength 2.5.
It is a static approximation: it does not follow lights or furniture. The environment asset's atom must keep the
default custom-asset atom id for the probe to be found (the lookup is by that id; first usable probe wins).
Everything else for such a scene (weaker sun, fill lights) belongs in a copy of the scene. Do not change global
bounce or core parameters for one environment.

Materials of these environments needed translation too: a third-party PBR family (plain and world-position
triplanar) reads `_Albedo`, `_ColorCorrection`, normal, a mask's metalness and roughness, and emission
(`PtHzmMap`, `PtHzmNormal`); read as `_MainTex` they came out white.

### Post-processing and depth of field

- **The author's post stack runs on the reconstructed image.** Run before RR, depth of field, fog, vignette and
  grading were treated as lighting and reconstructed away. Under path tracing the bridge
  (`components/render/dlss-bridge/VaMDLSSBridge.cs`) redirects the post-processing component's source and
  destination so that it reads and writes the RR output, with per-eye depth supplied by the compositor.
- **Depth image direction.** The tracer's depth has its top row first; the image Unity hands to post-processing has
  its bottom row first. Supplying it unflipped blurred the person and left the far background sharp. Fixed; with
  the flip, focus distance and circle-of-confusion distribution match stock on a desktop orbit camera. The bug
  escaped because every test used a fixed camera with depth of field off: exercise the scene's real play flow.
- **Depth of field is off in VR under path tracing** (`PostDofInVr` = false; test entry `PostVrDof(true)`). The
  author chose the lens for a monitor (field of view 20-35 degrees); at a headset's 110 degrees nearly everything
  but the person is blurred, and a headset has no eye tracking to follow focus. It is switched off for that draw
  only; the plugin's state is untouched. This is a look decision, not a saving: frame rate is the same either way.
  On the desktop depth of field stays as authored.

### The background budget

A path's budget is set by the first thing the eye rests on past clear layers (`PtTracePixel`, `budgetSet`). A
person (skin, clothing, mesh hair, fibres) keeps `PathBounces`. Anything else is background and, where the budget
applies: no diffuse bounce (`kPathAuthorBgBounce`: its diffuse light comes from the light samples alone, which is
what Unity shows), and a reflection lobe of roughness 0.4 and above reads the sky cube unoccluded instead of tracing
(`kPathAuthorBgRough`); sharp reflections are still traced. A path that left a person stays the person's.

| `PathBackground` | Where the budget applies | Look |
|---|---|---|
| `Off` | nowhere | bounce setting governs everything |
| `Far` | past 8 m from the camera; between 8 and 16 m a growing share of paths (no visible line), whole past 16 m | not distinguishable in the scenes compared |
| `All` | everywhere that is not a person | background a few percent darker in most scenes; clearly darker where a room relied on bounce (below) |

`All` is also the way to get the stock picture's dead-black walls back in a dark stage: what makes them lighter
under tracing is real bounce.

### Other translations that are not rule bits

`Nature/` vegetation shaders count as lit (their passes have no name, which a pass-name test read as unlit); a
third-party mirror surface is an additive sheet; third-party water surfaces are replayed as the author drew them;
unlit volumetric light cones are replayed as effects (see [path-tracing.md](path-tracing.md)).

## Settings

| File / section / key | Values | Trades |
|---|---|---|
| `BepInEx/config/local.vam.skintransmission.cfg` `[RayTracing] PathBackground` | `Off` / `Far` (default) / `All` | path time against background bounce light; each display mode's preset keeps its own value |
| same, `[RayTracing] PathBounces` | 0-3 | governs people and any background that keeps its budget |
| `VaMSkinTransmission.SetPathAuthorTest(rules, ambient, contact, ambientRange, emitter)` | test only | rule bits for one run; 0 = physical tracer |
| `VaMDLSSBridge.PostVrDof(bool)` | test only | depth of field in VR under path tracing |
| `RtScene.CameraOnlyTest` | test only | the outline-shell match |

See [settings-and-menu.md](settings-and-menu.md) for the menu rows.

## Measured

Development machine ([frame-budget.md](frame-budget.md)). Brightness figures are the mean 8-bit luminance of a
screenshot and the share of pure-black pixels, third-party post-processing off unless stated.

| What | Conditions | Result |
|---|---|---|
| Reflection probe | desktop, a dark club stage, close view | 30.3 / 77 % black -> 39.0 / 3 % (stock 40.8 / 1 %; stock with the probe disabled 30.4 / 77 %) |
| same | author's camera | 8.2 / 90 % -> 16.7 / 11 % (stock, another session, 17.4 / 14 %) |
| All light rules on vs off | 13 stages of one third-party showcase, close view | medians as recorded (read as ratios to stock): mean 0.96 -> 0.98, contrast 1.01 -> 0.99. Small for most stages |
| Unity ambient per family + effect replay | desktop, one stage, wide view | 48.0 / 68 % -> 85.6 / 34 % (the development tree's raster route: 123.0 / 9 %) |
| Outline shell camera-only | same stage, a view of the shelled object | 109.9 / 22 % -> 124.8 / 10 % |
| Background `Off` / `Far` / `All` | desktop, a hotel interior, near field within 8 m, switched live | path time 10.45 / 10.46 / 3.71 ms |
| same | simulated headset 144 Hz, frame generation off, a character close-up scene | 47.3 / 47.0 / 51.6-54.7 real fps; path per eye 3.9 / 3.8 / 2.7-2.9 ms |
| Background budget, VR (`All`) vs none | simulated headset 144 Hz, RR input 35 %, 3 bounces, frame generation off, three scenes | 44.2 -> 51, 51.5 -> 55, 45.7 -> 58 real fps; in the third, path per eye 5.0 / 4.2 -> 2.6 / 1.9 ms |
| Look cost of `All` | 13 stages + two interiors against no budget | one stage -5.7 %, the others within 1.3 % (three are animated, not comparable); a daylight interior -3 %; a baked hotel interior -12 to -15 %. People unchanged |
| Post stack moved after RR | simulated headset, a stage with the stack | about -1.3 to -2 fps |
| Depth of field on vs off in VR | simulated headset | 29.0-29.9 fps both |
| Indoor probe on a baked interior | near / far view | 101.9 / 91.3 -> 116.9 / 105.9 (stock 158.2 / 120.7: the baked light is still missing) |

Not measured: the frame-rate cost of the probe rule, the cost of recapturing a sky cube, desktop cost of the rules
with frame generation on. Nothing in this document has been judged by a person in a real headset.

## Pitfalls

- **Comparing with the post stack on.** Its LUT lifts blacks; brightness conclusions are then mostly about the LUT.
- **Comparing against `Legacy` and calling it stock.** It carries this mod's skin shading.
- **Judging a stage from a free camera.** Stages and panorama spheres are dressed for the author's camera.
- **A scene's "play" flow sets state that a static load does not** (camera rides, depth of field, light intensities
  set by state transitions and not always reset). Compare stages in a fixed order and through the real buttons.
- **A lamp looks blocked by its own fixture**: check the light's shadow near plane reached the record
  (`RtScene` status `light_shadow`).
- **A statically batched room looks untextured**: half-float UVs, not materials ([path-tracing.md](path-tracing.md)).
- **VR frame rate looks too good on a stage with its own UI camera**: the stage may not be in the tracer at all
  (second stereo camera, [path-tracing.md](path-tracing.md)).
- **A panorama sphere tints the scene**: camera-only geometry must stay out of emitter sampling.

## Tried and dropped

- **Shadow strength as shadow darkness.** Measured: it is blur everywhere. Do not reintroduce.
- **Spot falloff along the axis (z).** Measured: true distance; the z version nearly doubled one stage's brightness.
- **"Vegetation receives no lights."** Stock vegetation is lit.
- **Replaying opaque-queue objects as effects.** Sorting breaks (the depth layer is copied at the first draw).
- **Cast-shadows Off made camera-only for every object, and any name containing "outline".** Too broad: such
  objects then stopped occluding ambient and bouncing light. Narrowed to the matched shell.
- **Cast-shadows Off still blocking everything** (the earlier "better, not alike" choice) and **shadowless lights
  still casting**: overridden by the rules above once it was clear these settings are authored statements.
- **Solid shadows under a person's sheer clothing** as in Unity: ugly on the body. And a global lift of negative
  alpha adjust on clothing: it revealed parts authors had hidden with alpha -1.
- **"No bounce on backgrounds when the author gave no ambient"**: written, not installed; the airier room is an
  advantage, and `PathBackground = All` gives the dead-black version on request.
- **A darker, sky-based indoor environment** for a baked interior: darker than the probe version.
- **Depth of field in VR**: see above.

## Open

- Baked environments remain darker than stock; lightmaps are not read.
- One street stage and one office stage are brighter and less black than stock. The first is bounce (with 0 bounces
  it matches stock). The second's blue floor does not change with `All`; suspected light from an emissive backdrop,
  not investigated.
- A black wall in a classroom stage is lighter than stock (about 13 against 4 in sRGB); suspected F0 0.04 reflection
  of a bright floor, not investigated.
- Far grass slightly dark in one outdoor stage; a bar floor's reflection too strong.
- Whether the third-party PBR family reads the reflection probe was not checked; it is not given the probe. One
  opaque shader of that family is not translated.
- Window emitters in baked interiors: emission strength and the light they cast were not confirmed.
- Procedural and beta panoramic skies were not checked on screen; skies that are not `Skybox/` materials are not
  guaranteed.

## Verify

- **Offline** (`components/shading/rt-core/tests`, see [path-tracing.md](path-tracing.md)): the probes run with the
  rule bits at 0 (the default of `PathEnvironment::authorRules`), so their outputs must stay byte-identical across a
  rule change; they cover the range window, shadow-only proxies, hidden sky, light linking and the separate diffuse
  and reflection scales. No probe in `tests/` sets a rule bit: the development tree's rule contracts are not in this
  repository, so the rules themselves are covered in the game only. A new rule should come with a contract here.
- **In the game only**: everything about look. Procedure: stock picture and traced picture from the author's
  camera, post stack off on both; sort each difference by the standing rule; for a suspected misreading, isolate
  the Unity behaviour on a test surface (one light, one plane) and measure it; add one rule bit; re-run the offline
  probes with the bit cleared and set. Then repeat the comparison in VR ([../protocol/testing.md](../protocol/testing.md)).
