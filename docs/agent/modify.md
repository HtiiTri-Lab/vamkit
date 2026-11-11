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
