#!/usr/bin/env python3
"""
SDGO .mod file parser - Extracts animation events from ZOA OBS model files.

Parses the Kudiz OBS section (materials, animation entries, frame events)
from .mod files used by SD Gundam Capsule Fighter Online's ZOA engine.

Format reference: RE-Analysis-Report/MOD-OBS-FORMAT.md

Usage:
    python -X utf8 mod_parser.py <file.mod> [--json] [--all-events] [--summary]
    python -X utf8 mod_parser.py <directory> [--json] [--all-events] [--summary]
"""

import struct
import json
import sys
import os
import argparse
from pathlib import Path
from typing import Optional

# ─── Constants ───────────────────────────────────────────────────────────────

MOD_VERSION = 0xC6E9
ZOA_MAGIC = b'ZOA\x00'

# AnimEvent struct size
ANIM_EVENT_SIZE = 0xAC  # 172 bytes

# Material entry size: 4 texture slots (0x40 each) + properties (0x44)
MATERIAL_SIZE = 4 * 0x40 + 0x44  # = 0x144 = 324 bytes

# Resource entry size: type_id (4) + path wchar[32] (0x40)
RESOURCE_SIZE = 0x44  # 68 bytes

# Event type enum
EVENT_TYPES = {
    0x01: "MATERIAL",
    0x02: "ATTACH_EFFECT",
    0x03: "DETACH_EFFECT",
    0x06: "ATTACH_EFFECT_FORCED",
    0x07: "DETACH_EFFECT_ALT",
    0x08: "ROTATION",
    0x09: "POSITION",
    0x0A: "SCALE",
    0x0B: "SOUND",
    0x0C: "DELAY",
    0x0D: "TEXTURE",
    0x0E: "UNKNOWN_0E",
    0x0F: "FORWARD",
    0x10: "RIGHT",
    0x11: "PLAYRATE",
    0x12: "STOPDELAY",
    0x13: "RESET",
    0x14: "HIDE",
    0x15: "SHOW",
    0x16: "VOLUME",
    0x17: "VOLUME_DIST",
    0x18: "FADE",
    0x1B: "UP",
}

# Resource type IDs
RESOURCE_TYPES = {
    0x00020003: "CEffect",
    0x00030000: "Sound3D",
    0x00010000: "SubModel",
}


# ─── Binary reading helpers ──────────────────────────────────────────────────

def read_int32(data: bytes, offset: int) -> int:
    return struct.unpack_from('<i', data, offset)[0]


def read_uint32(data: bytes, offset: int) -> int:
    return struct.unpack_from('<I', data, offset)[0]


def read_float(data: bytes, offset: int) -> float:
    return struct.unpack_from('<f', data, offset)[0]


def read_wchar(data: bytes, offset: int, byte_len: int) -> str:
    """Read a null-terminated wchar16-LE string from a fixed-size buffer."""
    raw = data[offset:offset + byte_len]
    try:
        text = raw.decode('utf-16-le')
    except UnicodeDecodeError:
        text = raw.decode('utf-16-le', errors='replace')
    return text.split('\x00')[0]


# ─── Kudiz section finder ───────────────────────────────────────────────────

def find_kudiz_offset(data: bytes) -> Optional[int]:
    """
    Find the byte offset where the Kudiz OBS section begins.

    The CModel geometry section (variable size) precedes the Kudiz section.
    Since we cannot parse CModel::LoadModel without its decompilation,
    we use pattern matching to locate the Kudiz section.

    Strategy:
    1. Fast path: search for 'Global_Script' (wchar16-LE) which appears as
       the first animation entry name in most unit .mod files, then backtrack
       to derive the Kudiz start from the material count.
    2. Fallback: scan for valid material_count + animation structure patterns.
    """
    fsize = len(data)

    # ── Fast path: find Global_Script and backtrack ──
    gs_pattern = 'Global_Script'.encode('utf-16-le')
    gs_pos = data.find(gs_pattern)

    if gs_pos > 8:
        # Global_Script is an action_name field (0x80 bytes wchar[64])
        # Layout before it:
        #   [anim_count: int32] [entry_end_offset: int32] [action_name: 0x80 bytes]
        # So anim_count is at gs_pos - 8, entry_end_offset at gs_pos - 4
        ac = read_int32(data, gs_pos - 8)
        eeo = read_int32(data, gs_pos - 4)

        if 0 < ac <= 300 and 0 < eeo < fsize:
            # Try to find material_count by searching backwards.
            # Search from highest to lowest mc so we find the true Kudiz start
            # (mc=0 would find a false positive at the anim_count position).
            anim_count_offset = gs_pos - 8
            for mc in range(50, -1, -1):
                kudiz_start = anim_count_offset - mc * MATERIAL_SIZE - 4
                if kudiz_start < 4:
                    continue
                mc_read = read_int32(data, kudiz_start)
                if mc_read == mc:
                    if _validate_kudiz(data, kudiz_start):
                        return kudiz_start

    # ── Fallback: scan for valid structure ──
    return _scan_for_kudiz(data)


