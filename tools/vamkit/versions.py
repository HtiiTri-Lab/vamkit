"""What this machine has of everything the kit depends on, against the machine the kit was made on.

  python tools/vamkit versions [--vam <VaM>]      every version, marked where it differs from the reference machine
  python tools/vamkit versions --json             the same as JSON: to attach to a report, or to save as a reference
  python tools/vamkit versions --against FILE     against another machine's JSON instead of the reference

The reference (docs/agent/reference-machine.json, explained in docs/agent/reference-machine.md) is the one machine
everything in this repository was built, run and measured on. A line that differs is not an error and not a reason to
stop: it is a part of this machine nobody has run the kit with, and the place to look first when something behaves
differently. Changes nothing. Records versions and model names only: no paths, user or machine names, serial numbers.
"""
from __future__ import annotations

import datetime
import glob
import json
import os
from pathlib import Path
import re
import shutil
import sys
import winreg

REFERENCE = Path(__file__).resolve().parents[2] / "docs" / "agent" / "reference-machine.json"
HKLM, HKCU = winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER

# The system files the kit's native code runs on: Unity's renderer through 11on12, the compositor, the path tracer and
# frame generation on D3D12, the desktop presenter on DXGI, NVIDIA's NGX through the driver.
SYSTEM_FILES = ("d3d11.dll", "d3d11on12.dll", "d3d12.dll", "D3D12Core.dll", "dxgi.dll", "dxcore.dll", "d3dcompiler_47.dll",
                "nvapi64.dll", "vcruntime140.dll", "msvcp140.dll")

# What a difference in a line has meant so far, or what it touches, and where to read. A line without a note has not
# mattered yet as far as anyone knows, which is not the same as not mattering.
NOTES = {
    ("Windows", "release"): "The kit was made on Windows 11 only. Windows 10 22H2 is the first other release reported "
                            "(2026-11-12): its d3d11on12.dll stopped the GPU at the first upscaled frame. Expect differences in "
                            "D3D11On12, DXGI presentation and the driver model; docs/agent/reference-machine.md",
    ("Windows", "d3d11on12.dll"): "Before Microsoft's fix an unwrap that names 11on12's own queue stops it for good; the proxy "
                                  "tests this DLL at every start (d3d11_proxy.log, line 'on12: d3d11on12.dll ...'). By hand: "
                                  "<VaM>/vamkit/bin/unwrap_probe.exe own. docs/knowledge/d3d11on12.md, 'Unwrap / return'",
    ("Windows", "d3d11.dll"): "Unity renders through it, and through 11on12 under the kit; docs/knowledge/d3d11on12.md",
    ("Windows", "d3d12.dll"): "The compositor, the path tracer, ray reconstruction and frame generation run on D3D12",
    ("Windows", "D3D12Core.dll"): "The D3D12 runtime itself (the kit ships no Agility SDK); ray tracing and enhanced barriers "
                                  "depend on it",
    ("Windows", "dxgi.dll"): "The desktop presenter's swap chain and latency waits; docs/knowledge/frame-generation.md",
    ("Windows", "hardware GPU scheduling"): "DLSS frame generation needs it on; docs/knowledge/frame-generation.md",
    ("Windows", "DirectX user settings"): "Windows' graphics settings (windowed game optimizations, auto HDR, VRR) change how "
                                          "the desktop presenter's frames reach the screen",
    ("Graphics", "GPU"): "RTX 50 runs DLSS-G at more than 2x, RTX 40 at 2x, older RTX without DLSS-G (FSR frame generation "
                         "instead); without RTX there is no ray reconstruction, so path tracing switches off and XeSS or FSR "
                         "upscale. Only an RTX 5090 has run the kit; docs/knowledge/upscalers-other-vendors.md",
    ("Graphics", "driver"): "NGX (DLSS, RR, DLSS-G) is part of the driver; docs/agent/runtimes.md",
    ("Graphics", "NGX core"): "The driver's DLSS loader; the run-time files below are loaded through it",
    ("Graphics", "displays"): "Frame generation holds real frames at refresh / multiplier on the desktop",
    ("VR", "OpenXR runtime"): "VaM's OpenVR calls go through OpenComposite to this runtime. The development tree ran with "
                              "Virtual Desktop (Quest 3), this repository's build only on the simulated headset (tools/lab); "
                              "SteamVR, Meta's, Pimax's and the others are unexplored. docs/agent/runtimes.md",
    ("VaM", "Unity"): "Every native part is bound to Unity 2018.1's D3D11 renderer; docs/agent/adapt-vam-version.md",
    ("VaM", "mono.dll"): "gc-tuning patches bytes in it and steps aside when they differ; docs/knowledge/gc.md",
    ("VaM", "BepInEx"): "The plugins build against the BepInEx in the VaM folder; docs/agent/adapt-vam-version.md",
    ("VaM", "Harmony"): "Harmony 2.0 (BepInEx 5.0-5.3) or HarmonyX (5.4): components/shared/harmony-compat",
    ("Hardware", "CPU"): "cpu-boost, the hair and cloth solvers place threads by CCD; made on a two-CCD Ryzen X3D. "
                         "docs/knowledge/cpu.md",
    ("Build tools", "MSVC"): "Build only; a newer toolset may warn where this one did not",
}

