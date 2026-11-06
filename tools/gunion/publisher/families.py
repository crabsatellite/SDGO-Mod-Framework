"""Stage 1: derive the publishable family set from the relocation plan.

Every target inventory context of the snapshot-bound plan becomes one family
whose forms, weapons, skills and funnels are resolved to final record IDs.
Contexts with unfinished source models are excluded. Contexts with more than
two forms are split: the reciprocal pair (or the one-way terminal form) stays
publishable with the ordinary native profiles, and each extra entry variant is
recorded as deferred until the native form-graph runtime admits it. Nothing
here reserves IDs or claims native acceptance.
"""
from __future__ import annotations

from pathlib import Path
import re
import struct

from .common import (Config, FUNNEL_BYTES, NAME_OFFSETS, SKILL_BYTES, UNIT_BYTES, UNIT_SKILL_OFFSETS,
                     UNIT_TRANSFORM_OFFSET, UNIT_WEAPON_OFFSET, WEAPON_BYTES, WEAPON_FUNNEL_OFFSET,
                     dump_json, load_json, sha256, u32, wide_field)

SCHEMA = 'sdgo.gunion-roster-families/1'
KIND_BYTES = {2: UNIT_BYTES, 10: WEAPON_BYTES, 11: FUNNEL_BYTES, 13: SKILL_BYTES}
SINGLE, LINKED, ONE_WAY = 'native-single-form-v1', 'native-linked-forms-v1', 'native-one-way-forms-v1'
_CJK = re.compile('[぀-ヿ㐀-鿿]')


def load_plan(config: Config):
    plan_raw = (config.plan_dir / 'plan.json').read_bytes()
    plan = load_json(config.plan_dir / 'plan.json')
    definitions_raw = (config.plan_dir / 'proposed-server-definitions.json').read_bytes()
    definitions = load_json(config.plan_dir / 'proposed-server-definitions.json')
    if plan.get('schema') != 'sdgo:record-relocation-plan/2':
        raise ValueError('unsupported relocation plan schema')
    records = {}
    requests = {}
    for row in plan['requests']:
        kind, target = int(row['kind']), int(row['requestedTargetId'])
        raw = (config.plan_dir / row['generatedMember']).read_bytes()
        if len(raw) != KIND_BYTES[kind] or len(raw) != row['bytes'] or sha256(raw) != row['requestedRecordSha256']:
            raise ValueError('relocated record differs from the plan: ' + row['key'])
        if u32(raw, 0) != target:
            raise ValueError('relocated record identity differs: ' + row['key'])
        if (kind, target) in records:
            raise ValueError('relocated record target repeats: %d:%d' % (kind, target))
        records[(kind, target)] = raw
        requests[(kind, target)] = row
    values = {}
    for section, kind, key in (('units', 2, 'UnitNumber'), ('weapons', 10, 'WeaponNo'),
                               ('skills', 13, 'SkillID'), ('funnels', 11, 'FunnelID')):
        for row in definitions[section]:
            target = int(row['requestedTargetId'])
            if row['values'][key] != target or (kind, target) not in records:
                raise ValueError('server definition does not match a relocated record: %s %d' % (section, target))
            if sha256(records[(kind, target)]) != row['clientRecord']['sha256']:
                raise ValueError('server definition client record differs: %s %d' % (section, target))
            values[(kind, target)] = row
    missing = [key for key in records if key not in values]
    if missing:
        raise ValueError('relocated records without server definitions: ' + str(sorted(missing)[:8]))
    return {'plan': plan, 'plan_sha256': sha256(plan_raw), 'definitions': definitions,
            'definitions_sha256': sha256(definitions_raw), 'records': records, 'requests': requests, 'values': values}


def _names(raw: bytes) -> dict:
    texts = [wide_field(raw, offset, 100) for offset in NAME_OFFSETS]
    chinese = next((text for text in texts if text and _CJK.search(text)), '')
    english = next((text for text in texts if text and not _CJK.search(text)), '')
    return {'fields': texts, 'NameCN': chinese or english or texts[0], 'NameEN': english or chinese or texts[0]}


def _check_record_values(kind: int, raw: bytes, row: dict) -> None:
    values = row['values']
    if kind == 2:
        if u32(raw, UNIT_TRANSFORM_OFFSET) != values['UnitNumber_Transform']:
            raise ValueError('unit transform differs between record and definition: %d' % values['UnitNumber'])
        skills = struct.unpack_from('<II', raw, UNIT_SKILL_OFFSETS[0])
        if skills != (values['SkillNumber_1'], values['SkillNumber_2']):
            raise ValueError('unit skills differ between record and definition: %d' % values['UnitNumber'])
        weapons = struct.unpack_from('<4I', raw, UNIT_WEAPON_OFFSET)
        expected = tuple(values[name] for name in ('WeaponNumber_1', 'WeaponNumber_2', 'WeaponNumber_3', 'WeaponNumber_FatalAttack'))
        if weapons != expected:
            raise ValueError('unit weapons differ between record and definition: %d' % values['UnitNumber'])
    elif kind == 10:
        if u32(raw, WEAPON_FUNNEL_OFFSET) != values['FunnelID']:
            raise ValueError('weapon funnel differs between record and definition: %d' % values['WeaponNo'])


