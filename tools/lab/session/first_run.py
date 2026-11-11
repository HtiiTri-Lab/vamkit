"""The first run after an install: does the installed build start, does the chain come up, does vanilla load nothing.

Two short desktop sessions in the background (no headset needed), from an installed VaM folder:
  vanilla   VaM's own files; BepInEx must not start and no file of the kit may be loaded into the process
  mod       the plugins load, the proxy is in the process, a scene with one person renders, DLSS super resolution and
            frame generation come up; path tracing is switched on while playing (the first time after a build the
            driver compiles the pipelines: about 20 s without frames) and ray reconstruction takes over; then off
  vr        the mod in VR on the simulated headset (../simxr-runtime; left out when it is not built): VaM's VR
            session comes up, frames reach the headset and show a picture, path tracing takes over, frame generation
            hands the headset one frame per refresh while VaM renders refresh / multiplier
Afterwards VaM's own files and the window settings must be back, and the test agent out of the plugins folder.
The scene is scenes/person.json (one default person, one light), staged in Saves/scene/_agent-test/ for the run.
What each step saw is kept as seen.json in the run folder; the logs are in <VaM>/vamkit/logs/<run>/.

    python tools/lab/session/first_run.py --vam <VaM folder> [--scene Saves/scene/x.json] [--only vanilla|mod|vr]

Three verdicts, not two. `ok` and `FAIL` are what they say. `LESS` means the run works and the user gets less than the
kit has: a plugin stepped aside, something else ran in place of what was asked for, a run-time file is not installed.
Each LESS line says why and how to mend it; it is never counted as passed.
Exit code 0: everything holds. 2: it runs, with less (tell the user what they are not getting). 1: a check failed.
This starts the game: one VaM at a time, and the GPU is busy while it runs
(about one minute for vanilla, two for the mod, three for VR).
"""
import argparse
import ctypes
import json
import re
import shutil
import subprocess
import sys
import time
import tomllib
import winreg
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE))
from session import SIMULATED_RUNTIME, Session, SessionError, SimXRError, processes, running_in, warnings_in  # noqa: E402

REFRESH, MULTIPLIER = 144, 4   # the VR session's simulated display and frame generation (--refresh, --multiplier)

failures = []
lessened = []   # what works, with less than the kit has


def check(ok, what, detail=""):
    print(("ok    " if ok else "FAIL  ") + what + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failures.append(what)
    return ok


def less(what, why, mend=""):
    """Not a failure and not a pass: the run goes on with less. Said with the reason and the way to mend it."""
    print("LESS  " + what + f": {why}" + (f"\n      to mend it: {mend}" if mend else ""), flush=True)
    lessened.append(what)


def missing_runtimes(vam):
    """The kit's run-time files that are not in this VaM folder: [(name, what the user does not get, how to add it)]."""
    done = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "vamkit"), "verify", "--vam", str(vam), "--json"], capture_output=True, text=True)
    found = []
    try:
        for c in json.loads(done.stdout):
            if c.get("name", "").startswith("run-time file ") and not c.get("ok"):
                found.append((c["name"][len("run-time file "):], c.get("detail", "")))
    except ValueError:
        pass
    return found


def not_whole(vam, bepinex, where):
    """Run-time files that are not installed and plugins that stepped aside: LESS, each with its reason."""
    for name, detail in missing_runtimes(vam):
        less(f"{where}: {name} is not installed", detail)
    warned = warnings_in(bepinex)
    if warned:
        less(f"{where}: {len(warned)} warning(s) from the plugins: something stepped aside or is not whole",
             "\n      " + "\n      ".join(warned[:8]), "read each line: a plugin names what it left to VaM and why (docs/agent/adapt-vam-version.md)")


