"""CPU time of every thread of a running VaM, from outside (no ETW, no admin): CPU cycles per thread
(QueryThreadCycleTime), the module its start address lies in, its description and CPU mask, and how busy each logical
CPU is (QueryIdleProcessorCycleTime). Run as a sampler beside a game run, then cut the windows out:

    census = Census(pid); census.start()  ...  census.stop(); census.window(t0, t1, frames)
    python -I -u thread_census.py <pid> <seconds>        # standalone: one window, printed
"""
import ctypes as C
import ctypes.wintypes as W
import json
import sys
import threading
import time

k32 = C.WinDLL('kernel32', use_last_error=True)
ntdll = C.WinDLL('ntdll')

TH32CS_SNAPTHREAD = 4
THREAD_QUERY_INFORMATION, THREAD_QUERY_LIMITED_INFORMATION = 0x40, 0x800
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x400, 0x10


class THREADENTRY32(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD), ('th32OwnerProcessID', W.DWORD),
                ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG), ('dwFlags', W.DWORD)]


class MODULEINFO(C.Structure):
    _fields_ = [('lpBaseOfDll', C.c_void_p), ('SizeOfImage', W.DWORD), ('EntryPoint', C.c_void_p)]


class GROUP_AFFINITY(C.Structure):
    _fields_ = [('Mask', C.c_ulonglong), ('Group', W.WORD), ('Reserved', W.WORD * 3)]


k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.OpenThread.restype = W.HANDLE
k32.OpenProcess.restype = W.HANDLE
k32.GetCurrentThread.restype = W.HANDLE
k32.QueryThreadCycleTime.argtypes = [W.HANDLE, C.POINTER(C.c_ulonglong)]
k32.GetThreadDescription.argtypes = [W.HANDLE, C.POINTER(C.c_wchar_p)]
k32.GetThreadGroupAffinity.argtypes = [W.HANDLE, C.POINTER(GROUP_AFFINITY)]
k32.LocalFree.argtypes = [C.c_void_p]
k32.CloseHandle.argtypes = [W.HANDLE]
k32.K32EnumProcessModulesEx.argtypes = [W.HANDLE, C.POINTER(C.c_void_p), W.DWORD, C.POINTER(W.DWORD), W.DWORD]
k32.K32GetModuleInformation.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(MODULEINFO), W.DWORD]
k32.K32GetModuleBaseNameW.argtypes = [W.HANDLE, C.c_void_p, C.c_wchar_p, W.DWORD]
ntdll.NtQueryInformationThread.argtypes = [W.HANDLE, C.c_int, C.c_void_p, C.c_ulong, C.c_void_p]
k32.QueryIdleProcessorCycleTime.argtypes = [C.POINTER(C.c_ulong), C.POINTER(C.c_ulonglong)]


def tsc_hz():
    """cycles a second of QueryThreadCycleTime: a thread spinning on one core for 0.3 s"""
    h = k32.GetCurrentThread()
    a, b = C.c_ulonglong(), C.c_ulonglong()
    k32.QueryThreadCycleTime(h, C.byref(a)); t0 = time.perf_counter()
    while time.perf_counter() - t0 < 0.3:
        pass
    k32.QueryThreadCycleTime(h, C.byref(b)); t1 = time.perf_counter()
    return (b.value - a.value) / (t1 - t0)


