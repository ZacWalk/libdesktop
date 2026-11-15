# This file is part of libdesktop, a C++20 platform layer for desktop applications
# Copyright 2026 Zac Walker
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Check multiresolution ICO directories and decode every frame through Windows, without an app."""

import ctypes
import struct
import sys
from ctypes import wintypes
from pathlib import Path

sys.dont_write_bytecode = True
from iconlib import ICON_SIZES


class IconInfo(ctypes.Structure):
    _fields_ = [("icon", wintypes.BOOL), ("x", wintypes.DWORD), ("y", wintypes.DWORD),
                ("mask", wintypes.HBITMAP), ("colour", wintypes.HBITMAP)]


class Bitmap(ctypes.Structure):
    _fields_ = [("kind", wintypes.LONG), ("width", wintypes.LONG), ("height", wintypes.LONG),
                ("stride", wintypes.LONG), ("planes", wintypes.WORD), ("bits", wintypes.WORD),
                ("pixels", ctypes.c_void_p)]


def main() -> None:
    if sys.platform != "win32" or len(sys.argv) < 2:
        raise SystemExit("usage (Windows): python tools\\check_ico.py file.ico [file.ico ...]")
    user, gdi = ctypes.WinDLL("user32", use_last_error=True), ctypes.WinDLL("gdi32", use_last_error=True)
    user.LoadImageW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR, wintypes.UINT,
                               ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user.LoadImageW.restype = wintypes.HANDLE
    user.GetIconInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(IconInfo)]
    user.GetIconInfo.restype = wintypes.BOOL
    user.DestroyIcon.argtypes = [wintypes.HANDLE]
    gdi.GetObjectW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.POINTER(Bitmap)]
    gdi.GetObjectW.restype = ctypes.c_int
    gdi.DeleteObject.argtypes = [wintypes.HANDLE]
    frames = 0
    for argument in sys.argv[1:]:
        path = Path(argument).resolve()
        data = path.read_bytes()
        if struct.unpack_from("<3H", data) != (0, 1, len(ICON_SIZES)):
            raise AssertionError(f"{path}: invalid ICO directory")
        for i, size in enumerate(ICON_SIZES):
            entry = struct.unpack_from("<4B2H2I", data, 6 + i * 16)
            if entry[:2] != (size % 256, size % 256) or entry[4:6] != (1, 32):
                raise AssertionError(f"{path}: missing {size}px / 32-bit frame")
            length, offset = entry[6:]
            if length == 0 or offset + length > len(data):
                raise AssertionError(f"{path}: truncated frame")
            handle = user.LoadImageW(None, str(path), 1, size, size, 0x10)
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
            info = IconInfo()
            try:
                if not user.GetIconInfo(handle, ctypes.byref(info)):
                    raise ctypes.WinError(ctypes.get_last_error())
                bitmap = Bitmap()
                if not info.colour or not gdi.GetObjectW(info.colour, ctypes.sizeof(bitmap), ctypes.byref(bitmap)):
                    raise ctypes.WinError(ctypes.get_last_error())
                if (bitmap.width, bitmap.height, bitmap.bits) != (size, size, 32):
                    raise AssertionError(f"{path}: Windows decoded the wrong dimensions or depth")
            finally:
                if info.colour:
                    gdi.DeleteObject(info.colour)
                if info.mask:
                    gdi.DeleteObject(info.mask)
                user.DestroyIcon(handle)
            frames += 1
        print(f"PASS: {path.name}: {len(ICON_SIZES)} native frames")
    print(f"{len(sys.argv) - 1} ICO files, {frames} frames decoded; no windows opened")


if __name__ == "__main__":
    main()
