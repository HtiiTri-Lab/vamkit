# Judging performance

What counts as a cost, what counts as a gain, and how to tell smoothness problems apart. The numbers themselves and
where a frame's time goes are in [../knowledge/frame-budget.md](../knowledge/frame-budget.md).

## What is being optimised

The person plays in a headset or at a monitor. They care about the frame rate and evenness of normal play. Resource
figures are not goals.

1. **An extra cost is acceptable when the frame rate of normal rendering (menus closed) does not drop.** A short pause
   when a menu opens, neural rendering that runs at full size while the menu is up, memory that stays allocated after a
   switch: none of these justify redesigning or removing a feature. When asked "does it cost much", answer with the
   frame rate in normal play, briefly, and keep the feature.
2. **A saving goes to frame rate.** When something becomes cheaper, existing settings and presets keep the same picture
   and the gain shows up as frames per second. Do not re-tune a budget so that the same preset buys more density. A
   denser picture is an additional, higher preset; presets meant for speed stay as they are.
3. **Smooth is not the same as reaching the refresh rate.** With frame generation, real frames must arrive on a fixed
   beat: headset refresh divided by the multiplier (144 Hz at 4x is 36 real frames per second). Being a little short of
   the beat drops to the next step; being above it gains nothing. "Below refresh" does not explain a stutter.

## Two different smoothness problems

| | Local deformation jumps | Whole-picture stalls |
|---|---|---|
| What the person sees | one part (a body region, hair, cloth) jumps or stops while the rest moves | everything freezes for an instant |
| Where to look | input pose -> timestamp of the solver's result -> hand-over -> display interpolation -> the actual trajectory | real-frame spikes, the presenter's submission times, vsync gaps, CPU stops (garbage collection), your own timers |
| Common wrong conclusion | "every frame changes and the interpolation ratio is 100%", which only says one stage works | "the frame rate is below refresh" |

Four rates exist at once and must not be confused: the display refresh, the real input frame rate, the physics substeps
and the generated frames. Interpolation moves displayed frames between two physics results; physics advances on the
real input rhythm.

A stutter with a fixed period: compare the period with your own timers before suspecting the scene, the game or the
frame rate. Four two-second whole-scene searches lined up once cost two frames every two seconds in a heavy scene; they
were invisible in light scenes (see [design-rules.md](design-rules.md), "No timed whole-scene work").

When a short test does not reproduce what the person reported, record "this short test did not reproduce it" and keep
fixing what they described.

## Before implementing: estimate the added work

Count it: pixels x coverage x rays (or samples, or channels) per pixel; dispatches per frame; bytes copied. Look first
for data that already exists and for a cheap existing control. "Only one channel changes" does not mean cheap. A ray
count does not convert to a frame rate: when you have no basis for the cost, say it is unknown rather than guessing a
percentage. An offline contract that passes shows the implementation is right, not that the picture gains anything.
This is not a ban on added cost: state the need, the size and the alternatives first.

## Measuring

- Judge the frame time of normal play. To see sub-millisecond GPU savings, switch frame generation off so that VaM is
  GPU-bound, and compare the mean frame time with an adjacent reference window in the same session.
- Sessions differ from each other by a few frames per second for no reason you control. Compare inside one session
  (switch the setting live); never draw a conclusion from two sessions that differ by less than that.
- Stage timers in the compositor report over windows of about 10 s: hold each variant for 30 to 40 s.
- Other programs use the GPU too. Check what else is rendering before measuring (a desktop application drawing through a
  software path can take a quarter of the GPU while idle).
- A long job: say how long you expect it to take, measure its speed in the first minute, and report if the estimate was
  wrong. One VaM at a time. When you stop a script, stop its child processes as well.

## CPU

A new thread, a parallel loop, per-vertex work every frame or a new native library changes the CPU load: run the census
(`tools/lab/census`) for the candidate only and compare with the recorded baseline. A shader-only change, a rebuilt
native library with the same work, or installing what was tested does not. Rules for CPU work are in
[../knowledge/cpu.md](../knowledge/cpu.md).

## Think the whole change through

When a person adds a sentence to a problem they already described, put it into the same picture; do not change one
module per sentence. Work out what the solver, the hand-over and the display each own, then make one coherent fix.
Reverting is for diagnosis or to stop damage; the problem is done when it is fixed.
