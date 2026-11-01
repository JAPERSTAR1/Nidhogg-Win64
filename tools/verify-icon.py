"""Read PE resources as data without executing the game."""
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import struct
import sys

root = Path(__file__).resolve().parents[1]
path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'port/out/Nidhogg.exe'
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD]
kernel.LoadLibraryExW.restype = ctypes.c_void_p
kernel.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
kernel.FindResourceW.restype = ctypes.c_void_p
kernel.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel.LoadResource.restype = ctypes.c_void_p
kernel.LockResource.argtypes = [ctypes.c_void_p]
kernel.LockResource.restype = ctypes.c_void_p
kernel.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel.SizeofResource.restype = wintypes.DWORD
kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
handle = kernel.LoadLibraryExW(str(path.resolve()), None, 0x2 | 0x20)
if not handle: raise ctypes.WinError(ctypes.get_last_error())
try:
    def resource(identifier, kind):
        found = kernel.FindResourceW(handle, identifier, kind)
        if not found: raise ctypes.WinError(ctypes.get_last_error())
        loaded = kernel.LoadResource(handle, found)
        pointer = kernel.LockResource(loaded)
        if not pointer: raise ctypes.WinError(ctypes.get_last_error())
        return ctypes.string_at(pointer, kernel.SizeofResource(handle, found))
    group = resource(101, 14)
    reserved, kind, count = struct.unpack_from('<HHH', group)
    assert reserved == 0 and kind == 1 and count == 7
    sizes = []
    for index in range(count):
        width, height, _, _, _, _, length, identifier = struct.unpack_from('<BBBBHHIH', group, 6 + index * 14)
        assert len(resource(identifier, 3)) == length
        sizes.append([width or 256, height or 256])
    png = resource(102, 10)
    assert png == (root / 'port/resources/nidhogg-logo.png').read_bytes()
    print(json.dumps({'executable': str(path.resolve()), 'icon_sizes': sizes,
                      'embedded_original_image_matches': True,
                      'logo_sha256': hashlib.sha256(png).hexdigest()}, indent=2))
finally:
    kernel.FreeLibrary(handle)
