"""Where the NVIDIA run-time libraries of a launch come from.

The kit's own copy first (`<VaM>/vamkit/bin/dlss/...`, put there by `vamkit install`). Where the kit lacks one, the copy
the graphics driver keeps in its model store is used instead: an NVIDIA driver downloads DLSS, Ray Reconstruction and
Frame Generation libraries into `%ProgramData%/NVIDIA/NGX/models/<feature>/versions/<number>/files/` as `.bin` files,
which are the DLLs under another name. A missing file in the kit is therefore not a reason to go without the feature
on a card that can run it. What cannot be found at all is reported with what is lost and how to get it, never skipped
in silence.

    locate(vam)                 what each library would come from; nothing is copied (the panel's summary)
    prepare(vam, log=print)     the folders for this launch; driver copies are verified and copied into the kit's cache
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess

# The release the mod was built and run against. Used first where the driver has it; else the newest the driver has.
TESTED = (310, 9, 16)
# what: (folder under bin/dlss, file name, the driver store's folder, what it is for, what is lost without it)
LIBRARIES = {
    "sr": ("sr", "nvngx_dlss.dll", "dlss", "DLSS super resolution (models K / L / M)",
           "XeSS or FSR upscale in its place"),
    "rr": ("sr", "nvngx_dlssd.dll", "dlssd", "DLSS Ray Reconstruction", "path tracing cannot be shown"),
    "fg": ("fg", "nvngx_dlssg.dll", "dlssg", "DLSS Frame Generation (up to 4x)",
           "FSR frame generation (2x) stands in"),
}
REMEDY = ("Get it with `python tools/vamkit fetch` in the vamkit repository and install again, or update the NVIDIA "
          "driver and start a game that uses the feature once (the driver then downloads it).")


def store():
    return Path(os.environ.get("ProgramData", "C:/ProgramData")) / "NVIDIA" / "NGX" / "models"


def version_of(number):
    """The driver names a version folder major << 16 | minor << 8 | patch (20318480 is 310.9.16)."""
    return number >> 16, (number >> 8) & 255, number & 255


def in_store(what):
    """[(version, path)] of the driver's copies of one library: the tested release first, then newest first."""
    found = []
    try:
        for folder in (store() / LIBRARIES[what][2] / "versions").iterdir():
            if not folder.name.isdigit():
                continue
            files = sorted((folder / "files").glob("*.bin"))
            if files:
                found.append((version_of(int(folder.name)), files[0]))
    except OSError:
        return []
    found.sort(key=lambda entry: (entry[0] != TESTED, [-part for part in entry[0]]))
    return found


def signed_by_nvidia(path):
    """Authenticode: a valid signature whose signer is NVIDIA Corporation."""
    command = ("$s = Get-AuthenticodeSignature -LiteralPath '" + str(path).replace("'", "''") + "'; "
               "if ($s.Status -eq 'Valid' -and $s.SignerCertificate.Subject -match 'O=NVIDIA Corporation') { 'yes' } else { 'no' }")
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True,
                              text=True, timeout=30, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.SubprocessError):
        return False
    return done.stdout.strip() == "yes"


def locate(vam):
    """{what: {"source": "kit" | "driver" | "missing", "path", "version"}}: nothing is copied or verified."""
    kit = Path(vam) / "vamkit" / "bin" / "dlss"
    report = {}
    for what, (folder, name, _, _, _) in LIBRARIES.items():
        own = kit / folder / name
        if own.is_file():
            report[what] = dict(source="kit", path=str(own), version=None)
            continue
        driver = in_store(what)
        if driver:
            report[what] = dict(source="driver", path=str(driver[0][1]), version=".".join(map(str, driver[0][0])))
        else:
            report[what] = dict(source="missing", path=None, version=None)
    return report


def describe(report, wanted=("sr", "rr", "fg")):
    """Lines for a person: only what is not simply the kit's own file."""
    lines = []
    for what in wanted:
        entry = report[what]
        _, name, _, purpose, lost = LIBRARIES[what]
        if entry["source"] == "driver":
            lines.append(f"{purpose}: {name} is not in the kit; the graphics driver's copy {entry['version']} is used.")
        elif entry["source"] == "missing":
            lines.append(f"{purpose}: {name} was found neither in the kit nor in the graphics driver's model store, so "
                         f"{lost}. {REMEDY}")
    return lines


def prepare(vam, log=print):
    """(folder with nvngx_dlss.dll and nvngx_dlssd.dll, folder with nvngx_dlssg.dll, report) for this launch.

    A library the kit lacks is copied from the driver's store into `<VaM>/vamkit/cache/ngx/<folder>/` under its DLL
    name, after its signature was checked, together with the kit's own files of that folder; later launches reuse the
    copy as long as the driver's file is the same."""
    vam = Path(vam)
    kit = vam / "vamkit" / "bin" / "dlss"
    cache = vam / "vamkit" / "cache" / "ngx"
    report = locate(vam)
    folders = {}
    for folder in ("sr", "fg"):
        members = [what for what, entry in LIBRARIES.items() if entry[0] == folder]
        if all(report[what]["source"] != "driver" for what in members):
            folders[folder] = kit / folder        # the kit's own, complete or not: what is missing is in the report
            continue
        target = cache / folder
        target.mkdir(parents=True, exist_ok=True)
        marker = target / "source.json"
        try:
            known = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            known = {}
        for what in members:
            name = LIBRARIES[what][1]
            entry = report[what]
            if entry["source"] == "kit":
                if not (target / name).is_file() or (target / name).stat().st_size != Path(entry["path"]).stat().st_size:
                    shutil.copy2(entry["path"], target / name)
            elif entry["source"] == "driver":
                source = Path(entry["path"])
                stamp = [str(source), source.stat().st_size, int(source.stat().st_mtime)]
                if known.get(name) != stamp or not (target / name).is_file():
                    if not signed_by_nvidia(source):
                        log(f"{name}: the graphics driver's copy {entry['version']} has no valid NVIDIA signature; not used")
                        report[what] = dict(source="missing", path=None, version=None)
                        (target / name).unlink(missing_ok=True)
                        known.pop(name, None)
                        continue
                    shutil.copy2(source, target / name)
                    known[name] = stamp
            else:
                (target / name).unlink(missing_ok=True)
        marker.write_text(json.dumps(known, indent=1), encoding="utf-8")
        folders[folder] = target
    for line in describe(report):
        log(line)
    return folders["sr"], folders["fg"], report
