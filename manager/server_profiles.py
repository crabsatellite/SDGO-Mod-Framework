"""Owner profiles: portable configuration/content, never a complete game copy."""
from __future__ import annotations
from pathlib import Path
from urllib.parse import urlsplit,unquote,urljoin,quote
import hashlib,json,re,shutil,subprocess,uuid,zipfile,os
from . import storage,updates,git_source,framework_source,runtime_layers

KIND='sdgo-server-profile'
MANIFEST='server-profile.json'
SHARED=runtime_layers.SHARED
CONTENT_PREFIX='content/'
HASH=re.compile('[0-9a-f]{64}')
ID=re.compile('[0-9a-f]{32}')
FORBIDDEN_SUFFIXES={'.exe','.dll','.asi','.sql','.mdf','.ldf','.ndf','.pdb','.dmp','.log','.bak'}
BASE_NAMES={'GOnline.exe','GPackData.zpk','GPackData.zdx','config.xml','d3d9.dll','winmm.dll','sdgo/core.dll','sdgo/catalog.json'}
SECRET_KEYS={'password','passwd','pwd','username','token','secret','apikey','privatekey','birthdays','birthdaypeople','birthdayplayers','accountprofiles','savedaccount','savedpassword','savedaccounts','loginaccount','loginpassword'}


def feed(value):
    if not isinstance(value,str)or len(value)>2048:raise ValueError('服主配置来源地址无效')
    if value.startswith(('git+ssh://','git+https://')):git_source.parse(value);return value
    p=urlsplit(value)
    if p.scheme=='https'and p.hostname and not p.username and not p.password and not p.query and not p.fragment:return value
    if p.scheme=='local+file'and p.netloc in('','localhost')and not p.query and not p.fragment:
        local_path(value);return value
    raise ValueError('服主配置支持 HTTPS、Git SSH/HTTPS 或明确选择的本地清单')


def local_path(value):
    p=urlsplit(value);raw=unquote(p.path)
    if re.match('/[A-Za-z]:/',raw):raw=raw[1:]
    path=Path(raw)
    if not path.is_absolute():raise ValueError('离线配置须使用绝对路径')
    return path.resolve()


def binding(value):
    if not isinstance(value,dict)or value.get('schema_version')!=3 or value.get('kind')!='sdgo-client-runtime'or not ID.fullmatch(str(value.get('source_id',''))):raise ValueError('服主配置绑定无效')
    label=value.get('label');feeds=value.get('feeds')
    if not isinstance(label,str)or not 1<=len(label.strip())<=80 or not isinstance(feeds,list)or not 1<=len(feeds)<=3 or len(set(feeds))!=len(feeds):raise ValueError('服主配置名称或镜像清单无效')
    return {'schema_version':3,'kind':'sdgo-client-runtime','source_id':value['source_id'],'label':label.strip(),
            'feeds':[feed(url)for url in feeds],'framework':framework_source.binding(value.get('framework'))}


def content_name(name):
    updates.safe_name(name)
    if name in BASE_NAMES or name.startswith(('sdgo/local-state/','sdgo/local-mods/','sdgo/logs/','sdgo/backups/'))or Path(name).suffix.casefold()in FORBIDDEN_SUFFIXES:
        raise ValueError('服主客户端内容不能包含程序、SQL、数据库或本地状态：'+name)
    if name.endswith(('/mod.ini','/manager.json')):raise ValueError('Mod 程序元数据来自框架源，参数使用共享设置清单')
    if name==SHARED or name.startswith(('sdgo/mods/','anrs/','fxrs/','gdrs/','mdrs/','scp/','sdrs/','sol/','sprs/','trns/','txrs/','uirs/'))or name=='sdgo/sdgo.ini':return name
    raise ValueError('服主内容不在声明的客户端扩展范围：'+name)


def check_secrets(value):
    if isinstance(value,dict):
        for key,item in value.items():
            normalized=re.sub('[^a-z]','',str(key).casefold())
            if normalized in SECRET_KEYS and item not in(None,'',[],{}):raise ValueError('私人凭据或名单不能进入玩家配置档')
            check_secrets(item)
    elif isinstance(value,list):
        if len(value)==3 and all(isinstance(x,str)for x in value) and re.sub('[^a-z]','',value[1].casefold())in SECRET_KEYS and value[2]:raise ValueError('私人凭据或名单不能进入玩家配置档')
        for item in value:check_secrets(item)


