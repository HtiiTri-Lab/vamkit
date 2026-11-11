// VaM DLSS bridge (BepInEx 5, Unity 2018.1, C# 3).
//
// Dormant unless the DLSS launcher sets VAM_DLSS_BRIDGE=1 AND the running
// openvr_api.dll is the local OpenComposite DLSS build (it exports VaMDLSS_*).
// Per frame, for the main stereo camera only:
//   * sub-pixel Halton jitter on both stereo projections while DLSS is active
//     (the native side reports that through VaMDLSS_GetState),
//   * MSAA off (DLSS replaces it; a multisampled depth buffer cannot be read),
//   * camera matrices + jitter handed to the native side through
//     CommandBuffer.IssuePluginEventAndData at CameraEvent.AfterForwardAlpha,
//     where the scene depth buffer is still bound,
//   * object motion: VaM skins (DAZSkinV2) and clothing (DAZSkinWrap) are
//     GPU-skinned into world-space compute buffers and drawn with
//     Graphics.DrawMesh, so Unity produces no motion vectors for them.
//     Harmony postfixes record which of them drew this frame; their vertex and
//     index buffers go to the native side, which redraws them with this and
//     the previous frame's positions.
//   * hair motion: VaM physics hair (GPUTools HairRender) builds its strands on
//     the GPU from a buffer of tessellated guide-strand points. Visible hair
//     items hand that buffer, its valid point count and the points per strand
//     to the native side, which turns the guides into motion ribbons. Hair
//     items are known from a Harmony postfix on HairRender.Awake (plus one
//     search when hair motion starts), never by searching the scene per frame
//     or on a timer.
//   * rigid motion (2.7.0): props, CUAs and toys are MeshRenderers that Unity
//     gives no motion vectors here either. Their renderers come from VaM's own
//     per-atom lists (Atom._meshRenderers / _dynamicMeshRenderers, re-read when
//     Harmony postfixes on Atom.Init, (De)RegisterDynamicMeshRenderer,
//     AddAtom or RemoveAtom fire). Each frame every one's localToWorld is read;
//     the visible ones that moved go to the native side with their mesh's
//     vertex and index buffers and this and the previous frame's matrix.
//     Cloth keeps camera motion.
//   * skinned motion (2.8.0): animated CUAs (a folding fan in a hand, dancers)
//     are Unity SkinnedMeshRenderers, which VaM does not list. A Harmony
//     postfix on CustomUnityAssetLoader.DoLoadedCallbacks (both its prefab and
//     its scene loads end there) collects the ones under each loaded CUA. Each
//     frame every bone's skinning matrix (bone localToWorld x bind pose) is
//     read; for the visible ones whose bones moved, the native side gets them
//     with the previous frame's and the mesh's bind pose and bone weights,
//     read once per mesh (readable meshes only), and skins the mesh twice.
//   * render scale: [Performance] RenderScale is live in this build. The
//     bridge renders at it by setting Unity's eye texture scale to VaM's own
//     render scale times VaMDLSS_GetEyeTextureScale, which also follows the
//     live OutputScale (2.6.0). Native frames - no jitter, flagged so the
//     native side sends them out without DLSS SR - are rendered at 100% of
//     the output with DLSSPreset=Off (the native side's StateSrOff) and while
//     VaM's own menus are open (main HUD, start screen or scene browser;
//     DLSSMenuNative). 100% with a model is DLAA: jittered, through DLSS.
// 2.5.0 removed menu UI motion and the nasolabial band (no use to the user);
// with the hair search they ran FindObjectsOfType every 2 s, which in a scene
// with many objects (a large stage scene) made two ~95 ms frames every 2 s. Their
// FrameExtension fields stay zero.
// 2.7.1: hair points per strand = TessSegments and the valid points = guide
// strands x TessSegments (2.7.0 joined Segments guides tip to root, as RtCore
// found in SkinTransmission 0.22.1).
// 2.8.0: skinned motion (above); a stage scene's hand fans moved with camera
// motion only, so DLSS-G's generated frames left them where the real frame
// had them. Only with a native side that sets StateSkinnedMotion: an older
// one would take a skinned draw for a skin.
// 2.8.1: rigid and skinned motion only for submeshes Unity draws with a
// material in the opaque queues (renderQueue <= 2500, the ones Unity itself
// gives motion vectors), and not for shadow-only renderers. Transparent ones
// (Standard Fade/Transparent glows, additive beams, glass) write no depth, so
// the native side matched them against the surface behind them: where one
// passed within the depth tolerance in front of a face, its motion took the
// face's pixels (faces deformed now and then in front of stage effects).
// 2.8.2: a skinned renderer out of view for CacheFrames frames came back with
// its mesh's pinned arrays already freed by the sweep: "Handle is not pinned",
// and skinned motion stopped for the rest of the session (the stage's
// fans kept camera motion from the first section change on). Such a renderer
// is now bound again when it comes back.
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using BepInEx;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR;
using UnityEngine.UI;

[BepInPlugin("local.vam.dlssbridge", "VaM DLSS Bridge", "2.9.19")]
public sealed class VaMDLSSBridge : BaseUnityPlugin
{
    private static readonly bool Desktop = Environment.GetEnvironmentVariable("VAM_DISPLAY_MODE") == "Desktop";
    private static int ViewCount { get { return Desktop ? 1 : 2; } }
    private RenderTexture desktopTarget, previousTarget;
    private Camera desktopOutput;
    private CommandBuffer desktopPresent;
    private bool previousAllowMsaa;
    private static FieldInfo desktopCenterField, desktopCameraField, desktopUiField;
    private int EyeWidth { get { return Desktop && desktopTarget != null ? desktopTarget.width : XRSettings.eyeTextureWidth; } }
    private int EyeHeight { get { return Desktop && desktopTarget != null ? desktopTarget.height : XRSettings.eyeTextureHeight; } }
    private const int CaptureEvent = 0x56440001;
    private const int ApiVersion = 3;
    private const int FrameSize = 1040;     // must match FrameInfoV2 in dx11_vam_dlss.cpp
    private const int FrameVersion = 3;
    private const int SlotCount = 4;        // render thread lags the main thread by <= 2 frames
    private const int DrawStride = 64;      // must match ObjectDraw in dx11_vam_dlss.cpp
    private const int MaxDraws = 2048;
    private const int MaxRecordedCalls = 4096;
    private const int CacheFrames = 300;    // native keeps buffers referenced for 600 frames
    private const uint FrameMagic = 0x534C4456;
    private const int StateSelected = 1, StateFailed = 4, StateObjectMotion = 16, StateHairMotion = 32, StateMenuNative = 128,
                      StateSrOff = 512, StateRigidMotion = 1024, StateSkinnedMotion = 2048,StatePathRendering=32768;
    private const uint FlagNative = 4, FlagMenuResized = 8; // FrameInfo flags, see FrameInfo in dx11_vam_dlss.cpp
    private const float ScaleSettleSeconds = 0.5f;          // a render scale slider being dragged applies once it rests
    private const int NativeTailFrames = 90;
    private const int HairStride = 56;      // GPUTools TessRenderParticle
    private const uint DrawHair = 2;        // ObjectDraw.flags, see kDrawHair in dx11_vam_dlss.cpp
    private const uint DrawRigid = 8;       // ObjectDraw.flags, see kDrawRigid in dx11_vam_dlss.cpp
    private const uint DrawSkinned = 16;    // ObjectDraw.flags, see kDrawSkinned in dx11_vam_dlss.cpp
    private const int MaxRigidMatrices = 4096; // a pair per rigid renderer, one per bone of a skinned one
    private const int RigidMatrixStride = 128; // this and the previous frame's localToWorld, row-major
    private const int OpaqueQueueLast = 2500;  // RenderQueue.GeometryLast: Geometry and AlphaTest write depth
    private const int SeeThroughLogs = 24;     // renderers named in the log when first skipped
    // 2.9.7 (11-06, user: 作者在角色上挂了很多透明部件，比如麦克风……这可能也是角色脸部变形的原因): a clothing part its
    // author hid with Alpha Adjust -1 on a see-through shader draws nothing, yet VaM still draws it (transparent queue,
    // depth written): it is no longer redrawn for motion, so it cannot take a face's pixels.
    // 2.9.8 (11-06, user: 头发矢量没有了): only VaM's clothing family Custom/Subsurface/*SeparateAlpha*, whose alpha is
    // saturate(_AlphaTex.a + _AlphaAdjust) (disassembly, docs\sheer-cloth-20261105). 2.9.7 took any material in the
    // see-through queue with the property: hair shaders (Custom/Hair/*) and opaque shaders an author put in that queue
    // keep a stray Alpha Adjust and still draw - their motion went.
    private static readonly int AlphaAdjustId = Shader.PropertyToID("_AlphaAdjust");
    private static readonly Dictionary<int, bool> hideableShaders = new Dictionary<int, bool>();
    private static readonly Dictionary<int, bool> loggedHidden = new Dictionary<int, bool>();
    private static bool Hidden(Material m)
    {
        if (m.renderQueue < 2450) return false;
        Shader shader = m.shader;
        if (shader == null) return false;
        int sid = shader.GetInstanceID();
        bool hideable;
        if (!hideableShaders.TryGetValue(sid, out hideable))
        {
            string name = shader.name;
            hideable = name.StartsWith("Custom/Subsurface/", StringComparison.Ordinal) && name.IndexOf("SeparateAlpha", StringComparison.Ordinal) >= 0;
            hideableShaders[sid] = hideable;
        }
        if (!hideable || !m.HasProperty(AlphaAdjustId) || m.GetFloat(AlphaAdjustId) > -0.999f) return false;
        int mid = m.GetInstanceID();
        if (!loggedHidden.ContainsKey(mid) && loggedHidden.Count < SeeThroughLogs)
        {
            loggedHidden[mid] = true;
            Debug.Log("VaM DLSS bridge: hidden clothing part (Alpha Adjust -1) not redrawn for motion: " + m.name + " / " + shader.name);
        }
        return true;
    }
    // 2.9.6 (11-05): the raster route captures after the transparents again, and opaqueDepthCommand hands OC the depth
    // from before them (OpaqueDepthEvent). 2.9.5 captured only before them (a see-through thing has no business
    // deciding the depth and motion of what is behind it): one in front of a person no longer took the person's depth
    // and motion, but VaM's see-through clothing (transparent queue, writes depth) lost its own too, and clothing has
    // to keep its depth and motion whatever it is made of. OC now uses the depth before the transparents for the
    // guides, and matches a person's skin, clothing and hair against either.
    private CameraEvent CaptureCameraEvent = CameraEvent.AfterForwardAlpha;
    private const int OpaqueDepthEvent = 5;   // kOpaqueDepthEvent in dx11_vam_dlss.cpp
    private bool pathCaptureOrdered;
    private CommandBuffer latePathCommand, opaqueDepthCommand;

    [DllImport("openvr_api")] private static extern int VaMDLSS_GetApiVersion();
    [DllImport("openvr_api")] private static extern IntPtr VaMDLSS_GetRenderEventFunc();
    [DllImport("openvr_api")] private static extern int VaMDLSS_GetState();
    [DllImport("openvr_api")] private static extern int VaMDLSS_GetJitterPhaseCount();
    [DllImport("openvr_api")] private static extern int VaMDLSS_GetRenderScalePercent();
    [DllImport("openvr_api")] private static extern float VaMDLSS_GetEyeTextureScale(int percent);
    [DllImport("openvr_api")] private static extern int VaMDLSS_GetDesktopSize(out int width, out int height);
    [DllImport("openvr_api", CharSet=CharSet.Unicode)] private static extern int VaMDLSS_CaptureDesktop(string path);
    public static string CaptureDesktop(string path) { return VaMDLSS_CaptureDesktop(path) != 0 ? "queued" : "busy"; }
    // A test capture of desktop frame generation: the next generated frame as <path>, with the real frame it follows
    // (<path>.real.ppm) and what was presented (<path>.presented.ppm).
    [DllImport("openvr_api", CharSet=CharSet.Unicode)] private static extern int VaMDLSS_CaptureGenerated(string path);
    public static string CaptureGenerated(string path)
    {
        try { return VaMDLSS_CaptureGenerated(path) != 0 ? "queued" : "busy"; }
        catch (EntryPointNotFoundException) { return "unavailable"; }
    }

    private IntPtr slots = IntPtr.Zero, drawSlots = IntPtr.Zero, matrixSlots = IntPtr.Zero;
    private IntPtr renderEvent = IntPtr.Zero;
    private bool nativeReady, nativeAbsent;
    private float nextNativeProbe;
    private int probeAttempts;

    private Camera target;
    private CommandBuffer command;
    private float nextCameraScan;

    private int originalMsaa = -1;
    private bool msaaLogged;

    private bool jitteredThisFrame;
    private float jitterX, jitterY, previousJitterX, previousJitterY;
    private int jitterIndex;
    private int lastPreCullFrame = -1, lastPreRenderFrame = -1;

    private readonly Matrix4x4[] projection = new Matrix4x4[2];
    private readonly Matrix4x4[] rasterProjection = new Matrix4x4[2];
    private readonly Matrix4x4[] view = new Matrix4x4[2];
    private readonly Matrix4x4[] previousProjection = new Matrix4x4[2];
    private readonly Matrix4x4[] previousView = new Matrix4x4[2];
    private bool havePrevious;
    private int previousFrame = -1, previousEyeWidth, previousEyeHeight;

    // ---- object motion -------------------------------------------------
    private static readonly List<Component> skinCalls = new List<Component>(64);
    private static readonly List<Component> wrapCalls = new List<Component>(64);
    private static readonly List<bool> wrapNoDelay = new List<bool>(64);
    private static bool collecting;

    private sealed class SkinAccess
    {
        public FieldInfo renderSuspend, staticDraw, mesh, delayDisplay, useSmoothing, raw, smoothed, delayed;
        public FieldInfo drawOffset, materialsEnabled, useSimple, simpleMaterial, gpuMaterials;
    }
    private sealed class WrapAccess
    {
        public FieldInfo renderSuspend, staticDraw, mesh, skin, delayed, drawVertices;
        public FieldInfo materialsEnabled, useSimple, simpleMaterial, gpuMaterials;
    }
    private sealed class BufferEntry
    {
        public IntPtr pointer;
        public int stride, count, lastUsed;
    }
    private sealed class MeshEntry
    {
        public IntPtr indexBuffer, vertexBuffer;
        public bool index32;
        public int subMeshCount, vertexCount, lastUsed;
        public uint[] start, count;
        public int[] baseVertex;
        public float retryAt;
    }

