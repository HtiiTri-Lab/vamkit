"""Starts VaM with the visual mod or untouched, and puts VaM's own files back when it exits.

mod      OpenComposite as openvr_api.dll, the D3D11 proxy beside VaM.exe, OFXR armed (VR only), this run's settings.
vanilla  VaM's own files, and BepInEx switched off for this process, so nothing of ours is loaded.

Layout, as `vamkit install` leaves it:
  <VaM>/vamkit/launcher    this file, the panel, the presets
  <VaM>/vamkit/bin         openvr_api.dll, d3d11.dll, ofxr/, dlss/{sr,nr,fg}
  <VaM>/vamkit/original    what the install found in VaM's place

  python launch.py --variant mod --display VR --run <folder with performance.ini, tray.ini, selection.json> [--test]
  python launch.py --restore
"""
from __future__ import annotations

import argparse
import ctypes
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time
import winreg
from ctypes import wintypes

sys.path.insert(0, str(Path(__file__).resolve().parent))   # ModeProfiles lies beside this file, also under `python -I`
import ModeProfiles as profiles

ROOT = Path(__file__).resolve().parent
KIT = ROOT.parent
VAM = KIT.parent
BIN = KIT / "bin"
ORIGINAL = KIT / "original"
LOGS = KIT / "logs"
PLUGINS = VAM / "VaM_Data" / "Plugins"
OPENVR = PLUGINS / "openvr_api.dll"
OC_INI = PLUGINS / "opencomposite.ini"
PROXY = VAM / "d3d11.dll"
OFXR_HOME = Path(os.environ.get("LOCALAPPDATA", "")) / "OFXR Bridge"
TRAY = BIN / "ofxr" / "OFXRBridgeTray.exe"
LAYERS_KEY = r"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
SCREEN_KEY = r"Software\MeshedVR\VaM"            # Unity's player settings of VaM, shared by every copy on this PC
SCREEN_FILE = KIT / "screen-before-test.json"   # the window settings a test run found there
# Everything of ours a parent environment could leak into a vanilla VaM or an earlier mod run.
OUR_VARIABLES = ("VAM_DISPLAY_MODE", "VAM_PERFORMANCE_CONFIG", "VAM_DESKTOP_CONFIG", "VAM_NR_PREPASS_PATH", "VAM_NR_PREPASS_LOG",
                 "VAM_DLSS_RUNTIME_PATH", "VAM_DLSS_LOG", "VAM_DLSS_BRIDGE", "D3DINTEROP_MODE", "D3DINTEROP_LOG", "OFXR_DLSSG",
                 "OFXR_DLSSG_RUNTIME_PATH", "OFXR_DLSSG_LOG", "OFXR_DLSSG_LITE", "OFXR_DLSSG_CAPTURE", "OFXR_FRAME_MULTIPLIER")
OC_MARK = "; vamkit"   # tells the mod's opencomposite.ini from the one that was found
OC_SETTINGS = "\r\n" + OC_MARK + "\r\ninitUsingVulkan=false\r\ninvertUsingShaders=true\r\nsupersampleRatio=1.0\r\n"


def vam_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq VaM.exe", "/NH"], capture_output=True, text=True,
                         creationflags=subprocess.CREATE_NO_WINDOW).stdout
    return "VaM.exe" in out


def ours(name, target):
    """True when the file in VaM's place is the one the mod puts there (the mod ran last and was not cleaned up)."""
    if not target.is_file():
        return False
    if name == "opencomposite.ini":
        return OC_MARK in target.read_text(encoding="utf-8", errors="replace")
    mine = BIN / name
    return mine.is_file() and mine.stat().st_size == target.stat().st_size and mine.read_bytes() == target.read_bytes()


def put_back(name, target):
    """VaM's own copy from the record: the file that was found there, or `<name>.absent` when there was none.
    Only a file of the mod's is replaced: VaM's own (also one a VaM update put there since) stays as it is, and so does
    an empty place. Without a record the file is left alone too."""
    if not ours(name, target):
        return
    source = ORIGINAL / name
    if source.is_file():
        shutil.copy2(source, target)
    elif (ORIGINAL / (name + ".absent")).is_file():
        target.unlink(missing_ok=True)


