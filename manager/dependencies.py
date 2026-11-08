"""Versioned Mod relationships. No runtime, configuration or database writes."""
from __future__ import annotations
import re
import configparser

ID=re.compile(r'[a-z][a-z0-9_-]{0,63}\Z')
CAP=re.compile(r'[a-z][a-z0-9_.-]{0,95}\Z')
KINDS=('required','optional','conflicts')


def version(value):
    if not isinstance(value,str) or not re.fullmatch(r'\d+(?:\.\d+){0,3}(?:[-+][A-Za-z0-9_.-]+)?',value):
        raise ValueError('Mod 依赖版本无效')
    main,_,tail=value.partition('-');main=main.split('+')[0]
    parts=tuple(map(int,main.split('.')))
    if any(p>2147483647 for p in parts):raise ValueError('Mod 依赖版本超出范围')
    suffix=tuple((0,int(p))if p.isdigit()else(1,p)for p in tail.split('.'))if tail else()
    return (parts+(0,)*(4-len(parts)),not bool(tail),suffix)


def validate(contract):
    provides=contract.get('provides',{})
    if not isinstance(provides,dict) or any(not CAP.fullmatch(k) or type(v)is not int or not 1<=v<=65535 for k,v in provides.items()):
        raise ValueError('Mod 能力声明无效')
    rules=contract.get('dependencies',{})
    if not isinstance(rules,dict) or set(rules)-set(KINDS):raise ValueError('Mod 依赖类型无效')
    settings={s['id'] for s in contract.get('settings',[])}
    runtime_enabled=contract.get('runtime_enabled_setting')
    if runtime_enabled is not None:
        field=next((s for s in contract.get('settings',[])if s['id']==runtime_enabled),{})
        if field.get('type')!='boolean'or field.get('file')not in('mod.ini','config.ini'):raise ValueError('运行开关声明无效')
    seen=set()
    for kind,rows in rules.items():
        if not isinstance(rows,list) or len(rows)>64:raise ValueError('Mod 依赖列表无效')
        for row in rows:
            if not isinstance(row,dict) or set(row)-{'id','minimum','capability','capability_version','scope','when'}:
                raise ValueError('Mod 依赖字段无效')
            identity=row.get('id','')
            if not ID.fullmatch(identity) or(identity==contract['id']and row.get('scope','host')=='host')or identity in seen:raise ValueError('Mod 依赖标识重复或无效')
            seen.add(identity)
            if 'minimum'in row:version(row['minimum'])
            if row.get('scope','host')not in ('host','client','battle','agent','world'):raise ValueError('Mod 依赖宿主无效')
            if 'capability'in row and not CAP.fullmatch(row['capability']):raise ValueError('Mod 依赖能力无效')
            if type(row.get('capability_version',1))is not int or not 1<=row.get('capability_version',1)<=65535:raise ValueError('Mod 依赖能力版本无效')
            if 'when'in row:
                condition=row['when']
                if not isinstance(condition,dict) or set(condition)!={'setting','equals'} or condition['setting']not in settings or type(condition['equals'])not in (bool,int,float,str):
                    raise ValueError('Mod 条件依赖无效')
    return contract


def applies(row,node):
    condition=row.get('when')
    return not condition or node.get('settings',{}).get(condition['setting'])==condition['equals']


def issues(nodes):
    """Nodes include host/role/contract/settings; evaluate actual effective switches."""
    result=[]
    for node in nodes:
        if not node['enabled']:continue
        contract=node.get('contract')or{}
        for kind,rows in contract.get('dependencies',{}).items():
            for row in rows:
                if not applies(row,node):continue
                scope=row.get('scope','host')
                peers=[p for p in nodes if p['id']==row['id'] and (p['host']==node['host'] if scope=='host' else p['role']==scope)]
                active=[p for p in peers if p['enabled']]
                problem=''
                if kind=='conflicts':
                    if active:problem='不能同时启用'
                elif not active:
                    if kind=='required':problem='需要先启用'
                else:
                    compatible=[]
                    for peer in active:
                        try:old='minimum'in row and version(peer['version'])<version(row['minimum'])
                        except ValueError:old=True
                        cap=row.get('capability')
                        if not old and (not cap or peer.get('contract',{}).get('provides',{}).get(cap,0)==row.get('capability_version',1)):compatible.append(peer)
                    if not compatible:problem='接口或版本不兼容，请配套更新'
                if problem:
                    result.append({'host':node['host'],'id':node['id'],'dependency':row['id'],'kind':kind,
                                   'message':(contract.get('title')or node['id'])+'：'+problem+' '+row['id']})
    active={(n['host'],n['id']):n for n in nodes if n['enabled']};edges={key:[]for key in active}
    for key,n in active.items():
        for kind in('required','optional'):
            for rule in n.get('contract',{}).get('dependencies',{}).get(kind,[]):
                peer=(n['host'],rule['id'])
                if rule.get('scope','host')=='host'and applies(rule,n)and peer in active:edges[key].append(peer)
    visited=set();stack=[]
    def visit(key):
        if key in stack:
            n=active[key];result.append({'host':key[0],'id':key[1],'dependency':key[1],'kind':'cycle','message':(n.get('contract',{}).get('title')or n['id'])+'：存在循环加载依赖'});return
        if key in visited:return
        stack.append(key)
        for peer in edges[key]:visit(peer)
        stack.pop();visited.add(key)
    for key in edges:visit(key)
    return result


