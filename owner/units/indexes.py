"""Keep native encyclopedia IDs stable across owner and built-in content scopes."""
from __future__ import annotations
from pathlib import Path
import struct

FIRST=0x375BE1

def catalog_records(raw):
    from .compiler import fnv,CATALOG_RECORD_LIMIT,CATALOG_PAYLOAD_LIMIT
    if not isinstance(raw,bytes) or len(raw)<32:raise ValueError('机体目录头部不完整')
    magic,version,header,count,size,checksum,reserved=struct.unpack_from('<8s6I',raw)
    if (magic!=b'SDGOUN1\0' or version!=1 or header!=32 or reserved or count>CATALOG_RECORD_LIMIT or
            size>CATALOG_PAYLOAD_LIMIT or size!=len(raw)-32 or fnv(raw[32:])!=checksum):
        raise ValueError('机体目录版本、容量或校验不一致')
    records={};cursor=32
    sizes={2:4976,10:1871,11:732,12:1559,13:1132,20:106,33:1197,34:614,85:614}
    for _ in range(count):
        if len(raw)-cursor<24:raise ValueError('机体目录记录头部截断')
        kind,identity,size,checksum,a,b=struct.unpack_from('<6I',raw,cursor);cursor+=24
        data=raw[cursor:cursor+size];cursor+=size
        if (sizes.get(kind)!=size or a or b or len(data)!=size or not 1<=identity<=9999999 or
                identity in records or struct.unpack_from('<I',data)[0]!=identity or data[4] or fnv(data)!=checksum):
            raise ValueError('机体目录记录身份、长度或校验不一致')
        records[identity]=(kind,data)
    if cursor!=len(raw):raise ValueError('机体目录尾部数据不一致')
    return records

def installed(client):
    result={}
    if client is None:return result
    module=Path(client)/'sdgo/mods/unit_catalog'
    for scope,folder in [('owner',module),('presets',module/'presets')]:
        path=folder/'catalog.bin'
        if not path.is_file():continue
        if path.is_symlink() or path.stat().st_size>64*1024*1024+32:raise ValueError('已安装机体目录不是受支持的普通文件')
        result[scope]={identity:raw for identity,(kind,raw) in catalog_records(path.read_bytes()).items() if kind==85}
    return result

class Allocator:
    def __init__(self,original,scopes,scope):
        if scope not in ('owner','presets'):raise ValueError('机体图鉴分区无效')
        rows=dict(original);self.current={};self.used=set(rows)
        for name,entries in scopes.items():
            if name not in ('owner','presets'):raise ValueError('未知机体图鉴分区')
            for identity,raw in entries.items():
                if identity in rows:raise ValueError('机体图鉴编号在多个分区重复')
                rows[identity]=raw;self.used.add(identity)
                if name==scope:
                    unit=struct.unpack_from('<I',raw,6)[0]
                    if unit in self.current:raise ValueError('机体图鉴的根机体重复')
                    self.current[unit]=identity
        if not rows or set(rows)!=set(range(FIRST,FIRST+len(rows))):raise ValueError('机体图鉴目录不连续，不能安全添加预设')
        self.next=FIRST+len(rows);self.assigned=set()

    def allocate(self,unit):
        if unit in self.assigned:raise ValueError('机体图鉴根机体重复分配')
        self.assigned.add(unit)
        if unit in self.current:return self.current[unit]
        identity=self.next;self.next+=1;self.used.add(identity);return identity
