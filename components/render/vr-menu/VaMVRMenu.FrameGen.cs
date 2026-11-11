// Frame generation on and off while VaM runs, through the OFXR tray's pause.
//
// When it arms, the tray makes a manual-reset event Local\OFXRBridgePause-<n>,
// where <n> is the number in the layer's `[ofxr] control_event`
// (Local\OFXRBridgeArmStop-<n>, implicit_layer.cpp). The layer checks the
// event at every xrEndFrame (apply_embedded_control). While it is set, the layer
// passes VaM's frames through without generating any and paces VaM at the
// headset's own rate. This is the tray's "Pause frame generation"; the FPS
// overlay shows its pause sign. Stock OFXR (optical flow) and OFXR-DLSSG both
// have it, so nothing in the layer changes for this.
//
// This plugin sets the event while the tab's 插帧 switch is off
// ([Performance] FrameGeneration), and while VaM's menus are open with
// DLSSMenuNative on (the start screen and scene browser, worldUIActivated, or
// the main menu, MainHUDVisible; the same test as the DLSS bridge, which
// renders those frames at 100% without SR). It clears the event otherwise,
// but only once the menus have stayed closed for MenuResumeSeconds: VRMenu-v4
// resumed in the frame the menu button closed them, and 13 of 19 closes in
// one run (every one with NR before the upscaler) bounced back open within
// 0.06-0.4 s, most likely read by VaM as a second press of the menu button
// while the layer restarted its presenter with the button still held (VaM's
// GetStateDown is bState && bChanged, and OpenComposite keeps bChanged until
// the next frame it ends). VRMenu-v3, with no pause, bounced once in 18
// closes; opening, which pauses, never bounced. Resuming after the button is
// let go leaves nothing to read twice.
// The event is opened for each look and closed again, so the tray can make a
// fresh one when it arms again. A pause or resume from the tray's own menu or
// key, seen while no menu holds the pause, becomes the switch's new value.
using System;
using System.Runtime.InteropServices;
using UnityEngine;

public sealed partial class VaMVRMenu
{
    private const string ArmPrefix = "Local\\OFXRBridgeArmStop-", PausePrefix = "Local\\OFXRBridgePause-";
    private const string FrameGenId = "framegen", MenuNativeId = "menu_native";
    private const uint EventModifyState = 0x0002, Synchronize = 0x00100000, WaitObject0 = 0;
    private const float PauseCheckSeconds = 0.5f, PauseRetrySeconds = 2f, MenuResumeSeconds = 0.6f;

