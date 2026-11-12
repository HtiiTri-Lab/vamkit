# Finding the cause of a problem

An index from what is seen to where it is explained. Each knowledge document has a "Pitfalls" section with the cause
and the remedy; this page only gets you to the right one. Read
[../protocol/testing.md](../protocol/testing.md) before starting sessions to look for a cause: most of these were found
by reading code, not by capturing more.

## First moves, always

0. **What is different here?** `python tools/vamkit versions --vam <VaM>` marks every line where this machine differs
   from the one the kit was made on, with what that line has meant so far. A problem nobody has seen before most
   likely lives in one of them ([reference-machine.md](reference-machine.md)).
1. **Which run?** Find the run's folder under `<VaM>/vamkit/logs/` and read what that session actually loaded: plugin
   versions, which switches were on, the lines that say a feature went live. Do not reason about a session from memory.
2. **Is the pipeline alive?** In that run's logs: did every DLSS feature get created (no create failure), did the path
   tracer take over when it was switched on, is frame generation generating, did our plugins load without errors.
3. **Does it happen with the feature off?** Every part has a switch (`AGENTS.md`, "Settings an agent may change"). One
   switch flipped inside the same session separates "ours" from "the scene's" faster than anything else.
4. **Does it happen in vanilla?** `launch.py --variant vanilla`. If yes, it is not this project.
5. **State the mechanism before changing anything**: which stage supplies this pixel's colour, motion or depth; which
   thread stops; which file a setting is read from.

## Nothing works, or it does not start

| Seen | Read |
|---|---|
| No upscaling, no neural rendering, no frame generation, path tracing never takes over; the logs show a DLSS create failure | [gc.md](../knowledge/gc.md) (address space taken from CUDA), then anything else that reserves address space or hooks system calls |
| The picture freezes for about half a minute the first time path tracing starts after a build | [path-tracing.md](../knowledge/path-tracing.md): the driver compiles the pipelines once |
| The in-game settings tab is missing | [settings-and-menu.md](../knowledge/settings-and-menu.md) |
| VR launch refused when started as administrator | [settings-and-menu.md](../knowledge/settings-and-menu.md) |
| After a killed run VaM starts with the wrong files (`vamkit verify` says the mod's file is in VaM's place), or the frame generation layer stays registered | [../protocol/testing.md](../protocol/testing.md), "Stopping a test midway"; `launch.py --restore` |
| Crash at scene load inside d3d11 after the shader generators were edited | [skin.md](../knowledge/skin.md) |
| Crash with d3d11 on top of the stack and the hair plugin's native library below | [hair.md](../knowledge/hair.md): the hook only forwards; look further down the stack |
| Rare crash in Mono during startup | [rejected-and-open.md](../knowledge/rejected-and-open.md): open |
| Hang at the moment a menu opens or closes | [upscaling.md](../knowledge/upscaling.md), [rejected-and-open.md](../knowledge/rejected-and-open.md): open |
| The picture freezes (or, before presenter v3, VaM stops responding) with the GPU nearly idle, at the first upscaled or path-traced frame | first the `on12:` lines in `d3d11_proxy.log`: the `d3d11on12.dll`, the Windows build and the unwrap probe's verdict. Windows 10 and Windows 11 builds without Microsoft's fix stop 11on12's queue when an unwrap names it; the kit names it only after `vamkit/bin/unwrap_probe.exe` has cleared the DLL ([d3d11on12.md](../knowledge/d3d11on12.md), "Unwrap / return"). Then the `desktop wait:` and `desktop presenter:` lines in `dlssg.log` say which queue stopped ([frame-generation.md](../knowledge/frame-generation.md): the desktop presenter); `D3DINTEROP_ON12_WORKER=0` runs 11on12 without its worker thread |
| Process crash at exit (0xC0000409) | [cpu.md](../knowledge/cpu.md) |
| A plugin logs that it left VaM's code alone (unknown method, unknown `mono.dll`) | [adapt-vam-version.md](adapt-vam-version.md) |

## The picture is wrong

| Seen | Read |
|---|---|
| Render scale has no effect with path tracing on; `DLSSPreset=RR` behaves as K with it off | [upscaling.md](../knowledge/upscaling.md): both intended |
| Upscaling pauses and the image goes soft | [upscaling.md](../knowledge/upscaling.md): VaM's own render scale |
| Small menu text shakes; the picture changes when a menu opens | [upscaling.md](../knowledge/upscaling.md), [neural-rendering.md](../knowledge/neural-rendering.md): native frames while a menu is open |
| Ghosting behind transparent things on head turns; hair shimmering at a low render scale | [upscaling.md](../knowledge/upscaling.md), [motion-vectors.md](../knowledge/motion-vectors.md) |
| A face or body outline warps while frame generation is on | [motion-vectors.md](../knowledge/motion-vectors.md): whose motion and depth the pixel carries |
| Dark bars at the picture's edge when the view pans with frame generation | [frame-generation.md](../knowledge/frame-generation.md) |
| Dark speckles that grow on still skin with path tracing and vanish when the camera moves | [path-tracing.md](../knowledge/path-tracing.md): separate tracer from reconstructor with the reference view first |
| Objects of a room look untextured, or traced textures are stale, with path tracing | [path-tracing.md](../knowledge/path-tracing.md) |
| A path-traced scene looks darker, flatter or differently lit than with raster lighting | [path-tracing-authored-lighting.md](../knowledge/path-tracing-authored-lighting.md): compare with third-party post-processing off; fill in what the tracer failed to read, do not converge |
| A lamp seems blocked by its own fixture; a panorama tints the scene; a light cone turned into a dark solid | [path-tracing-authored-lighting.md](../knowledge/path-tracing-authored-lighting.md), [path-tracing.md](../knowledge/path-tracing.md) (adapters) |
| A scene's own menu or a world-space panel is missing or hidden behind a person with path tracing | [frame-path.md](../knowledge/frame-path.md), [path-tracing.md](../knowledge/path-tracing.md) (UI composition) |
| Hard line under the chin or along shadow edges; plastic or flat highlights; dark eyes with a clothing item | [skin.md](../knowledge/skin.md) |
| Highlights grey or hazy, halos, tone mapping applied twice | [tone-and-display-lighting.md](../knowledge/tone-and-display-lighting.md) |
| The picture after neural rendering is warmer or darker in one configuration | [neural-rendering.md](../knowledge/neural-rendering.md): the packed canvas width |
| Hidden accessories appear, or clothing looks too sheer | [clothing.md](../knowledge/clothing.md), [path-tracing.md](../knowledge/path-tracing.md): what a negative alpha adjustment means |
| Hair too dark, too bright or too costly with path tracing | [path-tracing.md](../knowledge/path-tracing.md), [hair.md](../knowledge/hair.md) |

