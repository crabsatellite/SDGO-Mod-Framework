"""Read non-player configuration for an imported-unit acquisition preview."""
from __future__ import annotations

import datetime
import hashlib
import json
from pathlib import Path
import struct

from .compiler import CATALOG_RECORD_LIMIT
from .repository import _connect,database_target_key


TABLES={
    'GOnlineGame':('tbl_ItemCategory','tbl_MixTreeFile','tbl_CoinMachinePrice','tbl_CoinCapsuleList',
        'tbl_BattleResultItemDropSetting','tbl_BattleResultItemDropDice','tbl_QuestInfo','tbl_QuestPrerequest',
        'tbl_ItemMachinePrice','tbl_ItemMachineList','tbl_ItemMachineConditionList','tbl_ItemMachinePrime'),
    'GDStoreBilling':('tbls_StoreItems','tbls_SubItems'),
}


def read_configuration(server_ini):
    """One bounded read transaction; never select inventories or account state."""
    connection=_connect(Path(server_ini));tables={}
    try:
        cur=connection.cursor()
        cur.execute('SET TRANSACTION ISOLATION LEVEL SERIALIZABLE')
        for database,names in TABLES.items():
            for name in names:
                prefix='dbo.' if database=='GOnlineGame' else 'GDStoreBilling.dbo.'
                if name=='tbl_CoinCapsuleList':
                    query='SELECT DISTINCT MachineID,UnitID FROM '+prefix+name
                elif name=='tbl_ItemMachineList':
                    query='SELECT MachineID,CategoryID,Amount,COUNT_BIG(*) AS ExpandedEntries FROM '+prefix+name+' GROUP BY MachineID,CategoryID,Amount'
                else:query='SELECT * FROM '+prefix+name
                cur.execute(query)
                columns=[c[0] for c in cur.description]
                rows=cur.fetchmany(200001)
                if len(rows)>200000:raise ValueError('获取途径配置表超过读取上限：'+name)
                values=[dict(zip(columns,row)) for row in rows]
                values.sort(key=lambda row:json.dumps(row,sort_keys=True,default=str))
                tables[(database,name)]=values
        connection.rollback()
    finally:connection.close()
    material={db+'.'+name:rows for (db,name),rows in tables.items()}
    digest=hashlib.sha256(json.dumps(material,sort_keys=True,ensure_ascii=False,default=str).encode('utf-8')).hexdigest()
    return {'tables':tables,'configuration_sha256':digest,'database_target':database_target_key(Path(server_ini)),
            'captured_at':datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}


def activity_grants(client,tables):
    """Use listed native activity machines and enabled authoritative prices."""
    from manager.engines.capsules.capsule_catalog import ClientData,gdt_records
    from manager.engines.capsules.capsule_client_compile import resident_extensions
    _,extensions,_,_=resident_extensions(Path(client),ClientData(Path(client)))
    size,rows=gdt_records(extensions['0099'][0])
    if size!=179:raise ValueError('活动扭蛋列表布局不匹配')
    listed={struct.unpack_from('<I',row,5)[0] for row in rows if row[4]==0}
    active={int(row['MachineID']) for row in tables[('GOnlineGame','tbl_ItemMachinePrice')] if not row['Disable']}
    grants=[]
    for row in tables[('GOnlineGame','tbl_ItemMachineList')]:
        mid=int(row['MachineID'])
        if mid in listed & active and int(row['CategoryID'])>0 and int(row['Amount'])>0:
            grants.append({'category_id':int(row['CategoryID']),
                'witness':{'source':'configured_activity_machine','machine_id':mid}})
    return grants


def installed_item_records(client):
    """Read the complete configured capsule table plus sealed unit additions.

    The original GPack table alone omits collection-created permanent gifts.
    This is configuration evidence, not proof that a current game loaded it.
    """
    from manager.engines.capsules.capsule_catalog import ClientData,gdt_records
    from manager.engines.capsules.capsule_client_compile import resident_extensions
    from .compiler import native_catalog
    root=Path(client).resolve()
    _,tables,_,_=resident_extensions(root,ClientData(root))
    size,rows=gdt_records(tables['0031'][0])
    if size!=614:raise ValueError('当前图纸/机体物品表布局不匹配')
    result={struct.unpack_from('<I',raw)[0]:raw for raw in rows}
    if len(result)!=len(rows):raise ValueError('物品表含重复全局编号')
    module=root/'sdgo/mods/unit_catalog';path=module/'catalog.bin'
    if path.is_file():
        receipt=json.loads((module/'install-receipt.json').read_text(encoding='utf-8-sig'))
        saved=next((r for r in receipt.get('files',[]) if r['path'].replace('\\','/').lower()=='sdgo/mods/unit_catalog/catalog.bin'),None)
        raw=path.read_bytes()
        if not saved or hashlib.sha256(raw).hexdigest()!=saved['installed_sha256']:
            raise ValueError('已安装机体目录的数据哈希与收据不一致')
        if len(raw)<32 or raw[:8]!=b'SDGOUN1\0':raise ValueError('已安装机体目录容器无效')
        count=struct.unpack_from('<I',raw,16)[0]
        if count>CATALOG_RECORD_LIMIT:raise ValueError('已安装机体目录超过容量')
        cursor=32;records=[]
        for index in range(count):
            if cursor+24>len(raw):raise ValueError('机体目录记录头截断')
            kind,identity,extent,*_=struct.unpack_from('<6I',raw,cursor);cursor+=24
            if cursor+extent>len(raw):raise ValueError('机体目录记录截断')
            record=raw[cursor:cursor+extent];cursor+=extent;records.append((kind,identity,record))
            if kind==34:
                if identity in result and result[identity]!=record:raise ValueError('物品全局编号跨模块冲突')
                result[identity]=record
        if cursor!=len(raw) or native_catalog(records)!=raw:raise ValueError('已安装机体目录校验失败')
    return result
