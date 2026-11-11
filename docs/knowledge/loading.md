# Loading: scene load and image load

A heavy VaM scene takes minutes to load, most of it single-threaded work VaM does on its main thread or on one worker,
one item at a time. Two plugins shorten that without changing what is loaded:

- **scene-load** (`components/runtime/scene-load`, installed as `VaMSceneLoad.dll` plus the native `VaMSceneLoadNative.bin`,
  plugin id `local.vam.sceneload`, version 1.6.0): plugin compile cache, faster reads out of `.var` packages, less repeated
  work in VaM's load path, asset bundles prepared in memory, atoms made without a frame each, deferred dropdown buttons,
  audio decoded in parallel, scene-switch management, a cap on VaM's texture cache.
- **image-load** (`components/runtime/image-load`, installed as `ImageLoadPipeline.dll` plus `ImageLoadWorkNative.bin`,
  plugin id `local.vam.imageloadpipeline`, version string 0.1.0 in every build, so identify a build by its hash): VaM's
  image queue (character and clothing textures) made parallel.

Both work on any lighting route, in VR and on the desktop, and both aim at loading time only; neither adds per-frame work in
play (the image plugin's main-thread cost in a CPU census was 0.005 ms per frame; scene-load's was not censused), and the
resident costs are the memory and cache effects named below. Almost every change is meant to give exactly VaM's result; the places where it knowingly does not are listed under
"Where the result differs from VaM's" and must be read before changing anything. Loads here are measured as **total time
from starting the load to its end** unless said otherwise; the time until the scene is *usable* can be later (see the
audio frame below).

## How VaM loads a scene, and where the time goes

Facts about VaM 1.22.0.3 (Unity 2018.1, Mono 2.x, FMOD) that this work rests on. The collector side is in [gc](gc.md);
more VaM facts in [vam-facts](vam-facts.md).

**The load coroutine.** `SuperController.LoadInternal` and `LoadCo` run on the main thread: clear the old scene (every
atom is pre-restored and removed, people go back to a pool, `MemoryOptimizer` calls `Resources.UnloadUnusedAssets` and a
full `GC.Collect`), create every atom (one `yield` per atom, so a scene with N atoms needs at least N frames), restore the
atoms' content, restore plugins (**one frame**, can be tens of seconds), then wait for asset bundles, then about 35 more
frames of settling. A frame that long is what Windows reports as "not responding".

Where the time went in the two heavy scenes studied (desktop mode, path tracing on, original VaM; a "stage scene" with 106
atoms, 137 plugin instances of 43 distinct plugins, about 17 MB of plugin source; and a larger stage scene with 821 atoms, 33
sub-scenes, 322 custom-unity-asset (CUA) atoms of which 310 were inactive, 197 asset bundles):

| Part | What VaM does | Cost seen |
|---|---|---|
| Plugin compile | every plugin instance is compiled from source by `mcs` on the main thread on every load, no cache | 15.0 s of a 30 s plugin frame (first load, stage scene) |
| Person creation | morph library, clothing / hair catalogues, instantiation, about 4-7 s per person | 5-7 s per person |
| Asset bundles | `AssetLoader` handles one request per coroutine step: reads the `.var` entry into a managed `byte[]` on a thread, `LoadFromMemoryAsync`, callback; LZMA bundles are decompressed whole on Unity's single loading thread; at least one frame between requests; inactive CUAs are loaded too | 70-150 s of a 166.8 s load (large scene, original VaM), about 20 ms per frame while the main thread waits |
| Morph deltas | plugins that touch every package morph call `DAZMorph.LoadDeltasFromBinaryFile`: a `BinaryReader` reading 4 bytes per call over SharpZipLib's managed inflate | one frame of 5.5 s at the end of a load |
| Package lookups | SharpZipLib's `ZipFile.FindEntry` scans every entry with a culture-aware comparison, for every file opened from a `.var` | 5-6% of sampled main-thread time |
| Clear and restore | `ResetClothing` / `ResetHair` call `SyncAnatomy` after each of about 5000 items, and `SyncAnatomy` walks all of them: O(n^2) from unchanged inputs | 2.8 s of a 6.3 s clear frame |
| Dropdowns | for every atom a scene adds, every plugin rebuilds every atom dropdown, creating button objects | about 7 s in the plugin frame of the large scene (see pitfalls: partly measured through the test's own checks) |
| `Selectable.OnDisable` | UGUI removes it from a global list with `List.Remove`, which compares with `Object.Equals` (linear) over tens of thousands of entries | 8.6% of a 35.7 s frame |
| Audio | a scene plugin loads a voice-over bundle whose clips are set to `DecompressOnLoad` + preload on the main thread: Unity decodes about 4 GB of PCM in one frame | 24 s frame (scene with 371 clips, about 6 hours) |
| Collector | each collection marks the whole managed heap | 23 collections / 26.9 s in the first stage-scene load before [gc-tuning](gc.md) |
| Images | one worker, head of the queue only; the main thread finishes each image (texture, `Apply`, `Compress`, cache write, callbacks) before the next starts | see image load |

Two corrections that shape the design: the bytes of a `.var` bundle are read on a background thread and cost almost
nothing (43 MB in the first scene); the main-thread cost is Unity completing the bundle's contents synchronously (plugins
call `LoadAsset` in the callback) and, for LZMA bundles, Unity's own decompression. And the same plugin compiled twice gives
the same bytes, so the compile can be a cache hit, but Mono refuses to load two assemblies with the same name as two
instances, which dictates how the cache works (below).

## How scene-load works

