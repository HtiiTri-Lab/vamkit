# Documentation map

Written for a coding agent that has this repository and nothing else. Three kinds of document, by what you need:

| Folder | Answers | Read it when |
|---|---|---|
| [agent/](agent) | What do I do? | you were given a job: install, change something, move to another VaM build, find a cause |
| [knowledge/](knowledge) | Why is it built this way? What is known? | before changing a component, and whenever a number or a behaviour surprises you |
| [protocol/](protocol) | How does this project work? | before testing, measuring, or touching the user's VaM folder |

Every component folder under `components/` also has a `README.md`: files, build, settings, interfaces, tests, and what
a change must not break. Start there for anything local to one component.

## agent/

| Document | For |
|---|---|
| [install.md](agent/install.md) | getting the mod into a VaM folder, step by step, with what works today |
| [runtimes.md](agent/runtimes.md) | the DLSS runtimes and SDKs that are not in the repository: names, versions, where they come from |
| [modify.md](agent/modify.md) | changing or adding a component: the loop, recipes, tools for finding out what happens |
| [adapt-vam-version.md](agent/adapt-vam-version.md) | what is bound to VaM's code and data, and how to bring each part back on another build |
| [troubleshoot.md](agent/troubleshoot.md) | symptom to cause, across all components |
| [open-items.md](agent/open-items.md) | what was left for the installing agent to decide or finish on the user's machine, with what is known about each |

There is no separate tuning procedure yet. To tune for a machine or headset, read
[knowledge/frame-budget.md](knowledge/frame-budget.md) (where the time goes, the frame-rate target) and
[knowledge/settings-and-menu.md](knowledge/settings-and-menu.md) (every setting and what it trades), and judge results
by [protocol/performance.md](protocol/performance.md).

## knowledge/

Each document has the same sections: how it works, settings, measured, pitfalls, tried and dropped, open, verify.

| Document | Topic |
|---|---|
| [frame-path.md](knowledge/frame-path.md) | a frame from Unity's camera to the display, in VR and on the desktop; who owns each step |
| [d3d11on12.md](knowledge/d3d11on12.md) | Unity's D3D11 device on a D3D12 queue: why, what it costs, the rules that keep it cheap |
| [upscaling.md](knowledge/upscaling.md) | DLSS super resolution models, Ray Reconstruction, render scales, sharpening |
| [neural-rendering.md](knowledge/neural-rendering.md) | neural rendering (NR) on the output |
| [frame-generation.md](knowledge/frame-generation.md) | frame generation in VR and the desktop presenter; pacing |
| [motion-vectors.md](knowledge/motion-vectors.md) | where each element's motion and depth come from |
| [frame-budget.md](knowledge/frame-budget.md) | the development machine, the frame-rate target, where a frame's time goes, every gain measured |
| [settings-and-menu.md](knowledge/settings-and-menu.md) | every setting, the presets per display mode, the in-game menu |
| [path-tracing.md](knowledge/path-tracing.md) | the path tracer end to end |
| [path-tracing-authored-lighting.md](knowledge/path-tracing-authored-lighting.md) | reading scenes authored for raster lighting |
| [skin.md](knowledge/skin.md) | skin shading with path tracing off: lifted programs, layers, specular, mouth, teeth, eyes |
| [tone-and-display-lighting.md](knowledge/tone-and-display-lighting.md) | tone mapping and the optional display lighting |
| [hair.md](knowledge/hair.md) | hair shading, hair physics, the hair budget |
| [clothing.md](knowledge/clothing.md) | clothing shading, cloth physics, wrap batching |
| [cpu.md](knowledge/cpu.md) | character hot paths in native code, the shared worker pool, rules for CPU work |
| [gc.md](knowledge/gc.md) | the periodic stall and what was done to Mono's collector |
| [loading.md](knowledge/loading.md) | scene load and image load: where VaM's load time goes, the two components' mechanisms, what differs from VaM's result, settings, measurements |
| [vam-facts.md](knowledge/vam-facts.md) | facts about VaM and its Unity build that shaped the design |
| [rejected-and-open.md](knowledge/rejected-and-open.md) | directions evaluated and not taken; problems still open |

## protocol/

| Document | Rule set |
|---|---|
| [testing.md](protocol/testing.md) | the test card, one run that records everything, a losing candidate stops, which test covers which change |
| [performance.md](protocol/performance.md) | what counts as a cost or a gain; two kinds of stutter; estimating added work; measuring |
| [changes.md](protocol/changes.md) | touching the user's VaM folder; what never enters the repository; reporting |
| [design-rules.md](protocol/design-rules.md) | picture first, motion first, reason from code, core fix or lever, menus, CPU work |

## Tools

[../tools/lab/README.md](../tools/lab/README.md): the simulated headset and its scripts, the in-game test agent,
background sessions, the benchmark, the CPU census, stack and crash-address tools. Offline probes live with their
components (`components/*/*/tests`, `components/runtime/cpu-boost/harness`).

[benchmarks/README.md](benchmarks/README.md): the standard scenes, what is measured and how, how to run it on your
machine, and the results measured so far.

## Keeping this useful

When you learn something the next agent would otherwise have to rediscover (a mechanism, a pitfall, a route that lost),
write it into the knowledge document of that topic in the same turn. Record the conclusion and where the evidence is,
not the story of finding it. Numbers carry their conditions. Nothing private goes in: no names of the user's scenes or
characters, no paths of their machine, no chat.
