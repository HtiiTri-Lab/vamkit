// Native DAZSkinV2.SkinMeshPart (VaMCpuBoostNative CB_SkinPart). Shared by the plugin and the offline tests.
// C# 3 / .NET 3.5, compiled with /unsafe: VaM's arrays go to the native code as pointers, nothing is copied per call.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

internal static class CpuBoostNative
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate IntPtr SkinCreateFn(int numVerts, int numBones,
        int[] rotationOrder, float[] bulge12, int[] weightCount, int[] weightVertex, float[] weightData9,
        int[] fullCount, int[] fullVertex, int[] generalCount, int[] generalVertex, float[] generalWeight);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void SkinDestroyFn(IntPtr skin);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void SetThreadsFn(int n);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int SkinPartFn(IntPtr skin, int start, int stop,
        IntPtr onlyVerts, IntPtr onlyBones, int useGeneral, float bulgeScale, IntPtr changeMats, IntPtr w2lMats,
        IntPtr l2wMats, IntPtr angles, IntPtr workingVerts, IntPtr startVertsCopy);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int NormalsSubsetFn(IntPtr triangles, int triLen,
        IntPtr vertices, int vertLen, IntPtr normals, int normLen, IntPtr surfaceNormals, int surfLen,
        IntPtr trianglesToUse, int ttuLen, IntPtr verticesToUse, int vtuLen, IntPtr marker, int markerLen, int normalizeSurface);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int SoftMorphFn(IntPtr sets, int nSets, IntPtr prms,
        IntPtr verts, int vertsLen, IntPtr postSkinVerts, int psvLen, IntPtr postSkinNormalVerts, int psnvLen,
        IntPtr postSkinMorphs, int psmLen, out int changed);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int MorphApplyFn(IntPtr items, int nItems,
        IntPtr verts, int vertsLen, IntPtr visible, int visibleLen, int offVertex, int offDelta);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void SetPowFn(IntPtr fn);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void TimesFn([Out] double[] ms);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate double PingFn(int n, int chunks);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void PowBatchFn(float[] f, float[] p, [Out] float[] result, int n);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void SetWorkerCpusFn(ulong[] masks, int n);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate ulong L3MaskFn(int i);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void CpuHistFn(int kind, [Out] long[] counts, int reset);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate ulong PinFn(ulong mask);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int CoreMasksFn([Out] ulong[] masks, int max);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int TaskStatsFn(int task, [Out] byte[] name, int len, [Out] double[] stats, int reset);

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    internal const int ExpectedApi = 5;
    internal static IntFn ApiVersion;
    internal static SkinCreateFn SkinCreate;
    internal static SkinDestroyFn SkinDestroy;
    internal static SkinPartFn SkinPart;
    internal static SetThreadsFn SetThreads;
    internal static IntFn GetThreads;
    internal static NormalsSubsetFn NormalsSubset;
    internal static SoftMorphFn SoftMorph;
    internal static MorphApplyFn MorphApply;
    internal static SetPowFn SetPow;
    internal static PowBatchFn PowBatch;
    internal static TimesFn MorphTimes;
    internal static PingFn PoolPing;
    internal static SetWorkerCpusFn SetWorkerCpus;
    internal static L3MaskFn L3Mask;
    internal static CpuHistFn CpuHist;
    internal static PinFn TestPinThread;
    internal static CoreMasksFn CoreMasks;
    internal static IntFn TaskCount;
    internal static TaskStatsFn TaskStats;

    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new EntryPointNotFoundException(name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    // VaM's Mono resolves [DllImport] only from VaM_Data\Plugins, so the library is loaded by path and bound by name.
    internal static void Load(string path)
    {
        IntPtr m = LoadLibraryW(path);
        if (m == IntPtr.Zero) throw new DllNotFoundException(path + " (error " + Marshal.GetLastWin32Error() + ")");
        ApiVersion = Bind<IntFn>(m, "CB_ApiVersion");
        int api = ApiVersion();
        if (api != ExpectedApi) throw new InvalidOperationException("VaMCpuBoostNative API " + api + ", expected " + ExpectedApi);
        SkinCreate = Bind<SkinCreateFn>(m, "CB_SkinCreate");
        SkinDestroy = Bind<SkinDestroyFn>(m, "CB_SkinDestroy");
        SkinPart = Bind<SkinPartFn>(m, "CB_SkinPart");
        SetThreads = Bind<SetThreadsFn>(m, "CB_SetThreads");
        GetThreads = Bind<IntFn>(m, "CB_GetThreads");
        NormalsSubset = Bind<NormalsSubsetFn>(m, "CB_NormalsSubset");
        SoftMorph = Bind<SoftMorphFn>(m, "CB_SoftMorph");
        MorphApply = Bind<MorphApplyFn>(m, "CB_MorphApply");
        SetPow = Bind<SetPowFn>(m, "CB_SetPow");
        PowBatch = Bind<PowBatchFn>(m, "CB_PowBatch");
        MorphTimes = Bind<TimesFn>(m, "CB_MorphTimes");
        PoolPing = Bind<PingFn>(m, "CB_PoolPing");
        SetWorkerCpus = Bind<SetWorkerCpusFn>(m, "CB_SetWorkerCpus");
        L3Mask = Bind<L3MaskFn>(m, "CB_L3Mask");
        CpuHist = Bind<CpuHistFn>(m, "CB_CpuHist");
        TestPinThread = Bind<PinFn>(m, "CB_TestPinThread");
        CoreMasks = Bind<CoreMasksFn>(m, "CB_CoreMasks");
        TaskCount = Bind<IntFn>(m, "CB_TaskCount");
        TaskStats = Bind<TaskStatsFn>(m, "CB_TaskStats");
    }
}

