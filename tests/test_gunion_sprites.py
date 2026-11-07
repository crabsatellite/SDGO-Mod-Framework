"""Structural and resolver tests for source-bound SPR recovery."""
from __future__ import annotations

import hashlib
import importlib.util
import math
from pathlib import Path
import struct
import unittest

from tools.gunion.sprites import (
    SpriteFormatError,
    TruncatedSprite,
    UnsupportedSpriteVersion,
    normalize_resource_path,
    parse_sprite,
    resolve_sprite,
)


def wide(value: str, *, padding: bytes = b"") -> bytes:
    encoded = value.encode("utf-16le") + b"\0\0"
    if len(encoded) + len(padding) > 64:
        raise ValueError("wide fixture")
    return encoded + padding + b"\0" * (64 - len(encoded) - len(padding))


def frame(version: int, *, duration: int = 20, height: int = 16, width: int = 32,
          uv=(0.0, -0.0, 1.0, 1.0), field24=(-0.0, 2.0),
          color: int = 0x80ABCDEF, field10: int = 0x12345678,
          texture: str = r"Txrs\Atlas.txr", editor: str = "author",
          editor_padding: bytes = b"") -> bytes:
    value = struct.pack("<III4f2f", duration, height, width, *uv, *field24)
    if version == 11_000:
        value += struct.pack("<II", color, field10)
    return value + wide(texture) + wide(editor, padding=editor_padding)


def sprite(version: int, frames: list[bytes], playback: float = 1.0) -> bytes:
    return struct.pack("<II", version, len(frames)) + b"".join(frames) + struct.pack("<f", playback)


def resource(data: bytes, **provenance):
    return {"data": data, "sha256": hashlib.sha256(data).hexdigest(), "provenance": provenance}


class SpriteParserTest(unittest.TestCase):
    def test_v11000_preserves_offsets_color_raw_fields_and_float_bits(self):
        data = sprite(11_000, [
            frame(11_000, editor_padding=b"\x12\x34"),
            frame(11_000, texture=r"txrs\second.txr", color=0xFFFFFFFF, field10=0xFF),
        ], playback=-0.0)
        result = parse_sprite(data, r"Sprs\Test.spr")
        self.assertEqual((result["version"], result["frameCount"], result["recordSize"]),
                         (11_000, 2, 172))
        first = result["frames"][0]
        self.assertEqual(first["byteRange"], {"offset": 8, "size": 172, "end": 180})
        self.assertEqual(first["fieldProvenance"]["texturePath"]["offset"], 52)
        self.assertEqual(first["fieldProvenance"]["editorNameRaw"]["offset"], 116)
        self.assertEqual(first["colorArgb"], 0x80ABCDEF)
        self.assertEqual(first["field10Raw"], 0x12345678)
        self.assertEqual(first["uvBitsHex"]["top"], "00000080")
        self.assertEqual(first["field24BitsHex"]["first"], "00000080")
        self.assertLess(math.copysign(1.0, first["uv"]["top"]), 0)
        self.assertEqual(result["playbackRateBitsHex"], "00000080")
        self.assertEqual(result["playbackRateProvenance"]["offset"], 352)
        self.assertEqual(first["editorNameRaw"], "author")
        self.assertIn("1234", first["fieldProvenance"]["editorNameRaw"]["paddingRawHex"])
        self.assertEqual(first["textureDependencyPath"], r"txrs\atlas.txr")

    def test_v10000_has_explicit_nonserialized_native_defaults(self):
        data = sprite(10_000, [frame(10_000)])
        result = parse_sprite(data)
        item = result["frames"][0]
        self.assertEqual(len(data), 176)
        self.assertEqual(item["byteRange"]["size"], 164)
        self.assertEqual(item["colorArgb"], 0xFFFFFFFF)
        self.assertEqual(item["field10Raw"], 0xFF)
        self.assertEqual(item["colorArgbSource"], "native-v10000-default-not-serialized")
        self.assertNotIn("colorArgb", item["fieldProvenance"])
        self.assertEqual(item["fieldProvenance"]["texturePath"]["offset"], 44)

    def test_truncation_unknown_version_and_trailing_bytes_are_distinct(self):
        complete = sprite(11_000, [frame(11_000)])
        self.assertEqual(len(complete), 184)
        with self.assertRaisesRegex(TruncatedSprite, r"playbackRate.*expected=184, actual=183, missing=1"):
            parse_sprite(complete[:-1], "old-block-80104")
        with self.assertRaises(UnsupportedSpriteVersion):
            parse_sprite(struct.pack("<II", 12_000, 0) + b"\0" * 4)
        with self.assertRaisesRegex(SpriteFormatError, "trailing bytes"):
            parse_sprite(complete + b"x")

    def test_nonfinite_and_zero_dimensions_are_retained_as_unsupported(self):
        data = sprite(11_000, [
            frame(11_000, height=0, uv=(float("nan"), 0.0, 1.0, 1.0)),
        ], playback=float("inf"))
        result = parse_sprite(data)
        codes = [item["code"] for item in result["unsupported"]]
        self.assertEqual(codes.count("nonfinite-float"), 2)
        self.assertIn("zero-frame-dimension", codes)
        self.assertEqual(result["frames"][0]["uvBitsHex"]["left"], "0000c07f")

    def test_empty_texture_and_negative_playback_are_not_defaulted(self):
        data = sprite(11_000, [frame(11_000, texture="")], playback=-1.0)
        result = parse_sprite(data)
        self.assertEqual(
            [item["code"] for item in result["unsupported"]],
            ["missing-frame-texture-path", "negative-playback-rate"],
        )
        self.assertEqual(result["frames"][0]["texturePath"], "")
        self.assertEqual(result["playbackRate"], -1.0)

    def test_empty_and_short_streams_fail_closed(self):
        with self.assertRaises(TruncatedSprite):
            parse_sprite(b"")
        with self.assertRaisesRegex(TruncatedSprite, "frame 0 texturePath"):
            parse_sprite(struct.pack("<II", 10_000, 1) + b"\0" * 40)


