// In-game menu of the VaM VR settings plugin: a "VR Graphics" tab in VaM's main
// menu, added the way the hair-budget component's VaMHairBudget.Menu.cs adds its
// "Hair Budget" tab. UITabSelector shows the child of its own transform that
// carries a UITab and has the name of the toggle that is on, so the tab is a
// clone of the last tab toggle (fresh listeners, same ToggleGroup) plus an
// empty UITab panel.
// Layout (v30), made for a laser pointer: nothing scrolls and every target is
// big. Across the top this run's status; on the left a side bar with one entry
// per page and Back to launch settings; on the right the tabs of the page's sub-pages
// (when it has more than one) and its settings, one row each at a fixed
// height: the label, then Off / On or the choices as one segmented row of
// buttons, the chosen one lit, or a number as − bar + and its value (− and + a
// fixed step, the bar for big moves). Rows under a switch that is off are left
// out and the rest move up; a section longer than the room is cut into
// sub-pages, so a new setting never makes anything scroll. (v8-v29: two
// scrolling columns of folding groups with hints, the left one 2.4 views
// tall, VaM's slider prefab with its -1000..+1000 buttons, and the choice
// labels in the text field prefab's black on the tab's dark panel.)
// v31: each level its own look, so a first look tells them apart - pages a
// dark side bar (the page shown filled, with a bright edge), sub-pages text
// tabs (the one shown underlined), and the settings in cards: a switch and
// what belongs to it (its children, the rows hidden with it, VaMVRMenu.cs
// `group`) share one card, the switch's label bold, the rest indented on a
// guide line; the choices are segmented rows inside the cards. v30 had VaM's
// button prefab for all three and told the levels apart by indent only.
// Every control is built here (Unity Button / Slider / Text) on the button
// prefab's rounded sprite. Any failure leaves VaM's menu untouched; the
// launch panel still works.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using UnityEngine;
using UnityEngine.UI;

public sealed partial class VaMVRMenu
{
    private const string MenuTabName = "VR Settings";      // GameObject name UITabSelector matches on
    private static readonly string MenuTabLabel = Desktop ? Lang.T("Graphics") : Lang.T("VR Graphics");
    private const string NoTranslate = "᠎";           // XUnity AutoTranslator IgnoreTextStartingWith
    private const int MenuAttemptsMax = 5;
    // In the tab panel's units (1100 × 1380 in VaM 1.22; VaM's own tab buttons are 80 tall).
    private const float Pad = 16f, Gap = 14f, NavWidth = 220f, NavHeight = 96f, StatusLines = 4f;
    private const float BarHeight = 64f, BarTabWidth = 180f, RowHeight = 88f, LiveHeight = 70f, RowGap = 6f;
    private const float CardGap = 14f, CardPadX = 14f, CardPadY = 10f;
    private const float LabelWidth = 230f, ChildIndent = 30f, StepWidth = 92f, ValueWidth = 100f, HandleWidth = 34f;

    private static readonly Color Accent = new Color(0.22f, 0.52f, 0.95f);        // a chosen value, a bar's fill
    private static readonly Color Bright = new Color(0.55f, 0.80f, 1f);           // the edge of the page shown, the tab underline
    private static readonly Color PageShown = new Color(0.17f, 0.37f, 0.72f);
    private static readonly Color SideBar = new Color(0f, 0f, 0f, 0.28f);
    private static readonly Color CardColor = new Color(1f, 1f, 1f, 0.07f);
    private static readonly Color Guide = new Color(0.55f, 0.80f, 1f, 0.45f);
    private static readonly Color OptionOff = new Color(1f, 1f, 1f, 0.13f);
    private static readonly Color PlainFill = new Color(1f, 1f, 1f, 0.17f);
    private static readonly Color LabelColor = new Color(0.93f, 0.95f, 0.98f);
    private static readonly Color DimColor = new Color(0.72f, 0.80f, 0.90f);     // status, live rows, pages not shown
    private static readonly Color TrackColor = new Color(1f, 1f, 1f, 0.18f);

    // A button of the tab, by level: a page, a sub-page, a value, or anything
    // else (− / +, Back to launch settings).
    private enum Look { Page, Tab, Option, Plain }

    private sealed class Btn
    {
        public Look look;
        public Button button;
        public Image fill, mark;                           // mark: a page's edge, a tab's underline
        public Text text;
    }

    // A card: one switch and what belongs to it, in one section.
    private sealed class Card
    {
        public GameObject go;
        public string key;                                 // the switch at the top of its chain
        public readonly List<Item> rows = new List<Item>();
    }

    private GameObject menuToggle, menuContent;
    private RectTransform rowsArea, barArea;
    private Text statusText;
    private readonly List<Btn> barButtons = new List<Btn>();
    private readonly List<string> pagesLogged = new List<string>();
    private Page shownPage;                                // kept while VaM runs
    private int shownPart;
    private float rowsRoom = 1100f, rowsTop;
    private bool menuBuilt, menuGaveUp, syncing;
    private int menuAttempts;
    private float nextMenuTry, nextStatus;
    private Font font;
    private int fontSize = 28;
    private Sprite buttonSprite;
    private Image.Type buttonSpriteType = Image.Type.Simple;

