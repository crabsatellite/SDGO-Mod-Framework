import base64
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

from manager import packages, storage
from manager.engines.mission_behavior.profile_codec import (
    FIELDS,
    OC_BINDINGS,
    compile_reference,
    decode_bundle,
)
from manager.mission_behavior import MissionBehavior
from manager.workspace import Workspace


ROOT = Path(__file__).resolve().parents[1]


def manifest(identity):
    host = "GOnline-fixture" if identity == "mission_behavior" else "BattleServer-fixture"
    return (
        "[mod]\n"
        "distribution=public\n"
        f"id={identity}\n"
        "category=gameplay\n"
        "feature=mission-behavior\n"
        "channel=beta\n"
        f"name={identity}\n"
        "version=0.1.0\n"
        f"dll={identity}.dll\n"
        f"hosts={host}\n"
        "config_files=config.ini,profile.bin\n"
        "enabled=1\n"
    ).encode()


def registration(identity, role):
    return storage.json_bytes(
        {
            "schema_version": 1,
            "distribution": "public",
            "id": identity,
            "title": identity,
            "category": "gameplay",
            "hosts": [role],
            "settings": [],
            "provider": "mission_behavior",
        }
    )


class MissionBehaviorManagerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        contracts = self.root / "contracts"
        contracts.mkdir()
        self.workspace = Workspace(self.root / "workspace", contracts)
        self.addCleanup(self.workspace.close)

        self.client = self.root / "game/client"
        self.battle1 = self.root / "game/server/BattleServer1"
        self.battle2 = self.root / "game/server/BattleServer2"
        self._host(self.client, "client", "client-1")
        self._host(self.battle1, "battle", "battle-1")
        self._host(self.battle2, "battle", "battle-2")
        self.workspace.save_workspace()
        self.provider = MissionBehavior(self.workspace)
        self.workspace.providers[self.provider.id] = self.provider
        patcher = mock.patch(
            "manager.mission_behavior.decode_profile",
            side_effect=self.decode_profile,
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    @staticmethod
    def decode_profile(raw):
        if not raw.startswith(b"MBP1") or len(raw) < 8:
            raise ValueError("invalid synthetic profile")
        return {
            "format_version": 1,
            "bindings_generation": int.from_bytes(raw[4:8], "little"),
            "entry_count": 3,
            "mission_count": 39,
            "server_delta_count": 3,
            "client_override_count": 12,
        }

    def _host(self, path, role, identity):
        path.mkdir(parents=True)
        executable = "GOnline.exe" if role == "client" else "BattleServer.exe"
        (path / executable).write_bytes(b"fixture")
        mod_id = "mission_behavior" if role == "client" else "mission_behavior_server"
        mod = path / "sdgo/mods" / mod_id
        mod.mkdir(parents=True)
        (mod / "mod.ini").write_bytes(manifest(mod_id))
        (mod / "manager.json").write_bytes(registration(mod_id, role))
        (mod / f"{mod_id}.dll").write_bytes(b"MZ public fixture")
        (mod / "symbols.fragment.ini").write_bytes(b"[host]\ntimestamp=0\nsize_of_image=0\n")
        self.workspace.settings["hosts"].append(
            {
                "id": identity,
                "path": str(path),
                "name": path.name,
                "role": role,
                "runtime_mode": "owner",
            }
        )

    @staticmethod
    def profile(generation=7):
        return b"MBP1" + generation.to_bytes(4, "little") + b"public behavior deltas"

    @staticmethod
    def native_row(identity, mission, npc, *, x=1, delay=0, limit=1):
        return dict(
            zip(
                FIELDS,
                (
                    identity,
                    mission,
                    1,
                    1,
                    npc,
                    x,
                    2,
                    3,
                    delay,
                    0,
                    0,
                    1,
                    limit,
                    0,
                ),
            )
        )

    @classmethod
    def real_profile(cls):
        current = [
            cls.native_row(1, 88006, 85054),
            cls.native_row(2, 88010, 85058, x=4, limit=2),
        ]
        oc = [
            cls.native_row(1, 88006, 85079, delay=5),
            cls.native_row(2, 88010, 85062, x=8, limit=3),
        ]
        return compile_reference(current, oc, dict(OC_BINDINGS), label="OC")

    def request(self, path, body=None, method="POST"):
        return self.provider.request(method, path, body or {}, {})

    def state(self, profile="oc"):
        return self.request(
            "/api/mission-behavior/preview",
            {
                "client_host": "client-1",
                "battle_hosts": ["battle-1", "battle-2"],
                "profile": profile,
            },
        )

    def files(self):
        return {
            path: (path.read_bytes() if path.is_file() else None)
            for path in (
                self.client / "sdgo/mods/mission_behavior/config.ini",
                self.client / "sdgo/mods/mission_behavior/profile.bin",
                self.battle1 / "sdgo/mods/mission_behavior_server/config.ini",
                self.battle1 / "sdgo/mods/mission_behavior_server/profile.bin",
                self.battle2 / "sdgo/mods/mission_behavior_server/config.ini",
                self.battle2 / "sdgo/mods/mission_behavior_server/profile.bin",
                self.workspace.path,
                self.provider.bundle_path,
            )
        }

    def import_profile(self, raw=None):
        state = self.request("/api/mission-behavior", method="GET")
        return self.request(
            "/api/mission-behavior/import",
            {
                "profile": base64.b64encode(raw or self.profile()).decode(),
                "expected_revision": state["manager_bundle"]["revision"],
            },
        )

    def apply(self, profile):
        state = self.state(profile)
        return self.request(
            "/api/mission-behavior/apply",
            {
                "client_host": "client-1",
                "battle_hosts": ["battle-1", "battle-2"],
                "profile": profile,
                "expected_revision": state["transaction_revision"],
            },
        )

    def test_default_is_oc_and_oc_refuses_missing_owner_profile_without_writes(self):
        before = self.files()
        state = self.request("/api/mission-behavior", method="GET")
        self.assertEqual(state["selection"]["profile"], "oc")
        self.assertEqual(state["selection"]["battle_hosts"], ["battle-1", "battle-2"])
        self.assertFalse(state["source_bundle"]["ready"])
        self.assertFalse(state["ready_to_apply"])
        with self.assertRaisesRegex(ValueError, "缺少服主提供"):
            self.request(
                "/api/mission-behavior/apply",
                {
                    **state["selection"],
                    "expected_revision": state["transaction_revision"],
                },
            )
        self.assertEqual(self.files(), before)

    def test_import_and_apply_write_identical_profile_and_preserve_unknown_settings(self):
        for path in (
            self.client / "sdgo/mods/mission_behavior/config.ini",
            self.battle1 / "sdgo/mods/mission_behavior_server/config.ini",
            self.battle2 / "sdgo/mods/mission_behavior_server/config.ini",
        ):
            path.write_bytes(b"; keep comment\r\n[settings]\r\nProfile=gco\r\nKeep=yes\r\n")
        raw = self.profile(11)
        self.import_profile(raw)
        result = self.apply("oc")
        self.assertTrue(result["saved"])
        self.assertEqual(result["restart_roles"], ["client", "battle"])
        for root, mod_id in (
            (self.client, "mission_behavior"),
            (self.battle1, "mission_behavior_server"),
            (self.battle2, "mission_behavior_server"),
        ):
            directory = root / "sdgo/mods" / mod_id
            self.assertEqual((directory / "profile.bin").read_bytes(), raw)
            config = (directory / "config.ini").read_bytes()
            self.assertIn(b"; keep comment\r\n", config)
            self.assertIn(b"Profile=oc\r\n", config)
            self.assertIn(b"Keep=yes\r\n", config)
        saved = self.workspace.settings["modules"]["mission_behavior"]
        self.assertEqual(saved["profile"], "oc")
        self.assertEqual(saved["battle_hosts"], ["battle-1", "battle-2"])
        self.assertTrue(result["consistent"]["profile"])
        self.assertTrue(result["consistent"]["bundle"])

    def test_gco_can_be_synchronized_without_an_oc_profile(self):
        result = self.apply("gco")
        self.assertTrue(result["saved"])
        for root, mod_id in (
            (self.client, "mission_behavior"),
            (self.battle1, "mission_behavior_server"),
            (self.battle2, "mission_behavior_server"),
        ):
            config = storage.ini_values(
                (root / "sdgo/mods" / mod_id / "config.ini").read_bytes()
            )
            self.assertEqual(config["settings"]["Profile"], "gco")
            self.assertFalse((root / "sdgo/mods" / mod_id / "profile.bin").exists())

    def test_gco_removes_unusable_profile_remnants_from_the_whole_group(self):
        for root, mod_id, raw in (
            (self.client, "mission_behavior", b"invalid client profile"),
            (self.battle1, "mission_behavior_server", self.profile(1)),
            (self.battle2, "mission_behavior_server", self.profile(2)),
        ):
            (root / "sdgo/mods" / mod_id / "profile.bin").write_bytes(raw)
        state = self.state("gco")
        self.assertFalse(state["consistent"]["bundle"])
        result = self.apply("gco")
        self.assertTrue(result["consistent"]["bundle"])
        for root, mod_id in (
            (self.client, "mission_behavior"),
            (self.battle1, "mission_behavior_server"),
            (self.battle2, "mission_behavior_server"),
        ):
            self.assertFalse(
                (root / "sdgo/mods" / mod_id / "profile.bin").exists()
            )

    def test_saved_server_group_expands_when_a_new_battle_channel_is_registered(self):
        self.apply("gco")
        battle3 = self.root / "game/server/BattleServer3"
        self._host(battle3, "battle", "battle-3")
        state = self.request("/api/mission-behavior", method="GET")
        self.assertEqual(
            state["selection"]["battle_hosts"],
            ["battle-1", "battle-2", "battle-3"],
        )

    def test_partial_battle_group_and_invalid_profile_are_rejected(self):
        partial = self.request(
            "/api/mission-behavior/preview",
            {
                "client_host": "client-1",
                "battle_hosts": ["battle-1"],
                "profile": "gco",
            },
        )
        self.assertIn("完整", partial["selection_error"])
        with self.assertRaisesRegex(ValueError, "只能选择"):
            self.request(
                "/api/mission-behavior/apply",
                {
                    "client_host": "client-1",
                    "battle_hosts": ["battle-1", "battle-2"],
                    "profile": "other",
                    "expected_revision": partial["transaction_revision"],
                },
            )
        self.assertFalse(
            (self.client / "sdgo/mods/mission_behavior/config.ini").exists()
        )

    def test_concurrent_edit_rejects_the_whole_transaction(self):
        state = self.state("gco")
        path = self.battle1 / "sdgo/mods/mission_behavior_server/config.ini"
        path.write_bytes(b"[settings]\nExternal=yes\n")
        before = self.files()
        with self.assertRaises(storage.Conflict):
            self.request(
                "/api/mission-behavior/apply",
                {
                    "client_host": "client-1",
                    "battle_hosts": ["battle-1", "battle-2"],
                    "profile": "gco",
                    "expected_revision": state["transaction_revision"],
                },
            )
        self.assertEqual(self.files(), before)

    def test_write_failure_rolls_back_every_client_and_battle_file(self):
        self.import_profile()
        before = self.files()
        state = self.state("oc")
        failure_path = self.battle2 / "sdgo/mods/mission_behavior_server/config.ini"
        real_atomic = storage.atomic
        failed = False

        def fail_once(path, raw):
            nonlocal failed
            if Path(path) == failure_path and not failed:
                failed = True
                raise PermissionError("synthetic locked file")
            return real_atomic(path, raw)

        with mock.patch("manager.mission_behavior.storage.atomic", side_effect=fail_once):
            with self.assertRaises(PermissionError):
                self.request(
                    "/api/mission-behavior/apply",
                    {
                        "client_host": "client-1",
                        "battle_hosts": ["battle-1", "battle-2"],
                        "profile": "oc",
                        "expected_revision": state["transaction_revision"],
                    },
                )
        self.assertTrue(failed)
        self.assertEqual(self.files(), before)
        self.assertNotIn("mission_behavior", self.workspace.settings["modules"])

    def test_running_target_blocks_switch_without_stopping_or_writing(self):
        self.import_profile()
        before = self.files()
        with mock.patch.object(
            MissionBehavior,
            "_is_running",
            side_effect=lambda target: target["host"]["id"] == "battle-2",
        ):
            state = self.state("oc")
            self.assertFalse(state["ready_to_apply"])
            with self.assertRaisesRegex(ValueError, "请先关闭"):
                self.request(
                    "/api/mission-behavior/apply",
                    {
                        "client_host": "client-1",
                        "battle_hosts": ["battle-1", "battle-2"],
                        "profile": "oc",
                        "expected_revision": state["transaction_revision"],
                    },
                )
        self.assertEqual(self.files(), before)

    def test_target_starting_after_preview_still_blocks_before_the_first_write(self):
        self.import_profile()
        state = self.state("oc")
        before = self.files()
        with mock.patch.object(
            MissionBehavior,
            "_is_running",
            side_effect=(False, False, False, False, False, True),
        ):
            with self.assertRaisesRegex(ValueError, "刚刚开始运行"):
                self.request(
                    "/api/mission-behavior/apply",
                    {
                        "client_host": "client-1",
                        "battle_hosts": ["battle-1", "battle-2"],
                        "profile": "oc",
                        "expected_revision": state["transaction_revision"],
                    },
                )
        self.assertEqual(self.files(), before)

    def test_disabled_component_blocks_switch_without_changing_other_targets(self):
        self.import_profile()
        manifest_path = (
            self.battle2 / "sdgo/mods/mission_behavior_server/mod.ini"
        )
        manifest_path.write_bytes(
            storage.patch_ini(
                manifest_path.read_bytes(), [("mod", "enabled", "0")]
            )
        )
        before = self.files()
        state = self.state("oc")
        self.assertFalse(state["ready_to_apply"])
        with self.assertRaisesRegex(ValueError, "请先启用"):
            self.request(
                "/api/mission-behavior/apply",
                {
                    "client_host": "client-1",
                    "battle_hosts": ["battle-1", "battle-2"],
                    "profile": "oc",
                    "expected_revision": state["transaction_revision"],
                },
            )
        self.assertEqual(self.files(), before)

    def test_invalid_import_and_import_conflict_preserve_previous_bundle(self):
        first = self.import_profile(self.profile(1))
        previous = self.provider.bundle_path.read_bytes()
        with self.assertRaises(ValueError):
            self.request(
                "/api/mission-behavior/import",
                {
                    "profile": base64.b64encode(b"invalid").decode(),
                    "expected_revision": first["manager_bundle"]["revision"],
                },
            )
        self.assertEqual(self.provider.bundle_path.read_bytes(), previous)
        with self.assertRaises(storage.Conflict):
            self.request(
                "/api/mission-behavior/import",
                {
                    "profile": base64.b64encode(self.profile(2)).decode(),
                    "expected_revision": storage.revision(b"stale"),
                },
            )
        self.assertEqual(self.provider.bundle_path.read_bytes(), previous)

    def test_real_codec_import_apply_export_and_corruption_rejection(self):
        raw = self.real_profile()
        decoded = decode_bundle(raw)
        self.assertEqual(decoded["entry_count"], 2)
        self.assertEqual(decoded["mission_count"], 2)
        with mock.patch(
            "manager.mission_behavior.decode_profile",
            side_effect=decode_bundle,
        ):
            imported = self.import_profile(raw)
            metadata = imported["imported_profile"]
            self.assertEqual(metadata["version"], 1)
            self.assertEqual(metadata["bindings_generation"], 1)
            self.assertEqual(metadata["entry_count"], 2)
            self.assertEqual(metadata["mission_count"], 2)
            self.assertEqual(metadata["label"], "OC")
            self.assertNotIn("rows", metadata)
            self.assertNotIn("source_digest", metadata)

            applied = self.apply("oc")
            self.assertTrue(applied["consistent"]["profile"])
            self.assertTrue(applied["consistent"]["bundle"])
            package = self.request(
                "/api/mission-behavior/export-client",
                {
                    "client_host": "client-1",
                    "battle_hosts": ["battle-1", "battle-2"],
                    "profile": "oc",
                    "expected_revision": applied["transaction_revision"],
                },
            )
            with zipfile.ZipFile(io.BytesIO(package.data)) as archive:
                self.assertEqual(
                    archive.read("client/sdgo/mods/mission_behavior/profile.bin"),
                    raw,
                )

            corrupted = bytearray(raw)
            corrupted[-1] ^= 1
            state = self.request("/api/mission-behavior", method="GET")
            with self.assertRaisesRegex(ValueError, "校验失败"):
                self.request(
                    "/api/mission-behavior/import",
                    {
                        "profile": base64.b64encode(corrupted).decode(),
                        "expected_revision": state["manager_bundle"]["revision"],
                    },
                )
            self.assertEqual(self.provider.bundle_path.read_bytes(), raw)

    def test_export_requires_the_same_applied_profile_and_bundle_on_every_target(self):
        self.import_profile()
        self.apply("oc")
        battle = self.battle2 / "sdgo/mods/mission_behavior_server"
        battle.joinpath("profile.bin").write_bytes(self.profile(99))
        state = self.state("oc")
        self.assertFalse(state["consistent"]["bundle"])
        with self.assertRaisesRegex(ValueError, "同步应用"):
            self.request(
                "/api/mission-behavior/export-client",
                {
                    "client_host": "client-1",
                    "battle_hosts": ["battle-1", "battle-2"],
                    "profile": "oc",
                    "expected_revision": state["transaction_revision"],
                },
            )

        battle.joinpath("profile.bin").write_bytes(self.profile())
        battle.joinpath("config.ini").write_bytes(b"[settings]\nProfile=gco\n")
        state = self.state("oc")
        self.assertFalse(state["consistent"]["profile"])
        with self.assertRaisesRegex(ValueError, "同步应用"):
            self.request(
                "/api/mission-behavior/export-client",
                {
                    "client_host": "client-1",
                    "battle_hosts": ["battle-1", "battle-2"],
                    "profile": "oc",
                    "expected_revision": state["transaction_revision"],
                },
            )

    def test_player_export_contains_only_public_client_component_and_applied_profile(self):
        self.import_profile()
        applied = self.apply("oc")
        package = self.request(
            "/api/mission-behavior/export-client",
            {
                "client_host": "client-1",
                "battle_hosts": ["battle-1", "battle-2"],
                "profile": "oc",
                "expected_revision": applied["transaction_revision"],
            },
        )
        with zipfile.ZipFile(io.BytesIO(package.data)) as archive:
            names = set(archive.namelist())
            self.assertIn(
                "client/sdgo/mods/mission_behavior/profile.bin", names
            )
            self.assertIn(
                "client/sdgo/mods/mission_behavior/mission_behavior.dll", names
            )
            self.assertIn("symbols.fragment.ini", names)
            self.assertIn(
                "client/sdgo/mods/mission_behavior/symbols.fragment.ini",
                names,
            )
            self.assertEqual(
                archive.read("symbols.fragment.ini"),
                archive.read(
                    "client/sdgo/mods/mission_behavior/symbols.fragment.ini"
                ),
            )
            self.assertFalse(any("server" in name.casefold() for name in names))
            self.assertFalse(any(name.endswith((".sql", ".mdf", ".ldf")) for name in names))
            release = json.loads(archive.read("manifest.json"))
            self.assertEqual(release["feature"], "mission-behavior")
            self.assertEqual(release["profile"], "oc")
            self.assertEqual(
                release["profile_sha256"],
                storage.revision(
                    archive.read("client/sdgo/mods/mission_behavior/profile.bin")
                ),
            )
            combined = b"".join(archive.read(name) for name in names)
            self.assertNotIn(str(self.root).encode(), combined)

    def test_exported_player_package_imports_through_the_standard_manager(self):
        raw = self.profile()
        self.import_profile(raw)
        applied = self.apply("oc")
        package = self.request(
            "/api/mission-behavior/export-client",
            {
                "client_host": "client-1",
                "battle_hosts": ["battle-1", "battle-2"],
                "profile": "oc",
                "expected_revision": applied["transaction_revision"],
            },
        )
        player_root = self.root / "player"
        player_root.mkdir()
        (player_root / "GOnline.exe").write_bytes(b"fixture")
        contracts = self.root / "player-contracts"
        contracts.mkdir()
        player = Workspace(self.root / "player-workspace", contracts)
        self.addCleanup(player.close)
        player.settings["hosts"].append(
            {
                "id": "player-client",
                "path": str(player_root),
                "name": "Player",
                "role": "client",
                "runtime_mode": "player",
            }
        )
        with mock.patch("manager.packages._running", return_value=False):
            result = packages.import_client(
                player,
                "player-client",
                base64.b64encode(package.data).decode("ascii"),
            )
        installed = player_root / "sdgo/mods/mission_behavior"
        self.assertEqual((installed / "profile.bin").read_bytes(), raw)
        self.assertEqual(
            (installed / "symbols.fragment.ini").read_bytes(),
            (self.client / "sdgo/mods/mission_behavior/symbols.fragment.ini").read_bytes(),
        )
        self.assertTrue(
            (player_root / "sdgo/symbols/GOnline-00000000-00000000.ini").is_file()
        )
        self.assertEqual(result["activation"], "restart_required")

    def test_provider_uses_one_embedded_ui_without_raw_path_fields(self):
        description = self.provider.describe()
        self.assertTrue(description["configured"])
        self.assertTrue(description["embedded_configuration"])
        self.assertEqual(description["fields"], {})
        panel = (ROOT / "manager/static/panels/mission_behavior.js").read_text(
            encoding="utf-8"
        )
        self.assertIn("OC（默认）", panel)
        self.assertIn("G改（现有）", panel)
        self.assertIn("全部战斗频道", panel)
        self.assertNotIn("cmd", panel.casefold())
        app = (ROOT / "manager/static/app.js").read_text(encoding="utf-8")
        self.assertIn("config.embedded_configuration", app)


if __name__ == "__main__":
    unittest.main()