    private Harmony harmony;
    private bool motionPatched, motionUnavailable, motionLogged;
    private SkinAccess skinAccess;
    private WrapAccess wrapAccess;
    private readonly Dictionary<ComputeBuffer, BufferEntry> buffers = new Dictionary<ComputeBuffer, BufferEntry>();
    private readonly Dictionary<Mesh, MeshEntry> meshes = new Dictionary<Mesh, MeshEntry>();
    private readonly Dictionary<int, int> drawnThisFrame = new Dictionary<int, int>();
    private readonly List<ComputeBuffer> staleBuffers = new List<ComputeBuffer>();
    private readonly List<Mesh> staleMeshes = new List<Mesh>();
    private int lastCacheSweep;

    // ---- hair motion ---------------------------------------------------
    private sealed class HairAccess
    {
        public Type render, physics;
        public FieldInfo rend, data, world;            // HairRender.rend/.data, GPHairPhysics.world
        public PropertyInfo particles, size;           // HairDataFacade.TessRenderParticles, .Size (x = guide strands)
        public PropertyInfo count, tessSegments;       // HairPhysicsWorld GpuValue<int>
        public PropertyInfo computeBuffer;             // GpuBuffer<TessRenderParticle>.ComputeBuffer
        public PropertyInfo intValue;                  // GpuValue<int>.Value
    }
    // A hair item's guide strand count, read again when its particle buffer changes (a rebuild).
    private sealed class HairGuides
    {
        public ComputeBuffer buffer;
        public int guides;
    }
    private HairAccess hairAccess;
    private bool collectingHair, hairUnavailable, hairLogged;
    private int lastHairSweep;
    // Every HairRender since hair motion started (HairAwake), destroyed ones swept.
    private static readonly List<Component> hairs = new List<Component>();
    private readonly Dictionary<int, Component> hairPhysics = new Dictionary<int, Component>();
    private readonly Dictionary<int, HairGuides> hairGuides = new Dictionary<int, HairGuides>();

    // ---- rigid motion ----------------------------------------------------
    private sealed class RigidAccess
    {
        public FieldInfo type, meshRenderers, dynamicMeshRenderers;  // Atom
        public PropertyInfo singleton;                               // SuperController
        public MethodInfo getAtoms;
    }
    private sealed class RigidEntry
    {
        public MeshRenderer renderer;
        public MeshFilter filter;
        public Transform transform;
        public int id, previousFrame = -2;
        public Matrix4x4 previous;
        public Material[] materials;        // sharedMaterials, read again after a rebuild
    }
    private RigidAccess rigidAccess;
    private bool collectingRigid, rigidUnavailable, rigidLogged;
    private static bool rigidDirty = true;   // set by the atom hooks
    private readonly List<RigidEntry> rigids = new List<RigidEntry>();
    private readonly Dictionary<int, RigidEntry> rigidById = new Dictionary<int, RigidEntry>();
    private readonly HashSet<int> rigidSeen = new HashSet<int>();
    private readonly List<int> staleRigid = new List<int>();
    private int rigidMatrixCount, rigidMoved, rigidRebuilds, rigidFrames;
    private long rigidTicks;
    // Test switch (VaMTestAgent "set"): false sends transparent and shadow-only
    // renderers too, as 2.8.0 did.
    private static bool opaqueOnly = true;
    private int rigidSeeThrough, skinnedSeeThrough;     // moved but skipped this frame
    private readonly HashSet<int> seeThroughLogged = new HashSet<int>();

    // ---- skinned motion --------------------------------------------------
    // A mesh's bind pose and bone weights, pinned for the native side, which
    // copies them the first time it draws the mesh.
    private sealed class SkinnedMesh
    {
        public Vector3[] positions;
        public BoneWeight[] weights;
        public Matrix4x4[] bindposes;       // one identity for a renderer without bones
        public GCHandle positionHandle, weightHandle;
        public int lastUsed;
        public bool unusable;
        public bool freed;                  // swept: its handles are free, entries bind again
    }
    private sealed class SkinnedEntry
    {
        public SkinnedMeshRenderer renderer;
        public Mesh mesh;                   // the sharedMesh bones and data below are for
        public SkinnedMesh data;
        public Transform[] bones;
        public Matrix4x4[] current, previous;
        public int id, previousFrame = -2;
        public bool unusable;
        public Material[] materials;        // sharedMaterials, read again when the mesh changes
    }
    private bool skinnedReady, skinnedUnavailable, collectingSkinned, skinnedLogged, skinnedDirty;
    // CustomUnityAssetLoaders that finished loading, from the DoLoadedCallbacks postfix.
    private static readonly List<Component> loadedCuas = new List<Component>();
    private readonly List<SkinnedEntry> skinneds = new List<SkinnedEntry>();
    private readonly HashSet<int> skinnedIds = new HashSet<int>();
    private readonly Dictionary<Mesh, SkinnedMesh> skinnedMeshes = new Dictionary<Mesh, SkinnedMesh>();
    private readonly List<Mesh> staleSkinned = new List<Mesh>();
    private int skinnedMoved, skinnedFrames, lastSkinnedSweep, unreadableLogs;
    private long skinnedTicks;

    // ---- render scale, native frames, menus ------------------------------
    private bool scaleUnavailable, menuAccessFailed, prefsAccessFailed, scaledByBridge;
    private int seenPercent = -2, settledPercent = -1, appliedPercent = -1, scaleLogs, menuLogs;
    private float seenSince, vamScaleFallback = -1f;
    private bool wantNative, wantMenuResized, menusOpen, srOff;  // decided in LateUpdate
    private bool nativeFrame, menuResized, tailMenuResized;   // this frame, decided in PreCull
    private int nativeWidth, nativeHeight, normalWidth, normalHeight, tailFrames;
    private PropertyInfo superSingleton, hudVisible, worldUiActive, prefsScale;
    private FieldInfo prefsSingleton;

    private void Awake()
    {
        if (Environment.GetEnvironmentVariable("VAM_DLSS_BRIDGE") != "1")
        {
            enabled = false;
            return;
        }
        slots = Marshal.AllocHGlobal(FrameSize * SlotCount);
        drawSlots = Marshal.AllocHGlobal(DrawStride * MaxDraws * SlotCount);
        matrixSlots = Marshal.AllocHGlobal(RigidMatrixStride * MaxRigidMatrices * SlotCount);
        command = new CommandBuffer();
        latePathCommand=new CommandBuffer();latePathCommand.name="VaM PT transparent composition";
        opaqueDepthCommand = new CommandBuffer { name = "VaM DLSS bridge opaque depth" };
        command.name = "VaM DLSS bridge capture";
        SetupPathUi();
        SetupPostEffects();
        if (Desktop) SetupDesktopInput();
        Camera.onPreCull += PreCull;
        Camera.onPreRender += PreRender;
        Camera.onPostRender += PostRender;
        Logger.LogInfo("VaM DLSS bridge armed; waiting for the OpenComposite DLSS build");
    }

    private void OnDestroy()
    {
        if (desktopInputOwner == this) desktopInputOwner = null;
        collecting = false;
        Camera.onPreCull -= PreCull;
        Camera.onPreRender -= PreRender;
        Camera.onPostRender -= PostRender;
        DetachTarget();
        RestoreMsaa();
        if (scaledByBridge)
        {
            try { XRSettings.eyeTextureResolutionScale = VaMRenderScale(); } catch (Exception) { }
        }
        // The slot memory is intentionally not freed: an in-flight render
        // event may still read it. It is well under a megabyte.
    }

    private void ProbeNative()
    {
        nextNativeProbe = Time.realtimeSinceStartup + 2f;
        try
        {
            int version = VaMDLSS_GetApiVersion();
            if (version != ApiVersion)
            {
                nativeAbsent = true;
                Logger.LogWarning("VaM DLSS bridge: native API version " + version + " does not match " + ApiVersion + "; bridge disabled");
                return;
            }
            renderEvent = VaMDLSS_GetRenderEventFunc();
            nativeReady = renderEvent != IntPtr.Zero;
            if (nativeReady) Logger.LogInfo("VaM DLSS bridge: native OpenComposite DLSS exports found");
        }
        catch (EntryPointNotFoundException)
        {
            nativeAbsent = true;
            Logger.LogInfo("VaM DLSS bridge: openvr_api.dll has no DLSS exports (not the DLSS launcher); bridge disabled");
        }
        catch (DllNotFoundException)
        {
            if (++probeAttempts > 30)
            {
                nativeAbsent = true;
                Logger.LogInfo("VaM DLSS bridge: openvr_api.dll not loaded (not in VR); bridge disabled");
            }
        }
        catch (Exception e)
        {
            nativeAbsent = true;
            Logger.LogWarning("VaM DLSS bridge: native probe failed: " + e.Message);
        }
    }

    private static int NativeState()
    {
        try { return VaMDLSS_GetState(); }
        catch (Exception) { return StateFailed; }
    }

