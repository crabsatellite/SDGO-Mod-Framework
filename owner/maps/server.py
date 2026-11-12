"""Apply a map import server plan (owner.maps.bundle) to the WorldServer game database.

Rows are keyed per table (see bundle.TABLE_KEYS). A key that already exists must either hold the same values
or be owned by an earlier publication of the same map owner (recorded in ``dbo.sdgo_MapImportRows``); anything else is refused
before writing. All writes run in one transaction under an application lock. Servers cache map tables, so
the Battle/World services must be restarted afterwards; this module never starts or stops processes.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

from owner.units import repository

from .bundle import LEGACY_PLAN_SCHEMAS, PLAN_SCHEMA, TABLE_KEYS
from . import block as block_protocol, mission as mission_protocol

LEDGER = 'sdgo_MapImportRows'
APP_LOCK = 'sdgo.map_import'
SERVER_POLICY_MOD = 'map_catalog_server'
SERVER_POLICY_KEY = 'ImmediateTransitionMissionIds'
COLUMNS = {
    'tbl_MapFile': ('MapNo', 'MapName', 'MapID', 'NormalMode', 'DeathMatchMode', 'TagMatch', 'GridMatch', 'MissionMode',
                    'LeaderMatch', 'AiMatch', 'AiBattleShipMatch', 'HotRandomMap', 'FastMatchRandom', 'Server_Training',
                    'Server_Beginner', 'Server_Expert', 'Server_Clan', 'Disable'),
    'tbl_MapData': ('Version', 'MapID', 'MinX', 'MinZ', 'MaxX', 'MaxZ', 'BlockCnt', 'NumItemPatternRed',
                    'NumItemPatternBlue'),
    'tbl_MapItemPath': ('MapID', 'Team', 'PatternID', 'Orders', 'Idx', 'PosX', 'PosY', 'PosZ'),
    'tbl_MapObjects': block_protocol.SERVER_OBJECT_COLUMNS,
    'tbl_MapBlocks': block_protocol.SERVER_BLOCK_COLUMNS,
    'tbl_MissionMode': ('GameModeID', 'ModeName', 'ModeType', 'MissionGroupIndex', 'GameMap', 'ScriptTime', 'GameTime',
                        'GameLife', 'GameNpc', 'GameAllyNpc', 'DropConditionID', 'NeedEvent', 'Min2PlayerTime',
                        'Min3PlayerTime', 'Min4PlayerTime'),
    'tbl_MissionClear': ('ClearID', 'ClearType', 'UseWho', 'UseHow', 'UseObjectID', 'UseObjectCount', 'px', 'py', 'pz',
                         'UseTime', 'AbsTime', 'AddPnt', 'AddUnitExp', 'MissionModeID', 'EventOrder', 'EventNext',
                         'SceneTime', 'FriendNpcRemove', 'EnemyNpcRemove', 'ZoneID', 'EventAreaX', 'EventAreaY',
                         'EventAreaZ', 'EventAreaR'),
    'tbl_MissionModeStart': ('MissionModeStartID', 'MissionID', 'MissionPart', 'FriendNPC', 'EnemyNPC', 'ExtendMode'),
    'tbl_MissionNpc': ('MissionNpcID', 'MissionID', 'MissionDifficult', 'MissionPart', 'NpcID', 'px', 'py', 'pz',
                       'AfterTime', 'AbsTime', 'ManualID', 'NpcParty', 'CreateLimit', 'ZoneIndex'),
    'tbl_MissionExp': ('MissionExpID', 'MissionID', 'MissionDifficulty', 'UseRankC', 'UserExp', 'UnitExp', 'UserPoint',
                       'UnitExpOnFail', 'Disable'),
    'tbl_MissionRankBossData': ('MissionID', 'DropConditionID', 'ClearPoint', 'DifficultPoint', 'BossID_1', 'BossID_2',
                                'BossID_3', 'BossID_4', 'BossID_5'),
    'tbl_WeaponsRev': mission_protocol.SERVER_WEAPON_COLUMNS,
    'tbl_UnitNpc': mission_protocol.SERVER_UNIT_NPC_COLUMNS,
    'tbl_NpcData': ('NpcDataID', 'NpcName', 'UseUnitID', 'UseUnitHP', 'NpcPoint', 'ObjectHpUp', 'NpcType', 'AddUnitEXP',
                    'AddPoint', 'BossFlag', 'NpcUnitLevel', 'ItemBuffNumber', 'CreateUnitHp', 'DestroyUnitHp'),
    # mission drops: the drop setting picks a normal and a rare dice machine; dice rows (identity idx) are keyed by
    # machine and cumulative probability
    'tbl_BattleResultItemDropSetting': ('DropConditionID', 'NormalMachineID', 'RareMachineID', 'UniqueMachineID'),
    'tbl_BattleResultItemDropDice': ('ItemMachineID', 'CategoryID', 'Prob', 'ItemName', 'Amount', 'Opt', 'RareDice',
                                     'SumProb'),
}
# Insert order respects references (weapons -> units -> NPC data -> spawns); removal uses the reverse order.
TABLE_ORDER = ('tbl_MapObjects', 'tbl_MapData', 'tbl_MapBlocks', 'tbl_MapFile', 'tbl_MapItemPath', 'tbl_WeaponsRev', 'tbl_UnitNpc', 'tbl_NpcData', 'tbl_BattleResultItemDropDice',
               'tbl_BattleResultItemDropSetting', 'tbl_MissionMode', 'tbl_MissionModeStart', 'tbl_MissionClear',
               'tbl_MissionNpc', 'tbl_MissionExp', 'tbl_MissionRankBossData')


UNIT_NPC_SCHEMA = (
    ('UnitNumber', 'int', 4, False), ('CustomNumber', 'int', 4, False),
    ('Influence1', 'tinyint', 1, False), ('Influence2', 'tinyint', 1, False),
    ('UnitModelName', 'varchar', 24, False), ('UnitGradeNumber', 'tinyint', 1, False),
    ('UnitGradeNumber2', 'tinyint', 1, False), ('MasteredWeaponType', 'tinyint', 1, False),
    ('BattleCost', 'smallint', 2, False), ('Star', 'tinyint', 1, False),
    ('UnitNumber_Transform', 'int', 4, False), ('HP', 'int', 4, False), ('Shield', 'int', 4, True),
    ('ShieldType', 'tinyint', 1, False), ('ShieldRate', 'int', 4, True), ('DashType', 'tinyint', 1, False),
    ('MoveLevel', 'tinyint', 1, False), ('DashLevel', 'tinyint', 1, False),
    ('QuickStepLevel', 'tinyint', 1, False), ('MotivePower', 'tinyint', 1, False),
    ('BoosterIncrease', 'smallint', 2, False), ('BoosterDecrease', 'smallint', 2, False),
    ('FatalAttackIncrease', 'smallint', 2, False), ('AdaptEnvironType', 'tinyint', 1, False),
    ('AdaptEnvironSpace', 'smallint', 2, False), ('AdaptEnvironGround', 'smallint', 2, False),
    ('AdaptEnvironUnderwater', 'smallint', 2, False), ('AdaptEnvironDesert', 'smallint', 2, False),
    ('AdaptEnvironSky', 'smallint', 2, False), ('Sight', 'smallint', 2, False),
    ('SkillNumber_1', 'int', 4, False), ('SkillNumber_2', 'int', 4, False),
    ('SkillNumber_3', 'int', 4, False), ('StrikingPower', 'smallint', 2, False),
    ('DefensivePower', 'smallint', 2, False), ('MasteredWeaponIX', 'int', 4, False),
    ('WeaponNumber_1', 'int', 4, True), ('WeaponNumber_2', 'int', 4, True),
    ('WeaponNumber_3', 'int', 4, True), ('WeaponNumber_FatalAttack', 'int', 4, True),
    ('ViewOrder', 'int', 4, False), ('MaxEnergyAmount', 'real', 4, True),
    ('MaxEnergyCost', 'real', 4, True), ('EnergyDecAmount', 'tinyint', 1, True),
    ('UnitGrowIndex', 'int', 4, True), ('DailyLimit', 'int', 4, True),
    ('MonthLimit', 'int', 4, True), ('UserGradeLimit', 'int', 4, True),
    ('SallyReady', 'tinyint', 1, True), ('CollectionUnit', 'tinyint', 1, True),
)
_INTEGER_RANGES = {'tinyint': (0, 255), 'smallint': (-32768, 32767), 'int': (-2147483648, 2147483647)}


def _validate_catalog_value(table: str, name: str, value, kind: str, size: int, nullable: bool) -> None:
    if value is None:
        if nullable:
            return
        raise ValueError(f'{table}.{name} 不能为空')
    if kind in _INTEGER_RANGES:
        if isinstance(value, bool) or not isinstance(value, int) or not _INTEGER_RANGES[kind][0] <= value <= _INTEGER_RANGES[kind][1]:
            raise ValueError(f'{table}.{name} 不是有效的 {kind}')
    elif kind == 'real':
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
            raise ValueError(f'{table}.{name} 不是有限数字')
    elif kind == 'varchar':
        # Legacy weapon names may contain embedded NUL bytes.  They are cloned byte-for-byte from the fixed
        # server catalogue, so validate type/declared width without rewriting or normalising them here.
        if not isinstance(value, str) or len(value) > size:
            raise ValueError(f'{table}.{name} 不是长度不超过 {size} 的文本')
    else:
        raise AssertionError(kind)


def _validate_paired_catalog(plan: dict) -> None:
    schemas = {'tbl_UnitNpc': UNIT_NPC_SCHEMA, 'tbl_WeaponsRev': repository.SQL_SCHEMAS['tbl_WeaponsRev']}
    for table, schema in schemas.items():
        for row in plan['tables'].get(table, []):
            for name, kind, size, nullable in schema:
                _validate_catalog_value(table, name, row[name], kind, size, nullable)
    unit_ids = {row['UnitNumber'] for row in plan['tables'].get('tbl_UnitNpc', [])}
    weapon_ids = {row['WeaponNo'] for row in plan['tables'].get('tbl_WeaponsRev', [])}
    unit_end = mission_protocol.UNIT_LABEL_FIRST + 256 * 100
    weapon_end = mission_protocol.WEAPON_COPY_FIRST + 256 * 100
    if any(not mission_protocol.UNIT_LABEL_FIRST <= value < unit_end for value in unit_ids):
        raise ValueError('tbl_UnitNpc 含任务派生范围外的机体编号')
    if any(not mission_protocol.WEAPON_COPY_FIRST <= value < weapon_end
           for value in weapon_ids):
        raise ValueError('tbl_WeaponsRev 含任务派生范围外的武器编号')
    referenced = set()
    for row in plan['tables'].get('tbl_UnitNpc', []):
        slots = {int(row[name] or 0) for name in mission_protocol.SERVER_UNIT_WEAPON_FIELDS}
        cloned = slots & weapon_ids
        if not cloned:
            raise ValueError(f"tbl_UnitNpc {row['UnitNumber']} 没有引用同计划的派生武器")
        if any(mission_protocol.WEAPON_COPY_FIRST <= value < weapon_end
               and value not in weapon_ids for value in slots):
            raise ValueError(f"tbl_UnitNpc {row['UnitNumber']} 引用了缺失的派生武器")
        referenced.update(cloned)
    if referenced != weapon_ids:
        raise ValueError('服务端计划含未被派生机体引用的武器')
    npc_units = {row['UseUnitID'] for row in plan['tables'].get('tbl_NpcData', [])}
    missing = sorted(unit_ids - npc_units)
    if missing:
        raise ValueError('派生机体没有匹配的 NpcData：' + ','.join(map(str, missing)))



def _validate_blocks(plan: dict) -> None:
    objects = plan['tables'].get('tbl_MapObjects', [])
    placements = plan['tables'].get('tbl_MapBlocks', [])
    object_ids = set()
    for row in objects:
        identity = row['ObjectID']
        if type(identity) is not int or not 1 <= identity <= 0x7fffffff or identity in object_ids:
            raise ValueError('tbl_MapObjects.ObjectID 无效或重复')
        object_ids.add(identity)
        name = row['ObjectName']
        if not isinstance(name, str) or not name or len(name) > 50 or '\0' in name:
            raise ValueError('tbl_MapObjects.ObjectName 无效')
        hp = row['HP']
        if hp is not None and (type(hp) is not int or not 1 <= hp <= 0x7fffffff):
            raise ValueError('tbl_MapObjects.HP 无效')
        for field in ('CollisionDestroy', 'TeamKill'):
            if type(row[field]) is not int or not 0 <= row[field] <= 255:
                raise ValueError(f'tbl_MapObjects.{field} 无效')
        for field in ('ExplosionTime', 'Damage', 'FragmentType'):
            value = row[field]
            if value is not None and (type(value) is not int or not -32768 <= value <= 32767):
                raise ValueError(f'tbl_MapObjects.{field} 无效')
        for field in ('ExplosionRadius', 'ExplosionRadiusPower'):
            value = row[field]
            if value is not None and (isinstance(value, bool) or not isinstance(value, (int, float))
                                      or not math.isfinite(float(value))):
                raise ValueError(f'tbl_MapObjects.{field} 无效')

    by_map = {}
    referenced = set()
    for row in placements:
        if type(row['MapID']) is not int or type(row['BlockID']) is not int or type(row['ObjectID']) is not int:
            raise ValueError('tbl_MapBlocks 的编号字段无效')
        if row['ObjectID'] not in object_ids:
            raise ValueError(f"tbl_MapBlocks 引用了计划外的 ObjectID {row['ObjectID']}")
        referenced.add(row['ObjectID'])
        bounds = [row[name] for name in ('MinX', 'MinY', 'MinZ', 'MaxX', 'MaxY', 'MaxZ')]
        if any(isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value))
               for value in bounds):
            raise ValueError('tbl_MapBlocks 含无效碰撞范围')
        if not all(bounds[index] < bounds[index + 3] for index in range(3)):
            raise ValueError('tbl_MapBlocks 的最小范围必须小于最大范围')
        if any(type(row[name]) is not int or row[name] != 0 for name in ('Child_No_1', 'Child_No_2', 'Child_No_3')):
            raise ValueError('导入的 tbl_MapBlocks 子物件编号必须为 0')
        by_map.setdefault(row['MapID'], []).append(row['BlockID'])
    if referenced != object_ids:
        raise ValueError('tbl_MapObjects 含未被 tbl_MapBlocks 引用的定义')
    map_rows = {row['MapID']: row for row in plan['tables'].get('tbl_MapData', [])}
    for map_id, block_ids in by_map.items():
        expected = list(range(block_protocol.FIRST_BLOCK_ID, block_protocol.FIRST_BLOCK_ID + len(block_ids)))
        if sorted(block_ids) != expected:
            raise ValueError(f'tbl_MapBlocks MapID {map_id} 的 BlockID 必须从 7001 连续编号')
        if map_id not in map_rows or map_rows[map_id]['BlockCnt'] != len(block_ids):
            raise ValueError(f'tbl_MapData MapID {map_id} 的 BlockCnt 与 tbl_MapBlocks 不一致')
    for map_id, row in map_rows.items():
        if row['BlockCnt'] != len(by_map.get(map_id, [])):
            raise ValueError(f'tbl_MapData MapID {map_id} 的 BlockCnt 与命名物件数量不一致')

def _validate_runtime_policy(plan: dict) -> list[int]:
    policy = plan.get('runtime_policy')
    if policy is None:
        policy = {}
    if not isinstance(policy, dict) or set(policy) - {'immediate_transition_missions'}:
        raise ValueError('服务端计划的 runtime_policy 无效')
    mission_ids = policy.get('immediate_transition_missions', [])
    if (not isinstance(mission_ids, list)
            or any(type(value) is not int or value <= 0 or value > 0xffffffff for value in mission_ids)
            or mission_ids != sorted(set(mission_ids))):
        raise ValueError('服务端计划的 immediate_transition_missions 必须是递增且不重复的 uint32 正整数列表')
    rendered = ','.join(map(str, mission_ids))
    if len(mission_ids) > 256 or len(rendered.encode('ascii')) >= 2047:
        raise ValueError('服务端计划的 immediate_transition_missions 超过运行时名单容量')
    planned_modes = {row.get('GameModeID') for row in plan.get('tables', {}).get('tbl_MissionMode', [])
                     if isinstance(row, dict)}
    missing = [value for value in mission_ids if value not in planned_modes]
    if missing:
        raise ValueError('服务端节奏策略引用了计划外的 MissionModeID：' + ','.join(map(str, missing)))
    plan['runtime_policy'] = {'immediate_transition_missions': mission_ids}
    return mission_ids


def load_plan(path) -> dict:
    plan = json.loads(Path(path).read_text(encoding='utf-8'))
    schema = plan.get('schema')
    if ((schema != PLAN_SCHEMA and schema not in LEGACY_PLAN_SCHEMAS)
            or not isinstance(plan.get('tables'), dict)):
        raise ValueError('不是地图导入服务端计划')
    if schema == 'sdgo.map-import-server-plan/1':
        if plan.get('runtime_policy') not in (None, {}, {'immediate_transition_missions': []}):
            raise ValueError('旧版服务端计划不能声明 runtime_policy，请用当前协议重新编译')
        if any(table in plan['tables'] for table in ('tbl_WeaponsRev', 'tbl_UnitNpc')):
            raise ValueError('旧版服务端计划不能声明配对机体/武器表，请用当前协议重新编译')
        plan['runtime_policy'] = {'immediate_transition_missions': []}
    if schema in LEGACY_PLAN_SCHEMAS and any(
            table in plan['tables'] for table in ('tbl_MapObjects', 'tbl_MapBlocks')):
        raise ValueError('旧版服务端计划不能声明命名物件表，请用当前协议重新编译')
    for table, rows in plan['tables'].items():
        if table not in COLUMNS:
            raise ValueError('服务端计划包含未支持的表：' + table)
        if not isinstance(rows, list):
            raise ValueError(f'{table} 行列表无效')
        seen = set()
        for row in rows:
            if not isinstance(row, dict) or set(row) != set(COLUMNS[table]):
                raise ValueError(f'{table} 行的列与预期不符')
            key = tuple(row[name] for name in TABLE_KEYS[table])
            if key in seen:
                raise ValueError(f'{table} 含重复键 {key}')
            seen.add(key)
    _validate_paired_catalog(plan)
    _validate_blocks(plan)
    _validate_runtime_policy(plan)
    return plan


def _configured_mission_ids(raw: bytes) -> list[int] | None:
    from manager import storage
    try:
        value = storage.ini_values(raw).get('settings', SERVER_POLICY_KEY, fallback='').strip()
    except (ValueError, UnicodeError):
        return None
    if not value:
        return []
    if len(value.encode('utf-8')) >= 2047:
        return None
    parts = [part.strip() for part in value.replace(';', ',').split(',')]
    if (len(parts) > 256
            or any(not part or not part.isascii() or not part.isdecimal() for part in parts)):
        return None
    values = [int(part, 10) for part in parts]
    if any(value <= 0 or value > 0xffffffff for value in values):
        return None
    return sorted(set(values))


def _battle_policy_hosts(server_ini) -> list[dict]:
    from manager import operations, storage
    _, _, consumers = operations.topology(Path(server_ini))
    result = []
    for host in sorted({node.parent for node in consumers if node.name == 'BattleServer.exe'}, key=str):
        mod = host / 'sdgo/mods' / SERVER_POLICY_MOD
        dll = mod / (SERVER_POLICY_MOD + '.dll')
        mod_ini = mod / 'mod.ini'
        symbols = mod / 'symbols.fragment.ini'
        config = mod / 'config.ini'
        problem = ''
        values = None
        if (mod.is_symlink() or dll.is_symlink() or mod_ini.is_symlink() or symbols.is_symlink()
                or config.is_symlink()):
            problem = '模块目录或程序文件不能是目录链接'
        elif not dll.is_file() or not mod_ini.is_file() or not symbols.is_file():
            problem = '缺少 DLL、mod.ini 或 symbols.fragment.ini'
        else:
            try:
                values = storage.ini_values(mod_ini.read_bytes())
                if values.get('mod', 'id', fallback='').strip() != SERVER_POLICY_MOD:
                    problem = 'mod.ini 的模块标识不匹配'
            except (OSError, ValueError, UnicodeError):
                problem = 'mod.ini 无法读取'
        installed = not problem
        enabled = global_enabled = False
        if installed:
            enabled = values.get('mod', 'enabled', fallback='1').strip() != '0'
            category = values.get('mod', 'category', fallback='gameplay').strip()
            feature = values.get('mod', 'feature', fallback='map-import').strip()
            gates_path = host / 'sdgo/sdgo.ini'
            try:
                gates = storage.ini_values(gates_path.read_bytes()) if gates_path.is_file() else None
                global_enabled = gates is None or all(
                    gates.get(section, key, fallback='1').strip() != '0'
                    for section, key in (('categories', category), ('features', feature),
                                         ('mods', SERVER_POLICY_MOD)))
            except (OSError, ValueError, UnicodeError):
                global_enabled = False
                problem = 'sdgo.ini 无法读取'
        configured = _configured_mission_ids(config.read_bytes()) if installed and config.is_file() else []
        result.append({'host': host.name, 'path': host, 'mod': mod, 'installed': installed,
                       'enabled': enabled, 'global_enabled': global_enabled, 'configured_ids': configured,
                       'problem': problem})
    return result


def runtime_policy_state(plan: dict, server_ini) -> dict:
    mission_ids = _validate_runtime_policy(plan)
    try:
        hosts = _battle_policy_hosts(server_ini)
        topology_error = ''
    except ValueError as error:
        hosts = []
        topology_error = str(error)
    missing = [row['host'] for row in hosts if not row['installed']] if mission_ids else []
    if topology_error:
        missing = []
    elif mission_ids and not hosts:
        missing = ['BattleServer']
    expected = mission_ids
    public_hosts = []
    for row in hosts:
        configured = row['configured_ids'] == expected
        public_hosts.append({'host': row['host'], 'installed': row['installed'], 'configured': configured,
                             'enabled': row['enabled'], 'global_enabled': row['global_enabled'],
                             'problem': row['problem']})
    if topology_error:
        status = 'topology_error'
    elif mission_ids and missing:
        status = 'pending_programme_deployment'
    elif any(row['installed'] and row['configured_ids'] != expected for row in hosts):
        status = 'configuration_required'
    elif mission_ids and any(not row['enabled'] or not row['global_enabled'] for row in hosts):
        status = 'activation_required'
    elif mission_ids:
        # Disk configuration alone cannot prove that a running BattleServer has loaded the hook.
        status = 'configured'
    else:
        status = 'native'
    configured_hosts = [row['host'] for row in hosts
                        if row['installed'] and row['configured_ids'] == expected]
    ready_hosts = [row['host'] for row in hosts if row['installed'] and row['enabled'] and
                   row['global_enabled'] and row['configured_ids'] == expected]
    return {'status': status, 'required_module': SERVER_POLICY_MOD, 'mission_ids': mission_ids,
            'hosts': public_hosts, 'missing_hosts': missing, 'configured_hosts': configured_hosts,
            'ready_hosts': ready_hosts, 'topology_error': topology_error,
            'policy_active': False, 'runtime_verified': False,
            'restart_required': status in ('configuration_required', 'activation_required', 'configured')}


def apply_runtime_policy(plan: dict, server_ini) -> dict:
    """Write the planned mission allowlist to every BattleServer that has the required programme.

    Missing programmes are reported as a deployment-pending state and never represented as active. All target
    bytes are prepared before writing; if any write or readback fails, earlier configuration writes are restored.
    """
    from manager import storage
    mission_ids = _validate_runtime_policy(plan)
    before = runtime_policy_state(plan, server_ini)
    if before['topology_error'] or (mission_ids and before['status'] == 'pending_programme_deployment'):
        return before
    hosts = [row for row in _battle_policy_hosts(server_ini) if row['installed']]
    if not hosts:
        return before
    desired = ','.join(map(str, mission_ids))
    prepared = []
    for row in hosts:
        config = row['mod'] / 'config.ini'
        config_raw = config.read_bytes() if config.is_file() else b'[settings]\n'
        mod_ini = row['mod'] / 'mod.ini'
        mod_raw = mod_ini.read_bytes()
        config_new = storage.patch_ini(config_raw, [('settings', SERVER_POLICY_KEY, desired)])
        mod_new = storage.patch_ini(mod_raw, [('mod', 'enabled', '1')])
        if _configured_mission_ids(config_new) != mission_ids:
            raise ValueError('节奏策略配置生成后读回不一致：' + row['host'])
        prepared.append((row['host'], config, config_raw if config.is_file() else None, config_new,
                         mod_ini, mod_raw, mod_new))
    written = []
    try:
        # Configure every host before enabling any host, so no BattleServer can load a half-written allowlist.
        for _, config, original, data, _, _, _ in prepared:
            if original != data:
                storage.atomic(config, data)
                written.append((config, original))
            if config.read_bytes() != data:
                raise ValueError('配置写入后读回不一致')
        for _, _, _, _, mod_ini, original, data in prepared:
            if data != original:
                storage.atomic(mod_ini, data)
                written.append((mod_ini, original))
            if mod_ini.read_bytes() != data:
                raise ValueError('模块开关写入后读回不一致')
    except Exception as error:
        rollback_errors = []
        for target, original in reversed(written):
            try:
                if original is None:
                    storage.remove(target)
                else:
                    storage.atomic(target, original)
            except Exception as rollback_error:
                rollback_errors.append(target.name + ':' + type(rollback_error).__name__)
        reason = str(error) if isinstance(error, ValueError) else type(error).__name__
        if rollback_errors:
            raise ValueError('服务端节奏策略写入失败，且部分原配置未能恢复：' + reason + '；' +
                             ','.join(rollback_errors)) from None
        raise ValueError('服务端节奏策略写入失败，已恢复原配置：' + reason) from None
    result = runtime_policy_state(plan, server_ini)
    changed = any(original != data or mod_original != mod_data
                  for _, _, original, data, _, mod_original, mod_data in prepared)
    result['configured_hosts'] = [row[0] for row in prepared]
    result['configuration_changed'] = changed
    result['restart_required'] = changed
    return result


def _same(a, b) -> bool:
    if isinstance(a, float) or isinstance(b, float):
        return abs(float(a) - float(b)) <= 1e-3 * max(1.0, abs(float(a)))
    return a == b if not isinstance(a, str) else a.rstrip() == str(b).rstrip()


def _ensure_ledger(cursor) -> None:
    cursor.execute(f"IF OBJECT_ID(N'dbo.{LEDGER}',N'U') IS NULL CREATE TABLE dbo.{LEDGER}("
                   "TableName sysname NOT NULL, RowKey nvarchar(200) NOT NULL, MapKey nvarchar(128) NOT NULL,"
                   "Revision nvarchar(64) NOT NULL, RowJson nvarchar(max) NOT NULL, UpdatedAt datetime2(0) NOT NULL,"
                   "CONSTRAINT PK_sdgo_MapImportRows PRIMARY KEY(TableName,RowKey))")


def _row_key(table: str, row: dict) -> str:
    return json.dumps([row[k] for k in TABLE_KEYS[table]])


def _existing(cursor, table: str, row: dict):
    where = ' AND '.join(f'{k}=?' for k in TABLE_KEYS[table])
    found = cursor.execute(f'SELECT {",".join(COLUMNS[table])} FROM dbo.{table} WITH (UPDLOCK,HOLDLOCK) WHERE {where}',
                           *[row[k] for k in TABLE_KEYS[table]]).fetchall()
    if len(found) > 1:
        raise ValueError(f'{table} 中键 {_row_key(table, row)} 不唯一')
    return dict(zip(COLUMNS[table], found[0])) if found else None


def preflight(plan: dict, server_ini, map_key: str | None = None) -> dict:
    connection = repository._connect(Path(server_ini))
    try:
        return _plan_changes(connection.cursor(), plan, lock=False, map_key=map_key)
    finally:
        connection.rollback()
        connection.close()


def _plan_changes(cursor, plan: dict, lock: bool, map_key: str | None = None) -> dict:
    ledger_exists = cursor.execute(f"SELECT OBJECT_ID(N'dbo.{LEDGER}',N'U')").fetchone()[0] is not None
    owned = {}
    if ledger_exists:
        owned = {(r[0], r[1]): r[2] for r in
                 cursor.execute(f'SELECT TableName,RowKey,MapKey FROM dbo.{LEDGER}').fetchall()}
    changes = {'insert': [], 'update': [], 'keep': [], 'conflicts': []}
    for table in sorted(plan['tables'], key=TABLE_ORDER.index):
        rows = plan['tables'][table]
        for row in rows:
            current = _existing(cursor, table, row)
            key = _row_key(table, row)
            row_owner = owned.get((table, key))
            if current is None:
                changes['insert'].append((table, row))
            elif map_key is not None and row_owner is not None and row_owner != map_key:
                # Even an identical row remains owned by the other map.  Treating it as keep would let the
                # apply MERGE silently transfer its ledger ownership to this map_key.
                changes['conflicts'].append({'table': table, 'key': key})
            elif all(_same(current[c], row[c]) for c in COLUMNS[table]):
                changes['keep'].append((table, row))
            elif map_key is not None and row_owner == map_key:
                changes['update'].append((table, row))
            else:
                changes['conflicts'].append({'table': table, 'key': key})
    summary = {k: len(v) for k, v in changes.items() if k != 'conflicts'}
    summary['conflicts'] = changes['conflicts']
    summary['_changes'] = changes
    return summary


def apply(plan: dict, server_ini, map_key: str) -> dict:
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()
        cursor.execute('SET XACT_ABORT ON')
        cursor.execute('BEGIN TRANSACTION')
        lock = cursor.execute("DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,@LockMode=N'Exclusive',"
                              "@LockOwner=N'Transaction',@LockTimeout=5000; SELECT @r;", APP_LOCK).fetchone()
        if lock is None or int(lock[0]) < 0:
            raise ValueError('另一项地图导入正在进行，请稍后重试')
        _ensure_ledger(cursor)
        result = _plan_changes(cursor, plan, lock=True, map_key=map_key)
        if result['conflicts']:
            raise ValueError('服务端已有不属于地图导入的同键行：' +
                             ', '.join(f"{c['table']} {c['key']}" for c in result['conflicts'][:8]))
        changes = result.pop('_changes')
        # Rows an earlier publication of this owner key recorded but this plan no longer has (a removed
        # map, fewer spawns, ...) are deleted first, in reverse dependency order.
        planned = {(table, _row_key(table, row)) for table, rows in plan['tables'].items() for row in rows}
        stale = [(t, k) for t, k in cursor.execute(f'SELECT TableName,RowKey FROM dbo.{LEDGER} WHERE MapKey=?',
                                                    map_key).fetchall() if (t, k) not in planned]
        pruned = 0
        for table in reversed(TABLE_ORDER):
            for name, key in stale:
                if name == table:
                    where = ' AND '.join(f'{k}=?' for k in TABLE_KEYS[table])
                    pruned += cursor.execute(f'DELETE FROM dbo.{table} WHERE {where}', *json.loads(key)).rowcount
        for name, key in stale:
            cursor.execute(f'DELETE FROM dbo.{LEDGER} WHERE TableName=? AND RowKey=?', name, key)
        for table, row in changes['insert']:
            columns = COLUMNS[table]
            cursor.execute(f'INSERT dbo.{table}({",".join(columns)}) VALUES({",".join("?" * len(columns))})',
                           *[row[c] for c in columns])
        for table, row in changes['update']:
            keys = TABLE_KEYS[table]
            values = [c for c in COLUMNS[table] if c not in keys]
            cursor.execute(f'UPDATE dbo.{table} SET {",".join(f"{c}=?" for c in values)} WHERE '
                           f'{" AND ".join(f"{k}=?" for k in keys)}', *[row[c] for c in values], *[row[k] for k in keys])
        for table, row in changes['insert'] + changes['update'] + changes['keep']:
            key = _row_key(table, row)
            body = json.dumps(row, ensure_ascii=False, sort_keys=True)
            cursor.execute(f'MERGE dbo.{LEDGER} AS t USING (SELECT ? AS TableName, ? AS RowKey) AS s '
                           'ON t.TableName=s.TableName AND t.RowKey=s.RowKey '
                           'WHEN MATCHED THEN UPDATE SET MapKey=?, Revision=?, RowJson=?, UpdatedAt=SYSUTCDATETIME() '
                           'WHEN NOT MATCHED THEN INSERT(TableName,RowKey,MapKey,Revision,RowJson,UpdatedAt) '
                           'VALUES(?,?,?,?,?,SYSUTCDATETIME());',
                           table, key, map_key, plan['revision'], body, table, key, map_key, plan['revision'], body)
        cursor.execute('COMMIT TRANSACTION')
        connection.commit()
        return {'inserted': len(changes['insert']), 'updated': len(changes['update']), 'kept': len(changes['keep']),
                'removed': pruned, 'revision': plan['revision'], 'restart_required': True}
    except ValueError:
        connection.rollback()
        raise
    except Exception as error:
        connection.rollback()
        raise ValueError('地图服务端行写入失败，已全部回滚：' + type(error).__name__) from None
    finally:
        connection.close()


def readback(plan: dict, server_ini) -> dict:
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()
        missing, differ = [], []
        for table, rows in plan['tables'].items():
            for row in rows:
                current = _existing(cursor, table, row)
                if current is None:
                    missing.append((table, _row_key(table, row)))
                elif not all(_same(current[c], row[c]) for c in COLUMNS[table]):
                    differ.append((table, _row_key(table, row)))
        return {'ok': not missing and not differ, 'missing': missing, 'differ': differ,
                'digest': hashlib.sha256(json.dumps(plan['tables'], sort_keys=True).encode()).hexdigest()}
    finally:
        connection.rollback()
        connection.close()


def remove(map_key: str, server_ini, *, drop_empty_ledger: bool = False) -> dict:
    """Delete every row a map import recorded for `map_key` (reverse dependency order) and its ledger rows."""
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()
        cursor.execute('SET XACT_ABORT ON')
        cursor.execute('BEGIN TRANSACTION')
        lock = cursor.execute("DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,@LockMode=N'Exclusive',"
                              "@LockOwner=N'Transaction',@LockTimeout=5000; SELECT @r;", APP_LOCK).fetchone()
        if lock is None or int(lock[0]) < 0:
            raise ValueError('另一项地图导入正在进行，请稍后重试')
        if cursor.execute(f"SELECT OBJECT_ID(N'dbo.{LEDGER}',N'U')").fetchone()[0] is None:
            return {'deleted': 0}
        owned = cursor.execute(f'SELECT TableName,RowKey FROM dbo.{LEDGER} WHERE MapKey=?', map_key).fetchall()
        deleted = 0
        for table in reversed(TABLE_ORDER):
            for name, key in owned:
                if name != table:
                    continue
                values = json.loads(key)
                where = ' AND '.join(f'{k}=?' for k in TABLE_KEYS[table])
                deleted += cursor.execute(f'DELETE FROM dbo.{table} WHERE {where}', *values).rowcount
        cursor.execute(f'DELETE FROM dbo.{LEDGER} WHERE MapKey=?', map_key)
        if drop_empty_ledger and cursor.execute(f'SELECT COUNT(*) FROM dbo.{LEDGER}').fetchone()[0] == 0:
            cursor.execute(f'DROP TABLE dbo.{LEDGER}')
        cursor.execute('COMMIT TRANSACTION')
        connection.commit()
        return {'deleted': deleted, 'ledger_rows': len(owned)}
    except ValueError:
        connection.rollback()
        raise
    except Exception as error:
        connection.rollback()
        raise ValueError('地图服务端行删除失败，已全部回滚：' + type(error).__name__) from None
    finally:
        connection.close()
