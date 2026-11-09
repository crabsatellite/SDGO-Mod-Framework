"""Load and validate a neutral map package (`sdgo.map-package` v1).

A map package is authored outside the game: glTF 2.0 models, PNG images and a JSON manifest. Nothing in it
names game-internal records except optional references to resources that already exist in the target
client (``native_model``, native BGM); those are checked against the client when the package is compiled.

Coordinate frame (the same as the game world): X east, Y up, Z north, origin at the terrain centre, game
units. Height and mask images cover the whole terrain with row 0 = north edge and column 0 = west edge.
glTF assets keep their own convention (right handed, -Z forward); the compiler mirrors Z when converting.
``rotation_y`` turns an object clockwise seen from above: 0 faces north (+Z), 90 faces east (+X).
"""
from __future__ import annotations

import hashlib
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any

SCHEMA = 'sdgo.map-package'
SCHEMA_VERSION = 1
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_PACKAGE_BYTES = 512 * 1024 * 1024
MAX_FILES = 2048
MAX_OBJECTS = 1024
MAX_LAYERS = 8
HALF_EXTENTS = (4096.0, 8192.0, 16384.0)
GRID = 257
PVP_MODES = ('normal', 'deathmatch', 'grid', 'tag', 'captain', 'ai', 'ai_battleship')
SERVER_TYPES = ('training', 'beginner', 'expert', 'clan')
TEAM_SPAWNS = 6
SHARED_SPAWNS = 12
ITEM_PATTERN_POINTS = 8
NATIVE_PATH = re.compile(r'(?i)(mdrs\\[0-9a-z_]{1,24}\.mod|bgm\\[0-9a-z_]{1,40}\.mp3)')
KEY = re.compile(r'[a-z0-9][a-z0-9._-]{2,63}')


class MapPackageError(ValueError):
    pass


def _fail(message: str):
    raise MapPackageError(message)


def _number(value: Any, what: str, lo: float | None = None, hi: float | None = None) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        _fail(f'{what} 必须是数字')
    value = float(value)
    if (lo is not None and value < lo) or (hi is not None and value > hi):
        _fail(f'{what} 超出范围 [{lo}, {hi}]')
    return value


def _vector(value: Any, size: int, what: str, limit: float = 1e6) -> tuple[float, ...]:
    if not isinstance(value, (list, tuple)) or len(value) != size:
        _fail(f'{what} 必须是 {size} 个数字')
    return tuple(_number(v, what, -limit, limit) for v in value)


def _colour(value: Any, what: str) -> tuple[float, float, float]:
    return tuple(_number(v, what, 0.0, 4.0) for v in _vector(value, 3, what))


def _text(value: Any, what: str, limit: int, required: bool = True) -> str:
    if value is None and not required:
        return ''
    if not isinstance(value, str) or (required and not value.strip()) or len(value) > limit or '\0' in value:
        _fail(f'{what} 必须是 1..{limit} 个字符的文本')
    return value


def _names(value: Any, what: str, limit: int, en_limit: int) -> dict[str, str]:
    if not isinstance(value, dict) or not isinstance(value.get('zh'), str):
        _fail(f'{what} 需要 zh 文本')
    zh = _text(value['zh'], what + '.zh', limit)
    en = _text(value.get('en'), what + '.en', en_limit, required=False)
    if en and not all(32 <= ord(c) < 127 for c in en):
        _fail(f'{what}.en 只能使用 ASCII 字符')
    return {'zh': zh, 'en': en}


@dataclass
class Spawn:
    position: tuple[float, float, float]
    facing: tuple[float, float]


@dataclass
class MapObject:
    position: tuple[float, float, float]
    rotation_y: float
    scale: tuple[float, float, float]
    model: str = ''                      # package path of a .glb/.gltf
    native_model: str = ''               # existing client model, e.g. mdrs\0823.mod (reference only)
    collision: str | list = 'auto'       # 'auto' | 'none' | [{"center", "half_extents", "rotation_y"}]
    object_id: str = ''
    only: str = ''                       # '' | 'pvp' | 'mission'
    initially_hidden: bool = False


