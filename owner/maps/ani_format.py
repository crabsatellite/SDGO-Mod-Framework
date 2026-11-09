"""ZOA skeletal animation (`.ani`, version 3): exact codec, engine-faithful sampler and keyframe builder.

Layout (little-endian), as read by zoa_graphics.dll's binary animation loader (vtable slot +0x14 of the
loader built by CModelAnimation::Load; the loader rejects anything but ``ZOA`` ``\\0`` ``03``)::

    char[3]  "ZOA"   u8 0   u8 version (= 3)
    i32      num_frames          last frame index; playback runs 0..num_frames at 30 frames per second
    i32      bone_count
    bone_count x wchar[128]      bone names, UTF-16LE, NUL-terminated, zero padded (256 bytes)
    bone_count x {
        f32[16] local_tm         rest local matrix, D3D row-vector (v' = v * M), row 3 = translation
        f32[3]  scale            rest local scale        } the rest S/R/T are used only when a channel
        f32[4]  rotation         rest local quaternion   } has no keys and the engine blends/transitions
        f32[3]  translation      rest local translation  }
        f32[16] world_tm         rest world matrix (tooling only; the engine does not use it for playback)
        i32 n_scale, i32 n_rotation, i32 n_translation
        n_scale       x { u16 start, u16 end, f32[3] }
        n_rotation    x { u16 start, u16 end, f32[4] (x, y, z, w) }
        n_translation x { u16 start, u16 end, f32[3] }
    }

There is no compression, frame-rate field, trailer or per-file hierarchy: tracks bind to a model's nodes by
exact (case-sensitive) name through CModel::LinkAnimation, and parenting comes from the model.

Playback semantics (CNodeAnimation evaluation, zoa_graphics.dll 0x1001B530):

* a key holds its value over the inclusive frame range [start, end]; between one key's ``end`` and the next
  key's ``start`` the engine interpolates (linear for scale/translation, D3DXQuaternionSlerp for rotation)
  using fractional frame time, so contiguous per-frame keys play smoothly at any frame rate;
* before the first key the first value is used, after the last key the last value is held;
* if a bone has rotation keys its local matrix is S*R*T from the keys (missing scale -> 1, missing
  translation -> 0!); without rotation keys the file's ``local_tm`` is used as is;
* model nodes that are absent from the file keep the model's own bind pose (.mod local matrix);
* world = local * parent_world (row vectors), parent taken from the model's node table.

Space: model space of the unit (Y up, D3D left-handed, units are the .mod's units, a typical SD unit is about
16 units tall before the OBS Global_Script scale). Units face -Z with their left side on +X, so a glTF asset
(right-handed, faces +Z, left on +X) maps to it by mirroring Z: see :func:`engine_trs_from_gltf`.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import Iterable, Mapping, Sequence

MAGIC = b'ZOA\0'
VERSION = 3
FPS = 30.0
NAME_SLOT = 256
MAX_NAME_CHARS = NAME_SLOT // 2 - 1
REST_BYTES = 168
MAX_FRAME = 32767            # key frames are read back as signed 16-bit values by the evaluator
MAX_BONES = 4096
MAX_KEYS = 1 << 20

_HEADER = struct.Struct('<3sBBii')
_REST = struct.Struct('<16f3f4f3f16f')
_COUNTS = struct.Struct('<3i')
_KEY3 = struct.Struct('<HH3f')
_KEY4 = struct.Struct('<HH4f')
IDENTITY = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)


class AniFormatError(ValueError):
    pass


@dataclass
class Key:
    start: int
    end: int
    value: tuple


@dataclass
class AniBone:
    name: str
    local_tm: tuple = IDENTITY
    scale: tuple = (1.0, 1.0, 1.0)
    rotation: tuple = (0.0, 0.0, 0.0, 1.0)
    translation: tuple = (0.0, 0.0, 0.0)
    world_tm: tuple = IDENTITY
    scale_keys: list = field(default_factory=list)
    rotation_keys: list = field(default_factory=list)
    translation_keys: list = field(default_factory=list)
    name_slot: bytes | None = None    # raw 256-byte slot, kept only when it is not the canonical encoding


@dataclass
class AniFile:
    num_frames: int
    bones: list
    version: int = VERSION

    @property
    def duration(self) -> float:
        return self.num_frames / FPS

    def bone(self, name: str) -> AniBone:
        for bone in self.bones:
            if bone.name == name:
                return bone
        raise KeyError(name)


# --------------------------------------------------------------------------------------------------- codec

def _name_slot(name: str) -> bytes:
    raw = name.encode('utf-16-le')
    if '\0' in name or len(name) > MAX_NAME_CHARS:
        raise AniFormatError('bone name must be 1..%d characters without NUL: %r' % (MAX_NAME_CHARS, name))
    return raw.ljust(NAME_SLOT, b'\0')


def _decode_slot(slot: bytes) -> str:
    for i in range(0, len(slot), 2):
        if slot[i] == 0 and slot[i + 1] == 0:
            return slot[:i].decode('utf-16-le', 'surrogatepass')
    raise AniFormatError('bone name slot lacks a NUL terminator')


def _keys(data, pos, count, unpack, size):
    if not 0 <= count <= MAX_KEYS or pos + count * size > len(data):
        raise AniFormatError('key table exceeds the file')
    keys = []
    for _ in range(count):
        row = unpack(data, pos)
        keys.append(Key(row[0], row[1], tuple(row[2:])))
        pos += size
    return keys, pos


def parse_ani(data: bytes) -> AniFile:
    """Parse an `.ani` exactly as the engine does; the whole buffer must be consumed."""
    if len(data) < _HEADER.size:
        raise AniFormatError('file is shorter than the header')
    magic, nul, version, num_frames, bone_count = _HEADER.unpack_from(data, 0)
    if magic + bytes([nul]) != MAGIC:
        raise AniFormatError('not a ZOA file')
    if version != VERSION:
        raise AniFormatError('unsupported .ani version %d (engine accepts 3 only)' % version)
    if not 0 <= bone_count <= MAX_BONES:
        raise AniFormatError('bone count %d is out of range' % bone_count)
    pos = _HEADER.size
    if pos + bone_count * NAME_SLOT > len(data):
        raise AniFormatError('name table exceeds the file')
    bones = []
    for i in range(bone_count):
        slot = data[pos:pos + NAME_SLOT]
        name = _decode_slot(slot)
        bones.append(AniBone(name, name_slot=None if _name_slot_or_none(name) == slot else bytes(slot)))
        pos += NAME_SLOT
    for bone in bones:
        if pos + REST_BYTES + _COUNTS.size > len(data):
            raise AniFormatError('bone block exceeds the file')
        rest = _REST.unpack_from(data, pos)
        bone.local_tm, bone.scale, bone.rotation = rest[0:16], rest[16:19], rest[19:23]
        bone.translation, bone.world_tm = rest[23:26], rest[26:42]
        pos += REST_BYTES
        n_scale, n_rot, n_pos = _COUNTS.unpack_from(data, pos)
        pos += _COUNTS.size
        bone.scale_keys, pos = _keys(data, pos, n_scale, _KEY3.unpack_from, _KEY3.size)
        bone.rotation_keys, pos = _keys(data, pos, n_rot, _KEY4.unpack_from, _KEY4.size)
        bone.translation_keys, pos = _keys(data, pos, n_pos, _KEY3.unpack_from, _KEY3.size)
    if pos != len(data):
        raise AniFormatError('%d trailing bytes after the last bone' % (len(data) - pos))
    return AniFile(num_frames, bones, version)


def _name_slot_or_none(name: str) -> bytes | None:
    try:
        return _name_slot(name)
    except AniFormatError:
        return None


def serialize_ani(ani: AniFile) -> bytes:
    if ani.version != VERSION:
        raise AniFormatError('only version 3 can be written')
    out = bytearray(_HEADER.pack(b'ZOA', 0, ani.version, ani.num_frames, len(ani.bones)))
    for bone in ani.bones:
        slot = bone.name_slot if bone.name_slot is not None else _name_slot(bone.name)
        if len(slot) != NAME_SLOT:
            raise AniFormatError('raw name slot must be 256 bytes')
        out += slot
    for bone in ani.bones:
        out += _REST.pack(*bone.local_tm, *bone.scale, *bone.rotation, *bone.translation, *bone.world_tm)
        out += _COUNTS.pack(len(bone.scale_keys), len(bone.rotation_keys), len(bone.translation_keys))
        for keys, packer in ((bone.scale_keys, _KEY3), (bone.rotation_keys, _KEY4),
                             (bone.translation_keys, _KEY3)):
            for key in keys:
                out += packer.pack(key.start, key.end, *key.value)
    return bytes(out)


def validate_ani(ani: AniFile) -> list[str]:
    """Return problems that would make the engine misplay a file we built (not used for game files)."""
    problems = []
    if not 1 <= ani.num_frames <= MAX_FRAME:
        problems.append('num_frames must be 1..%d' % MAX_FRAME)
    seen = set()
    for bone in ani.bones:
        if bone.name in seen:
            problems.append('duplicate bone name %r' % bone.name)
        seen.add(bone.name)
        if _name_slot_or_none(bone.name) is None:
            problems.append('invalid bone name %r' % bone.name)
        channels = (('scale', bone.scale_keys), ('rotation', bone.rotation_keys),
                    ('translation', bone.translation_keys))
        if bone.rotation_keys and not (bone.scale_keys and bone.translation_keys):
            problems.append('%s: rotation keys without scale/translation keys play as scale 1 / '
                            'translation 0' % bone.name)
        for label, keys in channels:
            previous_end = -1
            for key in keys:
                if not (0 <= key.start <= key.end <= MAX_FRAME) or key.start <= previous_end:
                    problems.append('%s %s keys must be ordered, non-overlapping frame ranges' % (bone.name, label))
                    break
                if not all(math.isfinite(v) for v in key.value):
                    problems.append('%s %s key holds a non-finite value' % (bone.name, label))
                    break
                previous_end = key.end
            if keys and (keys[0].start != 0 or keys[-1].end != ani.num_frames):
                problems.append('%s %s keys must cover frames 0..%d' % (bone.name, label, ani.num_frames))
    return problems


# ------------------------------------------------------------------------------------------------ math

def quat_normalize(q: Sequence[float]) -> tuple:
    n = math.sqrt(sum(c * c for c in q))
    if n < 1e-12:
        raise AniFormatError('zero-length quaternion')
    return tuple(c / n for c in q)


def compose(scale: Sequence[float], rotation: Sequence[float], translation: Sequence[float]) -> tuple:
    """S * R * T as a D3D row-vector matrix (D3DXMatrixRotationQuaternion layout)."""
    x, y, z, w = rotation
    sx, sy, sz = scale
    r = ((1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)),
         (2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)),
         (2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)))
    return (sx * r[0][0], sx * r[0][1], sx * r[0][2], 0.0,
            sy * r[1][0], sy * r[1][1], sy * r[1][2], 0.0,
            sz * r[2][0], sz * r[2][1], sz * r[2][2], 0.0,
            translation[0], translation[1], translation[2], 1.0)


def mat_mul(a: Sequence[float], b: Sequence[float]) -> tuple:
    """Row-major a * b (child_local * parent_world gives child_world)."""
    return tuple(sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4))


def _lerp(a, b, u):
    return tuple(x + (y - x) * u for x, y in zip(a, b))


def slerp(a: Sequence[float], b: Sequence[float], u: float) -> tuple:
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0:
        b, dot = tuple(-c for c in b), -dot
    if dot > 0.9995:
        return quat_normalize(_lerp(a, b, u))
    theta = math.acos(min(1.0, dot))
    s = math.sin(theta)
    wa, wb = math.sin((1 - u) * theta) / s, math.sin(u * theta) / s
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def evaluate_channel(keys: Sequence[Key], frame: float, rotation: bool = False):
    """Value of one channel at a (fractional) frame, following the engine's key search exactly."""
    if not keys:
        return None
    if frame > keys[-1].end:
        return keys[-1].value
    previous = None
    for key in keys:
        if frame <= key.end:
            if previous is None or key.start <= frame:
                return key.value
            u = (frame - previous.end) / (key.start - previous.end)
            return slerp(previous.value, key.value, u) if rotation else _lerp(previous.value, key.value, u)
        previous = key
    return keys[-1].value


