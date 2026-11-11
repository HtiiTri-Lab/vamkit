# Working on vamkit: notes for coding agents

You are probably here because someone wants this mod installed, tuned or fixed on their own VaM. Read this file first,
then `manifest/components.toml` (what each component is and where it goes).

## Quick start: pull it and use it

```
git clone <this repository> && cd vamkit
python tools/vamkit doctor --vam "<the user's VaM folder>"    # what is there and what is missing; changes nothing
python tools/vamkit fetch --accept-licenses                   # after the user agreed to the vendors' licence terms
python tools/vamkit build
python tools/vamkit test
python tools/vamkit install --vam "<VaM>"                     # prints the plan: show it to the user, then add --yes
python tools/vamkit verify --vam "<VaM>"                      # ok / LESS / FAIL for every check
python tools/vamkit fetch --lab --accept-licenses && python tools/vamkit build --lab     # the simulated headset
python tools/lab/session/first_run.py --vam "<VaM>"           # vanilla, the mod on the desktop, the mod in VR
```

`doctor` names every missing tool with what to install. The first run needs no headset and no person at the PC: it
starts VaM in the background, checks that every part runs as asked, and ends with 0 (all as asked), 2 (works with
less: each loss is listed with its reason and remedy, and is yours to mend) or 1 (a check failed).
`docs/agent/install.md` goes through each step, what to ask the user and when.

Then make it right for this machine: the card, the headset and its runtime, the VaM build. Measure with
`tools/lab/bench/bench.py` before and after a change (`docs/benchmarks/README.md`), find causes with
`docs/agent/troubleshoot.md`, and tell the user in their words what they get, what they get less of here, and why.
The user starts the game with `vamkit-launcher.bat` in the VaM folder; the launch panel and the in-game menus speak
English or Chinese (`components/shared/lang`: another language is one table).

## What this repository is for

It is a base, not a finished product with a fence around it. You are as capable as whoever wrote it. With its
architecture, sources, measurements, guides and tools you are expected to get the user whatever can be got on their
machine, including things this project never implemented: another upscaler or frame generator, a higher multiplier,
a denoiser for cards without RTX, another VaM build, another headset. Nothing here sets a limit on purpose.

Read the documents accordingly. "Not taken", "tried and dropped", "do not" and every default record what was chosen,
measured or got wrong, on one machine and for one person: they save you from repeating a dead end and tell you what
it would take to try again. They are not prohibitions. A number in the code that caps something (a multiplier, a
thread count, a list of models) is where the work stopped, not where it must stop. When the user wants more, find where
it plugs in (`docs/agent/modify.md`, "Building what is not here"), build it, and verify it the way the neighbouring
parts are verified.

Three limits are real, and they are the only ones:

- **Law.** What may be committed to a public repository or handed to others: nothing derived from VaM's files, no
  vendor binaries (rules 1 and 2). On the user's own machine you may generate, fetch and build all of it.
- **Consent.** The VaM folder, the settings and the choices are the user's (rules 3 and 4).
- **Wrong results.** A part that replaces VaM's own work must give VaM's results, and one that cannot must say so
  loudly and be mended, not accepted (`docs/protocol/design-rules.md`, rule 10).

## Hard rules

1. **Nothing generated from VaM goes into git.** Several shaders are VaM's own pixel programs lifted to HLSL and edited
   (skin, clothing, hair). The generators run on the user's machine and write to `out/gen/<component>/`, which is
   ignored. Never commit `out/`, DXBC, `.bin`, `.dll`, `.pkl`, listings of VaM's IL or of its compute kernels, or
   anything else read out of `VaM_Data`.
2. **No NVIDIA, Intel or AMD binaries or SDKs in git.** They come from `vamkit fetch` into `third_party/` (ignored).
3. **Vanilla stays vanilla.** `launch.py --variant vanilla` must start VaM with VaM's own `openvr_api.dll`, no
   `d3d11.dll` of ours, BepInEx switched off, and none of our environment variables. It is the user's way back.
