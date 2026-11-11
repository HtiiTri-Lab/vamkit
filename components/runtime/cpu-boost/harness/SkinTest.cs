// Bit-for-bit check of the native SkinMeshPart against VaM's own DAZSkinV2.SkinMeshPart, both running inside VaM's
// Mono (mono_host.exe). Synthetic skins cover all six rotation orders, an invalid order, fully weighted and
// near-1 weights, zero and negative weights and bulges, angles on the +-0.01 thresholds, duplicate entries, onlyVerts
// and onlyBones filters, ranges split like VaM's skin sub-threads, general weights, and several frames in a row.
//     mono_host.exe <VaM folder> SkinTest.exe <folder with VaMCpuBoostNative.dll> [seed] [cases]
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization;
using UnityEngine;

internal static class SkinTest
{
    private const BindingFlags F = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    private static uint seed;
    private static int fails;

    private static uint Next() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
    private static float U() { return (Next() >> 8) * (1f / 16777216f); }                // [0,1)
    private static float S(float scale) { return (U() * 2f - 1f) * scale; }
    private static int I(int n) { return (int)(Next() % (uint)n); }

    private static void Set(object o, string name, object v)
    {
        FieldInfo f = o.GetType().GetField(name, F);
        if (f == null) throw new MissingFieldException(o.GetType().Name, name);
        f.SetValue(o, v);
    }

    // A weight value with the edge cases SkinMeshPart branches on.
    private static float W()
    {
        switch (I(12))
        {
            case 0: return 0f;
            case 1: return 1f;
            case 2: return 0.99999f;          // not > 0.99999f
            case 3: return 0.999995f;         // > 0.99999f
            case 4: return -S(0.3f) * 0.5f - 0.01f;
            default: return U();
        }
    }
    private static float Angle()
    {
        switch (I(10))
        {
            case 0: return 0.01f;
            case 1: return -0.01f;
            case 2: return 0.0100001f;
            case 3: return -0.0100001f;
            case 4: return 0f;
            default: return S(1.8f);
        }
    }
    private static float Bulge() { int k = I(5); return k == 0 ? 0f : k == 1 ? -U() : U(); }

    // Any affine matrix will do for a bit-for-bit test (Quaternion.Euler / inverse are engine calls, absent here):
    // a rotation-sized 3x3 block, a translation, and the usual 0 0 0 1 bottom row.
    private static Matrix4x4 Mat()
    {
        var m = new Matrix4x4();
        m.m00 = 1f + S(0.3f); m.m01 = S(0.8f); m.m02 = S(0.8f); m.m03 = S(1f);
        m.m10 = S(0.8f); m.m11 = 1f + S(0.3f); m.m12 = S(0.8f); m.m13 = S(1f);
        m.m20 = S(0.8f); m.m21 = S(0.8f); m.m22 = 1f + S(0.3f); m.m23 = S(1f);
        m.m30 = 0f; m.m31 = 0f; m.m32 = 0f; m.m33 = 1f;
        return m;
    }

    private sealed class Case
    {
        public int numVerts, numBones; public bool general;
        public DAZSkinV2Node[] nodes;
    }

