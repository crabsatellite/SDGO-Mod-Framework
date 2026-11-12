from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

from owner.maps import boundary_format as B, server
from manager import storage
import test_map_runtime_policy as fixtures


def declaration():
    return {'mission_id': 90001, 'phase': 1, 'flags': 7, 'warning_speech_id': 123,
            'rect': [-4600, 2100, 4600, -6200], 'damage_edge_mask': 2,
            'damage_permille': 50, 'damage_cooldown_ms': 2000}


def plan():
    return {'tables': {'tbl_MissionMode': [{'GameModeID': 90001}],
                       'tbl_MissionModeStart': [{'MissionID': 90001, 'MissionPart': 1}]},
            'runtime_policy': {'immediate_transition_missions': [90001], 'boundaries': [declaration()]}}


class BoundaryFormatTests(unittest.TestCase):
    def test_fixed_native_width_and_roundtrip(self):
        rows = [declaration()]
        raw = B.encode(rows)
        self.assertEqual(len(raw), 32 + 44)
        self.assertEqual(raw[:8], b'SDGOBND1')
        self.assertEqual(struct.unpack_from('<3I', raw, 8), (1, 32, 44))
        self.assertEqual(B.decode(raw), rows)

    def test_corruption_truncation_trailing_and_duplicate_rows_are_rejected(self):
        raw = B.encode([declaration()])
        changed = bytearray(raw)
        changed[-1] ^= 1
        for bad in (bytes(changed), raw[:-1], raw + b'\0'):
            with self.subTest(length=len(bad)), self.assertRaises(ValueError): B.decode(bad)
        with self.assertRaises(ValueError): B.encode([declaration(), declaration()])

    def test_noncanonical_and_unsafe_fields_are_rejected(self):
        for field, value in [('phase', 9), ('flags', 8), ('damage_permille', True),
                             ('damage_permille', 1000), ('damage_edge_mask', 16),
                             ('damage_cooldown_ms', 249), ('rect', [0, float('nan'), 1, -1])]:
            row = declaration(); row[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError): B.encode([row])
        for flags in (0, 2, 3):
            row = declaration(); row['flags'] = flags
            with self.assertRaises(ValueError): B.encode([row])

    def test_plan_cannot_apply_contact_policy_to_foreign_mission_or_phase(self):
        for field, value in [('phase', 2), ('mission_id', 90002)]:
            value_plan = plan(); value_plan['runtime_policy']['boundaries'][0][field] = value
            with self.assertRaisesRegex(ValueError, '计划外'): server._validate_runtime_policy(value_plan)


class BoundaryDeploymentTests(unittest.TestCase):
    def fixture(self, root, version='0.2.0'):
        ini, battle = fixtures.TransitionPolicyDeploymentTests._server(root)
        mod = fixtures.TransitionPolicyDeploymentTests._install_module(battle)
        path = mod / 'mod.ini'
        path.write_bytes(storage.patch_ini(path.read_bytes(), [('mod', 'version', version)]))
        return ini, mod

    def test_old_pacing_programme_is_not_misreported_as_contact_ready(self):
        with tempfile.TemporaryDirectory() as temporary:
            ini, mod = self.fixture(Path(temporary), '0.1.0')
            before = (mod / 'config.ini').read_bytes()
            result = server.apply_runtime_policy(plan(), ini)
            self.assertEqual(result['status'], 'pending_programme_deployment')
            self.assertEqual(result['missing_hosts'], ['BattleServer1'])
            self.assertEqual((mod / 'config.ini').read_bytes(), before)
            self.assertFalse((mod / 'boundaries.bin').exists())

    def test_installs_exact_policy_and_clears_retired_damage(self):
        with tempfile.TemporaryDirectory() as temporary:
            ini, mod = self.fixture(Path(temporary))
            result = server.apply_runtime_policy(plan(), ini)
            self.assertEqual(result['status'], 'configured')
            self.assertTrue(result['boundary_contact_required'])
            self.assertFalse(result['policy_active'])
            self.assertEqual(B.decode((mod / 'boundaries.bin').read_bytes()), [declaration()])
            retired = plan(); retired['runtime_policy'].pop('boundaries')
            result = server.apply_runtime_policy(retired, ini)
            self.assertEqual(B.decode((mod / 'boundaries.bin').read_bytes()), [])
            self.assertFalse(result['boundary_contact_required'])
            self.assertIn('Unrelated=preserved', (mod / 'config.ini').read_text())

    def test_failed_enable_restores_both_policy_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            ini, mod = self.fixture(Path(temporary))
            files = ['config.ini', 'mod.ini', 'boundaries.bin']
            (mod / 'boundaries.bin').write_bytes(B.encode([]))
            before = {name: (mod / name).read_bytes() for name in files}
            atomic = storage.atomic
            failed = False
            def interrupted(path, raw):
                nonlocal failed
                if path.name == 'mod.ini' and not failed:
                    failed = True
                    raise OSError('fixture write interruption')
                return atomic(path, raw)
            with mock.patch.object(storage, 'atomic', side_effect=interrupted):
                with self.assertRaisesRegex(ValueError, '已恢复'): server.apply_runtime_policy(plan(), ini)
            self.assertTrue(failed)
            self.assertEqual({name: (mod / name).read_bytes() for name in files}, before)


if __name__ == '__main__': unittest.main()
