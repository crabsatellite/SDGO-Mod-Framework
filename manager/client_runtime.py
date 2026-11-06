"""Complete client updates owned by Manager, with pinned sources and recovery.

The descriptor is supplied by the client distributor. Framework releases never
contain original game programs, archive segments or deployment-specific URLs.
"""
from __future__ import annotations
from pathlib import Path
from contextlib import ExitStack
import hashlib,json,os,re,shutil,subprocess,threading,time,uuid,zipfile
from . import storage,updates,distribution,git_source
from .target_locks import lease

SOURCE='sdgo/client-source.json'
RECEIPT='sdgo/local-state/client-runtime.json'
SERVICE='manager.client_updates'
PROTECTED=('config.xml','.git','ScreenShots','app_log','zoa_log','sdgo/logs','sdgo/local-mods',
           'sdgo/local-state','sdgo/backups','sdgo/install-receipts','sdgo/install-state')
EXTENSIONS={'.exe','.dll','.asi','.ini','.xml','.ttf','.dat','.des','.mpg','.zdx','.zpk'}
TREES={'BGM','Briefing','miles','mods','anrs','fxrs','gdrs','mdrs','scp','sdrs','sol','sprs','trns','txrs','uirs','sdgo/mods','sdgo/symbols'}
FIXED={'sdgo/core.dll','sdgo/sdgo.ini','sdgo/catalog.json',SOURCE}
HASH=re.compile('[0-9a-f]{64}');OID=re.compile('[0-9a-f]{40}')


def source(value):
    if(not isinstance(value,dict)or value.get('schema_version')!=1 or value.get('kind')!='sdgo-client-runtime'or
       not re.fullmatch('[0-9a-f]{32}',str(value.get('source_id','')))):
        raise ValueError('完整客户端更新源身份无效')
    feeds=value.get('feeds');label=value.get('label')
    if not isinstance(feeds,list)or not 1<=len(feeds)<=3 or len(set(feeds))!=len(feeds):raise ValueError('配置一至三个不同的客户端 Git 源')
    for url in feeds:git_source.parse(url)
    if not isinstance(label,str)or not label.strip()or len(label)>80:raise ValueError('客户端更新源名称无效')
    return {'schema_version':1,'kind':'sdgo-client-runtime','source_id':value['source_id'],'label':label.strip(),'feeds':feeds}


def installed_source(root):
    path=updates.child(root,SOURCE)
    return source(storage.read_json(path))if path.is_file()else None


def protected(name):
    return any(name.casefold()==p.casefold()or name.casefold().startswith(p.casefold()+'/')for p in PROTECTED)


def file_hash(path,git=False):
    result=hashlib.sha1()if git else hashlib.sha256()
    with Path(path).open('rb')as stream:
        if git:result.update(('blob '+str(os.fstat(stream.fileno()).st_size)+'\0').encode())
        for chunk in iter(lambda:stream.read(1024*1024),b''):result.update(chunk)
    return result.hexdigest()


def matches(path,entry):
    return path.is_file()and path.stat().st_size==entry['bytes']and file_hash(path,'git_blob'in entry)==entry.get('git_blob',entry.get('sha256'))


def runtime_path(name,contract):
    updates.safe_name(name)
    if protected(name):return False
    if name in FIXED:return True
    if '/'not in name:return Path(name).suffix.casefold()in contract['root_extensions']or name in contract['entrypoints']
    return any(name.startswith(tree+'/')for tree in contract['trees'])


