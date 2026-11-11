from copy import deepcopy
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "game_test_mission_plan_contract",
    ROOT / "tools" / "game_test.py",
)
game_test = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(game_test)


class MissionPlanContractTests(unittest.TestCase):
    """Offline contract tests only: no game, server, database, or desktop launch."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.sequence = 0

    def tearDown(self):
        self.temporary.cleanup()

    def plan(
        self,
        mode=5,
        play_unit_uid=None,
        target_host="127.0.0.1",
        respawns=None,
        game_mode_id=None,
        mission_ai_count=game_test.MISSION_AI_COUNT,
    ):
        plan = game_test.make_plan(
            target_host,
            5001,
            "1",
            "1",
            "1",
            1,
            False,
            3,
            None,
            play_unit_uid=play_unit_uid,
            mission_mode=mode,
            mission_respawns=respawns,
            mission_game_mode_id=game_mode_id,
            mission_ai_count=mission_ai_count,
        )
        plan["network_policy"] = "direct"
        plan["quiet"] = True
        return plan

    def profile(self, state="existing_account"):
        profile = game_test.ini_parser()
        if state == "registered":
            profile["account"] = {
                "username": "Pilot9229A",
                "password": "Pass9229Test",
                "password_format": "plain",
            }
        else:
            profile["account"] = {
                "username": "MissionTest",
                "password": "offline-password",
                "password_format": "plain",
            }
        profile["registration"] = {"state": state}
        return profile

    def capsule(
        self,
        mode=5,
        play_unit_uid=None,
        *,
        route=False,
        account_state="existing_account",
        respawns=None,
        game_mode_id=None,
        mission_ai_count=game_test.MISSION_AI_COUNT,
    ):
        self.sequence += 1
        run_dir = self.base / f"run-{self.sequence}"
        client = run_dir / "client"
        client.mkdir(parents=True)
        game = client / game_test.GAME_EXE
        game.write_bytes(b"offline mission plan fixture")
        run_id = str(uuid.uuid4())
        plan = self.plan(
            mode,
            play_unit_uid,
            respawns=respawns,
            game_mode_id=game_mode_id,
            mission_ai_count=mission_ai_count,
        )
        if route:
            plan.update({
                "target_host": "127.90.0.1",
                "login_port": 5001,
                "network_policy": game_test.ROUTED_LOOPBACK_POLICY,
                "route_profile_path": game_test.ROUTE_PROFILE_RELATIVE.as_posix(),
                "route_profile_sha256": "a" * 64,
                "route_profile_route_count": 1,
                "logical_login_ipv4": "127.90.0.1",
                "logical_login_port": 5001,
                "physical_login_ipv4": "127.90.0.2",
                "physical_login_port": 6001,
            })
        profile_path = self.base / "persistent-account.ini"
        game_test.write_job(
            client,
            run_id,
            plan,
            self.profile(account_state),
            profile_path,
        )
        run_ini = client / "sdgo" / "gametest" / "run.ini"
        metadata = {
            "schema": game_test.SCHEMA_VERSION,
            "run_id": run_id,
            "suite": game_test.MISSION_SUITE,
            "status": "prepared",
            "created_utc": "offline",
            "source": {"offline": True},
            "template": str(self.base / "template"),
            "stage": str(self.base / "stage"),
            "plan": deepcopy(plan),
            "client": {
                "root": str(client.resolve()),
                "entry": str(game.resolve()),
                "game": str(game.resolve()),
                "host_key": game_test.HOST_KEY,
                "events": str(
                    (client / "sdgo" / "gametest" / "events.jsonl").resolve()
                ),
                "stdout": str((run_dir / "receipts" / "stdout.log").resolve()),
                "stderr": str((run_dir / "receipts" / "stderr.log").resolve()),
                "immutable_hashes": {
                    game_test.GAME_EXE: game_test.sha256(game),
                    "sdgo/gametest/run.ini": game_test.sha256(run_ini),
                },
                "initial_inputs": {},
            },
            "execution": None,
        }
        game_test.atomic_write(
            run_dir / "run.json",
            json.dumps(metadata, indent=2, sort_keys=True),
        )
        return run_dir

    def metadata(self, run_dir):
        return game_test.read_json(run_dir / "run.json")

    def test_respawn_policy_roundtrips_in_the_sealed_mission_scope(self):
        for policy in ('player','infinite'):
            run=self.capsule(respawns=policy)
            metadata=game_test.validate_metadata(run)
            self.assertEqual(metadata['plan']['mission_respawns'],policy)
            self.assertEqual(game_test.required_cases_for_plan(metadata['plan'])[-1],'mission.respawn')
            self.assertEqual(metadata['plan']['room_title'].endswith(' inf'),policy=='infinite')

    def test_respawn_policy_cannot_be_changed_independently_of_the_room_title(self):
        run=self.capsule(respawns='player')
        self.mutate_run_ini(run,'mission_respawns','infinite')
        with self.assertRaises(game_test.GameTestError):game_test.validate_metadata(run)

    def test_imported_mission_follows_the_flow_to_its_natural_end(self):
        plan = game_test.make_plan('127.0.0.1', 5001, '1', '1', '1', 1, False, 3, None,
                                   mission_mode=5, mission_game_mode_id=88072)
        self.assertEqual(plan['mission_game_mode_id'], 88072)
        self.assertEqual(game_test.required_cases_for_plan(plan),
                         game_test.FLOW_CASES[:3] + game_test.MISSION_FLOW_CASES + ('mission.import_flow',))
        with self.assertRaises(game_test.GameTestError):  # a GameModeID above the mode-5 range is a mode-8 mission
            game_test.make_plan('127.0.0.1', 5001, '1', '1', '1', 1, False, 3, None,
                                mission_mode=5, mission_game_mode_id=0x313991)


    def test_zero_ai_is_sealed_only_for_an_exact_ordinary_imported_mission(self):
        plan = game_test.make_plan(
            "127.0.0.1", 5001, "1", "1", "1", 1, False, 3, None,
            mission_mode=5,
            mission_game_mode_id=88072,
            mission_ai_count=0,
        )
        self.assertEqual(plan["mission_ai_count"], 0)
        self.assertEqual(plan["room_title"], f"{plan['title_nonce']} imported mission lv2")
        self.assertGreater(len(plan["room_title"].encode('utf-8')), 20)
        self.assertLessEqual(len(plan["room_title"].encode('utf-8')), 512)
        self.assertNotIn("!ai", plan["room_title"])
        self.assertEqual(
            game_test.required_cases_for_plan(plan),
            game_test.FLOW_CASES[:3] + game_test.MISSION_FLOW_CASES + ("mission.import_flow",),
        )

        run_dir = self.capsule(game_mode_id=88072, mission_ai_count=0)
        metadata = game_test.validate_metadata(run_dir)
        self.assertEqual(metadata["plan"]["mission_ai_count"], 0)
        public = game_test.read_public_plan(run_dir / "client")
        self.assertEqual(public["mission_ai_count"], 0)
        self.assertEqual(public["room_title"], metadata["plan"]["room_title"])
        required = game_test.required_cases_for_plan(public)
        self.complete_execution(run_dir, required)
        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["layers"]["native"]["mission_ai_count"], 0)

    def test_zero_ai_title_cannot_fall_back_to_an_unbound_legacy_name(self):
        run_dir = self.capsule(game_mode_id=88072, mission_ai_count=0)
        plan = game_test.validate_metadata(run_dir)["plan"]
        other_nonce = 'gt0001' if plan['title_nonce'] == 'gt0000' else 'gt0000'
        for title in (
            f"{plan['title_nonce']} lv2",
            f"{other_nonce} imported mission lv2",
            f"{plan['title_nonce']} imported mission !ai0 lv2",
        ):
            with self.subTest(title=title):
                self.mutate_run_ini(run_dir, 'room_title', title)
                with self.assertRaises(game_test.GameTestError):
                    game_test.read_public_plan(run_dir / 'client')

    def test_zero_ai_rejects_every_non_imported_or_mutated_scope(self):
        invalid = (
            {"mission_mode": 5, "mission_ai_count": 0},
            {"mission_mode": 5, "mission_game_mode_id": 88072, "mission_ai_count": 0, "mission_respawns": "player"},
            {"mission_mode": 5, "mission_game_mode_id": 88006, "mission_ai_count": 0, "mission_behavior_profile": "oc"},
            {"mission_ai_count": 0},
            {"mission_mode": 5, "mission_game_mode_id": 88072, "mission_ai_count": 1},
            {"mission_mode": 5, "mission_game_mode_id": 88072, "mission_ai_count": True},
        )
        for options in invalid:
            with self.subTest(options=options), self.assertRaises(game_test.GameTestError):
                game_test.make_plan(
                    "127.0.0.1", 5001, "1", "1", "1", 1, False, 3, None,
                    **options,
                )

        parsed = game_test.parse_args([
            "prepare", "--client-template", str(self.base), "--stage", str(self.base),
            "--run-dir", str(self.base / "parsed"), "--mission-mode", "5",
            "--mission-game-mode-id", "88072", "--mission-ai-count", "0",
        ])
        self.assertEqual(parsed.mission_ai_count, 0)
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                game_test.parse_args([
                    "prepare", "--client-template", str(self.base), "--stage", str(self.base),
                    "--run-dir", str(self.base / "invalid"), "--mission-ai-count", "2",
                ])

    def test_respawn_probe_requires_a_mission(self):
        with self.assertRaises(game_test.GameTestError):
            game_test.make_plan('127.0.0.1',5001,'1','1','1',1,False,3,None,mission_respawns='infinite')

    def write_metadata(self, run_dir, metadata):
        game_test.atomic_write(
            run_dir / "run.json",
            json.dumps(metadata, indent=2, sort_keys=True),
        )

    def mutate_run_ini(self, run_dir, field, value):
        path = run_dir / "client" / "sdgo" / "gametest" / "run.ini"
        parser = game_test.read_ini(path)
        if value is None:
            del parser["run"][field]
        else:
            parser["run"][field] = value
        game_test.write_ini(path, parser)

    def native_event(self, metadata, event, case="", status="running", pid=7701):
        return {
            "run_id": metadata["run_id"],
            "role": "client",
            "event": event,
            "case": case,
            "status": status,
            "message": "offline contract evidence",
            "host_key": game_test.HOST_KEY,
            "pid": pid,
            "image_path": metadata["client"]["game"],
            "duration_ms": 1,
        }

    def complete_execution(self, run_dir, cases):
        metadata = self.metadata(run_dir)
        pid = 7701
        events = [self.native_event(metadata, "run_begin", pid=pid)]
        events.extend(
            self.native_event(metadata, "case_end", case, "pass", pid)
            for case in cases
        )
        events.append(self.native_event(metadata, "run_end", status="pass", pid=pid))
        Path(metadata["client"]["events"]).write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )
        metadata["execution"] = {
            "started_utc": "offline-start",
            "finished_utc": "offline-finish",
            "entry_pid": pid,
            "game_pid": pid,
            "game_image_path": metadata["client"]["game"],
            "status": "completed",
            "timed_out": False,
            "error": None,
            "network_verified": {
                "verified": True,
                "pid": pid,
                "remote_ipv4": metadata["plan"]["target_host"],
                "remote_port": metadata["plan"]["login_port"],
            },
            "quiet_verified": {
                "verified": True,
                "foreground_safe": True,
                "pid": pid,
                "audio_muted": True,
                "firewall_verified": True,
            },
            "quiet_cleanup": {
                "status": "closed",
                "cleanup": {"status": "removed"},
            },
        }
        self.write_metadata(run_dir, metadata)

    def test_modes_seal_real_mission_ai_without_pvp_plan_fields(self):
        common = game_test.FLOW_CASES[:3]
        for mode in game_test.SUPPORTED_MISSION_MODES:
            with self.subTest(mode=mode):
                run_dir = self.capsule(mode)
                metadata = game_test.validate_metadata(run_dir)
                plan = metadata["plan"]
                run = game_test.read_ini(
                    run_dir / "client" / "sdgo" / "gametest" / "run.ini"
                )["run"]
                self.assertEqual(metadata["suite"], "native-mission-smoke")
                self.assertEqual(run["suite"], "pvp-ai")
                self.assertEqual(plan["mission_mode"], mode)
                self.assertEqual(plan["mission_scope"], "bounded_native_smoke")
                self.assertEqual(plan["mission_ai_count"], 3)
                self.assertEqual(plan["mission_ai_level"], 2)
                self.assertEqual(
                    plan["room_title"],
                    f"{plan['title_nonce']} !ai3 lv2",
                )
                self.assertLessEqual(len(plan["room_title"].encode("ascii")), 20)
                self.assertNotIn("mode_sequence", plan)
                self.assertNotIn("expected_ai", plan)
                self.assertNotIn("mode_sequence", run)
                self.assertNotIn("expected_ai", run)
                self.assertEqual(
                    game_test.required_cases_for_plan(plan),
                    common + game_test.MISSION_FLOW_CASES,
                )
                self.assertFalse(plan["mission_clear_claim"])
                self.assertFalse(plan["mission_result_claim"])
                self.assertFalse(plan["mission_active_exit_claim"])

    def test_owned_unit_is_normal_setup_before_mission_cases(self):
        uid = 2**63 + 17
        run_dir = self.capsule(8, uid)
        metadata = game_test.validate_metadata(run_dir)
        self.assertEqual(metadata["plan"]["play_unit_uid"], uid)
        self.assertEqual(
            game_test.required_cases_for_plan(metadata["plan"]),
            game_test.FLOW_CASES[:3]
            + ("setup.active_unit",)
            + game_test.MISSION_FLOW_CASES,
        )

    def test_cli_and_function_scopes_reject_pvp_pages_and_preparation_mix(self):
        parsed = game_test.parse_args([
            "prepare",
            "--client-template", "client",
            "--stage", "stage",
            "--run-dir", "run",
            "--mission-mode", "5",
            "--play-unit-uid", "99",
        ])
        self.assertEqual(parsed.mission_mode, 5)
        self.assertEqual(parsed.mode_sequence, "1")
        self.assertEqual(parsed.play_unit_uid, 99)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            game_test.parse_args([
                "prepare",
                "--client-template", "client",
                "--stage", "stage",
                "--run-dir", "run",
                "--mission-mode", "5",
                "--mode-sequence", "1",
            ])
        for keyword in (
            "bootstrap_only",
            "battleship_only",
            "gift_requests_only",
            "deck_only",
            "pages_only",
            "collection_draw",
        ):
            arguments = {keyword: True, "mission_mode": 5}
            with self.subTest(scope=keyword), self.assertRaises(game_test.GameTestError):
                game_test.required_flow_cases("1", **arguments)
        with self.assertRaises(game_test.GameTestError):
            game_test.make_plan(
                "127.0.0.1", 5001, "1", "1", "3", 1, False, 3, None,
                mission_mode=5,
            )
        with self.assertRaises(game_test.GameTestError):
            game_test.make_plan(
                "127.0.0.1", 5001, "1", "1", "1", 1, False, 4, None,
                mission_mode=5,
            )
        with self.assertRaises(game_test.GameTestError):
            game_test.required_cases_for_plan({
                **self.plan(),
                "mode_sequence": "1",
            })

    def test_pvp_parser_does_not_expand_to_mission_modes(self):
        for value in ("5", "8", "10", "1,5", "8,1"):
            with self.subTest(value=value), self.assertRaises(game_test.GameTestError):
                game_test.parse_mode_sequence(value)
        self.assertIsNone(game_test.validate_mission_mode(None))
        for value in (True, False, 1, 3, 7, "5"):
            with self.subTest(value=value), self.assertRaises(game_test.GameTestError):
                game_test.validate_mission_mode(value)

    def test_run_ini_mission_contract_rejects_field_value_and_scope_tampering(self):
        mutations = (
            ("suite", "native-mission-smoke"),
            ("mission_mode", "1"),
            ("mission_mode", "05"),
            ("mission_scope", "full_mission"),
            ("mission_ai_count", "2"),
            ("mission_ai_level", "5"),
            ("mission_clear_claim", "1"),
            ("mission_result_claim", "1"),
            ("mission_active_exit_claim", "1"),
            ("room_title", "gt0000 !ai3 lv2"),
            ("required_cases", ",".join(game_test.FLOW_CASES)),
            ("retry_count", "2"),
            ("expect_first_fail", "1"),
            ("mode_sequence", "1"),
            ("expected_ai", "3"),
            ("pages_only", "0"),
            ("random_loadout", "0"),
            ("unexpected_scope", "0"),
            ("network_policy", None),
            ("quiet", None),
            ("mission_mode", None),
            ("mission_ai_count", None),
        )
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                run_dir = self.capsule()
                self.mutate_run_ini(run_dir, field, value)
                with self.assertRaises(game_test.GameTestError):
                    game_test.read_public_plan(run_dir / "client")

    def test_metadata_plan_requires_exact_keys_values_and_json_types(self):
        mutations = (
            lambda plan: plan.__setitem__("mission_mode", True),
            lambda plan: plan.__setitem__("mission_ai_count", True),
            lambda plan: plan.__setitem__("mission_ai_level", 2.0),
            lambda plan: plan.__setitem__("mission_clear_claim", 0),
            lambda plan: plan.__setitem__("mode_sequence", "1"),
            lambda plan: plan.__setitem__("expected_ai", 3),
            lambda plan: plan.__setitem__("unexpected_scope", False),
            lambda plan: plan.pop("mission_scope"),
            lambda plan: plan.pop("room_title"),
        )
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                run_dir = self.capsule()
                metadata = self.metadata(run_dir)
                mutate(metadata["plan"])
                self.write_metadata(run_dir, metadata)
                with self.assertRaises(game_test.GameTestError):
                    game_test.validate_metadata(run_dir)

        run_dir = self.capsule()
        metadata = self.metadata(run_dir)
        metadata["suite"] = "pvp-ai-normal-flow"
        self.write_metadata(run_dir, metadata)
        with self.assertRaises(game_test.GameTestError):
            game_test.validate_metadata(run_dir)

    def test_registered_and_imported_accounts_remain_distinct_valid_states(self):
        for state in ("registered", "existing_account"):
            with self.subTest(state=state):
                run_dir = self.capsule(account_state=state)
                game_test.validate_metadata(run_dir)
                account = game_test.read_ini(
                    run_dir
                    / "client"
                    / "sdgo"
                    / "gametest"
                    / "private"
                    / "account.ini"
                )
                self.assertEqual(account["registration"]["state"], state)

        for state in ("pending_first_login", "auth_accepted", "unknown", ""):
            with self.subTest(rejected_state=state):
                run_dir = self.capsule(account_state=state)
                with self.assertRaises(game_test.GameTestError):
                    game_test.validate_metadata(run_dir)

    def test_prepare_reuses_registered_profile_without_relabeling_or_retry(self):
        fixture_class = importlib.import_module(
            "tests.test_game_test"
        ).GameTestRunnerTests
        fixture = fixture_class("runTest")
        fixture.setUp()
        try:
            fixture.account_profile.write_text(
                """[account]
