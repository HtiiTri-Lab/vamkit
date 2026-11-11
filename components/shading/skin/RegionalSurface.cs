using System;
using System.IO;
using System.Reflection;
using UnityEngine;

// Called only when a mesh is baked. Unsupported UV layouts keep the uniform pipeline.
internal static class RegionalSurface
{
    private static Vector2[] layout;
    private static float[] response;
    private static bool missing;
    internal static float[] ForMesh(Vector2[] uv, out string state)
    {
        try
        {
            // The regional map derives from VaM's mesh and is not in every build: the uniform pipeline then.
            if (layout == null && !missing && Assembly.GetExecutingAssembly().GetManifestResourceInfo("Regional.FemaleRegional.bin") == null)
            {
                missing = true;
                Debug.Log("VaM skin transmission: no regional map in this build; the uniform skin response is used");
            }
            if (missing) { state = "uniform: no regional map in this build"; return null; }
            if (layout == null)
            {
                using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("Regional.FemaleRegional.bin"))
                using (BinaryReader r = new BinaryReader(s))
                {
                    if (r.ReadUInt32() != 0x31524746) throw new IOException("regional map signature");
                    int n = r.ReadInt32();
                    if (n < 1 || n > 1000000) throw new IOException("regional map size");
                    Vector2[] u = new Vector2[n]; float[] w = new float[n];
                    for (int i = 0; i < n; i++)
                    {
                        u[i] = new Vector2(r.ReadSingle(), r.ReadSingle()); w[i] = r.ReadSingle();
                        if (float.IsNaN(w[i]) || w[i] < .5f || w[i] > 1.08f) throw new IOException("regional map range");
                    }
                    layout = u; response = w;
                }
            }
            if (uv.Length != layout.Length) { state = "uniform: vertex count differs"; return null; }
            for (int i = 0; i < uv.Length; i++)
                if (!(Mathf.Abs(uv[i].x-layout[i].x) < .00001f && Mathf.Abs(uv[i].y-layout[i].y) < .00001f))
                { state = "uniform: unsupported UV layout at vertex " + i; return null; }
            state = "female regional v1: " + response.Length + " UV vertices verified";
            return response;
        }
        catch (Exception e) { state = "uniform: regional asset " + e.Message; return null; }
    }
}
