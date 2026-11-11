"""SimXR client and log analysis (the simulated Quest 3 in source\\SimXR).

Library:
    from simxr import Client, analyze
    with Client.wait(log_dir) as sim:          # waits for simxr-control.json, connects
        sim.call("motion", type="sine", axis="yaw", amplitude=30, period_s=2, duration_s=8)
        sim.mark("turn")
        sim.capture(frames=8, scale=0.5, wait=True)
        print(sim.stats(reset=True))
    report = analyze(log_dir)                 # simxr-vsync.csv / simxr-frames.csv / markers

CLI:
    python simxr.py ping|status|stats|help [--port N | --log-dir DIR]
    python simxr.py call '{"cmd":"press","hand":"left","button":"menu"}'
    python simxr.py analyze DIR [--from-mark LABEL] [--to-mark LABEL] [--json]

Times in the CSVs are milliseconds since the simulated headset was loaded. The vsync CSV has one
row per simulated display refresh: which submitted frame was on screen (seq), whether it was new,
how many refreshes in a row it has been repeated, its age (submit -> display) and the angle between
the pose it was rendered for and the true head pose at that refresh. The frames CSV has one row per
submitted frame; sensor_ms is the display time the application rendered it for (xrEndFrame
displayTime), so "shown vs intended refresh" tells late and early frames apart from repeats.
"""
import argparse
import bisect
import csv
import json
import os
import socket
import statistics
import sys
import time

DEFAULT_PORT = 37601


class SimXRError(RuntimeError):
    pass


class MainThreadBusy(SimXRError):
    """The VaM test agent stopped waiting for VaM's main thread; the request is still queued."""


class Client:
    def __init__(self, port=DEFAULT_PORT, host="127.0.0.1", timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        self.next_id = 1

    @staticmethod
    def wait(log_dir=None, port=None, timeout=120.0, process=None, info_file="simxr-control.json", io_timeout=10.0):
        """Connects once the server is up: SimXR writes simxr-control.json when the headset session
        starts; the VaM test agent writes vam-agent.json (same protocol, info_file="vam-agent.json")."""
        deadline = time.time() + timeout
        info_path = os.path.join(log_dir, info_file) if log_dir else None
        last_error = None
        while time.time() < deadline:
            if process is not None and process.poll() is not None:
                raise SimXRError("the process exited (code %s) before %s appeared" % (process.returncode, info_file))
            target = port
            if target is None and info_path and os.path.exists(info_path):
                try:
                    with open(info_path) as f:
                        target = json.load(f)["port"]
                except (ValueError, KeyError, OSError) as e:
                    last_error = e
            if target is not None:
                try:
                    client = Client(target, timeout=io_timeout)
                except OSError as e:
                    last_error = e
                else:
                    try:
                        client.call("ping")
                        return client
                    except OSError as e:
                        client.close()
                        last_error = e
            time.sleep(0.25)
        raise SimXRError("no server from %s within %.0f s (%s)" % (info_file, timeout, last_error))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def call(self, cmd, **fields):
        return self.receive(self.send(cmd, **fields), cmd)

    def send(self, cmd, **fields):
        """Sends a request; returns its id for receive()."""
        request = dict(fields)
        request["cmd"] = cmd
        request["id"] = self.next_id
        self.next_id += 1
        self.sock.sendall((json.dumps(request) + "\n").encode())
        return request["id"]

    def receive(self, request_id, cmd=""):
        """The answer to one request. A socket timeout leaves the request pending: calling receive()
        again keeps waiting for the same answer (nothing is sent twice)."""
        while True:
            while b"\n" not in self.buffer:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise SimXRError("control port closed")
                self.buffer += chunk
            line, self.buffer = self.buffer.split(b"\n", 1)
            response = json.loads(line.decode("utf-8", "replace"))
            # A late answer to an earlier request that timed out is skipped (the agent echoes ids as strings).
            if str(response.get("id")) == str(request_id):
                break
            # The VaM test agent (TestAgent-v3) gives up on VaM's main thread after 120 s (a long scene
            # load) and says so without the request's id; the request itself stays queued and still
            # runs, but no other answer to it will come.
            if "id" not in response and "main thread" in str(response.get("error", "")):
                raise MainThreadBusy("%s: %s" % (cmd, response.get("error")))
        if not response.get("ok"):
            raise SimXRError("%s: %s" % (cmd, response.get("error")))
        return response

    # Convenience wrappers; every field of the control port is also reachable through call().
    def status(self):
        return self.call("status")

    def stats(self, reset=False):
        return self.call("stats", reset=reset)

    def mark(self, label):
        return self.call("mark", label=label)

    def head(self, pos=None, ypr=None):
        fields = {}
        if pos is not None:
            fields["pos"] = list(pos)
        if ypr is not None:
            fields["ypr"] = list(ypr)
        return self.call("head", **fields)

    def motion(self, **fields):
        return self.call("motion", **fields)

    def press(self, hand, button, ms=100, delay_ms=0, value=1.0):
        return self.call("press", hand=hand, button=button, ms=ms, delay_ms=delay_ms, value=value)

    def controller(self, hand, **fields):
        return self.call("controller", hand=hand, **fields)

    def session(self, **flags):
        return self.call("session", **flags)

    def capture(self, wait=True, timeout=60.0, **fields):
        started = self.call("capture", **fields)
        if not wait:
            return started
        return self.wait_capture(timeout)

    def wait_capture(self, timeout=60.0):
        deadline = time.time() + timeout
        while True:
            status = self.call("capture_status")
            if status.get("done"):
                return status
            if time.time() > deadline:
                raise SimXRError("capture not finished after %.0f s: %s" % (timeout, status))
            time.sleep(0.05)


# ----------------------------------------------------------------------------------- analysis

def _read_runs(path):
    """CSV rows split into runs: the headset appends a header each time it is loaded."""
    runs = []
    if not os.path.exists(path):
        return runs
    with open(path, newline="") as f:
        header = None
        for row in csv.reader(f):
            if not row:
                continue
            if row[0] in ("vsync", "seq", "t_ms"):
                header = row
                runs.append([])
                continue
            if header is None or len(row) != len(header):
                continue  # torn last line of a live file
            runs[-1].append(dict(zip(header, row)))
    return runs


def _pct(values, p):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(p * (len(ordered) - 1) + 0.5))]


