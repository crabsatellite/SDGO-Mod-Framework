import copy
import json
import struct
import tempfile
import unittest
from pathlib import Path
from owner.maps import mission as M, package as P
from tests.test_map_import import mission_spec, templates, ids, write_package


def build(spec):
    tpl=templates()
    tpl.routes[2]=tpl.routes[1]
    with tempfile.TemporaryDirectory()as tmp:
        pkg=P.load(write_package(Path(tmp),mission=spec))
        return M.compile_mission(pkg,spec,map_no=144,terrain_path='Trns\\m143x.trn',ids=ids(),templates=tpl)


class DifficultyHpTests(unittest.TestCase):
    def spec(self):
        d=mission_spec();d['schema_version']=5
        d['difficulties']=[{'level':1,'time_limit':600,'lives':7},{'level':2,'time_limit':600,'lives':5}]
        d['npcs']['target']['hp_by_difficulty']={'1':1000,'2':5000}
        d['phases'][0]['retreat_allies']=['ally']
        d['npcs']['ally']['hp_by_difficulty']={'1':6000,'2':9000}
        return d
    def test_difficulties_have_paired_distinct_npc_templates_and_spawns(self):
        out=build(self.spec())
        npc_rows={ident:raw for kind,ident,raw in out.rows if kind==M.NPC_TEMPLATE_KIND}
        sql={row['NpcDataID']:row for row in out.server['tbl_NpcData']}
        expected={1:1000,2:5000}
        ids_seen=set()
        for row in out.server['tbl_MissionNpc']:
            if row['NpcParty']!=2:continue
            ident=row['NpcID'];ids_seen.add(ident)
            self.assertEqual(sql[ident]['UseUnitHP'],expected[row['MissionDifficult']])
            self.assertEqual(struct.unpack_from('<I',npc_rows[ident],229)[0],sql[ident]['UseUnitHP'])
        self.assertEqual(len(ids_seen),2)
        retreats=out.report['retreats']
        self.assertNotEqual(retreats[0]['npc_id'],retreats[1]['npc_id'])
        self.assertEqual(sql[retreats[0]['npc_id']]['UseUnitHP'],6000)
        self.assertEqual(sql[retreats[1]['npc_id']]['UseUnitHP'],9000)
    def test_mapping_must_cover_exact_declared_levels(self):
        for values in ({'1':1000},{'1':1000,'2':0},{'1':1000,'2':True},{'1':1000,'2':2000,'3':3000}):
            d=self.spec();d['npcs']['target']['hp_by_difficulty']=values
            with self.subTest(values=values),self.assertRaisesRegex(M.MissionError,'hp_by_difficulty'):
                build(d)
    def test_old_compiler_schema_cannot_silently_ignore_hp_variants(self):
        d=self.spec();d['schema_version']=4;d['phases'][0].pop('retreat_allies')
        with self.assertRaisesRegex(M.MissionError,'schema_version=5'):build(d)


if __name__=='__main__':unittest.main()
