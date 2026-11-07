"""Separate Manager shutdown from one-file launcher cleanup in native smokes."""
import ctypes
import json
import os
import subprocess


def stop_manager(process, session_path, request_exit):
    handle = None
    if os.name == 'nt':
        pid = json.loads(session_path.read_text(encoding='utf-8'))['pid']
        if pid != process.pid:
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.WaitForSingleObject.restype = ctypes.c_uint32
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x00100000, False, pid)
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
    try:
        request_exit()
        if not handle:
            return process.wait(timeout=30)
        if handle and kernel.WaitForSingleObject(handle, 30000) != 0:
            raise RuntimeError('Manager application did not exit within 30 seconds')
        # The app has exited; the one-file launcher can still be removing its
        # extracted libraries. Its cleanup must not conceal an app shutdown hang.
        for attempt in range(3):
            try:
                return process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                if attempt == 2:
                    raise
                print('Manager application exited; waiting for launcher cleanup', flush=True)
    finally:
        if handle:
            kernel.CloseHandle(handle)
