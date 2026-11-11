"""Offline: runs the native library's mono.dll layout checks against VaM's mono.dll mapped as an image (no code of it
runs, nothing is hooked).  python -I check_layout.py"""
import ctypes
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
VAM = Path(__import__('os').environ.get('VAMKIT_VAM') or sys.exit('set VAMKIT_VAM to the VaM folder'))
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.LoadLibraryExW.restype = ctypes.c_void_p
mono = k.LoadLibraryExW(str(VAM / 'Mono/EmbedRuntime/mono.dll'), None, 0x1)  # DONT_RESOLVE_DLL_REFERENCES
lib = ctypes.CDLL(str(REPO / 'out/gc-tuning/VaMGcTuningNative.bin'))  # native/build.cmd writes it
buf = ctypes.create_string_buffer(512)
ok = lib.GT_CheckImage(ctypes.c_void_p(mono), buf, 512)
print(ok, buf.value.decode())
