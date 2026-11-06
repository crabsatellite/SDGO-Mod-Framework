"""Typed ZFX texture dependency extraction.

The native ZOA loader reads each emitter texture carrier as a 32-byte narrow
C string.  Bytes after the first NUL remain serialized storage, but they are
not part of the filename passed to the texture loader.  This module therefore
keeps typed references and post-NUL scanner candidates separate.

No numeric filename padding is performed.  A typed ``022.txr`` field remains
``022.txr``; a field beginning with NUL remains empty even when later storage
bytes happen to contain ``022.txr``.
"""
from __future__ import annotations

import copy
import hashlib
import re
import struct
from typing import Any, Mapping


DEPENDENCY_SCHEMA = "gunion-zfx-dependencies/1"
SOURCE_PROFILE_SCHEMA = "gunion-zfx-source-profile/1"
CONSUMER_CONTRACT_ID = "zoa-native-zfx-texture-c-string-v1"

_MAGIC = b"ZOAEFFECT\0"
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_SOURCE_PATH = re.compile(r"^[A-Za-z0-9_.\\/:-]{1,512}$")
_TEXTURE_LEAF = re.compile(r"^[A-Za-z0-9_.-]+\.txr$", re.IGNORECASE)
_POST_NULL_TEXTURE = re.compile(rb"(?i)([A-Za-z0-9_.-]+\.txr)")

_EMITTER_FIXED_BYTES = 550
_TEXTURE_OFFSET = 4
_TEXTURE_BYTES = 32
_MAX_EMITTERS = 100_000

_ZOA_GRAPHICS_SHA256 = (
    "7f0a8a8ad58402b90a3ac3d6b1795f6a2ac55b827f183ec92f41db83741e8473"
)
_ZOA_CORE_SHA256 = (
    "c6312144957f11905daf4d37da3544cb217e74c83c04c6c8dd265e862b36aef4"
)


def union_native_zfx_source_profile(
    source_path: str,
    source_sha256: str,
) -> dict[str, Any]:
    """Return the proven native/Minecraft consumer contract for one ZFX blob."""

    if not isinstance(source_path, str) or not _SOURCE_PATH.fullmatch(source_path):
        raise ValueError("source_path must be a bounded logical or local path")
    if not isinstance(source_sha256, str) or not _SHA256.fullmatch(
        source_sha256.lower()
    ):
        raise ValueError("source_sha256 must be lowercase hexadecimal SHA-256")
    return {
        "schema": SOURCE_PROFILE_SCHEMA,
        "profileId": CONSUMER_CONTRACT_ID,
        "source": {
            "path": source_path.replace("\\", "/"),
            "sha256": source_sha256.lower(),
        },
        "fieldContract": {
            "name": "emitter.textureName",
            "relativeOffset": _TEXTURE_OFFSET,
            "length": _TEXTURE_BYTES,
            "encoding": "windows-narrow-c-string",
            "terminator": "first-nul",
        },
        "nativeConsumer": {
            "loadCommon": {
                "artifact": "zoa_graphics.dll",
                "artifactSha256": _ZOA_GRAPHICS_SHA256,
                "address": "0x10035DC0",
                "size": 3511,
                "spanSha256": (
                    "abae18a805ba2ad3385b9ae0854551dde877b66c7c8b4ea982a2872e11f58c12"
                ),
                "operation": (
                    "read 32 bytes; ZoaCharToWChar(codepage=0, pointer) "
                    "with NUL-terminated input"
                ),
            },
            "charConversion": {
                "artifact": "zoa_core.dll",
                "artifactSha256": _ZOA_CORE_SHA256,
                "address": "0x100039A0",
                "size": 106,
                "spanSha256": (
                    "7f33ab3fa01c58c5f3ba21020207e745f9d1223d1a6f9de9f1eb2355e3412c51"
                ),
                "operation": "MultiByteToWideChar input length -1",
            },
            "setTexture": {
                "artifact": "zoa_graphics.dll",
                "artifactSha256": _ZOA_GRAPHICS_SHA256,
                "address": "0x1003FAE0",
                "size": 374,
                "spanSha256": (
                    "e4c538c42a93941241b47231e5bc815bfab5efd8c9161a3a6044f050b2a87de1"
                ),
                "operation": (
                    "pass literal filename, optionally preceded by the configured "
                    "texture root, to ZoaLoadTextureUsingResMgr"
                ),
            },
            "normalization": {
                "numericWidth": "literal-preserved",
                "zeroPadding": False,
                "postNulBytesConsumed": False,
            },
        },
        "minecraftConsumers": [
            {
                "target": "sdgo_craft",
                "parser": (
                    "ZfxParser.readFixedString stops at the first NUL in the "
                    "32-byte carrier"
                ),
                "resolver": (
                    "TextureResolver and SourceAssetNamespace preserve the "
                    "numeric stem; no zero padding"
                ),
            },
            {
                "target": "CapsuleArena",
                "parser": (
                    "ZfxParser.readFixedString stops at the first NUL in the "
                    "32-byte carrier"
                ),
                "resolver": (
                    "TextureResolver and SourceAssetNamespace preserve the "
                    "numeric stem; no zero padding"
                ),
            },
        ],
    }


