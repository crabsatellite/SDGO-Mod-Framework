"""Catalog-bound native stage requests; eligibility remains native game business."""
from __future__ import annotations
import hashlib
import struct

SCHEMA='sdgo.unit-stage-rules/1'

def normalize(value):
    if not isinstance(value,list) or len(value)>128:raise ValueError('阶段规则必须是最多128项的列表')
    result=[];seen=set()
    for row in value:
        if not isinstance(row,dict) or set(row)!={'unit_id','target_unit_id','skill_id','max_hp_percent'}:
            raise ValueError('阶段规则字段无效')
        for key in ('unit_id','target_unit_id','skill_id'):
            if type(row[key]) is not int or not 1<=row[key]<=9999999:raise ValueError('阶段规则编号无效')
        from .skill_slots import ordinary
        if not ordinary(row['skill_id']):raise ValueError('阶段技能编号被原生客户端分到额外技能槽，不能用作普通机体技能')
        if type(row['max_hp_percent']) is not int or not 1<=row['max_hp_percent']<=99:
            raise ValueError('阶段规则耐久阈值必须为1–99%')
        if row['unit_id'] in seen or row['unit_id']==row['target_unit_id']:
            raise ValueError('阶段规则起点重复或指向自身')
        seen.add(row['unit_id']);result.append(dict(row))
    return sorted(result,key=lambda r:r['unit_id'])

def encode(value,catalog,records):
    from .compiler import fnv,native_catalog
    if not isinstance(records,dict) or native_catalog([(kind,identity,raw) for (kind,identity),raw in records.items()])!=catalog:
        raise ValueError('阶段规则的记录集合与完整客户端目录不同')
    rows=normalize(value)
    for row in rows:
        source=records.get((2,row['unit_id']));target=records.get((2,row['target_unit_id']))
        skill=records.get((13,row['skill_id']))
        if not source or len(source)!=4976 or not target or len(target)!=4976 or not skill or len(skill)!=1132:
            raise ValueError('阶段规则需要同版目录中的完整起点、目标和技能记录')
        if struct.unpack_from('<I',source,316)[0]!=row['target_unit_id']:
            raise ValueError('阶段目标与原生有向变形边不同')
        if row['skill_id'] not in struct.unpack_from('<II',source,575):raise ValueError('阶段技能不在起点机体的技能槽中')
        if source[320] or target[320]:raise ValueError('自动阶段使用标准原生变形条件；不能覆盖特殊条件')
        if (skill[155]!=2 or struct.unpack_from('<h',skill,156)[0]!=row['max_hp_percent'] or skill[1131]):
            raise ValueError('阶段技能须为相同耐久阈值的原生被动技能，不能携带变形调度')
    payload=b''.join(struct.pack('<4I',*(row[key] for key in ('unit_id','target_unit_id','skill_id','max_hp_percent'))) for row in rows)
    return struct.pack('<8s4I32sI20s',b'SDGOST1\0',1,80,len(rows),len(payload),hashlib.sha256(catalog).digest(),fnv(payload),bytes(20))+payload
