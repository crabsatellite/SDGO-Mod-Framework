"""Parse source SPR carriers and resolve their exact named dependencies.

The parser preserves serialized bytes and offsets. It does not crop, rescale,
reframe, or silently replace source images. Minecraft-ready RGBA is available
only through an explicitly supplied texture decoder.
"""
from __future__ import annotations

from collections.abc import Callable, Mapping
import hashlib
import math
import struct
from typing import Any


SPR_RECORD_SIZES = {10_000: 164, 11_000: 172}
SPR_HEADER_SIZE = 8
SPR_PLAYBACK_SIZE = 4
WIDE_FIELD_SIZE = 64


class SpriteFormatError(ValueError):
    """A source SPR cannot be represented by the authenticated layout."""


class UnsupportedSpriteVersion(SpriteFormatError):
    """The source declares an SPR version outside the authenticated set."""


class TruncatedSprite(SpriteFormatError):
    """The source ends inside a mandatory serialized field."""


def normalize_resource_path(path: str) -> str:
    """Return the case-insensitive logical path supplied to a resolver."""
    if not isinstance(path, str):
        raise TypeError("resource path must be text")
    normalized = path.replace("/", "\\").lower()
    parts = normalized.split("\\")
    if (not normalized or normalized.startswith("\\") or ":" in parts[0]
            or any(part in ("", ".", "..") for part in parts)):
        raise ValueError(f"unsafe or empty logical resource path: {path!r}")
    return normalized


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _field(data: bytes, offset: int, size: int) -> dict[str, Any]:
    raw = data[offset:offset + size]
    return {"offset": offset, "size": size, "end": offset + size, "rawHex": raw.hex()}


def _float(data: bytes, offset: int) -> tuple[float, str]:
    raw = data[offset:offset + 4]
    return struct.unpack("<f", raw)[0], raw.hex()


def _wide(data: bytes, offset: int) -> tuple[str | None, dict[str, Any], str | None]:
    raw = data[offset:offset + WIDE_FIELD_SIZE]
    terminator = next(
        (index for index in range(0, WIDE_FIELD_SIZE, 2)
         if raw[index:index + 2] == b"\0\0"),
        WIDE_FIELD_SIZE,
    )
    error = None
    try:
        text = raw[:terminator].decode("utf-16le", errors="strict")
    except UnicodeDecodeError as exc:
        text = None
        error = str(exc)
    carrier = {
        "offset": offset,
        "size": WIDE_FIELD_SIZE,
        "end": offset + WIDE_FIELD_SIZE,
        "rawHex": raw.hex(),
        "contentRawHex": raw[:terminator].hex(),
        "terminatorOffset": offset + terminator if terminator < WIDE_FIELD_SIZE else None,
        "paddingRawHex": raw[terminator + 2:].hex()
        if terminator < WIDE_FIELD_SIZE else "",
    }
    return text, carrier, error


def _truncation_detail(version: int, frame_count: int, actual: int) -> str:
    record_size = SPR_RECORD_SIZES[version]
    if actual < SPR_HEADER_SIZE:
        return "header"
    records_end = SPR_HEADER_SIZE + frame_count * record_size
    if actual < records_end:
        frame = (actual - SPR_HEADER_SIZE) // record_size
        within = (actual - SPR_HEADER_SIZE) % record_size
        fields = [
            (0, 4, "durationSourceUpdates"),
            (4, 4, "height"),
            (8, 4, "width"),
            (12, 16, "uv"),
            (28, 8, "field24Raw"),
        ]
        if version == 11_000:
            fields.extend(((36, 4, "colorArgb"), (40, 4, "field10Raw")))
            texture, editor = 44, 108
        else:
            texture, editor = 36, 100
        fields.extend(((texture, 64, "texturePath"), (editor, 64, "editorNameRaw")))
        name = next((name for start, size, name in fields
                     if start <= within < start + size), "frame")
        return f"frame {frame} {name}"
    return "playbackRate"


