"""A copy of a VaM installation's program files, without any content: a place to try an install without touching the
VaM someone uses.

Copied: VaM.exe and the other files beside it (its settings among them), VaM_Data, Mono, Plugins, Assets, Keys, and
BepInEx's core with its Doorstop files. Not copied: scenes, looks, packages and caches (Saves, Custom, AddonPackages,
Cache, ...), BepInEx's plugins and their settings, injected DLLs beside VaM.exe (d3d11.dll, dxgi.dll, ...), logs and
archives. VaM's built-in content (its default scene and person) comes with VaM_Data, so the copy starts on its own.
The source is only read. About 17 GB for VaM 1.22.

    python tools/lab/session/trial_copy.py --from <VaM folder> --to <new folder>
"""
import argparse
import shutil
import sys
import time
from pathlib import Path

FOLDERS = ("VaM_Data", "Mono", "MonoBleedingEdge", "Plugins", "Assets", "Keys")
# DLLs that are not VaM's: injectors that Windows loads from the game's folder, and a profiler runtime a developer may have put there.
INJECTED = {"d3d11.dll", "dxgi.dll", "d3d9.dll", "d3d12.dll", "dinput8.dll", "opengl32.dll", "version.dll", "winmm.dll", "dsound.dll",
            "winpixeventruntime.dll"}
SKIPPED_TYPES = {".log", ".rar", ".zip", ".7z", ".md", ".obj", ".mtl", ".bat", ".lnk", ".tmp"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from", dest="source", required=True)
    ap.add_argument("--to", dest="target", required=True)
    args = ap.parse_args()
    source, target = Path(args.source).resolve(), Path(args.target).resolve()
    if not (source / "VaM.exe").is_file():
        sys.exit(f"{source} has no VaM.exe")
    if target.exists() and any(target.iterdir()):
        sys.exit(f"{target} exists and is not empty")
    if source in target.parents or source == target:
        sys.exit("the copy must not lie inside the VaM folder it is made from")
    started, count, size = time.monotonic(), 0, 0
    target.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.iterdir()):
        if path.is_file() and path.name.lower() not in INJECTED and path.suffix.lower() not in SKIPPED_TYPES:
            shutil.copy2(path, target / path.name)
            count, size = count + 1, size + path.stat().st_size
    for name in FOLDERS + ("BepInEx/core",):
        if (source / name).is_dir():
            shutil.copytree(source / name, target / name)
            files = [p for p in (target / name).rglob("*") if p.is_file()]
            count, size = count + len(files), size + sum(p.stat().st_size for p in files)
    for name in ("Saves", "Custom", "AddonPackages", "BepInEx/plugins"):
        (target / name).mkdir(parents=True, exist_ok=True)
    print(f"{count} files, {size / 2**30:.1f} GB copied to {target} in {time.monotonic() - started:.0f} s")
    missing = [n for n in ("VaM.exe", "UnityPlayer.dll", "VaM_Data/Managed/Assembly-CSharp.dll", "BepInEx/core/BepInEx.dll", "winhttp.dll") if not (target / n).is_file()]
    if missing:
        print("not in the source, so not in the copy: " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
