"""Compile a neutral mission description (`sdgo.map-mission` v1/v2) into server mission rows, client mirror records
and a text Lua script for cutscenes and phase events.

The mission runs on a map row of the map package (normally its ``mission`` variant). Server semantics follow
the native BattleServer: phases are EventOrder 1..8, a phase completes when all its goal rows with the same
EventNext are satisfied, the fail list ends the mission, spawns are FIFO queues per side and phase.

Mission JSON (all coordinates in the map's world frame, seconds for times)::

    {"schema": "sdgo.map-mission", "schema_version": 1, "mission_key": "author.map.mission",
     "name": {"zh": ...}, "description": {"zh": ...}, "briefing": {"zh": [up to 5 lines]},
     "difficulties": [{"level": 3, "required_rank": 20, "time_limit": 900, "lives": 5}],
     "bgm": {"native": "BGM\\\\0036.mp3"},
     "npcs": {key: {"template": id} | {"template": id, "ai": name, "hp": n, "boss": bool, "name": {...}}
                   | {"unit": unit_id, "base": template_id, "hp": n, "ai": name, "name": {...}}
                   (+ optional "label": overhead name, "attack" / "special_attack": weapon damage multipliers,
                    "cutin": {"native_sprite": "sprs\\\\NNNN.spr" | "image": 1024x156 PNG,
                              "line": dialogue key})},
     "area": [left x, top z, right x, bottom z] (the map compiler paints every movable area's edge on the ground),
     "presentation": {"list_banner", "room_icon", "room_poster", "title", "loading": PNG paths,
                      "loading_screen": "poster" | "random"},
     "lines": {key: {"portrait": "sprs\\\\NNNN.spr", "speaker": {"zh"}, "text": {"zh"}}},
     "banners": {key: {"image": "images/x.png", "size": [w, h]}},
     "sectors": {key: {"warp": [[x,y,z] per player slot], "facing": [dx, dz], "rect": [x1, z1, x2, z2]}},
     "phases": [{"key", "objective": {"zh"}, "slots": {"ally", "enemy"},
                 "spawns": [{"npc", "side": "enemy"|"ally", "positions": [[x,y,z]...], "at": s, "min_free": n}],
                 "goal": {"kill": [{"npc", "count"}...]} | {"survive": s},
                 "fail": [{"time_limit": s} | {"lose": {"npc", "count"}}],
                 "enter": {"cutscene": key, "lines": [keys], "sector": key}, "reward": {"points", "unit_exp"}}],
     "opening": cutscene key, "ending": cutscene key,
     "cutscenes": {key: {"length": s, "actors": {name: {"npc": key, "at": [x,y,z], "facing": [dx,dz]}},
                         "camera": [{"t", "pos", "look"}...],
                         "events": [{"t", "actor", "anim", "loop", "move_to"} | {"t", "banner", "at"?, "distance"?}
                                    | {"t", "shake": [amp, sec]} | {"t", "effect": {"fx", "at", "scale"}}
                                    | {"t", "sound"} | {"t", "line"}]}}}
"""
from __future__ import annotations

import hashlib
import json
import math
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from .package import Package, MapPackageError

SCHEMA = 'sdgo.map-mission'
GAME_MODE_KIND, CLEAR_KIND, NPC_TEMPLATE_KIND = 0x2A, 0x2F, 0x29
CATALOGUE_KIND, ROUTE_KIND, OBJECTIVE_KIND, SPEECH_KIND, SPAWN_KIND = 0x1D, 0x1F, 0x20, 0x36, 0x2E
UNIT_KIND, UNIT_RECORD_BYTES = 0x02, 4976
WEAPON_KIND, WEAPON_RECORD_BYTES = 0x0A, 1871
UNIT_LABEL_FIRST = 9_600_000      # derived unit copies; damage-changing copies are paired on the server
WEAPON_COPY_FIRST = 9_700_000     # paired client/server weapon copies for an NPC's damage multiplier
UNIT_WEAPON_SLOTS = (585, 589, 593)   # unit record: weapons 1..3 (u32 ids); +597 is the special attack
UNIT_SPECIAL_SLOT = 597
WEAPON_DAMAGE = 108                   # weapon record: DamageSize (u32)
SERVER_UNIT_NPC_COLUMNS = tuple(('UnitNumber CustomNumber Influence1 Influence2 UnitModelName UnitGradeNumber '
    'UnitGradeNumber2 MasteredWeaponType BattleCost Star UnitNumber_Transform HP Shield ShieldType ShieldRate '
    'DashType MoveLevel DashLevel QuickStepLevel MotivePower BoosterIncrease BoosterDecrease FatalAttackIncrease '
    'AdaptEnvironType AdaptEnvironSpace AdaptEnvironGround AdaptEnvironUnderwater AdaptEnvironDesert '
    'AdaptEnvironSky Sight SkillNumber_1 SkillNumber_2 SkillNumber_3 StrikingPower DefensivePower MasteredWeaponIX '
    'WeaponNumber_1 WeaponNumber_2 WeaponNumber_3 WeaponNumber_FatalAttack ViewOrder MaxEnergyAmount MaxEnergyCost '
    'EnergyDecAmount UnitGrowIndex DailyLimit MonthLimit UserGradeLimit SallyReady CollectionUnit').split())
SERVER_WEAPON_COLUMNS = tuple(('WeaponNo WeaponName AttackStyle RangeType HitType DamageSize SpeedRate RangeSize '
    'AttackSpeed ExtraAttackType BulletRemain ReloadTime PushDist CoolTime SpecialFeature PushType StunBase '
    'StunFinal Pierce PierceRate MovingShot MaxCombo FunnelID').split())
SERVER_UNIT_WEAPON_FIELDS = ('WeaponNumber_1', 'WeaponNumber_2', 'WeaponNumber_3', 'WeaponNumber_FatalAttack')
DROP_CONDITION_BASE = 1_000_000       # native mission drop ids: 1000000 + group * 1000 + difficulty level
DROP_MACHINE_BASE = 390_000           # imported rare dice machines: 390000 + group * 10 + level
NATIVE_NORMAL_MACHINE = 101_000       # the shipped missions' normal drops per level: 101000 + level (levels 1..5)
DICE_TOTAL = 1000                     # every dice machine's probabilities add up to 1000 (per mille)
SIZES = {UNIT_KIND: UNIT_RECORD_BYTES, WEAPON_KIND: WEAPON_RECORD_BYTES, GAME_MODE_KIND: 1090, CLEAR_KIND: 391, NPC_TEMPLATE_KIND: 598, CATALOGUE_KIND: 1309, ROUTE_KIND: 34,
         OBJECTIVE_KIND: 634, SPEECH_KIND: 506, SPAWN_KIND: 152}
CLEAR_ID_LIMIT = 3238001          # client looks up kind 0x37 text for Clear IDs >= this
MODE5_ID_LIMIT = 0x313990         # create-room sends mode byte 5 at or below this GameModeID
MAX_PHASES = 8
MAX_GOALS = 5
MAX_FAILS = 5
MAX_SLOTS = 10
BANNER_DISTANCE = 450.0   # banner anchor ahead of the camera
BANNER_CENTERING = 200.0  # calibrated for the cloned text effect: its quad is drawn this far left of the anchor
FPS = 60
CAMERA_PATH_STEP = 0.1    # seconds between camera path samples; the runtime interpolates every frame
MOVE_TARGET_LIFT = 100.0  # cutscene move targets stay this far above the ground
LUA_NAME = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,39}')


class MissionError(ValueError):
    pass


def _fail(message: str):
    raise MissionError(message)


@dataclass
class MissionIds:
    """ID plan. Every range must be unused in the target client and server (checked by the caller)."""
    group: int                      # MissionGroupIndex = catalogue group
    catalogue: int                  # 0056 row (191001.. contiguous)
    route_first: int                # 0057 rows (195001.. contiguous); also MissionExpID
    objective_first: int            # 0080 rows (197001.. contiguous)
    game_mode_first: int            # GameModeID per difficulty (<= MODE5_ID_LIMIT -> room mode 5)
    clear_first: int                # ClearIDs (< CLEAR_ID_LIMIT)
    spawn_first: int                # MissionNpcID
    mode_start_first: int           # MissionModeStartID
    npc_first: int                  # new NpcDataID == client NPC template id
    speech_first: int               # operator speech rows
    lua_prefix: str                 # global Lua name prefix, e.g. 'SDGOM1'


@dataclass
class MissionBuild:
    rows: list[tuple[int, int, bytes]] = field(default_factory=list)
    server: dict[str, list[dict]] = field(default_factory=dict)
    lua: str = ''
    resources: dict[str, bytes] = field(default_factory=dict)
    report: dict = field(default_factory=dict)
    weapon_copies: dict[tuple[int, float], int] = field(default_factory=dict)   # (source, factor) -> copy id


# --------------------------------------------------------------------------- byte helpers
def _w(row: bytearray, offset: int, size: int, text: str, what: str) -> None:
    encoded = text.encode('utf-16-le')
    if len(encoded) > size - 2:
        _fail(f'{what} 过长（最多 {size // 2 - 1} 个字符）')
    row[offset:offset + size] = encoded.ljust(size, b'\0')


def _u32(row: bytearray, offset: int, value: int) -> None:
    struct.pack_into('<I', row, offset, int(value) & 0xFFFFFFFF)


def _i32(row: bytearray, offset: int, value: int) -> None:
    struct.pack_into('<i', row, offset, int(value))


def _f32(row: bytearray, offset: int, value: float) -> None:
    struct.pack_into('<f', row, offset, float(value))