// Where the native workers run. Only our own worker threads get CPU masks; VaM's threads and the process keep theirs.
//   Cores   one worker per physical core, on the cores Windows prefers least (on this PC CCD1, which Windows keeps
//           parked while VaM runs on CCD0). Default: in the game the workers then stay off VaM's cores and cannot
//           queue up on one awake core of a parked CCD.
//   Any     no masks, Windows places the workers (on this PC: on the same 4 awake CCD0 cores as VaM's threads)
internal static class CpuBoostPlace
{
    internal static string mode = "Any";

    internal static int L3Count()
    {
        int n = 0;
        while (n < 8 && CpuBoostNative.L3Mask(n) != 0) n++;
        return n;
    }

    // Sets the worker count and placement; returns what is now in place.
    internal static string Apply(string m, int threads)
    {
        ulong[] masks = new ulong[0];
        string k = "Any";
        if (string.Equals((m ?? "").Trim(), "Cores", StringComparison.OrdinalIgnoreCase))
        {
            var cores = new ulong[64];
            int count = CpuBoostNative.CoreMasks(cores, 64);
            if (count > 0)
            {
                k = "Cores"; masks = new ulong[Math.Min(count, Math.Max(threads, 1))];
                for (int i = 0; i < masks.Length; i++) masks[i] = cores[i];
            }
        }
        if (CpuBoostNative.GetThreads() != threads) CpuBoostNative.SetThreads(threads);
        CpuBoostNative.SetWorkerCpus(masks, masks.Length);
        mode = k;
        return k + ", " + CpuBoostNative.GetThreads() + " workers";
    }

    // Calls and worker chunks by CCD since the last reset: skin / morphs / normals / soft (caller CPU), workers, and
    // worker chunks on the caller's CCD vs another.
    internal static string Where(bool reset)
    {
        string[] names = { "skin", "morphs", "normals", "soft", "workers" };
        var c = new long[64];
        var s = new System.Text.StringBuilder();
        for (int kind = 0; kind < names.Length; kind++)
        {
            CpuBoostNative.CpuHist(kind, c, reset ? 1 : 0);
            long[] byL3 = new long[8]; long smt = 0, all = 0;
            for (int lp = 0; lp < 64; lp++)
            {
                if (c[lp] == 0) continue;
                all += c[lp];
                if ((lp & 1) == 1) smt += c[lp];
                for (int l = 0; l < 8; l++) if ((CpuBoostNative.L3Mask(l) >> lp & 1UL) != 0) byL3[l] += c[lp];
            }
            s.Append(names[kind]).Append(" ccd0 ").Append(byL3[0]).Append(" ccd1 ").Append(byL3[1]).Append(" odd-lp ").Append(smt).Append("; ");
        }
        CpuBoostNative.CpuHist(5, c, reset ? 1 : 0);
        s.Append("chunks same-ccd ").Append(c[0]).Append(" cross-ccd ").Append(c[1]);
        return s.ToString();
    }

    // Per-CPU counts of one kind (0 skin, 1 morphs, 2 normals, 3 soft, 4 workers), for the in-game report.
    internal static string Cpus(int kind)
    {
        var c = new long[64];
        CpuBoostNative.CpuHist(kind, c, 0);
        var s = new System.Text.StringBuilder();
        for (int lp = 0; lp < 32; lp++) s.Append(lp == 0 ? "" : ",").Append(c[lp]);
        return s.ToString();
    }
}

