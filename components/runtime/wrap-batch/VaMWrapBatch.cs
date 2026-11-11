// VaM wrap batch: the clothing skin wraps' per-frame GPU work side by side (heavy-character study,
// 2026-11-03; docs/knowledge/clothing.md).
//
// Every clothing item VaM wraps onto the skin runs DAZSkinWrap.LateUpdate each frame: about a dozen small compute
// dispatches that each read the one before (copy last frame's vertices, normals and tangents for the one-frame display
// delay, SkinWrap, the smoothing passes, MapVertices, surface and vertex normals, mapped normals, tangent directions,
// tangents), each on a few thousand vertices. On D3D11On12 every one of them waits for all earlier work (the
// translation layer's barriers wait for idle on NVIDIA): ~3-4 us each with the GPU nearly empty, so a character in many
// clothes pays for the count, not the work (one character's 36 wraps against another's 12: LateUpdate GPU 0.9 -> 2.9 ms).
//
// With this plugin a wrap's LateUpdate still runs on the CPU as VaM wrote it (its checks, the smoothing ping-pong, its
// materials, DrawMeshGPU), but its ComputeShader calls (SetBuffer, SetFloat, Dispatch; a Harmony transpiler on
// DAZSkinWrap.LateUpdate, UpdateVertsGPU and the MeshSmoothGPU / MapVerticesGPU / RecalculateNormalsGPU /
// RecalculateTangentsGPU methods they call) are recorded instead of issued. Before anything reads or rewrites what
// those dispatches read or write, the recorded work runs: the first dispatch of every waiting wrap, then the second,
// and so on, each with the calls that came before it in its own wrap, and on D3D11On12 a Flush of the context between
// these phases (a new command list: the buffers start in COMMON and a phase's dispatches need no barriers), as the cloth
// and hair solvers do. "Before" is: Application.onBeforeRender and any camera's PreCull (the end of the frame's
// LateUpdate work), the LateUpdate / Update / cleanup of a skin a waiting wrap reads (VaM's skinning, and everything
// that rewrites the skin's buffers inside it), DAZSkinWrap.Dispatch / PostProcessDispatch / GPUCleanup of a waiting
// wrap (cloth and hair use a wrap through these). Wraps that provide matrices to hair or cloth and wraps not yet initialised run as
// before. Every dispatch reads and writes what it did before, in the same order within its wrap, and a wrap reads only
// its skin and its own buffers, so the result is the same bit for bit (Verify: every wrap's vertices, normals, tangents
// and delayed copies against VaM's own LateUpdate rerun on the same inputs).
//
// 0.2.0: simulated clothing too. Its render-frame work is GPUTools' ClothSettings.LateUpdate: copy the wrap targets to
// last frame's, the wrap's Dispatch (SkinWrap onto this frame's skin), the cloth's DispatchImpl (GPUTools kernels: draw
// positions from the particles, the vertex-only data) and the wrap's PostProcessDispatch (smoothing, MapVertices,
// normals, tangents, DrawMeshGPU), ~15 dependent dispatches per item. The whole LateUpdate is recorded as one job (GPUTools'
// KernelBase.Dispatch / BindAttributes and the wrap's Dispatch are transpiled too, with SetInt / SetVector / SetBool /
// SetFloats / SetTexture) and runs in the same batch. GPUTools binds every buffer and value again before each dispatch,
// so interleaving items that share a shader changes nothing. Its results are read by the next physics step and by the
// renderer, both after the batch. Not recorded: items not yet initialised, cloth that provides matrices, and cloth with
// accessories (BuildClothAccessories reads the particles back in the middle of the chain). Verify reruns ClothSettings.
// LateUpdate on the same inputs (its frame counter and last-frame targets put back afterwards) and compares the wrap
// targets, the particles, the vertex-only data and the drawn vertices, normals and tangents.
//
// Config BepInEx\config\local.vam.wrapbatch.cfg: Enabled (read at startup). Test agent: VaMWrapBatch.SetEnabled(bool),
// SetCloth(bool), Stats(), ResetStats(), Verify(int frames), VerifyResult().
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using GPUTools.Cloth.Scripts;
using GPUTools.Cloth.Scripts.Runtime.Data;
using GPUTools.Cloth.Scripts.Runtime.Physics;
using GPUTools.Common.Scripts.PL.Abstract;
using GPUTools.Skinner.Scripts.Providers;
using HarmonyLib;
using UnityEngine;

[BepInPlugin("local.vam.wrapbatch", "VaM Wrap Batch", "0.2.1")]
public sealed class VaMWrapBatch : BaseUnityPlugin
{
    private const string Native = "VaMWrapBatchNative";
    private const int ApiVersion = 1;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    // Bound with GetProcAddress: VaM's Mono looks for [DllImport] libraries in VaM_Data\Plugins only.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PtrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn(IntPtr texture);