    private static Case MakeCase(int numVerts, int numBones, bool general)
    {
        var c = new Case { numVerts = numVerts, numBones = numBones, general = general };
        var wl = new System.Collections.Generic.List<DAZSkinV2VertexWeights>[numBones];
        var fl = new System.Collections.Generic.List<int>[numBones];
        var gl = new System.Collections.Generic.List<DAZSkinV2GeneralVertexWeights>[numBones];
        for (int b = 0; b < numBones; b++) { wl[b] = new System.Collections.Generic.List<DAZSkinV2VertexWeights>(); fl[b] = new System.Collections.Generic.List<int>(); gl[b] = new System.Collections.Generic.List<DAZSkinV2GeneralVertexWeights>(); }
        for (int v = 0; v < numVerts; v++)
        {
            int n = 1 + I(4);
            for (int k = 0; k < n; k++)
            {
                int b = I(numBones);
                if (I(5) == 0) fl[b].Add(v);
                else
                {
                    var w = new DAZSkinV2VertexWeights();
                    w.vertex = v; w.xweight = W(); w.yweight = W(); w.zweight = W();
                    w.xleftbulge = Bulge(); w.xrightbulge = Bulge(); w.yleftbulge = Bulge();
                    w.yrightbulge = Bulge(); w.zleftbulge = Bulge(); w.zrightbulge = Bulge();
                    wl[b].Add(w);
                    if (I(40) == 0) wl[b].Add(w);   // the same entry twice in one bone
                }
                var g = new DAZSkinV2GeneralVertexWeights(); g.vertex = v; g.weight = I(8) == 0 ? 0f : U();
                gl[b].Add(g);
            }
        }
        c.nodes = new DAZSkinV2Node[numBones];
        for (int b = 0; b < numBones; b++)
        {
            // Shuffle each bone's lists: the C# applies them in list order whatever it is.
            Shuffle(wl[b]); Shuffle(fl[b]); Shuffle(gl[b]);
            var node = new DAZSkinV2Node();
            node.rotationOrder = (Quaternion2Angles.RotationOrder)(b == 3 ? 6 : I(6));
            node.weights = wl[b].ToArray(); node.fullyWeightedVertices = fl[b].ToArray(); node.generalWeights = gl[b].ToArray();
            var f = new DAZSkinV2BulgeFactors();
            f.xposleft = Bulge(); f.xnegleft = Bulge(); f.xposright = Bulge(); f.xnegright = Bulge();
            f.yposleft = Bulge(); f.ynegleft = Bulge(); f.yposright = Bulge(); f.ynegright = Bulge();
            f.zposleft = Bulge(); f.znegleft = Bulge(); f.zposright = Bulge(); f.znegright = Bulge();
            node.bulgeFactors = f;
            c.nodes[b] = node;
        }
        return c;
    }

    private static void Shuffle<T>(System.Collections.Generic.List<T> l)
    {
        for (int i = l.Count - 1; i > 0; i--) { int j = I(i + 1); T t = l[i]; l[i] = l[j]; l[j] = t; }
    }

    private static int Compare(Vector3[] a, Vector3[] b, string what)
    {
        int bad = 0, first = -1;
        for (int i = 0; i < a.Length; i++)
        {
            if (BitConverter.ToInt32(BitConverter.GetBytes(a[i].x), 0) != BitConverter.ToInt32(BitConverter.GetBytes(b[i].x), 0) ||
                BitConverter.ToInt32(BitConverter.GetBytes(a[i].y), 0) != BitConverter.ToInt32(BitConverter.GetBytes(b[i].y), 0) ||
                BitConverter.ToInt32(BitConverter.GetBytes(a[i].z), 0) != BitConverter.ToInt32(BitConverter.GetBytes(b[i].z), 0))
            { if (first < 0) first = i; bad++; }
        }
        if (bad > 0)
        {
            Console.WriteLine("FAIL  " + what + ": " + bad + " of " + a.Length + " vertices differ, first " + first +
                " C# " + a[first].x.ToString("R") + "," + a[first].y.ToString("R") + "," + a[first].z.ToString("R") +
                " native " + b[first].x.ToString("R") + "," + b[first].y.ToString("R") + "," + b[first].z.ToString("R"));
            fails++;
        }
        return bad;
    }

