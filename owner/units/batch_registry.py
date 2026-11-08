"""Batch roster publications that the Manager's unit host may offer to downstream editors.

A protocol-ready batch publication from any source is applied to the game database
by `batch_server.apply`. Registering it here records the publication folder only after a
read-back shows every family's rows and ownership ledger at the publication revision in
the configured database. Registered families then appear to the capsule, activity and
shop editors beside the Manager's own unit-import projects, with the same row shape.

Nothing here writes the game database, the client or any machine; registration is a
local record in the unit workspace.
"""
from __future__ import annotations

import datetime
import json
import os
from pathlib import Path
import re
import threading
from typing import Any

from . import batch_server
from .repository import database_target_key

REGISTRY = 'batch-publications.json'
SCHEMA = 'sdgo.unit-batch-registry/1'
REGISTRATIONS = 'registrations.json'
PACKAGE = 'package/client/sdgo/mods/unit_catalog'
MAX_REGISTRATIONS_BYTES = 64 * 1024 * 1024
MAX_PUBLICATIONS = 64
_SPRITE = re.compile(r'sprs/[a-z0-9_.-]+\.spr\Z')
_lock = threading.Lock()
_cache: dict[tuple, Any] = {}


def _path(workspace) -> Path:
    return Path(workspace) / REGISTRY


def load(workspace) -> list[dict]:
    path = _path(workspace)
    if not path.is_file():
        return []
    value = json.loads(path.read_text('utf-8'))
    if not isinstance(value, dict) or value.get('schema') != SCHEMA or not isinstance(value.get('publications'), list):
        raise ValueError('批量阵容登记文件格式无效')
    return value['publications']


def _save(workspace, publications: list[dict]) -> None:
    path = _path(workspace)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps({'schema': SCHEMA, 'publications': publications}, ensure_ascii=False, indent=1), 'utf-8')
    os.replace(temporary, path)


def _complete(counts: dict) -> list[str]:
    """Names of the read-back counts that do not match the publication."""
    pairs = (('units', 'plan_units'), ('weapons', 'plan_weapons'), ('names', 'plan_units'), ('items', 'plan_items'),
             ('skills', 'plan_skills'), ('funnels', 'plan_funnels'), ('ledger_families_at_revision', 'plan_families'))
    return [name for name, plan in pairs if counts.get(name) != counts.get(plan)]


def register(workspace, folder, server_ini) -> dict:
    """Record an applied publication; refuse one that is not completely applied at its revision."""
    publication = batch_server.load_publication(folder)
    counts = batch_server.readback(publication, server_ini)
    missing = _complete(counts)
    if missing:
        raise ValueError('该批量阵容尚未完整应用到当前服务器：' + '、'.join(missing) + '；请先在批量阵容面板应用')
    _registration_items(publication)
    entry = {'folder': str(publication.root), 'owner': publication.owner, 'revision': publication.revision,
             'server_target': database_target_key(Path(server_ini)), 'families': len(publication.families),
             'registered_at': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}
    with _lock:
        publications = [row for row in load(workspace)
                        if not (row['owner'] == entry['owner'] and row['server_target'] == entry['server_target'])]
        if len(publications) >= MAX_PUBLICATIONS:
            raise ValueError('登记的批量阵容过多')
        publications.append(entry)
        _save(workspace, publications)
    return {**entry, 'readback': counts}


def unregister(workspace, owner: str, server_target: str) -> bool:
    with _lock:
        publications = load(workspace)
        kept = [row for row in publications if not (row['owner'] == owner and row['server_target'] == server_target)]
        if len(kept) != len(publications):
            _save(workspace, kept)
        return len(kept) != len(publications)


def _registration_items(publication) -> list[dict]:
    path = publication.root / REGISTRATIONS
    if not path.is_file() or path.stat().st_size > MAX_REGISTRATIONS_BYTES:
        raise ValueError('批量发布缺少 registrations.json')
    value = json.loads(path.read_text('utf-8'))
    items = value.get('items') if isinstance(value, dict) else None
    if not isinstance(items, list):
        raise ValueError('批量发布的 registrations.json 格式无效')
    owned = {family['family_key']: family for family in publication.families}
    result = []
    for item in items:
        if not isinstance(item, dict) or item.get('family_key') not in owned:
            continue  # carried families keep their own owner (for example a Manager project)
        family = owned[item['family_key']]
        root = item.get('unit_id')
        if type(root) is not int or root != family['root_unit_id'] or item.get('category_id') != family['item']['CatalogueID']:
            raise ValueError('批量发布的登记与服务端计划不一致：' + str(item.get('family_key')))
        for key in ('capsule_sprite', 'custom_candidate_sprite', 'unit_primary_sprite', 'shop_sprite'):
            if not isinstance(item.get(key), str) or not _SPRITE.match(item[key]):
                raise ValueError('批量发布的登记图片路径无效：' + str(item.get('family_key')))
        result.append(item)
    return result


