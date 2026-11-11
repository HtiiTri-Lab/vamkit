// An index mask over GPUCollidersManager's own ordered lists. Only the hair solver reads it; the game's colliders and
// fallback solver remain untouched.
//   1  a breast collider (l/rPectoral, BreastPhysicsMesh): position-only contact
//   2  the same, replaced for the hair by the breast as SoftBody draws it. VaMSoftBody.BreastSpheres gives her surface
//      spheres (centres deep in the soft body, radii to the drawn surface, on the pose of the skin VaM uploads next);
//      they go to the solver with every collider update, per breast behind a bound. VaM's colliders follow its own
//      breast physics, which the drawn breast no longer shows, so hair resting on them floated above it or sank in.
// Without the spheres (no SoftBody, her soft body off, nothing solved yet) her colliders stay 1.
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using GPUTools.Physics.Scripts.Behaviours;
using HarmonyLib;
using UnityEngine;

internal static class HairBodyContact
{
    static bool dirty = true;
    static GPUCollidersManager owner;
    static ComputeBuffer buffer;
    static uint[] flags;
    static string report = "not built";
    static int rebuilds;
    public static IntPtr Buffer;
    public static int SphereCount;
    const BindingFlags Any = BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;

    // a person with tagged breast colliders
    sealed class Body
    {
        public Atom atom;
        public DAZCharacterRun run;
        public Transform left, right;                 // her pectorals: which breast a sphere belongs to
        public readonly List<int> colliders = new List<int>();
        public bool soft;
        public int spheres;
    }
    static readonly List<Body> bodies = new List<Body>();
    delegate int SpheresFn(DAZCharacterRun run, Vector4[] buf);
    static SpheresFn breastSpheres;
    static Func<int> presentationRevision;
    static int lastPresentationRevision=-1;
    internal static bool PresentationChanged()
    { return presentationRevision!=null && presentationRevision()!=lastPresentationRevision; }
    internal static void RefreshPresentation() { UpdateBodies(); }
    static bool looked;
    static string softError;
    static Vector4[] got = new Vector4[256], packed = new Vector4[512];
    static ComputeBuffer bodyBuffer;
    public static IntPtr BodyBuffer;
    public static int BodyCount;
    public static bool Drawn = true;                  // VaMHairSolver.SetDrawnBreasts (tests)

    static DAZBone Pectoral(Component c)
    {
        for (Transform t = c.transform; t != null; t = t.parent)
        {
            DAZBone bone = t.GetComponent<DAZBone>();
            if (bone != null && (bone.name == "lPectoral" || bone.name == "rPectoral")) return bone;
            AutoCollider auto = t.GetComponent<AutoCollider>();
            if (auto != null && auto.bone != null && (auto.bone.name == "lPectoral" || auto.bone.name == "rPectoral")) return auto.bone;
        }
        return null;
    }

    static bool IsBreast(Component c)
    {
        if (Pectoral(c) != null) return true;
        for (Transform t = c.transform; t != null; t = t.parent)
        {
            DAZPhysicsMesh mesh = t.GetComponent<DAZPhysicsMesh>();
            if (mesh != null && mesh.name == "BreastPhysicsMesh") return true;
        }
        return false;
    }

    // on a rebuild only: her atom, its character and her pectorals
    static Body BodyOf(Component c)
    {
        Atom atom = c.GetComponentInParent<Atom>();
        if (atom == null) return null;
        Body b = null;
        foreach (Body x in bodies) if (x.atom == atom) { b = x; break; }
        if (b == null)
        {
            b = new Body { atom = atom };
            foreach (DAZCharacterRun r in atom.GetComponentsInChildren<DAZCharacterRun>(false))
                if (r.isActiveAndEnabled && r.skin != null) { b.run = r; break; }
            bodies.Add(b);
        }
        DAZBone p = Pectoral(c);
        if (p != null) { if (p.name == "lPectoral") b.left = p.transform; else b.right = p.transform; }
        return b;
    }

    static string Path(Component c)
    {
        string p = c.name;
        for (Transform t = c.transform.parent; t != null; t = t.parent) p = t.name + "/" + p;
        return p;
    }