@dataclass
class Layer:
    texture: str
    mask: str
    tiling: float


@dataclass
class Package:
    root: Path
    manifest: dict
    map_key: str
    name: dict
    description: dict
    modes: list[str]
    servers: list[str]
    half_extent: float
    height_image: str
    height_min: float
    height_max: float
    layers: list[Layer]
    sun_direction: tuple[float, float, float]
    sun_color: tuple[float, float, float]
    ambient: tuple[float, float, float]
    bake_shadows: bool
    fog_color: tuple[float, float, float]
    fog_start: float
    fog_end: float
    sky: dict
    play_half_x: float
    play_half_z: float
    ceiling: float
    objects: list[MapObject]
    spawns: dict[str, list[Spawn]]
    sectors: dict[str, list[Spawn]]
    item_patterns: dict[str, list[list[tuple[float, float, float]]]]
    preview: str
    minimap: str
    bgm_native: str = ''
    bgm_file: str = ''
    files: dict[str, str] = field(default_factory=dict)

    def read(self, relative: str) -> bytes:
        if relative not in self.files:
            _fail(f'文件未在 files 清单中登记：{relative}')
        path = (self.root / relative).resolve()
        if not path.is_relative_to(self.root) or not path.is_file():
            _fail(f'找不到包内文件：{relative}')
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != self.files[relative]:
            _fail(f'文件摘要与 files 清单不一致：{relative}')
        return raw

    def resolver(self, base: str):
        """read_file callback for glTF external buffers/images relative to `base`."""
        folder = PurePosixPath(base).parent

        def read(uri: str) -> bytes:
            target = str(PurePosixPath(folder, uri))
            return self.read(_relative(target, 'glTF uri'))
        return read