def _classify(members: list[int], root: int, transforms: dict[int, int]) -> dict:
    """Return the publishable core and deferred entry variants of one context."""
    member_set = set(members)
    for identity in members:
        target = transforms[identity]
        if target and target not in member_set:
            raise ValueError('transform target outside its inventory context: %d -> %d' % (identity, target))
    if len(members) == 1:
        if transforms[root] == root:
            return {'profile': None, 'root': root, 'forms': [], 'entries': [root],
                    'reason': 'self-transform-loop-requires-native-profile'}
        if transforms[root]:
            raise ValueError('single form with a transform target: %d' % root)
        return {'profile': SINGLE, 'root': root, 'forms': [root], 'entries': []}
    cycle = [identity for identity in members if transforms[identity] and transforms[transforms[identity]] == identity]
    sinks = [identity for identity in members if not transforms[identity]]
    if len(members) == 2:
        if len(cycle) == 2:
            return {'profile': LINKED, 'root': root, 'forms': [root, transforms[root]], 'entries': []}
        if len(sinks) == 1 and transforms[root] == sinks[0]:
            return {'profile': ONE_WAY, 'root': root, 'forms': [root, sinks[0]], 'entries': []}
        raise ValueError('two-form context without a reciprocal pair or one-way terminal: %s' % members)
    if len(cycle) == 2:
        core_root = root if root in cycle else transforms[root]
        if core_root not in cycle:
            raise ValueError('context root does not enter the reciprocal pair: %s' % members)
        entries = [identity for identity in members if identity not in cycle]
        for entry in entries:
            if transforms[entry] not in cycle:
                raise ValueError('entry variant does not enter the reciprocal pair: %d' % entry)
        return {'profile': LINKED, 'root': core_root, 'forms': [core_root, transforms[core_root]], 'entries': entries}
    if len(sinks) == 1 and not cycle:
        sink = sinks[0]
        if root == sink:
            raise ValueError('context root is the terminal form: %s' % members)
        if transforms[root] != sink:
            raise ValueError('context root does not reach the terminal form: %s' % members)
        entries = [identity for identity in members if identity not in (root, sink)]
        for entry in entries:
            if transforms[entry] != sink:
                raise ValueError('entry variant does not reach the terminal form: %d' % entry)
        return {'profile': ONE_WAY, 'root': root, 'forms': [root, sink], 'entries': entries}
    raise ValueError('unsupported form topology: %s' % members)


