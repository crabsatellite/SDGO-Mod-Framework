"""Negative login-recovery proof parsing and private profile isolation."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import game_test
from tools import game_test_login_recovery as recovery
from tools import game_test_login_terminal as terminal


class TerminalProofTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.run_id = "11111111-2222-3333-8444-555555555555"

    def terminal_message(
        self,
        scenario: terminal.TerminalScenario,
        *,
        credentials: int = 0,
        gate: int = 1,
    ) -> str:
        rebuilds = max(1, scenario.expected_retries + 1)
        connection = (
            4 if scenario.name == "exhaustion" else 1
        )
        auth_replies = 1 if scenario.wrong_password else 0
        return (
            "login_recovery_terminal generation=7 "
            f"state={scenario.expected_state} "
            f"outcome={scenario.expected_outcome} "
            f"retries={scenario.expected_retries} "
            f"retry_sends_delta={scenario.expected_retries} "
            f"rebuilds_delta={rebuilds} "
            f"connection_delta={connection} "
            f"auth_replies_delta={auth_replies} "
            f"reject={scenario.expected_reject} "
            f"app=4 gate={gate} ready=1 "
            f"credentials={credentials} active=0 interaction=0"
        )

    def write_events(
        self,
        scenario: terminal.TerminalScenario,
        *,
        credentials: int = 0,
        gate: int = 1,
    ) -> Path:
        messages = []
        if scenario.faulted:
            messages.extend((
                recovery.READY_TO_SEND_MESSAGE,
                recovery.CONTROLLER_ACK_MESSAGE,
            ))
        messages.extend((
            "login_recovery_native_transport "
            "sequence=11 opcode=2329 size=77 "
            "success=1 result=1",
            recovery.INITIAL_SEND_MESSAGE,
            "login_recovery_native_capture "
            "generation=7 state=1 active=1 credentials=1",
            self.terminal_message(
                scenario,
                credentials=credentials,
                gate=gate,
            ),
        ))
        rows = [
            {
                "run_id": self.run_id,
                "event": "observation",
                "case": "flow.login",
                "status": "info",
                "message": message,
            }
            for message in messages
        ]
        rows.extend((
            {
                "run_id": self.run_id,
                "event": "assert",
                "case": "flow.login",
                "status": "fail",
                "message": terminal.EXPECTED_TERMINAL_ASSERT,
            },
            {
                "run_id": self.run_id,
                "event": "case_end",
                "case": "flow.login",
                "status": "fail",
                "message": "failed",
            },
            {
                "run_id": self.run_id,
                "event": "run_end",
                "case": "",
                "status": "fail",
                "message": "failed",
            },
        ))
        path = self.root / f"{scenario.name}.jsonl"
        path.write_text(
            "".join(
                json.dumps(row, sort_keys=True) + "\n"
                for row in rows
            ),
            encoding="utf-8",
        )
        return path

    def test_all_terminal_scenarios_require_restored_ui_then_fail(
        self,
    ) -> None:
        for scenario in terminal.SCENARIOS.values():
            with self.subTest(scenario=scenario.name):
                proof = terminal.validate_terminal_native_evidence(
                    self.write_events(scenario),
                    run_id=self.run_id,
                    scenario=scenario,
                )
                self.assertEqual(proof["app"], 4)
                self.assertEqual(proof["ready"], 1)
                self.assertEqual(proof["credentials"], 0)

    def test_offline_gate_one_is_accepted_but_credentials_are_not(
        self,
    ) -> None:
        scenario = terminal.SCENARIOS["exhaustion"]
        proof = terminal.validate_terminal_native_evidence(
            self.write_events(scenario, gate=1),
            run_id=self.run_id,
            scenario=scenario,
        )
        self.assertEqual(proof["gate"], 1)
        with self.assertRaises(
            terminal.LoginTerminalGameTestError
        ):
            terminal.validate_terminal_native_evidence(
                self.write_events(
                    scenario,
                    credentials=1,
                ),
                run_id=self.run_id,
                scenario=scenario,
            )

    def test_expected_report_schema_requires_bound_completed_native_failure(
        self,
    ) -> None:
        report = {
            "status": "fail",
            "layers": {
                "source": {"status": "bound"},
                "launch": {"status": "pass"},
                "native": {
                    "status": "fail",
                    "cases": {
                        "flow.login": {"status": "fail"},
                    },
                },
            },
        }
        with (
            mock.patch.object(
                terminal.game_test,
                "validate_metadata",
                return_value={
                    "execution": {"status": "completed"},
                },
            ) as validate_metadata,
            mock.patch.object(
                terminal.game_test,
                "verify_hashes",
            ) as verify_hashes,
        ):
            terminal.validate_expected_report_failure(
                report,
                run_dir=self.root,
            )
        validate_metadata.assert_called_once_with(self.root)
        verify_hashes.assert_called_once_with({
            "execution": {"status": "completed"},
        })

        invalid_reports = (
            {
                **report,
                "layers": {
                    **report["layers"],
                    "source": {"status": "pass"},
                },
            },
            {
                **report,
                "layers": {
                    **report["layers"],
                    "launch": {"status": "fail"},
                },
            },
            {
                **report,
                "layers": {
                    **report["layers"],
                    "native": {
                        "status": "pass",
                        "cases": {
                            "flow.login": {"status": "fail"},
                        },
                    },
                },
            },
            {
                **report,
                "layers": {
                    **report["layers"],
                    "native": {
                        "status": "fail",
                        "cases": {
                            "flow.login": {"status": "pass"},
                        },
                    },
                },
            },
        )
        for invalid in invalid_reports:
            with self.subTest(invalid=invalid):
                with self.assertRaises(
                    terminal.LoginTerminalGameTestError
                ):
                    terminal.validate_expected_report_failure(
                        invalid,
                        run_dir=self.root,
                    )
        with (
            mock.patch.object(
                terminal.game_test,
                "validate_metadata",
                return_value={
                    "execution": {"status": "launch_error"},
                },
            ),
            mock.patch.object(
                terminal.game_test,
                "verify_hashes",
            ),
        ):
            with self.assertRaises(
                terminal.LoginTerminalGameTestError
            ):
                terminal.validate_expected_report_failure(
                    report,
                    run_dir=self.root,
                )


class CancelFaultReadinessTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "relay.jsonl"
        self.run_id = "11111111-2222-3333-8444-555555555555"

    def write(self, rows: list[dict]) -> None:
        self.path.write_text(
            "".join(
                json.dumps(
                    {"run_id": self.run_id, **row},
                    sort_keys=True,
                ) + "\n"
                for row in rows
            ),
            encoding="utf-8",
        )

    def test_cancel_waits_for_positive_ordered_dropped_reply(self) -> None:
        armed = {
            "event": "server_blackhole_armed",
            "generation": 1,
            "sequence": 4,
            "client_bytes": 93,
        }
        dropped = {
            "event": "server_bytes_dropped",
            "generation": 1,
            "sequence": 5,
            "byte_count": 41,
            "total_dropped": 41,
        }
        for rows in (
            [],
            [armed],
            [armed, {**dropped, "byte_count": 0}],
            [armed, {**dropped, "sequence": 3}],
        ):
            with self.subTest(rows=rows):
                self.write(rows)
                self.assertFalse(
                    terminal._cancel_fault_reply_observed(
                        self.path,
                        run_id=self.run_id,
                    )
                )
        self.write([armed, dropped])
        self.assertTrue(
            terminal._cancel_fault_reply_observed(
                self.path,
                run_id=self.run_id,
            )
        )

    def test_cancel_rejects_foreign_fault_evidence(self) -> None:
        self.write([{
            "event": "server_blackhole_armed",
            "generation": 1,
            "sequence": 4,
            "client_bytes": 93,
            "run_id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        }])
        with self.assertRaises(
            terminal.LoginTerminalGameTestError
        ):
            terminal._cancel_fault_reply_observed(
                self.path,
                run_id=self.run_id,
            )


class WrongPasswordProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_private_wrong_md5_preserves_source_and_username(
        self,
    ) -> None:
        source = self.root / "account.ini"
        source.write_text(
            "[account]\n"
            "username=existing_user\n"
            "password=CorrectPass123\n"
            "password_format=plain\n"
            "[registration]\n"
            "state=existing_account\n",
            encoding="utf-8",
        )
        before = source.read_bytes()
        target = self.root / "wrong.ini"
        returned = terminal._make_wrong_password_profile(
            source,
            target,
        )
        copied = game_test.load_account_profile(target)
        self.assertEqual(returned, before)
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(
            copied["account"]["username"],
            "existing_user",
        )
        self.assertEqual(
            copied["account"]["password_format"],
            "md5",
        )
        self.assertEqual(
            copied["registration"]["state"],
            "existing_account",
        )
        self.assertNotEqual(
            copied["account"]["password"],
            "CorrectPass123",
        )


class HiddenCancelWindowTests(unittest.TestCase):
    def test_authoritative_read_guard_selects_hidden_main_window(
        self,
    ) -> None:
        selected = terminal._select_owned_cancel_window(
            [
                {
                    "hwnd": 101,
                    "pid": 77,
                    "owner_hwnd": 0,
                    "class_name": "unexpected-skin-class",
                    "title": "localized hidden title",
                    "visible": False,
                },
                {
                    "hwnd": 102,
                    "pid": 77,
                    "owner_hwnd": 0,
                    "class_name":
                        "SDGO.GameTest.GraphicsProxy",
                    "title": "",
                    "visible": False,
                },
                {
                    "hwnd": 103,
                    "pid": 88,
                    "owner_hwnd": 0,
                    "class_name": "_GONLINE_",
                    "title": "9YOU SDGO",
                    "visible": True,
                },
            ],
            pid=77,
            authoritative_hwnd=101,
        )
        self.assertEqual(selected["hwnd"], 101)
        self.assertFalse(selected["visible"])

    def test_ambiguous_or_owned_child_window_is_rejected(self) -> None:
        native = {
            "pid": 77,
            "owner_hwnd": 0,
            "class_name": "_GONLINE_",
            "title": "9YOU SDGO",
            "visible": False,
        }
        with self.assertRaises(
            terminal.LoginTerminalGameTestError
        ):
            terminal._select_owned_cancel_window(
                [
                    {"hwnd": 101, **native},
                    {"hwnd": 101, **native},
                ],
                pid=77,
                authoritative_hwnd=101,
            )
        with self.assertRaises(
            terminal.LoginTerminalGameTestError
        ):
            terminal._select_owned_cancel_window(
                [{
                    "hwnd": 103,
                    **native,
                    "owner_hwnd": 55,
                }],
                pid=77,
                authoritative_hwnd=103,
            )


class InitialCapsuleReuseTests(unittest.TestCase):
    class StopAfterPreparation(RuntimeError):
        pass

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.run_dir = self.root / "owned-run"
        self.profile = self.root / "account.ini"
        self.profile.write_text(
            "[account]\n"
            "username=existing_user\n"
            "password=CorrectPass123\n"
            "password_format=plain\n"
            "[registration]\n"
            "state=existing_account\n",
            encoding="utf-8",
        )
        self.upstream = terminal.Endpoint("127.0.0.1", 5001)

    def metadata(self) -> dict:
        return {
            "run_id": "11111111-2222-3333-8444-555555555555",
            "client": {
                "events": str(self.run_dir / "native-events.jsonl"),
            },
        }

    def invoke_until_relay(
        self,
        scenario: terminal.TerminalScenario,
        **patches,
    ) -> None:
        defaults = {
            "prepare": mock.DEFAULT,
            "reuse": mock.DEFAULT,
            **patches,
        }
        with (
            mock.patch.object(
                terminal.recovery,
                "preflight_upstream",
            ),
            mock.patch.object(
                terminal.recovery,
                "_reserve_loopback_port",
                return_value=51001,
            ),
            mock.patch.object(
                terminal.game_test,
                "prepare",
                return_value=self.metadata(),
            ) as prepared,
            mock.patch.object(
                terminal.game_test,
                "reuse_prepared",
                return_value=self.metadata(),
            ) as reused,
            mock.patch.object(
                terminal,
                "LoginFaultRelay",
                side_effect=self.StopAfterPreparation,
            ),
        ):
            with self.assertRaises(self.StopAfterPreparation):
                terminal.prepare_and_run(
                    client_template=self.root / "template",
                    stage=self.root / "stage",
                    run_dir=self.run_dir,
                    account_profile=self.profile,
                    upstream=self.upstream,
                    scenario=scenario,
                )
        defaults["prepare"] = prepared
        defaults["reuse"] = reused
        self.calls = defaults

    def test_existing_capsule_reuses_and_fresh_path_prepares(self) -> None:
        self.run_dir.mkdir()
        (self.run_dir / "run.json").write_text(
            '{"execution":{"status":"pass"}}',
            encoding="utf-8",
        )
        self.invoke_until_relay(
            terminal.SCENARIOS["exhaustion"]
        )
        self.calls["reuse"].assert_called_once()
        self.calls["prepare"].assert_not_called()

        shutil_target = self.root / "fresh-run"
        self.run_dir = shutil_target
        self.run_dir.mkdir()
        self.invoke_until_relay(
            terminal.SCENARIOS["exhaustion"]
        )
        self.calls["prepare"].assert_called_once()
        self.calls["reuse"].assert_not_called()

    def test_wrong_password_rebind_releases_then_reacquires_source_lock(
        self,
    ) -> None:
        self.run_dir.mkdir()
        (self.run_dir / "run.json").write_text(
            '{"execution":{"status":"pass","cleanup_errors":[]}}',
            encoding="utf-8",
        )
        lock_path = game_test.account_profile_lock_path(
            self.profile
        )
        events: list[str] = []

        def acquire(_path, _label):
            token = "first" if not events else "second"
            events.append("acquire-" + token)
            return lock_path, token

        def release(_path, token):
            events.append("release-" + token)

        def reuse(*args, **kwargs):
            events.append("reuse")
            self.assertTrue(kwargs["rebind_existing_account"])
            self.assertEqual(
                kwargs["account_profile"],
                self.run_dir.parent
                / f".{self.run_dir.name}.wrong-password-account.ini",
            )
            return self.metadata()

        with (
            mock.patch.object(
                terminal.recovery, "preflight_upstream"
            ),
            mock.patch.object(
                terminal.recovery,
                "_reserve_loopback_port",
                return_value=51001,
            ),
            mock.patch.object(
                terminal.game_test,
                "acquire_exclusive_lock",
                side_effect=acquire,
            ),
            mock.patch.object(
                terminal.game_test,
                "release_owned_lock",
                side_effect=release,
            ),
            mock.patch.object(
                terminal.game_test,
                "reuse_prepared",
                side_effect=reuse,
            ),
            mock.patch.object(
                terminal.game_test,
                "prepare",
            ) as prepared,
            mock.patch.object(
                terminal.game_test,
                "validate_metadata",
                return_value={
                    "client": {"root": str(self.root / "client")},
                    "plan": {},
                },
            ),
            mock.patch.object(
                terminal.game_test,
                "validate_private_account",
                return_value=lock_path,
            ),
            mock.patch.object(
                terminal,
                "LoginFaultRelay",
                side_effect=self.StopAfterPreparation,
            ),
        ):
            with self.assertRaises(self.StopAfterPreparation):
                terminal.prepare_and_run(
                    client_template=self.root / "template",
                    stage=self.root / "stage",
                    run_dir=self.run_dir,
                    account_profile=self.profile,
                    upstream=self.upstream,
                    scenario=terminal.SCENARIOS[
                        "wrong-password"
                    ],
                )
        prepared.assert_not_called()
        self.assertEqual(
            events[:4],
            [
                "acquire-first",
                "release-first",
                "reuse",
                "acquire-second",
            ],
        )


if __name__ == "__main__":
    unittest.main()
