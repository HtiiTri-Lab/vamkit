"""Tests of vamkit install / verify / uninstall / rollback and of the launcher's file swap, on a made-up VaM folder
and a made-up kit in a temporary directory. Nothing outside that directory is touched; no game is started.

    python -I tools/vamkit/tests/install_test.py        exit 0 when every check holds
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import install as kit_install  # noqa: E402

# The launcher's own files, as they are in the repository now (a module added there must not need a change here).
LAUNCHER = tuple(sorted(p.name for p in (Path(__file__).resolve().parents[3] / "components" / "render" / "launcher").iterdir()
                        if p.suffix in (".py", ".json")))
VAMS_OPENVR = b"VaM's own openvr_api.dll"
CFG_B = b"[General]\r\n\r\n## Runs the thing\r\nEnabled = false\r\nOther = 3\r\n"
failures = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


def quiet(function, *args, **kwargs):
    """Runs one command of the installer; returns its exit code and what it printed."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = function(*args, **kwargs)
    return code, out.getvalue()


def snapshot(root):
    files, folders = {}, set()
    for base, dirs, names in os.walk(root):
        for d in dirs:
            folders.add((Path(base) / d).relative_to(root).as_posix())
        for n in names:
            p = Path(base) / n
            files[p.relative_to(root).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    return files, folders


def difference(a, b):
    """What differs between two snapshots, as short text ('' when nothing does)."""
    (fa, da), (fb, db) = a, b
    parts = [f"-{p}" for p in sorted(set(fa) - set(fb))] + [f"+{p}" for p in sorted(set(fb) - set(fa))]
    parts += [f"~{p}" for p in sorted(set(fa) & set(fb)) if fa[p] != fb[p]]
    parts += [f"-{p}/" for p in sorted(da - db)] + [f"+{p}/" for p in sorted(db - da)]
    return " ".join(parts)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def make_vam(root):
    write(root / "VaM.exe", b"not a game")
    write(root / "winhttp.dll", b"doorstop")
    write(root / "VaM_Data/Plugins/openvr_api.dll", VAMS_OPENVR)
    write(root / "BepInEx/core/BepInEx.dll", b"bepinex")
    write(root / "BepInEx/plugins/Other.dll", b"someone's plugin")
    write(root / "BepInEx/plugins/PluginB.dll", b"an older PluginB that was here")
    write(root / "BepInEx/config/b.cfg", CFG_B)
    write(root / "Saves/scene/mine.json", b"{}")
    return root


def make_kit(root):
    write(root / "out/proxy/d3d11.dll", b"the proxy")
    write(root / "out/oc/openvr_api.dll", b"opencomposite, built")
    write(root / "out/a/PluginA.dll", b"plugin A, build 1")
    write(root / "out/b/PluginB.dll", b"plugin B, build 1")
    write(root / "third_party/rt/rt.dll", b"a runtime")
    for name in LAUNCHER:
        write(root / "launcher" / name, (REPO / "components/render/launcher" / name).read_bytes())
    write(root / "src/c/PluginC.cs", b'[BepInPlugin("test.c", "Plugin C", "1.0")]\npublic class PluginC {}\n')
    write(root / "out/c/PluginC.dll", b"plugin C, build 1")
    components = [
        dict(id="proxy", installs=[["out/proxy/d3d11.dll", "vamkit/bin/d3d11.dll"]]),
        dict(id="oc", installs=[["out/oc/openvr_api.dll", "vamkit/bin/openvr_api.dll"]]),
        dict(id="a", installs=[["out/a/PluginA.dll", "BepInEx/plugins/PluginA.dll"]], enable=[["BepInEx/config/a.cfg", "General", "Enabled", "true"]]),
        dict(id="b", installs=[["out/b/PluginB.dll", "BepInEx/plugins/PluginB.dll"]], enable=[["BepInEx/config/b.cfg", "General", "Enabled", "true"]]),
        dict(id="c", path="src/c", managed=dict(sources=["PluginC.cs"]), installs=[["out/c/PluginC.dll", "BepInEx/plugins/PluginC.dll"]]),
        dict(id="launcher", installs=[[f"launcher/{name}", f"vamkit/launcher/{name}"] for name in LAUNCHER]),
    ]
    items = [dict(id="rt", name="rt.dll", installs="vamkit/bin/dlss/sr/rt.dll"),
             dict(id="opt", name="opt.dll", optional=True, installs="vamkit/bin/dlss/nr/opt.dll", without="the optional thing"),
             dict(id="sdk", name="unused")]
    # as tools/vamkit reports the run-time files: rt is fetched, opt is not
    runtimes = lambda: [dict(id="rt", name="rt.dll", present=True, without="", how=""),
                        dict(id="opt", name="opt.dll", present=False, without="the optional thing", how="fetch --file opt=<path>")]
    return kit_install.Kit(root, components, items, lambda item: item["id"] == "rt", runtimes)


def load_launcher(vam):
    """launch.py as installed, with its paths bound to this VaM folder."""
    spec = importlib.util.spec_from_file_location("installed_launch_%d" % len(sys.modules), vam / "vamkit/launcher/launch.py")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(vam / "vamkit/launcher"))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.pop(0)
    return module


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        kit, vam = make_kit(tmp / "kit"), make_vam(tmp / "VaM")
        before = snapshot(vam)

        code, text = quiet(kit_install.install, kit, vam, False)
        check(code == 0 and not difference(before, snapshot(vam)) and "nothing was changed" in text, "the plan alone changes nothing")
        check("replace" in text and "PluginB.dll" in text, "the plan names the file it replaces")
        check("MISSING  opt.dll" in text and "you do not get: the optional thing" in text and "to add it: fetch --file opt=<path>" in text,
              "the plan says which run-time file is missing, what the user does not get and how to add it")

        code, text = quiet(kit_install.install, kit, vam, True)
        check(code == 1 and "--replace" in text and not difference(before, snapshot(vam)), "files that are not vamkit's are not installed over without --replace")
        code, text = quiet(kit_install.install, kit, vam, True, False, True)
        installed = snapshot(vam)
        check(code == 0, "install --yes --replace succeeds")
        check((vam / "BepInEx/plugins/PluginA.dll").read_bytes() == b"plugin A, build 1" and (vam / "vamkit/bin/dlss/sr/rt.dll").is_file()
              and not (vam / "vamkit/bin/dlss/nr").exists() and (vam / "vamkit/launcher/launch.py").is_file(), "plugins, runtime and launcher are in place")
        check((vam / "vamkit/original/replaced/BepInEx/plugins/PluginB.dll").read_bytes() == b"an older PluginB that was here"
              and (vam / "BepInEx/plugins/PluginB.dll").read_bytes() == b"plugin B, build 1", "a file under the same name is kept and replaced")
        check((vam / "vamkit/original/openvr_api.dll").read_bytes() == VAMS_OPENVR and (vam / "vamkit/original/d3d11.dll.absent").is_file()
              and (vam / "vamkit/original/opencomposite.ini.absent").is_file(), "VaM's own files are recorded as found")
        check((vam / "VaM_Data/Plugins/openvr_api.dll").read_bytes() == VAMS_OPENVR and not (vam / "d3d11.dll").exists(), "the install does not put the mod in VaM's place")
        b_cfg = (vam / "BepInEx/config/b.cfg").read_bytes()
        check(b"Enabled = true\r\n" in b_cfg and b"Other = 3\r\n" in b_cfg and b"## Runs the thing" in b_cfg, "a setting is written into an existing cfg, the rest of it kept")
        check(b"Enabled = true" in (vam / "BepInEx/config/a.cfg").read_bytes(), "a cfg that was not there is made")
        check(installed[0]["Saves/scene/mine.json"] == before[0]["Saves/scene/mine.json"] and installed[0]["BepInEx/plugins/Other.dll"] == before[0]["BepInEx/plugins/Other.dll"], "other files are untouched")
        code, text = quiet(kit_install.verify, kit, vam)
        check(code == 0 and "FAIL" not in text, "verify passes after the install")

        code, text = quiet(kit_install.install, kit, vam, True, False, True)
        check(code == 0 and "nothing to do" in text and not difference(installed, snapshot(vam)), "a second install does nothing")

        code, text = quiet(kit_install.uninstall, vam, True)
        check(code == 0 and not difference(before, snapshot(vam)), "uninstall leaves the folder as it was: " + (difference(before, snapshot(vam)) or "identical"))
        code, _ = quiet(kit_install.uninstall, vam, True)
        check(code == 1, "uninstall without an install says so")

        quiet(kit_install.install, kit, vam, True, False, True)
        code, _ = quiet(kit_install.rollback, vam, True)
        check(code == 0 and not difference(before, snapshot(vam)), "rollback of the first install leaves the folder as it was: " + (difference(before, snapshot(vam)) or "identical"))

        # an update: one more build, installed over the first; one step back is the first again
        quiet(kit_install.install, kit, vam, True, False, True)
        first = snapshot(vam)
        write(kit.repo / "out/a/PluginA.dll", b"plugin A, build 2")
        code, text = quiet(kit_install.verify, kit, vam)
        check(code == 0 and "differ from what is built now" in text, "verify notes that the build is newer than the install")
        code, text = quiet(kit_install.install, kit, vam, True, False, True)
        check(code == 0 and (vam / "BepInEx/plugins/PluginA.dll").read_bytes() == b"plugin A, build 2" and "update" in text, "an update replaces the installed file")
        step = sorted((vam / "vamkit/backup").iterdir())[-1]
        check((step / "files/BepInEx/plugins/PluginA.dll").read_bytes() == b"plugin A, build 1" and (step / "restore.cmd").is_file(), "the update's backup folder holds the file it replaced and a restore.cmd")
        any_vam = "VaM.exe" in subprocess.run(["tasklist", "/FI", "IMAGENAME eq VaM.exe", "/NH"], capture_output=True, text=True).stdout
        if any_vam:
            print("note  restore.cmd not run: a VaM is running on this machine and the script refuses then")
            quiet(kit_install.rollback, vam, True)
        else:
            done = subprocess.run(["cmd", "/c", str(step / "restore.cmd")], capture_output=True, text=True)
            after = snapshot(vam)
            rest = [p for p in difference(first, after).split() if "vamkit/backup/" not in p]
            check(done.returncode == 0 and not rest, "restore.cmd puts the first install back: " + (" ".join(rest) or "identical apart from its own folder"))
            shutil.rmtree(step)
        check((vam / "BepInEx/plugins/PluginA.dll").read_bytes() == b"plugin A, build 1", "one step back is the first build")
        write(kit.repo / "out/a/PluginA.dll", b"plugin A, build 1")
        code, _ = quiet(kit_install.uninstall, vam, True)
        check(code == 0 and not difference(before, snapshot(vam)), "uninstall after an update and a step back: " + (difference(before, snapshot(vam)) or "identical"))

        # the launcher's swap, a VaM that changed since the install, a run that was killed
        quiet(kit_install.install, kit, vam, True, False, True)
        launch = load_launcher(vam)
        launch.activate_mod()
        check((vam / "VaM_Data/Plugins/openvr_api.dll").read_bytes() == b"opencomposite, built" and (vam / "d3d11.dll").read_bytes() == b"the proxy"
              and b"; vamkit" in (vam / "VaM_Data/Plugins/opencomposite.ini").read_bytes(), "the launcher puts the mod's files in VaM's place")
        code, text = quiet(kit_install.verify, kit, vam)
        check(code == 1 and "a run was killed" in text, "verify reports the mod's files left in VaM's place")
        if any_vam:   # launch.py refuses while any VaM runs
            launch.restore_files()
        else:
            done = subprocess.run([sys.executable, "-I", str(vam / "vamkit/launcher/launch.py"), "--restore"], capture_output=True, text=True)
            check(done.returncode == 0, "launch.py --restore runs from the installed folder" + ("" if done.returncode == 0 else ": " + done.stderr.strip()[-200:]))
        check((vam / "VaM_Data/Plugins/openvr_api.dll").read_bytes() == VAMS_OPENVR and not (vam / "d3d11.dll").exists()
              and not (vam / "VaM_Data/Plugins/opencomposite.ini").exists(), "the launcher puts VaM's own back")
        write(vam / "VaM_Data/Plugins/openvr_api.dll", b"VaM's openvr_api.dll after an update")
        write(vam / "d3d11.dll", b"another injector")
        launch.restore_files()
        check((vam / "VaM_Data/Plugins/openvr_api.dll").read_bytes() == b"VaM's openvr_api.dll after an update" and (vam / "d3d11.dll").read_bytes() == b"another injector",
              "restoring leaves files alone that are not the mod's")
        launch.activate_mod()
        launch.restore_files()
        check((vam / "VaM_Data/Plugins/openvr_api.dll").read_bytes() == b"VaM's openvr_api.dll after an update" and (vam / "d3d11.dll").read_bytes() == b"another injector",
              "after a VaM update the next run puts the updated file back, not the one the install found")
        launch.activate_mod()   # ... and this run is killed
        code, text = quiet(kit_install.uninstall, vam, True)
        expected = (dict(before[0]), set(before[1]))
        expected[0]["VaM_Data/Plugins/openvr_api.dll"] = hashlib.sha256(b"VaM's openvr_api.dll after an update").hexdigest()
        expected[0]["d3d11.dll"] = hashlib.sha256(b"another injector").hexdigest()
        check(code == 0 and not difference(expected, snapshot(vam)), "uninstall after a killed run puts VaM's files back: " + (difference(expected, snapshot(vam)) or "identical"))
        (vam / "d3d11.dll").unlink()
        write(vam / "VaM_Data/Plugins/openvr_api.dll", VAMS_OPENVR)

        # what the mod writes while it is used stays the user's
        quiet(kit_install.install, kit, vam, True, False, True)
        write(vam / "vamkit/logs/run1/bepinex.log", b"log")
        write(vam / "vamkit/launcher/profiles/VR/LaunchPanel.json", b"{}")
        write(vam / "BepInEx/config/a.cfg", (vam / "BepInEx/config/a.cfg").read_bytes() + b"Level = 2\r\n")           # BepInEx wrote the plugin's settings
        write(vam / "BepInEx/config/b.cfg", (vam / "BepInEx/config/b.cfg").read_bytes() + b"Mine = 7\r\n")
        write(vam / "BepInEx/config/test.c.cfg", b"[General]\r\nLevel = 1\r\n")       # a plugin made its own cfg at its first run
        write(vam / "d3d11_proxy.log", b"the proxy's log")
        code, text = quiet(kit_install.uninstall, vam, True)
        rest = difference(before, snapshot(vam)).split()
        check(code == 0 and sorted(p for p in rest if not p.endswith("/")) == ["+BepInEx/config/a.cfg", "+BepInEx/config/test.c.cfg", "+vamkit/launcher/profiles/VR/LaunchPanel.json", "+vamkit/logs/run1/bepinex.log", "~BepInEx/config/b.cfg"],
              "uninstall keeps logs, presets and settings written since, and removes the mod's own log: " + " ".join(rest))
        check("test.c.cfg" in text and "made while the mod was used" in text, "uninstall names the settings a plugin made")
        check((vam / "BepInEx/config/b.cfg").read_bytes() == CFG_B + b"Mine = 7\r\n", "in a cfg that changed since, only the install's setting goes back")
        quiet(kit_install.install, kit, vam, True, False, True)
        code, _ = quiet(kit_install.uninstall, vam, True, True)
        check(code == 0 and not (vam / "vamkit").exists() and not (vam / "BepInEx/config/test.c.cfg").exists(), "uninstall --purge removes the logs, presets and the plugins' own settings too")
        (vam / "BepInEx/config/a.cfg").unlink(missing_ok=True)

        # refusals
        (kit.repo / "out/b/PluginB.dll").unlink()
        state = snapshot(vam)
        code, text = quiet(kit_install.install, kit, vam, True, False, True)
        check(code == 1 and "not there" in text and not difference(state, snapshot(vam)), "a kit that is not completely built is not installed")
        write(kit.repo / "out/b/PluginB.dll", b"plugin B, build 1")
        shutil.rmtree(vam / "BepInEx/core")
        code, text = quiet(kit_install.install, kit, vam, True, False, True)
        check(code == 1 and "BepInEx" in text and not (vam / "vamkit").exists(), "a VaM folder without BepInEx is refused")

    print(f"{len(failures)} check(s) failed" if failures else "all install checks hold")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
