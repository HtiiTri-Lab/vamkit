# Changelog

## Unreleased (v0.1, in progress)

- Visual components exported from the development tree: compositor, frame generation, DLSS bridge, menu, launcher,
  skin and path tracer, hair, clothing, tone mapping, display lighting.
- Path tracing is a switch in the game's menu instead of a choice at launch. On: DLSS RR F at its own render scale.
  Off: VaM's own lighting with our shading, DLSS SR K / L / M at theirs. The compositor follows the switch every frame.
- The launcher offers the mod or vanilla (VaM's own files, BepInEx off), in VR or on the desktop; `launch.py` replaces
  the PowerShell launch script.
- The NRD denoiser path is removed (path tracing always goes through RR).
- Desktop presenter v3: Unity's queue no longer waits on the GPU for the presenter's queue (its frames go through a ring
  of three inputs), Unity's thread waits for the presenter for at most a second, and long fence waits, latency-object
  timeouts and slow `Present` calls are logged in `dlssg.log`. In answer to a desktop hang on another machine (GPU
  nearly idle, Unity's thread in 11on12, the presenter in a fence wait), where a stall in presentation could stop all of
  VaM through that wait. `D3DINTEROP_ON12_WORKER=0` runs 11on12 without its own worker thread, as a diagnostic.
  Run on the desktop in a trial copy (DLSS-G 4x on 160 Hz, frame generation off and on while playing, two window
  resizes, also with `D3DINTEROP_ON12_WORKER=0`): 40 real fps with generation as before, no long wait logged. The hang
  was never seen on the development machine, so only the reporting machine can show whether this ends it.
- Windows 10, and Windows 11 builds without Microsoft's fix: the picture no longer freezes at the first upscaled or
  path-traced frame. With v3 the reporting machine kept answering, and its logs showed Unity's own queue stopped
  while the presenter sat idle. The compositor (DLSS SR, XeSS, FSR, RR) and the skin path tracer named 11on12's own
  queue to `UnwrapUnderlyingResource`. The `d3d11on12.dll` of those systems then puts a `Wait` ahead of its own `Signal`
  on that queue, and the queue never runs again (microsoft/D3D11On12 issue #51, fixed there on 2025-09-25; that machine
  runs Windows 10 22H2). At start-up the proxy now runs `vamkit/bin/unwrap_probe.exe` in a process of its own on the
  `d3d11on12.dll` VaM loaded. Unwraps name 11on12's queue only once the probe has come back; until then, and on a DLL
  where the probe stops, they name a queue of their own that runs no work. That queue works everywhere but cost about 5 % in
  VR without path tracing, so it is kept for the old DLLs. `d3d11_proxy.log` names the `d3d11on12.dll`, the Windows
  build and the probe's verdict with the DLL's version and the queue unwraps name; `D3DINTEROP_UNWRAP_QUEUE=own` /
  `separate` decides without the probe. Reproduced here
  with the `d3d11on12.dll` Windows 11 24H2 first shipped with (10.0.26100.1, kept in `WinSxS`), copied beside `VaM.exe`
  in a trial copy. The previous build stopped at the first DLSS frame and logged the same lines as the reporting
  machine (Unity's queue at 1225 of 1226, the presenter's at 2450 of 2450). This build runs there: desktop, DLSS SR,
  DLSS-G 4x, 40 real fps on 160 Hz, path tracing on for 40 s and off again.
- `vamkit versions`: what the machine has of everything the kit depends on (Windows and its graphics DLLs, hardware GPU
  scheduling, the card, driver and NGX core, the OpenXR runtime, VaM's Unity, Mono and BepInEx, the run-time files, the
  build tools), each line marked where it differs from the machine the kit was made on, with what that line has meant
  so far and where to read. The reference is `docs/agent/reference-machine.json`; `docs/agent/reference-machine.md`
  says what to do with a difference and keeps the machines reported so far. `doctor` ends with the count. Versions
  and model names only: no paths, user or machine names.
- Skin: screen-space subsurface scattering for the path tracer and light through thin parts on both routes
  (`[Subsurface] Strength`, `[General] Strength`, both in the menu); the old overall translucency layer is gone.
- Runtime components added: cpu-boost, gc-tuning, hair-solver, hair-budget, cloth-physics, wrap-batch. Worker thread
  counts are capped by the machine.
- The texture-space ray tracing (the atlas), EnvShadow and the launch-time lighting route are removed. Path tracing
  off now means VaM's own lighting and shadow maps with our skin shading and nothing traced. rt-core's probe outputs
  are identical to the development tree; the skin's spec, scattering and mouth tests pass; the skin host builds but
  has not been run in the game.
- Tables generated from the user's VaM (shader states, shadow casters, program sets, the shader type tree) are built
  into `out/gen/` and no longer checked in.

- The repository builds itself: `vamkit fetch` (pinned third-party SDKs and DLSS runtimes; vendor licences accepted by
  the user; optional runtimes taken from the user), `vamkit build` (generators, native libraries, plugins of all
  components, from `manifest/components.toml`), `vamkit test` (the offline tests). Every component builds from a clean
  tree; the generated shader headers are byte-identical to the development tree's.
- `vamkit build` stops with the `fetch` command to run when a component's third-party items are missing; the path
  tracer's shaders compile side by side and are skipped when nothing they are made from changed;
  `tools/check_manifest.py` checks the manifest against the tree and a build's outputs.
- rt-core's material probe: the pore-to-roughness contract reads the specular normal through a material without
  diffuse colour and also checks that with diffuse colour ray reconstruction is given the diffuse normal. It had been
  failing since the guide normal started to follow the lobe that carries the energy (the static-speckle fix); the
  tracer itself is unchanged.
- Files are stored without line-ending conversion (`.gitattributes`).
- Documentation for agents: `docs/agent`, `docs/knowledge`, `docs/protocol`, a README in every component, `tools/lab`.

- `vamkit install`, `verify`, `rollback` and `uninstall` (`tools/vamkit/install.py`): the plan first, `--yes` to act;
  every installed file and written setting is recorded in `<VaM>/vamkit/installed.json` with what was there before;
  each install leaves a backup folder with a `restore.cmd`; uninstall leaves the folder as it was before the first
  install and keeps the user's logs, presets and later settings. Tested on made-up VaM folders
  (`tools/vamkit/tests/install_test.py`, part of `vamkit test`) and with the real build into one.
- Launcher: VaM's own `openvr_api.dll`, `d3d11.dll` and `opencomposite.ini` are put back only where the mod's file is
  in their place, and the record of them follows VaM (an update of VaM, an injector added or removed since the
  install); `launch.py` and the panel also run under `python -I`.

- First run in the game from this repository's build, on the desktop, in a content-free copy of VaM 1.22
  (`tools/lab/session/first_run.py`): vanilla loads nothing of the kit; the mod loads its 14 plugins without an error
  line, renders a person, creates DLSS super resolution and frame generation, takes path tracing and ray reconstruction
  when switched on while playing (about 17 s the first time, while the driver compiles the pipelines) and gives them up
  when switched off; VaM's own files and window settings are back afterwards.
- Lab: `tools/lab/session` (a background desktop session driven through the test agent, the first-run check, a
  content-free trial copy of a VaM, a minimal scene). `launch.py --test` starts VaM minimized, keeps it from holding the
  focus and puts Unity's window settings back. The test agent's `wait_frames` answer carries its request id.
- Uninstall names the settings files the plugins made and removes the two logs the mod writes beside `VaM.exe`.

- Load-time components added: scene-load 1.6.0 (plugin-compile cache, asset bundles read in memory, parallel Vorbis
  decode, atoms created in batches, scene-switch fixes) and image-load 0.1.0 (images decoded in parallel off the main
  thread, mips and BC compression in native code, cache writes in the background). Both build from the repository to
  the sizes of the development build; `docs/knowledge/loading.md` explains them.
- scene-load's cap on VaM's texture cache is off by default (`[TextureCache] MaxGB = 0`). A cap deletes cache files, so
  it is the user's to set; the development machine runs 40 GB.
- The simulated headset is in the repository (`tools/lab/simxr-runtime`, a build of VirtualDesktop-OpenXR with a
  stand-in headset; `vamkit fetch --lab`, `vamkit build --lab`). Sessions can run in VR on it: `launch.py --xr-runtime`
  points the one VaM process at it. `first_run.py` has a VR part (VR session up, a picture in the frames handed to the
  headset, path tracing, frame generation at one frame per refresh); it passes in the trial copy.
- Benchmark (`tools/lab/bench`, `docs/benchmarks`): four standard scenes made of VaM's built-in content with a motion
  plugin, measured on the desktop and on the simulated headset, path tracing off / on, frame generation off / on,
  against VaM alone; the method, the metrics and the results measured on the development machine are published with it.
- Test agent: `atoms` and `storable` (a scene's parameters as plugins see them). Sessions: `baseline` (VaM's own files
  with the agent alone) and `without=[...]` (single plugins parked for a run).

- Builds against the BepInEx the user has: `components/shared/harmony-compat/HarmonyCompat.cs` is compiled into every
  plugin that patches VaM, so that one source builds against Harmony 2.0 (BepInEx 5.0-5.3) and HarmonyX (BepInEx 5.4,
  which makes the five-argument `Patch` a compile error).
- Run on an official installation: VaM 1.22.0.13 from MeshedVR's updater with BepInEx 5.4.23.6 from its release page.
  Every component builds against it (the generators find their shader programs), the first-run check passes on the
  desktop and on the simulated headset, without a licence key.
- Benchmark records carry VaM's physics rate and the physics steps per frame; a session that fails to start is tried
  once more, after waiting for another VaM on the machine to end.

- Upscaling and frame generation that do not need an RTX card: Intel XeSS and AMD FSR as upscalers with path
  tracing off (`DLSSPreset = XeSS / FSR`), FSR frame generation (2x) in VR and on the desktop
  (`FrameGenModel = FSR`), all three probed at start and chosen by what the graphics card can run. Run on an RTX card,
  also with DLSS switched off for the test (`VAMKIT_NO_DLSS=1`); nothing was run on a Radeon or an Arc
  (`docs/knowledge/upscalers-other-vendors.md`).
- cpu-boost recognises VaM's methods by their code with metadata tokens resolved to names, so a VaM that was only
  compiled again is recognised: active on 1.22.0.3 and on the official 1.22.0.13.

- Nothing missing is passed over in silence. `vamkit fetch` takes a run-time file of another version when the vendor
  signed it (and looks in NVIDIA's own model store), and says which version is in use; `fetch`, `doctor`, the install
  plan and `verify` name every missing run-time file with what the user does not get and how to add it; the first-run
  check has a third verdict, LESS (a missing file, a plugin that stepped aside, a stand-in in place of what was asked
  for), with its own exit code; the benchmark records plugin warnings. `docs/protocol/design-rules.md`, rule 10.

- FSR frame generation in VR runs without NVIDIA's optical flow; the launcher takes an NVIDIA library the kit lacks
  from the graphics driver's own store; the compositor and the frame generation write what was asked for, what runs and
  why into the run's log folder (`dlss/running.json`, `dlssg/running.json`), and the first-run check and the benchmark
  read it.
- The launch panel and the in-game menus (the graphics tab, Hair Budget, Display Lighting) in English and Chinese:
  English is the text in the code, a language is one table (`components/shared/lang`, `tools/check_lang.py`), the
  panel has a Language row and the in-game menus follow it at the next start. `vamkit-launcher.bat` in the VaM folder
  opens the panel. The READMEs say what the kit gives and what it was measured on, with pictures.

- `vamkit bepinex`: BepInEx 5 is fetched from its release page and put into a VaM folder that has none (a clean VaM),
  recorded and removable; the READMEs open with the advice to try the kit on a separate, clean copy of VaM.

Not done yet: a real headset and scenes with real content from this repository's build, VaM's own `-benchmark`
scenes in the benchmark (written, not run), a card that is not NVIDIA's, a real download test of `vamkit fetch` for
the NVIDIA items.
