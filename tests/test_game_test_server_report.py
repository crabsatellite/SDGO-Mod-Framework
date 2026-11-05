import copy
import json
from pathlib import Path
import tempfile
import unittest

from tools import game_test_server_report as report


class ServerReportTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'events.jsonl'
        self.metadata={'run_id':'6fd7578d-1e08-4a50-b2af-9dd44382b260',
                       'plan':{'mode_sequence':'1','expected_ai':3,'title_nonce':'gt1234','room_title':'gt1234 !ai e2 a1'}}

    def stream(self,modes=(1,)):
        common={'run_id':self.metadata['run_id'],'role':'server','suite':'pvp-ai'}
        expected_ai=self.metadata['plan']['expected_ai']
        events=[dict(common,event='observer_ready',host_key=report.BATTLE_HOST,pid=123,
                     normal_server_main=True,require_end=True,nonce='gt1234',
                     expected_host_pilot_id=9229,minimum_ai=expected_ai,
                     battle_ai_deaths=False,death_source='disabled')]
        host={'slot':0,'pilot_id':9229,'unit_or_npc_id':10003,'team':1,'is_ai':False}
        placement=(tuple((seat,1) for seat in range(1,1+expected_ai//2))+
                   tuple((seat,2) for seat in range(6,6+(expected_ai+1)//2)))
        bots=[{'slot':seat,'pilot_id':2147483650+i,'unit_or_npc_id':3604001+i,'team':team,'is_ai':True}
              for i,(seat,team) in enumerate(placement)]
        sequence=0
        for mode in modes:
            for step in report.STEPS:
                sequence+=1;setup=step!='request_accepted'
                events.append(dict(common,event=step,sequence=sequence,qpc=1000+sequence,thread_id=456,
                    mode=mode,room_cookie=100+mode,host_pilot_id=9229,title=self.metadata['plan']['room_title'],status_code=0,
                    humans_team_1=1,humans_team_2=0,
                    ai_team_1=expected_ai//2 if setup else 0,
                    ai_team_2=(expected_ai+1)//2 if setup else 0,
                    active_slots=1+expected_ai if setup else 1,
                    slots=copy.deepcopy([host]+(bots if setup else [])),score_team_1=24,score_team_2=36))
        return events

    def evaluate(self,events,**kwargs):
        self.path.write_text('\n'.join(json.dumps(e) for e in events)+'\n',encoding='utf-8')
        return report.evaluate(self.metadata,[self.path],9229,**kwargs)

    def test_real_matrix_requires_every_mode_not_only_first_server_summary(self):
        self.metadata['plan']['mode_sequence']='1,3,6,7,9'
        self.assertEqual(self.evaluate(self.stream((1,3,6,7,9)))['status'],'pass')
        incomplete=self.evaluate(self.stream())
        self.assertEqual(incomplete['status'],'fail')
        self.assertEqual(len(incomplete['errors']),4)

    def test_cohort_declaration_requires_all_real_humans(self):
        events=self.stream()
        for row in events[1:]:
            row['slots'].extend([dict(slot=2,pilot_id=9230,unit_or_npc_id=10003,team=1,is_ai=False),
                                 dict(slot=3,pilot_id=9231,unit_or_npc_id=10026,team=1,is_ai=False)])
            row['humans_team_1']=3;row['active_slots']+=2
        self.path.write_text('\n'.join(json.dumps(e) for e in events)+'\n',encoding='utf-8')
        self.assertEqual(report.evaluate(self.metadata,[self.path],9229)['status'],'fail')
        result=report.evaluate(self.metadata,[self.path],9229,expected_humans=[9229,9230,9231])
        self.assertEqual(result['status'],'pass')
        self.assertEqual(result['expected_human_pilot_ids'],[9229,9230,9231])

    def test_cohort_cannot_override_a_sealed_roster(self):
        self.metadata['plan']['human_pilot_ids']=[9229]
        with self.assertRaisesRegex(ValueError,'sealed plan'):
            report.evaluate(self.metadata,[],9229,expected_humans=[9229,9230])

    def test_same_run_role_nonce_and_native_main_are_bound(self):
        for field,value in (('run_id','stale-run'),('role','client'),('nonce','wrong'),
                            ('normal_server_main',False),('require_end',False),
                            ('minimum_ai',1),('pid',False)):
            events=self.stream();events[0][field]=value
            self.assertEqual(self.evaluate(events)['status'],'fail',field)

    def test_missing_end_and_out_of_order_active_cannot_pass(self):
        events=self.stream()
        self.assertEqual(self.evaluate(events[:-1])['status'],'fail')
        events[2],events[3]=events[3],events[2]
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_rematch_requires_two_complete_native_battles_instead_of_reusing_the_first_result(self):
        self.metadata['plan']['same_room_rematch']=True
        self.assertEqual(self.evaluate(self.stream())['status'],'fail')
        complete=self.stream((1,1))
        self.assertEqual(self.evaluate(complete)['status'],'pass')
        self.assertEqual(self.evaluate(complete[:-1])['status'],'fail')

    def test_duplicate_battles_and_duplicate_native_sequences_fail(self):
        self.assertEqual(self.evaluate(self.stream((1,1)))['status'],'fail')
        events=self.stream();events[3]['sequence']=events[2]['sequence']
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_coordinated_rematch_requires_two_fresh_complete_lifecycles(self):
        self.assertEqual(self.evaluate(self.stream(),expected_rounds=2)['status'],'fail')
        self.assertEqual(self.evaluate(self.stream((1,1)),expected_rounds=2)['status'],'pass')
        self.assertNotIn('same_room_rematch',self.metadata['plan'])
        for value in (True,0,3,'2'):
            with self.subTest(value=value),self.assertRaises(ValueError):
                self.evaluate(self.stream(),expected_rounds=value)
        self.metadata['plan']['same_room_rematch']=True
        with self.assertRaises(ValueError):self.evaluate(self.stream(),expected_rounds=1)

    def test_participant_identity_and_ally_enemy_placement_are_verified(self):
        for mutate in (
            lambda e:e[2]['slots'][1].update(pilot_id=9229),
            lambda e:e[2]['slots'][1].update(team=2),
            lambda e:e[3]['slots'][1].update(pilot_id=999999),
            lambda e:e[2].update(ai_team_2=1),
            lambda e:e[2].update(active_slots=True),
        ):
            events=self.stream();mutate(events)
            self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_allied_layout_requires_five_allies_one_enemy_and_keeps_team_binding(self):
        self.metadata['plan'].update(expected_ai=6,ai_layout='allied',ai_difficulty='Hard',
                                     room_title='gt1234 !ai1 a6 hard')
        events=self.stream()
        self.assertEqual(self.evaluate(events)['status'],'fail','balanced six is not the sealed allied layout')
        for event in events[2:]:
            event['slots'][4]['team']=1;event['slots'][5]['team']=1
            event['ai_team_1']=5;event['ai_team_2']=1
        self.assertEqual(self.evaluate(events)['status'],'pass')
        for field,value in (('ai_layout',None),('ai_layout',True),('ai_layout','balanced'),
                            ('ai_difficulty','Medium'),('ai_pool_level',4)):
            original=copy.deepcopy(self.metadata)
            self.metadata['plan'][field]=value
            with self.subTest(field=field,value=value):self.assertEqual(self.evaluate(events)['status'],'fail')
            self.metadata=original
        for event in events[1:]:
            for slot in event['slots']:slot['team']=3-slot['team']
            event['humans_team_1'],event['humans_team_2']=event['humans_team_2'],event['humans_team_1']
            event['ai_team_1'],event['ai_team_2']=event['ai_team_2'],event['ai_team_1']
        self.assertEqual(self.evaluate(events)['status'],'pass','host team may be either native faction')

    def test_title_host_and_failure_event_are_not_silently_ignored(self):
        for field,value in (('title','other room'),('host_pilot_id',6228),
                            ('status_code',False),('event','setup_failed')):
            events=self.stream();events[2][field]=value
            self.assertEqual(self.evaluate(events)['status'],'fail',field)

    def test_malformed_records_and_missing_files_fail_closed(self):
        self.assertEqual(self.evaluate([[]])['status'],'fail')
        self.path.write_text('invalid json',encoding='utf-8')
        self.assertEqual(report.evaluate(self.metadata,[self.path],9229)['status'],'fail')
        self.assertEqual(report.evaluate(self.metadata,[self.path.with_name('absent')],9229)['status'],'fail')

    def test_same_input_path_is_not_double_counted(self):
        self.evaluate(self.stream())
        result=report.evaluate(self.metadata,[self.path,self.path],9229)
        self.assertEqual(result['status'],'pass')
        self.assertEqual(len(result['sources']),1)

    def cooperative_stream(self):
        self.metadata['plan'].update(expected_ai=5,ai_allies=0,ai_teams='coop',
            human_pilot_ids=[9229,9230,9231],room_title='gt1234 !ai coop')
        events=self.stream();events[0]['minimum_ai']=5
        humans=[{'slot':0,'pilot_id':9230,'unit_or_npc_id':10003,'team':2,'is_ai':False},
                {'slot':5,'pilot_id':9231,'unit_or_npc_id':10026,'team':2,'is_ai':False},
                {'slot':11,'pilot_id':9229,'unit_or_npc_id':24788,'team':2,'is_ai':False}]
        bots=[{'slot':seat,'pilot_id':0x80000000+i+1,'unit_or_npc_id':3604001+i,'team':1,'is_ai':True}
              for i,seat in enumerate((1,2,3,6,7))]
        for event in events[1:]:
            setup=event['event']!='request_accepted'
            event.update(humans_team_1=0,humans_team_2=3,ai_team_1=5 if setup else 0,ai_team_2=0,
                active_slots=8 if setup else 3,slots=copy.deepcopy(humans+(bots if setup else [])))
        return events

    def test_three_humans_require_the_declared_existing_roster_through_the_whole_battle(self):
        events=self.cooperative_stream()
        self.assertEqual(self.evaluate(events)['status'],'pass')
        for index in range(1,5):
            wrong=copy.deepcopy(events);wrong[index]['slots'][0]['pilot_id']=9999
            self.assertEqual(self.evaluate(wrong)['status'],'fail')
        del self.metadata['plan']['human_pilot_ids']
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_cooperative_roster_cannot_pass_with_one_human_on_the_other_team(self):
        events=self.cooperative_stream()
        for event in events[1:]:
            event['slots'][0]['team']=1
            event.update(humans_team_1=1,humans_team_2=2)
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_ai_placement_is_relative_to_the_host_even_when_another_human_is_first(self):
        events=self.cooperative_stream()
        self.metadata['plan'].update(ai_teams='versus',ai_allies=2)
        for event in events[1:]:
            event['slots'][0]['team']=1
            event.update(humans_team_1=1,humans_team_2=2)
            if event['event']!='request_accepted':
                for bot in event['slots'][3:5]:bot['team']=2
                event.update(ai_team_1=3,ai_team_2=2)
        self.assertEqual(self.evaluate(events)['status'],'pass')

    def test_declared_human_roster_rejects_missing_host_duplicates_and_ai_identities(self):
        for roster in ([],[9230],[9229,9229],[9229,True],[9229,0x80000001],list(range(9229,9238))):
            with self.subTest(roster=roster),self.assertRaises(ValueError):
                report.human_roster(9229,roster)
        events=self.cooperative_stream()
        with self.assertRaisesRegex(ValueError,'capacity'):
            report.participants(events[2],9229,6,True,0,[9229,9230,9231],True)

    def test_native_ai_flag_must_match_the_native_pilot_id_bit(self):
        events=self.stream();events[2]['slots'][1]['pilot_id']=12345
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def test_mid_battle_team_swaps_cannot_pass_even_when_all_relative_counts_still_match(self):
        events=self.cooperative_stream();end=events[-1]
        for slot in end['slots']:slot['team']=3-slot['team']
        end.update(humans_team_1=3,humans_team_2=0,ai_team_1=0,ai_team_2=5)
        self.assertEqual(self.evaluate(events)['status'],'fail')

    def reused_stream(self):
        old=self.stream()
        session_id=self.metadata['run_id']
        self.metadata['run_id']='93836df5-d230-4aa0-9e5c-ea8a9e8fdd59'
        self.metadata['server_session']={'run_id':session_id,'nonce':'gt1234','pids':[123]}
        self.metadata['plan']['title_nonce']='gtabcd'
        expected_ai=self.metadata['plan']['expected_ai']
        self.metadata['plan']['room_title']=(
            f'gt1234abcd !ai e{(expected_ai+1)//2} a{expected_ai//2}')
        fresh=self.stream()[1:]
        for event in fresh:
            event['run_id']=session_id
            event['sequence']+=4;event['qpc']+=4
        return old,fresh

    def test_reused_process_requires_a_fresh_complete_room(self):
        old,fresh=self.reused_stream()
        self.assertEqual(self.evaluate(old+fresh)['status'],'pass')
        self.assertEqual(self.evaluate(old)['status'],'fail')
        self.assertEqual(self.evaluate(old+fresh[:-1])['status'],'fail')

    def test_reused_process_keeps_session_pid_and_native_run_identity(self):
        old,fresh=self.reused_stream()
        wrong_pid=copy.deepcopy(old+fresh);wrong_pid[0]['pid']=124
        self.assertEqual(self.evaluate(wrong_pid)['status'],'fail')
        wrong_run=copy.deepcopy(old+fresh);wrong_run[-1]['run_id']=self.metadata['run_id']
        self.assertEqual(self.evaluate(wrong_run)['status'],'fail')

    def test_reused_session_minimum_is_a_floor_but_actual_roster_stays_exact(self):
        self.metadata['plan']['expected_ai']=6
        old,fresh=self.reused_stream()
        old[0]['minimum_ai']=1
        valid=old+fresh
        self.assertEqual(self.evaluate(valid)['status'],'pass')

        wrong_roster=copy.deepcopy(valid)
        for event in wrong_roster:
            if (event.get('title')==self.metadata['plan']['room_title'] and
                    event.get('event') in report.STEPS[1:]):
                removed=event['slots'].pop()
                event['active_slots']-=1
                event[f'ai_team_{removed["team"]}']-=1
        self.assertEqual(self.evaluate(wrong_roster)['status'],'fail')

        excessive_minimum=copy.deepcopy(valid)
        excessive_minimum[0]['minimum_ai']=7
        self.assertEqual(self.evaluate(excessive_minimum)['status'],'fail')

    def test_fresh_damage_diagnostic_is_bound_and_never_counts_as_lifecycle(self):
        events=self.stream()
        events[0].update(battle_ai_deaths=True,death_source='direct_native_hook')
        diagnostic={'run_id':self.metadata['run_id'],'role':'server','suite':'pvp-ai',
                    'event':report.DAMAGE_EVENT,'source':'direct_native_hook',
                    'sequence':1,'qpc':1001,'thread_id':789,'room_cookie':101,
                    'host_pilot_id':9229,'mode':1}
        events.insert(2,diagnostic)
        self.assertEqual(self.evaluate(events)['status'],'pass')
        self.assertEqual(self.evaluate(events[:-1])['status'],'fail')

        wrong_source=copy.deepcopy(events)
        wrong_source[2]['source']='battle_rewards_bridge'
        self.assertEqual(self.evaluate(wrong_source)['status'],'fail')


if __name__=='__main__':unittest.main()
