"""`vamkit bepinex`: puts BepInEx into a VaM folder that has none, and takes it out again.

Every plugin of the kit is a BepInEx plugin and is compiled against the BepInEx that is in the VaM folder, so this
step comes before `build`. The archive is the one `vamkit fetch` brought (third_party/bepinex, from BepInEx's own
release page, checked by hash). A BepInEx that is already in the folder is never touched, whatever its version.

What is put in is recorded in <VaM>/vamkit/bepinex.json (every file with its hash). `vamkit bepinex --remove` and
`vamkit uninstall --purge` take out the files that are still as they were put in; what BepInEx or the user added
since (its config, its log, other plugins) stays and is named.
"""
import datetime
import json
from pathlib import Path
import sys
import zipfile

import install as kit_install

RECORD = "vamkit/bepinex.json"
CORE = "BepInEx/core/BepInEx.dll"


def present(vam):
    return (Path(vam) / CORE).is_file()


def read_record(vam):
    try:
        return json.loads((Path(vam) / RECORD).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def members(archive):
    """The archive's files as (path inside the VaM folder, zip entry). A name that would leave the folder is an error."""
    found = []
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename.replace("\\", "/")
            if info.is_dir():
                continue
            parts = name.split("/")
            if name.startswith("/") or ":" in name or ".." in parts or not all(parts):
                raise ValueError(f"the archive holds a name that is not a plain relative path: {info.filename!r}")
            found.append((name, info))
    return found


def put(vam, archive, version, yes):
    """BepInEx from `archive` into `vam`. Prints the plan; changes nothing without `yes`. Returns the exit code."""
    vam = Path(vam)
    if not (vam / "VaM.exe").is_file():
        print(f"no VaM.exe in {vam}", file=sys.stderr)
        return 1
    if present(vam):
        print(f"BepInEx is in {vam} already: nothing to do (the kit builds against the one that is there)")
        return 0
    if archive is None or not Path(archive).is_file():
        print("the BepInEx archive is not in third_party/ yet: python tools/vamkit fetch bepinex", file=sys.stderr)
        return 1
    files = members(archive)
    with zipfile.ZipFile(archive) as z:
        same = [name for name, info in files if (vam / name).is_file() and (vam / name).read_bytes() == z.read(info)]
    clashes = [name for name, _ in files if (vam / name).exists() and name not in same]
    print(f"BepInEx {version} into {vam}")
    print(f"  {len(files) - len(same)} file(s) from {Path(archive).name}: BepInEx/core/, winhttp.dll (its loader), doorstop_config.ini")
    for name in same:
        print(f"  there    {name}  (the same file already)")
    if clashes:
        for name in clashes:
            print(f"  CLASH    {name}  (a different file of this name is there: another loader, or the rest of an older BepInEx)")
        print("nothing was changed: move those files away, or install BepInEx 5 (x64) into this folder yourself", file=sys.stderr)
        return 1
    if not yes:
        print("nothing was changed. Run again with --yes.")
        return 0
    if kit_install.vam_running(vam):
        print("VaM is running from this folder: close it first", file=sys.stderr)
        return 1
    written = []
    with zipfile.ZipFile(archive) as z:
        for name, info in files:
            if name in same:
                continue
            target = vam / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(z.read(info))
            written.append(dict(path=name, sha256=kit_install.sha256(target)))
    record = vam / RECORD
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(json.dumps(dict(version=version, archive=Path(archive).name, time=datetime.datetime.now().isoformat(timespec="seconds"),
                                      files=written), indent=1) + "\n", encoding="utf-8")
    print(f"BepInEx {version} is in: {len(written)} file(s), recorded in {RECORD}. It makes its config and log at VaM's next start.")
    print("to take it out again: python tools/vamkit bepinex --remove --yes")
    return 0


def remove_plan(vam):
    """(record, the files still as put in, the ones changed since) or None when vamkit put no BepInEx in."""
    record = read_record(vam)
    if record is None:
        return None
    vam = Path(vam)
    there = [f for f in record.get("files", []) if (vam / f["path"]).is_file()]
    intact = [f for f in there if kit_install.sha256(vam / f["path"]) == f["sha256"]]
    return record, intact, [f for f in there if f not in intact]


def remove(vam, yes, quiet=False):
    """Takes out the BepInEx that `put` recorded. Returns the exit code."""
    vam = Path(vam)
    plan = remove_plan(vam)
    if plan is None:
        print(f"vamkit put no BepInEx into {vam} (no {RECORD}): nothing to take out")
        return 0
    record, intact, changed = plan
    if not quiet:
        print(f"BepInEx {record.get('version', '')} out of {vam}")
        print(f"  {len(intact)} file(s) removed, as they were put in")
        for f in changed:
            print(f"  left     {f['path']}  (changed since)")
        print("  left     what BepInEx or you added since: BepInEx/config, BepInEx/plugins, its log")
    if not yes:
        print("nothing was changed. Run again with --yes.")
        return 0
    if kit_install.vam_running(vam):
        print("VaM is running from this folder: close it first", file=sys.stderr)
        return 1
    for f in intact:
        (vam / f["path"]).unlink()
    (vam / RECORD).unlink(missing_ok=True)
    for folder in sorted({(vam / f["path"]).parent for f in intact}, key=lambda p: -len(p.parts)):
        while folder != vam and folder.is_dir() and not any(folder.iterdir()):   # only folders that are empty now
            folder.rmdir()
            folder = folder.parent
    if not quiet:
        print("BepInEx is out" + ("" if present(vam) or not (vam / "BepInEx").exists() else ": the BepInEx folder stays with what was added since"))
    return 0