def stand_ins(state, where):
    """What the kit itself says ran against what was asked (running_in): a stand-in is LESS, with the kit's own reason.
    Returns the parts it could judge, so that the caller reads the logs only for the others."""
    judged = set()
    part = (state.get("compositor") or {}).get("upscaler")
    if part and part.get("wanted"):
        judged.add("upscaler")
        if part.get("wanted") == part.get("running"):
            check(True, f"{where}: the upscaler asked for ({part['wanted']}) runs")
        else:
            less(f"{where}: asked for the upscaler {part['wanted']}, {part.get('running')} runs in its place", part.get("reason") or "no reason given",
                 "python tools/vamkit verify names a run-time file that is missing and how to add it")
    part = (state.get("compositor") or {}).get("path_tracing")
    if part and part.get("wanted") and not part.get("can"):
        less(f"{where}: path tracing was asked for and cannot run", part.get("reason") or "no reason given")
    part = (state.get("generation") or {}).get("frame_generation")
    if part and part.get("wanted"):
        judged.add("generation")
        if part.get("running") in (None, "", "none"):
            check(False, f"{where}: frame generation generates", f"asked for {part['wanted']}, nothing generates: {part.get('reason') or 'no reason given'}")
        elif part.get("wanted") == part.get("running"):
            check(True, f"{where}: the frame generation asked for ({part['wanted']}) generates", f"up to {part.get('frames_most')} frames shown per real frame")
        else:
            less(f"{where}: asked for {part['wanted']} frame generation, {part['running']} generates in its place (up to {part.get('frames_most')} frames per real frame)",
                 part.get("reason") or "no reason given", "python tools/vamkit verify names a run-time file that is missing and how to add it")
    return judged


def what_ran(vam, session, bepinex, compositor, generation, where, state=None):
    """What the user asked for against what ran: a stand-in in place of the upscaler or the frame generation asked
    for is LESS. From the kit's own status files when the run wrote them, from the logs otherwise."""
    not_whole(vam, bepinex, where)
    judged = stand_ins(state or {}, where)
    if "upscaler" not in judged:
        upscaler_from_log(session, compositor, where)
    if "generation" not in judged:
        generation_from_log(generation, where)


def upscaler_from_log(session, compositor, where):
    asked = str((session.settings or {}).get("dlss_preset", ""))
    ran = [name for name, mark in (("DLSS", "DLSS SR feature created"), ("XeSS", "XeSS feature created"), ("FSR", "FSR feature created")) if mark in compositor]
    wanted = "DLSS" if asked in ("K", "L", "M") else asked
    if not ran:
        check(asked == "Off", f"{where}: an upscaler was created (path tracing off)", f"asked for {asked}, none was created: see opencomposite.log")
    elif wanted in ran:
        check(True, f"{where}: the upscaler asked for ({asked}) was created", ("also created: " + ", ".join(r for r in ran if r != wanted)) if len(ran) > 1 else "")
    else:
        less(f"{where}: asked for the upscaler {asked}, {' and '.join(ran)} ran in its place",
             "the one asked for could not run here (opencomposite.log says why)", "python tools/vamkit verify lists run-time files that are missing")


def generation_from_log(generation, where):
    dlssg = bool(re.search(r"DLSS-G\[\d+\] first evaluation recorded", generation))
    other = bool(re.search(r"FSR-FG\[\d+\] first pair recorded", generation))
    if dlssg:
        check(True, f"{where}: DLSS frame generation generated frames")
    elif other:
        less(f"{where}: frame generation ran on FSR (2x), not on DLSS",
             "DLSS Frame Generation did not generate (dlssg.log says why): on an RTX 40 or 50 that costs the higher multipliers",
             "nvngx_dlssg.dll comes with a current NVIDIA driver once a DLSS Frame Generation title has run: python tools/vamkit fetch dlss-fg, then install")
    else:
        check(False, f"{where}: frame generation generated frames", "neither DLSS nor FSR generated: see dlssg/dlssg.log")


