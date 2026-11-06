"""Shared helpers for the roster publisher stages."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct

OWNER = 'gunion546'
UNIT_BYTES, WEAPON_BYTES, SKILL_BYTES, FUNNEL_BYTES = 4976, 1871, 1132, 732
ITEM_BYTES = 614
AFFILIATION_BYTES = 106
NAME_OFFSETS = (5, 105, 205)
UNIT_TRANSFORM_OFFSET = 316
UNIT_SKILL_OFFSETS = (575, 579)
UNIT_WEAPON_OFFSET = 585
WEAPON_FUNNEL_OFFSET = 1565
RESOURCE_ROOTS = ('mdrs', 'anrs', 'txrs', 'fxrs', 'sprs', 'sdrs', 'scp')


def sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def load_json(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def dump_json(path: Path, value) -> bytes:
    raw = (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=False) + '\n').encode('utf-8')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)
    return raw


def wide_field(raw: bytes, offset: int, capacity: int) -> str:
    data = raw[offset:offset + capacity]
    end = next((i for i in range(0, len(data) - 1, 2) if data[i:i + 2] == b'\0\0'), len(data) - len(data) % 2)
    return data[:end].decode('utf-16-le', errors='strict')


def write_wide(raw: bytearray, offset: int, capacity_chars: int, text: str) -> None:
    encoded = text.encode('utf-16-le')
    if len(encoded) + 2 > capacity_chars * 2:
        raise ValueError('text exceeds fixed native field: ' + text)
    raw[offset:offset + capacity_chars * 2] = encoded + bytes(capacity_chars * 2 - len(encoded))


def u32(raw: bytes, offset: int) -> int:
    return struct.unpack_from('<I', raw, offset)[0]


def normalize_path(path: str) -> str:
    return path.replace('\\', '/').lower()


class Config:
    """Caller-supplied private locations; validated once, never embedded."""

    def __init__(self, path: Path):
        value = load_json(path)
        self.path = path.resolve()
        self.delivery = Path(value['delivery_root']).resolve()
        self.native = self.delivery / 'native'
        self.plan_dir = Path(value['relocation_plan_dir']).resolve()
        self.baseline_client = Path(value['baseline_client']).resolve()
        self.output = Path(value['output_root']).resolve()
        self.owner = value.get('owner', OWNER)
        self.item_templates = {int(k): int(v) for k, v in value['item_templates_by_grade'].items()}
        self.excluded_units = {int(v) for v in value.get('excluded_source_units', [])}
        self.skill_atlas_source = value.get('skill_atlas_source', 'sprs/0594.spr')
        self.lua_toolchain_root = value.get('lua_toolchain_root')
        self.prescoped_program_manifests = list(value.get('prescoped_program_manifests', []))
        self.existing_catalog = value.get('existing_catalog')
        self.item_category_first = int(value.get('item_category_first', 1_119_010))
        self.affiliation_dictionary = value.get('affiliation_dictionary')
        self.module_binary = value.get('module_binary')
        for required in (self.delivery, self.native, self.plan_dir, self.baseline_client):
            if not required.is_dir():
                raise FileNotFoundError(str(required))
        self.output.mkdir(parents=True, exist_ok=True)

    def read_delivery(self, relative: str) -> bytes:
        target = (self.delivery / relative).resolve()
        if not target.is_relative_to(self.delivery) or target.is_symlink():
            raise ValueError('delivery member outside the delivery root: ' + relative)
        return target.read_bytes()
