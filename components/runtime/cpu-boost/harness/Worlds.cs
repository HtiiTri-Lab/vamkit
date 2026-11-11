// Synthetic VaM data shared by the offline tests: a seeded random source, reflection helpers, bit comparisons, and
// builders for RecalculateNormals, soft-body vertex group and morph bank worlds. A builder called twice with the same
// seed builds two identical, independent worlds (one for the game's C#, one for the native path).
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.Serialization;
using UnityEngine;

internal class Worlds
{
    protected const BindingFlags F = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    protected static uint seed;

    protected static uint Next() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
    protected static float U() { return (Next() >> 8) * (1f / 16777216f); }
    protected static float S(float s) { return (U() * 2f - 1f) * s; }
    protected static int I(int n) { return (int)(Next() % (uint)n); }
    protected static Vector3 V(float s) { return new Vector3(S(s), S(s), S(s)); }

    protected static void Set(object o, string name, object v)
    {
        FieldInfo f = o.GetType().GetField(name, F);
        if (f == null) throw new MissingFieldException(o.GetType().Name, name);
        f.SetValue(o, v);
    }
    protected static object Get(object o, string name) { return o.GetType().GetField(name, F).GetValue(o); }

    protected static int Bits(float f) { return BitConverter.ToInt32(BitConverter.GetBytes(f), 0); }
    protected static int Diff(Vector3[] a, Vector3[] b)
    {
        if (a == null || b == null) return a == b ? 0 : 1;
        if (a.Length != b.Length) return a.Length + b.Length;
        int bad = 0;
        for (int i = 0; i < a.Length; i++)
            if (Bits(a[i].x) != Bits(b[i].x) || Bits(a[i].y) != Bits(b[i].y) || Bits(a[i].z) != Bits(b[i].z)) bad++;
        return bad;
    }
    protected static int Diff(float[] a, float[] b)
    {
        if (a == null || b == null) return a == b ? 0 : 1;
        int bad = 0;
        for (int i = 0; i < a.Length; i++) if (Bits(a[i]) != Bits(b[i])) bad++;
        return bad;
    }
    protected static int Diff(bool[] a, bool[] b)
    {
        if (a == null || b == null) return a == b ? 0 : 1;
        int bad = 0;
        for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) bad++;
        return bad;
    }
    protected sealed class NWorld
    {
        public int[] tris, ttu, vtu;
        public Vector3[] verts, normals, surf;
        public bool[] marker;
    }

    // kind 0: VaM-like (sorted triangle starts, unique vertices); 1: shuffled with duplicates, skipped entries and
    // stale markers; 2: triangle starts that are not multiples of 3; 3/4/5: an index the C# throws on.
    protected static NWorld BuildNormals(uint s, int nv, int nt, int kind)
    {
        seed = s;
        var w = new NWorld();
        w.verts = new Vector3[nv];
        for (int i = 0; i < nv; i++)
        {
            int k = I(20);
            w.verts[i] = k == 0 && i > 0 ? w.verts[I(i)] : k == 1 ? new Vector3(I(3), I(3), I(3)) : V(1f);
        }
        w.tris = new int[nt * 3];
        int iso = nv - 40;   // the last 40 vertices: only in triangle pairs of opposite winding (exactly zero sums)
        for (int t = 0; t < nt; t++)
        {
            int a = I(iso), b = I(iso), c = I(iso);
            if (I(30) == 0) b = a;
            w.tris[t * 3] = a; w.tris[t * 3 + 1] = b; w.tris[t * 3 + 2] = c;
        }
        for (int q = 0; q + 1 < 12 && q * 2 + 1 < nt; q++)
        {
            int a = iso + I(40), b = iso + I(40), c = iso + I(40);
            if (q == 0) a = 0;   // vertex 0 with a zero normal takes the z = 1 branch
            int t0 = (q * 37) % nt, t1 = (q * 37 + 11) % nt;
            w.tris[t0 * 3] = a; w.tris[t0 * 3 + 1] = b; w.tris[t0 * 3 + 2] = c;
            w.tris[t1 * 3] = a; w.tris[t1 * 3 + 1] = c; w.tris[t1 * 3 + 2] = b;
        }
        w.normals = new Vector3[nv]; for (int i = 0; i < nv; i++) w.normals[i] = V(2f);
        w.surf = new Vector3[nt]; for (int i = 0; i < nt; i++) w.surf[i] = V(2f);
        w.marker = new bool[nv];
        if (kind != 0) for (int i = 0; i < nv; i++) w.marker[i] = I(25) == 0;

        var ttu = new List<int>(); var used = new List<int>();
        for (int t = 0; t < nt; t++) if (kind == 0 ? I(3) != 0 : I(2) == 0) ttu.Add(t * 3);
        for (int q = 0; q < 12 && q * 37 + 11 < nt * 2; q++) { ttu.Add(((q * 37) % nt) * 3); ttu.Add(((q * 37 + 11) % nt) * 3); }
        if (kind != 0)
        {
            for (int k = 0; k < 20; k++) ttu.Add(ttu[I(ttu.Count)]);              // duplicates
            for (int k = 0; k < 5; k++) ttu.Add(nt * 3 + I(50));                  // past the end: skipped
            for (int k = ttu.Count - 1; k > 0; k--) { int j = I(k + 1); int x = ttu[k]; ttu[k] = ttu[j]; ttu[j] = x; }
        }
        else ttu.Sort();
        if (kind == 2) for (int k = 0; k < 30; k++) ttu.Add(I(nt * 3 - 3) + 1);   // starts like 3k + 1: shifted triples
        foreach (int t in ttu) if (t + 2 < nt * 3 && t >= 0) { used.Add(w.tris[t]); used.Add(w.tris[t + 1]); used.Add(w.tris[t + 2]); }
        var vtu = new List<int>();
        var seen = new bool[nv];
        foreach (int v in used) if (!seen[v]) { seen[v] = true; vtu.Add(v); }
        if (kind != 0)
        {
            for (int k = 0; k < 30; k++) vtu.Add(I(nv));                          // duplicates and untouched vertices
            for (int k = 0; k < 5; k++) vtu.Add(nv + I(9));                       // past the marker array: skipped
            for (int k = vtu.Count - 1; k > 0; k--) { int j = I(k + 1); int x = vtu[k]; vtu[k] = vtu[j]; vtu[j] = x; }
        }
        else vtu.Sort();
        if (kind == 3) ttu.Insert(ttu.Count / 2, -3);
        if (kind == 4) { w.tris[ttu[ttu.Count / 2] + 1] = nv + 2; }
        if (kind == 5) vtu.Insert(vtu.Count / 2, -1);
        w.ttu = ttu.ToArray();
        w.vtu = vtu.ToArray();
        return w;
    }

    protected static RecalculateNormals MakeRN(NWorld w)
    {
        var rn = new RecalculateNormals(w.tris, w.verts, w.normals, w.surf, false);
        Set(rn, "thisMarkerArray", w.marker);
        return rn;
    }

    protected sealed class SWorld
    {
        public DAZPhysicsMeshSoftVerticesGroup g;
        public DAZSkinV2 skin;
        public List<DAZPhysicsMeshSoftVerticesSet> sets;
        public Vector3[] verts;
        public bool useSim, useInterp, useCustom, center, autoAnchor;
        public int lookAt, type;
        public float maxDist, scale, falloff, bias, mult;
    }

    protected static float Pick(params float[] v) { return v[I(v.Length)]; }

    protected static SWorld BuildSoft(uint s, int nv, int nSets, int maxInfl, int type, bool bad)
    {
        seed = s;
        var w = new SWorld();
        w.verts = new Vector3[nv];
        for (int i = 0; i < nv; i++) w.verts[i] = V(0.2f);
        w.useSim = I(4) != 0; w.useInterp = I(3) != 0; w.useCustom = I(3) != 0;
        w.center = I(2) == 0; w.autoAnchor = I(2) == 0;
        w.lookAt = I(5); w.type = type;
        w.maxDist = Pick(0.05f, 0.1f, 0.03f + U() * 0.2f);
        w.scale = Pick(1f, 4.5f, 0.5f + U() * 20f);
        w.falloff = Pick(1f, 2f, 0.5f, 3f, 0.1f + U() * 6f);
        w.bias = Pick(0f, 0.3f, -0.4f, S(0.9f));
        w.mult = Pick(1f, 0.75f, U() * 2f);
        w.sets = new List<DAZPhysicsMeshSoftVerticesSet>();
        for (int k = 0; k < nSets; k++)
        {
            var set = new DAZPhysicsMeshSoftVerticesSet();
            set.targetVertex = I(nv); set.anchorVertex = I(nv);
            int n = I(6) == 0 ? 0 : 1 + I(maxInfl);
            set.influenceVertices = new int[n];
            int region = I(nv);
            for (int i = 0; i < n; i++) set.influenceVertices[i] = (region + I(200)) % nv;   // neighbouring sets overlap
            set.influenceVerticesDistances = new float[n];
            set.influenceVerticesWeights = new float[n];
            for (int i = 0; i < n; i++) { set.influenceVerticesDistances[i] = S(1f); set.influenceVerticesWeights[i] = S(1f); }
            set.primaryMove = I(5) == 0 ? new Vector3(0f, 0f, 1f) : V(1f).normalized;
            set.move = V(0.02f);
            set.lastPositionThreaded = V(0.2f); set.currentPosition = V(0.2f);
            set.lastKinematicPositionThreaded = V(0.2f); set.currentKinematicPosition = V(0.2f);
            set.interpFactor = Pick(0f, 1f, -0.2f, 1.3f, U());
            w.sets.Add(set);
        }
        if (nSets > 3) w.sets.Add(w.sets[1]);   // the same set twice
        if (bad)   // an influence vertex past the end of verts: the C# throws there
        {
            int k = nSets / 2;
            while (w.sets[k].influenceVertices.Length == 0) k = (k + 1) % nSets;
            w.sets[k].influenceVertices[0] = nv + 3;
        }

        w.skin = (DAZSkinV2)FormatterServices.GetUninitializedObject(typeof(DAZSkinV2));
        w.skin.postSkinVerts = new bool[nv]; w.skin.postSkinNormalVerts = new bool[nv]; w.skin.postSkinMorphs = new Vector3[nv];
        for (int i = 0; i < nv; i++)
        {
            w.skin.postSkinVerts[i] = I(3) == 0; w.skin.postSkinNormalVerts[i] = I(3) == 0; w.skin.postSkinMorphs[i] = V(0.01f);
        }
        w.g = (DAZPhysicsMeshSoftVerticesGroup)FormatterServices.GetUninitializedObject(typeof(DAZPhysicsMeshSoftVerticesGroup));
        Set(w.g, "wasInit", true); Set(w.g, "_on", true); Set(w.g, "_freeze", false);
        Set(w.g, "_useSimulation", w.useSim); Set(w.g, "_useInterpolation", w.useInterp); Set(w.g, "useCustomInterpolation", w.useCustom);
        Set(w.g, "_maxInfluenceDistance", w.maxDist);
        Set(w.g, "_lookAtOption", Enum.ToObject(typeof(DAZPhysicsMeshSoftVerticesGroup.LookAtOption), w.lookAt));
        Set(w.g, "_softVerticesSets", w.sets);
        Set(w.g, "centerBetweenTargetAndAnchor", w.center);
        Set(w.g, "_influenceType", Enum.ToObject(typeof(DAZPhysicsMeshSoftVerticesGroup.InfluenceType), w.type));
        Set(w.g, "oneoverscale", w.scale);
        Set(w.g, "_skin", w.skin);
        Set(w.g, "_autoInfluenceAnchor", w.autoAnchor);
        Set(w.g, "_falloffPower", w.falloff);
        Set(w.g, "_weightBias", w.bias);
        Set(w.g, "_weightMultiplier", w.mult);
        return w;
    }


    // ---------------------------------------------------------------------------------------------------------
    // DAZMorphBank.ApplyMorphsThreadedFast
    // ---------------------------------------------------------------------------------------------------------

    protected sealed class MWorld
    {
        public DAZMorphBank bank;
        public List<DAZMorph> morphs;
        public Vector3[] verts, visible;
    }

    // nm morphs of up to maxDeltas deltas over nv vertices: visible, pose-control and hidden morphs, disabled ones,
    // unchanged values (skipped), values going to and from 0, duplicate vertices within a morph, vertices past the end
    // (skipped by the C#), morphs overlapping each other, and value formulas that drive other morphs (a second pass).
    // bad: 1 = a null entry in one changed morph's deltas, 2 = a changed morph whose deltas array is null.
    protected static MWorld BuildMorphs(uint s, int nv, int nm, int maxDeltas, int bad)
    {
        seed = s;
        var w = new MWorld();
        w.verts = new Vector3[nv]; w.visible = new Vector3[nv];
        for (int i = 0; i < nv; i++) { w.verts[i] = V(1f); w.visible[i] = V(1f); }
        w.morphs = new List<DAZMorph>();
        var byName = new Dictionary<string, DAZMorph>();
        for (int m = 0; m < nm; m++)
        {
            var dm = (DAZMorph)FormatterServices.GetUninitializedObject(typeof(DAZMorph));
            dm.morphName = dm.displayName = "morph" + m;
            dm.min = -1.6f; dm.max = 1.6f;
            dm.visible = I(4) != 0; dm.isPoseControl = I(5) == 0; dm.disable = I(20) == 0;
            dm.isRuntime = false; dm.deltasLoaded = true; dm.isDemandLoaded = false;
            float value = I(8) == 0 ? 0f : S(1.5f);
            Set(dm, "_morphValue", value);
            dm.appliedValue = I(4) == 0 ? value : I(8) == 0 ? 0f : S(1.5f);
            dm.active = dm.appliedValue != 0f;
            Set(dm, "_isDriven", false); Set(dm, "_drivenBy", "");
            Set(dm, "_hasMCMFormulas", false); Set(dm, "_wasZeroedKeepChildValues", false);
            int n = I(6) == 0 ? 0 : 1 + I(maxDeltas);
            var d = new DAZMorphVertex[n];
            int start = I(nv), span = Math.Max(1, nv / 3);
            for (int k = 0; k < n; k++)
            {
                var mv = new DAZMorphVertex();
                if (I(60) == 0) mv.vertex = nv + I(100);
                else if (k > 0 && I(25) == 0) mv.vertex = d[I(k)].vertex;
                else mv.vertex = (start + I(span)) % nv;
                mv.delta = V(0.02f);
                d[k] = mv;
            }
            dm.deltas = d;
            dm.formulas = new DAZMorphFormula[0];
            Set(dm, "_hasMorphValueFormulas", false);
            w.morphs.Add(dm);
            byName[dm.morphName] = dm;
        }
        for (int m = 0; m < nm; m++)
        {
            if (I(7) != 0) continue;
            var f = new DAZMorphFormula();
            f.targetType = (DAZMorphFormulaTargetType)0;   // MorphValue
            f.target = "morph" + I(nm);
            f.multiplier = S(1.2f);
            w.morphs[m].formulas = new DAZMorphFormula[] { f };
            Set(w.morphs[m], "_hasMorphValueFormulas", true);
        }
        if (bad != 0)
        {
            int m = nm / 2;
            while (w.morphs[m].disable || w.morphs[m].deltas.Length < 4 || w.morphs[m].appliedValue == (float)Get(w.morphs[m], "_morphValue"))
                m = (m + 1) % nm;
            if (bad == 1) w.morphs[m].deltas[w.morphs[m].deltas.Length / 2] = null;
            else w.morphs[m].deltas = null;
        }
        w.bank = (DAZMorphBank)FormatterServices.GetUninitializedObject(typeof(DAZMorphBank));
        Set(w.bank, "wasInit", true);
        Set(w.bank, "_morphs", w.morphs);
        Set(w.bank, "_morphsByName", byName);
        Set(w.bank, "dirtyMorphs", new List<DAZMorph>());
        Set(w.bank, "enableMCMMorphs", false);
        Set(w.bank, "geometryId", "g");
        return w;
    }

    // The next frame: new values for some morphs, the same in every world built from the same seed.
    protected static void NextMorphFrame(MWorld w, uint s, int frame)
    {
        seed = s * 31u + (uint)frame * 977u + 1u;
        foreach (DAZMorph m in w.morphs) if (I(3) == 0) Set(m, "_morphValue", I(6) == 0 ? 0f : S(1.5f));
    }

    // What ApplyMorphsThreadedFast leaves besides the vertices: per morph value, applied value, active, driven state.
    protected static float[] MorphState(MWorld w)
    {
        var r = new List<float>();
        foreach (DAZMorph m in w.morphs)
        {
            r.Add((float)Get(m, "_morphValue")); r.Add(m.appliedValue); r.Add(m.active ? 1f : 0f);
            r.Add((bool)Get(m, "_isDriven") ? 1f : 0f); r.Add(((string)Get(m, "_drivenBy")).GetHashCode());
        }
        r.Add((bool)Get(w.bank, "threadedVerticesChanged") ? 1f : 0f);
        r.Add((bool)Get(w.bank, "visibleNonPoseThreadedVerticesChanged") ? 1f : 0f);
        r.Add(((List<DAZMorph>)Get(w.bank, "dirtyMorphs")).Count);
        return r.ToArray();
    }
}
