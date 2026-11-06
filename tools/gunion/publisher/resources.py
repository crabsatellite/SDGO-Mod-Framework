"""Stage 2: resource closure with owner aliases for every publishable family.

Each family's recovered dependency graph is reduced to the resources the
owner's client must ship: resources absent from the selected baseline keep
their logical path, resources whose bytes differ from the baseline (or whose
logical path carries several recovered contents) receive an owner alias, and
every typed carrier that names an aliased resource is rewritten in place
(records, model material tables, model OBS strings, effect textures/meshes,
sprite textures). Baseline-identical resources are not shipped. Carriers that
no typed decoder covers are reported as open, never patched by byte scanning.
"""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import struct
import zipfile

from owner.units.assets import Baseline
from tools.gunion.native_alias_carriers import (Carrier, effect_carriers, field, fixed_records, model_materials,
                                                obs_carriers, rewrite_prefixes, sprite_carriers)
from tools.gunion.mod_parser import find_kudiz_offset

from .common import Config, FUNNEL_BYTES, SKILL_BYTES, UNIT_BYTES, WEAPON_BYTES, dump_json, load_json, normalize_path, sha256

SCHEMA = 'sdgo.gunion-roster-resources/1'
SKIP, PLAIN, ALIAS = 'baseline-identical', 'absent-ship-logical-path', 'owner-alias'
UNSUPPORTED = 'unsupported-native-type'
RUNTIME_ROOTS = ('mdrs', 'anrs', 'txrs', 'fxrs', 'sprs', 'sdrs')
# The original loaders read exactly one container type per root; anything else
# (a stray .psd under txrs) is a source authoring leftover the client never opens.
NATIVE_SUFFIX = {'mdrs': '.mod', 'anrs': '.ani', 'txrs': '.txr', 'fxrs': '.zfx', 'sprs': '.spr', 'sdrs': '.zsd'}


def _alias(owner: str, path: str, digest: str) -> str:
    owner_hash = sha256(owner.encode())[:8]
    token = sha256((path + '\0' + digest).encode())[:11]
    return path.split('/')[0] + '/o' + owner_hash + token + PurePosixPath(path).suffix


def _skill_carriers(raw: bytes, opaque: list | None = None) -> list:
    if len(raw) != SKILL_BYTES:
        raise ValueError('skill target record extent')
    try:
        carrier = field(raw, 358, 60, 'utf-16-le', 'skill-effect', 'fixed-native-grid-field')
    except (ValueError, UnicodeError) as error:
        if opaque is not None:
            literal = raw[358:418].split(b'\0\0')[0].decode('utf-16-le', errors='replace')
            opaque.append({'offset': 358, 'role': 'skill-effect', 'literal': literal[:60], 'reason': str(error)})
        return []
    return [carrier] if carrier else []


_UNIT_FIELDS = ((612, 60, 'unit-model'), (4654, 100, 'unit-portrait-shop'), (4754, 100, 'unit-portrait-capsule'),
                (4854, 100, 'unit-portrait-detail'), (0xF4C, 100, 'unit-sword-texture'))
_FUNNEL_FIELDS = tuple((6 + i * 100, 100, 'fang-model-slot-%d' % i) for i in range(6))


def _record_carriers(kind: int, raw: bytes, opaque: list | None = None) -> list:
    """Typed fixed fields of one record; literals outside the bounded path grammar stay opaque."""
    from tools.gunion.minecraft_fields import WEAPON_TEXT
    if kind == 2:
        fields = _UNIT_FIELDS
    elif kind == 10:
        names = ['muzzleEffectPath', 'impactEffectPath', 'soundPath', 'projectileModelPath', 'weaponHudTexturePath',
                 'weaponHudSelectedTexturePath', 'projectileTexturePath', 'secondarySoundPath']
        fields = tuple((*WEAPON_TEXT[name], 'weapon-' + name) for name in names)
    elif kind == 13:
        return _skill_carriers(raw, opaque)
    elif kind == 11:
        fields = _FUNNEL_FIELDS
    else:
        raise ValueError('unsupported record kind %d' % kind)
    result = []
    for offset, size, role in fields:
        try:
            carrier = field(raw, offset, size, 'utf-16-le', role, 'fixed-native-grid-field')
        except (ValueError, UnicodeError) as error:
            if opaque is not None:
                literal = raw[offset:offset + size].split(b'\0\0')[0].decode('utf-16-le', errors='replace')
                opaque.append({'offset': offset, 'role': role, 'literal': literal[:60], 'reason': str(error)})
            continue
        if carrier:
            result.append(carrier)
    return result


