from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import game_test


class RandomRoomTitleTests(unittest.TestCase):
    def test_existing_four_digit_title_is_unchanged(self):
        plan = {'expected_ai': 1, 'ai_allies': 0}
        self.assertEqual(game_test.random_loadout_room_title('abcd', plan),
                         'abcd !ai e1 a0 !rand')

    def test_reused_observer_keeps_its_complete_fresh_nonce(self):
        plan = {'expected_ai': 1, 'ai_allies': 0}
        title = game_test.random_loadout_room_title('abcde', plan)
        self.assertEqual(title, 'abcde !ai1 !rand')
        self.assertLessEqual(len(title.encode('utf-8')), 20)
        self.assertEqual(plan, {'expected_ai': 1, 'ai_allies': 0})

    def test_an_allied_bot_or_explicit_team_rule_is_not_replaced(self):
        for plan in ({'expected_ai': 1, 'ai_allies': 1},
                     {'expected_ai': 1, 'ai_allies': 0, 'ai_teams': 'coop'}):
            title = game_test.random_loadout_room_title('abcde', plan)
            self.assertEqual(title, f"abcde {game_test.planned_ai_room_command(plan)} !rand")

    def test_four_bot_cohort_preserves_both_native_team_counts(self):
        title=game_test.random_loadout_room_title('abcde',{'expected_ai':4,'ai_allies':2})
        self.assertEqual(title,'abcde !ai2 a2 !rand')
        self.assertLessEqual(len(title.encode('utf-8')),20)


if __name__ == '__main__':
    unittest.main()
