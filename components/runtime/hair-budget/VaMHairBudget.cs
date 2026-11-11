// VaM hair budget (BepInEx 5, Unity 2018.1, C# 3).
//
// VaM physics hair (GPUTools) draws, for every scalp triangle of guide strands
// ("patch"), hairMultiplier interpolated strands of curveDensity segments each
// (hardware tessellation, _TessFactor = hairMultiplier, curveDensity), per eye
// and per pass (base pass, extra pixel lights, shadow maps). Measured, a strand
// costs about its segments plus 25 more, so one item costs about
//   patches x hairMultiplier x (curveDensity + 25) x 24/49
// segments of a curveDensity-24 hair (HairBudgetMath.Cost); budgets are in it.
// Presets pick both values freely: across the installed library one person's
// hair ranges from tens of thousands to ~2.8 million segments.
//
// The plugin keeps every preset's own values (the sliders, scene and preset
// files still hold them; saving is unaffected) and lowers only the values the
// renderer and the solver read (HairLODSettings.FixedDensity/FixedDetail/
// FixedWidth, HairPhysicsSettings.Iterations), which VaM itself only writes
// from the slider callbacks:
//   1. per item, hairMultiplier x curveDensity <= DensityMax;
//   2. per person, then per scene, the cost total <= budget, by water-filling:
//      items costlier per patch than a common level come down to it; lighter
//      items, and detail pieces costing under 5% of the budget, keep their
//      values;
//   3. each reduction is split between curveDensity and hairMultiplier, with
//      floors that keep strands following their guides and curls, and width
//      grows by up to WidthCompensation to keep the coverage;
//   4. physics iterations <= IterationsMax; optionally no hair shadows.
// Steps 2 and 3 are in HairBudgetMath.cs, shared with the offline test.
// The values are worked out once for a situation and written again only when
// it changes: a hair item appears or goes, its sliders or the settings change.
// Hair items are known from a Harmony postfix on HairSimControl.Awake, never
// by searching the scene (1.0 searched every 2 s, two ~95 ms frames in a
// scene with many objects). 1.0's distance LOD, which rebuilt hair as the
// viewer moved, is gone.
// Separately, hair can get its own copy of VaM's physics rate settings
// (VaMHairBudget.HairRate.cs).
// Settings: BepInEx\config\VaMHairBudget.ini, re-read about once a second, and
// a "头发预算" tab in VaM's main menu (VaMHairBudget.Menu.cs).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using BepInEx;
using GPUTools.Hair.Scripts;
using GPUTools.Hair.Scripts.Geometry.Abstract;
using UnityEngine;

[BepInPlugin("local.vam.hairbudget", "VaM Hair Budget", "1.2.1")]
public sealed partial class VaMHairBudget : BaseUnityPlugin
{
    private const float ScanSeconds = 2f, CheckSeconds = 0.25f, ConfigSeconds = 1f;
    private const float WidthSliderMax = 0.001f;   // HairSimControl "width" slider range

    private sealed class Limits
    {
        public string name;
        public bool enabled;
        public long personBudget, sceneBudget;       // cost (HairBudgetMath.Cost), 0 = unlimited
        public int densityMax, curveDensityMin, hairMultiplierMin, iterationsMax;
        public float widthCompensation;

        public Limits(string name, bool enabled, long personK, long sceneK, int densityMax, int curveDensityMin,
            int hairMultiplierMin, int iterationsMax, float widthCompensation)
        {
            this.name = name; this.enabled = enabled;
            personBudget = personK * 1000; sceneBudget = sceneK * 1000;
            this.densityMax = densityMax; this.curveDensityMin = curveDensityMin;
            this.hairMultiplierMin = hairMultiplierMin; this.iterationsMax = iterationsMax;
            this.widthCompensation = widthCompensation;
        }
    }

    private static readonly Limits Off = new Limits("Off", false, 0, 0, 0, 0, 0, 0, 1f);

