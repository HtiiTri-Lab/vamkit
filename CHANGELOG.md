# Changelog

## Unreleased (v0.1, in progress)

- Visual components exported from the development tree: compositor, frame generation, DLSS bridge, menu, launcher,
  skin and path tracer, hair, clothing, tone mapping, display lighting.
- Path tracing is a switch in the game's menu instead of a choice at launch. On: DLSS RR F at its own render scale.
  Off: VaM's own lighting with our shading, DLSS SR K / L / M at theirs. The compositor follows the switch every frame.
- The launcher offers the mod or vanilla (VaM's own files, BepInEx off), in VR or on the desktop; `launch.py` replaces
  the PowerShell launch script.
- The NRD denoiser path is removed (path tracing always goes through RR).
- Skin: screen-space subsurface scattering for the path tracer and light through thin parts on both routes
  (`[Subsurface] Strength`, `[General] Strength`, both in the menu); the old overall translucency layer is gone.
- Runtime components added: cpu-boost, gc-tuning, hair-solver, hair-budget, cloth-physics, wrap-batch. Worker thread
  counts are capped by the machine. Scene and image loading work is not included.
- The texture-space ray tracing (the atlas), EnvShadow and the launch-time lighting route are removed. Path tracing
  off now means VaM's own lighting and shadow maps with our skin shading and nothing traced. rt-core's probe outputs
  are identical to the development tree; the skin's spec, scattering and mouth tests pass; the skin host builds but
  has not been run in the game.
- Tables generated from the user's VaM (shader states, shadow casters, program sets, the shader type tree) are built
  into `out/gen/` and no longer checked in.

- The repository builds itself: `vamkit fetch` (pinned third-party SDKs and DLSS runtimes; vendor licences accepted by
  the user; optional runtimes taken from the user), `vamkit build` (generators, native libraries, plugins of all 20
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

Not done yet: VR and real scenes from this repository's build, the simulated-headset test tools running from the
repository, a real download test of `vamkit fetch`.
