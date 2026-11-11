// Bit-for-bit checks of the native post-skin normals and soft-body vertex morphs against VaM's own
// RecalculateNormals.recalculateNormals(int[], int[]) and DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast,
// plus native Math.Pow against Mathf.Pow, all inside VaM's Mono (mono_host.exe). Each case builds the same synthetic
// world twice from one seed, runs the game's C# on one and the native code on the other, and compares every output.
//     mono_host.exe <VaM folder> PathsTest.exe <folder with VaMCpuBoostNative.dll> [seed] [cases] [workers] [bench]
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization;
using UnityEngine;

internal sealed class PathsTest : Worlds
{
    private static int fails, checks;

    private static void Check(bool ok, string what)
    {
        checks++;
        if (!ok) { fails++; Console.WriteLine("FAIL  " + what); }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Math.Pow
    // ---------------------------------------------------------------------------------------------------------

    private static float PowBase()
    {
        switch (I(10))
        {
            case 0: return 0f;
            case 1: return 1f;
            case 2: return U() * 1e-30f;
            case 3: return BitConverter.ToSingle(BitConverter.GetBytes((int)(Next() & 0x7fffff)), 0);   // denormal
            case 4: return 1f - U() * 1e-6f;
            case 5: return S(4f);
            default: return U();
        }
    }
    private static float PowExp()
    {
        switch (I(8))
        {
            case 0: return 1f;
            case 1: return 2f;
            case 2: return 0.5f;
            case 3: return 0f;
            case 4: return S(12f);
            default: return U() * 8f;
        }
    }

    private static void PowCheck(int n)
    {
        var f = new float[n]; var p = new float[n]; var r = new float[n];
        for (int i = 0; i < n; i++) { f[i] = PowBase(); p[i] = PowExp(); }
        f[0] = float.NaN; p[1] = float.NaN; f[2] = float.PositiveInfinity; p[3] = float.NegativeInfinity; f[4] = -1f; p[4] = float.PositiveInfinity;
        CpuBoostNative.PowBatch(f, p, r, n);
        int bad = 0, first = -1;
        for (int i = 0; i < n; i++) if (Bits(Mathf.Pow(f[i], p[i])) != Bits(r[i])) { if (first < 0) first = i; bad++; }
        Check(bad == 0, "Math.Pow: " + n + " inputs" + (bad > 0 ? ", " + bad + " differ, first " + f[first].ToString("R") + " ^ " + p[first].ToString("R") : ""));
        if (bad == 0) Console.WriteLine("ok    Math.Pow: native internal call == Mathf.Pow on " + n + " inputs");
    }

    // ---------------------------------------------------------------------------------------------------------
    // RecalculateNormals.recalculateNormals(int[] trianglesToUse, int[] verticesToUse)
    // ---------------------------------------------------------------------------------------------------------

    private static void NormalsCase(uint s, int nv, int nt, int kind)
    {
        NWorld a = BuildNormals(s, nv, nt, kind), b = BuildNormals(s, nv, nt, kind);
        RecalculateNormals rn = MakeRN(a);
        string name = "normals seed " + s + " kind " + kind + " (" + a.ttu.Length + " triangles, " + a.vtu.Length + " vertices)";
        bool throws = kind >= 3;
        for (int frame = 0; frame < 3; frame++)
        {
            Exception thrown = null;
            try { rn.recalculateNormals(a.ttu, a.vtu); } catch (Exception e) { thrown = e; }
            if (throws)
            {
                var before = (Vector3[])b.normals.Clone();
                bool done = CpuBoostNormals.TryRecalc(b.tris, b.verts, b.normals, b.surf, b.marker, b.ttu, b.vtu);
                Check(thrown != null && !done && Diff(before, b.normals) == 0, name + ": the C# throws (" +
                    (thrown == null ? "no exception" : thrown.GetType().Name) + "), native declines without writing");
                return;
            }
            bool ok = CpuBoostNormals.TryRecalc(b.tris, b.verts, b.normals, b.surf, b.marker, b.ttu, b.vtu);
            int dn = Diff(a.normals, b.normals), ds = Diff(a.surf, b.surf), dm = Diff(a.marker, b.marker);
            Check(thrown == null && ok && dn == 0 && ds == 0 && dm == 0, name + " frame " + frame + ": native " + ok +
                ", normals differ " + dn + ", surface normals " + ds + ", markers " + dm + (thrown != null ? ", C# threw " + thrown.GetType().Name : ""));
            // next frame: move the vertices the same way in both worlds
            for (int i = 0; i < nv; i++)
            {
                Vector3 d = new Vector3(Mathf.Sin(i * 0.37f + frame) * 0.01f, Mathf.Cos(i * 0.11f) * 0.01f, 0.003f);
                a.verts[i] += d; b.verts[i] += d;
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast(Vector3[] verts)
    // ---------------------------------------------------------------------------------------------------------

    private static bool SoftNative(SWorld w)
    {
        CpuBoostSoft.Params p = CpuBoostSoft.MakeParams(w.useSim, w.useInterp, w.useCustom, w.lookAt, w.center, w.type, w.autoAnchor,
            w.maxDist, w.scale, w.falloff, w.bias, w.mult);
        bool changed;
        bool done = CpuBoostSoft.TryMorph(w.sets, w.verts, p, w.skin.postSkinVerts, w.skin.postSkinNormalVerts, w.skin.postSkinMorphs, true, out changed);
        if (changed) w.skin.postSkinVertsChanged = true;
        return done;
    }

    private static void SoftCase(uint s, int nv, int nSets, int maxInfl, int type, bool bad)
    {
        SWorld a = BuildSoft(s, nv, nSets, maxInfl, type, bad), b = BuildSoft(s, nv, nSets, maxInfl, type, bad);
        string name = "soft seed " + s + " type " + type + " lookAt " + a.lookAt + " center " + a.center + " interp " +
            (a.useSim && a.useInterp && a.useCustom) + " bias " + a.bias.ToString("R") + " falloff " + a.falloff.ToString("R") + " (" + a.sets.Count + " sets)";
        for (int frame = 0; frame < 3; frame++)
        {
            Exception thrown = null;
            try { a.g.MorphVerticesThreadedFast(a.verts); } catch (Exception e) { thrown = e; }
            if (bad)
            {
                var before = (Vector3[])b.skin.postSkinMorphs.Clone();
                bool done = SoftNative(b);
                Check(thrown != null && !done && Diff(before, b.skin.postSkinMorphs) == 0, name + ": the C# throws (" +
                    (thrown == null ? "no exception" : thrown.GetType().Name) + "), native declines without writing");
                return;
            }
            bool ok = SoftNative(b);
            int dm = Diff(a.skin.postSkinMorphs, b.skin.postSkinMorphs), dv = Diff(a.skin.postSkinVerts, b.skin.postSkinVerts);
            int dn = Diff(a.skin.postSkinNormalVerts, b.skin.postSkinNormalVerts), dd = 0, dw = 0, dmove = 0;
            for (int k = 0; k < a.sets.Count; k++)
            {
                dd += Diff(a.sets[k].influenceVerticesDistances, b.sets[k].influenceVerticesDistances);
                dw += Diff(a.sets[k].influenceVerticesWeights, b.sets[k].influenceVerticesWeights);
                dmove += Diff(new Vector3[] { a.sets[k].move }, new Vector3[] { b.sets[k].move });
            }
            bool changed = a.skin.postSkinVertsChanged == b.skin.postSkinVertsChanged;
            Check(thrown == null && ok && dm == 0 && dv == 0 && dn == 0 && dd == 0 && dw == 0 && dmove == 0 && changed,
                name + " frame " + frame + ": native " + (ok ? "ran" : "declined " + CpuBoostSoft.lastDecline) + ", morphs differ " + dm + ", flags " + dv + "/" + dn +
                ", distances " + dd + ", weights " + dw + ", moves " + dmove + ", changed flag same " + changed +
                (thrown != null ? ", C# threw " + thrown.GetType().Name : ""));
            for (int i = 0; i < nv; i++)
            {
                Vector3 d = new Vector3(Mathf.Sin(i * 0.3f + frame) * 0.004f, 0.002f, Mathf.Cos(i * 0.7f) * 0.003f);
                a.verts[i] += d; b.verts[i] += d;
            }
            for (int k = 0; k < a.sets.Count; k++) { a.sets[k].interpFactor = b.sets[k].interpFactor = (frame + 1) * 0.37f; }
            a.skin.postSkinVertsChanged = b.skin.postSkinVertsChanged = false;
        }
    }

    // ---------------------------------------------------------------------------------------------------------

    private static double Ms(Stopwatch sw, int iters) { return sw.ElapsedTicks * 1000.0 / Stopwatch.Frequency / iters; }

    // The delta loops alone: the game's C# loop against Add + EndCall, 60 morphs of up to 8000 deltas on a body mesh.
    private static void BenchMorphDeltas()
    {
        MWorld a = BuildMorphs(77, 21556, 60, 8000, 0), b = BuildMorphs(77, 21556, 60, 8000, 0);
        int total = 0; foreach (DAZMorph m in a.morphs) total += m.deltas.Length;
        CpuBoostMorph.active = true;
        int iters = 40;
        double cs = 0, nat = 0, flat = 0, apply = 0;
        var t = new double[2];
        for (int i = 0; i < iters + 5; i++)
        {
            float w = i % 2 == 0 ? 0.37f : -0.37f;
            var sw = Stopwatch.StartNew();
            foreach (DAZMorph m in a.morphs)
            {
                bool vis = m.visible && !m.isPoseControl;
                foreach (DAZMorphVertex d in m.deltas)
                {
                    if (d.vertex < a.verts.Length)
                    {
                        a.verts[d.vertex].x += d.delta.x * w; a.verts[d.vertex].y += d.delta.y * w; a.verts[d.vertex].z += d.delta.z * w;
                        if (vis) { a.visible[d.vertex].x += d.delta.x * w; a.visible[d.vertex].y += d.delta.y * w; a.visible[d.vertex].z += d.delta.z * w; }
                    }
                }
            }
            double c = sw.ElapsedTicks * 1000.0 / Stopwatch.Frequency;
            sw = Stopwatch.StartNew();
            foreach (DAZMorph m in b.morphs) CpuBoostMorph.Add(m.deltas, w, m.visible && !m.isPoseControl, b.verts, b.visible);
            CpuBoostMorph.EndCall();
            double n = sw.ElapsedTicks * 1000.0 / Stopwatch.Frequency;
            CpuBoostNative.MorphTimes(t);
            if (i >= 5) { cs += c; nat += n; flat += t[0]; apply += t[1]; }
        }
        Check(Diff(a.verts, b.verts) == 0 && Diff(a.visible, b.visible) == 0, "bench morph deltas bits");
        Console.WriteLine("bench morph deltas: " + total + " deltas a call: C# loop " + (cs / iters).ToString("F3") + " ms, Add + EndCall " +
            (nat / iters).ToString("F3") + " ms (native flatten " + (flat / iters).ToString("F3") + ", apply " + (apply / iters).ToString("F3") + ")");
    }

    private static double IdlePing()
    {
        double t = 0;
        for (int i = 0; i < 50; i++) { System.Threading.Thread.Sleep(1); t += CpuBoostNative.PoolPing(1, 16); }
        return t / 50 * 1000;
    }

    private static void Bench()
    {
        CpuBoostNative.PoolPing(200, 16);
        Console.WriteLine("bench pool: empty job of 16 chunks " + (CpuBoostNative.PoolPing(2000, 16) * 1000).ToString("F1") +
            " us, back to back; after 1 ms idle " + IdlePing().ToString("F1") + " us");
        BenchMorphDeltas();
        // Post-skin normals at body scale: ~21.5k vertices, a 15k-triangle soft-body region.
        NWorld a = BuildNormals(77, 21556, 42000, 0), b = BuildNormals(77, 21556, 42000, 0);
        var ttu = new List<int>(); for (int t = 0; t < 15000; t++) ttu.Add(t * 3);
        a.ttu = b.ttu = ttu.ToArray();
        var seen = new bool[a.verts.Length]; var vtu = new List<int>();
        foreach (int t in a.ttu) for (int k = 0; k < 3; k++) { int v = a.tris[t + k]; if (!seen[v]) { seen[v] = true; vtu.Add(v); } }
        vtu.Sort(); a.vtu = b.vtu = vtu.ToArray();
        RecalculateNormals rn = MakeRN(a);
        for (int i = 0; i < 5; i++) { rn.recalculateNormals(a.ttu, a.vtu); CpuBoostNormals.TryRecalc(b.tris, b.verts, b.normals, b.surf, b.marker, b.ttu, b.vtu); }
        int iters = 50;
        var sw = Stopwatch.StartNew();
        for (int i = 0; i < iters; i++) rn.recalculateNormals(a.ttu, a.vtu);
        double cs = Ms(sw, iters);
        sw = Stopwatch.StartNew();
        for (int i = 0; i < iters; i++) CpuBoostNormals.TryRecalc(b.tris, b.verts, b.normals, b.surf, b.marker, b.ttu, b.vtu);
        double nat = Ms(sw, iters);
        Check(Diff(a.normals, b.normals) == 0, "bench normals bits");
        Console.WriteLine("bench normals: " + a.ttu.Length + " triangles, " + a.vtu.Length + " vertices: C# " + cs.ToString("F3") + " ms, native " + nat.ToString("F3") + " ms");

        // Soft body: four groups of 150 sets x up to 40 influence vertices (breasts and glutes sized).
        foreach (int type in new int[] { 0, 1, 2 })
        {
            var ga = new List<SWorld>(); var gb = new List<SWorld>();
            for (int k = 0; k < 4; k++) { ga.Add(BuildSoft(500u + (uint)k, 21556, 150, 40, type, false)); gb.Add(BuildSoft(500u + (uint)k, 21556, 150, 40, type, false)); }
            int infl = 0; foreach (var s in ga[0].sets) infl += s.influenceVertices.Length;
            for (int i = 0; i < 5; i++) for (int k = 0; k < 4; k++) { ga[k].g.MorphVerticesThreadedFast(ga[k].verts); SoftNative(gb[k]); }
            sw = Stopwatch.StartNew();
            for (int i = 0; i < iters; i++) for (int k = 0; k < 4; k++) ga[k].g.MorphVerticesThreadedFast(ga[k].verts);
            cs = Ms(sw, iters);
            sw = Stopwatch.StartNew();
            for (int i = 0; i < iters; i++) for (int k = 0; k < 4; k++) SoftNative(gb[k]);
            nat = Ms(sw, iters);
            int d = 0; for (int k = 0; k < 4; k++) d += Diff(ga[k].skin.postSkinMorphs, gb[k].skin.postSkinMorphs);
            Check(d == 0, "bench soft bits type " + type);
            Console.WriteLine("bench soft type " + type + ": 4 groups x " + ga[0].sets.Count + " sets (~" + infl + " influence vertices a group): C# " +
                cs.ToString("F3") + " ms, native " + nat.ToString("F3") + " ms");
        }
    }

    private static int Main(string[] args)
    {
        CpuBoostNative.Load(Path.Combine(args[0], "VaMCpuBoostNative.dll"));
        uint baseSeed = args.Length > 1 ? uint.Parse(args[1]) : 2654435769u;
        int cases = args.Length > 2 ? int.Parse(args[2]) : 8;
        int workers = args.Length > 3 ? int.Parse(args[3]) : 0;
        CpuBoostNative.SetThreads(workers);
        CpuBoostAddr.Init();
        Console.WriteLine("ok    Mono layout: array data at +" + CpuBoostAddr.ArrayData + ", DAZMorphVertex.vertex at +" + CpuBoostAddr.MorphVertex +
            ", .delta at +" + CpuBoostAddr.MorphDelta);
        CpuBoostSoft.InitPow();
        PowCheck(2000000);
        if (args.Length > 4 && args[4] == "bench") { Bench(); Console.WriteLine(fails == 0 ? "PASS" : "FAILED " + fails); return fails == 0 ? 0 : 1; }

        for (int c = 0; c < cases; c++)
        {
            uint s = baseSeed + (uint)c * 7919u;
            for (int kind = 0; kind <= 5; kind++) NormalsCase(s + (uint)kind, 3000 + c * 500, 4000 + c * 700, kind);
            for (int type = 0; type <= 3; type++) SoftCase(s + 100u + (uint)type, 2500, 40 + c * 10, 30, type, false);
            SoftCase(s + 200u, 2500, 40, 30, c % 3, true);
        }
        Console.WriteLine((fails == 0 ? "PASS " : "FAILED " + fails + " of ") + checks + " checks");
        return fails == 0 ? 0 : 1;
    }
}
