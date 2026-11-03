"""Typed blueprint authoring declarations, independent of native registration.

Native table layout compilation is a separate adapter. These declarations
retain unknowns as required fields and distinguish recovered and authored data.
"""
from __future__ import annotations

import copy
import re


PROTOCOL='sdgo.unit-blueprint'
SCHEMA_VERSION=1
# Enabled only when compiler, native consumer and guarded repository share it.
NATIVE_REGISTRATION_SUPPORTED=False
SOURCES={'source-recovered','authored','unknown'}
RECIPE_FIRST=209000
RECIPE_LAST=209999
CATEGORY_FIRST=1599000
CATEGORY_LAST=1599999
FIELDS={'schema_version','id','enabled','name','source','recipe_template_id',
        'item_template_id','item_category_id','output_unit_id','materials',
        'minimum_player_level','repay_point','encyclopedia_primary','internal_name'}


def _number(value,label,maximum=2_147_483_647,minimum=0,nullable=True):
    if value is None and nullable:return None
    if type(value) is not int or not minimum<=value<=maximum:
        raise ValueError(f'{label} 必须是 {minimum}..{maximum} 的整数')
    return value


def normalize(value,*,root_unit_id):
    """Validate declaration shape while allowing missing draft values."""
    if not isinstance(value,dict) or set(value)-FIELDS:
        raise ValueError('设计图声明包含未知字段')
    if value.get('schema_version')!=SCHEMA_VERSION:
        raise ValueError('不支持的设计图声明版本')
    result=copy.deepcopy(value)
    if type(result.get('enabled',True)) is not bool:raise ValueError('设计图启用状态无效')
    result.setdefault('enabled',True)
    if type(result.get('encyclopedia_primary',False)) is not bool:raise ValueError('图鉴主设计图标记无效')
    result.setdefault('encyclopedia_primary',False)
    name=result.get('name','')
    if not isinstance(name,str) or len(name)>48 or any(ord(c)<32 for c in name):
        raise ValueError('设计图名称须为48字符以内的可显示文本')
    result['name']=name.strip()
    internal=result.get('internal_name','')
    if not isinstance(internal,str) or len(internal)>50 or not internal.isascii() or any(ord(c)<32 for c in internal):
        raise ValueError('图纸服务端内部名称须为50字符以内ASCII文本')
    result['internal_name']=internal.strip()
    source=result.get('source',{'kind':'unknown'})
    if not isinstance(source,dict) or set(source)-{'kind','domain','record_id','record_sha256','note'}:
        raise ValueError('设计图来源声明无效')
    if source.get('kind') not in SOURCES:raise ValueError('设计图来源须为恢复、作者设定或未知')
    for key in ('domain','note'):
        v=source.get(key)
        if v is not None and (not isinstance(v,str) or len(v)>512 or any(ord(c)<32 for c in v)):
            raise ValueError('设计图来源说明无效')
    if 'record_id' in source:_number(source['record_id'],'原始设计图编号',minimum=1,nullable=False)
    if 'record_sha256' in source and (not isinstance(source['record_sha256'],str) or
            not re.fullmatch('[0-9a-f]{64}',source['record_sha256'])):
        raise ValueError('原始设计图记录SHA-256无效')
    result['source']=source
    for key in ('id','recipe_template_id','item_template_id','item_category_id'):
        result[key]=_number(result.get(key),key,minimum=1)
    output=_number(result.get('output_unit_id'), '设计图产物',minimum=1)
    if output is not None and output!=root_unit_id:
        raise ValueError('设计图产物必须是本机体族的机库根形态')
    result['output_unit_id']=output
    result['minimum_player_level']=_number(result.get('minimum_player_level'),'玩家最低等级',255)
    result['repay_point']=_number(result.get('repay_point'),'RepayPoint')
    materials=result.get('materials',[])
    if not isinstance(materials,list) or len(materials)>7:raise ValueError('原生设计图最多7个材料槽')
    seen=set();normalized=[]
    for slot,row in enumerate(materials,1):
        if not isinstance(row,dict) or set(row)-{'unit_id','minimum_level'}:
            raise ValueError('设计图材料字段无效')
        uid=_number(row.get('unit_id'),f'材料{slot}',minimum=1)
        level=_number(row.get('minimum_level'),f'材料{slot}最低等级',255,minimum=1)
        if uid is not None:
            if uid in seen:raise ValueError('当前设计图适配器要求各材料机体编号不同')
            if uid==output:raise ValueError('设计图不能将产物本身作为材料')
            seen.add(uid)
        normalized.append({'unit_id':uid,'minimum_level':level})
    result['materials']=normalized
    return result


