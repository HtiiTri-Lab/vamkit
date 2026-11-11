// VaM VR settings menu (BepInEx 5, Unity 2018.1, C# 3).
//
// A "VR 画面" tab in VaM's main menu with what is worth changing from inside
// the headset while the game runs (VaMVRMenu.Menu.cs builds the tab):
//   * frame generation, in the OFXR layer's ofxr_bridge.ini (beside the loaded
//     XR_APILAYER_XRFrameBridge_diagnostic.dll, written by the OFXR tray): the
//     multiplier and the FPS overlay, both re-read by the layer every 250 ms.
//     The multiplier is `[ofxr] frame_multiplier`, which OFXR-DLSSG-v10 and
//     later follow (2x to 4x with DLSS Frame Generation, whatever the launch
//     started with); the tray's 3X switch (triple_frame_gen) is kept to match
//     for the layers that know only that. The tray writes the whole file
//     again when its own menu is used or it re-arms, which puts its own
//     choice back.
//   * frame generation on and off ([Performance] FrameGeneration in this
//     run's performance.ini), through the tray's pause (VaMVRMenu.FrameGen.cs);
//     the launch panel always starts VaM through OFXR and leaves this to the
//     tab. While VaM's menus are open frame generation is off as well
//     (DLSSMenuNative), so menu text is never interpolated.
//   * noise reduction, in this run's performance.ini (VAM_PERFORMANCE_CONFIG,
//     written by LaunchPanel.py, passed on by Launch.ps1), which the NR
//     prepass re-reads every 250 ms; in the DLSS launcher also where NR runs
//     (before or after the upscaler, NRAfterUpscale).
//   * DLSS SR (v8, all live in the DLSS launcher): the model ([Performance]
//     DLSSPreset: Off, K, L or M; Off = no SR, native frames), the render
//     scale (RenderScale, 33-100% of the output; 100% with a model = DLAA),
//     which each model remembers for itself (RenderScaleK/L/M, by default 75,
//     33 and 50: choosing a model brings its own back), and the output size
//     (OutputScale, 75-150% of the runtime's size; the swapchains follow).
//     And whether VaM's own menus are shown without SR and without frame
//     generation (DLSSMenuNative: the DLSS bridge and this plugin read it).
//   * v34 restores sharpening after DLSS SR, [Performance] DLSSSharpness,
//     0-100 (0 = off), using the compositor's NVSharpen pass for K/L/M. Modern DLSS
//     no longer supports its old internal sharpness parameter. The launch
//     panel carries this value to the next launch. FFR stays off under 11on12.
//   * the NR model (v11: [NR] RenderPreset, 2 = new, 1 = 310.8.0's; the NR
//     prepass of OpenComposite-DLSS-v22 recreates its feature on a change).
//   * v29: NR after the upscaler's 处理比例 ([Performance] NRAfterScale) and
//     DLSS-G's mode ([Performance] DLSSGLite, 1 = Preset C; only with a layer
//     that follows it live, OFXR-DLSSG-v12), both the launch panel's before.
//   * v30: the same settings in pages (插帧, 清晰度, 降噪, 光影, 皮肤, 身体), each
//     with its sub-pages, laid out without scrolling (VaMVRMenu.Menu.cs); no
//     hints: the labels and values say what a row does. v31: pages, sub-pages,
//     cards and choices each with a look of their own. v32: 区域上移, the NR
//     region moved up or down ([NR] RegionShiftY, -30..30% of the eye image's
//     height; OpenComposite-DLSS-on12-nrshift).
//   * the skin highlights (v17: [Specular] Normalize of VaM Skin Transmission
//     0.18.0 and each character's level, a slider 0 no highlight .. 1 natural
//     matte .. 4 dewy; v14 had 0.13.0's four steps, v13 0.12.0's Gloss, v12
//     its [Specular] Look 0.4.0 .. 0.11.0): Normalize is a setting of that
//     plugin's own BepInEx config, not a file of this run's (set in memory, the
//     plugin follows at once - a few shader constants - and BepInEx saves its
//     config, so the look stays for the next start); the levels go through its
//     SkinGlossLevels / SetSkinGlossLevel, which keeps each skin's own level
//     for the next time it is loaded. Only while that plugin is loaded and
//     switched on.
// The launch panel starts DLSS SR with DLSS Frame Generation only (v9: its
// FSR 1, NIS and optical-flow launches went), so the tab has no other upscaler.
// These settings are this tab's alone: the launch panel shows none of them.
// It carries them to the next launch (taking them from this run's files),
// and keeps everything else.
// Only values change (VRMenuIni.cs): every other line stays, and each file is
// replaced whole through a temporary file and MoveFileEx, so a reader never
// sees half of one. Changes made elsewhere are picked up within a second.
// Dormant - no tab, no file access - unless VAM_PERFORMANCE_CONFIG names a
// launch panel run (performance.ini beside its selection.json): VaM started
// any other way, including the older launchers whose shared profile must not
// change, is left alone.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using BepInEx;
using BepInEx.Bootstrap;
using BepInEx.Configuration;
using UnityEngine;

[BepInPlugin("local.vam.vrframegen.menu", "VaM VR Settings Menu", "24.9.0")]
public sealed partial class VaMVRMenu : BaseUnityPlugin
{
    private const string PerformanceVariable = "VAM_PERFORMANCE_CONFIG";
    private static readonly bool Desktop = Environment.GetEnvironmentVariable("VAM_DISPLAY_MODE") == "Desktop";
    private static readonly string LayerModule = Desktop ? "d3d11_proxy.dll" : "XR_APILAYER_XRFrameBridge_diagnostic.dll";
    private const string MultiplierId = "frame_multiplier";
    [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] private static extern bool K32EnumProcessModules(IntPtr process, IntPtr[] modules, uint bytes, out uint needed);
    [DllImport("kernel32.dll", EntryPoint="GetModuleHandleExW")] private static extern bool GetModuleFromAddress(uint flags, IntPtr address, out IntPtr module);
    private static IntPtr presenterModule;
    private static IntPtr FindPresenter()
    {
        if (presenterModule != IntPtr.Zero) return presenterModule;
        IntPtr[] modules = new IntPtr[1024]; uint bytes;
        if (K32EnumProcessModules(GetCurrentProcess(), modules, (uint)(modules.Length * IntPtr.Size), out bytes))
            for (int i=0;i<modules.Length && i<bytes/IntPtr.Size;i++)
                if (GetProcAddress(modules[i], "OFXR_GetProviderApiVersionV4") != IntPtr.Zero) { presenterModule=modules[i]; break; }
        return presenterModule;
    }
    private static bool AcquireLayer(out IntPtr module)
    {
        if (!Desktop) return GetModuleHandleExW(0, LayerModule, out module);
        IntPtr address=FindPresenter(); module=IntPtr.Zero;
        return address!=IntPtr.Zero && GetModuleFromAddress(4, address, out module);
    }
    private const string PresetId = "dlss_preset", ScaleId = "render_scale", OutputId = "output_scale", RendererId = "skin_renderer";
    private const string SkinPlugin = "local.vam.skintransmission", SkinNormalizeId = "skin_normalize";
    private const int SkinSlots = 4;   // characters with a level slider (a scene's first four)
    private const float CheckSeconds = 0.5f, WriteSeconds = 0.2f, RetrySeconds = 0.5f;
    private const float LayerSearchSeconds = 1f, LayerWaitSeconds = 60f, LogSeconds = 1f;

