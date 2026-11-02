from pathlib import Path
import importlib.util
import json
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("game_test_routes_runner", ROOT / "tools/game_test.py")
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

NET_ROUTE_MANIFEST = """[mod]
id=net_route
category=network
feature=frp-distribution
api=1
dll=net_route.dll
enabled=0
hosts=GOnline-54BE1568-00C97000
"""


def valid_profile() -> str:
    return """[settings]
LoginIPv4=127.90.0.1
LoginPort=5001
RouteCount=3
Trace=0
Route1=tcp,127.90.0.1,5001,127.91.0.1,27100
Route2=udp,127.90.0.1,26101,127.91.0.1,27101
Route3=udp,127.90.0.1,26102,127.91.0.1,27102
"""


class RoutedLoopbackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.template = self.root / "template"
        self.stage = self.root / "stage"
        self.account = self.root / "account.ini"
        self.profile = self.root / "route.ini"
        self.make_template()
        self.make_stage()
        self.account.write_text(
            "[account]\nusername=RouteTest1\npassword=RoutePass123\n"
            "[registration]\nstate=auth_accepted\n",
            encoding="utf-8",
        )
        self.profile.write_text(valid_profile(), encoding="utf-8", newline="")
        patch = mock.patch.object(
            game_test, "git_source_ref", return_value={"commit": "0" * 40, "dirty": False}
        )
        patch.start()
        self.addCleanup(patch.stop)

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
        (sdgo / "mods" / "match_ai").mkdir(parents=True)
        (sdgo / "mods" / "match_ai" / "mod.ini").write_text(MATCH_MANIFEST)
        (sdgo / "mods" / "match_ai" / "match_ai.dll").write_bytes(b"match")
        route = sdgo / "mods" / "net_route"
        route.mkdir()
        (route / "mod.ini").write_text(NET_ROUTE_MANIFEST)
        (route / "net_route.dll").write_bytes(b"route")
        (route / "config.ini").write_bytes(b"[settings]\nPrivateEndpoint=preserve-source\n")
        (sdgo / "symbols").mkdir()
        (sdgo / "symbols" / f"{game_test.HOST_KEY}.ini").write_text(
            "[host]\nimage=GOnline.exe\n[game.app]\nkind=data\nrva=old\n"
            "[match.runtime]\nkind=code\nrva=old\n",
            encoding="utf-8",
        )
        (sdgo / "core.dll").write_bytes(b"core")
        (sdgo / "sdgo.ini").write_text(
            "[core]\nlog_level=1\n[mods]\nnet_route=0\nnet_probe=1\n",
            encoding="utf-8",
        )

    def make_stage(self):
        client = self.stage / "client"
        observer = client / "sdgo" / "mods" / "game_test_client"
        observer.mkdir(parents=True)
        (observer / "mod.ini").write_text(OBSERVER_MANIFEST)
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

    def prepare(self, name="run"):
        run_dir = self.root / name
        game_test.prepare(
            self.template,
            self.stage,
            run_dir,
            account_profile=self.account,
            target_host="127.90.0.1",
            login_port=5001,
            network_policy=game_test.ROUTED_LOOPBACK_POLICY,
            route_profile=self.profile,
        )
        return run_dir

    def test_prepare_installs_exact_profile_only_in_copy_and_seals_endpoints(self):
        source_manifest = (self.template / "sdgo/mods/net_route/mod.ini").read_bytes()
        source_config = (self.template / "sdgo/mods/net_route/config.ini").read_bytes()
        profile_bytes = self.profile.read_bytes()
        run_dir = self.prepare()
        metadata = game_test.read_json(run_dir / "run.json")
        plan = metadata["plan"]
        copied = run_dir / "client"
        job = game_test.read_ini(copied / "sdgo/gametest/run.ini")["run"]

        self.assertEqual((self.template / "sdgo/mods/net_route/mod.ini").read_bytes(), source_manifest)
        self.assertEqual((self.template / "sdgo/mods/net_route/config.ini").read_bytes(), source_config)
        self.assertEqual((copied / game_test.ROUTE_PROFILE_RELATIVE).read_bytes(), profile_bytes)
        self.assertEqual(
            game_test.read_ini(copied / "sdgo/mods/net_route/mod.ini")["mod"]["enabled"], "1"
        )
        switches = game_test.read_ini(copied / "sdgo/sdgo.ini")
        self.assertEqual(switches["categories"]["network"], "1")
        self.assertEqual(switches["features"]["frp-distribution"], "1")
        self.assertEqual(switches["mods"]["net_route"], "1")
        self.assertEqual(switches["mods"]["net_probe"], "0")
        self.assertEqual(plan["target_host"], "127.90.0.1")
        self.assertEqual(plan["login_port"], 5001)
        self.assertEqual(plan["physical_login_ipv4"], "127.91.0.1")
        self.assertEqual(plan["physical_login_port"], 27100)
        self.assertEqual(plan["route_profile_sha256"], game_test.sha256(self.profile))
        self.assertEqual(job["endpoint"], "127.90.0.1:5001")
        self.assertEqual(job["physical_login_ipv4"], "127.91.0.1")
        self.assertEqual(job["physical_login_port"], "27100")
        self.assertEqual(job["route_profile_sha256"], plan["route_profile_sha256"])
        self.assertEqual(job["quiet_loopback"], "1")
        self.assertEqual(game_test.validate_metadata(run_dir)["plan"], plan)
        self.assertIn(game_test.ROUTE_PROFILE_RELATIVE.as_posix(), metadata["client"]["immutable_hashes"])

    def test_os_peer_proof_uses_physical_route_and_keeps_logical_evidence(self):
        run_dir = self.prepare("peer")
        metadata = game_test.read_json(run_dir / "run.json")
        logical_only = [{"remote_ipv4": "127.90.0.1", "remote_port": 5001}]
        with mock.patch.object(game_test, "owned_tcp_peers", return_value=logical_only):
            self.assertIsNone(game_test.verify_login_peer(metadata, 4321))
        physical = [{"remote_ipv4": "127.91.0.1", "remote_port": 27100}]
        with mock.patch.object(game_test, "owned_tcp_peers", return_value=physical):
            proof = game_test.verify_login_peer(metadata, 4321)
        self.assertEqual(proof["logical_ipv4"], "127.90.0.1")
        self.assertEqual(proof["logical_port"], 5001)
        self.assertEqual(proof["physical_ipv4"], "127.91.0.1")
        self.assertEqual(proof["physical_port"], 27100)
        self.assertEqual(proof["route_profile_sha256"], metadata["plan"]["route_profile_sha256"])
        receipt = game_test.read_ini(run_dir / "client/sdgo/gametest/network-verified.ini")
        self.assertEqual(receipt["network"]["policy"], game_test.ROUTED_LOOPBACK_POLICY)
        self.assertEqual(receipt["network"]["logical_ipv4"], "127.90.0.1")
        self.assertEqual(receipt["network"]["physical_ipv4"], "127.91.0.1")

    def test_metadata_rederives_routes_and_rejects_tampering(self):
        run_dir = self.prepare("tamper")
        profile = run_dir / "client" / game_test.ROUTE_PROFILE_RELATIVE
        profile.write_text(valid_profile().replace("127.91.0.1,27100", "127.91.0.2,27100"))
        with self.assertRaisesRegex(game_test.GameTestError, "sealed plan"):
            game_test.validate_metadata(run_dir)

    def test_final_report_requires_separate_routed_peer_evidence(self):
        run_dir = self.prepare("report")
        metadata = game_test.read_json(run_dir / "run.json")
        pid = 4321
        with mock.patch.object(
            game_test,
            "owned_tcp_peers",
            return_value=[{"remote_ipv4": "127.91.0.1", "remote_port": 27100}],
        ):
            network = game_test.verify_login_peer(metadata, pid)
        metadata["execution"] = {
            "started_utc": "start",
            "finished_utc": "finish",
            "entry_pid": pid,
            "game_pid": pid,
            "game_image_path": metadata["client"]["game"],
            "status": "completed",
            "timed_out": False,
            "error": None,
            "network_verified": network,
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
        events = [
            {
                "run_id": metadata["run_id"],
                "role": "client",
                "event": "run_begin",
                "case": "",
                "status": "running",
                "message": "",
                "host_key": game_test.HOST_KEY,
                "pid": pid,
                "image_path": metadata["client"]["game"],
                "duration_ms": 1,
            }
        ]
        for case_id in game_test.FLOW_CASES:
            events.append(
                {
                    **events[0],
                    "event": "case_end",
                    "case": case_id,
                    "status": "pass",
                }
            )
        events.append({**events[0], "event": "run_end", "status": "pass"})
        Path(metadata["client"]["events"]).write_text(
            "".join(json.dumps(event) + "\n" for event in events), encoding="utf-8"
        )
        self.assertEqual(game_test.build_report(run_dir)["status"], "pass")

        metadata = game_test.read_json(run_dir / "run.json")
        metadata["execution"]["network_verified"]["physical_ipv4"] = "127.90.0.1"
        game_test.atomic_write(run_dir / "run.json", json.dumps(metadata))
        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "fail")
        self.assertIn(
            "does not preserve",
            " ".join(report["layers"]["launch"]["errors"]),
        )

    def test_profile_schema_rejects_dns_wildcards_secrets_and_route_conflicts(self):
        invalid = {
            "dns": valid_profile().replace("127.91.0.1,27100", "node.example.invalid,27100"),
            "wildcard": valid_profile().replace("127.91.0.1,27100", "0.0.0.0,27100"),
            "secret": valid_profile() + "Password=do-not-accept\n",
            "duplicate logical": valid_profile().replace(
                "Route3=udp,127.90.0.1,26102,127.91.0.1,27102",
                "Route3=udp,127.90.0.1,26101,127.91.0.1,27102",
            ),
            "ambiguous reverse": valid_profile().replace(
                "Route3=udp,127.90.0.1,26102,127.91.0.1,27102",
                "Route3=udp,127.90.0.1,26102,127.91.0.1,27101",
            ),
            "wrong login route": valid_profile().replace(
                "Route1=tcp,127.90.0.1,5001", "Route1=tcp,127.90.0.1,5002"
            ),
        }
        for name, contents in invalid.items():
            with self.subTest(name=name):
                path = self.root / f"{name.replace(' ', '-')}.ini"
                path.write_text(contents, encoding="utf-8")
                with self.assertRaises(game_test.GameTestError):
                    game_test.load_routed_loopback_profile(path, "127.90.0.1", 5001)

    def test_route_profile_is_required_only_for_routed_policy(self):
        with self.assertRaisesRegex(game_test.GameTestError, "requires --route-profile"):
            game_test.prepare(
                self.template,
                self.stage,
                self.root / "missing",
                account_profile=self.account,
                target_host="127.90.0.1",
                network_policy=game_test.ROUTED_LOOPBACK_POLICY,
            )
        with self.assertRaisesRegex(game_test.GameTestError, "requires --network-policy"):
            game_test.prepare(
                self.template,
                self.stage,
                self.root / "direct-with-profile",
                account_profile=self.account,
                route_profile=self.profile,
            )

    def test_cli_exposes_routed_loopback_profile(self):
        args = game_test.parse_args(
            [
                "prepare",
                "--client-template",
                str(self.template),
                "--stage",
                str(self.stage),
                "--run-dir",
                str(self.root / "cli"),
                "--network-policy",
                "routed_loopback",
                "--route-profile",
                str(self.profile),
            ]
        )
        self.assertEqual(args.network_policy, game_test.ROUTED_LOOPBACK_POLICY)
        self.assertEqual(args.route_profile, self.profile)


if __name__ == "__main__":
    unittest.main()
