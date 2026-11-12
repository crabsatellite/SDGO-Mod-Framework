"""Apply a batch unit-roster server plan through the ordinary unit repository.

A protocol-ready publication from any source provides `server-plan.json`,
`form-graph-requests.json` and the client package in one publication folder.
This module validates that folder, checks it read-only against the configured
WorldServer database and then, inside the Manager's stopped-server gate, applies:

1. the shared skill, funnel and affiliation rows in one transaction;
2. every family through `Repository.apply`, in dependency order (a family whose
   forms use another family's weapon or transform into another family's form is
   applied after it; a dependency cycle is refused before anything is written);
3. the native form graphs, compiled from the installed rows and written to every
   BattleServer host of the selected server group.

It never stops or starts game processes and never reads or writes accounts,
inventories, shop listings or gacha tables. Re-running the same publication is
idempotent: every row is owned through the existing ownership ledgers.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct
from typing import Any, Callable

from . import repository
from .compiler import CATALOG_RECORD_LIMIT
from .repository import FAMILY_KEY, ITEM_COLUMNS

PLAN_SCHEMA = 'sdgo.unit-roster-server-plan/1'
GRAPH_SCHEMA = 'sdgo.unit-roster-form-graphs/1'
# Publications written before the format was named neutrally carry these identifiers; they are the same format.
LEGACY_PLAN_SCHEMAS = frozenset({'sdgo.gunion-roster-server-plan/1'})
LEGACY_GRAPH_SCHEMAS = frozenset({'sdgo.gunion-roster-form-graphs/1'})
ACTION = 'unit_catalog.batch_roster'
GRAPH_MOD = 'unit_catalog_server'
CATALOG = 'package/client/sdgo/mods/unit_catalog/catalog.bin'
WEAPON_FIELDS = ('WeaponNumber_1', 'WeaponNumber_2', 'WeaponNumber_3', 'WeaponNumber_FatalAttack')
MAX_PLAN_BYTES = 128 * 1024 * 1024
MAX_CATALOG_BYTES = 64 * 1024 * 1024 + 32
MAX_FAMILIES = 4096
_FAMILY_FIELDS = {'family_key', 'root_unit_id', 'item_template_id', 'units', 'weapons', 'item', 'names'}
_FAMILY_OPTIONAL = {'shared_form_ids', 'shared_weapon_ids', 'behavior_profile'}
_RECORD_BYTES = {2: 4976, 10: 1871, 11: 732, 12: 1559, 13: 1132}
_IN_CHUNK = 500

Progress = Callable[[dict], None]


class Publication:
    """A validated batch publication folder (never the client files themselves)."""

    def __init__(self, root: Path, plan: dict, graphs: list, records: dict[int, dict[int, bytes]]):
        self.root, self.plan, self.graph_requests, self.records = root, plan, graphs, records

    @property
    def revision(self) -> str:
        return self.plan['revision']

    @property
    def owner(self) -> str:
        return self.plan['owner']

    @property
    def families(self) -> list[dict]:
        return self.plan['families']

    def unit_ids(self) -> list[int]:
        return sorted({int(row['UnitNumber']) for family in self.families for row in family['units']})

    def weapon_ids(self) -> list[int]:
        return sorted({int(row['WeaponNo']) for family in self.families for row in family['weapons']})

    def item_ids(self) -> list[int]:
        return sorted({int(family['item']['CatalogueID']) for family in self.families})

    def skill_ids(self) -> list[int]:
        return sorted(int(row['id']) for row in self.plan['skills']['skills'])

    def funnel_ids(self) -> list[int]:
        return sorted(int(row['id']) for row in self.plan['funnels']['funnels'])

    def influence_ids(self) -> list[int]:
        return sorted(int(row['InfluenceNumber']) for row in self.plan['affiliations'])

    def summary(self) -> dict:
        return {'owner': self.owner, 'revision': self.revision, 'families': len(self.families),
                'units': len(self.unit_ids()), 'weapons': len(self.weapon_ids()), 'items': len(self.item_ids()),
                'skills': len(self.skill_ids()), 'funnels': len(self.funnel_ids()),
                'affiliations': len(self.influence_ids()), 'form_graph_requests': len(self.graph_requests)}


def _read_json(path: Path, label: str) -> Any:
    if not path.is_file():
        raise ValueError('发布目录缺少 ' + label)
    if path.stat().st_size > MAX_PLAN_BYTES:
        raise ValueError(label + ' 超过大小上限')
    try:
        return json.loads(path.read_bytes().decode('utf-8-sig'))
    except (UnicodeError, json.JSONDecodeError):
        raise ValueError(label + ' 不是有效的 JSON') from None


def _positive(value: Any, label: str) -> int:
    if type(value) is not int or not 1 <= value <= repository.INT_MAX:
        raise ValueError(label + ' 必须是正整数')
    return value


def _sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _catalog_records(root: Path) -> dict[int, dict[int, bytes]]:
    """Parse the package catalog container; the package manifest seals its bytes."""
    path = root / CATALOG
    if not path.is_file():
        raise ValueError('发布目录缺少客户端机体目录 ' + CATALOG)
    if path.stat().st_size > MAX_CATALOG_BYTES:
        raise ValueError('客户端机体目录超过容量')
    raw = path.read_bytes()
    manifest_path = root / 'package/manifest.json'
    if manifest_path.is_file():
        manifest = _read_json(manifest_path, 'package/manifest.json')
        sealed = (manifest.get('files') or {}).get('client/sdgo/mods/unit_catalog/catalog.bin') if isinstance(manifest, dict) else None
        if sealed is not None and sealed != _sha(raw):
            raise ValueError('客户端机体目录与安装包清单的哈希不一致')
    if len(raw) < 32 or raw[:8] != b'SDGOUN1\0':
        raise ValueError('客户端机体目录容器无效')
    version, header, count, extent, _, reserved = struct.unpack_from('<6I', raw, 8)
    if version != 1 or header != 32 or reserved or len(raw) != 32 + extent or count > CATALOG_RECORD_LIMIT:
        raise ValueError('客户端机体目录头部无效')
    cursor, result = 32, {}
    for _ in range(count):
        if cursor + 24 > len(raw):
            raise ValueError('客户端机体目录记录头截断')
        kind, identity, size, _, flags, reserved = struct.unpack_from('<6I', raw, cursor)
        cursor += 24
        record = raw[cursor:cursor + size]
        cursor += size
        if flags or reserved or len(record) != size or len(record) < 5 or struct.unpack_from('<I', record)[0] != identity:
            raise ValueError('客户端机体目录记录无效：%d' % identity)
        expected = _RECORD_BYTES.get(kind)
        if expected is not None and size != expected:
            raise ValueError('客户端机体目录记录长度无效：%d' % identity)
        if identity in result.setdefault(kind, {}):
            raise ValueError('客户端机体目录记录重复：%d' % identity)
        result[kind][identity] = record
    if cursor != len(raw):
        raise ValueError('客户端机体目录长度无效')
    return result


def _validate_plan(plan: Any) -> dict:
    if not isinstance(plan, dict) or set(plan) != {'schema', 'owner', 'revision', 'families', 'skills', 'funnels', 'affiliations'}:
        raise ValueError('server-plan.json 结构不是批量阵容服务端计划')
    if plan['schema'] != PLAN_SCHEMA and plan['schema'] not in LEGACY_PLAN_SCHEMAS:
        raise ValueError('不支持的批量服务端计划版本：' + str(plan['schema'])[:80])
    if not isinstance(plan['owner'], str) or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,63}', plan['owner']):
        raise ValueError('批量服务端计划的所有者无效')
    if not isinstance(plan['revision'], str) or not re.fullmatch('[0-9a-f]{64}', plan['revision']):
        raise ValueError('批量服务端计划的内容版本无效')
    families = plan['families']
    if not isinstance(families, list) or not 1 <= len(families) <= MAX_FAMILIES:
        raise ValueError('批量服务端计划的机体族数量无效')
    keys, unit_owner, weapon_owner, items = set(), {}, {}, set()
    for family in families:
        if not isinstance(family, dict) or not _FAMILY_FIELDS <= set(family) <= _FAMILY_FIELDS | _FAMILY_OPTIONAL:
            raise ValueError('机体族计划的字段无效')
        key = family['family_key']
        if not isinstance(key, str) or FAMILY_KEY.fullmatch(key) is None or key in keys:
            raise ValueError('机体族标识无效或重复：' + str(key)[:130])
        keys.add(key)
        _positive(family['root_unit_id'], key + ' 的 root_unit_id')
        _positive(family['item_template_id'], key + ' 的 item_template_id')
        for field, owner, id_key in (('units', unit_owner, 'UnitNumber'), ('weapons', weapon_owner, 'WeaponNo')):
            rows = family[field]
            if not isinstance(rows, list) or (field == 'units' and not rows):
                raise ValueError(key + ' 的 ' + field + ' 无效')
            for row in rows:
                if not isinstance(row, dict):
                    raise ValueError(key + ' 的 ' + field + ' 行无效')
                identity = _positive(row.get(id_key), key + ' 的 ' + id_key)
                if identity in owner:
                    raise ValueError('%s %d 同时由 %s 和 %s 拥有' % (id_key, identity, owner[identity], key))
                owner[identity] = key
        item = family['item']
        if not isinstance(item, dict) or not {'CatalogueID', 'RefIndex', 'Name'} <= set(item) <= set(ITEM_COLUMNS):
            raise ValueError(key + ' 的物品类别行无效')
        if _positive(item['CatalogueID'], key + ' 的 CatalogueID') in items:
            raise ValueError('物品类别 %d 重复' % item['CatalogueID'])
        items.add(item['CatalogueID'])
        if not isinstance(family['names'], list):
            raise ValueError(key + ' 的名称行无效')
        for field in ('shared_form_ids', 'shared_weapon_ids'):
            shared = family.get(field, {})
            if not isinstance(shared, dict) or any(not isinstance(v, str) for v in shared.values()):
                raise ValueError(key + ' 的 ' + field + ' 无效')
    for section, schema in (('skills', 'sdgo.unit-server-dependencies/1'), ('funnels', 'sdgo.unit-server-funnels/1')):
        block = plan[section]
        if (not isinstance(block, dict) or set(block) != {'schema', 'owner', 'revision', section} or block['schema'] != schema or
                not isinstance(block[section], list)):
            raise ValueError('批量服务端计划的 ' + section + ' 段无效')
    affiliations = plan['affiliations']
    if not isinstance(affiliations, list):
        raise ValueError('批量服务端计划的势力段无效')
    seen = set()
    for row in affiliations:
        number = row.get('InfluenceNumber') if isinstance(row, dict) else None
        if type(number) is not int or not 1 <= number <= 255 or number in seen or not isinstance(row.get('InfluenceName'), str):
            raise ValueError('势力行无效：' + str(number)[:20])
        seen.add(number)
    return plan


def _validate_graphs(value: Any, plan: dict, records: dict[int, dict[int, bytes]]) -> list:
    if value is None:
        return []
    if (not isinstance(value, dict) or set(value) != {'schema', 'owner', 'revision', 'requests'}
            or (value['schema'] != GRAPH_SCHEMA and value['schema'] not in LEGACY_GRAPH_SCHEMAS)):
        raise ValueError('form-graph-requests.json 结构无效')
    if value['owner'] != plan['owner'] or value['revision'] != plan['revision']:
        raise ValueError('形态图请求与服务端计划不是同一次发布')
    requests = value['requests']
    if not isinstance(requests, list) or len(requests) > 512:
        raise ValueError('形态图请求数量无效')
    by_key = {family['family_key']: family for family in plan['families']}
    units = records.get(2, {})
    for request in requests:
        if not isinstance(request, dict) or not isinstance(request.get('families'), list) or not isinstance(request.get('records'), list):
            raise ValueError('形态图请求无效')
        if request.get('revision') != plan['revision']:
            raise ValueError('形态图请求的版本与服务端计划不一致')
        missing = [key for key in request['families'] if key not in by_key]
        if missing:
            raise ValueError('形态图请求引用了本次发布之外的机体族：' + str(missing[0])[:130])
        owned = {int(row['UnitNumber']) for key in request['families'] for row in by_key[key]['units']}
        roots = {by_key[key]['root_unit_id'] for key in request['families']}
        for declaration in request['records']:
            identity = declaration.get('id') if isinstance(declaration, dict) else None
            if identity not in owned:
                raise ValueError('形态图节点 %s 不属于请求的机体族' % str(identity)[:20])
            raw = units.get(identity)
            if raw is None or _sha(raw) != declaration.get('client_sha256'):
                raise ValueError('形态图节点 %d 与客户端机体目录不一致' % identity)
        if not isinstance(request.get('roots'), list) or any(root not in roots for root in request['roots']):
            raise ValueError('形态图入口必须是请求机体族的根形态')
    return requests


def load_publication(folder) -> Publication:
    """Read and validate a batch publication folder (or the roster output root)."""
    if not folder or not isinstance(folder, (str, Path)):
        raise ValueError('请选择批量发布目录')
    root = Path(folder).expanduser().resolve()
    if not (root / 'server-plan.json').is_file() and (root / 'publication/server-plan.json').is_file():
        root = root / 'publication'
    if not root.is_dir():
        raise ValueError('批量发布目录不存在')
    plan = _validate_plan(_read_json(root / 'server-plan.json', 'server-plan.json'))
    records = _catalog_records(root)
    from .skill_slots import ordinary
    for family in plan['families']:
        for row in family['units']:
            for field in ('SkillNumber_1','SkillNumber_2'):
                identity=row.get(field,0)
                if identity and not ordinary(identity):
                    raise ValueError(f"机体 {row.get('UnitNumber')} 的 {field} 编号 {identity} 被原生客户端分到额外技能槽；请使用普通技能编号或配套的类别适配器")
    graphs_path = root / 'form-graph-requests.json'
    graphs = _validate_graphs(_read_json(graphs_path, 'form-graph-requests.json') if graphs_path.is_file() else None, plan, records)
    for section, kind in (('skills', 13), ('funnels', 11)):
        for row in plan[section][section]:
            if not isinstance(row, dict) or row.get('id') not in records.get(kind, {}):
                raise ValueError('共享%s %s 缺少客户端记录' % ('技能' if kind == 13 else '浮游炮', str(row.get('id') if isinstance(row, dict) else row)[:20]))
    return Publication(root, plan, graphs, records)


def family_order(families: list[dict]) -> list[dict]:
    """Families in dependency order; refuses a cycle instead of guessing.

    A family depends on another family of the same plan when one of its forms
    references that family's weapon (WeaponNumber_1/2/3/FatalAttack) or form
    (UnitNumber_Transform), or when it declares that family as the owner of a
    shared form or weapon. Independent families keep their publication order.
    """
    keys = [family['family_key'] for family in families]
    by_key = {family['family_key']: family for family in families}
    weapon_owner = {int(row['WeaponNo']): family['family_key'] for family in families for row in family['weapons']}
    unit_owner = {int(row['UnitNumber']): family['family_key'] for family in families for row in family['units']}
    depends: dict[str, set[str]] = {key: set() for key in keys}
    for family in families:
        key = family['family_key']
        for row in family['units']:
            references = [(unit_owner, row.get('UnitNumber_Transform'))] + [(weapon_owner, row.get(field)) for field in WEAPON_FIELDS]
            for owners, value in references:
                owner = owners.get(int(value)) if type(value) is int and value else None
                if owner and owner != key:
                    depends[key].add(owner)
        for field in ('shared_form_ids', 'shared_weapon_ids'):
            for owner in (family.get(field) or {}).values():
                if owner in by_key and owner != key:
                    depends[key].add(owner)
    done: set[str] = set()
    order: list[dict] = []
    while len(order) < len(keys):
        ready = [key for key in keys if key not in done and depends[key] <= done]
        if not ready:
            raise ValueError('机体族之间存在循环依赖，已拒绝写入：' + ' → '.join(_cycle(depends, done)))
        done.update(ready)
        order.extend(by_key[key] for key in ready)
    return order


def _cycle(depends: dict[str, set[str]], done: set[str]) -> list[str]:
    remaining = {key for key in depends if key not in done}
    start = min(remaining)
    path, seen, node = [], {}, start
    while node not in seen:
        seen[node] = len(path)
        path.append(node)
        node = min(depends[node] & remaining)
    return [*path[seen[node]:], node][:12]


def family_plan(family: dict, revision: str, items: dict[int, dict]) -> dict:
    """The ordinary single-family repository plan for one batch family."""
    template = items.get(family['item_template_id'])
    if template is None:
        raise ValueError('%s 的物品类别模板 %d 不存在' % (family['family_key'], family['item_template_id']))
    value = {'family_key': family['family_key'], 'revision': revision, 'root_unit_id': family['root_unit_id'],
             'item_template_id': family['item_template_id'], 'units': family['units'], 'weapons': family['weapons'],
             'item': {**{column: template[column] for column in ITEM_COLUMNS}, **family['item']}, 'names': family['names']}
    if family.get('behavior_profile'):
        value['behavior_profile'] = family['behavior_profile']
    return value


# ---------------------------------------------------------------- server group

def battle_hosts(server_ini) -> list[dict]:
    """Every BattleServer host of the selected server group, with its runtime state."""
    from manager import operations, storage
    _, nodes, _ = operations.topology(server_ini)
    rows = []
    for host in sorted({node.parent for node in nodes if node.name == 'BattleServer.exe'}, key=str):
        mod = host / 'sdgo/mods' / GRAPH_MOD
        installed = (mod / (GRAPH_MOD + '.dll')).is_file() and (mod / 'mod.ini').is_file()
        enabled = setting = global_gate = False
        if installed:
            values = storage.ini_values((mod / 'mod.ini').read_bytes())
            enabled = values.get('mod', 'enabled', fallback='1').strip() != '0'
            setting = values.get('settings', 'Enabled', fallback='1').strip() != '0'
            category = values.get('mod', 'category', fallback='gameplay')
            feature = values.get('mod', 'feature', fallback=GRAPH_MOD)
            gates = storage.ini_values((host / 'sdgo/sdgo.ini').read_bytes()) if (host / 'sdgo/sdgo.ini').is_file() else None
            global_gate = gates is None or all(gates.get(section, key, fallback='1').strip() != '0' for section, key in
                                               (('categories', category), ('features', feature), ('mods', GRAPH_MOD)))
        graph = mod / 'form-graphs.bin'
        authority = host / 'sdgo/install-state/unit-form-authority.bin'
        rows.append({'host': host.name, 'path': host, 'installed': installed, 'enabled': enabled,
                     'setting_enabled': setting, 'global_enabled': global_gate,
                     'graph_sha256': _sha(graph.read_bytes()) if graph.is_file() else None,
                     'authority_sha256': _sha(authority.read_bytes()) if authority.is_file() else None})
    return rows


def running_services(server_ini) -> list[str]:
    """Native programs of the group that are open (running); probes without writing."""
    from manager import operations
    _, nodes, _ = operations.topology(server_ini)
    running = []
    for node in nodes:
        try:
            with operations.unmapped_image(node):
                pass
        except ValueError:
            running.append(node.parent.name + '/' + node.name)
    return running


def _public_hosts(rows: list[dict]) -> list[dict]:
    return [{key: value for key, value in row.items() if key != 'path'} for row in rows]


# ---------------------------------------------------------------- read-only preflight

def _chunks(values: list) -> list[list]:
    return [values[index:index + _IN_CHUNK] for index in range(0, len(values), _IN_CHUNK)]


def _object_exists(cursor, table: str) -> bool:
    row = cursor.execute("SELECT OBJECT_ID(?, N'U')", 'dbo.' + table).fetchone()
    return row is not None and row[0] is not None


def _shared_preview(cursor, publication: Publication) -> dict:
    """Resolve shared skill/funnel rows exactly as the writer would, using plain reads."""
    from . import server_dependencies as dependencies
    result = {}
    for kind, section, record_kind in (('skill', 'skills', 13), ('funnel', 'funnels', 11)):
        manifest = publication.plan[section]
        if not manifest[section]:
            result[section] = {'inserted': 0, 'updated': 0, 'reused': 0}
            continue
        _, _, table, columns, key, sql_schema, _, _ = dependencies._spec(kind)
        dependencies._schema(cursor, table, sql_schema)
        ledger = _object_exists(cursor, dependencies.LEDGER)
        if ledger:
            dependencies._schema(cursor, dependencies.LEDGER, dependencies.LEDGER_SCHEMA)
        available = {int(row[0]) for row in cursor.execute('SELECT [' + key + '] FROM dbo.[' + table + ']').fetchall()}
        wanted = sorted({entry['id'] for entry in manifest[section]} |
                        {entry['definition']['id'] for entry in manifest[section]
                         if isinstance(entry.get('definition'), dict) and entry['definition'].get('kind') == 'template'
                         and type(entry['definition'].get('id')) is int})
        rows = {}
        for part in _chunks(wanted):
            for row in cursor.execute('SELECT ' + ','.join('[' + name + ']' for name in columns) + ' FROM dbo.[' + table +
                                      '] WHERE [' + key + '] IN (' + ','.join('?' for _ in part) + ')', *part).fetchall():
                rows[int(row[0])] = dict(zip(columns, row))
        ownership = {}
        if ledger:
            for row in cursor.execute('SELECT RecordID,OwnerKey,Revision,ClientHash,DefinitionJson,InstalledHash '
                                      'FROM dbo.sdgo_UnitImportDependencies WHERE RecordKind=?', kind).fetchall():
                ownership[int(row[0])] = dict(zip(('owner', 'revision', 'client_hash', 'definition_json', 'installed_hash'), row[1:]))
        requested = {entry['id'] for entry in manifest[section]}
        planned = dependencies.prepare(manifest, publication.records.get(record_kind, {}),
                                       templates={k: v for k, v in rows.items()},
                                       current={k: v for k, v in rows.items() if k in requested},
                                       ownership={k: v for k, v in ownership.items() if k in requested},
                                       available_ids=available, kind=kind)
        result[section] = {action: sum(row['action'] == action for row in planned) for action in ('inserted', 'updated', 'reused')}
    result['affiliations'] = _influence_preview(cursor, publication.plan['affiliations'])
    return result


def _influence_layout(cursor) -> int:
    if not _object_exists(cursor, 'tbl_Influences'):
        raise ValueError('服务端缺少势力表 dbo.tbl_Influences')
    columns = [tuple(row) for row in cursor.execute(
        'SELECT c.name,t.name,c.max_length FROM sys.columns c JOIN sys.types t ON c.user_type_id=t.user_type_id '
        'WHERE c.object_id=OBJECT_ID(?) ORDER BY c.column_id', 'dbo.tbl_Influences').fetchall()]
    if ([(str(name), str(kind).lower()) for name, kind, _ in columns] != [('InfluenceNumber', 'tinyint'), ('InfluenceName', 'varchar')]):
        raise ValueError('服务端 dbo.tbl_Influences 结构与支持版本不一致')
    return int(columns[1][2])


def _influence_label(row: dict, limit: int) -> str:
    name = row['InfluenceName'].strip()
    # The column is a varchar; a name it cannot store becomes a neutral label.
    # Players see the client's own affiliation record, never this column.
    if not name or not name.isascii() or not name.isprintable():
        name = 'Affiliation %d' % row['InfluenceNumber']
    return name[:limit]


def _influence_preview(cursor, rows: list[dict]) -> dict:
    if not rows:
        return {'inserted': 0, 'kept': 0}
    _influence_layout(cursor)
    existing = {int(row[0]) for row in cursor.execute('SELECT InfluenceNumber FROM dbo.tbl_Influences').fetchall()}
    added = [row['InfluenceNumber'] for row in rows if row['InfluenceNumber'] not in existing]
    return {'inserted': len(added), 'kept': len(rows) - len(added)}


def _simulate_families(repo, publication: Publication, order: list[dict]) -> tuple[list, list, int]:
    """check_plan every family in order against the live catalog plus what earlier ones add."""
    catalog = repo.catalog
    catalog['skill_ids'] = set(catalog['skill_ids']) | set(publication.skill_ids())
    catalog['funnel_ids'] = set(catalog['funnel_ids']) | set(publication.funnel_ids())
    problems, rows, installed = [], [], 0
    for family in order:
        key = family['family_key']
        owned = repo._ownership.get(('unit', family['root_unit_id']))
        state = 'installed' if owned and owned[0] == key else 'new'
        installed += state == 'installed'
        try:
            repo.check_plan(family_plan(family, publication.revision, catalog['items']))
        except ValueError as error:
            problems.append({'family_key': key, 'error': str(error)[:300]})
            rows.append({'family_key': key, 'state': state, 'valid': False})
            continue
        rows.append({'family_key': key, 'state': state, 'valid': True})
        for row in family['weapons']:
            catalog['weapons'][row['WeaponNo']] = dict(row)
        for row in family['units']:
            catalog['units'][row['UnitNumber']] = dict(row)
        for row in family['names']:
            catalog['names'][row['UnitNumber']] = dict(row)
        catalog['items'][family['item']['CatalogueID']] = family_plan(family, publication.revision, catalog['items'])['item']
    return problems, rows, installed


def preflight(folder, server_ini) -> dict:
    """Read-only: validate the publication and every write it would make."""
    if not server_ini:
        raise ValueError('请先在机体导入模块配置 WorldServer INI')
    from manager import operations
    publication = load_publication(folder)
    operations.require_local_database(server_ini)
    problems: list[dict] = []
    try:
        order = family_order(publication.families)
    except ValueError as error:
        order = []
        problems.append({'family_key': '', 'error': str(error)})
    hosts = battle_hosts(server_ini)
    if publication.graph_requests:
        if not hosts:
            problems.append({'family_key': '', 'error': '所选服务组没有 BattleServer，无法安装形态图'})
        for row in hosts:
            if not row['installed']:
                problems.append({'family_key': '', 'error': '请先为 %s 安装配套框架的 %s' % (row['host'], GRAPH_MOD)})
    running = running_services(server_ini)
    shared: dict = {}
    connection = repository._connect(Path(server_ini))
    try:
        shared = _shared_preview(connection.cursor(), publication)
    except ValueError as error:
        problems.append({'family_key': '', 'error': '共享行：' + str(error)[:300]})
    except Exception:
        problems.append({'family_key': '', 'error': '共享行：读取服务端技能、浮游炮或势力表失败'})
    finally:
        try:
            connection.rollback()
        finally:
            connection.close()
    families: list = []
    installed = 0
    if order:
        repo = repository.Repository(server_ini=Path(server_ini))
        family_problems, families, installed = _simulate_families(repo, publication, order)
        problems.extend(family_problems)
    warnings = []
    if publication.graph_requests:
        for row in hosts:
            if row['installed'] and not (row['setting_enabled'] and row['global_enabled']):
                warnings.append('%s 的 %s 被 Manager 设置或全局开关关闭；形态图会写入，但服务端不会加载' % (row['host'], GRAPH_MOD))
    return {'publication': publication.summary(), 'order': [family['family_key'] for family in order],
            'families_installed': installed, 'families_new': len(families) - installed, 'families': families,
            'shared': shared, 'graph_hosts': _public_hosts(hosts), 'running_services': running,
            'servers_stopped': not running, 'problems': problems, 'warnings': warnings,
            'ready': not problems and not running, 'changes_applied': False, 'grants_units': False}


# ---------------------------------------------------------------- apply

def verify_installed(folder,server_ini,baseline) -> bool:
    """Read-only restart check, including live rows and complete graph owners."""
    from types import SimpleNamespace
    from . import graph_preservation
    publication=load_publication(folder)
    review=preflight(folder,server_ini)
    if review['problems'] or not readback(publication,server_ini)['complete']:return False
    retained=graph_preservation.read(server_ini,SimpleNamespace(graph_requests=[]),baseline)
    installed={row['owner']:row for row in retained['requests']}
    for desired in publication.graph_requests:
        actual=installed.get(desired['owner'])
        if (not actual or actual['revision']!=desired['revision'] or
            set(actual['roots'])!=set(desired['roots']) or
            set(actual['families'])!=set(desired.get('families',[desired['owner']])) or
            {row['id']:row['client_sha256'] for row in actual['records']}!=
            {row['id']:row['client_sha256'] for row in desired['records']}):return False
    return True

def _apply_shared(server_ini, publication: Publication) -> dict:
    from . import server_dependencies as dependencies
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()
        cursor.execute('SET XACT_ABORT ON')
        cursor.execute('BEGIN TRANSACTION')
        lock = cursor.execute("DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,@LockMode=N'Exclusive',"
                              "@LockOwner=N'Transaction',@LockTimeout=5000; SELECT @r;", repository.APP_LOCK).fetchone()
        if lock is None or int(lock[0]) < 0:
            raise ValueError('另一项机体目录更新正在进行，请稍后重试')
        result = {}
        for kind, section, record_kind in (('skill', 'skills', 13), ('funnel', 'funnels', 11)):
            manifest = publication.plan[section]
            if manifest[section]:
                result[section] = dependencies.apply(cursor, manifest, publication.records.get(record_kind, {}), kind=kind)['counts']
            else:
                result[section] = {'inserted': 0, 'updated': 0, 'reused': 0}
        rows = publication.plan['affiliations']
        added = []
        if rows:
            limit = _influence_layout(cursor)
            existing = {int(row[0]) for row in cursor.execute('SELECT InfluenceNumber FROM dbo.tbl_Influences WITH (UPDLOCK,HOLDLOCK)').fetchall()}
            for row in rows:
                if row['InfluenceNumber'] in existing:
                    continue
                cursor.execute('INSERT dbo.tbl_Influences(InfluenceNumber,InfluenceName) VALUES(?,?)',
                               row['InfluenceNumber'], _influence_label(row, limit))
                added.append(row['InfluenceNumber'])
        result['affiliations'] = {'inserted': len(added), 'kept': len(rows) - len(added)}
        cursor.execute('COMMIT TRANSACTION')
        connection.commit()
        return result
    except ValueError:
        connection.rollback()
        raise
    except Exception:
        connection.rollback()
        raise ValueError('共享技能、浮游炮或势力行写入失败，已全部回滚') from None
    finally:
        connection.close()


def _enable_mod(raw: bytes) -> bytes:
    from manager import storage
    return storage.patch_ini(raw, [('mod', 'enabled', '1')])


def install_graphs(server_ini, publication: Publication, hosts: list[dict], backup_root: Path | None,graph_context=None) -> dict:
    """Compile host authority from the installed rows and write it to every battle host."""
    from manager import storage
    from .form_graph_runtime import prepare_native_from_cursor
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()
        cursor.execute('SET XACT_ABORT ON')
        cursor.execute('BEGIN TRANSACTION')
        context=graph_context or {'requests':[],'records':{},'originals':{}}
        if set(context)!={'requests','records','originals'}:raise ValueError('保留形态图控制器上下文无效')
        client_records=dict(context['records'])
        for identity,raw in publication.records.get(2,{}).items():
            if identity in client_records and client_records[identity]!=raw:raise ValueError('保留形态图与新目录记录冲突')
            client_records[identity]=raw
        candidate, authority = prepare_native_from_cursor(cursor, [*context['requests'],*publication.graph_requests], client_records, originals=context['originals'])
        cursor.execute('COMMIT TRANSACTION')
        connection.commit()
    except ValueError:
        connection.rollback()
        raise
    except Exception:
        connection.rollback()
        raise ValueError('形态图编译失败，未写入任何战斗主机') from None
    finally:
        connection.close()
    missing = [row['host'] for row in hosts if not row['installed']]
    if missing or not hosts:
        raise ValueError('请先为 %s 安装配套框架的 %s' % ('、'.join(missing) or 'BattleServer', GRAPH_MOD))
    written = []
    for row in hosts:
        host = row['path']
        mod = host / 'sdgo/mods' / GRAPH_MOD
        targets = {mod / 'form-graphs.bin': candidate, host / 'sdgo/install-state/unit-form-authority.bin': authority}
        ini_path = mod / 'mod.ini'
        ini_raw = ini_path.read_bytes()
        enabled_raw = _enable_mod(ini_raw)
        if backup_root is not None:
            for target in (*targets, ini_path):
                if target.is_file():
                    saved = backup_root / host.name / target.relative_to(host)
                    if not saved.exists():
                        storage.atomic(saved, target.read_bytes())
        for target, data in targets.items():
            storage.atomic(target, data)
            if target.read_bytes() != data:
                raise ValueError('形态图写入后读回不一致：' + host.name)
        if enabled_raw != ini_raw:
            storage.atomic(ini_path, enabled_raw)
        written.append(host.name)
    return {'requests': len(publication.graph_requests), 'candidate_sha256': _sha(candidate),
            'authority_sha256': _sha(authority), 'hosts': written}


def apply(folder, server_ini, progress: Progress | None = None,graph_context=None,*,baseline=None) -> dict:
    """Apply shared rows, families in dependency order and form graphs.

    Requires every native service of the selected group to be stopped (the
    shared `server_change` gate refuses otherwise and never stops a process).
    """
    if not server_ini:
        raise ValueError('请先在机体导入模块配置 WorldServer INI')
    from manager.operations import server_change
    publication = load_publication(folder)
    order = family_order(publication.families)
    hosts = battle_hosts(server_ini) if publication.graph_requests else []
    if publication.graph_requests:
        missing = [row['host'] for row in hosts if not row['installed']]
        if missing or not hosts:
            raise ValueError('请先为 %s 安装配套框架的 %s' % ('、'.join(missing) or 'BattleServer', GRAPH_MOD))
    total = len(order)

    def report(stage, done=0, detail=''):
        if progress is not None:
            progress({'stage': stage, 'done': done, 'total': total, 'detail': detail})

    report('gate')
    with server_change(Path(server_ini), ACTION, ('fixed_catalog',)) as change:
        if publication.graph_requests:
            from . import graph_preservation
            if baseline is not None:
                if graph_context is not None:raise ValueError('保留形态图上下文重复')
                # Read the current graph/SQL/client witnesses under the same
                # group mutation gate as the incoming family writes.
                graph_context=graph_preservation.read(server_ini,publication,baseline)
            elif graph_context is None:
                graph_preservation.require_no_unrelated(hosts,publication)
        # Same ordered check as the preflight, against the locked live catalog,
        # so a refusal stops the batch before its first write.
        report('check')
        problems, _, _ = _simulate_families(repository.Repository(server_ini=Path(server_ini)), publication, order)
        if problems:
            raise ValueError('%s：%s（共 %d 个机体族未通过检查，未写入任何内容）' % (
                problems[0]['family_key'], problems[0]['error'], len(problems)))
        report('shared')
        shared = _apply_shared(server_ini, publication)
        repo = repository.Repository(server_ini=Path(server_ini))
        inserted = updated = 0
        for index, family in enumerate(order):
            report('families', index, family['family_key'])
            result = repo.apply(family_plan(family, publication.revision, repo.catalog['items']))
            inserted += sum(result['inserted'].values())
            updated += sum(result['updated'].values())
        report('families', total)
        graphs = {'requests': 0, 'hosts': []}
        if publication.graph_requests:
            report('graphs', total)
            graphs = install_graphs(server_ini, publication, hosts, change.directory / 'backups' / change.id,graph_context)
        report('readback', total)
        counts = readback(publication, server_ini, graphs)
        return change.result({'revision': publication.revision, 'owner': publication.owner,
                              'families': total, 'rows_inserted': inserted, 'rows_updated': updated,
                              'shared': shared, 'graphs': graphs, 'readback': counts,
                              'services_restarted': False, 'grants_units': False, 'applied': True})


# ---------------------------------------------------------------- readback

def readback(publication: Publication | str | Path, server_ini, graphs: dict | None = None) -> dict:
    """Count the publication's rows, ledger families and graph hosts (read-only)."""
    if not isinstance(publication, Publication):
        publication = load_publication(publication)
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()

        def count(table: str, key: str, ids: list[int]) -> int | None:
            if not ids:
                return 0
            if not _object_exists(cursor, table):
                return None
            return sum(int(cursor.execute('SELECT COUNT(*) FROM dbo.[%s] WHERE [%s] IN (%s)' % (table, key, ','.join('?' for _ in part)),
                                          *part).fetchone()[0]) for part in _chunks(ids))

        units = publication.unit_ids()
        result = {'plan_units': len(units), 'units': count('tbl_Units', 'UnitNumber', units),
                  'plan_weapons': len(publication.weapon_ids()), 'weapons': count('tbl_WeaponsRev', 'WeaponNo', publication.weapon_ids()),
                  'names': count('tbl_UnitNames', 'UnitNumber', units),
                  'plan_items': len(publication.item_ids()), 'items': count('tbl_ItemCategory', 'CatalogueID', publication.item_ids()),
                  'plan_skills': len(publication.skill_ids()), 'skills': count('tbl_SkillRev', 'SkillID', publication.skill_ids()),
                  'plan_funnels': len(publication.funnel_ids()), 'funnels': count('tbl_Funnel', 'FunnelID', publication.funnel_ids()),
                  'plan_affiliations': len(publication.influence_ids()),
                  'affiliations': count('tbl_Influences', 'InfluenceNumber', publication.influence_ids()),
                  'plan_families': len(publication.families), 'ledger_families': 0, 'ledger_families_at_revision': 0}
        if _object_exists(cursor, 'sdgo_UnitImportDefinitions'):
            keys = [family['family_key'] for family in publication.families]
            present, current = set(), set()
            for part in _chunks(keys):
                for family, revision in cursor.execute(
                        "SELECT DISTINCT FamilyKey,Revision FROM dbo.sdgo_UnitImportDefinitions WHERE RecordKind='unit' AND FamilyKey IN (%s)"
                        % ','.join('?' for _ in part), *part).fetchall():
                    present.add(str(family))
                    if str(revision) == publication.revision:
                        current.add(str(family))
            result['ledger_families'], result['ledger_families_at_revision'] = len(present), len(current)
    finally:
        try:
            connection.rollback()
        finally:
            connection.close()
    if publication.graph_requests:
        hosts = battle_hosts(server_ini)
        expected = graphs or {}
        result['graph_hosts'] = len(hosts)
        result['graph_hosts_installed'] = sum(
            1 for row in hosts if row['graph_sha256'] and row['authority_sha256'] and row['enabled'] and
            (not expected.get('candidate_sha256') or row['graph_sha256'] == expected['candidate_sha256']) and
            (not expected.get('authority_sha256') or row['authority_sha256'] == expected['authority_sha256']))
    else:
        result['graph_hosts'] = result['graph_hosts_installed'] = 0
    result['complete'] = (result['units'] == result['plan_units'] and result['weapons'] == result['plan_weapons'] and
                          result['names'] == result['plan_units'] and result['items'] == result['plan_items'] and
                          result['ledger_families_at_revision'] == result['plan_families'] and
                          result['graph_hosts_installed'] == result['graph_hosts'])
    return result
