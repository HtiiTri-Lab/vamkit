# launcher

The way into the game. A Tk panel (`LaunchPanel.py`) chooses the mod or vanilla VaM, in VR or on the desktop, keeps one
preset per display mode and starts `launch.py`. For the mod, `launch.py` puts OpenComposite in as `openvr_api.dll`, the D3D11
proxy beside `VaM.exe`, arms the OFXR tray (VR only), sets the environment variables the render and shading plugins need, runs
VaM and then puts VaM's own files back. Vanilla loads nothing of ours: VaM's own files, BepInEx switched off for the process, none of our
variables. Python standard library only (tkinter for the panel), Windows only. It is the only thing that makes the dormant
plugins live, and the player's way back to an untouched game.

## Files

| File | Holds |
|---|---|
| `launch.py` | `--variant mod|vanilla`, `--display VR|Desktop`, `--run <folder>`, `--test`, `--restore`; swaps files, builds the environment, starts VaM, restores on exit, keeps logs |
| `LaunchPanel.py` | The panel: settings validation (`validate`), `performance_ini()`, `flow_ini()`, `begin_launch()`, taking over what the in-game menu wrote into the run's `performance.ini`; `--preset 2|3|4` |
| `runtimes.py` | Where the NVIDIA libraries of a launch come from: the kit's own, else the graphics driver's model store (`locate`, `prepare`: signature checked, copied to `<VaM>/vamkit/cache/ngx/`), and what is found nowhere, with what is lost and how to get it (`describe`). `launch.py` calls `prepare`; the panel's `launch_notes` calls `locate` |
| `ModeProfiles.py` | Per-display-mode presets (`profiles/VR`, `profiles/Desktop`): capture, apply, byte-exact cleanup of test runs (`begin` / `finish`) |
| `lang.py` | The panel's words per language: `tr("English text")`, the tables of `components/shared/lang`, the Language row's choice (`profiles/language.json`), which the in-game menus read too |
| `vamkit-launcher.bat` | Installed beside `VaM.exe`: opens the panel with `pyw` / `pythonw` |
| `profile-manifest.json` | Which keys of which files a preset carries: `performance` keys, OFXR `layer` keys, `config` keys of the skin and tone-mapping cfgs, `whole_files` (the gloss tier file and the hair budget ini) |

## Build