    private string pauseName;          // null: no pause control in this run
    private bool pauseKnown;           // pauseSet is what this plugin last left the event at
    private bool pauseSet, pauseMissingLogged;
    private float nextPauseCheck, menusSeenAt = float.NegativeInfinity;
    // performance.ini as VaM started, until the settings are loaded.
    private int frameGenStart = 1, menuNativeStart = 1;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenEventW(uint access, bool inherit, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetEvent(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ResetEvent(IntPtr handle);

    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    // The pause event's name from the layer's ofxr_bridge.ini, once the layer
    // is found; the 插帧 switch is offered only where the event exists.
    private void FindPause(string layerIni)
    {
        try
        {
            string text = new VRMenuIni.Document(System.IO.File.ReadAllBytes(layerIni)).text;
            string arm = (VRMenuIni.Get(text, "ofxr", "control_event") ?? "").Trim();
            string number = arm.StartsWith(ArmPrefix, StringComparison.Ordinal) ? arm.Substring(ArmPrefix.Length) : "";
            bool digits = number.Length > 0 && number.Length <= 20;
            foreach (char c in number) digits = digits && c >= '0' && c <= '9';
            if (!digits)
            {
                Logger.LogInfo("VaM VR menu: the OFXR layer has no tray control (control_event '" + arm + "'): no 插帧 switch");
                return;
            }
            string name = PausePrefix + number;
            IntPtr handle = OpenEventW(EventModifyState | Synchronize, false, name);
            if (handle == IntPtr.Zero)
            {
                Logger.LogInfo("VaM VR menu: the OFXR tray's pause control " + name + " cannot be opened (error " +
                    Marshal.GetLastWin32Error() + "): no 插帧 switch");
                return;
            }
            CloseHandle(handle);
            pauseName = name;
            Logger.LogInfo("VaM VR menu: frame generation on/off through the OFXR tray's pause control " + name);
        }
        catch (Exception e)
        {
            Logger.LogWarning("VaM VR menu: cannot read " + layerIni + " for the tray's pause control: " + e.Message);
        }
    }

    private static bool MenusOpen()
    {
        SuperController sc = SuperController.singleton;
        return sc != null && (sc.MainHUDVisible || sc.worldUIActivated);
    }

    private bool FrameGenOn
    {
        get { Item i; return byId.TryGetValue(FrameGenId, out i) ? i.value == "1" : frameGenStart != 0; }
    }

    private bool MenuNativeOn
    {
        get { Item i; return byId.TryGetValue(MenuNativeId, out i) ? i.value == "1" : menuNativeStart != 0; }
    }

    // Every frame: a change of what the event should be is applied at once
    // (resuming after the menus waits MenuResumeSeconds); otherwise the event
    // is looked at twice a second for the tray's changes.
    private void UpdatePause(float now)
    {
        if (pauseName == null) return;
        if (MenuNativeOn && MenusOpen()) menusSeenAt = now;
        bool hold = now - menusSeenAt < MenuResumeSeconds;
        bool want = !FrameGenOn || hold;
        if (pauseKnown && want == pauseSet && now < nextPauseCheck) return;
        nextPauseCheck = now + PauseCheckSeconds;
        IntPtr handle = OpenEventW(EventModifyState | Synchronize, false, pauseName);
        if (handle == IntPtr.Zero)
        {
            // The tray disarmed, or is arming again: look again later.
            if (!pauseMissingLogged)
                Logger.LogInfo("VaM VR menu: the OFXR tray's pause control is gone (error " + Marshal.GetLastWin32Error() + "); retrying");
            pauseMissingLogged = true;
            pauseKnown = false;
            nextPauseCheck = now + PauseRetrySeconds;
            return;
        }
        try
        {
            pauseMissingLogged = false;
            bool set = WaitForSingleObject(handle, 0) == WaitObject0;
            if (pauseKnown && set != pauseSet && !hold)
            {
                // Paused or resumed from the tray: the switch follows.
                Item item;
                string value = set ? "0" : "1";
                if (byId.TryGetValue(FrameGenId, out item))
                {
                    if (item.value != value)
                    {
                        Change(item, value);
                        SyncMenu();
                    }
                }
                else frameGenStart = set ? 0 : 1;
                Logger.LogInfo("VaM VR menu: frame generation " + (set ? "paused" : "resumed") + " from the OFXR tray; the 插帧 switch follows");
                message = "OFXR 托盘" + (set ? "关闭" : "打开") + "了插帧，已同步到这里。";
                nextStatus = 0f;
                want = set;
            }
            if (set != want && !(want ? SetEvent(handle) : ResetEvent(handle)))
            {
                Logger.LogWarning("VaM VR menu: cannot " + (want ? "set" : "clear") + " the OFXR tray's pause control (error " +
                    Marshal.GetLastWin32Error() + ")");
                pauseKnown = false;
                return;
            }
            if (!pauseKnown || want != pauseSet)
            {
                Logger.LogInfo("VaM VR menu: frame generation " + (want ? "off" : "on") +
                    (want ? (FrameGenOn ? " while VaM's menus are open" : " (插帧 switch)") : ""));
                nextStatus = 0f;
            }
            pauseSet = want;
            pauseKnown = true;
        }
        finally { CloseHandle(handle); }
    }

    // Leaves the layer generating when the plugin goes, unless the switch is off.
    private void OnDestroy()
    {
        if (pauseName == null || !pauseKnown || !pauseSet || !FrameGenOn) return;
        IntPtr handle = OpenEventW(EventModifyState, false, pauseName);
        if (handle == IntPtr.Zero) return;
        ResetEvent(handle);
        CloseHandle(handle);
    }
}
