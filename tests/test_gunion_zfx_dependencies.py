import hashlib
import struct
import unittest

from tools.gunion.zfx_dependencies import (
    CONSUMER_CONTRACT_ID,
    parse_dependencies,
    union_native_zfx_source_profile,
)


def emitter(emitter_type: int, texture_slot: bytes) -> bytes:
    if len(texture_slot) > 32:
        raise ValueError("fixture texture slot too long")
    fixed = bytearray(550)
    struct.pack_into("<I", fixed, 0, emitter_type)
    fixed[4 : 4 + len(texture_slot)] = texture_slot
    variable = b"".join(struct.pack("<I", 0) for _ in range(4))
    variable += b"\0" + struct.pack("<I", 0)
    return bytes(fixed) + variable


def zfx(*emitters: tuple[int, bytes], version: int = 5) -> bytes:
    data = bytearray(b"ZOAEFFECT\0" + b"\0" + bytes([version]))
    data.extend(bytes(52 + 41))
    data.extend(struct.pack("<I", 0))
    data.extend(struct.pack("<I", len(emitters)))
    mesh_count = 0
    for emitter_type, texture_slot in emitters:
        data.extend(emitter(emitter_type, texture_slot))
        mesh_count += emitter_type == 2
    if version > 1:
        data.append(0)
    if version > 2:
        for index in range(mesh_count):
            data.extend(b"\0\0\0")
            name = f"{1000 + index}.mod".encode("ascii")
            data.extend(name + bytes(32 - len(name)))
    if version > 3:
        data.append(0)
    return bytes(data)


def profile(data: bytes):
    return union_native_zfx_source_profile(
        "fxrs/fixture.zfx", hashlib.sha256(data).hexdigest()
    )


class ZfxDependencyTest(unittest.TestCase):
    def test_leading_nul_excludes_post_null_texture_scanner_candidate(self):
        data = zfx(
            (1, b"0022.txr\0"),
            (2, b"\0" + b"022.txr\0"),
        )
        result = parse_dependencies(data, profile(data))

        self.assertEqual(["txrs/0022.txr"], [
            row["path"] for row in result["references"]
        ])
        self.assertEqual(1, len(result["candidateReferences"]))
        candidate = result["candidateReferences"][0]
        self.assertEqual("022.txr", candidate["literal"])
        self.assertEqual(
            "post-nul-storage-not-native-string", candidate["disposition"]
        )
        self.assertEqual(
            candidate["carrier"]["fieldOffset"],
            candidate["carrier"]["terminatorOffset"],
        )
        self.assertEqual(
            candidate["carrier"]["fieldOffset"] + 1,
            candidate["carrier"]["candidateOffset"],
        )

    def test_typed_three_digit_name_is_preserved_without_padding(self):
        data = zfx((1, b"022.txr\0"))
        result = parse_dependencies(data, profile(data))

        self.assertEqual("txrs/022.txr", result["references"][0]["path"])
        self.assertFalse(result["references"][0]["consumer"]["zeroPadding"])
        self.assertEqual(
            "literal-preserved",
            result["consumerContract"]["nativeConsumer"]["normalization"][
                "numericWidth"
            ],
        )

    def test_carrier_reports_exact_offset_encoding_and_length(self):
        data = zfx((1, b"0027.txr\0"))
        result = parse_dependencies(data, profile(data))
        carrier = result["references"][0]["carrier"]

        self.assertEqual(117, carrier["offset"])
        self.assertEqual(32, carrier["length"])
        self.assertEqual("windows-narrow-c-string", carrier["encoding"])
        self.assertEqual(125, carrier["terminatorOffset"])
        self.assertEqual(CONSUMER_CONTRACT_ID, result["consumerContract"]["id"])

    def test_mesh_trailer_is_consumed_without_promoting_model_as_texture(self):
        data = zfx((2, b"\0" + b"027.txr\0"))
        result = parse_dependencies(data, profile(data))

        self.assertEqual([], result["references"])
        self.assertEqual("1000.mod", result["trailer"]["meshCarriers"][0][
            "modelCarrier"
        ]["value"])
        self.assertEqual(0, result["format"]["trailingBytes"])

    def test_profile_hash_mismatch_fails_closed(self):
        data = zfx((1, b"0022.txr\0"))
        wrong = union_native_zfx_source_profile(
            "fxrs/fixture.zfx", hashlib.sha256(b"other").hexdigest()
        )
        with self.assertRaisesRegex(ValueError, "do not match"):
            parse_dependencies(data, wrong)

    def test_truncated_variable_vector_fails_closed(self):
        data = zfx((1, b"0022.txr\0"))
        damaged = bytearray(data)
        first_emitter = 113
        struct.pack_into("<I", damaged, first_emitter + 550, 0x7FFFFFFF)
        damaged = bytes(damaged)
        with self.assertRaisesRegex(ValueError, "byte budget"):
            parse_dependencies(damaged, profile(damaged))

    def test_magic_and_version_are_validated(self):
        data = bytearray(zfx((1, b"0022.txr\0")))
        data[0] = ord("X")
        data = bytes(data)
        with self.assertRaisesRegex(ValueError, "magic"):
            parse_dependencies(data, profile(data))

        data = bytearray(zfx((1, b"0022.txr\0")))
        data[0x0B] = 6
        data = bytes(data)
        with self.assertRaisesRegex(ValueError, "version"):
            parse_dependencies(data, profile(data))