def validate(value,source):
    if not isinstance(value,dict)or value.get('schema_version')!=1 or value.get('kind')!=KIND or value.get('source_id')!=source['source_id']:raise ValueError('服主配置身份不一致')
    if type(value.get('revision'))is not int or value['revision']<1:raise ValueError('服主配置修订无效')
    base=value.get('base',{});updates.safe_name(base.get('executable',''))
    if base.get('executable')!='GOnline.exe'or not re.fullmatch('GOnline-[0-9A-F]{8}-[0-9A-F]{8}',str(base.get('host_key',''))):raise ValueError('服主配置须声明兼容的原版客户端')
    lock=value.get('framework',{});updates.version(lock.get('version'))
    if not HASH.fullmatch(str(lock.get('sha256',''))):raise ValueError('框架版本锁定缺少摘要')
    if lock.get('commit')and not re.fullmatch('[0-9a-f]{40}',lock['commit']):raise ValueError('框架锁定提交无效')
    mods=value.get('mods');files=value.get('files')
    if not isinstance(mods,dict)or not mods or any(not re.fullmatch('[a-z][a-z0-9_]{0,63}',str(k))for k in mods):raise ValueError('服主须选择实际使用的 Mod')
    for version in mods.values():updates.version(version)
    if not isinstance(files,dict)or SHARED not in files or not 1<=len(files)<=100000:raise ValueError('服主内容文件清单无效')
    total=0;case=set()
    for name,entry in files.items():
        content_name(name)
        if name.casefold()in case:raise ValueError('服主内容存在大小写冲突')
        case.add(name.casefold())
        if not isinstance(entry,dict)or type(entry.get('bytes'))is not int or not 0<=entry['bytes']<=512*1024*1024 or not HASH.fullmatch(str(entry.get('sha256',''))):raise ValueError('服主内容文件摘要无效')
        total+=entry['bytes']
    if total>16*1024**3:raise ValueError('服主内容总量超过限制')
    shared=value.get('shared_files',[])
    if not isinstance(shared,list)or not set(shared)<=set(files):raise ValueError('共享配置文件声明无效')
    check_secrets(value.get('settings',{}))
    return value


def read(controller,url,name,limit,refresh=True):
    if url.startswith(('git+ssh://','git+https://')):
        remote,ref=git_source.parse(url);repo=controller.cache/'git'/updates.digest(url.encode());repo.mkdir(parents=True,exist_ok=True)
        if not(repo/'HEAD').is_file():controller.git(repo,['init','--bare']);controller.git(repo,['remote','add','origin',remote])
        if controller.git(repo,['config','--get','remote.origin.url']).decode().strip()!=remote:raise ValueError('内容缓存身份不一致')
        if refresh:controller.git(repo,['fetch','--filter=blob:none','--no-tags','--depth=1','origin','refs/heads/'+ref])
        commit=controller.git(repo,['rev-parse','FETCH_HEAD']).decode().strip()
        tree=controller.git(repo,['ls-tree','-z',commit,'--',name]).split(b'\0')[0]
        if not tree:raise ValueError('内容源缺少配置清单')
        head,leaf=tree.split(b'\t',1);mode,kind,oid=head.split()
        if kind!=b'blob'or mode not in(b'100644',b'100755')or leaf.decode()!=name:raise ValueError('内容源清单不是普通文件')
        cached={r.split()[0]for r in controller.git(repo,['cat-file','--batch-all-objects','--batch-check','--unordered']).splitlines()if r}
        if oid not in cached:controller.git(repo,['fetch','--no-tags','--no-write-fetch-head','--recurse-submodules=no','--filter=blob:none','--stdin','origin'],input=oid+b'\n')
        if int(controller.git(repo,['cat-file','-s',oid.decode()]))>limit:raise ValueError('服主清单超过限制')
        raw=controller.git(repo,['cat-file','blob',oid.decode()])
        if len(raw)>limit:raise ValueError('服主清单超过限制')
        return raw,{'type':'git','repo':str(repo),'commit':commit,'feed':url}
    if url.startswith('local+file:'):
        path=local_path(url);path=path if name==MANIFEST else updates.child(path.parent,name)
        if not path.is_file()or path.stat().st_size>limit:raise ValueError('离线配置文件无效或过大')
        return path.read_bytes(),{'type':'local','root':str(local_path(url).parent),'feed':url}
    path=url if name==MANIFEST else urljoin(url,name);raw=updates.fetch(path,limit,getattr(controller,'operation_stop',controller.stop_event))
    return raw,{'type':'https','feed':url}


