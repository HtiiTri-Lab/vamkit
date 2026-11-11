# image-load

VaM loads images through one worker thread: it reads and decodes one image, the main thread finishes it, and only then
does the next start. This plugin lets several images be prepared at once on cpu-boost's workers, decodes and builds
mipmaps natively, compresses to BC1/BC3 in parallel, and writes VaM's disk cache of the compressed textures in the
background. Background and measurements: [../../../docs/knowledge/loading.md](../../../docs/knowledge/loading.md).

BepInEx plugin `local.vam.imageloadpipeline` 0.1.0 (the version was not raised across its seven steps). Depends on
cpu-boost (workers) and scene-load (its native library reads the image files); without either, the stages that need
them stay off.

## Files

| File | Holds |
|---|---|
| `ImageLoadPipeline.cs` | Plugin entry; same-frame handling of consecutive memory-cache hits |
| `ImageLoadAsync.cs` | Disk-cache images prepared with native jobs |
| `ImageLoadPixels.cs` | Channel / invert / alpha pixel steps in parallel, with a self check |
| `ImageLoadCompression.cs` | BC1/BC3 compression of mipmaps on the workers |
| `CrossPipeline.cs`, `CrossSelfCheck.cs` | Source images decoded and compressed asynchronously across images; self check |
| `OffMain.cs` | Native mipmaps (Unity's own filter), background cache writes, how long the queue head waits |
| `StageProfile.cs` | Test-only stage timings |
| `native/` | `imagework.cpp`, `sourcework.cpp`, `mipwork.cpp`, `build.cmd`; `third_party/stb/stb_dxt.h` (public domain) |
| `tests/CacheDirProbe.cs` | Probe plugin: points VaM's texture cache at an empty folder for a test run |

## Build

`python -I tools/vamkit build image-load` writes `out\image-load\`: native `native\build.cmd` (`VAMKIT_VCVARS`) gives
`ImageLoadWorkNative.dll` copied to `.bin`; `ImageLoadPipeline.dll` is built from the `[component.managed]` recipe.

## Settings

`BepInEx/config/local.vam.imageloadpipeline.cfg`. **Every `[ImageLoad]` stage defaults to off in the code**, so
`vamkit install` writes the values below (`enable` in the manifest); change them in the cfg afterwards.

| Key | Code default | Installed | Meaning |
|---|---|---|---|
| `SameFrameMemoryHits` | `false` | `true` | Consecutive memory-cache hits handled in one frame |
| `AsyncPreparation` | `false` | `true` | Disk-cache images prepared with native jobs |
| `ParallelPixels` | `false` | `true` | Channel / invert / alpha steps in parallel |
| `ParallelCompression` | `false` | `true` | BC1/BC3 compression of Unity-generated mipmaps on the workers |
| `CrossImagePipeline` | `false` | `true` | Source images prepared asynchronously ahead of the queue |
| `NativeMipmaps` | `false` | `true` | Mipmaps built natively (same filter as Unity), compressed without main-thread Unity work |
| `AsyncCacheWrite` | `false` | `true` | Cache of our compressed textures written on a background thread (temporary file, then rename) |
| `HeadWaitMs` | `30` | `5000` | How long the queue head waits for its native decode before VaM decodes it again |

`[ImageLoadTest]` (`MipMode` 2 = Unity's filter, `ForceCold`, `StageProfile`) is for tests only.

## What it touches on disk

VaM's texture cache (`Cache\Textures`) gets files for textures we compressed; they are written as a temporary file and
renamed, metadata first. The cache's content is what VaM would have produced from the same images (the compression is
ours, so the bytes are not identical to Unity's). Deleting the cache is always safe. Cache size is capped by
scene-load's `[TextureCache] MaxGB`, not here.

## Tests

None in the manifest: the checks need images from the user's packages and a running game. The plugin has self checks
(`CrossSelfCheck`, a pixel check in `ImageLoadPixels.cs`) that compare the native result with VaM's own decode on generated images
and, when you set `VAMKIT_IMAGE_TEST_PATH` (one VaM path of a jpg) or `VAMKIT_IMAGE_TEST_PATHS` (several, `;`
separated), on your own images. `tests/CacheDirProbe.cs` is a probe plugin (reads `BepInEx\config\cachedirprobe.txt`).

## When it meets the unknown

A stage whose VaM method is not found, or whose native library or cpu-boost / scene-load is missing, stays off and
logs it. An image the native decoder does not take goes through VaM's own decoder, also when the queue head has waited
`HeadWaitMs`. A write to the cache that fails is counted and the texture still loads.

## What it does not cover

Unity 2018.1's mip filter is reproduced for the formats VaM loads (checked bit for bit on a game export); other
formats, or a Unity build with another filter, are not covered. Compressed output is not byte-identical to Unity's own
compressor. Peak commit memory rises with several images in flight.
