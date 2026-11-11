using System;
using System.Collections.Generic;
using System.Globalization;
using BepInEx;
using BepInEx.Configuration;
using UnityEngine;
using UnityEngine.Rendering;

// Independent, live-only lighting override. Authored lamp values and transforms
// are never edited. Only their enabled bits are held during the override.
[BepInPlugin("local.vam.displaylighting", "VaM Display Lighting", "1.6.0")]
public sealed partial class VaMDisplayLighting : BaseUnityPlugin
{
    // Backdrop: not the subject but whatever stands behind it (wall, set).
    private enum Aim { Face, Upper, Body, Backdrop }

    // How a lamp moves with a moving subject. Follow: stays at its place
    // around the subject, in the viewer's frame. Sun: direction fixed in the
    // world, position follows (parallel light: the rim walks round the
    // silhouette as the subject turns). Fixed: a real lamp in the room (the
    // subject moves through its light).
    private enum Motion { Follow, Sun, Fixed }

    // One lamp. Angles are around the direction from the subject to the
    // viewer: azimuth positive toward the key side, elevation above the
    // horizon. Linear =
    // luminance arriving at the aim point in linear units, before N.L (VaM:
    // linear colour space, LDR camera, no tone mapping). Edge = radius of
    // the lamp's shadow edge on the skin in metres (0: no shadow); see
    // ShadowStrength.
    private struct LampSpec
    {
        public string Name;
        public float Azimuth, Elevation, Linear, Angle, Edge, DistanceScale;
        public Vector3 Color;      // linear RGB, max channel 1
        public int Resolution;
        public Aim Aim;
        public Motion Motion;
        public LampSpec(string name, float azimuth, float elevation, float linear, float kelvin, Aim aim, Motion motion, float angle, float edge, int resolution, float distanceScale)
            : this(name, azimuth, elevation, linear, KelvinColor(kelvin), aim, motion, angle, edge, resolution, distanceScale) { }
        public LampSpec(string name, float azimuth, float elevation, float linear, Vector3 color, Aim aim, Motion motion, float angle, float edge, int resolution, float distanceScale)
        {
            Name = name; Azimuth = azimuth; Elevation = elevation; Linear = linear; Color = color; Aim = aim; Motion = motion;
            Angle = angle; Edge = edge; Resolution = resolution; DistanceScale = distanceScale;
        }
    }

    // One broad term of the environment light (Unity ambient SH): a cosine
    // lobe from a direction, no shadow, wraps past the terminator. Linear =
    // luminance on a surface facing it. World: fixed at build (a window, the
    // sun); otherwise it turns with the viewer line like the lamps.
    // In the specular environment (what skin, eyes and hair reflect) the
    // same term is a soft-edged disc of Size degrees radius.
    private struct EnvSpec
    {
        public float Azimuth, Elevation, Linear, Size;
        public Vector3 Color;
        public bool World;
    }

    private static EnvSpec Env(float azimuth, float elevation, float linear, Vector3 color)
    {
        EnvSpec e;
        e.Azimuth = azimuth; e.Elevation = elevation; e.Linear = linear; e.Color = color; e.World = false; e.Size = 20f;
        return e;
    }

    private static EnvSpec Wide(EnvSpec e) { e.Size = 35f; return e; }

    private static EnvSpec Env(float azimuth, float elevation, float linear, float kelvin) { return Env(azimuth, elevation, linear, KelvinColor(kelvin)); }
    private static EnvSpec InWorld(EnvSpec e) { e.World = true; return e; }

    private sealed class Style
    {
        public string Id, Label, Group;
        public float AmbientLinear;
        public Vector3 Ambient;
        public EnvSpec[] Env;
        public float Sheen = 1f;     // Skyshop specular intensity on the environment; above ~0.5 VaM skin reads oily
        public LampSpec[] Lamps;
    }

    private sealed class RigLamp
    {
        public Light Light;
        public LampSpec Spec;
        public float Distance;
        public Vector3 Direction;   // as built; a Sun keeps it
    }

    // Blackbody in linear sRGB (D65 white), max channel 1 (research/cct.py).
    private static readonly float[] KelvinTable = { 3000f, 3200f, 3500f, 4000f, 4300f, 5000f, 5500f, 6500f, 7500f, 9000f, 12000f };
    private static readonly Vector3[] KelvinRgb =
    {
        new Vector3(1f, .512f, .156f), new Vector3(1f, .553f, .196f), new Vector3(1f, .611f, .262f), new Vector3(1f, .698f, .381f),
        new Vector3(1f, .745f, .456f), new Vector3(1f, .842f, .634f), new Vector3(1f, .901f, .760f), new Vector3(1f, 1f, 1f),
        new Vector3(.823f, .886f, 1f), new Vector3(.670f, .779f, 1f), new Vector3(.526f, .667f, 1f),
    };

    // A gel: linear RGB scaled to max channel 1 (brightness comes from Linear).
    private static Vector3 Gel(float r, float g, float b)
    {
        float m = Mathf.Max(r, Mathf.Max(g, b));
        return new Vector3(r / m, g / m, b / m);
    }