    private static Limits Preset(string name)
    {
        switch (name.ToLowerInvariant())
        {
            case "off": return Off;
            case "high": return new Limits("High", true, 1200, 3000, 2048, 16, 6, 3, 1.3f);
            case "balanced": return new Limits("Balanced", true, 600, 1500, 1024, 12, 4, 2, 1.5f);
            case "performance": return new Limits("Performance", true, 350, 800, 640, 10, 3, 2, 1.6f);
        }
        return null;
    }

    private sealed class Hair
    {
        public HairSimControl control;
        public HairSettings settings;
        public JSONStorableFloat curveDensity, hairMultiplier, width, iterations;
        public string atom, name;
        public MeshRenderer renderer;
        public readonly HairCost cost = new HairCost();   // patches, segments, requested values, curls
        public int strands, requestedIterations;
        public float requestedWidth;
        public bool styleMode, active;
        public bool written;                    // last values written to the renderer and solver
        public int densityWritten, detailWritten, iterationsWritten;
        public float widthWritten;
        public bool shadowsTouched, originalShadows;
        public string lastReport;
    }

    private readonly Dictionary<HairSimControl, Hair> hairs = new Dictionary<HairSimControl, Hair>();
    private readonly Dictionary<string, string> atomReports = new Dictionary<string, string>();
    private Limits limits;
    private bool disableShadows, paused, dirty = true;
    private KeyCode toggleKey = KeyCode.None;
    private bool toggleCtrl, toggleShift, toggleAlt;
    private string configPath;
    private DateTime configStamp = DateTime.MinValue;
    private float nextScan, nextCheck, nextConfig;
    private bool failed;
    // Every HairSimControl made since the plugin started (ControlAwake), plus
    // those that were there before; destroyed ones are dropped in Scan.
    private static readonly List<HairSimControl> controls = new List<HairSimControl>();
    private bool tracking;

    private void Awake()
    {
        configPath = Path.Combine(Paths.ConfigPath, "VaMHairBudget.ini");
        try
        {
            if (!File.Exists(configPath))
            {
                // The commented template (VaMHairBudget.ini) is embedded at build time.
                using (Stream template = typeof(VaMHairBudget).Assembly.GetManifestResourceStream("VaMHairBudget.ini"))
                using (FileStream output = File.Create(configPath))
                {
                    byte[] buffer = new byte[8192];
                    int n;
                    while ((n = template.Read(buffer, 0, buffer.Length)) > 0) output.Write(buffer, 0, n);
                }
            }
        }
        catch (Exception e)
        {
            Logger.LogWarning("VaM hair budget: cannot write " + configPath + ": " + e.Message);
        }
        LoadConfig();
        if (limits == null) ApplySettings(false);
        StartTracking();
        Logger.LogInfo("VaM hair budget: profile " + limits.name + (disableShadows ? ", hair shadows off" : "") +
            "; settings " + configPath);
    }

    private void Update()
    {
        if (failed) return;
        try
        {
            float now = Time.realtimeSinceStartup;
            if (now >= nextConfig)
            {
                nextConfig = now + ConfigSeconds;
                if (LoadConfig()) dirty = true;
            }
            if (ToggleRequested())
            {
                paused = !paused;
                dirty = true;
                Logger.LogInfo(paused ? "VaM hair budget: paused, every hair item back to its own values"
                                      : "VaM hair budget: resumed, profile " + limits.name);
                SyncMenu();
            }
            if (saveAt > 0f && now >= saveAt)
            {
                saveAt = -1f;
                SaveConfig();
            }
            UpdateHairRate(now);
            UpdateMenu(now);
            if (now >= nextScan)
            {
                nextScan = now + ScanSeconds;
                if (Scan()) dirty = true;
            }
            if (now >= nextCheck)
            {
                nextCheck = now + CheckSeconds;
                if (Check()) dirty = true;
            }
            if (dirty)
            {
                dirty = false;
                Evaluate();
            }
        }
        catch (Exception e)
        {
            // Hand every item its own values back and stop for this session.
            failed = true;
            hairStep = 0f;                       // hair back on VaM's physics rate
            Logger.LogError("VaM hair budget stopped: " + e);
            try { RestoreAll(); } catch (Exception) { }
        }
    }

