"""Editable server dependencies in portable projects, with no runtime authority.

Missing values remain missing. Only audited shared fields come from the actual
client record. A selected stock template is a hash-bound complete clone, never a
source of guessed defaults. All changes retain the original package as evidence.
"""
from __future__ import annotations

import copy
import hashlib

from .native_content import validate_project_bindings
from .repository import _INTEGER_KINDS
from .server_dependencies import (
    _spec, canonical, normalize_partial_row, normalize_row, paired_values,
    row_hash, validate_manifest,
)

_LABELS={
    'SkillID':'技能编号','SkillName':'技能名称','Activate':'触发类型','ActivateValue':'触发数值',
    'MoveLv':'移动等级','DashLv':'冲刺等级','QuickLv':'快步等级','MotiveLv':'推进等级',
    'BoostIncreaseRate':'推进恢复倍率','BoostPower':'推进增量','Attack':'攻击增量','Defense':'防御增量',
    'RadarRadius':'雷达范围','SpIncreaseRate':'SP 增量','Scale':'体型倍率',
    'AutoBulletChargeRate':'自动回弹倍率','BulletChargeRate':'装填倍率','AtkSpdRate':'攻速倍率',
    'ActiveResult':'触发结果','Feature':'特性','UseTarget':'作用对象','BattleMode':'服务端战斗模式',
    'PartSpLevel':'部件 SP 等级','FunnelID':'浮游编号','FunnelProp':'浮游属性','AniTime':'动作时长',
    'AttackDelay':'攻击延迟','AttackCount':'攻击次数','Stealable':'可被夺取',
}


def stock_templates(catalog,kind):
    """Expose only complete, unowned rows from the configured target catalog."""
    _,section,_,_,key,_,_,_=_spec(kind)
    pending=catalog.get('pending_'+kind+'_ids',set())
    result=[]
    for identity,row in sorted(catalog.get(section,{}).items()):
        if identity in pending:continue
        try:values=normalize_row(row,kind=kind)
        except (ValueError,OverflowError,TypeError):continue
        if values[key]!=identity:continue
        result.append({'id':identity,'name':values.get('SkillName','浮游定义 '+str(identity)),
                       'sha256':row_hash(values,kind=kind),'values':values})
    return result


def describe(group,files,templates):
    result=[]
    for kind,native_kind,part in (('skill',13,'server_skills'),('funnel',11,'server_funnels')):
        _,section,_,columns,key,schema,_,_=_spec(kind)
        definitions={row['id']:row['definition'] for row in group['components'].get(part,{}).get(section,[])}
        choices={row['id']:row for row in templates[kind]}
        for record in group['records']:
            if record['kind']!=native_kind or not record['layout_supported']:continue
            raw=files[record['member']];paired=paired_values(raw,kind=kind)
            definition=copy.deepcopy(definitions.get(record['id']))
            values={};issues=[]
            if definition and definition['kind']=='template':
                template=choices.get(definition['id'])
                if template is None or template['sha256']!=definition['sha256']:
                    issues.append('原版模板缺失或已改变，请重新选择并核对。')
                else:values={**template['values'],key:record['id']}
            elif definition:values=definition['values']
            for field,value in paired.items():
                if field in values and values[field]!=value:issues.append('两端字段不一致：'+_LABELS[field])
            fields=[]
            for name,sql_type,size,_ in schema:
                field={'name':name,'label':_LABELS[name],'sql_type':sql_type,'required':True,
                       'readonly':name in paired,'value':paired.get(name,values.get(name)),
                       'source':'client-record' if name in paired else (
                           'stock-template' if definition and definition['kind']=='template' and name in values else
                           definition['origin'] if definition and name in values else 'missing')}
                if sql_type in _INTEGER_KINDS:field.update(type='integer',minimum=_INTEGER_KINDS[sql_type][0],maximum=_INTEGER_KINDS[sql_type][1])
                elif sql_type=='real':field.update(type='number',minimum=-3.4e38,maximum=3.4e38)
                else:field.update(type='string',max_length=size)
                fields.append(field)
            missing=[name for name in columns if name not in values and name not in paired]
            result.append({'kind':kind,'id':record['id'],'name':record['name'],'definition':definition,
                'fields':fields,'missing_fields':missing,'issues':issues,
                'complete':bool(definition and definition['kind']!='draft' and not missing and not issues),
                'note':'服务端战斗模式独立填写；不能从客户端技能末字节调度值推断。' if kind=='skill' else
                       '浮游字段使用目标服务器的有限整数类型；越界数据需要适配，不能截断。'})
    return result