4. **Ask before touching the user's VaM folder.** Commands that change it print a plan and need `--yes`. Keep VaM's own
   files in `<VaM>/vamkit/original/` before replacing anything; uninstall must leave the folder as it was.
5. **Keep the three places agreeing.** How it is built today: one switch for path tracing, in game, `[RayTracing] Renderer` in `BepInEx/config/local.vam.skintransmission.cfg`
   (`Legacy` = off, `PathTracing` = on), set by the menu. The compositor follows it every frame
   (`vam_performance_config.h` `VamPathRequested`): on means RR F at `RenderScaleRR`; off means `DLSSPreset`
   (`Off` / `K` / `L` / `M` / `XeSS` / `FSR`) at its own `RenderScale...`. Keep the menu, the launcher and the renderer
   agreeing. RR exists only on RTX cards: without one path tracing switches itself off and XeSS or FSR upscale.

## Where to read

| You want to | Read |
|---|---|
| Install on a new machine | `docs/agent/install.md`, `docs/agent/runtimes.md` |
| Tune for a GPU, CPU or headset | `docs/knowledge/frame-budget.md`, `docs/knowledge/settings-and-menu.md`, `docs/protocol/performance.md`, `docs/benchmarks/README.md` (measure before and after) |
| Shorten or debug scene / image loading, or touch the texture cache | `docs/knowledge/loading.md`, `docs/knowledge/gc.md` (collections during a load) |
| Change or extend a component | its `README.md`, `docs/agent/modify.md`, the knowledge document it names |
| Build something this project does not have | `docs/agent/modify.md`, "Building what is not here" |
| Find the cause of a problem | `docs/agent/troubleshoot.md` |
| Run on another VaM build | `docs/agent/adapt-vam-version.md` |
| Know what was left for you to decide on this machine | `docs/agent/open-items.md` |
| Decide how to test or measure | `docs/protocol/testing.md`, `docs/protocol/performance.md`, `tools/lab/README.md` |
| Touch the user's VaM folder | `docs/protocol/changes.md` |
| Design something new | `docs/protocol/design-rules.md`, `docs/knowledge/rejected-and-open.md` |

## Commands

```
python tools/vamkit doctor                      what is there, what is missing; changes nothing
python tools/vamkit fetch --accept-licenses     third-party SDKs and DLSS runtimes into third_party/ (pinned)
python tools/vamkit build [ids] [--steps ...]   generators, native libraries, plugins into out/<id>/
python tools/vamkit test [ids]                  the offline tests
python tools/vamkit fetch --lab / build --lab   the same two steps for the simulated headset (a test tool, out/lab/simxr)
python tools/vamkit install [--yes]             the build into the VaM folder; without --yes only the plan
python tools/vamkit verify                      the install's record against the VaM folder and the build
python tools/vamkit rollback [--yes]            takes the last install back
python tools/vamkit uninstall [--yes] [--purge] the VaM folder as it was before the first install
python tools/vamkit status                      which installed files are present in the VaM folder
python tools/check_manifest.py [--built]        the manifest against the tree (and, after a build, its outputs)
python tools/check_docs.py                      every path and link the documents name exists
python tools/check_lang.py [--new <code>]       the menus' language tables against the texts in the code
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
components/runtime/        cpu-boost, gc-tuning, hair-solver, hair-budget, cloth-physics, wrap-batch, scene-load, image-load
components/shared/         shader-lift (reads Unity shader data, lifts DXBC), lang (the menus' words per language),
                           harmony-compat (one source for both Harmony generations)
tools/vamkit/              the command line (Python standard library only)
tools/lab/                 test and measurement tools: simulated headset, test agent, sessions, benchmark, CPU census
docs/                      agent guides, knowledge per topic, working protocol, benchmarks (see the table above)
```

Installed layout inside the user's VaM folder:

