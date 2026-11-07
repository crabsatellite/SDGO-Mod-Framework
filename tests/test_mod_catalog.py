from pathlib import Path
import copy
import importlib.util
import json
import re
import shutil
import tempfile
import unittest
from unittest import mock
import zipfile

from tools import build_release
from manager.configuration import render as render_runtime_contract

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mod_catalog", ROOT / "tools/mod_catalog.py")
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class CatalogTests(unittest.TestCase):
    def test_ai_catalog_programs_are_generic_and_owner_data_is_not_bundled(self):
        full=catalog.read_catalog(ROOT)
        providers=[m for m in full['mods']if m['id']=='ai_catalog']
        self.assertEqual({m['target']for m in providers},{'client','server'})
        for mod in providers:
            self.assertFalse(mod['enabled_by_default']);self.assertIn('catalog.bin',mod['config_files'])
            self.assertNotIn('catalog.bin',catalog.public_payload_files(mod))
    def test_friend_fix_is_distributed_as_complete_automatic_client_server_feature(self):
        full=catalog.read_catalog(ROOT)
        base=catalog.select_catalog(full,'base')
        expected={'friend_list_fix':'server','friend_list_client_fix':'client'}
        self.assertEqual(set(full['features']['friend-list-fix']['mods']),set(expected))
        selected={m['id']:m for m in base['mods']}
        for identity,target in expected.items():
            mod=selected[identity]
            self.assertEqual(mod['target'],target)
            self.assertTrue(mod['enabled_by_default'])
            self.assertIn('manager.json',mod['files'])
            contract=json.loads((ROOT/'mods'/identity/'manager.json').read_text(encoding='utf-8-sig'))
            self.assertEqual(contract['presentation'],'automatic_fix')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for name in ("mods", "docs"):
            shutil.copytree(ROOT / name, self.root / name)
        for name in ("README.md", "README.zh-CN.md", "CMakeLists.txt"):
            shutil.copyfile(ROOT / name, self.root / name)

    def tearDown(self):
        self.temp.cleanup()

    def replace(self, path, before, after):
        p = self.root / path
        text = p.read_text(encoding="utf-8")
        self.assertIn(before, text, "fixture replacement must match the actual UTF-8 source")
        p.write_text(text.replace(before, after), encoding="utf-8")

    def test_complete_source_catalog(self):
        c = catalog.read_catalog(self.root)
        self.assertTrue({"findwindow_fix", "ime_fix", "startup_crash_fix", "pvp_ai", "match_ai", "coop_ai", "mission_ai", "net_probe"}.issubset({m["id"] for m in c["mods"]}))
        self.assertEqual(set(c["features"]["pvp-ai"]["mods"]), {"pvp_ai", "match_ai"})
        self.assertEqual(c["features"]["pvp-ai"]["channel"], "stable")
        self.assertEqual(c["features"]["room-random"]["mods"], ["room_random", "room_random_client"])
        self.assertEqual(c["features"]["room-random"]["category"], "gameplay")
        self.assertEqual(c["features"]["room-random"]["activation"], "room-title")
        self.assertFalse(next(m for m in c["mods"] if m["id"] == "net_probe")["enabled_by_default"])

    def test_base_has_only_fixes(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), "base")
        self.assertTrue({"findwindow_fix", "ime_fix", "startup_crash_fix"}.issubset({m["id"] for m in c["mods"]}))
        self.assertTrue(all(m["category"] == "fixes" for m in c["mods"]))

    def test_shop_localization_is_an_optional_manager_registered_client_feature(self):
        c=catalog.read_catalog(self.root)
        mod=next(m for m in c['mods'] if m['id']=='shop_localization')
        self.assertEqual(mod['category'],'gameplay')
        self.assertEqual(mod['target'],'client')
        self.assertIn('zh-CN.ini',mod['files'])
        base=catalog.select_catalog(c,'base')
        self.assertNotIn('shop_localization',{m['id'] for m in base['mods']})

    def test_mission_behavior_is_one_paired_manager_feature_with_owner_data_outside_release(self):
        c = catalog.read_catalog(self.root)
        feature = c["features"]["mission-behavior"]
        self.assertEqual(
            set(feature["mods"]),
            {"mission_behavior", "mission_behavior_server"},
        )
        self.assertEqual(feature["category"], "gameplay")
        self.assertEqual(feature["channel"], "stable")
        for identity in feature["mods"]:
            mod = next(item for item in c["mods"] if item["id"] == identity)
            self.assertEqual(
                set(mod["config_files"]),
                {"config.ini", "profile.bin"},
            )
            self.assertNotIn("profile.bin", mod["files"])
            registration = json.loads(
                (self.root / "mods" / identity / "manager.json").read_text(
                    encoding="utf-8-sig"
                )
            )
            self.assertEqual(registration["provider"], "mission_behavior")
            self.assertEqual(registration["settings"], [])

    def test_shop_components_are_classified_and_generated_helper_is_declared(self):
        c = catalog.read_catalog(self.root)
        label = next(m for m in c["mods"] if m["id"] == "shop_labels")
        host = next(m for m in c["mods"] if m["id"] == "shop_catalog")
        self.assertEqual(label["category"], "fixes")
        self.assertEqual(host["category"], "network")
        self.assertFalse(host["enabled_by_default"])
        self.assertEqual(host["runtime_files"], ["shop_catalog_apply.exe"])
        self.assertIn("catalog.ini", host["config_files"])

    def test_generated_tools_must_have_safe_executable_paths(self):
        self.replace("mods/shop_catalog/mod.ini", "runtime_files=shop_catalog_apply.exe", "runtime_files=../outside.exe")
        with self.assertRaises(ValueError):
            catalog.read_catalog(self.root)

    def test_starter_policy_is_a_default_server_fix(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), "base")
        mod = next(m for m in c["mods"] if m["id"] == "new_account_profile")
        self.assertEqual(mod["target"], "server")
        self.assertTrue(mod["enabled_by_default"])
        self.assertEqual(mod["config_files"], ["config.ini"])
        self.assertEqual(mod["hosts"], ["WorldServer-548A86E3-0010C000"])

    def test_catalog_and_package_contents_follow_category_order(self):
        c = catalog.read_catalog(self.root)
        order = {key: i for i, key in enumerate(c["categories"])}
        groups = [order[feature["category"]] for feature in c["features"].values()]
        self.assertEqual(groups, sorted(groups))

    def test_selected_feature_brings_both_halves_and_fixes(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), features=["pvp-ai"])
        self.assertTrue({"match_ai", "pvp_ai"}.issubset({m["id"] for m in c["mods"]}))
        self.assertTrue(all(m["category"] == "fixes" or m["feature"] == "pvp-ai" for m in c["mods"]))
        self.assertEqual(c["profile"], "custom")

    def test_capsule_runtime_requires_owner_data_and_diagnostics_stay_off(self):
        source = catalog.read_catalog(self.root)
        runtime = next(m for m in source["mods"] if m["id"] == "capsule_catalog")
        self.assertEqual(runtime["category"], "gameplay")
        self.assertEqual(runtime["feature"], "capsule-catalog")
        self.assertFalse(runtime["enabled_by_default"])
        self.assertEqual(runtime["config_files"], ["runtime.ini"])
        base = catalog.select_catalog(source, "base")
        self.assertIn("capsule_currency_fix", {m["id"] for m in base["mods"]})
        self.assertNotIn("capsule_catalog", {m["id"] for m in base["mods"]})
        probe = next(m for m in source["mods"] if m["id"] == "capsule_price_probe")
        self.assertEqual(probe["category"], "development")
        self.assertFalse(probe["enabled_by_default"])

    def test_unknown_feature_is_rejected(self):
        with self.assertRaises(ValueError):
            catalog.select_catalog(catalog.read_catalog(self.root), features=["random-unit-not-bundled"])

    def test_operator_feature_preserves_live_config_and_is_optional(self):
        source=catalog.read_catalog(self.root)
        selected=catalog.select_catalog(source,features=['operator-assistant'])
        mod=next(m for m in selected['mods'] if m['id']=='operator_catalog')
        self.assertEqual(mod['target'],'client')
        self.assertEqual(set(mod['config_files']),{'display.ini','definitions.json','manifest.json'})
        self.assertTrue(all(m['category']=='fixes' or m['id'] in {'operator_catalog','operator_catalog_server'} for m in selected['mods']))
        self.assertNotIn('operator_catalog',{m['id'] for m in catalog.select_catalog(source,'base')['mods']})

    def test_bad_profile_is_rejected(self):
        with self.assertRaises(ValueError):
            catalog.select_catalog(catalog.read_catalog(self.root), "unknown")

    def test_category_is_independent_of_beta(self):
        c = catalog.read_catalog(self.root)
        self.assertEqual(c["features"]["collection-capsule"]["category"], "gameplay")
        self.assertNotIn("beta", c["categories"])

    def test_feature_halves_must_agree(self):
        current = catalog.read_catalog(self.root)["features"]["pvp-ai"]["channel"]
        other = "stable" if current != "stable" else "beta"
        self.replace("mods/pvp_ai/mod.ini", "channel=" + current, "channel=" + other)
        with self.assertRaisesRegex(ValueError, "disagrees"):
            catalog.read_catalog(self.root)

    def test_missing_manifest_category_fails(self):
        self.replace("mods/ime_fix/mod.ini", "category=fixes\n", "")
        with self.assertRaises(KeyError):
            catalog.read_catalog(self.root)

    def test_unknown_category_fails(self):
        self.replace("mods/ime_fix/mod.ini", "category=fixes", "category=misc")
        with self.assertRaises(ValueError):
            catalog.read_catalog(self.root)

    def test_diagnostics_cannot_silently_start_on(self):
        self.replace("mods/net_probe/mod.ini", "enabled=0", "enabled=1")
        with self.assertRaisesRegex(ValueError, "disabled"):
            catalog.read_catalog(self.root)

    def test_cmake_registration_matches_manifests(self):
        self.replace("CMakeLists.txt", "sdgo_add_mod(ime_fix ", "sdgo_add_mod(missing_fix ")
        with self.assertRaisesRegex(ValueError, "Build/catalog"):
            catalog.read_catalog(self.root)

    def test_staged_startup_symbols_must_also_enter_the_release(self):
        self.replace("mods/startup_crash_fix/mod.ini", "files=symbols.fragment.ini\n", "")
        with self.assertRaisesRegex(ValueError, "Build payload missing from manifest: startup_crash_fix"):
            catalog.read_catalog(self.root)

    def test_missing_guide_fails(self):
        (self.root / "docs/pvp-ai.md").unlink()
        with self.assertRaisesRegex(ValueError, "guide"):
            catalog.read_catalog(self.root)

    def test_readme_category_and_feature_halves_match(self):
        self.replace("README.zh-CN.md", "`pvp_ai`、`match_ai`", "`pvp_ai`")
        with self.assertRaisesRegex(ValueError, "README"):
            catalog.read_catalog(self.root)

    def test_readme_beta_label_is_checked(self):
        self.replace("README.md", "Unit family import (Beta)", "Unit family import")
        with self.assertRaisesRegex(ValueError, "channel"):
            catalog.read_catalog(self.root)

    def test_stable_readme_cannot_keep_a_beta_label(self):
        self.replace("README.md", "| FPS cap <!--", "| FPS cap (Beta) <!--")
        with self.assertRaisesRegex(ValueError, "Stale README release channel"):
            catalog.read_catalog(self.root)

    def test_network_tools_share_a_complete_feature(self):
        c = catalog.read_catalog(self.root)
        self.assertEqual(set(c["features"]["frp-distribution"]["mods"]), {"net_route", "agent_udp_pool"})
        self.assertEqual(c["features"]["frp-distribution"]["toolkit"], "frp")
        self.assertEqual(next(m for m in c["mods"] if m["id"] == "agent_udp_pool")["target"], "server")
    def test_all_categories_have_readme_blocks(self):
        catalog.read_catalog(self.root)

    def test_unsafe_and_reserved_payloads(self):
        for bad in ("../other.ini", "C:/config.ini", "/root.ini", "a//x.ini", "a/./x.ini", "x.ini:stream", "x.ini."):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                catalog.safe_file(bad)
        self.assertEqual(catalog.safe_file("settings\\owner.json"), "settings/owner.json")

    def test_config_cannot_preserve_code_or_manifest(self):
        for bad in ("ime_fix.dll", "mod.ini", "../sdgo.ini"):
            path = self.root / "mods/ime_fix/mod.ini"
            original = path.read_text(encoding="utf-8")
            path.write_text(original.replace("id=ime_fix", f"id=ime_fix\nconfig_files={bad}"), encoding="utf-8")
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                catalog.read_catalog(self.root)
            path.write_text(original, encoding="utf-8")

    def test_missing_declared_data_fails(self):
        self.replace("mods/mission_ai/mod.ini", "files=mission_ai.lua", "files=missing.lua")
        with self.assertRaisesRegex(ValueError, "payload"):
            catalog.read_catalog(self.root)

    def test_future_data_config_contract_is_supported(self):
        self.replace("mods/mission_ai/mod.ini", "files=mission_ai.lua", "files=mission_ai.lua\nconfig_files=config.ini,settings/roster.json")
        c = catalog.read_catalog(self.root)
        mod = next(m for m in c["mods"] if m["id"] == "mission_ai")
        self.assertEqual(mod["config_files"], ["config.ini", "settings/roster.json"])

    def test_compiled_owner_data_can_be_preserved_without_entering_the_release_payload(self):
        self.assertIn(".bin", catalog.CONFIG_EXTENSIONS)
        self.assertNotIn(".dll", catalog.CONFIG_EXTENSIONS)
        self.assertNotIn(".exe", catalog.CONFIG_EXTENSIONS)
        mod = {
            "id": "mission_behavior",
            "target": "client",
            "files": ["manager.json"],
            "runtime_files": [],
            "config_files": ["config.ini", "profile.bin"],
        }
        self.assertEqual(
            catalog.public_payload_files(mod),
            ["mission_behavior.dll", "mod.ini", "manager.json"],
        )

        root = self.root / "owner-profile-contract"
        stage = root / "stage"
        source = root / "mods/mission_behavior"
        output = stage / "client/sdgo/mods/mission_behavior"
        source.mkdir(parents=True)
        output.mkdir(parents=True)
        for side, loader in (("client", "d3d9.dll"), ("server", "winmm.dll")):
            for name in (loader, "sdgo/core.dll", "sdgo/sdgo.ini"):
                path = stage / side / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
        mod_ini = (
            b"[mod]\n"
            b"distribution=public\n"
            b"id=mission_behavior\n"
            b"config_files=config.ini,profile.bin\n"
        )
        manager = b'{"distribution":"public"}'
        (source / "mod.ini").write_bytes(mod_ini)
        (source / "manager.json").write_bytes(manager)
        (source / "profile.bin").write_bytes(b"owner data must remain local")
        (output / "mission_behavior.dll").write_bytes(b"fixture")
        (output / "mod.ini").write_bytes(render_runtime_contract(mod_ini,json.loads(manager)))
        (output / "manager.json").write_bytes(manager)
        release_catalog = {"mods": [mod]}
        catalog.validate_stage(root, stage, release_catalog)
        self.assertFalse((output / "profile.bin").exists())
        self.assertNotIn(
            "profile.bin",
            {
                path.name
                for path in (
                    output / name for name in catalog.public_payload_files(mod)
                )
            },
        )

    def test_generic_framework_release_ignores_stale_owner_profile_in_stage(self):
        root = self.root / "release-owner-profile-contract"
        stage = root / "stage"
        for side, loader in (("client", "d3d9.dll"), ("server", "winmm.dll")):
            for name in (loader, "sdgo/core.dll", "sdgo/sdgo.ini"):
                path = stage / side / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
        mod = {
            "id": "mission_behavior",
            "feature": "mission-behavior",
            "target": "client",
            "category": "gameplay",
            "hosts": ["GOnline-fixture"],
            "version": "0.1.0",
            "channel": "beta",
            "files": ["manager.json"],
            "runtime_files": [],
            "config_files": ["config.ini", "profile.bin"],
            "distribution": "public",
        }
        folder = stage / "client/sdgo/mods/mission_behavior"
        folder.mkdir(parents=True)
        (folder / "mission_behavior.dll").write_bytes(b"fixture")
        (folder / "mod.ini").write_bytes(
            b"[mod]\ndistribution=public\nid=mission_behavior\n"
        )
        (folder / "manager.json").write_bytes(b'{"distribution":"public"}')
        (folder / "profile.bin").write_bytes(b"stale owner data in build stage")
        manager = root / "SDGO Mod Manager.exe"
        manager.write_bytes(b"fixture")
        release_catalog = {
            "schema_version": 1,
            "profile": "beta",
            "categories": {"gameplay": {"en": "Gameplay"}},
            "features": {
                "mission-behavior": {
                    "category": "gameplay",
                    "channel": "beta",
                    "mods": ["mission_behavior"],
                }
            },
            "mods": [mod],
        }
        output = root / "dist"
        with (
            mock.patch.object(build_release, "catalog_for", return_value=release_catalog),
            mock.patch.object(build_release, "validate_stage"),
            mock.patch.object(build_release, "require_redistributable"),
            mock.patch.object(build_release, "unpack"),
        ):
            archive_path = build_release.build(
                "beta",
                "99.0.0-beta.1",
                stage,
                manager,
                output,
                commit="a" * 40,
            )
        with zipfile.ZipFile(archive_path) as archive:
            names = set(archive.namelist())
        self.assertIn(
            "client/sdgo/mods/mission_behavior/mission_behavior.dll",
            names,
        )
        self.assertNotIn(
            "client/sdgo/mods/mission_behavior/profile.bin",
            names,
        )

    def test_generated_catalog_is_stable_and_portable(self):
        c = catalog.read_catalog(self.root)
        path = self.root / "output/catalog.json"
        catalog.write_catalog(c, path)
        stamp = path.stat().st_mtime_ns
        catalog.write_catalog(c, path)
        self.assertEqual(stamp, path.stat().st_mtime_ns)
        self.assertNotIn(str(self.root), path.read_text(encoding="utf-8"))
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["schema_version"], 1)

    def test_staging_requires_declared_payload_and_current_manifest(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), features=["coop-wingmen"])
        stage = self.root / "stage"
        for side, loader in (("client", "d3d9.dll"), ("server", "winmm.dll")):
            for name in (loader, "sdgo/core.dll", "sdgo/sdgo.ini"):
                p = stage / side / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b"fixture")
        for mod in c["mods"]:
            out = stage / mod["target"] / "sdgo/mods" / mod["id"]
            out.mkdir(parents=True)
            (out / f"{mod['id']}.dll").write_bytes(b"fixture")
            for name in ("mod.ini", *mod["files"]):
                shutil.copyfile(self.root / "mods" / mod["id"] / name, out / name)
            registration=json.loads((out/'manager.json').read_text(encoding='utf-8-sig'))
            (out/'mod.ini').write_bytes(render_runtime_contract((out/'mod.ini').read_bytes(),registration))
        catalog.validate_stage(self.root, stage, c)
        script = stage / "client/sdgo/mods/mission_ai/mission_ai.lua"
        script.write_text("old script", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Stale"):
            catalog.validate_stage(self.root, stage, c)
        script.unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            catalog.validate_stage(self.root, stage, c)


class PublicTextTests(unittest.TestCase):
    """Public docs and Manager text describe the framework, never one server's own operations."""

    # Add a public reference here when a guide needs to link to it.
    LINK_TARGETS = ("github.com/crabsatellite/SDGO-Mod-Framework", "learn.microsoft.com/",
                    "www.hko.gov.hk/", "www.lua.org/", "github.com/vikiboss/60s",
                    "docs.60s-api.viki.moe/", "www.bilibili.com/video/BV1Qa4y1z7eW")
    PRIVATE = re.compile(r"update\.bat|install\.bat|朋友服|friend[- ]servers?\b|"
                         r"\b[A-Za-z]:\\(?:Dev|Users)\\|\]\((?:\.\./)*skills/", re.I)

    def public_texts(self):
        paths = [ROOT / "README.md", ROOT / "README.zh-CN.md", ROOT / "AGENTS.md",
                 *ROOT.glob("docs/**/*.md"), *ROOT.glob("mods/*/manager.json"),
                 *ROOT.glob("manager/addons/*/manager.json")]
        for path in paths:
            yield path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8-sig")

    def test_no_private_server_operations(self):
        hits = [f"{name}:{text.count(chr(10), 0, m.start()) + 1}: {m.group(0)}"
                for name, text in self.public_texts() for m in self.PRIVATE.finditer(text)]
        self.assertEqual(hits, [], "say 'the server owner's client package or update' instead")

    def test_external_links_are_public_references(self):
        hits = [f"{name}: {url}" for name, text in self.public_texts()
                for url in re.findall(r"https?://([^\s)>`'\"]+)", text)
                if not url.startswith(self.LINK_TARGETS)
                and not re.match(r"(?:[\w-]+\.)*example\.(?:org|com|net)/", url)]
        self.assertEqual(hits, [])


if __name__ == "__main__":
    unittest.main()
