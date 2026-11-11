# Design rules

The judgement this project was built with. These are not a checklist to run; they are what to weigh before starting.
What the person in front of you says now outranks every line here.

## 1. The picture comes first, still and in motion

A still frame: surfaces round and even, no bulges, pits, hard folds, tears or seams. In motion: nothing jumps, flickers
or trembles, and switching a feature on or off is not visible as a switch. Numbers (error, percentile, "better than
stock", frames gained) support a judgement; alone they pass nothing. Build so that the look is right by construction,
then confirm in the game with a still and with motion: stock | new.

Tuning a look is delicate. Step by the amount the person named ("a little" is a small step); do not swing between
extremes. A description of how something feels ("high-frequency", meaning uneasy) is not a quantity to measure
literally.

## 2. The character's motion wins

What the animation data says (a timeline, motion capture, an author's keyframes) is what the picture shows. Anything
that would distort, delay, limit or shake it gives way:

- our own systems (physics, hair, clothing): when resources run short, lower their precision or fall back; never the
  character's motion;
- the picture pipeline: where motion vectors and depth are composed, a person's own motion outranks effects,
  transparent things and props. The way to get there is not a draw-order priority but making each pixel's motion and
  depth come from the opaque surface actually seen there (transparent things carry neither). UI is the exception and
  stays on top of everything.

Before a new feature or when chasing a look problem, ask first: is the character's pose and motion in this frame what
the animation data gave?

## 3. Reason from the code

Causes and parameters come from the code: this project's sources, VaM's behaviour, Unity's and PhysX's rules. State
"this code, at this point in the frame, makes the result this". Do not start the game to diagnose or to pick a
parameter, do not sweep settings, do not infer from screenshots. The game confirms one prediction derived from the code,
and only that one. When the cause is clear enough to act, fix it; do not chase precise numbers the fix does not need.
Performance and loading are computed before they are measured, too.

Before any measurement, write what each possible result would make you do differently. If nothing, skip it and change
the code path you already identified.

## 4. Is the fix in the core, or is it a knob?

Decide this every time; neither answer is the default.

- **Core**: the pipeline computes something wrong systematically and all content of that kind suffers (a material
  translated incompletely, a motion vector taken from the wrong surface). Fix it in the core, completely. A parameter
  must not cover it up.
- **Not core**: degree and taste (too sheer, too bright, too stiff), where the content already has a parameter for it.
  Turn that parameter.

Two questions when unsure: does the cheap change cover every case of this kind? Is it repairing an error or adjusting a
degree?

For the second kind, look for the cheapest lever first: data, a parameter, a switch, "does this thing need to take part
at all". The fix does not have to sit where the cause is. Do not set "restore the original" as a silent goal, and do not
look only in the layer you happen to be working in. Two examples from this project: faces warped by transparent effects
during frame generation were fixed by subtraction (transparent things stop contributing depth and motion), after a
priority rule had been built and removed; clothing that looked too sheer was first answered by rebuilding shadow maps in
the tracer, which looked worse, when the author's own opacity adjustment was the lever. And even a cheap lever needs one
more question: what else is it used for? (Authors hide whole parts with that same adjustment.)

The deeper an investigation went, the stronger the pull to redeem it with a complex solution. Resist it.

## 5. Do what was asked, by the smallest route, and stop when it works

- Try existing tools and options first. Extra scope (a model, a calibration, a new metric, a follow-up project) gets one
  sentence of asking, not work followed by an explanation.
- Once the direction is set, start. Put the baseline into the comparison (stock | new | difference) rather than ranking
  candidates first.
- To stop something else from interfering with a feature, switch that thing off or remove it at the source; do not
  rewrite your own system to route around it. Before switching anything off, think about what stops with it: a scene's
  own animation is never touched.
- A feature that does not solve what it was for: say so and ask whether to remove it. Do not leave it on by default, and
  do not remove it unasked.

## 6. Better, not closer

The path tracer is judged by whether the picture is right and good, not by how closely it reproduces the raster picture.
Fill in what the tracer failed to read from the author (a light's meaning, a probe, a cookie); do not converge to the
stock look, and do not tune a global parameter until one test scene matches. That is fitting, not fixing. See
[../knowledge/path-tracing-authored-lighting.md](../knowledge/path-tracing-authored-lighting.md).

Look for established practice before designing (documentation, the key papers), and define coverage by everything that
can occur, not by the one scene at hand. When you find yourself patching one case after another, check whether the
method itself left established practice.

## 7. No timed whole-scene work

Code that runs every frame or on a timer costs in proportion to the objects it tracks, never to the whole scene.

- No `FindObjectsOfType`, `Resources.FindObjectsOfTypeAll` or `GameObject.Find` on a timer.
- Register objects from a Harmony postfix on the component's `Awake`; seed once at start; drop nulls.
- Settings that decide content (hair density, level of detail, budgets) are fixed for a situation: recompute when
  objects appear or disappear or when the user changes a setting, not continuously with camera distance.

Before calling a feature done, search for whole-scene calls reachable from `Update` or a timer and state the worst case
in a heavy scene.

## 8. CPU work

Parallel work uses cpu-boost's worker pool; no second pool and no thread-count setting of your own. A long-lived solver
thread takes one core of its own through the pool's pinning call. VaM's, Unity's and the driver's threads are never
given affinity masks, and the process affinity is never set. No per-vertex managed loop on the main thread: do it
natively. Details in [../knowledge/cpu.md](../knowledge/cpu.md).

## 9. Settings and menus

- A control shows the real value it sets (a 2x / 3x / 4x selector, not "on means 3x unless...").
- Each setting lives in one place: the in-game menu holds what is adjusted while playing; the launch panel holds launch
  choices. The panel may carry a menu value to the next launch but does not show it.
- In VR: no explanatory text (labels and values speak for themselves), pages and sub-pages instead of scrolling, large
  targets. Each level looks different (page, sub-page, value), and belonging has a shape (a card, a bold first row), not
  only indentation.
- Read the rule of a control aloud before shipping it. If the sentence needs "but when...", redesign.

Details in [../knowledge/settings-and-menu.md](../knowledge/settings-and-menu.md).

## 10. Working with less is a failure to report, not a way to pass

The project exists to give the user more: a better picture, more frames. A part that cannot run and quietly makes
room for something lesser keeps everything "working" and delivers nothing. Safe fallbacks stay (a part that would
compute wrong results must step aside), but stepping aside is a fault state, never a success state.

- **Try before refusing.** Do not decide from a card's name, a file's presence or a version number that something
  cannot run. Ask the thing itself (create the feature, load the library, check the code) and take its answer.
- **Look for what is missing before doing without it.** A run-time file may be on the machine in another place or
  another version: the vendor's own model store, a newer signed build. Accept what the vendor signed, say which version
  is in use, and refuse only what cannot work, with the reason.
- **Never substitute in silence.** When a stand-in runs (another upscaler, another frame generator, VaM's own code
  after a plugin stepped aside), say at the moment and place the user or their agent will see: what was asked for,
  what runs instead, what that costs them, how to get the real thing. A line in a log nobody opens is silence.
- **Never rewrite the user's choice.** A setting that cannot be honoured now stays as the user set it; what runs
  instead is shown beside it. The next start, with the missing piece added, must give them what they chose.
- **Do not hide the door.** A menu row or panel entry for something that cannot run here says why it cannot, rather
  than disappearing.
- **A check that passes on less is wrong.** Tools report three states, not two: as asked, working with less (each
  item with its reason and remedy), failed. `vamkit fetch`, `doctor`, `install`, `verify`, the first-run check and the
  benchmark all do; keep it so in anything new. "No error" is not the test; "the user got what the kit has" is.

## 11. When corrected

Replace the rule that caused the mistake; do not stack another on top. Move by the size of the correction: a named
defect is fixed as that defect, not answered by swinging to the opposite extreme. A list of single-incident rules
becomes a contradictory checklist that is followed without thinking, which is the failure these rules exist to prevent.
