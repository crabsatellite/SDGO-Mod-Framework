"""Stage 4: assemble one native unit-catalog publication for the roster.

Produces the complete `unit_catalog` module directory (catalog records for
units, weapons, skills, funnels, affiliations, item categories and
encyclopedia indexes; the native program bundle; the skill atlas sidecar;
the resource overlay archive) together with the owner-side server plan,
registrations and an installer manifest. Existing private catalog records can
be carried forward so encyclopedia numbering stays contiguous. Nothing here
installs, writes SQL or claims native acceptance.
"""
from __future__ import annotations

import io
import json
from pathlib import Path
import re
import struct
from types import SimpleNamespace

from owner.native_archive import build_archive_file
from owner.units.assets import Baseline, sprite_binary
from owner.units.compiler import HOST, HOST_SHA256, fnv, wchar
from owner.units.native_programs import compile_bundle as compile_programs
from owner.units.skill_atlases import compile_bundle as compile_atlases
from tools.gunion.native_alias_carriers import sprite_carriers

from .common import (AFFILIATION_BYTES, Config, FUNNEL_BYTES, ITEM_BYTES, SKILL_BYTES, UNIT_BYTES, WEAPON_BYTES,
                     dump_json, load_json, sha256, u32, wide_field)

SCHEMA = 'sdgo.gunion-roster-publication/1'
KIND_BYTES = {2: UNIT_BYTES, 10: WEAPON_BYTES, 11: FUNNEL_BYTES, 13: SKILL_BYTES, 20: AFFILIATION_BYTES, 33: 1197, 34: ITEM_BYTES, 85: ITEM_BYTES}
INDEX_FIRST = 0x375BE1
OVERLAY_ROOTS = {'mdrs': '.mod', 'anrs': '.ani', 'txrs': '.txr', 'fxrs': '.zfx', 'sprs': '.spr', 'sdrs': '.zsd'}


def _parse_catalog(raw: bytes) -> list[tuple[int, int, bytes]]:
    if raw[:8] != b'SDGOUN1\0':
        raise ValueError('existing catalog magic differs')
    version, header, count, extent, checksum, reserved = struct.unpack_from('<6I', raw, 8)
    if version != 1 or header != 32 or reserved or len(raw) != 32 + extent or fnv(raw[32:]) != checksum:
        raise ValueError('existing catalog header differs')
    cursor, result = 32, []
    for _ in range(count):
        kind, identity, size, digest, flags, reserved = struct.unpack_from('<6I', raw, cursor)
        cursor += 24
        record = raw[cursor:cursor + size]
        cursor += size
        if flags or reserved or fnv(record) != digest or u32(record, 0) != identity:
            raise ValueError('existing catalog record differs: %d' % identity)
        result.append((kind, identity, record))
    if cursor != len(raw):
        raise ValueError('existing catalog extent differs')
    return result


def _catalog(records: list[tuple[int, int, bytes]]) -> bytes:
    seen = set()
    parts = []
    for kind, identity, raw in sorted(records, key=lambda row: row[1]):
        if kind not in KIND_BYTES or len(raw) != KIND_BYTES[kind]:
            raise ValueError('unsupported native record kind/length %d:%d' % (kind, len(raw)))
        if identity <= 0 or identity in seen or u32(raw, 0) != identity or raw[4] != 0:
            raise ValueError('native record identity/global key conflict: %d' % identity)
        seen.add(identity)
        parts.append(struct.pack('<6I', kind, identity, len(raw), fnv(raw), 0, 0) + raw)
    payload = b''.join(parts)
    if len(parts) > 4096 or len(payload) > 64 * 1024 * 1024:
        raise ValueError('catalog exceeds the native adapter capacity')
    return struct.pack('<8s6I', b'SDGOUN1\0', 1, 32, len(parts), len(payload), fnv(payload), 0) + payload


def _index_icons(png: bytes) -> dict[str, bytes]:
    from owner.units.presentation import _index_images
    return _index_images({'unit_primary': png})


