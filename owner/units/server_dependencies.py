"""Owned server skill/funnel definitions inside the caller's catalog transaction.

This data writer never admits a native record or actor. The supplied complete
client records and stock-template hashes are rechecked before any row changes.
Only new or unchanged, same-owner definitions can be written; baseline rows
and out-of-band edits are refused. No accounts, inventories or services are used.
"""
from __future__ import annotations

import hashlib
import copy
import json
import math
import re
import struct

from .repository import APP_LOCK, SKILL_COLUMNS, FUNNEL_COLUMNS, _validate_scalar
from .profiles.skill_target_1132 import recover_source_record

SCHEMA = 'sdgo.unit-server-dependencies/1'
FUNNEL_SCHEMA = 'sdgo.unit-server-funnels/1'
TABLE = 'tbl_SkillRev'
LEDGER = 'sdgo_UnitImportDependencies'
_SHARED = {
    'Activate': (155, 'B'), 'ActivateValue': (156, 'h'),
    'MoveLv': (258, 'h'), 'DashLv': (260, 'h'), 'QuickLv': (262, 'h'),
    'MotiveLv': (264, 'h'), 'BoostIncreaseRate': (266, 'f'),
    'BoostPower': (270, 'h'), 'Attack': (272, 'h'), 'Defense': (274, 'h'),
    'RadarRadius': (276, 'h'), 'Feature': (352, 'B'), 'UseTarget': (353, 'B'),
}
_REAL = {'BoostIncreaseRate', 'Scale', 'AutoBulletChargeRate', 'BulletChargeRate', 'AtkSpdRate'}
_BYTE = {'Activate', 'Feature', 'UseTarget', 'BattleMode', 'PartSpLevel'}
SQL_SCHEMA = tuple((name, 'int', 4, False) if name == 'SkillID' else
    (name, 'varchar', 50, False) if name == 'SkillName' else
    (name, 'real', 4, False) if name in _REAL else
    (name, 'tinyint', 1, False) if name in _BYTE else
    (name, 'smallint', 2, False) for name in SKILL_COLUMNS)
FUNNEL_SQL_SCHEMA = tuple((name, 'tinyint', 1, False) if name in ('AttackCount','Stealable') else
                         (name, 'int', 4, False) for name in FUNNEL_COLUMNS)
_FUNNEL_SHARED = {'FunnelProp':(5,'B'),'AniTime':(719,'I'),'AttackDelay':(723,'I'),
                  'AttackCount':(727,'I'),'Stealable':(731,'B')}


def _spec(kind):
    if kind=='skill':return SCHEMA,'skills',TABLE,SKILL_COLUMNS,'SkillID',SQL_SCHEMA,1132,_SHARED
    if kind=='funnel':return FUNNEL_SCHEMA,'funnels','tbl_Funnel',FUNNEL_COLUMNS,'FunnelID',FUNNEL_SQL_SCHEMA,732,_FUNNEL_SHARED
    raise ValueError('unsupported server dependency record kind')
LEDGER_SCHEMA = (
    ('RecordKind', 'varchar', 16, False), ('RecordID', 'int', 4, False),
    ('OwnerKey', 'nvarchar', 128, False), ('Revision', 'char', 64, False),
    ('ClientHash', 'char', 64, False), ('DefinitionJson', 'nvarchar', -1, False),
    ('InstalledHash', 'char', 64, False), ('UpdatedAt', 'datetime2', 6, False),
)


def _exact(value, fields, label):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError(label + ' fields differ')


def _hash(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('server dependency SHA-256 is invalid')
    return value


def _id(value):
    if type(value) is not int or not 1 <= value <= 9_999_999:
        raise ValueError('server dependency record ID is invalid')
    return value


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False)


def normalize_row(value, *, kind='skill'):
    _,_,_,columns,key,schema,_,_=_spec(kind)
    _exact(value, columns, 'server '+kind+' row')
    result = dict(value)
    for name, sql_kind, size, nullable in schema:
        _validate_scalar(name, value[name], sql_kind, size, nullable)
        if sql_kind == 'real':
            result[name] = struct.unpack('<f', struct.pack('<f', value[name]))[0]
            if not math.isfinite(result[name]): raise ValueError('server skill real value is not finite')
    _id(result[key])
    if kind=='skill' and not result['SkillName'].strip(): raise ValueError('server skill name must be supplied')
    return result