    // How established scene authors light people (a study of
    // 677 library scenes): lamps carry the light (spots of 50-80 deg at
    // 1-2.3 m, ambient ~0, Skyshop IBL off in 87%), front lamps sit at the
    // eyeline (median -3 deg, nine in ten below 20) where the VR viewer is,
    // the light on the face is neutral to cool (median R/B 1) with tinted
    // accents, and the light from behind is as strong as the key (median
    // 0.92). So each look keeps its lamps as the main light (the ray-traced
    // skin shadows them softly, 0.25 m disc) and the environment (Unity
    // ambient SH: skin, hair and the whole room, no shadow) only as a soft
    // wrap and the floor / backdrop bounce: less of it also leaves the room
    // darker than the subject. Each lamp keeps its colour and angles; Env
    // terms keep the specular sheen's direction (the cube is drawn from them).
    private static readonly Style[] Styles =
    {
        // Clamshell beauty: soft key just over the eyeline, weaker fill from
        // below (twin catchlights, smooth skin), two hair lights for a halo.
        new Style { Id = "Beauty", Label = Lang.N("Soft beauty"), Group = Lang.N("Classic"), AmbientLinear = 0.005f, Sheen = 0.4f, Ambient = KelvinColor(6500f),
            Env = new[] { Env(0f, 25f, 0.20f, 6500f), Wide(Env(0f, -60f, 0.06f, 6000f)), Wide(Env(180f, 15f, 0.08f, 6500f)) }, Lamps = new[] {
            new LampSpec("Key", 0f, 20f, 0.32f, 6500f, Aim.Face, Motion.Follow, 80f, 0.015f, 4096, 0.75f),
            new LampSpec("Under", 0f, -15f, 0.10f, 6000f, Aim.Face, Motion.Follow, 90f, 0.02f, 2048, 0.75f),
            new LampSpec("HairL", 135f, 30f, 0.30f, 7500f, Aim.Face, Motion.Follow, 70f, 0.008f, 2048, 1f),
            new LampSpec("HairR", -135f, 30f, 0.30f, 7500f, Aim.Face, Motion.Follow, 70f, 0.008f, 2048, 1f) } },
        // Rembrandt: one key on the short side, high enough for the cheek
        // triangle, no fill, a cool kicker tracing the shadow-side edge.
        new Style { Id = "Rembrandt", Label = Lang.N("Rembrandt"), Group = Lang.N("Classic"), AmbientLinear = 0.003f, Sheen = 0.35f, Ambient = KelvinColor(5500f),
            Env = new[] { Env(50f, 35f, 0.08f, 5500f), Env(-125f, 20f, 0.06f, 7500f) }, Lamps = new[] {
            new LampSpec("Key", 50f, 35f, 0.62f, 5500f, Aim.Face, Motion.Follow, 80f, 0.008f, 4096, 0.75f),
            new LampSpec("Kicker", -125f, 20f, 0.42f, 7500f, Aim.Face, Motion.Follow, 70f, 0.008f, 2048, 1f) } },
        // Window: a very large, low side source of daylight fixed in the
        // room, a warm room bounce on the other side. Painterly gradient.
        new Style { Id = "Window", Label = Lang.N("Window light"), Group = Lang.N("Classic"), AmbientLinear = 0.008f, Sheen = 0.35f, Ambient = KelvinColor(6500f),
            Env = new[] { InWorld(Wide(Env(75f, 18f, 0.12f, 6500f))), InWorld(Env(-70f, 0f, 0.09f, 4300f)), Wide(Env(0f, -60f, 0.05f, 5000f)) }, Lamps = new[] {
            new LampSpec("Window", 75f, 18f, 0.64f, 6500f, Aim.Upper, Motion.Fixed, 120f, 0.02f, 4096, 1f) } },
        // Golden hour: low warm sun from behind for a glowing rim and hair,
        // cool sky on the face (warm / cool pair), warm ground bounce.
        new Style { Id = "Golden", Label = Lang.N("Golden backlight"), Group = Lang.N("Classic"), AmbientLinear = 0.01f, Sheen = 0.35f, Ambient = KelvinColor(6500f),
            Env = new[] { InWorld(Env(140f, 15f, 0.25f, 3200f)), Env(10f, 20f, 0.16f, 7500f), Wide(Env(0f, -60f, 0.08f, 4300f)) }, Lamps = new[] {
            new LampSpec("Sun", 140f, 15f, 0.9f, 3200f, Aim.Upper, Motion.Sun, 60f, 0.006f, 4096, 1f),
            new LampSpec("Sky", 10f, 15f, 0.18f, 7500f, Aim.Face, Motion.Follow, 90f, 0.02f, 2048, 0.75f) } },
        // Contour: two strip-like kickers from behind both sides draw the
        // body's outline; a soft frontal key keeps the face readable.
        new Style { Id = "Contour", Label = Lang.N("Contour"), Group = Lang.N("Classic"), AmbientLinear = 0.005f, Sheen = 0.35f, Ambient = KelvinColor(6500f),
            Env = new[] { Env(15f, 30f, 0.08f, 6500f), Wide(Env(180f, 10f, 0.10f, 7500f)) }, Lamps = new[] {
            new LampSpec("StripL", 125f, 15f, 0.50f, 7500f, Aim.Body, Motion.Follow, 80f, 0.01f, 2048, 1f),
            new LampSpec("StripR", -125f, 15f, 0.50f, 7500f, Aim.Body, Motion.Follow, 80f, 0.01f, 2048, 1f),
            new LampSpec("Key", 15f, 18f, 0.30f, 6500f, Aim.Face, Motion.Follow, 80f, 0.012f, 4096, 0.75f) } },
        // Cinematic warm / cool: warm key, cool blue rim from the other side.
        new Style { Id = "Cinema", Label = Lang.N("Cool-warm cinema"), Group = Lang.N("Classic"), AmbientLinear = 0.004f, Sheen = 0.35f, Ambient = KelvinColor(9000f),
            Env = new[] { Env(40f, 20f, 0.06f, 5200f), Env(-130f, 25f, 0.12f, 12000f) }, Lamps = new[] {
            new LampSpec("Key", 40f, 18f, 0.50f, 5200f, Aim.Face, Motion.Follow, 80f, 0.008f, 4096, 0.75f),
            new LampSpec("Rim", -135f, 22f, 0.50f, 12000f, Aim.Face, Motion.Follow, 70f, 0.008f, 2048, 1f) } },

        // ---- Contemporary: looks from current beauty / idol / fashion media. Not
        // bound to warm white; colour also goes on the backdrop behind the
        // subject. Kept inside VaM's LDR: gels are luminance-normalised and
        // the lit skin stays under clipping; shadows are lifted by a tinted
        // environment instead of grading.

        // Clear high-key (K-beauty / idol): cool-white environment from just
        // off the viewer line, bright floor bounce (neck and chin), blue
        // backdrop bounce on the edges; lamps for shape, cyan edges, the wash.
        new Style { Id = "Clear", Label = Lang.N("Clear cool white"), Group = Lang.N("Contemporary"), AmbientLinear = 0.02f, Sheen = 0.5f, Ambient = Gel(0.62f, 0.78f, 1f),
            Env = new[] { Env(15f, 20f, 0.30f, Gel(0.84f, 0.84f, 1f)), Wide(Env(0f, -60f, 0.20f, Gel(0.9f, 0.9f, 1f))), Wide(Env(180f, 0f, 0.25f, Gel(0.45f, 0.7f, 1f))) }, Lamps = new[] {
            new LampSpec("Key", 15f, 15f, 0.42f, Gel(0.84f, 0.84f, 1f), Aim.Face, Motion.Follow, 80f, 0.015f, 4096, 0.75f),
            new LampSpec("Under", 0f, -20f, 0.14f, Gel(0.9f, 0.9f, 1f), Aim.Face, Motion.Follow, 90f, 0.02f, 2048, 0.75f),
            new LampSpec("EdgeL", 125f, 10f, 0.38f, Gel(0.7f, 0.88f, 1f), Aim.Upper, Motion.Follow, 70f, 0.008f, 2048, 1f),
            new LampSpec("EdgeR", -125f, 10f, 0.38f, Gel(0.7f, 0.88f, 1f), Aim.Upper, Motion.Follow, 70f, 0.008f, 2048, 1f),
            new LampSpec("Wash", 180f, 5f, 0.32f, Gel(0.30f, 0.62f, 1f), Aim.Backdrop, Motion.Follow, 110f, 0f, 0, 1f) } },
        // Pastel: pink environment and clamshell, lavender floor, pale blue
        // from behind and on the hair, pink-lavender backdrop. Airy.
        new Style { Id = "Pastel", Label = Lang.N("Pastel cream"), Group = Lang.N("Contemporary"), AmbientLinear = 0.015f, Sheen = 0.45f, Ambient = Gel(0.9f, 0.8f, 1f),
            Env = new[] { Env(0f, 25f, 0.24f, Gel(1f, 0.80f, 0.88f)), Wide(Env(0f, -60f, 0.13f, Gel(0.88f, 0.82f, 1f))), Wide(Env(180f, 10f, 0.18f, Gel(0.75f, 0.85f, 1f))) }, Lamps = new[] {
            new LampSpec("Key", 0f, 20f, 0.32f, Gel(1f, 0.80f, 0.88f), Aim.Face, Motion.Follow, 80f, 0.015f, 4096, 0.75f),
            new LampSpec("Under", 0f, -15f, 0.10f, Gel(0.88f, 0.82f, 1f), Aim.Face, Motion.Follow, 90f, 0.02f, 2048, 0.75f),
            new LampSpec("HairL", 135f, 30f, 0.30f, Gel(0.75f, 0.85f, 1f), Aim.Face, Motion.Follow, 70f, 0.008f, 2048, 1f),
            new LampSpec("HairR", -135f, 30f, 0.30f, Gel(0.75f, 0.85f, 1f), Aim.Face, Motion.Follow, 70f, 0.008f, 2048, 1f),
            new LampSpec("Wash", 180f, 5f, 0.45f, Gel(1f, 0.72f, 0.9f), Aim.Backdrop, Motion.Follow, 120f, 0f, 0, 1f) } },
        // Neon duotone (music video / club): magenta from the key side, blue
        // from the other (cyan turns skin grey-green), both also as broad
        // environment; a weak neutral front keeps the middle of the face.
        new Style { Id = "Neon", Label = Lang.N("Two-tone neon"), Group = Lang.N("Contemporary"), AmbientLinear = 0.005f, Sheen = 0.45f, Ambient = Gel(0.5f, 0.4f, 1f),
            Env = new[] { Env(95f, 10f, 0.09f, Gel(1f, 0.18f, 0.72f)), Env(-95f, 10f, 0.10f, Gel(0.15f, 0.55f, 1f)), Env(0f, 60f, 0.04f, Gel(0.5f, 0.4f, 1f)) }, Lamps = new[] {
            new LampSpec("Front", 0f, 12f, 0.10f, 6500f, Aim.Face, Motion.Follow, 80f, 0.015f, 2048, 0.75f),
            new LampSpec("Magenta", 95f, 15f, 0.29f, Gel(1f, 0.18f, 0.72f), Aim.Face, Motion.Follow, 70f, 0.01f, 2048, 1f),
            new LampSpec("Blue", -95f, 15f, 0.37f, Gel(0.15f, 0.55f, 1f), Aim.Face, Motion.Follow, 70f, 0.01f, 2048, 1f),
            new LampSpec("Wash", 180f, 0f, 0.12f, Gel(0.45f, 0.2f, 1f), Aim.Backdrop, Motion.Follow, 110f, 0f, 0, 1f) } },
        // Sunset lamp (social-media projector): a round red-orange pool on
        // the face and one beside the head on the backdrop; blue-violet
        // environment for the shadows, a violet rim.
        new Style { Id = "Sunset", Label = Lang.N("Sunset lamp"), Group = Lang.N("Contemporary"), AmbientLinear = 0.012f, Sheen = 0.45f, Ambient = Gel(0.6f, 0.45f, 1f),
            Env = new[] { Env(55f, 15f, 0.09f, Gel(1f, 0.36f, 0.12f)), Env(-60f, 10f, 0.10f, Gel(0.55f, 0.42f, 1f)), Wide(Env(180f, 10f, 0.08f, Gel(0.55f, 0.42f, 1f))) }, Lamps = new[] {
            new LampSpec("Sun", 55f, 15f, 0.45f, Gel(1f, 0.36f, 0.12f), Aim.Face, Motion.Follow, 45f, 0.006f, 4096, 1f),
            new LampSpec("Rim", -135f, 20f, 0.34f, Gel(0.5f, 0.45f, 1f), Aim.Face, Motion.Follow, 70f, 0.01f, 2048, 1f),
            new LampSpec("Circle", 165f, 8f, 0.32f, Gel(1f, 0.32f, 0.08f), Aim.Backdrop, Motion.Follow, 40f, 0f, 0, 1f) } },
        // Direct flash (editorial / Gen-Z snapshot): one hard light almost on
        // the viewer line, crisp shadow on the backdrop, a dim room. Bold.
        new Style { Id = "Flash", Label = Lang.N("Direct flash"), Group = Lang.N("Contemporary"), AmbientLinear = 0.005f, Sheen = 0.3f, Ambient = Gel(0.7f, 0.8f, 1f),
            Env = new[] { Env(0f, 10f, 0.06f, 6200f) }, Lamps = new[] {
            new LampSpec("Flash", 0f, 6f, 0.60f, 6200f, Aim.Face, Motion.Follow, 100f, 0.004f, 4096, 0.8f) } },
    };

