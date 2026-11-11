// VaM hair one-pass lighting (BepInEx 5, Unity 2018.1, C# 3).
//
// GPUTools hair is drawn whole once per pixel light that reaches it (Unity's ForwardBase + one ForwardAdd per further
// light). VaMHairOnePassNative.bin recognises those draws in the stereo eye camera's opaque pass; this plugin loads it
// early (before the game creates hair shaders, which it tags by bytecode), and marks the eye camera's opaque pass with
// render events at CameraEvent.BeforeForwardOpaque / AfterForwardOpaque, plus one event at the end of every frame.
//
// 2.0.1: with [General] Enabled (BepInEx\config\local.vam.haironepass.cfg, read at startup,
// default on) the hair items of the eye camera's opaque pass are drawn through a visibility buffer (strands): their
// points computed once per frame, the strands' quads rasterised in compute, the ForwardBase lighting and all ForwardAdd
// lights shaded in compatible full-target batches, the same picture as Unity's draws. What strands cannot take is drawn
// as 1.0.0 drew it (each item's ForwardAdd draws as one draw with all those lights, Unity's ForwardBase draw as it is).
// Off: nothing is loaded, VaM is unchanged.
//
// 2.1.0: strands add the light that passes through the fibre (Marschner's TT lobe, shaders\hair_strands.hlsl
// HairTransmission), so hair lit from behind glows; merge (the fallback) keeps 1.0.0's lighting.
//
// 2.1.1: native only (same picture): a strand quad too large for one thread (the camera close to the hair) is
// rasterised by a thread group sharing its lines, and quads wholly behind the eye are dropped.
//
// Test API (VaM test agent): Strands(on) / Merge(on) switch the mode (off: Unity's draws);
// Transmission(on) the lobe (on by default; off = 2.0.1's lighting);
// Observe(true) records which hair draws come in which order and how their constant buffers were filled; Report()
// returns the last frame as JSON (merge and strands statistics, the opaque pass's GPU time since ResetTiming());
// Snapshot(path) writes the eye camera's target at the end of the next opaque pass to a file; Capture(path) the hair
// draws of the next opaque pass as Unity issued them.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using UnityEngine;
using UnityEngine.Rendering;

[BepInPlugin("local.vam.haironepass", "VaM Hair One-Pass Lighting", "2.2.0")]
public sealed class VaMHairOnePass : BaseUnityPlugin
{
    private const string Native = "VaMHairOnePassNative";
    private static readonly bool Desktop = Environment.GetEnvironmentVariable("VAM_DISPLAY_MODE") == "Desktop";
    private const int ApiVersion = 5;
    private const int ModeOff = 0, ModeObserve = 1, ModeMerge = 2, ModeStrands = 3;
    private const int OpInstall = 1, OpBegin = 2, OpEnd = 3, OpFrame = 4;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    // Bound with GetProcAddress: VaM's Mono looks for [DllImport] libraries in VaM_Data\Plugins only.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PtrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void SetFn(int value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn(IntPtr resource);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadFn(byte[] buf, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void VoidFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void PathFn([MarshalAs(UnmanagedType.LPStr)] string path);

    private static IntFn hopApiVersion, hopInstalled, hopStrandsReady, hopSnapshotsTaken, hopCapturesTaken;
    private static VoidFn hopResetTiming;
    private static PathFn hopSnapshot, hopCapture;
    private static PtrFn hopRenderEventFunc;
    private static SetFn hopSetMode, hopSetTransmission;
    private static InitFn hopInit;
    private static ReadFn hopRead, hopLastError;

    private static IntPtr renderEvent;
    private static string loadError;
    private static int mode;

