# Path tracing

VaM lights a scene with Unity 2018.1's forward raster renderer: point-source lights, shadow maps, a flat ambient term,
no bounce light, no real reflections. With `[RayTracing] Renderer = PathTracing` the whole scene (people, clothing,
hair strands, rooms, props) is path traced at the DLSS Ray Reconstruction input resolution, one sample per pixel, and
RR preset F rebuilds the output-resolution image from the noisy colour and a set of guides. A player sees soft
shadows with real penumbrae, contact shadows, occluded ambient, reflections of the room in glossy surfaces, catch
lights in the eyes, optional bounce light, and hair lit as fibres. The switch is followed while playing; with
`Legacy` nothing is traced and VaM's own lighting runs with this mod's skin, hair and clothing shading
([skin.md](skin.md), [hair.md](hair.md), [clothing.md](clothing.md)).

Lighting authored for raster: [path-tracing-authored-lighting.md](path-tracing-authored-lighting.md). The frame
around the tracer: [frame-path.md](frame-path.md), [d3d11on12.md](d3d11on12.md), [upscaling.md](upscaling.md).

## How it works

| Layer | Where | Job |
|---|---|---|
| Scene record (managed) | `components/shading/skin/RtScene.cs`, `PathSkinned.cs`, `PathSubmitted.cs`, `PathCanvasUi.cs` | once a frame, describe every draw, material, light and hair to the native host |
| Native host | `components/shading/skin/native/rt.cpp`, `skin_transmission.cpp`, `path_textures.inl`, `path_overlay.inl` | turn D3D11 objects into D3D12 resources on the 11on12 queue, fill the `rtc::` structures, replay UI and effects into overlay layers |
| Core ("RtCore") | `components/shading/rt-core/` (`src/rtcore.cpp`, `shaders/path_trace.hlsl`, `include/rtcore.h`, `include/path_tracing.h`) | acceleration structures, the integrator, the guides |

The compositor (`components/render/opencomposite/OpenOVR/Compositor/vam_rr.inl`) owns the per-eye guide textures,
has the trace recorded (`rtc::RecordPathTrace`), runs RR and composites the overlay layers. RtCore only records into
the command list it is handed; it never submits and never waits on a fence (`rtcore.h` header comment).

### 1. The scene record (RtScene.cs)

`RtScene.PreCull` runs at the frame's first camera pre-cull after the skins have drawn and writes one record (three
rotating slots of unmanaged memory) that the render thread picks up through `IssuePluginEventAndData`. There are no
per-frame scene searches: renderers are known from hooks (atom renderer lists, `HairRender.Awake`,
`Graphics.Internal_DrawMesh` submissions in `PathSubmitted.cs`, renderer material changes).

- **People** (`AddSkin`, `AddWrap`): the same GPU buffers VaM draws from, i.e. the skinned, morphed, smoothed
  vertices with `_normalsBuffer` normals and tangents; whatever another component did to the vertices is in the
  traced shape. Skin decals within 1 mm of the skin, the cornea, the eye reflection layer and the tear layer are
  `DrawNoOcclude` (`rtc::kNoOcclude`): they receive light and never occlude.
- **Scene meshes** (`RebuildRigid`, `AddRigid`): mesh renderers of every atom, custom-asset skinned meshes
  (`PathSkinned.cs`; skinning and blend shapes on the GPU, `shaders/path_skinning.hlsl`), `Graphics.DrawMesh` meshes.
- **Materials** (`MaterialFor`): each Unity material becomes a `rtc::PathMaterial`. The translation is per shader
  family and follows what the family's pixel program computes, not its property names ("Material translation").
- **Lights** (`AddLights`, `LinkLights`): position, direction, `GammaToLinear(colour x intensity)`, range, cone,
  Unity's shadow strength and near plane, whether Unity would shadow it at all, and the culling mask, which becomes
  at most five light-link classes.
