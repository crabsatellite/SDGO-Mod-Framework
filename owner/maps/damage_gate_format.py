"""Paired prerequisite damage gates for imported mission targets."""
from __future__ import annotations
import struct

MAGIC = b'SDGODMG1'
HEADER = struct.Struct('<8s6I')
ROW = struct.Struct('<5I')
MAX_ROWS = 16384
FIELDS = ('mission_id', 'phase', 'target_npc_id', 'guard_npc_id', 'required_defeats')


def normalize(rows):
    if not isinstance(rows, list) or len(rows) > MAX_ROWS:
        raise ValueError('damage gate declarations exceed the row limit')
    output, seen = [], set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != set(FIELDS):
            raise ValueError('damage gate declaration fields do not match the protocol')
        item = {}
        for key in FIELDS:
            value = row[key]
            limit = 8 if key == 'phase' else 255 if key == 'required_defeats' else 0xffffffff
            if type(value) is not int or not 1 <= value <= limit:
                raise ValueError('damage gate ' + key + ' is outside its native range')
            item[key] = value
        if item['target_npc_id'] == item['guard_npc_id']:
            raise ValueError('damage gate target cannot guard itself')
        identity = tuple(item[k] for k in FIELDS[:4])
        if identity in seen:
            raise ValueError('duplicate damage gate declaration')
        seen.add(identity)
        output.append(item)
    return sorted(output, key=lambda row: tuple(row[k] for k in FIELDS[:4]))


def checksum(raw):
    value = 2166136261
    for byte in raw:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def encode(rows):
    rows = normalize(rows)
    payload = b''.join(ROW.pack(*(row[k] for k in FIELDS)) for row in rows)
    return HEADER.pack(MAGIC, 1, HEADER.size, ROW.size, len(rows), len(payload), checksum(payload)) + payload


def decode(raw):
    if not isinstance(raw, bytes) or len(raw) < HEADER.size:
        raise ValueError('damage gate declaration has a short header')
    magic, version, header, size, count, length, digest = HEADER.unpack_from(raw)
    if ((magic, version, header, size) != (MAGIC, 1, HEADER.size, ROW.size) or count > MAX_ROWS or
            length != count * ROW.size or len(raw) != header + length or checksum(raw[header:]) != digest):
        raise ValueError('damage gate declaration header, length or checksum does not match')
    rows = [dict(zip(FIELDS, values)) for values in ROW.iter_unpack(raw[header:])]
    if normalize(rows) != rows:
        raise ValueError('damage gate declarations are not sorted')
    return rows