def modules(pid):
    """Paths of the modules loaded into a process."""
    psapi, kernel = ctypes.WinDLL("psapi"), ctypes.WinDLL("kernel32")
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    psapi.EnumProcessModulesEx.argtypes = (wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD)
    psapi.GetModuleFileNameExW.argtypes = (wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD)
    handle = kernel.OpenProcess(0x0410, False, pid)   # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    if not handle:
        return []
    try:
        array, needed = (wintypes.HMODULE * 4096)(), wintypes.DWORD()
        if not psapi.EnumProcessModulesEx(handle, array, ctypes.sizeof(array), ctypes.byref(needed), 3):
            return []
        found = []
        for module in array[: needed.value // ctypes.sizeof(wintypes.HMODULE)]:
            name = ctypes.create_unicode_buffer(32768)
            if psapi.GetModuleFileNameExW(handle, module, name, 32768):
                found.append(name.value)
        return found
    finally:
        kernel.CloseHandle(handle)


def screen():
    values = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\MeshedVR\VaM") as key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                name, value, _ = winreg.EnumValue(key, i)
                if name.startswith("Screenmanager "):
                    values[name] = value
    except OSError:
        pass
    return values


def installed_plugins(vam):
    """The plugin assemblies the manifest installs: name -> the name BepInEx logs for it."""
    components = tomllib.loads((REPO / "manifest" / "components.toml").read_text(encoding="utf-8"))["component"]
    names = {}
    for c in components:
        for _, target in c.get("installs", []):
            if target.startswith("BepInEx/plugins/") and target.endswith(".dll"):
                stem = Path(target).stem
                source = next((p.read_text(encoding="utf-8", errors="replace") for p in (REPO / c["path"]).rglob(stem + ".cs")), "")
                declared = re.search(r'\[BepInPlugin\("[^"]+",\s*"([^"]+)"', source)
                names[stem] = declared.group(1) if declared else stem
    return names


def after(vam, screen_before, what):
    done = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "vamkit"), "verify", "--vam", str(vam)], capture_output=True, text=True)
    check(done.returncode == 0, f"after the {what} run VaM's own files are in place and the install is intact",
          "" if done.returncode == 0 else " | ".join(l for l in done.stdout.splitlines() if l.startswith("FAIL")))
    check(screen() == screen_before, f"after the {what} run the window settings in the registry are as before", "" if screen() == screen_before else f"{screen()} was {screen_before}")
    check(not (vam / "BepInEx" / "plugins" / "VaMTestAgent.dll").exists(), f"after the {what} run the test agent is out of the plugins folder")


def vanilla(vam):
    print("-- vanilla, desktop")
    log, screen_before = vam / "BepInEx" / "LogOutput.log", screen()
    before = log.stat().st_mtime if log.is_file() else None
    session = Session(vam, variant="vanilla", display="Desktop", name="first-run-vanilla")
    session.start()
    try:
        time.sleep(25)   # Unity is up and past the point where Doorstop would have started BepInEx
        alive = bool(processes(vam / "VaM.exe"))
        check(alive, "vanilla: VaM is running")
        loaded = modules(session.pid) if alive else []
        (session.run / "modules.json").write_text(json.dumps(loaded, indent=1), encoding="utf-8")
        kit = str(vam / "vamkit").lower()
        ours = [m for m in loaded if m.lower().startswith(kit) or m.lower() == str(vam / "d3d11.dll").lower()
                or "\\bepinex\\" in m.lower() or m.lower().endswith(".bin")]
        check(bool(loaded) and not ours, "vanilla: no file of the kit and nothing of BepInEx is loaded into VaM", ", ".join(ours) if ours else f"{len(loaded)} modules")
        check((log.stat().st_mtime if log.is_file() else None) == before, "vanilla: BepInEx wrote no log")
    finally:
        code = session.stop(wait=25)
    check(code is not None, "vanilla: the launcher finished after VaM", "VaM did not close on WM_CLOSE and was ended" if session.killed else f"exit code {code}")
    after(vam, screen_before, "vanilla")


def look(session, seen, name):
    """Status of VaM and of the skin plugin (which hosts the path tracer) at one moment."""
    skin = json.loads(session.invoke("VaMSkinTransmission", "Status"))
    seen[name] = dict(status=session.status(), skin=skin)
    rt = skin.get("native", {}).get("rt", {})
    return seen[name]["status"], skin, rt, rt.get("path_tracing", {})


