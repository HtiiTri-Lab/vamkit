"""Runs the standard scenes in an installed VaM folder and measures them.

One VaM session per scene and display (the desktop, or VR on the simulated headset). Inside a session the cases are
switched while the game runs, so that they are measured on the same scene state:

  raster      path tracing off, frame generation off     what the renderer can do with VaM's own lighting
  raster+fg   path tracing off, frame generation on      the same as it is played: real frames held at refresh / multiplier
  path        path tracing on,  frame generation off     what the path tracer can do
  path+fg     path tracing on,  frame generation on      as it is played

Any other scene works too: give it as VaM names it, `Saves/scene/mine.json` or `Creator.Package.1:/Saves/scene/x.json`.
It is loaded as it is and measured the same way (without the check that the standard scenes' motion runs): this is how
to measure the user's real content, which the standard scenes cannot stand for.

A scene named `vam:<level>` is one of VaM's own benchmark scenes (`VaM.exe -benchmark <level>`: BenchmarkBaseline,
BenchmarkGPU, BenchmarkCPU, BenchmarkCPUHighPhysics, BenchmarkHair, BenchmarkHairRender, BenchmarkHairSim,
BenchmarkClothSim, BenchmarkCrypt): VaM starts in it, nothing is loaded, and the averages VaM itself logs when its
benchmark is complete are kept with the record (`vam_benchmark`).

`--baseline` adds a desktop session with VaM's own files and none of the kit's plugins (BepInEx on for the test agent
only): the picture VaM renders by itself, as the reference for frame rates. `--without NAME` adds sessions with one
plugin parked (its files start with NAME, for example VaMHairSolver), cases raster and path, to show what that
component contributes. What is measured and how to read it: docs/benchmarks/README.md.

    python tools/lab/bench/bench.py run --vam <VaM folder> [--scenes portrait,dance,duo,stage] [--displays Desktop,VR]
                                        [--seconds 30] [--refresh 144] [--multiplier 4] [--baseline] [--without NAME ...]
    python tools/lab/bench/bench.py report <results.json> [more.json ...]      a Markdown table of results files

Results: out/lab/bench/<time>/results.json (one record per measured case) and report.md. This starts the game many
times: one VaM at a time, the GPU must be free, nobody should be using the machine for anything heavy meanwhile.
"""
import argparse
import csv
import ctypes
import datetime
import json
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "session"))
from session import Session, SessionError, SimXRError, any_vam, warnings_in  # noqa: E402
from simxr import analyze  # noqa: E402  (session put ../simxr on the path)

CASES = {"raster": (False, False), "raster+fg": (False, True), "path": (True, False), "path+fg": (True, True)}
VAM_SCENE = "vam:"   # a scene of VaM's own benchmark, by its level name
VAM_RESULT = re.compile(r"Benchmark complete\. Avg\. tot time: ([\d.]+) Avg\. physics time: ([\d.]+) Avg\. internal physics time: ([\d.]+) "
                        r"Avg\. scripts time: ([\d.]+) Avg\. render time: ([\d.]+) Avg\. wait time: ([\d.]+) Avg\. FPS: ([\d.]+)")
HITCH_MS = 50.0   # a frame this long is seen as a stall at every frame rate the benchmark runs at
HEAD_SWAY_DEG, HEAD_SWAY_S = 12.0, 5.0   # VR: the head looks left and right (peak 15 degrees a second), never still
HEADSET = ("display_hz", "displayed_fps", "submitted_fps", "repeat_ratio", "hitches", "content_hitches", "longest_image_ms", "dropped_frames",
           "pipeline_gaps", "frozen_pose_runs", "late_frames", "submit_interval_ms", "pose_age_intended_ms", "simulator_late_vsyncs")
STAGED = "Saves/scene/_agent-test/vamkit-bench"


