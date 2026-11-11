"""vamkit: check, fetch, build and test the mod for one VaM folder. Python standard library only.

  python tools/vamkit doctor                         what is there and what is missing; changes nothing
  python tools/vamkit fetch  [ids] [--accept-licenses] [--mirror DIR] [--file ID=PATH]
                                                     third-party SDKs and runtimes into third_party/
  python tools/vamkit build  [ids] [--steps generate,native,managed] [--clean]
                                                     every component into out/<id>/, in the manifest's order
  python tools/vamkit test   [ids]                   the offline tests of the components
  python tools/vamkit install   [--yes] [--replace]  what was built, into the VaM folder (install.py); the plan without --yes
  python tools/vamkit verify                         the install's record against the VaM folder and the build
  python tools/vamkit rollback  [--yes]              takes the last install back
  python tools/vamkit uninstall [--yes] [--purge]    the VaM folder as it was before the first install
  python tools/vamkit status                         which installed files are present in the VaM folder

The VaM folder comes from --vam, else VAMKIT_VAM, else local/vam.txt (one line, git-ignored). The build tools are found
by themselves; VAMKIT_VCVARS, VAMKIT_FXC, VAMKIT_DXC and VAMKIT_CSC override what is found.
"""
from __future__ import annotations

import argparse
import ctypes
import fnmatch
import glob
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "out"
THIRD = REPO / "third_party"
BELOW_NORMAL = 0x00004000
NO_WINDOW = 0x08000000


def components():
    return tomllib.loads((REPO / "manifest" / "components.toml").read_text(encoding="utf-8"))["component"]


def third_party():
    return tomllib.loads((REPO / "manifest" / "thirdparty.toml").read_text(encoding="utf-8"))["item"]


def vam_folder(given):
    for candidate in (given, os.environ.get("VAMKIT_VAM")):
        if candidate:
            return Path(candidate)
    saved = REPO / "local" / "vam.txt"
    if saved.is_file():
        return Path(saved.read_text(encoding="utf-8").strip())
    return None


def file_version(path):
    size = ctypes.windll.version.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return None
    data = ctypes.create_string_buffer(size)
    if not ctypes.windll.version.GetFileVersionInfoW(str(path), 0, size, data):
        return None
    pointer, length = ctypes.c_void_p(), ctypes.c_uint()
    if not ctypes.windll.version.VerQueryValueW(data, "\\", ctypes.byref(pointer), ctypes.byref(length)):
        return None
    words = (ctypes.c_uint32 * 4).from_address(pointer.value + 8)
    return f"{words[0] >> 16}.{words[0] & 0xffff}.{words[1] >> 16}.{words[1] & 0xffff}"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(args):
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=30, creationflags=NO_WINDOW).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


# ---- build tools ------------------------------------------------------------------------------------------------

def visual_studio():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return None
    found = run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"])
    return Path(found.splitlines()[0]) if found else None


def windows_sdk_bin(name):
    """The newest Windows SDK bin\\<version>\\x64 folder that holds `name`."""
    roots = sorted(glob.glob(os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                                          "Windows Kits", "10", "bin", "10.*", "x64", name)))
    return Path(roots[-1]).parent if roots else None