class SpriteResolverTest(unittest.TestCase):
    def setUp(self):
        self.spr = sprite(11_000, [
            frame(11_000, texture=r"Txrs\Atlas.txr", editor=r"Txrs\NotAlpha.txr"),
            frame(11_000, texture=r"txrs/atlas.txr"),
        ])
        self.txr = b"exact-native-txr"
        self.resources = {
            r"sprs\pf247.spr": resource(self.spr, block=100),
            r"txrs\atlas.txr": resource(self.txr, block=200),
        }
        self.calls = []

    def resolver(self, path):
        self.calls.append(path)
        if path not in self.resources:
            raise KeyError(path)
        return self.resources[path]

    def test_exact_dependency_and_explicit_rgba_atlas(self):
        rgba = bytes(range(16))

        def decoder(path, data):
            self.assertEqual(path, r"txrs\atlas.txr")
            self.assertIs(data, self.resources[path]["data"])
            return {
                "schema": "rgba8/1",
                "width": 2,
                "height": 2,
                "rgba": rgba,
                "provenance": {"decoder": "fixture/1"},
            }

        result = resolve_sprite("SPRS/PF247.SPR", self.resolver, texture_decoder=decoder)
        self.assertEqual(self.calls, [r"sprs\pf247.spr", r"txrs\atlas.txr"])
        self.assertEqual(len(result["dependencies"]), 1)
        dependency = result["dependencies"][0]
        self.assertEqual(dependency["path"], r"txrs\atlas.txr")
        self.assertEqual([ref["frameIndex"] for ref in dependency["references"]], [0, 1])
        self.assertEqual(dependency["native"]["data"], self.txr)
        self.assertEqual(dependency["minecraft"]["rgba"], rgba)
        self.assertEqual(dependency["minecraft"]["policy"],
                         "complete-source-atlas-no-crop-no-rescale")
        self.assertTrue(result["minecraft"]["ready"])
        self.assertEqual(result["minecraft"]["frames"][0]["uvBitsHex"]["top"], "00000080")
        self.assertNotIn(r"txrs\notalpha.txr", self.calls)
        self.assertEqual(result["unsupported"], [])

    def test_native_only_keeps_exact_bytes_without_implicit_decoder(self):
        result = resolve_sprite(r"sprs\pf247.spr", self.resolver)
        self.assertIs(result["native"]["data"], self.spr)
        self.assertIs(result["dependencies"][0]["native"]["data"], self.txr)
        self.assertIsNone(result["dependencies"][0]["minecraft"])
        self.assertFalse(result["minecraft"]["ready"])
        self.assertEqual(result["unsupported"], [])

    def test_missing_texture_and_decoder_failure_remain_explicit(self):
        del self.resources[r"txrs\atlas.txr"]
        result = resolve_sprite(r"sprs\pf247.spr", self.resolver)
        self.assertEqual(result["unsupported"][0]["code"], "missing-texture-resource")
        self.assertIsNone(result["dependencies"][0]["native"])
        self.assertFalse(result["minecraft"]["ready"])

        self.resources[r"txrs\atlas.txr"] = resource(self.txr, block=200)
        failed = resolve_sprite(
            r"sprs\pf247.spr", self.resolver,
            texture_decoder=lambda _path, _data: {
                "schema": "rgba8/1", "width": 2, "height": 2, "rgba": b"short"})
        self.assertEqual(failed["unsupported"][0]["code"], "texture-decode-failed")

    def test_missing_or_malformed_sprite_is_a_bounded_closure_result(self):
        missing = resolve_sprite(r"sprs\missing.spr", self.resolver)
        self.assertIsNone(missing["sprite"])
        self.assertEqual(missing["unsupported"][0]["code"], "missing-sprite-resource")

        broken = b"\xf8\x2a\0\0\x01\0\0\0"
        self.resources[r"sprs\broken.spr"] = resource(broken, block=300)
        malformed = resolve_sprite(r"sprs\broken.spr", self.resolver)
        self.assertIsNone(malformed["sprite"])
        self.assertEqual(malformed["unsupported"][0]["code"],
                         "unsupported-or-malformed-sprite")
        self.assertEqual(malformed["native"]["data"], broken)

    def test_resolver_hash_mismatch_and_unsafe_paths_fail_closed(self):
        self.resources[r"sprs\pf247.spr"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            resolve_sprite(r"sprs\pf247.spr", self.resolver)
        for path in ("", r"..\secret", r"C:\absolute.spr", r"\rooted.spr"):
            with self.assertRaises(ValueError):
                normalize_resource_path(path)


@unittest.skipUnless(
    Path(r"E:\Dev\sd-re\SDGO-mc-assets\extracted\sprs\0257.spr").is_file(),
    "owner's authenticated current-client SPR fixtures are absent",
)
class PrivateSpriteFixtureTest(unittest.TestCase):
    def test_real_v10000_and_v11000_files_parse_with_exact_extent(self):
        root = Path(r"E:\Dev\sd-re\SDGO-mc-assets\extracted\sprs")
        for name, version in (("0257.spr", 10_000), ("00.spr", 11_000)):
            data = (root / name).read_bytes()
            result = parse_sprite(data, f"sprs/{name}")
            self.assertEqual(result["version"], version)
            self.assertEqual(result["size"], len(data))
            self.assertEqual(
                result["playbackRateProvenance"]["end"], len(data))


@unittest.skipUnless(
    Path(r"E:\Dev\sd-re\SDGO_UNION_5.4.6\DataPack.bin").is_file(),
    "owner's retained Union DataPack.bin is absent",
)
class PrivateUnionNamedClosureTest(unittest.TestCase):
    def test_pf247_resolves_exact_lb07_and_explicit_full_rgba_atlas(self):
        from tools.gunion.container import Archive
        from tools.gunion.index import read_index
        from tools.gunion.native_codec import NativeCodec

        converter_path = Path(r"E:\Dev\sd-re\SDGO-mc-assets\tools\txr_convert.py")
        if not converter_path.is_file():
            self.skipTest("owner's explicit TXR decoder is absent")
        specification = importlib.util.spec_from_file_location(
            "gunion_private_txr_convert", converter_path)
        module = importlib.util.module_from_spec(specification)
        try:
            specification.loader.exec_module(module)
        except ImportError as exc:
            self.skipTest(f"owner's explicit TXR decoder dependency is absent: {exc}")

        source = Path(r"E:\Dev\sd-re\SDGO_UNION_5.4.6\DataPack.bin")
        index, archive = read_index(source), Archive(source)
        with NativeCodec() as native:
            def resolver(path):
                entry = index.resolve(path)
                block = archive.read(entry.block, decoder=native)
                return resource(
                    block.data, index=entry.index, offset=entry.offset,
                    sourceSha256="c525c84afbef48fc05d3c905e646f8226bcd95ba5a9ad41a6c3e4cb2410a39c0")

            def decoder(path, data):
                image = module.convert_txr_data(data).convert("RGBA")
                return {
                    "schema": "rgba8/1",
                    "width": image.width,
                    "height": image.height,
                    "rgba": image.tobytes(),
                    "provenance": {
                        "adapter": "txr_convert.convert_txr_data",
                        "logicalPath": path,
                    },
                }

            result = resolve_sprite(
                r"sprs\pf247.spr", resolver, texture_decoder=decoder)
        self.assertEqual(
            result["sprite"]["sha256"],
            "628666770882ad95c60b81a4a3a89a0614b9c8d3c0ec63db92b30ff34f562485")
        self.assertEqual(result["sprite"]["frames"][0]["texturePath"], r"Txrs\lb07.txr")
        self.assertEqual(result["sprite"]["frames"][0]["width"], 122)
        self.assertEqual(result["sprite"]["frames"][0]["height"], 122)
        self.assertEqual(len(result["dependencies"]), 1)
        dependency = result["dependencies"][0]
        self.assertEqual(
            dependency["native"]["sha256"],
            "71e0a56fbfe143dd2388518815630c7808550419394a98dbea5b62e5b3374061")
        self.assertEqual(
            (dependency["minecraft"]["width"], dependency["minecraft"]["height"]),
            (1024, 1024))
        self.assertEqual(
            dependency["minecraft"]["rgbaSha256"],
            "31143ad1d035612a84d7a7b4b47a135f81e1370f9f08e4adf6967ae3764537cb")
        self.assertTrue(result["minecraft"]["ready"])
        self.assertEqual(result["unsupported"], [])


if __name__ == "__main__":
    unittest.main()
