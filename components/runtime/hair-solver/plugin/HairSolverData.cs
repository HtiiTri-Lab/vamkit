// What VaMHairSolver hands the native solver for one item, built from GPUTools' joint data, and the checks that the
// data is what the solver models. Pure functions (no Unity calls), so plugin\tests\DataTest.cs runs them offline.
using System;
using GPUTools.Common.Scripts.PL.Tools;
using GPUTools.Physics.Scripts.Types.Joints;

internal struct HairSolverClingEntry
{
    public int other;
    public float rest, elasticity;
}

internal sealed class HairSolverItemData
{
    public float[] rest2;                      // per particle: x distance / spline rest of (i-1, i), y compression rest of (i-2, i)
    public uint[] clingOffsets;                // n + 1
    public HairSolverClingEntry[] clingEntries;   // both ends of every cling joint (at least one element)
    public int clingEntryCount, clingJoints;
    public int distanceSweeps, compressionSweeps, clingSweeps;
    public bool hasCompression;
}

internal static class HairSolverData
{
    // How often a GPUTools grouped kernel sweeps its colours: GroupsData must be the groups' (Start, Num) repeated.
    // GPUTools' builders read GroupedData.Data twice in OnBuild and every read appends the groups again, so a built
    // item sweeps twice. Returns 0 and why when GroupsData is something else.
    public static int Sweeps(GroupedData<GPDistanceJoint> g, out string why)
    {
        why = null;
        int groups = g.Groups.Count, entries = g.GroupsData.Count;
        if (groups == 0) return entries == 0 ? 1 : 0;
        if (entries == 0 || entries % groups != 0) { why = entries + " group entries for " + groups + " groups"; return 0; }
        for (int e = 0; e < entries; e++)
        {
            int start = 0;
            for (int q = 0; q < e % groups; q++) start += g.Groups[q].Count;
            GroupData d = g.GroupsData[e];
            if (d.Start != start || d.Num != g.Groups[e % groups].Count) { why = "group entry " + e + " is not a repeat of the groups"; return 0; }
        }
        return entries / groups;
    }

