// Temporary observer (taken out of BepInEx\plugins after the run): how much CPU time each of our plugins takes, and on
// which threads. Every way VaM and Unity enter a plugin assembly in BepInEx\plugins is timed with a Harmony prefix and
// finalizer: its MonoBehaviours' Unity messages, its coroutines, its Harmony prefixes / postfixes / finalizers on any
// method, the static methods its transpilers splice into VaM's code (found from the strings and method tokens in the
// transpilers' IL), and its camera callbacks. Time is inclusive and counted once per plugin and thread: a plugin's
// entry called from inside that plugin's own entry adds nothing. Threads the plugins start themselves (solve threads,
// native pools) are not entries; tools\thread_census.py counts them from outside.
//     Arm(dir, delay s, seconds)  then  Finish()  -> dir\census.json, dir\census-frames.csv
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Text;
using System.Threading;
using BepInEx;
using HarmonyLib;
using UnityEngine;

[BepInPlugin("local.vam.cpuboost.census", "CPU census probe", "1.0.0")]
public sealed class CensusProbe : BaseUnityPlugin
{
    const BindingFlags All = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;
    const int Rows = 4096;
    static readonly string[] Messages = { "Update", "LateUpdate", "FixedUpdate", "OnPreCull", "OnPreRender", "OnPostRender",
        "OnRenderImage", "OnRenderObject", "OnWillRenderObject", "OnGUI", "OnBecameVisible", "OnAnimatorIK", "OnAnimatorMove" };

    static Harmony harmony;
    static readonly List<string> plugins = new List<string>();          // assembly names
    static readonly Dictionary<Assembly, int> pluginOf = new Dictionary<Assembly, int>();
    static readonly Dictionary<IntPtr, int> slotOf = new Dictionary<IntPtr, int>();   // method handle -> slot
    static readonly List<MethodBase> slots = new List<MethodBase>();
    static readonly List<string> slotKind = new List<string>();
    static int[] slotPlugin = new int[0];
    static long[] mainTicks = new long[0], otherTicks = new long[0], mainCalls = new long[0], otherCalls = new long[0];
    static long[] pluginMainTicks = new long[0];
    static readonly Dictionary<string, long[]> otherByThread = new Dictionary<string, long[]>();   // thread -> per plugin
    static int mainId;
    static string failures = "";
    static int patchFailures;

    [ThreadStatic] static int[] depth;
    [ThreadStatic] static long[] started;
    [ThreadStatic] static int[] outer;
    [ThreadStatic] static long[] mine;      // this thread's per-plugin ticks (other threads), merged at Finish

    static readonly List<long[]> threadTables = new List<long[]>();
    static readonly List<string> threadNames = new List<string>();

    static bool armed, counting, done;
    static double begin, end;
    static string directory;
    static int frames;
    static double[][] rows;
    static long[] lastPluginMain;
    static double overheadTicks;

    static double Clock() { return Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency; }
    static string F(double v) { return v.ToString("0.####", CultureInfo.InvariantCulture); }

    void Start()
    {
        mainId = Thread.CurrentThread.ManagedThreadId;
        harmony = new Harmony("local.vam.cpuboost.census");
        Discover();
        Logger.LogInfo("census: " + slots.Count + " entries in " + plugins.Count + " plugins, " + patchFailures + " not patchable" + failures);
    }

    static bool Ours(Assembly a)
    {
        if (a == typeof(CensusProbe).Assembly) return false;
        string loc;
        try { loc = a.Location; } catch { return false; }
        if (string.IsNullOrEmpty(loc)) return false;
        string dir = Path.GetDirectoryName(Path.GetFullPath(loc));
        return string.Equals(dir, Path.GetFullPath(Paths.PluginPath), StringComparison.OrdinalIgnoreCase) &&
               !Path.GetFileName(loc).EndsWith("Probe.dll", StringComparison.OrdinalIgnoreCase);
    }

    static int Plugin(Assembly a)
    {
        int p;
        if (pluginOf.TryGetValue(a, out p)) return p;
        p = plugins.Count; plugins.Add(a.GetName().Name); pluginOf[a] = p;
        return p;
    }