def validate_contract(manifest,binding):
    c=manifest.get('runtime',{})
    if c.get('schema_version')!=1 or c.get('source_id')!=binding['source_id']:raise ValueError('客户端清单与所选发布者不一致')
    if not isinstance(c.get('root_extensions'),list)or not set(c['root_extensions'])<=EXTENSIONS:raise ValueError('客户端文件范围无效')
    if not isinstance(c.get('trees'),list)or not set(c['trees'])<=TREES:raise ValueError('客户端资源范围无效')
    if c.get('entrypoints')not in([],['SDGO Mod Manager.exe']):raise ValueError('客户端更新必须由 Manager 统一管理')
    for name in c.get('remove_paths',[]):
        updates.safe_name(name)
        if protected(name)or Path(name).suffix.casefold()in('.mdf','.ldf','.ndf'):raise ValueError('退役清单与私人数据范围冲突')
    b=c.get('base',{})
    if b.get('path')!='GPackData.zpk'or not OID.fullmatch(str(b.get('commit','')))or not HASH.fullmatch(str(b.get('sha256',''))):raise ValueError('原版归档身份无效')
    if type(b.get('bytes'))is not int or not 0<b['bytes']<=0xffffffff:raise ValueError('原版归档大小无效')
    parts=b.get('parts');seen=set();total=0
    if not isinstance(parts,list)or not 1<=len(parts)<=512:raise ValueError('原版归档分段无效')
    for p in parts:
        if(not re.fullmatch('GPackData\\.zpk\\.part[0-9]+\\.zip',str(p.get('path','')))or p['path']in seen or
           p.get('entry')!='GPackData.zpk'or not HASH.fullmatch(str(p.get('sha256','')))or
           type(p.get('bytes'))is not int or not 0<p['bytes']<=64*1024*1024 or
           type(p.get('unpacked_bytes'))is not int or not 0<p['unpacked_bytes']<=64*1024*1024):raise ValueError('原版归档分段清单无效')
        total+=p['unpacked_bytes'];seen.add(p['path'])
    if total!=b['bytes']:raise ValueError('原版归档分段不完整')
    return c