def _vec(value, what: str, size: int = 3) -> tuple:
    if not isinstance(value, (list, tuple)) or len(value) != size or not all(
            isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) for v in value):
        _fail(f'{what} 需要 {size} 个数字')
    return tuple(float(v) for v in value)


def _zh(value, what: str, limit: int) -> str:
    if not isinstance(value, dict) or not isinstance(value.get('zh'), str) or not value['zh'] or len(value['zh']) > limit:
        _fail(f'{what}.zh 需要 1..{limit} 个字符')
    return value['zh']


def _ascii(text: str, limit: int) -> str:
    folded = ''.join(c if 32 <= ord(c) < 127 else '?' for c in text)
    return folded[:limit]


# --------------------------------------------------------------------------- templates
@dataclass
class Templates:
    """Native rows the mirrors are cloned from (read from the target client)."""
    game_mode: bytes
    clear: bytes
    routes: dict[int, bytes]            # difficulty byte -> route row
    objective: bytes
    catalogue: bytes
    speech: bytes
    npc_templates: dict[int, bytes]     # template id -> row
    npc_data: dict[int, dict]           # NpcDataID -> server row
    spawn: bytes                        # client spawn mirror row (one per MissionNpc row)
    unit_exists: Callable[[int], bool] = lambda unit: True
    unit_labels: Callable[[int], set] | None = None   # unit id -> action labels of its model
    unit_record: Callable[[int], bytes | None] | None = None   # unit id -> client unit record (for NPC labels)
    weapon_record: Callable[[int], bytes | None] | None = None  # weapon id -> client weapon record (NPC attack)
    # source unit -> complete tbl_UnitNpc definition.  For a tbl_Units-only source, template_unit supplies the
    # NPC template whose SQL-only CustomNumber is inherited; all native combat fields still come from source.
    server_unit_record: Callable[[int, int | None], dict | None] | None = None
    server_weapon_record: Callable[[int], dict | None] | None = None
    blueprints: dict[int, list[tuple[int, str]]] | None = None  # output unit -> [(blueprint item category, name)]
    native_exists: Callable[[str], bool] | None = None  # referenced native cut-in artwork


def _lua_string(text: str) -> str:
    if not all(32 <= ord(c) < 127 for c in text):
        _fail('Lua 文本只能使用 ASCII（中文对白请用 lines）')
    return "'" + text.replace('\\', '\\\\').replace("'", "\\'") + "'"


def _num(v: float) -> str:
    text = ('%.2f' % v).rstrip('0').rstrip('.')
    return text if text not in ('-0', '') else '0'


def _v3(v) -> str:
    return 'vector3(%s,%s,%s)' % (_num(v[0]), _num(v[1]), _num(v[2]))


def _catmull(points: list[tuple[float, tuple, tuple]], step: float) -> list[tuple[float, tuple, tuple]]:
    """Sample camera keys (t, pos, look) with a centripetal-free uniform Catmull-Rom spline every `step` s."""
    if len(points) == 1:
        return points
    out = []
    t_end = points[-1][0]
    t = 0.0
    while t <= t_end + 1e-6:
        i = max(0, min(len(points) - 2, next((k for k in range(len(points) - 1) if points[k + 1][0] >= t), len(points) - 2)))
        t0, t1 = points[i][0], points[i + 1][0]
        u = 0.0 if t1 <= t0 else (t - t0) / (t1 - t0)
        p = [points[max(0, i - 1)], points[i], points[i + 1], points[min(len(points) - 1, i + 2)]]

        def spline(idx):
            a, b, c, d = (q[idx] for q in p)
            return tuple(0.5 * ((2 * b[k]) + (-a[k] + c[k]) * u + (2 * a[k] - 5 * b[k] + 4 * c[k] - d[k]) * u * u
                                + (-a[k] + 3 * b[k] - 3 * c[k] + d[k]) * u * u * u) for k in range(3))
        out.append((round(t, 3), spline(1), spline(2)))
        t += step
    return out


RUNTIME_LUA = r'''
-- SDGO map import cutscene runtime (generated; Lua 5.0, ASCII only)
SDGOMI = SDGOMI or {}
function SDGOMI.cleanup(m, c)
  local s = SDGOMI[c.id]
  if s == nil then return end
  if s.actors ~= nil then
    for k, a in pairs(s.actors) do if a ~= nil then a:ReMoveObject() end end
  end
  s.actors = nil
  if s.hidden ~= nil then
    for i = 1, s.hidden.n do UnHideObject(s.hidden[i]) end
  end
  s.hidden = nil
  if s.demo == 1 then m:DemoEnd(); m:ChangeGameCamera(); s.demo = 0 end
end
function SDGOMI.init(m, c)
  local s = {frame = 0, next = 1, actors = {}, demo = 1}
  SDGOMI[c.id] = s
  m:DemoPlay()
  if c.area ~= nil then SDGOMI.area(m, c.area) end
  if c.hide == 1 then
    s.hidden = {n = 0}
    local count = GetNumPlayer()
    for i = 0, count - 1 do
      local p = GetPlayer(i)
      if p ~= nil then
        local id = p:GetID()
        HideObject(id)
        s.hidden.n = s.hidden.n + 1
        s.hidden[s.hidden.n] = id
      end
    end
  end
  for name, a in pairs(c.actors) do
    local team = OBJECT_TEAM_RED
    if a.enemy == 1 then team = OBJECT_TEAM_BLUE end
    local npc = m:CreateNpc(a.tpl, a.at, team, a.dir)
    if npc ~= nil then npc:SetLuaFunctionName(); s.actors[name] = npc end
  end
  m:ChangeMoveCamera()
  local cam = m:GetMoveCamera()
  cam:SetCameraPos(c.cam0); cam:SetEyePoint(c.look0)
  return 1
end
function SDGOMI.run(m, c, e, s)
  local cam = m:GetMoveCamera()
  if e.cam ~= nil then cam:MoveCameraPos(e.cam, e.sec); cam:MoveEyePoiont(e.look, e.sec)
  elseif e.anim ~= nil then local a = s.actors[e.actor]; if a ~= nil then a:ChangeAni(e.anim, e.loop) end
  elseif e.move ~= nil then local a = s.actors[e.actor]; if a ~= nil then a:SetRoadSearchType(0); a:SetMoveTargetPos(e.move) end
  elseif e.fx ~= nil then CreateEffectA(e.fx, e.at, e.scale, e.dir, e.up)
  elseif e.sound ~= nil then PlayEffectSound(e.sound)
  elseif e.shake ~= nil then cam:CameraShake(e.shake, e.sec)
  elseif e.speech ~= nil then PutOperatorSpeech(e.speech)
  end
end
function SDGOMI.camera(m, c, s)
  local p = c.path
  if p == nil or p.n < 1 then return end
  local k = s.cam or 1
  while k < p.n and p[k + 1][1] <= s.frame do k = k + 1 end
  s.cam = k
  local a = p[k]
  local b = p[k]
  if k < p.n then b = p[k + 1] end
  local u = 0
  if b[1] > a[1] then u = (s.frame - a[1]) / (b[1] - a[1]) end
  if u < 0 then u = 0 end
  if u > 1 then u = 1 end
  local cam = m:GetMoveCamera()
  cam:SetCameraPos(vector3(a[2] + (b[2] - a[2]) * u, a[3] + (b[3] - a[3]) * u, a[4] + (b[4] - a[4]) * u))
  cam:SetEyePoint(vector3(a[5] + (b[5] - a[5]) * u, a[6] + (b[6] - a[6]) * u, a[7] + (b[7] - a[7]) * u))
end
function SDGOMI.update(m, c)
  local s = SDGOMI[c.id]
  if s == nil then return 456 end
  s.frame = s.frame + 1
  SDGOMI.camera(m, c, s)
  for name, a in pairs(c.actors) do
    if a.hold == 1 and s.actors[name] ~= nil then s.actors[name]:SetNowPos(a.at) end
  end
  while s.next <= c.n and c.events[s.next].f <= s.frame do
    SDGOMI.run(m, c, c.events[s.next], s)
    s.next = s.next + 1
  end
  if s.frame >= c.frames then SDGOMI.cleanup(m, c); return 456 end
  return 0
end
function SDGOMI.skip(m, c) SDGOMI.cleanup(m, c); return 456 end
function SDGOMI.warp(m, w)
  local n = GetNumPlayer()
  for i = 1, n do
    local k = i
    if k > w.n then k = w.n end
    local p = w.points[k]
    PlayerWarp(i - 1, p, w.dir)
    local unit = GetPlayer(i - 1)
    if unit ~= nil and w.rect ~= nil then unit:SetTerrainRect(w.rect[1], w.rect[2], w.rect[3], w.rect[4]) end
  end
  if w.rect ~= nil then SDGOMI.local_area(w.rect) end
end
function SDGOMI.local_area(r)
  local doc = GetDocument()
  if doc == nil then return end
  local p = doc:GetLocalPlayer()
  if p == nil then return end
  local me = CastingUnit(p)
  if me ~= nil then me:SetTerrainRect(r[1], r[2], r[3], r[4]); me:SetTerrainRectOnOff(true) end
end
function SDGOMI.area(m, r)
  for i = 1, GetNumPlayer() do
    local unit = GetPlayer(i - 1)
    if unit ~= nil then unit:SetTerrainRect(r[1], r[2], r[3], r[4]) end
  end
  SDGOMI.local_area(r)
end
'''


def _complete_server_row(row, columns: tuple[str, ...], what: str) -> dict:
    if not isinstance(row, dict) or set(row) != set(columns):
        _fail(f'{what} 的服务端定义字段不完整或含未知字段')
    return dict(row)