def primary_id(recipes):
    active=[r for r in recipes if r.get('enabled',True)]
    selected=[r for r in active if r.get('encyclopedia_primary',False)]
    if len(active)==1:return active[0].get('id')
    if not active:return 0
    if len(selected)!=1:raise ValueError('有多份启用设计图时，请明确选择一份图鉴主设计图')
    return selected[0].get('id')


def from_record(decoded,*,domain):
    return {'schema_version':1,'id':None,'enabled':True,'name':decoded['name'],'internal_name':'',
        'source':{'kind':'source-recovered','domain':domain,'record_id':decoded['id'],
                  'record_sha256':decoded['source_sha256']},
        'recipe_template_id':None,'item_template_id':None,'item_category_id':None,
        'output_unit_id':decoded['output_unit_id'],'materials':copy.deepcopy(decoded['materials']),
        'minimum_player_level':decoded['minimum_player_level'],'repay_point':decoded['repay_point'],
        'encyclopedia_primary':False}


def requirements(recipe,*,root_unit_id,unit_ids=None):
    """List missing author inputs without inventing server or combat values."""
    r=normalize(recipe,root_unit_id=root_unit_id)
    if not r['enabled']:return []
    issues=[]
    def need(code,message,field):issues.append({'code':code,'message':message,'field':field,'level':'required'})
    for field,label in [('id','目标设计图编号'),('name','设计图名称'),('internal_name','图纸服务端内部名称'),('recipe_template_id','原生设计图显示模板'),
        ('item_template_id','原生图纸物品模板'),('item_category_id','目标图纸物品类别'),
        ('output_unit_id','产物根形态'),('minimum_player_level','玩家最低等级'),('repay_point','原生RepayPoint')]:
        if r.get(field) in (None,''):need('blueprint_field','请填写'+label,'blueprints.'+field)
    if r['source']['kind']=='unknown':need('blueprint_source','请明确恢复来源或标记为作者设定','blueprints.source')
    elif r['source']['kind']=='source-recovered':
        for key in ('domain','record_id','record_sha256'):
            if not r['source'].get(key):need('blueprint_source','恢复的设计图缺少'+key,'blueprints.source.'+key)
    if not 3<=len(r['materials'])<=5:
        need('blueprint_material','当前原生合成界面支持3至5个材料；6/7槽原始声明仅保留为未开放的来源数据','blueprints.materials')
    for i,row in enumerate(r['materials']):
        for field in ('unit_id','minimum_level'):
            if row[field] is None:need('blueprint_material','请补齐材料和最低等级',f'blueprints.materials.{i}.{field}')
        if unit_ids is not None and row['unit_id'] is not None and row['unit_id'] not in unit_ids:
            need('blueprint_material','材料机体尚未在宿主登记',f'blueprints.materials.{i}.unit_id')
    return issues


def native_material_hash(unit_ids):
    """Exact sp_GetNextUnitHash arithmetic, with its signed-INT products.

    sp_MakeMixTreeList chains this over each nonzero material. Do not invoke
    that global rebuild procedure when registering an owner-scoped recipe.
    """
    total=0
    for unit in unit_ids:
        unit=_number(unit,'材料机体编号',minimum=1,nullable=False)
        square=unit*unit;scaled=unit*10000
        if max(square,scaled)>2_147_483_647:
            raise ValueError('材料编号超过原生sp_GetNextUnitHash的INT乘法范围')
        total+=square+scaled
    return total


def server_rows(recipe,*,root_unit_id):
    """Build exactly the three native non-player definition rows/groups."""
    r=normalize(recipe,root_unit_id=root_unit_id)
    if not r['enabled']:raise ValueError('已停用的设计图不能作为新增登记计划')
    issues=requirements(r,root_unit_id=root_unit_id)
    if issues:raise ValueError(issues[0]['message'])
    material_hash=native_material_hash(m['unit_id'] for m in r['materials'])
    row={'Idx':r['id'],'OutUnitID':r['output_unit_id'],
         'MinUserLevel':r['minimum_player_level'],'RepayPoint':r['repay_point']}
    for i in range(1,8):
        material=r['materials'][i-1] if i<=len(r['materials']) else None
        row['UnitID'+str(i)]=material['unit_id'] if material else 0
        row['UnitLevel'+str(i)]=material['minimum_level'] if material else 0
    return {'recipe':row,'info':{'ItemID':r['id'],'Hash':material_hash,'OutUnitID':r['output_unit_id']},
            'materials':[{'ItemID':r['id'],'UnitID':m['unit_id'],'UnitLv':m['minimum_level']} for m in r['materials']]}
