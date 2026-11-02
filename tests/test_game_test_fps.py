from pathlib import Path
import importlib.util
import io
import json
import os
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("game_test_fps_module", ROOT / "tools/game_test.py")
game_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(game_test)


TEST_MANIFEST = """[mod]
id=game_test_client
category=development
feature=game-test
name=fixture observer
version=1
api=1
dll=game_test_client.dll
enabled=0
hosts=GOnline-54BE1568-00C97000
requires=game.app,test.client_update
"""

FPS_MANIFEST = """[mod]
id=fps_limit
category=gameplay
feature=fps-limit
name=fixture fps limit
version=1
api=1
dll=fps_limit.dll
enabled=1
hosts=GOnline-54BE1568-00C97000
requires=fps.config.force60
"""

OTHER_MANIFEST = """[mod]
id=other_mod
category=gameplay
feature=other
name=preserved
version=1
api=1
dll=other_mod.dll
enabled=1
hosts=*
requires=legacy.unresolved.symbol
"""


class FakeProcess:
    pid = 42001

    def __init__(self):
        self.returncode = None

    def poll(self):
        return self.returncode

    def kill(self):
        self.returncode = -9

    def wait(self, timeout=None):
        return self.returncode


class FakeQuietRuntime:
    def __init__(self, metadata, receipt_dir):
        self.metadata = metadata

    def start(self):
        game_test.validate_muted_audio(Path(self.metadata["client"]["root"]))

    def verify(self, pid):
        return {
            "verified": True,
            "foreground_safe": True,
            "pid": pid,
            "audio_muted": True,
            "firewall_verified": True,
        }

    def close(self):
        return {"status": "closed", "cleanup": {"status": "removed"}}


class NativeFpsGameTestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.template = self.root / "client-template"
        self.stage = self.root / "stage"
        self.template.mkdir()
        self._make_template()
        self._make_stage()
        patch = mock.patch.object(
            game_test,
            "git_source_ref",
            return_value={"commit": "a" * 40, "dirty": False},
        )
        patch.start()
        self.addCleanup(patch.stop)

    def tearDown(self):
        self.temp.cleanup()

    def _make_template(self):
        (self.template / "GOnline.exe").write_bytes(b"game")
        (self.template / "d3d9.dll").write_bytes(b"loader")
        (self.template / "GPackData.zpk").write_bytes(b"assets")
        config = (
            '<Config><Sound><BGMVolume Value="22" use="true"/>'
            '<EffectVolume Value="33" use="true"/><VoiceVolume Value="44" use="true"/></Sound>'
            '<Framework><Timer VerticalSync="true" RenderSync="true" resolution="high" updatefps="60"/>'
            '</Framework><LoginData source="sentinel"><CheckLoginIdSave use="true" id="REAL_USER"/>'
            '<LastJoinServer><Mission server="2" channel="1"/></LastJoinServer>'
            '<CheckAutoLogin use="true" id="REAL_USER" pass="REAL_PASSWORD"/>'
            '<UnknownCredential token="DO_NOT_COPY"/></LoginData></Config>'
        )
        (self.template / "config.xml").write_text(config, encoding="utf-8")
        sdgo = self.template / "sdgo"
        for identifier, manifest, payload in (
            ("fps_limit", FPS_MANIFEST, b"installed-fps"),
            ("other_mod", OTHER_MANIFEST, b"preserved-other"),
        ):
            directory = sdgo / "mods" / identifier
            directory.mkdir(parents=True)
            (directory / "mod.ini").write_text(manifest, encoding="utf-8")
            (directory / f"{identifier}.dll").write_bytes(payload)
        (sdgo / "symbols").mkdir()
        (sdgo / "symbols" / f"{game_test.HOST_KEY}.ini").write_text(
            "[host]\nimage=GOnline.exe\n[game.app]\nkind=data\nrva=old\n"
            "[test.client_update]\nkind=code\nrva=old\n"
            "[fps.config.force60]\nkind=code\nrva=old\n",
            encoding="utf-8",
        )
        (sdgo / "core.dll").write_bytes(b"old-core")
        (sdgo / "sdgo.ini").write_text(
            "[core]\nlog_level=1\n[categories]\ngameplay=0\ndevelopment=0\n"
            "[features]\nfps-limit=1\ngame-test=0\nother=1\n"
            "[mods]\nfps_limit=1\ngame_test_client=0\nother_mod=1\n",
            encoding="utf-8",
        )

    def _make_stage(self):
        client = self.stage / "client"
        observer = client / "sdgo" / "mods" / "game_test_client"
        fps = client / "sdgo" / "mods" / "fps_limit"
        observer.mkdir(parents=True)
        fps.mkdir(parents=True)
        (client / "d3d9.dll").write_bytes(b"stage-loader")
        (client / "sdgo" / "core.dll").write_bytes(b"stage-core")
        (client / "sdgo" / "sdgo.ini").write_text("[core]\nlog_level=0\n", encoding="utf-8")
        (observer / "game_test_client.dll").write_bytes(b"observer")
        (observer / "mod.ini").write_text(TEST_MANIFEST, encoding="utf-8")
        (fps / "fps_limit.dll").write_bytes(b"staged-fps")
        (fps / "mod.ini").write_text(FPS_MANIFEST, encoding="utf-8")
        symbols = client / "sdgo" / "symbols"
        symbols.mkdir()
        (symbols / f"{game_test.HOST_KEY}.ini").write_text(
            "[host]\nimage=GOnline.exe\n[game.app]\nkind=data\nrva=new\n"
            "[test.client_update]\nkind=code\nrva=new\n"
            "[fps.config.force60]\nkind=code\nrva=new\n",
            encoding="utf-8",
        )

    def prepare(self, name="fps", **kwargs):
        run_dir = self.root / name
        with mock.patch.object(
            game_test,
            "default_account_profile",
            side_effect=AssertionError("FPS prepare read the default account path"),
        ), mock.patch.object(
            game_test,
            "load_account_profile",
            side_effect=AssertionError("FPS prepare read an account profile"),
        ):
            game_test.prepare(
                self.template,
                self.stage,
                run_dir,
                fps_cap=160,
                **kwargs,
            )
        return run_dir

    def test_prepare_roundtrip_scrubs_login_and_preserves_source_and_other_mods(self):
        original = (self.template / "config.xml").read_bytes()
        run_dir = self.prepare()
        metadata = game_test.validate_metadata(run_dir)
        self.assertEqual(metadata["suite"], game_test.FPS_SUITE)
        self.assertEqual(
            metadata["plan"],
            {
                "target_host": "127.0.0.1",
                "login_port": 5001,
                "network_policy": "direct",
                "quiet": True,
                "launcher_path": "",
                "fps_cap": 160,
                "fps_mod_enabled": True,
            },
        )
        self.assertEqual((self.template / "config.xml").read_bytes(), original)
        client = run_dir / "client"
        self.assertFalse((client / "sdgo/gametest/private/account.ini").exists())
        self.assertEqual((client / "sdgo/mods/other_mod/other_mod.dll").read_bytes(), b"preserved-other")
        settings = game_test.read_ini(client / "sdgo/sdgo.ini")
        self.assertEqual(settings["categories"]["gameplay"], "1")
        self.assertEqual(settings["mods"]["other_mod"], "0")
        document = ET.parse(client / "config.xml").getroot()
        timer = document.find("Framework/Timer")
        self.assertEqual(timer.get("updatefps"), "160")
        self.assertEqual(timer.get("VerticalSync"), "false")
        self.assertEqual(timer.get("RenderSync"), "false")
        login = document.find("LoginData")
        self.assertIsNotNone(login.find("LastJoinServer"))
        self.assertNotIn("REAL_USER", ET.tostring(login, encoding="unicode"))
        self.assertNotIn("REAL_PASSWORD", ET.tostring(login, encoding="unicode"))
        self.assertNotIn("DO_NOT_COPY", ET.tostring(login, encoding="unicode"))
        job = game_test.read_ini(client / "sdgo/gametest/run.ini")
        self.assertEqual(job["run"]["suite"], game_test.FPS_SUITE)
        self.assertEqual(job["run"]["required_cases"], ",".join(game_test.FPS_CASES))
        self.assertNotIn("account_path", job["run"])
        self.assertEqual(game_test.read_public_plan(client), metadata["plan"])

    def test_disabled_mod_is_bound_in_manifest_global_settings_and_plan(self):
        run_dir = self.prepare("disabled", fps_mod_enabled=False)
        metadata = game_test.validate_metadata(run_dir)
        self.assertFalse(metadata["plan"]["fps_mod_enabled"])
        client = run_dir / "client"
        manifest = game_test.read_ini(client / "sdgo/mods/fps_limit/mod.ini")
        settings = game_test.read_ini(client / "sdgo/sdgo.ini")
        self.assertEqual(manifest["mod"]["enabled"], "0")
        self.assertEqual(settings["features"]["fps-limit"], "0")
        self.assertEqual(settings["mods"]["fps_limit"], "0")

    def test_missing_or_partial_fps_mod_is_rejected(self):
        for directory in (
            self.template / "sdgo/mods/fps_limit",
            self.stage / "client/sdgo/mods/fps_limit",
        ):
            for path in directory.iterdir():
                path.unlink()
            directory.rmdir()
        with self.assertRaisesRegex(game_test.GameTestError, "requires installed fps_limit"):
            self.prepare("missing")

    def test_fps_symbol_remains_strict_while_unrelated_legacy_symbol_is_preserved(self):
        for path in (
            self.template / "sdgo/symbols" / f"{game_test.HOST_KEY}.ini",
            self.stage / "client/sdgo/symbols" / f"{game_test.HOST_KEY}.ini",
        ):
            symbols = game_test.read_ini(path)
            symbols.remove_section("fps.config.force60")
            game_test.write_ini(path, symbols)
        with self.assertRaisesRegex(game_test.GameTestError, "fps.config.force60"):
            self.prepare("missing-fps-symbol")

    def test_malformed_job_and_public_metadata_are_rejected(self):
        run_dir = self.prepare("malformed")
        job_path = run_dir / "client/sdgo/gametest/run.ini"
        job = game_test.read_ini(job_path)
        job["run"]["fps_cap"] = "0"
        game_test.write_ini(job_path, job)
        with self.assertRaisesRegex(game_test.GameTestError, "fps_cap must be between"):
            game_test.validate_metadata(run_dir)

        second = self.prepare("metadata")
        metadata = game_test.read_json(second / "run.json")
        metadata["plan"]["account_path"] = "private/account.ini"
        game_test.atomic_write(second / "run.json", json.dumps(metadata))
        with self.assertRaisesRegex(game_test.GameTestError, "adds fields"):
            game_test.validate_metadata(second)

    def test_run_skips_account_lock_and_direct_login_peer(self):
        run_dir = self.prepare("run")
        metadata = game_test.read_json(run_dir / "run.json")
        process = FakeProcess()
        handles = (io.BytesIO(), io.BytesIO())
        with mock.patch.object(game_test, "QuietRuntime", FakeQuietRuntime), \
             mock.patch.object(game_test, "NamedStartupEvent", mock.MagicMock()), \
             mock.patch.object(game_test, "spawn_entry", return_value=(process, *handles)), \
             mock.patch.object(
                 game_test,
                 "wait_for_owned_game_process",
                 return_value={"pid": process.pid, "image_path": metadata["client"]["game"]},
             ), \
             mock.patch.object(game_test, "has_run_end", return_value=True), \
             mock.patch.object(game_test, "process_image_path", return_value=metadata["client"]["game"]), \
             mock.patch.object(
                 game_test,
                 "acquire_account_lock",
                 side_effect=AssertionError("FPS run acquired an account lock"),
             ), \
             mock.patch.object(
                 game_test,
                 "verify_direct_peer",
                 side_effect=AssertionError("FPS run required a login peer"),
             ), \
             mock.patch.object(game_test, "build_report", return_value={"status": "pass"}):
            self.assertEqual(game_test.run_prepared(run_dir, timeout=1, launch_timeout=0)["status"], "pass")

    def test_spawn_environment_does_not_inherit_account_path(self):
        run_dir = self.prepare("environment")
        metadata = game_test.read_json(run_dir / "run.json")
        captured = {}

        def fake_popen(arguments, **kwargs):
            captured.update(kwargs["env"])
            return FakeProcess()

        receipts = run_dir / "receipts"
        receipts.mkdir()
        with mock.patch.dict(os.environ, {"SDGO_GAME_TEST_ACCOUNT": "real-account.ini"}), \
             mock.patch.object(game_test.subprocess, "Popen", side_effect=fake_popen):
            process, stdout, stderr = game_test.spawn_entry(metadata, receipts)
            stdout.close()
            stderr.close()
        self.assertEqual(process.pid, FakeProcess.pid)
        self.assertNotIn("SDGO_GAME_TEST_ACCOUNT", captured)

    def test_fps_events_reuse_report_and_junit_paths(self):
        run_dir = self.prepare("report")
        metadata = game_test.read_json(run_dir / "run.json")
        pid = 42001
        metadata["execution"] = {
            "status": "completed",
            "game_pid": pid,
            "game_image_path": metadata["client"]["game"],
            "quiet_verified": {
                "verified": True,
                "foreground_safe": True,
                "pid": pid,
                "audio_muted": True,
                "firewall_verified": True,
            },
            "quiet_cleanup": {"status": "closed", "cleanup": {"status": "removed"}},
        }
        game_test.atomic_write(run_dir / "run.json", json.dumps(metadata))
        events = [{
            "run_id": metadata["run_id"],
            "role": "client",
            "event": "run_begin",
            "case": "",
            "status": "running",
            "message": "",
            "host_key": game_test.HOST_KEY,
            "pid": pid,
            "image_path": metadata["client"]["game"],
            "duration_ms": 0,
        }]
        for case in game_test.FPS_CASES:
            events.append({
                **events[0],
                "event": "case_end",
                "case": case,
                "status": "pass",
                "duration_ms": 10,
            })
        events.append({
            **events[0],
            "event": "run_end",
            "status": "pass",
        })
        Path(metadata["client"]["events"]).write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )
        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["layers"]["native"]["required_cases"], list(game_test.FPS_CASES))
        self.assertTrue((run_dir / "receipts/report.json").is_file())
        junit = ET.parse(run_dir / "receipts/junit.xml").getroot().find("testsuite")
        self.assertEqual(junit.get("tests"), "3")
        self.assertEqual(
            [case.get("name") for case in junit.findall("testcase")],
            list(game_test.FPS_CASES),
        )


if __name__ == "__main__":
    unittest.main()