    private const float RangeScale = 4f;           // aim point at 1/4 range: Unity falloff 1/(1+25u^2) = 0.39
    private const float ShadowReach = 1.0f;        // casters further than this toward the lamp are ignored (walls)
    private const float AnchorLag = 0.35f;         // s: lamps trail the body's drift, not breathing or jitter
    private const float TurnDeadband = 25f;        // deg: the viewer stepping or leaning does not swing the lamps
    private const float TurnLag = 1.0f;            // s: a swing, when it comes, is slow
    private const float BackdropOffset = 0.5f;     // m behind the head
    private const float BackdropDefault = 3f;      // m to the backdrop when no collider is found

    public static VaMDisplayLighting Instance;
    private ConfigEntry<string> styleSetting;
    private readonly Dictionary<Light, bool> lamps = new Dictionary<Light, bool>();
    private readonly List<Light> dead = new List<Light>();
    private readonly List<RigLamp> rigLamps = new List<RigLamp>();
    private GameObject rig;
    private SuperController controller;
    private Camera viewer;
    private Atom subject;
    private SkyshopLightController sky;
    private JSONStorableFloat masterParam, ambientParam, exposureParam, diffuseParam, specularParam, skyboxParam;
    private float oldMaster, oldAmbient, oldExposure, oldDiffuse, oldSpecular, oldSkybox;
    private mset.Sky envSky;
    private Texture oldCube;
    private bool oldHdrSpec;
    private Quaternion oldSkyRotation;
    private Cubemap envCube;
    private Color[] cubePixels;
    private float cubeYaw;
    private const int CubeSize = 128;
    private Color oldAmbientColor, ambientColor;
    private AmbientMode oldAmbientMode;
    private SphericalHarmonicsL2 oldProbe, envProbe;
    private Vector3[] envWorld = new Vector3[0];
    private float envYaw;
    private bool envFlipped;
    private readonly Vector3[] envDir = new Vector3[1];
    private readonly Color[] envOut = new Color[1];
    private bool running, suspended, subscribed;
    private int sourceCount;
    private string message = Lang.T("Stopped; the scene's own lighting.");
    private Vector3 front, right, faceAim, upperAim, bodyAim;
    private float side = 1f, baseDistance = 2.4f, frontYaw, targetYaw;
    private Transform eyeL, eyeR, headPoint, chestPoint, hipPoint;

