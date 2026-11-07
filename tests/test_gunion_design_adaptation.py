import copy
import hashlib
import json
import unittest

from tools.gunion import design_adaptation as design


def unit(number=1, fatal=10, **overrides):
    row = dict(UnitNumber=number, UnitGradeNumber=3, MasteredWeaponType=1,
               UnitGradeNumber2=2, HP=12000, StrikingPower=120,
               DefensivePower=120, MoveLevel=12, DashLevel=10, MotivePower=50,
               FatalAttackIncrease=fatal)
    row.update(overrides)
    return row


def weapon(number=1, speed=1.2, **overrides):
    row = dict(WeaponNo=number, AttackSpeed=speed, AttackStyle=1,
               RangeType=1, HitType=1, MaxCombo=4, BulletRemain=0, RangeSize=800)
    row.update(overrides)
    return row


def reference(units=None, weapons=None):
    tables = {}
    for key, rows in [('tbl_Units', units or [unit()]),
                      ('tbl_WeaponsRev', weapons or [weapon()]),
                      ('tbl_SkillRev', [dict(SkillID=70072)])]:
        payload = json.dumps(rows, ensure_ascii=False, sort_keys=True,
                             separators=(',', ':')).encode('utf8')
        tables[key] = dict(available=True, rows=rows,
                           rows_sha256=hashlib.sha256(payload).hexdigest())
    raw = json.dumps(dict(schema=design.REFERENCE_SCHEMA, tables=tables)).encode('utf8')
    return design.decode_reference(raw, expected_sha256=hashlib.sha256(raw).hexdigest())


class DesignAdaptationTest(unittest.TestCase):
    def test_explicit_authorization_is_required(self):
        with self.assertRaisesRegex(ValueError, 'authorization'):
            design.adapt_unit(unit(), reference=reference(), authorization='admin-only')

    def test_recovered_zero_wins_without_mutating_source(self):
        source = unit(99, 0)
        before = copy.deepcopy(source)
        result = design.adapt_unit(source, reference=reference(), authorization=design.AUTHORIZATION)
        self.assertEqual(result['values']['FatalAttackIncrease'], 0)
        self.assertEqual(result['design_values'], {})
        self.assertEqual(source, before)

    def test_exact_grade_and_weapon_class_then_nearest_stats(self):
        ref = reference([unit(1, 5, HP=1000), unit(2, 20),
                         unit(3, 30, UnitGradeNumber=2),
                         unit(4, 30, MasteredWeaponType=0)])
        source = unit(99); source.pop('FatalAttackIncrease')
        result = design.adapt_unit(source, reference=ref, neighbor_count=1,
                                  authorization=design.AUTHORIZATION)
        self.assertEqual(result['design_values'], {'FatalAttackIncrease': 20})
        self.assertEqual(result['design_provenance']['FatalAttackIncrease']['template_ids'], [2])
        self.assertFalse(result['nativeActivationAllowed'])
        self.assertFalse(result['playable'])

    def test_observed_lower_median_and_stable_ties(self):
        source = unit(99); source.pop('FatalAttackIncrease')
        a = design.adapt_unit(source, reference=reference([unit(3, 30), unit(1, 5), unit(2, 10), unit(4, 20)]),
                              authorization=design.AUTHORIZATION)
        b = design.adapt_unit(source, reference=reference([unit(4, 20), unit(2, 10), unit(1, 5), unit(3, 30)]),
                              authorization=design.AUTHORIZATION)
        self.assertEqual(a['design_values'], {'FatalAttackIncrease': 10})
        self.assertEqual(a['design_values'], b['design_values'])
        p = a['design_provenance']['FatalAttackIncrease']
        self.assertTrue(p['not_union_source'])
        self.assertEqual(p['template_ids'], [1, 2, 3, 4])

    def test_imported_rows_can_be_excluded_as_templates(self):
        source = unit(99); source.pop('FatalAttackIncrease')
        result = design.adapt_unit(source, reference=reference([unit(1, 5), unit(2, 30)]),
                                  excluded_template_ids=[1], authorization=design.AUTHORIZATION)
        self.assertEqual(result['design_values']['FatalAttackIncrease'], 30)

    def test_no_grade_match_is_not_silently_cross_rank(self):
        source = unit(99, UnitGradeNumber=5); source.pop('FatalAttackIncrease')
        with self.assertRaisesRegex(ValueError, 'no local template'):
            design.adapt_unit(source, reference=reference(), authorization=design.AUTHORIZATION)

    def test_nonfinite_source_stats_are_rejected(self):
        source = unit(99, HP=float('nan')); source.pop('FatalAttackIncrease')
        with self.assertRaisesRegex(ValueError, 'finite'):
            design.adapt_unit(source, reference=reference(), authorization=design.AUTHORIZATION)

    def test_weapon_absent_speed_has_explicit_provenance(self):
        source = weapon(99); source.pop('AttackSpeed')
        result = design.adapt_weapon(source, reference=reference(weapons=[weapon(1, 1.3), weapon(2, 1.1)]),
                                    authorization=design.AUTHORIZATION)
        self.assertEqual(result['design_values'], {'AttackSpeed': 1.1})
        self.assertEqual(source.get('AttackSpeed'), None)

    def test_weapon_finite_zero_is_preserved(self):
        result = design.adapt_weapon(weapon(99, 0), reference=reference(), authorization=design.AUTHORIZATION)
        self.assertEqual(result['values']['AttackSpeed'], 0)
        self.assertEqual(result['design_values'], {})

    def test_nonfinite_weapon_speed_requires_explicit_missing_field(self):
        with self.assertRaisesRegex(ValueError, 'finite'):
            design.adapt_weapon(weapon(99, float('nan')), reference=reference(), authorization=design.AUTHORIZATION)

    def test_snapshot_hash_and_row_hash_are_validated(self):
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            design.decode_reference(b'{}', expected_sha256='0'*64)
        raw = json.dumps({'schema': design.REFERENCE_SCHEMA, 'tables': {
            'tbl_Units': dict(available=True, rows=[unit()], rows_sha256='0'*64)}}).encode()
        with self.assertRaisesRegex(ValueError, 'row digest'):
            design.decode_reference(raw, expected_sha256=hashlib.sha256(raw).hexdigest())

    def test_duplicate_rows_and_json_keys_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'duplicate reference'):
            reference(units=[unit(), unit()])
        raw = b'{"schema":1,"schema":2}'
        with self.assertRaisesRegex(ValueError, 'duplicate JSON'):
            design.decode_reference(raw, expected_sha256=hashlib.sha256(raw).hexdigest())


if __name__ == '__main__':
    unittest.main()