def parse_sprite(data: bytes, path: str = "") -> dict[str, Any]:
    """Parse one exact SPR byte stream with byte-offset provenance.

    Non-finite floats and consumer-level geometry problems are retained in the
    result's ``unsupported`` list. Structural truncation and unknown versions
    raise a ``SpriteFormatError`` subclass.
    """
    if not isinstance(data, bytes):
        data = bytes(data)
    if len(data) < SPR_HEADER_SIZE:
        raise TruncatedSprite(
            f"SPR {path or '<unnamed>'} truncated header: expected 8 bytes, actual={len(data)}")

    version, frame_count = struct.unpack_from("<II", data, 0)
    if version not in SPR_RECORD_SIZES:
        raise UnsupportedSpriteVersion(
            f"SPR {path or '<unnamed>'} unsupported version {version}")
    record_size = SPR_RECORD_SIZES[version]
    expected = SPR_HEADER_SIZE + frame_count * record_size + SPR_PLAYBACK_SIZE
    if len(data) < expected:
        detail = _truncation_detail(version, frame_count, len(data))
        raise TruncatedSprite(
            f"SPR {path or '<unnamed>'} truncated {detail}: "
            f"version={version}, frames={frame_count}, expected={expected}, actual={len(data)}, "
            f"missing={expected-len(data)}")
    if len(data) > expected:
        raise SpriteFormatError(
            f"SPR {path or '<unnamed>'} trailing bytes: "
            f"version={version}, frames={frame_count}, expected={expected}, actual={len(data)}, "
            f"extra={len(data)-expected}")

    unsupported: list[dict[str, Any]] = []
    if frame_count == 0:
        unsupported.append({
            "code": "empty-sprite",
            "consumer": "native-render-and-minecraft-atlas",
            "offset": 4,
        })
    frames = []
    for index in range(frame_count):
        start = SPR_HEADER_SIZE + index * record_size
        duration, height, width = struct.unpack_from("<III", data, start)
        uv_names = ("left", "top", "right", "bottom")
        uv_values, uv_bits = {}, {}
        for component, name in enumerate(uv_names):
            offset = start + 12 + component * 4
            value, bits = _float(data, offset)
            uv_values[name], uv_bits[name] = value, bits
            if not math.isfinite(value):
                unsupported.append({
                    "code": "nonfinite-float",
                    "consumer": "native-render-and-minecraft-atlas",
                    "frameIndex": index,
                    "field": f"uv.{name}",
                    "offset": offset,
                    "rawHex": bits,
                })
        field24_values, field24_bits = {}, {}
        for component, name in enumerate(("first", "second")):
            offset = start + 28 + component * 4
            value, bits = _float(data, offset)
            field24_values[name], field24_bits[name] = value, bits
            if not math.isfinite(value):
                unsupported.append({
                    "code": "nonfinite-float",
                    "consumer": "unresolved-field24-consumer",
                    "frameIndex": index,
                    "field": f"field24Raw.{name}",
                    "offset": offset,
                    "rawHex": bits,
                })
        if version == 11_000:
            color, field10 = struct.unpack_from("<II", data, start + 36)
            texture_offset, editor_offset = start + 44, start + 108
            color_source = "serialized-v11000"
        else:
            # Authenticated native v10000 construction defaults. They are
            # explicitly marked as non-serialized and never attributed to the
            # source bytes.
            color, field10 = 0xFFFFFFFF, 0xFF
            texture_offset, editor_offset = start + 36, start + 100
            color_source = "native-v10000-default-not-serialized"

        texture, texture_carrier, texture_error = _wide(data, texture_offset)
        editor, editor_carrier, editor_error = _wide(data, editor_offset)
        if texture_error is not None:
            unsupported.append({
                "code": "invalid-texture-path-utf16",
                "consumer": "dependency-resolution",
                "frameIndex": index,
                "offset": texture_offset,
                "error": texture_error,
            })
        if editor_error is not None:
            unsupported.append({
                "code": "invalid-editor-name-utf16",
                "consumer": "source-metadata-only",
                "frameIndex": index,
                "offset": editor_offset,
                "error": editor_error,
            })
        if width == 0 or height == 0:
            unsupported.append({
                "code": "zero-frame-dimension",
                "consumer": "minecraft-atlas",
                "frameIndex": index,
                "offset": start + (8 if width == 0 else 4),
                "width": width,
                "height": height,
            })
        canonical_texture = None
        if texture:
            try:
                canonical_texture = normalize_resource_path(texture)
            except ValueError as exc:
                unsupported.append({
                    "code": "invalid-texture-logical-path",
                    "consumer": "dependency-resolution",
                    "frameIndex": index,
                    "offset": texture_offset,
                    "path": texture,
                    "error": str(exc),
                })
        else:
            unsupported.append({
                "code": "missing-frame-texture-path",
                "consumer": "native-render-and-minecraft-atlas",
                "frameIndex": index,
                "offset": texture_offset,
            })

        field_provenance = {
            "durationSourceUpdates": _field(data, start, 4),
            "height": _field(data, start + 4, 4),
            "width": _field(data, start + 8, 4),
            "uv": _field(data, start + 12, 16),
            "field24Raw": _field(data, start + 28, 8),
            "texturePath": texture_carrier,
            "editorNameRaw": editor_carrier,
        }
        if version == 11_000:
            field_provenance["colorArgb"] = _field(data, start + 36, 4)
            field_provenance["field10Raw"] = _field(data, start + 40, 4)
        frames.append({
            "index": index,
            "byteRange": {"offset": start, "size": record_size, "end": start + record_size},
            "durationSourceUpdates": duration,
            "height": height,
            "width": width,
            "uv": uv_values,
            "uvBitsHex": uv_bits,
            "field24Raw": field24_values,
            "field24BitsHex": field24_bits,
            "field24Semantic": "unresolved; authenticated UI evidence rejects render-origin/pivot use",
            "colorArgb": color,
            "colorArgbSource": color_source,
            "field10Raw": field10,
            "field10Semantic": "unresolved raw uint32; not certified as flip or alpha flag",
            "texturePath": texture,
            "textureDependencyPath": canonical_texture,
            "editorNameRaw": editor,
            "editorNameSemantic": "source metadata only; ignored by authenticated renderer dependency path",
            "fieldProvenance": field_provenance,
        })

    playback_offset = SPR_HEADER_SIZE + frame_count * record_size
    playback, playback_bits = _float(data, playback_offset)
    if not math.isfinite(playback):
        unsupported.append({
            "code": "nonfinite-float",
            "consumer": "native-update-and-minecraft-animation",
            "field": "playbackRate",
            "offset": playback_offset,
            "rawHex": playback_bits,
        })
    elif playback < 0:
        unsupported.append({
            "code": "negative-playback-rate",
            "consumer": "minecraft-animation",
            "field": "playbackRate",
            "offset": playback_offset,
            "rawHex": playback_bits,
            "value": playback,
        })
    return {
        "schema": "sdgo-sprite-source/1",
        "path": path,
        "sha256": _sha256(data),
        "size": len(data),
        "version": version,
        "frameCount": frame_count,
        "recordSize": record_size,
        "headerProvenance": {
            "version": _field(data, 0, 4),
            "frameCount": _field(data, 4, 4),
        },
        "frames": frames,
        "playbackRate": playback,
        "playbackRateBitsHex": playback_bits,
        "playbackRateProvenance": _field(data, playback_offset, 4),
        "unsupported": unsupported,
    }


