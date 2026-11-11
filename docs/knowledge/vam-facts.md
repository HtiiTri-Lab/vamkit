# Facts about VaM and its Unity build that shaped the design

Things a newcomer cannot guess from the code or from Unity documentation, found by reading VaM 1.22.0.3's IL, shader
disassembly and in-game readbacks, and that decided how the mod is built. Each item says what it is, how it was
established and where it matters. Platform: Unity 2018.1, legacy Mono (a 2018 `mono.dll`, C# 3 / .NET 3.5 class
library), built-in forward renderer, Windows, D3D11 (the mod's 11on12 layer is described in [d3d11on12](d3d11on12.md)).

## Plugins and Mono

- **`[DllImport]` resolves only from `VaM_Data/Plugins`.** A native library that a BepInEx plugin has already loaded with
  `LoadLibraryW` from `BepInEx/plugins` is not found by a `[DllImport]` there (`DllNotFoundException`); one in
  `VaM_Data/Plugins` (for example `openvr_api.dll`) is. So plugins here load their natives themselves:
  `LoadLibraryW(full path)`, `GetProcAddress`, `Marshal.GetDelegateForFunctionPointer` into an
  `[UnmanagedFunctionPointer(Cdecl)]` delegate field (`components/runtime/hair-solver/plugin/VaMHairSolver.cs`,
  `BindNative`; `components/runtime/gc-tuning/VaMGcTuning.cs`, `Native`). Arrays, struct arrays, `StringBuilder`, `out` /
  `ref` structs marshal correctly this way.
- **Name plugin natives `.bin`.** BepInEx reads every `.dll` under `BepInEx/plugins` as a .NET assembly and logs an error for a
  native one. The build scripts copy `X.dll` to `X.bin` (`components/runtime/cpu-boost/native/build.cmd`). Other native
  code finds a loaded `.bin` module with `GetModuleHandleW(L"VaMCpuBoostNative.bin")`.
- **Plugins are C# 3 for .NET 3.5.** The language level is C# 3 (no optional arguments, `dynamic`, `async`, string
  interpolation or `?.`) and the class library is .NET 3.5; build with the .NET 3.5 compiler (the repository's
  build scripts do) and `/unsafe` where a plugin passes pointers. Headers of the plugin sources say "C# 3".
- **Scene and in-game scripts are compiled by VaM itself** (its own `mcs`-based compiler, every time a scene loads, from
  source) and may not use the `System.Reflection` namespace: even `GetType().Name` is refused; use `is`. BepInEx plugins
  are compiled by us and are not under that restriction (`CpuBoost` uses `System.Reflection` and Harmony freely).
- **Mono methods that allocate** (matters for [gc](gc.md)): `BitConverter.DoubleToInt64Bits` (new `byte[]` per call),
  `Regex.IsMatch`, `Shader.name`, `Material.name`, `Material.GetTag`, `Renderer.sharedMaterials` (2018.1 has no
  `GetSharedMaterials(List)`), `FieldInfo.GetValue` on value types, `foreach` over `System.Array`.
  `long.GetHashCode` is high XOR low, so `(a << 32) | b` as a dictionary key collapses into collisions.
- **`UnityEngine.Object` `==` goes through the native object.** An object created without Unity (offline tests) compares equal
  to null; use `ReferenceEquals`.
- **Boehm GC never moves objects**, so native code may hold an address of a managed array for the duration of a call that
  keeps the array referenced ([cpu](cpu.md)). Mono's JIT evaluates float expressions in double and rounds on store, and
  `Math.Sin/Cos` are x87 instructions ([cpu](cpu.md)).
