"""Sealed inputs for the original unit encyclopedia and capsule preview cases."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path

INPUT = 'inputs/unit-scenario.json'
SETTINGS = {'UnitPagesOnly':'0','UnitPresentations':'0','UnitPortraits':'0',
    'CaptureNativeFrames':'0','CollectionPreviewOnly':'0',
    'UnitAcquisitionKind':'0','UnitAcquisitionMachine':'0','UnitAcquisitionUnit':'0'}
INDEX_CASES = ('pages.unit_index.list','pages.unit_index.detail',
    'pages.unit_index.alternate','pages.unit_index.base','pages.unit_index.close')
PORTRAIT_CASES = ('pages.unit_index.pose_wait','pages.unit_index.pose_ready')

def validate(value, plan):
    if not isinstance(value,dict) or set(value)-{'schema','capture_frames','portraits','acquisition','collection_preview'}:
        raise ValueError('invalid native unit scenario fields')
    if type(value.get('schema')) is not int or value['schema']!=1:
        raise ValueError('unsupported native unit scenario schema')
    if (not plan.get('pages_only') or not plan.get('play_unit_uid') or
        any(plan.get(k) for k in ('collection_draw','bootstrap_only','friends_only','room_titles_only','mission_mode')) or
        any(k.startswith('shop_') for k in plan)):
        raise ValueError('native unit scenarios require pages_only and an existing owned unit UID')
    result={'schema':1}
    for field in ('capture_frames','portraits','collection_preview'):
        flag=value.get(field,False)
        if type(flag) is not bool:raise ValueError('native unit scenario flags must be boolean')
        result[field]=flag
    if result['portraits'] and not result['capture_frames']:
        raise ValueError('native portraits require genuine frame capture')
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
    if value.get('acquisition'):
        a=value['acquisition'];result.update(UnitAcquisitionKind=str(a['kind']),
            UnitAcquisitionMachine=str(a['machine_id']),UnitAcquisitionUnit=str(a['unit_id']))
    return result

def cases(value):
    result=('framework.bootstrap','flow.login','flow.onboarding','setup.active_unit')+INDEX_CASES[:2]
    if value['portraits']:result+=PORTRAIT_CASES
    result+=INDEX_CASES[2:]
    if value.get('acquisition'):result+=('catalog.capsule','pages.unit_acquisition')
    if value['collection_preview']:result+=('catalog.capsule','pages.collection.preview')
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
        if config.get('settings',key,fallback=None)!=expected:
            raise ValueError('native unit observer settings disagree with sealed scenario: '+key)
    return {'unit_scenario':value,'unit_scenario_sha256':digest}
