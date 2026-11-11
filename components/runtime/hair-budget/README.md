# hair-budget

VaM's physics hair (GPUTools) draws, per scalp triangle of guide strands, `hairMultiplier` interpolated strands of
`curveDensity` segments, per eye and per pass (base, extra pixel lights, shadow maps). One preset's hair can reach
millions of segments. This plugin puts a cost budget on hair per item, per person and per scene by lowering only the
values the renderer and solver read (the sliders, scene and preset files keep their values), and can give hair its own
physics rate. The player gets a hair-cost limit with a tab in VaM's menu. Depends on BepInEx + Harmony; no other component.

## Files

| File | Holds |
|---|---|
| `VaMHairBudget.cs` | BepInEx plugin `local.vam.hairbudget` 1.2.1: Harmony postfix on `HairSimControl.Awake` (no scene searching), ini reading (about once a second), applying budgets |
| `HairBudgetMath.cs` | Cost model and water-filling, free of Unity so the offline test runs the same code |
| `VaMHairBudget.HairRate.cs` | Hair's own copy of VaM's Physics Rate / Update Cap and its own fixed-step clock |
| `VaMHairBudget.Menu.cs` | The tab in VaM's main menu (or User Preferences), built from VaM's plugin UI prefabs |
| `VaMHairBudget.ini` | Commented template, embedded in the plugin at build time and written to `BepInEx/config/` on first run |
| `tests/BudgetMathTest.cs` | Offline test of `HairBudgetMath` |

## Build

`python tools/vamkit build hair-budget` runs every step for this component (plugin) and writes to `out\hair-budget\`;
the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from the
`[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

Pure C#: compile `VaMHairBudget.cs`, `HairBudgetMath.cs`, `VaMHairBudget.HairRate.cs`, `VaMHairBudget.Menu.cs` as a
BepInEx plugin against the user's `BepInEx\core` and `VaM_Data\Managed` with the .NET 3.5 C# compiler (`VAMKIT_VAM`,
`VAMKIT_CSC`), embedding `VaMHairBudget.ini` as the resource named `VaMHairBudget.ini`. Output belongs in
`out\hair-budget\VaMHairBudget.dll`.

## Settings

`BepInEx/config/VaMHairBudget.ini` (comments inside are Chinese; keys are as below). Saved changes apply within about a second.

| Section / key | Default | Meaning |
|---|---|---|
| `[HairBudget] Profile` | `Balanced` | `Off`, `High` (120k segments per person, 300k per scene, density 2048, 3 iterations), `Balanced` (600k / 1.5M / 1024 / 2), `Performance` (350k / 800k / 640 / 2), `Custom` |
| `[HairBudget] DisableShadows` | `0` | `1` = hair casts no shadows (every shadowed light redraws hair; a point light six times) |
| `[HairBudget] ToggleKey` | `ctrl+shift+h` | Run-time toggle for comparing quality |
| `[HairBudget] MenuTab` | `MainMenu` | `MainMenu`, `UserPrefs` or `None` (restart VaM after changing) |
| `[Custom] PersonBudgetK`, `SceneBudgetK`, `DensityMax`, `CurveDensityMin`, `HairMultiplierMin`, `IterationsMax`, `WidthCompensation` | 600, 1500, 1024, 12, 4, 2, 1.5 | Used with `Profile=Custom` (0 = unlimited for budgets, density and iterations) |
| `[HairPhysics] Rate` | `VaM` | `VaM` = follow the body's physics steps (hair steps in its per-frame Dispatch, after the scalp is skinned); `Auto`; or 45 to 288 Hz |
| `[HairPhysics] UpdateCap` | `1` | 1 to 3, VaM's physics update cap |

Cost unit: patches x hairMultiplier x (curveDensity + 25) x 24/49 segments.

## Interfaces

- No exports. Reads and writes `HairLODSettings.FixedDensity/FixedDetail/FixedWidth` and `HairPhysicsSettings.Iterations`.
- With a hair rate set (including `Rate=VaM`, follow the body), hair steps in its per-frame Dispatch
  (`HairSettings.LateUpdate`) and the `Time.fixedDeltaTime` reads inside `HairPhysicsWorld.InitData` /
  `DispatchPhysicsImpl` return the hair step; style-mode items and items already on Update keep VaM's path. hair-solver
  runs under this clock (a frame with two hair steps keeps one solve).
- Log: search `BepInEx\LogOutput.log` for "VaM hair budget".

## Tests

- `tests/BudgetMathTest.cs`: runs `HairBudgetMath` over a library of person presets exported as a TSV (`BudgetMathTest.exe
  <preset-items.tsv> <out.csv>`) through High / Balanced / Performance and checks invariants (per-item cap, person budget).
  The exporter of that TSV is not in this repository, so the test cannot be reproduced as is. Needs the .NET 3.5 compiler.
- The menu and the hair clock are only exercised in the game.

## Invariants

- Never change what presets store: only the values the renderer and the solver read; saving a scene must keep the original values.
- Water-filling: items costlier per patch than a common level come down to it; lighter items and detail pieces under 5% of
  the budget keep their values; reductions are split between `curveDensity` and `hairMultiplier` with floors that keep
  strands following their guides and curls; width grows by up to `WidthCompensation`.
- Hair items are discovered by a Harmony postfix, never by searching the scene on a timer.
- Any failure of the hair clock puts hair back on VaM's FixedUpdate path for the session; a menu failure leaves the menu untouched.

## Read next

[../../../docs/knowledge/hair.md](../../../docs/knowledge/hair.md)
