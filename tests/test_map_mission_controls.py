"""Mission prerequisite protection and stock-style party movies, without a game or SQL."""
import copy
import io
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from owner.maps import bundle, compiler, damage_gate_format as G, deployment, mission as M, package as P, retreat_format as R, server
from tests.test_map_import import ids, mission_spec, templates, write_package


def control_spec():
    spec = mission_spec()
    spec['schema_version'] = 5
    spec['npcs']['guard'] = {'template': 501}
    spec['phases'][1]['spawns'] = [
        {'npc': 'target', 'positions': [[0, 150, 600]], 'at': 0},
        {'npc': 'guard', 'positions': [[300, 150, 600]], 'at': 0}]
    spec['phases'][1]['goal'] = {'kill': [{'npc': 'guard', 'count': 1}, {'npc': 'target', 'count': 1}]}
    spec['phases'][1]['damage_gates'] = [{'target': 'target', 'requires_defeat': ['guard'], 'warning': 'l1'}]
    return spec


def control_templates():
    data = templates()
    row = bytearray(data.npc_templates[500])
    struct.pack_into('<I', row, 0, 501)
    struct.pack_into('<I', row, 105, 1002)
    data.npc_templates[501] = bytes(row)
    data.npc_data[501] = dict(data.npc_data[500], NpcDataID=501, UseUnitID=1002)
    return data


def compile_control(root, spec):
    pkg = P.load(write_package(root/'package', mission=spec))
    mission = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn',
                                ids=ids(), templates=control_templates())
    mapped = compiler.compile_map(pkg, map_no=144, map_id=500, variant='pvp')
    report = bundle.build_publication([mapped], root/'publication', missions=[mission])
    return mission, report