def evaluate_local(bone: AniBone, frame: float) -> tuple:
    """Local matrix the engine's single-track Animate path produces for this bone at ``frame``."""
    if not bone.rotation_keys:
        return tuple(bone.local_tm)
    scale = evaluate_channel(bone.scale_keys, frame) or (1.0, 1.0, 1.0)
    rotation = evaluate_channel(bone.rotation_keys, frame, True)
    translation = evaluate_channel(bone.translation_keys, frame) or (0.0, 0.0, 0.0)
    return compose(scale, rotation, translation)


# --------------------------------------------------------------------------------------- coordinate space

def engine_trs_from_gltf(translation=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0, 1.0), scale=(1.0, 1.0, 1.0),
                         units: float = 1.0) -> tuple:
    """Map one glTF local TRS (right-handed, Y up, asset faces +Z) to engine model space (Z mirrored).

    ``units`` is engine units per glTF unit; keep 1.0 when animating a skeleton exported by
    :func:`gltf_nodes_from_skeleton`, which is already in engine units.
    """
    tx, ty, tz = translation
    qx, qy, qz, qw = rotation
    return ((tx * units, ty * units, -tz * units), (-qx, -qy, qz, qw), tuple(scale))


def gltf_trs_from_engine(translation, rotation, scale, units: float = 1.0) -> tuple:
    """Inverse of :func:`engine_trs_from_gltf` (the mirror is its own inverse)."""
    tx, ty, tz = translation
    qx, qy, qz, qw = rotation
    return ((tx / units, ty / units, -tz / units), (-qx, -qy, qz, qw), tuple(scale))