    private enum Kind { Toggle, Slider, Choice, Status }

    private sealed class Item
    {
        public string id, label;
        public bool layer;                 // ofxr_bridge.ini; else performance.ini
        public ConfigEntryBase entry;      // else another plugin's setting (v12), in no file of this run's
        public string section, key;
        public Kind kind;
        public int min, max;               // Slider range; Choice: 0..values.Length-1
        public bool fractional;           // continuous plugin setting, displayed to two decimal places
        public float step;                 // Slider: what − and + move it by
        public string unit = "";           // Slider: after its value
        public string[] values, names;     // Choice: the values in the file and what they mean; Toggle: off, on
        public string fallback;            // what the reader uses when the key is missing
        public string parent;              // shown only while this toggle is on
        public string unlessId, unlessValue; // hidden while that item has that value
        public int pathRule;               // 1: shown only while path tracing is on; -1: only while it is off
        public bool hidden;                // in the file only, no control
        public string value, original;     // as in the file now; at the first read
        public bool dirty;                 // changed here, not yet written
        public Func<string> live;          // Status: its text, asked once a second while the tab is seen
        public string group;               // shown in the card of that item, under it (v31), with no effect on what is shown
        public readonly List<Btn> buttons = new List<Btn>();   // Choice, Toggle
        public UnityEngine.UI.Slider bar;  // Slider
        public UnityEngine.UI.Text valueText;
        public GameObject control;         // its row in the menu
        public int skinSlot = -1;          // v17: a character's level slider (SkinGlossLevels' n-th line), not in a file
        public string skinUid, skinLabel;  // the character in that row now (null = none), its row label
        public float skinStart = -1f;      // its starting level, < 0 = not known yet
        public UnityEngine.UI.Text labelText;   // its label in the menu
    }

    // v30: a page of the tab (a button on its left) and its sections (the
    // sub-pages in the bar above the rows). A section that does not fit is
    // shown as two or more sub-pages; one that has no row shown is left out.
    private sealed class Page
    {
        public string title;
        public readonly List<Section> sections = new List<Section>();
        public Btn button;
    }

    private sealed class Section
    {
        public string title;               // null: the page's own
        public readonly List<Item> members = new List<Item>();
        public readonly List<Card> cards = new List<Card>();   // in the menu: its members, a card per switch
    }

    private sealed class Store
    {
        public string path, name;
        public bool loaded, pending, failed;
        public DateTime stamp = DateTime.MinValue;
        public float nextWrite, nextCheck;
    }

    private readonly List<Item> items = new List<Item>();
    private readonly Dictionary<string, Item> byId = new Dictionary<string, Item>();
    private readonly List<Page> pages = new List<Page>();
    private readonly Store performance = new Store(), layer = new Store();
    private readonly SortedDictionary<string, string> unlogged = new SortedDictionary<string, string>();
    private Section current;
    private bool dormant, failed, dlss, dlssg, quad;
    private int renderScale = 100, launchMultiplier;
    private string upscaler = "", message = "";
    private float startTime, nextLayerSearch, nextLog;
    private bool layerSearchDone;
    // The layer's report on its session (QueryLayer): whether it gives one at
    // all, the multiplier now, the most it can switch to, and whether it
    // follows a switch while it runs.
    private bool layerReports, layerSwitchable = true;
    private int framesNow, framesMost;

    private void Awake()
    {
        string path = Environment.GetEnvironmentVariable(PerformanceVariable);
        if (string.IsNullOrEmpty(path) || !File.Exists(path))
        {
            dormant = true;
            Logger.LogInfo("VaM VR menu: dormant (" + PerformanceVariable + " is not set: VaM was not started by the vamkit launcher)");
            return;
        }
        string selection = Path.Combine(Path.GetDirectoryName(path), "selection.json");
        if (!File.Exists(selection))
        {
            dormant = true;
            Logger.LogInfo("VaM VR menu: dormant (" + path + " is not a launch panel run: no selection.json beside it)");
            return;
        }
        performance.path = path;
        performance.name = "performance.ini";
        layer.name = "ofxr_bridge.ini";
        dlssg = Environment.GetEnvironmentVariable("OFXR_DLSSG") == "1";
        quad = Environment.GetEnvironmentVariable("OFXR_FRAME_MULTIPLIER") == "4";
        try
        {
            string text = new VRMenuIni.Document(File.ReadAllBytes(path)).text;
            upscaler = (VRMenuIni.Get(text, "Performance", "Upscaler") ?? "").Trim();
            dlss = upscaler.Equals("DLSS", StringComparison.OrdinalIgnoreCase);
            renderScale = dlss ? ParseInt(VRMenuIni.Get(text, "Performance", "RenderScale"), 75, 33, 100) : 100;
            frameGenStart = ParseInt(VRMenuIni.Get(text, "Performance", "FrameGeneration"), 1, 0, 1);
            menuNativeStart = ParseInt(VRMenuIni.Get(text, "Performance", "DLSSMenuNative"), 1, 0, 1);
            Match m = Regex.Match(File.ReadAllText(selection), "\"frame_multiplier\"\\s*:\\s*(\\d+)");
            if (m.Success) launchMultiplier = int.Parse(m.Groups[1].Value);
        }
        catch (Exception e)
        {
            dormant = true;
            Logger.LogWarning("VaM VR menu: cannot read " + path + ": " + e.Message + "; menu off");
            return;
        }
        startTime = Time.realtimeSinceStartup;
        Logger.LogInfo("VaM VR menu: settings " + path + " (upscaler " + upscaler + (dlssg ? ", DLSS Frame Generation" : "") +
            (launchMultiplier > 1 ? ", " + launchMultiplier + "x" : "") + "); the tab appears in the main menu");
    }