def resolve(controller,source):
    errors=[];available=[]
    for index,url in enumerate(source['feeds']):
        try:
            raw,transport=read(controller,url,MANIFEST,16*1024*1024)
            profile=validate(json.loads(raw),source)
            available.append((profile,transport,index+1))
        except InterruptedError:raise
        except(OSError,ValueError,subprocess.SubprocessError)as error:errors.append(str(error))
    if not available:raise ValueError('服主配置源暂不可用；保留当前安装。'+'；'.join(errors))
    revisions={}
    for profile,transport,index in available:
        fingerprint=updates.digest(storage.json_bytes(profile));prior=revisions.setdefault(profile['revision'],fingerprint)
        if prior!=fingerprint:raise ValueError('同一服主修订在不同来源提供了不同清单；拒绝安装')
    # Prefer the configured first source among identical heads, but a lagging
    # mirror must not conceal a newer valid owner revision before it is pinned.
    profile,transport,index=max(available,key=lambda row:row[0]['revision'])
    if transport['type']=='git':
        controller.fetch_mirrors[str(Path(transport['repo']).resolve())]=[(i+1,git_source.parse(v)[0])for i,v in enumerate(source['feeds'])if i+1!=index and v.startswith(('git+ssh://','git+https://'))]
    return profile,transport,index


def base_check(root,profile):
    base=profile['base'];exe=updates.child(root,base['executable'])
    if not exe.is_file()or updates.host_key(exe)!=base['host_key']:raise ValueError('这个服主配置需要匹配版本的原版客户端；不会下载另一套完整客户端覆盖')
    for name,entry in base.get('files',{}).items():
        updates.safe_name(name);path=updates.child(root,name)
        if not path.is_file()or path.stat().st_size!=entry['bytes']:raise ValueError('原版基础资源不兼容：'+name)
        digest=hashlib.sha256()
        with path.open('rb')as f:
            for chunk in iter(lambda:f.read(1024*1024),b''):digest.update(chunk)
        if digest.hexdigest()!=entry['sha256']:raise ValueError('原版基础资源摘要不一致：'+name)


