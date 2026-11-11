"""vamkit install, verify, uninstall and rollback: what a build made goes into a VaM folder, and comes out again.

What goes where is in manifest/components.toml (`installs`, `enable`) and manifest/thirdparty.toml (`installs`).
Everything written into the VaM folder is recorded in <VaM>/vamkit/installed.json, with what was there before:
  - a file that did not exist is deleted again by uninstall;
  - a file that did (someone else's, under the same name; a plugin's cfg) is kept in <VaM>/vamkit/original/replaced/
    and put back;
  - a setting written into a cfg that has changed since is set back key by key, to the value it had.
VaM's own openvr_api.dll, a d3d11.dll beside VaM.exe and opencomposite.ini are not replaced by the install: the
launcher swaps them while the mod runs. The install records what it finds there in <VaM>/vamkit/original/ (the file, or
`<name>.absent`), which is what the launcher puts back after a run; the launcher keeps that record current.
Every install first copies what it is about to change to <VaM>/vamkit/backup/<time>/ and writes a restore.cmd there
that puts it straight back: one step of rollback (`vamkit rollback` does the same).

Nothing here starts VaM. Every command that changes the VaM folder prints its plan and does nothing without --yes.
"""
import ctypes
import datetime
import hashlib
import json
import os
import re
import shutil
import sys
from ctypes import wintypes
from pathlib import Path

KIT = "vamkit"
RECORD = KIT + "/installed.json"
ORIGINAL = KIT + "/original"
REPLACED = ORIGINAL + "/replaced"
BACKUP = KIT + "/backup"
BIN = KIT + "/bin"
# Made while the mod is used; the user's, so uninstall keeps them unless --purge.
USER_DATA = ((KIT + "/logs", "the runs' logs"), (KIT + "/launcher/profiles", "the launch presets"))
# Logs the mod's binaries write beside VaM.exe while it runs; uninstall removes them.
RUN_LOGS = ("d3d11_proxy.log", "openovr_log")
# Folders the mod makes for itself while it runs and can make again (vamkit/cache/ngx: the vendors' run-time files it
# found on this machine and copied for use); uninstall removes them.
MADE = (KIT + "/cache",)
# What the launcher swaps while the mod runs: name in vamkit/original and vamkit/bin, path under the VaM folder.
LIVE = (("openvr_api.dll", "VaM_Data/Plugins/openvr_api.dll"), ("d3d11.dll", "d3d11.dll"),
        ("opencomposite.ini", "VaM_Data/Plugins/opencomposite.ini"))
OC_MARK = "; vamkit"   # the line launch.py adds to opencomposite.ini


class Kit:
    """Where the files come from: a repository's manifests and what its build and fetch produced."""

    def __init__(self, repo, components, items, fetched, runtimes=None):
        self.repo, self.components, self.items, self.fetched = Path(repo), components, items, fetched
        self.runtimes = runtimes or (lambda: [])   # the run-time files: present or not, what is lost, how to add


def ini():
    """The launcher's cfg reader and patcher (keeps comments, unknown keys, BOM and line endings)."""
    folder = str(Path(__file__).resolve().parents[2] / "components" / "render" / "launcher")
    if folder not in sys.path:
        sys.path.insert(0, folder)
    import ModeProfiles
    return ModeProfiles


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def vam_running(vam):
    """True when a process runs this folder's VaM.exe."""
    psapi, kernel = ctypes.WinDLL("psapi"), ctypes.WinDLL("kernel32")
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    ids, got = (wintypes.DWORD * 16384)(), wintypes.DWORD()
    if not psapi.EnumProcesses(ids, ctypes.sizeof(ids), ctypes.byref(got)):
        return False
    game = os.path.normcase(str((Path(vam) / "VaM.exe").resolve()))
    for pid in ids[: got.value // ctypes.sizeof(wintypes.DWORD)]:
        handle = kernel.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            continue
        try:
            name, size = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            if kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)) and os.path.normcase(name.value) == game:
                return True
        finally:
            kernel.CloseHandle(handle)
    return False


