// VaM cloth shading: VaM's clothing drawn with a cloth BRDF (GGX and Charlie sheen, specular anti-aliasing), by
// replacing the clothing shader's pixel programs for the garments' draws (VaMClothShadingNative.bin,
// native\cloth_shading.cpp; the programs from tools\gen_cloth_programs.py).
//
// Which draws: every DAZSkinWrap below a DAZClothingItem is seen when it draws (Harmony postfix on
// DAZSkinWrap.DrawMeshGPU, no scene searches). A garment is one that is neither on the skin (95% of its vertices within
// 1 mm, the skin plugin's rule: makeup, wet layers, decals) nor small (its base mesh under 0.15 m on every side: eye
// overlays, rings, earrings); each garment submesh's (index buffer, first index) goes to the native module. Everything
// else keeps VaM's own program.
//
// The index buffer is read once per mesh (Mesh.GetNativeIndexBufferPtr waits for the render thread to catch up, 4-5 ms
// a frame with one garment when it ran every frame, 0.1.0), and again when the wrap's mesh is replaced or a Harmony
// postfix sees that mesh's indices set (triangles, SetTriangles, SetIndices, Clear, UploadMeshData, subMeshCount,
// indexFormat); a buffer that is not there yet is asked for again after a second.
//
// Settings (BepInEx\config\local.vam.clothshading.cfg): Mode 0 VaM's own programs, 1 cloth; the cloth look's values.
// Test API (static, for the SimXR scripts): SetMode, SetLook, SetIndexCache (off = 0.1.0's read every frame), Status.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;

