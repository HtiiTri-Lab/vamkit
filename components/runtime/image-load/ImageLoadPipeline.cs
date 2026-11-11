using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;

[BepInPlugin("local.vam.imageloadpipeline", "Image Load Pipeline", "0.1.0")]
public sealed partial class ImageLoadPipeline : BaseUnityPlugin
{
    static ConfigEntry<bool> enabledPump;
    static BepInEx.Logging.ManualLogSource logger;
    static FieldInfo queueField;
    static Action<ImageLoaderThreaded> postProcess, preProcess;
    [ThreadStatic] static bool pumping;
    static long fastHits, pumpCalls, pumpTicks, budgetStops, pending, maxDepth;
    const int MaxPerFrame = 32;
    const double BudgetMs = 5.0;

    void Start()
    {
        logger = Logger;
        enabledPump = Config.Bind("ImageLoad", "SameFrameMemoryHits", false, "Process consecutive completed memory-cache hits on the main thread in the same frame");
        try {
            BindingFlags f = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
            Type t = typeof(ImageLoaderThreaded);
            queueField = t.GetField("queuedImages", f);
            MethodInfo pre = t.GetMethod("PreprocessImageQueue", f), post = t.GetMethod("PostProcessImageQueue", f);
            if (queueField == null || pre == null || post == null || queueField.FieldType != typeof(LinkedList<ImageLoaderThreaded.QueuedImage>)) throw new MissingMemberException("VaM image queue");
            preProcess = (Action<ImageLoaderThreaded>)Delegate.CreateDelegate(typeof(Action<ImageLoaderThreaded>), pre);
            postProcess = (Action<ImageLoaderThreaded>)Delegate.CreateDelegate(typeof(Action<ImageLoaderThreaded>), post);
            if (enabledPump.Value) new Harmony("local.vam.imageloadpipeline").Patch(pre, null, new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("AfterPreprocess")), null, null);
            logger.LogInfo("Image load pipeline: same-frame hits " + (enabledPump.Value ? "on" : "off"));
        } catch (Exception e) { enabledPump.Value = false; logger.LogError("Image load pipeline: same-frame hits off: " + e); }
        BindOffMain();
        SetUpAsync();
        SetUpPixels();
        SetUpCompression();
        SetUpStageProfile();
        SetUpCrossPipeline();
    }

    public static void AfterPreprocess(ImageLoaderThreaded __instance)
    {
        if (pumping || !enabledPump.Value) return;
        var queue = (LinkedList<ImageLoaderThreaded.QueuedImage>)queueField.GetValue(__instance);
        if (queue == null) return;
        if (queue.Count > maxDepth) maxDepth = queue.Count;
        if (queue.First == null || queue.First.Value == null) { pending++; return; }
        if (!queue.First.Value.processed) TryTakeReadyOnMain(queue.First.Value);
        if (!queue.First.Value.processed) { pending++; return; }
        long start = Stopwatch.GetTimestamp(); int done = 0; pumping = true;
        try {
            while (queue.First != null && queue.First.Value != null && queue.First.Value.processed) {
                postProcess(__instance);
                done++;
                // Preserve VaM's own preprocessing, including canceled nodes and callbacks that enqueued work.
                if (queue.Count == 0) break;
                preProcess(__instance); // original Update will wake a worker after this hook; it needs a preprocessed head
                if (done >= MaxPerFrame || (Stopwatch.GetTimestamp()-start)*1000.0/Stopwatch.Frequency >= BudgetMs) break;
                if (queue.First != null && queue.First.Value != null && !queue.First.Value.processed) TryTakeReadyOnMain(queue.First.Value);
            }
        } finally {
            pumping = false; pumpCalls++; fastHits += done; pumpTicks += Stopwatch.GetTimestamp() - start;
            if (done >= MaxPerFrame || (Stopwatch.GetTimestamp()-start)*1000.0/Stopwatch.Frequency >= BudgetMs) budgetStops++;
        }
    }

    public static string Stats() {
        return "sameFrame=" + fastHits + " pumpCalls=" + pumpCalls + " pumpMs=" + (pumpTicks*1000.0/Stopwatch.Frequency).ToString("F1") + " budgetStops=" + budgetStops + " otherHeads=" + pending + " maxQueue=" + maxDepth + AsyncStats() + PixelStats() + CompressionStats();
    }

    public static string Snapshot(string path) {
        var lines = new List<string>(); int errors = 0;
        try {
            var loader = ImageLoaderThreaded.singleton;
            var fi = typeof(ImageLoaderThreaded).GetField("textureCache", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            var cache = fi.GetValue(loader) as Dictionary<string, Texture2D>;
            using (var sha = SHA256.Create()) foreach (var kv in cache) {
                try {
                    var tex = kv.Value; if (tex == null) { lines.Add(kv.Key + "\tNULL"); continue; }
                    byte[] hash = sha.ComputeHash(tex.GetRawTextureData());
                    lines.Add(kv.Key + "\t" + tex.width + "x" + tex.height + "\t" + tex.format + "\t" + tex.mipmapCount + "\t" + BitConverter.ToString(hash).Replace("-", ""));
                } catch (Exception e) { errors++; lines.Add(kv.Key + "\tERROR:" + e.GetType().Name); }
            }
            lines.Sort(StringComparer.Ordinal); File.WriteAllLines(path, lines.ToArray());
            return "textures=" + lines.Count + " errors=" + errors;
        } catch (Exception e) { return "snapshot failed: " + e; }
    }
}
