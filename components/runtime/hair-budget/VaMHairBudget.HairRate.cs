// Hair physics rate: a copy of VaM's physics rate settings (Physics Rate and
// Physics Update Cap) that only physics hair uses.
//
// VaM steps GPUTools hair in FixedUpdate (HairSettings.FixedUpdate ->
// HairPhysicsWorld.FixedDispatch), so hair follows VaM's physics rate and
// update cap like everything else. With a hair rate set, hair gets its own
// fixed-step clock with the same rules as UserPreferences.SyncPhysics:
// step = 1 / rate (Auto as VaM: headset refresh rate, halved above 91 Hz,
// then 45/60/72/80/90; 72 without a headset), and a frame advances at most
// step x cap (never more than 0.05 s), like Time.maximumDeltaTime.
// Hair then steps in its per-frame Dispatch (HairSettings.LateUpdate, after
// the scalp skinning and before the strands are tessellated for drawing),
// and the Time.fixedDeltaTime reads in HairPhysicsWorld.InitData and
// DispatchPhysicsImpl return the hair step instead, so GPUTools' own rules
// (two passes when the step is over 20 ms, dt = step x Weight / passes) are
// unchanged. Style mode and items that already run on Update keep VaM's path.
//
// Following VaM (the default) takes the same steps as the body, in the same
// place: each frame's Dispatch runs the steps the body's physics took in the
// frame before, with that step. That frame's body is what the skin now drawn
// (and the scalp just skinned) shows, and what the colliders hair collides
// with were computed from; in FixedUpdate the hair would step a frame before
// its scalp is skinned to that pose.
// Any failure puts hair back on VaM's FixedUpdate path for the rest of the
// session.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Reflection.Emit;
using GPUTools.Hair.Scripts.Runtime.Data;
using GPUTools.Hair.Scripts.Runtime.Physics;
using HarmonyLib;
using UnityEngine;

public sealed partial class VaMHairBudget
{
    private const float HairRateLogSeconds = 30f, HairMenuSeconds = 5f, AutoRateSeconds = 1f;

    private static readonly List<string> HairRateIds = new List<string>(new string[] {
        "VaM", "Auto", "45", "60", "72", "80", "90", "120", "144", "240", "288" });
    private static readonly List<string> HairRateLabels = new List<string>(new string[] {
        Lang.T("Follow the body's physics"), Lang.T("Auto"), "45 Hz", "60 Hz", "72 Hz", "80 Hz", "90 Hz", "120 Hz", "144 Hz", "240 Hz", "288 Hz" });
    private static readonly List<string> HairCapIds = new List<string>(new string[] { "1", "2", "3" });

    // Static: the Harmony patches are static methods.
    private static float hairStep;                 // seconds per hair step of its own clock; 0 = none
    private static bool followBody;                // hair takes the body's steps, a frame later in Dispatch
    private static int hairCap = 1;
    // The body's fixed steps: the frame they were counted in, how many, and their step; and the frame before.
    private static int bodyFrame = -1, bodySteps, bodyFrameBefore = -1, bodyStepsBefore;
    private static float bodyStep, bodyStepBefore;
    private static int hairClockFrame = -1, hairStepsThisFrame;
    private static float hairAccumulator;
    private static float hairStepRunning;          // > 0 while hair steps: what HairFixedDeltaTime returns
    private static bool hairPatched, hairFailed;
    private static string hairFailure;
    private static Action<HairPhysicsWorld> dispatchPhysics;
    // Hair clock counters: [0] the log window (30 s), [1] the menu window (5 s).
    private static readonly int[] statFrames = new int[2], statSteps = new int[2], statItemSteps = new int[2];
    private static readonly float[] statReal = new float[2], statHair = new float[2];

    private float nextAutoRate, nextHairLog, nextHairMenu;
    private float autoRateHz;
    private string hairRateShown;                  // last logged setting
    private int menuFrames, menuSteps;             // the last finished menu window
    private float menuReal, menuHair;

    // ---- per frame (plugin Update) ----------------------------------------

