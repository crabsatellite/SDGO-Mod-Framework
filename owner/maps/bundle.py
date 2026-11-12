"""Turn compiled maps into a publication: a client package ZIP and a server plan.

Publication folder layout:
    SDGO-MapImport-Client.zip   manifest.json + client/sdgo/mods/map_catalog/* (+ client/BGM/* loose music)
    server-plan.json            rows per server table, keyed for idempotent apply
    report.json                 compile reports

The client ZIP is installed by `owner.maps.deployment.install_package` (Manager dispatches on
manifest module ``map_catalog``). It never contains game executables, original archives or credentials.
"""
from __future__ import annotations

import hashlib
import io
import json
import re
import struct
import zipfile
from pathlib import Path

from owner.native_archive import build_archive

from .compiler import CompiledMap

MODULE = 'map_catalog'
PREFIX = 'client/sdgo/mods/map_catalog/'
PLAN_SCHEMA = 'sdgo.map-import-server-plan/3'
LEGACY_PLAN_SCHEMAS = frozenset({'sdgo.map-import-server-plan/1', 'sdgo.map-import-server-plan/2'})
OVERLAY_DOMAINS = {'trns': '.trn', 'sol': '.sol', 'mdrs': '.mod', 'anrs': '.ani', 'txrs': '.txr', 'fxrs': '.zfx',
                   'sprs': '.spr', 'sdrs': '.zsd'}
TABLE_KEYS = {
    'tbl_MapFile': ('MapNo',),
    'tbl_MapData': ('MapID',),
    'tbl_MapItemPath': ('MapID', 'Team', 'PatternID', 'Orders'),
    'tbl_MapObjects': ('ObjectID',),
    'tbl_MapBlocks': ('MapID', 'BlockID'),
    'tbl_MissionMode': ('GameModeID',),
    'tbl_MissionClear': ('ClearID',),
    'tbl_MissionModeStart': ('MissionModeStartID',),
    'tbl_MissionNpc': ('MissionNpcID',),
    'tbl_MissionExp': ('MissionExpID',),
    'tbl_WeaponsRev': ('WeaponNo',),
    'tbl_UnitNpc': ('UnitNumber',),
    'tbl_NpcData': ('NpcDataID',),
    'tbl_MissionRankBossData': ('MissionID',),
    'tbl_BattleResultItemDropSetting': ('DropConditionID',),
    'tbl_BattleResultItemDropDice': ('ItemMachineID', 'SumProb'),
}


def _fnv(raw: bytes) -> int:
    value = 2166136261
    for byte in raw:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def native_catalog(rows: list[tuple[int, int, bytes]], flags: dict[int, int] | None = None) -> bytes:
    from .mission import SIZES
    from .block import CATALOG_KIND, RECORD_BYTES
    expected = {0x0F: 0x34C, CATALOG_KIND: RECORD_BYTES, **SIZES}
    flags = flags or {}
    if set(flags) - {ident for _kind, ident, _raw in rows}:
        raise ValueError('catalog flags reference an undeclared record')
    for kind, ident, raw in rows:
        if expected.get(kind) != len(raw):
            raise ValueError(f'record {ident} kind {kind:#x} has {len(raw)} bytes, expected {expected.get(kind)}')
        value = flags.get(ident, 0)
        if type(value) is not int or value < 0 or (value and
                (kind != 0x2A or not value & 1 or value & ~0xFF01)):
            raise ValueError('unsupported native catalog record flags')
    payload = b''.join(struct.pack('<6I', kind, ident, len(raw), _fnv(raw), flags.get(ident, 0), 0) + raw
                       for kind, ident, raw in rows)
    return struct.pack('<8s6I', b'SDGOMP1\0', 1, 32, len(rows), len(payload), _fnv(payload), 0) + payload


def allowed_overlay(name: str) -> bool:
    folder = name.split('/', 1)[0]
    return folder in OVERLAY_DOMAINS and name.endswith(OVERLAY_DOMAINS[folder])


def _write_zip(path: Path, files: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, files[name])


