import hashlib
import struct
import unittest

from owner.units.profiles import skill_target_1132 as profile


def source_record(skill_id=71001, dispatch=2):
    raw = bytearray((index * 37 + 11) & 0xFF for index in range(882))
    struct.pack_into("<I", raw, 0, skill_id)
    raw[881] = dispatch
    return bytes(raw)


def projected(source):
    return (
        source[:105]
        + bytes(50)
        + source[105:881]
        + bytes(200)
        + source[881:882]
    )


class UnitImportSkillTarget1132Tests(unittest.TestCase):
    def test_exact_projection_is_reversible_but_registration_stays_blocked(self):
        source = source_record()
        target = projected(source)
        result = profile.validate_projected_record(
            source=source,
            target=target,
            skill_id=71001,
            source_row=17,
            source_sha256=hashlib.sha256(source).hexdigest(),
            target_sha256=hashlib.sha256(target).hexdigest(),
        )

        self.assertEqual(result["target"]["size"], 1132)
        self.assertEqual(target[1131], source[881])
        self.assertEqual(profile.recover_source_record(target), source)
        self.assertTrue(result["dispatch"]["supported"])
        self.assertFalse(
            result["admission"]["dispatch_compatibility_blocked"]
        )
        self.assertTrue(result["admission"]["native_registration_blocked"])
        self.assertFalse(result["admission"]["native_runtime_accepted"])

    def test_unknown_dispatch_is_preserved_and_fails_closed(self):
        source = source_record(71097, 165)
        target = projected(source)
        result = profile.validate_projected_record(
            source=source,
            target=target,
            skill_id=71097,
            source_row=362,
            source_sha256=hashlib.sha256(source).hexdigest(),
            target_sha256=hashlib.sha256(target).hexdigest(),
        )

        self.assertEqual(target[1131], 165)
        self.assertFalse(result["dispatch"]["supported"])
        self.assertTrue(
            result["admission"]["dispatch_compatibility_blocked"]
        )
        self.assertIn(
            "dispatch value is outside the observed target branch domain",
            result["admission"]["blockers"],
        )

    def test_target_only_capacity_and_hash_drift_are_rejected(self):
        source = source_record()
        target = bytearray(projected(source))
        target[1000] = 1
        with self.assertRaisesRegex(ValueError, "do not match"):
            profile.validate_projected_record(
                source=source,
                target=bytes(target),
                skill_id=71001,
                source_row=17,
                source_sha256=hashlib.sha256(source).hexdigest(),
                target_sha256=hashlib.sha256(target).hexdigest(),
            )

        target = projected(source)
        with self.assertRaisesRegex(ValueError, "target record SHA-256"):
            profile.validate_projected_record(
                source=source,
                target=target,
                skill_id=71001,
                source_row=17,
                source_sha256=hashlib.sha256(source).hexdigest(),
                target_sha256="0" * 64,
            )

    def test_host_witness_is_exact(self):
        profile.validate_host_witness(
            gonline_sha256=profile.HOST_GONLINE_SHA256,
            raw_0253_sha256=profile.HOST_0253_RAW_SHA256,
            decoded_logical_0253_sha256=(
                profile.HOST_0253_DECODED_LOGICAL_SHA256
            ),
        )
        with self.assertRaisesRegex(ValueError, "raw_0253"):
            profile.validate_host_witness(
                gonline_sha256=profile.HOST_GONLINE_SHA256,
                raw_0253_sha256="0" * 64,
                decoded_logical_0253_sha256=(
                    profile.HOST_0253_DECODED_LOGICAL_SHA256
                ),
            )


if __name__ == "__main__":
    unittest.main()
