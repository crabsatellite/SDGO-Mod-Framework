import base64
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image

from owner.collection_capsule import client as capsule
from owner.units import activity_client


def png(color=(20, 40, 60), size=(722, 644)):
    output = io.BytesIO()
    Image.new("RGB", size, color).save(output, "PNG")
    return output.getvalue()


def asset(project="owner.s.machine", raw=None):
    raw = raw or png()
    digest = hashlib.sha256(raw).hexdigest()
    return {
        "key": project + ":activity_poster",
        "project_key": project,
        "role": "activity_poster",
        "asset_sha256": digest,
        "source_asset_sha256": "1" * 64,
        "original_sha256": "2" * 64,
        "project_revision": "3" * 64,
        "png_base64": base64.b64encode(raw).decode("ascii"),
        "metadata": {},
    }


def machine(
    identity=1990001,
    unit_id=70001,
    project="owner.s.machine",
    *,
    currency=2,
    price=25,
    artwork=None,
):
    artwork = artwork or asset(project)
    return {
        "schema_version": 1,
        "id": identity,
        "name": "Imported S machine",
        "unit_id": unit_id,
        "project_key": project,
        "currency_type": currency,
        "price": price,
        "artwork_key": artwork["key"],
        "artwork_sha256": artwork["asset_sha256"],
    }


def activity_row(index, machine_id, sprite):
    row = bytearray(capsule.TABLE_SIZES[capsule.ACTIVITY_TABLE])
    struct.pack_into("<I", row, 0, capsule.TABLE_BASES[capsule.ACTIVITY_TABLE] + index)
    struct.pack_into("<I", row, 5, machine_id)
    struct.pack_into("<I", row, 9, 0)
    struct.pack_into("<I", row, 15, 10000)
    capsule._put_wide(
        row,
        capsule.ACTIVITY_SPRITE_OFFSET,
        capsule.ACTIVITY_SPRITE_CAPACITY,
        sprite,
    )
    return bytes(row)


def baseline():
    rows = [
        activity_row(0, 9000, r"sprs\21000.spr"),
        activity_row(1, 9001, r"sprs\21001.spr"),
        activity_row(2, 990001, capsule.COLLECTION_SPRITE),
    ]
    gdt = capsule.encode_gdt(
        capsule.TABLE_SIZES[capsule.ACTIVITY_TABLE], rows, 987654
    )
    resources = {
        "gdrs/0099.gdt": gdt,
        "sprs/62000.spr": b"unrelated occupied sprite",
        "sprs/unrelated.spr": capsule._standalone_sprite(
            r"txrs\62001.txr", 1, 1, (0.0, 0.0, 1.0, 1.0)
        ),
        "sprs/65534.spr": b"collection sprite",
        "txrs/65534.txr": b"collection texture",
        "other/provider.bin": b"preserve me",
    }
    return gdt, resources, rows