    static Type[] Types(Assembly a)
    {
        try { return a.GetTypes(); }
        catch (ReflectionTypeLoadException e) { return e.Types.Where(t => t != null).ToArray(); }
    }

    // Finds the entries not timed yet and patches them; safe to call again (new patches applied since).
    static int Discover()
    {
        var found = new List<KeyValuePair<MethodBase, string>>();
        var ours = AppDomain.CurrentDomain.GetAssemblies().Where(Ours).ToList();
        foreach (Assembly a in ours)
            foreach (Type t in Types(a))
            {
                if (t.IsGenericTypeDefinition) continue;
                if (typeof(MonoBehaviour).IsAssignableFrom(t))
                    foreach (string m in Messages)
                    {
                        MethodInfo mi = t.GetMethod(m, All, null, Type.EmptyTypes, null);
                        if (mi != null) found.Add(new KeyValuePair<MethodBase, string>(mi, "message"));
                    }
                if (typeof(IEnumerator).IsAssignableFrom(t) && t.Name.StartsWith("<"))
                {
                    MethodInfo mn = t.GetMethod("MoveNext", All, null, Type.EmptyTypes, null);
                    if (mn != null) found.Add(new KeyValuePair<MethodBase, string>(mn, "coroutine"));
                }
            }
        foreach (MethodBase orig in Harmony.GetAllPatchedMethods().ToList())
        {
            Patches info = Harmony.GetPatchInfo(orig);
            if (info == null) continue;
            foreach (Patch p in info.Prefixes.Concat(info.Postfixes).Concat(info.Finalizers))
                if (p.GetMethod(orig) != null && Ours(p.GetMethod(orig).DeclaringType.Assembly) && p.owner != harmony.Id)
                    found.Add(new KeyValuePair<MethodBase, string>(p.GetMethod(orig), "patch " + orig.DeclaringType.Name + "." + orig.Name));
            foreach (Patch p in info.Transpilers)
                if (p.GetMethod(orig) != null && Ours(p.GetMethod(orig).DeclaringType.Assembly))
                    foreach (MethodBase m in Spliced(p.GetMethod(orig)))
                        found.Add(new KeyValuePair<MethodBase, string>(m, "spliced into " + orig.DeclaringType.Name + "." + orig.Name));
        }
        foreach (string ev in new[] { "onPreCull", "onPreRender", "onPostRender" })
        {
            FieldInfo f = typeof(Camera).GetField(ev, BindingFlags.Public | BindingFlags.Static);
            Delegate d = f == null ? null : f.GetValue(null) as Delegate;
            if (d != null)
                foreach (Delegate x in d.GetInvocationList())
                    if (Ours(x.Method.DeclaringType.Assembly)) found.Add(new KeyValuePair<MethodBase, string>(x.Method, "camera " + ev));
        }
        int added = 0;
        var pre = new HarmonyMethod(typeof(CensusProbe).GetMethod("Enter", BindingFlags.NonPublic | BindingFlags.Static));
        var fin = new HarmonyMethod(typeof(CensusProbe).GetMethod("Leave", BindingFlags.NonPublic | BindingFlags.Static));
        foreach (var kv in found)
        {
            MethodBase m = kv.Key;
            if (m.IsAbstract || m.ContainsGenericParameters || m.GetMethodBody() == null) continue;
            IntPtr h = m.MethodHandle.Value;
            if (slotOf.ContainsKey(h)) continue;
            int slot = slots.Count;
            slots.Add(m); slotKind.Add(kv.Value);
            Array.Resize(ref slotPlugin, slot + 1); slotPlugin[slot] = Plugin(m.DeclaringType.Assembly);
            slotOf[h] = slot;
        }
        Grow();
        for (int s = 0; s < slots.Count; s++)
        {
            if (slotKind[s].EndsWith("\u0000")) continue;   // patched (or failed) before
            try { harmony.Patch(slots[s], pre, null, null, fin); added++; }
            catch (Exception e) { patchFailures++; if (failures.Length < 2000) failures += "; " + Name(s) + ": " + e.GetType().Name; }
            slotKind[s] = slotKind[s] + "\u0000";
        }
        return added;
    }