class ClientRuntime:
    def __init__(self,workspace):
        self.workspace=workspace;self.cache=workspace.root/'client-updates';self.stop_event=threading.Event()
        self.lock=threading.RLock();self.busy=threading.Lock();self.thread=None;self.jobs=[];self.results={};self.plans={}

    def git(self,repository,args,input=None,output=None):
        from .owned_process import run
        result=run(
            ['git','-C',str(repository),*args],cwd=repository,timeout=1200 if output is not None or args[:1]==['cat-file']else 120,
            stop=self.stop_event,env=git_source.environment(),input=input,output=output)
        if result.returncode:raise ValueError(git_source.failure(result.stderr))
        return result.stdout

    def repository(self,binding):
        errors=[]
        for index,url in enumerate(binding['feeds']):
            if self.stop_event.is_set():raise InterruptedError('客户端更新已取消')
            remote,ref=git_source.parse(url);repo=self.cache/'git'/updates.digest(url.encode());repo.mkdir(parents=True,exist_ok=True)
            try:
                if not(repo/'HEAD').is_file():self.git(repo,['init','--bare']);self.git(repo,['remote','add','origin',remote])
                if self.git(repo,['config','--get','remote.origin.url']).decode().strip()!=remote:raise ValueError('客户端缓存身份不一致')
                self.git(repo,['fetch','--filter=blob:none','--no-tags','--depth=1','origin','refs/heads/'+ref])
                commit=self.git(repo,['rev-parse','FETCH_HEAD']).decode().strip()
                if not OID.fullmatch(commit):raise ValueError('客户端提交身份无效')
                manifest=json.loads(self.git(repo,['show',commit+':player-package.json']))
                contract=validate_contract(manifest,binding)
                return repo,commit,manifest,contract,index+1
            except (OSError,ValueError,subprocess.SubprocessError)as error:errors.append('源 '+str(index+1)+'：'+str(error))
        raise ValueError('；'.join(errors)+'。保留当前客户端。')

    def local_modules(self,root):
        excluded=set()
        for folder in(root/'sdgo/mods').glob('*'):
            if not folder.is_dir():continue
            ini=(folder/'mod.ini').read_bytes()if(folder/'mod.ini').is_file()else b''
            registration=(folder/'manager.json').read_bytes()if(folder/'manager.json').is_file()else None
            hint=b'local_only'in ini.lower()or(registration and b'local_only'in registration.lower())
            marker=any(distribution.NONREDISTRIBUTABLE_MARKER in f.read_bytes()for f in folder.glob('*.dll'))
            if hint or marker:
                if distribution.resolve(ini,registration)!=distribution.LOCAL_ONLY or not marker:raise ValueError('仅本地 Mod 的保护声明不完整：'+folder.name)
                excluded.add('sdgo/mods/'+folder.name)
        return excluded

    def plan(self,host):
        from .owner_channel import mode
        if host['role']!='client':raise ValueError('完整运行更新仅用于客户端')
        if mode(host)=='owner':raise ValueError('此目录已由服主 DIY 接管；请审阅上游组件或从发布目录维护完整包')
        root=Path(host['path']).resolve();binding=installed_source(root)
        if root==root.parent:raise ValueError('请选择独立的客户端文件夹，不使用磁盘根目录')
        if self.unresolved(root):raise ValueError('上次完整更新未完成；请先使用「恢复未完成更新」核对并恢复原文件')
        if binding is None:raise ValueError('这个客户端未配置完整运行更新源，请使用服主提供的客户端包或更新')
        from .owner_channel import installed_binding
        old_binding=installed_binding(root)
        if old_binding and old_binding['publisher_id']!=binding['source_id']:
            raise ValueError('原负责人和完整客户端来源不同，请先审阅来源迁移，保留现有内容')
        repo,commit,manifest,contract,index=self.repository(binding);excluded=self.local_modules(root);entries={};case=set()
        for raw in self.git(repo,['ls-tree','-r','-l','-z',commit]).split(b'\0'):
            if not raw:continue
            header,name=raw.split(b'\t',1);name=name.decode('utf-8')
            if name=='config.xml':continue
            if not runtime_path(name,contract):continue
            if any(name==p or name.startswith(p+'/')for p in excluded):raise ValueError('上游与仅本地 Mod 冲突：'+name)
            match=re.fullmatch(rb'100(?:644|755) blob ([a-f0-9]{40})\s+([0-9]+)',header)
            if not match or name.casefold()in case:raise ValueError('客户端清单包含链接或重复路径')
            size=int(match[2])
            if size>512*1024*1024:raise ValueError('客户端单文件超过限制')
            entries[name]={'git_blob':match[1].decode(),'bytes':size};case.add(name.casefold());updates.child(root,name)
        if not {'GOnline.exe',updates.MANAGER,'sdgo/core.dll',SOURCE}<=entries.keys():raise ValueError('完整客户端清单缺少必要程序或更新源')
        incoming=source(json.loads(self.git(repo,['show',commit+':'+SOURCE])))
        if incoming!=binding:raise ValueError('上游试图改变客户端绑定；请在 Manager 明确选择新的来源')
        entries['GPackData.zpk']={k:contract['base'][k]for k in('sha256','bytes')}
        if not 4<=len(entries)<=100000 or sum(e['bytes']for e in entries.values())>16*1024**3:raise ValueError('客户端清单超过限制')
        previous=storage.read_json(root/RECEIPT,{})
        if previous and previous.get('source_id')!=binding['source_id']:raise ValueError('此目录已有不同来源的安装记录，请先审阅迁移')
        # Private/addon-only files remain outside the source-owned inventory.
        # Authored INI settings use the framework's existing preservation rules.
        preserved={};merged={}
        for ini in(root/'sdgo/mods').glob('*/mod.ini'):
            relative=ini.relative_to(root).as_posix()
            if relative not in entries:continue
            cfg=updates.read_ini(ini.read_bytes())
            for name in cfg.get('mod','config_files',fallback='').split(','):
                if not name.strip():continue
                path=updates.child(ini.parent,name.strip());key=path.relative_to(root).as_posix()
                if key in entries and path.is_file():preserved[key]={'bytes':path.stat().st_size,'sha256':file_hash(path)}
            incoming=self.git(repo,['show',commit+':'+relative]);patches=[]
            for section in cfg.sections():
                for key,value in cfg[section].items():
                    if section.casefold()=='settings'or(section.casefold()=='mod'and key.casefold()=='enabled'):patches.append((section,key,value))
            raw=storage.patch_ini(incoming,patches)
            if raw!=incoming:merged[relative]={'bytes':len(raw),'sha256':updates.digest(raw)}
        global_path=root/'sdgo/sdgo.ini'
        if 'sdgo/sdgo.ini'in entries and global_path.is_file():preserved['sdgo/sdgo.ini']={'bytes':global_path.stat().st_size,'sha256':file_hash(global_path)}
        effective={**entries,**preserved,**merged}
        needed=[n for n,e in effective.items()if not matches(updates.child(root,n),e)]
        retired=set(contract.get('remove_paths',[]))|{n for n in previous.get('files',{})if n not in entries and runtime_path(n,contract)}
        retired=sorted(n for n in retired if n not in entries and not protected(n)and not any(n==p or n.startswith(p+'/')for p in excluded)and updates.child(root,n).is_file())
        snapshot={n:file_hash(updates.child(root,n))if updates.child(root,n).is_file()else None for n in set(needed)|set(retired)}
        value={'source_id':binding['source_id'],'commit':commit,'entries':entries,'needed':needed,'retired':retired,
               'snapshot':snapshot,'binding':binding,'mirror':index,'contract':contract,'repository':str(repo),
               'effective':effective,'preserved':preserved,'merged':list(merged)}
        value['digest']=updates.digest(storage.json_bytes(value));self.plans[host['id']]=value
        return {'status':'available'if needed or retired else'current','commit':commit,'source':binding['label'],
                'changed_files':len(needed),'retired_files':len(retired),'bytes':sum(entries[n]['bytes']for n in needed),'preserved_settings':len(preserved)+len(merged),
                'digest':value['digest'],'files':needed,'retired':retired,'data_migrations':False}

    def stage(self,plan,run,root):
        repo=Path(plan['repository']);commit=plan['commit'];stage=run/'payload';stage.mkdir(parents=True)
        needed=[n for n in plan['needed']if n!='GPackData.zpk']
        if needed:
            # Read raw objects. git archive/checkouts can apply EOL or export
            # attributes and therefore do not prove byte-identical delivery.
            archive=run/'runtime.blobs'
            requests=b''.join((plan['entries'][n]['git_blob']+'\n').encode('ascii')for n in needed)
            self.git(repo,['cat-file','--batch'],input=requests,output=archive)
            with archive.open('rb')as stream:
                for name in needed:
                    entry=plan['entries'][name];header=stream.readline(200)
                    if header!=(entry['git_blob']+' blob '+str(entry['bytes'])+'\n').encode():raise ValueError('下载文件清单无效：'+name)
                    path=updates.child(stage,name);path.parent.mkdir(parents=True,exist_ok=True)
                    remaining=entry['bytes']
                    with path.open('xb')as dst:
                        while remaining:
                            chunk=stream.read(min(remaining,1024*1024))
                            if not chunk:raise ValueError('Git 文件内容不完整')
                            dst.write(chunk);remaining-=len(chunk)
                    if stream.read(1)!=b'\n':raise ValueError('Git 文件边界无效')
                if stream.read(1):raise ValueError('Git 返回了额外文件')
            archive.unlink()
        if 'GPackData.zpk'in plan['needed']:
            base=plan['contract']['base']
            self.git(repo,['fetch','--filter=blob:none','--no-tags','--depth=1','origin',base['commit']])
            destination=stage/'GPackData.zpk'
            with destination.open('xb')as output:
                for part in base['parts']:
                    if self.stop_event.is_set():raise InterruptedError('客户端更新已取消')
                    segment=self.cache/'segments'/(part['sha256']+'.zip');segment.parent.mkdir(parents=True,exist_ok=True)
                    with lease('client-segment:'+str(segment.resolve())):
                        if not segment.is_file()or segment.stat().st_size!=part['bytes']or file_hash(segment)!=part['sha256']:
                            temporary=run/'segment.download';self.git(repo,['show',base['commit']+':'+part['path']],output=temporary)
                            if temporary.stat().st_size!=part['bytes']or file_hash(temporary)!=part['sha256']:raise ValueError('归档分段校验失败')
                            os.replace(temporary,segment)
                    with zipfile.ZipFile(segment)as z:
                        if z.namelist()!=[part['entry']]or z.getinfo(part['entry']).file_size!=part['unpacked_bytes']:raise ValueError('归档分段解包清单无效')
                        with z.open(part['entry'])as src:shutil.copyfileobj(src,output,1024*1024)
        for name in plan['needed']:
            if not matches(updates.child(stage,name),plan['entries'][name]):raise ValueError('下载文件校验失败：'+name)
        for name in plan['merged']:
            if name not in plan['needed']:continue
            old=updates.read_ini(updates.child(root,name).read_bytes());patches=[]
            for section in old.sections():
                for key,value in old[section].items():
                    if section.casefold()=='settings'or(section.casefold()=='mod'and key.casefold()=='enabled'):patches.append((section,key,value))
            path=updates.child(stage,name);storage.atomic(path,storage.patch_ini(path.read_bytes(),patches))
            if not matches(path,plan['effective'][name]):raise storage.Conflict('保留配置发生变化，请重新检查')
        return stage

    def install(self,host,expected):
        plan=self.plans.get(host['id'])
        if not plan or expected!=plan['digest']:raise storage.Conflict('请先检查完整客户端更新')
        root=Path(host['path']).resolve()
        with lease('host:'+str(root).casefold()):
            from .owner_channel import mode
            if mode(host)=='owner':raise ValueError('DIY 目录不能自动同步')
            if self.unresolved(root):raise ValueError('请先恢复上次未完成的完整客户端更新')
            if installed_source(root)!=plan['binding']:raise storage.Conflict('更新源已变化，请重新检查')
            if updates.running(root/'GOnline.exe'):return {'status':'waiting','message':'请关闭这个客户端后重试；不会结束游戏进程'}
            identity=uuid.uuid4().hex;run=self.cache/'runs'/identity;run.mkdir(parents=True)
            stage=self.stage(plan,run,root)
            # Finish from a verified copy of the same Manager, so its installed
            # executable can be replaced together with the rest of the runtime.
            own=getattr(getattr(self.workspace,'updates',None),'manager_exe',None)
            if own and Path(own).resolve()==(root/updates.MANAGER).resolve()and updates.MANAGER in plan['needed']:
                return self.handoff(host,plan,run,stage)
            return self.commit(host,plan,run,stage)

    def commit(self,host,plan,run,stage):
        root=Path(host['path']).resolve();backup=run/'recovery';changes=[]
        with lease('host:'+str(root).casefold()),ExitStack()as barrier:
            from .owner_channel import mode
            if mode(host)=='owner'or installed_source(root)!=plan['binding']:raise storage.Conflict('维护方式或更新来源已改变，请重新检查')
            if updates.running(root/'GOnline.exe'):return {'status':'waiting','message':'请关闭这个客户端后重试'}
            from .operations import unmapped_image
            for executable in(root/'GOnline.exe',stage/'GOnline.exe'):
                if executable.is_file():barrier.enter_context(unmapped_image(executable,replace=True))
            for name,old in plan['snapshot'].items():
                path=updates.child(root,name)
                if(file_hash(path)if path.is_file()else None)!=old:raise storage.Conflict('本地文件已改变，请重新检查：'+name)
            for name in plan['needed']:
                if not matches(updates.child(stage,name),plan['effective'][name]):raise ValueError('暂存客户端文件已改变：'+name)
            core=root/'sdgo/core.dll';incoming=stage/'sdgo/core.dll'
            from .operations import CORE_CONTRACT
            if incoming.is_file()and core.is_file()and CORE_CONTRACT in core.read_bytes()and CORE_CONTRACT not in incoming.read_bytes():
                raise ValueError('上游框架缺少数据维护保护，不能降级安装')
            from .dependencies import enforce_install
            metadata={n:updates.child(stage,n).read_bytes()for n in plan['needed']if n.endswith('/manager.json')or n.endswith('.ini')}
            metadata.update({n:None for n in plan['retired']if n.endswith('/manager.json')or n.endswith('.ini')})
            enforce_install(root,metadata,'client')
            journal={'schema_version':1,'status':'working','host':str(root),'source_id':plan['source_id'],'commit':plan['commit'],'changes':changes}
            storage.atomic(run/'transaction.json',storage.json_bytes(journal))
            try:
                for name in [*plan['retired'],*sorted(plan['needed'],key=lambda n:n==updates.MANAGER)]:
                    if self.stop_event.is_set():raise InterruptedError('客户端更新已取消')
                    target=updates.child(root,name);saved=updates.child(backup,name);saved.parent.mkdir(parents=True,exist_ok=True)
                    old=plan['snapshot'][name]
                    if target.is_file():shutil.copy2(target,saved)
                    row={'path':name,'previous':old,'installed':None};changes.append(row)
                    storage.atomic(run/'transaction.json',storage.json_bytes(journal))
                    if name in plan['retired']:target.unlink()
                    else:
                        target.parent.mkdir(parents=True,exist_ok=True);row['installed']=file_hash(updates.child(stage,name))
                        storage.atomic(run/'transaction.json',storage.json_bytes(journal))
                        temporary=target.with_name(target.name+'.'+run.name+'.tmp')
                        try:
                            shutil.copyfile(updates.child(stage,name),temporary);os.replace(temporary,target)
                        finally:temporary.unlink(missing_ok=True)
                different=[n for n,e in plan['effective'].items()if not matches(updates.child(root,n),e)]
                if different:raise ValueError('完整客户端读回失败：'+', '.join(different[:8]))
                receipt={'schema_version':1,'source_id':plan['source_id'],'commit':plan['commit'],'mirror':plan['mirror'],
                         'files':plan['entries'],'effective_files':plan['effective'],'personal_state_preserved':True,'data_migrations':False,'backup':str(backup)}
                storage.atomic(root/RECEIPT,storage.json_bytes(receipt));journal['status']='complete'
                storage.atomic(run/'transaction.json',storage.json_bytes(journal))
                return {'status':'installed','commit':plan['commit'],'verified_files':len(plan['entries']),'data_migrations':False}
            except BaseException:
                failures=[]
                for row in reversed(changes):
                    path=updates.child(root,row['path']);saved=updates.child(backup,row['path'])
                    try:
                        current=file_hash(path)if path.is_file()else None
                        if current==row['previous']:continue
                        if current!=row['installed']:raise ValueError('文件被外部修改')
                        if row['previous']:
                            if file_hash(saved)!=row['previous']:raise ValueError('恢复文件已变化')
                            os.replace(saved,path)
                        elif path.exists():path.unlink()
                    except (OSError,ValueError):failures.append(row['path'])
                journal.update(status='recovery_required'if failures else'rolled_back',recovery_required=failures)
                storage.atomic(run/'transaction.json',storage.json_bytes(journal));raise

    def unresolved(self,root):
        return [p for p in(self.cache/'runs').glob('*/transaction.json')
                if (v:=storage.read_json(p,{})).get('host')==str(Path(root).resolve())and v.get('status')in('working','recovery_required')]

    def recover(self,host,helper=False):
        root=Path(host['path']).resolve();pending=self.unresolved(root)
        if updates.running(root/'GOnline.exe'):return {'status':'waiting','message':'关闭客户端后恢复未完成更新'}
        for journal_path in pending:
            value=storage.read_json(journal_path);run=journal_path.parent
            own=getattr(getattr(self.workspace,'updates',None),'manager_exe',None)
            if not helper and own and Path(own).resolve()==root/updates.MANAGER and any(r['path']==updates.MANAGER for r in value['changes']):
                handoff=validate_handoff(run/'handoff.json');handoff['action']='recover'
                storage.atomic(run/'handoff.json',storage.json_bytes(handoff))
                return {'status':'restart_required','message':'重启 Manager 恢复未完成更新','handoff':str(run/'handoff.json')}
            with lease('host:'+str(root).casefold()),ExitStack()as barrier:
                from .operations import unmapped_image
                if(root/'GOnline.exe').is_file():barrier.enter_context(unmapped_image(root/'GOnline.exe',replace=True))
                for row in reversed(value['changes']):
                    path=updates.child(root,row['path']);saved=updates.child(run/'recovery',row['path'])
                    current=file_hash(path)if path.is_file()else None
                    if current==row['previous']:continue
                    if current!=row['installed']:raise storage.Conflict('恢复目标被外部修改，保留原件：'+row['path'])
                    if row['previous']:
                        if file_hash(saved)!=row['previous']:raise ValueError('恢复副本校验失败：'+row['path'])
                        path.parent.mkdir(parents=True,exist_ok=True);temporary=path.with_name(path.name+'.recover.tmp')
                        try:shutil.copyfile(saved,temporary);os.replace(temporary,path)
                        finally:temporary.unlink(missing_ok=True)
                    elif path.exists():path.unlink()
                value['status']='rolled_back';storage.atomic(journal_path,storage.json_bytes(value))
        self.plans.pop(host['id'],None)
        return {'status':'recovered','message':'已恢复未完成更新的原文件，请重新检查更新'}

    def handoff(self,host,plan,run,stage):
        # The helper waits for this Manager to close; no installed executable is
        # changed while it is running. The normal API exposes an explicit restart.
        value={'schema_version':1,'workspace':str(self.workspace.root),'host':host,'plan':plan,
               'run':str(run),'stage':str(stage),'parent_pid':os.getpid()}
        storage.atomic(run/'handoff.json',storage.json_bytes(value))
        return {'status':'restart_required','message':'完整包已校验；重启 Manager 完成安装','handoff':str(run/'handoff.json')}

    def restart(self,identity):
        result=self.results.get(identity,{})
        if result.get('status')!='restart_required':raise ValueError('没有等待完成的完整客户端更新')
        job=Path(result['handoff']);value=validate_handoff(job)
        if value['parent_pid']!=os.getpid():raise ValueError('更新交接属于其他 Manager 会话')
        helper=Path(value['run'])/'helper'/updates.MANAGER;helper.parent.mkdir(exist_ok=True)
        shutil.copy2(Path(value['stage'])/updates.MANAGER,helper)
        if not matches(helper,value['plan']['entries'][updates.MANAGER]):raise ValueError('更新助手校验失败')
        subprocess.Popen([str(helper),'--finish-client-update',str(job)],cwd=helper.parent,
                         creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),close_fds=True)

    def request(self,identity,action,digest=None):
        if action not in('check','install','recover'):raise ValueError('客户端更新操作无效')
        self.workspace.host(identity)
        with self.lock:self.jobs.append((identity,action,digest));self.results[identity]={'status':'queued'}
        self.start();return self.state()

    def state(self):
        rows=[]
        for host in self.workspace.settings['hosts']:
            if host['role']!='client':continue
            try:binding=installed_source(host['path']);error=''
            except (ValueError,OSError)as e:binding=None;error=str(e)
            rows.append({'id':host['id'],'name':host['name'],'source':binding,'error':error,'result':self.results.get(host['id'],{}),
                         'recovery_required':bool(self.unresolved(host['path']))})
        return {'healthy':True,'running':bool(self.thread and self.thread.is_alive()),'hosts':rows,
                'automatic':self.workspace.services.desired(SERVICE)if hasattr(self.workspace,'services')else False}

    def start(self):
        if self.thread and self.thread.is_alive():return
        self.stop_event.clear()
        def loop():
            next_check=0
            while not self.stop_event.is_set():
                with self.lock:job=self.jobs.pop(0)if self.jobs else None
                if job is None and self.workspace.services.desired(SERVICE)and time.monotonic()>=next_check:
                    next_check=time.monotonic()+3600
                    from .owner_channel import mode
                    for host in self.workspace.settings['hosts']:
                        if host['role']=='client'and mode(host)!='owner'and(Path(host['path'])/SOURCE).is_file():
                            with self.lock:self.jobs.append((host['id'],'sync',None))
                    continue
                if job is None:
                    for identity,result in list(self.results.items()):
                        if result.get('status')=='waiting'and identity in self.plans:
                            host=self.workspace.host(identity)
                            if not updates.running(Path(host['path'])/'GOnline.exe'):
                                job=(identity,'install',self.plans[identity]['digest']);break
                if job:
                    identity,action,digest=job
                    with self.busy:
                        with self.lock:self.results[identity]={'status':'working'}
                        try:
                            host=self.workspace.host(identity)
                            result=self.recover(host)if action=='recover'else self.plan(host)if action in('check','sync')else self.install(host,digest)
                            if action=='sync'and result['status']=='available':result=self.install(host,result['digest'])
                        except Exception as e:result={'status':'error','message':str(e)}
                        with self.lock:self.results[identity]=result
                self.stop_event.wait(.25)
        self.thread=threading.Thread(target=loop,name='client-runtime-updates',daemon=True);self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:self.thread.join(timeout=25)


