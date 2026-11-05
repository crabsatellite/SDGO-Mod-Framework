import hashlib
import struct
import unittest

from owner.units.profiles import funnel_target_732 as profile


def source_record():
    raw = bytearray((i * 37 + 11) % 256 for i in range(732))
    struct.pack_into('<I', raw, 0, 51000)
    raw[4] = 0
    return bytes(raw)


def validate(source, target, **changes):
    values = dict(source=source, target=target, source_id=51000,
                  target_id=9_100_001, source_row=0,
                  source_sha256=hashlib.sha256(source).hexdigest(),
                  target_sha256=hashlib.sha256(target).hexdigest())
    values.update(changes)
    return profile.validate_record(**values)


class FunnelProfile(unittest.TestCase):
    def test_relocation_preserves_all_opaque_fields_and_never_authorizes_registration(self):
        source = source_record()
        target = profile.prepare_record(source, target_id=9_100_001)
        result = validate(source, target)
        self.assertEqual(source[4:], target[4:])
        self.assertEqual(source, profile.prepare_record(target, target_id=51000))
        self.assertEqual(732, result['target']['size'])
        self.assertTrue(result['unknown_payload_preserved'])
        self.assertTrue(result['admission']['native_registration_blocked'])
        self.assertFalse(result['admission']['native_runtime_accepted'])

    def test_identity_preserving_transport_is_supported(self):
        source = source_record()
        result = validate(source, source, target_id=51000)
        self.assertEqual(result['source']['sha256'], result['target']['sha256'])

    def test_rehashed_payload_change_still_fails(self):
        source = source_record()
        target = bytearray(profile.prepare_record(source, target_id=9_100_001))
        for offset in (5, 105, 316, 575, 731):
            changed = bytearray(target); changed[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'outside the identity'):
                validate(source, bytes(changed))

    def test_incomplete_records_and_nonzero_security_are_rejected(self):
        source = source_record()
        bad_security = bytearray(source); bad_security[4] = 1
        for invalid in (source[:-1], source+b'\0', bytes(1132), bytes(bad_security), bytearray(source)):
            with self.assertRaises(ValueError):
                profile.prepare_record(invalid, target_id=9_100_001)

    def test_identity_row_and_hash_bindings_are_exact(self):
        source = source_record(); target = profile.prepare_record(source, target_id=9_100_001)
        for patch in ({'source_id':51001}, {'target_id':9_100_002}, {'source_row':-1},
                      {'source_row':True}, {'source_sha256':'0'*64},
                      {'target_sha256':'F'*64}):
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                validate(source, target, **patch)
        for identity in (0, True, -1, 10_000_000):
            with self.assertRaises(ValueError):
                profile.prepare_record(source, target_id=identity)


if __name__ == '__main__':
    unittest.main()
