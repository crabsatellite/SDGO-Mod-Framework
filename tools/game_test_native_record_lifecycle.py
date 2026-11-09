"""Verify the passive native-record registration and destruction receipt."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import stat
import struct
from typing import Any, NamedTuple


RECEIPT_RELATIVE = Path("sdgo/gametest/native-record-lifecycle.json")
CATALOG_RELATIVE = Path("sdgo/mods/unit_catalog/catalog.bin")
RECEIPT_LIMIT = 512 * 1024
CATALOG_PAYLOAD_LIMIT = 64 * 1024 * 1024
CATALOG_FILE_LIMIT = 32 + CATALOG_PAYLOAD_LIMIT
CATALOG_HEADER = struct.Struct("<8s6I")
CATALOG_ENTRY = struct.Struct("<6I")
CATALOG_MAGIC = b"SDGOUN1\0"
CATALOG_VERSION = 1
CATALOG_RECORD_LIMIT = 8192
MAX_RECORD_ID = 9_999_999
MAX_CAPTURE_IDS = 16
RECORD_BYTES = {
    2: 4976,
    10: 1871,
    11: 732,
    12: 1559,
    13: 1132,
    0x21: 1197,
    0x22: 614,
    0x55: 614,
}
CAPTURE_KINDS = frozenset((2, 10, 11, 13))
OBSERVER_SCOPE = "registration-and-native-grid-destruction-only"
POINTER_KEYS = ("raw", "wrapper", "node", "hash_node", "hash_value")
CAPTURE_KEYS = ("record", "insert", "register", "integrity")
HEX_POINTER = re.compile(r"0x[0-9a-f]{8}\Z")
SHA256 = re.compile(r"[0-9a-fA-F]{64}\Z")
REPARSE_POINT = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class CatalogRecord(NamedTuple):
    kind: int
    identity: int
    raw: bytes


def _fnv1a32(raw: bytes) -> int:
    value = 2166136261
    for byte in raw:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def _strict_int(value: Any, label: str) -> int:
    if type(value) is not int:
        raise ValueError(f"{label} must be an integer")
    return value


def _strict_one(value: Any, label: str) -> None:
    if _strict_int(value, label) != 1:
        raise ValueError(f"{label} must equal one")


def _exact_object(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    if type(value) is not dict or set(value) != keys:
        raise ValueError(f"{label} has missing or unexpected fields")
    return value


def _validate_ids(value: Any, label: str = "ids") -> list[int]:
    if type(value) is not list or not 1 <= len(value) <= MAX_CAPTURE_IDS:
        raise ValueError(f"{label} must contain between one and 16 ids")
    ids = [_strict_int(identity, f"{label} entry") for identity in value]
    if any(identity < 1 or identity > MAX_RECORD_ID for identity in ids):
        raise ValueError(f"{label} contains an out-of-range id")
    if ids != sorted(ids) or len(set(ids)) != len(ids):
        raise ValueError(f"{label} must be unique and sorted")
    return ids


def _is_link_or_reparse(path: Path) -> bool:
    metadata = path.lstat()
    return stat.S_ISLNK(metadata.st_mode) or bool(
        getattr(metadata, "st_file_attributes", 0) & REPARSE_POINT
    )


def _reject_link_components(path: Path) -> None:
    absolute = path.absolute()
    components = [absolute, *absolute.parents]
    for component in components:
        if component.exists() and _is_link_or_reparse(component):
            raise ValueError(f"linked path is not allowed: {component}")


def _fixed_file(root: Path, relative: Path) -> Path:
    if not isinstance(root, Path):
        raise ValueError("client_root must be a pathlib.Path")
    _reject_link_components(root)
    root_absolute = root.absolute()
    if not root_absolute.is_dir():
        raise OSError(f"client root is missing or not a directory: {root_absolute}")
    root_resolved = root_absolute.resolve(strict=True)

    target = root_absolute.joinpath(relative)
    current = root_absolute
    for part in relative.parts:
        current = current / part
        if not current.exists():
            raise OSError(f"required native proof file is missing: {target}")
        if _is_link_or_reparse(current):
            raise ValueError(f"linked native proof path is not allowed: {current}")
    resolved = target.resolve(strict=True)
    if not resolved.is_relative_to(root_resolved):
        raise ValueError(f"native proof path escapes client root: {target}")
    metadata = target.lstat()
    if not stat.S_ISREG(metadata.st_mode):
        raise OSError(f"native proof path is not a regular file: {target}")
    return target


def _read_bounded(root: Path, relative: Path, limit: int) -> bytes:
    path = _fixed_file(root, relative)
    before = path.stat()
    if before.st_size > limit:
        raise ValueError(f"{relative.as_posix()} exceeds its size limit")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        raw = stream.read(limit + 1)
        closed = os.fstat(stream.fileno())
    after = path.stat()
    if len(raw) > limit:
        raise ValueError(f"{relative.as_posix()} exceeds its size limit")
    identity_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    if any(
        getattr(before, field, None) != getattr(opened, field, None)
        or getattr(opened, field, None) != getattr(closed, field, None)
        or getattr(closed, field, None) != getattr(after, field, None)
        for field in identity_fields
    ):
        raise OSError(f"{relative.as_posix()} changed while it was read")
    return raw


def _parse_catalog(raw: bytes) -> dict[int, CatalogRecord]:
    if len(raw) < CATALOG_HEADER.size or len(raw) > CATALOG_FILE_LIMIT:
        raise ValueError("catalog.bin size is outside the supported bound")
    (
        magic,
        version,
        header_bytes,
        count,
        payload_bytes,
        payload_hash,
        reserved,
    ) = CATALOG_HEADER.unpack_from(raw)
    if (
        magic != CATALOG_MAGIC
        or version != CATALOG_VERSION
        or header_bytes != CATALOG_HEADER.size
        or reserved != 0
        or not 1 <= count <= CATALOG_RECORD_LIMIT
        or payload_bytes > CATALOG_PAYLOAD_LIMIT
    ):
        raise ValueError("catalog.bin has an unsupported header")
    if len(raw) != header_bytes + payload_bytes:
        raise ValueError("catalog.bin length does not match its header")
    payload = raw[header_bytes:]
    if _fnv1a32(payload) != payload_hash:
        raise ValueError("catalog.bin payload checksum does not match")

    records: dict[int, CatalogRecord] = {}
    cursor = header_bytes
    previous_id = 0
    for _ in range(count):
        if len(raw) - cursor < CATALOG_ENTRY.size:
            raise ValueError("catalog.bin has a truncated entry header")
        kind, identity, byte_count, record_hash, flags, entry_reserved = (
            CATALOG_ENTRY.unpack_from(raw, cursor)
        )
        cursor += CATALOG_ENTRY.size
        expected_bytes = RECORD_BYTES.get(kind)
        if (
            expected_bytes is None
            or byte_count != expected_bytes
            or identity < 1
            or identity > MAX_RECORD_ID
            or identity <= previous_id
            or flags != 0
            or entry_reserved != 0
            or len(raw) - cursor < byte_count
        ):
            raise ValueError("catalog.bin has an invalid or unsorted entry")
        record = raw[cursor : cursor + byte_count]
        cursor += byte_count
        if (
            _fnv1a32(record) != record_hash
            or struct.unpack_from("<I", record)[0] != identity
            or record[4] != 0
        ):
            raise ValueError("catalog.bin record identity, security byte, or hash is invalid")
        records[identity] = CatalogRecord(kind, identity, record)
        previous_id = identity
    if cursor != len(raw):
        raise ValueError("catalog.bin has trailing or unparsed payload bytes")
    return records


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _parse_receipt(raw: bytes) -> dict[str, Any]:
    try:
        decoded = raw.decode("utf-8")
        value = json.loads(
            decoded,
            object_pairs_hook=_unique_json_object,
            parse_constant=lambda constant: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON value: {constant}")
            ),
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("native lifecycle receipt is not valid UTF-8 JSON") from error
    return _exact_object(
        value,
        {
            "schema",
            "observer_scope",
            "run_id",
            "expected_ids",
            "clear_entered",
            "clear_returned",
            "app_final_entered",
            "app_final_returned",
            "singleton_null",
            "cross_record_pointers_distinct",
            "records",
            "passed",
        },
        "native lifecycle receipt",
    )


def _record_hex(value: Any, size: int) -> bytes:
    if (
        type(value) is not str
        or len(value) != size * 2
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValueError("receipt record_hex is not exact lowercase native bytes")
    return bytes.fromhex(value)


def verify_receipt(
    client_root: Path,
    ids: list[int],
    run_id: str,
    *,
    expected_catalog_sha256: str,
) -> dict[str, Any]:
    """Verify a native receipt against the exact sealed unit catalog."""

    expected_ids = _validate_ids(ids)
    if (
        type(run_id) is not str
        or not run_id
        or "\0" in run_id
        or len(run_id.encode("utf-8")) > 255
    ):
        raise ValueError("run_id is empty or outside the native observer bound")
    if type(expected_catalog_sha256) is not str or not SHA256.fullmatch(
        expected_catalog_sha256
    ):
        raise ValueError("expected_catalog_sha256 must be a SHA-256 hex digest")

    catalog_bytes = _read_bounded(
        client_root, CATALOG_RELATIVE, CATALOG_FILE_LIMIT
    )
    catalog_sha256 = hashlib.sha256(catalog_bytes).hexdigest()
    if not hmac.compare_digest(
        catalog_sha256, expected_catalog_sha256.lower()
    ):
        raise ValueError("sealed catalog SHA-256 does not match")
    catalog = _parse_catalog(catalog_bytes)
    selected: dict[int, CatalogRecord] = {}
    for identity in expected_ids:
        record = catalog.get(identity)
        if record is None:
            raise ValueError(f"expected id {identity} is absent from sealed catalog")
        if record.kind not in CAPTURE_KINDS:
            raise ValueError(f"expected id {identity} has a non-capturable kind")
        selected[identity] = record

    receipt_bytes = _read_bounded(client_root, RECEIPT_RELATIVE, RECEIPT_LIMIT)
    receipt = _parse_receipt(receipt_bytes)
    if receipt["schema"] != "sdgo.native-record-lifecycle/1":
        raise ValueError("native lifecycle receipt schema does not match")
    if receipt["observer_scope"] != OBSERVER_SCOPE:
        raise ValueError("native lifecycle observer scope does not match")
    if type(receipt["run_id"]) is not str or receipt["run_id"] != run_id:
        raise ValueError("native lifecycle receipt run_id does not match")
    receipt_ids = _validate_ids(receipt["expected_ids"], "receipt expected_ids")
    if receipt_ids != expected_ids:
        raise ValueError("native lifecycle receipt expected_ids do not match")
    for field in (
        "clear_entered",
        "clear_returned",
        "app_final_entered",
        "app_final_returned",
    ):
        _strict_one(receipt[field], field)
    if receipt["singleton_null"] is not True:
        raise ValueError("native singleton was not released")
    if receipt["cross_record_pointers_distinct"] is not True:
        raise ValueError("native record pointers are not globally distinct")
    if receipt["passed"] is not True:
        raise ValueError("native lifecycle receipt aggregate did not pass")

    rows = receipt["records"]
    if type(rows) is not list or len(rows) != len(expected_ids):
        raise ValueError("native lifecycle receipt has the wrong record count")
    observed: dict[int, dict[str, Any]] = {}
    all_pointers: set[int] = set()
    for index, candidate in enumerate(rows):
        row = _exact_object(
            candidate,
            {
                "id",
                "kind",
                "record_size",
                "record_hex",
                "pointers",
                "captures",
                "free_counts",
                "passed",
            },
            f"native lifecycle record {index}",
        )
        identity = _strict_int(row["id"], f"record {index} id")
        if identity in observed:
            raise ValueError(f"duplicate native lifecycle record id: {identity}")
        sealed = selected.get(identity)
        if sealed is None:
            raise ValueError(f"unexpected native lifecycle record id: {identity}")
        if _strict_int(row["kind"], f"record {identity} kind") != sealed.kind:
            raise ValueError(f"native lifecycle kind differs for id {identity}")
        if (
            _strict_int(row["record_size"], f"record {identity} size")
            != len(sealed.raw)
        ):
            raise ValueError(f"native lifecycle size differs for id {identity}")
        captured = _record_hex(row["record_hex"], len(sealed.raw))
        if captured != sealed.raw:
            raise ValueError(f"native lifecycle bytes differ for id {identity}")

        pointers = _exact_object(
            row["pointers"], set(POINTER_KEYS), f"record {identity} pointers"
        )
        for key in POINTER_KEYS:
            pointer = pointers[key]
            if type(pointer) is not str or not HEX_POINTER.fullmatch(pointer):
                raise ValueError(f"record {identity} {key} pointer is not 0x8hex")
            address = int(pointer[2:], 16)
            if address == 0 or address in all_pointers:
                raise ValueError(f"record {identity} {key} pointer is zero or aliased")
            all_pointers.add(address)

        captures = _exact_object(
            row["captures"],
            set(CAPTURE_KEYS) | {"conflict"},
            f"record {identity} captures",
        )
        for key in CAPTURE_KEYS:
            _strict_one(captures[key], f"record {identity} capture {key}")
        if captures["conflict"] is not False:
            raise ValueError(f"record {identity} has a native capture conflict")

        free_counts = _exact_object(
            row["free_counts"],
            set(POINTER_KEYS),
            f"record {identity} free_counts",
        )
        for key in POINTER_KEYS:
            _strict_one(free_counts[key], f"record {identity} free count {key}")
        if row["passed"] is not True:
            raise ValueError(f"native lifecycle row did not pass for id {identity}")
        observed[identity] = row
    if set(observed) != set(expected_ids):
        raise ValueError("native lifecycle receipt is missing expected records")

    return {
        "status": "pass",
        "receipt_sha256": hashlib.sha256(receipt_bytes).hexdigest(),
        "records": [
            {
                "id": identity,
                "kind": selected[identity].kind,
                "size": len(selected[identity].raw),
                "sha256": hashlib.sha256(selected[identity].raw).hexdigest(),
            }
            for identity in expected_ids
        ],
        "observer_scope": OBSERVER_SCOPE,
        "native_actor_lifecycle_accepted": False,
    }
