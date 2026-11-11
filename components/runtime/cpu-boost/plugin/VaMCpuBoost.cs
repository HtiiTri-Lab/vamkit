// VaM CPU Boost: runs VaM 1.22.0.3's character hot paths in native code (VaMCpuBoostNative.bin) with results
// bit-identical to the game's own C#. VaM's Assembly-CSharp.dll is not touched; methods are patched at run time with
// Harmony and fall back to the C# for anything the native code was not built for.
//
//   DAZSkinV2.SkinMeshPart                                     CPU skinning (prefix)
//   DAZMorphBank.ApplyMorphsThreadedFast                       morph deltas (transpiler + finalizer on its delta loops)
//   RecalculateNormals.recalculateNormals(int[], int[])        post-skin normals (prefix)
//   DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast  soft-body vertex morphs (prefix)
// A method is patched only when its IL is the 1.22.0.3 code the native version was checked against.
//
// The native library is also the CPU runtime every native plugin shares (CB_Task / CB_Run / CB_PinThread): one worker
// pool on the cores VaM's own threads leave alone, jobs by priority, CPU time counted per task (LogStats logs them).
//
// BepInEx\config\local.vam.cpuboost.cfg:
//   [General]     Enabled (true), Threads (native worker threads, 0 = the calling thread only), WorkerCpus (where our
//                 worker threads run: Cores (default) or Any; see CpuBoostPlace)
//   [Paths]       Skinning, Morphs, Normals, SoftVertices (each on by default)
//   [Diagnostics] VerifyEvery (0 = off; N = every Nth call of each path also runs VaM's C#, the game keeps the C#
//                 result, and any bit difference is logged), LogStats (calls / time every 30 s)
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Threading;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;

[BepInPlugin("local.vam.cpuboost", "VaM CPU Boost", "0.4.0")]
public sealed class VaMCpuBoost : BaseUnityPlugin
{
    // IL fingerprints (FNV-1a 64 of the method body) of the VaM 1.22.0.3 methods. NormalsCoreIL is the static overload
    // recalculateNormals(int[], int[]) forwards to.
    internal const ulong SkinIL = 0x00d8f4fa6c8ba2d7UL, MorphIL = 0x1dac2efd506ad0b3UL, NormalsIL = 0x6a4c1ffb629083f5UL,
        NormalsCoreIL = 0x532b4b0ab53d0605UL, SoftIL = 0x2ecbb4d86a957f40UL;

    private sealed class Stat { public long calls, ticks, verified, differing; public int counter, failed; }
    private static readonly Stat skinStat = new Stat(), morphStat = new Stat(), normalsStat = new Stat(), softStat = new Stat();
    private static volatile bool skinOn, morphOn, normalsOn, softOn;
    private static int verifyEvery;
    private static VaMCpuBoost self;

    private ConfigEntry<bool> statsSetting;
    private float nextStats;