def mod(vam, scene):
    print("-- mod, desktop")
    screen_before, plugins = screen(), installed_plugins(vam)
    staged = None
    if not scene:
        staged = vam / "Saves" / "scene" / "_agent-test"
        staged.mkdir(parents=True, exist_ok=True)
        shutil.copy2(HERE / "scenes" / "person.json", staged / "person.json")
        scene = "Saves/scene/_agent-test/person.json"
    session = Session(vam, variant="mod", display="Desktop", name="first-run-mod")
    seen = {}
    session.start()
    try:
        session.wait_ready()
        session.call("load_scene", path=scene)
        time.sleep(3)
        session.wait_ready(timeout=300)
        session.call("hud", close_all=True)
        time.sleep(6)
        status, skin, rt, path = look(session, seen, "start")
        check(status.get("fps", 0) > 5, "mod: frames go by", f"{status.get('fps')} fps")
        check(len(skin.get("skins", [])) > 0 and not skin.get("native_error"), "mod: the scene's person is taken by the skin shading", f"{len(skin.get('skins', []))} skin(s) {skin.get('native_error', '')}")
        loaded = modules(session.pid)
        (session.run / "modules.json").write_text(json.dumps(loaded, indent=1), encoding="utf-8")
        low = [m.lower() for m in loaded]
        check(str(vam / "d3d11.dll").lower() in low, "mod: the D3D11 proxy beside VaM.exe is the d3d11.dll in the process")
        natives = list((vam / "BepInEx" / "plugins").glob("*Native.bin"))
        missing = [p.name for p in natives if str(p).lower() not in low]
        check(not missing, "mod: every plugin's native library is loaded", ", ".join(missing) if missing else f"{len(natives)} libraries")

        if not check(rt.get("available") == 1, "mod: this machine can path trace", rt.get("reason", "")):
            raise SessionError("the path tracing steps were left out")
        check(path.get("active") == 0, "mod: path tracing is off at the start")
        print("      switching path tracing on (the first time after a build the driver compiles the pipelines)", flush=True)
        started = time.time()
        session.set("VaMSkinTransmission", "rtRenderer.Value", "PathTracing")
        session.call("wait_frames", wait=300, n=120)
        seconds = round(time.time() - started, 1)
        time.sleep(8)
        status, skin, rt, path = look(session, seen, "path_on")
        seen["path_on"]["seconds_to_120_frames"] = seconds
        check(path.get("active") == 1 and rt.get("failures") == 0 and rt.get("core", {}).get("failures") == 0 and not skin.get("native_error"),
              "mod: path tracing took over when switched on while playing",
              f"{path.get('calls')} traced frames, {rt.get('core', {}).get('triangles')} triangles, {path.get('hair_segments')} hair segments; 120 frames {seconds} s after the switch")
        check(skin.get("native", {}).get("reconstruction", {}).get("active") == 1, "mod: ray reconstruction is the upscaler while path tracing is on")
        check(status.get("fps", 0) > 5, "mod: frames go by with path tracing on", f"{status.get('fps')} fps")

        session.set("VaMSkinTransmission", "rtRenderer.Value", "Legacy")
        session.call("wait_frames", n=120)
        time.sleep(6)
        status, skin, rt, path = look(session, seen, "path_off")
        check(path.get("active") == 0 and skin.get("native", {}).get("reconstruction", {}).get("active") == 0, "mod: path tracing and ray reconstruction stop when switched off")
        check(status.get("fps", 0) > 5, "mod: frames go by with path tracing off again", f"{status.get('fps')} fps")
    except (SessionError, SimXRError, OSError) as e:
        check(False, "mod: the session ran to its end", str(e))
    finally:
        (session.run / "seen.json").write_text(json.dumps(seen, indent=1), encoding="utf-8")
        code = session.stop()
        if staged:
            shutil.rmtree(staged, ignore_errors=True)
    check(code == 0 and not session.killed, "mod: VaM quit when asked and the launcher finished",
          f"exit code {code}{', VaM had to be ended' if session.killed else ''}\n{session.launcher_text()[-400:]}" if code or session.killed else "")

    folder = session.log_folder()

    def text(name):
        path = folder / name if folder else None
        return path.read_text(encoding="utf-8", errors="replace") if path and path.is_file() else ""

    bepinex, compositor, generation = text("bepinex.log"), text("opencomposite.log"), text("dlssg/dlssg.log")
    check(bool(bepinex), "mod: BepInEx's log of the run was kept", str(folder))
    started = set(re.findall(r"Loading \[(.+?) [0-9][^\]]*\]", bepinex))
    not_loaded = [stem for stem, name in plugins.items() if name not in started]
    check(not not_loaded, "mod: BepInEx loaded every installed plugin", ", ".join(not_loaded) if not_loaded else f"{len(plugins)} plugins")
    errors = [l for l in bepinex.splitlines() if l.startswith("[Error") or l.startswith("[Fatal")]
    by = {}
    for line in errors:
        source = line.split("]", 1)[0].split(":", 1)[-1].strip()
        by[source] = by.get(source, 0) + 1
    check(not errors, "mod: no error lines in BepInEx's log", "; ".join(f"{k} x{v}" for k, v in by.items()) + ("\n      " + "\n      ".join(errors[:6]) if errors else ""))
    what_ran(vam, session, bepinex, compositor, generation, "mod", running_in(folder) if folder else None)
    check("VAM RR created" in compositor, "mod: DLSS ray reconstruction was created (path tracing on)")
    bad = sorted(set(re.findall(r"0x[bB][aA][dD][0-9a-fA-F]{5}", compositor + generation)))
    check(not bad, "mod: no NGX error code in the compositor's and the frame generation's logs", ", ".join(bad))
    after(vam, screen_before, "mod")
    print(f"      evidence: {session.run}\n      logs:     {folder}")


