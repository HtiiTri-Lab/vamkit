"""The VaM launch panel: the visual mod or vanilla, VR or desktop. Never launches a game merely by opening the panel."""
from __future__ import annotations

import argparse
import configparser
import ctypes
import datetime as dt
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox, ttk
import uuid
import winreg
sys.path.insert(0, str(Path(__file__).resolve().parent))   # ModeProfiles lies beside this file, also under `python -I`
import ModeProfiles as profiles
import lang
from lang import mark, tr
import runtimes

ROOT = Path(__file__).resolve().parent
VAM = ROOT.parent.parent
# The panel launches NVIDIA DLSS SR with DLSS Frame Generation only (2026-10-15;
# its FSR 1, NIS, native and optical-flow launches went). OpenComposite still
# uses FSR 1 for a frame DLSS SR cannot take, and OFXR optical flow for a pair
# DLSS-G cannot (tray.ini keeps its settings for that).
# DLSS SR models ([Performance] DLSSPreset), chosen in the in-game tab: Off is no
# SR at all (native frames), K/L/M each with a render scale of its own
# (RenderScaleK/L/M). Before v18 the panel chose Default, J, K, L or M.
# RR is not among them: it comes with path tracing, which the game's menu switches.
# XeSS and FSR are the two upscalers that are not DLSS (Intel's, and AMD's through the FSR API: FSR 4 on the Radeons that
# have it, FSR 3.1 on every other card). They need no RTX card; each has a render scale of its own too.
SR_MODELS = ("Off", "K", "L", "M", "XeSS", "FSR")
DLSS_MODELS = ("K", "L", "M")
# Frame generation ([Performance] FrameGenModel): DLSS Frame Generation (RTX 40: 2x, RTX 50: up to 4x) or FSR's (any
# card, 2x). Chosen in the in-game tab.
FG_MODELS = ("DLSS", "FSR")
# What the panel shows goes through lang.tr: English here, the other languages in vamkit/lang/<code>.json.
VARIANT_NAMES = {"mod": mark("Enhanced visuals (mod)"), "vanilla": mark("Vanilla")}
DISPLAY_NAMES = {"VR": "VR", "Desktop": mark("Desktop")}
NR_KEYS = {
    "nr_enabled": "Enabled", "nr_style": "Style", "nr_intensity": "Intensity",
    "nr_tone": "LocalToneStrength", "nr_structure": "LocalStructureStrength",
    "nr_skin": "SkinStructureStrength", "nr_region": "FoveationSize",
    "nr_model": "RenderPreset", "nr_shift": "RegionShiftY",
}
PERF_KEYS = {
    "render_scale": "RenderScale", "fsr_sharpness": "FSRSharpness", "nr_feather": "NRFeatherPercent",
    "ffr_enabled": "FFREnabled", "ffr_inner": "FFRFullRateRadius",
    "ffr_outer": "FFRHalfRateRadius", "dlss_sharpness": "DLSSSharpness", "dlss_sharpen_last": "DLSSSharpenLast",
    "rr_stereo": "RRStereo",
}
LIMITS = {
    "render_scale": (33, 100), "fsr_sharpness": (0, 100), "dlss_sharpness": (0, 100),
    "dlss_sharpen_last": (0, 1),
    "rr_stereo": (0, 100),
    "render_scale_k": (33, 100), "render_scale_l": (33, 100), "render_scale_m": (33, 100), "render_scale_rr": (33, 100),
    "render_scale_xess": (33, 100), "render_scale_fsr": (33, 100),
    "output_scale": (75, 150),
    "nr_enabled": (0, 1), "nr_style": (0, 2), "nr_intensity": (0, 100),
    "nr_tone": (0, 200), "nr_structure": (0, 200), "nr_skin": (0, 200),
    "nr_region": (10, 100), "nr_shift": (-30, 30), "nr_feather": (1, 45), "nr_model": (1, 2), "ffr_enabled": (0, 1),
    "ffr_inner": (20, 150), "ffr_outer": (20, 150), "flow_bidirectional": (0, 1),
    "nr_guides": (0, 1), "dlss_flip_x": (0, 1), "dlss_flip_y": (0, 1),
    "dlss_object_motion": (0, 1), "dlss_fg_eyes": (1, 3), "dlss_sr_eyes": (1, 3),
    "dlss_hair_motion": (0, 1), "dlss_hair_bias": (0, 100), "dlss_fg_lite": (0, 1),
    "nr_after": (0, 1), "nr_after_scale": (25, 100), "dlss_menu_native": (0, 1), "framegen": (0, 1),
}
# Settings the panel no longer shows (2026-10-15: DLSS SR + DLSS Frame Generation
# only; NR and the upscaler in parallel went with OpenComposite-DLSS-v19): always these values,
# whatever an older panel saved.
# The fallback FSR 1 sharpness, FFR's radii, the fallback optical flow, the
# motion vectors and the single-eye / runtime / capture diagnostics.
# FFR stays off: NVAPI D3D11 VRS is skipped under 11on12. Sharpening returned
# to the in-game menu in v34; it must be carried forward instead of forced off.
FIXED = dict(upscaler="DLSS", fg_backend="DLSSG", fsr_sharpness=40, ffr_inner=60, ffr_outer=85,
             ffr_enabled=0,
             flow_preset="medium", flow_scale=50, flow_bidirectional=0, nr_guides=1,
             dlss_object_motion=1, dlss_hair_motion=1, dlss_hair_bias=0, dlss_flip_x=0, dlss_flip_y=0,
             dlss_sr_eyes=3, dlss_fg_eyes=3)
# The in-game "VR Graphics" tab's settings (VaMVRMenu): only the tab changes them and
# the panel shows none of them. The panel carries them to the next launch: it
# takes them from the newest run's files when it starts (last_run_game_values)
# and from this run's while VaM runs (adopt_live, adopt_multiplier).
# frame_multiplier is [ofxr] frame_multiplier in the OFXR layer's
# ofxr_bridge.ini; the others sit in performance.ini (GAME_INI). framegen is the
# tab's Frame generation switch: every launch goes through OFXR, and the tab pauses frame
# generation through the OFXR tray's pause while it is off (from the start when
# it was off at launch) and while VaM's menus are open (dlss_menu_native).
# nr_model (Model, [NR] RenderPreset: 2 = NR 310.9.16's new model, 1 = 310.8.0's;
# OpenComposite-DLSS-v22) is the tab's; the registry has no such value, so it starts at 2.
# dlss_sharpness (NVSharpen after SR + NR, 0-100) is the tab's again since v34.
# nr_after_scale (Work scale, [Performance] NRAfterScale 25-100: NR after the upscaler on the
# output reduced to that percent, OpenComposite-DLSS-on12-nrscaled) and dlss_fg_lite (Mode,
# [Performance] DLSSGLite, 1 = Preset C, followed live by OFXR-DLSSG-v12) are the tab's since v29.
# nr_shift (Region up, [NR] RegionShiftY -30..30: the NR region moved up by that percent of the eye
# image's height, OpenComposite-DLSS-on12-nrshift) is the tab's since v32; the registry has none, so it starts at 0.
GAME_INI = {**{key: ("NR", ini) for key, ini in NR_KEYS.items()},
            "nr_feather": ("Performance", "NRFeatherPercent"), "render_scale": ("Performance", "RenderScale"),
            "dlss_sharpness": ("Performance", "DLSSSharpness"),
            "dlss_sharpen_last": ("Performance", "DLSSSharpenLast"),
            "rr_stereo": ("Performance", "RRStereo"),
            "nr_after": ("Performance", "NRAfterUpscale"), "nr_after_scale": ("Performance", "NRAfterScale"),
            "dlss_fg_lite": ("Performance", "DLSSGLite"), "dlss_menu_native": ("Performance", "DLSSMenuNative"),
            "framegen": ("Performance", "FrameGeneration"), "dlss_preset": ("Performance", "DLSSPreset"),
            "render_scale_k": ("Performance", "RenderScaleK"), "render_scale_l": ("Performance", "RenderScaleL"),
            "render_scale_m": ("Performance", "RenderScaleM"), "render_scale_rr": ("Performance", "RenderScaleRR"), "output_scale": ("Performance", "OutputScale"),
            "render_scale_xess": ("Performance", "RenderScaleXeSS"), "render_scale_fsr": ("Performance", "RenderScaleFSR"),
            "framegen_model": ("Performance", "FrameGenModel")}
