// Budget arithmetic of the VaM hair budget plugin, free of Unity so that the
// offline test (tests\BudgetMathTest.cs) runs exactly this code on the library.
using System;
using System.Collections.Generic;

public sealed class HairCost
{
    public int patches;            // scalp triangles of guide strands (index count / 3)
    public int segments;           // guide segments per strand
    public int requestedDensity;   // curveDensity from the slider
    public int requestedDetail;    // hairMultiplier from the slider
    public float curlStrength;     // curlScale x max(curlRoot, curlMid, curlTip)
    public float curlFrequency;    // curlFrequency slider, 0..20
    public float level;            // working per-patch cost, Cost(curveDensity, hairMultiplier)
    public int density, detail;    // result
}

public static class HairBudgetMath
{
    // GPU cost of drawing hair (measured in VaM 2026-10-15): at the same segment count, halving
    // hairMultiplier (strands) saves about twice what halving curveDensity (points per strand) saves; a strand costs
    // about its segments plus StrandCost more (fitted per scene: 22-29). Costs are in segments of a hair with
    // curveDensity CostReferenceDensity, which costs exactly its segment count; that is the library's mean
    // curveDensity per strand, so over the library the total is the segment total and budgets keep their unit.
    public const float StrandCost = 25f;
    public const float CostReferenceDensity = 24f;
    private const float CostScale = CostReferenceDensity / (CostReferenceDensity + StrandCost);

    // Per-patch cost of hairMultiplier strands of curveDensity segments.
    public static float Cost(float curveDensity, float hairMultiplier)
    {
        return hairMultiplier * (curveDensity + StrandCost) * CostScale;
    }

    // Per-patch cost as requested, hairMultiplier x curveDensity first capped at densityMax (0 = no cap; both
    // lowered by the same factor, as Split does).
    public static float RequestedLevel(HairCost h, int densityMax)
    {
        float rc = Math.Max(1, h.requestedDensity), rh = Math.Max(1, h.requestedDetail);
        if (densityMax > 0 && rc * rh > densityMax)
        {
            float s = (float)Math.Sqrt(densityMax / (rc * rh));
            rc *= s; rh *= s;
        }
        return Cost(rc, rh);
    }

    // Share of a budget below which an item counts as a cheap detail piece:
    // lowering it would save little and thin it out a lot.
    public const double SmallItemShare = 0.05;

    // Lowers the costliest items per patch to a common per-patch level until
    // the group total (patches x level) fits; items already below that level,
    // and cheap detail pieces, keep theirs.
    public static void WaterFill(List<HairCost> group, long budget, float floor)
    {
        if (budget <= 0 || group.Count == 0) return;
        double total = 0;
        foreach (HairCost h in group) total += (double)h.patches * h.level;
        if (total <= budget) return;
        double small = budget * SmallItemShare, kept = 0;
        List<HairCost> pool = new List<HairCost>();
        foreach (HairCost h in group)
        {
            double cost = (double)h.patches * h.level;
            if (cost <= small) kept += cost; else pool.Add(h);
        }
        if (pool.Count == 0 || kept >= budget * 0.5) { pool = new List<HairCost>(group); kept = 0; }
        long patches = 0;
        foreach (HairCost h in pool) patches += h.patches;
        if (patches <= 0) return;
        pool.Sort(delegate(HairCost a, HairCost b) { return a.level.CompareTo(b.level); });
        double rest = budget - kept;
        float level = -1f;
        foreach (HairCost h in pool)
        {
            double cap = rest / patches;
            if (h.level <= cap) { rest -= (double)h.patches * h.level; patches -= h.patches; continue; }
            level = (float)Math.Max(0.0, cap);
            break;
        }
        if (level < 0f) return;   // everything fits after all (rounding)
        level = Math.Max(level, floor);
        foreach (HairCost h in pool) if (h.level > level) h.level = level;
    }

    // Curls below this strength are a slight wave that needs no extra points.
    public const float CurlStrengthMin = 0.03f;
    public const float PointsPerCurl = 6f;

    // Points along a strand that its curls need (0 = no curls to protect).
    public static int CurlPoints(HairCost h)
    {
        if (h.curlStrength < CurlStrengthMin) return 0;
        return (int)Math.Ceiling(PointsPerCurl * h.curlFrequency);
    }

    // Lowest curveDensity Split may choose: about one point per 1.33 guide
    // segments, and enough points for the curls.
    public static int DensityFloor(HairCost h, int curveDensityMin)
    {
        int rc = Math.Max(1, h.requestedDensity);
        int floor = Math.Max(curveDensityMin, (int)Math.Ceiling(0.75 * h.segments));
        return Math.Min(rc, Math.Max(floor, CurlPoints(h)));
    }

    // Splits the reduction from requested to level between curveDensity and
    // hairMultiplier: both by the same factor (the one that makes the cost
    // level), within their floors. curveDensity is the number of points along
    // each drawn strand, hairMultiplier the number of strands per patch.
    public static void Split(HairCost h, int curveDensityMin, int hairMultiplierMin)
    {
        int rc = Math.Max(1, h.requestedDensity), rh = Math.Max(1, h.requestedDetail);
        h.density = rc;
        h.detail = rh;
        if (h.level >= Cost(rc, rh) - 0.5f) return;
        // Cost(rc s, rh s) = level:  rc rh CostScale s^2 + rh StrandCost CostScale s - level = 0
        double a = (double)rc * rh * CostScale, b = (double)rh * StrandCost * CostScale;
        double s = (-b + Math.Sqrt(b * b + 4.0 * a * Math.Max(0f, h.level))) / (2.0 * a);
        int floorCd = DensityFloor(h, curveDensityMin);
        int floorHm = Math.Min(rh, Math.Max(1, hairMultiplierMin));
        int cd = Clamp(Round(rc * s), floorCd, rc);
        int hm = Clamp(Round(h.level / ((cd + StrandCost) * CostScale)), floorHm, rh);
        if (hm == floorHm && Cost(cd, hm) > h.level * 1.15f) cd = Clamp(Round(h.level / (hm * CostScale) - StrandCost), floorCd, rc);
        h.density = cd;
        h.detail = hm;
    }

    // Width factor that keeps the coverage (strands x width) when hairMultiplier
    // drops, at most compensation.
    public static float WidthScale(int requestedDetail, int detail, float compensation)
    {
        if (detail >= requestedDetail || compensation <= 1f || detail <= 0) return 1f;
        return (float)Math.Min(compensation, Math.Sqrt((double)requestedDetail / detail));
    }

    private static int Round(double v) { return (int)Math.Round(v, MidpointRounding.AwayFromZero); }
    private static int Clamp(int v, int low, int high) { return v < low ? low : v > high ? high : v; }
}
