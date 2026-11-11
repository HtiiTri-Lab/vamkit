"""Resolves module+offset crash addresses to function and source line with dbghelp.

    python symbolize.py <module.dll> <offset> [<offset> ...]
    python symbolize.py --text "CRASH code ... at C:\\path\\x.dll+0x1b2fc ..."

The .pdb must sit next to the module (Build-SimXR.ps1 and the other build scripts copy it
there). Offsets are RVAs as printed by simxr_selftest's crash filter and by Windows event logs.
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

SYMOPT_UNDNAME = 0x2
SYMOPT_LOAD_LINES = 0x10
MAX_NAME = 1024


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_wchar * MAX_NAME)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_uint64)]


def symbolize(module, offsets):
    dbghelp = ctypes.WinDLL("dbghelp.dll", use_last_error=True)
    dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
    dbghelp.SymCleanup.argtypes = [wt.HANDLE]
    dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
    dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64, wt.DWORD,
                                         ctypes.c_void_p, wt.DWORD]
    dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                     ctypes.POINTER(SYMBOL_INFOW)]
    dbghelp.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD),
                                              ctypes.POINTER(IMAGEHLP_LINEW64)]
    process = wt.HANDLE(0x5157)  # any unique value works when no live process is involved
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES)
    if not dbghelp.SymInitializeW(process, os.path.dirname(os.path.abspath(module)), False):
        raise OSError("SymInitialize failed")
    base = 0x10000000
    try:
        if not dbghelp.SymLoadModuleExW(process, None, os.path.abspath(module), None, base, 0, None, 0):
            raise OSError("cannot load symbols for %s" % module)
        for offset in offsets:
            address = base + offset
            info = SYMBOL_INFOW()
            info.SizeOfStruct = 88  # sizeof(SYMBOL_INFOW) in C, with Name[1]
            info.MaxNameLen = MAX_NAME
            displacement = ctypes.c_uint64(0)
            name = "?"
            if dbghelp.SymFromAddrW(process, address, ctypes.byref(displacement), ctypes.byref(info)):
                name = "%s+0x%x" % (info.Name, displacement.value)
            line = IMAGEHLP_LINEW64()
            line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINEW64)
            line_displacement = wt.DWORD(0)
            where = ""
            if dbghelp.SymGetLineFromAddrW64(process, address, ctypes.byref(line_displacement), ctypes.byref(line)):
                where = "  %s:%d" % (line.FileName, line.LineNumber)
            print("%s+0x%x  %s%s" % (os.path.basename(module), offset, name, where))
    finally:
        dbghelp.SymCleanup(process)


def main(argv):
    if len(argv) >= 2 and argv[0] == "--text":
        for module, offset in re.findall(r"([A-Za-z]:\\[^\s]+?\.(?:dll|exe))\+0x([0-9a-fA-F]+)", " ".join(argv[1:])):
            symbolize(module, [int(offset, 16)])
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    symbolize(argv[0], [int(value, 16) for value in argv[1:]])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
