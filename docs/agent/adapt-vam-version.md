# Running on another VaM build

The project was developed against one VaM 1.x build (Unity 2018.1). It must not depend on that build: every part that
is bound to VaM's code or data checks what it finds and steps aside when it does not recognise it. This page says what
is bound to what, how each part reacts to a build it was not made for, and how to bring it back.

The rule for new code is the same: test behaviour, never a version list.

## What is bound to what

| Part | Bound to | On an unknown build | How you notice |
|---|---|---|---|
| Skin, hair, clothing replacement shaders | the bytecode of VaM's pixel programs in `VaM_Data/*.assets` | The generators run on this build's own programs. A patch that cannot find its place stops the generator with an error; nothing half-edited is produced | generator output; `spec_test`, `sss_test`, `mouth_test`, the clothing test |
| Path tracer's shader tables (program sets, overlay registry, shader states, shadow casters) | the shaders installed in this VaM | regenerated from this build | generator output |
| Path tracer's material translation | shader names and property meanings | a shader it has no contract for goes through the generic adapter and is logged once | BepInEx log: "PT object adapter" lines |
| cpu-boost | the IL of four VaM methods | a method whose IL fingerprint differs is left to VaM's own code, with a warning | BepInEx log; the plugin's statistics line shows which paths are on |
| gc-tuning | byte patterns and offsets inside `Mono/EmbedRuntime/mono.dll` | any mismatch: the collector is left as it is, with a warning | BepInEx log |
| hair-solver, cloth-physics | GPUTools' data layout and kernels inside VaM | an item it cannot take stays with GPUTools | verify modes: `[Diagnostics] VerifyEverySteps` |
| wrap-batch | the method bodies it transpiles | a failed patch leaves VaM's own path | BepInEx log; the plugin's `Verify` |
| Harmony patches in the other plugins | method names and signatures | the patch fails at load and the feature is off | BepInEx log at startup |
| Every plugin's build | the BepInEx 5 in the VaM folder: its `BepInEx.dll` and its Harmony (2.0 in BepInEx 5.0-5.3, HarmonyX in 5.4) | `components/shared/harmony-compat/HarmonyCompat.cs` is compiled into each plugin so that one source builds against both | `python tools/vamkit build` |
| OpenComposite, the D3D11 proxy, the OFXR layer | Unity's D3D11 renderer and OpenVR use, not VaM's code | unaffected by a VaM update that keeps the Unity version | |
| The launcher | `VaM.exe`, `VaM_Data/Plugins/openvr_api.dll`, BepInEx's loader | `doctor` reports what is missing | `python tools/vamkit doctor` |

Worst case for every row: the feature is off and VaM runs its own code. None of them computes with assumptions it has
not checked. That is the safe failure, and it is a failure: the user then has less than the kit offers, and a row that
stepped aside is something to mend, not something to accept. The first-run check and the benchmark report it as LESS
with the plugin's own lines; do not hand an installation over in that state without telling the user what they are
not getting ([../protocol/design-rules.md](../protocol/design-rules.md), rule 10).

## Procedure on a new build

1. `python tools/vamkit doctor --vam <VaM>`.
2. Regenerate everything into a clean `out/gen/` ([install.md](install.md), step 3). Read the output: every generator
   says how many programs it found and patched.
3. Rebuild and run the offline tests ([../protocol/testing.md](../protocol/testing.md), the table at the end).
4. Start once on the desktop with path tracing off, then on. Read the BepInEx log for warnings from our plugins.
5. For each part that stepped aside, decide with the user whether it matters to them before working on it.

## Bringing a part back

### A shader patch does not find its place

The generator names the program and the patch. Lift that program (`components/shading/skin/tools/asm_lift.py` through
the generator's own helpers) and compare it with what the patch expects: usually an instruction moved or a register
changed. Adjust the pattern, not the intent; then the component's tests must pass on this build. Keep the failure loud.

### cpu-boost leaves a method alone

The fingerprint is taken over the method's instructions with metadata tokens resolved to names, so a VaM that was
only compiled again is recognised; a warning means an instruction, a member or a string of that method changed. The
warning prints the new fingerprint, and `harness/run-paths.ps1` prints all five for the VaM it runs against. Do not
just replace the constant. Run the harness (`components/runtime/cpu-boost/harness`): it executes the native code
inside VaM's own Mono against the stock methods and compares bits. Read the new instructions (see "Builds seen so
far" for how to disassemble), update the native code statement by statement (the bit-identity rules are in
[../knowledge/cpu.md](../knowledge/cpu.md)), and change the constant only when the harness reports equality.

