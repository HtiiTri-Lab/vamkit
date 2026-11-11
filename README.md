# vamkit

A large image-quality and runtime-efficiency overhaul for Virt-A-Mate (VaM) 1.x on Windows with an NVIDIA RTX card: path tracing you can switch on and off in game,
DLSS super resolution and ray reconstruction, the DLSS neural denoiser, DLSS Frame Generation in VR and on the desktop,
new skin, hair and clothing shading, and runtime work that makes VaM's own hair, clothing and character updates cheaper.

[中文说明](README.zh.md)

**Status: work in progress.** The repository fetches its third-party parts, builds every component from source, runs
its offline tests and installs into a VaM folder reversibly (`vamkit fetch`, `build`, `test`, `install`, `verify`,
`uninstall`), and its first run on the desktop has passed in a content-free copy of VaM (`tools/lab/session`). It has
not been run in VR or with real scenes from this repository's build yet. See [CHANGELOG.md](CHANGELOG.md).

## What it does

| Part | What you get |
|---|---|
| Path tracing (in-game switch) | People, clothing, rooms and hair path traced, reconstructed by DLSS Ray Reconstruction |
| Path tracing off | VaM's own lighting and shadows with the new skin, hair and clothing shading |
| Upscaling | DLSS SR models K / L / M (path tracing off) or RR (path tracing on), each with its own render scale |
| Denoising | DLSS neural denoiser on the output, adjustable in game |
| Frame generation | DLSS Frame Generation 2-4x, in VR through the OFXR layer and on the desktop through our own presenter |
| Skin | Screen-space subsurface scattering and light through thin parts (ears, fingers), with or without path tracing |
| Runtime | Hair and cloth physics in fused GPU passes, clothing wraps batched, character CPU hot paths in native code, a faster Mono collector, a hair cost budget. Same results as VaM's own code |
| Launcher | Choose **mod** or **vanilla**, VR or desktop. Vanilla loads nothing of this project (and no BepInEx plugin at all) |

## How it is meant to be used

This project ships as source only. You build it on your own PC against your own copy of VaM, ideally with a coding
agent (Claude Code, Codex, Cursor and others read [AGENTS.md](AGENTS.md)). Some shaders are generated at build time
from the shaders inside your VaM installation; they never leave your PC.

Requirements:

- Your own licensed copy of VaM 1.x and BepInEx 5 installed in it
- Windows 10 or 11, an NVIDIA RTX GPU (path tracing needs DXR 1.1; RTX 50 recommended) and a current driver
- Visual Studio 2022 Build Tools (C++ and the Windows SDK), CMake, Python 3.11 or later

```
python tools/vamkit doctor --vam "D:/VaM"
python tools/vamkit fetch --accept-licenses
python tools/vamkit build
python tools/vamkit test
python tools/vamkit install          # prints the plan; --yes does it
```

## Written for your agent

The best way to install, tune and extend this project is to hand it to a coding agent. The repository is written
for that agent as much as for you:

- [docs/agent](docs/agent): how to install, change a component, adapt to another VaM build, and find the cause of a
  problem.
- [docs/knowledge](docs/knowledge): how every optimisation works and why it is built that way, what was measured,
  what was tried and dropped, what is still open.
- [docs/protocol](docs/protocol): how we test, judge performance and change an installation without breaking it.
- [tools/lab](tools/lab): the test and measurement tools we use ourselves.
- Every component folder has a README with its files, build, settings, tests and what a change must not break.

Whoever pulls this repository gets a tuning guide and a foundation for their own work, not only a build.

## Important

- Unofficial. Not affiliated with or endorsed by MeshedVR or NVIDIA.
- While the mod runs, the launcher replaces VaM's `VaM_Data/Plugins/openvr_api.dll` with OpenComposite and puts a
  `d3d11.dll` beside `VaM.exe`; it puts VaM's own files back when VaM exits. `launch.py --restore` does that by hand.
- NVIDIA runtimes and SDKs are not in this repository. `vamkit fetch` downloads them from NVIDIA after you accept
  their terms.

## License

Our code is MIT. OpenComposite is GPL-3.0 and OFXR Bridge is LGPL-3.0, as upstream. See [THIRD_PARTY.md](THIRD_PARTY.md).
