"""Byte-exact reader/writer and static-model builder for ZOA `.mod` files (CModelObs containers).

A `.mod` is read sequentially by `CModelObs::Load` (zoa_obs.dll) with no offset table:

    u32  file_version                0xC6E9 in every shipped file. Present when the first u32 is >= 0xC620;
                                     > 0xC61F enables the OBB section, > 0xC6E8 enables animation entry sizes.
    CModel section                   `ZOA` binary model, read by CZoaBinaryReader (zoa_graphics 0x10021B40),
                                     written by CZoaBinaryWriter (0x100212D0).
    Kudiz section                    CModelObsCommon::LoadKudiz: material overrides + animation table.
    OBB section                      CModelObsCommon::LoadNewFileVersion: collision boxes.

CModel section (all little endian)
----------------------------------
    char[3] 'ZOA', u8 0 (must be 0), u8 model_version (5; 47 shipped files use 4)
    i32 material_count
    i32 node_count                  one name per node in the first name table
    i32 bone_name_count             second name table (skeleton names; 0 for map props)
    i32 mesh_count                  number of kind-1 nodes (stored, not used by the reader)
    wchar[128] node_names[node_count]       0x100 bytes each, NUL padded
    wchar[128] bone_names[bone_name_count]
    material[material_count]        0x244 bytes each:
        D3DMATERIAL9 (0x44): diffuse RGBA, ambient RGBA, specular RGBA, emissive RGBA, f32 power
        wchar[256] texture path (0x200). Only loaded by CModel when textures are requested; CModelObs
        passes false, so in-game textures come from the Kudiz material slots below.
    node[node_count]:
        u8  kind                    1 mesh (CNode mesh), 2 special mesh (0x280 node, not in map files),
                                    3 bone, 4 helper, 5 dummy, 6 OBB node (CNodeOBB), 8 skipped, other: plain node
        u8  vertex_format           0 no vertex data, 1 pos+normal (24 B, FVF 0x12),
                                    2 pos+normal+uv (32 B, FVF 0x112). Kind 6 nodes carry 2.
        i32 flags                   (model_version >= 4) CNode render properties copied to node+0x204:
                                    0x01 two-sided (culling off), 0x04 screen alpha blend, 0x08 additive
                                    blend (when 0x04 clear), 0x10 cube-map reflection stage using
                                    `cubemap_color` as texture factor; 0x40 seen on alpha-cutout foliage
                                    and decals (meaning not traced).
        u32 cubemap_color           (model_version >= 5) D3DCOLOR, 0xFFFFFFFF default; only used with 0x10
                                    (alpha byte = reflection strength, e.g. 0x28000000 on window glass).
        f32[16] local_tm            row-major D3DX matrix (translation in elements 12..14)
        f32[3]  scale, f32[4] rotation quaternion (x, y, z, w), f32[3] position   (decomposed local_tm)
        f32[16] world_tm            node -> model space
        f32[16] offset_tm           observed to be inverse(world_tm)
        i32 index                   own node index (a few exporter files differ; not used for linking)
        i32 parent                  parent node index or -1 (CModelCommon::MakeHierachy)
        f32[3] bbox_min, f32[3] bbox_max   node-local AABB of the vertices; empty helpers use
                                    (+1e6,+1e6,+1e6,-1e6,-1e6,-1e6) (a few `NULL` roots store zeros).
                                    CalculateLocalBounds unions the bboxes of nodes with vertex_format != 0
                                    transformed by world_tm -> model AABB (CSceneObject +0xA4..+0xB8).
        kind 1/2 only:
            i32 vertex_count (must be > 0, else ZoaCriticalError), i32 face_count, i32 group_count,
            i32 bone_count, wchar[128] bones[bone_count]
            vertices: vertex_count * stride (stride from vertex_format; 0 => none stored)
            u16 indices[face_count * 3]
            group[group_count]: u32 start_index (into the index list), u32 face_count, u32 material
        kind 6 only: OBB record (0x84, below)

Kudiz section
-------------
    i32 count (== material_count in all 5198 shipped files; slot i overrides CModel material i)
    per material: wchar[32] textures[4] (0x40 each; empty or "-1" = keep), D3DMATERIAL9 (0x44)
        slot 0 is the diffuse texture (`Txrs\\NNNN.txr`); slots 1..3 are further texture stages.
    i32 animation_count (0 for static map props)
    per animation:
        i32 end_offset              (file_version > 0xC6E8) absolute file offset after the entry
        wchar[64] action name (trailing spaces trimmed by the loader)
        wchar[64] .ani path         omitted when the action is "Global_Script"
        u8  has_events
        if has_events: CFrameEventSys
            i32 resource_count, {u32 type, wchar[32] path} * n     (type 0x10000 model, 0x20003 effect
                                                                      `Fxrs\\*.zfx`, 0x30000 sound `sdrs\\*.zsd`)
            i32 frame_count, {i32 frame, i32 event_count, AnimEvent[0xAC] * event_count} * n

OBB section (file_version > 0xC61F)
-----------------------------------
    i32 count, OBB[count] (0x84 bytes = 33 floats):
        +0x00 center (model space)   +0x0C axis0, axis1, axis2 (unit vectors)   +0x30 size (full edge lengths)
        +0x3C world cache: center, axes, size*scale, half size. Filled at run time by OBB::Transform; every
              shipped record stores the default-constructed values (0,0,0, identity, 1,1,1, .5,.5,.5).
    CModelObs::GenerateOBB copies (center, axes, size) of each record into the scene object's intersect
    OBB array (CSceneObject +0xD4 count / +0xD8 array). Static and destroyable map blocks place the model
    with the .trn position/rotation/scale; unit spheres collide with these OBBs (sphere-OBB overlap and
    contact), and the scene AABB derived from the node bboxes is only the broad phase / culling box.
    A model without OBB records is visible but not solid; ground is the terrain heightmap.

Map usage
---------
    `.trn` LoadObjectInfo static blocks (0x92-byte records, object type 0x11) start with a wchar[32] model
    path (`mdrs\\NNNN.mod`, <= 31 chars) followed by placement vectors and flags; destroyable blocks
    (0xE1-byte records, object type 0x12) carry a wchar[64] path. Each block becomes a CModelObs
    (CModelObs::Load -> GenerateOBB, ChangeAni("default")) placed in the document quadtree (+0x82C4).
    1687 distinct models are referenced by the 154 shipped .trn files; 910 of them carry OBB records.
    Server tables (tbl_MapBlocks AABB per block) are separate data and do not read the .mod.

Fixed-size strings keep any bytes left after the terminator by the original tools in `*_tail` fields so
that `serialize(parse(data)) == data`; builders leave tails empty. Derived values (`mesh_count`,
animation `end_offset`) are stored as read and recomputed when they are None.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import Iterable, Sequence

MOD_FILE_VERSION = 0xC6E9
MODEL_VERSION = 5
VERSION_PREFIX_MIN = 0xC620     # first u32 below this is not a version prefix (loader seeks back)
OBB_SECTION_MIN = 0xC620        # > 0xC61F
ENTRY_SIZE_MIN = 0xC6E9         # > 0xC6E8

NODE_MESH = 1
NODE_SPECIAL_MESH = 2
NODE_BONE = 3
NODE_HELPER = 4
NODE_DUMMY = 5
NODE_OBB = 6

VF_NONE = 0
VF_POS_NORMAL = 1
VF_POS_NORMAL_UV = 2
VERTEX_STRIDE = {VF_NONE: 0, VF_POS_NORMAL: 24, VF_POS_NORMAL_UV: 32}

FLAG_TWO_SIDED = 0x01
FLAG_SCREEN_ALPHA = 0x04
FLAG_ADDITIVE = 0x08
FLAG_CUBEMAP = 0x10

DEFAULT_CUBEMAP_COLOR = 0xFFFFFFFF
NAME_SIZE = 0x100
MATERIAL_TEXTURE_SIZE = 0x200
KUDIZ_TEXTURE_SIZE = 0x40
ANIM_NAME_SIZE = 0x80
RESOURCE_PATH_SIZE = 0x40
ANIM_EVENT_SIZE = 0xAC
OBB_SIZE = 0x84
GLOBAL_SCRIPT = 'Global_Script'
MAX_U16_VERTICES = 0xFFFF

IDENTITY = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
OBB_CACHE_DEFAULT = (0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.5, 0.5, 0.5)
EMPTY_BBOX = ((1e6, 1e6, 1e6), (-1e6, -1e6, -1e6))
DEFAULT_MATERIAL = ((0.588, 0.588, 0.588, 1.0), (0.588, 0.588, 0.588, 1.0), (0.9, 0.9, 0.9, 1.0),
                    (0.0, 0.0, 0.0, 1.0), 0.0)   # the exporter's untextured material 0


class ModFormatError(ValueError):
    pass


# --------------------------------------------------------------------------- fixed strings

def _find_terminator(raw: bytes) -> int:
    start = 0
    while True:
        at = raw.find(b'\0\0', start)
        if at < 0:
            return len(raw)
        if at % 2 == 0:
            return at
        start = at + 1


def decode_wstr(raw: bytes) -> tuple[str, bytes]:
    """Split a NUL-padded UTF-16LE field into its text and the non-zero bytes left after the terminator."""
    end = _find_terminator(raw)
    text = raw[:end].decode('utf-16-le', 'surrogatepass')
    tail = raw[end + 2:].rstrip(b'\0') if end < len(raw) else b''
    return text, tail


def encode_wstr(text: str, size: int, tail: bytes = b'') -> bytes:
    raw = text.encode('utf-16-le', 'surrogatepass')
    if '\0' in text:
        raise ModFormatError(f'string contains NUL: {text!r}')
    if len(raw) < size:
        raw += b'\0\0' + tail
    if len(raw) > size:
        raise ModFormatError(f'string does not fit {size // 2} wchars: {text!r}')
    return raw.ljust(size, b'\0')


# --------------------------------------------------------------------------- records

Vec3 = tuple[float, float, float]
Vec4 = tuple[float, float, float, float]


@dataclass
class Material:
    """CModel material: D3DMATERIAL9 + texture path (0x244 bytes)."""
    diffuse: Vec4 = DEFAULT_MATERIAL[0]
    ambient: Vec4 = DEFAULT_MATERIAL[1]
    specular: Vec4 = DEFAULT_MATERIAL[2]
    emissive: Vec4 = DEFAULT_MATERIAL[3]
    power: float = DEFAULT_MATERIAL[4]
    texture: str = ''
    texture_tail: bytes = b''


@dataclass
class KudizMaterial:
    """OBS material override: four texture slots (wchar[32]) + D3DMATERIAL9 (0x144 bytes)."""
    textures: list[str] = field(default_factory=lambda: ['', '', '', ''])
    texture_tails: list[bytes] = field(default_factory=lambda: [b'', b'', b'', b''])
    diffuse: Vec4 = DEFAULT_MATERIAL[0]
    ambient: Vec4 = DEFAULT_MATERIAL[1]
    specular: Vec4 = DEFAULT_MATERIAL[2]
    emissive: Vec4 = DEFAULT_MATERIAL[3]
    power: float = DEFAULT_MATERIAL[4]


@dataclass
class OBB:
    """Collision box (0x84 bytes). `size` holds full edge lengths along the three axes."""
    center: Vec3 = (0.0, 0.0, 0.0)
    axes: tuple[Vec3, Vec3, Vec3] = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    size: Vec3 = (1.0, 1.0, 1.0)
    cache: tuple[float, ...] = OBB_CACHE_DEFAULT   # 18 floats of run-time world data

    @classmethod
    def from_aabb(cls, bbox_min: Sequence[float], bbox_max: Sequence[float]) -> 'OBB':
        lo = [float(v) for v in bbox_min]
        hi = [float(v) for v in bbox_max]
        if any(h < l for l, h in zip(lo, hi)):
            raise ModFormatError('AABB max is below min')
        return cls(center=tuple((l + h) * 0.5 for l, h in zip(lo, hi)), size=tuple(h - l for l, h in zip(lo, hi)))

    @classmethod
    def from_axes(cls, center: Sequence[float], axes: Sequence[Sequence[float]], size: Sequence[float]) -> 'OBB':
        """Box with explicit unit axes (rows of a rotation matrix) and full edge lengths."""
        unit = []
        for axis in axes:
            length = math.sqrt(sum(float(c) * float(c) for c in axis))
            if not length:
                raise ModFormatError('OBB axis has zero length')
            unit.append(tuple(float(c) / length for c in axis))
        if len(unit) != 3:
            raise ModFormatError('OBB needs three axes')
        return cls(center=tuple(float(c) for c in center), axes=tuple(unit), size=tuple(float(s) for s in size))

    def corners(self) -> list[Vec3]:
        out = []
        for sx in (-0.5, 0.5):
            for sy in (-0.5, 0.5):
                for sz in (-0.5, 0.5):
                    out.append(tuple(self.center[i] + sx * self.size[0] * self.axes[0][i]
                                     + sy * self.size[1] * self.axes[1][i] + sz * self.size[2] * self.axes[2][i]
                                     for i in range(3)))
        return out


@dataclass
class MaterialGroup:
    start_index: int      # offset into the u16 index list (= first face * 3)
    face_count: int
    material: int         # CModel material index


@dataclass
class Mesh:
    vertex_count: int
    vertex_data: bytes = b''      # vertex_count * stride, raw little-endian floats
    index_data: bytes = b''       # face_count * 3 u16
    groups: list[MaterialGroup] = field(default_factory=list)
    bones: list[str] = field(default_factory=list)
    bone_tails: list[bytes] = field(default_factory=list)

    @property
    def face_count(self) -> int:
        return len(self.index_data) // 6

    def vertices(self, vertex_format: int) -> list[tuple[float, ...]]:
        stride = VERTEX_STRIDE[vertex_format]
        if not stride:
            return []
        return [v for v in struct.iter_unpack('<%df' % (stride // 4), self.vertex_data)]

    def faces(self) -> list[tuple[int, int, int]]:
        return list(struct.iter_unpack('<3H', self.index_data))


@dataclass
class Node:
    name: str
    kind: int = NODE_DUMMY
    vertex_format: int = VF_NONE
    flags: int = 0
    cubemap_color: int = DEFAULT_CUBEMAP_COLOR
    local_tm: tuple[float, ...] = IDENTITY
    scale: Vec3 = (1.0, 1.0, 1.0)
    rotation: Vec4 = (0.0, 0.0, 0.0, 1.0)
    position: Vec3 = (0.0, 0.0, 0.0)
    world_tm: tuple[float, ...] = IDENTITY
    offset_tm: tuple[float, ...] = IDENTITY
    index: int = 0
    parent: int = -1
    bbox_min: Vec3 = EMPTY_BBOX[0]
    bbox_max: Vec3 = EMPTY_BBOX[1]
    mesh: Mesh | None = None
    obb: OBB | None = None
    name_tail: bytes = b''


@dataclass
class FrameEvents:
    """CFrameEventSys payload kept as raw records (static map models have none)."""
    resources: list[tuple[int, bytes]] = field(default_factory=list)       # (type, 0x40-byte path)
    frames: list[tuple[int, list[bytes]]] = field(default_factory=list)    # (frame, [0xAC event])


@dataclass
class Animation:
    name: str
    anim_file: str | None = None            # None for Global_Script
    events: FrameEvents | None = None
    events_flag: int = 0                    # raw has_events byte (0 => events is None)
    end_offset: int | None = None           # stored absolute end; None => computed
    name_tail: bytes = b''
    anim_file_tail: bytes = b''


@dataclass
class ModFile:
    materials: list[Material] = field(default_factory=list)
    nodes: list[Node] = field(default_factory=list)
    kudiz_materials: list[KudizMaterial] = field(default_factory=list)
    animations: list[Animation] = field(default_factory=list)
    obbs: list[OBB] | None = field(default_factory=list)   # None: section absent (truncated legacy files)
    bone_names: list[str] = field(default_factory=list)
    bone_name_tails: list[bytes] = field(default_factory=list)
    file_version: int | None = MOD_FILE_VERSION
    model_version: int = MODEL_VERSION
    model_flag: int = 0
    mesh_count: int | None = None           # None => count of kind-1 nodes
    trailing: bytes = b''

    @property
    def is_skinned(self) -> bool:
        return bool(self.bone_names) or any(n.kind == NODE_SPECIAL_MESH or (n.mesh and n.mesh.bones)
                                            for n in self.nodes)

    @property
    def is_static(self) -> bool:
        return not self.animations and not self.is_skinned

    def texture_paths(self) -> list[str]:
        """Every texture referenced by CModel materials, Kudiz slots and OBS texture resources."""
        seen: dict[str, None] = {}
        for material in self.materials:
            if material.texture:
                seen.setdefault(material.texture, None)
        for material in self.kudiz_materials:
            for texture in material.textures:
                if texture and texture != '-1':
                    seen.setdefault(texture, None)
        return list(seen)


# --------------------------------------------------------------------------- parsing

class _Reader:
    __slots__ = ('data', 'pos')

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def need(self, size: int) -> None:
        if size < 0 or self.pos + size > len(self.data):
            raise ModFormatError(f'truncated .mod at 0x{self.pos:X} (need {size} bytes)')

    def take(self, size: int) -> bytes:
        self.need(size)
        out = self.data[self.pos:self.pos + size]
        self.pos += size
        return out

    def unpack(self, fmt: str):
        size = struct.calcsize(fmt)
        self.need(size)
        out = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return out

    def i32(self) -> int:
        return self.unpack('<i')[0]

    def count(self, limit: int, what: str) -> int:
        value = self.i32()
        if not 0 <= value <= limit:
            raise ModFormatError(f'implausible {what} count {value} at 0x{self.pos - 4:X}')
        return value

    def wstr(self, size: int) -> tuple[str, bytes]:
        return decode_wstr(self.take(size))


def _read_material(r: _Reader) -> Material:
    f = r.unpack('<17f')
    texture, tail = r.wstr(MATERIAL_TEXTURE_SIZE)
    return Material(f[0:4], f[4:8], f[8:12], f[12:16], f[16], texture, tail)


def _read_obb(r: _Reader) -> OBB:
    f = r.unpack('<33f')
    return OBB(center=f[0:3], axes=(f[3:6], f[6:9], f[9:12]), size=f[12:15], cache=f[15:33])


def _read_node(r: _Reader, name: str, name_tail: bytes, model_version: int) -> Node:
    kind, vertex_format = r.unpack('<BB')
    flags = r.i32() if model_version >= 4 else 0
    cubemap = r.unpack('<I')[0] if model_version >= 5 else DEFAULT_CUBEMAP_COLOR
    t = r.unpack('<16f3f4f3f16f16fii6f')
    node = Node(name=name, kind=kind, vertex_format=vertex_format, flags=flags, cubemap_color=cubemap,
                local_tm=t[0:16], scale=t[16:19], rotation=t[19:23], position=t[23:26], world_tm=t[26:42],
                offset_tm=t[42:58], index=t[58], parent=t[59], bbox_min=t[60:63], bbox_max=t[63:66],
                name_tail=name_tail)
    if kind in (NODE_MESH, NODE_SPECIAL_MESH):
        vertex_count, face_count, group_count, bone_count = r.unpack('<4i')
        if vertex_count < 0 or face_count < 0 or group_count < 0 or not 0 <= bone_count <= 4096:
            raise ModFormatError(f'implausible mesh counts in node {name!r}')
        bones, tails = [], []
        for _ in range(bone_count):
            bone, tail = r.wstr(NAME_SIZE)
            bones.append(bone)
            tails.append(tail)
        vertex_data = r.take(vertex_count * VERTEX_STRIDE.get(vertex_format, 0))
        index_data = r.take(face_count * 6)
        r.need(group_count * 12)
        groups = [MaterialGroup(*g) for g in struct.iter_unpack('<3I', r.data[r.pos:r.pos + group_count * 12])]
        r.pos += group_count * 12
        node.mesh = Mesh(vertex_count, vertex_data, index_data, groups, bones, tails)
    elif kind == NODE_OBB:
        node.obb = _read_obb(r)
    return node


def _read_events(r: _Reader) -> FrameEvents:
    events = FrameEvents()
    for _ in range(r.count(1 << 16, 'resource')):
        kind = r.unpack('<I')[0]
        events.resources.append((kind, r.take(RESOURCE_PATH_SIZE)))
    for _ in range(r.count(1 << 20, 'frame')):
        frame, event_count = r.unpack('<ii')
        if not 0 <= event_count <= 1 << 20:
            raise ModFormatError('implausible event count')
        raw = r.take(event_count * ANIM_EVENT_SIZE)
        events.frames.append((frame, [raw[i:i + ANIM_EVENT_SIZE] for i in range(0, len(raw), ANIM_EVENT_SIZE)]))
    return events


def parse(data: bytes) -> ModFile:
    """Parse a complete `.mod`; raises ModFormatError for anything the engine could not read."""
    data = bytes(data)
    r = _Reader(data)
    mod = ModFile()
    r.need(4)
    first = struct.unpack_from('<I', data, 0)[0]
    if data[:3] != b'ZOA' and first >= VERSION_PREFIX_MIN:
        mod.file_version = first
        r.pos = 4
    else:
        mod.file_version = None
    gate = mod.file_version or 0
    if r.take(3) != b'ZOA':
        raise ModFormatError('missing ZOA model signature')
    mod.model_flag, mod.model_version = r.unpack('<BB')
    if mod.model_version not in (3, 4, 5):
        raise ModFormatError(f'unsupported ZOA model version {mod.model_version}')
    material_count = r.count(4096, 'material')
    node_count = r.count(65536, 'node')
    bone_count = r.count(65536, 'bone name')
    mod.mesh_count = r.i32()
    names = [r.wstr(NAME_SIZE) for _ in range(node_count)]
    for _ in range(bone_count):
        name, tail = r.wstr(NAME_SIZE)
        mod.bone_names.append(name)
        mod.bone_name_tails.append(tail)
    mod.materials = [_read_material(r) for _ in range(material_count)]
    mod.nodes = [_read_node(r, name, tail, mod.model_version) for name, tail in names]

    for _ in range(r.count(4096, 'Kudiz material')):
        material = KudizMaterial(textures=[], texture_tails=[])
        for _ in range(4):
            text, tail = r.wstr(KUDIZ_TEXTURE_SIZE)
            material.textures.append(text)
            material.texture_tails.append(tail)
        f = r.unpack('<17f')
        material.diffuse, material.ambient, material.specular, material.emissive, material.power = (
            f[0:4], f[4:8], f[8:12], f[12:16], f[16])
        mod.kudiz_materials.append(material)
    for _ in range(r.count(1 << 16, 'animation')):
        end = r.i32() if gate >= ENTRY_SIZE_MIN else None
        name, name_tail = r.wstr(ANIM_NAME_SIZE)
        animation = Animation(name=name, end_offset=end, name_tail=name_tail)
        if name.rstrip(' ') != GLOBAL_SCRIPT:
            animation.anim_file, animation.anim_file_tail = r.wstr(ANIM_NAME_SIZE)
        animation.events_flag = r.unpack('<B')[0]
        if animation.events_flag:
            animation.events = _read_events(r)
        mod.animations.append(animation)
    if gate >= OBB_SECTION_MIN:
        if len(data) - r.pos >= 4:
            mod.obbs = [_read_obb(r) for _ in range(r.count(1 << 16, 'OBB'))]
        else:
            mod.obbs = None
    else:
        mod.obbs = None
    mod.trailing = data[r.pos:]
    return mod


# --------------------------------------------------------------------------- serialization

def _f(values: Iterable[float]) -> bytes:
    values = tuple(values)
    return struct.pack('<%df' % len(values), *values)


def _material_bytes(m: Material | KudizMaterial) -> bytes:
    return _f((*m.diffuse, *m.ambient, *m.specular, *m.emissive, m.power))


def _obb_bytes(obb: OBB) -> bytes:
    if len(obb.cache) != 18:
        raise ModFormatError('OBB cache must have 18 floats')
    return _f((*obb.center, *obb.axes[0], *obb.axes[1], *obb.axes[2], *obb.size, *obb.cache))


def serialize(mod: ModFile, *, normalize: bool = False) -> bytes:
    """Write a `.mod`. Stored derived values are kept unless None or `normalize` is set."""
    out = bytearray()
    if mod.file_version is not None:
        out += struct.pack('<I', mod.file_version)
    gate = mod.file_version or 0
    mesh_count = mod.mesh_count
    if mesh_count is None or normalize:
        mesh_count = sum(1 for n in mod.nodes if n.kind == NODE_MESH)
    out += b'ZOA' + struct.pack('<BB4i', mod.model_flag, mod.model_version, len(mod.materials), len(mod.nodes),
                                len(mod.bone_names), mesh_count)
    for node in mod.nodes:
        out += encode_wstr(node.name, NAME_SIZE, node.name_tail)
    tails = mod.bone_name_tails or [b''] * len(mod.bone_names)
    for name, tail in zip(mod.bone_names, tails):
        out += encode_wstr(name, NAME_SIZE, tail)
    for material in mod.materials:
        out += _material_bytes(material) + encode_wstr(material.texture, MATERIAL_TEXTURE_SIZE, material.texture_tail)
    for node in mod.nodes:
        out += struct.pack('<BB', node.kind, node.vertex_format)
        if mod.model_version >= 4:
            out += struct.pack('<i', node.flags)
        if mod.model_version >= 5:
            out += struct.pack('<I', node.cubemap_color & 0xFFFFFFFF)
        out += _f((*node.local_tm, *node.scale, *node.rotation, *node.position, *node.world_tm, *node.offset_tm))
        out += struct.pack('<ii', node.index, node.parent) + _f((*node.bbox_min, *node.bbox_max))
        if node.kind in (NODE_MESH, NODE_SPECIAL_MESH):
            mesh = node.mesh
            if mesh is None:
                raise ModFormatError(f'mesh node {node.name!r} has no mesh')
            stride = VERTEX_STRIDE.get(node.vertex_format, 0)
            if len(mesh.vertex_data) != mesh.vertex_count * stride or len(mesh.index_data) % 6:
                raise ModFormatError(f'mesh buffers of {node.name!r} do not match its counts')
            out += struct.pack('<4i', mesh.vertex_count, mesh.face_count, len(mesh.groups), len(mesh.bones))
            bone_tails = mesh.bone_tails or [b''] * len(mesh.bones)
            for bone, tail in zip(mesh.bones, bone_tails):
                out += encode_wstr(bone, NAME_SIZE, tail)
            out += mesh.vertex_data + mesh.index_data
            for group in mesh.groups:
                out += struct.pack('<3I', group.start_index, group.face_count, group.material)
        elif node.kind == NODE_OBB:
            out += _obb_bytes(node.obb or OBB())
    out += struct.pack('<i', len(mod.kudiz_materials))
    for material in mod.kudiz_materials:
        if len(material.textures) != 4:
            raise ModFormatError('Kudiz materials have exactly four texture slots')
        slot_tails = material.texture_tails or [b''] * 4
        for text, tail in zip(material.textures, slot_tails):
            out += encode_wstr(text, KUDIZ_TEXTURE_SIZE, tail)
        out += _material_bytes(material)
    out += struct.pack('<i', len(mod.animations))
    for animation in mod.animations:
        size_at = len(out)
        if gate >= ENTRY_SIZE_MIN:
            out += b'\0\0\0\0'
        out += encode_wstr(animation.name, ANIM_NAME_SIZE, animation.name_tail)
        if animation.name.rstrip(' ') != GLOBAL_SCRIPT:
            out += encode_wstr(animation.anim_file or '', ANIM_NAME_SIZE, animation.anim_file_tail)
        flag = animation.events_flag if animation.events is not None else 0
        if animation.events is not None and not flag:
            flag = 1
        out += struct.pack('<B', flag)
        if animation.events is not None:
            events = animation.events
            out += struct.pack('<i', len(events.resources))
            for kind, path in events.resources:
                if len(path) != RESOURCE_PATH_SIZE:
                    raise ModFormatError('event resource path must be 0x40 bytes')
                out += struct.pack('<I', kind) + path
            out += struct.pack('<i', len(events.frames))
            for frame, records in events.frames:
                out += struct.pack('<ii', frame, len(records))
                for record in records:
                    if len(record) != ANIM_EVENT_SIZE:
                        raise ModFormatError('AnimEvent records are 0xAC bytes')
                    out += record
        if gate >= ENTRY_SIZE_MIN:
            end = len(out) if animation.end_offset is None or normalize else animation.end_offset
            struct.pack_into('<i', out, size_at, end)
    if mod.obbs is not None and gate >= OBB_SECTION_MIN:
        out += struct.pack('<i', len(mod.obbs))
        for obb in mod.obbs:
            out += _obb_bytes(obb)
    out += mod.trailing
    return bytes(out)


# --------------------------------------------------------------------------- validation

def validate(mod: ModFile) -> list[str]:
    """Problems the engine would reject or render wrongly (empty list = loadable static model)."""
    problems = []
    if mod.model_flag != 0 or mod.model_version != MODEL_VERSION:
        problems.append('model header must be ZOA\\0 version 5')
    if mod.file_version != MOD_FILE_VERSION:
        problems.append('file version must be 0xC6E9')
    if len(mod.kudiz_materials) != len(mod.materials):
        problems.append('Kudiz material count must equal the CModel material count')
    for i, node in enumerate(mod.nodes):
        where = f'node {i} {node.name!r}'
        if len(node.name.encode('utf-16-le')) >= NAME_SIZE:
            problems.append(f'{where}: name too long')
        if not -1 <= node.parent < len(mod.nodes) or node.parent == i:
            problems.append(f'{where}: invalid parent {node.parent}')
        if node.kind in (NODE_MESH, NODE_SPECIAL_MESH):
            mesh = node.mesh
            if mesh is None or mesh.vertex_count <= 0:
                problems.append(f'{where}: mesh without vertices (engine critical error)')
                continue
            if node.vertex_format not in (VF_POS_NORMAL, VF_POS_NORMAL_UV):
                problems.append(f'{where}: vertex format {node.vertex_format} stores no vertices')
            if mesh.vertex_count > MAX_U16_VERTICES:
                problems.append(f'{where}: more than 65535 vertices')
            if mesh.index_data and max(struct.unpack('<%dH' % (len(mesh.index_data) // 2), mesh.index_data)) >= mesh.vertex_count:
                problems.append(f'{where}: index out of range')
            covered = 0
            for group in mesh.groups:
                if group.material >= len(mod.materials):
                    problems.append(f'{where}: group material {group.material} out of range')
                if group.start_index != covered * 3:
                    problems.append(f'{where}: groups are not contiguous')
                covered += group.face_count
            if covered != mesh.face_count:
                problems.append(f'{where}: groups cover {covered} of {mesh.face_count} faces')
    for i, material in enumerate(mod.kudiz_materials):
        for text in material.textures:
            if len(text.encode('utf-16-le')) >= KUDIZ_TEXTURE_SIZE:
                problems.append(f'Kudiz material {i}: texture path longer than 31 characters')
    for i, obb in enumerate(mod.obbs or []):
        if any(not (s > 0 and math.isfinite(s)) for s in obb.size):
            problems.append(f'OBB {i}: size must be positive')
        for a in range(3):
            length = math.sqrt(sum(c * c for c in obb.axes[a]))
            if abs(length - 1.0) > 1e-3:
                problems.append(f'OBB {i}: axis {a} is not a unit vector')
    return problems


# --------------------------------------------------------------------------- builder

@dataclass
class BuildMaterial:
    """Material for the builder. `texture` is a client path such as 'Txrs\\9001.txr' (<= 31 chars)."""
    texture: str = ''
    diffuse: Vec4 = (1.0, 1.0, 1.0, 1.0)            # defaults: the values of ~75 % of shipped map-prop materials
    ambient: Vec4 = (150 / 255, 150 / 255, 150 / 255, 1.0)
    specular: Vec4 = (0.2, 0.2, 0.2, 1.0)
    emissive: Vec4 = (0.0, 0.0, 0.0, 1.0)
    power: float = 15.0


@dataclass
class BuildMesh:
    """Triangle mesh in model space: D3D left-handed, Y up, uv origin top-left, wrap addressing.

    Front faces satisfy dot(cross(p1 - p0, p2 - p0), normal) > 0 (99.7 % of shipped map faces); from a
    right-handed glTF asset negate Z of positions and normals and swap two indices of every triangle.
    `faces` index the parallel `positions`/`normals`/`uvs` arrays; `face_materials` gives the builder
    material index per face (a single int applies to all faces). `flags` are CNode render flags
    (FLAG_TWO_SIDED for foliage and fences, usually 0).
    """
    name: str
    positions: Sequence[Sequence[float]]
    normals: Sequence[Sequence[float]]
    uvs: Sequence[Sequence[float]]
    faces: Sequence[Sequence[int]]
    face_materials: Sequence[int] | int = 0
    flags: int = 0
    cubemap_color: int = DEFAULT_CUBEMAP_COLOR


def _mesh_chunks(mesh: BuildMesh, materials: Sequence[int]):
    """Yield (faces, face_materials, vertex_ids) chunks with at most 65535 distinct vertices each."""
    order = sorted(range(len(mesh.faces)), key=lambda f: materials[f])
    chunk, used = [], {}
    for f in order:
        face = mesh.faces[f]
        new = sum(1 for v in face if v not in used)
        if len(used) + new > MAX_U16_VERTICES:
            yield chunk, used
            chunk, used = [], {}
        for v in face:
            used.setdefault(v, len(used))
        chunk.append(f)
    if chunk:
        yield chunk, used


def _mesh_node(name: str, mesh: BuildMesh, faces: list[int], remap: dict[int, int], materials: Sequence[int],
               material_map: dict[int, int], index: int) -> Node:
    vertex_ids = sorted(remap, key=remap.get)
    vertex_data = bytearray()
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    for v in vertex_ids:
        p, n, t = mesh.positions[v], mesh.normals[v], mesh.uvs[v]
        vertex_data += struct.pack('<8f', p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1])
        for i in range(3):
            lo[i] = min(lo[i], float(p[i]))
            hi[i] = max(hi[i], float(p[i]))
    index_data = bytearray()
    groups: list[MaterialGroup] = []
    for count, f in enumerate(faces):
        a, b, c = mesh.faces[f]
        index_data += struct.pack('<3H', remap[a], remap[b], remap[c])
        material = material_map[materials[f]]
        if groups and groups[-1].material == material:
            groups[-1].face_count += 1
        else:
            groups.append(MaterialGroup(count * 3, 1, material))
    # bbox in node-local space, rounded outward to float32 so it still contains every vertex
    lo32 = tuple(struct.unpack('<3f', struct.pack('<3f', *lo)))
    hi32 = tuple(struct.unpack('<3f', struct.pack('<3f', *hi)))
    return Node(name=name, kind=NODE_MESH, vertex_format=VF_POS_NORMAL_UV, flags=mesh.flags,
                cubemap_color=mesh.cubemap_color, index=index, parent=-1, bbox_min=lo32, bbox_max=hi32,
                mesh=Mesh(len(vertex_ids), bytes(vertex_data), bytes(index_data), groups))


def build_static_model(meshes: Sequence[BuildMesh], materials: Sequence[BuildMaterial],
                       collision: Sequence[OBB] = (), *, root_name: str = 'NULL') -> ModFile:
    """Compile triangle meshes + collision boxes into a static map-block `.mod`.

    Layout mirrors the shipped map props: node 0 is the `NULL` dummy root (empty bbox), every mesh is a
    root-level kind-1 node with identity transforms and pos+normal+uv vertices, CModel material 0 is the
    exporter's untextured default followed by the supplied materials, the Kudiz table repeats every material
    with its texture in slot 0, there are no animations, and `collision` becomes the OBB section. Meshes
    with more than 65535 distinct vertices are split into several nodes (`name`, `name_1`, ...).
    """
    if not meshes:
        raise ModFormatError('a model needs at least one mesh')
    mod = ModFile()
    mod.materials.append(Material())
    mod.kudiz_materials.append(KudizMaterial())
    for material in materials:
        for text, size in ((material.texture, KUDIZ_TEXTURE_SIZE),):
            if len(text.encode('utf-16-le')) >= size:
                raise ModFormatError(f'texture path longer than 31 characters: {text!r}')
        mod.materials.append(Material(material.diffuse, material.ambient, material.specular, material.emissive,
                                      material.power, material.texture))
        mod.kudiz_materials.append(KudizMaterial(textures=[material.texture, '', '', ''],
                                                 texture_tails=[b'', b'', b'', b''], diffuse=material.diffuse,
                                                 ambient=material.ambient, specular=material.specular,
                                                 emissive=material.emissive, power=material.power))
    mod.nodes.append(Node(name=root_name))
    material_map = {i: i + 1 for i in range(len(materials))}
    for mesh in meshes:
        count = len(mesh.faces)
        if not count:
            raise ModFormatError(f'mesh {mesh.name!r} has no faces')
        if not (len(mesh.positions) == len(mesh.normals) == len(mesh.uvs)):
            raise ModFormatError(f'mesh {mesh.name!r}: positions, normals and uvs differ in length')
        face_materials = [mesh.face_materials] * count if isinstance(mesh.face_materials, int) else list(mesh.face_materials)
        if len(face_materials) != count or any(m not in material_map for m in face_materials):
            raise ModFormatError(f'mesh {mesh.name!r}: invalid face materials')
        for face in mesh.faces:
            if len(face) != 3 or any(not 0 <= v < len(mesh.positions) for v in face):
                raise ModFormatError(f'mesh {mesh.name!r}: invalid face {tuple(face)}')
        for part, (faces, remap) in enumerate(_mesh_chunks(mesh, face_materials)):
            name = mesh.name if part == 0 else f'{mesh.name}_{part}'
            mod.nodes.append(_mesh_node(name, mesh, faces, remap, face_materials, material_map, len(mod.nodes)))
    mod.obbs = [OBB(o.center, o.axes, o.size) for o in collision]
    mod.mesh_count = None
    problems = validate(mod)
    if problems:
        raise ModFormatError('; '.join(problems))
    return mod


def build_static_mod(meshes: Sequence[BuildMesh], materials: Sequence[BuildMaterial],
                     collision: Sequence[OBB] = (), **options) -> bytes:
    return serialize(build_static_model(meshes, materials, collision, **options))


def model_bounds(mod: ModFile) -> tuple[Vec3, Vec3] | None:
    """Model-space AABB the engine derives (CalculateLocalBounds): node bboxes through world_tm."""
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    found = False
    for node in mod.nodes:
        if not node.vertex_format:
            continue
        m = node.world_tm
        for x in (node.bbox_min[0], node.bbox_max[0]):
            for y in (node.bbox_min[1], node.bbox_max[1]):
                for z in (node.bbox_min[2], node.bbox_max[2]):
                    p = [x * m[0 + c] + y * m[4 + c] + z * m[8 + c] + m[12 + c] for c in range(3)]
                    for c in range(3):
                        lo[c] = min(lo[c], p[c])
                        hi[c] = max(hi[c], p[c])
                    found = True
    return (tuple(lo), tuple(hi)) if found else None


def collision_from_bounds(mod: ModFile) -> list[OBB]:
    """Fallback collision: one axis-aligned box around the whole model (prefer authored boxes)."""
    bounds = model_bounds(mod)
    return [OBB.from_aabb(*bounds)] if bounds else []
