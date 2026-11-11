// VaM Scene Load: scene loading work, one section per feature below (config: local.vam.sceneload.cfg).
// Scene plugins (.cs / .cslist) are compiled once, not on every scene load (local.vam.sceneload.cfg [PluginCache] Enabled).
//   VaM compiles every plugin instance from source with mcs when a scene loads (MVRPluginManager.SyncPluginUrlInternal
//   -> DynamicCSharp ScriptDomain.CompileAndLoadScript* -> ScriptCompiler.CompileSources / CompileFiles), on the main
//   thread, with no cache: a stage scene with 137 plugin instances (43 distinct, about 17 MB of source) compiles all of it on every
//   load. ScriptCompiler.CompileSources / CompileFiles are wrapped: a compile whose inputs were seen before (the
//   sources, the compiler options and references, the plugin's assembly-name prefix and VaM_Data\Managed's assemblies)
//   returns the bytes mcs produced then, instead of compiling; VaM then loads them exactly as after a compile
//   (ScriptDomain.LoadAssembly: its security check of the assembly, then AppDomain.Load). Stored in
//   BepInEx\cache\VaMSceneLoad\plugins, one file per distinct plugin.
//   Assembly names: mcs names a plugin assembly "<prefix>_<McsCompiler.assemblyCounter>", the prefix being
//   "MVRPlugin_<path>_<md5 of the source>"; FileManager.GetPluginHash strips "_[0-9]+$" to know which plugin calls it,
//   and mono hands back an already loaded assembly of the same name instead of loading a second one (which would make
//   two instances of a plugin share their statics). So every load needs a fresh name of that form: on a miss the
//   counter is set to a 9-digit number for that one compile (the name gets 9 digits), and on a hit those 9 digits are
//   overwritten in place with a new 9-digit number (and the module's MVID with a new GUID); lengths never change.
//   Only VaM's plugin compiles (prefix MVRPlugin_) are cached; failed compiles never are.
//   The same scene reloaded in one session (measurements: docs/knowledge/loading.md): load 66.1 s -> 43.0 s; the
//   plugin phase 25.9 -> 6.4 s; the first load with an empty cache already reuses 94 of 135 compiles (same plugin,
//   several atoms). Tried and dropped: holding back VaM's unloads while loading (never asked for in that scene) and Unity's
//   async-load time slice (backgroundLoadingPriority High: the same load time).
// 1.1.0: reading files out of .var packages ([PackageRead] in the same cfg).
//   Sampling a heavy load showed the bytes of the asset bundles cost next to nothing (43 MB, read on a worker thread);
//   the package reads that do cost are VaM's own, on the main thread:
//   - FastEntryLookup: every file VaM opens in a package goes through SharpZipLib 0.86's ZipFile.FindEntry, a linear
//     scan of the package's entries with a culture-aware, case-insensitive string.Compare per entry (morph packs have
//     thousands). FindEntry now answers from an index per package: the first entry whose name equals the wanted one
//     ignoring case, ordinally. That is exactly FindEntry's answer when the wanted name and every entry name are
//     printable ASCII (no ASCII character is ignorable and invariant case folding is A-Z <-> a-z there; checked at
//     start, see AsciiCompareMatches); any other package or name, and the case-sensitive form, run SharpZipLib's own.
//   - FastMorphDeltas: DAZMorph.LoadDeltasFromBinaryFile reads a .vmb with a BinaryReader straight over SharpZipLib's
//     managed inflate stream, 4 bytes per call. For an entry of a .var package it now reads the entry in one go
//     (VaMSceneLoadNative.bin: one file read + miniz inflate, accepting exactly what SharpZipLib's GetInputStream
//     accepts) and fills the same DAZMorphVertex array from those bytes; anything else (loose files, unpacked
//     packages, entries the native reader refuses, short data) runs VaM's own method, which then behaves (and logs)
//     as before. Who calls it on the main thread while loading: person creation and scene plugins that
//     load the deltas of every package morph of the scene's people (about 5.5 s in one frame of every load of that scene).
// 1.2.0: VaM's own work while loading ([LoadPaths] in the same cfg). Each gives VaM's
//   result exactly (see the comments at each): SkipRepeatAnatomySync (clothing / hair resets recompute the anatomy state
//   after each of thousands of items from unchanged inputs: the clear and every person restore), FastRemovePackageFromPath
//   (two regexes per path), FastMorphSort (the morph list sort builds two strings per comparison; the same lists are
//   sorted many times per person), FastVarFileSearch (a culture-aware prefix test on every package file per search).
// 1.3.0: asset bundles of .var packages ([BundleLoad] in the same cfg; BundleLoads.cs): each bundle prepared once as a
//   file Unity opens directly (LZMA bundles stored uncompressed, on CPU Boost's workers while the scene restores), and
//   VaM's bundle queue handing over several ready bundles per frame, in its own order.
// 1.3.1: the rewritten bundle's blocks info LZ4-compressed like Unity's own uncompressed bundles (1.3.0 framed it as
//   literals only, larger than the info; Unity 2018.1 refused all 158 such files of a large stage scene, "Unable to read header
//   from archive file", and VaM's own load ran for them); after a second refused file the files stay off for the run.
// 1.4.0: the frames of a heavy load (LoadFrames.cs): atoms whose prefab is in memory made without a frame each, closed
//   dropdowns' button objects made when opened, Selectable removal by reference, morph metadata kept per file.
// 1.4.1: why a dropdown was built by VaM's own, counted per reason; the test check samples every n-th dropdown (the
//   first-of-each-button-prefab rule checked every one: each dropdown has its own button template).
// 1.5.0: asset bundles prepared in memory and read by Unity through a Stream (LoadFromStreamAsync), no files; DecompressOnLoad Vorbis AudioClips reach Unity as PCM16 decoded in parallel on CPU Boost
//   ([Audio] in the same cfg).
// 1.6.0: scene switches and memory over a session (SceneSwitch.cs): a scene
//   switch goes through where VaM's stops or breaks later loads (subscenes taking starting atoms along, throwing
//   atom-removed handlers and bundle callbacks, bundles a destroyed plugin never let go); bundles the next scene names
//   stay loaded for it; inactive appearance items unloaded when video memory fills; VaM's texture cache capped.
using System;
using System.CodeDom.Compiler;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using ICSharpCode.SharpZipLib.Zip;
using MVR.FileManagement;
using UnityEngine;

[BepInPlugin("local.vam.sceneload", "VaM Scene Load", "1.6.0")]
public sealed unsafe partial class VaMSceneLoad : BaseUnityPlugin
{
    const string Format = "vam-plugin-cache 2";
    const long NameBase = 900000000L;   // 9-digit assembly counters: never reached by VaM's own counter

    static ConfigEntry<bool> useCache;
    static BepInEx.Logging.ManualLogSource log;
    static string dir;
    static string environment;
    static FieldInfo fCompiler, fAssemblyData, fSymbolsData, fIsCompiling, fWarnings, fErrors, fParameters, fCounter;
    static long nextName;
    static int hits, misses, stored, failures;
    static double hitMs, missMs;
    static bool cacheReady;

