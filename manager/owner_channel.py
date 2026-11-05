"""Owner-approved Mod packs. Game originals and private workspace data stay local."""
from __future__ import annotations
import io
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import threading
import time
from urllib.parse import urljoin, urlsplit
import uuid
import zipfile
from . import distribution, storage, updates
from .unit_receipts import BLOCKED as RESOURCE_BLOCKED

SERVICE = 'manager.owner_updates'
BINDING = 'sdgo/owner-source.json'
RECEIPT = 'sdgo/owner-release.json'
KIND = 'sdgo-owner-modpack'
RESOURCE_ROOTS = {'anrs','fxrs','gdrs','mdrs','scp','sdrs','sol','sprs','trns','txrs','uirs'}
SECRET_KEYS = {'password','passwd','pwd','username','loginaccount','loginpassword',
               'savedaccount','savedpassword','token','secret','apikey','privatekey'}


def https(value):
    if not isinstance(value,str) or len(value)>2048:raise ValueError('更新源地址无效')
    parsed=urlsplit(value)
    if parsed.scheme!='https' or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError('更新源须为不含账号、令牌或片段的 HTTPS 地址')
    return value


def publisher(value):
    if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{32}',value):raise ValueError('服主发布身份无效')
    return value


def binding(value):
    if not isinstance(value,dict) or value.get('schema_version')!=1 or value.get('kind')!=KIND:
        raise ValueError('服主更新源格式无效')
    urls=value.get('feeds')
    if not isinstance(urls,list) or not 1<=len(urls)<=3 or len(set(urls))!=len(urls):raise ValueError('填写一至三个不同的服主更新地址')
    label=value.get('label','')
    if not isinstance(label,str) or not label.strip() or len(label)>80:raise ValueError('填写服主或整合包名称')
    return {'schema_version':1,'kind':KIND,'publisher_id':publisher(value.get('publisher_id')),
            'label':label.strip(),'feeds':[https(u) for u in urls]}


def installed_binding(root):
    path=updates.child(root,BINDING)
    return binding(json.loads(path.read_bytes())) if path.exists() else None


def mode(host):
    # A copied player package remains bound even in a new Manager workspace.
    try:
        if installed_binding(host['path']) is not None:return 'player'
    except (ValueError,OSError):return 'blocked'
    return host.get('runtime_mode','starter')


