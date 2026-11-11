# Clothing: shading, physics, wrap batching and see-through parts

VaM clothing costs in three places: the pixel programs that light it (VaM reuses the skin's one-lobe Blinn-Phong code), the
GPUTools cloth simulation (hundreds of tiny dependent compute dispatches per garment per physics step), and the skin-wrap
that glues every garment onto the body each frame (about a dozen more dependent dispatches per garment). A player sees the
same garments, with a fabric sheen instead of plastic highlights, and a character in many garments runs measurably faster.
Components: `components/shading/cloth-shading`, `components/runtime/cloth-physics`, `components/runtime/wrap-batch`.

## How it works

### Shading: `components/shading/cloth-shading`

**VaM's path.** Every garment goes through `DAZSkinWrap.DrawMeshGPU` -> `Graphics.DrawMesh` with the one shader
`Custom/Subsurface/TransparentGlossNMNoCullSeparateAlpha(ComputeBuff)`: ForwardBase (SrcAlpha blend, writes depth), one
ForwardAdd pass per pixel light (One One) and the fallback's shadow caster; the bounding box is 10 km. A garment's pixel
cost and its point-light shadow maps are the render-side cost (0.25-1 ms per real frame for a close garment).

**Which pixel programs are replaced.** `tools/gen_cloth_programs.py` lifts each of the shader's ForwardBase / ForwardAdd
pixel programs (30 in the development build) from the game's bytecode into HLSL and recompiles it with a mode in a
constant buffer at b12 (`native/cloth_programs.h`, generated, not committed). It reuses the skin component's generator to
find the same places by data flow (the clothing programs share the skin programs' lighting code: the exponent, the N.H
power, the specular normal, the diffuse N.L and its light vector, the view vector, the IBL mip). Mode 0 reproduces the
original program (tests only; in game a mode-0 draw keeps VaM's own program). The native library
(`native/cloth_shading.cpp`) identifies VaM's clothing programs by the SHA-1 of their bytecode (the device's
`CreatePixelShader` is patched so each shader is checked as it is created, and the replacement is created next to it),
follows `PSSetShader`, and on a registered draw binds the replacement with b12 and puts both back afterwards. The context
function table hooks are chained (template and live table) so they coexist with the skin and hair modules in any order.

**Which draws: real clothing only.** The plugin (`VaMClothShading.cs`) sees each `DAZSkinWrap` under a `DAZClothingItem`
when it draws (Harmony postfix on `DrawMeshGPU`; no scene searches) and classifies it once (`Classify`): not a garment if it
lies on the skin (95% of sampled vertices within 1 mm of it: makeup, wet layers, decals) or is small (base mesh under 0.15 m on
every side: eye overlays, rings, earrings). Each garment submesh's (index buffer, first index) is registered
(`CS_SetDraws`); only those draws with a clothing program bound go through the replacement. Nails, shoes and liquid decals
count as clothing too, but high-gloss materials stay on the GGX side of the blend, which looked acceptable.

**The cloth look (mode 1).** VaM's specular lobe (normalised Blinn-Phong, exponent from Gloss and the gloss map) is replaced
by: GGX with `alpha^2 = 2/(n+2)` (the same peak), Smith height-correlated visibility, times N.L, times N.V (VaM's lobe falls
as N.V over the hemisphere, GGX's does not; multiplying keeps the author's strength at every view angle). As the material
gets rough, a Charlie sheen lobe (Estevez and Kulla) with Neubelt visibility takes over (weight 0 at alpha `SheenFrom`, 1 at
`SheenFull`), the cloth lobe of common real-time engines. Specular anti-aliasing widens `alpha^2` by the screen-space
variance of the specular normal (Tokuyoshi and Kaplanyan: `+ min(2 sigma^2 (|ddx n|^2 + |ddy n|^2), kappa)`), and the
environment cube mip moves with it. VaM's specular colour (spec map x `_SpecColor` x its fresnel curve x `_SpecInt`) stays the
colour of both lobes; the diffuse (VaM's wrap with `_SubdermisColor`), the alpha, the decal and the environment light are
VaM's. At grazing angles the Charlie sheen dominates, consistent with published measured cloth behaviour (brighter, tinted,
stronger fresnel peak). Total energy is within 10% of VaM's highlight.

**Path-tracer interface.** `CS_GetPathMaterialV1(ib, first, parameters)` answers, for a draw's (index buffer, first index),
whether it is a registered garment (and `Mode > 0`) and returns the live look parameters. The path tracer (see
[path-tracing.md](path-tracing.md)) uses the same registration and the same controls: no second scene scan or classifier; it
treats garments as clothing material (sheen, grazing behaviour) while the skin-contact layers stay a separate role.

### Physics: `components/runtime/cloth-physics`

**What GPUTools does.** For each simulated garment and physics step, `ClothPhysicsWorld.DispatchPhysicsImpl` issues
`outer x (iterations x (3 + colour groups) + ~6)` dispatches: 100-3000 per garment per step, each with a fixed 3-4 microsecond
cost regardless of work, and garments one after another. GPUTools keeps the first 20 frames after a reset, neighbour
collision, garments without skin-wrap targets and those above 65536 particles, and this plugin leaves those alone.

**The fused step** (`solver/cloth_fused.hlsl`, `plugin/native/cloth_physics_native.cpp`, `plugin/VaMClothPhysics.cs`): a
Harmony prefix queues the step for a native library on Unity's D3D11 device (`GL.IssuePluginEvent`). Same particles, point
joints, skin-wrap targets, colliders, GPUTools' rules and parameters (`InitData` as GPUTools runs it), in `1 + 2 x outer`
dispatches per garment and step:

- `CSImport` (a thread per particle): GPUTools' particle buffer -> working buffers X / S.
- per outer step: `CSIterate` (ONE thread group per garment) does, per iteration, integrate, own-particle point joints, every
  joint colour group with a group barrier between them (where GPUTools dispatches per group), then the inner velocity;
  `CSCollide` (thread per particle) does collision reset, plane, sphere and capsule collision, grab joints, the velocity, and
  writes GPUTools' particle buffer back.
- Garments up to `MAX_SHARED` = 2720 particles keep positions in group-shared memory. Larger ones iterate in device memory.
- Joints in one colour group never share a particle (a recorded-input check), so a thread loads several joints before
  storing any (`JOINTS_IN_FLIGHT`): same result, fewer memory round trips on the single multiprocessor.
- Joints are packed `(A | B << 16, Rest)`: garments must have fewer than 65536 particles. The plugin takes only garments where
  every particle has exactly one point joint with `BodyId` equal to its index (all recorded garments did).

**Side by side.** The queued steps of all garments run together at the end of the physics step (`WaitForFixedUpdate`), the
dispatches interleaved (the large garments first). Anything that would touch a queued garment's inputs or results first
(the shared collider update, the garment's render-frame work, a second step of the same garment) runs the queue before it.
On D3D11On12 the native library flushes after import, after each outer step's iterate and after collision, because the
translation layer inserts a UAV barrier before a dispatch for any earlier-UAV resource and NVIDIA waits for idle on it; a new
command list per phase makes the garments truly overlap ([d3d11on12.md](d3d11on12.md)).

**Per-scene assignment** (`Assign`): solver garments overlap (a solve takes about its slowest garment plus 0.12 of the rest)
while GPUTools' garments run one after another. A fitted estimate per garment (solver: from particles and joints; GPUTools:
`0.29 + 0.00537 x iterations x colour-group entries + 0.00046 x thousand joints` ms) decides which garments go back to
GPUTools: the most expensive on the solver are given back as long as the physics step gets shorter. It is recomputed when
the set of active garments (stepped in the last 60 frames), the iteration count or the native garment set changes. A
17k-particle jacket went back to GPUTools in a scene where it was the critical path; every other garment in that scene stayed.

**Bit-identity and how it is checked.** The fused step keeps GPUTools' arithmetic and order, so results are bitwise equal to
GPUTools' on the same GPU. Evidence: (1) an offline replay of four recorded scenes in a harness (`cloth_harness`; not in this
repository) comparing against positions recorded in game, 0 m; (2) in game, `[Diagnostics] VerifyEverySteps`: every N steps
per garment GPUTools' own step runs on the same particles and the results are compared. WARP (software D3D) is not a valid
comparison: floating-point differences are amplified to millimetres by collision.

