// VaM hair solver (P3, mode A): GPUTools hair physics on the fused strand solver.
//
// Every physics step of every hair item goes through HairPhysicsWorld.DispatchPhysicsImpl (FixedUpdate via
// FixedDispatch, or HairBudget's own hair clock). With this plugin on, a Harmony prefix runs that step on
// solver\fused_strand.hlsl instead (VaMHairSolverNative.dll, on Unity's D3D11 device through GL.IssuePluginEvent):
// the same particles, point joints, scalp matrices and colliders, GPUTools' rules and parameters (InitData as GPUTools
// runs it), one to five dispatches per item and step instead of 30-190. GPUTools' renderer reads the particle buffer
// as before. Docs: docs\P0-原求解器与新方案.md section 8.
//
// Exact where the strands do not interact (no cling joints, or cling 0): one dispatch per step. With cling the
// default is cling gathered inside the strand dispatches ("Synchronous", mean difference to GPUTools 0.3-0.6 mm on the
// test hair); "Original" runs GPUTools' own cling kernel between them instead (same result as GPUTools, more dispatches).
// GPUTools sweeps every distance / compression / cling colour group as often as GroupsData lists it (twice after a
// build); the counts are read from the live kernels.
//
// Per item, GPUTools keeps the step (and the plugin says why in the log) for: style mode, joint data
// the solver does not model, more than 256 points per strand, and any error. Off by default
// (BepInEx\config\local.vam.hairsolver.cfg); nothing changes until it is turned on.
//
// Side by side (0.1.3): an item's step is a few dispatches on a few thousand particles, far from filling the GPU, and on
// D3D11On12 every item waited for the one before (the translation layer's barriers wait for all earlier work on NVIDIA):
// GPUTools tessellates each item from its particles right after its step (HairPhysicsWorld.DispatchImpl, in the same
// LateUpdate call with HairBudget's hair clock), and each of the solver's dispatches rewrote one shared DEFAULT constant
// buffer. Now the steps wait in the native library and run together (Solve: all items' first dispatches, then the
// second..., a Flush between them on D3D11On12), and every DispatchImpl waits until the steps before it have run
// (Flush): before a camera renders, before the item's next step, before a skin the item hangs on writes its scalp
// matrices, normals or vertices, before GPUCollidersManager rewrites the colliders, before a rebuild or dispose. Every
// kernel reads what it read before, so the result is the same; only the order of independent items' work changes.
// 0.1.7: a frame with two hair steps (HairBudget following a body that took two) keeps one solve too, each item's second
// step in the phases after its first, instead of a solve at every item's second step (2n + 1 solves in a row).
//
// Diagnostics (off by default, for tests on SimXR): the plugin loads even with Enabled = false, times every hair physics
// step on the main thread and on the GPU (timestamps around GPUTools' own dispatches or around the solver's), can check
// one step in N against GPUTools (both start from the same particles; the results are compared), and the VaM test agent
// can switch the solver at runtime: VaMHairSolver.SetSolver(bool), SetVerify(int), SetCling(bool), SetOverlap(bool)
// (off = 0.1.2's order and constants), SetRepeat(bool) (off = 0.1.6's solves), SetTiming(bool) (the per-step GPU brackets, which make the items wait for each
// other again), Stats(), ResetStats().
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using GPUTools.Common.Scripts.PL.Tools;
using GPUTools.Hair.Scripts;
using GPUTools.Hair.Scripts.Geometry.Create;
using GPUTools.Hair.Scripts.Runtime.Data;
using GPUTools.Hair.Scripts.Runtime.Physics;
using GPUTools.Physics.Scripts.Kernels;
using GPUTools.Skinner.Scripts.Providers;
using HarmonyLib;
using UnityEngine;

[BepInPlugin("local.vam.hairsolver", "VaM Hair Solver", "0.1.7")]
public sealed class VaMHairSolver : BaseUnityPlugin
{
    private const string Native = "VaMHairSolverNative";
    private const int ApiVersion = 5;

    // ---- native interface (plugin\native\hair_solver_native.cpp) -----------

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct HSStep
    {
        public IntPtr particles, pointJoints, transforms, spheres, lineSpheres, planes;
        public int op, flags;
        public float DT, accelX, accelY, accelZ;
        public float invDrag, weight, distanceScale, distancePower;
        public float compressionScale, compressionPower, splinePower, dtrecipInner;
        public float stepInner, dtrecipOuter, stepOuter, friction;
        public float staticFriction, collisionPower;
        public int doIntegrate, doCompression, doCollision, isFixed;
        public float nearbyScale, nearbyPower, nearbyRolloff, clingCap;
        public int outer, iterations, distanceRepeat, compressionRepeat, clingRepeat;
        public int reserved;
        public IntPtr positionOnly;
        public IntPtr bodySpheres;
        public int bodyCount, bodyReserved;
    }

