// VaM Scene Load: asset bundles of .var packages ([BundleLoad] / [Audio] in local.vam.sceneload.cfg).
//
// How VaM loads them (MeshVR.AssetLoader): every CustomUnityAsset atom, on or off, queues its bundle while the scene
// restores (QueueLoadAssetBundleFromFile); one coroutine takes the queue one request at a time, a frame between
// requests. For a bundle not loaded yet (LoadBundleFileAsync) it inflates the zip entry into a new managed byte[] on a
// thread of its own and waits for it, then AssetBundle.LoadFromMemoryAsync and waits for that, which for an LZMA bundle
// decompresses the whole bundle on Unity's one loading thread; then the request's callback (for a CUA: LoadAsset +
// Instantiate of its prefab). The scene load waits until every CUA's callback ran. A large stage scene: 322 CUAs (310
// off) queue 321 requests for 184 bundles, 3.3 GB of entries, 154 of them LZMA (4.8 GB once decompressed: about 85 s
// at the 55 MB/s one core decodes LZMA here), all of it one after another, plus at least two frames per request.
//
// What this changes, with the same bundles, the same callbacks, in the same order:
//   InMemory: when a request is queued, its bundle is prepared in the background (VaMSceneLoadNative.bin,
//     bundlecache.cpp: on CPU Boost's workers, in queue order) and kept in memory, nothing written to disk: an LZMA
//     bundle as the same archive stored uncompressed (the way Unity 2018.1 writes an uncompressed bundle; the archive's
//     files byte for byte what Unity would decompress), any other bundle as its entry's bytes. When the request's turn
//     comes Unity opens it with AssetBundle.LoadFromStreamAsync on a Stream over that memory instead of VaM's read +
//     LoadFromMemoryAsync; the memory goes when the AssetBundle is unloaded (1.3.x kept files in
//     BepInEx\cache\VaMSceneLoad\bundles). A request with nothing prepared (loose files, unpacked packages, a failed
//     job, a bundle Unity refuses this way) runs VaM's own steps, which then behave and log as before; after the second
//     refused bundle this stays off for the run.
//   ParallelDecode ([Audio]): AudioClips that are DecompressOnLoad + Vorbis (FMOD decodes each on Unity's main thread
//     when the clip loads: a scene's voice pack, 371 clips, 24 s in one frame) reach Unity as PCM16 instead
//     (m_CompressionFormat PCM, the same frames / channels / Hz / load settings): the first read of any of a bundle's
//     clips decodes all of them on CPU Boost's workers, a clip not done yet is decoded by the reading thread itself,
//     and each buffer goes once Unity read it. The decoder is libvorbis with FMOD's rounding: about one value in 5000
//     differs by 1 from FMOD's own. Decoded clips not read yet stop the background
//     decoding at DecodeAheadMB.
//   SameFrame: the queue coroutine hands over the next request in the same frame while this frame has spent less than
//     FrameBudgetMs on requests, instead of one request per frame; bundles of upcoming requests that are prepared are
//     opened ahead (LoadFromStreamAsync, in queue order), so most requests need no frame of their own.
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using ICSharpCode.SharpZipLib.Zip;
using MVR.FileManagement;
using UnityEngine;
using Loader = MeshVR.AssetLoader;
using Request = MeshVR.AssetLoader.AssetBundleFromFileRequest;

