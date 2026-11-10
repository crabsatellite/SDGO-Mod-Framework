"""Compile a validated map package into native client resources, a map table row and server rows.

Output of `compile_map` (nothing is written to disk here):

* ``resources``  overlay resources keyed by lower-case virtual path (``trns/m143.trn``, ``mdrs/m143o00.mod``,
  ``txrs/m143t00.txr``, ``sprs/m143p.spr`` ...). Every name starts with the map's resource prefix
  (``m<MapNo>``), which never matches a shipped (numeric) resource name.
* ``loose``      files placed next to the client (package BGM).
* ``rows``       native grid records ``(kind, id, bytes)``: one 844-byte map row (kind 0x0F).
* ``server``     rows for tbl_MapFile, tbl_MapData and tbl_MapItemPath.

`variant` selects which objects are placed: ``pvp`` (objects without ``only`` or ``only: pvp``) or
``mission`` (objects without ``only`` or ``only: mission``). A mission variant produces a separate map row
with the MissionMode flag, because the client never lists a mission row in PvP room lists.
"""
from __future__ import annotations

import hashlib
import io
import math
import struct
from dataclasses import dataclass, field
from typing import Callable

from PIL import Image, ImageFilter

from . import gltf, mod_format, trn_format, txr_format
from .package import GRID, Package, MapPackageError

MAP_KIND = 0x0F
MAP_ROW_BYTES = 0x34C
MAP_ROW_FIRST_ID = 100001
MAP_NO_LIMIT = 255
FIRST_IMPORTED_MAP_NO = 143
TEXTURE_LIMIT = 2048          # D3D9-safe maximum (txr_format.MAX_DIMENSION)
TEXTURE_FORMATS = ('PNG', 'JPEG')   # model and terrain colour textures
PREVIEW_SIZE = (120, 83)
RADAR_SIZE = 256
LIGHTMAP_SIZE = 512
NATIVE_PATH_CHARS = 31

# 14 flag bytes at +0x1FD in tbl_MapFile column order of the client row
FLAG_COLUMNS = ('NormalMode', 'DeathMatchMode', 'GridMatch', 'TagMatch', 'MissionMode', 'LeaderMatch', 'AiMatch',
                'AiBattleShipMatch', 'HotRandomMap', 'FastMatchRandom', 'Server_Training', 'Server_Beginner',
                'Server_Expert', 'Server_Clan')
MODE_COLUMNS = {'normal': 'NormalMode', 'deathmatch': 'DeathMatchMode', 'grid': 'GridMatch', 'tag': 'TagMatch',
                'captain': 'LeaderMatch', 'ai': 'AiMatch', 'ai_battleship': 'AiBattleShipMatch'}
SERVER_COLUMNS = {'training': 'Server_Training', 'beginner': 'Server_Beginner', 'expert': 'Server_Expert',
                  'clan': 'Server_Clan'}


class MapCompileError(ValueError):
    pass


@dataclass
class CompiledMap:
    map_no: int
    map_id: int
    variant: str
    prefix: str
    resources: dict[str, bytes] = field(default_factory=dict)
    loose: dict[str, bytes] = field(default_factory=dict)
    rows: list[tuple[int, int, bytes]] = field(default_factory=list)
    server: dict[str, list[dict]] = field(default_factory=dict)
    report: dict = field(default_factory=dict)


# --------------------------------------------------------------------------- helpers
def _native(path: str) -> str:
    """Virtual overlay name -> native backslash path with the shipped capitalisation of the folder."""
    folder, _, rest = path.partition('/')
    return {'trns': 'Trns', 'mdrs': 'mdrs', 'txrs': 'Txrs', 'sprs': 'sprs', 'bgm': 'BGM'}.get(folder, folder) + '\\' + rest


def _d3dcolor(rgb, alpha: int = 255) -> int:
    r, g, b = (max(0, min(255, int(round(c * 255)))) for c in rgb)
    return (alpha << 24) | (r << 16) | (g << 8) | b


def _png(image: Image.Image) -> bytes:
    out = io.BytesIO()
    image.save(out, format='PNG', optimize=False)
    return out.getvalue()