def _weapon_copy(build: 'MissionBuild', templates: Templates, ids: 'MissionIds', key: str, weapon: int,
                 factor: float) -> int:
    """Paired client/server copy of a changed weapon (one copy per source weapon and factor)."""
    cache = build.weapon_copies
    if (weapon, factor) in cache:
        return cache[(weapon, factor)]
    if templates.weapon_record is None:
        _fail(f'npcs.{key}.attack 需要读取目标客户端的武器记录')
    source = templates.weapon_record(weapon)
    if source is None or len(source) != WEAPON_RECORD_BYTES or struct.unpack_from('<I', source)[0] != weapon:
        _fail(f'npcs.{key}.attack：目标客户端没有匹配的武器 {weapon} 记录')
    damage = struct.unpack_from('<I', source, WEAPON_DAMAGE)[0]
    changed_damage = min(100000, int(round(damage * factor)))
    if changed_damage == damage:
        return weapon
    if templates.server_weapon_record is None:
        _fail(f'npcs.{key}.attack 需要读取目标服务端的武器记录')
    server_source = templates.server_weapon_record(weapon)
    server_source = _complete_server_row(server_source, SERVER_WEAPON_COLUMNS,
                                         f'npcs.{key}.attack 武器 {weapon}')
    if server_source['WeaponNo'] != weapon or server_source['DamageSize'] != damage:
        _fail(f'npcs.{key}.attack：客户端与服务端武器 {weapon} 的编号或伤害不匹配')
    ident = WEAPON_COPY_FIRST + ids.group * 100 + len(cache)
    if len(cache) >= 100 or templates.weapon_record(ident) is not None:
        _fail(f'npcs.{key}.attack：武器编号 {ident} 已被客户端占用或超出每个任务 100 个')
    copy = bytearray(source)
    _u32(copy, 0, ident)
    _u32(copy, WEAPON_DAMAGE, changed_damage)
    build.rows.append((WEAPON_KIND, ident, bytes(copy)))
    server_copy = dict(server_source)
    server_copy.update(WeaponNo=ident, DamageSize=changed_damage)
    build.server.setdefault('tbl_WeaponsRev', []).append(server_copy)
    cache[(weapon, factor)] = ident
    return ident


def _drop_blueprints(drops, npcs: dict, npc_units: dict, templates: Templates) -> list[tuple[int, str]]:
    """Blueprint items a mission can drop: those whose output unit appears in the mission (`"mission"`, the
    default: each NPC's unit, or the player unit of the same name for an NPC-only unit such as "高达 阿姆罗") or
    the listed unit ids."""
    if drops is None:
        return []
    if not isinstance(drops, dict) or set(drops) - {'normal', 'blueprints', 'blueprint_per_mille'}:
        _fail('drops 只能包含 normal、blueprints、blueprint_per_mille')
    if not isinstance(drops.get('normal', True), bool):
        _fail('drops.normal 必须是布尔值')
    wanted = drops.get('blueprints', 'mission')
    if wanted in (None, []):
        return []
    if templates.blueprints is None:
        _fail('drops.blueprints 需要读取目标服务端的设计图')
    if wanted == 'mission':
        def name(unit):
            raw = templates.unit_record(unit) if templates.unit_record else None
            return raw[5:105].decode('utf-16-le', 'ignore').split('\0')[0].strip() if raw else ''
        by_name: dict[str, set] = {}
        for unit in templates.blueprints:
            by_name.setdefault(name(unit), set()).add(unit)
        units = set()
        for unit in npc_units.values():
            units.add(unit)
            full = name(unit)
            for alias in {full, full.split('(')[0].split('（')[0].strip(), full.split(' ')[0]}:
                if alias:
                    units |= by_name.get(alias, set())
    elif isinstance(wanted, list) and all(isinstance(u, int) and not isinstance(u, bool) and u > 0 for u in wanted):
        units = set(wanted)
    else:
        _fail('drops.blueprints 需要 "mission" 或机体编号列表')
    return sorted({item for unit in units for item in templates.blueprints.get(unit, [])})


def _drop_rows(tables: dict, drops, items: list, group: int, level: int) -> int:
    """Server rows for one difficulty's drops; returns its DropConditionID (0 = no drops)."""
    if drops is None:
        return 0
    normal = drops.get('normal', True)
    per_mille = drops.get('blueprint_per_mille', 2)
    if isinstance(per_mille, bool) or not isinstance(per_mille, int) or not 1 <= per_mille <= 50:
        _fail('drops.blueprint_per_mille 需要 1..50（每张设计图的千分比）')
    total = per_mille * len(items)
    if total > 500:
        _fail(f'设计图掉落总概率 {total}‰ 超过 500‰，请降低 blueprint_per_mille 或指定更少的机体')
    if normal and not 1 <= level <= 5:
        _fail('drops.normal 只适用于等级 1..5（原版任务掉落）')
    if not normal and not items:
        return 0
    if not 1 <= level <= 9:
        _fail('掉落需要等级 1..9')
    drop_id = DROP_CONDITION_BASE + group * 1000 + level
    machine = DROP_MACHINE_BASE + group * 10 + level if items else 0
    if any(r['DropConditionID'] == drop_id for r in tables.get('tbl_BattleResultItemDropSetting', [])):
        _fail('同一任务的不同难度不能使用相同的等级（掉落编号会重复）')
    tables.setdefault('tbl_BattleResultItemDropSetting', []).append({
        'DropConditionID': drop_id, 'NormalMachineID': NATIVE_NORMAL_MACHINE + level if normal else 0,
        'RareMachineID': machine, 'UniqueMachineID': 0})
    if items:
        # the server rolls 0..999 and takes the first row whose cumulative SumProb reaches the roll: "nothing" first,
        # then each blueprint with exactly `per_mille` of the 1000 rolls
        dice = tables.setdefault('tbl_BattleResultItemDropDice', [])
        cumulative = DICE_TOTAL - 1 - total
        dice.append({'ItemMachineID': machine, 'CategoryID': 0, 'Prob': DICE_TOTAL - total, 'ItemName': 'none',
                     'Amount': 1, 'Opt': 0, 'RareDice': 5, 'SumProb': cumulative})
        for category, _name in items:
            cumulative += per_mille
            dice.append({'ItemMachineID': machine, 'CategoryID': category, 'Prob': per_mille,
                         'ItemName': _ascii(f'blueprint {category}', 50), 'Amount': 1, 'Opt': 0, 'RareDice': 5,
                         'SumProb': cumulative})
    return drop_id


def _validate_npcs(spec: dict, templates: Templates) -> dict:
    npcs = {}
    if not isinstance(spec, dict) or not spec:
        _fail('npcs 不能为空')
    for key, row in spec.items():
        if not re.fullmatch(r'[a-z][a-z0-9_]{0,31}', key) or not isinstance(row, dict):
            _fail(f'npcs.{key} 无效')
        if 'unit' in row:
            base = row.get('base')
            if base not in templates.npc_templates or base not in templates.npc_data:
                _fail(f'npcs.{key}.base 需要目标客户端与服务端都存在的 NPC 模板')
            if not templates.unit_exists(int(row['unit'])):
                _fail(f'npcs.{key}.unit 在目标客户端中不存在')
            npcs[key] = {'kind': 'unit', 'base': int(base), 'unit': int(row['unit']), **row}
        elif 'template' in row:
            tpl = row['template']
            if tpl not in templates.npc_templates or tpl not in templates.npc_data:
                _fail(f'npcs.{key}.template {tpl} 在目标客户端或服务端不存在')
            derived = any(k in row for k in ('ai', 'hp', 'boss', 'name', 'label', 'attack', 'special_attack', 'cutin'))
            npcs[key] = {'kind': 'derived' if derived else 'native', 'base': int(tpl), **row}
        else:
            _fail(f'npcs.{key} 需要 template 或 unit')
        for field_name in ('ai',):
            if field_name in row and not LUA_NAME.fullmatch(str(row[field_name])):
                _fail(f'npcs.{key}.ai 必须是 ASCII 脚本名')
        for field_name in ('attack', 'special_attack'):
            value = row.get(field_name)
            if value is not None and (isinstance(value, bool) or not isinstance(value, (int, float))
                                      or not 0.25 <= float(value) <= 4.0):
                _fail(f'npcs.{key}.{field_name} 需要 0.25..4 的倍率')
    return npcs


