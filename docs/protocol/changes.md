# Changing an installation

Rules for anything that touches the user's VaM folder or the repository. The user's VaM is theirs: scenes, presets,
looks and settings collected over years. An agent that breaks it, or leaves it in a state nobody can describe, has done
more harm than the feature was worth.

## The user's VaM folder

1. **Ask first, show the plan.** Commands that change the VaM folder print what they will do and need `--yes`. Say
   which files will be replaced and where the previous ones go.
2. **VaM is not running** when files are installed. Check for the process; do not install under a running game.
3. **Back up, copy, check once.** Before replacing a file, copy the current one (and its cfg) to a dated folder with a
   script that copies it straight back. After copying, check the hash once. No ceremony beyond that: no self-tests of
   the rollback, no repeated hashing.
4. **VaM's own files are kept in `<VaM>/vamkit/original/`** the first time anything of VaM's is replaced
   (`openvr_api.dll`; a `d3d11.dll` that was already there). When VaM had no such file, an `<name>.absent` marker is
   written instead, so that restoring deletes ours. The launcher keeps that record current (VaM updated, an injector
   added or removed) and puts a file back only where the mod's is in its place. Uninstall leaves the folder as it was
   found.
5. **One step of rollback is always ready**, and you say where it is. Older backups stay in their dated folders.
6. **Record what is installed.** After an install, the installed state (versions, hashes, where the rollback is) is
   written in one place and nowhere else; notes and READMEs do not repeat hashes.
7. **Nothing of ours in the user's content folders.** Test scenes, presets and textures live in the repository or a
   scratch folder. When a test has to load a scene, copy it into a temporary folder under `Saves/scene` that the tool
   removes when VaM exits; never leave test scenes where the user browses their own.
8. **Presets are the user's.** The launch panel keeps one preset per display mode and writes it into the plugin configs
   at launch. Change a setting through the menu or the preset, and do not put defaults back over values the user chose.
9. **Vanilla must stay vanilla.** After any change to the launcher, the proxy or the plugins, `launch.py --variant
   vanilla` still starts VaM with its own files and nothing of ours loaded. It is the user's way back.

## When an installed change misbehaves

Find the cause and fix it; do not revert as the first move. One session with the right data usually separates the
possibilities (a start that "hangs" for 26 s on the first path-traced frame after a shader change is the driver
compiling pipelines: the second start takes under a second). Revert when the person asks for it or when the cause
cannot be located, and say which.

After changing a path-tracing shader, run one path-traced session yourself before handing over, so that the driver's
one-time compile does not meet the person as a frozen game.

## The repository

1. **Nothing derived from VaM is committed**: generated shaders and tables (`out/gen/`), DXBC, listings of VaM's IL or
   compute kernels, anything read out of `VaM_Data`. Generators run on the user's machine.
2. **No vendor binaries or SDKs**: they are fetched into `third_party/` (ignored) from the vendor, or supplied by the
   user and checked by hash. See [../agent/runtimes.md](../agent/runtimes.md).
3. **Upstream notices stay** in `components/render/opencomposite` and `components/render/ofxr-dlssg`, and in every
   vendored third-party folder.
4. **Runtime components keep VaM's results.** A change to cpu-boost, hair-solver, cloth-physics or wrap-batch is done
   when its verify mode or harness shows equality again, not when it runs.
5. **A route that lost is removed everywhere**: engine code, API, menu switch, comparison tool. Do not leave it hidden
   or off by default; later work should find one path.
6. **One definition per fact.** Components are defined in `manifest/components.toml`, downloads in
   `manifest/thirdparty.toml`. Documents and tools read those; they do not keep lists of their own.
7. **Write down what you learned where the next agent will look**: the mechanism and the pitfall in the knowledge
   document of the topic, the rule in the protocol. Record conclusions and where the evidence is, not the story.

## Reporting

After a change, say what you verified and how (offline probe, simulated headset, built only), and what you did not.
"Not run in a real headset" means you did not run it; it is not by itself a risk to list. Do not describe a skipped step
as done, and do not present a small matter as serious or a real problem as small.