def build_publication(builds: list[CompiledMap], output: Path, *, program: dict[str, bytes] | None = None,
                      definitions: dict | None = None, missions: list | None = None) -> dict:
    """Write the client ZIP and server plan for `builds` (variants of one or more maps).

    `program` optionally adds the map_catalog Mod itself (``map_catalog.dll``, ``mod.ini``, ``manager.json``,
    ``symbols.fragment.ini``) for owner content that ships with its own module build.
    """
    if not builds:
        raise ValueError('nothing to publish')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    resources: dict[str, bytes] = {}
    loose: dict[str, bytes] = {}
    rows: list[tuple[int, int, bytes]] = []
    for build in builds:
        for name, raw in build.resources.items():
            if name in resources and resources[name] != raw:
                raise ValueError(f'resource {name} differs between builds')
            resources[name] = raw
        loose.update(build.loose)
        rows.extend(build.rows)
    lua = ''
    for mission in missions or []:
        for name, raw in mission.resources.items():
            if name in resources and resources[name] != raw:
                raise ValueError(f'resource {name} differs between builds')
            resources[name] = raw
        rows.extend(mission.rows)
        lua += mission.lua
    mission_reports = [m.report for m in missions or []]
    immediate_transition_missions = sorted({
        mode_id for report in mission_reports if report.get('transition_delay', 'native') == 'immediate'
        for mode_id in report['game_modes']
    })
    runtime_policy = {'immediate_transition_missions': immediate_transition_missions}
    from . import boundary_format
    boundaries = boundary_format.normalize([row for report in mission_reports for row in report.get('boundaries', [])])
    if boundaries:
        runtime_policy['boundaries'] = boundaries
    rows.sort(key=lambda row: (row[0] != 0x0F, row[0], row[1]))
    ids = [row[1] for row in rows]
    if len(set(ids)) != len(ids):
        raise ValueError('duplicate native record id')
    flags = {ident: value for mission in missions or [] for ident, value in mission.record_flags.items()}
    catalog = native_catalog(rows, flags)
    zdx, zpk, _ = build_archive(resources, allowed_overlay)
    overlay = ('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256=' + hashlib.sha256(zdx).hexdigest() +
               '\narchive=resources.zpk\narchive_sha256=' + hashlib.sha256(zpk).hexdigest() + '\n').encode('ascii')
    revision = hashlib.sha256(json.dumps({
        'catalog': hashlib.sha256(catalog).hexdigest(),
        'resources': {k: hashlib.sha256(v).hexdigest() for k, v in sorted(resources.items())},
        'loose': {k: hashlib.sha256(v).hexdigest() for k, v in sorted(loose.items())},
        'lua': hashlib.sha256(lua.encode('ascii')).hexdigest(),
        'server': hashlib.sha256(json.dumps([b.server for b in list(builds) + list(missions or [])],
                                            sort_keys=True, default=str).encode()).hexdigest(),
        'runtime_policy': runtime_policy,
    }, sort_keys=True).encode()).hexdigest()[:16]
    maps = [{'map_key': b.report['map_key'], 'map_no': b.map_no, 'map_id': b.map_id, 'variant': b.variant,
             'trn': b.report['trn'], 'blocks': b.report.get('blocks', [])} for b in builds]
    files = {PREFIX + 'catalog.bin': catalog, PREFIX + 'resource-overlay.ini': overlay,
             PREFIX + 'resources.zdx': zdx, PREFIX + 'resources.zpk': zpk,
             PREFIX + 'definitions.json': json.dumps({'protocol': 'sdgo.map-package', 'schema_version': 2,
                                                      'revision': revision, 'maps': maps, 'missions': mission_reports,
                                                      **(definitions or {})},
                                                     ensure_ascii=False, indent=2).encode('utf-8')}
    if lua:
        files[PREFIX + 'missions.lua'] = lua.encode('ascii')
    if boundaries:
        files[PREFIX + 'boundaries.bin'] = boundary_format.encode(boundaries)
    for name, raw in (program or {}).items():
        if name not in ('map_catalog.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'):
            raise ValueError(f'unexpected program file {name}')
        if name == 'mod.ini':
            raw = re.sub(rb'(?m)^enabled\s*=.*$', b'enabled=1', raw)
        files[PREFIX + name] = raw
    for name, raw in loose.items():
        if not re.fullmatch(r'BGM/m[0-9]{3}[a-z0-9]*\.mp3', name):
            raise ValueError(f'unexpected loose file {name}')
        files['client/' + name] = raw
    manifest = {'module': MODULE, 'schema_version': 1, 'revision': revision, 'maps': maps,
                'files': {name: hashlib.sha256(raw).hexdigest() for name, raw in sorted(files.items())},
                'owned_resources': sorted(resources), 'loose': sorted('client/' + n for n in loose),
                'restart_required': True}
    files['manifest.json'] = json.dumps(manifest, ensure_ascii=False, indent=2).encode('utf-8')
    client_zip = output / 'SDGO-MapImport-Client.zip'
    _write_zip(client_zip, files)

    tables: dict[str, list[dict]] = {}
    for build in list(builds) + list(missions or []):
        for table, items in build.server.items():
            keyed = tables.setdefault(table, [])
            for item in items:
                key = tuple(item[k] for k in TABLE_KEYS[table])
                same = [row for row in keyed if tuple(row[k] for k in TABLE_KEYS[table]) == key]
                if same and same[0] != item:
                    raise ValueError(f'{table} row {key} differs between builds')
                if not same:
                    keyed.append(item)
    plan = {'schema': PLAN_SCHEMA, 'revision': revision, 'maps': maps, 'keys': TABLE_KEYS, 'tables': tables,
            'runtime_policy': runtime_policy, 'credentials_included': False}
    (output / 'server-plan.json').write_text(json.dumps(plan, ensure_ascii=False, indent=1), encoding='utf-8')
    (output / 'report.json').write_text(json.dumps([b.report for b in builds], ensure_ascii=False, indent=1),
                                        encoding='utf-8')
    return {'revision': revision, 'client_zip': str(client_zip),
            'client_zip_sha256': hashlib.sha256(client_zip.read_bytes()).hexdigest(),
            'server_plan': str(output / 'server-plan.json'), 'records': len(rows), 'resources': len(resources)}
