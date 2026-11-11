# Hair: shading, physics and the cost budget

VaM's physics hair (GPUTools) is the most expensive thing a character wears: tens of thousands of strand segments per
person, drawn again for every pixel light, every shadow map and every eye, and simulated with dozens of tiny compute
dispatches per hair item per physics step. Three components cut that cost without changing the picture or the motion:
`hair-strands` draws the hair once through a software-rasterised visibility buffer, `hair-solver` runs GPUTools' own
physics rules fused into one compute pass per step, and `hair-budget` caps the hair detail per person and per scene.
A player sees the same hair (plus a light-through-the-fibre glow in strands mode), at a higher frame rate in scenes with
much hair. With path tracing on, strands do not draw at all: the hair is traced as swept spheres
([path-tracing.md](path-tracing.md)).

## How it works

### Shading: `components/shading/hair-strands`

**What VaM does.** The hair shader (`GPUTools/MeshedVR/HairOpt`) is vertex shader -> hull shader (isolines:
`hairMultiplier` strands per scalp triangle, `curveDensity` segments per strand) -> domain shader (the strand point and
its lighting) -> geometry shader (a camera-facing quad per segment) -> pixel shader. Unity's forward renderer draws the
whole hair once in ForwardBase and again, whole, for every further pixel light (ForwardAdd), and again for every shadow
map and for each extra camera (mirrors, window cameras, depth pre-pass). Cost is therefore the number of redraws, not
strand width: the pass is primitive-bound, and at headset resolution most quads cover no pixel centre.

**Seeing the draws.** No shader file is swapped. `native/hair_onepass.cpp` hooks the D3D11 immediate context's
function table (the runtime keeps it inside the context object and rewrites it on Flush, so both the template copy in
`d3d11.dll` and the live table are patched) and hashes every shader the game creates (SHA-1 of the bytecode); programs of
the hair shader are tagged. The table of known programs, `native/hair_programs.h`, is generated from the game's own
bytecode by `tools/gen_programs.py` and is not committed. The plugin (`VaMHairOnePass.cs`) loads the native library early
(before the game creates hair shaders) and marks the eye camera's opaque pass with render events at
`CameraEvent.BeforeForwardOpaque` / `AfterForwardOpaque` plus one end-of-frame event.

**Three modes** (`MODE_OFF`, `MODE_MERGE` = 2, `MODE_STRANDS` = 3; mode 1 only observes). Strands is the default; if its
objects cannot be made it runs merge, and if merge cannot be made, off. A failure inside a frame backs strands off for
`30 << min(fails, 7)` frames (the following opaque passes merge), so a recurring failure costs a growing quiet time.

