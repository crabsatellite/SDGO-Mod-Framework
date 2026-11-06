"""Source-bound expected payloads for the supported native battle unit table.

The original table insertion copies 0x72 bytes into a larger linked record.
This profile never copies list links/padding, installs records or grants actor
permission. SQL-only admission/catalog fields remain in the complete row hash.
Native readback must separately prove the expected bytes for each installed ID.
"""
from __future__ import annotations

import hashlib
import math
import struct

from . import repository as repo

SCHEMA = 'sdgo.unit-battle-definition/1'
HOST_SHA256 = 'f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4'
PAYLOAD_BYTES = 0x72
HEADER = struct.Struct('<8s4I')
MAGIC = b'SDGOBD1\0'
MAX_RECORDS = 4096
_FORMATS = {'tinyint':'B', 'smallint':'h', 'int':'i', 'real':'f'}

# Fixed profile, not a serializer inferred from whichever columns a project
# supplies. Offsets shared with the native consumers are asserted below.
FIELDS = (
    ('UnitNumber','i'),('Influence1','B'),('Influence2','B'),
    ('UnitGradeNumber','B'),('UnitGradeNumber2','B'),('MasteredWeaponType','B'),
    ('BattleCost','h'),('Star','B'),('UnitNumber_Transform','i'),('HP','i'),('Shield','i'),
    ('ShieldType','B'),('ShieldRate','i'),('DashType','B'),('MoveLevel','B'),
    ('DashLevel','B'),('QuickStepLevel','B'),('MotivePower','B'),
    ('BoosterIncrease','h'),('BoosterDecrease','h'),('FatalAttackIncrease','h'),
    ('AdaptEnvironType','B'),('AdaptEnvironSpace','h'),('AdaptEnvironGround','h'),
    ('AdaptEnvironUnderwater','h'),('AdaptEnvironDesert','h'),('AdaptEnvironSky','h'),
    ('Sight','h'),('SkillNumber_1','i'),('SkillNumber_2','i'),('SkillNumber_3','i'),
    ('StrikingPower','h'),('DefensivePower','h'),('MasteredWeaponIX','i'),
    ('WeaponNumber_1','i'),('WeaponNumber_2','i'),('WeaponNumber_3','i'),
    ('WeaponNumber_FatalAttack','i'),('ViewOrder','i'),('MaxEnergyAmount','f'),
    ('MaxEnergyCost','f'),('EnergyDecAmount','B'),('UnitGrowIndex','i'),
    ('UserGradeLimit','i'),('LevelRating','i'),
)
OFFSETS = {};_at = 0
for _name,_format in FIELDS:
    OFFSETS[_name] = _at;_at += struct.calcsize('<'+_format)
assert _at == PAYLOAD_BYTES
assert {key:OFFSETS[key] for key in ('UnitNumber_Transform','SkillNumber_1','SkillNumber_3',
    'MasteredWeaponIX','WeaponNumber_1','WeaponNumber_FatalAttack')} == {
    'UnitNumber_Transform':0xc,'SkillNumber_1':0x35,'SkillNumber_3':0x3d,
    'MasteredWeaponIX':0x45,'WeaponNumber_1':0x49,'WeaponNumber_FatalAttack':0x55}
_schema = {name:(kind,size,nullable) for name,kind,size,nullable in repo.SQL_SCHEMAS['tbl_Units']}
assert all(_FORMATS[_schema[name][0]] == form for name,form in FIELDS if name!='LevelRating')
SQL_ONLY_FIELDS = tuple(name for name in repo.UNIT_COLUMNS if name not in OFFSETS)
GRADE_COLUMNS=('UnitGradeNumber','UnitGradeNumber2','LevelRating')
GRADE_TABLE_SCHEMA=(
    ('UnitGradeNumber','tinyint',1,False),('UnitGradeNumber2','tinyint',1,False),
    ('DeleteUnitPoint','smallint',2,False),('DeleteUnitItemCategory','int',4,False),
    ('DeleteUnitItemAmount','smallint',2,False),('BaseCost','smallint',2,False),
    ('ResetWeight','smallint',2,False),('MaxReset','smallint',2,False),
    ('PremiumBuyEtcItem','int',4,False),('LevelRating','int',4,False),
)


def normalize_grades(rows):
    if not isinstance(rows,dict) or not 1<=len(rows)<=4096:
        raise ValueError('native unit grade snapshot budget')
    result={}
    for key,row in rows.items():
        if not isinstance(row,dict) or set(row)!=set(GRADE_COLUMNS):raise ValueError('native unit grade row differs')
        for name in GRADE_COLUMNS:
            repo._validate_scalar('tbl_UnitGradeInfo.'+name,row[name],'int' if name=='LevelRating' else 'tinyint',
                                  4 if name=='LevelRating' else 1,False)
        if type(key) is not tuple or len(key)!=2 or any(type(v) is not int for v in key) or key!=(row['UnitGradeNumber'],row['UnitGradeNumber2']):
            raise ValueError('native unit grade snapshot key differs')
        result[key]=dict(row)
    return result