class _Reader:
    def __init__(self, data: bytes):
        self.data = data

    def require(self, offset: int, length: int, label: str) -> None:
        if offset < 0 or length < 0 or offset + length > len(self.data):
            raise ValueError(
                f"ZFX {label} is truncated at {offset}+{length}/{len(self.data)}"
            )

    def u32(self, offset: int, label: str) -> int:
        self.require(offset, 4, label)
        return struct.unpack_from("<I", self.data, offset)[0]

    def skip_vector(
        self, offset: int, stride: int, label: str
    ) -> tuple[int, int]:
        count = self.u32(offset, label + " count")
        offset += 4
        remaining = len(self.data) - offset
        if count > remaining // stride:
            raise ValueError(
                f"ZFX {label} count {count} exceeds remaining byte budget"
            )
        length = count * stride
        self.require(offset, length, label)
        return offset + length, count


def _validate_profile(
    data: bytes, source_profile: Mapping[str, Any]
) -> dict[str, Any]:
    if not isinstance(source_profile, Mapping):
        raise ValueError("source_profile must be a mapping")
    profile = copy.deepcopy(dict(source_profile))
    if profile.get("schema") != SOURCE_PROFILE_SCHEMA:
        raise ValueError("unsupported ZFX source profile schema")
    if profile.get("profileId") != CONSUMER_CONTRACT_ID:
        raise ValueError("unsupported ZFX consumer contract")
    source = profile.get("source")
    if not isinstance(source, Mapping):
        raise ValueError("ZFX source profile lacks source identity")
    expected = str(source.get("sha256", "")).lower()
    if not _SHA256.fullmatch(expected):
        raise ValueError("ZFX source profile has invalid SHA-256")
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        raise ValueError("ZFX source bytes do not match source profile")
    return profile


def _ascii_c_string(raw: bytes) -> tuple[bytes, int | None]:
    nul = raw.find(b"\0")
    if nul < 0:
        return raw, None
    return raw[:nul], nul


def _candidate(
    *,
    literal: str,
    candidate_offset: int,
    emitter_index: int,
    emitter_type: int,
    field_offset: int,
    terminator_offset: int,
    raw: bytes,
) -> dict[str, Any]:
    return {
        "kind": "texture",
        "literal": literal,
        "candidatePath": f"txrs/{literal}",
        "disposition": "post-nul-storage-not-native-string",
        "reason": (
            "the native char converter receives the field start and an input "
            "length of -1, so the leading NUL terminates the value before this token"
        ),
        "carrier": {
            "field": "emitter.textureName",
            "emitterIndex": emitter_index,
            "emitterType": emitter_type,
            "fieldOffset": field_offset,
            "fieldLength": _TEXTURE_BYTES,
            "encoding": "windows-narrow-c-string",
            "terminatorOffset": terminator_offset,
            "candidateOffset": candidate_offset,
            "rawHex": raw.hex(),
        },
    }


