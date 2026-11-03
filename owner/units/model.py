"""Typed authoring state. Missing values remain missing until an owner supplies them."""
from __future__ import annotations

import copy
import hashlib
import json
import math
import re

UNIT_COLUMNS = (
    'UnitNumber Influence1 Influence2 UnitModelName UnitGradeNumber UnitGradeNumber2 MasteredWeaponType '
    'BattleCost Star UnitNumber_Transform HP Shield ShieldType ShieldRate DashType MoveLevel DashLevel '
    'QuickStepLevel MotivePower BoosterIncrease BoosterDecrease FatalAttackIncrease AdaptEnvironType '
    'AdaptEnvironSpace AdaptEnvironGround AdaptEnvironUnderwater AdaptEnvironDesert AdaptEnvironSky Sight '
    'SkillNumber_1 SkillNumber_2 SkillNumber_3 StrikingPower DefensivePower MasteredWeaponIX WeaponNumber_1 '
    'WeaponNumber_2 WeaponNumber_3 WeaponNumber_FatalAttack ViewOrder MaxEnergyAmount MaxEnergyCost '
    'EnergyDecAmount UnitGrowIndex DailyLimit MonthLimit UserGradeLimit SallyReady CollectionUnit UnitGraph'
).split()
WEAPON_COLUMNS = (
    'WeaponNo WeaponName AttackStyle RangeType HitType DamageSize SpeedRate RangeSize AttackSpeed '
    'ExtraAttackType BulletRemain ReloadTime PushDist CoolTime SpecialFeature PushType StunBase '
    'StunFinal Pierce PierceRate MovingShot MaxCombo FunnelID'
).split()
WEAPON_REFS = ('WeaponNumber_1', 'WeaponNumber_2', 'WeaponNumber_3', 'WeaponNumber_FatalAttack')
SKILL_REFS = ('SkillNumber_1', 'SkillNumber_2', 'SkillNumber_3')
PROFILES = {'native-single-form-v1', 'native-linked-forms-v1'}


class RevisionConflict(ValueError):
    pass

LABELS = {
    'UnitNumber':'机体编号','UnitModelName':'服务端内部名称（ASCII）','UnitGradeNumber':'等级主类','UnitGradeNumber2':'等级子类',
    'MasteredWeaponType':'机体属性类型','BattleCost':'对战消耗','Star':'卡组点数','UnitNumber_Transform':'变形目标编号',
    'HP':'耐久','Shield':'盾耐久','ShieldType':'盾类型','ShieldRate':'盾防御比例','Influence1':'势力1','Influence2':'势力2',
    'DashType':'推进类型','MoveLevel':'移动等级','DashLevel':'推进等级','QuickStepLevel':'横移等级','MotivePower':'机动力',
    'BoosterIncrease':'推进恢复','BoosterDecrease':'推进消耗','FatalAttackIncrease':'必杀增长','Sight':'视距',
    'SkillNumber_1':'技能1','SkillNumber_2':'技能2','SkillNumber_3':'技能3','StrikingPower':'攻击力','DefensivePower':'防御力',
    'MasteredWeaponIX':'属性索引','WeaponNumber_1':'1号武器','WeaponNumber_2':'2号武器','WeaponNumber_3':'3号武器',
    'WeaponNumber_FatalAttack':'必杀武器','ViewOrder':'显示排序','MaxEnergyAmount':'最大能源','MaxEnergyCost':'最大补充成本',
    'EnergyDecAmount':'能源消耗','UnitGrowIndex':'成长配置编号','DailyLimit':'每日限制','MonthLimit':'每月限制',
    'UserGradeLimit':'玩家等级限制','SallyReady':'允许直接出击','CollectionUnit':'收藏标记','UnitGraph':'属性图编号',
    'WeaponNo':'武器编号','WeaponName':'服务端武器内部名称（ASCII）','AttackStyle':'攻击形式','RangeType':'射程类型','HitType':'命中类型',
    'DamageSize':'伤害','SpeedRate':'速度倍率','RangeSize':'射程','AttackSpeed':'攻击速度','ExtraAttackType':'附加攻击类型',
    'BulletRemain':'弹量','ReloadTime':'装填','PushDist':'击退距离','CoolTime':'冷却','SpecialFeature':'特殊属性',
    'PushType':'击退类型','StunBase':'基础硬直','StunFinal':'最终硬直','Pierce':'贯穿','PierceRate':'贯穿倍率',
    'MovingShot':'移动射击','MaxCombo':'连击数','FunnelID':'浮游数据编号',
}
BYTE_FIELDS = set('Influence1 Influence2 UnitGradeNumber UnitGradeNumber2 MasteredWeaponType Star ShieldType DashType MoveLevel DashLevel QuickStepLevel MotivePower AdaptEnvironType SallyReady CollectionUnit AttackStyle RangeType HitType ExtraAttackType PushType Pierce PierceRate MovingShot MaxCombo'.split())
WORD_FIELDS = set('BattleCost BoosterIncrease BoosterDecrease FatalAttackIncrease Sight StrikingPower DefensivePower SpeedRate BulletRemain ReloadTime PushDist CoolTime StunBase StunFinal'.split())
FLOAT_FIELDS = {'MaxEnergyAmount','MaxEnergyCost','AttackSpeed'}
SIGNED_FIELDS = {name for name in UNIT_COLUMNS if name.startswith('AdaptEnviron') and name != 'AdaptEnvironType'}
TEXT_FIELDS = {'UnitModelName','WeaponName'}


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')


