// VaM cloth physics: GPUTools cloth physics on the fused solver (docs\设计.md).
//
// Every physics step of every simulated cloth item goes through ClothPhysicsWorld.DispatchPhysicsImpl
// (ClothSettings.FixedUpdate via FixedDispatch). GPUTools runs it as outer x (iterations x (3 + colour groups) + ~6)
// small dispatches. With this plugin on, a Harmony prefix queues the step for solver\cloth_fused.hlsl instead
// (VaMClothPhysicsNative.dll, on Unity's D3D11 device through GL.IssuePluginEvent): the same particles, point joints,
// skin-wrap targets and colliders, GPUTools' rules and parameters (InitData as GPUTools runs it), 1 + 2 x outer
// dispatches per item and step. The queued steps of all items run together at the end of the physics step
// (WaitForFixedUpdate), the items' dispatches interleaved so the GPU runs them side by side (on D3D11On12 the native
// library flushes between the phases of a solve for that); anything that would touch
// a queued item's inputs or results first (the shared collider update, the cloth's render-frame work, a second step
// of the same item) runs the queue before it. GPUTools' renderer reads the particle buffer as before.
//
// Same result as GPUTools bit for bit (offline replay of four recorded scenes, harness\cloth_harness.cpp, and in game:
// VerifyEverySteps on SimXR). Which items the solver takes is decided per scene (Assign): the solver's items run side
// by side, so a solve takes about as long as its slowest item, while GPUTools' items run one after another. An item
// beyond group-shared memory iterates in one thread group, so with large colour groups it is the solve's critical path
// (a 17k-particle jacket: 2.1 ms per step against 0.9 on GPUTools); since 0.3.0 one whose colour groups average at
// least MultiMinJoints joints spreads them over 4 to 8 thread groups (MultiGroups; same result, the jacket 3x faster),
// and an item still goes back to GPUTools when that makes the whole step shorter. GPUTools also keeps the first 20
// frames after a reset, neighbour collision, items without skin-wrap targets, more than 65536 particles and anything
// that fails (the log says why). Off by default
// (BepInEx\config\local.vam.clothphysics.cfg); nothing changes until it is turned on.
//
// Diagnostics (off by default, for tests on SimXR): the plugin loads even with Enabled = false, times GPUTools' cloth
// steps and the solver's solves on the main thread and on the GPU, can check one step in N per item against GPUTools
// (both start from the same particles; the results are compared), and the VaM test agent can switch the solver at
// runtime: VaMClothPhysics.SetSolver(bool), SetPhases(bool), SetMulti(bool), SetVerify(int), Stats(), ResetStats().
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using GPUTools.Cloth.Scripts.Runtime.Data;
using GPUTools.Cloth.Scripts.Runtime.Kernels;
using GPUTools.Cloth.Scripts.Runtime.Physics;
using GPUTools.Common.Scripts.PL.Tools;
using GPUTools.Physics.Scripts.Kernels;
using GPUTools.Physics.Scripts.Types.Joints;
using HarmonyLib;
using UnityEngine;

[BepInPlugin("local.vam.clothphysics", "VaM Cloth Physics", "0.3.0")]
public sealed class VaMClothPhysics : BaseUnityPlugin
{
    private const string Native = "VaMClothPhysicsNative";
    private const int ApiVersion = 4;
    private const int MaxParticles = 65536;
    private const int MaxShared = 2720;          // MAX_SHARED in cloth_fused.hlsl
    private const int MultiMinJoints = 1800;     // joints per colour group from which an item iterates over several groups
    private const int ActiveFrames = 60;         // an item stepped within this many frames counts for Assign

    // ---- native interface (plugin\native\cloth_physics_native.cpp) -----------

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct CPStep
    {
        public IntPtr particles, pointJoints, cur, old, spheres, lineSpheres, planes, grab;
        public int sphereCount, lineSphereCount, planeCount, grabCount;
        public int iterations, outer, doIntegrate, doCollision;
        public int doStiffness, reserved;
        public float DT, accelX, accelY, accelZ;
        public float invDrag, stiffness, compression, distanceScale;
        public float jointStrength, jointPrediction, breakThreshold, friction;
        public float staticFriction, collisionPower, dtrecipInner, stepInner;
        public float dtrecipOuter, stepOuter;
    }