    private sealed class Hooked
    {
        public Camera camera;
        public CommandBuffer begin, end;
    }
    private static readonly List<Hooked> hooked = new List<Hooked>();

    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception(Native + " has no " + name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    private void Awake()
    {
        bool enabled = Config.Bind("General", "Enabled", true,
            "Draw the eye camera's hair through a visibility buffer (strands; what it cannot take: each item's extra pixel lights in one draw). Off: VaM is unchanged. Read at startup.").Value;
        if (!enabled)
        {
            loadError = "disabled ([General] Enabled = false)";
            Logger.LogInfo("VaM hair one-pass: disabled in the config; nothing loaded");
            return;
        }
        try
        {
            if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11) throw new Exception("not Direct3D 11");
            // .bin, not .dll: BepInEx reads every .dll under plugins as a .NET assembly
            string dll = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(typeof(VaMHairOnePass).Assembly.Location), Native + ".bin");
            IntPtr m = LoadLibraryW(dll);
            if (m == IntPtr.Zero) throw new Exception("cannot load " + dll + " (error " + Marshal.GetLastWin32Error() + ")");
            hopApiVersion = Bind<IntFn>(m, "HOP_ApiVersion");
            hopInstalled = Bind<IntFn>(m, "HOP_Installed");
            hopStrandsReady = Bind<IntFn>(m, "HOP_StrandsReady");
            hopRenderEventFunc = Bind<PtrFn>(m, "HOP_RenderEventFunc");
            hopSetMode = Bind<SetFn>(m, "HOP_SetMode");
            hopSetTransmission = Bind<SetFn>(m, "HOP_SetTransmission");
            hopInit = Bind<InitFn>(m, "HOP_Init");
            hopRead = Bind<ReadFn>(m, "HOP_Read");
            hopLastError = Bind<ReadFn>(m, "HOP_LastError");
            if (hopApiVersion() != ApiVersion) throw new Exception("native API " + hopApiVersion() + ", plugin expects " + ApiVersion);
            hopResetTiming = Bind<VoidFn>(m, "HOP_ResetTiming");
            hopSnapshot = Bind<PathFn>(m, "HOP_Snapshot");
            hopSnapshotsTaken = Bind<IntFn>(m, "HOP_SnapshotsTaken");
            hopCapture = Bind<PathFn>(m, "HOP_Capture");
            hopCapturesTaken = Bind<IntFn>(m, "HOP_CapturesTaken");
            // now, before any scene: the game creates hair shaders later and they are tagged as they are created
            int r = hopInit(Texture2D.whiteTexture.GetNativeTexturePtr());
            if (r != 0) throw new Exception("HOP_Init " + r + ": " + NativeError());
            renderEvent = hopRenderEventFunc();
            mode = ModeStrands;
            hopSetMode(mode);
            Camera.onPreCull += PreCull;
            StartCoroutine(FrameEvents());
            string err = NativeError();  // the one-pass shaders could not be made: the native side stays off
            if (err.Length > 0) Logger.LogError("VaM hair one-pass: " + err + "; merging off");
            else Logger.LogInfo("VaM hair one-pass 2.1.1: loaded, strands on");
        }
        catch (Exception e)
        {
            loadError = e.Message;
            Logger.LogError("VaM hair one-pass: " + e.Message);
        }
    }

    private void OnDestroy()
    {
        Camera.onPreCull -= PreCull;
        foreach (Hooked h in hooked)
        {
            try
            {
                if (h.camera == null) continue;
                h.camera.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque, h.begin);
                h.camera.RemoveCommandBuffer(CameraEvent.AfterForwardOpaque, h.end);
            }
            catch (Exception) { }
        }
        hooked.Clear();
        if (hopSetMode != null) hopSetMode(0);
    }

    private IEnumerator FrameEvents()
    {
        WaitForEndOfFrame end = new WaitForEndOfFrame();
        GL.IssuePluginEvent(renderEvent, OpInstall << 24);
        for (int frame = 0; ; frame++)
        {
            yield return end;
            GL.IssuePluginEvent(renderEvent, OpFrame << 24);
            // the hooks go in on the render thread: say once whether they did
            if (frame == 10)
            {
                if (hopInstalled() == 0) Logger.LogError("VaM hair one-pass: context hooks not installed: " + NativeError());
                else if (hopStrandsReady() != 0) Logger.LogInfo("VaM hair one-pass: context hooks installed, strands ready");
                else Logger.LogError("VaM hair one-pass: context hooks installed, strands not ready (merging): " + NativeError());
            }
        }
    }

    // The eye camera: every camera that renders both eyes in one pass. Its opaque pass is marked once.
    private static void PreCull(Camera c)
    {
        if (c == null) return;
        if (!c.stereoEnabled && !(Desktop && CameraTarget.centerTarget != null && c == CameraTarget.centerTarget.targetCamera)) return;
        for (int i = hooked.Count - 1; i >= 0; i--)
        {
            if (hooked[i].camera == c) return;
            if (hooked[i].camera == null) hooked.RemoveAt(i);
        }
        Hooked h = new Hooked();
        h.camera = c;
        h.begin = new CommandBuffer();
        h.begin.name = "hair one-pass begin";
        h.begin.IssuePluginEvent(renderEvent, (OpBegin << 24) | (hooked.Count & 0xffff));
        h.end = new CommandBuffer();
        h.end.name = "hair one-pass end";
        h.end.IssuePluginEvent(renderEvent, (OpEnd << 24) | (hooked.Count & 0xffff));
        c.AddCommandBuffer(CameraEvent.BeforeForwardOpaque, h.begin);
        c.AddCommandBuffer(CameraEvent.AfterForwardOpaque, h.end);
        hooked.Add(h);
    }

    private static string NativeError()
    {
        byte[] buf = new byte[512];
        int n = hopLastError(buf, buf.Length);
        return Encoding.ASCII.GetString(buf, 0, Math.Max(0, Math.Min(n, buf.Length - 1)));
    }

    // ---- test API (VaM test agent) ------------------------------------------------------------------------------------

    public static string Observe(bool on)
    {
        return SetMode(on ? ModeObserve : ModeOff);
    }

    public static string Merge(bool on)
    {
        return SetMode(on ? ModeMerge : ModeOff);
    }

    public static string Strands(bool on)
    {
        return SetMode(on ? ModeStrands : ModeOff);
    }

    // 2.1.0: the strands' transmission lobe (hair lit from behind glows); off = 2.0.1's lighting.
    public static string Transmission(bool on)
    {
        if (loadError != null) return "error: " + loadError;
        hopSetTransmission(on ? 1 : 0);
        return "transmission " + (on ? "on" : "off");
    }

    private static string SetMode(int m)
    {
        if (loadError != null) return "error: " + loadError;
        mode = m;
        hopSetMode(mode);
        return "mode " + mode + ", context hooks " + (hopInstalled() != 0 ? "installed" : "not installed: " + NativeError()) +
               ", eye cameras " + hooked.Count;
    }

    public static string ResetTiming()
    {
        if (loadError != null) return "error: " + loadError;
        hopResetTiming();
        return "ok";
    }

    // The file: a 16-byte header (uint32 'SNAP', width, height, DXGI format) and the rows, 4 bytes per pixel.
    public static string Snapshot(string path)
    {
        if (loadError != null) return "error: " + loadError;
        hopSnapshot(path);
        return "requested";
    }

    public static int SnapshotsTaken()
    {
        return loadError != null ? -1 : hopSnapshotsTaken();
    }

    // The hair draws of the next merged opaque pass as Unity issued them, for tests\onepass_compare --replay.
    public static string Capture(string path)
    {
        if (loadError != null) return "error: " + loadError;
        hopCapture(path);
        return "requested";
    }

    public static int CapturesTaken()
    {
        return loadError != null ? -1 : hopCapturesTaken();
    }

    public static string Report()
    {
        if (loadError != null) return "{\"error\":\"" + loadError.Replace("\\", "\\\\").Replace("\"", "'") + "\"}";
        byte[] buf = new byte[1 << 17];
        int n = hopRead(buf, buf.Length);
        if (n < 0) { buf = new byte[-n + 1]; n = hopRead(buf, buf.Length); }
        string err = NativeError();
        return "{\"eye_cameras\":" + hooked.Count + ",\"native_error\":\"" + err.Replace("\"", "'") + "\",\"native\":" +
               Encoding.ASCII.GetString(buf, 0, Math.Max(0, n)) + "}";
    }
}
