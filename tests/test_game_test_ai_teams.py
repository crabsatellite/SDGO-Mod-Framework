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

    def test_named_population_policies_are_exercised_instead_of_replaced_with_exact_counts(self):
        for policy,allies in (('enemy',0),('fill',3)):
            for random in (False,True):
                run=self.fixture.prepare(policy+str(random),expected_ai=7,ai_allies=allies,
                    ai_policy=policy,random_loadout=random,random_teams=True)
                metadata=self.fixture.metadata(run)
                self.assertTrue(metadata['plan']['room_title'].endswith('!ai '+policy+(' !rand' if random else '')))
                self.assertEqual(self.gt.validate_metadata(run)['plan'],metadata['plan'])
                job=self.gt.read_ini(run/'client/sdgo/gametest/run.ini')
                self.assertEqual(job['run']['ai_policy'],policy)

    def test_automatic_policy_cannot_claim_a_different_population_or_skip_native_battle(self):
        for args in ({'expected_ai':3,'ai_policy':'enemy','ai_allies':0},
                     {'expected_ai':7,'ai_policy':'fill','ai_allies':0},
                     {'expected_ai':7,'ai_policy':'enemy','ai_allies':3},
                     {'expected_ai':7,'ai_policy':'enmey','ai_allies':0},
                     {'ai_policy':'fill','pages_only':True}):
            with self.subTest(args=args),self.assertRaises(self.gt.GameTestError):
                self.fixture.prepare('invalid-policy-'+str(len(str(args))),**args)

    def test_cooperative_grouping_is_sealed_independently_from_ai_population(self):
        for policy,allies,title in (('enemy',0,'!ai coop'),('fill',3,'!ai coop fill')):
            run=self.fixture.prepare('coop-'+policy,expected_ai=7,ai_allies=allies,
                ai_policy=policy,ai_teams='coop',random_teams=True)
            metadata=self.fixture.metadata(run)
            self.assertEqual(metadata['plan']['room_title'],metadata['plan']['title_nonce']+' '+title)
            self.assertEqual(metadata['plan']['ai_teams'],'coop')
            self.assertLessEqual(len(metadata['plan']['room_title'].encode('ascii')),20)
            self.assertEqual(self.gt.read_ini(run/'client/sdgo/gametest/run.ini')['run']['ai_teams'],'coop')
            self.assertEqual(self.gt.validate_metadata(run)['plan'],metadata['plan'])

    def test_cooperative_exact_population_keeps_counts_and_explicit_versus_keeps_native_teams(self):
        for teams,title in (('coop','!ai5 coop a1'),('versus','!ai e5 a1')):
            run=self.fixture.prepare('team-exact-'+teams,expected_ai=6,ai_allies=1,ai_teams=teams)
            metadata=self.fixture.metadata(run)
            self.assertTrue(metadata['plan']['room_title'].endswith(title))
            self.assertEqual(self.gt.validate_metadata(run)['plan'],metadata['plan'])

    def test_cooperative_random_units_keep_the_grouping_command_and_reject_long_native_titles(self):
        run=self.fixture.prepare('coop-random',expected_ai=7,ai_allies=0,
            ai_policy='enemy',ai_teams='coop',random_loadout=True,random_teams=True)
        metadata=self.fixture.metadata(run)
        self.assertTrue(metadata['plan']['room_title'].endswith('!ai coop !rand'))
        self.assertLessEqual(len(metadata['plan']['room_title'].encode('ascii')),20)
        self.assertEqual(self.gt.validate_metadata(run)['plan'],metadata['plan'])
        with self.assertRaisesRegex(self.gt.GameTestError,'twenty bytes'):
            self.fixture.prepare('coop-fill-random',expected_ai=7,ai_allies=3,
                ai_policy='fill',ai_teams='coop',random_loadout=True)

    def test_cooperative_policy_cannot_leak_into_non_battle_cases(self):
        for args in ({'ai_teams':'random'},{'ai_teams':'coop','pages_only':True},
                     {'ai_teams':'coop','mission_mode':5},{'ai_teams':'coop','bootstrap_only':True}):
            with self.subTest(args=args),self.assertRaises(self.gt.GameTestError):
                self.fixture.prepare('bad-team-'+str(len(str(args))),**args)

    def test_same_room_rematch_requires_waiting_host_and_a_second_complete_battle(self):
        run=self.fixture.prepare('same-room',expected_ai=7,ai_allies=0,ai_policy='enemy',same_room_rematch=True)
        metadata=self.fixture.metadata(run)
        self.assertIs(metadata['plan']['same_room_rematch'],True)
        expected=('framework.bootstrap','flow.login','flow.onboarding','pvp.room','pvp.start','pvp.ai_spawn',
            'pvp.waiting','pvp.rematch.room','pvp.rematch.start','pvp.rematch.ai_spawn','pvp.rematch.return')
        self.assertEqual(self.gt.required_cases_for_plan(metadata['plan']),expected)
        job=self.gt.read_ini(run/'client/sdgo/gametest/run.ini')
        self.assertEqual(job['run']['same_room_rematch'],'1')
        self.assertEqual(job['run']['required_cases'],','.join(expected))
        self.assertEqual(self.gt.validate_metadata(run)['plan'],metadata['plan'])

    def test_rematch_matrix_repeats_each_mode_before_leaving_its_room(self):
        cases=self.gt.required_flow_cases('3,9',same_room_rematch=True)
        self.assertEqual(len(cases),19)
        self.assertLess(cases.index('pvp.mode3.rematch.return'),cases.index('pvp.mode9.room'))
        self.assertIn('pvp.mode9.waiting',cases)
        self.assertIn('pvp.mode9.rematch.start',cases)

    def test_random_loadout_can_rematch_in_the_same_room(self):
        run=self.fixture.prepare('same-room-random',expected_ai=4,ai_allies=1,
                                 random_loadout=True,same_room_rematch=True)
        metadata=self.fixture.metadata(run)
        self.assertIs(metadata['plan']['same_room_rematch'],True)
        self.assertIs(metadata['plan']['random_loadout'],True)
        self.assertTrue(metadata['plan']['room_title'].endswith('!ai e3 a1 !rand'))
        self.assertIn('pvp.waiting',self.gt.required_cases_for_plan(metadata['plan']))
        self.assertIn('pvp.rematch.return',self.gt.required_cases_for_plan(metadata['plan']))
        job=self.gt.read_ini(run/'client/sdgo/gametest/run.ini')
        self.assertEqual(job['run']['same_room_rematch'],'1')
        self.assertEqual(job['run']['random_loadout'],'1')
        self.assertEqual(self.gt.validate_metadata(run)['plan'],metadata['plan'])

    def test_same_room_rematch_rejects_non_battle_scopes(self):
        for args in ({'bootstrap_only':True},{'pages_only':True},{'mission_mode':5},
                     {'battleship_only':True}):
            with self.subTest(args=args),self.assertRaises(self.gt.GameTestError):
                self.fixture.prepare('bad-rematch-'+str(len(str(args))),same_room_rematch=True,**args)
        with self.assertRaises(self.gt.GameTestError):
            self.fixture.prepare('integer-rematch',same_room_rematch=1)

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