def build(config: Config) -> dict:
    families = load_json(config.output / 'families.json')
    resources = load_json(config.output / 'resources.json')
    programs = load_json(config.output / 'programs.json')
    declaration = load_json(config.output / 'programs' / 'declaration.json')
    baseline = Baseline(config.baseline_client)
    baseline._loose_roots = []
    module_dir = config.output / 'publication' / 'unit_catalog'
    module_dir.mkdir(parents=True, exist_ok=True)

    # Shipped overlay resources as loaders over the content-addressed blobs.
    overlay: dict[str, object] = {}
    for target, row in resources['resources'].items():
        blob = config.output / 'resources' / (row['sha256'] + '.bin')
        overlay[target] = (lambda path=blob: path.read_bytes())
    generated: dict[str, bytes] = {}

    def resource_bytes(path: str) -> bytes:
        path = path.replace('\\', '/').lower()
        if path in generated:
            return generated[path]
        if path in overlay:
            return overlay[path]()
        return baseline.read(path)

    records: list[tuple[int, int, bytes]] = []
    carried = []
    if config.existing_catalog is not None:
        carried = _parse_catalog(Path(config.existing_catalog).read_bytes())
        records.extend(carried)
    occupied = {identity for _, identity, _ in records}

    def allocate(first: int, last: int) -> int:
        candidate = first
        while candidate <= last:
            if candidate not in occupied and not baseline.any_id(candidate):
                occupied.add(candidate)
                return candidate
            candidate += 1
        raise ValueError('no free identity in %d..%d' % (first, last))

    index_rows = baseline.unit_index()
    next_index = INDEX_FIRST + len(index_rows) + sum(1 for kind, _, _ in carried if kind == 85)
    plan_requests = {(row['kind'], row['requestedTargetId']): row for row in load_json(config.plan_dir / 'plan.json')['requests']}
    weapons_final: dict[int, bytes] = {}
    skill_records: dict[int, bytes] = {}
    registrations = []
    published = []
    server_plans = []
    from manager.engines.capsules.capsule_assets import Assets
    renderer = object.__new__(Assets)
    renderer.reader = SimpleNamespace(read_resource=resource_bytes)
    for family in families['families']:
        receipt = next(row for row in resources['families'] if row['family_key'] == family['family_key'])
        root = family['root_unit_id']
        forms = []
        for form in family['forms']:
            raw = (config.output / receipt['records']['2:%d' % form['id']]['member']).read_bytes()
            if u32(raw, 0) != form['id'] or len(raw) != UNIT_BYTES:
                raise ValueError('prepared unit record differs: %d' % form['id'])
            records.append((2, form['id'], raw))
            occupied.add(form['id'])
            forms.append({'id': form['id'], 'transform_target': form['transform'], 'model': wide_field(raw, 612, 60),
                          'server': form['values'], 'names': form['names'], 'record_sha256': sha256(raw)})
        weapons = []
        for weapon in family['weapons']:
            raw = (config.output / receipt['records']['10:%d' % weapon['id']]['member']).read_bytes()
            if u32(raw, 0) != weapon['id'] or len(raw) != WEAPON_BYTES:
                raise ValueError('prepared weapon record differs: %d' % weapon['id'])
            if weapon['id'] in weapons_final:
                if weapons_final[weapon['id']] != raw:
                    raise ValueError('families prepare different bytes for weapon %d' % weapon['id'])
            else:
                weapons_final[weapon['id']] = raw
                records.append((10, weapon['id'], raw))
                occupied.add(weapon['id'])
            weapons.append({'id': weapon['id'], 'server': weapon['values'], 'record_sha256': sha256(raw)})
        for identity in family['owned_skill_ids']:
            if identity not in skill_records:
                raw = (config.output / 'records' / 'shared' / ('13-%d.bin' % identity)).read_bytes()
                skill_records[identity] = raw
        for identity in family['owned_funnel_ids']:
            if identity not in occupied:
                raw = (config.output / 'records' / 'shared' / ('11-%d.bin' % identity)).read_bytes()
                records.append((11, identity, raw))
                occupied.add(identity)
        # Item category from the grade template and the root's localized name.
        template = baseline.record('item', family['item_template_id'])
        if not template or len(template) != ITEM_BYTES or template[4] != 0:
            raise ValueError('baseline item template missing: %d' % family['item_template_id'])
        category = allocate(config.item_category_first, 1_119_999)
        root_form = next(form for form in forms if form['id'] == root)
        name_cn = root_form['names']['NameCN'][:48]
        item = bytearray(template)
        struct.pack_into('<I', item, 0, category)
        wchar(item, 5, 50, name_cn)
        struct.pack_into('<I', item, 105, root)
        records.append((34, category, bytes(item)))
        # Encyclopedia index record with generated owned/missing icons from the shop portrait.
        root_raw = next(raw for kind, identity, raw in records if kind == 2 and identity == root)
        portrait = wide_field(root_raw, 4654, 100)
        image = renderer.sprite(portrait)
        stream = io.BytesIO()
        image.save(stream, 'PNG')
        icons = _index_icons(stream.getvalue())
        display = {}
        for role, letter in (('index_owned', 'o'), ('index_missing', 'g')):
            leaf = 'g546q%d%s' % (root, letter)
            texture, sprite = 'txrs/%s.txr' % leaf, 'sprs/%s.spr' % leaf
            png = icons[role]
            generated[texture] = png
            generated[sprite] = sprite_binary(texture.replace('/', '\\'), 122, 122)
            overlay[texture] = png
            overlay[sprite] = generated[sprite]
            display[role] = sprite
        if baseline.any_id(next_index) or next_index in occupied:
            raise ValueError('encyclopedia index %d is occupied' % next_index)
        index_id = next_index
        next_index += 1
        occupied.add(index_id)
        index = bytearray(ITEM_BYTES)
        struct.pack_into('<I', index, 0, index_id)
        index[5] = 1
        struct.pack_into('<I', index, 6, root)
        wchar(index, 14, 50, display['index_owned'].replace('/', '\\'))
        wchar(index, 114, 50, display['index_missing'].replace('/', '\\'))
        records.append((85, index_id, bytes(index)))
        family_entry = {'family_key': family['family_key'], 'name': name_cn, 'name_en': root_form['names']['NameEN'],
                        'root_unit_id': root, 'category_id': category, 'item_template_id': family['item_template_id'],
                        'behavior_profile': family['behavior_profile'], 'grade': family['grade'], 'unit_index_id': index_id,
                        'forms': forms, 'weapons': weapons, 'owned_skill_ids': family['owned_skill_ids'],
                        'baseline_skill_ids': family['baseline_skill_ids'], 'owned_funnel_ids': family['owned_funnel_ids'],
                        'baseline_funnel_ids': family['baseline_funnel_ids'], 'source_root_unit_id': family['source_root_unit_id'],
                        'deferred_entry_ids': family['deferred_entry_ids'], 'visibility': 'private'}
        published.append(family_entry)
        registrations.append({'key': 'unit:%d' % root, 'provider': 'unit_catalog', 'kind': 'unit', 'unit_id': root,
                              'category_id': category, 'family_key': family['family_key'], 'unit_index_id': index_id,
                              'index_owned_sprite': display['index_owned'], 'index_missing_sprite': display['index_missing'],
                              'display_name': name_cn, 'forms': [form['id'] for form in forms],
                              'shop_sprite': display['index_owned'], 'secret_store_sprite': wide_field(root_raw, 4754, 100).replace('\\', '/').lower(),
                              'capsule_sprite': wide_field(root_raw, 4854, 100).replace('\\', '/').lower(),
                              'unit_primary_sprite': portrait.replace('\\', '/').lower(),
                              'custom_candidate_sprite': wide_field(root_raw, 4854, 100).replace('\\', '/').lower(),
                              'enabled': True, 'storefront_changes': False, 'gacha_changes': False, 'blueprints': [],
                              'primary_blueprint_id': 0})
        item_row = {'CatalogueID': category, 'RefIndex': root, 'Name': root_form['names']['NameEN'][:48]}
        server_plans.append({'family_key': family['family_key'], 'root_unit_id': root, 'item_template_id': family['item_template_id'],
                             'units': [form['server'] for form in forms], 'weapons': [weapon['server'] for weapon in weapons],
                             'item': item_row,
                             'names': [{'UnitNumber': form['id'], 'NameCN': form['names']['NameCN'][:48], 'NameEN': form['names']['NameEN'][:48]}
                                       for form in forms]})
    # Every relocated skill record ships (linked-skill references may point at unreferenced ones).
    for (kind, identity), row in plan_requests.items():
        if kind == 13 and identity not in skill_records:
            skill_records[identity] = (config.plan_dir / row['generatedMember']).read_bytes()
    for identity, raw in skill_records.items():
        if len(raw) != SKILL_BYTES or u32(raw, 0) != identity:
            raise ValueError('skill record differs: %d' % identity)
        records.append((13, identity, raw))
        occupied.add(identity)
    affiliations = []
    if config.affiliation_dictionary:
        dictionary = load_json(config.delivery / config.affiliation_dictionary)
        for row in dictionary['rows']:
            record = row['client_record']
            raw = config.read_delivery('native/' + record['path'])
            if len(raw) != AFFILIATION_BYTES or sha256(raw) != record['sha256'] or u32(raw, 0) != record['key']:
                raise ValueError('affiliation record differs: %d' % row['values']['InfluenceNumber'])
            if baseline.any_id(record['key']):
                continue
            records.append((20, record['key'], raw))
            occupied.add(record['key'])
            affiliations.append({'InfluenceNumber': row['values']['InfluenceNumber'], 'InfluenceName': row['values']['InfluenceName'],
                                 'key': record['key'], 'authority': row.get('authority')})
    catalog = _catalog(records)
    catalog_sha = sha256(catalog)
    (module_dir / 'catalog.bin').write_bytes(catalog)

    # Native program bundle bound to this catalog.
    program_files = {program['member']: (config.output / 'programs' / program['member']).read_bytes() for program in declaration['programs']}
    bundle = compile_programs([declaration], catalog_sha, program_files, weapons_final)
    (module_dir / 'native-programs.bin').write_bytes(bundle)

    # Overlay archive, streamed to disk.
    def allowed(name: str) -> bool:
        root_name = name.split('/', 1)[0]
        return root_name in OVERLAY_ROOTS and name.endswith(OVERLAY_ROOTS[root_name])
    zdx, inventory = build_archive_file(overlay, allowed, module_dir / 'resources.zpk')
    (module_dir / 'resources.zdx').write_bytes(zdx)
    overlay_ini = ('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256=' + sha256(zdx) + '\narchive=resources.zpk\narchive_sha256='
                   + inventory['archive_sha256'] + '\n').encode('ascii')
    (module_dir / 'resource-overlay.ini').write_bytes(overlay_ini)

    # Skill atlas sidecar: the recovered atlas sprite ships under its owner alias.
    atlas_targets = [target for target, row in resources['resources'].items() if row['source_path'] == config.skill_atlas_source]
    atlas_receipt = None
    if atlas_targets:
        atlas = atlas_targets[0]
        atlas_raw = overlay[atlas]()
        textures = sorted({carrier.path for carrier in sprite_carriers(atlas_raw, atlas)})
        atlas_resources = {atlas: atlas_raw}
        for texture in textures:
            atlas_resources[texture] = resource_bytes(texture)
        manifest = {'schema': 'sdgo.unit-skill-atlases/1', 'owner': config.owner, 'revision': '',
                    'sprites': [{'path': atlas, 'sha256': sha256(atlas_raw),
                                 'textures': [{'path': texture, 'sha256': sha256(atlas_resources[texture])} for texture in textures]}],
                    'skills': [{'id': identity, 'record_sha256': sha256(raw), 'sprite': atlas} for identity, raw in sorted(skill_records.items())]}
        manifest['revision'] = sha256(json.dumps({k: v for k, v in manifest.items() if k != 'revision'}, sort_keys=True).encode())
        sidecar = compile_atlases([manifest], catalog_sha, sha256(zdx), inventory['archive_sha256'], skill_records, atlas_resources)
        (module_dir / 'skill-atlases.bin').write_bytes(sidecar)
        atlas_receipt = {'sprite': atlas, 'textures': textures, 'frames': len(sprite_carriers(atlas_raw, atlas)), 'sidecar_sha256': sha256(sidecar)}

    # Definitions, registrations, server plan and installer manifest.
    content_revision = sha256(json.dumps({'catalog': catalog_sha, 'overlay': inventory['archive_sha256'], 'programs': sha256(bundle),
                                          'families': [row['family_key'] for row in published]}, sort_keys=True).encode())
    definitions = {'protocol': 'sdgo.unit-family', 'schema_version': 1, 'revision': content_revision, 'owner': config.owner,
                   'families': published, 'registrations': registrations, 'affiliations': affiliations,
                   'skills': sorted(skill_records), 'deferred_entries': families['deferred_entries'],
                   'excluded_contexts': families['excluded_contexts']}
    (module_dir / 'definitions.json').write_bytes((json.dumps(definitions, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))
    repo_root = Path(__file__).resolve().parents[3]
    module_source = repo_root / 'mods' / 'unit_catalog'
    mod_ini = (module_source / 'mod.ini').read_text(encoding='utf-8-sig')
    mod_ini = re.sub(r'(?m)^enabled\s*=.*$', 'enabled=1', mod_ini)
    files_line = 'files=symbols.fragment.ini,catalog.bin,definitions.json,resource-overlay.ini,resources.zdx,resources.zpk,native-programs.bin'
    if atlas_receipt:
        files_line += ',skill-atlases.bin'
    mod_ini = re.sub(r'(?m)^files\s*=.*$', files_line, mod_ini)
    (module_dir / 'mod.ini').write_bytes(mod_ini.encode('utf-8'))
    for name in ('manager.json', 'symbols.fragment.ini'):
        (module_dir / name).write_bytes((module_source / name).read_bytes())
    binary = Path(config.module_binary).read_bytes()
    (module_dir / 'unit_catalog.dll').write_bytes(binary)
    module_files = {path.name: sha256(path.read_bytes()) for path in module_dir.iterdir() if path.is_file()}
    package_root = config.output / 'publication'
    manifest = {'module': 'unit_catalog', 'schema_version': 1, 'revision': content_revision, 'host': HOST, 'host_sha256': HOST_SHA256,
                'files': {'client/sdgo/mods/unit_catalog/' + name: digest for name, digest in module_files.items()},
                'owned_paths': [], 'resource_overlay': {'index_sha256': sha256(zdx), 'archive_sha256': inventory['archive_sha256'],
                                                        'entries': len(inventory['entries'])}}
    manifest['files']['symbols.fragment.ini'] = module_files['symbols.fragment.ini']
    (package_root / 'symbols.fragment.ini').write_bytes((module_dir / 'symbols.fragment.ini').read_bytes())
    dump_json(package_root / 'manifest.json', manifest)
    dump_json(package_root / 'registrations.json', {'schema_version': 1, 'provider': 'unit_catalog', 'revision': content_revision, 'items': registrations})
    skill_definitions = load_json(config.delivery / 'native' / 'design-adaptation-v1' / 'skill-server-definitions.json')
    server_plan = {'schema': 'sdgo.gunion-roster-server-plan/1', 'owner': config.owner, 'revision': content_revision,
                   'families': server_plans,
                   'skills': {'schema': 'sdgo.unit-server-dependencies/1', 'owner': config.owner, 'revision': '',
                              'skills': []},
                   'funnels': {'schema': 'sdgo.unit-server-funnels/1', 'owner': config.owner, 'revision': '', 'funnels': []},
                   'affiliations': affiliations}
    plan_definitions = load_json(config.plan_dir / 'proposed-server-definitions.json')
    skill_values = {row['requestedTargetId']: row['values'] for row in plan_definitions['skills']}
    for identity, raw in sorted(skill_records.items()):
        if identity not in skill_values:
            raise ValueError('no server definition for skill %d' % identity)
        server_plan['skills']['skills'].append({'id': identity, 'client_sha256': sha256(raw),
                                                'definition': {'kind': 'explicit', 'origin': 'source-derived', 'values': skill_values[identity]}})
    funnel_values = {row['requestedTargetId']: row['values'] for row in plan_definitions['funnels']}
    for kind, identity, raw in records:
        if kind == 11:
            server_plan['funnels']['funnels'].append({'id': identity, 'client_sha256': sha256(raw),
                                                      'definition': {'kind': 'explicit', 'origin': 'source-derived', 'values': funnel_values[identity]}})
    for section in ('skills', 'funnels'):
        block = server_plan[section]
        block['revision'] = sha256(json.dumps({k: v for k, v in block.items() if k != 'revision'}, sort_keys=True).encode())
    dump_json(package_root / 'server-plan.json', server_plan)
    counts = {'families': len(published), 'records': len(records), 'carried_records': len(carried),
              'kinds': {str(kind): sum(1 for row in records if row[0] == kind) for kind in sorted({row[0] for row in records})},
              'catalog_bytes': len(catalog), 'overlay_entries': len(inventory['entries']), 'overlay_bytes': inventory['archive_bytes'],
              'program_bundle_bytes': len(bundle), 'skill_atlas': atlas_receipt, 'affiliations': len(affiliations),
              'first_index_id': INDEX_FIRST + len(index_rows) + sum(1 for kind, _, _ in carried if kind == 85), 'next_index_id': next_index,
              'item_categories': sorted(row['category_id'] for row in published)[:3] + ['...'] + sorted(row['category_id'] for row in published)[-1:]}
    receipt = {'schema': SCHEMA, 'owner': config.owner, 'revision': content_revision, 'catalog_sha256': catalog_sha, 'counts': counts,
               'module_files': module_files, 'program_declaration_sha256': programs['declaration_sha256'],
               'ids_reserved': False, 'native_acceptance_claimed': False}
    dump_json(config.output / 'publication.json', receipt)
    return receipt
