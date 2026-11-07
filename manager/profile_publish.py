"""Export owner-selected configuration and generated content, not game originals."""
from pathlib import Path
import hashlib,json,shutil,time,uuid,zipfile
from . import storage,updates,owner_channel,server_profiles,runtime_layers


def git_stage(controller,directory,manifest,zip_payload=False):
    directory=Path(directory)
    attributes=directory/'.gitattributes';prior=attributes.read_bytes()if attributes.is_file()else b''
    marker=b'# SDGO owner profile: preserve the declared bytes\n'
    if marker not in prior:
        rules=b'\n'+marker+b'.gitattributes -text\nserver-profile.json -text -filter -ident\nprofile-source.json -text -filter -ident\ncontent/** -text -filter -ident\nreleases/** -text -filter -ident\n'
        storage.atomic(attributes,prior+rules)
    paths=['.gitattributes','server-profile.json','profile-source.json','content']
    if zip_payload:paths.append('releases')
    controller.git(directory,['add','--',*paths])
    # Existing tracked files may still have stat records from an earlier text
    # conversion. Re-normalize after installing the narrow binary rules.
    controller.git(directory,['add','--renormalize','--','server-profile.json','profile-source.json','content'])
    indexed={}
    for row in controller.git(directory,['ls-files','--stage','-z']).split(b'\0'):
        if not row:continue
        header,name=row.split(b'\t',1);mode,oid,stage=header.split()
        if stage!=b'0':raise storage.Conflict('发布目录有未解决的 Git 冲突')
        indexed[name.decode('utf-8')]=oid.decode()
    expected={server_profiles.CONTENT_PREFIX+n:e['git_blob']for n,e in manifest['files'].items()}
    for name in('server-profile.json','profile-source.json'):
        raw=(directory/name).read_bytes();expected[name]=hashlib.sha1(('blob '+str(len(raw))+'\0').encode()+raw).hexdigest()
    if any(indexed.get(name)!=oid for name,oid in expected.items()):raise ValueError('Git 过滤规则改变了锁定内容的原始字节；拒绝发布')