**Large garments on several thread groups.** A garment in device memory (above 2720 particles) iterated in one thread group,
colour group after colour group, so with large colour groups that single multiprocessor is the step's critical path.
`MultiGroups()` gives garments whose colour groups average at least `MultiMinJoints` = 1800 joints 4-8 groups (one per 512
joints of a colour group); `CSIterateMulti` (built with `MULTI`) spreads the joints of each colour group over them, with a
counter in the garment's `Sync` buffer standing in for the group barrier (`GridSync`: every group counts in after its last
thread's stores, then spins until all have arrived). Same joints per colour group, same arithmetic; only which thread
solves which joint changes; positions stored by one group must be visible to others, so `X` is `globallycoherent`.
Cost per colour group: about 2.0 + 0.2 x J microseconds multi-group (J = thousand constraints) against 0.5 + 1.55 x J
single-group, hence the 1800 threshold (below about 1400 joints per colour group, multi-group was slower).

**The fxc barrier-merging pitfall.** fxc merges two group barriers of the same kind with only UAV work between them into
one (a miscompilation here: results were off by 4-18 mm). `GridSync` therefore uses `AllMemoryBarrierWithGroupSync` for one
and `GroupMemoryBarrierWithGroupSync` for the other, passes a value through group-shared variables across each (`gsMark`,
`gsRelease`) so the code after depends on it, and the disassembly must show both `sync_uglobal_g_t` and `sync_g_t` at every
site. Check this after any edit of the barrier code or a compiler change.