def payload(controller,profile,transport,folder):
    from . import transfers
    folder.mkdir(parents=True,exist_ok=True)
    report=getattr(controller,'report_progress',lambda **values:None)
    stop=getattr(controller,'operation_stop',controller.stop_event)
    def object_path(entry):return controller.cache/'profile-objects'/entry['sha256']
    def reuse(path,entry):
        if transfers.verify(path,entry):return True
        saved=object_path(entry)
        if not transfers.verify(saved,entry):return False
        path.parent.mkdir(parents=True,exist_ok=True);path.unlink(missing_ok=True)
        try:os.link(saved,path)
        except OSError:shutil.copyfile(saved,path)
        return True
    total=sum(e['bytes']for e in profile['files'].values());done=0;count=0
    def start_file(name,entry):
        if stop.is_set():raise InterruptedError('下载已取消')
        report(phase='content',step='downloading',file=name,current_bytes=0,wire_bytes=0,current_total=entry['bytes'],verified_bytes=done,total_bytes=total,verified_files=count,total_files=len(profile['files']))
    def finish_file(path,name,entry):
        nonlocal done,count
        report(step='verifying')
        if not transfers.verify(path,entry):raise ValueError('服主内容文件校验失败：'+name)
        saved=object_path(entry)
        if not transfers.verify(saved,entry):
            saved.parent.mkdir(parents=True,exist_ok=True);saved.unlink(missing_ok=True)
            try:os.link(path,saved)
            except OSError:shutil.copyfile(path,saved)
        done+=entry['bytes'];count+=1
        report(verified_bytes=done,verified_files=count,current_bytes=entry['bytes'],step='verified')
    if transport['type']=='git':
        repo=Path(transport['repo']);commit=transport['commit']
        cached={r.split()[0].decode()for r in controller.git(repo,['cat-file','--batch-all-objects','--batch-check','--unordered']).splitlines()if r}
        pending=[]
        for name,entry in profile['files'].items():
            start_file(name,entry)
            path=updates.child(folder,name)
            if reuse(path,entry):finish_file(path,name,entry)
            else:pending.append((name,entry))
        objects={}
        if pending:
            for row in controller.git(repo,['ls-tree','-r','-z',commit,'--',CONTENT_PREFIX]).split(b'\0'):
                if not row:continue
                head,name=row.split(b'\t',1);mode,kind,oid=head.split()
                if kind==b'blob'and mode in(b'100644',b'100755'):objects[name.decode('utf-8')]=oid.decode()
            for name,entry in pending:
                oid=objects.get(CONTENT_PREFIX+name)
                if oid is None or entry.get('git_blob',oid)!=oid:raise ValueError('服主内容 Git 对象与锁定清单不一致：'+name)
        while pending:
            group=[];group_bytes=0
            while pending and(not group or group_bytes+pending[0][1]['bytes']<=8*1024*1024):
                name,entry=pending.pop(0);group.append((name,entry));group_bytes+=entry['bytes']
            report(phase='content',step='downloading',file=group[0][0]if len(group)==1 else'一组内容文件（'+str(len(group))+' 个）',current_bytes=0,wire_bytes=0,current_total=group_bytes,verified_bytes=done,total_bytes=total,verified_files=count,total_files=len(profile['files']))
            wanted=list(dict.fromkeys(objects[CONTENT_PREFIX+n]for n,e in group if objects[CONTENT_PREFIX+n]not in cached))
            if wanted:
                controller.git(repo,['fetch','--no-tags','--no-write-fetch-head','--recurse-submodules=no','--filter=blob:none','--stdin','origin'],input=''.join(oid+'\n'for oid in wanted).encode())
                cached.update(wanted)
            batch=folder.parent/(uuid.uuid4().hex+'.batch')
            try:
                requests=''.join(objects[CONTENT_PREFIX+n]+'\n'for n,e in group).encode()
                controller.git(repo,['cat-file','--batch'],input=requests,output=batch)
                with batch.open('rb')as stream:
                    for name,entry in group:
                        start_file(name,entry);oid=objects[CONTENT_PREFIX+name]
                        if stream.readline(200)!=(oid+' blob '+str(entry['bytes'])+'\n').encode():raise ValueError('Git 内容对象大小或身份不一致')
                        path=updates.child(folder,name);path.parent.mkdir(parents=True,exist_ok=True);temporary=path.with_name(path.name+'.download')
                        remaining=entry['bytes']
                        with temporary.open('wb')as output:
                            while remaining:
                                if stop.is_set():raise InterruptedError('下载已取消')
                                block=stream.read(min(remaining,1024*1024))
                                if not block:raise ValueError('Git 内容未完整接收')
                                output.write(block);remaining-=len(block);report(current_bytes=entry['bytes']-remaining)
                        if stream.read(1)!=b'\n'or not transfers.verify(temporary,entry):raise ValueError('服主内容文件校验失败：'+name)
                        temporary.replace(path);finish_file(path,name,entry)
                    if stream.read(1):raise ValueError('Git 返回了额外内容')
            finally:batch.unlink(missing_ok=True)
    elif not profile.get('payload'):
        for name,entry in profile['files'].items():
            start_file(name,entry)
            path=updates.child(folder,name)
            if not reuse(path,entry):
                if transport['type']=='local':
                    raw=updates.child(Path(transport['root']),CONTENT_PREFIX+name).read_bytes()
                    if len(raw)!=entry['bytes']or hashlib.sha256(raw).hexdigest()!=entry['sha256']:raise ValueError('服主内容文件校验失败：'+name)
                    storage.atomic(path,raw)
                else:
                    transfers.download(urljoin(transport['feed'],CONTENT_PREFIX+quote(name,safe='/')),object_path(entry),entry,stop,lambda n:report(current_bytes=n))
                    reuse(path,entry)
            finish_file(path,name,entry)
    else:
        asset=profile.get('payload',{});name=asset.get('path');updates.safe_name(name)
        if not name.endswith('.zip')or not HASH.fullmatch(str(asset.get('sha256',''))):raise ValueError('服主内容包清单无效')
        if type(asset.get('bytes'))is not int or not 0<asset['bytes']<=updates.MAX_ARCHIVE:raise ValueError('服主内容包大小无效')
        archive=folder.parent/(asset['sha256']+'.zip')
        if not archive.is_file()or hashlib.sha256(archive.read_bytes()).hexdigest()!=asset['sha256']:
            if transport['type']=='local':raw=updates.child(Path(transport['root']),name).read_bytes()
            else:
                report(phase='content_archive',current_bytes=0,total_bytes=asset['bytes'])
                transfers.download(urljoin(transport['feed'],name),archive,asset,stop,lambda n:report(current_bytes=n));raw=None
            if raw is not None:
                if len(raw)!=asset['bytes']or hashlib.sha256(raw).hexdigest()!=asset['sha256']:raise ValueError('服主内容包摘要不一致')
                storage.atomic(archive,raw)
        with zipfile.ZipFile(archive)as z:
            if set(z.namelist())!=set(profile['files'])or len(z.namelist())!=len(profile['files']):raise ValueError('服主内容包文件清单不一致')
            for name,entry in profile['files'].items():
                info=z.getinfo(name)
                if info.file_size!=entry['bytes']:raise ValueError('服主内容包文件大小不一致')
                path=updates.child(folder,name);path.parent.mkdir(parents=True,exist_ok=True)
                with z.open(name)as src,path.open('wb')as dst:shutil.copyfileobj(src,dst,1024*1024)
    for name,entry in profile['files'].items():
        path=updates.child(folder,name)
        if path.stat().st_size!=entry['bytes']or hashlib.sha256(path.read_bytes()).hexdigest()!=entry['sha256']:raise ValueError('服主内容文件校验失败：'+name)
        from .owner_channel import public_file
        if name==SHARED:check_secrets(json.loads(path.read_bytes()))
        else:public_file(name,path.read_bytes())
    return folder


