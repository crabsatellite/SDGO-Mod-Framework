import hashlib
import struct
import unittest

from owner.units.profiles import skill_target_1132 as profile


def source_record(skill_id=71001, dispatch=2, linked_skill_id=None):
    raw = bytearray((index * 37 + 11) & 0xFF for index in range(882))
    struct.pack_into("<I", raw, 0, skill_id)
    if linked_skill_id is not None:
        struct.pack_into("<I", raw, 304, linked_skill_id)
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


def relocated(source, target_skill_id, target_linked_skill_id=None):
    target = bytearray(projected(source))
    struct.pack_into("<I", target, 0, target_skill_id)
    if target_linked_skill_id is not None:
        struct.pack_into("<I", target, 354, target_linked_skill_id)
    return bytes(target)


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
        for skill_id, dispatch in ((71097, 165), (71135, 107)):
            with self.subTest(skill_id=skill_id, dispatch=dispatch):
                source = source_record(skill_id, dispatch)
                target = projected(source)
                result = profile.validate_projected_record(
                    source=source, target=target, skill_id=skill_id, source_row=362,
                    source_sha256=hashlib.sha256(source).hexdigest(),
                    target_sha256=hashlib.sha256(target).hexdigest(),
                )
                self.assertEqual(target[1131], dispatch)
                self.assertFalse(result["dispatch"]["supported"])
                self.assertTrue(result["admission"]["dispatch_compatibility_blocked"])
                self.assertTrue(result["admission"]["native_registration_blocked"])
                self.assertIn("dispatch value is outside the observed target branch domain",
                              result["admission"]["blockers"])

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


