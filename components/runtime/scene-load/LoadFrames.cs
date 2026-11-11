// VaM Scene Load: the frames of a heavy load ([LoadPaths] in local.vam.sceneload.cfg).
// A stage scene with about 800 atoms, first load after start-up with only the earlier features: 8.4-35.9 s
// creating atoms, one frame each (about 1000 frames of ~27 ms, the atoms' own work ~10% of it); 35.9-71.6 s one frame
// of restoring, where closed dropdowns built a button object for every choice (UIPopup.CreatePanelButtons 20%, every
// atom added rebuilds every atom list) and Unity UI removed each disabled button from its list by a linear search
// through Object.Equals (Selectable.OnDisable 7%); creating the person spent 0.7 s reading morph metadata.
// Each item gives VaM's own result ([LoadPaths] in local.vam.sceneload.cfg):
//   BatchAtomCreation: VaM's LoadCo yields once per atom it creates (StartCoroutine(AddAtomByType)), and an atom
//     whose asset is already loaded still waits a frame (LoadAtomFromBundleAsync waits for the asset manager first).
//     When the atom's prefab is in memory (VaM's own prefab cache, the pool, a built-in prefab) and the asset manager
//     is ready, the atom is made right there, by the same calls in the same order, and LoadCo goes on to the next
//     atom in the same frame, up to AtomFrameBudgetMs per frame; any other atom goes VaM's own way (and waits).
//   DeferPopupButtons: a dropdown (UIPopup) that is closed and gets its choices builds no button objects; everything
//     else CreatePanelButtons leaves is set as it would be (the current button index, the list's height, the scroll
//     position, which buttons are highlighted). The buttons are made, with the texts the choices have by then, right
//     before anything uses them: the dropdown opening, a highlight, its filter re-sorting the buttons.
//   FastSelectableRemove: Selectable.OnDisable takes itself out of Selectable.allSelectables by List.Remove, an
//     Object.Equals call per element; for the elements' types (none overrides Equals, checked for every loaded type)
//     that is reference equality, so the first element that is the same object is taken out, as before.
//   MorphMetaCache: DAZMorph.LoadMetaFromJSON for a morph file (.vmi) VaM imports for a person reads a dozen values
//     out of the JSON (strings, numbers parsed from text, enums by name); its results are kept per file (package path,
//     size, time and entry; a loose file's path, size, time) and set straight on the next import of the same file,
//     here and in later runs (BepInEx\cache\VaMSceneLoad\morphmeta.bin).
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Text;
using System.Threading;
using BepInEx.Configuration;
using HarmonyLib;
using MVR.FileManagement;
using SimpleJSON;
using UnityEngine;
using UnityEngine.UI;

public sealed unsafe partial class VaMSceneLoad
{
    sealed class RefEqOf<T> : IEqualityComparer<T> where T : class
    {
        public static readonly RefEqOf<T> Instance = new RefEqOf<T>();
        public bool Equals(T a, T b) { return ReferenceEquals(a, b); }
        public int GetHashCode(T o) { return System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(o); }
    }

    void SetUpLoadFrames(Harmony h)
    {
        SetUpAtomBatch(h);
        SetUpPopups(h);
        SetUpSelectable(h);
        SetUpMorphMeta(h);
        Logger.LogInfo("VaM scene load: atom batching " + (atomsReady && batchAtoms.Value ? "on" : "off") + ", deferred dropdown buttons " + (popupsReady && deferPopups.Value ? "on" : "off")
            + ", fast Selectable removal " + (selectableReady && fastSelectable.Value ? "on" : "off") + ", morph metadata cache " + (metaReady && metaCache.Value ? "on" : "off"));
    }

    // ---------------------------------------------------------------------------------------------------------------
    // atoms

    static ConfigEntry<bool> batchAtoms;
    static ConfigEntry<int> atomBudgetMs;
    static bool atomsReady, atomsPatched, testAtomsOff;
    static string atomsNote = "";
    static FieldInfo fAtomAssetByType, fAtomPrefabByType, fLastAddedAtom, fTypeToPool, fNameToPrefab, fAssetBundleName, fAssetName, fLoadCoCurrent;
    static Func<SuperController, string, Atom> poolOf;
    static MethodInfo mAddAtomByType;
    static int atomFrame = -1;
    static double atomFrameStart, atomMs;
    static long atomsFast, atomsVaM, atomsOverBudget, atomFrames, atomsFailed;

