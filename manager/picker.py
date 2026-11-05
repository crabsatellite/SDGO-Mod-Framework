"""Native file/directory selection requested by a click in the Manager UI.

The Windows common item dialog runs inside the Manager process on its own STA thread. It is owned by an invisible
topmost window, so it opens above the browser that sent the click, and is centred on the screen the pointer is on.
A cancelled dialog returns no path; any failure raises at once so the UI can offer manual path entry.
"""
import ctypes
import sys
import threading
import time
from ctypes import wintypes

LOCK = threading.Lock()
FILTERS = (('Supported files', '*.ini;*.json;*.dll;*.zip;*.png'), ('All files', '*.*'))
CLSID_FILE_OPEN_DIALOG = '{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}'
IID_FILE_OPEN_DIALOG = '{D57C7288-D4AD-4768-BE02-9D969532D960}'
CANCELLED = -2147023673  # HRESULT_FROM_WIN32(ERROR_CANCELLED)
FOS_NOCHANGEDIR, FOS_PICKFOLDERS, FOS_FORCEFILESYSTEM = 0x8, 0x20, 0x40
FOS_PATHMUSTEXIST, FOS_FILEMUSTEXIST = 0x800, 0x1000
SIGDN_FILESYSPATH = -2147123200  # 0x80058000
# Vtable slots: IUnknown 0-2, IModalWindow::Show 3, IFileDialog 4-26, IShellItem::GetDisplayName 5.
SHOW, SET_FILE_TYPES, SET_OPTIONS, GET_OPTIONS, GET_RESULT, RELEASE, DISPLAY_NAME = 3, 4, 9, 10, 20, 2, 5
WS_POPUP, WS_EX_TOPMOST, WS_EX_TOOLWINDOW, WS_EX_LAYERED, LWA_ALPHA, SW_SHOW = 0x80000000, 0x8, 0x80, 0x80000, 0x2, 5
SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE, MONITOR_DEFAULTTONEAREST = 0x1, 0x4, 0x10, 2


class GUID(ctypes.Structure):
    _fields_ = [('Data1', wintypes.DWORD), ('Data2', wintypes.WORD), ('Data3', wintypes.WORD), ('Data4', ctypes.c_ubyte * 8)]


class FilterSpec(ctypes.Structure):
    _fields_ = [('name', wintypes.LPCWSTR), ('spec', wintypes.LPCWSTR)]


class MonitorInfo(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', wintypes.RECT), ('rcWork', wintypes.RECT), ('dwFlags', wintypes.DWORD)]


WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def centred(work, width, height):
    """Top-left corner that centres a width x height window in a (left, top, right, bottom) work area."""
    left, top, right, bottom = work
    return left + max(0, (right - left - width) // 2), top + max(0, (bottom - top - height) // 2)


def options(kind):
    """IFileDialog option bits added to the dialog defaults for a picker kind."""
    common = FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST
    return common | (FOS_PICKFOLDERS if kind == 'directory' else FOS_FILEMUSTEXIST)


def _call(obj, index, *argtypes):
    table = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    return ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, *argtypes)(table[index])


def _check(hr, what):
    if hr < 0: raise OSError(f'{what} failed (0x{hr & 0xFFFFFFFF:08X})')
    return hr


def _bind(ole32, user32):
    user32.CreateWindowExW.restype = wintypes.HWND
    user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                       ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
    user32.SetLayeredWindowAttributes.argtypes = [wintypes.HWND, wintypes.DWORD, wintypes.BYTE, wintypes.DWORD]
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.SetForegroundWindow.argtypes = user32.DestroyWindow.argtypes = [wintypes.HWND]
    user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
    user32.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
    user32.MonitorFromPoint.restype = wintypes.HMONITOR
    user32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.POINTER(MonitorInfo)]
    user32.EnumThreadWindows.argtypes = [wintypes.DWORD, WNDENUMPROC, wintypes.LPARAM]
    user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    ole32.CLSIDFromString.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(GUID)]
    ole32.CLSIDFromString.restype = ole32.CoInitializeEx.restype = ole32.CoCreateInstance.restype = ctypes.c_long
    ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, wintypes.DWORD]
    ole32.CoCreateInstance.argtypes = [ctypes.POINTER(GUID), ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p)]
    ole32.CoTaskMemFree.argtypes = [ctypes.c_void_p]


def _work_area(user32):
    """Work area of the monitor under the pointer (the one showing the browser that was clicked)."""
    point = wintypes.POINT()
    if not user32.GetCursorPos(ctypes.byref(point)): point.x = point.y = 0
    info = MonitorInfo(cbSize=ctypes.sizeof(MonitorInfo))
    if not user32.GetMonitorInfoW(user32.MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), ctypes.byref(info)):
        return point.x, point.y, point.x + 1, point.y + 1
    work = info.rcWork
    return work.left, work.top, work.right, work.bottom