# The tab's settings that are a word, not a number, with the words each may hold.
GAME_WORDS = {"dlss_preset": SR_MODELS, "framegen_model": FG_MODELS}
GAME_KEYS = (*GAME_INI, "frame_multiplier")
# OFXR's tray settings for a launch.
TRAY = dict(backend="nvidia", nvidia_preset="medium", nvidia_input_scale="50", nvidia_bidirectional="0", deep_pipeline="1",
            excluded_processes="PimaxHome-Win64-Shipping.exe", single_swapchain_rings="1", vulkan_session_bridge="1", vulkan_bridge="0",
            d3d11_bridge="1", capture_at_end_frame="1", diagnostics="0", overlay_position="off", pause_hotkey="ctrl+alt+f7")


def initial_settings():
    s = dict(render_scale=50, upscaler="DLSS", fsr_sharpness=40, dlss_sharpness=25, dlss_sharpen_last=1,
             frame_multiplier=2, nr_enabled=1, nr_style=0, nr_intensity=100,
             nr_tone=80, nr_structure=100, nr_skin=85, nr_region=66, nr_shift=0, nr_feather=10, nr_model=2,
             ffr_enabled=1, ffr_inner=60, ffr_outer=85,
             flow_preset="medium", flow_scale=50, flow_bidirectional=0,
             dlss_preset="K", render_scale_k=75, render_scale_l=33, render_scale_m=50, render_scale_rr=33, output_scale=100,
             render_scale_xess=67, render_scale_fsr=67, framegen_model="DLSS",
             nr_guides=1, dlss_flip_x=0, dlss_flip_y=0,
             dlss_object_motion=1, fg_backend="DLSSG", dlss_fg_eyes=3, dlss_sr_eyes=3,
             dlss_hair_motion=1, dlss_hair_bias=0, dlss_fg_lite=0,
             nr_after=1, nr_after_scale=50, dlss_menu_native=1,
             framegen=1, variant="mod", display_mode="VR", rr_stereo=0)
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\Virtual Desktop, Inc.\OpenXR",
                            0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            for name, ini in NR_KEYS.items():
                reg = "FoveationSize" if name == "nr_region" else "DLSSNR_" + ini
                try:
                    s[name] = int(winreg.QueryValueEx(key, reg)[0])
                except (OSError, ValueError):
                    pass
    except OSError:
        pass
    # The requested 50% preset starts with NR enabled; existing NR strengths
    # are imported once without writing to the system registry.
    s["nr_enabled"] = 1
    return s


def display_adapters():
    """(PCI vendor id, name) of the display adapters Windows lists: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel."""
    class Device(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("DeviceName", ctypes.c_wchar * 32), ("DeviceString", ctypes.c_wchar * 128),
                    ("StateFlags", ctypes.c_ulong), ("DeviceID", ctypes.c_wchar * 128), ("DeviceKey", ctypes.c_wchar * 128)]
    found = []
    try:
        index = 0
        while True:
            device = Device()
            device.cb = ctypes.sizeof(device)
            if not ctypes.windll.user32.EnumDisplayDevicesW(None, index, ctypes.byref(device), 0):
                break
            index += 1
            vendor = re.search(r"VEN_([0-9A-Fa-f]{4})", device.DeviceID)
            entry = (int(vendor.group(1), 16), device.DeviceString) if vendor else None
            if entry and entry not in found:
                found.append(entry)
    except (OSError, AttributeError):
        pass
    return found


def launch_notes(s):
    """What this launch will not get as chosen, why, and how to get it: lines for the summary. Nothing here changes a
    setting. The compositor and the frame generation layer try what was chosen and only then put the next best in its
    place (`VamSrChoice`, `make_guided_frame_generator`); the NVIDIA libraries the kit lacks are taken from the
    graphics driver's own store at launch (runtimes.py). The launcher's part is to say what is missing."""
    names = {"sr": mark("DLSS Super Resolution"), "rr": mark("Ray Reconstruction (path tracing's reconstruction)"),
             "fg": mark("DLSS Frame Generation")}
    lost = {"sr": mark("XeSS / FSR stands in this time"), "rr": mark("path tracing cannot start"),
            "fg": mark("FSR frame generation (2×) is used this time")}
    wanted = ["rr"]
    if s["dlss_preset"] in DLSS_MODELS:
        wanted.insert(0, "sr")
    if s["framegen"] and s["framegen_model"] == "DLSS":
        wanted.append("fg")
    adapters = display_adapters()
    if adapters and not any(vendor == 0x10DE for vendor, _ in adapters):
        # Not a guess about what the card can do: NGX exists only in NVIDIA's driver.
        if len(wanted) > 1:
            return [tr("This machine has no NVIDIA card: the DLSS items chosen become XeSS / FSR upscaling and FSR frame generation (2×) once the game starts; the in-game menu lets you choose them directly.")]
        return []
    report = runtimes.locate(VAM)
    lines = []
    for what in wanted:
        entry = report[what]
        if entry["source"] == "driver":
            lines.append(tr("The run-time library of {0} is not in the kit; this launch uses the graphics driver's own {1}.",
                            tr(names[what]), entry["version"]))
        elif entry["source"] == "missing":
            lines.append(tr("The run-time library of {0}, {1}, is missing: {2}. To add it: update the NVIDIA driver and run a game with this feature once (the driver downloads it), or run python tools/vamkit fetch and then install in the vamkit repository.",
                            tr(names[what]), runtimes.LIBRARIES[what][1], tr(lost[what])))
    return lines


def validate(s):
    s = dict(s)
    s.setdefault("rr_stereo", 0)
    s.setdefault("render_scale_xess", 67)
    s.setdefault("render_scale_fsr", 67)
    s.setdefault("framegen_model", "DLSS")
    s.update(FIXED)
    for key, (low, high) in LIMITS.items():
        s[key] = int(s[key])
        if not low <= s[key] <= high:
            raise ValueError(tr("{0} must be between {1} and {2}.", key, low, high))
    if s["dlss_preset"] == "RR":
        s["dlss_preset"] = "K"
    if s["dlss_preset"] not in SR_MODELS:
        raise ValueError(tr("The upscaler model is not valid."))
    if s["framegen_model"] not in FG_MODELS:
        s["framegen_model"] = "DLSS"
    if s.get("variant") not in VARIANT_NAMES:
        raise ValueError(tr("Choose the mod or vanilla."))
    if s.get("display_mode") not in DISPLAY_NAMES:
        raise ValueError(tr("Choose VR or desktop."))
    if s["frame_multiplier"] not in (2, 3, 4):
        raise ValueError(tr("The frame multiplier can be 2×, 3× or 4×."))
    return s


def read_live(text):
    """The in-game tab's settings (GAME_INI) in the text of a run's performance.ini,
    as the panel keeps them. Values that are missing, not numbers or out of range are
    left out."""
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    try:
        parser.read_string(text)
    except configparser.Error:
        return {}
    # Section and key names without case, as GetPrivateProfileString reads them.
    sections = {name.lower(): parser[name] for name in reversed(parser.sections())}
    found = {}
    for key, (section, name) in GAME_INI.items():
        values = sections.get(section.lower())
        if values is None or name not in values:
            continue
        value = values[name].strip()
        if key in GAME_WORDS:
            word = next((m for m in GAME_WORDS[key] if m.lower() == value.lower()), None)
            if word is not None:
                found[key] = word
            continue
        try:
            number = int(value)
        except ValueError:
            continue
        low, high = LIMITS[key]
        if low <= number <= high:
            found[key] = number
    return found


