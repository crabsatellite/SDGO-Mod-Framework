import configparser
import copy
import math
from pathlib import Path
import tempfile
import unittest

from manager import configuration, dependencies, registry, storage


ROOT = Path(__file__).resolve().parents[1]
SERVER_KEYS = {
    "AceChanceEasy", "AceChanceMedium", "AceChanceHard",
    "AceDifficultyEasy", "AceDifficultyMedium", "AceDifficultyHard",
    "AceHpPercentEasy", "AceHpPercentMedium", "AceHpPercentHard",
    "AceAttackPercentEasy", "AceAttackPercentMedium", "AceAttackPercentHard",
}
EXPECTED_DEFAULTS = {
    "AceChanceEasy": 5,
    "AceChanceMedium": 5,
    "AceChanceHard": 5,
    "AceDifficultyEasy": "Medium",
    "AceDifficultyMedium": "Hard",
    "AceDifficultyHard": "Hard",
    "AceHpPercentEasy": 100,
    "AceHpPercentMedium": 100,
    "AceHpPercentHard": 120,
    "AceAttackPercentEasy": 100,
    "AceAttackPercentMedium": 100,
    "AceAttackPercentHard": 100,
}


class AceAiManagerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contracts = registry.load(ROOT / "mods")
        cls.client = cls.contracts["ace_ai"]
        cls.server = cls.contracts["ace_ai_server"]
        cls.fields = {field["key"]: field for field in cls.server["settings"]}

    def test_registry_loads_exact_public_client_and_server_contracts(self):
        self.assertEqual(["client"], self.client["hosts"])
        self.assertEqual(["battle"], self.server["hosts"])
        self.assertEqual("public", self.client["distribution"])
        self.assertEqual("public", self.server["distribution"])
        self.assertEqual(SERVER_KEYS, set(self.fields))
        self.assertEqual(12, len(self.server["settings"]))
        for field in self.server["settings"]:
            self.assertEqual("config.ini", field["file"])
            self.assertEqual("settings", field["section"])
            self.assertEqual("user", field["audience"])
            self.assertEqual("restart_required", field["activation"])

    def test_manager_defaults_match_the_packaged_native_policy_inputs(self):
        config = storage.ini_values(
            (ROOT / "mods/ace_ai_server/config.ini").read_bytes()
        )
        actual = {}
        for key, field in self.fields.items():
            raw = config.get("settings", key)
            if field["type"] == "integer":
                actual[key] = int(raw)
            else:
                actual[key] = raw
            self.assertEqual(field["default"], actual[key], key)
            self.assertEqual(str(field["default"]), registry.validate(field, field["default"]))
        self.assertEqual(EXPECTED_DEFAULTS, actual)
        for key in ("AceDifficultyEasy", "AceDifficultyMedium", "AceDifficultyHard"):
            self.assertEqual(["Easy", "Medium", "Hard", "Same"], self.fields[key]["options"])

    def test_integer_policy_fields_accept_boundaries_and_reject_non_finite_bool_and_range_errors(self):
        for suffix in ("Easy", "Medium", "Hard"):
            chance = self.fields["AceChance" + suffix]
            self.assertEqual("0", registry.validate(chance, 0))
            self.assertEqual("100", registry.validate(chance, 100))
            for value in (-1, 101, True, False, math.nan, math.inf, -math.inf, 5.0, "5"):
                with self.subTest(field=chance["key"], value=value):
                    with self.assertRaises(ValueError):
                        registry.validate(chance, value)
            for prefix in ("AceHpPercent", "AceAttackPercent"):
                field = self.fields[prefix + suffix]
                self.assertEqual("1", registry.validate(field, 1))
                self.assertEqual("1000", registry.validate(field, 1000))
                for value in (0, 1001, True, math.nan, math.inf):
                    with self.subTest(field=field["key"], value=value):
                        with self.assertRaises(ValueError):
                            registry.validate(field, value)

    def test_difficulty_choices_are_per_tier_and_manager_uses_medium_for_normal(self):
        for suffix in ("Easy", "Medium", "Hard"):
            field = self.fields["AceDifficulty" + suffix]
            for value in field["options"]:
                self.assertEqual(value, registry.validate(field, value))
            for value in ("Normal", "medium", "", True, math.nan):
                with self.subTest(field=field["key"], value=value):
                    with self.assertRaises(ValueError):
                        registry.validate(field, value)

    def test_catalog_contract_keeps_matching_public_inputs_and_no_private_overlay_in_programme(self):
        client_catalog = ROOT / "mods/ace_ai/base-pilots.json"
        server_catalog = ROOT / "mods/ace_ai_server/base-pilots.json"
        self.assertTrue(client_catalog.is_file())
        self.assertTrue(server_catalog.is_file())
        self.assertEqual(client_catalog.read_bytes(), server_catalog.read_bytes())
        self.assertIn("owner-pilots.json", self.client["configuration_files"])
        self.assertIn("owner-pilots.json", self.server["configuration_files"])
        self.assertFalse((ROOT / "mods/ace_ai/owner-pilots.json").exists())
        self.assertFalse((ROOT / "mods/ace_ai_server/owner-pilots.json").exists())

    def test_client_owns_the_reviewed_battle_chat_symbol_fragment(self):
        fragment = configparser.ConfigParser(interpolation=None)
        fragment.read(ROOT / "mods/ace_ai/symbols.fragment.ini", encoding="utf-8")
        self.assertEqual(
            {
                "host",
                "ace.unit_nameplate",
                "dialogue.battle_chat.ctor",
                "dialogue.battle_chat.dtor",
                "dialogue.battle_chat.append",
            },
            set(fragment.sections()),
        )
        self.assertEqual("GOnline.exe", fragment.get("host", "image"))
        self.assertEqual("0x54BE1568", fragment.get("host", "timestamp"))
        self.assertEqual("0x00C97000", fragment.get("host", "size_of_image"))
        for section in (
            "ace.unit_nameplate",
            "dialogue.battle_chat.ctor",
            "dialogue.battle_chat.dtor",
            "dialogue.battle_chat.append",
        ):
            self.assertEqual("code", fragment.get(section, "kind"))
            self.assertRegex(fragment.get(section, "rva"), r"^0x[0-9A-F]{8}$")
            self.assertGreaterEqual(len(fragment.get(section, "expect").split()), 20)

    def test_client_has_no_authoritative_roll_or_stat_settings(self):
        self.assertEqual([], self.client["settings"])
        self.assertEqual(["owner-pilots.json"], self.client["configuration_files"])
        self.assertEqual(["config.ini", "owner-pilots.json"], self.server["configuration_files"])
        self.assertTrue(SERVER_KEYS.isdisjoint(
            {field["key"] for field in self.client.get("settings", [])}
        ))

    def test_required_shared_bridge_cannot_be_disabled_or_replaced_with_pre_ace_version(self):
        nodes = [
            {"id": "ace_ai", "host": "player", "role": "client", "enabled": True,
             "version": "0.1.0", "settings": {}, "contract": self.client},
            {"id": "match_ai", "host": "player", "role": "client", "enabled": True,
             "version": "0.1.17", "settings": {}, "contract": {
                 "provides": {"battle.start.units.client": 1}}},
            {"id": "battle_ai", "host": "player", "role": "client", "enabled": True,
             "version": "0.4.15", "settings": {}, "contract": {}},
        ]
        self.assertEqual([], dependencies.issues(nodes))
        missing = copy.deepcopy(nodes); missing[1]["enabled"] = False
        with self.assertRaisesRegex(ValueError, "match_ai"):
            dependencies.enforce_transition(nodes, missing)
        old = copy.deepcopy(nodes); old[1]["version"] = "0.1.16"
        with self.assertRaisesRegex(ValueError, "match_ai"):
            dependencies.enforce_transition(nodes, old)
        missing_api = copy.deepcopy(nodes); missing_api[1]["contract"] = {}
        with self.assertRaisesRegex(ValueError, "match_ai"):
            dependencies.enforce_transition(nodes, missing_api)
        ace_off = copy.deepcopy(missing); ace_off[0]["enabled"] = False
        self.assertEqual([], dependencies.issues(ace_off))

    def test_configuration_contract_fingerprints_owner_overlay_but_not_public_program_catalog(self):
        client_manifest = storage.ini_values(
            (ROOT / "mods/ace_ai/mod.ini").read_bytes()
        )
        server_manifest = storage.ini_values(
            (ROOT / "mods/ace_ai_server/mod.ini").read_bytes()
        )
        self.assertEqual(
            ["mod.ini", "owner-pilots.json"],
            configuration.files(client_manifest, self.client),
        )
        self.assertEqual(
            ["config.ini", "mod.ini", "owner-pilots.json"],
            configuration.files(server_manifest, self.server),
        )
        self.assertEqual(
            {"base-pilots.json", "symbols.fragment.ini"},
            {name.strip() for name in client_manifest.get("mod", "files").split(",")},
        )
        self.assertEqual("owner-pilots.json", client_manifest.get("mod", "config_files"))
        self.assertIn("base-pilots.json", {name.strip() for name in server_manifest.get("mod", "files").split(",")})
        self.assertEqual(
            {"config.ini", "owner-pilots.json"},
            {name.strip() for name in server_manifest.get("mod", "config_files").split(",")},
        )

        with tempfile.TemporaryDirectory() as directory:
            mod = Path(directory) / "client/sdgo/mods/ace_ai"
            mod.mkdir(parents=True)
            (mod / "mod.ini").write_bytes((ROOT / "mods/ace_ai/mod.ini").read_bytes())
            (mod / "base-pilots.json").write_text("public-v1", encoding="utf-8")
            names = configuration.files(client_manifest, self.client)
            missing = configuration.fingerprint(mod, names)
            (mod / "owner-pilots.json").write_text("owner-v1", encoding="utf-8")
            owner_v1 = configuration.fingerprint(mod, names)
            self.assertNotEqual(missing, owner_v1)
            (mod / "owner-pilots.json").write_text("owner-v2", encoding="utf-8")
            owner_v2 = configuration.fingerprint(mod, names)
            self.assertNotEqual(owner_v1, owner_v2)
            (mod / "base-pilots.json").write_text("public-v2", encoding="utf-8")
            self.assertEqual(owner_v2, configuration.fingerprint(mod, names))

            server_mod = Path(directory) / "server/sdgo/mods/ace_ai_server"
            server_mod.mkdir(parents=True)
            (server_mod / "mod.ini").write_bytes(
                (ROOT / "mods/ace_ai_server/mod.ini").read_bytes()
            )
            (server_mod / "config.ini").write_bytes(
                (ROOT / "mods/ace_ai_server/config.ini").read_bytes()
            )
            (server_mod / "base-pilots.json").write_text("public-v1", encoding="utf-8")
            server_names = configuration.files(server_manifest, self.server)
            server_missing = configuration.fingerprint(server_mod, server_names)
            (server_mod / "owner-pilots.json").write_text("owner-v1", encoding="utf-8")
            server_owner = configuration.fingerprint(server_mod, server_names)
            self.assertNotEqual(server_missing, server_owner)
            (server_mod / "base-pilots.json").write_text("public-v2", encoding="utf-8")
            self.assertEqual(server_owner, configuration.fingerprint(server_mod, server_names))


if __name__ == "__main__":
    unittest.main()
