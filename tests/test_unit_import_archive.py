import io
import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import unittest
import zipfile

from owner.units import archive, assets, codec


def put_utf16(row, offset, size, value):
    encoded = value.encode("utf-16-le")
    if len(encoded) >= size:
        raise AssertionError("fixture string is too long")
    row[offset : offset + size] = encoded.ljust(size, b"\0")


def unit_record(identity=25060, transform=0, model="mdrs/test.mod", weapons=(61165, 0, 0, 0)):
    row = bytearray(codec.UNIT_SIZE)
    struct.pack_into("<I", row, 0, identity)
    put_utf16(row, 5, 100, f"Unit {identity}")
    row[308:311] = bytes((4, 2, 1))
    struct.pack_into("<H", row, 311, 10)
    row[313] = 5
    struct.pack_into("<I", row, 316, transform)
    struct.pack_into("<I", row, 321, 14500)
    struct.pack_into("<I", row, 325, 2000)
    row[329] = 1
    struct.pack_into("<I", row, 330, 100)
    row[540:546] = bytes((12, 1, 13, 14, 0, 60))
    struct.pack_into("<HH", row, 546, 33, 20)
    struct.pack_into("<HH", row, 556, 120, 125)
    row[562] = 3
    struct.pack_into("<hhhhhh", row, 563, 3, 2, -8, 1, 3, 4000)
    struct.pack_into("<II", row, 575, 70001, 70002)
    row[584] = 2
    for offset, value in zip((585, 589, 593, 597), weapons):
        struct.pack_into("<I", row, offset, value)
    put_utf16(row, 612, 60, model)
    put_utf16(row, 4654, 100, "sprs/shop.spr")
    put_utf16(row, 4754, 100, "sprs/capsule.spr")
    put_utf16(row, 4854, 100, "sprs/detail.spr")
    return bytes(row)


def weapon_record(identity=61165):
    row = bytearray(codec.WEAPON_SIZE)
    struct.pack_into("<I", row, 0, identity)
    put_utf16(row, 5, 100, f"Weapon {identity}")
    return bytes(row)


def item_record(identity=1100001):
    row = bytearray(614)
    struct.pack_into("<I", row, 0, identity)
    return bytes(row)


def gdt(records):
    size, count, extra = len(records[0]), len(records), 17
    key_size = (size + count + extra) % 65 + 192
    key = bytes(reversed(range(key_size)))
    output = bytearray(b"\xf9\xf8" + struct.pack("<iii", size, count, extra) + key)
    for row in records:
        for offset in range(0, size, key_size):
            encoded = bytearray(key_size)
            for index, value in enumerate(row[offset : offset + key_size]):
                encoded[key[index]] = value ^ key[(0x300 + 3 * index) % key_size]
            output.extend(encoded)
    return bytes(output)


def zip_bytes(files):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as package:
        for name, value in files.items():
            package.writestr(name, value)
    return output.getvalue()


class FakeBaseline:
    def __init__(self, resources=(), units=(), weapons=()):
        self.resources = set(resources)
        self.units = set(units)
        self.weapons = set(weapons)

    def exists(self, path):
        return path in self.resources

    def record(self, kind, identity):
        present = self.units if kind == "unit" else self.weapons if kind == "weapon" else set()
        return b"present" if identity in present else None


