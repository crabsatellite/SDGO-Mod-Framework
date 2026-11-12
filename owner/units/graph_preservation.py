"""Retain unrelated installed form graphs using their host/SQL/client witnesses."""
from __future__ import annotations
import hashlib
from pathlib import Path
import struct
from . import repository as repo
from .form_graph_runtime import HOST_SHA256

def sha(raw):return hashlib.sha256(raw).hexdigest()

def decode(candidate,authority):
    if len(candidate)<64 or struct.unpack_from('<8sII',candidate)!=(b'SDGOUF1\0',1,64):raise ValueError('已安装形态图版本无效')
    count,size=struct.unpack_from('<II',candidate,16)
    if size!=len(candidate)-64 or sha(candidate[64:])!=candidate[24:56].hex() or candidate[56:64]!=bytes(8):raise ValueError('已安装形态图校验不一致')
    if (len(authority)<160 or struct.unpack_from('<8sII',authority)!=(b'SDGOUA2\0',2,160) or
            struct.unpack_from('<II',authority,16)!=(count,size) or authority[24:56].hex()!=sha(candidate) or
            authority[88:120].hex()!=HOST_SHA256 or authority[160:160+size]!=candidate[64:]):
        raise ValueError('已安装形态图缺少配套宿主凭据')
    nodes,reserved=struct.unpack_from('<II',authority,152);witness=authority[160+size:]
    if reserved or len(witness)!=nodes*118 or authority[120:152].hex()!=sha(witness):raise ValueError('已安装形态图原生定义凭据不一致')
    cursor=64
    def take(n):
        nonlocal cursor
        if n>len(candidate)-cursor:raise ValueError('已安装形态图截断')
        raw=candidate[cursor:cursor+n];cursor+=n;return raw
    def word():return struct.unpack('<I',take(4))[0]
    owners=[]
    if not 1<=count<=512:raise ValueError('形态图所有者数量无效')
    for _ in range(count):
        extent=word()
        if not 1<=extent<=128:raise ValueError('形态图所有者名称无效')
        owner=take(extent).decode('ascii');revision=take(32).hex();root_count=word()
        if not 1<=root_count<=4096:raise ValueError('形态图入口数量无效')
        roots=[word() for _ in range(root_count)];node_count=word()
        if not 1<=node_count<=4096:raise ValueError('形态图节点数量无效')
        entries=[]
        for _ in range(node_count):
            identity,target,origin=word(),word(),word()
            if origin not in (1,2):raise ValueError('形态图节点来源无效')
            entries.append({'id':identity,'target':target,'origin':'owned' if origin==1 else 'original',
                            'client_sha256':take(32).hex(),'server_sha256':take(32).hex()})
        owners.append({'owner':owner,'revision_sha256':revision,'roots':roots,'nodes':entries})
    if cursor!=len(candidate):raise ValueError('形态图有尾部数据')
    return owners

def read(server_ini,publication,baseline):
    from .batch_server import battle_hosts
    from .indexes import catalog_records
    server_ini=Path(server_ini)
    hosts=battle_hosts(server_ini);pairs=[]
    for host in hosts:
        root=host['path'];candidate=root/'sdgo/mods/unit_catalog_server/form-graphs.bin';authority=root/'sdgo/install-state/unit-form-authority.bin'
        if candidate.exists()!=authority.exists():raise ValueError('已有形态图与宿主凭据不成对，拒绝替换')
        if candidate.exists():pairs.append((candidate.read_bytes(),authority.read_bytes()))
    if not pairs:return {'requests':[],'records':{},'originals':{}}
    if len(pairs)!=len(hosts) or any(pair!=pairs[0] for pair in pairs):raise ValueError('各对战主机的已有形态图不一致，拒绝覆盖')
    replacing={row['owner'] for row in publication.graph_requests}
    owners=[owner for owner in decode(*pairs[0]) if owner['owner'] not in replacing]
    if not owners:return {'requests':[],'records':{},'originals':{}}
    records={}
    for folder in [baseline.root/'sdgo/mods/unit_catalog',baseline.root/'sdgo/mods/unit_catalog/presets']:
        path=folder/'catalog.bin'
        if path.is_file():records.update({identity:raw for identity,(kind,raw) in catalog_records(path.read_bytes()).items() if kind==2})
    c=repo._connect(server_ini)
    try:
        cur=c.cursor();rows={int(row[0]):dict(zip(repo.UNIT_COLUMNS,row)) for row in cur.execute('SELECT '+','.join(repo.UNIT_COLUMNS)+' FROM tbl_Units')}
        ledgers={int(r[0]):(str(r[1]),str(r[2]),str(r[3])) for r in cur.execute("SELECT RecordID,FamilyKey,Revision,InstalledHash FROM sdgo_UnitImportDefinitions WHERE RecordKind='unit'")}
        requests=[];originals={};retained={}
        for owner in owners:
            families=set();revisions=set();declarations=[]
            for node in owner['nodes']:
                identity=node['id'];raw=records.get(identity)
                if raw is None:raw=baseline.record('unit',identity)
                if raw is None or sha(raw)!=node['client_sha256'] or identity not in rows or repo._row_hash(rows[identity],repo.UNIT_COLUMNS)!=node['server_sha256']:
                    raise ValueError('已有形态图的完整客户端／服务端记录发生变化：'+str(identity))
                if node['origin']=='owned':
                    ledger=ledgers.get(identity)
                    if not ledger or ledger[2]!=node['server_sha256']:raise ValueError('已有形态图缺少一致的所有权账本')
                    families.add(ledger[0]);revisions.add(ledger[1])
                else:originals[identity]={'client_sha256':node['client_sha256'],'server_sha256':node['server_sha256']}
                retained[identity]=raw;declarations.append({'id':identity,'client_sha256':sha(raw)})
            if len(revisions)!=1 or sha(next(iter(revisions)).encode())!=owner['revision_sha256'] or owner['owner'] not in families:
                raise ValueError('已有形态图的所有者版本与当前账本不一致')
            requests.append({'schema':'sdgo.unit-form-graphs/1','owner':owner['owner'],'families':sorted(families),
                'revision':next(iter(revisions)),'roots':owner['roots'],'records':declarations})
        return {'requests':requests,'records':retained,'originals':originals}
    finally:c.rollback();c.close()


def require_no_unrelated(hosts,publication):
    """A caller without client witnesses may replace only its own graphs."""
    replacing={row['owner'] for row in publication.graph_requests}
    for host in hosts:
        root=host['path'];candidate=root/'sdgo/mods/unit_catalog_server/form-graphs.bin'
        authority=root/'sdgo/install-state/unit-form-authority.bin'
        if candidate.exists()!=authority.exists():raise ValueError('已有形态图与宿主凭据不成对，拒绝替换')
        if candidate.exists() and any(owner['owner'] not in replacing for owner in decode(candidate.read_bytes(),authority.read_bytes())):
            raise ValueError('已有其他所有者的形态图；请配置配套客户端以验证并保留，未写入任何内容')