# The OFXR tray's runtime layers (v7443 is the one with DLSS Frame
# Generation), each with the ofxr_bridge.ini the layer reads.
OFXR_RUNTIME = Path(os.environ.get("LOCALAPPDATA", "")) / "OFXR Bridge" / "RuntimeLayer"


def layer_ini(_s):
    if _s.get("display_mode") == "Desktop":
        return Path(_s["run_directory"]) / "desktop-presenter.ini" if _s.get("run_directory") else profiles.directory("Desktop") / "desktop-presenter.ini"
    return OFXR_RUNTIME / "v7443" / "ofxr_bridge.ini"


def read_frame_multiplier(text):
    """[ofxr] frame_multiplier in the text of an ofxr_bridge.ini, as the in-game tab
    writes it (2-4), else None. The tray never writes the key."""
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    try:
        parser.read_string(text)
    except configparser.Error:
        return None
    sections = {name.lower(): parser[name] for name in reversed(parser.sections())}
    values = sections.get("ofxr")
    if values is None or "frame_multiplier" not in values:
        return None
    try:
        number = int(values["frame_multiplier"].strip())
    except ValueError:
        return None
    return number if 2 <= number <= 4 else None


def last_run_game_values(s):
    """The in-game tab's settings as the newest launch left them: its performance.ini,
    which the tab edits in place, and [ofxr] frame_multiplier where the tab wrote it
    after that launch started. Missing or invalid values are left out."""
    try:
        runs = sorted((p for p in (profiles.directory(s.get("display_mode", "VR")) / "panel-runs").iterdir()
                       if (p / "performance.ini").is_file() and (p / "selection.json").is_file()), reverse=True)
    except OSError:
        return {}
    if not runs:
        return {}
    try:
        found = read_live((runs[0] / "performance.ini").read_text(encoding="utf-8-sig"))
        started = (runs[0] / "selection.json").stat().st_mtime
    except OSError:
        return {}
    try:
        # A global VR layer file may now belong to the other mode. Only the
        # per-mode snapshot or the desktop run's own file can supply its value.
        if s.get("display_mode") != "Desktop":
            return found
        layer = runs[0] / "desktop-presenter.ini"
        if layer.stat().st_mtime >= started:
            multiplier = read_frame_multiplier(layer.read_text(encoding="utf-8-sig", errors="replace"))
            if multiplier is not None:
                found["frame_multiplier"] = multiplier
    except OSError:
        pass
    return found


def model_scale_key(model):
    return "render_scale_" + model.lower()


def adopt_model_scale(s, given):
    """Before v18 the panel's DLSS preset could be Default or J, and one render scale
    served every preset: those presets become K, and with DLSS the render scale becomes
    the chosen model's own unless `given` (the values read) already has one for it."""
    if s.get("dlss_preset") not in SR_MODELS:
        s["dlss_preset"] = "K"
    model = s["dlss_preset"]
    if (model != "Off" and s.get("upscaler") == "DLSS" and "render_scale" in given
            and model_scale_key(model) not in given):
        s[model_scale_key(model)] = s["render_scale"]


def effective_render_scale(s):
    """[Performance] RenderScale for a launch: the chosen model's own, 100 with SR off."""
    return 100 if s["dlss_preset"] == "Off" else s[model_scale_key(s["dlss_preset"])]


def load_settings(display_mode=None):
    mode = display_mode or profiles.selected_mode()
    profile = profiles.ensure(mode)
    s = initial_settings()
    try:
        saved = json.loads((profile / "LaunchPanel.json").read_text(encoding="utf-8-sig"))
        saved["display_mode"] = mode
        s.update({k: v for k, v in saved.items() if k in s})
        # Before the in-game tab owned the multiplier, 1 meant frame generation off.
        if "framegen" not in saved and saved.get("frame_multiplier") == 1:
            s.update(framegen=0, frame_multiplier=2)
        adopt_model_scale(s, saved)
        s = validate(s)
    except (OSError, ValueError, TypeError, KeyError, AttributeError):
        s = validate(dict(initial_settings(), display_mode=mode))
    try:
        snapshot = profiles.read_json(profile / "plugin-settings.json", {})
        performance = snapshot.get("performance", {})
        ini = "\n".join("[" + section + "]\n" + "\n".join(k + "=" + str(v) for k, v in keys.items() if v is not None)
                        for section, keys in performance.items())
        s.update(read_live(ini))
        mult = snapshot.get("layer", {}).get("ofxr", {}).get("frame_multiplier")
        if mult in ("2", "3", "4"):
            s["frame_multiplier"] = int(mult)
        game = last_run_game_values(s)
        merged = dict(s, **game)
        adopt_model_scale(merged, game)
        return validate(merged)
    except (ValueError, TypeError, KeyError):
        return s


