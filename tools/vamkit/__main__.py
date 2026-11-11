"""vamkit: check, fetch, build and test the mod for one VaM folder. Python standard library only.

  python tools/vamkit doctor                         what is there and what is missing; changes nothing
  python tools/vamkit fetch  [ids] [--accept-licenses] [--mirror DIR] [--file ID=PATH]
                                                     third-party SDKs and runtimes into third_party/
  python tools/vamkit build  [ids] [--steps generate,native,managed] [--clean]
                                                     every component into out/<id>/, in the manifest's order
  python tools/vamkit fetch --lab ... / build --lab  the same for the test tools under tools/lab (the simulated headset)
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
import re
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
        how = "python tools/vamkit fetch bepinex, then python tools/vamkit bepinex --yes puts it in (or install BepInEx 5 x64 yourself)"
        check("BepInEx 5", version is not None and version.startswith("5."), f"BepInEx {version}" if version else f"BepInEx/core/BepInEx.dll missing: {how}")
        doorstop = vam / "winhttp.dll"
        check("UnityDoorstop", doorstop.is_file(), f"winhttp.dll {file_version(doorstop)}" if doorstop.is_file() else "winhttp.dll missing (BepInEx's loader; it comes with BepInEx)")
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
    items = [i for i in third_party() if i.get("only") != "lab"]
    have = [i["id"] for i in items if fetched(i)]
    lacking = [i["id"] for i in items if not fetched(i) and not i.get("optional")]
    check("third-party downloads", not lacking, f"{len(have)} of {len(items)} in third_party/" + (f"; missing: {', '.join(lacking)} (vamkit fetch)" if lacking else ""), need=False)
    for r in runtimes():   # each run-time file by name: what is there, and what the user does not get without it
        if r["present"]:
            detail = f"version {r['version']}" + ("" if r["developed_with"] else ", not the version this project was developed with")
        else:
            detail = f"MISSING. You do not get: {r['without']}. To add it: {r['how']}"
        check(f"run-time file {r['name']}", r["present"], detail, need=False)
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


def marker_of(item):
    try:
        return json.loads((THIRD / item["id"] / MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def fetched(item):
    """Is this item in third_party/, for this manifest's pin? A run-time file of another version that was taken for
    its signature counts: the marker then also says which one it is (`got`)."""
    marker = marker_of(item)
    return marker is not None and {k: marker.get(k) for k in pin(item)} == pin(item)


def signature(path):
    """(signer's subject or None, file version) of a Windows binary: None unless its Authenticode signature is valid."""
    script = ("$s = Get-AuthenticodeSignature -LiteralPath $env:VAMKIT_FILE; $v = (Get-Item -LiteralPath $env:VAMKIT_FILE).VersionInfo; "
              "@{ok = ($s.Status -eq 'Valid'); subject = [string]$s.SignerCertificate.Subject; "
              "version = ('{0}.{1}.{2}.{3}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart)} | ConvertTo-Json -Compress")
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True, text=True,
                              env=dict(os.environ, VAMKIT_FILE=str(path)), timeout=60)
        found = json.loads(done.stdout.strip().splitlines()[-1])
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
        return None, ""
    return (found.get("subject") if found.get("ok") else None), found.get("version", "")


def numbers(version):
    return tuple(int(x) for x in re.findall(r"\d+", version)[:4])


def usable(item, path):
    """Can this file serve as the item's run-time file? (word, version, sha256): word is "developed-with" for the
    manifest's own hash, "signed" for another version with the vendor's valid signature, None with the reason in
    `version`'s place otherwise."""
    digest = sha256(path)
    if digest == item.get("sha256"):
        return "developed-with", "", digest
    if not item.get("signer"):
        return None, f"sha256 {digest[:12]}..., the manifest pins {item['sha256'][:12]}...", digest
    subject, version = signature(path)
    if subject is None or item["signer"].lower() not in subject.lower():
        return None, f"not a file with a valid signature of {item['signer']}", digest
    if item.get("min_version") and numbers(version) < numbers(item["min_version"]):
        return None, f"version {version} is older than {item['min_version']}, the oldest this project can use", digest
    return "signed", version, digest


