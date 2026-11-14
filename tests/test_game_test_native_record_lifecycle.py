from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "game_test_native_record_lifecycle",
    ROOT / "tools/game_test_native_record_lifecycle.py",
)
lifecycle = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(lifecycle)


def native_record(identity: int, size: int, seed: int) -> bytes:
    raw = bytearray((seed + index * 17) & 0xFF for index in range(size))
    struct.pack_into("<I", raw, 0, identity)
    raw[4] = 0
    return bytes(raw)


def catalog_bytes(records: list[tuple[int, int, bytes]]) -> bytes:
    parts = []
    for identity, kind, raw in sorted(records):
        parts.append(
            lifecycle.CATALOG_ENTRY.pack(
                kind, identity, len(raw), lifecycle._fnv1a32(raw), 0, 0
            )
            + raw
        )
    payload = b"".join(parts)
    return lifecycle.CATALOG_HEADER.pack(
        lifecycle.CATALOG_MAGIC,
        lifecycle.CATALOG_VERSION,
        lifecycle.CATALOG_HEADER.size,
        len(parts),
        len(payload),
        lifecycle._fnv1a32(payload),
        0,
    ) + payload


class NativeRecordLifecycleReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.catalog_path = self.root / lifecycle.CATALOG_RELATIVE
        self.receipt_path = self.root / lifecycle.RECEIPT_RELATIVE
        self.catalog_path.parent.mkdir(parents=True)
        self.receipt_path.parent.mkdir(parents=True)
        self.records = [
            (100_001, 2, native_record(100_001, 4976, 1)),
            (100_002, 10, native_record(100_002, 1871, 2)),
            (100_003, 11, native_record(100_003, 732, 3)),
            (100_004, 13, native_record(100_004, 1132, 4)),
            (100_006, 79, native_record(100_006, 89, 6)),
            (100_005, 0x21, native_record(100_005, 1197, 5)),
        ]
        self.ids = [100_001, 100_002, 100_003, 100_004,100_006]
        self.run_id = "fixture-native-lifecycle"
        self.write_catalog(self.records)
        self.receipt = self.make_receipt()
        self.write_receipt(self.receipt)

    def tearDown(self):
        self.temporary.cleanup()

    def write_catalog(self, records):
        raw = catalog_bytes(records)
        self.catalog_path.write_bytes(raw)
        self.catalog_sha256 = hashlib.sha256(raw).hexdigest()

    def make_receipt(self):
        by_id = {identity: (kind, raw) for identity, kind, raw in self.records}
        rows = []
        address = 0x1000
        for identity in self.ids:
            kind, raw = by_id[identity]
            pointers = {}
            for name in lifecycle.POINTER_KEYS:
                if kind==79 and name in ('hash_node','hash_value'):
                    pointers[name]='0x00000000';continue
                pointers[name] = f"0x{address:08x}"
                address += 0x10
            rows.append(
                {
                    "id": identity,
                    "kind": kind,
                    "integrity_mode":'native-not-indexed'if kind==79 else'native-hash',
                    "record_size": len(raw),
                    "record_hex": raw.hex(),
                    "pointers": pointers,
                    "captures": {
                        "record": 1,
                        "insert": 1,
                        "register": 1,
                        "integrity": 1,
                        "conflict": False,
                    },
                    "free_counts": {name:0 if kind==79 and name in ('hash_node','hash_value')else 1 for name in lifecycle.POINTER_KEYS},
                    "passed": True,
                }
            )
        return {
            "schema": "sdgo.native-record-lifecycle/2",
            "observer_scope": lifecycle.OBSERVER_SCOPE,
            "run_id": self.run_id,
            "expected_ids": self.ids,
            "clear_entered": 1,
            "clear_returned": 1,
            "app_final_entered": 1,
            "app_final_returned": 1,
            "singleton_null": True,
            "cross_record_pointers_distinct": True,
            "records": rows,
            "passed": True,
        }

    def write_receipt(self, receipt):
        self.receipt_path.write_text(
            json.dumps(receipt, separators=(",", ":")), encoding="utf-8"
        )

    def verify(self, **overrides):
        arguments = {
            "client_root": self.root,
            "ids": self.ids,
            "run_id": self.run_id,
            "expected_catalog_sha256": self.catalog_sha256,
        }
        arguments.update(overrides)
        return lifecycle.verify_receipt(**arguments)

    def test_complete_receipt_matches_every_sealed_native_record(self):
        result = self.verify()
        self.assertEqual(
            set(result),
            {
                "status",
                "receipt_sha256",
                "records",
                "observer_scope",
                "native_actor_lifecycle_accepted",
            },
        )
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["observer_scope"], lifecycle.OBSERVER_SCOPE)
        self.assertIs(result["native_actor_lifecycle_accepted"], False)
        self.assertEqual([row["id"] for row in result["records"]], self.ids)
        self.assertEqual(
            [row["kind"] for row in result["records"]], [2, 10, 11, 13,79]
        )
        self.assertEqual(
            result["receipt_sha256"],
            hashlib.sha256(self.receipt_path.read_bytes()).hexdigest(),
        )

    def test_rehashed_catalog_change_still_must_match_captured_record_bytes(self):
        changed = list(self.records)
        identity, kind, original = changed[2]
        raw = bytearray(original)
        raw[20] ^= 0x80
        changed[2] = (identity, kind, bytes(raw))
        self.write_catalog(changed)
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            self.verify()

    def test_catalog_must_match_the_passed_sealed_sha256(self):
        with self.assertRaisesRegex(ValueError, "sealed catalog SHA-256"):
            self.verify(expected_catalog_sha256="0" * 64)

    def test_receipt_must_match_the_exact_run(self):
        changed = copy.deepcopy(self.receipt)
        changed["run_id"] = "another-run"
        self.write_receipt(changed)
        with self.assertRaisesRegex(ValueError, "run_id"):
            self.verify()

    def test_missing_or_duplicate_record_id_is_rejected(self):
        changed = copy.deepcopy(self.receipt)
        changed["records"].pop()
        self.write_receipt(changed)
        with self.assertRaisesRegex(ValueError, "record count"):
            self.verify()

        changed = copy.deepcopy(self.receipt)
        changed["records"][-1]["id"] = changed["records"][0]["id"]
        self.write_receipt(changed)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.verify()

    def test_pointer_alias_across_records_is_rejected(self):
        changed = copy.deepcopy(self.receipt)
        changed["records"][1]["pointers"]["hash_value"] = (
            changed["records"][0]["pointers"]["raw"]
        )
        self.write_receipt(changed)
        with self.assertRaisesRegex(ValueError, "aliased"):
            self.verify()

    def test_unindexed_kind_requires_positive_absence_and_exact_ownership(self):
        for field,value in [('integrity_mode','native-hash'),('hash_pointer','0x000abcde'),('hash_free',1),('integrity_capture',0)]:
            changed=copy.deepcopy(self.receipt);row=changed['records'][-1]
            if field=='integrity_mode':row[field]=value
            elif field=='hash_pointer':row['pointers']['hash_node']=value
            elif field=='hash_free':row['free_counts']['hash_node']=value
            else:row['captures']['integrity']=value
            self.write_receipt(changed)
            with self.subTest(field=field),self.assertRaises(ValueError):self.verify()

    def test_legacy_hashed_receipts_remain_valid_but_cannot_claim_kind79(self):
        changed=copy.deepcopy(self.receipt);changed['schema']='sdgo.native-record-lifecycle/1'
        for row in changed['records']:row.pop('integrity_mode')
        self.write_receipt(changed)
        with self.assertRaisesRegex(ValueError,'schema2'):self.verify()
        changed['records']=changed['records'][:-1];changed['expected_ids']=changed['expected_ids'][:-1]
        self.write_receipt(changed)
        self.assertEqual('pass',self.verify(ids=self.ids[:-1])['status'])

    def test_missing_double_or_boolean_free_count_is_rejected(self):
        for value, pattern in ((None, "fields"), (2, "equal one"), (True, "integer")):
            with self.subTest(value=value):
                changed = copy.deepcopy(self.receipt)
                if value is None:
                    del changed["records"][0]["free_counts"]["raw"]
                else:
                    changed["records"][0]["free_counts"]["raw"] = value
                self.write_receipt(changed)
                with self.assertRaisesRegex(ValueError, pattern):
                    self.verify()

    def test_truncated_receipt_or_catalog_never_produces_a_pass(self):
        receipt = self.receipt_path.read_bytes()
        self.receipt_path.write_bytes(receipt[:-11])
        with self.assertRaises(ValueError):
            self.verify()

        self.write_receipt(self.receipt)
        catalog = self.catalog_path.read_bytes()
        self.catalog_path.write_bytes(catalog[:-1])
        truncated_hash = hashlib.sha256(self.catalog_path.read_bytes()).hexdigest()
        with self.assertRaises(ValueError):
            self.verify(expected_catalog_sha256=truncated_hash)


if __name__ == "__main__":
    unittest.main()