public sealed unsafe partial class VaMSceneLoad
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int BundleSubmitFn([MarshalAs(UnmanagedType.LPWStr)] string zip, long headerOffset, long compSize, int method, long size, int audio);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int BundleResultFn(int id, out long length, out double ms, out int clips);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int BundleReadFn(int id, long pos, byte* dst, int count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void BundleFreeFn(int id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void AudioConfigFn(int on, long cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int AudioStatsFn(double* values, int n);

    static ConfigEntry<bool> bundleMemory, sameFrame, parallelDecode;
    static ConfigEntry<int> frameBudgetMs, decodeAheadMB;
    static bool bundlesReady, pacingReady, bundlesBroken, testBundlesOff, testSameFrameOff;
    static BundleSubmitFn bundleSubmit;
    static BundleResultFn bundleResult;
    static BundleReadFn bundleRead;
    static BundleFreeFn bundleFree;
    static AudioConfigFn audioConfig;
    static AudioStatsFn audioStats;
    static IntPtr bundleOnCb;
    static FieldInfo fSingleton, fCounts, fBundles, fQueue, fProcThis, fProcCurrent, fProcPC;

    // state: 0 job running, 1 ready, -1 failed, 2 opened (its stream owns the memory), 3 dropped
    sealed class Prep { public string path; public int job, state, users, clips; public long length; }
    sealed class Opening { public AssetBundleCreateRequest req; public BundleStream stream; }
    sealed class Live { public AssetBundle ab; public BundleStream stream; public long length; }
    static readonly Dictionary<string, Opening> ahead = new Dictionary<string, Opening>();   // path -> opened ahead
    static readonly Dictionary<string, Prep> prepByPath = new Dictionary<string, Prep>();
    static readonly Dictionary<Request, Prep> prepByRequest = new Dictionary<Request, Prep>();
    static readonly List<Prep> pending = new List<Prep>();
    static readonly List<Live> live = new List<Live>();
    static readonly Dictionary<object, IEnumerator> procs = new Dictionary<object, IEnumerator>();
    static long bSubmitted, bLzma, bAudio, bAsIs, bFailed, bNotOurs, bFromMemory, bVaM, bEarly, bForced, bWaitFrames, bSameFrame, bRefused, bDropped, bUnloaded, bPcmClips;
    static double bBytes, bJobMs, bWaitMs;
    static long bReads, bReadTicks;
    static int bLastFail;
    static bool bLogged = true;
    static float nextReap;

    // A prepared bundle as the Stream Unity reads (Read / Seek / Length, maybe from its loading thread): the native
    // memory, which goes with Dispose.
    sealed class BundleStream : Stream
    {
        readonly int id;
        readonly long length;
        long pos;
        bool closed;
        public BundleStream(int id, long length) { this.id = id; this.length = length; }
        public override bool CanRead { get { return !closed; } }
        public override bool CanSeek { get { return !closed; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { return length; } }
        public override long Position { get { return pos; } set { lock (this) pos = value; } }
        public override void Flush() { }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        public override long Seek(long offset, SeekOrigin origin)
        {
            lock (this) {
                pos = origin == SeekOrigin.Begin ? offset : origin == SeekOrigin.Current ? pos + offset : length + offset;
                return pos;
            }
        }
        public override int Read(byte[] buffer, int offset, int count)
        {
            if (buffer == null || offset < 0 || count < 0 || offset > buffer.Length - count) throw new ArgumentOutOfRangeException();
            long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
            int n;
            lock (this) {
                if (closed || count == 0) return 0;
                fixed (byte* p = buffer) n = bundleRead(id, pos, p + offset, count);
                if (n < 0) throw new IOException("VaM scene load: reading the prepared asset bundle failed");
                pos += n;
            }
            Interlocked.Increment(ref bReads);
            Interlocked.Add(ref bReadTicks, System.Diagnostics.Stopwatch.GetTimestamp() - t0);
            return n;
        }
        protected override void Dispose(bool disposing)
        {
            lock (this) {
                if (!closed) { closed = true; bundleFree(id); }
            }
            base.Dispose(disposing);
        }
    }

    void SetUpBundleLoads(Harmony h)
    {
        bundleMemory = Config.Bind("BundleLoad", "InMemory", true, "Prepare asset bundles of .var packages in memory in the background (LZMA bundles stored uncompressed, the same archive) and let Unity read them through a stream, instead of VaM inflating each into a managed byte[] and Unity decompressing it on its one loading thread, one bundle after another; nothing is written to disk, the memory goes when the bundle is unloaded");
        sameFrame = Config.Bind("BundleLoad", "SameFrame", true, "Hand over the next queued asset bundle in the same frame (same order) while the frame has spent less than FrameBudgetMs on them, instead of one bundle per frame");
        frameBudgetMs = Config.Bind("BundleLoad", "FrameBudgetMs", 100, "With SameFrame: milliseconds of bundle hand-overs (CUA prefabs instantiating) per frame before the rest waits for the next frame");
        parallelDecode = Config.Bind("Audio", "ParallelDecode", true, "With InMemory: AudioClips that are DecompressOnLoad + Vorbis reach Unity as PCM16 decoded on CPU Boost's workers when the first of a bundle's clips is read, instead of FMOD decoding each on Unity's main thread when it loads (libvorbis with FMOD's rounding: about one value in 5000 differs by 1)");
        decodeAheadMB = Config.Bind("Audio", "DecodeAheadMB", 4096, "With ParallelDecode: decoded clips Unity has not read yet (all bundles) at which the background decoding waits; a clip read before it was decoded is decoded by the reading thread");
        BindingFlags inst = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public, any = inst | BindingFlags.Static;
        try {
            Type al = typeof(Loader);
            fSingleton = Need(al.GetField("singleton", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public), "AssetLoader.singleton");
            fCounts = Need(al.GetField("assetBundleReferenceCounts", inst), "AssetLoader.assetBundleReferenceCounts");
            fBundles = Need(al.GetField("pathToAssetBundle", inst), "AssetLoader.pathToAssetBundle");
            fQueue = Need(al.GetField("assetBundleFromFileQueue", inst), "AssetLoader.assetBundleFromFileQueue");
            if (fCounts.FieldType != typeof(Dictionary<string, int>) || fBundles.FieldType != typeof(Dictionary<string, AssetBundle>) || fQueue.FieldType != typeof(List<Request>))
                throw new MissingFieldException("AssetLoader fields of other types");
            MethodInfo queue = al.GetMethod("QueueLoadAssetBundleFromFile", any, null, new[] { typeof(Request) }, null);
            MethodInfo load = al.GetMethod("LoadBundleFileAsync", any, null, new[] { typeof(Request) }, null);
            if (queue == null || load == null || load.ReturnType != typeof(IEnumerator)) throw new MissingMethodException("AssetLoader.QueueLoadAssetBundleFromFile / LoadBundleFileAsync");
            if (zFirstOffset == null) zFirstOffset = FieldGetter<long>("offsetOfFirstEntry");
            string bin = Path.Combine(Path.GetDirectoryName(Info.Location), "VaMSceneLoadNative.bin");
            IntPtr m = LoadLibraryW(bin);
            if (m == IntPtr.Zero) throw new FileNotFoundException("cannot load " + bin);
            IntPtr api = GetProcAddress(m, "SL_BundleApi");
            if (api == IntPtr.Zero || ((ApiFn)Marshal.GetDelegateForFunctionPointer(api, typeof(ApiFn)))() != 2) throw new EntryPointNotFoundException("SL_BundleApi 2");
            bundleSubmit = (BundleSubmitFn)Native(m, "SL_BundleSubmit2", typeof(BundleSubmitFn));
            bundleResult = (BundleResultFn)Native(m, "SL_BundleResult2", typeof(BundleResultFn));
            bundleRead = (BundleReadFn)Native(m, "SL_BundleRead", typeof(BundleReadFn));
            bundleFree = (BundleFreeFn)Native(m, "SL_BundleFree", typeof(BundleFreeFn));
            audioConfig = (AudioConfigFn)Native(m, "SL_AudioConfig", typeof(AudioConfigFn));
            audioStats = (AudioStatsFn)Native(m, "SL_AudioStats", typeof(AudioStatsFn));
            bundleOnCb = GetProcAddress(m, "SL_BundleOnCpuBoost");
            if (bundleOnCb == IntPtr.Zero) throw new EntryPointNotFoundException("SL_BundleOnCpuBoost");
            audioConfig(parallelDecode.Value ? 1 : 0, (long)Math.Max(256, decodeAheadMB.Value) << 20);
            h.Patch(queue, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("QueuePostfix", any)), null, null);
            h.Patch(load, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("LoadPrefix", any)), null, null, null);
            bundlesReady = true;
            if (sameFrame.Value) {
                Type it = al.GetNestedType("<AssetBundleFromFileQueueProcessor>c__Iterator1", BindingFlags.NonPublic | BindingFlags.Public);
                if (it == null) throw new MissingMemberException("AssetLoader's queue coroutine");
                fProcThis = Need(it.GetField("$this", inst), "$this");
                fProcCurrent = Need(it.GetField("$current", inst), "$current");
                fProcPC = Need(it.GetField("$PC", inst), "$PC");
                MethodInfo mn = it.GetMethod("MoveNext", inst, null, Type.EmptyTypes, null);
                if (mn == null || fProcPC.FieldType != typeof(int)) throw new MissingMethodException("AssetLoader's queue coroutine MoveNext");
                h.Patch(mn, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ProcessorPrefix", any)), null, null, null);
                pacingReady = true;
            }
        } catch (Exception e) {
            Logger.LogError("VaM scene load: asset bundle loads " + (bundlesReady ? "same-frame hand-over" : "") + " off: " + e.Message);
        }
        Logger.LogInfo("VaM scene load: asset bundles in memory " + (bundlesReady && bundleMemory.Value ? "on" : "off") + ", parallel audio decode "
            + (bundlesReady && bundleMemory.Value && parallelDecode.Value ? "on (ahead " + decodeAheadMB.Value + " MB)" : "off") + ", same-frame hand-over " + (pacingReady ? "on" : "off"));
    }

    static Delegate Native(IntPtr m, string name, Type t)
    {
        IntPtr p = GetProcAddress(m, name);
        if (p == IntPtr.Zero) throw new EntryPointNotFoundException(name);
        return Marshal.GetDelegateForFunctionPointer(p, t);
    }

    static bool refused;
    static string loadingPath;   // the bundle LoadBundle is loading (requests run one at a time)
    static bool MemoryOn() { return bundlesReady && !bundlesBroken && !refused && bundleMemory.Value && !testBundlesOff; }

    static void Broken(Exception e)
    {
        if (bundlesBroken) return;
        bundlesBroken = true;
        log.LogError("VaM scene load: asset bundles in memory off for this run (VaM's own loads from here on): " + e);
    }

    // a request queued: prepare its bundle when it is not loaded and nothing is being prepared for it
    public static void QueuePostfix(Request abffr)
    {
        if (!MemoryOn() || abffr == null || abffr.path == null) return;
        try {
            Loader al = (Loader)fSingleton.GetValue(null);
            Dictionary<string, AssetBundle> loaded = al != null ? (Dictionary<string, AssetBundle>)fBundles.GetValue(al) : null;
            if (loaded != null && loaded.ContainsKey(abffr.path)) return;   // reused when its turn comes
            Prep p = Prepare(abffr.path);
            if (p != null) { prepByRequest[abffr] = p; p.users++; }
        } catch (Exception e) { Broken(e); }
    }

    // The bundle of a path in a (packed) .var package being prepared or ready, or a new job; null for anything VaM
    // does not read through LoadFromMemoryAsync.
    static Prep Prepare(string path)
    {
        Prep p;
        if (prepByPath.TryGetValue(path, out p) && (p.state == 0 || p.state == 1)) return p;
        if (!FileManager.IsFileInPackage(path)) return null;
        VarFileEntry vfe = FileManager.GetVarFileEntry(path);
        if (vfe == null || vfe.Simulated || vfe.Package == null) return null;
        ZipFile zf = vfe.Package.ZipFile;
        if (zf == null) return null;
        ZipEntry ze = zf.GetEntry(vfe.InternalSlashPath);
        if (ze == null || ze.IsCrypted || !ze.IsFile) return null;
        int method = (int)ze.CompressionMethod;
        long size = ze.Size, csize = ze.CompressedSize;
        if ((method != 0 && method != 8) || size <= 0 || size > int.MaxValue || csize < 0 || size != vfe.Size) return null;
        string full;
        lock (fullPaths) {
            if (!fullPaths.TryGetValue(vfe.Package.Path, out full)) fullPaths[vfe.Package.Path] = full = Path.GetFullPath(vfe.Package.Path);
        }
        if (!File.Exists(full)) return null;
        long header = zFirstOffset(zf) + ze.Offset;
        p = new Prep();
        p.path = path;
        p.job = bundleSubmit(full, header, csize, method, size, parallelDecode.Value ? 1 : 0);
        if (p.job < 1) return null;
        pending.Add(p);
        bSubmitted++;
        bLogged = false;
        prepByPath[path] = p;
        return p;
    }

    // a request no longer needs its prepared bundle: dropped when no other request does and it was not opened
    static void Release(Prep p)
    {
        if (p == null || --p.users > 0 || p.state == 2 || p.state == 3) return;
        if (p.state == 0) pending.Remove(p);
        bundleFree(p.job);
        p.state = 3;
        bDropped++;
        Prep q;
        if (prepByPath.TryGetValue(p.path, out q) && q == p) prepByPath.Remove(p.path);
    }

    // finished jobs
    static void Poll()
    {
        for (int i = pending.Count - 1; i >= 0; i--) {
            Prep p = pending[i];
            long length; double ms; int clips;
            int r = bundleResult(p.job, out length, out ms, out clips);
            if (r == 0) continue;
            pending.RemoveAt(i);
            bJobMs += ms;
            if (r > 0) {
                p.state = 1; p.length = length; p.clips = clips; bBytes += length;
                if (clips > 0) { bAudio++; bPcmClips += clips; } else if (r == 1) bLzma++; else bAsIs++;
            } else {
                p.state = -1;
                if (r == -40) bNotOurs++; else { bFailed++; bLastFail = r; }
                Prep q;
                if (prepByPath.TryGetValue(p.path, out q) && q == p) prepByPath.Remove(p.path);
            }
        }
        if (pending.Count == 0 && !bLogged) {
            bLogged = true;
            log.LogInfo("VaM scene load: asset bundles prepared in memory " + (bLzma + bAudio + bAsIs) + " (" + bLzma + " LZMA stored uncompressed, " + bAudio + " with " + bPcmClips + " clips made PCM16, "
                + (bBytes / 1e9).ToString("F2") + " GB, " + (bJobMs / 1000).ToString("F1") + " s of jobs, on CPU Boost " + (((ApiFn)Marshal.GetDelegateForFunctionPointer(bundleOnCb, typeof(ApiFn)))() == 1 ? "yes" : "no") + "), failed " + bFailed
                + (bFailed > 0 ? " (last code " + bLastFail + ")" : "") + ", not UnityFS 6 " + bNotOurs);
        }
    }

    // the memory of unloaded bundles, and prepared bundles no queued request needs any more (a queue VaM cleared)
    static void Reap()
    {
        for (int i = live.Count - 1; i >= 0; i--) {
            if (live[i].ab != null) continue;   // destroyed (AssetBundle.Unload) compares equal to null
            live[i].stream.Dispose();
            live.RemoveAt(i);
            bUnloaded++;
        }
        if (prepByRequest.Count == 0) return;
        Loader al = (Loader)fSingleton.GetValue(null);
        List<Request> q = al != null ? (List<Request>)fQueue.GetValue(al) : null;
        HashSet<Request> queued = new HashSet<Request>();
        if (q != null) foreach (Request r in q) queued.Add(r);
        List<Request> gone = null;
        foreach (KeyValuePair<Request, Prep> kv in prepByRequest)
            if (!queued.Contains(kv.Key) && kv.Value.path != loadingPath) (gone ?? (gone = new List<Request>())).Add(kv.Key);
        if (gone == null) return;
        foreach (Request r in gone) { Prep p = prepByRequest[r]; prepByRequest.Remove(r); Release(p); }
    }

    // from Update (LoadFrames.cs), once a second while something is loaded or prepared
    static void BundleUpdate()
    {
        if (!bundlesReady || Time.unscaledTime < nextReap || (live.Count == 0 && prepByRequest.Count == 0)) return;
        nextReap = Time.unscaledTime + 1f;
        try { Reap(); } catch (Exception e) { Broken(e); }
    }

    static Opening Open(Prep p)
    {
        Opening o = new Opening();
        o.stream = new BundleStream(p.job, p.length);
        p.state = 2;
        Prep q;
        if (prepByPath.TryGetValue(p.path, out q) && q == p) prepByPath.Remove(p.path);
        o.req = AssetBundle.LoadFromStreamAsync(o.stream, 0, 1 << 20);
        return o;
    }

    // bundles of upcoming requests that are prepared: opened ahead, in queue order
    static void Advance(Loader al)
    {
        if (!MemoryOn()) return;
        try {
            Poll();
            List<Request> q = (List<Request>)fQueue.GetValue(al);
            Dictionary<string, AssetBundle> loaded = (Dictionary<string, AssetBundle>)fBundles.GetValue(al);
            if (q == null) return;
            int n = Math.Min(q.Count, 32);
            for (int i = 0; i < n; i++) {
                Prep p;
                string path = q[i].path;
                // never the bundle being loaded right now (1.3.1: a request waiting for its bundle while a later request
                // of the same bundle got it opened ahead opened it a second time; Unity refuses a bundle loaded twice)
                if (path == loadingPath) continue;
                if (!prepByRequest.TryGetValue(q[i], out p) || p.state != 1 || ahead.ContainsKey(path)) continue;
                if (loaded != null && loaded.ContainsKey(path)) continue;
                ahead[path] = Open(p);
                bEarly++;
            }
        } catch (Exception e) { Broken(e); }
    }

    public static bool LoadPrefix(Loader __instance, Request abffr, ref IEnumerator __result)
    {
        if (!bundlesReady) return true;
        __result = LoadBundle(__instance, abffr, false);
        return false;
    }

    // AssetLoader.LoadBundleFileAsync with the bundle opened from memory when it was prepared; everything else is VaM's
    // own code, in the same order (reference count, the loaded-bundle table, VaM's error, the callback).
    static IEnumerator LoadBundle(Loader al, Request r, bool handOver)
    {
        Dictionary<string, int> counts = (Dictionary<string, int>)fCounts.GetValue(al);
        if (counts == null) { counts = new Dictionary<string, int>(); fCounts.SetValue(al, counts); }
        Dictionary<string, AssetBundle> bundles = (Dictionary<string, AssetBundle>)fBundles.GetValue(al);
        if (bundles == null) { bundles = new Dictionary<string, AssetBundle>(); fBundles.SetValue(al, bundles); }
        string path = r.path;
        int cnt;
        if (counts.TryGetValue(path, out cnt)) { cnt++; counts.Remove(path); counts.Add(path, cnt); }
        else counts.Add(path, 1);
        AssetBundle ab = null;
        Prep p = null;
        if (prepByRequest.TryGetValue(r, out p)) prepByRequest.Remove(r);
        if (!bundles.TryGetValue(path, out ab)) {
            loadingPath = path;
            AssetBundleCreateRequest abcr = null;
            bool memFailed = false;
            // a bundle opened ahead for this path is always taken by the first request that loads the path (also when
            // this was switched off since): nobody else would ever unload it
            Opening o;
            if (ahead.TryGetValue(path, out o)) ahead.Remove(path);
            {
                if (o == null && p != null && p.state != 2 && MemoryOn()) {
                    double w0 = Now();
                    for (;;) {
                        try { Poll(); } catch (Exception e) { Broken(e); }
                        if (p.state != 0 || bundlesBroken) break;
                        bWaitFrames++;
                        yield return null;
                    }
                    bWaitMs += Now() - w0;
                    if (ahead.TryGetValue(path, out o)) ahead.Remove(path);   // opened ahead meanwhile: that one
                    else if (p.state == 1 && !bundlesBroken) o = Open(p);
                }
                if (o != null) {
                    if (!o.req.isDone) {
                        if (handOver) bForced++;          // assetBundle below completes it now
                        else yield return o.req;
                    }
                    if (o.req.assetBundle != null) {
                        abcr = o.req; bFromMemory++;
                        Live l = new Live(); l.ab = o.req.assetBundle; l.stream = o.stream; l.length = o.stream.Length;
                        live.Add(l);
                    } else {
                        o.stream.Dispose();
                        memFailed = true;                 // VaM's own load decides
                    }
                }
            }
            Release(p);
            if (abcr == null) {
                bVaM++;
                OwnLoad own = new OwnLoad();
                IEnumerator e = VaMOwnLoad(path, own);
                while (e.MoveNext()) yield return e.Current;
                abcr = own.abcr;
                if (memFailed && abcr != null && abcr.assetBundle != null) {
                    // Unity refused the prepared bundle and accepted VaM's bytes: a format problem, not a one-off;
                    // after the second this stays off for this run (VaM's own load for every bundle, as without the plugin)
                    bRefused++;
                    if (bRefused == 1) log.LogWarning("VaM scene load: Unity refused the prepared asset bundle for " + path + "; VaM's own load used");
                    if (bRefused >= 2 && !refused) {
                        refused = true;
                        log.LogWarning("VaM scene load: Unity refused " + bRefused + " prepared asset bundles; asset bundles in memory off for this run");
                    }
                }
            }
            // 1.6.0: refused while unused bundles are kept: one of them may have the same files (Unity refuses a second)
            if ((abcr == null || !abcr.assetBundle) && UnloadAllKept(path)) {
                OwnLoad own = new OwnLoad();
                IEnumerator e = VaMOwnLoad(path, own);
                while (e.MoveNext()) yield return e.Current;
                abcr = own.abcr;
            }
            if (abcr != null) {
                if (!abcr.assetBundle) SuperController.LogError("Error during attempt to load assetbundle " + path + ". Not valid");
                else { ab = abcr.assetBundle; bundles.Add(path, abcr.assetBundle); }
            }
            loadingPath = null;
        } else Release(p);
        try { NoteRequest(path, r.callback); } catch (Exception e) { log.LogError("VaM scene load: noting the requester of " + path + ": " + e.Message); }
        r.assetBundle = ab;
        if (r.callback != null) {
            // 1.6.0: a callback that throws (a plugin given a bundle Unity refused) stopped our queue coroutine for the run
            if (!keepClearing.Value) r.callback(r);
            else {
                try { r.callback(r); }
                catch (Exception e) { callbackErrors++; SuperController.LogError("Exception during asset bundle callback for " + path + ": " + e); }
            }
        }
    }

    sealed class OwnLoad { public AssetBundleCreateRequest abcr; }

    // VaM's own steps of LoadBundleFileAsync for a bundle not loaded yet
    static IEnumerator VaMOwnLoad(string path, OwnLoad own)
    {
        AssetBundleCreateRequest abcr;
        if (FileManager.IsFileInPackage(path)) {
            VarFileEntry vfe = FileManager.GetVarFileEntry(path);
            if (vfe.Simulated) abcr = AssetBundle.LoadFromFileAsync(vfe.Package.Path + "\\" + vfe.InternalPath);
            else {
                byte[] bytes = new byte[vfe.Size];
                yield return FileManager.ReadAllBytesCoroutine(vfe, bytes);
                abcr = AssetBundle.LoadFromMemoryAsync(bytes);
            }
        } else abcr = AssetBundle.LoadFromFileAsync(path);
        if (abcr != null) yield return abcr;
        own.abcr = abcr;
    }

    // VaM's queue coroutine, run by ours from its next top-of-loop on: the same steps (create the queue, a frame, take
    // the first request, load it), then the next request in this same frame while the frame's budget lasts.
    public static bool ProcessorPrefix(object __instance, ref bool __result)
    {
        if (!pacingReady) return true;
        IEnumerator e;
        if (!procs.TryGetValue(__instance, out e)) {
            int pc = (int)fProcPC.GetValue(__instance);
            if (pc != 0 && pc != 1) return true;   // VaM's own coroutine is in a load: it finishes that one itself
            e = Processor((Loader)fProcThis.GetValue(__instance));
            procs[__instance] = e;
        }
        bool more;
        try { more = e.MoveNext(); }
        catch (Exception ex) {
            // 1.6.0: an exception out of a request would end VaM's coroutine for the run (every later request queued for
            // good): logged, and the queue starts over at the next frame
            procs.Remove(__instance);
            loadingPath = null;
            processorRestarts++;
            SuperController.LogError("Exception in the asset bundle queue: " + ex);
            fProcCurrent.SetValue(__instance, null);
            __result = true;
            return false;
        }
        fProcCurrent.SetValue(__instance, e.Current);
        __result = more;
        return false;
    }

    static IEnumerator Processor(Loader al)
    {
        for (;;) {
            if (fQueue.GetValue(al) == null) fQueue.SetValue(al, new List<Request>());
            yield return null;
            double t0 = Now();
            int inFrame = 0;
            Advance(al);
            for (;;) {
                List<Request> q = (List<Request>)fQueue.GetValue(al);
                if (q.Count <= 0) break;
                Request r = q[0];
                q.RemoveAt(0);
                bool together = sameFrame.Value && !testSameFrameOff && !bundlesBroken;
                IEnumerator load = LoadBundle(al, r, together);
                while (load.MoveNext()) {
                    yield return load.Current;
                    t0 = Now();
                    inFrame = 0;
                    Advance(al);
                }
                if (++inFrame > 1) bSameFrame++;
                if (!together || Now() - t0 >= frameBudgetMs.Value) break;
                Advance(al);
            }
        }
    }

    public static string SetBundleTestOff(bool memoryOff, bool sameFrameOff) { testBundlesOff = memoryOff; testSameFrameOff = sameFrameOff; return BundleStats(); }
    public static string SetAudioTestOff(bool off) { audioConfig(off || !parallelDecode.Value ? 0 : 1, (long)Math.Max(256, decodeAheadMB.Value) << 20); return AudioStats(); }

    public static string AudioStats()
    {
        if (audioStats == null) return "audio: -";
        double[] v = new double[18];
        fixed (double* p = v) audioStats(p, 18);
        return "audio: clips made PCM16 " + v[0] + " (bundles " + v[11] + ", further resource nodes " + v[14] + "), kept Vorbis " + v[1] + "; decoded in the background " + v[2] + ", by the reader " + v[3]
            + ", again after being read " + v[4] + ", background waits at the cap " + v[5] + "; buffers freed 2 s after their last byte " + v[6] + ", decode errors " + v[7]
            + ", decoded and dropped unread (bundle idle) " + v[15]
            + "; decode " + (v[8] / 1000).ToString("F2") + " s CPU for " + v[10] + " frames, reader waited " + (v[9] / 1000).ToString("F2") + " s; decoded in memory now "
            + (v[12] / 1048576).ToString("F0") + " MB, peak " + (v[13] / 1048576).ToString("F0") + " MB; bundles hold " + (v[16] / 1073741824).ToString("F2") + " GB, peak "
            + (v[17] / 1073741824).ToString("F2") + " GB";
    }

    public static string BundleStats()
    {
        long liveBytes = 0;
        foreach (Live l in live) liveBytes += l.length;
        return "bundles in memory: jobs " + bSubmitted + " (" + bLzma + " LZMA stored, " + bAudio + " with " + bPcmClips + " PCM16 clips, " + bAsIs + " as they are, " + bFailed + " failed" + (bFailed > 0 ? ", last " + bLastFail : "")
            + ", not UnityFS 6 " + bNotOurs + ", " + pending.Count + " running; " + (bBytes / 1e9).ToString("F2") + " GB, " + (bJobMs / 1000).ToString("F1") + " s of jobs)"
            + "; opened from memory " + bFromMemory + " (ahead " + bEarly + ", completed at once " + bForced + "), VaM's own " + bVaM + ", refused " + bRefused + ", dropped unused " + bDropped
            + "; loaded now " + live.Count + " (" + (liveBytes / 1e9).ToString("F2") + " GB), unloaded " + bUnloaded + "; stream reads " + bReads + " ("
            + (bReadTicks * 1.0 / System.Diagnostics.Stopwatch.Frequency).ToString("F2") + " s)"
            + "; waited for jobs " + bWaitFrames + " frames (" + (bWaitMs / 1000).ToString("F1") + " s); handed over in an already used frame " + bSameFrame
            + (bundlesBroken ? " [off after an error]" : "") + (refused ? " [off: Unity refused them]" : "") + " | " + AudioStats();
    }
}