    // One body-sized skin (21556 vertices as Genesis 2's base mesh, 100 bones, ~2.5 weights a vertex), the full range
    // as SkinMeshThreadedFast calls it without sub-threads: per-call time of the C# and of the native path.
    private static int Bench(MethodInfo part, int pool)
    {
        int numVerts = 21556, numBones = 100;
        Case c = MakeCase(numVerts, numBones, false);
        var skin = (DAZSkinV2)FormatterServices.GetUninitializedObject(typeof(DAZSkinV2));
        var change = new Matrix4x4[numBones]; var w2l = new Matrix4x4[numBones]; var l2w = new Matrix4x4[numBones];
        var angles = new Vector3[numBones];
        for (int b = 0; b < numBones; b++) { change[b] = Mat(); w2l[b] = Mat(); l2w[b] = Mat(); angles[b] = new Vector3(S(1.2f), S(1.2f), S(1.2f)); }
        var start = new Vector3[numVerts]; for (int v = 0; v < numVerts; v++) start[v] = new Vector3(S(1f), S(1f), S(1f));
        var vCs = (Vector3[])start.Clone(); var vNat = (Vector3[])start.Clone();
        Set(skin, "nodes", c.nodes); Set(skin, "_numBones", numBones); Set(skin, "_useGeneralWeights", false);
        Set(skin, "bulgeScale", 1f); Set(skin, "boneChangeFromOriginalMatrices", change); Set(skin, "boneWorldToLocalMatrices", w2l);
        Set(skin, "boneLocalToWorldMatrices", l2w); Set(skin, "boneRotationAngles", angles); Set(skin, "workingVerts", vCs);
        var isBase = new bool[numVerts]; for (int v = 0; v < numVerts; v++) isBase[v] = true;
        object[] a = { 0, numVerts - 1, isBase, null };
        CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, false, 1f, change, w2l, l2w, angles, vNat, null, 0, numVerts - 1, isBase, null);
        part.Invoke(skin, a);
        const int reps = 60;
        double f = 1000.0 / Stopwatch.Frequency;
        var sw = Stopwatch.StartNew();
        for (int r = 0; r < reps; r++) { Array.Copy(start, vCs, numVerts); part.Invoke(skin, a); }
        double cs = sw.ElapsedTicks * f / reps;
        double best = double.MaxValue, sum = 0;
        for (int r = 0; r < reps * 4; r++)
        {
            Array.Copy(start, vNat, numVerts);
            sw = Stopwatch.StartNew();
            CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, false, 1f, change, w2l, l2w, angles, vNat, null, 0, numVerts - 1, isBase, null);
            double t = sw.ElapsedTicks * f; sum += t; if (t < best) best = t;
        }
        int bad = Compare(vCs, vNat, "bench");
        Console.WriteLine("bench " + numVerts + " verts, " + numBones + " bones, native workers " + pool + ": C# " + cs.ToString("F3") +
            " ms/call, native avg " + (sum / (reps * 4)).ToString("F3") + " ms, best " + best.ToString("F3") + " ms" + (bad == 0 ? ", bits equal" : ""));
        return bad == 0 ? 0 : 1;
    }

    // Worker placement: the body-sized skin of Bench with the calling thread unpinned or pinned to one CCD and the
    // workers placed each way CpuBoostPlace offers. Per call: "frame" = after 2 ms asleep (as between frames), "b2b" = back to back,
    // "read" = the caller then reading the skinned vertices (as the post-skin normals do).
    private static int Ccd(MethodInfo part, int pool)
    {
        int numVerts = 21556, numBones = 100;
        Case c = MakeCase(numVerts, numBones, false);
        var skin = (DAZSkinV2)FormatterServices.GetUninitializedObject(typeof(DAZSkinV2));
        var change = new Matrix4x4[numBones]; var w2l = new Matrix4x4[numBones]; var l2w = new Matrix4x4[numBones];
        var angles = new Vector3[numBones];
        for (int b = 0; b < numBones; b++) { change[b] = Mat(); w2l[b] = Mat(); l2w[b] = Mat(); angles[b] = new Vector3(S(1.2f), S(1.2f), S(1.2f)); }
        var start = new Vector3[numVerts]; for (int v = 0; v < numVerts; v++) start[v] = new Vector3(S(1f), S(1f), S(1f));
        var vCs = (Vector3[])start.Clone(); var vNat = (Vector3[])start.Clone();
        Set(skin, "nodes", c.nodes); Set(skin, "_numBones", numBones); Set(skin, "_useGeneralWeights", false);
        Set(skin, "bulgeScale", 1f); Set(skin, "boneChangeFromOriginalMatrices", change); Set(skin, "boneWorldToLocalMatrices", w2l);
        Set(skin, "boneLocalToWorldMatrices", l2w); Set(skin, "boneRotationAngles", angles); Set(skin, "workingVerts", vCs);
        var isBase = new bool[numVerts]; for (int v = 0; v < numVerts; v++) isBase[v] = true;
        part.Invoke(skin, new object[] { 0, numVerts - 1, isBase, null });
        double f = 1000.0 / Stopwatch.Frequency;
        ulong[] callers = { 0UL, 1UL << 0, 1UL << 16 };
        string[] modes = { "Any", "Cores" };
        int fails = 0;
        foreach (ulong caller in callers)
        {
            CpuBoostNative.TestPinThread(caller);
            System.Threading.Thread.Sleep(5);
            foreach (string mode in modes)
            {
                string placed = CpuBoostPlace.Apply(mode, pool);
                for (int r = 0; r < 30; r++) { Array.Copy(start, vNat, numVerts); CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, false, 1f, change, w2l, l2w, angles, vNat, null, 0, numVerts - 1, isBase, null); }
                CpuBoostPlace.Where(true);
                const int reps = 200;
                var frame = new double[reps]; var read = new double[reps];
                float sink = 0;
                for (int r = 0; r < reps; r++)
                {
                    Array.Copy(start, vNat, numVerts);
                    System.Threading.Thread.Sleep(2);
                    var sw = Stopwatch.StartNew();
                    CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, false, 1f, change, w2l, l2w, angles, vNat, null, 0, numVerts - 1, isBase, null);
                    frame[r] = sw.ElapsedTicks * f;
                    sw = Stopwatch.StartNew();
                    float s = 0; for (int v = 0; v < numVerts; v++) s += vNat[v].x + vNat[v].y + vNat[v].z;
                    read[r] = sw.ElapsedTicks * f; sink += s;
                }
                string where = CpuBoostPlace.Where(true);
                var sw2 = Stopwatch.StartNew();
                for (int r = 0; r < reps; r++) CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, false, 1f, change, w2l, l2w, angles, vNat, null, 0, numVerts - 1, isBase, null);
                double b2b = sw2.ElapsedTicks * f / reps;
                Array.Copy(start, vNat, numVerts);
                CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, false, 1f, change, w2l, l2w, angles, vNat, null, 0, numVerts - 1, isBase, null);
                int bad = Compare(vCs, vNat, "ccd " + mode);
                fails += bad;
                Array.Sort(frame); Array.Sort(read);
                double mean = 0; foreach (double x in frame) mean += x; mean /= reps;
                Console.WriteLine("ccd caller " + (caller == 0UL ? "unpinned" : caller == 1UL ? "lp 0    " : "lp 16   ") + " " + placed.PadRight(26) + " frame median " + frame[reps / 2].ToString("F3") +
                    " mean " + mean.ToString("F3") + " p90 " + frame[reps * 9 / 10].ToString("F3") + " ms, b2b " + b2b.ToString("F3") +
                    " ms, read " + (read[reps / 2] * 1000).ToString("F1") + " us" + (bad == 0 ? ", bits equal" : ", DIFFER"));
                Console.WriteLine("    " + where + (sink == 0.5f ? " " : ""));
            }
        }
        CpuBoostNative.TestPinThread(0);
        CpuBoostPlace.Apply("Any", pool);
        return fails == 0 ? 0 : 1;
    }

    private static int Main(string[] args)
    {
        CpuBoostNative.Load(Path.Combine(args[0], "VaMCpuBoostNative.dll"));
        seed = args.Length > 1 ? uint.Parse(args[1]) : 0x9e3779b9u;
        int cases = args.Length > 2 ? int.Parse(args[2]) : 6;
        int pool = args.Length > 3 ? int.Parse(args[3]) : 0;
        CpuBoostNative.SetThreads(pool);
        MethodInfo part = typeof(DAZSkinV2).GetMethod("SkinMeshPart", F);
        if (args.Length > 4 && args[4] == "bench") return Bench(part, pool);
        if (args.Length > 4 && args[4] == "ccd") return Ccd(part, pool);
        Type mono = Type.GetType("Mono.Runtime");
        Console.WriteLine("runtime " + (mono != null ? "Mono" : "not Mono") + ", seed " + seed + ", cases " + cases + ", native workers " + CpuBoostNative.GetThreads());
        long tCs = 0, tNative = 0, tBuild = 0; long vertsDone = 0;
        int checks = 0;
        for (int ci = 0; ci < cases; ci++)
        {
            bool general = ci % 3 == 2;
            int numVerts = ci == 0 ? 2000 : 12000 + I(20000), numBones = ci == 0 ? 12 : 60 + I(90);
            Case c = MakeCase(numVerts, numBones, general);
            var skin = (DAZSkinV2)FormatterServices.GetUninitializedObject(typeof(DAZSkinV2));
            var change = new Matrix4x4[numBones]; var w2l = new Matrix4x4[numBones]; var l2w = new Matrix4x4[numBones];
            var angles = new Vector3[numBones];
            var startVerts = new Vector3[numVerts];
            var vCs = new Vector3[numVerts]; var vNat = new Vector3[numVerts];
            for (int v = 0; v < numVerts; v++) { startVerts[v] = new Vector3(S(1f), S(1f), S(1f)); vCs[v] = vNat[v] = general ? Vector3.zero : startVerts[v]; }
            float bulgeScale = 0.5f + U();
            Set(skin, "nodes", c.nodes); Set(skin, "_numBones", numBones); Set(skin, "_useGeneralWeights", general);
            Set(skin, "bulgeScale", bulgeScale);
            Set(skin, "boneChangeFromOriginalMatrices", change); Set(skin, "boneWorldToLocalMatrices", w2l);
            Set(skin, "boneLocalToWorldMatrices", l2w); Set(skin, "boneRotationAngles", angles);
            Set(skin, "startVertsCopy", startVerts);
            for (int frame = 0; frame < 3; frame++)
            {
                for (int b = 0; b < numBones; b++)
                {
                    change[b] = Mat(); w2l[b] = Mat(); l2w[b] = Mat();
                    angles[b] = new Vector3(Angle(), Angle(), Angle());
                }
                bool[] onlyVerts = null, onlyBones = null;
                int mode = (ci + frame) % 4;                 // 0 none, 1 onlyVerts, 2 onlyBones, 3 both
                if (mode == 1 || mode == 3) { onlyVerts = new bool[numVerts]; for (int v = 0; v < numVerts; v++) onlyVerts[v] = I(3) != 0; }
                if (mode == 2 || mode == 3) { onlyBones = new bool[numBones]; for (int b = 0; b < numBones; b++) onlyBones[b] = I(4) != 0; }
                int threads = frame == 1 ? 8 : 1;            // VaM's sub-threads each get [i*n, (i+1)*n-1], the last to the end
                int per = numVerts / threads;
                if (threads > 1)
                {
                    // As VaM's sub-threads: the ranges run at the same time, each from its own managed thread.
                    Set(skin, "workingVerts", vCs);
                    for (int t = 0; t < threads; t++)
                    {
                        int s = t * per, e = t == threads - 1 ? numVerts - 1 : (t + 1) * per - 1;
                        var sw0 = Stopwatch.StartNew();
                        part.Invoke(skin, new object[] { s, e, onlyVerts, onlyBones });
                        tCs += sw0.ElapsedTicks;
                    }
                    var workers = new System.Threading.Thread[threads];
                    bool[] okAll = new bool[threads];
                    var swn = Stopwatch.StartNew();
                    for (int t = 0; t < threads; t++)
                    {
                        int s = t * per, e = t == threads - 1 ? numVerts - 1 : (t + 1) * per - 1, ti = t;
                        bool[] ov = onlyVerts, ob = onlyBones;
                        workers[t] = new System.Threading.Thread(() => { okAll[ti] = CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, general, bulgeScale, change, w2l, l2w, angles, vNat, startVerts, s, e, ov, ob); });
                        workers[t].Start();
                    }
                    foreach (var w in workers) w.Join();
                    tNative += swn.ElapsedTicks;
                    foreach (bool ok in okAll) if (!ok) { Console.WriteLine("FAIL  native path declined case " + ci); fails++; }
                    vertsDone += numVerts;
                    threads = 0;
                }
                for (int t = 0; t < threads; t++)
                {
                    int s = t * per, e = t == threads - 1 ? numVerts - 1 : (t + 1) * per - 1;
                    Set(skin, "workingVerts", vCs);
                    var sw = Stopwatch.StartNew();
                    part.Invoke(skin, new object[] { s, e, onlyVerts, onlyBones });
                    if (frame > 0) tCs += sw.ElapsedTicks;
                    sw = Stopwatch.StartNew();
                    bool ok = CpuBoostSkin.TrySkinPart(skin, c.nodes, numBones, general, bulgeScale, change, w2l, l2w, angles,
                        vNat, startVerts, s, e, onlyVerts, onlyBones);
                    if (frame > 0) tNative += sw.ElapsedTicks; else tBuild += sw.ElapsedTicks;
                    if (!ok) { Console.WriteLine("FAIL  native path declined case " + ci); fails++; }
                    if (frame > 0) vertsDone += e - s + 1;
                }
                checks++;
                int bad = Compare(vCs, vNat, "case " + ci + " frame " + frame + (general ? " general" : " daz") + " mode " + mode + " threads " + threads);
                if (bad == 0) Console.WriteLine("ok    case " + ci + " frame " + frame + (general ? " general" : " daz") +
                    " verts " + numVerts + " bones " + numBones + " filter " + mode + " ranges " + (frame == 1 ? "8 concurrent" : "1"));
                if (bad > 0) { Array.Copy(vCs, vNat, numVerts); }   // keep later frames comparable
            }
        }
        double f = 1000.0 / Stopwatch.Frequency;
        Console.WriteLine("time C# " + (tCs * f).ToString("F1") + " ms, native " + (tNative * f).ToString("F1") + " ms (" +
            (vertsDone / 1000) + "k vertices, frames 1-2, single thread each); first frame incl. weight copy " + (tBuild * f).ToString("F1") + " ms");
        Console.WriteLine(fails == 0 ? "PASS " + checks + " checks" : "FAILED " + fails);
        return fails == 0 ? 0 : 1;
    }
}
