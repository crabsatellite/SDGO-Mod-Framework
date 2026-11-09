"""Offline contract of the room-system switching GameTest (--room-switch).

One client plays an OC room, a G改 1.0 room and a G改 2.0 room back to back;
the server report checks that each battle was built with its room's system.
"""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from tools import game_test
from tools import game_test_server_report as report

UID = 10906
PARTS = [30450, 30451, 30460, 30454]
G1 = [30123, 30124, 0, 0]


class RoomSwitchPlanTests(unittest.TestCase):
    def test_titles_follow_the_sealed_base(self):
        self.assertEqual(game_test.room_switch_titles('gt1a2b !ai e2 a1'),
                         ('gt1a2b !ai e2 a1 !oc', 'gt1a2b !ai e2 a1 !g1', 'gt1a2b !ai e2 a1'))

    def test_cases_play_three_rooms_in_one_session(self):
        cases = game_test.required_flow_cases('1', play_unit_uid=UID, oc_allocate='1:1', room_switch=True)
        rounds = tuple(f'pvp.{system}.{step}' for system in ('oc', 'g1', 'g2')
                       for step in ('room', 'start', 'ai_spawn', 'system', 'return'))
        self.assertEqual(cases, game_test.FLOW_CASES[:3] + (
            'oc.allocate', 'setup.active_unit', 'g1.loadout', 'oc.create') + rounds + ('oc.panel',))
        self.assertEqual(len(set(cases)), len(cases))

    def test_scope_is_rejected_outside_the_oc_battle(self):
        for options in ({'room_switch': True},
                        {'room_switch': True, 'oc_allocate': '1:1', 'oc_room': True},
                        {'room_switch': True, 'oc_allocate': '1:1', 'g1_room': True},
                        {'room_switch': True, 'oc_allocate': '1:1', 'same_room_rematch': True},
                        {'room_switch': 1, 'oc_allocate': '1:1'}):
            with self.subTest(options=options), self.assertRaises(game_test.GameTestError):
                game_test.required_flow_cases('1', play_unit_uid=UID, **options)
        with self.assertRaises(game_test.GameTestError):
            game_test.required_flow_cases('1,3', play_unit_uid=UID, oc_allocate='1:1', room_switch=True)


class RoomSwitchClientSettingsTests(unittest.TestCase):
    def test_quiet_room_switch_plays_battles_at_the_lowest_resolution(self):
        with tempfile.TemporaryDirectory() as folder:
            config = Path(folder) / 'config.xml'
            config.write_text('<?xml version="1.0" encoding="utf-8"?><Config><Video><GameScreenRes level="13" />'
                              '</Video><Sound><BGMVolume Value="50" use="true" /><EffectVolume Value="50" use="true" />'
                              '<VoiceVolume Value="50" use="true" /></Sound></Config>', encoding='utf-8')
            game_test.configure_muted_audio(Path(folder), battle_screen_res=game_test.SEVERAL_BATTLES_SCREEN_RES)
            text = config.read_text(encoding='utf-8')
            self.assertIn('<GameScreenRes level="0" />', text)
            self.assertIn('MotionBlurLevel="0"', text)
            config.write_text(text.replace('level="0"', 'level="13"'), encoding='utf-8')
            game_test.configure_muted_audio(Path(folder))
            self.assertIn('<GameScreenRes level="13" />', config.read_text(encoding='utf-8'))


class RoomSwitchReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'events.jsonl'
        self.metadata = {'run_id': '6fd7578d-1e08-4a50-b2af-9dd44382b260',
                         'plan': {'mode_sequence': '1', 'expected_ai': 3, 'title_nonce': 'gt1234',
                                  'room_title': 'gt1234 !ai e2 a1', 'room_switch': True,
                                  'oc_allocate': '1:1', 'play_unit_uid': UID}}

    @staticmethod
    def decision(system, native, battle, oc_applied=0, g1_profile=1):
        return {'sequence': 1, 'room_key': 7, 'uid': UID, 'system': system, 'form': 0,
                'oc_applied': oc_applied, 'g1_profile': g1_profile,
                'native_skills': list(native), 'battle_skills': list(battle)}

    def decisions(self):
        return {
            'oc': [self.decision(1, PARTS, [0, 0, 0, 0], oc_applied=1),
                   dict(self.decision(1, [0, 0, 0, 0], [0, 0, 0, 0], oc_applied=1), sequence=2, form=1)],
            'g1': [self.decision(2, PARTS, G1), dict(self.decision(2, G1, G1), sequence=2, form=1)],
            'g2': [self.decision(0, PARTS, PARTS), dict(self.decision(0, PARTS, PARTS), sequence=2, form=1)],
        }

    def stream(self, titles=None, decisions=None):
        titles = titles or game_test.room_switch_titles(self.metadata['plan']['room_title'])
        decisions = decisions or self.decisions()
        common = {'run_id': self.metadata['run_id'], 'role': 'server', 'suite': 'pvp-ai'}
        events = [dict(common, event='observer_ready', host_key=report.BATTLE_HOST, pid=123,
                       normal_server_main=True, require_end=True, nonce='gt1234',
                       expected_host_pilot_id=9229, minimum_ai=3,
                       battle_ai_deaths=False, death_source='disabled')]
        host = {'slot': 0, 'pilot_id': 9229, 'unit_or_npc_id': 10003, 'team': 1, 'is_ai': False}
        bots = [{'slot': seat, 'pilot_id': 2147483650 + i, 'unit_or_npc_id': 3604001 + i, 'team': team, 'is_ai': True}
                for i, (seat, team) in enumerate(((1, 1), (6, 2), (7, 2)))]
        sequence = 0
        for index, title in enumerate(titles):
            system = ('oc', 'g1', 'g2')[index % 3]
            for step in report.STEPS:
                sequence += 1
                setup = step != 'request_accepted'
                event = dict(common, event=step, sequence=sequence, qpc=1000 + sequence, thread_id=456,
                             mode=1, room_cookie=100 + index, host_pilot_id=9229, title=title, status_code=0,
                             humans_team_1=1, humans_team_2=0, ai_team_1=1 if setup else 0,
                             ai_team_2=2 if setup else 0, active_slots=4 if setup else 1,
                             slots=copy.deepcopy([host] + (bots if setup else [])),
                             score_team_1=24, score_team_2=36)
                if step == 'battle_end':
                    event['oc_room'] = copy.deepcopy(decisions[system])
                events.append(event)
        return events

    def evaluate(self, events):
        self.path.write_text('\n'.join(json.dumps(e) for e in events) + '\n', encoding='utf-8')
        return report.evaluate(self.metadata, [self.path], 9229)

    def test_three_rooms_with_their_systems_pass(self):
        result = self.evaluate(self.stream())
        self.assertEqual(result['status'], 'pass', result['errors'])
        self.assertEqual([b['room_system'] for b in result['battles']], ['oc', 'g1', 'g2'])

    def test_a_room_built_with_the_wrong_system_fails(self):
        decisions = self.decisions()
        decisions['g1'] = [self.decision(0, PARTS, PARTS)]
        result = self.evaluate(self.stream(decisions=decisions))
        self.assertEqual(result['status'], 'fail')
        self.assertTrue(any('expected g1' in error for error in result['errors']), result['errors'])

    def test_oc_room_keeping_custom_skills_fails(self):
        decisions = self.decisions()
        decisions['oc'] = [self.decision(1, PARTS, PARTS, oc_applied=1)]
        self.assertEqual(self.evaluate(self.stream(decisions=decisions))['status'], 'fail')

    def test_g1_room_keeping_the_record_skills_fails(self):
        decisions = self.decisions()
        decisions['g1'] = [self.decision(2, PARTS, PARTS), dict(self.decision(2, PARTS, PARTS), sequence=2, form=1)]
        self.assertEqual(self.evaluate(self.stream(decisions=decisions))['status'], 'fail')

    def test_g1_room_without_its_loadout_fails(self):
        decisions = self.decisions()
        decisions['g1'] = [self.decision(2, PARTS, [0, 0, 0, 0], g1_profile=0)]
        self.assertEqual(self.evaluate(self.stream(decisions=decisions))['status'], 'fail')

    def test_g2_room_changing_skills_fails(self):
        decisions = self.decisions()
        decisions['g2'] = [self.decision(0, PARTS, [0, 0, 0, 0])]
        self.assertEqual(self.evaluate(self.stream(decisions=decisions))['status'], 'fail')

    def test_missing_decision_for_the_play_unit_fails(self):
        decisions = self.decisions()
        decisions['oc'] = []
        self.assertEqual(self.evaluate(self.stream(decisions=decisions))['status'], 'fail')

    def test_rooms_out_of_order_or_missing_fail(self):
        titles = game_test.room_switch_titles(self.metadata['plan']['room_title'])
        self.assertEqual(self.evaluate(self.stream(titles=(titles[1], titles[0], titles[2])))['status'], 'fail')
        self.assertEqual(self.evaluate(self.stream(titles=titles[:2]))['status'], 'fail')

    def test_expected_rounds_cannot_override_the_switch(self):
        self.path.write_text('\n'.join(json.dumps(e) for e in self.stream()) + '\n', encoding='utf-8')
        with self.assertRaises(ValueError):
            report.evaluate(self.metadata, [self.path], 9229, expected_rounds=1)


if __name__ == '__main__':
    unittest.main()