Everything is patched at run time with Harmony; `Assembly-CSharp.dll` is not modified. Each path checks its preconditions
and hands the call back to VaM's own method when one does not hold, and each is counted per reason. The two plugins use
[CpuBoost](cpu.md)'s worker pool for background work (`CB_Task` / `CB_Run`), not their own threads.

### 1. Plugin compile cache (`[PluginCache]`)

`ScriptCompiler.CompileSources` / `CompileFiles` are wrapped. The key is the sources, compiler options and references, the
assembly-name prefix (`MVRPlugin_<path>_<md5 of source>`) and the names / sizes / times of the assemblies in
`VaM_Data\Managed`. A hit returns the bytes `mcs` produced earlier and VaM goes on exactly as after a compile
(`ScriptDomain.LoadAssembly`: its security check, then `AppDomain.Load`).

The catch is naming. `mcs` names an assembly `<prefix>_<counter>`; VaM's `GetPluginHash` strips the counter to know which
plugin calls it, and Mono returns an already loaded assembly of the same name instead of loading a second one, which would
make two instances of a plugin share their statics. So every load needs a fresh name of the same shape. On a miss the
compiler counter is temporarily set to a 9-digit number for that compile; on a hit those 9 digits are overwritten in place
with a new 9-digit number and the module's MVID with a new GUID (also in the debug symbols), so lengths never change. Only
VaM's own plugin compiles (prefix `MVRPlugin_`) are cached; **a failed compile is never cached** (so a plugin that always
fails to compile, such as one calling a method this VaM lacks, is recompiled every time).

Files: `BepInEx\cache\VaMSceneLoad\plugins\*.plc`, one per distinct plugin (41 files, 4.9 MB for the first stage scene),
format tag `vam-plugin-cache 2`. A changed VaM assembly or plugin source changes the key, so nothing needs deleting.

### 2. Package reads (`[PackageRead]`)

- `FastEntryLookup`: an `OrdinalIgnoreCase` index per package replaces `FindEntry`'s scan. It equals SharpZipLib's answer
  only when the wanted name and every entry name are printable ASCII (no ASCII character is ignorable and invariant case
  folding is A-Z <-> a-z there); the plugin checks that equivalence once at start (a failure turns the item off), builds
  an index only for packages whose names are all printable ASCII, and every other package or name, and the case-sensitive
  form, runs SharpZipLib's own scan.
- `FastMorphDeltas`: for a `.vmb` inside a `.var`, the native library reads the entry once and inflates it with miniz
  10.2.0, accepting exactly what SharpZipLib's `GetInputStream` accepts (local-header check including the zip64 extra
  field; what it refuses, the native reader refuses), and the plugin fills the same `DAZMorphVertex[]`. Anything else
  (loose files, unpacked packages, a refused entry, short data) runs VaM's method, which then logs as before.

### 3. Repeated work in VaM's own load path (`[LoadPaths]`)

Each item gives VaM's result; the checks are in "Verify".

- `SkipRepeatAnatomySync`: during `ResetClothing` / `ResetHair` an item marks the anatomy state dirty when its active
  flag, GameObject state or joint-adjustment flag changes; a `SyncAnatomy` with no dirty input is skipped (it only assigns
  from its inputs, so the skipped call would write the values already there). The first call and every call after a
  change run.
