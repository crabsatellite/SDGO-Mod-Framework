"""Editable fixed-pool defaults, bound to verified target metadata at installation."""
from __future__ import annotations

import copy
from functools import lru_cache
from pathlib import Path
import re

from .capsule_catalog import digest, read_json, validate

RESOURCE = Path(__file__).with_name('resources') / 'default-fixed.json'


@lru_cache(maxsize=1)
def _resource():
    value = read_json(RESOURCE)
    if (value.get('schema_version') != 1 or value.get('id') != 'acquisition-defaults'
            or value.get('version') != 1 or len(value.get('fixed_machines', [])) != 35):
        raise ValueError('默认扭蛋模板不完整')
    units = {row['id']: row for row in value['units']}
    if len(units) != len(value['units']):
        raise ValueError('默认扭蛋模板的机体编号重复')
    ids = set()
    for machine in value['fixed_machines']:
        if (machine['id'] in ids or type(machine['price']) is not int or machine['price'] < 0
                or len(machine['slots']) != 6 or sum(s['weight'] for s in machine['slots']) != 1000):
            raise ValueError('默认扭蛋模板的机台配置无效')
        ids.add(machine['id'])
        for ordinal, slot in enumerate(machine['slots'], 1):
            if (slot['unit_id'] not in units or type(slot['weight']) is not int or slot['weight'] <= 0
                    or slot['template_slot'] != ordinal or slot['native_draw_group'] not in range(1, 7)):
                raise ValueError('默认扭蛋模板的槽位无效')
    for unit in units.values():
        if (not re.fullmatch(r'[ABC](?:R|U|S)?', unit['grade'])
                or len(unit['unit_sprites']) != 2
                or any(not re.fullmatch(r'sprs\\[0-9]+\.spr', p) for p in unit['unit_sprites'])):
            raise ValueError('默认扭蛋模板的机体资源引用无效')
    return value


def template():
    """Return an independent copy so editor changes never modify the shipped template."""
    return copy.deepcopy(_resource())


def describe():
    data = _resource()
    return {'id': data['id'], 'version': data['version'], 'machine_count': 35,
            'native_machine_count': 78, 'total_fixed_machines': 113, 'sha256': digest(data)}


def artwork_references(catalog):
    """Only compatible target unit IDs may use the bundled numeric references."""
    units = {row['id']: row for row in catalog['units']}
    return {str(row['id']): list(row['unit_sprites']) for row in _resource()['units']
            if row['id'] in units and units[row['id']].get('grade') == row['grade']}


def apply(catalog, *, restore=False):
    """Seed a fresh native catalog once; only an explicit restore resets managed pools.

    No target unit definitions, artwork, native pools, custom candidates, other
    owner machines, activity content or retired IDs are replaced by this operation.
    """
    validate(catalog)
    data = _resource()
    native = [m for m in catalog['fixed_machines'] if m.get('owner') == 'native']
    expected_native = {100 * number + 1 for number in range(1, 79)}
    managed = {m['id']: m for m in data['fixed_machines']}
    non_native = [m for m in catalog['fixed_machines'] if m.get('owner') != 'native']
    marker = catalog.get('default_fixed_preset')
    if not restore and (marker or non_native):
        return copy.deepcopy(catalog), {'status': 'preserved', **describe()}
    if {m['id'] for m in native} != expected_native:
        if not restore:
            return copy.deepcopy(catalog), {'status': 'incompatible', **describe()}
        raise ValueError('默认模板要求完整的 78 台原生普通机；当前目录不匹配，未修改配置')
    by_id = {m['id']: m for m in catalog['fixed_machines']}
    by_unit = {u['id']: u for u in catalog['units']}
    # Validate the complete template before changing anything: partial default
    # pools must not become the server's authoritative prize configuration.
    for unit in data['units']:
        actual = by_unit.get(unit['id'])
        if (actual is None or actual.get('grade') != unit['grade']
                or not actual.get('eligible', False) or actual.get('unit_import')):
            raise ValueError('默认模板缺少匹配且可出击的原生机体：' + str(unit['id']))
    for machine in data['fixed_machines']:
        existing = by_id.get(machine['id'])
        if existing is not None and (existing.get('owner') != 'initialization'
                                     or existing.get('template_id') != machine['template_id']):
            raise ValueError('默认模板机台编号已被其他内容占用：' + str(machine['id']))
        source = by_id.get(machine['template_id'])
        if source is None or source.get('owner') != 'native' or len(source['slots']) != 6:
            raise ValueError('默认模板缺少原生六格模板：' + str(machine['template_id']))
        for slot, donor in zip(machine['slots'], source['slots']):
            if by_unit[slot['unit_id']]['grade'] != by_unit[donor['unit_id']]['grade']:
                raise ValueError('默认模板与目标机体等级不匹配：' + str(machine['id']))
    result = copy.deepcopy(catalog)
    added = []
    for machine in data['fixed_machines']:
        donor = by_id[machine['template_id']]
        slots = []
        for ref, source_slot in zip(machine['slots'], donor['slots']):
            unit = by_unit[ref['unit_id']]
            slots.append({**ref, 'sprites': copy.deepcopy(unit.get('capsule_sprites', [])),
                          'template_unit_id': source_slot['unit_id'],
                          'template_grade': by_unit[source_slot['unit_id']]['grade'],
                          'template_sprites': copy.deepcopy(source_slot.get('sprites', []))})
        added.append({**copy.deepcopy(machine), 'enabled': True, 'currency': 'GP',
                      'owner': 'initialization', 'draw_policy': 'independent_weighted',
                      'pricing_source': 'default_template', 'template_price': machine['price'],
                      'slots': slots})
    # Restore in place; unrelated author content and its display order survive.
    replacements = {m['id']: m for m in added}
    result['fixed_machines'] = [replacements.pop(m['id'], m) for m in result['fixed_machines']]
    result['fixed_machines'].extend(m for m in added if m['id'] in replacements)
    result['default_fixed_preset'] = describe()
    validate(result)
    return result, {'status': 'restored' if restore else 'initialized', **describe()}
