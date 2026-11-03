"""Build a strict cooperative Mission overlay for uirs/0586.uir.

The shipped resource contains four complete player-control families, while the
matching executable unconditionally binds six.  This generator retains every
authored record and clones complete inner-slot records for slots 4 and 5,
rebasing every absolute record end before writing a one-resource ZDX/ZPK.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib

from generate_room_ui import (
    ResourceError,
    build_archive,
    parse_generated,
    set_wide_name,
    sha256,
    wide_name,
    write_json,
)


EXPECTED_INDEX_SHA256 = "6223299c38c3c314ae5bc867db9490a7e8c3b033b16e8c19be98d49e9220cfbb"
EXPECTED_UI_SHA256 = "491eef91440f06b2bc97be52380d160f49eb7af753b0c5029ec77b9a1d2f469c"
EXPECTED_ENTRY = (3625, 4825524, 8)
EXPECTED_UI_SIZE = 25009
RESOURCE_NAME = "uirs/0586.uir"
FAMILIES = (
    "ClassIcon",
    "KingOfClanIcon",
    "BalloonStatic",
    "balloon",
    "PcBangIcon",
    "OutBtn",
    "UserNameIcon",
    "UserName",
    "UnitChangeIcon",
    "ReadyStarNumber",
    "ExpireIcon",
    "ChargeIcon",
    "UserInfoIcon",
    "LoadingIcon",
    "ClanBgIcon",
    "ClanFrontIcon",
    "Window",
    "ReadyIcon",
    "RoomZzang",
    "LampIcon",
    "catapultIcon",
)
CLONE_DY = -100.0


def read_archive_entry(client: Path) -> tuple[bytes, dict[str, object]]:
    index_path = client / "GPackData.zdx"
    pack_path = client / "GPackData.zpk"
    index = index_path.read_bytes()
    index_hash = sha256(index)
    if index_hash != EXPECTED_INDEX_SHA256:
        raise ResourceError(f"Unsupported GPackData.zdx SHA-256: {index_hash}")
    version, chunk_size, chunk_count, _, entry_count, _ = struct.unpack_from(
        "<6I", index
    )
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
        if cursor + 36 > len(index):
            raise ResourceError("Truncated GPackData.zdx metadata")
        stored_size, first_chunk, chunks = struct.unpack_from("<III", index, cursor)
        cursor += 36
        name = "".join(chars).replace("\\", "/").lower()
        if name == RESOURCE_NAME:
            if found is not None:
                raise ResourceError("Duplicate uirs/0586.uir entry")
            found = (stored_size, first_chunk, chunks)
    if any(value != 0xFF for value in index[cursor:]):
        raise ResourceError("GPackData.zdx has non-padding trailing bytes")
    if found != EXPECTED_ENTRY:
        raise ResourceError(f"Unsupported uirs/0586.uir archive metadata: {found}")

    stored_size, current, chunks = found
    if current + chunks > chunk_count or stored_size > chunks * chunk_size:
        raise ResourceError("Invalid uirs/0586.uir chunk range")
    packed = bytearray()
    seen: set[int] = set()
    with pack_path.open("rb") as pack:
        prior = 0
        for chain_index in range(chunks):
            if current in seen or current >= chunk_count:
                raise ResourceError("Invalid uirs/0586.uir chunk chain")
            seen.add(current)
            pack.seek(current * (chunk_size + 8))
            block = pack.read(chunk_size + 8)
            if len(block) != chunk_size + 8:
                raise ResourceError("Truncated GPackData.zpk chunk")
            next_chunk, previous_chunk = struct.unpack_from("<II", block)
            if chain_index and previous_chunk != prior:
                raise ResourceError("Broken uirs/0586.uir previous-chunk link")
            prior = current
            current = next_chunk
            packed.extend(block[8:])
    packed = bytes(packed[:stored_size])
    if not packed.startswith(b"ZOAGZIP1") or len(packed) < 12:
        raise ResourceError("uirs/0586.uir is not a ZOAGZIP1 stream")
    expanded_size = struct.unpack_from("<I", packed, 8)[0]
    decoded = zlib.decompress(packed[12:], 31)
    if (
        expanded_size != EXPECTED_UI_SIZE
        or len(decoded) != expanded_size
        or sha256(decoded) != EXPECTED_UI_SHA256
    ):
        raise ResourceError("Decoded uirs/0586.uir identity differs")
    return decoded, {
        "index_sha256": index_hash,
        "entry": {
            "stored_size": stored_size,
            "first_chunk": found[1],
            "chunks": chunks,
            "expanded_size": expanded_size,
            "expanded_sha256": EXPECTED_UI_SHA256,
        },
    }


def parse_records(decoded: bytes) -> tuple[bytes, list[bytes]]:
    if len(decoded) != EXPECTED_UI_SIZE or struct.unpack_from("<H", decoded)[0] != 0xEA62:
        raise ResourceError("Unsupported 0586.uir file header")
    root_end = struct.unpack_from("<I", decoded, 4)[0]
    if root_end != 131 or struct.unpack_from("<I", decoded, 8)[0] != 0xEA66:
        raise ResourceError("Unsupported 0586.uir root record")
    if decoded[12:76].decode("utf-16-le").split("\0", 1)[0] != "Dlg":
        raise ResourceError("Unsupported 0586.uir root identity")
    count = struct.unpack_from("<I", decoded, root_end)[0]
    if count != 88:
        raise ResourceError(f"Unsupported 0586.uir child count: {count}")
    prefix = decoded[: root_end + 4]
    records: list[bytes] = []
    cursor = root_end + 4
    for _ in range(count):
        if cursor + 100 > len(decoded):
            raise ResourceError("Truncated 0586.uir child record")
        end = struct.unpack_from("<I", decoded, cursor)[0]
        if end <= cursor or end > len(decoded):
            raise ResourceError("Invalid 0586.uir absolute child end")
        record = decoded[cursor:end]
        if struct.unpack_from("<I", record, 4)[0] != 0xEA66:
            raise ResourceError(f"Unsupported child base version: {wide_name(record)}")
        wide_name(record)
        records.append(record)
        cursor = end
    if cursor != len(decoded):
        raise ResourceError("0586.uir child records do not consume the file")
    return prefix, records


def clone_control(source: bytes, name: str) -> bytes:
    clone = bytearray(source)
    set_wide_name(clone, name)
    x, y = struct.unpack_from("<ff", clone, 76)
    struct.pack_into("<ff", clone, 76, x, y + CLONE_DY)
    return bytes(clone)


def repair_mission_render(record: bytes) -> bytes:
    if wide_name(record) != "Multi3DRender77":
        return record
    if (
        len(record) != 360
        or struct.unpack_from("<I", record, 0xA4)[0] != 4
    ):
        raise ResourceError("Unsupported Multi3DRender77 serialization")
    entry_start = 0xA8
    entry_size = 48
    repaired = bytearray(record)
    struct.pack_into("<I", repaired, 0xA4, 6)
    repaired.extend(record[entry_start + entry_size:entry_start + entry_size * 2])
    repaired.extend(record[entry_start + entry_size * 2:entry_start + entry_size * 3])
    return bytes(repaired)


def generate_ui(decoded: bytes) -> tuple[bytes, dict[str, object]]:
    prefix, records = parse_records(decoded)
    positions: dict[str, int] = {}
    for index, record in enumerate(records):
        name = wide_name(record)
        if not name or name in positions:
            raise ResourceError(f"Missing or duplicate control name: {name!r}")
        positions[name] = index
    for family in FAMILIES:
        present = {
            name
            for name in positions
            if name.startswith(family) and name[len(family):].isdigit()
        }
        expected = {f"{family}{slot}" for slot in range(5 if family == "LampIcon" else 4)}
        if present != expected:
            raise ResourceError(f"Unsupported {family} controls: {sorted(present)}")

    clones_after: dict[int, list[tuple[str, str]]] = {}
    for family in FAMILIES:
        targets = ((5, 2),) if family == "LampIcon" else ((4, 1), (5, 2))
        last = max(positions[f"{family}{slot}"] for slot in range(5 if family == "LampIcon" else 4))
        clones_after[last] = [
            (f"{family}{target}", f"{family}{source}")
            for target, source in targets
        ]

    output_records: list[bytes] = []
    clone_sources: dict[str, str] = {}
    for index, record in enumerate(records):
        output_records.append(repair_mission_render(record))
        for target, source in clones_after.get(index, []):
            output_records.append(clone_control(records[positions[source]], target))
            clone_sources[target] = source

    expected_new = len(FAMILIES) * 2 - 1
    expected_count = 88 + expected_new
    if len(output_records) != expected_count or len(clone_sources) != expected_new:
        raise ResourceError("Generated Mission control count differs")
    output = bytearray(prefix)
    struct.pack_into("<I", output, 131, expected_count)
    for record in output_records:
        copy = bytearray(record)
        struct.pack_into("<I", copy, 0, len(output) + len(copy))
        output.extend(copy)
    generated = bytes(output)
    _, verified = parse_generated(generated, expected_count)
    names = [wide_name(record) for record in verified]
    if len(names) != len(set(names)):
        raise ResourceError("Generated Mission controls contain duplicate names")
    for family in FAMILIES:
        expected = {f"{family}{slot}" for slot in range(6)}
        actual = {
            name
            for name in names
            if name.startswith(family) and name[len(family):].isdigit()
        }
        if actual != expected:
            raise ResourceError(f"Generated {family} family differs")
    anchors = {}
    for family in ("ClassIcon", "Window", "catapultIcon"):
        anchors[family] = {
            str(slot): list(struct.unpack_from(
                "<ff",
                verified[names.index(f"{family}{slot}")],
                76,
            ))
            for slot in range(6)
        }
    return generated, {
        "child_count": expected_count,
        "new_controls": len(clone_sources),
        "clone_sources": clone_sources,
        "resource_fixes": {
            "Multi3DRender77": {
                "serialized_count_offset": 0xA4,
                "before": 4,
                "after": 6,
                "entry_size": 48,
                "clone_sources": {"4": 1, "5": 2},
            }
        },
        "anchors": anchors,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("generated-mission"))
    args = parser.parse_args()
    source_root = Path(__file__).resolve().parent
    output = args.output if args.output.is_absolute() else source_root / args.output
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
        (ui_dir / "0586.uir").write_bytes(generated)
        archive = build_archive({RESOURCE_NAME: generated}, temporary / "overlay")
        files = {
            "ui/0586.uir": sha256(generated),
            "overlay.zdx": sha256((temporary / "overlay.zdx").read_bytes()),
            "overlay.zpk": sha256((temporary / "overlay.zpk").read_bytes()),
        }
        runtime = (
            "[overlay]\n"
            "schema=1\n"
            "overlay=overlay\n"
            "ui=ui\\0586.uir\n"
            f"ui_sha256={files['ui/0586.uir']}\n"
            f"overlay_zdx_sha256={files['overlay.zdx']}\n"
            f"overlay_zpk_sha256={files['overlay.zpk']}\n"
            f"source_zdx_sha256={source['index_sha256']}\n"
            f"source_ui_sha256={EXPECTED_UI_SHA256}\n"
        )
        (temporary / "runtime.ini").write_text(runtime, encoding="ascii", newline="\n")
        write_json(temporary / "overlay-index.json", archive)
        write_json(temporary / "manifest.json", {
            "schema_version": 1,
            "purpose": "prototype-native-0586-six-slot-overlay",
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
        "generated_ui_sha256": files["ui/0586.uir"],
        "child_count": generation["child_count"],
        "new_controls": generation["new_controls"],
        "overlay_zdx_sha256": files["overlay.zdx"],
        "overlay_zpk_sha256": files["overlay.zpk"],
    }, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    main()