    void Awake()
    {
        log = Logger;
        useCache = Config.Bind("PluginCache", "Enabled", true, "Reuse the assemblies VaM compiled for scene plugins before, instead of compiling the same sources again on every scene load");
        dir = Path.Combine(Path.Combine(Paths.CachePath, "VaMSceneLoad"), "plugins");
        Harmony h = new Harmony("local.vam.sceneload");
        Assembly vam = typeof(SuperController).Assembly;
        try {
            Type scriptCompiler = vam.GetType("DynamicCSharp.Compiler.ScriptCompiler", true);
            Type marshal = vam.GetType("DynamicCSharp.Compiler.McsMarshal", true);
            Type mcs = vam.GetType("DynamicCSharp.Compiler.McsCompiler", true);
            BindingFlags inst = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
            fCompiler = Need(scriptCompiler.GetField("compiler", inst), "ScriptCompiler.compiler");
            fAssemblyData = Need(scriptCompiler.GetField("assemblyData", inst), "ScriptCompiler.assemblyData");
            fSymbolsData = Need(scriptCompiler.GetField("symbolsData", inst), "ScriptCompiler.symbolsData");
            fIsCompiling = Need(scriptCompiler.GetField("isCompiling", inst), "ScriptCompiler.isCompiling");
            fWarnings = Need(scriptCompiler.GetField("warnings", inst), "ScriptCompiler.warnings");
            fErrors = Need(scriptCompiler.GetField("errors", inst), "ScriptCompiler.errors");
            fParameters = Need(marshal.GetField("parameters", inst), "McsMarshal.parameters");
            fCounter = Need(mcs.GetField("assemblyCounter", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public), "McsCompiler.assemblyCounter");
            if (fCounter.FieldType != typeof(long)) throw new MissingFieldException("McsCompiler.assemblyCounter is " + fCounter.FieldType);
            HarmonyMethod pre = new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("Prefix", BindingFlags.Static | BindingFlags.Public));
            HarmonyMethod post = new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("Postfix", BindingFlags.Static | BindingFlags.Public));
            foreach (string name in new[] { "CompileSources", "CompileFiles" }) {
                MethodInfo m = scriptCompiler.GetMethod(name, inst, null, new[] { typeof(string[]), typeof(string[]) }, null);
                if (m == null || m.ReturnType != typeof(bool)) throw new MissingMethodException("ScriptCompiler." + name);
                h.Patch(m, pre, post, null, null);
            }
            nextName = NameBase + (DateTime.UtcNow.Ticks / TimeSpan.TicksPerSecond % 1000) * 10000;  // a fresh range per run
            cacheReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: plugin cache off, VaM's compiler is not as expected: " + e.Message);
        }
        int files = Directory.Exists(dir) ? Directory.GetFiles(dir, "*.plc").Length : 0;
        Logger.LogInfo("VaM scene load: plugin cache " + (cacheReady && useCache.Value ? "on" : "off") + " (" + files + " compiled plugins in " + dir + ")");
        SetUpPackageReads(h);
        SetUpLoadPaths(h);
        SetUpBundleLoads(h);
        SetUpLoadFrames(h);
        SetUpSceneSwitch(h);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // 1.2.0 VaM's own load paths

    static ConfigEntry<bool> skipAnatomy, fastPackagePath;
    static bool anatomyReady, pathReady;
    [ThreadStatic] static DAZCharacterSelector resetSel;
    [ThreadStatic] static int resetDepth;
    [ThreadStatic] static bool anatomyDirty;
    static long anatomyRuns, anatomySkips, pathFast, pathRegex;

    void SetUpLoadPaths(Harmony h)
    {
        skipAnatomy = Config.Bind("LoadPaths", "SkipRepeatAnatomySync", true, "While VaM resets a person's clothing or hair item by item, recompute the anatomy state (genitals on/off, breast and glute joint adjustments) only when an item's state changed since the last recompute; VaM recomputes it after every one of the thousands of items, from unchanged inputs (same state at every point)");
        fastPackagePath = Config.Bind("LoadPaths", "FastRemovePackageFromPath", true, "FileManager.RemovePackageFromPath without its two regular expressions (the same string; single-line paths)");
        BindingFlags any = BindingFlags.Instance | BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public;
        try {
            Type cs = typeof(DAZCharacterSelector);
            MethodInfo sync = cs.GetMethod("SyncAnatomy", any, null, Type.EmptyTypes, null);
            MethodInfo rc = cs.GetMethod("ResetClothing", any, null, new[] { typeof(bool) }, null);
            MethodInfo rh = cs.GetMethod("ResetHair", any, null, new[] { typeof(bool) }, null);
            MethodInfo sc = cs.GetMethod("SetActiveClothingItem", any, null, new[] { typeof(DAZClothingItem), typeof(bool), typeof(bool) }, null);
            MethodInfo sh = cs.GetMethod("SetActiveHairItem", any, null, new[] { typeof(DAZHairGroup), typeof(bool), typeof(bool) }, null);
            PropertyInfo act = typeof(DAZDynamicItem).GetProperty("active", any);
            PropertyInfo ja = typeof(DAZClothingItem).GetProperty("jointAdjustEnabled", any);
            if (sync == null || rc == null || rh == null || sc == null || sh == null || act == null || act.GetSetMethod(true) == null || ja == null || ja.GetSetMethod(true) == null)
                throw new MissingMethodException("DAZCharacterSelector / DAZDynamicItem members");
            HarmonyMethod rPre = new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ResetPrefix", any)), rFin = new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ResetFinalizer", any));
            h.Patch(rc, rPre, null, null, rFin);
            h.Patch(rh, rPre, null, null, rFin);
            h.Patch(sc, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("SetClothingPrefix", any)), null, null, null);
            h.Patch(sh, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("SetHairPrefix", any)), null, null, null);
            h.Patch(act.GetSetMethod(true), new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ActivePrefix", any)), null, null, null);
            h.Patch(ja.GetSetMethod(true), new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("JointAdjustPrefix", any)), null, null, null);
            h.Patch(sync, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("AnatomyPrefix", any)), new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("AnatomyPostfix", any)), null, null);
            anatomyReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: anatomy sync skipping off: " + e.Message);
        }
        try {
            MethodInfo rp = typeof(FileManager).GetMethod("RemovePackageFromPath", any, null, new[] { typeof(string) }, null);
            if (rp == null || rp.ReturnType != typeof(string)) throw new MissingMethodException("FileManager.RemovePackageFromPath");
            if (!PackagePathMatches()) throw new Exception("the replacement differs from VaM's regular expressions on the built-in samples");
            h.Patch(rp, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("RemovePackagePrefix", any)), null, null, null);
            pathReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: fast RemovePackageFromPath off: " + e.Message);
        }
        fastMorphSort = Config.Bind("LoadPaths", "FastMorphSort", true, "Sorting a person's morph list (GenerateDAZMorphsControlUI.ResyncMorphs): build each morph's sort text once per sort instead of in every comparison, and reuse the result of comparing the same two texts again (the same comparison results, so the same order)");
        fastVarFind = Config.Bind("LoadPaths", "FastVarFileSearch", true, "FileManager.FindVarFilesRegex: remember which package files lie under a folder (the culture-aware prefix test VaM runs on every package file) until the package list changes (the same files, in the same order)");
        mainThread = System.Threading.Thread.CurrentThread.ManagedThreadId;
        try {
            Type ui = typeof(GenerateDAZMorphsControlUI);
            MethodInfo resync = ui.GetMethod("ResyncMorphs", any, null, Type.EmptyTypes, null);
            MethodInfo sort = ui.GetMethod("CustomMorphSort", any, null, new[] { typeof(DAZMorph), typeof(DAZMorph) }, null);
            if (resync == null || sort == null || sort.ReturnType != typeof(int)) throw new MissingMethodException("GenerateDAZMorphsControlUI.ResyncMorphs / CustomMorphSort");
            h.Patch(resync, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ResyncPrefix", any)), null, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ResyncFinalizer", any)));
            h.Patch(sort, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("MorphSortPrefix", any)), null, null, null);
            sortReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: fast morph sort off: " + e.Message);
        }
        try {
            fAllVar = typeof(FileManager).GetField("allVarFileEntries", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
            if (fAllVar == null || fAllVar.FieldType != typeof(HashSet<VarFileEntry>)) throw new MissingFieldException("FileManager.allVarFileEntries");
            foreach (string n in new[] { "generation", "version", "_version" }) {
                FieldInfo g = typeof(HashSet<VarFileEntry>).GetField(n, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                if (g != null && g.FieldType == typeof(int)) { fSetVersion = g; break; }
            }
            MethodInfo find = typeof(FileManager).GetMethod("FindVarFilesRegex", any, null, new[] { typeof(string), typeof(string), typeof(List<FileEntry>) }, null);
            MethodInfo clean = typeof(FileManager).GetMethod("CleanDirectoryPath", any, null, new[] { typeof(string) }, null);
            if (find == null || clean == null || !clean.IsPublic) throw new MissingMethodException("FileManager.FindVarFilesRegex / CleanDirectoryPath");
            h.Patch(find, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("FindVarPrefix", any)), null, null, null);
            findReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: fast package file search off: " + e.Message);
        }
        Logger.LogInfo("VaM scene load: anatomy sync skipping " + (anatomyReady && skipAnatomy.Value ? "on" : "off") + ", fast RemovePackageFromPath " + (pathReady && fastPackagePath.Value ? "on" : "off")
            + ", fast morph sort " + (sortReady && fastMorphSort.Value ? "on" : "off") + ", fast package file search " + (findReady && fastVarFind.Value ? "on" : "off"));
    }

    // GenerateDAZMorphsControlUI.ResyncMorphs sorts the visible morphs with CustomMorphSort:
    //   string.Compare(m1.resolvedRegionName + ":" + m1.resolvedDisplayName, m2.resolvedRegionName + ":" + m2.resolvedDisplayName)
    // (culture-aware, current culture), building both texts in every comparison of the sort. Inside ResyncMorphs the
    // comparison returns the same int from texts built once per morph and sort (rebuilt whenever either name the text
    // is made of is another string than when it was built), and the result of comparing two texts is remembered by
    // their content while the current culture stays the same object. Every comparison gives the value VaM's would, so
    // List.Sort orders the list exactly as before (ties included). A person's morph list is sorted again and again while
    // loading (each bank added, every demand-activated morph clean-up), mostly the same texts in the same order.
    static ConfigEntry<bool> fastMorphSort, fastVarFind;
    static bool sortReady, findReady;
    static int mainThread;
    sealed class MorphKey { public string region, display; public int id; }
    sealed class RefEq : IEqualityComparer<DAZMorph>
    {
        public static readonly RefEq Instance = new RefEq();
        public bool Equals(DAZMorph a, DAZMorph b) { return ReferenceEquals(a, b); }
        public int GetHashCode(DAZMorph m) { return System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(m); }
    }
    static int sortDepth;
    static Dictionary<DAZMorph, MorphKey> sortKeys;
    static readonly Dictionary<string, int> keyIds = new Dictionary<string, int>(StringComparer.Ordinal);
    static readonly List<string> keyTexts = new List<string>();
    // the pair key's own hash: mono's long.GetHashCode is hi ^ lo, which puts (a << 32 | b) for a few thousand ids into a
    // few thousand buckets (the first candidate's comparisons got slower than VaM's)
    sealed class PairHash : IEqualityComparer<long>
    {
        public bool Equals(long a, long b) { return a == b; }
        public int GetHashCode(long k) { return (int)(((ulong)k * 0x9E3779B97F4A7C15UL) >> 32); }
    }
    static readonly Dictionary<long, int> compareMemo = new Dictionary<long, int>(new PairHash());
    static CultureInfo memoCulture;
    static long sortCalls, sortCompares, sortMemoHits;

    public static void ResyncPrefix()
    {
        if (System.Threading.Thread.CurrentThread.ManagedThreadId != mainThread) return;
        if (sortDepth++ == 0 && sortReady && fastMorphSort.Value && !testSortOff && !bypass) sortKeys = new Dictionary<DAZMorph, MorphKey>(RefEq.Instance);
        sortCalls++;
    }
    public static Exception ResyncFinalizer(Exception __exception)
    {
        if (System.Threading.Thread.CurrentThread.ManagedThreadId != mainThread) return __exception;
        if (--sortDepth <= 0) { sortDepth = 0; sortKeys = null; }
        return __exception;
    }
    public static bool MorphSortPrefix(DAZMorph m1, DAZMorph m2, ref int __result)
    {
        if (sortKeys == null || bypass || (object)m1 == null || (object)m2 == null || System.Threading.Thread.CurrentThread.ManagedThreadId != mainThread) return true;
        int a, b;
        try { a = KeyId(m1); b = KeyId(m2); } catch { return true; }   // VaM's own comparison then fails the same way
        CultureInfo c = CultureInfo.CurrentCulture;
        if (!ReferenceEquals(c, memoCulture) || compareMemo.Count > 2000000 || keyTexts.Count > 400000) {
            compareMemo.Clear();
            if (keyTexts.Count > 400000) { keyTexts.Clear(); keyIds.Clear(); sortKeys.Clear(); a = KeyId(m1); b = KeyId(m2); }
            memoCulture = c;
        }
        sortCompares++;
        long k = ((long)a << 32) | (uint)b;
        int r;
        if (compareMemo.TryGetValue(k, out r)) sortMemoHits++;
        else { r = string.Compare(keyTexts[a], keyTexts[b]); compareMemo[k] = r; }
        __result = r;
        return false;
    }
    static int KeyId(DAZMorph m)
    {
        string region = m.resolvedRegionName, display = m.resolvedDisplayName;
        MorphKey k;
        if (sortKeys.TryGetValue(m, out k) && ReferenceEquals(k.region, region) && ReferenceEquals(k.display, display)) return k.id;
        string text = string.Concat(region, ":", display);
        int id;
        if (!keyIds.TryGetValue(text, out id)) { id = keyTexts.Count; keyTexts.Add(text); keyIds.Add(text, id); }
        if (k == null) { k = new MorphKey(); sortKeys[m] = k; }
        k.region = region; k.display = display; k.id = id;
        return id;
    }

    // FileManager.FindVarFilesRegex(dir, regex, found): for every file of every package (allVarFileEntries, a HashSet),
    // InternalSlashPath.StartsWith(dir) (culture-aware), then Regex.IsMatch(Name, regex, IgnoreCase) -> found.Add.
    // VaM calls it per person for the clothing and hair folders (RefreshDynamicClothes / Hair), each time testing every
    // package file. The prefix test's outcome per file is kept per folder, together with the set's modification
    // counter, the current culture and, per file, the entry and its path string; a call re-walks the set and uses the
    // kept outcome only where all of those are unchanged (otherwise it tests again). The regex part runs as before, on
    // the same files in the same order.
    sealed class PrefixCache { public HashSet<VarFileEntry> set; public int version; public CultureInfo culture; public VarFileEntry[] entries; public string[] paths; public bool[] under; }
    static FieldInfo fAllVar, fSetVersion;
    static readonly Dictionary<string, PrefixCache> prefixCaches = new Dictionary<string, PrefixCache>(StringComparer.Ordinal);
    static long findCalls, findCached, findOriginal;

    public static bool FindVarPrefix(string dir, string regex, List<FileEntry> foundFiles)
    {
        if (!findReady || !fastVarFind.Value || testFindOff || bypass || dir == null || foundFiles == null
            || System.Threading.Thread.CurrentThread.ManagedThreadId != mainThread) { findOriginal++; return true; }
        string d = FileManager.CleanDirectoryPath(dir);
        HashSet<VarFileEntry> all = (HashSet<VarFileEntry>)fAllVar.GetValue(null);
        if (all == null) { findCalls++; return false; }   // VaM: nothing to do
        int version = fSetVersion != null ? (int)fSetVersion.GetValue(all) : 0;   // an early out only: the walk below checks entries and paths
        CultureInfo culture = CultureInfo.CurrentCulture;
        PrefixCache pc;
        bool reuse = prefixCaches.TryGetValue(d, out pc) && ReferenceEquals(pc.set, all) && pc.version == version && ReferenceEquals(pc.culture, culture) && pc.entries.Length == all.Count;
        if (reuse) {
            int i = 0;
            foreach (VarFileEntry e in all) {
                if (i >= pc.entries.Length || !ReferenceEquals(e, pc.entries[i]) || !ReferenceEquals(e.InternalSlashPath, pc.paths[i])) { reuse = false; break; }
                i++;
            }
            if (i != pc.entries.Length) reuse = false;
        }
        if (!reuse) {
            int n = all.Count, i = 0;
            pc = new PrefixCache { set = all, version = version, culture = culture, entries = new VarFileEntry[n], paths = new string[n], under = new bool[n] };
            foreach (VarFileEntry e in all) {
                string p = e.InternalSlashPath;
                if (p == null || i >= n) { findOriginal++; return true; }   // VaM's own (which throws on a null path)
                pc.entries[i] = e; pc.paths[i] = p; pc.under[i] = p.StartsWith(d);
                i++;
            }
            if (i != n) { findOriginal++; return true; }
            prefixCaches[d] = pc;
        } else findCached++;
        findCalls++;
        for (int i = 0; i < pc.entries.Length; i++)
            if (pc.under[i] && System.Text.RegularExpressions.Regex.IsMatch(pc.entries[i].Name, regex, System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                foundFiles.Add(pc.entries[i]);
        return false;
    }

    // SyncAnatomy (DAZCharacterSelector) recomputes, from the person's clothing / hair items (active + flags), gender and
    // skin: the anatomy material slots of the skin and its mesh, the male anatomy colliders, and the breast / glute
    // joint adjustment (AdjustJoints setters, which only act on a change). It only assigns; its result depends on its
    // inputs alone. ResetClothing / ResetHair call SetActive*Item for every item of both genders (thousands), and each
    // of those calls SyncAnatomy at its end. Inside a reset a SyncAnatomy is skipped when no input changed since the
    // last one that ran for that person: the first one always runs; any item whose active flag, GameObject state
    // or joint-adjust flag is about to change marks the state dirty (and its own SyncAnatomy then runs, after its
    // OnEnable / OnDisable). A skipped call would have written exactly the values already there, so the anatomy state is
    // the same as VaM's at every point.
    public static void ResetPrefix(DAZCharacterSelector __instance)
    {
        if (resetDepth == 0) resetSel = __instance;
        resetDepth++;
        anatomyDirty = true;
    }
    public static Exception ResetFinalizer(Exception __exception)
    {
        if (--resetDepth <= 0) { resetDepth = 0; resetSel = null; }
        return __exception;
    }
    static void MarkItem(DAZDynamicItem item, bool active)
    {
        if (resetDepth == 0 || anatomyDirty) return;
        try {
            if ((object)item == null || item == null) return;   // SetActive*Item does nothing for a missing item
            if (item.active != active || item.gameObject.activeSelf != active) anatomyDirty = true;
        } catch { anatomyDirty = true; }
    }
    public static void SetClothingPrefix(DAZClothingItem item, bool active) { MarkItem(item, active); }
    public static void SetHairPrefix(DAZHairGroup item, bool active) { MarkItem(item, active); }
    public static void ActivePrefix(DAZDynamicItem __instance, bool value) { if (resetDepth > 0 && __instance.active != value) anatomyDirty = true; }
    public static void JointAdjustPrefix(DAZClothingItem __instance, bool value) { if (resetDepth > 0 && __instance.jointAdjustEnabled != value) anatomyDirty = true; }
    [ThreadStatic] static bool anatomySkipped;
    public static bool AnatomyPrefix(DAZCharacterSelector __instance)
    {
        anatomySkipped = false;
        if (resetDepth > 0 && !anatomyDirty && ReferenceEquals(__instance, resetSel) && anatomyReady && skipAnatomy.Value && !testAnatomyOff && !bypass) {
            anatomySkips++;
            anatomySkipped = true;
            return false;
        }
        return true;
    }
    // Harmony runs postfixes after a skipping prefix too; this one only counts a SyncAnatomy that ran (and returned)
    public static void AnatomyPostfix(DAZCharacterSelector __instance)
    {
        if (anatomySkipped) { anatomySkipped = false; return; }
        if (ReferenceEquals(__instance, resetSel)) anatomyDirty = false;
        if (resetDepth > 0) anatomyRuns++;
    }

    // RemovePackageFromPath: Regex.Replace(path, ".*:/", "") then Regex.Replace(.., @".*:\\", ""). On a path without
    // '\n' (where '.' stops) the first match of ".*:/" starts at 0 and greedily ends after the last ":/", and nothing
    // after it can match again: the result is what follows the last ":/" (the path itself if there is none); the same
    // for ":\". Paths with '\n', and null (the regex throws), run VaM's own method.
    public static bool RemovePackagePrefix(string path, ref string __result)
    {
        if (!pathReady || !fastPackagePath.Value || testPathOff || bypass || path == null || path.IndexOf('\n') >= 0) {
            System.Threading.Interlocked.Increment(ref pathRegex);
            return true;
        }
        __result = AfterLast(AfterLast(path, ":/"), ":\\");
        System.Threading.Interlocked.Increment(ref pathFast);
        return false;
    }
    static string AfterLast(string s, string sep)
    {
        int i = s.LastIndexOf(sep, StringComparison.Ordinal);
        return i < 0 ? s : s.Substring(i + sep.Length);
    }
    static string RemovePackageRegex(string path)
    {
        string s = System.Text.RegularExpressions.Regex.Replace(path, ".*:/", string.Empty);
        return System.Text.RegularExpressions.Regex.Replace(s, ".*:\\\\", string.Empty);
    }
    static bool PackagePathMatches()
    {
        string[] samples = { "", "a", ":/", ":\\", "x:/", "Author.Package.2:/Custom/Atom/Person/Morphs/a.vmi", "C:\\VaM\\Custom\\x.vam",
            "a:/b:/c", "a:\\b:/c:\\d", "a:/b\\c", "::/:/", ":\\:\\", "a :/ b", "Custom/Clothing/Female/x.vam", "a:/\r", "\u4e2d\u6587:/\u4e2d.vam",
            "SELF:/Custom/Scripts/x.cs", "x:/y/z:\\w:/v" };
        foreach (string s in samples) if (RemovePackageRegex(s) != AfterLast(AfterLast(s, ":/"), ":\\")) return false;
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // 1.1.0 package reads

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int ApiFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long ReadZipEntryFn([MarshalAs(UnmanagedType.LPWStr)] string path, long headerOffset, long compSize, int method, byte* dst, long dstLen);

    static ConfigEntry<bool> fastLookup, fastDeltas;
    static bool lookupReady, deltasReady;
    static ReadZipEntryFn readZipEntry;
    delegate T Getter<T>(ZipFile z);
    static Getter<ZipEntry[]> zEntries;
    static Getter<bool> zDisposed;
    static Getter<long> zFirstOffset;

    // a private ZipFile field read without reflection boxing (DynamicMethod owned by ZipFile)
    static Getter<T> FieldGetter<T>(string name)
    {
        FieldInfo f = typeof(ZipFile).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
        if (f == null || f.FieldType != typeof(T)) throw new MissingFieldException("ZipFile." + name);
        var dm = new System.Reflection.Emit.DynamicMethod("get_" + name, typeof(T), new[] { typeof(ZipFile) }, typeof(ZipFile), true);
        var il = dm.GetILGenerator();
        il.Emit(System.Reflection.Emit.OpCodes.Ldarg_0);
        il.Emit(System.Reflection.Emit.OpCodes.Ldfld, f);
        il.Emit(System.Reflection.Emit.OpCodes.Ret);
        return (Getter<T>)dm.CreateDelegate(typeof(Getter<T>));
    }
    [ThreadStatic] static bool bypass;   // test: this thread runs SharpZipLib's / VaM's own methods
    static long lookups, lookupsIndexed, indexBuilds, deltaFast, deltaFallback;
    static double deltaMs;

    sealed class EntryIndex { public ZipEntry[] entries; public Dictionary<string, int> first; }
    static readonly Dictionary<ZipFile, EntryIndex> indexes = new Dictionary<ZipFile, EntryIndex>();
    static readonly Dictionary<string, string> fullPaths = new Dictionary<string, string>();

    void SetUpPackageReads(Harmony h)
    {
        fastLookup = Config.Bind("PackageRead", "FastEntryLookup", true, "Find files in .var packages through an index instead of SharpZipLib's linear, culture-aware scan of every entry (same answer)");
        fastDeltas = Config.Bind("PackageRead", "FastMorphDeltas", true, "Read a morph's deltas (.vmb) out of a .var package in one native read instead of 4 bytes at a time through SharpZipLib's managed inflate (same values)");
        BindingFlags any = BindingFlags.Instance | BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public;
        try {
            zEntries = FieldGetter<ZipEntry[]>("entries_");
            zDisposed = FieldGetter<bool>("isDisposed_");
            zFirstOffset = FieldGetter<long>("offsetOfFirstEntry");
            if (!AsciiCompareMatches()) throw new Exception("culture-aware and ordinal case-insensitive compares differ on printable ASCII here");
            MethodInfo find = typeof(ZipFile).GetMethod("FindEntry", any, null, new[] { typeof(string), typeof(bool) }, null);
            MethodInfo dispose = typeof(ZipFile).GetMethod("DisposeInternal", any, null, new[] { typeof(bool) }, null);
            if (find == null || find.ReturnType != typeof(int) || dispose == null) throw new MissingMethodException("ZipFile.FindEntry / DisposeInternal");
            h.Patch(find, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("FindEntryPrefix", any)), null, null, null);
            h.Patch(dispose, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("DisposePostfix", any)), null, null);
            lookupReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: package entry index off: " + e.Message);
        }
        try {
            string bin = Path.Combine(Path.GetDirectoryName(Info.Location), "VaMSceneLoadNative.bin");
            IntPtr m = LoadLibraryW(bin);
            if (m == IntPtr.Zero) throw new FileNotFoundException("cannot load " + bin);
            IntPtr api = GetProcAddress(m, "SL_ApiVersion"), read = GetProcAddress(m, "SL_ReadZipEntry");
            if (api == IntPtr.Zero || read == IntPtr.Zero || ((ApiFn)Marshal.GetDelegateForFunctionPointer(api, typeof(ApiFn)))() != 1) throw new EntryPointNotFoundException("SL_ReadZipEntry (API 1)");
            readZipEntry = (ReadZipEntryFn)Marshal.GetDelegateForFunctionPointer(read, typeof(ReadZipEntryFn));
            MethodInfo load = typeof(DAZMorph).GetMethod("LoadDeltasFromBinaryFile", any, null, new[] { typeof(string) }, null);
            if (load == null) throw new MissingMethodException("DAZMorph.LoadDeltasFromBinaryFile");
            if (zFirstOffset == null) zFirstOffset = FieldGetter<long>("offsetOfFirstEntry");
            h.Patch(load, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("DeltasPrefix", any)), null, null, null);
            deltasReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: fast morph deltas off: " + e.Message);
        }
        Logger.LogInfo("VaM scene load: package entry index " + (lookupReady && fastLookup.Value ? "on" : "off") + ", fast morph deltas " + (deltasReady && fastDeltas.Value ? "on" : "off"));
    }

    // FindEntry's case-insensitive compare is string.Compare(a, b, true, InvariantCulture) == 0. On printable ASCII it
    // must agree with ordinal case-insensitive equality for the index to give FindEntry's answer: every pair of
    // characters, and no character that the culture compare would skip ("x" + c + "y" never equals "xy").
    static bool AsciiCompareMatches()
    {
        CultureInfo inv = CultureInfo.InvariantCulture;
        for (int c = 0x20; c <= 0x7E; c++) {
            string a = "x" + (char)c;
            if (string.Compare("x" + (char)c + "y", "xy", true, inv) == 0) return false;
            if (string.Compare("x" + (char)c + (char)c + "y", "x" + (char)c + "y", true, inv) == 0) return false;
            for (int d = 0x20; d <= 0x7E; d++) {
                string b = "x" + (char)d;
                if ((string.Compare(a, b, true, inv) == 0) != string.Equals(a, b, StringComparison.OrdinalIgnoreCase)) return false;
            }
        }
        return true;
    }

    static bool Printable(string s)
    {
        for (int i = 0; i < s.Length; i++) { char c = s[i]; if (c < 0x20 || c > 0x7E) return false; }
        return true;
    }

    public static bool FindEntryPrefix(ZipFile __instance, string name, bool ignoreCase, ref int __result)
    {
        if (!lookupReady || !fastLookup.Value || testLookupOff || bypass || !ignoreCase || name == null) return true;
        if (zDisposed(__instance)) return true;
        ZipEntry[] entries = zEntries(__instance);
        if (entries == null) return true;
        System.Threading.Interlocked.Increment(ref lookups);
        EntryIndex ix = IndexOf(__instance, entries);
        if (ix.first == null || !Printable(name)) return true;
        int i;
        __result = ix.first.TryGetValue(name, out i) ? i : -1;
        System.Threading.Interlocked.Increment(ref lookupsIndexed);
        return false;
    }

    static EntryIndex IndexOf(ZipFile zf, ZipEntry[] entries)
    {
        lock (indexes) {
            EntryIndex ix;
            if (indexes.TryGetValue(zf, out ix) && ReferenceEquals(ix.entries, entries)) return ix;
            ix = new EntryIndex { entries = entries };
            Dictionary<string, int> first = new Dictionary<string, int>(entries.Length, StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < entries.Length; i++) {
                string n = entries[i].Name;
                if (n == null || !Printable(n)) { first = null; break; }   // this package: SharpZipLib's own scan
                if (!first.ContainsKey(n)) first.Add(n, i);
            }
            ix.first = first;
            indexes[zf] = ix;
            indexBuilds++;
            return ix;
        }
    }

    public static void DisposePostfix(ZipFile __instance) { lock (indexes) indexes.Remove(__instance); }

    public static bool DeltasPrefix(DAZMorph __instance, string path)
    {
        if (!deltasReady || !fastDeltas.Value || testDeltasOff || bypass) return true;
        double t0 = Now();
        try {
            byte[] data = ReadPackageEntry(path);
            if (data == null || data.Length < 4) { System.Threading.Interlocked.Increment(ref deltaFallback); return true; }
            int n;
            DAZMorphVertex[] deltas;
            fixed (byte* p = data) {
                n = *(int*)p;
                if (n < 0 || 4L + 16L * n > data.Length) { System.Threading.Interlocked.Increment(ref deltaFallback); return true; }
                deltas = new DAZMorphVertex[n];
                byte* q = p + 4;
                for (int i = 0; i < n; i++, q += 16) {
                    DAZMorphVertex v = new DAZMorphVertex();
                    v.vertex = *(int*)q;
                    Vector3 d;
                    d.x = *(float*)(q + 4); d.y = *(float*)(q + 8); d.z = *(float*)(q + 12);
                    v.delta = d;
                    deltas[i] = v;
                }
            }
            __instance.numDeltas = n;
            __instance.deltas = deltas;
            System.Threading.Interlocked.Increment(ref deltaFast);
            lock (indexes) deltaMs += Now() - t0;
            return false;
        } catch {
            System.Threading.Interlocked.Increment(ref deltaFallback);
            return true;
        }
    }

    // The bytes of a file inside a (not unpacked) .var package, as VaM's VarFileEntryStream would read them;
    // null when this is not such a file or the native reader refuses it.
    static byte[] ReadPackageEntry(string path)
    {
        VarFileEntry ve = FileManager.GetFileEntry(path, true) as VarFileEntry;
        if (ve == null || ve.Simulated || ve.Package == null) return null;
        ZipFile zf = ve.Package.ZipFile;
        if (zf == null) return null;
        ZipEntry ze = zf.GetEntry(ve.InternalSlashPath);
        if (ze == null || ze.IsCrypted || !ze.IsFile) return null;
        int method = (int)ze.CompressionMethod;
        long size = ze.Size, csize = ze.CompressedSize;
        if ((method != 0 && method != 8) || size < 0 || size > int.MaxValue || csize < 0) return null;
        string full;
        lock (fullPaths) {
            if (!fullPaths.TryGetValue(ve.Package.Path, out full)) fullPaths[ve.Package.Path] = full = Path.GetFullPath(ve.Package.Path);
        }
        byte[] data = new byte[size];
        long r;
        fixed (byte* d = data) r = readZipEntry(full, zFirstOffset(zf) + ze.Offset, csize, method, d, size);
        return r == size ? data : null;
    }

    static FieldInfo Need(FieldInfo f, string what) { if (f == null) throw new MissingFieldException(what); return f; }

    public sealed class State { public string key, prefix, path; public long counter, name; public bool files; public double t0; }

    // __0: the sources (CompileSources) or file paths (CompileFiles); __1: extra assembly references
    public static bool Prefix(object __instance, MethodBase __originalMethod, string[] __0, string[] __1, ref bool __result, out State __state)
    {
        __state = null;
        if (!cacheReady || !useCache.Value || testOff || __0 == null) return true;
        try {
            CompilerParameters p = fParameters.GetValue(fCompiler.GetValue(__instance)) as CompilerParameters;
            string prefix = p != null ? p.OutputAssembly : null;
            if (prefix == null || !prefix.StartsWith("MVRPlugin_", StringComparison.Ordinal)) return true;
            bool files = __originalMethod.Name == "CompileFiles";
            string key = Key(prefix, p.CompilerOptions, __1, files, __0);
            string path = Path.Combine(dir, key + ".plc");
            double t0 = Now();
            if (File.Exists(path)) {
                byte[] symbols;
                byte[] data = Load(path, prefix, out symbols);
                if (data != null) {
                    fAssemblyData.SetValue(__instance, data);
                    fSymbolsData.SetValue(__instance, symbols);
                    fWarnings.SetValue(__instance, new string[0]);
                    fErrors.SetValue(__instance, new string[0]);
                    fIsCompiling.SetValue(__instance, false);
                    __result = true;
                    hits++; hitMs += Now() - t0;
                    return false;
                }
            }
            // a miss: this one compile gets a 9-digit counter, so the stored name has room for later names
            State s = new State { key = key, prefix = prefix, path = path, files = files, t0 = t0 };
            s.counter = (long)fCounter.GetValue(null);
            s.name = NextName();
            fCounter.SetValue(null, s.name);
            __state = s;
        } catch (Exception e) {
            failures++;
            if (failures <= 3) log.LogWarning("VaM scene load: plugin cache: compiling as usual: " + e.Message);
        }
        return true;
    }

    public static void Postfix(object __instance, bool __result, State __state)
    {
        if (__state == null) return;
        try {
            // VaM's own sequence goes on as if this compile had used it
            fCounter.SetValue(null, __state.counter + 1);
            misses++; missMs += Now() - __state.t0;
            byte[] data = __result ? fAssemblyData.GetValue(__instance) as byte[] : null;
            if (data == null) return;
            byte[] symbols = fSymbolsData.GetValue(__instance) as byte[];
            string name = __state.prefix + "_" + __state.name.ToString();
            if (Find(data, Encoding.UTF8.GetBytes(name)).Count == 0) { log.LogWarning("VaM scene load: plugin cache: not stored, assembly name not found: " + name); return; }
            Directory.CreateDirectory(dir);
            byte[] nameBytes = Encoding.UTF8.GetBytes(name);
            string temp = __state.path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            using (FileStream fs = new FileStream(temp, FileMode.CreateNew, FileAccess.Write)) {
                BinaryWriter w = new BinaryWriter(fs);
                w.Write(Format); w.Write(nameBytes.Length); w.Write(nameBytes); w.Write(data.Length); w.Write(data);
                w.Write(symbols != null ? symbols.Length : -1); if (symbols != null) w.Write(symbols);
                w.Flush();
            }
            if (File.Exists(__state.path)) File.Delete(temp); else File.Move(temp, __state.path);
            stored++;
        } catch (Exception e) {
            failures++;
            if (failures <= 3) log.LogWarning("VaM scene load: plugin cache: not stored: " + e.Message);
        }
    }

    // The cached assembly with a fresh 9-digit name (all places the stored name occurs) and a fresh module MVID;
    // its debug symbols (mono .mdb, when VaM made them) name the module by that MVID and get the same new one.
    static byte[] Load(string path, string prefix, out byte[] symbols)
    {
        byte[] stored, data;
        symbols = null;
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            BinaryReader r = new BinaryReader(fs);
            if (r.ReadString() != Format) return null;
            stored = r.ReadBytes(r.ReadInt32());
            data = r.ReadBytes(r.ReadInt32());
            int n = r.ReadInt32();
            if (n >= 0) symbols = r.ReadBytes(n);
        }
        string storedName = Encoding.UTF8.GetString(stored);
        if (!storedName.StartsWith(prefix + "_", StringComparison.Ordinal) || storedName.Length != prefix.Length + 10) return null;
        List<int> at = Find(data, stored);
        if (at.Count == 0) return null;
        byte[] digits = Encoding.ASCII.GetBytes(NextName().ToString());
        if (digits.Length != 9) return null;
        int tail = stored.Length - 9;
        foreach (int i in at) Buffer.BlockCopy(digits, 0, data, i + tail, 9);
        int mvid = MvidOffset(data);
        if (mvid < 0) { symbols = null; return data; }
        byte[] old = new byte[16];
        Buffer.BlockCopy(data, mvid, old, 0, 16);
        byte[] fresh = Guid.NewGuid().ToByteArray();
        Buffer.BlockCopy(fresh, 0, data, mvid, 16);
        if (symbols != null) {
            List<int> inSymbols = Find(symbols, old);
            if (inSymbols.Count == 0) symbols = null;
            else foreach (int i in inSymbols) Buffer.BlockCopy(fresh, 0, symbols, i, 16);
        }
        return data;
    }

    static long NextName() { return System.Threading.Interlocked.Increment(ref nextName); }

    // ECMA-335 II.24.2.1: the metadata root ("BSJB"), its stream headers, the #GUID heap; the module's MVID is GUID #1
    static int MvidOffset(byte[] d)
    {
        List<int> roots = Find(d, new byte[] { 0x42, 0x53, 0x4A, 0x42 });
        foreach (int root in roots) {
            if (root + 16 > d.Length) continue;
            int versionLength = BitConverter.ToInt32(d, root + 12);
            int h = root + 16 + versionLength;
            if (versionLength <= 0 || versionLength > 255 || h + 4 > d.Length) continue;
            int streams = BitConverter.ToUInt16(d, h + 2);
            h += 4;
            for (int s = 0; s < streams && h + 8 < d.Length; s++) {
                int offset = BitConverter.ToInt32(d, h), size = BitConverter.ToInt32(d, h + 4);
                int n = h + 8, e = n;
                while (e < d.Length && d[e] != 0) e++;
                string name = Encoding.ASCII.GetString(d, n, e - n);
                if (name == "#GUID" && size >= 16 && root + offset + 16 <= d.Length) return root + offset;
                h = n + ((e - n + 4) & ~3);
            }
        }
        return -1;
    }

    static List<int> Find(byte[] data, byte[] what)
    {
        List<int> at = new List<int>();
        for (int i = 0; i + what.Length <= data.Length; i++) {
            if (data[i] != what[0]) continue;
            int k = 1;
            while (k < what.Length && data[i + k] == what[k]) k++;
            if (k == what.Length) at.Add(i);
        }
        return at;
    }

    static string Key(string prefix, string options, string[] references, bool files, string[] inputs)
    {
        using (SHA1 sha = SHA1.Create()) {
            MemoryStream m = new MemoryStream();
            BinaryWriter w = new BinaryWriter(m, Encoding.UTF8);
            w.Write(Format); w.Write(Environment()); w.Write(prefix); w.Write(options ?? "");
            w.Write(references != null ? references.Length : -1);
            if (references != null) foreach (string r in references) w.Write(r ?? "");
            w.Write(files); w.Write(inputs.Length);
            foreach (string input in inputs) {
                if (files) { w.Write(input ?? ""); byte[] b = File.ReadAllBytes(input); w.Write(b.Length); w.Write(b); }
                else w.Write(input ?? "");
            }
            w.Flush();
            byte[] hash = sha.ComputeHash(m.ToArray());
            StringBuilder sb = new StringBuilder(40);
            foreach (byte b in hash) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
    }

    // VaM_Data\Managed's assemblies (VaM, Unity, mono's libraries, mcs) by name, size and time: a VaM update recompiles
    static string Environment()
    {
        if (environment != null) return environment;
        List<string> ids = new List<string>();
        foreach (string f in Directory.GetFiles(Path.Combine(Paths.GameRootPath, "VaM_Data\\Managed"), "*.dll")) {
            FileInfo fi = new FileInfo(f);
            ids.Add(fi.Name.ToLowerInvariant() + ":" + fi.Length + ":" + fi.LastWriteTimeUtc.Ticks);
        }
        ids.Sort(StringComparer.Ordinal);
        return environment = string.Join(";", ids.ToArray());
    }

    static double Now() { return System.Diagnostics.Stopwatch.GetTimestamp() * 1000.0 / System.Diagnostics.Stopwatch.Frequency; }

    // test interface (this run only, the cfg is not touched)
    static bool testOff, testLookupOff, testDeltasOff, testAnatomyOff, testPathOff, testSortOff, testFindOff;
    public static string SetTestOff(bool off) { testOff = off; return Stats(); }
    public static string SetPackageTestOff(bool lookupOff, bool deltasOff) { testLookupOff = lookupOff; testDeltasOff = deltasOff; return Stats(); }
    public static string SetLoadPathsTestOff(bool anatomyOff, bool pathOff) { testAnatomyOff = anatomyOff; testPathOff = pathOff; return Stats(); }
    public static string SetSearchTestOff(bool sortOff, bool findOff) { testSortOff = sortOff; testFindOff = findOff; prefixCaches.Clear(); return Stats(); }

    // Every morph list of the scene's people (built as ResyncMorphs builds it), sorted with VaM's comparison and with
    // the fast one (a fresh sort context, then the same list sorted again, which reuses remembered comparisons):
    // the same morphs in the same order.
    public static string VerifyMorphSort()
    {
        MethodInfo sort = typeof(GenerateDAZMorphsControlUI).GetMethod("CustomMorphSort", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        Comparison<DAZMorph> cmp = (Comparison<DAZMorph>)Delegate.CreateDelegate(typeof(Comparison<DAZMorph>), sort);
        int lists = 0, morphs = 0, diff = 0;
        double tVaM = 0, tFast = 0, tAgain = 0;
        StringBuilder sb = new StringBuilder();
        foreach (GenerateDAZMorphsControlUI ui in Resources.FindObjectsOfTypeAll<GenerateDAZMorphsControlUI>()) {   // inactive ones too
            if (ui == null || !ui.gameObject.scene.IsValid()) continue;
            List<DAZMorph> list = new List<DAZMorph>();
            Traverse t = Traverse.Create(ui);
            foreach (string f in new[] { "_morphBank1", "_morphBank2", "_morphBank3" }) {
                DAZMorphBank bank = t.Field(f).GetValue() as DAZMorphBank;
                if (bank != null && bank.morphs != null) foreach (DAZMorph m in bank.morphs) if (m.visible) list.Add(m);
            }
            if (list.Count == 0) continue;
            lists++; morphs += list.Count;
            List<DAZMorph> a = new List<DAZMorph>(list), b = new List<DAZMorph>(list), c = new List<DAZMorph>(list);
            double t0 = Now();
            bypass = true;
            try { a.Sort(cmp); } finally { bypass = false; }
            double t1 = Now();
            Dictionary<DAZMorph, MorphKey> saved = sortKeys; int savedDepth = sortDepth;
            sortKeys = new Dictionary<DAZMorph, MorphKey>(RefEq.Instance); sortDepth = 1;
            try {
                compareMemo.Clear();
                b.Sort(cmp);
                double t2 = Now();
                c.Sort(cmp);
                double t3 = Now();
                tFast += t2 - t1; tAgain += t3 - t2;
            } finally { sortKeys = saved; sortDepth = savedDepth; }
            tVaM += t1 - t0;
            for (int i = 0; i < list.Count; i++)
                if (!ReferenceEquals(a[i], b[i]) || !ReferenceEquals(a[i], c[i])) { diff++; if (diff <= 5) sb.Append(" | ").Append(i).Append(' ').Append(a[i].resolvedDisplayName); }
        }
        return "morph lists " + lists + " (" + morphs + " morphs), positions different " + diff + "; VaM " + tVaM.ToString("F0") + " ms, fast " + tFast.ToString("F0") + " ms, again " + tAgain.ToString("F0") + " ms" + sb;
    }

    // FindVarFilesRegex by VaM's own method and the fast one (first call, then again from the kept prefix results),
    // for the folders and patterns given ("dir|regex" pairs joined by ';'): the same files in the same order.
    public static string VerifyVarFind(string joined)
    {
        string[] queries = joined.Split(';');
        MethodInfo find = typeof(FileManager).GetMethod("FindVarFilesRegex", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public, null, new[] { typeof(string), typeof(string), typeof(List<FileEntry>) }, null);
        int n = 0, diff = 0, files = 0;
        double tVaM = 0, tFast = 0, tAgain = 0;
        StringBuilder sb = new StringBuilder();
        foreach (string q in queries) {
            int bar = q.IndexOf('|');
            string d = q.Substring(0, bar), rx = q.Substring(bar + 1);
            List<FileEntry> a = new List<FileEntry>(), b = new List<FileEntry>(), c = new List<FileEntry>();
            double t0 = Now();
            bypass = true;
            try { find.Invoke(null, new object[] { d, rx, a }); } finally { bypass = false; }
            double t1 = Now();
            prefixCaches.Remove(FileManager.CleanDirectoryPath(d));
            find.Invoke(null, new object[] { d, rx, b });
            double t2 = Now();
            find.Invoke(null, new object[] { d, rx, c });
            double t3 = Now();
            tVaM += t1 - t0; tFast += t2 - t1; tAgain += t3 - t2;
            n++; files += a.Count;
            bool same = a.Count == b.Count && a.Count == c.Count;
            for (int i = 0; same && i < a.Count; i++) same = ReferenceEquals(a[i], b[i]) && ReferenceEquals(a[i], c[i]);
            if (!same) { diff++; sb.Append(" | ").Append(q).Append(' ').Append(a.Count).Append('/').Append(b.Count).Append('/').Append(c.Count); }
            else sb.Append(" | ").Append(q).Append(' ').Append(a.Count);
        }
        return "searches " + n + " (" + files + " files), different " + diff + "; VaM " + tVaM.ToString("F0") + " ms, fast " + tFast.ToString("F0") + " ms, again " + tAgain.ToString("F0") + " ms"
            + (fSetVersion != null ? "" : " (no set counter)") + sb;
    }
    public static string Stats()
    {
        return "plugin cache hits " + hits + " (" + hitMs.ToString("F0") + " ms), misses " + misses + " (" + missMs.ToString("F0") + " ms compiling), stored " + stored + ", failures " + failures
            + " | entry lookups " + lookups + " (indexed " + lookupsIndexed + ", " + indexBuilds + " package indexes)"
            + " | morph deltas fast " + deltaFast + " (" + deltaMs.ToString("F0") + " ms), VaM's own " + deltaFallback
            + " | anatomy syncs in resets run " + anatomyRuns + ", skipped " + anatomySkips + " | RemovePackageFromPath fast " + pathFast + ", regex " + pathRegex
            + " | morph sorts " + sortCalls + ", comparisons " + sortCompares + " (remembered " + sortMemoHits + ")"
            + " | package file searches " + findCalls + " (kept prefix results " + findCached + "), VaM's own " + findOriginal
            + " | " + BundleStats() + " | " + LoadFramesStats() + " | " + SwitchStats();
    }

    // Every person's anatomy outputs now, and again after VaM's own SyncAnatomy runs once more: equal when the state left
    // by the load is what SyncAnatomy makes of the current inputs (with and without the skipping).
    public static string VerifyAnatomy()
    {
        MethodInfo syncAnatomyMethod = typeof(DAZCharacterSelector).GetMethod("SyncAnatomy", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public, null, Type.EmptyTypes, null);
        StringBuilder sb = new StringBuilder();
        int same = 0, diff = 0;
        foreach (DAZCharacterSelector cs in UnityEngine.Object.FindObjectsOfType<DAZCharacterSelector>()) {
            string before = AnatomyState(cs);
            bypass = true;
            try { syncAnatomyMethod.Invoke(cs, null); } finally { bypass = false; }
            string after = AnatomyState(cs);
            string who = cs.containingAtom != null ? cs.containingAtom.uid : cs.name;
            if (before == after) same++; else { diff++; sb.Append(who).Append(": ").Append(before).Append(" -> ").Append(after).Append('\n'); }
            sb.Append(who).Append(' ').Append(before.Length > 300 ? before.Substring(0, 300) + "..." : before).Append('\n');
        }
        return "people " + (same + diff) + ", same " + same + ", different " + diff + "\n" + sb;
    }
    static string AnatomyState(DAZCharacterSelector cs)
    {
        StringBuilder sb = new StringBuilder();
        Traverse t = Traverse.Create(cs);
        object character = t.Field("_selectedCharacter").GetValue();
        DAZSkinV2 skin = character != null ? Traverse.Create(character).Property("skin", null).GetValue() as DAZSkinV2 : null;
        if (skin != null) {
            sb.Append("skin ").Append(Bits(skin.materialsEnabled));
            if (skin.dazMesh != null) sb.Append(" mesh ").Append(Bits(skin.dazMesh.materialsEnabled));
        }
        foreach (string f in new[] { "femaleBreastAdjustJoints", "femaleGluteAdjustJoints" }) {
            AdjustJoints aj = t.Field(f).GetValue() as AdjustJoints;
            if (aj != null) sb.Append(' ').Append(f).Append(' ').Append(aj.springDamperMultiplierOn).Append(' ').Append(aj.springDamperMultiplier.ToString("R"));
        }
        Component[] male = t.Field("maleAnatomyComponents").GetValue() as Component[];
        if (male != null) foreach (Component c in male) if (c != null) foreach (Rigidbody rb in c.GetComponentsInChildren<Rigidbody>()) sb.Append(rb.detectCollisions ? '1' : '0');
        return sb.ToString();
    }
    static string Bits(bool[] a)
    {
        if (a == null) return "null";
        char[] c = new char[a.Length];
        for (int i = 0; i < a.Length; i++) c[i] = a[i] ? '1' : '0';
        return new string(c);
    }

    // RemovePackageFromPath fast vs VaM's regular expressions on every file uid and path VaM knows about
    public static string VerifyPackagePath(int max)
    {
        FieldInfo f = typeof(FileManager).GetField("allVarFileEntries", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        System.Collections.IEnumerable all = f != null ? f.GetValue(null) as System.Collections.IEnumerable : null;
        if (all == null) return "no allVarFileEntries";
        int n = 0, bad = 0;
        string sample = "";
        double tFast = 0, tRegex = 0;
        foreach (object o in all) {
            VarFileEntry e = o as VarFileEntry;
            if (e == null) continue;
            foreach (string s in new[] { e.Uid, e.InternalSlashPath, e.Path, e.Uid.Replace('/', '\\'), "SELF:/" + e.InternalSlashPath }) {
                if (s == null) continue;
                double t0 = Now(); string a = AfterLast(AfterLast(s, ":/"), ":\\"); double t1 = Now(); string b = RemovePackageRegex(s); double t2 = Now();
                tFast += t1 - t0; tRegex += t2 - t1; n++;
                if (a != b) { bad++; if (sample.Length < 400) sample += s + " -> " + a + " | " + b + "\n"; }
            }
            if (n >= max) break;
        }
        return "paths " + n + ", different " + bad + ", fast " + tFast.ToString("F0") + " ms vs regex " + tRegex.ToString("F0") + " ms\n" + sample;
    }

    // SharpZipLib 0.86's FindEntry, as compiled (the reference the index is checked against)
    static int FindEntryOriginal(ZipEntry[] entries, string name, bool ignoreCase)
    {
        for (int i = 0; i < entries.Length; i++)
            if (string.Compare(name, entries[i].Name, ignoreCase, CultureInfo.InvariantCulture) == 0) return i;
        return -1;
    }

    // Every entry name of up to maxPackages loaded packages (and an upper-case, a lower-case and a missing variant of
    // each) looked up through the index and through SharpZipLib's scan; returns the counts and any difference.
    public static string VerifyLookup(int maxPackages)
    {
        int packages = 0, names = 0, diff = 0, unindexed = 0;
        double tIndex = 0, tScan = 0;
        StringBuilder sb = new StringBuilder();
        var seen = new HashSet<ZipFile>();
        foreach (VarPackage pkg in FileManager.GetPackages()) {
            if (packages >= maxPackages) break;
            ZipFile zf = pkg.ZipFile;
            if (zf == null || zDisposed(zf) || !seen.Add(zf)) continue;
            ZipEntry[] entries = zEntries(zf);
            if (entries == null || entries.Length == 0) continue;
            packages++;
            EntryIndex ix = IndexOf(zf, entries);
            if (ix.first == null) { unindexed++; continue; }
            int step = Math.Max(1, entries.Length / 30);   // up to ~30 names per package (the scan is the slow side)
            for (int i = 0; i < entries.Length; i += step) {
                string n = entries[i].Name;
                foreach (string q in new[] { n, n.ToUpperInvariant(), n.ToLowerInvariant(), n + "_missing" }) {
                    names++;
                    double a = Now();
                    int fast = zf.FindEntry(q, true);
                    double b = Now();
                    int slow = FindEntryOriginal(entries, q, true);
                    double c = Now();
                    tIndex += b - a; tScan += c - b;
                    if (fast != slow) { diff++; if (diff <= 5) sb.Append(" | ").Append(pkg.Uid).Append(": ").Append(q).Append(' ').Append(fast).Append(" vs ").Append(slow); }
                }
            }
        }
        return "packages " + packages + " (" + unindexed + " not indexed: non-ASCII names), lookups " + names + ", differences " + diff
            + ", index " + tIndex.ToString("F0") + " ms vs scan " + tScan.ToString("F0") + " ms" + sb;
    }

    // Package morphs of the scene's people: deltas read the fast way and by VaM's own method, on fresh DAZMorph
    // objects, compared value by value (vertex index and the bits of x, y, z) and timed.
    public static string VerifyDeltas(int max)
    {
        int tried = 0, same = 0, diff = 0, fastFell = 0;
        double tFast = 0, tVaM = 0;
        StringBuilder sb = new StringBuilder();
        var paths = new HashSet<string>();
        foreach (DAZMorphBank bank in UnityEngine.Object.FindObjectsOfType<DAZMorphBank>()) {
            if (bank.morphs == null) continue;
            foreach (DAZMorph m in bank.morphs) {
                if (tried >= max) break;
                string p = m.deltasLoadPath;
                if (string.IsNullOrEmpty(p) || !m.isInPackage || !paths.Add(p)) continue;
                tried++;
                DAZMorph a = new DAZMorph(), b = new DAZMorph();
                long before = deltaFallback;
                double t0 = Now();
                a.LoadDeltasFromBinaryFile(p);
                double t1 = Now();
                bypass = true;
                try { b.LoadDeltasFromBinaryFile(p); } finally { bypass = false; }
                double t2 = Now();
                tFast += t1 - t0; tVaM += t2 - t1;
                if (deltaFallback != before) fastFell++;
                bool eq = a.numDeltas == b.numDeltas && (a.deltas == null) == (b.deltas == null)
                          && (a.deltas == null || a.deltas.Length == b.deltas.Length);
                if (eq && a.deltas != null) {
                    for (int i = 0; i < a.deltas.Length && eq; i++) {
                        DAZMorphVertex x = a.deltas[i], y = b.deltas[i];
                        if ((x == null) != (y == null)) { eq = false; break; }
                        if (x == null) continue;
                        eq = x.vertex == y.vertex && Bits(x.delta.x) == Bits(y.delta.x) && Bits(x.delta.y) == Bits(y.delta.y) && Bits(x.delta.z) == Bits(y.delta.z);
                    }
                }
                if (eq) same++; else { diff++; if (diff <= 5) sb.Append(" | ").Append(p); }
            }
        }
        return "morphs " + tried + ": same " + same + ", different " + diff + ", read by VaM's own method in the fast path " + fastFell
            + "; fast " + tFast.ToString("F0") + " ms vs VaM " + tVaM.ToString("F0") + " ms" + sb;
    }

    static int Bits(float f) { return BitConverter.ToInt32(BitConverter.GetBytes(f), 0); }
}
