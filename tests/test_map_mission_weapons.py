"""Paired client/server weapon clones for map-mission NPC attack multipliers."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))

from owner.maps import bundle, mission as M, pipeline, server  # noqa: E402
from owner.units import repository  # noqa: E402
from test_map_import import compile_spec, mission_spec, record_templates  # noqa: E402


def attack_spec(*, factor=2.0, label='Boss'):
    spec = mission_spec()
    spec['npcs']['boss'] = {'template': 500, 'hp': 20000, 'boss': True,
                            'label': label, 'attack': factor, 'special_attack': 1.5}
    spec['phases'][0]['spawns'].append({'npc': 'boss', 'positions': [[0, 150, 900]], 'at': 1})
    spec['phases'][0]['goal'] = {'kill': [{'npc': 'boss', 'count': 1}]}
    spec['phases'][0]['fail'].append({'lose': {'npc': 'boss', 'count': 1}})
    return spec


def client_rows(build):
    return {(kind, ident): raw for kind, ident, raw in build.rows}


def plan_for(build):
    return {'schema': bundle.PLAN_SCHEMA, 'revision': 'fixture', 'maps': [], 'keys': bundle.TABLE_KEYS,
            'tables': copy.deepcopy(build.server), 'runtime_policy': {'immediate_transition_missions': []},
            'credentials_included': False}


class PairedWeaponCompileTests(unittest.TestCase):
    def test_changed_weapons_publish_paired_server_unit_and_goal_identity(self):
        spec = attack_spec()
        tpl = record_templates()
        observed_labels = []
        tpl.unit_labels = lambda unit: observed_labels.append(unit) or {'win'}
        spec['cutscenes']['intro']['actors']['a']['npc'] = 'boss'
        tpl.blueprints = {1001: [(444001, 'Fixture blueprint')]}
        spec['drops'] = {'normal': False, 'blueprints': 'mission', 'blueprint_per_mille': 5}
        build = compile_spec(spec, tpl)
        rows = client_rows(build)
        unit_id = next(ident for kind, ident in rows if kind == M.UNIT_KIND)
        client_unit = rows[(M.UNIT_KIND, unit_id)]
        client_slots = [struct.unpack_from('<I', client_unit, offset)[0]
                        for offset in M.UNIT_WEAPON_SLOTS + (M.UNIT_SPECIAL_SLOT,)]

        server_unit = build.server['tbl_UnitNpc'][0]
        self.assertEqual(server_unit['UnitNumber'], unit_id)
        self.assertEqual([int(server_unit[name] or 0) for name in M.SERVER_UNIT_WEAPON_FIELDS], client_slots)
        weapons = {row['WeaponNo']: row for row in build.server['tbl_WeaponsRev']}
        self.assertEqual(set(weapons), {value for value in client_slots if value >= M.WEAPON_COPY_FIRST})
        for ident, row in weapons.items():
            self.assertEqual(row['DamageSize'], struct.unpack_from('<I', rows[(M.WEAPON_KIND, ident)], M.WEAPON_DAMAGE)[0])

        npc = next(row for row in build.server['tbl_NpcData'] if row['UseUnitHP'] == 20000)
        self.assertEqual(npc['UseUnitID'], unit_id)
        goals = [row for row in build.server['tbl_MissionClear'] if row['UseHow'] in (2, 6)]
        self.assertTrue(goals)
        self.assertEqual({row['UseObjectID'] for row in goals}, {unit_id})
        self.assertEqual(observed_labels, [1001])  # animation lookup stays on the original unit/model
        self.assertTrue(any(row['CategoryID'] == 444001 for row in build.server['tbl_BattleResultItemDropDice']))

    def test_label_only_and_no_actual_damage_change_do_not_publish_server_clones(self):
        spec = attack_spec()
        spec['npcs']['boss'].pop('attack')
        spec['npcs']['boss'].pop('special_attack')
        build = compile_spec(spec, record_templates())
        self.assertNotIn('tbl_UnitNpc', build.server)
        self.assertNotIn('tbl_WeaponsRev', build.server)
        npc = next(row for row in build.server['tbl_NpcData'] if row['UseUnitHP'] == 20000)
        self.assertEqual(npc['UseUnitID'], 1001)
        goal = next(row for row in build.server['tbl_MissionClear'] if row['UseHow'] == 2)
        self.assertEqual(goal['UseObjectID'], 1001)

        for factor in (1.0, 1.0001):
            spec = attack_spec(factor=factor, label=None)
            spec['npcs']['boss']['special_attack'] = factor
            build = compile_spec(spec, record_templates())
            self.assertNotIn('tbl_UnitNpc', build.server)
            self.assertNotIn('tbl_WeaponsRev', build.server)

    def test_weapon_clones_are_deduplicated_but_unit_clones_are_per_npc(self):
        spec = attack_spec()
        spec['npcs']['boss2'] = dict(spec['npcs']['boss'], label='Boss 2')
        spec['phases'][0]['spawns'].append({'npc': 'boss2', 'positions': [[100, 150, 900]], 'at': 1})
        build = compile_spec(spec, record_templates())
        self.assertEqual(len(build.server['tbl_UnitNpc']), 2)
        self.assertEqual(len(build.server['tbl_WeaponsRev']), 3)
        self.assertEqual(len([1 for kind, _, _ in build.rows if kind == M.WEAPON_KIND]), 3)

    def test_missing_incomplete_or_mismatched_server_sources_fail_closed(self):
        spec = attack_spec()
        tpl = record_templates()
        tpl.server_weapon_record = None
        with self.assertRaisesRegex(M.MissionError, '服务端的武器记录'):
            compile_spec(spec, tpl)

        tpl = record_templates()
        tpl.server_weapon_record = lambda weapon: {'WeaponNo': weapon}
        with self.assertRaisesRegex(M.MissionError, '字段不完整'):
            compile_spec(spec, tpl)

        tpl = record_templates()
        source_weapon = tpl.server_weapon_record
        tpl.server_weapon_record = lambda weapon: {**source_weapon(weapon), 'DamageSize': 1}
        with self.assertRaisesRegex(M.MissionError, '编号或伤害不匹配'):
            compile_spec(spec, tpl)

        tpl = record_templates()
        tpl.server_unit_record = None
        with self.assertRaisesRegex(M.MissionError, '服务端的机体记录'):
            compile_spec(spec, tpl)

        tpl = record_templates()
        source_unit = tpl.server_unit_record
        tpl.server_unit_record = lambda unit, template=None: {**source_unit(unit, template), 'WeaponNumber_1': 999}
        with self.assertRaisesRegex(M.MissionError, '武器槽不匹配'):
            compile_spec(spec, tpl)

    def test_client_clone_id_collision_is_still_refused(self):
        spec = attack_spec()
        tpl = record_templates()
        source = tpl.unit_record
        occupied = M.UNIT_LABEL_FIRST + 40 * 100
        tpl.unit_record = lambda unit: bytes(M.UNIT_RECORD_BYTES) if unit == occupied else source(unit)
        with self.assertRaisesRegex(M.MissionError, '被占用'):
            compile_spec(spec, tpl)

        tpl = record_templates()
        source_weapon = tpl.weapon_record
        occupied_weapon = M.WEAPON_COPY_FIRST + 40 * 100
        tpl.weapon_record = lambda weapon: bytes(M.WEAPON_RECORD_BYTES) if weapon == occupied_weapon else source_weapon(weapon)
        with self.assertRaisesRegex(M.MissionError, '被客户端占用'):
            compile_spec(spec, tpl)


class PlayerUnitConversionTests(unittest.TestCase):
    def test_player_only_gyan_inherits_template_custom_number_without_mutating_sources(self):
        template = {name: 0 for name in M.SERVER_UNIT_NPC_COLUMNS}
        template.update(UnitNumber=600233, CustomNumber=6, UnitModelName='Zaku II template')
        player = {name: 0 for name in repository.UNIT_COLUMNS}
        player.update(UnitNumber=11021, UnitModelName='Gyan', UnitGradeNumber=2, UnitGradeNumber2=4,
                      HP=12500, WeaponNumber_1=41105, WeaponNumber_2=41106,
                      WeaponNumber_3=41107, WeaponNumber_FatalAttack=43107, UnitGraph=1011021)
        before_template, before_player = copy.deepcopy(template), copy.deepcopy(player)
        result = pipeline._server_unit_record({600233: template}, {11021: player}, 11021, 600233)
        self.assertEqual(set(result), set(M.SERVER_UNIT_NPC_COLUMNS))
        self.assertEqual((result['UnitNumber'], result['CustomNumber'], result['UnitModelName']), (11021, 6, 'Gyan'))
        self.assertEqual([result[name] for name in M.SERVER_UNIT_WEAPON_FIELDS], [41105, 41106, 41107, 43107])
        self.assertNotIn('UnitGraph', result)
        self.assertEqual((template, player), (before_template, before_player))
        self.assertIsNone(pipeline._server_unit_record({600233: template}, {11021: player}, 11021, 999999))

    def test_player_only_gyan_compiles_through_the_paired_clone_path(self):
        spec = mission_spec()
        spec['npcs']['gyan'] = {'unit': 11021, 'base': 500, 'hp': 36000, 'label': '马·克贝',
                                'attack': 2.0, 'special_attack': 1.5}
        spec['phases'][0]['spawns'].append({'npc': 'gyan', 'positions': [[0, 150, 900]], 'at': 1})
        spec['phases'][0]['goal'] = {'kill': [{'npc': 'gyan'}]}
        tpl = record_templates()
        original_client = tpl.unit_record(1001)
        gyan_client = bytearray(original_client)
        struct.pack_into('<I', gyan_client, 0, 11021)
        original_server = tpl.server_unit_record(1001, 1001)
        player = {name: original_server[name] for name in M.SERVER_UNIT_NPC_COLUMNS
                  if name not in ('CustomNumber',)}
        player['UnitNumber'] = 11021
        player['UnitModelName'] = 'Gyan'
        player['UnitGraph'] = 1011021
        tpl.unit_exists = lambda unit: unit in (1001, 11021)
        tpl.unit_record = lambda unit: bytes(gyan_client) if unit == 11021 else (
            original_client if unit == 1001 else None)
        tpl.server_unit_record = lambda unit, template=None: pipeline._server_unit_record(
            {1001: original_server}, {11021: player}, unit, template)
        build = compile_spec(spec, tpl)
        clone = build.server['tbl_UnitNpc'][0]
        self.assertEqual((clone['CustomNumber'], clone['UnitModelName']), (6, 'Gyan'))
        self.assertEqual(next(row for row in build.server['tbl_NpcData'] if row['UseUnitHP'] == 36000)['UseUnitID'],
                         clone['UnitNumber'])


class ServerPlanTests(unittest.TestCase):
    def setUp(self):
        self.build = compile_spec(attack_spec(), record_templates())
        self.plan = plan_for(self.build)

    def load(self, plan=None):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'plan.json'
            path.write_text(json.dumps(plan or self.plan), encoding='utf-8')
            return server.load_plan(path)

    def test_complete_paired_plan_loads_and_orders_dependencies(self):
        loaded = self.load()
        self.assertTrue(loaded['tables']['tbl_WeaponsRev'])
        legacy = copy.deepcopy(self.plan)
        legacy['tables']['tbl_WeaponsRev'][0]['WeaponName'] = 'legacy\0name'
        self.assertEqual(self.load(legacy)['tables']['tbl_WeaponsRev'][0]['WeaponName'], 'legacy\0name')
        self.assertLess(server.TABLE_ORDER.index('tbl_WeaponsRev'), server.TABLE_ORDER.index('tbl_UnitNpc'))
        self.assertLess(server.TABLE_ORDER.index('tbl_UnitNpc'), server.TABLE_ORDER.index('tbl_NpcData'))

    def test_malformed_or_unpaired_clone_rows_are_refused(self):
        plan = copy.deepcopy(self.plan)
        plan['tables']['tbl_UnitNpc'][0].pop('CustomNumber')
        with self.assertRaisesRegex(ValueError, '列与预期'):
            self.load(plan)
        plan = copy.deepcopy(self.plan)
        plan['tables']['tbl_UnitNpc'][0]['UnitModelName'] = 'x' * 25
        with self.assertRaisesRegex(ValueError, '长度不超过 24'):
            self.load(plan)
        plan = copy.deepcopy(self.plan)
        plan['tables']['tbl_WeaponsRev'].pop()
        with self.assertRaisesRegex(ValueError, '缺失的派生武器|未被派生机体引用'):
            self.load(plan)
        plan = copy.deepcopy(self.plan)
        plan['tables']['tbl_UnitNpc'].append(copy.deepcopy(plan['tables']['tbl_UnitNpc'][0]))
        with self.assertRaisesRegex(ValueError, '重复键'):
            self.load(plan)

    def test_existing_ledger_owned_clones_upgrade_in_place_then_keep(self):
        rows = {(table, tuple(row[key] for key in bundle.TABLE_KEYS[table])): copy.deepcopy(row)
                for table, items in self.plan['tables'].items() for row in items}
        weapon_key = next(key for key in rows if key[0] == 'tbl_WeaponsRev')
        rows[weapon_key]['DamageSize'] -= 1
        unit_key = next(key for key in rows if key[0] == 'tbl_UnitNpc')
        rows[unit_key]['HP'] += 1
        owner = 'test-owner'
        owned = {(weapon_key[0], server._row_key(weapon_key[0], rows[weapon_key])): owner,
                 (unit_key[0], server._row_key(unit_key[0], rows[unit_key])): owner}
        cursor = _PlanCursor(rows, owned)
        first = server._plan_changes(cursor, self.plan, lock=False, map_key=owner)
        updated = {table for table, _ in first['_changes']['update']}
        self.assertEqual(updated, {'tbl_WeaponsRev', 'tbl_UnitNpc'})
        for table, row in first['_changes']['update']:
            key = (table, tuple(row[name] for name in bundle.TABLE_KEYS[table]))
            rows[key] = copy.deepcopy(row)
        second = server._plan_changes(_PlanCursor(rows, owned), self.plan, lock=False, map_key=owner)
        self.assertEqual(second['update'], 0)
        self.assertEqual(second['conflicts'], [])
        self.assertEqual(second['keep'], sum(len(items) for items in self.plan['tables'].values()))

        differing = {**rows, weapon_key: {**rows[weapon_key], 'DamageSize': 1}}
        unowned = server._plan_changes(_PlanCursor(differing, {}), self.plan, lock=False, map_key=owner)
        self.assertTrue(any(item['table'] == 'tbl_WeaponsRev' for item in unowned['conflicts']))
        weapon_ledger_key = next(key for key in owned if key[0] == 'tbl_WeaponsRev')
        other_owned = server._plan_changes(_PlanCursor(differing, {weapon_ledger_key: 'other-owner'}),
                                           self.plan, lock=False, map_key=owner)
        self.assertTrue(any(item['table'] == 'tbl_WeaponsRev' for item in other_owned['conflicts']))
        foreign_identical = server._plan_changes(_PlanCursor(rows, {weapon_ledger_key: 'other-owner'}),
                                                self.plan, lock=False, map_key=owner)
        self.assertTrue(any(item['table'] == 'tbl_WeaponsRev' for item in foreign_identical['conflicts']))
        self.assertFalse(any(table == 'tbl_WeaponsRev' and row['WeaponNo'] == weapon_key[1][0]
                             for table, row in foreign_identical['_changes']['keep']))


class _PlanCursor:
    def __init__(self, rows, owned):
        self.rows = rows
        self.owned = owned
        self.result = []

    def execute(self, sql, *params):
        if sql.startswith("SELECT OBJECT_ID"):
            self.result = [(1,)]
        elif sql.startswith('SELECT TableName,RowKey,MapKey'):
            self.result = sorted((table, key, owner) for (table, key), owner in self.owned.items())
        elif sql.startswith('SELECT ') and ' WITH (UPDLOCK,HOLDLOCK) ' in sql:
            match = re.search(r'FROM dbo\.([A-Za-z0-9_]+)', sql)
            table = match.group(1)
            row = self.rows.get((table, tuple(params)))
            self.result = [] if row is None else [tuple(row[name] for name in server.COLUMNS[table])]
        else:
            raise AssertionError(sql)
        return self

    def fetchone(self):
        return self.result[0] if self.result else None

    def fetchall(self):
        return list(self.result)


if __name__ == '__main__':
    unittest.main()