    private void Update()
    {
        if (dormant || failed) return;
        try
        {
            float now = Time.realtimeSinceStartup;
            FindLayer(now);
            UpdatePause(now);
            if (!performance.loaded) Load(performance, now);
            if (layer.path != null && !layer.loaded) Load(layer, now);
            Flush(performance, now);
            Flush(layer, now);
            CheckExternal(performance, now);
            CheckExternal(layer, now);
            if (unlogged.Count > 0 && now >= nextLog) LogWrites(now);
            UpdateMenu(now);
        }
        catch (Exception e)
        {
            failed = true;
            Logger.LogError("VaM VR menu: stopped after an unexpected error: " + e);
        }
    }

    // ---- the run -----------------------------------------------------------

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandleW(string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetModuleHandleExW(uint flags, string name, out IntPtr module);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern uint GetModuleFileNameW(IntPtr module, StringBuilder name, uint size);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool MoveFileExW(string existing, string replacement, uint flags);

    private const uint MoveReplaceExisting = 0x1;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int FrameMultiplierReport(out uint current, out uint most, out uint flags);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int LiveModeReport();

    // The OFXR layer is in VaM only in a launch with frame generation. It is
    // loaded with VR, usually before this plugin looks; the menu waits a while
    // for it, and for its session's report on how far the multiplier goes,
    // since the list of settings depends on both.
    private void FindLayer(float now)
    {
        if (layerSearchDone || now < nextLayerSearch) return;
        nextLayerSearch = now + LayerSearchSeconds;
        if (Desktop && layer.path == null)
        {
            IntPtr presenter = FindPresenter();
            // During P1-P3 the native presenter has not been installed yet.
            // The rest of the menu must not wait for an absent XR session.
            if (presenter == IntPtr.Zero || GetProcAddress(presenter, "OFXR_GetFrameMultiplierV1") == IntPtr.Zero)
            {
                layerSearchDone = true;
                return;
            }
            string ini = Environment.GetEnvironmentVariable("VAM_DESKTOP_CONFIG");
            if (!String.IsNullOrEmpty(ini) && File.Exists(ini))
            {
                layer.path = ini;
                FindPause(ini);
            }
        }
        if (launchMultiplier == 1)
        {
            layerSearchDone = true;                 // frame generation off: no layer
            return;
        }
        if (layer.path == null)
        {
            IntPtr module = Desktop ? FindPresenter() : GetModuleHandleW(LayerModule);
            if (module != IntPtr.Zero)
            {
                StringBuilder name = new StringBuilder(32768);
                uint n = GetModuleFileNameW(module, name, (uint)name.Capacity);
                string ini = n > 0 ? Path.Combine(Path.GetDirectoryName(name.ToString()), "ofxr_bridge.ini") : null;
                if (ini != null && File.Exists(ini))
                {
                    layer.path = ini;
                    Logger.LogInfo("VaM VR menu: OFXR layer settings " + ini);
                    FindPause(ini);
                }
            }
        }
        if (layer.path != null)
        {
            int answer = QueryLayer();
            if (answer != 0)
            {
                layerSearchDone = true;
                Logger.LogInfo(answer < 0
                    ? "VaM VR menu: this OFXR layer does not report its frame multiplier (older than OFXR-DLSSG-v10): its 3X switch only"
                    : "VaM VR menu: the OFXR session runs " + framesNow + "x and can switch between 2x and " + framesMost + "x" +
                        (layerSwitchable ? "" : ", but not while it runs"));
                return;
            }
        }
        if (now - startTime >= LayerWaitSeconds)
        {
            layerSearchDone = true;
            Logger.LogInfo(layer.path == null
                ? "VaM VR menu: no OFXR layer in VaM after " + LayerWaitSeconds + " s: frame generation items left out"
                : "VaM VR menu: the OFXR layer has no session after " + LayerWaitSeconds + " s: its 3X switch only");
        }
    }

    // The layer's report (OFXR_GetFrameMultiplierV1, OFXR-DLSSG-v10 and later):
    // 1 with framesNow, framesMost and layerSwitchable set, 0 while it has no
    // session, -1 from a layer without the report or with none loaded.
    private int QueryLayer()
    {
        IntPtr module;
        // Counted as a use, so the layer stays loaded while it is asked.
        if (!AcquireLayer(out module) || module == IntPtr.Zero) return -1;
        try
        {
            IntPtr report = GetProcAddress(module, "OFXR_GetFrameMultiplierV1");
            if (report == IntPtr.Zero) return -1;
            layerReports = true;
            FrameMultiplierReport ask = (FrameMultiplierReport)Marshal.GetDelegateForFunctionPointer(report, typeof(FrameMultiplierReport));
            uint now, most, flags;
            if (ask(out now, out most, out flags) == 0) return 0;
            framesNow = (int)now;
            framesMost = (int)most;
            layerSwitchable = (flags & 1) != 0;
            return 1;
        }
        finally { FreeLibrary(module); }
    }

    // True when the loaded OFXR layer follows [Performance] DLSSGLite while it
    // runs (OFXR_GetDlssgLiveModeV1, OFXR-DLSSG-v12 and later, 1 with DLSS-G).
    private bool LayerFollowsDlssgMode()
    {
        IntPtr module;
        if (!AcquireLayer(out module) || module == IntPtr.Zero) return false;
        try
        {
            IntPtr report = GetProcAddress(module, "OFXR_GetDlssgLiveModeV1");
            return report != IntPtr.Zero &&
                ((LiveModeReport)Marshal.GetDelegateForFunctionPointer(report, typeof(LiveModeReport)))() == 1;
        }
        finally { FreeLibrary(module); }
    }

    private bool ShapeKnown { get { return layerSearchDone; } }

    // What the tray's 3X switch gives: three, or four in a 4X launch - no more
    // than the session was made for, where the layer says.
    private int SwitchMultiplier { get { return Math.Min(quad ? 4 : 3, framesMost >= 3 ? framesMost : 4); } }

    // ---- items -------------------------------------------------------------

    // v30: the pages, left to right in what a change costs or shows:
    // 插帧 (frame generation and VaM's menus), 清晰度 (DLSS SR and the output),
    // 降噪 (NR: 基本 and 强度), 光影 (ray tracing, tone mapping), 皮肤 (the skin
    // highlights), 身体 (motion sampling, soft body). A new setting goes into the
    // page and section it belongs to; a page with no row (its plugin missing,
    // no frame generation) has no button.
    private void BuildItems()
    {
        items.Clear();
        byId.Clear();
        pages.Clear();
        AddPage("插帧");
        if (layer.path != null)
        {
            // The multipliers this session can switch to; from a layer without
            // the report, 2X and its switch's.
            List<string> values = new List<string>(), names = new List<string>();
            if (framesMost >= 3)
                for (int m = 2; m <= framesMost; m++) values.Add(m.ToString());
            else
            {
                values.Add("2");
                values.Add(SwitchMultiplier.ToString());
            }
            foreach (string v in values) names.Add(v + "×");
            if (pauseName != null) Add(OnOff(FrameGenId, "Performance", "FrameGeneration", "1", "插帧"));
            if (layerSwitchable)
            {
                Item multiplier = Add(Choice(MultiplierId, "ofxr", "frame_multiplier", "2", "倍数", values.ToArray(), names.ToArray()));
                multiplier.layer = true;
                if (pauseName != null) multiplier.parent = FrameGenId;
            }
            // v29: DLSS-G's mode, [Performance] DLSSGLite (the launch panel's before),
            // which OFXR-DLSSG-v12 follows live (OFXR_GetDlssgLiveModeV1).
            if (dlssg && LayerFollowsDlssgMode())
            {
                Item mode = Add(Choice("dlssg_lite", "Performance", "DLSSGLite", "0", "模式", new string[] { "0", "1" },
                    new string[] { "质量", "性能（Preset C）" }));
                if (pauseName != null) mode.parent = FrameGenId;
            }
            Add(Choice("overlay", "overlay", "position", "upper_right", "帧率显示",
                new string[] { "off", "upper_left", "upper_right", "lower_left", "lower_right" },
                new string[] { "关闭", "左上", "右上", "左下", "右下" })).layer = true;
        }
        // DLSSMenuNative: the DLSS bridge renders the menus at 100% without
        // SR; this plugin pauses frame generation (VaMVRMenu.FrameGen.cs).
        if (dlss || pauseName != null)
            Add(OnOff(MenuNativeId, "Performance", "DLSSMenuNative", "1", "VaM 菜单打开时")).names =
                new string[] { "照常", dlss && pauseName != null ? "暂停超分和插帧" : dlss ? "暂停超分" : "暂停插帧" };
        // DLSSPreset, RenderScale and OutputScale: live in the DLSS launcher
        // (vam_performance_config.h VamLiveRenderSettings, the bridge's eye
        // texture scale).
        AddPage("清晰度");
        if (dlss)
        {
            // The in-game path tracing switch decides the model. On: RR F at its own input scale. Off: K / L / M or none at
            // theirs, kept untouched while PT is on. vam_performance_config.h reads the same keys the same way.
            Item model = Add(Choice(PresetId, "Performance", "DLSSPreset", "K", "超分模型", new string[] { "Off", "K", "L", "M" },
                new string[] { "关闭", "K", "L", "M" }));
            model.pathRule = -1;
            Live("pt_reconstruction", PresetId, delegate { return "重建模型 · RR · F"; }).pathRule = 1;
            Item scale = Add(Number(ScaleId, "Performance", "RenderScale", "75", "渲染比例", 33, 100));
            scale.unit = "%";
            scale.unlessId = PresetId; scale.unlessValue = "Off";
            scale.pathRule = -1;
            Item scaleRr = Add(Number(ScaleId + "_RR", "Performance", "RenderScaleRR", "33", "渲染比例", 33, 100));
            scaleRr.unit = "%"; scaleRr.group = PresetId; scaleRr.pathRule = 1;
            Hidden(Number(ScaleId + "_K", "Performance", "RenderScaleK", "75", "", 33, 100));
            Hidden(Number(ScaleId + "_L", "Performance", "RenderScaleL", "33", "", 33, 100));
            Hidden(Number(ScaleId + "_M", "Performance", "RenderScaleM", "50", "", 33, 100));
            ConfigEntryBase rrDetailEntry = PluginSetting(SkinPlugin, "RayTracing", "RRDetail");
            if (rrDetailEntry != null) {
                Item detail = Add(Number("skin_rr_detail", "RayTracing", "RRDetail", "75", "细节保留", 0, 100));
                detail.entry = rrDetailEntry; detail.group = PresetId; detail.pathRule = 1; detail.step = 5f;
            }
            if (!Desktop) {
                Item stereo = Add(Number("rr_stereo", "Performance", "RRStereo", "0", "双眼一致性", 0, 100));
                stereo.unit = "%"; stereo.group = PresetId; stereo.pathRule = 1; stereo.step = 10f;
            }
            Item sharpness = Add(Number("dlss_sharpness", "Performance", "DLSSSharpness", "25", "锐化", 0, 100));
            sharpness.unit = "%";
            sharpness.unlessId = PresetId; sharpness.unlessValue = "Off";
            sharpness.group = PresetId;
            Item sharpenPosition = Add(Choice("dlss_sharpen_last", "Performance", "DLSSSharpenLast", "1", "锐化位置",
                new string[] { "0", "1" }, new string[] { "超分后", "最后" }));
            sharpenPosition.unlessId = PresetId; sharpenPosition.unlessValue = "Off";
            sharpenPosition.group = PresetId;
            if (Desktop)
            {
                Live("desktop_output", null, delegate { return "窗口分辨率 · " + Screen.width + " × " + Screen.height; });
            }
            else
                Add(Number(OutputId, "Performance", "OutputScale", "100", "输出分辨率", 75, 150)).unit = "%";
        }
        // NR: the prepass re-reads [NR] when the file changes. v11: [NR] RenderPreset,
        // the NR model (2 = 310.9.16's new model, 1 = 310.8.0's; the feature is made
        // again on a change). v28: 放大后 = NR on the output reduced to NRAfterScale
        // percent (OpenComposite-DLSS-on12-nrscaled), the default; v29: the scale is
        // this tab's. Only the DLSS launcher loads NR 310.9.16 and has NRAfterUpscale.
        // Fallbacks: the NR prepass's own defaults (dx11_nr_prepass.cpp); NRFeatherPercent
        // in vam_performance_config.h.
        AddPage("降噪");
        AddSection("基本");
        Add(OnOff("nr_enabled", "NR", "Enabled", "1", "降噪 NR"));
        if (dlss)
        {
            Add(Choice("nr_model", "NR", "RenderPreset", "2", "模型", new string[] { "2", "1" },
                new string[] { "新", "旧" })).parent = "nr_enabled";
            Add(Choice("nr_after", "Performance", "NRAfterUpscale", "1", "位置", new string[] { "0", "1" },
                new string[] { "放大前", "放大后" })).parent = "nr_enabled";
            Item nrScale = Add(Number("nr_after_scale", "Performance", "NRAfterScale", "50", "处理比例", 25, 100));
            nrScale.unit = "%";
            nrScale.parent = "nr_after";
            byId["nr_after"].pathRule = -1; // RR's NR always runs after it
        }
        Add(Choice("nr_style", "NR", "Style", "0", "风格", new string[] { "0", "1", "2" },
            new string[] { "0（默认）", "1", "2" })).parent = "nr_enabled";
        Item region = Add(Number("nr_region", "NR", "FoveationSize", "66", "处理区域", 10, 100));
        region.unit = "%";
        region.parent = "nr_enabled";
        Item shift = Add(Number("nr_shift", "NR", "RegionShiftY", "0", "区域上移", -30, 30));
        shift.unit = "%";
        shift.parent = "nr_enabled";
        Item feather = Add(Number("nr_feather", "Performance", "NRFeatherPercent", "10", "边缘羽化", 1, 45));
        feather.unit = "%";
        feather.parent = "nr_enabled";
        AddSection("强度");
        Add(Number("nr_intensity", "NR", "Intensity", "100", "整体强度", 0, 100)).parent = "nr_enabled";
        Add(Number("nr_tone", "NR", "LocalToneStrength", "20", "局部色调", 0, 200)).parent = "nr_enabled";
        Add(Number("nr_structure", "NR", "LocalStructureStrength", "70", "局部结构", 0, 200)).parent = "nr_enabled";
        Add(Number("nr_skin", "NR", "SkinStructureStrength", "50", "皮肤结构", 0, 200)).parent = "nr_enabled";
        // VaM Skin Transmission (none of these without it, or while it is off):
        // v17: [Specular] Normalize (bool) and each character's level (0.18.0
        // SkinGlossLevels / SetSkinGlossLevel). v19: 0.20.0's [Diffuse]
        // OverallScattering as three steps, 0 / 0.5 / 1. v20: 0.22.0's [RayTracing]
        // Enabled (the ray tracing does the shadows, the ambient occlusion and the
        // bounce light, for every character and the clothing). v27: 0.22.22's
        // [RayTracing] Ambient (2 遮蔽 + 反弹 / 1 只遮蔽 /
        // 0 关闭) and HairShadows under 光追. v33: 0.22.23's [RayTracing] Lights (1-8, the lights with
        // ray-traced shadows) under 光追. v42: 0.22.66's [RayTracing] PathBounces (0-3, the PT's diffuse bounces) under
        // 光追 on the path-tracing route, where 环境光 does not apply.
        ConfigEntryBase normalize = PluginSetting(SkinPlugin, "Specular", "Normalize");
        ConfigEntryBase rtBounces = null, rtBackground = null;
        if (normalize != null)
        {
            FindSkinSteps();
            rtBounces = PluginSetting(SkinPlugin, "RayTracing", "PathBounces");
            rtBackground = PluginSetting(SkinPlugin, "RayTracing", "PathBackground");
        }
        // v25: VaM Tone Mapping 0.1.0's [General] Mode: 0 off, 1 while Display Lighting lights the scene, 2 always.
        // v26: its 0.2.0 [General] Style, 0 neutral / 1 film (a row of its own: children only show while a parent is "1").
        ConfigEntryBase toneMode = PluginSetting("local.vam.tonemapping", "General", "Mode");
        ConfigEntryBase toneStyle = toneMode != null ? PluginSetting("local.vam.tonemapping", "General", "Style") : null;
        AddPage("光影");
        ConfigEntryBase renderer=PluginSetting(SkinPlugin,"RayTracing","Renderer");
        if(renderer!=null) {
            // Off = VaM's own raster lighting with our skin and hair shading.
            Item route=Add(Choice(RendererId,"RayTracing","Renderer","Legacy","路径追踪",
                new string[]{"Legacy","PathTracing"},new string[]{"关","开"}));
            route.entry=renderer;
            PluginInfo skinInfo;
            if(Chainloader.PluginInfos.TryGetValue(SkinPlugin,out skinInfo) && skinInfo.Instance!=null) {
                System.Reflection.MethodInfo state=skinInfo.Instance.GetType().GetMethod("RendererState",System.Reflection.BindingFlags.Public|System.Reflection.BindingFlags.Static);
                if(state!=null)Live("skin_renderer_state",RendererId,(Func<string>)Delegate.CreateDelegate(typeof(Func<string>),state));
            }
        }
        if (rtBounces != null && renderer != null)
        {
            Item bounces = Add(Choice("skin_rt_bounces", "RayTracing", "PathBounces", "0", "反弹", new string[] { "0", "1", "2", "3" },
                new string[] { "0 次", "1 次", "2 次", "3 次" }));
            bounces.entry = rtBounces;
            bounces.group = RendererId; bounces.pathRule = 1;
        }
        // v51: [RayTracing] PathBackground, where what is not the person is traced for less; the same row in VR and on the desktop. The PT's author rules have no switch.
        if (rtBackground != null && renderer != null)
        {
            Item background = Add(Choice("skin_rt_background", "RayTracing", "PathBackground", "Far", "背景减负", new string[] { "Off", "Far", "All" },
                new string[] { "不减负", "远处", "全部" }));
            background.entry = rtBackground;
            background.group = RendererId; background.pathRule = 1;
        }
        if (toneMode != null)
            Add(Choice("tone_mode", "General", "Mode", "1", "色调映射", new string[] { "0", "1", "2" },
                new string[] { "关闭", "展示照明时", "全局" })).entry = toneMode;
        if (toneStyle != null)
        {
            Item style = Add(Choice("tone_style", "General", "Style", "0", "风格", new string[] { "0", "1" },
                new string[] { "中性", "胶片" }));
            style.entry = toneStyle;
            if (toneMode != null) style.group = "tone_mode";
        }
        AddPage("皮肤");
        if (normalize != null)
        {
            // v52 (11-10): a row for [Subsurface] Strength, 0-1 (0 off). Both routes:
            // the raster route's screen-space SSS, and the PT's before RR (docs/knowledge/skin.md).
            ConfigEntryBase subsurface = PluginSetting(SkinPlugin, "Subsurface", "Strength");
            if (subsurface != null)
            {
                Item sss = Add(Number("skin_sss", "Subsurface", "Strength", "0", "次表面散射", 0, 1));
                sss.entry = subsurface;
                sss.fractional = true;
                sss.step = 0.1f;
            }
            // v54 (11-10): no 整体透光 row; the skin plugin no longer has
            // [Diffuse] OverallScattering (v19-v53 had it: 整体散射 0 / 0.5 / 1, v53 renamed 整体透光 and existing lighting only).
            // v55 (11-10): the 薄处透光 row, on both routes.
            // [General] Strength, the light through thin skin (ears, nostrils, fingers against a light). Both routes: the
            // existing lighting's transmission pass and the PT's PtThinCandidate (docs/knowledge/skin.md). 0 off, 1 as measured.
            ConfigEntryBase thinLight = PluginSetting(SkinPlugin, "General", "Strength");
            if (thinLight != null)
            {
                Item thin = Add(Number("skin_thin", "General", "Strength", "1", "薄处透光", 0, 2));
                thin.entry = thinLight;
                thin.fractional = true;
                thin.step = 0.1f;
            }
            Add(OnOff(SkinNormalizeId, "Specular", "Normalize", "1", "高光归一化")).entry = normalize;
            // One row per character, its label the character and its starting level (TakeSkinSteps).
            if (skinTiers != null)
                for (int k = 0; k < SkinSlots; k++)
                {
                    Item level = Add(Number("skin_level" + k, "", "", "2", "", 0, 4));
                    level.fractional = true;
                    level.step = 0.25f;
                    level.skinSlot = k;
                    level.parent = SkinNormalizeId;
                }
        }
        AddPage("身体");
        ConfigEntryBase sampling = PluginSetting("local.vam.frameclock", "Timeline", "StepSampling");
        if (sampling != null)
        {
            Add(OnOff("motion_sampling", "Timeline", "StepSampling", "1", "动作逐步采样")).entry = sampling;
            // FrameClock's own report: what it adapts now, the body's rate and the last real frame's steps.
            PluginInfo clock;
            if (Chainloader.PluginInfos.TryGetValue("local.vam.frameclock", out clock) && clock.Instance != null)
            {
                System.Reflection.MethodInfo status = clock.Instance.GetType().GetMethod("MenuStatus");
                if (status != null) Live("motion_status", "motion_sampling", (Func<string>)Delegate.CreateDelegate(typeof(Func<string>), status));
            }
        }
        ConfigEntryBase softBody = PluginSetting("local.vam.softbody", "General", "Enabled");
        if (softBody != null) Add(OnOff("softbody_enabled", "General", "Enabled", "1", "乳房软体")).entry = softBody;
        foreach (Item i in items) i.value = i.original = Normalize(i, EntryValue(i));
    }

    // A setting of another BepInEx plugin (string, bool, int or float), when that
    // plugin is loaded and not switched off ([General] Enabled); else null.
    // The enable switch itself stays available while off.
    private ConfigEntryBase PluginSetting(string guid, string section, string key)
    {
        try
        {
            PluginInfo info;
            if (!Chainloader.PluginInfos.TryGetValue(guid, out info) || info == null || info.Instance == null) return null;
            ConfigFile config = info.Instance.Config;
            ConfigEntry<bool> enabled;
            if (!(section == "General" && key == "Enabled") &&
                config.TryGetEntry<bool>("General", "Enabled", out enabled) && !enabled.Value) return null;
            ConfigEntryBase entry = config.ContainsKey(new ConfigDefinition(section, key)) ? config[section, key] : null;
            Type t = entry != null ? entry.SettingType : null;
            return t == typeof(string) || t == typeof(bool) || t == typeof(int) || t == typeof(float) ? entry : null;
        }
        catch (Exception e)
        {
            Logger.LogWarning("VaM VR menu: cannot reach " + guid + " [" + section + "] " + key + ": " + e.Message + "; left out");
            return null;
        }
    }

    // An item's value as its plugin holds it now (a bool as 1 / 0); null for
    // the file items.
    private static string EntryValue(Item item)
    {
        if (item.entry == null) return null;
        object v = item.entry.BoxedValue;
        if (v is bool) return (bool)v ? "1" : "0";
        if (v is float) return ((float)v).ToString("R", System.Globalization.CultureInfo.InvariantCulture);
        return v != null ? v.ToString() : null;
    }

    // A menu value as the plugin setting's own type.
    private static object EntryBoxed(Item item, string value)
    {
        Type t = item.entry.SettingType;
        if (t == typeof(bool)) return value == "1";
        if (t == typeof(int)) return int.Parse(value, System.Globalization.CultureInfo.InvariantCulture);
        if (t == typeof(float)) return float.Parse(value, System.Globalization.CultureInfo.InvariantCulture);
        return value;
    }

    // The file items of a store; another plugin's settings and the skin steps
    // are in none.
    private bool InStore(Item item, Store store)
    {
        return item.entry == null && item.skinSlot < 0 && item.layer == (store == layer);
    }

    // VaM Skin Transmission's level API (0.18.0): SkinGlossLevels() = one line
    // per character, "uid<TAB>starting level<TAB>level" (0.00 .. 4.00; -1 = not
    // known yet), SetSkinGlossLevel(uid, level); null without it.
    private System.Reflection.MethodInfo skinTiers, setSkinTier;

    private void FindSkinSteps()
    {
        skinTiers = setSkinTier = null;
        try
        {
            PluginInfo info;
            if (!Chainloader.PluginInfos.TryGetValue(SkinPlugin, out info) || info == null || info.Instance == null) return;
            Type t = info.Instance.GetType();
            System.Reflection.MethodInfo list = t.GetMethod("SkinGlossLevels", Type.EmptyTypes),
                set = t.GetMethod("SetSkinGlossLevel", new Type[] { typeof(string), typeof(float) });
            if (list != null && set != null && list.IsStatic && set.IsStatic) { skinTiers = list; setSkinTier = set; }
        }
        catch (Exception e)
        {
            Logger.LogWarning("VaM VR menu: cannot reach " + SkinPlugin + "'s skin levels: " + e.Message + "; left out");
        }
    }

    // The characters now in the level rows, in SkinGlossLevels' order; true
    // when a row changed (who is in it, its label or its level).
    private bool TakeSkinSteps()
    {
        if (skinTiers == null) return false;
        string[] lines;
        try { lines = ((skinTiers.Invoke(null, null) as string) ?? "").Split(new char[] { '\n' }, StringSplitOptions.RemoveEmptyEntries); }
        catch (Exception) { return false; }
        bool changed = false;
        foreach (Item i in items)
        {
            if (i.skinSlot < 0) continue;
            string[] f = i.skinSlot < lines.Length ? lines[i.skinSlot].Split('\t') : null;
            string uid = f != null && f.Length >= 3 ? f[0] : null;
            float start = f != null && f.Length >= 3 ? ParseLevel(f[1]) : -1f, level = f != null && f.Length >= 3 ? ParseLevel(f[2]) : -1f;
            string label = uid == null ? "" : "光泽 · " + uid + "\n" + (start >= 0f ? "起始 " + start.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) : "起始值计算中");
            string value = level >= 0f ? Normalize(i, level.ToString("R", System.Globalization.CultureInfo.InvariantCulture)) : i.value;
            if (uid == i.skinUid && label == i.skinLabel && value == i.value && start == i.skinStart) continue;
            i.skinUid = uid; i.skinLabel = label; i.value = value; i.skinStart = start;
            changed = true;
        }
        return changed;
    }

