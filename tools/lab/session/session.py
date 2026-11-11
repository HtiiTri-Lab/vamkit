"""One VaM session from an installed VaM folder (`vamkit install`), in the background, driven through the test agent.

The session is started by the installed launcher in its test mode (`launch.py --test`): VaM minimized and kept from
taking the focus, the saved presets untouched, the desktop window settings put back afterwards. The test agent
(../test-agent) is compiled here, copied into the VaM folder's BepInEx/plugins for the session and removed when it
ends. One VaM at a time: starting is refused while any VaM.exe runs.

    from session import Session
    with Session(vam, display="Desktop") as s:           # the mod, on the desktop
        s.wait_ready()
        print(s.status()["fps"])
        print(s.invoke("VaMSkinTransmission", "Status"))  # any static or singleton member, by reflection
        s.set("VaMSkinTransmission", "rtRenderer.Value", "PathTracing")
    # leaving the block quits VaM, waits for the launcher to put VaM's own files back and removes the agent

A vanilla session (variant="vanilla") has no agent, because BepInEx is off there: it can only be waited for and closed.
Python standard library only. Nothing here is for a real headset: VR sessions need the simulated headset (../simxr).
"""
import ctypes
import datetime
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "simxr"))
from simxr import Client, SimXRError  # noqa: E402  (the agent speaks the simulated headset's line protocol)

AGENT_SOURCE = HERE.parent / "test-agent" / "VaMTestAgent.cs"
AGENT_REFERENCES = ("BepInEx/core/BepInEx.dll", "VaM_Data/Managed/Assembly-CSharp.dll", "VaM_Data/Managed/UnityEngine.dll",
                    "VaM_Data/Managed/UnityEngine.CoreModule.dll", "VaM_Data/Managed/UnityEngine.UI.dll",
                    "VaM_Data/Managed/UnityEngine.UIModule.dll", "VaM_Data/Managed/UnityEngine.VRModule.dll",
                    "VaM_Data/Managed/UnityEngine.AudioModule.dll")


class SessionError(RuntimeError):
    pass


def build_agent(vam):
    """out/lab/VaMTestAgent.dll, compiled with the C# 3.5 compiler against this VaM (again when the source is newer)."""
    out = REPO / "out" / "lab" / "VaMTestAgent.dll"
    if out.is_file() and out.stat().st_mtime >= AGENT_SOURCE.stat().st_mtime:
        return out
    csc = os.environ.get("VAMKIT_CSC") or str(Path(os.environ["WINDIR"]) / "Microsoft.NET" / "Framework64" / "v3.5" / "csc.exe")
    out.parent.mkdir(parents=True, exist_ok=True)
    done = subprocess.run([csc, "/nologo", "/target:library", "/optimize+", "/warn:4", "/codepage:65001", "/out:" + str(out)]
                          + ["/reference:" + str(Path(vam) / r) for r in AGENT_REFERENCES] + [str(AGENT_SOURCE)],
                          capture_output=True, text=True)
    if done.returncode:
        raise SessionError("the test agent did not compile:\n" + done.stdout + done.stderr)
    return out


def processes(image):
    """Process ids whose executable is this file."""
    psapi, kernel = ctypes.WinDLL("psapi"), ctypes.WinDLL("kernel32")
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    ids, got = (wintypes.DWORD * 16384)(), wintypes.DWORD()
    psapi.EnumProcesses(ids, ctypes.sizeof(ids), ctypes.byref(got))
    wanted, found = os.path.normcase(str(Path(image).resolve())), []
    for pid in ids[: got.value // ctypes.sizeof(wintypes.DWORD)]:
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            continue
        try:
            name, size = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            if kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)) and os.path.normcase(name.value) == wanted:
                found.append(pid)
        finally:
            kernel.CloseHandle(handle)
    return found


def any_vam():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq VaM.exe", "/NH"], capture_output=True, text=True,
                         creationflags=subprocess.CREATE_NO_WINDOW).stdout
    return "VaM.exe" in out