- **Unity's player reads window messages once per frame** (`PeekMessage` in the main loop). Windows judged VaM "not
  responding" and replaced its window with a "ghost" window although the main thread was presenting normally and answered sent
  messages at once; with no new input it re-judged about every 5 s (a flash to the desktop about every 6 s in the log). With a
  flip-model swap chain the ghost has nothing to show. Seen when the mod took over presentation on the desktop. Only *retrieving*
  a message counted as responding. The desktop
  presenter posts a `WM_NULL` to the game window about once a second (Unity's peek takes it) and calls
  `DisableProcessWindowsGhosting` in the VaM process (`components/render/d3d11-proxy/desktop_presenter.cpp`, the keepalive
  and `VamDesktopInstall`). Also relevant to long scene loads, which can hold the main thread for tens of seconds.

## Morph runtime (from the IL of VaM 1.22.0.3)

To read IL, decompile `Assembly-CSharp.dll` (any decompiler, or Mono.Cecil queries for "who calls this member"). To find who
writes a value at run time, a Harmony prefix on the setter that records `new StackTrace(2, false)` is quicker than reading.

**Where a morph's value lives.** `DAZMorph.morphValue` reads `_morphValue`; `appliedValue` is the value the character
thread last really applied. The write chain: `set_morphValue` -> `jsonFloat.set_val` -> `JSONStorableFloat.InternalSetVal`
-> `DAZMorph.SetValueInternal(float)`, which is called **only if the value changed**. `SetValueInternal` does `AdjustRange`,
`_morphValue = v`, `activeImmediate` and **does not write back `jsonFloat.val`**: a prefix that changes its argument makes the
mesh use the new value while `jsonFloat.val` keeps the old one. `SetValue(v)` calls `SetValueInternal` only if `jsonFloat.val`
differs from `v` and does not write `jsonFloat.val` either; a test that edits a morph must set `m.jsonFloat.val = v`, otherwise
`SetValue(.5)` then `SetValue(0)` may skip the restore. `SetValueThreadSafe` writes `_morphValue` directly. Formula targets
(`isDriven`, `drivenBy`) are written inside `ApplyMorphsThreaded(Fast)` / `ResetMorphsFast`. Formula target types: MorphValue 0,
BoneCenterX/Y/Z 1-3, OrientationX/Y/Z 4-6, GeneralScale 7, ScaleX/Y/Z 8-10, MCM 11, MCMMult 12, RotationX/Y/Z 13-15; MorphValue
targets `clamp(multiplier * source)`, MCM sets this morph to source * multiplier, MCMMult multiplies this morph's applied value
by the target morph's value, targets found by name with `GetMorph(name)`. MCM correction morphs have an applied value only
when the matching character morph has a value: to predict a driven morph's rest value, multiply by the MCMMult factor.

**Who writes morphs while the game runs:** Timeline (the `geometry` storable's float parameters named `morph: <display
name>` / `morphOtherGender: <display name>`; unchanged value = no callback; a stopped Timeline writes nothing);
`DAZMeshEyelidControl` (eight eyelid morphs); `SetDAZMorph` joint / distance / angle drivers (body correction morphs, about 14),
whose `DoUpdate` runs on the **main thread** (`DAZCharacterRun.PrepRun` from `Update`, and `Connect`) through
`SetValueThreadSafe`; `SpeechBlend` (16 visemes plus mouth open); `SetDAZMorphRandom` (automatic expressions); `ApplyMorphsInternal`,
`RefreshPackageMorphs`, `ResetMorphs`, `DAZMorphSet.ApplySet`, `DAZSkinWrap.SetMeshMorphsFromUsedValue` (clothing's bank), the
morph pages' Zero/Default buttons and DAZ import. A package morph that finishes loading late makes `RefreshPackageMorphs` write
the restored value again, outside any restore: hooking only the restore path mistakes these appearance values for run-time
changes.

**Loading and restoring.** `SuperController.isLoading` is true while a scene loads and false when an appearance is loaded alone.
Morph restore goes only through `DAZCharacterSelector.RestoreFromJSON` and `GenerateDAZMorphsControlJSONStorable.RestoreFromJSON`
(non-merge: reset to default first, then `GetMorphByUid`, falling back to `GetMorphByDisplayName`, then
`DAZMorph.RestoreFromJSON` = `jsonFloat.val = value`). Appearance presets go through the Person's `AppearancePresets`
(`MeshVR.PresetManagerControl`), `PresetManager.LoadPreset` -> `Restore` -> each storable's `RestoreFromJSON(JSONClass,
restorePhysical, restoreAppearance, presetAtoms, setMissingToDefault)`; `JSONStorable.mergeRestore` marks merge loads. **Pose
morphs do not follow the appearance:** restore and reset split by `isPoseControl`, pose-type morphs follow `restorePhysical`
only, the rest follow `restoreAppearance` only (built-in expressions such as Mouth Open and Smile Full Face are pose-type; so
are some creator-made ones). `SELF:/...` in a package means that package's own `Creator.Package.Version:/...`; a scene copied out
of a package and saved elsewhere has to have them replaced.

**Display names repeat.** A package morph can share a built-in's display name (`Left Fingers Grasp`). Map a parameter name to a
morph through `sel.GetFloatJSONParam("morph: X")` and then find the morph in `morphsControlUI(OtherGender).GetMorphs()` with the
same `jsonFloat`; `GetMorphByDisplayName` can return the wrong one.

**The merged-mesh pipeline** (character thread, `DAZCharacterRun.RunThreaded`): `ApplyMorphsThreadedFast` ->
`mergedMesh.UpdateVerticesPrepThreadedFast` -> `UpdateVerticesFinishThreadedFast(m1, m2, m3, merged, alt=false)` -> copy to
`mergedMeshMorphedUVVerticesCopy` -> `SkinMeshPostVertsThreadedFast(copy)` (about 500 head vertices skinned to world space in
place) -> `SkinMeshThreadedFast(merged)`. `merged` is re-copied from mesh 1 each frame, so in-place edits do not accumulate.
`SecondaryMergeVerts` runs concurrently on another thread (the "task2" thread), calls the same Finish with `alt = true` on the
visible non-pose morph array: a hook on Finish must skip that call. Reading the copy from outside is unreliable (partly skinned
in place and being rewritten). `PrepRun` callers: `DAZCharacterRun.Update` (main thread), `Connect`, `SmoothApplyMorphs(Lite)`;
`Connect` swaps `mergedMesh` / `morphBank1`, and a morph library attaches to the mesh only after a few `PrepRun` calls, so compare the
three references when registering. `atom.uid` is still "UID" at the first `PrepRun`.

**What a plugin must not do with morphs.**
- Call `DAZMorph.LoadDeltas` from two threads: `LoadDeltasFromBinaryFile` allocates the array first and then fills it, and a
  second thread (or the main thread reading) sees a half-filled array with empty entries. A plugin that guards morph arrays
  must treat an array with null entries as "not known yet", let the call through, remember nothing, and decide on the next
  write; for a morph not yet loaded, read the file itself into a private array rather than calling `LoadDeltas`.
- Assume `SetValue` updates the UI value (above).
- Trust a copy of the merged vertices read from another thread.
- Use `System.Reflection` in an in-game script (above).

## Lights and exposure

- **Lights have no size.** Unity's built-in realtime lights are point sources; the game's `AdjustLightV2` chooses the shadow
  type automatically (`autoShadowType`, default on): point and spot lights are forced to **Hard**, directional lights to
  **Soft**. Readbacks (prefab values override the script defaults): spot near plane 0.25 m, bias 0.002, shadow map 2048 at the
  Medium quality setting; point light near 0.1 m, bias 0.014. The shadow edge is about one shadow-map texel (about 1 mm at 1.3 m),
  and a blocker closer than 0.25 m to a spot light casts no shadow. Scene JSON has no key for shadow type. Quality settings: shadow
  resolution Medium, `pixelLightCount` 6 (`ForcePixel` lights are not limited by it). Light `renderType` defaults to Auto;
  Auto lights beyond `pixelLightCount` and `ForceVertex` lights fall to vertex lighting / spherical harmonics, and the skin's
  ambient term carries them (about 6% of scenes in a 717-scene survey have more than 6 lights).
- **VaM's skin shader treats `shadowStrength` as a blur radius, never as depth.** On every surface the shadow is always 100% dark;
  the strength only sets how soft it is: Hard shadows take 25 PCF taps with radius `0.025 * strength` in shadow-map UV (point
  lights `(1 - x) * 0.1 m`); Soft shadows use PCSS (16 blocker taps) then 25 comparisons. At strength 1 with a wide cone this smears
  shadows 12-21 cm on a person: nose, eye sockets, chin and under-breast self-shadow vanish, which is why VaM people have "no
  self-shadow". Shadow cast/receive switches default on and are not the cause. A scene that omits `shadowStrength` gets 0.2, not 1
  (JSON stores only non-default values), so a missing key means a soft shadow. The path tracer therefore treats point and spot lights
  as an effect light with a fixed apparent size (8 degrees, `RtScene.PtLightAngular`) and a directional light as the sun
  (0.27 degrees); see [path-tracing-authored-lighting](path-tracing-authored-lighting.md).
- **Intensity is not linear.** Linear colour space, eye cameras LDR, forward, no tone mapping: values over 1.0 clip.
  `GraphicsSettings.lightsUseLinearIntensity` is false, so final linear light is about `(colour * intensity)^2.2`: intensity 0.26
  is 0.05, 2.0 is 4.6. Light colours are gamma too ((1, .955, .89) is about 5500 K).
- **Attenuation:** `1 / (1 + 25 * (d / range)^2)`; a person at a quarter of the range gets 0.39. Forward spot lights (measured at
  cone angles 60-160 degrees): `cookie(r) * 1/(1 + 25 q) * smoothstep(1, .64, q)` with `q = (true distance / range)^2` (not
  depth) and `r = tan(angle) / tan(half cone)`; the default cookie `Soft` (128 x 128, Alpha8) is 1 up to r = .63, .518 at .82,
  about 0 at .99. Grazing residual error is the BRDF: Standard uses `_WorldSpaceCameraPos` for the view vector.
- **Sky and ambient (Skyshop).** `masterIntensity` multiplies image-based lighting; `unityAmbientIntensity` becomes `_ExposureLM.x`
  and does **not** multiply `masterIntensity`; the skin's flat Unity ambient is `ambientLight (to linear) * _ExposureLM.x`;
  `camExposure` becomes `_ExposureIBL.w` and multiplies the direct light of Marmoset shaders only (hair and others are not
  affected). Defaults when a scene's `GlobalLighting` omits a key (read from the level's global sky; verified against stored value
  distributions and runtime readbacks): masterIntensity 1, diffuseIntensity 1, unityAmbientIntensity 1, unityAmbientColor black,
  camExposure 1. Counting a missing key as 0 gives wrong statistics: 59% of scenes store an explicit 0, 28% omit the key and so
  have full-strength museum sky.
- **Diffuse ambient depends on the shader family** (disassembly):

  | Family | Diffuse ambient |
  |---|---|
  | `Custom/Subsurface/*`, `Custom/Hair/*`, `Skin IBL` (skin, clothing, mesh hair) | `abs(L0 + f1 L1 + f2 L2) * (1 - _IBLFilter)`, L = Skyshop SH(`_SkyMatrix`^T n) * `_ExposureIBL.x` + Unity SH * `_ExposureLM.x`; `f1 = a + S(a^2 - a)`, `a = 1 - S/3`; `f2 = b + S(b^2 - b)`, `b = 1 - 0.75 S`; S = subsurface colour |
  | `Marmoset/*` | `abs(Skyshop SH) * IBL.x + max(0, Unity SH)` |
  | everything else (Standard, ...) | Unity SH only |

  Specular reflection reads the global Skyshop `specularCube` times master times specular (non-HDR cubes as RGBM), except that
  Unity's Standard takes its reflection from `unity_SpecCube0`, not from Skyshop. The visible skybox draws the sky's own
  `skyboxCube` with brightness `skyboxIntensity * camExposure` (not master), independent of the specular cube.
  VaM writes only `RenderSettings.ambientLight` at run time, never `ambientMode` or `ambientProbe`, so a plugin can set
  `ambientMode = Custom` plus an `ambientProbe` (`components/shading/display-lighting/VaMDisplayLighting.cs`). Skybox ambient
  mode does not work: VaM scenes have no skybox material, and in that mode every draw that uses the probe gets only the flat
  ambient colour. The level default is Flat, probe all zero, no light probes. `SphericalHarmonicsL2.AddDirectionalLight` takes
  the direction *towards* the light.
- **Stock skin shader** (`globalgamemanagers.assets`): diffuse = `2 (1 - S/2) * saturate(S/2 + (1 - S/2) N.L) * ((N.L + 1)/2)^2` per
  channel, so the dark side gets a flat floor and then a hard cut; one normalised Blinn-Phong lobe plus Fresnel; Skyshop IBL plus
  spherical harmonics whose orders 1 and 2 are attenuated by S; **no transmission term at all**. `_TranslucencyColor` /
  `_TranslucencyTex` are set only by DAZ import and have no UI. VaM and the denoiser both show no ear or finger light-through; with the
  neural denoiser on (a simulated headset, backlit) skin came out 20-30% darker with red and green up 4-8%.
- **Semi-transparent clothing** (disassembly): `Custom/Subsurface/Transparent*` clothing shaders have no ShadowCaster of their own and fall
  back to the `TransparentCutoutSeparateAlpha(ComputeBuff)` caster, which only discards pixels with
  `saturate(_AlphaTex.a + _AlphaAdjust) < _Cutoff`; clothing materials have `_Cutoff` 0.001, so **sheer cloth casts a solid shadow** and
  authors tuned Alpha Adjust for that. The cloth itself: the ForwardBase pass (main directional plus ambient) multiplies by alpha and
  blends SrcAlpha, giving `alpha^2 * C`; the ForwardAdd pass (point, spot, other directionals) multiplies by alpha with One One,
  giving `alpha * C`. An author hides a clothing part by setting Alpha Adjust to -1 on a transparent-queue material: such parts are
  still drawn into the transparent queue.
- Other engine facts: a `Skybox/Panoramic` sphere (queue 1000) is drawn first without depth write; `Nature/Tree Soft Occlusion*` is
  vertex-lit and its passes have no names (so "unlit" cannot be inferred from a pass name); `LightType.Area` is bake-only; a light
  with `RhythmAudioSource` attached keeps resetting its colour to the start colour through trigger actions even when silent.

## What VaM's own rendering costs

- Built-in forward, single-pass double-wide stereo, eye cameras LDR. Skin shaders (`Custom/Subsurface/*ComputeBuff`) have
  ForwardBase, ForwardAdd and ShadowCaster; hair shaders (GPUTools / MeshedVR `HairOpt`) have only ForwardBase and ShadowCaster, so
  every shadow map and the depth pre-pass redraw the hair, tessellated. Hair shadow casting comes from
  `HairShadowSettings.CastShadows`, an asset value the UI cannot change. The `generateDepthTexture` preference triggers a depth
  pre-pass; the mod's DLSS reads the bound depth buffer and does not need it.
- The `HairOpt` pipeline: vertex shader (by vertex id) -> hull shader isoline, 3 guide strands per patch, tessellation factor from a
  constant -> domain shader (particles stride 56, barycentric stride 12 times 64 per strand, side vector from the camera position, so it
  changes with view) -> geometry shader quad -> pixel shader. D3D11 facts measured on a desktop GPU: `SV_VertexID` is the raw index
  (`BaseVertexLocation` is not added); index 0xFFFFFFFF restarts a patch-list assembly; isoline domain coordinates are fixed point
  (within 1.9e-4 of k/n); integer partitioning is `ceil(clamp(f, 1, 64))`, `f <= 0` culls; stream output writes odd strip
  triangles as (1, 3, 2). A native rewrite of the geometry stage matched triangle-for-triangle (relative error up to 1.2e-4) but
  the geometry is only 10-45% of the hair pass and the best replacement was 0.91-1.16x, so it was not integrated
  ([rejected-and-open](rejected-and-open.md)).
- The window camera atom (`WindowCamera`): `MainWindowCamera` (target eye None, desktop window size) plus `MiniCamera` (a render
  texture feeding the atom's in-scene display). With the camera on, each costs one extra full scene render per frame with its own
  shadow maps. In VR the mod switches both off (`components/render/window-camera`); on the desktop, `MainWindowCamera` is also off
  because the mod's desktop output overwrites what it draws.
- Scene library survey (725 scenes): a median of 4 shadowed lights per scene, 38% with point-light shadows, 17% of shadowed lights
  with `shadowStrength < 0.25`. The default `InvisibleLight` prefab: point, Hard shadows on, range 5, shadow resolution Medium.
- Rough budget of a heavy dance scene at the time (3x frame generation, GPU 95%): about 32 ms a real frame, of which the mod's
  post-processing about 13 ms and VaM's own about 18-22 ms (estimate). Current numbers: [frame-budget](frame-budget.md).

## Reading materials Unity 2018.1 gives you

- **Changing a material's shader leaves its old properties.** After VaM swaps a character's materials to `ComputeBuff` shader
  variants, a material still carries the previous shader's properties: `HasProperty("_AlphaTex")` is true but `GetTexture`
  logs "Material doesn't have a texture property". Read a property only if the material's *current* shader declares it. A
  freshly created material of the same shader has exactly the shader's properties, so the scene recorder asks that once per
  (shader, property) and caches the answer (`components/shading/skin/RtScene.cs`, `Declared`). A remaining family of
  `_AlphaAdjust` float warnings did not come from there; its source is unknown.
- `Texture.updateCount` / `IncrementUpdateCount` exist: a CPU texture update refreshes the native pointer by count; a texture
  written by the GPU needs the author to call `IncrementUpdateCount`; render textures are handled per frame.
- Native code that changes material slots, or `Renderer.material` instantiating implicitly, can bypass managed setters, so the
  scene recorder re-checks the actual material slots of every registered renderer each frame (only those registered; no scene
  scan).

## Person controllers and joints

- A controller (`FreeControllerV3`) drives a bone through a `ConfigurableJoint` between the bone's rigidbody and a kinematic rigidbody
  at the control point (anchor at the bone origin): x/y/z drive = hold-position spring / damper / max force; slerp drive = hold
  rotation times a power scale; Comply multiplies by the rigidbody mass; Off sets them to 0. The control point's rigidbody is
  teleported (`set_position` / `set_rotation` in `UpdateTransform`), so PhysX sees it at rest and the drive damper acts on the bone's
  **absolute** velocity: without feed-forward the bone lags by damping / stiffness. Hold values are unconstrained: a value above the
  slider maximum works. Defaults: position 2000 / 35, max force 1000; rotation 100 / 5, hip, head and foot 250 / 5; pelvis, abdomen, neck,
  collar, shoulder, thigh and toe controllers default to Off. Moments of inertia are set explicitly (torso, thigh, upper arm, forearm
  0.1 kg m^2; neck, head, hand, foot 0.01; shin 0.05; toe 0.001; **collar 1.0**), so a collar at default drive has a 1.6 Hz,
  0.25-damped spring under the shoulder girdle. Main bones weigh 26.3 kg plus 290 attached rigidbodies (fingers, wrist bones, glutes,
  chest, jaw, tongue, collider joints) of 37.6 kg, 274 of them without gravity. Solver iterations 15 (20 per rigidbody), velocity
  iterations 1; angular drag mostly 10, collar 1.
- Timeline in RealTime mode advances time and samples controllers in `AtomAnimation.FixedUpdate` (the physics step), so every physics step that runs inside a frame samples once;
  the rendered pose is the rigidbody interpolated to the frame time. The phase of a physics step relative to a frame is random per launch.
- `FreeControllerV3.RestoreFromJSON` applies `position` as a world position and skips `localPosition` when both exist (same for rotation).
  Only `FreeController` has this precedence; a `DAZBone`'s position / rotation are local poses.

## Asset loading, for context

The mod's own load-time work is not in this repository (the development tree carried a scene-load and an image-load plugin); the facts
below explain why those loads are slow and why parts of this repository (the gc-tuning finalizer rule, the CpuBoost pool) mention loading.

- **Image loader.** `ImageLoaderThreaded` has one background worker that handles the head of the queue only; the main thread waits for
  it, finishes (create the Unity texture, `Apply`, `Compress`, write the cache, callbacks) one image at a time before the next
  starts. File reads, bitmap decoding and pixel transforms can be parallel; texture creation and callbacks must be on the main
  thread; the cold-cache path also writes the cache synchronously. It covers character and clothing textures and thumbnails, not the
  synchronous `ProcessImageImmediate` and textures embedded in asset bundles. A character file lists all its textures at once and the
  JSON restore is a synchronous loop, so requests need not wait for the previous image; some derived textures do depend on the
  previous one. Unity 2018.1's CPU mip filter (`Texture2D.Apply(true)`, RGB24 and RGBA32, sRGB or linear alike): truncated average of two
  horizontal neighbours, then the truncated average of two rows, `((a + b) >> 1 + (c + d) >> 1) >> 1`, and only the other direction
  when one dimension is 1; ordinary rounded or truncated 2 x 2 averages are wrong by 69% / 15% of texels.
- **Asset-bundle loader.** VaM's `AssetLoader` runs one coroutine that serves one request at a time; an LZMA-compressed bundle inside a
  `.var` is decompressed whole on Unity's only loading thread; there is at least one frame between requests, and a disabled custom
  unity asset still loads its bundle. Bundles are read from the `.var` on another thread into a `byte[]` (not the main thread's cost;
  the main thread's cost is Unity finishing the bundle's contents synchronously, such as audio). Unity 2018.1 refuses to open the same
  bundle twice at once (the second returns null), refuses an uncompressed bundle whose block info is not really LZ4-compressed
  (smaller than the original) with "Unable to read header from archive file", and reading `AssetBundleCreateRequest.assetBundle`
  completes an async load synchronously on the spot. FMOD stores `DecompressOnLoad` Vorbis as PCM16 (`(int)(x * 32767)` after
  clamping to +-1, truncating towards zero), cannot take a resource node larger than about 4 GB ("Unsupported file or audio format")
  and Unity often reads a clip twice in a row. A scene that preloads hours of voice-over this way makes Unity decode gigabytes of PCM
  on the main thread in one frame.
- **Scene plugins load slowly because VaM recompiles every plugin instance from source at every scene load** (no compile cache), and
  each load produces a new assembly, so Mono JITs everything again (Mono 2.x holds a global JIT lock and runs static constructors).
  A whole scene's plugin phase can sit in one frame (the game window is "not responding" for it). After a scene is cleared about 10 GB
  of private memory stays for the next scene: the managed heap only grows and half of it is free; this is reuse, not a leak.
  Starting atoms (such as the window camera) that are deleted while a sub-scene clears and stay in `startingAtoms` make later loads
  throw "Destination array was not long enough", and an exception in a plugin's atom-removed callback aborts the clear because VaM does
  not catch it.
