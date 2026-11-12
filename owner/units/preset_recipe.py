"""Compose public, data-only recipes against the selected target's originals.

Recipes contain template identities/hashes and typed edits, never original
models, textures, records, executables, connection details or player data.
"""
from __future__ import annotations
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
from . import codec,model
from .compiler import wchar
from .server_dependencies import _SHARED,normalize_row

SCHEMA='sdgo.unit-default-presets/1'

def load(path=None):
    path=Path(path) if path else Path(__file__).resolve().parents[2]/'mods/unit_catalog/default-presets.json'
    return validate(json.loads(path.read_bytes()))


def validate(value):
    """Reject incomplete recipes before reading originals or creating output."""
    from . import stages
    from .server_dependencies import normalize_partial_row
    from .skill_slots import ordinary
    fields={'schema','recipe_version','enabled_by_default','owner','host','host_sha256','templates','skills','presets','stage_rules','acquisition'}
    if not isinstance(value,dict) or set(value)!=fields or value['schema']!=SCHEMA or type(value['recipe_version']) is not int or value['recipe_version']!=1:
        raise ValueError('默认机体预设配方版本无效')
    if type(value['enabled_by_default']) is not bool:
        raise ValueError('默认机体预设开关无效')
    if not isinstance(value['owner'],str) or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,63}',value['owner']):
        raise ValueError('默认机体预设所有者无效')
    if not isinstance(value['host'],str) or not re.fullmatch('GOnline-[0-9A-F]{8}-[0-9A-F]{8}',value['host']):
        raise ValueError('默认机体预设客户端标识无效')
    if not isinstance(value['host_sha256'],str) or not re.fullmatch('[0-9a-f]{64}',value['host_sha256']):
        raise ValueError('默认机体预设客户端摘要无效')
    if value['acquisition']!={'creates_shop_products':False,'creates_gacha_slots':False,'grants_inventory_units':False}:
        raise ValueError('默认演示预设不能包含获取途径或发放操作')
    if not isinstance(value['presets'],list) or not 1<=len(value['presets'])<=16:raise ValueError('默认机体预设数量无效')
    def exact(row,keys):
        if not isinstance(row,dict) or set(row)!=set(keys):raise ValueError('预设条目字段无效')
    def identity(number,*,zero=False):
        if type(number) is not int or not (0 if zero else 1)<=number<=9999999:raise ValueError('预设编号无效')
        return number
    def sequence(rows,label,maximum):
        if not isinstance(rows,list) or len(rows)>maximum:raise ValueError('预设'+label+'列表无效')
        return rows
    templates=set()
    for row in sequence(value['templates'],'模板',256):
        exact(row,{'kind','id','sha256'});identity(row['id'])
        if type(row['kind']) is not int or row['kind'] not in (2,10,13,34):raise ValueError('预设模板类型无效')
        key=(row['kind'],row['id'])
        if key in templates or not isinstance(row['sha256'],str) or not re.fullmatch('[0-9a-f]{64}',row['sha256']):
            raise ValueError('预设模板重复或摘要无效')
        templates.add(key)
    records=set();skills=set();forms={};keys=set();families=set();categories=set()
    def authored(row,kind):
        number=identity(row['id'])
        if number in records:raise ValueError('预设新记录编号重复')
        if (kind,identity(row['template'])) not in templates:raise ValueError('预设缺少原版模板摘要')
        model.text(row['name'],'预设名称',48);records.add(number)
    for row in sequence(value['skills'],'技能',64):
        exact(row,{'id','template','name','internal_name','description','fields'});authored(row,13)
        model.text(row['description'],'预设技能说明',99)
        if not ordinary(row['id']):raise ValueError('预设技能编号不属于原生普通技能槽')
        if not isinstance(row['fields'],dict) or set(row['fields'])-set(_SHARED):raise ValueError('预设技能修改字段无效')
        normalize_partial_row({**row['fields'],'SkillName':row['internal_name']});skills.add(row['id'])
    for row in value['presets']:
        exact(row,{'key','family_key','name','profile','root_unit_id','item_category_id','item_template_id','forms','skills','weapons','description','gameplay_notes'})
        for key,seen in (('key',keys),('family_key',families)):
            if not isinstance(row[key],str) or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,63}',row[key]) or row[key] in seen:
                raise ValueError('预设标识无效或重复')
            seen.add(row[key])
        model.text(row['name'],'预设名称',48)
        for key in ('description','gameplay_notes'):model.text(row[key],'预设说明',2000)
        if row['profile'] not in model.PROFILES:raise ValueError('预设变形类型不支持')
        identity(row['root_unit_id']);category=identity(row['item_category_id']);identity(row['item_template_id'])
        if category in categories or category in records:raise ValueError('预设物品编号重复')
        categories.add(category)
        if not isinstance(row['skills'],list) or len(row['skills'])!=2 or any(type(n) is not int or n not in skills for n in row['skills']):
            raise ValueError('预设须引用两个已声明的普通技能')
        family_forms={}
        for form in sequence(row['forms'],'形态',16):
            exact(form,{'id','template','target','name','fields'});authored(form,2);identity(form['target'],zero=True)
            if not isinstance(form['fields'],dict) or set(form['fields'])-(set(model.UNIT_COLUMNS)-{'UnitNumber','UnitModelName','UnitNumber_Transform',*model.SKILL_REFS,'SallyReady'}):
                raise ValueError('预设机体修改字段无效')
            for key,item in form['fields'].items():model.field_value(key,item)
            family_forms[form['id']]=form;forms[form['id']]=(form,row['skills'])
        if row['root_unit_id'] not in family_forms or any(form['target'] and form['target'] not in family_forms for form in family_forms.values()):
            raise ValueError('预设变形边离开机体家族')
        for weapon in sequence(row['weapons'],'武器',64):
            exact(weapon,{'id','template','name','internal_name','fields'});authored(weapon,10)
            model.field_value('WeaponName',weapon['internal_name'])
            if not isinstance(weapon['fields'],dict) or set(weapon['fields'])-(set(model.WEAPON_COLUMNS)-{'WeaponNo','WeaponName'}):
                raise ValueError('预设武器修改字段无效')
            for key,item in weapon['fields'].items():model.field_value(key,item)
    if categories&records:raise ValueError('预设物品与原生记录编号重复')
    for rule in stages.normalize(value['stage_rules']):
        source= forms.get(rule['unit_id'])
        if not source or source[0]['target']!=rule['target_unit_id'] or rule['skill_id'] not in source[1]:
            raise ValueError('预设阶段规则与机体、技能或变形边不同')
    return value