def _validate_kudiz(data: bytes, offset: int) -> bool:
    """Validate a candidate Kudiz section offset."""
    fsize = len(data)
    if offset + 4 > fsize:
        return False

    mc = read_int32(data, offset)
    if not (0 <= mc <= 50):
        return False

    anim_off = offset + 4 + mc * MATERIAL_SIZE
    if anim_off + 4 > fsize:
        return False

    ac = read_int32(data, anim_off)
    if not (0 <= ac <= 300):
        return False

    if ac > 0:
        # Check first entry_end_offset and action name
        if anim_off + 8 + 0x80 > fsize:
            return False
        eeo = read_int32(data, anim_off + 4)
        if not (anim_off + 4 < eeo <= fsize):
            return False
        action = read_wchar(data, anim_off + 8, 0x80).strip()
        if not action or not all(c.isascii() and c.isprintable() for c in action):
            return False

    return True


def _scan_for_kudiz(data: bytes) -> Optional[int]:
    """
    Brute-force scan for the Kudiz section by looking for valid
    material_count + animation entry patterns.
    """
    fsize = len(data)
    best_offset = None
    best_score = -1

    # Search through possible offsets (CModel minimum = 5 bytes at offset 4)
    for off in range(9, fsize - 20):
        mc = read_int32(data, off)
        if not (0 <= mc <= 50):
            continue

        anim_off = off + 4 + mc * MATERIAL_SIZE
        if anim_off + 4 > fsize:
            continue

        ac = read_int32(data, anim_off)
        if not (0 <= ac <= 300):
            continue

        score = 0

        # Validate material texture names
        for m in range(mc):
            for slot in range(4):
                soff = off + 4 + m * MATERIAL_SIZE + slot * 0x40
                if soff + 0x40 > fsize:
                    break
                tex = read_wchar(data, soff, 0x40)
                if tex and ('Txrs' in tex or 'txrs' in tex or tex == '-1'):
                    score += 5

        # Validate animation entries
        if ac > 0:
            cur = anim_off + 4
            valid_entries = 0
            for i in range(min(ac, 5)):
                if cur + 4 > fsize:
                    break
                eeo = read_int32(data, cur)
                if not (cur < eeo <= fsize):
                    break
                name = read_wchar(data, cur + 4, 0x80).strip()
                if not (name and all(c.isascii() and c.isprintable() for c in name)):
                    break
                if name == 'Global_Script':
                    score += 100
                valid_entries += 1
                cur = eeo
            score += valid_entries * 20
        else:
            # Check material properties for plausible float values
            for m in range(mc):
                props_off = off + 4 + m * MATERIAL_SIZE + 4 * 0x40
                if props_off + 0x44 > fsize:
                    break
                ambient = struct.unpack_from('<4f', data, props_off)
                if all(0.0 <= v <= 1.0 for v in ambient) and any(v > 0.0 for v in ambient):
                    score += 3

        if score > best_score:
            best_score = score
            best_offset = off

    return best_offset


# ─── Parsers ─────────────────────────────────────────────────────────────────