    private void Awake()
    {
        Instance = this;
        string[] ids = new string[Styles.Length];
        for (int i = 0; i < Styles.Length; i++) ids[i] = Styles[i].Id;
        styleSetting = Config.Bind<string>("Lighting", "Style", "Beauty",
            new ConfigDescription("Lighting look: " + string.Join(", ", ids) + ".", new AcceptableValueList<string>(ids)));
        styleSetting.SettingChanged += delegate { if (running) BuildRig(); SyncMenu(); };
        Camera.onPreCull += BeforeCamera;
        Logger.LogInfo("VaM Display Lighting 1.6.0 loaded; stopped. Independent Display Lighting tab.");
    }

    private void Update()
    {
        if (controller == null)
        {
            controller = SuperController.singleton;
            if (controller != null) Subscribe();
        }
        if (running && (controller == null || controller.isLoading || subject == null || !subject.on))
            StopLighting();
        UpdateMenu();
    }

    private void Subscribe()
    {
        if (subscribed) return;
        controller.onAtomAddedHandlers += AtomAdded;
        controller.onAtomRemovedHandlers += AtomRemoved;
        controller.onSceneLoadedHandlers += SceneLoaded;
        controller.onSubSceneLoadedHandlers += SubSceneLoaded;
        controller.onBeforeSceneSaveHandlers += BeforeSave;
        controller.onSceneSavedHandlers += AfterSave;
        subscribed = true;
    }

    private void SceneLoaded() { StopLighting(); menuAttempted = false; }
    private void SubSceneLoaded(SubScene scene) { if (running) CaptureSceneLamps(); }
    private void AtomAdded(Atom atom) { if (running && atom != null) CaptureAtomLamps(atom); }
    private void AtomRemoved(Atom atom) { if (running && atom == subject) StopLighting(); }

    private Style CurrentStyle()
    {
        foreach (Style style in Styles) if (style.Id == styleSetting.Value) return style;
        return Styles[0];
    }

    // Explicit UI action: the only whole-scene seed. No timed object discovery.
    public string StartLighting()
    {
        if (running) return State();
        if (controller == null || controller.isLoading)
        { message = Lang.T("Start it once the scene has finished loading."); SyncMenu(); return State(); }
        try
        {
            Camera camera = controller.lookCamera != null ? controller.lookCamera : Camera.main;
            if (camera == null) throw new InvalidOperationException(Lang.T("No viewing camera found."));
            viewer = camera;
            subject = ChooseSubject(camera);
            if (subject == null) throw new InvalidOperationException(Lang.T("The scene has no Person switched on."));
            Frame(camera);
            CaptureSceneLamps();
            sourceCount = lamps.Count;
            oldAmbientColor = RenderSettings.ambientLight;
            oldAmbientMode = RenderSettings.ambientMode;
            oldProbe = RenderSettings.ambientProbe;
            sky = SkyshopLightController.singleton;
            if (sky != null)
            {
                masterParam = sky.GetFloatJSONParam("masterIntensity");
                ambientParam = sky.GetFloatJSONParam("unityAmbientIntensity");
                exposureParam = sky.GetFloatJSONParam("camExposure");
                diffuseParam = sky.GetFloatJSONParam("diffuseIntensity");
                specularParam = sky.GetFloatJSONParam("specularIntensity");
                skyboxParam = sky.GetFloatJSONParam("skyboxIntensity");
                if (masterParam == null || ambientParam == null || exposureParam == null || diffuseParam == null || specularParam == null || skyboxParam == null)
                    throw new InvalidOperationException(Lang.T("The ambient lighting is not initialised yet."));
                oldMaster = masterParam.val;
                oldAmbient = ambientParam.val;
                oldExposure = exposureParam.val;
                oldDiffuse = diffuseParam.val;
                oldSpecular = specularParam.val;
                oldSkybox = skyboxParam.val;
            }
            running = true;
            BuildRig();
            Logger.LogInfo("Display lighting started: subject=" + subject.uid + ", sources=" + sourceCount + ", front=" + front + ", side=" + side + ", face=" + faceAim + ", eyes=" + (eyeL != null) + ", chest=" + (chestPoint != null ? chestPoint.name : "-") + ", hip=" + (hipPoint != null ? hipPoint.name : "-"));
        }
        catch (Exception e)
        {
            StopLighting();
            message = Lang.F("Could not start: {0}", e.Message);
            Logger.LogError("Display lighting start: " + e);
        }
        SyncMenu();
        return State();
    }

    public string StopLighting()
    {
        bool wasRunning = running;
        running = false;
        suspended = false;
        DestroyRig();
        RestoreLamps();
        lamps.Clear();
        if (wasRunning) RestoreEnvironment();
        subject = null;
        viewer = null;
        eyeL = eyeR = headPoint = chestPoint = hipPoint = null;
        sky = null;
        masterParam = ambientParam = exposureParam = diffuseParam = specularParam = skyboxParam = null;
        message = Lang.T("Stopped; the scene's own lighting is back.");
        if (wasRunning) Logger.LogInfo("Display lighting stopped; original lights and environment restored.");
        SyncMenu();
        return State();
    }

    public string SetStyle(string value)
    {
        foreach (Style style in Styles)
            if (style.Id == value) { styleSetting.Value = value; return State(); }
        return "Invalid style";
    }

    private Atom ChooseSubject(Camera camera)
    {
        Atom best = null;
        float score = float.MaxValue;
        foreach (Atom atom in controller.GetAtoms())
        {
            if (atom == null || atom.type != "Person" || !atom.on || !atom.gameObject.activeInHierarchy) continue;
            Vector3 p = SubjectBounds(atom).center;
            Vector3 direction = p - camera.transform.position;
            float value = direction.sqrMagnitude * (1f + 2f * (1f - Vector3.Dot(camera.transform.forward, direction.normalized)));
            if (value < score) { score = value; best = atom; }
        }
        return best;
    }

    private static Bounds SubjectBounds(Atom atom)
    {
        bool found = false;
        Bounds bounds = new Bounds(atom.transform.position + Vector3.up, Vector3.one);
        foreach (string id in new string[] { "headControl", "chestControl", "hipControl", "lFootControl", "rFootControl", "lHandControl", "rHandControl" })
        {
            FreeControllerV3 control = atom.GetStorableByID(id) as FreeControllerV3;
            if (control == null) continue;
            if (!found) { bounds = new Bounds(control.transform.position, Vector3.zero); found = true; }
            else bounds.Encapsulate(control.transform.position);
        }
        bounds.Expand(0.30f);
        return bounds;
    }