def find_runtime(item):
    """The best file among the places the manifest names (`find`): the developed-with version when it is there, else
    the newest one the vendor signed."""
    candidates = [Path(p) for pattern in item.get("find", []) for p in glob.glob(pattern) if Path(p).is_file()]
    best = None
    for path in candidates:
        if sha256(path) == item.get("sha256"):
            return path
    for path in candidates:
        word, version, _ = usable(item, path)
        if word == "signed" and (best is None or numbers(version) > best[0]):
            best = (numbers(version), path)
    return best[1] if best else None


def how_to_add(item):
    if item["kind"] == "user":
        where = ("fetch looks in " + ", ".join(item["find"]) + ". " if item.get("find") else "")
        return f"{where}{item.get('suggest', '')} Then: python tools/vamkit fetch --file {item['id']}=<path to {item['name']}>".strip()
    return f"python tools/vamkit fetch {item['id']}" + (" --accept-licenses" if item.get("terms") == "accept" else "") + \
        (f" (downloads {item['url']}); or give a copy: --file {item['id']}=<path>" if item.get("url") else "")


def runtimes():
    """Every run-time file the kit installs: is it in third_party/, which version, and when it is missing what the
    user does not get and how to add it. Used by fetch, doctor, install and verify: nothing missing goes unsaid."""
    found = []
    for item in third_party():
        if not item.get("installs"):
            continue
        marker = marker_of(item) if fetched(item) else None
        got = (marker or {}).get("got")
        found.append(dict(id=item["id"], name=item["name"], present=marker is not None,
                          version=(got or {}).get("version") or (re.search(r"\d+\.\d+\.\d+", item["what"]) or [""])[0],
                          developed_with=marker is not None and not got,
                          without=item.get("without", item["what"]), how=how_to_add(item), optional=bool(item.get("optional"))))
    return found


def say_missing(found, indent="  "):
    """The lines for the run-time files that are missing."""
    lines = []
    for r in found:
        if not r["present"]:
            lines += [f"{indent}MISSING  {r['name']}  ({r['id']})", f"{indent}         you do not get: {r['without']}", f"{indent}         to add it: {r['how']}"]
    return lines


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
        if given.get(item["id"]) is not None:
            return place_file(item, Path(given[item["id"]]))
        try:
            with tempfile.TemporaryDirectory() as tmp:
                got = Path(tmp) / item["name"]
                download(item["url"], got)
                return place_file(item, got)
        except (OSError, urllib.error.URLError) as e:   # no network: is a copy of the vendor's on this machine?
            source = find_runtime(item)
            if source is None:
                raise
            return place_file(item, source) + f" (the download failed: {e})"
    elif kind == "user":
        source = given.get(item["id"]) or find_runtime(item)
        if source is None:
            return "not given"
        return place_file(item, Path(source))
    else:
        raise SystemExit(f"{item['id']}: unknown kind {kind}")
    (dest / MARKER).write_text(json.dumps(pin(item)), encoding="utf-8")
    return "fetched"


def place_file(item, source):
    if not source.is_file():
        return f"no file at {source}"
    word, version, digest = usable(item, source)
    if word is None:
        return f"cannot use {source}: {version}"
    dest = THIRD / item["id"]
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest / item["name"])
    marker = pin(item) if word == "developed-with" else dict(pin(item), got=dict(sha256=digest, version=version, source=str(source)))
    (dest / MARKER).write_text(json.dumps(marker), encoding="utf-8")
    if word == "developed-with":
        return "fetched"
    return f"fetched: version {version}, signed by {item['signer']}, not the version this project was developed with (models and cost can differ)"


def fetch(args):
    # the items of the lab tools (only = "lab") are fetched when asked for: --lab, or by id
    items = [i for i in third_party() if ((i["id"] in args.ids) if args.ids else (args.lab or i.get("only") != "lab"))]
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
        got = result == "present" or result.startswith("fetched")
        failed += not got and not i.get("optional")
        word = "ok  " if got else ("left out" if i.get("optional") else "FAIL")
        print(f"{word}  {i['id']:<14} {result}")
    # A run-time file that is not there is said, with what it costs and how to get it: also an optional one.
    missing = say_missing([r for r in runtimes() if r["id"] in {i["id"] for i in items}], indent="")
    if missing:
        print("\nThe kit builds and installs without these, and the user gets less:")
        print("\n".join(missing))
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


