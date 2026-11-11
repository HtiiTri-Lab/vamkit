# vr-menu

The in-game settings menu: a "VR 画面" tab in VaM's main menu with what is worth changing from inside the headset (or at
the desktop) while the game runs: frame generation, super resolution and Ray Reconstruction, the denoiser, the path tracing
switch and its bounces, tone mapping, skin scattering and light through thin parts. It is laid out for a laser pointer:
pages, sub-pages and cards, nothing scrolls, every target is big. It edits the run's `performance.ini`, the OFXR layer's
ini, and other plugins' BepInEx settings in memory. Depends on BepInEx; talks to the compositor, OFXR / presenter and skin /
tone-mapping plugins only through files, settings and exports. Dormant unless `VAM_PERFORMANCE_CONFIG` names a launcher run.

## Files

| File | Holds |
|---|---|
| `VaMVRMenu.cs` | The BepInEx plugin (GUID in the `BepInPlugin` attribute, version 24.9.0): the row definitions (`AddPage`, `Add(Choice(...))`, `Add(Number(...))`), reading and writing values, the layer lookup (OFXR layer in VR, the desktop presenter in desktop mode), live status |
| `VaMVRMenu.Menu.cs` | Builds the tab from Unity `Button` / `Slider` / `Text` on VaM's button sprite: side bar of pages, sub-pages, cards, segmented choices, number rows |
| `VaMVRMenu.FrameGen.cs` | Frame generation on / off through the OFXR tray's pause event (`Local\OFXRBridgePause-<n>`), also set while VaM's own menus are open (`DLSSMenuNative`) |
| `VRMenuIni.cs` | Ini text editing that changes values only, keeps other lines, comments, order and line endings, case-insensitive like `GetPrivateProfileString` |

## Build

`python tools/vamkit build vr-menu` runs every step for this component (plugin) and writes to `out\vr-menu\`; the
steps are listed in `manifest/components.toml`. The plugin is compiled by that command from the `[component.managed]`
recipe there (C# 3.5 compiler, against the user's VaM).

A BepInEx plugin compiled with the .NET 3.5 C# compiler (C# 3) against the user's `BepInEx\core` and
`VaM_Data\Managed` (`VAMKIT_VAM`, `VAMKIT_CSC`); the four files are one assembly, output `out\vr-menu\VaMVRMenu.dll`.

## Settings

No cfg of its own. What the tab edits (key: file, section, key):

| Page | Rows (file `[section] key`) |
|---|---|
| 插帧 (frame generation) | `ofxr_bridge.ini [ofxr] frame_multiplier` (2 to 4; desktop: the presenter), `performance.ini [Performance] FrameGeneration`, `DLSSGLite`, overlay `[overlay] position` |
| 清晰度 (clarity) | `[Performance] DLSSPreset` (`Off`/`K`/`L`/`M`, route off), `RenderScale` (K/L/M each remembered as `RenderScaleK/L/M`), `RenderScaleRR` (path tracing on), `OutputScale` 75 to 150, `DLSSSharpness`, `DLSSSharpenLast`, `RRStereo` (VR + path tracing), skin cfg `[RayTracing] RRDetail` |
| 降噪 (denoiser) | `[NR] RenderPreset`, `Style`, `Intensity`, `LocalToneStrength`, `LocalStructureStrength`, `SkinStructureStrength`, `FoveationSize`, `RegionShiftY`; `[Performance] NRAfterUpscale`, `NRAfterScale`, `NRFeatherPercent` |
| 光影 (lighting) | skin cfg `[RayTracing] Renderer` (path tracing on / off), `PathBounces`, `PathBackground` (shown only with path tracing); tone-mapping cfg `[General] Mode`, `Style` |
| 皮肤 (skin) | skin cfg `[Subsurface] Strength`, `[General] Strength` (thin parts), per-character gloss level through the skin plugin's `SkinGlossLevels` / `SetSkinGlossLevel` |
| 身体 (body) | Settings of other plugins found by id when loaded (the soft body and animation clock of the development tree are not in this repository; rows for absent plugins are left out) |

Rows under a switch that is off are hidden and the rest move up; a long section is cut into sub-pages. The launcher carries the same values
in its presets (`components/render/launcher/profile-manifest.json`); keep the two lists agreeing.

## Interfaces

- Environment: `VAM_PERFORMANCE_CONFIG` (the run's `performance.ini`; the tab does nothing without it), `VAM_DISPLAY_MODE` (`Desktop` looks
  for the presenter module through its export `OFXR_GetProviderApiVersionV4` instead of `XR_APILAYER_XRFrameBridge_diagnostic.dll`).
- Writes values in place and replaces each file whole through a temporary file and `MoveFileEx`, so a reader never sees half a file; changes
  made elsewhere are picked up within a second. The compositor and the NR pre-pass re-read `performance.ini` about every 250 ms; the OFXR layer
  re-reads its ini every 250 ms.
- Other plugins' settings (skin, tone-mapping) are set through their `BepInEx` `ConfigEntry` in memory (`PluginSetting(guid, section, key)`), so they
  follow at once and BepInEx saves them.
- Calls the skin plugin's `RendererState()` for the live route status.

## Tests

None offline in this repository (the development tree had `VRMenuIniTest.cs` for `VRMenuIni` and an INI check over all rows; neither was moved here).
Only the game shows the layout; the log and the "rows ... in ... cards ... everything fits" layout line say whether pages fit without scrolling.

## Invariants

- Only values change in other programs' files: every other line, comment, key order and line ending stays, so the launcher and the OFXR tray keep reading them.
- Any failure leaves VaM's own menu untouched; the launcher still works.
- Nothing scrolls and every target stays big (laser pointer): when adding a row, add it to a page or sub-page that has room, and put children under their
  switch with `parent` / `group` so they hide with it.
- Controls show the real value and unit; no hint text (labels and values say what a row does); each setting lives in one place.
- Rows that only make sense for one route carry a rule (for example `pathRule`) so they are hidden on the other.

## Read next

[../../../docs/knowledge/settings-and-menu.md](../../../docs/knowledge/settings-and-menu.md)
