using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.UI;

public sealed partial class VaMDisplayLighting
{
    private const string TabName = "Display Lighting";
    private bool menuAttempted;
    private GameObject menuPanel, menuTab;
    private UIDynamicButton runButton;
    private readonly List<UIDynamicButton> styleButtons = new List<UIDynamicButton>();
    private Text statusLabel;

    private void UpdateMenu()
    {
        if (menuPanel != null || menuAttempted || controller == null || controller.isLoading || controller.mainMenuTabSelector == null) return;
        menuAttempted = true;
        // One seed at UI creation, retried on scene-loaded events only.
        MVRPluginManager prefabs = FindObjectOfType<MVRPluginManager>();
        if (prefabs == null) return;
        try { BuildMenu(controller.mainMenuTabSelector, prefabs); }
        catch (Exception e) { DestroyMenu(); Logger.LogError("Display lighting menu: " + e); }
    }

    private void BuildMenu(UITabSelector selector, MVRPluginManager prefabs)
    {
        Transform container = selector.toggleContainer != null ? selector.toggleContainer : selector.transform;
        Dictionary<string, RectTransform> panels = new Dictionary<string, RectTransform>();
        foreach (Transform child in selector.transform)
            if (child.GetComponent<UITab>() != null) panels[child.name] = child as RectTransform;
        if (panels.ContainsKey(TabName)) return;
        List<Toggle> toggles = new List<Toggle>();
        foreach (Transform child in container)
        {
            Toggle toggle = child.GetComponent<Toggle>();
            if (toggle != null && child.gameObject.activeSelf && panels.ContainsKey(child.name)) toggles.Add(toggle);
        }
        if (toggles.Count == 0) throw new Exception("No main-menu tab template");
        Toggle template = toggles[toggles.Count - 1];
        RectTransform reference = panels[template.name];
        GameObject holder = new GameObject("DisplayLightingMenuHolder");
        holder.SetActive(false);
        menuTab = (GameObject)Instantiate(template.gameObject, holder.transform, false);
        menuTab.name = TabName;
        Toggle tab = menuTab.GetComponent<Toggle>();
        tab.onValueChanged = new Toggle.ToggleEvent();
        tab.isOn = false;
        tab.group = template.group;
        Text tabLabel = menuTab.GetComponentInChildren<Text>(true);
        tabLabel.text = "展示照明";
        menuTab.transform.SetParent(container, false);
        menuTab.transform.SetAsLastSibling();
        Destroy(holder);
        tab.onValueChanged.AddListener(delegate(bool on) { if (on) selector.ActiveTabChanged(); });
        if (container.GetComponent<LayoutGroup>() == null)
        {
            RectTransform last = template.GetComponent<RectTransform>();
            Vector2 step = new Vector2(0f, -(last.rect.height + 4f));
            if (toggles.Count > 1)
                step = last.anchoredPosition - toggles[toggles.Count - 2].GetComponent<RectTransform>().anchoredPosition;
            if (step.sqrMagnitude < 1f) step = new Vector2(0f, -(last.rect.height + 4f));
            menuTab.GetComponent<RectTransform>().anchoredPosition = last.anchoredPosition + step;
        }

        menuPanel = new GameObject(TabName, typeof(RectTransform));
        menuPanel.SetActive(false);
        menuPanel.AddComponent<UITab>();
        RectTransform panel = menuPanel.GetComponent<RectTransform>();
        panel.SetParent(selector.transform, false);
        panel.anchorMin = reference.anchorMin; panel.anchorMax = reference.anchorMax; panel.pivot = reference.pivot;
        panel.anchoredPosition = reference.anchoredPosition; panel.sizeDelta = reference.sizeDelta;
        panel.localScale = reference.localScale; panel.localRotation = reference.localRotation;
        Image background = reference.GetComponent<Image>();
        if (background != null)
        {
            Image image = menuPanel.AddComponent<Image>();
            image.sprite = background.sprite; image.type = background.type; image.color = background.color;
        }

        GameObject body = new GameObject("DisplayLightingControls", typeof(RectTransform));
        RectTransform rect = body.GetComponent<RectTransform>();
        rect.SetParent(panel, false);
        rect.anchorMin = new Vector2(0.06f, 0f); rect.anchorMax = new Vector2(0.94f, 1f);
        rect.offsetMin = new Vector2(0f, 20f); rect.offsetMax = new Vector2(0f, -25f);
        VerticalLayoutGroup layout = body.AddComponent<VerticalLayoutGroup>();
        layout.spacing = 14f; layout.childControlWidth = true; layout.childControlHeight = true;
        layout.childForceExpandWidth = true; layout.childForceExpandHeight = false;
        Font font = tabLabel.font;
        AddText(body.transform, font, "展示照明", 38, 60f);
        statusLabel = AddText(body.transform, font, message, 28, 85f);
        runButton = AddButton(prefabs, rect, "启动转换");
        runButton.button.onClick.AddListener(delegate { if (running) StopLighting(); else StartLighting(); });
        styleButtons.Clear();
        string group = null;
        RectTransform row = null;
        foreach (Style style in Styles)
        {
            if (style.Group != group)
            {
                group = style.Group;
                AddText(body.transform, font, group, 28, 42f);
                row = null;
            }
            if (row == null || row.childCount == 2) row = AddRow(rect);
            string id = style.Id;
            UIDynamicButton button = AddButton(prefabs, row, style.Label);
            button.button.onClick.AddListener(delegate { SetStyle(id); });
            styleButtons.Add(button);
        }
        AddText(body.transform, font,
            "主光是柔和的环境光（VaM 的环境球谐和环境高光），灯只做塑形、轮廓和背景色光。都围绕人物、按你看过去的方向摆放：人物走动时跟着走，转身时光在身上流动；窗边和太阳的方向固定在世界里。\n停止后恢复原灯光和环境；切换场景自动停止。", 28, 190f);
        SyncMenu();
        Logger.LogInfo("Display lighting: independent 展示照明 tab ready.");
    }