INFO_ONLY = {("Kit", "commit"), ("Kit", "captured")}


def reg(root, key, name):
    try:
        with winreg.OpenKey(root, key) as k:
            return winreg.QueryValueEx(k, name)[0]
    except OSError:
        return None


def cim(run):
    """GPUs, displays, CPU cores and memory from WMI, in one PowerShell call."""
    script = ("$g=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,CurrentHorizontalResolution,"
              "CurrentVerticalResolution,CurrentRefreshRate);$p=@(Get-CimInstance Win32_Processor | Select-Object NumberOfCores);"
              "$m=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory;"
              "ConvertTo-Json -Compress -Depth 3 @{gpus=$g;cpus=$p;memory=$m}")
    try:
        return json.loads(run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script]) or "{}")
    except ValueError:
        return {}


def windows(kit):
    cv = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"
    build = int(reg(HKLM, cv, "CurrentBuildNumber") or 0)
    out = {
        "release": f"Windows {11 if build >= 22000 else 10} {reg(HKLM, cv, 'DisplayVersion') or reg(HKLM, cv, 'ReleaseId') or ''}".strip(),
        "build": f"{reg(HKLM, cv, 'CurrentMajorVersionNumber')}.{reg(HKLM, cv, 'CurrentMinorVersionNumber')}.{build}.{reg(HKLM, cv, 'UBR')}",
    }
    system = Path(os.environ.get("WINDIR", r"C:\Windows")) / "System32"
    for name in SYSTEM_FILES:
        out[name] = (kit.file_version(system / name) or "present, no version") if (system / name).is_file() else "absent"
    hags = reg(HKLM, r"SYSTEM\CurrentControlSet\Control\GraphicsDrivers", "HwSchMode")
    out["hardware GPU scheduling"] = {2: "on", 1: "off"}.get(hags, "not set (the driver's default)")
    settings = reg(HKCU, r"Software\Microsoft\DirectX\UserGpuPreferences", "DirectXUserGlobalSettings") or ""
    out["DirectX user settings"] = ";".join(sorted(s for s in settings.split(";") if s)) or "not set"
    out[".NET Framework 3.5"] = "present" if (Path(os.environ.get("WINDIR", r"C:\Windows")) / "Microsoft.NET" / "Framework64" / "v3.5").is_dir() else "absent"
    out["VC++ redistributable"] = reg(HKLM, r"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64", "Version") or "absent"
    return out


# Graphics cards by vendor. The other adapters Windows lists are virtual displays (streaming and remote-desktop tools)
# and the basic display driver: counted, not named, since their names say which tools the user has installed.
GPU_VENDORS = re.compile(r"NVIDIA|AMD|Radeon|Intel|Arc|Qualcomm|Adreno|Snapdragon", re.I)


def graphics(kit, wmi):
    every = wmi.get("gpus") or []
    if isinstance(every, dict):
        every = [every]
    gpus = [g for g in every if GPU_VENDORS.search(g.get("Name") or "")]
    out = {"GPU": "; ".join(g["Name"] for g in gpus) or "?",
           "other display adapters": f"{len(every) - len(gpus)} (virtual or basic)"}
    smi = kit.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"])
    if smi:
        out["NVIDIA GPU"] = "; ".join(smi.splitlines())
    out["driver"] = "; ".join(f"{g.get('Name')}: {g.get('DriverVersion')}" for g in gpus) or "?"
    ngx = reg(HKLM, r"SOFTWARE\NVIDIA Corporation\Global\NGXCore", "FullPath")
    core = Path(ngx) / "_nvngx.dll" if ngx else None
    out["NGX core"] = (kit.file_version(core) or "present, no version") if core and core.is_file() else "absent"
    shown = [f"{g['CurrentHorizontalResolution']}x{g['CurrentVerticalResolution']} at {g.get('CurrentRefreshRate')} Hz"
             for g in gpus if g.get("CurrentHorizontalResolution")]
    out["displays"] = "; ".join(shown) or "?"
    return out


def vr(kit):
    path = reg(HKLM, r"SOFTWARE\Khronos\OpenXR\1", "ActiveRuntime")
    if not path or not Path(path).is_file():
        return {"OpenXR runtime": "none"}
    try:
        runtime = json.loads(Path(path).read_text(encoding="utf-8")).get("runtime", {})
    except (OSError, ValueError):
        return {"OpenXR runtime": "unreadable manifest"}
    library = (Path(path).parent / runtime.get("library_path", "")).resolve()
    version = kit.file_version(library) if library.is_file() else None
    return {"OpenXR runtime": f"{runtime.get('name') or Path(path).stem} {version or ''}".strip()}