def read_record(vam):
    try:
        return json.loads((vam / RECORD).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def ours_in_place(vam, name, path):
    """True when the file in VaM's place is the one the mod puts there (the mod is running, or a run was killed)."""
    there = vam / path
    if not there.is_file():
        return False
    if name == "opencomposite.ini":
        return OC_MARK in there.read_text(encoding="utf-8", errors="replace")
    mine = vam / BIN / name
    return mine.is_file() and mine.stat().st_size == there.stat().st_size and sha256(mine) == sha256(there)


def entries(kit):
    """(source file, path under the VaM folder, owner) for everything the manifests install; what is missing as text."""
    found, problems, notes = [], [], []
    for c in kit.components:
        for source, target in c.get("installs", []):
            path = kit.repo / source
            if path.is_file():
                found.append((path, target, c["id"]))
            else:
                problems.append(f"{c['id']}: {source} is not there (python tools/vamkit build {c['id']})")
    for i in kit.items:
        target = i.get("installs")
        if not target:
            continue
        path = kit.repo / "third_party" / i["id"] / i["name"]
        if kit.fetched(i) and path.is_file():
            found.append((path, target, i["id"]))
        elif i.get("optional"):
            pass   # said by name, with what it costs, in the plan's own section (missing_runtimes)
        else:
            problems.append(f"{i['id']}: {i['name']} is not fetched (python tools/vamkit fetch {i['id']})")
    return found, problems, notes


def plugin_settings(kit):
    """The settings files the installed plugins make and keep while they are used: BepInEx's <guid>.cfg of every
    plugin, and the files the launcher's presets carry. They are the user's; uninstall names them."""
    found = set()
    for c in kit.components:
        if not c.get("path"):
            continue
        if any(target.startswith("BepInEx/plugins/") for _, target in c.get("installs", [])):
            for source in (kit.repo / c["path"] / name for name in c.get("managed", {}).get("sources", [])):
                for guid in re.findall(r'\[BepInPlugin\("([^"]+)"', source.read_text(encoding="utf-8", errors="replace")):
                    found.add(f"BepInEx/config/{guid}.cfg")
        manifest = kit.repo / c["path"] / "profile-manifest.json"
        if manifest.is_file():
            presets = json.loads(manifest.read_text(encoding="utf-8"))
            found.update(presets.get("config", {}))
            found.update(presets.get("whole_files", []))
    return sorted(found)


def same_value(a, b):
    return a is not None and b is not None and str(a).strip().lower() == str(b).strip().lower()


def make_plan(kit, vam):
    record = read_record(vam) or {}
    ours = {f["path"]: f for f in record.get("files", [])}
    found, problems, notes = entries(kit)
    files = []
    for source, target, owner in found:
        there, digest = vam / target, sha256(source)
        if not there.is_file():
            state = "new"
        elif sha256(there) == digest:
            state = "same"
        else:
            state = "update" if target in ours else "replace"
        files.append(dict(source=str(source), path=target, owner=owner, sha256=digest, state=state,
                          adopt=there.is_file() and target not in ours))
    config = []
    for c in kit.components:
        for cfg, section, key, value in c.get("enable", []):
            now = ini().ini_values(vam / cfg, [(section, key)])[section][key]
            config.append(dict(file=cfg, section=section, key=key, value=value, now=now, owner=c["id"],
                               state="same" if same_value(now, value) else "set"))
    planned = {f["path"] for f in files}
    stale = [f for path, f in ours.items() if path not in planned]
    live = []
    for name, path in LIVE:
        recorded = (vam / ORIGINAL / name).is_file() or (vam / ORIGINAL / (name + ".absent")).is_file()
        live.append(dict(name=name, path=path, there=(vam / path).is_file(), recorded=recorded))
    if not (vam / "BepInEx" / "core" / "BepInEx.dll").is_file():
        problems.append("BepInEx 5 is not in this VaM folder (BepInEx/core/BepInEx.dll): the plugins would never load. "
                        "python tools/vamkit bepinex --yes puts it in; the plugins are built against it, so build after that")
    if (vam / "d3d11.dll").is_file() and not any(l["recorded"] for l in live if l["name"] == "d3d11.dll"):
        notes.append("a d3d11.dll is beside VaM.exe now: it is recorded as VaM's own and comes back in vanilla runs. "
                     "If it belongs to another injector or an earlier copy of this project, remove it first")
    pending = (not record or any(f["state"] != "same" or f["adopt"] for f in files) or any(c["state"] == "set" for c in config)
               or bool(stale) or any(not l["recorded"] for l in live))
    return dict(vam=str(vam), files=files, config=config, stale=stale, live=live, problems=problems, notes=notes,
                pending=pending, running=vam_running(vam), missing=[r for r in kit.runtimes() if not r["present"]])


def show_plan(plan):
    print(f"install into {plan['vam']}")
    words = dict(new="new    ", update="update ", replace="replace")
    for f in plan["files"]:
        if f["state"] != "same":
            extra = "  (the file there now is kept in vamkit/original/replaced/)" if f["state"] == "replace" else ""
            print(f"  {words[f['state']]}  {f['path']}  [{f['owner']}]{extra}")
    same = sum(f["state"] == "same" for f in plan["files"])
    if same:
        print(f"  {same} file(s) are already as built")
    for c in plan["config"]:
        if c["state"] == "set":
            print(f"  set      {c['file']}  [{c['section']}] {c['key']} = {c['value']}  (now: {c['now'] if c['now'] is not None else 'not set'})")
    for f in plan["stale"]:
        print(f"  remove   {f['path']}  (no longer part of the kit)")
    for l in plan["live"]:
        if not l["recorded"]:
            what = f"VaM's {l['path']} is copied to" if l["there"] else f"there is no {l['path']}: noted in"
            print(f"  record   {what} vamkit/original/")
    for line in plan["notes"]:
        print(f"  note     {line}")
    if plan.get("missing"):
        print("  Not installed because the files are not in third_party/. The kit works without them, and the user gets less:")
        for r in plan["missing"]:
            print(f"  MISSING  {r['name']}  ({r['id']})")
            print(f"           you do not get: {r['without']}")
            print(f"           to add it: {r['how']}, then install again")
    for line in plan["problems"]:
        print(f"  FAIL     {line}")


class Change:
    """The files one command writes and deletes. With a backup folder, each is first copied there (or noted as new)."""

    def __init__(self, vam, backup):
        self.vam, self.folder = vam, None
        self.replaced, self.created, self.dirs, self.seen = [], [], [], set()
        if backup:
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            name, n = stamp, 1
            while (vam / BACKUP / name).exists():
                n += 1
                name = f"{stamp}-{n}"
            self.folder = vam / BACKUP / name

    def touch(self, target):
        """Before `target` (a path under the VaM folder) is written or deleted."""
        if target in self.seen:
            return
        self.seen.add(target)
        path = self.vam / target
        if path.is_file():
            self.replaced.append(target)
            if self.folder:
                keep = self.folder / "files" / target
                keep.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, keep)
            return
        self.created.append(target)
        missing, parent = [], path.parent
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for folder in reversed(missing):
            folder.mkdir()
            self.dirs.append(folder.relative_to(self.vam).as_posix())

    def copy(self, source, target):
        self.touch(target)
        path = self.vam / target
        temporary = path.with_name(path.name + ".vamkit-tmp")
        shutil.copy2(source, temporary)
        os.replace(temporary, path)

    def write(self, target, data):
        self.touch(target)
        path = self.vam / target
        temporary = path.with_name(path.name + ".vamkit-tmp")
        temporary.write_bytes(data)
        os.replace(temporary, path)

    def delete(self, target):
        self.touch(target)
        (self.vam / target).unlink(missing_ok=True)

    def finish(self):
        """backup.json and restore.cmd: what `vamkit rollback`, or a double click, needs to undo this step."""
        if not self.folder:
            return
        self.folder.mkdir(parents=True, exist_ok=True)
        (self.folder / "backup.json").write_text(json.dumps(dict(replaced=self.replaced, created=self.created, dirs=self.dirs), indent=2), encoding="utf-8")
        lines = ["@echo off", "rem Puts back what this vamkit install replaced and removes what it added. Close VaM first.",
                 "setlocal", 'set "HERE=%~dp0"', 'for %%I in ("%HERE%..\\..\\..") do set "VAM=%%~fI"',
                 'tasklist /FI "IMAGENAME eq VaM.exe" /NH | find /I "VaM.exe" >NUL && (echo VaM is running: close it first & exit /b 1)']
        for target in self.created:
            lines.append(f'del "%VAM%\\{target.replace("/", chr(92))}" 2>NUL')
        for target in self.replaced:
            t = target.replace("/", "\\")
            lines.append(f'copy /y "%HERE%files\\{t}" "%VAM%\\{t}" >NUL || goto failed')
        lines += ["echo put back: %VAM%", "exit /b 0", ":failed", "echo a file could not be put back", "exit /b 1"]
        (self.folder / "restore.cmd").write_text("\r\n".join(lines) + "\r\n", encoding="utf-8", newline="")


