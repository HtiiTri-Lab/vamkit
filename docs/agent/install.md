# Installing

For an agent installing this project into someone's VaM. Read [../protocol/changes.md](../protocol/changes.md) first:
the VaM folder is the user's, and every step that touches it is announced and reversible.

**Status.** Every step below is a command. The whole flow has been run from this repository on the development
machine, twice: into a content-free copy of a repacked VaM 1.22.0.3 with BepInEx 5.0.1, and into an official
installation (VaM 1.22.0.13 from MeshedVR's updater, BepInEx 5.4.23.6 from its release page, no licence key): fetch
(from a local mirror), build, the offline tests, install, and the first run (`tools/lab/session/first_run.py`:
vanilla, then the mod on the desktop with path tracing switched on and off while playing, then the mod in VR on the
simulated headset with path tracing and frame generation). Its exit code is 0 when every part ran as asked, 2 when it
works with less than the kit has (a missing run-time file, a plugin that stepped aside, a stand-in upscaler or frame
generator: each is listed with its reason and remedy, and is yours to mend), 1 when a check failed. Other builds of
VaM:
[adapt-vam-version.md](adapt-vam-version.md). Not run from this repository's build yet: a real headset, a graphics
card that is not NVIDIA's, and scenes with real content. What was left for you to decide on the user's machine:
[open-items.md](open-items.md). Say what you ran and what you did not; do not
describe a step as run when it was not.

## What the user must bring

**A separate, clean copy of VaM to install into first.** Recommend it before anything else: a fresh VaM in a folder of
its own, with its own BepInEx and only the content needed for testing ([open-items.md](open-items.md), "A clean VaM to
test on"). The kit replaces files of VaM while it runs and changes how scenes load, render and simulate, and it has
been run on one PC; what it does to a large library of scenes, plugins and saves that nobody here has seen cannot be
foreseen. Install into the installation the user plays in only after the kit has proved itself on this machine, when
the user asks for it, and after backing up what they cannot replace.

- Their own licensed VaM 1.x. It needs BepInEx 5 (`BepInEx/core/BepInEx.dll`, `winhttp.dll` beside
  `VaM.exe`); `vamkit bepinex` puts it in when the folder has none (step 2b). Built and run with BepInEx 5.0.1 and 5.4.23.6; the plugins are compiled against the one that is there.
- Windows 10 or 11, an NVIDIA RTX GPU with a current driver. Path tracing needs DXR 1.1; hair in the path tracer uses
  linear swept spheres, which older RTX generations lack (everything else traces).
- Visual Studio 2022 Build Tools with the C++ workload, the ATL component (OpenComposite uses it) and a Windows SDK
  (fxc, dxc); CMake (it comes with the Build Tools); Git; Python 3.11 or later.
- The Windows feature ".NET Framework 3.5": its `csc.exe` compiles the plugins as C# 3, which is what VaM's Mono runs.
- For VR: a headset reached through an OpenXR runtime. The development setup is a Quest 3 through Virtual Desktop.

`python tools/vamkit doctor` checks all of this and says what is missing. It changes nothing. `python tools/vamkit
versions` then lists how this machine differs from the one the kit was made on: each difference is a place nobody has
run the kit, to be looked at during the first run rather than after the user meets it
([reference-machine.md](reference-machine.md)).

## The flow

Tell `vamkit` where VaM is once: `--vam <folder>` on each command, the environment variable `VAMKIT_VAM`, or one line
in `local/vam.txt` (git-ignored).

| Step | Command | What it does |
|---|---|---|
| 1 Check | `python tools/vamkit doctor`, `python tools/vamkit versions` | VaM, BepInEx, GPU, build tools, what is already fetched; what differs from the machine the kit was made on |
| 2 Fetch | `python tools/vamkit fetch --accept-licenses` | third-party SDKs and the DLSS runtimes into `third_party/` |
| 2b BepInEx | `python tools/vamkit bepinex`, then again with `--yes` | only when the VaM folder has no BepInEx (a clean VaM has none): puts BepInEx 5.4.23.6 from its own release page in, recorded so that `bepinex --remove` or `uninstall --purge` takes it out. A BepInEx that is there is never touched. Before `build`: the plugins are compiled against it |
| 3 Build | `python tools/vamkit build` | generators, native libraries and plugins of every component into `out/` |
| 4 Test | `python tools/vamkit test` | the offline tests against this VaM |
| 5 Install | `python tools/vamkit install`, then again with `--yes` | the plan first; then copies the build into the VaM folder, records what was there, writes two settings |
| 6 First run | `tools/lab/session/first_run.py --vam <VaM>` for you; `<VaM>/vamkit-launcher.bat` (the panel, `vamkit/launcher/LaunchPanel.py`) for the user | mod on the desktop first, then VR |

### 2 Fetch

`manifest/thirdparty.toml` pins every item (a commit, a version or a file hash); see [runtimes.md](runtimes.md).

- SDKs come from their vendors' own repositories with `git` (a shallow, partial clone of the pinned commit) or from
  nuget.org. Items under a vendor licence are listed first; read the licences, then pass `--accept-licenses`. Say this
  to the user: the acceptance is theirs.
- The DLSS runtimes for super resolution and ray reconstruction are downloaded from NVIDIA's repository and checked by
  hash.
- Two runtimes have no public download and are optional: the neural denoiser and frame generation. `fetch` looks for
  the frame generation runtime in the driver's own model store; for either one the user can point at their own copy:
  `python tools/vamkit fetch --file dlss-nr=<path to nvngx_dlssnr.dll>`. Without them the denoiser stays off and frame
  generation uses optical flow.
- `--mirror <folder>` takes everything from a folder laid out as `third_party/` (`<folder>/<id>/...`) instead of the
  network: for an offline machine or a second checkout.
- Fetching again is harmless: an item whose pin matches is left alone.

### 3 Build

```
python tools/vamkit build                 everything, in the manifest's order
python tools/vamkit build skin            one component (also several ids)
python tools/vamkit build --steps managed only the plugins; steps are generate, native, managed
python tools/vamkit build skin --clean    delete out/skin first
```

What happens, per component (`manifest/components.toml`):

1. **generate**: Python scripts read the shaders inside the user's own VaM and write to `out/gen/<id>/`. Nothing
   generated from VaM is in the repository. The path tracer's shaders are compiled here too, side by side; this is the
   long step (a minute or two) and it is skipped when nothing the shaders are made from has changed.
2. **native**: the component's `build.cmd` (MSVC; CMake inside it for OpenComposite and the OFXR layer).
3. **managed**: the BepInEx plugin, compiled with the C# 3.5 compiler against `<VaM>/VaM_Data/Managed` and
   `<VaM>/BepInEx/core`.

Each step prints one line with its time; a failing step prints the end of its log and the path of the full log
(`out/logs/`). A component whose third-party items (`needs` in the manifest) are not in `third_party/` yet stops before
its first step and names the `fetch` command to run. The tools are found automatically; `VAMKIT_VCVARS`, `VAMKIT_FXC`, `VAMKIT_DXC` and `VAMKIT_CSC` override
what is found. A Visual Studio without the ATL component can be given an `atlmfc` folder through `VAMKIT_ATL_ROOT`.

A generator that stops with a message about a program it cannot find or split is telling you this VaM build's shaders
differ from the ones the edits were written for: see [adapt-vam-version.md](adapt-vam-version.md).

### 4 Test

`python tools/vamkit test` runs each component's offline tests (the path tracer's contract probes, the skin, clothing
and tone-mapping shader tests, cpu-boost's bit-identity harness inside VaM's own Mono, the collector's checks against
this VaM's `mono.dll`). They need a GPU but not the game. A failure here on a VaM build the project was not developed
against is exactly what the tests are for: read [adapt-vam-version.md](adapt-vam-version.md) before changing anything.

### 5 Install

```
python tools/vamkit install              the plan: every file, new / update / replace, the settings; changes nothing
python tools/vamkit install --yes        does it
python tools/vamkit verify               the record against the VaM folder and against the build
python tools/vamkit rollback --yes       takes the last install back (one step)
```

Show the user the plan and wait for their yes before `--yes`. VaM must not be running from that folder.

What the install does (`tools/vamkit/install.py`; what goes where is only in the manifests):

- **Copies** every `installs` entry of `manifest/components.toml` and the fetched runtimes of
  `manifest/thirdparty.toml`, and checks each copy's hash once. `vamkit/bin/...` (OpenComposite as `openvr_api.dll`,
  the D3D11 proxy, the OFXR layer, the DLSS runtimes) is only a store: the launcher swaps those files into VaM's place
  while the mod runs. `BepInEx/plugins/...` files stay installed; they do nothing without the launcher's environment,
  except the runtime components, which work whenever BepInEx loads. A native library is installed as `.bin`, never
  `.dll`: BepInEx tries to load every `.dll` under `plugins` as a .NET assembly.
- **Writes the settings** under `enable` (hair-solver and cloth-physics are off by their own default) into the
  plugins' cfg files, leaving everything else in those files alone.
- **Records VaM's own files** as it finds them in `<VaM>/vamkit/original/`: `openvr_api.dll`, a `d3d11.dll` beside
  `VaM.exe`, `opencomposite.ini` (the file, or `<name>.absent`). The install does not replace them. A `d3d11.dll`
  that is already there belongs to something else (another injector, an earlier copy of this project): the plan says
  so; clear it up with the user before installing.
- **Keeps what it replaces.** A file that was already there under one of our names and that vamkit did not install
  (another copy of this project, installed by hand) stops the install unless `--replace` is given; then it goes to
  `<VaM>/vamkit/original/replaced/` and comes back on uninstall. Ask the user what that copy is before using the flag.
- **Leaves one step back ready**: `<VaM>/vamkit/backup/<time>/` holds what this install changed and a `restore.cmd`
  that puts it straight back without Python or the repository; `vamkit rollback` does the same.
- **Writes the record**, `<VaM>/vamkit/installed.json`: every installed file with its hash, the settings written and
  the values they had, where the rollback is. It is the one place that says what is installed.

`verify` exits 0 when the installed files are the recorded ones and VaM's own files are in their place. It notes when
the build in `out/` is newer than the install and when a setting the install wrote has another value now. "The mod's
file is in its place" without a running game means a run was killed: `python <VaM>/vamkit/launcher/launch.py
--restore`.

An update is the same command: build, `install` (the plan shows `update` lines), `install --yes`.

### 6 First run

`python tools/lab/session/first_run.py --vam <VaM folder>` does the first three steps below by itself, in the background
and without a headset: a vanilla session (nothing of the kit may be in the process), then a mod session on the desktop
with one person, DLSS super resolution and frame generation, and path tracing switched on and off while playing. When
the simulated headset is built (`vamkit fetch --lab --accept-licenses`, `vamkit build --lab`) a third session runs the
mod in VR on it: the VR session comes up, the frames handed to the headset show a picture, path tracing takes over,
frame generation hands over one frame per refresh. No real headset is used or needed, and the machine's own OpenXR
runtime is not touched. It prints one line per check and keeps what it saw. It starts the game two or three times
(three to six minutes): say so first. Then go through the steps with the user:

1. Start the launch panel, choose **mod** and **Desktop**. The desktop needs no headset and shows whether the chain
   works: the game starts, the menu opens, frame generation is generating.
2. Leave path tracing off for the first start. Then switch it on in the menu (lighting page). The first path-traced
   frame after a build waits for the driver to compile the pipelines, about 26 s with the picture frozen, once.
3. Look at the run's logs in `<VaM>/vamkit/logs/<time>-<mode>/`: no DLSS feature failed to create, the plugins loaded,
   no error storm.
4. Then VR, in the user's headset. To tune from there: [../knowledge/frame-budget.md](../knowledge/frame-budget.md) and
   [../knowledge/settings-and-menu.md](../knowledge/settings-and-menu.md); to put numbers on a setup before and after,
   [../benchmarks/README.md](../benchmarks/README.md).
5. Show the user **vanilla** in the panel once, so that they know their way back, and `launch.py --restore` for a run
   that was killed.

## What to ask the user, and when

- Before anything is written into the VaM folder. If they would rather not have their VaM touched yet, offer a trial:
  `python tools/lab/session/trial_copy.py --from <their VaM> --to <new folder>` copies the program files without any
  content (about 17 GB), and everything here works on the copy.
- Before `fetch --accept-licenses`: the vendors' licences are accepted by them, not by you.
- Which display they use most (VR or desktop) and, for VR, the headset's refresh rate: it sets the frame-rate target.
- Whether they have the two optional runtimes.
- Nothing else by default. Do not ask them to test in the headset what a log or the desktop can show.

## Uninstall

```
python tools/vamkit uninstall            the plan
python tools/vamkit uninstall --yes      the VaM folder as it was before the first install
```

Every installed file is deleted, or replaced by the file that was there before; VaM's own files are put back if a
killed run left the mod's in their place; a cfg the install made or changed goes back to what it was. Without a run in
between, the folder is file for file what it was. What the mod wrote while it was used is the user's and stays, and
the plan names it: the runs' logs (`vamkit/logs`), the launch presets (`vamkit/launcher/profiles`) and the settings
files the plugins made under `BepInEx/config` (in a cfg the install changed and that changed again since, only the keys
the install wrote go back). `--purge` removes those three as well; ask first. The two logs the mod's binaries write
beside `VaM.exe` are removed. A file that changed since the install is left and named.
