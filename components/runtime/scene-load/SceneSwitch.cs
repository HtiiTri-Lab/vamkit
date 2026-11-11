// VaM Scene Load: scene switches and memory over a session ([SceneSwitch], [BundleLoad] KeepForNextScene,
// [Memory], [TextureCache] in local.vam.sceneload.cfg). How VaM builds and frees memory and caches, and the
// measurements: docs/knowledge/loading.md.
//
// A (KeepClearing): a scene switch goes through where VaM's stops or breaks every later load; nothing else changes.
//   A1 SubScene.PreRemove removes every atom of the subscene; a stage scene parents the starting atom WindowCamera into a
//      subscene, so a clear destroyed a starting atom that stays in startingAtoms, and every later LoadInternal
//      fails in new Atom[atoms.Count] + atomsList.CopyTo ("Destination array was not long enough"). A starting atom in a
//      subscene that goes away is unparented instead (what RemoveAtom does for the children of any other atom).
//   A2 RemoveAtom calls the plugins' onAtomRemovedHandlers without a catch: one that throws (Timeline's AtomPlugin.
//      OnAtomRemoved) ends RemoveNonStartingAtoms halfway and the load never starts. Each handler
//      now runs on its own, an exception logged the way Atom.OnPreRemove logs its storables'.
//   A3 (BundleLoads.cs) a request's callback that throws is logged and the queue goes on (our queue coroutine stopped
//      for good: every later asset bundle request of the run stayed queued).
//   A4 before LoadInternal / RemoveNonStartingAtoms: starting atoms no longer in the scene (pooled, destroyed, replaced
//      in the atom table) are dropped from startingAtoms / atomsList, a live one missing from the table is put back.
//   A5 (ReleaseOrphanBundles) after a clear, a bundle still counted whose requesters were all destroyed (a plugin that
//      loads a bundle and never lets go) is let go with VaM's DoneWithAssetBundleFromFile: Unity
//      refuses a second bundle with the same files, so the next scene's own version of it failed. Requesters are the
//      callbacks' targets, recorded when a request is loaded; a request whose target is not a Unity object never is.
// B (KeepForNextScene) a bundle whose last user lets go stays loaded when the scene being loaded names it (its path
//   in the scene's JSON) or, outside a scene switch, for KeepSeconds; a request of it in that time reuses it as VaM
//   reuses any loaded bundle. Kept bundles the next scene does not name are unloaded before it loads anything; a bundle
//   Unity refuses while bundles are kept (the same files) unloads them all and loads once more. A large stage scene reloaded in
//   the same VaM: ready 38.4 -> 31.7 s, the voice pack's frame (5 s) gone.
// C (UnloadInactiveAtVideoMemoryPercent) appearance changes in a scene leave the old character / clothing / hair
//   instances loaded (inactive) until the next scene switch; when the process's video memory reaches this share of its
//   budget, the inactive instances of the scene's active people are unloaded (DAZCharacterSelector's own
//   UnloadInactive*; their textures go at once, no UnloadUnusedAssets). Two appearance changes in a large scene: 34 items,
//   86 ms, video memory 20.6 -> 17.6 GB.
// D (TextureCache MaxGB) VaM's texture cache (Cache\Textures, raw pixels of every texture it ever loaded) is never
//   trimmed (91 GB here). The last use of each cache file is recorded (QueuedImage.GetDiskCachePath, read or write);
//   over MaxGB the least recently used are deleted in the background down to 90% of it, never one used this run.
//   Off unless the user sets a cap (MaxGB 0): deleting someone's cache is their decision, not an install's.
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using BepInEx.Configuration;
using HarmonyLib;
using MVR.FileManagement;
using UnityEngine;
using Loader = MeshVR.AssetLoader;

