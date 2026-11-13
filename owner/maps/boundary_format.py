"""Portable per-phase native boundary declarations shared by both runtimes."""
from __future__ import annotations
import math
import struct

MAGIC = b'SDGOBND1'
HEADER = struct.Struct('<8s6I')
ROW = struct.Struct('<4I4f3I')
MAX_ROWS = 16384
FIELDS = {'mission_id', 'phase', 'flags', 'warning_speech_id', 'rect',
          'damage_edge_mask', 'damage_permille', 'damage_cooldown_ms'}


def _integer(value, name, low=0, high=0xffffffff):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'boundary {name} is outside {low}..{high}')
    return value


def normalize(rows):
    if not isinstance(rows, list) or len(rows) > MAX_ROWS:
        raise ValueError('boundary declarations exceed the row limit')
    output = []
    seen = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != FIELDS:
            raise ValueError('boundary declaration fields do not match the protocol')
        item = {key: _integer(row[key], key) for key in sorted(FIELDS - {'rect'})}
        _integer(item['mission_id'], 'mission_id', 1)
        _integer(item['phase'], 'phase', 1, 8)
        flags = _integer(item['flags'], 'flags', 0, 7)
        rect = row['rect']
        if (not isinstance(rect, (list, tuple)) or len(rect) != 4 or any(
                isinstance(v, bool) or not isinstance(v, (int, float)) or
                not math.isfinite(v) or abs(v) > 1e6 for v in rect)):
            raise ValueError('boundary rect requires four finite coordinates')
        item['rect'] = list(struct.unpack('<4f', struct.pack('<4f', *rect)))
        left, top, right, bottom = item['rect']
        if flags & 1:
            if not left < right or not bottom < top:
                raise ValueError('boundary rect has reversed or empty bounds')
        elif flags & 6:
            raise ValueError('boundary warning/damage requires an enabled area')
        if bool(flags & 2) != bool(item['warning_speech_id']):
            raise ValueError('boundary warning speech and flag disagree')
        edge, damage, cooldown = (item[k] for k in (
            'damage_edge_mask', 'damage_permille', 'damage_cooldown_ms'))
        if flags & 4:
            if not 1 <= edge <= 15 or not 1 <= damage <= 999 or not 250 <= cooldown <= 60000:
                raise ValueError('boundary damage parameters are invalid')
        elif edge or damage or cooldown:
            raise ValueError('disabled boundary damage has nonzero parameters')
        key = item['mission_id'], item['phase']
        if key in seen:
            raise ValueError('duplicate boundary mission/phase declaration')
        seen.add(key)
        output.append(item)
    return sorted(output, key=lambda item: (item['mission_id'], item['phase']))


def _checksum(raw):
    value = 2166136261
    for byte in raw:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def encode(rows):
    rows = normalize(rows)
    raw = b''.join(ROW.pack(item['mission_id'], item['phase'], item['flags'], item['warning_speech_id'],
                            *item['rect'], item['damage_edge_mask'], item['damage_permille'],
                            item['damage_cooldown_ms']) for item in rows)
    return HEADER.pack(MAGIC, 1, HEADER.size, ROW.size, len(rows), len(raw), _checksum(raw)) + raw


def decode(raw):
    if not isinstance(raw, bytes) or len(raw) < HEADER.size:
        raise ValueError('boundary declaration has a short header')
    magic, version, header, size, count, payload, checksum = HEADER.unpack_from(raw)
    if ((magic, version, header, size) != (MAGIC, 1, HEADER.size, ROW.size) or count > MAX_ROWS or
            payload != count * ROW.size or len(raw) != HEADER.size + payload or
            checksum != _checksum(raw[HEADER.size:])):
        raise ValueError('boundary declaration header, length or checksum does not match')
    rows = []
    for values in ROW.iter_unpack(raw[HEADER.size:]):
        rows.append(dict(zip(('mission_id', 'phase', 'flags', 'warning_speech_id'), values[:4]),
                         rect=list(values[4:8]), damage_edge_mask=values[8],
                         damage_permille=values[9], damage_cooldown_ms=values[10]))
    normalized = normalize(rows)
    if normalized != rows:
        raise ValueError('boundary declarations are not sorted')
    return rows