def file_hash(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for raw in iter(lambda:stream.read(1024*1024),b''):h.update(raw)
    return h.hexdigest()


def allowed(name):
    updates.safe_name(name)
    parts=name.split('/')
    if name in ('d3d9.dll','sdgo/core.dll','sdgo/sdgo.ini','sdgo/catalog.json',BINDING):return True
    if re.fullmatch(r'sdgo/symbols/[A-Za-z]+-[0-9A-F]{8}-[0-9A-F]{8}\.ini',name):return True
    if len(parts)>=4 and parts[:2]==['sdgo','mods'] and re.fullmatch('[a-z][a-z0-9_]{0,63}',parts[2]):
        return not any(p.casefold() in {'.git','__pycache__','cache','logs','backups','history','local-state'} for p in parts[3:]) and Path(name).suffix.lower() not in {'.log','.dmp','.bak','.tmp','.py','.ps1','.bat','.cmd','.pdb','.mdf','.ldf'} and parts[-1].casefold() not in {'gonline.exe','battleserver.exe','agentserver.exe','worldserver.exe','install-receipt.json','state.json','history.json'}
    return len(parts)>1 and parts[0] in RESOURCE_ROOTS and Path(name).suffix.lower() not in RESOURCE_BLOCKED|{
        '.mdf','.ldf','.bak','.log','.dmp','.tmp','.pdb'}


def public_file(name,raw):
    if not allowed(name):raise ValueError('文件不属于可分发整合包：'+name)
    distribution.require_redistributable(name,raw)
    def private(key,value):
        return re.sub('[^a-z]','',str(key).lower()) in SECRET_KEYS and value not in ('',None,False,0)
    if name.lower().endswith('.ini'):
        cfg=storage.ini_values(raw)
        if any(private(k,v) for s in cfg.sections() for k,v in cfg[s].items()):raise ValueError('配置含私人凭据，不能发布：'+name)
    if name.lower().endswith('.json'):
        def inspect(v):
            if isinstance(v,dict):
                if any(private(k,x) for k,x in v.items()):raise ValueError('配置含私人凭据，不能发布：'+name)
                for x in v.values():inspect(x)
            elif isinstance(v,list):
                for x in v:inspect(x)
        inspect(json.loads(raw))


def snapshot(host,source,extras):
    root=Path(host['path']).resolve()
    if host['role']!='client':raise ValueError('玩家整合包从已配置的客户端生成')
    if updates.running(root/'GOnline.exe'):raise ValueError('关闭这个客户端后再生成发布快照')
    files={};excluded=[];watched={}
    def add(name):
        path=updates.child(root,name)
        if not path.is_file():raise ValueError('发布文件不存在：'+name)
        before=path.stat();raw=path.read_bytes();after=path.stat()
        if (before.st_mtime_ns,before.st_size)!=(after.st_mtime_ns,after.st_size):raise storage.Conflict('发布文件正在变化：'+name)
        if len(raw)>64*1024*1024:raise ValueError('单个整合包文件超过 64 MiB：'+name)
        public_file(name,raw);files[name]=raw
        watched[path]=(after.st_size,after.st_mtime_ns)
    for name in ('d3d9.dll','sdgo/core.dll','sdgo/sdgo.ini'):add(name)
    for path in sorted((root/'sdgo/symbols').glob('*.ini')):add(path.relative_to(root).as_posix())
    public_ids=set()
    for folder in sorted((root/'sdgo/mods').glob('*')):
        if not folder.is_dir() or not (folder/'mod.ini').is_file():continue
        name=folder.name;cfg=updates.read_ini(updates.child(root,'sdgo/mods/'+name+'/mod.ini').read_bytes())
        registration=updates.child(root,'sdgo/mods/'+name+'/manager.json')
        try:scope=distribution.resolve((folder/'mod.ini').read_bytes(),registration.read_bytes() if registration.exists() else None)
        except ValueError:
            if cfg.get('mod','enabled',fallback='1').strip()=='0':excluded.append(name);continue
            raise ValueError('启用的 Mod 缺少一致的分发声明，先登记或停用再发布：'+name) from None
        if scope==distribution.LOCAL_ONLY or cfg.get('mod','category',fallback='')=='development':excluded.append(name);continue
        if cfg.get('mod','id',fallback='')!=name:raise ValueError('Mod 目录与身份不匹配：'+name)
        public_ids.add(name)
        names={name+'.dll','mod.ini','manager.json'};configs=set()
        for key in ('files','runtime_files','config_files'):
            items={x.strip() for x in cfg.get('mod',key,fallback='').split(',') if x.strip()}
            names.update(items)
            if key=='config_files':configs=items
        for leaf in sorted(names):
            relative='sdgo/mods/'+name+'/'+updates.safe_name(leaf)
            if leaf in configs and not updates.child(root,relative).exists():continue
            if updates.child(root,relative).is_file():add(relative)
            else:raise ValueError('Mod 声明的发布文件不完整：'+relative)
        # Owner compilers declare generated resources separately from the
        # framework's static manifest. Read only their relative file/hash map;
        # authoring/install receipts themselves may contain private local paths.
        generated=folder/'manifest.json'
        if generated.is_file():
            data=json.loads(generated.read_bytes())
            for leaf,expected in data.get('files',{}).items():
                relative='sdgo/mods/'+name+'/'+updates.safe_name(leaf);add(relative)
                want=expected.get('sha256') if isinstance(expected,dict) else expected
                if updates.digest(files[relative])!=want:raise ValueError('生成资源与服主清单不一致：'+relative)
        receipt=folder/'install-receipt.json'
        if receipt.is_file() and name=='unit_catalog':
            for entry in json.loads(receipt.read_bytes()).get('files',[]):
                if not entry.get('owned',True):continue
                relative=entry['path'].replace('\\','/');add(relative)
                if updates.digest(files[relative])!=entry['installed_sha256']:raise ValueError('机体内容与服主安装收据不一致：'+relative)
    # Only explicitly selected authored extras are added, never an entire client.
    for name in extras:
        path=updates.child(root,name)
        if path.is_dir():
            for item in sorted(path.rglob('*')):
                if item.is_file():add(item.relative_to(root).as_posix())
        else:add(name)
    for name in files:
        if name.startswith('sdgo/mods/') and name.split('/')[2] not in public_ids:raise ValueError('额外文件不能绕过 Mod 分发声明：'+name)
    catalog=storage.read_json(root/'sdgo/catalog.json',{'schema_version':1,'mods':[],'features':{},'categories':{}})
    catalog['mods']=[m for m in catalog['mods'] if m['id'] in public_ids]
    files['sdgo/catalog.json']=storage.json_bytes(catalog)
    files[BINDING]=storage.json_bytes(source)
    if len(files)>10000 or sum(map(len,files.values()))>updates.MAX_EXPANDED:raise ValueError('整合包超过文件数量或大小限制')
    expected={n:{'bytes':len(b),'sha256':updates.digest(b)} for n,b in sorted(files.items())}
    host_id=updates.host_key(root/'GOnline.exe')
    game_base={}
    for name in ('GPackData.zdx','GPackData.zpk'):
        path=updates.child(root,name)
        if path.is_file():
            before=path.stat();game_base[name]={'bytes':before.st_size,'sha256':file_hash(path)}
            watched[path]=(before.st_size,before.st_mtime_ns)
    for path,stamp in watched.items():
        actual=path.stat()
        if (actual.st_size,actual.st_mtime_ns)!=stamp:raise storage.Conflict('发布期间文件发生变化，请重新预览')
    plan={'publisher_id':source['publisher_id'],'host_key':host_id,'host_sha256':file_hash(root/'GOnline.exe'),
          'game_base':game_base,'files':expected,'excluded_mods':excluded,'extras':extras}
    return files,plan,updates.digest(storage.json_bytes(plan))


def unpack(raw,feed,source,destination):
    if len(raw)!=feed['bytes'] or updates.digest(raw)!=feed['sha256']:raise ValueError('服主整合包校验失败')
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        infos=z.infolist();names=[updates.safe_name(x.filename) for x in infos]
        if len(names)>10000 or len({n.casefold() for n in names})!=len(names) or sum(x.file_size for x in infos)>updates.MAX_EXPANDED:
            raise ValueError('服主整合包文件清单无效')
        if any(x.is_dir() or ((x.external_attr>>16)&0o170000)==0o120000 or x.file_size>64*1024*1024 for x in infos):raise ValueError('整合包含目录、链接或过大文件')
        m=json.loads(z.read('owner-release.json'))
        if (m.get('kind')!=KIND or m.get('schema_version')!=1 or m.get('publisher_id')!=source['publisher_id'] or
            m.get('revision')!=feed['revision'] or not re.fullmatch('GOnline-[0-9A-F]{8}-[0-9A-F]{8}',m.get('host_key',''))):
            raise ValueError('服主整合包身份不匹配')
        if not re.fullmatch('[0-9a-f]{64}',m.get('host_sha256','')):raise ValueError('缺少原版客户端身份')
        bases=m.get('game_base',{})
        if not isinstance(bases,dict) or not set(bases)<={'GPackData.zdx','GPackData.zpk'}:raise ValueError('原版素材身份无效')
        for expected in bases.values():
            if type(expected.get('bytes'))is not int or expected['bytes']<0 or not re.fullmatch('[0-9a-f]{64}',expected.get('sha256','')):raise ValueError('原版素材身份无效')
        files=m.get('files')
        if not isinstance(files,dict) or set(names)!=set(files)|{'owner-release.json'}:raise ValueError('整合包清单不完整')
        payload={}
        for name,expected in files.items():
            data=z.read(name);public_file(name,data)
            if len(data)!=expected['bytes'] or updates.digest(data)!=expected['sha256']:raise ValueError('整合包文件损坏：'+name)
            payload[name]=data
        next_binding=binding(json.loads(payload[BINDING]))
        if any(next_binding[k]!=source[k] for k in ('publisher_id','feeds','kind','schema_version')):raise ValueError('更新包不能悄悄更换玩家绑定源')
        roots={'/'.join(n.split('/')[:3]) for n in payload if n.startswith('sdgo/mods/')}
        for base in roots:
            data=payload.get(base+'/mod.ini',b'');distribution.require_public(data,payload.get(base+'/manager.json'))
            cfg=updates.read_ini(data);identity=base.split('/')[-1]
            if (cfg.get('mod','id',fallback='')!=identity or cfg.get('mod','dll',fallback='')!=identity+'.dll' or
                base+'/'+identity+'.dll' not in payload or cfg.get('mod','category',fallback='')=='development'):
                raise ValueError('整合包 Mod 身份或运行程序不完整')
    destination.mkdir(parents=True,exist_ok=True)
    for name,data in payload.items():storage.atomic(updates.child(destination,name),data)
    return m


def install(host,bundle,m,cache,is_running=None):
    is_running=is_running or updates.running
    root=Path(host['path']).resolve();exe=root/'GOnline.exe'
    if host['role']!='client' or mode(host)!='player':raise ValueError('该客户端尚未绑定服主更新源')
    source=installed_binding(root)
    if m['publisher_id']!=source['publisher_id']:raise ValueError('发布者与玩家绑定不一致')
    if updates.host_key(exe)!=m['host_key'] or updates.digest(exe.read_bytes())!=m['host_sha256']:raise ValueError('整合包不匹配这个原版客户端')
    for name,expected in m.get('game_base',{}).items():
        path=updates.child(root,name)
        if not path.is_file() or path.stat().st_size!=expected['bytes'] or file_hash(path)!=expected['sha256']:
            raise ValueError('原版素材不匹配；请先安装该服主提供的兼容客户端底包')
    old=storage.read_json(root/RECEIPT,{})
    if old and old.get('publisher_id')!=source['publisher_id']:raise ValueError('原安装记录属于另一服主，请先明确切换绑定')
    if old.get('revision',0)>m['revision']:raise ValueError('拒绝自动回退到较旧的服主版本')
    if old.get('revision')==m['revision'] and old.get('files')!=m['files']:raise ValueError('服主重复使用了已有版本号')
    changes={};retired=[]
    for name,expected in m['files'].items():
        data=updates.child(bundle,name).read_bytes();public_file(name,data)
        if len(data)!=expected['bytes'] or updates.digest(data)!=expected['sha256']:raise ValueError('下载缓存已变化：'+name)
        dest=updates.child(root,name)
        if not dest.exists() or dest.read_bytes()!=data:changes[dest]=data
    for name,expected in old.get('files',{}).items():
        if name in m['files']:continue
        if not allowed(name):raise ValueError('原安装收据包含越界文件')
        path=updates.child(root,name)
        if path.exists():
            if updates.digest(path.read_bytes())!=expected['sha256']:raise storage.Conflict('已退休文件有本地修改，保留原件：'+name)
            retired.append(path)
    marker=updates.child(root,RECEIPT);changes[marker]=storage.json_bytes(m)
    if is_running(exe):return {'status':'waiting','message':'等待这个客户端关闭','revision':m['revision']}
    with storage.LOCK:
        previous={p:p.read_bytes() if p.exists() else None for p in [*changes,*retired]};written=[]
        backup=Path(cache)/'backups'/source['publisher_id']/str(m['revision'])/updates.digest(str(root).encode())[:16]
        try:
            # Save all recovery copies before the first runtime mutation.
            for path,data in previous.items():
                if data is not None:storage.atomic(updates.child(backup,path.relative_to(root).as_posix()),data)
            if is_running(exe):return {'status':'waiting','message':'等待这个客户端关闭','revision':m['revision']}
            for path,data in changes.items():storage.atomic(path,data);written.append(path)
            for path in retired:path.unlink();written.append(path)
        except Exception:
            for path in reversed(written):
                if previous[path] is None:path.unlink(missing_ok=True)
                else:storage.atomic(path,previous[path])
            raise
    return {'status':'installed','revision':m['revision'],'changed_files':len(changes)-1,'retired_files':len(retired)}


class OwnerChannels:
    def __init__(self,workspace,fetcher=updates.fetch):
        self.workspace=workspace;self.fetcher=fetcher;self.cache=workspace.root/'owner-updates'
        self.lock=threading.RLock();self.busy=threading.Lock();self.thread=None;self.stop_event=threading.Event()
        self.jobs=[];self.results={};self.plans={};self.downloads={}

    def state(self):
        with self.lock:
            hosts=[]
            for h in self.workspace.settings['hosts']:
                try:source=installed_binding(h['path']);error=''
                except (ValueError,OSError):source=None;error='本地负责人绑定损坏；请明确切换维护方式或重新绑定'
                cfg=h.get('publication',{})
                hosts.append({'id':h['id'],'name':h['name'],'role':h['role'],'mode':mode(h),'source':source,
                    'publication':cfg,'result':self.results.get(h['id'],{}) or ({'status':'error','message':error} if error else {}),
                    'installed_revision':storage.read_json(Path(h['path'])/RECEIPT,{}).get('revision')})
            return {'running':bool(self.thread and self.thread.is_alive()),'healthy':True,'hosts':hosts}

    def configure(self,host_id,value):
        host=self.workspace.host(host_id)
        chosen=value.get('mode')
        if chosen not in ('starter','owner','player'):raise ValueError('整合包维护方式无效')
        if host['role']!='client' and (chosen=='player' or 'publication' in value):raise ValueError('玩家绑定与发布快照只适用于客户端；服务端可切换 DIY 保护')
        if not self.busy.acquire(blocking=False):raise storage.Conflict('更新或发布进行中，请完成后更改绑定')
        root=Path(host['path'])
        try:
            if updates.running(root/'GOnline.exe'):raise ValueError('关闭这个客户端后更改更新绑定')
            prepared=copy.deepcopy(host);source=None
            try:old=installed_binding(root)
            except (ValueError,OSError):
                if chosen=='player':raise ValueError('先解除损坏的负责人绑定，再绑定新源')
                old={'invalid_binding':True}
            if chosen=='player':
                source=binding(value['source'])
                if old and old['publisher_id']!=source['publisher_id']:
                    raise ValueError('先解除原服主绑定，再选择新负责人')
                self.check_source(source)
            elif chosen=='owner':
                if 'publication' in value:
                    cfg=value.get('publication',{});directory=Path(cfg.get('directory','')).expanduser()
                    if not cfg.get('directory') or not directory.is_absolute():raise ValueError('请选择绝对发布目录')
                    directory=directory.resolve();base=root.resolve()
                    if directory.is_relative_to(base) or base.is_relative_to(directory):raise ValueError('发布目录必须与客户端目录分开')
                    urls=cfg.get('feeds',[])
                    identity=host.get('publication',{}).get('publisher_id') or uuid.uuid4().hex
                    source=binding({'schema_version':1,'kind':KIND,'publisher_id':identity,'label':cfg.get('label',''),'feeds':urls})
                    extras=cfg.get('extras',[])
                    if not isinstance(extras,list) or len(extras)>128 or not all(isinstance(n,str) and n for n in extras):raise ValueError('额外共享文件列表无效')
                    for name in extras:updates.child(root,name)
                    remote=cfg.get('git_remote','').strip();branch=cfg.get('branch','main')
                    if remote:https(remote)
                    if not re.fullmatch('[A-Za-z0-9][A-Za-z0-9_/-]{0,99}',branch) or '..' in branch or '//' in branch:raise ValueError('发布分支名称无效')
                    prepared['publication']={**source,'directory':str(directory),'extras':extras,'git_remote':remote,'branch':branch}
            prepared['runtime_mode']=chosen
            with storage.LOCK:
                before_host=copy.deepcopy(host);before_services=copy.deepcopy(self.workspace.settings.get('services'))
                paths=[updates.child(root,n) for n in (BINDING,RECEIPT)]
                before_files={p:p.read_bytes() if p.exists() else None for p in paths}
                try:
                    if source is not None and chosen=='player':storage.atomic(paths[0],storage.json_bytes(source))
                    elif old:
                        # Explicit role change is the only path out of a player binding.
                        saved=self.cache/'unbound'/uuid.uuid4().hex
                        storage.atomic(saved/'source.json',before_files[paths[0]])
                        if paths[1].exists():storage.atomic(saved/'release.json',before_files[paths[1]]);paths[1].unlink()
                        paths[0].unlink()
                    host.clear();host.update(prepared)
                    if chosen=='player' and SERVICE not in self.workspace.settings.get('services',{}):
                        self.workspace.settings.setdefault('services',{})[SERVICE]={'enabled':True}
                    self.workspace.save_workspace()
                except Exception:
                    host.clear();host.update(before_host)
                    if before_services is None:self.workspace.settings.pop('services',None)
                    else:self.workspace.settings['services']=before_services
                    for p,raw in before_files.items():
                        if raw is None:p.unlink(missing_ok=True)
                        else:storage.atomic(p,raw)
                    raise
                self.plans.pop(host_id,None);self.downloads.pop(host_id,None)
            if chosen=='player' and self.workspace.services.desired(SERVICE):self.start()
        finally:self.busy.release()
        return self.state()

    def preview(self,host_id):
        host=self.workspace.host(host_id)
        if mode(host)!='owner':raise ValueError('先选择服主 DIY 维护方式')
        cfg=host.get('publication',{})
        if not cfg.get('directory'):raise ValueError('DIY 已接管；先配置发布目录和公开更新地址，再生成快照')
        source=binding(cfg)
        files,plan,digest=snapshot(host,source,cfg.get('extras',[]))
        with self.lock:self.plans[host_id]={'digest':digest,'plan':plan}
        return {**plan,'digest':digest,'file_count':len(files),'bytes':sum(map(len,files.values()))}

    def request(self,action,host_id,digest=None):
        if action not in ('publish','check','install','verify_publish'):raise ValueError('发布或更新操作无效')
        self.workspace.host(host_id)
        with self.lock:self.jobs.append((action,host_id,digest));self.results[host_id]={'status':'queued'}
        self.start();return self.state()

    def check_source(self,source):
        last=None
        for url in source['feeds']:
            try:raw=self.fetcher(url,2*1024*1024,self.stop_event)
            except (OSError,TimeoutError) as e:last=e;continue
            feed=json.loads(raw)
            if (feed.get('schema_version')!=1 or feed.get('kind')!=KIND or feed.get('publisher_id')!=source['publisher_id'] or
                type(feed.get('revision')) is not int or feed['revision']<1 or
                type(feed.get('bytes')) is not int or not 0<feed['bytes']<=updates.MAX_ARCHIVE or
                not re.fullmatch('[0-9a-f]{64}',str(feed.get('sha256','')))):
                raise ValueError('服主发布源身份或校验清单无效')
            asset=feed.get('asset','')
            updates.safe_name(asset)
            if '/' not in asset or not asset.endswith('.zip'):raise ValueError('整合包下载路径无效')
            feed['url']=https(urljoin(url,asset));return feed
        raise ValueError('服主更新源暂不可用；保留当前版本，不改用官方整合包') from last

    def resolve(self,urls):
        if not isinstance(urls,list) or not 1<=len(urls)<=3:raise ValueError('填写一至三个更新地址')
        urls=[https(u) for u in urls]
        for url in urls:
            try:feed=json.loads(self.fetcher(url,2*1024*1024,self.stop_event))
            except OSError:continue
            source=binding({'schema_version':1,'kind':KIND,'publisher_id':feed.get('publisher_id'),
                            'label':feed.get('label'),'feeds':urls})
            validated=self.check_source(source)
            return {'source':source,'revision':validated['revision'],'sha256':validated['sha256']}
        raise ValueError('负责人更新源暂不可用')

    def check(self,host_id,apply=False):
        host=self.workspace.host(host_id)
        source=installed_binding(host['path'])
        if mode(host)!='player' or source is None:raise ValueError('先绑定负责人的发布源')
        feed=self.check_source(source);old=storage.read_json(Path(host['path'])/RECEIPT,{})
        if old.get('revision',0)>feed['revision']:raise ValueError('拒绝较旧的服主发布版本')
        folder=self.cache/'downloads'/source['publisher_id']/(str(feed['revision'])+'-'+feed['sha256'][:16])
        raw=self.fetcher(feed['url'],updates.MAX_ARCHIVE,self.stop_event)
        m=unpack(raw,feed,source,folder)
        self.downloads[host_id]=(folder,m)
        result=install(host,folder,m,self.cache) if apply else {'status':'available','revision':m['revision'],'file_count':len(m['files'])}
        with self.lock:self.results[host_id]=result
        return result

    def publish(self,host_id,expected):
        host=self.workspace.host(host_id)
        if mode(host)!='owner':raise ValueError('只有服主 DIY 目录可以发布')
        cfg=host['publication'];source=binding(cfg);files,plan,digest=snapshot(host,source,cfg.get('extras',[]))
        if not expected or expected!=digest or self.plans.get(host_id,{}).get('digest')!=expected:raise storage.Conflict('发布预览已变化，请重新检查')
        output=Path(cfg['directory']);output.mkdir(parents=True,exist_ok=True)
        feed_path=output/'owner-feed.json';old=storage.read_json(feed_path,{})
        if old and old.get('publisher_id')!=source['publisher_id']:raise ValueError('发布目录属于另一发布者')
        revision=max(old.get('revision',0),host.get('published_revision',0))+1
        manifest={k:v for k,v in plan.items() if k not in ('excluded_mods','extras')}
        manifest.update(schema_version=1,kind=KIND,revision=revision,label=source['label'])
        stream=io.BytesIO()
        with zipfile.ZipFile(stream,'w',compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr('owner-release.json',storage.json_bytes(manifest))
            for name,data in sorted(files.items()):z.writestr(name,data)
        raw=stream.getvalue()
        if len(raw)>updates.MAX_ARCHIVE:raise ValueError('整合包超过 128 MiB，请缩小所选共享素材')
        sha=updates.digest(raw);asset=f'releases/{source["publisher_id"]}-{revision}-{sha[:16]}.zip'
        feed={'schema_version':1,'kind':KIND,'publisher_id':source['publisher_id'],'label':source['label'],
              'revision':revision,'asset':asset,'bytes':len(raw),'sha256':sha}
        # Assets are immutable; the feed pointer is replaced only after validation.
        validate=self.cache/'publish-check'/uuid.uuid4().hex
        unpack(raw,feed,source,validate)
        if cfg.get('git_remote'):self.git_prepare(output,cfg)
        storage.atomic(updates.child(output,asset),raw);storage.atomic(feed_path,storage.json_bytes(feed))
        if cfg.get('git_remote'):
            self.git(output,['add','--','owner-feed.json','releases'])
            self.git(output,['commit','-m',f'Publish owner Mod pack revision {revision}'])
        return self.verify_publish(host_id)

    def verify_publish(self,host_id):
        host=self.workspace.host(host_id)
        if mode(host)!='owner':raise ValueError('只有服主可以核对发布')
        cfg=host['publication'];source=binding(cfg);output=Path(cfg['directory'])
        feed=storage.read_json(output/'owner-feed.json',{})
        if feed.get('publisher_id')!=source['publisher_id']:raise ValueError('发布目录身份不匹配')
        raw=updates.child(output,feed['asset']).read_bytes()
        if updates.digest(raw)!=feed['sha256']:raise ValueError('本地发布资产已变化')
        if cfg.get('git_remote'):
            if self.git(output,['status','--porcelain']).strip():raise ValueError('发布仓库有未提交修改，请先处理')
            self.git(output,['push',cfg['git_remote'],'HEAD:refs/heads/'+cfg['branch']])
        # Read the actual public endpoint. A local export alone is not publication proof.
        public=self.check_source(source)
        if public['revision']!=feed['revision'] or public['sha256']!=feed['sha256']:raise ValueError('本地包已生成，但公开源尚未同步；同步后使用核对发布，无需重复生成版本')
        public_raw=self.fetcher(public['url'],updates.MAX_ARCHIVE,self.stop_event)
        if public_raw!=raw:raise ValueError('公开下载与刚发布的整合包不一致')
        with storage.LOCK:host['published_revision']=feed['revision'];self.workspace.save_workspace()
        result={'status':'published','revision':feed['revision'],'sha256':feed['sha256'],'source':source}
        with self.lock:self.results[host_id]=result
        return result

    @staticmethod
    def git(root,args):
        r=subprocess.run(['git','-C',str(root),*args],capture_output=True,timeout=180,
                         creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if r.returncode:raise ValueError('Git 发布未完成；检查仓库权限、分支与已有凭据后重试')
        return r.stdout

    def git_prepare(self,root,cfg):
        if not (root/'.git').exists():
            if any(root.iterdir()):raise ValueError('Git 发布请选择已克隆的干净仓库或空目录')
            self.git(root,['init','-b',cfg['branch']])
            self.git(root,['remote','add','origin',cfg['git_remote']])
            # Reuse existing history and credentials instead of replacing a
            # remote branch with a newly initialized unrelated history.
            refs=self.git(root,['ls-remote','--heads','origin','refs/heads/'+cfg['branch']])
            if refs.strip():
                self.git(root,['fetch','--depth=1','origin',cfg['branch']])
                self.git(root,['reset','--hard','FETCH_HEAD'])
        if self.git(root,['status','--porcelain']).strip():raise storage.Conflict('发布仓库有未提交修改，请先处理')
        branch=self.git(root,['symbolic-ref','--short','HEAD']).decode().strip()
        if branch!=cfg['branch']:raise ValueError('发布仓库当前分支与所选分支不一致')

    def start(self):
        if self.thread and self.thread.is_alive():return
        self.stop_event.clear()
        def run():
            next_poll=0
            while not self.stop_event.is_set():
                with self.lock:job=self.jobs.pop(0) if self.jobs else None
                if job:
                    action,identity,digest=job
                    with self.busy:
                        try:
                            with self.lock:self.results[identity]={'status':'working'}
                            if action=='publish':self.publish(identity,digest)
                            elif action=='verify_publish':self.verify_publish(identity)
                            else:self.check(identity,action=='install')
                        except Exception as e:
                            with self.lock:self.results[identity]={'status':'error','message':str(e)}
                elif self.workspace.services.desired(SERVICE) and time.monotonic()>=next_poll:
                    next_poll=time.monotonic()+3600
                    for host in self.workspace.settings['hosts']:
                        if host['role']=='client' and mode(host)=='player':
                            with self.lock:self.jobs.append(('install',host['id'],None))
                # A downloaded update waiting for game closure uses the same sealed cache.
                for identity,(folder,m) in list(self.downloads.items()):
                    if self.results.get(identity,{}).get('status')=='waiting':
                        try:
                            result=install(self.workspace.host(identity),folder,m,self.cache)
                            with self.lock:self.results[identity]=result
                        except Exception as e:
                            with self.lock:self.results[identity]={'status':'error','message':str(e)}
                self.stop_event.wait(.5)
        self.thread=threading.Thread(target=run,name='owner-modpack-updates',daemon=True);self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:self.thread.join(timeout=25)


def attach(workspace):
    c=OwnerChannels(workspace);workspace.owner_channels=c
    workspace.services.register(SERVICE,'服主整合包更新',c,c.start)
    return c


def sync_client(game_dir):
    """Headless entry in the same Manager EXE for an owner's player shortcut."""
    from .workspace import Workspace
    root=Path(game_dir).resolve()
    if not (root/'GOnline.exe').is_file() or installed_binding(root) is None:raise ValueError('这个客户端没有服主更新绑定')
    workspace=Workspace(updates.child(root,'sdgo/local-state/owner-updater'),Path(__file__).parent/'addons')
    try:
        workspace.add_host(root);host=workspace.settings['hosts'][0];host['runtime_mode']='player'
        controller=OwnerChannels(workspace);result=controller.check(host['id'],True)
        storage.atomic(workspace.root/'result.json',storage.json_bytes(result))
        return 0 if result['status'] in ('installed','current') else 2
    except Exception as e:
        storage.atomic(workspace.root/'result.json',storage.json_bytes({'status':'error','message':str(e)}))
        return 1
    finally:workspace.close()