def enforce_transition(before,after):
    old={(p['host'],p['id'],p['dependency'],p['kind'])for p in issues(before)}
    new=[p for p in issues(after)if(p['host'],p['id'],p['dependency'],p['kind'])not in old]
    if new:raise ValueError('；'.join(p['message']for p in new))


def content_files(contract):
    """Older installed manifests inherit known data guards, without edits."""
    retirement=contract.get('retirement')
    if retirement is None and contract.get('distribution')!='local_only'and ID.fullmatch(contract.get('id','')):
        from pathlib import Path
        import json
        path=Path(__file__).resolve().parents[1]/'mods'/contract['id']/'manager.json'
        if path.is_file():retirement=json.loads(path.read_bytes()).get('retirement')
    return(retirement or{}).get('content_files',[])


def enforce_install(root,changes,role):
    """Evaluate the actual candidate metadata and settings before file writes."""
    from pathlib import Path
    from . import storage
    import copy,json
    root=Path(root)
    def nodes(candidate):
        def raw(name):
            if candidate and name in changes:return changes[name]or b''
            path=root/name;return path.read_bytes()if path.is_file()else b''
        folders={p.parent.relative_to(root).as_posix()for tree in('mods','owner-mods','local-mods')for p in(root/'sdgo'/tree).glob('*/mod.ini')}
        if candidate:folders.update(n.rsplit('/',1)[0]for n in changes if n.endswith('/mod.ini')and n.startswith(('sdgo/mods/','sdgo/owner-mods/','sdgo/local-mods/')))
        try:global_ini=storage.ini_values(raw('sdgo/sdgo.ini'))
        except configparser.Error:
            if candidate or 'sdgo/sdgo.ini'not in changes:raise
            global_ini=storage.ini_values(b'')
        result=[]
        for folder in folders:
            data=raw(folder+'/mod.ini')
            if not data:continue
            try:
                cfg=storage.ini_values(data);identity=cfg.get('mod','id',fallback=folder.split('/')[-1]);registration=raw(folder+'/manager.json')
                contract=json.loads(registration)if registration else {'id':identity}
                validate(contract);contract=copy.deepcopy(contract)
            except(ValueError,KeyError,TypeError,configparser.Error):
                if not candidate and any(folder+'/'+leaf in changes for leaf in('mod.ini','manager.json')):continue
                raise
            contract['dependencies']={kind:[row for row in rows if row.get('scope','host')in('host',role)]for kind,rows in contract.get('dependencies',{}).items()}
            category=cfg.get('mod','category',fallback='other');feature=cfg.get('mod','feature',fallback=identity)
            enabled=cfg.get('mod','enabled',fallback='1')!='0'and all(global_ini.get(section,key,fallback='1')!='0'for section,key in(('categories',category),('features',feature),('mods',identity)))
            settings={}
            for field in contract.get('settings',[]):
                content=storage.ini_values(raw(folder+'/'+field['file']))
                value=content.get('settings',field['key'],fallback=field.get('default',''))
                if field['type']=='boolean':value=str(value).casefold()in('1','true')
                elif field['type']=='integer':value=int(value)
                elif field['type']=='number':value=float(value)
                settings[field['id']]=value
            gate=contract.get('runtime_enabled_setting')
            if gate:enabled=enabled and bool(settings.get(gate))
            result.append({'host':'target','role':role,'id':identity,'version':cfg.get('mod','version',fallback='0'),
                           'enabled':enabled,'settings':settings,'contract':contract})
        return result
    before,after=nodes(False),nodes(True);enforce_transition(before,after)
    old={n['id']:n for n in before};new={n['id']:n for n in after}
    for identity,node in old.items():
        if not node['enabled']or new.get(identity,{}).get('enabled'):continue
        for name in content_files(node['contract']):
            path=root/'sdgo/mods'/identity/name
            if path.is_file()and path.stat().st_size:raise ValueError('已安装内容仍需要 '+identity+'，先处理内容引用后再退役运行时')


def runtime_sections(contract):
    """Export the same host-local rules for the native loader; symbols stay requires=."""
    validate(contract);sections={'framework.dependencies':{'schema':'1'}}
    for kind in KINDS:
        rows=[r for r in contract.get('dependencies',{}).get(kind,[])if r.get('scope','host')=='host']
        sections['framework.dependencies'][kind]=','.join(r['id']for r in rows)
        for row in rows:
            sections['framework.dependency.'+row['id']]={k:str(row[k])for k in ('minimum','capability','capability_version')if k in row}
            if row.get('when'):
                field=next(f for f in contract['settings']if f['id']==row['when']['setting'])
                stringify=lambda v:'1'if v is True else'0'if v is False else str(v)
                sections['framework.dependency.'+row['id']].update(when_key=field['key'],when_file=field['file'],
                    when_value=stringify(row['when']['equals']),when_default=stringify(field.get('default','')))
    if contract.get('provides'):sections['framework.capabilities']={k:str(v)for k,v in contract['provides'].items()}
    if contract.get('runtime_enabled_setting'):
        field=next(s for s in contract['settings']if s['id']==contract['runtime_enabled_setting'])
        sections['framework.dependencies'].update(enabled_key=field['key'],enabled_file=field['file'],enabled_default='1'if field.get('default',True)else'0')
    return sections