def tools():
    """{VAMKIT_X: path or None} for the build tools: the environment's value, else what is found on this machine."""
    found = {}
    vs = visual_studio()
    vcvars = os.environ.get("VAMKIT_VCVARS") or (vs and vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat")
    found["VAMKIT_VCVARS"] = Path(vcvars) if vcvars and Path(vcvars).is_file() else None
    for variable, program in (("VAMKIT_FXC", "fxc.exe"), ("VAMKIT_DXC", "dxc.exe")):
        folder = os.environ.get(variable) or windows_sdk_bin(program)
        folder = Path(folder) if folder else None
        if folder and folder.is_file():
            folder = folder.parent
        found[variable] = folder if folder and (folder / program).is_file() else None
    csc = os.environ.get("VAMKIT_CSC") or Path(os.environ.get("WINDIR", r"C:\Windows")) / "Microsoft.NET" / "Framework64" / "v3.5" / "csc.exe"
    found["VAMKIT_CSC"] = Path(csc) if Path(csc).is_file() else None
    return found


HINTS = {
    "VAMKIT_VCVARS": "install Visual Studio 2022 Build Tools with the C++ workload",
    "VAMKIT_FXC": "install the Windows 10/11 SDK (fxc.exe)",
    "VAMKIT_DXC": "install the Windows 10/11 SDK (dxc.exe)",
    "VAMKIT_CSC": "enable the Windows feature \".NET Framework 3.5\" (the plugins are C# 3 for VaM's Mono)",
}


def build_environment(vam):
    """os.environ plus VAMKIT_* for the scripts; stops with what is missing."""
    env = dict(os.environ)
    missing = []
    if vam is None or not (vam / "VaM.exe").is_file():
        missing.append("the VaM folder: give --vam, set VAMKIT_VAM or write the path into local/vam.txt")
    else:
        env["VAMKIT_VAM"] = str(vam)
    for variable, path in tools().items():
        if path is None:
            missing.append(f"{variable}: {HINTS[variable]}")
        else:
            env[variable] = str(path)
    if missing:
        raise SystemExit("cannot build, missing:\n  " + "\n  ".join(missing))
    return env


# ---- doctor, status ---------------------------------------------------------------------------------------------

def doctor(vam):
    checks = []

    def check(name, ok, detail, need=True):
        checks.append(dict(name=name, ok=bool(ok), required=need, detail=detail))

    if vam is None:
        check("VaM folder", False, "give --vam, set VAMKIT_VAM or write the path into local/vam.txt")
    else:
        exe = vam / "VaM.exe"
        check("VaM.exe", exe.is_file(), f"{exe} (Unity {file_version(exe)})" if exe.is_file() else f"not found in {vam}")
        managed = vam / "VaM_Data" / "Managed" / "Assembly-CSharp.dll"
        check("VaM assemblies", managed.is_file(), str(managed.parent))
        assets = [vam / "VaM_Data" / n for n in ("globalgamemanagers.assets", "resources.assets")]
        check("VaM shader data (for the generators)", all(p.is_file() for p in assets), ", ".join(p.name for p in assets))
        bepinex = vam / "BepInEx" / "core" / "BepInEx.dll"
        version = file_version(bepinex) if bepinex.is_file() else None
        check("BepInEx 5", version is not None and version.startswith("5."), f"BepInEx {version}" if version else "BepInEx/core/BepInEx.dll missing")
        doorstop = vam / "winhttp.dll"
        check("UnityDoorstop", doorstop.is_file(), f"winhttp.dll {file_version(doorstop)}" if doorstop.is_file() else "winhttp.dll missing (BepInEx's loader)")
        openvr = vam / "VaM_Data" / "Plugins" / "openvr_api.dll"
        original = vam / "vamkit" / "original" / "openvr_api.dll"
        if original.is_file() and openvr.is_file():
            same = sha256(openvr) == sha256(original)
            check("openvr_api.dll is VaM's own", same, "matches the copy the install kept" if same else
                  "differs from the install's copy: a mod run did not finish; run launch.py --restore", need=False)
        stray = vam / "d3d11.dll"
        check("no d3d11.dll left beside VaM.exe", not stray.is_file() or (vam / "vamkit").is_dir(),
              "a d3d11.dll from something else is beside VaM.exe" if stray.is_file() and not (vam / "vamkit").is_dir() else "ok",
              need=False)
    gpu = run(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"])
    check("NVIDIA GPU and driver", "RTX" in gpu, gpu or "nvidia-smi found no NVIDIA GPU")
    check("Python 3.11+", sys.version_info >= (3, 11), sys.version.split()[0])
    found = tools()
    vs = visual_studio()
    check("Visual Studio C++ x64 tools (vcvars64.bat)", found["VAMKIT_VCVARS"], str(found["VAMKIT_VCVARS"] or HINTS["VAMKIT_VCVARS"]))
    cmake = shutil.which("cmake") or (vs and next(iter(glob.glob(str(vs / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"))), None))
    check("CMake", cmake, str(cmake) if cmake else "not on PATH and not in Visual Studio")
    atl = vs and glob.glob(str(vs / "VC/Tools/MSVC/*/atlmfc/include/atlbase.h"))
    check("ATL (OpenComposite needs it)", atl, "found" if atl else "add the Visual Studio component \"C++ ATL\", or give the build an atlmfc folder", need=False)
    check("Windows SDK fxc.exe", found["VAMKIT_FXC"], str(found["VAMKIT_FXC"] or HINTS["VAMKIT_FXC"]))
    check("Windows SDK dxc.exe", found["VAMKIT_DXC"], str(found["VAMKIT_DXC"] or HINTS["VAMKIT_DXC"]))
    check("C# 3.5 compiler", found["VAMKIT_CSC"], str(found["VAMKIT_CSC"] or HINTS["VAMKIT_CSC"]))
    check("git (for fetch)", shutil.which("git"), shutil.which("git") or "install Git for Windows")
    items = third_party()
    have = [i["id"] for i in items if fetched(i)]
    lacking = [i["id"] for i in items if not fetched(i) and not i.get("optional")]
    check("third-party downloads", not lacking, f"{len(have)} of {len(items)} in third_party/" + (f"; missing: {', '.join(lacking)} (vamkit fetch)" if lacking else ""), need=False)
    return checks


def status(vam):
    if vam is None:
        return dict(installed=False, detail="no VaM folder given")
    files = []
    for c in components():
        for _, target in c.get("installs", []):
            files.append(dict(component=c["id"], path=target, present=(vam / target).exists()))
    return dict(installed=(vam / "vamkit").is_dir(), vam=str(vam), files=files)


# ---- fetch ------------------------------------------------------------------------------------------------------

MARKER = ".vamkit-fetched.json"


def pin(item):
    return {k: item[k] for k in ("commit", "version", "sha256") if k in item}


def fetched(item):
    marker = THIRD / item["id"] / MARKER
    if not marker.is_file():
        return False
    try:
        return json.loads(marker.read_text(encoding="utf-8")) == pin(item)
    except ValueError:
        return False


def keep(source, dest, layout):
    """Copies what `layout` names from a download into third_party/<id>; the whole tree without one."""
    if dest.exists():
        shutil.rmtree(dest)
    if not layout:
        shutil.copytree(source, dest, ignore=shutil.ignore_patterns(".git"))
        return
    for src, dst in layout:
        s, d = source / src, dest / dst
        if not s.exists():
            raise SystemExit(f"{dest.name}: the download has no {src}")
        d.parent.mkdir(parents=True, exist_ok=True)
        if s.is_dir():
            shutil.copytree(s, d)
        else:
            shutil.copy2(s, d)


def git(args, cwd):
    done = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if done.returncode:
        raise SystemExit(f"git {' '.join(args)} failed:\n{done.stderr.strip()}")
    return done.stdout.strip()


def fetch_git(item, work):
    git(["init", "-q"], work)
    git(["remote", "add", "origin", item["url"]], work)
    if item.get("sparse"):
        git(["config", "core.sparseCheckout", "true"], work)
        (work / ".git" / "info").mkdir(exist_ok=True)
        (work / ".git" / "info" / "sparse-checkout").write_text("\n".join(item["sparse"]) + "\n", encoding="utf-8")
    git(["fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", item["commit"]], work)
    git(["checkout", "-q", "FETCH_HEAD"], work)
    head = git(["rev-parse", "HEAD"], work)
    if head != item["commit"]:
        raise SystemExit(f"{item['id']}: got commit {head}, the manifest pins {item['commit']}")


def download(url, to):
    request = urllib.request.Request(url, headers={"User-Agent": "vamkit-fetch"})
    with urllib.request.urlopen(request, timeout=120) as response, open(to, "wb") as f:
        shutil.copyfileobj(response, f)


def fetch_one(item, mirror, given):
    """Fills third_party/<id>. Returns a word for the report."""
    dest = THIRD / item["id"]
    if fetched(item):
        return "present"
    kind = item["kind"]
    if mirror is not None:
        source = mirror / item["id"]
        if not source.exists():
            return "not in the mirror"
        if kind in ("file", "user"):
            source = source / item["name"] if source.is_dir() else source
            return place_file(item, source)
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(source, dest, ignore=shutil.ignore_patterns(".git", MARKER))
    elif kind == "git":
        with tempfile.TemporaryDirectory() as tmp:
            fetch_git(item, Path(tmp))
            keep(Path(tmp), dest, item.get("layout"))
    elif kind == "nuget":
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package.zip"
            download(f"https://www.nuget.org/api/v2/package/{item['package']}/{item['version']}", package)
            with zipfile.ZipFile(package) as z:
                z.extractall(Path(tmp) / "x")
            keep(Path(tmp) / "x", dest, item.get("layout"))
    elif kind == "file":
        with tempfile.TemporaryDirectory() as tmp:
            got = Path(tmp) / item["name"]
            download(item["url"], got)
            return place_file(item, got)
    elif kind == "user":
        source = given.get(item["id"])
        if source is None:
            for pattern in item.get("find", []):
                source = next((Path(p) for p in glob.glob(pattern) if Path(p).is_file() and sha256(p) == item["sha256"]), None)
                if source:
                    break
        if source is None:
            return "not given" + (" (optional)" if item.get("optional") else "")
        return place_file(item, Path(source))
    else:
        raise SystemExit(f"{item['id']}: unknown kind {kind}")
    (dest / MARKER).write_text(json.dumps(pin(item)), encoding="utf-8")
    return "fetched"


def place_file(item, source):
    if not source.is_file():
        return f"no file at {source}"
    digest = sha256(source)
    if digest != item["sha256"]:
        return f"wrong file: sha256 {digest[:12]}..., the manifest pins {item['sha256'][:12]}..."
    dest = THIRD / item["id"]
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest / item["name"])
    (dest / MARKER).write_text(json.dumps(pin(item)), encoding="utf-8")
    return "fetched"


def fetch(args):
    items = [i for i in third_party() if not args.ids or i["id"] in args.ids]
    unknown = set(args.ids) - {i["id"] for i in items}
    if unknown:
        raise SystemExit("not in manifest/thirdparty.toml: " + ", ".join(sorted(unknown)))
    given = {}
    for pair in args.file or []:
        name, _, path = pair.partition("=")
        given[name] = Path(path)
    mirror = Path(args.mirror) if args.mirror else None
    need_terms = [i for i in items if i.get("terms") == "accept" and not fetched(i) and i["kind"] != "user"]
    if need_terms and mirror is None and not args.accept_licenses:
        print("These come under their vendor's licence. Read it, then run again with --accept-licenses:")
        for i in need_terms:
            print(f"  {i['id']:<14} {i['license']}  ({i.get('url', '')})")
        return 1
    THIRD.mkdir(exist_ok=True)
    failed = 0
    for i in items:
        try:
            result = fetch_one(i, mirror, given)
        except (OSError, urllib.error.URLError) as e:
            result = f"failed: {e}"
        ok = result in ("present", "fetched") or (i.get("optional") and result.startswith("not "))
        failed += not ok
        print(f"{'ok  ' if ok else 'FAIL'}  {i['id']:<14} {result}")
        if result.startswith("not given") and i.get("suggest"):
            print(f"      {i['suggest']}\n      then: vamkit fetch --file {i['id']}=<path to {i['name']}>")
    return 1 if failed else 0


# ---- build, test ------------------------------------------------------------------------------------------------

def selected(ids):
    all_ = components()
    unknown = set(ids) - {c["id"] for c in all_}
    if unknown:
        raise SystemExit("not in manifest/components.toml: " + ", ".join(sorted(unknown)))
    return [c for c in all_ if not ids or c["id"] in ids]


def step(name, argv, env, log, cwd=None):
    """Runs one build or test command at below-normal priority; its output goes to `log`. True when it exits 0."""
    log.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        code = subprocess.run([str(a) for a in argv], stdout=f, stderr=subprocess.STDOUT, env=env, cwd=cwd,
                              creationflags=BELOW_NORMAL).returncode
    print(f"{'ok  ' if code == 0 else 'FAIL'}  {name}  ({time.monotonic() - started:.0f} s)", flush=True)
    if code:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-25:]
        print("\n".join("      " + line for line in tail))
        print(f"      full log: {log}")
    return code == 0


def csc_arguments(c, vam, csc):
    m = c["managed"]
    folder = REPO / c["path"]
    out = OUT / c["id"]
    out.mkdir(parents=True, exist_ok=True)
    argv = [csc, "/nologo", "/target:library", "/optimize+", "/warn:4", "/codepage:65001", "/out:" + str(out / m["out"])]
    if m.get("unsafe"):
        argv.append("/unsafe")
    argv += ["/reference:" + str(vam / r) for r in m["references"]]
    argv += [f"/resource:{folder / f},{name}" for f, name in m.get("resources", [])]
    argv += [str(folder / s) for s in m["sources"]] + [str(REPO / g) for g in m.get("generated", [])]
    return argv


def missing_needs(c):
    """The third-party items this component's build reads that `vamkit fetch` has not put in third_party/ yet."""
    items = {i["id"]: i for i in third_party()}
    return [n for n in c.get("needs", []) if n not in items or not fetched(items[n])]


def needs_fetched(c):
    missing = missing_needs(c)
    if missing:
        print(f"FAIL  {c['id']:<16} needs {', '.join(missing)} in third_party/: python tools/vamkit fetch {' '.join(missing)}")
    return not missing


def build(args, vam):
    env = build_environment(vam)
    steps = set(args.steps.split(","))
    logs = OUT / "logs"
    for c in selected(args.ids):
        cid = c["id"]
        if args.clean and (OUT / cid).exists():
            shutil.rmtree(OUT / cid)
        if (("generate" in steps and c.get("generate")) or ("native" in steps and c.get("native"))) and not needs_fetched(c):
            return 1
        if "generate" in steps:
            for script in c.get("generate", []):
                if not step(f"{cid:<16} generate {Path(script).name}", [sys.executable, "-I", REPO / script], env,
                            logs / f"{cid}.generate.{Path(script).stem}.log"):
                    return 1
        if "native" in steps and c.get("native"):
            if not step(f"{cid:<16} native   {Path(c['native']).name}", ["cmd", "/c", REPO / c["native"]], env, logs / f"{cid}.native.log"):
                return 1
        if "managed" in steps and "managed" in c:
            if not step(f"{cid:<16} managed  {c['managed']['out']}", csc_arguments(c, vam, env["VAMKIT_CSC"]), env, logs / f"{cid}.managed.log"):
                return 1
    return 0


def expand(arg, env):
    """One word of a test command. A repo path is written with backslashes: csc 3.5 misreads a path that mixes both."""
    if "{repo}" in arg:
        head, _, tail = arg.partition("{repo}")
        arg = head + str(REPO) + tail.replace("/", "\\")
    return arg.replace("{python}", sys.executable).replace("{csc}", env["VAMKIT_CSC"])


def test(args, vam):
    env = build_environment(vam)
    failed = 0
    if not args.ids or "vamkit" in args.ids:   # the tool's own test: install, verify, uninstall, rollback on a made-up VaM folder
        script = REPO / "tools" / "vamkit" / "tests" / "install_test.py"
        failed += not step(f"{'vamkit':<16} test     {script.name}", [sys.executable, "-I", script], env, OUT / "logs" / "vamkit.test0.log")
    ids = [i for i in args.ids if i != "vamkit"]
    for c in selected(ids) if ids or not args.ids else []:
        if c.get("tests") and not needs_fetched(c):
            failed += 1
            continue
        for n, command in enumerate(c.get("tests", [])):
            argv = [expand(a, env) for a in command]
            shown = " ".join(Path(a).name if "{repo}" in b else a for a, b in zip(argv, command))
            if not step(f"{c['id']:<16} test     {shown}", argv, env, OUT / "logs" / f"{c['id']}.test{n}.log"):
                failed += 1
                break
    print(f"{failed} component(s) with a failing test" if failed else "all tests passed")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(prog="vamkit", description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=("doctor", "status", "fetch", "build", "test", "install", "verify", "uninstall", "rollback"))
    ap.add_argument("ids", nargs="*", help="component ids (build, test) or third-party ids (fetch); all when omitted")
    ap.add_argument("--vam")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--steps", default="generate,native,managed", help="build: which steps to run")
    ap.add_argument("--clean", action="store_true", help="build: delete out/<id> first")
    ap.add_argument("--accept-licenses", action="store_true", help="fetch: you have read the vendors' licences")
    ap.add_argument("--mirror", help="fetch: take everything from this folder (<DIR>/<id>/...) instead of the network")
    ap.add_argument("--file", action="append", metavar="ID=PATH", help="fetch: your own copy of a runtime that has no download")
    ap.add_argument("--yes", action="store_true", help="install, uninstall, rollback: do it (without it the plan is printed and nothing changes)")
    ap.add_argument("--replace", action="store_true", help="install: also over files of the same names that vamkit did not install (they are kept)")
    ap.add_argument("--purge", action="store_true", help="uninstall: also remove the runs' logs, the launch presets and the plugins' settings")
    args = ap.parse_args()
    vam = vam_folder(args.vam)
    if args.command in ("install", "verify", "uninstall", "rollback"):
        if vam is None:
            print("which VaM folder? --vam <folder>, VAMKIT_VAM, or one line in local/vam.txt", file=sys.stderr)
            return 1
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import install as kit_install
        if args.command == "uninstall":
            return kit_install.uninstall(vam, args.yes, args.purge)
        if args.command == "rollback":
            return kit_install.rollback(vam, args.yes)
        kit = kit_install.Kit(REPO, components(), third_party(), fetched)
        if args.command == "verify":
            return kit_install.verify(kit, vam, args.json)
        return kit_install.install(kit, vam, args.yes, args.json, args.replace)
    if args.command == "doctor":
        checks = doctor(vam)
        if args.json:
            print(json.dumps(checks, indent=2))
        else:
            for c in checks:
                mark = "ok  " if c["ok"] else ("FAIL" if c["required"] else "note")
                print(f"{mark}  {c['name']}: {c['detail']}")
        return 0 if all(c["ok"] or not c["required"] for c in checks) else 1
    if args.command == "fetch":
        return fetch(args)
    if args.command == "build":
        return build(args, vam)
    if args.command == "test":
        return test(args, vam)
    result = status(vam)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(("installed in " if result["installed"] else "not installed in ") + str(result.get("vam")))
        for f in result.get("files", []):
            print(f"  {'present' if f['present'] else 'missing'}  {f['path']}  ({f['component']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
