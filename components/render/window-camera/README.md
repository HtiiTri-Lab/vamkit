# window-camera

A WindowCamera atom with "Camera On" switches on two cameras: MainWindowCamera, which renders the whole scene again (every
shadow map included) to the desktop window, and MiniCamera, which renders a small texture onto the atom's display planes. In VR nobody
looks at the desktop window, and the display planes are not needed, so this plugin keeps both Camera components off. In our desktop mode
MainWindowCamera is off as well, because the pipeline's output camera writes the finished picture over the whole window after it. The player gets
the GPU time of a second full scene pass back (about 0.5 ms and 0.3 ms of GPU on a simple scene on the simulated headset; with path
tracing the first camera's region measured 5.3 ms against 1.6 ms with VaM's own lighting, because it also carries work that is not its own).
Depends on BepInEx + Harmony only.

## Files

| File | Holds |
|---|---|
| `VaMWindowCameraVR.cs` | Plugin `local.vam.windowcameravr` 1.3.0: Harmony postfixes `AfterSync` on `CameraControl.SyncCameraOnAndUseAudioListener` and `SyncUseAsMainCamera`; the set of cameras it turned off |

## Build

`python tools/vamkit build window-camera` runs every step for this component (plugin) and writes to
`out\window-camera\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from
the `[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

A BepInEx plugin compiled with the .NET 3.5 C# compiler against the user's `BepInEx\core` and `VaM_Data\Managed`
(`VAMKIT_VAM`, `VAMKIT_CSC`); output `out\window-camera\VaMWindowCameraVR.dll`. No generated sources.

## Settings

None. Behaviour is fixed: MainWindowCamera (no target texture, no eye) is off while VR is on, or in our desktop mode (`VAM_DISPLAY_MODE=Desktop`, set by the
launcher) unless it is set as VaM's main camera; MiniCamera (no eye) is off whenever the plugin is loaded. VaM's own Desktop Mode (no `VAM_DISPLAY_MODE`, no VR)
keeps MainWindowCamera; vanilla launches load no BepInEx, so VaM is entirely untouched there.

## Interfaces

- Reads `VAM_DISPLAY_MODE` and Unity's `XRSettings` state; patches `CameraControl` (VaM's `Assembly-CSharp`): `SyncCameraOnAndUseAudioListener`
  (from `Init` and on every Camera On or audio listener change) and `SyncUseAsMainCamera`.
- Leaves alone: the AudioListener on MainWindowCamera, the "Camera On" toggle and what scenes save. A camera set as VaM's main camera
  ("Use As Main Camera") keeps rendering, because `Camera.main` would change with it. The MiniCamera display planes keep the last picture.
- Nothing searches the scene; it only reacts to the two hooks.

## Tests

None offline: only the game shows it. Check the log line "VaM WindowCamera VR: MiniCamera stays off; MainWindowCamera stays off in VR ..." and that a scene
with a WindowCamera atom renders its cameras off while a headset (or the desktop pipeline) runs. The effect on frame time is only visible in game (the numbers
above are from a simulated headset, one simple scene each).

## Invariants

- Only Camera components are switched; nothing the scene or a save stores is changed, and what it turned off is remembered so it can be restored.
- A camera VaM uses as the main camera is never turned off.
- Plain VaM desktop mode (without our environment variable) behaves as VaM.
- Applies only to scenes with a WindowCamera atom that has Camera On.

## Read next

[../../../docs/knowledge/frame-budget.md](../../../docs/knowledge/frame-budget.md),
[../../../docs/knowledge/frame-path.md](../../../docs/knowledge/frame-path.md)
