"""Offline contracts for the source-only native room-style preview fixture."""
from pathlib import Path
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "native_room_style_preview"
sys.dont_write_bytecode = True
CANONICAL_FILES = {
    "CMakeLists.txt",
    "room_style_preview.cpp",
    "room_style_preview_core.h",
    "room_style_preview_status.h",
    "room_style_preview_overlay.cpp",
    "room_style_preview_overlay.h",
    "offline_test.cpp",
    "generate_room_ui.py",
    "generate_mission_ui.py",
    "mod.ini",
    "manager.json",
    "symbols.fragment.ini",
    "README.md",
}


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


ROOM_GENERATOR = load_module(
    "native_room_style_preview_room_generator",
    FIXTURE / "generate_room_ui.py",
)
sys.modules["generate_room_ui"] = ROOM_GENERATOR
try:
    MISSION_GENERATOR = load_module(
        "native_room_style_preview_mission_generator",
        FIXTURE / "generate_mission_ui.py",
    )
finally:
    sys.modules.pop("generate_room_ui", None)


def render_record(module, name: str, entries: list[bytes]) -> bytes:
    record = bytearray(0xA8 + len(entries) * 48)
    module.set_wide_name(record, name)
    struct.pack_into("<I", record, 0xA4, len(entries))
    for index, entry in enumerate(entries):
        if len(entry) != 48:
            raise ValueError("render entry must contain 48 bytes")
        start = 0xA8 + index * 48
        record[start:start + 48] = entry
    return bytes(record)


class NativeRoomStylePreviewFixtureTests(unittest.TestCase):
    def test_fixture_contains_only_the_canonical_source_files(self):
        self.assertEqual(
            {path.name for path in FIXTURE.iterdir()},
            CANONICAL_FILES,
        )
        self.assertFalse(any(path.is_dir() for path in FIXTURE.iterdir()))

        combined = "\n".join(
            path.read_text(encoding="utf-8")
            for path in FIXTURE.iterdir()
            if path.is_file()
        )
        self.assertIsNone(re.search(r"\b[A-Za-z]:\\", combined))
        self.assertIsNone(
            re.search(
                r"\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
                r"[0-9a-f]{4}-[0-9a-f]{12}\b",
                combined,
                re.IGNORECASE,
            )
        )

    def test_normal_render_table_extends_complete_entries_16_to_24(self):
        entry = bytes(range(48))
        source_entries = [entry] * 16
        source = render_record(
            ROOM_GENERATOR,
            "Multi3DRender4",
            source_entries,
        )

        generated = ROOM_GENERATOR.extend_multi3d_render4(source)

        self.assertEqual(struct.unpack_from("<I", generated, 0xA4)[0], 24)
        self.assertEqual(len(generated), 0xA8 + 24 * 48)
        self.assertEqual(generated[0xA8:0xA8 + 16 * 48], source[0xA8:])
        self.assertEqual(generated[0xA8 + 16 * 48:], entry * 8)

    def test_normal_render_table_rejects_nonidentical_source_entries(self):
        entries = [bytes([index]) * 48 for index in range(16)]
        source = render_record(ROOM_GENERATOR, "Multi3DRender4", entries)
        with self.assertRaisesRegex(
            ROOM_GENERATOR.ResourceError,
            "source entries differ",
        ):
            ROOM_GENERATOR.extend_multi3d_render4(source)

    def test_mission_render_table_extends_complete_entries_4_to_6(self):
        source_entries = [bytes([index]) * 48 for index in range(4)]
        source = render_record(
            MISSION_GENERATOR,
            "Multi3DRender77",
            source_entries,
        )

        generated = MISSION_GENERATOR.repair_mission_render(source)

        self.assertEqual(struct.unpack_from("<I", generated, 0xA4)[0], 6)
        self.assertEqual(len(generated), 0xA8 + 6 * 48)
        self.assertEqual(generated[0xA8:0xA8 + 4 * 48], source[0xA8:])
        self.assertEqual(
            generated[0xA8 + 4 * 48:],
            source_entries[1] + source_entries[2],
        )

    def test_generators_resolve_their_sibling_import_from_an_external_cwd(self):
        with tempfile.TemporaryDirectory() as temporary:
            environment = os.environ.copy()
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            result = subprocess.run(
                [
                    sys.executable,
                    str(FIXTURE / "generate_mission_ui.py"),
                    "--help",
                ],
                cwd=temporary,
                env=environment,
                capture_output=True,
                text=True,
                timeout=30,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--client", result.stdout)
        self.assertIn("--output", result.stdout)


if __name__ == "__main__":
    unittest.main()
