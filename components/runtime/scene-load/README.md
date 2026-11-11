# scene-load

Loading a heavy VaM scene spends most of its time on work VaM repeats or does one step at a time: compiling the same
scene plugins from source on every load, reading asset bundles one after another and decompressing them on Unity's single
loading thread, making atoms one per frame, building button objects for closed dropdowns, decoding hundreds of Vorbis
clips on the main thread. This plugin removes that work (or moves it onto the shared CPU workers) and gives VaM's own
result. It also keeps a scene switch inside one VaM session from breaking. Measured numbers and background are in
[../../../docs/knowledge/loading.md](../../../docs/knowledge/loading.md).

BepInEx plugin `local.vam.sceneload` 1.6.0. Native work uses cpu-boost's worker pool when it is present and runs on the
calling thread when it is not.

## Files

| File | Holds |
|---|---|
| `VaMSceneLoad.cs` | Plugin entry, plugin-compile cache, reads out of `.var` packages (entry index, morph deltas), VaM load-path shortcuts (anatomy sync, package paths, morph sort, package file search), verify entry points |
| `BundleLoads.cs` | Asset bundles prepared in memory, same-frame hand-over, parallel Vorbis decoding |
| `LoadFrames.cs` | Batched atom creation, deferred dropdown buttons, `Selectable` removal by reference, morph metadata cache |
| `SceneSwitch.cs` | Scene switches that do not break, kept bundles, unloading inactive appearance items, texture cache cap |
| `native/` | `sceneload.cpp` (`.var` entry read + inflate), `bundlecache.cpp` (bundle preparation, Vorbis rewrite), `vorbisdec.cpp/.h`, `memstate.cpp` (video memory in use), `vorbis_setup_index.h`, `build.cmd`; vendored `third_party/` (miniz, libogg 1.3.5, libvorbis 1.3.7) |
| `native/tools/` | `vorbis_setups.c`, `setup_index.py`, `build-vorbis-tools.cmd`: regenerate `vorbis_setup_index.h` |
| `tools/` | Python checks that take the user's own packages as arguments (see Tests) |
| `tests/` | In-game probe plugins (sources only; not built by the manifest) |

## Build