### gc-tuning does not recognise mono.dll

`components/runtime/gc-tuning/tools/check_layout.py` and the comments at the top of `native/gctuning.cpp` list every
offset and the instruction bytes expected there. A different Mono build needs each one located again by its code
pattern, the checks updated, and the offline tests (`fo_test`, `guard_test`) run against that `mono.dll`. If Unity's
Mono changed collector entirely, this component does not apply; leave it off.

### hair-solver or cloth-physics disagree with GPUTools

Switch the verify mode on. A difference means GPUTools' kernels or data layout changed in this build. The solvers'
header comments (`solver/fused_strand.hlsl`, `solver/cloth_fused.hlsl`) describe the order and arithmetic they follow;
compare with the kernels of this build. Until they agree, leave the component disabled: the result must be GPUTools'.

### A Harmony patch fails

The log names the method. Find its replacement in this build's `Assembly-CSharp.dll`, check that the patch's assumption
about what the method does still holds, and update the target. A patch that silently targets the wrong overload is
worse than one that fails.

### A plugin does not compile against this BepInEx

The plugins are compiled with the C# 3.5 compiler against the BepInEx in the user's VaM folder, so the BepInEx
version is part of the build. Known so far: HarmonyX (BepInEx 5.4) marks Harmony's five-argument `Patch` as an error
at compile time (`CS0619 ... Use newer Patch() instead`) and its current `Patch` has a sixth argument that Harmony 2.0
does not know. `components/shared/harmony-compat/HarmonyCompat.cs` hides the difference: `vamkit build` adds it to
every plugin that references `0Harmony.dll`. In plugin code write `Harmony` and `new Harmony(id)`, never
`HarmonyLib.Harmony`. A build script that compiles a plugin by itself (a test harness) has to add that file too.
cpu-boost's `harness/run-patch.ps1` compiles with it but cannot run on BepInEx 5.4: its stand-alone Mono host does not
load HarmonyX's own dependencies (`Mono.Cecil`). The script says so and passes; on such an installation the in-game
check is the BepInEx log and the plugin's verify mode.

## Builds seen so far

The user's VaM can be any recent build. Recognise code by what it is, never by a list of versions: a check that
accepts only one build's bytes turns a harmless rebuild of VaM into a feature that silently does nothing.

| Build | What was found (2026-11-11) |
|---|---|
| 1.22.0.3 (a repack, BepInEx 5.0.1) | the development build: everything was written against it |
| 1.22.0.13 (official updater, BepInEx 5.4.23.6) | Unity is still 2018.1.9 (another build number); `mono.dll` and `Assembly-CSharp.dll` differ. Of 30,599 methods 152 changed, 3 went and 102 are new, all in plugin security and confirmation (`FileManager`, `MVRPlugin`, `MVRPluginManager`, `DynamicCSharp`), saving and packages (`SuperController.Save*`, `VarPackage`, `PackageBuilder`), preference setters (`UserPreferences`) and six collider setters of `DAZPhysicsMeshSoftVerticesGroup`. No method that a plugin of this repository patches or replaces changed, and GPUTools did not change. The generators find all their shader programs; gc-tuning's checks of `mono.dll` pass; every plugin loads without an error line. cpu-boost's first fingerprint (over the raw bytes of a method body, which contain metadata tokens) rejected all four methods although their instructions are identical; it now resolves the tokens to names first and gives the same five values on both builds |

To compare two builds: disassemble both `Assembly-CSharp.dll` with `ildasm /text` (Windows SDK) and compare the
methods' instructions with offsets removed; tokens are names in that text. Keep the output out of the repository: it
is VaM's code.

## What to record

When you adapt a part to a new build, write into its knowledge document which build, what differed and how you
verified it, and keep the old fingerprint or pattern alongside the new one so that both builds work.
