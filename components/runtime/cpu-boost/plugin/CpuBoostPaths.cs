// Native post-skin normals, soft-body vertex morphs and morph deltas (VaMCpuBoostNative CB_NormalsSubset /
// CB_SoftMorph / CB_MorphApply). Shared by the plugin and the offline tests. C# 3 / .NET 3.5, compiled with /unsafe.
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using UnityEngine;

// How the plugin and the offline test recognise the methods of VaM they replace: by what the code does, not by which
// build it is from. A method body's bytes contain metadata tokens, numbers that change whenever VaM is compiled again,
// also when no instruction changed (1.22.0.13 has the instructions of 1.22.0.3 in all four methods and other tokens
// throughout). So the fingerprint is taken over the instructions with every token replaced by what it names: the
// member's declaring type, name and signature, or the string. Same fingerprint = same instructions on the same
// members. A method that really changed gets another fingerprint and is left to VaM.
internal static class CpuBoostCode
{
    // FNV-1a 64 of the resolved code, of the methods the native library was written against (VaM 1.22.0.3) and checked
    // bit for bit (harness/run-skin.ps1, run-paths.ps1, run-patch.ps1). NormalsCore is the static overload that
    // recalculateNormals(int[], int[]) forwards to.
    internal const ulong Skin = 0xec3d23f3bd18b799UL, Morph = 0x6e20284c4d78f74eUL, Normals = 0x4fd7e85e07238299UL,
        NormalsCore = 0xb1d69dec0e65d91fUL, Soft = 0x7c491976c95ac37aUL;

    internal sealed class Target { public string name; public MethodBase method; public ulong expected; }

    private const BindingFlags Any = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
    private static OpCode[] oneByte, twoByte;

    // The methods and what the plugin expects of them. The plugin finds them the same way (VaMCpuBoost.Install).
    internal static Target[] Targets()
    {
        return new Target[]
        {
            Of("DAZSkinV2.SkinMeshPart", typeof(DAZSkinV2).GetMethod("SkinMeshPart", Any, null,
                new Type[] { typeof(int), typeof(int), typeof(bool[]), typeof(bool[]) }, null), Skin),
            Of("DAZMorphBank.ApplyMorphsThreadedFast", typeof(DAZMorphBank).GetMethod("ApplyMorphsThreadedFast", Any, null,
                new Type[] { typeof(Vector3[]), typeof(Vector3[]), typeof(DAZBones) }, null), Morph),
            Of("RecalculateNormals.recalculateNormals(int[], int[])", typeof(RecalculateNormals).GetMethod("recalculateNormals", Any, null,
                new Type[] { typeof(int[]), typeof(int[]) }, null), Normals),
            Of("RecalculateNormals.recalculateNormals (static, subset)", typeof(RecalculateNormals).GetMethod("recalculateNormals",
                BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null,
                new Type[] { typeof(int[]), typeof(Vector3[]), typeof(Vector3[]), typeof(Vector3[]), typeof(int[]), typeof(int[]), typeof(bool[]), typeof(bool) }, null), NormalsCore),
            Of("DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast", typeof(DAZPhysicsMeshSoftVerticesGroup).GetMethod("MorphVerticesThreadedFast", Any, null,
                new Type[] { typeof(Vector3[]) }, null), Soft),
        };
    }

    private static Target Of(string name, MethodBase method, ulong expected)
    {
        Target t = new Target();
        t.name = name; t.method = method; t.expected = expected;
        return t;
    }

