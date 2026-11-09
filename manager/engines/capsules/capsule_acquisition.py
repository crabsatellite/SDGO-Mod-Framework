"""Build encyclopedia acquisition hints from the published capsule catalog."""
from __future__ import annotations

from collections import defaultdict
import hashlib
from pathlib import Path
import struct

FILE = 'capsule-acquisition.bin'
MARKER = b'SDGO_CAPSULE_ACQUISITION_V1'
MAGIC = b'SDGOCAS1'
MAX_UNITS = 20000
LABELS = {'activity': '活动扭蛋机', 'fixed': '普通扭蛋机',
          'coin': '代币扭蛋机', 'custom': '自定义扭蛋机'}


def imported_item_rows(client):
    """Read the existing protocol catalog; imported rewards need not be in GDT 0031."""
    path = Path(client) / 'sdgo/mods/unit_catalog/catalog.bin'
    if not path.is_file():
        return [], None
    if path.stat().st_size > 65 * 1024 * 1024:
        raise ValueError('Imported unit catalog exceeds the protocol limit')
    raw = path.read_bytes()
    if len(raw) < 32 or raw[:8] != b'SDGOUN1\0':
        raise ValueError('Imported unit catalog header is invalid')
    count = struct.unpack_from('<I', raw, 16)[0]
    if not 0 < count <= 8192:
        raise ValueError('Imported unit catalog record count is invalid')
    cursor = 32
    records, items = [], []
    for _ in range(count):
        if len(raw) - cursor < 24:
            raise ValueError('Imported unit catalog record header is truncated')
        kind, identity, size, *_ = struct.unpack_from('<6I', raw, cursor)
        cursor += 24
        if size > len(raw) - cursor:
            raise ValueError('Imported unit catalog record is truncated')
        record = raw[cursor:cursor + size]
        cursor += size
        records.append((kind, identity, record))
        if kind == 34:
            items.append(record)
    from owner.units.compiler import native_catalog
    if cursor != len(raw) or native_catalog(records) != raw:
        raise ValueError('Imported unit catalog checksum or layout is invalid')
    return items, hashlib.sha256(raw).hexdigest()


def display_number(machine_id, kind):
    """Native ordinary/activity IDs encode their visible number as N*100+1."""
    if type(machine_id) is not int or machine_id <= 0:
        return None
    if kind == 'custom':
        return machine_id
    if machine_id % 100 == 1 and (kind != 'activity' or 1 <= machine_id // 100 <= 9899):
        return machine_id // 100
    return None


def _ranges(numbers):
    numbers = sorted(set(numbers))
    ranges = []
    for number in numbers:
        if ranges and number == ranges[-1][1] + 1:
            ranges[-1][1] = number
        else:
            ranges.append([number, number])
    return '、'.join(str(first) if first == last else f'{first}–{last}'
                    for first, last in ranges)


def _characters(text):
    return len(text.encode('utf-16-le')) // 2


def encode(catalog, item_rows):
    """Use active pools, including every unit reward, without reading player state."""
    categories = {}
    for row in item_rows:
        if len(row) != 614:
            raise ValueError('Acquisition hints require native 614-byte item records')
        category = struct.unpack_from('<I', row)[0]
        unit = struct.unpack_from('<I', row, 105)[0]
        if row[4] == 0 and category // 100000 == 11 and 0 < unit <= 9999999:
            if category in categories and categories[category] != unit:
                raise ValueError('A unit reward category has conflicting references')
            categories[category] = unit
    sources = defaultdict(dict)

    def add(unit, kind, machine):
        if type(unit) is not int or not 0 < unit <= 9999999:
            raise ValueError('Capsule acquisition unit identity is invalid')
        mid = machine['id']
        number = display_number(mid, kind)
        name = str(machine.get('name', '')).strip()
        # Pinned/legacy IDs are identities, never player-facing machine numbers.
        if number is None and not name:
            return
        sources[unit][kind, mid] = {'kind': kind, 'machine_id': mid,
            'display_number': number, 'name': name}

    # The native panel already derives ordinary and custom-machine sources
    # from their native tables. +214 is its event-source section: supplement
    # activities there rather than duplicating ordinary sources as events.
    # Drafts are deliberately absent: only applied activity declarations belong
    # to the capsule publication, and the full pool can contain several units.
    for machine in catalog.get('activity_machines', []):
        if machine.get('retired', False) or machine.get('enabled') is False:
            continue
        for reward in machine['pool']:
            if reward.get('entries', 0) > 0 and reward.get('amount', 0) > 0:
                unit = categories.get(reward['category_id'])
                if unit is not None:
                    add(unit, 'activity', machine)
    if len(sources) > MAX_UNITS:
        raise ValueError('Capsule acquisition hints exceed the unit limit')
    entries = []
    raw = bytearray(struct.pack('<8sII', MAGIC, 1, len(sources)))
    for unit, found in sorted(sources.items()):
        values = sorted(found.values(), key=lambda r: (list(LABELS).index(r['kind']), r['machine_id']))
        parts = []
        for kind in LABELS:
            rows = [r for r in values if r['kind'] == kind]
            numbers = [r['display_number'] for r in rows if r['display_number'] is not None]
            if numbers:
                parts.append(LABELS[kind] + _ranges(numbers) + '号')
            parts.extend(LABELS[kind] + '：' + r['name'] for r in rows if r['display_number'] is None)
        first = next(r for kind in LABELS for r in values if r['kind'] == kind)
        brief = LABELS[first['kind']] + (str(first['display_number']) + '号'
                if first['display_number'] is not None else '：' + first['name'])
        if _characters(brief) >= 100:
            raise ValueError('Capsule acquisition name exceeds the native text field')
        text = '；'.join(parts)
        if _characters(text) >= 100:
            text = brief
        full, short = text.encode('utf-16-le'), brief.encode('utf-16-le')
        raw += struct.pack('<III', unit, len(full) // 2, len(short) // 2) + full + short
        entries.append({'unit_id': unit, 'text': text, 'brief': brief,
                        'sources': sorted(values, key=lambda r: (r['kind'], r['machine_id']))})
    return bytes(raw), {'schema_version': 1, 'unit_count': len(entries), 'entries': entries,
                       'scope': 'completed-product sources; existing sources and blueprint fields retained'}