def edit(bindings,files,*,manifest_path,kind,identity,definition,templates):
    groups=validate_project_bindings(bindings,files=files)
    group=next((row for row in groups if row['binding']['manifest_path']==manifest_path),None)
    if group is None:raise ValueError('配套资料组不存在，请刷新后重试')
    schema,section,_,columns,key,_,_,_=_spec(kind)
    native_kind=13 if kind=='skill' else 11
    record=next((row for row in group['records'] if row['kind']==native_kind and row['id']==identity and row['layout_supported']),None)
    if type(identity)is not int or record is None:raise ValueError('请选择本组已支持布局的原生定义')
    raw=files[record['member']];paired=paired_values(raw,kind=kind)
    definition=copy.deepcopy(definition)
    if not isinstance(definition,dict):raise ValueError('请提供服务端定义')
    if definition.get('kind')=='template':
        choice=next((row for row in templates[kind] if row['id']==definition.get('id')),None)
        if choice is None or definition.get('sha256')!=choice['sha256']:
            raise ValueError('原版模板缺失或已改变，请刷新后重选')
        values={**choice['values'],key:identity}
        if any(values[name]!=value for name,value in paired.items()):
            raise ValueError('所选模板与客户端共享字段不一致，请选兼容模板或明确编写服务端定义')
    elif definition.get('kind') in ('draft','explicit'):
        values=normalize_partial_row(definition.get('values'),kind=kind)
        if any(name in values and values[name]!=value for name,value in paired.items()):
            raise ValueError('客户端共享字段只读，不能只修改服务端一边')
        values.update(paired)
        if set(values)==set(columns):
            definition['kind']='explicit';values=normalize_row(values,kind=kind)
        elif definition['kind']=='explicit':raise ValueError('完整服务端定义仍缺少必填字段')
        definition['values']=values
    else:raise ValueError('请选择原版模板或手动填写服务端定义')
    part='server_skills' if kind=='skill' else 'server_funnels'
    manifest=copy.deepcopy(group['manifest']);output=dict(files)
    data=copy.deepcopy(group['components'].get(part,{'schema':schema,'owner':manifest['owner'],'revision':'0'*64,section:[]}))
    data[section]=[row for row in data[section] if row['id']!=identity]+[
        {'id':identity,'client_sha256':record['sha256'],'definition':definition}]
    data[section].sort(key=lambda row:row['id'])
    data['revision']=hashlib.sha256(canonical({name:value for name,value in data.items() if name!='revision'}).encode()).hexdigest()
    records={row['id']:files[row['member']] for row in group['records'] if row['kind']==native_kind and row['layout_supported']}
    validate_manifest(data,records,kind=kind,authoring=True)
    prefix='native-edits/'+hashlib.sha256(manifest_path.encode()).hexdigest()+'/'
    evidence=manifest.setdefault('evidence',[])
    origin=prefix+'source-manifest.json'
    if origin not in output:
        output[origin]=files[manifest_path]
        evidence.append({'member':origin,'sha256':hashlib.sha256(output[origin]).hexdigest()})
    def replace(component,value):
        target=prefix+component+'.json';previous=manifest.get(component)
        if target in output and (previous is None or previous['manifest_path']!=target):
            raise ValueError('服务端定义的制作成员路径冲突')
        if previous and previous['manifest_path']!=target:
            evidence.append({'member':previous['manifest_path'],'sha256':previous['manifest_sha256']})
        output[target]=canonical(value).encode('utf-8')
        manifest[component]={'manifest_path':target,'manifest_sha256':hashlib.sha256(output[target]).hexdigest()}
    replace(part,data)
    if kind=='skill' and 'skill_identities' in group['components']:
        request=copy.deepcopy(group['components']['skill_identities'])
        bound=next((row for row in request['bindings'] if row['target']==identity),None)
        if bound and (definition.get('kind')!='template' or definition.get('id')!=bound['source']):
            raise ValueError('该技能已有原生行为绑定，须保留对应模板；不能通过改写定义丢失行为')
        request['revision']=data['revision'];replace('skill_identities',request)
    manifest['revision']=hashlib.sha256(canonical({name:value for name,value in manifest.items() if name!='revision'}).encode()).hexdigest()
    output[manifest_path]=canonical(manifest).encode('utf-8')
    refs=[dict(row) for row in bindings]
    next(row for row in refs if row['manifest_path']==manifest_path)['manifest_sha256']=hashlib.sha256(output[manifest_path]).hexdigest()
    validate_project_bindings(refs,files=output)
    return refs,output