    // a level from SkinGlossLevels' text, -1 when there is none
    private static float ParseLevel(string text)
    {
        float v;
        return float.TryParse(text, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v) &&
               !float.IsNaN(v) && v >= 0f ? Mathf.Min(v, 4f) : -1f;
    }

    // A page, its first section the page's own (often its only one).
    private void AddPage(string title)
    {
        Page page = new Page();
        page.title = title;
        pages.Add(page);
        AddSection(null);
    }

    // A section of the last page: a sub-page in the bar above its rows.
    private void AddSection(string title)
    {
        current = new Section();
        current.title = title;
        pages[pages.Count - 1].sections.Add(current);
    }

    private Item Add(Item item)
    {
        items.Add(item);
        byId[item.id] = item;
        current.members.Add(item);
        return item;
    }

    // A value kept in the file without a control of its own.
    private void Hidden(Item item)
    {
        item.hidden = true;
        items.Add(item);
        byId[item.id] = item;
    }

    // A line of another plugin's state in the current section, in the card of
    // the item `group`: in the menu only, never in a file.
    private Item Live(string id, string group, Func<string> text)
    {
        Item i = new Item();
        i.id = id; i.kind = Kind.Status; i.live = text; i.group = group;
        current.members.Add(i);
        return i;
    }

    private static Item OnOff(string id, string section, string key, string fallback, string label)
    {
        Item i = new Item();
        i.id = id; i.section = section; i.key = key; i.fallback = fallback; i.label = label;
        i.kind = Kind.Toggle; i.min = 0; i.max = 1;
        i.values = new string[] { "0", "1" }; i.names = new string[] { "关", "开" };
        return i;
    }

