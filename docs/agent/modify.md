# Changing or extending a component

For an agent asked to change how something works. Read [../protocol/design-rules.md](../protocol/design-rules.md) before
designing and [../protocol/testing.md](../protocol/testing.md) before running anything.

## Before you edit

1. Open the component's `README.md`: files, build, settings, interfaces, tests, invariants.
2. Open the knowledge document it names in `docs/knowledge/`. Read "Pitfalls" and "Tried and dropped" first; the idea
   you are about to implement may be listed there with the reason it lost.
3. Find the invariant your change could break and the test that guards it. If no offline test guards it, say so in your
   plan: only the game will show it.
4. State the cause in terms of code ("this function, at this point in the frame, produces this") before proposing a
   fix. Then decide whether the fix belongs in the core or is a matter of degree with an existing lever.

## The loop

1. Edit.
2. Regenerate if you touched a generator or a shader that is compiled into a header (`out/gen/`).
3. Build the component (`build.cmd`; managed plugins with the C# 3.5 compiler).
4. Run the component's test. A change that is not meant to alter behaviour must leave probe outputs byte-identical.
5. Only then, if the change needs it, one session in the game with a test card written beforehand.
6. Report: what changed, what you verified and how, what you did not.

## Recipes

### Add a setting

- The plugin: a `Config.Bind` in its `Awake`, applied through a `SettingChanged` handler so that it works while playing.
- The in-game menu: one row in `components/render/vr-menu/VaMVRMenu.cs` on the page it belongs to, bound to the
  plugin's config entry (`PluginSetting`). Give it `pathRule` when it only makes sense with path tracing on (1) or off
  (-1).
- The presets: add the `[section, key]` to `components/render/launcher/profile-manifest.json` so that the value is kept
  per display mode.
- A compositor setting instead lives in the run's `performance.ini`; add its key to `vam_performance_config.h`, to the
  launch panel's writer and to the manifest's `performance` list.
- One place per setting. Read the control's rule aloud; if it needs "but when", redesign it.

### Teach the path tracer a material or a shader family

- Translation happens in `components/shading/skin/RtScene.cs` (which properties a shader reads and what they mean) and
  is consumed in `components/shading/rt-core/shaders/path_trace.hlsl`.
- Read the shader's real program first (`components/shading/rt-core/tools/lift_family.py`,
  `material_coverage.py`): what it computes, which inputs it reads. Translate the meaning, not the look of one scene.
- An object the tracer cannot translate is never a reason to refuse the scene: it goes through an adapter (overlay as
  UI or effect, or a generic surface from its standard properties) and is logged once.
- Add a contract to the probes (`components/shading/rt-core/tests`) that fails without your change. Existing outputs
  must stay byte-identical.
- After changing a tracing shader, the next path-traced start compiles pipelines in the driver for about 26 s. Run it
  once yourself.

### Change the skin, hair or clothing replacement shaders

- The edits live in the generators (`components/shading/skin/tools`, `hair-strands/tools`, `cloth-shading/tools`), which
  lift VaM's own programs and patch them. Change the patch, regenerate, run the component's tests.
- A patch finds its place by a pattern in the lifted program and stops with an error when the pattern is missing. Keep
  it that way: a silent skip would ship a half-edited shader.
- Generated programs never enter git.

### Change a runtime component (cpu-boost, hair-solver, cloth-physics, wrap-batch, gc-tuning)

- These promise VaM's own results. The work is done when the verify mode or harness reports equality again.
- Floating point: follow the rules in [../knowledge/cpu.md](../knowledge/cpu.md); an "equivalent" expression is usually
  not bit-identical.
- New threads or parallel loops use cpu-boost's pool. Then run the CPU census for the candidate.
- gc-tuning patches the loaded `mono.dll` only after checking its bytes. Keep every check; add one for every new place
  you touch.

### Change the compositor, the bridge or frame generation

- Nothing offline covers these. Plan one session on the desktop or the simulated headset, and decide beforehand what
  the logs must show.
- Settings are reread when the run's `performance.ini` changes: compare variants inside one session by editing the file.
- Keep upstream licence headers in OpenComposite and the OFXR layer; our additions are in the files the component
  READMEs name.

### Add a component

- A folder under `components/<group>/<id>/` with a `README.md` in the common shape.
- An entry in `manifest/components.toml` (path, build kind, needs, installs, enable, guard).
- If it reads anything from VaM at build time: a generator that writes to `out/gen/<id>/`, never a checked-in copy.
- If it is a BepInEx plugin with a native part: the native library is loaded by path with `LoadLibrary` and its exports
  bound with `GetProcAddress` (Mono resolves `DllImport` only from `VaM_Data/Plugins`), and it is installed as `.bin`.
