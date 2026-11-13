"""Presentation-only quiet retreats on authoritative successful phase cleanup."""
from __future__ import annotations
import struct
from .damage_gate_format import checksum

MAGIC = b'SDGORET1'
HEADER = struct.Struct('<8s6I')
ROW = struct.Struct('<3I')
FIELDS = ('mission_id', 'phase', 'npc_id')
MAX_ROWS = 16384


def normalize(rows):
    if not isinstance(rows, list) or len(rows) > MAX_ROWS:
        raise ValueError('retreat declarations exceed the row limit')
    output, seen = [], set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != set(FIELDS):
            raise ValueError('retreat declaration fields do not match the protocol')
        item = {}
        for key in FIELDS:
            value = row[key]
            if type(value) is not int or not 1 <= value <= (8 if key == 'phase' else 0xffffffff):
                raise ValueError('retreat ' + key + ' is outside its native range')
            item[key] = value
        identity = tuple(item[key] for key in FIELDS)
        if identity in seen:
            raise ValueError('duplicate retreat declaration')
        seen.add(identity)
        output.append(item)
    return sorted(output, key=lambda row: tuple(row[key]for key in FIELDS))


def encode(rows):
    rows = normalize(rows)
    raw = b''.join(ROW.pack(*(row[key]for key in FIELDS))for row in rows)
    return HEADER.pack(MAGIC, 1, HEADER.size, ROW.size, len(rows), len(raw), checksum(raw)) + raw


def decode(raw):
    if not isinstance(raw, bytes) or len(raw) < HEADER.size:
        raise ValueError('retreat declaration has a short header')
    magic, version, header, size, count, length, digest = HEADER.unpack_from(raw)
    if ((magic, version, header, size) != (MAGIC, 1, HEADER.size, ROW.size) or count > MAX_ROWS or
            length != count * ROW.size or len(raw) != header + length or checksum(raw[header:]) != digest):
        raise ValueError('retreat declaration header, length or checksum does not match')
    rows = [dict(zip(FIELDS, values))for values in ROW.iter_unpack(raw[header:])]
    if normalize(rows) != rows:
        raise ValueError('retreat declarations are not sorted')
    return rows