    // static methods of the transpiler's own assembly that its IL names (strings, ldftn / ldtoken): what it splices in
    static IEnumerable<MethodBase> Spliced(MethodInfo transpiler)
    {
        var names = new HashSet<string>();
        var direct = new List<MethodBase>();
        byte[] il;
        try { il = transpiler.GetMethodBody().GetILAsByteArray(); } catch { yield break; }
        Module mod = transpiler.Module;
        for (int i = 0; i < il.Length;)
        {
            OpCode op;
            if (il[i] == 0xFE) { op = Two[il[i + 1]]; i += 2; } else { op = One[il[i]]; i += 1; }
            int size = Operand(op, il, i);
            if (op == OpCodes.Ldstr) { try { names.Add(mod.ResolveString(BitConverter.ToInt32(il, i))); } catch { } }
            else if ((op == OpCodes.Ldftn || op == OpCodes.Ldtoken) && op.OperandType != OperandType.InlineType)
                try { MethodBase m = mod.ResolveMethod(BitConverter.ToInt32(il, i)); if (m != null) direct.Add(m); } catch { }
            i += size;
        }
        Assembly a = transpiler.DeclaringType.Assembly;
        foreach (MethodBase m in direct) if (m.IsStatic && m.DeclaringType.Assembly == a) yield return m;
        foreach (Type t in Types(a))
            foreach (MethodInfo m in t.GetMethods(All))
                if (m.IsStatic && names.Contains(m.Name) && m != transpiler && !m.Name.StartsWith("<")) yield return m;
    }