def prune(vam, dirs=()):
    """Removes the folders an install made, where they are empty now, and empty folders under <VaM>/vamkit."""
    for folder in sorted(dirs, key=len, reverse=True):
        try:
            (vam / folder).rmdir()
        except OSError:
            pass
    kit = vam / KIT
    if kit.is_dir():
        for root, _, _ in os.walk(kit, topdown=False):
            try:
                os.rmdir(root)
            except OSError:
                pass


def put_pristine(change, vam, f):
    """One installed file goes: deleted, or the file that was there before comes back."""
    if f["was"] == "kept" and (vam / REPLACED / f["path"]).is_file():
        change.copy(vam / REPLACED / f["path"], f["path"])
        change.delete(REPLACED + "/" + f["path"])
    else:
        change.delete(f["path"])


def install(kit, vam, yes, as_json=False, replace=False):
    plan = make_plan(kit, vam)
    if as_json:
        print(json.dumps(plan, indent=2))
    else:
        show_plan(plan)
    if plan["problems"]:
        return 1
    if not plan["pending"]:
        if not as_json:
            print("nothing to do: this VaM folder holds this build")
        return 0
    foreign = [f["path"] for f in plan["files"] if f["state"] == "replace"]
    if foreign and not replace and not as_json:
        print(f"  {len(foreign)} file(s) under the kit's names are in this VaM folder already and were not installed by vamkit (another copy of")
        print("  this project, installed by hand?). --replace installs over them and keeps them in vamkit/original/replaced/.")
    if not yes:
        if not as_json:
            print("nothing was changed. Run again with --yes to install.")
        return 0
    if foreign and not replace:
        print("not installed: --replace is needed to install over files that are not vamkit's", file=sys.stderr)
        return 1
    if plan["running"]:
        print("VaM is running from this folder: close it first", file=sys.stderr)
        return 1

    record = read_record(vam) or {}
    ours = {f["path"]: f for f in record.get("files", [])}
    change = Change(vam, backup=True)
    for name, path in LIVE:   # VaM's own files as found, once
        kept, marker = ORIGINAL + "/" + name, ORIGINAL + "/" + name + ".absent"
        if (vam / kept).is_file() or (vam / marker).is_file():
            continue
        if (vam / path).is_file():
            change.copy(vam / path, kept)
        else:
            change.write(marker, b"")
    files = []
    for f in plan["files"]:
        target = f["path"]
        was = ours.get(target, {}).get("was")
        if was is None:
            was = "absent"
            if (vam / target).is_file():
                change.copy(vam / target, REPLACED + "/" + target)
                was = "kept"
        if f["state"] != "same":
            change.copy(f["source"], target)
            if sha256(vam / target) != f["sha256"]:
                raise SystemExit(f"{target}: the copy differs from its source")
        files.append(dict(path=target, sha256=f["sha256"], component=f["owner"], was=was))
    for f in plan["stale"]:
        put_pristine(change, vam, f)

    cfg_files = dict(record.get("config_files", {}))
    keys = {(c["file"], c["section"].lower(), c["key"].lower()): c for c in record.get("config", [])}
    config = []
    for c in plan["config"]:
        cfg = c["file"]
        if cfg not in cfg_files:
            cfg_files[cfg] = dict(was="absent")
            if (vam / cfg).is_file():
                change.copy(vam / cfg, REPLACED + "/" + cfg)
                cfg_files[cfg] = dict(was="kept")
        before = keys.get((cfg, c["section"].lower(), c["key"].lower()))
        if c["state"] == "set":
            change.touch(cfg)
            ini().patch_ini(vam / cfg, {c["section"]: {c["key"]: c["value"]}})
        config.append(dict(file=cfg, section=c["section"], key=c["key"], value=c["value"], component=c["owner"],
                           was=before["was"] if before else c["now"]))
    for cfg in cfg_files:
        if (vam / cfg).is_file():
            cfg_files[cfg]["sha256"] = sha256(vam / cfg)

    new = dict(kit=1, time=datetime.datetime.now().isoformat(timespec="seconds"), repo=str(kit.repo), files=files, config=config,
               config_files=cfg_files, settings=plugin_settings(kit), rollback=BACKUP + "/" + change.folder.name)
    change.touch(RECORD)
    new["dirs"] = sorted(set(record.get("dirs", [])) | set(change.dirs))
    change.write(RECORD, (json.dumps(new, indent=2) + "\n").encode("utf-8"))
    change.finish()
    copied = sum(f["state"] != "same" for f in plan["files"])
    print(f"installed: {copied} file(s) copied and checked, {sum(c['state'] == 'set' for c in plan['config'])} setting(s) written")
    print(f"one step back: {change.folder / 'restore.cmd'}  (or: python tools/vamkit rollback --yes)")
    return 0