def _summary(values, digits=3):
    if not values:
        return None
    return {"n": len(values), "mean": round(statistics.fmean(values), digits), "p50": round(_pct(values, 0.5), digits),
            "p95": round(_pct(values, 0.95), digits), "p99": round(_pct(values, 0.99), digits),
            "max": round(max(values), digits)}


def read_markers(log_dir):
    # Every headset instance appends its own header; a label marked again later wins (mark_time).
    runs = _read_runs(os.path.join(log_dir, "simxr-markers.csv"))
    return [dict(t_ms=float(r["t_ms"]), vsync=int(r["vsync"]), label=r["label"]) for run in runs for r in run]


def analyze(log_dir, start_ms=None, end_ms=None, from_mark=None, to_mark=None, run=None, skip_start_ms=500.0):
    """Display-model summary of one headset run inside [start, end): the run given (index among
    the runs with rows), else the latest run that contains the start time, else the longest run.

    Repeats are refreshes that showed an already-shown frame; a "hitch" is a repeat streak of two
    or more refreshes in a row (the eye sees one image for >= 3 refreshes). Spikes are submit
    intervals longer than 1.5x the median interval. The first skip_start_ms of the run are ignored
    unless start_ms or a mark is given (startup is not steady state).

    Pose age: how long before its refresh the head was where a new frame was rendered for (0 when
    the pose prediction is exact; frame generation adds its delay). Stale frames: the OFXR DLSS-G
    layer keeps submitting while the application stalls, with the last real frame's render pose,
    so the headset gets a "new" frame every refresh and the eye a frozen image. A run of new frames
    with one render pose while the head moves is stale once a frame in it is older than the
    window's median pose age + 2.5 refreshes (the frames already in flight when a head turn starts
    keep the old pose too, but never get older than the pipeline's usual delay); content_* figures
    count stale frames as repeats. With a still head a stale frame cannot be told from a fresh one
    (the agent's frame log shows those stalls).
    """
    vsync_runs = _read_runs(os.path.join(log_dir, "simxr-vsync.csv"))
    frame_runs = _read_runs(os.path.join(log_dir, "simxr-frames.csv"))
    # One run per headset instance (VDXR makes a new one per OpenXR session; VaM's start makes
    # several): the two files get a header each at the same moment, so runs pair up by index.
    filled = [i for i, rows in enumerate(vsync_runs) if rows]
    if not filled:
        raise SimXRError("no simxr-vsync.csv rows in %s" % log_dir)
    markers = read_markers(log_dir)

    def mark_time(label):
        for m in reversed(markers):
            if m["label"] == label:
                return m["t_ms"]
        raise SimXRError("no mark %r (marks: %s)" % (label, [m["label"] for m in markers]))

    if from_mark:
        start_ms = mark_time(from_mark)
    if to_mark:
        end_ms = mark_time(to_mark)
    if run is not None:
        index = filled[run]
    else:
        # VaM opens and closes several sessions at start and one more as it quits; the one a
        # test measured is the latest that spans its start mark.
        covering = [i for i in filled if start_ms is not None and
                    float(vsync_runs[i][0]["t_ms"]) <= start_ms <= float(vsync_runs[i][-1]["t_ms"])]
        index = covering[-1] if covering else max(filled, key=lambda i: (len(vsync_runs[i]), i))
    vsyncs = vsync_runs[index]
    frames = frame_runs[index] if len(frame_runs) == len(vsync_runs) else (frame_runs[-1] if frame_runs else [])
    first_shown = next((float(v["t_ms"]) for v in vsyncs if int(v["seq"]) > 0), None)
    if start_ms is None:
        start_ms = (first_shown or 0.0) + skip_start_ms
    if end_ms is None:
        # Up to the last submitted frame: the refreshes after the application stopped are not pacing.
        end_ms = float(frames[-1]["submit_ms"]) if frames else float("inf")

    window = [v for v in vsyncs if start_ms <= float(v["t_ms"]) < end_ms]
    shown = [v for v in window if int(v["seq"]) > 0]
    new = [v for v in shown if v["new"] == "1"]
    repeats = [v for v in shown if v["new"] != "1"]
    streaks = []
    streak = 0
    for v in shown:
        if v["new"] == "1":
            if streak:
                streaks.append(streak)
            streak = 0
        else:
            streak += 1
    if streak:
        streaks.append(streak)
    streak_hist = {}
    for s in streaks:
        streak_hist[s] = streak_hist.get(s, 0) + 1
    hitches = [v for v in shown if int(v["repeat_streak"]) == 2]  # second repeat in a row = start of a hitch

    # What the eye sees: a refresh holds the previous image if it is a repeat or a stale frame.
    pose_keys = ("pos_x", "pos_y", "pos_z", "yaw", "pitch", "roll")
    frame_pose = {int(f["seq"]): tuple(f[k] for k in pose_keys) for f in frames}
    vsync_pos = {int(v["vsync"]): i for i, v in enumerate(vsyncs)}
    head_track = [(float(v["t_ms"]), float(v["head_yaw"]), float(v["head_pitch"])) for v in vsyncs]

    def pose_age(v):
        """Display time minus the latest time the head was within 0.02 deg of the frame's render
        pose (head track interpolated between refreshes; searched back up to 5 s)."""
        pose = frame_pose.get(int(v["seq"]))
        k = vsync_pos.get(int(v["vsync"]))
        if pose is None or k is None:
            return None
        return age_at(float(pose[3]), float(pose[4]), k, float(v["t_ms"]))

    def age_at(ry, rp, k, t_ms):
        """t_ms minus the latest time the head was at yaw ry / pitch rp, searching from refresh k."""
        # Back from refresh k, then up to two refreshes ahead (a pose predicted past it: negative age).
        for j in list(range(min(k, len(head_track) - 1), max(0, k - 600), -1)) + [k + 1, k + 2]:
            if not 0 < j < len(head_track):
                continue
            (t0, y0, p0), (t1, y1, p1) = head_track[j - 1], head_track[j]
            dy, dp = (y1 - y0 + 180.0) % 360.0 - 180.0, p1 - p0
            ey, ep = (ry - y0 + 180.0) % 360.0 - 180.0, rp - p0
            length = dy * dy + dp * dp
            u = min(1.0, max(0.0, (ey * dy + ep * dp) / length)) if length else 1.0  # still: the latest moment
            if (ey - u * dy) ** 2 + (ep - u * dp) ** 2 < 0.02 ** 2:
                return t_ms - (t0 + u * (t1 - t0))
        return None

    ages_pose = {}
    for v in new:
        age = pose_age(v)
        if age is not None:
            ages_pose[int(v["vsync"])] = age
    vsync_period = _pct([float(b["t_ms"]) - float(a["t_ms"]) for a, b in zip(vsyncs, vsyncs[1:])], 0.5) or 0.0
    stale_after = (_pct(list(ages_pose.values()), 0.5) or 0.0) + 2.5 * vsync_period
    # Runs of new frames sharing a render pose while the head moves (the first of each is fresh).
    is_stale = set()
    run = []

    def close_run():
        if len(run) > 1 and any(ages_pose.get(int(r["vsync"]), 0.0) > stale_after for r in run):
            is_stale.update(int(r["vsync"]) for r in run[1:])

    previous_head = None
    for v in new:
        pose = frame_pose.get(int(v["seq"]))
        head = (float(v["head_yaw"]), float(v["head_pitch"]))
        if run and pose == frame_pose.get(int(run[0]["seq"])) and previous_head != head:
            run.append(v)
        else:
            close_run()
            run = [v]
        previous_head = head
    close_run()
    held = [(float(v["t_ms"]), v["new"] != "1" or int(v["vsync"]) in is_stale) for v in shown]
    stale = len(is_stale)
    content_holds = []  # (first held refresh t_ms, refreshes the image stayed up)
    run_start, run_len = None, 0
    for t, h in held + [(None, False)]:
        if h:
            run_start = t if run_len == 0 else run_start
            run_len += 1
        elif run_len:
            content_holds.append((run_start, run_len + 1))
            run_len = 0

    window_frames = [f for f in frames if start_ms <= float(f["submit_ms"]) < end_ms]
    intervals = [float(b["submit_ms"]) - float(a["submit_ms"]) for a, b in zip(window_frames, window_frames[1:])]
    median_interval = _pct(intervals, 0.5) or 0.0

    # The pipeline's own output, as submitted to the runtime: independent of SimXR's display model
    # (which frames a refresh shows depends on how SimXR paces and picks them; what VaM, DLSS SR
    # and DLSS-G hand over does not). A gap: no frame for 4+ refreshes. A frozen pose: 3+ frames in
    # a row with one render pose while the head moves (DLSS-G resubmitting the last real frame while
    # VaM stalls). Intended pose age: how far behind the head a frame's content is at the display
    # time it was rendered for (frame generation's delay), not at the refresh SimXR showed it on.
    head_times = [t for t, _, _ in head_track]
    gaps = [(float(b["submit_ms"]), round(float(b["submit_ms"]) - float(a["submit_ms"]), 1))
            for a, b in zip(window_frames, window_frames[1:])
            if vsync_period and float(b["submit_ms"]) - float(a["submit_ms"]) > 3.5 * vsync_period]

    def head_moving(t_ms):
        k = bisect.bisect_right(head_times, t_ms) - 1
        return 0 < k < len(head_track) and head_track[k][1:] != head_track[k - 1][1:]

    # Intended pose age of each frame rendered for a moment the head was moving (a still head
    # gives 0 whatever the delay).
    intended_age = {}
    for f in window_frames:
        sensor = float(f["sensor_ms"])
        if sensor > 0 and head_moving(sensor):
            age = age_at(float(f["yaw"]), float(f["pitch"]), max(0, bisect.bisect_right(head_times, sensor) - 1), sensor)
            if age is not None:
                intended_age[f["seq"]] = age
    intended_ages = list(intended_age.values())
    # The frames already in flight when a turn starts keep the old pose too, but are never older
    # than the pipeline's usual delay; a stall's frames are.
    frozen_after = (_pct(intended_ages, 0.5) or 0.0) + 2.5 * vsync_period
    frozen = []
    same = [window_frames[0]] if window_frames else []
    for f in window_frames[1:] + [None]:
        if f is not None and tuple(f[k] for k in pose_keys) == tuple(same[0][k] for k in pose_keys):
            same.append(f)
            continue
        if len(same) >= 3 and any(intended_age.get(g["seq"], 0.0) > frozen_after for g in same):
            frozen.append((float(same[0]["submit_ms"]), len(same),
                           round(float(same[-1]["submit_ms"]) - float(same[0]["submit_ms"]), 1)))
        same = [f] if f is not None else []
    spikes = [(float(b["submit_ms"]), round(float(b["submit_ms"]) - float(a["submit_ms"]), 3))
              for a, b in zip(window_frames, window_frames[1:])
              if median_interval and float(b["submit_ms"]) - float(a["submit_ms"]) > 1.5 * median_interval]
    vsync_dt = [float(b["t_ms"]) - float(a["t_ms"]) for a, b in zip(window, window[1:])]
    period = _pct(vsync_dt, 0.5) or 0.0
    duration_s = len(window) * period / 1000.0  # one refresh period per vsync row
    pose_errors = [float(v["pose_err_deg"]) for v in shown if float(v["pose_err_deg"]) >= 0]
    pose_errors_new = [float(v["pose_err_deg"]) for v in new if float(v["pose_err_deg"]) >= 0]
    ages = [float(v["age_ms"]) for v in new if float(v["age_ms"]) >= 0]
    simulator_late = sum(1 for d in vsync_dt if period and d > 1.5 * period)
    dropped = sum(1 for f in window_frames if f["dropped"] == "1")
    # Shown on the refresh the application rendered it for (its xrEndFrame displayTime)?
    vsync_time = {int(v["vsync"]): float(v["t_ms"]) for v in vsyncs}
    offsets = {}
    early_reused = 0
    reused = 0
    previous_index = None
    for f in window_frames:
        # VDXR's submission thread occasionally submits a frame under the previous frame's OVR index
        # (it reads m_frameCompleted before the application thread advances it): the earlier frame
        # is then replaced before any refresh shows it and the later one goes out a refresh early.
        is_reused = f["frame_index"] == previous_index
        previous_index = f["frame_index"]
        reused += is_reused
        first = int(f["first_vsync"])
        sensor = float(f["sensor_ms"])
        if first > 0 and first in vsync_time and sensor > 0 and period:
            offset = int(round((vsync_time[first] - sensor) / period))
            offsets[offset] = offsets.get(offset, 0) + 1
            early_reused += offset < 0 and is_reused

    report = {
        "log_dir": log_dir,
        "window_ms": [round(start_ms, 3), None if end_ms == float("inf") else round(end_ms, 3)],
        "duration_s": round(duration_s, 3),
        "display_hz": round(1000.0 / period, 3) if period else None,
        "vsyncs": len(window),
        "blank_vsyncs": len(window) - len(shown),
        "new_frames": len(new),
        "displayed_fps": round(len(new) / duration_s, 3) if duration_s else None,
        "submitted": len(window_frames),
        "submitted_fps": round(len(window_frames) / duration_s, 3) if duration_s else None,
        "repeats": len(repeats),
        "repeat_ratio": round(len(repeats) / len(shown), 4) if shown else None,
        "repeat_streaks": dict(sorted(streak_hist.items())),
        "hitches": len(hitches),
        "hitch_times_ms": [round(float(v["t_ms"]), 1) for v in hitches[:50]],
        "pose_age_ms": _summary(list(ages_pose.values())),
        "stale_after_ms": round(stale_after, 1),
        "stale_frames": stale,
        "content_repeat_ratio": round(sum(1 for _, h in held if h) / len(shown), 4) if shown else None,
        "content_hitches": sum(1 for _, n in content_holds if n >= 3),
        "content_hitch_list": [(round(t, 1), round(n * period, 1)) for t, n in content_holds if n >= 3][:50],
        "longest_image_ms": round(max([n for _, n in content_holds] + [1]) * period, 1),
        "dropped_frames": dropped,
        "shown_vs_intended_refresh": dict(sorted(offsets.items())),
        "late_frames": sum(n for o, n in offsets.items() if o > 0),
        "early_frames": sum(n for o, n in offsets.items() if o < 0),
        "early_frames_reused_index": early_reused,
        "reused_frame_index": reused,
        "gpu_late_vsyncs": sum(1 for v in window if v["gpu_late"] == "1"),
        "simulator_late_vsyncs": simulator_late,
        "submit_interval_ms": _summary(intervals),
        "submit_spikes": len(spikes),
        "submit_spike_list": spikes[:50],
        "pipeline_gaps": len(gaps),
        "pipeline_gap_list": gaps[:50],
        "frozen_pose_runs": len(frozen),
        "frozen_pose_list": frozen[:50],
        "pose_age_intended_ms": _summary(intended_ages),
        "age_at_display_ms": _summary(ages),
        "pose_error_deg": _summary(pose_errors, 4),
        "pose_error_new_frames_deg": _summary(pose_errors_new, 4),
        "markers": markers,
    }
    return report


