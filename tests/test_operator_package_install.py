import configparser
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest import mock
import zipfile

from manager import storage
from manager.engines.operators import install_package, native_resources


HOST = install_package.HOST_KEY
PREFIX = install_package.ARCHIVE_PREFIX


def pe(stamp=0x54BE1568, size=0x00C97000):
    raw = bytearray(0x200)
    struct.pack_into("<I", raw, 0x3C, 0x80)
    raw[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<I", raw, 0x88, stamp)
    struct.pack_into("<I", raw, 0x80 + 24 + 56, size)
    return bytes(raw)


def fragment(operator_rva="0x1234"):
    text = (
        "[host]\n"
        "image=GOnline.exe\n"
        "timestamp=0x54BE1568\n"
        "size_of_image=0x00C97000\n"
    )
    for index, name in enumerate(sorted(install_package.TRUSTED_SYMBOLS)):
        if name in install_package.TRUSTED_CAPSULE_SYMBOLS:
            values = install_package.TRUSTED_CAPSULE_SYMBOLS[name]
            text += (
                f"\n[{name}]\n"
                f"kind={values['kind']}\n"
                f"rva={values['rva']}\n"
                f"expect={values['expect']}\n"
            )
            continue
        rva = operator_rva if name == "operator.load_data" else hex(0x2000 + index)
        text += f"\n[{name}]\nkind=code\nrva={rva}\nexpect=55 8B EC\n"
    return text.encode("ascii")


def resource_files(operator_id=60047, *, kind="upload", resources=None, sha256=None):
    root = "sdgo/mods/operator_catalog/assets/"
    if resources is None:
        if kind == "upload":
            resources = {
                native_resources.upload_texture_key(operator_id):
                b"synthetic upload texture"
            }
            resources.update(
                {
                    root + f"{operator_id}_{index}.spr": f"sprite {index}".encode()
                    for index in range(7)
                }
            )
        else:
            resources = {
                root + f"{operator_id}/0.gdt": b"synthetic dialogue table",
                root + f"{operator_id}/1.zsd": b"current voice",
            }
    bundle = native_resources.build_bundle(operator_id, resources)
    files = {
        f"assets/{operator_id}/" + name: raw
        for name, raw in bundle["files"].items()
    }
    avatar = (
        {
            "kind": "upload",
            "sha256": sha256 or hashlib.sha256(
                next(
                    resources[key]
                    for key in native_resources.upload_texture_keys(operator_id)
                    if key in resources
                )
            ).hexdigest(),
        }
        if kind == "upload"
        else {"kind": "pack", "sha256": sha256 or "b" * 64, "operator_id": 60001}
    )
    operator = {
        "id": operator_id,
        "managed": True,
        "avatar": avatar,
    }
    return files, operator


def package(
    *,
    revision="1" * 64,
    assets=None,
    operators=None,
    extra_entries=(),
    root_overrides=None,
    mod_overrides=None,
    manager_overrides=None,
    symbol_fragment=None,
    tamper=None,
    acquisition="both",
):
    if assets is None:
        assets, default_operator = resource_files()
        operators = [default_operator] if operators is None else operators
    else:
        assets = dict(assets)
        operators = [] if operators is None else operators
    if acquisition not in ("both", "none", "json", "bin"):
        raise ValueError("invalid acquisition fixture mode")
    acquisition_names = {
        "both": ["acquisition-sources.json", "acquisition-sources.bin"],
        "none": [],
        "json": ["acquisition-sources.json"],
        "bin": ["acquisition-sources.bin"],
    }[acquisition]
    config_files = ["display.ini", "definitions.json", "manifest.json"]
    config_files.extend(acquisition_names)
    mod = {
        "distribution": "public",
        "id": "operator_catalog",
        "dll": "operator_catalog.dll",
        "hosts": HOST,
        "requires": ",".join(sorted(install_package.TRUSTED_SYMBOLS)),
        "files": "display.example.ini,manager.json,manifest.json",
        "config_files": ",".join(config_files),
        "version": "0.2.0" if acquisition == "none" else "0.2.1",
        "enabled": "1",
    }
    for key, value in (mod_overrides or {}).items():
        if value is None:
            mod.pop(key, None)
        else:
            mod[key] = value
    mod_text = "[mod]\n" + "".join(f"{key}={value}\n" for key, value in mod.items())
    mod_text += "\n[settings]\nEnabled=1\n"
    manager = {
        "schema_version": 1,
        "distribution": "public",
        "id": "operator_catalog",
        "hosts": ["client"],
    }
    manager.update(manager_overrides or {})
    module = {
        "operator_catalog.dll": b"synthetic fixture DLL",
        "mod.ini": mod_text.encode(),
        "manager.json": json.dumps(manager).encode(),
        "display.ini": b"\xff\xfe[\x00c\x00a\x00t\x00a\x00l\x00o\x00g\x00]\x00\n\x00",
        "definitions.json": (
            json.dumps({"schema_version": 1, "operators": operators}) + "\n"
        ).encode(),
        "display.example.ini": b"[catalog]\nVersion=2\n",
        **assets,
    }
    source_files = {
        "acquisition-sources.json": b'{"schema_version":1,"blueprints":{}}\n',
        "acquisition-sources.bin": (
            b"SDGOAS1\0" + struct.pack("<6I", 1, 32, 0, 0, 2166136261, 2)
        ),
    }
    for name in acquisition_names:
        module[name] = source_files[name]
    generated = {
        name: {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        for name, raw in module.items()
        if name not in {"operator_catalog.dll", "mod.ini", "manager.json"}
    }
    module["manifest.json"] = (
        json.dumps(
            {
                "schema_version": 1,
                "module": "operator_catalog",
                "kind": "generated-client-resources",
                "revision": revision,
                "files": generated,
            }
        )
        + "\n"
    ).encode()
    registrations = json.dumps(
        {
            "schema_version": 1,
            "provider": "operator_catalog",
            "revision": revision,
            "items": [],
            "storefront_changes": False,
        }
    ).encode()
    entries = [
        *((PREFIX + name, raw, None) for name, raw in module.items()),
        ("symbols.fragment.ini", symbol_fragment or fragment(), None),
        ("Install-Operator.ps1", b"throw 'must never execute'\n", None),
        ("registrations.json", registrations, None),
        *extra_entries,
    ]
    manifest = {
        "module": "operator_catalog",
        "schema_version": 1,
        "revision": revision,
        "storefront_changes": False,
        "host": HOST,
        "files": {
            name: hashlib.sha256(raw).hexdigest() for name, raw, _info in entries
        },
    }
    manifest.update(root_overrides or {})
    entries.append(("manifest.json", json.dumps(manifest).encode(), None))
    if tamper:
        entries = [
            (name, tamper(name, raw), info) for name, raw, info in entries
        ]
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, raw, info in entries:
            if info is None:
                archive.writestr(name, raw)
            else:
                info.filename = name
                archive.writestr(info, raw)
    return output.getvalue(), revision


class Workspace:
    def __init__(self, root, *, fail_public=False):
        self.root = Path(root)
        self.fail_public = fail_public
        self.public_calls = 0

    def public(self):
        self.public_calls += 1
        if self.fail_public:
            raise RuntimeError("synthetic state failure")
        return {"schema_version": 1, "fixture": "state"}


class OperatorPackageInstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "GOnline.exe").write_bytes(pe())
        (self.root / "sdgo").mkdir()
        (self.root / "sdgo/core.dll").write_bytes(b"framework")
        symbols = self.root / "sdgo/symbols" / (HOST + ".ini")
        symbols.parent.mkdir()
        symbols.write_text(
            "[host]\nimage=GOnline.exe\ntimestamp=0x54BE1568\n"
            "size_of_image=0x00C97000\n\n"
            "[unrelated]\nkind=data\nrva=0x99\n",
            encoding="ascii",
        )
        self.workspace = Workspace(self.root)
        self.host = {"role": "client", "path": str(self.root), "id": "client"}

    def install(self, raw):
        with mock.patch.object(install_package.updates, "running", return_value=False):
            return install_package.install_client(self.workspace, self.host, raw)

    def test_installs_verified_module_merges_symbols_and_preserves_settings(self):
        module = self.root / "sdgo/mods/operator_catalog"
        module.mkdir(parents=True)
        (module / "mod.ini").write_text(
            "[mod]\ndistribution=public\nid=operator_catalog\n"
            "dll=operator_catalog.dll\nhosts=" + HOST + "\nenabled=0\n\n"
            "[settings]\nEnabled=0\nPrivatePath=C:\\owner\\private\n",
            encoding="utf-8",
        )
        (module / "manager.json").write_text(
            '{"schema_version":1,"distribution":"public","id":"operator_catalog",'
            '"hosts":["client"]}',
            encoding="utf-8",
        )
        personal = module / "assets/personal.zsd"
        personal.parent.mkdir()
        personal.write_bytes(b"unregistered personal resource")
        opack = b"synthetic inert opack resource"
        opack_name = hashlib.sha256(opack).hexdigest()
        bundled, operator = resource_files(
            kind="pack", sha256=opack_name
        )
        raw, _revision = package(
            assets={
                **bundled,
                "assets/imports/" + opack_name + ".opack": opack,
            },
            operators=[operator],
        )

        result = self.install(raw)

        self.assertTrue((module / "acquisition-sources.json").is_file())
        self.assertTrue((module / "acquisition-sources.bin").is_file())
        ownership = install_package._inner_manifest(
            (module / "manifest.json").read_bytes())
        self.assertTrue(
            {"acquisition-sources.json", "acquisition-sources.bin"}.issubset(
                ownership["files"]))
        installed = storage.ini_values((module / "mod.ini").read_bytes())
        self.assertEqual(installed["mod"]["enabled"], "0")
        self.assertEqual(installed["settings"]["enabled"], "0")
        self.assertEqual(installed["settings"]["privatepath"], r"C:\owner\private")
        receipt = native_resources.validate_bundle(
            60047,
            (module / "assets/60047/resource-overlay.ini").read_bytes(),
            (module / "assets/60047/resources.zdx").read_bytes(),
            (module / "assets/60047/resources.zpk").read_bytes(),
            include_payload=True,
        )
        self.assertEqual(receipt["layout"], "compiled")
        self.assertIn(
            "sdgo/mods/operator_catalog/assets/60047/0.gdt",
            receipt["resources"],
        )
        self.assertFalse((module / "assets/60047/0.gdt").exists())
        self.assertEqual(
            (module / "assets/imports" / (opack_name + ".opack")).read_bytes(),
            opack,
        )
        self.assertEqual(personal.read_bytes(), b"unregistered personal resource")
        symbols = configparser.ConfigParser(interpolation=None)
        symbols.read(self.root / "sdgo/symbols" / (HOST + ".ini"))
        self.assertTrue(symbols.has_section("unrelated"))
        self.assertTrue(symbols.has_section("operator.load_data"))
        self.assertTrue(symbols.has_section("capsule.main.open_file"))
        self.assertGreater(result["installed_files"], 0)
        self.assertEqual(result["retired_files"], [])
        self.assertEqual(result["kept_modified_retired"], [])
        self.assertEqual(result["activation"], "restart_required")
        self.assertEqual(result["state"]["fixture"], "state")
        self.assertFalse((self.root / "Install-Operator.ps1").exists())

    def test_accepts_legacy_020_package_without_optional_acquisition_sources(self):
        raw, _revision = package(acquisition="none")
        result = self.install(raw)
        module = self.root / "sdgo/mods/operator_catalog"
        self.assertGreater(result["installed_files"], 0)
        self.assertFalse((module / "acquisition-sources.json").exists())
        self.assertFalse((module / "acquisition-sources.bin").exists())
        manifest = install_package._inner_manifest(
            (module / "manifest.json").read_bytes())
        self.assertNotIn("acquisition-sources.json", manifest["files"])
        self.assertNotIn("acquisition-sources.bin", manifest["files"])

    def test_rejects_packages_with_only_one_acquisition_source_file(self):
        for mode in ("json", "bin"):
            with self.subTest(mode=mode):
                raw, _revision = package(acquisition=mode)
                with self.assertRaisesRegex(ValueError, "获得方式文件必须成对提供"):
                    self.install(raw)

    def test_accepts_current_workspace_legacy_public_pairing(self):
        raw, _revision = package(mod_overrides={"distribution": None})
        result = self.install(raw)
        self.assertGreater(result["installed_files"], 0)

    def test_accepts_legacy_long_upload_texture_bundle(self):
        identity = 60047
        resources = {
            native_resources.legacy_upload_texture_key(identity):
            b"legacy upload texture",
            **{
                f"sdgo/mods/operator_catalog/assets/{identity}_{index}.spr":
                f"sprite {index}".encode()
                for index in range(7)
            },
        }
        bundled, operator = resource_files(resources=resources)
        raw, _revision = package(assets=bundled, operators=[operator])
        result = self.install(raw)
        self.assertGreater(result["installed_files"], 0)

    def test_retires_only_unchanged_owned_assets_and_keeps_modified_or_unowned(self):
        module = self.root / "sdgo/mods/operator_catalog"
        assets = module / "assets"
        assets.mkdir(parents=True)
        retired_raw = b"old unchanged"
        modified_original = b"old expected"
        (assets / "retired.zsd").write_bytes(retired_raw)
        (assets / "modified.zsd").write_bytes(b"locally changed")
        (assets / "unregistered.zsd").write_bytes(b"keep unregistered")
        old_files = {
            "assets/retired.zsd": {
                "bytes": len(retired_raw),
                "sha256": hashlib.sha256(retired_raw).hexdigest(),
            },
            "assets/modified.zsd": {
                "bytes": len(modified_original),
                "sha256": hashlib.sha256(modified_original).hexdigest(),
            },
        }
        (module / "manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "module": "operator_catalog",
                    "kind": "generated-client-resources",
                    "revision": "2" * 64,
                    "files": old_files,
                }
            ),
            encoding="utf-8",
        )
        raw, revision = package()

        result = self.install(raw)

        quarantine = (
            self.root / "sdgo/local-state/operator-catalog-retired"
            / revision / "assets/retired.zsd"
        )
        self.assertFalse((assets / "retired.zsd").exists())
        self.assertEqual(quarantine.read_bytes(), retired_raw)
        self.assertEqual((assets / "modified.zsd").read_bytes(), b"locally changed")
        self.assertEqual(
            (assets / "unregistered.zsd").read_bytes(), b"keep unregistered"
        )
        self.assertEqual(
            result["retired_files"],
            ["sdgo/mods/operator_catalog/assets/retired.zsd"],
        )
        self.assertEqual(
            result["kept_modified_retired"],
            ["sdgo/mods/operator_catalog/assets/modified.zsd"],
        )
        self.assertEqual(len(result["warnings"]), 1)

    def test_rejects_running_wrong_host_and_missing_framework(self):
        raw, _revision = package()
        with mock.patch.object(install_package.updates, "running", return_value=True):
            with self.assertRaisesRegex(ValueError, "关闭"):
                install_package.install_client(
                    self.workspace, self.host, raw
                )
        (self.root / "GOnline.exe").write_bytes(pe(stamp=0x12345678))
        with self.assertRaisesRegex(ValueError, "不支持"):
            self.install(raw)
        (self.root / "GOnline.exe").write_bytes(pe())
        (self.root / "sdgo/core.dll").unlink()
        with self.assertRaisesRegex(ValueError, "Framework"):
            self.install(raw)

    def test_rejects_unsafe_unknown_tampered_and_inconsistent_archives(self):
        cases = {}
        cases["unknown root"] = package(
            extra_entries=(("launch.cmd", b"never", None),)
        )[0]
        cases["other mod"] = package(
            extra_entries=(("client/sdgo/mods/other/other.dll", b"x", None),)
        )[0]
        cases["path escape"] = package(
            extra_entries=((PREFIX + "assets/../escape.zsd", b"x", None),)
        )[0]
        cases["wrong dll"] = package(
            extra_entries=((PREFIX + "other.dll", b"x", None),)
        )[0]
        cases["bad hash"] = package(
            tamper=lambda name, raw: raw + b"x"
            if name == PREFIX + "definitions.json"
            else raw
        )[0]
        cases["distribution"] = package(
            mod_overrides={"distribution": "local_only"}
        )[0]
        trusted = sorted(install_package.TRUSTED_SYMBOLS)
        cases["missing fixed symbol"] = package(
            mod_overrides={"requires": ",".join(trusted[:-1])}
        )[0]
        cases["untrusted capsule symbol"] = package(
            mod_overrides={
                "requires": ",".join(trusted + ["capsule.main.untrusted"])
            }
        )[0]
        cases["modified trusted capsule body"] = package(
            symbol_fragment=fragment().replace(
                b"rva=0x007E5600", b"rva=0x007E5601"
            )
        )[0]
        link = zipfile.ZipInfo()
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        cases["symlink"] = package(
            extra_entries=((PREFIX + "assets/link.zsd", b"target", link),)
        )[0]
        duplicate_assets, duplicate_operator = resource_files()
        duplicate_assets["assets/60047/RESOURCES.ZDX"] = b"duplicate case"
        duplicate = package(
            assets=duplicate_assets, operators=[duplicate_operator]
        )[0]
        cases["duplicate case"] = duplicate
        for label, raw in cases.items():
            with self.subTest(label=label), self.assertRaises(ValueError):
                self.install(raw)

    def test_requires_complete_identity_bound_native_resource_archives(self):
        missing_assets, missing_operator = resource_files()
        del missing_assets["assets/60047/resources.zpk"]

        foreign_assets, _foreign_operator = resource_files(60048)
        foreign_assets = {
            name.replace("assets/60048/", "assets/60047/"): raw
            for name, raw in foreign_assets.items()
        }
        foreign_operator = {
            "id": 60047,
            "managed": True,
            "avatar": {"kind": "upload", "sha256": "a" * 64},
        }

        compiled_assets, compiled_operator = resource_files(kind="pack")
        compiled_operator["avatar"] = {"kind": "upload", "sha256": "a" * 64}

        cases = {
            "missing archive member": package(
                assets=missing_assets, operators=[missing_operator]
            )[0],
            "foreign config identity": package(
                assets=foreign_assets, operators=[foreign_operator]
            )[0],
            "avatar layout mismatch": package(
                assets=compiled_assets, operators=[compiled_operator]
            )[0],
            "loose compiled duplicate": package(
                extra_entries=(
                    (PREFIX + "assets/60047/0.gdt", b"must stay archived", None),
                )
            )[0],
        }
        for label, raw in cases.items():
            with self.subTest(label=label), self.assertRaises(ValueError):
                self.install(raw)

    def test_rejects_symbol_conflict_and_enforces_compressed_limit(self):
        symbol = self.root / "sdgo/symbols" / (HOST + ".ini")
        symbol.write_text(
            symbol.read_text(encoding="ascii")
            + "\n[operator.load_data]\nkind=code\nrva=0x9999\nexpect=90\n",
            encoding="ascii",
        )
        raw, _revision = package()
        with self.assertRaisesRegex(ValueError, "符号定义存在冲突"):
            self.install(raw)
        with mock.patch.object(install_package, "MAX_COMPRESSED", 1):
            with self.assertRaisesRegex(ValueError, "256 MiB"):
                self.install(raw)

    def test_enforces_entry_count_individual_and_expanded_limits(self):
        raw, _revision = package()
        checks = (
            ("MAX_FILES", 1, "10000"),
            ("MAX_FILE", 1, "64 MiB"),
            ("MAX_EXPANDED", 1, "512 MiB"),
        )
        for constant, value, message in checks:
            with self.subTest(limit=constant):
                with mock.patch.object(install_package, constant, value):
                    with self.assertRaisesRegex(ValueError, message):
                        self.install(raw)

    def test_state_failure_rolls_back_writes_symbols_and_retirement(self):
        module = self.root / "sdgo/mods/operator_catalog"
        assets = module / "assets"
        assets.mkdir(parents=True)
        retired = assets / "retired.zsd"
        retired.write_bytes(b"retire me")
        old_manifest = {
            "schema_version": 1,
            "module": "operator_catalog",
            "kind": "generated-client-resources",
            "revision": "2" * 64,
            "files": {
                "assets/retired.zsd": {
                    "bytes": len(b"retire me"),
                    "sha256": hashlib.sha256(b"retire me").hexdigest(),
                }
            },
        }
        (module / "manifest.json").write_text(
            json.dumps(old_manifest), encoding="utf-8"
        )
        before_manifest = (module / "manifest.json").read_bytes()
        symbol = self.root / "sdgo/symbols" / (HOST + ".ini")
        before_symbol = symbol.read_bytes()
        failing = Workspace(self.root, fail_public=True)
        raw, revision = package()

        with mock.patch.object(install_package.updates, "running", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "synthetic state failure"):
                install_package.install_client(failing, self.host, raw)

        self.assertEqual((module / "manifest.json").read_bytes(), before_manifest)
        self.assertEqual(symbol.read_bytes(), before_symbol)
        self.assertEqual(retired.read_bytes(), b"retire me")
        self.assertFalse(
            (
                self.root / "sdgo/local-state/operator-catalog-retired"
                / revision / "assets/retired.zsd"
            ).exists()
        )
        self.assertFalse((module / "definitions.json").exists())
        self.assertEqual(
            list(
                (
                    self.root / "sdgo/local-state/operator-catalog-backups"
                ).rglob("*.bak")
            ),
            [],
        )

    def test_injected_write_and_retirement_failures_roll_back(self):
        raw, revision = package()
        module = self.root / "sdgo/mods/operator_catalog"
        symbol = self.root / "sdgo/symbols" / (HOST + ".ini")
        before_symbol = symbol.read_bytes()
        real_atomic = storage.atomic
        failed = False

        def fail_write(path, data):
            nonlocal failed
            if Path(path) == module / "definitions.json" and not failed:
                failed = True
                raise OSError("synthetic write failure")
            return real_atomic(path, data)

        with mock.patch.object(install_package.updates, "running", return_value=False):
            with mock.patch.object(
                install_package.storage, "atomic", side_effect=fail_write
            ):
                with self.assertRaisesRegex(OSError, "synthetic write failure"):
                    install_package.install_client(
                        self.workspace, self.host, raw
                    )
        self.assertFalse((module / "operator_catalog.dll").exists())
        self.assertFalse((module / "assets/60047/resources.zpk").exists())
        self.assertEqual(symbol.read_bytes(), before_symbol)

        retired = module / "assets/retired.zsd"
        retired.parent.mkdir(parents=True, exist_ok=True)
        retired.write_bytes(b"retire me")
        (module / "manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "module": "operator_catalog",
                    "kind": "generated-client-resources",
                    "revision": "2" * 64,
                    "files": {
                        "assets/retired.zsd": {
                            "bytes": len(b"retire me"),
                            "sha256": hashlib.sha256(b"retire me").hexdigest(),
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        before_manifest = (module / "manifest.json").read_bytes()
        real_replace = os.replace
        failed = False

        def fail_retirement(source, destination):
            nonlocal failed
            if Path(source) == retired and not failed:
                failed = True
                raise OSError("synthetic retirement failure")
            return real_replace(source, destination)

        with mock.patch.object(install_package.updates, "running", return_value=False):
            with mock.patch.object(
                install_package.os, "replace", side_effect=fail_retirement
            ):
                with self.assertRaisesRegex(OSError, "synthetic retirement failure"):
                    install_package.install_client(
                        self.workspace, self.host, raw
                    )
        self.assertEqual(retired.read_bytes(), b"retire me")
        self.assertEqual((module / "manifest.json").read_bytes(), before_manifest)
        self.assertEqual(symbol.read_bytes(), before_symbol)
        self.assertFalse(
            (
                self.root / "sdgo/local-state/operator-catalog-retired"
                / revision / "assets/retired.zsd"
            ).exists()
        )


if __name__ == "__main__":
    unittest.main()