## Motion is not smooth

| Seen | Read |
|---|---|
| Everything stops for most of a second every few minutes | [gc.md](../knowledge/gc.md) |
| Two long frames at a fixed period (for example every 2 s), worse in heavy scenes | [../protocol/design-rules.md](../protocol/design-rules.md), "No timed whole-scene work"; [hair.md](../knowledge/hair.md) |
| The real frame rate sits one step below what the GPU load suggests | [frame-budget.md](../knowledge/frame-budget.md): the cadence rule |
| On the desktop the window flashes away for an instant, or the frame rate is quantised | [frame-generation.md](../knowledge/frame-generation.md): the desktop presenter |
| One long frame a few seconds after a feature was created; a hitch of 1 to 3 s when a model or size changes | [neural-rendering.md](../knowledge/neural-rendering.md), [upscaling.md](../knowledge/upscaling.md) |
| One part of a character jumps or lags while the rest moves | [../protocol/performance.md](../protocol/performance.md), "Two different smoothness problems"; then the component's document |
| The character's motion looks softened or delayed against its animation | [../protocol/design-rules.md](../protocol/design-rules.md), "The character's motion wins" |
| Hair or cloth moves differently from stock VaM | [hair.md](../knowledge/hair.md), [clothing.md](../knowledge/clothing.md): run the verify mode; the result must equal GPUTools' |

## Numbers do not add up

| Seen | Read |
|---|---|
| Two sessions with the same settings differ by a few frames per second | [frame-budget.md](../knowledge/frame-budget.md), [../protocol/performance.md](../protocol/performance.md): compare inside one session |
| A GPU saving does not show up as frame rate | [frame-budget.md](../knowledge/frame-budget.md): CPU-bound, or already on the beat |
| GPU time rises when the GPU is not saturated | [clothing.md](../knowledge/clothing.md), [d3d11on12.md](../knowledge/d3d11on12.md): waiting counts in timestamps |
| VR frame rate looks too good on a stage with its own UI camera | [path-tracing-authored-lighting.md](../knowledge/path-tracing-authored-lighting.md) |
| A worker thread's CPU time looks high | [cpu.md](../knowledge/cpu.md): spinning between tasks is counted |

## The build fails

`python tools/vamkit build <id>` repeats one component; `--steps generate`, `native` or `managed` repeats one step. The
full output of every step is in `out/logs/<id>.<step>.log`.

| Symptom | Cause | Remedy |
|---|---|---|
| `needs ... in third_party/` before the first step | the item was never fetched, or its pin in the manifest changed | the `fetch` command the line names |
| OpenComposite: `atlbase.h` not found | Visual Studio without the "C++ ATL" component | add the component, or point `VAMKIT_ATL_ROOT` at an `atlmfc` folder |
| a generator stops: a program was not found or could not be split | this VaM build's shaders are not the ones the edits were written for | [adapt-vam-version.md](adapt-vam-version.md); do not loosen the check |
| `gen_shaders.py` fails on an unknown option or intrinsic | an old Windows SDK's `dxc` (the tracer compiles `lib_6_5` / `cs_6_5` with HLSL 2021) | a current Windows SDK, or `VAMKIT_DXC` naming a newer `dxc.exe` with its `dxil.dll` |
| the C# compiler rejects syntax that looks fine | the plugins are C# 3 (`csc` of .NET Framework 3.5), which VaM's Mono runs: no `?.`, no string interpolation, no `out var` | write it the old way; do not switch compilers |
| a `build.cmd` run by hand says a variable is not set | the scripts take `VAMKIT_VAM`, `VAMKIT_VCVARS`, `VAMKIT_FXC`, `VAMKIT_DXC`, `VAMKIT_CSC` from `vamkit build` | run them through `vamkit build`, or set the variables yourself (`vamkit doctor` prints the folders it found) |
| BepInEx logs a load error for a native library | it was copied into `plugins` as `.dll` | native libraries are installed as `.bin` (`installs` in the manifest) |
| a file under an upstream component is missing after a clone | the upstream `.gitignore` inside `opencomposite` or `ofxr-dlssg` ignores it | `git status --ignored` in the checkout that has it; keep build inputs out of ignored paths |

## Tools

`tools/lab/README.md` lists them: thread stacks of a hung process, crash-address lookup, the pacing log reader, the CPU
census, and the path tracer's reference view (Ctrl+Alt+R).

## When you find a new cause

Write it into the "Pitfalls" section of the document it belongs to, as symptom, cause, remedy, and add a row here.
