# Tone mapping and display lighting

VaM's eye camera renders in low dynamic range with no tone mapping: anything brighter than 1.0 is cut off flat. That is
the floor under several things a player notices: a strong back light can only be raised to about the key's brightness
before it becomes flat white blocks, highlights do not roll off softly, bright edges have no glow, and scenes lit by
bright coloured lamps clip to saturated primaries. Two components address this. **Tone mapping**
(`components/shading/tone-mapping`) renders the main camera in HDR and applies a highlight shoulder (or a film curve) plus
a small warm bloom. **Display lighting** (`components/shading/display-lighting`) is an optional plugin that swaps a
scene's lamps for a one-click studio rig around the subject, in eleven styles, tuned from a survey of authored scenes.
They are independent plugins; tone mapping can run without display lighting and is most valuable where lamps are bright
enough to clip.

## How it works

### Tone mapping

Files: `plugin/VaMToneMapping.cs` (BepInEx plugin `local.vam.tonemapping`), `native/tonemap.cpp` (native library
`VaMToneMappingNative.bin`, API version 2), `shaders/tonemap.hlsl` (compute), `tools/build_shaders.py`.

1. **Camera.** `LateUpdate` finds the main camera (`CameraTarget.centerTarget.targetCamera`, the stereo eye camera in VR)
   and sets `allowHDR = true`, remembering the previous value. The setting is restored when the effect is off and RR is
   not running. HDR is also forced whenever DLSS Ray Reconstruction is active, whatever `Mode` is, because RR needs it
   (the bridge reports this as state bit 16384, `VaMDLSS_GetState`).
2. **Effect.** While tone mapping is wanted, a `ToneMapEffect` component is added to the camera. Its `OnRenderImage`
   sets the camera's source target as the render target, issues the native render event with the eye count, then blits
   to the camera's output. The native side reads the bound target, runs its passes in compute into a texture of its own
   and copies the result back in place. Pipeline state it touches (compute slots, output-merger targets) is restored.
   A target that is not a single-sampled float 2-D texture is left alone and counted (`Report()` shows the native
   `format` and `skip` counters). Both eyes may share one wide target; every read is clamped to its own eye, so neither
   the blur nor bilinear reads cross the seam.
3. **Where it runs (this is the part that changes with the render route).**
   - *Path tracing on (DLSS RR F)*: the plugin passes its event pointer through `VaMDLSS_WrapToneMapEvent` (exported from
     the compositor, `components/render/opencomposite/OpenOVR/Compositor/dx11_vam_dlss.cpp`). The wrapper (`OnRrToneMap`,
     `ToneRr` in `vam_rr.inl`) intercepts the event, runs the same native pass **once on RR's full-resolution output**
     (the double-wide scene image) and copies the result back, and the later stages (neural denoiser, sharpening, frame
     generation) see the toned image. If the compositor is not ready, the serial does not match or the target
     size does not fit, the wrapper returns false and the event runs as it would without RR.
   - *Path tracing off (DLSS SR K / L / M or off)*: no interception; it runs at the camera stage, before SR.
   - *In place*: when the target takes a UAV and a typed load of its format (RR's output image does), the last pass writes
     straight into it (`CSCompositeInPlace`, `CSCompositeFilmInPlace`), each thread reading and writing only its own
     pixel; this is bit-identical to writing a temporary and copying back, and saves one full-size RGBA16F copy (about
     0.17 ms on the development machine at the measured settings; see below). The camera-stage target has no UAV and
     takes the copy.
4. **Neutral style (0).** Bloom after Jimenez 2014 (as Unity's PostProcessing v2): a mip chain of 13-tap downsamples, the
   first with Karis' `1 / (1 + luma)` weights against single bright pixels and then the threshold (1.0, the old clip
   point) with a soft knee (0.25), tent upsamples summed back, the sum added tinted (1 / .6 / .4, warm: film halation) at
   intensity 0.15. Tone: per channel, **unchanged up to the knee 0.6**, above it the shoulder
   `1 - (1 - k) exp((k - x) / (1 - k))`, which reaches white smoothly. Everything VaM drew below the knee keeps its value
   byte for byte and only what VaM clipped is toned. This is why PostProcessing v1 was not used: its Hable curve changes
   the middle tones of every scene, and authors tuned their scenes to the clipped look.
5. **Film style (1).** The brightest channel goes through Lottes' 2016 curve `x^a / (x^(a d) b + c)` (a 1.5, d 1, mid grey
   0.18 out at 0.21, 3x the old white point to white); colours keep their ratios to that channel, a little more saturated
   in the mids (exponent 1.15) and bleaching toward white as the curve reaches it (`y^4`, "dye crosstalk"); then a print's
   split tone by sqrt-luminance (shadows slightly cyan, highlights slightly warm, strength 2, each tint normalised to
   luminance 1). Pure white becomes a slightly warm cream. The parameters were chosen qualitatively from published film
   behaviour and not swept; the first version (a 1.3, split 1) was too subtle and was strengthened one step. It changes
   the shadows and mids of every scene it runs on. Defaults live in `native/tonemap.cpp` (`gContrast`, `gShoulder`,
   `gHdrMax`, `gMidIn`, `gMidOut`, `gSaturation`, `gCrosstalk`, `gSplit`).
