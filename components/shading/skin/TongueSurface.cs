using System;
using System.IO;
using System.Globalization;
using System.Reflection;
using UnityEngine;

// Approved surface assets are embedded in the plugin. Only GPU render materials
// are bound; the appearance's original textures and scene files are not edited.
internal static class TongueSurface
{
    private static Texture2D normal, gloss;
    private static Vector2[] layout;
    private static float shininess, specular;
    private static bool attempted;
    private static string error;
    private static bool missingLogged;

    private static byte[] Read(string name)
    {
        using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("Mouth." + name))
        {
            if (s == null) throw new FileNotFoundException("missing embedded mouth asset " + name);
            byte[] b = new byte[(int)s.Length]; int done = 0, n;
            while (done < b.Length && (n = s.Read(b, done, b.Length - done)) > 0) done += n;
            if (done != b.Length) throw new EndOfStreamException(name);
            return b;
        }
    }

    private static Texture2D Load(string kind)
    {
        Texture2D t = new Texture2D(2, 2, TextureFormat.RGBA32, true, true);
        t.name = "SkinTransmission-Tongue-0.15.1-" + kind;
        t.hideFlags = HideFlags.HideAndDontSave;
        if (!t.LoadImage(Read("Tongue" + kind + ".png")))
        {
            UnityEngine.Object.Destroy(t);
            throw new IOException("cannot decode tongue " + kind);
        }
        t.Apply(true, false); t.filterMode = FilterMode.Trilinear; t.anisoLevel = 4;
        return t;
    }

    private static bool Ready()
    {
        if (attempted) return error == null;
        attempted = true;
        // The tongue assets derive from VaM's mesh and textures and are not in every build: VaM's own tongue then.
        if (Assembly.GetExecutingAssembly().GetManifestResourceInfo("Mouth.TongueNormal.png") == null)
        {
            error = "not in this build (VaM's tongue look)";
            if (!missingLogged) { missingLogged = true; Debug.Log("VaM skin transmission: no tongue assets in this build; VaM's tongue look is kept"); }
            return false;
        }
        try
        {
            normal = Load("Normal"); gloss = Load("Gloss");
            using (BinaryReader r = new BinaryReader(new MemoryStream(Read("TongueLayout.bin"))))
            {
                layout = new Vector2[r.ReadInt32()];
                for (int i = 0; i < layout.Length; i++) layout[i] = new Vector2(r.ReadSingle(), r.ReadSingle());
            }
            string[] settings = System.Text.Encoding.ASCII.GetString(Read("TongueSurface.txt")).Split('\n');
            shininess = float.Parse(settings[0], CultureInfo.InvariantCulture);
            specular = float.Parse(settings[1], CultureInfo.InvariantCulture);
            return true;
        }
        catch (Exception e) { error = e.Message; return false; }
    }

    internal static void Dispose()
    {
        if (normal != null) UnityEngine.Object.Destroy(normal);
        if (gloss != null) UnityEngine.Object.Destroy(gloss);
        normal = gloss = null; layout = null; attempted = false; error = null;
    }

    internal sealed class Binding
    {
        private Mesh checkedMesh;
        private int checkedSub = -1;
        private bool compatible;
        private Material material;
        private Texture originalNormal, originalGloss;
        private float originalOffset, originalShininess, originalSpecular;
        internal string State = "waiting";

        internal void Update(Material m, Mesh mesh, int sub)
        {
            if (!Ready()) { Restore(); State = "asset error: " + error; return; }
            if (mesh == null || m == null) { Restore(); return; }
            if (mesh != checkedMesh || sub != checkedSub)
            {
                Restore(); checkedMesh = mesh; checkedSub = sub;
                int[] triangles = mesh.GetTriangles(sub); Vector2[] uv = mesh.uv;
                compatible = triangles.Length == layout.Length;
                for (int i = 0; compatible && i < triangles.Length; i++)
                    compatible = (uv[triangles[i]] - layout[i]).sqrMagnitude < 1e-10f;
            }
            if (!compatible) { Restore(); State = "original: different tongue UV layout"; return; }
            if (!m.HasProperty("_BumpMap") || !m.HasProperty("_GlossTex") || !m.HasProperty("_Shininess") || !m.HasProperty("_SpecInt"))
            { Restore(); State = "original: unsupported tongue shader"; return; }
            if (material != m)
            {
                Restore(); material = m;
                originalNormal = m.GetTexture("_BumpMap"); originalGloss = m.GetTexture("_GlossTex");
                originalOffset = m.GetFloat("_GlossOffset"); originalShininess = m.GetFloat("_Shininess"); originalSpecular = m.GetFloat("_SpecInt");
            }
            // A preset can write the same GPU Material again. Remember its new
            // values before binding, so disabling restores the current preset.
            if (m.GetTexture("_BumpMap") != normal) { originalNormal = m.GetTexture("_BumpMap"); m.SetTexture("_BumpMap", normal); }
            if (m.GetTexture("_GlossTex") != gloss) { originalGloss = m.GetTexture("_GlossTex"); m.SetTexture("_GlossTex", gloss); }
            if (m.GetFloat("_GlossOffset") != 0f) { originalOffset = m.GetFloat("_GlossOffset"); m.SetFloat("_GlossOffset", 0f); }
            if (m.GetFloat("_Shininess") != shininess) { originalShininess = m.GetFloat("_Shininess"); m.SetFloat("_Shininess", shininess); }
            if (m.GetFloat("_SpecInt") != specular) { originalSpecular = m.GetFloat("_SpecInt"); m.SetFloat("_SpecInt", specular); }
            State = "bound: approved tongue 0.15.1";
        }

        internal void Restore()
        {
            if (material != null)
            {
                if (material.GetTexture("_BumpMap") == normal) material.SetTexture("_BumpMap", originalNormal);
                if (material.GetTexture("_GlossTex") == gloss) material.SetTexture("_GlossTex", originalGloss);
                if (material.GetFloat("_GlossOffset") == 0f) material.SetFloat("_GlossOffset", originalOffset);
                if (material.GetFloat("_Shininess") == shininess) material.SetFloat("_Shininess", originalShininess);
                if (material.GetFloat("_SpecInt") == specular) material.SetFloat("_SpecInt", originalSpecular);
            }
            material = null; originalNormal = originalGloss = null; State = "original";
        }
    }
}