    static void Changed() { dirty = true; }
    static void Destroyed(GPUCollidersManager __instance)
    {
        if (owner != __instance) return;
        Buffer = IntPtr.Zero;
        if (buffer != null) buffer.Release();
        buffer = null; flags = null; owner = null; SphereCount = 0; dirty = true;
        BodyBuffer = IntPtr.Zero; BodyCount = 0;
        if (bodyBuffer != null) bodyBuffer.Release();
        bodyBuffer = null;
        bodies.Clear();
        report = "not built";
    }

    static void Computed(GPUCollidersManager __instance, List<GpuSphereCollider> ___sphereColliders,
                         List<CapsuleLineSphereCollider> ___lineSphereColliders)
    {
        int ns = ___sphereColliders.Count, nl = ___lineSphereColliders.Count, n = ns + nl;
        if (owner != __instance || flags == null || flags.Length != n || SphereCount != ns) dirty = true;
        if (dirty) Rebuild(__instance, ___sphereColliders, ___lineSphereColliders);
        UpdateBodies();
    }

    static void Rebuild(GPUCollidersManager manager, List<GpuSphereCollider> sphereColliders, List<CapsuleLineSphereCollider> lineSphereColliders)
    {
        int ns = sphereColliders.Count, nl = lineSphereColliders.Count, n = ns + nl;
        rebuilds++;
        Buffer = IntPtr.Zero;
        owner = manager;
        SphereCount = ns;
        if (flags == null || flags.Length != n)
        {
            if (buffer != null) buffer.Release();
            buffer = n > 0 ? new ComputeBuffer(n, 4) : null;
            flags = new uint[n];
        }
        bodies.Clear();
        BodyCount = 0;
        StringBuilder b = new StringBuilder();
        int tagged = 0;
        for (int i = 0; i < n; i++)
        {
            Component c = i < ns ? (Component)sphereColliders[i] : lineSphereColliders[i - ns];
            bool breast = c != null && IsBreast(c);
            flags[i] = breast ? 1u : 0u;
            if (!breast) continue;
            tagged++;
            b.Append("\n").Append(i < ns ? "sphere " : "capsule ").Append(Path(c));
            Body body = BodyOf(c);
            if (body != null) body.colliders.Add(i);
        }
        if (buffer != null)
        {
            buffer.SetData(flags);
            Buffer = buffer.GetNativeBufferPtr(); // only on registration/rebuild, never per hair step
        }
        report = "position-only breasts=" + tagged + " spheres=" + ns + " capsules=" + nl + b;
        dirty = false;
    }

    static void Look()
    {
        looked = true;
        foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
        {
            Type t = a.GetType("VaMSoftBody");
            MethodInfo m = t != null ? t.GetMethod("PresentedBreastSpheres", BindingFlags.Static | BindingFlags.Public, null,
                new Type[] { typeof(DAZCharacterRun), typeof(Vector4[]) }, null) : null;
            if(m==null && t!=null)m=t.GetMethod("BreastSpheres",BindingFlags.Static|BindingFlags.Public,null,
                new Type[] { typeof(DAZCharacterRun),typeof(Vector4[]) },null);
            if (m != null && m.ReturnType == typeof(int))
            {
                breastSpheres = (SpheresFn)Delegate.CreateDelegate(typeof(SpheresFn), m);
                PropertyInfo revision=t.GetProperty("PresentationRevision",BindingFlags.Static|BindingFlags.Public);
                if(revision!=null)presentationRevision=(Func<int>)Delegate.CreateDelegate(typeof(Func<int>),revision.GetGetMethod());
                return;
            }
        }
    }

    // her surface spheres into got; a negative answer is the count she needs
    static int Spheres(DAZCharacterRun run)
    {
        int n = breastSpheres(run, got);
        if (n < 0) { got = new Vector4[Mathf.NextPowerOfTwo(-n)]; n = breastSpheres(run, got); }
        return Math.Max(n, 0);
    }

