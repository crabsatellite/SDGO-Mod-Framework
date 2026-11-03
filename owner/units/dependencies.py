"""Typed, owner-scoped dependency IR for native unit and skill records."""
from __future__ import annotations

import copy
import re
from typing import Any, Mapping


SCHEMA = "sdgo.native-dependencies/1"
MAX_RECORDS = 4096
MAX_FILES = 512
MAX_EDGES = 16384

DEPENDENCY_KINDS = frozenset(
    {
        "skill-icon-sprite-texture",
        "effect-zfx-texture",
        "linked-skill",
        "feature-dispatch",
        "unit-skill-slot",
        "server-skill-fields",
    }
)
FILE_DEPENDENCY_KINDS = frozenset(
    {"skill-icon-sprite-texture", "effect-zfx-texture"}
)
DEPENDENCY_STATES = frozenset({"resolved", "unknown", "missing"})
_SHA256 = re.compile(r"[0-9a-f]{64}")
_OWNER = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
_DEPENDENCY_ID = re.compile(r"[a-z0-9][a-z0-9._:/-]{0,159}")


def _exact_dict(
    value: Any, allowed: set[str], required: set[str], label: str
) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{label} must be an object")
    unknown = set(value) - allowed
    missing = required - set(value)
    if unknown:
        raise ValueError(f"{label} has unknown fields: {sorted(unknown)}")
    if missing:
        raise ValueError(f"{label} is missing fields: {sorted(missing)}")
    return value


def _text(value: Any, label: str, *, maximum: int = 240) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > maximum
        or any(ord(character) < 32 for character in value)
    ):
        raise ValueError(f"{label} must be nonempty bounded text")
    return value


def _hash(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _SHA256.fullmatch(value):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def _integer(value: Any, label: str, minimum: int, maximum: int) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not minimum <= value <= maximum
    ):
        raise ValueError(f"{label} must be in {minimum}..{maximum}")
    return value


def _owner(value: Any) -> str:
    if not isinstance(value, str) or not _OWNER.fullmatch(value):
        raise ValueError("dependency owner is invalid")
    return value


def _dependency_id(value: Any) -> str:
    if not isinstance(value, str) or not _DEPENDENCY_ID.fullmatch(value):
        raise ValueError("dependency id is invalid")
    return value


def _blockers(value: Any, label: str) -> list[str]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be a list")
    result = [_text(item, label) for item in value]
    if len(result) != len(set(result)):
        raise ValueError(f"{label} contains duplicates")
    return result


