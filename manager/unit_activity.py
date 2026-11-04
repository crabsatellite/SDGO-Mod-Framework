"""Dedicated native activity authoring in the existing Manager provider."""
from __future__ import annotations

import copy
from pathlib import Path

from owner.units import model
from owner.units.activity_repository import ActivityRepository
from owner.units.activity_patterns import PATTERNS, build_pattern, normalize_preferences
from owner.units.repository import database_target_key
from .engines.capsules.capsule_catalog import digest, validate


def _host(provider):
    units = provider.workspace.providers['units']
    unit_ini = units.config.get('server_ini')
    capsule_ini = provider.config.get('server_ini')
    if (not unit_ini or not capsule_ini or
        database_target_key(Path(unit_ini)) != database_target_key(Path(capsule_ini))):
        raise ValueError('机体导入和活动机台必须配置同一原生服务器')
    return units, Path(capsule_ini)


def activity_state(provider):
    app = provider.get_engine()
    with app.store.lock:
        catalog, _ = app.store.read()
        referenced = {r['category_id'] for machine in
                      catalog.get('activity_machines', []) + catalog.get('activity_drafts', [])
                      for r in machine.get('pool', [])}
        items = provider.workspace.providers['units'].get_engine().repository.catalog['items'] if referenced else {}
        names = {identity: items[identity]['Name'] for identity in referenced if identity in items}
        for row in catalog.get('activity_machines', []) + catalog.get('activity_drafts', []):
            names.setdefault(row['unit_category_id'], next((asset['name'] for asset in catalog.get('project_assets', [])
                                                            if asset['key'] == row['artwork_key']), '目标机体'))
        return {'catalog_revision': digest(catalog),
                'patterns': list(PATTERNS),
                'preferences': normalize_preferences(catalog.get('activity_preferences')),
                'drafts': copy.deepcopy(catalog.get('activity_drafts', [])),
                'machines': copy.deepcopy(catalog.get('activity_machines', [])),
                'reward_names': names,
                'artwork': [{'key': r['key'], 'png_base64': r['png_base64']}
                            for r in catalog.get('project_assets', []) if r['role'] == 'activity_poster']}