    private void UpdateHairRate(float now)
    {
        if (hairFailure != null)
        {
            Logger.LogError("VaM hair budget: hair physics back on VaM's physics rate for this session: " + hairFailure);
            hairFailure = null;
            nextStatus = 0f;
        }
        if (now >= nextAutoRate)
        {
            nextAutoRate = now + AutoRateSeconds;
            autoRateHz = AutoRate();
        }
        float hz = paused || hairFailed ? 0f : RateHz(settings.hairRate);
        float step = hz > 0f ? 1f / hz : 0f;
        bool follow = !paused && !hairFailed && settings.hairRate == "VaM";
        if ((step > 0f || follow) && !hairPatched)
        {
            PatchHair();
            if (!hairPatched) { step = 0f; follow = false; }
        }
        if (step != hairStep || follow != followBody || settings.hairCap != hairCap)
        {
            hairStep = step;
            followBody = follow;
            hairCap = settings.hairCap;
            hairAccumulator = 0f;
            ResetHairStats(0);
            ResetHairStats(1);
            menuFrames = 0;
        }
        string shown = HairRateText(hz);
        if (shown != hairRateShown)
        {
            hairRateShown = shown;
            Logger.LogInfo("VaM hair budget: " + shown);
            nextHairLog = now + HairRateLogSeconds;
        }
        if (now >= nextHairMenu)
        {
            nextHairMenu = now + HairMenuSeconds;
            menuFrames = statFrames[1]; menuSteps = statSteps[1]; menuReal = statReal[1]; menuHair = statHair[1];
            ResetHairStats(1);
        }
        if ((hairStep > 0f || followBody) && now >= nextHairLog)
        {
            nextHairLog = now + HairRateLogSeconds;
            if (statFrames[0] > 0)
                Logger.LogInfo("VaM hair budget: hair physics, last " + statReal[0].ToString("0", CultureInfo.InvariantCulture) + " s: " +
                    (statFrames[0] / Mathf.Max(statReal[0], 1e-3f)).ToString("0.0", CultureInfo.InvariantCulture) + " fps, " +
                    ((float)statSteps[0] / statFrames[0]).ToString("0.00", CultureInfo.InvariantCulture) + " hair steps per frame, " +
                    ((float)statItemSteps[0] / statFrames[0]).ToString("0.0", CultureInfo.InvariantCulture) + " item steps per frame, hair time " +
                    Percent(statHair[0], statReal[0]) + " of real time");
            ResetHairStats(0);
        }
    }

    private float RateHz(string id)
    {
        if (id == "VaM") return 0f;
        if (id == "Auto") return autoRateHz;
        int hz;
        return int.TryParse(id, NumberStyles.Integer, CultureInfo.InvariantCulture, out hz) && hz > 0 ? hz : 0f;
    }

    // UserPreferences.SyncPhysics, PhysicsRate.Auto.
    private static float AutoRate()
    {
        if (!UnityEngine.XR.XRDevice.isPresent) return 72f;
        float rate = UnityEngine.XR.XRDevice.refreshRate;
        if (rate > 91f) rate *= 0.5f;
        if (rate == 0f) return 90f;
        if (rate < 59f) return 45f;
        if (rate < 71f) return 60f;
        if (rate < 79f) return 72f;
        if (rate < 89f) return 80f;
        return 90f;
    }

    private string HairRateText(float hz)
    {
        if (hairFailed) return "hair physics follows VaM's physics rate (hair rate turned off after an error)";
        if (paused) return "hair physics follows VaM's physics rate while paused";
        if (followBody) return "hair physics takes the body's physics steps (count and step of the frame before, stepped after the scalp is skinned)";
        if (hz <= 0f) return "hair physics follows VaM's physics rate";
        float step = 1f / hz;
        // (the body's own step is left out: with the frame clock it changes every frame)
        return "hair physics rate " + hz.ToString("0", CultureInfo.InvariantCulture) + " Hz" +
            (settings.hairRate == "Auto" ? " (auto)" : "") + ", update cap " + settings.hairCap +
            ": hair steps " + Ms(step) + ", at most " + Ms(Mathf.Min(step * settings.hairCap, 0.05f)) + " of hair time per frame";
    }

    private static string Ms(float seconds)
    {
        return (seconds * 1000f).ToString("0.0", CultureInfo.InvariantCulture) + " ms";
    }

    private static string Percent(float part, float whole)
    {
        return (100f * part / Mathf.Max(whole, 1e-6f)).ToString("0", CultureInfo.InvariantCulture) + "%";
    }

    private static void ResetHairStats(int window)
    {
        statFrames[window] = statSteps[window] = statItemSteps[window] = 0;
        statReal[window] = statHair[window] = 0f;
    }

    // ---- patches -----------------------------------------------------------

