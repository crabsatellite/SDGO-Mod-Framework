"""Byte-exact reader/writer and new-map builder for ZOA/SDGO terrain map files (`Trns\\NNNN.trn`).

A `.trn` is read sequentially with no offset table by the battle map loader of GOnline.exe
(0x004A8B30). It is one stream with four consecutive parts, all little endian:

    1. CTerrain::LoadBinary        GOnline 0x00C069E0 (static copy of zoa_graphics 0x1006F4E0)
    2. scene globals               skybox, sun light, fog colour (read by 0x004A8B30 itself)
    3. CGameDocument::LoadObjectInfo   GOnline 0x004A27E0: blocks, effects, water, lights, start and
                                   item points, decals, force-move pads, death zones
    4. scene tail                  environment cube map, lens flares, observer cameras, object lightmaps

Every optional field is gated by the u16 `version` at offset 0 (the same value is used by all four parts,
the object part reads it back from CTerrain+4). Shipped files use versions 23, 24, 27, 29..32, 34, 35 and
38..41; 40 is the most common. This codec implements every gate of the loader for version >= 20 (older
CTerrain/scene gates are implemented too; versions 14..19 carry a legacy single lens flare that is not
decoded and are rejected). `vN+` below means "present when version >= N".

Coordinate system: DirectX left handed, Y up. With W = sections_x * tiles_x and H = sections_z * tiles_z
(256 x 256 in every shipped file) the height grid has (W+1) x (H+1) vertices stored row major; vertex
(col, row) is at world x = (col - W/2) * scale_x, z = (H/2 - row) * scale_z, y = height (world units, not
multiplied by scale_y). Row 0 is the +Z (north) edge. The terrain is always centred on the origin
(CTerrain bounds = +-W*scale_x/2, +-H*scale_z/2); tbl_MapData Min/Max X/Z of the server equal these bounds.

Part 1: CTerrain
----------------
    u16  version
    u32  sections_x, sections_z           CTerrain+0x2C/+0x30 (16, 16)
    u32  tiles_x, tiles_z                 tiles (quads) per section, CTerrain+0x34/+0x38 (16, 16)
    f32  scale[3]                         world units per tile on x / y / z (64; 128 and 32 also shipped)
    f32  heights[(W+1)*(H+1)]             world Y per vertex
    f32  normals[(W+1)*(H+1)][3]          unit vertex normals (engine averages face normals)
    f32  uv_origin[2]                     CTerrain+0x498C. Detail texture coordinate of vertex (col,row) =
                                          (col*scale_x + uv_origin[0], row*scale_z + uv_origin[1]) before
                                          the layer uv_transform. Normally (-W*scale_x/2, -H*scale_z/2).
    u32  layer_count                      CTerrain::LoadLayerData (0x00C060D0); 1..5 shipped
    layer[layer_count]:
        wchar[32] texture                 `Txrs\\NNNN.txr` (DXT texture loaded with CTexture::Load); empty =
                                          layer slot without texture. Bytes after the NUL are preserved.
        f32  uv_transform[16]             CTerrain+0x98C+i*0x40, texture matrix applied to uv above. Shipped:
                                          [s,0,0,0, 0,s,0,0, 0,0,4,0, 2s,2s,524272,1] with s = 1/tile_size
                                          (tile_size 256 or 320 world units).
        f32  specular[4], f32 power       (v3+) D3DMATERIAL9 specular RGBA and power of the layer material
                                          (CTerrain+0x65BC+i*0x6C); shipped default (0,0,0,0) / 15.
        u8   alpha[(257*257)]             (v6+) per-vertex blend weight 0..255, same grid as heights.
                                          (v<6: 256*256 bytes; the loader duplicates the last row/column.)
    u32  lightmap_w, lightmap_h           (v6+; older versions imply 256 x 256) 256, 512 or 1024
    u8   lightmap[w*h][3]                 B, G, R bytes per pixel (copied into an A8R8G8B8 texture);
                                          row 0 = +Z edge, column 0 = -X edge; covers the whole terrain.
    section[sections_z][sections_x]:      CTerrainSection::Init descriptor, 0x40 bytes:
        i32  layers[16]                   indices of the layers drawn on this section in drawing order,
                                          terminated by -1; entries after the -1 are ignored garbage.
    u32  attributes[aw*ah]                (v2+) CTerrain::GetAttrTable, aw = int(W*scale_x/32),
                                          ah = int(H*scale_z/32): one bit-flag word per 32x32 world-unit
                                          cell, same orientation as the height grid. Most cells are 0;
                                          shipped bits 0x1..0x20000000 (meanings not traced). Safe default 0.
    TerrainProp (CTerrain+0x6C, names from the zoa_graphics property table), in file order:
        f32 lod[3]                        (v4+) LOD switch distances (1000, 3000, 5000)
        f32 play_area_x                   (v5+) "Play Area X": fraction of the terrain half extent; the
                                          playable rectangle is x in +-(W*scale_x/2)*play_area_x (GOnline
                                          0x00657296 scales CTerrain bounds by it). Shipped 0.15..0.9.
        f32 play_area_z                   (v25+) "Play Area Z", same for z (older: = play_area_x)
        u8  show_play_area                (v31+) draw the play-area border
        f32 play_height                   (v15+) "Play Height" (1000..4000, 100000 = unlimited): flight
                                          ceiling; the client places a marker object at play_height + 500
        f32 legacy_view_distance          (v17..27) copied into all five distance levels
        f32 distance_levels[5][3]         (v28+) per graphics-quality level: view distance (far clip),
                                          fog far (end), fog near (start); fog uses fog_color below
        u8  skybox_render                 (v18+)
        u32 skybox_color                  (v25+) D3DCOLOR (0 in most maps)
        u8  show_dropship                 (v22+)
        u8  terrain_render                (v25+) 1 = draw the height-map terrain. 0 = hidden (87 of 125
                                          v39+ maps: flat terrain, the ground is made of static models)
        u8  terrain_kill                  (v26+) 1 = the (hidden) terrain is a kill floor
        wchar[32] cloud_texture           (v13+) cloud shadow texture, usually empty
        f32 cloud_tile[2], cloud_scroll[2](v13+) cloud texture scale and "Movemant" scroll speed
        u32 n, item_pattern_red[n][8]     (v21+) "ItemPattern Red": each pattern = 8 x i32 indices into
        u32 n, item_pattern_blue[n][8]          the item point list (server tbl_MapItemPath mirrors them)
        wchar[32] weather_effect          (v23+) `Fxrs\\NNNN.zfx` attached to the camera, may be empty
        u8  space_map                     (v33+) "Space Map"
        u8  water_flag                    (v27+) (meaning not traced)
        u32 water_fog_color               (v26+) "WaterFog Color"
        f32 water_fog_far, water_fog_near (v26+)
        wchar[32] water_effect            (v26+) "Water Effect" `Fxrs\\...zfx`, may be empty
        u8  sniping_culling               (v32+) "SnipingCulling"
        f32 sniping_distance              (v32+) "Distance"

Part 2: scene globals (GOnline 0x004A8B30)
-------------------------------------------
    wchar[32] skybox[6]                   CSkybox face textures (0x00C123E0), may be empty
    u32 light_ambient                     sun (D3DLIGHT_DIRECTIONAL) ambient D3DCOLOR
    u32 light_diffuse                     sun diffuse D3DCOLOR
    u32 light_specular                    (v10+; older 0xFFFFFFFF)
    f32 light_vector[3]                   sun direction = -normalize(light_vector)
    u32 fog_color                         D3DCOLOR passed to the renderer fog
    f32 legacy_fog_near, legacy_fog_far   (v<28 only) written into every distance level

Part 3: CGameDocument::LoadObjectInfo (0x004A27E0)
---------------------------------------------------
A transform is f32 position[3], forward[3], up[3], scale[3] (the engine orientation struct, type 3).
Common render flags map to CSceneObject fields: always_render -> +0x114 (no distance culling),
back_alpha -> +0xF6, near_alpha -> +0xF5 (camera-near fade); receive_shadow projects the terrain lightmap
onto the model (CModel+0x31C); low_detail_skip = the object is not created when the low-detail option is
set; lightmap_flag/lightmap_data = per-object baked lightmap coordinates (only loaded when the client's
lightmap option is on, otherwise skipped) referencing the textures of part 4.

    u32 n, static_block[n]                engine object kind 0x11, the static map props:
        wchar[32] model                   `mdrs\\NNNN.mod` (collision comes from the model OBB section)
        transform
        u8  receive_shadow
        u8  always_render                 (v5+)
        u8  back_alpha                    (v9+)
        u8  near_alpha                    (v19+)
        u8  low_detail_skip               (v36+)
        f32 path_speed                    (v39+) movement speed along the path (0.03 when unused)
        u32 m, f32 path[m][3]             (v39+) waypoints; non-empty = moving prop
        u8  lightmap_flag                 (v40+); if non-zero: u32 size, u8 lightmap_data[size]
    u32 n, destroyable_block[n]           (v11+) engine kind 0x12, breakable props/gates:
        u32 object_id                     object definition (model, HP: client object table + server
                                          tbl_MapObjects / tbl_MapBlocks)
        wchar[64] name                    (v24+) instance name, e.g. `NI_b04_21` (not a path)
        transform
        u8  receive_shadow, always_render, back_alpha
        u8  near_alpha                    (v19+)
        f32 path_speed, u32 m, f32 path[m][3]   (v39+)
        f32 unused[3]                     (v16+) read but not used (zero in shipped files)
        u8  lightmap_flag (+ u32 size, data)    (v40+)
    u32 n, trigger_block[n]               (v41+) engine kind 0x13 (logged as "Trigger Num"):
        u32 object_id, wchar[32] name (v24+), transform, u8 receive_shadow, always_render, back_alpha,
        u8 near_alpha (v19+), u8 low_detail_skip (v36+), path (v39+), lightmap (v40+)
    u32 n, effect[n]                      engine kind 3, placed particle effects (created only when the
                                          effect quality option is > 3):
        wchar[32] effect                  `Fxrs\\NNNN.zfx`
        transform
        u8  always_render                 (v8+)
        u8  low_detail_skip               (v37+)
        u8  back_alpha                    (v38+)
    u32 n, water[n]                       CWater planes (0x00C2DFC0):
        u32 t, wchar[32] textures[t]      animated surface frames
        f32 wrap_u, wrap_v                texture repeats (4, 4)
        f32 param_c                       (0.3; meaning not traced, CWater+0x180)
        f32 param_d                       (CWater+0x12C...; meaning not traced)
        transform                         only scale x/z are used (plane size); scale y ignored
    u32 n, D3DLIGHT9 dynamic_light[n]     0x68 bytes each: type, diffuse/specular/ambient RGBA,
                                          position, direction, range, falloff, attenuation0..2, theta, phi
    u32 n, start_point[n]                 0x5A bytes each:
        wchar[32] name                    `StartPoint`, `StartPoint_1`, ...
        u8  colour                        1 / 2 = team spawns (index 0..5 = player slot of that team),
                                          0 = shared relocation points (server-chosen index), 3 / 4 = Grid
                                          mode respawn points
        u8  index
        f32 position[3]                   world position of the spawn (unit is dropped from there)
        f32 direction[3]                  facing, XZ unit vector
    u32 n, item_point[n]                  (v7+) 0x59 bytes each: wchar[32] name, u8 item_type,
                                          f32 position[3], f32 direction[3]
    u32 n, decal[n]                       (v9+) CTerrainDecal (0x00C2AFC0) + transform:
        wchar[32] texture, u8 alpha, u8 receive_shadow, u8 u_wrap, u8 v_wrap,
        f32 tex_scale[2], f32 tex_offset[2], f32 tex_rotation (degrees), transform (scale = size)
    u32 n, force_move_pad[n]              (v29+) 0x1334-byte engine object (kind 0x21): transform
                                          (forward = push direction, scale = trigger box), f32 speed,
                                          f32 duration (updates)
    u32 n, death_zone[n]                  (v30+) 0x132C-byte engine object (kind 0x22): transform
                                          (scale = box size)

Part 4: scene tail (GOnline 0x004A8B30)
----------------------------------------
    u32 size, u8 env_cubemap[size]        (v12+) DDS file (256 DXT1 cube) for CRenderer::LoadCubemapFromMemory
    u32 n, lens_flare[n]                  (v20+) CLensFlare (0x00C10B70):
        f32 position[3], f32 min_distance, f32 max_distance, i32 fade_frames, u8 always_render,
        u32 e, element[e]: wchar[32] texture, f32 location, f32 size, u32 colour,
        u8 background (v35+), u8 hide_low_detail (v37+)
    u32 n, observer_point[n]              (v34+) CTerrain::GetObserverPoint, 0x39 bytes: u8 mode,
                                          f32 far, near, fov (radians), param4, param5, f32 position[3],
                                          direction[3], up[3] (spectator camera; arrow keys move it)
    u32 n, lightmap_texture[n]            (v40+) u32 size + DDS file each; atlases referenced by the
                                          per-object lightmap_data (loaded only with the lightmap option)

Anything after the last section is ignored by the client; it is kept in `TrnFile.trailing`.
"""
from __future__ import annotations