[BepInPlugin("local.vam.clothshading", "VaM Cloth Shading", "0.1.1")]
public sealed class VaMClothShading : BaseUnityPlugin
{
    private const string Native = "VaMClothShadingNative";
    private const int ApiVersion = 1;
    private const int OpInstall = 1, OpFrame = 2;
    private const float SkinMetres = 0.001f, SkinShare = 0.95f, SmallMetres = 0.15f;
    private const int SkinSamples = 4096;

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PtrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn(IntPtr texture);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ParamsFn(float[] p);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void DrawsFn(IntPtr[] ibs, int[] starts, int n);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int StatsFn(int[] buf, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ErrorFn(byte[] buf, int cap);

    private static IntFn csApiVersion;
    private static PtrFn csRenderEventFunc;
    private static InitFn csInit;
    private static ParamsFn csSetParams;
    private static DrawsFn csSetDraws;
    private static StatsFn csStats;
    private static ErrorFn csLastError;

    private static VaMClothShading self;
    private static IntPtr renderEvent;
    private static string loadError;
    private static FieldInfo fMesh, fStore, fStoreVerts, fProjection, fDazMesh, fUvMesh;

    private ConfigEntry<int> mode;
    private ConfigEntry<float> sheenFrom, sheenFull, sheenGain, specularGain, antiAliasing, antiAliasingCap;

    private sealed class Wrap
    {
        public bool garment;
        public string kind;   // garment, on skin, small, not clothing
        public Mesh mesh;
        public IntPtr indexBuffer;
        public int[] starts = new int[0];
        public bool refresh;     // the mesh's indices were set since the buffer was read
        public float retryAt;    // no buffer yet: when to ask again
    }

    private readonly Dictionary<DAZSkinWrap, Wrap> wraps = new Dictionary<DAZSkinWrap, Wrap>();
    private readonly Dictionary<Mesh, Wrap> byMesh = new Dictionary<Mesh, Wrap>();
    private static bool indexCache = true;
    private static int indexReads;
    private bool drawsDirty, installed;
    private int installTries;
    private float nextInstall;
    private float[] sentParams;

    private static T Bind<T>(IntPtr module, string name) where T : class
    {
        IntPtr p = GetProcAddress(module, name);
        if (p == IntPtr.Zero) throw new Exception(Native + " has no " + name);
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    private static FieldInfo Field(Type t, string name)
    {
        for (; t != null; t = t.BaseType)
        {
            FieldInfo f = t.GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            if (f != null) return f;
        }
        throw new MissingFieldException(name);
    }

    private void Awake()
    {
        self = this;
        mode = Config.Bind("General", "Mode", 1, new ConfigDescription(
            "0 = VaM's own clothing shading; 1 = cloth: GGX highlights that turn into a soft sheen on rough fabric, with " +
            "highlight anti-aliasing", new AcceptableValueRange<int>(0, 1)));
        sheenFrom = Config.Bind("Cloth", "SheenFrom", 0.25f, new ConfigDescription(
            "GGX roughness (alpha) at which the sheen starts to replace the highlight (VaM Gloss 7 ~ 0.13, Gloss 4 ~ 0.35)",
            new AcceptableValueRange<float>(0f, 1f)));
        sheenFull = Config.Bind("Cloth", "SheenFull", 0.5f, new ConfigDescription(
            "GGX roughness (alpha) from which a fabric is all sheen", new AcceptableValueRange<float>(0f, 1f)));
        sheenGain = Config.Bind("Cloth", "SheenGain", 2f, new ConfigDescription(
            "brightness of the sheen (2 = about the light of VaM's highlight at a 45 degree view: less facing, more at grazing)", new AcceptableValueRange<float>(0f, 4f)));
        specularGain = Config.Bind("Cloth", "SpecularGain", 2f, new ConfigDescription(
            "brightness of the cloth highlights (2 = the peak of VaM's own highlight)", new AcceptableValueRange<float>(0f, 4f)));
        antiAliasing = Config.Bind("Cloth", "AntiAliasing", 0.15915494f, new ConfigDescription(
            "how much the screen-space normal variance widens the highlight (0 = off; 0.159 = Tokuyoshi & Kaplanyan's)",
            new AcceptableValueRange<float>(0f, 1f)));
        antiAliasingCap = Config.Bind("Cloth", "AntiAliasingCap", 0.18f, new ConfigDescription(
            "the most the anti-aliasing adds to alpha^2", new AcceptableValueRange<float>(0f, 1f)));
        try
        {
            // .bin, not .dll: BepInEx reads every .dll under plugins as a .NET assembly
            string dll = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(typeof(VaMClothShading).Assembly.Location), Native + ".bin");
            IntPtr m = LoadLibraryW(dll);
            if (m == IntPtr.Zero) throw new Exception("cannot load " + dll + " (error " + Marshal.GetLastWin32Error() + ")");
            csApiVersion = Bind<IntFn>(m, "CS_ApiVersion");
            csRenderEventFunc = Bind<PtrFn>(m, "CS_RenderEventFunc");
            csInit = Bind<InitFn>(m, "CS_Init");
            csSetParams = Bind<ParamsFn>(m, "CS_SetParams");
            csSetDraws = Bind<DrawsFn>(m, "CS_SetDraws");
            csStats = Bind<StatsFn>(m, "CS_Stats");
            csLastError = Bind<ErrorFn>(m, "CS_LastError");
            if (csApiVersion() != ApiVersion) throw new Exception("native API " + csApiVersion() + ", plugin expects " + ApiVersion);
            renderEvent = csRenderEventFunc();
            Type wrap = typeof(DAZSkinWrap);
            fMesh = Field(wrap, "mesh");
            fStore = Field(wrap, "wrapStore");
            fStoreVerts = Field(typeof(DAZSkinWrapStore), "wrapVertices");
            fProjection = Field(Type.GetType("DAZSkinWrapStore+SkinWrapVert, Assembly-CSharp", true), "surfaceNormalProjection");
            fDazMesh = Field(wrap, "dazMesh");
            fUvMesh = Field(typeof(DAZMesh), "_uvMappedMesh");
            if (csInit(Texture2D.whiteTexture.GetNativeTexturePtr()) == 0) throw new Exception(NativeError());
            MethodInfo draw = wrap.GetMethod("DrawMeshGPU", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null,
                new Type[] { typeof(bool) }, null);
            if (draw == null) throw new MissingMethodException("DAZSkinWrap.DrawMeshGPU(bool)");
            Harmony harmony = new Harmony("local.vam.clothshading");
            harmony.Patch(draw, null,
                new HarmonyMethod(typeof(VaMClothShading).GetMethod("WrapDrawn", BindingFlags.Public | BindingFlags.Static)), null, null);
            HarmonyMethod changed = new HarmonyMethod(typeof(VaMClothShading).GetMethod("IndicesSet", BindingFlags.Public | BindingFlags.Static));
            string[] setters = { "set_triangles", "SetTriangles", "SetIndices", "Clear", "UploadMeshData", "set_subMeshCount", "set_indexFormat" };
            foreach (MethodInfo setter in typeof(Mesh).GetMethods(BindingFlags.Public | BindingFlags.Instance))
                if (!setter.IsGenericMethodDefinition && Array.IndexOf(setters, setter.Name) >= 0 &&
                    (setter.GetMethodImplementationFlags() & MethodImplAttributes.InternalCall) == 0)
                    harmony.Patch(setter, null, changed, null, null);
            Logger.LogInfo("VaM cloth shading: loaded, mode " + mode.Value);
        }
        catch (Exception e)
        {
            loadError = e.Message;
            Logger.LogError("VaM cloth shading: " + e.Message);
        }
    }

    private static string NativeError()
    {
        byte[] buf = new byte[512];
        int n = csLastError(buf, buf.Length);
        return Encoding.ASCII.GetString(buf, 0, Math.Max(0, Math.Min(n, buf.Length - 1)));
    }

    // Harmony postfix on DAZSkinWrap.DrawMeshGPU(bool): this wrap drew (or would have) this frame
    public static void WrapDrawn(DAZSkinWrap __instance)
    {
        if (self == null || loadError != null || __instance == null) return;
        try { self.Track(__instance); }
        catch (Exception e)
        {
            loadError = e.Message;
            self.Logger.LogError("VaM cloth shading: " + e);
        }
    }

    private void Track(DAZSkinWrap w)
    {
        Wrap e;
        if (!wraps.TryGetValue(w, out e))
        {
            e = new Wrap();
            Classify(w, e);
            wraps[w] = e;
        }
        if (!e.garment) return;
        Mesh mesh = fMesh.GetValue(w) as Mesh;
        if (indexCache && ReferenceEquals(mesh, e.mesh) && !e.refresh &&
            (e.indexBuffer != IntPtr.Zero || Time.realtimeSinceStartup < e.retryAt)) return;
        IntPtr ib = mesh != null ? mesh.GetNativeIndexBufferPtr() : IntPtr.Zero;   // waits for the render thread
        indexReads++;
        e.refresh = false;
        e.retryAt = ib == IntPtr.Zero ? Time.realtimeSinceStartup + 1f : 0f;
        if (mesh == e.mesh && ib == e.indexBuffer) return;
        if (!ReferenceEquals(e.mesh, null)) byMesh.Remove(e.mesh);
        if (!ReferenceEquals(mesh, null)) byMesh[mesh] = e;
        e.mesh = mesh;
        e.indexBuffer = ib;
        e.starts = new int[mesh != null && ib != IntPtr.Zero ? mesh.subMeshCount : 0];
        for (int k = 0; k < e.starts.Length; k++) e.starts[k] = (int)mesh.GetIndexStart(k);
        drawsDirty = true;
    }

    // Harmony postfix on Mesh's index setters: a tracked garment's index buffer is read again at its next draw
    public static void IndicesSet(Mesh __instance)
    {
        Wrap e;
        if (self != null && !ReferenceEquals(__instance, null) && self.byMesh.TryGetValue(__instance, out e)) e.refresh = true;
    }

    private void Classify(DAZSkinWrap w, Wrap e)
    {
        if (w.GetComponentInParent<DAZClothingItem>() == null) { e.kind = "not clothing"; return; }
        if (OnSkin(w)) { e.kind = "on skin"; return; }
        object dm = fDazMesh.GetValue(w);
        Mesh uv = dm != null ? fUvMesh.GetValue(dm) as Mesh : null;
        if (uv != null)
        {
            Vector3 size = uv.bounds.size;
            if (size.x < SmallMetres && size.y < SmallMetres && size.z < SmallMetres) { e.kind = "small"; return; }
        }
        e.kind = "garment";
        e.garment = true;
    }

    private static bool OnSkin(DAZSkinWrap w)
    {
        object store = fStore.GetValue(w);
        Array verts = store == null ? null : fStoreVerts.GetValue(store) as Array;
        if (verts == null || verts.Length == 0) return false;
        int step = Math.Max(1, verts.Length / SkinSamples), near = 0, total = 0;
        for (int i = 0; i < verts.Length; i += step)
        {
            object v = verts.GetValue(i);
            float p = v == null ? 1f : (float)fProjection.GetValue(v);
            if (Math.Abs(p) < SkinMetres) near++;
            total++;
        }
        return total > 0 && near >= SkinShare * total;
    }

    private void Update()
    {
        if (loadError != null) return;
        if (!installed && Time.realtimeSinceStartup >= nextInstall && installTries < 10)
        {
            GL.IssuePluginEvent(renderEvent, OpInstall);
            installTries++;
            nextInstall = Time.realtimeSinceStartup + 1f;
            int[] s = new int[1];
            csStats(s, 1);
            installed = s[0] != 0;
        }
        GL.IssuePluginEvent(renderEvent, OpFrame);
        float[] p = Params();
        bool same = sentParams != null;
        for (int i = 0; same && i < p.Length; i++) same = p[i] == sentParams[i];
        if (!same)
        {
            csSetParams(p);
            sentParams = p;
        }
        List<DAZSkinWrap> gone = null;
        foreach (KeyValuePair<DAZSkinWrap, Wrap> kv in wraps)
            if (kv.Key == null) (gone ?? (gone = new List<DAZSkinWrap>())).Add(kv.Key);
        if (gone != null)
        {
            foreach (DAZSkinWrap w in gone)
            {
                Wrap e = wraps[w];
                if (!ReferenceEquals(e.mesh, null)) byMesh.Remove(e.mesh);
                wraps.Remove(w);
            }
            drawsDirty = true;
        }
        if (drawsDirty) SendDraws();
    }

    private float[] Params()
    {
        return new float[] { mode.Value, antiAliasing.Value, antiAliasingCap.Value, specularGain.Value,
            sheenFrom.Value, sheenFull.Value, sheenGain.Value, 0f };
    }

    private void SendDraws()
    {
        drawsDirty = false;
        List<IntPtr> ibs = new List<IntPtr>();
        List<int> starts = new List<int>();
        foreach (KeyValuePair<DAZSkinWrap, Wrap> kv in wraps)
        {
            if (kv.Key == null || !kv.Value.garment || kv.Value.indexBuffer == IntPtr.Zero) continue;
            foreach (int s in kv.Value.starts)
            {
                ibs.Add(kv.Value.indexBuffer);
                starts.Add(s);
            }
        }
        csSetDraws(ibs.ToArray(), starts.ToArray(), ibs.Count);
    }

    // ---- test API (SimXR scripts) -------------------------------------------------------------------------------------

    public static string SetMode(int m)
    {
        if (self == null) return "error: not loaded";
        self.mode.Value = Mathf.Clamp(m, 0, 1);
        return "mode " + self.mode.Value;
    }

    public static string SetLook(float from, float full, float sheen, float specular, float aa)
    {
        if (self == null) return "error: not loaded";
        self.sheenFrom.Value = from;
        self.sheenFull.Value = full;
        self.sheenGain.Value = sheen;
        self.specularGain.Value = specular;
        self.antiAliasing.Value = aa;
        return "look " + string.Join(" ", Array.ConvertAll(self.Params(), x => x.ToString("0.###", CultureInfo.InvariantCulture)));
    }

    // off: the index buffer is read at every garment draw, as 0.1.0 did
    public static string SetIndexCache(bool on)
    {
        indexCache = on;
        return "index cache " + on + ", index buffer reads so far " + indexReads;
    }

    public static string Status()
    {
        if (self == null) return "{\"error\":\"not loaded\"}";
        if (loadError != null) return "{\"error\":" + Q(loadError) + "}";
        int[] s = new int[9];
        csStats(s, s.Length);
        Dictionary<string, int> kinds = new Dictionary<string, int>();
        StringBuilder items = new StringBuilder();
        foreach (KeyValuePair<DAZSkinWrap, Wrap> kv in self.wraps)
        {
            if (kv.Key == null) continue;
            int n;
            kinds.TryGetValue(kv.Value.kind, out n);
            kinds[kv.Value.kind] = n + 1;
            DAZClothingItem item = kv.Key.GetComponentInParent<DAZClothingItem>();
            if (item == null) continue;
            if (items.Length > 0) items.Append(',');
            items.Append("{\"item\":").Append(Q(!string.IsNullOrEmpty(item.displayName) ? item.displayName : item.name))
                 .Append(",\"kind\":").Append(Q(kv.Value.kind)).Append(",\"submeshes\":").Append(kv.Value.starts.Length).Append('}');
        }
        StringBuilder k = new StringBuilder();
        foreach (KeyValuePair<string, int> kv in kinds) k.Append(k.Length > 0 ? "," : "").Append(Q(kv.Key)).Append(':').Append(kv.Value);
        return "{\"mode\":" + self.mode.Value + ",\"params\":[" +
               string.Join(",", Array.ConvertAll(self.Params(), x => x.ToString("0.#####", CultureInfo.InvariantCulture))) + "]" +
               ",\"installed\":" + s[0] + ",\"programs_created\":" + s[1] + ",\"cloth_programs\":" + s[2] + ",\"failed\":" + s[3] +
               ",\"replacements\":" + s[4] + ",\"registered_draws\":" + s[5] + ",\"frame_cloth_draws\":" + s[6] +
               ",\"frame_replaced\":" + s[7] + ",\"frame_unregistered\":" + s[8] + ",\"index_cache\":" + (indexCache ? "true" : "false") +
               ",\"index_reads\":" + indexReads + ",\"frame\":" + Time.frameCount + ",\"error\":" + Q(NativeError()) +
               ",\"wraps\":{" + k + "},\"items\":[" + items + "]}";
    }

    private static string Q(string s)
    {
        return "\"" + (s ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }
}
