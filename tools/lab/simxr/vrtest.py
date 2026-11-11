"""VR test scenarios on SimXR, the simulated Quest 3 (no headset needed).

    python vrtest.py selfcheck [--simxr DIR] [--out DIR]
    python vrtest.py smoke|pacing|headturn|menu [--set key=value ...] [--scene PATH] [--seconds N]

selfcheck validates the simulator with simxr_selftest.exe (a tiny D3D11 OpenXR app):
  pacing   an app that keeps up shows a new frame on every 120 Hz refresh, pose error 0;
  motion   a sine head turn: frames shown when predicted carry no pose error;
  capture  captured eye images hold exactly what the app rendered (eye colour, the yaw it
           rendered for, its frame counter, the white square at 100,200);
  slow     an app that sleeps 10 ms per frame free-runs (VDXR submits asynchronously): each
           finished frame is shown at the next refresh, the others repeat, and a repeat during
           a 90 deg/s turn is 0.75 deg stale.

VaM scenarios (each starts VaM once on SimXR through Launch.ps1 -XrRuntime SimXR with the launch
panel's current settings, --set overrides on top; see vamrun.py):
  smoke     VaM reaches VR on SimXR, head tracking arrives in the right direction, idle pacing,
            one capture, DLSS SR / DLSS-G status after exit;
  pacing    still head, then sine head turns: is the pipeline's own output even (no gap of 4+
            refreshes between frames handed to the runtime, no frozen frames while VaM stalls, no
            VaM frame spike); SimXR's display figures (displayed fps, repeats) are reported only;
  headturn  constant-speed yaw turns, consecutive captures: does the content of every submitted
            frame (real or generated) move by its pose difference, and is there a double image;
  menu      the controller menu button opens VaM's main menu once (no bounce), closes it once;
            frame generation pauses while it is open and resumes after.

Each run writes report.json and report.txt (plus the SimXR logs and captures) to a new folder
under Extras\\VRFrameGen\\sim-results unless --out is given. Exit code 0 = every check passed.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simxr import Client, SimXRError, analyze, format_report  # noqa: E402

TASK_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LOADER = os.path.join(TASK_ROOT, "source", "VDXR-DLSSNR", "external", "OpenXR-Loader", "bin", "x64", "openxr_loader.dll")


def find_simxr(explicit=None):
    """The staged SimXR folder with the highest version, else the build output."""
    if explicit:
        return os.path.abspath(explicit)
    staged = []
    for name in os.listdir(TASK_ROOT):
        if name.startswith("SimXR-v") and os.path.exists(os.path.join(TASK_ROOT, name, "simxr-openxr.json")):
            try:
                staged.append((int(name[len("SimXR-v"):]), name))
            except ValueError:
                pass
    if staged:
        return os.path.join(TASK_ROOT, max(staged)[1])
    return os.path.join(TASK_ROOT, "source", "SimXR", "build", "bin")


LAST_OUT = {}


def new_out_dir(scenario, explicit=None):
    if explicit:
        os.makedirs(explicit, exist_ok=True)
        path = os.path.abspath(explicit)
    else:
        root = os.path.join(TASK_ROOT, "sim-results")
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        path = os.path.join(root, "%s-%s" % (stamp, scenario))
        os.makedirs(path)
    LAST_OUT.update(path=path, scenario=scenario)
    return path


def write_failure(error):
    """report.json / report.txt for a scenario that stopped with an error (VaM hung or exited)."""
    path = LAST_OUT.get("path")
    if not path:
        return
    report = {"scenario": LAST_OUT.get("scenario"), "passed": False, "error": str(error),
              "diagnosis": getattr(error, "diagnosis", None)}
    with open(os.path.join(path, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, ensure_ascii=False, default=str)
    with open(os.path.join(path, "report.txt"), "w", encoding="utf-8") as f:
        f.write("[FAIL] %s did not finish: %s\n" % (report["scenario"], error))
    print("%s FAILED (%s)" % (report["scenario"], path))


class Checks:
    def __init__(self):
        self.items = []

    def check(self, name, ok, detail):
        self.items.append({"check": name, "pass": bool(ok), "detail": detail})
        print("  [%s] %s: %s" % ("PASS" if ok else "FAIL", name, detail))
        return ok

    def within(self, name, value, low, high, unit=""):
        ok = value is not None and low <= value <= high
        return self.check(name, ok, "%s%s (expected %s..%s)" % (value, unit, low, high))

    @property
    def passed(self):
        return all(item["pass"] for item in self.items)


def selftest_env(simxr_dir, log_dir, port):
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(simxr_dir, "simxr-openxr.json")
    env["SIMXR_LOG_DIR"] = log_dir
    env["SIMXR_PORT"] = str(port)
    # Keep the selfcheck to SimXR alone: VD's compatibility layer is switched off here, and the
    # OFXR layer is only registered while its tray runs (selfcheck refuses to run then).
    env["DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY"] = "1"
    return env


def active_implicit_layers():
    """HKCU/HKLM implicit OpenXR layers that are enabled (value 0), other than VD's compatibility layer."""
    import winreg

    layers = []
    for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            key = winreg.OpenKey(hive, r"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit")
        except OSError:
            continue
        with key:
            index = 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(key, index)
                except OSError:
                    break
                index += 1
                if value == 0 and "openxr-oculus-compatibility" not in name.lower():
                    layers.append(name)
    return layers