def remember(name, target):
    """Before the mod takes this file's place: what is there now becomes the record (VaM may have been updated, an
    injector added or removed since the install). A file of the mod's left by a killed run is not VaM's: no change."""
    if ours(name, target):
        return
    kept, marker = ORIGINAL / name, ORIGINAL / (name + ".absent")
    ORIGINAL.mkdir(parents=True, exist_ok=True)
    if target.is_file():
        if not kept.is_file() or kept.read_bytes() != target.read_bytes():
            shutil.copy2(target, kept)
        marker.unlink(missing_ok=True)
    else:
        kept.unlink(missing_ok=True)
        marker.touch()


def restore_files():
    """VaM's own openvr_api.dll, d3d11.dll and opencomposite.ini, whatever a crash left there."""
    put_back("openvr_api.dll", OPENVR)
    put_back("d3d11.dll", PROXY)
    put_back("opencomposite.ini", OC_INI)


def activate_mod():
    remember("openvr_api.dll", OPENVR)
    remember("d3d11.dll", PROXY)
    remember("opencomposite.ini", OC_INI)
    shutil.copy2(BIN / "openvr_api.dll", OPENVR)
    shutil.copy2(BIN / "d3d11.dll", PROXY)
    source = ORIGINAL / "opencomposite.ini"
    text = source.read_text(encoding="utf-8", errors="replace") if source.is_file() else ""
    OC_INI.write_bytes((text + OC_SETTINGS).encode("utf-8"))


def registrations():
    found = []
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, LAYERS_KEY) as key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                name, value, _ = winreg.EnumValue(key, i)
                if name.lower().startswith(str(OFXR_HOME).lower() + "\\") and value == 0:
                    found.append(name)
    except OSError:
        pass
    return found


def arm_ofxr(run):
    """OFXR's tray registers the OpenXR layer while it runs. Returns the tray's process."""
    if subprocess.run(["tasklist", "/FI", "IMAGENAME eq OFXRBridgeTray.exe", "/NH"], capture_output=True, text=True,
                      creationflags=subprocess.CREATE_NO_WINDOW).stdout.find("OFXRBridgeTray") >= 0:
        raise SystemExit("OFXR's tray is already running. Close it first.")
    OFXR_HOME.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(run) / "tray.ini", OFXR_HOME / "tray.ini")
    tray = subprocess.Popen([str(TRAY)], cwd=TRAY.parent)
    deadline = time.monotonic() + 10
    while not registrations() and time.monotonic() < deadline and tray.poll() is None:
        time.sleep(0.1)
    if not registrations():
        raise SystemExit(f"OFXR did not arm. See {OFXR_HOME / 'tray-lifecycle.log'}")
    return tray


def disarm_ofxr(tray):
    if tray is None:
        return
    window = ctypes.windll.user32.FindWindowW("OFXRBridgeTrayWindow", "OFXR Bridge")
    if window:
        ctypes.windll.user32.PostMessageW(window, 0x10, 0, 0)  # WM_CLOSE
    try:
        tray.wait(5)
    except subprocess.TimeoutExpired:
        print("OFXR's tray did not exit; close its tray icon.")
    deadline = time.monotonic() + 5
    while registrations() and time.monotonic() < deadline:
        time.sleep(0.1)
    if registrations():
        print("An OFXR registration remains; close OFXR's tray icon.")


def mod_environment(mode, run, multiplier, log):
    env = {k: v for k, v in os.environ.items() if k not in OUR_VARIABLES}
    desktop = mode == "Desktop"
    if desktop:
        env.pop("XR_RUNTIME_JSON", None)
    env.update(VAM_DISPLAY_MODE=mode, VAM_PERFORMANCE_CONFIG=str(Path(run) / "performance.ini"),
               VAM_NR_PREPASS_PATH=str(BIN / "dlss" / "nr"), VAM_NR_PREPASS_LOG=str(log / "ngx-prepass"),
               VAM_DLSS_RUNTIME_PATH=str(BIN / "dlss" / "sr"), VAM_DLSS_LOG=str(log / "dlss"), VAM_DLSS_BRIDGE="1",
               D3DINTEROP_MODE="on12", OFXR_DLSSG="1", OFXR_DLSSG_RUNTIME_PATH=str(BIN / "dlss" / "fg"), OFXR_DLSSG_LOG=str(log / "dlssg"))
    if multiplier == 4:
        env["OFXR_FRAME_MULTIPLIER"] = "4"
    if desktop:
        env["VAM_DESKTOP_CONFIG"] = str(Path(run) / "desktop-presenter.ini")
    for folder in ("ngx-prepass", "dlss", "dlssg"):
        (log / folder).mkdir(parents=True, exist_ok=True)
    return env