    // − and + move it by 5, or by 10 on a range longer than 100.
    private static Item Number(string id, string section, string key, string fallback, string label, int min, int max)
    {
        Item i = OnOff(id, section, key, fallback, label);
        i.kind = Kind.Slider; i.min = min; i.max = max;
        i.values = i.names = null;
        i.step = max - min > 100 ? 10f : 5f;
        return i;
    }

    private static Item Choice(string id, string section, string key, string fallback, string label, string[] values, string[] names)
    {
        Item i = OnOff(id, section, key, fallback, label);
        i.kind = Kind.Choice; i.values = values; i.names = names; i.min = 0; i.max = values.Length - 1;
        return i;
    }

    // An item's value in its file's text, as its reader takes it.
    private string ReadItem(Item item, string text)
    {
        if (item.id != MultiplierId) return Normalize(item, VRMenuIni.Get(text, item.section, item.key));
        return VRMenuIni.FrameMultiplier(text, layerReports ? framesMost : 0, SwitchMultiplier).ToString();
    }

    // A file value as its reader takes it: GetPrivateProfileInt and the
    // reader's range for numbers; choices match without case.
    private static string Normalize(Item item, string raw)
    {
        if (item.id == PresetId && raw != null && raw.Trim().ToUpperInvariant() == "RR") return "K"; // as the renderer reads it
        if (raw == null) raw = item.fallback;
        switch (item.kind)
        {
            case Kind.Toggle:
                return ParseInt(raw, ParseInt(item.fallback, 0, 0, 1), 0, int.MaxValue) != 0 ? "1" : "0";
            case Kind.Slider:
                if (item.fractional)
                {
                    float f;
                    if (!float.TryParse(raw, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out f) ||
                        float.IsNaN(f) || float.IsInfinity(f)) f = float.Parse(item.fallback, System.Globalization.CultureInfo.InvariantCulture);
                    return Mathf.Clamp(f, item.min, item.max).ToString("0.##", System.Globalization.CultureInfo.InvariantCulture);
                }
                return ParseInt(raw, ParseInt(item.fallback, item.min, item.min, item.max), item.min, item.max).ToString();
            default:
                foreach (string v in item.values)
                    if (v.Equals(raw.Trim(), StringComparison.OrdinalIgnoreCase)) return v;
                return item.fallback;
        }
    }