**The give-up path.** A cross-group barrier needs every group of the dispatch resident at once; oversubscription would hang
the GPU. So one solve gives at most `kMultiCap` = 32 groups across all multi-group garments (the development machine has
170 multiprocessors, one 1024-thread group each; **this is a GPU-dependent constant**, see Settings) and the rest go
single-group. A wait that ends in neither arrival nor release gives up after `SPIN_LIMIT` = 50000 polls, sets a sticky flag
in `Sync[2]`; the other groups see it and stop waiting. The native library reads the flag back every 8 solves of that
garment (`kSyncEvery`); once set, that garment iterates in one group for the rest of the session and the log says so.

### Wrap batch: `components/runtime/wrap-batch`

**Why it is slow.** Every garment VaM wraps onto the skin runs `DAZSkinWrap.LateUpdate` each frame: about a dozen small
dependent compute dispatches on a few thousand vertices each (copy last frame's vertices, normals and tangents for the
one-frame display delay, SkinWrap, smoothing, MapVertices, surface and vertex normals, mapped normals, tangent
directions, tangents). On D3D11On12 every dispatch waits for all earlier work (about 3-4 microseconds each with an almost
empty GPU), so a character in many garments pays for the count, not the work.

**What the plugin does** (`VaMWrapBatch.cs`, a Harmony transpiler on `DAZSkinWrap.LateUpdate`, `UpdateVertsGPU` and the
`MeshSmoothGPU` / `MapVerticesGPU` / `RecalculateNormalsGPU` / `RecalculateTangentsGPU` methods): each wrap's
`ComputeShader` calls (`SetBuffer`, `SetFloat`, `Dispatch`; also `SetInt`, `SetVector`, `SetBool`, `SetFloats`,
`SetTexture`) are recorded instead of issued; the CPU side of `LateUpdate` still runs as VaM wrote it (checks, the smoothing
ping-pong, materials, `DrawMeshGPU`). Before anything reads or rewrites what those dispatches touch, the recorded work
replays: every waiting wrap's first dispatch, then every second, and so on, each with the calls that preceded it in its own
wrap, and on D3D11On12 a `Flush` between phases (a new command list: buffers start in COMMON, so the phase needs no
barriers). "Before" is: `Application.onBeforeRender` and any camera's `PreCull` (the end of the frame's LateUpdate work); the
`LateUpdate` / `Update` / cleanup of a skin a waiting wrap reads (VaM's skinning and everything that rewrites that skin's
buffers); `DAZSkinWrap.Dispatch` / `PostProcessDispatch` / `GPUCleanup` of a waiting wrap. Every dispatch reads and writes
what it did before in the same order within its wrap, and a wrap reads only its skin and its own buffers, so the result is
bitwise the same.

**Simulated garments too.** A simulated garment's render-frame work is `GPUTools ClothSettings.LateUpdate`: copy the wrap
targets to last frame's, the wrap's `Dispatch` (skin wrap onto this frame's skin), the cloth's `DispatchImpl` (GPUTools
kernels: draw positions from particles, vertex-only data) and the wrap's `PostProcessDispatch`: about 15 dependent
dispatches. The whole `LateUpdate` is recorded as one job (GPUTools' `KernelBase.Dispatch`, `BindAttributes` and the wrap's
`Dispatch` are transpiled too) and runs in the same batch. This is valid because GPUTools rebinds every buffer and value
before each dispatch (there is no "already bound" cache), so interleaving garments that share a shader changes nothing; its
results are read by the next physics step and by the renderer, both after the batch.

**Not recorded** (run as before): wraps not yet initialised; wraps that provide matrices to hair or cloth; simulated garments
that provide matrices; simulated garments with accessories (`BuildClothAccessories` reads particles back in the middle of the
chain, via `OutParticles.PullData`). Before a physics step (`DispatchPhysicsImpl`) any pending replay is run first.

**Verify.** `Verify(frames)` / `VerifyResult()` rerun VaM's own `LateUpdate` (and `ClothSettings.LateUpdate`, with its frame
counter and last-frame targets put back) on the same inputs and compare every buffer: wrap targets, particles, vertex-only
data, drawn vertices, normals, tangents and the delayed copies. The native library is only a render-event handle and a
flush (`native/wrap_batch_native.cpp`).

