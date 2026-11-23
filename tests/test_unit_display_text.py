"""Authoring rejects the legacy record that disables stock localization."""
from pathlib import Path
import struct
import tempfile
import unittest

from owner.units import display_text
from owner.units.codec import decode_unit
from owner.units.compiler import fnv, native_catalog
from owner.units.batch_server import CATALOG, _catalog_records
from unit_import_support import unit


def legacy_catalog(kind, raw):
    identity = struct.unpack_from('<I', raw)[0]
    payload = struct.pack('<6I', kind, identity, len(raw), fnv(raw), 0, 0) + raw
    return struct.pack('<8s6I', b'SDGOUN1\0', 1, 32, 1, len(payload), fnv(payload), 0) + payload


class DisplayTextTests(unittest.TestCase):
    def test_full_secondary_name_is_refused_before_import_and_compile(self):
        for offset in (105, 205):
            raw = bytearray(unit())
            raw[offset:offset + 100] = b'X\0' * 50
            before = bytes(raw)
            for operation in (lambda: decode_unit(before), lambda: native_catalog([(2, 90001, before)])):
                with self.assertRaisesRegex(ValueError, f'kind=2 id=90001 .*offset={offset} capacity=50'):
                    operation()
            self.assertEqual(before, bytes(raw))

    def test_last_slot_is_reserved_for_nul_and_counted_as_utf16_units(self):
        for text in ('X' * 49, '\U0001f680' * 24 + 'X'):
            raw = bytearray(unit())
            value = text.encode('utf-16-le')
            raw[105:205] = value.ljust(100, b'\0')
            self.assertEqual([], display_text.issues(2, 90001, raw))
            self.assertTrue(native_catalog([(2, 90001, bytes(raw))]))
        raw[105:205] = ('\U0001f680' * 25).encode('utf-16-le')
        with self.assertRaisesRegex(ValueError, 'offset=105'):
            display_text.validate(2, 90001, raw)

    def test_all_protected_fields_are_checked_without_reading_weapon_numeric_data(self):
        sizes = {2: 4976, 10: 1871, 13: 1132, 34: 614}
        for kind, specs in display_text.FIELDS.items():
            for offset, capacity, field in specs:
                with self.subTest(kind=kind, field=field):
                    raw = bytearray(sizes[kind])
                    struct.pack_into('<I', raw, 0, 90001)
                    raw[offset:offset + capacity * 2] = b'X\0' * capacity
                    with self.assertRaisesRegex(ValueError, f'offset={offset} capacity={capacity}'):
                        native_catalog([(kind, 90001, bytes(raw))])
        raw = bytearray(1871)
        raw[105:205] = b'X' * 100
        self.assertEqual([], display_text.issues(10, 90001, raw))

    def test_batch_import_rechecks_even_a_sealed_legacy_catalog_before_sql(self):
        raw = bytearray(unit())
        raw[105:205] = b'X\0' * 50
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / CATALOG
            path.parent.mkdir(parents=True)
            path.write_bytes(legacy_catalog(2, bytes(raw)))
            with self.assertRaisesRegex(ValueError, 'offset=105'):
                _catalog_records(Path(temporary))


if __name__ == '__main__':
    unittest.main()
