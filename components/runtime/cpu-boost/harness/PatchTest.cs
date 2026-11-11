// Runs the built plugin's own Harmony patches (VaMCpuBoost.Install) inside VaM's Mono and checks that every patched
// method gives the unpatched method's bits: SkinMeshPart, ApplyMorphsThreadedFast (transpiler + finalizer),
// recalculateNormals(int[], int[]) and MorphVerticesThreadedFast. Each case runs once on the stock methods, then the
// patches go in, and the same world (built again from the same seed) runs patched, then patched in verify mode.
//     mono_host.exe <VaM folder> PatchTest.exe <build folder with VaMCpuBoost.dll + VaMCpuBoostNative.bin> <VaM folder> [bench]
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization;
using HarmonyLib;
using UnityEngine;

internal sealed class PatchTest : Worlds
{
    private const BindingFlags SF = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
    private static string[] dirs;
    private static int fails, checks;
    private static Type main;

    private static Assembly Resolve(object sender, ResolveEventArgs e)
    {
        string name = new AssemblyName(e.Name).Name + ".dll";
        foreach (string d in dirs) if (File.Exists(Path.Combine(d, name))) return Assembly.LoadFrom(Path.Combine(d, name));
        return null;
    }

    private static void Check(bool ok, string what)
    {
        checks++;
        Console.WriteLine((ok ? "ok    " : "FAIL  ") + what);
        if (!ok) fails++;
    }

    private static int Main(string[] args)
    {
        dirs = new[] { args[0], Path.Combine(args[1], "VaM_Data\\Managed"), Path.Combine(args[1], "BepInEx\\core") };
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;   // before any method that touches Harmony is compiled
        return Run(args[0], args.Length > 2 && args[2] == "bench");
    }

    // The plugin's per-path counters (VaMCpuBoost.Stat).
    private static long Stat(string path, string field)
    {
        object s = main.GetField(path + "Stat", SF).GetValue(null);
        return (long)s.GetType().GetField(field).GetValue(s);
    }
    private static void SetVerify(int n) { main.GetField("verifyEvery", SF).SetValue(null, n); }

    // ---------------------------------------------------------------------------------------------------------------
    // DAZSkinV2.SkinMeshPart: 6000 vertices, 40 bones, weights 1-3 a vertex, a few fully weighted.
    // ---------------------------------------------------------------------------------------------------------------

    private static Matrix4x4 Mat()
    {
        var m = new Matrix4x4();
        m.m00 = 1f + S(0.3f); m.m01 = S(0.8f); m.m02 = S(0.8f); m.m03 = S(1f);
        m.m10 = S(0.8f); m.m11 = 1f + S(0.3f); m.m12 = S(0.8f); m.m13 = S(1f);
        m.m20 = S(0.8f); m.m21 = S(0.8f); m.m22 = 1f + S(0.3f); m.m23 = S(1f); m.m33 = 1f;
        return m;
    }

    private sealed class SkinWorld { public DAZSkinV2 skin; public DAZSkinV2Node[] nodes; public Vector3[] start; public bool[] onlyVerts; }