class MissionControlTests(unittest.TestCase):
    def test_gate_roundtrip_and_tamper_rejection(self):
        rows = [dict(mission_id=88072, phase=2, target_npc_id=500, guard_npc_id=501, required_defeats=1)]
        raw = G.encode(rows)
        self.assertEqual(G.decode(raw), rows)
        bad = bytearray(raw)
        bad[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            G.decode(bytes(bad))
        with self.assertRaisesRegex(ValueError, 'guard itself'):
            G.encode([dict(rows[0], guard_npc_id=500)])

    def test_paired_publication_contains_real_guard_count_and_warning(self):
        spec = control_spec()
        spec['phases'][1]['spawns'][1]['positions'].append([-300, 150, 600])
        spec['phases'][1]['goal']['kill'][0]['count'] = 2
        with tempfile.TemporaryDirectory() as tmp:
            built, output = compile_control(Path(tmp), spec)
            rows = built.report['damage_gates']
            self.assertEqual(rows[0]['required_defeats'], 2)
            self.assertIn('function SDGOMI_DG_88072_2_500', built.lua)
            with zipfile.ZipFile(output['client_zip']) as archive:
                manifest = json.loads(archive.read('manifest.json'))
                self.assertEqual(manifest['schema_version'], 2)
                self.assertEqual(manifest['minimum_programme'], '0.7.0')
                self.assertEqual(G.decode(archive.read(bundle.PREFIX+'damage-gates.bin')), rows)
            plan = server.load_plan(output['server_plan'])
            self.assertEqual(plan['runtime_policy']['damage_gates'], rows)
            plan['runtime_policy']['damage_gates'][0]['required_defeats'] = 3
            Path(output['server_plan']).write_text(json.dumps(plan), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '守卫刷怪数量'):
                server.load_plan(output['server_plan'])

    def test_self_cycle_absent_and_old_schema_are_refused(self):
        for kind in ('self', 'cycle', 'absent', 'old'):
            spec = control_spec()
            gates = spec['phases'][1]['damage_gates']
            if kind == 'self': gates[0]['requires_defeat'] = ['target']
            elif kind == 'cycle': gates.append({'target':'guard','requires_defeat':['target']})
            elif kind == 'absent': spec['phases'][1]['spawns'].pop()
            else: spec['schema_version'] = 4
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaises(M.MissionError):
                    compile_control(Path(tmp), spec)

    def test_advanced_package_refuses_old_programme_before_any_content_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _, output = compile_control(root/'source', control_spec())
            client = root/'client'
            folder = client/'sdgo/mods/map_catalog'
            folder.mkdir(parents=True)
            (client/'GOnline.exe').write_bytes(b'not executed')
            (folder/'mod.ini').write_bytes(b'[mod]\nid=map_catalog\nversion=0.6.0\n')
            (folder/'catalog.bin').write_bytes(b'owned existing bytes')
            before = {p.relative_to(client):p.read_bytes()for p in client.rglob('*')if p.is_file()}
            with patch.object(deployment, '_running', return_value=False):
                with self.assertRaisesRegex(ValueError, '0.7.0'):
                    deployment.install_package(client, Path(output['client_zip']).read_bytes())
            self.assertEqual(before, {p.relative_to(client):p.read_bytes()for p in client.rglob('*')if p.is_file()})

    def test_replacing_gated_mission_removes_its_old_client_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            spec = control_spec()
            _, gated = compile_control(root/'gated', spec)
            del spec['phases'][1]['damage_gates']
            spec['phases'][1]['goal'] = {'kill':[{'npc':'target','count':1}]}
            _, direct = compile_control(root/'direct', spec)
            client = root/'client'
            folder = client/'sdgo/mods/map_catalog'
            folder.mkdir(parents=True)
            (client/'GOnline.exe').write_bytes(b'not executed')
            (folder/'mod.ini').write_bytes(b'[mod]\nid=map_catalog\nversion=0.7.0\n')
            (folder/'personal.json').write_bytes(b'preserve unrelated author data')
            with patch.object(deployment, '_running', return_value=False):
                deployment.install_package(client, Path(gated['client_zip']).read_bytes())
                self.assertTrue((folder/'damage-gates.bin').is_file())
                result = deployment.install_package(client, Path(direct['client_zip']).read_bytes())
            self.assertFalse((folder/'damage-gates.bin').exists())
            self.assertIn('sdgo/mods/map_catalog/damage-gates.bin',result['removed'])
            self.assertEqual((folder/'personal.json').read_bytes(),b'preserve unrelated author data')
            self.assertEqual(result['receipt']['revision'],direct['revision'])

    def test_factory_style_party_preserves_route_and_cleanup_semantics(self):
        spec = mission_spec()
        spec['schema_version'] = 5
        spec['cutscenes']['move'] = copy.deepcopy(spec['cutscenes']['intro'])
        spec['cutscenes']['move']['party'] = {
            'from':[[-200,150,-300],[200,150,-300]],
            'via':[[-200,150,100],[200,150,100]],
            'to':[[-200,150,500],[200,150,500]], 'include_wingmen':True}
        spec['sectors'] = {'next':{'warp':[[-200,150,500],[200,150,500]]}}
        spec['phases'][1]['enter'].update(cutscene='move',sector='next')
        with tempfile.TemporaryDirectory() as tmp:
            built, output = compile_control(Path(tmp), spec)
        self.assertEqual(built.report['party_cutscenes'], ['move'])
        self.assertIn('CreateDummyPlayer(source)', built.lua)
        self.assertIn('actor:SetMoveTargetPos(p.finish[i])\n          actor:SaveActEvent()\n          actor:SetMoveTargetPos(p.via[i])', built.lua)
        self.assertIn('function SDGOMI.skip(m, c) SDGOMI.cleanup(m, c); return 456 end', built.lua)
        self.assertIn('UnHideObject(s.hidden[i])', built.lua)
        self.assertIn('s.started_ms = SDGO_MapCatalogClockMs()', built.lua)
        self.assertIn('s.frame = (SDGO_MapCatalogClockMs() - s.started_ms) * 0.06', built.lua)
        self.assertLess(built.lua.index('cam:SetCameraPos(c.cam0)'), built.lua.index('s.started_ms = SDGO_MapCatalogClockMs()'))

    def test_party_mismatched_slots_and_legacy_version_are_refused(self):
        spec = mission_spec()
        spec['cutscenes']['move'] = copy.deepcopy(spec['cutscenes']['intro'])
        spec['cutscenes']['move']['party'] = {'from':[[0,150,0]],'via':[[0,150,100]],'to':[[0,150,200]]}
        spec['sectors'] = {'next':{'warp':[[0,150,200]]}}
        spec['phases'][1]['enter'].update(cutscene='move',sector='next')
        with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(M.MissionError, 'schema_version=5'):
            compile_control(Path(tmp), spec)
        spec['schema_version'] = 5
        spec['cutscenes']['move']['party']['to'].append([200,150,200])
        spec['sectors']['next']['warp'].append([200,150,200])
        with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(M.MissionError, '数量必须相同'):
            compile_control(Path(tmp), spec)

    def test_quiet_retreat_is_presentation_only_and_changes_publication_revision(self):
        spec = mission_spec()
        spec['schema_version'] = 5
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before, old = compile_control(root/'normal', spec)
            spec['phases'][0]['retreat_allies'] = ['ally']
            after, new = compile_control(root/'retreat', spec)
            self.assertEqual(before.server, after.server)
            self.assertNotEqual(old['revision'], new['revision'])
            self.assertTrue(after.report['retreats'])
            with zipfile.ZipFile(new['client_zip'])as archive:
                self.assertEqual(R.decode(archive.read(bundle.PREFIX+'retreats.bin')), after.report['retreats'])
        self.assertTrue(all(row['FriendNpcRemove']==1 for row in after.server['tbl_MissionClear']))

    def test_quiet_retreat_cannot_hide_enemy_or_old_protocol(self):
        for old in (False, True):
            spec = mission_spec()
            spec['schema_version'] = 4 if old else 5
            spec['phases'][0]['retreat_allies'] = ['ally'if old else'target']
            with self.subTest(old=old), tempfile.TemporaryDirectory()as tmp, self.assertRaises(M.MissionError):
                compile_control(Path(tmp), spec)

    def test_missing_gated_server_programme_blocks_before_database_connection(self):
        with tempfile.TemporaryDirectory()as tmp:
            _, output = compile_control(Path(tmp), control_spec())
            plan = server.load_plan(output['server_plan'])
        with patch.object(server, 'runtime_policy_state', return_value={
                'topology_error':'','missing_hosts':['BattleServer1']}), \
                patch.object(server.repository, '_connect')as connect:
            with self.assertRaisesRegex(ValueError, '0.3.0'):
                server.apply(plan, 'not connected', 'test')
            connect.assert_not_called()


if __name__ == '__main__':
    unittest.main()