def headset(session, seen, name, seconds=10):
    """What reached the simulated headset in `seconds`, VaM's own frame rate at the end, one captured frame."""
    session.sim.stats(reset=True)
    time.sleep(seconds)
    stats = session.sim.stats()
    status = session.status()
    capture = session.sim.capture(frames=1, eyes="left", scale=0.25, name=name, timeout=60)   # a PNG to look at
    seen[name] = dict(stats=stats, status=status, capture=capture)
    try:   # and a small raw one to judge: a frame that shows something has brightness and spread
        raw = session.sim.capture(frames=1, eyes="left", scale=0.125, format="raw", name=name + "-check", timeout=60)
        entry = json.loads((Path(raw["dir"]) / "capture.jsonl").read_text(encoding="utf-8").splitlines()[-1])
        data = (Path(raw["dir"]) / entry["file"]).read_bytes()[: entry["width"] * entry["height"] * 4]
        colour = [b for i, b in enumerate(data) if i % 4 != 3]
        mean = sum(colour) / max(len(colour), 1)
        seen[name]["picture"] = dict(mean=round(mean, 1), spread=round((sum((b - mean) ** 2 for b in colour) / max(len(colour), 1)) ** 0.5, 1),
                                     size=[entry["width"], entry["height"]])
    except (SimXRError, OSError, ValueError, KeyError, IndexError) as e:
        seen[name]["picture"] = dict(error=str(e))
    picture = seen[name]["picture"]
    check(picture.get("mean", 0) > 1 and picture.get("spread", 0) > 4, f"vr: the frame handed to the headset shows a picture ({name})",
          picture.get("error") or f"mean {picture['mean']}, spread {picture['spread']} of 255; {Path(capture['dir']).name}/{name}-*.png")
    return stats, status, capture