    // n particles in strands of seg points. pointJoints / splineRest: GPUTools' PointJoints / PointToPreviousPointDistances
    // data; distance / compression / cling: the joint buffers' data with the kernels' GroupedData (compression and cling
    // may be empty or null). Returns why the solver cannot run the item, or null and the data.
    public static string Build(int n, int seg, GPPointJoint[] pointJoints, float[] splineRest,
        GPDistanceJoint[] distance, GroupedData<GPDistanceJoint> distanceGroups,
        GPDistanceJoint[] compression, GroupedData<GPDistanceJoint> compressionGroups,
        GPDistanceJoint[] cling, GroupedData<GPDistanceJoint> clingGroups, out HairSolverItemData data)
    {
        data = null;
        if (seg < 2 || seg > 256 || n <= 0 || n % seg != 0) return n + " particles, " + seg + " points per strand (solver: 2-256)";
        if (pointJoints == null || pointJoints.Length != n || splineRest == null || splineRest.Length != n)
            return "point joints / rest lengths not one per particle";
        for (int i = 0; i < n; i++) if (pointJoints[i].BodyId != i) return "point joint " + i + " belongs to particle " + pointJoints[i].BodyId;
        HairSolverItemData d = new HairSolverItemData();
        d.rest2 = new float[2 * n];
        bool[] seen = new bool[n];
        string why;

        // distance joints: (i-1, i) for every non-root point; odd i in the first colour, even in the second
        if (distanceGroups == null) return "distance joints: no groups";
        d.distanceSweeps = Sweeps(distanceGroups, out why);
        if (why != null) return "distance joints: " + why;
        if (distance == null || distanceGroups.Groups.Count != 2 || distance.Length != n - n / seg ||
            distanceGroups.Groups[0].Count + distanceGroups.Groups[1].Count != distance.Length)
            return "distance joints: not one per segment in two colours";
        for (int q = 0, colour = 0; colour < 2; colour++)
            for (int e = 0; e < distanceGroups.Groups[colour].Count; e++, q++)
            {
                GPDistanceJoint j = distance[q];
                int b = j.Body2Id;
                if (b <= 0 || b >= n || j.Body1Id != b - 1 || b % seg == 0 || seen[b] || (b & 1) != (colour == 0 ? 1 : 0))
                    return "distance joint " + q + " (" + j.Body1Id + ", " + b + ") is not what the solver models";
                if (Math.Abs(j.Distance - splineRest[b]) > 1e-5f * Math.Max(1f, Math.Abs(splineRest[b])))
                    return "distance joint " + q + " rest " + j.Distance + " differs from the spline rest " + splineRest[b];
                seen[b] = true;
                d.rest2[2 * b] = j.Distance;
            }

        // compression joints: (i-2, i) for every point from the third on, i % 4 in {2, 3} first; or none at all
        if (compressionGroups == null) return "compression joints: no groups";
        d.compressionSweeps = Sweeps(compressionGroups, out why);
        if (why != null) return "compression joints: " + why;
        d.hasCompression = compression != null && compression.Length > 0;
        if (d.hasCompression)
        {
            if (compressionGroups.Groups.Count != 2 || compression.Length != n - 2 * (n / seg) ||
                compressionGroups.Groups[0].Count + compressionGroups.Groups[1].Count != compression.Length)
                return "compression joints: not one per point in two colours";
            Array.Clear(seen, 0, n);
            for (int q = 0, colour = 0; colour < 2; colour++)
                for (int e = 0; e < compressionGroups.Groups[colour].Count; e++, q++)
                {
                    GPDistanceJoint j = compression[q];
                    int b = j.Body2Id;
                    if (b < 2 || b >= n || j.Body1Id != b - 2 || b % seg < 2 || seen[b] || ((b & 3) >= 2) != (colour == 0))
                        return "compression joint " + q + " (" + j.Body1Id + ", " + b + ") is not what the solver models";
                    seen[b] = true;
                    d.rest2[2 * b + 1] = j.Distance;
                }
        }

        // cling joints: any pairs; adjacency over particles, both ends, in joint order
        if (clingGroups == null) return "cling joints: no groups";
        d.clingSweeps = Sweeps(clingGroups, out why);
        if (why != null) return "cling joints: " + why;
        int joints = cling != null ? cling.Length : 0;
        uint[] off = new uint[n + 1];
        for (int q = 0; q < joints; q++)
        {
            if (cling[q].Body1Id < 0 || cling[q].Body1Id >= n || cling[q].Body2Id < 0 || cling[q].Body2Id >= n)
                return "cling joint " + q + " out of range";
            off[cling[q].Body1Id + 1]++;
            off[cling[q].Body2Id + 1]++;
        }
        for (int i = 0; i < n; i++) off[i + 1] += off[i];
        HairSolverClingEntry[] ent = new HairSolverClingEntry[Math.Max(1, (int)off[n])];
        uint[] fill = new uint[n];
        Array.Copy(off, fill, n);
        for (int q = 0; q < joints; q++)
        {
            GPDistanceJoint j = cling[q];
            HairSolverClingEntry a = new HairSolverClingEntry(), b = new HairSolverClingEntry();
            a.other = j.Body2Id; a.rest = j.Distance; a.elasticity = j.Elasticity;
            b.other = j.Body1Id; b.rest = j.Distance; b.elasticity = j.Elasticity;
            ent[fill[j.Body1Id]++] = a;
            ent[fill[j.Body2Id]++] = b;
        }
        d.clingOffsets = off;
        d.clingEntries = ent;
        d.clingEntryCount = (int)off[n];
        d.clingJoints = joints;
        data = d;
        return null;
    }
}