    private void Awake()
    {
        self = this;
        ConfigEntry<bool> enabled = Config.Bind("General", "Enabled", true, "Run the character hot paths natively (results bit-identical to VaM's C#).");
        ConfigEntry<int> threads = Config.Bind("General", "Threads", 8, "Native worker threads shared by all paths (0 = the calling thread only).");
        ConfigEntry<string> cpus = Config.Bind("General", "WorkerCpus", "Cores", "Where the native worker threads run (only those threads get CPU masks): " +
            "Cores = one worker per physical core, on the cores Windows prefers least (here CCD1, kept off VaM's cores); " +
            "Any = Windows places them.");
        ConfigEntry<bool> skin = Config.Bind("Paths", "Skinning", true, "DAZSkinV2.SkinMeshPart: CPU skinning.");
        ConfigEntry<bool> morphs = Config.Bind("Paths", "Morphs", true, "DAZMorphBank.ApplyMorphsThreadedFast: morph deltas.");
        ConfigEntry<bool> normals = Config.Bind("Paths", "Normals", true, "RecalculateNormals.recalculateNormals(int[], int[]): post-skin normals.");
        ConfigEntry<bool> soft = Config.Bind("Paths", "SoftVertices", true, "DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast: soft-body vertex morphs.");
        ConfigEntry<int> verify = Config.Bind("Diagnostics", "VerifyEvery", 0, "0 = off. N = every Nth call of each path also runs VaM's C#, the game keeps the C# result, and bit differences are logged.");
        statsSetting = Config.Bind("Diagnostics", "LogStats", true, "Log calls and native time every 30 seconds.");
        if (!enabled.Value) { Logger.LogInfo("off (Enabled = false)"); return; }
        try
        {
            string dir = Path.GetDirectoryName(typeof(VaMCpuBoost).Assembly.Location);
            CpuBoostNative.Load(Path.Combine(dir, "VaMCpuBoostNative.bin"));
            // The setting is an upper bound: two physical cores (counted as half the logical processors) stay free for
            // VaM's main and render threads, so the default 8 workers become fewer on a smaller CPU.
            int workers = Math.Min(Mathf.Clamp(threads.Value, 0, 32), Math.Max(1, Environment.ProcessorCount / 2 - 2));
            string placed = CpuBoostPlace.Apply(cpus.Value, workers);
            verifyEvery = Math.Max(0, verify.Value);
            string on = Install(new Harmony("local.vam.cpuboost"), skin.Value, morphs.Value, normals.Value, soft.Value,
                delegate(string m) { Logger.LogWarning(m); });
            Logger.LogInfo("on: " + (on.Length > 0 ? on : "nothing") + ", workers " + placed +
                (verifyEvery > 0 ? ", verifying every " + verifyEvery + " calls" : ""));
        }
        catch (Exception e)
        {
            skinOn = morphOn = normalsOn = softOn = false;
            CpuBoostMorph.active = false;
            Logger.LogError("not started, VaM's C# runs as usual: " + e);
        }
    }

    // Patches the paths asked for whose IL is the known 1.22.0.3 code; returns the ones now on. warn gets one line for
    // each path left off. Also used by the offline patch test.
    internal static string Install(Harmony h, bool skin, bool morphs, bool normals, bool soft, Action<string> warn)
    {
        const BindingFlags any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        var on = new List<string>();
        if (morphs || soft)
        {
            try { CpuBoostAddr.Init(); }
            catch (Exception e) { warn("morphs and soft vertices left off: " + e.Message); morphs = soft = false; }
        }
        if (soft)
        {
            try { CpuBoostSoft.InitPow(); }
            catch (Exception e) { warn("soft vertices left off: " + e.Message); soft = false; }
        }
        if (skin)
        {
            MethodInfo part = typeof(DAZSkinV2).GetMethod("SkinMeshPart", any, null,
                new Type[] { typeof(int), typeof(int), typeof(bool[]), typeof(bool[]) }, null);
            MethodInfo destroy = typeof(DAZSkinV2).GetMethod("OnDestroy", any, null, Type.EmptyTypes, null);
            if (part != null && destroy != null && Known(part, SkinIL, "DAZSkinV2.SkinMeshPart", warn))
            {
                h.Patch(part, Own("SkinPartPrefix"), Own("SkinPartPostfix"), null, null);
                h.Patch(destroy, null, Own("SkinDestroyed"), null, null);
                skinOn = true;
                on.Add("skinning");
            }
        }
        if (morphs)
        {
            MethodInfo apply = typeof(DAZMorphBank).GetMethod("ApplyMorphsThreadedFast", any, null,
                new Type[] { typeof(Vector3[]), typeof(Vector3[]), typeof(DAZBones) }, null);
            if (apply != null && Known(apply, MorphIL, "DAZMorphBank.ApplyMorphsThreadedFast", warn))
            {
                h.Patch(apply, Own("MorphPrefix"), null, Own("MorphTranspiler"), Own("MorphFinalizer"));
                CpuBoostMorph.active = morphOn = true;
                on.Add("morphs");
            }
        }
        if (normals)
        {
            MethodInfo recalc = typeof(RecalculateNormals).GetMethod("recalculateNormals", any, null,
                new Type[] { typeof(int[]), typeof(int[]) }, null);
            MethodInfo core = typeof(RecalculateNormals).GetMethod("recalculateNormals", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null,
                new Type[] { typeof(int[]), typeof(Vector3[]), typeof(Vector3[]), typeof(Vector3[]), typeof(int[]), typeof(int[]), typeof(bool[]), typeof(bool) }, null);
            if (recalc != null && core != null && Known(recalc, NormalsIL, "RecalculateNormals.recalculateNormals(int[], int[])", warn) &&
                Known(core, NormalsCoreIL, "RecalculateNormals.recalculateNormals (static, subset)", warn))
            {
                h.Patch(recalc, Own("NormalsPrefix"), Own("NormalsPostfix"), null, null);
                normalsOn = true;
                on.Add("normals");
            }
        }
        if (soft)
        {
            MethodInfo morph = typeof(DAZPhysicsMeshSoftVerticesGroup).GetMethod("MorphVerticesThreadedFast", any, null,
                new Type[] { typeof(Vector3[]) }, null);
            if (morph != null && Known(morph, SoftIL, "DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast", warn))
            {
                h.Patch(morph, Own("SoftPrefix"), Own("SoftPostfix"), null, null);
                softOn = true;
                on.Add("soft vertices");
            }
        }
        return string.Join(", ", on.ToArray());
    }