`python -I tools/vamkit build scene-load` runs both steps and writes `out\scene-load\`:
- native: `native\build.cmd` (needs `VAMKIT_VCVARS`), `VaMSceneLoadNative.dll` copied to `VaMSceneLoadNative.bin`;
- managed: `VaMSceneLoad.dll` from the `[component.managed]` recipe in `manifest/components.toml`.

## Settings

`BepInEx/config/local.vam.sceneload.cfg`. Every feature is on by default; each key switches that feature off.

| Section / key | Default | Meaning |
|---|---|---|
| `[PluginCache] Enabled` | `true` | Scene plugins compiled before are not compiled again (assemblies cached in `BepInEx\cache\VaMSceneLoad\plugins`) |
| `[PackageRead] FastEntryLookup` | `true` | Index per `.var` package instead of SharpZipLib's linear entry scan |
| `[PackageRead] FastMorphDeltas` | `true` | Morph deltas (`.vmb`) read and inflated natively in one go |
| `[LoadPaths] SkipRepeatAnatomySync` | `true` | Skip anatomy recomputation after each clothing / hair item when its inputs did not change |
| `[LoadPaths] FastRemovePackageFromPath` | `true` | `FileManager.RemovePackageFromPath` without regular expressions |
| `[LoadPaths] FastMorphSort` | `true` | Morph list sort text built once per sort |
| `[LoadPaths] FastVarFileSearch` | `true` | Remembered prefix test per folder in `FindVarFilesRegex` |
| `[LoadPaths] BatchAtomCreation` | `true` | Atoms whose prefab is in memory are made without waiting a frame each |
| `[LoadPaths] AtomFrameBudgetMs` | `100` | Milliseconds of atom creation per frame |
| `[LoadPaths] DeferPopupButtons` | `true` | A closed dropdown builds its button objects when opened |
| `[LoadPaths] FastSelectableRemove` | `true` | `Selectable.OnDisable` removes itself by reference |
| `[LoadPaths] MorphMetaCache` | `true` | Morph metadata (`.vmi`) kept per file in `BepInEx\cache\VaMSceneLoad\morphmeta.bin` |
| `[BundleLoad] InMemory` | `true` | Asset bundles prepared in memory in the background, read by Unity through a stream |
| `[BundleLoad] SameFrame` | `true` | Several ready bundles handed over per frame |
| `[BundleLoad] FrameBudgetMs` | `100` | Milliseconds of hand-overs per frame |
| `[BundleLoad] KeepForNextScene` | `true` | A bundle whose last user let go stays loaded if the next scene names it (60 s otherwise) |
| `[BundleLoad] KeepSeconds` | `60` | How long an unused bundle stays loaded outside a scene switch |
| `[Audio] ParallelDecode` | `true` | `DecompressOnLoad` Vorbis clips reach Unity as PCM16 decoded on the workers |
| `[Audio] DecodeAheadMB` | `4096` | Decoded clips not read yet at which background decoding waits |
| `[SceneSwitch] KeepClearing` | `true` | Scene clears and bundle callbacks that throw are logged and carried on |
| `[SceneSwitch] ReleaseOrphanBundles` | `true` | After a clear, bundles whose requesters were all destroyed are let go |
| `[Memory] UnloadInactiveAtVideoMemoryPercent` | `85` | Unload inactive appearance items when video memory reaches this share of its budget (no scene loading) |
| `[TextureCache] MaxGB` | `0` | Size cap of VaM's texture cache in GB; `0` = no cap, nothing is deleted (see below) |

## What it touches on disk

- `BepInEx\cache\VaMSceneLoad\` : compiled plugin assemblies, `morphmeta.bin`, `texturecache-use.txt`. Safe to delete.
- **VaM's texture cache `Cache\Textures` is left alone unless the user sets a cap.** VaM never trims this cache (it
  reached 91 GB on the development machine). With `[TextureCache] MaxGB` above 0 (GB of 2^30 bytes) the files used
  least recently are deleted in the background, down to 90% of the cap, never one used in this run; the first start
  with a larger cache deletes at once (about 30 s after start). The cache holds pixels VaM regenerates from the images
  the next time it needs them; no scene, preset or package is touched. The default is 0: nothing is deleted. Do not
  set a cap for someone without telling them how much will go. The last use of each cache file is recorded either way
  (`texturecache-use.txt`), so a cap set later knows what was used.
- Asset bundles are no longer written to disk; they live in memory while loaded (a heavy scene holds several GB).

## Tests

None in the manifest: every check needs the user's packages or a running game.
- `tools/scene_bundles.py` lists a scene's asset bundles (`VAMKIT_VAM` = the VaM folder; `--json` for the other tools).
- `tools/bundle_check.py`, `tools/mem_bundle_check.py` compare the native bundle preparation with a Python decode of the
  same `.var` entries (arguments in each docstring; they load the built `VaMSceneLoadNative.dll`).
- `tools/driver_check.py` runs the bundle job driver on cpu-boost's workers over a bundle list.
- `tools/fsb5.py`, `tools/audio_scan.py` read FSB5 clip layouts out of a bundle.
- `tests/*.cs` are probe plugins a test run copies into `BepInEx\plugins` and removes (`MemProbe`: memory and caches
  across load / clear; `BundleSceneProbe`: what a load left behind; `AudioTruthProbe`: what Unity decoded for each clip).
- The plugin has verify entry points (`Verify...` in `VaMSceneLoad.cs`: morph sort, package paths, entry lookup, deltas,
  anatomy) that compare with VaM's own methods on the loaded game.

## When it meets the unknown

Each patch is looked up by reflection (methods, fields, signatures). One that is not as expected leaves that feature
off and logs it; the rest go on. A bundle Unity refuses is loaded the way VaM does; after the second refusal bundle
preparation stays off for the run. Anything the native reader does not accept (loose files, unpacked packages, short
data) goes to VaM's own method.

## What it does not cover

The plugin-compile cache needs VaM's `ScriptCompiler` as in VaM 1.22. The bundle rewrite and the Vorbis path are for
Unity 2018.1 asset bundles and FMOD's stored setup headers (a clip whose setup it cannot reproduce stays Vorbis).
Decoded audio differs from FMOD's by 1 on about one value in 5000. First load after a start is cold; the gains are
largest on reloads and on scenes with many plugins, bundles and atoms.
