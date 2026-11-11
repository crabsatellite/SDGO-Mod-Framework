"""Offline contract of the OC special GameTest (--oc-special) and the OC battle stat check (oc.stats).

The native player probe fires the play unit's special in one ordinary PvP battle; oc.special checks the
OC client's record of the local special computation and oc.stats the local battle stat block.
"""
import importlib.util
import json
import unittest
from pathlib import Path

from tests import test_game_test as runner_tests

gt = runner_tests.game_test
ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('game_test_backend_oc_special', ROOT / 'tools/game_test_backend.py')
backend = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(backend)
UID = 10906
BATTLE = ('pvp.room', 'pvp.start', 'pvp.ai_spawn')


class OcSpecialCaseTests(unittest.TestCase):
    def test_special_battle_waits_for_the_special_before_the_return(self):
        cases = gt.required_flow_cases('1', play_unit_uid=UID, oc_allocate='5:2', oc_room=True, oc_special=True)
        self.assertEqual(cases, gt.FLOW_CASES[:3] + ('oc.allocate', 'setup.active_unit', 'oc.create') + BATTLE +
                         ('oc.stats', 'oc.special', 'pvp.return', 'oc.panel'))

    def test_plain_room_is_the_negative_control_with_the_same_cases(self):
        cases = gt.required_flow_cases('1', play_unit_uid=UID, oc_allocate='0:1,5:1', oc_special=True)
        self.assertEqual(cases, gt.FLOW_CASES[:3] + ('oc.allocate', 'setup.active_unit') + BATTLE +
                         ('oc.stats', 'oc.mobility', 'oc.special', 'pvp.return', 'oc.panel'))

    def test_attack_or_defense_allocation_checks_the_battle_stat_block(self):
        for allocation in ('2:1', '3:2', '1:1,3:1'):
            with self.subTest(allocation=allocation):
                cases = gt.required_flow_cases('1', play_unit_uid=UID, oc_allocate=allocation)
                self.assertIn('oc.stats', cases)
                self.assertNotIn('oc.special', cases)
                self.assertLess(cases.index('pvp.ai_spawn'), cases.index('oc.stats'))
                self.assertLess(cases.index('oc.stats'), cases.index('pvp.return'))
        for allocation in ('1:1', '0:1', '5:1'):
            with self.subTest(allocation=allocation):
                self.assertNotIn('oc.stats', gt.required_flow_cases('1', play_unit_uid=UID, oc_allocate=allocation))

    def test_room_switch_keeps_its_own_round_cases(self):
        cases = gt.required_flow_cases('1', play_unit_uid=UID, oc_allocate='2:1', room_switch=True)
        self.assertNotIn('oc.stats', cases)

    def test_special_scope_is_rejected_outside_its_battle(self):
        for options in ({'oc_special': True},
                        {'oc_special': True, 'oc_allocate': '1:1'},
                        {'oc_special': True, 'oc_allocate': '5:1', 'room_switch': True},
                        {'oc_special': 1, 'oc_allocate': '5:1'},
                        {'oc_special': True, 'oc_allocate': '5:1', 'pages_only': True}):
            with self.subTest(options=options), self.assertRaises(gt.GameTestError):
                gt.required_flow_cases('1', play_unit_uid=UID, **options)
        with self.assertRaises(gt.GameTestError):
            gt.required_flow_cases('1,3', play_unit_uid=UID, oc_allocate='5:1', oc_special=True)