- A knowledge document when it has a mechanism worth explaining, and a row in `docs/README.md`.

## Building what is not here

The project stops where its authors stopped. If the user wants something it does not have, the question is where it
plugs in and what must stay true, not whether it is allowed. The places new things attach, with what each must keep:

| You want | It plugs in at | What must stay true | How to check |
|---|---|---|---|
| Another temporal upscaler (path tracing off) | the compositor, beside the three that exist: `components/render/opencomposite/OpenOVR/Compositor/vam_sr_backends.h` (XeSS and FSR are the two small examples) and the hand-over in `dx11_vam_dlss.cpp`; a model name in `vam_performance_config.h`, the menu and the launcher | it gets the same colour, depth, motion vectors and jitter the others get ([../knowledge/motion-vectors.md](../knowledge/motion-vectors.md), [../knowledge/upscaling.md](../knowledge/upscaling.md)); it is probed, not assumed; menu, launcher and compositor name it alike | a desktop session: the feature is created, the picture is closest to the native frame with the jitter as passed; then VR per eye |
| Another frame generator, or more frames per pair | `components/render/ofxr-dlssg/include/xrfg/d3d12_frame_generator.hpp` (DLSS-G and FSR implement it); used by the VR layer and by the desktop presenter in `components/render/d3d11-proxy` | every real frame fed once and in order; generated frames sit at their in-between poses; pacing stays even ([../knowledge/frame-generation.md](../knowledge/frame-generation.md)). The cap of three generated frames (`kGuidedMaxOutputs`, and 2 / 3 / 4 in the launcher and menu) is where the work stopped: the DLSS-G runtime reported room for more on the development machine | the simulated headset's head-turn test (content shift per displayed frame), the presenter's pacing log |
| Path tracing on a card without RTX | where Ray Reconstruction is evaluated: `components/render/opencomposite/OpenOVR/Compositor/vam_rr.inl`; the tracer already writes the guides a denoiser needs ([../knowledge/path-tracing.md](../knowledge/path-tracing.md)) | the tracer's output is noisy low-resolution radiance: something must denoise and upscale it. An earlier NRD path was removed when RR became the only route; a denoiser plus XeSS or FSR is the obvious shape. Hair in the tracer needs the card's linear-swept-sphere support, everything else needs DXR 1.1 | the tracer's offline probes do not depend on the reconstruction; judge the picture in a session |
| A shading change or a new material family | the recipes above; the generators in `components/shading/*/tools` | VaM-derived output stays in `out/gen/` | the component's tests, then a look |
| A new runtime component | "Add a component" above | VaM's own results, a verify mode, workers from cpu-boost | its verify mode in the game |
| Another VaM build, another BepInEx | [adapt-vam-version.md](adapt-vam-version.md) | recognise code by content | build, first-run check, the plugins' own lines in the log |
| Numbers for the user's own scenes, another display, another multiplier | `tools/lab/bench/bench.py` takes any scene path, refresh rate and multiplier | compare inside one session | [../benchmarks/README.md](../benchmarks/README.md) |
| Something not in this table | read [../knowledge/frame-path.md](../knowledge/frame-path.md) for who owns which step of a frame, then the component's README | | |

What was tried before and why it was left is in each knowledge document ("Tried and dropped", "Open") and in
[../knowledge/rejected-and-open.md](../knowledge/rejected-and-open.md). Read those as a head start: they say what
failed, under which conditions, and what would have to be different. If your user's conditions are different, the
answer may be too.

## Tools for finding out what happens

| Question | Tool |
|---|---|
| Where is a hung VaM stuck? | `tools/lab/simxr/stacks.py <pid>`: native stacks of every thread |
| What is at this crash address? | `tools/lab/simxr/symbolize.py <module> <offset>` |
| Is the denoised picture or the tracer's input at fault? | the path tracer's reference view (Ctrl+Alt+R, or `[Performance] PTReference=1`): with the camera still it shows the running average of the tracer's raw input |
| How even is frame delivery? | `tools/lab/simxr/flightlog.py <run log folder>` on the frame generation layer's log |
| What does each thread cost? | `tools/lab/census` |
| What did this run actually load and do? | `<VaM>/vamkit/logs/<run>/`: BepInEx's log, the compositor's, the DLSS logs |

## Before you say it is done

- The test that guards your change passes; say which.
- Nothing derived from VaM, no binary and no private path entered the tree.
- The README and the knowledge document say what is now true. If you tried something that lost, add it to "Tried and
  dropped" with the reason, so that the next agent does not try it again.
