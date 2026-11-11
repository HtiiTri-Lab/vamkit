# Settings and the in-game menu

Everything an agent may tune lives in one of four places, and each setting lives in exactly one of them: the run's
`performance.ini` (render, upscaler, denoiser, frame generation), the plugins' BepInEx cfg files (shading, tone mapping,
runtime parts), the per-display-mode presets that carry both from one launch to the next, and the in-game menu that edits
the first two while the game runs. A player sees a "VR 画面" tab (desktop: "画面") inside VaM's main menu with pages and
big buttons; an agent tuning for another machine edits the same files or changes the preset so it survives the launch.

## How it works

### The run folder and `performance.ini`

`launcher/LaunchPanel.py` (`begin_launch`) creates `launcher/profiles/<VR|Desktop>/panel-runs/<timestamp>/` with
- `performance.ini`: written by `performance_ini(settings)`. `[Performance]` then `[NR]`.
- `tray.ini`: OFXR tray settings (`flow_ini`).
- `selection.json`: the settings of this launch (`frame_multiplier` included).
- `desktop-presenter.ini` (desktop only; `apply_layer` fills it from the preset's `layer` section; holds `[ofxr] frame_multiplier`).
- `launcher.log`.

`launch.py` then exports `VAM_PERFORMANCE_CONFIG=<run>/performance.ini` (and `VAM_DESKTOP_CONFIG` for desktop), plus
`VAM_DISPLAY_MODE`, `VAM_NR_PREPASS_PATH`, `VAM_DLSS_RUNTIME_PATH`, `OFXR_DLSSG_RUNTIME_PATH`, `D3DINTEROP_MODE=on12` and
the log folders; `OUR_VARIABLES` lists everything it removes first so a vanilla run is clean.

Readers: `ParseVamPerformanceSettings()` in `components/render/opencomposite/OpenOVR/Compositor/vam_performance_config.h`
(all `[Performance]` keys, re-read when the file's stamp changes, polled every 250 ms), `ReadSettings()` in
`dx11_nr_prepass.cpp` (`[NR]`), the bridge (`dlss-bridge/VaMDLSSBridge.cs`, through the compositor), the frame generation
layer (`[ofxr]` in its `ofxr_bridge.ini` / `desktop-presenter.ini`), and the menu. The file is the live control surface:
edit it while the game runs and the compositor follows within about 250 ms. Writers replace it whole through a temp file
(the menu uses `MoveFileEx`), so a reader never sees half a file.

`[Performance]` keys written by the launcher, with who owns them:

| Key | Owner | Note |
|---|---|---|
| `Enabled=1`, `Upscaler=DLSS` | fixed | `Upscaler` is forced by `FIXED` |
| `RenderScale`, `DLSSPreset`, `RenderScaleK/L/M/RR`, `OutputScale` | menu | semantics in [upscaling.md](upscaling.md) |
| `DLSSSharpness`, `DLSSSharpenLast`, `RRStereo` | menu | |
| `NRAfterUpscale`, `NRAfterScale`, `NRFeatherPercent` | menu | [denoiser.md](denoiser.md) |
| `FrameGeneration`, `DLSSGLite`, `DLSSMenuNative` | menu | [frame-generation.md](frame-generation.md) |
| `DLSSObjectMotion`, `DLSSHairMotion`, `DLSSHairBias`, `DLSSJitterFlipX/Y`, `NRUseDepth`, `NRUseMotion`, `DLSSFGEyes`, `DLSSSREyes`, `FFREnabled`, `FFRFullRateRadius`, `FFRHalfRateRadius`, `FSRSharpness` | fixed (`FIXED`) | Motion sources and diagnostics; do not tune ([motion-vectors.md](motion-vectors.md)) |
| `DLSSMipBias`, `DLSSRigidMotion`, `RRTransparentGuides`, `PTReference`, `NISSharpness` | code defaults only | The launcher does not write them; add the line to the run file to change them |

`[NR]` keys: `Enabled`, `Style`, `Intensity`, `LocalToneStrength`, `LocalStructureStrength`, `SkinStructureStrength`,
`FoveationSize`, `RenderPreset`, `RegionShiftY` (and `PackEyes`, code only). Ranges are in [denoiser.md](denoiser.md).
`[ofxr] frame_multiplier` (2-4) is not in `performance.ini`: it is in the OFXR layer's `ofxr_bridge.ini`
(`%LOCALAPPDATA%\OFXR Bridge\RuntimeLayer\v7443\`) in VR and in `desktop-presenter.ini` in the run folder on the desktop;
`[overlay] position` (frame-rate display corner) is in the same file.

A limit is enforced in up to four places, which must change together: the code reader's `read(...)` clamp, `LIMITS` in
`LaunchPanel.py`, the menu's `Number(...)` range, and, for NR, `ReadSettings()`.

### Plugin cfg files

Plain BepInEx config files in `<VaM>/BepInEx/config/`; most plugins follow edits live, some only at start (the
description in each `Config.Bind` says so).

| File | Settings an agent may change | Default in code |
|---|---|---|
| `local.vam.skintransmission.cfg` | `[RayTracing] Renderer` `Legacy` / `PathTracing` (the path-tracing switch, live) | `Legacy` |
| | `[RayTracing] PathBounces` 0-3 (diffuse bounces, path tracing only) | 0 |
| | `[RayTracing] PathBackground` `Off` / `Far` / `All` (background savings, path tracing only) | `Far` |
| | `[RayTracing] RRDetail` 0-100 | 75 |
| | `[Subsurface] Strength` 0-1 (scattering under skin, both routes) | 1.0 |
| | `[General] Strength` 0-4, menu offers 0-2 (light through thin skin, both routes) | 1.0 |
| | `[Specular] Normalize` true/false; per-character gloss levels in `local.vam.skintransmission.tiers.txt` | true |
| | `[General] Enabled` (plugin off = VaM unchanged, read at start) | true |
| `local.vam.tonemapping.cfg` | `[General] Mode` 0 off / 1 while display lighting is on / 2 always; `Style` 0 neutral / 1 film | 1 / 0 |
| `local.vam.cpuboost.cfg` | `[General] Enabled`, `Threads` (upper bound, capped by the processor count) | true / 8 |
| `local.vam.gctuning.cfg` | `[Mono] FreeSpaceDivisor` (3 = stock), `MarkThreads` (0 = stock marker), `AllocatedBytesPerFinalizer` (0 = stock rule) | 2 / 16 / 10000 |
| `local.vam.hairsolver.cfg`, `local.vam.clothphysics.cfg` | `[General] Enabled` (off by default in code; the install writes true) | false |
| `local.vam.wrapbatch.cfg` | `[General] Enabled` | true |
| `VaMHairBudget.ini` | `Profile` Off / High / Balanced / Performance / Custom, `DisableShadows`, `ToggleKey`, `MenuTab` | `Balanced` |
| `local.vam.clothshading.cfg`, `local.vam.haironepass.cfg` | `Mode` / `Enabled`, see [clothing.md](clothing.md), [hair.md](hair.md) | |

Plugins that draw for the compositor are dormant unless `launch.py` set the environment, so editing their cfg while
running vanilla changes nothing; the runtime plugins work whenever BepInEx loads (vanilla turns BepInEx off).
The menu changes a plugin's setting in memory through the plugin's own `ConfigEntry`; the plugin follows at once and BepInEx
saves the cfg.

### Presets per display mode

A launch is not a plain "start with this cfg": the launcher keeps one preset per display mode
(`launcher/profiles/VR`, `launcher/profiles/Desktop`; `profiles/display-mode.json` remembers the last choice) and writes it
into the plugin cfg files at every launch, then reads the result back when VaM exits. Code: `launcher/ModeProfiles.py`.
- `profile-manifest.json` (version 2) lists what a preset carries: `performance` ([section, key] pairs of the run's file),
  `layer` (`[ofxr] frame_multiplier`, `[overlay] position`), `config` (cfg files and their keys; for the skin cfg
  `[General] Strength`, `[RayTracing] PathBackground`, `PathBounces`, `RRDetail`, `Renderer`, `[Specular] Normalize`,
  `[Subsurface] Strength`; for tone mapping `Mode`, `Style`) and `whole_files` (`local.vam.skintransmission.tiers.txt`,
  `VaMHairBudget.ini`).
- `begin()`: records the before-state of every listed file in a journal (`preset-transaction.json` in the run's log folder),
  then `patch_ini`s each listed key from the preset's `plugin-settings.json` (it changes only the named keys and keeps
  comments, unknown keys, the byte order mark and line endings), restores whole files, patches the run's `performance.ini`
  only for keys it lacks (`missing_only`, so future menu keys survive without a hand list), and writes the OFXR layer's
  frame multiplier. The journal comes first, so a crash during application can be undone.
- `finish()`: with `--test` or when the game never launched, the before-state is put back. Otherwise the cfg files, as VaM
  left them (in-game changes included), become the new `plugin-settings.json`, and the run's `performance.ini` and the
  final multiplier update `LaunchPanel.json` through `read_live`.
- The first launch creates the VR preset from the plugin settings as they are; the first desktop launch is seeded from VR
  (`seeded_from`).
- Precedence when the panel loads a mode (`load_settings`): `LaunchPanel.json`, overridden by the performance values in
  `plugin-settings.json`, overridden by the newest run's `performance.ini`. Editing `LaunchPanel.json` alone therefore does
  not stick if the same key exists in the snapshot or the last run. To change a persistent default, edit the preset
  (`plugin-settings.json` for cfg and performance keys) with VaM closed, or change the setting in the menu and exit.
- Changing a cfg file directly while the launcher manages that key is overwritten at the next launch by the preset.
- A new setting that should persist per mode goes into `profile-manifest.json` (and `GAME_INI` / `LIMITS` /
  `performance_ini()` in `LaunchPanel.py` if it is a `performance.ini` key).

### The in-game menu

`components/render/vr-menu/` (`VaMVRMenu.cs` items and file access, `VaMVRMenu.Menu.cs` layout, `VaMVRMenu.FrameGen.cs`
frame-generation pause, `VRMenuIni.cs` INI editing). It is dormant (no tab, no file access) unless `VAM_PERFORMANCE_CONFIG`
names a run folder with a `selection.json` beside the `performance.ini`, so VaM started any other way is left alone. It
edits only values (`VRMenuIni.Set`: all other lines, comments and line endings stay), writes at most every 0.2 s, checks the
file's time every 0.5 s and takes external edits within a second, and never creates a missing file (a missing file means
its owner is gone). Each item is stored in one of three places: `performance.ini` (default), the OFXR layer's ini
(`item.layer`), or another plugin's `ConfigEntry` (`item.entry`).

Pages and rows (labels are Chinese in the game; shown here as English with the original in parentheses). Rows hide when
their plugin is missing or off, when a parent switch is off, or by route (`pathRule`: path-tracing-only or raster-only):

| Page | Rows (key) |
|---|---|
| Frame generation (插帧) | on/off (`[Performance] FrameGeneration`, implemented by pausing the OFXR layer through its tray pause event, also paused while VaM menus are open with `DLSSMenuNative`); multiplier 2x-4x (`[ofxr] frame_multiplier`, choices from the layer's `OFXR_GetFrameMultiplierV1`); mode quality / performance Preset C (`DLSSGLite`, shown only if the layer reports it follows live); frame-rate display corner (`[overlay] position`); "when VaM menu is open" normal / pause SR and generation (`DLSSMenuNative`) |
| Clarity (清晰度) | raster route: model Off/K/L/M (`DLSSPreset`) and render scale (`RenderScale`, hidden when Off); path tracing: a fixed status row "reconstruction model RR F", render scale (`RenderScaleRR`), detail preservation (`RRDetail`, step 5), cross-eye consistency (`RRStereo`, VR only, step 10); both: sharpen (`DLSSSharpness`), position after SR / last (`DLSSSharpenLast`); VR: output resolution (`OutputScale`), desktop: a status row with the window size |
| Noise reduction (降噪) | sub-page "basic": NR on/off, model new/old (`RenderPreset` 2/1), position before/after upscale (raster route only), reduced-size percent (`NRAfterScale`), style, region (`FoveationSize`), region shift (`RegionShiftY`), feather (`NRFeatherPercent`); sub-page "strength": overall, local tone, local structure, skin structure (all under NR on) |
| Lighting (光影) | path tracing off/on (`[RayTracing] Renderer`) plus a live status line; bounces 0-3 (`PathBounces`) and background savings no/far/all (`PathBackground`), path tracing only; tone mapping off / with display lighting / always (`Mode`) with style neutral / film |
| Skin (皮肤) | subsurface scattering 0-1 (`[Subsurface] Strength`, step .1); thin-part light 0-2 (`[General] Strength`, step .1); highlight normalisation (`[Specular] Normalize`); one gloss row per character, 0-4 step .25, kept per skin by the skin plugin |
| Body (身体) | rows for the animation clock and soft body plugins, which are not in this repository: the page does not appear |

The tab itself is added to VaM's main menu (tab name "VR Settings"; the title is "VR 画面", or "画面" in desktop mode). The
hair budget plugin has its own page elsewhere in the main menu (`MenuTab` in its ini).

### Rules the menu follows

- **Controls show real values.** A multiplier picker says 2x / 3x / 4x, never an on/off that means something indirect. A rule
  that needs "but when ..." in one sentence is redesigned, not documented. If an option is unreachable with the current
  backend, change the backend.
- **One place per setting.** A hot-changeable setting lives in the menu; a start-up choice lives in the launcher; nothing
  appears in both. The launcher may carry the menu's value to the next launch without showing it.
- **No explanatory text in VR.** Labels and values must say what a row does. Reading is costly in a headset.
- **Pages and sub-pages, never scrolling.** Rows beyond the room auto-split into "name 1 / 2" sub-pages by whole card. A page
  with no visible row has no button.
- **Each level looks different**: pages are a dark sidebar (220x96 buttons, the selected one bright with a left edge),
  sub-pages are text tabs with an underline, values are segmented buttons (selected: blue, white bold), numbers are
  "- slider + value" (the steps move in 5, or 10 beyond a 100 range, or the item's `step`, the slider for coarse drags).
  Relationship has a shape: a switch and the rows that belong to it share a card; the switch row is bold, children are
  indented 30 with a left guide line. The same look never means both "go to this page" and "choose this value".
- **Big targets.** A row is 88 units high; controls are custom (Unity Button/Slider/Text with VaM's rounded sprite), the
  panel is 1100 x 1380 units (VaM's own tab buttons are 80 high). Text colours are light (VaM's text prefab is black on dark).
- **A test that proves nothing is deleted** everywhere (engine code, API, panel switch, A/B tool), not hidden or defaulted
  off, so the next design has one path.
- Removed on purpose: fixed-foveated rendering and detailed upscaler options, diagnostics, any row for sharpening that
  no longer exists (RCAS), a one-step 2x/3x rule.

### Adding a setting to the menu

1. Decide its home: `performance.ini` (needs a reader in `vam_performance_config.h` or the consuming code), another plugin's
   cfg entry (`PluginSetting(guid, section, key)` returns null when the plugin is not loaded or its `[General] Enabled` is
   off, which hides the row), or the OFXR layer's ini (`item.layer = true`).
2. In `BuildItems()` (`VaMVRMenu.cs`) put the row in its page and section: `AddPage("title")` opens a page,
   `AddSection("name")` a sub-page, then `Add(OnOff(...))`, `Add(Choice(...))`, `Add(Number(...))`, or `Live(id, group, text)`
   for a read-only status line; `Hidden(...)` keeps a value in the file with no control. Attributes: `.parent` (shown only
   while that switch is on), `.group` (same card, no hide), `.pathRule` (1 path tracing only, -1 raster only), `.unlessId` /
   `.unlessValue`, `.entry`, `.layer`, `.step`, `.unit`, `.fractional` (continuous plugin value, two decimals).
3. For a persistent setting add it to `profile-manifest.json`; for a `performance.ini` key also `GAME_INI`, `LIMITS` and
   `performance_ini()` in `launcher/LaunchPanel.py`.
4. Keep ranges equal everywhere (above). A float setting presented as choices needs the plugin's formatted text (`"R"`) to
   equal one of the option values exactly, otherwise the row shows its fallback.
5. Run the layout audit: `LayoutAudit()` in `VaMVRMenu.Menu.cs` shows every page and sub-page and reports cards past the
   bottom, rows taller than a row, labels taller than their row, and buttons or values wider than their boxes. In the
   simulated headset it is called through the test agent's `invoke` command with the tab in view
   (`tools/lab/test-agent/VaMTestAgent.cs`). The development tree's page-sweep script that also clicks pages and checks the
   files is not in this repository.

## Measured and verified

- The menu's INI editor had 101-102 offline checks in the development tree (the test project is not in this repository).
  Layout audits passed for both routes after each page change, in a simulated headset.
- Menu writes were checked live in a simulated headset: a changed value reaches the file within 0.2 s and the compositor
  log reports the new state; a value changed outside the menu appears in the menu within a second.
- Not checked by a person in a real headset: the subsurface scattering and thin-part light rows.

## Pitfalls

- **No tab appears.** `VAM_PERFORMANCE_CONFIG` has no `selection.json` beside it, or `BuildItems()` threw (the loader
  retries forever and the tab never appears). Wrap every cross-plugin lookup in try/catch and add the row only when it is
  found. Search the BepInEx log for "VaM VR menu": "added the 'VR 画面' tab with N settings", "page ...: N rows, ...
  of ...", "wrote ...", "changed outside the menu", "session runs Nx".
- **Rows stretch to fill the page.** The row's `HorizontalLayoutGroup` reports its child's `flexibleHeight` upward; the row's
  `LayoutElement` must set `flexibleHeight = 0`. Never size the UI through `UIDynamic.height` (its getter and setter read
  different fields and a timer grows the text slowly); let the layout size it.
- **K / L / M labels turned into words.** An auto-translation plugin translated the single letters. Every label starts with
  U+180E (`NoTranslate`), an ignore prefix that plugin honours; a test that clicks by text must strip it first.
- **Changed NR range, nothing happens beyond the old limit.** The code reader's limit was not changed (see above).
- **Changes in the menu are lost at next launch.** Not lost: `finish()` captures them into the preset. They are lost if you
  launch with `--test` or edit the cfg while the launcher manages that key.
- **Edited `LaunchPanel.json` has no effect.** Precedence (above).
- **Launching as administrator in VR** is refused: OpenXR ignores a per-user layer in an elevated game.
- **A test script that prints a minus sign crashes on a legacy console.** Set stdout to UTF-8.
- **Opening a menu to change a setting costs a render-size swap** with `DLSSMenuNative` on (see the open hang in
  [upscaling.md](upscaling.md)); the menu is only reachable through VaM's own menu, so frequent tuning in a headset has
  that cost.

## Tried and dropped

- Settings duplicated in the launcher and the menu; a launcher page of upscaler details; a two-step 2x/3x switch (replaced by
  a multiplier picker). A setting that failed its A/B (running NR and SR in parallel, asynchronous post-processing) was
  deleted from engine, API, panel and tools instead of being left switched off.

## Open

- The labels are Chinese and there is no localisation layer; English needs a label table in `BuildItems()`.
- Real-headset review of the newer skin rows was not done.

## Verify

Offline: the INI editor's tests and the preset code (`ModeProfiles.py`) can be run without VaM (`begin` / `finish` with a
temporary folder and `--test`). In the game: run the layout audit, then change each row once and watch the file and the
BepInEx log; test a `--test` launch to confirm the preset is restored ([../protocol/testing.md](../protocol/testing.md),
[../protocol/changes.md](../protocol/changes.md)).