# Compiled into every plugin that references Harmony: one source for the Harmony 2.0 of BepInEx 5.0-5.3 and the
# HarmonyX of BepInEx 5.4 (the file says how).
HARMONY, HARMONY_COMPAT = "BepInEx/core/0Harmony.dll", "components/shared/harmony-compat/HarmonyCompat.cs"


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
    argv += [str(REPO / "components" / "shared" / s) for s in m.get("shared", [])]
    if HARMONY in m["references"]:
        argv.append(str(REPO / HARMONY_COMPAT))
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


# The lab's own builds: name, build script, the third-party items it reads. Not components: nothing of theirs is installed.
LAB = (("simxr-runtime", "tools/lab/simxr-runtime/build.cmd", ("libovr", "vdxr-meta", "vulkan-sdk", "wil", "gameinput", "detours", "ngx-sdk")),)


def build_lab(env):
    """The test tools that need building: the simulated headset (out/lab/simxr)."""
    for name, script, needs in LAB:
        if not needs_fetched(dict(id=name, needs=list(needs))):
            print("      (python tools/vamkit fetch --lab --accept-licenses fetches the lab tools' items)")
            return 1
        if not step(f"{name:<16} lab      {Path(script).name}", ["cmd", "/c", REPO / script], env, OUT / "logs" / f"{name}.lab.log"):
            return 1
    return 0


def build(args, vam):
    env = build_environment(vam)
    if args.lab:
        return build_lab(env)
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
        (OUT / c["id"]).mkdir(parents=True, exist_ok=True)   # a test may compile into it before any build has
        for n, command in enumerate(c.get("tests", [])):
            argv = [expand(a, env) for a in command]
            shown = " ".join(Path(a).name if "{repo}" in b else a for a, b in zip(argv, command))
            if not step(f"{c['id']:<16} test     {shown}", argv, env, OUT / "logs" / f"{c['id']}.test{n}.log"):
                failed += 1
                break
    print(f"{failed} component(s) with a failing test" if failed else "all tests passed")
    return 1 if failed else 0


def main():
    # A build log can hold bytes the console's code page has no character for; printing its tail must never hide it.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(prog="vamkit", description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=("doctor", "status", "fetch", "bepinex", "build", "test", "install", "verify", "uninstall", "rollback"))
    ap.add_argument("ids", nargs="*", help="component ids (build, test) or third-party ids (fetch); all when omitted")
    ap.add_argument("--vam")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--steps", default="generate,native,managed", help="build: which steps to run")
    ap.add_argument("--clean", action="store_true", help="build: delete out/<id> first")
    ap.add_argument("--lab", action="store_true", help="fetch: also the lab tools' items; build: the lab tools instead of the components (the simulated headset)")
    ap.add_argument("--accept-licenses", action="store_true", help="fetch: you have read the vendors' licences")
    ap.add_argument("--mirror", help="fetch: take everything from this folder (<DIR>/<id>/...) instead of the network")
    ap.add_argument("--file", action="append", metavar="ID=PATH", help="fetch: your own copy of a runtime that has no download")
    ap.add_argument("--yes", action="store_true", help="install, uninstall, rollback: do it (without it the plan is printed and nothing changes)")
    ap.add_argument("--replace", action="store_true", help="install: also over files of the same names that vamkit did not install (they are kept)")
    ap.add_argument("--purge", action="store_true", help="uninstall: also remove the runs' logs, the launch presets, the plugins' settings and a BepInEx that `vamkit bepinex` put in")
    ap.add_argument("--remove", action="store_true", help="bepinex: take out the BepInEx that `vamkit bepinex` put in")
    args = ap.parse_args()
    vam = vam_folder(args.vam)
    if args.command in ("bepinex", "install", "verify", "uninstall", "rollback"):
        if vam is None:
            print("which VaM folder? --vam <folder>, VAMKIT_VAM, or one line in local/vam.txt", file=sys.stderr)
            return 1
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import install as kit_install
        if args.command == "bepinex":   # the loader the plugins run in, for a VaM folder that has none; before `build`
            import bepinex
            if args.remove:
                return bepinex.remove(vam, args.yes)
            item = next(i for i in third_party() if i["id"] == "bepinex")
            return bepinex.put(vam, THIRD / "bepinex" / item["name"] if fetched(item) else None, item["version"], args.yes)
        if args.command == "uninstall":
            return kit_install.uninstall(vam, args.yes, args.purge)
        if args.command == "rollback":
            return kit_install.rollback(vam, args.yes)
        kit = kit_install.Kit(REPO, components(), third_party(), fetched, runtimes)
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
