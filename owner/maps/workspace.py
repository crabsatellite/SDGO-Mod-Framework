"""Owner workspace for imported maps and missions.

A workspace keeps the registered map packages (copied in, digest-checked) in a fixed order, together with
the owner's edits: display name, PvP modes, server types and whether the map is published. Building
compiles every enabled map into ONE client package and ONE server plan.

Native IDs are allocated at build time, sequentially and without gaps, right after the target client's own
rows: the client walks map rows from 100001 and the listed-mission tables (GameMode, route, objective,
catalogue, Clear, speech, NPC template) as contiguous ID blocks, so a gap would hide everything after it.
Removing or reordering a map therefore renumbers the maps after it; the server apply updates and prunes the
rows this workspace owns accordingly.
"""
from __future__ import annotations

import dataclasses
import hashlib
import json
import re
import shutil
from pathlib import Path

from . import compiler, mission as M, package as P

REGISTRY_SCHEMA = 'sdgo.map-workspace'
OWNER_KEY = 'sdgo.map-workspace'        # server ledger owner of every row a workspace build publishes
MAP_ID_FIRST = 500                       # server-only MapData IDs (no client walk)
MAX_MAPS = 32
KEY = re.compile(r'[a-z0-9][a-z0-9._-]{2,79}')
NAME_LIMIT = 48


class WorkspaceError(ValueError):
    pass


def _fail(message: str):
    raise WorkspaceError(message)