class _Delivery:
    def __init__(self, config: Config):
        self.config = config
        self.native = config.native
        self._packages = {}

    def payload(self, node: dict) -> bytes:
        payload = node['payload']
        if 'table' in payload:
            raw = self.config.read_delivery('native/' + payload['table'])[payload['offset']:payload['offset'] + payload['bytes']]
        else:
            raw = self.config.read_delivery('native/' + payload['path'])
        if len(raw) != payload['bytes'] or sha256(raw) != payload['sha256']:
            raise ValueError('delivery payload differs: ' + node['key'])
        return raw

    def package_member(self, zip_relative: str, member: str) -> bytes | None:
        archive = self._packages.get(zip_relative)
        if archive is None:
            path = (self.config.delivery / zip_relative).resolve()
            if not path.is_file():
                self._packages[zip_relative] = False
                return None
            archive = zipfile.ZipFile(io.BytesIO(path.read_bytes()))
            self._packages[zip_relative] = archive
        if archive is False:
            return None
        try:
            return archive.read(member)
        except KeyError:
            return None


def _portrait_sources(delivery: _Delivery, family: dict, path: str) -> bytes | None:
    """Portrait sprites/textures generated for authoring packages are not graph nodes."""
    candidates = ['native/design-adaptation-v2/family-packages/unit-%d.zip' % family['source_root_unit_id'],
                  'native/design-adaptation-v1/family-packages/unit-%d.zip' % family['source_root_unit_id']]
    candidates += ['native/authoring-packages-v3/unit-%d.zip' % form['source_id'] for form in family['forms']]
    for relative in candidates:
        raw = delivery.package_member(relative, path)
        if raw is not None:
            return raw
    return None


