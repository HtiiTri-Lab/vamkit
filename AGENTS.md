# Working on vamkit: notes for coding agents

You are probably here because someone wants this mod installed, tuned or fixed on their own VaM. Read this file first,
then `manifest/components.toml` (what each component is and where it goes).

## Hard rules

1. **Nothing generated from VaM goes into git.** Several shaders are VaM's own pixel programs lifted to HLSL and edited
   (skin, clothing, hair). The generators run on the user's machine and write to `out/gen/<component>/`, which is
   ignored. Never commit `out/`, DXBC, `.bin`, `.dll`, `.pkl`, listings of VaM's IL or of its compute kernels, or
   anything else read out of `VaM_Data`.
2. **No NVIDIA binaries or SDKs in git.** They come from `vamkit fetch` into `third_party/` (ignored).
3. **Vanilla stays vanilla.** `launch.py --variant vanilla` must start VaM with VaM's own `openvr_api.dll`, no
   `d3d11.dll` of ours, BepInEx switched off, and none of our environment variables. It is the user's way back.
4. **Ask before touching the user's VaM folder.** Commands that change it print a plan and need `--yes`. Keep VaM's own
   files in `<VaM>/vamkit/original/` before replacing anything; uninstall must leave the folder as it was.
5. **One switch for path tracing, in game.** `[RayTracing] Renderer` in `BepInEx/config/local.vam.skintransmission.cfg`
   (`Legacy` = off, `PathTracing` = on), set by the menu. The compositor follows it every frame
   (`vam_performance_config.h` `VamPathRequested`): on means RR F at `RenderScaleRR`; off means `DLSSPreset`
   (`Off` / `K` / `L` / `M`) at `RenderScaleK` / `L` / `M`. Keep the menu, the launcher and the renderer agreeing.

## Where to read

| You want to | Read |
|---|---|
| Install on a new machine | `docs/agent/install.md`, `docs/agent/runtimes.md` |
| Tune for a GPU, CPU or headset | `docs/knowledge/frame-budget.md`, `docs/knowledge/settings-and-menu.md`, `docs/protocol/performance.md` |
| Change or extend a component | its `README.md`, `docs/agent/modify.md`, the knowledge document it names |
| Find the cause of a problem | `docs/agent/troubleshoot.md` |
| Run on another VaM build | `docs/agent/adapt-vam-version.md` |
| Decide how to test or measure | `docs/protocol/testing.md`, `docs/protocol/performance.md`, `tools/lab/README.md` |
| Touch the user's VaM folder | `docs/protocol/changes.md` |
| Design something new | `docs/protocol/design-rules.md`, `docs/knowledge/rejected-and-open.md` |

## Commands

```
python tools/vamkit doctor                      what is there, what is missing; changes nothing
python tools/vamkit fetch --accept-licenses     third-party SDKs and DLSS runtimes into third_party/ (pinned)
python tools/vamkit build [ids] [--steps ...]   generators, native libraries, plugins into out/<id>/
python tools/vamkit test [ids]                  the offline tests
python tools/vamkit install [--yes]             the build into the VaM folder; without --yes only the plan
python tools/vamkit verify                      the install's record against the VaM folder and the build
python tools/vamkit rollback [--yes]            takes the last install back
python tools/vamkit uninstall [--yes] [--purge] the VaM folder as it was before the first install
python tools/vamkit status                      which installed files are present in the VaM folder
python tools/check_manifest.py [--built]        the manifest against the tree (and, after a build, its outputs)
python tools/check_docs.py                      every path and link the documents name exists
```

The VaM folder comes from `--vam`, `VAMKIT_VAM` or one line in `local/vam.txt`. How each component is built (its
generators, its build script, its plugin's sources and references, its tests) is in `manifest/components.toml` and
nowhere else. The commands that change the VaM folder print their plan and do nothing without `--yes`; nothing in
`tools/vamkit` starts the game.

## Layout

```
manifest/components.toml   each component: path, build kind, what it needs, where its files install
manifest/thirdparty.toml   pinned third-party downloads
components/render/         d3d11-proxy, opencomposite, ofxr-dlssg, dlss-bridge, vr-menu, launcher, window-camera
components/shading/        skin, rt-core (path tracer), hair-strands, cloth-shading, tone-mapping, display-lighting
components/runtime/        cpu-boost, gc-tuning, hair-solver, hair-budget, cloth-physics, wrap-batch
components/shared/         shader-lift (reads Unity shader data, lifts DXBC)
tools/vamkit/              the command line (Python standard library only)
tools/lab/                 test and measurement tools: simulated headset scripts, test agent, CPU census
docs/                      agent guides, knowledge per topic, working protocol (see the table above)
```

Installed layout inside the user's VaM folder:

```
<VaM>/vamkit/launcher/     the launch panel (LaunchPanel.py) and launch.py
<VaM>/vamkit/bin/          openvr_api.dll (OpenComposite), d3d11.dll (proxy), ofxr/, dlss/{sr,nr,fg}
<VaM>/vamkit/original/     VaM's own files as the install found them (<name>.absent when there was none)
<VaM>/BepInEx/plugins/     our plugins. Render and shading plugins are dormant unless launch.py set their environment;
                           the runtime plugins work whenever BepInEx loads (vanilla switches BepInEx off)
```

## Settings an agent may change for the user

| What | Where |
|---|---|
| Path tracing on/off | `[RayTracing] Renderer` (skin cfg), or the in-game menu |
| Path tracing bounces, background savings | `[RayTracing] PathBounces` 0-3, `PathBackground` Off / Far / All |
| Upscaler model and scale | the run's `performance.ini` `[Performance]` `DLSSPreset`, `RenderScaleK/L/M/RR`, `OutputScale` |
| Denoiser | `[NR]` keys in the same file |
| Skin scattering, light through thin parts | skin cfg `[Subsurface] Strength` 0-1, `[General] Strength` 0-2 (menu: skin page) |
| Turn a shading part off | skin `[General] Enabled`, hair `Enabled`, clothing `Mode = 0`, tone mapping `Mode = 0` |
| Turn a runtime part off | `Enabled = false` in `local.vam.cpuboost.cfg`, `.hairsolver.cfg`, `.clothphysics.cfg`, `.wrapbatch.cfg`; GC: `FreeSpaceDivisor = 3`, `MarkThreads = 0`, `AllocatedBytesPerFinalizer = 0` in `.gctuning.cfg`; hair budget: `Profile=Off` in `VaMHairBudget.ini` |
| Worker threads | `[General] Threads` (cpuboost) and `[Mono] MarkThreads` (gctuning) are upper bounds; both are capped by the machine's processor count |

The launch panel keeps one preset per display mode (`vamkit/launcher/profiles/VR`, `.../Desktop`) and writes it into
the plugin configs at each launch, so change settings through the menu or the preset, not only in the live cfg.

## Working rules

- Explain the cause from the code before running the game; use the game to confirm.
- After a change, say what you verified and what you did not (for example: built, not run in VR).
- The first path-traced frame after a shader change waits for the driver to compile the pipelines (about 26 s once).
- Keep upstream licenses and notices in `components/render/opencomposite` and `components/render/ofxr-dlssg`.
- The runtime plugins must give VaM's own results. Each has a verify mode (see `guard` in `manifest/components.toml`);
  run it after any change to them and on a VaM build you have not seen before.
- Native code that needs worker threads takes them from cpu-boost (`CB_Task` / `CB_Run`, found with
  `GetModuleHandleW` on `VaMCpuBoostNative.bin`) and runs on the calling thread when it is absent. No second pool.