def _open(raw: bytes, what: str, formats: tuple[str, ...] = ('PNG',)) -> Image.Image:
    """Decode a package image. Colour textures may also be JPEG (`formats`); masks, heights and UI art stay PNG."""
    names = '/'.join(formats)
    try:
        image = Image.open(io.BytesIO(raw))
        image.load()
    except Exception as error:  # Pillow raises many types for bad input
        raise MapCompileError(f'{what} 不是有效的 {names}：{error}') from None
    if image.format not in formats:
        raise MapCompileError(f'{what} 必须是 {names}')
    return image


def _pow2(value: int) -> int:
    size = 1
    while size < value and size < TEXTURE_LIMIT:
        size *= 2
    return size


def _texture_txr(image: Image.Image) -> bytes:
    rgba = image.convert('RGBA')
    width, height = _pow2(rgba.width), _pow2(rgba.height)
    if (width, height) != rgba.size:
        rgba = rgba.resize((width, height), Image.LANCZOS)
    if width * height >= 256 * 256 and rgba.getchannel('A').getextrema() == (255, 255):
        # opaque: JPEG like the client's own opaque textures (several times smaller than PNG)
        return txr_format.build_jpeg_txr(width, height, rgba.tobytes())
    return txr_format.build_txr(width, height, rgba.tobytes())


def _layer_weights(coverage: list) -> list:
    """Layer masks are coverage (each layer is painted over the ones below it). The terrain renderer adds
    the layers weighted by their per-vertex alpha, and every shipped map keeps those weights summing to
    255 at each vertex; a larger sum brightens the ground past 1.0, which the HDR bloom then blows out.
    Returns per-layer weights (a constant 255 or bytes per vertex) that composite like the coverage stack."""
    count = len(coverage)
    if count == 1:
        return [255]
    vertices = GRID * GRID
    columns = [bytearray(vertices) for _ in range(count)]
    for v in range(vertices):
        remaining = 255
        for i in range(count - 1, 0, -1):
            amount = coverage[i][v] if coverage[i] is not None else 1.0
            weight = min(remaining, int(amount * remaining + 0.5))
            columns[i][v] = weight
            remaining -= weight
        columns[0][v] = remaining
    return [bytes(c) for c in columns]


def _grid_image(image: Image.Image, what: str) -> list[float]:
    """257x257 grayscale image -> row-major values in 0..1 (row 0 = north)."""
    if image.size != (GRID, GRID):
        raise MapCompileError(f'{what} 必须是 {GRID}x{GRID} 像素')
    if image.mode in ('I;16', 'I;16B', 'I;16L', 'I'):
        values = list(image.getdata())
        scale = 65535.0 if max(values, default=0) <= 65535 else float(max(values))
        return [v / scale for v in values]
    gray = image.convert('L')
    return [v / 255.0 for v in gray.getdata()]


def spr_bytes(texture: str, width: int, height: int, texture_size: tuple[int, int]) -> bytes:
    """176-byte SPR v10000 showing the top-left width x height pixels of `texture`."""
    tw, th = texture_size
    out = bytearray(176)
    struct.pack_into('<5I4f', out, 0, 10000, 1, 20, height, width, 0.0, 0.0, width / tw, height / th)
    encoded = texture.encode('utf-16-le')
    if len(encoded) >= 132:
        raise MapCompileError('SPR texture path too long')
    out[44:44 + len(encoded)] = encoded
    return bytes(out)


def _put_wstr(row: bytearray, offset: int, size: int, text: str, what: str) -> None:
    encoded = text.encode('utf-16-le')
    if len(encoded) > size - 2:
        raise MapCompileError(f'{what} 过长（最多 {size // 2 - 1} 个字符）')
    row[offset:offset + size] = encoded.ljust(size, b'\0')


