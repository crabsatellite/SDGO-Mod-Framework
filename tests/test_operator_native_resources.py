import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

from manager.engines.operators import native_resources
from owner.collection_capsule.client import ArchiveReader


OPERATOR = 60047
ROOT = "sdgo/mods/operator_catalog/assets/"


def compiled(*, legacy_texture=False):
    prefix = ROOT + str(OPERATOR) + "/"
    return {
        prefix + "0.gdt": b"dialogue table",
        prefix + "1.spr": b"sprite",
        prefix + "2.zsd": b"voice",
        (prefix + "3.txr" if legacy_texture else
         native_resources.upload_texture_key(OPERATOR).replace("/0.txr","/3.txr")): b"texture",
    }


def uploaded(*, legacy_texture=False):
    texture = (
        native_resources.legacy_upload_texture_key(OPERATOR)
        if legacy_texture else native_resources.upload_texture_key(OPERATOR)
    )
    result = {texture: b"uploaded texture"}
    result.update(
        {
            ROOT + f"{OPERATOR}_{index}.spr": f"sprite {index}".encode()
            for index in range(7)
        }
    )
    return result


def config(index, archive, operator=OPERATOR):
    return (
        "[runtime]\n"
        "schema=1\n"
        f"format={native_resources.FORMAT}\n"
        f"operator_id={operator}\n"
        f"index={native_resources.INDEX_NAME}\n"
        f"index_sha256={hashlib.sha256(index).hexdigest()}\n"
        f"archive={native_resources.ARCHIVE_NAME}\n"
        f"archive_sha256={hashlib.sha256(archive).hexdigest()}\n"
    ).encode("ascii")


def metadata_offsets(index):
    cursor = 24
    result = []
    count = struct.unpack_from("<I", index, 16)[0]
    for _entry in range(count):
        while struct.unpack_from("<H", index, cursor)[0] != 0x00AA:
            cursor += 2
        cursor += 2
        result.append(cursor)
        cursor += 36
    return result