def _resource(path: str, resolver: Callable[[str], Mapping[str, Any]]) -> dict[str, Any]:
    supplied = resolver(path)
    if not isinstance(supplied, Mapping):
        raise TypeError(f"resolver returned no Resource mapping for {path}")
    if "data" not in supplied or "sha256" not in supplied or "provenance" not in supplied:
        raise ValueError(f"resolver Resource missing data/sha256/provenance for {path}")
    data = supplied["data"]
    if not isinstance(data, bytes):
        data = bytes(data)
    actual = _sha256(data)
    declared = str(supplied["sha256"]).lower()
    if declared != actual:
        raise ValueError(
            f"resolver Resource SHA-256 mismatch for {path}: declared={declared}, actual={actual}")
    return {
        "path": path,
        "data": data,
        "size": len(data),
        "sha256": actual,
        "provenance": supplied["provenance"],
    }


def _missing(code: str, path: str, error: BaseException) -> dict[str, Any]:
    return {"code": code, "path": path, "error": f"{type(error).__name__}: {error}"}


def resolve_sprite(
    path: str,
    resolver: Callable[[str], Mapping[str, Any]],
    *,
    texture_decoder: Callable[[str, bytes], Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    """Resolve one named SPR, exact TXR dependencies, and optional RGBA atlas.

    ``resolver`` receives normalized lower-case backslash paths and returns
    ``{"data": bytes, "sha256": hex, "provenance": ...}``.

    An optional decoder receives ``(normalized_path, exact_txr_bytes)`` and
    must return ``{"schema": "rgba8/1", "width": int, "height": int,
    "rgba": bytes}``. The returned RGBA is the complete decoded source atlas;
    this function never crops or scales it.
    """
    canonical = normalize_resource_path(path)
    unsupported: list[dict[str, Any]] = []
    try:
        native = _resource(canonical, resolver)
    except (KeyError, FileNotFoundError) as exc:
        return {
            "schema": "sdgo-sprite-resolution/1",
            "path": canonical,
            "sprite": None,
            "native": None,
            "dependencies": [],
            "minecraft": None,
            "unsupported": [_missing("missing-sprite-resource", canonical, exc)],
        }
    try:
        sprite = parse_sprite(native["data"], canonical)
    except SpriteFormatError as exc:
        return {
            "schema": "sdgo-sprite-resolution/1",
            "path": canonical,
            "sprite": None,
            "native": native,
            "dependencies": [],
            "minecraft": None,
            "unsupported": [_missing("unsupported-or-malformed-sprite", canonical, exc)],
        }
    unsupported.extend(sprite["unsupported"])

    grouped: dict[str, dict[str, Any]] = {}
    for frame in sprite["frames"]:
        dependency_path = frame["textureDependencyPath"]
        if not dependency_path:
            continue
        reference = {
            "frameIndex": frame["index"],
            "sourcePath": frame["texturePath"],
            "offset": frame["fieldProvenance"]["texturePath"]["offset"],
            "size": frame["fieldProvenance"]["texturePath"]["size"],
            "rawHex": frame["fieldProvenance"]["texturePath"]["rawHex"],
        }
        grouped.setdefault(dependency_path, {
            "kind": "texture",
            "path": dependency_path,
            "references": [],
            "native": None,
            "minecraft": None,
        })["references"].append(reference)

    dependencies = []
    for dependency_path, dependency in grouped.items():
        try:
            texture = _resource(dependency_path, resolver)
            dependency["native"] = texture
        except (KeyError, FileNotFoundError) as exc:
            unsupported.append(_missing("missing-texture-resource", dependency_path, exc))
            dependencies.append(dependency)
            continue
        if texture_decoder is not None:
            try:
                decoded = texture_decoder(dependency_path, texture["data"])
                if not isinstance(decoded, Mapping) or decoded.get("schema") != "rgba8/1":
                    raise ValueError("decoder must return schema rgba8/1")
                width, height = decoded.get("width"), decoded.get("height")
                rgba = decoded.get("rgba")
                if (not isinstance(width, int) or isinstance(width, bool) or width <= 0
                        or not isinstance(height, int) or isinstance(height, bool) or height <= 0):
                    raise ValueError("decoder returned invalid atlas dimensions")
                if not isinstance(rgba, bytes):
                    rgba = bytes(rgba)
                if len(rgba) != width * height * 4:
                    raise ValueError(
                        f"decoder RGBA length mismatch: expected={width*height*4}, actual={len(rgba)}")
                dependency["minecraft"] = {
                    "schema": "rgba8/1",
                    "width": width,
                    "height": height,
                    "rgba": rgba,
                    "rgbaSha256": _sha256(rgba),
                    "decoderProvenance": decoded.get("provenance"),
                    "policy": "complete-source-atlas-no-crop-no-rescale",
                }
            except Exception as exc:
                unsupported.append(_missing("texture-decode-failed", dependency_path, exc))
        dependencies.append(dependency)

    dependency_index = {dependency["path"]: index for index, dependency in enumerate(dependencies)}
    atlas_frames = []
    for frame in sprite["frames"]:
        atlas_frames.append({
            "index": frame["index"],
            "texturePath": frame["textureDependencyPath"],
            "textureDependencyIndex": dependency_index.get(frame["textureDependencyPath"]),
            "durationSourceUpdates": frame["durationSourceUpdates"],
            "width": frame["width"],
            "height": frame["height"],
            "uv": frame["uv"],
            "uvBitsHex": frame["uvBitsHex"],
            "field24Raw": frame["field24Raw"],
            "field24BitsHex": frame["field24BitsHex"],
            "colorArgb": frame["colorArgb"],
            "colorArgbSource": frame["colorArgbSource"],
            "field10Raw": frame["field10Raw"],
        })
    minecraft = {
        "schema": "sdgo-minecraft-atlas-source/1",
        "playbackRate": sprite["playbackRate"],
        "playbackRateBitsHex": sprite["playbackRateBitsHex"],
        "frames": atlas_frames,
        "texturesDecoded": sum(1 for dependency in dependencies
                               if dependency["minecraft"] is not None),
        "texturesRequired": len(dependencies),
        "ready": (
            texture_decoder is not None
            and all(dependency["minecraft"] is not None for dependency in dependencies)
            and not any(item.get("consumer") in {
                "native-render-and-minecraft-atlas",
                "minecraft-atlas",
                "native-update-and-minecraft-animation",
                "minecraft-animation",
                "dependency-resolution",
            } for item in unsupported)
        ),
        "policy": "source-uv-and-full-atlas-rgba-no-crop-no-rescale-no-fallback",
    }
    return {
        "schema": "sdgo-sprite-resolution/1",
        "path": canonical,
        "sprite": sprite,
        "native": native,
        "dependencies": dependencies,
        "minecraft": minecraft,
        "unsupported": unsupported,
    }