None: `python tools/vamkit install` copies the files to `<VaM>/vamkit/launcher/` and `vamkit-launcher.bat` to the VaM folder (the component's `installs`). Layout `launch.py` expects, relative to itself: `<VaM>/vamkit/launcher/` (here), `<VaM>/vamkit/bin/`
(`openvr_api.dll`, `d3d11.dll`, `ofxr/OFXRBridgeTray.exe`, `dlss/{sr,nr,fg}`, `xess/`, `fsr/`), `<VaM>/vamkit/original/` (VaM's own `openvr_api.dll`,
`d3d11.dll`, `opencomposite.ini`, or `<name>.absent` when there was none), `<VaM>/vamkit/logs/`. Do not commit `__pycache__`.

## Settings

The panel writes a run folder `profiles/<mode>/panel-runs/<stamp>/` with `performance.ini`, `tray.ini`, `selection.json` (and
`desktop-presenter.ini` for the desktop) and the preset `profiles/<mode>/LaunchPanel.json`. The game reads the run's
`performance.ini` through `VAM_PERFORMANCE_CONFIG`. Keys a person would change (also in the in-game menu):

| File / section / key | Default | Meaning |
|---|---|---|
| `performance.ini` `[Performance] DLSSPreset` | `K` | `Off`, `K`, `L`, `M` (DLSS), `XeSS`, `FSR`: the upscaler when path tracing is off (an old `RR` preset is read as `K`). The launcher never replaces the model: the compositor tries it and lets another stand in only where it cannot run. `launch_notes` says in the panel what will not run as chosen, why, and how to get it |
| `[Performance] RenderScaleK` / `L` / `M` / `XeSS` / `FSR` | 75 / 33 / 50 / 67 / 67 | Render scale of each model, 33 to 100 |
| `[Performance] RenderScaleRR` | 33 | Render scale of Ray Reconstruction (path tracing on), 33 to 100 |
| `[Performance] OutputScale` | 100 | Output scale, 75 to 150 |
| `[Performance] FrameGeneration`, `FrameGenModel`, `DLSSGLite` | 1, `DLSS`, 0 | Frame generation on / off, the generator (`DLSS` or `FSR`; the other stands in where the chosen one cannot run; FSR is 2x) and DLSS-G's light model |
| `[NR] Enabled`, `Style`, `Intensity`, ... | `Enabled` 1, `Style` 0, `Intensity` 100 | Neural rendering (NR) settings |
| `selection.json` `frame_multiplier` | 2 | 2, 3 or 4; written to the OFXR layer; `OFXR_FRAME_MULTIPLIER=4` is set for 4 |

Path tracing itself is not a launcher setting: it is `[RayTracing] Renderer` in the skin cfg, switched in game; the launcher carries
that key and a few others in the preset (`profile-manifest.json` `config`) and writes them into the plugin cfgs at each launch.
Change settings through the menu or the preset, not only in the live cfg.

## Interfaces

- Sets for the mod: `VAM_DISPLAY_MODE` (`VR` or `Desktop`), `VAM_PERFORMANCE_CONFIG`, `VAM_DESKTOP_CONFIG` (desktop),
  `VAM_NR_PREPASS_PATH` / `_LOG`, `VAM_DLSS_RUNTIME_PATH` / `VAM_DLSS_LOG` / `VAM_DLSS_BRIDGE=1`, `D3DINTEROP_MODE=on12`, `OFXR_DLSSG=1`,
  `OFXR_DLSSG_RUNTIME_PATH` / `_LOG`, `OFXR_FRAME_MULTIPLIER`, `VAM_XESS_RUNTIME_PATH` and `VAM_FSR_RUNTIME_PATH` (`vamkit/bin/xess`, `vamkit/bin/fsr`:
  the optional run-time libraries of XeSS and of AMD's FSR API). `OUR_VARIABLES` in `launch.py` is the list removed from the environment first (and
  for vanilla); extend it with every new variable. `VAMKIT_NO_DLSS=1` in the launcher's own environment is passed on: a test of the path a card
  without RTX takes (no DLSS SR, RR or DLSS-G). The other switches of that kind pass the same way: `VAMKIT_NO_NVOF=1` (no NVIDIA optical
  flow in the OpenXR layer), `VAMKIT_AS_VENDOR=1002` (stand-ins ordered as on an AMD card), `VAMKIT_FSR_FRAMES=2` (the probe of FSR's
  second generated frame). None of them changes what the launcher writes: it never replaces a chosen model.
- VaM starts with `-vrmode OpenVR` (VR) or `-vrmode None` (desktop); vanilla also passes the Doorstop disable flags and `DOORSTOP_DISABLE=TRUE`.
- Writes `VaM_Data/Plugins/openvr_api.dll`, `VaM_Data/Plugins/opencomposite.ini` (original text plus our lines) and `<VaM>/d3d11.dll`; the OFXR
  tray registers its OpenXR layer under `HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` while it runs.
- Copies `LogOutput.log`, `openovr_log`, `d3d11_proxy.log`, OpenComposite's log and VaM's `output_log.txt` into `vamkit/logs/<stamp>-<mode>/`.

## Tests

The file swap is covered offline by `tools/vamkit/tests/install_test.py` (the mod's files into VaM's place and back, a VaM that changed since
the install, a run that was killed). `launch.py --restore` puts VaM's own files back after a crash. `--test` is the mode tests start VaM in:
the saved presets stay as they were (`ModeProfiles.begin/finish` restore touched files byte for byte), VaM starts minimized and is kept from
taking the focus (`keep_in_background`), on the desktop it is a window of the monitor's size instead of full screen, and Unity's window
settings in the registry (shared by every copy of VaM on the PC) are put back afterwards (`screen-before-test.json` while the run lasts).
A whole run is `tools/lab/session/first_run.py`.

## Invariants

- Vanilla stays vanilla: no file of ours in VaM's place, no variable of ours, BepInEx off. Check this after any change.
- VaM's own file is never lost: before the mod's file takes its place, `remember()` makes what is there now the record in `vamkit/original/` (the file, or `<name>.absent`), unless it is the mod's own file left by a killed run. So the record follows a VaM update or an injector added or removed since the install.
- `put_back()` acts only where the mod's file is in VaM's place (`ours()`: same bytes as `vamkit/bin/<name>`, or the `; vamkit` line in `opencomposite.ini`). A file that is not the mod's, and an empty place, are left alone.
- `restore_files()` runs in `finally` on every exit path, and again by `--restore`.
- Refuses to start while `VaM.exe` runs, in VR as administrator (OpenXR ignores the per-user OFXR layer for elevated games), or while OFXR's tray already runs.
- Keep the panel, the in-game menu (`vr-menu`) and the renderer agreeing on setting names and ranges (`validate` ranges, `profile-manifest.json`).

## Read next

[../../../docs/knowledge/settings-and-menu.md](../../../docs/knowledge/settings-and-menu.md),
[../../../docs/knowledge/frame-path.md](../../../docs/knowledge/frame-path.md)
