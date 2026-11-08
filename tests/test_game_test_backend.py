"""Offline safety checks for the isolated native GameTest backend owner."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "game_test_backend_module", ROOT / "tools/game_test_backend.py"
)
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)

sys.path.insert(0, str(ROOT / "tools"))
import game_test_windows  # noqa: E402


def valid_backup_rows(root: Path) -> list[dict]:
    rows = []
    backups = root / "backups"
    backups.mkdir()
    for name in backend.DATABASES:
        path = backups / f"{name}.bak"
        path.write_bytes(("fixture-" + name).encode())
        rows.append(
            {
                "name": name,
                "file": path.name,
                "bytes": path.stat().st_size,
                "sha256": backend.game_test.sha256(path),
                "files": [
                    {"name": name + "_data", "type": "D"},
                    {"name": name + "_log", "type": "L"},
                ],
            }
        )
    return rows


class NativeServiceOrderTest(unittest.TestCase):
    def test_dependencies_precede_battle_without_changing_selection(self):
        names=['WorldServer','BattleServer6','AgentServer3','StoreServer','AgentServer1','ChattingServer']
        selected=[{'folder':name,'sentinel':index} for index,name in enumerate(names)]
        ordered=backend.ordered_native_services(selected)
        self.assertEqual([s['folder'] for s in ordered],
                         ['StoreServer','ChattingServer','AgentServer3','AgentServer1','BattleServer6','WorldServer'])
        self.assertEqual([s['folder'] for s in selected],names)
        self.assertEqual({id(s) for s in ordered},{id(s) for s in selected})

    def test_inactive_channels_are_never_added(self):
        selected=[{'folder':'BattleServer6'},{'folder':'AgentServer3'},{'folder':'WorldServer'}]
        self.assertEqual([s['folder'] for s in backend.ordered_native_services(selected)],
                         ['AgentServer3','BattleServer6','WorldServer'])


class FakeProcess:
    def __init__(self, *, exit_after_kill=True):
        self.pid = 41001
        self.returncode = None
        self.exit_after_kill = exit_after_kill
        self.killed = 0
        self.waited = 0
        self.closed = 0

    def poll(self):
        return self.returncode

    def kill(self):
        self.killed += 1
        if self.exit_after_kill:
            self.returncode = -9

    def wait(self, timeout=None):
        self.waited += 1
        if self.exit_after_kill:
            self.returncode = -9
        return self.returncode

    def close(self):
        if self.returncode is None:
            raise RuntimeError("still running")
        self.closed += 1


class BackendSafetyTests(unittest.TestCase):
    @unittest.skipUnless(os.name=='nt','Windows file sharing semantics')
    def test_status_write_survives_a_transient_native_reader_delete_lock(self):
        import ctypes
        from ctypes import wintypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,
                                    ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
        kernel.CreateFileW.restype=wintypes.HANDLE
        kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        kernel.CloseHandle.restype=wintypes.BOOL
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'status.json'
            path.write_text('{"phase":"old"}')
            # FILE_SHARE_READ|FILE_SHARE_WRITE deliberately omits DELETE.
            handle=kernel.CreateFileW(str(path),0x80000000,3,None,3,0,None)
            self.assertNotEqual(handle,ctypes.c_void_p(-1).value)
            timer=threading.Timer(1.25,lambda:kernel.CloseHandle(handle))
            timer.start()
            try:
                backend.write(path,{'phase':'ready'})
            finally:
                timer.join()
            self.assertEqual(json.loads(path.read_bytes()),{'phase':'ready'})
            self.assertEqual(list(path.parent.glob('.*.tmp')),[])

    def test_status_write_keeps_persistent_permission_failure_bounded(self):
        denied=PermissionError('persistent status denial')
        with mock.patch.object(backend,'atomic_bytes',side_effect=denied) as writer, \
                mock.patch.object(backend.time,'monotonic',side_effect=[10,11,14,15]), \
                mock.patch.object(backend.time,'sleep') as sleep:
            with self.assertRaises(PermissionError) as caught:
                backend.write(Path('status.json'),{'phase':'ready'})
        self.assertIs(caught.exception,denied)
        self.assertEqual(writer.call_count,3)
        self.assertEqual(sleep.call_count,2)

    @unittest.skipUnless(os.name=='nt','Windows process handles')
    def test_live_pid_probe_never_sends_a_console_signal_and_observes_real_child_exit(self):
        child=subprocess.Popen([sys.executable,'-c','import threading; threading.Event().wait(60)'],
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            with mock.patch.object(backend.os,'kill',side_effect=AssertionError('No signal is a read-only probe')):
                self.assertTrue(backend._pid_exists(child.pid))
                self.assertIsNone(child.poll())
                child.terminate();child.wait(timeout=backend.game_test.OWNED_PROCESS_EXIT_TIMEOUT)
                self.assertFalse(backend._pid_exists(child.pid))
        finally:
            if child.poll() is None:child.terminate()
            child.wait(timeout=backend.game_test.OWNED_PROCESS_EXIT_TIMEOUT)

    @unittest.skipUnless(os.name=='nt','Windows process handles')
    def test_inaccessible_pid_is_preserved_and_absent_pid_is_distinguished(self):
        kernel=mock.Mock();kernel.OpenProcess.return_value=None
        with mock.patch.object(backend.ctypes,'WinDLL',return_value=kernel):
            for error,exists in ((5,True),(87,False),(1168,False),(12345,True)):
                with self.subTest(error=error),mock.patch.object(backend.ctypes,'get_last_error',return_value=error):
                    self.assertEqual(backend._pid_exists(12345),exists)
        kernel.OpenProcess.assert_called_with(0x00100000,False,12345)

    def test_observer_uses_explicit_native_account_identity_only_for_authenticated_suites(self):
        self.assertEqual(backend.observer_host_id(True,None),0)
        with self.assertRaises(RuntimeError):backend.observer_host_id(True,12345)
        self.assertEqual(backend.observer_host_id(False,12345),12345)
        for invalid in (None,0,-1,True,0x100000000):
            with self.subTest(value=invalid),self.assertRaises(RuntimeError):
                backend.observer_host_id(False,invalid)

    def test_incomplete_server_stage_fails_before_any_docker_or_work_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            stage=root/'stage'
            source=root/'source'
            work=root/'work'
            for path in (stage/'server/sdgo/core.dll',source/'WorldServer/WorldServer.ini'):
                path.parent.mkdir(parents=True,exist_ok=True)
                path.write_bytes(b'fixture')
            with mock.patch.object(backend,'docker') as docker:
                with self.assertRaisesRegex(RuntimeError,'server/winmm.dll'):
                    backend.prepare(work,stage,source,'127.93.0.1',root/'backups')
            docker.assert_not_called()
            self.assertFalse(work.exists())

    def test_target_must_be_ipv4_loopback(self):
        self.assertEqual(backend.loopback_ipv4("127.92.0.1"), "127.92.0.1")
        for value in ("::1", "192.0.2.1", "0.0.0.0"):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                backend.loopback_ipv4(value)

    def test_work_directory_must_not_overlap_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.assertTrue(backend.paths_overlap(root / "source" / "child", root / "source"))
            self.assertTrue(backend.paths_overlap(root / "source", root / "source" / "child"))
            self.assertFalse(backend.paths_overlap(root / "source", root / "work"))

    def test_listener_preflight_rejects_exact_or_wildcard_collision(self):
        rows = [
            ("127.92.0.1", 5001, 101),
            ("0.0.0.0", 9302, 102),
            ("127.90.0.1", 9001, 103),
        ]
        with mock.patch.object(backend, "tcp_listener_rows", return_value=rows):
            with self.assertRaisesRegex(RuntimeError, "5001,9302"):
                backend.assert_listener_endpoints_free(
                    "127.92.0.1", [5001, 9001, 9302]
                )
            backend.assert_listener_endpoints_free("127.92.0.1", [9001])

    def test_backup_source_requires_exact_safe_verified_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source"
            destination = Path(temporary) / "destination"
            source.mkdir()
            destination.mkdir()
            rows = valid_backup_rows(source)
            (source / "backend.json").write_text(
                json.dumps({"backups": rows}), encoding="utf-8"
            )
            with mock.patch.object(backend, "emit"):
                copied = backend.load_backup_source(source, destination)
            self.assertEqual({row["name"] for row in copied}, set(backend.DATABASES))
            self.assertTrue(all((destination / row["file"]).is_file() for row in copied))

            unsafe = [dict(row) for row in rows]
            unsafe[0] = {**unsafe[0], "file": "../outside.bak"}
            (source / "backend.json").write_text(
                json.dumps({"backups": unsafe}), encoding="utf-8"
            )
            with self.assertRaisesRegex(RuntimeError, "unsafe filename"):
                backend.load_backup_source(source, destination)

    def test_backup_source_rejects_duplicate_database_or_filename(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source"
            destination = Path(temporary) / "destination"
            source.mkdir()
            destination.mkdir()
            rows = valid_backup_rows(source)

            duplicated_database = [dict(row) for row in rows]
            duplicated_database[-1]["name"] = duplicated_database[0]["name"]
            (source / "backend.json").write_text(
                json.dumps({"backups": duplicated_database}), encoding="utf-8"
            )
            with self.assertRaisesRegex(RuntimeError, "required database set"):
                backend.load_backup_source(source, destination)

            duplicated_file = [dict(row) for row in rows]
            duplicated_file[-1]["file"] = duplicated_file[0]["file"]
            duplicated_file[-1]["sha256"] = duplicated_file[0]["sha256"]
            (source / "backend.json").write_text(
                json.dumps({"backups": duplicated_file}), encoding="utf-8"
            )
            with self.assertRaisesRegex(RuntimeError, "duplicate filenames"):
                backend.load_backup_source(source, destination)

    def test_backup_manifest_rejects_invalid_logical_file_metadata(self):
        row = {
            "name": backend.DATABASES[0],
            "file": "database.bak",
            "sha256": "a" * 64,
            "files": [
                {"name": "data\nMOVE injection", "type": "D"},
                {"name": "log", "type": "L"},
            ],
        }
        with self.assertRaisesRegex(RuntimeError, "invalid logical file"):
            backend.backup_row(row)

    def test_live_source_backup_needs_explicit_opt_in(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            stage = root / "stage"
            work = root / "work"
            (source / "WorldServer").mkdir(parents=True)
            (source / "WorldServer/WorldServer.ini").write_text("", encoding="ascii")
            (stage / "server/sdgo").mkdir(parents=True)
            (stage / "server/sdgo/core.dll").write_bytes(b"fixture")
            with self.assertRaisesRegex(RuntimeError, "allow-live-copy-only"):
                backend.prepare(work, stage, source, "127.90.0.1")
            self.assertFalse(work.exists())

    def test_absolute_paths_rebase_only_from_owned_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "source"
            owned = root / "owned"
            source.mkdir()
            owned.mkdir()
            original = source / "WorldServer/data/table.bin"
            expected = owned / "WorldServer/data/table.bin"
            self.assertEqual(
                Path(backend.rebase_owned_path(str(original), source, owned)),
                expected,
            )
            package_runtime = source.parent / ".local_runtime/state"
            self.assertEqual(
                Path(backend.rebase_owned_path(str(package_runtime), source, owned)),
                owned.parent / ".local_runtime/state",
            )
            with self.assertRaisesRegex(RuntimeError, "external absolute path"):
                backend.rebase_owned_path(
                    str(root.parent / "outside/data.bin"), source, owned
                )

    def test_mod_ini_rebase_preserves_unrelated_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "source"
            owned = root / "owned"
            source.mkdir()
            owned.mkdir()
            path = root / "mod.ini"
            path.write_text(
                "[settings]\nasset=" + str(source / "shared/file.bin") +
                "\nmode=preserve\n",
                encoding="utf-8",
            )
            self.assertEqual(backend.rebase_owned_ini(path, source, owned), 1)
            parsed = backend.ini_values(path.read_bytes())
            self.assertEqual(parsed["settings"]["asset"], str(owned / "shared/file.bin"))
            self.assertEqual(parsed["settings"]["mode"], "preserve")

    def test_locked_session_proof_requires_private_identity_default_zero_and_foreground(self):
        class Api:
            default_count = 0
            foreground = 99

            def desktop_state(self, desktop, pid):
                return game_test_windows._DesktopState("owned-private", 0, 0)

            def named_desktop_state(self, name, pid):
                return game_test_windows._DesktopState(
                    "Default", self.default_count, self.default_count
                )

            def foreground_pid(self):
                return self.foreground

        owner = backend.BackendBackgroundProcess.__new__(
            backend.BackendBackgroundProcess
        )
        owner.pid = 100
        owner.run_id = "offline"
        owner.desktop_name = "owned-private"
        owner.desktop_path = r"WinSta0\owned-private"
        owner._desktop_handle = 123
        owner._api = Api()
        owner._input_desktop_at_launch = None
        owner._input_desktop_launch_error = "PermissionError: access denied"

        receipt = owner.verify_background()
        self.assertTrue(receipt["verified"], receipt)
        self.assertFalse(receipt["input_desktop_observed"])
        self.assertEqual(receipt["input_desktop_observation_error"], "access_denied")
        self.assertFalse(receipt["switch_desktop_called"])

        owner._api.default_count = 1
        self.assertFalse(owner.verify_background()["verified"])
        owner._api.default_count = 0
        owner._api.foreground = 100
        self.assertFalse(owner.verify_background()["verified"])

    @unittest.skipUnless(os.name == "nt", "Windows private desktop test")
    def test_real_server_owner_stays_off_default_desktop(self):
        owner = backend.BackendBackgroundProcess(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            cwd=ROOT,
            env=os.environ.copy(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            run_id="backend-owner-offline-test",
        )
        try:
            receipt = owner.verify_background()
            self.assertTrue(receipt["verified"], receipt)
            self.assertEqual(receipt["named_desktop_name"].casefold(), "default")
            self.assertEqual(receipt["named_desktop_target_window_count"], 0)
            self.assertNotEqual(receipt["foreground_pid"], owner.pid)
        finally:
            backend.stop_owned_process(owner)

    def test_owned_process_is_stopped_and_closed_before_retry(self):
        process = FakeProcess()
        backend.stop_owned_process(process)
        self.assertEqual(process.killed, 1)
        self.assertEqual(process.closed, 1)
        self.assertIsNotNone(process.poll())

        stuck = FakeProcess(exit_after_kill=False)
        with self.assertRaises(RuntimeError):
            backend.stop_owned_process(stuck)
        self.assertEqual(stuck.closed, 0)

    def test_termination_grace_waits_for_original_handle(self):
        process = FakeProcess()
        process.kill = mock.Mock(side_effect=TimeoutError("native teardown pending"))
        backend.stop_owned_process(process)
        self.assertEqual(process.waited, 1)
        self.assertEqual(process.closed, 1)
        self.assertIsNotNone(process.poll())

        stuck = FakeProcess(exit_after_kill=False)
        stuck.kill = mock.Mock(side_effect=TimeoutError("native teardown pending"))
        with self.assertRaises(RuntimeError):
            backend.stop_owned_process(stuck)
        self.assertEqual(stuck.closed, 0)

    def test_container_identity_requires_owner_token_loopback_and_owned_volume(self):
        private = {
            "owner": r"E:\owned",
            "owner_token": "a" * 32,
            "container": "sdgo-gametest-123456789abc",
            "container_id": "container-id",
            "volume": "sdgo-gametest-123456789abc-data",
            "port": 59874,
        }
        value = {
            "Id": "container-id",
            "Name": "/sdgo-gametest-123456789abc",
            "Config": {"Labels": backend.owned_labels(private)},
            "HostConfig": {
                "NetworkMode": "bridge",
                "PortBindings": {
                    "1433/tcp": [{"HostIp": "127.0.0.1", "HostPort": "59874"}]
                },
            },
            "Mounts": [
                {
                    "Type": "volume",
                    "Name": "sdgo-gametest-123456789abc-data",
                    "Destination": "/var/opt/mssql",
                }
            ],
        }
        with mock.patch.object(backend, "docker", return_value=json.dumps([value])):
            self.assertEqual(backend.identity(private)["Id"], "container-id")
        value["HostConfig"]["PortBindings"]["1433/tcp"][0]["HostIp"] = "0.0.0.0"
        with mock.patch.object(backend, "docker", return_value=json.dumps([value])):
            with self.assertRaisesRegex(RuntimeError, "identity mismatch"):
                backend.identity(private)

    def test_partial_prepare_cleanup_removes_only_private_marker(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            private = {
                "owner": str(work),
                "owner_token": "b" * 32,
                "container": "sdgo-gametest-abcdef123456",
            }
            (work / "private.json").write_text(json.dumps(private), encoding="utf-8")
            (work / "backend.json").write_text(
                json.dumps({"prepared": False}), encoding="utf-8"
            )
            backups = work / "backups"
            backups.mkdir()
            (backups / "receipt.txt").write_text("preserve", encoding="ascii")
            (work / "StoreServer.console.log").write_text(
                "private native log", encoding="utf-8"
            )
            (work / "preserve-result.json").write_text("{}", encoding="ascii")
            with (
                mock.patch.object(backend, "container_names", return_value=set()),
                mock.patch.object(backend, "volume_names", return_value=set()),
                mock.patch.object(backend, "emit"),
            ):
                backend.cleanup(work, False)
            self.assertFalse((work / "private.json").exists())
            self.assertTrue((backups / "receipt.txt").is_file())
            self.assertFalse((work / "StoreServer.console.log").exists())
            self.assertTrue((work / "preserve-result.json").is_file())
            receipt = json.loads((work / "cleanup.json").read_text(encoding="utf-8"))
            self.assertFalse(receipt["container_removed"])
            self.assertFalse(receipt["volume_removed"])
            self.assertEqual(
                receipt["console_logs_removed"], ["StoreServer.console.log"]
            )

    def test_cleanup_rejects_reparse_target_before_docker_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            private = {
                "owner": str(work),
                "owner_token": "c" * 32,
                "container": "sdgo-gametest-fedcba654321",
            }
            (work / "private.json").write_text(json.dumps(private), encoding="utf-8")
            with (
                mock.patch.object(backend, "is_reparse_point", return_value=True),
                mock.patch.object(backend, "container_names") as containers,
            ):
                with self.assertRaisesRegex(RuntimeError, "reparse point"):
                    backend.cleanup(work, True)
            containers.assert_not_called()
            self.assertTrue((work / "private.json").is_file())

    def test_runtime_cleanup_is_fixed_scope_and_preserves_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            private = {
                "owner": str(work),
                "owner_token": "d" * 32,
                "container": "sdgo-gametest-012345abcdef",
            }
            (work / "private.json").write_text(json.dumps(private), encoding="utf-8")
            for name in ("Server", "backups", ".local_runtime"):
                (work / name).mkdir()
                (work / name / "owned.txt").write_text("owned", encoding="ascii")
            (work / "result.json").write_text("{}", encoding="ascii")
            with (
                mock.patch.object(backend, "container_names", return_value=set()),
                mock.patch.object(backend, "volume_names", return_value=set()),
                mock.patch.object(backend, "emit"),
            ):
                backend.cleanup(work, True)
            for name in ("Server", "backups", ".local_runtime"):
                self.assertFalse((work / name).exists())
            self.assertTrue((work / "result.json").is_file())

    def test_public_status_does_not_read_or_emit_private_credentials(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            (work / "backend.json").write_text(
                json.dumps(
                    {
                        "prepared": True,
                        "target_ip": "127.90.0.1",
                        "backup_origin": "reused",
                    }
                ),
                encoding="utf-8",
            )
            (work / "private.json").write_text(
                json.dumps({"app_password": "never-print-this"}), encoding="utf-8"
            )
            (work / "status.json").write_text(
                json.dumps({"phase": "ready", "pid": 101, "services": [{}, {}]}),
                encoding="utf-8",
            )
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                backend.public_status(work)
            value = output.getvalue()
            self.assertNotIn("never-print-this", value)
            self.assertNotIn("private", value)
            self.assertEqual(json.loads(value)["service_count"], 2)


if __name__ == "__main__":
    unittest.main()
