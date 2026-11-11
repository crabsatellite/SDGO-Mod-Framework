"""Imported mission transition policy: compiler, server plan and BattleServer configuration."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))

from manager.maps import Maps  # noqa: E402
from owner.maps import bundle, compiler, mission as M, package as P, server  # noqa: E402
from test_map_import import compile_spec, ids, mission_spec, templates, write_package  # noqa: E402


class TransitionPolicyCompileTests(unittest.TestCase):
    def test_transition_delay_defaults_to_native_and_rejects_unknown_values(self):
        spec = mission_spec()
        self.assertEqual(compile_spec(spec).report['transition_delay'], 'native')
        spec['schema_version'] = 2
        spec['transition_delay'] = 'immediate'
        self.assertEqual(compile_spec(spec).report['transition_delay'], 'immediate')
        spec['transition_delay'] = 'fast'
        with self.assertRaisesRegex(M.MissionError, 'transition_delay'):
            compile_spec(spec)

    def _publication(self, root: Path, transition_delay: str):
        spec = mission_spec()
        if transition_delay != 'native':
            spec['schema_version'] = 2
            spec['transition_delay'] = transition_delay
        pkg = P.load(write_package(root / 'package', mission=spec))
        map_build = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp')
        mission_build = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn',
                                          ids=ids(), templates=templates())
        report = bundle.build_publication([map_build], root / 'publication', missions=[mission_build])
        plan_path = Path(report['server_plan'])
        return report, json.loads(plan_path.read_text(encoding='utf-8')), plan_path

    def test_runtime_policy_is_included_in_revision_and_server_plan(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            native_report, native_plan, _ = self._publication(root / 'native', 'native')
            immediate_report, immediate_plan, _ = self._publication(root / 'immediate', 'immediate')
        self.assertEqual(native_plan['schema'], bundle.PLAN_SCHEMA)
        self.assertEqual(immediate_plan['schema'], bundle.PLAN_SCHEMA)
        self.assertEqual(native_plan['runtime_policy'], {'immediate_transition_missions': []})
        self.assertEqual(immediate_plan['runtime_policy'], {'immediate_transition_missions': [ids().game_mode_first]})
        self.assertNotEqual(native_report['revision'], immediate_report['revision'])

    def test_server_plan_rejects_noncanonical_and_unplanned_mission_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _, plan, path = self._publication(root, 'immediate')
            plan['runtime_policy']['immediate_transition_missions'] = [True]
            path.write_text(json.dumps(plan), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '正整数'):
                server.load_plan(path)
            plan['runtime_policy']['immediate_transition_missions'] = [ids().game_mode_first + 99]
            path.write_text(json.dumps(plan), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '计划外'):
                server.load_plan(path)
            plan['runtime_policy']['immediate_transition_missions'] = [ids().game_mode_first,
                                                                        ids().game_mode_first]
            path.write_text(json.dumps(plan), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '递增且不重复'):
                server.load_plan(path)

    def test_legacy_server_plan_is_read_only_compatible_without_new_semantics(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _, plan, path = self._publication(root, 'native')
            plan['schema'] = 'sdgo.map-import-server-plan/1'
            plan.pop('runtime_policy')
            plan['tables'].pop('tbl_WeaponsRev', None)
            plan['tables'].pop('tbl_UnitNpc', None)
            path.write_text(json.dumps(plan), encoding='utf-8')
            loaded = server.load_plan(path)
            self.assertEqual(loaded['runtime_policy'], {'immediate_transition_missions': []})

            plan['runtime_policy'] = {'immediate_transition_missions': [ids().game_mode_first]}
            path.write_text(json.dumps(plan), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '旧版.*runtime_policy'):
                server.load_plan(path)

            plan['runtime_policy'] = {'immediate_transition_missions': []}
            plan['tables']['tbl_UnitNpc'] = []
            path.write_text(json.dumps(plan), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '旧版.*配对机体'):
                server.load_plan(path)

    def test_runtime_allowlist_matches_native_capacity_and_uint32_limits(self):
        def plan(values):
            return {'tables': {'tbl_MissionMode': [{'GameModeID': value} for value in values]},
                    'runtime_policy': {'immediate_transition_missions': values}}

        with self.assertRaisesRegex(ValueError, 'uint32'):
            server._validate_runtime_policy(plan([0x100000000]))
        values = list(range(1, 258))
        with self.assertRaisesRegex(ValueError, '容量'):
            server._validate_runtime_policy(plan(values))
        values = list(range(4_000_000_000, 4_000_000_220))
        self.assertLessEqual(len(values), 256)
        with self.assertRaisesRegex(ValueError, '容量'):
            server._validate_runtime_policy(plan(values))


class TransitionPolicyDeploymentTests(unittest.TestCase):
    @staticmethod
    def _plan(mission_ids):
        return {'tables': {'tbl_MissionMode': [{'GameModeID': value} for value in sorted(set(mission_ids))]},
                'runtime_policy': {'immediate_transition_missions': list(mission_ids)}}

    @staticmethod
    def _server(root: Path):
        world = root / 'Server/WorldServer'
        battle = root / 'Server/BattleServer1'
        world.mkdir(parents=True)
        battle.mkdir(parents=True)
        (world / 'WorldServer.exe').write_bytes(b'fixture')
        (battle / 'BattleServer.exe').write_bytes(b'fixture')
        ini = world / 'WorldServer.ini'
        ini.write_text('[DB Game]\nDBServerName=127.0.0.1\nDBName=fixture\n', encoding='ascii')
        return ini, battle

    @staticmethod
    def _install_module(battle: Path):
        mod = battle / 'sdgo/mods/map_catalog_server'
        mod.mkdir(parents=True)
        (mod / 'map_catalog_server.dll').write_bytes(b'fixture')
        (mod / 'symbols.fragment.ini').write_text('[fixture]\n', encoding='ascii')
        (mod / 'mod.ini').write_text(
            '[mod]\nid=map_catalog_server\nenabled=0\ncategory=gameplay\nfeature=map-import\n'
            '[private]\nKeep=unchanged\n', encoding='utf-8')
        (mod / 'config.ini').write_text(
            '; keep this comment\n[settings]\nUnrelated=preserved\nImmediateTransitionMissionIds=77\n',
            encoding='utf-8')
        return mod

    def test_apply_preserves_unrelated_ini_and_reports_actual_activation(self):
        with tempfile.TemporaryDirectory() as temporary:
            ini, battle = self._server(Path(temporary))
            mod = self._install_module(battle)
            plan = self._plan([90001, 90002])
            result = server.apply_runtime_policy(plan, ini)
            config = (mod / 'config.ini').read_text(encoding='utf-8')
            mod_ini = (mod / 'mod.ini').read_text(encoding='utf-8')
            self.assertEqual(result['status'], 'configured')
            self.assertFalse(result['policy_active'])
            self.assertFalse(result['runtime_verified'])
            self.assertTrue(result['restart_required'])
            self.assertEqual(result['configured_hosts'], ['BattleServer1'])
            self.assertEqual(result['ready_hosts'], ['BattleServer1'])
            self.assertIn('; keep this comment', config)
            self.assertIn('Unrelated=preserved', config)
            self.assertIn('ImmediateTransitionMissionIds=90001,90002', config)
            self.assertIn('Keep=unchanged', mod_ini)
            self.assertIn('enabled=1', mod_ini)

            second = server.apply_runtime_policy(plan, ini)
            self.assertEqual(second['status'], 'configured')
            self.assertFalse(second['configuration_changed'])
            self.assertFalse(second['restart_required'])

            (battle / 'sdgo/sdgo.ini').write_text('[features]\nmap-import=0\n', encoding='utf-8')
            state = server.runtime_policy_state(plan, ini)
            self.assertEqual(state['status'], 'activation_required')
            self.assertEqual(state['configured_hosts'], ['BattleServer1'])
            self.assertEqual(state['ready_hosts'], [])
            self.assertFalse(state['policy_active'])

    def test_existing_config_must_fit_the_native_parser_capacity(self):
        too_many = ','.join(['1'] * 257)
        self.assertIsNone(server._configured_mission_ids(
            ('[settings]\nImmediateTransitionMissionIds=' + too_many + '\n').encode('ascii')))

        too_long = ','.join(str(value) for value in range(4_000_000_000, 4_000_000_220))
        self.assertGreaterEqual(len(too_long.encode('ascii')), 2047)
        self.assertIsNone(server._configured_mission_ids(
            ('[settings]\nImmediateTransitionMissionIds=' + too_long + '\n').encode('ascii')))

    def test_missing_programme_is_pending_and_never_claimed_active(self):
        with tempfile.TemporaryDirectory() as temporary:
            ini, battle = self._server(Path(temporary))
            result = server.apply_runtime_policy(self._plan([90001]), ini)
            self.assertEqual(result['status'], 'pending_programme_deployment')
            self.assertEqual(result['missing_hosts'], ['BattleServer1'])
            self.assertFalse(result['policy_active'])
            self.assertFalse((battle / 'sdgo/mods/map_catalog_server/config.ini').exists())

    def test_topology_error_is_not_misreported_as_missing_programme(self):
        plan = self._plan([90001])
        with mock.patch.object(server, '_battle_policy_hosts', side_effect=ValueError('拓扑不可读')):
            result = server.apply_runtime_policy(plan, 'fixture.ini')
        self.assertEqual(result['status'], 'topology_error')
        self.assertEqual(result['topology_error'], '拓扑不可读')
        self.assertEqual(result['missing_hosts'], [])
        self.assertFalse(result['policy_active'])

    def test_write_failure_restores_the_existing_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            ini, battle = self._server(Path(temporary))
            mod = self._install_module(battle)
            original_config = (mod / 'config.ini').read_bytes()
            original_mod = (mod / 'mod.ini').read_bytes()
            calls = []

            def atomic(path, data):
                calls.append(Path(path).name)
                if len(calls) == 2:
                    raise OSError('fixture failure')
                Path(path).write_bytes(data)

            with mock.patch('manager.storage.atomic', side_effect=atomic):
                with self.assertRaisesRegex(ValueError, '已恢复原配置'):
                    server.apply_runtime_policy(self._plan([90001]), ini)
            self.assertEqual((mod / 'config.ini').read_bytes(), original_config)
            self.assertEqual((mod / 'mod.ini').read_bytes(), original_mod)

    def test_empty_policy_keeps_native_timing_without_requiring_the_programme(self):
        with tempfile.TemporaryDirectory() as temporary:
            ini, _ = self._server(Path(temporary))
            result = server.apply_runtime_policy(self._plan([]), ini)
            self.assertEqual(result['status'], 'native')
            self.assertFalse(result['policy_active'])
            self.assertEqual(result['missing_hosts'], [])


class TransitionPolicyManagerTests(unittest.TestCase):
    def test_server_rows_apply_before_missing_programme_is_reported(self):
        workspace = SimpleNamespace(root=Path('.'), mode='developer', settings={'modules': {'maps': {}}})
        provider = Maps(workspace)
        plan = {'tables': {'tbl_MissionMode': [{'GameModeID': 90001}]},
                'runtime_policy': {'immediate_transition_missions': [90001]}}
        events = []
        pending = {'status': 'pending_programme_deployment', 'missing_hosts': ['BattleServer1'],
                   'mission_ids': [90001], 'policy_active': False, 'runtime_verified': False}

        def apply_rows(*_args):
            events.append('rows')
            return {'inserted': 1}

        def apply_policy(*_args):
            events.append('policy')
            return pending

        with mock.patch.object(provider, '_plan', return_value=plan), \
                mock.patch.object(provider, '_server_ini', return_value='fixture.ini'), \
                mock.patch.object(provider, '_state', return_value={'ok': True}), \
                mock.patch.object(server, 'apply', side_effect=apply_rows), \
                mock.patch.object(server, 'apply_runtime_policy', side_effect=apply_policy):
            response = provider.request('POST', '/api/maps/server/apply', {}, {})
        self.assertEqual(events, ['rows', 'policy'])
        self.assertEqual(response['result']['runtime_policy'], pending)
        self.assertIn('节奏仍为原生等待', response['message'])


if __name__ == '__main__':
    unittest.main()
