"""Frame pacing as the OFXR layer saw it (its flight log), for comparing SimXR runs with real
Virtual Desktop runs: the same log exists for both, so the same numbers come out of both.

    python flightlog.py LOG_DIR[@FROM-TO] [LOG_DIR[@FROM-TO] ...] [--marks still,end] [--json FILE]

LOG_DIR is a run's folder under logs\\ (its ofxr-bridge-flight-*.log). FROM-TO are seconds of
flight-log time; without them a SimXR run (simxr\\ beside the log) is measured between two
SimXR marks (--marks, default still,end: vrtest pacing's still head + head turns), any other run
over its longest steady stretch with frame generation running (2 s bins: generated frames
submitted, repeats under 20%, the application above 5 fps).

The presenter thread hands the runtime one frame per cycle: internal_wait_frame (the runtime's
xrWaitFrame), internal_begin_frame, the presenter's own pace hold, internal_end_frame. Per cycle
and per application frame this prints the duration distributions, and what they say about the
runtime underneath:

  release phase    predicted display time minus the moment xrWaitFrame returned, minus its median:
                   how steadily the runtime releases frames against its own display clock.
  late releases    of the cycles where the presenter waited inside xrEndFrame (VDXR's asynchronous
                   submission thread had not yet got past ovr_WaitToBeginFrame), the share whose
                   release came > 1.5 ms later than usual against the next predicted display time.
  GPU-gated        long xrEndFrame waits (> 2 ms) that ended within 0.25 ms of a hand-over's GPU
                   work completing, against the same test at random times: a runtime that holds
                   frames for the GPU (SimXR's queue limit) shows up here.
  in flight        hand-overs whose GPU work was still running when xrWaitFrame returned.
  GPU done         when a fresh hand-over's GPU work completed, after its cycle's xrWaitFrame return.
  displayed est.   each fresh hand-over shown at the first refresh after both its GPU work completed
                   and 1 ms after its submission, the newest one winning: SimXR's display model,
                   applied to the layer's own GPU probes. Real runs have no refresh phase, so every
                   phase is tried (min/mean/max); SimXR runs also give the estimate at their true
                   phase and SimXR's own count (they agree within ~2 fps).

The GPU timing comes from the layer's join probes (presenter_vsync_lock 916: a fence signalled
on the runtime's queue at each hand-over; 914: when it completed), written with logging on.
"""
import argparse
import bisect
import collections
import json
import math
import os
import random
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simxr import _read_runs, read_markers  # noqa: E402

REFRESH_MS = 1000.0 / 120.0
LATCH_MS = 1.0
LINE = re.compile(r"seq=(\d+) ms=([\d.]+) tid=(\d+) phase=(\w) op=(\w+) result=(-?\d+) dur_us=(\d+) "
                  r"a=(\d+) b=(\d+) c=(\d+)")
SEQ, MS, TID, PHASE, OP, RESULT, DUR, A, B, C = range(10)


def pct(values, ps=(10, 50, 90, 99)):
    if not values:
        return None
    ordered = sorted(values)
    return [round(ordered[min(len(ordered) - 1, max(0, int(round(p / 100.0 * (len(ordered) - 1)))))], 2) for p in ps]