    private static int ParseInt(string text, int fallback, int min, int max)
    {
        int v;
        if (text == null) return fallback;
        Match m = Regex.Match(text.Trim(), "^[+-]?\\d+");
        if (!m.Success || !int.TryParse(m.Value, out v)) v = 0;   // present but not a number: 0, as GetPrivateProfileInt
        return Math.Max(min, Math.Min(max, v));
    }

    // The in-game path tracing switch, as the menu last set or read it.
    private bool PathOn()
    {
        Item route;
        return byId.TryGetValue(RendererId, out route) && route.value == "PathTracing";
    }

    private bool Shown(Item item)
    {
        if (item.hidden) return false;
        if (item.pathRule != 0 && (item.pathRule > 0) != PathOn()) return false;
        if (item.skinSlot >= 0 && item.skinUid == null) return false;
        Item other;
        // PT ignores DLSSPreset, so 关闭 hides nothing while PT is on.
        if (item.unlessId != null && byId.TryGetValue(item.unlessId, out other) && other.value == item.unlessValue &&
            !(item.unlessId == PresetId && PathOn())) return false;
        // Every switch above it is on (v29: 处理比例 sits under 降噪位置, which sits under 降噪 NR).
        for (Item i = item; i.parent != null && byId.TryGetValue(i.parent, out other); i = other)
            if (other.value != "1" && (other.id != "nr_after" || !PathOn())) return false;
        return true;
    }