- `FastRemovePackageFromPath`: the two regular expressions (`.*:/`, `.*:\\`) become "take what follows the last `:/` or
  `:\`". Identical on paths without a newline; a path with a newline or null goes to VaM's method; 18 sample paths are
  checked at start.
- `FastMorphSort`: `ResyncMorphs` builds `region + ":" + displayName` for both sides of every comparison and compares
  culture-aware. Each morph's sort text is built once per sort (rebuilt when either name changed) and the result of
  comparing two texts is remembered (while the culture object is unchanged), so every comparison returns what VaM's
  returns and the order, ties included, is the same. The memo table must not use `long` keys: Mono's `long.GetHashCode` is
  the XOR of the two halves, which put `(a << 32 | b)` keys into a few thousand buckets and made the first version slower
  than VaM; it uses a multiplicative hash.
- `FastVarFileSearch`: `FindVarFilesRegex` runs a culture-aware prefix test on every package file for every search. The
  per-folder result is remembered together with the collection's modification count, the current culture, each entry and
  its path string; the next search iterates the collection again and uses the remembered result only if nothing changed.
  The regular-expression part stays.
- `FastSelectableRemove`: `List<Selectable>.Remove` becomes find-by-reference plus `RemoveAt`. At start the plugin checks
  every loaded assembly (and later ones) for a `Selectable` subtype that overrides `Equals`; if one exists the item is off.
- `MorphMetaCache`: when `DAZMorphBank.RuntimeImportMorph` reads a `.vmi`, every field `LoadMetaFromJSON` sets is
  remembered per (`.var` path, size, time, entry name) or (file, size, time) and set straight on the next import. Not used
  when the formula table is not the freshly created kind. File `BepInEx\cache\VaMSceneLoad\morphmeta.bin`, read and
  written on a background thread (new entries are written once, 5 s after the last).

### 4. Atoms in batches, deferred dropdown buttons (`[LoadPaths]`, `LoadFrames.cs`)

`BatchAtomCreation`: a transpiler on `LoadCo` replaces `AddAtomByType` with `AtomFast`. When the atom's type has a pooled
instance, or its prefab is already in VaM's own cache (a bundle prefab also needs `AssetManagerReady`), the atom is made
right away with VaM's steps (async flag, loading icon, `lastAddedAtom`, the pool, the same `AddAtom` arguments, the
bundle flag) and the coroutine does not yield. Otherwise (the first atom of a type, whose prefab loads asynchronously), or
once `AtomFrameBudgetMs` (100) has been spent in the frame on atoms, the call goes back to VaM's coroutine step. Order,
calls and arguments are VaM's.

`DeferPopupButtons`: a closed dropdown (not `alwaysOpen`, not visible, panel inactive) that receives new choices does not
create its button objects. Everything else `CreatePanelButtons` does is set: the new array with empty slots, the current
button index, the list height from the button prefab's `sizeDelta` (anchors must be the same height), and
`HighlightCurrentValue`'s scroll position and colour. Before anything needs the buttons (open, filter sync, highlight,
`MakeVisible`, clearing the highlight) VaM's own method builds them; `SetPanelButtonText` is skipped while unbuilt (they
are built later from the then-current options); `ClearPanel` and `Start` drop the record.

### 5. Asset bundles from memory (`[BundleLoad]`, `BundleLoads.cs`, `native/bundlecache.cpp`)

When a request is queued, a background CpuBoost task ("scene load bundles", priority 0, one chunk per bundle) reads the
`.var` entry into native memory. An LZMA bundle (UnityFS 6, each block LZMA or stored) is rewritten as the same archive
with every block stored: header strings unchanged, flags 0x43, blocks info = the original hash plus one stored block (flag
0x40) per original block plus the original directory, the blocks info itself LZ4-compressed the way Unity's own
uncompressed bundles are (it must come out **smaller** than the info, otherwise it is written as is). Other bundles are
the entry's bytes unchanged. When VaM's queue gets to the request (reference counts, tables, error text and callbacks are
VaM's), the plugin calls `AssetBundle.LoadFromStreamAsync(stream, 0, 1 MB)` whose `Read` copies from the native memory;
the memory is freed with the bundle (a reaper runs once a second). A request whose job is not ready waits frame by frame.

`SameFrame`: the queue coroutine (taken over at `MoveNext`) hands over several requests per frame, in VaM's order, until
`FrameBudgetMs` (100) has been spent on hand-overs; ready bundles among the next 32 requests are opened ahead, except the
one being loaded. If Unity refuses a rewritten bundle, that request runs VaM's original path on the spot; after a
second refusal the run uses the original path for all bundles.

Nothing is written to disk (an earlier version wrote files; a user objected to the wear and the files saved only a second or
two, since preparation is not on the critical path). The memory cost is the whole point of being fast: bundles stay in memory
while loaded (see Measured).

### 6. Audio decoded in parallel (`[Audio]`, `native/vorbisdec.cpp`)

The bundle's `DecompressOnLoad` + Vorbis clips are changed to point at a PCM16 FSB5: the serialized file's `m_Resource`
offset and size and `m_CompressionFormat` (PCM) are rewritten, and Unity / FMOD load it as before, now receiving PCM with
no Vorbis to decode on the main thread. The decoding happens when the first clip of a bundle is read: all the bundle's
clips are decoded in parallel on CpuBoost workers, one clip per chunk; a clip not yet decoded when read is decoded by the
reading thread. Decoded buffers stay 2 s after being read (Unity usually reads a clip twice in a row), a bundle nobody reads
for 10 s drops its unread decoded clips, and background decoding waits while unread decoded data exceeds `DecodeAheadMB`.

Why the format is right: FMOD stores `DecompressOnLoad` Vorbis as PCM16, with rounding `(int)(x * 32767)` truncated toward
zero after clamping x to +-1 (a clipped sample is -32767, not -32768); 1310 clips of two scenes read back from the game were
all k/32768. The decoder is libvorbis 1.3.7 (`native/third_party`) with that rounding. An FSB's Vorbis stream carries no setup
header, so it is regenerated from the encoder parameters: the table `native/vorbis_setup_index.h` (458 entries keyed by the
CRC32 of the setup packet) was generated with libvorbis's own encoder (`native/tools/vorbis_setups.c`, `setup_index.py`).
A clip whose CRC is not in the table keeps its Vorbis and loads as before (in the large scene, 33 of 35 CRCs were covered;
3 clips were left as Vorbis).

FMOD refuses a resource node larger than about 4 GB: every clip then fails with "Unsupported file or audio format". The
rebuilt bundle therefore puts at most 1 GB of PCM into a node and starts a further node with a name of the same length as
the first (one character replaced by a non-hexadecimal letter) and `m_Source` pointing at it.

### 7. Scene switches and memory (`SceneSwitch.cs`, `native/memstate.cpp`)

Two heavy scenes could not be switched between in one VaM. Three separate causes, fixed under `[SceneSwitch]`:

1. A starting atom (the window camera) had been parented inside a sub-scene; the clear destroyed it with the sub-scene, but
   it stayed in `startingAtoms`, and every later `LoadInternal` threw "Destination array was not long enough". Now a
   starting atom in a removed sub-scene is only unparented.
2. A plugin's atom-removed callback threw (a null reference in an animation plugin) out of `RemoveAtom`, which does not
   catch it, so the clear stopped half way. Each handler now runs on its own and an exception is logged in VaM's format.
3. The bundle queue taken over in 5 runs every request's callback in one coroutine; a throwing callback made Unity end the
   coroutine and every later request waited forever. A throwing callback is now logged and the queue goes on (and the
   coroutine restarts if it ever throws itself). The trigger was a plugin that loads its own bundle and nobody releases,
   so the next scene's different version of the same content was refused by Unity ("another AssetBundle with the same
   files is already loaded").

Plus `KeepClearing` removes from `startingAtoms` any that are no longer in the scene and puts back live ones missing from the atom
table, before a load. `ReleaseOrphanBundles`: one frame after a clear, bundles whose count is still up but whose requesters
(the callback's target, read from the closure's captured Unity object) are all destroyed are released through VaM's own
`DoneWithAssetBundleFromFile`; a bundle whose requester cannot be identified is never touched.

`KeepForNextScene` / `KeepSeconds` (60): when the last user of a bundle lets go, the bundle is kept if the scene being loaded
names it (original path, `latest:/` form, or `SELF:/` when the scene is inside the package), or for 60 s outside a switch
(for example a CUA changing its asset); a request in that time reuses it (VaM already reuses loaded bundles). Before the
new scene loads anything, kept bundles it does not name are unloaded (so a same-content different version cannot clash); if
Unity still refuses a bundle while some are kept, all kept ones are unloaded and the load repeats VaM's way.

`UnloadInactiveAtVideoMemoryPercent` (85): once a second the process's video memory is read (DXGI
`QueryVideoMemoryInfo`); at that percent of the budget, with no scene loading, VaM's own `UnloadInactiveCharacters /
ClothingItems / HairItems` run on the people in the scene (appearance changes leave the old instances loaded until the next
scene switch), then not again for 30 s. `Resources.UnloadUnusedAssets` is not called (it freed 0.02 GB for a 2.6 s stall).
Pooled, inactive people are not touched.

`[TextureCache] MaxGB` (0 = no cap, the default: nothing is deleted): the plugin records the last time each file of VaM's
own texture cache (`Cache\Textures`, raw pixels plus a meta file) was read or written
(`BepInEx\cache\VaMSceneLoad\texturecache-use.txt`; file access times are unreliable). With a cap set, 30 s after start and
10 s after each load, in the background, it deletes the least recently used files until the folder is at 90% of the cap
(data first, then meta, since VaM reads a meta only when the data exists; orphan metas too; files used this run are never
deleted; files with no record count by modification time). **A cap deletes the user's regenerable cache**: with 40 GB, the
value the development machine runs, the one real-cache dry run would have deleted 4670 of 7035 files (54.8 GB, 90.8 ->
36.0 GB), and any texture VaM needs again is regenerated and re-cached at the price of a cold image load. That is why the
published default is off: set a cap only on the user's word, and tell them how much goes first.

## How image-load works

The VaM queue (`ImageLoaderThreaded`, covering character and clothing textures and thumbnails, not the synchronous
`ProcessImageImmediate` and not textures inside asset bundles): one worker, head only, the main thread finishes each image
before the next. All requests of a character file are in the queue from the start (the JSON restore is a synchronous
loop), so requests need not wait for each other; some derived textures do depend on the previous image. It was done in
six steps; the plugin keeps every step as a setting. Decoding in the native library uses GDI+, the decoder VaM's own
`Bitmap` path uses, and the cache is VaM's own texture cache (`Cache\Textures`): the same files, the same key
(path + compress / linear / normal flags).

1. **Same-frame memory hits** (`SameFrameMemoryHits`): consecutive completed memory-cache hits at the head are finished in
   one frame, up to 32 items and a 5 ms check between images (a single finish can exceed it).
2. **Native cache reads, parallel pixels** (`AsyncPreparation`, `ParallelPixels`): disk-cache reads run as CpuBoost native
   jobs (at most 4 at a time, files up to 128 MB, 512 MB reserved); the channel, invert and grayscale-to-alpha pixel
   transforms run in parallel; prepared images are finished back to back. Non-seekable streams and bump-to-normal go back
   to VaM's path.
3. **Parallel BC compression** (`ParallelCompression`): after Unity generated the mip chain, BC1 / BC3 compression runs on
   CpuBoost workers with the native encoder (stb_dxt, high quality), replacing the main-thread `Compress`.
4. **Cross-image pipeline** (`CrossImagePipeline`, needs 2 and 3): a separate source image is read and decoded natively, and
   its compression runs asynchronously, while the main thread finishes earlier images in VaM's queue order. In flight are up
   to `CrossMax` = 6 images (320 MB reserved each); source files over 32 MB or decoded images over 128 MB, thumbnails,
   forced reloads, web images, bump-to-normal and images already in the disk cache are left to VaM. Eligible compressed
   textures need a width and height that are multiples of 4, and with mips a power-of-two size; others are not compressed
   by this plugin (VaM compresses them in its cache as before).
5. (six slots instead of three: the same code, `CrossMax`.)
6. **Head wait, native mips, background cache write** (`HeadWaitMs`, `NativeMipmaps`, `AsyncCacheWrite`):
   - The head of the queue used to wait 30 ms for its native decode and then be decoded again by VaM, wasting the
     background work (19 / 27 fallbacks per cold load); it now waits up to `HeadWaitMs` (5000), on VaM's image thread, not
     the main thread.
   - The decode result gets its mip chain and BC compression natively on the coordinating thread, so the main thread no
     longer calls Unity's mip generation, and copies only the final BC bytes. The mip filter reproduces Unity 2018.1's CPU
     chain, fitted from dumps of `Texture2D.Apply(true)` (RGB24 / RGBA32, sRGB and linear alike): the two pixels of a row
     are averaged with truncation, then the two rows with truncation, `(((a+b)>>1) + ((c+d)>>1)) >> 1`; a dimension of 1
     only does the other direction, `(a+b)>>1`. A plain rounded or truncated 2x2 average is wrong (69% / 15% of bytes differ).
   - Textures this plugin compressed are written to the cache by a below-normal thread, reusing the BC bytes already
     in the texture: to `<name>.ilptmp` first, then renamed in VaM's order (meta first, data second); leftover `.ilptmp` are
     deleted at start, and at quit the thread gets up to 15 s to finish. VaM's own cache write (images this plugin did not
     compress) is untouched.

## Where the result differs from VaM's

- **Decoded audio**: about one value in 5000 differs by 1 step from FMOD's own decode (floating-point order differs),
  at samples within 0.003 of an integer boundary. Of 774 / 536 clips in the two scenes, 344 / 339 were value-for-value
  identical to the game's own decode (those left as Vorbis or not in a bundle) and the others matched in the first and last
  8 values and the extremes within 1. Clips whose setup CRC is not in the table keep Vorbis. Accepted by the user.
- **Compressed textures** from the native encoder are not byte-identical with Unity's `Compress`; the pixel pipeline before
  it (decode, channel transforms, mips) is. The cache holds our bytes for those textures. Peak committed memory rose by
  about 3 GiB (33.3 -> 36.3 GiB) in the step that added compression.
- **Everything else** is checked to give VaM's values: scene snapshots (atoms, every CUA loader field, the bundle tables)
  were identical line by line; the sorted morph lists, package searches, path strings, anatomy state, dropdown state and
  morph metadata were compared item by item with VaM's own methods in the game (0 differences in the runs listed below).
- The dropdown items, the batch atoms and the bundle reads change **when** things happen (a frame earlier, or buttons built
  later), not what ends up there.

## Settings

Scene-load: `BepInEx/config/local.vam.sceneload.cfg`. All read at start only; all defaults below are the code's. A change
to a path turns off only that path.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `PluginCache` | `Enabled` | true | reuse compiled plugin assemblies |
| `PackageRead` | `FastEntryLookup` | true | index instead of SharpZipLib's scan |
| `PackageRead` | `FastMorphDeltas` | true | native `.vmb` read from a `.var` |
| `LoadPaths` | `SkipRepeatAnatomySync` | true | skip `SyncAnatomy` with unchanged inputs |
| `LoadPaths` | `FastRemovePackageFromPath` | true | `RemovePackageFromPath` without regex |
| `LoadPaths` | `FastMorphSort` | true | morph list sort with remembered comparisons |
| `LoadPaths` | `FastVarFileSearch` | true | remembered per-folder prefix results |
| `LoadPaths` | `BatchAtomCreation` | true | atoms made without a frame each |
| `LoadPaths` | `AtomFrameBudgetMs` | 100 | ms of atom creation per frame, the rest back to VaM |
| `LoadPaths` | `DeferPopupButtons` | true | closed dropdowns do not build buttons |
| `LoadPaths` | `FastSelectableRemove` | true | remove by reference |
| `LoadPaths` | `MorphMetaCache` | true | `.vmi` metadata kept (`morphmeta.bin`) |
| `BundleLoad` | `InMemory` | true | prepare bundles in memory, read through a stream |
| `BundleLoad` | `SameFrame` | true | hand over several queued bundles per frame |
| `BundleLoad` | `FrameBudgetMs` | 100 | ms of hand-overs per frame |
| `BundleLoad` | `KeepForNextScene` | true | keep a released bundle the next scene names |
| `BundleLoad` | `KeepSeconds` | 60 | how long outside a switch |
| `Audio` | `ParallelDecode` | true | PCM16 from parallel decode (needs `InMemory`) |
| `Audio` | `DecodeAheadMB` | 4096 | unread decoded data at which decoding waits |
| `SceneSwitch` | `KeepClearing` | true | the three scene-switch fixes and the starting-atom check |
| `SceneSwitch` | `ReleaseOrphanBundles` | true | release bundles whose requesters are all destroyed |
| `Memory` | `UnloadInactiveAtVideoMemoryPercent` | 85 | 0 = never |
| `TextureCache` | `MaxGB` | 0 | cap of `Cache\Textures`, GB of 2^30 bytes; 0 = no cap, nothing deleted (the development machine runs 40) |

Image-load: `BepInEx/config/local.vam.imageloadpipeline.cfg`, section `[ImageLoad]`. **The code's defaults are all "off"
(the first four keys false, `HeadWaitMs` 30 = the previous behaviour); the development install writes the file with the
keys below set as shown.** A fresh install that does not write them gets no speed-up. Dependencies: `CrossImagePipeline`
needs `AsyncPreparation` and `ParallelCompression`; `NativeMipmaps` takes effect through `ParallelCompression`'s setup.

| Key | Code default | Development install | Meaning |
|---|---|---|---|
| `SameFrameMemoryHits` | false | true | step 1 |
| `AsyncPreparation` | false | true | step 2, native cache reads |
| `ParallelPixels` | false | true | step 2, parallel pixel transforms |
| `ParallelCompression` | false | true | step 3 |
| `CrossImagePipeline` | false | true | steps 4-5, in flight 6 (code constant, not a setting) |
| `NativeMipmaps` | false | true | step 6, native mips and BC |
| `AsyncCacheWrite` | false | true | step 6, background cache write |
| `HeadWaitMs` | 30 | 5000 | head waits this long for its native decode |

Test-only keys (`[ImageLoadTest]`): `ForceCold` (disable the disk-cache reads and writes), `MipMode` (2 = Unity's filter, 0 / 1
plain boxes for the offline comparison), `StageProfile`. Do not set them in a play install. Further
tuning constants (`CrossMax`, the 5 ms / 32 check, the 1 GB node limit, the 2 s and 10 s audio holds, the 60 s keep) are
constants in code.

CPU: both use CpuBoost's workers (`Threads`, `WorkerCpus` in its own cfg); neither sets thread affinity. In a measured
load, image work used about 0.6 cores on average: the limit was the serial chain, not compute.

## Measured

Development machine: 16-core X3D CPU, RTX 5090-class GPU, 125.7 GiB RAM; desktop mode, path tracing on, the user's desktop
settings; each cell is **one run** unless stated, and the baseline of one step is not the same run as the baseline of the
next. "Fresh VaM" = the first load after the process started. Times are start-to-end of the load, "LoadCo" = the load
coroutine without the clear frame. The run-to-run spread was not measured except where two numbers are given. Nothing was
tried in a headset.

### Scene load

*Stage scene A* (106 atoms, 137 plugin instances / 43 plugins), the plugin and package steps:

| Change | Condition | Before | After |
|---|---|---|---|
| Plugin cache (1.0.0) | LoadCo, VaM reloading the scene, disk cache warm | 66.1 s | 43.0 s |
| Plugin cache | LoadCo, fresh VaM, cache empty (94 of 135 compiles already hit: same plugin on several atoms) | 80.3 s | 43.6 s |
| Plugin cache | plugin phase, same two runs | 30.5 / 25.9 s | 13.3 / 6.4 s |
| Package reads (1.1.0) | LoadCo, reload | 31.2 s | 25.6 / 27.3 s |
| Package reads | last frame of the load | 5.5-5.7 s | 1.5-1.65 s |
| Repeated work (1.2.0) | fresh VaM | 40.3 s | 34.9 s |
| Repeated work | reload, clear frame | 32.2 s (5.9 s) | 28.5 s (2.7 s) |

Counts in the 1.2.0 runs: 32 anatomy syncs run and 30484 skipped in a fresh load (30516 run without it); morph sort 22
sorts, 4.14 million comparisons, 82% answered from memory.

*Stage scene B* (821 atoms, 33 sub-scenes, 322 CUAs, 197 bundles, one person), start to ready, fresh VaM:

| Version | Time | Note |
|---|---|---|
| original VaM | 166.8 s (LoadCo 158.0) | one run |
| 1.3.0 | 157.6 s | Unity refused all rewritten bundles (see pitfalls), so a fallback to VaM's path; 191 read errors |
| 1.3.1 (bundle files on disk) | 95.3 s (cache empty) / 94.6 s (ready) | all 197 opened from files |
| 1.4.0 (and a cloth-shading change in the same run) | 67.4 s (from 86.3 s in a 1.3.1 run with the same sampling on) | the two effects are not separable; atom-creation segment 34.5 -> 18.6 s |
| 1.4.1 | 52.0 s (ready 56.8 s) | test checks sampled |
| 1.5.0 | 47.9 s (ready 53.1 s), from 51.7 s | bundles in memory |

*Stage scene C* (399 atoms of which 108 active, two people, 289 plugin instances / 64 plugins, 93 CUAs in 48 bundles, 3
sub-scenes), fresh VaM, original vs 1.4.1: load to end of LoadCo 151.4 -> 43.0 s (atoms 26.7 -> 12.6 s, plugin frame 63.2 ->
16.4 s, bundle phase 63.3 -> 13.7 s, collections 52 / 16.3 s -> 9 / 1.8 s with gc-tuning on in both); the scene snapshot
matched line by line, apart from one sound-effect bundle's reference count (8 vs 7, a request still queued at the moment of
the snapshot). 1.5.0, same scene: LoadCo 42.8 -> 37.2 s; the voice-over frame after the load 24.1 -> 4.9 s; usable
(all clips loaded) about 68 -> about 43 s (the test's "ready" at 46.8 s). The usable time with VaM's path was about 183 s.

Memory with 1.5.0: bundles held in memory 4.0 GB (scene C) and 6.36 GB (scene B), for as long as the scene is loaded;
decoded audio peaks at 3.9 GB / 0.45 GB for a few seconds; process private memory peaked at 40.4 / 38.6 GB (1.4.1 was not
measured, so no comparison; VaM's own path also loads bundles from memory, its footprint was not measured).

*1.6.0* (one VaM, scene C, scene B, then a third scene in sequence; one run): fresh load 46.5 s (1.5.0: 45.3-57.8 s over three runs,
the variation being collections, see [gc](gc.md)); same VaM reloading scene C 32.8 s (1.5.0: 38.4 s) with 77 bundles kept and
all 77 reused, the voice-over frame gone; scene C -> B 53.5 s and B -> third scene 18.3 s (with 1.5.0 the first could not be loaded
and the second broke off during the clear). Orphan bundles released: 5-9 per clear. Unloading 34 inactive instances took
7 ms and lowered video memory 20.6 -> 17.6 GB, textures 7.26 -> 4.62 GB, private memory 47.4 -> 41.7 GB (forced with a 1%
threshold; the natural 85% trigger was never reached in a test). Texture cache: in a temporary folder the pruner removed
what the rule says; on the real cache the dry run counted 4670 of 7035 files, 90.8 -> 36.0 GB.

What stays after a scene is cleared, measured: about 10 GB of private memory (managed heap 8.8-9.1 GB which Boehm never
returns, with 4.1-4.6 GB of it free for the next scene; Unity 2.6 GB; textures 0.9; audio 1.0; plugin assemblies, which are
never unloaded, +306 per load of scene C; two pooled people). It is reused by the next scene, not a leak. Video memory
(budget about 31.4 GB) is the tight one: a heavy scene committed about 51 GB at peak, then a change of appearance added
7-8 GB private and 3.8 GB video memory that stay until the next scene switch.

### Image load

Each cell one run; "cold" = an empty cache folder (so the cache is also written), "warm" = reading it back; two scenes,
S (62 textures in the scene, 112 after the second load) and P (a person added). The cache folder was redirected by the test
plugin `components/runtime/image-load/tests/CacheDirProbe.cs`, not the user's.

| Step | Scene S | Scene S + person P | Note |
|---|---|---|---|
| 1 same-frame hits | warm 15.47 -> about 14.9 s | warm 5.67 -> 4.87 s | |
| 2 native reads, parallel pixels | warm 14.88 -> 14.56 s | warm 4.89 -> 3.49 s | |
| 3 parallel compression | 33.2 -> 30.6 s | 29.5 -> 20.6 s | commit peak +3 GiB |
| 4 cross pipeline (3 in flight) | 30.6 -> 26.9 s | 20.6 -> 15.8 s | |
| 5 six in flight | 27.4 -> 26.9 s | 16.9 -> 14.8 s | |
| 6 head wait, native mips, async write | 21.91 s (no aligned baseline) | 16.69 -> 13.69 s (and 14.94 with the first native mip mode) | |

Step 6 details: fallbacks to VaM's decode fell from 19 / 27 to 2 / 4 per cold load; the head waited 20 / 32 times, at most
0.3-0.38 s. In the step-6 comparison the old build finished with 50 / 100 textures in the cache against 62 / 112, so its
times are not comparable (and its warm run re-decoded 12 textures it had not written); the safe statements are "a scene with one added person: 16.7 -> 13.7-14.9 s cold, fewer fallbacks, no mip generation or cache write on the main thread" and "no
change warm". Cache writes in the test: 69 / 103 files (574 / 1064 MiB), 0 failures, none left over, writer idle 0.03 s after
the load ended. Main-thread cost of the plugin in a CPU census: 0.005 / 0.004 ms per frame. Per-step baselines are not the
same run; CPU parallel work cuts the load time, not the in-play frame rate.

A first candidate (two images in parallel on CpuBoost) did not win (15.5 -> 16.6 s) because of its own two-slot refill and
synchronous batch waits; it is not evidence against other parallel designs.

## Pitfalls

- **Unity refuses every rewritten bundle** ("Unable to read header from archive file") -> the blocks info is not LZ4
  compressed to something smaller than the original (a literals-only frame is larger: 219 -> 221 bytes). Compare against
  one of Unity's own uncompressed bundles field by field. Without the fix the result is correct and merely not faster.
- **A bundle opened twice returns empty / "Not valid"** -> Unity cannot load the same bundle twice. A request waiting for its
  own file must not let a later request for the same bundle open it ahead; the plugin skips the bundle being loaded and
  checks whether it was opened ahead after waiting.
- **Rewritten jobs run one after another** -> a CpuBoost `CB_Run` chunk pulls tasks until the queue is empty; if tasks are
  submitted one by one the other chunks have already exited. The driver hands every queued job to `CB_Run` per round, one chunk
  per job (offline: 72.1 s -> 16.5 s for 184 bundles; the rest waits on the largest bundle, 742 MB expanded).
- **Every clip of a voice-over bundle fails in FMOD** ("Cannot create FMOD::Sound instance for resource archive ...
  Unsupported file or audio format") -> a resource node over about 4 GB (0.5 GB and under were fine). Keep PCM16 nodes at
  1 GB or less.
- **Decoding runs twice** -> Unity reads most clips twice in a row; releasing a decoded buffer after one read re-decoded 331
  of 440 clips on the reading thread (15.5 s of reads). Hold 2 s.
- **A quadratic sort after the speed-up** -> `Dictionary<long, ...>` on Mono: see FastMorphSort above.
- **A test check slows the thing it checks** -> the rule "check the first dropdown of each button prefab" rebuilt every
  dropdown with VaM's method (each has its own button template), hiding the saving and producing a false cause for a
  while. Sample by count (the first 20, then every n-th) and count fast-path refusals per reason.
- **Material read errors during load** ("Material doesn't have a texture property '_AlphaTex' / '_DetailMap'", about 1100
  a load, then every frame) -> VaM swaps a person's material for a compute-buffer shader while keeping the old
  properties; `HasProperty` says yes, `GetTexture` errors. Read only properties the current shader declares (fixed in
  the skin plugin's scene reader). A separate family, "`_AlphaAdjust` float" (46 per load), has an unknown source.
- **Reloading the same stage scene fails** in VaM and the plugin alike (an exception while clearing, a copy to a too-short
  array); 1.6.0's scene-switch fixes address the starting-atom case. Unknown whether stock VaM would too.
- **The 24 s voice-over frame after the load** was not caused by this plugin; it is the author's `DecompressOnLoad` +
  preload clips decoded on the main thread. Only the decode is faster now (4.9 s remains: Unity reading 4 GB of PCM,
  FMOD creating 371 samples). Do **not** move the load to the background: clips must be usable when `LoadAsset` returns;
  a user ruled that out.
- **An image-load comparison that looks wrong**: do not compare runs that finished with different numbers of cached
  textures (50 vs 62); do not infer bindings from the cache's file count (compare material texture bindings); do not
  decide that a load has ended after 2 s of queue idleness; the system file cache makes the first run cold and the next
  warm, so say which; the Finish probe is not valid. A test run was occasionally terminated by the harness 30 s after quit,
  cause not found.

## Tried and dropped

- Holding back the unload on a person switch (`UnloadUnusedAssets` + `GC.Collect`) until after the load: never triggered
  in the first stage scene (people there have unload-on-switch off).
- `Application.backgroundLoadingPriority = High`: 38.0 s vs 38.1 s.
- Bundle files on disk (1.3.x-1.4.x): replaced by memory; the saving from keeping prepared files across runs is a second or
  two because preparation is off the critical path.
- Loading voice-over in the background or not preloading it: ruled out by the user (clips must be usable at once).
- Pushing new dropdowns' deferral to before `Start`: written, measured (0 cases), deleted.
- Unloading unused assets after each appearance change: only 0.02 GB for 2.6 s.
- A first image-load candidate running two images in parallel (above).
- Not done: a faster reference scan inside `Resources.UnloadUnusedAssets` (Mono's single-threaded `mono_unity_liveness_*`
  walk, about 1.65 s per load; an exact native copy is possible, a mistake crashes there, estimated 0.8-1.3 s saved);
  sharing the tail of the atom-UID-rename delegate chain (about 1 s, saves half); caching `isHidden`'s `File.Exists`
  (about 0.85 s; cannot be identical if an external process creates the file within the frame); the third-party audio
  loads and CUA instantiation (engine native); caching failed plugin compiles (about 0.6 s per load for one failing plugin).

## Open

- The plugin frame (restoring plugins: their own `Init` and UI building, about 8-9 s even with all caches; about 0.9 s of that is
  JIT because every load is a new assembly) has no single hot spot. The main thread runs at about 90% of one core in a reload
  and most of the rest is Unity instantiation and UI building on that thread, which cannot be split.
- The first load's collection count varied between 7 and 54 per run, 10 s apart. Cause found later: the finalizer-count chain in Mono's
  `GC_collect_or_expand`, fixed in [gc](gc.md) (section 4; the figures here for 1.5.0 and earlier include the variation).
- Main scene's bundle phase had 9-10 collections in all three 1.5.0 runs against 3 in the 1.4.1 run (about 2 s), cause not found.
- `DecompressOnLoad` clips that are loose `.wav` / `.mp3` files, loaded by VaM itself (about half of that scene's clips), were not examined.
- Image load: prefetching decodes at load start from the paths in the scene / appearance JSON, in parallel with atom
  creation and plugin init (expected to be the biggest remaining gain; first measure at what second the first image is
  queued); a faster decoder (libjpeg-turbo / libspng: PNG could be byte-identical, JPEG and GDI+ would differ slightly, the
  user must decide); with a texture cache cap set close to the cache's size (40 GB against 38 GB on the development
  machine), eviction will cause extra cold loads.
- Not measured: a headset, the user's real texture cache with the new image writer, natural video-memory pressure at 85%, a CUA
  changing its asset and returning within 60 s, warm-cache gains from step 3 on.

## Verify

Offline (no VaM), in `components/runtime/scene-load/tools` and `native/tools`:

- `bundle_check.py <dll> <workdir> <urls.txt | bundles.json>`: for every bundle a scene loads, the native job builds the file
  and the script compares it with the original decoded by Python (liblzma): header strings, size field, flags, a blocks info
  smaller than the original's, the same hash and directory bytes, data equal to the original blocks decoded; bundles left
  as they are equal the entry's bytes (184 + 9 bundles passed). It is written for 1.3.x's files.
- `mem_bundle_check.py`: the same for the in-memory form through `SL_BundleSubmit2` / `SL_BundleRead`: the rebuilt header and
  directory, every node equal to the original except the rewritten clip fields, the new nodes' limits (no node over 1 GB of PCM16), every PCM16
  clip's samples equal the native decode of the original Vorbis, a clip read twice gives the same bytes. Against the game's own decode
  (`components/runtime/scene-load/tests/AudioTruthProbe.cs` output) the first / last 8 values and extremes must be within 1.
- `driver_check.py <VaMSceneLoadNative.bin> <outdir>`: the job driver with CpuBoost's 8 workers; wall time against the sum of the
  jobs' times shows how many ran at once.
- `native/tools/fsbdec_test.cpp` (`build-fsbdec-test.cmd`): decodes every clip of an extracted-FSB index with each float-to-int16
  variant and prints hashes and boundary values comparable with `AudioTruthProbe`'s; `setup_index.py` regenerates
  `vorbis_setup_index.h`.
- Image load: the in-plugin `ImageLoadPipeline.CrossSelfCheck` (`components/runtime/image-load/CrossSelfCheck.cs`) compares the
  native decode with VaM's own for 32 flag combinations of a generated image with and without alpha and two real files
  (0 failures expected). The mip filter was fitted from dumps of Unity's `Texture2D.Apply(true)` chains and checked in the game; the dump and fitting scripts are not in this repository, so to re-check the filter on another Unity
  build, dump chains for random and ramp inputs of 256x256 and 16x16 in RGB24 and RGBA32, and compare `native/mipwork.cpp`
  (`MipMode` 2) byte for byte (the original check: 55.48 million bytes, 0 differ).

In the game (test agent, see `tools/lab/README.md`): the plugin exposes `Snapshot`, `Stats`, `BundleStats`, `AudioStats`,
`LoadFramesStats`, `SwitchStats` and one `Set...TestOff` per path to turn a path off in the same VaM for an A/B run, and the
comparison methods `VerifyLookup` (index against SharpZipLib's scan), `VerifyDeltas` (native `.vmb` read against VaM's, bit for
bit), `VerifyAnatomy`, `VerifyMorphSort`, `VerifyVarFind`, `VerifyPackagePath`, `VerifyMorphMeta`, `CheckDeferredPopups`.
`tests/MemProbe.cs` (memory snapshots, appearance changes, timed collections, scene switches) and `tests/BundleSceneProbe.cs`
(atoms, every CUA loader field, the bundle tables, hashed) give the scene snapshot used to prove "same result". Order of a proper
check: load the same scene with a path off and on in separate fresh VaMs (first load) and one VaM (reload); compare the snapshot
line by line, the error counts by step (the second load of a stage scene produced about 300 errors in both modes, from the
scene plugins), and the NGX state; then the timing.

When you change a path, keep it conditional (every precondition falls back to VaM's method), count each fallback by reason,
and compare against VaM's own method in the same VaM before trusting a time.

Load timing: start the clock at the load call, not the first coroutine step; "ready" must include the long frames after
the load (voice-over); do not trust the 2 s queue-idle heuristic. Probes that hook methods do not see plugin coroutines;
for a long frame with no owner, sample the main thread from another thread and read the RIP's module for native frames (a frame-pointer
chain through native frames reads stale return addresses).
