// The frame's scene for RtCore, once a frame. Record ABI is RecordVersion below.
// The path tracer seeds all Unity mesh/skinned renderers at scene commit and captures Graphics.Internal_DrawMesh
// submissions, and gives every material an explicit physical model and guide domain.
//
// With VaM on D3D11On12 (the d3d11-proxy component's d3d11.dll) the native module traces the scene. What it needs
// comes from here at the frame's first camera PreCull after the skins have drawn (the eye camera at the latest),
// through Graphics.ExecuteCommandBuffer with IssuePluginEventAndData(OP_RT_SCENE, slot). The records go into one of
// three slots of unmanaged memory. The list below was written for the texture-space ray tracing this plugin had
// before the path tracer (not in this repository); where the draws, lights and hair come from is the same.
// - Draws: every character's skin (DAZSkinV2.DrawMeshGPU binds, by its IL: verts = delayDisplayOneFrame ? delayed :
//   _useSmoothing ? smoothed : raw, normals = delayed or _normalsBuffer; drawn at drawOffset), clothing
//   (DAZSkinWrap.DrawMeshGPU(noDelay): _delayed / _draw verts and normals) and the MeshRenderers of the scene's other
//   atoms (Atom._meshRenderers / _dynamicMeshRenderers, re-read on Atom.Init, (De)RegisterDynamicMeshRenderer and
//   SuperController.AddAtom / RemoveAtom: no scene scans). Each submesh with its material: the plugin's key, the main
//   texture (average colour) and VaM's separate alpha map (_AlphaTex), Opaque / Cutout / Translucent / Emissive by render
//   queue and shader name, _Cutoff, _Color, _MainTex_ST, _TessPhong; and the Unity mesh's UV0 stream for the alpha test
//   (offset from the mesh's channels, stride from the buffer's size, checked natively). Not occluding (kNoOcclude): the
//   eyes' cornea, reflection and tear layers, and clothing lying on the skin (decals, makeup, tattoos: 95% of the wrap's
//   vertices within 1 mm of the skin by DAZSkinWrapStore's surfaceNormalProjection, measured once per wrap).
// - Lights: VaM's (AdjustLightV2: the plugin's registry) and Display Lighting's rig lamps (its rig, re-read when DL
//   builds a new one); the shadowed ones are traced (at most 8 natively), each with what Unity puts in
//   _WorldSpaceLightPos0 and _LightColor0 for it (VaM's colour rule: GammaToLinear(colour x intensity)).
// - Hair: GPUTools HairRender's render particles (TessRenderParticles), known from a HairRender.Awake postfix (plus one
//   search for the ones made before it, as VaM's DLSS bridge does).
// - The ambient light as ShadeSH9's constants (RenderSettings, every 15 frames).
// Native buffers and textures are taken once each (GetNative*Ptr synchronises with the render thread) and cached.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

internal static partial class RtScene
{
    private const int OpRtScene = 5;
    private const int HeaderBytes = 760, DrawBytes = 592, LightBytes = 88, HairBytes = 232, RecordVersion = 23;
    private const int MaxDraws = 8192, MaxLights = 64, MaxHairs = 64, Slots = 3;
    private const int SlotBytes = HeaderBytes + MaxDraws * DrawBytes + MaxLights * LightBytes + MaxHairs * HairBytes;
    private const int CacheFrames = 300;
    private const uint DrawIndex32 = 1, DrawRigid = 2, DrawSkin = 4, DrawWrap = 8, DrawNoOcclude = 16;
    private const uint DrawShadowOnly = 512;  // version 19, PT: a ShadowsOnly renderer blocks analytic lights, never seen
    // version 21, PT: the eye camera draws this submesh with nothing covered (a first-person plugin hides it there):
    // not seen by camera rays; reflections, shadows and bounces still are (EyeRendered). Hair records: flags bit 0.
    private const uint DrawCameraHidden = 1024;
    // 0.23.4 (native RT_DRAW_EFFECT / RT_DRAW_WATCH, CPU only): an author's shader without a generated contract. Effect:
    // its own raster draws are replayed over PT (with pathFlags 32 not traced, else traced hidden from camera rays);
    // watch: the renderer reads the real D3D11 state its draws use (ST_GetPathObservedV1, MaterialFor).
    private const uint DrawEffect = 2048, DrawWatch = 4096;
    // 0.23.5, PT (rtc::kCameraOnly): seen by camera rays only, as Unity draws it (in no shadow map, occluding no ambient).
    // 0.23.6: only the "Mountain River Scroll" outline shell of one stage pack (Outline: an inverted hull; Unity culls
    // its near half, only the rim shows; traced both ways it blocks the mountain's own light). The rule is kept to
    // that one object on purpose (0.23.5 also took every cast-shadows Off renderer and any *outline* name, and
    // those stopped blocking light in every scene). CameraOnlyTest = false: 0.23.4.
    private const uint DrawCameraOnly = 8192;
    // API 43, PT: cast-shadows Off: in no shadow map (authorRules kPathAuthorCull)
    private const uint DrawNoCast = 16384;
    // API 43, PT: a scene material Unity's shadow map takes as a cutout at `shadowCutoff` on its alpha (PathShadowCasters)
    private const uint KindSolidShadow = 0x400;
    public static bool CameraOnlyTest = true;
    internal static int lastCameraOnly;
    private static readonly Dictionary<int,bool> cameraOnlyRenderers=new Dictionary<int,bool>();
    internal static uint CameraOnly(Renderer r,Mesh mesh,int id)
    {
        if(!CameraOnlyTest || !VaMSkinTransmission.PathSceneActive)return 0u;
        bool match;int key=r.GetInstanceID();
        if(!cameraOnlyRenderers.TryGetValue(key,out match)) {
            match=false;
            if(r.name=="Outline")
                for(Transform t=r.transform.parent;t!=null && !match;t=t.parent)match=t.name.StartsWith("Mountain River Scroll",StringComparison.Ordinal);
            cameraOnlyRenderers[key]=match;
        }
        if(!match)return 0u;
        lastCameraOnly++;
        if(PathNoteNew(id,11))PathNote(r.name+": Mountain River Scroll's outline shell -> seen by PT camera rays only");
        return DrawCameraOnly;
    }
    private const uint KindOpaque = 0, KindCutout = 1, KindEmissive = 2, KindTranslucent = 3, KindFineAlpha = 0x100, KindHair = 0x200;
    // version 6: the ambient the material's shader draws (rtcore.h EnvironmentShader), kind bits 12-13
    private const uint EnvUnity = 0, EnvVamSkin = 1, EnvMarmoset = 2;
    private const float DecalMetres = 0.001f, DecalShare = 0.95f;
    private const int DecalSamples = 4096, HairStride = 56;


    private static IntPtr eventAndData, slots;
    private static CommandBuffer issue;
    private static readonly Regex NoOccludeSkin = new Regex("cornea|eyereflection|tear", RegexOptions.IgnoreCase);
    private static readonly Regex SkipSkin = new Regex("hidden", RegexOptions.IgnoreCase);
    private static List<Light> vamLights;
    private static bool ready, patched;
    private static string error = "", hairError = "", lampError = "";

    // DAZSkinV2 / DAZSkinWrap fields
    private static FieldInfo sRenderSuspend, sStaticDraw, sMesh, sDelay, sSmoothing, sRaw, sSmoothed, sDelayed, sDelayedNormals, sNormals, sOffset,
                             sMaterialsEnabled, sUseSimple, sSimple, sMaterials, sDazMesh;
    private static FieldInfo sTangents,sDelayedTangents,wTangents,wDelayedTangents;
    private static PropertyInfo pMaterialNames;
    private static FieldInfo wRenderSuspend, wStaticDraw, wMesh, wSkin, wDelayed, wDraw, wDelayedNormals, wNormals, wMaterialsEnabled, wUseSimple,
                             wSimple, wMaterials, wStore, wStoreVerts, wVertProjection;
    // atoms
    private static FieldInfo aType, aMeshRenderers, aDynamicMeshRenderers;
    private static PropertyInfo cSingleton;
    private static FieldInfo cLoadingField;
    private static PropertyInfo cLoadingProperty;
    private static bool pathLoading;
    private static bool pathRegistryActive;
    private static uint pathPhysicalLayers;
    private static readonly List<Light> pathLights=new List<Light>();
    private static MethodInfo cGetAtoms;
    // GPUTools hair
    private static Type hRender, hPhysics;
    private static FieldInfo hRend, hData, hWorld, hMesh;
    private static PropertyInfo hParticles, hComputeBuffer, hSize, hCount, hTess, hIntValue;
    private static PropertyInfo hBary,hBaryFixed,hBaryBuffer,hStyleMode;
    private static int hTessFactorId, hStandWidthId;
    private static readonly List<Component> hairs = new List<Component>();
    private static readonly Dictionary<int, Component> hairPhysics = new Dictionary<int, Component>();
    private sealed class HairShape { public ComputeBuffer buffer; public int guides, patches; }
    private static readonly Dictionary<int, HairShape> hairShapes = new Dictionary<int, HairShape>();
    // Status: the last frame's hairs as passed - guides, patches, points per strand, points, shadow width (m) each
    private static readonly float[] hairInfo = new float[MaxHairs * 5];
    // Display Lighting's rig (VaMDisplayLighting.Instance.rig)
    private static Type dlType;
    private static FieldInfo dlInstance, dlRig;
    private static GameObject dlRigSeen;
    private static readonly List<Light> dlLamps = new List<Light>();
    private static int dlNextLook;
    // ambient
    private static readonly float[] sh = new float[28];
    private static readonly float[] skySh = new float[28];  // version 6: Skyshop's diffuse SH in world directions x _ExposureIBL.x, _ExposureLM.x
    // version 7: what else reads the receivers' environment visibility (rtcore.h Frame::environmentSpecular): Skyshop's
    // reflection (_ExposureIBL.y), lights Unity folds into their SH (AddLights); _ExposureIBL.w
    private static float iblSpecular = 1f, iblExposure = 1f;
    private static float iblDiffuse = 1f; // version 18: _ExposureIBL.x, the lighting cube as diffuse radiance (PT)
    private static Texture pathLightingCube, pathBackgroundCube;
    private static IntPtr pathLightingPointer, pathBackgroundPointer, skyshopBackgroundPointer;
    // Optional scene marker: the authored, baked interior probe is indirect light,
    // distinct from the infinite Skyshop lighting/background cubes.
    private static Atom pathIndoorMarker;
    private static Texture pathIndoorCube;
    private static IntPtr pathIndoorPointer;
    private static readonly List<ReflectionProbe> pathProbes=new List<ReflectionProbe>();
    private static Matrix4x4 pathWorldToSky = Matrix4x4.identity;
    private static float pathBackgroundScale;
    private static Vector3[] shDirs;
    private static float[,] shFit;  // 9 x directions: least squares onto ShadeSH9's terms
    private static int shFrame = -1000;

    private static readonly List<Component> skinCalls = new List<Component>(), wrapCalls = new List<Component>();
    private static readonly List<bool> wrapNoDelay = new List<bool>();
    private static int callsFrame = -1, issuedFrame = -1;
    private static readonly Dictionary<int, bool> seen = new Dictionary<int, bool>();

    private sealed class BufferEntry { public IntPtr pointer; public int stride, count, lastUsed; }
    private sealed class MeshEntry
    {
        public IntPtr indexBuffer, vertexBuffer, uvBuffer, tangentBuffer;
        public uint uvOffset, normalOffset = uint.MaxValue, tangentOffset;
        public uint vertexStride,uvStride;
        public bool uvHalf,tangentHalf; // 11-09: Unity's Vertex Compression (UvFor)
        public string layoutError="",layoutNoted;
        public int streams = -1;
        public bool index32, uvTried;
        public bool refresh,dynamic;
        public bool? enhancedEyes;
        public uint revision;
        public int subMeshCount, vertexCount, lastUsed;
        public uint[] start, count;
        public bool[] triangles;
        public int[] baseVertex;
        public float retryAt;
    }
    // Stable identity and a separate content revision: animated parameters update in place.
    private sealed class MatInfo
    {
        public string name="",shader="",unsupported="",notedUnsupported,notedShader;
        public int id;  // the Material's instance id
        public long key,revision;
        public uint mainVersion,alphaVersion;
        public IntPtr main, alpha;
        public IntPtr normal, surface, emission, gloss, detailNormal, detailMask;
        public Vector4 normalST, surfaceST, emissionST, glossST, detailST;
        public float specBump, diffuseBump, detailBump;
        public Vector4 materialControls;
        public Vector4 mainST;
        public Color specular, emissionColor;
        public float roughness, metallic, normalScale, ior, transmission, glossOffset, shininess, specOffset,legacyFresnel;
        public uint pathFlags;
        public uint programSet;
        public bool shaderStateKnown,pathSupported,discard,environmentOnly;
        public uint authorSolid;public float shadowCutoff;public bool noReceive; // API 43 (not the person's: Write)
        public readonly uint[] sampling=new uint[8];
        // version 18: the program's second colour layer (decal, Standard detail albedo, mirror blend mask) and how
        // it and coverage enter the closure (rtc::PathMaterial::mode); modeParam: the mirror mask's power
        public IntPtr second;
        public Vector4 secondST;
        public uint secondSampling, mode;
        public float modeParam;
        public uint kind;   // KindFineAlpha (0x100) set: a clothing alpha map in a 2048 slot (lace), else 512; KindHair (0x200): hair cards
        public float cutoff, opacity, tess;
        public float adjust;  // VaM's _AlphaAdjust (separate alpha): the coverage is saturate(the map's alpha + this)
        public Color color;
        public Vector4 st;    // the alpha's texture scale / offset: _AlphaTex_ST with a separate alpha map, else _MainTex_ST
        public uint env;      // EnvUnity / EnvVamSkin / EnvMarmoset
        public Color subsurface;  // EnvVamSkin: _SubdermisColor, linear
        public float iblFilter;   // EnvVamSkin: _IBLFilter
        public int readFrame = -1000000, lastUsed;
        public bool readPath;
        public bool watch,effect,effectOnly;  // 0.23.4: DrawWatch / DrawEffect (effectOnly: not traced)
    }
    private sealed class RigidEntry { public MeshRenderer renderer; public MeshFilter filter; public Transform transform; public Material[] materials; public int id; public bool uiRole; }
    private static readonly Dictionary<ComputeBuffer, BufferEntry> buffers = new Dictionary<ComputeBuffer, BufferEntry>();
    private static readonly Dictionary<Mesh, MeshEntry> meshes = new Dictionary<Mesh, MeshEntry>();
    private static readonly Dictionary<Mesh, bool[]> skinNoOcclude = new Dictionary<Mesh, bool[]>();
    private static readonly Dictionary<int, MatInfo> materials = new Dictionary<int, MatInfo>();
    private static readonly Dictionary<int, string> wrapShaders = new Dictionary<int, string>();  // status: clothing's shaders (material id: name, queue)
    private static readonly Dictionary<Texture, IntPtr> textures = new Dictionary<Texture, IntPtr>();
    private static readonly Dictionary<Texture, int> liveTextureFrames = new Dictionary<Texture, int>();
    private static readonly Dictionary<int, KeyValuePair<object, bool>> decals = new Dictionary<int, KeyValuePair<object, bool>>();  // wrap id: (store, lies on the skin)
    private static readonly List<RigidEntry> rigids = new List<RigidEntry>();
    private static bool rigidDirty = true;
    // History breaks only (scene loading). RR/NRD/NR reject stale history per
    // pixel from their guides; a content edit must not drop both eyes' history.
    private static long sceneRevision=1;
    private static long contentRevision; // diagnostic: discrete content edits
    // 0.22.62: nothing about one object (an unknown shader, a HUD item, a light cookie, ...) refuses the PT scene.
    // Each such object is adapted in PT on its own; the log says once what was adapted and how.
    private static readonly HashSet<string> pathNotes=new HashSet<string>();
    private static int pathNoteCount;
    private static void PathNote(string text) {
        if(!VaMSkinTransmission.PathSceneActive || !pathNotes.Add(text))return;
        pathNoteCount++;
        UnityEngine.Debug.LogWarning("PT object adapter: "+text+" (this object only; the scene stays in PT)");
    }
    // 0.23.1: the record runs every frame, and Mono's collector stops VaM for 0.8-0.9 s each time ~2 GB of garbage has
    // piled up (every ~30 s in a heavy PT scene, 19.7 MB/s, 96% of it from here). A note's text is built only the first
    // time its object and kind come by; the notes themselves are as before (once per text).
    private static readonly HashSet<long> pathNoteKeys=new HashSet<long>();
    private static string notedSkinError,notedHairError;
    private static bool PathNoteNew(int id,int kind) {
        return VaMSkinTransmission.PathSceneActive && pathNoteKeys.Add(((long)id<<8)|(byte)kind);
    }
    // Mono's BitConverter.DoubleToInt64Bits goes through GetBytes (a new byte[] at each call): the same bits, in place.
    private static unsafe long Bits(double v) { return *(long*)&v; }
    // Unity's Shader.name makes a new string at each call; a shader keeps its name.
    private static readonly Dictionary<int,string> shaderNames=new Dictionary<int,string>();
    // Unity 2018.1's Material.HasProperty is also true for a property the material only carries over from a shader it
    // had before: VaM moves person materials to its ComputeBuff programs at runtime, and the nails keep _DetailMap /
    // _DetailWeight, the cornea, tear and eye reflection _AlphaTex / _AlphaAdjust. GetTexture / GetTextureScale /
    // GetTextureOffset of such a property log "Material doesn't have a texture property" at every read (about 1100
    // lines in one scene load, then every frame). A fresh material of the same shader has exactly the shader's own
    // properties (11-04 in-game check): reads go only to
    // properties the shader declares; one the program doesn't declare it doesn't use either. Cached per shader and name.
    private static readonly Dictionary<long,bool> declared=new Dictionary<long,bool>();
    internal static bool Declared(Material m,int nameId) {
        if(m==null || !m.HasProperty(nameId))return false;
        Shader s=m.shader;
        if(s==null)return false;
        long key=((long)s.GetInstanceID()<<32)|(uint)nameId;
        bool has;
        if(!declared.TryGetValue(key,out has)) {
            Material fresh=new Material(s);
            has=fresh.HasProperty(nameId);
            UnityEngine.Object.Destroy(fresh);
            declared[key]=has;
        }
        return has;
    }
    internal static bool Declared(Material m,string name) { return m!=null && m.HasProperty(name) && Declared(m,Shader.PropertyToID(name)); }
    private static Texture Tex(Material m,string name) { return Declared(m,name)?m.GetTexture(name):null; }

    private static string ShaderName(Shader s) {
        if(s==null)return "";
        int id=s.GetInstanceID();string name;
        if(!shaderNames.TryGetValue(id,out name)){name=s.name??"";shaderNames[id]=name;}
        return name;
    }
    // Material.GetTag makes a new string at each call: read again when the material's shader or queue changes (Unity's
    // Standard GUI sets its RenderType override together with the queue).
    private struct TagRead { public int shader,queue;public string renderType; }
    private static readonly Dictionary<int,TagRead> renderTypes=new Dictionary<int,TagRead>();
    private static string RenderType(Material m) {
        Shader s=m.shader;int id=m.GetInstanceID(),sid=s!=null?s.GetInstanceID():0,queue=m.renderQueue;
        TagRead t;
        if(!renderTypes.TryGetValue(id,out t) || t.shader!=sid || t.queue!=queue) {
            t.shader=sid;t.queue=queue;t.renderType=m.GetTag("RenderType",false,"");renderTypes[id]=t;
        }
        return t.renderType;
    }
    // 0.22.65: what the eye camera itself draws. A first-person plugin (Embody's Hide Geometry, ImprovedPoV) makes the
    // possessed person's head, hair and head clothing transparent in the eye camera's OnPreRender and restores them in
    // its OnPostRender (Embody 61: the skin's head materials on a separate-alpha shader at _AlphaAdjust -1, _Color.a 0;
    // the cornea / tear / eye reflection _Color.a and _SpecColor 0; scalps and head clothing _AlphaAdjust -1; GPUTools
    // hair _StandWidth 0). This record (the eye camera's PreCull) and the mirrors (rendered in its culling) see the head,
    // the raster eye view does not. The eye camera's onPostRender, before the plugin's own, reads the person materials of
    // the frame's record again: what draws something at the record and nothing there is hidden from PT's camera rays in
    // the next record; reflections, shadows and bounces keep it, as the mirrors do. A material the author left drawing
    // nothing is that at both moments and keeps PT's own handling.
    private static readonly List<Material> eyeWatch=new List<Material>();
    private static readonly List<bool> eyeWatchBare=new List<bool>();
    private static readonly HashSet<int> eyeWatchIds=new HashSet<int>(),eyeHidden=new HashSet<int>();
    private static int eyeWatchFrame=-1,lastCameraHidden,lastCameraHiddenHairs;
    private static readonly int idAlphaAdjust=Shader.PropertyToID("_AlphaAdjust"),idAlphaTex=Shader.PropertyToID("_AlphaTex"),
        idColor=Shader.PropertyToID("_Color"),idCutoff=Shader.PropertyToID("_Cutoff"),idStandWidth=Shader.PropertyToID("_StandWidth"),
        idTessFactor=Shader.PropertyToID("_TessFactor"),idSpecColor=Shader.PropertyToID("_SpecColor");
    private static void WatchEye(Material m) {
        if(m!=null && VaMSkinTransmission.PathSceneActive && eyeWatchIds.Add(m.GetInstanceID())){eyeWatch.Add(m);eyeWatchBare.Add(DrawsNothing(m));}
    }
    // Camera.onPostRender for every camera; only the eye camera of a frame that recorded counts.
    public static void EyeRendered(Camera c) {
        if(!ready || c==null || c!=pathEyeCamera || eyeWatchFrame!=Time.frameCount)return;
        eyeWatchFrame=-1;  // once a frame
        eyeHidden.Clear();
        for(int i=0;i<eyeWatch.Count;i++) {
            Material m=eyeWatch[i];
            if(m!=null && !eyeWatchBare[i] && DrawsNothing(m))eyeHidden.Add(m.GetInstanceID());
        }
    }
    // Nothing of it drawn as VaM's programs draw it now (MaterialFor's coverage rules): GPUTools hair at strand width 0;
    // a separate-alpha program at _AlphaAdjust -1; a cutout one whose _Color alpha (plus a post-colour _AlphaAdjust) is
    // under the cutoff; a blended one at no alpha and no _SpecColor (VaM's blended programs add their highlights over
    // any alpha: the cornea, tear and eye reflection sit at _Color.a 0; Embody clears both). Opaque programs ignore alpha.
    private static bool DrawsNothing(Material m) {
        if(m.HasProperty(idStandWidth) && m.HasProperty(idTessFactor))return !(m.GetFloat(idStandWidth)>0f);
        string name=ShaderName(m.shader);
        float adjust=Declared(m,idAlphaAdjust)?m.GetFloat(idAlphaAdjust):0f;
        if(Has(name,"SeparateAlpha") || (Declared(m,idAlphaTex) && m.GetTexture(idAlphaTex)!=null))return adjust<=-0.99f;
        uint kind=KindOf(m,name)&3;
        if(kind!=KindTranslucent && kind!=KindCutout)return false;
        float top=(m.HasProperty(idColor)?m.GetColor(idColor).a:1f)+adjust;
        if(kind==KindCutout)return top<(m.HasProperty(idCutoff)?m.GetFloat(idCutoff):0.5f);
        if(top>0.001f)return false;
        if(!m.HasProperty(idSpecColor))return true;
        Color spec=m.GetColor(idSpecColor);
        return Mathf.Max(spec.r,Mathf.Max(spec.g,spec.b))<=0f;
    }
    private static int lastSweep;

    // status
    private static int lastDraws, lastSkins, lastWraps, lastRigids, lastHairs, lastLights, lastShadowed, lastNoOcclude, lastUv, rigidRebuilds, frames;
    private static readonly int[] lastKinds = new int[4];
    private static int decalCount, uvMeshes, uvMissing, multiStream;
    private static string decalSample = "";
    private static double collectSumMs;
    private static int collectSamples;
    private static long collectMaxTicks;

    public static bool Ready { get { return ready; } }