def stage_scenes(vam):
    """The scenes and their plugin, copied under Saves/scene/_agent-test for the run (removed afterwards)."""
    target = vam / STAGED
    target.mkdir(parents=True, exist_ok=True)
    for path in (HERE / "scenes").iterdir():
        if path.suffix == ".json":
            (target / path.name).write_text(path.read_text(encoding="utf-8").replace("Saves/scene/vamkit-bench/", STAGED + "/"), encoding="utf-8")
        else:
            shutil.copy2(path, target / path.name)


def unstage_scenes(vam):
    shutil.rmtree(vam / "Saves" / "scene" / "_agent-test", ignore_errors=True)


def cpu_seconds(pid):
    """Kernel plus user time of a process so far."""
    kernel = ctypes.WinDLL("kernel32")
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        kernel.GetProcessTimes.argtypes = (wintypes.HANDLE,) + (ctypes.POINTER(wintypes.FILETIME),) * 4
        if not kernel.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
            return None
        return sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in times[2:]) / 1e7
    finally:
        kernel.CloseHandle(handle)


class GpuSampler(threading.Thread):
    """The whole GPU's load and memory in use, once a second (nvidia-smi; per-process figures are not available)."""

    def __init__(self):
        super().__init__(daemon=True)
        self.samples, self.stop = [], threading.Event()

    def run(self):
        while not self.stop.wait(1.0):
            sample = gpu_now()
            if sample:
                self.samples.append(sample)

    def result(self):
        self.stop.set()
        self.join(5)
        if not self.samples:
            return {}
        return dict(gpu_load_percent=round(statistics.mean(s[0] for s in self.samples), 1), video_memory_mb=max(s[1] for s in self.samples))


def gpu_now():
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,memory.used", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW).stdout.strip().splitlines()[0]
        load, memory = (float(x) for x in out.split(","))
        return load, memory
    except (OSError, ValueError, IndexError):
        return None


def frame_statistics(path):
    """VaM's own frames in one measured window, from the test agent's frame log."""
    times, collections, steps, menus = [], [], [], 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                if row["loading"] == "1":
                    continue
                menus += row.get("menus_open") == "1"
                times.append(float(row["dt_ms"]))
                collections.append(int(row["gc"]) if row.get("gc") else 0)
                if row.get("physics_steps") not in (None, ""):
                    steps.append(int(row["physics_steps"]))
            except (KeyError, ValueError):
                continue
    times = times[1:]   # the first row's time reaches back before the window
    if len(times) < 10:
        return dict(frames=len(times))
    ordered = sorted(times)

    def at(p):
        return round(ordered[min(len(ordered) - 1, int(p * len(ordered)))], 2)

    median = at(0.5)
    found = dict(frames=len(times), fps=round(1000.0 * len(times) / sum(times), 2), frame_ms_p50=median, frame_ms_p95=at(0.95), frame_ms_p99=at(0.99),
                 frame_ms_max=round(ordered[-1], 2), long_frames=sum(t > 2 * median for t in times), hitches=sum(t > HITCH_MS for t in times),
                 collections=collections[-1] - collections[0], frames_with_menu_open=menus)
    if steps:
        found["physics_steps_per_frame"] = round(sum(steps[1:]) / max(len(steps) - 1, 1), 3)
    return found


TIMING = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*?VAM GPU timing (.+?) eye=\d+: (\d+) samples .*?avg ms: (.*?) = ([\d.]+)")


def stage_times(log_text, start, end):
    """The compositor's GPU time per stage, from the lines it logs every ten seconds, for windows that lie inside
    [start, end] (a line stamped T covers the ten seconds before T)."""
    found = {}
    for line in log_text.splitlines():
        m = TIMING.match(line)
        if not m:
            continue
        stamp = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
        if not (start + 9.0 <= stamp <= end + 1.5):
            continue
        name, samples, total = m.group(2).strip(), int(m.group(3)), float(m.group(5))
        parts = dict(zip(m.group(4).split()[0::2], (float(x) for x in m.group(4).split()[1::2])))
        entry = found.setdefault(name, dict(samples=0, total_ms=0.0, parts={}))
        entry["total_ms"] += total * samples
        for key, value in parts.items():
            entry["parts"][key] = entry["parts"].get(key, 0.0) + value * samples
        entry["samples"] += samples
    for entry in found.values():
        n = max(entry["samples"], 1)
        entry["total_ms"] = round(entry["total_ms"] / n, 3)
        entry["parts"] = {k: round(v / n, 3) for k, v in entry["parts"].items()}
    return found