def attach(workspace):
    controller=ClientRuntime(workspace);workspace.client_runtime=controller
    workspace.services.register(SERVICE,'完整客户端更新',controller,controller.start)
    if SERVICE not in workspace.settings.get('services',{}):
        workspace.settings.setdefault('services',{})[SERVICE]={'enabled':True};workspace.save_workspace()
    return controller


def validate_handoff(job):
    job=Path(job).resolve();value=storage.read_json(job);workspace=Path(value['workspace']).resolve()
    run=Path(value['run']).resolve();stage=Path(value['stage']).resolve();plan=value['plan'];host=value['host']
    if(value.get('schema_version')!=1 or run.parent!=workspace/'client-updates/runs'or not re.fullmatch('[0-9a-f]{32}',run.name)or
       job!=run/'handoff.json'or stage!=run/'payload'or host.get('role')!='client'):
        raise ValueError('完整客户端更新交接无效')
    updates.child(workspace,job.relative_to(workspace).as_posix())
    expected=plan.get('digest');body={k:v for k,v in plan.items()if k!='digest'}
    if updates.digest(storage.json_bytes(body))!=expected:raise ValueError('完整客户端更新计划已改变')
    root=Path(host['path']).resolve()
    if root==root.parent or installed_source(root)!=plan['binding']:raise ValueError('完整客户端更新目标已改变')
    if not matches(stage/updates.MANAGER,plan['entries'][updates.MANAGER]):raise ValueError('Manager 暂存文件已改变')
    return value