    private void UpdateMenu(float now)
    {
        if (!performance.loaded) return;                    // the settings come first
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
            // VaM rebuilt its menu: add the tab again.
            if (menuToggle != null) Destroy(menuToggle);
            if (menuContent != null) Destroy(menuContent);
            menuToggle = menuContent = null;
            menuBuilt = false;
            return;
        }
        if (menuContent.activeInHierarchy && now >= nextStatus)
        {
            nextStatus = now + 1f;
            if (layerReports) QueryLayer();                 // the multiplier the session runs now
            TakeEntryValues();
            foreach (Page p in pages)
                foreach (Section s in p.sections)
                    foreach (Item i in s.members)
                        if (i.live != null && i.labelText != null && i.control.activeInHierarchy) i.labelText.text = NoTranslate + LiveText(i);
            if (statusText != null) statusText.text = NoTranslate + StatusText();
        }
        else if (!menuContent.activeInHierarchy)
        {
            nextStatus = 0f;                                // fresh on the next open
        }
    }

    // ---- building ----------------------------------------------------------

    private void TryBuildMenu()
    {
        SuperController sc = SuperController.singleton;
        if (sc == null) return;
        UITabSelector selector = sc.mainMenuTabSelector;
        MVRPluginManager prefabs = FindObjectOfType<MVRPluginManager>();
        if (selector == null || prefabs == null || prefabs.configurableTextFieldPrefab == null ||
            prefabs.configurableButtonPrefab == null) return;

        try
        {
            Transform toggles = selector.toggleContainer != null ? selector.toggleContainer : selector.transform;
            Dictionary<string, RectTransform> tabs = new Dictionary<string, RectTransform>();
            foreach (Transform child in selector.transform)
                if (child.GetComponent<UITab>() != null && !tabs.ContainsKey(child.name)) tabs[child.name] = child as RectTransform;
            if (tabs.ContainsKey(MenuTabName))
            {
                Logger.LogWarning("VaM VR menu: the main menu already has a '" + MenuTabName + "' tab; not adding another");
                menuGaveUp = true;
                return;
            }
            List<Toggle> tabToggles = new List<Toggle>();
            foreach (Transform child in toggles)
            {
                Toggle t = child.GetComponent<Toggle>();
                if (t != null && child.gameObject.activeSelf && tabs.ContainsKey(child.name)) tabToggles.Add(t);
            }
            LogMenuLayout(selector, toggles, tabToggles);
            if (tabToggles.Count == 0) throw new Exception("no tab toggles found under " + ObjectPath(toggles));

            Toggle template = tabToggles[tabToggles.Count - 1];
            menuToggle = MakeTabToggle(selector, toggles, template, tabToggles);
            RectTransform reference;
            if (!tabs.TryGetValue(template.name, out reference) || reference == null)
                foreach (RectTransform r in tabs.Values) { reference = r; break; }
            menuContent = MakeTabPanel(selector, reference);
            syncing = true;
            try { BuildControls(menuContent.GetComponent<RectTransform>(), prefabs); }
            finally { syncing = false; }
            menuBuilt = true;
            pagesLogged.Clear();
            SyncMenu();
            StringBuilder b = new StringBuilder();
            foreach (Page p in pages)
            {
                int n = 0, cards = 0;
                foreach (Section s in p.sections) { n += s.members.Count; cards += s.cards.Count; }
                if (n > 0) b.Append(b.Length > 0 ? ", " : "").Append(p.title).Append(' ').Append(n).Append(" in ").Append(cards);
            }
            Logger.LogInfo("VaM VR menu: added the '" + MenuTabLabel + "' tab with " + items.Count + " settings to the main menu (" +
                ObjectPath(selector.transform) + "); pages (rows in cards) " + b + "; room for rows " + rowsRoom.ToString("F0"));
        }
        catch (Exception e)
        {
            if (menuToggle != null) Destroy(menuToggle);
            if (menuContent != null) Destroy(menuContent);
            menuToggle = menuContent = null;
            statusText = null;
            barButtons.Clear();
            menuAttempts++;
            Logger.LogWarning("VaM VR menu: could not add the menu tab (attempt " + menuAttempts + "): " + e);
            if (menuAttempts >= MenuAttemptsMax)
            {
                menuGaveUp = true;
                Logger.LogWarning("VaM VR menu: menu tab given up; the launch panel still changes these settings while VaM runs");
            }
        }
    }

    private GameObject MakeTabToggle(UITabSelector selector, Transform toggles, Toggle template, List<Toggle> tabToggles)
    {
        // Clone under an inactive holder: an enabled clone of the selected tab
        // would join the ToggleGroup switched on and deselect VaM's own tab.
        GameObject holder = new GameObject("VaMVRMenuHolder");
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
            Logger.LogInfo("VaM VR menu: menu toggle placed by hand at " + rt.anchoredPosition + " (step " + step + ")");
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
        // VaM's own text and its buttons' rounded sprite, from its text field and button prefabs.
        Text sample = prefabs.configurableTextFieldPrefab.GetComponentInChildren<Text>(true);
        if (sample != null) { font = sample.font; fontSize = sample.fontSize; }
        if (font == null) font = Resources.GetBuiltinResource<Font>("Arial.ttf");
        UIDynamicButton buttonSample = prefabs.configurableButtonPrefab.GetComponent<UIDynamicButton>();
        if (buttonSample != null && buttonSample.buttonImage != null)
        {
            buttonSprite = buttonSample.buttonImage.sprite;
            buttonSpriteType = buttonSample.buttonImage.type;
        }
        // The panel takes the size of VaM's own tabs; their usual one if it is not known yet.
        float width = panel.rect.width > 400f ? panel.rect.width : 1100f, height = panel.rect.height > 400f ? panel.rect.height : 1380f;
        int small = Mathf.RoundToInt(fontSize * 0.85f);

        float statusHeight = Mathf.Ceil(small * 1.2f * StatusLines);
        statusText = MakeText(Area(panel, "Status", Pad, Pad, width - 2f * Pad, statusHeight), "", small, FontStyle.Normal, DimColor, TextAnchor.UpperLeft);
        Fill(statusText.rectTransform);
        float top = Pad + statusHeight + Gap, below = height - top - Pad;

        // Pages, a side bar on the left; Back to launch settings at its bottom.
        RectTransform nav = Area(panel, "Pages", Pad, top, NavWidth, below);
        Paint(nav.gameObject, SideBar);
        VerticalLayoutGroup navLayout = Stack(nav, false, 8f);
        navLayout.padding = new RectOffset(8, 8, 8, 8);
        foreach (Page p in pages)
        {
            Page page = p;                                 // each callback keeps its own page
            p.button = MakeBtn(nav, p.title, Look.Page, fontSize + 4);
            Size(p.button.button, -1f, NavHeight);
            p.button.button.onClick.AddListener(delegate() { shownPage = page; shownPart = 0; SyncMenu(); });
        }
        GameObject spacer = new GameObject("Spacer", typeof(RectTransform));
        spacer.transform.SetParent(nav, false);
        spacer.AddComponent<LayoutElement>().flexibleHeight = 1f;
        Btn restore = MakeBtn(nav, Lang.T("Back to launch settings"), Look.Plain, small);
        Size(restore.button, -1f, 72f);
        restore.button.onClick.AddListener(delegate() { RestoreLaunchValues(); });

        // The page, right of them: its sub-page tabs, then its cards.
        float left = Pad + NavWidth + Gap, pageWidth = width - left - Pad;
        barArea = Area(panel, "Tabs", left, top, pageWidth, BarHeight);
        HorizontalLayoutGroup bar = barArea.gameObject.AddComponent<HorizontalLayoutGroup>();
        bar.spacing = 4f;
        bar.childAlignment = TextAnchor.LowerLeft;
        bar.childControlWidth = true; bar.childControlHeight = true;
        bar.childForceExpandWidth = false; bar.childForceExpandHeight = true;
        Image baseline = Part(barArea, "Baseline", Vector2.zero, new Vector2(1f, 0f), Vector2.zero, new Vector2(0f, 2f), new Color(1f, 1f, 1f, 0.18f));
        baseline.sprite = null;
        baseline.gameObject.AddComponent<LayoutElement>().ignoreLayout = true;
        // The cards start under the tabs when the page has sub-pages, else where the tabs are;
        // the room for them is the smaller one either way.
        rowsTop = top;
        rowsRoom = below - BarHeight - Gap;
        rowsArea = Area(panel, "Cards", left, top, pageWidth, rowsRoom);
        Stack(rowsArea, false, CardGap);
        barButtons.Clear();                                // a rebuilt tab: none of the last one's controls
        foreach (Item i in items) { i.buttons.Clear(); i.bar = null; i.valueText = null; }
        // A card for each switch and the rows that belong to it, in each section.
        foreach (Page p in pages)
            foreach (Section s in p.sections)
            {
                s.cards.Clear();
                Card card = null;
                foreach (Item i in s.members)
                {
                    string key = CardKey(i);
                    if (card == null || key != card.key)
                    {
                        card = new Card();
                        card.key = key;
                        card.go = MakeCard();
                        s.cards.Add(card);
                    }
                    card.rows.Add(i);
                    AddControl(card, i);
                }
            }
    }

    // The item a row belongs to - the switch it sits under (parent), the item
    // that hides it (unlessId), or the one it is grouped with (group) - one step.
    private Item Owner(Item item)
    {
        Item up;
        string id = item.group ?? item.parent ?? item.unlessId;
        return id != null && byId.TryGetValue(id, out up) ? up : null;
    }

    private string CardKey(Item item)
    {
        Item i = item;
        for (Item up = Owner(i); up != null && up != item; up = Owner(i)) i = up;
        return i.id;
    }

    private int Depth(Item item)
    {
        int depth = 0;
        for (Item up = Owner(item); up != null && depth < 8; up = Owner(up)) depth++;
        return depth;
    }

    private GameObject MakeCard()
    {
        GameObject card = new GameObject("Card", typeof(RectTransform));
        card.transform.SetParent(rowsArea, false);
        Paint(card, CardColor);
        VerticalLayoutGroup v = Stack((RectTransform)card.transform, false, RowGap);
        v.padding = new RectOffset(Mathf.RoundToInt(CardPadX), Mathf.RoundToInt(CardPadX), Mathf.RoundToInt(CardPadY), Mathf.RoundToInt(CardPadY));
        LayoutElement e = card.AddComponent<LayoutElement>();
        e.flexibleHeight = 0f;                             // a row's label would make the cards share the room
        card.SetActive(false);                             // SyncMenu shows the page's cards
        return card;
    }

    private void AddControl(Card card, Item item)
    {
        Item it = item;                                    // each callback keeps its own item
        // Under its switch: the label moves right on a guide line, the controls stay in line.
        float indent = Mathf.Max(0, Depth(it) - Depth(card.rows[0])) * ChildIndent;
        bool head = it.id == card.key;                     // the switch the card is about
        GameObject row = new GameObject("Row " + it.id, typeof(RectTransform));
        row.transform.SetParent(card.go.transform, false);
        HorizontalLayoutGroup h = row.AddComponent<HorizontalLayoutGroup>();
        h.padding = new RectOffset(Mathf.RoundToInt(indent), 0, 0, 0);
        h.spacing = 10f;
        h.childAlignment = TextAnchor.MiddleLeft;
        h.childControlWidth = true; h.childControlHeight = true;
        h.childForceExpandWidth = false; h.childForceExpandHeight = false;
        LayoutElement re = row.AddComponent<LayoutElement>();
        re.minHeight = re.preferredHeight = RowHeightOf(it);
        re.flexibleWidth = 1f;
        re.flexibleHeight = 0f;                            // the label's would make the rows share the room
        it.control = row;
        if (indent > 0f)
        {
            // Up to the row above, so the lines of one switch's rows join.
            Image line = Part((RectTransform)row.transform, "Guide", Vector2.zero, new Vector2(0f, 1f),
                new Vector2(indent - ChildIndent / 2f - 1.5f, 0f), new Vector2(indent - ChildIndent / 2f + 1.5f, RowGap), Guide);
            line.sprite = null;
            line.gameObject.AddComponent<LayoutElement>().ignoreLayout = true;
        }

        int small = Mathf.RoundToInt(fontSize * 0.85f);
        bool dim = it.kind == Kind.Status;
        it.labelText = MakeText(row.transform, dim ? LiveText(it) : it.label, dim || it.skinSlot >= 0 ? small : fontSize,
            head ? FontStyle.Bold : FontStyle.Normal, dim ? DimColor : LabelColor, TextAnchor.MiddleLeft);
        it.labelText.name = "Label";
        LayoutElement le = it.labelText.gameObject.AddComponent<LayoutElement>();
        le.flexibleHeight = 1f;
        if (dim) { le.flexibleWidth = 1f; return; }
        le.minWidth = le.preferredWidth = LabelWidth - indent;

        if (it.kind == Kind.Slider)
        {
            Btn minus = MakeBtn(row.transform, "−", Look.Plain, fontSize + 6);
            Size(minus.button, StepWidth, RowHeight - 12f);
            minus.button.onClick.AddListener(delegate() { Step(it, -1); });
            it.bar = MakeBar(row.transform, it);
            Btn plus = MakeBtn(row.transform, "+", Look.Plain, fontSize + 6);
            Size(plus.button, StepWidth, RowHeight - 12f);
            plus.button.onClick.AddListener(delegate() { Step(it, 1); });
            it.valueText = MakeText(row.transform, ValueText(it), fontSize, FontStyle.Bold, LabelColor, TextAnchor.MiddleRight);
            LayoutElement ve = it.valueText.gameObject.AddComponent<LayoutElement>();
            ve.minWidth = ve.preferredWidth = ValueWidth;
            ve.flexibleHeight = 1f;
            return;
        }
        // Off / On or the choices: one segmented row.
        GameObject line2 = new GameObject("Choices", typeof(RectTransform));
        line2.transform.SetParent(row.transform, false);
        HorizontalLayoutGroup c = line2.AddComponent<HorizontalLayoutGroup>();
        c.padding = new RectOffset(0, 0, 6, 6);
        c.spacing = 4f;
        c.childControlWidth = true; c.childControlHeight = true;
        c.childForceExpandWidth = true; c.childForceExpandHeight = true;
        LayoutElement ce = line2.AddComponent<LayoutElement>();
        ce.flexibleWidth = 1f; ce.flexibleHeight = 1f;
        for (int k = 0; k < it.values.Length; k++)
        {
            string value = it.values[k];
            Btn b = MakeBtn(line2.transform, it.names[k], Look.Option, fontSize);   // an automatic translator must not touch K / L / M (NoTranslate)
            LayoutElement e = b.button.gameObject.AddComponent<LayoutElement>();
            e.minWidth = 0f; e.preferredWidth = 0f; e.flexibleWidth = 1f;
            b.button.onClick.AddListener(delegate() { SetValue(it, value); });
            it.buttons.Add(b);
        }
    }

    // A button of the given level: a rounded fill that takes the pointer, its
    // text, and for a page or a tab the mark that shows it is the one shown.
    private Btn MakeBtn(Transform parent, string label, Look look, int size)
    {
        GameObject go = new GameObject("Button", typeof(RectTransform));
        go.transform.SetParent(parent, false);
        Btn b = new Btn();
        b.look = look;
        b.fill = Paint(go, Color.white);
        b.button = go.AddComponent<Button>();
        b.button.targetGraphic = b.fill;
        ColorBlock colors = ColorBlock.defaultColorBlock;
        colors.normalColor = Color.white;
        colors.highlightedColor = new Color(1.3f, 1.3f, 1.3f, 2f);   // the pointer on it: brighter, a faint fill twice as strong
        colors.pressedColor = new Color(0.8f, 0.8f, 0.8f, 1f);
        colors.colorMultiplier = 1f;
        colors.fadeDuration = 0.05f;
        b.button.colors = colors;
        Navigation none = new Navigation();
        none.mode = Navigation.Mode.None;
        b.button.navigation = none;
        bool level = look == Look.Page || look == Look.Tab;
        b.text = MakeText(go.transform, label, size, level ? FontStyle.Bold : FontStyle.Normal, LabelColor,
            look == Look.Page ? TextAnchor.MiddleLeft : TextAnchor.MiddleCenter);
        Fill(b.text.rectTransform);
        if (look == Look.Page)
        {
            b.text.rectTransform.offsetMin = new Vector2(24f, 0f);
            b.mark = Part((RectTransform)go.transform, "Edge", new Vector2(0f, 0.14f), new Vector2(0f, 0.86f), Vector2.zero, new Vector2(7f, 0f), Bright);
        }
        else if (look == Look.Tab)
            b.mark = Part((RectTransform)go.transform, "Underline", new Vector2(0.08f, 0f), new Vector2(0.92f, 0f), Vector2.zero, new Vector2(0f, 6f), Bright);
        if (b.mark != null) b.mark.raycastTarget = false;
        Light(b, false);
        return b;
    }

    private void Light(Btn b, bool on)
    {
        switch (b.look)
        {
            case Look.Page:
                b.fill.color = on ? PageShown : new Color(1f, 1f, 1f, 0.05f);
                b.text.color = on ? Color.white : DimColor;
                b.mark.enabled = on;
                break;
            case Look.Tab:
                b.fill.color = on ? new Color(1f, 1f, 1f, 0.10f) : new Color(1f, 1f, 1f, 0.04f);
                b.text.color = on ? Color.white : DimColor;
                b.mark.enabled = on;
                break;
            case Look.Option:
                b.fill.color = on ? Accent : OptionOff;
                b.text.color = on ? Color.white : LabelColor;
                b.text.fontStyle = on ? FontStyle.Bold : FontStyle.Normal;
                break;
            default:
                b.fill.color = PlainFill;
                b.text.color = LabelColor;
                break;
        }
    }

    // A number's bar: a Unity slider the height of its row (the whole row
    // height takes the pointer), the track a band across its middle.
    private Slider MakeBar(Transform parent, Item it)
    {
        GameObject go = new GameObject("Bar", typeof(RectTransform));
        go.transform.SetParent(parent, false);
        LayoutElement e = go.AddComponent<LayoutElement>();
        e.minWidth = 120f; e.flexibleWidth = 1f; e.flexibleHeight = 1f;
        Image hit = go.AddComponent<Image>();
        hit.color = new Color(1f, 1f, 1f, 0f);
        RectTransform root = go.GetComponent<RectTransform>();
        Image track = Part(root, "Track", new Vector2(0f, 0.36f), new Vector2(1f, 0.64f), Vector2.zero, Vector2.zero, TrackColor);
        RectTransform fillArea = Part(root, "Fill Area", new Vector2(0f, 0.36f), new Vector2(1f, 0.64f),
            new Vector2(HandleWidth / 2f, 0f), new Vector2(-HandleWidth / 2f, 0f), Color.clear).rectTransform;
        fillArea.GetComponent<Image>().enabled = false;
        Image fill = Part(fillArea, "Fill", Vector2.zero, new Vector2(0f, 1f), new Vector2(-HandleWidth / 2f, 0f), Vector2.zero, Accent);
        RectTransform slide = Part(root, "Handle Slide Area", Vector2.zero, Vector2.one,
            new Vector2(HandleWidth / 2f, 4f), new Vector2(-HandleWidth / 2f, -4f), Color.clear).rectTransform;
        slide.GetComponent<Image>().enabled = false;
        Image handle = Part(slide, "Handle", Vector2.zero, new Vector2(0f, 1f), Vector2.zero, Vector2.zero, Color.white);
        handle.rectTransform.sizeDelta = new Vector2(HandleWidth, 0f);
        track.raycastTarget = fill.raycastTarget = false;
        Slider s = go.AddComponent<Slider>();
        s.fillRect = fill.rectTransform;
        s.handleRect = handle.rectTransform;
        s.targetGraphic = handle;
        s.direction = Slider.Direction.LeftToRight;
        Navigation none = new Navigation();
        none.mode = Navigation.Mode.None;
        s.navigation = none;
        s.minValue = it.min; s.maxValue = it.max;
        s.wholeNumbers = !it.fractional;
        s.value = SliderValue(it);
        s.onValueChanged.AddListener(delegate(float v) { OnSlider(it, v); });
        return s;
    }

    private Image Part(RectTransform parent, string name, Vector2 anchorMin, Vector2 anchorMax, Vector2 offsetMin, Vector2 offsetMax, Color color)
    {
        GameObject go = new GameObject(name, typeof(RectTransform));
        RectTransform rt = go.GetComponent<RectTransform>();
        rt.SetParent(parent, false);
        rt.anchorMin = anchorMin; rt.anchorMax = anchorMax;
        rt.offsetMin = offsetMin; rt.offsetMax = offsetMax;
        return Paint(go, color);
    }

    // A rounded fill (the button prefab's sprite) behind a box.
    private Image Paint(GameObject go, Color color)
    {
        Image image = go.AddComponent<Image>();
        image.sprite = buttonSprite;
        image.type = buttonSprite != null ? buttonSpriteType : Image.Type.Simple;
        image.color = color;
        return image;
    }

    // A box at a place in the panel (from its top left), not sized by a layout.
    private static RectTransform Area(RectTransform parent, string name, float x, float y, float width, float height)
    {
        GameObject go = new GameObject(name, typeof(RectTransform));
        RectTransform rt = go.GetComponent<RectTransform>();
        rt.SetParent(parent, false);
        rt.anchorMin = rt.anchorMax = rt.pivot = new Vector2(0f, 1f);
        rt.anchoredPosition = new Vector2(x, -y);
        rt.sizeDelta = new Vector2(width, height);
        return rt;
    }

    private static void Fill(RectTransform rt)
    {
        rt.anchorMin = Vector2.zero; rt.anchorMax = Vector2.one;
        rt.offsetMin = rt.offsetMax = Vector2.zero;
    }

    // Children one under the other, as wide as the box, each its own height.
    private static VerticalLayoutGroup Stack(RectTransform box, bool expand, float spacing)
    {
        VerticalLayoutGroup v = box.gameObject.AddComponent<VerticalLayoutGroup>();
        v.spacing = spacing;
        v.childAlignment = TextAnchor.UpperLeft;
        v.childControlWidth = true; v.childControlHeight = true;
        v.childForceExpandWidth = true; v.childForceExpandHeight = expand;
        return v;
    }

    // A fixed size in a layout; width < 0 leaves the width to the layout.
    private static void Size(Component c, float width, float height)
    {
        LayoutElement e = c.GetComponent<LayoutElement>();
        if (e == null) e = c.gameObject.AddComponent<LayoutElement>();
        if (width >= 0f) { e.minWidth = e.preferredWidth = width; e.flexibleWidth = 0f; }
        e.minHeight = e.preferredHeight = height;
        e.flexibleHeight = 0f;
    }

    private Text MakeText(Transform parent, string text, int size, FontStyle style, Color color, TextAnchor alignment)
    {
        GameObject go = new GameObject("Text", typeof(RectTransform));
        go.transform.SetParent(parent, false);
        Text t = go.AddComponent<Text>();
        t.font = font; t.fontSize = size; t.fontStyle = style; t.color = color;
        t.alignment = alignment;
        t.horizontalOverflow = HorizontalWrapMode.Wrap;
        t.verticalOverflow = VerticalWrapMode.Overflow;
        t.supportRichText = false;
        t.raycastTarget = false;
        t.text = NoTranslate + text;
        return t;
    }

    // ---- state -------------------------------------------------------------

    private static float RowHeightOf(Item item) { return item.kind == Kind.Status ? LiveHeight : RowHeight; }

    private static float SliderValue(Item item)
    {
        return float.Parse(item.value, System.Globalization.CultureInfo.InvariantCulture);
    }

    private static string ValueText(Item item)
    {
        return item.fractional ? SliderValue(item).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) : item.value + item.unit;
    }

    private static string LiveText(Item item)
    {
        try { return item.live() ?? ""; }
        catch (Exception) { return ""; }
    }

    private void OnSlider(Item item, float v)
    {
        if (item.fractional)
            SetValue(item, Mathf.Clamp(v, item.min, item.max).ToString("0.##", System.Globalization.CultureInfo.InvariantCulture));
        else
            SetValue(item, Mathf.Clamp(Mathf.RoundToInt(v), item.min, item.max).ToString());
    }

    // − and +: to the next multiple of the step down or up (33 + 5 = 35).
    private void Step(Item item, int direction)
    {
        float v = SliderValue(item), s = item.step > 0f ? item.step : 1f;
        float next = direction > 0 ? (Mathf.Floor(v / s + 0.001f) + 1f) * s : (Mathf.Ceil(v / s - 0.001f) - 1f) * s;
        OnSlider(item, Mathf.Clamp(next, item.min, item.max));
    }

    // A card's height with the rows it shows now; 0 when it shows none.
    private float CardHeight(Card card)
    {
        float h = 0f;
        int n = 0;
        foreach (Item i in card.rows)
            if (Shown(i)) { h += RowHeightOf(i); n++; }
        return n == 0 ? 0f : h + (n - 1) * RowGap + 2f * CardPadY;
    }

    // A page's cards to show, in parts that fit the room: each section with a
    // row shown is a part, or more than one when it is longer.
    private List<List<Card>> Parts(Page page, List<string> titles)
    {
        List<List<Card>> parts = new List<List<Card>>();
        foreach (Section s in page.sections)
        {
            int first = parts.Count;
            List<Card> part = null;
            float used = 0f;
            foreach (Card c in s.cards)
            {
                float h = CardHeight(c);
                if (h <= 0f) continue;
                if (part == null || (part.Count > 0 && used + CardGap + h > rowsRoom))
                {
                    part = new List<Card>();
                    parts.Add(part);
                    used = -CardGap;
                }
                part.Add(c);
                used += CardGap + h;
            }
            int n = parts.Count - first;
            for (int k = 0; k < n; k++) titles.Add((s.title ?? page.title) + (n > 1 ? " " + (k + 1) : ""));
        }
        return parts;
    }

    private void SyncMenu()
    {
        if (!menuBuilt) return;
        syncing = true;
        try
        {
            foreach (Item i in items)
            {
                if (i.control == null) continue;
                if (i.skinSlot >= 0 && i.labelText != null) i.labelText.text = NoTranslate + (i.skinLabel ?? "");
                if (i.bar != null) i.bar.value = SliderValue(i);
                if (i.valueText != null) i.valueText.text = NoTranslate + ValueText(i);
                for (int k = 0; k < i.buttons.Count; k++) Light(i.buttons[k], i.values[k] == i.value);
            }
            // The pages with a row to show; the one shown stays unless it has none left.
            List<List<Card>> parts = null;
            List<string> titles = null;
            Page first = null;
            foreach (Page p in pages)
            {
                List<string> t = new List<string>();
                List<List<Card>> c = Parts(p, t);
                bool any = c.Count > 0;
                if (p.button != null && p.button.button.gameObject.activeSelf != any) p.button.button.gameObject.SetActive(any);
                if (any && first == null) first = p;
                if (any && p == shownPage) { parts = c; titles = t; }
            }
            if (parts == null)
            {
                shownPage = first;
                shownPart = 0;
                titles = new List<string>();
                parts = first != null ? Parts(first, titles) : new List<List<Card>>();
            }
            shownPart = Mathf.Clamp(shownPart, 0, Mathf.Max(0, parts.Count - 1));
            foreach (Page p in pages) if (p.button != null) Light(p.button, p == shownPage);
            List<Card> cards = parts.Count > 0 ? parts[shownPart] : new List<Card>();
            int rows = 0;
            foreach (Page p in pages)
                foreach (Section s in p.sections)
                    foreach (Card c in s.cards)
                    {
                        bool on = cards.Contains(c);
                        if (c.go.activeSelf != on) c.go.SetActive(on);
                        if (!on) continue;
                        foreach (Item i in c.rows)
                        {
                            bool shown = Shown(i);
                            if (i.control.activeSelf != shown) i.control.SetActive(shown);
                            if (!shown) continue;
                            rows++;
                            if (i.live != null) i.labelText.text = NoTranslate + LiveText(i);
                        }
                    }
            // The tabs: the sub-pages, when the page has more than one.
            bool tabs = parts.Count > 1;
            while (tabs && barButtons.Count < parts.Count) AddBarButton();
            for (int k = 0; k < barButtons.Count; k++)
            {
                bool shown = tabs && k < parts.Count;
                if (barButtons[k].button.gameObject.activeSelf != shown) barButtons[k].button.gameObject.SetActive(shown);
                if (!shown) continue;
                barButtons[k].text.text = NoTranslate + titles[k];
                Light(barButtons[k], k == shownPart);
            }
            if (barArea.gameObject.activeSelf != tabs) barArea.gameObject.SetActive(tabs);
            rowsArea.anchoredPosition = new Vector2(rowsArea.anchoredPosition.x, -(tabs ? rowsTop + BarHeight + Gap : rowsTop));
            LogPage(titles, cards, rows);
        }
        finally { syncing = false; }
        nextStatus = 0f;
    }

    private void AddBarButton()
    {
        int k = barButtons.Count;                          // each callback keeps its own sub-page
        Btn b = MakeBtn(barArea, "", Look.Tab, fontSize + 2);
        Size(b.button, BarTabWidth, BarHeight);
        b.button.onClick.AddListener(delegate() { shownPart = k; SyncMenu(); });
        barButtons.Add(b);
    }

    // What this run does once the menus are closed, the per-eye sizes, and
    // what is shown while they are open (the tab is only seen then).
    private string StatusText()
    {
        StringBuilder b = new StringBuilder();
        Item item;
        bool path = PathOn();
        string model = path ? "RR" : dlss && byId.TryGetValue(PresetId, out item) ? item.value : "K";
        int percent = byId.TryGetValue(path ? ScaleId + "_RR" : ScaleId, out item) ? int.Parse(item.value) : renderScale;
        int output = byId.TryGetValue(OutputId, out item) ? int.Parse(item.value) : 100;
        bool sr = dlss && model != "Off";
        if (dlss)
        {
            if (!sr) b.Append(Lang.T("No upscaling (native)"));
            else
            {
                bool other = model == "XeSS" || model == "FSR";      // the upscalers that are not DLSS
                b.Append(other ? model : "DLSS " + (model == "RR" ? "RR · F" : model))
                    .Append(percent < 100 ? Lang.T(" upscaling") : other ? Lang.T(" native anti-aliasing") : " DLAA")
                    .Append(Lang.F(" · render {0}%", percent));
            }
            if (!Desktop) b.Append(Lang.F(" · output {0}%", output));
        }
        else b.Append(Lang.T("No DLSS upscaling"));
        b.Append('\n');
        if (launchMultiplier == 1) b.Append(Lang.T("No frame generation"));
        else if (layer.path == null) b.Append(Desktop ? Lang.T("Waiting for the presenter") : Lang.T("No OFXR layer found"));
        else if (pauseName != null && !FrameGenOn) b.Append(Lang.T("Frame generation is off"));
        else
        {
            // What the session runs, where the layer says; else what its file asks for. FSR's makes one frame per pair.
            Item multiplier, generator;
            bool fsr = fsrGeneratorOnly || (byId.TryGetValue(FgModelId, out generator) && generator.value == "FSR");
            int running = fsr ? 2 : layerReports && framesNow >= 2 ? framesNow
                : byId.TryGetValue(MultiplierId, out multiplier) ? int.Parse(multiplier.value) : 0;
            b.Append(fsr ? Lang.T("FSR frame generation") : dlssg ? Lang.T("DLSS Frame Generation") : Lang.T("Optical-flow frame generation"));
            if (running >= 2) b.Append(' ').Append(running).Append('×');
        }
        string sizes = EyeSizes(sr ? percent : 100);
        if (sizes != null) b.Append(" · ").Append(sizes);
        bool fg = pauseName != null && FrameGenOn;
        if (MenuNativeOn && (sr || fg) && MenusOpen())
            b.Append('\n').Append(Lang.T("Menus open: ")).Append(sr && fg ? Lang.T("no upscaling, no frame generation") : sr ? Lang.T("no upscaling") : Lang.T("no frame generation"));
        if (message.Length > 0) b.Append('\n').Append(message);
        return b.ToString();
    }

    // The output OpenComposite last reported for the left eye
    // (performance.ini.eye0.json, written when a size changes) and the render
    // for it at `percent`: while the tab is seen VaM's menus are open, and the
    // render then is the menus' own.
    private string EyeSizes(int percent)
    {
        try
        {
            string path = performance.path + ".eye0.json";
            if (!File.Exists(path)) return null;
            Match m = Regex.Match(File.ReadAllText(path), "\"output\":\\[(\\d+),(\\d+)\\]");
            if (!m.Success) return null;
            int ow = int.Parse(m.Groups[1].Value), oh = int.Parse(m.Groups[2].Value);
            return (Desktop ? Lang.T("Window ") : Lang.T("Per eye ")) + (ow * percent / 100) + "×" + (oh * percent / 100) + " → " + ow + "×" + oh;
        }
        catch (Exception) { return null; }
    }

    // ---- diagnostics -----------------------------------------------------

    // Each page and sub-page the first time it is shown: its cards, rows and the room they take.
    private void LogPage(List<string> titles, List<Card> cards, int rows)
    {
        if (shownPage == null || cards.Count == 0) return;
        float used = -CardGap;
        foreach (Card c in cards) used += CardGap + CardHeight(c);
        string key = shownPage.title + " · " + (titles.Count > 1 ? titles[shownPart] + " (" + (shownPart + 1) + " of " + titles.Count + ")" : titles[0]) +
            ": " + rows + " rows in " + cards.Count + " cards, " + used.ToString("F0") + " of " + rowsRoom.ToString("F0");
        if (pagesLogged.Contains(key)) return;
        pagesLogged.Add(key);
        if (pagesLogged.Count <= 40) Logger.LogInfo("VaM VR menu: page " + key);
    }

    // For a test (VaMTestAgent invoke), with the tab in view: shows every page and
    // sub-page in turn and reports what does not fit - a card past the bottom of
    // the room, a row taller than a row, a label taller than its row, a button or
    // value text wider than its box - then shows the page that was shown before.
    private string LayoutAudit()
    {
        if (!menuBuilt || rowsArea == null) return "no tab";
        Page was = shownPage;
        int wasPart = shownPart, shown = 0, cardsShown = 0;
        List<string> problems = new List<string>();
        Vector3[] corners = new Vector3[4];
        foreach (Page p in pages)
        {
            int count = Parts(p, new List<string>()).Count;
            for (int k = 0; k < count; k++)
            {
                shownPage = p; shownPart = k;
                SyncMenu();
                Canvas.ForceUpdateCanvases();
                Rect room = rowsArea.rect;
                foreach (Transform card in rowsArea)
                {
                    if (!card.gameObject.activeSelf) continue;
                    cardsShown++;
                    ((RectTransform)card).GetWorldCorners(corners);
                    if (rowsArea.InverseTransformPoint(corners[0]).y < room.yMin - 1f) problems.Add(p.title + " " + (k + 1) + " a card past the bottom");
                    foreach (Transform row in card)
                    {
                        if (!row.gameObject.activeSelf) continue;
                        shown++;
                        float rh = ((RectTransform)row).rect.height;
                        if (rh > RowHeight + 1f) problems.Add(p.title + " " + (k + 1) + " " + row.name + " " + rh.ToString("F0") + " tall");
                        foreach (Text t in row.GetComponentsInChildren<Text>(false))
                        {
                            Rect r = t.rectTransform.rect;
                            bool wraps = t.name == "Label" || t.text.Trim().IndexOf(' ') > 0;   // a label, or words that can take a second line
                            if (t.preferredHeight > r.height + 1f)
                                problems.Add(p.title + " " + (k + 1) + " " + row.name + " '" + t.text.Trim() + "' " + t.preferredHeight.ToString("F0") + " tall in " + r.height.ToString("F0"));
                            else if (!wraps && t.preferredWidth > r.width + 1f)
                                problems.Add(p.title + " " + (k + 1) + " " + row.name + " '" + t.text.Trim() + "' " + t.preferredWidth.ToString("F0") + " wide in " + r.width.ToString("F0"));
                        }
                    }
                }
            }
        }
        foreach (Page p in pages) if (p.button != null && p.button.text.preferredWidth > p.button.text.rectTransform.rect.width + 1f)
            problems.Add("page button '" + p.title + "' too narrow");
        shownPage = was; shownPart = wasPart;
        SyncMenu();
        return "rows " + shown + " in " + cardsShown + " cards, room " + rowsRoom.ToString("F0") + ", font " + fontSize + "; " +
            (problems.Count == 0 ? "everything fits" : problems.Count + " problems: " + string.Join(" | ", problems.ToArray()));
    }

    private void LogMenuLayout(UITabSelector selector, Transform toggles, List<Toggle> tabToggles)
    {
        StringBuilder b = new StringBuilder("VaM VR menu: menu layout ").Append(ObjectPath(selector.transform))
            .Append(", toggles under ").Append(ObjectPath(toggles));
        LayoutGroup group = toggles.GetComponent<LayoutGroup>();
        b.Append(group != null ? " (" + group.GetType().Name + ")" : " (no layout group)");
        // Whether VaM's tab buttons leave room for more: the column's own size
        // and how many buttons are active in it.
        RectTransform column = toggles as RectTransform;
        int active = 0;
        foreach (Transform child in toggles) if (child.gameObject.activeSelf) active++;
        if (column != null) b.Append(", column ").Append(column.rect.size).Append(" with ").Append(active).Append(" active buttons");
        RectTransform selectorRect = selector.transform as RectTransform;
        if (selectorRect != null) b.Append(", tab area ").Append(selectorRect.rect.size);
        foreach (Toggle t in tabToggles)
        {
            RectTransform r = t.GetComponent<RectTransform>();
            b.Append("\n  toggle '").Append(t.name).Append("'");
            if (r != null) b.Append(" pos ").Append(r.anchoredPosition).Append(" size ").Append(r.sizeDelta);
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