    private static SkinWorld BuildSkin()
    {
        seed = 424242;
        int nv = 6000, nb = 40;
        var w = new SkinWorld();
        w.nodes = new DAZSkinV2Node[nb];
        var wl = new List<DAZSkinV2VertexWeights>[nb]; var fl = new List<int>[nb];
        for (int b = 0; b < nb; b++) { wl[b] = new List<DAZSkinV2VertexWeights>(); fl[b] = new List<int>(); }
        for (int v = 0; v < nv; v++)
            for (int k = 1 + I(3); k > 0; k--)
            {
                int b = I(nb);
                if (I(6) == 0) { fl[b].Add(v); continue; }
                wl[b].Add(new DAZSkinV2VertexWeights { vertex = v, xweight = U(), yweight = U(), zweight = U(), xleftbulge = U(), xrightbulge = U(),
                    yleftbulge = U(), yrightbulge = U(), zleftbulge = U(), zrightbulge = U() });
            }
        for (int b = 0; b < nb; b++)
            w.nodes[b] = new DAZSkinV2Node { rotationOrder = (Quaternion2Angles.RotationOrder)I(6), weights = wl[b].ToArray(), fullyWeightedVertices = fl[b].ToArray(),
                generalWeights = new DAZSkinV2GeneralVertexWeights[0], bulgeFactors = new DAZSkinV2BulgeFactors { xposleft = U(), xnegleft = U(), xposright = U(),
                xnegright = U(), yposleft = U(), ynegleft = U(), yposright = U(), ynegright = U(), zposleft = U(), znegleft = U(), zposright = U(), znegright = U() } };
        w.skin = (DAZSkinV2)FormatterServices.GetUninitializedObject(typeof(DAZSkinV2));
        var change = new Matrix4x4[nb]; var w2l = new Matrix4x4[nb]; var l2w = new Matrix4x4[nb]; var ang = new Vector3[nb];
        for (int b = 0; b < nb; b++) { change[b] = Mat(); w2l[b] = Mat(); l2w[b] = Mat(); ang[b] = new Vector3(S(1.5f), S(1.5f), S(1.5f)); }
        Set(w.skin, "nodes", w.nodes); Set(w.skin, "_numBones", nb); Set(w.skin, "_useGeneralWeights", false); Set(w.skin, "bulgeScale", 1.3f);
        Set(w.skin, "boneChangeFromOriginalMatrices", change); Set(w.skin, "boneWorldToLocalMatrices", w2l);
        Set(w.skin, "boneLocalToWorldMatrices", l2w); Set(w.skin, "boneRotationAngles", ang);
        w.start = new Vector3[nv]; for (int v = 0; v < nv; v++) w.start[v] = V(1f);
        w.onlyVerts = new bool[nv]; for (int v = 0; v < nv; v++) w.onlyVerts[v] = I(4) != 0;
        return w;
    }

