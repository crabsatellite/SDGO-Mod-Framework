import copy
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from manager import storage
from manager.login_showcase import FIRST_INDEX_ID, LoginShowcase
from manager.workspace import Workspace
from owner.units.compiler import native_catalog


ROOT = Path(__file__).resolve().parents[1]


def fnv(raw):
    value = 2166136261
    for byte in raw:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def unchecked_catalog(kind, identity, raw):
    payload = struct.pack("<6I", kind, identity, len(raw), fnv(raw), 0, 0) + raw
    return struct.pack("<8s6I", b"SDGOUN1\0", 1, 32, 1, len(payload), fnv(payload), 0) + payload


def unit(identity, name, rank, subgrade):
    raw = bytearray(4976)
    struct.pack_into("<I", raw, 0, identity)
    encoded = name.encode("utf-16-le")
    raw[5:105] = encoded.ljust(100, b"\0")
    raw[308] = rank
    raw[309] = subgrade
    return bytes(raw)


def index(identity, unit_id, enabled=1):
    raw = bytearray(614)
    struct.pack_into("<I", raw, 0, identity)
    raw[5] = enabled
    struct.pack_into("<I", raw, 6, unit_id)
    return bytes(raw)


class FakeBaseline:
    definitions = {
        100: unit(100, "原生 SS", 4, 3),
        101: unit(101, "普通 S", 4, 2),
        102: unit(102, "原生 SR", 4, 4),
    }
    indexes = {
        FIRST_INDEX_ID: index(FIRST_INDEX_ID, 100),
        FIRST_INDEX_ID + 1: index(FIRST_INDEX_ID + 1, 101),
        FIRST_INDEX_ID + 2: index(FIRST_INDEX_ID + 2, 102),
    }

    def __init__(self, client):
        self.root = Path(client)

    def unit_index(self):
        return dict(self.indexes)

    def record(self, kind, identity):
        if kind != "unit":
            raise ValueError("fixture only supports units")
        return self.definitions.get(identity)


class LoginShowcaseManagerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.client = self.root / "client"
        self.client.mkdir()
        (self.client / "GOnline.exe").touch()
        mods = self.client / "sdgo/mods"
        for identity in ("startup_crash_fix", "login_showcase", "unit_catalog"):
            target = mods / identity
            target.mkdir(parents=True)
            (target / "manager.json").write_bytes((ROOT / "mods" / identity / "manager.json").read_bytes())
        (mods / "startup_crash_fix/mod.ini").write_bytes(
            (ROOT / "mods/startup_crash_fix/mod.ini").read_bytes()
        )
        login = (ROOT / "mods/login_showcase/mod.ini").read_text(encoding="utf-8")
        login += "\n; fixture comment\n[private]\nKeep=unchanged\n"
        (mods / "login_showcase/mod.ini").write_text(login, encoding="utf-8")
        unit_catalog_ini = (
            (ROOT / "mods/unit_catalog/mod.ini")
            .read_text(encoding="utf-8")
            .replace("enabled=0", "enabled=1", 1)
        )
        (mods / "unit_catalog/mod.ini").write_text(unit_catalog_ini, encoding="utf-8")
        imported_unit = unit(200, "导入 SU", 4, 5)
        imported_index = index(FIRST_INDEX_ID + 20, 200)
        (mods / "unit_catalog/catalog.bin").write_bytes(
            native_catalog([(2, 200, imported_unit), (85, FIRST_INDEX_ID + 20, imported_index)])
        )
        self.workspace = Workspace(self.root / "manager", ROOT / "mods")
        self.workspace.add_host(self.client)
        self.host_id = self.workspace.settings["hosts"][0]["id"]
        self.provider = LoginShowcase(self.workspace)
        self.workspace.providers[self.provider.id] = self.provider
        self.baseline_patch = patch("owner.units.assets.Baseline", FakeBaseline)
        self.baseline_patch.start()

    def tearDown(self):
        self.baseline_patch.stop()
        self.workspace.close()
        self.temp.cleanup()

    def state(self):
        return self.provider.request("GET", "/api/login-showcase", {}, {})

    def test_default_pool_uses_ss_sr_su_and_merges_enabled_imports(self):
        state = self.state()
        self.assertEqual(state["settings"]["grades"], ["SS", "SR", "SU"])
        self.assertEqual([item["id"] for item in state["catalog"]], [200, 102, 101, 100])
        self.assertEqual([item["id"] for item in state["selection"]["preview"]], [200, 102, 100])
        self.assertEqual(state["selection"]["count"], 3)
        self.assertNotIn(101, [item["id"] for item in state["selection"]["preview"]])
        self.assertEqual(state["catalog"][0]["name"], "导入 SU")
        self.assertTrue(self.provider.describe()["embedded_configuration"])

    def test_explicit_save_preserves_unregistered_ini_content_and_rejects_stale_writes(self):
        state = self.state()
        settings = copy.deepcopy(state["settings"])
        settings.update({"mode": "units", "unit_ids": [200, 100]})
        body = {
            "host_id": self.host_id,
            "settings": settings,
            "revisions": state["revisions"],
            "catalog_revision": state["catalog_revision"],
        }
        saved = self.provider.request("POST", "/api/login-showcase", body, {})
        self.assertEqual(saved["selection"]["count"], 2)
        self.assertEqual([item["id"] for item in saved["selection"]["preview"]], [200, 100])
        text = (self.client / "sdgo/mods/login_showcase/mod.ini").read_text(encoding="utf-8")
        self.assertIn("Mode=units", text)
        self.assertIn("UnitIDs=200,100", text)
        self.assertIn("; fixture comment", text)
        self.assertIn("Keep=unchanged", text)
        with self.assertRaises(storage.Conflict):
            self.provider.request("POST", "/api/login-showcase", body, {})

    def test_all_recent_and_arbitrary_grade_combinations_use_native_order(self):
        state = self.state()

        def save(settings):
            nonlocal state
            state = self.provider.request(
                "POST",
                "/api/login-showcase",
                {
                    "host_id": self.host_id,
                    "settings": settings,
                    "revisions": state["revisions"],
                    "catalog_revision": state["catalog_revision"],
                },
                {},
            )
            return [item["id"] for item in state["selection"]["preview"]]

        recent = copy.deepcopy(state["settings"])
        recent.update({"mode": "recent", "recent_count": 2})
        self.assertEqual(save(recent), [200, 102])
        all_units = copy.deepcopy(state["settings"])
        all_units["mode"] = "all"
        self.assertEqual(save(all_units), [200, 102, 101, 100])
        ordinary_s = copy.deepcopy(state["settings"])
        ordinary_s.update({"mode": "grades", "grades": ["S"]})
        self.assertEqual(save(ordinary_s), [101])

    def test_catalog_revision_and_exact_candidate_validation_are_fail_closed(self):
        state = self.state()
        unknown = copy.deepcopy(state["settings"])
        unknown.update({"mode": "units", "unit_ids": [999999]})
        with self.assertRaisesRegex(ValueError, "当前已安装图鉴"):
            self.provider.request(
                "POST",
                "/api/login-showcase",
                {
                    "host_id": self.host_id,
                    "settings": unknown,
                    "revisions": state["revisions"],
                    "catalog_revision": state["catalog_revision"],
                },
                {},
            )
        invalid = copy.deepcopy(state["settings"])
        invalid["recent_count"] = True
        with self.assertRaisesRegex(ValueError, "1 到 20000"):
            self.provider.request(
                "POST",
                "/api/login-showcase",
                {
                    "host_id": self.host_id,
                    "settings": invalid,
                    "revisions": state["revisions"],
                    "catalog_revision": state["catalog_revision"],
                },
                {},
            )
        original = FakeBaseline.definitions[100]
        changed = bytearray(original)
        changed[321] = 1
        FakeBaseline.definitions[100] = bytes(changed)
        try:
            with self.assertRaisesRegex(storage.Conflict, "机体目录已改变"):
                self.provider.request(
                    "POST",
                    "/api/login-showcase",
                    {
                        "host_id": self.host_id,
                        "settings": state["settings"],
                        "revisions": state["revisions"],
                        "catalog_revision": state["catalog_revision"],
                    },
                    {},
                )
        finally:
            FakeBaseline.definitions[100] = original

    def test_disabled_import_catalog_is_not_offered(self):
        path = self.client / "sdgo/mods/unit_catalog/mod.ini"
        path.write_text(path.read_text(encoding="utf-8").replace("enabled=1", "enabled=0", 1), encoding="utf-8")
        state = self.state()
        self.assertNotIn(200, [item["id"] for item in state["catalog"]])
        self.assertEqual(state["selection"]["count"], 2)

    def test_short_or_unreleased_imported_unit_records_fail_as_catalog_errors(self):
        path = self.client / "sdgo/mods/unit_catalog/catalog.bin"
        original = path.read_bytes()
        invalid = bytearray(unit(200, "未发布", 4, 5))
        invalid[4] = 1
        for raw in (struct.pack("<I", 200), bytes(invalid)):
            with self.subTest(length=len(raw), security=raw[4] if len(raw) > 4 else None):
                path.write_bytes(unchecked_catalog(2, 200, raw))
                with self.assertRaises(ValueError):
                    self.state()
        path.write_bytes(original)

    def test_provider_registration_and_player_facing_panel_contract(self):
        from manager.providers import attach

        second = Workspace(self.root / "attached", ROOT / "mods")
        try:
            attach(second)
            self.assertIn("login_showcase", second.providers)
        finally:
            second.close()
        panel = (ROOT / "manager/static/panels/login_showcase.js").read_text(encoding="utf-8")
        self.assertIn("所有机体", panel)
        self.assertIn("最近注册", panel)
        self.assertIn("指定机体", panel)
        self.assertIn("搜索机体名称", panel)
        self.assertIn("saveBar", panel)
        app = (ROOT / "manager/static/app.js").read_text(encoding="utf-8")
        self.assertIn("provider_audience==='user'", app)


if __name__ == "__main__":
    unittest.main()