from array import array
import copy
import math
import struct
import sys
from dataclasses import dataclass, field
from typing import Optional, Sequence


GRID_SECTIONS = 16
GRID_TILES = 16
GRID_VERTICES = GRID_SECTIONS * GRID_TILES + 1      # 257
DEFAULT_VERSION = 40
MIN_VERSION = 20
MAX_VERSION = 41
ATTRIBUTE_CELL = 32.0                                # world units per attribute cell (x 0.03125)
NAME_BYTES = 0x40                                    # wchar[32]
LONG_NAME_BYTES = 0x80                               # wchar[64]
SKYBOX_FACES = 6
DISTANCE_LEVELS = 5
ITEM_PATTERN_SLOTS = 8
SECTION_LAYER_SLOTS = 16
MAX_LAYERS = 15                                      # 16 section slots, one reserved for -1

_LE = sys.byteorder == 'little'

Vec3 = tuple


# ---------------------------------------------------------------------------------------------
# fixed-size UTF-16 strings
# ---------------------------------------------------------------------------------------------

def wstr(text: str, size: int = NAME_BYTES) -> bytes:
    """Encode `text` as a NUL-padded little endian wchar buffer of `size` bytes (raises if too long)."""
    if text is None:
        text = ''
    raw = str(text).encode('utf-16-le')
    if len(raw) + 2 > size:
        raise ValueError('%r does not fit a wchar[%d] field' % (text, size // 2))
    return raw + bytes(size - len(raw))


def wtext(raw: bytes) -> str:
    """Decode a fixed wchar buffer up to its first NUL (bytes after the NUL are ignored)."""
    for i in range(0, len(raw) - 1, 2):
        if raw[i] == 0 and raw[i + 1] == 0:
            raw = raw[:i]
            break
    return raw.decode('utf-16-le', 'replace')


def _farray(raw: bytes, typecode: str = 'f') -> array:
    a = array(typecode)
    a.frombytes(raw)
    if not _LE:
        a.byteswap()
    return a


def _abytes(a: array) -> bytes:
    if _LE:
        return a.tobytes()
    b = array(a.typecode, a)
    b.byteswap()
    return b.tobytes()


# ---------------------------------------------------------------------------------------------
# records
# ---------------------------------------------------------------------------------------------

@dataclass
class Transform:
    """Engine orientation: position, forward (look) vector, up vector, scale."""
    position: Vec3 = (0.0, 0.0, 0.0)
    forward: Vec3 = (0.0, 0.0, 1.0)
    up: Vec3 = (0.0, 1.0, 0.0)
    scale: Vec3 = (1.0, 1.0, 1.0)


@dataclass
class TerrainLayer:
    texture: bytes = field(default_factory=lambda: wstr(''))   # wchar[32] raw
    uv_transform: tuple = ()                                    # 16 f32
    specular: Optional[tuple] = (0.0, 0.0, 0.0, 0.0)            # v3+
    specular_power: Optional[float] = 15.0                      # v3+
    alpha: bytes = b''                                          # 257*257 (v6+) or 256*256

    @property
    def texture_name(self) -> str:
        return wtext(self.texture)


@dataclass
class TerrainProps:
    """CTerrain TerrainProp block (CTerrain+0x6C). None = absent in this version."""
    lod: Optional[tuple] = (1000.0, 3000.0, 5000.0)             # v4+
    play_area_x: Optional[float] = 0.3                          # v5+ fraction of the half extent
    play_area_z: Optional[float] = 0.3                          # v25+
    show_play_area: Optional[int] = 1                           # v31+
    play_height: Optional[float] = 1500.0                       # v15+
    legacy_view_distance: Optional[float] = None                # v17..27
    distance_levels: Optional[list] = None                      # v28+: 5 x (view, fog_far, fog_near)
    skybox_render: Optional[int] = 1                            # v18+
    skybox_color: Optional[int] = 0                             # v25+
    show_dropship: Optional[int] = 1                            # v22+
    terrain_render: Optional[int] = 1                           # v25+ (a height-map map must draw it)
    terrain_kill: Optional[int] = 0                             # v26+
    cloud_texture: Optional[bytes] = None                       # v13+ wchar[32]
    cloud_tile: Optional[tuple] = (1.0, 1.0)                    # v13+
    cloud_scroll: Optional[tuple] = (1.0, 1.0)                  # v13+
    item_patterns_red: Optional[list] = None                    # v21+: list of 8-int tuples
    item_patterns_blue: Optional[list] = None                   # v21+
    weather_effect: Optional[bytes] = None                      # v23+ wchar[32]
    space_map: Optional[int] = 0                                # v33+
    water_flag: Optional[int] = 0                               # v27+
    water_fog_color: Optional[int] = 0xFF0000FF                 # v26+
    water_fog_far: Optional[float] = 3500.0                     # v26+
    water_fog_near: Optional[float] = 1000.0                    # v26+
    water_effect: Optional[bytes] = None                        # v26+ wchar[32]
    sniping_culling: Optional[int] = 0                          # v32+
    sniping_distance: Optional[float] = 1500.0                  # v32+


@dataclass
class ObjectLightmap:
    """Per-object baked lightmap payload (v40+). flag==0 means none."""
    flag: int = 0
    data: bytes = b''


@dataclass
class StaticBlock:
    model: bytes = field(default_factory=lambda: wstr(''))      # wchar[32] `mdrs\\NNNN.mod`
    transform: Transform = field(default_factory=Transform)
    receive_shadow: int = 1
    always_render: int = 0                                      # v5+
    back_alpha: int = 0                                         # v9+
    near_alpha: int = 0                                         # v19+
    low_detail_skip: int = 0                                    # v36+
    path_speed: float = 0.03                                    # v39+
    path: list = field(default_factory=list)                    # v39+: list of Vec3
    lightmap: ObjectLightmap = field(default_factory=ObjectLightmap)  # v40+

    @property
    def model_name(self) -> str:
        return wtext(self.model)


@dataclass
class DestroyableBlock:
    object_id: int = 0
    name: bytes = field(default_factory=lambda: wstr('', LONG_NAME_BYTES))  # v24+ wchar[64]
    transform: Transform = field(default_factory=Transform)
    receive_shadow: int = 0
    always_render: int = 0
    back_alpha: int = 0
    near_alpha: int = 0                                         # v19+
    path_speed: float = 0.03                                    # v39+
    path: list = field(default_factory=list)                    # v39+
    unused: Vec3 = (0.0, 0.0, 0.0)                              # v16+
    lightmap: ObjectLightmap = field(default_factory=ObjectLightmap)  # v40+


@dataclass
class TriggerBlock:
    object_id: int = 0
    name: bytes = field(default_factory=lambda: wstr(''))       # v24+ wchar[32]
    transform: Transform = field(default_factory=Transform)
    receive_shadow: int = 0
    always_render: int = 0
    back_alpha: int = 0
    near_alpha: int = 0                                         # v19+
    low_detail_skip: int = 0                                    # v36+
    path_speed: float = 0.03                                    # v39+
    path: list = field(default_factory=list)                    # v39+
    lightmap: ObjectLightmap = field(default_factory=ObjectLightmap)  # v40+


@dataclass
class EffectInstance:
    effect: bytes = field(default_factory=lambda: wstr(''))     # wchar[32] `Fxrs\\NNNN.zfx`
    transform: Transform = field(default_factory=Transform)
    always_render: int = 0                                      # v8+
    low_detail_skip: int = 0                                    # v37+
    back_alpha: int = 0                                         # v38+


@dataclass
class WaterPlane:
    textures: list = field(default_factory=list)                # list of wchar[32] raw
    wrap_u: float = 4.0
    wrap_v: float = 4.0
    param_c: float = 0.3
    param_d: float = 0.0
    transform: Transform = field(default_factory=Transform)


@dataclass
class DynamicLight:
    """D3DLIGHT9 (0x68 bytes)."""
    type: int = 1                                               # 1 point, 2 spot, 3 directional
    diffuse: tuple = (1.0, 1.0, 1.0, 1.0)
    specular: tuple = (1.0, 1.0, 1.0, 1.0)
    ambient: tuple = (0.0, 0.0, 0.0, 1.0)
    position: Vec3 = (0.0, 0.0, 0.0)
    direction: Vec3 = (0.0, 0.0, 1.0)
    range: float = 400.0
    falloff: float = 1.0
    attenuation0: float = 0.3
    attenuation1: float = 0.0
    attenuation2: float = 1e-05
    theta: float = 0.5235988
    phi: float = 1.0471976


@dataclass
class StartPoint:
    name: bytes = field(default_factory=lambda: wstr('StartPoint'))
    colour: int = 1
    index: int = 0
    position: Vec3 = (0.0, 0.0, 0.0)
    direction: Vec3 = (0.0, 0.0, 1.0)

    @property
    def name_text(self) -> str:
        return wtext(self.name)


@dataclass
class ItemPoint:
    name: bytes = field(default_factory=lambda: wstr('ItemPoint'))
    item_type: int = 0
    position: Vec3 = (0.0, 0.0, 0.0)
    direction: Vec3 = (0.0, 0.0, 1.0)


@dataclass
class Decal:
    texture: bytes = field(default_factory=lambda: wstr(''))
    alpha: int = 255
    receive_shadow: int = 1
    u_wrap: int = 0
    v_wrap: int = 0
    tex_scale: tuple = (1.0, 1.0)
    tex_offset: tuple = (0.0, 0.0)
    tex_rotation: float = 0.0
    transform: Transform = field(default_factory=Transform)


@dataclass
class ForceMovePad:
    transform: Transform = field(default_factory=Transform)
    speed: float = 60.0
    duration: float = 40.0


@dataclass
class DeathZone:
    transform: Transform = field(default_factory=Transform)


@dataclass
class LensFlareElement:
    texture: bytes = field(default_factory=lambda: wstr(''))
    location: float = 1.0
    size: float = 1.0
    colour: int = 0xFFFFFFFF


@dataclass
class LensFlare:
    position: Vec3 = (0.0, 2000.0, 0.0)
    min_distance: float = 500.0
    max_distance: float = 2000.0
    fade_frames: int = 30
    always_render: int = 1
    elements: list = field(default_factory=list)
    background: int = 0                                         # v35+
    hide_low_detail: int = 0                                    # v37+


@dataclass
class ObserverPoint:
    mode: int = 0
    far: float = 20000.0
    near: float = 100.0
    fov: float = math.pi / 4
    param4: float = 7.0
    param5: float = 10.0
    position: Vec3 = (0.0, 3000.0, 0.0)
    direction: Vec3 = (0.0, -1.0, 0.0)
    up: Vec3 = (0.0, 0.0, 1.0)


@dataclass
class TrnFile:
    """Complete decoded `.trn`. Bulk grids are `array` objects (exact bit round trip).

    heights: array('f') (W+1)*(H+1); normals: array('f') 3*(W+1)*(H+1); attributes: array('I') aw*ah;
    lightmap: bytes BGR w*h*3; sections: list of 16-int tuples (sections_z rows of sections_x).
    """
    version: int = DEFAULT_VERSION
    sections_x: int = GRID_SECTIONS
    sections_z: int = GRID_SECTIONS
    tiles_x: int = GRID_TILES
    tiles_z: int = GRID_TILES
    scale: tuple = (64.0, 64.0, 64.0)
    heights: array = field(default_factory=lambda: array('f'))
    normals: array = field(default_factory=lambda: array('f'))
    uv_origin: tuple = (-8192.0, -8192.0)
    layers: list = field(default_factory=list)
    lightmap_size: tuple = (256, 256)
    lightmap: bytes = b''
    sections: list = field(default_factory=list)
    attributes: Optional[array] = None
    props: TerrainProps = field(default_factory=TerrainProps)
    # scene globals
    skybox: list = field(default_factory=lambda: [wstr('') for _ in range(SKYBOX_FACES)])
    light_ambient: int = 0xFF646464
    light_diffuse: int = 0xFFFFFFFF
    light_specular: int = 0xFFFFFFFF
    light_vector: Vec3 = (-0.5, 0.71, -0.5)                     # points towards the sun
    fog_color: int = 0xFF646464
    legacy_fog_near: Optional[float] = None                     # v<28
    legacy_fog_far: Optional[float] = None                      # v<28
    # object info
    static_blocks: list = field(default_factory=list)
    destroyable_blocks: list = field(default_factory=list)      # v11+
    trigger_blocks: list = field(default_factory=list)          # v41+
    effects: list = field(default_factory=list)
    waters: list = field(default_factory=list)
    dynamic_lights: list = field(default_factory=list)
    start_points: list = field(default_factory=list)
    item_points: list = field(default_factory=list)             # v7+
    decals: list = field(default_factory=list)                  # v9+
    force_move_pads: list = field(default_factory=list)         # v29+
    death_zones: list = field(default_factory=list)             # v30+
    # tail
    env_cubemap: bytes = b''                                    # v12+
    lens_flares: list = field(default_factory=list)             # v20+
    observer_points: list = field(default_factory=list)         # v34+
    lightmap_textures: list = field(default_factory=list)       # v40+
    trailing: bytes = b''

    # -- geometry helpers -------------------------------------------------------------------
    @property
    def width(self) -> int:
        return self.sections_x * self.tiles_x

    @property
    def height(self) -> int:
        return self.sections_z * self.tiles_z

    @property
    def vertex_count(self) -> int:
        return (self.width + 1) * (self.height + 1)

    @property
    def attribute_size(self) -> tuple:
        return attribute_size(self.width, self.height, self.scale)

    @property
    def bounds(self) -> tuple:
        """(min_x, max_x, min_z, max_z) in world units."""
        hx = self.width * self.scale[0] * 0.5
        hz = self.height * self.scale[2] * 0.5
        return (-hx, hx, -hz, hz)

    def vertex_xz(self, col: int, row: int) -> tuple:
        return ((col - self.width / 2) * self.scale[0], (self.height / 2 - row) * self.scale[2])

    def height_at(self, col: int, row: int) -> float:
        return self.heights[row * (self.width + 1) + col]

    def height_at_world(self, x: float, z: float) -> float:
        """Bilinear terrain height like CTerrain::GetHeightPos (clamped to the grid)."""
        fc = (x + self.width * self.scale[0] * 0.5) / self.scale[0]
        fr = (self.height * self.scale[2] * 0.5 - z) / self.scale[2]
        fc = min(max(fc, 0.0), self.width - 1e-6)
        fr = min(max(fr, 0.0), self.height - 1e-6)
        c, r = int(fc), int(fr)
        tx, tz = fc - c, fr - r
        n = self.width + 1
        h00 = self.heights[r * n + c]
        h01 = self.heights[r * n + c + 1]
        h10 = self.heights[(r + 1) * n + c]
        h11 = self.heights[(r + 1) * n + c + 1]
        return (h00 * (1 - tx) + h01 * tx) * (1 - tz) + (h10 * (1 - tx) + h11 * tx) * tz


def attribute_size(width: int, height: int, scale) -> tuple:
    """Attribute grid size exactly as the client computes it (ftol(W*scale_x*0.03125))."""
    return (int(width * float(struct.unpack('<f', struct.pack('<f', scale[0]))[0]) * 0.03125),
            int(height * float(struct.unpack('<f', struct.pack('<f', scale[2]))[0]) * 0.03125))


# ---------------------------------------------------------------------------------------------
# low level stream
# ---------------------------------------------------------------------------------------------

class TrnFormatError(ValueError):
    pass


class _Reader:
    __slots__ = ('data', 'pos')

    def __init__(self, data: bytes):
        self.data = memoryview(data)
        self.pos = 0

    def raw(self, n: int) -> bytes:
        if n < 0 or self.pos + n > len(self.data):
            raise TrnFormatError('truncated .trn: need %d bytes at offset %d of %d' % (n, self.pos, len(self.data)))
        b = bytes(self.data[self.pos:self.pos + n])
        self.pos += n
        return b

    def _unpack(self, fmt: str, n: int):
        if self.pos + n > len(self.data):
            raise TrnFormatError('truncated .trn at offset %d' % self.pos)
        v = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += n
        return v

    def u8(self) -> int:
        return self._unpack('<B', 1)[0]

    def u16(self) -> int:
        return self._unpack('<H', 2)[0]

    def u32(self) -> int:
        return self._unpack('<I', 4)[0]

    def i32(self) -> int:
        return self._unpack('<i', 4)[0]

    def f32(self) -> float:
        return self._unpack('<f', 4)[0]

    def floats(self, n: int) -> tuple:
        return self._unpack('<%df' % n, 4 * n)

    def ints(self, n: int) -> tuple:
        return self._unpack('<%di' % n, 4 * n)

    def vec3(self) -> tuple:
        return self._unpack('<3f', 12)

    def count(self, record_min: int, what: str) -> int:
        n = self.i32()
        if n < 0:
            return 0     # the client loops `while i < n` with a signed count
        if n * record_min > len(self.data) - self.pos:
            raise TrnFormatError('implausible %s count %d at offset %d' % (what, n, self.pos - 4))
        return n

    def transform(self) -> Transform:
        v = self._unpack('<12f', 48)
        return Transform(v[0:3], v[3:6], v[6:9], v[9:12])


class _Writer:
    __slots__ = ('parts',)

    def __init__(self):
        self.parts = []

    def raw(self, b: bytes, size: Optional[int] = None):
        if size is not None and len(b) != size:
            raise TrnFormatError('field must be %d bytes, got %d' % (size, len(b)))
        self.parts.append(bytes(b))

    def u8(self, v):
        self.parts.append(struct.pack('<B', int(v) & 0xFF))

    def u16(self, v):
        self.parts.append(struct.pack('<H', v))

    def u32(self, v):
        self.parts.append(struct.pack('<I', int(v) & 0xFFFFFFFF))

    def i32(self, v):
        self.parts.append(struct.pack('<i', v))

    def f32(self, v):
        self.parts.append(struct.pack('<f', v))

    def floats(self, vs, n: int):
        vs = tuple(vs)
        if len(vs) != n:
            raise TrnFormatError('expected %d floats, got %d' % (n, len(vs)))
        self.parts.append(struct.pack('<%df' % n, *vs))

    def ints(self, vs, n: int):
        vs = tuple(vs)
        if len(vs) != n:
            raise TrnFormatError('expected %d ints, got %d' % (n, len(vs)))
        self.parts.append(struct.pack('<%di' % n, *vs))

    def vec3(self, v):
        self.floats(v, 3)

    def transform(self, t: Transform):
        self.floats(tuple(t.position) + tuple(t.forward) + tuple(t.up) + tuple(t.scale), 12)

    def getvalue(self) -> bytes:
        return b''.join(self.parts)


def _name(b, size=NAME_BYTES) -> bytes:
    if b is None:
        return bytes(size)
    if isinstance(b, str):
        return wstr(b, size)
    if len(b) != size:
        raise TrnFormatError('wchar field must be %d bytes' % size)
    return bytes(b)


# ---------------------------------------------------------------------------------------------
# parse
# ---------------------------------------------------------------------------------------------

def _read_lightmap_payload(r: _Reader, v: int) -> ObjectLightmap:
    flag = r.u8()
    if flag:
        size = r.u32()
        return ObjectLightmap(flag, r.raw(size))
    return ObjectLightmap(0, b'')


def _read_path(r: _Reader) -> tuple:
    speed = r.f32()
    n = r.u32()
    if n * 12 > len(r.data) - r.pos:
        raise TrnFormatError('implausible path length %d at offset %d' % (n, r.pos - 4))
    pts = r.floats(3 * n) if n else ()
    return speed, [tuple(pts[i:i + 3]) for i in range(0, 3 * n, 3)]


def parse(data: bytes) -> TrnFile:
    """Decode a complete `.trn`. `serialize(parse(b)) == b` for every shipped file."""
    r = _Reader(data)
    t = TrnFile()
    v = t.version = r.u16()
    if v < MIN_VERSION:
        raise TrnFormatError('.trn version %d is older than the supported minimum %d' % (v, MIN_VERSION))
    t.sections_x, t.sections_z, t.tiles_x, t.tiles_z = r.u32(), r.u32(), r.u32(), r.u32()
    if not (0 < t.sections_x <= 256 and 0 < t.sections_z <= 256 and 0 < t.tiles_x <= 256 and 0 < t.tiles_z <= 256):
        raise TrnFormatError('implausible terrain grid %r' % ((t.sections_x, t.sections_z, t.tiles_x, t.tiles_z),))
    t.scale = r.floats(3)
    nv = t.vertex_count
    t.heights = _farray(r.raw(4 * nv))
    t.normals = _farray(r.raw(12 * nv))
    t.uv_origin = r.floats(2)
    # layers
    nl = r.u32()
    if nl > 64:
        raise TrnFormatError('implausible layer count %d' % nl)
    for _ in range(nl):
        layer = TerrainLayer()
        layer.texture = r.raw(NAME_BYTES)
        layer.uv_transform = r.floats(16)
        if v > 2:
            layer.specular = r.floats(4)
            layer.specular_power = r.f32()
        else:
            layer.specular = layer.specular_power = None
        layer.alpha = r.raw(GRID_VERTICES * GRID_VERTICES if v >= 6 else 256 * 256)
        t.layers.append(layer)
    # lightmap
    if v >= 6:
        lw, lh = r.u32(), r.u32()
    else:
        lw, lh = 256, 256
    if lw > 8192 or lh > 8192:
        raise TrnFormatError('implausible lightmap size %dx%d' % (lw, lh))
    t.lightmap_size = (lw, lh)
    t.lightmap = r.raw(lw * lh * 3)
    # sections
    t.sections = [r.ints(SECTION_LAYER_SLOTS) for _ in range(t.sections_x * t.sections_z)]
    # attribute table
    aw, ah = t.attribute_size
    t.attributes = _farray(r.raw(4 * aw * ah), 'I') if v >= 2 else None
    # TerrainProp
    p = t.props = TerrainProps(*([None] * len(TerrainProps.__dataclass_fields__)))
    if v >= 4:
        p.lod = r.floats(3)
    if v >= 5:
        p.play_area_x = r.f32()
    if v >= 25:
        p.play_area_z = r.f32()
    if v >= 31:
        p.show_play_area = r.u8()
    if v >= 15:
        p.play_height = r.f32()
    if 17 <= v < 28:
        p.legacy_view_distance = r.f32()
    if v >= 28:
        p.distance_levels = [r.floats(3) for _ in range(DISTANCE_LEVELS)]
    if v >= 18:
        p.skybox_render = r.u8()
    if v >= 25:
        p.skybox_color = r.u32()
    if v >= 22:
        p.show_dropship = r.u8()
    if v >= 25:
        p.terrain_render = r.u8()
    if v >= 26:
        p.terrain_kill = r.u8()
    if v >= 13:
        p.cloud_texture = r.raw(NAME_BYTES)
        p.cloud_tile = r.floats(2)
        p.cloud_scroll = r.floats(2)
    if v >= 21:
        p.item_patterns_red = [r.ints(ITEM_PATTERN_SLOTS) for _ in range(r.count(32, 'red item pattern'))]
        p.item_patterns_blue = [r.ints(ITEM_PATTERN_SLOTS) for _ in range(r.count(32, 'blue item pattern'))]
    if v >= 23:
        p.weather_effect = r.raw(NAME_BYTES)
    if v >= 33:
        p.space_map = r.u8()
    if v >= 26:
        if v >= 27:
            p.water_flag = r.u8()
        p.water_fog_color = r.u32()
        p.water_fog_far = r.f32()
        p.water_fog_near = r.f32()
        p.water_effect = r.raw(NAME_BYTES)
    if v >= 32:
        p.sniping_culling = r.u8()
        p.sniping_distance = r.f32()
    # scene globals
    t.skybox = [r.raw(NAME_BYTES) for _ in range(SKYBOX_FACES)]
    t.light_ambient = r.u32()
    t.light_diffuse = r.u32()
    t.light_specular = r.u32() if v >= 10 else None
    t.light_vector = r.vec3()
    t.fog_color = r.u32()
    if v < 28:
        t.legacy_fog_near = r.f32()
        t.legacy_fog_far = r.f32()
    # LoadObjectInfo
    for _ in range(r.count(0x71, 'static block')):
        b = StaticBlock()
        b.model = r.raw(NAME_BYTES)
        b.transform = r.transform()
        b.receive_shadow = r.u8()
        b.always_render = r.u8() if v > 4 else None
        b.back_alpha = r.u8() if v > 8 else None
        b.near_alpha = r.u8() if v > 18 else None
        b.low_detail_skip = r.u8() if v > 35 else None
        if v > 38:
            b.path_speed, b.path = _read_path(r)
        else:
            b.path_speed, b.path = None, None
        b.lightmap = _read_lightmap_payload(r, v) if v > 39 else None
        t.static_blocks.append(b)
    if v > 10:
        for _ in range(r.count(0x37, 'destroyable block')):
            b = DestroyableBlock()
            b.object_id = r.u32()
            b.name = r.raw(LONG_NAME_BYTES) if v > 23 else None
            b.transform = r.transform()
            b.receive_shadow, b.always_render, b.back_alpha = r.u8(), r.u8(), r.u8()
            b.near_alpha = r.u8() if v > 18 else None
            if v > 38:
                b.path_speed, b.path = _read_path(r)
            else:
                b.path_speed, b.path = None, None
            b.unused = r.vec3() if v > 15 else None
            b.lightmap = _read_lightmap_payload(r, v) if v > 39 else None
            t.destroyable_blocks.append(b)
    if v > 40:
        for _ in range(r.count(0x37, 'trigger block')):
            b = TriggerBlock()
            b.object_id = r.u32()
            b.name = r.raw(NAME_BYTES) if v > 23 else None
            b.transform = r.transform()
            b.receive_shadow, b.always_render, b.back_alpha = r.u8(), r.u8(), r.u8()
            b.near_alpha = r.u8() if v > 18 else None
            b.low_detail_skip = r.u8() if v > 35 else None
            if v > 38:
                b.path_speed, b.path = _read_path(r)
            else:
                b.path_speed, b.path = None, None
            b.lightmap = _read_lightmap_payload(r, v) if v > 39 else None
            t.trigger_blocks.append(b)
    for _ in range(r.count(0x70, 'effect')):
        e = EffectInstance()
        e.effect = r.raw(NAME_BYTES)
        e.transform = r.transform()
        e.always_render = r.u8() if v > 7 else None
        e.low_detail_skip = r.u8() if v > 36 else None
        e.back_alpha = r.u8() if v > 37 else None
        t.effects.append(e)
    for _ in range(r.count(0x44, 'water')):
        w = WaterPlane()
        nt = r.u32()
        if nt > 1024:
            raise TrnFormatError('implausible water texture count %d' % nt)
        w.textures = [r.raw(NAME_BYTES) for _ in range(nt)]
        w.wrap_u, w.wrap_v, w.param_c, w.param_d = r.floats(4)
        w.transform = r.transform()
        t.waters.append(w)
    for _ in range(r.count(0x68, 'dynamic light')):
        f = struct.unpack('<I25f', r.raw(0x68))
        t.dynamic_lights.append(DynamicLight(f[0], f[1:5], f[5:9], f[9:13], f[13:16], f[16:19], *f[19:26]))
    for _ in range(r.count(0x5A, 'start point')):
        name = r.raw(NAME_BYTES)
        colour, index = r.u8(), r.u8()
        t.start_points.append(StartPoint(name, colour, index, r.vec3(), r.vec3()))
    if v > 6:
        for _ in range(r.count(0x59, 'item point')):
            name = r.raw(NAME_BYTES)
            kind = r.u8()
            t.item_points.append(ItemPoint(name, kind, r.vec3(), r.vec3()))
    if v > 8:
        for _ in range(r.count(0x88, 'decal')):
            d = Decal()
            d.texture = r.raw(NAME_BYTES)
            d.alpha, d.receive_shadow, d.u_wrap, d.v_wrap = r.u8(), r.u8(), r.u8(), r.u8()
            d.tex_scale = r.floats(2)
            d.tex_offset = r.floats(2)
            d.tex_rotation = r.f32()
            d.transform = r.transform()
            t.decals.append(d)
    if v > 28:
        for _ in range(r.count(0x38, 'force-move pad')):
            tr = r.transform()
            speed, duration = r.f32(), r.f32()
            t.force_move_pads.append(ForceMovePad(tr, speed, duration))
    if v > 29:
        for _ in range(r.count(0x30, 'death zone')):
            t.death_zones.append(DeathZone(r.transform()))
    # tail
    if v >= 12:
        size = r.u32()
        t.env_cubemap = r.raw(size)
    if 14 <= v < 20:
        raise TrnFormatError('.trn version %d carries an undecoded legacy lens flare' % v)
    if v >= 20:
        for _ in range(r.count(0x1D, 'lens flare')):
            lf = LensFlare()
            lf.position = r.vec3()
            lf.min_distance, lf.max_distance = r.f32(), r.f32()
            lf.fade_frames = r.i32()
            lf.always_render = r.u8()
            ne = r.u32()
            if ne > 4096:
                raise TrnFormatError('implausible lens flare element count %d' % ne)
            for _ in range(ne):
                tex = r.raw(NAME_BYTES)
                loc, size = r.f32(), r.f32()
                lf.elements.append(LensFlareElement(tex, loc, size, r.u32()))
            lf.background = r.u8() if v >= 35 else None
            lf.hide_low_detail = r.u8() if v >= 37 else None
            t.lens_flares.append(lf)
    if v >= 34:
        for _ in range(r.count(0x39, 'observer point')):
            mode = r.u8()
            f = r.floats(5)
            t.observer_points.append(ObserverPoint(mode, *f, r.vec3(), r.vec3(), r.vec3()))
    if v >= 40:
        for _ in range(r.count(4, 'lightmap texture')):
            size = r.u32()
            t.lightmap_textures.append(r.raw(size))
    t.trailing = bytes(r.data[r.pos:])
    return t


# ---------------------------------------------------------------------------------------------
# serialize
# ---------------------------------------------------------------------------------------------

def _opt(value, default):
    return default if value is None else value


def _write_path(w: _Writer, speed, path):
    w.f32(_opt(speed, 0.03))
    path = path or []
    w.u32(len(path))
    for pt in path:
        w.vec3(pt)


def _write_lightmap_payload(w: _Writer, lm: Optional[ObjectLightmap]):
    if lm is None or not lm.flag:
        w.u8(0)
        return
    w.u8(lm.flag)
    w.u32(len(lm.data))
    w.raw(lm.data)


def serialize(trn: TrnFile) -> bytes:
    """Encode a TrnFile following the loader gates of `trn.version` exactly.

    Fields that are None but required by the version are written with the documented defaults, so a
    TrnFile can be re-targeted to another version by changing `version`.
    """
    t = trn
    v = int(t.version)
    if not MIN_VERSION <= v <= 0x7FFF or 14 <= v < 20:
        raise TrnFormatError('cannot write .trn version %d' % v)
    w = _Writer()
    w.u16(v)
    for n in (t.sections_x, t.sections_z, t.tiles_x, t.tiles_z):
        w.u32(n)
    w.floats(t.scale, 3)
    nv = t.vertex_count
    if len(t.heights) != nv or len(t.normals) != 3 * nv:
        raise TrnFormatError('heights/normals must have %d / %d floats' % (nv, 3 * nv))
    w.raw(_abytes(t.heights if isinstance(t.heights, array) else array('f', t.heights)))
    w.raw(_abytes(t.normals if isinstance(t.normals, array) else array('f', t.normals)))
    w.floats(t.uv_origin, 2)
    w.u32(len(t.layers))
    alpha_size = GRID_VERTICES * GRID_VERTICES if v >= 6 else 256 * 256
    for layer in t.layers:
        w.raw(_name(layer.texture))
        w.floats(layer.uv_transform, 16)
        if v > 2:
            w.floats(_opt(layer.specular, (0.0, 0.0, 0.0, 0.0)), 4)
            w.f32(_opt(layer.specular_power, 15.0))
        w.raw(layer.alpha, alpha_size)
    lw, lh = t.lightmap_size
    if v >= 6:
        w.u32(lw)
        w.u32(lh)
    elif (lw, lh) != (256, 256):
        raise TrnFormatError('versions < 6 imply a 256x256 lightmap')
    w.raw(t.lightmap, lw * lh * 3)
    if len(t.sections) != t.sections_x * t.sections_z:
        raise TrnFormatError('need %d section descriptors' % (t.sections_x * t.sections_z))
    for s in t.sections:
        w.ints(s, SECTION_LAYER_SLOTS)
    if v >= 2:
        aw, ah = t.attribute_size
        attrs = t.attributes if t.attributes is not None else array('I', bytes(4 * aw * ah))
        if len(attrs) != aw * ah:
            raise TrnFormatError('attribute table must have %d x %d entries' % (aw, ah))
        w.raw(_abytes(attrs if isinstance(attrs, array) else array('I', attrs)))
    p = t.props
    d = TerrainProps()
    if v >= 4:
        w.floats(_opt(p.lod, d.lod), 3)
    if v >= 5:
        w.f32(_opt(p.play_area_x, d.play_area_x))
    if v >= 25:
        w.f32(_opt(p.play_area_z, _opt(p.play_area_x, d.play_area_z)))
    if v >= 31:
        w.u8(_opt(p.show_play_area, d.show_play_area))
    if v >= 15:
        w.f32(_opt(p.play_height, d.play_height))
    levels = p.distance_levels
    if 17 <= v < 28:
        w.f32(_opt(p.legacy_view_distance, levels[-1][0] if levels else 12000.0))
    if v >= 28:
        if levels is None:
            far = _opt(p.legacy_view_distance, 12000.0)
            levels = [(far, _opt(t.legacy_fog_far, 5000.0), _opt(t.legacy_fog_near, 1500.0))] * DISTANCE_LEVELS
        if len(levels) != DISTANCE_LEVELS:
            raise TrnFormatError('need %d distance levels' % DISTANCE_LEVELS)
        for lv in levels:
            w.floats(lv, 3)
    if v >= 18:
        w.u8(_opt(p.skybox_render, d.skybox_render))
    if v >= 25:
        w.u32(_opt(p.skybox_color, d.skybox_color))
    if v >= 22:
        w.u8(_opt(p.show_dropship, d.show_dropship))
    if v >= 25:
        w.u8(_opt(p.terrain_render, d.terrain_render))
    if v >= 26:
        w.u8(_opt(p.terrain_kill, d.terrain_kill))
    if v >= 13:
        w.raw(_name(p.cloud_texture))
        w.floats(_opt(p.cloud_tile, d.cloud_tile), 2)
        w.floats(_opt(p.cloud_scroll, d.cloud_scroll), 2)
    if v >= 21:
        for patterns in (p.item_patterns_red or [], p.item_patterns_blue or []):
            w.u32(len(patterns))
            for pat in patterns:
                w.ints(pat, ITEM_PATTERN_SLOTS)
    if v >= 23:
        w.raw(_name(p.weather_effect))
    if v >= 33:
        w.u8(_opt(p.space_map, d.space_map))
    if v >= 26:
        if v >= 27:
            w.u8(_opt(p.water_flag, d.water_flag))
        w.u32(_opt(p.water_fog_color, d.water_fog_color))
        w.f32(_opt(p.water_fog_far, d.water_fog_far))
        w.f32(_opt(p.water_fog_near, d.water_fog_near))
        w.raw(_name(p.water_effect))
    if v >= 32:
        w.u8(_opt(p.sniping_culling, d.sniping_culling))
        w.f32(_opt(p.sniping_distance, d.sniping_distance))
    # scene globals
    if len(t.skybox) != SKYBOX_FACES:
        raise TrnFormatError('skybox needs %d faces' % SKYBOX_FACES)
    for face in t.skybox:
        w.raw(_name(face))
    w.u32(t.light_ambient)
    w.u32(t.light_diffuse)
    if v >= 10:
        w.u32(_opt(t.light_specular, 0xFFFFFFFF))
    w.vec3(t.light_vector)
    w.u32(t.fog_color)
    if v < 28:
        lv = (p.distance_levels or [(0.0, 5000.0, 1500.0)])[-1]
        w.f32(_opt(t.legacy_fog_near, lv[2]))
        w.f32(_opt(t.legacy_fog_far, lv[1]))
    # LoadObjectInfo
    w.u32(len(t.static_blocks))
    for b in t.static_blocks:
        w.raw(_name(b.model))
        w.transform(b.transform)
        w.u8(b.receive_shadow)
        if v > 4:
            w.u8(_opt(b.always_render, 0))
        if v > 8:
            w.u8(_opt(b.back_alpha, 0))
        if v > 18:
            w.u8(_opt(b.near_alpha, 0))
        if v > 35:
            w.u8(_opt(b.low_detail_skip, 0))
        if v > 38:
            _write_path(w, b.path_speed, b.path)
        if v > 39:
            _write_lightmap_payload(w, b.lightmap)
    if v > 10:
        w.u32(len(t.destroyable_blocks))
        for b in t.destroyable_blocks:
            w.u32(b.object_id)
            if v > 23:
                w.raw(_name(b.name, LONG_NAME_BYTES))
            w.transform(b.transform)
            w.u8(b.receive_shadow)
            w.u8(b.always_render)
            w.u8(b.back_alpha)
            if v > 18:
                w.u8(_opt(b.near_alpha, 0))
            if v > 38:
                _write_path(w, b.path_speed, b.path)
            if v > 15:
                w.vec3(_opt(b.unused, (0.0, 0.0, 0.0)))
            if v > 39:
                _write_lightmap_payload(w, b.lightmap)
    elif t.destroyable_blocks:
        raise TrnFormatError('destroyable blocks need version >= 11')
    if v > 40:
        w.u32(len(t.trigger_blocks))
        for b in t.trigger_blocks:
            w.u32(b.object_id)
            if v > 23:
                w.raw(_name(b.name))
            w.transform(b.transform)
            w.u8(b.receive_shadow)
            w.u8(b.always_render)
            w.u8(b.back_alpha)
            if v > 18:
                w.u8(_opt(b.near_alpha, 0))
            if v > 35:
                w.u8(_opt(b.low_detail_skip, 0))
            if v > 38:
                _write_path(w, b.path_speed, b.path)
            if v > 39:
                _write_lightmap_payload(w, b.lightmap)
    elif t.trigger_blocks:
        raise TrnFormatError('trigger blocks need version 41')
    w.u32(len(t.effects))
    for e in t.effects:
        w.raw(_name(e.effect))
        w.transform(e.transform)
        if v > 7:
            w.u8(_opt(e.always_render, 0))
        if v > 36:
            w.u8(_opt(e.low_detail_skip, 0))
        if v > 37:
            w.u8(_opt(e.back_alpha, 0))
    w.u32(len(t.waters))
    for wa in t.waters:
        w.u32(len(wa.textures))
        for tex in wa.textures:
            w.raw(_name(tex))
        w.floats((wa.wrap_u, wa.wrap_v, wa.param_c, wa.param_d), 4)
        w.transform(wa.transform)
    w.u32(len(t.dynamic_lights))
    for li in t.dynamic_lights:
        w.raw(struct.pack('<I25f', li.type, *li.diffuse, *li.specular, *li.ambient, *li.position,
                          *li.direction, li.range, li.falloff, li.attenuation0, li.attenuation1,
                          li.attenuation2, li.theta, li.phi))
    w.u32(len(t.start_points))
    for sp in t.start_points:
        w.raw(_name(sp.name))
        w.u8(sp.colour)
        w.u8(sp.index)
        w.vec3(sp.position)
        w.vec3(sp.direction)
    if v > 6:
        w.u32(len(t.item_points))
        for ip in t.item_points:
            w.raw(_name(ip.name))
            w.u8(ip.item_type)
            w.vec3(ip.position)
            w.vec3(ip.direction)
    elif t.item_points:
        raise TrnFormatError('item points need version >= 7')
    if v > 8:
        w.u32(len(t.decals))
        for dc in t.decals:
            w.raw(_name(dc.texture))
            for b in (dc.alpha, dc.receive_shadow, dc.u_wrap, dc.v_wrap):
                w.u8(b)
            w.floats(dc.tex_scale, 2)
            w.floats(dc.tex_offset, 2)
            w.f32(dc.tex_rotation)
            w.transform(dc.transform)
    if v > 28:
        w.u32(len(t.force_move_pads))
        for pad in t.force_move_pads:
            w.transform(pad.transform)
            w.f32(pad.speed)
            w.f32(pad.duration)
    if v > 29:
        w.u32(len(t.death_zones))
        for z in t.death_zones:
            w.transform(z.transform)
    # tail
    if v >= 12:
        w.u32(len(t.env_cubemap))
        w.raw(t.env_cubemap)
    if v >= 20:
        w.u32(len(t.lens_flares))
        for lf in t.lens_flares:
            w.vec3(lf.position)
            w.f32(lf.min_distance)
            w.f32(lf.max_distance)
            w.i32(lf.fade_frames)
            w.u8(lf.always_render)
            w.u32(len(lf.elements))
            for el in lf.elements:
                w.raw(_name(el.texture))
                w.f32(el.location)
                w.f32(el.size)
                w.u32(el.colour)
            if v >= 35:
                w.u8(_opt(lf.background, 0))
            if v >= 37:
                w.u8(_opt(lf.hide_low_detail, 0))
    if v >= 34:
        w.u32(len(t.observer_points))
        for ob in t.observer_points:
            w.u8(ob.mode)
            w.floats((ob.far, ob.near, ob.fov, ob.param4, ob.param5), 5)
            w.vec3(ob.position)
            w.vec3(ob.direction)
            w.vec3(ob.up)
    if v >= 40:
        w.u32(len(t.lightmap_textures))
        for tex in t.lightmap_textures:
            w.u32(len(tex))
            w.raw(tex)
    w.raw(t.trailing or b'')
    return w.getvalue()


# ---------------------------------------------------------------------------------------------
# builder for new maps
# ---------------------------------------------------------------------------------------------

def _grid_values(values, count: int, typecode: str, what: str) -> array:
    """Accept bytes (raw little endian), array, numpy array, flat or nested sequences."""
    if isinstance(values, (bytes, bytearray, memoryview)):
        a = _farray(bytes(values), typecode)
    elif isinstance(values, array):
        a = array(typecode, values)
    elif hasattr(values, 'dtype') and hasattr(values, 'ravel'):          # numpy, without importing it
        a = array(typecode, values.ravel().tolist())
    else:
        flat = []
        for item in values:
            if isinstance(item, (list, tuple)) or (hasattr(item, '__iter__') and not isinstance(item, (str, bytes))):
                flat.extend(item)
            else:
                flat.append(item)
        a = array(typecode, flat)
    if len(a) != count:
        raise ValueError('%s needs %d values, got %d' % (what, count, len(a)))
    return a


def _u8_grid(values, count: int, what: str) -> bytes:
    if isinstance(values, (int, float)):
        v = int(values)
        if not 0 <= v <= 255:
            raise ValueError('%s value must be 0..255' % what)
        return bytes([v]) * count
    if isinstance(values, (bytes, bytearray, memoryview)):
        b = bytes(values)
    else:
        a = _grid_values(values, count, 'd', what)
        if any(not 0 <= x <= 255 for x in a):
            raise ValueError('%s values must be 0..255' % what)
        b = bytes(int(round(x)) for x in a)
    if len(b) != count:
        raise ValueError('%s needs %d bytes, got %d' % (what, count, len(b)))
    return b


def compute_normals(heights: array, width: int, height: int, scale) -> array:
    """Central-difference vertex normals (row 0 = +Z). Within ~0.01 of the engine's averaged face normals."""
    n = width + 1
    sx, sz = float(scale[0]), float(scale[2])
    out = array('f', bytes(12 * n * (height + 1)))
    for row in range(height + 1):
        r0, r1 = max(row - 1, 0), min(row + 1, height)
        for col in range(n):
            c0, c1 = max(col - 1, 0), min(col + 1, width)
            dhdx = (heights[row * n + c1] - heights[row * n + c0]) / ((c1 - c0) * sx)
            # z decreases with row: dh/dz = (h[row-1] - h[row+1]) / dz
            dhdz = (heights[r0 * n + col] - heights[r1 * n + col]) / ((r1 - r0) * sz)
            x, y, z = -dhdx, 1.0, -dhdz
            inv = 1.0 / math.sqrt(x * x + y * y + z * z)
            i = 3 * (row * n + col)
            out[i], out[i + 1], out[i + 2] = x * inv, y * inv, z * inv
    return out


def solid_cubemap_dds(rgb=(96, 112, 128), size: int = 256) -> bytes:
    """A DXT1 cube map DDS of one colour with the exact header layout of the shipped environment maps
    (flags 0x1007, caps 0x1008, caps2 0xFE00, no mips; 196736 bytes for size 256)."""
    if size < 4 or size % 4:
        raise ValueError('cube face size must be a multiple of 4')
    r, g, b = (int(c) & 0xFF for c in rgb)
    c565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    header = struct.pack('<4s7I44x8I5I', b'DDS ', 124, 0x1007, size, size, 0, 0, 0,
                         32, 4, 0x31545844, 0, 0, 0, 0, 0, 0x1008, 0xFE00, 0, 0, 0)
    block = struct.pack('<HHI', c565, c565, 0)
    return header + block * ((size // 4) * (size // 4) * 6)


def layer_uv_transform(tile_size: float = 256.0) -> tuple:
    """Shipped layer texture matrix for a detail texture repeating every `tile_size` world units."""
    s = 1.0 / float(tile_size)
    return (s, 0.0, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 2 * s, 2 * s, 524272.0, 1.0)


def section_layers(trn: TrnFile) -> list:
    """Section descriptors: layer 0 always, plus every other layer with non-zero alpha in the section."""
    n = trn.width + 1
    out = []
    for sz in range(trn.sections_z):
        for sx in range(trn.sections_x):
            used = []
            for i, layer in enumerate(trn.layers):
                if i == 0:
                    used.append(0)
                    continue
                r0, c0 = sz * trn.tiles_z, sx * trn.tiles_x
                a = layer.alpha
                if any(a[(r0 + rr) * n + c0:(r0 + rr) * n + c0 + trn.tiles_x + 1].strip(b'\0')
                       for rr in range(trn.tiles_z + 1)):
                    used.append(i)
            used = used[:SECTION_LAYER_SLOTS - 1] + [-1]
            out.append(tuple(used + [0] * (SECTION_LAYER_SLOTS - len(used))))
    return out


_SCENE_TEMPLATE_FIELDS = ('skybox', 'light_ambient', 'light_diffuse', 'light_specular', 'light_vector',
                          'fog_color', 'env_cubemap', 'lens_flares', 'observer_points')


class TrnBuilder:
    """Assemble a new map `.trn` (default version 40, the most common shipped version).

    Minimal valid map: heights + one textured layer + at least the start points the modes need. Everything
    else has a safe default. Pass `template` (a parsed shipped `.trn`) to copy look-and-feel settings:
    TerrainProp (LOD, play area/height, distance levels/fog, skybox flags, cloud, weather/water effects,
    sniping culling), skybox textures, sun light, fog colour, environment cube map, lens flares and
    observer cameras. Map-specific content (objects, points, item patterns, object lightmap textures,
    attributes) is never copied.

    Without a template the environment cube map is a generated solid-colour DXT1 cube, the skybox faces are
    empty and sun/fog use the most common shipped values; a template is recommended for the look. The
    builder always writes terrain_render = 1 / terrain_kill = 0 (the height map is the ground). Fine
    tuning: edit `builder.trn.props` / `builder.trn.light_*` / `fog_color` / `skybox` before build().

    Spawn layout used by the modes (colour, index): (1, 0..5) and (2, 0..5) team spawns, (0, 0..11) shared
    relocation points, (3, *) and (4, *) Grid respawn points. Item points are indexed in insertion order;
    item patterns (8 indices each) per team select item spawn sets and must match server tbl_MapItemPath.

    Example:
        b = TrnBuilder(template=parse(open('0401.trn', 'rb').read()))
        b.set_heights(heights_257x257)                         # world-unit Y values, row 0 = +Z edge
        b.add_layer('Txrs\\\\2259.txr')                         # base layer, alpha 255 everywhere
        b.add_static_block('mdrs\\\\0823.mod', (0, 0, 0))
        b.add_start_point(1, 0, (-2000, 350, 0), (1, 0, 0))
        data = b.to_bytes()
    """

    def __init__(self, template: Optional[TrnFile] = None, *, version: int = DEFAULT_VERSION,
                 sections: int = GRID_SECTIONS, tiles: int = GRID_TILES, scale=64.0):
        if version < 28 or version > MAX_VERSION:
            raise ValueError('the builder writes versions 28..%d' % MAX_VERSION)
        if isinstance(scale, (int, float)):
            scale = (float(scale),) * 3
        t = self.trn = TrnFile(version=version, sections_x=sections, sections_z=sections,
                               tiles_x=tiles, tiles_z=tiles, scale=tuple(float(s) for s in scale))
        t.uv_origin = (-t.width * t.scale[0] / 2, -t.height * t.scale[2] / 2)
        t.heights = array('f', bytes(4 * t.vertex_count))
        t.normals = None
        t.props.distance_levels = [(12000.0, 3500.0, 1000.0)] * DISTANCE_LEVELS
        t.props.cloud_texture = wstr('')
        t.props.weather_effect = wstr('')
        t.props.water_effect = wstr('')
        t.props.item_patterns_red = []
        t.props.item_patterns_blue = []
        t.env_cubemap = solid_cubemap_dds()
        t.observer_points = [ObserverPoint()]
        self._lightmap = None
        if template is not None:
            self.apply_template(template)

    # -- template ---------------------------------------------------------------------------
    def apply_template(self, template: TrnFile):
        """Copy sky/fog/light/prop settings (not geometry or objects) from a parsed shipped map."""
        t = self.trn
        props = copy.deepcopy(template.props)
        props.item_patterns_red = []
        props.item_patterns_blue = []
        if props.distance_levels is None:
            far = _opt(props.legacy_view_distance, 12000.0)
            props.distance_levels = [(far, _opt(template.legacy_fog_far, 5000.0),
                                      _opt(template.legacy_fog_near, 1500.0))] * DISTANCE_LEVELS
        props.legacy_view_distance = None
        default = TerrainProps()
        for name in TerrainProps.__dataclass_fields__:
            if getattr(props, name) is None and name != 'legacy_view_distance':
                setattr(props, name, getattr(default, name))
        for name in ('cloud_texture', 'weather_effect', 'water_effect'):
            if getattr(props, name) is None:
                setattr(props, name, wstr(''))
        props.terrain_render = 1          # a height-map map must draw its terrain
        props.terrain_kill = 0
        t.props = props
        for name in _SCENE_TEMPLATE_FIELDS:
            setattr(t, name, copy.deepcopy(getattr(template, name)))
        if t.light_specular is None:
            t.light_specular = 0xFFFFFFFF
        if not t.env_cubemap:
            t.env_cubemap = solid_cubemap_dds()
        for lf in t.lens_flares:
            lf.background = _opt(lf.background, 0)
            lf.hide_low_detail = _opt(lf.hide_low_detail, 0)
        if not t.observer_points:
            t.observer_points = [ObserverPoint()]

    # -- terrain ----------------------------------------------------------------------------
    def set_heights(self, heights, normals=None):
        """`heights`: (W+1)x(H+1) world-unit Y values (nested rows, flat, numpy or raw f32 bytes).
        `normals`: optional matching 3-float normals; computed when omitted."""
        t = self.trn
        t.heights = _grid_values(heights, t.vertex_count, 'f', 'heights')
        t.normals = (_grid_values(normals, 3 * t.vertex_count, 'f', 'normals') if normals is not None
                     else compute_normals(t.heights, t.width, t.height, t.scale))

    def add_layer(self, texture: str, alpha=255, *, tile_size: float = 256.0,
                  specular=(0.0, 0.0, 0.0, 0.0), specular_power: float = 15.0, uv_transform=None) -> int:
        """Add a splat layer; `alpha` is 0..255 per vertex (257x257) or a constant. Layer 0 is the base and
        should cover everything (alpha 255). Returns the layer index."""
        t = self.trn
        if len(t.layers) >= MAX_LAYERS:
            raise ValueError('at most %d layers' % MAX_LAYERS)
        t.layers.append(TerrainLayer(wstr(texture), tuple(uv_transform or layer_uv_transform(tile_size)),
                                     tuple(float(x) for x in specular), float(specular_power),
                                     _u8_grid(alpha, GRID_VERTICES * GRID_VERTICES, 'layer alpha')))
        return len(t.layers) - 1

    def set_lightmap(self, width: int, height: int, pixels=None, *, rgb=None, fill=(240, 240, 240)):
        """Terrain lightmap (power-of-two, 256..1024 shipped). `pixels`: raw B,G,R bytes row 0 = +Z edge;
        `rgb`: alternatively a sequence of (r, g, b) tuples; otherwise a constant `fill` colour (r, g, b)."""
        if width <= 0 or height <= 0 or width > 4096 or height > 4096:
            raise ValueError('lightmap size out of range')
        if pixels is not None:
            data = bytes(pixels)
        elif rgb is not None:
            data = b''.join(bytes((int(b), int(g), int(r))) for r, g, b in rgb)
        else:
            r, g, b = fill
            data = bytes((int(b), int(g), int(r))) * (width * height)
        if len(data) != width * height * 3:
            raise ValueError('lightmap needs %d bytes' % (width * height * 3))
        self._lightmap = ((width, height), data)

    def set_play_area(self, x_fraction: float, z_fraction: float, *, height: Optional[float] = None,
                      show: bool = True):
        """Playable rectangle as fractions of the terrain half extent, flight ceiling (`height`)."""
        p = self.trn.props
        if not (0.0 < x_fraction <= 1.0 and 0.0 < z_fraction <= 1.0):
            raise ValueError('play area fractions must be in (0, 1]')
        p.play_area_x, p.play_area_z, p.show_play_area = float(x_fraction), float(z_fraction), int(bool(show))
        if height is not None:
            p.play_height = float(height)

    def set_attributes(self, values):
        """Attribute bit words per 32x32 world-unit cell (aw x ah, see TrnFile.attribute_size)."""
        aw, ah = self.trn.attribute_size
        self.trn.attributes = _grid_values(values, aw * ah, 'I', 'attributes')

    # -- objects ----------------------------------------------------------------------------
    @staticmethod
    def _transform(position, forward, up, scale) -> Transform:
        if isinstance(scale, (int, float)):
            scale = (float(scale),) * 3
        return Transform(tuple(map(float, position)), tuple(map(float, forward)), tuple(map(float, up)),
                         tuple(map(float, scale)))

    def add_static_block(self, model: str, position, forward=(0.0, 0.0, 1.0), up=(0.0, 1.0, 0.0),
                         scale=(1.0, 1.0, 1.0), *, receive_shadow=0, always_render=0, back_alpha=1,
                         near_alpha=0, low_detail_skip=0, path=(), path_speed=0.03) -> StaticBlock:
        """Static prop `mdrs\\NNNN.mod` (its OBB section provides collision). Flag defaults are the most
        common shipped combination; `path` (waypoints) makes it a moving prop."""
        b = StaticBlock(wstr(model), self._transform(position, forward, up, scale), int(receive_shadow),
                        int(always_render), int(back_alpha), int(near_alpha), int(low_detail_skip),
                        float(path_speed), [tuple(map(float, p)) for p in path], ObjectLightmap())
        self.trn.static_blocks.append(b)
        return b

    def add_destroyable_block(self, object_id: int, name: str, position, forward=(0.0, 0.0, 1.0),
                              up=(0.0, 1.0, 0.0), scale=(1.0, 1.0, 1.0), *, receive_shadow=0,
                              always_render=0, back_alpha=1, near_alpha=0) -> DestroyableBlock:
        """Breakable object of a client object definition `object_id` (server tbl_MapObjects/MapBlocks)."""
        b = DestroyableBlock(int(object_id), wstr(name, LONG_NAME_BYTES), self._transform(position, forward, up, scale),
                             int(receive_shadow), int(always_render), int(back_alpha), int(near_alpha),
                             0.03, [], (0.0, 0.0, 0.0), ObjectLightmap())
        self.trn.destroyable_blocks.append(b)
        return b

    def add_effect(self, effect: str, position, forward=(0.0, 0.0, 1.0), up=(0.0, 1.0, 0.0),
                   scale=(1.0, 1.0, 1.0), *, always_render=0, low_detail_skip=0, back_alpha=1) -> EffectInstance:
        e = EffectInstance(wstr(effect), self._transform(position, forward, up, scale), int(always_render),
                           int(low_detail_skip), int(back_alpha))
        self.trn.effects.append(e)
        return e

    def add_start_point(self, colour: int, index: int, position, direction=(0.0, 0.0, 1.0),
                        name: Optional[str] = None) -> StartPoint:
        if not 0 <= int(colour) <= 255 or not 0 <= int(index) <= 255:
            raise ValueError('start point colour/index are bytes')
        n = len(self.trn.start_points)
        sp = StartPoint(wstr(name if name is not None else ('StartPoint' if n == 0 else 'StartPoint_%d' % n)),
                        int(colour), int(index), tuple(map(float, position)), _unit_xz(direction))
        self.trn.start_points.append(sp)
        return sp

    def add_item_point(self, item_type: int, position, direction=(0.0, 0.0, 1.0),
                       name: Optional[str] = None) -> int:
        """Returns the item point index used by item patterns."""
        n = len(self.trn.item_points)
        self.trn.item_points.append(ItemPoint(
            wstr(name if name is not None else ('ItemPoint' if n == 0 else 'ItemPoint_%d' % n)),
            int(item_type), tuple(map(float, position)), _unit_xz(direction)))
        return n

    def add_item_pattern(self, team: str, point_indices: Sequence[int]):
        """One item spawn set for `team` ('red' / 'blue'): exactly 8 item point indices (every shipped
        pattern is full; the loader pre-fills records with -1, but partial patterns are untested)."""
        idx = [int(i) for i in point_indices]
        if len(idx) != ITEM_PATTERN_SLOTS or min(idx) < 0:
            raise ValueError('an item pattern needs %d item point indices' % ITEM_PATTERN_SLOTS)
        target = {'red': self.trn.props.item_patterns_red, 'blue': self.trn.props.item_patterns_blue}[team]
        target.append(tuple(idx))

    def add_dynamic_light(self, light: DynamicLight):
        self.trn.dynamic_lights.append(light)

    def add_death_zone(self, position, size, forward=(0.0, 0.0, 1.0), up=(0.0, 1.0, 0.0)):
        self.trn.death_zones.append(DeathZone(self._transform(position, forward, up, size)))

    def add_force_move_pad(self, position, size, direction, speed=60.0, duration=40.0, up=(0.0, 1.0, 0.0)):
        self.trn.force_move_pads.append(ForceMovePad(self._transform(position, direction, up, size),
                                                     float(speed), float(duration)))

    # -- output -----------------------------------------------------------------------------
    def build(self) -> TrnFile:
        t = copy.deepcopy(self.trn)
        if not t.layers:
            raise ValueError('a terrain needs at least one layer')
        if t.normals is None:
            t.normals = compute_normals(t.heights, t.width, t.height, t.scale)
        size, data = self._lightmap or ((256, 256), bytes((240, 240, 240)) * (256 * 256))
        t.lightmap_size, t.lightmap = size, data
        t.sections = section_layers(t)
        if t.attributes is None:
            aw, ah = t.attribute_size
            t.attributes = array('I', bytes(4 * aw * ah))
        n_items = len(t.item_points)
        for pat in (t.props.item_patterns_red or []) + (t.props.item_patterns_blue or []):
            if any(i >= n_items for i in pat):
                raise ValueError('item pattern %r references a missing item point' % (pat,))
        return t

    def to_bytes(self) -> bytes:
        return serialize(self.build())


def _unit_xz(direction) -> tuple:
    x, y, z = (float(c) for c in direction)
    n = math.hypot(x, z)
    if n == 0:
        return (0.0, 0.0, 1.0)
    return (x / n, 0.0, z / n)
