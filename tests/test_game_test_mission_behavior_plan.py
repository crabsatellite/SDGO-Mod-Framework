from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "game_test_mission_behavior_contract",
    ROOT / "tools" / "game_test.py",
)
game_test = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(game_test)


class MissionBehaviorPlanTests(unittest.TestCase):
    """Offline sealed-plan tests; no game, server, SQL, or desktop launch."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    @staticmethod
    def account():
        profile = game_test.ini_parser()
        profile["account"] = {
            "username": "MissionBehav",
            "password": "offline-password",
            "password_format": "plain",
        }
        profile["registration"] = {
            "state": "existing_account",
            "retry_count": "1",
            "expect_first_fail": "0",
        }
        return profile

    def plan(self, profile="oc", mission_id=88006):
        plan = game_test.make_plan(
            "127.0.0.1",
            5001,
            "1",
            "1",
            "1",
            1,
            False,
            3,
            None,
            mission_mode=5,
            mission_behavior_profile=profile,
            mission_game_mode_id=mission_id,
        )
        plan["network_policy"] = "direct"
        plan["quiet"] = True
        return plan

    def test_oc_and_gco_use_the_exact_native_mission_and_dedicated_cases(self):
        for profile in game_test.MISSION_BEHAVIOR_PROFILES:
            for mission_id in game_test.MISSION_BEHAVIOR_TARGETS:
                plan = self.plan(profile, mission_id)
                self.assertEqual(plan["mission_scope"], game_test.MISSION_BEHAVIOR_SCOPE)
                self.assertEqual(
                    game_test.metadata_suite_for_plan(plan),
                    game_test.MISSION_BEHAVIOR_SUITE,
                )
                self.assertEqual(plan["mission_behavior_profile"], profile)
                self.assertEqual(plan["mission_game_mode_id"], mission_id)
                self.assertEqual(
                    game_test.required_cases_for_plan(plan)[-1:],
                    ("mission.behavior",),
                )
                self.assertFalse(plan["mission_clear_claim"])
                self.assertFalse(plan["mission_result_claim"])
                self.assertFalse(plan["mission_active_exit_claim"])

    def test_behavior_plan_roundtrips_through_the_public_run_contract(self):
        client = self.root / "client"
        client.mkdir()
        run_id = str(uuid.uuid4())
        plan = self.plan("gco", 88009)
        game_test.write_job(
            client,
            run_id,
            plan,
            self.account(),
            self.root / "account.ini",
        )
        parsed = game_test.read_public_plan(client)
        self.assertEqual(parsed, plan)
        run = game_test.read_ini(client / "sdgo/gametest/run.ini")["run"]
        self.assertEqual(run["mission_behavior_profile"], "gco")
        self.assertEqual(run["mission_game_mode_id"], "88009")
        self.assertEqual(
            run["required_cases"].split(",")[-1:],
            ["mission.behavior"],
        )

    def test_metadata_cannot_change_the_sealed_profile_or_game_mode_id(self):
        client = self.root / "client"
        client.mkdir()
        game = client / game_test.GAME_EXE
        game.write_bytes(b"offline")
        run_id = str(uuid.uuid4())
        plan = self.plan("oc", 88006)
        game_test.write_job(
            client,
            run_id,
            plan,
            self.account(),
            self.root / "account.ini",
        )
        run_dir = client.parent
        metadata = {
            "schema": game_test.SCHEMA_VERSION,
            "run_id": run_id,
            "suite": game_test.MISSION_BEHAVIOR_SUITE,
            "status": "prepared",
            "created_utc": "offline",
            "source": {"offline": True},
            "template": str(self.root / "template"),
            "stage": str(self.root / "stage"),
            "plan": deepcopy(plan),
            "client": {
                "root": str(client.resolve()),
                "entry": str(game.resolve()),
                "game": str(game.resolve()),
                "host_key": game_test.HOST_KEY,
                "events": str((client / "sdgo/gametest/events.jsonl").resolve()),
                "stdout": str((run_dir / "stdout.log").resolve()),
                "stderr": str((run_dir / "stderr.log").resolve()),
                "immutable_hashes": {
                    game_test.GAME_EXE: game_test.sha256(game),
                    "sdgo/gametest/run.ini": game_test.sha256(
                        client / "sdgo/gametest/run.ini"
                    ),
                },
                "initial_inputs": {},
            },
            "execution": None,
        }
        game_test.atomic_write(
            run_dir / "run.json",
            json.dumps(metadata, indent=2, sort_keys=True),
        )
        self.assertEqual(
            game_test.validate_metadata(run_dir)["suite"],
            game_test.MISSION_BEHAVIOR_SUITE,
        )
        for field, value in (
            ("mission_behavior_profile", "gco"),
            ("mission_game_mode_id", 88009),
        ):
            changed = deepcopy(metadata)
            changed["plan"][field] = value
            game_test.atomic_write(
                run_dir / "run.json",
                json.dumps(changed, indent=2, sort_keys=True),
            )
            with self.assertRaises(game_test.GameTestError):
                game_test.validate_metadata(run_dir)
        game_test.atomic_write(
            run_dir / "run.json",
            json.dumps(metadata, indent=2, sort_keys=True),
        )

    def test_partial_unknown_or_wrong_mode_behavior_selection_is_refused(self):
        base = dict(
            target_host="127.0.0.1",
            login_port=5001,
            target_server="1",
            target_channel="1",
            mode_sequence="1",
            retry_count=1,
            expect_first_fail=False,
            expected_ai=3,
            launcher_path=None,
            mission_mode=5,
        )
        for patch in (
            {"mission_behavior_profile": "oc"},
            {"mission_game_mode_id": 88006},
            {
                "mission_behavior_profile": "other",
                "mission_game_mode_id": 88006,
            },
            {
                "mission_behavior_profile": "oc",
                "mission_game_mode_id": 1,
            },
        ):
            with self.subTest(patch=patch):
                with self.assertRaises(game_test.GameTestError):
                    game_test.make_plan(**base, **patch)
        with self.assertRaises(game_test.GameTestError):
            game_test.validate_mission_behavior("oc", 88006, 8)

    def test_cli_preserves_the_sealed_profile_and_exact_game_mode_id(self):
        parsed = game_test.parse_args([
            "prepare",
            "--client-template", str(self.root / "client"),
            "--stage", str(self.root / "stage"),
            "--run-dir", str(self.root / "run"),
            "--mission-mode", "5",
            "--mission-behavior-profile", "oc",
            "--mission-game-mode-id", "88006",
        ])
        self.assertEqual(parsed.mission_mode, 5)
        self.assertEqual(parsed.mission_behavior_profile, "oc")
        self.assertEqual(parsed.mission_game_mode_id, 88006)

    def test_client_configuration_uses_config_ini_and_preserves_manifest(self):
        client = self.root / "client"
        mod = client / "sdgo/mods/mission_behavior"
        mod.mkdir(parents=True)
        manifest = (
            "[mod]\n"
            "id=mission_behavior\n"
            "config_files=config.ini,profile.bin\n"
        )
        (mod / "mod.ini").write_text(manifest, encoding="utf-8")
        (mod / "config.ini").write_text(
            "[settings]\nProfile=gco\n",
            encoding="utf-8",
        )
        (mod / "profile.bin").write_bytes(b"sealed-owner-fixture")

        configured = game_test.configure_mission_behavior_client(client, "oc")

        self.assertEqual((mod / "mod.ini").read_text(encoding="utf-8"), manifest)
        self.assertEqual(
            game_test.read_ini(mod / "config.ini").get("settings", "Profile"),
            "oc",
        )
        self.assertEqual(configured["profile_data_present"], 1)
        self.assertEqual(
            configured["profile_data_sha256"],
            game_test.sha256(mod / "profile.bin"),
        )

    def test_oc_requires_owner_profile_but_gco_can_retain_current_behavior(self):
        client = self.root / "client"
        mod = client / "sdgo/mods/mission_behavior"
        mod.mkdir(parents=True)
        (mod / "mod.ini").write_text(
            "[mod]\nid=mission_behavior\n",
            encoding="utf-8",
        )
        with self.assertRaises(game_test.GameTestError):
            game_test.configure_mission_behavior_client(client, "oc")
        configured = game_test.configure_mission_behavior_client(client, "gco")
        self.assertEqual(configured["profile_data_present"], 0)
        self.assertEqual(
            game_test.read_ini(mod / "config.ini").get("settings", "Profile"),
            "gco",
        )

    def test_bounded_smoke_keeps_its_existing_scope_and_cases(self):
        plan = game_test.make_plan(
            "127.0.0.1",
            5001,
            "1",
            "1",
            "1",
            1,
            False,
            3,
            None,
            mission_mode=5,
        )
        self.assertEqual(plan["mission_scope"], game_test.MISSION_SCOPE)
        self.assertNotIn("mission_behavior_profile", plan)
        self.assertEqual(
            game_test.required_cases_for_plan(plan)[-4:],
            game_test.MISSION_FLOW_CASES,
        )


if __name__ == "__main__":
    unittest.main()
