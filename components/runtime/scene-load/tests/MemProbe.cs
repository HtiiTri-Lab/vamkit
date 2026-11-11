// Test-only probe: what memory and which caches a scene load / clear / in-scene load builds and frees.
// Goes into BepInEx\plugins only for a test run (the test script puts it there and takes it out).
// Snap(label, path, typeNames): one line appended to path and returned:
//   - managed: GC.GetTotalMemory, Boehm's heap size / large free / bytes allocated since start / collections (mono.dll
//     2018-07 layout);
//   - Unity: Profiler totals (0 in a release player is reported as 0), and for every loaded object of a type
//     (Resources.FindObjectsOfTypeAll): Texture2D (count, estimated bytes from size / format / mips; VaM's own are all readable: a CPU copy each),
//     RenderTexture, Cubemap, Mesh (count, vertices, readable), AudioClip (count, loaded, PCM bytes of loaded
//     DecompressOnLoad clips), Material, Shader, GameObject, AssetBundle, MonoBehaviour; loaded assemblies;
//   - VaM: atoms (dictionary / list / starting), the atom pool per type, AssetLoader's bundle / count tables;
//   - every collection field (static, or on the type's static "singleton") of each type in typeNames: its Count;
//   - VaM Scene Load's Stats().
// Clear fixes (on from Awake, test only; counted in Fixes()): VaM aborts a scene clear when a plugin's
//   onAtomRemovedHandlers throws (a timeline plugin: VamTimeline.AtomPlugin.OnAtomRemoved NullReferenceException out of
//   RemoveNonStartingAtoms, the load never starts), and fails the next load when a starting atom that was removed is
//   still in startingAtoms (RemoveNonStartingAtoms rebuilds atomsList from it, LoadCo copies atomsList into an array of
//   atoms.Count: "Destination array was not long enough"). Here: each removed-handler runs on its own
//   (an exception is logged the way Atom.OnPreRemove logs its storables'), and RemoveNonStartingAtoms first drops
//   starting atoms that are no longer the scene's (in the pool or destroyed; a live one missing from the dictionary is
//   put back into it), each logged; LoadInternal runs the same check first (it copies atomsList the same way).
//   SubScene.PreRemove unparents a starting atom instead of removing it (a stage scene parents WindowCamera into a subscene).
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Profiling;

