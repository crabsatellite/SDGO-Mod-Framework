import struct
import unittest

from tools.gunion.minecraft_fields import decode_record, known_unit_fields, core_table_rows


def unit():
    data = bytearray(4976)
    struct.pack_into("<I", data, 0, 12345)
    struct.pack_into("<I", data, 321, 14500)
    data[330:339] = bytes([50, 0, 0, 2, 1, 3, 4, 5, 6])
    data[584] = 2
    data[4033] = 1
    text = "骨;Upper;Lower".encode("utf-16le")
    data[4034:4034 + len(text)] = text
    text = "arm;arm; Blade".encode("utf-16le")
    data[0x902:0x902 + len(text)] = text
    text = "Vert; Auxiliary;~tail".encode("utf-16le")
    data[0xE7A:0xE7A + len(text)] = text
    text = "$weapon;~excluded".encode("utf-16le")
    data[0x326:0x326 + len(text)] = text
    # Invalid UTF16 in padding must not corrupt a terminated source name.
    data[5:9] = b"A\0\0\0"
    data[15:17] = b"\0\xd8"
    return decode_record(bytes(data), "unit", 11)


def weapon_fields(row=4, mode=1, threshold=205):
    data = bytearray(1871)
    struct.pack_into("<I", data, 0, 789)
    data[123] = mode
    struct.pack_into("<h", data, 126, threshold)
    struct.pack_into("<I", data, 160, 0x11223344)
    # Correct Union field starts at A4, not the 1867-byte baseline's A0.
    data[164:172] = "fxrs".encode("utf-16le")
    return decode_record(bytes(data), "weapon", row)


class FieldTests(unittest.TestCase):
    def test_preserves_overlapping_carriers_and_literal_node_list(self):
        d = unit()
        f = d["fields"]
        self.assertEqual(f["name"]["value"], "A")
        self.assertEqual(f["shieldRateByteRaw"]["value"], 50)
        self.assertEqual(f["shieldRateCarrierU32Raw"]["value"], 0x02000032)
        self.assertEqual(f["shieldDirectionModesRaw"]["value"], [2, 1, 3, 4, 5, 6])
        projected = known_unit_fields(d)
        self.assertEqual(projected["stats"]["hp"], 580)
        self.assertEqual(projected["vfxRuntime"]["sourceWeaponAttachmentNodes"][0], "arm;arm; Blade")
        self.assertNotIn("runtimeWeaponAttachmentNodes", projected["vfxRuntime"])
        self.assertEqual(f["selectionHideNodes"]["value"], "Vert; Auxiliary;~tail")
        self.assertEqual(f["selectionHideNodes"]["offset"], 0xE7A)
        self.assertEqual(f["weaponDisplayGroup0"]["value"], "$weapon;~excluded")

    def test_exact_reload_clock_and_union_insert_are_not_rounded_or_discarded(self):
        w = weapon_fields()
        self.assertEqual(w["fields"]["unionInsertedU32RawA0"]["value"], 0x11223344)
        self.assertEqual(w["fields"]["muzzleEffectPath"]["value"], "fxrs")
        result = core_table_rows([unit()], [w], unit_evidence=["unit"], weapon_evidence=["weapon"])
        rows = {r["table"]: r["value"] for r in result["rows"]}
        self.assertEqual(rows["runtime/weapon_reload.json"], {"modeRaw": 1, "thresholdRaw": 205})
        self.assertEqual(rows["runtime/weapon_banks.json"]["defaultSlot"], 1)
        self.assertEqual(rows["runtime/animation_partitions.json"]["roots"], ["骨", "Upper", "Lower"])

    def test_conflicting_source_variants_and_negative_reload_are_not_published(self):
        result = core_table_rows([], [weapon_fields(), weapon_fields(5, threshold=200)],
                                 unit_evidence=["unit"], weapon_evidence=["weapon"])
        self.assertEqual(result["rows"], [])
        self.assertEqual(result["sourceRowVariants"][0]["sourceRows"], [4, 5])
        self.assertEqual(result["blocked"][0]["reason"], "source-row-reload-value-conflict")
        result = core_table_rows([], [weapon_fields(threshold=-1)], unit_evidence=["u"], weapon_evidence=["w"])
        self.assertEqual(result["rows"], [])
        result = core_table_rows([], [weapon_fields()], unit_evidence=["u"], weapon_evidence=["w"],
                                 baseline_tables={"runtime/weapon_reload.json": {"789": {"modeRaw": 0, "thresholdRaw": 1}}})
        self.assertEqual(result["blocked"][0]["reason"], "existing-domain-value-conflict")


if __name__ == "__main__":
    unittest.main()
