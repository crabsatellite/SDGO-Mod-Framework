import unittest
from tests import test_game_test as fixture_module
from tools import game_test_server_report as server_report


class NativeAiTeamPlanTests(unittest.TestCase):
    def setUp(self):
        self.fixture=fixture_module.GameTestRunnerTests(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.addCleanup(self.fixture.tearDown)
        self.gt=fixture_module.game_test

    def test_one_vs_seven_is_preserved_by_sealed_job_roundtrip_with_random_units(self):
        run=self.fixture.prepare('one-vs-seven',expected_ai=7,ai_allies=0,random_loadout=True,random_teams=True)
        metadata=self.fixture.metadata(run)
        self.assertEqual(metadata['plan']['ai_allies'],0)
        self.assertEqual(metadata['plan']['room_title'],metadata['plan']['title_nonce']+' !ai e7 a0 !rand')
        self.assertLessEqual(len(metadata['plan']['room_title'].encode('ascii')),20)
        job=self.gt.read_ini(run/'client/sdgo/gametest/run.ini')
        self.assertEqual(job.getint('run','ai_allies'),0)
        self.assertEqual(job.getint('run','random_teams'),1)
        self.assertTrue(metadata['plan']['random_teams'])
        validated=self.gt.validate_metadata(run)
        self.assertEqual(validated['plan'],metadata['plan'])

    def test_explicit_asymmetric_counts_do_not_turn_into_balanced_expected_roles(self):
        run=self.fixture.prepare('asymmetric',expected_ai=6,ai_allies=1)
        plan=self.fixture.metadata(run)['plan']
        self.assertTrue(plan['room_title'].endswith('!ai e5 a1'))
        self.assertEqual(self.gt.validate_metadata(run)['plan']['ai_allies'],1)

    def test_old_callers_keep_the_original_balanced_defaults(self):
        run=self.fixture.prepare('default',expected_ai=5)
        plan=self.fixture.metadata(run)['plan']
        self.assertNotIn('ai_allies',plan)
        self.assertTrue(plan['room_title'].endswith('!ai e3 a2'))

    def test_invalid_or_non_battle_team_counts_fail_before_launch(self):
        for arguments in ({'expected_ai':7,'ai_allies':-1},{'expected_ai':7,'ai_allies':7},
                          {'ai_allies':0,'pages_only':True},{'ai_allies':0,'bootstrap_only':True},
                          {'ai_allies':0,'mission_mode':5}):
            with self.subTest(arguments=arguments),self.assertRaises(self.gt.GameTestError):
                self.fixture.prepare('invalid-'+str(len(str(arguments))),**arguments)
        with self.assertRaises(self.gt.GameTestError):
            self.fixture.prepare('invalid-random-teams',random_teams=True,pages_only=True)

    def test_passive_server_observer_requires_the_requested_host_relative_roles(self):
        for host_side in (1,2):
            enemy_side=3-host_side
            slots=[{'pilot_id':31,'slot':0,'team':host_side,'is_ai':False,'unit_or_npc_id':10003}]
            slots.extend({'pilot_id':0x80000000+i,'slot':i,'team':enemy_side,'is_ai':True,'unit_or_npc_id':3604000+i} for i in range(1,8))
            event={'slots':slots,'active_slots':8}
            for side in (1,2):
                event['humans_team_'+str(side)]=int(side==host_side)
                event['ai_team_'+str(side)]=7 if side==enemy_side else 0
            self.assertEqual(len(server_report.participants(event,31,7,True,0)),8)
            with self.assertRaisesRegex(ValueError,'placement'):
                server_report.participants(event,31,7,True,3)


if __name__=='__main__':unittest.main()