def build(config: Config) -> dict:
    data = load_plan(config)
    records, values = data['records'], data['values']
    units = {target: row for (kind, target), row in values.items() if kind == 2}
    weapons = {target: row for (kind, target), row in values.items() if kind == 10}
    skills = {target: row for (kind, target), row in values.items() if kind == 13}
    funnels = {target: row for (kind, target), row in values.items() if kind == 11}
    for (kind, target), raw in records.items():
        _check_record_values(kind, raw, values[(kind, target)])
    baseline_skills = {int(row['skill_id']) if isinstance(row, dict) else int(row)
                       for row in data['definitions'].get('requiredBaselineSkills', [])}
    families, deferred, excluded, used_units = [], [], [], set()
    for context in data['definitions']['target_inventory_contexts']:
        members = [int(identity) for identity in context['member_unit_ids']]
        root = int(context['root_unit_id'])
        if any(identity not in units for identity in members) or root not in members:
            raise ValueError('inventory context references unknown units: ' + context['context_id'])
        source_ids = {identity: int(units[identity]['sourceId']) for identity in members}
        if any(source in config.excluded_units for source in source_ids.values()):
            excluded.append({'context_id': context['context_id'], 'members': members,
                             'source_members': [source_ids[identity] for identity in members],
                             'reason': 'excluded-source-unit'})
            used_units.update(members)
            continue
        transforms = {identity: int(units[identity]['values']['UnitNumber_Transform']) for identity in members}
        shape = _classify(members, root, transforms)
        if shape['profile'] is None:
            for entry in shape['entries']:
                deferred.append({'id': entry, 'source_id': source_ids[entry], 'transform': transforms[entry],
                                 'family_key': None, 'core_forms': [], 'names': _names(records[(2, entry)]),
                                 'record_member': data['requests'][(2, entry)]['generatedMember'],
                                 'reason': shape['reason']})
                used_units.add(entry)
            continue
        family_root = shape['root']
        forms = []
        for identity in shape['forms']:
            row = dict(units[identity]['values'])
            row['SallyReady'] = 1 if identity == family_root else 0
            raw = records[(2, identity)]
            forms.append({'id': identity, 'source_id': source_ids[identity], 'transform': transforms[identity],
                          'record_sha256': sha256(raw), 'record_member': data['requests'][(2, identity)]['generatedMember'],
                          'names': _names(raw), 'values': row})
        weapon_ids = sorted({row['values'][name] for row in forms for name in
                             ('WeaponNumber_1', 'WeaponNumber_2', 'WeaponNumber_3', 'WeaponNumber_FatalAttack')
                             if row['values'][name]})
        family_weapons, baseline_weapons = [], []
        for identity in weapon_ids:
            if identity in weapons:
                family_weapons.append({'id': identity, 'source_id': int(weapons[identity]['sourceId']),
                                       'record_member': data['requests'][(10, identity)]['generatedMember'],
                                       'record_sha256': sha256(records[(10, identity)]), 'values': dict(weapons[identity]['values'])})
            else:
                baseline_weapons.append(identity)
        skill_ids = sorted({row['values'][name] for row in forms for name in ('SkillNumber_1', 'SkillNumber_2', 'SkillNumber_3')
                            if row['values'][name]})
        owned_skills = [identity for identity in skill_ids if identity in skills]
        baseline_skill_refs = [identity for identity in skill_ids if identity not in skills]
        unknown_skills = [identity for identity in baseline_skill_refs if identity not in baseline_skills]
        funnel_ids = sorted({row['values']['FunnelID'] for row in family_weapons if row['values']['FunnelID']})
        owned_funnels = [identity for identity in funnel_ids if identity in funnels]
        baseline_funnels = [identity for identity in funnel_ids if identity not in funnels]
        grade = int(units[family_root]['values']['UnitGradeNumber'])
        if grade not in config.item_templates:
            raise ValueError('no item template for grade %d (family %s)' % (grade, context['context_id']))
        family = {'family_key': 'unit-import.%s.family%d' % (config.owner, source_ids[family_root]),
                  'context_id': context['context_id'], 'source_root_unit_id': int(context['source_root_unit_id']),
                  'root_unit_id': family_root, 'behavior_profile': shape['profile'], 'grade': grade,
                  'item_template_id': config.item_templates[grade], 'forms': forms, 'weapons': family_weapons,
                  'baseline_weapon_ids': baseline_weapons, 'owned_skill_ids': owned_skills,
                  'baseline_skill_ids': baseline_skill_refs, 'unknown_skill_ids': unknown_skills,
                  'owned_funnel_ids': owned_funnels, 'baseline_funnel_ids': baseline_funnels,
                  'deferred_entry_ids': shape['entries']}
        families.append(family)
        used_units.update(shape['forms'])
        for entry in shape['entries']:
            raw = records[(2, entry)]
            deferred.append({'id': entry, 'source_id': source_ids[entry], 'transform': transforms[entry],
                             'family_key': family['family_key'], 'core_forms': shape['forms'],
                             'names': _names(raw), 'record_member': data['requests'][(2, entry)]['generatedMember'],
                             'reason': 'entry-variant-requires-native-form-graph-runtime'})
            used_units.add(entry)
    unused = sorted(set(units) - used_units)
    if unused:
        raise ValueError('relocated units outside every inventory context: %s' % unused[:8])
    referenced_weapons = {row['id'] for family in families for row in family['weapons']}
    referenced_skills = {identity for family in families for identity in family['owned_skill_ids']}
    referenced_funnels = {identity for family in families for identity in family['owned_funnel_ids']}
    result = {'schema': SCHEMA, 'owner': config.owner, 'plan_sha256': data['plan_sha256'],
              'definitions_sha256': data['definitions_sha256'],
              'counts': {'contexts': len(data['definitions']['target_inventory_contexts']), 'families': len(families),
                         'forms': sum(len(family['forms']) for family in families),
                         'profiles': {profile: sum(family['behavior_profile'] == profile for family in families)
                                      for profile in (SINGLE, LINKED, ONE_WAY)},
                         'deferred_entries': len(deferred), 'excluded_contexts': len(excluded),
                         'plan_units': len(units), 'plan_weapons': len(weapons), 'plan_skills': len(skills), 'plan_funnels': len(funnels),
                         'referenced_weapons': len(referenced_weapons), 'referenced_skills': len(referenced_skills),
                         'referenced_funnels': len(referenced_funnels),
                         'families_with_unknown_skills': sum(bool(family['unknown_skill_ids']) for family in families)},
              'unreferenced_plan_weapons': sorted(set(weapons) - referenced_weapons),
              'unreferenced_plan_skills': sorted(set(skills) - referenced_skills),
              'unreferenced_plan_funnels': sorted(set(funnels) - referenced_funnels),
              'families': families, 'deferred_entries': deferred, 'excluded_contexts': excluded,
              'ids_reserved': False, 'native_acceptance_claimed': False}
    dump_json(config.output / 'families.json', result)
    return result