def rollback(vam, yes):
    folder = vam / BACKUP
    steps = sorted(d for d in folder.iterdir() if (d / "backup.json").is_file()) if folder.is_dir() else []
    if not steps:
        print("no install to take back: there is no backup in this VaM folder")
        return 1
    last = steps[-1]
    info = json.loads((last / "backup.json").read_text(encoding="utf-8"))
    print(f"rollback of the install of {last.name} in {vam}")
    print(f"  {len(info['replaced'])} file(s) put back as they were, {len(info['created'])} file(s) it added removed")
    if not yes:
        print("nothing was changed. Run again with --yes.")
        return 0
    if vam_running(vam):
        print("VaM is running from this folder: close it first", file=sys.stderr)
        return 1
    for target in info["created"]:
        (vam / target).unlink(missing_ok=True)
    for target in info["replaced"]:
        (vam / target).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(last / "files" / target, vam / target)
    shutil.rmtree(last)
    prune(vam, info.get("dirs", []))
    print("done" + ("" if read_record(vam) else ": nothing of the kit is installed now"))
    return 0


def uninstall(vam, yes, purge=False):
    record = read_record(vam)
    if record is None:
        print(f"nothing is recorded as installed in {vam}")
        return 1
    print(f"uninstall from {vam}")
    files = record.get("files", [])
    changed = [f for f in files if (vam / f["path"]).is_file() and sha256(vam / f["path"]) != f["sha256"]]
    print(f"  {len(files) - len(changed)} installed file(s) removed" + (f", {sum(f['was'] == 'kept' for f in files)} of them replaced by the file that was there before" if any(f["was"] == "kept" for f in files) else ""))
    for f in changed:
        print(f"  left     {f['path']}  (changed since the install)")
    for name, path in LIVE:
        if ours_in_place(vam, name, path):
            print(f"  put back VaM's own {path}  (the mod's is in its place)")
    for cfg, state in record.get("config_files", {}).items():
        if not (vam / cfg).is_file():
            continue
        if state.get("sha256") == sha256(vam / cfg):
            print(f"  {'put back' if state['was'] == 'kept' else 'remove  '} {cfg}")
        elif state["was"] == "kept":
            print(f"  settings {cfg}: the keys the install wrote go back to their values, the rest stays")
        else:
            print(f"  left     {cfg}  (the plugin's settings, written since the install)")
    made = [p for p in record.get("settings", []) if p not in record.get("config_files", {}) and (vam / p).is_file()]
    for path in made:
        print(f"  {'remove  ' if purge else 'left    '} {path}  (a plugin's settings, made while the mod was used{'' if purge else '; --purge removes them'})")
    for path, what in USER_DATA:
        if (vam / path).exists():
            print(f"  {'remove  ' if purge else 'left    '} {path}  ({what}{'' if purge else '; --purge removes them'})")
    for name in RUN_LOGS:
        if (vam / name).is_file():
            print(f"  remove   {name}  (a log of the mod)")
    for path in MADE:
        if (vam / path).exists():
            print(f"  remove   {path}  (made by the mod while it ran; it can make it again)")
    import bepinex   # a BepInEx that `vamkit bepinex` put in: other plugins may use it by now, so it stays unless --purge
    loader = bepinex.remove_plan(vam)
    if loader is not None:
        print(f"  {'remove  ' if purge else 'left    '} BepInEx {loader[0].get('version', '')}  (put in by vamkit bepinex"
              f"{'' if purge else '; --purge, or python tools/vamkit bepinex --remove --yes, takes it out'})")
    if not yes:
        print("nothing was changed. Run again with --yes.")
        return 0
    if vam_running(vam):
        print("VaM is running from this folder: close it first", file=sys.stderr)
        return 1

    change = Change(vam, backup=False)
    for name, path in LIVE:   # a run that was killed left the mod's files in VaM's place
        if ours_in_place(vam, name, path):
            if (vam / ORIGINAL / name).is_file():
                change.copy(vam / ORIGINAL / name, path)
            else:
                change.delete(path)
    left = [f["path"] for f in changed]
    for f in files:
        if f not in changed:
            put_pristine(change, vam, f)
    for cfg, state in record.get("config_files", {}).items():
        path = vam / cfg
        if not path.is_file():
            continue
        if state.get("sha256") == sha256(path):
            put_pristine(change, vam, dict(path=cfg, was=state["was"]))
        elif state["was"] == "kept":
            for c in record.get("config", []):
                if c["file"] == cfg and same_value(ini().ini_values(path, [(c["section"], c["key"])])[c["section"]][c["key"]], c["value"]):
                    ini().patch_ini(path, {c["section"]: {c["key"]: c["was"]}})
            change.delete(REPLACED + "/" + cfg)
        else:
            left.append(cfg)
    for name, _ in LIVE:
        change.delete(ORIGINAL + "/" + name)
        change.delete(ORIGINAL + "/" + name + ".absent")
    change.delete(RECORD)
    shutil.rmtree(vam / BACKUP, ignore_errors=True)
    shutil.rmtree(vam / KIT / "launcher" / "__pycache__", ignore_errors=True)
    for path in made:
        if purge:
            change.delete(path)
        else:
            left.append(path)
    for name in RUN_LOGS:
        change.delete(name)
    for path in MADE:
        shutil.rmtree(vam / path, ignore_errors=True)
    for path, _ in USER_DATA:
        if (vam / path).exists():
            if purge:
                shutil.rmtree(vam / path, ignore_errors=True)
            else:
                left.append(path)
    if loader is not None:
        if purge:
            bepinex.remove(vam, True, quiet=True)
        else:
            left.append("BepInEx")
    prune(vam, record.get("dirs", []))
    print("uninstalled" + (": what is listed as left stays in the VaM folder" if left else ": the VaM folder is as it was before the install"))
    return 0