def normalize_partial_row(value, *, kind='skill'):
    """Validate supplied authoring fields without manufacturing missing values."""
    _,_,_,columns,_,schema,_,_=_spec(kind)
    if not isinstance(value,dict) or set(value)-set(columns):
        raise ValueError('server '+kind+' authoring fields differ')
    result={}
    for name,sql_kind,size,nullable in schema:
        if name not in value:continue
        _validate_scalar(name,value[name],sql_kind,size,nullable)
        result[name]=struct.unpack('<f',struct.pack('<f',value[name]))[0] if sql_kind=='real' else value[name]
        if sql_kind=='real' and not math.isfinite(result[name]):raise ValueError('server skill real value is not finite')
    if kind=='skill' and 'SkillName' in result and not result['SkillName'].strip():
        raise ValueError('server skill name must be supplied')
    return result


def paired_values(raw, *, kind='skill'):
    """Only fields with an audited shared encoding; dispatch is not SQL BattleMode."""
    _,_,_,_,key,_,extent,shared=_spec(kind)
    if not isinstance(raw,bytes) or len(raw)!=extent or raw[4]!=0:
        raise ValueError('server dependency complete client record differs')
    return {key:struct.unpack_from('<I',raw)[0],**{
        name:struct.unpack_from('<'+primitive,raw,offset)[0] for name,(offset,primitive) in shared.items()}}


def row_hash(value, *, kind='skill'):
    return hashlib.sha256(canonical(normalize_row(value,kind=kind)).encode('utf-8')).hexdigest()


def validate_manifest(value, client_records, *, kind='skill', authoring=False):
    schema,section,_,_,key,_,extent,_=_spec(kind)
    _exact(value, {'schema', 'owner', 'revision', section}, 'server dependency manifest')
    if value['schema'] != schema: raise ValueError('unsupported server dependency schema')
    if not isinstance(value['owner'], str) or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,63}', value['owner']):
        raise ValueError('server dependency owner is invalid')
    _hash(value['revision'])
    if not isinstance(value[section], list) or not 1 <= len(value[section]) <= 4096:
        raise ValueError('server dependency '+kind+' count budget')
    seen = set()
    for entry in value[section]:
        _exact(entry, {'id', 'client_sha256', 'definition'}, 'server dependency '+kind)
        identity = _id(entry['id'])
        if identity in seen: raise ValueError('duplicate server dependency record ID')
        seen.add(identity)
        raw = client_records.get(identity)
        if (not isinstance(raw, bytes) or len(raw) != extent or raw[4] != 0 or
                struct.unpack_from('<I', raw)[0] != identity or
                hashlib.sha256(raw).hexdigest() != _hash(entry['client_sha256'])):
            raise ValueError('server dependency complete client record identity/SHA-256 differs')
        if kind=='skill':
            recover_source_record(raw)
            if raw[1131] > 3 and not authoring: raise ValueError('unsupported native skill dispatch remains blocked')
        definition = entry['definition']
        if not isinstance(definition, dict): raise ValueError('server '+kind+' definition must be an object')
        if definition.get('kind') == 'template':
            _exact(definition, {'kind', 'id', 'sha256'}, 'server '+kind+' template')
            if _id(definition['id']) == identity: raise ValueError('server '+kind+' template cannot be its destination')
            _hash(definition['sha256'])
        elif definition.get('kind') == 'draft' and authoring:
            _exact(definition, {'kind', 'origin', 'values'}, 'draft server '+kind)
            if definition['origin'] not in ('authored', 'source-derived', 'target-adaptation'):
                raise ValueError('draft server '+kind+' origin must be declared')
            values=normalize_partial_row(definition['values'],kind=kind)
            if values.get(key)!=identity:raise ValueError('draft server '+kind+' destination differs')
            for name,expected in paired_values(raw,kind=kind).items():
                if name in values and values[name]!=expected:
                    raise ValueError('server/client shared '+kind+' field differs: '+name)
        elif definition.get('kind') == 'explicit':
            _exact(definition, {'kind', 'origin', 'values'}, 'explicit server '+kind)
            if definition['origin'] not in ('authored', 'source-derived', 'target-adaptation'):
                raise ValueError('explicit server '+kind+' origin must be declared')
            if normalize_row(definition['values'],kind=kind)[key] != identity:
                raise ValueError('explicit server '+kind+' destination differs')
        else: raise ValueError('server '+kind+' requires a template or complete explicit definition')
    return value