def read_flight(log_dir):
    logs = sorted(f for f in os.listdir(log_dir) if f.startswith("ofxr-bridge-flight-") and f.endswith(".log"))
    if not logs:
        raise SystemExit("no ofxr-bridge-flight-*.log in %s" % log_dir)
    records = []
    with open(os.path.join(log_dir, logs[-1]), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if m:
                g = m.groups()
                records.append((int(g[0]), float(g[1]), int(g[2]), g[3], g[4], int(g[5]), int(g[6]),
                                int(g[7]), int(g[8]), int(g[9])))
    return session_records(records)


def session_records(records):
    """The logger keeps a file's first records and a ring of the latest: start after the jump in
    sequence numbers; then the longest stretch between session boundaries (VaM opens and closes a
    first session at start-up)."""
    start = 0
    for i in range(1, len(records)):
        if records[i][SEQ] - records[i - 1][SEQ] > 1000:
            start = i
    records = records[start:]
    cuts = [i for i, r in enumerate(records)
            if r[OP] in ("session_begin", "session_end", "session_destroy") and r[PHASE] in ("B", "I")]
    bounds = [0] + cuts + [len(records)]
    best = max(zip(bounds, bounds[1:]),
               key=lambda ab: sum(1 for r in records[ab[0]:ab[1]] if r[OP] == "internal_wait_frame"))
    return records[best[0]:best[1]]


def cycles(records):
    """One dict per presenter cycle: wait/begin/pace/end durations (ms), wait and end return
    times, the predicted display time xrWaitFrame gave (ns), the next cycle's start."""
    out = []
    cur = None
    for r in records:
        op, phase = r[OP], r[PHASE]
        if op == "internal_wait_frame" and phase == "B":
            if cur and "end" in cur:
                cur["next_wait_begin"] = r[MS]
                out.append(cur)
            cur = {"wait_begin": r[MS]}
        elif cur is None:
            continue
        elif op == "internal_wait_frame" and phase == "E":
            cur.update(wait=r[DUR] / 1000.0, wait_ret=r[MS], pdt=r[A])
        elif op == "internal_begin_frame" and phase == "E":
            cur["begin"] = r[DUR] / 1000.0
        elif op == "presenter_pace" and phase == "I" and r[RESULT] in (0, 1, 2):
            cur["pace"] = r[A] / 1000.0
        elif op == "internal_end_frame" and phase == "E":
            cur.update(end=r[DUR] / 1000.0, end_ret=r[MS])
    return [c for c in out if "wait" in c and "end" in c]


def summarize(records, t0, t1):
    durations = collections.defaultdict(list)
    begins = collections.defaultdict(list)
    kinds = collections.Counter()
    for r in records:
        if not t0 <= r[MS] < t1:
            continue
        if r[PHASE] == "E":
            durations[r[OP]].append(r[DUR] / 1000.0)
        elif r[PHASE] == "B":
            begins[r[OP]].append(r[MS])
        elif r[OP] == "presenter_submission":
            kinds[{0: "repeat", 1: "real", 2: "generated"}.get(r[C], "other")] += 1
    return durations, begins, kinds


def steady_window(records, bin_s=2.0, min_bins=4):
    if not records:
        return None
    t, end = records[0][MS], records[-1][MS]
    good = []
    while t + bin_s * 1000 <= end:
        _, begins, kinds = summarize(records, t, t + bin_s * 1000)
        app = begins.get("app_wait_frame", [])
        ok = (kinds["generated"] > 0 and kinds["repeat"] <= 0.2 * sum(kinds.values())
              and len(app) / bin_s > 5)
        good.append((t, ok))
        t += bin_s * 1000
    best = cur = None
    for t, ok in good:
        if ok:
            cur = (cur[0], t + bin_s * 1000) if cur else (t, t + bin_s * 1000)
            if not best or cur[1] - cur[0] > best[1] - best[0]:
                best = cur
        else:
            cur = None
    return best if best and best[1] - best[0] >= min_bins * bin_s * 1000 else None


def sim_offset(log_dir, records):
    """SimXR time (its CSVs) minus flight-log time: every xrEndFrame return is followed within
    ~0.1 ms by SimXR receiving the frame from VDXR's submission thread."""
    runs = _read_runs(os.path.join(log_dir, "simxr", "simxr-frames.csv"))
    submits = sorted(float(r["submit_ms"]) for run in runs for r in run)
    ends = [c["end_ret"] for c in cycles(records)]
    if not submits or not ends:
        return None
    ends = ends[len(ends) // 4: len(ends) // 4 + 400]

    def score(off):
        n = 0
        for t in ends[::4]:
            i = bisect.bisect_left(submits, t + off - 0.2)
            if i < len(submits) and submits[i] <= t + off + 1.5:
                n += 1
        return n
    # The two clocks start within seconds of each other (headset load vs. layer load).
    coarse = max((score(o / 10.0), o / 10.0) for o in range(-300000, 300000, 50))
    fine = max((score(coarse[1] + o / 100.0), coarse[1] + o / 100.0) for o in range(-100, 101))
    return fine[1] if fine[0] >= 0.8 * len(ends[::4]) else None


def measure(log_dir, window=None, marks=("still", "end")):
    records = read_flight(log_dir)
    sim_dir = os.path.join(log_dir, "simxr")
    is_sim = os.path.exists(os.path.join(sim_dir, "simxr-frames.csv"))
    offset = sim_offset(log_dir, records) if is_sim else None
    source = "given"
    if window is None and is_sim and offset is not None:
        found = {m["label"]: m["t_ms"] for m in read_markers(sim_dir)}
        if marks[0] in found and marks[1] in found:
            window = (found[marks[0]] - offset, found[marks[1]] - offset)
            source = "SimXR marks %s-%s" % marks
    if window is None:
        window = steady_window(records)
        source = "steady frame generation"
    if window is None:
        raise SystemExit("%s: no window to measure (give FROM-TO seconds)" % log_dir)
    t0, t1 = window
    seconds = (t1 - t0) / 1000.0
    out = {"log_dir": log_dir, "simxr": is_sim, "window_s": [round(t0 / 1000, 1), round(t1 / 1000, 1)],
           "window_source": source}

    durations, begins, kinds = summarize(records, t0, t1)
    for op in ("internal_wait_frame", "internal_begin_frame", "internal_end_frame", "app_wait_frame",
               "app_end_frame"):
        out[op] = pct(durations.get(op, []))
    cs = [c for c in cycles(records) if t0 <= c["wait_ret"] < t1]
    out["presenter_pace"] = pct([c.get("pace", 0.0) for c in cs])
    out["presenter_cycle"] = pct([c["next_wait_begin"] - c["wait_begin"] for c in cs])
    out["presenter_fps"] = round(len(cs) / seconds, 1)
    app = begins.get("app_wait_frame", [])
    out["app_fps"] = round(len(app) / seconds, 1)
    out["app_frame_interval"] = pct([b - a for a, b in zip(app, app[1:])])
    out["submissions"] = dict(kinds)
    slots = collections.Counter(round((b["pdt"] - a["pdt"]) / 1e6 / REFRESH_MS) for a, b in zip(cs, cs[1:]))
    out["display_advance_slots_pct"] = {k: round(100.0 * v / max(1, sum(slots.values())), 1)
                                        for k, v in sorted(slots.items())}

    photon = [c["pdt"] / 1e6 - c["wait_ret"] for c in cs]
    median = sorted(photon)[len(photon) // 2] if photon else 0.0
    out["release_phase_p1_10_90_99"] = pct([p - median for p in photon], (1, 10, 90, 99))
    held = [n["pdt"] / 1e6 - c["end_ret"] for c, n in zip(cs, cs[1:]) if c["end"] > 0.3]
    if held:
        held_median = sorted(held)[len(held) // 2]
        out["late_releases_pct"] = round(100.0 * sum(1 for h in held if h - held_median < -1.5) / len(held), 1)

    done_times = sorted(r[MS] for r in records if r[OP] == "presenter_vsync_lock" and r[RESULT] == 914
                        and t0 - 100 <= r[MS] < t1 + 100)

    def since_done(t):
        i = bisect.bisect_right(done_times, t) - 1
        return t - done_times[i] if i >= 0 else 1e9
    long_ends = [c for c in cs if c["end"] > 2.0]
    if long_ends and done_times:
        rng = random.Random(1)
        baseline = [since_done(rng.uniform(t0, t1)) for _ in range(2000)]
        out["gpu_gated_end_waits_pct"] = round(100.0 * sum(1 for c in long_ends if since_done(c["end_ret"]) < 0.25)
                                               / len(long_ends), 1)
        out["gpu_gated_random_pct"] = round(100.0 * sum(1 for x in baseline if x < 0.25) / len(baseline), 1)

    # The GPU probes: 916 at each hand-over (b = submission, c = 2 synthetic / 1 real / 0 repeat), 914 done.
    signalled = []
    done = {}
    for r in records:
        if r[OP] == "presenter_vsync_lock" and r[RESULT] == 916:
            signalled.append((r[MS], r[A], r[C]))
        elif r[OP] == "presenter_vsync_lock" and r[RESULT] == 914:
            done.setdefault(r[A], r[MS])
    signal_times = [s[0] for s in signalled]
    if signalled and done:
        def in_flight(t):
            i = bisect.bisect_left(signal_times, t)
            return sum(1 for ms, v, _ in signalled[max(0, i - 12):i] if done.get(v, 1e18) > t)
        counts = collections.Counter(min(2, in_flight(c["wait_ret"])) for c in cs)
        out["in_flight_at_release_pct"] = {("2+" if k == 2 else str(k)): round(100.0 * v / max(1, len(cs)), 1)
                                           for k, v in sorted(counts.items())}
        end_returns = sorted(c["end_ret"] for c in cs)
        wait_returns = sorted(c["wait_ret"] for c in cs)
        after_release = []
        ready = []
        for ms, value, kind in signalled:
            if not t0 <= ms < t1 or kind == 0 or value not in done:
                continue
            i = bisect.bisect_right(wait_returns, ms) - 1
            if i >= 0:
                after_release.append(done[value] - wait_returns[i])
            j = bisect.bisect_left(end_returns, ms)  # this hand-over's xrEndFrame return = its submission
            submitted = end_returns[j] if j < len(end_returns) else ms
            ready.append(max(done[value], submitted + LATCH_MS))
        out["gpu_done_after_release"] = pct(after_release)
        out["fresh_fps"] = round(len(ready) / seconds, 1)
        by_phase = [len({math.ceil((t - k * 0.25) / REFRESH_MS) for t in ready}) / seconds
                    for k in range(int(REFRESH_MS / 0.25) + 1)]
        out["displayed_estimate_min_mean_max"] = [round(min(by_phase), 1), round(sum(by_phase) / len(by_phase), 1),
                                                  round(max(by_phase), 1)]
        if is_sim and offset is not None:
            vsyncs = [(float(r["t_ms"]) - offset, int(r["new"]))
                      for run in _read_runs(os.path.join(sim_dir, "simxr-vsync.csv")) for r in run]
            inside = [v for v in vsyncs if t0 <= v[0] < t1]
            if inside:
                phase = inside[0][0] % REFRESH_MS
                out["displayed_estimate_true_phase"] = round(
                    len({math.ceil((t - phase) / REFRESH_MS) for t in ready}) / seconds, 1)
                out["simxr_displayed_fps"] = round(sum(n for _, n in inside) / seconds, 1)
    return out


ROWS = [
    ("internal_wait_frame", "runtime xrWaitFrame ms p10/50/90/99"),
    ("internal_end_frame", "runtime xrEndFrame ms"),
    ("presenter_pace", "presenter pace hold ms"),
    ("presenter_cycle", "presenter cycle ms"),
    ("presenter_fps", "presenter fps"),
    ("display_advance_slots_pct", "display advance, refreshes %"),
    ("release_phase_p1_10_90_99", "release phase ms p1/10/90/99"),
    ("late_releases_pct", "late releases %"),
    ("gpu_gated_end_waits_pct", "xrEndFrame waits ending at GPU done %"),
    ("gpu_gated_random_pct", "  (random times) %"),
    ("in_flight_at_release_pct", "in flight at release %"),
    ("gpu_done_after_release", "GPU done after release ms"),
    ("app_fps", "application fps"),
    ("app_frame_interval", "application frame ms"),
    ("app_end_frame", "application xrEndFrame ms"),
    ("fresh_fps", "fresh hand-overs /s"),
    ("displayed_estimate_min_mean_max", "displayed estimate min/mean/max"),
    ("displayed_estimate_true_phase", "displayed estimate at SimXR phase"),
    ("simxr_displayed_fps", "SimXR displayed fps"),
]


def format_table(results):
    def cell(v):
        if v is None:
            return "-"
        if isinstance(v, dict):
            return " ".join("%s:%s" % kv for kv in v.items())
        if isinstance(v, list):
            return "/".join("%g" % x for x in v)
        return "%g" % v if isinstance(v, (int, float)) else str(v)
    names = [os.path.basename(os.path.normpath(r["log_dir"])) for r in results]
    lines = ["%-40s %s" % ("", "  |  ".join(names))]
    lines.append("%-40s %s" % ("window s", "  |  ".join("%s-%s (%s)" % (r["window_s"][0], r["window_s"][1],
                                                                       r["window_source"]) for r in results)))
    for key, label in ROWS:
        lines.append("%-40s %s" % (label, "  |  ".join(cell(r.get(key)) for r in results)))
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", help="LOG_DIR or LOG_DIR@FROM-TO (seconds of flight-log time)")
    parser.add_argument("--marks", default="still,end", help="SimXR runs: the marks bounding the window")
    parser.add_argument("--json", help="write the measurements to this file")
    args = parser.parse_args(argv)
    results = []
    for spec in args.runs:
        path, _, span = spec.partition("@")
        window = tuple(float(x) * 1000.0 for x in span.split("-")) if span else None
        results.append(measure(os.path.abspath(path), window, tuple(args.marks.split(","))))
    print(format_table(results))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
