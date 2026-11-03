"""Fail-closed Type-13 projection into the admitted 1132-byte host layout.

The profile proves record layout only. It cannot authorize registration,
invent server-only values, resolve dependencies, or claim runtime acceptance.
"""
from __future__ import annotations

import hashlib
import re
import struct
from typing import Any


PROFILE_ID = "native-type13-1132-v1"
RECORD_KIND = 13
SOURCE_RECORD_SIZE = 882
TARGET_RECORD_SIZE = 1132
SOURCE_DISPATCH_OFFSET = 881
TARGET_DISPATCH_OFFSET = 1131
SUPPORTED_DISPATCH_VALUES = frozenset({0, 1, 2, 3})

HOST_GONLINE_SHA256 = (
    "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92"
)
HOST_0253_RAW_SHA256 = (
    "b8c5e83d682b300df7fddb18617efca356542278aba3ef74f25a8752e0b78e38"
)
HOST_0253_DECODED_LOGICAL_SHA256 = (
    "91a86b680e5e7a80bf91bd23a00b5b5f84644973ff23eb60b39c03b3e8048263"
)
CONSUMER_EVIDENCE_SHA256 = (
    "ea0e4301a455ca6d6261fca75be0802adaf0bab6165a094a5609e05f25ca5cf5"
)

_SHA256 = re.compile(r"[0-9a-f]{64}")
_INSERTED_RANGES = ((105, 155), (931, 1131))


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _hash(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _SHA256.fullmatch(value):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def _identity(value: Any, label: str, *, allow_zero: bool = False) -> int:
    minimum = 0 if allow_zero else 1
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not minimum <= value <= 9_999_999
    ):
        raise ValueError(f"{label} must be an integer in {minimum}..9999999")
    return value


def target_profile() -> dict[str, Any]:
    """Return the immutable generic layout and admission contract."""
    return {
        "profile_id": PROFILE_ID,
        "record_kind": RECORD_KIND,
        "source_record_size": SOURCE_RECORD_SIZE,
        "target_record_size": TARGET_RECORD_SIZE,
        "copy_ranges": (
            ((0, 105), (0, 105)),
            ((105, 881), (155, 931)),
            ((881, 882), (1131, 1132)),
        ),
        "inserted_zero_ranges": _INSERTED_RANGES,
        "dispatch": {
            "source_offset": SOURCE_DISPATCH_OFFSET,
            "target_offset": TARGET_DISPATCH_OFFSET,
            "supported_values": tuple(sorted(SUPPORTED_DISPATCH_VALUES)),
        },
        "host_witness": {
            "gonline_sha256": HOST_GONLINE_SHA256,
            "raw_0253_sha256": HOST_0253_RAW_SHA256,
            "decoded_logical_0253_sha256": (
                HOST_0253_DECODED_LOGICAL_SHA256
            ),
            "consumer_evidence_sha256": CONSUMER_EVIDENCE_SHA256,
        },
        "admission": {
            "layout_is_registration": False,
            "layout_is_runtime_acceptance": False,
            "server_only_values_may_be_invented": False,
            "unknown_dispatch_may_be_zeroed": False,
        },
    }


def validate_host_witness(
    *,
    gonline_sha256: str,
    raw_0253_sha256: str,
    decoded_logical_0253_sha256: str,
) -> None:
    """Reject a target host that is not the exact analyzed build."""
    supplied = {
        "gonline_sha256": _hash(gonline_sha256, "GOnline identity"),
        "raw_0253_sha256": _hash(raw_0253_sha256, "raw 0253 identity"),
        "decoded_logical_0253_sha256": _hash(
            decoded_logical_0253_sha256, "decoded 0253 identity"
        ),
    }
    expected = target_profile()["host_witness"]
    for name, value in supplied.items():
        if value != expected[name]:
            raise ValueError(f"Type-13 host witness mismatch: {name}")


def _source_record(source: bytes) -> tuple[bytes, int]:
    source = bytes(source)
    if len(source) != SOURCE_RECORD_SIZE:
        raise ValueError(
            f"Type-13 source record must be {SOURCE_RECORD_SIZE} bytes"
        )
    skill_id = struct.unpack_from("<I", source, 0)[0]
    _identity(skill_id, "skill_id")
    return source, skill_id


def _project(source: bytes) -> bytes:
    return (
        source[:105]
        + bytes(50)
        + source[105:881]
        + bytes(200)
        + source[881:882]
    )


def recover_source_record(target: bytes) -> bytes:
    """Invert the projection and reject target-only bytes authored elsewhere."""
    target = bytes(target)
    if len(target) != TARGET_RECORD_SIZE:
        raise ValueError(
            f"Type-13 target record must be {TARGET_RECORD_SIZE} bytes"
        )
    for start, end in _INSERTED_RANGES:
        if any(target[start:end]):
            raise ValueError(
                f"Type-13 inserted range {start}..{end} is not zero-filled"
            )
    source = target[:105] + target[155:931] + target[1131:1132]
    _source_record(source)
    return source


def validate_projected_record(
    *,
    source: bytes,
    target: bytes,
    skill_id: int,
    source_row: int,
    source_sha256: str,
    target_sha256: str,
) -> dict[str, Any]:
    """Validate one source/target pair and return source-bound protocol IR."""
    source, embedded_skill_id = _source_record(source)
    skill_id = _identity(skill_id, "skill_id")
    source_row = _identity(source_row, "source_row", allow_zero=True)
    if embedded_skill_id != skill_id:
        raise ValueError("Type-13 source skill identity mismatch")
    expected_source_hash = _hash(source_sha256, "source record identity")
    expected_target_hash = _hash(target_sha256, "target record identity")
    if _sha256(source) != expected_source_hash:
        raise ValueError("Type-13 source record SHA-256 mismatch")

    target = bytes(target)
    if _sha256(target) != expected_target_hash:
        raise ValueError("Type-13 target record SHA-256 mismatch")
    if _project(source) != target:
        raise ValueError("Type-13 target bytes do not match the profile")
    if recover_source_record(target) != source:
        raise ValueError("Type-13 inverse projection mismatch")

    dispatch = source[SOURCE_DISPATCH_OFFSET]
    supported_dispatch = dispatch in SUPPORTED_DISPATCH_VALUES
    blockers = [
        "native consumer kind-13 capability is not enabled",
        "registration and initialization lifecycle is unresolved",
        "resource, linked-skill, server-field, and runtime closure is unresolved",
    ]
    if not supported_dispatch:
        blockers.append(
            "dispatch value is outside the observed target branch domain"
        )
    return {
        "schema": "sdgo.native-type13-record/1",
        "profile_id": PROFILE_ID,
        "record_kind": RECORD_KIND,
        "skill_id": skill_id,
        "source_row": source_row,
        "source": {
            "size": len(source),
            "sha256": expected_source_hash,
            "retention": "complete-record-required",
        },
        "target": {
            "size": len(target),
            "sha256": expected_target_hash,
            "inserted_zero_ranges": _INSERTED_RANGES,
        },
        "dispatch": {
            "value": dispatch,
            "source_offset": SOURCE_DISPATCH_OFFSET,
            "target_offset": TARGET_DISPATCH_OFFSET,
            "supported": supported_dispatch,
            "supported_values": tuple(sorted(SUPPORTED_DISPATCH_VALUES)),
        },
        "admission": {
            "layout_projected": True,
            "source_recoverable": True,
            "dispatch_compatibility_blocked": not supported_dispatch,
            "native_registration_blocked": True,
            "native_runtime_accepted": False,
            "blockers": tuple(blockers),
        },
    }