    private static RectTransform AddRow(RectTransform parent)
    {
        GameObject go = new GameObject("Row", typeof(RectTransform));
        go.transform.SetParent(parent, false);
        HorizontalLayoutGroup layout = go.AddComponent<HorizontalLayoutGroup>();
        layout.spacing = 14f; layout.childControlWidth = true; layout.childControlHeight = true;
        layout.childForceExpandWidth = true; layout.childForceExpandHeight = false;
        LayoutElement element = go.AddComponent<LayoutElement>();
        element.minHeight = 80f; element.preferredHeight = 80f;
        return go.GetComponent<RectTransform>();
    }

    private static UIDynamicButton AddButton(MVRPluginManager prefabs, RectTransform parent, string label)
    {
        Transform instance = (Transform)Instantiate(prefabs.configurableButtonPrefab);
        instance.SetParent(parent, false);
        instance.gameObject.SetActive(true);
        UIDynamicButton button = instance.GetComponent<UIDynamicButton>();
        button.label = label;
        // Avoid any serialized prefab callback being carried into this panel.
        button.button.onClick = new Button.ButtonClickedEvent();
        LayoutElement element = instance.GetComponent<LayoutElement>();
        if (element == null) element = instance.gameObject.AddComponent<LayoutElement>();
        element.minHeight = 80f; element.preferredHeight = 80f; element.flexibleWidth = 1f;
        return button;
    }

    private static Text AddText(Transform parent, Font font, string text, int size, float height)
    {
        GameObject go = new GameObject("Text", typeof(RectTransform));
        go.transform.SetParent(parent, false);
        Text label = go.AddComponent<Text>();
        label.font = font; label.fontSize = size; label.color = Color.white;
        label.alignment = TextAnchor.UpperLeft; label.supportRichText = false;
        label.horizontalOverflow = HorizontalWrapMode.Wrap; label.verticalOverflow = VerticalWrapMode.Overflow;
        label.raycastTarget = false; label.text = text;
        LayoutElement layout = go.AddComponent<LayoutElement>();
        layout.minHeight = height; layout.preferredHeight = height;
        return label;
    }

    private void SyncMenu()
    {
        if (statusLabel != null) statusLabel.text = message;
        if (runButton != null) runButton.label = running ? "停止并恢复" : "启动转换";
        Color chosen = new Color(0.30f, 0.55f, 0.90f);
        for (int i = 0; i < styleButtons.Count && i < Styles.Length; i++)
        {
            if (styleButtons[i] == null) continue;
            bool on = Styles[i].Id == styleSetting.Value;
            styleButtons[i].buttonColor = on ? chosen : Color.white;
            styleButtons[i].textColor = on ? Color.white : Color.black;
        }
    }

    private void DestroyMenu()
    {
        if (menuPanel != null) Destroy(menuPanel);
        if (menuTab != null) Destroy(menuTab);
        menuPanel = menuTab = null;
        statusLabel = null;
        runButton = null;
        styleButtons.Clear();
    }
}