username=Pilot9229A
password=Pass9229Test

[registration]
state=registered
""",
                encoding="utf-8",
            )
            original = fixture.account_profile.read_bytes()
            run_dir = fixture.prepare(
                "mission-registered",
                mission_mode=5,
            )
            metadata = fixture.metadata(run_dir)
            copied = fixture.private_account(run_dir)
            self.assertEqual(metadata["suite"], game_test.MISSION_SUITE)
            self.assertEqual(metadata["plan"]["retry_count"], 1)
            self.assertFalse(metadata["plan"]["expect_first_fail"])
            self.assertEqual(copied["registration"]["state"], "registered")
            self.assertEqual(fixture.account_profile.read_bytes(), original)
        finally:
            fixture.tearDown()
            fixture.doCleanups()

    def test_private_existing_account_states_require_single_attempt(self):
        for state in ("registered", "existing_account"):
            for field, value in (
                ("retry_count", "2"),
                ("expect_first_fail", "1"),
            ):
                with self.subTest(state=state, field=field):
                    run_dir = self.capsule(account_state=state)
                    account_path = (
                        run_dir
                        / "client"
                        / "sdgo"
                        / "gametest"
                        / "private"
                        / "account.ini"
                    )
                    account = game_test.read_ini(account_path)
                    account["registration"][field] = value
                    game_test.write_ini(account_path, account)
                    with self.assertRaises(game_test.GameTestError):
                        game_test.validate_metadata(run_dir)

        run_dir = self.capsule()
        account_path = (
            run_dir
            / "client"
            / "sdgo"
            / "gametest"
            / "private"
            / "account.ini"
        )
        account = game_test.read_ini(account_path)
        account["registration"]["state"] = "pending_first_login"
        game_test.write_ini(account_path, account)
        with self.assertRaises(game_test.GameTestError):
            game_test.validate_metadata(run_dir)

    def test_routed_loopback_contract_survives_mission_plan_round_trip(self):
        run_dir = self.capsule(8, route=True)
        plan = game_test.read_public_plan(run_dir / "client")
        self.assertEqual(plan["mission_mode"], 8)
        self.assertEqual(plan["network_policy"], "routed_loopback")
        self.assertEqual(plan["logical_login_ipv4"], "127.90.0.1")
        self.assertEqual(plan["physical_login_ipv4"], "127.90.0.2")
        self.assertEqual(plan["room_title"], f"{plan['title_nonce']} !ai3 lv2")

    def test_report_can_pass_only_the_bounded_mission_case_set(self):
        complete = self.capsule(5)
        complete_plan = self.metadata(complete)["plan"]
        required = game_test.required_cases_for_plan(complete_plan)
        self.complete_execution(complete, required)
        report = game_test.build_report(complete)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["suite"], "native-mission-smoke")
        self.assertEqual(report["layers"]["native"]["required_cases"], list(required))
        self.assertEqual(report["layers"]["native"]["mission_mode"], 5)
        self.assertEqual(
            report["layers"]["native"]["mission_claims"],
            {"clear": False, "result": False, "active_exit": False},
        )
        self.assertNotIn("mission.clear", required)
        self.assertNotIn("mission.result", required)
        self.assertNotIn("mission.exit", required)
        self.assertNotIn("pvp.return", required)

        missing_combat = self.capsule(5)
        missing_required = game_test.required_cases_for_plan(
            self.metadata(missing_combat)["plan"]
        )
        self.complete_execution(missing_combat, missing_required[:-1])
        missing_report = game_test.build_report(missing_combat)
        self.assertEqual(missing_report["status"], "fail")
        self.assertIn(
            "mission.combat",
            " ".join(missing_report["layers"]["native"]["pending"]),
        )

        pvp_substitute = self.capsule(8)
        pvp_cases = game_test.FLOW_CASES[:3] + game_test.FLOW_CASES[3:]
        self.complete_execution(pvp_substitute, pvp_cases)
        pvp_report = game_test.build_report(pvp_substitute)
        self.assertEqual(pvp_report["status"], "fail")
        self.assertIn(
            "mission.room",
            " ".join(pvp_report["layers"]["native"]["pending"]),
        )


if __name__ == "__main__":
    unittest.main()