    private void DetachTarget()
    {
        if (target != null)
        {
            try { target.RemoveCommandBuffer(CaptureCameraEvent, command); } catch (Exception) { }
            try { target.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha,latePathCommand); } catch(Exception){}
            try { target.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, opaqueDepthCommand); } catch (Exception) { }
            if (jitteredThisFrame) { if (Desktop) target.ResetProjectionMatrix(); else target.ResetStereoProjectionMatrices(); }
            if (Desktop) { RetargetDesktopCameras(desktopTarget, previousTarget); target.targetTexture = previousTarget; target.allowMSAA = previousAllowMsaa; }
        }
        if (desktopOutput != null) Destroy(desktopOutput.gameObject);
        if (desktopPresent != null) desktopPresent.Release();
        if (desktopTarget != null) Destroy(desktopTarget);
        desktopOutput = null; desktopTarget = null; desktopPresent = null;
        jitteredThisFrame = false;
        target = null;
        pathCaptureOrdered=false;
        havePrevious = false;
    }

    private void SelectTarget()
    {
        nextCameraScan = Time.realtimeSinceStartup + 2f;
        Camera best = null;
        if (Desktop)
        {
            if (desktopCenterField == null)
            {
                Type ct = Type.GetType("CameraTarget, Assembly-CSharp", true);
                desktopCenterField = Field(ct, "centerTarget", true);
                desktopCameraField = Field(ct, "targetCamera", false);
            }
            object center = desktopCenterField.GetValue(null);
            best = center != null ? desktopCameraField.GetValue(center) as Camera : null;
        }
        Camera[] cameras = Desktop ? new Camera[0] : Camera.allCameras;
        for (int i = 0; i < cameras.Length; i++)
        {
            Camera c = cameras[i];
            if (c == null || !c.stereoEnabled || c.targetTexture != null || c.cullingMask == 0) continue;
            if (c.stereoTargetEye != StereoTargetEyeMask.Both) continue;
            if (best == null || c.depth < best.depth) best = c;
        }
        if (best == target) return;
        DetachTarget();
        if (best == null) return;
        target = best;
        if (Desktop)
        {
            previousTarget = target.targetTexture;
            previousAllowMsaa = target.allowMSAA;
            target.allowMSAA = false;
            GameObject go = new GameObject("VaM Desktop Output");
            desktopOutput = go.AddComponent<Camera>();
            desktopOutput.clearFlags = CameraClearFlags.Nothing;
            desktopOutput.cullingMask = 0;
            desktopOutput.stereoTargetEye = StereoTargetEyeMask.None;
            desktopOutput.allowMSAA = false;
            desktopPresent = new CommandBuffer { name = "VaM DesktopPresent" };
            desktopOutput.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, desktopPresent);
            ApplyDesktopScale(100);
        }
        target.AddCommandBuffer(CaptureCameraEvent, command);
        target.AddCommandBuffer(CameraEvent.AfterForwardAlpha,latePathCommand);
        target.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, opaqueDepthCommand);
        Logger.LogInfo("VaM DLSS bridge: main " + (Desktop ? "desktop" : "stereo") + " camera = " + target.name);
    }

    private bool IsTarget(Camera cam)
    {
        if (target == null || !target.isActiveAndEnabled || Time.realtimeSinceStartup >= nextCameraScan) SelectTarget();
        return cam == target && target != null;
    }

    private void EnforceMsaa(bool failed)
    {
        if (failed)
        {
            RestoreMsaa();
            return;
        }
        if (QualitySettings.antiAliasing != 0)
        {
            if (originalMsaa < 0) originalMsaa = QualitySettings.antiAliasing;
            QualitySettings.antiAliasing = 0;
            if (!msaaLogged)
            {
                Logger.LogInfo("VaM DLSS bridge: MSAA " + originalMsaa + "x -> off for DLSS (VaM preferences are not modified)");
                msaaLogged = true;
            }
        }
    }

    private void RestoreMsaa()
    {
        if (originalMsaa > 0 && QualitySettings.antiAliasing == 0) QualitySettings.antiAliasing = originalMsaa;
        originalMsaa = -1;
    }

    private static float Halton(int index, int radix)
    {
        float result = 0f, fraction = 1f / radix;
        while (index > 0)
        {
            result += (index % radix) * fraction;
            index /= radix;
            fraction /= radix;
        }
        return result;
    }

    // Rewrite managed input callers, not Unity's native-backed camera methods
    // (this Unity/Harmony version cannot detour those wrappers safely).
    private static VaMDLSSBridge desktopInputOwner;
    private static readonly Dictionary<MethodInfo,MethodInfo> desktopInputCalls = new Dictionary<MethodInfo,MethodInfo>();
    private static readonly Dictionary<short,OpCode> desktopInputOpcodes = new Dictionary<short,OpCode>();
    private void SetupDesktopInput()
    {
        desktopInputOwner = this;
        Harmony input = new Harmony("local.vam.dlssbridge.desktopinput");
        BindingFlags methods=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance|BindingFlags.Static;
        Type own=typeof(VaMDLSSBridge),camera=typeof(Camera),rect=typeof(RectTransformUtility);
        AddDesktopInput(camera.GetMethod("ScreenPointToRay",methods,null,new Type[]{typeof(Vector2)},null),"DesktopRay2");
        AddDesktopInput(camera.GetMethod("ScreenPointToRay",new Type[]{typeof(Vector3)}),"DesktopRay3");
        AddDesktopInput(camera.GetMethod("ScreenToViewportPoint",new Type[]{typeof(Vector3)}),"DesktopToViewport");
        AddDesktopInput(camera.GetMethod("ScreenToWorldPoint",new Type[]{typeof(Vector3)}),"DesktopToWorld");
        AddDesktopInput(camera.GetMethod("WorldToScreenPoint",new Type[]{typeof(Vector3)}),"DesktopToScreen");
        AddDesktopInput(camera.GetProperty("pixelRect").GetGetMethod(),"DesktopRect");
        AddDesktopInput(rect.GetMethod("RectangleContainsScreenPoint",new Type[]{typeof(RectTransform),typeof(Vector2),typeof(Camera)}),"DesktopContains");
        AddDesktopInput(rect.GetMethod("ScreenPointToLocalPointInRectangle"),"DesktopLocalPoint");
        AddDesktopInput(rect.GetMethod("ScreenPointToWorldPointInRectangle"),"DesktopRectangleWorld");
        AddDesktopInput(rect.GetMethod("ScreenPointToRay"),"DesktopRectangleRay");
        AddDesktopInput(rect.GetMethod("WorldToScreenPoint"),"DesktopRectangleScreen");
        foreach(FieldInfo field in typeof(OpCodes).GetFields(BindingFlags.Static|BindingFlags.Public))
            if(field.FieldType==typeof(OpCode)){OpCode op=(OpCode)field.GetValue(null);desktopInputOpcodes[op.Value]=op;}
        int patched=0;
        // Once at startup. Includes VaM picking/gizmos and uGUI clicks, sliders,
        // scroll views and text selection; no scene scans or per-frame discovery.
        foreach(Assembly assembly in new Assembly[]{Type.GetType("SuperController, Assembly-CSharp",true).Assembly,typeof(Graphic).Assembly})
            foreach(Type type in assembly.GetTypes())
                foreach(MethodInfo method in type.GetMethods(methods|BindingFlags.DeclaredOnly))
                    if(!method.ContainsGenericParameters && DesktopInputCaller(method)){
                        input.Patch(method,null,null,new HarmonyMethod(own.GetMethod("MapDesktopInputCalls")),null);patched++;
                    }
        Logger.LogInfo("VaM desktop input: window coordinates mapped in "+patched+" managed input methods; render target unchanged");
    }
    private static void AddDesktopInput(MethodInfo source,string replacement)
    { if(source==null)throw new MissingMethodException(replacement);desktopInputCalls[source]=typeof(VaMDLSSBridge).GetMethod(replacement); }
    private static bool DesktopInputCaller(MethodInfo method)
    {
        MethodBody body=method.GetMethodBody();if(body==null)return false;byte[] bytes=body.GetILAsByteArray();
        for(int at=0;at<bytes.Length;){
            short value=bytes[at++];if(value==0xfe)value=(short)(0xfe00|bytes[at++]);OpCode op=desktopInputOpcodes[value];
            int size=0;
            switch(op.OperandType){
                case OperandType.InlineNone:break;
                case OperandType.ShortInlineBrTarget:case OperandType.ShortInlineI:case OperandType.ShortInlineVar:size=1;break;
                case OperandType.InlineVar:size=2;break;
                case OperandType.InlineI8:case OperandType.InlineR:size=8;break;
                case OperandType.InlineSwitch:size=4+4*BitConverter.ToInt32(bytes,at);break;
                default:size=4;break;
            }
            if((op==OpCodes.Call||op==OpCodes.Callvirt)&&size==4){
                MethodInfo called=method.Module.ResolveMethod(BitConverter.ToInt32(bytes,at)) as MethodInfo;
                if(called!=null && desktopInputCalls.ContainsKey(called))return true;
            }
            at+=size;
        }
        return false;
    }
    public static IEnumerable<CodeInstruction> MapDesktopInputCalls(IEnumerable<CodeInstruction> instructions)
    {
        foreach(CodeInstruction instruction in instructions){
            MethodInfo called=instruction.operand as MethodInfo,replacement;
            if(called!=null && (instruction.opcode==OpCodes.Call||instruction.opcode==OpCodes.Callvirt) && desktopInputCalls.TryGetValue(called,out replacement)){
                instruction.opcode=OpCodes.Call;instruction.operand=replacement;
            }
            yield return instruction;
        }
    }
    private static bool DesktopInputScale(Camera camera, out float x, out float y)
    {
        x = y = 1f;
        VaMDLSSBridge owner = desktopInputOwner;
        if (owner == null || camera != owner.target || owner.desktopTarget == null ||
            camera.targetTexture != owner.desktopTarget || Screen.width <= 0 || Screen.height <= 0) return false;
        x = (float)owner.desktopTarget.width / Screen.width;
        y = (float)owner.desktopTarget.height / Screen.height;
        return true;
    }
    private static Vector2 DesktopPoint(Camera camera,Vector2 point)
    { float x,y;if(DesktopInputScale(camera,out x,out y)){point.x*=x;point.y*=y;}return point; }
    private static Vector3 DesktopPoint3(Camera camera,Vector3 point)
    { Vector2 p=DesktopPoint(camera,point);point.x=p.x;point.y=p.y;return point; }
    public static Ray DesktopRay2(Camera camera,Vector2 point){return camera.ScreenPointToRay(DesktopPoint(camera,point));}
    public static Ray DesktopRay3(Camera camera,Vector3 point){return camera.ScreenPointToRay(DesktopPoint3(camera,point));}
    public static Vector3 DesktopToViewport(Camera camera,Vector3 point){return camera.ScreenToViewportPoint(DesktopPoint3(camera,point));}
    public static Vector3 DesktopToWorld(Camera camera,Vector3 point){return camera.ScreenToWorldPoint(DesktopPoint3(camera,point));}
    public static Vector3 DesktopToScreen(Camera camera,Vector3 point)
    { Vector3 result=camera.WorldToScreenPoint(point);float x,y;if(DesktopInputScale(camera,out x,out y)){result.x/=x;result.y/=y;}return result; }
    public static Rect DesktopRect(Camera camera)
    { float x,y;return DesktopInputScale(camera,out x,out y)?new Rect(0,0,Screen.width,Screen.height):camera.pixelRect; }
    public static bool DesktopContains(RectTransform rect,Vector2 point,Camera camera)
    { return RectTransformUtility.RectangleContainsScreenPoint(rect,DesktopPoint(camera,point),camera); }
    public static bool DesktopLocalPoint(RectTransform rect,Vector2 point,Camera camera,out Vector2 local)
    { return RectTransformUtility.ScreenPointToLocalPointInRectangle(rect,DesktopPoint(camera,point),camera,out local); }
    public static bool DesktopRectangleWorld(RectTransform rect,Vector2 point,Camera camera,out Vector3 world)
    { return RectTransformUtility.ScreenPointToWorldPointInRectangle(rect,DesktopPoint(camera,point),camera,out world); }
    public static Ray DesktopRectangleRay(Camera camera,Vector2 point)
    { return RectTransformUtility.ScreenPointToRay(camera,DesktopPoint(camera,point)); }
    public static Vector2 DesktopRectangleScreen(Camera camera,Vector3 point)
    { return camera!=null?(Vector2)DesktopToScreen(camera,point):RectTransformUtility.WorldToScreenPoint(camera,point); }

    // ---- object motion: discovery and Harmony hooks --------------------

    public static void SkinDrawn(Component __instance)
    {
        if (collecting && skinCalls.Count < MaxRecordedCalls) skinCalls.Add(__instance);
    }

    public static void WrapDrawn(Component __instance, bool noDelay)
    {
        if (collecting && wrapCalls.Count < MaxRecordedCalls)
        {
            wrapCalls.Add(__instance);
            wrapNoDelay.Add(noDelay);
        }
    }

    private static FieldInfo Field(Type type, string name, bool isStatic)
    {
        BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly |
                             (isStatic ? BindingFlags.Static : BindingFlags.Instance);
        for (Type t = type; t != null; t = t.BaseType)
        {
            FieldInfo f = t.GetField(name, flags);
            if (f != null) return f;
        }
        throw new MissingFieldException(type.Name, name);
    }

    private void SetupObjectMotion()
    {
        try
        {
            Type skin = Type.GetType("DAZSkinV2, Assembly-CSharp", true);
            Type wrap = Type.GetType("DAZSkinWrap, Assembly-CSharp", true);
            SkinAccess s = new SkinAccess();
            s.renderSuspend = Field(skin, "_renderSuspend", false);
            s.staticDraw = Field(skin, "staticDraw", true);
            s.mesh = Field(skin, "mesh", false);
            s.delayDisplay = Field(skin, "delayDisplayOneFrame", false);
            s.useSmoothing = Field(skin, "_useSmoothing", false);
            s.raw = Field(skin, "rawVertsBuffer", false);
            s.smoothed = Field(skin, "smoothedVertsBuffer", false);
            s.delayed = Field(skin, "delayedVertsBuffer", false);
            s.drawOffset = Field(skin, "drawOffset", false);
            s.materialsEnabled = Field(skin, "materialsEnabled", false);
            s.useSimple = Field(skin, "GPUuseSimpleMaterial", false);
            s.simpleMaterial = Field(skin, "GPUsimpleMaterial", false);
            s.gpuMaterials = Field(skin, "GPUmaterials", false);
            WrapAccess w = new WrapAccess();
            w.renderSuspend = Field(wrap, "_renderSuspend", false);
            w.staticDraw = Field(wrap, "staticDraw", true);
            w.mesh = Field(wrap, "mesh", false);
            w.skin = Field(wrap, "skin", false);
            w.delayed = Field(wrap, "_delayedVertsBuffer", false);
            w.drawVertices = Field(wrap, "_drawVerticesBuffer", false);
            w.materialsEnabled = Field(wrap, "materialsEnabled", false);
            w.useSimple = Field(wrap, "GPUuseSimpleMaterial", false);
            w.simpleMaterial = Field(wrap, "GPUsimpleMaterial", false);
            w.gpuMaterials = Field(wrap, "GPUmaterials", false);
            BindingFlags any = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
            MethodInfo skinDraw = skin.GetMethod("DrawMeshGPU", any, null, Type.EmptyTypes, null);
            MethodInfo wrapDraw = wrap.GetMethod("DrawMeshGPU", any, null, new Type[] { typeof(bool) }, null);
            if (skinDraw == null || wrapDraw == null) throw new MissingMethodException("DrawMeshGPU");
            BindingFlags hook = BindingFlags.Public | BindingFlags.Static;
            harmony = new Harmony("local.vam.dlssbridge.objectmotion");
            harmony.Patch(skinDraw, null, new HarmonyMethod(typeof(VaMDLSSBridge).GetMethod("SkinDrawn", hook)), null, null);
            harmony.Patch(wrapDraw, null, new HarmonyMethod(typeof(VaMDLSSBridge).GetMethod("WrapDrawn", hook)), null, null);
            skinAccess = s;
            wrapAccess = w;
            motionPatched = true;
            Logger.LogInfo("VaM DLSS bridge: object motion hooks installed (DAZSkinV2 / DAZSkinWrap GPU draws)");
        }
        catch (Exception e)
        {
            motionUnavailable = true;
            Logger.LogWarning("VaM DLSS bridge: object motion unavailable, camera motion only: " + e.Message);
        }
    }

    private BufferEntry BufferFor(ComputeBuffer buffer, int frame)
    {
        BufferEntry e;
        if (buffers.TryGetValue(buffer, out e))
        {
            e.lastUsed = frame;
            return e.pointer == IntPtr.Zero ? null : e;
        }
        e = new BufferEntry();
        e.lastUsed = frame;
        try
        {
            e.stride = buffer.stride;
            e.count = buffer.count;
            // Synchronises with the render thread: once per buffer, then cached.
            e.pointer = buffer.GetNativeBufferPtr();
        }
        catch (Exception) { e.pointer = IntPtr.Zero; }
        buffers[buffer] = e;
        return e.pointer == IntPtr.Zero ? null : e;
    }

    // withVertices: also the vertex buffer (stream 0, position first), which
    // only rigid meshes are drawn from.
    private MeshEntry MeshFor(Mesh mesh, int frame, bool withVertices)
    {
        int subMeshes = mesh.subMeshCount, vertices = mesh.vertexCount;
        MeshEntry e;
        if (meshes.TryGetValue(mesh, out e))
        {
            e.lastUsed = frame;
            if (e.subMeshCount == subMeshes && e.vertexCount == vertices)
            {
                if (e.indexBuffer != IntPtr.Zero && (!withVertices || e.vertexBuffer != IntPtr.Zero)) return e;
                if (Time.realtimeSinceStartup < e.retryAt) return null;
                if (e.indexBuffer != IntPtr.Zero)
                {
                    // Synchronises with the render thread, like the index buffer: once per mesh.
                    try { e.vertexBuffer = mesh.GetNativeVertexBufferPtr(0); }
                    catch (Exception) { e.vertexBuffer = IntPtr.Zero; }
                    if (e.vertexBuffer == IntPtr.Zero) e.retryAt = Time.realtimeSinceStartup + 1f;
                    return e.vertexBuffer == IntPtr.Zero ? null : e;
                }
            }
        }
        e = new MeshEntry();
        e.lastUsed = frame;
        e.subMeshCount = subMeshes;
        e.vertexCount = vertices;
        e.start = new uint[subMeshes];
        e.count = new uint[subMeshes];
        e.baseVertex = new int[subMeshes];
        try
        {
            for (int s = 0; s < subMeshes; s++)
            {
                e.start[s] = mesh.GetIndexStart(s);
                // Lines, points and quads are not drawn as triangle lists.
                e.count[s] = mesh.GetTopology(s) == MeshTopology.Triangles ? mesh.GetIndexCount(s) : 0u;
                e.baseVertex[s] = (int)mesh.GetBaseVertex(s);
            }
            e.index32 = mesh.indexFormat == IndexFormat.UInt32;
            e.indexBuffer = mesh.GetNativeIndexBufferPtr();
            if (withVertices && e.indexBuffer != IntPtr.Zero) e.vertexBuffer = mesh.GetNativeVertexBufferPtr(0);
        }
        catch (Exception) { e.indexBuffer = e.vertexBuffer = IntPtr.Zero; }
        bool ready = e.indexBuffer != IntPtr.Zero && (!withVertices || e.vertexBuffer != IntPtr.Zero);
        if (!ready) e.retryAt = Time.realtimeSinceStartup + 1f;
        meshes[mesh] = e;
        return ready ? e : null;
    }

    private void SweepCaches(int frame)
    {
        if (frame - lastCacheSweep < CacheFrames) return;
        lastCacheSweep = frame;
        staleBuffers.Clear();
        foreach (KeyValuePair<ComputeBuffer, BufferEntry> p in buffers)
            if (frame - p.Value.lastUsed > CacheFrames) staleBuffers.Add(p.Key);
        for (int i = 0; i < staleBuffers.Count; i++) buffers.Remove(staleBuffers[i]);
        staleMeshes.Clear();
        foreach (KeyValuePair<Mesh, MeshEntry> p in meshes)
            if (frame - p.Value.lastUsed > CacheFrames) staleMeshes.Add(p.Key);
        for (int i = 0; i < staleMeshes.Count; i++) meshes.Remove(staleMeshes[i]);
    }

    private static unsafe int WriteDraw(byte* output, int n, int key, BufferEntry b, MeshEntry m, int sub, Vector3 offset)
    {
        if (m.count[sub] == 0) return n;
        byte* d = output + n * DrawStride;
        *(long*)(d + 0) = key;
        *(long*)(d + 8) = b.pointer.ToInt64();
        *(long*)(d + 16) = m.indexBuffer.ToInt64();
        *(uint*)(d + 24) = m.start[sub];
        *(uint*)(d + 28) = m.count[sub];
        *(int*)(d + 32) = m.baseVertex[sub];
        *(uint*)(d + 36) = m.index32 ? 1u : 0u;
        float* f = (float*)(d + 40);
        f[0] = offset.x; f[1] = offset.y; f[2] = offset.z;
        *(uint*)(d + 52) = (uint)b.stride;
        *(uint*)(d + 56) = (uint)b.count;
        *(uint*)(d + 60) = 0;
        return n + 1;
    }

    private unsafe int AddSkin(Component c, int mask, int frame, byte* output, int n)
    {
        if (c == null) return n;
        int id = c.GetInstanceID();
        if (drawnThisFrame.ContainsKey(id)) return n;
        drawnThisFrame[id] = 1;
        if ((mask & (1 << c.gameObject.layer)) == 0) return n;
        SkinAccess a = skinAccess;
        if ((bool)a.renderSuspend.GetValue(c) || (bool)a.staticDraw.GetValue(null)) return n;
        Mesh mesh = a.mesh.GetValue(c) as Mesh;
        if (mesh == null) return n;
        ComputeBuffer vertices;
        if ((bool)a.delayDisplay.GetValue(c)) vertices = a.delayed.GetValue(c) as ComputeBuffer;
        else if ((bool)a.useSmoothing.GetValue(c)) vertices = a.smoothed.GetValue(c) as ComputeBuffer;
        else vertices = a.raw.GetValue(c) as ComputeBuffer;
        if (vertices == null) return n;
        bool[] enabledMaterials = a.materialsEnabled.GetValue(c) as bool[];
        Material simple = a.simpleMaterial.GetValue(c) as Material;
        bool useSimple = (bool)a.useSimple.GetValue(c) && simple != null;
        Material[] materials = a.gpuMaterials.GetValue(c) as Material[];
        Vector3 offset = (Vector3)a.drawOffset.GetValue(c);
        BufferEntry b = BufferFor(vertices, frame);
        MeshEntry m = b == null ? null : MeshFor(mesh, frame, false);
        if (m == null) return n;
        for (int s = 0; s < m.subMeshCount && n < MaxDraws; s++)
        {
            if (enabledMaterials == null || s >= enabledMaterials.Length || !enabledMaterials[s]) continue;
            if (!useSimple && (materials == null || s >= materials.Length || materials[s] == null)) continue;
            n = WriteDraw(output, n, id, b, m, s, offset);
        }
        return n;
    }

    private unsafe int AddWrap(Component c, bool noDelay, int mask, int frame, byte* output, int n)
    {
        if (c == null) return n;
        int id = c.GetInstanceID();
        if (drawnThisFrame.ContainsKey(id)) return n;
        drawnThisFrame[id] = 1;
        if ((mask & (1 << c.gameObject.layer)) == 0) return n;
        WrapAccess a = wrapAccess;
        if ((bool)a.renderSuspend.GetValue(c) || (bool)a.staticDraw.GetValue(null)) return n;
        Mesh mesh = a.mesh.GetValue(c) as Mesh;
        if (mesh == null) return n;
        Component skin = a.skin.GetValue(c) as Component;
        bool delayed = skin != null && !noDelay && (bool)skinAccess.delayDisplay.GetValue(skin);
        ComputeBuffer vertices = (delayed ? a.delayed.GetValue(c) : a.drawVertices.GetValue(c)) as ComputeBuffer;
        if (vertices == null) return n;
        bool[] enabledMaterials = a.materialsEnabled.GetValue(c) as bool[];
        Material simple = a.simpleMaterial.GetValue(c) as Material;
        bool useSimple = (bool)a.useSimple.GetValue(c) && simple != null;
        Material[] materials = a.gpuMaterials.GetValue(c) as Material[];
        BufferEntry b = BufferFor(vertices, frame);
        MeshEntry m = b == null ? null : MeshFor(mesh, frame, false);
        if (m == null) return n;
        for (int s = 0; s < m.subMeshCount && n < MaxDraws; s++)
        {
            if (!useSimple)
            {
                if (enabledMaterials == null || s >= enabledMaterials.Length || !enabledMaterials[s]) continue;
                if (materials == null || s >= materials.Length || materials[s] == null) continue;
            }
            if (Hidden(useSimple ? simple : materials[s])) continue;  // 2.9.7: a part its author hid
            n = WriteDraw(output, n, id, b, m, s, Vector3.zero);
        }
        return n;
    }

    // ---- hair motion: VaM physics hair -----------------------------------

    private static PropertyInfo Property(Type type, string name)
    {
        PropertyInfo p = type.GetProperty(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        if (p == null) throw new MissingMemberException(type.Name, name);
        return p;
    }

    private void SetupHairMotion()
    {
        try
        {
            HairAccess h = new HairAccess();
            h.render = Type.GetType("GPUTools.Hair.Scripts.Runtime.Render.HairRender, Assembly-CSharp", true);
            h.physics = Type.GetType("GPUTools.Hair.Scripts.Runtime.Physics.GPHairPhysics, Assembly-CSharp", true);
            Type facade = Type.GetType("GPUTools.Hair.Scripts.Runtime.Data.HairDataFacade, Assembly-CSharp", true);
            Type world = Type.GetType("GPUTools.Hair.Scripts.Runtime.Physics.HairPhysicsWorld, Assembly-CSharp", true);
            h.rend = Field(h.render, "rend", false);
            h.data = Field(h.render, "data", false);
            h.world = Field(h.physics, "world", false);
            h.particles = Property(facade, "TessRenderParticles");
            h.size = Property(facade, "Size");
            h.count = Property(world, "TessRenderParticlesCount");
            h.tessSegments = Property(world, "TessSegments");
            h.computeBuffer = Property(h.particles.PropertyType, "ComputeBuffer");
            h.intValue = Property(h.count.PropertyType, "Value");
            MethodInfo awake = h.render.GetMethod("Awake", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            if (awake == null) throw new MissingMethodException(h.render.Name, "Awake");
            new Harmony("local.vam.dlssbridge.hair").Patch(awake, null,
                new HarmonyMethod(typeof(VaMDLSSBridge).GetMethod("HairAwake", BindingFlags.Public | BindingFlags.Static)), null, null);
            // The ones made before the hook, inactive ones included: the only search.
            // It also returns prefabs from asset bundles, which are in no scene.
            UnityEngine.Object[] found = Resources.FindObjectsOfTypeAll(h.render);
            for (int i = 0; i < found.Length; i++)
            {
                Component c = found[i] as Component;
                if (c != null && c.gameObject.scene.IsValid()) HairAwake(c);
            }
            hairAccess = h;
            Logger.LogInfo("VaM DLSS bridge: hair motion ready (GPUTools physics hair guide strands; " + hairs.Count +
                           " HairRender(s) so far, later ones from HairRender.Awake)");
        }
        catch (Exception e)
        {
            hairUnavailable = true;
            Logger.LogWarning("VaM DLSS bridge: hair motion unavailable, hair keeps camera motion: " + e.Message);
        }
    }

    public static void HairAwake(Component __instance)
    {
        if (__instance != null && !hairs.Contains(__instance)) hairs.Add(__instance);
    }

    private void SweepHair(int frame)
    {
        if (frame - lastHairSweep < CacheFrames) return;
        lastHairSweep = frame;
        hairs.RemoveAll(delegate(Component c) { return c == null; });
        if (hairPhysics.Count > 4 * hairs.Count + 64) hairPhysics.Clear();
        if (hairGuides.Count > 4 * hairs.Count + 64) hairGuides.Clear();
    }

    private int HairValue(PropertyInfo property, object world)
    {
        object value = property.GetValue(world, null);
        return value == null ? 0 : (int)hairAccess.intValue.GetValue(value, null);
    }

    private unsafe int AddHair(Component c, int mask, int frame, byte* output, int n)
    {
        if (c == null) return n;
        int id = c.GetInstanceID();
        if (drawnThisFrame.ContainsKey(id)) return n;
        drawnThisFrame[id] = 1;
        GameObject go = c.gameObject;
        if (!go.activeInHierarchy || (mask & (1 << go.layer)) == 0) return n;
        HairAccess a = hairAccess;
        Renderer r = a.rend.GetValue(c) as Renderer;
        if (r == null || !r.enabled || !r.isVisible) return n;
        object data = a.data.GetValue(c);
        object gpu = data == null ? null : a.particles.GetValue(data, null);
        ComputeBuffer particles = gpu == null ? null : a.computeBuffer.GetValue(gpu, null) as ComputeBuffer;
        if (particles == null || particles.stride != HairStride) return n;
        Component physics;
        if (!hairPhysics.TryGetValue(id, out physics) || physics == null)
        {
            physics = c.GetComponent(a.physics);
            hairPhysics[id] = physics;
        }
        object world = physics == null ? null : a.world.GetValue(physics);
        if (world == null) return n;
        // GPUTools' CSTesselateWithNormals writes TessSegments points per guide
        // strand, the guides one after another (point = guide x TessSegments + k),
        // and TessRenderParticlesCount runs on past them: the guides' points only.
        // 2.7.0 took Segments x TessSegments points per strand, which joined
        // Segments guides tip to root into one strand. Never past the buffer; a
        // clipped count no longer divides into strands, and the native side then
        // splits strands by RootIndex.
        int tess = HairValue(a.tessSegments, world);
        int guides = GuidesOf(id, data, particles);
        int count = Math.Min(HairValue(a.count, world), particles.count);
        if (guides > 0 && tess > 0) count = Math.Min(count, guides * tess);
        if (count < 2) return n;
        int perStrand = guides > 0 && tess > 0 ? tess : 0;
        BufferEntry b = BufferFor(particles, frame);
        if (b == null) return n;
        byte* d = output + n * DrawStride;
        *(long*)(d + 0) = id;
        *(long*)(d + 8) = b.pointer.ToInt64();
        *(long*)(d + 16) = 0L;
        *(uint*)(d + 24) = (uint)perStrand;   // points per strand
        *(uint*)(d + 28) = (uint)count;       // valid points
        *(int*)(d + 32) = 0;
        *(uint*)(d + 36) = DrawHair;
        float* f = (float*)(d + 40);
        f[0] = 0f; f[1] = 0f; f[2] = 0f;
        *(uint*)(d + 52) = (uint)b.stride;
        *(uint*)(d + 56) = (uint)b.count;
        *(uint*)(d + 60) = 0;
        if (!hairLogged)
        {
            hairLogged = true;
            Logger.LogInfo("VaM DLSS bridge: hair motion for " + hairs.Count + " hair item(s); first has " + count +
                           " guide points (" + guides + " guide strands x " + perStrand + " points)");
        }
        return n + 1;
    }

    private int GuidesOf(int id, object data, ComputeBuffer particles)
    {
        HairGuides g;
        if (hairGuides.TryGetValue(id, out g) && ReferenceEquals(g.buffer, particles)) return g.guides;
        g = new HairGuides();
        g.buffer = particles;
        object size = hairAccess.size.GetValue(data, null);
        g.guides = size is Vector4 ? (int)((Vector4)size).x : 0;
        hairGuides[id] = g;
        return g.guides;
    }

    // ---- rigid motion: moving props, CUAs, toys ---------------------------

    public static void RigidChanged()
    {
        rigidDirty = true;
    }

    private static void PatchAll(Harmony h, Type type, string name, MethodInfo postfix)
    {
        int patched = 0;
        foreach (MethodInfo m in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
        {
            if (m.Name != name) continue;
            h.Patch(m, null, new HarmonyMethod(postfix), null, null);
            patched++;
        }
        if (patched == 0) throw new MissingMethodException(type.Name, name);
    }

    private void SetupRigidMotion()
    {
        try
        {
            RigidAccess a = new RigidAccess();
            Type atom = Type.GetType("Atom, Assembly-CSharp", true);
            Type controller = Type.GetType("SuperController, Assembly-CSharp", true);
            a.type = Field(atom, "type", false);
            a.meshRenderers = Field(atom, "_meshRenderers", false);
            a.dynamicMeshRenderers = Field(atom, "_dynamicMeshRenderers", false);
            a.singleton = controller.GetProperty("singleton", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
            a.getAtoms = controller.GetMethod("GetAtoms", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, Type.EmptyTypes, null);
            if (a.singleton == null || a.getAtoms == null) throw new MissingMemberException("SuperController.singleton / GetAtoms");
            // VaM's own per-atom MeshRenderer lists (Atom.Init; CUAs register
            // theirs as they load): rebuilt when they or the atom list change.
            MethodInfo changed = typeof(VaMDLSSBridge).GetMethod("RigidChanged", BindingFlags.Public | BindingFlags.Static);
            Harmony h = new Harmony("local.vam.dlssbridge.rigid");
            PatchAll(h, atom, "Init", changed);
            PatchAll(h, atom, "RegisterDynamicMeshRenderer", changed);
            PatchAll(h, atom, "DeregisterDynamicMeshRenderer", changed);
            PatchAll(h, controller, "AddAtom", changed);
            PatchAll(h, controller, "RemoveAtom", changed);
            rigidAccess = a;
            rigidDirty = true;
            Logger.LogInfo("VaM DLSS bridge: rigid motion ready (MeshRenderers of non-Person atoms, from VaM's atom lists)");
        }
        catch (Exception e)
        {
            rigidUnavailable = true;
            Logger.LogWarning("VaM DLSS bridge: rigid motion unavailable, props keep camera motion: " + e.Message);
        }
    }

    private void RebuildRigid()
    {
        rigidDirty = false;
        RigidAccess a = rigidAccess;
        rigidSeen.Clear();
        rigids.Clear();
        object controller = a.singleton.GetValue(null, null);
        System.Collections.IList atoms = controller == null ? null : a.getAtoms.Invoke(controller, null) as System.Collections.IList;
        if (atoms != null)
        {
            for (int i = 0; i < atoms.Count; i++)
            {
                Component atom = atoms[i] as Component;
                // Person atoms: skin, clothing and hair have their own motion; their
                // MeshRenderers include the hair renderer, whose mesh is no geometry.
                if (atom == null || (a.type.GetValue(atom) as string) == "Person") continue;
                AddRigid(a.meshRenderers.GetValue(atom) as System.Collections.IList);
                AddRigid(a.dynamicMeshRenderers.GetValue(atom) as System.Collections.IList);
            }
        }
        staleRigid.Clear();
        foreach (KeyValuePair<int, RigidEntry> p in rigidById)
            if (!rigidSeen.Contains(p.Key)) staleRigid.Add(p.Key);
        for (int i = 0; i < staleRigid.Count; i++) rigidById.Remove(staleRigid[i]);
        if (++rigidRebuilds <= 20 || rigidRebuilds % 100 == 0)
            Logger.LogInfo("VaM DLSS bridge: rigid motion watches " + rigids.Count + " MeshRenderer(s) in " + (atoms == null ? 0 : atoms.Count) +
                           " atoms (rebuild " + rigidRebuilds + ")");
    }

    private void AddRigid(System.Collections.IList renderers)
    {
        if (renderers == null) return;
        for (int i = 0; i < renderers.Count; i++)
        {
            MeshRenderer r = renderers[i] as MeshRenderer;
            if (r == null) continue;
            int id = r.GetInstanceID();
            if (!rigidSeen.Add(id)) continue;
            RigidEntry e;
            if (!rigidById.TryGetValue(id, out e))
            {
                e = new RigidEntry();
                e.renderer = r;
                e.transform = r.transform;
                // Static batches are drawn from a combined mesh already in world space.
                e.filter = r.isPartOfStaticBatch ? null : r.GetComponent<MeshFilter>();
                e.id = id;
                rigidById[id] = e;
            }
            e.materials = null;
            if (e.filter != null) rigids.Add(e);
        }
    }

    // A renderer's materials, read once and again when VaM's lists change, the
    // mesh changes or one of them was destroyed (replaced).
    private static Material[] MaterialsOf(Renderer r, ref Material[] cached)
    {
        if (cached != null)
        {
            int k = 0;
            while (k < cached.Length && cached[k] != null) k++;
            if (k == cached.Length) return cached;
        }
        cached = r.sharedMaterials;
        return cached;
    }

    // Whether Unity draws submesh s into the scene depth: with a material in
    // the opaque queues. Unity draws submesh s with material s, materials past
    // the last submesh draw that one again, and a submesh without a material
    // is not drawn.
    private static bool OpaqueSubmesh(Material[] materials, int s, int subMeshCount)
    {
        if (!opaqueOnly) return true;
        int last = s == subMeshCount - 1 ? materials.Length - 1 : Math.Min(s, materials.Length - 1);
        for (int k = s; k <= last; k++)
        {
            Material m = materials[k];
            if (m != null && m.renderQueue <= OpaqueQueueLast) return true;
        }
        return false;
    }

    // Moved, visible, but drawn by the camera into no depth (transparent
    // materials only, or shadow-only): no motion of its own, counted, and
    // the first ones named in the log.
    private bool SeeThrough(Renderer r, Material[] materials, int subMeshCount, string kind)
    {
        if (!opaqueOnly) return false;
        bool shadowOnly = r.shadowCastingMode == ShadowCastingMode.ShadowsOnly;
        if (!shadowOnly)
            for (int s = 0; s < subMeshCount; s++)
                if (OpaqueSubmesh(materials, s, subMeshCount)) return false;
        if (seeThroughLogged.Count < SeeThroughLogs && seeThroughLogged.Add(r.GetInstanceID()))
        {
            Material m = materials.Length > 0 ? materials[0] : null;
            Transform parent = r.transform.parent;
            Logger.LogInfo("VaM DLSS bridge: " + kind + " motion skips " + (parent != null ? parent.name + "/" : "") + r.name +
                           (shadowOnly ? " (shadows only)" : " (" + materials.Length + " material(s), first " +
                           (m == null ? "none" : (m.shader != null ? m.shader.name : "?") + " queue " + m.renderQueue) + ")"));
        }
        return true;
    }

    private static bool SameMatrix(ref Matrix4x4 a, ref Matrix4x4 b)
    {
        return a.m00 == b.m00 && a.m01 == b.m01 && a.m02 == b.m02 && a.m03 == b.m03 &&
               a.m10 == b.m10 && a.m11 == b.m11 && a.m12 == b.m12 && a.m13 == b.m13 &&
               a.m20 == b.m20 && a.m21 == b.m21 && a.m22 == b.m22 && a.m23 == b.m23;
    }

    // Every watched renderer's matrix each frame; ones that moved since the
    // previous frame and are visible go to the native side with both matrices.
    private unsafe int AddRigids(int mask, int frame, byte* output, int n, byte* matrices)
    {
        if (rigidDirty) RebuildRigid();
        long started = System.Diagnostics.Stopwatch.GetTimestamp();
        int moved = 0, seeThrough = 0;
        for (int i = 0; i < rigids.Count; i++)
        {
            RigidEntry e = rigids[i];
            if (e.transform == null) { rigidDirty = true; continue; }
            Matrix4x4 now = e.transform.localToWorldMatrix;
            bool history = e.previousFrame == frame - 1;
            Matrix4x4 before = e.previous;
            e.previous = now;
            e.previousFrame = frame;
            if (!history || SameMatrix(ref now, ref before)) continue;
            moved++;
            if (n >= MaxDraws || rigidMatrixCount >= MaxRigidMatrices) continue;
            MeshRenderer r = e.renderer;
            if (r == null || !r.enabled || !r.isVisible || (mask & (1 << r.gameObject.layer)) == 0) continue;
            Mesh mesh = e.filter == null ? null : e.filter.sharedMesh;
            MeshEntry m = mesh == null ? null : MeshFor(mesh, frame, true);
            if (m == null || m.vertexBuffer == IntPtr.Zero) continue;
            Material[] materials = MaterialsOf(r, ref e.materials);
            if (SeeThrough(r, materials, m.subMeshCount, "rigid")) { seeThrough++; continue; }
            int index = rigidMatrixCount++;
            float* f = (float*)(matrices + index * RigidMatrixStride);
            for (int row = 0; row < 4; row++)
                for (int col = 0; col < 4; col++)
                {
                    f[row * 4 + col] = now[row, col];
                    f[16 + row * 4 + col] = before[row, col];
                }
            for (int s = 0; s < m.subMeshCount && n < MaxDraws; s++)
            {
                if (m.count[s] == 0 || !OpaqueSubmesh(materials, s, m.subMeshCount)) continue;
                byte* d = output + n * DrawStride;
                *(long*)(d + 0) = e.id;
                *(long*)(d + 8) = m.vertexBuffer.ToInt64();
                *(long*)(d + 16) = m.indexBuffer.ToInt64();
                *(uint*)(d + 24) = m.start[s];
                *(uint*)(d + 28) = m.count[s];
                *(int*)(d + 32) = m.baseVertex[s];
                *(uint*)(d + 36) = DrawRigid | (m.index32 ? 1u : 0u);
                float* o = (float*)(d + 40);
                o[0] = 0f; o[1] = 0f; o[2] = 0f;
                *(uint*)(d + 52) = 0;                    // native: buffer size / vertex count
                *(uint*)(d + 56) = (uint)m.vertexCount;
                *(uint*)(d + 60) = (uint)(index + 1);    // 1-based matrix pair
                n++;
            }
        }
        rigidMoved = moved;
        rigidSeeThrough = seeThrough;
        rigidTicks += System.Diagnostics.Stopwatch.GetTimestamp() - started;
        if (++rigidFrames % 900 == 0)
        {
            Logger.LogInfo("VaM DLSS bridge: rigid motion, " + rigids.Count + " watched, " + moved + " moved this frame (" + rigidMatrixCount +
                           " drawn, " + seeThrough + " see-through skipped), " +
                           (rigidTicks * 1e6 / System.Diagnostics.Stopwatch.Frequency / 900).ToString("0") + " us/frame on average");
            rigidTicks = 0;
        }
        if (!rigidLogged && rigidMatrixCount > 0)
        {
            rigidLogged = true;
            Logger.LogInfo("VaM DLSS bridge: rigid motion for " + rigidMatrixCount + " moving MeshRenderer(s) (" + rigids.Count + " watched)");
        }
        return n;
    }

    // ---- skinned motion: animated CUAs -------------------------------------

    public static void CuaLoaded(Component __instance)
    {
        if (__instance != null) loadedCuas.Add(__instance);
    }

    private void SetupSkinnedMotion()
    {
        try
        {
            // The native side reads BoneWeight as float weight[4], int bone[4].
            if (Marshal.SizeOf(typeof(BoneWeight)) != 32 || Marshal.OffsetOf(typeof(BoneWeight), "m_Weight0").ToInt32() != 0 ||
                Marshal.OffsetOf(typeof(BoneWeight), "m_BoneIndex0").ToInt32() != 16)
                throw new NotSupportedException("BoneWeight is not float[4] then int[4]");
            Type loader = Type.GetType("CustomUnityAssetLoader, Assembly-CSharp", true);
            PatchAll(new Harmony("local.vam.dlssbridge.skinned"), loader, "DoLoadedCallbacks",
                typeof(VaMDLSSBridge).GetMethod("CuaLoaded", BindingFlags.Public | BindingFlags.Static));
            // The CUAs loaded before the hook: the only search.
            UnityEngine.Object[] found = Resources.FindObjectsOfTypeAll(loader);
            for (int i = 0; i < found.Length; i++)
            {
                Component c = found[i] as Component;
                if (c != null && c.gameObject.scene.IsValid()) CuaLoaded(c);
            }
            skinnedReady = true;
            Logger.LogInfo("VaM DLSS bridge: skinned motion ready (SkinnedMeshRenderers of CUAs, from CustomUnityAssetLoader.DoLoadedCallbacks; " +
                           loadedCuas.Count + " CUA(s) so far)");
        }
        catch (Exception e)
        {
            skinnedUnavailable = true;
            Logger.LogWarning("VaM DLSS bridge: skinned motion unavailable, animated CUAs keep camera motion: " + e.Message);
        }
    }

    // The SkinnedMeshRenderers under the CUAs that loaded since the last frame.
    private void TakeLoadedCuas()
    {
        for (int i = 0; i < loadedCuas.Count; i++)
        {
            Component c = loadedCuas[i];
            if (c == null) continue;
            SkinnedMeshRenderer[] found = c.GetComponentsInChildren<SkinnedMeshRenderer>(true);
            for (int k = 0; k < found.Length; k++)
            {
                int id = found[k].GetInstanceID();
                if (!skinnedIds.Add(id)) continue;
                SkinnedEntry e = new SkinnedEntry();
                e.renderer = found[k];
                e.id = id;
                skinneds.Add(e);
            }
        }
        loadedCuas.Clear();
    }

    private SkinnedMesh SkinnedData(Mesh mesh, int frame)
    {
        SkinnedMesh d;
        if (skinnedMeshes.TryGetValue(mesh, out d)) { d.lastUsed = frame; return d; }
        d = new SkinnedMesh();
        d.lastUsed = frame;
        skinnedMeshes[mesh] = d;
        if (!mesh.isReadable)
        {
            // Its vertices and bone weights stay on the GPU only.
            d.unusable = true;
            if (++unreadableLogs <= 20)
                Logger.LogInfo("VaM DLSS bridge: skinned mesh '" + mesh.name + "' is not readable; it keeps camera motion");
            return d;
        }
        d.positions = mesh.vertices;
        d.bindposes = mesh.bindposes;
        // Without bones Unity draws the mesh with the renderer's own matrix:
        // all-zero weights, which the native side gives to bone 0.
        d.weights = d.bindposes.Length > 0 ? mesh.boneWeights : new BoneWeight[d.positions.Length];
        if (d.bindposes.Length == 0) d.bindposes = new Matrix4x4[] { Matrix4x4.identity };
        if (d.positions.Length == 0 || d.weights.Length != d.positions.Length) { d.unusable = true; return d; }
        d.positionHandle = GCHandle.Alloc(d.positions, GCHandleType.Pinned);
        d.weightHandle = GCHandle.Alloc(d.weights, GCHandleType.Pinned);
        return d;
    }

    // The renderer's bones for its current mesh; false if it cannot be skinned here.
    private bool BindSkinned(SkinnedEntry e, Mesh mesh, int frame)
    {
        e.mesh = mesh;
        e.previousFrame = -2;
        e.materials = null;
        e.data = SkinnedData(mesh, frame);
        e.unusable = true;
        if (e.data.unusable) return false;
        int count = e.data.bindposes.Length;
        Transform[] bones = e.renderer.bones;
        if (bones.Length == 0) bones = new Transform[] { e.renderer.transform };
        if (bones.Length != count) return false;   // Unity does not draw such a renderer either
        for (int b = 0; b < count; b++) if (bones[b] == null) return false;
        e.bones = bones;
        e.current = new Matrix4x4[count];
        e.previous = new Matrix4x4[count];
        e.unusable = false;
        return true;
    }

    private void SweepSkinned(int frame)
    {
        if (skinnedDirty)
        {
            skinnedDirty = false;
            skinneds.RemoveAll(delegate(SkinnedEntry e) { return e.renderer == null; });
            skinnedIds.Clear();
            for (int i = 0; i < skinneds.Count; i++) skinnedIds.Add(skinneds[i].id);
        }
        if (frame - lastSkinnedSweep < CacheFrames) return;
        lastSkinnedSweep = frame;
        staleSkinned.Clear();
        foreach (KeyValuePair<Mesh, SkinnedMesh> p in skinnedMeshes)
            if (frame - p.Value.lastUsed > CacheFrames) staleSkinned.Add(p.Key);
        for (int i = 0; i < staleSkinned.Count; i++)
        {
            SkinnedMesh d = skinnedMeshes[staleSkinned[i]];
            // The native side copied them the first time it drew the mesh, frames ago.
            if (d.positionHandle.IsAllocated) d.positionHandle.Free();
            if (d.weightHandle.IsAllocated) d.weightHandle.Free();
            d.freed = true;
            skinnedMeshes.Remove(staleSkinned[i]);
        }
    }

    // Every watched renderer's skinning matrices each frame; the visible ones
    // whose bones moved since the previous frame go to the native side with
    // both frames' matrices, after the rigid renderers' pairs.
    private unsafe int AddSkinneds(int mask, int frame, byte* output, int n, byte* matrices)
    {
        long started = System.Diagnostics.Stopwatch.GetTimestamp();
        if (loadedCuas.Count > 0) TakeLoadedCuas();
        int moved = 0, seeThrough = 0;
        for (int i = 0; i < skinneds.Count; i++)
        {
            SkinnedEntry e = skinneds[i];
            SkinnedMeshRenderer r = e.renderer;
            if (r == null) { skinnedDirty = true; continue; }
            if (!r.enabled || !r.isVisible || (mask & (1 << r.gameObject.layer)) == 0) continue;
            Mesh mesh = r.sharedMesh;
            if (mesh == null) continue;
            // Out of view long enough, its mesh's data was swept (freed).
            if (mesh != e.mesh || e.data.freed) BindSkinned(e, mesh, frame);
            e.data.lastUsed = frame;
            if (e.unusable) continue;
            Matrix4x4[] swap = e.previous;
            e.previous = e.current;
            e.current = swap;
            bool history = e.previousFrame == frame - 1, changed = false;
            e.previousFrame = frame;
            Matrix4x4[] bindposes = e.data.bindposes;
            for (int b = 0; b < e.bones.Length; b++)
            {
                Transform bone = e.bones[b];
                if (bone == null) { e.mesh = null; history = false; break; }   // bound again next frame
                Matrix4x4 skin = bone.localToWorldMatrix * bindposes[b];
                e.current[b] = skin;
                if (history && !changed && !SameMatrix(ref skin, ref e.previous[b])) changed = true;
            }
            if (!history || !changed) continue;
            moved++;
            int bones = e.bones.Length;
            if (n >= MaxDraws || rigidMatrixCount + bones > MaxRigidMatrices) continue;
            MeshEntry m = MeshFor(mesh, frame, false);
            if (m == null) continue;
            Material[] materials = MaterialsOf(r, ref e.materials);
            if (SeeThrough(r, materials, m.subMeshCount, "skinned")) { seeThrough++; continue; }
            int first = rigidMatrixCount;
            rigidMatrixCount += bones;
            for (int b = 0; b < bones; b++)
            {
                float* f = (float*)(matrices + (first + b) * RigidMatrixStride);
                Matrix4x4 now = e.current[b], before = e.previous[b];
                for (int row = 0; row < 4; row++)
                    for (int col = 0; col < 4; col++)
                    {
                        f[row * 4 + col] = now[row, col];
                        f[16 + row * 4 + col] = before[row, col];
                    }
            }
            for (int s = 0; s < m.subMeshCount && n < MaxDraws; s++)
            {
                if (m.count[s] == 0 || !OpaqueSubmesh(materials, s, m.subMeshCount)) continue;
                byte* d = output + n * DrawStride;
                *(long*)(d + 0) = mesh.GetInstanceID();
                *(long*)(d + 8) = e.data.positionHandle.AddrOfPinnedObject().ToInt64();
                *(long*)(d + 16) = m.indexBuffer.ToInt64();
                *(uint*)(d + 24) = m.start[s];
                *(uint*)(d + 28) = m.count[s];
                *(int*)(d + 32) = m.baseVertex[s];
                *(uint*)(d + 36) = DrawSkinned | (m.index32 ? 1u : 0u);
                *(long*)(d + 40) = e.data.weightHandle.AddrOfPinnedObject().ToInt64();
                *(uint*)(d + 48) = (uint)bones;
                *(uint*)(d + 52) = 0;
                *(uint*)(d + 56) = (uint)e.data.positions.Length;
                *(uint*)(d + 60) = (uint)(first + 1);    // 1-based first bone pair
                n++;
            }
        }
        SweepSkinned(frame);
        skinnedMoved = moved;
        skinnedSeeThrough = seeThrough;
        skinnedTicks += System.Diagnostics.Stopwatch.GetTimestamp() - started;
        if (++skinnedFrames % 900 == 0)
        {
            Logger.LogInfo("VaM DLSS bridge: skinned motion, " + skinneds.Count + " watched, " + moved + " moved this frame (" +
                           seeThrough + " see-through skipped), " +
                           skinnedMeshes.Count + " meshes, " + (skinnedTicks * 1e6 / System.Diagnostics.Stopwatch.Frequency / 900).ToString("0") +
                           " us/frame on average");
            skinnedTicks = 0;
        }
        if (!skinnedLogged && moved > 0)
        {
            skinnedLogged = true;
            Logger.LogInfo("VaM DLSS bridge: skinned motion for " + moved + " moving SkinnedMeshRenderer(s) (" + skinneds.Count + " watched)");
        }
        return n;
    }

    // Consumes everything VaM drew since the previous main-camera render.
    private unsafe int CollectDraws(Camera cam, byte* output, byte* matrices)
    {
        int n = 0;
        int frame = Time.frameCount, mask = cam.cullingMask;
        drawnThisFrame.Clear();
        try
        {
            for (int i = 0; i < skinCalls.Count && n < MaxDraws; i++) n = AddSkin(skinCalls[i], mask, frame, output, n);
            for (int i = 0; i < wrapCalls.Count && n < MaxDraws; i++) n = AddWrap(wrapCalls[i], wrapNoDelay[i], mask, frame, output, n);
            if (!motionLogged && n > 0)
            {
                motionLogged = true;
                Logger.LogInfo("VaM DLSS bridge: object motion for " + skinCalls.Count + " skin and " + wrapCalls.Count + " clothing draws (" + n + " submeshes)");
            }
            SweepCaches(frame);
        }
        catch (Exception e)
        {
            n = 0;
            motionUnavailable = true;
            collecting = false;
            Logger.LogWarning("VaM DLSS bridge: object motion stopped, camera motion only: " + e.Message);
        }
        skinCalls.Clear();
        wrapCalls.Clear();
        wrapNoDelay.Clear();
        if (collectingHair)
        {
            try
            {
                SweepHair(frame);
                for (int i = 0; i < hairs.Count && n < MaxDraws; i++) n = AddHair(hairs[i], mask, frame, output, n);
            }
            catch (Exception e)
            {
                hairUnavailable = true;
                collectingHair = false;
                Logger.LogWarning("VaM DLSS bridge: hair motion stopped, hair keeps camera motion: " + e.Message);
            }
        }
        rigidMatrixCount = 0;
        if (collectingRigid)
        {
            try { n = AddRigids(mask, frame, output, n, matrices); }
            catch (Exception e)
            {
                rigidUnavailable = true;
                collectingRigid = false;
                rigidMatrixCount = 0;
                Logger.LogWarning("VaM DLSS bridge: rigid motion stopped, props keep camera motion: " + e.Message);
            }
        }
        if (collectingSkinned && collectingRigid)
        {
            int before = n, pairs = rigidMatrixCount;
            try { n = AddSkinneds(mask, frame, output, n, matrices); }
            catch (Exception e)
            {
                n = before;
                rigidMatrixCount = pairs;
                skinnedUnavailable = true;
                collectingSkinned = false;
                Logger.LogWarning("VaM DLSS bridge: skinned motion stopped, animated CUAs keep camera motion: " + e.Message);
            }
        }
        return n;
    }

    // PT overlays: event-driven registry. No periodic scene/canvas search.
    private const int MaxPathUi=4096,PathUiStride=80;
    private sealed class UiEntry {public Graphic graphic;public Vector3[] previous=new Vector3[3];public int frame=-1;public bool screen;}
    private static readonly Dictionary<Graphic,UiEntry> pathUi=new Dictionary<Graphic,UiEntry>();
    private readonly Vector3[] uiCorners=new Vector3[4];
    private IntPtr uiSlots;private int lastUiUse=-100;
    // UI drawn with ZTest Always is on top of everything in raster (VaM's desktop menus: overlayUI swaps the
    // default UI material to UI/Default-Overlay). The PT composite drops depth-tested (flag 0) UI wherever a
    // path-traced surface is nearer than the panel, so these must be sent as on-top (flag 1) like VR's menus.
    // Test switch: false = 2.9.2 behaviour (canvas camera decides).
    public static bool PathUiAlwaysShadersOnTop=true;
    private static readonly Dictionary<Shader,bool> uiShaderOnTop=new Dictionary<Shader,bool>();
    private bool UiAlwaysOnTop(Graphic graphic) {
        if(!PathUiAlwaysShadersOnTop)return false;
        Material material=graphic.material;Shader shader=material!=null?material.shader:null;
        if(shader==null)return false;
        bool onTop;
        if(uiShaderOnTop.TryGetValue(shader,out onTop))return onTop;
        string name=shader.name;
        onTop=name=="UI/Default-Overlay" || name=="GUI/Text Shader Overlay" || name=="Custom/TransparentHUD" || name=="Unlit/UnlitOverlayShader";
        uiShaderOnTop[shader]=onTop;
        Logger.LogInfo("VaM DLSS bridge: PT UI shader '"+name+"' "+(onTop?"is ZTest Always, kept on top of the path-traced scene":"is depth-tested against the path-traced scene"));
        return onTop;
    }
    private static bool IsVrMainHud(Graphic graphic) {
        for(Transform t=graphic!=null?graphic.transform:null;t!=null;t=t.parent)
            if(t.name=="MainUICanvas")return true;
        return false;
    }
    // ---- 11-09: the scene's image effects under PT ----
    // Scenes grade through Unity's PostProcessing v1 (MacGruber's PostMagic: bloom, LUT, vignette, grain, depth of field). With RR
    // the frame is RR's output, so effects drawn on the camera's image were lost. Each effect now takes RR's output, at output
    // size, and hands its result back (the eyes side by side, as on the camera's stereo image). The effects are a layer of their
    // own on the picture: the look they end at is not what PT's lighting is judged against (user, 11-09).
    private static VaMDLSSBridge postOwner;
    private static IntPtr postEvent;
    private static CommandBuffer postCommand;
    public static bool PostOnRr=true;              // test: false = the installed behaviour
    public static int PostOnRrRuns;                // test: frames run on RR's output
    public static string PostTest(bool on) { PostOnRr=on; return "on="+on+" runs="+PostOnRrRuns+" event="+(postEvent!=IntPtr.Zero); }
    public static bool PostDofInVr;                // test: true = depth of field also in VR
    public static string PostVrDof(bool on) { PostDofInVr=on; return "vr dof="+on; }
    private static readonly int cameraDepthId=Shader.PropertyToID("_CameraDepthTexture");
    private static FieldInfo postProfile,postDof;
    private static PropertyInfo postDofEnabled;
    private sealed class PostSwap { public RenderTexture source,destination,a,b,depth; public object dofOff; }
    [DllImport("openvr_api")] private static extern IntPtr VaMDLSS_GetPostEventFunc();
    [DllImport("openvr_api")] private static extern int VaMDLSS_GetPostSize(out int outputWidth,out int outputHeight,out int inputWidth,out int inputHeight);
    private void SetupPostEffects() {
        try {
            Type type=AccessTools.TypeByName("UnityEngine.PostProcessing.PostProcessingBehaviour");
            if(type==null)return;
            postEvent=VaMDLSS_GetPostEventFunc();postOwner=this;
            postCommand=new CommandBuffer{name="VaM image effects on RR"};
            postProfile=type.GetField("profile",BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic);
            new Harmony("local.vam.dlssbridge.posteffects").Patch(type.GetMethod("OnRenderImage",BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic),
                new HarmonyMethod(typeof(VaMDLSSBridge).GetMethod("PostEffectsBefore")),new HarmonyMethod(typeof(VaMDLSSBridge).GetMethod("PostEffectsAfter")),null,null);
        } catch(Exception e){postEvent=IntPtr.Zero;Logger.LogWarning("Image effects on RR unavailable: "+e.Message);}
    }
    // The profile's depth of field when it is on, else null.
    private static object DepthOfFieldOn(object behaviour) {
        try {
            object profile=postProfile.GetValue(behaviour);
            if(profile==null)return null;
            if(postDof==null)postDof=profile.GetType().GetField("depthOfField");
            object model=postDof.GetValue(profile);
            if(postDofEnabled==null)postDofEnabled=model.GetType().GetProperty("enabled");
            return postDofEnabled!=null && (bool)postDofEnabled.GetValue(model,null)?model:null;
        } catch(Exception){return null;}
    }
    public static void PostEffectsBefore(MonoBehaviour __instance,ref RenderTexture __0,ref RenderTexture __1,out object __state) {
        __state=null;
        VaMDLSSBridge self=postOwner;
        if(!PostOnRr || postEvent==IntPtr.Zero || self==null || self.target==null || __0==null || __instance.gameObject!=self.target.gameObject)return;
        if((NativeState()&StatePathRendering)==0)return;
        int ow,oh,iw,ih;
        if(VaMDLSS_GetPostSize(out ow,out oh,out iw,out ih)==0)return;
        RenderTextureDescriptor d=__0.descriptor;d.width=ow;d.height=oh;d.depthBufferBits=0;d.msaaSamples=1;d.useMipMap=false;d.autoGenerateMips=false;
        PostSwap swap=new PostSwap{source=__0,destination=__1,a=RenderTexture.GetTemporary(d),b=RenderTexture.GetTemporary(d)};
        postCommand.Clear();
        postCommand.SetRenderTarget(swap.a);
        postCommand.IssuePluginEvent(postEvent,1);
        // Depth of field reads the camera's depth, and PT's camera draws no geometry into Unity's depth pass.
        // VR has none: the headset does not know where the eye looks, and the lens was chosen for a 20-35 degree monitor view,
        // so at 110 degrees it blurs nearly everything but the person. Off for this draw only, the plugin's own state stays.
        object dof=DepthOfFieldOn(__instance);
        if(dof!=null && !Desktop && !PostDofInVr) { postDofEnabled.SetValue(dof,false,null); swap.dofOff=dof; dof=null; }
        if(dof!=null && iw>0 && ih>0) {
            swap.depth=RenderTexture.GetTemporary(iw,ih,0,RenderTextureFormat.RFloat,RenderTextureReadWrite.Linear);
            swap.depth.filterMode=FilterMode.Point;
            postCommand.SetRenderTarget(swap.depth);
            postCommand.IssuePluginEvent(postEvent,3);
        }
        Graphics.ExecuteCommandBuffer(postCommand);
        if(swap.depth!=null)Shader.SetGlobalTexture(cameraDepthId,swap.depth);
        __0=swap.a;__1=swap.b;__state=swap;PostOnRrRuns++;
    }
    public static void PostEffectsAfter(object __state) {
        PostSwap swap=__state as PostSwap;
        if(swap==null)return;
        if(swap.dofOff!=null)postDofEnabled.SetValue(swap.dofOff,true,null);
        postCommand.Clear();
        postCommand.SetRenderTarget(swap.b);
        postCommand.IssuePluginEvent(postEvent,2);
        Graphics.ExecuteCommandBuffer(postCommand);
        Graphics.Blit(swap.source,swap.destination);
        RenderTexture.ReleaseTemporary(swap.a);RenderTexture.ReleaseTemporary(swap.b);
        if(swap.depth!=null)RenderTexture.ReleaseTemporary(swap.depth);
    }
    private void SetupPathUi() {
        try {
            Harmony h=new Harmony("local.vam.dlssbridge.pathui");
            BindingFlags methods=BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic;
            h.Patch(typeof(Graphic).GetMethod("OnEnable",methods),null,new HarmonyMethod(typeof(VaMDLSSBridge).GetMethod("PathUiEnabled")),null,null);
            h.Patch(typeof(Graphic).GetMethod("OnDisable",methods),null,new HarmonyMethod(typeof(VaMDLSSBridge).GetMethod("PathUiDisabled")),null,null);
            foreach(Graphic graphic in Resources.FindObjectsOfTypeAll<Graphic>())if(graphic.isActiveAndEnabled)PathUiEnabled(graphic);
        } catch(Exception e){Logger.LogWarning("PT UI registry unavailable: "+e.Message);}
    }
    public static void PathUiEnabled(Graphic __instance) {
        if(__instance!=null && !pathUi.ContainsKey(__instance))pathUi[__instance]=new UiEntry{graphic=__instance};
    }
    public static void PathUiDisabled(Graphic __instance) {if(!ReferenceEquals(__instance,null))pathUi.Remove(__instance);}
    private unsafe void CollectPathUi(Camera cam,byte* frame,int slotIndex,bool reset) {
        int pathState=NativeState();
        bool pathRendering=(pathState&StatePathRendering)!=0;
        bool vrPtUiOnTop=!Desktop && pathRendering;
        if(!pathRendering || pathUi.Count==0) {
            if(uiSlots!=IntPtr.Zero && Time.frameCount-lastUiUse>SlotCount+2){Marshal.FreeHGlobal(uiSlots);uiSlots=IntPtr.Zero;}
            return;
        }
        bool vrMainHudFrame=!Desktop && (NativeState()&StatePathRendering)!=0;
        byte* output=null;int n=0;
        foreach(UiEntry entry in pathUi.Values) {
            Graphic graphic=entry.graphic;
            if(graphic==null || !graphic.isActiveAndEnabled || graphic.canvas==null)continue;
            Canvas canvas=graphic.canvas.rootCanvas;
            if(!canvas.isActiveAndEnabled)continue;
            bool screen=canvas.renderMode!=RenderMode.WorldSpace;
            if(screen && canvas.worldCamera!=null && !canvas.worldCamera.isActiveAndEnabled)continue;
            int layer=1<<graphic.gameObject.layer;
            bool mainSees=(cam.cullingMask&layer)!=0;
            bool canvasSees=canvas.worldCamera!=null && canvas.worldCamera.isActiveAndEnabled && (canvas.worldCamera.cullingMask&layer)!=0;
            if(canvas.renderMode!=RenderMode.ScreenSpaceOverlay && !mainSees && !canvasSees)continue;
            CanvasRenderer renderer=graphic.canvasRenderer;
            // Unity 2018 has no GetInheritedAlpha. Final per-pixel replay also
            // rejects invisible CanvasGroup content when assigning UI guides.
            if(renderer.cull || graphic.color.a*renderer.GetAlpha()<=0)continue;
            if(n==MaxPathUi){Logger.LogError("PT UI coverage exceeds "+MaxPathUi+" active elements");break;}
            if(n==0) {
                if(uiSlots==IntPtr.Zero)uiSlots=Marshal.AllocHGlobal(MaxPathUi*PathUiStride*SlotCount);
                output=(byte*)uiSlots.ToPointer()+slotIndex*MaxPathUi*PathUiStride;
            }
            graphic.rectTransform.GetWorldCorners(uiCorners);
            bool valid=!reset && entry.frame==Time.frameCount-1 && entry.screen==screen;
            float* p=(float*)(output+n*PathUiStride);
            // Unity: bottom-left, top-left, top-right, bottom-right.
            for(int k=0;k<3;k++) {
                Vector3 current=uiCorners[k==0?0:k==1?3:1];
                if(screen) {
                    if(canvas.worldCamera!=null) {
                        current=canvas.worldCamera.WorldToViewportPoint(current);Rect rect=canvas.worldCamera.rect;
                        current.x=rect.x+current.x*rect.width;current.y=rect.y+current.y*rect.height;
                    } else {current.x/=Math.Max(Screen.width,1);current.y/=Math.Max(Screen.height,1);}
                    current.z=.999f;
                }
                Vector3 previous=valid?entry.previous[k]:current;
                p[k*3]=current.x;p[k*3+1]=current.y;p[k*3+2]=current.z;
                p[9+k*3]=previous.x;p[10+k*3]=previous.y;p[11+k*3]=previous.z;entry.previous[k]=current;
            }
            // VaM's VR HUD is a world-space Canvas rather than ScreenSpaceOverlay. It is the main interface panel
            // (SceneAtoms/.../HUD/MainUICanvas) and should cover the path-traced scene like the desktop UI/Default-Overlay.
            // Only that canvas is promoted; scene-owned world UI keeps its depth test and the desktop route is unchanged.
            bool vrMainHud=vrMainHudFrame && IsVrMainHud(graphic);
            bool vrCanvasOnTop=vrPtUiOnTop;
            *(uint*)(output+n*PathUiStride+72)=screen?3u:canvas.worldCamera!=null && canvas.worldCamera!=cam || UiAlwaysOnTop(graphic) || vrMainHud || vrCanvasOnTop?1u:0u;
            *(int*)(output+n*PathUiStride+76)=canvas.GetInstanceID();entry.frame=Time.frameCount;entry.screen=screen;n++;
        }
        *(long*)(frame+980)=n>0?(long)output:0;*(int*)(frame+988)=n;*(int*)(frame+992)=PathUiStride;
        if(n>0){*(uint*)(frame+976)|=4u;lastUiUse=Time.frameCount;}
        else if(uiSlots!=IntPtr.Zero && Time.frameCount-lastUiUse>SlotCount+2){Marshal.FreeHGlobal(uiSlots);uiSlots=IntPtr.Zero;}
    }

    // ---- render scale, native frames, menus ------------------------------

    // Before rendering: the render scale to use this frame, and setting it.
    private void LateUpdate()
    {
        if (!nativeReady || nativeAbsent || target == null) return;
        int state = NativeState();
        CameraEvent desiredCapture = (state&StatePathRendering)!=0?CameraEvent.BeforeForwardOpaque:CameraEvent.AfterForwardAlpha;
        if (desiredCapture != CaptureCameraEvent) {
            if (target != null) target.RemoveCommandBuffer(CaptureCameraEvent, command);
            CaptureCameraEvent = desiredCapture;
            pathCaptureOrdered=false;
            if (target != null) target.AddCommandBuffer(CaptureCameraEvent, command);
            havePrevious = false;
        }
        bool failed = (state & StateFailed) != 0;
        int percent = -1;
        if (!scaleUnavailable)
        {
            try { percent = VaMDLSS_GetRenderScalePercent(); }
            catch (Exception e)
            {
                scaleUnavailable = true;
                Logger.LogInfo("VaM DLSS bridge: this openvr_api.dll has no live render scale (" + e.GetType().Name + ")");
            }
        }
        float now = Time.realtimeSinceStartup;
        if (percent != seenPercent) { seenPercent = percent; seenSince = now; }
        if (settledPercent < 0 || (percent != settledPercent && now - seenSince >= ScaleSettleSeconds)) settledPercent = percent;
        bool menus = !failed && (state & StateMenuNative) != 0 && MenusOpen();
        if (menus != menusOpen)
        {
            menusOpen = menus;
            if (++menuLogs <= 40 || menuLogs % 100 == 0)
                Logger.LogInfo("VaM DLSS bridge: VaM menus " + (menus ? "open: rendering at 100%, no DLSS SR" : "closed") + " (" + menuLogs + ")");
        }
        bool off = !failed && (state & StateSrOff) != 0;
        if (off != srOff)
        {
            srOff = off;
            Logger.LogInfo("VaM DLSS bridge: " + (off ? "SR off (DLSSPreset=Off): native frames at the output size" : "SR on"));
        }
        int percentNow = settledPercent < 0 ? -1 : menus || off ? 100 : settledPercent;
        if (percentNow > 0) ApplyScale(percentNow);
        wantNative = percentNow > 0 && (menus || off);
        wantMenuResized = menus && !off && settledPercent < 100;
    }

    private void ApplyScale(int percent)
    {
        if (Desktop) { ApplyDesktopScale(percent); return; }
        float factor;
        try { factor = VaMDLSS_GetEyeTextureScale(percent); }
        catch (Exception) { scaleUnavailable = true; return; }
        if (!(factor > 0f)) return;                         // VaM has not asked for its size yet
        float vam = VaMRenderScale();
        float want = vam * factor, before = XRSettings.eyeTextureResolutionScale;
        if (Math.Abs(before - want) <= 1e-4f) { appliedPercent = percent; return; }
        // A size change replaces reconstruction targets, not the PT scene.
        // Reconstruct synchronously ensures the new size before dispatching it;
        // invalidating the consumer here tore down every PT material and hair cache.
        havePrevious=false;
        XRSettings.eyeTextureResolutionScale = want;
        scaledByBridge = Math.Abs(factor - 1f) > 1e-4f;
        // The same percent at another factor: the output size (OutputScale) changed.
        if (++scaleLogs <= 40 || scaleLogs % 100 == 0)
            Logger.LogInfo("VaM DLSS bridge: render scale " + percent + "% of the output" + (percent == appliedPercent ? " (output size changed)" : "") +
                           " (eye texture scale " + before.ToString("0.000") + " -> " + want.ToString("0.000") + ", VaM's own " + vam.ToString("0.00") + ")");
        appliedPercent = percent;
    }

    private void RetargetDesktopCameras(RenderTexture old, RenderTexture replacement)
    {
        if (old == null) return;
        // Camera.CopyFrom copies targetTexture too. PostMagic's UI CameraHook
        // copies the main camera once, so it otherwise keeps our destroyed RT
        // after a resize (Unity 2018 crashes on its render thread). Move every
        // scene camera sharing our owned target before destroying it. Include
        // inactive cameras: a later enable must not resurrect the old target.
        // This discovery runs only on target replacement/detach, never per frame.
        Camera[] cameras = Resources.FindObjectsOfTypeAll<Camera>();
        int moved = 0;
        for (int i = 0; i < cameras.Length; i++)
        {
            Camera camera = cameras[i];
            if (camera == null || !camera.gameObject.scene.IsValid() || camera.targetTexture != old) continue;
            camera.targetTexture = replacement;
            if (camera != target) moved++;
        }
        if (moved != 0) Logger.LogInfo("VaM DLSS bridge: desktop target shared by " + moved + " other camera(s); rebound before release");
    }

    private void ApplyDesktopScale(int percent)
    {
        if (target == null || desktopOutput == null) return;
        int ow, oh;
        VaMDLSS_GetDesktopSize(out ow, out oh);
        // Screen is the bootstrap and resize source until the next bound RTV
        // confirms the backbuffer. Never apply VR OutputScale on desktop.
        if (ow <= 0 || oh <= 0 || ow != Screen.width || oh != Screen.height) { ow = Screen.width; oh = Screen.height; }
        int w = Math.Max(4, (ow * percent + 50) / 100), h = Math.Max(4, (oh * percent + 50) / 100);
        desktopOutput.depth = target.depth + 0.001f;
        desktopOutput.targetDisplay = target.targetDisplay;
        if (desktopUiField == null) desktopUiField = Field(Type.GetType("MeshVR.ScreenUICamera, Assembly-CSharp", true), "screenUICamera", true);
        Camera screenUi = desktopUiField.GetValue(null) as Camera;
        // VaM's ScreenUICamera keeps the preceding screen camera's colour.
        // Both must use the same HDR screen buffer; forcing this camera to LDR
        // leaves the UI camera's HDR buffer with ScreenCameraClear's black.
        desktopOutput.allowHDR = screenUi != null ? screenUi.allowHDR : target.allowHDR;
        if (desktopTarget != null && desktopTarget.width == w && desktopTarget.height == h) return;
        // Keep the PT scene resident, as on the stereo path. The render thread
        // drains the old reconstruction targets before rebuilding them at w/h.
        RenderTexture old = desktopTarget;
        // The camera's HDR effects render into their HDR intermediate; this is
        // their final, sRGB colour texture, just as Unity's eye texture is.
        desktopTarget = new RenderTexture(w, h, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
        desktopTarget.name = "VaM Desktop Input";
        desktopTarget.antiAliasing = 1;
        desktopTarget.Create();
        RetargetDesktopCameras(old, desktopTarget);
        target.targetTexture = desktopTarget;
        if (old != null) Destroy(old);
        havePrevious = false;
        appliedPercent = percent;
        Logger.LogInfo("VaM DLSS bridge: desktop input " + w + "x" + h + " -> window " + ow + "x" + oh);
    }

    // VaM's own render scale (UserPreferences.renderScale), which multiplies ours.
    private float VaMRenderScale()
    {
        if (!prefsAccessFailed)
        {
            try
            {
                if (prefsScale == null)
                {
                    Type t = Type.GetType("UserPreferences, Assembly-CSharp");
                    if (t != null)
                    {
                        prefsSingleton = t.GetField("singleton", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
                        prefsScale = t.GetProperty("renderScale", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
                    }
                    if (prefsSingleton == null || prefsScale == null) throw new MissingMemberException("UserPreferences.singleton / renderScale");
                }
                object prefs = prefsSingleton.GetValue(null);
                if (prefs != null)
                {
                    float v = (float)prefsScale.GetValue(prefs, null);
                    if (v > 0.01f) return v;
                }
            }
            catch (Exception e)
            {
                prefsAccessFailed = true;
                Logger.LogWarning("VaM DLSS bridge: VaM's render scale is unreadable (" + e.Message + "); taking the one Unity had at first");
            }
        }
        // Read before the bridge first changed it (ApplyScale asks first).
        if (vamScaleFallback < 0f) vamScaleFallback = XRSettings.eyeTextureResolutionScale;
        return vamScaleFallback;
    }

    // VaM's main HUD, or its world UI (start screen, scene browser).
    private bool MenusOpen()
    {
        if (menuAccessFailed) return false;
        try
        {
            if (superSingleton == null)
            {
                Type t = Type.GetType("SuperController, Assembly-CSharp");
                if (t != null)
                {
                    superSingleton = t.GetProperty("singleton", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
                    hudVisible = t.GetProperty("MainHUDVisible", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
                    worldUiActive = t.GetProperty("worldUIActivated", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
                }
                if (superSingleton == null || hudVisible == null || worldUiActive == null)
                    throw new MissingMemberException("SuperController.singleton / MainHUDVisible / worldUIActivated");
            }
            object controller = superSingleton.GetValue(null, null);
            return controller != null && ((bool)hudVisible.GetValue(controller, null) || (bool)worldUiActive.GetValue(controller, null));
        }
        catch (Exception e)
        {
            menuAccessFailed = true;
            Logger.LogWarning("VaM DLSS bridge: VaM's menu state is unreadable (" + e.Message + "); menus keep DLSS SR");
            return false;
        }
    }

    // Native frames this frame. Leaving them, frames stay native while Unity
    // still renders at their size (it applies a new eye texture scale a frame
    // or two later): DLSS SR would otherwise build a feature for that size.
    private void UpdateNativeFrames()
    {
        int w = EyeWidth, h = EyeHeight;
        if (wantNative)
        {
            nativeFrame = true;
            menuResized = tailMenuResized = wantMenuResized;
            nativeWidth = w; nativeHeight = h; tailFrames = 0;
            return;
        }
        if (nativeFrame && w == nativeWidth && h == nativeHeight && (w != normalWidth || h != normalHeight) && ++tailFrames <= NativeTailFrames)
        {
            menuResized = tailMenuResized;
            return;
        }
        nativeFrame = menuResized = false;
        normalWidth = w; normalHeight = h;
    }

    // ---- per-frame camera handling ---------------------------------------

    private void PreCull(Camera cam)
    {
        if (nativeAbsent) return;
        if (!nativeReady)
        {
            if (Time.realtimeSinceStartup >= nextNativeProbe) ProbeNative();
            if (!nativeReady) return;
        }
        if (!IsTarget(cam) || lastPreCullFrame == Time.frameCount) return;
        lastPreCullFrame = Time.frameCount;
        int state = NativeState();
        CameraEvent desiredCapture = (state&StatePathRendering)!=0?CameraEvent.BeforeForwardOpaque:CameraEvent.AfterForwardAlpha;
        if (desiredCapture != CaptureCameraEvent) {
            if (target != null) target.RemoveCommandBuffer(CaptureCameraEvent, command);
            CaptureCameraEvent = desiredCapture;
            pathCaptureOrdered=false;
            if (target != null) target.AddCommandBuffer(CaptureCameraEvent, command);
            havePrevious = false;
        }
        bool failed = (state & StateFailed) != 0;
        EnforceMsaa(failed);
        bool wantMotion = !failed && (state & StateObjectMotion) != 0 && !motionUnavailable;
        if (wantMotion && !motionPatched) SetupObjectMotion();
        collecting = wantMotion && motionPatched;
        if (!collecting)
        {
            skinCalls.Clear();
            wrapCalls.Clear();
            wrapNoDelay.Clear();
        }
        bool wantHair = !failed && (state & StateHairMotion) != 0 && !hairUnavailable;
        if (wantHair && hairAccess == null) SetupHairMotion();
        collectingHair = wantHair && hairAccess != null;
        bool wantRigid = !failed && (state & StateRigidMotion) != 0 && !rigidUnavailable;
        if (wantRigid && rigidAccess == null) SetupRigidMotion();
        collectingRigid = wantRigid && rigidAccess != null;
        bool wantSkinned = collectingRigid && (state & StateSkinnedMotion) != 0 && !skinnedUnavailable;
        if (wantSkinned && !skinnedReady) SetupSkinnedMotion();
        collectingSkinned = wantSkinned && skinnedReady;
        UpdateNativeFrames();
        jitteredThisFrame = false;
        for (int e = 0; e < ViewCount; e++)
        {
            Camera.StereoscopicEye eye = (Camera.StereoscopicEye)e;
            if (Desktop) projection[e] = cam.projectionMatrix;
            else { cam.CopyStereoDeviceProjectionMatrixToNonJittered(eye); projection[e] = cam.GetStereoNonJitteredProjectionMatrix(eye); }
            rasterProjection[e] = projection[e];
        }
        if ((state & StateSelected) == 0 || failed || nativeFrame)
        {
            jitterX = jitterY = 0f;
            return;
        }
        int phases = 32;
        try { phases = Math.Max(8, VaMDLSS_GetJitterPhaseCount()); } catch (Exception) { }
        jitterIndex = (jitterIndex % phases) + 1;
        // Image content shift in upright eye pixels (x right, y down), in (-0.5, 0.5).
        jitterX = Halton(jitterIndex, 2) - 0.5f;
        jitterY = Halton(jitterIndex, 3) - 0.5f;
        float w = Math.Max(1, EyeWidth), h = Math.Max(1, EyeHeight);
        for (int e = 0; e < ViewCount; e++)
        {
            // OpenGL-convention projection with w = -z: NDC.x shifts by -dm02,
            // NDC.y by -dm12. Content right by jx px and down by jy px:
            Matrix4x4 p = projection[e];
            p.m02 -= 2f * jitterX / w;
            p.m12 += 2f * jitterY / h;
            rasterProjection[e] = p;
            if (Desktop) cam.projectionMatrix = p; else cam.SetStereoProjectionMatrix((Camera.StereoscopicEye)e, p);
        }
        jitteredThisFrame = true;
    }

    private bool CameraJumped()
    {
        Matrix4x4 now = view[0].inverse, before = previousView[0].inverse;
        Vector3 moved = (Vector3)now.GetColumn(3) - (Vector3)before.GetColumn(3);
        float angle = Vector3.Angle(now.GetColumn(2), before.GetColumn(2));
        return moved.magnitude > 1.0f || angle > 30f;
    }

    private static unsafe void WriteMatrix(byte* frame, int offset, Matrix4x4 m)
    {
        float* f = (float*)(frame + offset);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                f[r * 4 + c] = m[r, c];
    }

    private unsafe void PreRender(Camera cam)
    {
        if (!nativeReady || cam != target || target == null || lastPreRenderFrame == Time.frameCount) return;
        lastPreRenderFrame = Time.frameCount;
        // Every PreCull callback has now registered its command buffers. Put
        // PT after SkinTransmission's Begin at the same BeforeOpaque event,
        // including launch directly into PT and a newly selected camera.
        if(CaptureCameraEvent==CameraEvent.BeforeForwardOpaque&&!pathCaptureOrdered) {
            target.RemoveCommandBuffer(CaptureCameraEvent,command);
            target.AddCommandBuffer(CaptureCameraEvent,command);pathCaptureOrdered=true;
        }
        int w = EyeWidth, h = EyeHeight;
        for (int e = 0; e < ViewCount; e++) view[e] = Desktop ? cam.worldToCameraMatrix : cam.GetStereoViewMatrix((Camera.StereoscopicEye)e);
        bool reset = !havePrevious || previousFrame != Time.frameCount - 1 ||
                     w != previousEyeWidth || h != previousEyeHeight || CameraJumped();
        int slotIndex = Time.frameCount % SlotCount;
        IntPtr slot = new IntPtr(slots.ToInt64() + (long)slotIndex * FrameSize);
        byte* b = (byte*)slot.ToPointer();
        *(uint*)(b + 0) = FrameMagic;
        *(uint*)(b + 4) = FrameSize;
        *(uint*)(b + 8) = FrameVersion;
        *(uint*)(b + 12) = (uint)((reset ? 1 : 0) | (jitteredThisFrame ? 2 : 0)) |
                           (nativeFrame ? FlagNative : 0u) | (menuResized ? FlagMenuResized : 0u) |
                           (CaptureCameraEvent==CameraEvent.BeforeForwardOpaque?16u:0u);
        *(long*)(b + 16) = Time.frameCount;
        float* f = (float*)(b + 24);
        f[0] = jitteredThisFrame ? jitterX : 0f;
        f[1] = jitteredThisFrame ? jitterY : 0f;
        f[2] = reset ? 0f : previousJitterX;
        f[3] = reset ? 0f : previousJitterY;
        *(uint*)(b + 40) = (uint)w;
        *(uint*)(b + 44) = (uint)h;
        f = (float*)(b + 48);
        f[0] = Time.unscaledDeltaTime * 1000f;
        f[1] = cam.nearClipPlane;
        f[2] = cam.farClipPlane;
        f[3] = cam.fieldOfView;
        for (int e = 0; e < ViewCount; e++)
        {
            // clipToPrevClip = P' * V' * V^-1 * P^-1 (relative motion first for precision).
            Matrix4x4 clipToPrevClip = reset ? Matrix4x4.identity
                : previousProjection[e] * (previousView[e] * view[e].inverse) * projection[e].inverse;
            WriteMatrix(b, 64 + e * 64, clipToPrevClip);
            WriteMatrix(b, 192 + e * 64, projection[e]);
            WriteMatrix(b, 320 + e * 64, projection[e].inverse);
            WriteMatrix(b, 448 + e * 64, view[e].inverse);
            // Version 2: world-space clip transforms for the object motion pass.
            Matrix4x4 worldToClip = projection[e] * view[e];
            WriteMatrix(b, 576 + e * 64, (jitteredThisFrame ? rasterProjection[e] : projection[e]) * view[e]);
            WriteMatrix(b, 704 + e * 64, worldToClip);
            WriteMatrix(b, 832 + e * 64, reset ? worldToClip : previousProjection[e] * previousView[e]);
        }
        IntPtr drawSlot = new IntPtr(drawSlots.ToInt64() + (long)slotIndex * DrawStride * MaxDraws);
        IntPtr matrixSlot = new IntPtr(matrixSlots.ToInt64() + (long)slotIndex * RigidMatrixStride * MaxRigidMatrices);
        rigidMatrixCount = 0;
        int drawCount = collecting || collectingHair || collectingRigid ? CollectDraws(cam, (byte*)drawSlot.ToPointer(), (byte*)matrixSlot.ToPointer()) : 0;
        *(long*)(b + 960) = drawCount > 0 ? drawSlot.ToInt64() : 0L;
        *(int*)(b + 968) = drawCount;
        *(int*)(b + 972) = DrawStride;
        *(uint*)(b + 976) = (collecting ? 1u : 0u) | (collectingHair ? 2u : 0u) | (collectingRigid ? 8u : 0u) | (collectingSkinned ? 16u : 0u);
        // FrameExtension (UI rectangles at 980, nasolabial bands at 996): zero, both removed.
        for (int o = 980; o < FrameSize; o += 4) *(uint*)(b + o) = 0;
        CollectPathUi(cam,b,slotIndex,reset);
        // Rigid draws' matrix pairs (2.7.0) and skinned draws' bone pairs (2.8.0), see FrameExtension::rigidMatrices.
        int matrixCount = drawCount > 0 ? rigidMatrixCount : 0;
        *(long*)(b + 1008) = matrixCount > 0 ? matrixSlot.ToInt64() : 0L;
        *(int*)(b + 1016) = matrixCount;
        *(int*)(b + 1020) = RigidMatrixStride;
        *(uint*)(b + 1024) = (uint)ViewCount;
        *(uint*)(b + 1028) = 0;
        *(long*)(b + 1032) = Desktop && desktopTarget != null ? desktopTarget.GetNativeTexturePtr().ToInt64() : 0;
        command.Clear();
        command.IssuePluginEventAndData(renderEvent, CaptureEvent, slot);
        if (Desktop && desktopPresent != null) {
            desktopPresent.Clear();
            // Use the active screen colour target, including Unity's HDR
            // intermediate. Forcing CameraTarget here bypasses that buffer;
            // ScreenUICamera then resolves its still-black HDR colour over it.
            desktopPresent.IssuePluginEventAndData(renderEvent, 4, slot);
        }
        latePathCommand.Clear();
        if(CaptureCameraEvent==CameraEvent.BeforeForwardOpaque)latePathCommand.IssuePluginEventAndData(renderEvent,3,slot);
        opaqueDepthCommand.Clear();
        if (CaptureCameraEvent != CameraEvent.BeforeForwardOpaque) opaqueDepthCommand.IssuePluginEventAndData(renderEvent, OpaqueDepthEvent, slot);
        for (int e = 0; e < ViewCount; e++)
        {
            previousProjection[e] = projection[e];
            previousView[e] = view[e];
        }
        previousJitterX = jitteredThisFrame ? jitterX : 0f;
        previousJitterY = jitteredThisFrame ? jitterY : 0f;
        previousFrame = Time.frameCount;
        previousEyeWidth = w;
        previousEyeHeight = h;
        havePrevious = true;
    }

    private void PostRender(Camera cam)
    {
        if (cam != target || target == null || !jitteredThisFrame) return;
        if (Desktop) cam.ResetProjectionMatrix(); else cam.ResetStereoProjectionMatrices();
        jitteredThisFrame = false;
    }
}