def compile_mission(package: Package, spec: dict, *, map_no: int, terrain_path: str, ids: MissionIds,
                    templates: Templates, banner_effect: Callable[[bytes, str], dict[str, bytes]] | None = None,
                    occluders: list | None = None) -> MissionBuild:
    """Compile `spec` (parsed mission JSON). `banner_effect(png, name)` returns overlay resources for a banner
    effect named `name` (a cloned text effect bound to the PNG) or None when banners are unsupported."""
    version = spec.get('schema_version')
    if spec.get('schema') != SCHEMA or type(version) is not int or version not in (1, 2):
        _fail(f'任务文件必须声明 schema={SCHEMA} 且 schema_version=1 或 2')
    if not 1 <= ids.group <= 255 or ids.clear_first + 1000 > CLEAR_ID_LIMIT or ids.game_mode_first > MODE5_ID_LIMIT:
        _fail('任务 ID 规划越界')
    if not LUA_NAME.fullmatch(ids.lua_prefix) or len(ids.lua_prefix) > 12:
        _fail('lua_prefix 无效')
    build = MissionBuild()
    transition_delay = spec.get('transition_delay', 'native')
    if transition_delay not in ('native', 'immediate'):
        _fail('transition_delay 只能是 native 或 immediate')
    mission_name = _zh(spec.get('name'), 'name', 48)
    name_en = (spec.get('name') or {}).get('en') or spec.get('mission_key', 'mission')
    description = _zh(spec.get('description'), 'description', 240)
    briefing = (spec.get('briefing') or {}).get('zh') or []
    if not isinstance(briefing, list) or len(briefing) > 5 or not all(isinstance(x, str) and len(x) <= 49 for x in briefing):
        _fail('briefing.zh 最多 5 行、每行 49 字')
    difficulties = spec.get('difficulties')
    if not isinstance(difficulties, list) or not 1 <= len(difficulties) <= 4:
        _fail('difficulties 需要 1..4 档')
    npcs = _validate_npcs(spec.get('npcs'), templates)
    if version < 2 and (transition_delay != 'native' or any(npc.get('cutin') is not None for npc in npcs.values())):
        _fail('机体图片横幅与立即阶段衔接需要 schema_version=2；旧编译器必须拒绝这些新语义')
    lines = spec.get('lines') or {}
    banners = spec.get('banners') or {}
    sectors = spec.get('sectors') or {}
    cutscenes = spec.get('cutscenes') or {}
    phases = spec.get('phases')
    if not isinstance(phases, list) or not 1 <= len(phases) <= MAX_PHASES:
        _fail(f'phases 需要 1..{MAX_PHASES} 个阶段')
    phase_order = {}
    for i, phase in enumerate(phases):
        key = phase.get('key') if isinstance(phase, dict) else None
        if not isinstance(key, str) or not re.fullmatch(r'[a-z0-9_]{1,31}', key) or key in phase_order:
            _fail(f'phases[{i}].key 无效或重复')
        phase_order[key] = i + 1

    # -- NPC ids: native templates keep their id; derived/unit NPCs get new NpcData + template rows
    npc_ids: dict[str, int] = {}
    npc_units: dict[str, int] = {}
    npc_goal_units: dict[str, int] = {}
    next_npc = ids.npc_first
    for key, npc in npcs.items():
        base_row = templates.npc_templates[npc['base']]
        base_data = templates.npc_data[npc['base']]
        if npc['kind'] == 'native':
            npc_ids[key] = npc['base']
            npc_units[key] = int(base_data['UseUnitID'])
            npc_goal_units[key] = int(base_data['UseUnitID'])
            continue
        ident = next_npc
        next_npc += 1
        unit = npc['unit'] if npc['kind'] == 'unit' else int(base_data['UseUnitID'])
        hp = int(npc.get('hp', base_data['UseUnitHP']))
        boss = 1 if npc.get('boss', bool(base_data['BossFlag'])) else 0
        row = bytearray(base_row)
        _u32(row, 0, ident)
        if npc.get('name'):
            _w(row, 5, 100, _zh(npc['name'], f'npcs.{key}.name', 49), f'npcs.{key}.name')
        client_unit = unit
        server_unit = unit
        label = npc.get('label')
        attack = npc.get('attack')
        special = npc.get('special_attack', attack)
        if label is not None or attack is not None or special is not None:
            # The NPC gets a client unit copy.  A label-only copy remains client-only; a copy with changed
            # weapon slots is published with matching tbl_UnitNpc/tbl_WeaponsRev server rows.
            # label: the overhead label is the unit's display name up to its first "(".
            # attack / special_attack: normal and special damage use the paired weapon rows.
            if label is not None and (not isinstance(label, str) or not label.strip() or len(label) > 49
                                      or '(' in label):
                _fail(f'npcs.{key}.label 需要 1..49 字、不含半角括号')
            if templates.unit_record is None:
                _fail(f'npcs.{key} 需要读取目标客户端的机体记录')
            source = templates.unit_record(unit)
            if source is None or len(source) != UNIT_RECORD_BYTES:
                _fail(f'npcs.{key}：目标客户端没有机体 {unit} 的记录')
            copies = len([r for r in build.rows if r[0] == UNIT_KIND])
            client_unit = UNIT_LABEL_FIRST + ids.group * 100 + copies
            if copies >= 100 or templates.unit_record(client_unit) is not None:
                _fail(f'npcs.{key}：机体编号 {client_unit} 已被占用或超出每个任务 100 个')
            copy = bytearray(source)
            _u32(copy, 0, client_unit)
            if label is not None:
                _w(copy, 5, 100, label, f'npcs.{key}.label')
            source_slots = [struct.unpack_from('<I', source, slot)[0]
                            for slot in UNIT_WEAPON_SLOTS + (UNIT_SPECIAL_SLOT,)]
            for slot in UNIT_WEAPON_SLOTS + (UNIT_SPECIAL_SLOT,):
                factor = special if slot == UNIT_SPECIAL_SLOT else attack
                weapon = struct.unpack_from('<I', copy, slot)[0]
                if factor is None or float(factor) == 1.0 or weapon == 0:
                    continue
                _u32(copy, slot, _weapon_copy(build, templates, ids, key, weapon, float(factor)))
            changed_slots = [struct.unpack_from('<I', copy, slot)[0]
                             for slot in UNIT_WEAPON_SLOTS + (UNIT_SPECIAL_SLOT,)]
            if changed_slots != source_slots:
                if templates.server_unit_record is None:
                    _fail(f'npcs.{key}.attack 需要读取目标服务端的机体记录')
                server_source = templates.server_unit_record(unit, int(base_data['UseUnitID']))
                server_source = _complete_server_row(server_source, SERVER_UNIT_NPC_COLUMNS,
                                                     f'npcs.{key}.attack 机体 {unit}')
                if server_source['UnitNumber'] != unit:
                    _fail(f'npcs.{key}.attack：服务端机体 {unit} 的编号不匹配')
                server_slots = [int(server_source[field] or 0) for field in SERVER_UNIT_WEAPON_FIELDS]
                if server_slots != source_slots:
                    _fail(f'npcs.{key}.attack：客户端与服务端机体 {unit} 的武器槽不匹配')
                server_copy = dict(server_source)
                server_copy['UnitNumber'] = client_unit
                for field, weapon_id in zip(SERVER_UNIT_WEAPON_FIELDS, changed_slots):
                    server_copy[field] = weapon_id or None if server_source[field] is None else weapon_id
                build.server.setdefault('tbl_UnitNpc', []).append(server_copy)
                server_unit = client_unit
            build.rows.append((UNIT_KIND, client_unit, bytes(copy)))
        _u32(row, 105, client_unit)
        if npc.get('ai'):
            _w(row, 109, 100, npc['ai'], f'npcs.{key}.ai')
        _u32(row, 229, hp)
        row[259] = boss
        build.rows.append((NPC_TEMPLATE_KIND, ident, bytes(row)))
        data = dict(base_data)
        data.update({'NpcDataID': ident, 'UseUnitID': server_unit, 'UseUnitHP': hp, 'BossFlag': boss,
                     'NpcName': _ascii(((npc.get('name') or {}).get('en') or key), 32)})
        build.server.setdefault('tbl_NpcData', []).append(data)
        npc_ids[key] = ident
        npc_units[key] = unit
        npc_goal_units[key] = server_unit

    drop_items = _drop_blueprints(spec.get('drops'), npcs, npc_units, templates)

    # -- speech rows for dialogue lines
    speech_ids: dict[str, int] = {}
    for n, (key, line) in enumerate(sorted(lines.items())):
        ident = ids.speech_first + n
        row = bytearray(templates.speech)
        _u32(row, 0, ident)
        portrait = line.get('portrait', '')
        if not re.fullmatch(r'(?i)sprs\\[0-9a-z_]{1,24}\.spr', portrait):
            _fail(f'lines.{key}.portrait 必须形如 sprs\\\\NNNN.spr')
        _w(row, 5, 100, portrait, f'lines.{key}.portrait')
        speaker = _zh(line.get('speaker'), f'lines.{key}.speaker', 20)
        _w(row, 105, 400, f'[{speaker}]\n{_zh(line.get("text"), f"lines.{key}.text", 150)}', f'lines.{key}')
        build.rows.append((SPEECH_KIND, ident, bytes(row)))
        speech_ids[key] = ident

    # Native OriginalModeOperator's full-width UnitIcon. The NPC-create path reads the dialogue at +390
    # and the sprite at +398; the original UI owns its animation, placement and resolution scaling.
    for cutin_index, (key, npc) in enumerate(npcs.items()):
        cutin = npc.get('cutin')
        if cutin is None:
            continue
        if (not isinstance(cutin, dict) or set(cutin) not in
                ({'native_sprite', 'line'}, {'image', 'line'}, {'image', 'line', 'crop'}) or cutin.get('line') not in speech_ids):
            _fail(f'npcs.{key}.cutin 需要 native_sprite 或 image（二选一）与已定义的 line')
        if 'native_sprite' in cutin:
            sprite = cutin['native_sprite']
            if not isinstance(sprite, str) or not re.fullmatch(r'(?i)sprs\\[0-9a-z_]{1,24}\.spr', sprite):
                _fail(f'npcs.{key}.cutin.native_sprite 必须形如 sprs\\\\NNNN.spr')
            if templates.native_exists is None or not templates.native_exists(sprite):
                _fail(f'npcs.{key}.cutin.native_sprite 在目标客户端不存在')
        else:
            from PIL import Image
            import io
            from .compiler import spr_bytes, _native
            from .txr_format import build_txr
            image = Image.open(io.BytesIO(package.read(cutin['image'])))
            if not (1 <= image.width <= 2048 and 1 <= image.height <= 2048):
                _fail(f'npcs.{key}.cutin.image 的宽高不能超过 2048 像素')
            rect = cutin.get('crop')
            if rect is None:
                if image.size != (1024, 156):
                    _fail(f'npcs.{key}.cutin.image 必须是 1024x156，其他尺寸需声明 crop')
                rect = [0, 0, 1024, 156]
            if (not isinstance(rect, list) or len(rect) != 4 or any(type(v) is not int for v in rect)
                    or not 0 <= rect[0] < rect[2] <= image.width
                    or not 0 <= rect[1] < rect[3] <= image.height):
                _fail(f'npcs.{key}.cutin.crop 需要图片范围内的 [左,上,右,下] 像素坐标')
            height = round(1024 * (rect[3] - rect[1]) / (rect[2] - rect[0]))
            if not 64 <= height <= 512:
                _fail(f'npcs.{key}.cutin.crop 需要适合横幅的宽高比例')
            # Keep source pixels intact. The native sprite's UV rectangle clips
            # the margins, and its logical size preserves the selected aspect.
            width_t = 1 << (image.width - 1).bit_length()
            height_t = 1 << (image.height - 1).bit_length()
            image = image.convert('RGBA')
            canvas = Image.new('RGBA', (width_t, height_t), (0, 0, 0, 0))
            canvas.paste(image, (0, 0))
            texture = f'txrs/{ids.lua_prefix.lower()}u{cutin_index:02d}.txr'
            sprite_name = f'sprs/{ids.lua_prefix.lower()}u{cutin_index:02d}.spr'
            build.resources[texture] = build_txr(width_t, height_t, canvas.tobytes())
            native_sprite = bytearray(spr_bytes(_native(texture), 1024, height, (width_t, height_t)))
            struct.pack_into('<4f', native_sprite, 20, rect[0]/width_t, rect[1]/height_t,
                             rect[2]/width_t, rect[3]/height_t)
            build.resources[sprite_name] = bytes(native_sprite)
            sprite = _native(sprite_name)
        for index, (kind, ident, raw) in enumerate(build.rows):
            if kind == NPC_TEMPLATE_KIND and ident == npc_ids[key]:
                row = bytearray(raw)
                _u32(row, 390, speech_ids[cutin['line']])
                _w(row, 398, 200, sprite, f'npcs.{key}.cutin.native_sprite')
                build.rows[index] = (kind, ident, bytes(row))
                break
        else:
            _fail(f'npcs.{key}.cutin 缺少派生 NPC 模板')

    # -- banners: one overlay effect per banner image
    banner_fx: dict[str, str] = {}
    for n, (key, banner) in enumerate(sorted(banners.items())):
        if banner_effect is None:
            _fail('当前客户端不支持横幅特效')
        fx = f'{ids.lua_prefix.lower()}b{n:02d}.zfx'
        build.resources.update(banner_effect(package.read(banner['image']), fx))
        banner_fx[key] = fx

    def area_lua(rect, what: str) -> str:
        """Movable area [left x, top z, right x, bottom z] (north up) in the order of the client's
        SetTerrainRect, the call native missions use for their per-phase areas."""
        left, top, right, bottom = _vec(rect, what, 4)
        if not (left < right and bottom < top):
            _fail(f'{what} 需要 [左 x, 上 z, 右 x, 下 z]，且左 < 右、下 < 上')
        if max(abs(left), abs(right), abs(top), abs(bottom)) > package.half_extent:
            _fail(f'{what} 超出地形范围')
        return '{%s}' % ','.join(_num(v) for v in (left, top, right, bottom))

    # movable area active in each phase (the mission area, then each phase's enter.sector rect / enter.area):
    # every spawn of a phase must be inside it, or players cannot reach the units the goal counts
    phase_areas = []
    active = tuple(_vec(spec['area'], 'area', 4)) if spec.get('area') is not None else None
    for p_index, phase in enumerate(phases):
        enter = phase.get('enter') or {}
        rect = (sectors.get(enter.get('sector')) or {}).get('rect') if enter.get('sector') else None
        if rect is not None:
            active = tuple(_vec(rect, f'sectors.{enter["sector"]}.rect', 4))
        if enter.get('area') is not None:
            active = tuple(_vec(enter['area'], f'phases[{p_index}].enter.area', 4))
        phase_areas.append(active)

    start_area = 'nil'
    if spec.get('area') is not None:
        if not spec.get('opening'):
            _fail('area（开局活动范围）需要 opening 过场：范围在开场过场开始时设置')
        start_area = area_lua(spec['area'], 'area')

    # -- cutscenes -> Lua tables
    lua_parts = [RUNTIME_LUA]
    cutscene_names: dict[str, tuple[str, float]] = {}
    for n, (key, cut) in enumerate(sorted(cutscenes.items())):
        fn = f'{ids.lua_prefix}_C{n}'
        length = float(cut.get('length', 0))
        if not 1.0 <= length <= 60.0:
            _fail(f'cutscenes.{key}.length 需要 1..60 秒')
        actors = {}
        actor_pos = {}
        for actor, a in (cut.get('actors') or {}).items():
            if not re.fullmatch(r'[a-z][a-z0-9_]{0,15}', actor) or a.get('npc') not in npc_ids:
                _fail(f'cutscenes.{key}.actors.{actor} 无效')
            fx, fz = _vec(a.get('facing', [0, 1]), f'cutscenes.{key}.actors.{actor}.facing', 2)
            if occluders is not None:
                from .occlusion import blocked
                if blocked(occluders, _vec(a.get('at'), 'actor.at')):
                    _fail(f'cutscenes.{key}.actors.{actor} 位于物件内部')
            actor_pos[actor] = _vec(a.get('at'), 'actor.at')
            # hold: keep the actor at `at` every frame (units fall to the ground after CreateNpc; a hovering
            # ship such as a mothership would otherwise drop and sink its hull into the terrain)
            hold = a.get('hold', False)
            if not isinstance(hold, bool):
                _fail(f'cutscenes.{key}.actors.{actor}.hold 必须是布尔值')
            # side: actors are on the players' side by default, so AI wingmen never shoot at them during the
            # cutscene; "enemy" puts an actor on the enemy team as the shipped single-player cutscenes do
            side = a.get('side', 'ally')
            if side not in ('ally', 'enemy'):
                _fail(f'cutscenes.{key}.actors.{actor}.side 只能是 ally 或 enemy')
            actors[actor] = '%s={tpl=%d,at=%s,dir=%s,hold=%d,enemy=%d}' % (
                actor, npc_ids[a['npc']], _v3(_vec(a.get('at'), 'actor.at')), _v3((fx, 0.0, fz)), 1 if hold else 0,
                1 if side == 'enemy' else 0)
        keys = [(float(k['t']), _vec(k['pos'], 'camera.pos'), _vec(k['look'], 'camera.look'))
                for k in (cut.get('camera') or [])]
        if not keys or keys[0][0] != 0.0 or any(b[0] <= a[0] for a, b in zip(keys, keys[1:])):
            _fail(f'cutscenes.{key}.camera 需要从 t=0 开始、时间递增的关键帧')
        samples = _catmull(keys, 0.25)
        if occluders is not None:
            from .occlusion import shot_problem
            for t, pos, look in samples:
                problem = shot_problem(lambda p: package_ground(package, p), occluders, pos, look)
                if problem:
                    _fail(f'cutscenes.{key} 在 t={t:.2f}s：{problem}（镜头 {_v3(pos)}，看向 {_v3(look)}）')
        events = []
        # the camera follows a dense path set every frame (as the shipped cutscenes do with SetCameraPos):
        # chained timed moves restart from the last set position and make the shot jump back
        path = _catmull(keys, CAMERA_PATH_STEP)
        path_lua = '{n=%d,%s}' % (len(path), ','.join(
            '{%d,%s,%s,%s,%s,%s,%s}' % ((max(1, int(round(t * FPS))),) + tuple(_num(v) for v in tuple(p) + tuple(l)))
            for t, p, l in path))
        cam_at = {round(t, 3): (p, l) for t, p, l in samples}
        for e in cut.get('events') or []:
            t = float(e.get('t', 0))
            if not 0 <= t <= length:
                _fail(f'cutscenes.{key} 事件时间越界')
            if 'anim' in e:
                if not LUA_NAME.fullmatch(e['anim']) or e.get('actor') not in actors:
                    _fail(f'cutscenes.{key} 动作事件无效')
                if templates.unit_labels is not None:
                    actor_npc = (cut.get('actors') or {})[e['actor']]['npc']
                    available = templates.unit_labels(npc_units[actor_npc])
                    if e['anim'] not in available:
                        _fail(f'cutscenes.{key}: 演员 {e["actor"]} 的模型没有动作 {e["anim"]}')
                events.append((t, "actor='%s',anim=%s,loop=%s" % (e['actor'], _lua_string(e['anim']),
                                                                   'true' if e.get('loop') else 'false')))
            if 'move_to' in e:
                if e.get('actor') not in actors:
                    _fail(f'cutscenes.{key} 移动事件无效')
                if (cut.get('actors') or {})[e['actor']].get('hold'):
                    _fail(f'cutscenes.{key}：固定位置（hold）的演员 {e["actor"]} 不能移动')
                target = _vec(e['move_to'], 'move_to')
                # a target on the ground pulls the unit's body down to it: keep it a unit's height above, like spawns
                if target[1] < package_ground(package, target) + MOVE_TARGET_LIFT:
                    _fail(f'cutscenes.{key} 在 t={t:.2f}s：演员 {e["actor"]} 的移动目标离地不足 {MOVE_TARGET_LIFT:.0f}')
                if occluders is not None:
                    from .occlusion import blocked
                    start = actor_pos[e['actor']]
                    for i in range(1, 25):
                        q = tuple(start[k] + (target[k] - start[k]) * i / 24 for k in range(3))
                        if blocked(occluders, q) or blocked(occluders, (q[0], q[1] + 120, q[2])):
                            _fail(f'cutscenes.{key} 在 t={t:.2f}s：演员 {e["actor"]} 的移动路径穿过物件')
                actor_pos[e['actor']] = target
                events.append((t, "actor='%s',move=%s" % (e['actor'], _v3(target))))
            if 'banner' in e:
                if e['banner'] not in banner_fx:
                    _fail(f'cutscenes.{key} 引用未定义的横幅 {e["banner"]}')
                near = min(cam_at, key=lambda s: abs(s - t))
                pos, look = cam_at[near]
                d = [look[k] - pos[k] for k in range(3)]
                length_d = math.sqrt(sum(x * x for x in d)) or 1.0
                d = [x / length_d for x in d]
                distance = float(e.get('distance', BANNER_DISTANCE))
                if not 100.0 <= distance <= 2000.0:
                    _fail(f'cutscenes.{key} 横幅 distance 需要 100..2000')
                # the cloned text effect draws its quad left of the anchor: shift the anchor to camera-right
                right = (d[2], 0.0, -d[0])
                norm = math.hypot(right[0], right[2]) or 1.0
                at = e.get('at') or [pos[k] + d[k] * distance + right[k] / norm * BANNER_CENTERING for k in range(3)]
                if occluders is not None:
                    from .occlusion import shot_problem
                    problem = shot_problem(lambda p: package_ground(package, p), occluders, pos, at)
                    if problem:
                        _fail(f'cutscenes.{key} 横幅 {e["banner"]} 在 t={t:.2f}s 不可见：{problem}')
                facing = [-x for x in d]
                events.append((t, 'fx=%s,at=%s,scale=%s,dir=%s,up=vector3(0,1,0)' % (
                    _lua_string(banner_fx[e['banner']]), _v3(at), _v3((1.0, 1.0, 1.0)), _v3(facing))))
            if 'effect' in e:
                fx = e['effect']
                if not re.fullmatch(r'[0-9a-z_]{1,24}\.zfx', fx.get('fx', '')):
                    _fail(f'cutscenes.{key} 特效名无效')
                s = float(fx.get('scale', 1.0))
                events.append((t, 'fx=%s,at=%s,scale=%s,dir=vector3(0,0,1),up=vector3(0,1,0)' % (
                    _lua_string(fx['fx']), _v3(_vec(fx.get('at'), 'effect.at')), _v3((s, s, s)))))
            if 'sound' in e:
                if not re.fullmatch(r'[0-9a-z_]{1,24}\.zsd', e['sound']):
                    _fail(f'cutscenes.{key} 音效名无效')
                events.append((t, 'sound=%s' % _lua_string(e['sound'])))
            if 'shake' in e:
                amp, sec = _vec(e['shake'], 'shake', 2)
                events.append((t, 'shake=%s,sec=%s' % (_num(amp), _num(sec))))
            if 'line' in e:
                if e['line'] not in speech_ids:
                    _fail(f'cutscenes.{key} 引用未定义的台词 {e["line"]}')
                events.append((t, 'speech=%d' % speech_ids[e['line']]))
        events.sort(key=lambda x: x[0])
        p0, l0 = keys[0][1], keys[0][2]
        hide = cut.get('hide_players', True)
        if not isinstance(hide, bool):
            _fail(f'cutscenes.{key}.hide_players 必须是布尔值')
        area = start_area if key == spec.get('opening') else 'nil'    # the opening applies the starting area
        lua_parts.append('%s = {id=%s, frames=%d, n=%d, hide=%d, area=%s, cam0=%s, look0=%s, path=%s, actors={%s}, events={\n  %s\n}}\n' % (
            fn, _lua_string(fn), int(round(length * FPS)), len(events), 1 if hide else 0, area, _v3(p0), _v3(l0),
            path_lua, ','.join(actors.values()),
            ',\n  '.join('{f=%d,%s}' % (max(1, int(round(t * FPS))), body) for t, body in events)))
        lua_parts.append('function %s_LuaInit(m) return SDGOMI.init(m, %s) end\n'
                         'function %s_Update(m) return SDGOMI.update(m, %s) end\n'
                         'function %s_Skip(m) return SDGOMI.skip(m, %s) end\n' % (fn, fn, fn, fn, fn, fn))
        cutscene_names[key] = (fn, length)

    # -- sector warps + phase-entry events
    warp_tables = {}
    for key, sector in sectors.items():
        points = [_vec(p, f'sectors.{key}.warp') for p in sector.get('warp') or []]
        if occluders is not None:
            from .occlusion import blocked
            if any(blocked(occluders, p) for p in points):
                _fail(f'sectors.{key}.warp 有传送点位于物件内部')
        if not 1 <= len(points) <= 8:
            _fail(f'sectors.{key}.warp 需要 1..8 个点')
        fx, fz = _vec(sector.get('facing', [0, 1]), f'sectors.{key}.facing', 2)
        rect = sector.get('rect')
        rect_lua = 'nil' if rect is None else area_lua(rect, f'sectors.{key}.rect')
        warp_tables[key] = '{n=%d,points={%s},dir=%s,rect=%s}' % (len(points), ','.join(_v3(p) for p in points),
                                                                   _v3((fx, 0, fz)), rect_lua)

    def event_function(phase_index: int, enter: dict) -> str:
        body = []
        for line in enter.get('lines') or []:
            if line not in speech_ids:
                _fail(f'phases[{phase_index}].enter.lines 引用未定义的台词 {line}')
            body.append('  PutOperatorSpeech(%d)' % speech_ids[line])
        if enter.get('sector'):
            if enter['sector'] not in warp_tables:
                _fail(f'phases[{phase_index}].enter.sector 未定义')
            body.append('  SDGOMI.warp(m, %s)' % warp_tables[enter['sector']])
        if enter.get('area') is not None:
            body.append('  SDGOMI.area(m, %s)' % area_lua(enter['area'], f'phases[{phase_index}].enter.area'))
        if not body:
            return ''
        fn = f'{ids.lua_prefix}_E{phase_index}'
        lua_parts.append('function %s(m)\n%s\n  return 123\nend\n' % (fn, '\n'.join(body)))
        return fn

    # -- server rows per difficulty
    for name_text in (spec.get('opening'), spec.get('ending')):
        if name_text is not None and name_text not in cutscene_names:
            _fail(f'opening/ending 引用未定义的过场 {name_text}')
    opening = cutscene_names.get(spec.get('opening'), ('', 0.0))
    ending = cutscene_names.get(spec.get('ending'), ('', 0.0))
    bgm = (spec.get('bgm') or {}).get('native', '')
    clear_id = ids.clear_first
    spawn_id = ids.spawn_first
    mode_start_id = ids.mode_start_first
    tables = build.server
    for d_index, difficulty in enumerate(difficulties):
        level = int(difficulty.get('level', 3))
        if level not in templates.routes:
            _fail(f'difficulties[{d_index}].level {level} 没有可用的原生路由模板')
        mode_id = ids.game_mode_first + d_index
        time_limit = int(difficulty.get('time_limit', 900))
        if time_limit % 60 or not 60 <= time_limit <= 3600:
            _fail('time_limit 必须是 60..3600 的 60 的倍数')
        lives = int(difficulty.get('lives', 5))
        max_slots = max(int(p.get('slots', {}).get('ally', 0)) + int(p.get('slots', {}).get('enemy', 8)) for p in phases)
        tables.setdefault('tbl_MissionMode', []).append({
            'GameModeID': mode_id, 'ModeName': _ascii(f'{name_en} L{level}', 20), 'ModeType': 1,
            'MissionGroupIndex': ids.group, 'GameMap': map_no, 'ScriptTime': int(math.ceil(opening[1])),
            'GameTime': time_limit, 'GameLife': lives, 'GameNpc': min(MAX_SLOTS, max_slots), 'GameAllyNpc': 0,
            'DropConditionID': 0, 'NeedEvent': 0, 'Min2PlayerTime': 30, 'Min3PlayerTime': 30, 'Min4PlayerTime': 30})
        drop_id = _drop_rows(tables, spec.get('drops'), drop_items, ids.group, level)
        tables['tbl_MissionMode'][-1]['DropConditionID'] = drop_id
        # Ranking/boss summary row: every listed native mission room reads it before starting.
        bosses = [npc_ids[k] for k, n in npcs.items() if n.get('boss')][:5]
        tables.setdefault('tbl_MissionRankBossData', []).append({
            'MissionID': mode_id, 'DropConditionID': drop_id, 'ClearPoint': 20,
            'DifficultPoint': min(255, 5 * level), **{f'BossID_{i + 1}': (bosses[i] if i < len(bosses) else 0)
                                                     for i in range(5)}})
        route_id = ids.route_first + d_index
        tables.setdefault('tbl_MissionExp', []).append({
            'MissionExpID': route_id, 'MissionID': mode_id, 'MissionDifficulty': level,
            'UseRankC': int(difficulty.get('required_rank', 5)), 'UserExp': 3000, 'UnitExp': 4000, 'UserPoint': 0,
            'UnitExpOnFail': 800, 'Disable': 0})

        # client GameMode mirror
        gm = bytearray(templates.game_mode)
        _u32(gm, 0, mode_id)
        _w(gm, 5, 100, mission_name, 'name')
        gm[105] = 1
        _u32(gm, 106, ids.group)
        gm[110] = ids.group & 0xFF
        _w(gm, 111, 100, terrain_path, 'terrain')
        _u32(gm, 211, map_no)
        _w(gm, 215, 100, '', 'sol')
        _w(gm, 315, 100, '', 'rule')
        _w(gm, 415, 100, opening[0], 'opening')
        _i32(gm, 515, int(math.ceil(opening[1])))
        _w(gm, 519, 100, ending[0], 'ending')
        _w(gm, 619, 100, bgm, 'bgm')
        _w(gm, 719, 100, '', 'bgm2')
        _u32(gm, 1038, time_limit)
        _u32(gm, 1042, lives)
        _u32(gm, 1046, min(MAX_SLOTS, max_slots))
        _u32(gm, 1050, 0)
        _u32(gm, 1062, 0)
        for off in range(1066, 1090, 4):        # optional target-marker ClearIDs (native missions may leave them 0)
            _u32(gm, off, 0)
        first_spawn = {}                        # party -> first MissionNpcID of this GameMode

        # route + objective
        route = bytearray(templates.routes[level])
        _u32(route, 0, route_id)
        _u32(route, 5, mode_id)
        _u32(route, 10, int(difficulty.get('required_rank', 5)))
        build.rows.append((ROUTE_KIND, route_id, bytes(route)))
        objective = bytearray(templates.objective)
        _u32(objective, 0, ids.objective_first + d_index)
        _u32(objective, 5, ids.group)
        objective[9] = level
        _w(objective, 10, 100, mission_name, 'objective title')
        # five briefing lines: u32 colour (kept from the template) + wchar[50] text
        for k in range(5):
            _w(objective, 114 + 104 * k, 100, briefing[k] if k < len(briefing) else '', 'briefing')
        build.rows.append((OBJECTIVE_KIND, ids.objective_first + d_index, bytes(objective)))

        for p_index, phase in enumerate(phases):
            order = p_index + 1
            next_key = phase.get('next', phases[p_index + 1]['key'] if p_index + 1 < len(phases) else None)
            event_next = phase_order[next_key] if next_key else 0
            slots = phase.get('slots') or {}
            ally, enemy = int(slots.get('ally', 0)), int(slots.get('enemy', 8))
            if ally + enemy > MAX_SLOTS:
                _fail(f'phases[{p_index}] 同屏 NPC（ally+enemy）不能超过 {MAX_SLOTS}')
            tables.setdefault('tbl_MissionModeStart', []).append({
                'MissionModeStartID': mode_start_id, 'MissionID': mode_id, 'MissionPart': order,
                'FriendNPC': ally, 'EnemyNPC': enemy, 'ExtendMode': 0})
            mode_start_id += 1
            enter = phase.get('enter') or {}
            cut_name, scene = cutscene_names.get(enter.get('cutscene'), ('', 0.0)) if enter.get('cutscene') else ('', 0.0)
            if enter.get('cutscene') and enter['cutscene'] not in cutscene_names:
                _fail(f'phases[{p_index}].enter.cutscene 未定义')
            # Native ScriptTime/SceneTime already holds the phase and resets its spawn clock when the
            # cutscene ends. Spawn times are therefore relative to active play; adding the scene length here
            # would make every authored delay run twice.
            event_fn = event_function(p_index, enter) if d_index == 0 else f'{ids.lua_prefix}_E{p_index}' \
                if any((enter.get('lines'), enter.get('sector'), enter.get('area') is not None)) else ''
            reward = phase.get('reward') or {}
            objective_text = _zh(phase.get('objective'), f'phases[{p_index}].objective', 49)

            goal = phase.get('goal') or {}
            goal_rows = []
            if 'kill_any' in goal:
                # any enemy, the shipped "destroy N" row (UseWho 2, object 0): one counter for a mixed group
                count = goal['kill_any']
                if not isinstance(count, int) or isinstance(count, bool) or not 1 <= count <= 255:
                    _fail(f'phases[{p_index}].goal.kill_any 需要 1..255')
                goal_rows.append({'UseWho': 2, 'UseHow': 2, 'UseObjectID': 0, 'UseObjectCount': count, 'UseTime': 0})
            if 'kill' in goal:
                units_seen = set()
                for g in goal['kill']:
                    if g.get('npc') not in npc_goal_units:
                        _fail(f'phases[{p_index}].goal.kill 引用未定义的 NPC')
                    unit = npc_goal_units[g['npc']]
                    if unit in units_seen:
                        _fail(f'phases[{p_index}].goal.kill 中的 NPC 使用了相同机体，计数会互相干扰')
                    units_seen.add(unit)
                    goal_rows.append({'UseWho': 2, 'UseHow': 2, 'UseObjectID': unit, 'UseObjectCount': int(g.get('count', 1)),
                                      'UseTime': 0})
            elif 'survive' in goal and not goal_rows:
                goal_rows.append({'UseWho': 6, 'UseHow': 4, 'UseObjectID': 0, 'UseObjectCount': 0,
                                  'UseTime': int(goal['survive'])})
            elif not goal_rows:
                _fail(f'phases[{p_index}].goal 需要 kill、kill_any 或 survive')
            if len(goal_rows) > MAX_GOALS:
                _fail(f'phases[{p_index}] 目标最多 {MAX_GOALS} 条')
            fail_rows = []
            for f in phase.get('fail') or []:
                if 'time_limit' in f:
                    fail_rows.append({'UseWho': 6, 'UseHow': 4, 'UseObjectID': 0, 'UseObjectCount': 0,
                                      'UseTime': int(f['time_limit'])})
                elif 'lose' in f:
                    lose = f['lose']
                    if lose.get('npc') not in npc_goal_units:
                        _fail(f'phases[{p_index}].fail.lose 引用未定义的 NPC')
                    fail_rows.append({'UseWho': 2, 'UseHow': 6, 'UseObjectID': npc_goal_units[lose['npc']],
                                      'UseObjectCount': int(lose.get('count', 1)), 'UseTime': 0})
                else:
                    _fail(f'phases[{p_index}].fail 只支持 time_limit / lose')
            if len(fail_rows) > MAX_FAILS:
                _fail(f'phases[{p_index}] 失败条件最多 {MAX_FAILS} 条')

            for r_index, (clear_type, row) in enumerate([(1, r) for r in goal_rows] + [(2, r) for r in fail_rows]):
                first = r_index == 0
                server_row = {'ClearID': clear_id, 'ClearType': clear_type, 'UseWho': row['UseWho'],
                              'UseHow': row['UseHow'], 'UseObjectID': row['UseObjectID'],
                              'UseObjectCount': row['UseObjectCount'], 'px': 0, 'py': 0, 'pz': 0,
                              'UseTime': row['UseTime'], 'AbsTime': 0,
                              'AddPnt': int(reward.get('points', 50)) if clear_type == 1 else 0,
                              'AddUnitExp': int(reward.get('unit_exp', 100)) if clear_type == 1 else 0,
                              'MissionModeID': mode_id, 'EventOrder': order,
                              'EventNext': event_next if clear_type == 1 else 0,
                              'SceneTime': int(math.ceil(scene)) if (first and clear_type == 1) else 0,
                              'FriendNpcRemove': 1, 'EnemyNpcRemove': 1, 'ZoneID': 0,
                              'EventAreaX': 0.0, 'EventAreaY': 0.0, 'EventAreaZ': 0.0, 'EventAreaR': 0.0}
                tables.setdefault('tbl_MissionClear', []).append(server_row)
                c = bytearray(templates.clear)
                _u32(c, 0, clear_id)
                c[5], c[6], c[7] = clear_type, row['UseWho'], row['UseHow']
                _i32(c, 8, row['UseObjectID'])
                _i32(c, 12, 0)
                c[16] = min(255, row['UseObjectCount'])
                for off in (17, 21, 25):
                    _f32(c, off, 0.0)
                _i32(c, 29, row['UseTime'])
                _i32(c, 33, 0)
                _i32(c, 37, server_row['AddUnitExp'])
                _i32(c, 41, mode_id)
                _i32(c, 45, order)
                _w(c, 53, 100, event_fn if (first and clear_type == 1 and order > 1) else '', 'event')
                _w(c, 153, 100, cut_name if (first and clear_type == 1 and order > 1) else '', 'cutscene')
                _i32(c, 253, server_row['SceneTime'])
                _i32(c, 257, server_row['EventNext'])
                c[261] = 1 if (first and clear_type == 1) else 0
                c[265], c[266] = 1, 1
                _u32(c, 267, 0)
                for off in (275, 279, 283, 287):
                    _f32(c, off, 0.0)
                _w(c, 291, 100, objective_text if (first and clear_type == 1) else '', 'objective text')
                build.rows.append((CLEAR_KIND, clear_id, bytes(c)))
                clear_id += 1
            if order == 1 and (event_fn or cut_name):
                _fail('第一阶段的开场请用 opening 过场；enter 只用于后续阶段')

            # spawns: FIFO per side, keep times ascending by id
            queue = []
            for s_index, spawn in enumerate(phase.get('spawns') or []):
                if spawn.get('npc') not in npc_ids:
                    _fail(f'phases[{p_index}].spawns[{s_index}] 引用未定义的 NPC')
                side = {'enemy': 2, 'ally': 1}.get(spawn.get('side', 'enemy'))
                if side is None:
                    _fail(f'phases[{p_index}].spawns[{s_index}].side 只能是 enemy 或 ally')
                at = float(spawn.get('at', 0))
                every = float(spawn.get('every', 0))
                for k, position in enumerate(spawn.get('positions') or []):
                    pos = _vec(position, f'phases[{p_index}].spawns[{s_index}].positions[{k}]')
                    if occluders is not None:
                        from .occlusion import blocked
                        if blocked(occluders, pos):
                            _fail(f'phases[{p_index}].spawns[{s_index}].positions[{k}] 位于物件内部')
                    ground = package_ground(package, pos)
                    if pos[1] < ground + 20:
                        _fail(f'phases[{p_index}].spawns[{s_index}] 刷怪点低于地面')
                    area = phase_areas[p_index]
                    if area is not None and not (area[0] <= pos[0] <= area[2] and area[3] <= pos[2] <= area[1]):
                        _fail(f'phases[{p_index}].spawns[{s_index}].positions[{k}] 不在本阶段的活动范围内'
                              f'（x {area[0]:.0f}..{area[2]:.0f}，z {area[3]:.0f}..{area[1]:.0f}）')
                    queue.append((side, at + every * k, s_index, k, npc_ids[spawn['npc']], pos,
                                  int(spawn.get('min_free', 1))))
            queue.sort(key=lambda q: (q[0], q[1], q[2], q[3]))
            for side, t, _, _, npc, pos, min_free in queue:
                # timed waves use AbsTime (seconds since the phase began), as the shipped reinforcements do:
                # AfterTime only advances while the spawn chain moves, so a quickly cleared field stalled it
                after, absolute = (max(0, int(round(t))), 0) if t < 2 else (0, int(round(t)))
                tables.setdefault('tbl_MissionNpc', []).append({
                    'MissionNpcID': spawn_id, 'MissionID': mode_id, 'MissionDifficult': level, 'MissionPart': order,
                    'NpcID': npc, 'px': int(round(pos[0])), 'py': int(round(pos[1])), 'pz': int(round(pos[2])), 'AfterTime': after,
                    'AbsTime': absolute, 'ManualID': 0, 'NpcParty': side, 'CreateLimit': max(1, min(MAX_SLOTS, min_free)),
                    'ZoneIndex': 0})
                # client mirror: the room start check walks these rows from GameMode +1054/+1058 and
                # preloads every NPC model; the walk stops at the first row of another GameMode
                mirror = bytearray(templates.spawn)
                _u32(mirror, 0, spawn_id)
                mirror[4] = 0                                   # ZoneIndex
                _i32(mirror, 5, mode_id)
                _i32(mirror, 9, level)
                _i32(mirror, 13, order)
                _i32(mirror, 17, npc)
                mirror[21] = side
                _w(mirror, 22, 100, '0', 'spawn name')
                for k in range(3):
                    _f32(mirror, 123 + 4 * k, float(pos[k]))
                _f32(mirror, 135, 0.0)
                _i32(mirror, 139, after)
                mirror[143:148] = bytes(5)                     # +147 AbsTime
                _i32(mirror, 148, 0)                           # ManualID (kind 0x32 group) unused
                build.rows.append((SPAWN_KIND, spawn_id, bytes(mirror)))
                first_spawn.setdefault(side, spawn_id)
                spawn_id += 1
            if not any(q[0] == 2 for q in queue) and ('kill' in goal or 'kill_any' in goal):
                _fail(f'phases[{p_index}] 有击破目标却没有敌方刷怪')
        if 2 not in first_spawn:
            _fail('任务至少需要一个敌方刷怪（客户端开战检查要求）')
        _u32(gm, 1054, first_spawn[2])
        _u32(gm, 1058, first_spawn.get(1, 0))
        build.rows.append((GAME_MODE_KIND, mode_id, bytes(gm)))

    # catalogue (one per mission group)
    cat = bytearray(templates.catalogue)
    _u32(cat, 0, ids.catalogue)
    _u32(cat, 5, ids.group)
    _w(cat, 9, 100, mission_name, 'name')
    _w(cat, 409, 500, description, 'description')
    for offset, path in _catalogue_art(package, spec.get('presentation'), ids.lua_prefix.lower(), build).items():
        _w(cat, offset, 100, path, f'presentation ({offset})')
    build.rows.append((CATALOGUE_KIND, ids.catalogue, bytes(cat)))

    for kind, ident, raw in build.rows:
        if len(raw) != SIZES[kind]:
            _fail(f'内部错误：记录 {ident} (kind {kind:#x}) 长度 {len(raw)} != {SIZES[kind]}')
    build.lua = ''.join(lua_parts)
    if not all(ord(c) < 128 for c in build.lua):
        _fail('生成的 Lua 含非 ASCII 字符')
    build.report = {'mission_key': spec.get('mission_key'), 'group': ids.group,
                    'game_modes': [ids.game_mode_first + i for i in range(len(difficulties))],
                    'transition_delay': transition_delay,
                    'phases': len(phases), 'clear_rows': clear_id - ids.clear_first,
                    'spawn_rows': spawn_id - ids.spawn_first, 'npc_rows': next_npc - ids.npc_first,
                    'speech_rows': len(speech_ids), 'cutscenes': {k: v[0] for k, v in cutscene_names.items()},
                    'lua_bytes': len(build.lua), 'records': len(build.rows)}
    return build