internal static class CpuBoostSkin
{
    // One native copy of a skin's weights. The references it was built from are kept so a re-imported or re-initialised
    // skin is noticed on the next call and rebuilt.
    private sealed class Model
    {
        public IntPtr handle;
        public DAZSkinV2Node[] nodes;
        public int numBones, numVerts;
        public object[] parts;        // per bone: weights, fullyWeightedVertices, generalWeights, bulgeFactors
        public int[] orders;
        public bool unusable;         // shape the native code does not take: always the C#
        // Lists the C# would dereference and find null (it throws there); a call that needs one goes to the C#.
        public bool[] bulgeMissing, weightsMissing, generalMissing;
    }

    // By reference: UnityEngine.Object's Equals / GetHashCode go through instance IDs and the native object.
    private sealed class ByReference : IEqualityComparer<DAZSkinV2>
    {
        public bool Equals(DAZSkinV2 a, DAZSkinV2 b) { return ReferenceEquals(a, b); }
        public int GetHashCode(DAZSkinV2 o) { return System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(o); }
    }

    private static readonly Dictionary<DAZSkinV2, Model> models = new Dictionary<DAZSkinV2, Model>(new ByReference());
    private static readonly object gate = new object();
    internal static int calls, fallbacks, builds;

    private static bool Matches(Model m, DAZSkinV2Node[] nodes, int numBones, int numVerts)
    {
        if (m.nodes != nodes || m.numBones != numBones || m.numVerts != numVerts) return false;
        for (int b = 0; b < numBones; b++)
        {
            DAZSkinV2Node n = nodes[b];
            if (n == null) return false;
            int k = b * 4;
            if (!ReferenceEquals(m.parts[k], n.weights) || !ReferenceEquals(m.parts[k + 1], n.fullyWeightedVertices) ||
                !ReferenceEquals(m.parts[k + 2], n.generalWeights) || !ReferenceEquals(m.parts[k + 3], n.bulgeFactors) ||
                m.orders[b] != (int)n.rotationOrder) return false;
        }
        return true;
    }

    private static Model Build(DAZSkinV2Node[] nodes, int numBones, int numVerts)
    {
        Model m = new Model();
        m.nodes = nodes; m.numBones = numBones; m.numVerts = numVerts;
        m.parts = new object[numBones * 4];
        m.orders = new int[numBones];
        var bulge = new float[numBones * 12];
        var wCount = new int[numBones]; var fCount = new int[numBones]; var gCount = new int[numBones];
        int nw = 0, nf = 0, ng = 0;
        for (int b = 0; b < numBones; b++)
        {
            DAZSkinV2Node n = nodes[b];
            if (n == null) { m.unusable = true; return m; }
            m.parts[b * 4] = n.weights; m.parts[b * 4 + 1] = n.fullyWeightedVertices;
            m.parts[b * 4 + 2] = n.generalWeights; m.parts[b * 4 + 3] = n.bulgeFactors;
            m.orders[b] = (int)n.rotationOrder;
            // The C# reads each list only in its own mode; a missing list is fine as long as that mode never runs, so
            // a null list just counts as empty here and the per-call mode check below keeps the C# for the other mode.
            if (n.weights != null) { foreach (var w in n.weights) if (w == null) { m.unusable = true; return m; } wCount[b] = n.weights.Length; }
            if (n.fullyWeightedVertices != null) fCount[b] = n.fullyWeightedVertices.Length;
            if (n.generalWeights != null) { foreach (var g in n.generalWeights) if (g == null) { m.unusable = true; return m; } gCount[b] = n.generalWeights.Length; }
            nw += wCount[b]; nf += fCount[b]; ng += gCount[b];
            DAZSkinV2BulgeFactors f = n.bulgeFactors;
            if (f != null)
            {
                int k = b * 12;
                bulge[k] = f.xposleft; bulge[k + 1] = f.xnegleft; bulge[k + 2] = f.xposright; bulge[k + 3] = f.xnegright;
                bulge[k + 4] = f.yposleft; bulge[k + 5] = f.ynegleft; bulge[k + 6] = f.yposright; bulge[k + 7] = f.ynegright;
                bulge[k + 8] = f.zposleft; bulge[k + 9] = f.znegleft; bulge[k + 10] = f.zposright; bulge[k + 11] = f.znegright;
            }
        }
        var wVertex = new int[nw]; var wData = new float[nw * 9]; var fVertex = new int[nf];
        var gVertex = new int[ng]; var gWeight = new float[ng];
        int wi = 0, fi = 0, gi = 0;
        for (int b = 0; b < numBones; b++)
        {
            DAZSkinV2Node n = nodes[b];
            for (int i = 0; i < wCount[b]; i++, wi++)
            {
                DAZSkinV2VertexWeights w = n.weights[i];
                wVertex[wi] = w.vertex;
                int k = wi * 9;
                wData[k] = w.xweight; wData[k + 1] = w.yweight; wData[k + 2] = w.zweight;
                wData[k + 3] = w.xleftbulge; wData[k + 4] = w.xrightbulge; wData[k + 5] = w.yleftbulge;
                wData[k + 6] = w.yrightbulge; wData[k + 7] = w.zleftbulge; wData[k + 8] = w.zrightbulge;
            }
            for (int i = 0; i < fCount[b]; i++, fi++) fVertex[fi] = n.fullyWeightedVertices[i];
            for (int i = 0; i < gCount[b]; i++, gi++) { gVertex[gi] = n.generalWeights[i].vertex; gWeight[gi] = n.generalWeights[i].weight; }
        }
        m.handle = CpuBoostNative.SkinCreate(numVerts, numBones, m.orders, bulge, wCount, wVertex, wData, fCount, fVertex,
            gCount, gVertex, gWeight);
        if (m.handle == IntPtr.Zero) m.unusable = true;   // a vertex index outside workingVerts: the C# throws there
        m.bulgeMissing = new bool[numBones];
        for (int b = 0; b < numBones; b++) m.bulgeMissing[b] = nodes[b].bulgeFactors == null;
        m.weightsMissing = new bool[numBones];
        for (int b = 0; b < numBones; b++) m.weightsMissing[b] = nodes[b].weights == null || nodes[b].fullyWeightedVertices == null;
        m.generalMissing = new bool[numBones];
        for (int b = 0; b < numBones; b++) m.generalMissing[b] = nodes[b].generalWeights == null;
        builds++;
        return m;
    }