    static readonly OpCode[] One = new OpCode[256], Two = new OpCode[256];
    static CensusProbe()
    {
        foreach (FieldInfo f in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
        {
            OpCode o = (OpCode)f.GetValue(null);
            ushort v = (ushort)o.Value;
            if (o.Size == 1) One[v] = o; else Two[v & 0xFF] = o;
        }
    }
    static int Operand(OpCode op, byte[] il, int i)
    {
        switch (op.OperandType)
        {
            case OperandType.InlineNone: return 0;
            case OperandType.ShortInlineBrTarget: case OperandType.ShortInlineI: case OperandType.ShortInlineVar: return 1;
            case OperandType.InlineVar: return 2;
            case OperandType.InlineI8: case OperandType.InlineR: return 8;
            case OperandType.InlineSwitch: return 4 + 4 * BitConverter.ToInt32(il, i);
            default: return 4;
        }
    }

    static void Grow()
    {
        int n = slots.Count, p = plugins.Count;
        Array.Resize(ref mainTicks, n); Array.Resize(ref otherTicks, n); Array.Resize(ref mainCalls, n); Array.Resize(ref otherCalls, n);
        Array.Resize(ref pluginMainTicks, p);
    }

    static string Name(int s) { MethodBase m = slots[s]; return m.DeclaringType.FullName + "." + m.Name; }

    // ---- the timers --------------------------------------------------------------------------------------------
    static void Enter(MethodBase __originalMethod)
    {
        int s;
        if (!counting || __originalMethod == null || !slotOf.TryGetValue(__originalMethod.MethodHandle.Value, out s)) return;
        if (depth == null) Thread0();
        int p = slotPlugin[s];
        if (p >= MaxPlugins) return;
        if (depth[p]++ == 0) { outer[p] = s; started[p] = Stopwatch.GetTimestamp(); }
    }

    static void Leave(MethodBase __originalMethod)
    {
        int s;
        if (depth == null || __originalMethod == null || !slotOf.TryGetValue(__originalMethod.MethodHandle.Value, out s)) return;
        int p = slotPlugin[s];
        if (p >= depth.Length || depth[p] == 0) return;
        if (--depth[p] != 0) return;
        long dt = Stopwatch.GetTimestamp() - started[p];
        int o = outer[p];
        if (Thread.CurrentThread.ManagedThreadId == mainId) { mainTicks[o] += dt; mainCalls[o]++; pluginMainTicks[p] += dt; }
        else { Interlocked.Add(ref otherTicks[o], dt); Interlocked.Increment(ref otherCalls[o]); mine[p] += dt; }
    }

    const int MaxPlugins = 64;
    static void Thread0()
    {
        int p = MaxPlugins;
        depth = new int[p]; started = new long[p]; outer = new int[p];
        if (Thread.CurrentThread.ManagedThreadId != mainId)
        {
            mine = new long[p];
            lock (threadTables)
            {
                threadTables.Add(mine);
                Thread t = Thread.CurrentThread;
                threadNames.Add((string.IsNullOrEmpty(t.Name) ? "thread" : t.Name) + " #" + t.ManagedThreadId);
            }
        }
    }

    // a frame of the wrapper alone, for the cost per call that the counts carry
    static void Nop() { }
    static double MeasureOverhead()
    {
        MethodInfo nop = typeof(CensusProbe).GetMethod("Nop", BindingFlags.NonPublic | BindingFlags.Static);
        Action call = (Action)Delegate.CreateDelegate(typeof(Action), nop);
        long t0 = Stopwatch.GetTimestamp();
        for (int i = 0; i < 20000; i++) call();
        long bare = Stopwatch.GetTimestamp() - t0;
        // the wrapper of a timed slot: count it under a scratch plugin that is never reported
        int s = slots.Count; slots.Add(nop); slotKind.Add("calibration"); Array.Resize(ref slotPlugin, s + 1);
        slotPlugin[s] = plugins.Count; plugins.Add("(calibration)"); Grow();
        slotOf[nop.MethodHandle.Value] = s;
        harmony.Patch(nop, new HarmonyMethod(typeof(CensusProbe).GetMethod("Enter", BindingFlags.NonPublic | BindingFlags.Static)), null, null,
                      new HarmonyMethod(typeof(CensusProbe).GetMethod("Leave", BindingFlags.NonPublic | BindingFlags.Static)));
        bool was = counting; counting = true;
        depth = null;
        t0 = Stopwatch.GetTimestamp();
        for (int i = 0; i < 20000; i++) call();
        long wrapped = Stopwatch.GetTimestamp() - t0;
        counting = was;
        mainTicks[s] = mainCalls[s] = 0; pluginMainTicks[slotPlugin[s]] = 0;
        return (wrapped - bare) / 20000.0;
    }

    // ---- the window --------------------------------------------------------------------------------------------
    public static string Arm(string dir, float delay, float seconds)
    {
        if (armed) return "already armed";
        int more = Discover();
        if (overheadTicks == 0) overheadTicks = MeasureOverhead();
        directory = dir; Directory.CreateDirectory(dir);
        begin = Clock() + delay; end = begin + seconds;
        rows = new double[Rows][];
        frames = 0; armed = true; done = false;
        return "armed: " + slots.Count + " entries (" + more + " new) in " + plugins.Count + " plugins, wrapper " +
               F(overheadTicks * 1e6 / Stopwatch.Frequency) + " us a call, window " + F(begin) + ".." + F(end);
    }

    void Update()
    {
        if (!armed) return;
        double now = Clock();
        if (!counting && now >= begin && now < end)
        {
            Array.Clear(mainTicks, 0, mainTicks.Length); Array.Clear(otherTicks, 0, otherTicks.Length);
            Array.Clear(mainCalls, 0, mainCalls.Length); Array.Clear(otherCalls, 0, otherCalls.Length);
            Array.Clear(pluginMainTicks, 0, pluginMainTicks.Length);
            lock (threadTables) foreach (long[] t in threadTables) Array.Clear(t, 0, t.Length);
            lastPluginMain = new long[pluginMainTicks.Length];
            for (int f = 0; f < Rows; f++) rows[f] = new double[pluginMainTicks.Length + 1];
            begin = now; counting = true; return;
        }
        if (!counting) return;
        if (frames < Rows)
        {
            double[] r = rows[frames];
            r[0] = now;
            for (int p = 0; p < pluginMainTicks.Length; p++)
            { r[p + 1] = (pluginMainTicks[p] - lastPluginMain[p]) * 1000.0 / Stopwatch.Frequency; lastPluginMain[p] = pluginMainTicks[p]; }
        }
        frames++;
        if (now >= end) { counting = false; armed = false; done = true; end = now; }
    }

    public static string Finish()
    {
        if (!done) return "window not finished (" + frames + " frames)";
        double ms = 1000.0 / Stopwatch.Frequency, seconds = end - begin, perFrame = 1.0 / Math.Max(frames, 1);
        double ov = overheadTicks * ms;
        var sb = new StringBuilder();
        sb.Append("{\n \"frames\": ").Append(frames).Append(", \"seconds\": ").Append(F(seconds));
        sb.Append(", \"wrapper_us_per_call\": ").Append(F(ov * 1000)).Append(", \"patch_failures\": ").Append(patchFailures);
        sb.Append(", \"failures\": \"").Append(failures.Replace("\"", "'").Replace("\\", "/")).Append("\",\n \"plugins\": [\n");
        bool firstP = true;
        for (int p = 0; p < plugins.Count; p++)
        {
            if (plugins[p] == "(calibration)") continue;
            double main = 0, other = 0, mc = 0, oc = 0;
            for (int s = 0; s < slots.Count; s++)
                if (slotPlugin[s] == p) { main += mainTicks[s] * ms; other += otherTicks[s] * ms; mc += mainCalls[s]; oc += otherCalls[s]; }
            if (!firstP) sb.Append(",\n"); firstP = false;
            sb.Append("  {\"plugin\": \"").Append(plugins[p]).Append("\", \"main_ms_per_frame\": ").Append(F(main * perFrame));
            sb.Append(", \"main_calls_per_frame\": ").Append(F(mc * perFrame));
            sb.Append(", \"main_wrapper_ms_per_frame\": ").Append(F(mc * perFrame * ov * 2));
            sb.Append(", \"other_threads_ms_per_frame\": ").Append(F(other * perFrame));
            sb.Append(", \"other_calls_per_frame\": ").Append(F(oc * perFrame));
            sb.Append(", \"other_threads\": {");
            bool firstT = true;
            lock (threadTables)
                for (int t = 0; t < threadTables.Count; t++)
                    if (p < threadTables[t].Length && threadTables[t][p] > 0)
                    {
                        if (!firstT) sb.Append(", "); firstT = false;
                        sb.Append("\"").Append(threadNames[t].Replace("\"", "'")).Append("\": ").Append(F(threadTables[t][p] * ms * perFrame));
                    }
            sb.Append("}, \"entries\": [");
            var mine2 = Enumerable.Range(0, slots.Count).Where(s => slotPlugin[s] == p && (mainCalls[s] + otherCalls[s]) > 0)
                .OrderByDescending(s => mainTicks[s] + otherTicks[s]).ToList();
            for (int k = 0; k < mine2.Count; k++)
            {
                int s = mine2[k];
                if (k > 0) sb.Append(", ");
                sb.Append("{\"method\": \"").Append(Name(s).Replace("\"", "'")).Append("\", \"kind\": \"").Append(slotKind[s].TrimEnd('\u0000').Replace("\"", "'"));
                sb.Append("\", \"main_ms\": ").Append(F(mainTicks[s] * ms * perFrame)).Append(", \"main_calls\": ").Append(F(mainCalls[s] * perFrame));
                sb.Append(", \"other_ms\": ").Append(F(otherTicks[s] * ms * perFrame)).Append(", \"other_calls\": ").Append(F(otherCalls[s] * perFrame)).Append("}");
            }
            sb.Append("]}");
        }
        sb.Append("\n ],\n \"entries_timed\": ").Append(slots.Count).Append("\n}\n");
        File.WriteAllText(Path.Combine(directory, "census.json"), sb.ToString());
        var csv = new StringBuilder("t");
        foreach (string p in plugins) csv.Append(",").Append(p);
        csv.Append("\n");
        for (int f = 0; f < Math.Min(frames, Rows); f++)
        {
            if (rows[f] == null) continue;
            csv.Append(F(rows[f][0]));
            for (int p = 1; p < rows[f].Length; p++) csv.Append(",").Append(F(rows[f][p]));
            csv.Append("\n");
        }
        File.WriteAllText(Path.Combine(directory, "census-frames.csv"), csv.ToString());
        return "written: " + frames + " frames, " + F(seconds) + " s";
    }
}