# ------------------------------------------------------------------------------------------------ builder

@dataclass
class SkeletonBone:
    """Rest pose of one node in engine model space (local to ``parent``; -1 = root).

    ``local_tm`` / ``world_tm`` are the exact rest matrices when known (from the `.mod`); otherwise they are
    composed from scale/rotation/translation.
    """
    name: str
    parent: int
    scale: tuple = (1.0, 1.0, 1.0)
    rotation: tuple = (0.0, 0.0, 0.0, 1.0)
    translation: tuple = (0.0, 0.0, 0.0)
    local_tm: tuple | None = None
    world_tm: tuple | None = None


@dataclass
class BoneTrack:
    """Authored keys for one bone: sequences of ``(time_seconds, value)``; ``None`` = keep rest value.

    ``interpolation`` follows glTF: ``LINEAR`` (lerp / shortest slerp) or ``STEP``. CUBICSPLINE input must
    be baked by the caller first.
    """
    translation: Sequence | None = None
    rotation: Sequence | None = None
    scale: Sequence | None = None
    interpolation: str = 'LINEAR'


def gltf_nodes_from_skeleton(skeleton: Sequence[SkeletonBone]) -> list[dict]:
    """glTF node dicts (rest pose, engine units, Z mirrored) so authors animate the exact bone frames."""
    nodes = []
    for bone in skeleton:
        t, r, s = gltf_trs_from_engine(bone.translation, bone.rotation, bone.scale)
        nodes.append({'name': bone.name, 'translation': list(t), 'rotation': list(r), 'scale': list(s)})
    for index, bone in enumerate(skeleton):
        if bone.parent >= 0:
            nodes[bone.parent].setdefault('children', []).append(index)
    return nodes


