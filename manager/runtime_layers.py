"""Compose program releases, owner content and personal preferences once."""
from pathlib import Path
import base64,json,re
from . import storage,updates,framework_source

SHARED='sdgo/shared-settings.json'


def program_files(framework):
    return {n[7:]if n.startswith('client/')else n:{'source':n,**entry}
            for n,entry in framework['manifest']['files'].items()if n==updates.MANAGER or n.startswith('client/')}


def compose(controller,binding,contract,root,entries,repo,commit):
    spec=contract.get('layers',{})
    if spec.get('schema_version')!=1:raise ValueError('服主内容缺少双源归属声明')
    framework=framework_source.load(controller,binding['framework'],constraints=spec);manifest=framework['manifest'];files=program_files(framework)
    minimum=spec.get('minimum_framework');maximum=spec.get('maximum_framework')
    current=updates.version(manifest['version'])[:3]
    if minimum is None or current<updates.version(minimum)[:3]or maximum and current>updates.version(maximum)[:3]:raise ValueError('框架版本不符合服主内容的兼容要求')
    catalog=json.loads((Path(framework['bundle'])/'client/sdgo/catalog.json').read_text('utf-8'))
    mods={m['id']:m for m in catalog['mods'] if m['target']=='client'}
    for identity,required in spec.get('required_mods',{}).items():
        if identity not in mods or updates.version(mods[identity]['version'])<updates.version(required):raise ValueError('服主内容需要兼容的 Mod：'+identity)
    allowed={'sdgo/sdgo.ini'}
    for identity,mod in mods.items():
        directory=Path(framework['bundle'])/'client/sdgo/mods'/identity
        cfg=updates.read_ini((directory/'mod.ini').read_bytes());registration=storage.read_json(directory/'manager.json',{})
        for name in set(mod.get('config_files',[]))|set(registration.get('configuration_files',[]))|set(cfg.get('mod','config_files',fallback='').split(',')):
            if name and name not in('mod.ini','manager.json','root:config.xml')and not name.casefold().endswith(('.dll','.exe')):allowed.add('sdgo/mods/'+identity+'/'+name)
    shared_files=spec.get('shared_files',[])
    if not isinstance(shared_files,list)or len(set(shared_files))!=len(shared_files)or not set(shared_files)<=allowed:raise ValueError('共享配置超出 Mod 声明的范围')
    for name in shared_files:
        updates.safe_name(name)
        if name not in entries:raise ValueError('服主共享配置文件缺失：'+name)
    collisions=(set(entries)&set(files))-set(shared_files)
    if collisions:raise ValueError('服主内容不能替换框架程序或元数据：'+', '.join(sorted(collisions)[:5]))
    if SHARED not in entries:raise ValueError('服主内容缺少共享设置清单')
    raw=controller.git(repo,['show',commit+':'+SHARED])
    if len(raw)>1024*1024:raise ValueError('共享设置清单过大')
    shared=json.loads(raw)
    if shared.get('schema_version')!=1:raise ValueError('共享设置清单版本无效')
    patches=shared.get('patches',{});personal=shared.get('personal_settings',[])
    if not isinstance(patches,dict)or not isinstance(personal,list):raise ValueError('共享设置格式无效')
    rendered={};effective={n:{k:v for k,v in e.items()if k!='source'}for n,e in files.items()};effective.update(entries)
    for name,rows in patches.items():
        updates.safe_name(name)
        if name not in files or not re.fullmatch(r'sdgo/mods/[a-z][a-z0-9_]*/mod\.ini',name):raise ValueError('共享设置只能覆盖 Mod 参数：'+name)
        if not isinstance(rows,list)or len(rows)>10000:raise ValueError('共享参数清单无效')
        for row in rows:
            if not isinstance(row,list)or len(row)!=3 or not(row[0]=='settings'or row[:2]==['mod','enabled']):raise ValueError('共享设置不能替换 Mod 身份或接口')
        data=storage.patch_ini((Path(framework['bundle'])/files[name]['source']).read_bytes(),rows)
        rendered[name]=data
    # Personal fields are explicitly declared by the distributor, rather than
    # preserving every gameplay setting and hiding owner configuration changes.
    by_file={}
    for row in personal:
        if not isinstance(row,dict)or set(row)!={'file','section','key'}:raise ValueError('个人参数声明无效')
        name=row['file'];updates.safe_name(name)
        if name not in files or not re.fullmatch(r'sdgo/mods/[a-z][a-z0-9_]*/mod\.ini',name)or row['section']!='settings':raise ValueError('个人参数超出允许范围')
        identity=name.split('/')[2];registration=storage.read_json(Path(framework['bundle'])/'client/sdgo/mods'/identity/'manager.json',{})
        registered={field['key'].casefold()for field in registration.get('settings',[])if field.get('file')=='mod.ini'}
        if registration.get('settings')and row['key'].casefold()not in registered:raise ValueError('个人参数未在 Mod 登记')
        path=updates.child(root,name)
        if path.is_file():
            cfg=updates.read_ini(path.read_bytes())
            if cfg.has_option(row['section'],row['key']):by_file.setdefault(name,[]).append((row['section'],row['key'],cfg.get(row['section'],row['key'])))
    for name,rows in by_file.items():rendered[name]=storage.patch_ini(rendered.get(name,(Path(framework['bundle'])/files[name]['source']).read_bytes()),rows)
    for name,data in rendered.items():effective[name]={'bytes':len(data),'sha256':updates.digest(data)}
    receipt={'schema_version':1,'version':manifest['version'],'channel':manifest['channel'],'source_commit':manifest['source_commit'],
             'mods':{m['id']:m['version']for m in catalog['mods']}}
    generated={'sdgo/release.json':base64.b64encode(storage.json_bytes(receipt)).decode()}
    combined={**{n:{k:v for k,v in e.items()if k!='source'}for n,e in files.items()},**entries}
    for name,encoded in generated.items():combined[name]=effective[name]={'bytes':len(base64.b64decode(encoded)),'sha256':updates.digest(base64.b64decode(encoded))}
    return combined,effective,{'framework':framework,'framework_files':files,'content_files':sorted(entries),
        'rendered':{n:base64.b64encode(v).decode()for n,v in rendered.items()},'generated':generated,'shared_files':shared_files,'personal_settings':personal}


def stage(plan,stage):
    import shutil
    for name,entry in plan.get('framework_files',{}).items():
        if name not in plan['needed']or name in plan['content_files']:continue
        target=updates.child(stage,name);target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(Path(plan['framework']['bundle'])/entry['source'],target)
    for name,encoded in plan.get('generated',{}).items():
        if name in plan['needed']:storage.atomic(updates.child(stage,name),base64.b64decode(encoded,validate=True))


def render(plan,stage):
    for name,encoded in plan.get('rendered',{}).items():
        if name in plan['needed']:storage.atomic(updates.child(stage,name),base64.b64decode(encoded,validate=True))