def _owner(user32, work):
    """Invisible topmost window at the screen centre; dialogs it owns stay above the browser."""
    x, y = centred(work, 1, 1)
    owner = user32.CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, 'STATIC', 'SDGO Mod Manager', WS_POPUP,
                                   x, y, 1, 1, None, None, None, None)
    if not owner: raise OSError('CreateWindowExW failed')
    user32.SetLayeredWindowAttributes(owner, 0, 0, LWA_ALPHA)
    user32.ShowWindow(owner, SW_SHOW)
    user32.SetForegroundWindow(owner)
    return owner


def _centre_when_shown(user32, thread, work, done):
    """The dialog picks its own first position; move it to the screen centre as soon as it is visible."""
    found = []

    def visit(hwnd, _):
        name = ctypes.create_unicode_buffer(16)
        user32.GetClassNameW(hwnd, name, 16)
        if name.value == '#32770' and user32.IsWindowVisible(hwnd): found.append(hwnd); return False
        return True

    callback = WNDENUMPROC(visit)
    deadline = time.monotonic() + 15
    while not done.is_set() and time.monotonic() < deadline and not found:
        user32.EnumThreadWindows(thread, callback, 0)
        if not found: done.wait(0.01)
    if found:
        rect = wintypes.RECT()
        user32.GetWindowRect(found[0], ctypes.byref(rect))
        x, y = centred(work, rect.right - rect.left, rect.bottom - rect.top)
        user32.SetWindowPos(found[0], None, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)


def _show(kind):
    ole32, user32 = ctypes.WinDLL('ole32'), ctypes.WinDLL('user32')
    _bind(ole32, user32)
    clsid, iid = GUID(), GUID()
    _check(ole32.CLSIDFromString(CLSID_FILE_OPEN_DIALOG, ctypes.byref(clsid)), 'CLSIDFromString')
    _check(ole32.CLSIDFromString(IID_FILE_OPEN_DIALOG, ctypes.byref(iid)), 'CLSIDFromString')
    initialized = ole32.CoInitializeEx(None, 0x2 | 0x4) >= 0  # apartment threaded, no OLE1 DDE
    dialog, item, owner = ctypes.c_void_p(), ctypes.c_void_p(), None
    try:
        _check(ole32.CoCreateInstance(ctypes.byref(clsid), None, 1, ctypes.byref(iid), ctypes.byref(dialog)), 'CoCreateInstance')
        current = wintypes.DWORD()
        _check(_call(dialog, GET_OPTIONS, ctypes.POINTER(wintypes.DWORD))(dialog, ctypes.byref(current)), 'GetOptions')
        _check(_call(dialog, SET_OPTIONS, wintypes.DWORD)(dialog, current.value | options(kind)), 'SetOptions')
        if kind == 'file':
            specs = (FilterSpec * len(FILTERS))(*FILTERS)
            _check(_call(dialog, SET_FILE_TYPES, wintypes.UINT, ctypes.POINTER(FilterSpec))(dialog, len(FILTERS), specs), 'SetFileTypes')
        work = _work_area(user32)
        owner = _owner(user32, work)
        shown = threading.Event()
        threading.Thread(target=_centre_when_shown, args=(user32, ctypes.windll.kernel32.GetCurrentThreadId(), work, shown),
                         name='manager-path-picker-position', daemon=True).start()
        try: hr = _call(dialog, SHOW, wintypes.HWND)(dialog, owner)
        finally: shown.set()
        if hr == CANCELLED: return {'path': None}
        _check(hr, 'Show')
        _check(_call(dialog, GET_RESULT, ctypes.POINTER(ctypes.c_void_p))(dialog, ctypes.byref(item)), 'GetResult')
        name = ctypes.c_void_p()
        _check(_call(item, DISPLAY_NAME, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p))(item, SIGDN_FILESYSPATH, ctypes.byref(name)),
               'GetDisplayName')
        try: return {'path': ctypes.wstring_at(name.value)}
        finally: ole32.CoTaskMemFree(name)
    finally:
        for obj in (item, dialog):
            if obj.value: _call(obj, RELEASE)(obj)
        if owner: user32.DestroyWindow(owner)
        if initialized: ole32.CoUninitialize()


def choose(kind):
    if kind not in ('directory', 'file'): raise ValueError('选择器类型无效')
    if sys.platform != 'win32': raise ValueError('此系统不支持路径选择窗口，请手动填写路径')
    if not LOCK.acquire(blocking=False): raise ValueError('已有一个路径选择窗口，请先完成或取消')
    try:
        result = {}

        def run():
            try: result['value'] = _show(kind)
            except Exception as error: result['error'] = error

        worker = threading.Thread(target=run, name='manager-path-picker', daemon=True)
        worker.start(); worker.join()
        if 'error' in result: raise ValueError('路径选择窗口未能打开，请手动填写路径') from result['error']
        return result['value']
    finally: LOCK.release()