def vam_versions(kit, vam):
    exe = vam / "VaM.exe"
    if not exe.is_file():
        return {}

    def of(relative):
        p = vam / relative
        return (kit.file_version(p) or "present, no version") if p.is_file() else "absent"

    return {"Unity": kit.file_version(exe) or "?", "mono.dll": of("Mono/EmbedRuntime/mono.dll"),
            "BepInEx": of("BepInEx/core/BepInEx.dll"), "Harmony": of("BepInEx/core/0Harmony.dll"),
            "UnityDoorstop": of("winhttp.dll")}


def build_tools(kit):
    found = kit.tools()
    out = {}
    vcvars = found.get("VAMKIT_VCVARS")
    toolsets = sorted(glob.glob(str(Path(vcvars).parents[2] / "Tools" / "MSVC" / "*"))) if vcvars else []
    out["MSVC"] = Path(toolsets[-1]).name if toolsets else "absent"
    sdk = found.get("VAMKIT_FXC")
    out["Windows SDK"] = Path(sdk).parent.name if sdk else "absent"
    cmake = shutil.which("cmake")
    if not cmake and vcvars:
        bundled = glob.glob(str(Path(vcvars).parents[3] / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"))
        cmake = bundled[0] if bundled else None
    out["CMake"] = (kit.run([cmake, "--version"]).splitlines() or ["?"])[0].replace("cmake version ", "") if cmake else "absent"
    out["C# compiler"] = (kit.file_version(found["VAMKIT_CSC"]) or "present, no version") if found.get("VAMKIT_CSC") else "absent"
    out["Python"] = sys.version.split()[0]
    out["git"] = kit.run(["git", "--version"]).replace("git version ", "") or "absent"
    return out


def collect(kit, vam=None):
    """{group: {key: value}} for this machine."""
    wmi = cim(kit.run)
    cpus = wmi.get("cpus") or []
    cores = sum(int(c.get("NumberOfCores") or 0) for c in ([cpus] if isinstance(cpus, dict) else cpus))
    groups = {
        "Windows": windows(kit),
        "Graphics": graphics(kit, wmi),
        "VR": vr(kit),
        "Hardware": {
            "CPU": (reg(HKLM, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString") or "?").strip(),
            "cores": f"{cores} cores, {os.cpu_count()} logical processors",
            "memory": f"{round(int(wmi.get('memory') or 0) / 2**30)} GB",
        },
    }
    if vam is not None:
        found = vam_versions(kit, Path(vam))
        if found:
            groups["VaM"] = found
    groups["Kit run-time files"] = {r["name"]: (r["version"] + ("" if r["developed_with"] else " (not the pinned one)")) if r["present"] else "missing"
                                    for r in kit.runtimes()}
    groups["Build tools"] = build_tools(kit)
    groups["Kit"] = {"commit": kit.run(["git", "-C", str(kit.repo), "rev-parse", "--short", "HEAD"]) or "?",
                     "captured": datetime.date.today().isoformat()}
    return groups


def reference(path=None):
    p = Path(path) if path else REFERENCE
    return json.loads(p.read_text(encoding="utf-8")) if p.is_file() else None


def compare(ours, theirs):
    """[(group, key, ours, theirs or None, state)] with state same / differs / new; groups only one side has are left out."""
    rows = []
    for group, values in ours.items():
        if group not in theirs:
            continue
        for key, value in values.items():
            if (group, key) in INFO_ONLY:
                continue
            other = theirs[group].get(key)
            rows.append((group, key, value, other, "new" if other is None else "same" if other == value else "differs"))
    return rows


def differences(ours, theirs):
    return [r for r in compare(ours, theirs) if r[4] != "same"]


def print_report(ours, theirs, against):
    rows = {(g, k): (t, s) for g, k, _, t, s in compare(ours, theirs)} if theirs else {}
    width = max(len(k) for values in ours.values() for k in values)
    for group, values in ours.items():
        print(group)
        for key, value in values.items():
            other, state = rows.get((group, key), (None, ""))
            mark = {"same": "same", "differs": "DIFF", "new": "new "}.get(state, "    ")
            line = f"  {mark}  {key:<{width}}  {value}"
            if state == "differs":
                line += f"   (reference: {other})"
            print(line)
            if state in ("differs", "new") and (group, key) in NOTES:
                print(f"        {NOTES[(group, key)]}")
    if not theirs:
        print(f"\nno reference to compare with ({against})")
        return
    diff = [r for r in compare(ours, theirs) if r[4] != "same"]
    skipped = [g for g in theirs if g not in ours]
    print(f"\n{len(diff)} line(s) differ from {against}" + (f"; not compared: {', '.join(skipped)} (give --vam for VaM)" if skipped else "") + ".")
    if diff:
        print("The reference record does not have these. They are where to look first when something here behaves differently,"
              "\nand worth trying before the user runs into them: docs/agent/reference-machine.md.")
