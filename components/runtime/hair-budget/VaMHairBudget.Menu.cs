// In-game menu of the VaM hair budget plugin: a "Hair Budget" tab in VaM's main
// menu (SuperController.mainMenuTabSelector; MenuTab=UserPrefs in the ini puts
// it under User Preferences instead, MenuTab=None leaves the menu alone).
//
// UITabSelector shows the child of its own transform that carries a UITab and
// has the name of the toggle that is on, so the tab is a clone of the last tab
// toggle (fresh listeners, same ToggleGroup) plus an empty UITab panel. The
// controls are VaM's plugin UI prefabs (MVRPluginManager.configurable*Prefab)
// bound to JSONStorable params exactly as MVRScript.CreateSlider etc. do.
// Any failure leaves the menu untouched; the ini keeps working.
using System;
using System.Collections.Generic;
using System.Text;
using UnityEngine;
using UnityEngine.UI;

public sealed partial class VaMHairBudget
{
    private const string MenuTabName = "Hair Budget";      // GameObject name UITabSelector matches on
    private static readonly string MenuTabLabel = Lang.T("Hair Budget");
    private const string NoTranslate = "᠎";            // XUnity AutoTranslator IgnoreTextStartingWith
    private const int MenuAttemptsMax = 5;

    private static readonly List<string> ProfileIds = new List<string>(new string[] { "Off", "High", "Balanced", "Performance", "Custom" });
    private static readonly List<string> ProfileLabels = new List<string>(new string[] {
        Lang.T("Off (hair left as it is)"), Lang.T("High quality (only the extremes)"), Lang.T("Balanced (recommended)"), Lang.T("Performance"),
        Lang.T("Custom (the values on the right)") });

    private sealed class PersonStat
    {
        public string atom;
        public int items, lowered;
        public double requested, drawn;
    }

    private readonly List<PersonStat> personStats = new List<PersonStat>();
    private GameObject menuToggle, menuContent;
    private bool menuBuilt, menuGaveUp, syncing;
    private int menuAttempts;
    private float nextMenuTry, nextStatus;
    private JSONStorableStringChooser uiProfile, uiHairRate, uiHairCap;
    private JSONStorableBool uiShadows, uiPaused;
    private JSONStorableString uiStatus;
    private JSONStorableFloat uiPerson, uiScene, uiDensity, uiIterations, uiWidth, uiCdMin, uiHmMin;

    private void UpdateMenu(float now)
    {
        if (!menuBuilt)
        {
            if (!menuGaveUp && now >= nextMenuTry)
            {
                nextMenuTry = now + 2f;
                TryBuildMenu();
            }
            return;
        }
        if (menuContent == null || menuToggle == null)
        {
            // The menu was rebuilt by VaM: add the tab again.
            menuBuilt = false;
            return;
        }
        if (menuContent.activeInHierarchy && now >= nextStatus)
        {
            nextStatus = now + 1f;
            uiStatus.val = NoTranslate + StatusText();
        }
    }

    // ---- building ----------------------------------------------------------

