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
| OpenComposite, the D3D11 proxy, the OFXR layer | Unity's D3D11 renderer and OpenVR use, not VaM's code | unaffected by a VaM update that keeps the Unity version | |
| The launcher | `VaM.exe`, `VaM_Data/Plugins/openvr_api.dll`, BepInEx's loader | `doctor` reports what is missing | `python tools/vamkit doctor` |

Worst case for every row: the feature is off and VaM runs its own code. None of them computes with assumptions it has
not checked.

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

The warning prints the new fingerprint. Do not just add it to the list. Run the harness
(`components/runtime/cpu-boost/harness`): it executes the plugin's patches inside VaM's own Mono against the stock
methods and compares bits. If the method's IL changed, read the new IL, update the native code statement by statement
(the bit-identity rules are in [../knowledge/cpu.md](../knowledge/cpu.md)), and add the fingerprint only when the
harness reports equality.

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

## What to record

When you adapt a part to a new build, write into its knowledge document which build, what differed and how you
verified it, and keep the old fingerprint or pattern alongside the new one so that both builds work.