    private const int OpSnapshot = 0, OpSaveRef = 1, OpRestore = 2, OpCompare = 3;   // diagnostics
    private const int ChannelGpuTools = 0, ChannelSolver = 1;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    // Bound with GetProcAddress: VaM's Mono looks for [DllImport] libraries in VaM_Data\Plugins only.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PtrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int CreateItemFn(int particleCount, IntPtr particles, IntPtr pointJoints,
        IntPtr stiffJoints, int[] stiffGroups, int stiffGroupCount, IntPtr nearJoints, int[] nearGroups, int nearGroupCount, int multiGroups,
        out int addEvent);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int QueueStepFn(int item, ref CPStep step);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ItemFn(int item);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int SetPhasesFn(int on);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ItemCountersFn(int item, out int steps, out int groups, out int shared);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ItemMultiFn(int item, out int groups, out int solves, out int gaveUp);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void SolveCountersFn(out int solves, out int itemSteps, out int dispatches);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)] private delegate int LastErrorFn(StringBuilder buf, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int QueueTimerFn(int channel, int begin, IntPtr anyBuffer);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void GpuTimeFn(int channel, out double sumMs, out double maxMs, out int count, out int skipped, int reset);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int QueueCheckFn(int item, int op, IntPtr particles, IntPtr pointJoints);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ItemVerifyFn(int item, float[] out6, int[] counts3, int reset);

    private static IntFn CP_ApiVersion, CP_StepSize, CP_LostJobs, CP_QueueSolve;
    private static PtrFn CP_GetRenderEventFunc;
    private static CreateItemFn CP_CreateItem;
    private static QueueStepFn CP_QueueStep;
    private static ItemFn CP_QueueRelease, CP_ItemStatus;
    private static SetPhasesFn CP_SetPhases, CP_SetMulti;
    private static ItemCountersFn CP_ItemCounters;
    private static ItemMultiFn CP_ItemMulti;
    private static SolveCountersFn CP_SolveCounters;
    private static LastErrorFn CP_GetLastError;
    private static QueueTimerFn CP_QueueTimer;
    private static GpuTimeFn CP_GpuTime;
    private static QueueCheckFn CP_QueueCheck;
    private static ItemVerifyFn CP_ItemVerify;

    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception(Native + ".dll has no " + name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    private static void BindNative(IntPtr m)
    {
        CP_ApiVersion = Bind<IntFn>(m, "CP_ApiVersion");
        CP_StepSize = Bind<IntFn>(m, "CP_StepSize");
        CP_LostJobs = Bind<IntFn>(m, "CP_LostJobs");
        CP_QueueSolve = Bind<IntFn>(m, "CP_QueueSolve");
        CP_GetRenderEventFunc = Bind<PtrFn>(m, "CP_GetRenderEventFunc");
        CP_CreateItem = Bind<CreateItemFn>(m, "CP_CreateItem");
        CP_QueueStep = Bind<QueueStepFn>(m, "CP_QueueStep");
        CP_QueueRelease = Bind<ItemFn>(m, "CP_QueueRelease");
        CP_ItemStatus = Bind<ItemFn>(m, "CP_ItemStatus");
        CP_SetPhases = Bind<SetPhasesFn>(m, "CP_SetPhases");
        CP_SetMulti = Bind<SetPhasesFn>(m, "CP_SetMulti");
        CP_ItemCounters = Bind<ItemCountersFn>(m, "CP_ItemCounters");
        CP_ItemMulti = Bind<ItemMultiFn>(m, "CP_ItemMulti");
        CP_SolveCounters = Bind<SolveCountersFn>(m, "CP_SolveCounters");
        CP_GetLastError = Bind<LastErrorFn>(m, "CP_GetLastError");
        CP_QueueTimer = Bind<QueueTimerFn>(m, "CP_QueueTimer");
        CP_GpuTime = Bind<GpuTimeFn>(m, "CP_GpuTime");
        CP_QueueCheck = Bind<QueueCheckFn>(m, "CP_QueueCheck");
        CP_ItemVerify = Bind<ItemVerifyFn>(m, "CP_ItemVerify");
    }

    // ---- state -------------------------------------------------------------

    private sealed class Item
    {
        public string name;
        public int id;                       // native item, 0 = none
        public bool failed;                  // GPUTools until the item is rebuilt
        public string why;                   // last reason GPUTools kept it
        // what the native item was built from (rebuilt when any of it changes)
        public ComputeBuffer particles, pointJoints;
        public object stiffData, nearData, stiffGroups, nearGroups;
        public int stiffEntries, nearEntries;
        public IntPtr pParticles, pPointJoints;
        public ComputeBuffer cur, old, spheres, lineSpheres, planes, grab;
        public IntPtr pCur, pOld, pSpheres, pLineSpheres, pPlanes, pGrab;
        public int particleCount, groupEntries, joints;
        public int multiGroups;              // thread groups of its iterate (0 = one)
        public bool multiGaveUp;             // the native library went back to one group
        public bool announced;
        // Assign
        public bool useSolver = true;
        public int estIterations = -1, lastStepFrame;
        public float estSolverMs, estGpuToolsMs;
        public bool queued;                  // a step waits for the next solve
        // diagnostics
        public int sinceVerify;
        public bool comparePending;
        public ComputeBuffer timerBuffer;
        public IntPtr pTimer;
    }

    private static VaMClothPhysics instance;
    private static bool active;              // patched, native library ready
    private static bool takeover;            // the solver runs the steps (Enabled; SetSolver with Diagnostics)
    private static bool diagnostics;
    private static int verifyEvery;          // diagnostics: check one step in N per item against GPUTools (0 = never)
    private static bool multiOn = true;      // multi items iterate over their thread groups (SetMulti with Diagnostics)
    private static bool inOriginal;          // diagnostics: GPUTools' own step is running for a check
    private static Action<ClothPhysicsWorld> dispatchPhysics, initData, updateBuffers;
    private static Func<StiffnessJointsKernel, GroupedData<GPDistanceJoint>> stiffnessJoints;
    private static FieldInfo settingsField;
    private static IntPtr renderEvent;
    private static readonly Dictionary<ClothPhysicsWorld, Item> items = new Dictionary<ClothPhysicsWorld, Item>();
    private static readonly List<Item> queue = new List<Item>();
    private static readonly List<Item> assignItems = new List<Item>();
    private static bool assignDirty = true;
    private static int assignKey;            // ActiveKey at the last Assign
    private const string AssignWhy = "GPUTools is faster in this scene";
    private static CPStep step;
    private static int lostJobsLogged;
    // diagnostics: per channel (0 GPUTools' steps, 1 the solver: queueing and solves) main-thread time
    private static readonly double[] cpuSum = new double[2], cpuMax = new double[2];
    private static readonly int[] cpuCount = new int[2];
    private static int timingDepth, timingChannel, statsFrame;
    private static IntPtr timingBuffer;
    private static long timingStart;
    private static readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();

    private ConfigEntry<bool> enabledSetting;
    private ConfigEntry<bool> diagnosticsSetting;
    private ConfigEntry<int> verifySetting;

    private void Awake()
    {
        instance = this;
        enabledSetting = Config.Bind("General", "Enabled", false,
            "Run GPUTools cloth physics on the fused solver (same result, fewer GPU dispatches). Off: VaM is unchanged. Read at startup.");
        diagnosticsSetting = Config.Bind("Diagnostics", "Diagnostics", false,
            "For tests: load even when Enabled = false, time cloth physics (main thread and GPU) and let the VaM test agent " +
            "switch the solver (VaMClothPhysics.SetSolver / SetVerify / Stats / ResetStats).");
        verifySetting = Config.Bind("Diagnostics", "VerifyEverySteps", 0,
            "With Diagnostics: every N physics steps per item, run GPUTools' step on the same particles too and compare " +
            "(stalls the render thread for the read-back). 0 = never.");
        diagnostics = diagnosticsSetting.Value;
        verifyEvery = diagnostics ? Math.Max(0, verifySetting.Value) : 0;
        if (!enabledSetting.Value && !diagnostics)
        {
            Logger.LogInfo("VaM cloth physics: off (Enabled = false in " + Config.ConfigFilePath + ")");
            return;
        }
        try
        {
            string dir = Path.GetDirectoryName(typeof(VaMClothPhysics).Assembly.Location);
            // .bin, not .dll: BepInEx reads every .dll under plugins as a .NET assembly and logs an error for a native one
            string dll = Path.Combine(dir, Native + ".bin");
            IntPtr module = LoadLibraryW(dll);
            if (module == IntPtr.Zero)
                throw new Exception("cannot load " + dll + " (error " + Marshal.GetLastWin32Error() + ")");
            BindNative(module);
            if (CP_ApiVersion() != ApiVersion || CP_StepSize() != Marshal.SizeOf(typeof(CPStep)))
                throw new Exception("native library version " + CP_ApiVersion() + " / step size " + CP_StepSize() +
                    ", plugin expects " + ApiVersion + " / " + Marshal.SizeOf(typeof(CPStep)));
            renderEvent = CP_GetRenderEventFunc();

            Type world = typeof(ClothPhysicsWorld);
            BindingFlags any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            MethodInfo physics = world.GetMethod("DispatchPhysicsImpl", any, null, Type.EmptyTypes, null);
            MethodInfo init = world.GetMethod("InitData", any, null, Type.EmptyTypes, null);
            MethodInfo update = world.GetMethod("UpdateBuffers", any, null, Type.EmptyTypes, null);
            MethodInfo render = world.GetMethod("DispatchImpl", any, null, Type.EmptyTypes, null);
            MethodInfo copyToOld = world.GetMethod("DispatchCopyToOld", any, null, Type.EmptyTypes, null);
            MethodInfo dispose = world.GetMethod("Dispose", any, null, Type.EmptyTypes, null);
            MethodInfo colliders = typeof(GPUCollidersManager).GetMethod("FixedUpdate", any, null, Type.EmptyTypes, null);
            if (physics == null || init == null || update == null || render == null || copyToOld == null || dispose == null || colliders == null)
                throw new Exception("ClothPhysicsWorld has no DispatchPhysicsImpl/InitData/UpdateBuffers/DispatchImpl/DispatchCopyToOld/Dispose " +
                    "or GPUCollidersManager no FixedUpdate");
            foreach (string f in new string[] { "data", "frame", "iterations", "outerIterations", "usesPreCalcVerts",
                                                "particleNeiborsCollisionKernel", "pointJointsPreCalculatedKernel",
                                                "stiffnessJointsKernel", "nearbyJointsKernel" })
                if (world.GetField(f, any) == null) throw new Exception("ClothPhysicsWorld has no field " + f);
            initData = (Action<ClothPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<ClothPhysicsWorld>), init);
            updateBuffers = (Action<ClothPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<ClothPhysicsWorld>), update);
            dispatchPhysics = (Action<ClothPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<ClothPhysicsWorld>), physics);
            settingsField = typeof(ClothDataFacade).GetField("settings", any);
            PropertyInfo joints = typeof(StiffnessJointsKernel).GetProperty("StiffnessJoints", any);
            MethodInfo jointsGet = joints != null ? joints.GetGetMethod(true) : null;
            if (jointsGet == null || jointsGet.ReturnType != typeof(GroupedData<GPDistanceJoint>))
                throw new Exception("StiffnessJointsKernel has no StiffnessJoints (GroupedData<GPDistanceJoint>)");
            stiffnessJoints = (Func<StiffnessJointsKernel, GroupedData<GPDistanceJoint>>)Delegate.CreateDelegate(
                typeof(Func<StiffnessJointsKernel, GroupedData<GPDistanceJoint>>), jointsGet);

            BindingFlags own = BindingFlags.Static | BindingFlags.NonPublic;
            Harmony harmony = new Harmony("local.vam.clothphysics");
            harmony.Patch(physics, new HarmonyMethod(typeof(VaMClothPhysics).GetMethod("PhysicsPrefix", own)), null, null, null);
            if (diagnostics)
            {
                HarmonyMethod first = new HarmonyMethod(typeof(VaMClothPhysics).GetMethod("TimingPrefix", own));
                first.priority = Priority.First;
                HarmonyMethod last = new HarmonyMethod(typeof(VaMClothPhysics).GetMethod("TimingPostfix", own));
                last.priority = Priority.Last;
                harmony.Patch(physics, first, last, null, null);
            }
            // the queue runs before anything else reads or writes a queued item's inputs or results
            HarmonyMethod flush = new HarmonyMethod(typeof(VaMClothPhysics).GetMethod("FlushPrefix", own));
            flush.priority = Priority.First;
            harmony.Patch(colliders, flush, null, null, null);     // shared colliders of the next physics step
            harmony.Patch(render, flush, null, null, null);        // the render frame's particles
            harmony.Patch(copyToOld, flush, null, null, null);     // the skin-wrap targets
            harmony.Patch(dispose, null, new HarmonyMethod(typeof(VaMClothPhysics).GetMethod("DisposePostfix", own)), null, null);
            StartCoroutine(FlushAtPhysicsStepEnd());
            active = true;
            takeover = enabledSetting.Value;
            Logger.LogInfo("VaM cloth physics: " + (takeover ? "on" : "loaded, solver off") +
                (diagnostics ? ", diagnostics on (verify every " + verifyEvery + " steps)" : "") +
                " (ClothPhysicsWorld.DispatchPhysicsImpl taken over; GPUTools keeps items where it is faster and anything unsupported)");
        }
        catch (Exception e)
        {
            active = false;
            Logger.LogError("VaM cloth physics: unavailable, cloth stays on GPUTools: " + e);
        }
    }

    // ---- patches -------------------------------------------------------------

    private static bool PhysicsPrefix(ClothPhysicsWorld __instance, ClothDataFacade ___data, int ___frame,
        ref int ___iterations, ref int ___outerIterations, bool ___usesPreCalcVerts,
        ParticleNeiborsCollisionKernel ___particleNeiborsCollisionKernel,
        PointJointsPreCalculatedKernel ___pointJointsPreCalculatedKernel,
        StiffnessJointsKernel ___stiffnessJointsKernel, StiffnessJointsKernel ___nearbyJointsKernel)
    {
        if (!active || inOriginal || ___data == null) return true;
        Item it = GetItem(__instance, ___data);
        if (it.queued) Flush();                // a second step before the solve: the first one runs now
        if (!takeover || it.failed || ___frame <= 20) return true;
        try
        {
            string keep = ___particleNeiborsCollisionKernel != null ? "neighbour collision"
                : !___usesPreCalcVerts || ___pointJointsPreCalculatedKernel == null ? "no skin-wrap targets"
                : null;
            if (keep != null) { Keep(it, keep); return true; }
            if (it.id != 0)
            {
                int status = CP_ItemStatus(it.id);
                if (status < 0) { Fail(it, "render thread error " + status + ": " + LastError()); return true; }
                if (status > 0 && !it.multiGaveUp)
                {
                    it.multiGaveUp = true;
                    it.estIterations = -1;
                    instance.Logger.LogWarning("VaM cloth physics: " + it.name + ": " + LastError());
                }
            }
            string why = Prepare(it, __instance, ___stiffnessJointsKernel, ___nearbyJointsKernel);
            if (why != null) { Fail(it, why); return true; }
            it.lastStepFrame = Time.frameCount;
            if (it.estIterations != ___iterations) { Estimate(it, ___iterations); assignDirty = true; }
            if (assignDirty || ActiveKey() != assignKey) Assign();
            if (!it.useSolver) return true;
            initData(__instance);
            updateBuffers(__instance);
            why = RefreshInputs(it, __instance);
            if (why != null) { Fail(it, why); return true; }
            bool check = verifyEvery > 0 && ++it.sinceVerify >= verifyEvery;
            if (check)
            {
                // GPUTools' step and ours from the same particles: snapshot, GPUTools (the original method), keep its
                // result, restore, ours (queued), compare after the solve (CP_ItemVerify has the numbers)
                it.sinceVerify = 0;
                IssueCheck(it, OpSnapshot);
                inOriginal = true;
                try { dispatchPhysics(__instance); }
                finally { inOriginal = false; }
                IssueCheck(it, OpSaveRef);
                IssueCheck(it, OpRestore);
            }
            QueueStep(it, __instance, ___data, ___iterations, ___outerIterations);
            if (check) it.comparePending = true;
            return false;
        }
        catch (Exception e)
        {
            Fail(it, e.ToString());
            return true;
        }
    }

    private static void FlushPrefix()
    {
        if (queue.Count != 0) Flush();
    }

    private IEnumerator FlushAtPhysicsStepEnd()
    {
        WaitForFixedUpdate end = new WaitForFixedUpdate();
        while (true)
        {
            yield return end;
            if (queue.Count != 0) Flush();
        }
    }

    private static void DisposePostfix(ClothPhysicsWorld __instance)
    {
        Item it;
        if (!items.TryGetValue(__instance, out it)) return;
        Release(it);
        items.Remove(__instance);
    }

    // ---- per item --------------------------------------------------------------

    private static Item GetItem(ClothPhysicsWorld w, ClothDataFacade data)
    {
        Item it;
        if (!items.TryGetValue(w, out it))
        {
            it = new Item();
            it.name = ItemName(data);
            items[w] = it;
        }
        return it;
    }

    private static void Keep(Item it, string why)
    {
        if (it.why != why) instance.Logger.LogInfo("VaM cloth physics: " + it.name + ": " + why + ", GPUTools runs it");
        it.why = why;
    }

    private static void Fail(Item it, string why)
    {
        if (!it.failed)
            instance.Logger.LogWarning("VaM cloth physics: " + it.name + " stays on GPUTools: " + why);
        it.failed = true;
        it.why = why;
        Release(it);
    }

    private static void Release(Item it)
    {
        if (it.queued) { queue.Remove(it); it.queued = false; it.comparePending = false; }
        if (it.id != 0)
        {
            int ev = CP_QueueRelease(it.id);
            if (ev >= 0) GL.IssuePluginEvent(renderEvent, ev);
        }
        it.id = 0;
        it.particles = it.pointJoints = it.cur = it.old = it.spheres = it.lineSpheres = it.planes = it.grab = null;
        it.stiffData = it.nearData = it.stiffGroups = it.nearGroups = null;
    }

    // Builds the native item from GPUTools' data, or rebuilds it when that data changed (BuildRuntimeCloth's
    // OnUpdateSettings makes new joints). Returns why the item cannot run, or null.
    private static string Prepare(Item it, ClothPhysicsWorld w, StiffnessJointsKernel sk, StiffnessJointsKernel nk)
    {
        if (w.Particles == null || w.PointJoints == null || w.Particles.ComputeBuffer == null || w.PointJoints.ComputeBuffer == null)
            return "missing physics buffers";
        ComputeBuffer particles = w.Particles.ComputeBuffer, pointJoints = w.PointJoints.ComputeBuffer;
        object sData = sk != null && sk.StiffnessJointsBuffer != null ? (object)sk.StiffnessJointsBuffer.Data : null;
        object nData = nk != null && nk.StiffnessJointsBuffer != null ? (object)nk.StiffnessJointsBuffer.Data : null;
        GroupedData<GPDistanceJoint> sGroups = JointGroups(sk), nGroups = JointGroups(nk);
        int sEntries = sGroups != null ? sGroups.GroupsData.Count : 0;
        int nEntries = nGroups != null ? nGroups.GroupsData.Count : 0;
        if (it.id == 0 || particles != it.particles || pointJoints != it.pointJoints || sData != it.stiffData ||
            nData != it.nearData || sGroups != it.stiffGroups || nGroups != it.nearGroups ||
            sEntries != it.stiffEntries || nEntries != it.nearEntries)
        {
            Release(it);
            int n = particles.count;
            if (n > MaxParticles) return "more than " + MaxParticles + " particles";
            if (pointJoints.count != n) return pointJoints.count + " point joints for " + n + " particles";
            int[] sg, ng;
            int entries = 0, joints = 0;
            sg = Groups(sk, ref entries, ref joints);
            ng = Groups(nk, ref entries, ref joints);
            int multi = MultiGroups(n, entries, joints);
            // GetNativeBufferPtr waits for the render thread: only when a buffer changes.
            it.pParticles = particles.GetNativeBufferPtr();
            it.pPointJoints = pointJoints.GetNativeBufferPtr();
            IntPtr pStiff = sg.Length != 0 ? sk.StiffnessJointsBuffer.ComputeBuffer.GetNativeBufferPtr() : IntPtr.Zero;
            IntPtr pNear = ng.Length != 0 ? nk.StiffnessJointsBuffer.ComputeBuffer.GetNativeBufferPtr() : IntPtr.Zero;
            if (it.pParticles == IntPtr.Zero || it.pPointJoints == IntPtr.Zero) return "no native buffer";
            int ev;
            int id = CP_CreateItem(n, it.pParticles, it.pPointJoints, pStiff, sg, sg.Length / 2, pNear, ng, ng.Length / 2, multi, out ev);
            if (id == 0) return "native item: " + LastError();
            GL.IssuePluginEvent(renderEvent, ev);
            it.id = id;
            it.particles = particles;
            it.pointJoints = pointJoints;
            it.stiffData = sData;
            it.nearData = nData;
            it.stiffGroups = sGroups;
            it.nearGroups = nGroups;
            it.stiffEntries = sEntries;
            it.nearEntries = nEntries;
            it.particleCount = n;
            it.groupEntries = entries;
            it.joints = joints;
            it.multiGroups = multi;
            it.multiGaveUp = false;
            it.estIterations = -1;
            if (!it.announced)
            {
                it.announced = true;
                instance.Logger.LogInfo("VaM cloth physics: " + it.name + ": " + n + " particles, " + entries + " colour groups, " +
                    joints + " joints: solver" + (multi > 0 ? ", iterating over " + multi + " thread groups" : ""));
            }
        }
        return null;
    }

    // GPU ms per physics step, fitted on the recorded scenes (RTX 5090 D, docs\设计.md): the solver iterates an item in
    // one thread group, so its time follows the joints; GPUTools' follows its dispatches (one per colour group and
    // iteration). outer is the same for every item of a scene and scales both alike, so it is left out. A multi item's
    // iterate takes (2.0 + 0.2 j) / (0.5 + 1.55 j) of the one-group time, j = thousands of joints per colour group
    // (harness, per colour group: µs on 4 to 8 groups against µs on one).
    private static void Estimate(Item it, int iterations)
    {
        it.estIterations = iterations;
        float kj = iterations * it.joints / 1000f;
        if (it.particleCount <= MaxShared) it.estSolverMs = 0.215f + 0.0018f * kj;
        else
        {
            float iterate = 0.00424f * kj;
            if (multiOn && it.multiGroups > 0 && !it.multiGaveUp)
            {
                float j = it.joints / (1000f * Math.Max(it.groupEntries, 1));
                iterate *= (2.0f + 0.2f * j) / (0.5f + 1.55f * j);
            }
            it.estSolverMs = 0.21f + iterate;
        }
        it.estGpuToolsMs = 0.29f + 0.00537f * iterations * it.groupEntries + 0.00046f * kj;
    }

    // Thread groups for an item's iterate: in device memory with colour groups of MultiMinJoints joints or more on
    // average, one per 512 joints of a colour group, 4 to 8; 0 = one group. Below that the barrier between colour
    // groups costs more than the spread saves (harness: 1.4k joints per colour group was slower on 4 groups).
    private static int MultiGroups(int particles, int entries, int joints)
    {
        if (particles <= MaxShared || entries == 0) return 0;
        int perGroup = joints / entries;
        return perGroup < MultiMinJoints ? 0 : Math.Min(8, Math.Max(4, (perGroup + 511) / 512));
    }

    private static bool IsActive(Item it, int now)
    {
        return it.id != 0 && now - it.lastStepFrame <= ActiveFrames;
    }

    // Changes when the set of active items does.
    private static int ActiveKey()
    {
        int key = 0, now = Time.frameCount;
        foreach (Item it in items.Values)
            if (IsActive(it, now)) key = unchecked(key * 31 + it.id);
        return key;
    }

    private static readonly Comparison<Item> bySolverMs = (a, b) => b.estSolverMs.CompareTo(a.estSolverMs);

    // The solver's items of a solve run side by side: it takes about as long as its slowest item plus 0.12 of the
    // others (the recorded scenes); GPUTools' items run one after another, each after all the work before it. The items
    // that cost the solver most go to GPUTools as long as that makes the physics step shorter.
    private static void Assign()
    {
        assignDirty = false;
        assignKey = ActiveKey();
        assignItems.Clear();
        int now = Time.frameCount;
        foreach (Item it in items.Values)
            if (IsActive(it, now)) assignItems.Add(it);
        assignItems.Sort(bySolverMs);
        int best = 0;
        float bestMs = float.MaxValue, gpuTools = 0f;
        for (int k = 0; k <= assignItems.Count; k++)
        {
            if (k > 0) gpuTools += assignItems[k - 1].estGpuToolsMs;
            float rest = 0f;
            for (int i = k + 1; i < assignItems.Count; i++) rest += assignItems[i].estSolverMs;
            float ms = gpuTools + (k < assignItems.Count ? assignItems[k].estSolverMs + 0.12f * rest : 0f);
            if (ms < bestMs - 1e-4f) { bestMs = ms; best = k; }
        }
        for (int i = 0; i < assignItems.Count; i++)
        {
            Item it = assignItems[i];
            bool use = i >= best;
            if (use != it.useSolver)
                instance.Logger.LogInfo("VaM cloth physics: " + it.name + ": " + (use ? "solver" : "GPUTools") + " (estimated " +
                    it.estSolverMs.ToString("0.00") + " ms per step on the solver, " + it.estGpuToolsMs.ToString("0.00") + " on GPUTools; " +
                    assignItems.Count + " items in the scene)");
            it.useSolver = use;
            if (!use) it.why = AssignWhy;
            else if (it.why == AssignWhy) it.why = null;
        }
    }

    // The step's inputs as UpdateBuffers left them (it takes the skin-wrap targets and colliders from the data facade).
    private static string RefreshInputs(Item it, ClothPhysicsWorld w)
    {
        it.pCur = Pointer(w.PreCalculatedPositions, ref it.cur, it.pCur);
        it.pOld = Pointer(w.OldPreCalculatedPositions, ref it.old, it.pOld);
        it.pSpheres = Pointer(w.ProcessedSpheres, ref it.spheres, it.pSpheres);
        it.pLineSpheres = Pointer(w.ProcessedLineSpheres, ref it.lineSpheres, it.pLineSpheres);
        it.pPlanes = Pointer(w.Planes, ref it.planes, it.pPlanes);
        it.pGrab = Pointer(w.GrabSpheres, ref it.grab, it.pGrab);
        return it.pCur == IntPtr.Zero || it.pOld == IntPtr.Zero ? "no skin-wrap target buffer" : null;
    }

    // A joint kernel's colour groups as GroupsData lists them, (start, count) pairs; counts the groups GPUTools
    // dispatches (count > 0) and their joints.
    private static int[] Groups(StiffnessJointsKernel k, ref int entries, ref int joints)
    {
        GroupedData<GPDistanceJoint> grouped = JointGroups(k);
        if (grouped == null || k.StiffnessJointsBuffer == null || k.StiffnessJointsBuffer.ComputeBuffer == null)
            return new int[0];
        List<GroupData> g = grouped.GroupsData;
        int[] a = new int[2 * g.Count];
        for (int i = 0; i < g.Count; i++)
        {
            a[2 * i] = g[i].Start;
            a[2 * i + 1] = g[i].Num;
            if (g[i].Num > 0) { entries++; joints += g[i].Num; }
        }
        return a;
    }

    // StiffnessJointsKernel.StiffnessJoints is not public
    private static GroupedData<GPDistanceJoint> JointGroups(StiffnessJointsKernel k)
    {
        return k != null ? stiffnessJoints(k) : null;
    }

    private static IntPtr Pointer<T>(GpuBuffer<T> b, ref ComputeBuffer cached, IntPtr cachedPtr) where T : struct
    {
        ComputeBuffer c = b != null ? b.ComputeBuffer : null;
        if (c == cached) return cachedPtr;
        cached = c;
        return c != null ? c.GetNativeBufferPtr() : IntPtr.Zero;
    }

    // DispatchPhysicsImpl after InitData / UpdateBuffers, frame > 20: its values for this step, and the GpuValues it
    // leaves behind.
    private static void QueueStep(Item it, ClothPhysicsWorld w, ClothDataFacade data, int iterations, int outer)
    {
        float stepDT = Time.fixedDeltaTime * Mathf.Max(data.Weight, 0.01f) / (float)outer;
        float dt = stepDT / (float)iterations;
        Vector3 accel = dt * dt * 0.5f * (data.Gravity + data.Wind);
        step.particles = it.pParticles;
        step.pointJoints = it.pPointJoints;
        step.cur = it.pCur;
        step.old = it.pOld;
        step.spheres = it.pSpheres;
        step.lineSpheres = it.pLineSpheres;
        step.planes = it.pPlanes;
        step.grab = it.pGrab;
        step.sphereCount = it.spheres != null ? it.spheres.count : 0;
        step.lineSphereCount = it.lineSpheres != null ? it.lineSpheres.count : 0;
        step.planeCount = it.planes != null ? it.planes.count : 0;
        step.grabCount = it.grab != null ? it.grab.count : 0;
        step.iterations = iterations;
        step.outer = outer;
        step.doIntegrate = data.IntegrateEnabled ? 1 : 0;
        step.doCollision = data.CollisionEnabled ? 1 : 0;
        step.doStiffness = data.Stiffness > 0f ? 1 : 0;
        step.DT = dt;
        step.accelX = accel.x; step.accelY = accel.y; step.accelZ = accel.z;
        step.invDrag = w.InvDrag.Value;
        step.stiffness = w.Stiffness.Value;
        step.compression = w.CompressionResistance.Value;
        step.distanceScale = w.DistanceScale.Value;
        step.jointStrength = w.JointStrength.Value;
        step.jointPrediction = w.JointPrediction.Value;
        step.breakThreshold = w.BreakThreshold.Value;
        step.friction = w.Friction.Value;
        step.staticFriction = w.StaticFriction.Value;
        step.collisionPower = w.CollisionPower.Value;
        step.dtrecipInner = 1f / dt;
        step.stepInner = 1f / (float)(iterations * outer);
        step.dtrecipOuter = 1f / stepDT;
        step.stepOuter = 1f / (float)outer;
        int ev = CP_QueueStep(it.id, ref step);
        if (ev < 0) throw new Exception("queueing the step failed");
        GL.IssuePluginEvent(renderEvent, ev);
        it.queued = true;
        queue.Add(it);
        // what GPUTools' loop leaves in the GpuValues
        w.T.Value = 1f;
        w.AccelDT2.Value = accel;
        w.Step.Value = 1f / (float)outer;
        w.DT.Value = stepDT;
        w.DTRecip.Value = 1f / stepDT;
        int lost = CP_LostJobs();
        if (lost != lostJobsLogged)
        {
            lostJobsLogged = lost;
            instance.Logger.LogWarning("VaM cloth physics: " + lost + " jobs were dropped before the render thread ran them");
        }
    }

    // Every queued step: one solve job (the items' dispatches interleaved), then the checks waiting for it.
    private static void Flush()
    {
        long t0 = diagnostics ? clock.ElapsedTicks : 0;
        bool timed = diagnostics && queue.Count != 0;
        if (timed) Event(CP_QueueTimer(ChannelSolver, 1, queue[0].pParticles));
        Event(CP_QueueSolve());
        GL.InvalidateState();
        if (timed) Event(CP_QueueTimer(ChannelSolver, 0, queue[0].pParticles));
        for (int i = 0; i < queue.Count; i++)
        {
            Item it = queue[i];
            it.queued = false;
            if (it.comparePending) { it.comparePending = false; IssueCheck(it, OpCompare); }
        }
        queue.Clear();
        if (diagnostics) AddCpu(ChannelSolver, clock.ElapsedTicks - t0);
    }

    private static void Event(int ev)
    {
        if (ev >= 0) GL.IssuePluginEvent(renderEvent, ev);
    }

    private static void IssueCheck(Item it, int op)
    {
        Event(CP_QueueCheck(it.id, op, it.pParticles, it.pPointJoints));
    }

    // ---- diagnostics -------------------------------------------------------------

    // First prefix / last postfix on DispatchPhysicsImpl: main-thread time per item step on the channel that runs it,
    // and a GPU timestamp bracket around GPUTools' own steps (the solver's GPU time is in its solves). Nested calls (a
    // check's GPUTools step) are inside the outer bracket.
    private static void TimingPrefix(ClothPhysicsWorld __instance, ClothDataFacade ___data, int ___frame)
    {
        if (timingDepth++ != 0) return;
        timingBuffer = IntPtr.Zero;
        timingChannel = ChannelGpuTools;
        try
        {
            if (___data == null) return;
            Item it = GetItem(__instance, ___data);
            ComputeBuffer c = __instance.Particles != null ? __instance.Particles.ComputeBuffer : null;
            if (c != it.timerBuffer) { it.timerBuffer = c; it.pTimer = c != null ? c.GetNativeBufferPtr() : IntPtr.Zero; }
            bool solver = active && takeover && !it.failed && ___frame > 20 && it.id != 0 && it.useSolver;
            timingChannel = solver ? ChannelSolver : ChannelGpuTools;
            if (!solver && it.pTimer != IntPtr.Zero)
            {
                timingBuffer = it.pTimer;
                Event(CP_QueueTimer(ChannelGpuTools, 1, timingBuffer));
            }
        }
        catch (Exception) { timingBuffer = IntPtr.Zero; }
        finally { timingStart = clock.ElapsedTicks; }
    }

    private static void TimingPostfix()
    {
        if (--timingDepth != 0) return;
        AddCpu(timingChannel, clock.ElapsedTicks - timingStart);
        if (timingBuffer != IntPtr.Zero) Event(CP_QueueTimer(ChannelGpuTools, 0, timingBuffer));
    }

    private static void AddCpu(int channel, long ticks)
    {
        double ms = ticks * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        cpuSum[channel] += ms;
        cpuCount[channel]++;
        if (ms > cpuMax[channel]) cpuMax[channel] = ms;
    }

    // For the VaM test agent (invoke type=VaMClothPhysics method=...), with Diagnostics on.
    public static bool SetSolver(bool on)
    {
        if (active && diagnostics)
        {
            if (!on && queue.Count != 0) Flush();
            takeover = on;
        }
        return takeover;
    }

    // On D3D11On12 the solve flushes between its phases (so that the items run side by side); off = 0.1.0's solve.
    public static bool SetPhases(bool on)
    {
        if (active && diagnostics) CP_SetPhases(on ? 1 : 0);
        return active && (CP_SetPhases(-1) & 2) != 0;
    }

    // Off: every item iterates in one thread group and Assign estimates it so (0.2.0); same result either way.
    public static bool SetMulti(bool on)
    {
        if (active && diagnostics && on != multiOn)
        {
            if (queue.Count != 0) Flush();
            CP_SetMulti(on ? 1 : 0);
            multiOn = on;
            foreach (Item it in items.Values) it.estIterations = -1;
            assignDirty = true;
        }
        return multiOn;
    }

    public static int SetVerify(int everySteps)
    {
        if (active && diagnostics) verifyEvery = Math.Max(0, everySteps);
        return verifyEvery;
    }

    public static void ResetStats()
    {
        statsFrame = Time.frameCount;
        for (int c = 0; c < 2; c++)
        {
            cpuSum[c] = cpuMax[c] = 0;
            cpuCount[c] = 0;
            if (active)
            {
                double a, b;
                int n, k;
                CP_GpuTime(c, out a, out b, out n, out k, 1);
            }
        }
        if (active)
            foreach (Item it in items.Values)
                if (it.id != 0) CP_ItemVerify(it.id, null, null, 1);
    }

    // JSON: the switches, render frames since ResetStats, per channel the time so far (main thread and GPU, sums for
    // per-frame figures), the solver's counters, per item what the plugin made of it.
    public static string Stats()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        StringBuilder b = new StringBuilder();
        int solves = 0, itemSteps = 0, dispatches = 0;
        if (active) CP_SolveCounters(out solves, out itemSteps, out dispatches);
        int flags = active ? CP_SetPhases(-1) : 0;
        b.Append("{\"active\":").Append(active ? "true" : "false").Append(",\"solver\":").Append(takeover ? "true" : "false")
         .Append(",\"on12\":").Append((flags & 1) != 0 ? "true" : "false").Append(",\"phases\":").Append((flags & 2) != 0 ? "true" : "false")
         .Append(",\"multi\":").Append(multiOn ? "true" : "false")
         .Append(",\"verify_every\":").Append(verifyEvery)
         .Append(",\"frames\":").Append(Time.frameCount - statsFrame)
         .Append(",\"lost_jobs\":").Append(active ? CP_LostJobs() : 0)
         .Append(",\"solves\":").Append(solves).Append(",\"solved_steps\":").Append(itemSteps).Append(",\"dispatches\":").Append(dispatches)
         .Append(",\"channels\":[");
        for (int c = 0; c < 2; c++)
        {
            double gs = 0, gm = 0;
            int gn = 0, gk = 0;
            if (active) CP_GpuTime(c, out gs, out gm, out gn, out gk, 0);
            if (c > 0) b.Append(',');
            b.Append("{\"name\":\"").Append(c == 0 ? "gputools" : "solver").Append("\",\"cpu_calls\":").Append(cpuCount[c])
             .Append(",\"cpu_ms_sum\":").Append(cpuSum[c].ToString("0.000", inv))
             .Append(",\"cpu_ms_max\":").Append(cpuMax[c].ToString("0.0000", inv))
             .Append(",\"gpu_brackets\":").Append(gn).Append(",\"gpu_skipped\":").Append(gk)
             .Append(",\"gpu_ms_sum\":").Append(gs.ToString("0.000", inv))
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
            int steps = 0, groups = 0, shared = 0, multiGroups = 0, multiSolves = 0, gaveUp = 0;
            if (active && it.id != 0)
            {
                CP_ItemCounters(it.id, out steps, out groups, out shared);
                CP_ItemMulti(it.id, out multiGroups, out multiSolves, out gaveUp);
                CP_ItemVerify(it.id, v, k3, 0);
            }
            else { Array.Clear(v, 0, 6); Array.Clear(k3, 0, 3); }
            b.Append("{\"name\":\"").Append(Escape(it.name)).Append("\",\"native\":").Append(it.id != 0 ? "true" : "false")
             .Append(",\"failed\":").Append(it.failed ? "true" : "false")
             .Append(",\"why\":\"").Append(Escape(it.why ?? "")).Append('"')
             .Append(",\"particles\":").Append(it.particleCount).Append(",\"groups\":").Append(it.groupEntries)
             .Append(",\"joints\":").Append(it.joints).Append(",\"shared\":").Append(shared)
             .Append(",\"multi_groups\":").Append(multiGroups).Append(",\"multi_solves\":").Append(multiSolves)
             .Append(",\"multi_gave_up\":").Append(gaveUp)
             .Append(",\"solver_steps\":").Append(steps).Append(",\"use_solver\":").Append(it.useSolver ? "true" : "false")
             .Append(",\"est_solver_ms\":").Append(it.estSolverMs.ToString("0.000", inv))
             .Append(",\"est_gputools_ms\":").Append(it.estGpuToolsMs.ToString("0.000", inv))
             .Append(",\"verify\":{\"checks\":").Append(k3[2])
             .Append(",\"last_max_pos_m\":").Append(v[0].ToString("E3", inv)).Append(",\"last_mean_pos_m\":").Append(v[1].ToString("E3", inv))
             .Append(",\"last_max_vel\":").Append(v[2].ToString("E3", inv)).Append(",\"worst_pos_m\":").Append(v[3].ToString("E3", inv))
             .Append(",\"worst_vel\":").Append(v[4].ToString("E3", inv)).Append(",\"mean_of_means_m\":").Append(v[5].ToString("E3", inv))
             .Append(",\"last_bit_identical\":").Append(k3[0]).Append(",\"non_finite\":").Append(k3[1]).Append("}}");
        }
        b.Append("]}");
        return b.ToString();
    }

    // JSON, per item: how far the particles are from their skin-wrap targets (PreCalculatedPositions), read back from the
    // GPU (waits for it; tests only). A cloth that explodes shows metres here.
    public static string Spread()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        if (active && queue.Count != 0) Flush();
        StringBuilder b = new StringBuilder("{\"solver\":").Append(takeover ? "true" : "false").Append(",\"items\":[");
        bool first = true;
        foreach (KeyValuePair<ClothPhysicsWorld, Item> kv in items)
        {
            ClothPhysicsWorld w = kv.Key;
            Item it = kv.Value;
            ComputeBuffer pc = w.Particles != null ? w.Particles.ComputeBuffer : null;
            ComputeBuffer tc = w.PreCalculatedPositions != null ? w.PreCalculatedPositions.ComputeBuffer : null;
            if (pc == null || tc == null || pc.stride < 12) continue;
            int s = pc.stride / 4, n = Math.Min(pc.count, tc.count), bad = 0;
            float[] p = new float[pc.count * s];
            Vector3[] t = new Vector3[tc.count];
            pc.GetData(p);
            tc.GetData(t);
            float[] d = new float[n];
            for (int i = 0; i < n; i++)
            {
                float x = p[i * s] - t[i].x, y = p[i * s + 1] - t[i].y, z = p[i * s + 2] - t[i].z;
                d[i] = Mathf.Sqrt(x * x + y * y + z * z);
                if (float.IsNaN(d[i]) || float.IsInfinity(d[i])) { bad++; d[i] = 0f; }
            }
            Array.Sort(d);
            int over10 = 0, over50 = 0;
            for (int i = 0; i < n; i++) { if (d[i] > 0.1f) over10++; if (d[i] > 0.5f) over50++; }
            if (!first) b.Append(',');
            first = false;
            b.Append("{\"name\":\"").Append(Escape(it.name)).Append("\",\"on_solver\":")
             .Append(takeover && it.id != 0 && !it.failed && it.useSolver ? "true" : "false")
             .Append(",\"particles\":").Append(n).Append(",\"non_finite\":").Append(bad)
             .Append(",\"p50_m\":").Append((n > 0 ? d[n / 2] : 0f).ToString("0.0000", inv))
             .Append(",\"p99_m\":").Append((n > 0 ? d[(int)(0.99 * (n - 1))] : 0f).ToString("0.0000", inv))
             .Append(",\"max_m\":").Append((n > 0 ? d[n - 1] : 0f).ToString("0.0000", inv))
             .Append(",\"over_10cm\":").Append(over10).Append(",\"over_50cm\":").Append(over50).Append('}');
        }
        return b.Append("]}").ToString();
    }

    private static string Escape(string s)
    {
        return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
    }

    private static string ItemName(ClothDataFacade data)
    {
        try
        {
            Component c = settingsField != null ? settingsField.GetValue(data) as Component : null;
            if (c == null) return "cloth item";
            Transform t = c.transform;
            string s = t.name;
            for (int up = 0; up < 3 && t.parent != null; up++) { t = t.parent; s = t.name + "/" + s; }
            return s;
        }
        catch (Exception) { return "cloth item"; }
    }

    private static string LastError()
    {
        StringBuilder b = new StringBuilder(512);
        CP_GetLastError(b, b.Capacity);
        return b.ToString();
    }

    private void OnDestroy()
    {
        if (!active) return;
        if (queue.Count != 0) Flush();
        foreach (Item it in items.Values) Release(it);
        items.Clear();
    }
}