    internal static ulong Fingerprint(MethodBase m)
    {
        if (oneByte == null)
        {
            OpCode[] one = new OpCode[256], two = new OpCode[256];
            foreach (FieldInfo f in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                if (f.FieldType != typeof(OpCode)) continue;
                OpCode c = (OpCode)f.GetValue(null);
                if (c.Size == 1) one[(ushort)c.Value & 0xff] = c; else two[(ushort)c.Value & 0xff] = c;
            }
            twoByte = two; oneByte = one;
        }
        byte[] il = m.GetMethodBody().GetILAsByteArray();
        Module module = m.Module;
        Type[] ofType = m.DeclaringType != null && m.DeclaringType.IsGenericType ? m.DeclaringType.GetGenericArguments() : null;
        Type[] ofMethod = m.IsGenericMethod ? m.GetGenericArguments() : null;
        ulong h = 14695981039346656037UL;
        int i = 0;
        while (i < il.Length)
        {
            OpCode c;
            if (il[i] == 0xFE && i + 1 < il.Length) { h = Add(h, il[i]); h = Add(h, il[i + 1]); c = twoByte[il[i + 1]]; i += 2; }
            else { h = Add(h, il[i]); c = oneByte[il[i]]; i += 1; }
            int size;
            bool token = false, text = false;
            switch (c.OperandType)
            {
                case OperandType.InlineNone: size = 0; break;
                case OperandType.ShortInlineBrTarget: case OperandType.ShortInlineI: case OperandType.ShortInlineVar: size = 1; break;
                case OperandType.InlineVar: size = 2; break;
                case OperandType.InlineI8: case OperandType.InlineR: size = 8; break;
                case OperandType.InlineSwitch: size = 4 + 4 * BitConverter.ToInt32(il, i); break;
                case OperandType.InlineString: size = 4; text = true; break;
                case OperandType.InlineField: case OperandType.InlineMethod: case OperandType.InlineType: case OperandType.InlineTok:
                    size = 4; token = true; break;
                default: size = 4; break;   // a branch target, a 32-bit constant, a float, a signature
            }
            if (i + size > il.Length) size = il.Length - i;
            string named = null;
            if (size == 4 && (text || token))
            {
                int value = BitConverter.ToInt32(il, i);
                try { named = text ? "\"" + module.ResolveString(value) : Name(module.ResolveMember(value, ofType, ofMethod)); }
                catch (Exception) { named = null; }   // not resolvable here: the token itself is hashed, and nothing will match
            }
            if (named != null) foreach (char ch in named) { h = Add(h, (byte)ch); h = Add(h, (byte)(ch >> 8)); }
            else for (int k = 0; k < size; k++) h = Add(h, il[i + k]);
            i += size;
        }
        return h;
    }

    private static ulong Add(ulong h, byte b) { return (h ^ b) * 1099511628211UL; }

    private static string Name(Type t) { return t == null ? "?" : (t.FullName ?? t.Name); }

    private static string Name(MemberInfo member)
    {
        Type type = member as Type;
        if (type != null) return "T:" + Name(type);
        FieldInfo field = member as FieldInfo;
        if (field != null) return "F:" + Name(field.DeclaringType) + "::" + field.Name + ":" + Name(field.FieldType);
        MethodBase method = member as MethodBase;
        if (method == null) return "?:" + member.Name;
        System.Text.StringBuilder s = new System.Text.StringBuilder("M:");
        s.Append(Name(method.DeclaringType)).Append("::").Append(method.Name).Append('(');
        foreach (ParameterInfo p in method.GetParameters()) s.Append(Name(p.ParameterType)).Append(',');
        s.Append(')');
        MethodInfo info = method as MethodInfo;
        if (info != null) s.Append(Name(info.ReturnType));
        if (method.IsGenericMethod) foreach (Type a in method.GetGenericArguments()) s.Append('<').Append(Name(a)).Append('>');
        return s.ToString();
    }
}

// Addresses of managed objects for the native code. VaM's Mono GC (Boehm) never moves an object, so an address stays
// valid for as long as the object is referenced, and every object handed over is referenced by the caller throughout.
internal static unsafe class CpuBoostAddr
{
    internal delegate IntPtr AddrFn(object o);
    internal static AddrFn Of;
    internal static int ArrayData;                  // element 0 of an array, from the array's address
    internal static int MorphVertex, MorphDelta;    // DAZMorphVertex.vertex / .delta, from the object's address