def revision(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def text(value, label, maximum=48):
    if not isinstance(value, str) or not value.strip() or len(value.encode('utf-16-le'))//2 > maximum:
        raise ValueError(f'{label}必须是 1–{maximum} 个 UTF-16 字符')
    if any(ord(ch)<32 for ch in value):
        raise ValueError(label+'不能包含控制字符')
    return value.strip()


def field_value(name, value):
    if name in TEXT_FIELDS:
        value=text(value, LABELS[name])
        if not value.isascii():raise ValueError(LABELS[name]+'请使用 ASCII；中文展示名独立保留，不写入原生 varchar')
        return value
    if name in FLOAT_FIELDS:
        if isinstance(value, bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or not 0<=value<=2147483647:
            raise ValueError(LABELS.get(name,name)+'必须是有限的非负数')
        return float(value)
    if isinstance(value,bool) or not isinstance(value,int):
        raise ValueError(LABELS.get(name,name)+'必须是整数')
    low, high = (-32768,32767) if name in SIGNED_FIELDS else (0,255 if name in BYTE_FIELDS else 65535 if name in WORD_FIELDS else 2147483647)
    if not low<=value<=high:
        raise ValueError(f'{LABELS.get(name,name)}须在 {low}..{high} 之间')
    return value


def schema(kind):
    return [{'name':name,'label':LABELS.get(name,name),'type':'string' if name in TEXT_FIELDS else 'float' if name in FLOAT_FIELDS else 'int',
             'required':True,'readonly':name in {'UnitNumber','WeaponNo','SallyReady'}} for name in (UNIT_COLUMNS if kind=='unit' else WEAPON_COLUMNS)]


def new_record(record, kind):
    columns=UNIT_COLUMNS if kind=='unit' else WEAPON_COLUMNS
    known={name:copy.deepcopy(value) for name,value in record['client'].items() if name in columns}
    identity=record['id'];key='UnitNumber' if kind=='unit' else 'WeaponNo';namekey='UnitModelName' if kind=='unit' else 'WeaponName'
    known[key]=identity
    display=record['name']
    if kind=='unit':
        try:english=record['raw'][105:205].decode('utf-16-le').split('\0')[0].strip()
        except UnicodeError:english=''
        if english and english.isascii():display=english
    if display.isascii():known[namekey]=display
    else:known.pop(namekey,None)
    return {'id':identity,'name':record['name'],'client':copy.deepcopy(record['client']),'server':known,
            'source_sha':hashlib.sha256(record['raw']).hexdigest(),'sources':{name:'package' for name in known},
            'resource_refs':list(record.get('resource_refs',[])), 'template_id':None,
            **({'model':record.get('model',''),'transform_target':record.get('transform_target',0)} if kind=='unit' else {})}


def update_record(record, change, kind, catalog):
    allowed={'id','server','template_id'}
    if not isinstance(change,dict) or set(change)-allowed:
        raise ValueError('记录更新包含未知字段')
    result=copy.deepcopy(record);columns=UNIT_COLUMNS if kind=='unit' else WEAPON_COLUMNS
    if 'template_id' in change:
        template=change['template_id']
        if isinstance(template,bool) or not isinstance(template,int) or template not in catalog:
            raise ValueError('请选择有效的原版配置模板')
        for name in columns:
            if result['server'].get(name) is None and name in catalog[template]:
                result['server'][name]=copy.deepcopy(catalog[template][name]);result['sources'][name]='template:'+str(template)
        result['template_id']=template
    supplied=change.get('server',{})
    if not isinstance(supplied,dict) or set(supplied)-set(columns):
        raise ValueError('服务端定义包含未知字段')
    for name,value in supplied.items():
        if name=='SallyReady':
            continue  # Derived after all edits from the explicit inventory root.
        if name in {'UnitNumber','WeaponNo'} and value!=result['id']:
            raise ValueError('不能通过属性编辑改变原始记录编号')
        if value is None or (name in TEXT_FIELDS and value==''):
            result['server'].pop(name,None);result['sources'].pop(name,None)
        else:
            normalized=field_value(name,value)
            if name not in result['server'] or result['server'][name]!=normalized:
                result['server'][name]=normalized;result['sources'][name]='manual'
    return result


def content_revision(draft):
    return revision({key:value for key,value in draft.items() if key not in {
        'content_revision','updated','status','confirmation','release','server_result','published_server_result','published','history','requirements','can_confirm'}})


def update(draft, patch, catalog):
    if not isinstance(patch,dict) or set(patch)-{'name','root_unit_id','behavior_profile','forms','weapons','presentation','item_template_id'}:
        raise ValueError('草稿更新包含未知字段')
    result=copy.deepcopy(draft)
    if 'name' in patch:result['name']=text(patch['name'],'机体名称')
    if 'root_unit_id' in patch:
        root=patch['root_unit_id']
        published=result.get('published')
        if published and root!=published['root_unit_id']:
            raise ValueError('已发布机体不能改变机库根形态，避免改变玩家已持有机体的身份')
        if root is not None and (type(root) is not int or root not in {r['id'] for r in result['forms']}):
            raise ValueError('机库根形态必须来自这份机体包')
        result['root_unit_id']=root
        for form in result['forms']:
            if root is None:
                form['server'].pop('SallyReady',None);form['sources'].pop('SallyReady',None)
            else:
                form['server']['SallyReady']=int(form['id']==root);form['sources']['SallyReady']='explicit-root'
    if 'behavior_profile' in patch:
        if patch['behavior_profile'] is not None and patch['behavior_profile'] not in PROFILES:raise ValueError('不支持的原生变形类型')
        result['behavior_profile']=patch['behavior_profile']
    if 'item_template_id' in patch:
        identity=patch['item_template_id']
        if identity is not None and (type(identity) is not int or identity not in catalog.get('items',{}) or identity//100000!=11):
            raise ValueError('请选择有效的原生机体物品模板')
        result['item_template_id']=identity
    for key,kind,table in (('forms','unit','units'),('weapons','weapon','weapons')):
        if key not in patch:continue
        if not isinstance(patch[key],list):raise ValueError('形态和武器更新必须是列表')
        changed=set()
        for change in patch[key]:
            identity=change.get('id') if isinstance(change,dict) else None
            if identity in changed:raise ValueError('不能重复修改同一记录')
            changed.add(identity)
            index=next((i for i,r in enumerate(result[key]) if r['id']==identity),None)
            if index is None:raise ValueError('更新引用了不存在的机体或武器')
            result[key][index]=update_record(result[key][index],change,kind,catalog.get(table,{}))
    if 'presentation' in patch:
        value=patch['presentation']
        if not isinstance(value,dict) or set(value)-{'shop','capsule'}:raise ValueError('无效的展示设置')
        for role,fields in value.items():
            if not isinstance(fields,dict) or set(fields)-{'asset_sha','caption','source_mode'}:raise ValueError('无效的展示字段')
            for key,val in fields.items():
                if key=='source_mode' and val not in ('native','selected'):raise ValueError('请选择沿用原生图片或使用所选源图')
                if key=='asset_sha' and val is not None and (not isinstance(val,str) or not re.fullmatch('[0-9a-f]{64}',val)):
                    raise ValueError('请选择已上传或已解析的预览图片')
                if key=='caption':
                    if not isinstance(val,str) or len(val)>100 or any(ord(ch)<32 for ch in val):raise ValueError('展示说明最多 100 个字符且不含控制字符')
                    val=val.strip()
                result['presentation'][role][key]=val
    root=result.get('root_unit_id')
    for form in result['forms']:
        if root is not None:
            form['server']['SallyReady']=int(form['id']==root)
            form['sources']['SallyReady']='explicit-root'
        if form['server'].get('UnitNumber_Transform',form['transform_target'])!=form['transform_target']:
            raise ValueError('变形目标必须与原始形态记录一致；不能只修改服务端一边')
        for name in WEAPON_REFS:
            original=form['client'].get(name)
            if original is not None and form['server'].get(name,original)!=original:
                raise ValueError('武器引用必须与原始客户端记录一致')
    result['status']='draft';result.pop('confirmation',None);result.pop('release',None);result.pop('server_result',None)
    result['content_revision']=content_revision(result)
    return result


def definitions(plan):
    if not isinstance(plan,dict) or set(plan)-{'schema_version','root_unit_id','behavior_profile','item_template_id','units','weapons'}:
        raise ValueError('定义文件只接受 schema_version、形态/武器定义和根形态设置')
    if plan.get('schema_version')!=1:raise ValueError('不支持的定义文件版本')
    patch={key:plan[key] for key in ('root_unit_id','behavior_profile','item_template_id') if key in plan}
    for src,target,identity in (('units','forms','UnitNumber'),('weapons','weapons','WeaponNo')):
        rows=plan.get(src,[])
        if not isinstance(rows,list):raise ValueError('定义文件记录必须是列表')
        patch[target]=[{'id':row[identity],'server':row} for row in rows]
    return patch