    private static Vector3[] RunSkin(SkinWorld w)
    {
        var verts = (Vector3[])w.start.Clone();
        Set(w.skin, "workingVerts", verts);
        typeof(DAZSkinV2).GetMethod("SkinMeshPart", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .Invoke(w.skin, new object[] { 0, verts.Length - 1, w.onlyVerts, null });
        return verts;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Morph cases: frames of ApplyMorphsThreadedFast with value changes in between.
    // ---------------------------------------------------------------------------------------------------------------

    private sealed class MorphFrame { public Vector3[] verts, visible; public float[] state; public bool result; public string thrown; }
    private sealed class MorphCase { public uint seed; public int nv, nm, maxDeltas, bad; public string name; }

    private static MorphFrame[] RunMorphs(MorphCase c, int frames)
    {
        MWorld w = BuildMorphs(c.seed, c.nv, c.nm, c.maxDeltas, c.bad);
        var r = new MorphFrame[frames];
        for (int f = 0; f < frames; f++)
        {
            var mf = new MorphFrame();
            try { mf.result = w.bank.ApplyMorphsThreadedFast(w.verts, w.visible, null); }
            catch (Exception e) { mf.thrown = e.GetType().Name; }
            mf.verts = (Vector3[])w.verts.Clone(); mf.visible = (Vector3[])w.visible.Clone(); mf.state = MorphState(w);
            r[f] = mf;
            NextMorphFrame(w, c.seed, f);
        }
        return r;
    }

    private static void CompareMorphs(MorphCase c, MorphFrame[] stock, MorphFrame[] got, string how)
    {
        for (int f = 0; f < stock.Length; f++)
        {
            MorphFrame a = stock[f], b = got[f];
            int dv = Diff(a.verts, b.verts), dvis = Diff(a.visible, b.visible), ds = Diff(a.state, b.state);
            bool same = a.result == b.result && a.thrown == b.thrown;
            if (c.bad == 1)
            {
                // A null delta entry: the game's loop throws there every frame. The patched method throws the same
                // exception with the same vertices on the first frame; the later morphs of that call have already had
                // their applied values updated (documented), so only frame 0 is compared, and only the vertices.
                if (f > 0) break;
                Check(dv == 0 && dvis == 0 && a.thrown == b.thrown && a.thrown != null, c.name + " " + how + " frame 0: C# throws " + a.thrown +
                    ", patched throws " + b.thrown + ", vertices differ " + dv + "/" + dvis + " (morph bookkeeping differs in " + ds + " values, expected)");
                continue;
            }
            Check(dv == 0 && dvis == 0 && ds == 0 && same, c.name + " " + how + " frame " + f + ": vertices differ " + dv + ", visible " + dvis +
                ", morph state " + ds + ", result " + a.result + "/" + b.result + (a.thrown != null || b.thrown != null ? ", thrown " + a.thrown + "/" + b.thrown : ""));
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Normals and soft-vertex cases through the patched methods.
    // ---------------------------------------------------------------------------------------------------------------

    private sealed class NormalsFrame { public Vector3[] normals, surf; public bool[] marker; }

    private static NormalsFrame[] RunNormals(uint s, int kind, int frames)
    {
        NWorld w = BuildNormals(s, 3000, 4000, kind);
        var rn = new RecalculateNormals(w.tris, w.verts, w.normals, w.surf, false);
        Set(rn, "thisMarkerArray", w.marker);
        var r = new NormalsFrame[frames];
        for (int f = 0; f < frames; f++)
        {
            rn.recalculateNormals(w.ttu, w.vtu);
            r[f] = new NormalsFrame { normals = (Vector3[])w.normals.Clone(), surf = (Vector3[])w.surf.Clone(), marker = (bool[])w.marker.Clone() };
            for (int i = 0; i < w.verts.Length; i++) w.verts[i] += new Vector3(Mathf.Sin(i * 0.37f + f) * 0.01f, 0.004f, Mathf.Cos(i * 0.21f) * 0.01f);
        }
        return r;
    }

    private sealed class SoftFrame { public Vector3[] psm, moves; public bool[] psv, psnv; public float[] dist, wts; public bool changed; }

    private static SoftFrame[] RunSoft(uint s, int type, int frames)
    {
        SWorld w = BuildSoft(s, 2500, 60, 30, type, false);
        var r = new SoftFrame[frames];
        for (int f = 0; f < frames; f++)
        {
            w.skin.postSkinVertsChanged = false;
            w.g.MorphVerticesThreadedFast(w.verts);
            var sf = new SoftFrame { psm = (Vector3[])w.skin.postSkinMorphs.Clone(), psv = (bool[])w.skin.postSkinVerts.Clone(),
                psnv = (bool[])w.skin.postSkinNormalVerts.Clone(), changed = w.skin.postSkinVertsChanged };
            var d = new List<float>(); var wt = new List<float>(); var mv = new List<Vector3>();
            foreach (var set in w.sets) { d.AddRange(set.influenceVerticesDistances); wt.AddRange(set.influenceVerticesWeights); mv.Add(set.move); }
            sf.dist = d.ToArray(); sf.wts = wt.ToArray(); sf.moves = mv.ToArray();
            r[f] = sf;
            for (int i = 0; i < w.verts.Length; i++) w.verts[i] += new Vector3(0.003f, Mathf.Sin(i * 0.5f + f) * 0.004f, 0.001f);
            foreach (var set in w.sets) set.interpFactor = (f + 1) * 0.31f;
        }
        return r;
    }

    // ---------------------------------------------------------------------------------------------------------------

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static int Run(string build, bool bench)
    {
        Assembly plugin = Assembly.LoadFrom(Path.Combine(build, "VaMCpuBoost.dll"));
        Type native = plugin.GetType("CpuBoostNative");
        main = plugin.GetType("VaMCpuBoost");
        native.GetMethod("Load", SF).Invoke(null, new object[] { Path.Combine(build, "VaMCpuBoostNative.bin") });
        ((Delegate)native.GetField("SetThreads", SF).GetValue(null)).DynamicInvoke(8);

        var morphCases = new List<MorphCase>();
        morphCases.Add(new MorphCase { seed = 11, nv = 3000, nm = 30, maxDeltas = 400, name = "morphs small" });
        morphCases.Add(new MorphCase { seed = 12, nv = 5000, nm = 60, maxDeltas = 2000, name = "morphs medium" });
        morphCases.Add(new MorphCase { seed = 13, nv = 21556, nm = 40, maxDeltas = 20000, name = "morphs body-sized (threads)" });
        morphCases.Add(new MorphCase { seed = 14, nv = 800, nm = 6, maxDeltas = 50, name = "morphs tiny" });
        morphCases.Add(new MorphCase { seed = 15, nv = 3000, nm = 30, maxDeltas = 400, bad = 2, name = "morphs with a null deltas array" });
        morphCases.Add(new MorphCase { seed = 16, nv = 3000, nm = 30, maxDeltas = 400, bad = 1, name = "morphs with a null delta entry" });
        const int frames = 4;

        // ---- stock -------------------------------------------------------------------------------------------------
        SkinWorld skinWorld = BuildSkin();
        Vector3[] skinRef = RunSkin(skinWorld);
        var morphRef = new List<MorphFrame[]>();
        foreach (MorphCase c in morphCases) morphRef.Add(RunMorphs(c, frames));
        var normalsRef = new List<NormalsFrame[]>();
        for (int kind = 0; kind <= 2; kind++) normalsRef.Add(RunNormals(900u + (uint)kind, kind, 3));
        var softRef = new List<SoftFrame[]>();
        for (int type = 0; type <= 2; type++) softRef.Add(RunSoft(700u + (uint)type, type, 3));
        double morphStockMs = bench ? BenchMorphs() : 0;

        // ---- patch -------------------------------------------------------------------------------------------------
        var warnings = new List<string>();
        var h = new Harmony("test.cpuboost");
        string on = (string)main.GetMethod("Install", SF).Invoke(null, new object[] { h, true, true, true, true,
            (Action<string>)delegate(string m) { warnings.Add(m); Console.WriteLine("warn  " + m); } });
        Check(on == "skinning, morphs, normals, soft vertices" && warnings.Count == 0, "Install: on " + on);

        // ---- patched -----------------------------------------------------------------------------------------------
        Vector3[] skinPat = RunSkin(skinWorld);
        Check(Stat("skin", "calls") == 1 && Diff(skinRef, skinPat) == 0, "SkinMeshPart patched: native calls " + Stat("skin", "calls") +
            ", vertices differ " + Diff(skinRef, skinPat));
        for (int i = 0; i < morphCases.Count; i++) CompareMorphs(morphCases[i], morphRef[i], RunMorphs(morphCases[i], frames), "patched");
        Check(Stat("morph", "calls") > 0, "ApplyMorphsThreadedFast went through the native path (flushes " + Stat("morph", "calls") + ")");
        for (int kind = 0; kind <= 2; kind++) CompareNormals(kind, normalsRef[kind], RunNormals(900u + (uint)kind, kind, 3), "patched");
        Check(Stat("normals", "calls") == 9, "recalculateNormals went through the native path (" + Stat("normals", "calls") + " of 9 calls)");
        for (int type = 0; type <= 2; type++) CompareSoft(type, softRef[type], RunSoft(700u + (uint)type, type, 3), "patched");
        Check(Stat("soft", "calls") == 9, "MorphVerticesThreadedFast went through the native path (" + Stat("soft", "calls") + " of 9 calls)");

        // ---- verify mode: the C# runs and is kept, the native result is compared ------------------------------------
        SetVerify(1);
        Vector3[] skinVer = RunSkin(skinWorld);
        Check(Stat("skin", "verified") == 1 && Stat("skin", "differing") == 0 && Diff(skinRef, skinVer) == 0, "SkinMeshPart verify: verified " +
            Stat("skin", "verified") + ", differing " + Stat("skin", "differing"));
        for (int i = 0; i < morphCases.Count; i++) if (morphCases[i].bad == 0) CompareMorphs(morphCases[i], morphRef[i], RunMorphs(morphCases[i], frames), "verify");
        Check(Stat("morph", "verified") > 0 && Stat("morph", "differing") == 0, "ApplyMorphsThreadedFast verify: verified " + Stat("morph", "verified") +
            ", differing " + Stat("morph", "differing"));
        for (int kind = 0; kind <= 2; kind++) CompareNormals(kind, normalsRef[kind], RunNormals(900u + (uint)kind, kind, 3), "verify");
        Check(Stat("normals", "verified") == 9 && Stat("normals", "differing") == 0, "recalculateNormals verify: verified " + Stat("normals", "verified") +
            ", differing " + Stat("normals", "differing"));
        for (int type = 0; type <= 2; type++) CompareSoft(type, softRef[type], RunSoft(700u + (uint)type, type, 3), "verify");
        Check(Stat("soft", "verified") == 9 && Stat("soft", "differing") == 0, "MorphVerticesThreadedFast verify: verified " + Stat("soft", "verified") +
            ", differing " + Stat("soft", "differing"));
        SetVerify(0);

        // ---- a skin the native code declines (null weights on a bone) runs the C#, which throws as it always did -----
        skinWorld.nodes[5].weights = null;
        Exception thrown = null;
        try { RunSkin(skinWorld); } catch (TargetInvocationException e) { thrown = e.InnerException; }
        Check(thrown is NullReferenceException, "declined skin runs the C#, which throws as before (" + (thrown == null ? "no exception" : thrown.GetType().Name) + ")");

        if (bench)
        {
            double pat = BenchMorphs();
            Console.WriteLine("bench morphs: 60 changed morphs, ~240k deltas a call: C# " + morphStockMs.ToString("F3") + " ms, patched " + pat.ToString("F3") + " ms");
        }
        Console.WriteLine((fails == 0 ? "PASS " : "FAILED " + fails + " of ") + checks + " checks");
        return fails == 0 ? 0 : 1;
    }

    private static void CompareNormals(int kind, NormalsFrame[] a, NormalsFrame[] b, string how)
    {
        for (int f = 0; f < a.Length; f++)
        {
            int dn = Diff(a[f].normals, b[f].normals), ds = Diff(a[f].surf, b[f].surf), dm = Diff(a[f].marker, b[f].marker);
            Check(dn == 0 && ds == 0 && dm == 0, "recalculateNormals kind " + kind + " " + how + " frame " + f + ": normals differ " + dn +
                ", surface normals " + ds + ", markers " + dm);
        }
    }

    private static void CompareSoft(int type, SoftFrame[] a, SoftFrame[] b, string how)
    {
        for (int f = 0; f < a.Length; f++)
        {
            int dm = Diff(a[f].psm, b[f].psm), dv = Diff(a[f].psv, b[f].psv), dn = Diff(a[f].psnv, b[f].psnv);
            int dd = Diff(a[f].dist, b[f].dist), dw = Diff(a[f].wts, b[f].wts), dmove = Diff(a[f].moves, b[f].moves);
            Check(dm + dv + dn + dd + dw + dmove == 0 && a[f].changed == b[f].changed, "MorphVerticesThreadedFast type " + type + " " + how +
                " frame " + f + ": morphs differ " + dm + ", flags " + dv + "/" + dn + ", distances " + dd + ", weights " + dw + ", moves " + dmove +
                ", changed " + a[f].changed + "/" + b[f].changed);
        }
    }

    // ms per ApplyMorphsThreadedFast call with 60 morphs of ~4000 deltas changing every call (body-sized mesh).
    private static double BenchMorphs()
    {
        MWorld w = BuildMorphs(77, 21556, 60, 8000, 0);
        foreach (DAZMorph m in w.morphs) { m.disable = false; m.formulas = new DAZMorphFormula[0]; Set(m, "_hasMorphValueFormulas", false); }
        int iters = 40;
        double total = 0;
        for (int i = 0; i < iters + 5; i++)
        {
            foreach (DAZMorph m in w.morphs) Set(m, "_morphValue", (i % 2 == 0 ? 0.5f : -0.5f) + m.appliedValue * 0.001f);
            var sw = Stopwatch.StartNew();
            w.bank.ApplyMorphsThreadedFast(w.verts, w.visible, null);
            if (i >= 5) total += sw.ElapsedTicks * 1000.0 / Stopwatch.Frequency;
        }
        return total / iters;
    }
}
