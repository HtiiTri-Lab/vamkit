"""One VaM session on SimXR, driven from Python (used by vrtest.py's VaM scenarios).

    with VamRun("pacing", overrides={"frame_multiplier": 4}) as run:
        run.wait_ready()
        run.sim.motion(type="sine", axis="yaw", amplitude=40, period_s=2, duration_s=10)
        print(run.agent.call("status"))

The launch is the launch panel's own (LaunchPanel.py: its saved settings and the in-game tab's
values from the newest panel run, with `overrides` on top), written to a new folder under
profiles\\sim-runs (never panel-runs, so the panel does not take it for the user's last launch)
and started through Launch.ps1 -XrRuntime SimXR, which installs the VaM test agent for the run,
restores every file it switched when VaM exits, and gives the OFXR layer's ofxr_bridge.ini its
content and time back. Leaving the block asks VaM to quit and waits for the launcher.

A VaM that stops answering is diagnosed rather than waited out: an error dialog of VaM's (an
OpenComposite abort shows a message box and blocks Unity's render thread, invisible while VaM is
minimized), the runtime's errors (simxr\OpenXR.log), GPU driver resets (nvlddmkm events) and
whether SimXR still receives frames go into VamHung's message and failure.json; then VaM is
closed so that the launcher restores everything.
"""
import ctypes
import ctypes.wintypes as wt
import datetime
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simxr import Client, MainThreadBusy, SimXRError  # noqa: E402

TASK_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
VAM_ROOT = os.path.abspath(os.path.join(TASK_ROOT, "..", ".."))
# Test scenes live in their projects (Extras\<project>\test-scenes), not in the user's scene
# browser: a run copies the one it loads into this folder and removes the folder when VaM has
# exited (and before it starts, after an interrupted run). Scenes a test saves through VaM go
# here too. A normal launch of Launch.ps1 removes a leftover one.
TEST_SCENE_DIR = os.path.join(VAM_ROOT, "Saves", "scene", "_agent-test")


def stage_scene(path):
    """The path VaM should load: a scene file outside Saves\\scene is copied into TEST_SCENE_DIR."""
    full = os.path.abspath(path if os.path.isabs(path) else os.path.join(VAM_ROOT, path))
    scenes = os.path.normcase(os.path.join(VAM_ROOT, "Saves", "scene") + os.sep)
    if not os.path.isfile(full) or os.path.normcase(full).startswith(scenes):
        return path
    os.makedirs(TEST_SCENE_DIR, exist_ok=True)
    dest = os.path.join(TEST_SCENE_DIR, os.path.basename(full))
    shutil.copyfile(full, dest)
    return dest


def remove_test_scenes():
    shutil.rmtree(TEST_SCENE_DIR, ignore_errors=True)


class VamHung(SimXRError):
    """VaM stopped answering, exited or shows an error dialog; the message has the diagnosis."""

    def __init__(self, message, diagnosis=None):
        SimXRError.__init__(self, message)
        self.diagnosis = diagnosis or {}


def process_alive(pid):
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    handle = k32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = wt.DWORD(0)
    k32.GetExitCodeProcess(handle, ctypes.byref(code))
    k32.CloseHandle(handle)
    return code.value == 259  # STILL_ACTIVE


def kill_process(pid):
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    handle = k32.OpenProcess(0x0001 | 0x00100000, False, int(pid))  # PROCESS_TERMINATE | SYNCHRONIZE
    if handle:
        k32.TerminateProcess(handle, 1)
        k32.WaitForSingleObject(handle, 10000)
        k32.CloseHandle(handle)


def dialogs_of(pid):
    """Text of the message boxes / dialogs (#32770 windows) a process shows."""
    user32 = ctypes.WinDLL("user32")
    enum_proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    found = []

    def text(hwnd):
        n = user32.GetWindowTextLengthW(hwnd)
        buffer = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buffer, n + 1)
        return buffer.value

    def class_name(hwnd):
        buffer = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buffer, 256)
        return buffer.value

    def top(hwnd, _):
        owner = wt.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and class_name(hwnd) == "#32770":
            parts = []

            def child(c, _):
                if class_name(c) == "Static" and text(c):
                    parts.append(text(c))
                return True

            user32.EnumChildWindows(hwnd, enum_proc(child), 0)
            found.append({"title": text(hwnd), "text": "\n".join(parts)})
        return True

    user32.EnumWindows(enum_proc(top), 0)
    return found