class UnitImportRelocatedSkillTarget1132Tests(unittest.TestCase):
    def validate(
        self,
        source,
        target,
        *,
        source_skill_id=70003,
        target_skill_id=9_000_313,
        **overrides,
    ):
        arguments = {
            "source": source,
            "target": target,
            "source_skill_id": source_skill_id,
            "target_skill_id": target_skill_id,
            "source_row": 17,
            "source_sha256": hashlib.sha256(source).hexdigest(),
            "target_sha256": hashlib.sha256(target).hexdigest(),
        }
        arguments.update(overrides)
        return profile.validate_relocated_record(**arguments)

    def test_relocated_id_above_u16_is_preserved_and_inverse_is_exact(self):
        source = source_record(70003, 2)
        target = relocated(source, 9_000_313)
        result = self.validate(source, target)

        self.assertEqual(result["schema"],
                         "sdgo.native-type13-relocated-record/1")
        self.assertEqual(result["source_skill_id"], 70003)
        self.assertEqual(result["target_skill_id"], 9_000_313)
        self.assertEqual(result["identity_rewrite"]["allowed_target_span"],
                         (0, 4))
        self.assertEqual(result["target"]["allowed_rewrite_span"], (0, 4))
        self.assertEqual(result["source"]["sha256"],
                         hashlib.sha256(source).hexdigest())
        self.assertEqual(result["projected"]["sha256"],
                         hashlib.sha256(projected(source)).hexdigest())
        self.assertEqual(result["target"]["sha256"],
                         hashlib.sha256(target).hexdigest())
        restored = bytearray(target)
        struct.pack_into("<I", restored, 0, 70003)
        self.assertEqual(profile.recover_source_record(bytes(restored)), source)
        self.assertTrue(result["admission"]["source_recoverable"])
        self.assertTrue(result["admission"]["native_registration_blocked"])
        self.assertFalse(result["admission"]["native_runtime_accepted"])
        original = profile.validate_projected_record(
            source=source,
            target=projected(source),
            skill_id=70003,
            source_row=17,
            source_sha256=hashlib.sha256(source).hexdigest(),
            target_sha256=hashlib.sha256(projected(source)).hexdigest(),
        )
        self.assertEqual(
            result["admission"]["blockers"],
            original["admission"]["blockers"],
        )

    def test_rehashed_unknown_target_byte_is_rejected(self):
        source = source_record(70003, 2)
        target = bytearray(relocated(source, 9_000_313))
        target[500] ^= 0x80
        target = bytes(target)
        with self.assertRaisesRegex(ValueError, "do not match"):
            self.validate(source, target)

    def test_source_target_ids_and_hashes_are_bound(self):
        source = source_record(70003, 2)
        target = relocated(source, 9_000_313)
        cases = (
            ({"source_skill_id": 70004}, "source skill identity"),
            ({"target_skill_id": 9_000_312}, "target skill identity"),
            ({"target_skill_id": True}, "target_skill_id"),
            ({"source_sha256": "0" * 64}, "source record SHA-256"),
            ({"target_sha256": "0" * 64}, "target record SHA-256"),
        )
        for overrides, message in cases:
            with self.subTest(overrides=overrides):
                with self.assertRaisesRegex(ValueError, message):
                    self.validate(source, target, **overrides)

    def test_unknown_dispatch_values_remain_preserved_and_blocked(self):
        for source_skill_id, dispatch, target_skill_id in (
            (71097, 165, 9_000_197),
            (71135, 107, 9_000_235),
        ):
            with self.subTest(source_skill_id=source_skill_id):
                source = source_record(source_skill_id, dispatch)
                target = relocated(source, target_skill_id)
                result = self.validate(
                    source,
                    target,
                    source_skill_id=source_skill_id,
                    target_skill_id=target_skill_id,
                )
                self.assertEqual(result["dispatch"]["value"], dispatch)
                self.assertEqual(target[1131], dispatch)
                self.assertFalse(result["dispatch"]["supported"])
                self.assertTrue(
                    result["admission"]["dispatch_compatibility_blocked"]
                )
                self.assertTrue(
                    result["admission"]["native_registration_blocked"]
                )
                self.assertFalse(
                    result["admission"]["native_runtime_accepted"]
                )
                self.assertIn(
                    "dispatch value is outside the observed target branch domain",
                    result["admission"]["blockers"],
                )

    def test_unknown_dispatch_cannot_be_rewritten_to_a_supported_value(self):
        for source_skill_id, source_dispatch, target_dispatch in (
            (71097, 165, 0),
            (71135, 107, 2),
        ):
            with self.subTest(source_skill_id=source_skill_id):
                source = source_record(source_skill_id, source_dispatch)
                target = bytearray(
                    relocated(source, 9_000_000 + source_skill_id)
                )
                target[1131] = target_dispatch
                target = bytes(target)
                with self.assertRaisesRegex(ValueError, "do not match"):
                    self.validate(
                        source,
                        target,
                        source_skill_id=source_skill_id,
                        target_skill_id=9_000_000 + source_skill_id,
                    )

    def test_explicit_linked_skill_rewrite_is_fixed_and_reversible(self):
        source = source_record(70003, 2, linked_skill_id=70004)
        target = relocated(
            source, 9_000_313, target_linked_skill_id=9_000_314
        )
        result = self.validate(
            source,
            target,
            linked_skill_rewrite={
                "source_skill_id": 70004,
                "target_skill_id": 9_000_314,
            },
        )

        link = result["linked_skill_rewrite"]
        self.assertEqual(link["record_kind"], 13)
        self.assertEqual(link["reference_kind"], 13)
        self.assertEqual(link["source_offset"], 304)
        self.assertEqual(link["target_offset"], 354)
        self.assertEqual(link["source_skill_id"], 70004)
        self.assertEqual(link["target_skill_id"], 9_000_314)
        self.assertEqual(
            result["target"]["allowed_rewrite_spans"],
            ((0, 4), (354, 358)),
        )
        restored = bytearray(target)
        struct.pack_into("<I", restored, 0, 70003)
        struct.pack_into("<I", restored, 354, 70004)
        self.assertEqual(bytes(restored), projected(source))
        self.assertEqual(profile.recover_source_record(bytes(restored)), source)
        self.assertTrue(result["admission"]["native_registration_blocked"])
        self.assertFalse(result["admission"]["native_runtime_accepted"])

    def test_linked_skill_change_requires_explicit_authorization(self):
        source = source_record(70003, 2, linked_skill_id=70004)
        target = relocated(
            source, 9_000_313, target_linked_skill_id=9_000_314
        )
        with self.assertRaisesRegex(ValueError, "do not match"):
            self.validate(source, target)

    def test_linked_skill_ids_are_bound_to_source_and_target(self):
        source = source_record(70003, 2, linked_skill_id=70004)
        target = relocated(
            source, 9_000_313, target_linked_skill_id=9_000_314
        )
        for rewrite, message in (
            (
                {
                    "source_skill_id": 70005,
                    "target_skill_id": 9_000_314,
                },
                "source linked-skill",
            ),
            (
                {
                    "source_skill_id": 70004,
                    "target_skill_id": 9_000_315,
                },
                "target linked-skill",
            ),
        ):
            with self.subTest(rewrite=rewrite):
                with self.assertRaisesRegex(ValueError, message):
                    self.validate(
                        source,
                        target,
                        linked_skill_rewrite=rewrite,
                    )

    def test_linked_skill_map_and_ids_are_strict(self):
        source = source_record(70003, 2, linked_skill_id=70004)
        target = relocated(
            source, 9_000_313, target_linked_skill_id=9_000_314
        )
        cases = (
            (True, "must contain only"),
            (
                {
                    "source_skill_id": 70004,
                    "target_skill_id": 9_000_314,
                    "offset": 354,
                },
                "must contain only",
            ),
            (
                {
                    "source_skill_id": True,
                    "target_skill_id": 9_000_314,
                },
                "source_skill_id",
            ),
            (
                {"source_skill_id": 70004, "target_skill_id": 0},
                "target_skill_id",
            ),
        )
        for rewrite, message in cases:
            with self.subTest(rewrite=rewrite):
                with self.assertRaisesRegex(ValueError, message):
                    self.validate(
                        source,
                        target,
                        linked_skill_rewrite=rewrite,
                    )

    def test_changes_after_the_fixed_link_field_remain_rejected(self):
        source = source_record(70003, 2, linked_skill_id=70004)
        target = bytearray(
            relocated(
                source, 9_000_313, target_linked_skill_id=9_000_314
            )
        )
        target[358] ^= 0x01
        target = bytes(target)
        with self.assertRaisesRegex(ValueError, "do not match"):
            self.validate(
                source,
                target,
                linked_skill_rewrite={
                    "source_skill_id": 70004,
                    "target_skill_id": 9_000_314,
                },
            )


if __name__ == "__main__":
    unittest.main()
