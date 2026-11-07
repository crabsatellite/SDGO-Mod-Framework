"""Sealed login-recovery GameTest inputs and metadata-only proof parsing."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import test_game_test as fixtures
from tools import game_test_login_recovery as recovery

game_test = fixtures.game_test


class LoginRecoveryPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = fixtures.GameTestRunnerTests("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.addCleanup(self.fixture.tearDown)
        observer = (
            self.fixture.stage /
            "client/sdgo/mods/game_test_client/mod.ini"
        )
        observer_config = game_test.read_ini(observer)
        observer_config["mod"]["version"] = "0.1.8"
        game_test.write_ini(observer, observer_config)
        account = game_test.read_ini(
            self.fixture.account_profile
        )
        account["registration"]["state"] = "existing_account"
        game_test.write_ini(
            self.fixture.account_profile,
            account,
        )
        staged = (
            self.fixture.stage /
            "client/sdgo/mods/login_recovery_client"
        )
        staged.mkdir(parents=True)
        (staged / "mod.ini").write_text(
            "[mod]\n"
            "id=login_recovery_client\n"
            "dll=login_recovery_client.dll\n"
            f"hosts={game_test.HOST_KEY}\n",
            encoding="utf-8",
        )
        (staged / "login_recovery_client.dll").write_bytes(
            b"login recovery fixture"
        )

    def prepare(self, name: str, **changes):
        options = {
            "account_profile": self.fixture.account_profile,
            "pages_only": True,
            "page_group": "hangar",
            "retry_count": 1,
            "expect_first_fail": False,
            "login_recovery_min_retries": 0,
            **changes,
        }
        path = self.fixture.root / name
        return path, game_test.prepare(
            self.fixture.template,
            self.fixture.stage,
            path,
            **options,
        )

    def test_recovery_fields_roundtrip_and_are_hash_sealed(self) -> None:
        run, metadata = self.prepare(
            "recovery",
            login_recovery_min_retries=1,
            login_recovery_hold_after_completion=True,
        )
        expected = {
            "login_recovery": True,
            "login_recovery_min_retries": 1,
            "login_recovery_hold_after_completion": True,
        }
        self.assertEqual(
            expected,
            {key: metadata["plan"][key] for key in expected},
        )
        public = game_test.read_public_plan(run / "client")
        self.assertEqual(
            expected,
            {key: public[key] for key in expected},
        )
        config = game_test.read_ini(
            run / "client/sdgo/gametest/run.ini"
        )["run"]
        self.assertEqual(config["login_recovery"], "1")
        self.assertEqual(
            config["login_recovery_min_retries"], "1"
        )
        self.assertEqual(
            config["login_recovery_hold_after_completion"], "1"
        )
        self.assertTrue(
            (
                run /
                "client/sdgo/mods/login_recovery_client/"
                "login_recovery_client.dll"
            ).is_file()
        )
        game_test.verify_hashes(
            game_test.validate_metadata(run)
        )

    def test_recovery_scope_rejects_unsealed_or_registration_paths(self) -> None:
        invalid = (
            {"login_recovery_min_retries": True},
            {"login_recovery_min_retries": 2},
            {
                "pages_only": False,
                "page_group": None,
            },
            {
                "retry_count": 2,
                "expect_first_fail": True,
            },
            {
                "login_recovery_min_retries": None,
                "login_recovery_hold_after_completion": True,
            },
        )
        for index, changes in enumerate(invalid):
            with self.subTest(changes=changes):
                with self.assertRaises(game_test.GameTestError):
                    self.prepare(f"invalid-{index}", **changes)

    def test_run_ini_rejects_partial_and_noncanonical_recovery_fields(
        self,
    ) -> None:
        run, _ = self.prepare("tamper")
        path = run / "client/sdgo/gametest/run.ini"
        for key, value in (
            ("login_recovery_min_retries", "01"),
            ("login_recovery_hold_after_completion", "true"),
        ):
            config = game_test.read_ini(path)
            config["run"][key] = value
            game_test.write_ini(path, config)
            with self.assertRaisesRegex(
                game_test.GameTestError,
                "invalid normal-flow run.ini",
            ):
                game_test.read_public_plan(run / "client")
            config["run"][key] = (
                "0" if key.endswith("completion") else "0"
            )
            game_test.write_ini(path, config)
        config = game_test.read_ini(path)
        config.remove_option(
            "run", "login_recovery_hold_after_completion"
        )
        game_test.write_ini(path, config)
        with self.assertRaisesRegex(
            game_test.GameTestError,
            "seal every recovery field",
        ):
            game_test.read_public_plan(run / "client")


class LoginRecoveryProofTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.directory = Path(self.root.name)
        self.run_id = "11111111-2222-3333-8444-555555555555"

    def write_events(self, messages: list[tuple[str, str]]) -> Path:
        path = self.directory / "events.jsonl"
        rows = [
            {
                "run_id": self.run_id,
                "event": "observation",
                "case": case,
                "message": message,
            }
            for case, message in messages
        ]
        path.write_text(
            "".join(
                json.dumps(row, sort_keys=True) + "\n"
                for row in rows
            ),
            encoding="utf-8",
        )
        return path

    def final(self, *, retries: int = 1, reject: int = 1002) -> str:
        return (
            "login_recovery_final generation=7 state=6 outcome=1 "
            f"retries={retries} retry_sends_delta={retries} "
            f"rebuilds_delta={retries} connection_delta={retries} "
            f"auth_replies_delta={2 if reject else 1} "
            f"reject={reject} app=5 pilot=104 "
            "credentials=0 active=0 interaction=0"
        )

    def faulted_login_messages(self) -> list[tuple[str, str]]:
        return [
            ("flow.login", recovery.READY_TO_SEND_MESSAGE),
            ("flow.login", recovery.CONTROLLER_ACK_MESSAGE),
            (
                "flow.login",
                "login_recovery_native_transport "
                "sequence=11 opcode=2329 size=77 "
                "success=1 result=1",
            ),
            ("flow.login", recovery.INITIAL_SEND_MESSAGE),
            (
                "flow.login",
                "login_recovery_native_capture "
                "generation=7 state=1 active=1 credentials=1",
            ),
        ]

    def test_native_proof_requires_one_initial_send_and_real_success(
        self,
    ) -> None:
        path = self.write_events([
            *self.faulted_login_messages(),
            ("flow.onboarding", self.final()),
        ])
        proof = recovery.validate_native_evidence(
            path,
            run_id=self.run_id,
            minimum_retries=1,
        )
        self.assertEqual(proof["pilot"], 104)
        self.assertEqual(proof["reject"], 1002)

    def test_native_proof_rejects_missing_retry_or_duplicate_send(
        self,
    ) -> None:
        for messages in (
            [
                *self.faulted_login_messages(),
                ("flow.onboarding", self.final(
                    retries=0, reject=0
                )),
            ],
            [
                *self.faulted_login_messages(),
                ("flow.login", recovery.INITIAL_SEND_MESSAGE),
                ("flow.onboarding", self.final()),
            ],
        ):
            with self.subTest(messages=messages):
                with self.assertRaises(
                    recovery.LoginRecoveryGameTestError
                ):
                    recovery.validate_native_evidence(
                        self.write_events(messages),
                        run_id=self.run_id,
                        minimum_retries=1,
                    )

    def test_scenarios_arm_faults_after_sealed_native_ui_ready(self) -> None:
        self.assertEqual(
            recovery.SCENARIOS["baseline"].minimum_retries,
            0,
        )
        self.assertEqual(
            recovery.SCENARIOS[
                "transient-blackhole"
            ].arm_after_client_bytes,
            recovery.MINIMUM_FAULT_WIRE_BYTES,
        )
        self.assertTrue(
            recovery.SCENARIOS[
                "stale-session-hold"
            ].hold_after_completion
        )

    def test_expected_hold_termination_requires_every_independent_guard(
        self,
    ) -> None:
        report = {
            "status": "fail",
            "layers": {
                "source": {"status": "bound"},
                "launch": {
                    "status": "fail",
                    "execution": {
                        "status": "launch_error",
                    },
                },
            },
        }
        termination = {
            "verified": True,
            "terminated": True,
            "pid": 1234,
            "image_name": "GOnline.exe",
        }
        native = {
            "state": 6,
            "outcome": 1,
            "pilot": 104,
            "credentials": 0,
            "active": 0,
        }
        relay = {
            "mode": (
                recovery.FaultMode.
                HOLD_UPSTREAM_ON_CLIENT_CLOSE
            ),
            "upstream_hold_count": 1,
        }
        self.assertTrue(
            recovery._verified_hold_termination(
                report=report,
                hold_terminated=True,
                hold_termination=termination,
                native_proof=native,
                relay_proof=relay,
            )
        )
        for changes in (
            {"hold_terminated": False},
            {"hold_termination": None},
            {
                "relay_proof": {
                    **relay,
                    "upstream_hold_count": 0,
                },
            },
            {
                "report": {
                    **report,
                    "layers": {
                        **report["layers"],
                        "source": {"status": "pass"},
                    },
                },
            },
            {
                "report": {
                    **report,
                    "layers": {
                        **report["layers"],
                        "launch": {
                            "status": "fail",
                            "execution": {
                                "status": "completed",
                            },
                        },
                    },
                },
            },
        ):
            arguments = {
                "report": report,
                "hold_terminated": True,
                "hold_termination": termination,
                "native_proof": native,
                "relay_proof": relay,
                **changes,
            }
            with self.subTest(changes=changes):
                self.assertFalse(
                    recovery._verified_hold_termination(
                        **arguments
                    )
                )


if __name__ == "__main__":
    unittest.main()