class OcSpecialPrepareTests(unittest.TestCase):
    def setUp(self):
        self.fixture = runner_tests.GameTestRunnerTests(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.cleanup_fixture)
        self.set_observer_version('0.1.33')

    def cleanup_fixture(self):
        try:
            self.fixture.tearDown()
        finally:
            self.fixture.doCleanups()

    def set_observer_version(self, version):
        manifest = self.fixture.stage / 'client/sdgo/mods/game_test_client/mod.ini'
        data = gt.read_ini(manifest)
        data['mod']['version'] = version
        gt.write_ini(manifest, data)

    def prepare(self, name, **options):
        options.setdefault('play_unit_uid', UID)
        return self.fixture.prepare(name, **options)

    def observer(self, run):
        return gt.read_ini(run / 'client/sdgo/mods/game_test_client/mod.ini')['settings']

    def test_special_battle_is_sealed_with_the_probe_settings(self):
        run = self.prepare('oc-special', oc_allocate='5:2', oc_room=True, oc_special=True)
        metadata = self.fixture.metadata(run)
        self.assertTrue(metadata['plan']['oc_special'])
        job = gt.read_ini(run / 'client/sdgo/gametest/run.ini')['run']
        self.assertEqual(job['oc_special'], '1')
        # Seven enemy AI and no allied AI hunt the only human, so a target comes within reach.
        self.assertEqual((job['expected_ai'], job['ai_allies']), ('7', '0'))
        self.assertTrue(job['room_title'].endswith(' !ai e7 a0 !oc'))
        self.assertLessEqual(len(job['room_title'].encode('utf-8')), 20)
        self.assertEqual(job['required_cases'].split(',')[-4:], ['oc.stats', 'oc.special', 'pvp.return', 'oc.panel'])
        settings = self.observer(run)
        self.assertEqual(settings['BattlePlayerSpecial'], '1')
        for key, value in gt.OC_SPECIAL_OBSERVER_SETTINGS.items():
            self.assertEqual(settings[key], value)
        self.assertIn('sdgo/mods/game_test_client/mod.ini', metadata['client']['immutable_hashes'])
        checked = gt.validate_metadata(run)
        self.assertTrue(checked['plan']['oc_special'])
        public = gt.read_public_plan(run / 'client')
        self.assertIn('oc.special', gt.required_cases_for_plan(public))

    def test_ordinary_battles_keep_the_special_mode_off(self):
        for name, options in (('plain', {}), ('attack', {'oc_allocate': '2:1'})):
            with self.subTest(name=name):
                run = self.prepare(name, **options)
                self.assertEqual(self.observer(run)['BattlePlayerSpecial'], '0')
                self.assertNotIn('oc_special', gt.read_ini(run / 'client/sdgo/gametest/run.ini')['run'])
                self.assertNotIn('oc_special', self.fixture.metadata(run)['plan'])

    def test_special_battle_plays_against_seven_enemy_ai(self):
        run = self.prepare('explicit-layout', oc_allocate='5:1', oc_special=True, expected_ai=7, ai_allies=0)
        job = gt.read_ini(run / 'client/sdgo/gametest/run.ini')['run']
        self.assertTrue(job['room_title'].endswith(' !ai e7 a0'))
        for options in ({'ai_allies': 1, 'expected_ai': 7}, {'expected_ai': 5}, {'expected_ai': 7, 'ai_difficulty': 'Hard'},
                        {'expected_ai': 7, 'ai_allies': 0, 'random_teams': True}, {'expected_ai': 7, 'ai_policy': 'enemy'}):
            with self.subTest(options=options), self.assertRaisesRegex(gt.GameTestError, 'seven enemy AI'):
                self.prepare('wrong-layout', oc_allocate='5:1', oc_special=True, **options)

    def test_reused_client_may_hold_an_earlier_special_seal(self):
        # A stopped client sealed before the all-enemy layout can still be reused; a new seal cannot omit it.
        run = self.prepare('earlier-seal', oc_allocate='5:1', oc_special=True)
        job_path = run / 'client/sdgo/gametest/run.ini'
        job = gt.read_ini(job_path)
        job['run']['expected_ai'] = '3'
        del job['run']['ai_allies']
        gt.write_ini(job_path, job)
        path = run / 'run.json'
        data = json.loads(path.read_text(encoding='utf-8'))
        data['plan']['expected_ai'] = 3
        del data['plan']['ai_allies']
        path.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaisesRegex(gt.GameTestError, 'all-enemy'):
            gt.validate_metadata(run)
        self.assertTrue(gt.validate_metadata(run, for_reuse=True)['plan']['oc_special'])

    def test_special_requires_the_current_observer(self):
        self.set_observer_version('0.1.32')
        with self.assertRaisesRegex(gt.GameTestError, '0.1.33'):
            self.prepare('old-observer', oc_allocate='5:1', oc_room=True, oc_special=True)

    def test_special_rejects_other_scopes(self):
        for options in ({'oc_special': True},
                        {'oc_special': True, 'oc_allocate': '1:1', 'oc_room': True},
                        {'oc_special': True, 'oc_allocate': '5:1', 'room_switch': True},
                        {'oc_special': True, 'oc_allocate': '5:1', 'unit_transform_steps': 3},
                        {'oc_special': True, 'oc_allocate': '5:1', 'apsalus_roundtrip': True},
                        {'oc_special': 1, 'oc_allocate': '5:1'}):
            with self.subTest(options=options), self.assertRaises(gt.GameTestError):
                self.prepare('rejected', **options)

    def test_sealed_special_cannot_be_changed_after_preparation(self):
        run = self.prepare('tampered-plan', oc_allocate='5:2', oc_room=True, oc_special=True)
        path = run / 'run.json'
        data = json.loads(path.read_text(encoding='utf-8'))
        del data['plan']['oc_special']
        path.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaisesRegex(gt.GameTestError, 'OC special'):
            gt.validate_metadata(run)

        run = self.prepare('tampered-job', oc_allocate='5:2', oc_room=True, oc_special=True)
        job_path = run / 'client/sdgo/gametest/run.ini'
        job = gt.read_ini(job_path)
        job['run']['oc_allocate'] = '1:2'
        gt.write_ini(job_path, job)
        with self.assertRaises(gt.GameTestError):
            gt.validate_metadata(run)

        run = self.prepare('tampered-layout', oc_allocate='5:2', oc_room=True, oc_special=True)
        job_path = run / 'client/sdgo/gametest/run.ini'
        job = gt.read_ini(job_path)
        job['run']['ai_allies'] = '1'
        gt.write_ini(job_path, job)
        with self.assertRaises(gt.GameTestError):
            gt.validate_metadata(run)

        run = self.prepare('tampered-observer', oc_allocate='5:2', oc_room=True, oc_special=True)
        manifest = run / 'client/sdgo/mods/game_test_client/mod.ini'
        observer = gt.read_ini(manifest)
        observer['settings']['BattlePlayerSecondary'] = '0'
        gt.write_ini(manifest, observer)
        with self.assertRaisesRegex(gt.GameTestError, 'observer settings'):
            gt.validate_metadata(run)

        run = self.prepare('added-plan', oc_allocate='5:2', oc_room=True)
        path = run / 'run.json'
        data = json.loads(path.read_text(encoding='utf-8'))
        data['plan']['oc_special'] = True
        path.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaises(gt.GameTestError):
            gt.validate_metadata(run)