_GROUND_CACHE: dict[int, object] = {}


def package_ground(package: Package, pos) -> float:
    from .compiler import _Terrain
    terrain = _GROUND_CACHE.get(id(package))
    if terrain is None:
        terrain = _GROUND_CACHE[id(package)] = _Terrain(package)
    return terrain.at(pos[0], pos[2])


# Mission catalogue artwork: protocol key -> (catalogue offset, exact size). The sprites are what the mission
# list, room list and mission room show; `loading` is the loading-screen texture (art in the 4:3 band
# y 128..896, as in the client's own loading screens). Missing keys keep the template mission's images.
# `loading_screen` picks the battle loading screen: "poster" shows `loading` (the default when it is given),
# "random" keeps the client's random loading pictures (the catalogue's loading texture stays empty).
LOADING_SCREENS = ('poster', 'random')
CATALOGUE_ART = {
    'list_banner': (109, (155, 68)),
    'room_icon': (209, (120, 83)),
    'room_poster': (309, (250, 120)),
    'title': (909, (155, 22)),
    'loading': (1209, (1024, 1024)),
}


def _catalogue_art(package: Package, presentation, prefix: str, build: MissionBuild) -> dict[int, str]:
    from PIL import Image
    import io
    from .compiler import spr_bytes, _native
    from .txr_format import build_txr
    if presentation is None:
        return {}
    if not isinstance(presentation, dict) or set(presentation) - set(CATALOGUE_ART) - {'loading_screen'}:
        _fail('presentation 只能包含 ' + '、'.join((*CATALOGUE_ART, 'loading_screen')))
    screen = presentation.get('loading_screen', 'poster' if 'loading' in presentation else 'random')
    if screen not in LOADING_SCREENS:
        _fail('presentation.loading_screen 只能是 poster 或 random')
    if screen == 'poster' and 'loading' not in presentation:
        _fail('presentation.loading_screen = poster 需要 presentation.loading 图片')
    paths = {CATALOGUE_ART['loading'][0]: ''}       # random unless the poster is written below
    for n, (key, (offset, size)) in enumerate(CATALOGUE_ART.items()):
        if key not in presentation or (key == 'loading' and screen == 'random'):
            continue
        relative = presentation[key]
        if not isinstance(relative, str) or not relative.lower().endswith('.png'):
            _fail(f'presentation.{key} 必须是包内 PNG')
        try:
            image = Image.open(io.BytesIO(package.read(relative))).convert('RGBA')
        except MapPackageError:
            raise
        except Exception as error:
            _fail(f'presentation.{key} 无法读取：{error}')
        if image.size != size:
            _fail(f'presentation.{key} 必须是 {size[0]}x{size[1]} 像素（当前 {image.width}x{image.height}）')
        width, height = (1 << (size[0] - 1).bit_length(), 1 << (size[1] - 1).bit_length())
        canvas = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        canvas.paste(image, (0, 0))
        texture = f'txrs/{prefix}a{n}.txr'
        build.resources[texture] = build_txr(width, height, canvas.tobytes())
        if key == 'loading':
            paths[offset] = _native(texture)
        else:
            sprite = f'sprs/{prefix}a{n}.spr'
            build.resources[sprite] = spr_bytes(_native(texture), size[0], size[1], (width, height))
            paths[offset] = _native(sprite)
    return paths