def format_report(report):
    def stat(name, unit=""):
        s = report.get(name)
        if not s:
            return "%s: -" % name
        return "%s: p50 %s  p95 %s  p99 %s  max %s%s (n=%d)" % (name, s["p50"], s["p95"], s["p99"], s["max"], unit, s["n"])

    lines = [
        "window %s ms, %.2f s, display %s Hz" % (report["window_ms"], report["duration_s"], report["display_hz"]),
        "displayed %s fps (%d new frames), submitted %s fps, repeats %d (%.1f%%), hitches %d, dropped %d, "
        "gpu late %d, simulator late %d" % (
            report["displayed_fps"], report["new_frames"], report["submitted_fps"], report["repeats"],
            100.0 * (report["repeat_ratio"] or 0.0), report["hitches"], report["dropped_frames"],
            report["gpu_late_vsyncs"], report["simulator_late_vsyncs"]),
        "repeat streaks (length: count) %s" % report["repeat_streaks"],
        "shown vs intended refresh (refreshes late: frames) %s, reused OVR frame index %d (early among them %d)" % (
            report["shown_vs_intended_refresh"], report["reused_frame_index"], report["early_frames_reused_index"]),
        stat("submit_interval_ms"),
        "submit spikes (> 1.5x median) %d %s" % (report["submit_spikes"], report["submit_spike_list"][:10]),
        stat("age_at_display_ms"),
        stat("pose_error_deg"),
        stat("pose_error_new_frames_deg"),
        stat("pose_age_ms"),
    ]
    lines.append("pipeline output: submitted %s fps, gaps of 4+ refreshes %d %s, frozen-pose runs %d %s" % (
        report["submitted_fps"], report["pipeline_gaps"], report["pipeline_gap_list"][:10],
        report["frozen_pose_runs"], report["frozen_pose_list"][:10]))
    lines.append(stat("pose_age_intended_ms"))
    if report["hitches"]:
        lines.append("hitches at ms %s" % report["hitch_times_ms"][:20])
    lines.append("seen by the eye (stale frames count as repeats): stale frames %d, held %.1f%%, hitches %d, "
                 "longest image %s ms %s" % (report["stale_frames"], 100.0 * (report["content_repeat_ratio"] or 0.0),
                                             report["content_hitches"], report["longest_image_ms"],
                                             report["content_hitch_list"][:10]))
    return "\n".join(lines)


# ----------------------------------------------------------------------------------------- CLI

def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", help="ping, status, stats, help, call, analyze")
    parser.add_argument("arg", nargs="?", help="JSON request for call, folder for analyze")
    parser.add_argument("--port", type=int)
    parser.add_argument("--log-dir")
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--from-mark")
    parser.add_argument("--to-mark")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    if args.command == "analyze":
        report = analyze(args.arg or args.log_dir, from_mark=args.from_mark, to_mark=args.to_mark)
        print(json.dumps(report, indent=1) if args.json else format_report(report))
        return 0

    if args.port:
        client = Client(args.port)
    else:
        client = Client.wait(args.log_dir, port=None if args.log_dir else DEFAULT_PORT, timeout=5)
    with client:
        if args.command == "call":
            request = json.loads(args.arg)
            response = client.call(request.pop("cmd"), **request)
        elif args.command == "stats":
            response = client.stats(reset=args.reset)
        elif args.command == "help":
            print(client.call("help")["help"])
            return 0
        else:
            response = client.call(args.command)
    print(json.dumps(response, indent=1))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except SimXRError as e:
        print("simxr: %s" % e, file=sys.stderr)
        sys.exit(1)
