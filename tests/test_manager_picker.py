"""Path picker contract: in-process native dialog, fast failure, single dialog at a time.

Set SDGO_PICKER_LIVE=1 on a Windows desktop session to open the real dialogs; the live test closes them itself.
"""
import ctypes
import os
from pathlib import Path
import sys
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from manager import picker  # noqa: E402


class PickerContract(unittest.TestCase):
    def test_dialog_runs_in_process_without_a_helper_process(self):
        source = (ROOT / 'manager/picker.py').read_text(encoding='utf-8')
        self.assertNotIn('subprocess', source)
        self.assertNotIn('powershell', source.lower())

    def test_folder_and_file_options(self):
        self.assertTrue(picker.options('directory') & picker.FOS_PICKFOLDERS)
        self.assertFalse(picker.options('file') & picker.FOS_PICKFOLDERS)
        self.assertTrue(picker.options('file') & picker.FOS_FILEMUSTEXIST)
        for kind in ('directory', 'file'):
            self.assertTrue(picker.options(kind) & picker.FOS_FORCEFILESYSTEM)

    def test_unknown_kind_is_rejected(self):
        with self.assertRaises(ValueError): picker.choose('drive')

    @unittest.skipUnless(sys.platform == 'win32', 'Windows dialog')
    def test_dialog_failure_returns_at_once_for_manual_entry(self):
        with mock.patch.object(picker, '_show', side_effect=OSError('CoCreateInstance failed')):
            started = time.monotonic()
            with self.assertRaisesRegex(ValueError, '手动填写路径'): picker.choose('directory')
            self.assertLess(time.monotonic() - started, 5)
        self.assertFalse(picker.LOCK.locked())

    @unittest.skipUnless(sys.platform == 'win32', 'Windows dialog')
    def test_cancel_and_selection_pass_through(self):
        with mock.patch.object(picker, '_show', return_value={'path': None}):
            self.assertEqual(picker.choose('directory'), {'path': None})
        with mock.patch.object(picker, '_show', return_value={'path': r'C:\SDGO\client'}):
            self.assertEqual(picker.choose('file'), {'path': r'C:\SDGO\client'})

    @unittest.skipUnless(sys.platform == 'win32', 'Windows dialog')
    def test_second_dialog_is_refused_while_one_is_open(self):
        opened, release = threading.Event(), threading.Event()

        def show(kind):
            opened.set(); release.wait(5); return {'path': None}

        with mock.patch.object(picker, '_show', side_effect=show):
            worker = threading.Thread(target=picker.choose, args=('directory',)); worker.start()
            self.assertTrue(opened.wait(5))
            with self.assertRaisesRegex(ValueError, '已有一个路径选择窗口'): picker.choose('directory')
            release.set(); worker.join(5)
        self.assertFalse(picker.LOCK.locked())


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('SDGO_PICKER_LIVE') == '1', 'needs a Windows desktop session')
class LiveDialog(unittest.TestCase):
    def dialog_window(self, deadline=8.0):
        user32 = ctypes.WinDLL('user32')
        found = []
        callback = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        def visit(hwnd, _):
            pid, name = ctypes.c_ulong(), ctypes.create_unicode_buffer(64)
            user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(pid))
            user32.GetClassNameW(ctypes.c_void_p(hwnd), name, 64)
            if pid.value == os.getpid() and name.value == '#32770' and user32.IsWindowVisible(ctypes.c_void_p(hwnd)): found.append(hwnd)
            return True

        end = time.monotonic() + deadline
        while time.monotonic() < end and not found:
            user32.EnumWindows(callback(visit), None); time.sleep(.1)
        return found[0] if found else None

    def check(self, kind):
        user32 = ctypes.WinDLL('user32')
        user32.GetWindowLongW.restype = ctypes.c_long
        result = {}
        worker = threading.Thread(target=lambda: result.setdefault('value', picker.choose(kind)), daemon=True)
        started = time.monotonic(); worker.start()
        hwnd = self.dialog_window()
        self.assertIsNotNone(hwnd, kind + ' dialog did not appear')
        self.assertLess(time.monotonic() - started, 8)
        self.assertTrue(user32.GetWindowLongW(ctypes.c_void_p(hwnd), -20) & 0x8, kind + ' dialog is not above other windows')
        user32.PostMessageW(ctypes.c_void_p(hwnd), 0x0010, 0, 0)  # WM_CLOSE cancels the dialog
        worker.join(10)
        self.assertEqual(result.get('value'), {'path': None})

    def test_folder_dialog_appears_on_top(self):
        self.check('directory')

    def test_file_dialog_appears_on_top(self):
        self.check('file')


if __name__ == '__main__':
    unittest.main()
