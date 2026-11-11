// Test-only probe: what Unity itself decoded for every DecompressOnLoad AudioClip in memory, so a parallel
// decoder can be checked against it sample for sample. Goes into BepInEx\plugins only for a test run
// (the test script puts it there and takes it out).
// Count(): loaded / total DecompressOnLoad clips and their samples (to wait until a scene's plugins stopped loading).
// Truth(path, dumpDir, dumpPerKey): one line per AudioClip (name, load type, state, samples, channels, frequency, length,
//   flags); for loaded DecompressOnLoad clips the data read back with GetData: whether every value is k / 32768 with k an
//   int16 (exact), the FNV-1a 64 hash of the int16 values (interleaved, little-endian) or of the float bits when not
//   exact, the min / max, the first and last 8 values; the dumpPerKey shortest clips per (channels, frequency) written
//   to dumpDir as raw int16 (or float32 when not exact).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using BepInEx;
using UnityEngine;

[BepInPlugin("local.vam.test.audiotruthprobe", "Audio truth probe (test only)", "1.0.0")]
public class AudioTruthProbe : BaseUnityPlugin
{
    public static string Count()
    {
        AudioClip[] all = Resources.FindObjectsOfTypeAll<AudioClip>();
        int dol = 0, loaded = 0; long samples = 0;
        foreach (AudioClip c in all) {
            if (c == null || c.loadType != AudioClipLoadType.DecompressOnLoad) continue;
            dol++;
            if (c.loadState == AudioDataLoadState.Loaded) { loaded++; samples += (long)c.samples * c.channels; }
        }
        return "clips " + all.Length + " dol " + dol + " loaded " + loaded + " samples " + samples;
    }

