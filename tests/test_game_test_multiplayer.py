"""Offline launch admission only; these fixtures are not gameplay evidence."""
from pathlib import Path
import copy
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import game_test_multiplayer as multiplayer


class MultiplayerAdmissionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.runs = [self.root / name for name in ('host', 'peer1', 'peer2')]
        self.pilots = [17, 18, 19]
        plan = dict(friends_only=True, quiet=True, network_policy='direct',
                    target_host='127.212.0.1', login_port=5001,
                    mode_sequence='3', expected_ai=5, room_title='gt-test !ai5 coop')
        self.metadata = {run: {'run_id': str(index), 'plan': copy.deepcopy(plan),
                               'client': {'root': str(run / 'client')}}
                         for index, run in enumerate(self.runs)}
        self.names = ['existing-host', 'existing-peer1', 'existing-peer2']
        self.patch('validate_metadata', side_effect=lambda run: self.metadata[run.resolve()])
        self.verify = self.patch('verify_hashes')
        self.patch('read_ini', side_effect=lambda path: {'account': {'username': self.names[
            next(i for i, run in enumerate(self.runs) if path.is_relative_to(run))]}})
        self.launch = self.patch('run_prepared', side_effect=AssertionError('admission must precede native launch'))

    def patch(self, name, **kwargs):
        patcher = mock.patch.object(multiplayer.game_test, name, **kwargs)
        result = patcher.start()
        self.addCleanup(patcher.stop)
        return result

    def reject(self, message, *, peers=None, pilots=None):
        with self.assertRaisesRegex(multiplayer.game_test.GameTestError, message):
            multiplayer.campaign(self.runs[0], self.runs[1:] if peers is None else peers,
                                 self.pilots if pilots is None else pilots, [], self.root / 'result.json')
        self.launch.assert_not_called()

    def test_duplicate_client_path_cannot_start_two_actors(self):
        self.reject('distinct owned run', peers=[self.runs[0]], pilots=self.pilots[:2])

    def test_solo_runs_use_the_regular_battle_suite(self):
        self.reject('at least two real humans', peers=[], pilots=self.pilots[:1])

    def test_humans_and_ai_share_the_eight_native_slots(self):
        for metadata in self.metadata.values():
            metadata['plan']['expected_ai'] = 6
        self.reject('native room capacity')

    def test_account_identity_is_unique_case_insensitively(self):
        self.names[:2] = ['ExistingPilot', 'existingpilot']
        self.reject('distinct existing accounts')

    def test_nonquiet_or_routed_actor_is_rejected_before_launch(self):
        for key, value in (('quiet', False), ('friends_only', False), ('network_policy', 'preserve')):
            with self.subTest(key=key):
                plan = self.metadata[self.runs[1]]['plan']
                previous = plan[key]
                plan[key] = value
                self.reject('quiet direct friends-only')
                plan[key] = previous

    def test_cross_endpoint_mode_or_population_is_rejected_before_launch(self):
        for key, value in (('target_host', '127.213.0.1'), ('login_port', 5002),
                           ('mode_sequence', '6'), ('expected_ai', 4)):
            with self.subTest(key=key):
                plan = self.metadata[self.runs[2]]['plan']
                previous = plan[key]
                plan[key] = value
                self.reject('endpoint or population differs')
                plan[key] = previous

    def test_failed_sealed_hash_stops_all_native_launches(self):
        self.verify.side_effect = multiplayer.game_test.GameTestError('sealed hash mismatch')
        self.reject('sealed hash mismatch')

    def test_multi_mode_campaign_cannot_claim_one_coordinated_battle(self):
        for metadata in self.metadata.values():
            metadata['plan']['mode_sequence'] = '3,6'
        self.reject('one native battle')

    def test_round_count_is_strict_before_native_launch(self):
        for rounds in (0, 3, True, '2'):
            with self.subTest(rounds=rounds):
                with self.assertRaisesRegex(multiplayer.game_test.GameTestError, 'rounds must be 1 or 2'):
                    multiplayer.campaign(self.runs[0], self.runs[1:], self.pilots, [],
                                         self.root / 'result.json', rounds=rounds)
                self.launch.assert_not_called()

    def run_protocol_fixture(self, *, rounds=2, stale_second_start=False, retain_ghost=False):
        calls = []
        fixture = self
        cohort = {'actors': [], 'joined': 1, 'generation': 0, 'clean': False}

        class Actor:
            def __init__(self, run, completion):
                self.index = fixture.runs.index(run)
                self.metadata = fixture.metadata[run]
                self.directory = run / 'client/sdgo/gametest'
                self.directory.mkdir(parents=True)
                self.round = 0
                self.battle = False
                self.settled = False
                cohort['actors'].append(self)

            def state(self):
                generation = cohort['generation']
                start_sequence = 1 if stale_second_start and generation == 2 else generation
                clean = cohort['clean']
                return {'run': {'ready': '1'}, 'state': {'pilot': str(fixture.pilots[self.index])},
                        'command': {'result': '4', 'done': '1'},
                        'room': {'actual_room_id': '81', 'local_is_host': '1' if self.index == 0 else '0',
                                 'player_count': str(cohort['joined'])},
                        'pvp': {'channel_id': '9304', 'channel_group': '1', 'join_original': '1',
                                'room': '81', 'host': '17', 'humans': str(cohort['joined']),
                                'room_occupied': str(3 if clean else cohort['joined'] + (5 if generation else 0)),
                                'room_ai': '0' if clean else ('5' if generation else '0'),
                                'round': str(self.round), 'ready_reply': '1', 'ready_original': '1',
                                'ready_model': '1', 'battle_seen': '1' if self.battle else '0',
                                'start_sequence': str(start_sequence if self.battle else 0),
                                'bots': '5', 'local_bots': '5', 'remote_bots': '5',
                                'settled': '1' if self.settled else '0', 'end_opcode': str(0x0871),
                                'end_sequence': str(100 + generation if self.settled else 0),
                                'movement_events': '10' if self.battle else '0'}}

            def wait(self, condition, timeout=90):
                state = self.state()
                if not condition(state):
                    raise AssertionError('fixture lacks a required coordinator state')
                return state

            def command(self, operation, **fields):
                calls.append((self.index, operation, fields))
                if operation == 'room_create':
                    cohort['clean'] = False
                elif operation == 'room_join':
                    cohort['joined'] += 1
                    cohort['clean'] = cohort['joined'] == len(fixture.pilots)
                elif operation == 'room_ready':
                    self.round = cohort['generation'] + 1
                    self.battle = self.settled = False
                elif operation == 'room_start':
                    cohort['generation'] += 1
                    cohort['clean'] = False
                    for actor in cohort['actors']:
                        actor.round = cohort['generation']
                        actor.battle = True
                        actor.settled = False
                elif operation == 'battle_finish':
                    self.settled = True
                    cohort['clean'] = not retain_ghost
                state = self.state()
                if operation == 'room_leave' and (fields.get('channel_id') != 9304 or fields.get('room_id') != 81):
                    state['command']['result'] = '258'
                return state

        self.launch.side_effect = None
        self.launch.return_value = {'status': 'pass'}
        evaluated = {'rounds': None}

        def evaluate(*args, **kwargs):
            evaluated['rounds'] = kwargs.get('expected_rounds')
            return {'status': 'pass', 'errors': []}

        with mock.patch.object(multiplayer, 'FriendActor', Actor), mock.patch.object(
                multiplayer.game_test_server_report, 'evaluate', side_effect=evaluate):
            result = multiplayer.campaign(self.runs[0], self.runs[1:], self.pilots, [],
                                          self.root / 'result.json', rounds=rounds)
        return result, calls, evaluated

    def test_coordinator_carries_native_room_identity_through_guest_and_host_leave(self):
        # Protocol fixture only. The real clients must still prove every step.
        result, calls, evaluated = self.run_protocol_fixture(rounds=2)
        self.assertEqual(result['status'], 'pass')
        self.assertEqual(result['expected_rounds'], 2)
        self.assertEqual(evaluated['rounds'], 2)
        self.assertEqual([index for index, operation, _ in calls if operation == 'room_leave'], [1, 2, 0])
        starts = [i for i, (_, operation, _) in enumerate(calls) if operation == 'room_start']
        self.assertEqual(len(starts), 2)
        self.assertEqual([sum(operation == 'room_ready' for _, operation, _ in calls[:start])
                          for start in starts], [2, 4])

    def test_second_round_cannot_reuse_the_first_native_start_count(self):
        with self.assertRaisesRegex(AssertionError, 'required coordinator state'):
            self.run_protocol_fixture(rounds=2, stale_second_start=True)

    def test_coordinator_refuses_to_leave_a_returned_room_with_ghost_ai_slots(self):
        with self.assertRaisesRegex(AssertionError, 'required coordinator state'):
            self.run_protocol_fixture(rounds=1, retain_ghost=True)


if __name__ == '__main__':
    unittest.main()