def _relative(value: Any, what: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 200:
        _fail(f'{what} 必须是包内相对路径')
    path = PurePosixPath(value.replace('\\', '/'))
    if path.is_absolute() or any(part in ('', '.', '..') for part in path.parts) or ':' in value:
        _fail(f'{what} 不是合法的包内相对路径：{value}')
    return path.as_posix()


def _spawn(value: Any, what: str, limit: float) -> Spawn:
    if not isinstance(value, dict):
        _fail(f'{what} 必须是对象')
    position = _vector(value.get('position'), 3, what + '.position', limit * 2)
    if abs(position[0]) > limit or abs(position[2]) > limit:
        _fail(f'{what} 位于地形之外')
    fx, fz = _vector(value.get('facing', [0, 1]), 2, what + '.facing')
    length = math.hypot(fx, fz)
    if length < 1e-6:
        _fail(f'{what}.facing 不能为零向量')
    return Spawn(position, (fx / length, fz / length))


def _spawn_list(value: Any, what: str, limit: float, lo: int, hi: int) -> list[Spawn]:
    if value is None and lo == 0:
        return []
    if not isinstance(value, list) or not lo <= len(value) <= hi:
        _fail(f'{what} 需要 {lo}..{hi} 个出生点')
    return [_spawn(item, f'{what}[{i}]', limit) for i, item in enumerate(value)]


def _collision(value: Any, what: str):
    if value in (None, 'auto'):
        return 'auto'
    if value == 'none':
        return 'none'
    if not isinstance(value, list) or not 1 <= len(value) <= 64:
        _fail(f'{what} 必须是 auto、none 或 1..64 个碰撞盒')
    boxes = []
    for i, box in enumerate(value):
        if not isinstance(box, dict):
            _fail(f'{what}[{i}] 必须是对象')
        boxes.append({'center': _vector(box.get('center'), 3, f'{what}[{i}].center'),
                      'half_extents': tuple(_number(v, f'{what}[{i}].half_extents', 0.5, 1e5)
                                            for v in _vector(box.get('half_extents'), 3, f'{what}[{i}].half_extents')),
                      'rotation_y': _number(box.get('rotation_y', 0.0), f'{what}[{i}].rotation_y', -360, 360)})
    return boxes


def load(root: Path | str) -> Package:
    """Read `map.json` from a package folder and validate every field and file digest."""
    root = Path(root).resolve()
    manifest_path = root / 'map.json'
    if not manifest_path.is_file() or manifest_path.stat().st_size > MAX_MANIFEST_BYTES:
        _fail('地图包缺少 map.json 或文件过大')
    try:
        m = json.loads(manifest_path.read_bytes().decode('utf-8-sig'))
    except (UnicodeDecodeError, ValueError) as error:
        _fail(f'map.json 不是有效的 UTF-8 JSON：{error}')
    if not isinstance(m, dict) or m.get('schema') != SCHEMA or m.get('schema_version') != SCHEMA_VERSION:
        _fail(f'map.json 必须声明 schema={SCHEMA} 且 schema_version={SCHEMA_VERSION}')
    key = m.get('map_key')
    if not isinstance(key, str) or not KEY.fullmatch(key):
        _fail('map_key 必须是 3..64 个小写字母、数字、点、下划线或连字符')

    files = m.get('files')
    if not isinstance(files, dict) or not 1 <= len(files) <= MAX_FILES:
        _fail('files 必须列出包内所有文件的 SHA-256')
    checked: dict[str, str] = {}
    total = 0
    for name, digest in files.items():
        relative = _relative(name, 'files')
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            _fail(f'files[{name}] 不是 SHA-256')
        path = (root / relative).resolve()
        if not path.is_relative_to(root) or not path.is_file() or path.is_symlink():
            _fail(f'files 列出的文件不存在：{name}')
        size = path.stat().st_size
        if size > MAX_FILE_BYTES:
            _fail(f'文件超过 64 MiB：{name}')
        total += size
        checked[relative] = digest
    if total > MAX_PACKAGE_BYTES:
        _fail('地图包超过 512 MiB')

    def packaged(value: Any, what: str, suffixes: tuple[str, ...]) -> str:
        relative = _relative(value, what)
        if relative not in checked:
            _fail(f'{what} 引用的文件未登记在 files 中：{relative}')
        if not relative.lower().endswith(suffixes):
            _fail(f'{what} 需要 {"/".join(suffixes)} 文件')
        return relative

    modes = m.get('modes', ['normal', 'deathmatch'])
    if not isinstance(modes, list) or any(mode not in PVP_MODES + ('mission',) for mode in modes):
        _fail('modes 只能包含 ' + ', '.join(PVP_MODES + ('mission',)))
    servers = m.get('servers', ['beginner', 'expert'])
    if not isinstance(servers, list) or not servers or any(s not in SERVER_TYPES for s in servers):
        _fail('servers 只能包含 ' + ', '.join(SERVER_TYPES))

    terrain = m.get('terrain')
    if not isinstance(terrain, dict):
        _fail('缺少 terrain')
    half = _number(terrain.get('half_extent'), 'terrain.half_extent')
    if half not in HALF_EXTENTS:
        _fail('terrain.half_extent 只能是 4096、8192 或 16384')
    height = terrain.get('height') or {}
    height_image = packaged(height.get('image'), 'terrain.height.image', ('.png',))
    h_min = _number(height.get('min', 0.0), 'terrain.height.min', -20000, 20000)
    h_max = _number(height.get('max', 0.0), 'terrain.height.max', -20000, 20000)
    if h_max < h_min:
        _fail('terrain.height.max 不能小于 min')
    raw_layers = terrain.get('layers')
    if not isinstance(raw_layers, list) or not 1 <= len(raw_layers) <= MAX_LAYERS:
        _fail(f'terrain.layers 需要 1..{MAX_LAYERS} 层')
    layers = []
    for i, layer in enumerate(raw_layers):
        if not isinstance(layer, dict):
            _fail(f'terrain.layers[{i}] 必须是对象')
        mask = '' if i == 0 and not layer.get('mask') else packaged(layer.get('mask'), f'terrain.layers[{i}].mask', ('.png',))
        layers.append(Layer(packaged(layer.get('texture'), f'terrain.layers[{i}].texture', ('.png',)), mask,
                            _number(layer.get('tiling', 32), f'terrain.layers[{i}].tiling', 1, 512)))
    lighting = terrain.get('lighting') or {}
    sun = _vector(lighting.get('sun_direction', [-0.4, -1.0, 0.3]), 3, 'terrain.lighting.sun_direction')
    if sun[1] >= 0 or math.hypot(*sun) < 1e-6:
        _fail('terrain.lighting.sun_direction 必须指向下方 (y < 0)')
    fog = terrain.get('fog') or {}
    fog_start = _number(fog.get('start', 6000), 'terrain.fog.start', 0, 100000)
    fog_end = _number(fog.get('end', 16000), 'terrain.fog.end', 1, 100000)
    if fog_end <= fog_start:
        _fail('terrain.fog.end 必须大于 start')
    sky = terrain.get('sky') or {}
    sky = {'zenith': _colour(sky.get('zenith', [0.25, 0.45, 0.85]), 'terrain.sky.zenith'),
           'horizon': _colour(sky.get('horizon', [0.78, 0.86, 0.95]), 'terrain.sky.horizon'),
           'ground': _colour(sky.get('ground', [0.55, 0.55, 0.52]), 'terrain.sky.ground')}
    play = terrain.get('play_area') or {}
    play_x = _number(play.get('half_x', half * 0.5), 'terrain.play_area.half_x', 512, half)
    play_z = _number(play.get('half_z', half * 0.5), 'terrain.play_area.half_z', 512, half)
    ceiling = _number(play.get('ceiling', h_max + 1500), 'terrain.play_area.ceiling', h_min + 200, 100000)

    raw_objects = m.get('objects', [])
    if not isinstance(raw_objects, list) or len(raw_objects) > MAX_OBJECTS:
        _fail(f'objects 最多 {MAX_OBJECTS} 个')
    objects = []
    ids = set()
    for i, row in enumerate(raw_objects):
        what = f'objects[{i}]'
        if not isinstance(row, dict):
            _fail(f'{what} 必须是对象')
        position = _vector(row.get('position'), 3, what + '.position', half * 2)
        if abs(position[0]) > half or abs(position[2]) > half:
            _fail(f'{what} 位于地形之外')
        scale = row.get('scale', 1.0)
        scale = (_number(scale, what + '.scale', 0.01, 100),) * 3 if not isinstance(scale, list) else \
            tuple(_number(s, what + '.scale', 0.01, 100) for s in _vector(scale, 3, what + '.scale'))
        obj = MapObject(position, _number(row.get('rotation_y', 0.0), what + '.rotation_y', -360, 360), scale,
                        collision=_collision(row.get('collision'), what + '.collision'))
        if ('model' in row) == ('native_model' in row):
            _fail(f'{what} 需要且只能有 model 或 native_model 之一')
        if 'model' in row:
            obj.model = packaged(row['model'], what + '.model', ('.glb', '.gltf'))
        else:
            native = row['native_model']
            if not isinstance(native, str) or not NATIVE_PATH.fullmatch(native) or not native.lower().endswith('.mod'):
                _fail(f'{what}.native_model 必须形如 mdrs\\\\NNNN.mod')
            obj.native_model = native
        if row.get('id') is not None:
            obj.object_id = _text(row['id'], what + '.id', 31)
            if not re.fullmatch(r'[A-Za-z0-9_]+', obj.object_id) or obj.object_id in ids:
                _fail(f'{what}.id 必须唯一且只含字母、数字和下划线')
            ids.add(obj.object_id)
        obj.only = row.get('only', '')
        if obj.only not in ('', 'pvp', 'mission'):
            _fail(f'{what}.only 只能是 pvp 或 mission')
        obj.initially_hidden = bool(row.get('initially_hidden', False))
        objects.append(obj)

    raw_spawns = m.get('spawns')
    if not isinstance(raw_spawns, dict):
        _fail('缺少 spawns')
    spawns = {'team_a': _spawn_list(raw_spawns.get('team_a'), 'spawns.team_a', half, TEAM_SPAWNS, TEAM_SPAWNS),
              'team_b': _spawn_list(raw_spawns.get('team_b'), 'spawns.team_b', half, TEAM_SPAWNS, TEAM_SPAWNS),
              'shared': _spawn_list(raw_spawns.get('shared'), 'spawns.shared', half, 1, SHARED_SPAWNS),
              'grid_respawn_a': _spawn_list(raw_spawns.get('grid_respawn_a'), 'spawns.grid_respawn_a', half, 0, 8),
              'grid_respawn_b': _spawn_list(raw_spawns.get('grid_respawn_b'), 'spawns.grid_respawn_b', half, 0, 8)}
    if 'grid' in modes and (not spawns['grid_respawn_a'] or not spawns['grid_respawn_b']):
        _fail('grid 模式需要 grid_respawn_a 与 grid_respawn_b')
    sectors = {}
    for name, sector in (m.get('sectors') or {}).items():
        if not re.fullmatch(r'[a-z0-9_]{1,31}', str(name)) or not isinstance(sector, dict):
            _fail(f'sectors.{name} 名称或内容无效')
        sectors[name] = _spawn_list(sector.get('spawns'), f'sectors.{name}.spawns', half, 1, 12)

    patterns = {'red': [], 'blue': []}
    raw_patterns = m.get('item_patterns') or {}
    for team in ('red', 'blue'):
        rows = raw_patterns.get(team, [])
        if not isinstance(rows, list) or len(rows) > 8:
            _fail(f'item_patterns.{team} 最多 8 组')
        for p, pattern in enumerate(rows):
            if not isinstance(pattern, list) or len(pattern) != ITEM_PATTERN_POINTS:
                _fail(f'item_patterns.{team}[{p}] 需要正好 {ITEM_PATTERN_POINTS} 个道具点')
            patterns[team].append([_vector((point or {}).get('position'), 3, f'item_patterns.{team}[{p}]', half * 2)
                                   for point in pattern])
    if len(patterns['red']) != len(patterns['blue']):
        _fail('item_patterns 的 red 与 blue 组数必须相同')

    presentation = m.get('presentation') or {}
    preview = packaged(presentation.get('preview'), 'presentation.preview', ('.png',))
    minimap = packaged(presentation.get('minimap'), 'presentation.minimap', ('.png',))
    bgm = presentation.get('bgm')
    bgm_native = bgm_file = ''
    if isinstance(bgm, dict) and 'native' in bgm:
        bgm_native = bgm['native']
        if not isinstance(bgm_native, str) or not NATIVE_PATH.fullmatch(bgm_native) or not bgm_native.lower().endswith('.mp3'):
            _fail('presentation.bgm.native 必须形如 BGM\\\\NNNN.mp3')
    elif bgm is not None:
        bgm_file = packaged(bgm, 'presentation.bgm', ('.mp3',))
    else:
        _fail('presentation.bgm 必须是包内 mp3 或 {"native": "BGM\\\\NNNN.mp3"}')

    return Package(root=root, manifest=m, map_key=key, name=_names(m.get('name'), 'name', 48, 100),
                   description=(_names(m['description'], 'description', 101, 400) if m.get('description')
                                else {'zh': '', 'en': ''}),
                   modes=[x for x in modes if x != 'mission'], servers=servers, half_extent=half,
                   height_image=height_image, height_min=h_min, height_max=h_max, layers=layers,
                   sun_direction=sun, sun_color=_colour(lighting.get('sun_color', [1, 1, 1]), 'terrain.lighting.sun_color'),
                   ambient=_colour(lighting.get('ambient', [0.4, 0.4, 0.45]), 'terrain.lighting.ambient'),
                   bake_shadows=bool(lighting.get('bake_shadows', True)),
                   fog_color=_colour(fog.get('color', [0.7, 0.75, 0.8]), 'terrain.fog.color'),
                   fog_start=fog_start, fog_end=fog_end, sky=sky, play_half_x=play_x, play_half_z=play_z,
                   ceiling=ceiling, objects=objects, spawns=spawns, sectors=sectors, item_patterns=patterns,
                   preview=preview, minimap=minimap, bgm_native=bgm_native, bgm_file=bgm_file, files=checked)