- **Hair** (`AddHairs`): each hair's GPU render particles (the strands the raster hair draws) with radius,
  roughness, cuticle shift, colour and the author's specular tint (`rtc::PathHairItem`).
- **Environment**: Skyshop's lighting cube and diffuse SH with their exposure scales, Unity's ambient SH
  (`UpdateAmbient`), the visible background (`UpdateBackground`, `UpdateSkyCapture`), a reflection probe (`PathProbe`).
- **UI and effects**: world canvases (`PathCanvasUi.cs`), UI shaders and unlit additive effects are not traced; the
  native host replays their real D3D11 draws into overlay layers (section 7).

### 2. The native host

`rt.cpp` runs on Unity's render thread after a flush of the D3D11 context. It unwraps or copies D3D11 buffers and
textures into D3D12 (`path_textures.inl`), fills `rtc::Geometry`, `rtc::Light`, `rtc::Frame`, `rtc::PathMaterial`
and `rtc::PathEnvironment`, records hair (`rtc::RecordPathHair`) and the eye rigs (`RecordPathEyes`), and calls
`rtc::Record`. The skin parameters of this mod's raster skin shader (specular lobes, gloss tier, scattering and
thin-light strength) are written into `PathMaterial::skin[]` by `RtPathSkinParameters` (`skin_transmission.cpp`)
every frame, so a menu change is live. `ST_PathSceneRevisionV1` exports the revision the compositor resets history
by. Only a device error pauses tracing (retry with back-off); the hair plugin is told every frame whether the
tracer has the strands, so a paused frame still has raster hair.

### 3. The core: acceleration structures and masks

`rtc::Record` builds or refits one bottom level per geometry and one top level per frame: skinned geometry is
refitted every frame, rigid geometry built once and refitted when marked dirty. Skin is Phong-tessellated into the
structure (section 6). Hair is a bottom level of linear swept spheres (`src/path_hair.inl`, NVAPI, chained end
caps), refitted per frame and rebuilt at most every 16 rendered frames, because an endlessly refitted BVH degrades
under large animated shape changes.

Instance masks (comment above `InstanceMask` in `rtcore.cpp`, mirrored in `path_hair.inl`):

| Bit | Seen by | Notes |
|---|---|---|
| `0x02` | camera rays | absent for `kCameraHidden`: a first-person plugin hides the possessed head in the eye camera only; it still casts shadows and shows in mirrors |
| `0x01` | every other ray (continuations, environment and emitter samples) | absent for `kCameraOnly` |
| `0x04` | shadow rays to analytic lights | `kShadowOnly` proxies have this bit alone; `kNoCast` objects lose it under the author rules |
| `0x08 << k` | shadow rays of lights that light class `k` | replaces `0x04` for geometry in one of the 5 light-link classes |

**Per-light linking.** Unity's culling mask excludes an object from a light as receiver and as caster. A light's
shadow ray mask is `0x04 | (classes it lights) << 3` (`PtLightCandidate`); light selection skips lights whose
`excludedLinks` contain the surface's class. Hair gets one fibre bottom level per class. Past five classes the extra
layers are lit by every light. Environment and emitters are not linked.

**Opacity micromaps** (`src/path_omm.inl`, `shaders/path_omm.hlsl`). Cutout and translucent submeshes get a 4-level
micromap baked from their alpha map; a micro-triangle is marked only where the shader would decide the same for
every sample (coverage exactly 0 or 1). Skin, thin glass, decals and tessellated geometry are left out. A map is
baked after its texture version and parameters have been unchanged for 8 frames, never for an alpha map that may
change any frame (`PathMaterial::alphaVersion == 0`). Test bit 19 cleared removes them; same picture by contract.

### 4. The integrator (path_trace.hlsl, PtTracePixel)

