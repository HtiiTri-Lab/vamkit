# dlss-bridge

The Unity side of the VR / desktop compositor. The compositor (opencomposite) runs in `openvr_api.dll` and can only see what
Unity submits; this BepInEx plugin supplies the rest for the main camera: sub-pixel Halton jitter, MSAA off, camera matrices,
depth captured at a chosen camera event, object motion (skins, clothing, hair, rigid props, animated custom assets) as buffers
for the native side to redraw as motion vectors, the render scale, UI rectangles for the path tracer's composition, the
scene's post-processing run on the reconstructed image, and in desktop mode the window-to-render-target input mapping. The
player gets correct DLSS / RR guides and motion. Dormant unless the launcher set `VAM_DLSS_BRIDGE=1` and the running
`openvr_api.dll` is our OpenComposite build (it exports `VaMDLSS_*`). Depends on BepInEx + Harmony and on opencomposite.

## Files

| File | Holds |
|---|---|
| `VaMDLSSBridge.cs` | The whole plugin `local.vam.dlssbridge` (2.9.19, about 2100 lines): jitter and capture, `FrameInfo` slots, object / hair / rigid / skinned motion collection, render scale, UI for path tracing, post-processing hand-over, desktop input mapping |

## Build

`python tools/vamkit build dlss-bridge` runs every step for this component (plugin) and writes to `out\dlss-bridge\`;
the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from the
`[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

A BepInEx plugin compiled with the .NET 3.5 C# compiler (C# 3) against the user's `BepInEx\core` (BepInEx, 0Harmony)
and `VaM_Data\Managed` (UnityEngine, Assembly-CSharp, UI): `VAMKIT_VAM`, `VAMKIT_CSC`; output
`out\dlss-bridge\VaMDLSSBridge.dll` (installed to `BepInEx\plugins`). Uses `unsafe`-free code only; no generated
sources.

## Settings

No cfg of its own. Behaviour follows the live `performance.ini` through the native side (`[Performance] RenderScale`, `DLSSPreset`,
`OutputScale`, `DLSSMenuNative`; render scale is applied by setting Unity's eye texture scale to VaM's own scale times
`VaMDLSS_GetEyeTextureScale`, a slider being dragged applies once it rests 0.5 s). Environment: `VAM_DLSS_BRIDGE=1` (required),
`VAM_DISPLAY_MODE` (`Desktop` = one view and the desktop output camera; otherwise two eyes). Static test switches for the test agent:
`PostOnRr` / `PostTest(bool)` (post-processing on RR's output), `PostDofInVr` / `PostVrDof(bool)` (depth of field in VR; off by
default because the author's camera values assume a monitor field of view), `PathUiAlwaysShadersOnTop`.

## Interfaces

- Imports from `openvr_api.dll` (`[DllImport("openvr_api")]`): `VaMDLSS_GetApiVersion`, `GetRenderEventFunc`, `GetState`,
  `GetJitterPhaseCount`, `GetRenderScalePercent`, `GetEyeTextureScale`, `GetDesktopSize`, `CaptureDesktop`, `CaptureGenerated`, `GetPostEventFunc`,
  `GetPostSize`. `CaptureDesktop(path)` and `CaptureGenerated(path)` are public statics for tests (through the test agent): the compositor's desktop
  output, and the presenter's next generated frame with the real frame it follows, as `.ppm` files.
  The state bits (`StateSelected`, `StateObjectMotion`, `StateHairMotion`, `StateSrOff`, `StateRigidMotion`, `StateSkinnedMotion`,
  `StatePathRendering`, ...) are the native side's; `ApiVersion = 3`.
- Shared layouts that must match `components/render/opencomposite/OpenOVR/Compositor/dx11_vam_dlss.cpp`: `FrameSize` 1040 (`FrameInfoV2`),
  `DrawStride` 64 (`ObjectDraw`), the `kDraw*` flags (`DrawHair` 2, `DrawRigid` 8, `DrawSkinned` 16), `OpaqueDepthEvent` 5 (`kOpaqueDepthEvent`), `FrameMagic`.
  Change both sides together.
- Render events: the frame data goes through `CommandBuffer.IssuePluginEventAndData` at the capture camera event (`AfterForwardAlpha`;
  `BeforeForwardAlpha` also issues a depth event for the guides).
- Harmony hooks: `DAZSkinV2` / `DAZSkinWrap` / `HairRender.Awake` draws, `Atom` mesh-renderer lists, `CustomUnityAssetLoader.DoLoadedCallbacks`,
  `PostProcessingBehaviour.OnRenderImage` (source and destination swapped for RR's image), the camera's input methods in desktop mode.
- Materials considered for motion: only opaque queues (`renderQueue <= 2500`), not shadow-only renderers; a clothing part hidden by
  `Alpha Adjust = -1` on `Custom/Subsurface/*SeparateAlpha*` (queue >= 2450) is not redrawn.

## Tests

None offline: it needs Unity, VaM and the compositor. In game or on the simulated headset the log shows "hidden clothing part ... not redrawn for motion",
the PT UI shader decisions, "VaM desktop input: window coordinates mapped in N managed input methods" and the desktop size line. Whether motion
is right is judged on the picture (frame generation and DLSS ghosting), not by a probe.

## Invariants

- Dormant without `VAM_DLSS_BRIDGE=1` and our `openvr_api.dll`: vanilla must not feel this plugin.
- No scene searches per frame or on a timer: hair, rigid and skinned renderers are discovered through Harmony postfixes and VaM's own per-atom
  lists (the earlier timed search cost two ~95 ms frames every 2 s in a large scene).
- Transparent (non-depth-writing) objects get no motion or depth of their own, so they cannot take a face's pixels from the surface behind;
  people's skin, clothing and hair match against either depth the compositor holds.
- Struct sizes and flags above stay equal to the native side; a mismatch shows as wrong motion or a crash, not an error.
- The pinned buffers of a skinned renderer that left view are rebound when it returns (otherwise "Handle is not pinned" stops its motion for the session).

## Read next

[../../../docs/knowledge/motion-vectors.md](../../../docs/knowledge/motion-vectors.md),
[../../../docs/knowledge/upscaling.md](../../../docs/knowledge/upscaling.md),
[../../../docs/knowledge/frame-path.md](../../../docs/knowledge/frame-path.md)