    // Body points the lamps aim at: rendered bones (they move with physics
    // and animation), controls only as a fallback.
    private Transform BodyPoint(DAZBones bones, string bone, string control)
    {
        DAZBone found = bones != null ? bones.GetDAZBone(bone) : null;
        if (found != null) return found.transform;
        FreeControllerV3 fallback = subject.GetStorableByID(control) as FreeControllerV3;
        return fallback != null ? fallback.transform : null;
    }

    private void ReadAnchors(out Vector3 face, out Vector3 upper, out Vector3 body)
    {
        Vector3 head = headPoint.position;
        face = eyeL != null && eyeR != null ? (eyeL.position + eyeR.position) * 0.5f : head;
        Vector3 chest = chestPoint != null ? chestPoint.position : head - Vector3.up * 0.35f;
        Vector3 hip = hipPoint != null ? hipPoint.position : chest - Vector3.up * 0.4f;
        upper = Vector3.Lerp(chest, head, 0.6f);
        body = Vector3.Lerp(hip, chest, 0.5f);
    }

    // The rig is laid out around the line from the subject to the viewer,
    // like a studio around its camera: as the subject turns, light plays
    // over the form. Asymmetric looks put the key on the short side (the
    // side the face turns toward), chosen once at start.
    private void Frame(Camera camera)
    {
        DAZBones bones = subject.GetComponentInChildren<DAZBones>();
        headPoint = BodyPoint(bones, "head", "headControl");
        if (headPoint == null) throw new InvalidOperationException(Lang.T("The person has no head bone."));
        chestPoint = BodyPoint(bones, "chest", "chestControl");
        hipPoint = BodyPoint(bones, "hip", "hipControl");
        DAZBone lEye = bones != null ? bones.GetDAZBone("lEye") : null, rEye = bones != null ? bones.GetDAZBone("rEye") : null;
        eyeL = lEye != null ? lEye.transform : null;
        eyeR = rEye != null ? rEye.transform : null;
        ReadAnchors(out faceAim, out upperAim, out bodyAim);
        Vector3 facing = Vector3.zero;
        if (eyeL != null && eyeR != null)
        {
            facing = Vector3.Cross(Vector3.up, eyeL.position - eyeR.position);
            if (Vector3.Dot(facing, faceAim - headPoint.position) < 0f) facing = -facing;
            facing = Vector3.ProjectOnPlane(facing, Vector3.up);
        }
        Vector3 toViewer = Vector3.ProjectOnPlane(camera.transform.position - faceAim, Vector3.up);
        if (toViewer.sqrMagnitude < 0.0025f) toViewer = facing;          // viewer straight above
        if (toViewer.sqrMagnitude < 1e-6f) toViewer = Vector3.back;
        frontYaw = targetYaw = Mathf.Atan2(toViewer.x, toViewer.z) * Mathf.Rad2Deg;
        SetFront();
        // eye axis under ~3 cm horizontal (lying, rolled head): no turn to read
        side = facing.sqrMagnitude > 0.25f * 0.0036f && Vector3.Dot(facing.normalized, right) < -0.17f ? -1f : 1f;
        baseDistance = Mathf.Clamp(SubjectBounds(subject).size.magnitude * 0.8f, 2.0f, 3.2f);
    }

    private void SetFront()
    {
        front = Quaternion.Euler(0f, frontYaw, 0f) * Vector3.forward;
        right = Vector3.Cross(Vector3.up, front).normalized;
    }

    // Per frame, O(rig lamps): smoothed anchors, the viewer line with a dead
    // band, then Follow and Sun lamps re-placed. Fixed lamps never move.
    private void LateUpdate()
    {
        if (!running || suspended || rig == null || subject == null) return;
        float dt = Time.unscaledDeltaTime;
        Vector3 face, upper, body;
        ReadAnchors(out face, out upper, out body);
        float k = 1f - Mathf.Exp(-dt / AnchorLag);
        faceAim = Vector3.Lerp(faceAim, face, k);
        upperAim = Vector3.Lerp(upperAim, upper, k);
        bodyAim = Vector3.Lerp(bodyAim, body, k);
        Camera camera = controller.lookCamera != null ? controller.lookCamera : viewer;
        if (camera != null)
        {
            Vector3 toViewer = Vector3.ProjectOnPlane(camera.transform.position - faceAim, Vector3.up);
            if (toViewer.sqrMagnitude > 0.0025f)
            {
                float yaw = Mathf.Atan2(toViewer.x, toViewer.z) * Mathf.Rad2Deg;
                if (Mathf.Abs(Mathf.DeltaAngle(targetYaw, yaw)) > TurnDeadband) targetYaw = yaw;
            }
        }
        frontYaw = Mathf.LerpAngle(frontYaw, targetYaw, 1f - Mathf.Exp(-dt / TurnLag));
        SetFront();
        if (Mathf.Abs(Mathf.DeltaAngle(envYaw, frontYaw)) > 0.5f)
        {
            BuildEnv();
            TurnSky();
        }
        foreach (RigLamp lamp in rigLamps) Place(lamp);
    }

    private Vector3 LampDirection(LampSpec spec)
    {
        float azimuth = spec.Azimuth * side * Mathf.Deg2Rad, elevation = spec.Elevation * Mathf.Deg2Rad;
        return (front * Mathf.Cos(azimuth) + right * Mathf.Sin(azimuth)) * Mathf.Cos(elevation) + Vector3.up * Mathf.Sin(elevation);
    }

    private void Place(RigLamp lamp)
    {
        if (lamp.Spec.Motion == Motion.Fixed || lamp.Light == null) return;
        Put(lamp, lamp.Spec.Motion == Motion.Sun ? lamp.Direction : LampDirection(lamp.Spec));
    }

    // A backdrop lamp sits just behind the head and points away from the
    // viewer; every other lamp points at its aim point.
    private void Put(RigLamp lamp, Vector3 direction)
    {
        Transform t = lamp.Light.transform;
        if (lamp.Spec.Aim == Aim.Backdrop)
        {
            t.position = faceAim + direction * lamp.Distance;
            t.rotation = Quaternion.LookRotation(direction, Vector3.up);
        }
        else
        {
            t.position = AimPoint(lamp.Spec.Aim) + direction * lamp.Distance;
            t.rotation = Quaternion.LookRotation(-direction, Vector3.up);
        }
    }

    private void DestroyRig()
    {
        if (rig != null) { rig.SetActive(false); Destroy(rig); rig = null; }
        rigLamps.Clear();
    }

    // Laid out from the current viewer line and body position.
    private void BuildRig()
    {
        DestroyRig();
        Style style = CurrentStyle();
        rig = new GameObject("VaMDisplayLighting (temporary)");
        rig.hideFlags = HideFlags.DontSave;
        rig.transform.SetParent(transform, false);
        foreach (LampSpec spec in style.Lamps) AddLamp(spec);
        envWorld = new Vector3[style.Env != null ? style.Env.Length : 0];
        for (int i = 0; i < envWorld.Length; i++) envWorld[i] = EnvDirection(style.Env[i]);
        BuildEnv();
        BuildCube();
        if (suspended) rig.SetActive(false);
        else HoldOverride();
        message = Lang.F("Relighting · {0} · {1} · {2} scene lamps read", Lang.T(style.Label), subject.uid, sourceCount);
    }

