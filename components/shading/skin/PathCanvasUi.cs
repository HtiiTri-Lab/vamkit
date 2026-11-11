// 0.23.3: canvas elements drawn with their own material are UI over PT, whatever their shader.
//
// PT keeps only what it traces plus the draws the native overlay capture (path_overlay.inl) replays as UI. That capture
// knew UI by VaM's stock UI pixel programs only, so a canvas element with a plugin's own UI shader vanished in PT and
// only its text (VaM's default UI material) stayed: every menu button of one stage pack is a VaMStory action panel whose
// background Image uses VaMStory's HZM/UI/UIShader01 (24 pixel programs, none in the stock catalog).
//
// Here: the canvas elements whose material is not the default UI material are tracked from Graphic.OnEnable /
// OnDisable / material set (no scene search); once a frame the textures they draw (Graphic.mainTexture: sprite, image,
// font; never Unity's default white/black or a render texture, which other draws bind too) go to the native side, kept
// 8 frames after their last use (the render thread runs behind). A draw the scene registry does not know that samples
// one of them is replayed as UI. Allocation-free while the set does not change.
using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using UnityEngine.UI;

public static class PathCanvasUi
{
    internal delegate void SetFn(long[] textures, int count);
    internal delegate int DumpFn([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string dir);
    private static SetFn set;
    private static DumpFn dump;
    private static readonly HashSet<Graphic> custom = new HashSet<Graphic>();
    private static readonly List<Graphic> dead = new List<Graphic>();
    private static readonly Dictionary<Texture, int> used = new Dictionary<Texture, int>();
    private static readonly List<Texture> expired = new List<Texture>();
    private static readonly List<long> current = new List<long>();
    private static long[] sent = new long[0];
    private static Texture white, black;
    private static int lastFrame = -1, sends;
    private const int KeepFrames = 8;

    internal static void Setup(Harmony harmony, SetFn native, DumpFn dumpLayers)
    {
        set = native; dump = dumpLayers;
        white = Texture2D.whiteTexture; black = Texture2D.blackTexture;
        BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        Type t = typeof(Graphic);
        harmony.Patch(t.GetMethod("OnEnable", flags), null, new HarmonyMethod(typeof(PathCanvasUi).GetMethod("Enabled")), null, null);
        harmony.Patch(t.GetMethod("OnDisable", flags), null, new HarmonyMethod(typeof(PathCanvasUi).GetMethod("Disabled")), null, null);
        harmony.Patch(t.GetProperty("material").GetSetMethod(), null, new HarmonyMethod(typeof(PathCanvasUi).GetMethod("MaterialSet")), null, null);
        foreach (Graphic g in Resources.FindObjectsOfTypeAll<Graphic>()) if (g.isActiveAndEnabled) Track(g);
    }

    public static void Enabled(Graphic __instance) { Track(__instance); }
    public static void Disabled(Graphic __instance) { if (!ReferenceEquals(__instance, null)) custom.Remove(__instance); }
    public static void MaterialSet(Graphic __instance) { if (__instance != null && __instance.isActiveAndEnabled) Track(__instance); }

    private static void Track(Graphic g)
    {
        if (g == null) return;
        Material m = g.material;
        if (m != null && m != g.defaultMaterial) custom.Add(g); else custom.Remove(g);
    }

    // Once a frame (RtScene.PreCull). pt: the PT route is selected; otherwise the set is emptied once.
    internal static void Frame(bool pt)
    {
        if (set == null) return;
        int frame = Time.frameCount;
        if (frame == lastFrame) return;
        lastFrame = frame;
        if (pt)
        {
            foreach (Graphic g in custom)
            {
                if (g == null) { dead.Add(g); continue; }
                if (!g.isActiveAndEnabled || g.canvas == null) continue;
                Texture t = g.mainTexture;
                if (t == null || t is RenderTexture || t == white || t == black) continue;
                used[t] = frame;
            }
            for (int i = 0; i < dead.Count; i++) custom.Remove(dead[i]);
            dead.Clear();
        }
        current.Clear();
        foreach (KeyValuePair<Texture, int> kv in used)
        {
            if (!pt || kv.Key == null || frame - kv.Value > KeepFrames) { expired.Add(kv.Key); continue; }
            long p = RtScene.TexturePtr(kv.Key).ToInt64();
            if (p != 0) current.Add(p);
        }
        for (int i = 0; i < expired.Count; i++) used.Remove(expired[i]);
        expired.Clear();
        current.Sort();
        bool same = current.Count == sent.Length;
        for (int i = 0; same && i < sent.Length; i++) same = current[i] == sent[i];
        if (same) return;
        sent = current.ToArray();
        set(sent, sent.Length);
        sends++;
    }

    // Test switch: false = 0.23.2 behaviour (only the stock UI catalog), true = this release.
    public static string Test(bool on)
    {
        if (set != null) set(null, on ? -2 : -1);
        return Status();
    }

    // 0.23.4 test switch: false = 0.23.3 behaviour (author shaders inferred, no effect replay, Standard copies and the
    // Roughness setup not translated), true = this release. Materials are re-read every frame.
    public static string TestAuthor(bool on)
    {
        RtScene.authorOff = !on;
        if (set != null) set(null, on ? -6 : -5);
        return "author shaders " + (on ? "on" : "off (0.23.3)");
    }

    // 0.23.5 test: false = the camera-only outline shell traced like any surface again (0.23.4)
    public static string TestCameraOnly(bool on)
    {
        RtScene.CameraOnlyTest = on;
        return "camera-only " + (on ? "on" : "off (0.23.4)");
    }

    // Test switch: false = 0.23.2 behaviour (UI drawn after PT's late composition is dropped), true = this release
    // (it goes to the after-tone layer, path_overlay.inl gOverlayLateOff).
    public static string TestLate(bool on)
    {
        if (set != null) set(null, on ? -4 : -3);
        return Status();
    }

    // Test: the next frame start writes the previous frame's PT UI replay layers into dir (path_overlay.inl).
    public static string Dump(string dir)
    {
        return dump != null && dump(dir) == 1 ? "requested" : "unavailable";
    }

    public static string Status()
    {
        return "custom canvas elements " + custom.Count + ", textures " + sent.Length + ", sends " + sends;
    }
}
