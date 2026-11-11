// VaM tone mapping 0.2.2 (BepInEx 5, Unity 2018.1, C# 3).
//
// VaM's eye camera renders LDR without tone mapping: light above 1.0 is cut off flat. While on, this renders VaM's main
// camera (CameraTarget.centerTarget, the stereo eye camera in VR) in HDR and, in its OnRenderImage, bloom plus a
// highlight shoulder (VaMToneMappingNative.bin, shaders\tonemap.hlsl): below the knee (0.6) every value stays as VaM
// drew it, above it the curve rolls off to white instead of clipping, and light above the old clip point (1.0) glows a
// little, warm (film halation). Then the result is blitted to the camera's output, so what OpenComposite / DLSS get is
// in VaM's usual range.
//
// [General] Mode (BepInEx\config\local.vam.tonemapping.cfg, live, and in the VR menu): 0 off (VaM unchanged: the camera
// keeps its own HDR setting, nothing is attached), 1 while Display Lighting lights the scene (its running and not
// suspended state, read each frame), 2 always. A camera target that is not a float texture (no HDR) is blitted as it
// is and the reason shows in Report().
//
// [General] Style (0.2.0, live, in the VR menu): 0 neutral (the shoulder above), 1 film - a whole film curve with a toe
// and more mid contrast, bright colours bleaching to white, a print's cool shadows and warm highlights (tonemap.hlsl's
// CSCompositeFilm). It changes the mids and shadows of every scene it is on in.
//
// Test API (VaM test agent): Mode(m) sets the mode for this run (not saved), Style(s) the style; Params(knee,
// threshold, softKnee, intensity, r, g, b) the curve and bloom; Film(contrast, shoulder, hdrMax, midIn, midOut,
// saturation, crosstalk, split) the film style; Report() the state as JSON.
using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Bootstrap;
using BepInEx.Configuration;
using UnityEngine;
using UnityEngine.Rendering;

