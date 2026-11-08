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
FIXED={'sdgo/core.dll','sdgo/sdgo.ini','sdgo/catalog.json','player-package.json','sdgo/shared-settings.json',SOURCE}
HASH=re.compile('[0-9a-f]{64}');OID=re.compile('[0-9a-f]{40}')


def source(value):
    if isinstance(value,dict)and value.get('schema_version')==3:
        from .server_profiles import binding
        return binding(value)
    if(not isinstance(value,dict)or value.get('schema_version')not in(1,2) or value.get('kind')!='sdgo-client-runtime'or
       not re.fullmatch('[0-9a-f]{32}',str(value.get('source_id','')))):
        raise ValueError('完整客户端更新源身份无效')
    feeds=value.get('feeds');label=value.get('label')
    if not isinstance(feeds,list)or not 1<=len(feeds)<=3 or len(set(feeds))!=len(feeds):raise ValueError('配置一至三个不同的客户端 Git 源')
    for url in feeds:git_source.parse(url)
    if not isinstance(label,str)or not label.strip()or len(label)>80:raise ValueError('客户端更新源名称无效')
    result={'schema_version':value['schema_version'],'kind':'sdgo-client-runtime','source_id':value['source_id'],'label':label.strip(),'feeds':feeds}
    if value['schema_version']==2:
        from .framework_source import binding
        result['framework']=binding(value.get('framework'))
    return result


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
    if name in contract.get('documentation',[]):return True
    if '/'not in name:return Path(name).suffix.casefold()in contract['root_extensions']or name in contract['entrypoints']
    return any(name.startswith(tree+'/')for tree in contract['trees'])


