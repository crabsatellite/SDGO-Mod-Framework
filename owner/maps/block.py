"""Named native destroyable-block records used by map package schema v2.

The target client loads ``gdrs/0008.gdt`` as native grid kind 0x0E with fixed
513-byte rows. Terrain destroyable placements are scene kind 0x12 and the
server mirrors them in ``tbl_MapObjects`` / ``tbl_MapBlocks``. New definitions
clone a target-client/server template and replace only the traced identity,
internal key, player-facing name, model and HP fields.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass

from . import mod_format

CATALOG_KIND = 0x0E
RECORD_BYTES = 513
FIRST_BLOCK_ID = 7001
SERVER_OBJECT_COLUMNS = (
    'ObjectID', 'ObjectName', 'HP', 'CollisionDestroy', 'ExplosionRadius',
    'ExplosionRadiusPower', 'ExplosionTime', 'TeamKill', 'Damage', 'FragmentType',
)
SERVER_BLOCK_COLUMNS = (
    'MapID', 'BlockID', 'ObjectID', 'MinX', 'MinY', 'MinZ', 'MaxX', 'MaxY', 'MaxZ',
    'Child_No_1', 'Child_No_2', 'Child_No_3',
)


class BlockProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Template:
    native: bytes
    server: dict


def _w(row: bytearray, offset: int, size: int, text: str, what: str) -> None:
    raw = text.encode('utf-16-le')
    if not text or '\0' in text or len(raw) + 2 > size:
        raise BlockProtocolError(f'{what} 超过原生字段容量')
    row[offset:offset + size] = bytes(size)
    row[offset:offset + len(raw)] = raw


def build_definition(template: Template, *, identity: int, name: str, display_name: str, model: str,
                     hp: int | None = None) -> tuple[bytes, dict]:
    """Clone one verified native/server template and bind it to an imported model."""
    if len(template.native) != RECORD_BYTES:
        raise BlockProtocolError('原生物件模板不是 513 字节')
    if set(template.server) != set(SERVER_OBJECT_COLUMNS):
        raise BlockProtocolError('tbl_MapObjects 模板列与协议不匹配')
    if type(identity) is not int or not 1 <= identity <= 0xffffffff:
        raise BlockProtocolError('原生物件编号无效')
    if not isinstance(name, str) or not name.isascii() or not name or len(name) > 31:
        raise BlockProtocolError('服务端物件键需要 1..31 个 ASCII 字符')
    if not isinstance(display_name, str):
        raise BlockProtocolError('原生物件显示名称无效')
    if not isinstance(model, str) or '\0' in model or len(model) > 29:
        raise BlockProtocolError('原生物件模型路径需要 1..29 个字符')
    resolved_hp = template.server['HP'] if hp is None else hp
    if type(resolved_hp) is not int or not 1 <= resolved_hp <= 0x7fffffff:
        raise BlockProtocolError('原生物件 HP 需要 1..2147483647')

    row = bytearray(template.native)
    struct.pack_into('<I', row, 0, identity)
    row[4] = 0
    _w(row, 5, 100, display_name, '原生物件显示名称')
    for offset in (105, 169, 229):
        _w(row, offset, 60, model, '原生物件模型路径')
    struct.pack_into('<i', row, 165, resolved_hp)

    server = dict(template.server)
    server.update(ObjectID=identity, ObjectName=name, HP=resolved_hp)
    return bytes(row), server


def _rotate_y(point, angle: float):
    s, c = math.sin(angle), math.cos(angle)
    return (point[0] * c + point[2] * s, point[1], -point[0] * s + point[2] * c)


def world_aabb(model_raw: bytes, position, rotation_y: float, scale) -> tuple[float, float, float, float, float, float]:
    """Transform the compiled MOD collision OBBs into the server's world AABB."""
    try:
        model = mod_format.parse(model_raw)
    except Exception as error:
        raise BlockProtocolError(f'原生物件模型无法解析：{error}') from None
    if not model.obbs:
        raise BlockProtocolError('原生物件模型必须含碰撞盒')
    angle = math.radians(rotation_y)
    points = []
    for obb in model.obbs:
        for corner in obb.corners():
            scaled = tuple(float(corner[i]) * float(scale[i]) for i in range(3))
            rotated = _rotate_y(scaled, angle)
            points.append(tuple(float(position[i]) + rotated[i] for i in range(3)))
    lo = tuple(min(p[i] for p in points) for i in range(3))
    hi = tuple(max(p[i] for p in points) for i in range(3))
    if not all(math.isfinite(v) for v in (*lo, *hi)) or any(hi[i] <= lo[i] for i in range(3)):
        raise BlockProtocolError('原生物件碰撞范围无效')
    return (*lo, *hi)


def server_placement(*, map_id: int, block_id: int, object_id: int, aabb) -> dict:
    if type(block_id) is not int or not FIRST_BLOCK_ID <= block_id <= 0x7fffffff:
        raise BlockProtocolError('地图 BlockID 无效')
    min_x, min_y, min_z, max_x, max_y, max_z = aabb
    return {
        'MapID': map_id, 'BlockID': block_id, 'ObjectID': object_id,
        'MinX': min_x, 'MinY': min_y, 'MinZ': min_z,
        'MaxX': max_x, 'MaxY': max_y, 'MaxZ': max_z,
        'Child_No_1': 0, 'Child_No_2': 0, 'Child_No_3': 0,
    }


__all__ = [
    'BlockProtocolError', 'CATALOG_KIND', 'FIRST_BLOCK_ID', 'RECORD_BYTES',
    'SERVER_BLOCK_COLUMNS', 'SERVER_OBJECT_COLUMNS', 'Template',
    'build_definition', 'server_placement', 'world_aabb',
]