def parse_dependencies(
    data: bytes | bytearray | memoryview,
    source_profile: Mapping[str, Any],
) -> dict[str, Any]:
    """Parse exact emitter texture dependencies from one native ZFX blob."""

    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise ValueError("ZFX data must be bytes-like")
    raw_data = bytes(data)
    profile = _validate_profile(raw_data, source_profile)
    reader = _Reader(raw_data)
    reader.require(0, 0x6D, "header")
    if raw_data[: len(_MAGIC)] != _MAGIC:
        raise ValueError("ZFX magic mismatch")

    version = raw_data[0x0B]
    if version < 1 or version > 5:
        raise ValueError(f"unsupported ZFX version {version}")

    offset = 0x69
    offset, effect_keyframe_count = reader.skip_vector(
        offset, 8, "effect size keyframes"
    )
    emitter_count = reader.u32(offset, "emitter count")
    offset += 4
    if emitter_count > _MAX_EMITTERS:
        raise ValueError("ZFX emitter count exceeds safety budget")

    emitters: list[dict[str, Any]] = []
    references: list[dict[str, Any]] = []
    candidates: list[dict[str, Any]] = []
    mesh_indices: list[int] = []

    for emitter_index in range(emitter_count):
        emitter_offset = offset
        reader.require(emitter_offset, _EMITTER_FIXED_BYTES, "emitter fixed block")
        emitter_type = reader.u32(emitter_offset, "emitter type")
        field_offset = emitter_offset + _TEXTURE_OFFSET
        field_raw = raw_data[field_offset : field_offset + _TEXTURE_BYTES]
        value_raw, nul_index = _ascii_c_string(field_raw)
        try:
            value = value_raw.decode("ascii")
        except UnicodeDecodeError:
            value = None

        emitter = {
            "index": emitter_index,
            "type": emitter_type,
            "offset": emitter_offset,
            "textureCarrier": {
                "field": "emitter.textureName",
                "offset": field_offset,
                "length": _TEXTURE_BYTES,
                "encoding": "windows-narrow-c-string",
                "terminatorOffset": (
                    None if nul_index is None else field_offset + nul_index
                ),
                "rawHex": field_raw.hex(),
                "value": value,
            },
        }
        emitters.append(emitter)
        if emitter_type == 2:
            mesh_indices.append(emitter_index)

        if value_raw:
            if value is not None and _TEXTURE_LEAF.fullmatch(value):
                references.append(
                    {
                        "kind": "texture",
                        "path": f"txrs/{value}",
                        "literal": value,
                        "authority": (
                            "typed-zfx-emitter-texture-native-c-string"
                        ),
                        "carrier": copy.deepcopy(emitter["textureCarrier"])
                        | {
                            "emitterIndex": emitter_index,
                            "emitterType": emitter_type,
                        },
                        "consumer": {
                            "nativeLookup": (
                                "configured-texture-root + literal filename"
                            ),
                            "numericWidth": "literal-preserved",
                            "zeroPadding": False,
                        },
                    }
                )
            else:
                candidates.append(
                    {
                        "kind": "texture",
                        "literalHex": value_raw.hex(),
                        "candidatePath": None,
                        "disposition": (
                            "typed-native-string-shape-needs-separate-resolution"
                        ),
                        "carrier": copy.deepcopy(emitter["textureCarrier"])
                        | {
                            "emitterIndex": emitter_index,
                            "emitterType": emitter_type,
                        },
                    }
                )

        if nul_index is not None:
            suffix_start = nul_index + 1
            suffix = field_raw[suffix_start:]
            for match in _POST_NULL_TEXTURE.finditer(suffix):
                literal = match.group(1).decode("ascii")
                candidates.append(
                    _candidate(
                        literal=literal,
                        candidate_offset=field_offset
                        + suffix_start
                        + match.start(1),
                        emitter_index=emitter_index,
                        emitter_type=emitter_type,
                        field_offset=field_offset,
                        terminator_offset=field_offset + nul_index,
                        raw=field_raw,
                    )
                )

        offset = emitter_offset + _EMITTER_FIXED_BYTES
        offset, color_time_count = reader.skip_vector(
            offset, 8, "emitter color times"
        )
        offset, color_vector_count = reader.skip_vector(
            offset, 16, "emitter color vectors"
        )
        offset, size_keyframe_count = reader.skip_vector(
            offset, 8, "emitter size keyframes"
        )
        offset, subdivision_keyframe_count = reader.skip_vector(
            offset, 4, "emitter subdivision keyframes"
        )
        emitter["variableCounts"] = {
            "colorTimes": color_time_count,
            "colorVectors": color_vector_count,
            "sizeKeyframes": size_keyframe_count,
            "subdivisionKeyframes": subdivision_keyframe_count,
        }
        if version > 4:
            reader.require(offset, 1, "version-5 emitter flag")
            emitter["version5Flag"] = raw_data[offset]
            offset += 1
            offset, extra_keyframe_count = reader.skip_vector(
                offset, 16, "version-5 emitter extra keyframes"
            )
            emitter["variableCounts"]["extraKeyframes"] = extra_keyframe_count

    trailer: dict[str, Any] = {"meshCarriers": []}
    if version > 1:
        reader.require(offset, 1, "effect trailer flag 38")
        trailer["flag38"] = raw_data[offset]
        offset += 1
    if version > 2:
        for emitter_index in mesh_indices:
            reader.require(offset, 35, "mesh emitter trailer")
            flags = list(raw_data[offset : offset + 3])
            name_offset = offset + 3
            name_raw = raw_data[name_offset : name_offset + 32]
            name_value_raw, name_nul = _ascii_c_string(name_raw)
            try:
                name_value = name_value_raw.decode("ascii")
            except UnicodeDecodeError:
                name_value = None
            trailer["meshCarriers"].append(
                {
                    "emitterIndex": emitter_index,
                    "offset": offset,
                    "flags": flags,
                    "modelCarrier": {
                        "offset": name_offset,
                        "length": 32,
                        "encoding": "windows-narrow-c-string",
                        "terminatorOffset": (
                            None if name_nul is None else name_offset + name_nul
                        ),
                        "rawHex": name_raw.hex(),
                        "value": name_value,
                    },
                }
            )
            offset += 35
    if version > 3:
        reader.require(offset, 1, "effect trailer flag 39")
        trailer["flag39"] = raw_data[offset]
        offset += 1
    if offset != len(raw_data):
        raise ValueError(
            f"ZFX parser left {len(raw_data) - offset} trailing bytes"
        )

    return {
        "schema": DEPENDENCY_SCHEMA,
        "source": {
            **copy.deepcopy(dict(profile["source"])),
            "bytes": len(raw_data),
        },
        "consumerContract": {
            "id": CONSUMER_CONTRACT_ID,
            "fieldContract": copy.deepcopy(profile["fieldContract"]),
            "nativeConsumer": copy.deepcopy(profile["nativeConsumer"]),
            "minecraftConsumers": copy.deepcopy(profile["minecraftConsumers"]),
        },
        "format": {
            "version": version,
            "effectSizeKeyframeCount": effect_keyframe_count,
            "emitterCount": emitter_count,
            "parsedBytes": offset,
            "trailingBytes": 0,
        },
        "emitters": emitters,
        "trailer": trailer,
        "references": references,
        "candidateReferences": candidates,
        "referencesCompleteForContract": True,
    }