def verify(kit, vam, as_json=False):
    """Static check of an install: the record against the VaM folder and against the build. Starts nothing."""
    checks = []

    def check(ok, name, detail, need=True):
        checks.append(dict(name=name, ok=bool(ok), required=need, detail=detail))

    record = read_record(vam)
    check(record is not None, "install record", str(vam / RECORD) if record else "not installed: there is no vamkit/installed.json in this VaM folder")
    if record:
        files = record.get("files", [])
        missing = [f["path"] for f in files if not (vam / f["path"]).is_file()]
        changed = [f["path"] for f in files if (vam / f["path"]).is_file() and sha256(vam / f["path"]) != f["sha256"]]
        check(not missing and not changed, "installed files",
              f"{len(files)} file(s) as installed" if not missing and not changed else
              "; ".join(([f"missing: {', '.join(missing)}"] if missing else []) + ([f"changed since the install: {', '.join(changed)}"] if changed else [])))
        installed = {f["path"]: f["sha256"] for f in files}
        found, unbuilt, _ = entries(kit)
        newer = [target for source, target, _ in found if target not in installed or installed[target] != sha256(source)]
        if unbuilt:
            check(False, "against the build", f"not compared: {len(unbuilt)} file(s) of the kit are not built or fetched here", need=False)
        else:
            check(not newer, "against the build", "the install is this build" if not newer else
                  f"{len(newer)} file(s) differ from what is built now (python tools/vamkit install): {', '.join(newer[:6])}{' ...' if len(newer) > 6 else ''}", need=False)
        other = []
        for c in record.get("config", []):
            now = ini().ini_values(vam / c["file"], [(c["section"], c["key"])])[c["section"]][c["key"]]
            if not same_value(now, c["value"]):
                other.append(f"{c['file']} [{c['section']}] {c['key']} = {now} (the install wrote {c['value']})")
        check(not other, "settings the install wrote", "as written" if not other else "; ".join(other), need=False)
        how = {r["id"]: r["how"] for r in kit.runtimes()}
        for i in kit.items:   # a run-time file that is not installed: said by name, with what the user does not get
            target = i.get("installs")
            if target and not (vam / target).is_file():
                check(False, f"run-time file {i['name']}", f"not installed. You do not get: {i.get('without', i.get('what', ''))}. "
                      f"To add it: {how.get(i['id'], 'python tools/vamkit fetch ' + i['id'])}, then install again", need=False)
        running = vam_running(vam)
        for name, path in LIVE:
            recorded = (vam / ORIGINAL / name).is_file() or (vam / ORIGINAL / (name + ".absent")).is_file()
            if not recorded:
                check(False, f"VaM's own {name}", "no record in vamkit/original: install again")
            elif ours_in_place(vam, name, path):
                check(running, f"VaM's own {name}", "the mod's file is in its place" + (" (the mod is running)" if running else
                      ": a run was killed. python <VaM>/vamkit/launcher/launch.py --restore puts VaM's own back"))
            else:
                check(True, f"VaM's own {name}", "in place" if (vam / path).is_file() else "none, as found")
        check((vam / "BepInEx" / "core" / "BepInEx.dll").is_file() and (vam / "winhttp.dll").is_file(), "BepInEx", "present" if (vam / "winhttp.dll").is_file() else "BepInEx or its winhttp.dll is gone: the plugins will not load")
        step = vam / record.get("rollback", "")
        check((step / "backup.json").is_file(), "one step back", str(step / "restore.cmd") if (step / "backup.json").is_file() else "the last install's backup folder is gone", need=False)
    if as_json:
        print(json.dumps(checks, indent=2))
    else:
        for c in checks:
            word = "ok  " if c["ok"] else ("FAIL" if c["required"] else ("LESS" if c["name"].startswith("run-time file") else "note"))
            print(f"{word}  {c['name']}: {c['detail']}")
    return 0 if all(c["ok"] or not c["required"] for c in checks) else 1
