// Offline test of HairBudgetMath.cs (the plugin's own budget code) on the
// library exported by audit_hair.py: every person preset is put through the
// High / Balanced / Performance limits as the plugin would (per-item density
// cap, then the person budget), and invariants are checked.
// Usage: BudgetMathTest.exe <preset-items.tsv> <out.csv>
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

public static class BudgetMathTest
{
    private sealed class Profile
    {
        public string name; public long budget; public int densityMax, cdMin, hmMin; public float widthComp;
        public Profile(string name, long budgetK, int densityMax, int cdMin, int hmMin, float widthComp)
        { this.name = name; budget = budgetK * 1000; this.densityMax = densityMax; this.cdMin = cdMin; this.hmMin = hmMin; this.widthComp = widthComp; }
    }

    // Keep in step with VaMHairBudget.Preset().
    private static readonly Profile[] Profiles = {
        new Profile("High", 1200, 2048, 16, 6, 1.3f),
        new Profile("Balanced", 600, 1024, 12, 4, 1.5f),
        new Profile("Performance", 350, 640, 10, 3, 1.6f),
    };

    public static int Main(string[] args)
    {
        Dictionary<string, List<string[]>> presets = new Dictionary<string, List<string[]>>();
        List<string> order = new List<string>();
        foreach (string line in File.ReadAllLines(args[0]))
        {
            string[] f = line.Split('\t');
            if (f.Length != 8) continue;
            List<string[]> list;
            if (!presets.TryGetValue(f[0], out list)) { presets[f[0]] = list = new List<string[]>(); order.Add(f[0]); }
            list.Add(f);
        }
        int failures = 0;
        using (StreamWriter csv = new StreamWriter(args[1]))
        {
            csv.WriteLine("profile,preset,items,before,after,ratio,over_budget,min_hairMultiplier_ratio,min_curveDensity_ratio,max_width_scale");
            foreach (Profile p in Profiles)
            {
                List<double> ratios = new List<double>(), segRatios = new List<double>();
                int changed = 0, over = 0, items = 0, itemsChanged = 0;
                double worstHm = 1, worstCd = 1;
                foreach (string key in order)
                {
                    List<HairCost> costs = new List<HairCost>();
                    double before = 0, segBefore = 0;
                    foreach (string[] f in presets[key])
                    {
                        HairCost c = new HairCost();
                        c.patches = int.Parse(f[2]); c.segments = int.Parse(f[3]);
                        c.requestedDensity = int.Parse(f[4]); c.requestedDetail = int.Parse(f[5]);
                        c.curlStrength = float.Parse(f[6], CultureInfo.InvariantCulture); c.curlFrequency = float.Parse(f[7], CultureInfo.InvariantCulture);
                        if (c.patches <= 0) continue;
                        c.level = HairBudgetMath.RequestedLevel(c, p.densityMax);
                        before += (double)c.patches * HairBudgetMath.Cost(c.requestedDensity, c.requestedDetail);
                        segBefore += (double)c.patches * c.requestedDensity * c.requestedDetail;
                        costs.Add(c);
                    }
                    if (costs.Count == 0) continue;
                    HairBudgetMath.WaterFill(costs, p.budget, HairBudgetMath.Cost(Math.Max(1, p.cdMin), Math.Max(1, p.hmMin)));
                    double after = 0, segAfter = 0, minHm = 1, minCd = 1, maxWidth = 1;
                    foreach (HairCost c in costs)
                    {
                        float level = c.level;
                        HairBudgetMath.Split(c, p.cdMin, p.hmMin);
                        items++;
                        after += (double)c.patches * HairBudgetMath.Cost(c.density, c.detail);
                        segAfter += (double)c.patches * c.density * c.detail;
                        if (c.density < 1 || c.detail < 1 || c.density > c.requestedDensity || c.detail > c.requestedDetail)
                        { failures++; Console.WriteLine("FAIL range " + p.name + " " + key); }
                        if (c.density < HairBudgetMath.DensityFloor(c, p.cdMin) || c.detail < Math.Min(c.requestedDetail, p.hmMin))
                        { failures++; Console.WriteLine("FAIL below a floor " + p.name + " " + key); }
                        if (level >= HairBudgetMath.Cost(c.requestedDensity, c.requestedDetail) - 0.5f && (c.density != c.requestedDensity || c.detail != c.requestedDetail))
                        { failures++; Console.WriteLine("FAIL changed an item within its level " + p.name + " " + key); }
                        if (c.density != c.requestedDensity || c.detail != c.requestedDetail) itemsChanged++;
                        minHm = Math.Min(minHm, (double)c.detail / c.requestedDetail);
                        minCd = Math.Min(minCd, (double)c.density / c.requestedDensity);
                        maxWidth = Math.Max(maxWidth, HairBudgetMath.WidthScale(c.requestedDetail, c.detail, p.widthComp));
                    }
                    if (after > before + 0.5 || segAfter > segBefore + 0.5) { failures++; Console.WriteLine("FAIL grew " + p.name + " " + key); }
                    segRatios.Add(segAfter / segBefore);
                    bool overBudget = after > p.budget * 1.15;
                    if (overBudget) over++;
                    if (after < before - 0.5) changed++;
                    ratios.Add(after / before);
                    worstHm = Math.Min(worstHm, minHm);
                    worstCd = Math.Min(worstCd, minCd);
                    csv.WriteLine(string.Join(",", new string[] { p.name, "\"" + key.Replace("\"", "'") + "\"", costs.Count.ToString(),
                        ((long)before).ToString(), ((long)after).ToString(), (after / before).ToString("0.000", CultureInfo.InvariantCulture),
                        overBudget ? "1" : "0", minHm.ToString("0.00", CultureInfo.InvariantCulture), minCd.ToString("0.00", CultureInfo.InvariantCulture),
                        maxWidth.ToString("0.00", CultureInfo.InvariantCulture) }));
                }
                ratios.Sort();
                segRatios.Sort();
                Console.WriteLine(string.Format(CultureInfo.InvariantCulture,
                    "{0,-12} presets {1}: changed {2} ({3:0}%), items changed {4}/{5}, over budget x1.15 {6}; kept cost p10 {7:0.00} median {8:0.00}, segments p10 {9:0.00} median {10:0.00}; worst hairMultiplier x{11:0.00}, curveDensity x{12:0.00}",
                    p.name, ratios.Count, changed, 100.0 * changed / ratios.Count, itemsChanged, items, over,
                    ratios[ratios.Count / 10], ratios[ratios.Count / 2], segRatios[segRatios.Count / 10], segRatios[segRatios.Count / 2], worstHm, worstCd));
            }
        }
        Console.WriteLine(failures == 0 ? "RESULT: PASS" : "RESULT: FAIL (" + failures + ")");
        return failures == 0 ? 0 : 1;
    }
}