    private static IntFn wbApiVersion, wbFlushes;
    private static PtrFn wbRenderEventFunc;
    private static InitFn wbInit;

    // ---- recorded calls --------------------------------------------------------

    private const byte OpBuffer = 0, OpFloat = 1, OpDispatch = 2, OpInt = 3, OpVector = 4, OpBool = 5, OpFloats = 6, OpTexture = 7;

    private struct Op
    {
        public byte kind;
        public ComputeShader cs;
        public int kernel, x, y, z;          // x: SetInt's value, SetBool's (0 / 1)
        public string name;
        public ComputeBuffer buffer;
        public float value;
        public Vector4 vector;
        public float[] floats;               // a copy: GPUTools reuses its matrix arrays
        public Texture texture;
    }

    private sealed class Wrap
    {
        public DAZSkinWrap wrap;
        public DAZSkinV2 skin;
        public ClothSettings cloth;          // a simulated item's whole ClothSettings.LateUpdate (null: the wrap's LateUpdate)
        public readonly List<Op> ops = new List<Op>();
        public int dispatches, cursor, done;
        public bool pending;
        // Verify: last frame's results, which the delayed copies must reproduce
        public ComputeBuffer prevVerts, prevNormals, prevTangents;
    }

    private static VaMWrapBatch self;
    private static bool ready;                  // patched, native library loaded
    private static bool batching;
    private static int on12 = -1;
    private static IntPtr renderEvent;
    private static readonly Dictionary<DAZSkinWrap, Wrap> wraps = new Dictionary<DAZSkinWrap, Wrap>();
    private static readonly Dictionary<ClothSettings, Wrap> cloths = new Dictionary<ClothSettings, Wrap>();
    private static readonly List<Wrap> pending = new List<Wrap>();
    private static Wrap recording;
    private static bool replaying, verifyRerun;
    private static bool clothBatching = true;  // SetCloth (tests)
    private static int pendingFrame;
    private static string failure;

    private static FieldInfo fDraw, fProvide, fSkin, fWrapVerts, fDrawVerts, fNormals, fTangents, fDelayedVerts, fDelayedNormals,
        fDelayedTangents, fDelay, fWasInit, fClothWorld, fWorldFrame;
    private static MethodInfo mLateUpdate, mClothLate;

    // stats since ResetStats
    private static int statFrame, statBatches, statWraps, statCloths, statPhases, statDispatches, statFlushBase;
    private static readonly Dictionary<string, int> statWhy = new Dictionary<string, int>();

    // Verify
    private static int verifyFrames;
    private static readonly StringBuilder verifyLog = new StringBuilder();
    private static int verifyWraps, verifyBad, verifyBatches;

    private ConfigEntry<bool> enabledSetting;

    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception(Native + " has no " + name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    private void Awake()
    {
        self = this;
        enabledSetting = Config.Bind("General", "Enabled", true,
            "Run the clothing skin wraps' per-frame GPU work side by side (same result, the GPU waits less). Off: VaM is unchanged. Read at startup.");
        try
        {
            // .bin, not .dll: BepInEx reads every .dll under plugins as a .NET assembly
            string dll = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(typeof(VaMWrapBatch).Assembly.Location), Native + ".bin");
            IntPtr m = LoadLibraryW(dll);
            if (m == IntPtr.Zero) throw new Exception("cannot load " + dll + " (error " + Marshal.GetLastWin32Error() + ")");
            wbApiVersion = Bind<IntFn>(m, "WB_ApiVersion");
            wbFlushes = Bind<IntFn>(m, "WB_Flushes");
            wbRenderEventFunc = Bind<PtrFn>(m, "WB_RenderEventFunc");
            wbInit = Bind<InitFn>(m, "WB_Init");
            if (wbApiVersion() != ApiVersion) throw new Exception("native API " + wbApiVersion() + ", plugin expects " + ApiVersion);
            renderEvent = wbRenderEventFunc();
            on12 = wbInit(Texture2D.whiteTexture.GetNativeTexturePtr());
            if (on12 < 0) throw new Exception("no D3D11 device");
            Patch();
            ready = true;
            batching = enabledSetting.Value;
            Application.onBeforeRender += BeforeRender;
            Camera.onPreCull += PreCull;
            Logger.LogInfo("VaM wrap batch: " + (batching ? "on" : "off (Enabled = false)") +
                (on12 == 1 ? ", D3D11On12: a flush between phases" : ", plain D3D11: no flushes"));
        }
        catch (Exception e)
        {
            failure = e.Message;
            Logger.LogError("VaM wrap batch: " + e.Message + " (VaM's own wrap work runs)");
        }
    }

