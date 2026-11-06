"""Non-destructive maintenance gates and activation receipts for explicit writes.

Importing this module or upgrading Manager never connects to a database. Existing
content without a change receipt retains its legacy activation status.
"""
from __future__ import annotations
from contextlib import contextmanager, ExitStack
import hashlib
import json
import os
from pathlib import Path
import threading
import time
import uuid
import ipaddress
import socket
import xml.etree.ElementTree as ET
from . import storage
from .target_locks import lease

NATIVE={'WorldServer.exe','BattleServer.exe','StoreServer.exe','AgentServer.exe','ChattingServer.exe'}
CORE_CONTRACT=b'SDGO_CORE_RUNTIME_RECEIPTS_V1'
_context=threading.local()


def target_key(ini):
    cfg=storage.ini_values(Path(ini).read_bytes())
    section=next((s for s in cfg.sections()if s.replace(' ','').casefold()=='dbgame'),None)
    if section is None:raise ValueError('维护目标缺少 DB Game 配置')
    values={k.casefold():v.strip() for k,v in cfg[section].items()}
    server=values.get('dbservername','');database=values.get('dbname','')
    if not server or not database:raise ValueError('维护目标缺少数据库身份')
    return hashlib.sha256((server.casefold()+'\0'+database.casefold()).encode()).hexdigest()


def require_local_database(ini):
    cfg=storage.ini_values(Path(ini).read_bytes());section=next((s for s in cfg.sections()if s.replace(' ','').casefold()=='dbgame'),None)
    if section is None:raise ValueError('维护目标缺少 DB Game 配置')
    values={k.casefold():v for k,v in cfg[section].items()};name=values.get('dbservername','').strip().casefold()
    if name.startswith('tcp:'):name=name[4:]
    host=name.split(',')[0].split('\\')[0]
    local={'.','(local)','localhost',socket.gethostname().casefold(),os.environ.get('COMPUTERNAME','').casefold()}
    if host in local:return
    try:
        if ipaddress.ip_address(host).is_loopback:return
    except ValueError:pass
    try:addresses={row[4][0]for row in socket.getaddrinfo(socket.gethostname(),None)}
    except OSError:addresses=set()
    if host in addresses:return
    raise ValueError('当前 Manager 无法核实远程数据库的全部原生服务；请在服务器所在机器执行这项维护')


def topology(ini):
    ini=Path(ini).resolve(strict=True);selected=ini.parent
    if selected==selected.parent:raise ValueError('维护目标不能是磁盘根目录')
    def executables(root):return [p for p in root.iterdir()if p.is_file()and p.name in NATIVE]
    own=executables(selected)
    if not own:raise ValueError('所选配置旁没有原生服务器程序，不能核实维护范围')
    declared_group=selected.parent.name.casefold()=='server'or(selected.parent.parent/'ServerStartUp/ServerStartUp.xml').is_file()
    siblings=[p for p in selected.parent.iterdir()if p.is_dir()and not p.is_symlink()and executables(p)]if declared_group else[]
    group=selected.parent if declared_group and selected in siblings else selected
    nodes=sorted((exe for folder in (siblings if group!=selected else [selected])for exe in executables(folder)),key=str)
    for exe in nodes:
        if not exe.resolve().is_relative_to(group)or exe.is_symlink()or getattr(exe.stat(),'st_file_attributes',0)&0x400:
            raise ValueError('维护范围不能使用目录链接')
    startup=group.parent/'ServerStartUp/ServerStartUp.xml'
    enabled=None
    if startup.is_file():
        tree=ET.fromstring(startup.read_bytes());enabled=set()
        for entry in tree.iter('Process'):
            if entry.get('Run','').casefold()not in ('true','1'):continue
            relative=entry.get('Path','').replace('\\','/')
            path=(group.parent/relative).resolve()
            if path in nodes:enabled.add(path)
    # Unknown topology is conservative; it is never inferred from open ports.
    consumers=[n for n in nodes if n.name in ('WorldServer.exe','BattleServer.exe','StoreServer.exe')and(enabled is None or n in enabled)]
    if not consumers:consumers=own
    return group,nodes,consumers