    private void PatchHair()
    {
        if (hairPatched || hairFailed) return;
        try
        {
            Type world = typeof(HairPhysicsWorld);
            BindingFlags any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            MethodInfo physics = world.GetMethod("DispatchPhysicsImpl", any, null, Type.EmptyTypes, null);
            MethodInfo initData = world.GetMethod("InitData", any, null, Type.EmptyTypes, null);
            MethodInfo dispatch = world.GetMethod("Dispatch", any, null, Type.EmptyTypes, null);
            MethodInfo fixedDispatch = world.GetMethod("FixedDispatch", any, null, Type.EmptyTypes, null);
            if (physics == null || initData == null || dispatch == null || fixedDispatch == null)
                throw new Exception("HairPhysicsWorld has no DispatchPhysicsImpl/InitData/Dispatch/FixedDispatch");
            if (world.GetField("data", any) == null || world.GetField("isPhysics", any) == null)
                throw new Exception("HairPhysicsWorld has no data/isPhysics field");
            dispatchPhysics = (Action<HairPhysicsWorld>)Delegate.CreateDelegate(typeof(Action<HairPhysicsWorld>), physics);

            BindingFlags own = BindingFlags.Static | BindingFlags.NonPublic;
            Harmony harmony = new Harmony("local.vam.hairbudget.hairrate");
            HarmonyMethod transpiler = new HarmonyMethod(typeof(VaMHairBudget).GetMethod("FixedDeltaTimeTranspiler", own));
            transpilerHits = 0;
            harmony.Patch(initData, null, null, transpiler, null);
            if (transpilerHits != 1) throw new Exception("InitData: " + transpilerHits + " Time.fixedDeltaTime reads replaced, expected 1");
            transpilerHits = 0;
            harmony.Patch(physics, null, null, transpiler, null);
            if (transpilerHits != 1) throw new Exception("DispatchPhysicsImpl: " + transpilerHits + " Time.fixedDeltaTime reads replaced, expected 1");
            harmony.Patch(fixedDispatch, new HarmonyMethod(typeof(VaMHairBudget).GetMethod("HairFixedDispatchPrefix", own)), null, null, null);
            harmony.Patch(dispatch, new HarmonyMethod(typeof(VaMHairBudget).GetMethod("HairDispatchPrefix", own)), null, null, null);
            hairPatched = true;
            Logger.LogInfo("VaM hair budget: hair physics rate patches in place (HairPhysicsWorld Dispatch/FixedDispatch, fixedDeltaTime in InitData/DispatchPhysicsImpl)");
        }
        catch (Exception e)
        {
            // A half-applied patch set only matters while hairStep > 0, which it
            // never becomes after a failure.
            hairFailed = true;
            Logger.LogError("VaM hair budget: hair physics rate unavailable, hair stays on VaM's physics rate: " + e);
        }
    }

    private static int transpilerHits;

    private static IEnumerable<CodeInstruction> FixedDeltaTimeTranspiler(IEnumerable<CodeInstruction> instructions)
    {
        MethodInfo getter = typeof(Time).GetProperty("fixedDeltaTime").GetGetMethod();
        MethodInfo replacement = typeof(VaMHairBudget).GetMethod("HairFixedDeltaTime", BindingFlags.Static | BindingFlags.Public);
        foreach (CodeInstruction i in instructions)
        {
            if ((i.opcode == OpCodes.Call || i.opcode == OpCodes.Callvirt) && i.operand is MethodInfo && (MethodInfo)i.operand == getter)
            {
                i.opcode = OpCodes.Call;
                i.operand = replacement;
                transpilerHits++;
            }
            yield return i;
        }
    }

    // What HairPhysicsWorld.InitData and DispatchPhysicsImpl read instead of
    // Time.fixedDeltaTime.
    public static float HairFixedDeltaTime()
    {
        return hairStepRunning > 0f ? hairStepRunning : Time.fixedDeltaTime;
    }

    // The body's fixed steps per frame (every physics step, whatever its clock).
    private void FixedUpdate()
    {
        int frame = Time.frameCount;
        if (frame != bodyFrame)
        {
            bodyFrameBefore = bodyFrame; bodyStepsBefore = bodySteps; bodyStepBefore = bodyStep;
            bodyFrame = frame; bodySteps = 0;
        }
        bodySteps++;
        bodyStep = Time.fixedDeltaTime;
    }