    private Vector3 EnvDirection(EnvSpec e)
    {
        float azimuth = e.Azimuth * side * Mathf.Deg2Rad, elevation = e.Elevation * Mathf.Deg2Rad;
        return (front * Mathf.Cos(azimuth) + right * Mathf.Sin(azimuth)) * Mathf.Cos(elevation) + Vector3.up * Mathf.Sin(elevation);
    }

    private float EnvLuminance(SphericalHarmonicsL2 sh, Vector3 direction)
    {
        envDir[0] = direction;
        sh.Evaluate(envDir, envOut);
        return 0.2126f * envOut[0].r + 0.7152f * envOut[0].g + 0.0722f * envOut[0].b;
    }

    // The environment light as Unity ambient SH (linear). Each term is
    // scaled by evaluating it toward its own direction, so Linear means the
    // same as a lamp's. Rebuilt only when the viewer line turns.
    private void BuildEnv()
    {
        Style style = CurrentStyle();
        SphericalHarmonicsL2 sh = new SphericalHarmonicsL2();
        sh.Clear();
        Vector3 a = style.Ambient * (style.AmbientLinear / Luminance(style.Ambient));
        sh.AddAmbientLight(new Color(a.x, a.y, a.z, 1f));
        if (style.Env != null)
            for (int i = 0; i < style.Env.Length; i++)
            {
                EnvSpec e = style.Env[i];
                Vector3 d = e.World && i < envWorld.Length ? envWorld[i] : EnvDirection(e);
                Color color = new Color(e.Color.x, e.Color.y, e.Color.z, 1f);
                SphericalHarmonicsL2 one = new SphericalHarmonicsL2();
                one.Clear();
                one.AddDirectionalLight(d, color, 1f);
                float toward = EnvLuminance(one, d), away = EnvLuminance(one, -d);
                envFlipped = away > toward;           // which way Unity's direction argument points
                if (envFlipped) { one.Clear(); one.AddDirectionalLight(-d, color, 1f); toward = away; }
                if (toward > 1e-6f) sh = sh + one * (e.Linear / toward);
            }
        envProbe = sh;
        envYaw = frontYaw;
        // Shaders still on the flat legacy ambient get the average.
        Color sum = Color.black;
        Vector3[] axes = { Vector3.up, Vector3.down, front, -front, right, -right };
        foreach (Vector3 axis in axes) { envDir[0] = axis; sh.Evaluate(envDir, envOut); sum += envOut[0]; }
        sum *= 1f / axes.Length;
        ambientColor = new Color(ToGamma(Mathf.Max(0f, sum.r)), ToGamma(Mathf.Max(0f, sum.g)), ToGamma(Mathf.Max(0f, sum.b)), 1f);
    }

    // The specular environment: the same terms as soft discs on a small HDR
    // cube (linear RGBAHalf, alpha 1 = Skyshop's non-HDR decode), blurred
    // by its mips. Built at style change; later turns rotate the sky.
    private void BuildCube()
    {
        Style style = CurrentStyle();
        if (envCube == null)
        {
            envCube = new Cubemap(CubeSize, TextureFormat.RGBAHalf, true);
            envCube.hideFlags = HideFlags.DontSave;
            envCube.wrapMode = TextureWrapMode.Clamp;
            envCube.filterMode = FilterMode.Trilinear;
            cubePixels = new Color[CubeSize * CubeSize];
        }
        int n = style.Env != null ? style.Env.Length : 0;
        Vector3[] dirs = new Vector3[n];
        Vector3[] radiance = new Vector3[n];
        float[] inner = new float[n], outer = new float[n];
        for (int i = 0; i < n; i++)
        {
            EnvSpec e = style.Env[i];
            dirs[i] = e.World && i < envWorld.Length ? envWorld[i] : EnvDirection(e);
            // a disc of radius r gives a facing surface L = R sin^2 r
            float sin = Mathf.Sin(e.Size * Mathf.Deg2Rad);
            radiance[i] = e.Color * (e.Linear / Luminance(e.Color) / (sin * sin));
            inner[i] = Mathf.Cos((e.Size - 12f) * Mathf.Deg2Rad);
            outer[i] = Mathf.Cos((e.Size + 12f) * Mathf.Deg2Rad);
        }
        // No ambient veil in what is reflected (the SH keeps the diffuse one):
        // the cornea keeps a dark pupil with defined catchlights.
        Vector3 a = Vector3.zero;
        for (int face = 0; face < 6; face++)
        {
            for (int y = 0; y < CubeSize; y++)
                for (int x = 0; x < CubeSize; x++)
                {
                    Vector3 d = CubeDirection(face, x, y);
                    Vector3 c = a;
                    for (int i = 0; i < n; i++)
                    {
                        float t = Mathf.InverseLerp(outer[i], inner[i], Vector3.Dot(d, dirs[i]));
                        if (t > 0f) c += radiance[i] * (t * t * (3f - 2f * t));
                    }
                    cubePixels[y * CubeSize + x] = new Color(c.x, c.y, c.z, 1f);
                }
            envCube.SetPixels(cubePixels, (CubemapFace)face, 0);
        }
        envCube.Apply(true, false);
        cubeYaw = frontYaw;
        TurnSky();
    }

    // Direct3D cube layout, row 0 at the top of each face.
    private static Vector3 CubeDirection(int face, int x, int y)
    {
        float u = (x + 0.5f) / CubeSize * 2f - 1f, v = (y + 0.5f) / CubeSize * 2f - 1f;
        Vector3 d;
        switch (face)
        {
            case 0: d = new Vector3(1f, -v, -u); break;
            case 1: d = new Vector3(-1f, -v, u); break;
            case 2: d = new Vector3(u, 1f, v); break;
            case 3: d = new Vector3(u, -1f, -v); break;
            case 4: d = new Vector3(u, -v, 1f); break;
            default: d = new Vector3(-u, -v, -1f); break;
        }
        return d.normalized;
    }

    private void TurnSky()
    {
        if (envSky == null) return;
        envSky.transform.rotation = Quaternion.Euler(0f, frontYaw - cubeYaw, 0f);
        envSky.Apply();
    }

    // The scene's global sky carries our cube while running (VaM's skin,
    // eyes and hair sample it). Only its cube, HDR flag and rotation change.
    private void TakeSky(mset.Sky target)
    {
        envSky = target;
        if (envSky == null) return;
        oldCube = envSky.SpecularCube;
        oldHdrSpec = envSky.HDRSpec;
        oldSkyRotation = envSky.transform.rotation;
        envSky.SpecularCube = envCube;
        envSky.HDRSpec = false;
        TurnSky();
    }