    private void TryBuildMenu()
    {
        if (settings.menuTab.Equals("None", StringComparison.OrdinalIgnoreCase)) { menuGaveUp = true; return; }
        SuperController sc = SuperController.singleton;
        if (sc == null) return;
        bool userPrefs = settings.menuTab.Equals("UserPrefs", StringComparison.OrdinalIgnoreCase);
        UITabSelector selector = userPrefs ? sc.userPrefsTabSelector : sc.mainMenuTabSelector;
        MVRPluginManager prefabs = FindObjectOfType<MVRPluginManager>();
        if (selector == null || prefabs == null || prefabs.configurableSliderPrefab == null || prefabs.configurableTogglePrefab == null ||
            prefabs.configurablePopupPrefab == null || prefabs.configurableTextFieldPrefab == null) return;

        try
        {
            Transform toggles = selector.toggleContainer != null ? selector.toggleContainer : selector.transform;
            Dictionary<string, RectTransform> tabs = new Dictionary<string, RectTransform>();
            foreach (Transform child in selector.transform)
                if (child.GetComponent<UITab>() != null && !tabs.ContainsKey(child.name)) tabs[child.name] = child as RectTransform;
            List<Toggle> tabToggles = new List<Toggle>();
            foreach (Transform child in toggles)
            {
                Toggle t = child.GetComponent<Toggle>();
                if (t != null && child.gameObject.activeSelf && tabs.ContainsKey(child.name) && child.name != MenuTabName) tabToggles.Add(t);
            }
            if (tabs.ContainsKey(MenuTabName))
            {
                Logger.LogWarning("VaM hair budget: menu already has a '" + MenuTabName + "' tab; not adding another");
                menuGaveUp = true;
                return;
            }
            LogMenuLayout(selector, toggles, tabs, tabToggles);
            if (tabToggles.Count == 0) throw new Exception("no tab toggles found under " + ObjectPath(toggles));

            Toggle template = tabToggles[tabToggles.Count - 1];
            menuToggle = MakeTabToggle(selector, toggles, template, tabToggles);
            RectTransform reference;
            if (!tabs.TryGetValue(template.name, out reference) || reference == null)
                foreach (RectTransform r in tabs.Values) { reference = r; break; }
            menuContent = MakeTabPanel(selector, reference);
            // Binding a param to its control may call back with the starting
            // value; that must not count as the user switching to Custom.
            syncing = true;
            try { BuildControls(menuContent.GetComponent<RectTransform>(), prefabs); }
            finally { syncing = false; }
            menuBuilt = true;
            SyncMenu();
            Logger.LogInfo("VaM hair budget: added the '" + MenuTabLabel + "' tab to " +
                (userPrefs ? "User Preferences" : "the main menu") + " (" + ObjectPath(selector.transform) + ")");
        }
        catch (Exception e)
        {
            if (menuToggle != null) Destroy(menuToggle);
            if (menuContent != null) Destroy(menuContent);
            menuToggle = menuContent = null;
            menuAttempts++;
            Logger.LogWarning("VaM hair budget: could not add the menu tab (attempt " + menuAttempts + "): " + e);
            if (menuAttempts >= MenuAttemptsMax)
            {
                menuGaveUp = true;
                Logger.LogWarning("VaM hair budget: menu tab given up; BepInEx\\config\\VaMHairBudget.ini still works");
            }
        }
    }

    private GameObject MakeTabToggle(UITabSelector selector, Transform toggles, Toggle template, List<Toggle> tabToggles)
    {
        // Clone under an inactive holder: an enabled clone of the selected tab
        // would join the ToggleGroup switched on and deselect VaM's own tab.
        GameObject holder = new GameObject("VaMHairBudgetHolder");
        holder.SetActive(false);
        GameObject go = (GameObject)Instantiate(template.gameObject, holder.transform, false);
        go.name = MenuTabName;
        Toggle toggle = go.GetComponent<Toggle>();
        // The clone's serialized listeners belong to another tab: start clean.
        toggle.onValueChanged = new Toggle.ToggleEvent();
        toggle.isOn = false;
        toggle.group = template.group;
        Text label = go.GetComponentInChildren<Text>(true);
        if (label != null) label.text = MenuTabLabel;
        go.transform.SetParent(toggles, false);
        go.transform.SetAsLastSibling();
        Destroy(holder);
        toggle.onValueChanged.AddListener(delegate(bool on) { if (on) selector.ActiveTabChanged(); });

        RectTransform rt = go.GetComponent<RectTransform>();
        if (toggles.GetComponent<LayoutGroup>() == null && rt != null && tabToggles.Count > 0)
        {
            // Hand-placed toggles: continue the list with its usual step.
            List<Vector2> steps = new List<Vector2>();
            for (int i = 1; i < tabToggles.Count; i++)
            {
                RectTransform a = tabToggles[i - 1].GetComponent<RectTransform>(), b = tabToggles[i].GetComponent<RectTransform>();
                if (a != null && b != null) steps.Add(b.anchoredPosition - a.anchoredPosition);
            }
            RectTransform last = tabToggles[tabToggles.Count - 1].GetComponent<RectTransform>();
            Vector2 step = new Vector2(0f, -(last.rect.height + 4f));
            if (steps.Count > 0)
            {
                steps.Sort(delegate(Vector2 x, Vector2 y) { return x.y.CompareTo(y.y); });
                step = steps[steps.Count / 2];
                if (step.sqrMagnitude < 1f) step = new Vector2(0f, -(last.rect.height + 4f));
            }
            rt.anchoredPosition = last.anchoredPosition + step;
            Logger.LogInfo("VaM hair budget: menu toggle placed by hand at " + rt.anchoredPosition + " (step " + step + ")");
        }
        return go;
    }