class UnitImportArchiveTests(unittest.TestCase):
    def test_codec_decodes_only_verified_fields_and_preserves_raw(self):
        raw = unit_record(transform=25061, weapons=(61165, 45082, 61169, 43286))
        result = codec.decode_record(raw)
        self.assertEqual(result["kind"], "unit")
        self.assertEqual(result["id"], 25060)
        self.assertIs(result["raw"], raw)
        self.assertEqual(result["model"], "mdrs/test.mod")
        self.assertEqual(result["transform_target"], 25061)
        self.assertEqual(result["weapons"], [61165, 45082, 61169, 43286])
        self.assertEqual(result["client"]["UnitGradeNumber"], 4)
        self.assertEqual(result["client"]["HP"], 14500)
        self.assertEqual(result["client"]["MoveLevel"], 12)
        self.assertEqual(result["client"]["DashType"], 1)
        self.assertEqual(result["client"]["BoosterIncrease"], 33)
        self.assertEqual(result["client"]["StrikingPower"], 120)
        self.assertEqual(result["client"]["AdaptEnvironUnderwater"], -8)
        self.assertEqual(result["client"]["Sight"], 4000)
        self.assertEqual(result["client"]["SkillNumber_1"], 70001)
        self.assertEqual(result["client"]["MasteredWeaponIX"], 2)
        self.assertEqual(result["client"]["portrait_capsule"], "sprs/capsule.spr")
        self.assertEqual(
            result["resource_refs"],
            ["mdrs/test.mod", "sprs/capsule.spr", "sprs/detail.spr", "sprs/shop.spr"],
        )
        weapon = codec.decode_record(weapon_record())
        self.assertEqual(weapon["client"]["WeaponNo"], 61165)
        self.assertEqual(weapon["client"]["WeaponName"], "Weapon 61165")
        self.assertEqual(weapon["client"]["DamageSize"], 0)
        patched = codec.patch_weapon(
            weapon["raw"],
            {
                "DamageSize": 12345,
                "AttackSpeed": 1.5,
                "BulletRemain": 8,
                "MaxCombo": 3,
                "FunnelID": 198061,
            },
        )
        decoded_patch = codec.decode_weapon(patched)["client"]
        self.assertEqual(decoded_patch["DamageSize"], 12345)
        self.assertEqual(decoded_patch["AttackSpeed"], 1.5)
        self.assertEqual(decoded_patch["BulletRemain"], 8)
        self.assertEqual(decoded_patch["MaxCombo"], 3)
        self.assertEqual(decoded_patch["FunnelID"], 198061)
        patched_unit = codec.patch_unit(
            raw,
            {
                "HP": 16000,
                "UnitModelName": "Display Name",
                "MoveLevel": 22,
                "DashType": 4,
                "BoosterIncrease": 44,
                "BoosterDecrease": 30,
                "StrikingPower": 150,
                "DefensivePower": 145,
                "AdaptEnvironType": 5,
                "AdaptEnvironSpace": -3,
                "AdaptEnvironGround": -1,
                "AdaptEnvironUnderwater": 6,
                "AdaptEnvironDesert": -2,
                "AdaptEnvironSky": 4,
                "Sight": 5000,
                "SkillNumber_1": 70101,
                "SkillNumber_2": 70202,
                "MasteredWeaponIX": 4,
            },
        )
        decoded_unit_patch = codec.decode_unit(patched_unit)
        self.assertEqual(decoded_unit_patch["client"]["HP"], 16000)
        self.assertEqual(decoded_unit_patch["name"], "Display Name")
        self.assertEqual(decoded_unit_patch["client"]["MoveLevel"], 22)
        self.assertEqual(decoded_unit_patch["client"]["DashType"], 4)
        self.assertEqual(decoded_unit_patch["client"]["BoosterIncrease"], 44)
        self.assertEqual(decoded_unit_patch["client"]["StrikingPower"], 150)
        self.assertEqual(decoded_unit_patch["client"]["AdaptEnvironSpace"], -3)
        self.assertEqual(decoded_unit_patch["client"]["SkillNumber_2"], 70202)
        self.assertEqual(decoded_unit_patch["client"]["MasteredWeaponIX"], 4)
        with self.assertRaisesRegex(ValueError, "lack a verified client offset"):
            codec.patch_unit(raw, {"SkillNumber_3": 70003})
        with self.assertRaisesRegex(ValueError, "invalid i16"):
            codec.patch_unit(raw, {"AdaptEnvironSpace": -32769})
        with self.assertRaises(ValueError):
            codec.decode_record(b"unknown")
        blocked = bytearray(raw)
        blocked[4] = 1
        with self.assertRaises(ValueError):
            codec.decode_record(bytes(blocked))
        unterminated = bytearray(raw)
        unterminated[612:672] = b"A\0" * 30
        with self.assertRaisesRegex(ValueError, "not NUL terminated"):
            codec.decode_unit(bytes(unterminated))

    def test_resource_scanner_finds_prefixed_and_bare_texture_references(self):
        value = (
            "mdrs\\unit.mod\0txrs\\body.txr\0".encode("utf-16-le")
            + b"prefix 46n6.txr suffix"
        )
        self.assertEqual(
            codec.scan_resource_references(value, ".zfx"),
            ["mdrs/unit.mod", "txrs/46n6.txr", "txrs/body.txr"],
        )

    def test_zip_inspection_strips_wrapper_and_reports_supplementable_dependencies(self):
        package = zip_bytes(
            {
                "Legacy Package/gdrs/config/unit.bin": unit_record(weapons=(61165, 45082, 0, 0)),
                "Legacy Package/gdrs/config/weapon.bin": weapon_record(61165),
                "Legacy Package/mdrs/test.mod": b"ZOA model",
                "Legacy Package/txrs/local.txr": b"texture",
            }
        )
        baseline = FakeBaseline(
            resources=("sprs/shop.spr", "sprs/capsule.spr", "sprs/detail.spr"),
        )
        result = archive.inspect_upload("legacy.zip", package, baseline)
        self.assertIn("mdrs/test.mod", result["files"])
        self.assertEqual([form["id"] for form in result["forms"]], [25060])
        self.assertEqual([weapon["id"] for weapon in result["weapons"]], [61165])
        self.assertEqual(result["external_refs"], ["weapon:45082"])
        self.assertEqual(
            result["resources"],
            [{"path": "mdrs/test.mod", "kind": "model"}, {"path": "txrs/local.txr", "kind": "texture"}],
        )
        self.assertIn("不是完整语义解析", result["warnings"][-1])

    def test_family_closure_requires_model_and_transform_but_allows_single_form(self):
        single = zip_bytes({"unit.bin": unit_record(transform=0), "mdrs/test.mod": b"model"})
        baseline = FakeBaseline(resources=("sprs/shop.spr", "sprs/capsule.spr", "sprs/detail.spr"))
        result = archive.inspect_upload("single.zip", single, baseline)
        self.assertEqual(result["forms"][0]["transform_target"], 0)
        missing_model = zip_bytes({"unit.bin": unit_record(model="mdrs/missing.mod")})
        with self.assertRaisesRegex(ValueError, "model is missing"):
            archive.inspect_upload("missing.zip", missing_model, baseline)
        missing_transform = zip_bytes(
            {"unit.bin": unit_record(transform=29999), "mdrs/test.mod": b"model"}
        )
        with self.assertRaisesRegex(ValueError, "transform target is missing"):
            archive.inspect_upload("transform.zip", missing_transform, baseline)
        resolved = FakeBaseline(
            resources=("mdrs/test.mod", "sprs/shop.spr", "sprs/capsule.spr", "sprs/detail.spr"),
            units=(29999,),
        )
        self.assertEqual(
            archive.inspect_upload("transform.zip", missing_transform, resolved)["forms"][0]["transform_target"],
            29999,
        )

    def test_archive_rejects_unsafe_entries_payloads_and_duplicate_native_ids(self):
        cases = [
            {"../escape.bin": unit_record(), "mdrs/test.mod": b"model"},
            {"unit.bin": unit_record(), "mdrs/test.mod": b"model", "install.ps1": b"Write-Host bad"},
            {"unknown.bin": b"x" * 200, "mdrs/test.mod": b"model"},
            {
                "unit.bin": unit_record(identity=61165),
                "weapon.bin": weapon_record(identity=61165),
                "mdrs/test.mod": b"model",
            },
        ]
        for files in cases:
            with self.subTest(files=tuple(files)), self.assertRaises(ValueError):
                archive.inspect_upload("unsafe.zip", zip_bytes(files))
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as package:
            link = zipfile.ZipInfo("linked.bin")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            package.writestr(link, "target")
        with self.assertRaisesRegex(ValueError, "links and special"):
            archive.inspect_upload("link.zip", output.getvalue())

    def test_archive_enforces_upload_file_count_and_expansion_ratio_limits(self):
        with self.assertRaisesRegex(ValueError, "32 MiB"):
            archive.inspect_upload("huge.zip", b"x" * (archive.MAX_UPLOAD + 1))
        too_many = {f"files/{index}.txt": b"" for index in range(archive.MAX_FILES + 1)}
        with self.assertRaisesRegex(ValueError, "more than"):
            archive.inspect_upload("many.zip", zip_bytes(too_many))
        bomb = {
            "unit.bin": unit_record(),
            "mdrs/test.mod": b"model",
            "padding.dat": b"\0" * (4 * 1024 * 1024),
        }
        with self.assertRaisesRegex(ValueError, "compression ratio"):
            archive.inspect_upload("bomb.zip", zip_bytes(bomb))
        duplicate = io.BytesIO()
        with zipfile.ZipFile(duplicate, "w") as package:
            package.writestr("A.txt", b"a")
            package.writestr("a.TXT", b"b")
        with self.assertRaisesRegex(ValueError, "case-insensitive duplicate"):
            archive.inspect_upload("duplicate.zip", duplicate.getvalue())

    def test_baseline_reads_loose_records_checks_any_id_and_renders_portrait(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow is not installed")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "GPackData.zdx").write_bytes(struct.pack("<6I", 1, 64, 0, 0, 0, 0))
            (root / "GPackData.zpk").write_bytes(b"")
            for relative, value in {
                "gdrs/0237.gdt": gdt([unit_record(identity=10001, model="mdrs/base.mod")]),
                "gdrs/0067.gdt": gdt([weapon_record(identity=101)]),
                "gdrs/0031.gdt": gdt([item_record(identity=1100001)]),
                "mdrs/base.mod": b"model",
            }.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(value)
            source = io.BytesIO()
            Image.new("RGBA", (8, 6), (255, 128, 0, 200)).save(source, "PNG")
            texture = root / "txrs/preview.txr"
            texture.parent.mkdir(parents=True)
            texture.write_bytes(source.getvalue())
            sprite = root / "sprs/preview.spr"
            sprite.parent.mkdir(parents=True)
            sprite.write_bytes(assets.sprite_binary("txrs/preview.txr", 8, 6))

            baseline = assets.Baseline(root)
            self.assertEqual(len(baseline.record("unit", 10001)), codec.UNIT_SIZE)
            self.assertEqual(len(baseline.record("weapon", 101)), codec.WEAPON_SIZE)
            self.assertEqual(len(baseline.record("item", 1100001)), 614)
            self.assertTrue(baseline.any_id(10001))
            self.assertTrue(baseline.any_id(1100001))
            self.assertFalse(baseline.any_id(999999))
            png = baseline.portrait("sprs/preview.spr")
            with Image.open(io.BytesIO(png)) as image:
                self.assertEqual(image.size, (8, 6))
                self.assertEqual(image.format, "PNG")
            bitmap = io.BytesIO()
            Image.new("RGB", (8, 8), "blue").save(bitmap, "BMP")
            cropped_sprite = bytearray(assets.sprite_binary("txrs/package.txr", 8, 8))
            struct.pack_into("<4f", cropped_sprite, 20, 0.25, 0.25, 0.75, 0.75)
            overlay = {
                "sprs/package.spr": bytes(cropped_sprite),
                "txrs/package.txr": bitmap.getvalue(),
            }
            package_png = assets.render_portrait("sprs/package.spr", overlay.__getitem__)
            with Image.open(io.BytesIO(package_png)) as image:
                self.assertEqual(image.size, (4, 4))
                self.assertEqual(image.format, "PNG")
            # Authored sprites may start a texel outside the texture (native UVs clamp to the edge): clamp, do not refuse.
            struct.pack_into("<4f", cropped_sprite, 20, 0.25, -0.125, 0.75, 0.75)
            overlay["sprs/package.spr"] = bytes(cropped_sprite)
            with Image.open(io.BytesIO(assets.render_portrait("sprs/package.spr", overlay.__getitem__))) as image:
                self.assertEqual(image.size, (4, 6))
            struct.pack_into("<4f", cropped_sprite, 20, 0.25, -0.5, 0.75, 0.75)  # 4 texels outside: refused
            overlay["sprs/package.spr"] = bytes(cropped_sprite)
            with self.assertRaisesRegex(ValueError, "invalid portrait crop"):
                assets.render_portrait("sprs/package.spr", overlay.__getitem__)
            info = baseline.public_info()
            self.assertEqual(info["native_id_coverage"]["strict_record_types"], {"unit": 2, "weapon": 10, "item": 34,"unit_index":85,"blueprint":33,"skill_parameter":79})
            self.assertTrue(info["native_id_coverage"]["all_gdrs_ids_scanned"])

    def test_only_receipt_owned_unchanged_loose_resources_may_be_updated(self):
        import hashlib,json
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'GPackData.zdx').write_bytes(struct.pack('<6I',1,64,0,0,0,0))
            (root/'GPackData.zpk').write_bytes(b'')
            file=root/'txrs/managed.txr';file.parent.mkdir();file.write_bytes(b'installed')
            baseline=assets.Baseline(root)
            receipt=root/'sdgo/mods/unit_catalog/install-receipt.json';receipt.parent.mkdir(parents=True)
            data={'module':'unit_catalog','schema_version':1,'host':'GOnline-54BE1568-00C97000',
                'host_sha256':'e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92',
                'files':[{'path':'txrs/managed.txr','owned':True,'installed_sha256':hashlib.sha256(b'installed').hexdigest()}]}
            receipt.write_text(json.dumps(data))
            self.assertTrue(baseline.owns_unchanged_resource('txrs/managed.txr'))
            file.write_bytes(b'external edit');self.assertFalse(baseline.owns_unchanged_resource('txrs/managed.txr'))
            file.write_bytes(b'installed');data['files'][0]['owned']=False;receipt.write_text(json.dumps(data))
            self.assertFalse(baseline.owns_unchanged_resource('txrs/managed.txr'))
            data['files'][0]['owned']=True;receipt.write_text(json.dumps(data));baseline.entries['txrs/managed.txr']=(0,0,0)
            self.assertFalse(baseline.owns_unchanged_resource('txrs/managed.txr'))

    def test_png_normalization_rejects_wrong_format_and_oversized_dimensions(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow is not installed")
        source = io.BytesIO()
        Image.new("RGB", (3, 2), "red").save(source, "PNG")
        normalized = assets.normalize_png(source.getvalue())
        self.assertTrue(normalized.startswith(b"\x89PNG\r\n\x1a\n"))
        wrong = io.BytesIO()
        Image.new("RGB", (3, 2), "red").save(wrong, "BMP")
        with self.assertRaises(ValueError):
            assets.normalize_png(wrong.getvalue())
        too_wide = io.BytesIO()
        Image.new("1", (2049, 1)).save(too_wide, "PNG")
        with self.assertRaises(ValueError):
            assets.normalize_png(too_wide.getvalue())

    @unittest.skipUnless(os.environ.get("SDGO_UNIT_IMPORT_SAMPLES"), "private RAR samples are opt-in")
    def test_configured_real_rar_samples(self):
        executable = os.environ.get("SDGO_7Z")
        client = os.environ.get("SDGO_CLIENT_ROOT")
        baseline = assets.Baseline(Path(client)) if client else None
        results = {}
        for value in os.environ["SDGO_UNIT_IMPORT_SAMPLES"].split(os.pathsep):
            path = Path(value)
            inspected = archive.inspect_upload(path.name, path.read_bytes(), baseline, executable)
            results.update({form["id"]: inspected for form in inspected["forms"]})
        self.assertEqual(len(results[25060]["forms"]), 2)
        self.assertEqual(results[25060]["forms"][0]["transform_target"], 25061)
        self.assertEqual(len(results[25128]["forms"]), 1)
        self.assertEqual(results[25128]["forms"][0]["transform_target"], 0)
        self.assertFalse(results[25060]["external_refs"])
        self.assertFalse(results[25128]["external_refs"])

    @unittest.skipUnless(
        os.environ.get("SDGO_CLIENT_ROOT") and os.environ.get("SDGO_CATALOG_EXPORT_ROOT"),
        "client/catalog cross-check is opt-in",
    )
    def test_verified_weapon_offsets_match_current_client_and_server_catalog(self):
        baseline = assets.Baseline(Path(os.environ["SDGO_CLIENT_ROOT"]))
        client = {}
        for filename in assets.WEAPON_FILES:
            for identity, raw in baseline._records(f"gdrs/{filename}.gdt", codec.WEAPON_SIZE).items():
                if raw[4] == 0:
                    client.setdefault(identity, raw)
        rows = []
        export_root = Path(os.environ["SDGO_CATALOG_EXPORT_ROOT"])
        for path in sorted(export_root.glob("dbo.tbl_WeaponsRev.part*.json")):
            rows.extend(json.loads(path.read_text(encoding="utf-8-sig")))
        shared = [
            field
            for field in codec.WEAPON_FIELDS
            if field not in ("security", "WeaponNo", "WeaponName")
        ]
        matched = 0
        equal = {field: 0 for field in shared}
        for server in rows:
            raw = client.get(server["WeaponNo"])
            if raw is None:
                continue
            matched += 1
            decoded = codec.decode_weapon(raw)["client"]
            for field in shared:
                if decoded[field] == server[field]:
                    equal[field] += 1
        self.assertGreaterEqual(matched, 8000)
        for field in shared:
            with self.subTest(field=field):
                self.assertGreaterEqual(equal[field] / matched, 0.98)
        for field in ("AttackStyle", "ExtraAttackType", "PushDist", "PushType", "MaxCombo", "FunnelID"):
            self.assertEqual(equal[field], matched)

    @unittest.skipUnless(
        os.environ.get("SDGO_CLIENT_ROOT") and os.environ.get("SDGO_CATALOG_EXPORT_ROOT"),
        "client/catalog cross-check is opt-in",
    )
    def test_verified_unit_offsets_match_current_client_and_server_catalog(self):
        baseline = assets.Baseline(Path(os.environ["SDGO_CLIENT_ROOT"]))
        client = {}
        for filename in assets.UNIT_FILES:
            for identity, raw in baseline._records(f"gdrs/{filename}.gdt", codec.UNIT_SIZE).items():
                if raw[4] == 0:
                    previous = client.setdefault(identity, raw)
                    self.assertEqual(previous, raw, f"conflicting client unit {identity}")
        rows = []
        export_root = Path(os.environ["SDGO_CATALOG_EXPORT_ROOT"])
        for path in sorted(export_root.glob("dbo.tbl_Units.part*.json")):
            rows.extend(json.loads(path.read_text(encoding="utf-8-sig")))
        shared = (
            "MoveLevel",
            "DashType",
            "DashLevel",
            "QuickStepLevel",
            "MotivePower",
            "BoosterIncrease",
            "BoosterDecrease",
            "StrikingPower",
            "DefensivePower",
            "AdaptEnvironType",
            "AdaptEnvironSpace",
            "AdaptEnvironGround",
            "AdaptEnvironUnderwater",
            "AdaptEnvironDesert",
            "AdaptEnvironSky",
            "Sight",
            "SkillNumber_1",
            "SkillNumber_2",
            "MasteredWeaponIX",
        )
        matched = 0
        equal = {field: 0 for field in shared}
        for server in rows:
            raw = client.get(server["UnitNumber"])
            if raw is None:
                continue
            matched += 1
            decoded = codec.decode_unit(raw)["client"]
            for field in shared:
                if decoded[field] == server[field]:
                    equal[field] += 1
        self.assertGreaterEqual(matched, 1000)
        for field in shared:
            with self.subTest(field=field):
                self.assertGreaterEqual(equal[field] / matched, 0.98)
        for field in (
            "MoveLevel",
            "DashType",
            "DashLevel",
            "QuickStepLevel",
            "BoosterIncrease",
            "BoosterDecrease",
            "AdaptEnvironUnderwater",
            "AdaptEnvironDesert",
            "AdaptEnvironSky",
            "Sight",
            "SkillNumber_1",
            "SkillNumber_2",
        ):
            self.assertEqual(equal[field], matched)


if __name__ == "__main__":
    unittest.main()