    private static Model Get(DAZSkinV2 skin, DAZSkinV2Node[] nodes, int numBones, int numVerts)
    {
        lock (gate)
        {
            Model m;
            if (models.TryGetValue(skin, out m) && Matches(m, nodes, numBones, numVerts)) return m;
            if (m != null && m.handle != IntPtr.Zero) CpuBoostNative.SkinDestroy(m.handle);
            m = Build(nodes, numBones, numVerts);
            models[skin] = m;
            return m;
        }
    }

    internal static void Forget(DAZSkinV2 skin)
    {
        lock (gate)
        {
            Model m;
            if (!models.TryGetValue(skin, out m)) return;
            if (m.handle != IntPtr.Zero) CpuBoostNative.SkinDestroy(m.handle);
            models.Remove(skin);
        }
    }

    // Runs SkinMeshPart natively and returns true, or returns false for the C# to run (same inputs, untouched).
    internal static unsafe bool TrySkinPart(DAZSkinV2 skin, DAZSkinV2Node[] nodes, int numBones, bool useGeneral,
        float bulgeScale, Matrix4x4[] change, Matrix4x4[] w2l, Matrix4x4[] l2w, Vector3[] angles,
        Vector3[] workingVerts, Vector3[] startVertsCopy, int start, int stop, bool[] onlyVerts, bool[] onlyBones)
    {
        calls++;
        // Anything the C# would throw on, or that this code was not built for, stays with the C#.
        if (nodes == null || workingVerts == null || change == null || numBones <= 0 || numBones > nodes.Length ||
            change.Length < numBones || (onlyBones != null && onlyBones.Length < numBones) ||
            (onlyVerts != null && onlyVerts.Length < workingVerts.Length))
        { fallbacks++; return false; }
        if (useGeneral ? (startVertsCopy == null || startVertsCopy.Length < workingVerts.Length)
                       : (w2l == null || l2w == null || angles == null || w2l.Length < numBones || l2w.Length < numBones || angles.Length < numBones))
        { fallbacks++; return false; }
        Model m = Get(skin, nodes, numBones, workingVerts.Length);
        if (m.unusable) { fallbacks++; return false; }
        for (int b = 0; b < numBones; b++)
        {
            if (onlyBones != null && !onlyBones[b]) continue;
            if (useGeneral ? m.generalMissing[b] : (m.weightsMissing[b] || m.bulgeMissing[b])) { fallbacks++; return false; }
        }
        int r;
        fixed (Matrix4x4* pc = change)
        fixed (Matrix4x4* pw = w2l)
        fixed (Matrix4x4* pl = l2w)
        fixed (Vector3* pa = angles)
        fixed (Vector3* pv = workingVerts)
        fixed (Vector3* ps = startVertsCopy)
        fixed (bool* pov = onlyVerts)
        fixed (bool* pob = onlyBones)
        {
            r = CpuBoostNative.SkinPart(m.handle, start, stop, (IntPtr)pov, (IntPtr)pob, useGeneral ? 1 : 0, bulgeScale,
                (IntPtr)pc, (IntPtr)pw, (IntPtr)pl, (IntPtr)pa, (IntPtr)pv, (IntPtr)ps);
        }
        if (r != 0) { fallbacks++; return false; }
        return true;
    }
}