### See-through and hidden clothing (facts)

- Authors hide clothing parts by setting Alpha Adjust to -1 on a see-through shader. The materials in question are VaM's
  `Custom/Subsurface/*SeparateAlpha*` family, whose alpha is `saturate(_AlphaTex.a + _AlphaAdjust)`; at -1 nothing shows.
  VaM still draws such parts (transparent queue, 2450 and above; they write depth). **Raising Alpha Adjust globally to make
  sheer clothing less see-through also reveals every hidden part**: microphones, props and other items authors hung on
  characters show up. A plugin that added +0.2 to negative Alpha Adjust on garments was installed and removed for that
  reason; do not repeat it. A change that lets garments look less transparent must keep -1 meaning "not drawn".
- The two places that honour this are in this repository: the frame-generation bridge does not redraw a material in the
  `SeparateAlpha` family with queue >= 2450 and Alpha Adjust <= -0.999 for motion (`Hidden()` in
  `components/render/dlss-bridge/VaMDLSSBridge.cs`; it logs the first 24 as "hidden clothing part ... not redrawn for motion"),
  and the path-traced scene does not put them in (`DrawsNothing`, `MaterialFor` in `components/shading/skin/RtScene.cs`).
  The rule is limited to that shader family because hair shaders (`Custom/Hair/*`) and opaque shaders that an author put in a
  see-through queue keep a stray Alpha Adjust and still draw; an earlier, broader version removed hair motion.
- Why hidden parts matter beyond looks: if their depth and motion reach the frame generator, a part floating in front of a
  face takes the face's pixels and the face's outline shimmers. See [motion-vectors.md](motion-vectors.md).
- Sheer clothing in the path tracer: an attempt to match raster's look (light and shadow treat translucent cloth as a solid
  occluder, a single alpha multiply under point and spot lights) produced ugly shadows and was rolled back. Translucent layers
  enter the denoiser's guide buffers by coverage, blended per layer, not by a 0.5 cut-off, so thin cloth over skin is not
  guided as skin ([denoiser.md](denoiser.md)).

### Cloth explosion root cause (development tree, for context)

A character's skirt and blouse once burst apart in one dance scene. It was not the cloth plugin and not the scene itself:
two systems were pushing the same chest and hips at once (the development tree's breast soft body and the scene's own breast
physics plugin; the soft body failed to disable the scene's plugin because its script used fields where the soft body looked for
properties). Skin on the torso then jittered at high frequency in a still pose (breast median 5.6 mm per frame, maximum 19 mm),
garments pinned to that skin followed, and a skirt pinned only at the waist with low drag was amplified into a wild swing
(15 s after load, the skirt's median / farthest distance from its wrap target 0.50 / 1.28 m with the soft body, 0.027 / 0.32 m
with all our plugins removed, and the same with only the soft body removed). The fix was in the soft body. If a garment bursts:
measure each garment's distance to its wrap target over time, switch plugins off one at a time to separate causes, and look for two plugins driving the same body region. A
scene's own timeline may also legitimately undress a character (allowing garments to fall, then hiding them); that is not an
explosion.

## Settings

| File / section / key | Values | Trades |
|---|---|---|
| `BepInEx/config/local.vam.clothshading.cfg` `[General] Mode` | `1` (default) / `0` | `0` = VaM's own clothing shading |
| same `[Cloth]` `SheenFrom`, `SheenFull` | `0.25`, `0.5` | GGX alpha where sheen starts and fully replaces the highlight (VaM Gloss 7 ~ 0.13, Gloss 4 ~ 0.35) |
| same `SheenGain`, `SpecularGain` | `2`, `2` | brightness of the sheen (2 = VaM's highlight at a 45 degree view) and the highlight (2 = VaM's peak) |
| same `AntiAliasing`, `AntiAliasingCap` | `0.159`, `0.18` | highlight widening by normal variance (0 = off); cap on what it adds to `alpha^2` |
| `BepInEx/config/local.vam.clothphysics.cfg` `[General] Enabled` | `false` in code; install turns it on (`manifest/components.toml`) | `false`: GPUTools simulates |
| same `[Diagnostics] Diagnostics`, `VerifyEverySteps` | `false`, `0` | tests; verify stalls the render thread for the read-back |
| `BepInEx/config/local.vam.wrapbatch.cfg` `[General] Enabled` | `true` | read at startup |

