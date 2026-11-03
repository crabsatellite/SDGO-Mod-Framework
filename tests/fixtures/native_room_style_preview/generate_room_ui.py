"""Build a strict, preview-only 12-slot overlay for uirs/0373.uir.

The generator reads the sealed client archive without modifying it.  It accepts
only the pinned 8-slot resource, clones complete serialized controls for slots
8..11, rebases every absolute record end, and emits an independent ZDX/ZPK
overlay for the native GOnline resource loader.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib


EXPECTED_INDEX_SHA256 = "6223299c38c3c314ae5bc867db9490a7e8c3b033b16e8c19be98d49e9220cfbb"
EXPECTED_UI_SHA256 = "71ddab556d1bca67dfe71859a0bf1c91c927cea075c742473a50e94b8350d323"
EXPECTED_ENTRY = (7209, 4825022, 15)
EXPECTED_UI_SIZE = 58214
RESOURCE_NAME = "uirs/0373.uir"

FAMILIES = (
    "Slot",
    "Window",
    "RoomZzang",
    "ReadyIcon",
    "balloon",
    "BalloonStatic",
    "PcBangIcon",
    "RandomTeamIcon",
    "UserNameIcon",
    "UserName",
    "ReadyStarNumber",
    "ClassIcon",
    "KingOfClanIcon",
    "BattleShipIcon",
    "OutBtn",
    "UnitChangeIcon",
    "LoadingIcon",
    "ClanBgIcon",
    "ClanFrontIcon",
    "ChargeIcon",
    "ExpireIcon",
    "UserInfoIcon",
)

# Duplicate the authored inner four controls as a second, higher arc.  The
# source and destination Slot anchors have identical X and differ by -60 Y:
#   4 -> 8:  (270,463) -> (270,403)
#   6 -> 10: (392,443) -> (392,383)
#   7 -> 11: (525,443) -> (525,383)
#   5 -> 9:  (650,463) -> (650,403)
# This order retains the inner-arc left-to-right serialization order.
CLONE_PLAN = ((8, 4), (10, 6), (11, 7), (9, 5))
CLONE_DY = -60.0


class ResourceError(RuntimeError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_archive_entry(client: Path) -> tuple[bytes, dict[str, object]]:
    index_path = client / "GPackData.zdx"
    pack_path = client / "GPackData.zpk"
    index = index_path.read_bytes()
    index_hash = sha256(index)
    if index_hash != EXPECTED_INDEX_SHA256:
        raise ResourceError(f"Unsupported GPackData.zdx SHA-256: {index_hash}")
    if len(index) < 24:
        raise ResourceError("GPackData.zdx is truncated")
    version, chunk_size, chunk_count, _, entry_count, _ = struct.unpack_from("<6I", index)
    if version != 1 or chunk_size != 512 or entry_count > 1_000_000:
        raise ResourceError("Unsupported GPackData.zdx header")

    cursor = 24
    found: tuple[int, int, int] | None = None
    for _ in range(entry_count):
        chars: list[str] = []
        while True:
            if cursor + 2 > len(index):
                raise ResourceError("Truncated GPackData.zdx path")
            value = struct.unpack_from("<H", index, cursor)[0] ^ 0xAA
            cursor += 2
            if value == 0:
                break
            chars.append(chr(value))
            if len(chars) > 4096:
                raise ResourceError("Oversized GPackData.zdx path")
        if cursor + 36 > len(index):
            raise ResourceError("Truncated GPackData.zdx metadata")
        stored_size, first_chunk, chunks = struct.unpack_from("<III", index, cursor)
        cursor += 36
        name = "".join(chars).replace("\\", "/").lower()
        if name == RESOURCE_NAME:
            if found is not None:
                raise ResourceError("Duplicate uirs/0373.uir entry")
            found = (stored_size, first_chunk, chunks)
    # This sealed index reserves a fixed all-FF tail after the declared
    # entries.  Refuse any non-padding trailing content.
    if any(value != 0xFF for value in index[cursor:]):
        raise ResourceError("GPackData.zdx has non-padding trailing bytes")
    if found != EXPECTED_ENTRY:
        raise ResourceError(f"Unsupported uirs/0373.uir archive metadata: {found}")

    stored_size, first_chunk, chunks = found
    if first_chunk + chunks > chunk_count or stored_size > chunks * chunk_size:
        raise ResourceError("Invalid uirs/0373.uir chunk range")
    packed = bytearray()
    seen: set[int] = set()
    current = first_chunk
    with pack_path.open("rb") as pack:
        for chain_index in range(chunks):
            if current in seen or current >= chunk_count:
                raise ResourceError("Invalid uirs/0373.uir chunk chain")
            seen.add(current)
            pack.seek(current * (chunk_size + 8))
            block = pack.read(chunk_size + 8)
            if len(block) != chunk_size + 8:
                raise ResourceError("Truncated GPackData.zpk chunk")
            next_chunk, previous_chunk = struct.unpack_from("<II", block)
            if chain_index and previous_chunk != prior:
                raise ResourceError("Broken uirs/0373.uir previous-chunk link")
            prior = current
            current = next_chunk
            packed.extend(block[8:])
    packed = bytes(packed[:stored_size])
    if not packed.startswith(b"ZOAGZIP1") or len(packed) < 12:
        raise ResourceError("uirs/0373.uir is not a ZOAGZIP1 stream")
    expanded_size = struct.unpack_from("<I", packed, 8)[0]
    if expanded_size != EXPECTED_UI_SIZE:
        raise ResourceError(f"Unsupported decoded uirs/0373.uir size: {expanded_size}")
    decoded = zlib.decompress(packed[12:], 31)
    if len(decoded) != expanded_size or sha256(decoded) != EXPECTED_UI_SHA256:
        raise ResourceError("Decoded uirs/0373.uir identity differs")
    return decoded, {
        "index_sha256": index_hash,
        "entry": {
            "stored_size": stored_size,
            "first_chunk": first_chunk,
            "chunks": chunks,
            "expanded_size": expanded_size,
            "expanded_sha256": EXPECTED_UI_SHA256,
        },
    }


def wide_name(record: bytes) -> str:
    raw = record[8:72]
    try:
        return raw.decode("utf-16-le").split("\0", 1)[0]
    except UnicodeDecodeError as error:
        raise ResourceError("Invalid UTF-16 control name") from error


def set_wide_name(record: bytearray, name: str) -> None:
    encoded = (name + "\0").encode("utf-16-le")
    if len(encoded) > 64:
        raise ResourceError(f"Control name exceeds wchar[32]: {name}")
    record[8:72] = encoded.ljust(64, b"\0")


def parse_records(decoded: bytes) -> tuple[bytes, list[bytes]]:
    if len(decoded) != EXPECTED_UI_SIZE or struct.unpack_from("<H", decoded)[0] != 0xEA62:
        raise ResourceError("Unsupported 0373.uir file header")
    root_end = struct.unpack_from("<I", decoded, 4)[0]
    if root_end != 131 or struct.unpack_from("<I", decoded, 8)[0] != 0xEA66:
        raise ResourceError("Unsupported 0373.uir root record")
    root_name = decoded[12:76].decode("utf-16-le").split("\0", 1)[0]
    if root_name != "Dlg" or root_end + 4 > len(decoded):
        raise ResourceError("Unsupported 0373.uir root identity")
    count = struct.unpack_from("<I", decoded, root_end)[0]
    if count != 213:
        raise ResourceError(f"Unsupported 0373.uir child count: {count}")

    prefix = decoded[: root_end + 4]
    records: list[bytes] = []
    cursor = root_end + 4
    for _ in range(count):
        if cursor + 100 > len(decoded):
            raise ResourceError("Truncated 0373.uir child record")
        end = struct.unpack_from("<I", decoded, cursor)[0]
        if end <= cursor or end > len(decoded):
            raise ResourceError("Invalid 0373.uir absolute child end")
        record = decoded[cursor:end]
        if struct.unpack_from("<I", record, 4)[0] != 0xEA66:
            raise ResourceError(f"Unsupported child base version: {wide_name(record)}")
        wide_name(record)
        records.append(record)
        cursor = end
    if cursor != len(decoded):
        raise ResourceError("0373.uir child records do not consume the file")
    return prefix, records


def validate_source_controls(records: list[bytes]) -> dict[str, int]:
    positions: dict[str, int] = {}
    for index, record in enumerate(records):
        name = wide_name(record)
        if not name or name in positions:
            raise ResourceError(f"Missing or duplicate control name: {name!r}")
        positions[name] = index
    for family in FAMILIES:
        actual = {name for name in positions if name.startswith(family) and name[len(family):].isdigit()}
        expected = {f"{family}{slot}" for slot in range(8)}
        if actual != expected:
            raise ResourceError(f"Unsupported {family} controls")
    unit_icons = {
        name for name in positions
        if name.startswith("BattleShipUnitTypeIcon")
        and name[len("BattleShipUnitTypeIcon"):].isdigit()
    }
    if unit_icons != {f"BattleShipUnitTypeIcon{index}" for index in range(32)}:
        raise ResourceError("Unsupported BattleShipUnitTypeIcon controls")
    if {f"Multi3DRender{index}" for index in range(1, 5)} - positions.keys():
        raise ResourceError("Missing Multi3DRender controls")
    if "chattingIcon" not in positions:
        raise ResourceError("Missing chattingIcon")
    return positions


def clone_control(source: bytes, name: str) -> bytes:
    clone = bytearray(source)
    set_wide_name(clone, name)
    x, y = struct.unpack_from("<ff", clone, 76)
    struct.pack_into("<ff", clone, 76, x, y + CLONE_DY)
    return bytes(clone)


def extend_multi3d_render4(source: bytes) -> bytes:
    if wide_name(source) != "Multi3DRender4":
        raise ResourceError("Expected Multi3DRender4")
    entry_offset = 0xA8
    entry_size = 48
    before = struct.unpack_from("<I", source, 0xA4)[0]
    if before != 16 or len(source) != entry_offset + before * entry_size:
        raise ResourceError("Unsupported Multi3DRender4 serialization")
    entries = [
        source[entry_offset + index * entry_size:entry_offset + (index + 1) * entry_size]
        for index in range(before)
    ]
    if any(entry != entries[0] for entry in entries[1:]):
        raise ResourceError("Multi3DRender4 source entries differ")
    output = bytearray(source)
    struct.pack_into("<I", output, 0xA4, 24)
    output.extend(entries[0] * 8)
    return bytes(output)


def generate_ui(decoded: bytes) -> tuple[bytes, dict[str, object]]:
    prefix, records = parse_records(decoded)
    positions = validate_source_controls(records)
    family_at_last: dict[int, str] = {}
    for family in FAMILIES:
        family_at_last[max(positions[f"{family}{slot}"] for slot in range(8))] = family
    unit_last = max(positions[f"BattleShipUnitTypeIcon{index}"] for index in range(32))

    generated_records: list[bytes] = []
    clone_sources: dict[str, str] = {}
    for index, record in enumerate(records):
        if wide_name(record) == "Multi3DRender4":
            record = extend_multi3d_render4(record)
        generated_records.append(record)
        family = family_at_last.get(index)
        if family is not None:
            for target_slot, source_slot in CLONE_PLAN:
                target = f"{family}{target_slot}"
                source = f"{family}{source_slot}"
                generated_records.append(clone_control(records[positions[source]], target))
                clone_sources[target] = source
        if index == unit_last:
            for target_slot, source_slot in CLONE_PLAN:
                for subslot in range(4):
                    target_index = target_slot * 4 + subslot
                    source_index = source_slot * 4 + subslot
                    target = f"BattleShipUnitTypeIcon{target_index}"
                    source = f"BattleShipUnitTypeIcon{source_index}"
                    generated_records.append(clone_control(records[positions[source]], target))
                    clone_sources[target] = source

    expected_count = len(FAMILIES) * 12 + 48 + 4 + 1
    if len(generated_records) != expected_count or len(clone_sources) != 104:
        raise ResourceError("Generated control count differs")

    output = bytearray(prefix)
    struct.pack_into("<I", output, 131, expected_count)
    generated_offsets: dict[str, int] = {}
    for record in generated_records:
        start = len(output)
        copy = bytearray(record)
        struct.pack_into("<I", copy, 0, start + len(copy))
        name = wide_name(copy)
        if name in generated_offsets:
            raise ResourceError(f"Generated duplicate control name: {name}")
        generated_offsets[name] = start
        output.extend(copy)

    generated = bytes(output)
    _, verified_records = parse_generated(generated, expected_count)
    verified_names = {wide_name(record) for record in verified_records}
    expected_names = {
        *(f"{family}{slot}" for family in FAMILIES for slot in range(12)),
        *(f"BattleShipUnitTypeIcon{index}" for index in range(48)),
        *(f"Multi3DRender{index}" for index in range(1, 5)),
        "chattingIcon",
    }
    if verified_names != expected_names:
        raise ResourceError("Generated control-name set differs")

    anchors = {}
    for slot in range(12):
        record = verified_records[next(
            index for index, item in enumerate(verified_records)
            if wide_name(item) == f"Slot{slot}"
        )]
        anchors[str(slot)] = list(struct.unpack_from("<ff", record, 76))
    return generated, {
        "child_count": expected_count,
        "new_controls": len(clone_sources),
        "clone_sources": clone_sources,
        "slot_anchors": anchors,
        "resource_fixes": {
            "Multi3DRender4": {
                "before": 16,
                "after": 24,
                "entry_size": 48,
                "serialized_count_offset": 0xA4,
                "added_entries": 8,
                "clone_source": 0,
            },
        },
        "generated_offsets": {
            name: generated_offsets[name]
            for name in sorted(clone_sources)
        },
    }


def parse_generated(data: bytes, expected_count: int) -> tuple[bytes, list[bytes]]:
    root_end = struct.unpack_from("<I", data, 4)[0]
    if root_end != 131 or struct.unpack_from("<I", data, root_end)[0] != expected_count:
        raise ResourceError("Generated root child count differs")
    prefix = data[: root_end + 4]
    records: list[bytes] = []
    cursor = root_end + 4
    for _ in range(expected_count):
        if cursor + 4 > len(data):
            raise ResourceError("Generated child record is truncated")
        end = struct.unpack_from("<I", data, cursor)[0]
        if end <= cursor or end > len(data):
            raise ResourceError("Generated absolute child end differs")
        records.append(data[cursor:end])
        cursor = end
    if cursor != len(data):
        raise ResourceError("Generated records do not consume the file")
    return prefix, records


def build_archive(resources: dict[str, bytes], output: Path) -> dict[str, object]:
    chunk_size = 512
    entries: list[dict[str, object]] = []
    chunks: list[bytes] = []
    for name, raw in sorted(resources.items()):
        packed = b"ZOAGZIP1" + struct.pack("<I", len(raw)) + gzip.compress(raw, mtime=0)
        start = len(chunks)
        count = (len(packed) + chunk_size - 1) // chunk_size
        for offset in range(count):
            cursor = len(chunks)
            chunks.append(
                struct.pack("<II", cursor + 1, max(0, cursor - 1))
                + packed[offset * chunk_size:(offset + 1) * chunk_size].ljust(chunk_size, b"\0")
            )
        entries.append({
            "path": name.replace("/", "\\").lower(),
            "stored_size": len(packed),
            "start_chunk": start,
            "chunks": count,
            "expanded_size": len(raw),
            "expanded_sha256": sha256(raw),
        })
    zdx = bytearray(struct.pack("<6I", 1, chunk_size, len(chunks), 0, len(entries), 0))
    for entry in entries:
        for character in str(entry["path"]) + "\0":
            zdx.extend(struct.pack("<H", ord(character) ^ 0xAA))
        zdx.extend(struct.pack(
            "<III",
            int(entry["stored_size"]),
            int(entry["start_chunk"]),
            int(entry["chunks"]),
        ) + bytes(24))
    output.with_suffix(".zdx").write_bytes(zdx)
    output.with_suffix(".zpk").write_bytes(b"".join(chunks))
    return {"chunk_size": chunk_size, "entries": entries}


def write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("generated-12slot"))
    args = parser.parse_args()

    source_root = Path(__file__).resolve().parent
    output = args.output
    if not output.is_absolute():
        output = source_root / output
    output = output.resolve()
    if output.parent != source_root:
        raise SystemExit("Output must be a direct child of the source fixture directory")
    if output.exists():
        raise SystemExit(f"Output already exists: {output}")

    temporary = source_root / (output.name + ".pending")
    if temporary.exists():
        raise SystemExit(f"Pending output already exists: {temporary}")
    temporary.mkdir()
    try:
        decoded, source = read_archive_entry(args.client.resolve())
        generated, generation = generate_ui(decoded)
        ui_dir = temporary / "ui"
        ui_dir.mkdir()
        ui_path = ui_dir / "0373.uir"
        ui_path.write_bytes(generated)
        archive = build_archive({RESOURCE_NAME: generated}, temporary / "overlay")

        files = {
            "ui/0373.uir": sha256(generated),
            "overlay.zdx": sha256((temporary / "overlay.zdx").read_bytes()),
            "overlay.zpk": sha256((temporary / "overlay.zpk").read_bytes()),
        }
        runtime = (
            "[overlay]\n"
            "schema=1\n"
            "overlay=overlay\n"
            f"overlay_zdx_sha256={files['overlay.zdx']}\n"
            f"overlay_zpk_sha256={files['overlay.zpk']}\n"
            "ui=ui\\0373.uir\n"
            f"ui_sha256={files['ui/0373.uir']}\n"
            f"source_zdx_sha256={source['index_sha256']}\n"
            f"source_ui_sha256={EXPECTED_UI_SHA256}\n"
        )
        (temporary / "runtime.ini").write_text(runtime, encoding="ascii", newline="\n")
        write_json(temporary / "overlay-index.json", archive)
        write_json(temporary / "manifest.json", {
            "schema_version": 1,
            "purpose": "prototype-native-0373-12-slot-overlay",
            "resource": RESOURCE_NAME,
            "source": source,
            "generation": generation,
            "files": files,
        })
        temporary.replace(output)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    print(json.dumps({
        "output": str(output),
        "generated_ui_sha256": files["ui/0373.uir"],
        "child_count": generation["child_count"],
        "new_controls": generation["new_controls"],
        "overlay_zdx_sha256": files["overlay.zdx"],
        "overlay_zpk_sha256": files["overlay.zpk"],
    }, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    main()
