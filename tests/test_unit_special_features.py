"""Kind-12 special-feature records travel in the native unit catalog beside the weapons that name them."""
import struct
import unittest

from owner.units import compiler


def record(kind, identity, size):
    raw = bytearray(size)
    struct.pack_into('<I', raw, 0, identity)
    raw[5:5 + len('Feature'.encode('utf-16-le'))] = 'Feature'.encode('utf-16-le')
    return bytes(raw)


class SpecialFeatureCatalog(unittest.TestCase):
    def test_special_feature_record_is_published_with_its_weapon(self):
        weapon = bytearray(record(10, 61165, 1871))
        struct.pack_into('<I', weapon, 132, 59051)
        payload = compiler.native_catalog([(10, 61165, bytes(weapon)), (12, 59051, record(12, 59051, 1559))])
        magic, version, header, count = struct.unpack_from('<8s3I', payload)
        self.assertEqual((magic, count), (b'SDGOUN1\0', 2))
        kind, identity, size = struct.unpack_from('<3I', payload, header)
        self.assertEqual((kind, identity, size), (12, 59051, 1559))

    def test_special_feature_needs_the_native_length(self):
        for size in (1554, 1871):
            with self.assertRaises(ValueError):
                compiler.native_catalog([(12, 59051, record(12, 59051, size))])


if __name__ == '__main__':
    unittest.main()