def prepare(value, client_records, *, templates, current, ownership, available_ids, kind='skill'):
    """Resolve every requested row before the first write; no defaults guessed."""
    _,section,_,_,key,_,_,shared=_spec(kind)
    validate_manifest(value, client_records,kind=kind)
    all_ids = set(available_ids) | {entry['id'] for entry in value[section]}
    result = []
    for entry in value[section]:
        identity = entry['id']; definition = entry['definition']
        if definition['kind'] == 'template':
            template = templates.get(definition['id'])
            if template is None or row_hash(template,kind=kind) != definition['sha256']:
                raise ValueError('server '+kind+' template is missing or changed')
            row = dict(normalize_row(template,kind=kind)); row[key] = identity
        else: row = normalize_row(definition['values'],kind=kind)
        raw = client_records[identity]
        for name, (offset, primitive) in shared.items():
            if row[name] != struct.unpack_from('<' + primitive, raw, offset)[0]:
                raise ValueError('server/client shared '+kind+' field differs: ' + name)
        linked = struct.unpack_from('<I', raw, 354)[0] if kind=='skill' else 0
        if linked and linked not in all_ids: raise ValueError('linked server skill definition is missing')
        installed = current.get(identity); owned = ownership.get(identity)
        if installed is None and owned is not None:
            raise ValueError('owned server '+kind+' row is missing; refusing reconstruction')
        digest = row_hash(row,kind=kind)
        if installed is not None:
            if owned is None or owned['owner'] != value['owner']:
                raise ValueError('server '+kind+' is baseline or belongs to another owner')
            if row_hash(installed,kind=kind) != owned['installed_hash']:
                raise ValueError('server '+kind+' changed outside its ownership ledger')
            same = (digest == owned['installed_hash'] and owned['client_hash'] == entry['client_sha256'] and
                    owned['definition_json'] == canonical(definition))
            if owned['revision'] == value['revision'] and not same:
                raise ValueError('same server dependency revision cannot change definition data')
            action = 'reused' if same and owned['revision'] == value['revision'] else 'updated'
        else: action = 'inserted'
        result.append({'id': identity, 'kind':kind,'row': row, 'hash': digest, 'action': action,
                       'client_hash': entry['client_sha256'], 'definition_json': canonical(definition)})
    return result


def _schema(cursor, table, expected):
    rows = cursor.execute('SELECT c.name,t.name,c.max_length,c.is_nullable FROM sys.columns c '
        'JOIN sys.types t ON c.user_type_id=t.user_type_id WHERE c.object_id=OBJECT_ID(?) ORDER BY c.column_id',
        'dbo.' + table).fetchall()
    actual = tuple((str(r[0]), str(r[1]), int(r[2]), bool(r[3])) for r in rows)
    if actual != expected: raise ValueError('server dependency SQL schema differs: ' + table)


def _fetch(cursor, identity, kind='skill'):
    _,_,table,columns,key,_,_,_=_spec(kind)
    result = cursor.execute('SELECT ' + ','.join('[' + name + ']' for name in columns) +
        ' FROM dbo.['+table+'] WITH (UPDLOCK,HOLDLOCK) WHERE ['+key+']=?', identity).fetchone()
    return None if result is None else dict(zip(columns, result))