- **Strands** (`hair_strands.hlsl`, driven from the "strands" section of `hair_onepass.cpp`). The ForwardBase draws of
  both eyes are taken (constants, buffers and output state kept, draws skipped). When the ForwardBase draws end:
  1. `HsBuild` (compute, once per item and frame): every strand point exactly as the original domain shader computes it,
     plus what lighting needs (tangent, normal, colour, specular shift), in 32-point tiles. The isoline domain
     coordinates are read back once from the hardware tessellator through stream-out, so subdivision equals the
     original's.
  2. `HsRaster` (compute, per item and eye): the geometry shader's quads rasterised in software with D3D11's rules
     (16.8 fixed point, top-left fill, cull mode, depth clip) into a visibility buffer: a depth pass (atomic max keeps the
     nearest) and an id pass (item and segment, `item << 26 | segment + 1`).
  3. `HsResolve` (pixel shader over the whole eye target, once per pass): for each pixel's segment, the original's
     per-point lighting at both ends, interpolated perspective-correct as the rasteriser would, then for ForwardAdd each
     light's per-pixel attenuation and shadow; writes colour and depth. ForwardAdd lights are taken too and resolved in
     compatible full-target batches of up to 16 lights (`HS_MAX_LIGHTS`) with separate masks per item and eye.
  Items whose draws cannot be taken go through unchanged, ForwardAdd included. A ForwardAdd light of a taken item that
  cannot be taken meets a depth the resolve wrote (equal to the GPU's within about 1e-6, not always exactly) and misses
  pixels: that is the failure that triggers the back-off above.
- **Merge** (`hair_onepass.hlsl`, compiled per light count `HOP_LIGHTS` 0..8, with or without the screen-position
  interpolant for screen-space shadows). Unity's ForwardBase draw stays; the item's ForwardAdd draws are replaced by one
  draw per item and eye that adds every further light at once (additive blend, depth equal), each light limited to its own
  ForwardAdd scissor rectangle. The arithmetic follows the original DXBC instruction by instruction; the strand point and
  shading code is shared with the offline comparison tool that checked it against the original. One deliberate
  difference: spot shadow coordinates and point-light vectors are computed per pixel from the interpolated world position
  instead of per vertex (both are affine in position). `HOP_BASE 1` (ambient, main light and lights in one draw) exists
  only for the offline comparison.
- **Off**: Unity's draws.

**Lighting.** `HairDirect` in `hair_strands.hlsl` is VaM's two offset Kajiya-Kay lobes, a wrapped diffuse and fresnel.
`HairTransmission` adds the light that passes through the fibre: a Marschner-style TT lobe (longitudinal Gaussian whose
width is the reciprocal square root of the material gloss, widened by a 0.25 m disc for local lights
(`HS_LIGHT_RADIUS`) with offset -0.035; azimuth term after Karis 2016, `exp(-3.65 cos(phi) - 3.98)`;
`(1 - F)^2`; hair colour absorbed along the path length), multiplied by light colour and the IBL exposure. Hair lit from
behind glows. It exists only in strands' analytic resolve; hair Unity still draws, the merge fallback and shadow maps do
not have it. A test switch (`Transmission(bool)`) turns it off.

**Which hair strands takes.** Only the quality hair shader programs known to the generated table (the large majority of
installed hair). The fast single-pass variant and the "thicken" variants are different programs and stay with Unity. The
shader type comes from the hair's saved JSON and can be overridden by a preset or scene.

**Shadow maps stay Unity's.** A compute rasteriser for shadow maps measured 0.37-8.5 ms against the original's
0.10-0.47 ms in the same offline replay; the cost is rasterising visible shadow-casting triangles, so only fewer
triangles would help, which would change the shadows. Not done.

**Close to the camera.** The software rasteriser assigns one thread per quad, so a quad covering hundreds of pixels is
serial work, and with the camera inside the hair every quad behind the eye also entered the clip list. `HsRasterGroup`
(64 threads per entry, sharing a triangle's scanlines) takes quads too large for one thread, and quads wholly behind the
eye are dropped at the start. Output is byte-identical. Offline, a heavy-hair character: 0.37 ms far away, 1.16 ms at
10 cm and 4.3 ms with the camera inside the hair before the change; inside the hair 2.39 ms after, 10 cm unchanged (that
distance is a throughput limit; going lower needs large segments on the hardware rasteriser).

**Under path tracing.** `HOP_PathModeV1(1)` (called from the skin plugin's native code, `components/shading/skin/native/rt.cpp`,
when path tracing takes the scene and `0` when it stops, pauses or fails) stops the strand raster and the visibility
buffer, frees their large buffers and makes the plugin skip the hair ForwardBase and ForwardAdd draws. What the path
tracer needs is the strand points: `HOP_BuildPathHairV2` runs only the shared build shader and the isoline domain on the
live tessellated guide particles (`native/path_hair_registry.inl`) and hands the points plus per-item parameters
(`radius` = `_StandWidth`, `roughness` from the primary specular exponent, specular shift, diffuse) to
`components/shading/rt-core/src/path_hair.inl`, which builds linear swept spheres (LSS, an NVIDIA ray tracing extension)
one acceleration structure per light-link class and camera visibility. The structure is refitted each frame and rebuilt
at most every 16 rendered frames (a long refit chain keeps a poor tree after large animated changes). The author's hidden
`_SpecularColor` is read and mapped to the reflection lobe; hair colour, transmission lobe and geometric density are
unchanged. Inter-strand multiple scattering is a closed-form term, not traced rays: tracing it (each scatter one shadow
ray and one continuation ray, up to 64 per path) restored the hair's brightness but cost about 2 ms per eye with the
screen full of hair; the closed-form diffuse-cylinder lobe with gain 1 replaced it. See [path-tracing.md](path-tracing.md).

### Physics: `components/runtime/hair-solver`

**What GPUTools does.** `HairPhysicsWorld.DispatchPhysicsImpl` runs, per hair item and physics step, outer x
(iterations x ~8 + ~5) compute dispatches (about 20-40; a default hair item), and each `KernelBase.Dispatch` rebinds every
buffer and parameter by name. The particles are few (median ~1.9k per item, maximum 65k), so the cost is the dispatch
count times the item count.

**The fused solver** (`solver/fused_strand.hlsl`, `plugin/native/hair_solver_native.cpp`, `plugin/VaMHairSolver.cs`). A
Harmony prefix on `DispatchPhysicsImpl` queues the step for a native library that runs on Unity's D3D11 device through
`GL.IssuePluginEvent`; the particle, point-joint, scalp-matrix and collider buffers are GPUTools' own, read and written in
place, so GPUTools' renderer and, for an item that falls back, GPUTools' own solver see exactly what GPUTools would have
left. Everything that touches one strand (integration, point joints, two-colour distance and compression joints, spline
joints, velocity, plane / sphere / capsule collision) runs with the strand's positions in group-shared memory, in the
original order and with the original arithmetic (the shader header lists the order per substep). One thread per particle;
`strandsPerGroup` whole strands per 256-thread group. GPUTools sweeps every distance / compression / cling colour group
as often as its `GroupsData` lists it, and that is twice because the joint builders read `GroupedData.Data` twice in
`OnBuild` and every read appends the groups again; the plugin reads the counts from the live kernels (`distanceRepeat`,
`compressionRepeat`, `clingRepeat`) instead of assuming one.

**Why strands are simulated at their guides' spacing.** GPUTools' rules (stretch, bounce, damping) depend on the point
spacing, so rendered strands are not simulated at a finer spacing: a direct per-strand length solve, long-range
attachments, DFTL and curl-spring compliance were each tried against GPUTools and removed. The goal is to run every
installed hairstyle as GPUTools runs it, not to invent a new hair model.

**Cling modes** (the only cross-strand step; `[General] Cling`):

| Case | Mode | Dispatches per step | Fidelity |
|---|---|---|---|
| no cling joints, or cling power 0 | `kOpStep` / CSStep: all substeps and outer passes in one dispatch | 1 | exact |
| `Synchronous` (default) | Jacobi: each strand's B phase gathers its cling joints from positions all strands had after A | 1 + outer x iterations | mean difference to GPUTools 0.3-0.6 mm on the test hair |
| `Original` | GPUTools' own `NearbyDistanceJoints` kernel runs colour by colour between the solver's dispatches | more | same as GPUTools (about 0.4 micrometre per step) |

A third in-shader mode (cling once per step on a snapshot, `clingMode 2`) is not used for hair with cling: neighbours
extrapolated by velocity made the coupling unstable.

**One solve per frame, items side by side.** One item's step is a few dispatches on a few thousand particles, far from
filling the GPU. On D3D11On12 each item waited for the one before ([d3d11on12.md](d3d11on12.md): the translation layer
inserts UAV barriers that wait for idle on NVIDIA), and GPUTools tessellates each item right after its step. So steps
wait in the native library and run together (`Solve`: every item's first dispatch, then every second, with a `Flush`
between phases on D3D11On12), the constant buffer is DYNAMIC (`Map` WRITE_DISCARD; an `UpdateSubresource` of a DEFAULT
buffer put a copy and two state transitions on the GPU each time), and `DispatchImpl` (the tessellation) waits until the
steps before it ran: before a camera renders, before the item's next step, before a skin the item hangs on writes its
scalp matrices, normals or vertices, before `GPUCollidersManager` rewrites colliders, before a rebuild or dispose.
A frame in which hair takes two steps (hair following a body that took two, see the budget below) keeps **one** solve: the
item's second step joins the batch in the phases after its first (`kOptRepeat`, plugin `SetRepeat`), instead of a solve
at every item's second step, which with n items was 2n + 1 serial solves, each as long as the slowest strand chain
(`CSStep` is one thread group doing outer x iterations from start to finish). Nothing writes GPU-side hair state between
the two steps (`InitData` / `UpdateBuffers` set CPU-side values only), so this is safe.

**Chest collision.** Hair collides with VaM's colliders through `GPUCollidersManager`. `HairBodyContact.cs` keeps an index
mask over its ordered lists: a collider on the left/right pectoral or the breast physics mesh is marked 1, "position-only
contact" (the minimum displacement that prevents penetration, with the inward velocity removed; tangential and outward
motion kept; no drag, hold or power from the body). Hair gets no springy push from the body, only a position
relationship. If the soft body plugin (not in this repository; the development tree's breast soft body) is installed and
exposes `PresentedBreastSpheres` (spheres whose radii reach the skin actually uploaded) or `BreastSpheres`, the breast
colliders are replaced for the hair by those spheres (mask 2, per breast behind a bound sphere), because VaM's colliders
follow VaM's own breast physics, which a soft body no longer shows; hair resting on them floated above the drawn breast
or sank into it. When the presented geometry's revision changes, outstanding GPU work reading the old data is flushed
before the collider buffer is refreshed. **Without a soft body, or with it off, the colliders stay as VaM's own (mask 1).**
The solver also adds GPUTools' plane collision for VaM's ground-plane colliders.

**What stays with GPUTools** (the log says why per item): style mode, joint data the solver does not model, more than 256
points per strand, and any error. Diagnostics (`[Diagnostics] Diagnostics`, `VerifyEverySteps`) can run GPUTools' step
on the same particles every N steps per item and compare.

### Budget: `components/runtime/hair-budget`

The budget writes only what the renderer and solver read (`HairLODSettings.FixedDensity` / `FixedDetail` / `FixedWidth`,
`PhysicsSettings.Iterations`), never the presets, scenes or sliders, so saving is unaffected.

1. **Cost model** (`HairBudgetMath.Cost`): per scalp triangle ("patch"), `hairMultiplier x (curveDensity + 25) x 24/49`
   segments of a curveDensity-24 hair. Measured in game: a strand costs about its segments plus 25 more; curveDensity 24
   costs exactly its segment count, the library's mean. Item cost = patches x that. Light and camera pass counts are left
   out on purpose: they change in play.
2. **Per item**: `hairMultiplier x curveDensity` is capped at `DensityMax` (both lowered by the same factor).
3. **Water-filling**, per person, then per scene: items costlier per patch than a common level come down to it; items
   already lighter keep their values; pieces costing under 5% of the budget (`SmallItemShare`) are kept as they are,
   unless that leaves half the budget or more fixed, in which case everything is in the pool.
4. **Split** the reduction between the two sliders: both scaled by the same factor (solving
   `Cost(rc s, rh s) = level`), within floors that keep strands following their guides and curls:
   `curveDensity >= max(CurveDensityMin, 0.75 x guide segments)`, and `ceil(6 x curlFrequency)` points when the curl is
   strong (> 0.03); `hairMultiplier >= HairMultiplierMin`. Width grows by `sqrt(requested / actual)`, capped at
   `WidthCompensation`, to keep coverage.
5. Physics iterations <= `IterationsMax`; optionally no hair shadows (`DisableShadows`: each shadowed light draws the hair
   again, six times for a point light).

Hair items are known from a Harmony postfix on `HairSimControl.Awake` (one `Resources.FindObjectsOfTypeAll` seed at
start). Values are worked out once for a situation and written again only when an item appears or goes, its sliders
change or the settings change; `Check()` every 0.25 s only compares tracked items. If the Awake hook cannot be made the
plugin falls back to `FindObjectsOfType` every 2 s (logged as a warning), which is the pattern to avoid: see below.

**Hair physics rate.** `[HairPhysics] Rate / UpdateCap` is a copy of VaM's Physics Rate and Update Cap that only physics
hair uses (`VaMHairBudget.HairRate.cs`). Rules as VaM's `SyncPhysics`: step = 1/rate; a frame advances at most step x cap
(never more than 0.05 s). `Rate = VaM` (the default, "follow body physics") takes the steps the body's physics took in the
frame before, with that step, run in the hair's per-frame `Dispatch` (after scalp skinning, before tessellation): the
frame drawn is the pose the previous body step produced and the colliders were computed from, so hair stepping in
`FixedUpdate` would run a frame before its scalp is skinned to that pose. A fixed rate (45...288 Hz) or `Auto` (headset
refresh rate, halved above 91 Hz) gives hair its own clock. Any failure puts hair back on VaM's `FixedUpdate`. The solver
uses the budget's `HairFixedDeltaTime` when the budget is present.

## Settings

| File / section / key | Values | Trades |
|---|---|---|
| `BepInEx/config/local.vam.haironepass.cfg` `[General] Enabled` | `true` (default) / `false` | `false`: nothing loaded, VaM's hair unchanged. Read at startup |
| `BepInEx/config/local.vam.hairsolver.cfg` `[General] Enabled` | `false` in code; the install turns it on (`enable` in `manifest/components.toml`) | `false`: GPUTools solves |
| same, `[General] Cling` | `Synchronous` / `Original` | speed (Synchronous) vs bit-fidelity of cling hair (Original) |
| same, `[Diagnostics] Diagnostics` / `VerifyEverySteps` | `false` / `0` | tests only; verify stalls the render thread for a read-back |
| `BepInEx/config/VaMHairBudget.ini` `[HairBudget] Profile` | `Off`, `High`, `Balanced` (default), `Performance`, `Custom` | detail vs cost |
| same, `DisableShadows`, `ToggleKey`, `MenuTab` | `0`, `ctrl+shift+h`, `MainMenu`/`UserPrefs`/`None` | hair casts no shadows; runtime toggle; where the in-game tab goes (restart to apply) |
| same, `[Custom]` `PersonBudgetK`, `SceneBudgetK`, `DensityMax`, `CurveDensityMin`, `HairMultiplierMin`, `IterationsMax`, `WidthCompensation` | `600`, `1500`, `1024`, `12`, `4`, `2`, `1.5` (= Balanced) | the cost units are thousands of segments |
| same, `[HairPhysics] Rate`, `UpdateCap` | `VaM` / `Auto` / `45`..`288`; `1`..`3` | hair on the body's steps or its own clock |

Profiles (person budget / scene budget in thousands of segments, density cap, iteration cap): High 1200 / 3000, 2048, 3;
Balanced 600 / 1500, 1024, 2; Performance 350 / 800, 640, 2. The ini is re-read about once a second; no restart is needed
except for `MenuTab`. The in-game tab's label is Chinese in the code (the plugin was written for a Chinese UI).

## Measured

All numbers come from the development machine ([frame-budget.md](frame-budget.md)); "simulated headset" is the desktop
harness that presents as a headset, not a real one. Nothing here was measured in a real headset.

- **Strands vs Unity vs merge**, offline replay of one heavy-hair character (resolution not recorded; approximate):
  Unity 3.28 ms, merge 1.72 ms, strands 0.62 ms. Simulated headset, full density, opaque pass: strands -1.77 ms against
  Unity; once the budget limits density, strands is about equal to merge.
- **Where the hair cost goes**: 0.6-2.7 ms per frame for three characters in a simulated headset; only GPU-bound scenes
  lose frames. The same setting sometimes cost 1.3-7x more at the same GPU load; cause unknown.
- **Hair GPU time with strands** is about 0.53x that of merge for the same settings (0.44x to 0.67x across scenes), with
  the same shape, so the cost model above holds for strands unchanged.
- **Solver, per item per step**: 0.286 -> 0.123 ms GPU in a simulated headset; a scene of 14 small items 27.8 -> 32.5 fps,
  a three-person scene 33.5 -> 35.9 fps, one 65k-particle item within noise. Agreement with GPUTools to about 1 micrometre
  per step (no cling).
- **Side by side**, offline, 21 items: 0.536 -> 0.062 ms. In game (a heavy two-character scene) about -0.5 ms per frame.
- **One solve per frame** (a frame with two hair steps): offline solves 2400 -> 200 and GPU per frame 0.64 -> 0.53 ms (with
  cling 1.86 -> 0.51 ms), particle hashes identical across 1, 2 and 3 steps per frame, with and without cling, on
  D3D11On12 and plain D3D11. In game (heavy two-character dance scene, 11 hair items all on the solver, path tracing, RR
  at 33% input, simulated headset): solves per frame 5.9-6.9 -> 1.3; real fps without frame generation 55.89 / 54.83
  against 53.03 / 50.78 (alternating on/off in one session); frame GPU 17.87 / 18.23 against 18.83 / 19.67 ms;
  frame generation x4 held its 36 fps cap either way.
- **Per-step GPU timestamps** make the items wait for each other again (21 items 0.062 -> 0.607 ms): time with
  `SetTiming(false)` and compare frame times.

## Pitfalls

- **Ints packed as float bits in a `Vector4`** become denormals and are flushed to 0 by VaM's Mono when copied. A first
  version sent each side's sphere count that way and hair never touched the chest, and an offline test (writing bits from
  C++) could not see it. Send counts as ordinary float values.
- **Crash with `d3d11` on top and `VaMHairOnePassNative` below it** is usually only the plugin's context hook forwarding
  a call unchanged (the `Map` hook's return address is the frame); the caller below it passed the bad argument. One such
  crash was a null buffer mapped by the frame-generation compositor, nothing to do with hair.
- **D3D11 debug layer** breaks the plugin's template-table hook (do not add `--debug` to DLL tests). Patching only the
  live context table is not enough: the runtime rewrites Draw slots on Flush, so the `d3d11.dll` template must be patched
  too.
- **Observed constant buffers** must be `AddRef`ed and swept periodically, or address reuse makes the CPU copy overrun
  (heap corruption). HLSL `line` is a keyword.
- **Savings go to frame rate, not density.** Draw cost halved by strands did not re-tune the budget profiles for denser hair;
  Balanced under strands is about Performance under merge. A denser look is a new, higher profile; the profiles aimed at speed
  do not move ([protocol/performance.md](../protocol/performance.md)).
- **Budget values do not follow camera distance.** An earlier version rebuilt hair LOD as the viewer moved (22 rebuilds while
  walking) and scanned the scene every 2 s: with four such timers (two ~95 ms frames every 2 s in a scene with many
  objects) the periodic hitch was ours. Settings that decide content are fixed for a situation and recomputed only when
  an item appears or goes, or when the player changes a setting. Never `FindObjectsOfType` on a timer
  ([design-rules.md](../protocol/design-rules.md)).
- When comparing costs, alternate on/off inside one VaM session: the same hair settings cost 1.3-7x more in some runs.

## Tried and dropped

- **Native rewrite of tessellation + geometry stages** as one compute pass: triangles matched 1:1 (38 variants, 5 scenes), but
  geometry is only 10-45% of the hair pass; best replacement 0.91-1.16x. Not a bottleneck.
- **Pre-seeding the depth buffer** for the frame generator's hair motion-vector band (2 cm wide): it cut pixel-shader
  invocations 2-3x with identical output but needed two full-screen passes and was slightly slower; the band was measured
  at 0.11-0.13 ms (0.1-0.3 ms at headset resolution) and is not the bottleneck.
- **Per-strand direct length solve, long-range attachments, DFTL, curl-spring compliance** in the solver: removed.
- **Tracing inter-strand multiple scattering** for path-traced hair: too costly; replaced by the closed-form term.
- **Simulating every drawn strand** ("mode B"): validated only offline; needs load-time encryption and its own renderer.

## Open

- Hair resting on the soft body's breast: the presented spheres leave gaps between them (drawn surface outside the spheres
  by a median 1 mm, 95th percentile 6.4 mm), so hair that lands in a gap can sink in; denser spheres or deeper, larger
  spheres were identified but not done. The soft body is not in this repository.
- Three hair items in one scene showed particles differing from GPUTools' by 2-8 mm in an occasional single-step check
  (none had cling), and one item showed a non-finite difference. The cause was not found; the check's procedure is the
  same as for earlier versions.
- Real-headset behaviour of all three components is unverified; the chest contact rule was checked only offline
  (36 contact cases on WARP).
- Style mode and `RebindData` are untested in the solver (style mode is left to GPUTools).

## Verify

- **Budget arithmetic**: `components/runtime/hair-budget/tests/BudgetMathTest.cs` runs the plugin's own
  `HairBudgetMath.cs` over a library export (one row per hair item of each person preset) with the High / Balanced /
  Performance limits and checks the invariants (budget respected, floors held). Offline, no game.
- **Solver against GPUTools**: set `[Diagnostics] Diagnostics = true` and `VerifyEverySteps = N`; every N steps per
  item GPUTools' step runs on the same particles and the results are compared (log and `Stats()` JSON). Use
  `Cling = Original` for micrometre agreement on hair with cling. Needs the game (a simulated headset is enough); timing needs
  `SetTiming(false)` for a frame-time comparison. The development tree also had an offline GPU replay harness and a WARP
  contact-mechanics test; they are not in this repository.
- **Strands against Unity's draws**: `Report()` (JSON of strand and merge statistics) and `Snapshot(path)` from the test
  agent ([tools/lab/README.md](../../tools/lab/README.md)); compare the eye target with `Strands(false)`. Compare the
  path that actually executed ("strands ready" / "not ready (merging)" in the log and `"strands"` in `Report()`), not the
  requested mode.
- Only the game shows: hair collision with a soft body, transmission look, behaviour in a real headset.
- Hair misbehaving or a new crash: first the log lines "VaM hair one-pass", "VaM hair solver", "VaM hair budget"; then
  `Enabled = false` on the component under suspicion (the shading plugin hooks D3D11 draws in every run, so it is the
  usual first suspect).