    void SetUpAtomBatch(Harmony h)
    {
        batchAtoms = Config.Bind("LoadPaths", "BatchAtomCreation", true, "While a scene loads, make an atom whose prefab is already in memory right away and go on to the next atom in the same frame (VaM waits a frame per atom); the same calls in the same order");
        atomBudgetMs = Config.Bind("LoadPaths", "AtomFrameBudgetMs", 100, "Atoms made in one frame stop after this many milliseconds; the rest go on in the next frames");
        BindingFlags inst = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
        try {
            Type sc = typeof(SuperController);
            fAtomAssetByType = Need(sc.GetField("atomAssetByType", inst), "SuperController.atomAssetByType");
            fAtomPrefabByType = Need(sc.GetField("atomPrefabByType", inst), "SuperController.atomPrefabByType");
            fLastAddedAtom = Need(sc.GetField("lastAddedAtom", inst), "SuperController.lastAddedAtom");
            fTypeToPool = Need(sc.GetField("typeToAtomPool", inst), "SuperController.typeToAtomPool");
            fNameToPrefab = Need(sc.GetField("assetBundleAssetNameToPrefab", inst), "SuperController.assetBundleAssetNameToPrefab");
            if (fAtomPrefabByType.FieldType != typeof(Dictionary<string, Atom>) || fLastAddedAtom.FieldType != typeof(Atom)
                || fTypeToPool.FieldType != typeof(Dictionary<string, List<Atom>>) || fNameToPrefab.FieldType != typeof(Dictionary<string, GameObject>)
                || !fAtomAssetByType.FieldType.IsGenericType || fAtomAssetByType.FieldType.GetGenericTypeDefinition() != typeof(Dictionary<,>))
                throw new MissingFieldException("SuperController's atom tables are not as expected");
            Type asset = fAtomAssetByType.FieldType.GetGenericArguments()[1];
            fAssetBundleName = Need(asset.GetField("assetBundleName"), "AtomAsset.assetBundleName");
            fAssetName = Need(asset.GetField("assetName"), "AtomAsset.assetName");
            MethodInfo pool = sc.GetMethod("GetAtomOfTypeFromPool", inst, null, new[] { typeof(string) }, null);
            if (pool == null || pool.ReturnType != typeof(Atom)) throw new MissingMethodException("SuperController.GetAtomOfTypeFromPool");
            poolOf = (Func<SuperController, string, Atom>)Delegate.CreateDelegate(typeof(Func<SuperController, string, Atom>), pool);
            mAddAtomByType = sc.GetMethod("AddAtomByType", inst, null, new[] { typeof(string), typeof(string), typeof(bool), typeof(bool), typeof(bool) }, null);
            if (mAddAtomByType == null || mAddAtomByType.ReturnType != typeof(IEnumerator)) throw new MissingMethodException("SuperController.AddAtomByType");
            Type it = null;
            foreach (Type nt in sc.GetNestedTypes(BindingFlags.NonPublic | BindingFlags.Public))
                if (nt.Name.StartsWith("<LoadCo>")) { if (it != null) throw new Exception("two LoadCo iterators"); it = nt; }
            if (it == null) throw new MissingMemberException("SuperController.LoadCo");
            fLoadCoCurrent = Need(it.GetField("$current", inst), "LoadCo.$current");
            h.Patch(it.GetMethod("MoveNext", inst), null, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("LoadCoTranspiler", BindingFlags.Static | BindingFlags.Public)), null);
            if (!atomsPatched) throw new Exception("LoadCo is not as expected: " + atomsNote);
            atomsReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: atom batching off: " + e.Message);
        }
    }

    // LoadCo, per atom of the scene that does not exist yet:
    //     $current = StartCoroutine(AddAtomByType(type, uid, false, false, false)); return true;   (resume:) ...
    // becomes
    //     $current = StartOrSkip(this, AtomFast(this, type, uid, false, false, false)); if ($current == null) goto resume;
    //     return true;   (resume:) ...
    // AtomFast returns null when it made the atom itself; otherwise VaM's own AddAtomByType iterator, run as before.
    public static IEnumerable<CodeInstruction> LoadCoTranspiler(IEnumerable<CodeInstruction> instructions, ILGenerator il)
    {
        List<CodeInstruction> code = new List<CodeInstruction>(instructions);
        MethodInfo start = typeof(MonoBehaviour).GetMethod("StartCoroutine", new[] { typeof(IEnumerator) });
        int at = -1, sites = 0;
        for (int i = 0; i + 2 < code.Count; i++)
            if ((code[i].opcode == OpCodes.Call || code[i].opcode == OpCodes.Callvirt) && Equals(code[i].operand, mAddAtomByType)
                && (code[i + 1].opcode == OpCodes.Call || code[i + 1].opcode == OpCodes.Callvirt) && Equals(code[i + 1].operand, start)
                && code[i + 2].opcode == OpCodes.Stfld && Equals(code[i + 2].operand, fLoadCoCurrent)) { at = i; sites++; }
        if (sites != 1) { atomsNote = "AddAtomByType yields " + sites; return code; }
        int leave = -1;
        for (int i = at + 3; i < code.Count && i < at + 12; i++)
            if (code[i].opcode == OpCodes.Leave || code[i].opcode == OpCodes.Leave_S) { leave = i; break; }
        if (leave < 0 || leave + 1 >= code.Count) { atomsNote = "no leave after the yield"; return code; }
        CodeInstruction resume = code[leave + 1];
        bool target = false;
        foreach (CodeInstruction c in code)
            if (c.opcode == OpCodes.Switch && c.operand is Label[])
                foreach (Label l in (Label[])c.operand) if (resume.labels.Contains(l)) target = true;
        if (!target) { atomsNote = "the instruction after the yield is not a resume point"; return code; }
        Label go = il.DefineLabel();
        resume.labels.Add(go);
        code[at].opcode = OpCodes.Call;
        code[at].operand = typeof(VaMSceneLoad).GetMethod("AtomFast", BindingFlags.Static | BindingFlags.Public);
        code[at + 1].opcode = OpCodes.Call;
        code[at + 1].operand = typeof(VaMSceneLoad).GetMethod("StartOrSkip", BindingFlags.Static | BindingFlags.Public);
        code.InsertRange(at + 3, new[] { new CodeInstruction(OpCodes.Ldarg_0, null), new CodeInstruction(OpCodes.Ldfld, fLoadCoCurrent), new CodeInstruction(OpCodes.Brfalse, go) });
        atomsPatched = true;
        return code;
    }

    public static Coroutine StartOrSkip(MonoBehaviour mb, IEnumerator e) { return e == null ? null : mb.StartCoroutine(e); }

    static IEnumerator AtomVaM(SuperController sc, string type, string uid, bool userInvoked, bool forceSelect, bool forceFocus)
    {
        atomsVaM++;
        return sc.AddAtomByType(type, uid, userInvoked, forceSelect, forceFocus);
    }

    // VaM's AddAtomByType (and LoadAtomFromBundleAsync for an atom from an asset bundle), without the waits, when none
    // of them would wait for anything: pool -> AddAtom; asset bundle atom -> (asset manager ready) GetCachedPrefab ->
    // AddAtom -> loadedFromBundle; built-in prefab -> AddAtom. The checks only read; the calls are VaM's own.
    public static IEnumerator AtomFast(SuperController sc, string type, string uid, bool userInvoked, bool forceSelect, bool forceFocus)
    {
        if (!atomsReady || testAtomsOff || !batchAtoms.Value || userInvoked || sc == null || string.IsNullOrEmpty(type))
            return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus);
        double t0 = Now();
        int frame = Time.frameCount;
        if (frame != atomFrame) { atomFrame = frame; atomFrameStart = t0; atomFrames++; }
        else if (t0 - atomFrameStart > atomBudgetMs.Value) { atomsOverBudget++; return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus); }
        // which way VaM goes, read only
        bool pooled = false;
        object asset = null;
        string bundle = null, name = null;
        Atom prefabAtom = null;
        try {
            List<Atom> pool;
            Dictionary<string, List<Atom>> pools = (Dictionary<string, List<Atom>>)fTypeToPool.GetValue(sc);
            if (pools != null && pools.TryGetValue(type, out pool) && pool != null) {
                foreach (Atom a in pool) {
                    if ((object)a == null) return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus);   // VaM's own fails there
                    if (!a.inPool) continue;          // GetAtomOfTypeFromPool takes the first one in the pool
                    if (!a) return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus);
                    pooled = true;
                    break;
                }
            }
            if (!pooled) {
                IDictionary assets = (IDictionary)fAtomAssetByType.GetValue(sc);
                if (assets != null && assets.Contains(type)) {
                    asset = assets[type];
                    if (asset == null || !sc.assetManagerReady) return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus);
                    bundle = (string)fAssetBundleName.GetValue(asset);
                    name = (string)fAssetName.GetValue(asset);
                    Dictionary<string, GameObject> cached = (Dictionary<string, GameObject>)fNameToPrefab.GetValue(sc);
                    GameObject prefab;
                    if (cached == null || !cached.TryGetValue(bundle + ":" + name, out prefab) || prefab == null || prefab.GetComponent<Atom>() == null)
                        return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus);   // the asset is loaded first (frames)
                } else {
                    Dictionary<string, Atom> prefabs = (Dictionary<string, Atom>)fAtomPrefabByType.GetValue(sc);
                    if (prefabs == null || !prefabs.TryGetValue(type, out prefabAtom)) return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus);   // VaM's error
                }
            }
        } catch (Exception) {
            return AtomVaM(sc, type, uid, userInvoked, forceSelect, forceFocus);
        }
        // VaM's steps
        try {
            AsyncFlag flag = new AsyncFlag("Load Atom " + type);
            sc.SetLoadingIconFlag(flag);
            fLastAddedAtom.SetValue(sc, null);
            Atom atom = poolOf(sc, type);
            if (atom) {
                sc.AddAtom(atom, uid, userInvoked, forceSelect, forceFocus, false);
            } else if (asset != null) {
                GameObject go = sc.GetCachedPrefab(bundle, name);
                Transform t = sc.AddAtom(go.GetComponent<Atom>(), uid, userInvoked, forceSelect, false, true);
                if (t != null) {
                    Atom made = t.GetComponent<Atom>();
                    if (made != null) made.loadedFromBundle = true;
                }
            } else {
                sc.AddAtom(prefabAtom, uid, userInvoked, forceSelect, forceFocus, true);
            }
            flag.Raise();
            atomsFast++;
        } catch (Exception e) {
            // an exception ends VaM's coroutine (Unity logs it) and LoadCo goes on with the next atom
            atomsFailed++;
            Debug.LogException(e);
        }
        atomMs += Now() - t0;
        return null;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // dropdown buttons

    static ConfigEntry<bool> deferPopups;
    static bool popupsReady, testPopupsOff, materializing, watchingScroll;
    static FieldInfo pPopupValues, pDisplayValues, pFiltered, pButtonToValue, pCurrentValue, pCurrentButtonIndex, pPopupTransforms, pPopupButtons, pVisible, pScrollRect, pHighlightIndex;
    static MethodInfo mCreate;
    public sealed class Pending { public bool[] made, selected; public Color selectColor, normalColor; }
    public sealed class PopupCheck { public Pending d; public float size, scroll; public int cbi; }
    static readonly Dictionary<UIPopup, Pending> unbuilt = new Dictionary<UIPopup, Pending>(RefEqOf<UIPopup>.Instance);
    static int popupVerifyEvery;
    static long popupsDeferred, popupsMaterialized, popupsDropped, popupCalls, popupVerified, popupMismatch, popupTextsSkipped, popupSweeps, popupNotDeferredAnchors;
    // why VaM's own built the buttons (one count per reason)
    static long notOff, notAlwaysOpen, notOpen, notNoPanel, notPanelActive, notNoValues, notOther;
    static float lastReplicaScroll, scrollSeen;
    static double popupMs, materializeMs;
    static string popupMismatchNote = "";

    void SetUpPopups(Harmony h)
    {
        deferPopups = Config.Bind("LoadPaths", "DeferPopupButtons", true, "A closed dropdown that gets new choices makes its button objects when it is opened (or highlighted), with every other part of its state set as VaM sets it; VaM rebuilds every atom dropdown of every plugin for each atom a scene adds");
        BindingFlags inst = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
        try {
            Type p = typeof(UIPopup);
            pPopupValues = Need(p.GetField("_popupValues", inst), "UIPopup._popupValues");
            pDisplayValues = Need(p.GetField("_displayPopupValues", inst), "UIPopup._displayPopupValues");
            pFiltered = Need(p.GetField("_filteredNumPopupValues", inst), "UIPopup._filteredNumPopupValues");
            pButtonToValue = Need(p.GetField("buttonIndexToValueIndex", inst), "UIPopup.buttonIndexToValueIndex");
            pCurrentValue = Need(p.GetField("_currentValue", inst), "UIPopup._currentValue");
            pCurrentButtonIndex = Need(p.GetField("_currentButtonIndex", inst), "UIPopup._currentButtonIndex");
            pPopupTransforms = Need(p.GetField("_popupTransforms", inst), "UIPopup._popupTransforms");
            pPopupButtons = Need(p.GetField("_popupButtons", inst), "UIPopup._popupButtons");
            pVisible = Need(p.GetField("_visible", inst), "UIPopup._visible");
            pScrollRect = Need(p.GetField("scrollRect", inst), "UIPopup.scrollRect");
            pHighlightIndex = Need(p.GetField("_currentHighlightIndex", inst), "UIPopup._currentHighlightIndex");
            if (pPopupValues.FieldType != typeof(string[]) || pFiltered.FieldType != typeof(int) || pButtonToValue.FieldType != typeof(Dictionary<int, int>)
                || pCurrentValue.FieldType != typeof(string) || pCurrentButtonIndex.FieldType != typeof(int) || pPopupTransforms.FieldType != typeof(RectTransform[])
                || pPopupButtons.FieldType != typeof(Button[]) || pVisible.FieldType != typeof(bool) || pScrollRect.FieldType != typeof(ScrollRect) || pHighlightIndex.FieldType != typeof(int))
                throw new MissingFieldException("UIPopup's fields are not as expected");
            mCreate = p.GetMethod("CreatePanelButtons", inst, null, Type.EmptyTypes, null);
            Func<string, Type[], MethodInfo> m = (n, a) => { MethodInfo r = p.GetMethod(n, inst, null, a, null); if (r == null) throw new MissingMethodException("UIPopup." + n); return r; };
            BindingFlags st = BindingFlags.Static | BindingFlags.Public;
            Func<string, HarmonyMethod> hm = n => new HarmonyMethod(typeof(VaMSceneLoad).GetMethod(n, st));
            if (mCreate == null) throw new MissingMethodException("UIPopup.CreatePanelButtons");
            h.Patch(mCreate, hm("CreatePrefix"), hm("CreatePostfix"), null, null);
            h.Patch(m("SetPanelButtonText", new[] { typeof(int) }), hm("TextPrefix"), null, null, null);
            h.Patch(m("ClearPanel", Type.EmptyTypes), hm("DropPrefix"), null, null, null);
            h.Patch(m("Start", Type.EmptyTypes), hm("DropPrefix"), null, null, null);
            foreach (string n in new[] { "SyncFilter", "HighlightCurrentButton", "HighlightPreviousButton", "HighlightNextButton", "MakeVisible" })
                h.Patch(m(n, Type.EmptyTypes), hm("MaterializePrefix"), null, null, null);
            h.Patch(m("HighlightCurrentValue", Type.EmptyTypes), hm("HighlightPrefix"), null, null, null);
            h.Patch(m("ClearHighlightButton", Type.EmptyTypes), hm("ClearHighlightPrefix"), null, null, null);
            MethodInfo scroll = typeof(ScrollRect).GetProperty("verticalNormalizedPosition").GetSetMethod();
            h.Patch(scroll, hm("ScrollPrefix"), null, null, null);
            popupsReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: deferred dropdown buttons off: " + e.Message);
        }
    }

    static bool PopupsOn() { return popupsReady && !testPopupsOff && deferPopups.Value; }

    // What VaM's CreatePanelButtons leaves on a closed dropdown, without the button objects: new arrays (empty
    // slots), the current button index, the list's height (button heights from the button prefab: a RectTransform
    // made without a parent is its sizeDelta, and with anchors at the same height it stays so under the list), then
    // its closing HighlightCurrentValue: the highlighted buttons (kept) and the scroll position.
    static bool Defer(UIPopup p)
    {
        if (!PopupsOn()) { notOff++; return false; }
        if (p.alwaysOpen) { notAlwaysOpen++; return false; }
        if ((bool)pVisible.GetValue(p)) { notOpen++; return false; }
        RectTransform panel = p.popupPanel;
        if (panel == null) { notNoPanel++; return false; }
        if (panel.gameObject.activeSelf) { notPanelActive++; return false; }         // only a closed dropdown
        string[] values = (string[])pPopupValues.GetValue(p);
        RectTransform prefab = p.popupButtonPrefab;
        if (values == null || prefab == null) { notNoValues++; return false; }    // VaM's makes nothing then
        if (prefab.anchorMin.y != prefab.anchorMax.y) { popupNotDeferredAnchors++; return false; }
        int n = (int)pFiltered.GetValue(p);
        if (n < 0) { notOther++; return false; }
        bool filtering = p.useFiltering;
        Dictionary<int, int> toValue = (Dictionary<int, int>)pButtonToValue.GetValue(p);
        if (filtering && toValue == null) { notOther++; return false; }
        string cur = (string)pCurrentValue.GetValue(p);
        float h = prefab.sizeDelta.y;
        float v1 = 2f * p.topBottomBuffer;
        float v2 = v1;
        int cbi = -1;
        bool[] made = new bool[n], selected = new bool[n];
        try {
            for (int i = 0; i < n; i++) {
                int v = i;
                if (filtering && !toValue.TryGetValue(i, out v)) continue;
                bool match = cur == values[v];
                if (match) cbi = i;
                made[i] = true;
                selected[i] = match;
                v1 += h;
                v2 += h;
            }
        } catch (Exception) {
            notOther++;
            return false;                                                         // VaM's own fails the same way
        }
        if (p.topButton != null) {
            RectTransform top = p.topButton.GetComponent<RectTransform>();
            if (top != null) v1 += top.rect.height;
        }
        pPopupTransforms.SetValue(p, new RectTransform[n]);
        pPopupButtons.SetValue(p, new Button[n]);
        pCurrentButtonIndex.SetValue(p, cbi);
        if (p.buttonParent == null) {
            RectTransform own = p.GetComponent<RectTransform>();
            if (own != null) { Vector2 s = own.sizeDelta; s.y = v1; own.sizeDelta = s; }
        } else {
            Vector2 s = p.buttonParent.sizeDelta; s.y = v2; p.buttonParent.sizeDelta = s;
        }
        lastReplicaScroll = float.NaN;
        ScrollRect sr = (ScrollRect)pScrollRect.GetValue(p);
        if (cbi != -1 && sr != null && sr.vertical && !sr.horizontal) {
            float height = h;
            float half = (sr.viewport.rect.height - height) / 2f;
            float pos = (float)cbi * height - half;
            float t = Mathf.Clamp01(pos / (sr.content.rect.height - sr.viewport.rect.height));
            lastReplicaScroll = 1f - t;
            sr.verticalNormalizedPosition = 1f - t;
        }
        Pending d = new Pending();
        d.made = made; d.selected = selected; d.selectColor = p.selectColor; d.normalColor = p.normalColor;
        if (unbuilt.Count > 4096 && (unbuilt.Count & 1023) == 0) Sweep();
        unbuilt[p] = d;
        return true;
    }

    static void Sweep()
    {
        List<UIPopup> gone = new List<UIPopup>();
        foreach (UIPopup p in unbuilt.Keys) if (p == null) gone.Add(p);
        foreach (UIPopup p in gone) unbuilt.Remove(p);
        popupSweeps++;
    }

    // the buttons, now: VaM's CreatePanelButtons with its closing highlight left out, the list height and current
    // button index as they are (VaM set them when it built), then the highlight colours of when it built
    static void Materialize(UIPopup p)
    {
        Pending d;
        if (materializing || !unbuilt.TryGetValue(p, out d)) return;
        unbuilt.Remove(p);
        double t0 = Now();
        int cbi = (int)pCurrentButtonIndex.GetValue(p);
        RectTransform target = p.buttonParent != null ? p.buttonParent : p.GetComponent<RectTransform>();
        Vector2 size = target != null ? target.sizeDelta : Vector2.zero;
        materializing = true;
        try { mCreate.Invoke(p, null); }
        finally { materializing = false; }
        if (target != null) target.sizeDelta = size;
        pCurrentButtonIndex.SetValue(p, cbi);
        Button[] buttons = (Button[])pPopupButtons.GetValue(p);
        if (buttons != null)
            for (int i = 0; i < buttons.Length && i < d.selected.Length; i++) {
                if (buttons[i] == null) continue;
                ColorBlock c = buttons[i].colors;
                c.normalColor = d.selected[i] ? d.selectColor : d.normalColor;
                buttons[i].colors = c;
            }
        popupsMaterialized++;
        materializeMs += Now() - t0;
    }

    public static bool CreatePrefix(UIPopup __instance, out PopupCheck __state)
    {
        __state = null;
        if (materializing) return true;
        double t0 = Now();
        popupCalls++;
        // the first 20 and then every n-th (not per button prefab: every dropdown has its own button template)
        bool check = popupVerifyEvery > 0 && (popupCalls <= 20 || popupCalls % popupVerifyEvery == 0);
        bool deferred = Defer(__instance);
        popupMs += Now() - t0;
        if (!deferred) return true;
        if (!check) { popupsDeferred++; return false; }
        // check: VaM's own runs on top; its results are compared in the postfix
        Pending d = unbuilt[__instance];
        unbuilt.Remove(__instance);
        RectTransform target = __instance.buttonParent != null ? __instance.buttonParent : __instance.GetComponent<RectTransform>();
        __state = new PopupCheck();
        __state.d = d; __state.size = target != null ? target.sizeDelta.y : float.NaN; __state.scroll = lastReplicaScroll; __state.cbi = (int)pCurrentButtonIndex.GetValue(__instance);
        scrollSeen = float.NaN;
        watchingScroll = true;
        return true;
    }

    public static void CreatePostfix(UIPopup __instance, PopupCheck __state)
    {
        if (__state == null) return;
        watchingScroll = false;
        popupVerified++;
        StringBuilder bad = new StringBuilder();
        RectTransform target = __instance.buttonParent != null ? __instance.buttonParent : __instance.GetComponent<RectTransform>();
        float size = target != null ? target.sizeDelta.y : float.NaN;
        if (Bits(size) != Bits(__state.size)) bad.Append(" height ").Append(size.ToString("R")).Append(" vs ").Append(__state.size.ToString("R"));
        if (Bits(scrollSeen) != Bits(__state.scroll) && !(float.IsNaN(scrollSeen) && float.IsNaN(__state.scroll)))
            bad.Append(" scroll ").Append(scrollSeen.ToString("R")).Append(" vs ").Append(__state.scroll.ToString("R"));
        int cbi = (int)pCurrentButtonIndex.GetValue(__instance);
        if (cbi != __state.cbi) bad.Append(" button ").Append(cbi).Append(" vs ").Append(__state.cbi);
        Button[] buttons = (Button[])pPopupButtons.GetValue(__instance);
        int n = buttons != null ? buttons.Length : -1, wrongMade = 0, wrongColour = 0;
        if (n != __state.d.made.Length) bad.Append(" buttons ").Append(n).Append(" vs ").Append(__state.d.made.Length);
        else
            for (int i = 0; i < n; i++) {
                if ((buttons[i] != null) != __state.d.made[i]) { wrongMade++; continue; }
                if (buttons[i] == null) continue;
                Color want = __state.d.selected[i] ? __state.d.selectColor : __state.d.normalColor;
                if (buttons[i].colors.normalColor != want) wrongColour++;
            }
        if (wrongMade > 0) bad.Append(" made ").Append(wrongMade);
        if (wrongColour > 0) bad.Append(" colours ").Append(wrongColour);
        if (bad.Length > 0) {
            popupMismatch++;
            if (popupMismatchNote.Length < 1500) popupMismatchNote += " | " + __instance.name + ":" + bad;
        }
    }

    public static void ScrollPrefix(float value) { if (watchingScroll) scrollSeen = value; }
    public static bool TextPrefix(UIPopup __instance)
    {
        if (unbuilt.Count == 0 || !unbuilt.ContainsKey(__instance)) return true;
        popupTextsSkipped++;
        return false;                                       // set when the buttons are made, from the choices then
    }
    public static void DropPrefix(UIPopup __instance) { if (unbuilt.Count > 0 && unbuilt.Remove(__instance)) popupsDropped++; }   // VaM clears the buttons next
    public static void MaterializePrefix(UIPopup __instance) { if (unbuilt.Count > 0) Materialize(__instance); }
    public static bool HighlightPrefix(UIPopup __instance)
    {
        if (materializing) return false;                    // the highlight of when VaM built them is applied after
        if (unbuilt.Count > 0) Materialize(__instance);
        return true;
    }
    public static void ClearHighlightPrefix(UIPopup __instance)
    {
        if (unbuilt.Count > 0 && unbuilt.ContainsKey(__instance) && (int)pHighlightIndex.GetValue(__instance) != -1) Materialize(__instance);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Selectable.allSelectables

    static ConfigEntry<bool> fastSelectable;
    static bool selectableReady, selectableOn = true, testSelectableOff;
    static FieldInfo fListItems;
    static int selectableVerifyEvery, selectableSites;
    static long selectableCalls, selectableVerified, selectableMismatch;
    static double selectableMs;
    static string selectableNote = "";

    void SetUpSelectable(Harmony h)
    {
        fastSelectable = Config.Bind("LoadPaths", "FastSelectableRemove", true, "Selectable.OnDisable takes itself out of Unity UI's list of selectables by reference (List.Remove compares every element with Object.Equals, which is reference equality for these types)");
        try {
            fListItems = Need(typeof(List<Selectable>).GetField("_items", BindingFlags.Instance | BindingFlags.NonPublic), "List._items");
            if (fListItems.FieldType != typeof(Selectable[])) throw new MissingFieldException("List._items is " + fListItems.FieldType);
            foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies()) CheckEquals(a);
            AppDomain.CurrentDomain.AssemblyLoad += (s, e) => CheckEquals(e.LoadedAssembly);
            MethodInfo off = typeof(Selectable).GetMethod("OnDisable", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public, null, Type.EmptyTypes, null);
            if (off == null) throw new MissingMethodException("Selectable.OnDisable");
            h.Patch(off, null, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("SelectableTranspiler", BindingFlags.Static | BindingFlags.Public)), null);
            if (selectableSites != 1) throw new Exception("Selectable.OnDisable has " + selectableSites + " List.Remove calls");
            selectableReady = true;
        } catch (Exception e) {
            Logger.LogError("VaM scene load: fast Selectable removal off: " + e.Message);
        }
    }

    static void CheckEquals(Assembly a)
    {
        if (!selectableOn) return;
        Type[] types;
        try { types = a.GetTypes(); }
        catch (ReflectionTypeLoadException e) { types = e.Types; }
        catch (Exception) { return; }
        foreach (Type t in types) {
            if (t == null || !t.IsSubclassOf(typeof(Selectable))) continue;
            MethodInfo eq = t.GetMethod("Equals", BindingFlags.Instance | BindingFlags.Public, null, new[] { typeof(object) }, null);
            if (eq != null && eq.DeclaringType != typeof(UnityEngine.Object)) {
                selectableOn = false;
                selectableNote = t.FullName + " overrides Equals";
                if (log != null) log.LogWarning("VaM scene load: fast Selectable removal off for this run: " + selectableNote);
                return;
            }
        }
    }

    public static IEnumerable<CodeInstruction> SelectableTranspiler(IEnumerable<CodeInstruction> instructions)
    {
        MethodInfo remove = typeof(List<Selectable>).GetMethod("Remove", new[] { typeof(Selectable) });
        MethodInfo ours = typeof(VaMSceneLoad).GetMethod("SelectableRemove", BindingFlags.Static | BindingFlags.Public);
        List<CodeInstruction> code = new List<CodeInstruction>(instructions);
        int n = 0;
        foreach (CodeInstruction c in code)
            if ((c.opcode == OpCodes.Callvirt || c.opcode == OpCodes.Call) && Equals(c.operand, remove)) { c.opcode = OpCodes.Call; c.operand = ours; n++; }
        selectableSites = n;
        return code;
    }

    // List<Selectable>.Remove: the first element equal to item (Object.Equals: the same object), taken out by RemoveAt
    public static bool SelectableRemove(List<Selectable> list, Selectable item)
    {
        if (!selectableReady || !selectableOn || testSelectableOff || !fastSelectable.Value) return list.Remove(item);
        double t0 = Now();
        Selectable[] items = (Selectable[])fListItems.GetValue(list);
        int size = list.Count, found = -1;
        for (int i = 0; i < size; i++) if ((object)items[i] == (object)item) { found = i; break; }
        selectableCalls++;
        if (selectableVerifyEvery > 0 && selectableCalls % selectableVerifyEvery == 0) {
            selectableVerified++;
            if (list.IndexOf(item) != found) selectableMismatch++;
        }
        if (found >= 0) list.RemoveAt(found);
        selectableMs += Now() - t0;
        return found >= 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // morph metadata

    const string MetaFormat = "vam-morphmeta 1";
    static ConfigEntry<bool> metaCache;
    static bool metaReady, testMetaOff, metaDirty;
    static string metaFile;
    static Thread metaLoader, metaSaver;
    static Dictionary<string, Meta> metaLoaded;
    static Dictionary<string, Meta> metas = new Dictionary<string, Meta>();
    static readonly Dictionary<VarPackage, string> packageIds = new Dictionary<VarPackage, string>(RefEqOf<VarPackage>.Instance);
    [ThreadStatic] static string metaKey;
    static int metaVerifyEvery;
    static long metaHits, metaMisses, metaStored, metaVerified, metaMismatch, metaHitCalls, metaFileEntries, metaSaves;
    static double metaHitMs, metaMissMs, metaLoadMs;
    static float lastMetaStore;
    static string metaNote = "";

    sealed class Meta
    {
        public string morphName, displayName, overrideName, group, region, overrideRegion;
        public float min, max;
        public int numDeltas;
        public bool isPoseControl, formulasSet;
        public int[] fType;
        public string[] fTarget;
        public float[] fMult;
    }
    public sealed class MetaState { public string key; public DAZMorphFormula[] before; public object check; public double t0; }

    void SetUpMorphMeta(Harmony h)
    {
        metaCache = Config.Bind("LoadPaths", "MorphMetaCache", true, "Keep what VaM reads out of each morph's metadata file (.vmi) and set it straight on the next import of the same, unchanged file (BepInEx\\cache\\VaMSceneLoad\\morphmeta.bin)");
        metaFile = Path.Combine(Path.Combine(BepInEx.Paths.CachePath, "VaMSceneLoad"), "morphmeta.bin");
        BindingFlags inst = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
        try {
            MethodInfo import = typeof(DAZMorphBank).GetMethod("RuntimeImportMorph", inst, null, new[] { typeof(FileEntry), typeof(bool), typeof(bool), typeof(bool) }, null);
            MethodInfo load = typeof(DAZMorph).GetMethod("LoadMetaFromJSON", inst, null, new[] { typeof(JSONNode) }, null);
            if (import == null || load == null) throw new MissingMethodException("DAZMorphBank.RuntimeImportMorph / DAZMorph.LoadMetaFromJSON");
            BindingFlags st = BindingFlags.Static | BindingFlags.Public;
            h.Patch(import, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ImportPrefix", st)), null, null, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("ImportFinalizer", st)));
            h.Patch(load, new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("MetaPrefix", st)), new HarmonyMethod(typeof(VaMSceneLoad).GetMethod("MetaPostfix", st)), null, null);
            metaReady = true;
            if (metaCache.Value && File.Exists(metaFile)) {
                metaLoader = new Thread(ReadMetaFile);
                metaLoader.IsBackground = true;
                metaLoader.Start();
            }
        } catch (Exception e) {
            Logger.LogError("VaM scene load: morph metadata cache off: " + e.Message);
        }
    }

    static bool MetaOn() { return metaReady && !testMetaOff && metaCache.Value && System.Threading.Thread.CurrentThread.ManagedThreadId == mainThread; }

    static string MetaKeyOf(FileEntry fe)
    {
        if (fe == null) return null;
        VarFileEntry ve = fe as VarFileEntry;
        if (ve != null) {
            VarPackage p = ve.Package;
            if (p == null || ve.InternalSlashPath == null) return null;
            string id;
            if (!packageIds.TryGetValue(p, out id)) {
                id = "var|" + p.Path + "|" + p.Size + "|" + p.LastWriteTime.Ticks + "|";
                packageIds[p] = id;
            }
            return id + ve.InternalSlashPath;
        }
        return fe.Path == null ? null : "file|" + fe.Path + "|" + fe.Size + "|" + fe.LastWriteTime.Ticks;
    }

    static void EnsureMetaLoaded()
    {
        if (metaLoader == null) return;
        double t0 = Now();
        metaLoader.Join();
        metaLoader = null;
        if (metaLoaded != null) {
            foreach (KeyValuePair<string, Meta> kv in metaLoaded) if (!metas.ContainsKey(kv.Key)) metas[kv.Key] = kv.Value;
            metaFileEntries = metaLoaded.Count;
            metaLoaded = null;
        }
        metaLoadMs += Now() - t0;
    }

    public static void ImportPrefix(FileEntry morphInfFile) { metaKey = MetaOn() ? MetaKeyOf(morphInfFile) : null; }
    public static Exception ImportFinalizer(Exception __exception) { metaKey = null; return __exception; }

    public static bool MetaPrefix(DAZMorph __instance, out MetaState __state)
    {
        __state = null;
        string key = metaKey;
        metaKey = null;                                     // the import's own LoadMetaFromJSON call, only that one
        if (key == null || !MetaOn()) return true;
        double t0 = Now();
        EnsureMetaLoaded();
        Meta m;
        if (metas.TryGetValue(key, out m)) {
            metaHitCalls++;
            if (metaVerifyEvery > 0 && metaHitCalls % metaVerifyEvery == 0) {
                __state = new MetaState();
                __state.key = key; __state.before = __instance.formulas; __state.check = m; __state.t0 = t0;
                return true;                                // VaM's own runs; compared in the postfix
            }
            Apply(m, __instance);
            metaHits++;
            metaHitMs += Now() - t0;
            return false;
        }
        __state = new MetaState();
        __state.key = key; __state.before = __instance.formulas; __state.t0 = t0;
        return true;
    }

    public static void MetaPostfix(DAZMorph __instance, MetaState __state)
    {
        if (__state == null) return;
        Meta got = Capture(__instance, __state.before);
        if (__state.check != null) {
            metaVerified++;
            if (got == null || !SameMeta(got, (Meta)__state.check)) { metaMismatch++; if (metaNote.Length < 800) metaNote += " | " + __state.key; }
            return;
        }
        metaMisses++;
        if (got != null && metas.Count < 500000) { metas[__state.key] = got; metaStored++; metaDirty = true; lastMetaStore = Time.realtimeSinceStartup; }
        metaMissMs += Now() - __state.t0;
    }

    // after VaM's LoadMetaFromJSON on a new DAZMorph: every field it sets (one it did not set keeps the new object's
    // value, the same on every new DAZMorph); null when the formulas are not what VaM's makes (a new array of new
    // formulas), then not kept
    static Meta Capture(DAZMorph d, DAZMorphFormula[] before)
    {
        Meta m = new Meta();
        m.morphName = d.morphName; m.displayName = d.displayName; m.overrideName = d.overrideName; m.group = d.group; m.region = d.region; m.overrideRegion = d.overrideRegion;
        m.min = d.min; m.max = d.max; m.numDeltas = d.numDeltas; m.isPoseControl = d.isPoseControl;
        DAZMorphFormula[] f = d.formulas;
        m.formulasSet = !ReferenceEquals(f, before);
        if (m.formulasSet) {
            if (f == null) return null;
            m.fType = new int[f.Length]; m.fTarget = new string[f.Length]; m.fMult = new float[f.Length];
            for (int i = 0; i < f.Length; i++) {
                if (f[i] == null) return null;
                m.fType[i] = (int)f[i].targetType; m.fTarget[i] = f[i].target; m.fMult[i] = f[i].multiplier;
            }
        }
        return m;
    }

    static void Apply(Meta m, DAZMorph d)
    {
        d.morphName = m.morphName; d.displayName = m.displayName; d.overrideName = m.overrideName; d.group = m.group; d.region = m.region; d.overrideRegion = m.overrideRegion;
        d.min = m.min; d.max = m.max; d.numDeltas = m.numDeltas; d.isPoseControl = m.isPoseControl;
        if (m.formulasSet) {
            DAZMorphFormula[] f = new DAZMorphFormula[m.fType.Length];
            for (int i = 0; i < f.Length; i++) {
                f[i] = new DAZMorphFormula();
                f[i].targetType = (DAZMorphFormulaTargetType)m.fType[i]; f[i].target = m.fTarget[i]; f[i].multiplier = m.fMult[i];
            }
            d.formulas = f;
        }
    }

    static bool SameMeta(Meta a, Meta b)
    {
        if (a.morphName != b.morphName || a.displayName != b.displayName || a.overrideName != b.overrideName || a.group != b.group || a.region != b.region || a.overrideRegion != b.overrideRegion) return false;
        if (Bits(a.min) != Bits(b.min) || Bits(a.max) != Bits(b.max) || a.numDeltas != b.numDeltas || a.isPoseControl != b.isPoseControl || a.formulasSet != b.formulasSet) return false;
        if ((a.fType == null) != (b.fType == null)) return false;
        if (a.fType == null) return true;
        if (a.fType.Length != b.fType.Length) return false;
        for (int i = 0; i < a.fType.Length; i++)
            if (a.fType[i] != b.fType[i] || a.fTarget[i] != b.fTarget[i] || Bits(a.fMult[i]) != Bits(b.fMult[i])) return false;
        return true;
    }

    static void WriteStr(BinaryWriter w, string s) { w.Write(s != null); if (s != null) w.Write(s); }
    static string ReadStr(BinaryReader r) { return r.ReadBoolean() ? r.ReadString() : null; }

    static void ReadMetaFile()
    {
        try {
            Dictionary<string, Meta> d = new Dictionary<string, Meta>();
            using (BinaryReader r = new BinaryReader(new BufferedStream(File.OpenRead(metaFile), 1 << 20), Encoding.UTF8)) {
                if (r.ReadString() != MetaFormat) return;
                int n = r.ReadInt32();
                for (int i = 0; i < n; i++) {
                    string key = r.ReadString();
                    Meta m = new Meta();
                    m.morphName = ReadStr(r); m.displayName = ReadStr(r); m.overrideName = ReadStr(r); m.group = ReadStr(r); m.region = ReadStr(r); m.overrideRegion = ReadStr(r);
                    m.min = r.ReadSingle(); m.max = r.ReadSingle(); m.numDeltas = r.ReadInt32(); m.isPoseControl = r.ReadBoolean(); m.formulasSet = r.ReadBoolean();
                    int k = r.ReadInt32();
                    if (k >= 0) {
                        m.fType = new int[k]; m.fTarget = new string[k]; m.fMult = new float[k];
                        for (int j = 0; j < k; j++) { m.fType[j] = r.ReadInt32(); m.fTarget[j] = ReadStr(r); m.fMult[j] = r.ReadSingle(); }
                    }
                    d[key] = m;
                }
            }
            metaLoaded = d;
        } catch (Exception e) {
            metaNote += " read " + e.GetType().Name + ": " + e.Message;
        }
    }

    static void WriteMetaFile(object state)
    {
        KeyValuePair<string, Meta>[] all = (KeyValuePair<string, Meta>[])state;
        string tmp = metaFile + ".tmp";
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(metaFile));
            using (BinaryWriter w = new BinaryWriter(new BufferedStream(File.Create(tmp), 1 << 20), Encoding.UTF8)) {
                w.Write(MetaFormat);
                w.Write(all.Length);
                foreach (KeyValuePair<string, Meta> kv in all) {
                    Meta m = kv.Value;
                    w.Write(kv.Key);
                    WriteStr(w, m.morphName); WriteStr(w, m.displayName); WriteStr(w, m.overrideName); WriteStr(w, m.group); WriteStr(w, m.region); WriteStr(w, m.overrideRegion);
                    w.Write(m.min); w.Write(m.max); w.Write(m.numDeltas); w.Write(m.isPoseControl); w.Write(m.formulasSet);
                    int k = m.fType != null ? m.fType.Length : -1;
                    w.Write(k);
                    for (int j = 0; j < k; j++) { w.Write(m.fType[j]); WriteStr(w, m.fTarget[j]); w.Write(m.fMult[j]); }
                }
            }
            if (File.Exists(metaFile)) File.Delete(metaFile);
            File.Move(tmp, metaFile);
            metaSaves++;
        } catch (Exception e) {
            metaNote += " write " + e.GetType().Name + ": " + e.Message;
        }
    }

    void Update()
    {
        BundleUpdate();
        try { SwitchUpdate(); } catch (Exception e) { log.LogError("VaM scene load: " + e); }
        // morph metadata learnt this run: written a few seconds after the last new entry, off the main thread
        if (!metaDirty || Time.realtimeSinceStartup - lastMetaStore < 5f || (metaSaver != null && metaSaver.IsAlive)) return;
        EnsureMetaLoaded();
        metaDirty = false;
        KeyValuePair<string, Meta>[] all = new KeyValuePair<string, Meta>[metas.Count];
        int i = 0;
        foreach (KeyValuePair<string, Meta> kv in metas) all[i++] = kv;
        metaSaver = new Thread(WriteMetaFile);
        metaSaver.IsBackground = true;
        metaSaver.Start(all);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // test interface (this run only)

    public static string SetLoadFramesTestOff(bool atomsOff, bool popupsOff, bool selectableOff, bool metaOff)
    {
        testAtomsOff = atomsOff; testPopupsOff = popupsOff; testSelectableOff = selectableOff; testMetaOff = metaOff;
        return LoadFramesStats();
    }

    // sampled checks while loading: every n-th deferred dropdown (and the first of each button prefab) also built by
    // VaM's own and compared; every n-th Selectable removal also searched by List.IndexOf; every n-th cached morph
    // file also read by VaM's own and compared
    public static string SetLoadFramesVerify(int popupEvery, int selectableEvery, int metaEvery)
    {
        popupVerifyEvery = popupEvery; selectableVerifyEvery = selectableEvery; metaVerifyEvery = metaEvery;
        return LoadFramesStats();
    }

    public static string LoadFramesStats()
    {
        int left = 0;
        foreach (UIPopup p in unbuilt.Keys) if (p != null) left++;
        return "atoms made in the frame " + atomsFast + " (" + atomMs.ToString("F0") + " ms, " + atomFrames + " frames), VaM's own " + atomsVaM + " (over the frame budget " + atomsOverBudget + "), failed " + atomsFailed
            + (atomsReady ? "" : " [off " + atomsNote + "]")
            + " | dropdowns deferred " + popupsDeferred + " (" + popupMs.ToString("F0") + " ms), buttons made later " + popupsMaterialized + " (" + materializeMs.ToString("F0") + " ms), dropped unbuilt " + popupsDropped
            + ", still unbuilt " + left + ", texts left for later " + popupTextsSkipped
            + "; VaM's own built: off " + notOff + ", always open " + notAlwaysOpen + ", open " + notOpen + ", no panel " + notNoPanel + ", panel active " + notPanelActive
            + ", no choices or prefab " + notNoValues + ", anchors not level " + popupNotDeferredAnchors + ", other " + notOther
            + ", checked " + popupVerified + " (different " + popupMismatch + ")" + popupMismatchNote
            + " | Selectable removals " + selectableCalls + " (" + selectableMs.ToString("F0") + " ms), checked " + selectableVerified + " (different " + selectableMismatch + ")" + (selectableOn ? "" : " [off: " + selectableNote + "]")
            + " | morph metadata hits " + metaHits + " (" + metaHitMs.ToString("F0") + " ms), misses " + metaMisses + " (" + metaMissMs.ToString("F0") + " ms incl. VaM's), stored " + metaStored
            + ", checked " + metaVerified + " (different " + metaMismatch + "), entries " + metas.Count + " (from file " + metaFileEntries + ", " + metaLoadMs.ToString("F0") + " ms waiting), saves " + metaSaves + metaNote;
    }

    // After a load: every morph file of the scene's people read by VaM's own LoadMetaFromJSON (onto a new DAZMorph,
    // the JSON as RuntimeImportMorph gets it) and set from the cache, both compared field by field and timed.
    public static string VerifyMorphMeta(int max)
    {
        EnsureMetaLoaded();
        int tried = 0, same = 0, diff = 0, missing = 0, noJson = 0;
        double tVaM = 0, tCache = 0;
        StringBuilder sb = new StringBuilder();
        HashSet<string> seen = new HashSet<string>();
        foreach (DAZMorphBank bank in UnityEngine.Object.FindObjectsOfType<DAZMorphBank>()) {
            if (bank.morphs == null) continue;
            foreach (DAZMorph morph in bank.morphs) {
                if (tried >= max) break;
                string path = morph.metaLoadPath;
                if (string.IsNullOrEmpty(path)) continue;
                FileEntry fe = FileManager.GetFileEntry(path, false);
                string key = MetaKeyOf(fe);
                if (key == null || !seen.Add(key)) continue;
                Meta m;
                if (!metas.TryGetValue(key, out m)) { missing++; continue; }
                JSONNode jn = null;
                VarFileEntry ve = fe as VarFileEntry;
                if (ve != null) jn = ve.Package.GetJSONCache(ve.InternalSlashPath);
                if (jn == null) jn = JSON.Parse(FileManager.ReadAllText(fe));
                if (jn == null) { noJson++; continue; }
                tried++;
                DAZMorph a = new DAZMorph(), b = new DAZMorph();
                a.morphBank = bank; b.morphBank = bank;
                DAZMorphFormula[] before = a.formulas;
                double t0 = Now();
                a.LoadMetaFromJSON(jn);                         // no import around it: VaM's own
                double t1 = Now();
                Apply(m, b);
                double t2 = Now();
                tVaM += t1 - t0; tCache += t2 - t1;
                Meta got = Capture(a, before), set = Capture(b, before);
                if (got != null && set != null && SameMeta(got, m) && SameMeta(set, m) && SameMeta(got, set)) same++;
                else { diff++; if (diff <= 5) sb.Append(" | ").Append(key); }
            }
        }
        return "morph files " + tried + ": same " + same + ", different " + diff + ", not in the cache " + missing + ", no JSON " + noJson
            + "; VaM's LoadMetaFromJSON " + tVaM.ToString("F0") + " ms vs cache " + tCache.ToString("F0") + " ms" + sb;
    }

    // After a load: up to max dropdowns still without buttons get them now, checked against what they should show:
    // a button where VaM's would have one, its text from the choices, highlight colours, list height, current button
    // index and scroll position unchanged by the making.
    public static string CheckDeferredPopups(int max)
    {
        int tried = 0, good = 0, bad = 0;
        StringBuilder sb = new StringBuilder();
        List<UIPopup> list = new List<UIPopup>();
        foreach (UIPopup p in unbuilt.Keys) if (p != null) list.Add(p);
        foreach (UIPopup p in list) {
            if (tried >= max) break;
            Pending d;
            if (!unbuilt.TryGetValue(p, out d)) continue;
            tried++;
            RectTransform target = p.buttonParent != null ? p.buttonParent : p.GetComponent<RectTransform>();
            float size = target != null ? target.sizeDelta.y : float.NaN;
            int cbi = (int)pCurrentButtonIndex.GetValue(p);
            ScrollRect sr = (ScrollRect)pScrollRect.GetValue(p);
            Vector2 content = sr != null && sr.content != null ? sr.content.anchoredPosition : Vector2.zero;
            Materialize(p);
            StringBuilder why = new StringBuilder();
            if (target != null && Bits(target.sizeDelta.y) != Bits(size)) why.Append(" height");
            if ((int)pCurrentButtonIndex.GetValue(p) != cbi) why.Append(" index");
            if (sr != null && sr.content != null && sr.content.anchoredPosition != content) why.Append(" scroll");
            Button[] buttons = (Button[])pPopupButtons.GetValue(p);
            string[] values = (string[])pPopupValues.GetValue(p), display = (string[])pDisplayValues.GetValue(p);
            Dictionary<int, int> toValue = (Dictionary<int, int>)pButtonToValue.GetValue(p);
            if (buttons == null || buttons.Length != d.made.Length) why.Append(" count");
            else
                for (int i = 0; i < buttons.Length; i++) {
                    if ((buttons[i] != null) != d.made[i]) { why.Append(" made ").Append(i); break; }
                    if (buttons[i] == null) continue;
                    int v = i;
                    if (p.useFiltering) toValue.TryGetValue(i, out v);
                    Text[] texts = buttons[i].GetComponentsInChildren<Text>(true);
                    string want = p.useDifferentDisplayValues ? display[v] : values[v];
                    if (texts.Length > 0 && texts[0].text != want) { why.Append(" text ").Append(i); break; }
                    Color c = d.selected[i] ? d.selectColor : d.normalColor;
                    if (buttons[i].colors.normalColor != c) { why.Append(" colour ").Append(i); break; }
                }
            if (why.Length == 0) good++; else { bad++; if (bad <= 5) sb.Append(" | ").Append(p.name).Append(why); }
        }
        return "dropdowns given their buttons " + tried + ": as expected " + good + ", not " + bad + sb;
    }
}