class OcSpecialBackendFixtureTests(unittest.TestCase):
    def test_serve_accepts_the_development_fixture(self):
        args = backend.build_parser().parse_args([
            'serve', '--work', 'w', '--client-run', 'c', '--stage', 's', '--startup-xml', 'x',
            '--oc-special-probe', 'build/Release/oc_special_server_probe.dll'])
        self.assertEqual(args.oc_special_probe, Path('build/Release/oc_special_server_probe.dll'))

    def test_fixture_site_is_the_native_special_request_sp_check(self):
        symbol = backend.OC_SPECIAL_PROBE_SYMBOL
        self.assertIn('[gametest.oc_special_sp_gate]', symbol)
        self.assertIn('kind=site', symbol)
        self.assertIn('rva=0x000C3AEA', symbol)
        # mov ecx,[ebp-0x328DC] / cmp dword [ecx+5],30000 / jae
        self.assertIn('expect=8B 8D 24 D7 FC FF 81 79 05 30 75 00 00 73 36', symbol)
        source = (ROOT / 'tests/oc_special_server_probe.cpp').read_text(encoding='utf-8')
        self.assertIn('"gametest.oc_special_sp_gate"', source)
        self.assertIn('Never included in runtime releases', source)


if __name__ == '__main__':
    unittest.main()
