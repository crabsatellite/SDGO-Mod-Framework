import struct
import unittest

from tools.gunion.obs_edge import (
    ACTION_TEXT_SIZE,
    EVENT_SIZE,
    KUDIZ_MATERIAL_ENTRY_SIZE,
    ObsEdgeError,
    parse_obs_edge,
)


def text(value: str) -> bytes:
    encoded = value.encode("utf-16le")
    if len(encoded) > ACTION_TEXT_SIZE:
        raise AssertionError(value)
    return encoded + bytes(ACTION_TEXT_SIZE - len(encoded))


def resource(type_id: int, path: str) -> bytes:
    encoded = path.encode("utf-16le")
    if len(encoded) > 0x40:
        raise AssertionError(path)
    return struct.pack("<i", type_id) + encoded + bytes(0x40 - len(encoded))


def decoded_action(name: str, animation: str, body: bytes) -> bytearray:
    return bytearray(struct.pack("<i", 0) + text(name) + text(animation) + body)


def global_action(body: bytes) -> bytearray:
    return bytearray(struct.pack("<i", 0) + text("Global_Script") + body)


def short_action(name: str, animation: str) -> bytearray:
    return bytearray(struct.pack("<i", 0) + text(name) + text(animation))


def fixture(material_count: int, actions: list[bytearray], trailing: bytes = b"") -> bytes:
    prefix = bytearray(struct.pack("<i", 0xC6E9))
    kudiz_offset = len(prefix)
    prefix += struct.pack("<i", material_count)
    prefix += bytes(material_count * KUDIZ_MATERIAL_ENTRY_SIZE)
    prefix += struct.pack("<i", len(actions))
    for action in actions:
        start = len(prefix)
        prefix += action
        struct.pack_into("<i", prefix, start, len(prefix))
    prefix += trailing
    return bytes(prefix)


class ObsEdgeTest(unittest.TestCase):
    def test_material_count_uses_source_boundary_instead_of_legacy_64_cap(self):
        source = fixture(119, [decoded_action("wait", "anrs\\1.ani", b"\0")])
        report = parse_obs_edge(source, 4)
        self.assertEqual(report.material_count, 119)
        self.assertEqual(report.action_count, 1)
        self.assertEqual(report.actions[0].disposition, "decoded-ordinary")
        self.assertEqual(report.actions[0].has_events_raw, 0)

    def test_filter_skipped_short_row_keeps_physical_order_and_following_events(self):
        event = bytes(range(EVENT_SIZE))
        body = (
            b"\1"
            + struct.pack("<i", 1)
            + resource(0x30000, "sdrs\\0047.zsd")
            + struct.pack("<i", 1)
            + struct.pack("<ii", 12, 1)
            + event
        )
        actions = [
            global_action(b"\0"),
            short_action("win", "anrs\\18069.ani"),
            decoded_action("btk02_jump", "anrs\\6897.ani", body),
        ]
        source = fixture(0, actions, b"\0\0\0\0")
        report = parse_obs_edge(source, 4)
        self.assertEqual(report.action_count, 3)
        self.assertEqual(report.decoded_action_count, 2)
        self.assertEqual(report.filter_skipped_action_count, 1)
        self.assertEqual(report.total_event_count, 1)
        skipped = report.actions[1]
        self.assertEqual(skipped.action_name, "win")
        self.assertEqual(skipped.animation_path, "anrs\\18069.ani")
        self.assertEqual(skipped.disposition, "native-filter-skipped-body-omitted")
        self.assertEqual(skipped.native_map_admission, "requires-filter-exclusion")
        self.assertIsNone(skipped.has_events_raw)
        self.assertIn("resource count exceeds source boundary", skipped.body_probe_error)
        following = report.actions[2]
        self.assertEqual(following.action_name, "btk02_jump")
        self.assertEqual(following.event_count, 1)
        self.assertEqual(following.resources[0].path, "sdrs\\0047.zsd")
        self.assertEqual(report.trailing_byte_count, 4)

    def test_successful_body_decode_wins_even_when_ordinary_hint_points_to_body(self):
        action = decoded_action("idle", "anrs\\2.ani", b"\0")
        source = bytearray(fixture(0, [action]))
        entry_offset = 12
        body_offset = entry_offset + 4 + ACTION_TEXT_SIZE * 2
        struct.pack_into("<i", source, entry_offset, body_offset)
        report = parse_obs_edge(bytes(source), 4)
        self.assertEqual(report.actions[0].disposition, "decoded-ordinary")
        self.assertEqual(report.actions[0].has_events_raw, 0)
        self.assertEqual(report.actions[0].declared_end, body_offset)
        self.assertEqual(report.actions[0].decoded_end, body_offset + 1)

    def test_body_failure_without_exact_filter_boundary_is_not_hidden(self):
        action = decoded_action("broken", "anrs\\3.ani", b"\1" + struct.pack("<i", 9999))
        source = fixture(0, [action])
        with self.assertRaisesRegex(ObsEdgeError, "body decode failed"):
            parse_obs_edge(source, 4)

    def test_global_entry_end_must_cover_its_decoded_body(self):
        source = bytearray(fixture(0, [global_action(b"\0")]))
        entry_offset = 12
        struct.pack_into("<i", source, entry_offset, entry_offset + 4)
        with self.assertRaisesRegex(ObsEdgeError, "Global_Script declared end"):
            parse_obs_edge(bytes(source), 4)


if __name__ == "__main__":
    unittest.main()