    // ---- files -------------------------------------------------------------

    private static void ReplaceFile(string path, byte[] bytes)
    {
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllBytes(temporary, bytes);
            if (!MoveFileExW(temporary, path, MoveReplaceExisting))
                throw new IOException("MoveFileEx failed, error " + Marshal.GetLastWin32Error());
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); } catch (Exception) { }
        }
    }

    private void Load(Store store, float now)
    {
        if (now < store.nextCheck) return;
        store.nextCheck = now + RetrySeconds;
        if (!ShapeKnown) return;                          // the list of settings needs the layer's answer
        try
        {
            if (items.Count == 0) BuildItems();
            DateTime stamp = File.GetLastWriteTimeUtc(store.path);
            string text = new VRMenuIni.Document(File.ReadAllBytes(store.path)).text;
            foreach (Item i in items)
                if (InStore(i, store)) i.value = i.original = ReadItem(i, text);
            store.stamp = stamp;
            store.loaded = true;
            store.failed = false;
            Item multiplier;
            if (store == layer && launchMultiplier == 0 && byId.TryGetValue(MultiplierId, out multiplier))
                launchMultiplier = int.Parse(multiplier.value);
            SyncMenu();
        }
        catch (Exception e)
        {
            if (!store.failed) Logger.LogWarning("VaM VR menu: cannot read " + store.path + ": " + e.Message + " (retrying)");
            store.failed = true;
        }
    }

    // Writes this store's changed values, at most every WriteSeconds.
    private void Flush(Store store, float now)
    {
        if (!store.pending || !store.loaded || now < store.nextWrite) return;
        store.nextWrite = now + WriteSeconds;
        List<VRMenuIni.Edit> edits = new List<VRMenuIni.Edit>();
        List<Item> written = new List<Item>();
        foreach (Item i in items)
        {
            if (!i.dirty || !InStore(i, store)) continue;
            if (i.id == MultiplierId) edits.AddRange(VRMenuIni.FrameMultiplierEdits(int.Parse(i.value)));
            else edits.Add(new VRMenuIni.Edit(i.section, i.key, i.value));
            written.Add(i);
        }
        try
        {
            // Never create the file: a missing one means its owner is gone.
            if (!File.Exists(store.path)) throw new FileNotFoundException("the file is gone", store.path);
            VRMenuIni.Document document = new VRMenuIni.Document(File.ReadAllBytes(store.path));
            string updated = VRMenuIni.Set(document.text, edits);
            if (updated != document.text) ReplaceFile(store.path, document.Bytes(updated));
            store.stamp = File.GetLastWriteTimeUtc(store.path);
            foreach (Item i in written)
            {
                i.dirty = false;
                unlogged[store.name + " " + i.key] = i.value;
            }
            store.pending = false;
            if (store.failed) Logger.LogInfo("VaM VR menu: " + store.name + " written again");
            store.failed = false;
            message = "已写入。";
        }
        catch (Exception e)
        {
            store.nextWrite = now + RetrySeconds;
            if (!store.failed) Logger.LogWarning("VaM VR menu: cannot write " + store.path + ": " + e.Message + " (retrying)");
            store.failed = true;
            message = "写入失败，正在重试：" + e.Message;
        }
        nextStatus = 0f;
    }

    // Takes over what the launch panel or the tray wrote meanwhile.
    private void CheckExternal(Store store, float now)
    {
        if (!store.loaded || now < store.nextCheck) return;
        store.nextCheck = now + CheckSeconds;
        try
        {
            DateTime stamp = File.GetLastWriteTimeUtc(store.path);
            if (stamp == store.stamp) return;
            string text = new VRMenuIni.Document(File.ReadAllBytes(store.path)).text;
            store.stamp = stamp;
            List<string> taken = new List<string>();
            foreach (Item i in items)
            {
                if (!InStore(i, store) || i.dirty) continue;
                string v = ReadItem(i, text);
                if (v == i.value) continue;
                i.value = v;
                taken.Add(i.key + "=" + v);
            }
            if (taken.Count == 0) return;
            Logger.LogInfo("VaM VR menu: " + store.name + " changed outside the menu: " + string.Join(", ", taken.ToArray()));
            message = (store == layer ? "OFXR 托盘" : "启动器") + "改了设置，已同步到这里。";
            SyncMenu();
        }
        catch (Exception) { }                     // being replaced or briefly locked: the next check
    }

    private void LogWrites(float now)
    {
        nextLog = now + LogSeconds;
        List<string> parts = new List<string>();
        foreach (KeyValuePair<string, string> kv in unlogged) parts.Add(kv.Key + "=" + kv.Value);
        unlogged.Clear();
        Logger.LogInfo("VaM VR menu: wrote " + string.Join(", ", parts.ToArray()));
    }

    // ---- changes from the menu --------------------------------------------

    private void SetValue(Item item, string value)
    {
        if (item.id == PresetId && Array.IndexOf(item.values, value) < 0) return; // RR only comes with PT
        if (syncing || value == item.value) return;
        message = "";
        Change(item, value);
        FollowModel(item);
        SyncMenu();
    }

    // The render scale belongs to the model: choosing one brings back its own
    // (RenderScaleK/L/M), moving the slider keeps it for the model chosen.
    private void FollowModel(Item item)
    {
        Item preset, scale, own;
        if (!byId.TryGetValue(PresetId, out preset) || !byId.TryGetValue(ScaleId, out scale) ||
            !byId.TryGetValue(ScaleId + "_" + preset.value, out own)) return;
        if (item == preset && scale.value != own.value) Change(scale, own.value);
        else if (item == scale && own.value != scale.value) Change(own, scale.value);
    }

    private void Change(Item item, string value)
    {
        item.value = value;
        if (item.skinSlot >= 0)
        {
            // A character's level: kept by the skin plugin for that skin.
            try
            {
                string r = setSkinTier.Invoke(null, new object[] { item.skinUid, float.Parse(value, System.Globalization.CultureInfo.InvariantCulture) }) as string;
                if (r != null && r.StartsWith("error")) throw new Exception(r);
                unlogged["skin level " + item.skinUid] = value;
                message = "已切换。";
            }
            catch (Exception e)
            {
                Exception x = e.InnerException ?? e;
                Logger.LogWarning("VaM VR menu: cannot set " + item.skinUid + "'s skin level to " + value + ": " + x.Message);
                message = "切换失败：" + x.Message;
                TakeSkinSteps();
            }
            nextStatus = 0f;
            return;
        }
        if (item.entry != null)
        {
            // Another plugin's setting: set now; the plugin follows its
            // SettingChanged, and BepInEx saves the config file.
            try
            {
                item.entry.BoxedValue = EntryBoxed(item, value);
                unlogged[Path.GetFileName(item.entry.ConfigFile.ConfigFilePath) + " " + item.key] = value;
                message = "已切换。";
            }
            catch (Exception e)
            {
                item.value = Normalize(item, EntryValue(item));
                Logger.LogWarning("VaM VR menu: cannot set " + item.section + "." + item.key + " to " + value + ": " + e.Message);
                message = "切换失败：" + e.Message;
            }
            nextStatus = 0f;
            return;
        }
        item.dirty = true;
        (item.layer ? layer : performance).pending = true;
    }

    // Another plugin's settings, changed elsewhere meanwhile (its own config
    // manager, a test): looked at while the tab is seen.
    private void TakeEntryValues()
    {
        bool changed = TakeSkinSteps();
        foreach (Item i in items)
        {
            if (i.entry == null) continue;
            string v = Normalize(i, EntryValue(i));
            if (v == i.value) continue;
            i.value = v;
            changed = true;
        }
        if (changed) SyncMenu();
    }

    // Puts every value back to what this launch started with.
    private void RestoreLaunchValues()
    {
        if (syncing) return;
        foreach (Item i in items)
            if (i.skinSlot < 0 && i.value != i.original) Change(i, i.original);   // the levels are the skins' own
        message = "已恢复本次启动时的设置。";
        SyncMenu();
    }
}