    private static FieldInfo F(Type t, string name, bool isStatic)
    {
        BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly | (isStatic ? BindingFlags.Static : BindingFlags.Instance);
        for (Type x = t; x != null; x = x.BaseType)
        {
            FieldInfo f = x.GetField(name, flags);
            if (f != null) return f;
        }
        throw new MissingFieldException(t.Name, name);
    }

    private static PropertyInfo P(Type t, string name)
    {
        PropertyInfo p = t.GetProperty(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        if (p == null) throw new MissingMemberException(t.Name, name);
        return p;
    }

    private static void PatchAll(Harmony h, Type type, string name, MethodInfo postfix)
    {
        int n = 0;
        foreach (MethodInfo m in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly))
        {
            if (m.Name != name) continue;
            h.Patch(m, null, new HarmonyMethod(postfix), null, null);
            n++;
        }
        if (n == 0) throw new MissingMethodException(type.Name, name);
    }

    // eventAndData: ST_RenderEventAndDataFunc; lights: the plugin's AdjustLightV2 registry (read every frame)
    public static void Setup(Harmony harmony, IntPtr eventAndDataFunc, List<Light> lightRegistry)
    {
        try
        {
            eventAndData = eventAndDataFunc;
            vamLights = lightRegistry;
            Type skin = Type.GetType("DAZSkinV2, Assembly-CSharp", true);
            Type wrap = Type.GetType("DAZSkinWrap, Assembly-CSharp", true);
            Type dazMesh = Type.GetType("DAZMesh, Assembly-CSharp", true);
            Type store = Type.GetType("DAZSkinWrapStore, Assembly-CSharp", true);
            Type storeVert = Type.GetType("DAZSkinWrapStore+SkinWrapVert, Assembly-CSharp", true);
            sRenderSuspend = F(skin, "_renderSuspend", false);
            sStaticDraw = F(skin, "staticDraw", true);
            sMesh = F(skin, "mesh", false);
            sDelay = F(skin, "delayDisplayOneFrame", false);
            sSmoothing = F(skin, "_useSmoothing", false);
            sRaw = F(skin, "rawVertsBuffer", false);
            sSmoothed = F(skin, "smoothedVertsBuffer", false);
            sDelayed = F(skin, "delayedVertsBuffer", false);
            sDelayedNormals = F(skin, "delayedNormalsBuffer", false);
            sNormals = F(skin, "_normalsBuffer", false);
            sTangents=F(skin,"_tangentsBuffer",false);
            sDelayedTangents=F(skin,"delayedTangentsBuffer",false);
            sOffset = F(skin, "drawOffset", false);
            sMaterialsEnabled = F(skin, "materialsEnabled", false);
            sUseSimple = F(skin, "GPUuseSimpleMaterial", false);
            sSimple = F(skin, "GPUsimpleMaterial", false);
            sMaterials = F(skin, "GPUmaterials", false);
            sDazMesh = F(skin, "dazMesh", false);
            pMaterialNames = dazMesh.GetProperty("materialNames", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            if (pMaterialNames == null) throw new MissingMemberException("DAZMesh", "materialNames");
            wRenderSuspend = F(wrap, "_renderSuspend", false);
            wStaticDraw = F(wrap, "staticDraw", true);
            wMesh = F(wrap, "mesh", false);
            wSkin = F(wrap, "skin", false);
            wDelayed = F(wrap, "_delayedVertsBuffer", false);
            wDraw = F(wrap, "_drawVerticesBuffer", false);
            wDelayedNormals = F(wrap, "_delayedNormalsBuffer", false);
            wNormals = F(wrap, "_normalsBuffer", false);
            wTangents=F(wrap,"_tangentsBuffer",false);
            wDelayedTangents=F(wrap,"_delayedTangentsBuffer",false);
            wMaterialsEnabled = F(wrap, "materialsEnabled", false);
            wUseSimple = F(wrap, "GPUuseSimpleMaterial", false);
            wSimple = F(wrap, "GPUsimpleMaterial", false);
            wMaterials = F(wrap, "GPUmaterials", false);
            wStore = F(wrap, "wrapStore", false);
            wStoreVerts = F(store, "wrapVertices", false);
            wVertProjection = F(storeVert, "surfaceNormalProjection", false);
            Type atom = Type.GetType("Atom, Assembly-CSharp", true);
            Type controller = Type.GetType("SuperController, Assembly-CSharp", true);
            aType = F(atom, "type", false);
            aMeshRenderers = F(atom, "_meshRenderers", false);
            aDynamicMeshRenderers = F(atom, "_dynamicMeshRenderers", false);
            cSingleton = controller.GetProperty("singleton", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
            cGetAtoms = controller.GetMethod("GetAtoms", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, Type.EmptyTypes, null);
            if (cSingleton == null || cGetAtoms == null) throw new MissingMemberException("SuperController.singleton / GetAtoms");
            BindingFlags any = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
            cLoadingField=controller.GetField("isLoading",any);
            cLoadingProperty=controller.GetProperty("isLoading",any);
            if(cLoadingField==null && cLoadingProperty==null)throw new MissingMemberException("SuperController.isLoading");
            MethodInfo wrapDraw = wrap.GetMethod("DrawMeshGPU", any, null, new Type[] { typeof(bool) }, null);
            if (wrapDraw == null) throw new MissingMethodException("DAZSkinWrap", "DrawMeshGPU(bool)");
            BindingFlags hook = BindingFlags.Public | BindingFlags.Static;
            harmony.Patch(wrapDraw, null, new HarmonyMethod(typeof(RtScene).GetMethod("WrapDrawn", hook)), null, null);
            MethodInfo changed = typeof(RtScene).GetMethod("AtomsChanged", hook);
            PatchAll(harmony, atom, "Init", changed);
            PatchAll(harmony, atom, "RegisterDynamicMeshRenderer", changed);
            PatchAll(harmony, atom, "DeregisterDynamicMeshRenderer", changed);
            PatchAll(harmony, controller, "AddAtom", changed);
            PatchAll(harmony, controller, "RemoveAtom", changed);
            SetupAmbientFit();
            SetupHair(harmony);
            SetupPathSkins(harmony);
            SetupMeshChanges(harmony);
            SetupRendererChanges(harmony);
            SetupSubmittedDraws(harmony);
            slots = Marshal.AllocHGlobal(Slots * SlotBytes);
            issue = new CommandBuffer();
            issue.name = "skin transmission rt scene";
            patched = true;
            ready = true;
        }
        catch (Exception e)
        {
            error = e.Message;
            ready = false;
        }
    }

    private static void SetupHair(Harmony harmony)
    {
        try
        {
            hRender = Type.GetType("GPUTools.Hair.Scripts.Runtime.Render.HairRender, Assembly-CSharp", true);
            hPhysics = Type.GetType("GPUTools.Hair.Scripts.Runtime.Physics.GPHairPhysics, Assembly-CSharp", true);
            Type facade = Type.GetType("GPUTools.Hair.Scripts.Runtime.Data.HairDataFacade, Assembly-CSharp", true);
            Type world = Type.GetType("GPUTools.Hair.Scripts.Runtime.Physics.HairPhysicsWorld, Assembly-CSharp", true);
            hRend = F(hRender, "rend", false);
            hData = F(hRender, "data", false);
            hMesh = F(hRender, "mesh", false);
            hWorld = F(hPhysics, "world", false);
            hParticles = P(facade, "TessRenderParticles");
            hBary=P(facade,"Barycentrics");hBaryFixed=P(facade,"BarycentricsFixed");hStyleMode=P(facade,"StyleMode");
            hBaryBuffer=P(hBary.PropertyType,"ComputeBuffer");
            hSize = P(facade, "Size");
            hCount = P(world, "TessRenderParticlesCount");
            hTess = P(world, "TessSegments");
            hTessFactorId = Shader.PropertyToID("_TessFactor");
            hStandWidthId = Shader.PropertyToID("_StandWidth");
            hComputeBuffer = P(hParticles.PropertyType, "ComputeBuffer");
            hIntValue = P(hCount.PropertyType, "Value");
            MethodInfo awake = hRender.GetMethod("Awake", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            if (awake == null) throw new MissingMethodException(hRender.Name, "Awake");
            harmony.Patch(awake, null, new HarmonyMethod(typeof(RtScene).GetMethod("HairAwake", BindingFlags.Public | BindingFlags.Static)), null, null);
            // the ones made before the hook, inactive ones included: the only search (prefabs in no scene left out)
            UnityEngine.Object[] found = Resources.FindObjectsOfTypeAll(hRender);
            for (int i = 0; i < found.Length; i++)
            {
                Component c = found[i] as Component;
                if (c != null && c.gameObject.scene.IsValid()) HairAwake(c);
            }
        }
        catch (Exception e)
        {
            hairError = e.Message;
            hRender = null;
        }
    }

    public static void HairAwake(Component __instance)
    {
        if (__instance != null && !hairs.Contains(__instance)) hairs.Add(__instance);
    }

    // DAZSkinV2.DrawMeshGPU (the plugin's SkinDrawn postfix calls this)
    public static void SkinDrawn(Component c)
    {
        if (!ready || c == null) return;
        int f = Time.frameCount;
        if (f != callsFrame) NewFrame(f);
        if (skinCalls.Count < MaxDraws) skinCalls.Add(c);
    }

    public static void WrapDrawn(Component __instance, bool noDelay)
    {
        if (!ready || __instance == null) return;
        int f = Time.frameCount;
        if (f != callsFrame) NewFrame(f);
        if (wrapCalls.Count < MaxDraws)
        {
            wrapCalls.Add(__instance);
            wrapNoDelay.Add(noDelay);
        }
    }

    public static void AtomsChanged() { rigidDirty = true;contentRevision++; }

    // the plugin's eye overlay items (clothing hugging an eyeball: shadow and reflection layers): not occluding
    private static readonly Dictionary<int, bool> eyeOverlays = new Dictionary<int, bool>();
    public static void EyeOverlay(Component wrap)
    {
        if (wrap != null) eyeOverlays[wrap.GetInstanceID()] = true;
    }

    // Enhanced Eyes is a stack of raster shadow/reflection shells, not a replacement eyeball.
    // PT already supplies those effects. Keep the item saved/active, but register its draws as
    // discard so neither DXR nor the raster effect replay can blacken the underlying eye.
    // Diagnostic field for a same-session comparison; no user setting is needed.
    public static bool PtEnhancedEyesCompat = true;
    internal static bool EnhancedEyesClothing(Component wrap)
    {
        // The instantiated wrap retains the .vam internal uid as its name; its parent
        // DAZClothingItem can be the generic CustomClothingItem rather than that asset.
        string internalUid = wrap != null ? wrap.name : null;
        if (!string.IsNullOrEmpty(internalUid) &&
            (internalUid.StartsWith("Hunting-Succubus:Enhanced Eyes ", StringComparison.OrdinalIgnoreCase) ||
             internalUid.StartsWith("Hunting-Succubus:Enhanced_Eyes_", StringComparison.OrdinalIgnoreCase))) return true;
        DAZClothingItem item = wrap != null ? wrap.GetComponentInParent<DAZClothingItem>() : null;
        string uid = item != null ? item.uid : null;
        if (string.IsNullOrEmpty(uid)) return false;
        return uid.StartsWith("Hunting-Succubus:Enhanced Eyes ", StringComparison.OrdinalIgnoreCase) ||
            uid.StartsWith("Hunting-Succubus:Enhanced_Eyes_", StringComparison.OrdinalIgnoreCase) ||
            uid.IndexOf("/Hunting-Succubus/Enhanced_Eyes/", StringComparison.OrdinalIgnoreCase) >= 0;
    }
    internal static bool PtReplacesEnhancedEyes(Component wrap)
    {
        return VaMSkinTransmission.PathSceneActive && PtEnhancedEyesCompat && EnhancedEyesClothing(wrap);
    }

    private static void NewFrame(int f)
    {
        callsFrame = f;
        skinCalls.Clear();
        wrapCalls.Clear();
        wrapNoDelay.Clear();
        submittedDraws.Clear();submittedOrdinals.Clear();
    }

    // Every camera's PreCull: the frame's records at the first one after the skins have drawn (the eye camera at the
    // latest). mask: the eye camera's culling mask (-1 before it is known).
    public static unsafe void PreCull(Camera camera, bool eyeCamera, int mask, bool on)
    {
        if (!ready || !on) {
            if(eyeCamera)PathCanvasUi.Frame(false);
            RestorePathScene();
            // 0.22.62: 光追 off still tells the render thread once a frame (no records): a PT frame in flight ends there
            // and the hair plugin draws its strands again. Without it PT stayed "on" with nothing recorded.
            if(ready && eyeCamera && Time.frameCount!=issuedFrame) {
                issuedFrame=Time.frameCount;issue.Clear();
                issue.IssuePluginEventAndData(eventAndData,OpRtScene<<24,IntPtr.Zero);Graphics.ExecuteCommandBuffer(issue);
            }
            return;
        }
        object controller=cSingleton.GetValue(null,null);
        bool loading=controller==null || (cLoadingField!=null?(bool)cLoadingField.GetValue(controller):(bool)cLoadingProperty.GetValue(controller,null));
        if(loading!=pathLoading) {pathLoading=loading;rigidDirty=true;sceneRevision++;}
        bool pathSelected=VaMSkinTransmission.PathSceneActive;
        if(pathSelected!=pathRegistryActive){pathRegistryActive=pathSelected;rigidDirty=true;}
        PathRasterMode(pathSelected&&!loading);
        PathSceneMode(!loading && VaMSkinTransmission.PathRenderingActive);
        int frame = Time.frameCount;
        if (eyeCamera) pathEyeCamera = camera;
        if (frame == issuedFrame) return;
        if (frame != callsFrame && !eyeCamera) return;  // no skin drawn yet this frame: a later camera
        issuedFrame = frame;
        long t0 = Stopwatch.GetTimestamp();
        PathCanvasUi.Frame(pathSelected);
        if (frame != callsFrame) NewFrame(frame);  // no skin drawn this frame
        if (pathSelected) PollObserved();
        byte* slot = (byte*)slots.ToPointer() + (long)(frame % Slots) * SlotBytes;
        byte* draws = slot + HeaderBytes;
        int n = 0;
        pathPhysicalLayers=0;
        pathSpecialMeshes.Clear();
        eyeWatch.Clear();eyeWatchBare.Clear();eyeWatchIds.Clear();eyeWatchFrame=frame;lastCameraHidden=lastCameraHiddenHairs=lastCameraOnly=0;
        if(!pathSelected)eyeHidden.Clear();
        seen.Clear();
        int skinsN = 0, wrapsN = 0;
        lastNoOcclude = lastUv = 0;
        for (int k = 0; k < 4; k++) lastKinds[k] = 0;
        for (int i = 0; i < skinCalls.Count && n < MaxDraws; i++)
        {
            int before = n;
            n = AddSkin(skinCalls[i], mask, frame, draws, n);
            if (n > before) skinsN++;
        }
        for (int i = 0; i < wrapCalls.Count && n < MaxDraws; i++)
        {
            int before = n;
            n = AddWrap(wrapCalls[i], wrapNoDelay[i], mask, frame, draws, n);
            if (n > before) wrapsN++;
        }
        if (rigidDirty && !loading) RebuildRigid();
        int rigidN = 0;
        skyCaptureCandidate=null;skyCaptureCandidateEntry=null;skyCaptureCandidateMaterial=null;skyCaptureCandidateQueue=int.MinValue;
        for (int i = 0; i < rigids.Count && n < MaxDraws; i++)
        {
            int before = n;
            n = AddRigid(rigids[i], mask, frame, draws, n);
            if (n > before) rigidN++;
        }
        n=AddPathSkins(mask,frame,draws,n);
        n=AddSubmittedDraws(mask,frame,draws,n);
        byte* lights = draws + MaxDraws * DrawBytes;
        byte* hairRecords = lights + MaxLights * LightBytes;
        // Hairs first: their layers are physical layers the lights are checked against.
        int nh = AddHairs(hairRecords, mask, frame);
        int nl = AddLights(lights, frame);
        LinkLights(draws, n, lights, nl, hairRecords, nh);
        // the header, then the three arrays packed (the native side reads them back to back)
        if (n < MaxDraws) Move(lights, draws + n * DrawBytes, nl * LightBytes);
        if (n < MaxDraws || nl < MaxLights) Move(hairRecords, draws + n * DrawBytes + nl * LightBytes, nh * HairBytes);
        UpdateAmbient(frame);
        *(int*)slot = RecordVersion;
        *(int*)(slot + 4) = n;
        *(int*)(slot + 8) = nl;
        *(int*)(slot + 12) = nh;
        float* shOut = (float*)(slot + 16);
        for (int k = 0; k < 28; k++) shOut[k] = sh[k];
        float* skyOut = (float*)(slot + 400);
        for (int k = 0; k < 28; k++) skyOut[k] = skySh[k];
        *(int*)(slot + 128) = -1;  // RtSceneHeader::viewers: no longer read (the record keeps its layout)
        *(float*)(slot + 132) = iblSpecular;
        *(float*)(slot + 136) = iblExposure;
        *(int*)(slot + 140) = 0;  // RtSceneHeader::shLights: no longer read
        *(long*)(slot + 512) = pathLightingPointer.ToInt64();
        *(long*)(slot + 520) = pathBackgroundPointer.ToInt64();
        float* skyMatrix = (float*)(slot + 528);
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 4; col++) skyMatrix[row * 4 + col] = pathWorldToSky[row, col];
        *(float*)(slot + 576) = iblSpecular;
        *(float*)(slot + 580) = pathBackgroundScale;
        *(float*)(slot + 584) = 1f;
        // 0.22.62: only a scene still loading (a transaction) waits; no object's adaptation refuses the scene.
        *(uint*)(slot + 588) = VaMSkinTransmission.PathSceneActive ? 0x80000000u|(loading?0x40000000u:0u) : 0u;
        if(!loading && VaMSkinTransmission.PathSceneActive) {
            if(n>=MaxDraws && PathNoteNew(0,1))PathNote("draw capacity "+MaxDraws+" reached: later objects are not in PT");
            if(nl>=MaxLights && PathNoteNew(0,2))PathNote("light capacity "+MaxLights+" reached: later lights are not in PT");
            if(nh>=MaxHairs && PathNoteNew(0,3))PathNote("hair capacity "+MaxHairs+" reached: later hair items are not in PT");
            if(pathSkinError.Length>0 && !ReferenceEquals(pathSkinError,notedSkinError)){notedSkinError=pathSkinError;PathNote("skinned mesh "+pathSkinError+": not in PT");}
            if(hairError.Length>0 && !ReferenceEquals(hairError,notedHairError)){notedHairError=hairError;PathNote("hair "+hairError+": not in PT");}
        }
        *(long*)(slot+592)=sceneRevision;
        *(float*)(slot+600)=iblDiffuse;*(uint*)(slot+604)=0u;
        for(int k=0;k<12;k++)((float*)(slot+608))[k]=pathBackgroundToSky[k];
        *(float*)(slot+656)=pathBackgroundColor.r;*(float*)(slot+660)=pathBackgroundColor.g;*(float*)(slot+664)=pathBackgroundColor.b;*(float*)(slot+668)=capturedSkyActive?1f:0f;
        bool indoor=pathSelected && !loading && pathIndoorMarker!=null && pathIndoorMarker.on && pathIndoorCube!=null;
        *(long*)(slot+672)=indoor?pathIndoorPointer.ToInt64():0;
        *(float*)(slot+680)=indoor?2.5f:0f;*(uint*)(slot+684)=0u;
        // 11-09: Unity's reflection probe, what Standard's indirect specular reads (unity_SpecCube0). A renderer in Unity takes
        // the probes its bounds overlap; here the scene's one that matters: the highest importance, then the one around the camera.
        ReflectionProbe probe=pathSelected && !loading?PathProbe():null;
        Bounds probeBox=probe!=null?probe.bounds:new Bounds();
        Vector4 probeDecode=probe!=null?probe.textureHDRDecodeValues:Vector4.zero;
        Vector3 probeCentre=probe!=null?probe.transform.position:Vector3.zero;
        *(long*)(slot+688)=probe!=null?probe.texture.GetNativeTexturePtr().ToInt64():0;
        *(float*)(slot+696)=probeDecode.x;*(float*)(slot+700)=probeDecode.y;*(float*)(slot+704)=probeDecode.z;*(float*)(slot+708)=probeDecode.w;
        *(float*)(slot+712)=probeBox.min.x;*(float*)(slot+716)=probeBox.min.y;*(float*)(slot+720)=probeBox.min.z;*(float*)(slot+724)=probe!=null && probe.boxProjection?1f:0f;
        *(float*)(slot+728)=probeBox.max.x;*(float*)(slot+732)=probeBox.max.y;*(float*)(slot+736)=probeBox.max.z;*(float*)(slot+740)=0f;
        *(float*)(slot+744)=probeCentre.x;*(float*)(slot+748)=probeCentre.y;*(float*)(slot+752)=probeCentre.z;*(float*)(slot+756)=0f;
        issue.Clear();
        issue.IssuePluginEventAndData(eventAndData, OpRtScene << 24, new IntPtr(slot));
        Graphics.ExecuteCommandBuffer(issue);
        Sweep(frame);
        long ticks = Stopwatch.GetTimestamp() - t0;
        lastDraws = n; lastSkins = skinsN; lastWraps = wrapsN; lastRigids = rigidN; lastLights = nl; lastHairs = nh;
        frames++;
        collectSumMs += ticks * 1000.0 / Stopwatch.Frequency;
        collectSamples++;
        if (ticks > collectMaxTicks) collectMaxTicks = ticks;
    }

    private static unsafe void Move(byte* from, byte* to, int bytes)
    {
        if (from == to || bytes <= 0) return;
        for (int i = 0; i < bytes; i++) to[i] = from[i];  // to < from: forward copy is safe
    }

    // ---- buffers, meshes, textures, materials --------------------------------------------------------------------

    private static BufferEntry BufferFor(ComputeBuffer buffer, int frame)
    {
        if (buffer == null) return null;
        BufferEntry e;
        if (buffers.TryGetValue(buffer, out e))
        {
            e.lastUsed = frame;
            return e.pointer == IntPtr.Zero ? null : e;
        }
        e = new BufferEntry();
        e.lastUsed = frame;
        try
        {
            e.stride = buffer.stride;
            e.count = buffer.count;
            e.pointer = buffer.GetNativeBufferPtr();  // synchronises with the render thread: once per buffer
        }
        catch (Exception) { e.pointer = IntPtr.Zero; }
        buffers[buffer] = e;
        return e.pointer == IntPtr.Zero ? null : e;
    }

    private static int VertexBufferCount(Mesh mesh)
    {
        try
        {
            PropertyInfo p = typeof(Mesh).GetProperty("vertexBufferCount");
            return p == null ? -1 : (int)p.GetValue(mesh, null);
        }
        catch (Exception) { return -1; }
    }

    // withVertices: also the vertex buffer (stream 0, position first; rigid meshes). The UV0 stream once per mesh:
    // Unity 2018's vertex layout is position, normal, tangent, colour (4 bytes), UV0 ... in one stream; with two streams
    // (skinning layout) UV0 is in the second after the colour. Unreadable meshes: none (alpha 1).
    private static MeshEntry MeshFor(Mesh mesh, int frame, bool withVertices)
    {
        int subMeshes = mesh.subMeshCount, vertices = mesh.vertexCount;
        MeshEntry e;
        if (meshes.TryGetValue(mesh, out e))
        {
            e.lastUsed = frame;
            if (!e.refresh && e.subMeshCount == subMeshes && e.vertexCount == vertices)
            {
                if (e.indexBuffer != IntPtr.Zero && (!withVertices || e.vertexBuffer != IntPtr.Zero)) return e;
                if (Time.realtimeSinceStartup < e.retryAt) return null;
                if (e.indexBuffer != IntPtr.Zero)
                {
                    try { e.vertexBuffer = mesh.GetNativeVertexBufferPtr(0); }
                    catch (Exception) { e.vertexBuffer = IntPtr.Zero; }
                    if (e.vertexBuffer == IntPtr.Zero) e.retryAt = Time.realtimeSinceStartup + 1f;
                    return e.vertexBuffer == IntPtr.Zero ? null : e;
                }
            }
        }
        bool dynamic=e!=null && e.dynamic;uint revision=e!=null?e.revision:0;
        e = new MeshEntry();e.dynamic=dynamic;e.revision=revision;
        e.lastUsed = frame;
        e.subMeshCount = subMeshes;
        e.vertexCount = vertices;
        e.start = new uint[subMeshes];
        e.count = new uint[subMeshes];
        e.triangles = new bool[subMeshes];
        e.baseVertex = new int[subMeshes];
        try
        {
            for (int s = 0; s < subMeshes; s++)
            {
                e.start[s] = mesh.GetIndexStart(s);
                e.count[s] = mesh.GetIndexCount(s);
                e.triangles[s] = mesh.GetTopology(s) == MeshTopology.Triangles;
                e.baseVertex[s] = (int)mesh.GetBaseVertex(s);
            }
            e.index32 = mesh.indexFormat == IndexFormat.UInt32;
            e.indexBuffer = mesh.GetNativeIndexBufferPtr();  // synchronises with the render thread: once per mesh
            if (withVertices && e.indexBuffer != IntPtr.Zero) e.vertexBuffer = mesh.GetNativeVertexBufferPtr(0);
        }
        catch (Exception) { e.indexBuffer = e.vertexBuffer = IntPtr.Zero; }
        bool ok = e.indexBuffer != IntPtr.Zero && (!withVertices || e.vertexBuffer != IntPtr.Zero);
        if (!ok) e.retryAt = Time.realtimeSinceStartup + 1f;
        meshes[mesh] = e;
        return ok ? e : null;
    }
    private static void SetupMeshChanges(Harmony harmony) {
        MethodInfo changed=typeof(RtScene).GetMethod("MeshChanged",BindingFlags.Public|BindingFlags.Static);
        string[] names={"set_vertices","set_normals","set_tangents","set_uv","set_uv2","set_colors","set_colors32","set_triangles",
            "SetVertices","SetNormals","SetTangents","SetUVs","SetColors","SetTriangles","SetIndices","Clear","UploadMeshData"};
        foreach(MethodInfo method in typeof(Mesh).GetMethods(BindingFlags.Public|BindingFlags.Instance)) {
            if(method.IsGenericMethodDefinition || method.GetMethodBody()==null || Array.IndexOf(names,method.Name)<0)continue;
            harmony.Patch(method,null,new HarmonyMethod(changed),null,null);
        }
    }
    public static void MeshChanged(Mesh __instance) {
        MeshEntry entry;
        if(__instance!=null && meshes.TryGetValue(__instance,out entry)) {
            entry.refresh=true;entry.dynamic=true;entry.revision++;
        }
    }

    private static void SetupRendererChanges(Harmony harmony) {
        MethodInfo changed=typeof(RtScene).GetMethod("RendererMaterialsChanged",BindingFlags.Public|BindingFlags.Static);
        string[] names={"set_material","set_materials","set_sharedMaterial","set_sharedMaterials"};
        foreach(MethodInfo method in typeof(Renderer).GetMethods(BindingFlags.Public|BindingFlags.Instance)) {
            if(method.GetMethodBody()!=null && Array.IndexOf(names,method.Name)>=0)
                harmony.Patch(method,null,new HarmonyMethod(changed),null,null);
        }
    }
    public static void RendererMaterialsChanged(Renderer __instance) {
        if(__instance==null)return;
        for(int i=0;i<rigids.Count;i++)if(rigids[i].renderer==__instance)rigids[i].materials=null;
        for(int i=0;i<pathSkins.Count;i++)if(pathSkins[i].renderer==__instance)pathSkins[i].materials=null;
        contentRevision++;
    }
    private static void ReadSharedMaterials(Renderer renderer,ref Material[] cache) {
        // Native animation and Renderer.material's lazy clone can bypass managed
        // setter hooks. This Unity 2018.1 build has no non-allocating shared-
        // material getter: read only registered renderers, never scan the scene.
        Material[] current=renderer.sharedMaterials;
        bool changed=cache==null || cache.Length!=current.Length;
        if(!changed)for(int i=0;i<current.Length;i++)changed|=cache[i]!=current[i];
        cache=current;
        if(changed)contentRevision++;
    }

    private static readonly MethodInfo meshHasChannel = typeof(Mesh).GetMethod("HasChannel",BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance);
    private static bool HasMeshChannel(Mesh mesh,string channel)
    {
        if(meshHasChannel==null)return false;
        object value=Enum.Parse(meshHasChannel.GetParameters()[0].ParameterType,channel);
        return (bool)meshHasChannel.Invoke(mesh,new object[]{value});
    }
    private static void UvFor(Mesh mesh, MeshEntry e)
    {
        if (e.uvTried) return;
        e.uvTried = true;
        try
        {
            // Native channel metadata remains available for GPU-only asset-bundle
            // meshes. Reading mesh.uv/normals allocated whole CPU arrays and lost
            // all texture coordinates when isReadable was false.
            bool normals=HasMeshChannel(mesh,"Normal"),tangents=HasMeshChannel(mesh,"Tangent"),colours=HasMeshChannel(mesh,"Color");
            e.streams=VertexBufferCount(mesh);
            // 11-09: a vertex is not always floats. Unity's Vertex Compression holds tangents and UVs in half floats (its default
            // mask: tangent, UV0, UV2+ in halves, UV1, the lightmap's, in floats; Hotel's Combined Meshes, 44 bytes a vertex, went
            // without UVs: every texture one texel, no labels, no posters). 2018.1 names no formats: the buffers' bytes per vertex choose.
            IntPtr firstStream=e.vertexBuffer!=IntPtr.Zero?e.vertexBuffer:mesh.GetNativeVertexBufferPtr(0);
            ulong firstBytes=VaMSkinTransmission.PathBufferBytes(firstStream),secondBytes=e.streams>=2?VaMSkinTransmission.PathBufferBytes(mesh.GetNativeVertexBufferPtr(1)):0ul;
            uint uvBytes=0,tangentBytes=0;
            for(int layout=0;layout<VertexLayouts.Length;layout++) {
                int[] l=VertexLayouts[layout];
                uint tb=tangents?(uint)l[0]:0u,ub=0u;
                for(int channel=0;channel<8;channel++)if(HasMeshChannel(mesh,"TexCoord"+channel))ub+=(uint)l[Math.Min(channel+1,3)];
                uint first=(uint)(12+(normals?12:0))+tb,second=(colours?4u:0u)+ub;
                bool fits=e.streams<=1?firstBytes==(ulong)e.vertexCount*(first+second):firstBytes==(ulong)e.vertexCount*first && secondBytes==(ulong)e.vertexCount*second;
                if(layout==0 || fits){uvBytes=ub;tangentBytes=tb;e.tangentHalf=tb==8u;e.uvHalf=l[1]==4;}
                if(fits)break;
            }
            uint vertexBytes=(uint)(12+(normals?12:0))+tangentBytes;
            e.vertexStride=vertexBytes+(e.streams<=1?(colours?4u:0u)+uvBytes:0u);
            e.uvStride=e.streams<=1?e.vertexStride:(colours?4u:0u)+uvBytes;
            IntPtr position=e.vertexBuffer!=IntPtr.Zero?e.vertexBuffer:mesh.GetNativeVertexBufferPtr(0);
            ulong actual=VaMSkinTransmission.PathBufferBytes(position);
            if(e.streams>2 || actual!=(ulong)e.vertexCount*e.vertexStride)
                e.layoutError="Unsupported vertex layout: "+mesh.name+" (bytes "+actual+", float-layout "+((ulong)e.vertexCount*e.vertexStride)+", streams "+e.streams+")";
            e.normalOffset=normals?12u:uint.MaxValue;
            if(tangents) {
                e.tangentBuffer=e.vertexBuffer!=IntPtr.Zero?e.vertexBuffer:mesh.GetNativeVertexBufferPtr(0);
                e.tangentOffset=normals?24u:12u;
            }
            if(!HasMeshChannel(mesh,"TexCoord0")){uvMissing++;return;}
            e.streams = VertexBufferCount(mesh);
            if (e.streams <= 1)
            {
                e.uvBuffer = e.vertexBuffer != IntPtr.Zero ? e.vertexBuffer : mesh.GetNativeVertexBufferPtr(0);
                e.uvOffset = vertexBytes + (colours ? 4u : 0u);
            }
            else
            {
                multiStream++;
                e.uvBuffer = mesh.GetNativeVertexBufferPtr(1);
                e.uvOffset = colours ? 4u : 0u;
            }
            if (e.uvBuffer != IntPtr.Zero) uvMeshes++;
            if(e.uvBuffer!=IntPtr.Zero && VaMSkinTransmission.PathBufferBytes(e.uvBuffer)!=(ulong)e.vertexCount*e.uvStride)
                e.layoutError="Unsupported UV layout: "+mesh.name;
        }
        catch (Exception error) { e.uvBuffer = IntPtr.Zero;uvMissing++;e.layoutError="Mesh layout unavailable: "+mesh.name+": "+error.Message; }
    }

    private static readonly int[][] VertexLayouts={new[]{16,8,8,8},new[]{8,4,8,4},new[]{8,4,4,4}}; // bytes of the tangent, UV0, UV1, UV2+: floats first
    private static readonly Dictionary<Texture,uint> textureVersions=new Dictionary<Texture,uint>();
    internal static IntPtr TexturePtr(Texture t)
    {
        if (t == null) return IntPtr.Zero;
        IntPtr p;
        if (textures.TryGetValue(t, out p)) {
            uint version;
            if(!(t is RenderTexture) && textureVersions.TryGetValue(t,out version) && version==t.updateCount)return p;
            int frame;if(liveTextureFrames.TryGetValue(t,out frame) && frame==Time.frameCount)return p;
        }
        try { p = t.GetNativeTexturePtr(); }  // synchronises with the render thread: once per texture
        catch (Exception) { p = IntPtr.Zero; }
        textures[t] = p;textureVersions[t]=t.updateCount;
        if(t is RenderTexture)liveTextureFrames[t]=Time.frameCount;
        return p;
    }

    private static bool Has(string s, string part) { return s.IndexOf(part, StringComparison.OrdinalIgnoreCase) >= 0; }

    // Blended (render queue past 2500, Transparent / Fade shaders): rays go on through 1 - alpha; alpha-tested (queue
    // 2450+, Cutout / AlphaTest / AlphaMask): blocks where alpha >= _Cutoff; unlit / self-lit: blocks, bounces its colour
    private static uint KindOf(Material m,string name)
    {
        int q = m.renderQueue;
        PathShaderStates.State state=PathShaderStates.Read(m,name);
        bool cutout=Has(name,"Cutout") || Has(name,"AlphaTest") || m.IsKeywordEnabled("_ALPHATEST_ON");
        if(state.known) {
            if(state.destination!=0)return KindTranslucent;
            if(cutout)return KindCutout;
            if(Has(name,"Unlit") || Has(name,"Emissive") || Has(name,"Self-Illumin"))return KindEmissive;
            return KindOpaque;
        }
        string renderType=RenderType(m);
        if(cutout || Has(renderType,"Cutout"))return KindCutout;
        if (q > 2500 || Has(name, "Transparent") || Has(name, "Fade")) return KindTranslucent;
        if (q >= 2450 || Has(name, "Cutout") || Has(name, "AlphaTest") || Has(name, "AlphaMask")) return KindCutout;
        if (Has(name, "Unlit") || Has(name, "Emissive") || Has(name, "Self-Illumin")) return KindEmissive;
        return KindOpaque;
    }

    // Interface and editor families (the native UI catalog's whole-family entries, path_overlay_programs.h): VaM's
    // HUD targets and billboards, the Battlehub handles, debug views, the hair painter, MK's UI. Never scene
    // surfaces: drawn over PT as UI. 0.22.62: Custom/TransparentHUD (a person's HUD targets) made PT refuse scenes.
    private static bool InterfaceShader(string name) {
        return name.StartsWith("UI/",StringComparison.Ordinal) || name.StartsWith("GUI/",StringComparison.Ordinal) ||
            name.StartsWith("Hidden/Internal-GUI",StringComparison.Ordinal) || name.StartsWith("Custom/SteamVR_",StringComparison.Ordinal) ||
            name.StartsWith("Battlehub/",StringComparison.Ordinal) || name=="Custom/TransparentHUD" || name=="Custom/TransparentBillboard" ||
            name.StartsWith("Custom/Debug",StringComparison.Ordinal) || name=="GPUTools/Painter" ||
            name.StartsWith("MK/Glow/Selective/UI/",StringComparison.Ordinal) || name=="Oculus/OVRMRClipPlane";
    }
    // A shader without a generated contract (an author's own CUA shader): Unity's conventions. Standard-style
    // materials carry _SrcBlend / _DstBlend / _Cull; otherwise the queue and RenderType decide (KindOf), culling back.
    private static PathShaderStates.State InferState(Material m,string name) {
        PathShaderStates.State s=new PathShaderStates.State();
        bool blended=(KindOf(m,name)&3u)==KindTranslucent;
        s.source=m.HasProperty("_SrcBlend")?(int)m.GetFloat("_SrcBlend"):blended?5:1;
        s.destination=m.HasProperty("_DstBlend")?(int)m.GetFloat("_DstBlend"):blended?10:0;
        s.cull=m.HasProperty("_Cull")?(int)m.GetFloat("_Cull"):m.HasProperty("_CullMode")?(int)m.GetFloat("_CullMode"):2;
        s.known=true;return s;
    }

    private static long Mix(long h, long v) { unchecked { return (h ^ v) * 1099511628211L; } }
    private static Vector4 TextureST(Material m,string name)
    {
        if(!Declared(m,name))return new Vector4(1,1,0,0);
        Vector2 scale=m.GetTextureScale(name),offset=m.GetTextureOffset(name);
        return new Vector4(scale.x,scale.y,offset.x,offset.y);
    }
    private static uint TextureMode(Texture t) {
        return t==null?0u:(uint)t.wrapModeU|((uint)t.wrapModeV<<2)|(t.filterMode==FilterMode.Point?16u:t.filterMode==FilterMode.Bilinear?32u:0u);
    }
    private static uint TextureMode(Material m,string name) {
        return TextureMode(Tex(m,name));
    }
    private static long MixST(long key,Vector4 st)
    {
        for(int i=0;i<4;i++)key=Mix(key,Bits(st[i]));
        return key;
    }

    // ---- 0.23.4: author shaders (pt-scene-elements-20261105) -----------------------------------------------------
    // A shader without a generated contract carries its Blend / ZWrite / Cull in its compiled passes; Unity 2018.1
    // cannot read them (PluginIdea/HPVL's light cones: Blend OneMinusDstColor One, ZWrite Off, Cull Front and Back,
    // no _SrcBlend property; inferred as an alpha-blended surface they were traced as solid cones around their own
    // spot lights: black cones and an unlit stage). The renderer reads the D3D11 state of their draws (RT_DRAW_WATCH,
    // native PathObserve); the next read translates the material by it:
    //   unlit (no FORWARD / DEFERRED / PREPASS pass: the author's program computes the final colour): its own draws
    //     replayed over PT as an effect, as authored; not traced when it blends or writes no depth, else traced hidden
    //     from camera rays (shadows, reflections, bounces) as an emitter of its main colour;
    //   lit: a PT surface from its standard properties with the real blend and cull; a lit glow (additive, screen,
    //     multiply: not an alpha blend) that writes no depth: replayed, not traced;
    //   a mirror family (name) stays a PT mirror.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int ObservedFn([In, Out] uint[] pairs, int cap);
    internal static ObservedFn stObserved;
    internal static bool authorOff; // test only (PathCanvasUi.TestAuthor): 0.23.3 translation of author shaders
    private static readonly uint[] observedPairs = new uint[1024];
    private static readonly Dictionary<int, uint> observedStates = new Dictionary<int, uint>();
    private static int observedChanges;
    private static void PollObserved() {
        if (stObserved == null) return;
        int n = stObserved(observedPairs, observedPairs.Length);
        for (int i = 0; i < n; i++) {
            int id = unchecked((int)observedPairs[2 * i]); uint bits = observedPairs[2 * i + 1], old;
            if (observedStates.TryGetValue(id, out old) && old == bits) continue;
            observedStates[id] = bits; observedChanges++;
            MatInfo e; if (materials.TryGetValue(id, out e)) e.readFrame = -1000000;
        }
    }
    // D3D11_BLEND -> UnityEngine.Rendering.BlendMode
    private static int UnityBlend(int d3d) {
        switch (d3d) { case 1: return 0; case 2: return 1; case 3: return 3; case 4: return 6; case 5: return 5; case 6: return 10;
                       case 7: return 7; case 8: return 8; case 9: return 2; case 10: return 4; case 11: return 9; default: return 1; }
    }
    private static readonly Dictionary<int, bool> shaderLit = new Dictionary<int, bool>();
    private static readonly Dictionary<int, uint> shaderObserved = new Dictionary<int, uint>(); // shader id -> last observed state
    // Unity names a surface shader's lit passes FORWARD / FORWARD_DELTA / DEFERRED / PREPASS; hand-written lit passes
    // FORWARDBASE / FORWARDADD... Of the 205 author shaders in the library 95 of 98 lit ones match, no unlit one does.
    // Unity's Nature/Tree Soft Occlusion: vertex lit, its passes carry no name. Taken for unlit, the 1573 grass cards of Produce
    // 69's forest emitted their main colour: tree trunks and legs lit orange from the grass (11-09).
    private static bool AuthorLit(Material m) {
        Shader sh = m.shader; int sid = sh != null ? sh.GetInstanceID() : 0; bool lit;
        if (shaderLit.TryGetValue(sid, out lit)) return lit;
        lit = sh != null && sh.name.StartsWith("Nature/", StringComparison.Ordinal);
        for (int i = 0; i < m.passCount && !lit; i++) {
            string pass = m.GetPassName(i) ?? "";
            lit = pass.StartsWith("FORWARD", StringComparison.OrdinalIgnoreCase) || pass.StartsWith("DEFERRED", StringComparison.OrdinalIgnoreCase) ||
                  pass.StartsWith("PREPASS", StringComparison.OrdinalIgnoreCase);
        }
        shaderLit[sid] = lit; return lit;
    }
    // Authors' copies of Unity's Standard (Standard (Roughness setup) in 86 packages, Standard-DoubleSided, Standard
    // 2-Sided, Bakery/Standard ...): the Standard property set, Unity's keyword semantics.
    private static bool StandardCopy(Material m, string name) {
        // Declared: VaM's person materials keep the properties of a Standard they once had (HasProperty is true for them)
        return name != "Standard" && name != "Standard (Specular setup)" && !name.StartsWith("Custom/Subsurface/", StringComparison.Ordinal) && !name.StartsWith("Custom/Hair/", StringComparison.Ordinal) &&
               !name.StartsWith("Marmoset/", StringComparison.Ordinal) && Declared(m, "_Mode") && Declared(m, "_UVSec") &&
               Declared(m, "_Glossiness") && Declared(m, "_SrcBlend") && Declared(m, "_DstBlend") &&
               (Declared(m, "_MetallicGlossMap") || Declared(m, "_SpecGlossMap"));
    }

    // On-demand inspection for the existing game test agent; no per-frame work.
    private static string HzmMaterialAudit() {
        System.Text.StringBuilder b=new System.Text.StringBuilder();
        foreach(MatInfo e in materials.Values) {
            if(e.shader==null || !e.shader.StartsWith("HZM/General/PBRAdvanced",StringComparison.Ordinal) || Time.frameCount-e.lastUsed>120)continue;
            b.Append(e.name).Append('|').Append(e.shader).Append('|').Append(e.mode).Append('|')
             .Append(e.main!=IntPtr.Zero?1:0).Append('|').Append(e.normal!=IntPtr.Zero?1:0).Append('|')
             .Append(e.surface!=IntPtr.Zero?1:0).Append('|').Append(e.emission!=IntPtr.Zero?1:0).Append('|')
             .Append(e.metallic.ToString("R",System.Globalization.CultureInfo.InvariantCulture)).Append('|')
             .Append(e.roughness.ToString("R",System.Globalization.CultureInfo.InvariantCulture)).Append('|')
             .Append(e.specBump.ToString("R",System.Globalization.CultureInfo.InvariantCulture)).Append('|')
             .Append(Mathf.Max(e.emissionColor.r,Mathf.Max(e.emissionColor.g,e.emissionColor.b)).ToString("R",System.Globalization.CultureInfo.InvariantCulture)).Append('\n');
        }
        return b.ToString();
    }
    private static MatInfo MaterialFor(Material m, int frame)
    {
        if (m == null) return null;
        PreparePathMaterialDraw(m);
        int id = m.GetInstanceID();
        MatInfo e;
        if (!materials.TryGetValue(id, out e)) { e = new MatInfo();e.name=m.name??"";e.id=id; materials[id] = e; }
        e.lastUsed = frame;
        // Both renderers need current coverage/material state. Legacy reads
        // only its atlas inputs; mode changes cannot reuse an incomplete PT record.
        bool path=VaMSkinTransmission.PathSceneActive;
        if(e.readFrame==frame && e.readPath==path)return e;
        e.readFrame=frame;e.readPath=path;
        string shaderName=ShaderName(m.shader);
        // Hazmhox PBR shaders use _Albedo and _ColorCorrection, not Unity Standard's properties.
        // WPos samples three world-space projections; the path shader handles their blend.
        // 11-09: PBRAdvanced2Sided is PBRAdvanced's program with Cull Off. Glass07 / Glass08 are the same author's surfaces in
        // Unity's premultiplied transparency (their disassembly): albedo _GlassColor (x _Diffuse), opacity its alpha, metallic
        // mask.r x intensity, roughness mask.a x intensity; not a refractive volume (Hotel's poster behind a rough solid of
        // glass, its bottles without their labels). Glass07's GrabPass refraction offset is not reproduced, and their mask is not
        // read (PT's HZM slot reads PBRAdvanced's layout, roughness in .g): the intensities alone.
        bool hzmGlass=path && (shaderName=="HZM/Transparent/Glass07PBRMask" || shaderName=="HZM/Transparent/Glass08PBRDiffuseMask");
        bool hzm=path && (shaderName=="HZM/General/PBRAdvanced" || shaderName=="HZM/General/PBRAdvancedWPos" || shaderName=="HZM/General/PBRAdvanced2Sided" || hzmGlass);
        bool hzmWorld=hzm && shaderName=="HZM/General/PBRAdvancedWPos";
        bool panoramaBeta=shaderName=="Skybox/PanoramicBeta";
        bool panorama=path && (shaderName=="Skybox/Panoramic" || panoramaBeta) &&
            (!m.HasProperty("_Mapping") || m.GetFloat("_Mapping")>=0.5f);
        string mainName=hzmGlass?"_Diffuse":hzm?"_Albedo":panoramaBeta?"_Tex":"_MainTex";
        Texture main = Tex(m,mainName);
        Texture alpha = Tex(m,"_AlphaTex");
        e.main = TexturePtr(main);
        e.alpha = TexturePtr(alpha);
        e.mainVersion=main is RenderTexture?(uint)frame:main!=null?main.updateCount:0;
        e.alphaVersion=alpha is RenderTexture?(uint)frame:alpha!=null?alpha.updateCount:e.mainVersion;
        e.kind = KindOf(m,shaderName);
        e.cutoff = m.HasProperty("_Cutoff") ? m.GetFloat("_Cutoff") : 0.5f;
        Color c = hzmGlass && Declared(m,"_GlassColor") ? m.GetColor("_GlassColor") : hzm && Declared(m,"_ColorCorrection") ? m.GetColor("_ColorCorrection") : m.HasProperty("_Color") ? m.GetColor("_Color") : Color.white;
        if(hzm && !hzmGlass)c.a=1f; // the HZM PBR programs write opaque coverage
        e.opacity = c.a;
        e.color = c.linear;
        Vector2 sc = main != null ? m.GetTextureScale(mainName) : Vector2.one, of = main != null ? m.GetTextureOffset(mainName) : Vector2.zero;
        e.adjust = 0f;
        // VaM's separate-alpha shaders (clothing, hair scalps and cards, eyelashes): their programs' alpha is
        // saturate(_AlphaTex's alpha at uv0 x _AlphaTex_ST + _AlphaAdjust), never the main texture's nor _Color's. With
        // an alpha map: rays go on through 1 - that (the host adds the adjust when it fills the slot); -1: not drawn;
        // without one (VaM's white default) or at +1: opaque, unless the adjust takes it down (a white map at 1 + adjust).
        e.shader=shaderName;
        if (alpha != null || Has(shaderName, "SeparateAlpha"))
        {
            float adjust = Declared(m,idAlphaAdjust) ? m.GetFloat(idAlphaAdjust) : 0f;
            e.opacity = 1f;
            if (adjust <= -0.99f) { e.kind = KindTranslucent; e.opacity = 0f; }
            else if (alpha == null || adjust >= 0.99f)
            {
                float a = alpha == null ? Mathf.Clamp01(1f + adjust) : 1f;
                if (a >= 0.999f) e.kind = e.kind == KindEmissive ? KindEmissive : KindOpaque;
                else { e.kind = KindTranslucent; e.alpha = TexturePtr(Texture2D.whiteTexture); e.opacity = a; }
            }
            else
            {
                e.kind = (e.kind&3)==KindCutout?KindCutout:KindTranslucent;
                e.adjust = adjust;
                // 0.22.0 SimXR: with _MainTex_ST a lace choker's tiled alpha was stretched into centimetre bars
                sc = m.GetTextureScale("_AlphaTex");
                of = m.GetTextureOffset("_AlphaTex");
            }
            // clothing (VaM's Transparent...SeparateAlpha family) with an alpha map: lace threads are a 512 texel wide
            // (a grey sheet over the skin): RtCore's 2048 slots. Hair cards and the rest: 512.
            if (alpha != null && e.kind == KindTranslucent && Has(shaderName, "Subsurface/Transparent")) e.kind |= KindFineAlpha;
            // 0.22.1: hair cards and scalps (Custom/Hair/...): RtCore's close range for hair, not clothing's
            if (e.kind == KindTranslucent && Has(shaderName, "Custom/Hair/")) e.kind |= KindHair;
        }
        // API 43 (docs\pt-author-intent-20261109): what Unity does with this material's shadows. Past queue 2500 it receives
        // none. Translucent on a Custom/Subsurface program, the shadow map takes it through its fallback's ShadowCaster: a
        // cutout at _Cutoff, or every pixel (Alpha Cutoff 0 even at Alpha Adjust -1: an unseen shell that casts). Standard
        // dithers by alpha, which is 1 - alpha. A person's own materials stay as they are (Write): 0.23.7, whole shadows of
        // clothing on the body, was refused.
        e.authorSolid=0;e.shadowCutoff=e.cutoff;e.noReceive=m.renderQueue>2500;
        if((e.kind&3u)==KindTranslucent && (e.kind&KindHair)==0 && shaderName.StartsWith("Custom/Subsurface/",StringComparison.Ordinal)) {
            int caster=PathShadowCasters.Of(shaderName);
            bool separate=alpha!=null || Has(shaderName,"SeparateAlpha");
            if(caster==PathShadowCasters.Solid || (caster==PathShadowCasters.SeparateAlpha && separate) || (caster==PathShadowCasters.MainAlpha && !separate)) {
                e.authorSolid=KindSolidShadow;
                if(caster==PathShadowCasters.Solid)e.shadowCutoff=0f;
            }
        }
        e.st = new Vector4(sc.x, sc.y, of.x, of.y);
        e.env = EnvironmentOf(shaderName);
        e.subsurface = e.env == EnvVamSkin && m.HasProperty("_SubdermisColor") ? m.GetColor("_SubdermisColor").linear : Color.black;
        e.iblFilter = e.env == EnvVamSkin && m.HasProperty("_IBLFilter") ? m.GetFloat("_IBLFilter") : 0f;
        e.tess = Has(shaderName,"Tess") && m.HasProperty("_TessPhong") ? m.GetFloat("_TessPhong") : 0f;
        e.discard=shaderName=="Custom/Discard";
        if(!path) {
            bool legacyOverlay=!e.discard && (Has(RenderType(m),"Overlay") ||
                shaderName.StartsWith("UI/",StringComparison.Ordinal) || shaderName.StartsWith("GUI/",StringComparison.Ordinal) ||
                shaderName.StartsWith("Hidden/Internal-GUI",StringComparison.Ordinal) || shaderName.StartsWith("Custom/SteamVR_",StringComparison.Ordinal));
            e.pathFlags=legacyOverlay?32u:0u;
            long key=Mix(Mix(Mix(-3750763034362895579L,id),e.main.ToInt64()),e.alpha.ToInt64());
            key=MixST(MixST(Mix(key,e.kind),e.color),e.st);
            key=MixST(key,new Vector4(e.cutoff,e.opacity,e.adjust,e.tess));
            key=MixST(Mix(key,e.env),new Vector4(e.subsurface.r,e.subsurface.g,e.subsurface.b,e.iblFilter));
            e.revision=key==0?1:key;e.key=((long)unchecked((uint)id))<<2;
            return e;
        }
        // PT materials retain the actual textures, separate from the old
        // average-colour/alpha records. Shader-specific VaM gloss stays explicit.
        Vector2 ms = main != null ? m.GetTextureScale(mainName) : Vector2.one;
        Vector2 mo = main != null ? m.GetTextureOffset(mainName) : Vector2.zero;
        e.mainST = new Vector4(ms.x, ms.y, mo.x, mo.y);
        e.normal = TexturePtr(Tex(m,"_BumpMap"));
        bool metalWorkflow = hzm || m.HasProperty("_Metallic");
        bool vamGloss = Declared(m,"_GlossTex");
        string surfaceName = hzm?(Declared(m,"_Mask1")?"_Mask1":"_Mask"):metalWorkflow ? "_MetallicGlossMap" : m.HasProperty("_SpecGlossMap") ? "_SpecGlossMap" : "_SpecTex";
        e.surface = TexturePtr(Tex(m,surfaceName));
        e.emission = TexturePtr(Tex(m,"_EmissionMap"));
        e.gloss = TexturePtr(vamGloss ? m.GetTexture("_GlossTex") : null);
        bool vamDetail=Declared(m,"_DetailMap") && Declared(m,"_DetailWeight");
        e.detailNormal=TexturePtr(vamDetail?m.GetTexture("_DetailMap"):Tex(m,"_DetailNormalMap"));
        e.detailMask=TexturePtr(Tex(m,"_DetailMask"));
        e.normalST=TextureST(m,"_BumpMap");e.surfaceST=TextureST(m,surfaceName);
        e.emissionST=TextureST(m,"_EmissionMap");e.glossST=TextureST(m,"_GlossTex");
        // Unity Standard shares detail albedo's UV transform with its normal.
        e.detailST=TextureST(m,vamDetail?"_DetailMap":m.HasProperty("_DetailAlbedoMap")?"_DetailAlbedoMap":"_DetailNormalMap");
        e.second=IntPtr.Zero;e.secondST=new Vector4(1,1,0,0);e.secondSampling=0;e.mode=0;e.modeParam=0;
        if(hzm) {
            e.normal=TexturePtr(Tex(m,"_Normal"));e.normalST=TextureST(m,"_Normal");
            e.emission=TexturePtr(Tex(m,"_Emissive"));e.emissionST=TextureST(m,"_Emissive");
            if(hzmGlass)e.surface=IntPtr.Zero;else e.mode|=1024u;
        }
        if(hzmWorld) {
            float tiling=Declared(m,"_Tiling")?m.GetFloat("_Tiling"):1f;
            e.mainST=new Vector4(tiling,tiling,0,0);
            e.mode|=512u;e.modeParam=Declared(m,"_AlbedoBlend")?m.GetFloat("_AlbedoBlend"):1f;
        }
        // VaM's Subsurface programs composite _DecalTex (uv0 x its ST) over the tinted albedo by its alpha (skin
        // makeup and tattoos, clothing prints); unassigned is Unity's clear black.
        Texture decal=Tex(m,"_DecalTex");
        if(decal!=null){e.second=TexturePtr(decal);e.secondST=TextureST(m,"_DecalTex");e.secondSampling=TextureMode(decal);e.mode|=1u;}
        bool standard=shaderName=="Standard" || shaderName=="Standard (Specular setup)" || (!authorOff && StandardCopy(m,shaderName));
        // 0.23.4: Unity's Standard (Roughness setup): _Glossiness is the perceptual roughness, _SpecGlossMap.r the roughness
        // map (_SPECGLOSSMAP), the metallic map's alpha is not smoothness (UnityStandardInput.cginc MetallicRough).
        bool roughnessSetup=!authorOff && standard && Declared(m,"_Metallic") && Declared(m,"_SpecGlossMap") && !Declared(m,"_GlossMapScale");
        Texture roughnessMap=roughnessSetup && m.IsKeywordEnabled("_SPECGLOSSMAP")?Tex(m,"_SpecGlossMap"):null;
        if(standard && !authorOff)e.mode|=262144u; // reads unity_SpecCube0 (PtProbeRadiance)
        if(standard) {
            // Unity Standard reads a map only under its keyword, and a metallic/specular map REPLACES _Metallic /
            // _SpecColor, its alpha (or the albedo's) scaled by _GlossMapScale replaces _Glossiness.
            if(!m.IsKeywordEnabled("_NORMALMAP"))e.normal=IntPtr.Zero;
            bool detail=m.IsKeywordEnabled("_DETAIL_MULX2");
            if(!detail)e.detailNormal=IntPtr.Zero;
            Texture detailAlbedo=detail?Tex(m,"_DetailAlbedoMap"):null;
            if(detailAlbedo!=null){e.second=TexturePtr(detailAlbedo);e.secondST=e.detailST;e.secondSampling=TextureMode(detailAlbedo);e.mode|=2u;}
            if(!m.IsKeywordEnabled(metalWorkflow?"_METALLICGLOSSMAP":"_SPECGLOSSMAP"))e.surface=IntPtr.Zero;
            if(m.IsKeywordEnabled("_SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A"))e.mode|=64u;
            if(m.IsKeywordEnabled("_ALPHAPREMULTIPLY_ON"))e.mode|=8u;
            // 0.23.4: every main map (albedo, normal, metallic / specular, emission, occlusion) samples uv0 x _MainTex_ST
            // (UnityStandardInput.cginc TexCoords); their own _ST are never read. A tiled floor kept 1x normals.
            if(!authorOff)e.normalST=e.surfaceST=e.emissionST=e.mainST;
        }
        e.specBump=m.HasProperty("_SpecularBumpiness")?m.GetFloat("_SpecularBumpiness"):1;
        if(hzmWorld)e.specBump=Declared(m,"_NormalMalBlend")?m.GetFloat("_NormalMalBlend"):1f;
        e.diffuseBump=m.HasProperty("_DiffuseBumpiness")?m.GetFloat("_DiffuseBumpiness"):1;
        e.detailBump=vamDetail?m.GetFloat("_DetailWeight"):m.HasProperty("_DetailNormalMapScale")?m.GetFloat("_DetailNormalMapScale"):1;
        PathShaderStates.State renderState=PathShaderStates.Read(m,shaderName);
        if(panorama) {renderState.known=true;renderState.source=1;renderState.destination=0;renderState.cull=0;}
        if(hzmGlass) {renderState.known=true;renderState.source=1;renderState.destination=10;renderState.cull=2;} // One, OneMinusSrcAlpha, Cull Back
        e.programSet=PathShaderPrograms.Id(shaderName);
        e.shaderStateKnown=renderState.known;
        if(!renderState.known)renderState=InferState(m,shaderName);
        e.discard=shaderName=="Custom/Discard"; // verified unconditional pixel discard
        bool overlay=!e.discard && (Has(RenderType(m),"Overlay") || InterfaceShader(shaderName));
        // 0.23.4: translate an author's shader by the state its draws really use (see PollObserved)
        e.watch=!authorOff && !e.shaderStateKnown && !e.discard && !overlay;
        e.effect=e.effectOnly=false;
        uint seen=0;
        string authorNote="";
        // Third-party water (Lux Water's ocean, PluginIdea's wet floors) is its own renderer: displaced in tessellation, foam
        // from the depth under it, a reflection camera. Standard properties make a flat coloured sheet of it. Drawn as authored.
        bool waterReplay=Has(shaderName,"Lux Water/WaterSurface") || Has(shaderName,"PluginIdea/WaterSurface");
        if(waterReplay){e.effect=e.effectOnly=true;authorNote="third-party water: replayed over PT as authored, not traced";}
        // A material is observed only where its own draw resolves in the registry: renderers sharing one mesh (HPVL's
        // cones, many per scene) register one material per index range, and off-screen renderers are never drawn. Those
        // take the state another material of the same shader was drawn with; an unlit shader in the transparent queue
        // that was never drawn is taken as an effect until it is (in 0.23.4's first run a stage's light cones that were not
        // observed stayed black solids).
        int sid=m.shader!=null?m.shader.GetInstanceID():0;
        bool observed=e.watch && observedStates.TryGetValue(id,out seen) && (seen&2u)!=0;
        if(observed)shaderObserved[sid]=seen;
        else if(e.watch && shaderObserved.TryGetValue(sid,out seen))observed=true;
        if(!observed && e.watch && m.renderQueue>=2450 && !AuthorLit(m) && !Has(shaderName,"Mirror")) {
            e.effect=e.effectOnly=true;
            authorNote="unlit author shader in the transparent queue, not drawn yet: replayed over PT when drawn, not traced";
        }
        if(observed) {
            int src=(int)((seen>>3)&31u),dst=(int)((seen>>8)&31u);
            bool blend=(seen&4u)!=0 && !(src==2 && dst==1),depthWrites=(seen&65536u)!=0;
            bool alphaLike=blend && (src==5 || src==2) && dst==6;
            renderState.source=blend?UnityBlend(src):1;renderState.destination=blend?UnityBlend(dst):0;
            if(((seen>>18)&3u)==1u)renderState.cull=0; // D3D11_CULL_NONE (front / back can be Unity's mirrored-transform swap)
            bool lit=AuthorLit(m);
            if(!Has(shaderName,"Mirror")) {
                if(!lit){e.effect=true;e.effectOnly=blend || !depthWrites;}
                else if(blend && !alphaLike && !depthWrites)e.effect=e.effectOnly=true;
            }
            if(!e.effectOnly) {
                bool cutout=Has(shaderName,"Cutout") || Has(shaderName,"AlphaTest") || m.IsKeywordEnabled("_ALPHATEST_ON") ||
                    Has(RenderType(m),"Cutout") || m.HasProperty("_Cutoff");
                uint low=e.kind&3u;
                if(alphaLike && low!=KindTranslucent)e.kind=(e.kind&~3u)|KindTranslucent;
                else if(!blend && low==KindTranslucent)e.kind=(e.kind&~3u)|(cutout?KindCutout:KindOpaque);
            }
            authorNote=(lit?"lit":"unlit")+" author shader, its draws "+(blend?"blend "+src+"/"+dst:"opaque")+(depthWrites?", write depth":", no depth")+
                (((seen>>18)&3u)==1u?", two-sided":"")+": "+(e.effectOnly?"replayed over PT as authored, not traced":
                e.effect?"replayed over PT as authored, traced hidden from the camera (emits its main colour)":"a PT surface from its standard properties");
        }
        e.pathSupported=e.discard || overlay || shaderName=="Standard" || shaderName=="Standard (Specular setup)" ||
            shaderName.StartsWith("Custom/Subsurface/") || shaderName.StartsWith("Custom/Hair/") ||
            shaderName.StartsWith("Marmoset/") || shaderName.StartsWith("Unlit/") ||
            shaderName=="Legacy Shaders/Diffuse" || shaderName=="Legacy Shaders/Specular" ||
            shaderName=="Legacy Shaders/Transparent/Diffuse" || shaderName=="Legacy Shaders/Transparent/Specular" || Has(shaderName,"Custom/Mirror");
        e.environmentOnly=!e.discard && !overlay && (Has(shaderName,"Marmoset/Sky") || (Has(shaderName,"Skybox") && !panorama));
        // 0.22.62: a shader without its own adapter is translated by its standard properties (main map x colour,
        // normal, specular / gloss, alpha / cutout, emission, Unity's blend and cull); it stays a PT surface.
        e.unsupported=e.pathSupported && e.shaderStateKnown && !m.IsKeywordEnabled("_PARALLAXMAP")?"":
            !e.shaderStateKnown?"no adapter for this shader: generic surface from its standard properties, Unity's blend/cull":
            m.IsKeywordEnabled("_PARALLAXMAP")?"parallax offset not applied":"no adapter for this shader: generic surface from its standard properties";
        // 0.23.4: an observed author shader is noted once with its translation
        if(authorNote.Length>0) {
            e.unsupported="";
            if(PathNoteNew(sid^authorNote.GetHashCode(),10))PathNote(e.name+" / "+shaderName+": "+authorNote); // once per shader and translation
        }
        e.pathFlags = (metalWorkflow ? 1u : 0u) | (!vamGloss && e.surface != IntPtr.Zero ? 2u : 0u) | 4u | (vamGloss ? 16u : 0u)
            | (overlay?32u:0u) | (m.HasProperty("_SpecularBumpiness")?256u:0u) | (vamDetail?65536u:0u);
        if(e.effectOnly)e.pathFlags|=32u; // 0.23.4: replayed, never traced
        if(renderState.known && renderState.cull==2)e.pathFlags|=524288u;
        if(renderState.known && renderState.cull==1)e.pathFlags|=134217728u;
        if(Has(shaderName,"Cutout") || Has(shaderName,"AlphaTest") || m.IsKeywordEnabled("_ALPHATEST_ON"))e.pathFlags|=8388608u;
        bool postAlpha=Declared(m,idAlphaAdjust) && alpha==null && !Has(shaderName,"SeparateAlpha");
        e.materialControls=new Vector4(m.HasProperty("_DiffOffset")?m.GetFloat("_DiffOffset"):0,
            m.HasProperty("_SpecInt")?Mathf.Max(m.GetFloat("_SpecInt"),0):1,postAlpha?m.GetFloat("_AlphaAdjust"):0,e.cutoff);
        if(shaderName.StartsWith("Legacy Shaders/")) {
            if(m.HasProperty("_Shininess"))e.pathFlags|=268435456u;
            else e.materialControls.y=0; // authored diffuse-only surface
        }
        if(postAlpha)e.pathFlags|=32768u;
        e.specular = m.HasProperty("_SpecColor") ? m.GetColor("_SpecColor").linear : new Color(.04f,.04f,.04f,1);
        e.emissionColor = m.HasProperty("_EmissionColor") ? m.GetColor("_EmissionColor").linear : (e.kind == KindEmissive ? e.color : Color.black);
        e.roughness = 1f - Mathf.Clamp01(m.HasProperty("_Glossiness") ? m.GetFloat("_Glossiness") : .5f);
        e.metallic = hzm ? Mathf.Clamp01(m.GetFloat("_MetallicIntensity")) : metalWorkflow ? Mathf.Clamp01(m.GetFloat("_Metallic")) : 0f;
        if(hzm) {
            e.roughness=Mathf.Clamp01(m.GetFloat(Declared(m,"_RoughnessIntensity1")?"_RoughnessIntensity1":"_RoughnessIntensity"));
            // One environment's shader declares a switch and colour; another's shader of the same name omits both.
            bool enabled=e.emission!=IntPtr.Zero && (!Declared(m,"_UseEmissive") || m.GetFloat("_UseEmissive")!=0);
            e.emissionColor=enabled?(Declared(m,"_EmissiveColor")?m.GetColor("_EmissiveColor").linear:Color.white):Color.black;
        }
        if(standard) {
            float glossScale=m.HasProperty("_GlossMapScale")?Mathf.Clamp01(m.GetFloat("_GlossMapScale")):1f;
            if(e.surface!=IntPtr.Zero){e.roughness=1f-glossScale;if(metalWorkflow)e.metallic=1f;else e.specular=Color.white;}
            if((e.mode&64u)!=0){e.roughness=1f-glossScale;e.pathFlags&=~2u;}
            if(!m.IsKeywordEnabled("_EMISSION")){e.emission=IntPtr.Zero;e.emissionColor=Color.black;}
        }
        if(roughnessSetup) {
            e.pathFlags&=~(2u|16u);e.roughness=Mathf.Clamp01(m.GetFloat("_Glossiness"));
            if(roughnessMap!=null){e.gloss=TexturePtr(roughnessMap);e.glossST=e.mainST;e.pathFlags|=2147483648u;} // PT_ROUGHNESS_MAP
        }
        e.normalScale = m.HasProperty("_BumpScale") ? m.GetFloat("_BumpScale") : m.HasProperty("_Bumpiness")?m.GetFloat("_Bumpiness"):1f;
        if(hzm && !hzmWorld && Declared(m,"_NormalIntensity"))e.normalScale=m.GetFloat("_NormalIntensity");
        if(hzmGlass && Declared(m,"_NormalScale"))e.normalScale=m.GetFloat("_NormalScale");
        e.ior = m.HasProperty("_IOR") ? Mathf.Max(1.001f,m.GetFloat("_IOR")) : 1.5f;
        e.transmission = !hzmGlass && (e.pathFlags&32)==0 && (Has(shaderName,"Glass") || Has(shaderName,"Refract")) ? 1f : 0f;
        e.glossOffset = m.HasProperty("_GlossOffset") ? m.GetFloat("_GlossOffset") : 0f;
        e.shininess = m.HasProperty("_Shininess") ? m.GetFloat("_Shininess") : 8f;
        e.specOffset = m.HasProperty("_SpecOffset") ? m.GetFloat("_SpecOffset") : 0f;
        e.legacyFresnel=m.HasProperty("_Fresnel")?Mathf.Clamp01(m.GetFloat("_Fresnel")):.5f;
        if(shaderName.StartsWith("Custom/Subsurface/") && !overlay)e.pathFlags|=33554432u;
        if(shaderName.StartsWith("Marmoset/") && m.HasProperty("_SpecTex") && !overlay)e.pathFlags|=67108864u;
        // Skyshop stores its gloss in SpecTex.a and uses a log2 exponent,
        // unlike Standard's linear smoothness. Keep this separate from VaM's GlossTex.
        if(!vamGloss && surfaceName=="_SpecTex" && m.HasProperty("_Shininess")) {
            e.pathFlags=(e.pathFlags&~2u)|512u;
        }
        bool unlit=panorama || Has(shaderName,"Unlit") || (e.effect && !e.effectOnly),glow=Has(shaderName,"Emissive");
        if(unlit || glow) {
            e.emission=e.main;e.emissionST=e.mainST;e.emissionColor=e.color;
            e.pathFlags|=unlit?1024u:8192u;
            if(m.HasProperty("_DiffOffset"))e.pathFlags|=131072u;
            if(panorama) {
                e.mode|=2048u;
                if(m.HasProperty("_ImageType") && m.GetFloat("_ImageType")>=.5f)e.mode|=4096u;
                if(m.HasProperty("_MirrorOnBack") && m.GetFloat("_MirrorOnBack")>=.5f)e.mode|=8192u;
                if(m.HasProperty("_Layout")) {
                    int layout=Mathf.RoundToInt(m.GetFloat("_Layout"));
                    if(layout==1)e.mode|=16384u;else if(layout==2)e.mode|=32768u;
                }
                e.modeParam=m.HasProperty("_Rotation")?m.GetFloat("_Rotation"):0f;
                Color tint=m.HasProperty("_Tint")?m.GetColor("_Tint").linear:Color.white;
                float exposure=m.HasProperty("_Exposure")?m.GetFloat("_Exposure"):1f;
                e.emissionColor=tint*(4.5947938f*exposure);
            }
        } else if(Declared(m,"_Illum")) {
            e.emission=TexturePtr(m.GetTexture("_Illum"));e.emissionST=TextureST(m,"_Illum");
            e.emissionColor=e.color;e.pathFlags|=4096u;
            // Skyshop's Self-Illumin / Glow programs: _Illum.rgb x _GlowColor x _GlowStrength.
            if(m.HasProperty("_GlowColor")) {
                e.emissionColor=m.GetColor("_GlowColor").linear*(m.HasProperty("_GlowStrength")?Mathf.Max(m.GetFloat("_GlowStrength"),0f):1f);
                e.pathFlags&=~4096u;
            }
        }
        // Domains apply to entire shader families. Anatomical surface roles
        // below supply physical constants only; traversal/PSR has no name cases.
        if(Has(shaderName,"Marmoset/Transparent/Simple Glass/"))e.pathFlags|=2048u;
        if(Has(shaderName,"Simple Glass") && Has(e.name,"EyeReflection")) {
            e.ior=1.336f;e.transmission=1;e.roughness=.05f;e.pathFlags|=2048u|262144u;
        }
        if(Has(shaderName,"Subsurface/Transparent") && e.name.StartsWith("Tear",StringComparison.OrdinalIgnoreCase)) {
            // Tear is a wet film, not diffuse alpha-blended paint. Keep the
            // author's coverage (including zero), with clear bulk transmission.
            e.ior=1.336f;e.transmission=1;e.roughness=.05f;e.pathFlags|=2048u|16384u|262144u;
        }
        // 0.22.70: the eyeball in its lids' fold (PT's analytic diffuse scaled by the share its lids leave open)
        string eyePart=e.name;  // 0.23.1: read once (Material.name makes a new string at each call)
        if(shaderName.StartsWith("Custom/Subsurface/") && (eyePart.StartsWith("Sclera",StringComparison.OrdinalIgnoreCase) ||
            eyePart.StartsWith("Iris",StringComparison.OrdinalIgnoreCase) || eyePart.StartsWith("Pupil",StringComparison.OrdinalIgnoreCase)))
            e.pathFlags|=1073741824u;
        // Shader bytecode outputs black with alpha, SrcAlpha/OneMinusSrcAlpha,
        // ZWrite Off. Preserve that attenuation for ALL uses of this family.
        if(Has(shaderName,"Subsurface/AlphaMask")) {
            e.pathFlags|=2048u|1048576u;e.kind=KindTranslucent;e.transmission=1;
        }
        if(renderState.known && renderState.destination==1 && (renderState.source==1 || renderState.source==5)) {
            e.pathFlags|=2048u|2097152u;e.kind=KindTranslucent;e.transmission=1;
            if(renderState.source==5)e.pathFlags|=4194304u; // additive source is alpha-weighted
        }
        if(Has(shaderName,"PluginIdea/MirrorSurface")) {
            // PluginIdea's HighPerformanceMirrorReflection (a stage pack's walls and floors, their windows behind them): its
            // program adds reflection x _ReflectStrengh x (_Freshnel (1 + dir.y)^5 + .1) over the background, alpha _Color.a;
            // no reflection unless a mirror camera is looking at it, so it is not VaM's opaque mirror. GrabPass refraction
            // of the Refract variant is not reproduced.
            e.metallic=0;e.roughness=.03f;e.surface=IntPtr.Zero;e.pathFlags&=~(1u|2u|16u|512u|2097152u|4194304u);
            e.pathFlags|=2048u;e.kind=KindTranslucent;e.transmission=1;e.effect=e.effectOnly=false;
            e.specular=Color.black;e.materialControls.y=m.HasProperty("_ReflectStrengh")?Mathf.Max(m.GetFloat("_ReflectStrengh"),0f):1f;
            e.mode|=131072u;e.modeParam=m.HasProperty("_Freshnel")?Mathf.Max(m.GetFloat("_Freshnel"),0f):1f;
        } else if(Has(shaderName,"Mirror")) {
            // VaM's mirror programs: the reflection x _ReflectionColor (x lerp(1, _ReflectionBlendTex, power), at the
            // main texture's UV), plus the main texture's plain Lambert x (1 - _ReflectionOpacity); no _Color, no
            // Fresnel. Custom/Mirror itself has no main texture: reflection only.
            float opacity=m.HasProperty("_ReflectionOpacity")?Mathf.Clamp01(m.GetFloat("_ReflectionOpacity")):1f;
            e.metallic=0;e.roughness=.03f;e.surface=IntPtr.Zero;e.pathFlags&=~(1u|2u|16u|512u);
            e.specular=m.HasProperty("_ReflectionColor")?m.GetColor("_ReflectionColor").linear:Color.white;
            e.materialControls.y=1;e.mode|=16u;
            float diffuseWeight=main!=null && m.HasProperty("_MainTex")?1f-opacity:0f;e.color=new Color(diffuseWeight,diffuseWeight,diffuseWeight,e.color.a);
            Texture mask=Tex(m,"_ReflectionBlendTex");
            float power=m.HasProperty("_ReflectionBlendTexPower")?Mathf.Clamp01(m.GetFloat("_ReflectionBlendTexPower")):0f;
            if(mask!=null && power>0){e.second=TexturePtr(mask);e.secondST=e.mainST;e.secondSampling=TextureMode(mask);e.mode|=128u;e.modeParam=power;}
        }
        // TransparentGloss...SeparateAlpha clothing and MimicAlpha hair premultiply colour AND specular by the
        // coverage before SrcAlpha blending: they show alpha squared, not alpha.
        if((Has(shaderName,"Subsurface/TransparentGloss") && Has(shaderName,"SeparateAlpha")) || Has(shaderName,"Hair/MainSeparateAlphaMimicAlpha"))e.mode|=4u;
        // Custom/Hair layer programs remap uv0 x _MainTex_ST into [_uvXMin,_uvXMax] x [_uvYMin,_uvYMax] and read
        // every map (alpha, normal, specular, gloss too) there.
        if(m.HasProperty("_uvXMin") && m.HasProperty("_uvXMax") && m.HasProperty("_uvYMin") && m.HasProperty("_uvYMax")) {
            float x0=m.GetFloat("_uvXMin"),y0=m.GetFloat("_uvYMin"),rx=m.GetFloat("_uvXMax")-x0,ry=m.GetFloat("_uvYMax")-y0;
            Vector4 st=new Vector4(e.mainST.x*rx,e.mainST.y*ry,x0+e.mainST.z*rx,y0+e.mainST.w*ry);
            e.mainST=e.st=e.normalST=e.surfaceST=e.glossST=st;
        }
        e.sampling[0]=TextureMode(main);e.sampling[1]=TextureMode(m,hzm?"_Normal":"_BumpMap");e.sampling[2]=TextureMode(m,surfaceName);
        e.sampling[3]=unlit||glow?e.sampling[0]:TextureMode(m,hzm?"_Emissive":m.HasProperty("_Illum")?"_Illum":"_EmissionMap");
        e.sampling[4]=roughnessMap!=null?TextureMode(roughnessMap):TextureMode(m,"_GlossTex");e.sampling[5]=TextureMode(m,vamDetail?"_DetailMap":"_DetailNormalMap");
        e.sampling[6]=TextureMode(m,"_DetailMask");e.sampling[7]=TextureMode(alpha!=null?alpha:main);
        long k = Mix(Mix(Mix(-3750763034362895579L, id), e.main.ToInt64()), e.alpha.ToInt64());
        k = Mix(Mix(Mix(k, e.kind), Bits(e.cutoff)), Bits(e.opacity));
        k = Mix(Mix(Mix(k, Bits(e.color.r)), Bits(e.color.g)), Bits(e.color.b));
        k=MixST(k,e.st);
        k = Mix(k, Bits(e.adjust));
        k = Mix(Mix(Mix(Mix(k,e.normal.ToInt64()),e.surface.ToInt64()),e.emission.ToInt64()),e.gloss.ToInt64());
        k = Mix(k,e.pathFlags);
        k=MixST(k,new Vector4(e.roughness,e.metallic,e.normalScale,e.ior));k=Mix(k,Bits(e.transmission));
        k=MixST(MixST(k,e.specular),e.emissionColor);
        k=MixST(k,new Vector4(e.glossOffset,e.shininess,e.specOffset,e.tess));k=MixST(k,e.mainST);
        k=Mix(k,Bits(e.legacyFresnel));
        k=Mix(Mix(k,e.detailNormal.ToInt64()),e.detailMask.ToInt64());
        k=MixST(MixST(MixST(MixST(MixST(k,e.normalST),e.surfaceST),e.emissionST),e.glossST),e.detailST);
        k=MixST(k,new Vector4(e.specBump,e.diffuseBump,e.detailBump,0));
        k=MixST(k,e.materialControls);
        for(int i=0;i<8;i++)k=Mix(k,e.sampling[i]);
        k=MixST(Mix(Mix(Mix(k,e.second.ToInt64()),e.secondSampling),e.mode),e.secondST);
        k=Mix(k,Bits(e.modeParam));
        k=MixST(Mix(k,e.env),new Vector4(e.subsurface.r,e.subsurface.g,e.subsurface.b,e.iblFilter));
        e.revision=k==0?1:k;
        e.key=((long)unchecked((uint)id))<<2; // stable identity; scalar animation updates in place
        return e;
    }

    // per submesh of a character's mesh: whether it is one of the eyes' overlay layers (not occluding) or skipped
    private static bool[] SkinNoOcclude(Component c, Mesh mesh)
    {
        bool[] o;
        if (skinNoOcclude.TryGetValue(mesh, out o) && o.Length == 2 * mesh.subMeshCount) return o;
        int subs = mesh.subMeshCount;
        o = new bool[2 * subs];
        object dm = sDazMesh.GetValue(c);
        string[] names = dm != null ? pMaterialNames.GetValue(dm, null) as string[] : null;
        for (int k = 0; k < subs; k++)
        {
            string name = names != null && k < names.Length && names[k] != null ? names[k] : "";
            o[k] = NoOccludeSkin.IsMatch(name);
            o[subs + k] = SkipSkin.IsMatch(name);
        }
        skinNoOcclude[mesh] = o;
        return o;
    }

    // a wrap lying on the skin (decals, makeup, tattoos, eye overlays): 95% of its vertices within 1 mm (sampled)
    private static bool WrapOnSkin(Component c)
    {
        int id = c.GetInstanceID();
        object store = wStore.GetValue(c);
        KeyValuePair<object, bool> d;
        if (decals.TryGetValue(id, out d) && ReferenceEquals(d.Key, store)) return d.Value;
        bool on = false;
        Array verts = store == null ? null : wStoreVerts.GetValue(store) as Array;
        if (verts != null && verts.Length > 0)
        {
            int step = Math.Max(1, verts.Length / DecalSamples), near = 0, total = 0;
            for (int i = 0; i < verts.Length; i += step)
            {
                object v = verts.GetValue(i);
                float p = v == null ? 1f : (float)wVertProjection.GetValue(v);
                if (Math.Abs(p) < DecalMetres) near++;
                total++;
            }
            on = total > 0 && near >= DecalShare * total;
            if (decalSample.Length < 600)
                decalSample += (decalSample.Length > 0 ? "; " : "") + c.name.Replace("\"", "'") + " " + near + "/" + total + (on ? " on skin" : "");
            if (on) decalCount++;
        }
        decals[id] = new KeyValuePair<object, bool>(store, on);
        return on;
    }

    // ---- records ----------------------------------------------------------------------------------------------

    private static unsafe int Write(byte* draws, int n, long owner, IntPtr vertices, IntPtr normals, MeshEntry m, int sub, uint flags, uint stride,
                                    uint vertexCount, ref Matrix4x4 w, MatInfo mat,int layer)
    {
        if (m.count[sub] == 0) return n;
        bool path=VaMSkinTransmission.PathSceneActive;
        if(mat!=null&&mat.discard)flags|=256u;
        // A sky sphere supplies a visible backdrop; it must not shadow the scene's lights.
        if(path && mat!=null && (mat.mode&2048u)!=0)flags|=DrawCameraOnly;
        bool overlay=(flags&128u)!=0 || (mat!=null && (mat.pathFlags&32u)!=0);
        // A sky dome's program samples its cubemap by view direction: the PT environment is the sky, not a mesh.
        // 11-09: whatever atom holds it. VaM's own dome is under CoreControl, an editor-only atom (flag 128): it went to the
        // interface layer as an opaque draw, which composites after the effects and covered every effect in front of the sky
        // (one scene's HPVL cones, the whole purple haze), and PT's background with it.
        if(path && (mat==null || (mat.pathFlags&32u)==0) && (flags&256u)==0 && mat!=null && mat.environmentOnly) {
            if(PathNoteNew(mat.id,4))PathNote(mat.name+" / "+mat.shader+": sky dome mesh, the PT background is the scene's sky");
            return n;
        }
        if((flags&256u)==0 && !overlay && !m.triangles[sub]) {
            if(!path)return n;
            // Lines and points have no area to trace: drawn like the forward effects (particles), over PT.
            if(mat!=null?PathNoteNew(mat.id,5):PathNoteNew((int)owner,6))
                PathNote(mat!=null?mat.name+" / "+mat.shader+": line/point geometry, drawn as an effect":"line/point geometry of "+owner+", drawn as an effect");
            overlay=true;flags|=128u;
        }
        if(!overlay&&(flags&256u)==0)pathPhysicalLayers|=1u<<layer;
        if((flags&256u)==0 && !overlay && path && m.layoutError.Length>0 && !ReferenceEquals(m.layoutNoted,m.layoutError)) {
            m.layoutNoted=m.layoutError;PathNote(m.layoutError+": read with the stride of its buffer");
        }
        if(!overlay && (flags&DrawShadowOnly)==0 && mat!=null && mat.unsupported.Length>0 && path &&
            (!ReferenceEquals(mat.notedUnsupported,mat.unsupported) || !ReferenceEquals(mat.notedShader,mat.shader))) {
            mat.notedUnsupported=mat.unsupported;mat.notedShader=mat.shader;
            PathNote(mat.name+" / "+mat.shader+": "+mat.unsupported);
        }
        if(path && !overlay && (flags&(256u|DrawShadowOnly))==0 && mat!=null && eyeHidden.Contains(mat.id)) {flags|=DrawCameraHidden;lastCameraHidden++;}
        // 0.23.4: author shaders: the renderer reads their real state; an effect's own draws are replayed over PT, and
        // one that is still traced (an unlit solid) is hidden from camera rays (its replay is what the camera sees)
        if(path && mat!=null && (flags&256u)==0) {
            if(mat.watch)flags|=DrawWatch;
            if(mat.effect){flags|=DrawEffect;if(!overlay && (flags&DrawShadowOnly)==0)flags|=DrawCameraHidden;}
        }
        byte* d = draws + n * DrawBytes;
        *(long*)(d + 0) = owner;
        *(long*)(d + 8) = vertices.ToInt64();
        *(long*)(d + 16) = normals.ToInt64();
        *(long*)(d + 24) = m.indexBuffer.ToInt64();
        *(uint*)(d + 32) = m.start[sub];
        *(uint*)(d + 36) = m.count[sub];
        *(int*)(d + 40) = m.baseVertex[sub];
        *(uint*)(d + 44) = flags | (m.index32 ? DrawIndex32 : 0u);
        *(uint*)(d + 48) = stride!=0?stride:m.layoutError.Length==0?m.vertexStride:0;
        *(uint*)(d + 52) = vertexCount;
        float* x = (float*)(d + 56);
        x[0] = w.m00; x[1] = w.m01; x[2] = w.m02; x[3] = w.m03;
        x[4] = w.m10; x[5] = w.m11; x[6] = w.m12; x[7] = w.m13;
        x[8] = w.m20; x[9] = w.m21; x[10] = w.m22; x[11] = w.m23;
        float* f = (float*)(d + 124);
        if (mat != null)
        {
            *(long*)(d + 104) = mat.key | ((flags&DrawSkin)!=0?1L:(flags&DrawWrap)!=0?2L:0L);
            *(long*)(d + 112) = mat.main.ToInt64();
            bool sceneMaterial=path && (flags&(DrawSkin|DrawWrap))==0;
            *(uint*)(d + 120) = mat.kind | mat.env << 12 | (sceneMaterial?mat.authorSolid:0u);
            f[0] = sceneMaterial && mat.authorSolid!=0?mat.shadowCutoff:mat.cutoff; f[1] = mat.opacity;
            f[2] = mat.color.r; f[3] = mat.color.g; f[4] = mat.color.b;
            f[5] = mat.st.x; f[6] = mat.st.y; f[7] = mat.st.z; f[8] = mat.st.w;
            f[9] = mat.tess;
            *(long*)(d + 184) = mat.alpha.ToInt64();
            f[17] = mat.subsurface.r; f[18] = mat.subsurface.g; f[19] = mat.subsurface.b; f[20] = mat.iblFilter;
            lastKinds[mat.kind & 3]++;
        }
        else
        {
            *(long*)(d + 104) = 0L;
            *(long*)(d + 112) = 0L;
            *(uint*)(d + 120) = KindOpaque;
            f[0] = 0.5f; f[1] = 1f; f[2] = f[3] = f[4] = 0.5f; f[5] = f[6] = 1f; f[7] = f[8] = 0f; f[9] = 0f;
            *(long*)(d + 184) = 0L;
            f[17] = f[18] = f[19] = f[20] = 0f;
        }
        *(uint*)(d + 164) = m.uvOffset;
        *(long*)(d + 168) = m.uvBuffer.ToInt64();
        *(uint*)(d + 176) = m.layoutError.Length==0?m.uvStride|(m.uvHalf?0x80000000u:0u):0;
        *(float*)(d + 180) = mat != null ? mat.adjust : 0f;  // _AlphaAdjust (was padding: 0 = the map as it is)
        for (int k = 208; k < DrawBytes; k += 4) *(uint*)(d + k) = 0;
        *(uint*)(d + 588) = (uint)layer;  // LinkLights turns it into the draw's link class
        if (mat != null && path)
        {
            *(long*)(d + 208) = mat.normal.ToInt64(); *(long*)(d + 216) = mat.surface.ToInt64();
            *(long*)(d + 224) = mat.emission.ToInt64(); *(long*)(d + 232) = mat.gloss.ToInt64();
            float* p = (float*)(d + 240);
            p[0]=mat.mainST.x; p[1]=mat.mainST.y; p[2]=mat.mainST.z; p[3]=mat.mainST.w;
            bool skin = (flags & DrawSkin) != 0;
            p[4]=mat.specular.r; p[5]=mat.specular.g; p[6]=mat.specular.b; p[7]=mat.roughness;
            p[8]=mat.emissionColor.r; p[9]=mat.emissionColor.g; p[10]=mat.emissionColor.b; p[11]=mat.metallic;
            p[12]=mat.normalScale; p[13]=mat.ior; p[14]=mat.transmission;
            *(uint*)(d + 300) = mat.pathFlags | (skin ? 8u : 0u) | (overlay?32u:0u);
            p[16]=mat.glossOffset; p[17]=mat.shininess; p[18]=mat.specOffset;
        }
        if(overlay)*(uint*)(d+300)|=32u;
        *(uint*)(d + 316) = m.normalOffset;
        *(long*)(d+472)=m.layoutError.Length==0?m.tangentBuffer.ToInt64():0; // float-layout offsets only (else UV derivatives)
        *(uint*)(d+480)=m.tangentOffset;*(uint*)(d+484)=m.vertexStride;*(uint*)(d+488)=m.tangentHalf?2u:5u;
        *(uint*)(d+492)=m.revision;
        // Atlas material/alpha invalidation also consumes this shared revision.
        *(long*)(d+528)=mat!=null?mat.revision:0;
        *(uint*)(d+544)=mat!=null?mat.mainVersion:0;
        *(uint*)(d+548)=mat!=null?mat.alphaVersion:0;
        if(mat!=null && path) {
            float* p=(float*)(d+320);
            for(int i=0;i<4;i++){p[i]=mat.normalST[i];p[4+i]=mat.surfaceST[i];p[8+i]=mat.emissionST[i];p[12+i]=mat.glossST[i];}
            p[16]=mat.specBump;p[17]=mat.diffuseBump;p[18]=mat.detailBump;
            *(long*)(d+400)=mat.detailNormal.ToInt64();*(long*)(d+408)=mat.detailMask.ToInt64();
            for(int i=0;i<4;i++)*(float*)(d+416+i*4)=mat.detailST[i];
            for(int i=0;i<4;i++)*(float*)(d+456+i*4)=mat.materialControls[i];
            for(int i=0;i<8;i++)*(uint*)(d+496+i*4)=mat.sampling[i];
            *(long*)(d+528)=mat.revision;
            *(float*)(d+536)=mat.legacyFresnel;
            *(uint*)(d+540)=mat.programSet;
            *(long*)(d+552)=mat.second.ToInt64();
            for(int i=0;i<4;i++)*(float*)(d+560+i*4)=mat.secondST[i];
            *(uint*)(d+576)=mat.secondSampling;*(uint*)(d+580)=mat.mode|(mat.noReceive && (flags&(DrawSkin|DrawWrap))==0?65536u:0u);*(float*)(d+584)=mat.modeParam;
        }
        if ((flags & DrawNoOcclude) != 0) lastNoOcclude++;
        if (m.uvBuffer != IntPtr.Zero) lastUv++;
        return n + 1;
    }

    private static unsafe int AddSkin(Component c, int mask, int frame, byte* draws, int n)
    {
        if (c == null) return n;
        int id = c.GetInstanceID();
        if (seen.ContainsKey(id)) return n;
        seen[id] = true;
        if ((mask & (1 << c.gameObject.layer)) == 0) return n;
        if ((bool)sRenderSuspend.GetValue(c) || (bool)sStaticDraw.GetValue(null)) return n;
        Mesh mesh = sMesh.GetValue(c) as Mesh;
        if (mesh == null) return n;
        bool delayed = (bool)sDelay.GetValue(c);
        ComputeBuffer vertices;
        if (delayed) vertices = sDelayed.GetValue(c) as ComputeBuffer;
        else if ((bool)sSmoothing.GetValue(c)) vertices = sSmoothed.GetValue(c) as ComputeBuffer;
        else vertices = sRaw.GetValue(c) as ComputeBuffer;
        ComputeBuffer normals = sNormals.GetValue(c) as ComputeBuffer;  // DrawMeshGPU binds normals only when it is set
        if (normals != null && delayed) normals = sDelayedNormals.GetValue(c) as ComputeBuffer;
        if (vertices == null) return n;
        bool[] enabled = sMaterialsEnabled.GetValue(c) as bool[];
        Material simple = sSimple.GetValue(c) as Material;
        bool useSimple = (bool)sUseSimple.GetValue(c) && simple != null;
        Material[] mats = sMaterials.GetValue(c) as Material[];
        Vector3 offset = (Vector3)sOffset.GetValue(c);
        BufferEntry b = BufferFor(vertices, frame);
        BufferEntry nb = BufferFor(normals, frame);
        BufferEntry tb=BufferFor((delayed?sDelayedTangents:sTangents).GetValue(c) as ComputeBuffer,frame);
        MeshEntry m = b == null ? null : MeshFor(mesh, frame, false);
        if (m == null) return n;
        pathSpecialMeshes.Add(mesh);
        UvFor(mesh, m);
        bool[] o = SkinNoOcclude(c, mesh);
        Matrix4x4 w = Matrix4x4.identity;
        w.m03 = offset.x; w.m13 = offset.y; w.m23 = offset.z;
        int subs = m.subMeshCount;
        for (int s = 0; s < subs && n < MaxDraws; s++)
        {
            if (enabled == null || s >= enabled.Length || !enabled[s] || (o[subs + s] && !VaMSkinTransmission.PathSceneActive)) continue;
            Material mat = useSimple ? simple : mats != null && s < mats.Length ? mats[s] : null;
            if (mat == null) continue;
            WatchEye(mat);
            int at=n;
            n = Write(draws, n, id, b.pointer, nb != null ? nb.pointer : IntPtr.Zero, m, s, DrawSkin | (o[s] ? DrawNoOcclude : 0u), (uint)b.stride,
                      (uint)mesh.vertexCount, ref w, MaterialFor(mat, frame),c.gameObject.layer);
            if(n>at)WriteDynamicTangent(draws+at*DrawBytes,tb);
        }
        return n;
    }

    private static unsafe int AddWrap(Component c, bool noDelay, int mask, int frame, byte* draws, int n)
    {
        if (c == null) return n;
        int id = c.GetInstanceID();
        if (seen.ContainsKey(id)) return n;
        seen[id] = true;
        // DAZSkinWrap.DrawMeshGPU submits Graphics.DrawMesh on layer 0,
        // independently of the wrap component's hierarchy layer.
        if ((mask & 1) == 0) return n;
        if ((bool)wRenderSuspend.GetValue(c) || (bool)wStaticDraw.GetValue(null)) return n;
        Mesh mesh = wMesh.GetValue(c) as Mesh;
        if (mesh == null) return n;
        Component skin = wSkin.GetValue(c) as Component;
        bool delayed = skin != null && !noDelay && (bool)sDelay.GetValue(skin);
        ComputeBuffer vertices = (delayed ? wDelayed.GetValue(c) : wDraw.GetValue(c)) as ComputeBuffer;
        ComputeBuffer normals = (delayed ? wDelayedNormals.GetValue(c) : wNormals.GetValue(c)) as ComputeBuffer;
        if (vertices == null) return n;
        bool[] enabled = wMaterialsEnabled.GetValue(c) as bool[];
        Material simple = wSimple.GetValue(c) as Material;
        bool useSimple = (bool)wUseSimple.GetValue(c) && simple != null;
        Material[] mats = wMaterials.GetValue(c) as Material[];
        BufferEntry b = BufferFor(vertices, frame);
        BufferEntry nb = BufferFor(normals, frame);
        BufferEntry tb=BufferFor((delayed?wDelayedTangents:wTangents).GetValue(c) as ComputeBuffer,frame);
        MeshEntry m = b == null ? null : MeshFor(mesh, frame, false);
        if (m == null) return n;
        pathSpecialMeshes.Add(mesh);
        UvFor(mesh, m);
        uint flags = DrawWrap | (eyeOverlays.ContainsKey(id) || WrapOnSkin(c) ? DrawNoOcclude : 0u);
        if (VaMSkinTransmission.PathSceneActive && PtEnhancedEyesCompat)
        {
            if (!m.enhancedEyes.HasValue) m.enhancedEyes = EnhancedEyesClothing(c);
            if (m.enhancedEyes.Value)
            {
                flags |= 256u; // RT_DRAW_DISCARD: still register each draw for raster reconciliation
                if (PathNoteNew(id, 13)) PathNote(c.name + ": Enhanced Eyes shells replaced by PT eyes");
            }
        }
        Matrix4x4 w = Matrix4x4.identity;
        for (int s = 0; s < m.subMeshCount && n < MaxDraws; s++)
        {
            if (!useSimple && (enabled == null || s >= enabled.Length || !enabled[s])) continue;
            Material mat = useSimple ? simple : mats != null && s < mats.Length ? mats[s] : null;
            if (mat == null) continue;
            // 0.23.8 (11-06, user: 作者在角色上挂了很多透明部件，比如麦克风……这可能也是角色脸部变形的原因): a part its
            // author hid with Alpha Adjust -1 on a see-through shader draws nothing: not in PT at all (camera, shadows,
            // guides - its depth and motion never stand in front of a face).
            // 0.23.9 (user: 头发矢量没有了): only Custom/Subsurface/*SeparateAlpha* (alpha = saturate(_AlphaTex.a +
            // _AlphaAdjust)); hair shaders and opaque shaders in that queue keep a stray Alpha Adjust and still draw.
            if (mat.renderQueue >= 2450 && Declared(mat, idAlphaAdjust) && mat.GetFloat(idAlphaAdjust) <= -0.999f)
            {
                string sn = ShaderName(mat.shader);
                if (sn.StartsWith("Custom/Subsurface/", StringComparison.Ordinal) && sn.IndexOf("SeparateAlpha", StringComparison.Ordinal) >= 0) continue;
            }
            int mid = mat.GetInstanceID();
            if (!wrapShaders.ContainsKey(mid) && wrapShaders.Count < 4096)
                wrapShaders[mid] = (mat.shader != null ? mat.shader.name : "?") + " q" + mat.renderQueue;
            WatchEye(mat);
            int at=n;
            n = Write(draws, n, id, b.pointer, nb != null ? nb.pointer : IntPtr.Zero, m, s, flags, (uint)b.stride, (uint)mesh.vertexCount, ref w,
                      MaterialFor(mat, frame),0);
            if(n>at)WriteDynamicTangent(draws+at*DrawBytes,tb);
        }
        return n;
    }

    private static unsafe void WriteDynamicTangent(byte* d,BufferEntry t) {
        // Never use rest-pose tangents on deformed skin/cloth. A missing draw
        // buffer explicitly falls back to the deformed UV derivative basis.
        *(long*)(d+472)=t!=null?t.pointer.ToInt64():0;
        *(uint*)(d+480)=0;*(uint*)(d+484)=t!=null?(uint)t.stride:0;
    }

    private static unsafe int AddRigid(RigidEntry e, int mask, int frame, byte* draws, int n)
    {
        if (e.renderer == null || e.transform == null || e.filter == null) { rigidDirty = true; return n; }
        if (!e.renderer.enabled || !e.renderer.gameObject.activeInHierarchy || (mask & (1 << e.renderer.gameObject.layer)) == 0) return n;
        Mesh mesh = e.filter.sharedMesh;
        if (mesh == null) return n;
        if(!VaMSkinTransmission.PathSceneActive && (e.uiRole || e.renderer.shadowCastingMode==ShadowCastingMode.Off))return n;
        MeshEntry m = MeshFor(mesh, frame, true);
        if (m == null) return n;
        UvFor(mesh, m);
        ReadSharedMaterials(e.renderer,ref e.materials);
        bool propertyBlock=VaMSkinTransmission.PathSceneActive && e.renderer.HasPropertyBlock();
        Matrix4x4 w = e.transform.localToWorldMatrix;int first=0;
        if(e.renderer.isPartOfStaticBatch) {
            first=e.renderer.subMeshStartIndex;
            Transform root=staticBatchRoot!=null?staticBatchRoot.GetValue(e.renderer,null) as Transform:null;
            w=root!=null?root.localToWorldMatrix:Matrix4x4.identity;
        }
        int count=e.materials!=null?Math.Min(e.materials.Length,m.subMeshCount-first):0;
        uint shadowOnly=VaMSkinTransmission.PathSceneActive && e.renderer.shadowCastingMode==ShadowCastingMode.ShadowsOnly?DrawShadowOnly:0u;
        if(!e.uiRole)shadowOnly|=CameraOnly(e.renderer,mesh,e.id);
        if(VaMSkinTransmission.PathSceneActive && e.renderer.shadowCastingMode==ShadowCastingMode.Off)shadowOnly|=DrawNoCast;
        for (int s = 0; s < count && n < MaxDraws; s++)
        {
            Material mat = e.materials != null && s < e.materials.Length ? e.materials[s] : null;
            if (mat == null) continue;
            MatInfo info=MaterialFor(mat,frame);
            if(info.environmentOnly && SkyNeedsCapture(mat) && mat.renderQueue>=skyCaptureCandidateQueue &&
                e.renderer.bounds.Contains(pathEyeCamera!=null?pathEyeCamera.transform.position:Vector3.zero)) {
                skyCaptureCandidate=e.renderer;skyCaptureCandidateEntry=e;skyCaptureCandidateMaterial=mat;skyCaptureCandidateQueue=mat.renderQueue;
            }
            if(propertyBlock && !e.uiRole && (info.pathFlags&32)==0 && PathNoteNew(e.id,7))PathNote(e.renderer.name+": MaterialPropertyBlock not read, the material's own values are used");
            n = Write(draws, n, e.id, m.vertexBuffer, IntPtr.Zero, m, first+s, DrawRigid|(m.dynamic?64u:0u)|(e.uiRole?128u:0u)|shadowOnly, 0u, (uint)m.vertexCount, ref w, info,e.renderer.gameObject.layer);
        }
        return n;
    }

    private static ReflectionProbe PathProbe()
    {
        ReflectionProbe best=null;
        Vector3 eye=pathEyeCamera!=null?pathEyeCamera.transform.position:Vector3.zero;
        bool bestAround=false;
        for(int i=0;i<pathProbes.Count;i++) {
            ReflectionProbe p=pathProbes[i];
            if(p==null || !p.enabled || !p.gameObject.activeInHierarchy || p.texture==null)continue;
            bool around=p.bounds.Contains(eye);
            if(best==null || p.importance>best.importance || (p.importance==best.importance && around && !bestAround)){best=p;bestAround=around;}
        }
        return best;
    }

    private static void RebuildRigid()
    {
        rigidDirty = false;
        rigids.Clear();
        pathLights.Clear();
        pathProbes.Clear();
        pathIndoorMarker=null;pathIndoorCube=null;pathIndoorPointer=IntPtr.Zero;
        if(pathRegistryActive && SuperController.singleton!=null) {
            pathIndoorMarker=SuperController.singleton.GetAtomByUid("PT_IndoorEnvironment");
            Atom source=SuperController.singleton.GetAtomByUid("CustomUnityAsset");
            Transform root=source!=null && source.reParentObject!=null?source.reParentObject.Find("object/rescaleObject"):null;
            if(pathIndoorMarker!=null && root!=null) {
                ReflectionProbe[] probes=root.GetComponentsInChildren<ReflectionProbe>(true);
                foreach(ReflectionProbe probe in probes)if(probe!=null && probe.enabled && probe.texture!=null) {
                    pathIndoorCube=probe.texture;pathIndoorPointer=pathIndoorCube.GetNativeTexturePtr();break;
                }
            }
        }
        object controller = cSingleton.GetValue(null, null);
        System.Collections.IList atoms = controller == null ? null : cGetAtoms.Invoke(controller, null) as System.Collections.IList;
        if (atoms == null) return;
        Dictionary<int, bool> ids = new Dictionary<int, bool>();
        for (int i = 0; i < atoms.Count; i++)
        {
            Component atom = atoms[i] as Component;
            // Exclude procedural hair by its renderer type below, not every
            // MeshRenderer owned by a Person (which can also own real props).
            if (atom == null) continue;
            string type=aType.GetValue(atom) as string;
            bool ui=EditorOnlyAtom(type);
            AddRigidList(aMeshRenderers.GetValue(atom) as System.Collections.IList, ids,ui);
            AddRigidList(aDynamicMeshRenderers.GetValue(atom) as System.Collections.IList, ids,ui);
        }
        {
            // One event-driven seed at selection/scene commit, including
            // non-atom props and non-CUA skinned renderers (hands, toys, etc.).
            AddRigidList(Resources.FindObjectsOfTypeAll<MeshRenderer>(),ids,false);
            foreach(SkinnedMeshRenderer renderer in Resources.FindObjectsOfTypeAll<SkinnedMeshRenderer>())RegisterPathSkin(renderer);
            if(pathRegistryActive)foreach(Light light in Resources.FindObjectsOfTypeAll<Light>())if(light!=null&&light.gameObject.scene.IsValid())pathLights.Add(light);
            if(pathRegistryActive)foreach(ReflectionProbe probe in Resources.FindObjectsOfTypeAll<ReflectionProbe>())if(probe!=null&&probe.gameObject.scene.IsValid())pathProbes.Add(probe);
        }
        rigidRebuilds++;
    }

    private static void AddRigidList(System.Collections.IList renderers, Dictionary<int, bool> ids,bool ui)
    {
        if (renderers == null) return;
        for (int i = 0; i < renderers.Count; i++)
        {
            MeshRenderer r = renderers[i] as MeshRenderer;
            if (r == null || !r.gameObject.scene.IsValid() || (hRender!=null && r.GetComponent(hRender)!=null)) continue;
            int id = r.GetInstanceID();
            if (ids.ContainsKey(id)) continue;
            ids[id] = true;
            MeshFilter f = r.GetComponent<MeshFilter>();
            if (f == null) continue;
            RigidEntry e = new RigidEntry();
            e.renderer = r;
            e.filter = f;
            e.transform = r.transform;
            e.materials = r.sharedMaterials;
            e.id = id;
            // Parent atom registries may also contain linked child atoms.
            // Role belongs to the nearest atom, not whichever list saw it first.
            Component owner=r.GetComponentInParent(aType.DeclaringType);
            e.uiRole=(owner!=null?EditorOnlyAtom(aType.GetValue(owner) as string):ui) || r.GetComponentInParent<Canvas>()!=null;
            rigids.Add(e);
        }
    }
    private static bool EditorOnlyAtom(string type) {
        return type=="CoreControl" || type=="PlayerNavigationPanel" || type=="VRController" || type=="WindowCamera" || type=="InvisibleLight" || type=="Empty";
    }

    private static readonly PropertyInfo staticBatchRoot=typeof(Renderer).GetProperty("staticBatchRootTransform",BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance);

    // ---- lights ------------------------------------------------------------------------------------------------

    private static void LookForLamps(int frame)
    {
        try
        {
            if (dlType == null)
            {
                if (frame < dlNextLook) return;
                dlNextLook = frame + CacheFrames;
                foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
                {
                    Type t = null;
                    try { t = a.GetType("VaMDisplayLighting", false); }
                    catch (Exception) { }  // dynamic (scene script) assemblies
                    if (t == null) continue;
                    dlType = t;
                    dlInstance = t.GetField("Instance", BindingFlags.Public | BindingFlags.Static);
                    dlRig = t.GetField("rig", BindingFlags.NonPublic | BindingFlags.Instance);
                    if (dlInstance == null || dlRig == null) { lampError = "Display Lighting without Instance / rig"; dlRig = null; }
                    break;
                }
                if (dlType == null) return;
            }
            if (dlRig == null) return;
            object dl = dlInstance.GetValue(null);
            GameObject rig = dl != null ? dlRig.GetValue(dl) as GameObject : null;
            if (ReferenceEquals(rig, dlRigSeen) && (rig != null || dlLamps.Count == 0)) return;
            dlRigSeen = rig;
            dlLamps.Clear();
            if (rig != null) dlLamps.AddRange(rig.GetComponentsInChildren<Light>(true));  // the rig's own few lamps, once per rig
        }
        catch (Exception e)
        {
            lampError = e.Message;
            dlRig = null;
        }
    }

    // Unity's Light.cullingMask (Light Atom Excluder moves atoms to a layer the
    // light leaves out): an excluded object is neither lit by that light nor
    // shadows it. Each physical layer some light leaves out becomes a link
    // class, one of the five free TLAS instance-mask bits; the records carry
    // the class bit (draws, hairs) and the classes a light leaves out. Both
    // routes (0.22.60): PT, and the installed route's atlas shadows and bounce
    // (Unity's own passes already keep the cullingMask). Past five classes both
    // routes light the rest from every light (0.22.62: PT used to refuse the scene).
    private static readonly uint[] linkOfLayer = new uint[32];
    private static uint linkedLayers;
    private static readonly uint[] lightExcluded = new uint[MaxLights];  // status
    private static unsafe void LinkLights(byte* draws, int n, byte* lights, int nl, byte* hairRecords, int nh)
    {
        uint split = 0;
        for (int i = 0; i < nl; i++) split |= pathPhysicalLayers & ~*(uint*)(lights + i * LightBytes + 84);
        int classes = 0;
        for (int layer = 0; layer < 32; layer++)
        {
            linkOfLayer[layer] = 0u;
            if ((split >> layer & 1u) == 0u) continue;
            if (classes < 5) linkOfLayer[layer] = 1u << classes;
            classes++;
        }
        linkedLayers = split;
        if (classes > 5 && pathRegistryActive && PathNoteNew(classes,10)) PathNote(classes + " light-linked layers: past five, a layer is lit by every light");
        for (int i = 0; i < n; i++) { uint* at = (uint*)(draws + i * DrawBytes + 588); *at = *at < 32u ? linkOfLayer[*at] : 0u; }
        for (int i = 0; i < nh; i++) { uint* at = (uint*)(hairRecords + i * HairBytes + 224); *at = *at < 32u ? linkOfLayer[*at] : 0u; }
        for (int i = 0; i < nl; i++)
        {
            uint* at = (uint*)(lights + i * LightBytes + 84);
            uint lit = *at, excluded = 0u;
            for (int layer = 0; layer < 32; layer++)
                if (linkOfLayer[layer] != 0u && (lit >> layer & 1u) == 0u) excluded |= linkOfLayer[layer];
            *at = excluded;lightExcluded[i] = excluded;
        }
    }

    private static readonly Dictionary<int, bool> lightSeen = new Dictionary<int, bool>();
    // diagnostics: per light of the last frame, Unity's type, shadows, shadowNearPlane, shadowResolution, shadowBias, spotAngle,
    // shadowStrength
    private static readonly float[] lightDiag = new float[MaxLights * 7];
    // PT point/spot emitter: angular radius (radians) and radius (m). The review sets them through the test agent to
    // compare sizes on one frame.
    internal static float PtLightAngular = .1396f, PtLightRadius = 0f;
    private static unsafe int AddLights(byte* out0, int frame)
    {
        LookForLamps(frame);
        lightSeen.Clear();
        int n = 0;
        lastShadowed = 0;
        bool linearRule = GraphicsSettings.lightsUseLinearIntensity;
        for (int pass = 0; pass < (pathRegistryActive?3:2); pass++)
        {
            List<Light> list = pass == 0 ? vamLights : pass==1?dlLamps:pathLights;
            if (list == null) continue;
            for (int i = 0; i < list.Count && n < MaxLights; i++)
            {
                Light l = list[i];
                if (l == null || !l.isActiveAndEnabled || l.intensity <= 0f) continue;
                if(pathRegistryActive) {
                    uint layers=(uint)l.cullingMask;
                    if((layers&pathPhysicalLayers)==0)continue;
                    if(l.cookie!=null && PathNoteNew(l.GetInstanceID(),8))PathNote(l.name+": light cookie not applied in PT");
                }
                int id = l.GetInstanceID();
                if (lightSeen.ContainsKey(id)) continue;
                lightSeen[id] = true;
                uint type = l.type == LightType.Directional ? 0u : l.type == LightType.Spot ? 1u : l.type == LightType.Point ? 2u : 99u;
                if (type == 99u) {
                    if(pathRegistryActive && PathNoteNew(id,9))PathNote(l.name+" / "+l.type+": not a realtime light in Unity, not in PT either");
                    continue;
                }
                Transform t = l.transform;
                Vector3 p = t.position, d = t.forward;
                Color c = linearRule ? l.color.linear * l.intensity : (l.color * l.intensity).linear;
                bool shadows = l.shadows != LightShadows.None;
                byte* r = out0 + n * LightBytes;
                *(long*)(r + 0) = id;
                *(uint*)(r + 8) = type;
                // API 43, PT: Unity's strength and near plane; bit 28: no shadow in Unity (no shadows, or a vertex light)
                *(uint*)(r + 12) = (shadows ? 1u : 0u) | (uint)Mathf.Clamp(Mathf.RoundToInt(l.shadowStrength*255f),0,255)<<8 |
                    (uint)Mathf.Clamp(Mathf.RoundToInt(l.shadowNearPlane*1024f),0,4095)<<16 |
                    (!shadows || l.renderMode==LightRenderMode.ForceVertex ? 1u<<28 : 0u);
                float* f = (float*)(r + 16);
                f[0] = p.x; f[1] = p.y; f[2] = p.z;
                f[3] = d.x; f[4] = d.y; f[5] = d.z;
                f[6] = c.r; f[7] = c.g; f[8] = c.b;
                f[9] = l.range;
                f[10] = Mathf.Cos(0.5f * l.spotAngle * Mathf.Deg2Rad);
                // Unity has no emitter size (VaM draws point/spot shadows Hard).
                // To VaM's authors a point/spot light is a rendering effect,
                // placed and ranged for the look (Unity's falloff follows the
                // range, not the distance), not a fixture of some size: PT gives
                // it one apparent size from wherever it lights, PtLightAngular
                // of angular radius (0.22.56-0.22.63: a 3 cm emitter, hard from
                // 2 m). A directional light is the sun: 0.27 deg (VaM draws it Soft).
                f[11] = pathRegistryActive ? PtLightRadius : .25f;
                f[12] = pathRegistryActive ? (type == 0u ? .00465f : PtLightAngular) : .0873f;
                if (type == 0u) { f[13] = -d.x; f[14] = -d.y; f[15] = -d.z; f[16] = 0f; }
                else { f[13] = p.x; f[14] = p.y; f[15] = p.z; f[16] = 1f; }
                *(uint*)(r + 84) = (uint)l.cullingMask;  // LinkLights turns it into the excluded link classes
                if (shadows) lastShadowed++;
                int g = n * 7;
                lightDiag[g] = type; lightDiag[g + 1] = (int)l.shadows; lightDiag[g + 2] = l.shadowNearPlane;
                lightDiag[g + 3] = (int)l.shadowResolution; lightDiag[g + 4] = l.shadowBias; lightDiag[g + 5] = l.spotAngle;
                lightDiag[g + 6] = l.shadowStrength;
                n++;
            }
        }
        return n;
    }

    // ---- hair --------------------------------------------------------------------------------------------------

    private static int HairValue(PropertyInfo p, object world)
    {
        object v = p.GetValue(world, null);
        return v == null ? 0 : (int)hIntValue.GetValue(v, null);
    }

    private static unsafe int AddHairs(byte* out0, int mask, int frame)
    {
        if (hRender == null) return 0;
        int n = 0;
        for (int i = 0; i < hairs.Count && n < MaxHairs; i++)
        {
            Component c = hairs[i];
            if (c == null) continue;
            try
            {
                GameObject go = c.gameObject;
                if (!go.activeInHierarchy || (mask & (1 << go.layer)) == 0) continue;
                Renderer r = hRend.GetValue(c) as Renderer;
                if (r == null || !r.enabled || (!VaMSkinTransmission.PathSceneActive && r.shadowCastingMode == UnityEngine.Rendering.ShadowCastingMode.Off)) continue;
                object data = hData.GetValue(c);
                object gpu = data == null ? null : hParticles.GetValue(data, null);
                ComputeBuffer particles = gpu == null ? null : hComputeBuffer.GetValue(gpu, null) as ComputeBuffer;
                if (particles == null || particles.stride != HairStride) continue;
                int id = c.GetInstanceID();
                Component physics;
                if (!hairPhysics.TryGetValue(id, out physics) || physics == null)
                {
                    physics = c.GetComponent(hPhysics);
                    hairPhysics[id] = physics;
                }
                object world = physics == null ? null : hWorld.GetValue(physics);
                if (world == null) continue;
                // GPUTools' CSTesselateWithNormals writes TessSegments points per guide strand, the guides first (point =
                // guide x TessSegments + k) and its count runs on past them: the guides' points only. (0.22.0 read
                // Segments x TessSegments points per strand: 15 guides joined tip to root.)
                int tess = HairValue(hTess, world);
                HairShape shape = ShapeFor(id, c, data, particles);
                int count = Math.Min(HairValue(hCount, world), particles.count);
                if (shape.guides > 0 && tess > 0) count = Math.Min(count, shape.guides * tess);
                float width = ShadowWidth(r, shape);
                if (count < 2 || !(width > 0f)) continue;
                // The display shader shortens the original guides with _Length. In split hairstyles a small
                // detail can use only a few percent of them; the hidden remainder must not cast a full-size shadow.
                Material material = r.sharedMaterial;
                Vector4 length = material.HasProperty("_Length") ? material.GetVector("_Length") : Vector4.one;
                length.x = Mathf.Clamp01(length.x); length.y = Mathf.Clamp01(length.y); length.z = Mathf.Clamp01(length.z);
                if (!(Mathf.Max(length.x, Mathf.Max(length.y, length.z)) > 0f)) continue;
                BufferEntry b = BufferFor(particles, frame);
                if (b == null) continue;
                Bounds bounds = r.bounds;
                Vector3 lo = bounds.min, hi = bounds.max;
                byte* d = out0 + n * HairBytes;
                *(long*)(d + 0) = id;
                *(long*)(d + 8) = b.pointer.ToInt64();
                *(uint*)(d + 16) = (uint)count;
                *(uint*)(d + 20) = tess > 0 ? (uint)tess : 0u;   // points per guide strand (0: split where RootIndex changes)
                float* f = (float*)(d + 24);
                f[0] = width;
                f[1] = lo.x; f[2] = lo.y; f[3] = lo.z;
                f[4] = hi.x; f[5] = hi.y; f[6] = hi.z;
                f[7] = length.x; f[8] = length.y; f[9] = length.z;
                for(int clear=64;clear<HairBytes;clear+=4)*(uint*)(d+clear)=0;
                *(uint*)(d+224)=(uint)go.layer;pathPhysicalLayers|=1u<<go.layer;  // LinkLights: the link class
                if(VaMSkinTransmission.PathSceneActive) {
                    Mesh mesh=hMesh.GetValue(c) as Mesh;MeshEntry me=mesh==null?null:MeshFor(mesh,frame,false);
                    bool styling=(bool)hStyleMode.GetValue(data,null);
                    object bg=(styling?hBaryFixed:hBary).GetValue(data,null);
                    ComputeBuffer bary=bg==null?null:hBaryBuffer.GetValue(bg,null) as ComputeBuffer;
                    BufferEntry be=bary==null?null:BufferFor(bary,frame);
                    if(me==null||me.subMeshCount==0||be==null)continue; // GPUTools still building it (a new person): a later frame
                    // version 21: flags bit 0, the eye camera draws it at strand width 0 (EyeRendered)
                    WatchEye(material);
                    if(eyeHidden.Contains(material.GetInstanceID())){*(uint*)(d+228)=1u;lastCameraHiddenHairs++;}
                    byte* p=d+64;
                    *(long*)p=id;*(long*)(p+8)=b.pointer.ToInt64();*(long*)(p+16)=be.pointer.ToInt64();*(long*)(p+24)=me.indexBuffer.ToInt64();
                    Texture noise=Tex(material,"_NoiseTex");
                    Texture color=Tex(material,"_ColorTex");
                    *(long*)(p+32)=TexturePtr(noise??Texture2D.whiteTexture).ToInt64();
                    *(long*)(p+40)=TexturePtr(color??Texture2D.blackTexture).ToInt64(); // shift saturate(sample-.5) = 0
                    *(uint*)(p+48)=me.count[0];*(uint*)(p+52)=me.index32?1u:0u;*(uint*)(p+56)=me.start[0];*(int*)(p+60)=me.baseVertex[0];
                    Vector4 tf=material.GetVector("_TessFactor");float* v=(float*)(p+64);
                    for(int k0=0;k0<4;k0++){v[k0]=tf[k0];v[4+k0]=length[k0];}
                    v[8]=material.GetFloat("_MaxSpread");v[9]=material.GetFloat("_RandomBarycentric");v[10]=material.GetFloat("_StandWidth");v[11]=material.GetFloat("_Diffuse");
                    v[12]=material.GetFloat("_FresnelPower");v[13]=material.GetFloat("_FresnelAtten");v[14]=material.GetFloat("_SpecularShift");v[15]=material.GetFloat("_PrimarySpecular");
                    v[16]=material.GetFloat("_SecondarySpecular");v[17]=material.GetFloat("_RandomTexColorPower");v[18]=material.GetFloat("_RandomTexColorOffset");v[19]=material.GetFloat("_IBLFactor");
                    // GPUTools sets this hidden uniform directly (not a
                    // ShaderLab Color property). Read its actual value; a
                    // second sRGB conversion would erase dark authored shine.
                    Color specular=material.GetColor("_SpecularColor");
                    v[20]=specular.r;v[21]=specular.g;v[22]=specular.b;v[23]=1;
                }
                int k = n * 5;
                hairInfo[k] = shape.guides; hairInfo[k + 1] = shape.patches; hairInfo[k + 2] = tess; hairInfo[k + 3] = count; hairInfo[k + 4] = width;
                n++;
            }
            catch (Exception e)
            {
                hairError = e.Message;
            }
        }
        return n;
    }

    // A hair item's guide strands (HairDataFacade.Size.x) and patches (its mesh: 3 guides each), read again when its
    // particle buffer changes (a rebuild).
    private static HairShape ShapeFor(int id, Component c, object data, ComputeBuffer particles)
    {
        HairShape s;
        if (hairShapes.TryGetValue(id, out s) && ReferenceEquals(s.buffer, particles)) return s;
        s = new HairShape();
        s.buffer = particles;
        s.guides = (int)((Vector4)hSize.GetValue(data, null)).x;
        Mesh mesh = hMesh.GetValue(c) as Mesh;
        s.patches = mesh != null ? (int)(mesh.GetIndexCount(0) / 3) : 0;
        hairShapes[id] = s;
        return s;
    }

    // 0.22.1: the shadow width RtCore's hair volume takes, per guide strand: the projected width of the rendered strands
    // it stands for, as the renderer draws them this frame - patches x _TessFactor.x lines over the guides, each a
    // camera-facing ribbon 2 x _StandWidth wide (GPUTools puts its WorldScale in) - x pi/4 for random orientation.
    // (0.22.0 passed StandWidth alone: the guides only, some 20x too thin a volume - hair cast almost no shadow, user
    // 2026-10-22.) 0: no shadow.
    private static float ShadowWidth(Renderer r, HairShape s)
    {
        Material m = r.sharedMaterial;
        if (m == null || s.guides <= 0 || s.patches <= 0 || !m.HasProperty(hTessFactorId) || !m.HasProperty(hStandWidthId)) return 0f;
        float lines = m.GetVector(hTessFactorId).x;
        return 2f * m.GetFloat(hStandWidthId) * 0.7853982f * lines * s.patches / s.guides;
    }

    // ---- ambient: ShadeSH9's constants -------------------------------------------------------------------------

    // ShadeSH9(n) = SHA . (n, 1) + SHB . (xy, yz, zz, zx) + SHC (xx - yy): nine terms per channel, fitted by least squares
    // to the ambient sampled in 26 directions (the matrix once)
    private static void SetupAmbientFit()
    {
        List<Vector3> dirs = new List<Vector3>();
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                for (int z = -1; z <= 1; z++)
                    if (x != 0 || y != 0 || z != 0) dirs.Add(new Vector3(x, y, z).normalized);
        shDirs = dirs.ToArray();
        int N = shDirs.Length;
        double[,] a = new double[N, 9];
        for (int i = 0; i < N; i++)
        {
            Vector3 v = shDirs[i];
            double[] b = { v.x, v.y, v.z, 1.0, v.x * v.y, v.y * v.z, v.z * v.z, v.z * v.x, v.x * v.x - v.y * v.y };
            for (int k = 0; k < 9; k++) a[i, k] = b[k];
        }
        // (A^T A)^-1 A^T by Gauss-Jordan on [A^T A | A^T]
        double[,] m = new double[9, 9 + N];
        for (int r = 0; r < 9; r++)
        {
            for (int c = 0; c < 9; c++)
            {
                double s = 0;
                for (int i = 0; i < N; i++) s += a[i, r] * a[i, c];
                m[r, c] = s;
            }
            for (int i = 0; i < N; i++) m[r, 9 + i] = a[i, r];
        }
        for (int c = 0; c < 9; c++)
        {
            int piv = c;
            for (int r = c + 1; r < 9; r++) if (Math.Abs(m[r, c]) > Math.Abs(m[piv, c])) piv = r;
            for (int k = 0; k < 9 + N; k++) { double t = m[c, k]; m[c, k] = m[piv, k]; m[piv, k] = t; }
            double d = m[c, c];
            for (int k = 0; k < 9 + N; k++) m[c, k] /= d;
            for (int r = 0; r < 9; r++)
            {
                if (r == c) continue;
                double f = m[r, c];
                if (f == 0) continue;
                for (int k = 0; k < 9 + N; k++) m[r, k] -= f * m[c, k];
            }
        }
        shFit = new float[9, N];
        for (int r = 0; r < 9; r++)
            for (int i = 0; i < N; i++) shFit[r, i] = (float)m[r, 9 + i];
    }

    private static Color[] shSamples;
    private static readonly float[] ambientCoefficients=new float[9];
    private static readonly Vector4[] currentSkySh=new Vector4[9];
    private static readonly float[] skyCoefficients=new float[9];
    private static readonly float[,] skyQuadratic=new float[3,3],skyRotated=new float[3,3];
    private static void UpdateAmbient(int frame)
    {
        shFrame = frame;
        int N = shDirs.Length;
        if (shSamples == null) shSamples = new Color[N];
        Color[] v = shSamples;
        AmbientMode mode = RenderSettings.ambientMode;
        if (mode == AmbientMode.Custom)
        {
            SphericalHarmonicsL2 probe = RenderSettings.ambientProbe;  // what ShadeSH9 evaluates (Display Lighting sets it)
            probe.Evaluate(shDirs, v);
        }
        else if (mode == AmbientMode.Trilight)
        {
            Color sky = RenderSettings.ambientSkyColor.linear, eq = RenderSettings.ambientEquatorColor.linear, gr = RenderSettings.ambientGroundColor.linear;
            for (int i = 0; i < N; i++) v[i] = shDirs[i].y >= 0 ? Color.Lerp(eq, sky, shDirs[i].y) : Color.Lerp(eq, gr, -shDirs[i].y);
        }
        else
        {
            Color c = RenderSettings.ambientLight.linear;  // Flat; VaM's Skybox mode has no skybox: the flat colour too
            for (int i = 0; i < N; i++) v[i] = c;
        }
        for (int ch = 0; ch < 3; ch++)
        {
            float[] k = ambientCoefficients;
            for (int r = 0; r < 9; r++)
            {
                float s = 0;
                for (int i = 0; i < N; i++) s += shFit[r, i] * (ch == 0 ? v[i].r : ch == 1 ? v[i].g : v[i].b);
                k[r] = s;
            }
            // SHA (x, y, z, 1), SHB (xy, yz, zz, zx), SHC[ch] (xx - yy)
            for (int q = 0; q < 4; q++) sh[4 * ch + q] = k[q];
            for (int q = 0; q < 4; q++) sh[12 + 4 * ch + q] = k[4 + q];
            sh[24 + ch] = k[8];
        }
        for (int i = 0; i < 27; i++) if(float.IsNaN(sh[i]) || float.IsInfinity(sh[i]))sh[i]=0;
        sh[27] = 0f;
        UpdateSkyAmbient();
    }

    // Version 6: Skyshop's diffuse SH as VaM's shaders read it (its globals: _SH0.._SH8 evaluated on the sky-rotated normal
    // m = _SkyMatrix^T n, x _ExposureIBL.x; skin shaders also scale Unity's by _ExposureLM.x), rotated into world
    // directions for RtCore (rtcore.h Frame::skySh): per channel (L1 xyz, L0), (xy, yz, 3zz - 1, zx), then (xx - yy)
    private static readonly int[] skyShIds = new int[9];
    private static int exposureIblId, exposureLmId, skyMatrixId;
    private static void UpdateSkyAmbient()
    {
        if (skyMatrixId == 0)
        {
            for (int i = 0; i < 9; i++) skyShIds[i] = Shader.PropertyToID("_SH" + i);
            exposureIblId = Shader.PropertyToID("_ExposureIBL");
            exposureLmId = Shader.PropertyToID("_ExposureLM");
            skyMatrixId = Shader.PropertyToID("_SkyMatrix");
        }
        Vector4 ibl = Shader.GetGlobalVector(exposureIblId), lm = Shader.GetGlobalVector(exposureLmId);
        // Skyshop sets both whenever it applies a sky; all zero: never set (no Skyshop), Unity's ambient as it is
        bool noSky = ibl == Vector4.zero && lm == Vector4.zero;
        skySh[27] = noSky ? 1f : lm.x;
        iblSpecular = noSky ? 0f : ibl.y;
        iblDiffuse = noSky ? 0f : ibl.x;
        iblExposure = noSky ? 1f : ibl.w;
        Matrix4x4 r = Shader.GetGlobalMatrix(skyMatrixId);
        pathWorldToSky = noSky ? Matrix4x4.identity : r.transpose;
        Texture lighting = Shader.GetGlobalTexture("_SpecCubeIBL"), background = Shader.GetGlobalTexture("_SkyCubeIBL");
        if (lighting != pathLightingCube) {
            pathLightingCube = lighting;
            pathLightingPointer = lighting != null ? lighting.GetNativeTexturePtr() : IntPtr.Zero;
        }
        if (background != pathBackgroundCube) {
            pathBackgroundCube = background;
            skyshopBackgroundPointer = background != null ? background.GetNativeTexturePtr() : IntPtr.Zero;
        }
        pathBackgroundPointer=skyshopBackgroundPointer;
        pathBackgroundScale = noSky || ibl.w <= 0 ? 0f : ibl.z / ibl.w;
        for(int i=0;i<9;i++)currentSkySh[i]=Shader.GetGlobalVector(skyShIds[i]);
        for (int ch = 0; ch < 3; ch++)
        {
            float[] c = skyCoefficients;
            for (int i = 0; i < 9; i++) c[i] = currentSkySh[i][ch] * ibl.x;
            // L1: c . m = (R c) . n with c = (SH3, SH1, SH2)
            Vector3 l1 = r.MultiplyVector(new Vector3(c[3], c[1], c[2]));
            // L2: m^T Q m (Q traceless: 3zz - 1 = 2zz - xx - yy on the sphere) = n^T (R Q R^T) n
            float[,] q=skyQuadratic,w=skyRotated;
            q[0,0]=c[8]-c[6];q[0,1]=q[1,0]=c[4]/2;q[0,2]=q[2,0]=c[7]/2;
            q[1,1]=-c[8]-c[6];q[1,2]=q[2,1]=c[5]/2;q[2,2]=2*c[6];
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++)
                {
                    float sum = 0;
                    for (int i = 0; i < 3; i++)
                        for (int j = 0; j < 3; j++) sum += r[a, i] * q[i, j] * r[b, j];
                    w[a, b] = sum;
                }
            skySh[4*ch]=l1.x;skySh[4*ch+1]=l1.y;skySh[4*ch+2]=l1.z;skySh[4*ch+3]=c[0];
            skySh[12+4*ch]=2*w[0,1];skySh[13+4*ch]=2*w[1,2];skySh[14+4*ch]=w[2,2]/2;skySh[15+4*ch]=2*w[0,2];
            skySh[24 + ch] = (w[0, 0] - w[1, 1]) / 2;
        }
        for (int i = 0; i < 28; i++) if (float.IsNaN(skySh[i]) || float.IsInfinity(skySh[i])) skySh[i] = i == 27 ? 1f : 0f;
        UpdateBackground(noSky, r);
    }

    // ---- visible background (record version 19) ----------------------------------------------------------------
    // VaM's visible sky is a Skyshop mesh under SkyshopLightController.skyContainer: showSkybox toggles the container,
    // skyboxYAngle rotates it (not the lighting sky). Marmoset/Skybox IBL samples _SkyCubeIBL at _SkyMatrix^T x the mesh's
    // object-space normal, Skydome IBL at its object-space position (their disassembly). With no sky drawn, the camera
    // clears to its background colour: PT shows that too instead of a sky the author hid.
    private static FieldInfo skySingletonField;
    private static FieldInfo[] skyContainerFields = new FieldInfo[0];
    private static bool skyReflected;
    private static Camera pathEyeCamera;
    private static readonly Dictionary<Transform, Renderer[]> skyRenderers = new Dictionary<Transform, Renderer[]>();
    private static readonly Dictionary<Mesh, float> skyNormalSign = new Dictionary<Mesh, float>();
    private static readonly float[] pathBackgroundToSky = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    private static Color pathBackgroundColor;
    private static Camera skyCaptureCamera;
    private static RenderTexture skyCaptureCube;
    private static IntPtr skyCapturePointer;
    private static Renderer skyCaptureRenderer;
    private static Material skyCaptureMaterial;
    private static Renderer skyCaptureCandidate;
    private static RigidEntry skyCaptureEntry,skyCaptureCandidateEntry;
    private static GameObject cameraStereoSkyProxy;
    private static Material skyCaptureCandidateMaterial;
    private static int skyCaptureCandidateQueue;
    private static long skyCaptureKey;
    private static bool capturedSkyActive;
    private static readonly string[] skyTextureNames={"_Tex","_MainTex","_FrontTex","_BackTex","_LeftTex","_RightTex","_UpTex","_DownTex"};
    private static readonly string[] skyFloatNames={"_Exposure","_Rotation","_Mapping","_ImageType","_Layout","_MirrorOnBack","_SunDisk","_SunSize","_SunSizeConvergence","_AtmosphereThickness"};
    private static readonly string[] skyColorNames={"_Tint","_SkyTint","_GroundColor"};
    internal static bool IsSkyCaptureCamera(Camera camera) { return camera!=null && camera==skyCaptureCamera; }
    private static bool SkyEntryHasMaterial(RigidEntry entry, Material mat) {
        if(entry==null || entry.renderer==null || !entry.renderer.enabled || !entry.renderer.gameObject.activeInHierarchy || entry.materials==null)return false;
        for(int i=0;i<entry.materials.Length;i++)if(entry.materials[i]==mat)return true;
        return false;
    }
    private static bool SkyNeedsCapture(Material mat) {
        if(mat==null || mat.shader==null)return false;
        string name=ShaderName(mat.shader);
        if(!name.StartsWith("Skybox/",StringComparison.Ordinal))return false;
        // The physical panorama adapter handles lat-long layouts, including stereo, directly.
        return (name!="Skybox/Panoramic" && name!="Skybox/PanoramicBeta") ||
            (mat.HasProperty("_Mapping") && mat.GetFloat("_Mapping")<.5f);
    }
    private static void SetCameraStereoSkyProxy(Material mat) {
        if(mat==null || pathEyeCamera==null) {
            if(cameraStereoSkyProxy!=null && cameraStereoSkyProxy.activeSelf) {cameraStereoSkyProxy.SetActive(false);rigidDirty=true;}
            return;
        }
        if(cameraStereoSkyProxy==null) {
            cameraStereoSkyProxy=GameObject.CreatePrimitive(PrimitiveType.Sphere);
            cameraStereoSkyProxy.name="PT camera stereo sky";cameraStereoSkyProxy.hideFlags=HideFlags.HideAndDontSave;
            Component collider=cameraStereoSkyProxy.GetComponent("SphereCollider");if(collider!=null)UnityEngine.Object.Destroy(collider);
            cameraStereoSkyProxy.transform.localScale=Vector3.one*16000f;
            rigidDirty=true;
        }
        if(!cameraStereoSkyProxy.activeSelf) {cameraStereoSkyProxy.SetActive(true);rigidDirty=true;}
        cameraStereoSkyProxy.transform.position=pathEyeCamera.transform.position;
        int mask=pathEyeCamera.cullingMask,layer=0;while(layer<31 && (mask&(1<<layer))==0)layer++;
        cameraStereoSkyProxy.layer=layer;
        MeshRenderer renderer=cameraStereoSkyProxy.GetComponent<MeshRenderer>();
        if(renderer.sharedMaterial!=mat)renderer.sharedMaterial=mat;
    }
    private static long SkyCaptureKey(Material mat) {
        long key=Mix(1469598103934665603L,mat.GetInstanceID());
        for(int i=0;i<skyTextureNames.Length;i++)if(mat.HasProperty(skyTextureNames[i])) {
            Texture t=mat.GetTexture(skyTextureNames[i]);
            key=Mix(key,t!=null?t.GetInstanceID():0);
            if(t!=null)key=Mix(key,t.updateCount);
        }
        for(int i=0;i<skyFloatNames.Length;i++)if(mat.HasProperty(skyFloatNames[i]))key=Mix(key,Bits(mat.GetFloat(skyFloatNames[i])));
        for(int i=0;i<skyColorNames.Length;i++)if(mat.HasProperty(skyColorNames[i]))key=MixST(key,mat.GetColor(skyColorNames[i]));
        if(mat.shader!=null && ShaderName(mat.shader)=="Skybox/Procedural" && RenderSettings.sun!=null) {
            key=MixST(key,RenderSettings.sun.transform.forward);
            key=MixST(key,RenderSettings.sun.color);
            key=Mix(key,Bits(RenderSettings.sun.intensity));
        }
        return key;
    }
    // Uses the already-maintained renderer registry. A disabled auxiliary camera renders only when the author changes
    // the sky material, so Unity itself evaluates cubemap, six-sided and procedural sky shaders and their HDR decode.
    internal static void UpdateSkyCapture() {
        if(!VaMSkinTransmission.PathRenderingActive) { SetCameraStereoSkyProxy(null);skyCaptureRenderer=null;skyCaptureEntry=null;skyCaptureMaterial=null;return; }
        Renderer selected=skyCaptureCandidate;Material mat=skyCaptureCandidateMaterial;
        RigidEntry entry=skyCaptureCandidateEntry;
        if(selected==null || !SkyEntryHasMaterial(entry,mat)) {selected=null;entry=null;mat=null;}
        if(mat==null && pathEyeCamera!=null && pathEyeCamera.clearFlags==CameraClearFlags.Skybox) {
            Skybox own=pathEyeCamera.GetComponent<Skybox>();
            Material m=own!=null && own.enabled && own.material!=null?own.material:RenderSettings.skybox;
            if(m!=null && m.shader!=null && ShaderName(m.shader).StartsWith("Skybox/",StringComparison.Ordinal)) {
                if(!SkyNeedsCapture(m) && m.HasProperty("_Layout") && m.GetFloat("_Layout")>=.5f) {
                    SetCameraStereoSkyProxy(m);skyCaptureRenderer=null;skyCaptureEntry=null;skyCaptureMaterial=null;return;
                }
                mat=m;
            }
        }
        SetCameraStereoSkyProxy(null);
        skyCaptureRenderer=selected;skyCaptureEntry=entry;
        if(mat==null) {skyCaptureMaterial=null;return;}
        long key=SkyCaptureKey(mat);
        if(mat==skyCaptureMaterial && key==skyCaptureKey && skyCapturePointer!=IntPtr.Zero)return;
        if(skyCaptureCamera==null) {
            GameObject go=new GameObject("PT sky capture");go.hideFlags=HideFlags.HideAndDontSave;
            skyCaptureCamera=go.AddComponent<Camera>();skyCaptureCamera.enabled=false;skyCaptureCamera.cullingMask=0;
            skyCaptureCamera.clearFlags=CameraClearFlags.Skybox;skyCaptureCamera.allowHDR=true;
            go.AddComponent<Skybox>();
        }
        if(skyCaptureCube==null) {
            skyCaptureCube=new RenderTexture(2048,2048,16,RenderTextureFormat.ARGBHalf,RenderTextureReadWrite.Linear);
            skyCaptureCube.hideFlags=HideFlags.HideAndDontSave;skyCaptureCube.dimension=TextureDimension.Cube;
            skyCaptureCube.useMipMap=false;skyCaptureCube.Create();skyCapturePointer=skyCaptureCube.GetNativeTexturePtr();
        }
        skyCaptureCamera.GetComponent<Skybox>().material=mat;
        if(skyCaptureCamera.RenderToCubemap(skyCaptureCube)) {skyCaptureMaterial=mat;skyCaptureKey=key;}
        else {skyCaptureMaterial=null;skyCaptureKey=0;}
    }
    private static string pathBackgroundSource = "none";
    // 0.23.1: the status text of the background is built when its source changes, not every frame
    private static UnityEngine.Object backgroundSourceOf;private static int backgroundSourceKind;
    private static bool BackgroundSourceChanged(UnityEngine.Object of,int kind) {
        if(kind==backgroundSourceKind && ReferenceEquals(of,backgroundSourceOf))return false;
        backgroundSourceOf=of;backgroundSourceKind=kind;return true;
    }

    private static Renderer VisibleSkyRenderer()
    {
        if (!skyReflected)
        {
            skyReflected = true;
            Type t = Type.GetType("SkyshopLightController, Assembly-CSharp", false);
            if (t != null)
            {
                const BindingFlags inst = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
                skySingletonField = t.GetField("singleton", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
                skyContainerFields = new[] { t.GetField("skyContainer", inst), t.GetField("skyContainer2", inst), t.GetField("overlaySkyContainer", inst) };
            }
        }
        object controller = skySingletonField != null ? skySingletonField.GetValue(null) : null;
        if (controller == null) return null;
        foreach (FieldInfo f in skyContainerFields)
        {
            Transform root = f != null ? f.GetValue(controller) as Transform : null;
            if (root == null || !root.gameObject.activeInHierarchy) continue;
            Renderer[] list;
            if (!skyRenderers.TryGetValue(root, out list)) { list = root.GetComponentsInChildren<Renderer>(true); skyRenderers[root] = list; }
            foreach (Renderer r in list)
            {
                if (r == null || !r.enabled || !r.gameObject.activeInHierarchy) continue;
                Material m = r.sharedMaterial;
                if (m != null && ShaderName(m.shader).StartsWith("Marmoset/Sky", StringComparison.Ordinal)) return r;
            }
        }
        return null;
    }

    // +1: the sky mesh's normals point outward (the sampled direction is the view direction), -1: inward
    private static float SkyNormalSign(Renderer r)
    {
        MeshFilter filter = r.GetComponent<MeshFilter>();
        Mesh mesh = filter != null ? filter.sharedMesh : null;
        if (mesh == null) return 1f;
        float s;
        if (skyNormalSign.TryGetValue(mesh, out s)) return s;
        s = 1f;
        if (mesh.isReadable)
        {
            Vector3[] v = mesh.vertices, n = mesh.normals;
            Vector3 c = mesh.bounds.center;
            float sum = 0f;
            for (int i = 0; i < v.Length && i < n.Length; i += Math.Max(1, v.Length / 64)) sum += Vector3.Dot(n[i], v[i] - c);
            if (sum < 0f) s = -1f;
        }
        skyNormalSign[mesh] = s;
        return s;
    }

    private static void UpdateBackground(bool noSky, Matrix4x4 skyMatrix)
    {
        Matrix4x4 toSky = pathWorldToSky;
        pathBackgroundColor = Color.black;
        capturedSkyActive=false;
        if(skyCaptureMaterial!=null && skyCapturePointer!=IntPtr.Zero &&
            (skyCaptureRenderer==null || SkyEntryHasMaterial(skyCaptureEntry,skyCaptureMaterial))) {
            capturedSkyActive=true;pathBackgroundPointer=skyCapturePointer;
            pathBackgroundScale=1f/(iblExposure>0f?iblExposure:1f);
            toSky=skyCaptureRenderer!=null?skyCaptureRenderer.transform.worldToLocalMatrix:Matrix4x4.identity;
            if(BackgroundSourceChanged(skyCaptureMaterial,4))pathBackgroundSource="Unity sky capture "+skyCaptureMaterial.name;
            for(int row=0;row<3;row++)for(int col=0;col<4;col++)pathBackgroundToSky[row*4+col]=col<3?toSky[row,col]:0f;
            return;
        }
        Renderer sky = noSky ? null : VisibleSkyRenderer();
        if (sky != null)
        {
            Matrix4x4 local = sky.transform.worldToLocalMatrix;
            if (ShaderName(sky.sharedMaterial.shader).IndexOf("Skydome", StringComparison.Ordinal) >= 0) { toSky = local; if(BackgroundSourceChanged(sky,1))pathBackgroundSource = "skydome " + sky.name; }
            else
            {
                toSky = skyMatrix.transpose * local;
                if (SkyNormalSign(sky) < 0f) toSky = Matrix4x4.Scale(new Vector3(-1f, -1f, -1f)) * toSky;
                if(BackgroundSourceChanged(sky,2))pathBackgroundSource = "skybox " + sky.name;
            }
        }
        else
        {
            Camera cam = pathEyeCamera;
            Material box = RenderSettings.skybox;
            Skybox own = cam != null ? cam.GetComponent<Skybox>() : null;
            if (own != null && own.enabled && own.material != null) box = own.material;
            if (cam != null && cam.clearFlags == CameraClearFlags.Skybox && box != null)
                { if(BackgroundSourceChanged(box,3))pathBackgroundSource = "camera skybox " + (box.shader != null ? ShaderName(box.shader) : "?") + " (not translated: lighting sky)"; }
            else
            {
                pathBackgroundScale = 0f;
                if (cam != null && (cam.clearFlags == CameraClearFlags.SolidColor || cam.clearFlags == CameraClearFlags.Skybox))
                {
                    // the frame is multiplied by camera exposure afterwards; a clear colour is not
                    Color c = cam.backgroundColor.linear;
                    float e = iblExposure > 0f ? iblExposure : 1f;
                    pathBackgroundColor = new Color(c.r / e, c.g / e, c.b / e, 0f);
                    pathBackgroundSource = "clear colour";backgroundSourceKind=0;
                }
                else { pathBackgroundSource = cam == null ? "no eye camera" : "no clear";backgroundSourceKind=0; }
            }
        }
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 4; col++) pathBackgroundToSky[row * 4 + col] = col < 3 ? toSky[row, col] : 0f;
    }

    public static string PathBackground()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        Func<float, string> F = x => x.ToString("0.######", inv);
        Func<Vector3, string> V = v => "[" + F(v.x) + "," + F(v.y) + "," + F(v.z) + "]";
        Func<string, string> S = x => "\"" + (x ?? "").Replace("\\", "/").Replace("\"", "'") + "\"";
        StringBuilder sb = new StringBuilder("{\"source\":").Append(S(pathBackgroundSource));
        sb.Append(",\"scale\":").Append(F(pathBackgroundScale)).Append(",\"color\":[").Append(F(pathBackgroundColor.r)).Append(',')
          .Append(F(pathBackgroundColor.g)).Append(',').Append(F(pathBackgroundColor.b)).Append("],\"to_sky\":[");
        for (int i = 0; i < 12; i++) sb.Append(i > 0 ? "," : "").Append(F(pathBackgroundToSky[i]));
        sb.Append("],\"exposure\":").Append(F(iblExposure));
        Camera cam = pathEyeCamera;
        if (cam != null)
        {
            Color b = cam.backgroundColor;
            sb.Append(",\"camera\":{\"name\":").Append(S(cam.name)).Append(",\"clear\":").Append(S(cam.clearFlags.ToString()))
              .Append(",\"background\":[").Append(F(b.r)).Append(',').Append(F(b.g)).Append(',').Append(F(b.b)).Append("]}");
        }
        Material box = RenderSettings.skybox;
        sb.Append(",\"render_settings_skybox\":").Append(box == null ? "null" : S(box.name + " / " + (box.shader != null ? box.shader.name : "?")));
        object controller = skySingletonField != null ? skySingletonField.GetValue(null) : null;
        sb.Append(",\"containers\":[");
        bool firstRoot = true;
        foreach (FieldInfo f in skyContainerFields)
        {
            if (f == null) continue;
            Transform root = controller != null ? f.GetValue(controller) as Transform : null;
            if (!firstRoot) sb.Append(',');
            firstRoot = false;
            sb.Append("{\"field\":").Append(S(f.Name)).Append(",\"exists\":").Append(root != null ? 1 : 0);
            if (root != null)
            {
                sb.Append(",\"active\":").Append(root.gameObject.activeInHierarchy ? 1 : 0).Append(",\"euler\":").Append(V(root.eulerAngles)).Append(",\"renderers\":[");
                Renderer[] list = root.GetComponentsInChildren<Renderer>(true);
                for (int i = 0; i < list.Length; i++)
                {
                    Renderer r = list[i];
                    MeshFilter mf = r.GetComponent<MeshFilter>();
                    Mesh mesh = mf != null ? mf.sharedMesh : null;
                    Material m = r.sharedMaterial;
                    sb.Append(i > 0 ? "," : "").Append("{\"name\":").Append(S(r.name)).Append(",\"enabled\":").Append(r.enabled && r.gameObject.activeInHierarchy ? 1 : 0)
                      .Append(",\"shader\":").Append(S(m != null && m.shader != null ? m.shader.name : "")).Append(",\"mesh\":").Append(S(mesh != null ? mesh.name : ""))
                      .Append(",\"readable\":").Append(mesh != null && mesh.isReadable ? 1 : 0).Append(",\"normal_sign\":").Append(F(SkyNormalSign(r)))
                      .Append(",\"position\":").Append(V(r.transform.position)).Append(",\"euler\":").Append(V(r.transform.eulerAngles))
                      .Append(",\"scale\":").Append(V(r.transform.lossyScale)).Append(",\"bounds\":").Append(V(r.bounds.size)).Append('}');
                }
                sb.Append(']');
            }
            sb.Append('}');
        }
        return sb.Append("]}").ToString();
    }