[BepInPlugin("local.vam.test.memprobe", "Memory probe (test only)", "1.0.0")]
public unsafe class MemProbe : BaseUnityPlugin
{
    const BindingFlags inst = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    const BindingFlags stat = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;

    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m, string name);

    static byte* mono;
    static long* wordsBefore, wordsSince, heapSize, largeFree;
    static int* gcNo;
    static string monoState = "not checked";
    static BepInEx.Logging.ManualLogSource log;
    static int handlerErrors, startingDropped, startingRepaired;
    static readonly StringBuilder fixLog = new StringBuilder();

    static long RipTarget(int rva, int dispOffset, int insLen) { return rva + insLen + *(int*)(mono + rva + dispOffset); }

    void Awake()
    {
        log = Logger;
        IntPtr m = GetModuleHandleW("mono.dll");
        if (m != IntPtr.Zero) {
            mono = (byte*)m.ToPointer();
            bool ok = mono[0x158dc4] == 0x48 && mono[0x158dc5] == 0x8B && mono[0x158dc6] == 0x05 && RipTarget(0x158dc4, 3, 7) == 0x27c828
                   && mono[0x158dd0] == 0x48 && RipTarget(0x158dd0, 3, 7) == 0x27c820;
            IntPtr cc = GetProcAddress(m, "mono_gc_collection_count");
            if (ok && cc != IntPtr.Zero) {
                byte* f = (byte*)cc.ToPointer(); long rva = f - mono;
                ok = f[0] == 0x8B && f[1] == 0x05 && rva + 6 + *(int*)(f + 2) == 0x269b90;
            } else ok = false;
            if (ok) {
                wordsBefore = (long*)(mono + 0x27c820); wordsSince = (long*)(mono + 0x27c828);
                heapSize = (long*)(mono + 0x27c7e0); largeFree = (long*)(mono + 0x27c808); gcNo = (int*)(mono + 0x269b90);
                monoState = "ok";
            } else { monoState = "mono.dll layout differs"; mono = null; }
        } else monoState = "no mono.dll";
        // VaM Scene Load 1.6.0 has these fixes itself (SceneSwitch.cs): the probe's stay off then
        try {
            string sl = Path.Combine(Path.GetDirectoryName(Info.Location), "VaMSceneLoad.dll");
            if (File.Exists(sl) && Encoding.ASCII.GetString(File.ReadAllBytes(sl)).Contains("SwitchStats")) { Logger.LogInfo("Memory probe: mono " + monoState + ", clear fixes off (VaM Scene Load has them)"); return; }
        } catch { }
        try {
            Harmony h = new Harmony("local.vam.test.memprobe");
            MethodInfo rna = typeof(SuperController).GetMethod("RemoveNonStartingAtoms", inst, null, Type.EmptyTypes, null);
            MethodInfo ra = typeof(SuperController).GetMethod("RemoveAtom", inst, null, new[] { typeof(Atom), typeof(bool) }, null);
            MethodInfo li = typeof(SuperController).GetMethod("LoadInternal", inst, null, new[] { typeof(string), typeof(bool), typeof(bool) }, null);
            MethodInfo spr = typeof(SubScene).GetMethod("PreRemove", inst, null, Type.EmptyTypes, null);
            h.Patch(rna, new HarmonyMethod(typeof(MemProbe).GetMethod("StartingPrefix", stat)), null, null, null);
            h.Patch(li, new HarmonyMethod(typeof(MemProbe).GetMethod("StartingPrefix", stat)), null, null, null);
            h.Patch(spr, new HarmonyMethod(typeof(MemProbe).GetMethod("SubScenePreRemove", stat)), null, null, null);
            h.Patch(ra, null, null, new HarmonyMethod(typeof(MemProbe).GetMethod("RemoveAtomTranspiler", stat)), null);
            Logger.LogInfo("Memory probe: mono " + monoState + ", clear fixes on");
        } catch (Exception e) { Logger.LogError("Memory probe: clear fixes failed: " + e); }
    }

    // ---- clear fixes -------------------------------------------------------------------------------------------------

    public static IEnumerable<CodeInstruction> RemoveAtomTranspiler(IEnumerable<CodeInstruction> code)
    {
        MethodInfo invoke = typeof(SuperController.OnAtomRemoved).GetMethod("Invoke");
        MethodInfo safe = typeof(MemProbe).GetMethod("InvokeRemoved", stat);
        int n = 0;
        foreach (CodeInstruction c in code) {
            if ((c.opcode == OpCodes.Callvirt || c.opcode == OpCodes.Call) && c.operand as MethodInfo == invoke) {
                c.opcode = OpCodes.Call; c.operand = safe; n++;
            }
            yield return c;
        }
        if (n != 1) log.LogWarning("Memory probe: RemoveAtom had " + n + " OnAtomRemoved invokes");
    }

    public static void InvokeRemoved(SuperController.OnAtomRemoved handlers, Atom atom)
    {
        foreach (Delegate d in handlers.GetInvocationList()) {
            try { ((SuperController.OnAtomRemoved)d)(atom); }
            catch (Exception e) {
                handlerErrors++;
                SuperController.LogError("Exception during atom removed handler " + (d.Target != null ? d.Target.GetType().FullName : "?") + "." + d.Method.Name + " for " + (atom != null ? atom.uid : "null") + ": " + e);
            }
        }
    }

    // SubScene.PreRemove removes every atom in the subscene; a stage scene parents the starting WindowCamera atom into a
    // subscene, so a scene clear destroyed a starting atom (still in startingAtoms: every later load fails).
    // Here a starting atom in a subscene that goes away is only unparented (what RemoveAtom does for any other child).
    public static bool SubScenePreRemove(SubScene __instance)
    {
        try {
            SuperController s = SuperController.singleton;
            HashSet<Atom> starting = (HashSet<Atom>)typeof(SuperController).GetField("startingAtoms", inst).GetValue(s);
            IEnumerable inSub = typeof(SubScene).GetField("_atomsInSubScene", inst).GetValue(__instance) as IEnumerable;
            if (starting == null || inSub == null) return true;
            List<Atom> copy = new List<Atom>();
            foreach (object o in inSub) copy.Add(o as Atom);
            foreach (Atom a in copy) {
                if (a != null && starting.Contains(a)) {
                    a.parentAtom = null;
                    startingKept++;
                    fixLog.Append("kept starting ").Append(a.uid).Append(" out of subscene ").Append(__instance.containingAtom != null ? __instance.containingAtom.uid : "?").Append("; ");
                    log.LogWarning("Memory probe: starting atom " + a.uid + " unparented instead of removed with its subscene");
                } else s.RemoveAtom(a);
            }
            return false;
        } catch (Exception e) { log.LogError("Memory probe: subscene remove fix failed: " + e); return true; }
    }

    public static void StartingPrefix(SuperController __instance)
    {
        try {
            HashSet<Atom> starting = (HashSet<Atom>)typeof(SuperController).GetField("startingAtoms", inst).GetValue(__instance);
            Dictionary<string, Atom> atoms = (Dictionary<string, Atom>)typeof(SuperController).GetField("atoms", inst).GetValue(__instance);
            if (starting == null || atoms == null) return;
            List<Atom> drop = null;
            foreach (Atom a in starting) {
                if (a == null) continue;
                Atom b;
                if (atoms.TryGetValue(a.uid, out b) && ReferenceEquals(a, b)) continue;
                string what = a.uid + " (" + a.type + ", inPool " + a.inPool + ", destroyed " + a.destroyed + ", active " + a.gameObject.activeSelf + ", dictionary has " + (b != null ? b.type : "none") + ")";
                if (a.inPool || a.destroyed || b != null) {
                    (drop ?? (drop = new List<Atom>())).Add(a);
                    startingDropped++;
                    fixLog.Append("dropped starting ").Append(what).Append("; ");
                    log.LogWarning("Memory probe: starting atom no longer in the scene dropped: " + what);
                } else {
                    atoms[a.uid] = a;
                    startingRepaired++;
                    fixLog.Append("repaired ").Append(what).Append("; ");
                    log.LogWarning("Memory probe: starting atom missing from the atom table put back: " + what);
                }
            }
            if (drop != null) {
                List<Atom> list = (List<Atom>)typeof(SuperController).GetField("atomsList", inst).GetValue(__instance);
                foreach (Atom a in drop) { starting.Remove(a); if (list != null) list.Remove(a); }
            }
        } catch (Exception e) { log.LogError("Memory probe: starting atoms check failed: " + e); }
    }

    static int startingKept;

    public static string Fixes()
    {
        return "handler errors " + handlerErrors + ", starting kept " + startingKept + ", dropped " + startingDropped + ", repaired " + startingRepaired + (fixLog.Length > 0 ? " | " + fixLog : "");
    }

    // ---- snapshot ----------------------------------------------------------------------------------------------------

    static double Bpp(TextureFormat f)
    {
        switch (f) {
            case TextureFormat.DXT1: case TextureFormat.BC4: case TextureFormat.DXT1Crunched: return 0.5;
            case TextureFormat.DXT5: case TextureFormat.BC5: case TextureFormat.BC6H: case TextureFormat.BC7: case TextureFormat.DXT5Crunched: return 1;
            case TextureFormat.Alpha8: case TextureFormat.R8: return 1;
            case TextureFormat.RGB24: return 3;
            case TextureFormat.RGBAHalf: return 8;
            case TextureFormat.RGBAFloat: return 16;
            case TextureFormat.RHalf: case TextureFormat.RG16: case TextureFormat.R16: case TextureFormat.RGB565: case TextureFormat.ARGB4444: case TextureFormat.RGBA4444: return 2;
            case TextureFormat.RGHalf: case TextureFormat.RFloat: return 4;
            case TextureFormat.RGFloat: return 8;
            default: return 4;
        }
    }

    static long MipBytes(int w, int h, int mips, double bpp, int faces)
    {
        double s = 0;
        for (int i = 0; i < Math.Max(1, mips); i++) { s += Math.Max(1, w >> i) * (double)Math.Max(1, h >> i) * bpp; }
        return (long)(s * faces);
    }

    static double GB(double b) { return Math.Round(b / 1073741824.0, 3); }

    static int Count(object o)
    {
        ICollection c = o as ICollection;
        if (c != null) return c.Count;
        if (o == null) return -1;
        PropertyInfo p = o.GetType().GetProperty("Count", BindingFlags.Instance | BindingFlags.Public);
        return p != null && p.PropertyType == typeof(int) ? (int)p.GetValue(o, null) : -2;
    }

    static Type Find(string name)
    {
        foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies()) {
            Type t = a.GetType(name, false);
            if (t != null) return t;
        }
        return null;
    }

    static void Collections(StringBuilder sb, string typeName)
    {
        Type t = Find(typeName);
        if (t == null) { sb.Append(" | ").Append(typeName).Append(" missing"); return; }
        object single = null;
        FieldInfo sf = t.GetField("singleton", stat) ?? t.GetField("_singleton", stat);
        if (sf != null) single = sf.GetValue(null);
        else { PropertyInfo sp = t.GetProperty("singleton", stat); if (sp != null) single = sp.GetValue(null, null); }
        sb.Append(" | ").Append(t.Name);
        foreach (FieldInfo f in t.GetFields(stat)) {
            if (!typeof(IEnumerable).IsAssignableFrom(f.FieldType) || f.FieldType == typeof(string)) continue;
            sb.Append(' ').Append(f.Name).Append('=').Append(Count(f.GetValue(null)));
        }
        if (single != null)
            foreach (FieldInfo f in t.GetFields(inst)) {
                if (!typeof(IEnumerable).IsAssignableFrom(f.FieldType) || f.FieldType == typeof(string) || f.FieldType.IsArray) continue;
                sb.Append(" .").Append(f.Name).Append('=').Append(Count(f.GetValue(single)));
            }
    }

    public static string Snap(string label, string path, string typeNames)
    {
        float t0 = Time.realtimeSinceStartup;
        StringBuilder sb = new StringBuilder();
        sb.Append(label).Append(" t=").Append(t0.ToString("F1")).Append(" frame=").Append(Time.frameCount);
        sb.Append(" | managed ").Append(GB(GC.GetTotalMemory(false)));
        if (mono != null)
            sb.Append(" boehm_heap ").Append(GB(*heapSize)).Append(" large_free ").Append(GB(*largeFree)).Append(" allocated_total ").Append(GB((wordsBefore[0] + wordsSince[0]) * 8.0))
              .Append(" gc_no ").Append(gcNo[0]);
        else sb.Append(" (").Append(monoState).Append(')');
        sb.Append(" | unity allocated ").Append(GB(Profiler.GetTotalAllocatedMemoryLong())).Append(" reserved ").Append(GB(Profiler.GetTotalReservedMemoryLong()))
          .Append(" gfx_driver ").Append(GB(Profiler.GetAllocatedMemoryForGraphicsDriver()));
        try {
            long tb = 0; int tn = 0;
            foreach (Texture2D tx in Resources.FindObjectsOfTypeAll<Texture2D>()) {
                long b = MipBytes(tx.width, tx.height, tx.mipmapCount, Bpp(tx.format), 1);
                tn++; tb += b;
            }
            sb.Append(" | Texture2D ").Append(tn).Append(' ').Append(GB(tb)).Append(" GB");
            long cb = 0; int cn = 0;
            foreach (Cubemap c in Resources.FindObjectsOfTypeAll<Cubemap>()) { cn++; cb += MipBytes(c.width, c.height, c.mipmapCount, Bpp(c.format), 6); }
            sb.Append(" Cubemap ").Append(cn).Append(' ').Append(GB(cb));
            long rb = 0; int rn = 0;
            foreach (RenderTexture r in Resources.FindObjectsOfTypeAll<RenderTexture>()) {
                rn++;
                int bpp = r.format == RenderTextureFormat.ARGBHalf ? 8 : r.format == RenderTextureFormat.ARGBFloat ? 16 : r.format == RenderTextureFormat.Depth ? 4 : 4;
                rb += (long)r.width * r.height * Math.Max(1, r.volumeDepth) * bpp * Math.Max(1, r.antiAliasing) + (r.depth > 0 ? (long)r.width * r.height * 4 * Math.Max(1, r.antiAliasing) : 0);
            }
            sb.Append(" RenderTexture ").Append(rn).Append(' ').Append(GB(rb));
            long mv = 0; int mn = 0, mr = 0;
            foreach (Mesh me in Resources.FindObjectsOfTypeAll<Mesh>()) { mn++; mv += me.vertexCount; if (me.isReadable) mr++; }
            sb.Append(" Mesh ").Append(mn).Append(" verts ").Append(mv).Append(" readable ").Append(mr);
            long ab = 0, abOther = 0; int an = 0, al = 0, ad = 0;
            foreach (AudioClip a in Resources.FindObjectsOfTypeAll<AudioClip>()) {
                an++;
                if (a.loadState != AudioDataLoadState.Loaded) continue;
                al++;
                long b = (long)a.samples * a.channels * 2;
                if (a.loadType == AudioClipLoadType.DecompressOnLoad) { ad++; ab += b; } else abOther += b;
            }
            sb.Append(" AudioClip ").Append(an).Append(" loaded ").Append(al).Append(" decompressed ").Append(ad).Append(' ').Append(GB(ab)).Append(" GB (other loaded pcm-equivalent ").Append(GB(abOther)).Append(')');
            sb.Append(" Material ").Append(Resources.FindObjectsOfTypeAll<Material>().Length);
            sb.Append(" Shader ").Append(Resources.FindObjectsOfTypeAll<Shader>().Length);
            sb.Append(" GameObject ").Append(Resources.FindObjectsOfTypeAll<GameObject>().Length);
            sb.Append(" AssetBundle ").Append(Resources.FindObjectsOfTypeAll<AssetBundle>().Length);
            sb.Append(" MonoBehaviour ").Append(Resources.FindObjectsOfTypeAll<MonoBehaviour>().Length);
        } catch (Exception e) { sb.Append(" | objects failed ").Append(e.GetType().Name).Append(' ').Append(e.Message); }
        try {
            SuperController s = SuperController.singleton;
            Dictionary<string, Atom> atoms = (Dictionary<string, Atom>)typeof(SuperController).GetField("atoms", inst).GetValue(s);
            List<Atom> list = (List<Atom>)typeof(SuperController).GetField("atomsList", inst).GetValue(s);
            HashSet<Atom> starting = (HashSet<Atom>)typeof(SuperController).GetField("startingAtoms", inst).GetValue(s);
            int on = 0;
            foreach (Atom a in list) if (a != null && a.on) on++;
            sb.Append(" | atoms ").Append(atoms.Count).Append(" list ").Append(list.Count).Append(" on ").Append(on).Append(" starting ").Append(starting != null ? starting.Count : -1);
            int ghosts = 0;
            foreach (Atom a in list) { Atom b; if (a == null || !atoms.TryGetValue(a.uid, out b) || !ReferenceEquals(a, b)) ghosts++; }
            sb.Append(" list_not_in_table ").Append(ghosts);
            IDictionary pool = (IDictionary)typeof(SuperController).GetField("typeToAtomPool", inst).GetValue(s);
            sb.Append(" pool");
            if (pool != null) foreach (DictionaryEntry kv in pool) {
                    int n = 0;
                    foreach (Atom a in (IEnumerable)kv.Value) if (a != null && a.inPool) n++;
                    if (n > 0) sb.Append(' ').Append(kv.Key).Append('=').Append(n);
                }
        } catch (Exception e) { sb.Append(" | atoms failed ").Append(e.Message); }
        foreach (string tn in (typeNames ?? "").Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries)) {
            try { Collections(sb, tn.Trim()); } catch (Exception e) { sb.Append(" | ").Append(tn).Append(" failed ").Append(e.Message); }
        }
        try {
            Type sl = Find("VaMSceneLoad");
            MethodInfo st = sl != null ? sl.GetMethod("BundleStats", stat) : null;
            if (st != null) sb.Append(" | ").Append((string)st.Invoke(null, null));
        } catch (Exception e) { sb.Append(" | scene load stats failed ").Append(e.Message); }
        sb.Append(" | assemblies ").Append(AppDomain.CurrentDomain.GetAssemblies().Length);
        sb.Append(" | fixes ").Append(Fixes());
        sb.Append(" | snap ").Append(((Time.realtimeSinceStartup - t0) * 1000).ToString("F0")).Append(" ms");
        string line = sb.ToString();
        if (!string.IsNullOrEmpty(path)) File.AppendAllText(path, line + "\n");
        return line;
    }

    // an appearance preset onto a person, the way VaM's preset browser does it (the same call a preset browser makes)
    public static string LoadLook(string uid, string path)
    {
        Atom a = SuperController.singleton.GetAtomByUid(uid);
        if (a == null) throw new Exception("no atom " + uid);
        foreach (MeshVR.PresetManagerControl control in a.GetComponentsInChildren<MeshVR.PresetManagerControl>(true)) {
            object pm = control.GetType().GetField("pm", inst).GetValue(control);
            if (pm == null) continue;
            string folder = (string)pm.GetType().GetMethod("GetStoreFolderPath", inst).Invoke(pm, new object[] { true });
            if (folder.Replace('\\', '/').IndexOf("/Appearance", StringComparison.OrdinalIgnoreCase) < 0) continue;
            control.GetType().GetMethod("LoadPresetWithPath", inst).Invoke(control, new object[] { path });
            return "loaded " + path;
        }
        throw new Exception("Appearance preset control missing");
    }

    // ---- experiments for the load manager design ---------------------------------------------------------------

    // Keep asset bundles whose last user let go (DoneWithAssetBundleFromFile bringing the count to 0): the bundle stays
    // in VaM's table without a count, so the next request of the same path reuses it (no conflict handling: only for
    // reloading the same scene).
    static bool keepBundles, keepPatched;
    static int keptBundles;
    public static string KeepBundles(bool on)
    {
        if (!keepPatched) {
            new Harmony("local.vam.test.memprobe.keep").Patch(typeof(MeshVR.AssetLoader).GetMethod("DoneWithAssetBundleFromFile", stat), new HarmonyMethod(typeof(MemProbe).GetMethod("DonePrefix", stat)), null, null, null);
            keepPatched = true;
        }
        keepBundles = on;
        return "keep " + on + ", kept " + keptBundles;
    }
    public static bool DonePrefix(string path)
    {
        if (!keepBundles || path == null) return true;
        object al = typeof(MeshVR.AssetLoader).GetField("singleton", stat).GetValue(null);
        if (al == null) return true;
        Dictionary<string, int> counts = (Dictionary<string, int>)typeof(MeshVR.AssetLoader).GetField("assetBundleReferenceCounts", inst).GetValue(al);
        Dictionary<string, AssetBundle> bundles = (Dictionary<string, AssetBundle>)typeof(MeshVR.AssetLoader).GetField("pathToAssetBundle", inst).GetValue(al);
        int n;
        if (counts == null || bundles == null || !counts.TryGetValue(path, out n) || n != 1 || !bundles.ContainsKey(path)) return true;
        counts.Remove(path);
        keptBundles++;
        return false;
    }

    // Every person's inactive character / clothing / hair instances unloaded (DAZCharacterSelector's own
    // UnloadInactiveObjects without its morph parts and its delayed UnloadUnusedAssets); counted before.
    public static string UnloadInactive()
    {
        float t0 = Time.realtimeSinceStartup;
        int people = 0, loadedInactive = 0;
        foreach (DAZCharacterSelector s in Resources.FindObjectsOfTypeAll<DAZCharacterSelector>()) {
            if (s == null || s.gameObject.scene.name == null) continue;
            people++;
            foreach (string f in new[] { "_femaleCharacters", "_maleCharacters", "_femaleClothingItems", "_maleClothingItems", "_femaleHairItems", "_maleHairItems" }) {
                FieldInfo fi = typeof(DAZCharacterSelector).GetField(f, inst);
                IEnumerable items = fi != null ? fi.GetValue(s) as IEnumerable : null;
                if (items == null) continue;
                foreach (object o in items) {
                    JSONStorableDynamic d = o as JSONStorableDynamic;
                    if (d == null) continue;
                    object instObj = typeof(JSONStorableDynamic).GetField("instance", inst).GetValue(d);
                    if (instObj as UnityEngine.Object != null && (!d.gameObject.activeInHierarchy || !d.enabled)) loadedInactive++;
                }
            }
            foreach (string m in new[] { "UnloadInactiveCharacters", "UnloadInactiveClothingItems", "UnloadInactiveHairItems" })
                typeof(DAZCharacterSelector).GetMethod(m, inst, null, Type.EmptyTypes, null).Invoke(s, null);
        }
        return "people " + people + ", loaded inactive " + loadedInactive + ", " + ((Time.realtimeSinceStartup - t0) * 1000).ToString("F0") + " ms";
    }

    // still loading: scene load, images queued, asset bundle requests queued
    public static string Busy()
    {
        int images = -1, bundles = -1;
        try {
            Type il = Find("ImageLoaderThreaded");
            object single = il.GetField("singleton", stat).GetValue(null);
            images = Count(il.GetField("queuedImages", inst).GetValue(single));
            FieldInfo n = il.GetField("numRealQueuedImages", inst);
            if (n != null) images += (int)n.GetValue(single) * 1000;
        } catch { }
        try {
            object al = typeof(MeshVR.AssetLoader).GetField("singleton", stat).GetValue(null);
            bundles = Count(typeof(MeshVR.AssetLoader).GetField("assetBundleFromFileQueue", inst).GetValue(al));
        } catch { }
        return (SuperController.singleton.isLoading ? "loading" : "idle") + " images " + images + " bundles " + bundles;
    }

    // a full collection now (Boehm's own), timed: what the heap is once the garbage is gone
    public static string Collect()
    {
        float t0 = Time.realtimeSinceStartup;
        GC.Collect();
        return "GC.Collect " + ((Time.realtimeSinceStartup - t0) * 1000).ToString("F0") + " ms, managed " + GB(GC.GetTotalMemory(false));
    }

    // Resources.UnloadUnusedAssets, timed until done (frames counted by the caller)
    public static AsyncOperation unloadOp;
    static float unloadT0;
    public static string Unload() { unloadT0 = Time.realtimeSinceStartup; unloadOp = Resources.UnloadUnusedAssets(); return "started"; }
    public static string UnloadDone() { return unloadOp == null ? "none" : unloadOp.isDone ? "done " + ((Time.realtimeSinceStartup - unloadT0) * 1000).ToString("F0") + " ms" : "running"; }
}