# --------------------------------------------------------------------------- terrain
class _Terrain:
    def __init__(self, package: Package):
        self.p = package
        span = package.height_max - package.height_min
        values = _grid_image(_open(package.read(package.height_image), 'terrain.height.image'), 'terrain.height.image')
        self.heights = [package.height_min + v * span for v in values]
        self.cell = 2 * package.half_extent / (GRID - 1)

    def at(self, x: float, z: float) -> float:
        half = self.p.half_extent
        fc = min(max((x + half) / self.cell, 0.0), GRID - 1.000001)
        fr = min(max((half - z) / self.cell, 0.0), GRID - 1.000001)
        c, r = int(fc), int(fr)
        tc, tr = fc - c, fr - r
        h = self.heights
        a = h[r * GRID + c] + (h[r * GRID + c + 1] - h[r * GRID + c]) * tc
        b = h[(r + 1) * GRID + c] + (h[(r + 1) * GRID + c + 1] - h[(r + 1) * GRID + c]) * tc
        return a + (b - a) * tr

    def lightmap(self) -> tuple[int, int, bytes]:
        """Bake sun + ambient (+ terrain cast shadows) per height vertex, then resample to LIGHTMAP_SIZE."""
        p, h, cell = self.p, self.heights, self.cell
        sx, sy, sz = p.sun_direction
        length = math.sqrt(sx * sx + sy * sy + sz * sz)
        lx, ly, lz = -sx / length, -sy / length, -sz / length          # towards the sun
        horizontal = math.hypot(lx, lz)
        top = max(h)
        values = bytearray(GRID * GRID * 3)
        for r in range(GRID):
            for c in range(GRID):
                i = r * GRID + c
                hl = h[i - 1] if c > 0 else h[i]
                hr = h[i + 1] if c < GRID - 1 else h[i]
                hn = h[i - GRID] if r > 0 else h[i]                     # north neighbour (row - 1 = +Z)
                hs = h[i + GRID] if r < GRID - 1 else h[i]
                nx, ny, nz = (hl - hr) / (2 * cell), 1.0, (hs - hn) / (2 * cell)
                n = math.sqrt(nx * nx + ny * ny + nz * nz)
                lambert = max(0.0, (nx * lx + ny * ly + nz * lz) / n)
                shadow = 1.0
                if p.bake_shadows and lambert > 0 and horizontal > 1e-4:
                    x = -p.half_extent + c * cell
                    z = p.half_extent - r * cell
                    y = h[i] + 2.0
                    step = cell
                    rise = ly / horizontal * step
                    dx, dz = lx / horizontal * step, lz / horizontal * step
                    while True:
                        x += dx
                        z += dz
                        y += rise
                        if abs(x) >= p.half_extent or abs(z) >= p.half_extent or y > top:
                            break
                        if self.at(x, z) > y:
                            shadow = 0.35
                            break
                # shipped lightmaps span ~90..245: keep headroom so slopes and shadows stay visible
                light = [min(1.0, 0.7 * p.ambient[k] + 0.6 * p.sun_color[k] * lambert * shadow) for k in range(3)]
                # lightmap stores B, G, R
                values[i * 3:i * 3 + 3] = bytes((int(light[2] * 250), int(light[1] * 250), int(light[0] * 250)))
        image = Image.frombytes('RGB', (GRID, GRID), bytes(values))
        image = image.resize((LIGHTMAP_SIZE, LIGHTMAP_SIZE), Image.BICUBIC).filter(ImageFilter.GaussianBlur(1.2))
        return LIGHTMAP_SIZE, LIGHTMAP_SIZE, image.tobytes()


def _sky_textures(sky: dict) -> dict[str, bytes]:
    zenith, horizon, ground = sky['zenith'], sky['horizon'], sky['ground']

    def lerp(a, b, t):
        return tuple(int(255 * min(1.0, a[k] + (b[k] - a[k]) * t)) for k in range(3))
    side = Image.new('RGB', (256, 256))
    px = side.load()
    for y in range(256):
        if y < 128:
            colour = lerp(zenith, horizon, (y / 127.0) ** 1.8)
        else:
            colour = lerp(horizon, ground, min(1.0, (y - 128) / 48.0))
        for x in range(256):
            px[x, y] = colour
    top = Image.new('RGB', (64, 64), lerp(zenith, zenith, 0))
    bottom = Image.new('RGB', (64, 64), lerp(ground, ground, 0))
    return {'top': _texture_txr(top), 'side': _texture_txr(side), 'bottom': _texture_txr(bottom)}


