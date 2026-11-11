"""Native call stacks of every thread of a live process (a hung VaM, for example), with dbghelp.

    python stacks.py <pid> [--frames 40] [--all] [--symbols DIR;DIR...]

Each thread is suspended for the moment its stack is read and resumed at once. Symbols: our
modules' .pdb files from the path written into each module (the build folders) or from --symbols;
Windows modules give exported names only. Mono's JIT-compiled C# frames have no unwind data, so a
stack usually ends where managed code begins: the native frames above it are what a hang shows.
Threads whose whole stack is the idle wait of a thread pool are left out unless --all.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_GET_CONTEXT = 0x0008
THREAD_SUSPEND_RESUME = 0x0002
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
TH32CS_SNAPTHREAD = 0x4
CONTEXT_AMD64_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664
SYMOPT_UNDNAME, SYMOPT_DEFERRED_LOADS, SYMOPT_LOAD_LINES, SYMOPT_FAIL_CRITICAL_ERRORS = 0x2, 0x4, 0x10, 0x200
MAX_NAME = 512

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD), ("EntryPoint", ctypes.c_void_p)]


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_wchar * MAX_NAME)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_uint64)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.Thread32First.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.Thread32Next.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.LocalFree.argtypes = [ctypes.c_void_p]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODULEINFO), wt.DWORD]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbghelp.SymCleanup.argtypes = [wt.HANDLE]
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
dbghelp.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINEW64)]
dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbghelp.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

IDLE_TOPS = ("NtWaitForWorkViaWorkerFactory", "ZwWaitForWorkViaWorkerFactory")


def modules(process):
    needed = wt.DWORD(0)
    psapi.EnumProcessModulesEx(process, None, 0, ctypes.byref(needed), 3)
    count = needed.value // ctypes.sizeof(ctypes.c_void_p)
    handles = (ctypes.c_void_p * count)()
    psapi.EnumProcessModulesEx(process, handles, needed, ctypes.byref(needed), 3)
    out = []
    for handle in handles:
        name = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(process, handle, name, 1024)
        info = MODULEINFO()
        psapi.GetModuleInformation(process, handle, ctypes.byref(info), ctypes.sizeof(info))
        out.append((info.lpBaseOfDll or 0, info.SizeOfImage, name.value))
    return sorted(out)


def describe(process, mods, address):
    module = next(((b, n) for b, s, n in mods if b <= address < b + s), None)
    where = "%s+0x%x" % (os.path.basename(module[1]), address - module[0]) if module else "0x%x" % address
    info = SYMBOL_INFOW()
    info.SizeOfStruct = 88
    info.MaxNameLen = MAX_NAME
    displacement = ctypes.c_uint64(0)
    text = where
    if module and dbghelp.SymFromAddrW(process, address, ctypes.byref(displacement), ctypes.byref(info)):
        text = "%s!%s+0x%x  (%s)" % (os.path.splitext(os.path.basename(module[1]))[0], info.Name, displacement.value, where)
        line = IMAGEHLP_LINEW64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINEW64)
        line_displacement = wt.DWORD(0)
        if dbghelp.SymGetLineFromAddrW64(process, address, ctypes.byref(line_displacement), ctypes.byref(line)):
            text += "  %s:%d" % (os.path.basename(line.FileName), line.LineNumber)
    return text, info.Name if module else ""


def thread_ids(pid):
    snapshot = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    ids = []
    ok = k32.Thread32First(snapshot, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            ids.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snapshot, ctypes.byref(entry))
    k32.CloseHandle(snapshot)
    return ids


def walk(process, thread, max_frames):
    raw = ctypes.create_string_buffer(1232 + 16)
    context = (ctypes.addressof(raw) + 15) & ~15  # CONTEXT must be 16-byte aligned
    ctypes.memset(context, 0, 1232)
    ctypes.c_uint32.from_address(context + 0x30).value = CONTEXT_AMD64_FULL
    if k32.SuspendThread(thread) == 0xFFFFFFFF:
        return None
    try:
        if not k32.GetThreadContext(thread, context):
            return None
        rsp = ctypes.c_uint64.from_address(context + 0x98).value
        rbp = ctypes.c_uint64.from_address(context + 0xA0).value
        rip = ctypes.c_uint64.from_address(context + 0xF8).value
        frame = ctypes.create_string_buffer(1024)  # STACKFRAME64 (+ room for newer KDHELP64 versions)
        for offset, value in ((0, rip), (32, rbp), (48, rsp)):  # AddrPC, AddrFrame, AddrStack
            ctypes.c_uint64.from_buffer(frame, offset).value = value
            ctypes.c_uint32.from_buffer(frame, offset + 12).value = 3  # AddrModeFlat
        pcs = []
        table = ctypes.cast(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
        base = ctypes.cast(dbghelp.SymGetModuleBase64, ctypes.c_void_p)
        while len(pcs) < max_frames:
            if not dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, frame, context, None, table, base, None):
                break
            pc = ctypes.c_uint64.from_buffer(frame, 0).value
            if pc == 0 or (pcs and pcs[-1] == pc and len(pcs) > 2 and pcs[-2] == pc):
                break
            pcs.append(pc)
        return pcs
    finally:
        k32.ResumeThread(thread)


def thread_name(thread):
    text = ctypes.c_wchar_p()
    if k32.GetThreadDescription(thread, ctypes.byref(text)) >= 0 and text.value:
        name = text.value
        k32.LocalFree(ctypes.cast(text, ctypes.c_void_p))
        return name
    return ""


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pid", type=int)
    parser.add_argument("--frames", type=int, default=40)
    parser.add_argument("--all", action="store_true", help="also threads idle in a thread pool")
    parser.add_argument("--symbols", default="", help="extra .pdb folders, ';'-separated")
    args = parser.parse_args(argv)

    process = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, args.pid)
    if not process:
        print("cannot open process %d (error %d)" % (args.pid, ctypes.get_last_error()))
        return 1
    mods = modules(process)
    search = ";".join([p for p in args.symbols.split(";") if p] + sorted({os.path.dirname(n) for _, _, n in mods}))
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS)
    if not dbghelp.SymInitializeW(process, search, True):
        print("SymInitialize failed (%d)" % ctypes.get_last_error())
        return 1
    skipped = 0
    try:
        for tid in thread_ids(args.pid):
            thread = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION, False, tid)
            if not thread:
                continue
            try:
                name = thread_name(thread)
                pcs = walk(process, thread, args.frames)
            finally:
                k32.CloseHandle(thread)
            if not pcs:
                print("thread %d %s: no stack" % (tid, name))
                continue
            lines = [describe(process, mods, pc) for pc in pcs]
            if not args.all and lines[0][1] in IDLE_TOPS:
                skipped += 1
                continue
            print("thread %d %s" % (tid, name))
            for text, _ in lines:
                print("    " + text)
    finally:
        dbghelp.SymCleanup(process)
        k32.CloseHandle(process)
    if skipped:
        print("(%d thread-pool threads idle, left out; --all shows them)" % skipped)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