    private static GameObject MakeTabPanel(UITabSelector selector, RectTransform reference)
    {
        GameObject go = new GameObject(MenuTabName, typeof(RectTransform));
        go.SetActive(false);                               // UITabSelector shows it
        go.AddComponent<UITab>();
        RectTransform rt = go.GetComponent<RectTransform>();
        rt.SetParent(selector.transform, false);
        if (reference != null)
        {
            rt.anchorMin = reference.anchorMin; rt.anchorMax = reference.anchorMax; rt.pivot = reference.pivot;
            rt.anchoredPosition = reference.anchoredPosition; rt.sizeDelta = reference.sizeDelta;
            rt.localScale = reference.localScale; rt.localRotation = reference.localRotation;
            rt.SetSiblingIndex(reference.GetSiblingIndex() + 1);
            Image background = reference.GetComponent<Image>();
            if (background != null)
            {
                Image image = go.AddComponent<Image>();
                image.sprite = background.sprite; image.type = background.type; image.color = background.color;
            }
        }
        return go;
    }

    private void BuildControls(RectTransform panel, MVRPluginManager prefabs)
    {
        RectTransform left = MakeColumn(panel, "Left", 0f, 0.5f);
        RectTransform right = MakeColumn(panel, "Right", 0.5f, 1f);

        AddText(left, prefabs, 330f,
            Lang.T("VaM Hair Budget\n\nPuts a ceiling on what simulated hair costs to draw (per person and for the whole scene). Above it, only the most expensive hair comes down to a common level; cheaper hair and small pieces stay as they are.\n\nOnly the values the renderer and the physics read change: the hair sliders, the scene and the preset files keep their own values, and saves are not affected.\n\nCost = patches × hm × (cd + 25) × 24/49 segments (25 more per strand)\nSettings are saved to BepInEx\\config\\VaMHairBudget.ini as you change them"));
        uiProfile = new JSONStorableStringChooser("profile", ProfileIds, ProfileLabels, settings.profile, Lang.T("Profile"),
            new JSONStorableStringChooser.SetStringCallback(OnProfile));
        UIDynamicPopup popup = Make<UIDynamicPopup>(prefabs.configurablePopupPrefab, left);
        popup.label = Lang.T("Profile");
        uiProfile.popup = popup.popup;
        uiShadows = new JSONStorableBool("shadows", settings.disableShadows, new JSONStorableBool.SetBoolCallback(OnShadows));
        AddToggle(left, prefabs, uiShadows, Lang.T("Hair casts no shadows (one pass less per light)"));
        uiPaused = new JSONStorableBool("paused", paused, new JSONStorableBool.SetBoolCallback(OnPaused));
        AddToggle(left, prefabs, uiPaused, Lang.T("Off for now (to compare; not saved)"));
        AddText(left, prefabs, 110f, Lang.T("Hair physics rate: by default it follows the body's physics (the same steps and step length); a rate chosen here is a copy of VaM's physics rate setting that applies to simulated hair only."));
        uiHairRate = new JSONStorableStringChooser("hairRate", HairRateIds, HairRateLabels, settings.hairRate, Lang.T("Hair physics rate"),
            new JSONStorableStringChooser.SetStringCallback(OnHairRate));
        UIDynamicPopup ratePopup = Make<UIDynamicPopup>(prefabs.configurablePopupPrefab, left);
        ratePopup.label = Lang.T("Hair physics rate");
        uiHairRate.popup = ratePopup.popup;
        uiHairCap = new JSONStorableStringChooser("hairCap", HairCapIds, HairCapIds, settings.hairCap.ToString(), Lang.T("Hair update cap"),
            new JSONStorableStringChooser.SetStringCallback(OnHairCap));
        UIDynamicPopup capPopup = Make<UIDynamicPopup>(prefabs.configurablePopupPrefab, left);
        capPopup.label = Lang.T("Hair update cap");
        uiHairCap.popup = capPopup.popup;
        uiStatus = new JSONStorableString("status", "");
        UIDynamicTextField status = Make<UIDynamicTextField>(prefabs.configurableTextFieldPrefab, left);
        status.height = 470f;
        uiStatus.dynamicText = status;

        AddText(right, prefabs, 110f, Lang.T("Moving any of these switches the profile to Custom; a limit of 0 means no limit."));
        uiPerson = AddSlider(right, prefabs, Lang.T("Cost limit per person (10k segments)"), 0f, 300f, "F0", 0);
        uiScene = AddSlider(right, prefabs, Lang.T("Cost limit for the scene (10k segments)"), 0f, 800f, "F0", 1);
        uiDensity = AddSlider(right, prefabs, Lang.T("Density limit per piece, hm×cd"), 0f, 4096f, "F0", 2);
        uiIterations = AddSlider(right, prefabs, Lang.T("Physics iteration limit"), 0f, 5f, "F0", 3);
        uiWidth = AddSlider(right, prefabs, Lang.T("Strand width compensation (×)"), 1f, 2f, "F2", 4);
        uiCdMin = AddSlider(right, prefabs, Lang.T("curveDensity floor"), 2f, 32f, "F0", 6);
        uiHmMin = AddSlider(right, prefabs, Lang.T("hairMultiplier floor"), 1f, 16f, "F0", 7);
    }