Constants in code worth knowing on a different GPU: `MultiMinJoints` (1800) and `MultiGroups` (4-8) in
`VaMClothPhysics.cs`, `kMultiCap` (32) and `kMaxMulti` (16) in `cloth_physics_native.cpp`, `SPIN_LIMIT` in the shader. The
cap must stay below the number of thread groups the GPU can keep resident (multiprocessors x 1024-thread groups per
multiprocessor); on a smaller GPU lower `kMultiCap`, or switch multi-group off (`SetMulti(false)` in the test API). The give-up
path protects against a hang but a hung GPU may still hit the driver's timeout.

## Measured

Development machine, simulated headset unless noted ([frame-budget.md](frame-budget.md)); no real-headset number exists.

- **Fused solver vs GPUTools** (in game, cloth GPU time): -17 to -83% by scene (first version); the second version
  (flushes between phases) on a light scene 1.20 -> 0.53 ms per frame (103.6 -> 114.4 fps) and on a CPU-bound scene
  31.8 -> 32.8 fps, p95 frame time 38.0 -> 34.2 ms. Bit-identical in every comparison.
- **Multi-group iterate** (heavy two-character dance scene, path tracing, RR 33% input, two runs alternating in one VaM
  session): garment solve GPU 2.8 -> 1.9 ms per frame (1.84 / 1.95 against 2.78 / 2.87 in the two pairs); real fps without
  frame generation 53.89 / 49.35 against 48.93 / 46.39; probe-frame GPU 18.54 / 20.26 against 20.42 / 21.52 ms; frame
  generation x4 34.56 against 34.30 (at the 36 fps cap). In game, three garments used 7, 6 and 4 groups, nine stayed single;
  every 10th step was compared against GPUTools on each garment, 47 comparisons per garment, all 0 m, no give-ups.
- **Wrap batch** (same scene): a character with 36 wraps against another with 12: `LateUpdate` GPU 2.9 against 0.9 ms
  (the cost is the dispatch count). Batching wraps: about -0.2 to -0.25 ms GPU per frame, offline bit-identical. Adding
  simulated garments' render chain: probe-frame GPU -0.29 ms; real fps without frame generation +0.75 (two runs alternating);
  11 simulated garments all joined (24 recorded jobs, 271 dispatches, 15 phases per frame); `Verify(3)` over 3 batches of 72
  garments, 0 mismatches.
- **Cloth shading**: render-side savings were not measurable: skipping the 25 shadow taps where a light's attenuation is 0
  saved nothing in game; a tighter bounding box drew fewer primitives with the same time; the per-light compositing pass
  cost at most 0.1 ms; drawing opaque garments before the skin averaged zero (drawing all garments first lets stockings and
  lace hide skin). The look costs nothing measurable.
- **Index-buffer read** (see pitfalls): the first garment each frame waited 4.5-5.5 ms for the render thread in a scene with 25
  garments; with a pointer cache `LateUpdate` fell 7.8 -> 0.6 ms but the wait moved to `PlayerSendFrameStarted`, so frame rate
  did not change.

## Pitfalls

- **`Mesh.GetNativeIndexBufferPtr()` stalls the main thread** until Unity's render thread catches up (4 to 5 ms a frame
  with one garment when it was read every frame). `VaMClothShading.cs` `Track()` therefore reads the pointer once per
  mesh and again only when the wrap's mesh is replaced or a Harmony postfix sees that mesh's index data rewritten; a
  buffer that is not there yet is asked for again after a second. The every-frame read did not change the frame rate
  when the GPU was the limit (the wait only moved), but it dominated scene loading, where hundreds of frames each
  waited. Do not reintroduce a per-frame native pointer read anywhere; `SetIndexCache(false)` exists only for comparison.