    // ---- tracking --------------------------------------------------------

    public static void ControlAwake(HairSimControl __instance)
    {
        if (__instance != null && !controls.Contains(__instance)) controls.Add(__instance);
    }

    private void StartTracking()
    {
        try
        {
            System.Reflection.MethodInfo awake = typeof(HairSimControl).GetMethod("Awake",
                System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
            if (awake == null) throw new MissingMethodException("HairSimControl", "Awake");
            new HarmonyLib.Harmony("local.vam.hairbudget.tracking").Patch(awake, null,
                new HarmonyLib.HarmonyMethod(typeof(VaMHairBudget).GetMethod("ControlAwake")), null, null);
            // Those made before the hook, inactive ones included: the only search.
            // It also returns prefabs from asset bundles, which are in no scene.
            foreach (UnityEngine.Object o in Resources.FindObjectsOfTypeAll(typeof(HairSimControl)))
            {
                HairSimControl c = o as HairSimControl;
                if (c != null && c.gameObject.scene.IsValid()) ControlAwake(c);
            }
            tracking = true;
            Logger.LogInfo("VaM hair budget: hair items tracked from HairSimControl.Awake (" + controls.Count + " so far), no scene search");
        }
        catch (Exception e)
        {
            tracking = false;
            Logger.LogWarning("VaM hair budget: cannot hook HairSimControl.Awake (" + e.Message + "); searching the scene every " + ScanSeconds + " s instead");
        }
    }

    private bool Scan()
    {
        bool changed = false;
        IList<HairSimControl> found;
        if (tracking)
        {
            controls.RemoveAll(delegate(HairSimControl c) { return c == null; });
            found = controls;
        }
        else found = FindObjectsOfType<HairSimControl>();
        HashSet<HairSimControl> seen = new HashSet<HairSimControl>();
        foreach (HairSimControl c in found)
        {
            // Only items whose object is active, as FindObjectsOfType returns.
            if (c == null || !c.gameObject.activeInHierarchy) continue;
            seen.Add(c);
            Hair h;
            if (!hairs.TryGetValue(c, out h))
            {
                h = Track(c);
                if (h == null) continue;
                hairs[c] = h;
                changed = true;
            }
            if (RefreshGeometry(h)) changed = true;
        }
        // An inactive item draws nothing; when it comes back it is tracked
        // again from its sliders.
        List<HairSimControl> gone = null;
        foreach (KeyValuePair<HairSimControl, Hair> kv in hairs)
        {
            if (kv.Key == null || !seen.Contains(kv.Key))
            {
                if (gone == null) gone = new List<HairSimControl>();
                gone.Add(kv.Key);
            }
        }
        if (gone != null)
        {
            foreach (HairSimControl c in gone)
            {
                Hair h = hairs[c];
                if (c != null && h.settings != null) Restore(h);
                hairs.Remove(c);
            }
            changed = true;
        }
        return changed;
    }

    private static Hair Track(HairSimControl c)
    {
        if (c.hairSettings == null) return null;
        Hair h = new Hair();
        h.control = c;
        h.settings = c.hairSettings;
        h.curveDensity = c.GetFloatJSONParam("curveDensity");
        h.hairMultiplier = c.GetFloatJSONParam("hairMultiplier");
        h.width = c.GetFloatJSONParam("width");
        h.iterations = c.GetFloatJSONParam("iterations");
        if (h.curveDensity == null || h.hairMultiplier == null || h.width == null || h.iterations == null) return null;
        h.atom = c.containingAtom != null ? c.containingAtom.uid : "(no atom)";
        DAZDynamicItem item = c.GetComponentInParent<DAZDynamicItem>();
        h.name = item != null && !string.IsNullOrEmpty(item.displayName) ? item.displayName : c.name;
        ReadRequested(h);
        return h;
    }

    private static bool RefreshGeometry(Hair h)
    {
        int patches = 0, strands = 0, segments = 0;
        try
        {
            GeometryProviderBase provider = h.settings.StandsSettings != null ? h.settings.StandsSettings.Provider : null;
            if (provider != null)
            {
                int[] indices = provider.GetIndices();
                patches = indices != null ? indices.Length / 3 : 0;
                segments = provider.GetSegmentsNum();
                strands = segments > 0 ? provider.GetStandsNum() : 0;
            }
        }
        catch (Exception)
        {
            patches = 0;   // not generated yet
        }
        if (h.renderer == null && h.settings.HairBuidCommand != null && h.settings.HairBuidCommand.render != null)
            h.renderer = h.settings.HairBuidCommand.render.GetComponent<MeshRenderer>();
        bool changed = patches != h.cost.patches || segments != h.cost.segments;
        h.cost.patches = patches; h.cost.segments = segments; h.strands = strands;
        return changed;
    }

    private static bool ReadRequested(Hair h)
    {
        int density = Mathf.FloorToInt(h.curveDensity.val);
        int detail = Mathf.FloorToInt(h.hairMultiplier.val);
        int iterations = Mathf.FloorToInt(h.iterations.val);
        float width = h.width.val;
        var r = h.settings.RenderSettings;
        float curl = r != null ? r.WavinessScale * Mathf.Max(r.WavinessRoot, Mathf.Max(r.WavinessMid, r.WavinessTip)) : 0f;
        float frequency = r != null ? r.WavinessFrequency : 0f;
        bool style = h.settings.PhysicsSettings != null && h.settings.PhysicsSettings.StyleMode;
        bool active = h.control.isActiveAndEnabled && !h.control.renderSuspend;
        HairCost c = h.cost;
        bool changed = density != c.requestedDensity || detail != c.requestedDetail || iterations != h.requestedIterations ||
            width != h.requestedWidth || curl != c.curlStrength || frequency != c.curlFrequency ||
            style != h.styleMode || active != h.active;
        c.requestedDensity = density; c.requestedDetail = detail; c.curlStrength = curl; c.curlFrequency = frequency;
        h.requestedIterations = iterations; h.requestedWidth = width; h.styleMode = style; h.active = active;
        return changed;
    }

    private bool Check()
    {
        bool changed = false;
        foreach (Hair h in hairs.Values)
        {
            if (h.control == null || h.settings == null) { changed = true; nextScan = 0f; continue; }
            if (ReadRequested(h)) changed = true;
            // A slider callback (preset load, UI, plugins) wrote its own value.
            if (h.written && (h.settings.LODSettings.FixedDensity != h.densityWritten || h.settings.LODSettings.FixedDetail != h.detailWritten ||
                h.settings.LODSettings.FixedWidth != h.widthWritten || h.settings.PhysicsSettings.Iterations != h.iterationsWritten))
                changed = true;
        }
        return changed;
    }

    // ---- budget ----------------------------------------------------------

    private void Evaluate()
    {
        Limits l = paused ? Off : limits;
        List<Hair> live = new List<Hair>();
        List<HairCost> liveCosts = new List<HairCost>();
        Dictionary<string, List<Hair>> people = new Dictionary<string, List<Hair>>();
        Dictionary<string, List<HairCost>> peopleCosts = new Dictionary<string, List<HairCost>>();
        foreach (Hair h in hairs.Values)
        {
            if (h.control == null || h.settings == null) continue;
            HairCost c = h.cost;
            c.level = HairBudgetMath.RequestedLevel(c, 0);
            if (!l.enabled || !h.active || h.styleMode || c.patches <= 0) continue;
            c.level = HairBudgetMath.RequestedLevel(c, l.densityMax);
            live.Add(h);
            liveCosts.Add(c);
            List<Hair> group;
            List<HairCost> costs;
            if (!people.TryGetValue(h.atom, out group))
            {
                people[h.atom] = group = new List<Hair>();
                peopleCosts[h.atom] = costs = new List<HairCost>();
            }
            else costs = peopleCosts[h.atom];
            group.Add(h);
            costs.Add(c);
        }
        float floor = HairBudgetMath.Cost(Mathf.Max(1, l.curveDensityMin), Mathf.Max(1, l.hairMultiplierMin));
        foreach (List<HairCost> costs in peopleCosts.Values) HairBudgetMath.WaterFill(costs, l.personBudget, floor);
        HairBudgetMath.WaterFill(liveCosts, l.sceneBudget, floor);

        HashSet<Hair> limited = new HashSet<Hair>(live);
        foreach (Hair h in hairs.Values)
        {
            if (h.control == null || h.settings == null) continue;
            if (limited.Contains(h)) Apply(h, l); else Restore(h);
        }
        Report(people);
        CollectStats();
    }

    private void Apply(Hair h, Limits l)
    {
        HairCost c = h.cost;
        HairBudgetMath.Split(c, l.curveDensityMin, l.hairMultiplierMin);
        float w = h.requestedWidth;
        float widen = HairBudgetMath.WidthScale(c.requestedDetail, c.detail, l.widthCompensation);
        if (widen > 1f) w = Mathf.Max(w, Mathf.Min(w * widen, WidthSliderMax));
        int it = h.requestedIterations;
        if (l.iterationsMax > 0) it = Mathf.Min(it, l.iterationsMax);
        Write(h, c.density, c.detail, w, it);
        if (disableShadows && !paused)
        {
            if (!h.shadowsTouched) { h.originalShadows = h.settings.ShadowSettings.CastShadows; h.shadowsTouched = true; }
            h.settings.ShadowSettings.CastShadows = false;
        }
        else RestoreShadows(h);
    }

    private static void Restore(Hair h)
    {
        Write(h, h.cost.requestedDensity, h.cost.requestedDetail, h.requestedWidth, h.requestedIterations);
        RestoreShadows(h);
    }

    private void RestoreAll()
    {
        foreach (Hair h in hairs.Values) if (h.control != null && h.settings != null) Restore(h);
    }

    private static void RestoreShadows(Hair h)
    {
        if (!h.shadowsTouched) return;
        h.settings.ShadowSettings.CastShadows = h.originalShadows;
        h.shadowsTouched = false;
    }

    private static void Write(Hair h, int density, int detail, float width, int iterations)
    {
        h.settings.LODSettings.FixedDensity = density;
        h.settings.LODSettings.FixedDetail = detail;
        h.settings.LODSettings.FixedWidth = width;
        h.settings.PhysicsSettings.Iterations = iterations;
        h.densityWritten = density; h.detailWritten = detail; h.widthWritten = width; h.iterationsWritten = iterations;
        h.written = true;
    }

    // ---- logging ---------------------------------------------------------

    private void Report(Dictionary<string, List<Hair>> people)
    {
        Limits l = paused ? Off : limits;
        foreach (KeyValuePair<string, List<Hair>> kv in people)
        {
            double requested = 0, drawn = 0, requestedSegments = 0, drawnSegments = 0;
            int lowered = 0;
            StringBuilder items = new StringBuilder();
            foreach (Hair h in kv.Value)
            {
                HairCost c = h.cost;
                requested += (double)c.patches * HairBudgetMath.Cost(c.requestedDensity, c.requestedDetail);
                drawn += (double)c.patches * HairBudgetMath.Cost(h.densityWritten, h.detailWritten);
                requestedSegments += (double)c.patches * c.requestedDensity * c.requestedDetail;
                drawnSegments += (double)c.patches * h.densityWritten * h.detailWritten;
                bool changed = h.densityWritten != c.requestedDensity || h.detailWritten != c.requestedDetail ||
                    h.iterationsWritten != h.requestedIterations || h.widthWritten != h.requestedWidth;
                string line = "  '" + h.name + "' patches " + c.patches + ", strands " + h.strands + " x " + c.segments + " segments: ";
                if (changed)
                {
                    lowered++;
                    line += "curveDensity " + c.requestedDensity + "->" + h.densityWritten +
                        ", hairMultiplier " + c.requestedDetail + "->" + h.detailWritten +
                        (h.widthWritten != h.requestedWidth ? ", width x" + (h.widthWritten / Mathf.Max(1e-9f, h.requestedWidth)).ToString("0.00") : "") +
                        (h.iterationsWritten != h.requestedIterations ? ", iterations " + h.requestedIterations + "->" + h.iterationsWritten : "") +
                        (HairBudgetMath.CurlPoints(c) > 0 && h.densityWritten == HairBudgetMath.DensityFloor(c, l.curveDensityMin) &&
                         h.densityWritten < c.requestedDensity ? " (curveDensity kept for curls)" : "");
                }
                else line += "unchanged (curveDensity " + h.densityWritten + ", hairMultiplier " + h.detailWritten + ")";
                if (line != h.lastReport) { items.Append('\n').Append(line); h.lastReport = line; }
            }
            string summary = kv.Value.Count + " hair item(s), cost " + Thousands(requested) + " -> " + Thousands(drawn) +
                " (strand segments " + Thousands(requestedSegments) + " -> " + Thousands(drawnSegments) + "), " + lowered + " lowered";
            string last;
            atomReports.TryGetValue(kv.Key, out last);
            if (summary == last && items.Length == 0) continue;
            atomReports[kv.Key] = summary;
            Logger.LogInfo("VaM hair budget [" + l.name + "] " + kv.Key + ": " + summary +
                (l.personBudget > 0 ? " (person budget " + Thousands(l.personBudget) + ")" : "") + items);
        }
    }

    private static string Thousands(double v)
    {
        return (v / 1000.0).ToString("0") + "k";
    }

    // ---- settings --------------------------------------------------------

    private bool ToggleRequested()
    {
        if (toggleKey == KeyCode.None || !Input.GetKeyDown(toggleKey)) return false;
        bool ctrl = Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl);
        bool shift = Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift);
        bool alt = Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt);
        return ctrl == toggleCtrl && shift == toggleShift && alt == toggleAlt;
    }

    // Everything the ini (and the in-game menu) can set. Custom values are kept
    // while a fixed profile is selected.
    private sealed class Settings
    {
        public string profile = "Balanced";
        public bool disableShadows;
        public string toggleKey = "";
        public string menuTab = "MainMenu";
        public int personK = 600, sceneK = 1500, densityMax = 1024, curveDensityMin = 12, hairMultiplierMin = 4, iterationsMax = 2;
        public float widthCompensation = 1.5f;
        public string hairRate = "VaM";        // HairRateIds
        public int hairCap = 1;                 // 1..3
    }

    private Settings settings = new Settings();
    private float saveAt = -1f;

    private Limits Derive(Settings s)
    {
        if (s.profile.Equals("Custom", StringComparison.OrdinalIgnoreCase))
        {
            s.profile = "Custom";
            return new Limits("Custom", true, s.personK, s.sceneK, s.densityMax, Math.Max(2, s.curveDensityMin),
                Math.Max(1, s.hairMultiplierMin), s.iterationsMax, s.widthCompensation);
        }
        Limits l = Preset(s.profile);
        if (l != null)
        {
            s.profile = l.name;
            return l;
        }
        Logger.LogWarning("VaM hair budget: unknown Profile=" + s.profile + "; using Balanced");
        s.profile = "Balanced";
        return Preset("balanced");
    }

    private bool LoadConfig()
    {
        DateTime stamp;
        try
        {
            if (!File.Exists(configPath)) return false;
            stamp = File.GetLastWriteTimeUtc(configPath);
            if (stamp == configStamp) return false;
        }
        catch (Exception) { return false; }
        configStamp = stamp;

        Dictionary<string, string> v = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            string section = "";
            foreach (string raw in File.ReadAllLines(configPath))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                if (line[0] == '[') { section = line.Trim('[', ']').Trim(); continue; }
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string value = line.Substring(eq + 1);
                int comment = value.IndexOf(';');
                if (comment >= 0) value = value.Substring(0, comment);
                v[section + "." + line.Substring(0, eq).Trim()] = value.Trim();
            }
        }
        catch (Exception e)
        {
            Logger.LogWarning("VaM hair budget: cannot read " + configPath + ": " + e.Message);
            return false;
        }

        Settings s = new Settings();
        s.profile = Get(v, "HairBudget.Profile", s.profile);
        s.disableShadows = GetInt(v, "HairBudget.DisableShadows", 0) != 0;
        s.toggleKey = Get(v, "HairBudget.ToggleKey", "");
        s.menuTab = Get(v, "HairBudget.MenuTab", s.menuTab);
        s.personK = GetInt(v, "Custom.PersonBudgetK", s.personK);
        s.sceneK = GetInt(v, "Custom.SceneBudgetK", s.sceneK);
        s.densityMax = GetInt(v, "Custom.DensityMax", s.densityMax);
        s.curveDensityMin = GetInt(v, "Custom.CurveDensityMin", s.curveDensityMin);
        s.hairMultiplierMin = GetInt(v, "Custom.HairMultiplierMin", s.hairMultiplierMin);
        s.iterationsMax = GetInt(v, "Custom.IterationsMax", s.iterationsMax);
        s.widthCompensation = GetFloat(v, "Custom.WidthCompensation", s.widthCompensation);
        string rate = Get(v, "HairPhysics.Rate", s.hairRate);
        int rateIndex = HairRateIds.FindIndex(delegate(string id) { return id.Equals(rate, StringComparison.OrdinalIgnoreCase); });
        if (rateIndex < 0) Logger.LogWarning("VaM hair budget: unknown [HairPhysics] Rate=" + rate + "; hair follows VaM's physics rate");
        s.hairRate = rateIndex >= 0 ? HairRateIds[rateIndex] : "VaM";
        s.hairCap = Mathf.Clamp(GetInt(v, "HairPhysics.UpdateCap", s.hairCap), 1, 3);
        settings = s;
        ParseKey(s.toggleKey);
        ApplySettings(false);
        return true;
    }

    // Recomputes the limits from settings; save also writes them to the ini,
    // debounced so that dragging a menu slider does not rewrite it every frame.
    private void ApplySettings(bool save)
    {
        Limits next = Derive(settings);
        if (limits != null && (next.name != limits.name || next.name == "Custom" || settings.disableShadows != disableShadows))
            Logger.LogInfo("VaM hair budget: profile " + next.name + (settings.disableShadows ? ", hair shadows off" : ""));
        limits = next;
        disableShadows = settings.disableShadows;
        dirty = true;
        if (save) saveAt = Time.realtimeSinceStartup + 0.5f;
        SyncMenu();
    }

    // Rewrites only the keys the menu owns; comments and other lines stay.
    private void SaveConfig()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        Dictionary<string, string> values = new Dictionary<string, string>();
        values["HairBudget.Profile"] = settings.profile;
        values["HairBudget.DisableShadows"] = settings.disableShadows ? "1" : "0";
        values["Custom.PersonBudgetK"] = settings.personK.ToString(inv);
        values["Custom.SceneBudgetK"] = settings.sceneK.ToString(inv);
        values["Custom.DensityMax"] = settings.densityMax.ToString(inv);
        values["Custom.CurveDensityMin"] = settings.curveDensityMin.ToString(inv);
        values["Custom.HairMultiplierMin"] = settings.hairMultiplierMin.ToString(inv);
        values["Custom.IterationsMax"] = settings.iterationsMax.ToString(inv);
        values["Custom.WidthCompensation"] = settings.widthCompensation.ToString("0.##", inv);
        values["HairPhysics.Rate"] = settings.hairRate;
        values["HairPhysics.UpdateCap"] = settings.hairCap.ToString(inv);
        try
        {
            List<string> lines = new List<string>(File.Exists(configPath) ? File.ReadAllLines(configPath) : new string[0]);
            HashSet<string> done = new HashSet<string>();
            string section = "";
            for (int i = 0; i < lines.Count; i++)
            {
                string line = lines[i].Trim();
                if (line.StartsWith("[")) { section = line.Trim('[', ']').Trim(); continue; }
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string key = section + "." + line.Substring(0, eq).Trim();
                string value;
                if (!values.TryGetValue(key, out value)) continue;
                lines[i] = line.Substring(0, eq).Trim() + "=" + value;
                done.Add(key);
            }
            foreach (string name in new string[] { "HairBudget", "Custom", "HairPhysics" })
            {
                // Missing keys go right under their section header (or a new section).
                List<string> missing = new List<string>();
                foreach (KeyValuePair<string, string> kv in values)
                    if (kv.Key.StartsWith(name + ".") && !done.Contains(kv.Key)) missing.Add(kv.Key.Substring(name.Length + 1) + "=" + kv.Value);
                if (missing.Count == 0) continue;
                int header = lines.FindIndex(delegate(string l) { return l.Trim().Equals("[" + name + "]", StringComparison.OrdinalIgnoreCase); });
                if (header >= 0) lines.InsertRange(header + 1, missing);
                else { lines.Add(""); lines.Add("[" + name + "]"); lines.AddRange(missing); }
            }
            File.WriteAllText(configPath, string.Join(Environment.NewLine, lines.ToArray()) + Environment.NewLine, new UTF8Encoding(true));
            configStamp = File.GetLastWriteTimeUtc(configPath);
        }
        catch (Exception e)
        {
            Logger.LogWarning("VaM hair budget: cannot save " + configPath + ": " + e.Message);
        }
    }

    private void ParseKey(string text)
    {
        toggleKey = KeyCode.None;
        toggleCtrl = toggleShift = toggleAlt = false;
        if (string.IsNullOrEmpty(text)) return;
        foreach (string part in text.Split('+'))
        {
            string p = part.Trim();
            if (p.Equals("ctrl", StringComparison.OrdinalIgnoreCase)) toggleCtrl = true;
            else if (p.Equals("shift", StringComparison.OrdinalIgnoreCase)) toggleShift = true;
            else if (p.Equals("alt", StringComparison.OrdinalIgnoreCase)) toggleAlt = true;
            else
            {
                try { toggleKey = (KeyCode)Enum.Parse(typeof(KeyCode), p, true); }
                catch (Exception) { toggleKey = KeyCode.None; Logger.LogWarning("VaM hair budget: unknown ToggleKey " + text); return; }
            }
        }
    }

    private static string Get(Dictionary<string, string> v, string key, string fallback)
    {
        string s;
        return v.TryGetValue(key, out s) && s.Length > 0 ? s : fallback;
    }

    private static int GetInt(Dictionary<string, string> v, string key, int fallback)
    {
        int n;
        return int.TryParse(Get(v, key, ""), out n) ? Math.Max(0, n) : fallback;
    }

    private static float GetFloat(Dictionary<string, string> v, string key, float fallback)
    {
        float f;
        return float.TryParse(Get(v, key, ""), System.Globalization.NumberStyles.Float,
            System.Globalization.CultureInfo.InvariantCulture, out f) ? Mathf.Max(0f, f) : fallback;
    }
}
