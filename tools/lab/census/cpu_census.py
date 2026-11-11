"""Which tasks take VaM's CPU, in one background SimXR run: a heavy two-person dance scene with a detailed appearance, Timeline playing,
144 Hz, the installed plugins as they are. Two windows in the same VaM: frame generation 4x (about 36 real frames, as
played) and off (the CPU-heavier, higher frame rate). Three views of each window:
  - main thread per Unity subsystem: Extras\\FrameClock\\tests\\bin\\PhaseProbe.dll
  - our plugins, on every thread they are entered on: census\\bin\\CensusProbe.dll
  - every thread of the process and every logical CPU, from outside: census\\thread_census.py
Both probes are taken out of BepInEx\\plugins afterwards. --candidate DIR: the files in DIR replace the plugins of the
same name for this run only (the replaced ones are kept in OUT\\replaced and put back, hashes checked).
    python -u cpu_census.py [OUT] [--candidate DIR]       # default OUT census\\results\\<time>
"""
import argparse, hashlib, json, shutil, sys, time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
VAM = HERE.parents[2]
sys.path.insert(0, str(VAM / 'Extras' / 'VRFrameGen' / 'tools' / 'simxr'))
sys.path.insert(0, str(VAM / 'Extras' / 'FrameClock' / 'tools'))
sys.path.insert(0, str(HERE))
import shoot  # noqa: E402
from vamrun import VamRun  # noqa: E402
import physics_budget as pb  # noqa: E402
from thread_census import Census  # noqa: E402

PLUGINS = VAM / 'BepInEx' / 'plugins'
CONFIG = VAM / 'BepInEx' / 'config'
VRFG = VAM / 'Extras' / 'VRFrameGen'
PROBES = [('PhaseProbe.dll', VAM / 'Extras' / 'FrameClock' / 'tests' / 'bin' / 'PhaseProbe.dll'),
          ('CensusProbe.dll', HERE / 'bin' / 'CensusProbe.dll')]
SECONDS = 10.0


def call(s, typ, method, *args):
    return pb.call(s, typ, method, *args)


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()[:8]


def swap_back(saved):
    """the replaced plugins back (retrying while VaM lets go of them); True when every hash is the original"""
    ok = True
    for name, (backup, digest) in saved.items():
        target = CONFIG / name[len('config/'):] if name.startswith('config/') else PLUGINS / name
        for _ in range(60):
            try:
                shutil.copy2(backup, target)
                break
            except OSError:
                time.sleep(1)
        ok &= target.exists() and sha(target) == digest
    return ok


def main(out, candidate=None):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (VRFG / 'active-test.json').exists() or shoot.vam_running():
        raise SystemExit('VaM is occupied')
    for name, _ in PROBES:
        if (PLUGINS / name).exists():
            raise SystemExit('a probe is already in plugins: ' + name)
    result = {'scene': pb.SCENE, 'look': pb.LOOK}
    census = None
    saved = {}
    shoot.VamRun = lambda *a, **kw: VamRun(*a, sim_env={'SIMXR_REFRESH': 144}, **kw)
    try:
        if candidate:
            files = [f for f in sorted(Path(candidate).iterdir()) if f.is_file()]
            missing = [f.name for f in files if not (PLUGINS / f.name).exists()]
            if missing:
                raise SystemExit('candidate files with no installed counterpart: ' + ', '.join(missing))
            (out / 'replaced').mkdir(exist_ok=True)
            # BepInEx rewrites a candidate's cfg (its header, dropped settings): every cfg goes back too
            for c in CONFIG.glob('*.cfg'):
                backup = out / 'replaced' / c.name
                shutil.copy2(c, backup)
                saved['config/' + c.name] = (backup, sha(backup))
            result['candidate'] = {}
            for f in files:
                backup = out / 'replaced' / f.name
                shutil.copy2(PLUGINS / f.name, backup)
                saved[f.name] = (backup, sha(backup))
                shutil.copy2(f, PLUGINS / f.name)
                result['candidate'][f.name] = {'installed': saved[f.name][1], 'candidate': sha(f)}
        for name, src in PROBES:
            shutil.copy2(src, PLUGINS / name)
        with shoot.Shoot('cpu-census', out, budget_s=320, ini={('Performance', 'FrameGeneration'): 1},
                         overrides={'frame_multiplier': 4}) as s:
            s.load(pb.SCENE, freeze=False)
            result['log_dir'] = s.log_dir
            result['look_load'] = call(s, 'PhaseProbe', 'LoadLook', '#Girl', pb.LOOK)
            has_soft_body = 'local.vam.softbody' in s.run.call_agent('status').get('plugins', [])
            deadline = time.monotonic() + 45
            while not call(s, 'PhaseProbe', 'Ready', '#Girl').startswith('ready '):
                if time.monotonic() > deadline:
                    raise RuntimeError('appearance not ready')
                time.sleep(.5)
            time.sleep(2.2)
            census = Census(s.run.vam_pid).start()
            for mode in ('fg4', 'fgoff'):
                r = result[mode] = {}
                d = out / mode
                d.mkdir()
                if mode == 'fgoff':
                    s.ini('Performance', 'FrameGeneration', 0)
                    time.sleep(3)
                r['init'] = call(s, 'PhaseProbe', 'Init', '#Girl')
                s.close_menus()
                if has_soft_body:
                    call(s, 'VaMSoftBody', 'Report')                  # resets its counters
                r['phase_arm'] = call(s, 'PhaseProbe', 'Arm', str(d), 20., SECONDS)
                r['census_arm'] = call(s, 'CensusProbe', 'Arm', str(d), 2.0, SECONDS)
                print(mode, r['census_arm'], flush=True)
                t0 = r['window_wall'] = time.time() + 2
                time.sleep(2 + SECONDS + .4)
                r['soft_body_report'] = call(s, 'VaMSoftBody', 'Report') if has_soft_body else 'not installed'
                r['phase_finish'] = call(s, 'PhaseProbe', 'Finish')
                r['census_finish'] = call(s, 'CensusProbe', 'Finish')
                r['menus_open'] = s.menus_open()
                phases = pb.metrics(d)
                r['phases'] = phases
                r['gpu'] = pb.gpu_usage(s.log_dir, t0, t0 + SECONDS)
                time.sleep(.6)
                r['window_frames'] = int(round(phases['real_fps'] * SECONDS))
                print(mode, 'fps', phases['real_fps'], 'menus', r['menus_open'], flush=True)
            census.stop()
            for mode in ('fg4', 'fgoff'):
                r = result[mode]
                r['threads'] = census.window(r['window_wall'], r['window_wall'] + SECONDS + .5, r['window_frames'])
                (out / mode / 'threads.json').write_text(json.dumps(r['threads'], indent=1), encoding='utf-8')
            census = None
    finally:
        if census:
            census.stop()
        for name, _ in PROBES:
            for _ in range(60):
                try:
                    if (PLUGINS / name).exists():
                        (PLUGINS / name).unlink()
                    break
                except OSError:
                    time.sleep(1)
        result['probes_removed'] = not any((PLUGINS / n).exists() for n, _ in PROBES)
        if saved:
            result['plugins_restored'] = swap_back(saved)
        (out / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
        if not result['probes_removed']:
            raise RuntimeError('probe still in plugins')
        if saved and not result['plugins_restored']:
            raise RuntimeError('the installed plugins are not back: see ' + str(out / 'replaced'))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out', nargs='?', type=Path, default=HERE / 'results' / datetime.now().strftime('%Y%m%d-%H%M%S'))
    ap.add_argument('--candidate', type=Path)
    a = ap.parse_args()
    main(a.out, a.candidate)
