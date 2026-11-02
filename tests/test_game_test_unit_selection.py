from pathlib import Path
import configparser
import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "game_test_unit_selection_runner", ROOT / "tools/game_test.py"
)
game_test = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(game_test)


OBSERVER_MANIFEST = """[mod]
id=game_test_client
category=development
feature=game-test
api=1
dll=game_test_client.dll
enabled=0
hosts=GOnline-54BE1568-00C97000
requires=game.app,test.client_init,test.client_update,test.client_set_state
"""

MATCH_MANIFEST = """[mod]
id=match_ai
category=gameplay
feature=pvp-ai
api=1
dll=match_ai.dll
enabled=1
hosts=GOnline-54BE1568-00C97000
requires=game.app,match.runtime
"""


class PlayUnitPlanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.template = self.root / "template"
        self.stage = self.root / "stage"
        self.account = self.root / "account.ini"
        self.make_template()
        self.make_stage()
        self.account.write_text(
            "[account]\nusername=UnitOwner1\npassword=UnitPass1234\n"
            "[registration]\nstate=auth_accepted\n",
            encoding="utf-8",
        )
        source_patch = mock.patch.object(
            game_test,
            "git_source_ref",
            return_value={"commit": "0" * 40, "dirty": False},
        )
        source_patch.start()
        self.addCleanup(source_patch.stop)

    def make_template(self):
        self.template.mkdir()
        (self.template / "GOnline.exe").write_bytes(b"game")
        (self.template / "d3d9.dll").write_bytes(b"loader")
        (self.template / "config.xml").write_text(
            '<Config><Sound><BGMVolume Value="30" use="true"/>'
            '<EffectVolume Value="30" use="true"/>'
            '<VoiceVolume Value="30" use="true"/></Sound></Config>',
            encoding="utf-8",
        )
        sdgo = self.template / "sdgo"
        target = sdgo / "mods" / "match_ai"
        target.mkdir(parents=True)
        (target / "mod.ini").write_text(MATCH_MANIFEST, encoding="utf-8")
        (target / "match_ai.dll").write_bytes(b"match")
        (sdgo / "symbols").mkdir()
        (sdgo / "symbols" / f"{game_test.HOST_KEY}.ini").write_text(
            "[host]\nimage=GOnline.exe\n"
            "[game.app]\nkind=data\nrva=old\n"
            "[match.runtime]\nkind=code\nrva=old\n",
            encoding="utf-8",
        )
        (sdgo / "core.dll").write_bytes(b"core")
        (sdgo / "sdgo.ini").write_text("[core]\nlog_level=1\n", encoding="utf-8")

    def make_stage(self):
        client = self.stage / "client"
        observer = client / "sdgo" / "mods" / "game_test_client"
        observer.mkdir(parents=True)
        (observer / "mod.ini").write_text(OBSERVER_MANIFEST, encoding="utf-8")
        (observer / "game_test_client.dll").write_bytes(b"observer")
        (client / "sdgo" / "core.dll").parent.mkdir(parents=True, exist_ok=True)
        (client / "sdgo" / "core.dll").write_bytes(b"new-core")
        symbols = client / "sdgo" / "symbols"
        symbols.mkdir()
        (symbols / f"{game_test.HOST_KEY}.ini").write_text(
            "[host]\nimage=GOnline.exe\n"
            "[game.app]\nkind=data\nrva=app\n"
            "[match.runtime]\nkind=code\nrva=match\n"
            "[test.client_init]\nkind=code\nrva=init\n"
            "[test.client_update]\nkind=code\nrva=update\n"
            "[test.client_set_state]\nkind=code\nrva=state\n",
            encoding="utf-8",
        )

    def prepare(self, name, **kwargs):
        run_dir = self.root / name
        game_test.prepare(
            self.template,
            self.stage,
            run_dir,
            account_profile=self.account,
            **kwargs,
        )
        return run_dir

    def job(self, run_dir):
        return game_test.read_ini(
            run_dir / "client" / "sdgo" / "gametest" / "run.ini"
        )

    def metadata(self, run_dir):
        return game_test.read_json(run_dir / "run.json")

    def test_default_plan_omits_selection_and_preserves_existing_case_sequence(self):
        run_dir = self.prepare("default")
        metadata = self.metadata(run_dir)
        job = self.job(run_dir)["run"]
        self.assertNotIn("play_unit_uid", metadata["plan"])
        self.assertNotIn("play_unit_uid", job)
        self.assertNotIn("setup.active_unit", job["required_cases"].split(","))
        self.assertEqual(
            tuple(job["required_cases"].split(",")),
            game_test.FLOW_CASES,
        )

    def test_pvp_selection_is_public_immutable_and_reported_after_onboarding(self):
        uid = 97175
        run_dir = self.prepare("pvp", play_unit_uid=uid)
        metadata = self.metadata(run_dir)
        job = self.job(run_dir)["run"]
        expected = game_test.FLOW_CASES[:3] + ("setup.active_unit",) + game_test.FLOW_CASES[3:]

        self.assertEqual(metadata["plan"]["play_unit_uid"], uid)
        self.assertEqual(job["play_unit_uid"], str(uid))
        self.assertEqual(tuple(job["required_cases"].split(",")), expected)
        self.assertEqual(game_test.validate_metadata(run_dir)["plan"], metadata["plan"])
        game_test.verify_hashes(metadata)
        self.assertEqual(
            metadata["client"]["immutable_hashes"]["sdgo/gametest/run.ini"],
            game_test.sha256(run_dir / "client" / "sdgo" / "gametest" / "run.ini"),
        )

        private = configparser.ConfigParser(interpolation=None)
        private.read(
            run_dir / "client" / "sdgo" / "gametest" / "private" / "account.ini",
            encoding="utf-8",
        )
        self.assertFalse(any("play_unit_uid" in private[section] for section in private.sections()))
        public = (run_dir / "run.json").read_text(encoding="utf-8") + (
            run_dir / "client" / "sdgo" / "gametest" / "run.ini"
        ).read_text(encoding="utf-8")
        self.assertNotIn("UnitPass1234", public)
        self.assertNotIn("UnitOwner1", public)

        report = game_test.build_report(run_dir)
        self.assertEqual(tuple(report["layers"]["native"]["required_cases"]), expected)
        self.assertEqual(report["layers"]["native"]["requested_play_unit_uid"], uid)

    def test_pages_selection_uses_the_same_native_setup_case(self):
        uid = 118323
        run_dir = self.prepare(
            "pages",
            pages_only=True,
            collection_draw=True,
            play_unit_uid=uid,
        )
        cases = tuple(self.job(run_dir)["run"]["required_cases"].split(","))
        self.assertEqual(cases[:4], game_test.FLOW_CASES[:3] + ("setup.active_unit",))
        self.assertIn("pages.hangar.read", cases)
        self.assertIn("pages.collection.delivery", cases)
        self.assertEqual(game_test.read_public_plan(run_dir / "client")["play_unit_uid"], uid)

    def test_selection_rejects_non_uint64_values_and_preparation_only_scenarios(self):
        invalid_values = (0, -1, 2**64, True, "97175")
        for index, value in enumerate(invalid_values):
            with self.subTest(value=value):
                with self.assertRaisesRegex(game_test.GameTestError, "non-zero uint64"):
                    self.prepare(f"bad-value-{index}", play_unit_uid=value)

        for flag in ("bootstrap_only", "battleship_only", "gift_requests_only", "deck_only"):
            with self.subTest(flag=flag):
                run_dir = self.root / f"bad-scope-{flag}"
                with self.assertRaisesRegex(game_test.GameTestError, "only for PvP and pages"):
                    game_test.prepare(
                        self.template,
                        self.stage,
                        run_dir,
                        account_profile=self.account,
                        play_unit_uid=97175,
                        **{flag: True},
                    )
                self.assertFalse(run_dir.exists())

    def test_selection_never_creates_or_retries_registration(self):
        missing = self.root / "new-account.ini"
        with self.assertRaisesRegex(game_test.GameTestError, "cannot create"):
            game_test.prepare(
                self.template,
                self.stage,
                self.root / "new-account-run",
                account_profile=missing,
                create_test_account=True,
                play_unit_uid=97175,
            )
        self.assertFalse(missing.exists())

        pending = self.root / "pending.ini"
        pending.write_text(
            "[account]\nusername=PendUnit01\npassword=PendPass1234\n"
            "[registration]\nstate=pending_first_login\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(game_test.GameTestError, "already registered"):
            game_test.prepare(
                self.template,
                self.stage,
                self.root / "pending-run",
                account_profile=pending,
                play_unit_uid=97175,
            )

    def test_metadata_rejects_selection_value_and_presence_tampering(self):
        changed = self.prepare("changed", play_unit_uid=97175)
        metadata = self.metadata(changed)
        metadata["plan"]["play_unit_uid"] = 118323
        game_test.atomic_write(changed / "run.json", json.dumps(metadata))
        with self.assertRaisesRegex(game_test.GameTestError, "disagrees"):
            game_test.validate_metadata(changed)

        removed_public = self.prepare("removed-public", play_unit_uid=97175)
        parser = self.job(removed_public)
        parser.remove_option("run", "play_unit_uid")
        parser["run"]["required_cases"] = ",".join(game_test.FLOW_CASES)
        game_test.write_ini(
            removed_public / "client" / "sdgo" / "gametest" / "run.ini",
            parser,
        )
        with self.assertRaisesRegex(game_test.GameTestError, "add or remove"):
            game_test.validate_metadata(removed_public)

        added_public = self.prepare("added-public")
        parser = self.job(added_public)
        parser["run"]["play_unit_uid"] = "97175"
        parser["run"]["required_cases"] = ",".join(
            game_test.FLOW_CASES[:3]
            + ("setup.active_unit",)
            + game_test.FLOW_CASES[3:]
        )
        game_test.write_ini(
            added_public / "client" / "sdgo" / "gametest" / "run.ini",
            parser,
        )
        with self.assertRaisesRegex(game_test.GameTestError, "add or remove"):
            game_test.validate_metadata(added_public)

    def test_public_plan_rejects_noncanonical_and_overflow_tampering(self):
        for index, value in enumerate(("0", "097175", "+97175", str(2**64))):
            with self.subTest(value=value):
                run_dir = self.prepare(f"public-{index}", play_unit_uid=97175)
                parser = self.job(run_dir)
                parser["run"]["play_unit_uid"] = value
                game_test.write_ini(
                    run_dir / "client" / "sdgo" / "gametest" / "run.ini",
                    parser,
                )
                with self.assertRaises(game_test.GameTestError):
                    game_test.read_public_plan(run_dir / "client")

    def test_cli_names_the_native_owned_uid_contract(self):
        arguments = game_test.parse_args(
            [
                "prepare",
                "--client-template",
                str(self.template),
                "--stage",
                str(self.stage),
                "--run-dir",
                str(self.root / "cli"),
                "--play-unit-uid",
                "97175",
            ]
        )
        self.assertEqual(arguments.play_unit_uid, 97175)

        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(SystemExit):
            game_test.parse_args(["prepare", "--help"])
        help_text = output.getvalue()
        self.assertIn("--play-unit-uid", help_text)
        self.assertIn("genuinely owned", help_text)


if __name__ == "__main__":
    unittest.main()