def compose(recipe,baseline,catalog):
    """Return complete paired candidate records. This does not register anything."""
    validate(recipe)
    from .assets import _decode_gdt
    skills={struct.unpack_from('<I',raw)[0]:raw for raw in _decode_gdt(baseline.read('gdrs/0253.gdt'),1132)}
    originals={}
    for witness in recipe['templates']:
        if not isinstance(witness,dict) or set(witness)!={'kind','id','sha256'}:raise ValueError('预设模板声明无效')
        kind,identity=witness['kind'],witness['id']
        raw=skills.get(identity) if kind==13 else baseline.record({2:'unit',10:'weapon',34:'item'}[kind],identity)
        if raw is None or hashlib.sha256(raw).hexdigest()!=witness['sha256']:
            raise ValueError(f'原版模板 {kind}/{identity} 与预设支持版本不一致；没有安装任何内容')
        originals[kind,identity]=raw
    def template(kind,identity):
        raw=originals.get((kind,identity))
        if raw is None:raise ValueError('预设引用了未声明的完整原版模板')
        return raw
    skill_records={};skill_rows={}
    for entry in recipe['skills']:
        identity=entry['id'];raw=bytearray(template(13,entry['template']))
        if baseline.any_id(identity):raise ValueError('预设技能编号已被目标原版占用：'+str(identity))
        row=copy.deepcopy(catalog['skills'][entry['template']]);row.update(entry['fields'])
        row.update(SkillID=identity,SkillName=entry['internal_name'],BattleMode=0)
        row=normalize_row(row)
        struct.pack_into('<I',raw,0,identity);wchar(raw,5,50,entry['name']);raw[1131]=0
        # Native unit details read the 100-wide UTF-16 field at +0x213.
        # Keep the second native description in sync with the authored effect.
        for offset in (531,731):wchar(raw,offset,100,entry['description'])
        for name,(offset,primitive) in _SHARED.items():struct.pack_into('<'+primitive,raw,offset,row[name])
        skill_records[identity]=bytes(raw);skill_rows[identity]=row
    families=[]
    for entry in recipe['presets']:
        if entry['profile'] not in model.PROFILES:raise ValueError('预设变形类型不支持')
        units=[];weapons=[];records={}
        for source in entry['forms']:
            identity=source['id'];raw=template(2,source['template'])
            if baseline.any_id(identity):raise ValueError('预设机体编号已被目标原版占用：'+str(identity))
            row=copy.deepcopy(catalog['units'][source['template']]);row.update(source['fields'])
            row.update(UnitNumber=identity,UnitModelName='Demo_'+str(identity),UnitNumber_Transform=source['target'],
                SkillNumber_1=entry['skills'][0],SkillNumber_2=entry['skills'][1],SkillNumber_3=0,
                SallyReady=int(identity==entry['root_unit_id']))
            for name,value in row.items():model.field_value(name,value)
            raw=bytearray(codec.patch_unit(raw,{name:value for name,value in row.items() if name in codec.UNIT_FIELDS and codec.UNIT_FIELDS[name]['type']!='utf16le'}))
            raw[320]=0
            for offset in (5,105,205):wchar(raw,offset,50,source['name'])
            units.append(row);records[2,identity]=bytes(raw)
        for source in entry['weapons']:
            identity=source['id'];raw=template(10,source['template'])
            if baseline.any_id(identity):raise ValueError('预设武器编号已被目标原版占用：'+str(identity))
            row=copy.deepcopy(catalog['weapons'][source['template']]);row.update(source['fields'])
            row.update(WeaponNo=identity,WeaponName=source['internal_name'])
            for name,value in row.items():model.field_value(name,value)
            raw=bytearray(codec.patch_weapon(raw,{name:value for name,value in row.items() if name!='WeaponName'}))
            wchar(raw,5,50,source['name']);weapons.append(row);records[10,identity]=bytes(raw)
        families.append({'recipe':copy.deepcopy(entry),'units':units,'weapons':weapons,'records':records})
    return {'families':families,'skill_records':skill_records,'skill_rows':skill_rows,'stage_rules':copy.deepcopy(recipe['stage_rules'])}