One program in three variants, each as a ray-generation shader (shader execution reordering, `NvReorderThread` keyed
by material class) and as a compute shader with inline ray queries: plain, `PT_SSS` (skin scattering in screen
space) and `PT_SPLIT` (separate diffuse and specular radiance for a denoiser route that is not in this repository).
Reordering is used when NVAPI reports it (`rtc::PathUsesSer`); environment variable `RTC_PATH_COMPUTE` forces compute.

1. **Camera ray** (`PtHitTracked`). Camera rays respect authored face culling; transport rays see both sides.
   Cutouts test their cutoff. Translucent coverage strictly between 0 and 1 is taken against a hashed per-primitive
   threshold, so the expected colour is the alpha blend. Clear interfaces (glass, the eye's film: `PtClearInterface`)
   are passed without spending a bounce, up to 16.
2. **Guides** are written from the first sample's primary hit (`PtPrimaryGuides`, section 5).
3. **Direct light: one reservoir, one shadow ray per vertex** (`PtGatherDirect`, `PtResolveDirect`,
   `shaders/path_lighting.hlsli`). Proposals are streamed into a weighted reservoir (`PtDirectCandidate`): one
   analytic light picked by estimated incident power (`PtAnalyticCandidate`), one direction from the sky cube's
   distribution (`PtEnvironmentCandidate`), the indoor probe (`PtIndoorCandidate`), one emissive triangle
   (`PtEmitterCandidate`; triangles and their CDF from `src/path_lighting.inl`), and on first-hit skin the light
   through thin parts. The winner gets the one visibility query (`PtVisibility`, which attenuates through
   translucent layers and clear sheets). The light count does not multiply rays. No spatial or temporal reservoirs.
   Analytic lights are sampled over a disc of constant apparent size. Where a specular lobe is narrower than the
   disc (`2 a^2 < R/D`: a wet cornea, a mirror) a second proposal is drawn from the lobe and combined by the power
   heuristic in the same reservoir; without it such a surface was almost always black with a rare bright sample.
4. **Per-pixel visibility reuse** (`PtVisRate`, `PtVisUpdate`, test bit 31). At the first surface each pixel keeps
   12 bytes: per proposal source (analytic lights 0-8, the last slot shared; sky 9; indoor probe 10; emitters 11) a
   running rate of how often its winning pick was visible. Next frame that source's proposal weights are multiplied
   by the rate (floor 0.05). Only proposal weights change: unbiased, no extra rays. A pixel that moved more than 0.3
   input pixels starts again at "visible".
5. **Continuation** (`PtSample`). At most `[RayTracing] PathBounces` diffuse vertices and `PathView::maxBounces`
   vertices in all. Past the diffuse budget only specular lobes continue (mirrors, glass, gloss, hair); the diffuse
   light at that vertex comes from the light samples alone with MIS weight 1 (`PtMisLobes`). From the second bounce
   Russian roulette survives with `clamp(max throughput, 0.05, 0.95)`.
6. **Escape.** A ray that leaves the scene reads the environment (`PtEnvironmentLobes`): the visible background for a
   camera ray, otherwise the lighting cube and the ambient SH, MIS-weighted against the environment proposal.
7. **Indirect clamp.** Light gathered after a diffuse bounce is capped at luminance 4 on the output scale per sample
   (biased; under 0.1 % of the energy in the measured room). About one sample in 100 000 was 200-300 times the
   surface mean, and RR left a dark patch around each.

### 5. What Ray Reconstruction gets

`rtc::PathView` outputs at input size: `color`, `diffuse` (albedo guide; alpha = instance, negative for fibres),
`normal` (w = roughness), `specular` (specular albedo; alpha = material), `depth`, `hitDistance`, `motion`, and an
optional `detailReference`.

- **Depth and motion** belong to the same primary surface point, unjittered (`PtPreviousPosition`,
  `src/path_motion.inl`; skinned meshes and fibres keep last frame's positions, hair moves by its centre line).
- **Specular hit distance**: 0 means "lobe not sampled this frame"; a reflection that escapes reports 10000 m.
- **Sky pixels** carry specular albedo 0.5 (RR integration guide). **Attenuating layers** scale both albedo guides
  by their transmission; absorption is not specular reflectance.
- **Transparent stacks** (test bits 27, 29, 30). Where coverage varies with the seed (sheer cloth, lace), albedo,
  normal and roughness are blended over up to four layers by the share of frames the path takes each; with only the
  layer above coverage 0.5, RR resolved sheer cloth away as if it were light on the skin behind. Depth and motion
  follow the first layer that is a person (skin, clothing, mesh hair: `PathMaterial::mode` 256; fibres). See-through
  layers in front that are not a person (haze, light beams, glitter shells, glass) contribute neither; without a
  person layer the deepest layer wins. A pixel behind a floating translucent layer shows two things moving apart,
  and giving it the layer's motion made frame generation drag the face along ([motion-vectors.md](motion-vectors.md)).
- **Normal guide on skin** blends the diffuse normal and the pore-detail normal by the diffuse/specular share; with
  the pore normal alone RR printed pores as dark dots in still frames.
- **History resets** only on scene load, camera jump, a break in frame continuity and a renderer switch. Content
  changes (a material, a texture, an object appearing) do not reset; an earlier version reset on each, hundreds of
  times a session, taking ray reconstruction and the frame-generation guides with it.
- **Detail retention** (`[RayTracing] RRDetail`): a deterministic, dejittered material reference restores only
  contrast RR lost in the same direction; 0 turns the work and its resources off.
- RR runs before tone mapping; UI and replayed effects are composited after RR. One guide surface per pixel: a
  clear layer such as the cornea borrows the normal and roughness of the surface behind it.

### 6. People

- **Phong tessellation and seams** (`Submesh::tess`, `tessAlpha`, `tessLevel`). All regions of one character use the
  same (highest) level and Phong strength; per-region levels put different polylines on shared edges and opened dark
  seams at neck, upper arm and leg (cost: 231 136 -> 548 832 triangles for one character, frame rate not measured).
  Front/back tests and ray offsets use the normal of the sub-triangle actually hit; grazing visibility rays start
  near the smooth surface the vertex normals describe.
- **Silhouettes.** Where the shading normal faces away from the eye while the triangle faces it, `PtShading` bends it
  toward the triangle normal (below `dot(n, v) = 0.1`); otherwise the BSDF was 0: 1-2 pixel black gaps on outlines.
- **Skin model.** The 14 skin regions and lips use this mod's skin lobes with the raster shader's parameters
  ([skin.md](skin.md)). VaM's direct-diffuse gain `2(1 - S/2)` (`S` = `_SubdermisColor`) multiplies analytic light
  only (`directGain`); baked into the albedo it also amplified ambient, bounces and the RR albedo guide.
- **Subsurface scattering in screen space, before RR** (`PT_SSS`, `shaders/path_scatter.hlsl`, `PathScatterX`,
  `PathScatterY`). When a live skin material has scattering strength above 0, the trace writes, at skin the eye
  meets first, `strength x direct diffuse / albedo guide` and the depth along the camera axis; two separable passes
  blur it with the six-Gaussian skin profile (21 taps) and the result is added to RR's colour input times the same
  guide. No rays, no chroma noise. At strength 0 the plain variant runs, byte-identical to a build without the
  feature. A ray-sampled BSSRDF (`PtSampleSkin`, `path_skin_bssrdf.hlsli`) remains in the source for other variants.
- **Light through thin parts** (`PtThinCandidate`). At first-hit skin one light behind the surface is picked by
  incident power x facing; one opaque ray of at most 20 mm from just under the surface toward it finds the far side
  (ear, nostril, finger). Contribution: incident light x `max(N.-L, 0)` x `[General] Strength` x albedo x a
  per-channel Gaussian of the path length (sigma 2.5 / 1.45 / 0.85 mm) x a low-light fade, as one more proposal of
  the same reservoir. Nothing within 20 mm: no light. Strength 0: skipped.
- **Eyes** (eyeball materials carry `PT_EYEBALL`).
  - *Surface film*: the eye reflection and tear layers are clear sheets with roughness 0.03 and the reflectance of
    tear fluid (n 1.336, about 2.1 %), keeping the author's tint; authored 0 stays 0. With the authored gloss
    (roughness about 0.25) the catch light was a grey smear.
  - *Catch lights* come from the lobe proposal (item 3) and from environment and emitter samples passing clear
    sheets; before, an eyeball under its film had no sky light once the diffuse budget was 0.
  - *Eyelid occlusion* (`PtLidLight`): four cosine-distributed 12 mm rays per eyeball hit (lids block, lashes by
    coverage, hair too); the analytic lights' diffuse is scaled by the open share. Highlights keep their shadow ray.
  - *Eyelid band* (`PtEyeBand`, `rtc::SetPathEyeRigs`, `RecordPathEyes`, `RtPathEye`): the lid's rim, which the mesh
    has no thickness for. The host gathers each character's eye rig (centres, radii, lid contact lines) on the traced
    vertices every frame; the eyeball's albedo is multiplied by
    `(1 - upper u e^(-d^2/wu^2)) (1 - lower (1-u) e^(-d^2/wl^2))` along the lid lines. Diffuse only.
  - *Side falloff* (`PtDirectScale`): skin-family materials outside the skin model (sclera, iris, caruncle, mouth,
    gums) get VaM's full direct term `2(1-S/2) saturate(S/2+(1-S/2)N.L) ((N.L+1)/2)^2`, darkening the eye corners.
  - Iris and sclera highlights are added on the full albedo: authors set iris colour through the specular colour.
- **Teeth, tongue.** Teeth use direct gain 1; the tongue is a wet two-lobe surface (`TongueSurface.cs`, same folder).
- **Hair.** Swept-sphere strands with a far-field R / TT / TRT fibre model (`shaders/path_hair_bsdf.hlsli`,
  `third_party/rtxcr`). Multiple scattering among fibres is not traced: an analytic diffuse-cylinder lobe x the
  author's colour x `PT_HAIR_MULTIPLE` (1) shares the light sample and shadow ray. A hair hit spends one bounce; its
  continuation brings the room's light and stops at the next fibre (`afterHair`). Kept from the raster hair: colour
  to absorption, `_SpecularColor` to the R lobe, `_PrimarySpecular` to roughness, tip taper, shift to cuticle tilt.

### 7. Objects are adapted, never rejected

No single object may send the scene back to raster. An earlier version refused a scene on the first unsupported
material, and a HUD helper that exists only while a real controller is held locked whole sessions out. Adapters,
each logged once as `PT object adapter: ...` (`RtScene.PathNote`):

| Object | Handling |
|---|---|
| UI and editor shaders, world canvases | overlay layer (`path_overlay.inl`): the real draw is replayed against black and white; the difference is transmittance, the black one premultiplied foreground |
| Canvas elements with their own UI shader (grab-pass glass) | canvas textures are registered natively; draws sampling them are replayed as UI with the grab pass replaced by the backdrop (translucency kept, refraction not) |
| UI drawn after the compositor took the HDR layer | an output-stage layer composited after tone mapping |
| Unlit author shaders (light cones, additive sheets), particles | the blend, depth and cull state their draws really use is observed (`ST_GetPathObservedV1`, `PollObserved`) and they are replayed as effects, not traced; a light cone no longer becomes a black solid blocking its lamp |
| Lit shaders without a translation | a generic surface from their standard properties |
| Line and point meshes | effects overlay |
| Property blocks, custom light cookies | the material's own values; cookies not applied |
| More than 5 link classes, capacity limits | extra layers lit by all lights; objects past capacity stay out |

To add one: handle the object in `RtScene` and note it with `PathNote`. Never add a scene-level refusal.
### Material translation (read from the shader bytecode)

- `_DecalTex` (make-up, tattoos) is laid over the albedo by its own alpha and ST, not times `_Color`.
- `TransparentGloss...SeparateAlpha` clothing and the mimic-alpha hair program multiply colour and highlight by
  alpha, then blend SrcAlpha: alpha squared on screen. VaM's separate alpha is
  `saturate(_AlphaTex.a(uv0 x _AlphaTex_ST) + _AlphaAdjust)`; `_Color.a` is not read.
- Layered hair programs sample inside `_uvXMin.._uvYMax`. Mirror programs: reflection x `_ReflectionColor` plus the
  main texture's Lambert x `(1 - _ReflectionOpacity)`, no Fresnel. Emission: `_Illum x _GlowColor x _GlowStrength`.
- Unity Standard follows its keywords (maps replace scalars, smoothness x `_GlossMapScale`); author copies get the
  same semantics; the roughness setup reads roughness (`PT_ROUGHNESS_MAP`); all main maps use `_MainTex_ST`.
- A third-party nine-layer "enhanced eyes" clothing item is discarded under tracing (`PtReplacesEnhancedEyes`): its
  shadow shells stacked on the traced eye made it dark.

### Pipeline compile and texture residency

- **First-use compile.** `RecordPathTrace` creates the ray-tracing state object synchronously on the render thread;
  the driver compiles the whole pipeline and the window does not respond meanwhile. The driver caches by shader
  bytecode: once per shader change, and once more the first time a variant is used (scattering 0 -> above 0).
- **Texture residency** (`path_textures.inl`). Borrowing a D3D11 texture through 11on12 queues a synchronisation
  sequence per texture; about 80 material textures per eye left the GPU idle before each eye's trace. Every texture
  D3D11 can only write by copy, update or discard is copied once into a host-owned D3D12 texture and bound from
  then on; seven write methods of the immediate context are hooked and a write marks the texture for re-copy.
  Render targets are still borrowed, once a frame. No copy within 2 GB of the video-memory budget
  (`kPathCopyReserve`); first copies are spread at 16 textures or 512 MB a frame.

## Settings

| File / section / key | Values | Trades |
|---|---|---|
| `BepInEx/config/local.vam.skintransmission.cfg` `[RayTracing] Renderer` | `PathTracing` / `Legacy` (code default `Legacy`) | the whole route; followed live |
| same, `[RayTracing] PathBounces` | 0-3, default 0 | 0: lights and occluded sky only, the modelling a scene was lit for; 3: full bounce, flatter faces, more path time |
| same, `[RayTracing] PathBackground` | `Off` / `Far` / `All`, default `Far` | see the authored lighting document |
| same, `[RayTracing] RRDetail` | 0-100, default 75 | material detail restored after RR; 0 frees its resources |
| same, `[Subsurface] Strength` | 0-1 | above 0 selects `PT_SSS` |
| same, `[General] Strength` | 0 and up, 1 = measured skin | light through thin skin; 0 skips the step |
| RR input scale `RenderScaleRR` | [upscaling.md](upscaling.md) | path cost and detail scale with input pixels |
| `[Performance] PTReference` (`vam_performance_config.h`), Ctrl+Alt+R | 0 / 1 | diagnostic: running mean of the raw traced input while the camera is still |

`rtc::SetTestLayers` (`rtcore.h`) has one bit per behaviour for A/B work, all set by default: 19 micromaps, 20 lobe
proposal, 21 samples pass clear sheets, 22 film roughness, 23 skin-family falloff, 24 film reflectance, 25 eyelid
occlusion, 26 eyelid band, 27 blended guides, 28 Unity ambient per shader family, 29 / 30 guide depth and motion
layer, 31 visibility reuse and the indirect clamp.

## Measured

Development machine ([frame-budget.md](frame-budget.md)), simulated headset at 144 Hz unless stated; GPU times are
the trace per eye. Two sessions of one heavy scene differ by about 3 real fps: a single run is not a cost.

| What | Conditions | Result |
|---|---|---|
| First-use pipeline compile | desktop, new shader bytes | first frame 26.5 s; same bytes at the next launch 0.6 s |
| First switch to `PT_SSS` | simulated headset | about 22 s stall, cached afterwards |
| Fibre scattering traced (up to 64 events) -> analytic lobe | face close-up filling the view, RR input 50 % | 6.5-6.9 / 5.1-5.2 ms -> 4.5 / 3.8 ms (the two eyes) |
| same | same view at RR 33 %; heavy two-character dance scene at RR 33 %, frame generation off | 3.7-4.0 / 2.8-2.9 -> 2.4 / 1.8 ms; 2.04 / 1.73 -> 1.51 / 1.29 ms |
| `PathBounces` 0 against 3 | face close-up, RR 50 % | 2.88 against 3.55 ms |
| Texture residency | heavy dance scene playing, RR 33 %, GPU event tracing on | frame generation off 42.0 -> 51.8 real fps; 4x 32.5 -> 36.0 (the cap at 144 Hz); GPU idle per frame 3.09 -> 0.88 ms; 83 textures, 1286 MiB |
| Opacity micromaps | same scene; simple outfit / outfit of 25 clothing items (148 k triangles covered) | no measurable difference / about -0.19 ms a frame (4 % of path time) |
| Screen-space scattering; thin light | face close-up, RR 50 % | about +0.1-0.2 ms; within noise |
| Visibility reuse + clamp | a daylight interior with a person, fixed camera | frames with zero direct light on the person 47 % -> 24 %; RR pixels darker than the accumulated reference 2.65 % -> 0.45 %; mean unchanged |

Cost drivers: input pixels (RR scale), the share of the view filled by hair and skin, the diffuse bounce count for
whatever keeps its budget, translucent layers per ray. Lights cost selection work, not rays. Outside the trace, the
swept-sphere build handles roughly 0.9 million segments a frame for a heavy hairstyle (conditions not recorded).

## Pitfalls

- **The game freezes about half a minute when tracing first starts after a shader change**: the driver's pipeline
  compile. After changing a traced shader, run one session to the first traced frame before handing over.
- **`Material doesn't have a texture property ...` every frame.** VaM swaps a material's shader and the material
  keeps the old properties; Unity 2018.1's `HasProperty` says yes. Read properties only through `RtScene.Declared`.
- **Statically batched objects look untextured.** Unity's vertex compression stores tangents and UVs as half
  floats; `UvFor` picks the layout from the buffer's byte size. Check this before suspecting materials.
- **Traced textures go stale.** The tracer binds its own D3D12 copies; a new way of writing a texture (a deferred
  context, another plugin writing through D3D12) bypasses the hooks in `path_textures.inl`.
- **Static dark speckles that vanish when the camera moves.** Separate tracer from reconstructor first with
  `PTReference`: a smooth mean means a heavy-tailed input (measure the share of zero frames and max / mean), not a
  wrong guide.
- **A second stereo camera in VR** (a scene's UI camera with stereo enabled) was taken for an eye: the record was
  filtered by its culling mask, most of the stage never reached the tracer, and frame rates looked far too good.
  Only the active stereo centre camera is an eye now; check `RtScene.Status()` before trusting a VR frame rate.
- **New tracer inputs** get their own RtCore call (as `SetPathEyeRigs`); `PathView` is an ABI with the compositor.
- **Offline bounce contracts** need a purely diffuse wall: a glossy one reflects, so it beats direct-only at 0 bounces.
- **Negative `_AlphaAdjust` is not always sheer cloth**: -1 is how authors hide parts. Hidden parts on the
  separate-alpha skin-family programs stay out of the traced scene (`DrawsNothing`); lifting alpha globally reveals
  them.

## Tried and dropped

- **Tracing fibre-to-fibre scattering outside the bounce budget**: right brightness, about 2 ms per eye when hair
  fills the view. Its offline-fitted gain for the analytic lobe (1.739) looked too bright in the game; 1 is used.
  Counting it inside a 4-bounce budget truncates it and breaks the colour-to-absorption conversion (light hair kept
  18 % of its brightness offline). Adjust brightness only through `PT_HAIR_MULTIPLE`.
- **Catch lights**: a smaller disc for highlights (fits one scene); specular continuation hitting the disc with MIS
  (no visible change); corneal curvature (the catch light did not move and shrank); removing the iris's own
  highlight (an authored pink iris turned blue); 2 mm of eyelid thickness (not visible).
- **Static speckles**: a nearest-hit ray per skin pixel to stabilise guides (too costly) and an RR responsivity mask
  (no visible effect; `PathResponsivity` remains in the API).
- **Ray-sampled skin scattering without extra rays**: 4-5 % darker with an unexplained blue shift. **Solid shadows
  under a person's sheer clothing**, as Unity's shadow caster gives: rejected on look.
- **Guessing outline shells from geometry**, and making every cast-shadows-off object camera-only: too broad;
  `RtScene.CameraOnly` matches one known shell by name and parent.
- **Shadow-terminator correction**: hardly visible on characters. **BVH tuned for traversal**: no gain (shading
  dominates path time).

## Open

- Most look changes have not been judged by a person in a real headset. The pipeline compile still runs on the
  render thread. Sheer clothing lets more light onto the body than raster (shadow rays attenuate by `1 - alpha`).
- Hair with the analytic lobe is brighter in the game than the offline fit predicted (cause guessed: the lobe
  favours front light); a single strand's effective albedo can exceed 1.
- Mouth-corner dark spots and a seam on one upper arm were reduced, not confirmed gone. Eye corners remain brighter
  than in raster; lashes are not the cause.
- `PathTracingAvailable()` needs NVAPI linear-swept-sphere support (per the development notes a Blackwell-generation
  GPU); behaviour on an older card was not run.

## Verify

- **Offline contract probes** (`components/shading/rt-core/tests`). `build.cmd` (needs `VAMKIT_VCVARS`, `VAMKIT_DXC`)
  builds `out/rt-core-tests/probe.exe`; run `probe.exe <output dir>`; exit code 0 = pass, results in `result.json`
  and one JSON per group. It runs RtCore on the D3D12 debug layer against small analytic scenes. `probe.cpp`: direct
  and four-bounce means, sky rotation, glass, clear-film camera motion, fibre motion across the 16-frame rebuild,
  whether reordering is active. `contract_probe.inl`: coverage and guides, attenuation and additive layers,
  thin-sheet stacks, authored tangents, camera culling, address modes, specular references taken from VaM's and
  Skyshop's bytecode, decals, premultiplied alpha, separate diffuse and reflection scales, hidden sky, shadow-only
  proxies, the range window, light linking, RR guide rules, hair volume brightness, camera-hidden geometry and
  fibres, grazing normals, the eye rules. Also `lighting_probe.inl`, `material_probe.inl`, `omm_probe.inl`
  (micromaps on and off agree), `split_probe.inl`, `fiber_probe.inl`, `motion_size_probe.inl`, and
  `shadow_probe.cpp` (`build_shadow_probe.cmd`). They reject wrong cross-stage data; they cannot approve a look. A
  change meant to be a no-op is checked by byte-identical probe output, not by eye.
- **In the game only**: the look, the first-use compile, UI overlays, real materials. `RtScene.Status()`,
  `PathMaterials()`, `PathMeshInventory()` and `PathBackground()` report what was recorded; the log lists adapters.
  See [../protocol/testing.md](../protocol/testing.md) and [../../tools/lab/README.md](../../tools/lab/README.md).