    // every collider update (main thread, the waiting hair steps already ran): the drawn breasts, and the mask with them
    static void UpdateBodies()
    {
        if (!looked) Look();
        if (breastSpheres == null || flags == null) return;
        try
        {
            int k = 0;
            bool changed = false;
            foreach (Body b in bodies)
            {
                int n = Drawn && b.run != null && b.left != null && b.right != null ? Spheres(b.run) : 0;
                b.spheres = n;
                if ((n > 0) != b.soft) { b.soft = n > 0; changed = true; }
                if (n == 0) continue;
                k = Pack(k, n, b.left.position, b.right.position);
                k = Pack(k, n, b.right.position, b.left.position);
            }
            if (k > 0)
            {
                if (bodyBuffer == null || bodyBuffer.count < k)
                {
                    if (bodyBuffer != null) bodyBuffer.Release();
                    bodyBuffer = new ComputeBuffer(Mathf.NextPowerOfTwo(Math.Max(k, 64)), 16);
                    BodyBuffer = bodyBuffer.GetNativeBufferPtr();   // only when it grows
                }
                bodyBuffer.SetData(packed, 0, 0, k);
            }
            BodyCount = k;
            if(presentationRevision!=null)lastPresentationRevision=presentationRevision();
            if (changed)
            {
                foreach (Body b in bodies) foreach (int i in b.colliders) flags[i] = b.soft ? 2u : 1u;
                if (buffer != null) buffer.SetData(flags);
            }
        }
        catch (Exception e)
        {
            breastSpheres = null; presentationRevision=null;softError = e.Message; BodyCount = 0;
            for (int i = 0; i < flags.Length; i++) if (flags[i] == 2u) flags[i] = 1u;
            if (buffer != null) buffer.SetData(flags);
        }
    }

    // the spheres of got nearer to `side` than to `other`, after their bound and count
    static int Pack(int k, int n, Vector3 side, Vector3 other)
    {
        if (packed.Length < k + 2 + n) Array.Resize(ref packed, Mathf.NextPowerOfTwo(k + 2 + n));
        Vector3 sum = Vector3.zero;
        int m = 0;
        for (int i = 0; i < n; i++)
        {
            Vector3 c = got[i];
            if ((c - side).sqrMagnitude > (c - other).sqrMagnitude) continue;
            packed[k + 2 + m++] = got[i];
            sum += c;
        }
        if (m == 0) return k;
        Vector3 centre = sum / m;
        float r = 0;
        for (int i = 0; i < m; i++) { Vector4 s = packed[k + 2 + i]; r = Mathf.Max(r, ((Vector3)s - centre).magnitude + s.w); }
        packed[k] = new Vector4(centre.x, centre.y, centre.z, r);
        packed[k + 1] = new Vector4(m, 0, 0, 0);       // a value, not int bits: Mono flushes those denormals to 0
        return k + 2 + m;
    }

    public static string Report()
    {
        StringBuilder b = new StringBuilder(report).Append("\nmask rebuilds=").Append(rebuilds);
        b.Append("\ndrawn breasts: ").Append(breastSpheres != null ? "SoftBody spheres" : looked ? "no SoftBody spheres" : "not looked");
        if (softError != null) b.Append(" (failed: ").Append(softError).Append(")");
        foreach (Body x in bodies)
            b.Append("\n  ").Append(x.atom != null ? x.atom.uid : "?").Append(x.soft ? " drawn breasts, spheres=" + x.spheres : " VaM colliders")
             .Append(", colliders=").Append(x.colliders.Count);
        return b.Append("\n  entries=").Append(BodyCount).ToString();
    }

    public static void Install(Harmony h)
    {
        Type t = typeof(GPUCollidersManager);
        foreach (string name in new string[] { "RegSphereCollider", "DeregSphereCollider", "RegLineSphereCollider", "DeregLineSphereCollider" })
            h.Patch(t.GetMethod(name, Any), null, new HarmonyMethod(typeof(HairBodyContact).GetMethod("Changed", Any)), null, null);
        h.Patch(t.GetMethod("ComputeColliders", Any), null, new HarmonyMethod(typeof(HairBodyContact).GetMethod("Computed", Any)), null, null);
        h.Patch(t.GetMethod("OnDestroy", Any), new HarmonyMethod(typeof(HairBodyContact).GetMethod("Destroyed", Any)), null, null, null);
    }
}