def measure(session, name, seconds, vr):
    """One window of `seconds` on the state the session is in. Returns the record's measured part."""
    session.call("hud", close_all=True)
    time.sleep(1.0)
    frames = session.run / f"frames-{name.replace('+', '-')}.csv"
    session.call("frame_log", on=True, path=str(frames))
    if vr:
        session.sim.motion(target="head", type="sine", axis="yaw", amplitude=HEAD_SWAY_DEG, period_s=HEAD_SWAY_S, duration_s=seconds + 2 * HEAD_SWAY_S)
        time.sleep(HEAD_SWAY_S)   # one sway before the window, so that it starts on a moving head
        session.sim.mark(name + ":start")
    sampler = GpuSampler()
    cpu0, t0 = cpu_seconds(session.pid), time.time()
    sampler.start()
    time.sleep(seconds)
    cpu1, t1 = cpu_seconds(session.pid), time.time()
    record = dict(window=[t0, t1], seconds=round(t1 - t0, 1))
    record.update(sampler.result())
    if vr:
        session.sim.mark(name + ":end")
        session.sim.motion(target="head", type="still")
    session.call("frame_log", on=False)
    record.update(frame_statistics(frames))
    if cpu0 is not None and cpu1 is not None:
        record["cpu_cores"] = round((cpu1 - cpu0) / (t1 - t0), 2)
    status = session.status()
    record["menus_open_at_end"] = bool(status.get("menus_open"))
    try:
        record["physics_hz"] = round(1.0 / float(session.get("UnityEngine.Time", "fixedDeltaTime")), 1)
    except (SessionError, SimXRError, TypeError, ValueError, ZeroDivisionError):
        pass
    return record


def skin_state(session):
    try:
        skin = json.loads(session.invoke("VaMSkinTransmission", "Status"))
    except (SessionError, SimXRError, ValueError, TypeError):
        return {}
    rt = skin.get("native", {}).get("rt", {})
    path, core = rt.get("path_tracing", {}), rt.get("core", {})
    return dict(can_trace=rt.get("available") == 1, tracing=path.get("active") == 1, traced_triangles=core.get("triangles"),
                hair_segments=path.get("hair_segments"), scene_build_gpu_ms=core.get("gpu", {}).get("build_ms"), scene_record_cpu_ms=rt.get("cpu_ms"),
                reconstruction=skin.get("native", {}).get("reconstruction", {}).get("active") == 1, skins=len(skin.get("skins", [])),
                error=skin.get("native_error") or rt.get("last_error") or "")


PREFERENCES = ("msaaLevel", "pixelLightCount", "shaderLOD", "physicsRate", "physicsUpdateCap", "physicsHighQuality", "smoothPasses", "softPhysics",
               "desktopVsync", "mirrorReflections", "realtimeReflectionProbes", "glowEffects")


def vam_preferences(session):
    """VaM's own quality preferences (User Preferences in its menu): the frame rate depends on them as much as on the
    scene, so every record carries them. Read, never changed."""
    found = {}
    for name in PREFERENCES:
        try:
            found[name] = session.get("UserPreferences", "singleton." + name)
        except (SessionError, SimXRError):
            pass
    return found


def plugin_warnings(vam):
    """Warning and error lines of BepInEx's log of the VaM that is running: a plugin that stepped aside or failed says
    so there in the first seconds. A measurement of a kit that is not whole is a measurement of something else."""
    try:
        return warnings_in((Path(vam) / "BepInEx" / "LogOutput.log").read_text(encoding="utf-8", errors="replace"))[:40]
    except OSError:
        return []