def _sample(keys: Sequence, time: float, interpolation: str, rotation: bool):
    if time <= keys[0][0]:
        return tuple(keys[0][1])
    if time >= keys[-1][0]:
        return tuple(keys[-1][1])
    for (t0, v0), (t1, v1) in zip(keys, keys[1:]):
        if t0 <= time <= t1:
            if interpolation == 'STEP' or t1 == t0:
                return tuple(v1) if time >= t1 else tuple(v0)
            u = (time - t0) / (t1 - t0)
            return slerp(v0, v1, u) if rotation else _lerp(v0, v1, u)
    return tuple(keys[-1][1])


def _f32(value: Iterable[float]) -> tuple:
    return struct.unpack('<%df' % len(tuple(value)), struct.pack('<%df' % len(tuple(value)), *value))


def _ranges(values: Sequence[tuple]) -> list[Key]:
    keys = []
    for frame, value in enumerate(values):
        if keys and keys[-1].value == value:
            keys[-1].end = frame
        else:
            keys.append(Key(frame, frame, value))
    return keys


def _check_track(name, label, keys, width):
    if keys is None:
        return None
    rows = sorted(((float(t), tuple(float(c) for c in v)) for t, v in keys), key=lambda row: row[0])
    if not rows:
        raise AniFormatError('%s %s track is empty' % (name, label))
    for t, v in rows:
        if len(v) != width or not math.isfinite(t) or t < 0 or not all(math.isfinite(c) for c in v):
            raise AniFormatError('%s %s key must be (time >= 0, %d finite numbers)' % (name, label, width))
    return rows


