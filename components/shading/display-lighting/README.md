# display-lighting

An optional, live-only lighting override for showing a character: eleven styles of a small lamp rig (key, fill, rim,
backdrop) placed around the subject in the viewer's frame, an ambient colour and an environment probe, started and
stopped from a "Display Lighting" tab in VaM's menu. Authored lamp values and transforms are never edited; only their
enabled bits are held while the override runs. Depends on BepInEx only. Optional (`optional = true` in the manifest);
tone-mapping's mode 1 follows it.

## Files

| File | Holds |
|---|---|
| `VaMDisplayLighting.cs` | BepInEx plugin `local.vam.displaylighting` 1.6.0: the style table (`Styles`), rig construction (`BuildRig`), lamp motion modes, scene lamp capture, start / stop, test API |
| `VaMDisplayLighting.Menu.cs` | The "Display Lighting" tab: start / stop button, one button per style, status text |

## Build

`python tools/vamkit build display-lighting` runs every step for this component (plugin) and writes to
`out\display-lighting\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command
from the `[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

Pure C#, two files, compiled as a BepInEx plugin against the user's `BepInEx\core` and `VaM_Data\Managed` with the
.NET 3.5 C# compiler (`VAMKIT_VAM`, `VAMKIT_CSC`). Output belongs in `out\display-lighting\VaMDisplayLighting.dll`.

## Settings

`BepInEx/config/local.vam.displaylighting.cfg`:

| Section / key | Default | Meaning |
|---|---|---|
| `[Lighting] Style` | `Beauty` | One of `Beauty`, `Rembrandt`, `Window`, `Golden`, `Contour`, `Cinema` (traditional) and `Clear`, `Pastel`, `Neon`, `Sunset`, `Flash` (contemporary). Changing it rebuilds the rig while running |

Lighting itself is not on by default: it starts only from the tab's button (`StartLighting()`), and stops on a scene load,
when the subject atom is removed or switched off, or when the scene is loading.

## Interfaces

- Public methods (test agent, tab buttons): `StartLighting()`, `StopLighting()`, `SetStyle(string)`, `State()`, `LampState()`.
- Reads while running: the subject atom, the viewer camera (`Camera.onPreCull`), the Skyshop light controller and `mset.Sky`
  environment, and VaM's global lighting values (master, ambient, exposure, diffuse, specular, skybox), which it restores
  on stop.
- Each lamp (`LampSpec`): azimuth and elevation around the subject-to-viewer direction, luminance at the aim point in
  linear units before N.L, colour, spot angle, shadow edge radius in metres (0 = no shadow), shadow resolution, aim
  (`Face`, `Upper`, `Body`, `Backdrop`) and motion (`Follow`, `Sun`, `Fixed`).
- tone-mapping reads this plugin's running / suspended state (soft dependency) for its `Mode = 1`.

## Tests

None offline: only the game shows it. The test agent (`tools/lab/test-agent`) can start, stop and switch styles and read
`State()` / `LampState()`.

## Invariants

- Live-only: never write authored lamp values or transforms into the scene; scene saves must not contain the rig
  (`BeforeSave` / `AfterSave` hooks take it out and put it back).
- No timed discovery of scene objects: the whole-scene lamp seed is the explicit start action; later lamps arrive through
  the atom-added handlers.
- Everything changed (global lighting values, ambient mode, probe, cubemap, skybox rotation) is restored on stop.

## Read next

[../../../docs/knowledge/tone-and-display-lighting.md](../../../docs/knowledge/tone-and-display-lighting.md)