6. **Not done on purpose.** Grain, vignette, chromatic aberration and depth of field are not applied in VR (two eyes would
   disagree; grain is wiped by the denoiser and smeared by frame generation). Colour grading by LUT and mild bloom are
   fine. A third style (an AgX-like curve with Blender's inset/outset matrices, white point 3) was built and shown; it
   kept the hue of bright coloured lights better (an orange lamp's hue moved 16 to 54 degrees in the neutral style versus
   16 to 29 in AgX, in an offline test) and was not installed. It is not in this repository.

### Display lighting

File: `display-lighting/VaMDisplayLighting.cs` (plugin `local.vam.displaylighting`, 1.6.0) and `VaMDisplayLighting.Menu.cs`
(a "Display Lighting" tab in VaM's main menu with start/stop and one button per style). It always starts stopped.

- **What it does.** On start it picks the Person nearest the viewer's view, pauses all existing lights (only their enabled
  bits; authored values and transforms are never edited), takes over the ambient light and exposure, builds the rig around
  the subject and keeps it following. Before a scene is saved it restores everything and resumes after; switching scenes
  or removing the subject stops it and restores exactly. New atoms are handled by event, never a scene scan.
- **Ambient.** The plugin sets `RenderSettings.ambientMode = Custom` with its own spherical-harmonics probe. In Unity
  2018.1 the Skybox mode with no skybox material uses only the flat ambient colour, so the probe would be ignored.
  The Skyshop controls it saves and restores include `masterIntensity`, `unityAmbientIntensity`, `camExposure`,
  `diffuseIntensity`, `specularIntensity`, `skyboxIntensity`. A specular environment (the cube skin, eyes and hair
  reflect) is built from the same ambient terms as soft-edged discs (20 degrees, 35 for broad terms). A bright area
  or a uniform base in that cube makes the cornea centre mirror it, which turns irises white and pupils to nothing, so
  the key is a 20-degree disc with no base.
- **Styles.** Ids in the config: Beauty (clamshell: soft key just above eye line, weaker fill from below, two hair
  lights), Rembrandt (one key on the short side, 35 degrees, triangle of light on the cheek), Window, Golden (back light),
  Contour, Cinema (cool/warm), then the contemporary set Clear, Pastel, Neon, Sunset, Flash. Each style is a list of
  lamps (`LampSpec`: azimuth and elevation around the subject-to-viewer line, luminance in linear units at the aim point
  before N.L, colour, cone angle, shadow edge, shadow-map resolution, aim, motion) plus environment terms (`EnvSpec`).
  The rig turns with the viewer: lamps follow with a 0.35 s lag (`AnchorLag`) and a 25 degree dead band
  (`TurnDeadband`) so stepping or leaning does not swing the light; sun and window lamps stay fixed in the world.
- **Shadows.** VaM's skin and eye shaders read a lamp's `shadowStrength` as the blur radius of its shadow, never as
  darkness (see [vam-facts.md](vam-facts.md)). At 1 with a wide cone it blurs nose, eye-socket, chin and under-breast
  shadows away (10 to 20 cm). Each lamp therefore carries an `Edge` (the shadow's edge radius on skin in metres), and
  `ShadowStrength()` inverts the PCF radius formula to set `shadowStrength` for it; lamps use Hard shadows because Soft is
  PCSS there and hair strands then draw sharp streaks on faces.
- **How 1.6.0 was derived.** From a survey of 677 installed authored scenes that have a person and lights. Authors use
  lamps as the main light (spots, cone median 80 degrees, 1 to 2.3 m away); the skin's own environment light is zero in
  about six in ten scenes (a missing key means the default full-strength museum sky, which a naive count mistakes for
  zero); the front lamp sits at eye height (median -3 degrees, nine in ten below 20) where a VR viewer is; the light on
  the face is neutral to slightly cool (median log2 R/B 0), a third of front lights are tinted; the back light is about
  as strong as the key (median 0.92 of it; in 44 percent of scenes the brightest lamp is behind); a front lamp is a median
  1.65 m away; 106 scenes show the sky; 146 use a post-processing LUT/bloom stack. Not different from the generated rigs:
  face-to-hip brightness ratio (about 1) and three lamps reaching the person. The 1.6.0 rig therefore: lamps are the
  main light again, front environment term roughly halved and lowered to 20 to 30 degrees, the main lamp at 15 to 20
  degrees (Rembrandt 35 to keep its triangle), cone 80 degrees, distance x0.75, ambient base roughly halved and neutral,
  key light 6500 K (Rembrandt 5500, Cinema 5200; warmth only in accents), back and rim lamps raised to about key level,
  the scene's visible sky kept; the total light reaching the face from the front is unchanged from the previous version.
- **Why the main light moved back from the environment to lamps.** Earlier versions put the key into the environment
  term to avoid the blurred raster skin shadows. In the development tree, ray-traced skin shadows (a 0.25 m disc light)
  made lamp shadows soft on their own, which removed that reason; and an environment key lights the room and the person
  equally so the person does not stand out. **That softness came from the texture-space ray tracing that is not in this
  repository.** With raster skin shading here, softness comes from the `Edge`/`shadowStrength` mechanism above; with path
  tracing, from the effect-light angular size ([path-tracing-authored-lighting.md](path-tracing-authored-lighting.md)).
  The rig was not re-judged for these routes after that change; if styles look harsh on the raster route, start with
  `Edge`.
- **Optional.** `manifest/components.toml` marks it `optional = true`; nothing else depends on it. Tone mapping has a soft
  dependency on it (below).

## Settings

| File / section / key | Values | Meaning |
|---|---|---|
| `local.vam.tonemapping.cfg` `[General] Mode` | 0, 1, 2; default 1 | 0 off (VaM unchanged; camera keeps its HDR setting unless RR needs it); 1 only while display lighting is running and not suspended; 2 always |
| `local.vam.tonemapping.cfg` `[General] Style` | 0, 1; default 0 | 0 neutral shoulder, 1 film |
| `local.vam.displaylighting.cfg` `[Lighting] Style` | Beauty (default), Rembrandt, Window, Golden, Contour, Cinema, Clear, Pastel, Neon, Sunset, Flash | the rig; on/off is the plugin's menu tab, it is stopped at every launch |

Both tone-mapping keys are live (read in `LateUpdate`) and are on the VR menu ([settings-and-menu.md](settings-and-menu.md):
"Tone mapping: off / with display lighting / always" and a style choice). Mode 1 finds display lighting by reflection on
its private `running` and `suspended` fields (`DisplayLightingOn()`); renaming them leaves mode 1 off forever and logs
"Display Lighting has no running state; mode 1 stays off". The bloom and curve numbers are not config keys: they are
the test API (`Params(...)`, `Film(...)`) and the defaults in `tonemap.cpp`.

## Measured

- Neutral and film styles cost about 0.10 ms GPU each on the camera target in a simulated headset at its default size
  (about 0.078 ms per frame in one test of 2018x1072 eyes); the target format there was RGBA16F.
- The in-place composite under RR saves about 0.17 ms (writing a temporary and copying back is gone); with the other copies
  removed in the same change the output stage lost about 0.35 ms per real frame (development machine, RR 50 percent
  input, a heavy dance scene). Frame rate itself changed less than the measurement noise between sessions.
- Look, neutral style: in a gold back-lit scene that had no clipped pixels, the only change was a 6 percent dimming of
  values in 0.6 to 1.0 (the cost of the shoulder) and a warm halo on bright edges. The gain is in scenes whose lamps
  clip. Film style: lit skin on average +2 to +5 levels (8-bit), brightest tenth +4 to +10, shadows deeper (stills in a
  simulated headset).

## Pitfalls

- **Highlights look grey or hazy, a halo around bright edges, the picture is warmer than the author's** -> switch `Mode` to
  0 and compare. That is the tone mapper, not the scene.
- **A film look the owner finds too faint or too heavy** -> edit the film defaults in `tonemap.cpp`; to iterate, drive
  `Film(...)` from the test agent and render side-by-side stills; to change the neutral curve, `Params(...)`. A change to
  the neutral knee changes every authored scene because they were tuned against clipping.
- **Mode 1 never turns on** -> the reflection to display lighting failed (field renamed) or display lighting is not
  installed.
- **Tone mapping applied twice or to the wrong image under RR** -> the plugin's event must go through
  `VaMDLSS_WrapToneMapEvent`; the compositor then runs it on RR's output. Look for "tone=1" in the compositor's RR
  submission log line (`rrToneSerial == guides.serial`).
- **Display lighting shows eyes with a white iris** -> the specular environment has an overly bright area or a base
  colour; keep the key disc at 20 degrees and the base at zero.
- **Skin shadows disappear with lamps** -> `shadowStrength` is a blur radius in VaM skin shaders; keep the lamp's `Edge`
  (see above), do not set strength to 1 on a wide cone.
- **Display lighting readings after the rig changes** -> verify the ambient from what the GPU reads, not from what the plugin
  set; VaM only writes `ambientLight`, so an `ambientMode` other than Custom ignores the probe.

## Tried and dropped

- **AgX-like style** (a third style): kept the hue of bright coloured lamps, compressed everything above mid grey harder
  (grey 0.5 to 0.42 against the neutral's 0.5, the film's 0.60); built, compared and not kept: the one person who judged it preferred the other
  two. It is a third curve beside the two in `components/shading/tone-mapping/shaders/tonemap.hlsl` and a `Mode` value;
  add it back when someone wants that look.
- **Film grain, vignette, chromatic aberration, depth of field in VR**: see above.
- **VaM's built-in post-processing (PostProcessing v1) as the tone mapper**: its curve changes mid tones.
- **Environment-as-key-light rig** (display lighting 1.4 to 1.5): replaced; see above.

## Open

- The 1.6.0 rig was derived against the ray-traced skin shadows of the development tree and installed without a game
  side-by-side; it has not been judged on this repository's two routes in a headset.
- Whether the HDR values that mode 0 passes along under RR (the camera is HDR but no effect is attached) are clamped or
  carried downstream was not checked in these notes; confirm in the compositor before relying on it.

## Verify

- `tests/build.cmd` builds the offline test; `tests/tm_test.cpp` checks the native library on a synthetic two-eye FP16
  target: (1) away from anything bright style 0 returns pixels at or below the knee unchanged (FP16-exact) and style 1
  returns the film curve of the input; (2) a flat patch above the knee follows the shoulder or the film curve plus its
  small bloom; (3) a bright patch in the left eye by the seam adds no light to the right eye; (4) NaN and infinity stay
  where they are and do not spread; (5) the render target bound before the event is bound after it. Run
  `tm_test <VaMToneMappingNative.dll> [--style 0|1] [--dump file]`.
- The routing under RR, the HDR camera handling and the look need the game (a simulated headset for stills and the
  timing, a person for the look). The plugin's `Report()` (test API) shows mode, style, whether the effect is attached,
  camera, HDR and the native format/skip counters.
- Display lighting has no offline test. Verify in game: start from the tab, check the log, the restore on save and on a
  scene change, and the ambient values read back from the GPU.