def parse_materials(data: bytes, offset: int, count: int) -> list:
    """Parse the material override table."""
    materials = []
    off = offset

    for m in range(count):
        mat = {"index": m, "textures": [], "properties": {}}

        # 4 texture slots, each wchar[32] = 0x40 bytes
        for slot in range(4):
            tex = read_wchar(data, off, 0x40)
            if tex and tex != '-1':
                mat["textures"].append({"slot": slot, "path": tex})
            off += 0x40

        # Material properties: D3DMATERIAL9-like (17 floats = 0x44 bytes)
        props = struct.unpack_from('<17f', data, off)
        mat["properties"] = {
            "ambient": list(props[0:4]),
            "diffuse": list(props[4:8]),
            "specular": list(props[8:12]),
            "emissive": list(props[12:16]),
            "power": props[16],
        }
        off += 0x44

        materials.append(mat)

    return materials, off


def parse_resource_table(data: bytes, offset: int) -> tuple:
    """Parse CFrameEventSys resource table."""
    off = offset
    res_count = read_int32(data, off)
    off += 4

    resources = []
    for i in range(res_count):
        type_id = read_uint32(data, off)
        off += 4
        path = read_wchar(data, off, 0x40)
        off += 0x40

        type_name = RESOURCE_TYPES.get(type_id, f"UNKNOWN_0x{type_id:08X}")
        resources.append({
            "index": i,
            "type_id": type_id,
            "type_name": type_name,
            "path": path,
        })

    return resources, off


def parse_anim_event(data: bytes, offset: int) -> dict:
    """Parse a single AnimEvent struct (0xAC = 172 bytes)."""
    off = offset

    res_index = read_int32(data, off)
    event_type_id = read_int32(data, off + 0x04)
    param_x = read_float(data, off + 0x08)
    param_y = read_float(data, off + 0x0C)
    param_z = read_float(data, off + 0x10)
    param_w = read_float(data, off + 0x14)

    # Extra fields (can be float or uint depending on event type)
    extra = []
    for i in range(4):
        extra.append(read_float(data, off + 0x18 + i * 4))

    unknown_28 = read_uint32(data, off + 0x28)
    bone_name = read_wchar(data, off + 0x2C, 0x80)

    event_type_name = EVENT_TYPES.get(event_type_id, f"UNKNOWN_0x{event_type_id:02X}")

    event = {
        "type": event_type_name,
        "type_id": event_type_id,
        "resource_index": res_index,
        "float_params": [
            round(param_x, 6),
            round(param_y, 6),
            round(param_z, 6),
            round(param_w, 6),
        ],
        "extra": [round(e, 6) for e in extra],
        "bone_name": bone_name,
    }

    if unknown_28 != 0:
        event["unknown_28"] = unknown_28

    return event


def parse_frame_events(data: bytes, offset: int) -> tuple:
    """Parse CFrameEventSys frame event map."""
    off = offset

    # Resource table
    resources, off = parse_resource_table(data, off)

    # Frame groups
    frame_group_count = read_int32(data, off)
    off += 4

    events_by_frame = []
    total_event_count = 0

    for fg in range(frame_group_count):
        frame_number = read_int32(data, off)
        off += 4
        event_count = read_int32(data, off)
        off += 4

        frame_events = []
        for e in range(event_count):
            event = parse_anim_event(data, off)
            event["frame"] = frame_number
            frame_events.append(event)
            off += ANIM_EVENT_SIZE
            total_event_count += 1

        events_by_frame.append({
            "frame": frame_number,
            "events": frame_events,
        })

    return resources, events_by_frame, total_event_count, off