@contextmanager
def unmapped_image(path,replace=False):
    """Hold the exact executable closed through commit; never stop a process."""
    if os.name!='nt':
        yield;return
    import ctypes
    from ctypes import wintypes as w
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.CreateFileW.argtypes=(w.LPCWSTR,w.DWORD,w.DWORD,w.LPVOID,w.DWORD,w.DWORD,w.HANDLE);k.CreateFileW.restype=w.HANDLE
    k.CloseHandle.argtypes=(w.HANDLE,)
    # Permit verification reads. A writable handle still prevents SEC_IMAGE
    # creation, so a server cannot start until the commit barrier is released.
    handle=k.CreateFileW(str(path),0xC0000000,5 if replace else 1,None,3,0x80,None)
    if handle==ctypes.c_void_p(-1).value:
        raise ValueError('请先停止维护范围内的原生服务，并确认程序文件可访问：'+path.parent.name+'/'+path.name)
    try:yield
    finally:k.CloseHandle(handle)


def logs_before(nodes):
    return {str(p):p.stat().st_size for n in nodes for p in (n.parent/'LogData').glob('*.txt')if p.is_file()}


def channel_ports(ini,nodes):
    if not any(n.name=='BattleServer.exe'for n in nodes):return []
    from .database import connect
    db=connect(ini)
    try:
        values=[int(r[0])for r in db.cursor().execute('SELECT DISTINCT AddrPT FROM dbo.tbl_ChannelInfo').fetchall()]
        if any(not 1<=p<=65535 for p in values):raise ValueError('频道端口配置无效')
        return sorted(set(values))
    finally:db.close()


@contextmanager
def server_change(ini,action,effects=('fixed_catalog',)):
    """Shared gate for direct engine calls as well as the authenticated API."""
    group,nodes,consumers=topology(ini);key=target_key(ini);require_local_database(ini)
    active=getattr(_context,'changes',{})
    if key in active:
        yield active[key];return
    directory=group/'sdgo/local-state/changes'
    with ExitStack()as stack:
        stack.enter_context(lease('database:'+key))
        stack.enter_context(lease('server:'+str(group)))
        for node in nodes:stack.enter_context(unmapped_image(node))
        ports=channel_ports(ini,consumers)
        if not ports:consumers=[n for n in consumers if n.name!='BattleServer.exe']
        for node in consumers:
            if node.name=='StoreServer.exe':continue
            core=node.parent/'sdgo/core.dll'
            if not core.is_file()or core.stat().st_size>32*1024*1024 or CORE_CONTRACT not in core.read_bytes():
                raise ValueError('请先更新这个服务组的配套框架，再提交目录变更：'+node.parent.name)
        identity=uuid.uuid4().hex
        receipt={'schema_version':1,'id':identity,'action':action,'effects':list(effects),
                 'target':key,'status':'working','created_ns':time.time_ns(),
                 'nodes':[str(n)for n in consumers],'battle_ports':ports,'log_offsets':logs_before(consumers)}
        ticket=Change(directory,receipt);ticket.save();_context.changes={**active,key:ticket}
        try:
            yield ticket
        except BaseException:
            receipt['status']='recovery_required'if receipt.get('data_committed')else'failed';receipt['finished_ns']=time.time_ns()
            ticket.save();raise
        else:
            receipt['status']='pending_restart';receipt['committed_ns']=time.time_ns();ticket.save()
            for prior in directory.glob('*.json'):
                if prior.stem==identity:continue
                old=storage.read_json(prior,{})
                if old.get('target')!=key or old.get('action')!=action or old.get('status')not in('working','pending_restart','recovery_required'):continue
                family=receipt.get('content',{}).get('family_key');old_family=old.get('content',{}).get('family_key')
                if family and old_family and family!=old_family:continue
                if family and not old_family and '/drafts/'not in action:continue
                old['status']='superseded';old['superseded_by']=identity;storage.atomic(prior,storage.json_bytes(old))
        finally:_context.changes=active


class Change:
    def __init__(self,directory,value):self.directory,self.value=directory,value
    @property
    def id(self):return self.value['id']
    def save(self):storage.atomic(self.directory/(self.id+'.json'),storage.json_bytes(self.value))
    def result(self,value):
        # No account identifiers, balances, rows or credentials enter this receipt.
        self.value['content']={k:value[k]for k in ('family_key','revision','catalog_revision')if k in value}
        self.value['data_committed']=True;self.value['committed_ns']=time.time_ns();self.save()
        return {**value,'activation_id':self.id,'activation':'pending_restart'}