def selftest_has(simxr_dir, option):
    """Whether this stage's simxr_selftest.exe knows a command-line option (older stages lack some)."""
    with open(os.path.join(simxr_dir, "simxr_selftest.exe"), "rb") as f:
        return option.encode() in f.read()  # the usage text names every option


def run_selftest(simxr_dir, log_dir, seconds, port, script, sleep_ms=0, gpu_ms=0, env=None):
    """Starts simxr_selftest.exe, runs script(client) once the session is up, waits for exit."""
    os.makedirs(log_dir, exist_ok=True)
    exe = os.path.join(simxr_dir, "simxr_selftest.exe")
    loader = os.path.join(simxr_dir, "openxr_loader.dll")
    if not os.path.exists(loader):
        loader = LOADER
    args = [exe, loader, str(seconds), "--log", os.path.join(log_dir, "selftest.log")]
    if sleep_ms:
        args += ["--sleep-ms", str(sleep_ms)]
    if gpu_ms:
        args += ["--gpu-ms", str(gpu_ms)]
    process_env = selftest_env(simxr_dir, log_dir, port)
    process_env.update(env or {})
    process = subprocess.Popen(args, env=process_env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result = None
    try:
        with Client.wait(log_dir, timeout=30, process=process) as client:
            result = script(client)
    finally:
        try:
            process.wait(timeout=seconds + 30)
        except subprocess.TimeoutExpired:
            process.kill()
            raise SimXRError("simxr_selftest.exe did not exit")
    with open(os.path.join(log_dir, "selftest.log"), errors="replace") as f:
        log = f.read()
    return process.returncode, log, result


def check_captures(capture_dir, checks):
    from PIL import Image

    entries = []
    with open(os.path.join(capture_dir, "capture.jsonl")) as f:
        for line in f:
            if line.strip():
                entries.append(json.loads(line))
    checks.check("capture files", len(entries) == 8, "%d eye images listed in capture.jsonl (expected 8)" % len(entries))
    bad = []
    counters = {}
    for entry in entries:
        image = Image.open(os.path.join(capture_dir, entry["file"])).convert("RGB")
        eye = 0 if entry["eye"] == "left" else 1
        r, g, b = image.getpixel((400, 100))
        square = image.getpixel((132, 232))
        outside = image.getpixel((90, 232))
        expected_g = max(0, min(255, round(entry["render_ypr"][0] + 128)))
        problems = []
        if b != 64 + 128 * eye:
            problems.append("B=%d (eye %s expects %d)" % (b, entry["eye"], 64 + 128 * eye))
        if abs(g - expected_g) > 1:
            problems.append("G=%d but rendered yaw %.3f gives %d" % (g, entry["render_ypr"][0], expected_g))
        if square != (255, 255, 255):
            problems.append("square pixel %s" % (square,))
        if outside == (255, 255, 255):
            problems.append("pixel left of the square is white")
        if problems:
            bad.append("%s: %s" % (entry["file"], "; ".join(problems)))
        counters.setdefault(entry["seq"], set()).add(r)
    checks.check("capture content", not bad, "all eye images match what was rendered" if not bad else bad[:4])
    # Both eyes of a frame carry the same frame counter, and consecutive frames count up by one.
    same = all(len(values) == 1 for values in counters.values())
    seqs = sorted(counters)
    steps = [((next(iter(counters[b])) - next(iter(counters[a]))) % 256, b - a) for a, b in zip(seqs, seqs[1:])]
    checks.check("capture frame counter", same and all(dr == ds for dr, ds in steps),
                 "seq %s, counter steps %s" % (seqs, [dr for dr, _ in steps]))


def selfcheck(args):
    simxr_dir = find_simxr(args.simxr)
    layers = active_implicit_layers()
    if layers:
        raise SimXRError("implicit OpenXR layers are active (is OFXR's tray running?): %s" % layers)
    out = new_out_dir("selfcheck", args.out)
    print("SimXR %s -> %s" % (simxr_dir, out))
    checks = Checks()
    report = {"scenario": "selfcheck", "simxr": simxr_dir, "runs": {}}

    # 1. An application that keeps up, then a sine head turn, then a capture during a still head.
    def fast_script(client):
        status = client.status()
        time.sleep(2.0)
        client.mark("steady")
        time.sleep(2.0)
        client.mark("sine")
        # Scheduled 200 ms ahead, so that every frame rendered from now on can predict it exactly.
        client.motion(type="sine", axis="yaw", amplitude=30, period_s=1.0, duration_s=2.0, delay_ms=200)
        time.sleep(2.4)
        client.mark("capture")
        client.head(ypr=[17, 0, 0])
        time.sleep(0.2)
        capture = client.capture(frames=4, crop=[0, 0, 512, 512], name="selfcheck", wait=True, timeout=20)
        client.mark("end")
        return {"status": status, "capture": capture}

    log_dir = os.path.join(out, "fast")
    code, log, result = run_selftest(simxr_dir, log_dir, 8, args.port, fast_script)
    checks.check("fast app exit", code == 0 and "RESULT: PASS" in log, "exit %s" % code)
    device = result["status"].get("device", {})
    checks.check("device", device.get("model") == "Meta Quest 3" and device.get("eye_w") == 3072,
                 "%s %sx%s %s Hz" % (device.get("model"), device.get("eye_w"), device.get("eye_h"), device.get("display_hz")))
    steady = analyze(log_dir, from_mark="steady", to_mark="sine")
    sine = analyze(log_dir, from_mark="sine", to_mark="capture")
    report["runs"]["fast_steady"] = steady
    report["runs"]["fast_sine"] = sine
    print(format_report(steady))
    checks.within("steady displayed fps", steady["displayed_fps"], 118.5, 121.0)
    checks.within("steady repeats", steady["repeats"], 0, 2)
    checks.within("steady pose error p99", steady["pose_error_deg"]["p99"], 0.0, 0.01, " deg")
    checks.within("sine displayed fps", sine["displayed_fps"], 118.0, 121.0)
    # Frames shown on the refresh they were rendered for carry no pose error; the only exception is
    # VDXR's reused-index race (a frame goes out one refresh early), which the analysis counts.
    for name, run in (("steady", steady), ("sine", sine)):
        unexplained = run["early_frames"] - run["early_frames_reused_index"] + run["late_frames"]
        checks.check("%s: frames shown on their intended refresh" % name, unexplained == 0,
                     "%s, reused index %d, early from reuse %d" % (run["shown_vs_intended_refresh"],
                                                                    run["reused_frame_index"],
                                                                    run["early_frames_reused_index"]))
    checks.within("sine pose error of new frames p95", sine["pose_error_new_frames_deg"]["p95"], 0.0, 0.05, " deg")
    check_captures(os.path.join(log_dir, "captures"), checks)

    # 2. An application that sleeps 10 ms per frame: half rate; repeats are stale during a turn.
    def slow_script(client):
        time.sleep(1.5)
        client.mark("slow")
        time.sleep(2.0)
        client.mark("turn")
        client.motion(type="turn", axis="yaw", speed=90, duration_s=2.0)
        time.sleep(2.0)
        client.mark("end")
        return None

    log_dir = os.path.join(out, "slow")
    code, log, _ = run_selftest(simxr_dir, log_dir, 6.5, args.port, slow_script, sleep_ms=10)
    checks.check("slow app exit", code == 0 and "RESULT: PASS" in log, "exit %s" % code)
    slow = analyze(log_dir, from_mark="slow", to_mark="turn")
    turn = analyze(log_dir, from_mark="turn", to_mark="end")
    report["runs"]["slow"] = slow
    report["runs"]["slow_turn"] = turn
    print(format_report(slow))
    # VDXR submits asynchronously and xrWaitFrame does not block (defer_frame_wait), so a slow
    # application free-runs at its own rate: every frame it finishes is shown at the next refresh
    # and the refreshes in between repeat (no half-rate lock).
    app_fps = 1000.0 / slow["submit_interval_ms"]["p50"]
    hz = slow["display_hz"]
    checks.within("slow displayed fps = app rate", round(slow["displayed_fps"] / min(hz, app_fps), 4), 0.97, 1.02)
    checks.within("slow repeat ratio = 1 - app rate / refresh", round(slow["repeat_ratio"] - (1.0 - min(hz, app_fps) / hz), 4),
                  -0.03, 0.03)
    # A free-running application cannot be told which refresh its frame will make: frames shown a
    # refresh after their display time carry one refresh of head motion, never more.
    late_share = turn["late_frames"] / max(1, turn["submitted"])
    checks.check("slow turn: only late frames carry pose error",
                 (turn["pose_error_new_frames_deg"]["p50"] <= 0.05) == (late_share < 0.5)
                 and turn["pose_error_new_frames_deg"]["max"] <= 0.80,
                 "new-frame pose error p50 %s max %s deg, shown vs intended %s" % (
                     turn["pose_error_new_frames_deg"]["p50"], turn["pose_error_new_frames_deg"]["max"],
                     turn["shown_vs_intended_refresh"]))
    # A repeated refresh shows a frame one refresh older than the head: 90 deg/s / 120 Hz = 0.75 deg.
    checks.within("slow turn pose error max (repeats)", turn["pose_error_deg"]["max"], 0.70, 0.80, " deg")

    # 3. A GPU-bound application (about 15 ms of GPU work per frame). The display model holds a
    #    frame's begin until at most SIMXR_MAX_QUEUED_FRAMES (default 1) earlier frames are still on
    #    the GPU, so frames are not queued up to reach the display several refreshes late; the same
    #    application without the limit is measured for comparison (reported, not checked: how deep
    #    the D3D11 driver lets the CPU run ahead is the driver's business).
    if not selftest_has(simxr_dir, "--gpu-ms"):
        checks.check("GPU-bound app", False, "this SimXR build's simxr_selftest.exe has no --gpu-ms")
    else:
        def gpu_script(client):
            time.sleep(2.0)
            client.mark("gpu")
            time.sleep(2.5)
            client.mark("end")
            return client.stats()

        gpu_runs = {}
        for name, extra in (("gpu", {}), ("gpu_unlimited", {"SIMXR_MAX_QUEUED_FRAMES": "0"})):
            log_dir = os.path.join(out, name)
            code, log, stats = run_selftest(simxr_dir, log_dir, 5.5, args.port, gpu_script, gpu_ms=15, env=extra)
            checks.check("%s app exit" % name, code == 0 and "RESULT: PASS" in log, "exit %s" % code)
            gpu_runs[name] = analyze(log_dir, from_mark="gpu", to_mark="end")
            gpu_runs[name]["queue_waits"] = stats.get("gpu_queue_waits")
            report["runs"][name] = gpu_runs[name]
            print("-- %s" % name)
            print(format_report(gpu_runs[name]))
        gpu = gpu_runs["gpu"]
        checks.check("GPU-bound app: frame begins held by the queue limit", (gpu["queue_waits"] or 0) > 0,
                     "%s held-back frame begins" % gpu["queue_waits"])
        # With one frame allowed in flight a frame waits for its predecessor's GPU work and then
        # runs its own: about 2 x 15 ms from submission, plus up to two refreshes of alignment.
        # (Without the limit the selftest queued ~0.9 s of GPU work: frames ~108 refreshes late.)
        limit_ms = 2 * 15.0 + 2 * 1000.0 / (gpu["display_hz"] or 120.0)
        age = gpu["age_at_display_ms"]
        checks.within("GPU-bound app: age at display p95 bounded by the queue", age["p95"] if age else None, 0.0,
                      round(limit_ms, 1), " ms (shown vs intended %s)" % gpu["shown_vs_intended_refresh"])
        checks.within("GPU-bound app: displayed fps (about 1000 / 15 ms)", gpu["displayed_fps"], 40.0, 80.0)
        report["gpu_queue_comparison"] = {
            name: {"age_at_display_ms_p50": r["age_at_display_ms"]["p50"] if r["age_at_display_ms"] else None,
                   "shown_vs_intended_refresh": r["shown_vs_intended_refresh"], "displayed_fps": r["displayed_fps"]}
            for name, r in gpu_runs.items()}
        print("GPU queue limit vs none: %s" % report["gpu_queue_comparison"])

    report["checks"] = checks.items
    report["passed"] = checks.passed
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(out, "report.txt"), "w") as f:
        for item in checks.items:
            f.write("[%s] %s: %s\n" % ("PASS" if item["pass"] else "FAIL", item["check"], item["detail"]))
    print("selfcheck %s (%s)" % ("PASSED" if checks.passed else "FAILED", out))
    return 0 if checks.passed else 1


