"""Lossless inventory for Union Kudiz/OBS edge-format model records.

This module does not interpret event semantics or admit actions for gameplay.
It preserves the physical common-action table, including records that the
native loader can only skip through its caller-supplied action filter.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import struct
from typing import Any


KUDIZ_MATERIAL_ENTRY_SIZE = 0x144
ACTION_TEXT_SIZE = 0x80
RESOURCE_TEXT_SIZE = 0x40
RESOURCE_ENTRY_SIZE = 4 + RESOURCE_TEXT_SIZE
EVENT_SIZE = 0xAC
VERSION_WITH_ENTRY_END = 0xC6E8
VERSION_WITH_ACTION_FILTER = 0xC6E9
GLOBAL_ACTION = "Global_Script"
GLOBAL_ANIMATION = "Global_is_None_Anim"


class ObsEdgeError(ValueError):
    """The source bytes do not satisfy the preserved Kudiz/OBS layout."""


@dataclass(frozen=True)
class ResourceEntry:
    ordinal: int
    byte_offset: int
    type_id: int
    path: str
    sha256: str


@dataclass(frozen=True)
class FrameGroup:
    ordinal: int
    byte_offset: int
    frame: int
    event_count: int
    events_offset: int
    events_end: int
    events_sha256: str


@dataclass(frozen=True)
class ActionEntry:
    ordinal: int
    entry_offset: int
    declared_end: int | None
    decoded_end: int
    physical_end: int
    action_name_offset: int
    action_name: str
    animation_path_offset: int | None
    animation_path: str
    body_offset: int
    disposition: str
    native_map_admission: str
    has_events_offset: int | None
    has_events_raw: int | None
    resources: tuple[ResourceEntry, ...]
    frame_groups: tuple[FrameGroup, ...]
    event_count: int
    body_probe_error: str | None
    decoded_sha256: str
    physical_sha256: str


@dataclass(frozen=True)
class ObsEdgeReport:
    schema: str
    source_sha256: str
    source_size: int
    file_version: int
    kudiz_offset: int
    material_count: int
    material_table_offset: int
    material_table_end: int
    material_table_sha256: str
    action_count: int
    decoded_action_count: int
    filter_skipped_action_count: int
    total_event_count: int
    actions: tuple[ActionEntry, ...]
    decoded_end: int
    trailing_byte_count: int
    trailing_sha256: str
    trailing_prefix_hex: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), ensure_ascii=False, indent=2, sort_keys=True) + "\n"


class _Reader:
    def __init__(self, data: bytes):
        self.data = data

    def require(self, offset: int, size: int, label: str) -> None:
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ObsEdgeError(
                f"truncated {label}: offset={offset} size={size} source={len(self.data)}"
            )

    def i32(self, offset: int, label: str) -> int:
        self.require(offset, 4, label)
        return struct.unpack_from("<i", self.data, offset)[0]

    def u8(self, offset: int, label: str) -> int:
        self.require(offset, 1, label)
        return self.data[offset]

    def text(self, offset: int, size: int, label: str) -> str:
        self.require(offset, size, label)
        raw = self.data[offset : offset + size]
        try:
            value = raw.decode("utf-16le")
        except UnicodeDecodeError as error:
            raise ObsEdgeError(f"invalid UTF-16LE {label} at {offset}: {error}") from error
        value = value.split("\0", 1)[0]
        while len(value) > 1 and value.endswith(" "):
            value = value[:-1]
        return value


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _bounded_count(reader: _Reader, offset: int, unit_size: int, label: str) -> int:
    value = reader.i32(offset, label)
    if value < 0:
        raise ObsEdgeError(f"negative {label}: {value}")
    remaining = len(reader.data) - offset - 4
    if value > remaining // unit_size:
        raise ObsEdgeError(
            f"{label} exceeds source boundary: count={value} unit={unit_size} "
            f"remaining={remaining}"
        )
    return value


def _parse_action_body(
    reader: _Reader, body_offset: int, action_name: str
) -> tuple[int, int, tuple[ResourceEntry, ...], tuple[FrameGroup, ...], int]:
    has_events = reader.u8(body_offset, f"{action_name} has-events")
    pos = body_offset + 1
    if has_events == 0:
        return pos, has_events, (), (), 0

    resource_count = _bounded_count(
        reader, pos, RESOURCE_ENTRY_SIZE, f"{action_name} resource count"
    )
    pos += 4
    resources: list[ResourceEntry] = []
    for ordinal in range(resource_count):
        entry_offset = pos
        type_id = reader.i32(pos, f"{action_name} resource {ordinal} type")
        path_offset = pos + 4
        path = reader.text(
            path_offset, RESOURCE_TEXT_SIZE, f"{action_name} resource {ordinal} path"
        )
        reader.require(entry_offset, RESOURCE_ENTRY_SIZE, f"{action_name} resource {ordinal}")
        resources.append(
            ResourceEntry(
                ordinal=ordinal,
                byte_offset=entry_offset,
                type_id=type_id,
                path=path,
                sha256=_sha(reader.data[entry_offset : entry_offset + RESOURCE_ENTRY_SIZE]),
            )
        )
        pos += RESOURCE_ENTRY_SIZE

    # A frame-group header is eight bytes. The file boundary is the only
    # format-derived upper bound; an arbitrary low corpus cap would reject
    # otherwise representable private-server data.
    group_count = _bounded_count(reader, pos, 8, f"{action_name} frame-group count")
    pos += 4
    groups: list[FrameGroup] = []
    total_events = 0
    for ordinal in range(group_count):
        group_offset = pos
        frame = reader.i32(pos, f"{action_name} frame group {ordinal} frame")
        event_count = reader.i32(pos + 4, f"{action_name} frame group {ordinal} event count")
        if event_count < 0:
            raise ObsEdgeError(
                f"negative {action_name} frame group {ordinal} event count: {event_count}"
            )
        events_offset = pos + 8
        reader.require(
            events_offset,
            event_count * EVENT_SIZE,
            f"{action_name} frame group {ordinal} events",
        )
        events_end = events_offset + event_count * EVENT_SIZE
        groups.append(
            FrameGroup(
                ordinal=ordinal,
                byte_offset=group_offset,
                frame=frame,
                event_count=event_count,
                events_offset=events_offset,
                events_end=events_end,
                events_sha256=_sha(reader.data[events_offset:events_end]),
            )
        )
        total_events += event_count
        pos = events_end
    return pos, has_events, tuple(resources), tuple(groups), total_events


def parse_obs_edge(data: bytes, kudiz_offset: int) -> ObsEdgeReport:
    """Parse a physical Kudiz common-action table without losing skipped rows.

    Ordinary records are decoded sequentially, matching the native null-filter
    path. If that decode fails and the record's declared end is exactly the byte
    after its animation filename, the record is preserved as a native
    filter-skipped row with an omitted body. No other parse failure is hidden.
    """

    reader = _Reader(data)
    file_version = reader.i32(0, "model version")
    material_count = _bounded_count(
        reader, kudiz_offset, KUDIZ_MATERIAL_ENTRY_SIZE, "Kudiz material count"
    )
    material_table_offset = kudiz_offset + 4
    material_table_end = material_table_offset + material_count * KUDIZ_MATERIAL_ENTRY_SIZE
    action_count = reader.i32(material_table_end, "Kudiz action count")
    if action_count < 0:
        raise ObsEdgeError(f"negative Kudiz action count: {action_count}")

    # The shortest supported physical row is an entry-end word plus a
    # Global_Script name and one has-events byte. Keep the bound structural.
    minimum_action_size = (4 if file_version > VERSION_WITH_ENTRY_END else 0) + ACTION_TEXT_SIZE + 1
    remaining = len(data) - material_table_end - 4
    if action_count > remaining // minimum_action_size:
        raise ObsEdgeError(
            f"Kudiz action count exceeds source boundary: count={action_count} "
            f"remaining={remaining}"
        )

    pos = material_table_end + 4
    actions: list[ActionEntry] = []
    total_events = 0
    filter_skipped = 0
    for ordinal in range(action_count):
        entry_offset = pos
        declared_end: int | None = None
        if file_version > VERSION_WITH_ENTRY_END:
            declared_end = reader.i32(pos, f"action {ordinal} declared end")
            pos += 4

        action_name_offset = pos
        action_name = reader.text(pos, ACTION_TEXT_SIZE, f"action {ordinal} name")
        pos += ACTION_TEXT_SIZE
        is_global = action_name == GLOBAL_ACTION

        animation_path_offset: int | None = None
        animation_path = GLOBAL_ANIMATION
        if not is_global:
            animation_path_offset = pos
            animation_path = reader.text(
                pos, ACTION_TEXT_SIZE, f"action {ordinal} animation path"
            )
            pos += ACTION_TEXT_SIZE
        body_offset = pos

        body_probe_error: str | None = None
        try:
            (
                decoded_end,
                has_events,
                resources,
                groups,
                action_events,
            ) = _parse_action_body(reader, body_offset, action_name)
            disposition = "decoded-global" if is_global else "decoded-ordinary"
            admission = "native-map-entry"
            physical_end = decoded_end
        except ObsEdgeError as error:
            if (
                not is_global
                and file_version >= VERSION_WITH_ACTION_FILTER
                and declared_end == body_offset
            ):
                decoded_end = body_offset
                physical_end = body_offset
                has_events = None
                resources = ()
                groups = ()
                action_events = 0
                disposition = "native-filter-skipped-body-omitted"
                admission = "requires-filter-exclusion"
                body_probe_error = str(error)
                filter_skipped += 1
            else:
                raise ObsEdgeError(
                    f"action {ordinal} {action_name!r} body decode failed: {error}"
                ) from error

        if is_global and declared_end is not None:
            if declared_end < decoded_end or declared_end > len(data):
                raise ObsEdgeError(
                    f"Global_Script declared end outside decoded source: "
                    f"declared={declared_end} decoded={decoded_end} source={len(data)}"
                )
            physical_end = declared_end
            pos = declared_end
        else:
            pos = physical_end

        actions.append(
            ActionEntry(
                ordinal=ordinal,
                entry_offset=entry_offset,
                declared_end=declared_end,
                decoded_end=decoded_end,
                physical_end=physical_end,
                action_name_offset=action_name_offset,
                action_name=action_name,
                animation_path_offset=animation_path_offset,
                animation_path=animation_path,
                body_offset=body_offset,
                disposition=disposition,
                native_map_admission=admission,
                has_events_offset=body_offset if has_events is not None else None,
                has_events_raw=has_events,
                resources=resources,
                frame_groups=groups,
                event_count=action_events,
                body_probe_error=body_probe_error,
                decoded_sha256=_sha(data[entry_offset:decoded_end]),
                physical_sha256=_sha(data[entry_offset:physical_end]),
            )
        )
        total_events += action_events

    trailing = data[pos:]
    return ObsEdgeReport(
        schema="gunion-obs-edge/1",
        source_sha256=_sha(data),
        source_size=len(data),
        file_version=file_version,
        kudiz_offset=kudiz_offset,
        material_count=material_count,
        material_table_offset=material_table_offset,
        material_table_end=material_table_end,
        material_table_sha256=_sha(data[material_table_offset:material_table_end]),
        action_count=action_count,
        decoded_action_count=action_count - filter_skipped,
        filter_skipped_action_count=filter_skipped,
        total_event_count=total_events,
        actions=tuple(actions),
        decoded_end=pos,
        trailing_byte_count=len(trailing),
        trailing_sha256=_sha(trailing),
        trailing_prefix_hex=trailing[:64].hex(),
    )


def parse_obs_edge_file(path: Path, kudiz_offset: int) -> ObsEdgeReport:
    return parse_obs_edge(path.read_bytes(), kudiz_offset)