def finish(job,launch=True):
    value=validate_handoff(job)
    if os.name=='nt':
        import ctypes
        from ctypes import wintypes as w
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD);kernel.OpenProcess.restype=w.HANDLE
        kernel.WaitForSingleObject.argtypes=(w.HANDLE,w.DWORD);kernel.CloseHandle.argtypes=(w.HANDLE,)
        process=kernel.OpenProcess(0x100000,False,int(value['parent_pid']))
        if process:
            try:
                if kernel.WaitForSingleObject(process,120000)!=0:raise ValueError('请正常退出旧 Manager 后再更新')
            finally:kernel.CloseHandle(process)
    from .workspace import Workspace
    workspace=Workspace(Path(value['workspace']),Path(__file__).parent/'addons')
    try:
        controller=ClientRuntime(workspace)
        result=controller.recover(value['host'],helper=True)if value.get('action')=='recover'else controller.commit(value['host'],value['plan'],Path(value['run']),Path(value['stage']))
        storage.atomic(Path(value['run'])/'result.json',storage.json_bytes(result))
    finally:workspace.close()
    if result['status']in('installed','recovered'):
        Path(job).unlink()
        if launch:subprocess.Popen([str(Path(value['host']['path'])/updates.MANAGER),'--workspace',value['workspace']],
            cwd=value['host']['path'],creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),close_fds=True)
    return result
