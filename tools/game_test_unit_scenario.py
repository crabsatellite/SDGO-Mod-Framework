"""Sealed inputs for the original unit encyclopedia and capsule preview cases."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path

INPUT = 'inputs/unit-scenario.json'
SETTINGS = {'UnitPagesOnly':'0','UnitPresentations':'0','UnitPortraits':'0',
    'CaptureNativeFrames':'0','CollectionPreviewOnly':'0',
    'UnitAcquisitionKind':'0','UnitAcquisitionMachine':'0','UnitAcquisitionUnit':'0',
    'CapsulePreviewMachine':'990001','CapsulePreviewPrice':'100000','CapsulePreviewCurrency':'0',
    'ActivityDrawOnce':'0','ActivityRewardCount':'0','BlueprintRecipe':'0','BlueprintBookUid':'0',
    'BlueprintMaterialCount':'0','BlueprintSynthesize':'0','BlueprintOutputUnit':'0'}
INDEX_CASES = ('pages.unit_index.list','pages.unit_index.detail',
    'pages.unit_index.alternate','pages.unit_index.base','pages.unit_index.close')
PORTRAIT_CASES = ('pages.unit_index.pose_wait','pages.unit_index.pose_ready')

def validate(value, plan):
    if not isinstance(value,dict) or set(value)-{'schema','capture_frames','portraits','acquisition','collection_preview','activity_target','activity_draw','reward_pairs','blueprint'}:
        raise ValueError('invalid native unit scenario fields')
    if type(value.get('schema')) is not int or value['schema']!=1:
        raise ValueError('unsupported native unit scenario schema')
    if (not plan.get('pages_only') or not plan.get('play_unit_uid') or
        any(plan.get(k) for k in ('collection_draw','bootstrap_only','friends_only','room_titles_only','mission_mode')) or
        any(k.startswith('shop_') for k in plan)):
        raise ValueError('native unit scenarios require pages_only and an existing owned unit UID')
    result={'schema':1}
    for field in ('capture_frames','portraits','collection_preview','activity_draw'):
        flag=value.get(field,False)
        if type(flag) is not bool:raise ValueError('native unit scenario flags must be boolean')
        if field!='activity_draw' or field in value:result[field]=flag
    if result['portraits'] and not result['capture_frames']:
        raise ValueError('native portraits require genuine frame capture')
    if 'blueprint' in value:
        blueprint=value['blueprint']
        if (not isinstance(blueprint,dict) or not {'recipe_id','book_uid'}<=set(blueprint) or
            set(blueprint)-{'recipe_id','book_uid','materials','synthesize','output_unit_id'} or
            type(blueprint['recipe_id']) is not int or not 0<blueprint['recipe_id']<2**31 or
            type(blueprint['book_uid']) is not int or not 0<blueprint['book_uid']<2**63):
            raise ValueError('blueprint preview needs an exact recipe and owned book identity')
        if (plan.get('page_group') or result['portraits'] or result['collection_preview'] or result.get('activity_draw') or
            any(field in value for field in ('acquisition','activity_target','reward_pairs'))):
            raise ValueError('blueprint preview cannot be mixed with other unit-page scopes')
        result['blueprint']=dict(blueprint)
        if 'materials' in blueprint:
            materials=blueprint['materials']
            if not isinstance(materials,list) or not 3<=len(materials)<=5:
                raise ValueError('native blueprint material selection needs three to five owned instances')
            seen=set()
            for material in materials:
                if (not isinstance(material,dict) or set(material)!={'unit_id','uid'} or
                    type(material['unit_id']) is not int or not 0<material['unit_id']<2**31 or
                    type(material['uid']) is not int or not 0<material['uid']<2**63 or material['uid'] in seen):
                    raise ValueError('native blueprint material identities must be exact and distinct')
                seen.add(material['uid'])
            result['blueprint']['materials']=[dict(m) for m in materials]
        if 'synthesize' in blueprint or 'output_unit_id' in blueprint:
            if (blueprint.get('synthesize') is not True or not blueprint.get('materials') or
                type(blueprint.get('output_unit_id')) is not int or not 0<blueprint['output_unit_id']<2**31):
                raise ValueError('native blueprint synthesis needs explicit consent, materials and expected output')
    if 'activity_target' in value:
        target=value['activity_target']
        if not result['collection_preview'] or not isinstance(target,dict) or set(target)!={'machine_id','price','currency'}:
            raise ValueError('activity target needs an explicit non-purchasing preview and exact identity/price/currency')
        if any(type(target[key]) is not int or not 0<target[key]<2**31 for key in ('machine_id','price')) or type(target['currency']) is not int or target['currency'] not in (0,1,2):
            raise ValueError('native activity preview identity, price or currency is invalid')
        result['activity_target']=dict(target)
    pairs=value.get('reward_pairs',[])
    if result.get('activity_draw'):
        if 'activity_target' not in result or not isinstance(pairs,list) or not 2<=len(pairs)<=128:
            raise ValueError('one native activity draw needs its exact target and typed reward pairs')
        seen=set()
        for pair in pairs:
            if not isinstance(pair,list) or len(pair)!=2 or any(type(n) is not int or not 0<n<2**31 for n in pair) or pair[0]//100000 not in (11,15,22) or tuple(pair) in seen:
                raise ValueError('activity reward pair is invalid or duplicated')
            seen.add(tuple(pair))
        result['reward_pairs']=[list(pair) for pair in pairs]
    elif pairs:raise ValueError('reward expectations require the explicit native draw scope')
    if 'acquisition' in value:
        item=value['acquisition']
        if not isinstance(item,dict) or set(item)!={'kind','machine_id','unit_id'}:
            raise ValueError('native acquisition needs kind, machine_id and unit_id')
        if any(type(v) is not int or not 0<v<2**32 for v in item.values()) or item['kind'] not in (1,3):
            raise ValueError('unsupported native acquisition identity or machine kind')
        if result['collection_preview']:raise ValueError('choose one native capsule preview scope')
        result['acquisition']=dict(item)
    return result

def encoded(value):
    return json.dumps(value,sort_keys=True,separators=(',',':')).encode('utf-8')

def bind(value,plan):
    value=validate(value,plan)
    return {'unit_scenario':value,'unit_scenario_sha256':hashlib.sha256(encoded(value)).hexdigest()}

def settings(value=None):
    result=dict(SETTINGS)
    if value is None:return result
    result.update(UnitPagesOnly='1',UnitPresentations='1',
        UnitPortraits=str(int(value['portraits'])),CaptureNativeFrames=str(int(value['capture_frames'])),
        CollectionPreviewOnly=str(int(value['collection_preview'])))
    if value.get('blueprint'):
        result.update(UnitPresentations='0',BlueprintRecipe=str(value['blueprint']['recipe_id']),
                      BlueprintBookUid=str(value['blueprint']['book_uid']))
        materials=value['blueprint'].get('materials',[])
        result['BlueprintMaterialCount']=str(len(materials))
        result['BlueprintSynthesize']=str(int(value['blueprint'].get('synthesize',False)))
        result['BlueprintOutputUnit']=str(value['blueprint'].get('output_unit_id',0))
        for index,material in enumerate(materials):
            result['BlueprintMaterialUnit'+str(index)]=str(material['unit_id'])
            result['BlueprintMaterialUid'+str(index)]=str(material['uid'])
    if value.get('acquisition'):
        a=value['acquisition'];result.update(UnitAcquisitionKind=str(a['kind']),
            UnitAcquisitionMachine=str(a['machine_id']),UnitAcquisitionUnit=str(a['unit_id']))
    if value.get('activity_target'):
        a=value['activity_target'];result.update(CapsulePreviewMachine=str(a['machine_id']),
            CapsulePreviewPrice=str(a['price']),CapsulePreviewCurrency=str(a['currency']))
    if value.get('activity_draw'):
        result['ActivityDrawOnce']='1';result['ActivityRewardCount']=str(len(value['reward_pairs']))
        for index,(category,amount) in enumerate(value['reward_pairs']):
            result['ActivityRewardCategory'+str(index)]=str(category)
            result['ActivityRewardAmount'+str(index)]=str(amount)
    return result

def cases(value):
    result=('framework.bootstrap','flow.login','flow.onboarding','setup.active_unit')+INDEX_CASES[:2]
    if value.get('blueprint'):
        return result[:4]+('pages.blueprint.book','pages.blueprint.recipe')+(
            ('pages.blueprint.materials',) if value['blueprint'].get('materials') else ())+(
            ('pages.blueprint.synthesis',) if value['blueprint'].get('synthesize') else ())
    if value['portraits']:result+=PORTRAIT_CASES
    result+=INDEX_CASES[2:]
    if value.get('acquisition'):result+=('catalog.capsule','pages.unit_acquisition')
    if value['collection_preview']:result+=('catalog.capsule','pages.collection.preview')
    if value.get('activity_draw'):result+=('pages.activity.draw',)
    return result

def read(root, digest, plan, read_ini):
    if not isinstance(digest,str) or len(digest)!=64 or any(c not in '0123456789abcdef' for c in digest):
        raise ValueError('invalid native unit scenario digest')
    root=Path(root);path=root/'sdgo/gametest'/INPUT
    if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()) or path.stat().st_size>4096:
        raise ValueError('native unit scenario input escapes its copied client or exceeds its budget')
    raw=path.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=digest:raise ValueError('native unit scenario input hash changed')
    value=validate(json.loads(raw),plan)
    if raw!=encoded(value):raise ValueError('native unit scenario must use its canonical input bytes')
    config=read_ini(root/'sdgo/mods/game_test_client/mod.ini')
    for key,expected in settings(value).items():
        fallback=SETTINGS.get(key) if key.startswith(('CapsulePreview','Activity','Blueprint')) else None
        if config.get('settings',key,fallback=fallback)!=expected:
            raise ValueError('native unit observer settings disagree with sealed scenario: '+key)
    return {'unit_scenario':value,'unit_scenario_sha256':digest}
