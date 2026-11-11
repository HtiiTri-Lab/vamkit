// VaM WindowCamera in VR (BepInEx 5, Unity 2018.1, C# 3).
//
// A WindowCamera atom with "Camera On" switches on its CameraGroup (CameraControl.SyncCameraOnAndUseAudioListener):
//   MainWindowCamera  (cameraToControl) no target texture, no eye: renders the whole scene again, with every shadow
//                     map, to the desktop window (3840 x 2160 here);
//   MiniCamera        (cameraToControl2) renders into the small texture on the atom's own display planes and HUD
//                     view (in the world, so a headset can see it).
// While VR is on this plugin keeps both Camera components off: nobody looks at the desktop window, and the user
// does not need the display planes (11-02: "MiniCamera可以直接关闭"; they keep the last picture). The AudioListener
// on MainWindowCamera, the toggle and what scenes save stay as they are. A camera set as VaM's main camera ("Use As
// Main Camera") keeps rendering, since Camera.main would change with it.
// 1.3.0: our desktop mode (VAM_DISPLAY_MODE=Desktop) too. There the bridge's "VaM Desktop Output" camera (depth
// 2.001) writes the finished picture over the whole window after MainWindowCamera (depth 1), so its full 4K scene
// pass (and in existing lighting its own shadow maps) was never seen: 1.05-1.4 ms of GPU a frame in a heavy scene. VaM's
// own Desktop Mode.bat (no VAM_DISPLAY_MODE) is not touched.
// Measured on SimXR (a heavy two-person scene, 2026-10-15): MainWindowCamera 0.52 ms a frame, MiniCamera 0.30 ms; 2026-11-02 its
// region was 1.6 ms (existing lighting) / 5.3 ms (path tracing) at RR F, since the first camera of a frame also
// carries work that is not its own.
// Hooks: Harmony postfixes on CameraControl.SyncCameraOnAndUseAudioListener (from Init, and on every Camera On or
// audio listener change) and SyncUseAsMainCamera; nothing searches the scene.
using System;
using System.Collections.Generic;
using System.Reflection;
using BepInEx;
using UnityEngine;
using UnityEngine.XR;

[BepInPlugin("local.vam.windowcameravr", "VaM WindowCamera VR", "1.3.0")]
public sealed class VaMWindowCameraVR : BaseUnityPlugin
{
    private static readonly bool DesktopPipeline = Environment.GetEnvironmentVariable("VAM_DISPLAY_MODE") == "Desktop";
    private static BepInEx.Logging.ManualLogSource log;
    private static readonly HashSet<Camera> turnedOff = new HashSet<Camera>();

    private void Awake()
    {
        log = Logger;
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        try
        {
            HarmonyLib.Harmony harmony = new HarmonyLib.Harmony("local.vam.windowcameravr");
            HarmonyLib.HarmonyMethod after = new HarmonyLib.HarmonyMethod(typeof(VaMWindowCameraVR).GetMethod("AfterSync"));
            foreach (string name in new string[] { "SyncCameraOnAndUseAudioListener", "SyncUseAsMainCamera" })
            {
                MethodInfo method = typeof(CameraControl).GetMethod(name, flags);
                if (method == null) throw new MissingMethodException("CameraControl", name);
                harmony.Patch(method, null, after, null, null);
            }
            log.LogInfo("VaM WindowCamera VR: MiniCamera stays off; MainWindowCamera stays off in VR" + (DesktopPipeline ? " and in this desktop mode (the pipeline's output covers it)" : ""));
        }
        catch (Exception e)
        {
            log.LogError("VaM WindowCamera VR: cannot hook CameraControl (" + e.Message + "); doing nothing");
        }
    }

    public static void AfterSync(CameraControl __instance)
    {
        try
        {
            Apply(__instance);
        }
        catch (Exception e)
        {
            log.LogWarning("VaM WindowCamera VR: " + e.Message);
        }
    }

    private static void Apply(CameraControl control)
    {
        string atom = control.containingAtom != null ? control.containingAtom.uid : control.name;
        Camera camera = control.cameraToControl;
        if (camera != null)
        {
            bool desktopOnly = camera.targetTexture == null && camera.stereoTargetEye == StereoTargetEyeMask.None;
            Keep(camera, desktopOnly && (XRSettings.enabled || DesktopPipeline) && !control.useAsMainCamera, atom,
                XRSettings.enabled ? "desktop window only" : "covered by the desktop pipeline's output",
                control.useAsMainCamera ? " (main camera)" : "");
        }
        Camera mini = control.cameraToControl2;
        if (mini != null && mini != camera)
            Keep(mini, mini.stereoTargetEye == StereoTargetEyeMask.None, atom, "display planes", "");
    }

    private static void Keep(Camera camera, bool off, string atom, string why, string onNote)
    {
        if (off)
        {
            if (!camera.enabled) return;
            turnedOff.RemoveWhere(c => c == null);
            camera.enabled = false;
            turnedOff.Add(camera);
            log.LogInfo("VaM WindowCamera VR: " + atom + " " + camera.name + " off (" + why + ")");
        }
        else if (turnedOff.Remove(camera))
        {
            camera.enabled = true;
            log.LogInfo("VaM WindowCamera VR: " + atom + " " + camera.name + " on again" + onNote);
        }
    }
}