class OperatorNativeResourceTests(unittest.TestCase):
    def test_compiled_bundle_is_deterministic_and_round_trips_native_bytes(self):
        first = native_resources.build_bundle(OPERATOR, compiled())
        second = native_resources.build_bundle(OPERATOR, compiled())
        self.assertEqual(first, second)
        receipt = native_resources.validate_bundle(
            OPERATOR,
            first["files"][native_resources.CONFIG_NAME],
            first["files"][native_resources.INDEX_NAME],
            first["files"][native_resources.ARCHIVE_NAME],
            include_payload=True,
        )
        self.assertEqual(receipt["layout"], "compiled")
        self.assertEqual(receipt["resources"], compiled())
        self.assertLess(
            len(first["files"][native_resources.ARCHIVE_NAME]),
            64 * 1024 * 1024,
        )

    def test_upload_bundle_uses_short_texture_alias_and_recovers_legacy_texture(self):
        bundle = native_resources.build_bundle(OPERATOR, uploaded())
        receipt = native_resources.validate_bundle(
            OPERATOR,
            bundle["files"][native_resources.CONFIG_NAME],
            bundle["files"][native_resources.INDEX_NAME],
            bundle["files"][native_resources.ARCHIVE_NAME],
            include_payload=True,
        )
        self.assertEqual(receipt["layout"], "upload")
        self.assertEqual(receipt["resources"], uploaded())
        self.assertNotIn(ROOT + f"{OPERATOR}.spr", receipt["resources"])
        self.assertIn("sdgo/optex/60047/0.txr", receipt["resources"])
        legacy = native_resources.build_bundle(OPERATOR, uploaded(legacy_texture=True))
        legacy_receipt = native_resources.validate_bundle(
            OPERATOR,
            legacy["files"][native_resources.CONFIG_NAME],
            legacy["files"][native_resources.INDEX_NAME],
            legacy["files"][native_resources.ARCHIVE_NAME],
            include_payload=True,
        )
        self.assertEqual(legacy_receipt["layout"], "upload")
        self.assertEqual(legacy_receipt["resources"], uploaded(legacy_texture=True))

    def test_default_operator_ids_use_the_same_exact_upload_archive_contract(self):
        for identity in (60003, 60013, 60018, 60025, 60041):
            resources = {
                native_resources.upload_texture_key(identity): b"uploaded texture"
            }
            resources.update(
                {
                    ROOT + f"{identity}_{index}.spr": f"sprite {index}".encode()
                    for index in range(7)
                }
            )
            with self.subTest(operator_id=identity):
                bundle = native_resources.build_bundle(identity, resources)
                receipt = native_resources.validate_bundle(
                    identity,
                    bundle["files"][native_resources.CONFIG_NAME],
                    bundle["files"][native_resources.INDEX_NAME],
                    bundle["files"][native_resources.ARCHIVE_NAME],
                    include_payload=True,
                )
                self.assertEqual(receipt["layout"], "upload")
                self.assertEqual(receipt["resources"], resources)

    def test_generated_archive_is_readable_by_independent_native_archive_reader(self):
        bundle = native_resources.build_bundle(OPERATOR, compiled())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            index = root / native_resources.INDEX_NAME
            archive = root / native_resources.ARCHIVE_NAME
            index.write_bytes(bundle["files"][native_resources.INDEX_NAME])
            archive.write_bytes(bundle["files"][native_resources.ARCHIVE_NAME])
            reader = ArchiveReader(index, archive)
            self.assertEqual(set(reader.entries), set(compiled()))
            for name, raw in compiled().items():
                self.assertEqual(reader.read(name), raw)

    def test_texture_alias_accepts_native_decimal_syntax_at_31_wchar_boundary(self):
        resources=compiled()
        texture=next(name for name in resources if name.endswith('.txr'))
        raw=resources.pop(texture)
        boundary='sdgo/optex/60047/0000000001.txr'
        self.assertEqual(len(boundary),31)
        resources[boundary]=raw
        bundle=native_resources.build_bundle(OPERATOR,resources)
        receipt=native_resources.validate_bundle(
            OPERATOR,
            bundle["files"][native_resources.CONFIG_NAME],
            bundle["files"][native_resources.INDEX_NAME],
            bundle["files"][native_resources.ARCHIVE_NAME],
            include_payload=True,
        )
        self.assertIn(boundary,receipt["resources"])

    def test_identity_scope_and_layout_are_fail_closed(self):
        cases = [
            {ROOT + "60048/0.gdt": b"cross operator"},
            {"sdgo/optex/60048/0.txr": b"cross operator alias"},
            {"sdgo/optex/60047/" + "1" * 16 + ".txr": b"alias too long"},
            {"sdgo/optex/60047/0.spr": b"alias wrong type"},
            {
                ROOT + f"{OPERATOR}/0.gdt": b"compiled",
                native_resources.legacy_upload_texture_key(OPERATOR): b"flat",
            },
            {key: value for key, value in uploaded().items() if not key.endswith("_6.spr")},
            {ROOT + f"{OPERATOR}/0.exe": b"program"},
            {ROOT.upper() + f"{OPERATOR}/0.GDT": b"mixed case"},
        ]
        for resources in cases:
            with self.subTest(resources=sorted(resources)), self.assertRaises(ValueError):
                native_resources.build_bundle(OPERATOR, resources)

    def test_hash_chunk_range_duplicate_and_manifest_tampering_are_rejected(self):
        bundle = native_resources.build_bundle(OPERATOR, compiled())
        index = bundle["files"][native_resources.INDEX_NAME]
        archive = bundle["files"][native_resources.ARCHIVE_NAME]
        manifest = bundle["files"][native_resources.CONFIG_NAME]

        with self.assertRaisesRegex(ValueError, "SHA-256"):
            native_resources.validate_bundle(
                OPERATOR, manifest, index, archive[:-1] + b"x"
            )
        with self.assertRaises(ValueError):
            native_resources.validate_bundle(
                OPERATOR, manifest + b"unexpected=1\n", index, archive
            )

        bad_archive = bytearray(archive)
        struct.pack_into("<I", bad_archive, 0, 99)
        with self.assertRaisesRegex(ValueError, "区块链"):
            native_resources.validate_bundle(
                OPERATOR, config(index, bytes(bad_archive)), index, bytes(bad_archive)
            )

        bad_index = bytearray(index)
        offsets = metadata_offsets(index)
        struct.pack_into("<I", bad_index, offsets[1] + 4, 0)
        with self.assertRaisesRegex(ValueError, "区块范围"):
            native_resources.validate_bundle(
                OPERATOR, config(bytes(bad_index), archive), bytes(bad_index), archive
            )

        duplicate = bytearray(index)
        first_path = index[24:offsets[0]]
        second_start = offsets[0] + 36
        self.assertEqual(len(first_path), offsets[1] - second_start)
        duplicate[second_start:offsets[1]] = first_path
        with self.assertRaisesRegex(ValueError, "重复"):
            native_resources.validate_bundle(
                OPERATOR, config(bytes(duplicate), archive), bytes(duplicate), archive
            )


if __name__ == "__main__":
    unittest.main()
