// Compiled into every plugin that patches VaM (tools/vamkit adds this file to a managed build that references
// 0Harmony.dll). It lets one source build against whichever BepInEx 5 is in the user's VaM folder.
//
// The plugins call Harmony's five-argument Patch(original, prefix, postfix, transpiler, finalizer): the C# 3.5
// compiler has no optional arguments, so every argument is written out. Harmony 2.0 (BepInEx 5.0 to 5.3) has only
// that method. HarmonyX (BepInEx 5.4) still has it, for plugins built earlier, but marks it obsolete as an error and
// adds a sixth argument to the current one, so a source with five arguments does not compile there and a source with
// six does not compile against Harmony 2.0.
//
// This class has the name the plugins already use. A type of the global namespace is found before one that a using
// directive brings in, so `new Harmony(id)` and `Harmony h` in a plugin mean this class, and its Patch is the one
// their calls bind to. It passes the call on to the five-argument method, which exists at run time in both; the
// compiler accepts that call because it is made from a member that is itself marked obsolete.
//
// Rules for plugin code: write `Harmony`, never `HarmonyLib.Harmony`; create it with `new Harmony(id)`.
using System;
using System.Reflection;
using HarmonyLib;

public class Harmony : HarmonyLib.Harmony
{
    public Harmony(string id) : base(id) { }

    public new void Patch(MethodBase original, HarmonyMethod prefix, HarmonyMethod postfix, HarmonyMethod transpiler, HarmonyMethod finalizer)
    {
#pragma warning disable 612, 618
        PatchWithFive(original, prefix, postfix, transpiler, finalizer);
#pragma warning restore 612, 618
    }

    [Obsolete("only for Patch above")]
    private void PatchWithFive(MethodBase original, HarmonyMethod prefix, HarmonyMethod postfix, HarmonyMethod transpiler, HarmonyMethod finalizer)
    {
        base.Patch(original, prefix, postfix, transpiler, finalizer);
    }
}
