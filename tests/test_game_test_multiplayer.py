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


if __name__ == '__main__':
    unittest.main()