def expected_payload(row,grade_row):
    """Project a complete trusted SQL snapshot; preserve every payload scalar.

    Existing read-only varchar values can contain legacy NUL padding. They are
    retained in the row hash; this does not relax the strict new-row writer.
    """
    if not isinstance(row,dict) or set(row) != set(repo.UNIT_COLUMNS):
        raise ValueError('native unit definition needs a complete SQL row')
    for name,kind,size,nullable in repo.SQL_SCHEMAS['tbl_Units']:
        value=row[name]
        if kind in ('varchar','nvarchar') and isinstance(value,str):
            if len(value) > (size//2 if kind=='nvarchar' else size):
                raise ValueError('native unit SQL text extent differs')
        else:repo._validate_scalar('tbl_Units.'+name,value,kind,size,nullable)
    for name in ('UnitNumber','UnitNumber_Transform'):
        value=row[name]
        if not ((name=='UnitNumber_Transform' and value==0) or repo.UNIT_ID_FIRST<=value<=repo.UNIT_ID_LAST):
            raise ValueError('native unit definition ID is outside the supported domain')
    key=(row['UnitGradeNumber'],row['UnitGradeNumber2'])
    grades=normalize_grades({key:grade_row})
    values={**row,'LevelRating':grades[key]['LevelRating']}
    try:return b''.join(struct.pack('<'+form,values[name]) for name,form in FIELDS)
    except (struct.error,OverflowError) as error:
        raise ValueError('native unit payload scalar cannot be encoded') from error


def decode_payload(raw):
    if not isinstance(raw,bytes) or len(raw)!=PAYLOAD_BYTES:
        raise ValueError('native unit payload extent differs')
    result={name:struct.unpack_from('<'+form,raw,OFFSETS[name])[0] for name,form in FIELDS}
    for name,value in result.items():
        if isinstance(value,float) and not math.isfinite(value):raise ValueError('native unit payload is non-finite')
    for name in ('UnitNumber','UnitNumber_Transform'):
        value=result[name]
        if not ((name=='UnitNumber_Transform' and value==0) or repo.UNIT_ID_FIRST<=value<=repo.UNIT_ID_LAST):
            raise ValueError('native unit payload identity differs')
    return result


def compile_expectations(rows,grade_rows):
    """Diagnostic expected bytes only, with no ownership/activation authority."""
    if not isinstance(rows,dict) or not 1<=len(rows)<=MAX_RECORDS:
        raise ValueError('native unit definition count budget')
    if any(type(identity) is not int or not isinstance(row,dict) for identity,row in rows.items()):
        raise ValueError('native unit snapshot map differs')
    grades=normalize_grades(grade_rows)
    payload=[];receipts=[]
    for identity,row in sorted(rows.items()):
        if type(identity) is not int or identity!=row.get('UnitNumber'):
            raise ValueError('native unit snapshot key differs')
        key=(row.get('UnitGradeNumber'),row.get('UnitGradeNumber2'))
        if key not in grades:raise ValueError('native unit grade join is missing')
        raw=expected_payload(row,grades[key]);payload.append(raw)
        receipts.append({'id':identity,'server_row_sha256':repo._row_hash(row,repo.UNIT_COLUMNS),
                         'payload_sha256':hashlib.sha256(raw).hexdigest()})
    raw=HEADER.pack(MAGIC,1,PAYLOAD_BYTES,len(rows),0)+b''.join(payload)
    grade_hash=hashlib.sha256(repo._canonical_json([grades[key] for key in sorted(grades)]).encode('utf-8')).hexdigest()
    return raw,{'schema':SCHEMA,'host_sha256':HOST_SHA256,'expected_sha256':hashlib.sha256(raw).hexdigest(),'grade_snapshot_sha256':grade_hash,
        'records':receipts,'sql_only_fields':list(SQL_ONLY_FIELDS),'native_readback_accepted':False,
        'native_actor_accepted':False,'ordinary_publication_allowed':False}


def compare_payload(expected,actual):
    before=decode_payload(expected);after=decode_payload(actual)
    return [{'field':name,'offset':OFFSETS[name],'expected':before[name],'actual':after[name]}
            for name,form in FIELDS if expected[OFFSETS[name]:OFFSETS[name]+struct.calcsize('<'+form)]
            != actual[OFFSETS[name]:OFFSETS[name]+struct.calcsize('<'+form)]]