    internal static void Init()
    {
        var dm = new DynamicMethod("CpuBoostAddrOf", typeof(IntPtr), new Type[] { typeof(object) }, typeof(CpuBoostAddr).Module, true);
        ILGenerator il = dm.GetILGenerator();
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Conv_U);
        il.Emit(OpCodes.Ret);
        Of = (AddrFn)dm.CreateDelegate(typeof(AddrFn));

        // The layout is checked against the compiler's own pointers before anything relies on it.
        var f = new float[3]; var n = new int[3]; var v = new Vector3[3]; var b = new bool[3];
        fixed (float* p = f) ArrayData = (int)((byte*)p - (byte*)Of(f));
        fixed (int* p = n) Check((byte*)p - (byte*)Of(n) == ArrayData, "int[] data offset");
        fixed (Vector3* p = v) Check((byte*)p - (byte*)Of(v) == ArrayData, "Vector3[] data offset");
        fixed (bool* p = b) Check((byte*)p - (byte*)Of(b) == ArrayData, "bool[] data offset");
        Check(ArrayData >= 16 && ArrayData <= 64, "array data offset " + ArrayData);

        var mv = new DAZMorphVertex();
        mv.vertex = 0x1234567;
        mv.delta = new Vector3(1.5f, -2.25f, 3.125f);
        fixed (int* p = &mv.vertex) MorphVertex = (int)((byte*)p - (byte*)Of(mv));
        fixed (Vector3* p = &mv.delta) MorphDelta = (int)((byte*)p - (byte*)Of(mv));
        var arr = new DAZMorphVertex[] { mv };
        byte* o = (byte*)*(IntPtr*)((byte*)Of(arr) + ArrayData);
        Check(o == (byte*)Of(mv), "DAZMorphVertex[] element");
        Check(*(int*)(o + MorphVertex) == 0x1234567 && ((float*)(o + MorphDelta))[0] == 1.5f &&
              ((float*)(o + MorphDelta))[1] == -2.25f && ((float*)(o + MorphDelta))[2] == 3.125f, "DAZMorphVertex fields");
    }

    internal static IntPtr Data(Array a) { return a == null ? IntPtr.Zero : (IntPtr)((byte*)Of(a) + ArrayData); }
    internal static int Len(Array a) { return a == null ? -1 : a.Length; }

    private static void Check(bool ok, string what)
    {
        if (!ok) throw new InvalidOperationException("unexpected Mono object layout: " + what);
    }
}

// RecalculateNormals.recalculateNormals(int[] trianglesToUse, int[] verticesToUse): the post-skin normals.
internal static unsafe class CpuBoostNormals
{
    internal static long calls, fallbacks;

    // Runs it natively and returns true, or returns false (nothing written) for the C# to run.
    internal static bool TryRecalc(int[] triangles, Vector3[] vertices, Vector3[] normals, Vector3[] surfaceNormals,
        bool[] marker, int[] trianglesToUse, int[] verticesToUse)
    {
        calls++;
        int r;
        fixed (int* tri = triangles) fixed (Vector3* ve = vertices) fixed (Vector3* no = normals)
        fixed (Vector3* sn = surfaceNormals) fixed (bool* mk = marker) fixed (int* ttu = trianglesToUse)
        fixed (int* vtu = verticesToUse)
        {
            r = CpuBoostNative.NormalsSubset((IntPtr)tri, CpuBoostAddr.Len(triangles), (IntPtr)ve, CpuBoostAddr.Len(vertices),
                (IntPtr)no, CpuBoostAddr.Len(normals), (IntPtr)sn, CpuBoostAddr.Len(surfaceNormals),
                (IntPtr)ttu, CpuBoostAddr.Len(trianglesToUse), (IntPtr)vtu, CpuBoostAddr.Len(verticesToUse),
                (IntPtr)mk, CpuBoostAddr.Len(marker), 1);
        }
        if (r != 0) { fallbacks++; return false; }
        return true;
    }
}

// DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast(Vector3[] verts): soft-body moves into post-skin morphs.
internal static unsafe class CpuBoostSoft
{
    // One DAZPhysicsMeshSoftVerticesSet for the native code (cpuboost_native.cpp SoftSet).
    [StructLayout(LayoutKind.Explicit, Size = 128)]
    internal struct Set
    {
        [FieldOffset(0)] public int target;
        [FieldOffset(4)] public int anchor;
        [FieldOffset(8)] public int count;
        [FieldOffset(16)] public IntPtr infl;
        [FieldOffset(24)] public IntPtr dist;
        [FieldOffset(32)] public IntPtr wts;
        [FieldOffset(40)] public int distLen;
        [FieldOffset(44)] public int wtsLen;
        [FieldOffset(48)] public Vector3 primaryMove;
        [FieldOffset(60)] public Vector3 move;
        [FieldOffset(72)] public Vector3 lastPositionThreaded;
        [FieldOffset(84)] public Vector3 currentPosition;
        [FieldOffset(96)] public Vector3 lastKinematicPositionThreaded;
        [FieldOffset(108)] public Vector3 currentKinematicPosition;
        [FieldOffset(120)] public float interpFactor;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Params
    {
        public int useInterp, lookAtNormal, center, influenceType, autoInfluenceAnchor;
        public float maxInfluenceDistance, oneoverscale, falloffPower, weightBias, weightMultiplier;
    }

    [ThreadStatic] private static Set[] buffer;
    internal static long calls, fallbacks;
    internal static int lastDecline;   // the native code's reason for the last decline (diagnostics)
    internal static bool powReady;

    // The values MorphVerticesThreadedFast works out before its loop (V_0, V_2) and the group settings it reads.
    internal static Params MakeParams(bool useSimulation, bool useInterpolation, bool useCustomInterpolation, int lookAtOption,
        bool centerBetweenTargetAndAnchor, int influenceType, bool autoInfluenceAnchor, float maxInfluenceDistance,
        float oneoverscale, float falloffPower, float weightBias, float weightMultiplier)
    {
        Params p;
        p.useInterp = useSimulation && useInterpolation && useCustomInterpolation ? 1 : 0;
        p.lookAtNormal = lookAtOption == 2 || lookAtOption == 3 || lookAtOption == 4 ? 1 : 0;
        p.center = centerBetweenTargetAndAnchor ? 1 : 0;
        p.influenceType = influenceType;
        p.autoInfluenceAnchor = autoInfluenceAnchor ? 1 : 0;
        p.maxInfluenceDistance = maxInfluenceDistance;
        p.oneoverscale = oneoverscale;
        p.falloffPower = falloffPower;
        p.weightBias = weightBias;
        p.weightMultiplier = weightMultiplier;
        return p;
    }