def banner_effect_factory(read_native: Callable[[str], bytes], base_effect: str = 'fxrs/0230.zfx'):
    """Banner effects clone a native text effect from the target client (reference only) and bind every
    emitter texture to the banner image. Returns a callable for compile_mission(banner_effect=...)."""
    from owner.units import effect_resources
    from PIL import Image
    import io
    from .txr_format import build_txr
    base = read_native(base_effect)
    carriers = effect_resources.texture_carriers(base)['textures']
    if not carriers:
        raise MissionError('横幅模板特效没有贴图槽')

    def make(png: bytes, fx_name: str) -> dict[str, bytes]:
        image = Image.open(io.BytesIO(png)).convert('RGBA')
        if image.size != (512, 128):
            image = image.resize((512, 128), Image.LANCZOS)
        texture_leaf = fx_name[:-4] + '.txr'
        patched = bytearray(base)
        encoded = (texture_leaf + '\0').encode('ascii')
        for carrier in carriers:
            if carrier['texture'] is None:
                continue
            if len(encoded) > carrier['length']:
                raise MissionError('横幅贴图名过长')
            patched[carrier['offset']:carrier['offset'] + len(encoded)] = encoded
        check = effect_resources.texture_carriers(bytes(patched))['textures']
        if any(c['texture'] not in (None, 'txrs/' + texture_leaf) for c in check):
            raise MissionError('横幅特效贴图替换读回不一致')
        return {'fxrs/' + fx_name: bytes(patched), 'txrs/' + texture_leaf: build_txr(512, 128, image.tobytes())}
    return make