    // The ambient a material's shader adds (rtcore.h EnvironmentShader, from VaM's shaders' disassembly): VaM's skin
    // family (Custom/Subsurface, Custom/Hair: skin, clothing, mesh hair), Skyshop's own Marmoset shaders, or Unity's
    // ambient alone (Standard, assets' own shaders)
    private static uint EnvironmentOf(string shaderName)
    {
        if (shaderName.StartsWith("Custom/Subsurface/", StringComparison.Ordinal) || shaderName.StartsWith("Custom/Hair/", StringComparison.Ordinal) ||
            Has(shaderName, "Skin IBL")) return EnvVamSkin;
        if (shaderName.StartsWith("Marmoset/", StringComparison.Ordinal)) return EnvMarmoset;
        return EnvUnity;
    }

    // ---- caches ----------------------------------------------------------------------------------------------

    private static readonly List<ComputeBuffer> staleBuffers = new List<ComputeBuffer>();
    private static readonly List<Mesh> staleMeshes = new List<Mesh>();
    private static readonly List<int> staleIds = new List<int>();
    private static readonly List<Texture> staleTextures = new List<Texture>();
    private static void Sweep(int frame)
    {
        if (frame - lastSweep < CacheFrames) return;
        lastSweep = frame;
        staleBuffers.Clear();
        foreach (KeyValuePair<ComputeBuffer, BufferEntry> p in buffers)
            if (frame - p.Value.lastUsed > CacheFrames) staleBuffers.Add(p.Key);
        for (int i = 0; i < staleBuffers.Count; i++) buffers.Remove(staleBuffers[i]);
        staleMeshes.Clear();
        foreach (KeyValuePair<Mesh, MeshEntry> p in meshes)
            if (frame - p.Value.lastUsed > CacheFrames || p.Key == null) staleMeshes.Add(p.Key);
        for (int i = 0; i < staleMeshes.Count; i++) { meshes.Remove(staleMeshes[i]); skinNoOcclude.Remove(staleMeshes[i]); }
        staleIds.Clear();
        foreach (KeyValuePair<int, MatInfo> p in materials)
            if (frame - p.Value.lastUsed > CacheFrames) staleIds.Add(p.Key);
        for (int i = 0; i < staleIds.Count; i++) materials.Remove(staleIds[i]);
        if (renderTypes.Count > 4 * materials.Count + 256) renderTypes.Clear();
        staleTextures.Clear();
        foreach (KeyValuePair<Texture, IntPtr> p in textures)
            if (p.Key == null) staleTextures.Add(p.Key);
        for (int i = 0; i < staleTextures.Count; i++){textures.Remove(staleTextures[i]);textureVersions.Remove(staleTextures[i]);liveTextureFrames.Remove(staleTextures[i]);}
        hairs.RemoveAll(delegate(Component c) { return c == null; });
        SweepSubmitted(frame);
        if (hairPhysics.Count > 4 * hairs.Count + 64) hairPhysics.Clear();
        if (decals.Count > 512) decals.Clear();
        if (vamLights != null) vamLights.RemoveAll(delegate(Light l) { return l == null; });
    }