    // FixedUpdate: hair with its own rate, or following the body, steps in Dispatch instead.
    private static bool HairFixedDispatchPrefix(HairDataFacade ___data)
    {
        if ((hairStep <= 0f && !followBody) || ___data == null) return true;
        try { return ___data.StyleMode || ___data.RunPhysicsOnUpdate; }
        catch (Exception) { return true; }
    }

    // LateUpdate, per hair item: run this frame's hair steps, then let
    // Dispatch tessellate as usual (it skips physics: RunPhysicsOnUpdate is off).
    private static void HairDispatchPrefix(HairPhysicsWorld __instance, HairDataFacade ___data, ref bool ___isPhysics)
    {
        if ((hairStep <= 0f && !followBody) || ___data == null) return;
        try
        {
            if (___data.StyleMode || ___data.RunPhysicsOnUpdate) return;
            AdvanceHairClock();
            if (hairStepsThisFrame == 0) return;
            __instance.UpdateIsPhysics();
            if (!___isPhysics) return;
            hairStepRunning = hairStepThisFrame;
            try
            {
                for (int i = 0; i < hairStepsThisFrame; i++) dispatchPhysics(__instance);
            }
            finally { hairStepRunning = 0f; }
            statItemSteps[0] += hairStepsThisFrame;
            statItemSteps[1] += hairStepsThisFrame;
        }
        catch (Exception e)
        {
            hairStepRunning = 0f;
            hairStep = 0f;
            followBody = false;
            hairFailed = true;
            hairFailure = e.ToString();
        }
    }

    private static float hairStepThisFrame;

    // One clock for all hair, advanced by the first hair item of each frame:
    // the body's steps of the frame before, or the hair's own fixed-step clock
    // (Unity's fixed loop with the hair's step and cap).
    private static void AdvanceHairClock()
    {
        int frame = Time.frameCount;
        if (frame == hairClockFrame) return;
        hairClockFrame = frame;
        float real = Time.unscaledDeltaTime;
        int n;
        if (followBody)
        {
            // the frame before this one; a frame without steps (paused) has none
            if (bodyFrame == frame - 1) { n = bodySteps; hairStepThisFrame = bodyStep; }
            else if (bodyFrame == frame && bodyFrameBefore == frame - 1) { n = bodyStepsBefore; hairStepThisFrame = bodyStepBefore; }
            else { n = 0; hairStepThisFrame = 0f; }
            n = Mathf.Min(n, 4);   // a hitch of more steps: the hair keeps up without a burst of steps
        }
        else
        {
            float delta = Mathf.Min(real, Mathf.Min(hairStep * hairCap, 0.05f)) * Time.timeScale;
            hairAccumulator += delta;
            n = Mathf.FloorToInt(hairAccumulator / hairStep + 1e-4f);
            n = Mathf.Clamp(n, 0, hairCap);
            hairAccumulator = Mathf.Max(0f, hairAccumulator - n * hairStep);
            hairStepThisFrame = hairStep;
        }
        hairStepsThisFrame = n;
        for (int w = 0; w < 2; w++)
        {
            statFrames[w]++;
            statSteps[w] += n;
            statReal[w] += real;
            statHair[w] += n * hairStepThisFrame;
        }
    }

    // ---- menu status ---------------------------------------------------------

    private string HairRateStatus()
    {
        if (hairFailed) return Lang.T("Hair physics: an error; this run follows VaM's physics rate (see the log)");
        if (paused) return Lang.T("Hair physics: follows VaM's physics rate setting");
        if (followBody)
        {
            string f = Lang.T("Hair physics: follows the body's physics (the same steps and step length, advanced after the scalp is skinned)");
            if (menuFrames > 0)
                f += Lang.F("; last {0} s: {1} steps per frame, hair speed {2}", menuReal.ToString("0", CultureInfo.InvariantCulture),
                    ((float)menuSteps / menuFrames).ToString("0.0", CultureInfo.InvariantCulture), Percent(menuHair, menuReal));
            return f;
        }
        if (hairStep <= 0f) return Lang.T("Hair physics: follows VaM's physics rate setting");
        string s = Lang.F("Hair physics: {0} Hz, update cap {1}", (1f / hairStep).ToString("0", CultureInfo.InvariantCulture), hairCap);
        if (menuFrames > 0)
            s += Lang.F("; last {0} s: {1} steps per frame, hair speed {2}", menuReal.ToString("0", CultureInfo.InvariantCulture),
                ((float)menuSteps / menuFrames).ToString("0.0", CultureInfo.InvariantCulture), Percent(menuHair, menuReal));
        return s;
    }
}