    private static RectTransform MakeColumn(RectTransform parent, string name, float x0, float x1)
    {
        GameObject view = new GameObject(name, typeof(RectTransform));
        RectTransform vr = view.GetComponent<RectTransform>();
        vr.SetParent(parent, false);
        vr.anchorMin = new Vector2(x0, 0f); vr.anchorMax = new Vector2(x1, 1f);
        vr.offsetMin = new Vector2(10f, 10f); vr.offsetMax = new Vector2(-10f, -10f);
        Image hit = view.AddComponent<Image>();
        hit.color = new Color(1f, 1f, 1f, 0f);            // lets the empty area take scroll drags
        view.AddComponent<RectMask2D>();
        ScrollRect scroll = view.AddComponent<ScrollRect>();
        scroll.horizontal = false;
        scroll.movementType = ScrollRect.MovementType.Clamped;
        scroll.scrollSensitivity = 30f;

        GameObject content = new GameObject("Content", typeof(RectTransform));
        RectTransform cr = content.GetComponent<RectTransform>();
        cr.SetParent(vr, false);
        cr.anchorMin = new Vector2(0f, 1f); cr.anchorMax = new Vector2(1f, 1f); cr.pivot = new Vector2(0.5f, 1f);
        cr.offsetMin = Vector2.zero; cr.offsetMax = Vector2.zero;
        VerticalLayoutGroup layout = content.AddComponent<VerticalLayoutGroup>();
        layout.spacing = 8f;
        layout.childAlignment = TextAnchor.UpperLeft;
        layout.childControlWidth = true; layout.childControlHeight = true;
        layout.childForceExpandWidth = true; layout.childForceExpandHeight = false;
        content.AddComponent<ContentSizeFitter>().verticalFit = ContentSizeFitter.FitMode.PreferredSize;
        scroll.viewport = vr;
        scroll.content = cr;
        return cr;
    }

    private static T Make<T>(Transform prefab, RectTransform parent) where T : Component
    {
        Transform t = (Transform)Instantiate(prefab);
        t.SetParent(parent, false);
        t.gameObject.SetActive(true);
        T c = t.GetComponent<T>();
        if (c == null) throw new Exception(prefab.name + " has no " + typeof(T).Name);
        return c;
    }

    private static void AddText(RectTransform parent, MVRPluginManager prefabs, float height, string text)
    {
        JSONStorableString s = new JSONStorableString("text", NoTranslate + text);
        UIDynamicTextField field = Make<UIDynamicTextField>(prefabs.configurableTextFieldPrefab, parent);
        field.height = height;
        s.dynamicText = field;
    }

    private static void AddToggle(RectTransform parent, MVRPluginManager prefabs, JSONStorableBool b, string label)
    {
        UIDynamicToggle toggle = Make<UIDynamicToggle>(prefabs.configurableTogglePrefab, parent);
        toggle.label = label;
        b.toggle = toggle.toggle;
    }

