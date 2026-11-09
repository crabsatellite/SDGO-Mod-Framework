"""Apply a map import server plan (owner.maps.bundle) to the WorldServer game database.

Rows are keyed per table (see bundle.TABLE_KEYS). A key that already exists must either hold the same values
or be owned by an earlier map import (recorded in ``dbo.sdgo_MapImportRows``); anything else is refused
before writing. All writes run in one transaction under an application lock. Servers cache map tables, so
the Battle/World services must be restarted afterwards; this module never starts or stops processes.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from owner.units import repository

from .bundle import PLAN_SCHEMA, TABLE_KEYS

LEDGER = 'sdgo_MapImportRows'
APP_LOCK = 'sdgo.map_import'
COLUMNS = {
    'tbl_MapFile': ('MapNo', 'MapName', 'MapID', 'NormalMode', 'DeathMatchMode', 'TagMatch', 'GridMatch', 'MissionMode',
                    'LeaderMatch', 'AiMatch', 'AiBattleShipMatch', 'HotRandomMap', 'FastMatchRandom', 'Server_Training',
                    'Server_Beginner', 'Server_Expert', 'Server_Clan', 'Disable'),
    'tbl_MapData': ('Version', 'MapID', 'MinX', 'MinZ', 'MaxX', 'MaxZ', 'BlockCnt', 'NumItemPatternRed',
                    'NumItemPatternBlue'),
    'tbl_MapItemPath': ('MapID', 'Team', 'PatternID', 'Orders', 'Idx', 'PosX', 'PosY', 'PosZ'),
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
    'tbl_NpcData': ('NpcDataID', 'NpcName', 'UseUnitID', 'UseUnitHP', 'NpcPoint', 'ObjectHpUp', 'NpcType', 'AddUnitEXP',
                    'AddPoint', 'BossFlag', 'NpcUnitLevel', 'ItemBuffNumber', 'CreateUnitHp', 'DestroyUnitHp'),
}
# Insert order respects references (NPC data before spawns, modes before their rows).
TABLE_ORDER = ('tbl_MapData', 'tbl_MapFile', 'tbl_MapItemPath', 'tbl_NpcData', 'tbl_MissionMode', 'tbl_MissionModeStart',
               'tbl_MissionClear', 'tbl_MissionNpc', 'tbl_MissionExp', 'tbl_MissionRankBossData')


def load_plan(path) -> dict:
    plan = json.loads(Path(path).read_text(encoding='utf-8'))
    if plan.get('schema') != PLAN_SCHEMA or not isinstance(plan.get('tables'), dict):
        raise ValueError('不是地图导入服务端计划')
    for table, rows in plan['tables'].items():
        if table not in COLUMNS:
            raise ValueError('服务端计划包含未支持的表：' + table)
        for row in rows:
            if set(row) != set(COLUMNS[table]):
                raise ValueError(f'{table} 行的列与预期不符')
    return plan


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


def preflight(plan: dict, server_ini) -> dict:
    connection = repository._connect(Path(server_ini))
    try:
        return _plan_changes(connection.cursor(), plan, lock=False)
    finally:
        connection.rollback()
        connection.close()


def _plan_changes(cursor, plan: dict, lock: bool) -> dict:
    ledger_exists = cursor.execute(f"SELECT OBJECT_ID(N'dbo.{LEDGER}',N'U')").fetchone()[0] is not None
    owned = set()
    if ledger_exists:
        owned = {(r[0], r[1]) for r in cursor.execute(f'SELECT TableName,RowKey FROM dbo.{LEDGER}').fetchall()}
    changes = {'insert': [], 'update': [], 'keep': [], 'conflicts': []}
    for table in sorted(plan['tables'], key=TABLE_ORDER.index):
        rows = plan['tables'][table]
        for row in rows:
            current = _existing(cursor, table, row)
            key = _row_key(table, row)
            if current is None:
                changes['insert'].append((table, row))
            elif all(_same(current[c], row[c]) for c in COLUMNS[table]):
                changes['keep'].append((table, row))
            elif (table, key) in owned:
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
        result = _plan_changes(cursor, plan, lock=True)
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