class ActivityClientTests(unittest.TestCase):
    def test_explicit_owned_rename_changes_only_machine_id_and_is_idempotent(self):
        gdt, resources, _ = baseline()
        artwork = asset()
        first = activity_client.compile_activity_client(
            gdt, resources, [machine(artwork=artwork)], [artwork]
        )
        rename = {
            "from_id": 1990001,
            "to_id": 9401,
            "project_key": "owner.s.machine",
            "unit_id": 70001,
        }
        renamed_machine = machine(9401, artwork=artwork)
        renamed = activity_client.compile_activity_client(
            first["gdt"],
            first["resources"],
            [renamed_machine],
            [artwork],
            ownership=first["ownership"],
            renames=[rename],
        )

        _, before_rows, before_extra = capsule.decode_gdt(first["gdt"])
        _, after_rows, after_extra = capsule.decode_gdt(renamed["gdt"])
        self.assertEqual(before_extra, after_extra)
        self.assertEqual(before_rows[:-1], after_rows[:-1])
        before = before_rows[-1]
        after = after_rows[-1]
        self.assertEqual(before[:5], after[:5])
        self.assertEqual(before[9:], after[9:])
        self.assertEqual(struct.unpack_from("<I", before, 5)[0], 1990001)
        self.assertEqual(struct.unpack_from("<I", after, 5)[0], 9401)
        self.assertEqual(
            {
                path: raw
                for path, raw in renamed["resources"].items()
                if path.lower().replace("\\", "/") != "gdrs/0099.gdt"
            },
            {
                path: raw
                for path, raw in first["resources"].items()
                if path.lower().replace("\\", "/") != "gdrs/0099.gdt"
            },
        )
        self.assertEqual(renamed["placements"][0]["action"], "renamed")
        self.assertEqual(renamed["placements"][0]["grid_id"], 145004)
        self.assertEqual(renamed["placements"][0]["price"], 25)
        self.assertEqual(renamed["ownership"][0]["machine_id"], 9401)
        self.assertEqual(
            renamed["ownership"][0]["row_sha256"],
            hashlib.sha256(after).hexdigest(),
        )
        for field in (
            "grid_id",
            "sprite_path",
            "texture_path",
            "sprite_sha256",
            "texture_sha256",
            "project_key",
            "unit_id",
            "artwork_sha256",
        ):
            self.assertEqual(
                renamed["ownership"][0][field],
                first["ownership"][0][field],
            )

        repeated = activity_client.compile_activity_client(
            renamed["gdt"],
            renamed["resources"],
            [renamed_machine],
            [artwork],
            ownership=renamed["ownership"],
            renames=[rename],
        )
        self.assertEqual(repeated["gdt"], renamed["gdt"])
        self.assertEqual(repeated["resources"], renamed["resources"])
        self.assertEqual(repeated["ownership"], renamed["ownership"])
        self.assertEqual(repeated["placements"][0]["action"], "unchanged")

        with self.assertRaisesRegex(
            activity_client.ActivityClientError,
            "cannot also change native content",
        ):
            activity_client.compile_activity_client(
                first["gdt"],
                first["resources"],
                [machine(9401, price=99, artwork=artwork)],
                [artwork],
                ownership=first["ownership"],
                renames=[rename],
            )

    def test_rename_rejects_unowned_source_foreign_target_other_owner_and_forgery(self):
        gdt, resources, rows = baseline()
        artwork = asset()
        first = activity_client.compile_activity_client(
            gdt, resources, [machine(artwork=artwork)], [artwork]
        )
        rename = {
            "from_id": 1990001,
            "to_id": 9401,
            "project_key": "owner.s.machine",
            "unit_id": 70001,
        }

        for source in (9000, 990001):
            with self.subTest(source=source), self.assertRaisesRegex(
                activity_client.ActivityClientError, "source .* is not owned"
            ):
                activity_client.compile_activity_client(
                    gdt,
                    resources,
                    [],
                    [],
                    renames=[{**rename, "from_id": source}],
                )

        _, installed_rows, installed_extra = capsule.decode_gdt(first["gdt"])
        foreign_rows = list(installed_rows)
        foreign_rows[0] = activity_row(0, 9401, r"sprs\21000.spr")
        foreign_gdt = capsule.encode_gdt(179, foreign_rows, installed_extra)
        foreign_resources = dict(first["resources"])
        foreign_resources["gdrs/0099.gdt"] = foreign_gdt
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "target 9401.*foreign row"
        ):
            activity_client.compile_activity_client(
                foreign_gdt,
                foreign_resources,
                [],
                [],
                ownership=first["ownership"],
                renames=[rename],
            )

        other_artwork = asset("owner.other.machine", png((1, 2, 3)))
        two_owned = activity_client.compile_activity_client(
            gdt,
            resources,
            [
                machine(1990001, 70001, "owner.s.machine", artwork=artwork),
                machine(9401, 70002, "owner.other.machine", artwork=other_artwork),
            ],
            [artwork, other_artwork],
        )
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "target 9401.*another owner"
        ):
            activity_client.compile_activity_client(
                two_owned["gdt"],
                two_owned["resources"],
                [],
                [],
                ownership=two_owned["ownership"],
                renames=[rename],
            )

        for field, value in (
            ("project_key", "owner.forged"),
            ("unit_id", 70002),
        ):
            with self.subTest(field=field), self.assertRaisesRegex(
                activity_client.ActivityClientError, "source 1990001 ownership differs"
            ):
                activity_client.compile_activity_client(
                    first["gdt"],
                    first["resources"],
                    [],
                    [],
                    ownership=first["ownership"],
                    renames=[{**rename, field: value}],
                )

        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "rename mappings overlap"
        ):
            activity_client.compile_activity_client(
                first["gdt"],
                first["resources"],
                [],
                [],
                ownership=first["ownership"],
                renames=[rename, {**rename, "to_id": 9402}],
            )
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "fields differ from schema"
        ):
            activity_client.compile_activity_client(
                first["gdt"],
                first["resources"],
                [],
                [],
                ownership=first["ownership"],
                renames=[{**rename, "owner": "forged"}],
            )

    def test_allocator_also_reserves_base_archive_ids_without_loading_resource_bytes(self):
        gdt,resources,_=baseline();artwork=asset()
        result=activity_client.compile_activity_client(gdt,resources,[machine(artwork=artwork)],[artwork],
            reserved_resource_ids={62002,62003})
        self.assertEqual(62004,result['placements'][0]['resource_id'])

    def test_append_preserves_collection_rows_resources_extra_and_allocates_free_pair(self):
        gdt, resources, original_rows = baseline()
        artwork = asset()
        original_resources = copy.deepcopy(resources)
        result = activity_client.compile_activity_client(
            gdt, resources, [machine(artwork=artwork)], [artwork]
        )

        self.assertEqual(resources, original_resources)
        self.assertEqual(result["metadata"]["extra"], 987654)
        size, rows, extra = capsule.decode_gdt(result["gdt"])
        self.assertEqual(size, 179)
        self.assertEqual(extra, 987654)
        self.assertEqual(rows[:3], original_rows)
        self.assertEqual(struct.unpack_from("<I", rows[2], 5)[0], 990001)
        self.assertEqual(len(rows), 4)
        added = rows[3]
        self.assertEqual(struct.unpack_from("<I", added, 0)[0], 145004)
        self.assertEqual(struct.unpack_from("<I", added, 5)[0], 1990001)
        self.assertEqual(struct.unpack_from("<I", added, 9)[0], 2)
        self.assertEqual(struct.unpack_from("<I", added, 15)[0], 25)
        self.assertEqual(capsule._activity_sprite(added), r"sprs\62002.spr")
        self.assertEqual(result["resources"]["other/provider.bin"], b"preserve me")
        self.assertEqual(result["resources"]["sprs/65534.spr"], b"collection sprite")
        self.assertEqual(result["resources"]["gdrs/0099.gdt"], result["gdt"])
        self.assertIn("sprs/62002.spr", result["resources"])
        self.assertIn("txrs/62002.txr", result["resources"])

        placement = result["placements"][0]
        self.assertEqual(placement["action"], "added")
        self.assertEqual(placement["grid_id"], 145004)
        self.assertEqual(placement["native_position"], 1)
        self.assertEqual((placement["page"], placement["page_slot"]), (1, 1))
        self.assertTrue(placement["native_position_predicted"])
        self.assertEqual(placement["resource_id"], 62002)
        self.assertEqual(
            placement["asset"]["asset_sha256"], artwork["asset_sha256"]
        )
        self.assertEqual(
            result["ownership"][0]["row_sha256"], hashlib.sha256(added).hexdigest()
        )

    def test_recompile_is_byte_identical_and_reviewed_update_reuses_grid_and_resources(self):
        gdt, resources, _ = baseline()
        artwork = asset()
        first = activity_client.compile_activity_client(
            gdt, resources, [machine(artwork=artwork)], [artwork]
        )
        repeated = activity_client.compile_activity_client(
            first["gdt"],
            first["resources"],
            [machine(artwork=artwork)],
            [artwork],
            ownership=first["ownership"],
        )
        self.assertEqual(repeated["gdt"], first["gdt"])
        self.assertEqual(repeated["resources"], first["resources"])
        self.assertEqual(repeated["ownership"], first["ownership"])
        self.assertEqual(repeated["placements"][0]["action"], "unchanged")

        replacement = asset(raw=png((100, 30, 10), (361, 322)))
        updated = activity_client.compile_activity_client(
            repeated["gdt"],
            repeated["resources"],
            [machine(price=99, artwork=replacement)],
            [replacement],
            ownership=repeated["ownership"],
        )
        placement = updated["placements"][0]
        self.assertEqual(placement["action"], "updated")
        self.assertEqual(placement["grid_id"], 145004)
        self.assertEqual(placement["resource_id"], 62002)
        self.assertEqual(placement["price"], 99)
        self.assertNotEqual(
            updated["ownership"][0]["texture_sha256"],
            first["ownership"][0]["texture_sha256"],
        )

    def test_multiple_new_machines_cross_native_page_boundary_in_reviewed_order(self):
        gdt, resources, _ = baseline()
        artworks = []
        configurations = []
        for index in range(10):
            project = f"owner.s.machine.{index}"
            artwork = asset(project, png((20 + index, 40, 60)))
            artworks.append(artwork)
            configurations.append(
                machine(
                    1990001 + index,
                    70001 + index,
                    project,
                    price=25 + index,
                    artwork=artwork,
                )
            )
        result = activity_client.compile_activity_client(
            gdt, resources, configurations, artworks
        )
        self.assertEqual(
            [row["grid_id"] for row in result["placements"]],
            list(range(145004, 145014)),
        )
        self.assertEqual(
            [row["resource_id"] for row in result["placements"]],
            list(range(62002, 62012)),
        )
        self.assertEqual(
            (result["placements"][-1]["page"], result["placements"][-1]["page_slot"]),
            (1, 1),
        )
        self.assertEqual((result['placements'][0]['page'],result['placements'][0]['page_slot']),(2,2))

    def test_omitted_owned_machine_is_verified_and_retained_without_deletion(self):
        gdt, resources, _ = baseline()
        artwork = asset()
        first = activity_client.compile_activity_client(
            gdt, resources, [machine(artwork=artwork)], [artwork]
        )
        retained = activity_client.compile_activity_client(
            first["gdt"],
            first["resources"],
            [],
            [],
            ownership=first["ownership"],
        )
        self.assertEqual(retained["gdt"], first["gdt"])
        self.assertEqual(retained["resources"], first["resources"])
        self.assertEqual(retained["ownership"], first["ownership"])
        self.assertEqual(retained["placements"][0]["action"], "retained")
        self.assertIsNone(retained["placements"][0]["name"])

    def test_foreign_machine_collision_and_owned_tampering_fail_closed(self):
        gdt, resources, rows = baseline()
        artwork = asset()
        foreign = list(rows)
        mutable = bytearray(foreign[0])
        struct.pack_into("<I", mutable, 5, 1990001)
        foreign[0] = bytes(mutable)
        foreign_gdt = capsule.encode_gdt(179, foreign, 987654)
        foreign_resources = dict(resources)
        foreign_resources["gdrs/0099.gdt"] = foreign_gdt
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "occupied by a foreign row"
        ):
            activity_client.compile_activity_client(
                foreign_gdt,
                foreign_resources,
                [machine(artwork=artwork)],
                [artwork],
            )

        first = activity_client.compile_activity_client(
            gdt, resources, [machine(artwork=artwork)], [artwork]
        )
        tampered = dict(first["resources"])
        tampered["sprs/62002.spr"] += b"x"
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "resources were edited"
        ):
            activity_client.compile_activity_client(
                first["gdt"],
                tampered,
                [machine(artwork=artwork)],
                [artwork],
                ownership=first["ownership"],
            )

        _, installed_rows, installed_extra = capsule.decode_gdt(first["gdt"])
        edited_rows = list(installed_rows)
        edited = bytearray(edited_rows[-1])
        struct.pack_into("<I", edited, 15, 777)
        edited_rows[-1] = bytes(edited)
        edited_gdt = capsule.encode_gdt(179, edited_rows, installed_extra)
        edited_resources = dict(first["resources"])
        edited_resources["gdrs/0099.gdt"] = edited_gdt
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "row was edited"
        ):
            activity_client.compile_activity_client(
                edited_gdt,
                edited_resources,
                [machine(artwork=artwork)],
                [artwork],
                ownership=first["ownership"],
            )

        wrong_owner = copy.deepcopy(first["ownership"])
        wrong_owner[0]["owner"] = "another/provider"
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "identity is unsupported"
        ):
            activity_client.compile_activity_client(
                first["gdt"],
                first["resources"],
                [machine(artwork=artwork)],
                [artwork],
                ownership=wrong_owner,
            )

    def test_existing_0099_sprite_path_reserves_numeric_pair_without_a_resource(self):
        gdt, resources, rows = baseline()
        changed = list(rows)
        changed[0] = activity_row(0, 9000, r"sprs\62002.spr")
        changed_gdt = capsule.encode_gdt(179, changed, 987654)
        changed_resources = dict(resources)
        changed_resources["gdrs/0099.gdt"] = changed_gdt
        artwork = asset()
        result = activity_client.compile_activity_client(
            changed_gdt,
            changed_resources,
            [machine(artwork=artwork)],
            [artwork],
        )
        self.assertEqual(result["placements"][0]["resource_id"], 62003)

    def test_unrelated_duplicate_machine_ids_are_preserved_without_becoming_owned(self):
        gdt, resources, rows = baseline()
        changed = list(rows)
        changed[1] = activity_row(1, 9000, r"sprs\21001.spr")
        changed_gdt = capsule.encode_gdt(179, changed, 987654)
        changed_resources = dict(resources)
        changed_resources["gdrs/0099.gdt"] = changed_gdt
        artwork = asset()
        result = activity_client.compile_activity_client(
            changed_gdt,
            changed_resources,
            [machine(artwork=artwork)],
            [artwork],
        )
        _, compiled, _ = capsule.decode_gdt(result["gdt"])
        self.assertEqual(compiled[:3], changed)

    def test_hidden_empty_sprite_row_is_preserved(self):
        gdt, resources, rows = baseline()
        changed = list(rows)
        changed[0] = activity_row(0, 0, "")
        changed_gdt = capsule.encode_gdt(179, changed, 987654)
        changed_resources = dict(resources)
        changed_resources["gdrs/0099.gdt"] = changed_gdt
        artwork = asset()
        result = activity_client.compile_activity_client(
            changed_gdt,
            changed_resources,
            [machine(artwork=artwork)],
            [artwork],
        )
        _, compiled, _ = capsule.decode_gdt(result["gdt"])
        self.assertEqual(compiled[0], changed[0])

    def test_validation_rejects_stale_artwork_currency_one_bool_and_discontinuous_grid(self):
        gdt, resources, rows = baseline()
        artwork = asset()
        stale = machine(artwork=artwork)
        stale["artwork_sha256"] = "f" * 64
        cases = (
            ("absent or stale", stale, artwork),
            ("must be 0 or 2", machine(currency=1, artwork=artwork), artwork),
            ("must be 0 or 2", machine(currency=True, artwork=artwork), artwork),
        )
        for message, configuration, supplied_asset in cases:
            with self.subTest(message=message), self.assertRaisesRegex(
                activity_client.ActivityClientError, message
            ):
                activity_client.compile_activity_client(
                    gdt, resources, [configuration], [supplied_asset]
                )

        malformed = list(rows)
        mutable = bytearray(malformed[1])
        struct.pack_into("<I", mutable, 0, 145999)
        malformed[1] = bytes(mutable)
        bad_gdt = capsule.encode_gdt(179, malformed, 987654)
        bad_resources = dict(resources)
        bad_resources["gdrs/0099.gdt"] = bad_gdt
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "grid IDs are not continuous"
        ):
            activity_client.compile_activity_client(
                bad_gdt,
                bad_resources,
                [machine(artwork=artwork)],
                [artwork],
            )

    def test_inputs_are_not_mutated_and_mismatched_provider_gdt_is_rejected(self):
        gdt, resources, _ = baseline()
        artwork = asset()
        configurations = [machine(artwork=artwork)]
        assets = [artwork]
        resources_before = copy.deepcopy(resources)
        configurations_before = copy.deepcopy(configurations)
        assets_before = copy.deepcopy(assets)
        activity_client.compile_activity_client(
            gdt, resources, configurations, assets
        )
        self.assertEqual(resources, resources_before)
        self.assertEqual(configurations, configurations_before)
        self.assertEqual(assets, assets_before)

        mismatched = dict(resources)
        mismatched["gdrs/0099.gdt"] = b"different"
        with self.assertRaisesRegex(
            activity_client.ActivityClientError, "different 0099.gdt"
        ):
            activity_client.compile_activity_client(
                gdt, mismatched, configurations, assets
            )


if __name__ == "__main__":
    unittest.main()