    private JSONStorableFloat AddSlider(RectTransform parent, MVRPluginManager prefabs, string label, float min, float max, string format, int which)
    {
        JSONStorableFloat f = new JSONStorableFloat(label, min, new JSONStorableFloat.SetFloatCallback(delegate(float v) { OnCustom(which, v); }),
            min, max, true, true);
        UIDynamicSlider slider = Make<UIDynamicSlider>(prefabs.configurableSliderPrefab, parent);
        slider.Configure(label, min, max, min, true, format, true, false);
        f.slider = slider.slider;
        return f;
    }

    // ---- state -------------------------------------------------------------

    // Values the sliders show: the profile's own, or the Custom values for
    // Custom and Off (moving one of them starts from what is shown).
    private Limits ShownLimits()
    {
        if (settings.profile == "Custom" || settings.profile == "Off")
        {
            Settings s = settings;
            return new Limits("Custom", true, s.personK, s.sceneK, s.densityMax, s.curveDensityMin, s.hairMultiplierMin,
                s.iterationsMax, s.widthCompensation);
        }
        return limits;
    }

    private void SyncMenu()
    {
        if (!menuBuilt) return;
        syncing = true;
        try
        {
            Limits l = ShownLimits();
            uiProfile.val = settings.profile;
            uiShadows.val = settings.disableShadows;
            uiPaused.val = paused;
            uiHairRate.val = settings.hairRate;
            uiHairCap.val = settings.hairCap.ToString();
            uiPerson.val = l.personBudget / 10000f;
            uiScene.val = l.sceneBudget / 10000f;
            uiDensity.val = l.densityMax;
            uiIterations.val = l.iterationsMax;
            uiWidth.val = l.widthCompensation;
            uiCdMin.val = l.curveDensityMin;
            uiHmMin.val = l.hairMultiplierMin;
        }
        finally { syncing = false; }
        nextStatus = 0f;
    }

    private void OnProfile(string id)
    {
        if (syncing) return;
        settings.profile = id;
        ApplySettings(true);
    }

    private void OnShadows(bool b)
    {
        if (syncing) return;
        settings.disableShadows = b;
        ApplySettings(true);
    }

    private void OnPaused(bool b)
    {
        if (syncing) return;
        paused = b;
        dirty = true;
        Logger.LogInfo(paused ? "VaM hair budget: paused from the menu" : "VaM hair budget: resumed from the menu, profile " + limits.name);
        nextStatus = 0f;
    }

    private void OnHairRate(string id)
    {
        if (syncing || !HairRateIds.Contains(id)) return;
        settings.hairRate = id;
        ApplySettings(true);
    }

    private void OnHairCap(string id)
    {
        int cap;
        if (syncing || !int.TryParse(id, out cap)) return;
        settings.hairCap = Mathf.Clamp(cap, 1, 3);
        ApplySettings(true);
    }

    private void OnCustom(int which, float v)
    {
        if (syncing) return;
        Settings s = settings;
        if (s.profile != "Custom")
        {
            // Start from the values on screen.
            Limits l = ShownLimits();
            s.personK = (int)(l.personBudget / 1000); s.sceneK = (int)(l.sceneBudget / 1000);
            s.densityMax = l.densityMax; s.iterationsMax = l.iterationsMax;
            s.widthCompensation = l.widthCompensation;
            s.curveDensityMin = l.curveDensityMin; s.hairMultiplierMin = l.hairMultiplierMin;
            s.profile = "Custom";
        }
        switch (which)
        {
            case 0: s.personK = Mathf.RoundToInt(v) * 10; break;
            case 1: s.sceneK = Mathf.RoundToInt(v) * 10; break;
            case 2: s.densityMax = Mathf.RoundToInt(v); break;
            case 3: s.iterationsMax = Mathf.RoundToInt(v); break;
            case 4: s.widthCompensation = Mathf.Round(v * 100f) / 100f; break;
            case 6: s.curveDensityMin = Mathf.RoundToInt(v); break;
            case 7: s.hairMultiplierMin = Mathf.RoundToInt(v); break;
        }
        ApplySettings(true);
    }