def activation(ini,identity):
    if not identity:return {'status':'legacy','message':'已有内容保持当前状态'}
    if not isinstance(identity,str)or len(identity)!=32 or any(c not in'0123456789abcdef'for c in identity):raise ValueError('生效回执无效')
    group,_,_=topology(ini);path=group/'sdgo/local-state/changes'/(identity+'.json')
    value=storage.read_json(path,{})
    if value.get('target')!=target_key(ini):return {'status':'unknown','message':'没有这个数据库的变更回执'}
    if value.get('status')!='pending_restart':return {'status':value.get('status','unknown')}
    from .runtime_state import native_activation
    states=[native_activation(Path(n),value)for n in value['nodes']]
    ports=value.get('battle_ports');covered=set()
    if ports is not None:
        for node,state in zip(value['nodes'],states):
            if Path(node).name!='BattleServer.exe':continue
            intersects=set(ports)&set(state.get('ports',[]))
            if state['ready']:covered.update(intersects)
            if not intersects:
                state.update(ready=True,required=False,reason='等待状态由已配置频道端口覆盖核对')
        missing=set(ports)-covered
        for port in sorted(missing):states.append({'node':'频道 '+str(port),'ready':False,'reason':'等待此频道的新进程、监听和原生表加载完成'})
    ready=bool(states)and all(n['ready']for n in states)
    if ready:
        value['status']='active';value['activated_ns']=time.time_ns();storage.atomic(path,storage.json_bytes(value))
    return {'status':'active'if ready else'pending_restart','nodes':states,
            'message':'原生服务已重新加载'if ready else'等待维护范围内的服务重启并就绪'}


def pending(ini):
    """Existing installs have no records; they remain available without a migration."""
    parent=Path(ini).resolve().parent
    if not any(any((root/'sdgo/local-state/changes').glob('*.json'))for root in(parent,parent.parent)):return False
    group,_,_=topology(ini)
    for path in(group/'sdgo/local-state/changes').glob('*.json'):
        try:
            value=storage.read_json(path)
            if value.get('status')in('failed','superseded','active'):continue
            if activation(ini,value['id'])['status']not in('active','legacy'):return True
        except (OSError,ValueError,KeyError):return True
    return False


def backup_tables(db,ticket,tables,max_bytes=128*1024*1024):
    """Retain retired module history locally; never return row contents over HTTP."""
    import base64
    def encode(value):
        if isinstance(value,bytes):return {'binary':base64.b64encode(value).decode('ascii')}
        if value is None or type(value)in(str,bool,int,float):return value
        return {'sql_text':str(value),'python_type':type(value).__name__}
    saved=[];total=0;root=ticket.directory/'backups'/ticket.id;root.mkdir(parents=True,exist_ok=False)
    cursor=db.cursor()
    try:
        for table in tables:
            if table not in ('sdgo_collection_capsule.Seen','sdgo_collection_capsule.Coverage','sdgo_collection_capsule.DrawLog'):raise ValueError('历史备份表未登记')
            if cursor.execute("SELECT OBJECT_ID(?,N'U')",table).fetchone()[0]is None:continue
            cursor.execute('SELECT * FROM '+table);columns=[c[0]for c in cursor.description]
            name=table.replace('.','_')+'.jsonl';count=0
            with(root/name).open('xb')as stream:
                for row in cursor:
                    raw=storage.json_bytes([encode(v)for v in row]);total+=len(raw)
                    if total>max_bytes:raise ValueError('旧历史超过自动备份预算，请先安排数据库备份；本次未删除历史')
                    stream.write(raw);count+=1
                stream.flush();os.fsync(stream.fileno())
            saved.append({'table':table,'columns':columns,'rows':count,'file':name})
        storage.atomic(root/'manifest.json',storage.json_bytes({'schema_version':1,'tables':saved,'bytes':total}))
        ticket.value['retained_history']={'tables':len(saved),'rows':sum(r['rows']for r in saved),'bytes':total}
    finally:cursor.close()