def snapshot(host,source,framework,extras=(),personal_settings=()):
    root=Path(host['path']).resolve();source=server_profiles.binding(source)
    # Existing providers already register their exact generated outputs and
    # receipts. Reuse that inventory instead of diffing an entire client tree.
    legacy={'publisher_id':source['source_id'],'label':source['label'],'feeds':[]}
    files,_,_=owner_channel.snapshot(host,legacy,list(extras),max_file_bytes=512*1024*1024,include_game_base=False)
    official=runtime_layers.program_files(framework);catalog=storage.read_json(Path(framework['bundle'])/'client/sdgo/catalog.json')
    programs={m['id']:m for m in catalog['mods']if m['target']=='client'}
    selected={};patches={};shared_files=[];content={}
    for identity,mod in programs.items():
        folder=root/'sdgo/mods'/identity
        if not(folder/'mod.ini').is_file():continue
        current=updates.read_ini((folder/'mod.ini').read_bytes())
        if current.get('mod','distribution',fallback='public')=='local_only':continue
        if current.get('mod','version',fallback='')!=mod['version']:raise ValueError('先采用所选框架中的 Mod 版本，再发布配置档：'+identity)
        selected[identity]=mod['version'];rows=[]
        if current.has_section('settings'):rows.extend(['settings',key,value]for key,value in current['settings'].items())
        rows.append(['mod','enabled',current.get('mod','enabled',fallback='1')]);patches['sdgo/mods/'+identity+'/mod.ini']=rows
        registration=storage.read_json(Path(framework['bundle'])/'client/sdgo/mods'/identity/'manager.json')
        configs=set(mod.get('config_files',[]))|set(registration.get('configuration_files',[]))|set(current.get('mod','config_files',fallback='').split(','))
        for name in configs:
            relative='sdgo/mods/'+identity+'/'+name
            if name in('','mod.ini','manager.json','root:config.xml'):continue
            path=updates.child(root,relative)
            if relative not in files and path.is_file():
                before=path.stat();raw=path.read_bytes();after=path.stat()
                if(before.st_mtime_ns,before.st_size)!=(after.st_mtime_ns,after.st_size):raise storage.Conflict('共享配置正在变化：'+relative)
                if len(raw)>512*1024*1024:raise ValueError('共享配置超过文件限制：'+relative)
                owner_channel.public_file(relative,raw);files[relative]=raw
            if relative in files:shared_files.append(relative)
    for name,raw in files.items():
        if name in(owner_channel.BINDING,owner_channel.RECEIPT):continue
        if name in official and name not in shared_files and name!='sdgo/sdgo.ini':continue
        if name.endswith(('/mod.ini','/manager.json'))or name.startswith('sdgo/symbols/'):continue
        server_profiles.content_name(name);content[name]=raw
    if 'sdgo/sdgo.ini'in content:shared_files.append('sdgo/sdgo.ini')
    shared={'schema_version':1,'patches':patches,'personal_settings':list(personal_settings)}
    server_profiles.check_secrets(shared);content[server_profiles.SHARED]=storage.json_bytes(shared)
    manifest={'schema_version':1,'kind':server_profiles.KIND,'source_id':source['source_id'],'label':source['label'],
              'revision':1,'base':{'executable':'GOnline.exe','host_key':updates.host_key(root/'GOnline.exe')},
              'framework_source':source['framework'],
              'framework':{k:framework[k]for k in('version','sha256','commit')},'mods':selected,
              'shared_files':sorted(set(shared_files)),
              'files':{n:{'bytes':len(raw),'sha256':updates.digest(raw),'git_blob':hashlib.sha1(('blob '+str(len(raw))+'\0').encode()+raw).hexdigest()}for n,raw in content.items()}}
    server_profiles.validate(manifest,source)
    return content,manifest


def publish(host,source,framework,directory,extras=(),personal_settings=(),zip_payload=False):
    root=Path(host['path']).resolve();directory=Path(directory).resolve()
    if directory==directory.parent or directory.is_relative_to(root)or root.is_relative_to(directory):raise ValueError('配置档发布目录须与客户端目录分开')
    content,manifest=snapshot(host,source,framework,extras,personal_settings)
    directory.mkdir(parents=True,exist_ok=True);marker=directory/server_profiles.MANIFEST;prior=storage.read_json(marker,{})
    if prior and prior.get('source_id')!=source['source_id']:raise ValueError('发布目录属于另一服主')
    old={k:v for k,v in prior.items()if k not in('revision','payload')};new={k:v for k,v in manifest.items()if k not in('revision','payload')}
    manifest['revision']=prior.get('revision',0)+(old!=new or not prior)
    for name,raw in content.items():storage.atomic(updates.child(directory,server_profiles.CONTENT_PREFIX+name),raw)
    if zip_payload:
        asset='releases/content-'+str(manifest['revision'])+'.zip';target=updates.child(directory,asset);target.parent.mkdir(parents=True,exist_ok=True)
        if target.exists()and old!=new:raise ValueError('已发布内容包不可覆盖')
        if not target.exists():
            with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED)as z:
                for name,raw in sorted(content.items()):z.writestr(name,raw)
        raw=target.read_bytes()
        if len(raw)>updates.MAX_ARCHIVE:raise ValueError('离线压缩包超过限制；请使用目录或 Git 内容源')
        manifest['payload']={'path':asset,'bytes':len(raw),'sha256':updates.digest(raw)}
    storage.atomic(marker,storage.json_bytes(manifest));storage.atomic(directory/'profile-source.json',storage.json_bytes(source))
    return {'revision':manifest['revision'],'files':len(content),'bytes':sum(map(len,content.values())),'manifest':str(marker),'framework':manifest['framework']['version']}
