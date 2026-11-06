"""Bounded codec shared with sdgo/mission_behavior_profile.h.

Reference game data stays in the server owner's workspace/player package. The
public framework contains this compiler and no original task tables or terrain.
"""
from __future__ import annotations
import hashlib
import json
import math
import struct
import zlib

HEADER = struct.Struct('<8s8I32s32s24s')
ROW = struct.Struct('<5I3i6I')
ENTRY_BYTES = 4 + ROW.size * 2
MAGIC = b'SDGOMBP1'
MAX_ENTRIES = 4096
FIELDS = ('MissionNpcID', 'MissionID', 'MissionDifficult', 'MissionPart',
          'NpcID', 'px', 'py', 'pz', 'AfterTime', 'AbsTime', 'ManualID',
          'NpcParty', 'CreateLimit', 'ZoneIndex')
CHANGES = {4: 1, 5: 2, 6: 4, 7: 8, 8: 16, 12: 32}
OC_BINDINGS = {**{i: 'MM001_NpcBoss001' for i in
    (85054, 85058, 85062, 85081, 85082, 85083, 85084, 85169, 85170, 85171)},
    85079: 'MM001_NpcBoss002', 85080: 'MM001_NpcBoss002'}


def _normal_row(row):
    if not isinstance(row, dict) or any(k not in row for k in FIELDS):
        raise ValueError('任务配置缺少必要的原生字段')
    result=[]
    for i,key in enumerate(FIELDS):
        value=row[key]
        if isinstance(value,bool) or not isinstance(value,(int,float)):
            raise ValueError('任务字段类型不正确')
        if i in (5,6,7):
            if not math.isfinite(value) or value!=int(value) or abs(value)>10000000:
                raise ValueError('任务坐标超出范围')
            value=int(value)
        else:
            if not math.isfinite(value) or value!=int(value) or not 0<=value<=0xffffffff:
                raise ValueError('任务整数超出范围')
            value=int(value)
        result.append(value)
    if (not result[0] or not result[1] or not result[4] or
        any(result[i]>255 for i in (2,3,11,12,13)) or
        any(result[i]>65535 for i in (8,9))):
        raise ValueError('任务标识或类型超出范围')
    return tuple(result)


def _index(rows):
    if not isinstance(rows,(list,tuple)):
        raise ValueError('任务引用必须为记录数组')
    if len(rows)>1000000:
        raise ValueError('任务记录过多')
    out={}
    for row in rows:
        value=_normal_row(row)
        if value[0] in out:
            raise ValueError('任务记录编号重复')
        out[value[0]]=value
    return out


def _mask(before,after):
    mask=0
    for i,(a,b) in enumerate(zip(before,after)):
        if a!=b:
            if i not in CHANGES:
                raise ValueError('OC 引用包含尚未适配的任务字段变化')
            mask|=CHANGES[i]
    return mask


def compile_reference(current_npc_rows, oc_npc_rows, oc_client_bindings,
                      *, label='OC', mission_ids=None):
    """Compile verified owner inputs; preserve current-only modes and economy.

    Inputs are static MissionNpc records, not credentials or database dumps.
    Only NPC selection, coordinates, spawn delay and limit may change. Native
    Clear/opcode, object-ID migrations and rewards stay in the current engine.
    """
    if not isinstance(oc_client_bindings,dict):
        raise ValueError('缺少 OC 客户端行为引用')
    bindings={int(k):v for k,v in oc_client_bindings.items()}
    if any(bindings.get(k)!=v for k,v in OC_BINDINGS.items()):
        raise ValueError('OC Boss 行为引用不匹配')
    for k in (85145,85146,85147):
        if k in bindings and bindings[k]!='MM001_NpcBoss001E':
            raise ValueError('OC 非常简单难度引用不匹配')
    if not isinstance(label,str) or any(ord(c)<32 for c in label):
        raise ValueError('任务配置名称不正确')
    label_bytes=label.encode('utf8')
    if len(label_bytes)>23:
        raise ValueError('任务配置名称过长')
    current,old=_index(current_npc_rows),_index(oc_npc_rows)
    allowed=None if mission_ids is None else set(mission_ids)
    entries=[]
    for key in sorted(current.keys()&old.keys()):
        before,after=current[key],old[key]
        if allowed is not None and before[1] not in allowed:
            continue
        mask=_mask(before,after)
        if mask:entries.append((mask,before,after))
    if len(entries)>MAX_ENTRIES:
        raise ValueError('任务行为差异过多')
    if not entries:
        raise ValueError('OC 配置没有可验证的任务行为数据')
    payload=b''.join(struct.pack('<I',mask)+ROW.pack(*before)+ROW.pack(*after)
                     for mask,before,after in entries)
    baseline=hashlib.sha256(b''.join(ROW.pack(*x[1]) for x in entries)).digest()
    source=hashlib.sha256(b''.join(ROW.pack(*x[2]) for x in entries)+
        json.dumps(OC_BINDINGS,sort_keys=True,separators=(',',':')).encode()).digest()
    header=HEADER.pack(MAGIC,1,HEADER.size,1|(2 if entries else 0),ENTRY_BYTES,
        len(entries),len(payload),zlib.crc32(payload),1,source,baseline,
        label_bytes.ljust(24,b'\0'))
    result=header+payload
    decode_bundle(result)
    return result


def decode_bundle(raw):
    if not isinstance(raw,(bytes,bytearray)) or len(raw)<HEADER.size:
        raise ValueError('缺少任务行为配置头')
    if len(raw)>HEADER.size+MAX_ENTRIES*ENTRY_BYTES:
        raise ValueError('任务行为配置过大')
    (magic,version,hsize,flags,esize,count,psize,crc,generation,
     source,baseline,label)=HEADER.unpack_from(raw)
    if (magic!=MAGIC or version!=1 or hsize!=HEADER.size or esize!=ENTRY_BYTES or
        generation!=1 or not flags&1 or flags&~3 or not count or count>MAX_ENTRIES or
        psize!=count*ENTRY_BYTES or len(raw)!=hsize+psize or bool(flags&2)!=bool(count)):
        raise ValueError('任务行为配置版本或结构不匹配')
    payload=raw[hsize:]
    if zlib.crc32(payload)!=crc:
        raise ValueError('任务行为配置校验失败')
    entries=[];previous=0
    for i in range(count):
        pos=i*ENTRY_BYTES
        mask=struct.unpack_from('<I',payload,pos)[0]
        a=ROW.unpack_from(payload,pos+4);b=ROW.unpack_from(payload,pos+4+ROW.size)
        before=_normal_row(dict(zip(FIELDS,a)));after=_normal_row(dict(zip(FIELDS,b)))
        if before[0]<=previous or not mask or mask&~63 or _mask(before,after)!=mask:
            raise ValueError('任务行为记录重复、无效或包含不支持的变化')
        previous=before[0]
        entries.append({'mask':mask,'baseline':dict(zip(FIELDS,before)),
                        'oc':dict(zip(FIELDS,after))})
    try:name=label.split(b'\0',1)[0].decode('utf8')
    except UnicodeDecodeError as exc:raise ValueError('任务配置名称编码不正确') from exc
    return {'format':'SDGOMBP1','version':version,'bindings_generation':generation,
            'flags':flags,'entry_count':count,'mission_count':len({x['baseline']['MissionID'] for x in entries}),
            'source_digest':source.hex(),'baseline_digest':baseline.hex(),
            'label':name,'sha256':hashlib.sha256(raw).hexdigest(),'rows':entries}