def motion_cycles(session):
    """Progress of the first person's motion plugin: a scene that stands still is not the scene that was asked for."""
    try:
        person = next(a["uid"] for a in session.atoms() if a["type"] == "Person")
        return session.storable(person, "plugin#0_BenchMotion", get="Cycles").get("value")
    except (SessionError, SimXRError, StopIteration):
        return None


def inside(vam, path):
    """A path under the VaM folder as text relative to it (results are compared and published: no machine paths)."""
    if not path:
        return None
    try:
        return Path(path).resolve().relative_to(vam).as_posix()
    except ValueError:
        return Path(path).name


def machine():
    """What the numbers were measured on, as far as it can be read without asking."""
    found = dict(processors=__import__("os").cpu_count())
    try:
        line = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"],
                              capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW).stdout.strip().splitlines()[0]
        found["gpu"], found["driver"], found["video_memory"] = (x.strip() for x in line.split(","))
    except (OSError, ValueError, IndexError):
        pass
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            found["cpu"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    except OSError:
        pass
    ctypes.windll.user32.SetProcessDPIAware()
    found["desktop"] = [ctypes.windll.user32.GetSystemMetrics(0), ctypes.windll.user32.GetSystemMetrics(1)]
    return found


def run_session(vam, scene, display, variant, cases, seconds, settings, sim_env, without, out):
    """One VaM session. Returns its records (one per case), also when it fails halfway."""
    vr = display == "VR"
    own = scene.startswith(VAM_SCENE)   # one of VaM's own benchmark scenes
    standard = (HERE / "scenes" / f"bench-{scene}.json").is_file()   # one of ours; anything else is a scene of the user's
    name = scene if standard or own else Path(scene.replace(":", "/")).stem
    label = f"{re.sub('[^A-Za-z0-9._-]+', '-', name)}-{display}-{variant}" + ("-without-" + "-".join(without) if without else "")
    print(f"-- {label}", flush=True)
    base = dict(scene=scene, display=display, variant=variant, without=without, time=datetime.datetime.now().isoformat(timespec="seconds"))
    records, before = [], gpu_now()
    session = Session(vam, variant=variant, display=display, overrides=settings, name="bench-" + label, sim_env=sim_env if vr else None, without=without,
                      game_args=["-benchmark", scene[len(VAM_SCENE):]] if own else None)
    started = time.time()
    try:
        session.start()
        session.wait_ready()
        base["start_seconds"] = round(time.time() - started, 1)
        if own:   # VaM started in the scene
            session.call("hud", close_all=True)
            time.sleep(8)
        else:
            loading = time.time()
            session.call("load_scene", path=f"{STAGED}/bench-{scene}.json" if standard else scene)
            time.sleep(2)
            session.wait_ready(timeout=1800)   # a scene with real content can take minutes
            base["load_seconds"] = round(time.time() - loading, 1)
            session.call("hud", close_all=True)
            time.sleep(8)
            atoms = {a["uid"] for a in session.atoms()}
            if standard:
                expected = json.loads((HERE / "scenes" / f"bench-{scene}.json").read_text(encoding="utf-8"))["atoms"]
                base["atoms_missing"] = sorted(a["id"] for a in expected if a["id"] not in atoms)
            else:
                base["atoms"] = len(atoms)
        if variant == "mod":
            base["settings"] = {k: session.settings.get(k) for k in ("dlss_preset", "render_scale_k", "render_scale_rr", "output_scale", "frame_multiplier", "nr_enabled")}
        base["vam_preferences"] = vam_preferences(session)
        base["plugin_warnings"] = plugin_warnings(vam)
        if base["plugin_warnings"]:   # said first: a long run of a kit that is not whole is wasted
            print(f"   !! {len(base['plugin_warnings'])} warning(s) in BepInEx's log; the kit may not be whole:", flush=True)
            for line in base["plugin_warnings"][:6]:
                print("      " + line[:200], flush=True)
        base["video_memory_mb_before_vam"] = before[1] if before else None
        tracing = False
        for name in cases:
            record = dict(base, case=name)
            if variant == "mod":
                want_path, want_generation = CASES[name]
                if want_path != tracing:
                    session.set("VaMSkinTransmission", "rtRenderer.Value", "PathTracing" if want_path else "Legacy")
                    session.call("wait_frames", wait=300, n=90)
                    tracing = want_path
                session.set_ini("Performance", "FrameGeneration", "1" if want_generation else "0")
                time.sleep(6)
            cycles = motion_cycles(session) if standard else None
            record.update(measure(session, name, seconds, vr))
            if standard:
                after = motion_cycles(session)
                record["motion_cycles"] = round(after - cycles, 2) if cycles is not None and after is not None else None
            if variant == "mod":
                record["renderer"] = skin_state(session)
                record["running"] = session.running()   # what runs against what was asked, in the kit's own words
            if vr:
                try:
                    capture = session.sim.capture(frames=1, eyes="left", scale=0.5, name=f"{scene}-{name.replace('+', '-')}", timeout=60)
                    record["capture"] = {k: capture[k] for k in capture if k not in ("id", "ok")}
                except (SimXRError, OSError) as e:
                    record["capture"] = str(e)
            records.append(record)
            print(f"   {name:<10} {record.get('fps')} fps  p95 {record.get('frame_ms_p95')} ms  cpu {record.get('cpu_cores')}  gpu {record.get('gpu_load_percent')}%  "
                  f"{record.get('video_memory_mb')} MB" + ("  MENU OPEN" if record.get("frames_with_menu_open") else ""), flush=True)
    except (SessionError, SimXRError, OSError) as e:
        records.append(dict(base, case="failed", error=str(e)))
        print(f"   FAILED: {e}", flush=True)
    finally:
        code = session.stop()
    folder = session.log_folder()
    if own:   # what VaM's benchmark said about itself, from Unity's log lines in BepInEx's log
        said = None
        for path in ([folder / "bepinex.log"] if folder else []) + [vam / "BepInEx" / "LogOutput.log"]:
            if path.is_file():
                found = VAM_RESULT.findall(path.read_text(encoding="utf-8", errors="replace"))
                if found:
                    keys = ("total_ms", "physics_ms", "internal_physics_ms", "scripts_ms", "render_ms", "wait_ms", "fps")
                    said = dict(zip(keys, (float(x) for x in found[-1])))
                    break
        for record in records:
            record["vam_benchmark"] = said
    # Which frame generator made frames in this session: a stand-in for the one asked for is not the case asked for.
    generation = folder / "dlssg" / "dlssg.log" if folder else None
    made = generation.read_text(encoding="utf-8", errors="replace") if generation and generation.is_file() else ""
    generators = [name for name, mark in (("DLSS", r"DLSS-G\[\d+\] first evaluation recorded"), ("FSR", r"FSR-FG\[\d+\] first pair recorded")) if re.search(mark, made)]
    for record in records:
        if "fg" in record.get("case", ""):
            record["generated_by"] = generators
    log = folder / "opencomposite.log" if folder else None
    text = log.read_text(encoding="utf-8", errors="replace") if log and log.is_file() else ""
    for record in records:
        record["launcher_exit"], record["logs"], record["run"] = code, inside(vam, folder), inside(vam, session.run)
        if isinstance(record.get("capture"), dict) and record["capture"].get("dir"):
            record["capture"]["dir"] = inside(vam, record["capture"]["dir"])
        if "window" in record and text:
            record["gpu_stages"] = stage_times(text, *record["window"])
        if vr and "window" in record and folder:
            try:   # the simulated display over the same window: what a headset would have shown
                shown = analyze(str(folder / "simxr"), from_mark=record["case"] + ":start", to_mark=record["case"] + ":end")
                record["headset"] = {k: shown.get(k) for k in HEADSET}
            except (SimXRError, OSError, ValueError, KeyError) as e:
                record["headset"] = dict(error=str(e))
    (out / f"{label}.json").write_text(json.dumps(records, indent=1), encoding="utf-8")
    return records


def with_retry(*arguments):
    """One VaM session, started once more when it failed: VaM has a rare hang at start that is not what is measured
    (docs/knowledge/rejected-and-open.md). Both attempts stay in the results."""
    records = run_session(*arguments)
    if records and records[-1].get("case") == "failed":
        print("   once more", flush=True)
        time.sleep(10)
        waited = 0
        while any_vam() and waited < 1800:   # someone else's VaM (one at a time on a machine): wait for it to end
            time.sleep(15)
            waited += 15
        again = run_session(*arguments)
        for r in again:
            r["attempt"] = 2
        records += again
    return records


def run(args):
    vam = Path(args.vam).resolve()
    out = REPO / "out" / "lab" / "bench" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    scenes = args.scenes.split(",")
    settings = dict(frame_multiplier=args.multiplier, framegen=1)
    sim_env = {"SIMXR_REFRESH": args.refresh, "SIMXR_HEAD_HEIGHT_CM": 160}   # eyes where VaM's desktop camera is
    results = []
    (out / "machine.json").write_text(json.dumps(dict(machine(), refresh=args.refresh, multiplier=args.multiplier, seconds=args.seconds), indent=1), encoding="utf-8")
    if any((HERE / "scenes" / f"bench-{s}.json").is_file() for s in scenes):
        stage_scenes(vam)
    try:
        for scene in scenes:
            for display in args.displays.split(","):
                results += with_retry(vam, scene, display, "mod", args.cases.split(","), args.seconds, settings, sim_env, [], out)
                (out / "results.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
            if args.baseline:
                results += with_retry(vam, scene, "Desktop", "baseline", ["vam"], args.seconds, settings, sim_env, [], out)
            for name in args.without or []:
                results += with_retry(vam, scene, "Desktop", "mod", ["raster", "path"], args.seconds, settings, sim_env, [name], out)
            (out / "results.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
    finally:
        unstage_scenes(vam)
    (out / "report.md").write_text(report([out / "results.json"]), encoding="utf-8")
    print(f"results: {out}")
    return 0 if all(r.get("case") != "failed" for r in results) else 1


def void(r):
    """Why a record cannot be used, or nothing."""
    reasons = []
    if r.get("case") == "failed":
        reasons.append("failed")
    if r.get("frames_with_menu_open"):
        reasons.append("a menu was open")
    if r.get("atoms_missing"):
        reasons.append("atoms missing")
    if "motion_cycles" in r and r.get("motion_cycles") in (None, 0):
        reasons.append("no motion")
    if r.get("case", "").startswith("path") and not (r.get("renderer") or {}).get("tracing"):
        reasons.append("path tracing did not take over")
    return ", ".join(reasons)


def degraded(r):
    """Not void, but to be read with care: the plugins warned about something in this session."""
    notes = []
    if r.get("plugin_warnings"):
        notes.append(f"{len(r['plugin_warnings'])} plugin warning(s), see the record")
    state = r.get("running") or {}
    told = False
    for part in ((state.get("compositor") or {}).get("upscaler"), (state.get("generation") or {}).get("frame_generation") if "fg" in r.get("case", "") else None):
        if part and part.get("wanted") and part.get("wanted") != part.get("running"):
            notes.append(f"asked for {part['wanted']}, {part.get('running')} ran ({part.get('reason') or 'no reason given'})")
            told = True
    made = r.get("generated_by")
    if not told and made is not None and "DLSS" not in made and r.get("display") == "Desktop":
        notes.append("frames generated by FSR (2x), not by DLSS" if made else "no frame generator logged a frame")
    return "; ".join(notes)


def summary(records):
    """Real frames per second, one row per scene: the kit's records only (no parked plugins) and VaM alone."""
    columns = [("Desktop", "raster"), ("Desktop", "path"), ("Desktop", "raster+fg"), ("Desktop", "path+fg"),
               ("VR", "raster"), ("VR", "path"), ("VR", "raster+fg"), ("VR", "path+fg")]
    found = {}
    for r in records:
        if r.get("without"):
            continue
        key = (r["scene"], "alone") if r["variant"] == "baseline" else (r["scene"], r["display"], r.get("case"))
        found[key] = "void" if void(r) else r.get("fps", "")
    scenes = list(dict.fromkeys(r["scene"] for r in records))
    lines = ["| scene | VaM alone, desktop | " + " | ".join(f"{d.lower()} {c}" for d, c in columns) + " |", "|---|" + "---|" * (len(columns) + 1)]
    for scene in scenes:
        lines.append(f"| {scene} | {found.get((scene, 'alone'), '')} | " + " | ".join(str(found.get((scene, d, c), "")) for d, c in columns) + " |")
    return lines


def report(paths):
    records = [r for p in paths for r in json.loads(Path(p).read_text(encoding="utf-8"))]
    lines = ["Real frames per second (frames VaM rendered; with frame generation on they are held at refresh / multiplier):", ""]
    lines += summary(records)
    lines += ["", "Every record:", "",
              "| scene | display | setup | case | real fps | frame ms p50 / p95 / p99 | long frames | CPU cores | GPU load | video memory | notes |",
              "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in records:
        setup = "VaM alone" if r["variant"] == "baseline" else ("without " + ", ".join(r["without"]) if r.get("without") else "kit")
        if r.get("case") == "failed":
            lines.append(f"| {r['scene']} | {r['display']} | {setup} | failed | | | | | | | {r.get('error', '')[:80]} |")
            continue
        notes = ["VOID: " + void(r)] if void(r) else []
        if degraded(r):
            notes.append(degraded(r))
        renderer = r.get("renderer") or {}
        if renderer.get("error"):
            notes.append(renderer["error"][:60])
        shown = r.get("headset") or {}
        if shown.get("displayed_fps"):
            notes.append(f"headset: {shown['displayed_fps']} new frames/s at {shown.get('display_hz')} Hz, {round(100 * (shown.get('repeat_ratio') or 0), 1)} % repeats, "
                         f"{shown.get('hitches')} hitches")
        elif shown.get("error"):
            notes.append("headset log: " + shown["error"][:60])
        if r.get("hitches"):
            notes.append(f"{r['hitches']} frames over {int(HITCH_MS)} ms")
        lines.append(f"| {r['scene']} | {r['display']} | {setup} | {r['case']} | {r.get('fps', '')} | {r.get('frame_ms_p50', '')} / {r.get('frame_ms_p95', '')} / {r.get('frame_ms_p99', '')} | "
                     f"{r.get('long_frames', '')} | {r.get('cpu_cores', '')} | {r.get('gpu_load_percent', '')} % | {r.get('video_memory_mb', '')} MB | {'; '.join(notes)} |")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)
    r = sub.add_parser("run")
    r.add_argument("--vam", required=True)
    r.add_argument("--scenes", default="portrait,dance,duo,stage")
    r.add_argument("--displays", default="Desktop,VR")
    r.add_argument("--cases", default="raster,raster+fg,path,path+fg")
    r.add_argument("--seconds", type=float, default=30)
    r.add_argument("--refresh", type=int, default=144, help="the simulated headset's refresh rate")
    r.add_argument("--multiplier", type=int, default=4,
                   help="frames shown per real frame with frame generation on. Not checked here: the launcher and the generator decide what "
                        "they can do, and the record carries the multiplier that ran")
    r.add_argument("--baseline", action="store_true")
    r.add_argument("--without", action="append", metavar="NAME")
    p = sub.add_parser("report")
    p.add_argument("results", nargs="+")
    args = ap.parse_args()
    if args.command == "report":
        print(report(args.results))
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
