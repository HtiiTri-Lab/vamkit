// Test-only probe for VaM Scene Load (asset bundle preparation, same-frame hand-over). Goes into
// BepInEx\plugins only for a test run (the test script puts it there and takes it out).
// Snapshot(path): what a scene load left behind, so a load with 1.3.0 on and one with VaM's own path can be compared:
//   - atoms: count, how many on, per type;
//   - every CustomUnityAssetLoader in the scene: its atom, and each string / bool / int / UnityEngine.Object field
//     (objects by name, with the renderer and mesh-filter counts under a GameObject), one sorted line per loader;
//   - MeshVR.AssetLoader: each dictionary (path -> bundle, path -> reference count): entries, sorted keys, and for
//     int values their sum; the queue length;
//   - process memory (private, working set) and the managed heap.
// Writes the lines to path and returns the counts plus a hash of the loader lines and of the bundle keys.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using BepInEx;
using UnityEngine;

[BepInPlugin("local.vam.test.bundlesceneprobe", "Bundle scene probe (test only)", "1.0.0")]
public class BundleSceneProbe : BaseUnityPlugin
{
    const BindingFlags inst = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    const BindingFlags stat = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;

    static string Hash(List<string> lines)
    {
        byte[] h = MD5.Create().ComputeHash(Encoding.UTF8.GetBytes(string.Join("\n", lines.ToArray())));
        return BitConverter.ToString(h, 0, 6).Replace("-", "").ToLowerInvariant();
    }

    static string Value(object v)
    {
        if (v == null) return "null";
        UnityEngine.Object o = v as UnityEngine.Object;
        if (o != null || v is UnityEngine.Object) {
            if (o == null) return "destroyed";
            GameObject g = o as GameObject;
            if (g != null) return "go:" + g.name + " r" + g.GetComponentsInChildren<Renderer>(true).Length + " m" + g.GetComponentsInChildren<MeshFilter>(true).Length;
            return o.GetType().Name + ":" + o.name;
        }
        return Convert.ToString(v);
    }

    public static string Snapshot(string path)
    {
        StringBuilder sum = new StringBuilder();
        List<string> detail = new List<string>();
        SuperController sc = SuperController.singleton;
        List<Atom> atoms = sc.GetAtoms();
        int on = 0;
        SortedDictionary<string, int> types = new SortedDictionary<string, int>();
        foreach (Atom a in atoms) {
            if (a.on) on++;
            int k; types.TryGetValue(a.type, out k); types[a.type] = k + 1;
        }
        sum.Append("atoms ").Append(atoms.Count).Append(" (on ").Append(on).Append("; ");
        foreach (KeyValuePair<string, int> t in types) sum.Append(t.Key).Append(' ').Append(t.Value).Append(", ");
        sum.Length -= 2;
        sum.Append(")");

        // CustomUnityAssetLoader: every one in the scene, inactive ones too
        Type cuaT = typeof(CustomUnityAssetLoader);
        List<FieldInfo> fields = new List<FieldInfo>();
        for (Type t = cuaT; t != null && t != typeof(MonoBehaviour); t = t.BaseType)
            foreach (FieldInfo f in t.GetFields(inst | BindingFlags.DeclaredOnly))
                if (f.FieldType == typeof(string) || f.FieldType == typeof(bool) || f.FieldType == typeof(int) || typeof(UnityEngine.Object).IsAssignableFrom(f.FieldType))
                    fields.Add(f);
        List<string> cuaLines = new List<string>();
        Dictionary<string, int> objSet = new Dictionary<string, int>();
        int renderers = 0;
        foreach (UnityEngine.Object o in Resources.FindObjectsOfTypeAll(cuaT)) {
            Component c = o as Component;
            if (c == null || !c.gameObject.scene.IsValid()) continue;
            Atom at = c.GetComponentInParent<Atom>();
            StringBuilder l = new StringBuilder();
            l.Append(at != null ? at.uid : "?").Append(" | ").Append(c.name);
            foreach (FieldInfo f in fields) {
                object v = f.GetValue(c);
                string s = Value(v);
                l.Append(" | ").Append(f.Name).Append('=').Append(s);
                if (typeof(UnityEngine.Object).IsAssignableFrom(f.FieldType) && s != "null" && s != "destroyed") {
                    int k; objSet.TryGetValue(f.Name, out k); objSet[f.Name] = k + 1;
                    GameObject g = v as GameObject;
                    if (g != null) renderers += g.GetComponentsInChildren<Renderer>(true).Length;
                }
            }
            cuaLines.Add(l.ToString());
        }
        cuaLines.Sort(StringComparer.Ordinal);
        sum.Append("; CUA loaders ").Append(cuaLines.Count).Append(" (set: ");
        foreach (KeyValuePair<string, int> kv in objSet) sum.Append(kv.Key).Append(' ').Append(kv.Value).Append(", ");
        sum.Append("renderers under set GameObjects ").Append(renderers).Append("), hash ").Append(Hash(cuaLines));
        detail.Add("# CUA loaders");
        detail.AddRange(cuaLines);

        // MeshVR.AssetLoader's tables
        Type al = cuaT.Assembly.GetType("MeshVR.AssetLoader");
        object single = null;
        foreach (FieldInfo f in al.GetFields(stat)) if (f.FieldType == al) single = f.GetValue(null);
        if (single == null) sum.Append("; AssetLoader: no instance");
        else {
            foreach (FieldInfo f in al.GetFields(inst)) {
                object v = f.GetValue(single);
                IDictionary dict = v as IDictionary;
                if (dict != null) {
                    List<string> keys = new List<string>();
                    long total = 0;
                    foreach (DictionaryEntry e in dict) {
                        keys.Add(Convert.ToString(e.Key) + (e.Value is int ? " = " + e.Value : ""));
                        if (e.Value is int) total += (int)e.Value;
                    }
                    keys.Sort(StringComparer.Ordinal);
                    sum.Append("; ").Append(f.Name).Append(' ').Append(dict.Count);
                    if (total > 0) sum.Append(" (sum ").Append(total).Append(')');
                    sum.Append(" hash ").Append(Hash(keys));
                    detail.Add("# AssetLoader." + f.Name);
                    detail.AddRange(keys);
                } else if (v is ICollection) sum.Append("; ").Append(f.Name).Append(" count ").Append(((ICollection)v).Count);
            }
        }
        System.Diagnostics.Process p = System.Diagnostics.Process.GetCurrentProcess();
        sum.Append("; memory private ").Append((p.PrivateMemorySize64 / 1e9).ToString("F2")).Append(" GB, working set ")
           .Append((p.WorkingSet64 / 1e9).ToString("F2")).Append(" GB, managed ").Append((GC.GetTotalMemory(false) / 1e9).ToString("F2")).Append(" GB");
        if (!string.IsNullOrEmpty(path)) System.IO.File.WriteAllText(path, sum.ToString() + "\n" + string.Join("\n", detail.ToArray()) + "\n");
        return sum.ToString();
    }
}