    // Per person totals for the status text, after Evaluate wrote the values.
    private void CollectStats()
    {
        personStats.Clear();
        Dictionary<string, PersonStat> byAtom = new Dictionary<string, PersonStat>();
        foreach (Hair h in hairs.Values)
        {
            if (h.control == null || h.settings == null || !h.active || h.cost.patches <= 0) continue;
            PersonStat p;
            if (!byAtom.TryGetValue(h.atom, out p))
            {
                p = new PersonStat();
                p.atom = h.atom;
                byAtom[h.atom] = p;
                personStats.Add(p);
            }
            HairCost c = h.cost;
            p.items++;
            p.requested += (double)c.patches * HairBudgetMath.Cost(c.requestedDensity, c.requestedDetail);
            p.drawn += (double)c.patches * HairBudgetMath.Cost(h.densityWritten, h.detailWritten);
            if (h.densityWritten != c.requestedDensity || h.detailWritten != c.requestedDetail || h.iterationsWritten != h.requestedIterations)
                p.lowered++;
        }
        personStats.Sort(delegate(PersonStat a, PersonStat b) { return b.requested.CompareTo(a.requested); });
    }

    private string StatusText()
    {
        StringBuilder b = new StringBuilder();
        int index = ProfileIds.IndexOf(settings.profile);
        b.Append(Lang.T("Profile: ")).Append(index >= 0 ? ProfileLabels[index] : settings.profile);
        if (paused) b.Append(Lang.T("  (off for now)"));
        b.Append('\n').Append(HairRateStatus()).Append('\n');
        if (personStats.Count == 0)
        {
            b.Append('\n').Append(Lang.T("No simulated hair is shown in the scene."));
            return b.ToString();
        }
        double requested = 0, drawn = 0;
        foreach (PersonStat p in personStats) { requested += p.requested; drawn += p.drawn; }
        b.Append(Lang.F("Scene cost: {0} → {1} ×10k segments", Wan(requested), Wan(drawn)));
        if (!paused && limits.enabled && limits.sceneBudget > 0) b.Append(Lang.F(" (limit {0})", Wan(limits.sceneBudget)));
        b.Append("\n\n");
        int shown = 0;
        foreach (PersonStat p in personStats)
        {
            if (++shown > 8) { b.Append(Lang.F("… and {0} more people", personStats.Count - 8)).Append('\n'); break; }
            b.Append(Lang.F("{0}: {1} pieces, {2} → {3} ×10k", p.atom, p.items, Wan(p.requested), Wan(p.drawn)));
            if (p.lowered > 0) b.Append(Lang.F(" ({0} lowered)", p.lowered));
            b.Append('\n');
        }
        b.Append('\n').Append(Lang.T("What changed for each piece is in BepInEx\\LogOutput.log (search for VaM hair budget)."));
        return b.ToString();
    }

    private static string Wan(double segments)
    {
        return (segments / 10000.0).ToString("0.0");
    }

    // ---- diagnostics -----------------------------------------------------

    private void LogMenuLayout(UITabSelector selector, Transform toggles, Dictionary<string, RectTransform> tabs, List<Toggle> tabToggles)
    {
        StringBuilder b = new StringBuilder("VaM hair budget: menu layout ").Append(ObjectPath(selector.transform))
            .Append(", toggles under ").Append(ObjectPath(toggles));
        LayoutGroup group = toggles.GetComponent<LayoutGroup>();
        b.Append(group != null ? " (" + group.GetType().Name + ")" : " (no layout group)");
        foreach (Toggle t in tabToggles)
        {
            RectTransform r = t.GetComponent<RectTransform>();
            b.Append("\n  toggle '").Append(t.name).Append("'");
            if (r != null) b.Append(" pos ").Append(r.anchoredPosition).Append(" size ").Append(r.sizeDelta);
        }
        foreach (KeyValuePair<string, RectTransform> kv in tabs)
        {
            RectTransform r = kv.Value;
            b.Append("\n  tab '").Append(kv.Key).Append("'");
            if (r != null) b.Append(" anchors ").Append(r.anchorMin).Append("-").Append(r.anchorMax).Append(" size ").Append(r.rect.size);
        }
        Logger.LogInfo(b.ToString());
    }

    private static string ObjectPath(Transform t)
    {
        string p = t.name;
        for (Transform up = t.parent; up != null; up = up.parent) p = up.name + "/" + p;
        return p;
    }
}