    private void ReleaseSky()
    {
        if (envSky == null) return;
        envSky.SpecularCube = oldCube;
        envSky.HDRSpec = oldHdrSpec;
        envSky.transform.rotation = oldSkyRotation;
        envSky.Apply();
        envSky = null;
        oldCube = null;
    }

    private Vector3 AimPoint(Aim aim) { return aim == Aim.Face ? faceAim : aim == Aim.Upper ? upperAim : bodyAim; }

    private void AddLamp(LampSpec spec)
    {
        Vector3 direction = LampDirection(spec);
        bool backdrop = spec.Aim == Aim.Backdrop;
        // distance from the lamp to what it lights
        float distance = backdrop ? BackdropReach(direction) - BackdropOffset : WallClearance(AimPoint(spec.Aim), direction, baseDistance * spec.DistanceScale);
        GameObject go = new GameObject(spec.Name);
        go.transform.SetParent(rig.transform, false);
        Light light = go.AddComponent<Light>();
        light.type = LightType.Spot;
        light.spotAngle = spec.Angle;
        light.range = distance * RangeScale;
        // Colour at max channel 1; luminance folded into the intensity. The
        // falloff at 1/RangeScale of the range is fixed. VaM keeps Unity's
        // legacy rule (intensity applied before gamma -> linear).
        Vector3 c = spec.Color;
        light.color = new Color(ToGamma(c.x), ToGamma(c.y), ToGamma(c.z), 1f);
        float linear = spec.Linear / Luminance(c) * (1f + 25f / (RangeScale * RangeScale));
        light.intensity = GraphicsSettings.lightsUseLinearIntensity || QualitySettings.activeColorSpace != ColorSpace.Linear ? linear : Mathf.LinearToGammaSpace(linear);
        light.renderMode = LightRenderMode.ForcePixel;
        light.shadows = spec.Edge > 0f ? LightShadows.Hard : LightShadows.None;
        if (light.shadows != LightShadows.None)
        {
            light.shadowStrength = ShadowStrength(spec.Edge, spec.Angle, distance);
            light.shadowCustomResolution = spec.Resolution;
            light.shadowBias = 0.015f;
            light.shadowNormalBias = 0.08f;
            // Walls or furniture near the lamp do not block it (closed rooms).
            light.shadowNearPlane = Mathf.Clamp(distance - ShadowReach, 0.1f, 10f);
        }
        light.cullingMask = -1;
        RigLamp lamp = new RigLamp { Light = light, Spec = spec, Distance = backdrop ? BackdropOffset : distance, Direction = direction };
        Put(lamp, direction);
        rigLamps.Add(lamp);
    }

    // VaM's skin shaders (skin, eyes, lashes, its mesh hair) read a lamp's
    // shadowStrength as the blur radius of its shadow, not as darkness (always
    // full): 25 PCF taps over 0.025 * strength of the shadow map, i.e. a radius
    // of 0.05 * strength * tan(angle / 2) * distance on the subject. At
    // strength 1 a wide cone blurs the nose, eye-socket, chin and under-breast
    // shadows away (10-20 cm): the person casts no shadow on itself. Unity's
    // Soft mode is PCSS there (hair strands draw sharp streaks on the face),
    // so lamps use Hard. Other shaders (walls, floor) take strength as
    // darkness as usual: their shadows get faint.
    private static float ShadowStrength(float edge, float angle, float distance)
    {
        return Mathf.Clamp(edge / (0.05f * Mathf.Tan(angle * 0.5f * Mathf.Deg2Rad) * distance), 0.01f, 1f);
    }

    // One raycast at build: how far behind the subject the backdrop is.
    private float BackdropReach(Vector3 direction)
    {
        float nearest = float.MaxValue;
        foreach (RaycastHit hit in Physics.RaycastAll(faceAim + direction * BackdropOffset, direction, 12f, -1, QueryTriggerInteraction.Ignore))
        {
            Atom owner = hit.collider.GetComponentInParent<Atom>();
            if (owner == null || owner == subject) continue;
            nearest = Mathf.Min(nearest, BackdropOffset + hit.distance);
        }
        return nearest == float.MaxValue ? BackdropDefault : Mathf.Max(BackdropOffset + 0.5f, nearest);
    }

    // One raycast per lamp at build: a lamp behind a wall with a collider
    // moves in front of it. Render-only walls are handled by the near plane.
    private float WallClearance(Vector3 aim, Vector3 direction, float distance)
    {
        float nearest = distance;
        foreach (RaycastHit hit in Physics.RaycastAll(aim + direction * ShadowReach, direction, distance - ShadowReach, -1, QueryTriggerInteraction.Ignore))
        {
            // Scene geometry belongs to an Atom; the VR rig and UI do not.
            Atom owner = hit.collider.GetComponentInParent<Atom>();
            if (owner == null || owner == subject) continue;
            nearest = Mathf.Min(nearest, ShadowReach + hit.distance - 0.1f);
        }
        return Mathf.Max(ShadowReach + 0.3f, nearest);
    }

    private static Vector3 KelvinColor(float kelvin)
    {
        if (kelvin <= KelvinTable[0]) return KelvinRgb[0];
        for (int i = 1; i < KelvinTable.Length; i++)
            if (kelvin <= KelvinTable[i])
            {
                // interpolate in mired (perceptually even)
                float a = 1e6f / KelvinTable[i - 1], b = 1e6f / KelvinTable[i], m = 1e6f / kelvin;
                return Vector3.Lerp(KelvinRgb[i - 1], KelvinRgb[i], (a - m) / (a - b));
            }
        return KelvinRgb[KelvinRgb.Length - 1];
    }