    private const int OpReset = 0, OpStep = 1, OpJacobi = 2, OpStrand = 3;
    private const int OpSnapshot = 4, OpSaveRef = 5, OpRestore = 6, OpCompare = 7;   // diagnostics
    private const int FlagB = 1, FlagEnd = 2, FlagA = 4;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    // Bound with GetProcAddress: VaM's Mono looks for [DllImport] libraries in VaM_Data\Plugins only and does not reuse
    // a module loaded from BepInEx\plugins (DllNotFoundException in the first SimXR run).
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PtrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int CreateItemFn(int particleCount, int segments, float[] rest2, uint[] clingOffsets, HairSolverClingEntry[] clingEntries, int clingEntryCount);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int QueueStepFn(int item, ref HSStep step);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ItemFn(int item);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ItemCountersFn(int item, out int steps, out int dispatches);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)] private delegate int LastErrorFn(StringBuilder buf, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int QueueTimerFn(int channel, int begin, IntPtr anyBuffer);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void GpuTimeFn(int channel, out double sumMs, out double maxMs, out int count, out int skipped, int reset);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ItemVerifyFn(int item, float[] out6, int[] counts3, int reset);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int OptionsFn(int options);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void BatchStatsFn(out int solves, out int steps, out int phases, int reset);

    private static IntFn HS_ApiVersion, HS_StepSize, HS_LostJobs, HS_QueueSolve;
    private static OptionsFn HS_SetOptions;
    private static BatchStatsFn HS_BatchStats;
    private static PtrFn HS_GetRenderEventFunc;
    private static CreateItemFn HS_CreateItem;
    private static QueueStepFn HS_QueueStep;
    private static ItemFn HS_QueueRelease, HS_ItemStatus;
    private static ItemCountersFn HS_ItemCounters;
    private static LastErrorFn HS_GetLastError;
    private static QueueTimerFn HS_QueueTimer;
    private static GpuTimeFn HS_GpuTime;
    private static ItemVerifyFn HS_ItemVerify;

    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception(Native + ".dll has no " + name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    private static void BindNative(IntPtr m)
    {
        HS_ApiVersion = Bind<IntFn>(m, "HS_ApiVersion");
        HS_StepSize = Bind<IntFn>(m, "HS_StepSize");
        HS_LostJobs = Bind<IntFn>(m, "HS_LostJobs");
        HS_GetRenderEventFunc = Bind<PtrFn>(m, "HS_GetRenderEventFunc");
        HS_CreateItem = Bind<CreateItemFn>(m, "HS_CreateItem");
        HS_QueueStep = Bind<QueueStepFn>(m, "HS_QueueStep");
        HS_QueueRelease = Bind<ItemFn>(m, "HS_QueueRelease");
        HS_ItemStatus = Bind<ItemFn>(m, "HS_ItemStatus");
        HS_ItemCounters = Bind<ItemCountersFn>(m, "HS_ItemCounters");
        HS_GetLastError = Bind<LastErrorFn>(m, "HS_GetLastError");
        HS_QueueTimer = Bind<QueueTimerFn>(m, "HS_QueueTimer");
        HS_GpuTime = Bind<GpuTimeFn>(m, "HS_GpuTime");
        HS_ItemVerify = Bind<ItemVerifyFn>(m, "HS_ItemVerify");
        HS_QueueSolve = Bind<IntFn>(m, "HS_QueueSolve");
        HS_SetOptions = Bind<OptionsFn>(m, "HS_SetOptions");
        HS_BatchStats = Bind<BatchStatsFn>(m, "HS_BatchStats");
    }

    // ---- state -------------------------------------------------------------

    private sealed class Item
    {
        public string name;
        public int id;                       // native item, 0 = none
        public bool failed;                  // GPUTools until the item is rebuilt (RebindData)
        public bool announced;
        // what the native item was built from (rebuilt when any of it changes)
        public ComputeBuffer particles, pointJoints;
        public object distanceData, compressionData, nearbyData, distanceGroups, compressionGroups, nearbyGroups;
        public int distanceGroupEntries, compressionGroupEntries, nearbyGroupEntries, segments;
        public IntPtr pParticles, pPointJoints;
        public ComputeBuffer transforms, spheres, lineSpheres, planes;
        public IntPtr pTransforms, pSpheres, pLineSpheres, pPlanes;
        public int planeCount;
        public int particleCount, segmentCount, distanceRepeat, compressionRepeat, clingRepeat, clingJoints;
        public bool hasCompression;
        public string why;                   // last reason GPUTools kept it
        // side by side
        public HairPhysicsWorld world;
        public bool jobPending;              // a step waits for Solve
        public bool deferred;                // GPUTools' DispatchImpl waits for Flush
        public bool skinLooked;
        public object skin;                  // the DAZSkinV2 / DAZSkinWrap its scalp matrices and normals come from (null = unknown)
        // diagnostics
        public int sinceVerify;
        public ComputeBuffer timerBuffer;
        public IntPtr pTimer;
    }

    private static VaMHairSolver instance;
    private static bool active;              // patched, native library ready
    private static bool takeover;            // the solver runs the steps (Enabled; SetSolver with Diagnostics)
    private static bool diagnostics;
    private static int verifyEvery;          // diagnostics: check one step in N per item against GPUTools (0 = never)
    private static bool inOriginal;          // diagnostics: GPUTools' own step is running for a check
    private static Action<HairPhysicsWorld> dispatchPhysics;
    private static bool originalCling;
    private static bool bodyContact = true;
    // diagnostics: per channel (0 GPUTools, 1 solver) main-thread time per item step
    private static readonly double[] cpuSum = new double[2], cpuMax = new double[2];
    private static readonly int[] cpuCount = new int[2];
    private static int timingDepth, timingChannel;
    private static IntPtr timingBuffer;
    private static long timingStart;
    private static readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
    private static IntPtr renderEvent;
    private static Action<HairPhysicsWorld> initData, updateBuffers;
    private static Func<float> hairFixedDeltaTime;   // HairBudget's hair clock, when it is installed
    private static bool hairClockLooked;
    private static FieldInfo settingsField;
    private static readonly Dictionary<HairPhysicsWorld, Item> items = new Dictionary<HairPhysicsWorld, Item>();
    private static HSStep step;
    private static int lostJobsLogged;
    // side by side (header): steps waiting for Solve, render prep waiting for Flush
    private static bool overlap = true;      // diagnostics: SetOverlap(false) = 0.1.2's order and constants
    private static bool repeat = true;       // diagnostics: SetRepeat(false) = 0.1.6's solve at an item's second step
    private static bool flushing;
    private static readonly List<Item> pendingSteps = new List<Item>();
    private static readonly List<Item> deferred = new List<Item>();
    private static Action<HairPhysicsWorld> dispatchImpl;
    private static FieldInfo scalpProviderField;
    private static bool overlapFailureLogged;
    private static int flushes, flushedItems;   // diagnostics
    private static bool timing = true;       // diagnostics: per-step GPU brackets

    private ConfigEntry<bool> enabledSetting;
    private ConfigEntry<string> clingSetting;
    private ConfigEntry<bool> diagnosticsSetting;
    private ConfigEntry<int> verifySetting;

    private void Awake()
    {
        instance = this;
        enabledSetting = Config.Bind("General", "Enabled", false,
            "Run GPUTools hair physics on the fused strand solver. Off: VaM is unchanged. Read at startup.");
        clingSetting = Config.Bind("General", "Cling", "Synchronous",
            "Hair with cling joints: Synchronous (cling inside the solver, 1 + outer x iterations dispatches per step) or " +
            "Original (GPUTools' own cling kernel between the solver dispatches: same result as GPUTools, more dispatches).");
        diagnosticsSetting = Config.Bind("Diagnostics", "Diagnostics", false,
            "For tests: load even when Enabled = false, time every hair physics step (main thread and GPU) and let the " +
            "VaM test agent switch the solver (VaMHairSolver.SetSolver / SetVerify / SetCling / SetOverlap / SetTiming / Stats / ResetStats).");
        verifySetting = Config.Bind("Diagnostics", "VerifyEverySteps", 0,
            "With Diagnostics: every N physics steps per item, run GPUTools' step on the same particles too and compare " +
            "(stalls the render thread for the read-back). 0 = never.");
        diagnostics = diagnosticsSetting.Value;
        verifyEvery = diagnostics ? Math.Max(0, verifySetting.Value) : 0;
        if (!enabledSetting.Value && !diagnostics)
        {
            Logger.LogInfo("VaM hair solver: off (Enabled = false in " + Config.ConfigFilePath + ")");
            return;
        }
        originalCling = string.Equals(clingSetting.Value, "Original", StringComparison.OrdinalIgnoreCase);
        try
        {
            string dir = Path.GetDirectoryName(typeof(VaMHairSolver).Assembly.Location);
            // .bin, not .dll: BepInEx reads every .dll under plugins as a .NET assembly and logs an error for a native one
            string dll = Path.Combine(dir, Native + ".bin");
            IntPtr module = LoadLibraryW(dll);
            if (module == IntPtr.Zero)
                throw new Exception("cannot load " + dll + " (error " + Marshal.GetLastWin32Error() + ")");
            BindNative(module);
            if (HS_ApiVersion() != ApiVersion || HS_StepSize() != Marshal.SizeOf(typeof(HSStep)))
                throw new Exception("native library version " + HS_ApiVersion() + " / step size " + HS_StepSize() +
                    ", plugin expects " + ApiVersion + " / " + Marshal.SizeOf(typeof(HSStep)));
            renderEvent = HS_GetRenderEventFunc();

            Type world = typeof(HairPhysicsWorld);
            BindingFlags any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            MethodInfo physics = world.GetMethod("DispatchPhysicsImpl", any, null, Type.EmptyTypes, null);
            MethodInfo init = world.GetMethod("InitData", any, null, Type.EmptyTypes, null);
            MethodInfo update = world.GetMethod("UpdateBuffers", any, null, Type.EmptyTypes, null);
            MethodInfo rebind = world.GetMethod("RebindData", any, null, Type.EmptyTypes, null);
            MethodInfo dispose = world.GetMethod("Dispose", any, null, Type.EmptyTypes, null);
            if (physics == null || init == null || update == null || rebind == null || dispose == null)
                throw new Exception("HairPhysicsWorld has no DispatchPhysicsImpl/InitData/UpdateBuffers/RebindData/Dispose");
            foreach (string f in new string[] { "data", "frame", "iterations", "outerIterations", "distanceJointsKernel",
                                                "compressionJointsKernel", "nearbyDistanceJointsKernel" })
                if (world.GetField(f, any) == null) throw new Exception("HairPhysicsWorld has no field " + f);
            initData = (Action<HairPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<HairPhysicsWorld>), init);
            dispatchPhysics = (Action<HairPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<HairPhysicsWorld>), physics);
            updateBuffers = (Action<HairPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<HairPhysicsWorld>), update);
            settingsField = typeof(HairDataFacade).GetField("settings", any);

            BindingFlags own = BindingFlags.Static | BindingFlags.NonPublic;
            Harmony harmony = new Harmony("local.vam.hairsolver");
            HairBodyContact.Install(harmony);
            harmony.Patch(physics, new HarmonyMethod(typeof(VaMHairSolver).GetMethod("PhysicsPrefix", own)), null, null, null);
            if (diagnostics)
            {
                HarmonyMethod first = new HarmonyMethod(typeof(VaMHairSolver).GetMethod("TimingPrefix", own));
                first.priority = Priority.First;
                HarmonyMethod last = new HarmonyMethod(typeof(VaMHairSolver).GetMethod("TimingPostfix", own));
                last.priority = Priority.Last;
                harmony.Patch(physics, first, last, null, null);
            }
            HarmonyMethod world0 = new HarmonyMethod(typeof(VaMHairSolver).GetMethod("WorldPrefix", own));
            harmony.Patch(rebind, world0, new HarmonyMethod(typeof(VaMHairSolver).GetMethod("RebindPostfix", own)), null, null);
            harmony.Patch(dispose, world0, new HarmonyMethod(typeof(VaMHairSolver).GetMethod("DisposePostfix", own)), null, null);
            overlap = SetUpOverlap(harmony, any, own);
            HS_SetOptions(overlap ? (repeat ? 7 : 3) : 1);   // dynamic constants either way; batched steps only with the hooks in place
            active = true;
            takeover = enabledSetting.Value;
            Logger.LogInfo("VaM hair solver: " + (takeover ? "on" : "loaded, solver off") + ", cling " + (originalCling ? "Original" : "Synchronous") +
                (overlap ? ", items side by side" : "") + (diagnostics ? ", diagnostics on (verify every " + verifyEvery + " steps)" : "") +
                " (HairPhysicsWorld.DispatchPhysicsImpl taken over; GPUTools keeps style mode and anything unsupported)");
        }
        catch (Exception e)
        {
            active = false;
            Logger.LogError("VaM hair solver: unavailable, hair stays on GPUTools: " + e);
        }
    }

    // ---- patches -------------------------------------------------------------

    private static bool PhysicsPrefix(HairPhysicsWorld __instance, HairDataFacade ___data, ref int ___frame,
        ref int ___iterations, ref int ___outerIterations, DistanceJointsKernel ___distanceJointsKernel,
        CompressionJointsKernel ___compressionJointsKernel, NearbyDistanceJointsKernel ___nearbyDistanceJointsKernel)
    {
        if (!active || inOriginal || ___data == null) return true;
        if(HairBodyContact.PresentationChanged())
        {
            // Submit every reader of the previous sphere buffer before writing
            // it. At most one refresh per newly published skin generation.
            Flush();HairBodyContact.RefreshPresentation();
        }
        Item it;
        items.TryGetValue(__instance, out it);
        if (it != null && it.deferred) Flush();                         // this step rewrites what its render prep reads
        if (!takeover || (it != null && it.failed) || ___data.StyleMode)
        {
            if (it != null && it.jobPending) Solve();                   // GPUTools' step reads the particles
            if (!takeover || (it != null && it.failed)) return true;
        }
        try
        {
            if (it == null) it = GetItem(__instance, ___data);
            if (___data.StyleMode)                                       // GPUTools' own path while styling
            {
                if (it.why != StyleModeWhy) instance.Logger.LogInfo("VaM hair solver: " + it.name + ": style mode, GPUTools runs it");
                it.why = StyleModeWhy;
                return true;
            }
            if (it.why == StyleModeWhy) it.why = null;
            initData(__instance);
            updateBuffers(__instance);
            if (it.id != 0)
            {
                int status = HS_ItemStatus(it.id);
                if (status != 0) { Fail(it, "render thread error " + status + ": " + LastError()); return true; }
            }
            string why = Prepare(it, __instance, ___distanceJointsKernel, ___compressionJointsKernel, ___nearbyDistanceJointsKernel);
            if (why != null) { Fail(it, why); return true; }
            if (!it.skinLooked) { it.skin = SkinOf(___data); it.skinLooked = true; }
            bool check = verifyEvery > 0 && ___frame > 20 && ++it.sinceVerify >= verifyEvery;
            if (check)
            {
                // GPUTools' step and ours from the same particles: snapshot, GPUTools (the original method), keep its
                // result, restore, ours, compare (read back on the render thread; HS_ItemVerify has the numbers)
                it.sinceVerify = 0;
                IssueCheck(it, OpSnapshot);
                inOriginal = true;
                try { dispatchPhysics(__instance); }
                finally { inOriginal = false; }
                IssueCheck(it, OpSaveRef);
                IssueCheck(it, OpRestore);
            }
            RunStep(it, __instance, ___data, ___frame, ___iterations, ___outerIterations, ___nearbyDistanceJointsKernel);
            if (check) IssueCheck(it, OpCompare);
            return false;
        }
        catch (Exception e)
        {
            if (it == null) it = GetItem(__instance, ___data);
            Fail(it, e.ToString());
            return true;
        }
    }

    private static Item GetItem(HairPhysicsWorld w, HairDataFacade data)
    {
        Item it;
        if (!items.TryGetValue(w, out it))
        {
            it = new Item();
            it.name = ItemName(data);
            it.world = w;
            items[w] = it;
        }
        return it;
    }

    private const string StyleModeWhy = "style mode";

    // A rebuilt item (BuildRuntimeHair.RebuildHair) gets a fresh chance and fresh data.
    private static void RebindPostfix(HairPhysicsWorld __instance)
    {
        Item it;
        if (!items.TryGetValue(__instance, out it)) return;
        Release(it);
        it.failed = false;
        it.announced = false;
    }

    private static void DisposePostfix(HairPhysicsWorld __instance)
    {
        Item it;
        if (!items.TryGetValue(__instance, out it)) return;
        Release(it);
        items.Remove(__instance);
    }

    // RebindData / Dispose: the item's waiting work first.
    private static void WorldPrefix(HairPhysicsWorld __instance)
    {
        Item it;
        if (items.TryGetValue(__instance, out it) && (it.jobPending || it.deferred)) Flush();
    }

    // ---- side by side (header) -----------------------------------------------------

    // Side by side needs the render prep to wait and every writer of what it and the waiting steps read hooked; without
    // them the items run one after another as in 0.1.2.
    private bool SetUpOverlap(Harmony harmony, BindingFlags any, BindingFlags own)
    {
        MethodInfo impl = typeof(HairPhysicsWorld).GetMethod("DispatchImpl", any, null, Type.EmptyTypes, null);
        MethodInfo[] skinWrites = {
            typeof(DAZSkinV2).GetMethod("Update", any, null, Type.EmptyTypes, null),
            typeof(DAZSkinV2).GetMethod("LateUpdate", any, null, Type.EmptyTypes, null),
            typeof(DAZSkinWrap).GetMethod("LateUpdate", any, null, Type.EmptyTypes, null),
            typeof(DAZSkinWrap).GetMethod("Dispatch", any, null, Type.EmptyTypes, null) };
        MethodInfo skinDispatch = typeof(DAZSkinV2).GetMethod("Dispatch", any, null, Type.EmptyTypes, null);
        MethodInfo[] colliders = {
            typeof(GPUCollidersManager).GetMethod("FixedUpdate", any, null, Type.EmptyTypes, null),
            typeof(GPUCollidersManager).GetMethod("ComputeColliders", any),
            typeof(GPUCollidersManager).GetMethod("OnDestroy", any, null, Type.EmptyTypes, null) };
        scalpProviderField = typeof(RuntimeHairGeometryCreator).GetField("_ScalpProvider", any);
        if (impl == null || skinDispatch == null || scalpProviderField == null || settingsField == null ||
            Array.IndexOf(skinWrites, null) >= 0 || Array.IndexOf(colliders, null) >= 0 ||
            typeof(DAZSkinV2).GetField("needsDispatch", any) == null)
        {
            Logger.LogWarning("VaM hair solver: hair items run one after another (a method or field for running them side by side is missing)");
            return false;
        }
        dispatchImpl = (Action<HairPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<HairPhysicsWorld>), impl);
        harmony.Patch(impl, new HarmonyMethod(typeof(VaMHairSolver).GetMethod("DeferPrefix", own)), null, null, null);
        HarmonyMethod skin = new HarmonyMethod(typeof(VaMHairSolver).GetMethod("SkinPrefix", own));
        skin.priority = Priority.First;
        foreach (MethodInfo m in skinWrites) harmony.Patch(m, skin, null, null, null);
        HarmonyMethod matrices = new HarmonyMethod(typeof(VaMHairSolver).GetMethod("SkinDispatchPrefix", own));
        matrices.priority = Priority.First;
        harmony.Patch(skinDispatch, matrices, null, null, null);
        HarmonyMethod solve = new HarmonyMethod(typeof(VaMHairSolver).GetMethod("CollidersPrefix", own));
        solve.priority = Priority.First;
        foreach (MethodInfo m in colliders) harmony.Patch(m, solve, null, null, null);   // the shared colliders and body mask
        Camera.onPreCull += OnPreCull;                        // the render frame's tessellated hair
        Application.onBeforeRender += Flush;
        return true;
    }

    // HairPhysicsWorld.DispatchImpl (every frame, after the item's physics steps): GPUTools' render prep, which reads the
    // particles, scalp matrices and normals. It waits for Flush, so the steps of all items are queued together.
    private static bool DeferPrefix(HairPhysicsWorld __instance, HairDataFacade ___data)
    {
        if (!active || !overlap || !takeover || flushing || ___data == null) return true;
        try
        {
            Item it = GetItem(__instance, ___data);
            if (it.deferred) Flush();                 // last frame's has not run (no camera rendered)
            if (!it.skinLooked) { it.skin = SkinOf(___data); it.skinLooked = true; }
            if (it.skin == null) { Flush(); return true; }   // inputs from a provider whose writes are not hooked: in place
            it.deferred = true;
            deferred.Add(it);
            return false;
        }
        catch (Exception e) { OverlapFailed(e); return true; }
    }

    // DAZSkinV2.Update / LateUpdate, DAZSkinWrap.LateUpdate / Dispatch: the skin writes vertices, normals or scalp matrices.
    private static void SkinPrefix(object __instance)
    {
        if (flushing || (pendingSteps.Count == 0 && deferred.Count == 0)) return;
        try { if (Carries(__instance, pendingSteps) || Carries(__instance, deferred)) Flush(); }
        catch (Exception e) { OverlapFailed(e); }
    }

    // DAZSkinV2.Dispatch: the scalp matrices, once a frame (later calls of the frame do nothing).
    private static void SkinDispatchPrefix(object __instance, bool ___needsDispatch)
    {
        if (___needsDispatch) SkinPrefix(__instance);
    }

    private static bool Carries(object skin, List<Item> list)
    {
        for (int i = 0; i < list.Count; i++)
            if (ReferenceEquals(list[i].skin, skin)) return true;
        return false;
    }

    // GPUCollidersManager.FixedUpdate / ComputeColliders / OnDestroy: the colliders the waiting steps read are rewritten
    // (or HairBodyContact's mask is rebuilt or released).
    private static void CollidersPrefix()
    {
        if (pendingSteps.Count != 0) Solve();
    }

    private static void OnPreCull(Camera c)
    {
        Flush();
    }

    // The waiting steps (one solve: the items side by side), then GPUTools' render prep of every waiting item, in the
    // order GPUTools asked for it.
    private static void Flush()
    {
        Solve();
        if (deferred.Count == 0 || flushing) return;
        flushing = true;
        flushes++;
        flushedItems += deferred.Count;
        try
        {
            for (int i = 0; i < deferred.Count; i++)
            {
                Item it = deferred[i];
                it.deferred = false;
                try { dispatchImpl(it.world); }
                catch (Exception e)
                {
                    if (!overlapFailureLogged)
                    {
                        overlapFailureLogged = true;
                        instance.Logger.LogWarning("VaM hair solver: " + it.name + ": GPUTools' render prep failed: " + e);
                    }
                }
            }
        }
        finally
        {
            deferred.Clear();
            flushing = false;
        }
    }

    private static void Solve()
    {
        if (pendingSteps.Count == 0) return;
        for (int i = 0; i < pendingSteps.Count; i++) pendingSteps[i].jobPending = false;
        pendingSteps.Clear();
        int ev = HS_QueueSolve();
        if (ev < 0) return;
        GL.IssuePluginEvent(renderEvent, ev);
        GL.InvalidateState();
    }

    private static void OverlapFailed(Exception e)
    {
        if (!overlapFailureLogged)
        {
            overlapFailureLogged = true;
            instance.Logger.LogWarning("VaM hair solver: items one after another from now on: " + e);
        }
        try { Flush(); }
        catch (Exception) { }
        overlap = false;
        HS_SetOptions(1);
    }

    // The skin a hair item's scalp matrices and normals come from (HairSettings -> RuntimeHairGeometryCreator._ScalpProvider,
    // through PreCalcMeshProviderHolder), or null when it is anything else.
    private static object SkinOf(HairDataFacade data)
    {
        try
        {
            HairSettings s = settingsField.GetValue(data) as HairSettings;
            RuntimeHairGeometryCreator c = s != null && s.StandsSettings != null ? s.StandsSettings.Provider as RuntimeHairGeometryCreator : null;
            PreCalcMeshProvider p = c != null && scalpProviderField != null ? scalpProviderField.GetValue(c) as PreCalcMeshProvider : null;
            PreCalcMeshProviderHolder h = p as PreCalcMeshProviderHolder;
            object skin = h != null ? (object)h.provider : p;
            return skin is DAZSkinV2 || skin is DAZSkinWrap ? skin : null;
        }
        catch (Exception) { return null; }
    }

    // ---- per item --------------------------------------------------------------

    private static void Fail(Item it, string why)
    {
        if (!it.failed)
            instance.Logger.LogWarning("VaM hair solver: " + it.name + " stays on GPUTools: " + why);
        it.failed = true;
        it.why = why;
        Release(it);
    }

    private static void Release(Item it)
    {
        if (pendingSteps.Count != 0) Solve();   // the native release runs them first anyway
        it.skinLooked = false;
        it.skin = null;
        if (it.id != 0)
        {
            int ev = HS_QueueRelease(it.id);
            if (ev >= 0) GL.IssuePluginEvent(renderEvent, ev);
        }
        it.id = 0;
        it.particles = it.pointJoints = it.transforms = it.spheres = it.lineSpheres = it.planes = null;
        it.distanceData = it.compressionData = it.nearbyData = null;
        it.distanceGroups = it.compressionGroups = it.nearbyGroups = null;
    }

    // Builds the native item from GPUTools' data, or refreshes it when that data changed (BuildRuntimeHair rebuilds,
    // HairSimControl.UpdateAllHairSettings pushes new joint lengths). Returns why the item cannot run, or null.
    private static string Prepare(Item it, HairPhysicsWorld w, DistanceJointsKernel dk, CompressionJointsKernel ck,
        NearbyDistanceJointsKernel nk)
    {
        if (w.Particles == null || w.PointJoints == null || w.Transforms == null || w.PointToPreviousPointDistances == null ||
            dk == null || ck == null || nk == null || dk.DistanceJoints == null || ck.CompressionJoints == null || nk.NearbyDistanceJoints == null)
            return "missing physics buffers";
        ComputeBuffer particles = w.Particles.ComputeBuffer, pointJoints = w.PointJoints.ComputeBuffer;
        object dData = dk.DistanceJointsBuffer != null ? (object)dk.DistanceJointsBuffer.Data : null;
        object cData = ck.CompressionJointsBuffer != null ? (object)ck.CompressionJointsBuffer.Data : null;
        object nData = nk.NearbyDistanceJointsBuffer != null ? (object)nk.NearbyDistanceJointsBuffer.Data : null;
        if (it.id == 0 || particles != it.particles || pointJoints != it.pointJoints || dData != it.distanceData ||
            cData != it.compressionData || nData != it.nearbyData || dk.DistanceJoints != it.distanceGroups ||
            ck.CompressionJoints != it.compressionGroups || nk.NearbyDistanceJoints != it.nearbyGroups ||
            dk.DistanceJoints.GroupsData.Count != it.distanceGroupEntries ||
            ck.CompressionJoints.GroupsData.Count != it.compressionGroupEntries ||
            nk.NearbyDistanceJoints.GroupsData.Count != it.nearbyGroupEntries || w.Segments.Value != it.segments)
        {
            Release(it);
            string why = Build(it, w, dk, ck, nk);
            if (why != null) return why;
            it.particles = particles;
            it.pointJoints = pointJoints;
            it.distanceData = dData;
            it.compressionData = cData;
            it.nearbyData = nData;
            it.distanceGroups = dk.DistanceJoints;
            it.compressionGroups = ck.CompressionJoints;
            it.nearbyGroups = nk.NearbyDistanceJoints;
            it.distanceGroupEntries = dk.DistanceJoints.GroupsData.Count;
            it.compressionGroupEntries = ck.CompressionJoints.GroupsData.Count;
            it.nearbyGroupEntries = nk.NearbyDistanceJoints.GroupsData.Count;
            it.segments = w.Segments.Value;
            // GetNativeBufferPtr waits for the render thread: only when a buffer changes.
            it.pParticles = particles.GetNativeBufferPtr();
            it.pPointJoints = pointJoints.GetNativeBufferPtr();
        }
        it.pTransforms = Pointer(w.Transforms, ref it.transforms, it.pTransforms);
        it.pSpheres = Pointer(w.ProcessedSpheres, ref it.spheres, it.pSpheres);
        it.pLineSpheres = Pointer(w.ProcessedLineSpheres, ref it.lineSpheres, it.pLineSpheres);
        it.pPlanes = Pointer(w.Planes, ref it.planes, it.pPlanes);      // plane colliders (null without any)
        it.planeCount = it.planes != null ? it.planes.count : 0;
        if (it.pParticles == IntPtr.Zero || it.pPointJoints == IntPtr.Zero || it.pTransforms == IntPtr.Zero)
            return "no native buffer";
        return null;
    }

    private static IntPtr Pointer<T>(GpuBuffer<T> b, ref ComputeBuffer cached, IntPtr cachedPtr) where T : struct
    {
        ComputeBuffer c = b != null ? b.ComputeBuffer : null;
        if (c == cached) return cachedPtr;
        cached = c;
        return c != null ? c.GetNativeBufferPtr() : IntPtr.Zero;
    }

    private static string Build(Item it, HairPhysicsWorld w, DistanceJointsKernel dk, CompressionJointsKernel ck,
        NearbyDistanceJointsKernel nk)
    {
        int n = w.Particles.Count, seg = w.Segments.Value;
        HairSolverItemData d;
        string why = HairSolverData.Build(n, seg, w.PointJoints.Data, w.PointToPreviousPointDistances.Data,
            dk.DistanceJointsBuffer != null ? dk.DistanceJointsBuffer.Data : null, dk.DistanceJoints,
            ck.CompressionJointsBuffer != null ? ck.CompressionJointsBuffer.Data : null, ck.CompressionJoints,
            nk.NearbyDistanceJointsBuffer != null ? nk.NearbyDistanceJointsBuffer.Data : null, nk.NearbyDistanceJoints, out d);
        if (why != null) return why;
        int id = HS_CreateItem(n, seg, d.rest2, d.clingOffsets, d.clingEntries, d.clingEntryCount);
        if (id == 0) return "native item: " + LastError();
        it.id = id;
        it.particleCount = n;
        it.segmentCount = seg;
        it.distanceRepeat = d.distanceSweeps;
        it.compressionRepeat = d.compressionSweeps;
        it.clingRepeat = d.clingSweeps;
        it.clingJoints = d.clingJoints;
        it.hasCompression = d.hasCompression;
        if (!it.announced)
        {
            it.announced = true;
            instance.Logger.LogInfo("VaM hair solver: " + it.name + ": " + (n / seg) + " strands x " + seg + " points, " +
                d.clingJoints + " cling joints; GPUTools sweeps distance " + d.distanceSweeps + "x, compression " +
                d.compressionSweeps + "x, cling " + d.clingSweeps + "x");
        }
        return null;
    }

    // DispatchPhysicsImpl, non-style path, after InitData / UpdateBuffers.
    private static void RunStep(Item it, HairPhysicsWorld w, HairDataFacade data, int frame, int iterations, int outer,
        NearbyDistanceJointsKernel nk)
    {
        step.particles = it.pParticles;
        step.pointJoints = it.pPointJoints;
        step.transforms = it.pTransforms;
        step.spheres = it.pSpheres;
        step.lineSpheres = it.pLineSpheres;
        step.planes = it.pPlanes;
        step.positionOnly = bodyContact ? HairBodyContact.Buffer : IntPtr.Zero;
        step.reserved = HairBodyContact.SphereCount;
        step.bodySpheres = bodyContact ? HairBodyContact.BodyBuffer : IntPtr.Zero;
        step.bodyCount = bodyContact ? HairBodyContact.BodyCount : 0;
        step.flags = 0;
        if (frame < 10)
        {
            step.op = OpReset;
            Issue(it);
            return;
        }
        float stepDT = FixedDeltaTime() * Mathf.Max(data.Weight, 0.01f) / (float)outer;
        float dt = stepDT / (float)iterations;
        Vector3 accel = dt * dt * 0.5f * (data.Gravity + data.Wind);
        step.DT = dt;
        step.accelX = accel.x; step.accelY = accel.y; step.accelZ = accel.z;
        step.invDrag = w.InvDrag.Value;
        step.weight = w.Weight.Value;
        step.distanceScale = w.DistanceScale.Value;
        step.distancePower = w.DistanceJointPower.Value;
        step.compressionScale = w.CompressionDistanceScale.Value;
        step.compressionPower = w.CompressionJointPower.Value;
        step.splinePower = w.SplineJointPower.Value;
        step.dtrecipInner = 1f / dt;
        step.stepInner = 1f / (float)(iterations * outer);
        step.dtrecipOuter = 1f / stepDT;
        step.stepOuter = 1f / (float)outer;
        step.friction = w.Friction.Value;
        step.staticFriction = w.StaticFriction.Value;
        step.collisionPower = w.CollisionPower.Value;
        step.doIntegrate = frame > 20 ? 1 : 0;
        step.doCompression = it.hasCompression && step.compressionPower != 0f ? 1 : 0;
        step.doCollision = data.IsCollisionEnabled ? 1 : 0;
        step.isFixed = w.IsFixed.Value;
        step.nearbyScale = w.NearbyDistanceScale.Value;
        step.nearbyPower = w.NearbyJointPower.Value;
        step.nearbyRolloff = w.NearbyJointPowerRolloff.Value;
        step.clingCap = 0.5f;
        step.outer = outer;
        step.iterations = iterations;
        step.distanceRepeat = it.distanceRepeat;
        step.compressionRepeat = it.compressionRepeat;
        step.clingRepeat = it.clingRepeat;

        bool cling = it.clingJoints > 0 && step.nearbyPower != 0f;
        if (!cling) { step.op = OpStep; step.nearbyPower = 0f; Issue(it); return; }
        if (!originalCling) { step.op = OpJacobi; Issue(it); return; }
        // GPUTools' cling kernel (its own groups and sweeps) between the strand dispatches, as DispatchPhysicsImpl orders them
        step.op = OpStrand;
        bool pendingB = false, pendingEnd = false;
        for (int o = 0; o < outer; o++)
        {
            for (int i = 0; i < iterations; i++)
            {
                step.flags = FlagA | (pendingB ? FlagB : 0) | (pendingEnd ? FlagEnd : 0);
                Issue(it);
                pendingEnd = false;
                nk.Dispatch();
                pendingB = true;
            }
            pendingEnd = true;
        }
        step.flags = FlagB | FlagEnd;
        Issue(it);
    }

    private static void IssueCheck(Item it, int op)
    {
        step.particles = it.pParticles;
        step.op = op;
        Issue(it, false);
    }

    private static void Issue(Item it)
    {
        Issue(it, true);
    }

    // A step of an item whose inputs' writers are hooked waits for Solve (side by side), its item's later steps of the
    // frame too (the native library puts them in the phases after, or runs the batch first without kOptRepeat); anything
    // else runs the waiting steps first (the native library would too) and then itself.
    private static void Issue(Item it, bool changesState)
    {
        int op = step.op;
        bool batchable = overlap && changesState && (op == OpReset || op == OpStep || op == OpJacobi);
        bool batched = batchable && it.skin != null;
        if (pendingSteps.Count != 0 && !batched) Solve();
        int ev = HS_QueueStep(it.id, ref step);
        if (ev < 0) throw new Exception("queueing the step failed");
        GL.IssuePluginEvent(renderEvent, ev);
        if (batched)
        {
            it.jobPending = true;
            pendingSteps.Add(it);
        }
        else
        {
            if (batchable)
            {
                int solve = HS_QueueSolve();
                if (solve >= 0) GL.IssuePluginEvent(renderEvent, solve);
            }
            if (changesState) GL.InvalidateState();
        }
        int lost = HS_LostJobs();
        if (lost != lostJobsLogged)
        {
            lostJobsLogged = lost;
            instance.Logger.LogWarning("VaM hair solver: " + lost + " physics steps were dropped before the render thread ran them");
        }
    }

    // What DispatchPhysicsImpl reads as Time.fixedDeltaTime: HairBudget's hair step while its hair clock runs.
    private static float FixedDeltaTime()
    {
        if (!hairClockLooked)
        {
            hairClockLooked = true;
            try
            {
                foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
                {
                    if (a.GetName().Name != "VaMHairBudget") continue;
                    Type t = a.GetType("VaMHairBudget");
                    MethodInfo m = t != null ? t.GetMethod("HairFixedDeltaTime", BindingFlags.Public | BindingFlags.Static, null, Type.EmptyTypes, null) : null;
                    if (m != null && m.ReturnType == typeof(float))
                    {
                        hairFixedDeltaTime = (Func<float>)Delegate.CreateDelegate(typeof(Func<float>), m);
                        instance.Logger.LogInfo("VaM hair solver: hair steps follow VaMHairBudget's hair physics clock");
                    }
                }
            }
            catch (Exception e) { instance.Logger.LogWarning("VaM hair solver: VaMHairBudget's hair clock not usable: " + e.Message); }
        }
        return hairFixedDeltaTime != null ? hairFixedDeltaTime() : Time.fixedDeltaTime;
    }

    // ---- diagnostics -------------------------------------------------------------

    // First prefix / last postfix on DispatchPhysicsImpl: main-thread time and a GPU timestamp bracket per item step,
    // on the channel of the solver expected to run it (0 GPUTools, 1 this solver). Nested calls (a check's GPUTools
    // step) are inside the outer bracket.
    private static void TimingPrefix(HairPhysicsWorld __instance, HairDataFacade ___data)
    {
        if (timingDepth++ != 0) return;
        timingBuffer = IntPtr.Zero;
        try
        {
            Item it = GetItem(__instance, ___data);
            timingChannel = takeover && !it.failed && ___data != null && !___data.StyleMode ? 1 : 0;
            if (!timing) return;
            ComputeBuffer c = __instance.Particles != null ? __instance.Particles.ComputeBuffer : null;
            if (c != it.timerBuffer) { it.timerBuffer = c; it.pTimer = c != null ? c.GetNativeBufferPtr() : IntPtr.Zero; }
            timingBuffer = it.pTimer;
            if (timingBuffer != IntPtr.Zero)
            {
                Solve();   // a bracket around one item's step: the items one after another again
                int ev = HS_QueueTimer(timingChannel, 1, timingBuffer);
                if (ev >= 0) GL.IssuePluginEvent(renderEvent, ev);
            }
        }
        catch (Exception) { timingBuffer = IntPtr.Zero; }
        finally { timingStart = clock.ElapsedTicks; }
    }

    private static void TimingPostfix()
    {
        if (--timingDepth != 0) return;
        double ms = (clock.ElapsedTicks - timingStart) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        cpuSum[timingChannel] += ms;
        cpuCount[timingChannel]++;
        if (ms > cpuMax[timingChannel]) cpuMax[timingChannel] = ms;
        if (timingBuffer != IntPtr.Zero)
        {
            Solve();
            int ev = HS_QueueTimer(timingChannel, 0, timingBuffer);
            if (ev >= 0) GL.IssuePluginEvent(renderEvent, ev);
        }
    }

    // For the VaM test agent (invoke type=VaMHairSolver method=...), with Diagnostics on.
    public static bool SetSolver(bool on)
    {
        if (active && diagnostics)
        {
            if (!on) Flush();
            takeover = on;
        }
        return takeover;
    }

    // Off: the items one after another, the constants rewritten with UpdateSubresource (0.1.2).
    public static bool SetOverlap(bool on)
    {
        if (active && diagnostics && (dispatchImpl != null || !on))
        {
            if (!on) Flush();
            overlap = on;
            HS_SetOptions(on ? (repeat ? 7 : 3) : 0);
        }
        return overlap;
    }

    // Off: an item's second step of a frame runs the waiting batch first (0.1.6).
    public static bool SetRepeat(bool on)
    {
        if (active && diagnostics)
        {
            repeat = on;
            if (overlap) HS_SetOptions(on ? 7 : 3);
        }
        return repeat;
    }

    public static bool SetTiming(bool on)
    {
        if (active && diagnostics) timing = on;
        return timing;
    }

    public static int SetVerify(int everySteps)
    {
        if (active && diagnostics) verifyEvery = Math.Max(0, everySteps);
        return verifyEvery;
    }

    public static bool SetCling(bool original)
    {
        if (active && diagnostics) originalCling = original;
        return originalCling;
    }

    public static bool SetBodyContact(bool on)
    {
        if (active && diagnostics) bodyContact = on;
        return bodyContact;
    }

    // off: the hair rests on VaM's breast colliders as in 0.1.3, also where SoftBody gives the drawn breasts
    public static bool SetDrawnBreasts(bool on)
    {
        if (active && diagnostics) HairBodyContact.Drawn = on;
        return HairBodyContact.Drawn;
    }

    public static string BodyContactReport() { return HairBodyContact.Report(); }

    public static void ResetStats()
    {
        for (int c = 0; c < 2; c++)
        {
            cpuSum[c] = cpuMax[c] = 0;
            cpuCount[c] = 0;
            if (active)
            {
                double a, b;
                int n, k;
                HS_GpuTime(c, out a, out b, out n, out k, 1);
            }
        }
        if (active)
        {
            foreach (Item it in items.Values)
                if (it.id != 0) HS_ItemVerify(it.id, null, null, 1);
            int s, n, p;
            HS_BatchStats(out s, out n, out p, 1);
        }
        flushes = flushedItems = 0;
    }

    // JSON: the switches, per channel the steps timed so far (main thread and GPU), per item what the solver made of it.
    public static string Stats()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        StringBuilder b = new StringBuilder();
        b.Append("{\"active\":").Append(active ? "true" : "false").Append(",\"solver\":").Append(takeover ? "true" : "false")
         .Append(",\"cling\":\"").Append(originalCling ? "Original" : "Synchronous").Append("\",\"verify_every\":").Append(verifyEvery)
         .Append(",\"lost_jobs\":").Append(active ? HS_LostJobs() : 0);
        int solves = 0, batched = 0, phases = 0;
        if (active) HS_BatchStats(out solves, out batched, out phases, 0);
        b.Append(",\"overlap\":").Append(overlap ? "true" : "false").Append(",\"repeat\":").Append(repeat ? "true" : "false").Append(",\"timing\":").Append(timing ? "true" : "false")
         .Append(",\"native_options\":").Append(active ? HS_SetOptions(-1) : 0)
         .Append(",\"solves\":").Append(solves).Append(",\"solved_steps\":").Append(batched).Append(",\"solve_phases\":").Append(phases)
         .Append(",\"flushes\":").Append(flushes).Append(",\"flushed_items\":").Append(flushedItems).Append(",\"channels\":[");
        for (int c = 0; c < 2; c++)
        {
            double gs = 0, gm = 0;
            int gn = 0, gk = 0;
            if (active) HS_GpuTime(c, out gs, out gm, out gn, out gk, 0);
            if (c > 0) b.Append(',');
            b.Append("{\"name\":\"").Append(c == 0 ? "gputools" : "solver").Append("\",\"steps\":").Append(cpuCount[c])
             .Append(",\"cpu_ms_mean\":").Append((cpuCount[c] > 0 ? cpuSum[c] / cpuCount[c] : 0).ToString("0.0000", inv))
             .Append(",\"cpu_ms_max\":").Append(cpuMax[c].ToString("0.0000", inv))
             .Append(",\"gpu_steps\":").Append(gn).Append(",\"gpu_skipped\":").Append(gk)
             .Append(",\"gpu_ms_mean\":").Append((gn > 0 ? gs / gn : 0).ToString("0.0000", inv))
             .Append(",\"gpu_ms_max\":").Append(gm.ToString("0.0000", inv)).Append('}');
        }
        b.Append("],\"items\":[");
        bool firstItem = true;
        float[] v = new float[6];
        int[] k3 = new int[3];
        foreach (Item it in items.Values)
        {
            if (!firstItem) b.Append(',');
            firstItem = false;
            int steps = 0, dispatches = 0;
            if (active && it.id != 0) { HS_ItemCounters(it.id, out steps, out dispatches); HS_ItemVerify(it.id, v, k3, 0); }
            else { Array.Clear(v, 0, 6); Array.Clear(k3, 0, 3); }
            b.Append("{\"name\":\"").Append(Escape(it.name)).Append("\",\"native\":").Append(it.id != 0 ? "true" : "false")
             .Append(",\"failed\":").Append(it.failed ? "true" : "false")
             .Append(",\"why\":\"").Append(Escape(it.why ?? "")).Append('"')
             .Append(",\"particles\":").Append(it.particleCount).Append(",\"segments\":").Append(it.segmentCount)
             .Append(",\"cling_joints\":").Append(it.clingJoints).Append(",\"planes\":").Append(it.planeCount)
             .Append(",\"sweeps\":[").Append(it.distanceRepeat).Append(',').Append(it.compressionRepeat).Append(',').Append(it.clingRepeat).Append(']')
             .Append(",\"native_steps\":").Append(steps).Append(",\"native_dispatches\":").Append(dispatches)
             .Append(",\"verify\":{\"checks\":").Append(k3[2])
             .Append(",\"last_max_pos_m\":").Append(v[0].ToString("E3", inv)).Append(",\"last_mean_pos_m\":").Append(v[1].ToString("E3", inv))
             .Append(",\"last_max_vel\":").Append(v[2].ToString("E3", inv)).Append(",\"worst_pos_m\":").Append(v[3].ToString("E3", inv))
             .Append(",\"worst_vel\":").Append(v[4].ToString("E3", inv)).Append(",\"mean_of_means_m\":").Append(v[5].ToString("E3", inv))
             .Append(",\"last_bit_identical\":").Append(k3[0]).Append(",\"non_finite\":").Append(k3[1]).Append("}}");
        }
        b.Append("]}");
        return b.ToString();
    }

    private static string Escape(string s)
    {
        return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
    }

    private static string ItemName(HairDataFacade data)
    {
        try
        {
            Component c = settingsField != null ? settingsField.GetValue(data) as Component : null;
            if (c == null) return "hair item";
            Transform t = c.transform;
            string s = t.name;
            for (int up = 0; up < 3 && t.parent != null; up++) { t = t.parent; s = t.name + "/" + s; }
            return s;
        }
        catch (Exception) { return "hair item"; }
    }

    private static string LastError()
    {
        StringBuilder b = new StringBuilder(512);
        HS_GetLastError(b, b.Capacity);
        return b.ToString();
    }

    private void OnDestroy()
    {
        if (!active) return;
        Camera.onPreCull -= OnPreCull;
        Application.onBeforeRender -= Flush;
        try { Flush(); }
        catch (Exception) { }
        foreach (Item it in items.Values) Release(it);
        items.Clear();
    }
}