    public static unsafe string Truth(string path, string dumpDir, int dumpPerKey)
    {
        AudioClip[] all = Resources.FindObjectsOfTypeAll<AudioClip>();
        var lines = new List<string>();
        var byKey = new Dictionary<string, List<AudioClip>>();
        foreach (AudioClip c in all) {
            if (c == null || c.loadType != AudioClipLoadType.DecompressOnLoad || c.loadState != AudioDataLoadState.Loaded) continue;
            string k = c.channels + "x" + c.frequency;
            List<AudioClip> l;
            if (!byKey.TryGetValue(k, out l)) byKey[k] = l = new List<AudioClip>();
            l.Add(c);
        }
        var dump = new HashSet<AudioClip>();
        foreach (var kv in byKey) {
            kv.Value.Sort((a, b) => a.samples.CompareTo(b.samples));
            for (int i = 0; i < kv.Value.Count && i < dumpPerKey; i++) dump.Add(kv.Value[i]);
        }
        if (dump.Count > 0) Directory.CreateDirectory(dumpDir);
        int n = 0, exactClips = 0, inexactClips = 0; long values = 0;
        var t0 = System.Diagnostics.Stopwatch.StartNew();
        float[] buf = new float[1 << 20];
        byte[] raw = new byte[(1 << 20) * 4];
        foreach (AudioClip c in all) {
            if (c == null) continue;
            var sb = new StringBuilder();
            sb.Append(c.name).Append('\t').Append(c.loadType).Append('\t').Append(c.loadState).Append('\t').Append(c.samples).Append('\t')
              .Append(c.channels).Append('\t').Append(c.frequency).Append('\t').Append(c.length.ToString("R")).Append('\t')
              .Append(c.preloadAudioData ? "pre" : "nopre").Append('\t').Append(c.loadInBackground ? "bg" : "main").Append('\t').Append(c.GetInstanceID());
            if (c.loadType == AudioClipLoadType.DecompressOnLoad && c.loadState == AudioDataLoadState.Loaded && c.channels > 0) {
                int ch = c.channels;
                long total = (long)c.samples * ch;
                int framesPer = buf.Length / ch;
                ulong h = 14695981039346656037UL, hf = 14695981039346656037UL;
                long inexact = 0; float mn = float.MaxValue, mx = float.MinValue;
                var head = new StringBuilder(); var tail = new float[8]; int tailN = 0;
                FileStream fs = null;
                if (dump.Contains(c)) fs = File.Create(Path.Combine(dumpDir, n.ToString("D4") + "-" + Safe(c.name) + "-" + ch + "x" + c.frequency + ".pcm"));
                bool dumpFloat = false;
                var firstChunk = new List<float[]>();
                for (int f0 = 0; f0 < c.samples; f0 += framesPer) {
                    int frames = Math.Min(framesPer, c.samples - f0);
                    float[] b = frames * ch == buf.Length ? buf : new float[frames * ch];
                    c.GetData(b, f0);
                    int m = frames * ch;
                    fixed (float* p = b) {
                        for (int i = 0; i < m; i++) {
                            float v = p[i];
                            if (v < mn) mn = v;
                            if (v > mx) mx = v;
                            float s = v * 32768f;
                            int k = (int)s;
                            if (k != s || k < -32768 || k > 32767) inexact++;
                            ushort u = (ushort)(short)k;
                            h = (h ^ (byte)u) * 1099511628211UL;
                            h = (h ^ (byte)(u >> 8)) * 1099511628211UL;
                            uint bits = *(uint*)(p + i);
                            hf = (hf ^ (bits & 255)) * 1099511628211UL;
                            hf = (hf ^ ((bits >> 8) & 255)) * 1099511628211UL;
                            hf = (hf ^ ((bits >> 16) & 255)) * 1099511628211UL;
                            hf = (hf ^ (bits >> 24)) * 1099511628211UL;
                            if (f0 == 0 && i < 8) head.Append(i == 0 ? "" : ",").Append(k);
                            tail[tailN++ & 7] = s;
                        }
                    }
                    if (fs != null) {
                        if (f0 == 0 && inexact > 0) dumpFloat = true;
                        if (dumpFloat) { Buffer.BlockCopy(b, 0, raw, 0, m * 4); fs.Write(raw, 0, m * 4); }
                        else {
                            fixed (float* p = b) fixed (byte* r = raw) {
                                short* q = (short*)r;
                                for (int i = 0; i < m; i++) q[i] = (short)(int)(p[i] * 32768f);
                            }
                            fs.Write(raw, 0, m * 2);
                        }
                    }
                }
                if (fs != null) { fs.Close(); if (dumpFloat) File.Move(fs.Name, fs.Name + ".f32"); }
                var tl = new StringBuilder();
                for (int i = 0; i < 8 && i < tailN; i++) tl.Append(i == 0 ? "" : ",").Append((int)tail[(tailN - Math.Min(8, tailN) + i) & 7]);
                sb.Append('\t').Append(inexact == 0 ? "exact" : "inexact " + inexact).Append('\t').Append(h.ToString("x16")).Append('\t').Append(hf.ToString("x16"))
                  .Append('\t').Append(mn.ToString("R")).Append('\t').Append(mx.ToString("R")).Append('\t').Append(head).Append('\t').Append(tl)
                  .Append('\t').Append(fs != null ? "dumped" : "");
                if (inexact == 0) exactClips++; else inexactClips++;
                values += total;
            }
            lines.Add(sb.ToString());
            n++;
        }
        File.WriteAllText(path, string.Join("\n", lines.ToArray()) + "\n", new UTF8Encoding(false));
        return "clips " + n + " checked exact " + exactClips + " inexact " + inexactClips + " values " + values + " dumped " + dump.Count + " in " + t0.ElapsedMilliseconds + " ms";
    }

    static string Safe(string s)
    {
        var sb = new StringBuilder();
        foreach (char ch in s) sb.Append(ch < 128 && (char.IsLetterOrDigit(ch) || ch == '_' || ch == '-') ? ch : '_');
        return sb.Length > 60 ? sb.ToString(0, 60) : sb.ToString();
    }
}