    private void OnDestroy()
    {
        try { Before("destroy"); } catch (Exception) { }
        Application.onBeforeRender -= BeforeRender;
        Camera.onPreCull -= PreCull;
    }

    // ---- patches ---------------------------------------------------------------

    private static readonly Dictionary<MethodInfo, MethodInfo> replace = new Dictionary<MethodInfo, MethodInfo>();
    private static int replaced;

    private static void Patch()
    {
        BindingFlags own = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
        Type wrap = typeof(DAZSkinWrap), skin = typeof(DAZSkinV2);
        Assembly vam = wrap.Assembly;
        fDraw = Field(wrap, "draw");
        fProvide = Field(wrap, "provideToWorldMatrices");
        fSkin = Field(wrap, "skin");
        fWrapVerts = Field(wrap, "_wrapVerticesBuffer");
        fDrawVerts = Field(wrap, "_drawVerticesBuffer");
        fNormals = Field(wrap, "_normalsBuffer");
        fTangents = Field(wrap, "_tangentsBuffer");
        fDelayedVerts = Field(wrap, "_delayedVertsBuffer");
        fDelayedNormals = Field(wrap, "_delayedNormalsBuffer");
        fDelayedTangents = Field(wrap, "_delayedTangentsBuffer");
        fDelay = Field(skin, "delayDisplayOneFrame");

        Type cs = typeof(ComputeShader);
        replace[Method(cs, "SetBuffer", typeof(int), typeof(string), typeof(ComputeBuffer))] = typeof(VaMWrapBatch).GetMethod("SetBuffer", own);
        replace[Method(cs, "SetFloat", typeof(string), typeof(float))] = typeof(VaMWrapBatch).GetMethod("SetFloat", own);
        replace[Method(cs, "Dispatch", typeof(int), typeof(int), typeof(int), typeof(int))] = typeof(VaMWrapBatch).GetMethod("Dispatch", own);
        replace[Method(cs, "SetInt", typeof(string), typeof(int))] = typeof(VaMWrapBatch).GetMethod("SetInt", own);
        replace[Method(cs, "SetVector", typeof(string), typeof(Vector4))] = typeof(VaMWrapBatch).GetMethod("SetVector", own);
        replace[Method(cs, "SetBool", typeof(string), typeof(bool))] = typeof(VaMWrapBatch).GetMethod("SetBool", own);
        replace[Method(cs, "SetFloats", typeof(string), typeof(float[]))] = typeof(VaMWrapBatch).GetMethod("SetFloats", own);
        replace[Method(cs, "SetTexture", typeof(int), typeof(string), typeof(Texture))] = typeof(VaMWrapBatch).GetMethod("SetTexture", own);

        Type cloth = typeof(ClothSettings), kernel = typeof(KernelBase);
        fWasInit = Field(cloth, "_wasInit");
        fClothWorld = Field(typeof(ClothPhysics), "world");
        fWorldFrame = Field(typeof(ClothPhysicsWorld), "frame");
        mClothLate = Method(cloth, "LateUpdate");

        mLateUpdate = Method(wrap, "LateUpdate");
        List<MethodInfo> recorded = new List<MethodInfo>();
        recorded.Add(mLateUpdate);
        recorded.Add(Method(wrap, "Dispatch"));
        recorded.Add(Method(kernel, "Dispatch"));
        recorded.Add(Method(kernel, "BindAttributes"));
        recorded.Add(Method(wrap, "UpdateVertsGPU", typeof(bool)));
        Type smooth = FindType(vam, "MeshSmoothGPU");
        recorded.Add(Method(smooth, "LaplacianSmoothGPU", typeof(ComputeBuffer), typeof(ComputeBuffer)));
        recorded.Add(Method(smooth, "SpringSmoothGPU", typeof(ComputeBuffer), typeof(ComputeBuffer), typeof(float), typeof(float)));
        recorded.Add(Method(smooth, "SpringSmooth2GPU", typeof(ComputeBuffer), typeof(ComputeBuffer), typeof(float), typeof(float)));
        recorded.Add(Method(smooth, "HCCorrectionGPU", typeof(ComputeBuffer), typeof(ComputeBuffer), typeof(float)));
        recorded.Add(Method(FindType(vam, "MapVerticesGPU"), "Map", typeof(ComputeBuffer)));
        recorded.Add(Method(FindType(vam, "RecalculateNormalsGPU"), "RecalculateNormals", typeof(ComputeBuffer)));
        recorded.Add(Method(FindType(vam, "RecalculateTangentsGPU"), "RecalculateTangents", typeof(ComputeBuffer), typeof(ComputeBuffer)));

        Harmony h = new Harmony("local.vam.wrapbatch");
        HarmonyMethod transpiler = new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("Transpiler", own));
        foreach (MethodInfo m in recorded)
        {
            int before = replaced;
            h.Patch(m, null, null, transpiler, null);
            if (replaced == before) throw new Exception(m.DeclaringType.Name + "." + m.Name + " has no ComputeShader call to record");
        }
        // kernels with their own Dispatch (the joint kernels; none in the render-frame chain, but a recorded call must
        // never run around the recording)
        Type[] types;
        try { types = vam.GetTypes(); }
        catch (ReflectionTypeLoadException e) { types = e.Types; }
        foreach (Type t in types)
        {
            if (t == null || !t.IsSubclassOf(kernel)) continue;
            MethodInfo d = t.GetMethod("Dispatch", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly,
                null, Type.EmptyTypes, null);
            if (d != null && d.GetMethodBody() != null) h.Patch(d, null, null, transpiler, null);
        }
        h.Patch(mLateUpdate, new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("LatePrefix", own)), null, null,
            new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("LateFinalizer", own)));
        h.Patch(mClothLate, new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("ClothPrefix", own)), null, null,
            new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("ClothFinalizer", own)));
        // a physics step reads a cloth job's targets and particles (normally the batch ran before rendering)
        HarmonyMethod physics = new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("PhysicsPrefix", own));
        physics.priority = Priority.First;
        h.Patch(Method(typeof(ClothPhysicsWorld), "DispatchPhysicsImpl"), physics, null, null, null);
        HarmonyMethod consumer = new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("ConsumerPrefix", own));
        consumer.priority = Priority.First;
        h.Patch(Method(wrap, "Dispatch"), consumer, null, null, null);
        h.Patch(Method(wrap, "PostProcessDispatch", typeof(ComputeBuffer)), consumer, null, null, null);
        HarmonyMethod cleanup = new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("CleanupPrefix", own));
        cleanup.priority = Priority.First;
        h.Patch(Method(wrap, "GPUCleanup"), cleanup, null, null, null);
        HarmonyMethod skinWrite = new HarmonyMethod(typeof(VaMWrapBatch).GetMethod("SkinPrefix", own));
        skinWrite.priority = Priority.First;
        foreach (string n in new string[] { "Update", "LateUpdate", "SkinMeshGPUCleanup", "OnDestroy" })
            h.Patch(Method(skin, n), skinWrite, null, null, null);
    }

    private static FieldInfo Field(Type t, string name)
    {
        FieldInfo f = t.GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        if (f == null) throw new MissingFieldException(t.Name, name);
        return f;
    }

    private static Type FindType(Assembly a, string name)
    {
        Type t = a.GetType(name);
        if (t == null) throw new Exception("no type " + name);
        return t;
    }

    private static MethodInfo Method(Type t, string name, params Type[] args)
    {
        MethodInfo m = t.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, args, null);
        if (m == null) throw new MissingMethodException(t.Name, name);
        return m;
    }

    // ComputeShader.SetBuffer / SetFloat / Dispatch -> the static methods below (same stack: the shader, then the arguments)
    private static IEnumerable<CodeInstruction> Transpiler(IEnumerable<CodeInstruction> code)
    {
        foreach (CodeInstruction c in code)
        {
            MethodInfo m = c.operand as MethodInfo, r;
            if (m != null && (c.opcode == OpCodes.Callvirt || c.opcode == OpCodes.Call) && replace.TryGetValue(m, out r))
            {
                c.opcode = OpCodes.Call;
                c.operand = r;
                replaced++;
            }
            yield return c;
        }
    }

    public static void SetBuffer(ComputeShader cs, int kernel, string name, ComputeBuffer buffer)
    {
        if (recording == null) { cs.SetBuffer(kernel, name, buffer); return; }
        Op o = new Op();
        o.kind = OpBuffer; o.cs = cs; o.kernel = kernel; o.name = name; o.buffer = buffer;
        recording.ops.Add(o);
    }

    public static void SetFloat(ComputeShader cs, string name, float value)
    {
        if (recording == null) { cs.SetFloat(name, value); return; }
        Op o = new Op();
        o.kind = OpFloat; o.cs = cs; o.name = name; o.value = value;
        recording.ops.Add(o);
    }

    public static void Dispatch(ComputeShader cs, int kernel, int x, int y, int z)
    {
        if (recording == null) { cs.Dispatch(kernel, x, y, z); return; }
        Op o = new Op();
        o.kind = OpDispatch; o.cs = cs; o.kernel = kernel; o.x = x; o.y = y; o.z = z;
        recording.ops.Add(o);
        recording.dispatches++;
    }

    public static void SetInt(ComputeShader cs, string name, int value)
    {
        if (recording == null) { cs.SetInt(name, value); return; }
        Op o = new Op();
        o.kind = OpInt; o.cs = cs; o.name = name; o.x = value;
        recording.ops.Add(o);
    }

    public static void SetVector(ComputeShader cs, string name, Vector4 value)
    {
        if (recording == null) { cs.SetVector(name, value); return; }
        Op o = new Op();
        o.kind = OpVector; o.cs = cs; o.name = name; o.vector = value;
        recording.ops.Add(o);
    }

    public static void SetBool(ComputeShader cs, string name, bool value)
    {
        if (recording == null) { cs.SetBool(name, value); return; }
        Op o = new Op();
        o.kind = OpBool; o.cs = cs; o.name = name; o.x = value ? 1 : 0;
        recording.ops.Add(o);
    }

    public static void SetFloats(ComputeShader cs, string name, float[] values)
    {
        if (recording == null) { cs.SetFloats(name, values); return; }
        Op o = new Op();
        o.kind = OpFloats; o.cs = cs; o.name = name; o.floats = values != null ? (float[])values.Clone() : null;
        recording.ops.Add(o);
    }

    public static void SetTexture(ComputeShader cs, int kernel, string name, Texture texture)
    {
        if (recording == null) { cs.SetTexture(kernel, name, texture); return; }
        Op o = new Op();
        o.kind = OpTexture; o.cs = cs; o.kernel = kernel; o.name = name; o.texture = texture;
        recording.ops.Add(o);
    }

    private static void Apply(Op o)
    {
        switch (o.kind)
        {
            case OpBuffer: o.cs.SetBuffer(o.kernel, o.name, o.buffer); break;
            case OpFloat: o.cs.SetFloat(o.name, o.value); break;
            case OpInt: o.cs.SetInt(o.name, o.x); break;
            case OpVector: o.cs.SetVector(o.name, o.vector); break;
            case OpBool: o.cs.SetBool(o.name, o.x != 0); break;
            case OpFloats: o.cs.SetFloats(o.name, o.floats); break;
            case OpTexture: o.cs.SetTexture(o.kernel, o.name, o.texture); break;
            default: o.cs.Dispatch(o.kernel, o.x, o.y, o.z); break;
        }
    }

    // ---- recording -----------------------------------------------------------------

    private static void LatePrefix(DAZSkinWrap __instance)
    {
        if (!batching || replaying || verifyRerun || recording != null) return;
        if (pending.Count > 0 && Time.frameCount != pendingFrame) Replay("previous frame");
        try
        {
            // 0.2.1: the public flags read directly (FieldInfo.GetValue boxed each bool, every wrap every frame)
            if (!__instance.draw || __instance.provideToWorldMatrices || fWrapVerts.GetValue(__instance) == null) return;
            DAZSkinV2 skin = fSkin.GetValue(__instance) as DAZSkinV2;
            if (skin == null) return;
            Wrap w;
            if (!wraps.TryGetValue(__instance, out w))
            {
                w = new Wrap();
                w.wrap = __instance;
                wraps[__instance] = w;
            }
            if (w.pending) Replay("wrap again");
            w.skin = skin;
            w.ops.Clear();
            w.dispatches = 0;
            w.cursor = 0;
            w.done = 0;
            if (verifyFrames > 0)
            {
                w.prevVerts = fDrawVerts.GetValue(__instance) as ComputeBuffer;
                w.prevNormals = fNormals.GetValue(__instance) as ComputeBuffer;
                w.prevTangents = fTangents.GetValue(__instance) as ComputeBuffer;
            }
            recording = w;
        }
        catch (Exception e) { Fail(e); }
    }

    // A simulated item's ClothSettings.LateUpdate: its wrap (PreCalcProvider), already initialised, no accessories.
    private static void ClothPrefix(ClothSettings __instance)
    {
        if (!batching || !clothBatching || replaying || verifyRerun || recording != null) return;
        if (pending.Count > 0 && Time.frameCount != pendingFrame) Replay("previous frame");
        try
        {
            if (!(bool)fWasInit.GetValue(__instance)) return;
            MeshProvider mp = __instance.MeshProvider;
            if (mp == null || (int)mp.Type != 2) return;
            DAZSkinWrap d = mp.PreCalcProvider as DAZSkinWrap;
            if (d == null || d.provideToWorldMatrices || fWrapVerts.GetValue(d) == null) return;
            DAZSkinV2 skin = fSkin.GetValue(d) as DAZSkinV2;
            RuntimeData rt = __instance.Runtime;
            if (skin == null || rt == null || rt.OutParticles != null) return;
            Wrap w;
            if (!cloths.TryGetValue(__instance, out w))
            {
                w = new Wrap();
                w.cloth = __instance;
                cloths[__instance] = w;
            }
            Wrap own;
            if (w.pending || (wraps.TryGetValue(d, out own) && own.pending)) Replay("wrap again");
            w.wrap = d;
            w.skin = skin;
            w.ops.Clear();
            w.dispatches = 0;
            w.cursor = 0;
            w.done = 0;
            if (verifyFrames > 0)
            {
                w.prevVerts = fDrawVerts.GetValue(d) as ComputeBuffer;
                w.prevNormals = fNormals.GetValue(d) as ComputeBuffer;
                w.prevTangents = fTangents.GetValue(d) as ComputeBuffer;
            }
            recording = w;
        }
        catch (Exception e) { Fail(e); }
    }

    private static void ClothFinalizer(ClothSettings __instance)
    {
        Wrap w = recording;
        if (w == null || w.cloth != __instance) return;
        recording = null;
        Finish(w);
    }

    // after VaM's LateUpdate, also when it threw (what it issued before that still runs, in its order)
    private static void LateFinalizer(DAZSkinWrap __instance)
    {
        Wrap w = recording;
        if (w == null || w.cloth != null || w.wrap != __instance) return;
        recording = null;
        Finish(w);
    }

    private static void Finish(Wrap w)
    {
        if (w.ops.Count == 0) return;
        if (w.dispatches == 0)
        {
            RunAll(w);
            return;
        }
        w.pending = true;
        pending.Add(w);
        pendingFrame = Time.frameCount;
    }

    // what a wrap recorded so far, now and in order (something needs it before the batch would run)
    private static void RunAll(Wrap w)
    {
        for (int i = w.cursor; i < w.ops.Count; i++) Apply(w.ops[i]);
        w.ops.Clear();
        w.cursor = 0;
        w.dispatches = 0;
        w.done = 0;
    }

    // ---- running the batch -------------------------------------------------------

    private static void Before(string why)
    {
        if (replaying) return;
        if (recording != null)
        {
            Wrap r = recording;
            recording = null;
            try { RunAll(r); }
            finally { recording = r; }
        }
        if (pending.Count > 0) Replay(why);
    }

    private static void BeforeRender() { Before("render"); }

    private static void PreCull(Camera c) { if (pending.Count > 0) Before("camera"); }

    // a wrap's own buffers: its Dispatch / PostProcessDispatch for cloth or hair (not inside the cloth job being
    // recorded, which they are part of), its GPUCleanup
    private static void ConsumerPrefix(DAZSkinWrap __instance)
    {
        if (recording != null && recording.cloth != null && recording.wrap == __instance) return;
        if (Waiting(__instance)) Before("provider");
    }

    private static void CleanupPrefix(DAZSkinWrap __instance) { if (Waiting(__instance)) Before("cleanup"); }

    private static void PhysicsPrefix() { if (pending.Count > 0) Before("physics"); }

    private static bool Waiting(DAZSkinWrap d)
    {
        if (pending.Count == 0 && recording == null) return false;
        if (recording != null && recording.wrap == d) return true;
        for (int i = 0; i < pending.Count; i++)
            if (pending[i].wrap == d) return true;
        return false;
    }

    private static void SkinPrefix(DAZSkinV2 __instance)
    {
        if (pending.Count == 0 && recording == null) return;
        bool reads = recording != null && recording.skin == __instance;
        for (int i = 0; i < pending.Count && !reads; i++) reads = pending[i].skin == __instance;
        if (reads) Before("skin");
    }

    private static void Replay(string why)
    {
        replaying = true;
        bool verify = verifyFrames > 0 && (why == "render" || why == "camera" || why == "skin" || why == "provider");
        List<float[]> before = verify ? VerifyBefore() : null;
        try
        {
            int phases = 0, dispatched = 0;
            while (true)
            {
                bool any = false, more = false;
                for (int i = 0; i < pending.Count; i++)
                {
                    Wrap w = pending[i];
                    if (w.done >= w.dispatches) continue;
                    // this wrap's calls up to and including its next dispatch; after its last one, the calls that follow
                    while (w.cursor < w.ops.Count)
                    {
                        Op o = w.ops[w.cursor++];
                        Apply(o);
                        if (o.kind == OpDispatch) { w.done++; dispatched++; break; }
                    }
                    if (w.done == w.dispatches) while (w.cursor < w.ops.Count) Apply(w.ops[w.cursor++]);
                    any = true;
                    if (w.done < w.dispatches) more = true;
                }
                if (!any) break;
                phases++;
                if (!more) break;
                if (on12 == 1) GL.IssuePluginEvent(renderEvent, 0);
            }
            statBatches++;
            statWraps += pending.Count;
            for (int i = 0; i < pending.Count; i++) if (pending[i].cloth != null) statCloths++;
            statPhases += phases;
            statDispatches += dispatched;
            int n;
            statWhy.TryGetValue(why, out n);
            statWhy[why] = n + 1;
            if (verify) VerifyAfter(before);
        }
        catch (Exception e) { Fail(e); }
        finally
        {
            for (int i = 0; i < pending.Count; i++)
            {
                pending[i].pending = false;
                pending[i].ops.Clear();
            }
            pending.Clear();
            replaying = false;
        }
    }

    private static void Fail(Exception e)
    {
        batching = false;
        failure = e.ToString();
        if (self != null) self.Logger.LogError("VaM wrap batch: stopped for this session, VaM's own wrap work runs: " + e);
    }

    // ---- test agent ----------------------------------------------------------------

    public static string SetEnabled(bool on)
    {
        if (!ready) return "not ready: " + failure;
        if (!on) Before("switch");
        batching = on;
        return "wrap batch " + (on ? "on" : "off");
    }

    // simulated clothing's ClothSettings.LateUpdate in the batch (on) or as VaM runs it (off); the other wraps unchanged
    public static string SetCloth(bool on)
    {
        if (!ready) return "not ready: " + failure;
        if (!on) Before("switch");
        clothBatching = on;
        return "cloth jobs " + (on ? "on" : "off");
    }

    public static void ResetStats()
    {
        statFrame = Time.frameCount;
        statBatches = statWraps = statCloths = statPhases = statDispatches = 0;
        statFlushBase = ready ? wbFlushes() : 0;
        statWhy.Clear();
    }

    public static string Stats()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        int frames = Math.Max(1, Time.frameCount - statFrame);
        StringBuilder b = new StringBuilder();
        b.Append("{\"ready\":").Append(ready ? "true" : "false").Append(",\"batching\":").Append(batching ? "true" : "false")
         .Append(",\"cloth\":").Append(clothBatching ? "true" : "false")
         .Append(",\"on12\":").Append(on12).Append(",\"frames\":").Append(frames)
         .Append(",\"batches_per_frame\":").Append(((float)statBatches / frames).ToString("0.00", inv))
         .Append(",\"wraps_per_frame\":").Append(((float)statWraps / frames).ToString("0.00", inv))
         .Append(",\"cloth_jobs_per_frame\":").Append(((float)statCloths / frames).ToString("0.00", inv))
         .Append(",\"phases_per_frame\":").Append(((float)statPhases / frames).ToString("0.00", inv))
         .Append(",\"dispatches_per_frame\":").Append(((float)statDispatches / frames).ToString("0.00", inv))
         .Append(",\"flushes_per_frame\":").Append(((float)((ready ? wbFlushes() : 0) - statFlushBase) / frames).ToString("0.00", inv))
         .Append(",\"known_wraps\":").Append(wraps.Count).Append(",\"why\":{");
        bool first = true;
        foreach (KeyValuePair<string, int> k in statWhy)
        {
            if (!first) b.Append(',');
            first = false;
            b.Append('"').Append(k.Key).Append("\":").Append(k.Value);
        }
        b.Append("},\"failure\":\"").Append((failure ?? "").Replace("\\", "/").Replace("\"", "'").Replace("\r", " ").Replace("\n", " ")).Append("\"}");
        return b.ToString();
    }

    // The next `frames` batches (before rendering, a camera, a skin's update or a provider) are checked: every wrap's delayed copies against its last
    // frame's results, then VaM's own LateUpdate runs again on the same inputs (not recorded) and its vertices, normals
    // and tangents are compared with the batch's, bit for bit. The delayed copies are put back afterwards; the rerun's
    // DrawMesh calls draw the wraps a second time in those frames (tests only).
    public static string Verify(int frames)
    {
        verifyFrames = frames;
        verifyLog.Length = 0;
        verifyWraps = verifyBad = verifyBatches = 0;
        return "verifying the next " + frames + " batches";
    }

    public static string VerifyResult()
    {
        return "{\"pending_frames\":" + verifyFrames + ",\"batches\":" + verifyBatches + ",\"wraps\":" + verifyWraps + ",\"mismatched\":" + verifyBad +
            ",\"log\":\"" + verifyLog.ToString().Replace("\"", "'") + "\"}";
    }

    private static float[] Read(ComputeBuffer b)
    {
        if (b == null) return null;
        float[] a = new float[b.count * b.stride / 4];
        b.GetData(a);
        return a;
    }

    // per pending wrap: its last frame's vertices, normals, tangents (what the delayed copies must hold afterwards); a
    // cloth job's wrap targets before the batch (what its last-frame targets must hold afterwards)
    private static List<float[]> VerifyBefore()
    {
        List<float[]> r = new List<float[]>();
        foreach (Wrap w in pending)
        {
            r.Add(Read(w.prevVerts));
            r.Add(Read(w.prevNormals));
            r.Add(Read(w.prevTangents));
            r.Add(w.cloth != null ? Read(World(w).PreCalculatedPositions.ComputeBuffer) : null);
        }
        return r;
    }

    private static ClothPhysicsWorld World(Wrap w)
    {
        return fClothWorld.GetValue(w.cloth.GetComponent<ClothPhysics>()) as ClothPhysicsWorld;
    }

    private static int Diff(float[] a, float[] b)
    {
        if (a == null || b == null) return a == b ? 0 : -1;
        if (a.Length != b.Length) return -2;
        int[] ia = new int[a.Length], ib = new int[b.Length];
        Buffer.BlockCopy(a, 0, ia, 0, a.Length * 4);
        Buffer.BlockCopy(b, 0, ib, 0, b.Length * 4);
        int n = 0;
        for (int i = 0; i < ia.Length; i++) if (ia[i] != ib[i]) n++;
        return n;
    }

    private static void VerifyAfter(List<float[]> before)
    {
        verifyFrames--;
        verifyBatches++;
        List<Wrap> list = new List<Wrap>(pending);
        for (int i = 0; i < list.Count; i++)
        {
            Wrap w = list[i];
            DAZSkinWrap d = w.wrap;
            bool delay = (bool)fDelay.GetValue(w.skin) && fDelayedVerts.GetValue(d) != null;
            float[] dv = Read(fDelayedVerts.GetValue(d) as ComputeBuffer), dn = Read(fDelayedNormals.GetValue(d) as ComputeBuffer),
                dt = Read(fDelayedTangents.GetValue(d) as ComputeBuffer);
            float[] av = Read(fDrawVerts.GetValue(d) as ComputeBuffer), an = Read(fNormals.GetValue(d) as ComputeBuffer),
                at = Read(fTangents.GetValue(d) as ComputeBuffer);
            int[] bad = new int[10];
            if (delay)
            {
                bad[0] = Diff(dv, before[4 * i]);
                bad[1] = fNormals.GetValue(d) != null ? Diff(dn, before[4 * i + 1]) : 0;
                bad[2] = fTangents.GetValue(d) != null ? Diff(dt, before[4 * i + 2]) : 0;
            }
            if (w.cloth == null)
            {
                verifyRerun = true;
                try { mLateUpdate.Invoke(d, null); }
                finally { verifyRerun = false; }
            }
            else
            {
                // targets, last-frame targets, particles, vertex-only data after the batch; the rerun copies this
                // frame's targets over last frame's and counts a frame: both put back
                ClothPhysicsWorld world = World(w);
                ComputeBuffer cur = world.PreCalculatedPositions.ComputeBuffer, old = world.OldPreCalculatedPositions.ComputeBuffer;
                ComputeBuffer particles = world.Particles.ComputeBuffer, only = w.cloth.Runtime.ClothOnlyVertices.ComputeBuffer;
                float[] ac = Read(cur), ao = Read(old), ap = Read(particles), ay = Read(only);
                bad[6] = Diff(ao, before[4 * i + 3]);
                object frame = fWorldFrame.GetValue(world);
                verifyRerun = true;
                try { mClothLate.Invoke(w.cloth, null); }
                finally { verifyRerun = false; fWorldFrame.SetValue(world, frame); }
                bad[7] = Diff(ac, Read(cur));
                bad[8] = Diff(ap, Read(particles));
                bad[9] = Diff(ay, Read(only));
                if (ao != null) old.SetData(ao);
            }
            bad[3] = Diff(av, Read(fDrawVerts.GetValue(d) as ComputeBuffer));
            bad[4] = Diff(an, Read(fNormals.GetValue(d) as ComputeBuffer));
            bad[5] = Diff(at, Read(fTangents.GetValue(d) as ComputeBuffer));
            if (delay)
            {
                // the rerun's copies hold this frame: last frame's back
                if (dv != null) (fDelayedVerts.GetValue(d) as ComputeBuffer).SetData(dv);
                if (dn != null) (fDelayedNormals.GetValue(d) as ComputeBuffer).SetData(dn);
                if (dt != null) (fDelayedTangents.GetValue(d) as ComputeBuffer).SetData(dt);
            }
            verifyWraps++;
            bool ok = true;
            foreach (int k in bad) ok &= k == 0;
            if (!ok)
            {
                verifyBad++;
                verifyLog.Append(d.name).Append(" delayed ").Append(bad[0]).Append('/').Append(bad[1]).Append('/').Append(bad[2])
                    .Append(" result ").Append(bad[3]).Append('/').Append(bad[4]).Append('/').Append(bad[5]);
                if (w.cloth != null)
                    verifyLog.Append(" cloth old/targets/particles/vertex-only ").Append(bad[6]).Append('/').Append(bad[7]).Append('/')
                        .Append(bad[8]).Append('/').Append(bad[9]);
                verifyLog.Append("; ");
            }
            else if (verifyBatches == 1)
                verifyLog.Append(d.name).Append(w.cloth != null ? " (cloth)" : "").Append(" ok (").Append(w.dispatches).Append(" dispatches); ");
        }
    }
}