    private static float Luminance(Vector3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
    private static float ToGamma(float v) { return QualitySettings.activeColorSpace == ColorSpace.Linear ? Mathf.LinearToGammaSpace(v) : v; }

    private void CaptureSceneLamps()
    {
        // Includes inactive scene lights, CUA child lights, and lights that
        // animation could enable later. Excludes UI, HMD/controller lamps.
        foreach (Atom atom in controller.GetAtoms()) if (atom != null) CaptureAtomLamps(atom);
    }

    private void CaptureAtomLamps(Atom atom)
    {
        foreach (Light light in atom.GetComponentsInChildren<Light>(true))
            if (light != null && !lamps.ContainsKey(light) && (rig == null || !light.transform.IsChildOf(rig.transform)))
                lamps.Add(light, light.enabled);
    }

    private void BeforeCamera(Camera camera) { if (running && !suspended) HoldOverride(); }

    private void HoldOverride()
    {
        // O(tracked lights), no allocations/discovery; also beats authored
        // flicker/timeline writes before culling instead of flickering a frame.
        dead.Clear();
        foreach (KeyValuePair<Light, bool> pair in lamps)
        {
            if (pair.Key == null) dead.Add(pair.Key);
            else if (pair.Key.enabled) pair.Key.enabled = false;
        }
        foreach (Light light in dead) lamps.Remove(light);
        // Skyshop: the scene sky's own diffuse SH off (scene skies tint skin);
        // its specular carries our cube. Diffuse comes from Unity's ambient SH
        // x unityAmbientIntensity (_ExposureLM.x, not scaled by master).
        // camExposure multiplies Marmoset shaders' direct light: back to 1.
        // The visible sky stays the scene's: it draws its own skybox cube at
        // skyboxIntensity x camExposure (Sky.ComputeExposureVector .z).
        if (sky != null)
        {
            float sheen = CurrentStyle().Sheen;
            float skybox = oldSkybox * oldExposure;
            if (masterParam.val != 1f) masterParam.val = 1f;
            if (diffuseParam.val != 0f) diffuseParam.val = 0f;
            if (specularParam.val != sheen) specularParam.val = sheen;
            if (skyboxParam.val != skybox) skyboxParam.val = skybox;
            if (ambientParam.val != 1f) ambientParam.val = 1f;
            if (exposureParam.val != 1f) exposureParam.val = 1f;
            mset.SkyManager manager = mset.SkyManager.Get();
            mset.Sky global = manager != null ? manager.GlobalSky : null;
            if (global != envSky) { ReleaseSky(); TakeSky(global); }
            else if (envSky != null && envSky.SpecularCube != envCube) TakeSky(envSky);
        }
        // Custom mode: Unity renders the probe as given (VaM never rebuilds it).
        // Skybox mode did not: VaM has no skybox material, and the draws got
        // only the flat ambientLight (1.5.1, SimXR 20261020).
        if (RenderSettings.ambientMode != AmbientMode.Custom) RenderSettings.ambientMode = AmbientMode.Custom;
        if (RenderSettings.ambientLight != ambientColor) RenderSettings.ambientLight = ambientColor;
        if (RenderSettings.ambientProbe != envProbe) RenderSettings.ambientProbe = envProbe;
    }

    private void RestoreLamps()
    {
        foreach (KeyValuePair<Light, bool> pair in lamps) if (pair.Key != null) pair.Key.enabled = pair.Value;
    }

    private void RestoreEnvironment()
    {
        ReleaseSky();
        if (sky != null)
        {
            masterParam.val = oldMaster;
            diffuseParam.val = oldDiffuse;
            specularParam.val = oldSpecular;
            skyboxParam.val = oldSkybox;
            ambientParam.val = oldAmbient;
            exposureParam.val = oldExposure;
        }
        RenderSettings.ambientProbe = oldProbe;
        RenderSettings.ambientMode = oldAmbientMode;
        RenderSettings.ambientLight = oldAmbientColor;
    }

    private void BeforeSave()
    {
        if (!running) return;
        suspended = true;
        if (rig != null) rig.SetActive(false);
        RestoreLamps();
        RestoreEnvironment();
    }

    private void AfterSave()
    {
        if (!running || !suspended) return;
        suspended = false;
        if (rig != null) rig.SetActive(true);
        HoldOverride();
    }

    public string State()
    {
        return "running=" + running + ";suspended=" + suspended + ";style=" + styleSetting.Value + ";sources=" + lamps.Count + ";rig=" + rigLamps.Count + ";subject=" + (subject != null ? subject.uid : "") + ";menu=" + (menuPanel != null) + ";message=" + message;
    }

    // Read-only verification, also useful in logs. No scene discovery.
    public string LampState()
    {
        System.Text.StringBuilder text = new System.Text.StringBuilder();
        foreach (KeyValuePair<Light, bool> pair in lamps)
            if (pair.Key != null) text.Append(pair.Key.name).Append('|').Append(pair.Value).Append('|').Append(pair.Key.enabled).Append('\n');
        text.Append("FRAME|").Append(front.ToString("F3")).Append('|').Append(side).Append('|').Append(faceAim.ToString("F3"))
            .Append('|').Append(upperAim.ToString("F3")).Append('|').Append(bodyAim.ToString("F3")).Append('|').Append(targetYaw.ToString("F1", CultureInfo.InvariantCulture)).Append('\n');
        text.Append("ENV|").Append(envFlipped).Append('|').Append(RenderSettings.ambientMode).Append('|').Append(RenderSettings.ambientProbe == envProbe)
            .Append('|').Append(EnvLuminance(envProbe, front).ToString("F3", CultureInfo.InvariantCulture)).Append('|').Append(EnvLuminance(envProbe, Vector3.up).ToString("F3", CultureInfo.InvariantCulture))
            .Append('|').Append(EnvLuminance(envProbe, -front).ToString("F3", CultureInfo.InvariantCulture)).Append('|').Append(EnvLuminance(envProbe, Vector3.down).ToString("F3", CultureInfo.InvariantCulture)).Append('\n');
        text.Append("SKY|").Append(envSky != null ? envSky.name : "-").Append('|').Append(envSky != null && envSky.SpecularCube == envCube)
            .Append('|').Append(envSky != null ? envSky.transform.eulerAngles.y.ToString("F1", CultureInfo.InvariantCulture) : "-")
            .Append('|').Append(masterParam != null ? masterParam.val + "/" + diffuseParam.val + "/" + specularParam.val + "/" + skyboxParam.val : "-").Append('\n');
        foreach (RigLamp lamp in rigLamps)
        {
            Light light = lamp.Light;
            if (light != null) text.Append("RIG|").Append(light.name).Append('|').Append(light.transform.position.ToString("F4")).Append('|').Append(light.color.ToString("F4")).Append('|').Append(light.intensity.ToString(CultureInfo.InvariantCulture))
                .Append('|').Append(light.range.ToString("F3", CultureInfo.InvariantCulture)).Append('|').Append(light.spotAngle.ToString(CultureInfo.InvariantCulture))
                .Append('|').Append(light.shadowStrength.ToString(CultureInfo.InvariantCulture)).Append('|').Append(light.shadowNearPlane.ToString("F3", CultureInfo.InvariantCulture))
                .Append('|').Append(light.shadowCustomResolution).Append('|').Append(lamp.Spec.Motion).Append('\n');
        }
        return text.ToString();
    }

    private void OnDisable() { if (Instance == this) StopLighting(); }
    private void OnDestroy()
    {
        StopLighting();
        Camera.onPreCull -= BeforeCamera;
        if (controller != null && subscribed)
        {
            controller.onAtomAddedHandlers -= AtomAdded;
            controller.onAtomRemovedHandlers -= AtomRemoved;
            controller.onSceneLoadedHandlers -= SceneLoaded;
            controller.onSubSceneLoadedHandlers -= SubSceneLoaded;
            controller.onBeforeSceneSaveHandlers -= BeforeSave;
            controller.onSceneSavedHandlers -= AfterSave;
        }
        DestroyMenu();
        if (envCube != null) Destroy(envCube);
        if (Instance == this) Instance = null;
    }
}