class MapWorkspace:
    def __init__(self, root):
        self.root = Path(root)
        self.packages = self.root / 'packages'
        self.registry_path = self.root / 'maps.json'

    # ------------------------------------------------------------------ registry
    def load(self) -> list[dict]:
        if not self.registry_path.is_file():
            return []
        data = json.loads(self.registry_path.read_text(encoding='utf-8'))
        if data.get('schema') != REGISTRY_SCHEMA or data.get('schema_version') != 1 or \
                not isinstance(data.get('maps'), list):
            _fail('地图工作区登记文件无效')
        return data['maps']

    def _save(self, maps: list[dict]) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        temporary = self.registry_path.with_suffix('.json.new')
        temporary.write_text(json.dumps({'schema': REGISTRY_SCHEMA, 'schema_version': 1, 'maps': maps},
                                        ensure_ascii=False, indent=2), encoding='utf-8')
        temporary.replace(self.registry_path)

    def _entry(self, maps: list[dict], key: str) -> dict:
        for entry in maps:
            if entry['key'] == key:
                return entry
        _fail('地图未登记：' + key)

    def package(self, entry: dict) -> P.Package:
        pkg = P.load(self.packages / entry['key'])
        if hashlib.sha256((self.packages / entry['key'] / 'map.json').read_bytes()).hexdigest() != entry['digest']:
            _fail(f'{entry["key"]} 的地图包已被改动，请重新导入')
        edits = entry.get('edits') or {}
        name = dict(pkg.name)
        if edits.get('name_zh'):
            name['zh'] = edits['name_zh']
        if edits.get('name_en'):
            name['en'] = edits['name_en']
        return dataclasses.replace(pkg, name=name, modes=list(edits.get('modes', pkg.modes)),
                                   servers=list(edits.get('servers', pkg.servers)))

    def add(self, source) -> dict:
        """Copy a map package folder into the workspace (validated first). Re-adding a key replaces its files."""
        source = Path(source)
        pkg = P.load(source)
        if not KEY.fullmatch(pkg.map_key):
            _fail('map_key 只能使用小写字母、数字、点、下划线和连字符')
        maps = self.load()
        if len(maps) >= MAX_MAPS and all(m['key'] != pkg.map_key for m in maps):
            _fail(f'一个工作区最多登记 {MAX_MAPS} 张地图')
        files = ['map.json'] + sorted(pkg.manifest.get('files', {}))
        target = self.packages / pkg.map_key
        staging = self.packages / (pkg.map_key + '.new')
        if staging.exists():
            shutil.rmtree(staging)
        for relative in files:
            raw = (source / relative).read_bytes() if relative == 'map.json' else pkg.read(relative)
            path = staging / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        P.load(staging)                                   # the copy must validate on its own
        if target.exists():
            shutil.rmtree(target)
        staging.replace(target)
        digest = hashlib.sha256((target / 'map.json').read_bytes()).hexdigest()
        missions = sorted(m for m in pkg.manifest.get('files', {}) if m.startswith('missions/') and m.endswith('.json'))
        for entry in maps:
            if entry['key'] == pkg.map_key:
                entry.update(digest=digest, missions=[m for m in entry.get('missions', missions) if m in missions]
                             or missions)
                break
        else:
            maps.append({'key': pkg.map_key, 'digest': digest, 'enabled': True, 'missions': missions, 'edits': {}})
        self._save(maps)
        return self.describe_entry(self._entry(maps, pkg.map_key))

    def update(self, key: str, values: dict) -> dict:
        if not isinstance(values, dict) or set(values) - {'name_zh', 'name_en', 'modes', 'servers', 'enabled',
                                                          'missions', 'position'}:
            _fail('地图设置包含未支持的字段')
        maps = self.load()
        entry = self._entry(maps, key)
        pkg = P.load(self.packages / key)
        edits = dict(entry.get('edits') or {})
        for field in ('name_zh', 'name_en'):
            if field in values:
                text = values[field]
                if not isinstance(text, str) or len(text) > NAME_LIMIT or any(ord(c) < 32 for c in text):
                    _fail(f'地图名称最多 {NAME_LIMIT} 个字符')
                if text.strip():
                    edits[field] = text.strip()
                else:
                    edits.pop(field, None)
        if 'modes' in values:
            modes = values['modes']
            if not isinstance(modes, list) or any(m not in P.PVP_MODES for m in modes) or len(set(modes)) != len(modes):
                _fail('对战模式无效')
            edits['modes'] = [m for m in P.PVP_MODES if m in modes]
        if 'servers' in values:
            servers = values['servers']
            if not isinstance(servers, list) or not servers or any(s not in P.SERVER_TYPES for s in servers):
                _fail('至少选择一种服务器类型')
            edits['servers'] = [s for s in P.SERVER_TYPES if s in servers]
        if 'enabled' in values:
            if not isinstance(values['enabled'], bool):
                _fail('enabled 必须是布尔值')
            entry['enabled'] = values['enabled']
        if 'missions' in values:
            listed = sorted(m for m in pkg.manifest.get('files', {}) if m.startswith('missions/') and m.endswith('.json'))
            chosen = values['missions']
            if not isinstance(chosen, list) or any(m not in listed for m in chosen):
                _fail('只能选择地图包内的任务文件')
            entry['missions'] = [m for m in listed if m in chosen]
        entry['edits'] = edits
        if not edits.get('modes', pkg.modes) and not entry['missions'] and entry['enabled']:
            _fail('启用的地图至少需要一种对战模式或一个任务')
        if 'position' in values:
            position = values['position']
            if not isinstance(position, int) or not 0 <= position < len(maps):
                _fail('排序位置无效')
            maps.remove(entry)
            maps.insert(position, entry)
        self._save(maps)
        return self.describe_entry(entry)

    def remove(self, key: str) -> dict:
        maps = self.load()
        entry = self._entry(maps, key)
        maps.remove(entry)
        self._save(maps)
        target = self.packages / key
        if target.exists():
            shutil.rmtree(target)
        return {'removed': key, 'remaining': len(maps)}

    def describe_entry(self, entry: dict) -> dict:
        try:
            pkg = self.package(entry)
        except (ValueError, OSError) as error:
            return {'key': entry['key'], 'enabled': entry.get('enabled', True), 'error': str(error)[:300]}
        missions = []
        for relative in entry.get('missions', []):
            try:
                spec = json.loads(pkg.read(relative))
                missions.append({'file': relative, 'name': (spec.get('name') or {}).get('zh', ''),
                                 'difficulties': len(spec.get('difficulties') or []),
                                 'phases': len(spec.get('phases') or [])})
            except (ValueError, OSError):
                missions.append({'file': relative, 'error': '任务文件无法读取'})
        all_missions = sorted(m for m in pkg.manifest.get('files', {}) if m.startswith('missions/') and m.endswith('.json'))
        return {'key': entry['key'], 'enabled': entry.get('enabled', True), 'name': pkg.name,
                'description': pkg.description, 'modes': pkg.modes, 'servers': pkg.servers,
                'half_extent': pkg.half_extent, 'objects': len(pkg.objects), 'missions': missions,
                'available_missions': all_missions, 'edits': entry.get('edits') or {}}

    def describe(self) -> dict:
        return {'maps': [self.describe_entry(e) for e in self.load()], 'pvp_modes': list(P.PVP_MODES),
                'server_types': list(P.SERVER_TYPES)}

    # ------------------------------------------------------------------ build
    def build(self, client, server_ini, output, *, program: dict[str, bytes] | None = None,
              log=print) -> dict:
        """Compile every enabled map (and its selected missions) into `output` (client ZIP + server plan)."""
        from owner.units.assets import Baseline
        from . import bundle, pipeline
        client = Path(client).resolve()
        baseline = Baseline(client)
        enabled = [e for e in self.load() if e.get('enabled', True)]
        if not enabled:
            _fail('没有启用的地图')
        native_exists = pipeline.native_exists_for(client, baseline)
        cursor = pipeline.next_ids(baseline)
        templates = None
        banner = None
        builds, missions, assignments = [], [], []
        map_no = cursor['map_no']
        for index, entry in enumerate(enabled):
            pkg = self.package(entry)
            pipeline.check_placements(pkg, baseline)
            shared: dict = {}
            mission_files = list(entry.get('missions', []))
            first = None
            variants = []
            if pkg.modes:
                variants.append(('pvp', map_no))
                map_no += 1
            if mission_files:
                variants.append(('mission', map_no))
                map_no += 1
            if not variants:
                _fail(f'{entry["key"]} 没有对战模式也没有任务')
            if map_no - 1 > 0xFF:
                _fail('地图编号超过 255，请减少登记的地图')
            map_id = MAP_ID_FIRST + index
            mission_build = None
            for variant, number in variants:
                build = compiler.compile_map(pkg, map_no=number, map_id=map_id, variant=variant,
                                             prefix=first.prefix if first else None,
                                             native_exists=native_exists, shared=shared)
                compiler.verify_trn(build.resources[build.report['trn'].replace('\\', '/').lower()])
                builds.append(build)
                first = first or build
                if variant == 'mission':
                    mission_build = build
            assignment = {'key': entry['key'], 'map_id': map_id, 'map_nos': {v: n for v, n in variants},
                          'missions': []}
            for relative in mission_files:
                if templates is None:
                    templates = pipeline.mission_templates(baseline, server_ini)
                    banner = M.banner_effect_factory(lambda path: baseline.read(path))
                spec = json.loads(pkg.read(relative))
                ids = pipeline.mission_ids(cursor, len(missions))
                if 'occluders' not in assignment:
                    assignment['occluders'] = pipeline.occluders(pkg, baseline)
                built = M.compile_mission(pkg, spec, map_no=assignment['map_nos']['mission'],
                                          terrain_path=mission_build.report['trn'], ids=ids, templates=templates,
                                          banner_effect=banner, occluders=assignment['occluders'])
                for kind, ident, _ in built.rows:
                    if baseline.any_id(ident):
                        _fail(f'任务记录 {ident} 与客户端已有记录冲突')
                pipeline.advance(cursor, built)
                missions.append(built)
                assignment['missions'].append({'file': relative, **built.report})
            assignment.pop('occluders', None)
            assignments.append(assignment)
            log(json.dumps({'map': entry['key'], 'map_nos': assignment['map_nos'],
                            'missions': len(assignment['missions'])}, ensure_ascii=False))
        output = Path(output)
        report = bundle.build_publication(builds, output, program=program, missions=missions)
        report['owner_key'] = OWNER_KEY
        report['maps'] = assignments
        (output / 'workspace-build.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                                     encoding='utf-8')
        return report
