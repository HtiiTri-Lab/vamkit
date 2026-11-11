using System;
using System.Collections;
using UnityEngine;

// Moves one person the same way every time, so that a benchmark scene has motion without any content of its own:
// the hips sway, bounce and turn, the chest and head follow with a delay, the hands swing. The pose a frame shows
// depends only on the time since the scene was loaded (wall clock), not on the frame rate, so two configurations
// are measured on the same movement. A scene plugin of a Person atom (VaM's own plugin API, nothing else needed).
//
//   Speed   cycles per second of the basic sway (the other movements are fixed multiples of it)
//   Size    scale of every amplitude: 0 stands still, 1 is an energetic dance, 2 is violent
//   Phase   offset in cycles, so that several people do not move in unison
//   Cycles  (read only) how many cycles have gone by: a test reads it to see that the movement runs
public class BenchMotion : MVRScript
{
    private JSONStorableFloat speed, size, phase, cycles;
    private FreeControllerV3 hip, chest, head, leftHand, rightHand;
    private Vector3 hipAt, chestAt, headAt, leftAt, rightAt;
    private Quaternion hipTurn, chestTurn, headTurn;
    private bool ready;
    private float started;

    public override void Init()
    {
        speed = new JSONStorableFloat("Speed", 0.6f, 0f, 3f);
        size = new JSONStorableFloat("Size", 1f, 0f, 2f);
        phase = new JSONStorableFloat("Phase", 0f, 0f, 1f);
        RegisterFloat(speed);
        RegisterFloat(size);
        RegisterFloat(phase);
        cycles = new JSONStorableFloat("Cycles", 0f, 0f, 1e9f);
        cycles.isStorable = false;
        RegisterFloat(cycles);
        CreateSlider(speed);
        CreateSlider(size);
        CreateSlider(phase);
        if (containingAtom == null || containingAtom.type != "Person")
        {
            SuperController.LogError("BenchMotion belongs on a Person atom");
            return;
        }
        hip = Control("hipControl");
        chest = Control("chestControl");
        head = Control("headControl");
        leftHand = Control("lHandControl");
        rightHand = Control("rHandControl");
        StartCoroutine(Begin());
    }

    private FreeControllerV3 Control(string id)
    {
        return containingAtom.GetStorableByID(id) as FreeControllerV3;
    }

    // The scene's own pose is the centre of the movement: taken once the load has put the controls in place.
    private IEnumerator Begin()
    {
        while (SuperController.singleton.isLoading) yield return null;
        for (int i = 0; i < 30; i++) yield return null;
        if (hip == null || head == null) yield break;
        hipAt = hip.transform.position;
        hipTurn = hip.transform.rotation;
        headAt = head.transform.position;
        headTurn = head.transform.rotation;
        if (chest != null) { chestAt = chest.transform.position; chestTurn = chest.transform.rotation; }
        if (leftHand != null) leftAt = leftHand.transform.position;
        if (rightHand != null) rightAt = rightHand.transform.position;
        started = Time.realtimeSinceStartup;
        ready = true;
    }

    private static float Wave(float cycles) { return Mathf.Sin(cycles * 2f * Mathf.PI); }

    public void Update()
    {
        if (!ready || size.val <= 0f) return;
        float c = (Time.realtimeSinceStartup - started) * speed.val + phase.val;   // cycles of the basic sway
        float a = size.val;
        cycles.valNoCallback = c;
        Vector3 sway = new Vector3(0.09f * Wave(c), 0.035f * Wave(2f * c), 0.05f * Wave(c + 0.25f)) * a;
        hip.transform.position = hipAt + sway;
        hip.transform.rotation = Quaternion.Euler(4f * a * Wave(2f * c), 18f * a * Wave(c), 7f * a * Wave(c + 0.5f)) * hipTurn;
        // the upper body answers a little later and a little less
        Vector3 upper = new Vector3(0.06f * Wave(c - 0.08f), 0.03f * Wave(2f * c - 0.1f), 0.03f * Wave(c + 0.17f)) * a;
        if (chest != null && chest.currentPositionState == FreeControllerV3.PositionState.On)
        {
            chest.transform.position = chestAt + upper;
            chest.transform.rotation = Quaternion.Euler(0f, -10f * a * Wave(c - 0.1f), 0f) * chestTurn;
        }
        head.transform.position = headAt + new Vector3(0.07f * Wave(c - 0.15f), 0.03f * Wave(2f * c - 0.2f), 0.03f * Wave(c + 0.1f)) * a;
        head.transform.rotation = Quaternion.Euler(6f * a * Wave(2f * c), 22f * a * Wave(0.5f * c), 5f * a * Wave(c)) * headTurn;
        if (leftHand != null && leftHand.currentPositionState == FreeControllerV3.PositionState.On)
            leftHand.transform.position = leftAt + new Vector3(0.12f * Wave(c), 0.18f * Wave(c + 0.25f), 0.14f * Wave(2f * c)) * a;
        if (rightHand != null && rightHand.currentPositionState == FreeControllerV3.PositionState.On)
            rightHand.transform.position = rightAt + new Vector3(0.12f * Wave(c + 0.5f), 0.18f * Wave(c + 0.75f), 0.14f * Wave(2f * c + 0.5f)) * a;
    }
}
