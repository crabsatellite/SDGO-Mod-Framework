"""Offline checks for the private-desktop GameTest process owner.

The real probe starts only this Python interpreter with CREATE_NO_WINDOW.  Its
temporary Win32 STATIC window lives on the non-input desktop and is never shown
on the user's desktop.  No game, service, server, input, or audio setting is
used.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock
import uuid


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_test_windows  # noqa: E402


WINDOW_PROBE = textwrap.dedent(
    r"""
    import ctypes
    from ctypes import wintypes
    import json
    import os
    import threading
    import time

    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    imm = ctypes.WinDLL("imm32", use_last_error=True)
    WNDPROC = ctypes.WINFUNCTYPE(
        ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT,
        wintypes.WPARAM, wintypes.LPARAM,
    )
    class WNDCLASSW(ctypes.Structure):
        _fields_ = [
            ("style", wintypes.UINT),
            ("lpfnWndProc", WNDPROC),
            ("cbClsExtra", ctypes.c_int),
            ("cbWndExtra", ctypes.c_int),
            ("hInstance", wintypes.HINSTANCE),
            ("hIcon", wintypes.HICON),
            ("hCursor", wintypes.HANDLE),
            ("hbrBackground", wintypes.HBRUSH),
            ("lpszMenuName", wintypes.LPCWSTR),
            ("lpszClassName", wintypes.LPCWSTR),
        ]
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.GetCurrentThreadId.restype = wintypes.DWORD
    kernel.GetPriorityClass.argtypes = [wintypes.HANDLE]
    kernel.GetPriorityClass.restype = wintypes.DWORD
    kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel.GetModuleHandleW.restype = wintypes.HMODULE
    user.OpenDesktopW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD,
    ]
    user.OpenDesktopW.restype = wintypes.HANDLE
    user.SetThreadDesktop.argtypes = [wintypes.HANDLE]
    user.SetThreadDesktop.restype = wintypes.BOOL
    user.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
    user.RegisterClassW.restype = wintypes.ATOM
    user.DefWindowProcW.argtypes = [
        wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
    ]
    user.DefWindowProcW.restype = ctypes.c_ssize_t
    imm.ImmDisableIME.argtypes = [wintypes.DWORD]
    imm.ImmDisableIME.restype = wintypes.BOOL
    user.CreateWindowExW.argtypes = [
        wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p,
    ]
    user.CreateWindowExW.restype = wintypes.HWND
    window = user.CreateWindowExW(
        0, "STATIC", "SDGO GameTest background probe",
        0x00CF0000 | 0x10000000,
        0, 0, 160, 80, None, None, None, None,
    )
    if not window:
        raise ctypes.WinError(ctypes.get_last_error())
    proxy_ready = threading.Event()
    proxy_result = {}

    @WNDPROC
    def window_proc(hwnd, message, wparam, lparam):
        return user.DefWindowProcW(hwnd, message, wparam, lparam)

    def create_default_proxy():
        desktop = user.OpenDesktopW("Default", 0, False, 0x0001 | 0x0002 | 0x0080)
        if not desktop:
            proxy_result["error"] = repr(ctypes.WinError(ctypes.get_last_error()))
            proxy_ready.set()
            return
        if not user.SetThreadDesktop(desktop):
            proxy_result["error"] = repr(ctypes.WinError(ctypes.get_last_error()))
            proxy_ready.set()
            return
        imm.ImmDisableIME(kernel.GetCurrentThreadId())
        instance = kernel.GetModuleHandleW(None)
        window_class = WNDCLASSW()
        window_class.lpfnWndProc = window_proc
        window_class.hInstance = instance
        window_class.lpszClassName = "SDGO.GameTest.GraphicsProxy"
        if not user.RegisterClassW(ctypes.byref(window_class)):
            proxy_result["error"] = repr(ctypes.WinError(ctypes.get_last_error()))
            proxy_ready.set()
            return
        proxy = user.CreateWindowExW(
            0x08000000 | 0x00000080,
            "SDGO.GameTest.GraphicsProxy",
            "SDGO GameTest hidden graphics proxy",
            0,
            0, 0, 1, 1, None, None, instance, None,
        )
        if not proxy:
            proxy_result["error"] = repr(ctypes.WinError(ctypes.get_last_error()))
        else:
            proxy_result["hwnd"] = int(proxy)
        proxy_ready.set()
        time.sleep(30)

    worker = threading.Thread(target=create_default_proxy, daemon=True)
    worker.start()
    if not proxy_ready.wait(5) or "error" in proxy_result:
        raise RuntimeError(proxy_result.get("error", "default proxy timed out"))
    print(json.dumps({
        "pid": os.getpid(),
        "marker": os.environ["SDGO_BACKGROUND_PROBE"],
        "cwd": os.getcwd(),
        "window": int(window),
        "proxy": proxy_result["hwnd"],
        "priority_class": int(kernel.GetPriorityClass(kernel.GetCurrentProcess())),
    }), flush=True)
    time.sleep(30)
    """
)


class FailingApi:
    def __init__(self) -> None:
        self.closed_desktops = []

    def assert_window_station(self):
        return "WinSta0"

    def input_desktop_name(self):
        return "Default"

    def create_desktop(self, _):
        return 7001

    def spawn(self, *_):
        raise OSError(2, "synthetic CreateProcessW failure")

    def close_desktop(self, desktop):
        self.closed_desktops.append(desktop)


class BackgroundProcessPolicyTests(unittest.TestCase):
    @staticmethod
    def _input_desktop_api():
        api = game_test_windows._Win32Api.__new__(
            game_test_windows._Win32Api
        )
        api.user = mock.Mock()
        api.input_desktop_open_retry_count = 0
        api.last_input_desktop_open_retry_count = 0
        api.close_desktop = mock.Mock()
        return api

    def test_input_desktop_access_denied_retries_then_reads_real_name(self):
        api = self._input_desktop_api()
        api.user.OpenInputDesktop.side_effect = [0, 456]
        api._user_object_name = mock.Mock(return_value="Default")

        with (
            mock.patch.object(
                game_test_windows.ctypes,
                "get_last_error",
                return_value=game_test_windows.ERROR_ACCESS_DENIED,
            ),
            mock.patch.object(game_test_windows.time, "sleep") as sleep,
        ):
            self.assertEqual(api.input_desktop_name(), "Default")

        self.assertEqual(api.user.OpenInputDesktop.call_count, 2)
        sleep.assert_called_once_with(
            game_test_windows.INPUT_DESKTOP_RETRY_DELAY_SECONDS
        )
        self.assertEqual(api.input_desktop_open_retry_count, 1)
        self.assertEqual(api.last_input_desktop_open_retry_count, 1)
        api._user_object_name.assert_called_once_with(456)
        api.close_desktop.assert_called_once_with(456)

    def test_input_desktop_access_denied_timeout_keeps_error_and_no_name(self):
        api = self._input_desktop_api()
        api.user.OpenInputDesktop.return_value = 0
        api.desktop_state = mock.Mock()

        with (
            mock.patch.object(
                game_test_windows.ctypes,
                "get_last_error",
                return_value=game_test_windows.ERROR_ACCESS_DENIED,
            ),
            mock.patch.object(
                game_test_windows.ctypes,
                "set_last_error",
            ) as set_last_error,
            mock.patch.object(
                game_test_windows._Win32Api,
                "_error",
                return_value=OSError(
                    game_test_windows.ERROR_ACCESS_DENIED,
                    "synthetic access denied",
                ),
            ),
            mock.patch.object(game_test_windows.time, "sleep") as sleep,
        ):
            with self.assertRaises(OSError) as raised:
                api.input_desktop_state(100)

        self.assertEqual(
            raised.exception.errno,
            game_test_windows.ERROR_ACCESS_DENIED,
        )
        self.assertEqual(
            api.user.OpenInputDesktop.call_count,
            game_test_windows.INPUT_DESKTOP_MAX_RETRIES + 1,
        )
        self.assertEqual(
            sleep.call_count,
            game_test_windows.INPUT_DESKTOP_MAX_RETRIES,
        )
        self.assertEqual(
            api.input_desktop_open_retry_count,
            game_test_windows.INPUT_DESKTOP_MAX_RETRIES,
        )
        self.assertEqual(
            api.last_input_desktop_open_retry_count,
            game_test_windows.INPUT_DESKTOP_MAX_RETRIES,
        )
        set_last_error.assert_called_once_with(
            game_test_windows.ERROR_ACCESS_DENIED
        )
        api.desktop_state.assert_not_called()
        api.close_desktop.assert_not_called()

    def test_input_desktop_other_error_does_not_retry_or_fallback(self):
        api = self._input_desktop_api()
        api.user.OpenInputDesktop.return_value = 0
        api._user_object_name = mock.Mock(return_value="Default")

        with (
            mock.patch.object(
                game_test_windows.ctypes,
                "get_last_error",
                return_value=87,
            ),
            mock.patch.object(
                game_test_windows.ctypes,
                "set_last_error",
            ) as set_last_error,
            mock.patch.object(
                game_test_windows._Win32Api,
                "_error",
                return_value=OSError(87, "synthetic invalid parameter"),
            ),
            mock.patch.object(game_test_windows.time, "sleep") as sleep,
        ):
            with self.assertRaises(OSError) as raised:
                api.input_desktop_name()

        self.assertEqual(raised.exception.errno, 87)
        api.user.OpenInputDesktop.assert_called_once_with(
            0, False, game_test_windows.DESKTOP_READOBJECTS
        )
        sleep.assert_not_called()
        set_last_error.assert_called_once_with(87)
        api._user_object_name.assert_not_called()
        api.close_desktop.assert_not_called()

    def test_input_desktop_proxy_is_allowed_only_with_exact_hidden_window_identity(self):
        class Api:
            def desktop_state(self,desktop,pid):return game_test_windows._DesktopState('owned-private',1,1)
            def input_desktop_state(self,pid):return game_test_windows._DesktopState('Default',1,1,(self.window,))
            def foreground_pid(self):return 99
        api=Api()
        api.input_desktop_open_retry_count=3
        owner=game_test_windows.WindowsBackgroundProcess.__new__(game_test_windows.WindowsBackgroundProcess)
        owner.pid=100;owner.run_id='offline';owner.desktop_name='owned-private';owner.desktop_path='WinSta0\\owned-private'
        owner._desktop_handle=123;owner._api=api;owner._input_desktop_at_launch='Default';owner._closed=True
        owner.allow_named_desktop_fallback=False
        original={'hwnd':456,'class_name':'SDGO.GameTest.GraphicsProxy','visible':False,
                  'noactivate':True,'toolwindow':True,'appwindow':False}
        api.window=original.copy()
        receipt=owner.verify_background(100,allowed_input_windows=(456,))
        self.assertTrue(receipt['verified'])
        self.assertEqual(receipt['input_desktop_open_retries'],3)
        self.assertFalse(owner.verify_background(100)['verified'])
        with mock.patch.object(api,'input_desktop_state',return_value=game_test_windows._DesktopState('Default',0,0)):
            self.assertFalse(owner.verify_background(100,allowed_input_windows=(456,))['verified'])
        for key,value in (('visible',True),('noactivate',False),('toolwindow',False),
                          ('appwindow',True),('class_name','different'),('hwnd',789)):
            api.window={**original,key:value}
            with self.subTest(field=key):self.assertFalse(owner.verify_background(100,allowed_input_windows=(456,))['verified'])

    def test_named_desktop_fallback_is_explicit_and_keeps_input_state_unknown(self):
        class Api:
            def __init__(self):
                self.foreground = 99
                self.proxy = {
                    'hwnd':456, 'pid':100,
                    'class_name':'SDGO.GameTest.GraphicsProxy',
                    'visible':False, 'noactivate':True,
                    'toolwindow':True, 'appwindow':False,
                }
            def desktop_state(self,desktop,pid):
                return game_test_windows._DesktopState('owned-private',1,1)
            def input_desktop_state(self,pid):
                raise PermissionError(5, 'OpenInputDesktop denied')
            def named_desktop_state(self,name,pid):
                return game_test_windows._DesktopState('Default',1,1,(self.proxy,))
            def foreground_pid(self):
                return self.foreground
        api=Api()
        owner=game_test_windows.WindowsBackgroundProcess.__new__(game_test_windows.WindowsBackgroundProcess)
        owner.pid=100;owner.run_id='offline';owner.desktop_name='owned-private';owner.desktop_path='WinSta0\\owned-private'
        owner._desktop_handle=123;owner._api=api;owner._input_desktop_at_launch=None;owner._closed=True
        owner.allow_named_desktop_fallback=True

        receipt=owner.verify_background(100,allowed_input_windows=(456,))
        self.assertTrue(receipt['verified'],receipt)
        self.assertTrue(receipt['isolation_safe'],receipt)
        self.assertEqual(receipt['isolation_mode'],'private_desktop_with_hidden_default_proxy')
        self.assertIsNone(receipt['input_desktop_name'])
        self.assertIsNone(receipt['input_desktop_unchanged'])
        self.assertIsNone(receipt['input_target_window_count'])
        self.assertIsNone(receipt['input_desktop_safe'])
        self.assertEqual(receipt['named_desktop_target_window_count'],1)

        owner.allow_named_desktop_fallback=False
        strict=owner.verify_background(100,allowed_input_windows=(456,))
        self.assertFalse(strict['verified'])
        self.assertIn('OpenInputDesktop denied',strict['error'])

    def test_named_desktop_fallback_rejects_unproven_proxy_and_foreground(self):
        class Api:
            def __init__(self):
                self.foreground = 99
                self.proxy = {
                    'hwnd':456, 'pid':100,
                    'class_name':'SDGO.GameTest.GraphicsProxy',
                    'visible':False, 'noactivate':True,
                    'toolwindow':True, 'appwindow':False,
                }
            def desktop_state(self,desktop,pid):
                return game_test_windows._DesktopState('owned-private',1,1)
            def input_desktop_state(self,pid):
                raise PermissionError(5, 'OpenInputDesktop denied')
            def named_desktop_state(self,name,pid):
                return game_test_windows._DesktopState('Default',1,1,(self.proxy,))
            def foreground_pid(self):
                return self.foreground
        api=Api()
        owner=game_test_windows.WindowsBackgroundProcess.__new__(game_test_windows.WindowsBackgroundProcess)
        owner.pid=100;owner.run_id='offline';owner.desktop_name='owned-private';owner.desktop_path='WinSta0\\owned-private'
        owner._desktop_handle=123;owner._api=api;owner._input_desktop_at_launch=None;owner._closed=True
        owner.allow_named_desktop_fallback=True
        original=api.proxy.copy()
        for key,value in (
            ('visible',True),
            ('class_name','unknown.proxy'),
            ('pid',101),
            ('hwnd',789),
        ):
            api.proxy={**original,key:value}
            with self.subTest(field=key):
                receipt=owner.verify_background(100,allowed_input_windows=(456,))
                self.assertFalse(receipt['verified'],receipt)
                self.assertFalse(receipt['isolation_safe'],receipt)
        api.proxy=original
        api.foreground=100
        receipt=owner.verify_background(100,allowed_input_windows=(456,))
        self.assertFalse(receipt['verified'],receipt)
        self.assertFalse(receipt['foreground_safe'],receipt)

    def test_named_desktop_fallback_rejects_non_access_denied_observation_errors(self):
        class Api:
            def desktop_state(self,desktop,pid):
                return game_test_windows._DesktopState('owned-private',1,1)
            def input_desktop_state(self,pid):
                raise OSError(87,'invalid parameter')
            def named_desktop_state(self,name,pid):
                raise AssertionError('named fallback must not run')
            def foreground_pid(self):
                return 99
        owner=game_test_windows.WindowsBackgroundProcess.__new__(game_test_windows.WindowsBackgroundProcess)
        owner.pid=100;owner.run_id='offline';owner.desktop_name='owned-private';owner.desktop_path='WinSta0\\owned-private'
        owner._desktop_handle=123;owner._api=Api();owner._input_desktop_at_launch=None;owner._closed=True
        owner.allow_named_desktop_fallback=True
        receipt=owner.verify_background(100,allowed_input_windows=(456,))
        self.assertFalse(receipt['verified'])
        self.assertIn('invalid parameter',receipt['error'])

    def test_termination_exit_race_is_idempotent_but_live_access_failure_still_raises(self):
        api=game_test_windows._Win32Api.__new__(game_test_windows._Win32Api)
        api.kernel=mock.Mock();api.poll=mock.Mock(return_value=None)
        api.kernel.TerminateProcess.return_value=False
        api.kernel.WaitForSingleObject.return_value=game_test_windows.WAIT_OBJECT_0
        api._error=mock.Mock(return_value=OSError(5,'termination raced exit'))
        api.terminate(123)
        api.kernel.WaitForSingleObject.assert_called_once()
        api.kernel.WaitForSingleObject.return_value=game_test_windows.WAIT_TIMEOUT
        with self.assertRaises(OSError):api.terminate(123)

    def test_rejects_command_strings_and_invalid_environment(self):
        with self.assertRaises(TypeError):
            game_test_windows._argument_list("python -c pass")
        with self.assertRaises(ValueError):
            game_test_windows._argument_list([])
        with self.assertRaises(ValueError):
            game_test_windows._environment_block({"BAD=NAME": "x"})

    def test_spawn_failure_closes_only_the_new_desktop(self):
        api = FailingApi()
        with mock.patch.object(
            game_test_windows.WindowsBackgroundProcess,
            "_new_api",
            return_value=api,
        ):
            with self.assertRaises(OSError):
                game_test_windows.WindowsBackgroundProcess(
                    [sys.executable, "-c", "pass"],
                    ROOT,
                    os.environ.copy(),
                    subprocess.DEVNULL,
                    subprocess.DEVNULL,
                    str(uuid.uuid4()),
                )
        self.assertEqual(api.closed_desktops, [7001])


@unittest.skipUnless(
    os.name == "nt" and os.environ.get("SDGO_INPUT_DESKTOP_WATCH_SECONDS"),
    "set SDGO_INPUT_DESKTOP_WATCH_SECONDS for the read-only watcher",
)
class InputDesktopReadOnlyWatcherTests(unittest.TestCase):
    def test_watch_input_desktop_access_state_changes(self):
        duration = float(os.environ["SDGO_INPUT_DESKTOP_WATCH_SECONDS"])
        self.assertGreater(duration, 0)
        interval = float(
            os.environ.get("SDGO_INPUT_DESKTOP_WATCH_INTERVAL", "0.1")
        )
        self.assertGreater(interval, 0)

        api = game_test_windows._Win32Api()
        api.kernel.ProcessIdToSessionId.argtypes = [
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
        ]
        api.kernel.ProcessIdToSessionId.restype = wintypes.BOOL
        session = wintypes.DWORD()
        if not api.kernel.ProcessIdToSessionId(
            os.getpid(), ctypes.byref(session)
        ):
            raise api._error("ProcessIdToSessionId failed")

        log_path = os.environ.get("SDGO_INPUT_DESKTOP_WATCH_LOG")
        log = (
            open(log_path, "a", encoding="utf-8")
            if log_path else None
        )

        def probe() -> dict:
            row = {
                "session_id": int(session.value),
                "read_access": game_test_windows.DESKTOP_READOBJECTS,
                "read_error": 0,
                "name": None,
                "name_source": None,
                "name_query_error": 0,
            }
            ctypes.set_last_error(0)
            desktop = api.user.OpenInputDesktop(
                0, False, game_test_windows.DESKTOP_READOBJECTS
            )
            if api._valid_handle(desktop):
                try:
                    row["name"] = api._user_object_name(int(desktop))
                    row["name_source"] = "read_access"
                except OSError as error:
                    row["name_query_error"] = int(error.errno or 0)
                finally:
                    api.close_desktop(int(desktop))
                return row

            row["read_error"] = ctypes.get_last_error()
            if row["read_error"] != game_test_windows.ERROR_ACCESS_DENIED:
                return row

            ctypes.set_last_error(0)
            name_desktop = api.user.OpenInputDesktop(0, False, 0)
            if not api._valid_handle(name_desktop):
                row["name_query_error"] = ctypes.get_last_error()
                return row
            try:
                row["name"] = api._user_object_name(int(name_desktop))
                row["name_source"] = "zero_access_after_read_denied"
            except OSError as error:
                row["name_query_error"] = int(error.errno or 0)
            finally:
                api.close_desktop(int(name_desktop))
            return row

        deadline = time.monotonic() + duration
        previous = None
        try:
            while True:
                row = probe()
                state = json.dumps(row, sort_keys=True)
                if state != previous:
                    row["timestamp_utc"] = datetime.now(
                        timezone.utc
                    ).isoformat()
                    line = json.dumps(row, sort_keys=True)
                    print(line, flush=True)
                    if log is not None:
                        log.write(line + "\n")
                        log.flush()
                    previous = state
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(interval, remaining))
        finally:
            if log is not None:
                log.close()


@unittest.skipUnless(os.name == "nt", "real private desktops require Windows")
class BackgroundProcessWin32Tests(unittest.TestCase):
    def test_real_child_window_stays_on_background_desktop(self):
        with tempfile.TemporaryDirectory(prefix="sdgo-bg-probe-") as temporary:
            environment = os.environ.copy()
            environment["SDGO_BACKGROUND_PROBE"] = "isolated"
            process = game_test_windows.WindowsBackgroundProcess(
                [sys.executable, "-c", WINDOW_PROBE],
                temporary,
                environment,
                subprocess.PIPE,
                subprocess.PIPE,
                str(uuid.uuid4()),
                allow_named_desktop_fallback=True,
            )
            try:
                self.assertIsNotNone(process.stdout)
                ready = json.loads(process.stdout.readline())
                self.assertEqual(ready["pid"], process.pid)
                self.assertEqual(ready["marker"], "isolated")
                self.assertEqual(ready["priority_class"], game_test_windows.NORMAL_PRIORITY_CLASS)
                self.assertEqual(Path(ready["cwd"]), Path(temporary))

                receipt = process.verify_background(
                    process.pid, allowed_input_windows=(ready["proxy"],),
                )
                self.assertTrue(receipt, json.dumps(receipt, indent=2))
                self.assertTrue(receipt["verified"], receipt)
                self.assertTrue(receipt["foreground_safe"], receipt)
                self.assertTrue(receipt["isolation_safe"], receipt)
                self.assertGreaterEqual(
                    receipt["target_background_window_count"], 1
                )
                if receipt["isolation_mode"] == "private_desktop":
                    self.assertTrue(receipt["input_desktop_safe"], receipt)
                    self.assertEqual(receipt["input_target_window_count"], 1)
                else:
                    self.assertEqual(
                        receipt["isolation_mode"],
                        "private_desktop_with_hidden_default_proxy",
                    )
                    self.assertIsNone(receipt["input_desktop_name"])
                    self.assertIsNone(receipt["input_desktop_unchanged"])
                    self.assertIsNone(receipt["input_target_window_count"])
                    self.assertEqual(
                        receipt["named_desktop_target_window_count"], 1,
                    )
                json.dumps(receipt)

                with self.assertRaises(RuntimeError):
                    process.close()
                self.assertIsNone(process.poll())
                process.kill()
                self.assertEqual(process.wait(timeout=5), 1)
                process.close()
                process.close()
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                process.close()

    def test_wait_timeout_and_separate_stdout_stderr_handles(self):
        code = (
            "import sys,time;"
            "print('out',flush=True);"
            "print('err',file=sys.stderr,flush=True);"
            "time.sleep(30)"
        )
        process = game_test_windows.WindowsBackgroundProcess(
            [sys.executable, "-c", code],
            ROOT,
            os.environ.copy(),
            subprocess.PIPE,
            subprocess.PIPE,
            str(uuid.uuid4()),
        )
        try:
            stdout_line = process.stdout.readline()
            stderr_line = process.stderr.readline()
            with self.assertRaises(subprocess.TimeoutExpired):
                process.wait(timeout=0.01)
            process.kill()
            self.assertEqual(process.wait(timeout=5), 1)
            self.assertEqual(stdout_line, b"out\r\n")
            self.assertEqual(stderr_line, b"err\r\n")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.close()


if __name__ == "__main__":
    unittest.main()