def plan(controller,host,source):
    from .client_runtime import file_hash,matches,RECEIPT,SOURCE
    report=getattr(controller,'report_progress',lambda **values:None)
    report(phase='manifest',source=source['label'])
    root=Path(host['path']).resolve();profile,transport,index=resolve(controller,source);base_check(root,profile)
    identity=updates.digest(storage.json_bytes(profile));folder=controller.cache/'profiles'/source['source_id']/identity/'content'
    # A new content revision reuses byte-identical installed files as well as
    # completed downloads. Never download the whole resource set for one edit.
    report(phase='reuse',total_files=len(profile['files']),verified_files=0)
    for number,(name,entry)in enumerate(profile['files'].items(),1):
        if getattr(controller,'operation_stop',controller.stop_event).is_set():raise InterruptedError('更新已取消')
        cached=updates.child(folder,name);installed=updates.child(root,name)
        if not matches(cached,entry)and matches(installed,entry):
            cached.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(installed,cached)
        report(verified_files=number,file=name)
    try:payload(controller,profile,transport,folder)
    except InterruptedError:raise
    except(OSError,ValueError,subprocess.SubprocessError)as first:
        for fallback_index,url in enumerate(source['feeds']):
            if url==transport['feed']:continue
            try:
                raw,alternate=read(controller,url,MANIFEST,16*1024*1024)
                mirrored=validate(json.loads(raw),source)
                if updates.digest(storage.json_bytes(mirrored))!=identity:continue
                report(phase='mirror',message='备用源已匹配同一配置修订和摘要，继续缺失文件')
                payload(controller,profile,alternate,folder);transport=alternate;index=fallback_index+1;break
            except InterruptedError:raise
            except(OSError,ValueError,subprocess.SubprocessError):continue
        else:raise ValueError('服主内容在配置的镜像均不可用或校验失败；保留当前安装：'+str(first))from first
    # Compose uses the same ownership, dependency and one-transaction installer
    # as complete-runtime updates, without requiring any original-game payload.
    contract={'layers':{'schema_version':1,'minimum_framework':profile['framework']['version'],
        'separate_manager':True,
        'locked_framework':profile['framework'],'required_mods':profile['mods'],'mod_ids':list(profile['mods']),
        'shared_files':profile.get('shared_files',[])},'root_extensions':[],'entrypoints':[],
        'trees':['sdgo/mods','anrs','fxrs','gdrs','mdrs','scp','sdrs','sol','sprs','trns','txrs','uirs']}
    entries={n:dict(e)for n,e in profile['files'].items()};entries={n:{k:e[k]for k in('bytes','sha256')}for n,e in entries.items()}
    # Pass the already verified content file directly rather than changing the
    # controller transport while another UI thread reads state.
    declared=framework_source.binding(profile.get('framework_source',source['framework']))
    authorized=framework_source.binding(source['framework'])
    if any(declared[k]!=authorized[k]for k in('repository','source_id')):raise ValueError('服主不能静默更换已授权的框架发布者')
    # The owner chooses the pinned programme/channel; an imported binding can
    # choose matching transport mirrors without being silently overwritten.
    delegated={**authorized,'channel':declared['channel']}if authorized.get('transport')==declared.get('transport')else declared
    report(phase='framework')
    combined,effective,layers=runtime_layers.compose(controller,{**source,'framework':delegated},contract,root,entries,None,identity,shared_data=(folder/SHARED).read_bytes())
    if layers['framework']['manifest'].get('capabilities',{}).get('runtime_profiles')!=1:raise ValueError('所选框架程序尚不支持通用服主配置档；保留现有客户端')
    needed=[n for n,e in effective.items()if not matches(updates.child(root,n),e)]
    prior=storage.read_json(root/RECEIPT,{})
    if prior and prior.get('source_id')!=source['source_id']:raise ValueError('这个客户端已有不同服主的安装记录')
    prior_profile=prior.get('profile',{});previous_files=prior['files']if prior_profile else{}
    if prior_profile.get('revision',0)>profile['revision']:raise ValueError('拒绝自动回退到较旧的服主配置')
    if prior_profile.get('revision')==profile['revision']and prior_profile.get('sha256')!=identity:raise ValueError('服主不能重复使用同一修订号发布不同内容')
    retired=[n for n in previous_files if n not in combined and n!=SOURCE and updates.child(root,n).is_file()]
    snapshot={n:file_hash(updates.child(root,n))if updates.child(root,n).is_file()else None for n in set(needed)|set(retired)}
    snapshot[RECEIPT]=file_hash(root/RECEIPT)if(root/RECEIPT).is_file()else None
    value={'source_id':source['source_id'],'commit':identity,'entries':combined,'needed':needed,'retired':retired,'snapshot':snapshot,
           'binding':source,'mirror':index,'contract':contract,'repository':'','effective':effective,'preserved':{},'merged':[],
           'profile':{'revision':profile['revision'],'sha256':identity,'transport':transport['type'],'base':profile['base']},
           'profile_payload':str(folder),**layers}
    value['digest']=updates.digest(storage.json_bytes(value));controller.plans[host['id']]=value
    return {'status':'available'if needed or retired else'current','commit':identity,'source':source['label'],'changed_files':len(needed),
            'retired_files':len(retired),'bytes':sum(combined[n]['bytes']for n in needed),'preserved_settings':len(profile.get('personal_settings',[])),
            'digest':value['digest'],'files':needed,'retired':retired,'data_migrations':False,'framework_version':layers['framework']['version'],
            'framework_commit':layers['framework']['commit'],'profile_revision':profile['revision'],'profile_transport':transport['type']}


def stage(plan,destination):
    for name in plan['needed']:
        if name not in plan['content_files']:continue
        target=updates.child(destination,name);target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(updates.child(Path(plan['profile_payload']),name),target)
