"""Public display metadata for the native completed-unit acquisition panel."""
import struct
from .rules import validate

FILE = 'collection-acquisition.bin'
MARKER = b'SDGO_COLLECTION_ACQUISITION_V1'

def encode(source_units, unit_items, rules):
    validate(rules)
    source_units=list(source_units)
    roots=sorted({row if type(row) is int else row['unit_id'] for row in source_units} or
                 {row['unit_id'] for row in unit_items})
    if not roots:return None
    if len(roots)>20000 or any(type(uid) is not int or not 0<uid<=0x7fffffff for uid in roots):
        raise ValueError('Invalid collection acquisition roots')
    limit=rules['draw_limit'];period=rules['limit_period']
    quota='不限次' if period=='none' or not limit else ('每日' if period=='day' else '每周')+str(limit)+'次'
    text=f"收藏扭蛋（活动扭蛋页）：{rules['price_gp']:,} GB/次，{quota}。优先未解锁图鉴。"
    brief='收藏扭蛋（活动扭蛋页）'
    full=text.encode('utf-16-le');short=brief.encode('utf-16-le')
    if len(full)>=200:raise ValueError('Collection acquisition explanation is too long')
    raw=struct.pack('<8sIII',b'SDGOACQ1',len(roots),len(full)//2,len(short)//2)
    raw+=struct.pack('<'+'I'*len(roots),*roots)+full+short
    return raw,{'schema_version':1,'root_count':len(roots),'unit_ids':roots,
                'text':text,'brief':brief,'rules':dict(rules),
                'native_kind':85,'source_text_offset':214,'source_text_bytes':200,
                'scope':'completed-product explanation only; original sources and blueprint fields retained'}