def build_ani(skeleton: Sequence[SkeletonBone], tracks: Mapping[str, BoneTrack], *,
              num_frames: int | None = None, bones: Sequence[str] | None = None) -> AniFile:
    """Bake authored keys into an engine `.ani` for ``skeleton`` (rest pose and parents in engine space).

    Key times are seconds. Each animated bone gets all three channels (a channel without keys keeps its
    rest value; the engine would otherwise play scale 1 / translation 0) sampled at every integer frame
    0..num_frames of the engine's fixed 30 frame-per-second clock (default num_frames: last key time rounded
    up); runs of identical float32 values merge into one key range like the shipped exporter.

    ``bones`` selects and orders the written bones; by default only the animated ones (in skeleton order)
    are written, so every other model node keeps the model's exact bind pose in game. A listed bone without
    a track is written with no keys and its exact rest ``local_tm`` (the engine then uses that matrix).
    """
    index = {bone.name: i for i, bone in enumerate(skeleton)}
    if len(index) != len(skeleton):
        raise AniFormatError('skeleton bone names must be unique')
    unknown = sorted(set(tracks) - set(index))
    if unknown:
        raise AniFormatError('tracks name bones missing from the skeleton: %s' % ', '.join(unknown))
    checked = {}
    last_time = 0.0
    for name, track in tracks.items():
        if track.interpolation not in ('LINEAR', 'STEP'):
            raise AniFormatError('%s: interpolation must be LINEAR or STEP (bake CUBICSPLINE first)' % name)
        rows = (_check_track(name, 'translation', track.translation, 3),
                _check_track(name, 'rotation', track.rotation, 4),
                _check_track(name, 'scale', track.scale, 3))
        if rows[1]:
            rows = (rows[0], [(t, quat_normalize(v)) for t, v in rows[1]], rows[2])
        checked[name] = rows
        last_time = max([last_time] + [r[-1][0] for r in rows if r])
    if num_frames is None:
        num_frames = max(1, int(math.ceil(last_time * FPS - 1e-6)))
    if not 1 <= num_frames <= MAX_FRAME:
        raise AniFormatError('num_frames must be 1..%d' % MAX_FRAME)
    order = list(bones) if bones is not None else [bone.name for bone in skeleton if bone.name in tracks]
    if not order or len(set(order)) != len(order) or any(name not in index for name in order):
        raise AniFormatError('written bones must be a non-empty list of unique skeleton names')
    missing = sorted(set(tracks) - set(order))
    if missing:
        raise AniFormatError('animated bones must be written: %s' % ', '.join(missing))

    rest_local, rest_world = {}, {}

    def local(i):
        if i not in rest_local:
            bone = skeleton[i]
            rest_local[i] = tuple(bone.local_tm) if bone.local_tm is not None else compose(
                bone.scale, quat_normalize(bone.rotation), bone.translation)
        return rest_local[i]

    def world(i):
        if i not in rest_world:
            bone = skeleton[i]
            if bone.world_tm is not None:
                rest_world[i] = tuple(bone.world_tm)
            else:
                rest_world[i] = local(i) if bone.parent < 0 else mat_mul(local(i), world(bone.parent))
        return rest_world[i]

    times = [frame / FPS for frame in range(num_frames + 1)]
    out = []
    for name in order:
        bone = skeleton[index[name]]
        rest_q = quat_normalize(bone.rotation)
        rest = (tuple(bone.scale), rest_q, tuple(bone.translation))
        channels = [[], [], []]
        if name in tracks:
            translation_rows, rotation_rows, scale_rows = checked[name]
            interpolation = tracks[name].interpolation
            channels = []
            for rows, default, is_rot in ((scale_rows, rest[0], False), (rotation_rows, rest[1], True),
                                          (translation_rows, rest[2], False)):
                if rows is None:
                    channels.append([Key(0, num_frames, _f32(default))])
                    continue
                values = [_sample(rows, t, interpolation, is_rot) for t in times]
                if is_rot:
                    for i in range(1, len(values)):   # keep neighbours on one hemisphere for the engine slerp
                        if sum(a * b for a, b in zip(values[i - 1], values[i])) < 0:
                            values[i] = tuple(-c for c in values[i])
                channels.append(_ranges([_f32(v) for v in values]))
        out.append(AniBone(name, local_tm=_f32(local(index[name])), scale=_f32(rest[0]), rotation=_f32(rest_q),
                           translation=_f32(rest[2]), world_tm=_f32(world(index[name])),
                           scale_keys=channels[0], rotation_keys=channels[1], translation_keys=channels[2]))
    ani = AniFile(num_frames, out)
    problems = validate_ani(ani)
    if problems:
        raise AniFormatError('; '.join(problems))
    return ani


def skeleton_from_ani(ani: AniFile) -> list[SkeletonBone]:
    """Rest skeleton of an existing `.ani` (parents inferred from its rest world matrices)."""
    result = []
    for i, bone in enumerate(ani.bones):
        parent, best = -1, 1e-3
        for j in range(i):
            error = max(abs(a - b) for a, b in zip(mat_mul(bone.local_tm, ani.bones[j].world_tm), bone.world_tm))
            if error < best:
                parent, best = j, error
        result.append(SkeletonBone(bone.name, parent, tuple(bone.scale), tuple(bone.rotation), tuple(bone.translation)))
    return result