def atomic_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(text, encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def save_settings(s, remember=True):
    s = validate(s)
    path = profiles.ensure(s["display_mode"]) / "LaunchPanel.json"
    old = profiles.read_json(path, {})
    if "seeded_from" in old:
        s["seeded_from"] = old["seeded_from"]
    atomic_write(path, json.dumps(s, ensure_ascii=False, indent=2) + "\n")
    if remember:
        profiles.select(s["display_mode"])


def performance_ini(s):
    s = validate(dict(s))
    lines = ["; VaM launch panel: per-run settings; no registry writes.",
             "[Performance]", "Enabled=1", "Upscaler=" + s["upscaler"]]
    effective = dict(s, render_scale=effective_render_scale(s))
    lines.extend(f"{ini}={effective[key]}" for key, ini in PERF_KEYS.items())
    # The model, each model's render scale and the output scale are the in-game
    # tab's; the motion and diagnostic keys are FIXED.
    lines += ["DLSSPreset=" + s["dlss_preset"], f"RenderScaleK={s['render_scale_k']}",
              f"RenderScaleL={s['render_scale_l']}", f"RenderScaleM={s['render_scale_m']}", f"RenderScaleRR={s['render_scale_rr']}",
              f"RenderScaleXeSS={s['render_scale_xess']}", f"RenderScaleFSR={s['render_scale_fsr']}",
              f"OutputScale={s['output_scale']}", f"DLSSJitterFlipX={s['dlss_flip_x']}",
              f"DLSSJitterFlipY={s['dlss_flip_y']}", f"NRUseDepth={s['nr_guides']}",
              f"NRUseMotion={s['nr_guides']}", f"DLSSObjectMotion={s['dlss_object_motion']}",
              f"DLSSFGEyes={s['dlss_fg_eyes']}", f"DLSSSREyes={s['dlss_sr_eyes']}",
              f"DLSSHairMotion={s['dlss_hair_motion']}", f"DLSSHairBias={s['dlss_hair_bias']}",
              f"DLSSMenuNative={s['dlss_menu_native']}",
              f"NRAfterUpscale={s['nr_after']}", f"NRAfterScale={s['nr_after_scale']}",
              f"FrameGeneration={s['framegen']}", f"FrameGenModel={s['framegen_model']}",
              f"DLSSGLite={s['dlss_fg_lite']}"]
    lines.extend(["", "[NR]"])
    lines.extend(f"{ini}={s[key]}" for key, ini in NR_KEYS.items())
    return "\n".join(lines) + "\n"


def flow_ini(s, multiplier):
    section = dict(TRAY, nvidia_preset=s["flow_preset"], nvidia_input_scale=str(s["flow_scale"]),
                   nvidia_bidirectional=str(s["flow_bidirectional"]), triple_frame_gen=str(int(multiplier >= 3)))
    return "[tray]\n" + "\n".join(f"{key}={value}" for key, value in section.items()) + "\n"


def launch_multiplier(s):
    """Frames per VaM frame a launch is made for: the in-game tab's multiplier. Every
    launch goes through OFXR; with the tab's Frame generation switch off (framegen 0) it starts paused."""
    return s["frame_multiplier"]


def begin_launch(settings):
    s = validate(settings)
    multiplier = launch_multiplier(s)
    save_settings(s)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    run = profiles.directory(s["display_mode"]) / "panel-runs" / stamp
    run.mkdir(parents=True)
    atomic_write(run / "performance.ini", performance_ini(s))
    atomic_write(run / "tray.ini", flow_ini(s, multiplier))
    # frame_multiplier here: what this launch is made for; framegen: whether it starts
    # generating (the in-game tab pauses it otherwise).
    atomic_write(run / "selection.json", json.dumps(dict(s, frame_multiplier=multiplier), indent=2) + "\n")
    python = Path(sys.executable).with_name("python.exe")
    args = [str(python), str(ROOT / "launch.py"), "--variant", s["variant"], "--display", s["display_mode"], "--run", str(run)]
    s["run_directory"] = str(run)
    with (run / "launcher.log").open("wb") as output:
        child = subprocess.Popen(args, cwd=VAM, stdout=output, stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
    return child, run, s


def submitted_dimensions(run, eye):
    """Read only this launch's per-eye submission; never infer from older logs."""
    try:
        data = json.loads((run / f"performance.ini.eye{eye}.json").read_text(encoding="utf-8"))
        if data.get("eye") != eye:
            return None
        if data.get("status") == "fallback":
            return "fallback"
        if data.get("status") != "submitted":
            return None
        sizes = (*data["input"], *data["output"])
        if len(sizes) != 4 or any(type(n) is not int or n <= 0 for n in sizes):
            return None
        return sizes
    except (OSError, ValueError, KeyError, TypeError):
        return None


def output_scale_of(ini):
    """[Performance] OutputScale of a run's performance.ini as it is now (75-150)."""
    try:
        found = read_live(ini.read_text(encoding="utf-8-sig"))
    except OSError:
        return 100
    return found.get("output_scale", 100)


def last_output_size(display_mode="VR"):
    """Per-eye headset size the runtime recommended in the latest panel launch that
    reported an output (that output without its OutputScale), or None. Only used to
    estimate sizes; the real ones are read after launch."""
    try:
        reports = sorted((profiles.directory(display_mode) / "panel-runs").glob("*/performance.ini.eye0.json"),
                         key=lambda p: p.stat().st_mtime, reverse=True)
    except OSError:
        return None
    for path in reports[:10]:
        try:
            ow, oh = json.loads(path.read_text(encoding="utf-8"))["output"]
            if type(ow) is int and type(oh) is int and ow > 0 and oh > 0:
                scale = output_scale_of(path.parent / "performance.ini")
                return round(ow * 100 / scale), round(oh * 100 / scale)
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return None


# ---------------------------------------------------------------- appearance

def monitor_dimensions():
    # DEVMODEW current primary display mode (physical pixels, independent of DPI).
    mode = ctypes.create_string_buffer(220)
    ctypes.c_ushort.from_buffer(mode, 68).value = 220
    if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, mode):
        return tuple(ctypes.c_uint32.from_buffer(mode, n).value for n in (172, 176, 184))
    raise OSError(tr("Cannot read the monitor's resolution and refresh rate"))

class T:
    BG = "#0d1218"
    SURFACE = "#141b24"
    RAISED = "#1b2430"
    HOVER = "#243142"
    BORDER = "#222c39"
    TRACK = "#334052"
    TEXT = "#e7ecf3"
    MUTED = "#8995a7"
    FAINT = "#566273"
    ACCENT = "#3ecf8e"
    ACCENT_HOVER = "#5ddca3"
    ACCENT_TEXT = "#04261a"
    ACCENT_DIM = "#1d4d39"
    WARN = "#e8b84f"
    FONT = "Microsoft YaHei UI"


def rounded(canvas, x1, y1, x2, y2, r, **kw):
    r = max(0.0, min(r, (x2 - x1) / 2, (y2 - y1) / 2))
    points = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2,
              x2 - r, y2, x1 + r, y2, x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    return canvas.create_polygon(points, smooth=True, **kw)


def value_of(var):
    try:
        return var.get()
    except (tk.TclError, ValueError):
        return None


class Segmented(tk.Frame):
    """A row of mutually exclusive choices; the selected one is filled."""

    def __init__(self, panel, master, var, options):
        super().__init__(master, bg=T.RAISED, padx=panel.S(3), pady=panel.S(3))
        self.var, self.locked, self.blocked, self.items = var, False, set(), []
        for column, (label, value) in enumerate(options):
            item = tk.Label(self, text=label, font=(T.FONT, 10), padx=panel.S(14), pady=panel.S(5),
                            bg=T.RAISED, fg=T.TEXT)
            item.grid(row=0, column=column, sticky="nsew", padx=(0 if column == 0 else panel.S(2), 0))
            item.bind("<Button-1>", lambda _e, v=value: self.pick(v))
            item.bind("<Enter>", lambda _e, i=item, v=value: self.hover(i, v, True))
            item.bind("<Leave>", lambda _e, i=item, v=value: self.hover(i, v, False))
            self.items.append((item, value))
        var.trace_add("write", lambda *_: self.paint())
        self.paint()

    def pick(self, value):
        if not self.locked and value not in self.blocked and value_of(self.var) != value:
            self.var.set(value)

    def hover(self, item, value, inside):
        if not self.locked and value not in self.blocked and value_of(self.var) != value:
            item.configure(bg=T.HOVER if inside else T.RAISED)

    def set_locked(self, locked):
        if locked != self.locked:
            self.locked = locked
            self.paint()

    def set_blocked(self, blocked):
        if set(blocked) != self.blocked:
            self.blocked = set(blocked)
            self.paint()

    def paint(self):
        current = value_of(self.var)
        for item, value in self.items:
            if value == current:
                item.configure(bg=T.ACCENT_DIM if self.locked else T.ACCENT,
                               fg=T.TEXT if self.locked else T.ACCENT_TEXT,
                               font=(T.FONT, 10, "bold"), cursor="arrow")
            elif self.locked or value in self.blocked:
                item.configure(bg=T.RAISED, fg=T.FAINT, font=(T.FONT, 10), cursor="arrow")
            else:
                item.configure(bg=T.RAISED, fg=T.TEXT, font=(T.FONT, 10), cursor="hand2")


class Switch(tk.Frame):
    """On/off toggle bound to a 0/1 IntVar."""

    def __init__(self, panel, master, var, text="", bg=T.SURFACE):
        super().__init__(master, bg=bg)
        self.var, self.locked = var, False
        self.w, self.h = panel.S(40), panel.S(22)
        self.canvas = tk.Canvas(self, width=self.w, height=self.h, bg=bg, highlightthickness=0, cursor="hand2")
        self.canvas.pack(side="left")
        widgets = [self.canvas]
        if text:
            self.label = tk.Label(self, text=text, bg=bg, fg=T.TEXT, font=(T.FONT, 10), cursor="hand2")
            self.label.pack(side="left", padx=(panel.S(10), 0))
            widgets.append(self.label)
        for widget in widgets:
            widget.bind("<Button-1>", self.toggle)
        var.trace_add("write", lambda *_: self.paint())
        self.paint()

    def toggle(self, _event=None):
        if not self.locked:
            self.var.set(0 if value_of(self.var) else 1)

    def set_locked(self, locked):
        if locked != self.locked:
            self.locked = locked
            cursor = "arrow" if locked else "hand2"
            self.canvas.configure(cursor=cursor)
            if hasattr(self, "label"):
                self.label.configure(cursor=cursor, fg=T.MUTED if locked else T.TEXT)
            self.paint()

    def paint(self):
        c, w, h = self.canvas, self.w, self.h
        c.delete("all")
        on = bool(value_of(self.var))
        if self.locked:
            track, knob = (T.ACCENT_DIM if on else T.RAISED), "#7d8794"
        else:
            track, knob = (T.ACCENT if on else T.TRACK), "#ffffff"
        rounded(c, 1, 1, w - 1, h - 1, h / 2, fill=track, outline=track)
        r = h / 2 - 3
        x = w - h / 2 if on else h / 2
        c.create_oval(x - r, h / 2 - r, x + r, h / 2 + r, fill=knob, outline=knob)


class Slider(tk.Frame):
    """Drag track plus an exact number box, bound to an IntVar."""

    def __init__(self, panel, master, var, low, high, unit=""):
        super().__init__(master, bg=T.SURFACE)
        self.var, self.low, self.high, self.locked = var, low, high, False
        self.pad, self.knob = panel.S(9), panel.S(8)
        self.canvas = tk.Canvas(self, height=panel.S(26), bg=T.SURFACE, highlightthickness=0, cursor="hand2")
        self.canvas.grid(row=0, column=0, sticky="ew")
        self.columnconfigure(0, weight=1)
        self.entry = tk.Entry(self, textvariable=var, width=4, justify="center", font=(T.FONT, 10),
                              bg=T.RAISED, fg=T.TEXT, relief="flat", highlightthickness=1,
                              highlightbackground=T.BORDER, highlightcolor=T.ACCENT, insertbackground=T.TEXT,
                              disabledbackground=T.RAISED, disabledforeground=T.FAINT,
                              selectbackground=T.ACCENT_DIM)
        self.entry.grid(row=0, column=1, padx=(panel.S(12), 0), ipady=panel.S(3))
        self.entry.bind("<Up>", lambda _e: self.step(1))
        self.entry.bind("<Down>", lambda _e: self.step(-1))
        self.unit = tk.Label(self, text=unit, bg=T.SURFACE, fg=T.MUTED, font=(T.FONT, 9), width=2, anchor="w")
        self.unit.grid(row=0, column=2, padx=(panel.S(4), 0))
        self.canvas.bind("<Configure>", lambda _e: self.paint())
        self.canvas.bind("<Button-1>", self.drag)
        self.canvas.bind("<B1-Motion>", self.drag)
        var.trace_add("write", lambda *_: self.paint())

    def step(self, delta):
        value = value_of(self.var)
        if not self.locked and isinstance(value, int):
            self.var.set(min(self.high, max(self.low, value + delta)))
        return "break"

    def drag(self, event):
        if self.locked:
            return
        width = max(1, self.canvas.winfo_width() - 2 * self.pad)
        fraction = min(1.0, max(0.0, (event.x - self.pad) / width))
        value = round(self.low + fraction * (self.high - self.low))
        if value_of(self.var) != value:
            self.var.set(value)

    def set_locked(self, locked):
        if locked != self.locked:
            self.locked = locked
            self.entry.configure(state="disabled" if locked else "normal")
            self.canvas.configure(cursor="arrow" if locked else "hand2")
            self.paint()

    def paint(self):
        c = self.canvas
        c.delete("all")
        value = value_of(self.var)
        if not isinstance(value, int):
            return
        w, h = c.winfo_width(), c.winfo_height()
        x0, x1, y = self.pad, max(self.pad + 1, w - self.pad), h / 2
        fraction = (min(self.high, max(self.low, value)) - self.low) / max(1, self.high - self.low)
        xv = x0 + fraction * (x1 - x0)
        rounded(c, x0, y - 2, x1, y + 2, 2, fill=T.TRACK, outline=T.TRACK)
        fill = T.ACCENT_DIM if self.locked else T.ACCENT
        if xv > x0 + 1:
            rounded(c, x0, y - 2, xv, y + 2, 2, fill=fill, outline=fill)
        r = self.knob
        c.create_oval(xv - r, y - r, xv + r, y + r, fill="#7d8794" if self.locked else "#ffffff",
                      outline=fill, width=2)


class Row:
    """One labelled setting inside a card: title, control, optional hint."""

    def __init__(self, panel, card, title, hint=""):
        self.frame = tk.Frame(card.body, bg=T.SURFACE)
        self.frame.grid(row=card.add_row(), column=0, sticky="ew", pady=panel.S(5))
        self.frame.columnconfigure(0, minsize=panel.S(150))
        self.frame.columnconfigure(1, weight=1)
        self.title = tk.Label(self.frame, text=title, bg=T.SURFACE, fg=T.TEXT, font=(T.FONT, 10), anchor="w")
        self.title.grid(row=0, column=0, sticky="nw", pady=(panel.S(4), 0))
        self.hint_text = tk.StringVar(value=hint)
        self.hint = tk.Label(self.frame, textvariable=self.hint_text, bg=T.SURFACE, fg=T.MUTED,
                             font=(T.FONT, 9), justify="left", anchor="w")
        self.hint.grid(row=1, column=1, sticky="w", pady=(panel.S(4), 0))
        panel.wrap(self.hint, 210)
        if not hint:
            self.hint.grid_remove()

    def control(self, widget, stretch=False):
        widget.grid(row=0, column=1, sticky="ew" if stretch else "w")
        return widget

    def show(self, visible):
        if visible:
            self.frame.grid()
        else:
            self.frame.grid_remove()

    def set_hint(self, text, warn=False):
        self.hint_text.set(text)
        self.hint.configure(fg=T.WARN if warn else T.MUTED)
        if text:
            self.hint.grid()
        else:
            self.hint.grid_remove()


class Card(tk.Frame):
    """One pipeline stage: numbered header, one-line purpose, its settings."""

    def __init__(self, panel, master, badge, title, purpose, switch_var=None, collapsible=False):
        super().__init__(master, bg=T.SURFACE, highlightthickness=1, highlightbackground=T.BORDER)
        S = panel.S
        self.rows, self.open = 0, not collapsible
        head = tk.Frame(self, bg=T.SURFACE)
        head.pack(fill="x", padx=S(18), pady=(S(14), S(2)))
        head.columnconfigure(1, weight=1)
        size = S(28)
        dot = tk.Canvas(head, width=size, height=size, bg=T.SURFACE, highlightthickness=0)
        dot.create_oval(1, 1, size - 1, size - 1, fill=T.ACCENT_DIM, outline=T.ACCENT_DIM)
        dot.create_text(size / 2, size / 2, text=badge, fill=T.ACCENT, font=(T.FONT, 10, "bold"))
        dot.grid(row=0, column=0, rowspan=2, sticky="n", padx=(0, S(12)), pady=(S(2), 0))
        self.title = tk.Label(head, text=title, bg=T.SURFACE, fg=T.TEXT, font=(T.FONT, 13, "bold"), anchor="w")
        self.title.grid(row=0, column=1, sticky="w")
        self.purpose = tk.Label(head, text=purpose, bg=T.SURFACE, fg=T.MUTED, font=(T.FONT, 9),
                                justify="left", anchor="w")
        self.purpose.grid(row=1, column=1, sticky="w", pady=(S(2), 0))
        panel.wrap(self.purpose, 140)
        if switch_var is not None:
            self.switch = Switch(panel, head, switch_var)
            self.switch.grid(row=0, column=2, rowspan=2, sticky="e")
        if collapsible:
            self.arrow = tk.Label(head, text=tr("Show ▾"), bg=T.SURFACE, fg=T.MUTED, font=(T.FONT, 9), cursor="hand2")
            self.arrow.grid(row=0, column=2, rowspan=2, sticky="e")
            for widget in (head, self.title, self.purpose, self.arrow, dot):
                widget.bind("<Button-1>", lambda _e: self.set_open(not self.open))
                widget.configure(cursor="hand2")
        self.body = tk.Frame(self, bg=T.SURFACE)
        self.body.columnconfigure(0, weight=1)
        self.body_pad = (S(18), S(14))
        self.empty = tk.Label(self.body, text="", bg=T.SURFACE, fg=T.MUTED, font=(T.FONT, 9), anchor="w")
        if self.open:
            self.body.pack(fill="x", padx=self.body_pad[0], pady=(S(4), self.body_pad[1]))
        else:
            tk.Frame(self, bg=T.SURFACE, height=S(10)).pack(fill="x")
        self.panel = panel

    def add_row(self):
        self.rows += 1
        return self.rows

    def set_open(self, opened):
        self.open = opened
        self.arrow.configure(text=tr("Hide ▴") if opened else tr("Show ▾"))
        if opened:
            self.body.pack(fill="x", padx=self.body_pad[0], pady=(self.panel.S(4), self.body_pad[1]))
        else:
            self.body.pack_forget()

    def set_empty(self, text):
        """A note shown when none of the card's rows applies."""
        self.empty.configure(text=text)
        if text:
            self.empty.grid(row=0, column=0, sticky="w", pady=self.panel.S(4))
        else:
            self.empty.grid_remove()


# ---------------------------------------------------------------- the panel

class LaunchPanel:
    STARTUP_KEYS = {"variant", "display_mode"}

    def __init__(self, root, preset=None):
        self.root, self.child, self.run, self.active = root, None, None, None
        self.factor = root.winfo_fpixels("1i") / 96.0
        s = load_settings()
        self.display_mode = s["display_mode"]
        if preset:
            s.update(framegen=1, frame_multiplier=preset)
        self.v = {key: tk.IntVar(root, value=value) if isinstance(value, int) else tk.StringVar(root, value=value)
                  for key, value in s.items()}
        self.language = tk.StringVar(root, value=lang.current())   # not a launch setting: kept by lang.choose
        self.outer = None
        self.updating = False
        self.save_job = None
        self.live_text = None  # performance.ini as the panel last wrote or took it over
        self.launched_at = 0.0  # when the running launch started (a POSIX time)
        self.output_size = last_output_size()
        self.build()
        self.watch()
        self.refresh(initial=True)

    def watch(self):
        """The panel's own traces, after the controls' (build): a change is drawn, then acted on."""
        for key, variable in self.v.items():
            variable.trace_add("write", lambda *_, k=key: self.changed(k))
        self.language.trace_add("write", lambda *_: self.set_language())

    def set_language(self):
        code = self.language.get()
        if code == lang.current():
            return
        try:
            lang.choose(code)
        except OSError as error:
            self.set_status(tr("Saving failed: {0}", error))
            return
        self.root.after_idle(self.rebuild)   # not from inside the click that changed it

    def rebuild(self):
        """Every text again in the language chosen: the controls are made anew on the same variables."""
        for variable in (*self.v.values(), self.language):
            for mode, name in variable.trace_info():
                variable.trace_remove(mode, name)
        self.outer.destroy()
        self.build()
        self.watch()
        self.refresh(initial=True)
        self.paint_running()
        if self.running():
            self.set_status(tr("The in-game menus follow with the next start of VaM."))

    def paint_running(self):
        if self.running():
            self.launch_button.configure(state="disabled", text=tr("VaM is running…"), bg=T.ACCENT_DIM, cursor="arrow")
            self.state_label.configure(text=tr("● Running"), fg=T.ACCENT)
        else:
            self.launch_button.configure(state="normal", text=tr("Launch VaM"), bg=T.ACCENT, cursor="hand2")
            self.state_label.configure(text=tr("● Not running"), fg=T.MUTED)

    # -- helpers

    def S(self, n):
        return int(round(n * self.factor))

    def wrap(self, label, margin):
        self.wraps.append((label, margin))

    def get(self, key):
        return value_of(self.v[key])

    def running(self):
        return self.child is not None and self.child.poll() is None

    def segmented(self, row, key, options):
        widget = row.control(Segmented(self, row.frame, self.v[key], options))
        if key in self.STARTUP_KEYS:
            self.locks.append(widget)
        return widget

    def switch(self, row, key, text=""):
        widget = row.control(Switch(self, row.frame, self.v[key], text))
        if key in self.STARTUP_KEYS:
            self.locks.append(widget)
        return widget

    def slider(self, row, key, unit=""):
        low, high = LIMITS[key]
        widget = row.control(Slider(self, row.frame, self.v[key], low, high, unit), stretch=True)
        if key in self.STARTUP_KEYS:
            self.locks.append(widget)
        return widget

    def add(self, card, key, title, hint=""):
        row = Row(self, card, title, hint)
        self.rows[key] = row
        return row

    # -- layout

    def build(self):
        S, root = self.S, self.root
        self.locks, self.wraps, self.rows = [], [], {}
        root.title(tr("VaM Launcher"))
        if self.outer is None:              # the first time: the window's own size and the wheel
            width = min(S(1180), root.winfo_screenwidth() - S(60))
            height = min(S(860), root.winfo_screenheight() - S(90))
            root.geometry(f"{width}x{height}")
            root.minsize(S(960), S(620))
            root.bind_all("<MouseWheel>", self.wheel, add="+")
        root.configure(bg=T.BG)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.layout("Dark.Vertical.TScrollbar", [("Vertical.Scrollbar.trough", {"sticky": "ns", "children": [
            ("Vertical.Scrollbar.thumb", {"expand": "1", "sticky": "nswe"})]})])
        style.configure("Dark.Vertical.TScrollbar", troughcolor=T.BG, background=T.RAISED, bordercolor=T.BG,
                        lightcolor=T.RAISED, darkcolor=T.RAISED, gripcount=0, width=S(10))
        style.map("Dark.Vertical.TScrollbar", background=[("active", T.HOVER)])

        outer = self.outer = tk.Frame(root, bg=T.BG)
        outer.pack(fill="both", expand=True, padx=S(22), pady=(S(18), S(18)))
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(1, weight=1)

        header = tk.Frame(outer, bg=T.BG)
        header.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, S(14)))
        header.columnconfigure(0, weight=1)
        tk.Label(header, text=tr("VaM Launcher"), bg=T.BG, fg=T.TEXT, font=(T.FONT, 19, "bold")).grid(row=0, column=0, sticky="w")
        tk.Label(header, text=tr("Path tracing, upscaling, neural rendering and frame generation are set in the in-game menu."),
                 bg=T.BG, fg=T.MUTED, font=(T.FONT, 10)).grid(row=1, column=0, sticky="w", pady=(S(2), 0))
        self.state_label = tk.Label(header, text=tr("● Not running"), bg=T.BG, fg=T.MUTED, font=(T.FONT, 10, "bold"))
        self.state_label.grid(row=0, column=1, rowspan=2, sticky="e")

        # Left: the pipeline stages, scrollable.
        left = tk.Frame(outer, bg=T.BG)
        left.grid(row=1, column=0, sticky="nsew")
        left.rowconfigure(0, weight=1)
        left.columnconfigure(0, weight=1)
        self.canvas = tk.Canvas(left, bg=T.BG, highlightthickness=0)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        bar = ttk.Scrollbar(left, orient="vertical", command=self.canvas.yview, style="Dark.Vertical.TScrollbar")
        bar.grid(row=0, column=1, sticky="ns", padx=(S(6), 0))
        self.canvas.configure(yscrollcommand=bar.set)
        content = tk.Frame(self.canvas, bg=T.BG)
        item = self.canvas.create_window(0, 0, window=content, anchor="nw")
        content.bind("<Configure>", lambda _e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", lambda e: (self.canvas.itemconfigure(item, width=e.width), self.rewrap(e.width)))
        self.content = content

        self.build_start(content)

        # Right: what this launch will do, and the launch button.
        side = tk.Frame(outer, bg=T.BG, width=S(340))
        side.grid(row=1, column=1, sticky="ns", padx=(S(18), 0))
        side.grid_propagate(False)
        side.columnconfigure(0, weight=1)
        side.rowconfigure(3, weight=1)
        self.build_side(side)

    def card(self, master, *args, **kwargs):
        card = Card(self, master, *args, **kwargs)
        card.pack(fill="x", pady=(0, self.S(12)))
        return card

    def build_start(self, master):
        card = self.card(master, "1", tr("How to start"),
                         tr("The mod: skin and hair shading, upscaling (DLSS, XeSS or FSR), neural rendering and frame generation; path tracing is switched in the in-game menu. Vanilla: loads none of the mod's files; for going back and for comparing."))
        row = self.add(card, "variant", tr("Start as"))
        self.segmented(row, "variant", [(tr(name), value) for value, name in VARIANT_NAMES.items()])
        row = self.add(card, "display_mode", tr("Display"))
        self.segmented(row, "display_mode", [(tr(name), value) for value, name in DISPLAY_NAMES.items()])
        # Each language under its own name; the in-game menus read the same choice when VaM starts.
        names = lang.languages()
        if len(names) > 1:
            row = self.add(card, "language", tr("Language"))
            row.control(Segmented(self, row.frame, self.language, [(name, code) for code, name in names]))

    def build_side(self, side):
        S = self.S
        summary = tk.Frame(side, bg=T.SURFACE, highlightthickness=1, highlightbackground=T.BORDER)
        summary.grid(row=0, column=0, sticky="ew")
        tk.Label(summary, text=tr("This launch"), bg=T.SURFACE, fg=T.TEXT, font=(T.FONT, 13, "bold")).pack(anchor="w", padx=S(16), pady=(S(14), S(8)))
        self.steps = {}
        self.step_titles = {}
        titles = (("variant", tr("Start as")), ("render", tr("Render")), ("nr", tr("Neural rendering")), ("upscale", tr("Upscale")),
                  ("framegen", tr("Frame gen")), ("output", tr("→ Headset")))
        # As wide as the longest title of this language (7 holds the Chinese ones).
        title_width = max(7, 1 + max(len(title) for title in (*(title for _, title in titles), tr("→ Monitor"))))
        for key, title in titles:
            line = tk.Frame(summary, bg=T.SURFACE)
            line.pack(fill="x", padx=S(16), pady=S(3))
            title_label = tk.Label(line, text=title, bg=T.SURFACE, fg=T.MUTED, font=(T.FONT, 9), width=title_width, anchor="w")
            title_label.pack(side="left")
            self.step_titles[key] = title_label
            value = tk.Label(line, text="", bg=T.SURFACE, fg=T.TEXT, font=(T.FONT, 10), anchor="w", justify="left")
            value.pack(side="left", fill="x", expand=True)
            value.configure(wraplength=S(220 if title_width == 7 else 200))
            self.steps[key] = value
        self.need = tk.Label(summary, text="", bg=T.SURFACE, fg=T.WARN, font=(T.FONT, 9), justify="left",
                             anchor="w", wraplength=S(300))
        self.need.pack(fill="x", padx=S(16), pady=(S(10), S(14)))

        live = tk.Frame(side, bg=T.SURFACE, highlightthickness=1, highlightbackground=T.BORDER)
        live.grid(row=1, column=0, sticky="ew", pady=(S(12), 0))
        self.sizes_title = tk.Label(live, text=tr("Actual resolution (per eye)"), bg=T.SURFACE, fg=T.TEXT, font=(T.FONT, 11, "bold"))
        self.sizes_title.pack(anchor="w", padx=S(16), pady=(S(12), S(6)))
        self.sizes = tk.Label(live, text="", bg=T.SURFACE, fg=T.MUTED, font=(T.FONT, 9), justify="left",
                              anchor="w", wraplength=S(300))
        self.sizes.pack(fill="x", padx=S(16), pady=(0, S(12)))

        self.status = tk.Label(side, text="", bg=T.BG, fg=T.MUTED, font=(T.FONT, 9), justify="left",
                               anchor="sw", wraplength=S(320))
        self.status.grid(row=3, column=0, sticky="sew", pady=(S(12), S(10)))

        buttons = tk.Frame(side, bg=T.BG)
        buttons.grid(row=4, column=0, sticky="ew")
        buttons.columnconfigure(0, weight=1)
        self.launch_button = tk.Button(buttons, text=tr("Launch VaM"), command=self.launch, font=(T.FONT, 14, "bold"),
                                       bg=T.ACCENT, fg=T.ACCENT_TEXT, activebackground=T.ACCENT_HOVER,
                                       activeforeground=T.ACCENT_TEXT, disabledforeground="#9fb3aa",
                                       relief="flat", bd=0, cursor="hand2", pady=S(10))
        self.launch_button.grid(row=0, column=0, sticky="ew")
        self.logs_button = tk.Button(buttons, text=tr("Open the log folder"), command=self.open_logs, font=(T.FONT, 10),
                                     bg=T.RAISED, fg=T.TEXT, activebackground=T.HOVER, activeforeground=T.TEXT,
                                     relief="flat", bd=0, cursor="hand2", pady=S(6))
        self.logs_button.grid(row=1, column=0, sticky="ew", pady=(S(8), 0))
        self.set_status(tr("Settings are saved as you change them. The game only runs when you click “Launch VaM”."))

    def rewrap(self, width):
        for label, margin in self.wraps:
            label.configure(wraplength=max(self.S(200), width - self.S(margin)))

    def wheel(self, event):
        widget = event.widget
        while widget is not None:
            if widget is self.canvas:
                self.canvas.yview_scroll(-int(event.delta / 120), "units")
                return
            widget = getattr(widget, "master", None)

    def set_status(self, text):
        self.status.configure(text=text)

    # -- behaviour

    def changed(self, key):
        if not self.updating:
            if key == "display_mode" and self.get(key) != self.display_mode:
                if self.running():
                    self.updating = True
                    self.v[key].set(self.display_mode)
                    self.updating = False
                    return
                old = self.collect()
                old["display_mode"] = self.display_mode
                save_settings(old, remember=False)
                self.display_mode = self.get(key)
                loaded = load_settings(self.display_mode)
                self.updating = True
                try:
                    for name, value in loaded.items():
                        self.v[name].set(value)
                finally:
                    self.updating = False
                self.output_size = last_output_size(self.display_mode)
                self.active = self.run = None
            self.refresh()

    def collect(self):
        return validate({key: variable.get() for key, variable in self.v.items()})

    def refresh(self, initial=False):
        running = self.running()
        for widget in self.locks:
            widget.set_locked(running)
        self.refresh_summary()
        self.refresh_sizes()
        if not initial:
            self.schedule_save()

    def refresh_summary(self):
        steps = self.steps
        try:
            s = self.collect()
        except (ValueError, tk.TclError):
            steps["render"].configure(text=tr("Enter a valid number"), fg=T.WARN)
            return
        steps["variant"].configure(text=tr(VARIANT_NAMES[s["variant"]]), fg=T.TEXT)
        if s["variant"] == "vanilla":
            for key in ("render", "nr", "upscale", "framegen", "output"):
                steps[key].configure(text="—", fg=T.MUTED)
            self.need.configure(text=tr("Loads none of the mod's files, as if it was never installed."))
            return
        base = self.output_size
        desktop = s["display_mode"] == "Desktop"
        monitor = monitor_dimensions() if desktop else None
        out = monitor[:2] if desktop else ((round(base[0] * s["output_scale"] / 100), round(base[1] * s["output_scale"] / 100)) if base else None)
        scale = effective_render_scale(s)
        render = f"{scale}%"
        if out and scale != 100:
            # The eye texture is scaled from the output (the DLSS bridge).
            render += tr(" (about {0} × {1})", out[0] * scale // 100, out[1] * scale // 100)
        steps["render"].configure(text=render, fg=T.TEXT)
        after = tr(" · after upscaling") if s["nr_after"] else ""
        steps["nr"].configure(text=tr("On · region {0}%{1}", s["nr_region"], after) if s["nr_enabled"] else tr("Off##choice"),
                              fg=T.TEXT if s["nr_enabled"] else T.MUTED)
        if s["dlss_preset"] == "Off":
            upscale = tr("Upscaling off (native)")
        elif s["dlss_preset"] in DLSS_MODELS:
            upscale = tr("DLSS Super Resolution · model {0}", s["dlss_preset"]) + (" · DLAA" if scale == 100 else "")
        else:
            upscale = tr("{0} upscaling", s["dlss_preset"]) + (tr(" · native anti-aliasing") if scale == 100 else "")
        upscale += tr("; RR · F while path tracing is on")
        if not desktop and s["output_scale"] != 100:
            upscale += tr(" · output {0}%", s["output_scale"])
        steps["upscale"].configure(text=upscale, fg=T.TEXT)
        multiplier = launch_multiplier(s)
        if s["framegen_model"] == "FSR":
            multiplier = 2                      # one generated frame per pair, whatever the multiplier asks for
            framegen = tr("2× · FSR frame generation")
        else:
            framegen = tr("{0}× · DLSS Frame Generation ({1})", multiplier,
                          tr("performance · Preset C") if s["dlss_fg_lite"] else tr("quality"))
        if not s["framegen"]:
            framegen = tr("Off (can be switched on in game) · ") + framegen
        steps["framegen"].configure(text=framegen, fg=T.TEXT if s["framegen"] else T.MUTED)
        steps["output"].configure(text=f"{out[0]} × {out[1]}" if out else tr("Shown after the first launch"), fg=T.TEXT if out else T.MUTED)
        self.step_titles["output"].configure(text=tr("→ Monitor") if desktop else tr("→ Headset"))
        if desktop:
            steps["output"].configure(text=f"{monitor[0]} × {monitor[1]} @ {monitor[2]} Hz")
        if desktop and s["framegen"]:
            self.need.configure(text=tr("{0} Hz needs a steady {1} real fps.", monitor[2], f"{monitor[2] / multiplier:g}"))
        elif s["framegen"]:
            self.need.configure(text=tr("144 Hz needs a steady {0} real fps; 120 Hz needs {1}. Switch SSW off in Virtual Desktop.",
                                        f"{144 / multiplier:g}", f"{120 / multiplier:g}"))
        else:
            self.need.configure(text="")
        notes = launch_notes(s)
        if notes:
            self.need.configure(text=chr(10).join(filter(None, [self.need.cget("text")] + notes)))

    def refresh_sizes(self):
        desktop = self.get("display_mode") == "Desktop"
        self.sizes_title.configure(text=tr("Actual window resolution") if desktop else tr("Actual resolution (per eye)"))
        if self.get("variant") == "vanilla":
            self.sizes.configure(text=tr("Not shown for vanilla."), fg=T.MUTED)
            return
        if not self.running():
            if desktop:
                self.sizes.configure(text=tr("After launch: this run's render and output size of the window."), fg=T.MUTED)
                return
            if self.output_size:
                self.sizes.configure(text=tr("After launch: this run's actual render and output size per eye.") + "\n" +
                                          tr("Last headset output: {0} × {1}", *self.output_size), fg=T.MUTED)
            else:
                self.sizes.configure(text=tr("After launch: this run's actual render and output size per eye."), fg=T.MUTED)
            return
        lines = []
        for eye, label in enumerate((tr("Window"),) if desktop else (tr("Left eye"), tr("Right eye"))):
            sizes = submitted_dimensions(self.run, eye)
            if sizes is None:
                lines.append(tr("{0}: waiting for the first frame…", label))
            elif sizes == "fallback":
                lines.append(tr("{0}: upscaling failed and frames are submitted as they are; see the log", label))
            else:
                w, h, ow, oh = sizes
                lines.append(tr("{0}: {1} × {2} → {3} × {4}", label, w, h, ow, oh))
                if eye == 0:
                    # The runtime's size: the output without this run's OutputScale.
                    scale = output_scale_of(self.run / "performance.ini")
                    self.output_size = (round(ow * 100 / scale), round(oh * 100 / scale))
        self.sizes.configure(text="\n".join(lines), fg=T.TEXT)

    def schedule_save(self):
        if self.save_job:
            self.root.after_cancel(self.save_job)
        self.save_job = self.root.after(400, self.save_now)

    def save_now(self):
        self.save_job = None
        try:
            save_settings(self.collect())
        except (ValueError, tk.TclError):
            pass  # an unfinished number in a box; saved once it is valid
        except OSError as error:
            self.set_status(tr("Saving failed: {0}", error))

    def adopt_live(self):
        """Take over what the in-game "VR Graphics" tab wrote into this run's performance.ini
        (its settings, GAME_INI) and keep it for the next launch."""
        if self.run is None or self.active is None or self.live_text is None:
            return
        self.adopt_multiplier()
        try:
            text = (self.run / "performance.ini").read_text(encoding="utf-8-sig")
        except OSError:
            return
        if text == self.live_text:
            return
        written, found = read_live(self.live_text), read_live(text)
        self.live_text = text
        taken = {key: value for key, value in found.items()
                 if written.get(key) != value and self.get(key) == self.active.get(key)}
        if not taken:
            return
        try:
            validate(dict(self.active, **taken))
        except (ValueError, KeyError, TypeError):
            self.set_status(tr("The in-game menu wrote a setting that is not valid; it was not taken."))
            return
        self.active.update(taken)
        self.updating = True
        try:
            for key, value in taken.items():
                self.v[key].set(value)
        finally:
            self.updating = False
        self.refresh()
        self.set_status(tr("The in-game menu changed settings; the next launch keeps them."))

    def adopt_multiplier(self):
        """Take over the frame multiplier the in-game tab chose and keep it for the next
        launch: [ofxr] frame_multiplier in the OFXR layer's ofxr_bridge.ini. Only the tab
        writes that key (the tray drops it whenever it rewrites the file), and a file not
        written since this launch started is the previous run's."""
        path = layer_ini(dict(self.active, run_directory=str(self.run)))
        try:
            if path.stat().st_mtime < self.launched_at:
                return
            found = read_frame_multiplier(path.read_text(encoding="utf-8-sig", errors="replace"))
        except OSError:
            return
        if found is None or found == self.active["frame_multiplier"]:
            return
        self.active["frame_multiplier"] = found
        self.updating = True
        try:
            self.v["frame_multiplier"].set(found)
        finally:
            self.updating = False
        self.refresh()
        self.set_status(tr("The in-game menu set the frame multiplier to {0}×; the next launch keeps it.", found))

    def launch(self):
        if self.running():
            return
        self.launched_at = dt.datetime.now().timestamp()
        try:
            self.child, self.run, self.active = begin_launch(self.collect())
        except (OSError, ValueError, tk.TclError) as error:
            messagebox.showerror(tr("Cannot launch"), str(error), parent=self.root)
            return
        self.live_text = performance_ini(self.active)
        self.paint_running()
        # The tab's name as the in-game menu shows it (VaMVRMenu.Menu.cs MenuTabLabel).
        self.set_status(tr("Starting. Path tracing, the upscaler model, the render scale, neural rendering and frame generation are changed in the “{0}” tab of the game's main menu; VaM's own files are put back when it exits, and closing this window does not stop the game.",
                           tr("Graphics") if self.active["display_mode"] == "Desktop" else tr("VR Graphics")))
        self.refresh()
        self.root.after(800, self.poll)

    def poll(self):
        code = self.child.poll()
        self.adopt_live()
        if code is None:
            self.refresh_sizes()
            self.root.after(800, self.poll)
            return
        self.paint_running()
        self.refresh()
        if code:
            self.set_status(tr("Starting or putting the files back did not finish; open the log for the reason."))
            text = (self.run / "launcher.log").read_text(encoding="utf-8-sig", errors="replace")
            messagebox.showerror(tr("The launcher reports an error"), text[-2600:] or tr("exit code {0}", code), parent=self.root)
        else:
            self.set_status(tr("VaM has exited and its own files are back. Change settings and launch again when you like."))

    def open_logs(self):
        os.startfile(self.run if self.run else ROOT.parent / "logs")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", type=int, choices=(2, 3, 4))
    args = parser.parse_args()
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    LaunchPanel(root, args.preset)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