    public static void ResetTiming()
    {
        collectSumMs = 0;
        collectSamples = 0;
        collectMaxTicks = 0;
    }

    // "name qQueue xCount; ..." over the clothing materials seen, most used first
    // Explicit test-agent query, never a periodic scene scan.
    public static string PathMaterials()
    {
        StringBuilder sb=new StringBuilder("[");bool first=true;
        foreach(Material m in Resources.FindObjectsOfTypeAll<Material>())
        {
            MatInfo e;if(m==null||!materials.TryGetValue(m.GetInstanceID(),out e))continue;
            if(!first)sb.Append(',');first=false;
            sb.Append("{\"key\":\"").Append(unchecked((ulong)e.key)).Append("\",\"name\":\"")
              .Append((m.name??"").Replace("\\","/").Replace("\"","'"))
              .Append("\",\"shader\":\"").Append((m.shader!=null?m.shader.name:"").Replace("\\","/").Replace("\"","'"))
              .Append("\",\"queue\":").Append(m.renderQueue)
              .Append(",\"state_known\":").Append(e.shaderStateKnown?"true":"false")
              .Append(",\"supported\":").Append(e.pathSupported?"true":"false")
              .Append(",\"unsupported_reason\":\"").Append(e.unsupported).Append("\",\"last_used_frame\":").Append(e.lastUsed)
              .Append(",\"kind\":").Append(e.kind).Append(",\"path_flags\":").Append(e.pathFlags)
              .Append(",\"path_mode\":").Append(e.mode).Append(",\"second_layer\":").Append(e.second!=IntPtr.Zero?"true":"false")
              .Append(",\"read_frame\":").Append(e.readFrame).Append(",\"reader\":\"").Append(e.readPath?"pt":"atlas").Append("\"")
              .Append(",\"author_watch\":").Append(e.watch?"true":"false").Append(",\"author_effect\":").Append(e.effect?"true":"false")
              .Append(",\"author_effect_only\":").Append(e.effectOnly?"true":"false")
              .Append(",\"observed\":").Append(observedStates.ContainsKey(e.id)?observedStates[e.id]:0u)
              .Append(",\"skin_key\":\"").Append(unchecked((ulong)(e.key|1))).Append("\",\"wrap_key\":\"").Append(unchecked((ulong)(e.key|2))).Append("\"}");
        }
        return sb.Append(']').ToString();
    }
    public static string PathMeshInventory()
    {
        StringBuilder sb=new StringBuilder("[");bool first=true;
        foreach(KeyValuePair<Mesh,MeshEntry> item in meshes) {
            if(item.Key==null)continue;
            MeshEntry e=item.Value;if(!first)sb.Append(',');first=false;
            sb.Append("{\"mesh\":\"").Append(item.Key.name.Replace("\\","/").Replace("\"","'"))
              .Append("\",\"id\":").Append(item.Key.GetInstanceID())
              .Append(",\"index_buffer\":\"").Append(e.indexBuffer.ToInt64().ToString("x"))
              .Append("\",\"last_used\":").Append(e.lastUsed).Append(",\"ranges\":[");
            for(int i=0;i<e.subMeshCount;i++) {
                if(i>0)sb.Append(',');sb.Append('[').Append(e.start[i]).Append(',').Append(e.count[i]).Append(',').Append(e.baseVertex[i]).Append(']');
            }
            sb.Append("]}");
        }
        return sb.Append(']').ToString();
    }
    private static string WrapShaderSummary()
    {
        Dictionary<string, int> n = new Dictionary<string, int>();
        foreach (string s in wrapShaders.Values)
        {
            int k;
            n.TryGetValue(s, out k);
            n[s] = k + 1;
        }
        List<KeyValuePair<string, int>> list = new List<KeyValuePair<string, int>>(n);
        list.Sort(delegate(KeyValuePair<string, int> a, KeyValuePair<string, int> b) { return b.Value.CompareTo(a.Value); });
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < list.Count && i < 24; i++)
            sb.Append(i > 0 ? "; " : "").Append(list[i].Key.Replace("\"", "'").Replace("\\", "/")).Append(" x").Append(list[i].Value);
        return sb.ToString();
    }

    // "type shadows near_m resolution bias angle; ..." of the lights last passed: Unity's own values (status)
    private static string LightShadowSummary()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        StringBuilder sb = new StringBuilder();
        for (int k = 0; k < lastLights; k++)
        {
            if (k > 0) sb.Append("; ");
            for (int j = 0; j < 7; j++) sb.Append(j > 0 ? " " : "").Append(lightDiag[k * 7 + j].ToString("0.####", inv));
        }
        return sb.ToString();
    }

    private static string LightLinkSummary()
    {
        StringBuilder sb = new StringBuilder();
        for (int k = 0; k < lastLights && k < MaxLights; k++) sb.Append(k > 0 ? " " : "").Append(lightExcluded[k]);
        return sb.ToString();
    }

    // "guides patches tess points width_mm; ..." of the hairs last passed (status)
    private static string HairSummary()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        StringBuilder sb = new StringBuilder();
        for (int k = 0; k < lastHairs; k++)
        {
            if (k > 0) sb.Append("; ");
            for (int j = 0; j < 4; j++) sb.Append((int)hairInfo[k * 5 + j]).Append(' ');
            sb.Append((hairInfo[k * 5 + 4] * 1000f).ToString("0.000", inv));
        }
        return sb.ToString();
    }

    // test (0.22.1, SimXR): every known hair - active, layer, renderer bounds, counts, the material's _TessFactor and
    // _StandWidth - and its particles read back (GetData stalls: test only): the box of the points AddHairs passes,
    // how many lie in the bounds, a root and a tip
    public static string HairProbe()
    {
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        StringBuilder sb = new StringBuilder("[");
        Func<Vector3, string> V = v => "[" + v.x.ToString("0.0000", inv) + "," + v.y.ToString("0.0000", inv) + "," + v.z.ToString("0.0000", inv) + "]";
        for (int i = 0; i < hairs.Count; i++)
        {
            Component c = hairs[i];
            if (c == null) continue;
            if (sb.Length > 1) sb.Append(",");
            try
            {
                Renderer r = hRend.GetValue(c) as Renderer;
                object data = hData.GetValue(c);
                object gpu = data == null ? null : hParticles.GetValue(data, null);
                ComputeBuffer particles = gpu == null ? null : hComputeBuffer.GetValue(gpu, null) as ComputeBuffer;
                Component physics = c.GetComponent(hPhysics);
                object world = physics == null ? null : hWorld.GetValue(physics);
                int tess = world == null ? -1 : HairValue(hTess, world), count = world == null ? -1 : HairValue(hCount, world);
                sb.Append("{\"name\":\"").Append(c.gameObject.name.Replace("\"", "'")).Append("\",\"active\":").Append(c.gameObject.activeInHierarchy ? 1 : 0)
                  .Append(",\"layer\":").Append(c.gameObject.layer).Append(",\"renderer\":").Append(r == null ? -1 : r.enabled ? 1 : 0)
                  .Append(",\"tess\":").Append(tess).Append(",\"count\":").Append(count);
                if (r != null)
                {
                    sb.Append(",\"bounds\":[").Append(V(r.bounds.min)).Append(",").Append(V(r.bounds.max)).Append("]");
                    Material m = r.sharedMaterial;
                    sb.Append(",\"casts_shadows\":").Append(r.shadowCastingMode == UnityEngine.Rendering.ShadowCastingMode.Off ? 0 : 1);
                    if (m != null && m.HasProperty("_Length")) sb.Append(",\"length\":").Append(V(m.GetVector("_Length")));
                    if (m != null && m.HasProperty(hTessFactorId)) sb.Append(",\"tess_factor\":").Append(V(m.GetVector(hTessFactorId)));
                    if (m != null && m.HasProperty(hStandWidthId)) sb.Append(",\"stand_width\":").Append(m.GetFloat(hStandWidthId).ToString("0.000000", inv));
                }
                if (particles != null && data != null)
                {
                    HairShape shape = ShapeFor(c.GetInstanceID(), c, data, particles);
                    sb.Append(",\"guides\":").Append(shape.guides).Append(",\"patches\":").Append(shape.patches).Append(",\"buffer\":").Append(particles.count)
                      .Append(",\"stride\":").Append(particles.stride);
                    if (particles.stride == HairStride && particles.count > 0)
                    {
                        float[] f = new float[particles.count * (HairStride / 4)];
                        particles.GetData(f);
                        int n = Math.Min(particles.count, count > 0 ? count : particles.count);
                        if (shape.guides > 0 && tess > 0) n = Math.Min(n, shape.guides * tess);
                        Vector3 lo = Vector3.one * 1e9f, hi = -lo;
                        int inside = 0;
                        for (int k = 0; k < n; k++)
                        {
                            Vector3 p = new Vector3(f[k * 14], f[k * 14 + 1], f[k * 14 + 2]);
                            lo = Vector3.Min(lo, p); hi = Vector3.Max(hi, p);
                            if (r != null && r.bounds.Contains(p)) inside++;
                        }
                        sb.Append(",\"points\":").Append(n).Append(",\"inside\":").Append(inside).Append(",\"box\":[").Append(V(lo)).Append(",").Append(V(hi)).Append("]")
                          .Append(",\"p0\":").Append(V(new Vector3(f[0], f[1], f[2])));
                        if (tess > 1 && n >= tess) sb.Append(",\"tip0\":").Append(V(new Vector3(f[(tess - 1) * 14], f[(tess - 1) * 14 + 1], f[(tess - 1) * 14 + 2])));
                    }
                }
                sb.Append("}");
            }
            catch (Exception e)
            {
                sb.Append("{\"error\":\"").Append(e.Message.Replace("\"", "'")).Append("\"}");
            }
        }
        return sb.Append("]").ToString();
    }

    public static string Status()
    {
        StringBuilder sb = new StringBuilder();
        System.Globalization.CultureInfo inv = System.Globalization.CultureInfo.InvariantCulture;
        sb.Append("{\"ready\":").Append(ready ? 1 : 0).Append(",\"patched\":").Append(patched ? 1 : 0)
          .Append(",\"error\":\"").Append(error.Replace("\"", "'")).Append("\",\"hair_error\":\"").Append(hairError.Replace("\"", "'"))
          .Append("\",\"lamp_error\":\"").Append(lampError.Replace("\"", "'")).Append("\",\"frames\":").Append(frames)
          .Append(",\"draws\":").Append(lastDraws).Append(",\"skins\":").Append(lastSkins).Append(",\"wraps\":").Append(lastWraps)
          .Append(",\"rigids\":").Append(lastRigids).Append(",\"rigids_watched\":").Append(rigids.Count).Append(",\"rigid_rebuilds\":").Append(rigidRebuilds)
          .Append(",\"path_skinned_watched\":").Append(pathSkins.Count).Append(",\"path_skin_error\":\"").Append(pathSkinError.Replace("\"","'")).Append("\"")
          .Append(",\"path_object_notes\":").Append(pathNoteCount)
          .Append(",\"eye_watched\":").Append(eyeWatch.Count).Append(",\"eye_hidden_materials\":").Append(eyeHidden.Count)
          .Append(",\"author_observed\":").Append(observedStates.Count).Append(",\"author_observed_changes\":").Append(observedChanges)
          .Append(",\"camera_only_renderers\":").Append(lastCameraOnly).Append(",\"camera_hidden_draws\":").Append(lastCameraHidden).Append(",\"camera_hidden_hairs\":").Append(lastCameraHiddenHairs)
          .Append(",\"hairs\":").Append(lastHairs).Append(",\"hairs_known\":").Append(hairs.Count).Append(",\"hair_list\":\"").Append(HairSummary()).Append("\"")
          .Append(",\"lights\":").Append(lastLights).Append(",\"shadowed\":").Append(lastShadowed).Append(",\"dl_lamps\":").Append(dlLamps.Count)
          .Append(",\"light_shadow\":\"").Append(LightShadowSummary()).Append("\"")
          .Append(",\"linked_layers\":").Append(linkedLayers).Append(",\"light_excluded\":\"").Append(LightLinkSummary()).Append("\"").Append(",\"physical_layers\":").Append(pathPhysicalLayers)
          .Append(",\"kinds\":[").Append(lastKinds[0]).Append(",").Append(lastKinds[1]).Append(",").Append(lastKinds[2]).Append(",").Append(lastKinds[3])
          .Append("],\"no_occlude\":").Append(lastNoOcclude).Append(",\"with_uv\":").Append(lastUv)
          .Append(",\"uv_meshes\":").Append(uvMeshes).Append(",\"uv_missing\":").Append(uvMissing).Append(",\"multi_stream\":").Append(multiStream)
          .Append(",\"decals\":").Append(decalCount).Append(",\"eye_overlays\":").Append(eyeOverlays.Count).Append(",\"wrap_contact\":\"").Append(decalSample).Append("\"")
          .Append(",\"materials\":").Append(materials.Count).Append(",\"textures\":").Append(textures.Count)
          .Append(",\"wrap_shaders\":\"").Append(WrapShaderSummary()).Append("\"")
          .Append(",\"color_space\":\"").Append(QualitySettings.activeColorSpace.ToString()).Append("\"")
          .Append(",\"path_loading\":").Append(pathLoading?1:0)
          .Append(",\"history_revision\":").Append(sceneRevision).Append(",\"content_revision\":").Append(contentRevision)
          .Append(",\"path_scene_lights\":").Append(pathLights.Count)
          .Append(",\"ambient_frame\":").Append(shFrame)
          .Append(",\"ambient_mode\":\"").Append(RenderSettings.ambientMode.ToString()).Append("\",\"sh0\":[")
          .Append(sh[3].ToString("0.0000", inv)).Append(",").Append(sh[7].ToString("0.0000", inv)).Append(",").Append(sh[11].ToString("0.0000", inv)).Append("]")
          .Append(",\"collect_ms\":").Append((collectSamples > 0 ? collectSumMs / collectSamples : 0).ToString("0.0000", inv))
          .Append(",\"collect_max_ms\":").Append((collectMaxTicks * 1000.0 / Stopwatch.Frequency).ToString("0.0000", inv))
          .Append(",\"collect_samples\":").Append(collectSamples).Append("}");
        return sb.ToString();
    }
}
