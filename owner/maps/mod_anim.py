"""Action table of CModelObs `.mod` files: locate it, round-trip it, add cutscene labels.

Only the Kudiz animation table is rewritten; the CModel section (geometry, skeleton, materials), the Kudiz
material overrides and the OBB section are carried over as the original bytes.

File layout as read by CModelObs::Load / CModelObsCommon::LoadKudiz (zoa_obs.dll 0x1000CA20)::

    u32 file_version                 0xC6E9 in shipped files; a first u32 < 0xC620 is not a version
                                     (legacy file starting with "ZOA"; then no entry offsets, no OBB section)
    CModel section                   "ZOA" 0 <model_version 3..5> ... (see _skip_model)
    i32 material_count, material_count x 0x144     Kudiz texture/material overrides
    i32 action_count                 <- the action (animation) table
    action_count x {
        i32 end_offset               only when file_version > 0xC6E8: ABSOLUTE file offset just past this
                                     entry; the loader seeks there after Global_Script and when it skips
                                     actions filtered out by the caller, so every value must be exact
        wchar[64] label              NUL padded; the loader trims trailing spaces
        wchar[64] ani_path           e.g. "anrs\\2815.ani"; absent when label == "Global_Script"
        u8 has_events
        CFrameEventSys (has_events != 0):
            i32 resource_count, resource_count x {u32 type, wchar[32] path}
            i32 frame_count, frame_count x {i32 frame (-1 = on start), i32 n, n x AnimEvent[0xAC]}
    }
    i32 obb_count, obb_count x 0x84  (file_version > 0xC61F)

How a label reaches its `.ani`: LoadKudiz pushes label and ani_path into two parallel vectors (and the event
table into a label-keyed map); CModelObsCommon::LoadAnimation then loads every path with
CModelAnimation::Load (a missing or invalid `.ani` makes the whole model's animation load fail) and calls
CModel::AddAnimation(label, animation), which links each `.ani` track to the model node with the same name
(exact, case-sensitive) and refuses a duplicate label. CModelObs::ChangeAni(label, loop, rate, blend,
priority, ...) looks the label up by string. Loop, play rate and blend time are NOT stored in the entry: they
are arguments of the caller (the client's script binding passes rate 1.0 and blend 0.1 s). Per-entry timing
effects (PLAYRATE 17, STOPDELAY 18, HIDE/SHOW 20/21, SOUND 11, effects 2/3/6/7, ...) are AnimEvents in the
optional event table; an entry with has_events = 0 is valid. The shipped editor writes entries sorted by label
(it saves from a std::map); the loader does not depend on the order.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Callable, Sequence

from . import ani_format

ENTRY_SIZE_MIN = 0xC6E9
OBB_SECTION_MIN = 0xC620
VERSION_PREFIX_MIN = 0xC620
GLOBAL_SCRIPT = 'Global_Script'
LABEL_SLOT = 0x80
MAX_LABEL_CHARS = LABEL_SLOT // 2 - 1
KUDIZ_MATERIAL = 0x144
RESOURCE_RECORD = 0x44
EVENT_RECORD = 0xAC
OBB_RECORD = 0x84
VERTEX_STRIDE = {1: 24, 2: 32}

_I32 = struct.Struct('<i')
_NODE_TRS = struct.Struct('<16f3f4f3f16f16fii6f')


class ModAnimError(ValueError):
    pass


@dataclass
class ModelNode:
    """Read-only view of one CModel node (enough to drive the `.ani` builder)."""
    index: int
    name: str
    kind: int                  # 1 mesh, 2 special mesh, 5 dummy, 6 OBB node, ...
    vertex_format: int
    parent: int                # -1 = root; the engine's hierarchy (CModelCommon::MakeHierachy)
    scale: tuple
    rotation: tuple
    translation: tuple
    local_tm: tuple
    world_tm: tuple
    offset_tm: tuple           # inverse(world_tm)
    bones: list                # per-mesh bone name list (empty in shipped segmented unit models)


@dataclass
class ActionEntry:
    label: str
    animation: str | None              # None only for Global_Script
    events: bytes | None = None        # raw CFrameEventSys payload; None => has_events byte is 0
    has_events: int | None = None      # raw flag byte (None => 1 if events else 0)
    end_offset: int | None = None      # value read from the file
    label_slot: bytes | None = None    # raw slots, kept only when not the canonical encoding
    animation_slot: bytes | None = None
    start: int | None = None           # absolute offset the entry was read from (None for new entries)


@dataclass
class ModAnimFile:
    file_version: int | None
    head: bytes                        # version prefix + CModel section + Kudiz materials (verbatim)
    model_offset: int                  # where "ZOA" starts
    model_end: int                     # first byte after the CModel section
    table_offset: int                  # offset of action_count
    actions: list
    tail: bytes                        # OBB section and any trailing bytes (verbatim)
    nodes: list = field(default_factory=list)
    model_version: int = 5
    anomalies: list = field(default_factory=list)   # e.g. stored end offsets that disagree with the data

    @property
    def sized_entries(self) -> bool:
        return (self.file_version or 0) >= ENTRY_SIZE_MIN

    def labels(self) -> list[str]:
        return [a.label for a in self.actions]

    def find(self, label: str) -> ActionEntry:
        for action in self.actions:
            if action.label.rstrip(' ') == label:
                return action
        raise KeyError(label)


# --------------------------------------------------------------------------------------------- strings

def _decode(slot: bytes) -> str:
    for i in range(0, len(slot), 2):
        if slot[i] == 0 and slot[i + 1] == 0:
            return slot[:i].decode('utf-16-le', 'surrogatepass')
    raise ModAnimError('fixed string lacks a NUL terminator')


def _encode(text: str, size: int) -> bytes:
    raw = text.encode('utf-16-le')
    if '\0' in text or len(raw) > size - 2:
        raise ModAnimError('string does not fit %d bytes: %r' % (size, text))
    return raw.ljust(size, b'\0')


def _slot_or_raw(slot: bytes):
    text = _decode(slot)
    try:
        canonical = _encode(text, len(slot))
    except ModAnimError:
        canonical = None
    return text, None if canonical == slot else bytes(slot)


# ------------------------------------------------------------------------------------------- CModel skip

class _Cursor:
    def __init__(self, data: bytes, pos: int = 0):
        self.data, self.pos = data, pos

    def take(self, size: int) -> bytes:
        if size < 0 or self.pos + size > len(self.data):
            raise ModAnimError('section runs past the end of the file at %d' % self.pos)
        chunk = self.data[self.pos:self.pos + size]
        self.pos += size
        return chunk

    def i32(self) -> int:
        return _I32.unpack(self.take(4))[0]

    def count(self, limit: int, what: str) -> int:
        value = self.i32()
        if not 0 <= value <= limit:
            raise ModAnimError('implausible %s count %d' % (what, value))
        return value


def _skip_model(c: _Cursor) -> tuple[int, list]:
    """Walk the CModel section exactly like CZoaBinaryReader (zoa_graphics 0x10021B40) without keeping it."""
    if c.take(3) != b'ZOA':
        raise ModAnimError('missing ZOA model signature')
    _flag, version = c.take(2)
    if version not in (3, 4, 5):
        raise ModAnimError('unsupported model version %d' % version)
    materials = c.count(4096, 'material')
    node_count = c.count(65536, 'node')
    bone_names = c.count(65536, 'bone name')
    c.i32()                                                   # stored mesh count
    names = [_decode(c.take(0x100)) for _ in range(node_count)]
    c.take(bone_names * 0x100 + materials * (0x44 + 0x200))
    nodes = []
    for index, name in enumerate(names):
        kind, vertex_format = c.take(2)
        if version >= 4:
            c.take(4)                                         # render flags
        if version >= 5:
            c.take(4)                                         # cube-map colour
        t = _NODE_TRS.unpack(c.take(_NODE_TRS.size))
        bones = []
        if kind in (1, 2):
            vertices, faces, groups, bone_count = struct.unpack('<4i', c.take(16))
            if min(vertices, faces, groups) < 0 or not 0 <= bone_count <= 4096:
                raise ModAnimError('implausible mesh counts in node %r' % name)
            bones = [_decode(c.take(0x100)) for _ in range(bone_count)]
            c.take(vertices * VERTEX_STRIDE.get(vertex_format, 0) + faces * 6 + groups * 12)
        elif kind == 6:
            c.take(OBB_RECORD)
        nodes.append(ModelNode(index, name, kind, vertex_format, t[59], t[16:19], t[19:23], t[23:26],
                               t[0:16], t[26:42], t[42:58], bones))
    return version, nodes


def _event_payload_end(c: _Cursor) -> None:
    for _ in range(c.count(1 << 16, 'event resource')):
        c.take(RESOURCE_RECORD)
    for _ in range(c.count(1 << 20, 'event frame')):
        c.i32()
        c.take(c.count(1 << 20, 'event') * EVENT_RECORD)


# --------------------------------------------------------------------------------------------- codec

def parse_mod(data: bytes) -> ModAnimFile:
    """Split a `.mod` into verbatim head, parsed action table and verbatim tail."""
    data = bytes(data)
    if len(data) < 8:
        raise ModAnimError('file too short')
    first = _I32.unpack_from(data, 0)[0] & 0xFFFFFFFF
    file_version = first if data[:3] != b'ZOA' and first >= VERSION_PREFIX_MIN else None
    c = _Cursor(data, 4 if file_version is not None else 0)
    model_offset = c.pos
    model_version, nodes = _skip_model(c)
    model_end = c.pos
    c.take(c.count(4096, 'Kudiz material') * KUDIZ_MATERIAL)
    table_offset = c.pos
    sized = (file_version or 0) >= ENTRY_SIZE_MIN
    actions, anomalies = [], []
    for _ in range(c.count(1 << 16, 'action')):
        start = c.pos
        end_offset = c.i32() if sized else None
        label, label_slot = _slot_or_raw(c.take(LABEL_SLOT))
        animation = animation_slot = None
        if label.rstrip(' ') != GLOBAL_SCRIPT:
            animation, animation_slot = _slot_or_raw(c.take(LABEL_SLOT))
        has_events = c.take(1)[0]
        events = None
        if has_events:
            events_start = c.pos
            _event_payload_end(c)
            events = data[events_start:c.pos]
        if sized and end_offset != c.pos:
            # The engine reads entries sequentially and only seeks to end_offset after Global_Script or when
            # a caller filters actions out; keep the stored value (byte-exact) and report it.
            if label.rstrip(' ') == GLOBAL_SCRIPT:
                raise ModAnimError('Global_Script end offset %d differs from its parsed end %d' % (end_offset, c.pos))
            anomalies.append('entry %r stores end offset %d but ends at %d' % (label, end_offset, c.pos))
        actions.append(ActionEntry(label, animation, events, has_events, end_offset, label_slot, animation_slot,
                                   start))
    tail = data[c.pos:]
    if (file_version or 0) >= OBB_SECTION_MIN and len(tail) >= 4:
        obbs = _I32.unpack_from(tail, 0)[0]
        if not 0 <= obbs <= 1 << 16 or 4 + obbs * OBB_RECORD > len(tail):
            raise ModAnimError('OBB section does not fit the file')
        if len(tail) != 4 + obbs * OBB_RECORD:
            anomalies.append('%d bytes follow the OBB section' % (len(tail) - 4 - obbs * OBB_RECORD))
    elif (file_version or 0) >= OBB_SECTION_MIN:
        anomalies.append('OBB section is truncated (%d bytes)' % len(tail))
    return ModAnimFile(file_version, data[:table_offset], model_offset, model_end, table_offset, actions, tail,
                       nodes, model_version, anomalies)


def _entry_bytes(action: ActionEntry, start: int, sized: bool, repair: bool = False) -> bytes:
    body = bytearray(action.label_slot if action.label_slot is not None else _encode(action.label, LABEL_SLOT))
    if action.label.rstrip(' ') != GLOBAL_SCRIPT:
        if action.animation is None:
            raise ModAnimError('action %r needs an .ani path' % action.label)
        body += (action.animation_slot if action.animation_slot is not None
                 else _encode(action.animation, LABEL_SLOT))
    flag = action.has_events if action.has_events is not None else (1 if action.events is not None else 0)
    if bool(flag) != (action.events is not None):
        raise ModAnimError('action %r: has_events flag and event payload disagree' % action.label)
    body.append(flag)
    if action.events is not None:
        body += action.events
    if not sized:
        return bytes(body)
    end = start + 4 + len(body)
    if not repair and action.end_offset is not None and action.start == start:
        end = action.end_offset          # unmoved original entry: keep its stored value verbatim
    return _I32.pack(end) + bytes(body)


def serialize_mod(mod: ModAnimFile, *, repair_offsets: bool = False) -> bytes:
    """Head and tail verbatim; action table rebuilt. New or moved entries get computed absolute end offsets;
    unmoved original entries keep their stored value unless ``repair_offsets`` recomputes all of them."""
    out = bytearray(mod.head)
    out += _I32.pack(len(mod.actions))
    for action in mod.actions:
        out += _entry_bytes(action, len(out), mod.sized_entries, repair_offsets)
    out += mod.tail
    return bytes(out)


# ------------------------------------------------------------------------------------------- editing

def validate_label(label: str) -> None:
    if not label or label != label.strip() or '\0' in label or len(label) > MAX_LABEL_CHARS:
        raise ModAnimError('label must be 1..%d characters without NUL or surrounding spaces' % MAX_LABEL_CHARS)
    if label == GLOBAL_SCRIPT or label == 'Global_is_None_Anim':
        raise ModAnimError('reserved label %r' % label)


def ani_path(number: int) -> str:
    """Canonical resource path the shipped tables use, e.g. ani_path(2815) == 'anrs\\\\2815.ani'."""
    if not 0 <= number <= 99_999_999:
        raise ModAnimError('animation number out of range')
    return 'anrs\\%04d.ani' % number


def add_action(mod: ModAnimFile, label: str, animation: str, *, events: bytes | None = None,
               sorted_position: bool = False, exists: Callable[[str], bool] | None = None) -> ActionEntry:
    """Add ``label`` -> ``animation`` (an `anrs\\NNNN.ani` path). Appends by default so every original
    entry keeps its bytes; ``sorted_position`` mimics the editor's label order instead. ``exists`` lets the
    caller confirm the `.ani` will be present (a missing one breaks loading of all the model's actions)."""
    validate_label(label)
    if any(a.label.rstrip(' ') == label for a in mod.actions):
        raise ModAnimError('label %r already exists (CModel::AddAnimation refuses duplicates)' % label)
    _encode(animation, LABEL_SLOT)
    if not animation.lower().endswith('.ani') or '/' in animation:
        raise ModAnimError('animation path must be a backslash resource path ending in .ani: %r' % animation)
    if exists is not None and not exists(animation):
        raise ModAnimError('animation %r is not available' % animation)
    if events is not None:
        _event_payload_end(_Cursor(events))
    entry = ActionEntry(label, animation, events)
    if sorted_position:
        position = next((i for i, a in enumerate(mod.actions) if _wkey(a.label) > _wkey(label)), len(mod.actions))
        mod.actions.insert(position, entry)
    else:
        mod.actions.append(entry)
    return entry


def _wkey(text: str) -> tuple:
    """std::less<std::wstring> order (UTF-16 code units)."""
    raw = text.encode('utf-16-le', 'surrogatepass')
    return struct.unpack('<%dH' % (len(raw) // 2), raw)


def cutscene_copy(data: bytes, actions: Sequence[tuple[str, str]], **options) -> bytes:
    """Return the bytes of a copy of model ``data`` with extra (label, ani_path) actions; head and tail are
    verified to be byte-identical to the original."""
    mod = parse_mod(data)
    for label, animation in actions:
        add_action(mod, label, animation, **options)
    out = serialize_mod(mod)
    again = parse_mod(out)
    if again.head != mod.head or again.tail != mod.tail or again.labels() != mod.labels():
        raise ModAnimError('rewritten model failed verification')
    return out


# ------------------------------------------------------------------------------------------- skeleton

def skeleton(mod: ModAnimFile) -> list[ani_format.SkeletonBone]:
    """Rest skeleton (engine model space) of every node, in node order, with the model's parents."""
    return [ani_format.SkeletonBone(n.name, n.parent, tuple(n.scale), tuple(n.rotation), tuple(n.translation),
                                    tuple(n.local_tm), tuple(n.world_tm)) for n in mod.nodes]


# ---------------------------------------------------------------------------------------------- events

@dataclass
class AnimEvent:
    """One 0xAC AnimEvent record (layout from CModelObs::ThisEvent/ChildEvent)."""
    event_type: int
    resource: int = -1                       # -1 = the model itself, else index into the resource list
    params: tuple = (0.0, 0.0, 0.0, 0.0)
    extra: bytes = bytes(16)                 # colour / extra words
    reserved: int = 0
    bone: str = ''


EVENT_HIDE, EVENT_SHOW, EVENT_SOUND, EVENT_PLAYRATE, EVENT_STOPDELAY = 20, 21, 11, 17, 18


def build_events(frames: Sequence[tuple[int, Sequence[AnimEvent]]],
                 resources: Sequence[tuple[int, str]] = ()) -> bytes:
    """CFrameEventSys payload: ``resources`` = [(type_id, path)], ``frames`` = [(frame or -1, [events])]."""
    out = bytearray(_I32.pack(len(resources)))
    for type_id, path in resources:
        out += struct.pack('<I', type_id) + _encode(path, 0x40)
    out += _I32.pack(len(frames))
    for frame, events in frames:
        out += struct.pack('<ii', frame, len(events))
        for e in events:
            if len(e.extra) != 16:
                raise ModAnimError('AnimEvent extra must be 16 bytes')
            out += struct.pack('<ii4f', e.resource, e.event_type, *e.params) + e.extra
            out += struct.pack('<I', e.reserved) + _encode(e.bone, 0x80)
    return bytes(out)