def vr(vam, scene):
    print("-- mod, VR on the simulated headset")
    if not SIMULATED_RUNTIME.is_file():
        print(f"      left out: no simulated headset at {SIMULATED_RUNTIME} (python tools/vamkit fetch --lab --accept-licenses, then build --lab)")
        return
    screen_before = screen()
    staged = None
    if not scene:
        staged = vam / "Saves" / "scene" / "_agent-test"
        staged.mkdir(parents=True, exist_ok=True)
        shutil.copy2(HERE / "scenes" / "person.json", staged / "person.json")
        scene = "Saves/scene/_agent-test/person.json"
    session = Session(vam, variant="mod", display="VR", name="first-run-vr", overrides=dict(framegen=1, frame_multiplier=MULTIPLIER),
                      sim_env={"SIMXR_REFRESH": REFRESH, "SIMXR_HEAD_HEIGHT_CM": 160})
    seen = {}
    code = None
    try:
        session.start()
        session.wait_ready()
        status = session.status()
        seen["start"] = dict(status=status, headset=session.sim.status())
        xr = status.get("xr", {})
        check(xr.get("enabled") is True, "vr: VaM runs a VR session", f"{xr.get('device')} {xr.get('model')}, {xr.get('eye_w')}x{xr.get('eye_h')} per eye, {xr.get('refresh_hz')} Hz")
        session.call("load_scene", path=scene)
        time.sleep(3)
        session.wait_ready(timeout=300)
        session.call("hud", close_all=True)
        session.set_ini("Performance", "FrameGeneration", "0")
        time.sleep(8)
        stats, status, capture = headset(session, seen, "raster")
        skin = json.loads(session.invoke("VaMSkinTransmission", "Status"))
        seen["raster"]["skin"] = skin
        check(status.get("fps", 0) > 5, "vr: frames go by", f"{status.get('fps')} fps in VaM")
        check(len(skin.get("skins", [])) > 0 and not skin.get("native_error"), "vr: the scene's person is taken by the skin shading")
        check(stats.get("new_frames", 0) > 50, "vr: the headset is handed new frames",
              f"{stats.get('displayed_fps')} new frames a second on a {stats.get('display_hz')} Hz display, frame generation off")

        session.set("VaMSkinTransmission", "rtRenderer.Value", "PathTracing")
        session.call("wait_frames", wait=300, n=120)
        time.sleep(8)
        stats, status, capture = headset(session, seen, "path")
        skin = json.loads(session.invoke("VaMSkinTransmission", "Status"))
        seen["path"]["skin"] = skin
        rt = skin.get("native", {}).get("rt", {})
        check(rt.get("path_tracing", {}).get("active") == 1 and not skin.get("native_error"), "vr: path tracing took over",
              f"{rt.get('core', {}).get('triangles')} triangles, {rt.get('path_tracing', {}).get('hair_segments')} hair segments, {status.get('fps')} fps in VaM")
        check(skin.get("native", {}).get("reconstruction", {}).get("active") == 1, "vr: ray reconstruction is the upscaler while path tracing is on")

        session.set_ini("Performance", "FrameGeneration", "1")
        time.sleep(8)
        stats, status, capture = headset(session, seen, "path-fg")
        beat = REFRESH / MULTIPLIER
        check(abs(status.get("fps", 0) - beat) <= 0.1 * beat and stats.get("submitted_fps", 0) >= 0.9 * REFRESH,
              f"vr: with frame generation VaM renders {REFRESH} / {MULTIPLIER} and the headset is handed a frame per refresh",
              f"{status.get('fps')} fps in VaM, {stats.get('submitted_fps')} frames a second handed over, {stats.get('repeats')} repeated refreshes in {stats.get('vsyncs')}")
    except (SessionError, SimXRError, OSError) as e:
        check(False, "vr: the session ran to its end", str(e))
    finally:
        if session.run:
            (session.run / "seen.json").write_text(json.dumps(seen, indent=1), encoding="utf-8")
        code = session.stop()
        if staged:
            shutil.rmtree(staged, ignore_errors=True)
    check(code == 0 and not session.killed, "vr: VaM quit when asked and the launcher finished",
          f"exit code {code}{', VaM had to be ended' if session.killed else ''}\n{session.launcher_text()[-400:]}" if code or session.killed else "")
    folder = session.log_folder()

    def text(name):
        path = folder / name if folder else None
        return path.read_text(encoding="utf-8", errors="replace") if path and path.is_file() else ""

    compositor, generation = text("opencomposite.log"), text("dlssg/dlssg.log")
    not_whole(vam, text("bepinex.log"), "vr")
    stand_ins(running_in(folder) if folder else {}, "vr")
    eyes = re.findall(r"DLSS SR feature created: eye=(\d) input=(\d+x\d+) output=(\d+x\d+)", compositor)
    check({e for e, _, _ in eyes} == {"0", "1"}, "vr: DLSS super resolution was created for both eyes", f"{eyes[0][1]} -> {eyes[0][2]} per eye" if eyes else "")
    check(len(set(re.findall(r"VAM RR created eye=(\d)", compositor))) == 2, "vr: DLSS ray reconstruction was created for both eyes")
    check('"generating"' in text("dlssg/dlssg-status.json"), "vr: the frame generation layer reports that it generates")
    bad = sorted(set(re.findall(r"0x[bB][aA][dD][0-9a-fA-F]{5}", compositor + generation)))
    check(not bad, "vr: no NGX error code in the compositor's and the frame generation's logs", ", ".join(bad))
    after(vam, screen_before, "VR")
    print(f"      evidence: {session.run}\n      logs:     {folder}")


def main():
    global REFRESH, MULTIPLIER
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vam", required=True)
    ap.add_argument("--scene", help="another scene to load in the mod session, as VaM names it (Saves/scene/x.json)")
    ap.add_argument("--only", choices=("vanilla", "mod", "vr"))
    ap.add_argument("--refresh", type=int, default=REFRESH, help="the simulated headset's refresh rate in the VR part")
    ap.add_argument("--multiplier", type=int, default=MULTIPLIER, help="frames shown per real frame in the VR part")
    args = ap.parse_args()
    vam = Path(args.vam).resolve()
    REFRESH, MULTIPLIER = args.refresh, args.multiplier
    if args.only in (None, "vanilla"):
        vanilla(vam)
    if args.only in (None, "mod"):
        mod(vam, args.scene)
    if args.only in (None, "vr"):
        vr(vam, args.scene)
    if failures:
        print(f"{len(failures)} check(s) failed" + (f"; {len(lessened)} more work with less (LESS above)" if lessened else ""))
        return 1
    if lessened:
        print(f"the first run works, with less than the kit has: {len(lessened)} LESS line(s) above say what the user is not getting and how to mend it")
        return 2
    print("the first run holds: every part ran as asked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