    [DllImport("kernel32", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr LookupIcallFn(IntPtr monoMethod);

    // Mathf.Pow is (float)Math.Pow, an internal call into the C runtime inside mono.dll: hand the native code that
    // very function, then check it against Mathf.Pow before using it.
    internal static void InitPow()
    {
        powReady = false;
        IntPtr mono = GetModuleHandleW("mono.dll");
        if (mono == IntPtr.Zero) throw new DllNotFoundException("mono.dll is not loaded");
        IntPtr lookup = GetProcAddress(mono, "mono_lookup_internal_call");
        if (lookup == IntPtr.Zero) throw new EntryPointNotFoundException("mono_lookup_internal_call");
        MethodInfo pow = typeof(Math).GetMethod("Pow", new Type[] { typeof(double), typeof(double) });
        var fn = (LookupIcallFn)Marshal.GetDelegateForFunctionPointer(lookup, typeof(LookupIcallFn));
        IntPtr icall = fn(pow.MethodHandle.Value);
        if (icall == IntPtr.Zero) throw new EntryPointNotFoundException("System.Math::Pow internal call");
        CpuBoostNative.SetPow(icall);
        const int n = 4096;
        var f = new float[n]; var p = new float[n]; var r = new float[n];
        uint s = 12345;
        for (int i = 0; i < n; i++)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5; f[i] = (s >> 8) * (1f / 16777216f);
            s ^= s << 13; s ^= s >> 17; s ^= s << 5; p[i] = (s >> 8) * (8f / 16777216f);
        }
        f[0] = 0f; f[1] = 1f; p[2] = 0f; p[3] = 1f; f[4] = float.NaN; p[5] = float.NaN;
        CpuBoostNative.PowBatch(f, p, r, n);
        for (int i = 0; i < n; i++)
        {
            float c = Mathf.Pow(f[i], p[i]);
            if (BitConverter.ToInt32(BitConverter.GetBytes(c), 0) != BitConverter.ToInt32(BitConverter.GetBytes(r[i]), 0))
            {
                CpuBoostNative.SetPow(IntPtr.Zero);
                throw new InvalidOperationException("native Math.Pow differs from Mathf.Pow at " + f[i].ToString("R") + " ^ " + p[i].ToString("R"));
            }
        }
        powReady = true;
    }

    // Runs it natively into the given post-skin arrays and returns true, or returns false (nothing written) for the C# to
    // run. writeMoves: set each set's move as the C# does (off for a verify run, which leaves the game's state alone).
    internal static bool TryMorph(List<DAZPhysicsMeshSoftVerticesSet> sets, Vector3[] verts, Params prm, bool[] psv,
        bool[] psnv, Vector3[] psm, bool writeMoves, out bool changed)
    {
        calls++;
        changed = false;
        if (sets == null) { fallbacks++; return false; }
        int n = sets.Count;
        if (n == 0) return true;
        Set[] buf = buffer;
        if (buf == null || buf.Length < n) buffer = buf = new Set[Math.Max(n, 64)];
        for (int i = 0; i < n; i++)
        {
            DAZPhysicsMeshSoftVerticesSet s = sets[i];
            if (s == null) { fallbacks++; return false; }
            buf[i].target = s.targetVertex;
            buf[i].anchor = s.anchorVertex;
            buf[i].count = CpuBoostAddr.Len(s.influenceVertices);
            buf[i].infl = CpuBoostAddr.Data(s.influenceVertices);
            buf[i].dist = CpuBoostAddr.Data(s.influenceVerticesDistances);
            buf[i].distLen = CpuBoostAddr.Len(s.influenceVerticesDistances);
            buf[i].wts = CpuBoostAddr.Data(s.influenceVerticesWeights);
            buf[i].wtsLen = CpuBoostAddr.Len(s.influenceVerticesWeights);
            buf[i].primaryMove = s.primaryMove;
            buf[i].move = s.move;
            buf[i].lastPositionThreaded = s.lastPositionThreaded;
            buf[i].currentPosition = s.currentPosition;
            buf[i].lastKinematicPositionThreaded = s.lastKinematicPositionThreaded;
            buf[i].currentKinematicPosition = s.currentKinematicPosition;
            buf[i].interpFactor = s.interpFactor;
        }
        int r, c;
        fixed (Set* sp = buf) fixed (Vector3* vp = verts) fixed (bool* a = psv) fixed (bool* b = psnv) fixed (Vector3* m = psm)
        {
            r = CpuBoostNative.SoftMorph((IntPtr)sp, n, (IntPtr)(&prm), (IntPtr)vp, CpuBoostAddr.Len(verts),
                (IntPtr)a, CpuBoostAddr.Len(psv), (IntPtr)b, CpuBoostAddr.Len(psnv), (IntPtr)m, CpuBoostAddr.Len(psm), out c);
        }
        if (r != 0) { fallbacks++; lastDecline = r; return false; }
        if (writeMoves && prm.useInterp != 0)
            for (int i = 0; i < n; i++) sets[i].move = buf[i].move;
        changed = c != 0;
        return true;
    }

    // The moves the last TryMorph on this thread worked out (verify runs compare them with the C#'s).
    internal static Vector3 LastMove(int i) { return buffer[i].move; }
}

// DAZMorphBank.ApplyMorphsThreadedFast: its two delta loops, deferred to the end of the call and applied natively.
//
// A Harmony transpiler puts Add in front of each loop (skipping the loop when Add returns true), and a Harmony finalizer
// calls EndCall when the method returns or throws. Nothing else in the method reads verts or visibleNonPoseVerts, so
// applying the same additions in the same order at the end gives the same bits.
internal static unsafe class CpuBoostMorph
{
    [StructLayout(LayoutKind.Explicit, Size = 24)]
    internal struct Item
    {
        [FieldOffset(0)] public IntPtr slots;
        [FieldOffset(8)] public int count;
        [FieldOffset(12)] public float w;
        [FieldOffset(16)] public int visible;
    }

