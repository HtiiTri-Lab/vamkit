"""One table per window from a cpu_census.py run: the main thread by Unity subsystem and by plugin, every other
thread grouped by what started it, and how busy each CCD is.
    python -I -u census_report.py <run dir>      -> <run dir>\\report.json, printed summary
"""
import json, sys
from collections import defaultdict
from pathlib import Path


def group(t):
    """a thread's task: its description, else the module its start address is in"""
    name, mod = (t.get('name') or '').strip(), t.get('module') or '?'
    if name:
        return name
    return mod


def window(run, mode):
    r = run[mode]
    d = Path(run['_dir']) / mode
    census = json.loads((d / 'census.json').read_text(encoding='utf-8'))
    threads = r['threads']
    ph = r['phases']
    frames = r['window_frames']
    out = {'fps': ph['real_fps'], 'frame_ms_median': ph['frame_ms_median'], 'gpu': r.get('gpu'),
           'main_thread_phases_ms': ph['per_frame_ms'], 'top_slots_ms': ph['top_slots_ms_per_frame']}
    plugins = []
    for p in census['plugins']:
        if p['main_calls_per_frame'] == 0 and p['other_calls_per_frame'] == 0:
            continue
        plugins.append({'plugin': p['plugin'], 'main_ms': p['main_ms_per_frame'],
                        'main_ms_less_wrapper': round(max(0.0, p['main_ms_per_frame'] - p['main_wrapper_ms_per_frame']), 4),
                        'main_calls': p['main_calls_per_frame'], 'other_threads_ms': p['other_threads_ms_per_frame'],
                        'other_calls': p['other_calls_per_frame'], 'other_threads': p['other_threads'],
                        'top_entries': [{k: e[k] for k in ('method', 'kind', 'main_ms', 'main_calls', 'other_ms', 'other_calls')}
                                        for e in p['entries'][:4]]})
    plugins.sort(key=lambda p: -(p['main_ms'] + p['other_threads_ms']))
    out['plugins'] = plugins
    out['wrapper_us_per_call'] = census['wrapper_us_per_call']
    g = defaultdict(lambda: {'threads': 0, 'cores': 0.0, 'ms_per_frame': 0.0, 'masks': set(), 'modules': set()})
    for t in threads['threads']:
        k = g[group(t)]
        k['threads'] += 1; k['cores'] += t['core_share']; k['ms_per_frame'] += t['ms_per_frame'] or 0
        k['masks'].add(t.get('mask', '')); k['modules'].add(t.get('module') or '?')
    out['threads'] = sorted(({'task': n, 'threads': v['threads'], 'cores': round(v['cores'], 3),
                              'ms_per_frame': round(v['ms_per_frame'], 2), 'start_modules': sorted(v['modules']),
                              'masks': sorted(v['masks'])} for n, v in g.items()), key=lambda x: -x['cores'])
    out['process_cores'] = threads['process_cores']
    busy = threads['cpu_busy']
    out['ccd_busy_cores'] = {'ccd0': round(sum(max(0, b) for b in busy[:16]), 2), 'ccd1': round(sum(max(0, b) for b in busy[16:32]), 2)}
    out['cpu_busy'] = [round(max(0, b), 2) for b in busy]
    out['soft_body_report'] = r.get('soft_body_report')
    return out


def main(d):
    d = Path(d)
    run = json.loads((d / 'result.json').read_text(encoding='utf-8'))
    run['_dir'] = str(d)
    rep = {m: window(run, m) for m in ('fg4', 'fgoff') if m in run and 'threads' in run[m]}
    (d / 'report.json').write_text(json.dumps(rep, indent=1, ensure_ascii=False), encoding='utf-8')
    for m, w in rep.items():
        print('==', m, 'fps', w['fps'], 'frame', w['frame_ms_median'], 'ms; process', w['process_cores'], 'cores; CCD busy', w['ccd_busy_cores'])
        print('  main thread:', json.dumps(w['main_thread_phases_ms']))
        for p in w['plugins']:
            print('  %-26s main %.3f ms (%.1f calls)  other threads %.3f ms (%.1f calls)' % (
                p['plugin'], p['main_ms_less_wrapper'], p['main_calls'], p['other_threads_ms'], p['other_calls']))
        for t in w['threads'][:30]:
            print('  thread %-40s x%-3d %.3f cores  %.2f ms/frame  %s %s' % (t['task'][:40], t['threads'], t['cores'], t['ms_per_frame'],
                                                                          ','.join(t['start_modules'])[:50], ','.join(t['masks'])[:40]))


if __name__ == '__main__':
    main(sys.argv[1])