# ------------------------------------------------------------------ VaM scenarios
#
# Each starts VaM once on SimXR through Launch.ps1 (vamrun.VamRun: the launch panel's current
# settings, --set overrides on top), drives it through the SimXR control port and the VaM test
# agent, asks VaM to quit and waits for the launcher to restore everything. Measurements that
# depend on GPU load (fps, hitches) are reported as findings against soft limits.

# A scene with a person, hair and four lights (one of the user's packages): what pacing and
# head-turn checks need; VaM's start screen is an empty dark room. --scene none skips it.
DEFAULT_SCENE = "VAMSOY.XiaoYi.1:/Saves/scene/VAMSOY COLLECTION/XiaoYi.json"


def scene_of(args, default=DEFAULT_SCENE):
    if args.scene is None:
        return default
    return None if args.scene.lower() == "none" else args.scene


def sim_stage(args):
    """--simxr for a VaM scenario: the stage folder's name (Launch.ps1 -SimXRStage), else the newest stage."""
    if not args.simxr:
        return None
    path = os.path.abspath(args.simxr)
    if os.path.dirname(path) != TASK_ROOT or not os.path.exists(os.path.join(path, "build.json")):
        raise SimXRError("--simxr for a VaM scenario must be a staged SimXR-v* folder in %s" % TASK_ROOT)
    return os.path.basename(path)