def keep_logs(log, started):
    for source, name in ((VAM / "BepInEx" / "LogOutput.log", "bepinex.log"), (VAM / "openovr_log", "openovr_log"),
                         (VAM / "d3d11_proxy.log", "d3d11_proxy.log"),
                         (Path(os.environ.get("LOCALAPPDATA", "")) / "OpenComposite" / "logs" / "opencomposite.log", "opencomposite.log"),
                         (Path.home() / "AppData" / "LocalLow" / "MeshedVR" / "VaM" / "output_log.txt", "vam-output_log.txt")):
        try:
            if source.stat().st_mtime >= started:
                shutil.copy2(source, log / name)
        except OSError:
            pass


def screen_settings():
    """Unity keeps the window's mode and size in the registry, one set for every copy of VaM on this PC."""
    values = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SCREEN_KEY) as key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                name, value, kind = winreg.EnumValue(key, i)
                if name.startswith("Screenmanager "):
                    values[name] = [value, kind]
    except OSError:
        pass
    return values


def restore_screen():
    """The window settings as they were before a test run made VaM a window (also after a test that was killed)."""
    saved = profiles.read_json(SCREEN_FILE)
    if saved is None:
        return
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SCREEN_KEY, 0, winreg.KEY_SET_VALUE) as key:
            for name, (value, kind) in saved.items():
                winreg.SetValueEx(key, name, 0, kind, value)
    except OSError:
        pass
    SCREEN_FILE.unlink(missing_ok=True)


def test_window():
    """A test run's window: not full screen (a minimized full-screen window stops rendering), the monitor's size."""
    profiles.write_json(SCREEN_FILE, screen_settings())
    ctypes.windll.user32.SetProcessDPIAware()
    width, height = ctypes.windll.user32.GetSystemMetrics(0), ctypes.windll.user32.GetSystemMetrics(1)
    return ["-screen-fullscreen", "0", "-screen-width", str(width), "-screen-height", str(height)]


def start_game(arguments, env, test):
    """VaM. In a test run it starts minimized without the focus and is kept that way: someone is working at this PC."""
    if not test:
        return subprocess.Popen([str(VAM / "VaM.exe"), *arguments], cwd=VAM, env=env)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 7   # SW_SHOWMINNOACTIVE
    game = subprocess.Popen([str(VAM / "VaM.exe"), *arguments], cwd=VAM, env=env, startupinfo=startup)
    threading.Thread(target=keep_in_background, args=(game,), daemon=True).start()
    return game


def keep_in_background(game):
    """Unity activates its window once it is made, also a minimized one. Whenever VaM is in front: a window that is
    shown is minimized (which hands the focus to the window below); a minimized one that holds the focus all the same
    gives it back to the window that had it before. A window of VaM shown without the focus is minimized too."""
    user, kernel = ctypes.windll.user32, ctypes.windll.kernel32
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    user.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
    user.GetWindowLongW.argtypes = (wintypes.HWND, ctypes.c_int)
    user.AttachThreadInput.argtypes = (wintypes.DWORD, wintypes.DWORD, wintypes.BOOL)
    for name in ("IsWindowVisible", "IsIconic", "IsWindow", "GetWindowTextLengthW", "SetForegroundWindow"):
        getattr(user, name).argtypes = (wintypes.HWND,)
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def owner(window):
        pid = wintypes.DWORD()
        thread = user.GetWindowThreadProcessId(window, ctypes.byref(pid))
        return pid.value, thread

    def usable(window):   # a window a person could be working in
        return (window and user.IsWindow(window) and owner(window)[0] != game.pid and user.IsWindowVisible(window)
                and not user.IsIconic(window) and user.GetWindowTextLengthW(window) > 0
                and not user.GetWindowLongW(window, -20) & 0x80)   # not a tool window

    def below():   # the first such window in Z order
        found = []

        def each(window, _):
            if usable(window):
                found.append(window)
                return False
            return True

        user.EnumWindows(callback(each), 0)
        return found[0] if found else None

    def shown(window, _):
        if owner(window)[0] == game.pid and user.IsWindowVisible(window) and not user.IsIconic(window):
            user.ShowWindow(window, 7)   # SW_SHOWMINNOACTIVE
        return True

    shown = callback(shown)
    last, handed = user.GetForegroundWindow(), 0
    while game.poll() is None:
        front = user.GetForegroundWindow()
        if front:
            pid, thread = owner(front)
            if pid != game.pid:
                last = front
            elif not user.IsIconic(front):
                user.ShowWindow(front, 6)   # SW_MINIMIZE
            else:
                target = last if usable(last) else below()
                if target:
                    me = kernel.GetCurrentThreadId()
                    user.AttachThreadInput(me, thread, True)   # lets this thread move the focus VaM's thread holds
                    user.SetForegroundWindow(target)
                    user.AttachThreadInput(me, thread, False)
                    handed += 1
                    if handed <= 3:
                        print("VaM held the focus while minimized; handed back to the window that had it.", flush=True)
        user.EnumWindows(shown, 0)
        time.sleep(0.25)


