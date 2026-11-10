"""Generic Ace pilot metadata validation and private owner-overlay compilation."""
from __future__ import annotations

import json

from .project import normalize_metadata

SCHEMA_VERSION = 1
MAX_PILOTS = 8192


def normalize_catalog(value):
    if (not isinstance(value, dict) or set(value) != {'schema_version', 'pilots'}
            or value.get('schema_version') != SCHEMA_VERSION):
        raise ValueError('王牌驾驶员目录声明无效')
    rows = value.get('pilots')
    if not isinstance(rows, list) or len(rows) > MAX_PILOTS:
        raise ValueError('王牌驾驶员目录列表无效')
    result = []
    seen = set()
    for entry in rows:
        if not isinstance(entry, dict) or set(entry) != {'unit_id', 'display_name', 'kind'}:
            raise ValueError('王牌驾驶员目录字段无效')
        unit_id = entry.get('unit_id')
        if type(unit_id) is not int or not 0 < unit_id <= 9_999_999 or unit_id in seen:
            raise ValueError('王牌驾驶员机体编号无效或重复')
        metadata = normalize_metadata({'ace_pilot': {
            'display_name': entry.get('display_name'), 'kind': entry.get('kind')}})
        seen.add(unit_id)
        result.append({'unit_id': unit_id, **metadata['ace_pilot']})
    result.sort(key=lambda row: row['unit_id'])
    return {'schema_version': SCHEMA_VERSION, 'pilots': result}


def _add_unit_id(result, value, label):
    if value is None:
        return
    if type(value) is not int or not 0 < value <= 9_999_999:
        raise ValueError(label + ' 机体编号无效')
    result.add(value)


def _project_unit_ids(project, metadata):
    result = set()
    definitions = project.get('definitions', {})
    if definitions is None:
        definitions = {}
    if not isinstance(definitions, dict):
        raise ValueError('机体项目 definitions 无效')
    _add_unit_id(result, project.get('root_unit_id'), '机体项目')
    _add_unit_id(result, definitions.get('root_unit_id'), '机体项目 definitions')
    _add_unit_id(result, metadata.get('declared_unit_id'), '机体项目资料')

    forms = project.get('forms', [])
    if not isinstance(forms, list):
        raise ValueError('机体项目 forms 无效')
    for form in forms:
        if not isinstance(form, dict) or not isinstance(form.get('server'), dict):
            raise ValueError('机体项目形态无效')
        if 'UnitNumber' not in form['server']:
            raise ValueError('机体项目形态缺少机体编号')
        _add_unit_id(result, form['server']['UnitNumber'], '机体项目形态')

    units = definitions.get('units', [])
    if not isinstance(units, list):
        raise ValueError('机体项目 definitions.units 无效')
    for unit in units:
        if not isinstance(unit, dict):
            raise ValueError('机体项目 definitions.units 记录无效')
        if 'UnitNumber' not in unit:
            raise ValueError('机体项目 definitions.units 缺少机体编号')
        _add_unit_id(result, unit['UnitNumber'], '机体项目 definitions.units')
    return result


def catalog_from_projects(projects):
    """Compile only explicitly authored Ace metadata; no fallback is invented."""
    if not isinstance(projects, list):
        raise ValueError('机体项目列表无效')
    pilots = {}
    for project in projects:
        if not isinstance(project, dict):
            raise ValueError('机体项目无效')
        metadata = normalize_metadata(project.get('metadata', {}))
        pilot = metadata.get('ace_pilot')
        if pilot is None:
            continue
        unit_ids = _project_unit_ids(project, metadata)
        if not unit_ids:
            raise ValueError('王牌驾驶员项目缺少机体编号')
        for unit_id in unit_ids:
            prior = pilots.get(unit_id)
            if prior is not None and prior != pilot:
                raise ValueError('同一机体声明了不同的王牌驾驶员')
            pilots[unit_id] = pilot
    return normalize_catalog({'schema_version': SCHEMA_VERSION, 'pilots': [
        {'unit_id': unit_id, **pilot} for unit_id, pilot in pilots.items()]})


def encode_catalog(value):
    return (json.dumps(normalize_catalog(value), ensure_ascii=False, indent=2) + '\n').encode('utf-8')