def gpu_resets(since):
    """NVIDIA driver errors (nvlddmkm, e.g. 153 = engine timeout and reset) and Windows display
    driver resets (Display 4101) in the System log since a time."""
    powershell = os.path.join(os.environ["SystemRoot"], "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
    script = ("Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm','Display'; StartTime=[datetime]'%s'} "
              "-ErrorAction SilentlyContinue | ForEach-Object { '{0} {1} {2}' -f $_.TimeCreated.ToString('HH:mm:ss'), "
              "$_.ProviderName, $_.Id }" % since.strftime("%Y-%m-%d %H:%M:%S"))
    env = {k: v for k, v in os.environ.items() if k.upper() != "PSMODULEPATH"}
    try:
        out = subprocess.run([powershell, "-NoProfile", "-Command", script], capture_output=True, text=True,
                             timeout=30, env=env, creationflags=subprocess.CREATE_NO_WINDOW).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return ["(event log not read: %s)" % e]
    return [line.strip() for line in out.splitlines() if line.strip()]


def runtime_errors(log_dir, limit=8):
    """Failures the runtime logged (simxr\OpenXR.log), each with its Origin/Source lines."""
    path = os.path.join(log_dir, "simxr", "OpenXR.log")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    out = []
    for i, line in enumerate(lines):
        if "fail" in line.lower() and "configuration file" not in line:
            block = [line] + [l.strip() for l in lines[i + 1:i + 3] if l.startswith((" ", "\t"))]
            out.append(" | ".join(block))
    return out[-limit:]


def panel_module():
    sys.path.insert(0, TASK_ROOT)
    import LaunchPanel  # noqa: E402  (tkinter is imported but no window is opened)
    return LaunchPanel


class VamRun:
    def __init__(self, name, overrides=None, max_seconds=600, sim_env=None, stage=None, display_mode='VR'):
        self.name = name
        self.overrides = dict(overrides or {})
        self.max_seconds = max_seconds
        self.sim_env = dict(sim_env or {})
        self.stage = stage
        if display_mode not in ('VR', 'Desktop'):
            raise ValueError('display_mode must be VR or Desktop')
        self.display_mode = display_mode
        self.run_dir = None
        self.log_dir = None
        self.launcher = None
        self.sim = None
        self.agent = None
        self.settings = None
        self.lines = []
        self._reader = None
        self.vam_pid = None
        self.started = None
        self.failure = None
        self._owner_lock = None

    # ------------------------------------------------------------------ setup

    def prepare(self):
        panel = panel_module()
        s = panel.load_settings(self.display_mode)
        s.update(self.overrides)
        s = panel.validate(s)
        multiplier = panel.launch_multiplier(s)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        self.run_dir = os.path.join(TASK_ROOT, "profiles", "sim-runs", "%s-%s" % (stamp, self.name))
        os.makedirs(self.run_dir)
        from pathlib import Path
        run = Path(self.run_dir)
        panel.atomic_write(run / "performance.ini", panel.performance_ini(s))
        panel.atomic_write(run / "tray.ini", panel.flow_ini(s, multiplier))
        panel.atomic_write(run / "selection.json", json.dumps(dict(s, frame_multiplier=multiplier, test_run=True, overrides=self.overrides), indent=2) + "\n")
        self.settings = dict(s, frame_multiplier=multiplier)
        return run

    def start(self):
        # All callers (including scripts without Shoot) must share one runtime.
        # The OS releases this lock if its owning Python process exits.
        import msvcrt
        owner = open(os.path.join(TASK_ROOT, ".simxr-owner.lock"), "a+b")
        if os.fstat(owner.fileno()).st_size == 0:
            owner.write(b"1")
            owner.flush()
        owner.seek(0)
        try:
            msvcrt.locking(owner.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            owner.close()
            raise SimXRError("Another SimXR driver owns VaM; wait for it to finish")
        self._owner_lock = owner
        if os.path.exists(os.path.join(TASK_ROOT, "active-test.json")):
            owner.close()
            self._owner_lock = None
            raise SimXRError("Another test is active (active-test.json); do not replace its VaM")
        remove_test_scenes()
        run = self.prepare()
        self.started = datetime.datetime.now()
        powershell = os.path.join(os.environ["SystemRoot"], "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
        args = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(TASK_ROOT, "Launch.ps1"),
                "-Mode", "OFXR", "-RuntimeVariant", "Bundled", "-PerformanceProfile", str(run / "performance.ini"),
                "-FrameMultiplier", str(self.settings["frame_multiplier"]), "-OfxrProfile", str(run / "tray.ini"),
                "-FrameGenBackend", "DLSSG", "-DisplayMode", self.display_mode, "-TestRun",
                "-DiagnosticSeconds", str(self.max_seconds)]
        if self.display_mode == 'VR':
            args += ["-XrRuntime", "SimXR"]
        if self.stage:
            args += ["-SimXRStage", self.stage]
        if self.display_mode == 'Desktop' or 'lighting_route' in self.overrides:
            args += ["-LightingRoute", self.settings["lighting_route"]]
        env = dict(os.environ)
        # Started from PowerShell 7, the inherited module path makes Windows PowerShell 5.1 load
        # 7's modules (Get-FileHash then fails); without it 5.1 uses its own default.
        for key in [k for k in env if k.upper() == "PSMODULEPATH"]:
            del env[key]
        env.update({k: str(v) for k, v in self.sim_env.items()})
        # From VaM's folder, as the launch panel starts it.
        self.launcher = subprocess.Popen(args, cwd=os.path.abspath(os.path.join(TASK_ROOT, "..", "..")), env=env,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
        log = open(os.path.join(self.run_dir, "launcher.log"), "wb")

        def pump():
            for raw in iter(self.launcher.stdout.readline, b""):
                log.write(raw)
                log.flush()
                line = raw.decode("utf-8", "replace").rstrip()
                self.lines.append(line)
                if line.startswith("LOG DIRECTORY: "):
                    self.log_dir = line[len("LOG DIRECTORY: "):].strip()
            log.close()

        self._reader = threading.Thread(target=pump, daemon=True)
        self._reader.start()
        deadline = time.time() + 120
        while self.log_dir is None:
            if self.launcher.poll() is not None:
                time.sleep(0.5)
                raise SimXRError("Launch.ps1 exited (%s) before starting VaM:\n%s" % (
                    self.launcher.returncode, "\n".join(self.lines[-15:])))
            if time.time() > deadline:
                raise SimXRError("Launch.ps1 printed no log directory within 120 s")
            time.sleep(0.2)
        print("  run %s\n  logs %s" % (self.run_dir, self.log_dir))
        # The agent is up as soon as BepInEx has loaded it; SimXR once VaM's VR session made its swapchains.
        self.agent = Client.wait(self.log_dir, timeout=180, process=self.launcher, info_file="vam-agent.json", io_timeout=10)
        with open(os.path.join(self.log_dir, "vam-agent.json")) as f:
            self.vam_pid = json.load(f).get("pid")
        # Silent while the user works at this PC (AudioListener.volume is not saved anywhere).
        self.call_agent("set", type="UnityEngine.AudioListener", member="volume", value=0)
        if self.display_mode == 'VR':
            self.sim = Client.wait(os.path.join(self.log_dir, "simxr"), timeout=240, process=self.launcher)
        return self

    def call_agent(self, cmd, timeout=600, **fields):
        """An agent request that may wait out a long stall of VaM's main thread (scene loading),
        sent once; while it waits, VaM is checked for an error dialog or an exit (VamHung)."""
        deadline = time.time() + timeout
        request_id = self.agent.send(cmd, **fields)
        while True:
            try:
                return self.agent.receive(request_id, cmd)
            except (TimeoutError, socket.timeout):
                self.check("waiting for VaM to answer %r" % cmd)
                if time.time() > deadline:
                    raise self.hung("VaM has not answered %r for %d s" % (cmd, timeout))
            except MainThreadBusy:
                # The main thread was held over 120 s (a very large scene's load: ~3 min) and the agent
                # dropped the answer. The request is still queued ahead of anything sent now, so
                # once a ping is answered it has run; its own result is lost.
                while True:
                    if time.time() > deadline:
                        raise self.hung("VaM's main thread busy for %d s (%r)" % (timeout, cmd))
                    self.check("waiting for VaM's main thread after %r" % cmd)
                    ping = self.agent.send("ping")
                    try:
                        while True:
                            try:
                                self.agent.receive(ping, "ping")
                                return {"ok": True, "id": request_id, "answer_lost": True}
                            except (TimeoutError, socket.timeout):
                                self.check("waiting for VaM's main thread after %r" % cmd)
                                if time.time() > deadline:
                                    raise self.hung("VaM's main thread busy for %d s (%r)" % (timeout, cmd))
                    except MainThreadBusy:
                        continue

    def diagnose(self):
        """What VaM, the runtime, SimXR and the GPU driver say right now."""
        d = {"vam_pid": self.vam_pid,
             "vam_running": bool(self.vam_pid) and process_alive(self.vam_pid),
             "launcher_running": self.launcher is not None and self.launcher.poll() is None}
        d["dialogs"] = dialogs_of(self.vam_pid) if d["vam_running"] else []
        if self.sim is not None:
            try:
                frames = self.sim.call("status").get("frames", {})
                d["simxr_last_submit_age_ms"] = frames.get("last_submit_age_ms")
                d["simxr_submitted"] = frames.get("submitted")
            except (SimXRError, OSError) as e:
                d["simxr_status"] = "no answer (%s)" % e
        if self.log_dir:
            d["runtime_errors"] = runtime_errors(self.log_dir)
        if self.started:
            d["gpu_driver_events"] = gpu_resets(self.started)
        return d

    def hung(self, message):
        diagnosis = self.diagnose()
        self.failure = dict(diagnosis, message=message)
        lines = [message]
        for dialog in diagnosis.get("dialogs", []):
            lines.append("VaM shows a dialog %r: %s" % (dialog["title"], dialog["text"]))
        if not diagnosis["vam_running"]:
            lines.append("VaM is not running")
        if "simxr_last_submit_age_ms" in diagnosis:
            lines.append("last frame submitted to SimXR %.1f s ago" % ((diagnosis["simxr_last_submit_age_ms"] or 0) / 1000.0))
        lines += ["runtime: %s" % e for e in diagnosis.get("runtime_errors", [])]
        if diagnosis.get("gpu_driver_events"):
            lines.append("GPU driver events since the launch: %s" % ", ".join(diagnosis["gpu_driver_events"]))
        return VamHung("\n  ".join(lines), diagnosis)

    def check(self, doing=""):
        """Raises VamHung if VaM shows an error dialog or is gone."""
        if self.launcher is not None and self.launcher.poll() is not None:
            raise self.hung("the launcher exited (VaM closed) while %s" % doing)
        if self.vam_pid and not process_alive(self.vam_pid):
            raise self.hung("VaM exited while %s" % doing)
        if self.vam_pid and dialogs_of(self.vam_pid):
            raise self.hung("VaM shows an error dialog while %s" % doing)

    def capture(self, timeout=60.0, **fields):
        """A SimXR capture; while it waits, VaM is checked as in call_agent."""
        self.sim.call("capture", **fields)
        deadline = time.time() + timeout
        last_check = time.time()
        while True:
            status = self.sim.call("capture_status")
            if status.get("done"):
                return status
            if time.time() - last_check > 2.0:
                self.check("capturing")
                last_check = time.time()
            if time.time() > deadline:
                raise self.hung("capture not finished after %.0f s: %s" % (timeout, status))
            time.sleep(0.05)

    def __enter__(self):
        try:
            return self.start()
        except BaseException:
            self.stop()
            raise

    def __exit__(self, exc_type, exc, tb):
        if exc is not None and self.failure is None and self.launcher is not None:
            try:
                self.failure = dict(self.diagnose(), message="%s: %s" % (exc_type.__name__, exc))
            except Exception as e:  # the diagnosis must not hide the original error
                self.failure = {"message": "%s: %s" % (exc_type.__name__, exc), "diagnosis_error": str(e)}
        self.stop()

    def stop(self, timeout=180):
        if self.failure is not None and self.log_dir:
            try:
                with open(os.path.join(self.log_dir, "failure.json"), "w", encoding="utf-8") as f:
                    json.dump(self.failure, f, indent=1, ensure_ascii=False)
            except OSError:
                pass
        if self.agent is not None:
            try:
                self.agent.sock.settimeout(5)
                if self.vam_pid and process_alive(self.vam_pid):
                    answer = self.agent.call("ping")
                    if int(answer.get("pid", 0)) == int(self.vam_pid):
                        self.agent.call("quit")
            except (SimXRError, OSError):
                pass
            self.agent.close()
            self.agent = None
        if self.sim is not None:
            self.sim.close()
            self.sim = None
        if self.vam_pid:
            # A VaM that cannot quit (an error dialog, a hung main thread) is closed, so that the
            # launcher restores everything now rather than after -DiagnosticSeconds.
            deadline = time.time() + 30
            while process_alive(self.vam_pid) and time.time() < deadline:
                time.sleep(0.5)
            if process_alive(self.vam_pid):
                print("  VaM did not quit within 30 s; closing it (the launcher then restores everything)")
                kill_process(self.vam_pid)
        if self.launcher is not None:
            try:
                self.launcher.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                print("  Launch.ps1 still running after %d s; it closes VaM after its -DiagnosticSeconds" % timeout)
            if self._reader:
                self._reader.join(timeout=5)
        if not (self.vam_pid and process_alive(self.vam_pid)):
            remove_test_scenes()
        if self._owner_lock is not None:
            self._owner_lock.close()
            self._owner_lock = None

    # ------------------------------------------------------------- helpers

    def wait_ready(self, timeout=300, settle_s=3.0, close_menus=True):
        """Until VaM has finished loading and its frame counter advances; then, with close_menus,
        closes what VaM opens at start (the world UI's scene browser counts as an open menu, and
        with menus open the DLSS bridge renders without SR and frame generation pauses: the user
        closes it in the headset); then settle_s more."""
        deadline = time.time() + timeout
        status = None
        while time.time() < deadline:
            status = self.call_agent("status", timeout=max(1, deadline - time.time()))
            if not status.get("loading") and status.get("fps", 0) > 0:
                if close_menus:
                    self.close_menus()
                time.sleep(settle_s)
                return self.call_agent("status")
            time.sleep(0.5)
        raise SimXRError("VaM still loading after %d s: %s" % (timeout, status))

    def close_menus(self, timeout=20):
        """Closing the start screen's world UI makes VaM open its main HUD a frame later, so
        this closes again until both stay closed for a second."""
        deadline = time.time() + timeout
        while True:
            self.call_agent("hud", close_all=True)
            time.sleep(1.0)
            status = self.call_agent("status")
            if not status.get("menus_open"):
                time.sleep(1.0)
                status = self.call_agent("status")
                if not status.get("menus_open"):
                    return status
            if time.time() > deadline:
                raise SimXRError("VaM's menus did not stay closed: hud %s, world UI %s" % (
                    status.get("hud_visible"), status.get("world_ui")))

    def load_scene(self, path, timeout=600):
        if ":/" in path:
            # A scene inside a .var package ("Creator.Package.1:/Saves/scene/x.json"): no file to
            # check on disk, so straight to SuperController.Load.
            self.call_agent("invoke", type="SuperController", method="Load", args=[path])
        else:
            self.call_agent("load_scene", path=stage_scene(path))
        time.sleep(1.0)
        return self.wait_ready(timeout)

    def time_offset_ms(self):
        """SimXR time (ms since the headset loaded, as in its CSVs) minus agent QPC time (ms):
        a mark and an agent ping back to back, the smaller round trip of three."""
        best = None
        for _ in range(3):
            t0 = time.perf_counter()
            self.sim.mark("_clock")
            agent = self.agent.call("ping")
            t1 = time.perf_counter()
            if best is None or t1 - t0 < best[0]:
                best = (t1 - t0, agent["qpc_s"])
        from simxr import read_markers
        marks = [m for m in read_markers(os.path.join(self.log_dir, "simxr")) if m["label"] == "_clock"]
        return marks[-1]["t_ms"] - best[1] * 1000.0 if marks else None

    def results(self):
        """Status files the DLSS SR bridge and the DLSS-G layer leave (after VaM exits)."""
        out = {}
        for key, rel in (("dlss_sr", ("dlss", "dlss-status.json")), ("dlss_fg", ("dlssg", "dlssg-status.json"))):
            path = os.path.join(self.log_dir, *rel)
            try:
                with open(path, encoding="utf-8-sig") as f:
                    out[key] = json.load(f)
            except (OSError, ValueError):
                out[key] = None
        return out


def read_frame_log(path):
    """vam-frames.csv from the agent: one row per VaM frame (frame, qpc_s, dt_ms, loading, menus_open,
    gc = Mono collections so far, agent 1.2.0+)."""
    import csv
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                rows.append({"frame": int(row["frame"]), "qpc_ms": float(row["qpc_s"]) * 1000.0,
                             "dt_ms": float(row["dt_ms"]), "loading": row["loading"] == "1",
                             "menus_open": row.get("menus_open", row.get("hud")) == "1",
                             "gc": int(row["gc"]) if row.get("gc") not in (None, "") else None})
            except (KeyError, ValueError):
                continue
    return rows