def run_mod(mode, run, test):
    desktop = mode == "Desktop"
    if not desktop and ctypes.windll.shell32.IsUserAnAdmin():
        raise SystemExit("Start this normally, not as administrator: OpenXR ignores the per-user OFXR layer in elevated games.")
    selection = profiles.read_json(Path(run) / "selection.json", {})
    multiplier = int(selection.get("frame_multiplier", 2))
    log = LOGS / (datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + mode)
    log.mkdir(parents=True)
    transaction = log / "preset-transaction.json"
    env = mod_environment(mode, run, multiplier, log)
    tray = None
    launched = False
    try:
        profiles.begin(mode, run, transaction, test)
        if not desktop:
            tray = arm_ofxr(run)
            profiles.apply_layer(mode, run)  # the tray rewrites the layer's file when it arms
        activate_mod()
        started = time.time()
        arguments = ["-vrmode", "None" if desktop else "OpenVR"] + (test_window() if test and desktop else [])
        game = start_game(arguments, env, test)
        launched = True
        print(f"VaM started ({mode}, mod{', test: in the background' if test else ''}). Log folder: {log}", flush=True)
        code = game.wait()
        print(f"VaM exited with code {code}.", flush=True)
        keep_logs(log, started)
        return code or 0
    finally:
        restore_files()
        restore_screen()
        disarm_ofxr(tray)
        if transaction.exists():
            profiles.finish(transaction, launched)


def run_vanilla(mode, test):
    restore_files()
    env = {k: v for k, v in os.environ.items() if k not in OUR_VARIABLES}
    env["DOORSTOP_DISABLE"] = "TRUE"  # read by every UnityDoorstop version; the arguments differ between them
    desktop = mode == "Desktop"
    try:
        arguments = (["--doorstop-enable", "false", "--doorstop-enabled", "false", "-vrmode", "None" if desktop else "OpenVR"]
                     + (test_window() if test and desktop else []))
        game = start_game(arguments, env, test)
        print(f"VaM started ({mode}, vanilla: BepInEx off{', test: in the background' if test else ''}).", flush=True)
        code = game.wait()
        print(f"VaM exited with code {code}.", flush=True)
        return code or 0
    finally:
        restore_screen()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=("mod", "vanilla"), default="mod")
    ap.add_argument("--display", choices=profiles.MODES, default="VR")
    ap.add_argument("--run", type=Path)
    ap.add_argument("--test", action="store_true",
                    help="a test run: the saved presets stay as they were, VaM starts minimized and is kept from taking the focus, "
                         "on the desktop as a window (the window settings are put back afterwards)")
    ap.add_argument("--restore", action="store_true", help="only put VaM's own files back")
    args = ap.parse_args()
    if vam_running():
        raise SystemExit("VaM is already running. Close it first.")
    restore_screen()   # a test run that was killed
    if args.restore:
        restore_files()
        print("VaM's own files are back.")
        return 0
    if args.variant == "vanilla":
        return run_vanilla(args.display, args.test)
    if args.run is None:
        raise SystemExit("--run is required with the mod.")
    return run_mod(args.display, args.run, args.test)


if __name__ == "__main__":
    sys.exit(main())
