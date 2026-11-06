"""Synthetic binary contract tests; no original mission rows are fixtures."""
import struct
import unittest
import zlib

from manager.engines.mission_behavior import profile_codec as codec


class MissionProfileCodecTests(unittest.TestCase):
    @staticmethod
    def row(identity=1, **changes):
        row = dict(zip(codec.FIELDS, (identity, 100, 1, 1, 200, -10, 20, 30,
                                     0, 0, 0, 1, 1, 0)))
        row.update(changes)
        return row

    def bundle(self):
        return codec.compile_reference(
            [self.row()], [self.row(NpcID=201, px=-25, AfterTime=7)],
            codec.OC_BINDINGS)

    @staticmethod
    def rewrite_header(raw, **values):
        names = ('magic', 'version', 'hsize', 'flags', 'esize', 'count',
                 'psize', 'crc', 'generation', 'source', 'baseline', 'label')
        header = dict(zip(names, codec.HEADER.unpack_from(raw)))
        header.update(values)
        return codec.HEADER.pack(*(header[name] for name in names)) + raw[codec.HEADER.size:]

    def mutate_payload(self, callback):
        raw = bytearray(self.bundle())
        callback(raw)
        return self.rewrite_header(raw, crc=zlib.crc32(raw[codec.HEADER.size:]))

    def test_signed_coordinates_and_supported_delta_roundtrip(self):
        decoded = codec.decode_bundle(self.bundle())
        self.assertEqual(decoded['entry_count'], 1)
        self.assertEqual(decoded['rows'][0]['baseline']['px'], -10)
        self.assertEqual(decoded['rows'][0]['oc']['px'], -25)
        self.assertEqual(decoded['rows'][0]['oc']['NpcID'], 201)

    def test_empty_oc_profile_cannot_report_success(self):
        with self.assertRaises(ValueError):
            codec.compile_reference([self.row()], [self.row()], codec.OC_BINDINGS)
        raw = self.rewrite_header(self.bundle(), flags=1, count=0, psize=0, crc=0)
        with self.assertRaises(ValueError):
            codec.decode_bundle(raw[:codec.HEADER.size])

    def test_version_lengths_generation_and_checksum_are_enforced(self):
        raw = self.bundle()
        for changes in ({'version': 2}, {'hsize': 129}, {'flags': 7},
                        {'esize': 117}, {'count': codec.MAX_ENTRIES + 1},
                        {'psize': 0}, {'generation': 2}, {'crc': 0}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                codec.decode_bundle(self.rewrite_header(raw, **changes))
        for invalid in (raw[:-1], raw + b'\0'):
            with self.assertRaises(ValueError):
                codec.decode_bundle(invalid)

    def test_duplicate_rows_unsupported_or_redundant_masks_are_refused(self):
        raw = self.bundle()
        duplicate = raw + raw[codec.HEADER.size:]
        payload = duplicate[codec.HEADER.size:]
        duplicate = self.rewrite_header(duplicate, count=2, psize=len(payload),
                                        crc=zlib.crc32(payload))
        with self.assertRaises(ValueError):
            codec.decode_bundle(duplicate)
        for mask in (0, 0x40, 0x3f):
            broken = self.mutate_payload(
                lambda data: struct.pack_into('<I', data, codec.HEADER.size, mask))
            with self.subTest(mask=mask), self.assertRaises(ValueError):
                codec.decode_bundle(broken)

    def test_immutable_fields_nonintegral_coordinates_and_wire_limits_are_refused(self):
        for changes in ({'MissionID': 101}, {'MissionPart': 2}, {'NpcParty': 2},
                        {'ManualID': 1}, {'AbsTime': 1}, {'ZoneIndex': 1},
                        {'px': 0.5}, {'AfterTime': 65536}, {'CreateLimit': 256}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                codec.compile_reference([self.row()], [self.row(**changes)],
                                        codec.OC_BINDINGS)


if __name__ == '__main__':
    unittest.main()