# --------------------------------------------------------------------------- models
def _flat_normals(mesh: gltf.Mesh) -> list[tuple[float, float, float]]:
    normals = [[0.0, 0.0, 0.0] for _ in mesh.positions]
    for a, b, c in mesh.triangles:
        pa, pb, pc = mesh.positions[a], mesh.positions[b], mesh.positions[c]
        u = [pb[k] - pa[k] for k in range(3)]
        v = [pc[k] - pa[k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        for i in (a, b, c):
            for k in range(3):
                normals[i][k] += n[k]
    out = []
    for n in normals:
        length = math.sqrt(sum(x * x for x in n)) or 1.0
        out.append((n[0] / length, n[1] / length, n[2] / length))
    return out


class _ModelCompiler:
    def __init__(self, package: Package, prefix: str, resources: dict[str, bytes]):
        self.package = package
        self.prefix = prefix
        self.resources = resources
        self.textures: dict[str, str] = {}       # sha256 of source -> native path
        self.models: dict[tuple, str] = {}
        self.stats = {'models': 0, 'triangles': 0, 'textures': 0, 'collision_boxes': 0}

    def texture(self, source: bytes, what: str) -> str:
        digest = hashlib.sha256(source).hexdigest()
        if digest not in self.textures:
            name = f'txrs/{self.prefix}t{len(self.textures):02d}.txr'
            self.resources[name] = _texture_txr(_open(source, what, TEXTURE_FORMATS))
            self.textures[digest] = _native(name)
            self.stats['textures'] += 1
        return self.textures[digest]

    def solid(self, colour, what: str) -> str:
        image = Image.new('RGBA', (4, 4), tuple(max(0, min(255, int(c * 255))) for c in colour))
        return self.texture(_png(image), what)

    def model(self, relative: str, collision) -> str:
        key = (relative, repr(collision))
        if key in self.models:
            return self.models[key]
        try:
            meshes = gltf.load(self.package.read(relative), self.package.resolver(relative))
        except gltf.GltfError as error:
            raise MapCompileError(f'{relative}: {error}') from None
        if not meshes:
            raise MapCompileError(f'{relative} 没有可用的三角形网格')
        materials: list[mod_format.BuildMaterial] = []
        material_index: dict[str, int] = {}
        build: list[mod_format.BuildMesh] = []
        boxes: list[mod_format.OBB] = []
        for n, mesh in enumerate(meshes):
            if mesh.material.texture_png:
                texture = self.texture(mesh.material.texture_png, f'{relative} 贴图')
                diffuse = tuple(mesh.material.color)
            else:
                texture = self.solid(mesh.material.color, f'{relative} 材质颜色')
                diffuse = (1.0, 1.0, 1.0, 1.0)
            if texture not in material_index:
                material_index[texture] = len(materials)
                materials.append(mod_format.BuildMaterial(texture=texture, diffuse=diffuse))
            normals = mesh.normals if len(mesh.normals) == len(mesh.positions) else _flat_normals(mesh)
            uvs = mesh.uvs if len(mesh.uvs) == len(mesh.positions) else [(0.0, 0.0)] * len(mesh.positions)
            # glTF is right handed (-Z forward); the game is left handed: mirror Z and swap the winding
            positions = [(x, y, -z) for x, y, z in mesh.positions]
            build.append(mod_format.BuildMesh(
                name=f'mesh{n:02d}', positions=positions, normals=[(x, y, -z) for x, y, z in normals],
                uvs=uvs, faces=[(a, c, b) for a, b, c in mesh.triangles],
                face_materials=material_index[texture],
                flags=mod_format.FLAG_TWO_SIDED if mesh.material.double_sided else 0))
            self.stats['triangles'] += len(mesh.triangles)
            if collision == 'auto':
                lo = [min(p[k] for p in positions) for k in range(3)]
                hi = [max(p[k] for p in positions) for k in range(3)]
                if all(hi[k] - lo[k] > 1.0 for k in range(3)):
                    boxes.append(mod_format.OBB.from_aabb(lo, hi))
        if isinstance(collision, list):
            for box in collision:
                angle = math.radians(box['rotation_y'])
                axes = ((math.cos(angle), 0.0, -math.sin(angle)), (0.0, 1.0, 0.0), (math.sin(angle), 0.0, math.cos(angle)))
                boxes.append(mod_format.OBB.from_axes(box['center'], axes, [2 * v for v in box['half_extents']]))
        try:
            raw = mod_format.build_static_mod(build, materials, boxes)
        except mod_format.ModFormatError as error:
            raise MapCompileError(f'{relative}: {error}') from None
        name = f'mdrs/{self.prefix}o{len(self.models):02d}.mod'
        self.resources[name] = raw
        self.models[key] = _native(name)
        self.stats['models'] += 1
        self.stats['collision_boxes'] += len(boxes)
        return self.models[key]


# --------------------------------------------------------------------------- map row and server rows
def map_row(package: Package, map_no: int, variant: str, trn: str, preview: str, radar: str, bgm: str) -> bytes:
    row = bytearray(MAP_ROW_BYTES)
    struct.pack_into('<I', row, 0, MAP_ROW_FIRST_ID + map_no)
    struct.pack_into('<I', row, 5, map_no)
    _put_wstr(row, 0x09, 0x64, package.name['zh'], 'name.zh')
    _put_wstr(row, 0x6D, 0x64, preview, 'preview path')
    _put_wstr(row, 0xD1, 0x64, trn, 'terrain path')
    _put_wstr(row, 0x135, 0x64, bgm, 'BGM path')
    for i, column in enumerate(FLAG_COLUMNS):
        row[0x1FD + i] = flags(package, variant)[column]
    _put_wstr(row, 0x20B, 0xC8, package.description['zh'], 'description.zh')
    # The float following the description is the scene's bright-pass threshold.
    # Zero sends the entire scene into Bloom and can wash the map out to white.
    struct.pack_into('<f', row, 0x2D3, 0.85)
    _put_wstr(row, 0x2D7, 0x64, radar, 'radar path')
    struct.pack_into('<4i', row, 0x33B, 0, 0, RADAR_SIZE, RADAR_SIZE)
    row[0x34B] = 1
    return bytes(row)


def flags(package: Package, variant: str) -> dict[str, int]:
    values = {column: 0 for column in FLAG_COLUMNS}
    if variant == 'mission':
        values['MissionMode'] = 1
    else:
        for mode in package.modes:
            values[MODE_COLUMNS[mode]] = 1
    for server in package.servers:
        values[SERVER_COLUMNS[server]] = 1
    return values


# --------------------------------------------------------------------------- entry point
def compile_map(package: Package, *, map_no: int, map_id: int, variant: str = 'pvp', prefix: str | None = None,
                native_exists: Callable[[str], bool] | None = None, shared: dict | None = None) -> CompiledMap:
    """Compile one map row variant. `shared` (a dict) lets a second variant reuse models/textures/images."""
    if variant not in ('pvp', 'mission'):
        raise MapCompileError('variant 只能是 pvp 或 mission')
    if not FIRST_IMPORTED_MAP_NO <= map_no <= MAP_NO_LIMIT:
        raise MapCompileError(f'MapNo 必须在 {FIRST_IMPORTED_MAP_NO}..{MAP_NO_LIMIT}')
    if not 1 <= map_id <= 65535:
        raise MapCompileError('MapID 必须在 1..65535')
    if variant == 'pvp' and not package.modes:
        raise MapCompileError('对战地图至少需要一个 modes')
    prefix = prefix or f'm{map_no}'
    shared = shared if shared is not None else {}
    out = CompiledMap(map_no=map_no, map_id=map_id, variant=variant, prefix=prefix)
    resources = shared.setdefault('resources', {})
    terrain = shared.get('terrain') or shared.setdefault('terrain', _Terrain(package))
    models = shared.get('models') or shared.setdefault('models', _ModelCompiler(package, prefix, resources))

    # -- terrain layers, sky, lightmap (shared between variants)
    if 'layers' not in shared:
        coverage = []
        textures = []
        for i, layer in enumerate(package.layers):
            textures.append(models.texture(package.read(layer.texture), f'terrain.layers[{i}].texture'))
            coverage.append(_grid_image(_open(package.read(layer.mask), f'terrain.layers[{i}].mask'),
                                        f'terrain.layers[{i}].mask') if layer.mask else None)
        weights = _layer_weights(coverage)
        shared['layers'] = [(texture, alpha, 2 * package.half_extent / layer.tiling)
                            for texture, alpha, layer in zip(textures, weights, package.layers)]
        sky = _sky_textures(package.sky)
        for part, raw in sky.items():
            resources[f'txrs/{prefix}s{part[0]}.txr'] = raw
        shared['sky'] = [_native(f'txrs/{prefix}st.txr'), _native(f'txrs/{prefix}sb.txr')] + \
            [_native(f'txrs/{prefix}ss.txr')] * 4
        shared['lightmap'] = terrain.lightmap()

    builder = trn_format.TrnBuilder(scale=package.half_extent * 2 / (GRID - 1))
    builder.set_heights(terrain.heights)
    for texture, alpha, tile in shared['layers']:
        builder.add_layer(texture, alpha, tile_size=tile)
    w, h, pixels = shared['lightmap']
    builder.set_lightmap(w, h, pixels)
    builder.set_play_area(package.play_half_x / package.half_extent, package.play_half_z / package.half_extent,
                          height=package.ceiling, show=True)
    t = builder.trn
    view = max(package.fog_end + 1000.0, 9000.0)
    t.props.distance_levels = [(view, package.fog_end, package.fog_start)] * trn_format.DISTANCE_LEVELS
    t.props.skybox_render = 1
    t.skybox = [trn_format.wstr(name) for name in shared['sky']]
    t.light_ambient = _d3dcolor(package.ambient)
    t.light_diffuse = _d3dcolor(package.sun_color)
    t.light_specular = 0xFFFFFFFF
    sx, sy, sz = package.sun_direction
    length = math.sqrt(sx * sx + sy * sy + sz * sz)
    t.light_vector = (-sx / length, -sy / length, -sz / length)
    t.fog_color = _d3dcolor(package.fog_color)

    # -- objects
    placed = 0
    for i, obj in enumerate(package.objects):
        if obj.only and obj.only != variant:
            continue
        if obj.native_model:
            if native_exists is not None and not native_exists(obj.native_model):
                raise MapCompileError(f'objects[{i}] 引用的原生模型在目标客户端中不存在：{obj.native_model}')
            path = obj.native_model
        else:
            path = models.model(obj.model, obj.collision)
        if len(path) > NATIVE_PATH_CHARS:
            raise MapCompileError(f'objects[{i}] 模型路径超过 {NATIVE_PATH_CHARS} 个字符')
        angle = math.radians(obj.rotation_y)
        builder.add_static_block(path, obj.position, (math.sin(angle), 0.0, math.cos(angle)), (0.0, 1.0, 0.0),
                                 obj.scale)
        placed += 1

    # -- spawns (colour 1 = team A/red, 2 = team B/blue, 0 = shared, 3/4 = Grid respawns)
    def spawn(colour: int, index: int, s, what: str):
        ground = terrain.at(s.position[0], s.position[2])
        if s.position[1] < ground + 20:
            raise MapCompileError(f'{what} 低于地面（地面高度 {ground:.0f}）')
        builder.add_start_point(colour, index, s.position, (s.facing[0], 0.0, s.facing[1]))
    for colour, key in ((1, 'team_a'), (2, 'team_b'), (0, 'shared'), (3, 'grid_respawn_a'), (4, 'grid_respawn_b')):
        for index, s in enumerate(package.spawns[key]):
            spawn(colour, index, s, f'spawns.{key}[{index}]')

    # -- item patterns (item point type byte = its index, as in every shipped map)
    item_rows = []
    for team_id, team in ((1, 'red'), (2, 'blue')):
        for pattern_id, pattern in enumerate(package.item_patterns[team]):
            indices = []
            for order, position in enumerate(pattern):
                index = len(builder.trn.item_points)
                if index > 255:
                    raise MapCompileError('道具点超过 256 个')
                builder.add_item_point(index, position)
                indices.append(index)
                item_rows.append({'MapID': map_id, 'Team': team_id, 'PatternID': pattern_id, 'Orders': order,
                                  'Idx': index, 'PosX': position[0], 'PosY': position[1], 'PosZ': position[2]})
            builder.add_item_pattern(team, indices)

    trn_name = f'trns/{prefix}{"" if variant == "pvp" else "x"}.trn'
    resources[trn_name] = builder.to_bytes()

    # -- presentation (shared)
    if 'presentation' not in shared:
        preview = _open(package.read(package.preview), 'presentation.preview').convert('RGB').resize(PREVIEW_SIZE, Image.LANCZOS)
        canvas = Image.new('RGB', (128, 128), (0, 0, 0))
        canvas.paste(preview, (0, 0))
        resources[f'txrs/{prefix}p.txr'] = txr_format.build_txr(128, 128, canvas.convert('RGBA').tobytes())
        resources[f'sprs/{prefix}p.spr'] = spr_bytes(_native(f'txrs/{prefix}p.txr'), PREVIEW_SIZE[0], PREVIEW_SIZE[1], (128, 128))
        minimap = _open(package.read(package.minimap), 'presentation.minimap').convert('RGB')
        fx = package.play_half_x / package.half_extent
        fz = package.play_half_z / package.half_extent
        box = (minimap.width * (1 - fx) / 2, minimap.height * (1 - fz) / 2,
               minimap.width * (1 + fx) / 2, minimap.height * (1 + fz) / 2)
        radar = minimap.crop(tuple(int(round(v)) for v in box)).resize((RADAR_SIZE, RADAR_SIZE), Image.LANCZOS)
        resources[f'txrs/{prefix}r.txr'] = txr_format.build_txr(RADAR_SIZE, RADAR_SIZE, radar.convert('RGBA').tobytes())
        if package.bgm_file:
            shared['bgm'] = f'BGM\\{prefix}.mp3'
            shared['loose'] = {f'BGM/{prefix}.mp3': package.read(package.bgm_file)}
        else:
            if native_exists is not None and not native_exists(package.bgm_native):
                raise MapCompileError(f'目标客户端中不存在 BGM：{package.bgm_native}')
            shared['bgm'] = package.bgm_native
            shared['loose'] = {}
        shared['presentation'] = True
    out.loose = dict(shared['loose'])
    out.rows.append((MAP_KIND, MAP_ROW_FIRST_ID + map_no,
                     map_row(package, map_no, variant, _native(trn_name), _native(f'sprs/{prefix}p.spr'),
                             _native(f'txrs/{prefix}r.txr'), shared['bgm'])))

    for name in resources:
        if not name.split('/', 1)[1].startswith(prefix):
            raise MapCompileError(f'资源名不在本图前缀下：{name}')
        if native_exists is not None and native_exists(_native(name)):
            raise MapCompileError(f'资源名与目标客户端已有资源冲突：{name}')
    out.resources = resources

    half = int(package.half_extent)
    flag_values = flags(package, variant)
    out.server = {
        'tbl_MapFile': [{'MapNo': map_no, 'MapName': (package.name['en'] or package.map_key)[:100], 'MapID': map_id,
                         **flag_values, 'Disable': 0}],
        'tbl_MapData': [{'Version': 0, 'MapID': map_id, 'MinX': -half, 'MinZ': half, 'MaxX': half, 'MaxZ': -half,
                         'BlockCnt': 0, 'NumItemPatternRed': len(package.item_patterns['red']),
                         'NumItemPatternBlue': len(package.item_patterns['blue'])}],
        'tbl_MapItemPath': item_rows,
    }
    out.report = {'map_key': package.map_key, 'map_no': map_no, 'map_id': map_id, 'variant': variant,
                  'objects': placed, 'spawns': {k: len(v) for k, v in package.spawns.items()},
                  'item_patterns': {k: len(v) for k, v in package.item_patterns.items()},
                  'trn': _native(trn_name), 'trn_bytes': len(resources[trn_name]), **models.stats}
    return out


def verify_trn(raw: bytes) -> trn_format.TrnFile:
    """Parse a compiled .trn back and require a byte-identical re-serialisation."""
    parsed = trn_format.parse(raw)
    if trn_format.serialize(parsed) != raw:
        raise MapCompileError('compiled .trn does not round-trip')
    return parsed


__all__ = ['CompiledMap', 'MapCompileError', 'MapPackageError', 'compile_map', 'map_row', 'flags', 'spr_bytes',
           'verify_trn', 'FIRST_IMPORTED_MAP_NO', 'MAP_ROW_FIRST_ID', 'MAP_KIND']