def validate_contract(manifest,binding):
    c=manifest.get('runtime',{})
    if c.get('schema_version')!=1 or c.get('source_id')!=binding['source_id']:raise ValueError('客户端清单与所选发布者不一致')
    if not isinstance(c.get('root_extensions'),list)or not set(c['root_extensions'])<=EXTENSIONS:raise ValueError('客户端文件范围无效')
    if not isinstance(c.get('trees'),list)or not set(c['trees'])<=TREES:raise ValueError('客户端资源范围无效')
    if c.get('entrypoints')not in([],['SDGO Mod Manager.exe']):raise ValueError('客户端更新必须由 Manager 统一管理')
    documentation=c.get('documentation',[])
    if not isinstance(documentation,list)or len(documentation)>12:raise ValueError('客户端说明文件清单无效')
    for name in documentation:
        updates.safe_name(name)
        if '/'in name or not name.casefold().endswith('.md'):raise ValueError('客户端说明须为根目录 Markdown 文件')
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
        self.fetch_mirrors={};self.fetch_mirror={}
        self.active_host=None
        self.cancel_event=threading.Event()
        class OperationStop:
            def is_set(token):return self.stop_event.is_set()or self.cancel_event.is_set()
        self.operation_stop=OperationStop()
        self.binding_requests={}
        self.launch_tokens={}

    def launch(self,root,token):
        from . import launch
        root=Path(root).resolve()
        host=next((h for h in self.workspace.settings['hosts']if Path(h['path']).resolve()==root and h['role']=='client'),None)
        if host is None:
            self.workspace.add_host(str(root))
            host=next(h for h in self.workspace.settings['hosts']if Path(h['path']).resolve()==root and h['role']=='client')
        completed=launch.state(root,token)
        if completed.get('status')in('launched','cancelled'):return completed
        launch.accept(root,token)
        with self.lock:
            existing=self.launch_tokens.get(host['id'])
            if existing and self.results.get(host['id'],{}).get('status')in('queued','working','restart_required'):
                return {'token':existing,'host':host['id'],'status':'queued'}
            self.launch_tokens[host['id']]=token
            self.jobs=[j for j in self.jobs if j[0]!=host['id']]
            self.jobs.insert(0,(host['id'],'launch',token))
            self.results[host['id']]={'status':'queued','launch_token':token}
        self.start();return {'token':token,'host':host['id'],'status':'queued'}

    def launch_state(self,root,token):
        from . import launch
        root=Path(root).resolve()
        with self.lock:
            for h in self.workspace.settings['hosts']:
                result=self.results.get(h['id'],{})
                if Path(h['path']).resolve()==root and result.get('launch_token')==token:return {**result,'host':h['id'],'token':token}
        return {**launch.state(root,token),'token':token}

    def report_progress(self,**values):
        if self.active_host:
            with self.lock:
                row=self.results.setdefault(self.active_host,{'status':'working'})
                if values.get('phase')!=row.get('progress',{}).get('phase')and 'phase'in values:row['progress']={}
                row.setdefault('progress',{}).update(values)

    def git_progress(self,raw):
        rows=re.findall(rb', ([0-9.]+) (bytes|KiB|MiB|GiB)',raw)
        if rows:
            amount,unit=rows[-1];scale={b'bytes':1,b'KiB':1024,b'MiB':1024**2,b'GiB':1024**3}[unit]
            self.git_received=max(getattr(self,'git_received',0),int(float(amount)*scale))
            self.report_progress(wire_bytes=self.git_received)

    def bind_profile(self,identity,urls,offline=None):
        self.workspace.host(identity)
        if not self.busy.acquire(blocking=False):raise storage.Conflict('另一个更新正在处理，请等待完成或取消后再导入')
        self.binding_requests[identity]={'feeds':urls,'offline':offline}
        with self.lock:self.active_host=identity;self.cancel_event.clear();self.results[identity]={'status':'working','progress':{'phase':'manifest'}}
        try:
            self._bind_profile(identity,urls,offline)
            if self.operation_stop.is_set():raise InterruptedError('配置导入已取消')
        except InterruptedError:
            self.workspace.host(identity)['client_updates_paused']=True;self.workspace.save_workspace()
            with self.lock:self.results[identity]={'status':'cancelled','message':'已取消；尚未安装运行文件，可继续导入'}
            return self.state()
        except Exception as error:
            with self.lock:self.results[identity]={'status':'error','message':str(error)}
            raise
        finally:
            with self.lock:self.active_host=None;self.cancel_event.clear()
            self.busy.release()
        self.binding_requests.pop(identity,None)
        with self.lock:self.jobs.append((identity,'sync',None));self.results[identity]={'status':'queued'}
        self.start();return self.state()

    def _bind_profile(self,identity,urls,offline=None):
        from . import server_profiles,framework_source
        from .owner_channel import mode
        host=self.workspace.host(identity);root=Path(host['path']).resolve()
        if host['role']!='client'or mode(host)=='owner':raise ValueError('在玩家客户端安装服主配置；服主编辑目录不会被覆盖')
        if offline:urls=['local+'+Path(offline).expanduser().resolve().as_uri()]
        if not isinstance(urls,list)or not 1<=len(urls)<=3:raise ValueError('填写服主配置地址或选择离线配置文件')
        errors=[]
        for url in urls:
            try:
                url=server_profiles.feed(url);raw,_=server_profiles.read(self,url,server_profiles.MANIFEST,16*1024*1024)
                profile=json.loads(raw)
                if profile.get('kind')=='sdgo-client-runtime':
                    imported=server_profiles.binding(profile)
                    profile,_,_=server_profiles.resolve(self,imported);urls=imported['feeds']
                    profile={**profile,'framework_source':imported['framework']}
                framework=profile.get('framework_source')or{'schema_version':1,'kind':'sdgo-framework-runtime',
                    'source_id':updates.digest(('sdgo-framework-runtime:'+updates.REPOSITORY).encode())[:32],
                    'repository':updates.REPOSITORY,'channel':'stable','transport':'github-releases'}
                source=server_profiles.binding({'schema_version':3,'kind':'sdgo-client-runtime','source_id':profile.get('source_id'),
                    'label':profile.get('label'),'feeds':urls,'framework':framework,
                    **({'manager_policy':'owner','channel':profile['channel']}if profile.get('schema_version')==2 else{})})
                server_profiles.validate(profile,source);server_profiles.base_check(root,profile)
                old=installed_source(root)
                if old and old['source_id']!=source['source_id']:raise ValueError('先明确解除原服主绑定，再导入另一服主配置')
                if self.operation_stop.is_set():raise InterruptedError('配置导入已取消')
                with lease('host:'+str(root).casefold()):
                    marker=root/SOURCE
                    if marker.is_file():storage.atomic(self.cache/'binding-backups'/uuid.uuid4().hex/'source.json',marker.read_bytes())
                    storage.atomic(marker,storage.json_bytes(source));host['runtime_mode']='player';host['client_updates_paused']=False;self.workspace.save_workspace()
                return source
            except InterruptedError:raise
            except(OSError,ValueError,subprocess.SubprocessError)as error:errors.append(str(error))
        raise ValueError('无法导入服主配置：'+'；'.join(errors))

    def publish_profile(self,identity,directory,feeds,extras=None,zip_payload=False,git_remote='',branch='content'):
        host=self.workspace.host(identity)
        with self.busy,lease('host:'+str(Path(host['path']).resolve()).casefold()):
            return self._publish_profile(identity,directory,feeds,extras,zip_payload,git_remote,branch)

    def _publish_profile(self,identity,directory,feeds,extras,zip_payload,git_remote,branch):
        from . import server_profiles,profile_publish,framework_source
        from .owner_channel import mode
        host=self.workspace.host(identity)
        if host['role']!='client'or mode(host)!='owner':raise ValueError('服主在自己的编辑客户端发布配置档')
        output=Path(directory).expanduser().resolve()
        base=Path(host['path']).resolve()
        if not directory or not Path(directory).is_absolute()or output==output.parent or output.is_relative_to(base)or base.is_relative_to(output):raise ValueError('配置档发布目录须为客户端之外的绝对路径')
        updater=self.workspace.updates
        publication=host.setdefault('profile_publication',{})
        publication.setdefault('source_id',host.get('publication',{}).get('publisher_id')or uuid.uuid4().hex)
        prior=storage.read_json(output/server_profiles.MANIFEST,{})
        installed=storage.read_json(base/'sdgo/release.json',{})
        if not installed.get('version'):installed=storage.read_json(base/RECEIPT,{}).get('framework',{})
        # Content publication keeps the accepted programme choice. A later
        # upstream release or a Manager restart does not upgrade that choice.
        reuse=bool(prior.get('source_id')==publication['source_id']and prior.get('framework',{}).get('version')==installed.get('version')and installed.get('version'))
        if not reuse and not updater.latest:raise ValueError('先检查并选择官方框架版本，再发布服主配置档')
        configured=self.workspace.settings.get('updates',{}).get('git_source')
        source_framework=configured or(prior.get('framework_source')if reuse else None)or{'schema_version':1,'kind':'sdgo-framework-runtime',
            'source_id':updates.digest(('sdgo-framework-runtime:'+updater.source_repository()).encode())[:32],
            'repository':updater.source_repository(),'channel':updater.latest['channel'],'transport':'github-releases'}
        lock=prior['framework']if reuse else{'version':updater.latest['version'],'sha256':updater.latest['sha256']}
        release=framework_source.load(self,source_framework,{'minimum_framework':lock['version'],'locked_framework':lock}if reuse or not configured else None)
        if publication.get('programme_framework'):
            source_framework=publication['programme_framework_source'];lock=publication['programme_framework']
            release=framework_source.load(self,source_framework,{'minimum_framework':lock['version'],'locked_framework':lock})
        source=server_profiles.binding({'schema_version':3,'kind':'sdgo-client-runtime','source_id':publication['source_id'],
            'label':host['name'],'feeds':feeds,'framework':source_framework,
            **({'manager_policy':'owner','channel':publication['owner_programme']['channel']}if publication.get('owner_programme')else{})})
        if git_remote:
            from .git_source import parse
            parse(('git+ssh://'if git_remote.startswith('ssh://')else'git+')+git_remote.removeprefix('ssh://')+'#'+branch)
            output.mkdir(parents=True,exist_ok=True)
            self.workspace.owner_channels.git_prepare(output,{'branch':branch,'git_remote':git_remote},git=self.git)
            self.git(output,['sparse-checkout','set','content','releases'])
        result=profile_publish.publish(host,source,release,output,extras or[],publication.get('personal_settings',[]),zip_payload,publication.get('owner_programme'))
        publication.update(directory=str(output),feeds=feeds,revision=result['revision'],git_remote=git_remote,branch=branch,extras=extras or[]);self.workspace.save_workspace()
        if git_remote:
            profile_publish.git_stage(self,output,storage.read_json(output/server_profiles.MANIFEST),zip_payload)
            if self.git(output,['diff','--cached','--name-only']).strip():self.git(output,['commit','-m','Publish owner content revision '+str(result['revision'])])
            self.git(output,['push','origin','HEAD:refs/heads/'+branch])
            expected=self.git(output,['rev-parse','HEAD']).decode().strip()
            actual=self.git(output,['ls-remote','origin','refs/heads/'+branch]).decode().split()
            if actual[:1]!=[expected]:raise ValueError('内容已生成，但远程发布提交尚未核验；重试发布会复用当前修订')
            result.update(status='published',commit=expected)
        return result

    def publish_programmes(self,identity,directory,feeds,selected,channel='beta',git_remote='',branch='owner-runtime/beta'):
        from . import owner_programmes,framework_source
        from .owner_channel import mode
        host=self.workspace.host(identity)
        if host['role']!='client'or mode(host)!='owner':raise ValueError('只在维护者客户端发布本服程序')
        if not directory or not Path(directory).is_absolute():raise ValueError('选择客户端之外的程序发布目录')
        if not self.busy.acquire(blocking=False):raise ValueError('正在执行更新或发布，请稍后再试')
        try:
            publication=host.setdefault('profile_publication',{})
            publication.setdefault('source_id',host.get('publication',{}).get('publisher_id')or uuid.uuid4().hex)
            updater=self.workspace.updates
            if not updater.latest:raise ValueError('先在 Manager 检查并选择官方基线版本')
            source=self.workspace.settings.get('updates',{}).get('git_source')or{'schema_version':1,'kind':'sdgo-framework-runtime',
                'source_id':updates.digest(('sdgo-framework-runtime:'+updater.source_repository()).encode())[:32],
                'repository':updater.source_repository(),'channel':updater.latest['channel'],'transport':'github-releases'}
            framework=framework_source.load(self,source,{'minimum_framework':updater.latest['version'],
                'locked_framework':{k:updater.latest[k]for k in('version','sha256')}})
            if framework['manifest'].get('capabilities',{}).get('owner_programmes')!=1:raise ValueError('选择支持独立本服程序的官方基线')
            output=Path(directory).resolve()
            client=Path(host['path']).resolve()
            if output==output.parent or output.is_relative_to(client)or client.is_relative_to(output):raise ValueError('程序发布目录须与客户端分开')
            output.mkdir(parents=True,exist_ok=True)
            if git_remote:
                from .git_source import parse
                parse(('git+ssh://'if git_remote.startswith('ssh://')else'git+')+git_remote.removeprefix('ssh://')+'#'+branch)
                self.workspace.owner_channels.git_prepare(output,{'branch':branch,'git_remote':git_remote},git=self.git)
                self.git(output,['sparse-checkout','set','programmes','revisions'])
            lock={k:framework[k]for k in('version','sha256','commit')}
            result=owner_programmes.publish(host,{'source_id':publication['source_id'],'channel':channel,'feeds':feeds},lock,output,selected)
            if git_remote:
                attributes=output/'.gitattributes';rules=b'owner-programme.json -text -filter -ident\nprogrammes/** -text -filter -ident\nrevisions/** -text -filter -ident\n'
                original=attributes.read_bytes()if attributes.is_file()else b''
                if rules not in original:storage.atomic(attributes,original+b'\n'+rules)
                self.git(output,['add','--','.gitattributes',owner_programmes.MANIFEST])
                if (output/'programmes').exists():self.git(output,['add','--','programmes'])
                self.git(output,['add','--','revisions'])
                if self.git(output,['diff','--cached','--name-only']).strip():self.git(output,['commit','-m','Publish owner programme revision '+str(result['revision'])])
                result['commit']=self.git(output,['rev-parse','HEAD']).decode().strip()
                self.git(output,['push','origin','HEAD:refs/heads/'+branch])
                if self.git(output,['ls-remote','origin','refs/heads/'+branch]).decode().split()[:1]!=[result['commit']]:raise ValueError('本服程序远程提交未核验')
            # Re-read the actual published artifact with the player resolver.
            owner_programmes.load(self,result,lock)
            publication.update(owner_programme=result,programme_framework=lock,programme_framework_source=source,
                programme_directory=str(output),programme_git_remote=git_remote,programme_branch=branch,programme_mods=selected,programme_feeds=feeds)
            self.workspace.save_workspace()
            return {'revision':result['revision'],'mods':selected,'framework':lock['version'],'source':result}
        finally:self.busy.release()

    def git(self,repository,args,input=None,output=None):
        from .owned_process import run
        fetching=args[:1]==['fetch'];network=args[:1]in(['fetch'],['push'],['ls-remote']);key=str(Path(repository).resolve())
        prefix=['-c','fetch.negotiationAlgorithm=noop']if'--stdin'in args else[]
        if network:prefix+=['-c','http.lowSpeedLimit=0']
        if fetching:
            if '--progress'not in args:args=[args[0],'--progress',*args[1:]]
        attempts=[(None,args)]
        if fetching and 'origin'in args:
            for index,remote in self.fetch_mirrors.get(key,[]):
                alternate=list(args);alternate[alternate.index('origin')]=remote;attempts.append((index,alternate))
        errors=[]
        for index,arguments in attempts:
            pack=Path(repository)/'objects/pack'
            before={p.name:p.stat().st_size for p in pack.glob('*')if p.is_file()}if fetching else{}
            if fetching:self.git_received=0;self.report_progress(wire_bytes=0)
            def poll_pack():
                try:
                    received=sum(max(0,p.stat().st_size-before.get(p.name,0))for p in pack.glob('*')if p.is_file()and(p.suffix=='.pack'or p.name.startswith('tmp_pack')))
                    if received>self.git_received:
                        self.git_received=received;self.report_progress(wire_bytes=received)
                except OSError:pass  # A pack can be atomically renamed while sampled.
            try:
                result=run(
                    ['git','-C',str(repository),*prefix,*arguments],cwd=repository,timeout=None if network else 1200 if output is not None or args[:1]==['cat-file']else 120,
                    stop=self.operation_stop,env=git_source.environment(),input=input,output=output,**({'progress':self.git_progress,'poll':poll_pack}if fetching else{}))
            except subprocess.TimeoutExpired:
                errors.append('读取更新仓库超时');continue
            if result.returncode:
                errors.append(git_source.failure(result.stderr));continue
            if index is not None:self.fetch_mirror[key]=index
            return result.stdout
        raise ValueError('；'.join(errors))

    @staticmethod
    def move_image(source,target,run):
        # Windows cannot replace an existing name while its image barrier is
        # open. Move that still-locked inode aside, then expose the locked new
        # inode at the now-absent name. Neither executable can start in between.
        previous=None
        if target.exists():
            if not target.is_file():raise storage.Conflict('程序目标不是普通文件：'+target.name)
            previous=run/'pinned-images'/(uuid.uuid4().hex+'-'+target.name)
            previous.parent.mkdir(parents=True,exist_ok=True);os.rename(target,previous)
        try:os.rename(source,target)
        except BaseException:
            if previous is not None:os.rename(previous,target)
            raise

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
                incoming=source(json.loads(self.git(repo,['show',commit+':'+SOURCE])))
                if incoming!=binding:raise ValueError('这个镜像的来源绑定尚未同步，继续检查同身份备用源')
                key=str(repo.resolve());self.fetch_mirror[key]=index+1
                self.fetch_mirrors[key]=[(i+1,git_source.parse(value)[0])for i,value in enumerate(binding['feeds'])if i>index]
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
        if binding['schema_version']==3:
            from .server_profiles import plan
            return plan(self,host,binding)
        from .owner_channel import installed_binding
        old_binding=installed_binding(root)
        if old_binding and old_binding['publisher_id']!=binding['source_id']:
            raise ValueError('原负责人和完整客户端来源不同，请先审阅来源迁移，保留现有内容')
        repo,commit,manifest,contract,index=self.repository(binding);excluded=self.local_modules(root);entries={};case=set()
        unknown=[]
        # Asking ls-tree for sizes (-l) makes a partial clone fetch every blob,
        # including historical release archives outside the runtime scope.
        for raw in self.git(repo,['ls-tree','-r','-z',commit]).split(b'\0'):
            if not raw:continue
            header,name=raw.split(b'\t',1);name=name.decode('utf-8')
            if name=='config.xml':
                if (root/name).is_file():continue
            elif not runtime_path(name,contract):continue
            if any(name==p or name.startswith(p+'/')for p in excluded):raise ValueError('上游与仅本地 Mod 冲突：'+name)
            match=re.fullmatch(rb'100(?:644|755) blob ([a-f0-9]{40})',header)
            if not match or name.casefold()in case:raise ValueError('客户端清单包含链接或重复路径')
            oid=match[1].decode();path=updates.child(root,name)
            entries[name]={'git_blob':oid};case.add(name.casefold())
            if path.is_file()and path.stat().st_size<=512*1024*1024 and file_hash(path,git=True)==oid:
                entries[name]['bytes']=path.stat().st_size
            else:unknown.append(name)
        if unknown:
            requests=b''.join((entries[n]['git_blob']+'\n').encode()for n in unknown)
            # Enumerate local objects without causing promisor downloads.
            # Explicit noop fetches would otherwise retransmit cached blobs.
            cached={row.split()[0]for row in self.git(repo,['cat-file','--batch-all-objects','--batch-check','--unordered']).splitlines()if row}
            missing=b''.join((entries[n]['git_blob']+'\n').encode()for n in unknown if entries[n]['git_blob'].encode()not in cached)
            # Fetch all wanted runtime objects in one transport operation.
            # Otherwise a promisor cat-file can authenticate/fetch once per file.
            if missing:self.git(repo,['fetch','--no-tags','--no-write-fetch-head','--recurse-submodules=no','--filter=blob:none','--stdin','origin'],input=missing)
            rows=self.git(repo,['cat-file','--batch-check'],input=requests).splitlines()
            if len(rows)!=len(unknown):raise ValueError('运行文件对象信息不完整')
            for name,row in zip(unknown,rows):
                fields=row.split()
                if len(fields)!=3 or fields[0].decode()!=entries[name]['git_blob']or fields[1]!=b'blob':raise ValueError('运行文件对象信息无效')
                size=int(fields[2])
                if not 0<=size<=512*1024*1024:raise ValueError('客户端单文件超过限制')
                entries[name]['bytes']=size
        required={'GOnline.exe',SOURCE}if binding['schema_version']==2 else{'GOnline.exe',updates.MANAGER,'sdgo/core.dll',SOURCE}
        if not required<=entries.keys():raise ValueError('完整客户端清单缺少必要程序或更新源')
        incoming=source(json.loads(self.git(repo,['show',commit+':'+SOURCE])))
        if incoming!=binding:raise ValueError('上游试图改变客户端绑定；请在 Manager 明确选择新的来源')
        entries['GPackData.zpk']={k:contract['base'][k]for k in('sha256','bytes')}
        if not 4<=len(entries)<=100000 or sum(e['bytes']for e in entries.values())>16*1024**3:raise ValueError('客户端清单超过限制')
        previous=storage.read_json(root/RECEIPT,{})
        if previous and previous.get('source_id')!=binding['source_id']:raise ValueError('此目录已有不同来源的安装记录，请先审阅迁移')
        # Private/addon-only files remain outside the source-owned inventory.
        # Authored INI settings use the framework's existing preservation rules.
        layers={}
        if binding['schema_version']==2:
            from .runtime_layers import compose
            entries,effective,layers=compose(self,binding,contract,root,entries,repo,commit)
            preserved={};merged={}
        else:
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
        snapshot[RECEIPT]=file_hash(root/RECEIPT)if(root/RECEIPT).is_file()else None
        value={'source_id':binding['source_id'],'commit':commit,'entries':entries,'needed':needed,'retired':retired,
               'snapshot':snapshot,'binding':binding,'mirror':self.fetch_mirror.get(str(repo.resolve()),index),'contract':contract,'repository':str(repo),
               'effective':effective,'preserved':preserved,'merged':list(merged),**layers}
        value['digest']=updates.digest(storage.json_bytes(value));self.plans[host['id']]=value
        return {'status':'available'if needed or retired else'current','commit':commit,'source':binding['label'],
                'changed_files':len(needed),'retired_files':len(retired),'bytes':sum(entries[n]['bytes']for n in needed),'preserved_settings':len(preserved)+len(merged),
                'digest':value['digest'],'files':needed,'retired':retired,'data_migrations':False,
                'framework_version':layers.get('framework',{}).get('version'),'framework_commit':layers.get('framework',{}).get('commit'),
                'shared_configuration_files':len(layers.get('shared_files',[]))}

    def stage(self,plan,run,root):
        repo=Path(plan['repository']);commit=plan['commit'];stage=run/'payload';stage.mkdir(parents=True)
        needed=[n for n in plan['needed']if n!='GPackData.zpk'and(not plan.get('framework')or n in plan['content_files'])]
        if plan.get('profile'):
            from .server_profiles import stage as stage_profile
            stage_profile(plan,stage);needed=[]
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
                    if self.operation_stop.is_set():raise InterruptedError('客户端更新已取消')
                    segment=self.cache/'segments'/(part['sha256']+'.zip');segment.parent.mkdir(parents=True,exist_ok=True)
                    with lease('client-segment:'+str(segment.resolve())):
                        if not segment.is_file()or segment.stat().st_size!=part['bytes']or file_hash(segment)!=part['sha256']:
                            # Explicitly fetch the pinned blob so segment data
                            # uses the same mirror fallback as regular files.
                            oid=self.git(repo,['rev-parse',base['commit']+':'+part['path']]).decode().strip()
                            if not OID.fullmatch(oid):raise ValueError('归档分段对象身份无效')
                            self.git(repo,['fetch','--no-tags','--no-write-fetch-head','--recurse-submodules=no','--filter=blob:none','--stdin','origin'],input=(oid+'\n').encode())
                            temporary=run/'segment.download';self.git(repo,['cat-file','blob',oid],output=temporary)
                            if temporary.stat().st_size!=part['bytes']or file_hash(temporary)!=part['sha256']:raise ValueError('归档分段校验失败')
                            os.replace(temporary,segment)
                    with zipfile.ZipFile(segment)as z:
                        if z.namelist()!=[part['entry']]or z.getinfo(part['entry']).file_size!=part['unpacked_bytes']:raise ValueError('归档分段解包清单无效')
                        with z.open(part['entry'])as src:shutil.copyfileobj(src,output,1024*1024)
        if plan.get('framework'):
            from .runtime_layers import stage as stage_framework
            stage_framework(plan,stage)
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
        if plan.get('framework'):
            from .runtime_layers import render
            render(plan,stage)
            for name in plan['needed']:
                if not matches(updates.child(stage,name),plan['effective'][name]):raise ValueError('双源配置校验失败：'+name)
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
            self.report_progress(phase='staging',total_files=len(plan['needed']))
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
            executables=[root/'GOnline.exe']
            if updates.MANAGER in plan['needed']:executables.append(root/updates.MANAGER)
            for executable in executables:
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
            self.report_progress(phase='install',total_files=len(plan['needed'])+len(plan['retired']),verified_files=0)
            try:
                for name in [*plan['retired'],*sorted(plan['needed'],key=lambda n:n==updates.MANAGER)]:
                    if self.operation_stop.is_set():raise InterruptedError('客户端更新已取消')
                    target=updates.child(root,name);saved=updates.child(backup,name);saved.parent.mkdir(parents=True,exist_ok=True)
                    old=plan['snapshot'][name]
                    if(file_hash(target)if target.is_file()else None)!=old:raise storage.Conflict('文件已被外部修改：'+name)
                    if target.is_file():
                        shutil.copy2(target,saved)
                        if file_hash(saved)!=old:raise storage.Conflict('备份期间文件已改变：'+name)
                    row={'path':name,'previous':old,'installed':None};changes.append(row)
                    self.report_progress(verified_files=len(changes),file=name)
                    storage.atomic(run/'transaction.json',storage.json_bytes(journal))
                    if name in plan['retired']:target.unlink()
                    else:
                        target.parent.mkdir(parents=True,exist_ok=True);row['installed']=file_hash(updates.child(stage,name))
                        storage.atomic(run/'transaction.json',storage.json_bytes(journal))
                        temporary=target.with_name(target.name+'.'+run.name+'.tmp')
                        try:
                            shutil.copyfile(updates.child(stage,name),temporary)
                            if name in('GOnline.exe',updates.MANAGER):barrier.enter_context(unmapped_image(temporary,replace=True))
                            if name in('GOnline.exe',updates.MANAGER):self.move_image(temporary,target,run)
                            else:os.replace(temporary,target)
                        finally:temporary.unlink(missing_ok=True)
                different=[n for n,e in plan['effective'].items()if not matches(updates.child(root,n),e)]
                if different:raise ValueError('完整客户端读回失败：'+', '.join(different[:8]))
                receipt={'schema_version':1,'source_id':plan['source_id'],'commit':plan['commit'],'mirror':plan['mirror'],
                         'files':plan['entries'],'effective_files':plan['effective'],'personal_state_preserved':True,'data_migrations':False,'backup':str(backup),
                         'framework':{k:v for k,v in plan.get('framework',{}).items()if k not in('bundle','manifest')},
                         'content_commit':plan['commit'],'shared_files':plan.get('shared_files',[])}
                if plan.get('owner_programme'):receipt['owner_programme']=plan['owner_programme']['source']
                if plan.get('quarantined'):receipt['quarantined_files']=plan['quarantined']
                if plan.get('profile'):receipt['profile']=plan['profile']
                marker=root/RECEIPT;marker_before=file_hash(marker)if marker.is_file()else None
                if marker.is_file():
                    saved=updates.child(backup,RECEIPT);saved.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(marker,saved)
                raw=storage.json_bytes(receipt)
                changes.append({'path':RECEIPT,'previous':marker_before,'installed':updates.digest(raw)})
                storage.atomic(run/'transaction.json',storage.json_bytes(journal))
                storage.atomic(marker,raw);journal['status']='complete'
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
                            if row['path']in('GOnline.exe',updates.MANAGER):barrier.enter_context(unmapped_image(saved,replace=True))
                            if row['path']in('GOnline.exe',updates.MANAGER):self.move_image(saved,path,run)
                            else:os.replace(saved,path)
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
                if any(r['path']==updates.MANAGER for r in value['changes'])and(root/updates.MANAGER).is_file():
                    barrier.enter_context(unmapped_image(root/updates.MANAGER,replace=True))
                for row in reversed(value['changes']):
                    path=updates.child(root,row['path']);saved=updates.child(run/'recovery',row['path'])
                    current=file_hash(path)if path.is_file()else None
                    if current==row['previous']:continue
                    if current!=row['installed']:raise storage.Conflict('恢复目标被外部修改，保留原件：'+row['path'])
                    if row['previous']:
                        if file_hash(saved)!=row['previous']:raise ValueError('恢复副本校验失败：'+row['path'])
                        path.parent.mkdir(parents=True,exist_ok=True);temporary=path.with_name(path.name+'.recover.tmp')
                        try:
                            shutil.copyfile(saved,temporary)
                            if row['path']in('GOnline.exe',updates.MANAGER):barrier.enter_context(unmapped_image(temporary,replace=True))
                            if row['path']in('GOnline.exe',updates.MANAGER):self.move_image(temporary,path,run)
                            else:os.replace(temporary,path)
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
        if action not in('check','install','recover','sync'):raise ValueError('客户端更新操作无效')
        with self.lock:
            result=self.results.get(identity,{})
            if result.get('launch_token')and result.get('status')in('queued','working','restart_required'):
                raise storage.Conflict('正在准备启动游戏，请等待或在启动窗口取消后再手动更新')
        self.workspace.host(identity)['client_updates_paused']=False
        self.workspace.save_workspace()
        with self.lock:self.jobs.append((identity,action,digest));self.results[identity]={'status':'queued'}
        self.start();return self.state()

    def cancel(self,identity):
        host=self.workspace.host(identity);host['client_updates_paused']=True;self.workspace.save_workspace()
        with self.lock:
            self.jobs=[job for job in self.jobs if job[0]!=identity]
            if self.active_host==identity:self.cancel_event.set()
            else:
                self.results[identity]={'status':'cancelled','message':'下载已取消；已校验文件保留，可继续下载'}
                token=self.launch_tokens.get(identity)
                if token:
                    from .launch import save
                    self.results[identity]['launch_token']=token
                    save(host['path'],token,**self.results[identity])
        return self.state()

    def state(self):
        rows=[]
        for host in self.workspace.settings['hosts']:
            if host['role']!='client':continue
            with self.lock:
                result=dict(self.results.get(host['id'],{}));plan=self.plans.get(host['id'])
                working=result.get('status')in('queued','working')
                try:binding=plan['binding']if working and plan else installed_source(host['path']);error=''
                except (ValueError,OSError)as e:binding=None;error=str(e)
                # Pair journal reads with the worker's transition to working.
                recovery=False if working else bool(self.unresolved(host['path']))
            from .owner_channel import mode
            rows.append({'id':host['id'],'name':host['name'],'mode':mode(host),'source':binding,'pending':self.binding_requests.get(host['id']),'error':error,'result':result,
                         'recovery_required':recovery})
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
                        if host['role']=='client'and mode(host)!='owner'and not host.get('client_updates_paused')and(Path(host['path'])/SOURCE).is_file():
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
                        with self.lock:self.active_host=identity;self.cancel_event.clear();self.results[identity]={'status':'working',**({'launch_token':digest}if action=='launch'else{})}
                        try:
                            host=self.workspace.host(identity)
                            if action=='launch':
                                from .launch import run
                                result=run(self,host,digest)
                            else:result=self.recover(host)if action=='recover'else self.plan(host)if action in('check','sync')else self.install(host,digest)
                            if action=='sync'and result['status']=='available':result=self.install(host,result['digest'])
                        except InterruptedError:result={'status':'cancelled','message':'操作已取消，已校验文件保留；缺失或不完整内容不会安装'}
                        except Exception as e:result={'status':'error','message':str(e)}
                        if action=='launch':
                            from .launch import save
                            result['launch_token']=digest
                            try:save(host['path'],digest,**{k:v for k,v in result.items()if k!='handoff'})
                            except(OSError,ValueError)as error:
                                result={**result,'status':'error','message':'无法保存启动结果：'+str(error)}
                        with self.lock:self.results[identity]=result;self.active_host=None;self.cancel_event.clear()
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
        if launch:subprocess.Popen([str(Path(value['host']['path'])/updates.MANAGER),'--workspace',value['workspace'],*(['--no-browser']if value.get('launch_request')else[])],
            cwd=value['host']['path'],creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),close_fds=True)
    return result
