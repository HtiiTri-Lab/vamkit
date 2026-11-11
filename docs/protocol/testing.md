# Testing protocol

How this project tests. The rules come from mistakes: sessions that took minutes each and answered nothing, a dozen
captures where two would do, a cause hunted through screenshots that the code would have given in one reading. A test
exists so that a person can decide about a change. It is not proof of diligence.

One VaM session costs minutes and holds the GPU, the headset path and the user's attention. Spend them deliberately.

## 1. Write the test card first

Three lines, in your reply, before anything runs:

1. The question, in the words of the person who asked.
2. What each possible result makes you do differently.
3. How many runs and how long.

If line 2 is empty, do not run: the result cannot change what you do. Then decide what the run records and how you get
it, so that one run answers everything you can foresee:

| What | Example |
|---|---|
| Is the result still right? | hash of a scene snapshot; per-item comparison with the original path; probe outputs byte for byte |
| Did it get better? | time of the stage, wait time, frames per second with frame generation off |
| Did the new code run? | the change's own counters: times the new path was taken, times it fell back |
| Did anything break? | error lines per log segment, before and after |
| Is the pipeline alive? | DLSS features created (no NGX create failure in the logs), the path tracer took over, frame generation is generating |

Prepare probes and log markers before the run. Do not run a second session for a number you could have recorded in the
first.

## 2. Use the evidence that exists; fill only the new gap

Do not measure the installed state again because a candidate exists. Quote the earlier measurement with the conditions
it was taken under; when a condition differs, measure only that. A rebuilt native library, a new version number or a
shader-only change does not make the old CPU numbers stale.

Offline replay compares two versions on the same recorded input. What happens outside the recording (PhysX, VaM's own
systems reacting) has to be seen in the game.

## 3. Design it right once

- Reuse the scenes, tools and data that exist (`tools/lab`, the probes in each component's `tests/`).
- The scene must show the problem directly and unobstructed.
- Every variant starts from the same state: reload, or freeze animation and switch within the same frame.
- On/off comparisons happen inside one VaM session. Do not start a second VaM for the other half.
- Take one still and check the framing before a series.

## 4. Data finds the fault; a person judges the look

Find errors with data the game exports: vertices, bones, material and lighting components, plugin status dumps, logs,
frame times. Before handing anything over, look at your own stills and raw motion frames once for framing, occlusion and
visible technical faults (black blocks, tearing, missing objects), and give the person the originals.

Whether it looks good is the person's call, in the headset or on their screen. Numbers (error, percentiles, "closer to
the original") are aids and cannot pass a change on their own.

**After touching anything process-wide** (address space reservations, import-table or system-function hooks, thread
priorities, memory limits), the candidate run must also check the rest of the pipeline. A collector change once reserved
address space in a way that left CUDA no contiguous range: every DLSS feature failed to create, the path tracer never
took over, and the collector's own numbers looked perfect.

## 5. Least time

Decide which number settles the question and when it first appears; set waits and timeouts to that moment. Read what is
already on disk (progress lines, file times, `nvidia-smi`) instead of waiting. Report and act on the first sufficient
number; let the rest finish in the background.

## 6. A candidate that does not win its first round stops

Write on the card: "does not beat the current state across the board -> the route stops". When the result comes, do that
and report. Do not adjust the candidate, add a feature or change the ground truth to rescue it: that defends your design
instead of answering the question.

- A clean scene that already says no is enough. You do not need every number to be clean.
- A rewrite that is line-for-line equivalent needs no comparison run. Read the code.
- Do not find causes by starting session after session. One run to locate, one run for the candidate, per round. If you
  still want more, report what you have and stop.
- Do not write the conclusion, or promise "I install it when this finishes", before the result exists. If the result
  cannot change the action, the run was not needed; if it can, the conclusion is not known yet.
- A small menu change that uses an existing mechanism needs no game session; the build's own checks are enough.
- A game comparison that did not line up (the character moved between two stills) is not retaken when an offline
  contract already proved the mechanism. Show what you have, say plainly that it does not line up, and let the person
  look in the game.
- **Picture artifacts are not diagnosed from screenshots.** When the person has said what they see, which switch makes
  it go away and where it occurs, treat that as fact and reason from the mechanism: which stage supplies this pixel's
  colour, motion and depth. Ask one question that separates two causes rather than capturing more images.
- Pick recorded cases by the question. A structural change whose result does not depend on lighting needs one or two
  recordings that contain the content it touches, compared "current | candidate", not the whole set.

## 7. Nothing is verified twice

When the files you install are the files you tested, check only hashes, config and the rollback. Run the game again only
if the installation adds a path the test did not take (different files, another way of switching a setting, a plugin
that was absent during the test, a change to the launch chain), and then test only that path. Report "verified by run X"
rather than describing what was not rerun as run.

## 8. The real headset belongs to the person

Putting on a headset to test is a real cost for them. Decide everything you can with the simulated headset, logs and
offline probes. Ask for one short headset session only when nothing else can answer, with one yes/no question, the
setting exposed as a menu option and a trigger that is easy to hit ("play normally, do X a few times").

What the simulated headset cannot do: hands, poses, interaction, changing segments the way a person plays. When a person
reports a physics or motion problem from play, find in the code which condition produces it (direction of force, a stop,
a contact) and construct that condition in an offline replay. A recorded animation clip is not a reproduction.

When a person reports how something looked, first establish what that session actually ran: versions, switches and the
line that says the feature was live, from that run's logs (`<VaM>/vamkit/logs/<run>/`). More than once the feature under
discussion was switched off in the session being described.

## 9. Stopping a test midway

Killing the test script takes the launcher with it and leaves VaM's files swapped. Close VaM, then run
`python <VaM>/vamkit/launcher/launch.py --restore`, and check that no stale OpenXR layer registration of the frame
generation layer is left under `HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit`. A retry loop must retry only the
failure it was written for; it must not restart VaM after the person closed it.

## Which test covers which change

| You changed | Run | It proves |
|---|---|---|
| The path tracer core or its shaders | `components/shading/rt-core/tests/build.cmd`, then `probe.exe` and `shadow_probe.exe` | Every contract still holds. With no intended change in behaviour, outputs are byte-identical to the previous build's; a new behaviour gets a new contract |
| The skin shader generators | `components/shading/skin/tests` (spec, sss, mouth) | The replacement programs match VaM's own programs where they must, and the edits do what they claim |
| cpu-boost | `components/runtime/cpu-boost/harness` | Each patched method returns VaM's own bits inside VaM's Mono |
| gc-tuning | `components/runtime/gc-tuning/tools` (`fo_test`, `guard_test`) | The replaced code agrees with Mono's own on random inputs; the boundary guard holds |
| hair-solver, cloth-physics, wrap-batch | their verify modes, in the game | Results equal GPUTools' / VaM's own step |
| The compositor, the bridge, frame generation, the menu | a session on the simulated headset or the desktop | Only the game shows it |
| First use of a changed path-tracing shader | one path-traced session yourself before handing over | The driver compiles the pipelines once (about 26 s with the picture frozen); the person should not meet that as a hang |

See also [performance.md](performance.md) for how results are judged and [../../tools/lab/README.md](../../tools/lab/README.md)
for the tools.