    private sealed class Pending
    {
        public DAZMorphVertex[][] deltas = new DAZMorphVertex[32][];
        public float[] w = new float[32];
        public bool[] visible = new bool[32];
        public Item[] items = new Item[32];
        public int n;
        public Vector3[] verts, visibleVerts;
        // Verify run: the loops run as the C# has them, and are recorded so EndCall can apply them natively to the
        // copies taken when the method started, then compare.
        public bool verifying, verifyBroken;
        public Vector3[] verifyVerts, verifyVisible;
    }

    [ThreadStatic] private static Pending pending;
    internal static volatile bool active;
    internal static long deferred, flushes, replays, direct, deltaCount;

    private static Pending Get() { Pending p = pending; if (p == null) pending = p = new Pending(); return p; }

    // Deferred loops waiting for this call's EndCall (not counting a verify run).
    internal static bool HasPending() { Pending p = pending; return p != null && p.n > 0 && !p.verifying; }

    // In place of one delta loop: true when the loop is deferred (and skipped), false for the C# loop to run now, after
    // everything deferred so far has been applied (so the order of additions stays the same).
    internal static bool Add(DAZMorphVertex[] deltas, float w, bool visible, Vector3[] verts, Vector3[] visibleVerts)
    {
        Pending p = pending;
        bool usable = deltas != null && verts != null && (!visible || (visibleVerts != null && visibleVerts.Length >= verts.Length));
        if (p != null && p.verifying)
        {
            if (usable && (p.n == 0 || (ReferenceEquals(p.verts, verts) && ReferenceEquals(p.visibleVerts, visibleVerts))))
                Record(p, deltas, w, visible, verts, visibleVerts);
            else p.verifyBroken = true;
            return false;
        }
        if (!active || !usable)
        {
            if (p != null && p.n > 0) Flush();
            direct++;
            return false;
        }
        if (p == null) p = Get();
        if (p.n > 0 && (!ReferenceEquals(p.verts, verts) || !ReferenceEquals(p.visibleVerts, visibleVerts))) Flush();
        Record(p, deltas, w, visible, verts, visibleVerts);
        deferred++;
        return true;
    }

    private static void Record(Pending p, DAZMorphVertex[] deltas, float w, bool visible, Vector3[] verts, Vector3[] visibleVerts)
    {
        if (p.n == p.deltas.Length)
        {
            int size = p.n * 2;
            var d = new DAZMorphVertex[size][]; Array.Copy(p.deltas, d, p.n); p.deltas = d;
            var ws = new float[size]; Array.Copy(p.w, ws, p.n); p.w = ws;
            var vs = new bool[size]; Array.Copy(p.visible, vs, p.n); p.visible = vs;
            p.items = new Item[size];
        }
        p.deltas[p.n] = deltas; p.w[p.n] = w; p.visible[p.n] = visible; p.n++;
        p.verts = verts; p.visibleVerts = visibleVerts;
    }

    // Verify run for this call: copies of the arrays as they are now.
    internal static void BeginVerify(Vector3[] verts, Vector3[] visibleVerts)
    {
        Pending p = Get();
        if (p.n > 0) Flush();
        p.verifying = true; p.verifyBroken = false;
        p.verifyVerts = verts == null ? null : (Vector3[])verts.Clone();
        p.verifyVisible = visibleVerts == null ? null : (Vector3[])visibleVerts.Clone();
    }