public sealed unsafe partial class VaMSceneLoad
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int VideoMemoryFn(out long usage, out long budget);

    static ConfigEntry<bool> keepClearing, releaseOrphans, keepBundles;
    static ConfigEntry<int> keepSeconds, vramPercent, textureCacheGB;
    static bool clearFixReady, keepReady, imageReady;
    static VideoMemoryFn videoMemory;
    static FieldInfo fStarting, fAtoms, fAtomsList, fSubAtoms, fDynInstance;
    static FieldInfo[] fItemArrays;
    static int handlerErrors, startingKept, startingDropped, startingRepaired, callbackErrors, processorRestarts;

    void SetUpSceneSwitch(Harmony h)
    {
        keepClearing = Config.Bind("SceneSwitch", "KeepClearing", true, "Keep a scene clear going where VaM's stops or breaks later loads: a starting atom inside a subscene is unparented, not removed; each atom-removed handler of a plugin runs on its own (an exception is logged); starting atoms no longer in the scene are dropped before a load; a throwing asset bundle callback is logged and the bundle queue goes on");
        releaseOrphans = Config.Bind("SceneSwitch", "ReleaseOrphanBundles", true, "After a scene clear, let go of asset bundles still counted whose requesters were all destroyed (a plugin that never released its bundle), so the next scene can load its own version of it");
        keepBundles = Config.Bind("BundleLoad", "KeepForNextScene", true, "An asset bundle whose last user lets go stays loaded when the scene being loaded names it, or outside a scene switch for KeepSeconds, and a request of it in that time reuses it; kept bundles the next scene does not name are unloaded before it loads anything");
        keepSeconds = Config.Bind("BundleLoad", "KeepSeconds", 60, "With KeepForNextScene: seconds an unused asset bundle stays loaded outside a scene switch (a scene clear, a CustomUnityAsset changing its asset)");
        vramPercent = Config.Bind("Memory", "UnloadInactiveAtVideoMemoryPercent", 85, "When the process's video memory reaches this percent of its budget (and no scene is loading), unload the inactive character / clothing / hair instances of the scene's people (left loaded by appearance changes until the next scene switch); 0 = never");
        textureCacheGB = Config.Bind("TextureCache", "MaxGB", 0, "Size cap of VaM's texture cache (Cache\\Textures; GB of 2^30 bytes): over it the cache files used least recently are deleted in the background down to 90% of it, never one used this run; 0 = no cap, nothing is ever deleted (the default)");
        BindingFlags inst = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public, stat = BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public;
        try {
            Type sc = typeof(SuperController);
            fStarting = Need(sc.GetField("startingAtoms", inst), "SuperController.startingAtoms");
            fAtoms = Need(sc.GetField("atoms", inst), "SuperController.atoms");
            fAtomsList = Need(sc.GetField("atomsList", inst), "SuperController.atomsList");
            fSubAtoms = Need(typeof(SubScene).GetField("_atomsInSubScene", inst), "SubScene._atomsInSubScene");
            if (fStarting.FieldType != typeof(HashSet<Atom>) || fAtoms.FieldType != typeof(Dictionary<string, Atom>) || fAtomsList.FieldType != typeof(List<Atom>) || !typeof(IEnumerable).IsAssignableFrom(fSubAtoms.FieldType))
                throw new MissingFieldException("SuperController / SubScene fields of other types");
            MethodInfo rna = sc.GetMethod("RemoveNonStartingAtoms", inst, null, Type.EmptyTypes, null);
            MethodInfo ra = sc.GetMethod("RemoveAtom", inst, null, new[] { typeof(Atom), typeof(bool) }, null);
            MethodInfo li = sc.GetMethod("LoadInternal", inst, null, new[] { typeof(string), typeof(bool), typeof(bool) }, null);
            MethodInfo spr = typeof(SubScene).GetMethod("PreRemove", inst, null, Type.EmptyTypes, null);
            if (rna == null || ra == null || li == null || spr == null) throw new MissingMethodException("RemoveNonStartingAtoms / RemoveAtom / LoadInternal / SubScene.PreRemove");
            h.Patch(rna, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ClearPrefix", stat)), new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ClearPostfix", stat)), null, null);
            h.Patch(li, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("LoadInternalPrefix", stat)), null, null, null);
            h.Patch(spr, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("SubScenePreRemove", stat)), null, null, null);
            h.Patch(ra, null, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("RemoveAtomTranspiler", stat)), null);
            clearFixReady = true;
        } catch (Exception e) { Logger.LogError("VaM scene load: scene switch fixes off: " + e.Message); }
        try {
            if (!bundlesReady) throw new InvalidOperationException("asset bundle loads are not ours");
            MethodInfo done = typeof(Loader).GetMethod("DoneWithAssetBundleFromFile", stat, null, new[] { typeof(string) }, null);
            if (done == null) throw new MissingMethodException("AssetLoader.DoneWithAssetBundleFromFile");
            h.Patch(done, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("DonePrefix", stat)), null, null, null);
            keepReady = true;
        } catch (Exception e) { Logger.LogError("VaM scene load: keeping asset bundles off: " + e.Message); }
        try {
            fDynInstance = Need(typeof(JSONStorableDynamic).GetField("instance", inst), "JSONStorableDynamic.instance");
            List<FieldInfo> arrays = new List<FieldInfo>();
            foreach (string f in new[] { "_femaleCharacters", "_maleCharacters", "_femaleClothingItems", "_maleClothingItems", "_femaleHairItems", "_maleHairItems" })
                arrays.Add(Need(typeof(DAZCharacterSelector).GetField(f, inst), "DAZCharacterSelector." + f));
            fItemArrays = arrays.ToArray();
            IntPtr m = LoadLibraryW(Path.Combine(Path.GetDirectoryName(Info.Location), "VaMSceneLoadNative.bin"));
            IntPtr vm = m != IntPtr.Zero ? GetProcAddress(m, "SL_VideoMemory") : IntPtr.Zero;
            if (vm == IntPtr.Zero) throw new EntryPointNotFoundException("SL_VideoMemory");
            videoMemory = (VideoMemoryFn)Marshal.GetDelegateForFunctionPointer(vm, typeof(VideoMemoryFn));
        } catch (Exception e) { Logger.LogError("VaM scene load: unloading inactive appearance items off: " + e.Message); videoMemory = null; }
        try {
            Type qi = typeof(ImageLoaderThreaded).GetNestedType("QueuedImage", BindingFlags.Public | BindingFlags.NonPublic);
            MethodInfo gdc = qi != null ? qi.GetMethod("GetDiskCachePath", inst, null, Type.EmptyTypes, null) : null;
            if (gdc == null || gdc.ReturnType != typeof(string)) throw new MissingMethodException("ImageLoaderThreaded.QueuedImage.GetDiskCachePath");
            h.Patch(gdc, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("CachePathPostfix", stat)), null, null);
            imageReady = true;
        } catch (Exception e) { Logger.LogError("VaM scene load: texture cache cap off: " + e.Message); }
        Logger.LogInfo("VaM scene load: scene switch fixes " + (clearFixReady && keepClearing.Value ? "on" : "off") + ", orphan bundles released " + (clearFixReady && releaseOrphans.Value ? "on" : "off")
            + ", bundles kept for the next scene " + (keepReady && keepBundles.Value ? "on (" + keepSeconds.Value + " s outside a switch)" : "off")
            + ", inactive appearance items unloaded at " + (videoMemory != null && vramPercent.Value > 0 ? vramPercent.Value + "% of video memory" : "off")
            + ", texture cache cap " + (imageReady && textureCacheGB.Value > 0 ? textureCacheGB.Value + " GB" : "off"));
    }

    // ---------------------------------------------------------------------------------------------------------------
    // A: the clear

    public static IEnumerable<CodeInstruction> RemoveAtomTranspiler(IEnumerable<CodeInstruction> code)
    {
        MethodInfo invoke = typeof(SuperController.OnAtomRemoved).GetMethod("Invoke");
        MethodInfo safe = typeof(VaMSceneLoad).GetMethod("InvokeRemoved", BindingFlags.Static | BindingFlags.Public);
        int n = 0;
        foreach (CodeInstruction c in code) {
            if ((c.opcode == OpCodes.Callvirt || c.opcode == OpCodes.Call) && c.operand as MethodInfo == invoke) { c.opcode = OpCodes.Call; c.operand = safe; n++; }
            yield return c;
        }
        if (n != 1) log.LogWarning("VaM scene load: RemoveAtom has " + n + " atom-removed handler calls (1 expected)");
    }

    public static void InvokeRemoved(SuperController.OnAtomRemoved handlers, Atom atom)
    {
        if (!keepClearing.Value) { handlers(atom); return; }
        foreach (Delegate d in handlers.GetInvocationList()) {
            try { ((SuperController.OnAtomRemoved)d)(atom); }
            catch (Exception e) {
                handlerErrors++;
                SuperController.LogError("Exception during atom removed handler " + (d.Target != null ? d.Target.GetType().FullName : d.Method.DeclaringType.FullName) + "." + d.Method.Name + " of " + (atom != null ? atom.uid : "null") + ": " + e);
            }
        }
    }

    public static bool SubScenePreRemove(SubScene __instance)
    {
        if (!keepClearing.Value) return true;
        try {
            SuperController s = SuperController.singleton;
            HashSet<Atom> starting = (HashSet<Atom>)fStarting.GetValue(s);
            IEnumerable inSub = fSubAtoms.GetValue(__instance) as IEnumerable;
            if (starting == null || inSub == null) return true;
            List<Atom> copy = new List<Atom>();
            foreach (object o in inSub) copy.Add(o as Atom);
            foreach (Atom a in copy) {
                if (a != null && starting.Contains(a)) {
                    a.parentAtom = null;
                    startingKept++;
                    log.LogWarning("VaM scene load: starting atom " + a.uid + " unparented instead of removed with subscene " + (__instance.containingAtom != null ? __instance.containingAtom.uid : "?"));
                } else s.RemoveAtom(a);
            }
            return false;
        } catch (Exception e) { log.LogError("VaM scene load: subscene removal: " + e); return true; }
    }

    static void StartingCheck(SuperController sc)
    {
        HashSet<Atom> starting = (HashSet<Atom>)fStarting.GetValue(sc);
        Dictionary<string, Atom> atoms = (Dictionary<string, Atom>)fAtoms.GetValue(sc);
        if (starting == null || atoms == null) return;
        List<Atom> drop = null;
        foreach (Atom a in starting) {
            if (a == null) { (drop ?? (drop = new List<Atom>())).Add(a); continue; }
            Atom b;
            if (atoms.TryGetValue(a.uid, out b) && ReferenceEquals(a, b)) continue;
            string what = a.uid + " (" + a.type + ", in pool " + a.inPool + ", destroyed " + a.destroyed + ", atom table has " + (b != null ? b.type : "none") + ")";
            if (a.inPool || a.destroyed || b != null) {
                (drop ?? (drop = new List<Atom>())).Add(a);
                startingDropped++;
                log.LogWarning("VaM scene load: starting atom no longer in the scene dropped: " + what);
            } else {
                atoms[a.uid] = a;
                startingRepaired++;
                log.LogWarning("VaM scene load: starting atom missing from the atom table put back: " + what);
            }
        }
        if (drop == null) return;
        List<Atom> list = (List<Atom>)fAtomsList.GetValue(sc);
        foreach (Atom a in drop) {
            starting.Remove(a);
            if (list != null) for (int i = list.Count - 1; i >= 0; i--) if (ReferenceEquals(list[i], a)) list.RemoveAt(i);
        }
    }

    static int clearFrame;

    public static void ClearPrefix(SuperController __instance)
    {
        if (!keepClearing.Value) return;
        try { StartingCheck(__instance); } catch (Exception e) { log.LogError("VaM scene load: starting atoms check: " + e); }
    }

    public static void ClearPostfix() { clearFrame = Time.frameCount; }

    // LoadInternal(saveName, loadMerge, editMode): the starting atoms check (it copies atomsList the same way), and for
    // a scene switch, the scene's JSON for which kept bundles it names
    public static void LoadInternalPrefix(SuperController __instance, string saveName, bool loadMerge)
    {
        if (keepClearing.Value) {
            try { StartingCheck(__instance); } catch (Exception e) { log.LogError("VaM scene load: starting atoms check: " + e); }
        }
        if (!keepReady || !keepBundles.Value || loadMerge || __instance.isLoading || string.IsNullOrEmpty(saveName)) return;
        try {
            if (!FileManager.FileExists(saveName, false, false)) return;
            switchJson = FileManager.ReadAllText(saveName, false);
            int pk = saveName.IndexOf(":/", StringComparison.Ordinal);
            switchPackage = pk > 0 ? saveName.Substring(0, pk) : null;
            switching = true;
            switchFrame = Time.frameCount;
            List<string> drop = null;
            foreach (string p in kept.Keys) if (!Named(p)) (drop ?? (drop = new List<string>())).Add(p);
            if (drop != null) foreach (string p in drop) UnloadKept(p);
            List<string> keys = new List<string>(kept.Keys);
            foreach (string p in keys) kept[p] = float.MaxValue;
        } catch (Exception e) { switching = false; switchJson = null; log.LogError("VaM scene load: reading the scene for kept bundles: " + e.Message); }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // A5 / B: asset bundles over a clear

    static readonly object Unknown = new object();
    static readonly Dictionary<string, List<object>> owners = new Dictionary<string, List<object>>();
    static readonly Dictionary<string, float> kept = new Dictionary<string, float>();   // path -> unscaled time it goes
    static string switchJson, switchPackage;
    static bool switching;
    static int switchFrame;
    static long kKept, kReused, kUnloaded, kConflicts, orphansReleased;

    // (LoadBundle) a request loaded: its callback's target, the request's owner
    static void NoteRequest(string path, Delegate cb)
    {
        if (kept.Count > 0 && kept.Remove(path)) kReused++;
        if (!releaseOrphans.Value) return;
        object o = Unknown;
        if (cb != null) {
            Delegate[] all = cb.GetInvocationList();
            object t = all.Length == 1 ? all[0].Target : null;
            if (t is UnityEngine.Object) o = t;
            else if (t != null) {
                // a closure: the Unity object it captured ("<>4__this" or a component)
                foreach (FieldInfo f in t.GetType().GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
                    if (typeof(UnityEngine.Object).IsAssignableFrom(f.FieldType)) { object v = f.GetValue(t); if (v is UnityEngine.Object && (UnityEngine.Object)v != null) { o = v; break; } }
            }
        }
        List<object> l;
        if (!owners.TryGetValue(path, out l)) owners[path] = l = new List<object>();
        foreach (object x in l) if (ReferenceEquals(x, o)) return;
        l.Add(o);
    }

    static bool Gone(object o) { return o is UnityEngine.Object && (UnityEngine.Object)o == null; }

    // a frame after a clear (the removed atoms' objects are destroyed by then), before the next scene asks for anything
    static void ReleaseOrphans()
    {
        Loader al = (Loader)fSingleton.GetValue(null);
        Dictionary<string, int> counts = al != null ? (Dictionary<string, int>)fCounts.GetValue(al) : null;
        if (counts == null) { owners.Clear(); return; }
        List<string> stale = null, orphan = null;
        foreach (KeyValuePair<string, List<object>> kv in owners) {
            if (!counts.ContainsKey(kv.Key)) { (stale ?? (stale = new List<string>())).Add(kv.Key); continue; }
            bool allGone = kv.Value.Count > 0;
            foreach (object o in kv.Value) if (!Gone(o)) { allGone = false; break; }
            if (allGone) (orphan ?? (orphan = new List<string>())).Add(kv.Key);
        }
        if (stale != null) foreach (string p in stale) owners.Remove(p);
        if (orphan == null) return;
        foreach (string p in orphan) {
            int n = counts[p];
            owners.Remove(p);
            log.LogWarning("VaM scene load: asset bundle " + p + " still counted " + n + " after its requesters were all destroyed: released");
            for (int i = 0; i < n && counts.ContainsKey(p); i++) Loader.DoneWithAssetBundleFromFile(p);
            orphansReleased++;
        }
    }

    // B: does the scene being loaded name this bundle: its path as it is, for a file of package Author.Name.Version
    // also Author.Name.latest:/<file>, and SELF:/<file> when the scene is in that same package (another version of a
    // package is another bundle: kept, it would make Unity refuse the one the scene loads)
    static bool Named(string path)
    {
        if (switchJson == null || path.Length == 0) return false;
        if (switchJson.IndexOf(path, StringComparison.Ordinal) >= 0) return true;
        int i = path.IndexOf(":/", StringComparison.Ordinal);
        if (i <= 0) return false;
        string package = path.Substring(0, i), file = path.Substring(i + 2);
        if (switchPackage == package && switchJson.IndexOf("SELF:/" + file, StringComparison.Ordinal) >= 0) return true;
        int v = package.LastIndexOf('.');
        return v > 0 && switchJson.IndexOf(package.Substring(0, v) + ".latest:/" + file, StringComparison.Ordinal) >= 0;
    }

    // AssetLoader.DoneWithAssetBundleFromFile(path): the last user letting go of a loaded bundle keeps it
    public static bool DonePrefix(string path)
    {
        if (!keepBundles.Value || path == null || testKeepOff) return true;
        try {
            Loader al = (Loader)fSingleton.GetValue(null);
            if (al == null) return true;
            Dictionary<string, int> counts = (Dictionary<string, int>)fCounts.GetValue(al);
            Dictionary<string, AssetBundle> bundles = (Dictionary<string, AssetBundle>)fBundles.GetValue(al);
            int n; AssetBundle ab;
            if (counts == null || bundles == null || !counts.TryGetValue(path, out n) || n != 1 || !bundles.TryGetValue(path, out ab) || ab == null) return true;
            if (switching && !Named(path)) return true;   // the scene being loaded does not use it: VaM unloads it now
            counts.Remove(path);
            kept[path] = switching ? float.MaxValue : Time.unscaledTime + Math.Max(0, keepSeconds.Value);
            kKept++;
            return false;
        } catch (Exception e) { log.LogError("VaM scene load: keeping " + path + ": " + e.Message); return true; }
    }

    // what DoneWithAssetBundleFromFile does when the count reaches 0
    static void UnloadKept(string path)
    {
        kept.Remove(path);
        Loader al = (Loader)fSingleton.GetValue(null);
        if (al == null) return;
        Dictionary<string, int> counts = (Dictionary<string, int>)fCounts.GetValue(al);
        Dictionary<string, AssetBundle> bundles = (Dictionary<string, AssetBundle>)fBundles.GetValue(al);
        if (counts != null && counts.ContainsKey(path)) return;   // in use again
        AssetBundle ab;
        if (bundles == null || !bundles.TryGetValue(path, out ab)) return;
        Debug.Log("Unloading unused asset bundle " + path);
        if (ab != null) ab.Unload(true);
        bundles.Remove(path);
        kUnloaded++;
    }

    // (LoadBundle) Unity refused a bundle while others are kept: one of them may have the same files
    static bool UnloadAllKept(string path)
    {
        if (kept.Count == 0) return false;
        kConflicts++;
        log.LogWarning("VaM scene load: " + path + " did not load while " + kept.Count + " unused asset bundles were kept; unloading them and loading it again");
        foreach (string p in new List<string>(kept.Keys)) UnloadKept(p);
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // C: inactive appearance items

    static long vUse, vBudget, vUnloads, vItems;
    static float nextVramCheck, nextVramUnload;
    static double vMs;
    static int testVramPercent = -1;

    static int LoadedInactive(DAZCharacterSelector s)
    {
        int n = 0;
        foreach (FieldInfo f in fItemArrays) {
            IEnumerable items = f.GetValue(s) as IEnumerable;
            if (items == null) continue;
            foreach (object o in items) {
                JSONStorableDynamic d = o as JSONStorableDynamic;
                if (d == null) continue;
                UnityEngine.Object i = fDynInstance.GetValue(d) as UnityEngine.Object;
                if (i != null && (!d.gameObject.activeInHierarchy || !d.enabled)) n++;
            }
        }
        return n;
    }

    // the scene's people that are on (pooled ones and ones switched off are VaM's: freed at the next scene switch)
    static List<DAZCharacterSelector> People()
    {
        List<DAZCharacterSelector> l = new List<DAZCharacterSelector>();
        foreach (Atom a in SuperController.singleton.GetAtoms()) {
            if (a == null || a.type != "Person" || a.inPool) continue;
            DAZCharacterSelector s = a.GetStorableByID("geometry") as DAZCharacterSelector;
            if (s != null && s.isActiveAndEnabled) l.Add(s);
        }
        return l;
    }

    static string UnloadInactive(string why)
    {
        double t0 = Now();
        int people = 0, items = 0;
        foreach (DAZCharacterSelector s in People()) {
            int n = LoadedInactive(s);
            if (n == 0) continue;
            people++; items += n;
            s.UnloadInactiveCharacters();
            s.UnloadInactiveClothingItems();
            s.UnloadInactiveHairItems();
        }
        double ms = Now() - t0;
        if (items > 0) { vUnloads++; vItems += items; vMs += ms; }
        string r = "unloaded " + items + " inactive appearance items of " + people + " people (" + ms.ToString("F0") + " ms)";
        if (items > 0) log.LogInfo("VaM scene load: " + why + ": " + r);
        return r;
    }

    static void MemoryCheck()
    {
        int pct = testVramPercent >= 0 ? testVramPercent : vramPercent.Value;
        if (videoMemory == null || pct <= 0 || Time.unscaledTime < nextVramCheck) return;
        nextVramCheck = Time.unscaledTime + 1f;
        long use, budget;
        if (videoMemory(out use, out budget) == 0 || budget <= 0) return;
        vUse = use; vBudget = budget;
        if (use * 100 < budget * pct || Time.unscaledTime < nextVramUnload) return;
        nextVramUnload = Time.unscaledTime + 30f;   // also when there was nothing to unload: no scan every second while full
        UnloadInactive("video memory " + (use / 1073741824.0).ToString("F1") + " / " + (budget / 1073741824.0).ToString("F1") + " GB (" + (use * 100 / budget) + "%)");
    }

    // ---------------------------------------------------------------------------------------------------------------
    // D: texture cache

    static readonly Dictionary<string, long> lastUse = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);   // file name -> UTC ticks
    static readonly HashSet<string> usedThisRun = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    static bool useLoaded, pruneDue = true;
    static volatile bool pruning;
    static float pruneAfter = 30f;
    static string pruneNote = "";
    static long prunedFiles, prunedBytes;

    static string UseFile() { return Path.Combine(Path.Combine(BepInEx.Paths.CachePath, "VaMSceneLoad"), "texturecache-use.txt"); }

    public static void CachePathPostfix(string __result)
    {
        if (__result == null) return;
        string name = Path.GetFileName(__result);
        long now = DateTime.UtcNow.Ticks;
        lock (lastUse) { lastUse[name] = now; usedThisRun.Add(name); }
    }

    static void LoadUse()   // under lock (lastUse)
    {
        if (useLoaded) return;
        useLoaded = true;
        string f = UseFile();
        if (!File.Exists(f)) return;
        foreach (string line in File.ReadAllLines(f)) {
            int t = line.IndexOf('\t');
            long ticks;
            if (t <= 0 || !long.TryParse(line.Substring(0, t), out ticks)) continue;
            string name = line.Substring(t + 1);
            long have;
            if (!lastUse.TryGetValue(name, out have) || have < ticks) lastUse[name] = ticks;
        }
    }

    // Least recently used files of dir over cap deleted down to 90% of cap (data first, then its meta: VaM reads the
    // meta only when the data exists); never a file used this run; meta files without data go too.
    static string Prune(string dir, long cap, bool dryRun, bool record)
    {
        DirectoryInfo di = new DirectoryInfo(dir);
        if (!di.Exists) return "no " + dir;
        Dictionary<string, FileInfo> data = new Dictionary<string, FileInfo>(StringComparer.OrdinalIgnoreCase), meta = new Dictionary<string, FileInfo>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (FileInfo f in di.GetFiles()) {
            total += f.Length;
            if (f.Name.EndsWith(".vamcache", StringComparison.OrdinalIgnoreCase)) data[f.Name] = f;
            else if (f.Name.EndsWith(".vamcachemeta", StringComparison.OrdinalIgnoreCase)) meta[f.Name] = f;
        }
        List<KeyValuePair<long, FileInfo>> order = new List<KeyValuePair<long, FileInfo>>();
        HashSet<string> used;
        lock (lastUse) {
            LoadUse();
            used = new HashSet<string>(usedThisRun, StringComparer.OrdinalIgnoreCase);
            foreach (FileInfo f in data.Values) {
                long t = f.LastWriteTimeUtc.Ticks, u;
                if (lastUse.TryGetValue(f.Name, out u) && u > t) t = u;
                order.Add(new KeyValuePair<long, FileInfo>(t, f));
            }
        }
        order.Sort(delegate(KeyValuePair<long, FileInfo> a, KeyValuePair<long, FileInfo> b) { return a.Key.CompareTo(b.Key); });
        long before = total, target = cap / 10 * 9;
        int files = 0, failed = 0, orphanMeta = 0;
        long freed = 0;
        foreach (KeyValuePair<string, FileInfo> kv in meta) {
            string d = kv.Key.Substring(0, kv.Key.Length - 4);   // X.vamcachemeta -> X.vamcache
            if (data.ContainsKey(d) || used.Contains(d)) continue;
            try { if (!dryRun) kv.Value.Delete(); total -= kv.Value.Length; freed += kv.Value.Length; orphanMeta++; } catch { failed++; }
        }
        if (total > cap) {
            foreach (KeyValuePair<long, FileInfo> e in order) {
                if (total <= target) break;
                FileInfo f = e.Value;
                if (used.Contains(f.Name)) continue;
                try {
                    if (!dryRun) f.Delete();
                    total -= f.Length; freed += f.Length; files++;
                    FileInfo m;
                    if (meta.TryGetValue(f.Name + "meta", out m)) { try { if (!dryRun) m.Delete(); total -= m.Length; freed += m.Length; } catch { failed++; } }
                    lock (lastUse) lastUse.Remove(f.Name);
                } catch { failed++; }
            }
        }
        if (!dryRun && record) {
            prunedFiles += files; prunedBytes += freed;
            // the use record: files still there
            StringBuilder sb = new StringBuilder();
            lock (lastUse) {
                foreach (KeyValuePair<string, long> kv in lastUse)
                    if (data.ContainsKey(kv.Key) || usedThisRun.Contains(kv.Key)) sb.Append(kv.Value).Append('\t').Append(kv.Key).Append('\n');
            }
            Directory.CreateDirectory(Path.GetDirectoryName(UseFile()));
            File.WriteAllText(UseFile(), sb.ToString());
        }
        return (dryRun ? "would delete " : "deleted ") + files + " cache files + " + orphanMeta + " meta files without data (" + (freed / 1073741824.0).ToString("F1") + " GB), failed " + failed
            + "; " + (before / 1073741824.0).ToString("F1") + " -> " + (total / 1073741824.0).ToString("F1") + " GB of cap " + (cap / 1073741824.0).ToString("F1") + " GB, " + data.Count + " cache files, used this run " + used.Count;
    }

    static void PruneCheck()
    {
        if (!imageReady || textureCacheGB.Value <= 0 || !pruneDue || pruning || Time.unscaledTime < pruneAfter || !CacheManager.CachingEnabled) return;
        string dir = CacheManager.GetTextureCacheDir();
        if (string.IsNullOrEmpty(dir)) return;
        pruneDue = false;
        pruning = true;
        long cap = (long)textureCacheGB.Value << 30;
        ThreadPool.QueueUserWorkItem(delegate(object o) {
            try {
                Thread.CurrentThread.Priority = System.Threading.ThreadPriority.BelowNormal;
                pruneNote = Prune(dir, cap, false, true);
                log.LogInfo("VaM scene load: texture cache " + dir + ": " + pruneNote);
            } catch (Exception e) { pruneNote = "failed: " + e.Message; log.LogError("VaM scene load: texture cache trim: " + e); }
            finally { Thread.CurrentThread.Priority = System.Threading.ThreadPriority.Normal; pruning = false; }
        });
    }

    // ---------------------------------------------------------------------------------------------------------------

    // from Update (LoadFrames.cs)
    static void SwitchUpdate()
    {
        SuperController sc = SuperController.singleton;
        if (sc == null) return;
        bool loading = sc.isLoading;
        if (clearFrame != 0 && Time.frameCount > clearFrame) {
            clearFrame = 0;
            if (keepReady && releaseOrphans.Value) { try { ReleaseOrphans(); } catch (Exception e) { log.LogError("VaM scene load: releasing orphan bundles: " + e); } }
        }
        if (switching && !loading && Time.frameCount > switchFrame + 1) {
            switching = false;
            switchJson = null;
            switchPackage = null;
            List<string> keys = new List<string>(kept.Keys);
            foreach (string p in keys) if (kept[p] == float.MaxValue) kept[p] = Time.unscaledTime + Math.Max(0, keepSeconds.Value);
            pruneDue = true;   // a scene loaded: textures it made new may have pushed the cache over its cap
            pruneAfter = Time.unscaledTime + 10f;
        }
        if (loading) return;
        if (kept.Count > 0) {
            List<string> due = null;
            foreach (KeyValuePair<string, float> kv in kept) if (kv.Value <= Time.unscaledTime) (due ?? (due = new List<string>())).Add(kv.Key);
            if (due != null) foreach (string p in due) UnloadKept(p);
        }
        MemoryCheck();
        PruneCheck();
    }

    void OnApplicationQuit()
    {
        if (!imageReady || pruning) return;
        try {
            StringBuilder sb = new StringBuilder();
            lock (lastUse) {
                LoadUse();
                foreach (KeyValuePair<string, long> kv in lastUse) sb.Append(kv.Value).Append('\t').Append(kv.Key).Append('\n');
            }
            Directory.CreateDirectory(Path.GetDirectoryName(UseFile()));
            File.WriteAllText(UseFile(), sb.ToString());
        } catch { }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // test interface (this run only)

    static bool testKeepOff;
    public static string SetKeepTestOff(bool off) { testKeepOff = off; return SwitchStats(); }
    public static string SetVramTestPercent(int pct) { testVramPercent = pct; nextVramCheck = 0; nextVramUnload = 0; return SwitchStats(); }
    public static string UnloadInactiveNow() { return UnloadInactive("test"); }
    public static string CountInactive()
    {
        int people = 0, items = 0;
        foreach (DAZCharacterSelector s in People()) { people++; items += LoadedInactive(s); }
        return "people " + people + ", loaded inactive items " + items;
    }
    public static string TexturePrune(string dir, double capGB, bool dryRun) { return Prune(dir, (long)(capGB * 1073741824.0), dryRun, false); }

    public static string SwitchStats()
    {
        int lu;
        lock (lastUse) lu = usedThisRun.Count;
        return "scene switch: atom-removed handler errors " + handlerErrors + ", starting atoms kept out of subscenes " + startingKept + ", dropped " + startingDropped + ", put back " + startingRepaired
            + ", bundle callback errors " + callbackErrors + ", queue restarts " + processorRestarts + ", orphan bundles released " + orphansReleased
            + "; bundles kept " + kKept + " (reused " + kReused + ", unloaded " + kUnloaded + ", same-files conflicts " + kConflicts + ", kept now " + kept.Count + (switching ? ", switching" : "") + ")"
            + "; video memory " + (vUse / 1073741824.0).ToString("F1") + " / " + (vBudget / 1073741824.0).ToString("F1") + " GB, inactive appearance unloads " + vUnloads + " (" + vItems + " items, " + vMs.ToString("F0") + " ms)"
            + "; texture cache files used this run " + lu + ", trimmed " + prunedFiles + " (" + (prunedBytes / 1073741824.0).ToString("F1") + " GB)" + (pruneNote.Length > 0 ? " [" + pruneNote + "]" : "") + (pruning ? " (trimming)" : "");
    }
}
