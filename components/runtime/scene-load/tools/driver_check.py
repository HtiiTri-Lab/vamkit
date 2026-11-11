"""Offline check of VaM Scene Load's bundle job driver (no VaM): CPU Boost's native library loaded and set to its
in-game 8 workers, then VaMSceneLoadNative.bin; every bundle of a scene (the list scene_bundles.py --json writes) submitted one by one (1 ms apart, the way the
main thread queues them), polled until all are done. Prints the wall time, the sum of the jobs' times (their
ratio = how many ran at once) and CPU Boost's stats for the task. An early driver ran the jobs one after another
(68 s of jobs in about 60 s in game).
    VAMKIT_VAM=<VaM folder> python -I driver_check.py <VaMSceneLoadNative.bin> <outdir (deleted after)> <bundles.json>"""
import ctypes, json, os, shutil, sys, time, zipfile
from pathlib import Path

VAM = Path(os.environ['VAMKIT_VAM'])
cb = ctypes.CDLL(str(VAM / 'BepInEx/plugins/VaMCpuBoostNative.bin'))
cb.CB_SetThreads(8)
dll = ctypes.CDLL(sys.argv[1])
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
submit = dll.SL_BundleSubmit
submit.restype = ctypes.c_int
submit.argtypes = [ctypes.c_wchar_p, ctypes.c_int64, ctypes.c_int64, ctypes.c_int, ctypes.c_int64, ctypes.c_wchar_p]
result = dll.SL_BundleResult
result.restype = ctypes.c_int
result.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_int64), ctypes.POINTER(ctypes.c_double)]

bundles = json.load(open(sys.argv[3], encoding='utf-8'))['bundles']
jobs = []
for i, v in enumerate(bundles.values()):
    zz = zipfile.ZipFile(VAM / v['var'])
    names = {n.lower(): n for n in zz.namelist()}
    zi = zz.getinfo(names[v['url'].split(':/', 1)[1].lower()])
    jobs.append((str((VAM / v['var']).resolve()), zi.header_offset, zi.compress_size, zi.compress_type, zi.file_size, str(out / ('%03d.bundle' % i))))
try:
    t0 = time.perf_counter()
    ids = []
    for j in jobs:
        ids.append(submit(*j))
        time.sleep(0.001)
    t_sub = time.perf_counter() - t0
    done, sum_ms, codes = {}, 0.0, {}
    while len(done) < len(ids):
        for k in ids:
            if k in done:
                continue
            b, ms = ctypes.c_int64(0), ctypes.c_double(0)
            r = result(k, ctypes.byref(b), ctypes.byref(ms))
            if r != 0:
                done[k] = time.perf_counter() - t0
                sum_ms += ms.value
                codes[r] = codes.get(r, 0) + 1
        time.sleep(0.01)
    wall = max(done.values())
    name = ctypes.create_string_buffer(64); st = (ctypes.c_double * 8)()
    stats = []
    for i in range(cb.CB_TaskCount()):
        cb.CB_TaskStats(i, name, 64, st, 0)
        stats.append((name.value.decode(), [round(x, 1) for x in st[:3]]))
    print('jobs %d, codes %s, submitted in %.2f s, all done at %.1f s, jobs %.1f s in total -> %.1f at once; CPU Boost %s'
          % (len(ids), codes, t_sub, wall, sum_ms / 1000, sum_ms / 1000 / wall, stats))
finally:
    shutil.rmtree(out, ignore_errors=True)
