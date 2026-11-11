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
A baseline session (variant="baseline") is what measurements compare with: VaM's own files and none of the kit's plugins,
but BepInEx on with the test agent alone, so that a script can load a scene and read frame times. For the run the
kit's plugins are parked in BepInEx/plugins.vamkit-parked and put back afterwards (also by the next start, if a run
was killed). `without=["VaMHairSolver", ...]` parks single plugins of a mod session the same way, to measure what one
component contributes.

VR sessions (display="VR") run on the simulated headset (../simxr-runtime, built to out/lab/simxr): this VaM process
alone is pointed at that OpenXR runtime, nothing on the machine changes. `s.sim` is then the client of the simulated
headset's control port (head and controller motion, captures, statistics of the frames handed to the runtime; see
../simxr/simxr.py). `sim_env` sets its variables, for example {"SIMXR_REFRESH": 144}. A real headset is never used.
Python standard library only.
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
SIMULATED_RUNTIME = REPO / "out" / "lab" / "simxr" / "simxr-openxr.json"   # built by ../simxr-runtime
AGENT_REFERENCES = ("BepInEx/core/BepInEx.dll", "VaM_Data/Managed/Assembly-CSharp.dll", "VaM_Data/Managed/UnityEngine.dll",
                    "VaM_Data/Managed/UnityEngine.CoreModule.dll", "VaM_Data/Managed/UnityEngine.UI.dll",
                    "VaM_Data/Managed/UnityEngine.UIModule.dll", "VaM_Data/Managed/UnityEngine.VRModule.dll",
                    "VaM_Data/Managed/UnityEngine.AudioModule.dll")


# A warning BepInEx's own Harmony prints in VaM, about nothing of ours.
IGNORED_WARNINGS = ("AccessTools.Property: Could not find property for type UnityEngine.Application and name isBatchMode",)


def warnings_in(log_text):
    """Warning and error lines of a BepInEx log. A plugin that stepped aside or failed says so there in the first
    seconds: such a run is not the whole kit, whatever else works."""
    found = []
    for line in log_text.splitlines():
        if line.startswith(("[Warning", "[Error", "[Fatal")) and not any(known in line for known in IGNORED_WARNINGS):
            found.append(line[:300])
    return found


def running_in(folder):
    """What ran against what was asked, as the kit itself says it in a run's log folder:
    {"compositor": dlss/running.json, "generation": dlssg/running.json}. The compositor's file has `upscaler` and
    `path_tracing` ({wanted, running, reason}; path tracing also `can`), `can_run` and `why_not` per upscaler; the frame
    generation's has `frame_generation` ({wanted, running, reason, frames_most}), `can_run`, `why_not`. A part whose
    file is not there yet is left out. A stand-in shows as wanted != running, with the reason."""
    found = {}
    for key, name in (("compositor", "dlss"), ("generation", "dlssg")):
        try:
            found[key] = json.loads((Path(folder) / name / "running.json").read_text(encoding="utf-8"))
        except (OSError, ValueError, TypeError):
            pass
    return found


class SessionError(RuntimeError):
    pass


def build_agent(vam):
    """out/lab/VaMTestAgent.dll, compiled with the C# 3.5 compiler against this VaM (again when the source is newer
    or the last build was for another VaM folder: its assemblies and BepInEx may be other versions)."""
    out = REPO / "out" / "lab" / "VaMTestAgent.dll"
    built_for = out.with_suffix(".for.txt")
    where = str(Path(vam).resolve())
    if (out.is_file() and out.stat().st_mtime >= AGENT_SOURCE.stat().st_mtime
            and built_for.is_file() and built_for.read_text(encoding="utf-8").strip() == where):
        return out
    csc = os.environ.get("VAMKIT_CSC") or str(Path(os.environ["WINDIR"]) / "Microsoft.NET" / "Framework64" / "v3.5" / "csc.exe")
    out.parent.mkdir(parents=True, exist_ok=True)
    done = subprocess.run([csc, "/nologo", "/target:library", "/optimize+", "/warn:4", "/codepage:65001", "/out:" + str(out)]
                          + ["/reference:" + str(Path(vam) / r) for r in AGENT_REFERENCES] + [str(AGENT_SOURCE)],
                          capture_output=True, text=True)
    if done.returncode:
        raise SessionError("the test agent did not compile:\n" + done.stdout + done.stderr)
    built_for.write_text(where + "\n", encoding="utf-8")
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


PARKED = "BepInEx/plugins.vamkit-parked"


def park(vam, stems):
    """Moves plugin files out of BepInEx/plugins for one run: all of them (stems None), or the ones whose names start
    with one of `stems` (a plugin and its native library). The test agent stays. What was moved is listed in the parked
    folder, so that unpark() puts back exactly that."""
    plugins, parked = Path(vam) / "BepInEx" / "plugins", Path(vam) / PARKED
    parked.mkdir(parents=True, exist_ok=True)
    moved = []
    for path in sorted(plugins.iterdir()):
        if path.is_file() and path.name != "VaMTestAgent.dll" and (stems is None or any(path.name.startswith(s) for s in stems)):
            shutil.move(str(path), str(parked / path.name))
            moved.append(path.name)
    (parked / "parked.json").write_text(json.dumps(moved), encoding="utf-8")
    return moved