def parse_sim(pairs):
    """--sim KEY=VALUE: SimXR settings for the run (SIMXR_ prefix optional), e.g. MAX_QUEUED_FRAMES=0."""
    env = {}
    for pair in pairs or []:
        key, _, value = pair.partition("=")
        key = key.strip().upper()
        env[key if key.startswith("SIMXR_") else "SIMXR_" + key] = value.strip()
    return env


def parse_overrides(pairs):
    overrides = {}
    for pair in pairs or []:
        key, _, value = pair.partition("=")
        try:
            overrides[key] = int(value)
        except ValueError:
            overrides[key] = value
    return overrides


def write_report(out, scenario, report, checks):
    report["scenario"] = scenario
    report["checks"] = checks.items
    report["passed"] = checks.passed
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, ensure_ascii=False, default=str)
    with open(os.path.join(out, "report.txt"), "w", encoding="utf-8") as f:
        for item in checks.items:
            f.write("[%s] %s: %s\n" % ("PASS" if item["pass"] else "FAIL", item["check"], item["detail"]))
    print("%s %s (%s)" % (scenario, "PASSED" if checks.passed else "FAILED", out))
    return 0 if checks.passed else 1


def wrap_degrees(value):
    return (value + 180.0) % 360.0 - 180.0


def vam_window_stats(run, start_ms, end_ms, offset_ms):
    """VaM's own frame times (agent frame log) between two SimXR times."""
    from vamrun import read_frame_log
    rows = read_frame_log(os.path.join(run.log_dir, "vam-frames.csv"))
    window = [r for r in rows if start_ms <= r["qpc_ms"] + offset_ms < end_ms]
    times = sorted(r["dt_ms"] for r in window)
    if not times:
        return None
    median = times[len(times) // 2]
    previous_gc = {}
    for a, b in zip(rows, rows[1:]):
        previous_gc[b["frame"]] = a["gc"]

    def gc_step(r):
        """True when the Mono collection count stepped on this frame (None: agent without the column)."""
        before = previous_gc.get(r["frame"])
        return None if r["gc"] is None or before is None else r["gc"] > before

    # [SimXR time of the frame's end, ms, a garbage collection ran in it]
    long_frames = [[round(r["qpc_ms"] + offset_ms, 1), round(r["dt_ms"], 2), gc_step(r)]
                   for r in window if r["dt_ms"] > 1.5 * median]
    gcs = [r for r in window if gc_step(r)]
    return {"frames": len(window), "fps": round(len(window) / ((end_ms - start_ms) / 1000.0), 2),
            "dt_p50": round(median, 3), "dt_p95": round(times[int(0.95 * (len(times) - 1))], 3),
            "dt_max": round(times[-1], 3), "long_frames": len(long_frames), "long_frame_list": long_frames[:30],
            "gc_collections": len(gcs) if window[0]["gc"] is not None else None}


def mark_times(log_dir):
    from simxr import read_markers
    return {m["label"]: m["t_ms"] for m in read_markers(os.path.join(log_dir, "simxr"))}


def vam_smoke(args):
    from vamrun import VamRun
    out = new_out_dir("smoke", args.out)
    checks = Checks()
    report = {}
    run = VamRun("smoke", parse_overrides(args.set), max_seconds=args.max_seconds, sim_env=parse_sim(args.sim),
                 stage=sim_stage(args))
    with run:
        status = run.wait_ready()
        report["agent_status"] = status
        sim_status = run.sim.status()
        report["sim_status"] = sim_status
        xr = status.get("xr", {})
        checks.check("VaM in VR", xr.get("enabled") is True and bool(xr.get("device")),
                     "XR %s, device %s, eye %sx%s" % (xr.get("enabled"), xr.get("device"), xr.get("eye_w"), xr.get("eye_h")))
        checks.check("headset", sim_status.get("device", {}).get("model") == "Meta Quest 3",
                     "%s, swapchains %s" % (sim_status.get("device"), sim_status.get("swapchains")))
        checks.check("test agent", "local.vam.vrframegen.testagent" in status.get("plugins", []), "plugins %s" % status.get("plugins"))
        # Head tracking reaches VaM, in the right direction (Unity: yaw right +, pitch down +).
        run.sim.head(ypr=[25, 0, 0])
        time.sleep(0.6)
        yaw = run.agent.call("status")["head"]["euler"][1]
        run.sim.head(ypr=[0, 15, 0])
        time.sleep(0.6)
        pitch = run.agent.call("status")["head"]["euler"][0]
        run.sim.head(ypr=[0, 0, 0])
        checks.check("head yaw reaches VaM", abs(wrap_degrees(yaw + 25)) < 1.5,
                     "SimXR yaw +25 (left) -> Unity euler y %.2f (expect -25)" % wrap_degrees(yaw))
        checks.check("head pitch reaches VaM", abs(wrap_degrees(pitch + 15)) < 1.5,
                     "SimXR pitch +15 (up) -> Unity euler x %.2f (expect -15)" % wrap_degrees(pitch))
        time.sleep(1.0)
        run.sim.mark("idle")
        time.sleep(args.seconds)
        run.sim.mark("idle_end")
        capture = run.capture(frames=1, scale=0.25, name="smoke", timeout=30)
        report["capture"] = capture
        report["agent_status_end"] = run.agent.call("status")
    idle = analyze(os.path.join(run.log_dir, "simxr"), from_mark="idle", to_mark="idle_end")
    report["idle"] = idle
    print(format_report(idle))
    report["results"] = run.results()
    report["settings"] = run.settings
    report["log_dir"] = run.log_dir
    report["run_dir"] = run.run_dir
    checks.check("frames shown", (idle["displayed_fps"] or 0) > 10, "displayed %s fps" % idle["displayed_fps"])
    checks.check("capture written", capture.get("written", 0) == 2, "%s eye image(s) in %s" % (capture.get("written"), capture.get("dir")))
    sr = report["results"]["dlss_sr"]
    checks.check("DLSS SR running", bool(sr) and sr.get("status") == "evaluating", "dlss-status %s" % (sr,))
    if run.settings.get("framegen"):
        fg = report["results"]["dlss_fg"]
        checks.check("DLSS-G generating", bool(fg) and fg.get("status") == "generating", "dlssg-status %s" % (fg,))
    return write_report(out, "smoke", report, checks)


def vam_pacing(args):
    from vamrun import VamRun
    out = new_out_dir("pacing", args.out)
    checks = Checks()
    report = {}
    run = VamRun("pacing", parse_overrides(args.set), max_seconds=args.max_seconds, sim_env=parse_sim(args.sim),
                 stage=sim_stage(args))
    with run:
        report["agent_status"] = run.wait_ready()
        scene = scene_of(args)
        if scene:
            report["scene"] = scene
            report["scene_status"] = run.load_scene(scene)
        time.sleep(args.settle)
        run.agent.call("frame_log", on=True)
        run.sim.mark("still")
        time.sleep(args.seconds)
        run.sim.mark("turn")
        run.sim.motion(type="sine", axis="yaw", amplitude=35, period_s=2.5, duration_s=args.seconds, delay_ms=100)
        time.sleep(args.seconds + 0.4)
        run.sim.mark("end")
        run.agent.call("frame_log", on=False)
        offset = run.time_offset_ms()
        report["agent_status_end"] = run.agent.call("status")
    sim_dir = os.path.join(run.log_dir, "simxr")
    marks = mark_times(run.log_dir)
    for name, a, b in (("still", "still", "turn"), ("turn", "turn", "end")):
        window = analyze(sim_dir, from_mark=a, to_mark=b)
        window["vam"] = vam_window_stats(run, marks[a], marks[b], offset) if offset is not None else None
        report[name] = window
        print("-- %s" % name)
        print(format_report(window))
        print("VaM frames: %s" % window["vam"])
    report["results"] = run.results()
    report["settings"] = run.settings
    report["log_dir"] = run.log_dir
    # Checked on what the pipeline itself produces (VaM's frame times, the frames VaM + DLSS SR +
    # DLSS-G hand to the runtime), not on which of them SimXR's display model shows: that depends
    # on how SimXR paces and picks frames, a stand-in for Virtual Desktop's unknown policy. A
    # GPU-bound scene (VaM ~35 fps x 3 < 120 Hz) is normal play and passes if it is even.
    for name in ("still", "turn"):
        w = report[name]
        checks.check("%s: pipeline output has no gap of 4+ refreshes" % name, w["pipeline_gaps"] == 0,
                     "%d gap(s) [SimXR ms, ms] %s; submitted %s fps" % (
                         w["pipeline_gaps"], w["pipeline_gap_list"][:10], w["submitted_fps"]))
        if name == "turn":
            # While the head moves, DLSS-G resubmitting the last real frame (VaM stalled) shows as
            # frames with one render pose that are older than the pipeline's usual delay.
            checks.check("turn: no frozen frames (one render pose resubmitted while VaM stalls)", w["frozen_pose_runs"] == 0,
                         "%d run(s) [SimXR ms, frames, ms] %s" % (w["frozen_pose_runs"], w["frozen_pose_list"][:10]))
        if w.get("vam"):
            # A spike: one VaM frame taking 2.5x the window's median or more (GC, a scene search...).
            checks.within("%s: VaM's longest frame vs its median" % name,
                          round(w["vam"]["dt_max"] / w["vam"]["dt_p50"], 2) if w["vam"]["dt_p50"] else None, 0.0, 2.5,
                          "x (max %s ms, median %s ms, VaM %s fps; long frames [SimXR ms, ms, GC ran]: %s)" % (
                              w["vam"]["dt_max"], w["vam"]["dt_p50"], w["vam"]["fps"], w["vam"]["long_frame_list"][:8]))
    return write_report(out, "pacing", report, checks)


def vam_headturn(args):
    from vamrun import VamRun
    out = new_out_dir("headturn", args.out)
    checks = Checks()
    report = {"turns": {}}
    speeds = [int(v) for v in args.speeds.split(",")]
    if args.reanalyze:
        report["reanalyzed"] = args.reanalyze
        headturn_analysis(args.reanalyze, speeds, out, checks, report)
        return write_report(out, "headturn", report, checks)
    run = VamRun("headturn", parse_overrides(args.set), max_seconds=args.max_seconds, sim_env=parse_sim(args.sim),
                 stage=sim_stage(args))
    with run:
        report["agent_status"] = run.wait_ready()
        scene = scene_of(args)
        if scene:
            report["scene"] = scene
            report["scene_status"] = run.load_scene(scene)
        time.sleep(args.settle)
        for speed in speeds:
            run.sim.head(ypr=[-speed * 0.75, 0, 0])  # start to the right so the turn sweeps the scene
            time.sleep(1.0)
            run.sim.mark("turn%d" % speed)
            run.sim.motion(type="turn", axis="yaw", speed=speed, duration_s=1.5, delay_ms=200)
            run.capture(frames=args.frames, every=1, delay_ms=700, scale=args.scale, eyes="left",
                        name="turn%d" % speed, timeout=60)
            time.sleep(0.3)
            run.sim.mark("turn%d_end" % speed)
        run.sim.head(ypr=[0, 0, 0])
    headturn_analysis(run.log_dir, speeds, out, checks, report)
    report["results"] = run.results()
    report["settings"] = run.settings
    report["log_dir"] = run.log_dir
    return write_report(out, "headturn", report, checks)


def headturn_analysis(log_dir, speeds, out, checks, report):
    """Per turn speed: pacing of the window, and whether consecutive captured frames move by what
    their render poses call for (imagecheck.turn_consistency, on the cylinder)."""
    import imagecheck
    sim_dir = os.path.join(log_dir, "simxr")
    capture_dir = os.path.join(sim_dir, "captures")
    for speed in speeds:
        window = analyze(sim_dir, from_mark="turn%d" % speed, to_mark="turn%d_end" % speed)
        rows = imagecheck.turn_consistency(capture_dir, "turn%d" % speed)
        sheet = imagecheck.contact_sheet(capture_dir, "turn%d" % speed, os.path.join(out, "turn%d.png" % speed))
        errors = [abs(r["error_px"]) for r in rows]
        ghosts = [r["second_peak_ratio"] for r in rows]
        report["turns"][speed] = {"pacing": window, "frames": rows, "contact_sheet": sheet}
        print("-- turn %d deg/s" % speed)
        print(format_report(window))
        for r in rows:
            print("   seq %(seq)d  dyaw %(dyaw_deg)+.3f  expected %(expected_dx)+.2f px  measured %(measured_dx)+.2f px"
                  "  err %(error_px)+.2f  second peak %(second_peak_ratio).2f" % r)
        if rows:
            # On the cylinder a correct frame moves within ~0.3 px of its pose difference (synthetic
            # views: < 0.2 px). The error as time: how far off its pose's moment the content was
            # drawn (at 240 deg/s, 3x DLSS-G frames sit ~0.5 ms off in the 3-frame cadence).
            # Limit: 1 ms of motion, but never below 1 px (measurement noise at slow turns).
            px_per_ms = speed / 1000.0 * (rows[0]["expected_dx"] / rows[0]["dyaw_deg"] if rows[0]["dyaw_deg"] else 0.0)
            worst = max(errors)
            worst_ms = worst / px_per_ms if px_per_ms else None
            checks.check("turn %d: content moves with the pose (max error <= 1 ms of motion, 1 px floor)" % speed,
                         worst <= max(1.0, px_per_ms * 1.0),
                         "max |error| %.2f px = %s ms of motion at %d deg/s" % (
                             worst, "%.2f" % worst_ms if worst_ms is not None else "-", speed))
            # A 50/50 double image gives a second peak of ~0.55-0.6, real content 0.1-0.3; ghosts
            # weaker than ~40% are below this measure's noise (synthetic: 30% ghost -> 0.18).
            checks.within("turn %d: no double image (max second-peak ratio)" % speed, round(max(ghosts), 3), 0.0, 0.45)
        else:
            checks.check("turn %d: consecutive captures" % speed, False, "no consecutive frame pairs captured")
        # Reported, not checked: how far behind the head a frame's content is at the display time it
        # was rendered for (frame generation waits for the next real frame); the headset's timewarp
        # re-aims it. Measured on the submitted frames, not on SimXR's display.
        age = window.get("pose_age_intended_ms")
        print("   content delay at intended display p50 %s ms, p95 %s ms" % (
            age["p50"] if age else "-", age["p95"] if age else "-"))


def vam_menu(args):
    from vamrun import VamRun
    out = new_out_dir("menu", args.out)
    checks = Checks()
    report = {}
    run = VamRun("menu", parse_overrides(args.set), max_seconds=args.max_seconds, sim_env=parse_sim(args.sim),
                 stage=sim_stage(args))

    def watch(seconds, before):
        """Changes of "VaM's menus are open" (main HUD or world UI) from the state before the press,
        polled as fast as VaM answers: one entry = one clean toggle, more = a bounce."""
        transitions = []
        start = time.perf_counter()
        last = before
        while time.perf_counter() - start < seconds:
            visible = run.agent.call("status")["menus_open"]
            if visible != last:
                transitions.append((round(time.perf_counter() - start, 3), visible))
                last = visible
        return transitions

    with run:
        report["agent_status"] = run.wait_ready()
        time.sleep(1.5)
        run.sim.mark("closed_before")
        time.sleep(2.0)
        run.sim.mark("press_open")
        before_open = run.agent.call("status")["menus_open"]
        run.sim.press(args.hand, args.button, ms=120)
        opened = watch(2.5, before_open)
        run.sim.mark("open")
        time.sleep(2.0)
        run.sim.mark("press_close")
        before_close = run.agent.call("status")["menus_open"]
        run.sim.press(args.hand, args.button, ms=120)
        closed = watch(2.5, before_close)
        run.sim.mark("closed_after")
        time.sleep(2.0)
        run.sim.mark("end")
    report["open_transitions"] = opened
    report["close_transitions"] = closed
    sim_dir = os.path.join(run.log_dir, "simxr")
    windows = {}
    for name, a, b in (("before", "closed_before", "press_open"), ("open", "open", "press_close"), ("after", "closed_after", "end")):
        windows[name] = analyze(sim_dir, from_mark=a, to_mark=b)
    report["windows"] = windows
    report["settings"] = run.settings
    report["log_dir"] = run.log_dir
    report["results"] = run.results()
    print("open: %s\nclose: %s" % (opened, closed))
    for name, w in windows.items():
        print("-- %s: submitted %s fps, displayed %s fps, repeats %s" % (name, w["submitted_fps"], w["displayed_fps"], w["repeats"]))
    report["menus_open_before_presses"] = [before_open, before_close]
    checks.check("press opens the main menu once (no bounce)", before_open is False and [v for _, v in opened] == [True],
                 "menus open before %s, changes after the press [s, open]: %s" % (before_open, opened))
    checks.check("second press closes it once", before_close is True and [v for _, v in closed] == [False],
                 "menus open before %s, changes after the press [s, open]: %s" % (before_close, closed))
    if run.settings.get("framegen") and run.settings.get("dlss_menu_native"):
        before, open_, after = windows["before"]["submitted_fps"], windows["open"]["submitted_fps"], windows["after"]["submitted_fps"]
        checks.check("frame generation pauses while the menu is open", bool(before and open_) and open_ < 0.8 * before,
                     "submitted fps before %s, menu open %s, after %s" % (before, open_, after))
        checks.check("frame generation resumes after the menu closes", bool(before and after) and after > 0.9 * before,
                     "submitted fps before %s, after %s" % (before, after))
    return write_report(out, "menu", report, checks)


SCENARIOS = {"selfcheck": selfcheck, "smoke": vam_smoke, "pacing": vam_pacing, "headturn": vam_headturn, "menu": vam_menu}


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario", choices=sorted(SCENARIOS))
    parser.add_argument("--simxr", help="SimXR folder (default: newest SimXR-v* stage; selfcheck falls back to the "
                        "build output); VaM scenarios take a staged SimXR-v* folder only")
    parser.add_argument("--out", help="result folder (default: a new folder under sim-results)")
    parser.add_argument("--port", type=int, default=37611, help="selfcheck: control port")
    parser.add_argument("--set", action="append", metavar="KEY=VALUE",
                        help="VaM scenarios: a launch panel setting for this run (e.g. frame_multiplier=4, dlss_preset=M)")
    parser.add_argument("--scene", help="VaM scenarios: a scene to load first (a path relative to VaM's folder or "
                        "Creator.Package.1:/Saves/scene/x.json; pacing and headturn default to %s, 'none' = VaM's "
                        "start screen)" % DEFAULT_SCENE)
    parser.add_argument("--sim", action="append", metavar="KEY=VALUE",
                        help="VaM scenarios: a SimXR setting for this run (e.g. MAX_QUEUED_FRAMES=0, LATCH_US=2000)")
    parser.add_argument("--reanalyze", metavar="LOG_DIR",
                        help="headturn: analyse an earlier run's logs and captures again instead of running VaM")
    parser.add_argument("--seconds", type=float, default=10.0, help="smoke/pacing: length of each measured window")
    parser.add_argument("--settle", type=float, default=5.0, help="pacing/headturn: seconds after loading before measuring")
    parser.add_argument("--max-seconds", type=int, default=600, help="VaM scenarios: Launch.ps1 closes VaM after this")
    parser.add_argument("--speeds", default="60,120,240", help="headturn: yaw speeds in deg/s")
    parser.add_argument("--frames", type=int, default=16, help="headturn: consecutive frames captured per turn")
    parser.add_argument("--scale", type=float, default=0.5, help="headturn: capture scale")
    parser.add_argument("--hand", default="left", help="menu: controller with the menu button")
    # OpenComposite maps OpenVR's ApplicationMenu (what VaM's menu toggle reads) to Y on the left Touch
    # controller and B on the right; the Touch menu button is OpenVR's system button.
    parser.add_argument("--button", default="y", help="menu: button that toggles VaM's main menu (y left / b right)")
    args = parser.parse_args(argv)
    return SCENARIOS[args.scenario](args)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (SimXRError, OSError) as e:
        print("vrtest: %s" % e, file=sys.stderr)
        write_failure(e)
        sys.exit(1)