def apply(cursor, value, client_records, *, kind='skill'):
    """Use an existing transaction; caller owns commit/rollback and restart."""
    schema,section,table,columns,key,sql_schema,_,_=_spec(kind)
    value = copy.deepcopy(value)
    validate_manifest(value, client_records,kind=kind)
    client_records = {entry['id']: client_records.get(entry['id']) for entry in value[section]}
    transaction = cursor.execute('SELECT @@TRANCOUNT,XACT_STATE()').fetchone()
    if transaction is None or int(transaction[0]) < 1 or int(transaction[1]) != 1:
        raise ValueError('server dependencies require an active, committable transaction')
    lock = cursor.execute("DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,"
        "@LockMode=N'Exclusive',@LockOwner=N'Transaction',@LockTimeout=5000; SELECT @r;", APP_LOCK).fetchone()
    if lock is None or int(lock[0]) < 0: raise ValueError('another unit catalog transaction is active')
    _schema(cursor, table, sql_schema)
    ledger_exists = bool(cursor.execute("SELECT CASE WHEN OBJECT_ID(N'dbo.sdgo_UnitImportDependencies',N'U') IS NULL THEN 0 ELSE 1 END").fetchone()[0])
    if ledger_exists: _schema(cursor, LEDGER, LEDGER_SCHEMA)
    templates, current, ownership = {}, {}, {}
    available = {int(row[0]) for row in cursor.execute('SELECT ['+key+'] FROM dbo.['+table+'] WITH (HOLDLOCK)')}
    for entry in value[section]:
        identity = entry['id']
        if entry['definition']['kind'] == 'template':
            template = entry['definition']['id']; templates[template] = _fetch(cursor, template,kind)
        row = _fetch(cursor, identity,kind)
        if row is not None: current[identity] = row
        if ledger_exists:
            owned = cursor.execute('SELECT OwnerKey,Revision,ClientHash,DefinitionJson,InstalledHash '
                "FROM dbo.sdgo_UnitImportDependencies WITH (UPDLOCK,HOLDLOCK) WHERE RecordKind=? AND RecordID=?",kind,identity).fetchone()
            if owned is not None: ownership[identity] = dict(zip(
                ('owner', 'revision', 'client_hash', 'definition_json', 'installed_hash'), owned))
    planned = prepare(value, client_records, templates=templates, current=current, ownership=ownership, available_ids=available,kind=kind)
    if not ledger_exists:
        cursor.execute('CREATE TABLE dbo.sdgo_UnitImportDependencies('
            'RecordKind varchar(16) NOT NULL,RecordID int NOT NULL,OwnerKey nvarchar(64) NOT NULL,'
            'Revision char(64) NOT NULL,ClientHash char(64) NOT NULL,DefinitionJson nvarchar(max) NOT NULL,'
            'InstalledHash char(64) NOT NULL,UpdatedAt datetime2(0) NOT NULL,'
            'CONSTRAINT PK_sdgo_UnitImportDependencies PRIMARY KEY(RecordKind,RecordID))')
        _schema(cursor, LEDGER, LEDGER_SCHEMA)
    for entry in planned:
        if entry['action'] == 'reused': continue
        row = entry['row']; identity = entry['id']
        if entry['action'] == 'inserted':
            cursor.execute('INSERT dbo.['+table+'](' + ','.join('[' + name + ']' for name in columns) +
                ') VALUES(' + ','.join('?' for _ in columns) + ')', *(row[name] for name in columns))
        else:
            cursor.execute('UPDATE dbo.['+table+'] SET ' + ','.join('[' + name + ']=?' for name in columns if name != key) +
                ' WHERE ['+key+']=?', *(row[name] for name in columns if name != key), identity)
        readback = _fetch(cursor, identity,kind)
        if readback is None or row_hash(readback,kind=kind) != entry['hash']:
            raise ValueError('server '+kind+' readback differs; refusing lossy definition')
        if entry['action'] == 'inserted':
            cursor.execute("INSERT dbo.sdgo_UnitImportDependencies(RecordKind,RecordID,OwnerKey,Revision,ClientHash,DefinitionJson,InstalledHash,UpdatedAt) "
                "VALUES(?,?,?,?,?,?,?,SYSUTCDATETIME())",kind,identity,value['owner'],value['revision'],
                entry['client_hash'], entry['definition_json'], entry['hash'])
        else:
            cursor.execute("UPDATE dbo.sdgo_UnitImportDependencies SET Revision=?,ClientHash=?,DefinitionJson=?,InstalledHash=?,UpdatedAt=SYSUTCDATETIME() "
                "WHERE RecordKind=? AND RecordID=? AND OwnerKey=?",value['revision'],entry['client_hash'],
                entry['definition_json'],entry['hash'],kind,identity,value['owner'])
        owned = cursor.execute('SELECT OwnerKey,Revision,ClientHash,DefinitionJson,InstalledHash '
            "FROM dbo.sdgo_UnitImportDependencies WHERE RecordKind=? AND RecordID=?",kind,identity).fetchone()
        if owned is None or tuple(owned) != (value['owner'], value['revision'], entry['client_hash'], entry['definition_json'], entry['hash']):
            raise ValueError('server dependency ownership readback differs')
    return {'schema':schema,'kind':kind,'owner':value['owner'],'revision':value['revision'],
            'counts': {action: sum(row['action'] == action for row in planned) for action in ('inserted', 'updated', 'reused')},
            'records': [{'id': row['id'], 'sha256': row['hash'], 'action': row['action']} for row in planned],
            'restart_required': True, 'native_registration_authorized': False, 'actor_accepted': False,
            'player_data_written': False, 'caller_transaction_remains_open': True}