def modules(pid):
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    out = []
    if not h:
        return out
    arr = (C.c_void_p * 4096)()
    need = W.DWORD()
    if k32.K32EnumProcessModulesEx(h, arr, C.sizeof(arr), C.byref(need), 3):
        for i in range(need.value // C.sizeof(C.c_void_p)):
            mi = MODULEINFO()
            name = C.create_unicode_buffer(260)
            if k32.K32GetModuleInformation(h, arr[i], C.byref(mi), C.sizeof(mi)):
                k32.K32GetModuleBaseNameW(h, arr[i], name, 260)
                out.append((mi.lpBaseOfDll or 0, (mi.lpBaseOfDll or 0) + mi.SizeOfImage, name.value))
    k32.CloseHandle(h)
    return sorted(out)


def module_of(mods, addr):
    for lo, hi, name in mods:
        if lo <= addr < hi:
            return name, addr - lo
    return '?', addr


def thread_ids(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(); te.dwSize = C.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, C.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, C.byref(te))
    k32.CloseHandle(snap)
    return out


def idle_cycles():
    n = C.c_ulong(0)
    k32.QueryIdleProcessorCycleTime(C.byref(n), None)
    buf = (C.c_ulonglong * (n.value // 8))()
    k32.QueryIdleProcessorCycleTime(C.byref(n), buf)
    return list(buf)


class Census:
    def __init__(self, pid, period=0.5):
        self.pid, self.period = pid, period
        self.hz = tsc_hz()
        self.info = {}           # tid -> dict(start module, offset, name, mask)
        self.handles = {}
        self.samples = []        # (t, {tid: cycles}, [idle cycles per cpu])
        self.mods = modules(pid)
        self._stop = threading.Event()
        self._th = None

    def _open(self, tid):
        h = k32.OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, False, tid)
        if not h:
            return None
        start = C.c_void_p()
        ntdll.NtQueryInformationThread(h, 9, C.byref(start), C.sizeof(start), None)
        mod, off = module_of(self.mods, start.value or 0)
        if mod == '?':
            self.mods = modules(self.pid)
            mod, off = module_of(self.mods, start.value or 0)
        desc = C.c_wchar_p()
        name = ''
        if k32.GetThreadDescription(h, C.byref(desc)) >= 0 and desc.value:
            name = desc.value
        if desc:
            k32.LocalFree(C.cast(desc, C.c_void_p))
        self.handles[tid] = h
        self.info[tid] = {'module': mod, 'offset': hex(off), 'name': name}
        return h

    def _mask(self, tid):
        ga = GROUP_AFFINITY()
        h = self.handles.get(tid)
        return ga.Mask if h and k32.GetThreadGroupAffinity(h, C.byref(ga)) else 0

    def sample(self):
        cyc = {}
        for tid in thread_ids(self.pid):
            h = self.handles.get(tid) or self._open(tid)
            if not h:
                continue
            c = C.c_ulonglong()
            if k32.QueryThreadCycleTime(h, C.byref(c)):
                cyc[tid] = c.value
        self.samples.append((time.time(), cyc, idle_cycles()))

    def _run(self):
        while not self._stop.is_set():
            t = time.perf_counter()
            try:
                self.sample()
            except Exception as e:      # the process may be exiting
                print('thread census:', e, flush=True)
            self._stop.wait(max(0.0, self.period - (time.perf_counter() - t)))

    def start(self):
        self._th = threading.Thread(target=self._run, daemon=True)
        self._th.start()
        return self

    def stop(self):
        self._stop.set()
        if self._th:
            self._th.join(5)
        masks = {tid: self._mask(tid) for tid in self.handles}
        for h in self.handles.values():
            k32.CloseHandle(h)
        self.handles = {}
        for tid, m in masks.items():
            self.info[tid]['mask'] = hex(m)

    def window(self, t0, t1, frames=None):
        """per thread: share of one core over [t0, t1] and ms per frame; per CPU: busy share (all processes)"""
        inside = [s for s in self.samples if t0 <= s[0] <= t1]
        if len(inside) < 2:
            return None
        (ta, ca, ia), (tb, cb, ib) = inside[0], inside[-1]
        dt = tb - ta
        rows = []
        for tid, c in cb.items():
            if tid not in ca:
                continue
            share = (c - ca[tid]) / (self.hz * dt)
            if share < 0.0005:
                continue
            i = self.info.get(tid, {})
            rows.append({'tid': tid, 'module': i.get('module'), 'offset': i.get('offset'), 'name': i.get('name'),
                         'mask': i.get('mask', ''), 'core_share': round(share, 4),
                         'ms_per_frame': round(share * dt * 1000 / frames, 3) if frames else None})
        rows.sort(key=lambda r: -r['core_share'])
        busy = [round(1 - (b - a) / (self.hz * dt), 3) for a, b in zip(ia, ib)]
        return {'seconds': round(dt, 2), 'tsc_hz': round(self.hz), 'frames': frames, 'threads': rows, 'cpu_busy': busy,
                'process_cores': round(sum(r['core_share'] for r in rows), 3)}


if __name__ == '__main__':
    pid, secs = int(sys.argv[1]), float(sys.argv[2])
    c = Census(pid).start()
    t0 = time.time()
    time.sleep(secs + 0.6)
    c.stop()
    print(json.dumps(c.window(t0, t0 + secs + 0.6), indent=1))