def _publication(entry: dict):
    folder = Path(entry['folder'])
    plan = folder / 'server-plan.json'
    key = ('publication', str(folder).casefold(), plan.stat().st_mtime_ns if plan.is_file() else 0)
    with _lock:
        cached = _cache.get(key)
    if cached is None:
        publication = batch_server.load_publication(folder)
        cached = (publication, _registration_items(publication))
        with _lock:
            for stale in [k for k in _cache if k[0] == 'publication' and k[1] == key[1]]:
                _cache.pop(stale)
            _cache[key] = cached
    publication, items = cached
    if publication.revision != entry['revision']:
        raise ValueError('批量发布目录已改变，请重新应用并登记：' + str(folder))
    return publication, items


def rows(workspace, server_ini) -> list[dict]:
    """Registration rows (the Manager project row shape) for the configured database."""
    if not server_ini:
        return []
    target = database_target_key(Path(server_ini))
    result = []
    for entry in load(workspace):
        if entry.get('server_target') != target:
            continue
        publication, items = _publication(entry)
        families = {family['family_key']: family for family in publication.families}
        for item in items:
            family = families[item['family_key']]
            root = next(row for row in family['units'] if int(row['UnitNumber']) == item['unit_id'])
            result.append({
                'key': item['key'], 'provider': 'unit_catalog', 'kind': 'unit',
                'activation': {'status': 'active', 'source': 'batch'},
                'unit_id': item['unit_id'], 'category_id': item['category_id'], 'family_key': item['family_key'],
                'revision': publication.revision, 'display_name': item['display_name'], 'forms': list(item['forms']),
                'rank': int(root['UnitGradeNumber']), 'subgrade': int(root['UnitGradeNumber2']),
                'shop_sprite': item['shop_sprite'], 'secret_store_sprite': item.get('secret_store_sprite'),
                'unit_primary_sprite': item['unit_primary_sprite'], 'capsule_sprite': item['capsule_sprite'],
                'custom_candidate_sprite': item['custom_candidate_sprite'],
                'server_applied': True, 'server_target': target,
                'batch': {'owner': publication.owner, 'revision': publication.revision, 'folder': str(publication.root)}})
    return result


def _overlay(folder: Path):
    from configparser import ConfigParser
    from owner.native_archive import OverlayReader
    package = folder / PACKAGE
    parser = ConfigParser(interpolation=None)
    parser.read_string((package / 'resource-overlay.ini').read_text('utf-8-sig'))
    runtime = parser['runtime']
    parts = int(runtime.get('parts', '1'))
    return OverlayReader((package / runtime['index']).read_bytes(), package / runtime['archive'],
                         parts, int(runtime.get('part_chunks', '0')) if parts > 1 else 0)


def sprite_png(row: dict, field: str = 'capsule_sprite', baseline_client=None) -> bytes:
    """Render one of a batch family's published sprites to PNG from the publication's own archive.

    The publisher does not ship resources that are byte-identical to the baseline client (the game reads those from
    its own archive), so a sprite or texture missing from the publication is read from `baseline_client`'s archive
    when one is configured.
    """
    from owner.units.assets import Baseline, render_portrait
    if field not in ('capsule_sprite', 'custom_candidate_sprite', 'unit_primary_sprite', 'shop_sprite'):
        raise ValueError('批量机体图片类型无效')
    batch = row.get('batch')
    if not isinstance(batch, dict):
        raise ValueError('该机体不是批量阵容登记')
    key = ('png', batch['folder'].casefold(), batch['revision'], row[field], str(baseline_client or '').casefold())
    with _lock:
        cached = _cache.get(key)
    if cached is not None:
        return cached
    baseline = []
    with _overlay(Path(batch['folder'])) as reader:
        def read(path):
            if reader.has(path) or not baseline_client:
                return reader.read(path)
            if not baseline:
                archive = Baseline(Path(baseline_client))
                archive._loose_roots = []  # archive only, like the publisher's baseline comparison
                baseline.append(archive)
            if not baseline[0].exists(path):
                return reader.read(path)  # reports the missing resource
            return baseline[0].read(path)
        raw = render_portrait(row[field], read)
    with _lock:
        if sum(1 for k in _cache if k[0] == 'png') >= 512:
            for stale in [k for k in _cache if k[0] == 'png'][:128]:
                _cache.pop(stale)
        _cache[key] = raw
    return raw