def activity_items(provider, query=''):
    units = provider.workspace.providers['units']
    source = units.get_engine(); catalog = source.repository.catalog
    terms = str(query).casefold().split()
    items = []
    for identity, row in catalog['items'].items():
        if identity // 100000 not in (11, 15, 22) or row['TimeLimit'] != 0:
            continue
        text = f"{identity} {row['Name']} {row['RefIndex']}".casefold()
        if not all(word in text for word in terms):
            continue
        items.append({'category_id': identity, 'name': row['Name'],
                      'kind': {11:'unit',15:'blueprint',22:'consumable'}[identity // 100000],
                      'reference_id': row['RefIndex']})
    ini = units.config.get('server_ini')
    target = database_target_key(Path(ini)) if ini else None
    known = {row['category_id'] for row in items}
    with source.lock:
        for draft in source.read()['drafts'].values():
            family = draft.get('published'); receipt = draft.get('published_server_result', draft.get('server_result', {}))
            if (not family or draft['status']=='disabled' or not receipt.get('applied') or
                receipt.get('server_target')!=target):continue
            candidates=[{'category_id':family['category_id'],'name':family['name'],
                         'kind':'unit','reference_id':family['root_unit_id']}]
            candidates += [{'category_id':bp['item_category_id'],'name':bp['name'],
                            'kind':'blueprint','reference_id':bp['id']}
                           for bp in family.get('blueprints', []) if bp.get('enabled')]
            for row in candidates:
                text=f"{row['category_id']} {row['name']} {row['reference_id']}".casefold()
                if row['category_id'] not in known and all(word in text for word in terms):
                    items.append(row);known.add(row['category_id'])
    return {'items': sorted(items, key=lambda r: (r['kind'], r['category_id']))[:100]}


def save_activity(provider, body):
    fields = {'expected_revision', 'id', 'name', 'pattern_id', 'currency_type', 'price', 'template_machine_id', 'pool'}
    if not isinstance(body, dict) or set(body) != fields:
        raise ValueError('活动机台编辑字段无效')
    units, ini = _host(provider)
    source = units.get_engine()
    app = provider.get_engine()
    with app.store.lock, source.lock:
        catalog, raw = app.store.read()
        app.store._expect(catalog, body['expected_revision'])
        previous = next((r for r in catalog.get('activity_machines', []) if r['id'] == body['id']), None)
        draft = previous or next((r for r in catalog.get('activity_drafts', []) if r['id'] == body['id']), None)
        if draft is None:
            raise ValueError('请先从已登记的 S 级机体项目生成独立活动机草稿')
        project = next((r for r in source.read()['drafts'].values()
                        if r['family_key'] == draft['project_key'] and r['status'] != 'disabled'), None)
        family = project.get('published') if project else None
        if not family or family['root_unit_id'] != draft['unit_id']:
            raise ValueError('活动机台项目尚未登记或已改变根形态')
        receipt = project.get('published_server_result', project.get('server_result', {}))
        if not receipt.get('applied') or receipt.get('server_target') != database_target_key(ini):
            raise ValueError('该机体尚未在当前配置的原生服务器完成登记')
        root = next(r for r in family['forms'] if r['id'] == family['root_unit_id'])
        if root['server']['UnitGradeNumber'] != 4:
            raise ValueError('独立活动机台只用于 S 级及以上的机库根形态')
        artwork = next((r for r in catalog.get('project_assets', []) if r['key'] == draft['artwork_key']), None)
        if artwork is None:
            raise ValueError('请先导入该项目的活动海报')
        machine = {'schema_version': 1, 'id': draft['id'], 'project_key': draft['project_key'],
                   'pattern_id': body['pattern_id'],
                   'unit_id': family['root_unit_id'], 'unit_category_id': family['category_id'],
                   'name': body['name'], 'currency_type': body['currency_type'], 'price': body['price'],
                   'template_machine_id': body['template_machine_id'], 'pool': copy.deepcopy(body['pool']),
                   'artwork_key': artwork['key'], 'artwork_sha256': artwork['asset_sha256']}
        machine['revision'] = model.revision(machine)
        candidate = copy.deepcopy(catalog)
        active = candidate.setdefault('activity_machines', [])
        current = next((r for r in active if r['id'] == machine['id']), None)
        if current is not None:
            active[active.index(current)] = machine
        else:
            active.append(machine)
        validate(candidate)
        keys = ('schema_version', 'id', 'unit_id', 'project_key', 'revision',
                'currency_type', 'price', 'template_machine_id', 'pool')
        review = ActivityRepository(ini).preflight({key: machine[key] for key in keys})
        if candidate != catalog:
            app.store._save(candidate, raw)
        return {'catalog_revision': digest(candidate), 'machine': machine,
                'review': review, 'changes_applied': False,
                'message': '活动机台配置已保存并通过原生目录检查；编译并应用后才会上架'}


def renumber_activity(provider,body):
    """Stage an owned, unused activity-machine number change for paired apply."""
    from owner.units.activity_ids import machine_id_from_number
    if not isinstance(body,dict) or set(body)!={'expected_revision','id','number'}:
        raise ValueError('活动机台改号字段无效')
    new_id=machine_id_from_number(body['number'])
    _,ini=_host(provider);app=provider.get_engine()
    with app.store.lock:
        catalog,raw=app.store.read();app.store._expect(catalog,body['expected_revision'])
        machine=next((row for row in catalog.get('activity_machines',[]) if row['id']==body['id']),None)
        if machine is None:raise ValueError('请先保存独立活动机配置，再修改编号')
        if new_id==machine['id']:
            return {'catalog_revision':digest(catalog),'machine':copy.deepcopy(machine),'changes_applied':False,'message':'活动机台编号未改变'}
        if any(row['id']==new_id for row in catalog.get('activity_machines',[])+catalog.get('activity_drafts',[])):
            raise ValueError('该活动机台编号已被其他项目占用')
        changed=copy.deepcopy(machine);changed['id']=new_id;changed.pop('revision',None);changed['revision']=model.revision(changed)
        fields=('schema_version','id','unit_id','project_key','revision','currency_type','price','template_machine_id','pool')
        config={key:changed[key] for key in fields}
        review=ActivityRepository(ini).preflight_renumber(config,machine['id'])
        candidate=copy.deepcopy(catalog)
        candidate['activity_machines']=[changed if row['id']==machine['id'] else row for row in candidate['activity_machines']]
        for row in candidate.get('activity_drafts',[]):
            if row['id']==machine['id'] and row['project_key']==machine['project_key']:
                row['id']=new_id
        migrations=[row for row in candidate.get('activity_machine_renames',[]) if row['project_key']!=machine['project_key']]
        migrations.append({'from_id':machine['id'],'to_id':new_id,'project_key':machine['project_key'],'unit_id':machine['unit_id']})
        candidate['activity_machine_renames']=migrations
        validate(candidate);app.store._save(candidate,raw)
        return {'catalog_revision':digest(candidate),'machine':changed,'review':review,'changes_applied':False,
                'message':'活动机台编号已保存；编译并应用后同步更新客户端和服务器，保留现有奖池与素材'}


def preview_activity_pattern(provider, body):
    if not isinstance(body, dict) or set(body) != {'expected_revision', 'id', 'pattern_id'}:
        raise ValueError('活动模板请求无效')
    from owner.units.acquisition_host import read_configuration, activity_grants
    units, ini = _host(provider)
    app = provider.get_engine(); source = units.get_engine()
    with app.store.lock, source.lock:
        catalog, _ = app.store.read(); app.store._expect(catalog, body['expected_revision'])
        row = next((r for r in catalog.get('activity_machines', []) if r['id'] == body['id']), None)
        row = row or next((r for r in catalog.get('activity_drafts', []) if r['id'] == body['id']), None)
        if row is None:raise ValueError('活动机台不存在')
        project = next((r for r in source.read()['drafts'].values()
                        if r['family_key'] == row['project_key'] and r['status'] != 'disabled'), None)
        family = project.get('published') if project else None
        if not family or family['root_unit_id'] != row['unit_id']:raise ValueError('请先登记对应机体项目')
        receipt=project.get('published_server_result',project.get('server_result',{}))
        if not receipt.get('applied') or receipt.get('server_target')!=database_target_key(ini):
            raise ValueError('该机体尚未在当前配置的原生服务器完成登记')
        root = next(r for r in family['forms'] if r['id'] == family['root_unit_id'])['server']
        tables = read_configuration(ini)['tables']
        listed = {r['witness']['machine_id'] for r in activity_grants(units.config['client'], tables)}
        preset = build_pattern(body['pattern_id'],
            {'unit_id': row['unit_id'], 'rank': root['UnitGradeNumber'], 'subgrade': root['UnitGradeNumber2']},
            family['category_id'], units=list(source.repository.catalog['units'].values()),
            categories=tables[('GOnlineGame', 'tbl_ItemCategory')], prices=tables[('GOnlineGame', 'tbl_ItemMachinePrice')],
            pool=tables[('GOnlineGame', 'tbl_ItemMachineList')], listed_machines=listed,
            preferences=catalog.get('activity_preferences'),
            excluded_machine_ids={r['id'] for r in catalog.get('activity_machines',[])+catalog.get('activity_drafts',[])})
        if any(r['id'] == row['id'] for r in catalog.get('activity_machines', [])):
            preset['template_machine_id'] = row['template_machine_id']
        items=source.repository.catalog['items']
        names={r['category_id']:items[r['category_id']]['Name'] for r in preset['pool'] if r['category_id'] in items}
        names[family['category_id']]=project['name']
        return {'preset': preset, 'changes_applied': False, 'reward_names': names}


def save_activity_preferences(provider, body):
    if not isinstance(body,dict) or set(body)!={'expected_revision','preferences'}:
        raise ValueError('本服模板设置请求无效')
    preferences=normalize_preferences(body['preferences']);app=provider.get_engine()
    with app.store.lock:
        catalog,raw=app.store.read();app.store._expect(catalog,body['expected_revision'])
        candidate=copy.deepcopy(catalog);candidate['activity_preferences']=preferences;validate(candidate)
        if candidate!=catalog:app.store._save(candidate,raw)
    return {'catalog_revision':digest(candidate),'preferences':preferences,'changes_applied':False,
            'message':'本服模板参数已保存到当前制作目录；未修改已有机台或游戏服务器'}
