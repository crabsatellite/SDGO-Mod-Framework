"""Pure client-table compiler for owner-reviewed imported activity machines.

The native activity page is a continuous ``0099.gdt`` grid.  This adapter
appends or updates only records authenticated by a prior ownership receipt.
It has no database, filesystem, deployment, or runtime side effects.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import io
import re
import struct
from collections.abc import Mapping, Sequence
from typing import Any

from owner.collection_capsule import client as capsule_client
from owner.units.assets import normalize_png
from owner.units.project import normalize_metadata


SCHEMA_VERSION = 1
OWNER = "unit_import/activity/v1"
TABLE_RESOURCE = "gdrs/0099.gdt"
RESOURCE_ID_MIN = 62000
RESOURCE_ID_MAX = 65400
RESERVED_RESOURCE_IDS = frozenset((65534, 65535))
MAX_PROJECT_ASSET_BYTES = 40 * 1024 * 1024
_HASH = re.compile(r"[0-9a-f]{64}")
_PROJECT = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
_NUMERIC_RESOURCE = re.compile(r"(sprs|txrs)/([0-9]{5})\.(spr|txr)")
_OWNERSHIP_FIELDS = {
    "schema_version",
    "owner",
    "machine_id",
    "grid_id",
    "sprite_path",
    "texture_path",
    "row_sha256",
    "sprite_sha256",
    "texture_sha256",
    "project_key",
    "unit_id",
    "artwork_sha256",
}
_MACHINE_FIELDS = {
    "schema_version",
    "id",
    "name",
    "unit_id",
    "project_key",
    "currency_type",
    "price",
    "artwork_key",
    "artwork_sha256",
}


class ActivityClientError(ValueError):
    """The requested activity-table update is unsafe or incomplete."""


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _integer(value: Any, label: str, minimum: int, maximum: int) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ActivityClientError(f"{label} must be an integer in {minimum}..{maximum}")
    return value


def _hash(value: Any, label: str) -> str:
    if not isinstance(value, str) or _HASH.fullmatch(value) is None:
        raise ActivityClientError(f"{label} must be a lowercase SHA-256")
    return value


def _project(value: Any, label: str = "project_key") -> str:
    if not isinstance(value, str) or _PROJECT.fullmatch(value) is None:
        raise ActivityClientError(f"{label} is invalid")
    return value


def _path(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise ActivityClientError(f"{label} must be text")
    normalized = value.replace("\\", "/").lower()
    if (
        not normalized
        or len(normalized) > 240
        or ":" in normalized
        or any(ord(character) < 32 or ord(character) > 126 for character in normalized)
        or any(part in ("", ".", "..") for part in normalized.split("/"))
    ):
        raise ActivityClientError(f"{label} is invalid")
    return normalized


def _resource_map(resources: Mapping[str, bytes]) -> tuple[dict[str, bytes], dict[str, str]]:
    if not isinstance(resources, Mapping):
        raise ActivityClientError("resources must be a mapping")
    copied: dict[str, bytes] = {}
    normalized: dict[str, str] = {}
    for supplied, raw in resources.items():
        name = _path(supplied, "resource path")
        if name in normalized:
            raise ActivityClientError(f"duplicate normalized resource path: {name}")
        if type(raw) is not bytes:
            raise ActivityClientError(f"resource {name} must contain bytes")
        normalized[name] = supplied
        copied[supplied] = raw
    return copied, normalized


def _decode_assets(project_assets: Sequence[Mapping[str, Any]]) -> dict[str, dict[str, Any]]:
    if (
        not isinstance(project_assets, Sequence)
        or isinstance(project_assets, (str, bytes, bytearray))
        or len(project_assets) > 2048
    ):
        raise ActivityClientError("project_assets must be a list with at most 2048 entries")
    decoded: dict[str, dict[str, Any]] = {}
    total = 0
    for index, supplied in enumerate(project_assets):
        if not isinstance(supplied, Mapping):
            raise ActivityClientError(f"project asset {index} must be an object")
        row = dict(supplied)
        project_key = _project(row.get("project_key"), f"project asset {index} project_key")
        role = row.get("role")
        key = row.get("key")
        if role not in ("activity_poster", "shop_portrait", "capsule_portrait"):
            raise ActivityClientError(f"project asset {index} has an unsupported role")
        if key != f"{project_key}:{role}" or key in decoded:
            raise ActivityClientError(f"project asset {index} identity is invalid or duplicate")
        for field in (
            "asset_sha256",
            "source_asset_sha256",
            "original_sha256",
            "project_revision",
        ):
            _hash(row.get(field), f"project asset {key} {field}")
        encoded = row.get("png_base64")
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (ValueError, TypeError):
            raise ActivityClientError(f"project asset {key} PNG encoding is invalid") from None
        total += len(raw)
        if total > MAX_PROJECT_ASSET_BYTES:
            raise ActivityClientError("project assets exceed the portable 40 MiB limit")
        if _sha256(raw) != row["asset_sha256"]:
            raise ActivityClientError(f"project asset {key} PNG hash differs")
        try:
            normalize_png(raw)
            metadata = normalize_metadata(row.get("metadata", {}))
        except ValueError as error:
            raise ActivityClientError(f"project asset {key}: {error}") from None
        decoded[key] = {
            **row,
            "metadata": metadata,
            "raw": raw,
        }
    return decoded


def _machines(
    machines: Sequence[Mapping[str, Any]],
    assets: Mapping[str, Mapping[str, Any]],
) -> list[dict[str, Any]]:
    if (
        not isinstance(machines, Sequence)
        or isinstance(machines, (str, bytes, bytearray))
        or len(machines) > 999
    ):
        raise ActivityClientError("machines must be a list with at most 999 entries")
    result: list[dict[str, Any]] = []
    machine_ids: set[int] = set()
    unit_ids: set[int] = set()
    project_keys: set[str] = set()
    for index, supplied in enumerate(machines):
        if not isinstance(supplied, Mapping) or set(supplied) != _MACHINE_FIELDS:
            raise ActivityClientError(f"activity machine {index} fields differ from schema v1")
        row = dict(supplied)
        if row["schema_version"] != SCHEMA_VERSION:
            raise ActivityClientError(f"activity machine {index} schema is unsupported")
        machine_id = _integer(row["id"], f"activity machine {index} id", 1, 0x7FFFFFFF)
        unit_id = _integer(
            row["unit_id"], f"activity machine {machine_id} unit_id", 1, 9_999_999
        )
        if machine_id in machine_ids:
            raise ActivityClientError(f"duplicate activity machine ID: {machine_id}")
        if unit_id in unit_ids:
            raise ActivityClientError(f"duplicate dedicated activity unit: {unit_id}")
        machine_ids.add(machine_id)
        unit_ids.add(unit_id)
        name = row["name"]
        if (
            not isinstance(name, str)
            or not name.strip()
            or len(name) > 128
            or "\0" in name
            or any(ord(character) < 32 for character in name)
        ):
            raise ActivityClientError(f"activity machine {machine_id} name is invalid")
        project_key = _project(
            row["project_key"], f"activity machine {machine_id} project_key"
        )
        if project_key in project_keys:
            raise ActivityClientError(
                f"duplicate dedicated activity project: {project_key}"
            )
        project_keys.add(project_key)
        currency = row["currency_type"]
        if type(currency) is not int or currency not in (0, 2):
            raise ActivityClientError(
                f"activity machine {machine_id} currency_type must be 0 or 2"
            )
        price = _integer(
            row["price"], f"activity machine {machine_id} price", 1, 0x7FFFFFFF
        )
        artwork_sha256 = _hash(
            row["artwork_sha256"],
            f"activity machine {machine_id} artwork_sha256",
        )
        artwork_key = row["artwork_key"]
        if artwork_key != f"{project_key}:activity_poster":
            raise ActivityClientError(
                f"activity machine {machine_id} artwork_key belongs to another project"
            )
        artwork = assets.get(artwork_key)
        if (
            artwork is None
            or artwork["role"] != "activity_poster"
            or artwork["project_key"] != project_key
            or artwork["asset_sha256"] != artwork_sha256
        ):
            raise ActivityClientError(
                f"activity machine {machine_id} artwork is absent or stale"
            )
        result.append(
            {
                **row,
                "name": name.strip(),
                "id": machine_id,
                "unit_id": unit_id,
                "project_key": project_key,
                "price": price,
                "artwork": artwork,
            }
        )
    return result


def _texture_references(raw: bytes) -> set[str]:
    """Read conservative NUL-terminated ASCII paths from UTF-16 SPR fields."""
    result: set[str] = set()
    for alignment in (0, 1):
        run = bytearray()
        for offset in range(alignment, len(raw) - 1, 2):
            low, high = raw[offset], raw[offset + 1]
            if high == 0 and 0x20 <= low <= 0x7E:
                run.append(low)
                if len(run) > 240:
                    run.clear()
                continue
            if low == 0 and high == 0 and run:
                try:
                    value = run.decode("ascii").replace("\\", "/").lower()
                except UnicodeDecodeError:
                    value = ""
                if re.fullmatch(r"txrs/[0-9]{5}\.txr", value):
                    result.add(value)
            run.clear()
    return result


def _numeric_id(path: str, kind: str) -> int | None:
    match = _NUMERIC_RESOURCE.fullmatch(path)
    if match is None or match.group(1) != kind or match.group(3) != kind[0:3]:
        return None
    return int(match.group(2))


def _occupied_resource_ids(
    rows: Sequence[bytes],
    resources: Mapping[str, bytes],
    normalized: Mapping[str, str],
) -> set[int]:
    occupied = set(RESERVED_RESOURCE_IDS)
    for name in normalized:
        match = _NUMERIC_RESOURCE.fullmatch(name)
        if match is not None:
            occupied.add(int(match.group(2)))
    for row in rows:
        try:
            supplied_sprite = capsule_client._activity_sprite(row)
        except capsule_client.ClientRegistryError as error:
            raise ActivityClientError(str(error)) from None
        if not supplied_sprite:
            continue
        sprite = _path(supplied_sprite, "0099 activity sprite path")
        resource_id = _numeric_id(sprite, "sprs")
        if resource_id is not None:
            occupied.add(resource_id)
    for normalized_name, supplied_name in normalized.items():
        if not normalized_name.endswith(".spr"):
            continue
        for texture in _texture_references(resources[supplied_name]):
            resource_id = _numeric_id(texture, "txrs")
            if resource_id is not None:
                occupied.add(resource_id)
    return occupied


def _ownership(
    supplied: Sequence[Mapping[str, Any]],
    rows: Sequence[bytes],
    rows_by_machine: Mapping[int, Sequence[int]],
    resources: Mapping[str, bytes],
    normalized: Mapping[str, str],
) -> tuple[dict[int, dict[str, Any]], dict[int, str]]:
    if (
        not isinstance(supplied, Sequence)
        or isinstance(supplied, (str, bytes, bytearray))
        or len(supplied) > 999
    ):
        raise ActivityClientError("ownership must be a list with at most 999 entries")
    result: dict[int, dict[str, Any]] = {}
    actions: dict[int, str] = {}
    grids: set[int] = set()
    paths: set[str] = set()
    units: set[int] = set()
    projects: set[str] = set()
    for index, raw_entry in enumerate(supplied):
        if not isinstance(raw_entry, Mapping) or set(raw_entry) != _OWNERSHIP_FIELDS:
            raise ActivityClientError(f"activity ownership {index} fields differ from schema v1")
        entry = dict(raw_entry)
        if entry["schema_version"] != SCHEMA_VERSION or entry["owner"] != OWNER:
            raise ActivityClientError(f"activity ownership {index} identity is unsupported")
        machine_id = _integer(entry["machine_id"], "owned machine_id", 1, 0x7FFFFFFF)
        grid_id = _integer(entry["grid_id"], "owned grid_id", 145001, 0xFFFFFFFF)
        unit_id = _integer(entry["unit_id"], "owned unit_id", 1, 9_999_999)
        project_key = _project(entry["project_key"], "owned project_key")
        artwork_sha256 = _hash(entry["artwork_sha256"], "owned artwork_sha256")
        sprite_path = _path(entry["sprite_path"], "owned sprite_path")
        texture_path = _path(entry["texture_path"], "owned texture_path")
        sprite_id = _numeric_id(sprite_path, "sprs")
        texture_id = _numeric_id(texture_path, "txrs")
        if (
            sprite_id is None
            or texture_id is None
            or sprite_id != texture_id
            or not RESOURCE_ID_MIN <= sprite_id <= RESOURCE_ID_MAX
        ):
            raise ActivityClientError(
                f"activity ownership {machine_id} resource pair is outside the owned range"
            )
        for field in ("row_sha256", "sprite_sha256", "texture_sha256"):
            _hash(entry[field], f"activity ownership {machine_id} {field}")
        if (
            machine_id in result
            or grid_id in grids
            or sprite_path in paths
            or texture_path in paths
            or unit_id in units
            or project_key in projects
        ):
            raise ActivityClientError("activity ownership identities overlap")
        row_indexes = rows_by_machine.get(machine_id, ())
        if not row_indexes:
            raise ActivityClientError(f"owned activity machine {machine_id} is missing")
        if len(row_indexes) != 1:
            raise ActivityClientError(
                f"owned activity machine {machine_id} appears more than once"
            )
        row_index = row_indexes[0]
        row = rows[row_index]
        if struct.unpack_from("<I", row, 0)[0] != grid_id:
            raise ActivityClientError(f"owned activity machine {machine_id} grid changed")
        try:
            current_sprite = _path(
                capsule_client._activity_sprite(row),
                "owned activity sprite path",
            )
        except capsule_client.ClientRegistryError as error:
            raise ActivityClientError(str(error)) from None
        if current_sprite != sprite_path or _sha256(row) != entry["row_sha256"]:
            raise ActivityClientError(f"owned activity machine {machine_id} row was edited")
        sprite_key = normalized.get(sprite_path)
        texture_key = normalized.get(texture_path)
        if sprite_key is None or texture_key is None:
            raise ActivityClientError(
                f"owned activity machine {machine_id} resources are missing"
            )
        sprite = resources[sprite_key]
        texture = resources[texture_key]
        if (
            _sha256(sprite) != entry["sprite_sha256"]
            or _sha256(texture) != entry["texture_sha256"]
            or texture_path not in _texture_references(sprite)
        ):
            raise ActivityClientError(
                f"owned activity machine {machine_id} resources were edited"
            )
        entry.update(
            machine_id=machine_id,
            grid_id=grid_id,
            unit_id=unit_id,
            project_key=project_key,
            artwork_sha256=artwork_sha256,
            sprite_path=sprite_path,
            texture_path=texture_path,
        )
        result[machine_id] = entry
        actions[machine_id] = "retained"
        grids.add(grid_id)
        units.add(unit_id)
        projects.add(project_key)
        paths.update((sprite_path, texture_path))
    owned_sprites = {
        entry["sprite_path"]: machine_id for machine_id, entry in result.items()
    }
    owned_textures = {
        entry["texture_path"]: entry["sprite_path"] for entry in result.values()
    }
    for row_index, row in enumerate(rows):
        try:
            supplied_sprite = capsule_client._activity_sprite(row)
        except capsule_client.ClientRegistryError as error:
            raise ActivityClientError(str(error)) from None
        if not supplied_sprite:
            continue
        sprite_path = _path(supplied_sprite, "0099 activity sprite path")
        machine_id = owned_sprites.get(sprite_path)
        if machine_id is not None and rows_by_machine[machine_id][0] != row_index:
            raise ActivityClientError(
                f"owned activity sprite is shared by foreign machine: {sprite_path}"
            )
    for normalized_name, supplied_name in normalized.items():
        if not normalized_name.endswith(".spr"):
            continue
        for texture_path in _texture_references(resources[supplied_name]):
            expected_sprite = owned_textures.get(texture_path)
            if expected_sprite is not None and normalized_name != expected_sprite:
                raise ActivityClientError(
                    f"owned activity texture is shared by foreign sprite: {texture_path}"
                )
    return result, actions


def _machine_rows(rows: Sequence[bytes]) -> dict[int, list[int]]:
    result: dict[int, list[int]] = {}
    for index, row in enumerate(rows):
        machine_id = struct.unpack_from("<I", row, 5)[0]
        result.setdefault(machine_id, []).append(index)
    return result


def _make_row(grid_id: int, machine: Mapping[str, Any], sprite_path: str) -> bytes:
    row = bytearray(capsule_client.TABLE_SIZES[capsule_client.ACTIVITY_TABLE])
    struct.pack_into("<I", row, 0, grid_id)
    struct.pack_into("<I", row, 5, machine["id"])
    struct.pack_into("<I", row, 9, machine["currency_type"])
    struct.pack_into("<I", row, 15, machine["price"])
    try:
        capsule_client._put_wide(
            row,
            capsule_client.ACTIVITY_SPRITE_OFFSET,
            capsule_client.ACTIVITY_SPRITE_CAPACITY,
            sprite_path.replace("/", "\\"),
        )
    except capsule_client.ClientRegistryError as error:
        raise ActivityClientError(str(error)) from None
    return bytes(row)


def _set_resource(
    resources: dict[str, bytes],
    normalized: dict[str, str],
    path: str,
    raw: bytes,
) -> None:
    supplied = normalized.get(path)
    if supplied is None:
        resources[path] = raw
        normalized[path] = path
    else:
        resources[supplied] = raw


def compile_activity_client(
    raw_gdt: bytes,
    resources: Mapping[str, bytes],
    machines: Sequence[Mapping[str, Any]],
    project_assets: Sequence[Mapping[str, Any]],
    *,
    ownership: Sequence[Mapping[str, Any]] = (),
    reserved_resource_ids: Sequence[int] = (),
) -> dict[str, Any]:
    """Compile reviewed dedicated activity machines into a provider resource set.

    Omitted prior-owned machines are retained.  There is deliberately no delete
    operation in this pure adapter.
    """
    if type(raw_gdt) is not bytes:
        raise ActivityClientError("raw_gdt must contain bytes")
    output_resources, normalized_resources = _resource_map(resources)
    existing_table_key = normalized_resources.get(TABLE_RESOURCE)
    if (
        existing_table_key is not None
        and output_resources[existing_table_key] != raw_gdt
    ):
        raise ActivityClientError("resources contain a different 0099.gdt")
    try:
        rows, extra = capsule_client._validate_table(
            capsule_client.ACTIVITY_TABLE, raw_gdt
        )
    except capsule_client.ClientRegistryError as error:
        raise ActivityClientError(str(error)) from None
    rows = list(rows)
    original_rows = tuple(rows)
    rows_by_machine = _machine_rows(rows)
    assets = _decode_assets(project_assets)
    normalized_machines = _machines(machines, assets)
    owned, actions = _ownership(
        ownership,
        rows,
        rows_by_machine,
        output_resources,
        normalized_resources,
    )
    owned_units = {
        entry["unit_id"]: machine_id for machine_id, entry in owned.items()
    }
    owned_projects = {
        entry["project_key"]: machine_id for machine_id, entry in owned.items()
    }
    occupied = _occupied_resource_ids(
        rows, output_resources, normalized_resources
    )
    if (not isinstance(reserved_resource_ids, (list, tuple, set, frozenset)) or
        any(type(value) is not int or not 0 <= value <= 65535 for value in reserved_resource_ids)):
        raise ActivityClientError("reserved resource IDs must be native numeric IDs")
    occupied.update(reserved_resource_ids)
    available = iter(
        resource_id
        for resource_id in range(RESOURCE_ID_MIN, RESOURCE_ID_MAX + 1)
        if resource_id not in occupied
    )

    machine_details: dict[int, dict[str, Any]] = {}
    for machine in normalized_machines:
        machine_id = machine["id"]
        prior = owned.get(machine_id)
        row_indexes = rows_by_machine.get(machine_id, ())
        if len(row_indexes) > 1:
            raise ActivityClientError(
                f"activity machine ID {machine_id} appears more than once"
            )
        row_index = row_indexes[0] if row_indexes else None
        if row_index is not None and prior is None:
            raise ActivityClientError(
                f"activity machine ID {machine_id} is occupied by a foreign row"
            )
        if prior is not None:
            if (
                prior["project_key"] != machine["project_key"]
                or prior["unit_id"] != machine["unit_id"]
            ):
                raise ActivityClientError(
                    f"activity machine {machine_id} cannot change project or unit ownership"
                )
            resource_id = _numeric_id(prior["sprite_path"], "sprs")
            sprite_path = prior["sprite_path"]
            texture_path = prior["texture_path"]
            grid_id = prior["grid_id"]
            if resource_id is None or row_index is None:
                raise ActivityClientError(
                    f"activity ownership {machine_id} failed its verified invariant"
                )
        else:
            unit_owner = owned_units.get(machine["unit_id"])
            project_owner = owned_projects.get(machine["project_key"])
            if unit_owner is not None or project_owner is not None:
                raise ActivityClientError(
                    f"activity machine {machine_id} duplicates a retained owned project or unit"
                )
            try:
                resource_id = next(available)
            except StopIteration:
                raise ActivityClientError(
                    f"no free paired SPR/TXR ID remains in "
                    f"{RESOURCE_ID_MIN}..{RESOURCE_ID_MAX}"
                ) from None
            occupied.add(resource_id)
            sprite_path = f"sprs/{resource_id:05d}.spr"
            texture_path = f"txrs/{resource_id:05d}.txr"
            row_index = len(rows)
            grid_id = capsule_client.TABLE_BASES[capsule_client.ACTIVITY_TABLE] + row_index
            rows.append(bytes(capsule_client.TABLE_SIZES[capsule_client.ACTIVITY_TABLE]))
            rows_by_machine[machine_id] = [row_index]
            owned_units[machine["unit_id"]] = machine_id
            owned_projects[machine["project_key"]] = machine_id

        try:
            normalized_cover = capsule_client.normalize_activity_cover(
                machine["artwork"]["raw"]
            )
            texture, uv, cover_size = capsule_client._card_texture(
                io.BytesIO(normalized_cover)
            )
            sprite = capsule_client._standalone_sprite(
                texture_path.replace("/", "\\"),
                cover_size[0],
                cover_size[1],
                uv,
            )
        except (ValueError, OSError, capsule_client.ClientRegistryError) as error:
            raise ActivityClientError(
                f"activity machine {machine_id} poster compilation failed: {error}"
            ) from None
        new_row = _make_row(grid_id, machine, sprite_path)
        old_row = rows[row_index]
        old_sprite = (
            output_resources[normalized_resources[sprite_path]]
            if sprite_path in normalized_resources
            else None
        )
        old_texture = (
            output_resources[normalized_resources[texture_path]]
            if texture_path in normalized_resources
            else None
        )
        if prior is None:
            action = "added"
        elif (
            old_row == new_row
            and old_sprite == sprite
            and old_texture == texture
            and prior["artwork_sha256"] == machine["artwork_sha256"]
        ):
            action = "unchanged"
        else:
            action = "updated"
        rows[row_index] = new_row
        _set_resource(
            output_resources, normalized_resources, texture_path, texture
        )
        _set_resource(
            output_resources, normalized_resources, sprite_path, sprite
        )
        entry = {
            "schema_version": SCHEMA_VERSION,
            "owner": OWNER,
            "machine_id": machine_id,
            "grid_id": grid_id,
            "sprite_path": sprite_path,
            "texture_path": texture_path,
            "row_sha256": _sha256(new_row),
            "sprite_sha256": _sha256(sprite),
            "texture_sha256": _sha256(texture),
            "project_key": machine["project_key"],
            "unit_id": machine["unit_id"],
            "artwork_sha256": machine["artwork_sha256"],
        }
        owned[machine_id] = entry
        actions[machine_id] = action
        machine_details[machine_id] = {
            "name": machine["name"],
            "asset": {
                field: machine["artwork"][field]
                for field in (
                    "key",
                    "asset_sha256",
                    "source_asset_sha256",
                    "original_sha256",
                    "project_revision",
                )
            },
            "normalized_poster_sha256": _sha256(normalized_cover),
        }

    try:
        encoded_gdt = capsule_client.encode_gdt(
            capsule_client.TABLE_SIZES[capsule_client.ACTIVITY_TABLE],
            rows,
            extra,
        )
    except capsule_client.ClientRegistryError as error:
        raise ActivityClientError(str(error)) from None
    _set_resource(
        output_resources,
        normalized_resources,
        TABLE_RESOURCE,
        encoded_gdt,
    )

    placements = []
    for machine_id, entry in sorted(owned.items(), key=lambda item: item[1]["grid_id"]):
        row_index = entry["grid_id"] - capsule_client.TABLE_BASES[
            capsule_client.ACTIVITY_TABLE
        ]
        if not 0 <= row_index < len(rows):
            raise ActivityClientError(f"owned activity machine {machine_id} grid is invalid")
        row = rows[row_index]
        native_position = row_index + 1
        detail = machine_details.get(machine_id)
        placements.append(
            {
                "schema_version": SCHEMA_VERSION,
                "owner": OWNER,
                "action": actions[machine_id],
                "machine_id": machine_id,
                "name": detail["name"] if detail else None,
                "unit_id": entry["unit_id"],
                "project_key": entry["project_key"],
                "artwork_sha256": entry["artwork_sha256"],
                "table": capsule_client.ACTIVITY_TABLE,
                "grid_type": capsule_client.ACTIVITY_GRID_TYPE,
                "grid_id": entry["grid_id"],
                "row_index": row_index,
                "native_position": native_position,
                "page": (native_position - 1) // 8 + 1,
                "page_slot": (native_position - 1) % 8 + 1,
                "currency_type": struct.unpack_from("<I", row, 9)[0],
                "price": struct.unpack_from("<I", row, 15)[0],
                "resource_id": _numeric_id(entry["sprite_path"], "sprs"),
                "sprite_path": entry["sprite_path"],
                "texture_path": entry["texture_path"],
                "row_sha256": entry["row_sha256"],
                "sprite_sha256": entry["sprite_sha256"],
                "texture_sha256": entry["texture_sha256"],
                "asset": copy.deepcopy(detail["asset"]) if detail else None,
                "normalized_poster_sha256": (
                    detail["normalized_poster_sha256"] if detail else None
                ),
            }
        )

    return {
        "schema_version": SCHEMA_VERSION,
        "owner": OWNER,
        "gdt": encoded_gdt,
        "resources": output_resources,
        "placements": placements,
        "ownership": [
            copy.deepcopy(entry)
            for _, entry in sorted(owned.items(), key=lambda item: item[1]["grid_id"])
        ],
        "metadata": {
            "table": capsule_client.ACTIVITY_TABLE,
            "table_resource": TABLE_RESOURCE,
            "row_size": capsule_client.TABLE_SIZES[capsule_client.ACTIVITY_TABLE],
            "grid_type": capsule_client.ACTIVITY_GRID_TYPE,
            "grid_base": capsule_client.TABLE_BASES[capsule_client.ACTIVITY_TABLE],
            "extra": extra,
            "original_row_count": len(original_rows),
            "final_row_count": len(rows),
            "retention": "append_or_authenticated_update_only",
            "resource_id_range": [RESOURCE_ID_MIN, RESOURCE_ID_MAX],
            "reserved_resource_ids": sorted(RESERVED_RESOURCE_IDS),
            "gdt_sha256": _sha256(encoded_gdt),
        },
    }