def parse_animations(data: bytes, offset: int, count: int, file_version: int) -> tuple:
    """Parse the animation entry table."""
    animations = []
    off = offset

    for i in range(count):
        anim = {}

        # Entry end offset (for version > 0xC6E8)
        entry_end = None
        if file_version > 0xC6E8:
            entry_end = read_int32(data, off)
            off += 4

        # Action name: wchar[64] = 0x80 bytes
        action_name = read_wchar(data, off, 0x80).strip()
        anim["name"] = action_name
        off += 0x80

        if action_name == "Global_Script":
            anim["ani_file"] = "Global_is_None_Anim"

            # has_events flag (1 byte)
            has_events = data[off]
            off += 1

            if has_events:
                resources, frame_events, event_count, off = parse_frame_events(data, off)
                anim["resources"] = resources
                anim["frame_groups"] = frame_events
                # Flatten events for easy access
                anim["events"] = _flatten_events(frame_events, resources)
                anim["event_count"] = event_count
            else:
                anim["events"] = []
                anim["event_count"] = 0

            # Seek to entry end for alignment
            if entry_end is not None and entry_end > off:
                off = entry_end
        else:
            # Normal animation entry
            # Animation filename: wchar[64] = 0x80 bytes
            ani_file = read_wchar(data, off, 0x80).strip()
            anim["ani_file"] = ani_file
            off += 0x80

            # has_events flag (1 byte)
            has_events = data[off]
            off += 1

            if has_events:
                resources, frame_events, event_count, off = parse_frame_events(data, off)
                anim["resources"] = resources
                anim["frame_groups"] = frame_events
                anim["events"] = _flatten_events(frame_events, resources)
                anim["event_count"] = event_count
            else:
                anim["events"] = []
                anim["event_count"] = 0

            # Seek to entry end for alignment
            if entry_end is not None and entry_end > off:
                off = entry_end

        animations.append(anim)

    return animations, off


def _flatten_events(frame_groups: list, resources: list) -> list:
    """Flatten frame_groups into a single events list with resource info."""
    events = []
    for fg in frame_groups:
        for ev in fg["events"]:
            flat = {
                "frame": ev["frame"],
                "type": ev["type"],
                "type_id": ev["type_id"],
                "resource_index": ev["resource_index"],
                "float_params": ev["float_params"],
                "bone_name": ev["bone_name"],
            }
            # Add resource path if available
            ri = ev["resource_index"]
            if 0 <= ri < len(resources):
                flat["resource_path"] = resources[ri]["path"]
                flat["resource_type"] = resources[ri]["type_name"]

            # Include extra fields if non-zero
            if any(e != 0.0 for e in ev["extra"]):
                flat["extra"] = ev["extra"]

            if "unknown_28" in ev:
                flat["unknown_28"] = ev["unknown_28"]

            events.append(flat)
    return events


def parse_obb_section(data: bytes, offset: int) -> tuple:
    """Parse the OBB extension section."""
    if offset + 4 > len(data):
        return [], offset

    obb_count = read_int32(data, offset)
    off = offset + 4

    obbs = []
    for i in range(obb_count):
        if off + 0x84 > len(data):
            break
        # Parse center, rotation matrix, half-extents
        center = struct.unpack_from('<3f', data, off)
        rotation = struct.unpack_from('<9f', data, off + 0x0C)
        half_extents = struct.unpack_from('<3f', data, off + 0x30)
        obbs.append({
            "center": list(center),
            "half_extents": list(half_extents),
        })
        off += 0x84

    return obbs, off


# ─── Main parser ─────────────────────────────────────────────────────────────

def parse_mod_file(filepath: str) -> dict:
    """Parse a .mod file and return structured data."""
    with open(filepath, 'rb') as f:
        data = f.read()

    filename = os.path.basename(filepath)
    result = {
        "filename": filename,
        "file_size": len(data),
    }

    if len(data) < 12:
        result["error"] = "File too small"
        return result

    # File version
    file_version = read_uint32(data, 0)
    result["file_version"] = f"0x{file_version:X}"

    if file_version != MOD_VERSION:
        result["warning"] = f"Unexpected version 0x{file_version:X} (expected 0x{MOD_VERSION:X})"

    # Find Kudiz section
    kudiz_offset = find_kudiz_offset(data)
    if kudiz_offset is None:
        result["error"] = "Could not find Kudiz OBS section"
        return result

    result["kudiz_offset"] = f"0x{kudiz_offset:X}"
    result["cmodel_size"] = kudiz_offset - 4

    # Parse materials
    off = kudiz_offset
    mat_count = read_int32(data, off)
    off += 4

    materials, off = parse_materials(data, off, mat_count)
    result["materials"] = materials
    result["material_count"] = mat_count

    # Parse animations
    anim_count = read_int32(data, off)
    off += 4

    animations, off = parse_animations(data, off, anim_count, file_version)
    result["animations"] = animations
    result["animation_count"] = anim_count

    # Total event count
    total_events = sum(a.get("event_count", 0) for a in animations)
    result["total_event_count"] = total_events

    # Parse OBB section (version > 0xC61F)
    if file_version > 0xC61F:
        obbs, off = parse_obb_section(data, off)
        result["obb_count"] = len(obbs)
        if obbs:
            result["obbs"] = obbs

    return result


