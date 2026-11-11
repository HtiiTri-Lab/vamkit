"""The first run after an install: does the installed build start, does the chain come up, does vanilla load nothing.

Two short desktop sessions in the background (no headset needed), from an installed VaM folder:
  vanilla   VaM's own files; BepInEx must not start and no file of the kit may be loaded into the process
  mod       the plugins load, the proxy is in the process, a scene with one person renders, DLSS super resolution and
            frame generation come up; path tracing is switched on while playing (the first time after a build the
            driver compiles the pipelines: about 20 s without frames) and ray reconstruction takes over; then off
Afterwards VaM's own files and the window settings must be back, and the test agent out of the plugins folder.
The scene is scenes/person.json (one default person, one light), staged in Saves/scene/_agent-test/ for the run.
What each step saw is kept as seen.json in the run folder; the logs are in <VaM>/vamkit/logs/<run>/.

    python tools/lab/session/first_run.py --vam <VaM folder> [--scene Saves/scene/x.json] [--only vanilla|mod]

Exit code 0 when every check holds. This starts the game: one VaM at a time, and the GPU is busy while it runs
(about one minute for vanilla, two for the mod).
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
from session import Session, SessionError, SimXRError, processes  # noqa: E402

failures = []


def check(ok, what, detail=""):
    print(("ok    " if ok else "FAIL  ") + what + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failures.append(what)
    return ok


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
    check("DLSS SR feature created" in compositor, "mod: DLSS super resolution was created (path tracing off)")
    check("VAM RR created" in compositor, "mod: DLSS ray reconstruction was created (path tracing on)")
    check(bool(re.search(r"DLSS-G\[\d+\] first evaluation recorded", generation)), "mod: DLSS frame generation generated frames")
    bad = sorted(set(re.findall(r"0x[bB][aA][dD][0-9a-fA-F]{5}", compositor + generation)))
    check(not bad, "mod: no NGX error code in the compositor's and the frame generation's logs", ", ".join(bad))
    after(vam, screen_before, "mod")
    print(f"      evidence: {session.run}\n      logs:     {folder}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vam", required=True)
    ap.add_argument("--scene", help="another scene to load in the mod session, as VaM names it (Saves/scene/x.json)")
    ap.add_argument("--only", choices=("vanilla", "mod"))
    args = ap.parse_args()
    vam = Path(args.vam).resolve()
    if args.only != "mod":
        vanilla(vam)
    if args.only != "vanilla":
        mod(vam, args.scene)
    print(f"{len(failures)} check(s) failed" if failures else "the first run holds")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