def build(config: Config) -> dict:
    families = load_json(config.output / 'families.json')
    delivery = _Delivery(config)
    graph_root = config.native / 'family-resource-graph-v3'
    manifest = load_json(graph_root / 'manifest.json')
    graph_index = {int(row['sourceRootUnitId']): row for row in manifest['families']}
    family_graphs, needed = {}, set()
    for family in families['families']:
        row = graph_index.get(family['source_root_unit_id'])
        if row is None:
            raise ValueError('no resource graph for family ' + family['family_key'])
        graph = load_json(graph_root / row['path'])
        family_graphs[family['family_key']] = graph
        needed.update(graph['nodeKeys'])
    nodes = {}
    with (graph_root / 'nodes.jsonl').open('r', encoding='utf-8') as stream:
        for line in stream:
            node = json.loads(line)
            if node['key'] in needed:
                nodes[node['key']] = node
    missing_nodes = sorted(needed - set(nodes))
    if missing_nodes:
        raise ValueError('graph nodes missing from nodes.jsonl: %s' % missing_nodes[:5])
    baseline = Baseline(config.baseline_client)
    baseline._loose_roots = []
    plan = load_json(config.plan_dir / 'plan.json')
    plan_records: dict[tuple[int, int], bytes] = {}
    for row in plan['requests']:
        if row['kind'] in (11, 13):
            plan_records[(row['kind'], row['requestedTargetId'])] = (config.plan_dir / row['generatedMember']).read_bytes()

    # Pass 1: every recovered resource content per logical path, across all selected
    # families. Bytes are loaded lazily: only carrier-bearing types stay cached.
    sources: dict[tuple[str, str], object] = {}
    contents: dict[str, set[str]] = {}
    cache: dict[tuple[str, str], bytes] = {}
    CACHED_SUFFIXES = ('.mod', '.zfx', '.spr')

    def remember(path: str, digest: str, source) -> None:
        contents.setdefault(path, set()).add(digest)
        sources.setdefault((path, digest), source)

    def read(path: str, digest: str) -> bytes:
        key = (path, digest)
        if key in cache:
            return cache[key]
        source = sources[key]
        raw = source if isinstance(source, bytes) else delivery.payload(source)
        if sha256(raw) != digest:
            raise ValueError('resource content digest differs: ' + path)
        if PurePosixPath(path).suffix in CACHED_SUFFIXES:
            cache[key] = raw
        return raw

    for key, node in nodes.items():
        if node['kind'] != 'resource':
            continue
        path = normalize_path(node['logical_path'])
        if path.split('/')[0] == 'scp':
            continue
        remember(path, node['payload']['sha256'], node)
    for family in families['families']:
        for form in family['forms']:
            raw = (config.plan_dir / form['record_member']).read_bytes()
            for carrier in _record_carriers(2, raw):
                if carrier.path.split('/')[0] in ('sprs', 'txrs') and carrier.path not in contents:
                    blob = _portrait_sources(delivery, family, carrier.path)
                    if blob is not None:
                        remember(carrier.path, sha256(blob), blob)
    def safe_sprite_carriers(raw: bytes, path: str) -> list:
        try:
            return sprite_carriers(raw, path)
        except (ValueError, UnicodeError, struct.error, IndexError):
            return []

    # Sprite frames of generated portraits reference generated textures.
    for path in list(contents):
        if path.startswith('sprs/g546v2u'):
            for digest in list(contents[path]):
                for carrier in safe_sprite_carriers(read(path, digest), path):
                    if carrier.path not in contents:
                        for family in families['families']:
                            blob = _portrait_sources(delivery, family, carrier.path)
                            if blob is not None:
                                remember(carrier.path, sha256(blob), blob)
                                break

    # Pass 2: disposition per (path, content) against the baseline and sibling contents.
    dispositions: dict[tuple[str, str], dict] = {}
    unsupported: dict[str, dict] = {}
    for path, variants in contents.items():
        root_name = path.split('/')[0]
        if PurePosixPath(path).suffix.lower() != NATIVE_SUFFIX.get(root_name):
            for digest in variants:
                dispositions[(path, digest)] = {'status': UNSUPPORTED, 'target': None}
            unsupported[path] = {'digests': sorted(variants), 'families': []}
            continue
        baseline_raw = baseline.read(path) if baseline.exists(path) else None
        baseline_digest = sha256(baseline_raw) if baseline_raw is not None else None
        for digest in variants:
            if baseline_digest == digest:
                dispositions[(path, digest)] = {'status': SKIP, 'target': path}
            elif baseline_digest is None and len(variants) == 1:
                dispositions[(path, digest)] = {'status': PLAIN, 'target': path}
            else:
                dispositions[(path, digest)] = {'status': ALIAS, 'target': _alias(config.owner, path, digest)}

    # Pass 3: typed carriers for every shipped/kept resource content; parents of aliased
    # resources are cloned under an alias because their bytes change.
    carriers: dict[tuple[str, str], list] = {}
    obs_proofs: dict[tuple[str, str], dict] = {}
    parse_errors: dict[str, str] = {}
    import tools.gunion.native_alias_carriers as carrier_module
    opaque_literals: dict[str, list] = {}
    for path, variants in contents.items():
        suffix = PurePosixPath(path).suffix
        for digest in variants:
            if suffix not in CACHED_SUFFIXES:
                carriers[(path, digest)] = []
                continue
            raw = read(path, digest)
            carrier_module.OPAQUE_LITERALS = []
            try:
                if suffix == '.mod':
                    current = model_materials(raw)
                    offset = find_kudiz_offset(raw)
                    if offset is None:
                        raise ValueError('OBS section not located')
                    extra, proof = obs_carriers(raw, offset)
                    if proof['opaque']:
                        parse_errors[path + '#' + digest[:8]] = 'opaque OBS carriers: %d' % len(proof['opaque'])
                    current = current + extra
                    obs_proofs[(path, digest)] = {'offset': offset, 'proof': proof}
                elif suffix == '.zfx':
                    current = effect_carriers(raw, path)
                elif suffix == '.spr':
                    current = sprite_carriers(raw, path)
                else:
                    current = []
            except (ValueError, AssertionError, UnicodeError, struct.error, IndexError) as error:
                parse_errors[path + '#' + digest[:8]] = str(error)
                current = []
            if carrier_module.OPAQUE_LITERALS:
                opaque_literals[path + '#' + digest[:8]] = [{'offset': offset, 'role': role, 'literal': literal[:80]}
                                                           for offset, role, literal in carrier_module.OPAQUE_LITERALS]
            carrier_module.OPAQUE_LITERALS = None
            carriers[(path, digest)] = current

    def resolve(reference: str, family_contents: dict[str, str]) -> tuple[str, str] | None:
        digest = family_contents.get(reference)
        return (reference, digest) if digest else None

    # Per family: which content of each path this family uses (graph payload or portrait source).
    results, shipped, open_nodes, family_receipts = {}, {}, [], []
    total_bytes = 0
    for family in families['families']:
        graph = family_graphs[family['family_key']]
        family_contents: dict[str, str] = {}
        for key in graph['nodeKeys']:
            node = nodes[key]
            if node['kind'] != 'resource' or normalize_path(node['logical_path']).startswith('scp/'):
                continue
            family_contents[normalize_path(node['logical_path'])] = node['payload']['sha256']
        records = {}
        for form in family['forms']:
            records[(2, form['id'])] = (config.plan_dir / form['record_member']).read_bytes()
        for weapon in family['weapons']:
            records[(10, weapon['id'])] = (config.plan_dir / weapon['record_member']).read_bytes()
        for identity in family['owned_skill_ids']:
            records[(13, identity)] = plan_records[(13, identity)]
        for identity in family['owned_funnel_ids']:
            records[(11, identity)] = plan_records[(11, identity)]
        opaque_fields: dict[str, list] = {}
        record_carriers = {}
        for key, raw in records.items():
            found: list = []
            record_carriers[key] = _record_carriers(key[0], raw, found)
            if found:
                opaque_fields['%d:%d' % key] = found
        for (kind, identity), current in record_carriers.items():
            for carrier in current:
                if carrier.path not in family_contents and carrier.path in contents:
                    family_contents[carrier.path] = sorted(contents[carrier.path])[0]
        for path in list(family_contents):
            if path.startswith('sprs/g546v2u'):
                for carrier in safe_sprite_carriers(read(path, family_contents[path]), path):
                    if carrier.path not in family_contents and carrier.path in contents:
                        family_contents[carrier.path] = sorted(contents[carrier.path])[0]
        # Alias closure inside this family: any resource whose carriers name an aliased path is aliased.
        aliases: dict[str, str] = {}
        for path, digest in family_contents.items():
            disposition = dispositions[(path, digest)]
            if disposition['status'] == ALIAS:
                aliases[path] = disposition['target']
        while True:
            added = False
            for path, digest in family_contents.items():
                if path in aliases:
                    continue
                if any(carrier.path in aliases for carrier in carriers[(path, digest)]):
                    aliases[path] = _alias(config.owner, path, digest)
                    added = True
            if not added:
                break
        family_shipped = []
        for path, digest in family_contents.items():
            disposition = dispositions[(path, digest)]
            target = aliases.get(path, path)
            if disposition['status'] == UNSUPPORTED:
                unsupported[path]['families'].append(family['family_key'])
                continue
            if disposition['status'] == SKIP and path not in aliases:
                continue
            if target in shipped:
                shipped[target]['families'].append(family['family_key'])
                family_shipped.append(target)
                continue
            raw = read(path, digest)
            prepared, spans = rewrite_prefixes(raw, carriers[(path, digest)], aliases)
            if (path, digest) in obs_proofs and spans:
                offset = obs_proofs[(path, digest)]['offset']
                carrier_module.OPAQUE_LITERALS = []
                try:
                    _, after = obs_carriers(prepared, offset)
                finally:
                    carrier_module.OPAQUE_LITERALS = None
                before = obs_proofs[(path, digest)]['proof']
                if before != after:
                    raise ValueError('OBS structure changed by alias rewrite: ' + path)
            uncovered = []
            for edge in nodes.get('resource:' + path, {}).get('edges', []):
                if not edge.get('required') or not edge['target'].startswith('resource:'):
                    continue
                dependency = normalize_path(edge['target'][9:])
                if dependency in aliases and all(carrier.path != dependency for carrier in carriers[(path, digest)]):
                    uncovered.append({'target': dependency, 'role': edge.get('role')})
            if uncovered or (path + '#' + digest[:8]) in parse_errors:
                open_nodes.append({'family': family['family_key'], 'path': path, 'target': target,
                                   'uncovered': uncovered, 'parser': parse_errors.get(path + '#' + digest[:8])})
            if target.split('/')[0] not in RUNTIME_ROOTS:
                raise ValueError('runtime resource outside the native resource roots: ' + target)
            prepared_digest = sha256(prepared)
            previous = shipped.get(target)
            if previous is not None and previous['sha256'] != prepared_digest:
                raise ValueError('two families ship different bytes at ' + target)
            if previous is None:
                shipped[target] = {'source_path': path, 'source_sha256': digest, 'sha256': prepared_digest, 'bytes': len(prepared),
                                   'status': disposition['status'] if path not in aliases else ALIAS, 'rewrites': spans,
                                   'families': []}
                blob = config.output / 'resources' / (prepared_digest + '.bin')
                blob.parent.mkdir(parents=True, exist_ok=True)
                if not blob.exists():
                    blob.write_bytes(prepared)
                total_bytes += len(prepared)
            shipped[target]['families'].append(family['family_key'])
            family_shipped.append(target)
        prepared_records = {}
        for (kind, identity), raw in records.items():
            prepared, spans = rewrite_prefixes(raw, record_carriers[(kind, identity)], aliases)
            if kind in (11, 13):
                # Skills and funnels are shared across families; every family must prepare identical bytes.
                member = config.output / 'records' / 'shared' / ('%d-%d.bin' % (kind, identity))
                member.parent.mkdir(parents=True, exist_ok=True)
                if member.exists() and member.read_bytes() != prepared:
                    raise ValueError('families prepare different bytes for shared record %d:%d' % (kind, identity))
            else:
                member = config.output / 'records' / family['family_key'] / ('%d-%d.bin' % (kind, identity))
                member.parent.mkdir(parents=True, exist_ok=True)
            member.write_bytes(prepared)
            for carrier in record_carriers[(kind, identity)]:
                target = aliases.get(carrier.path, carrier.path)
                if target not in shipped and not baseline.exists(target):
                    open_nodes.append({'family': family['family_key'], 'path': carrier.path, 'target': target,
                                       'uncovered': [], 'parser': None, 'record': '%d:%d' % (kind, identity),
                                       'reason': 'record carrier resolves to no shipped or baseline resource'})
            prepared_records['%d:%d' % (kind, identity)] = {'member': str(member.relative_to(config.output)).replace('\\', '/'),
                                                             'sha256': sha256(prepared), 'rewrites': spans}
        family_receipts.append({'family_key': family['family_key'], 'aliases': aliases, 'shipped': sorted(family_shipped),
                                'records': prepared_records, 'opaque_record_fields': opaque_fields})
        results[family['family_key']] = len(family_shipped)
    counts = {'families': len(family_receipts), 'resources_shipped': len(shipped), 'shipped_bytes': total_bytes,
              'aliased': sum(row['status'] == ALIAS for row in shipped.values()),
              'plain': sum(row['status'] == PLAIN for row in shipped.values()),
              'open_nodes': len(open_nodes), 'parser_errors': len(parse_errors), 'opaque_literal_resources': len(opaque_literals),
              'unsupported_native_types': len(unsupported),
              'by_root': {root: sum(1 for target in shipped if target.startswith(root + '/')) for root in RUNTIME_ROOTS},
              'bytes_by_root': {root: sum(row['bytes'] for target, row in shipped.items() if target.startswith(root + '/')) for root in RUNTIME_ROOTS}}
    receipt = {'schema': SCHEMA, 'owner': config.owner, 'families_sha256': sha256((config.output / 'families.json').read_bytes()),
               'counts': counts, 'resources': shipped, 'families': family_receipts, 'open_nodes': open_nodes,
               'parser_errors': parse_errors, 'opaque_literals': opaque_literals, 'unsupported': unsupported,
               'native_acceptance_claimed': False}
    dump_json(config.output / 'resources.json', receipt)
    return receipt