    private static HarmonyMethod Own(string name)
    {
        return new HarmonyMethod(typeof(VaMCpuBoost).GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic));
    }

    internal static ulong Fingerprint(MethodBase m)
    {
        byte[] il = m.GetMethodBody().GetILAsByteArray();
        ulong h = 14695981039346656037UL;
        foreach (byte b in il) { h ^= b; h *= 1099511628211UL; }
        return h;
    }

    private static bool Known(MethodBase m, ulong expected, string name, Action<string> warn)
    {
        ulong f = Fingerprint(m);
        if (f == expected) return true;
        warn(name + " left alone: its code is not the VaM 1.22.0.3 code checked offline (IL fingerprint " + f.ToString("x16") + ")");
        return false;
    }

    private void Update()
    {
        if (!statsSetting.Value || Time.realtimeSinceStartup < nextStats) return;
        nextStats = Time.realtimeSinceStartup + 30f;
        var parts = new List<string>();
        Report(parts, "skinning", skinStat, CpuBoostSkin.fallbacks);
        Report(parts, "morph flushes", morphStat, CpuBoostMorph.direct);
        Report(parts, "normals", normalsStat, CpuBoostNormals.fallbacks);
        Report(parts, "soft vertices", softStat, CpuBoostSoft.fallbacks);
        if (parts.Count > 0) Logger.LogInfo("last 30 s: " + string.Join(" | ", parts.ToArray()));
        string tasks = Tasks(true);
        if (tasks.Length > 0) Logger.LogInfo("last 30 s, CPU of every task on the shared runtime: " + tasks);
    }

    private static double tasksSince;

    // Every task run on the native runtime (ours and other plugins' natives): calls, the callers' time in it and the
    // CPU time of its chunks on all threads, each in ms a second since the last reset. Also for the test agent.
    public static string Tasks(bool reset)
    {
        if (CpuBoostNative.TaskCount == null) return "";
        double now = Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency, seconds = Math.Max(1e-3, now - tasksSince);
        if (reset) tasksSince = now;
        var parts = new List<string>();
        var name = new byte[48];
        var s = new double[3];
        for (int i = 0, n = CpuBoostNative.TaskCount(); i < n; i++)
        {
            if (CpuBoostNative.TaskStats(i, name, name.Length, s, reset ? 1 : 0) == 0 || s[0] == 0) continue;
            int len = Array.IndexOf(name, (byte)0);
            parts.Add(System.Text.Encoding.ASCII.GetString(name, 0, len < 0 ? name.Length : len) + " " + (s[0] / seconds).ToString("F0") +
                " calls/s, callers " + (s[1] / seconds).ToString("F1") + " ms/s, CPU " + (s[2] / seconds).ToString("F1") + " ms/s");
        }
        return string.Join(" | ", parts.ToArray());
    }

    private static void Report(List<string> parts, string name, Stat s, long fallbacks)
    {
        long calls = Interlocked.Exchange(ref s.calls, 0), ticks = Interlocked.Exchange(ref s.ticks, 0);
        long verified = Interlocked.Read(ref s.verified), differing = Interlocked.Read(ref s.differing);
        if (calls == 0 && verified == 0) return;
        parts.Add(name + " " + calls + " native, avg " + (calls > 0 ? (ticks * 1000.0 / Stopwatch.Frequency / calls).ToString("F3") : "-") +
            " ms, C# fallbacks " + fallbacks + (verifyEvery > 0 ? ", verified " + verified + ", differing " + differing : ""));
    }

    private static bool Verify(Stat s) { return verifyEvery > 0 && Interlocked.Increment(ref s.counter) % verifyEvery == 0; }
    private static void Count(Stat s, long t0) { Interlocked.Add(ref s.ticks, Stopwatch.GetTimestamp() - t0); Interlocked.Increment(ref s.calls); }

    private static void Verified(Stat s, string name, int bad, string detail)
    {
        Interlocked.Increment(ref s.verified);
        if (bad == 0) return;
        Interlocked.Increment(ref s.differing);
        if (self != null) self.Logger.LogWarning("verify: " + name + " differs: " + detail);
    }

    // A failure inside a path turns that path off for the session; VaM's C# runs from there.
    private static void Fail(Stat s, string name, Exception e)
    {
        if (s == skinStat) skinOn = false;
        else if (s == morphStat) { morphOn = false; CpuBoostMorph.active = false; }
        else if (s == normalsStat) normalsOn = false;
        else if (s == softStat) softOn = false;
        if (Interlocked.Exchange(ref s.failed, 1) == 0 && self != null)
            self.Logger.LogError(name + ": native path stopped for this session, VaM's C# runs from here: " + e);
    }

    private static T[] Clone<T>(T[] a) { return a == null ? null : (T[])a.Clone(); }
    private static bool Same(float a, float b) { return BitConverter.ToInt32(BitConverter.GetBytes(a), 0) == BitConverter.ToInt32(BitConverter.GetBytes(b), 0); }
    private static int Differ(Vector3[] a, Vector3[] b)
    {
        if (a == null || b == null) return a == b ? 0 : 1;
        int bad = 0;
        for (int i = 0; i < a.Length && i < b.Length; i++) if (!Same(a[i].x, b[i].x) || !Same(a[i].y, b[i].y) || !Same(a[i].z, b[i].z)) bad++;
        return bad;
    }
    private static int Differ(float[] a, float[] b)
    {
        if (a == null || b == null) return a == b ? 0 : 1;
        int bad = 0;
        for (int i = 0; i < a.Length && i < b.Length; i++) if (!Same(a[i], b[i])) bad++;
        return bad;
    }
    private static int Differ(bool[] a, bool[] b)
    {
        if (a == null || b == null) return a == b ? 0 : 1;
        int bad = 0;
        for (int i = 0; i < a.Length && i < b.Length; i++) if (a[i] != b[i]) bad++;
        return bad;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // DAZSkinV2.SkinMeshPart
    // ---------------------------------------------------------------------------------------------------------------

    // State for a verified call: the native result over the same inputs, compared after the C# ran.
    private sealed class SkinCheck { public Vector3[] native; public int start, stop; }

    private static bool SkinPartPrefix(DAZSkinV2 __instance, int startIndex, int stopIndex, bool[] onlyVerts, bool[] onlyBones,
        int ____numBones, bool ____useGeneralWeights, Matrix4x4[] ___boneChangeFromOriginalMatrices,
        Matrix4x4[] ___boneWorldToLocalMatrices, Matrix4x4[] ___boneLocalToWorldMatrices, Vector3[] ___boneRotationAngles,
        Vector3[] ___workingVerts, Vector3[] ___startVertsCopy, ref SkinCheck __state)
    {
        if (!skinOn) return true;
        try
        {
            if (Verify(skinStat) && ___workingVerts != null)
            {
                // Native into a copy; the C# then runs on the real array and the postfix compares the range.
                var copy = (Vector3[])___workingVerts.Clone();
                if (CpuBoostSkin.TrySkinPart(__instance, __instance.nodes, ____numBones, ____useGeneralWeights, __instance.bulgeScale,
                    ___boneChangeFromOriginalMatrices, ___boneWorldToLocalMatrices, ___boneLocalToWorldMatrices, ___boneRotationAngles,
                    copy, ___startVertsCopy, startIndex, stopIndex, onlyVerts, onlyBones))
                    __state = new SkinCheck { native = copy, start = startIndex, stop = stopIndex };
                return true;
            }
            long t0 = Stopwatch.GetTimestamp();
            bool done = CpuBoostSkin.TrySkinPart(__instance, __instance.nodes, ____numBones, ____useGeneralWeights, __instance.bulgeScale,
                ___boneChangeFromOriginalMatrices, ___boneWorldToLocalMatrices, ___boneLocalToWorldMatrices, ___boneRotationAngles,
                ___workingVerts, ___startVertsCopy, startIndex, stopIndex, onlyVerts, onlyBones);
            if (done) Count(skinStat, t0);
            return !done;
        }
        catch (Exception e) { Fail(skinStat, "SkinMeshPart", e); return true; }
    }

    private static void SkinPartPostfix(DAZSkinV2 __instance, Vector3[] ___workingVerts, SkinCheck __state)
    {
        if (__state == null) return;
        try
        {
            int lo = Math.Max(0, __state.start), hi = Math.Min(___workingVerts.Length - 1, __state.stop), bad = 0, first = -1;
            for (int v = lo; v <= hi; v++)
            {
                Vector3 a = ___workingVerts[v], b = __state.native[v];
                if (!Same(a.x, b.x) || !Same(a.y, b.y) || !Same(a.z, b.z)) { if (first < 0) first = v; bad++; }
            }
            Verified(skinStat, "SkinMeshPart", bad, bad == 0 ? "" : bad + " of " + (hi - lo + 1) + " vertices of " + __instance.name +
                ", first " + first + " C# " + ___workingVerts[first].ToString("R") + " native " + __state.native[first].ToString("R"));
        }
        catch (Exception e) { Fail(skinStat, "SkinMeshPart", e); }
    }

    private static void SkinDestroyed(DAZSkinV2 __instance)
    {
        try { CpuBoostSkin.Forget(__instance); } catch (Exception e) { Fail(skinStat, "SkinMeshPart", e); }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // DAZMorphBank.ApplyMorphsThreadedFast
    // ---------------------------------------------------------------------------------------------------------------

    private static void MorphPrefix(Vector3[] verts, Vector3[] visibleNonPoseVerts)
    {
        if (!morphOn) return;
        try { if (Verify(morphStat)) CpuBoostMorph.BeginVerify(verts, visibleNonPoseVerts); }
        catch (Exception e) { Fail(morphStat, "ApplyMorphsThreadedFast", e); }
    }

    // Runs when the method returns or throws: applies the deferred delta loops. An exception from a replayed loop (a
    // delta the game's own loop throws on) is the method's exception, as it would have been unpatched.
    private static Exception MorphFinalizer(Exception __exception)
    {
        Exception thrown = null;
        try
        {
            bool timed = CpuBoostMorph.HasPending();
            long t0 = Stopwatch.GetTimestamp();
            int bad = CpuBoostMorph.EndCall();
            if (timed) Count(morphStat, t0);
            if (bad >= 0) Verified(morphStat, "ApplyMorphsThreadedFast", bad, bad + " vertices");
        }
        catch (Exception e) { thrown = e; }
        return thrown ?? __exception;
    }

    // In front of each of the two delta loops: if (CpuBoostMorph.Add(morph.deltas, value - appliedValue, <visible
    // branch>, verts, visibleNonPoseVerts)) skip the loop. Built for the 1.22.0.3 IL (checked by its fingerprint).
    private static IEnumerable<CodeInstruction> MorphTranspiler(IEnumerable<CodeInstruction> instructions)
    {
        var code = new List<CodeInstruction>(instructions);
        FieldInfo deltas = typeof(DAZMorph).GetField("deltas");
        FieldInfo changed = typeof(DAZMorphBank).GetField("threadedVerticesChanged", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        MethodInfo add = typeof(CpuBoostMorph).GetMethod("Add", BindingFlags.Static | BindingFlags.NonPublic);
        object skip = null;
        CodeInstruction weightStore = null;
        var loops = new List<int>();
        for (int i = 0; i + 1 < code.Count; i++)
        {
            CodeInstruction c = code[i];
            // if (morph.deltas.Length <= 0) goto <after the loops>
            if (skip == null && c.opcode == OpCodes.Ldfld && Equals(c.operand, deltas) && code[i + 1].opcode == OpCodes.Ldlen)
                for (int k = i + 2; k < i + 6 && k < code.Count; k++)
                    if (code[k].opcode == OpCodes.Ble || code[k].opcode == OpCodes.Ble_S) { skip = code[k].operand; break; }
            // threadedVerticesChanged = true; V_19 = value - appliedValue
            if (weightStore == null && c.opcode == OpCodes.Stfld && Equals(c.operand, changed))
                for (int k = i + 1; k < i + 6 && k < code.Count; k++)
                    if (IsStloc(code[k].opcode)) { weightStore = code[k]; break; }
            // V_21 = morph.deltas / V_24 = morph.deltas: the start of each loop
            if (c.opcode == OpCodes.Ldfld && Equals(c.operand, deltas) && IsStloc(code[i + 1].opcode) && i > 0 && IsLdloc(code[i - 1].opcode))
                loops.Add(i - 1);
        }
        if (skip == null || weightStore == null || loops.Count != 2)
            throw new InvalidOperationException("ApplyMorphsThreadedFast: delta loops not found (" + (skip != null) + ", " + (weightStore != null) + ", " + loops.Count + ")");
        for (int n = loops.Count - 1; n >= 0; n--)
        {
            int at = loops[n];
            CodeInstruction morph = code[at];
            var ins = new List<CodeInstruction>();
            ins.Add(new CodeInstruction(morph.opcode, morph.operand));
            ins.Add(new CodeInstruction(OpCodes.Ldfld, deltas));
            ins.Add(new CodeInstruction(LoadFor(weightStore.opcode), weightStore.operand));
            ins.Add(new CodeInstruction(n == 0 ? OpCodes.Ldc_I4_1 : OpCodes.Ldc_I4_0, null));   // the first loop is the visible one
            ins.Add(new CodeInstruction(OpCodes.Ldarg_1, null));
            ins.Add(new CodeInstruction(OpCodes.Ldarg_2, null));
            ins.Add(new CodeInstruction(OpCodes.Call, add));
            ins.Add(new CodeInstruction(OpCodes.Brtrue, skip));
            // Branches to the loop start now land on the Add call.
            ins[0].labels.AddRange(morph.labels);
            morph.labels.Clear();
            code.InsertRange(at, ins);
        }
        return code;
    }

    private static bool IsStloc(OpCode o) { return o == OpCodes.Stloc || o == OpCodes.Stloc_S || o == OpCodes.Stloc_0 || o == OpCodes.Stloc_1 || o == OpCodes.Stloc_2 || o == OpCodes.Stloc_3; }
    private static bool IsLdloc(OpCode o) { return o == OpCodes.Ldloc || o == OpCodes.Ldloc_S || o == OpCodes.Ldloc_0 || o == OpCodes.Ldloc_1 || o == OpCodes.Ldloc_2 || o == OpCodes.Ldloc_3; }
    private static OpCode LoadFor(OpCode store)
    {
        if (store == OpCodes.Stloc_S) return OpCodes.Ldloc_S;
        if (store == OpCodes.Stloc_0) return OpCodes.Ldloc_0;
        if (store == OpCodes.Stloc_1) return OpCodes.Ldloc_1;
        if (store == OpCodes.Stloc_2) return OpCodes.Ldloc_2;
        if (store == OpCodes.Stloc_3) return OpCodes.Ldloc_3;
        return OpCodes.Ldloc;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RecalculateNormals.recalculateNormals(int[] trianglesToUse, int[] verticesToUse)
    // ---------------------------------------------------------------------------------------------------------------

    private sealed class NormalsCheck { public Vector3[] normals, surf; public bool[] marker; }

    private static bool NormalsPrefix(int[] trianglesToUse, int[] verticesToUse, int[] ___triangles, Vector3[] ___vertices,
        Vector3[] ___normals, Vector3[] ___surfaceNormals, bool[] ___thisMarkerArray, ref NormalsCheck __state)
    {
        if (!normalsOn) return true;
        try
        {
            if (Verify(normalsStat))
            {
                var c = new NormalsCheck { normals = Clone(___normals), surf = Clone(___surfaceNormals), marker = Clone(___thisMarkerArray) };
                if (CpuBoostNormals.TryRecalc(___triangles, ___vertices, c.normals, c.surf, c.marker, trianglesToUse, verticesToUse)) __state = c;
                return true;
            }
            long t0 = Stopwatch.GetTimestamp();
            bool done = CpuBoostNormals.TryRecalc(___triangles, ___vertices, ___normals, ___surfaceNormals, ___thisMarkerArray,
                trianglesToUse, verticesToUse);
            if (done) Count(normalsStat, t0);
            return !done;
        }
        catch (Exception e) { Fail(normalsStat, "recalculateNormals", e); return true; }
    }

    private static void NormalsPostfix(Vector3[] ___normals, Vector3[] ___surfaceNormals, bool[] ___thisMarkerArray, NormalsCheck __state)
    {
        if (__state == null) return;
        try
        {
            int dn = Differ(___normals, __state.normals), ds = Differ(___surfaceNormals, __state.surf), dm = Differ(___thisMarkerArray, __state.marker);
            Verified(normalsStat, "recalculateNormals", dn + ds + dm, "normals " + dn + ", surface normals " + ds + ", markers " + dm);
        }
        catch (Exception e) { Fail(normalsStat, "recalculateNormals", e); }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast(Vector3[] verts)
    // ---------------------------------------------------------------------------------------------------------------

    private sealed class SoftCheck
    {
        public DAZSkinV2 skin; public List<DAZPhysicsMeshSoftVerticesSet> sets;
        public Vector3[] psm, moves; public bool[] psv, psnv; public float[][] dist, wts;
    }

    private static bool SoftPrefix(Vector3[] verts, bool ___wasInit, bool ____on, bool ____freeze, bool ____useSimulation,
        bool ____useInterpolation, bool ___useCustomInterpolation, float ____maxInfluenceDistance,
        DAZPhysicsMeshSoftVerticesGroup.LookAtOption ____lookAtOption, List<DAZPhysicsMeshSoftVerticesSet> ____softVerticesSets,
        bool ___centerBetweenTargetAndAnchor, DAZPhysicsMeshSoftVerticesGroup.InfluenceType ____influenceType,
        float ___oneoverscale, DAZSkinV2 ____skin, bool ____autoInfluenceAnchor, float ____falloffPower, float ____weightBias,
        float ____weightMultiplier, ref SoftCheck __state)
    {
        if (!softOn || !___wasInit || !____on || ____freeze) return true;   // the C# returns at once
        // By reference: the C# dereferences the managed object, and Unity's == also calls a destroyed object null.
        if (ReferenceEquals(____skin, null)) return true;
        try
        {
            CpuBoostSoft.Params prm = CpuBoostSoft.MakeParams(____useSimulation, ____useInterpolation, ___useCustomInterpolation,
                (int)____lookAtOption, ___centerBetweenTargetAndAnchor, (int)____influenceType, ____autoInfluenceAnchor,
                ____maxInfluenceDistance, ___oneoverscale, ____falloffPower, ____weightBias, ____weightMultiplier);
            List<DAZPhysicsMeshSoftVerticesSet> sets = ____softVerticesSets;
            bool changed;
            if (Verify(softStat))
            {
                // Native into copies of the skin's arrays (distances and weights go to the sets' own arrays, which the
                // C# recomputes before it reads them); the C# then runs and the postfix compares.
                var c = new SoftCheck { skin = ____skin, sets = sets, psm = Clone(____skin.postSkinMorphs),
                    psv = Clone(____skin.postSkinVerts), psnv = Clone(____skin.postSkinNormalVerts) };
                if (CpuBoostSoft.TryMorph(sets, verts, prm, c.psv, c.psnv, c.psm, false, out changed) && sets != null)
                {
                    int n = sets.Count;
                    c.dist = new float[n][]; c.wts = new float[n][]; c.moves = new Vector3[n];
                    for (int i = 0; i < n; i++)
                    {
                        c.dist[i] = Clone(sets[i].influenceVerticesDistances);
                        c.wts[i] = Clone(sets[i].influenceVerticesWeights);
                        c.moves[i] = prm.useInterp != 0 ? CpuBoostSoft.LastMove(i) : sets[i].move;
                    }
                    __state = c;
                }
                return true;
            }
            long t0 = Stopwatch.GetTimestamp();
            bool done = CpuBoostSoft.TryMorph(sets, verts, prm, ____skin.postSkinVerts, ____skin.postSkinNormalVerts,
                ____skin.postSkinMorphs, true, out changed);
            if (!done) return true;
            if (changed) ____skin.postSkinVertsChanged = true;
            Count(softStat, t0);
            return false;
        }
        catch (Exception e) { Fail(softStat, "MorphVerticesThreadedFast", e); return true; }
    }

    private static void SoftPostfix(SoftCheck __state)
    {
        if (__state == null) return;
        try
        {
            DAZSkinV2 skin = __state.skin;
            int dm = Differ(skin.postSkinMorphs, __state.psm), dv = Differ(skin.postSkinVerts, __state.psv);
            int dn = Differ(skin.postSkinNormalVerts, __state.psnv), dd = 0, dw = 0, dmove = 0;
            for (int i = 0; i < __state.moves.Length; i++)
            {
                DAZPhysicsMeshSoftVerticesSet s = __state.sets[i];
                dd += Differ(s.influenceVerticesDistances, __state.dist[i]);
                dw += Differ(s.influenceVerticesWeights, __state.wts[i]);
                if (!Same(s.move.x, __state.moves[i].x) || !Same(s.move.y, __state.moves[i].y) || !Same(s.move.z, __state.moves[i].z)) dmove++;
            }
            Verified(softStat, "MorphVerticesThreadedFast", dm + dv + dn + dd + dw + dmove, "post-skin morphs " + dm + ", flags " + dv +
                "/" + dn + ", distances " + dd + ", weights " + dw + ", moves " + dmove);
        }
        catch (Exception e) { Fail(softStat, "MorphVerticesThreadedFast", e); }
    }
}