def unpark(vam):
    plugins, parked = Path(vam) / "BepInEx" / "plugins", Path(vam) / PARKED
    if not parked.is_dir():
        return
    for path in sorted(parked.iterdir()):
        if path.name != "parked.json" and not (plugins / path.name).exists():
            shutil.move(str(path), str(plugins / path.name))
    shutil.rmtree(parked, ignore_errors=True)


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
    def __init__(self, vam, variant="mod", display="Desktop", overrides=None, name="session", runtime=None, sim_env=None, without=None,
                 game_args=None):
        self.vam, self.variant, self.display, self.name = Path(vam), variant, display, name
        self.overrides = dict(overrides or {})
        self.without = list(without or [])
        self.game_args = list(game_args or [])   # more arguments for VaM.exe, for example ["-benchmark", "BenchmarkCrypt"]
        self.runtime = Path(runtime) if runtime else SIMULATED_RUNTIME
        self.sim_env = {k: str(v) for k, v in (sim_env or {}).items()}
        self.sim = None
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
        unpark(self.vam)   # a run that was killed
        vr = self.display == "VR"
        if vr and self.variant != "mod":
            raise SessionError("VaM's own VR path is OpenVR: without the kit's OpenComposite it needs SteamVR and a headset (or SteamVR's "
                               "null driver), which this tool does not set up. The simulated headset is an OpenXR runtime and is reached "
                               "through the kit only")
        if vr and not self.runtime.is_file():
            raise SessionError(f"no simulated headset at {self.runtime}: build tools/lab/simxr-runtime first")
        sys.path.insert(0, str(self.launcher_folder))
        import LaunchPanel as panel   # tkinter is imported, no window is opened
        s = panel.load_settings(self.display)
        s.update(self.overrides)
        s["variant"], s["display_mode"] = ("mod" if self.variant == "mod" else "vanilla"), self.display
        s = panel.validate(s)
        multiplier = panel.launch_multiplier(s)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.run = self.launcher_folder / "profiles" / "test-runs" / f"{stamp}-{self.name}"
        self.run.mkdir(parents=True)
        panel.atomic_write(self.run / "performance.ini", panel.performance_ini(s))
        panel.atomic_write(self.run / "tray.ini", panel.flow_ini(s, multiplier))
        panel.atomic_write(self.run / "selection.json", json.dumps(dict(s, frame_multiplier=multiplier, test_run=True), indent=2) + "\n")
        self.settings = dict(s, frame_multiplier=multiplier)
        agent = self.variant in ("mod", "baseline")
        if self.variant == "baseline":
            park(self.vam, None)
        elif self.without:
            park(self.vam, self.without)
        if agent:
            shutil.copy2(build_agent(self.vam), self.plugin)
        env = dict(os.environ, VAM_TEST_AGENT="1", VAM_TEST_AGENT_LOG=str(self.run), **self.sim_env)
        python = Path(sys.executable).with_name("python.exe")
        self.started = time.time()
        with open(self.run / "launcher.log", "wb") as output:
            self.launcher = subprocess.Popen([str(python), str(self.launcher_folder / "launch.py"), "--variant", s["variant"],
                                              "--display", self.display, "--run", str(self.run), "--test"]
                                             + (["--xr-runtime", str(self.runtime)] if vr else [])
                                             + (["--keep-bepinex"] if self.variant == "baseline" else [])
                                             + (["--game-args=" + subprocess.list2cmdline(self.game_args)] if self.game_args else []),
                                             cwd=self.vam, env=env, stdout=output, stderr=subprocess.STDOUT,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            if agent:
                self.agent = Client.wait(str(self.run), timeout=180, process=self.launcher, info_file="vam-agent.json", io_timeout=10)
                self.pid = json.loads((self.run / "vam-agent.json").read_text())["pid"]
                self.call("set", type="UnityEngine.AudioListener", member="volume", value=0)   # silent: someone works at this PC
                if vr:   # up once VaM's VR session has made its swapchains
                    self.sim = Client.wait(str(self.log_folder() / "simxr"), timeout=240, process=self.launcher)
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
                if self.sim is not None:
                    self.sim.close()
                    self.sim = None
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
                unpark(self.vam)

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

    def atoms(self):
        """The scene's atoms: [{uid, type, on}]."""
        return self.call("atoms")["atoms"]

    def storable(self, atom, storable=None, **fields):
        """A scene parameter as VaM's plugins see it. storable=None: the atom's storable ids. Otherwise, by keyword:
        nothing = the parameter names by kind; get=name; set=name, value=v; action=name."""
        if storable is not None:
            fields["storable"] = storable
        return self.call("storable", atom=atom, **fields)

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
        for attempt in range(20):   # the compositor may have the file open for reading at this moment
            try:
                ModeProfiles.patch_ini(self.run / "performance.ini", {section: {key: value}})
                return
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.1)

    def running(self):
        """running_in() of this session's log folder: what runs now against what was asked."""
        folder = self.log_folder()
        return running_in(folder) if folder else {}

    def launcher_text(self):
        if self.run is None:   # start was refused before a run folder was made
            return ""
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