```
<VaM>/vamkit-launcher.bat  what the user double-clicks: opens the launch panel
<VaM>/vamkit/launcher/     the launch panel (LaunchPanel.py) and launch.py
<VaM>/vamkit/lang/         the menus' language tables (zh.json ...)
<VaM>/vamkit/bin/          openvr_api.dll (OpenComposite), d3d11.dll (proxy), ofxr/, dlss/{sr,nr,fg}, xess/, fsr/
<VaM>/vamkit/original/     VaM's own files as the install found them (<name>.absent when there was none)
<VaM>/BepInEx/plugins/     our plugins. Render and shading plugins are dormant unless launch.py set their environment;
                           the runtime plugins work whenever BepInEx loads (vanilla switches BepInEx off)
```

## Where the settings are

The ones most often changed for a user. Every other setting is as open to you: `docs/knowledge/settings-and-menu.md`.

| What | Where |
|---|---|
| Path tracing on/off | `[RayTracing] Renderer` (skin cfg), or the in-game menu |
| Path tracing bounces, background savings | `[RayTracing] PathBounces` 0-3, `PathBackground` Off / Far / All |
| Upscaler model and scale | the run's `performance.ini` `[Performance]` `DLSSPreset` (`Off`, `K`, `L`, `M`, `XeSS`, `FSR`), `RenderScaleK/L/M/RR/XeSS/FSR`, `OutputScale` |
| Frame generation model | `[Performance] FrameGenModel` `DLSS` / `FSR` (FSR is 2x; DLSS needs RTX 40 for 2x, RTX 50 for more) |
| Denoiser | `[NR]` keys in the same file |
| Skin scattering, light through thin parts | skin cfg `[Subsurface] Strength` 0-1, `[General] Strength` 0-2 (menu: skin page) |
| Turn a shading part off | skin `[General] Enabled`, hair `Enabled`, clothing `Mode = 0`, tone mapping `Mode = 0` |
| Turn a runtime part off | `Enabled = false` in `local.vam.cpuboost.cfg`, `.hairsolver.cfg`, `.clothphysics.cfg`, `.wrapbatch.cfg`; GC: `FreeSpaceDivisor = 3`, `MarkThreads = 0`, `AllocatedBytesPerFinalizer = 0` in `.gctuning.cfg`; hair budget: `Profile=Off` in `VaMHairBudget.ini`; loading: the keys of `local.vam.sceneload.cfg` and `local.vam.imageloadpipeline.cfg` (each stage has its own, `docs/knowledge/loading.md`) |
| Worker threads | `[General] Threads` (cpuboost) and `[Mono] MarkThreads` (gctuning) are upper bounds; both are capped by the machine's processor count |

The launch panel keeps one preset per display mode (`vamkit/launcher/profiles/VR`, `.../Desktop`) and writes it into
the plugin configs at each launch, so change settings through the menu or the preset, not only in the live cfg.

## Working rules

- Explain the cause from the code before running the game; use the game to confirm.
- After a change, say what you verified and what you did not (for example: built, not run in VR).
- Working with less is not passing. When a run-time file is missing, a plugin stepped aside or a stand-in ran, the
  tools say LESS with the reason and the remedy: tell the user what they are not getting and mend it, do not report
  the install as done (`docs/protocol/design-rules.md`, rule 10).
- The first path-traced frame after a shader change waits for the driver to compile the pipelines (about 26 s once).
- Keep upstream licenses and notices in `components/render/opencomposite` and `components/render/ofxr-dlssg`.
- The runtime plugins must give VaM's own results. Each has a verify mode (see `guard` in `manifest/components.toml`);
  run it after any change to them and on a VaM build you have not seen before.
- Native code that needs worker threads takes them from cpu-boost (`CB_Task` / `CB_Run`, found with
  `GetModuleHandleW` on `VaMCpuBoostNative.bin`) and runs on the calling thread when it is absent. No second pool.