- **GPUTools clothing steps' arithmetic is driver-compile sensitive.** Adding code around the collision loop (for example
  culling) left the DXBC arithmetic unchanged, yet NVIDIA's driver rounded differently and results drifted to millimetres in
  15 steps. Around anything that must stay bit-identical add no code; after a change compare against GPUTools' recorded
  particles on the target GPU. `fxc` and the system `d3dcompiler_47` at /O3 produce the same output.
- **Recorded inputs satisfy conditions the solver relies on**: every particle has exactly one point joint with `BodyId` = its
  index; joints of one colour group share no particle; no neighbour collision. Recorded `GroupsData` held two copies because the
  recording probe repeatedly toggled simulation and GPUTools appends on each rebuild; in a normal game it is one copy, and the
  plugin reads the live table.
- Garments simulated in game are fewer than recorded (hidden or non-simulating ones do not run). With only one device-memory
  garment left, single-group iterate is slower than GPUTools (a jacket: 2.1 against 0.9 ms per step), which is why
  `Assign` and the multi-group path exist.
- **A GPU that is not saturated** (CPU-bound scene): flushes leave the GPU waiting for the next command list, which counts in
  GPU timestamps (a submission measured 1.36 -> 3.03 ms) but not in frame time. Judge by frame rate and p95 over the same
  window, not by submit timing.
- **Timing perturbs**: running the offline cloth test or builds while timing in game raised baselines (up to 0.6 ms).
  Measure when the GPU is idle.
- `StiffnessJointsKernel.StiffnessJoints` is not a public property (read its getter by reflection); `GPUCollidersManager.FixedUpdate`
  recomputes the shared sphere / capsule buffer every physics step.
- The cloth plugin's collision does not see a soft body's colliders; only GPUTools' own sphere and capsule colliders register.

## Tried and dropped

- **Batching every cloth dispatch by time slot** (merging dispatches across garments): about 7 microseconds per slot, only
  slightly faster in scenes with many large garments; removed.
- **Prefetching the next colour group's tables, changing iterate thread counts**: no gain; single-group iterate is not read
  latency bound. HLSL `?:` evaluates both sides.
- **Other solvers** (VBD, small-step XPBD, Jacobi + Chebyshev, OGC) change the look; worth doing only if a better look is the goal,
  then with side-by-side images. An asynchronous compute queue was investigated and not done.
- **Cloth shading speed-ups**: shadow-tap skipping, tighter bounds, per-light compositing, ordering opaque garments before skin
  ([Measured](#measured)): no measurable space in the render side without changing the picture.
- **Global Alpha Adjust raise** for sheer cloth: reveals hidden parts (above). **Solid-occluder shadows for translucent
  cloth** in the path tracer: ugly shadows.
- **A cross-group barrier** was first judged not worth doing; it was done once the multi-group measurement showed a 3x gain on
  the large jacket (the barrier, with the fxc pitfall).

## Open

- Hair or cloth acceptance in a real headset is unverified for all three runtime components.
- Garments with accessories and cloth that provides matrices stay out of the wrap batch; their share of cost is unknown.
- `kMultiCap` was sized for the development GPU only; the behaviour on small GPUs is predicted from the code, not tested.
- Sheer clothing in the path tracer: the notes record no accepted fix for garments that look too transparent there.

## Verify

- **Shading programs**: `components/shading/cloth-shading/tests/cloth_test.cpp` (build: `tests/build_test.cmd`; run
  `cloth_test <ClothShading dir> [hash ...]` after `tools/gen_cloth_programs.py` has produced the programs and cases): for each
  clothing pixel program the original and its replacement draw the same 32 x 32 grid of random inputs; mode 0 must reproduce the
  original, mode 1 must change pixels and give no NaN or infinity where the original has none. The D3D debug layer, when
  installed, checks linking and bindings. Offline, no game. Look comparison needs the game: a frozen scene with a garment
  rendered with `Mode` 0 and 1, side by side (only a still scene is comparable).
- **Cloth physics**: `Diagnostics = true`, `VerifyEverySteps = N`; read the log and `Stats()` (fields include
  `multi_gave_up`). Needs the game or a simulated headset. After editing `cloth_fused.hlsl`, disassemble and check both sync
  instructions.
- **Wrap batch**: `VaMWrapBatch.Verify(3)` then `VerifyResult()`; `Stats()`; `SetEnabled(false)` / `SetCloth(false)` to
  compare. Needs the game.
- **Hidden parts**: log lines "hidden clothing part (Alpha Adjust -1) not redrawn for motion" name the first 24; check them
  against what the author hid.
- Only a person looking at the image can judge the sheen look, and only a headset shows it as worn.