    // When ApplyMorphsThreadedFast returns or throws. Normally applies what was deferred; after a verify run returns the
    // number of vertices where the native result differs from the game's (-1: nothing to compare).
    internal static int EndCall()
    {
        Pending p = pending;
        if (p == null) return -1;
        if (!p.verifying) { Flush(); return -1; }
        try
        {
            if (p.verifyBroken || p.n == 0 || p.verifyVerts == null) return -1;
            if (Apply(p, p.verifyVerts, p.verifyVisible) != 0) return -1;
            int bad = Differ(p.verifyVerts, p.verts);
            if (p.visibleVerts != null) bad += Differ(p.verifyVisible, p.visibleVerts);
            return bad;
        }
        finally
        {
            p.verifying = false; p.verifyVerts = null; p.verifyVisible = null;
            Clear(p);
        }
    }

    private static int Differ(Vector3[] a, Vector3[] b)
    {
        int bad = 0;
        fixed (Vector3* pa = a) fixed (Vector3* pb = b)
        {
            int* x = (int*)pa, y = (int*)pb;
            for (int i = 0, n = a.Length * 3; i < n; i += 3)
                if (x[i] != y[i] || x[i + 1] != y[i + 1] || x[i + 2] != y[i + 2]) bad++;
        }
        return bad;
    }

    private static void Clear(Pending p)
    {
        for (int i = 0; i < p.n; i++) p.deltas[i] = null;
        p.n = 0; p.verts = null; p.visibleVerts = null;
    }

    // Applies what this thread deferred, in order.
    internal static void Flush()
    {
        Pending p = pending;
        if (p == null || p.n == 0) return;
        try
        {
            int r;
            try { r = Apply(p, p.verts, p.visibleVerts); } catch (Exception) { r = -1; }
            flushes++;
            // A delta the C# would throw on (nothing was written): run the C# loops in order, which throw where the
            // game's own loop throws, with the same additions made before it.
            if (r != 0) { replays++; Replay(p); }
        }
        finally { Clear(p); }
    }

    private static int Apply(Pending p, Vector3[] verts, Vector3[] visibleVerts)
    {
        for (int i = 0; i < p.n; i++)
        {
            DAZMorphVertex[] d = p.deltas[i];
            p.items[i].slots = d.Length > 0 ? CpuBoostAddr.Data(d) : IntPtr.Zero;
            p.items[i].count = d.Length;
            p.items[i].w = p.w[i];
            p.items[i].visible = p.visible[i] ? 1 : 0;
            deltaCount += d.Length;
        }
        fixed (Item* it = p.items) fixed (Vector3* v = verts) fixed (Vector3* vis = visibleVerts)
        {
            return CpuBoostNative.MorphApply((IntPtr)it, p.n, (IntPtr)v, verts.Length, (IntPtr)vis, CpuBoostAddr.Len(visibleVerts),
                CpuBoostAddr.MorphVertex, CpuBoostAddr.MorphDelta);
        }
    }

    // The two loops of ApplyMorphsThreadedFast as the game has them.
    private static void Replay(Pending p)
    {
        Vector3[] verts = p.verts, visibleNonPoseVerts = p.visibleVerts;
        int length = verts.Length;
        for (int i = 0; i < p.n; i++)
        {
            DAZMorphVertex[] deltas = p.deltas[i];
            float w = p.w[i];
            if (p.visible[i])
            {
                foreach (DAZMorphVertex d in deltas)
                {
                    if (d.vertex < length)
                    {
                        verts[d.vertex].x += d.delta.x * w;
                        verts[d.vertex].y += d.delta.y * w;
                        verts[d.vertex].z += d.delta.z * w;
                        visibleNonPoseVerts[d.vertex].x += d.delta.x * w;
                        visibleNonPoseVerts[d.vertex].y += d.delta.y * w;
                        visibleNonPoseVerts[d.vertex].z += d.delta.z * w;
                    }
                }
            }
            else
            {
                foreach (DAZMorphVertex d in deltas)
                {
                    if (d.vertex < length)
                    {
                        verts[d.vertex].x += d.delta.x * w;
                        verts[d.vertex].y += d.delta.y * w;
                        verts[d.vertex].z += d.delta.z * w;
                    }
                }
            }
        }
    }
}