# ─── Output formatters ───────────────────────────────────────────────────────

def print_summary(result: dict):
    """Print a human-readable summary."""
    print(f"File: {result['filename']} ({result['file_size']} bytes)")
    print(f"Version: {result.get('file_version', '?')}")

    if "error" in result:
        print(f"ERROR: {result['error']}")
        return

    print(f"Kudiz offset: {result.get('kudiz_offset', '?')}")
    print(f"CModel size: {result.get('cmodel_size', '?')} bytes")
    print(f"Materials: {result.get('material_count', 0)}")
    print(f"Animations: {result.get('animation_count', 0)}")
    print(f"Total events: {result.get('total_event_count', 0)}")
    print(f"OBBs: {result.get('obb_count', 0)}")

    # Event type distribution
    type_counts = {}
    for anim in result.get("animations", []):
        for ev in anim.get("events", []):
            t = ev["type"]
            type_counts[t] = type_counts.get(t, 0) + 1

    if type_counts:
        print("\nEvent distribution:")
        for t, c in sorted(type_counts.items(), key=lambda x: -x[1]):
            print(f"  {t}: {c}")


def print_events(result: dict, all_events: bool = False):
    """Print animation events in human-readable format."""
    interesting_types = {
        0x02, 0x03, 0x06, 0x07, 0x0B,  # ATTACH/DETACH/SOUND
    }

    for anim in result.get("animations", []):
        events = anim.get("events", [])
        if not events:
            continue

        filtered = events if all_events else [
            e for e in events if e["type_id"] in interesting_types
        ]
        if not filtered:
            continue

        print(f"\n--- {anim['name']} ({anim.get('ani_file', '?')}) ---")
        if anim.get("resources"):
            print(f"  Resources: {len(anim['resources'])}")
            for r in anim["resources"]:
                print(f"    [{r['index']}] {r['type_name']}: {r['path']}")

        for ev in filtered:
            frame_str = f"F{ev['frame']}" if ev['frame'] >= 0 else "F-1"
            bone_str = f" bone=\"{ev['bone_name']}\"" if ev['bone_name'] else ""
            res_str = f" res={ev['resource_index']}"
            extra_str = ""
            if ev.get("resource_path"):
                extra_str = f" ({ev['resource_path']})"

            params = ev['float_params']
            params_str = ""
            if any(p != 0.0 for p in params):
                params_str = f" params=[{', '.join(f'{p:.1f}' for p in params)}]"

            print(f"  [{frame_str}] {ev['type']}{res_str}{params_str}{bone_str}{extra_str}")


# ─── CLI ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Parse SDGO .mod files and extract animation events."
    )
    parser.add_argument(
        "path",
        help="Path to a .mod file or directory of .mod files",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output as JSON",
    )
    parser.add_argument(
        "--all-events",
        action="store_true",
        help="Show all event types (default: only ATTACH/DETACH/SOUND)",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="Print summary statistics only",
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="Compact JSON output (no indentation)",
    )

    args = parser.parse_args()
    path = Path(args.path)

    if path.is_file():
        files = [path]
    elif path.is_dir():
        files = sorted(path.glob("*.mod"))
        if not files:
            print(f"No .mod files found in {path}", file=sys.stderr)
            sys.exit(1)
    else:
        print(f"Path not found: {path}", file=sys.stderr)
        sys.exit(1)

    results = []
    for fpath in files:
        result = parse_mod_file(str(fpath))
        results.append(result)

    if args.json:
        output = results[0] if len(results) == 1 else results
        indent = None if args.compact else 2
        print(json.dumps(output, indent=indent, ensure_ascii=False))
    elif args.summary:
        for result in results:
            print_summary(result)
            if len(results) > 1:
                print()
    else:
        for result in results:
            print_summary(result)
            print_events(result, all_events=args.all_events)
            if len(results) > 1:
                print("\n" + "=" * 60)


if __name__ == "__main__":
    main()