def close_windows(pid):
    """Asks every top-level window of the process to close (what the window's X does)."""
    user = ctypes.windll.user32
    user.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user.PostMessageW.argtypes = (wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM)

    def each(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid:
            user.PostMessageW(window, 0x10, 0, 0)   # WM_CLOSE
        return True

    user.EnumWindows(ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(each), 0)


class Session:
    def __init__(self, vam, variant="mod", display="Desktop", overrides=None, name="session"):
        self.vam, self.variant, self.display, self.name = Path(vam), variant, display, name
        self.overrides = dict(overrides or {})
        self.launcher_folder = self.vam / "vamkit" / "launcher"
        self.run = self.launcher = self.agent = self.pid = self.settings = None
        self.plugin = self.vam / "BepInEx" / "plugins" / "VaMTestAgent.dll"
        self.started = None
        self.killed = False   # VaM did not close when asked and was ended

    # ---- start and stop ----------------------------------------------------------------------------------------

    def start(self):
        if not (self.launcher_folder / "launch.py").is_file():
            raise SessionError(f"{self.vam} has no vamkit launcher: python tools/vamkit install first")
        if any_vam():
            raise SessionError("a VaM is running on this machine: one at a time")
        sys.path.insert(0, str(self.launcher_folder))
        import LaunchPanel as panel   # tkinter is imported, no window is opened
        s = panel.load_settings(self.display)
        s.update(self.overrides)
        s["variant"], s["display_mode"] = self.variant, self.display
        s = panel.validate(s)
        multiplier = panel.launch_multiplier(s)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.run = self.launcher_folder / "profiles" / "test-runs" / f"{stamp}-{self.name}"
        self.run.mkdir(parents=True)
        panel.atomic_write(self.run / "performance.ini", panel.performance_ini(s))
        panel.atomic_write(self.run / "tray.ini", panel.flow_ini(s, multiplier))
        panel.atomic_write(self.run / "selection.json", json.dumps(dict(s, frame_multiplier=multiplier, test_run=True), indent=2) + "\n")
        self.settings = dict(s, frame_multiplier=multiplier)
        if self.variant == "mod":
            shutil.copy2(build_agent(self.vam), self.plugin)
        env = dict(os.environ, VAM_TEST_AGENT="1", VAM_TEST_AGENT_LOG=str(self.run))
        python = Path(sys.executable).with_name("python.exe")
        self.started = time.time()
        with open(self.run / "launcher.log", "wb") as output:
            self.launcher = subprocess.Popen([str(python), str(self.launcher_folder / "launch.py"), "--variant", self.variant,
                                              "--display", self.display, "--run", str(self.run), "--test"],
                                             cwd=self.vam, env=env, stdout=output, stderr=subprocess.STDOUT,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            if self.variant == "mod":
                self.agent = Client.wait(str(self.run), timeout=180, process=self.launcher, info_file="vam-agent.json", io_timeout=10)
                self.pid = json.loads((self.run / "vam-agent.json").read_text())["pid"]
                self.call("set", type="UnityEngine.AudioListener", member="volume", value=0)   # silent: someone works at this PC
            else:
                deadline = time.time() + 60
                while not self.pid:
                    found = processes(self.vam / "VaM.exe")
                    self.pid = found[0] if found else None
                    if self.launcher.poll() is not None or time.time() > deadline:
                        raise SessionError("VaM did not start:\n" + self.launcher_text())
                    time.sleep(0.25)
        except Exception:
            self.stop()
            raise
        return self

    def stop(self, wait=60):
        """Quits VaM, waits for the launcher (it puts VaM's own files and the window settings back), removes the agent.
        Returns the launcher's exit code, None when it had to be left running."""
        try:
            if self.agent is not None:
                try:
                    self.call("quit", wait=15)
                except (OSError, SimXRError, SessionError):
                    pass
                self.agent.close()
                self.agent = None
            elif self.pid and processes(self.vam / "VaM.exe"):
                close_windows(self.pid)
            deadline = time.time() + wait
            while self.launcher is not None and self.launcher.poll() is None and time.time() < deadline:
                time.sleep(0.25)
            if self.launcher is not None and self.launcher.poll() is None:
                self.killed = True
                for pid in processes(self.vam / "VaM.exe"):   # VaM did not close: end it; the launcher then cleans up
                    subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
                try:
                    self.launcher.wait(30)
                except subprocess.TimeoutExpired:
                    pass
            return self.launcher.poll() if self.launcher is not None else None
        finally:
            if not processes(self.vam / "VaM.exe"):
                self.plugin.unlink(missing_ok=True)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    # ---- while it runs -----------------------------------------------------------------------------------------

    def call(self, cmd, wait=180, **fields):
        """One request to the agent, sent once. The answer may sit behind a long frame (VaM starting, a scene loading,
        the driver compiling the path tracer's pipelines): it is waited for up to `wait` seconds."""
        request = self.agent.send(cmd, **fields)
        deadline = time.time() + wait
        while True:
            try:
                return self.agent.receive(request, cmd)
            except (TimeoutError, socket.timeout):
                if self.launcher.poll() is not None:
                    raise SessionError(f"VaM is gone (waiting for {cmd})")
                if time.time() > deadline:
                    raise SessionError(f"no answer to {cmd} within {wait} s")

    def status(self):
        return self.call("status")

    def get(self, type_name, member):
        return self.call("get", type=type_name, member=member).get("value")

    def set(self, type_name, member, value):
        return self.call("set", type=type_name, member=member, value=value)

    def invoke(self, type_name, method, *args):
        return self.call("invoke", type=type_name, method=method, args=list(args)).get("result")

    def wait_ready(self, timeout=180, frames=30):
        """Until VaM is not loading and frames go by. Returns the status."""
        deadline = time.time() + timeout
        while True:
            status = self.status()
            if not status.get("loading"):
                self.call("wait_frames", n=frames)
                return self.status()
            if time.time() > deadline:
                raise SessionError("VaM is still loading after %d s" % timeout)
            time.sleep(1)

    def set_ini(self, section, key, value):
        """A live setting of the compositor: this run's performance.ini, which it reads again when the file changes."""
        import ModeProfiles   # from the installed launcher folder (on sys.path since start)
        ModeProfiles.patch_ini(self.run / "performance.ini", {section: {key: value}})

    def launcher_text(self):
        try:
            return (self.run / "launcher.log").read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def log_folder(self):
        """This run's log folder under <VaM>/vamkit/logs (the launcher prints it), None before VaM started or in vanilla."""
        for line in self.launcher_text().splitlines():
            if "Log folder: " in line:
                return Path(line.split("Log folder: ", 1)[1].strip())
        return None