[BepInPlugin("local.vam.tonemapping", "VaM Tone Mapping", "0.2.2")]
[BepInDependency("local.vam.displaylighting", BepInDependency.DependencyFlags.SoftDependency)]
public sealed class VaMToneMapping : BaseUnityPlugin
{
    private const int ApiVersion = 2;
    private const int OpTonemap = 1;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PtrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn(IntPtr resource);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadFn(byte[] buf, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ParamsFn(float knee, float threshold, float softKnee, float intensity, float r, float g, float b);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void StyleFn(int style);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FilmFn(float contrast, float shoulder, float hdrMax, float midIn, float midOut, float saturation, float crosstalk, float split);

    // Bound with GetProcAddress: VaM's Mono looks for [DllImport] libraries in VaM_Data\Plugins only.
    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception("VaMToneMappingNative.bin has no " + name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    internal static IntPtr renderEvent = IntPtr.Zero;
    private static ReadFn tmRead;
    private static ParamsFn tmParams;
    private static StyleFn tmStyle;
    private static FilmFn tmFilm;
    private static string loadError;
    private static VaMToneMapping instance;

    private ConfigEntry<int> mode, style;
    private int testMode = -1, testStyle = -1, styleSent = -1;
    private Camera camera;
    private ToneMapEffect effect;
    private bool hdrBefore, hdrOwned;
    [DllImport("openvr_api")] private static extern int VaMDLSS_GetState();
    [DllImport("openvr_api")] private static extern IntPtr VaMDLSS_WrapToneMapEvent(IntPtr original);
    private Texture2D deviceTexture;

    // Display Lighting's state (VaMDisplayLighting 1.6.0: private bool running, suspended), found once
    private object lighting;
    private FieldInfo lightingRunning, lightingSuspended;
    private bool lightingLooked;

    private void Awake()
    {
        instance = this;
        mode = Config.Bind("General", "Mode", 1, new ConfigDescription(
            "Tone mapping and bloom: 0 off (VaM unchanged), 1 while Display Lighting is on, 2 always.", new AcceptableValueRange<int>(0, 2)));
        style = Config.Bind("General", "Style", 0, new ConfigDescription(
            "0 neutral (only what VaM clipped is toned), 1 film (a film curve and a print's colour; changes the mids and shadows).",
            new AcceptableValueRange<int>(0, 1)));
        try
        {
            string dir = System.IO.Path.GetDirectoryName(Info.Location);
            IntPtr m = LoadLibraryW(System.IO.Path.Combine(dir, "VaMToneMappingNative.bin"));
            if (m == IntPtr.Zero) throw new Exception("cannot load VaMToneMappingNative.bin (error " + Marshal.GetLastWin32Error() + ")");
            IntFn api = Bind<IntFn>(m, "TM_ApiVersion");
            if (api() != ApiVersion) throw new Exception("native API " + api() + ", plugin expects " + ApiVersion);
            InitFn init = Bind<InitFn>(m, "TM_Init");
            renderEvent = Bind<PtrFn>(m, "TM_RenderEventFunc")();
            try { renderEvent = VaMDLSS_WrapToneMapEvent(renderEvent); } catch (Exception) { }
            tmRead = Bind<ReadFn>(m, "TM_Read");
            tmParams = Bind<ParamsFn>(m, "TM_SetParams");
            tmStyle = Bind<StyleFn>(m, "TM_SetStyle");
            tmFilm = Bind<FilmFn>(m, "TM_SetFilm");
            deviceTexture = new Texture2D(1, 1, TextureFormat.RGBA32, false);
            int r = init(deviceTexture.GetNativeTexturePtr());
            if (r != 0) throw new Exception("TM_Init " + r + ": " + Read());
            Logger.LogInfo("VaM tone mapping 0.2.2: loaded, mode " + mode.Value + ", style " + style.Value);
        }
        catch (Exception e)
        {
            loadError = e.Message;
            Logger.LogError("VaM tone mapping: " + e.Message + "; off");
        }
    }

    private void LateUpdate()
    {
        if (loadError != null) return;
        int s = testStyle >= 0 ? testStyle : style.Value;
        if (s != styleSent)
        {
            tmStyle(s);
            styleSent = s;
        }
        int m = testMode >= 0 ? testMode : mode.Value;
        bool want = m == 2 || (m == 1 && DisplayLightingOn());
        bool rr = false;
        try { rr = (VaMDLSS_GetState() & 16384) != 0; } catch (Exception) { }
        Camera c = MainCamera();
        if (c != camera) Detach();
        camera = c;
        if (camera == null) return;
        if ((want || rr) && !hdrOwned) { hdrBefore = camera.allowHDR; hdrOwned = true; }
        if (want || rr) camera.allowHDR = true;
        if (want && effect == null)
        {
            effect = camera.gameObject.AddComponent<ToneMapEffect>();
            Logger.LogInfo("VaM tone mapping: on (" + camera.name + ", HDR was " + (hdrBefore ? "on" : "off") + ")");
        }
        else if (!want && effect != null)
        {
            Destroy(effect); effect = null;
            Logger.LogInfo("VaM tone mapping: off");
        }
        if (!want && !rr && hdrOwned) { camera.allowHDR = hdrBefore; hdrOwned = false; }
    }

    private void OnDestroy()
    {
        Detach();
    }

    private void Detach()
    {
        if (effect != null) Destroy(effect);
        effect = null;
        if (camera != null && hdrOwned) camera.allowHDR = hdrBefore;
        hdrOwned = false;
    }

    private static Camera MainCamera()
    {
        try
        {
            return CameraTarget.centerTarget != null ? CameraTarget.centerTarget.targetCamera : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private bool DisplayLightingOn()
    {
        if (!lightingLooked)
        {
            lightingLooked = true;
            PluginInfo info;
            if (Chainloader.PluginInfos.TryGetValue("local.vam.displaylighting", out info) && info != null && info.Instance != null)
            {
                lighting = info.Instance;
                Type t = lighting.GetType();
                lightingRunning = t.GetField("running", BindingFlags.Instance | BindingFlags.NonPublic);
                lightingSuspended = t.GetField("suspended", BindingFlags.Instance | BindingFlags.NonPublic);
                if (lightingRunning == null) Logger.LogWarning("VaM tone mapping: Display Lighting has no running state; mode 1 stays off");
            }
        }
        if (lighting == null || lightingRunning == null) return false;
        if (!(bool)lightingRunning.GetValue(lighting)) return false;
        return lightingSuspended == null || !(bool)lightingSuspended.GetValue(lighting);
    }

    private static string Read()
    {
        if (tmRead == null) return "";
        byte[] buf = new byte[1024];
        int n = tmRead(buf, buf.Length);
        return Encoding.ASCII.GetString(buf, 0, Math.Max(0, Math.Min(n - 1, buf.Length - 1)));
    }

    // ---- test API (VaM test agent) ------------------------------------------------------------------------------------

    public static string Mode(int m)
    {
        if (instance == null) return "error: not loaded";
        instance.testMode = m < 0 ? -1 : Math.Min(m, 2);
        instance.LateUpdate();
        return Report();
    }

    public static string Style(int s)
    {
        if (instance == null) return "error: not loaded";
        instance.testStyle = s < 0 ? -1 : Math.Min(s, 1);
        instance.LateUpdate();
        return Report();
    }

    public static string Film(float contrast, float shoulder, float hdrMax, float midIn, float midOut, float saturation, float crosstalk, float split)
    {
        if (tmFilm == null) return "error: " + (loadError ?? "not loaded");
        tmFilm(contrast, shoulder, hdrMax, midIn, midOut, saturation, crosstalk, split);
        return Report();
    }

    public static string Params(float knee, float threshold, float softKnee, float intensity, float r, float g, float b)
    {
        if (tmParams == null) return "error: " + (loadError ?? "not loaded");
        tmParams(knee, threshold, softKnee, intensity, r, g, b);
        return Report();
    }

    public static string Report()
    {
        if (instance == null) return "{\"error\":\"not loaded\"}";
        if (loadError != null) return "{\"error\":\"" + loadError.Replace("\"", "'") + "\"}";
        return "{\"mode\":" + instance.mode.Value + ",\"test_mode\":" + instance.testMode + ",\"style\":" + instance.style.Value +
               ",\"test_style\":" + instance.testStyle + ",\"active\":" + (instance.effect != null ? 1 : 0) +
               ",\"camera\":\"" + (instance.camera != null ? instance.camera.name : "") + "\",\"hdr\":" +
               (instance.camera != null && instance.camera.allowHDR ? 1 : 0) + ",\"display_lighting\":" + (instance.DisplayLightingOn() ? 1 : 0) +
               ",\"native\":" + Read() + "}";
    }
}

// On the main camera while tone mapping is on: the native passes on the camera's HDR target, then the copy out.
public sealed class ToneMapEffect : MonoBehaviour
{
    private CommandBuffer command;

    private void OnRenderImage(RenderTexture src, RenderTexture dst)
    {
        if (VaMToneMapping.renderEvent != IntPtr.Zero)
        {
            if (command == null) command = new CommandBuffer { name = "VaM tone mapping" };
            int eyes = src.vrUsage == VRTextureUsage.TwoEyes ? 2 : 1;
            command.Clear();
            command.SetRenderTarget(src);
            command.IssuePluginEvent(VaMToneMapping.renderEvent, (1 << 24) | (eyes << 16));
            Graphics.ExecuteCommandBuffer(command);
        }
        Graphics.Blit(src, dst);
    }

    private void OnDestroy()
    {
        if (command != null) command.Release();
        command = null;
    }
}