def validate_manifest(value: Any) -> dict[str, Any]:
    """Validate dependency IR without converting unknowns into defaults."""
    manifest = _exact_dict(
        value,
        {"schema", "owner", "records", "edges"},
        {"schema", "owner", "records", "edges"},
        "dependency manifest",
    )
    if manifest["schema"] != SCHEMA:
        raise ValueError("unsupported dependency manifest schema")
    owner = _owner(manifest["owner"])

    records = manifest["records"]
    edges = manifest["edges"]
    if not isinstance(records, list) or len(records) > MAX_RECORDS:
        raise ValueError(f"dependency manifest exceeds {MAX_RECORDS} records")
    if not isinstance(edges, list) or len(edges) > MAX_EDGES:
        raise ValueError(f"dependency manifest exceeds {MAX_EDGES} edges")

    normalized_edges: list[dict[str, Any]] = []
    edge_ids: set[str] = set()
    unresolved_required: dict[str, str] = {}
    file_count = 0
    for index, raw in enumerate(edges):
        edge = _exact_dict(
            raw,
            {
                "id",
                "owner",
                "kind",
                "reference",
                "state",
                "required",
                "sha256",
                "proof_sha256",
            },
            {"id", "owner", "kind", "reference", "state", "required"},
            f"dependency edge {index}",
        )
        edge_id = _dependency_id(edge["id"])
        if edge_id in edge_ids:
            raise ValueError(f"duplicate dependency id: {edge_id}")
        edge_ids.add(edge_id)
        if _owner(edge["owner"]) != owner:
            raise ValueError(f"dependency edge {edge_id} crosses owners")
        kind = edge["kind"]
        if kind not in DEPENDENCY_KINDS:
            raise ValueError(f"dependency edge {edge_id} has unsupported kind")
        state = edge["state"]
        if state not in DEPENDENCY_STATES:
            raise ValueError(f"dependency edge {edge_id} has invalid state")
        required = edge["required"]
        if not isinstance(required, bool):
            raise ValueError(f"dependency edge {edge_id} required must be bool")
        reference = _text(edge["reference"], f"dependency edge {edge_id} reference")
        artifact_hash = edge.get("sha256")
        proof_hash = edge.get("proof_sha256")
        if artifact_hash is not None:
            artifact_hash = _hash(
                artifact_hash, f"dependency edge {edge_id} artifact"
            )
        if proof_hash is not None:
            proof_hash = _hash(
                proof_hash, f"dependency edge {edge_id} proof"
            )
        if kind in FILE_DEPENDENCY_KINDS:
            file_count += 1
            if state == "resolved" and artifact_hash is None:
                raise ValueError(
                    f"resolved file dependency {edge_id} needs SHA-256"
                )
        if state == "resolved" and proof_hash is None:
            raise ValueError(
                f"resolved dependency {edge_id} needs proof SHA-256"
            )
        if required and state != "resolved":
            unresolved_required[edge_id] = state
        normalized_edges.append(
            {
                "id": edge_id,
                "owner": owner,
                "kind": kind,
                "reference": reference,
                "state": state,
                "required": required,
                "sha256": artifact_hash,
                "proof_sha256": proof_hash,
            }
        )
    if file_count > MAX_FILES:
        raise ValueError(f"dependency manifest exceeds {MAX_FILES} files")

    normalized_records: list[dict[str, Any]] = []
    skill_ids: set[int] = set()
    record_blockers: list[str] = []
    for index, raw in enumerate(records):
        record = _exact_dict(
            raw,
            {
                "skill_id",
                "source_row",
                "profile_id",
                "source_sha256",
                "target_sha256",
                "dispatch",
                "dependency_ids",
                "blockers",
            },
            {
                "skill_id",
                "source_row",
                "profile_id",
                "source_sha256",
                "target_sha256",
                "dispatch",
                "dependency_ids",
                "blockers",
            },
            f"dependency record {index}",
        )
        skill_id = _integer(record["skill_id"], "skill_id", 1, 9_999_999)
        if skill_id in skill_ids:
            raise ValueError(f"duplicate dependency skill: {skill_id}")
        skill_ids.add(skill_id)
        dependency_ids = record["dependency_ids"]
        if not isinstance(dependency_ids, list):
            raise ValueError(f"skill {skill_id} dependency_ids must be a list")
        normalized_ids = [_dependency_id(item) for item in dependency_ids]
        if len(normalized_ids) != len(set(normalized_ids)):
            raise ValueError(f"skill {skill_id} has duplicate dependencies")
        missing_edges = sorted(set(normalized_ids) - edge_ids)
        if missing_edges:
            raise ValueError(
                f"skill {skill_id} references unknown dependencies: "
                f"{missing_edges}"
            )
        blockers = _blockers(record["blockers"], f"skill {skill_id} blockers")
        record_blockers.extend(f"skill:{skill_id}:{item}" for item in blockers)
        normalized_records.append(
            {
                "skill_id": skill_id,
                "source_row": _integer(
                    record["source_row"], "source_row", 0, 9_999_999
                ),
                "profile_id": _text(
                    record["profile_id"], "profile_id", maximum=96
                ),
                "source_sha256": _hash(
                    record["source_sha256"], "source record"
                ),
                "target_sha256": _hash(
                    record["target_sha256"], "target record"
                ),
                "dispatch": _integer(record["dispatch"], "dispatch", 0, 255),
                "dependency_ids": normalized_ids,
                "blockers": blockers,
            }
        )

    unresolved = [
        f"dependency:{edge_id}:{state}"
        for edge_id, state in sorted(unresolved_required.items())
    ]
    blockers = [
        "native consumer kind-13 capability is not enabled",
        "registration and initialization lifecycle is unresolved",
        *unresolved,
        *record_blockers,
    ]
    return {
        "schema": SCHEMA,
        "owner": owner,
        "records": copy.deepcopy(normalized_records),
        "edges": copy.deepcopy(normalized_edges),
        "admission": {
            "record_count": len(normalized_records),
            "edge_count": len(normalized_edges),
            "file_count": file_count,
            "required_dependencies_resolved": not unresolved_required,
            "native_registration_blocked": True,
            "native_runtime_accepted": False,
            "blockers": blockers,
        },
    }
