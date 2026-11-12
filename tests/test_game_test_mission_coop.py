from pathlib import Path
import importlib.util
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "game_test_mission_coop", ROOT / "tools/game_test_mission_coop.py"
)
coop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coop)


def gate_row(**updates):
    sample = {
        "elapsed_ms": 1000, "game_mode": 88072, "block": 7001,
        "query_valid": 1, "scenes": 4, "found": 1, "ray_hits": 1,
        "hp": 10000000, "obbs": 1, "spheres": 0, "phase_observer": 1,
        "hide_count": 0, "last_action": 0, "last_object": 0,
    }
    sample.update(updates)
    return {
        "role": "client", "event": "observation", "case": "friends.session",
        "message": coop.GATE_PREFIX + json.dumps(sample),
    }


class MissionCoopContracts(unittest.TestCase):
    def test_each_client_needs_an_ordered_valid_closed_to_hidden_transition(self):
        rows = [
            gate_row(),
            gate_row(elapsed_ms=2000, found=0, ray_hits=0, hp=0, obbs=0,
                     hide_count=1, last_action=1, last_object=7001),
        ]
        observed = coop.gate_transition(rows, 88072)
        self.assertEqual(observed["block"], 7001)
        self.assertEqual(observed["closed"]["ray_hits"], 1)
        self.assertEqual(observed["open"]["ray_hits"], 0)
        # A cinematic hides its players after changing the gate. The fixed
        # native ray still proves the gate is absent in a live world.
        human_hide = gate_row(elapsed_ms=2000, found=0, ray_hits=0, hp=0, obbs=0,
                              hide_count=3, last_action=1, last_object=2)
        self.assertIsNotNone(coop.gate_transition([rows[0], human_hide], 88072))
        unloaded = gate_row(elapsed_ms=2000, scenes=0, found=0, ray_hits=0, hp=0,
                            obbs=0, hide_count=3, last_action=1, last_object=2)
        self.assertIsNone(coop.gate_transition([rows[0], unloaded], 88072))
        # A tall shield can be the ray's sole object. Its removal is a valid
        # empty query only with an explicit native battle-state receipt.
        empty_live = gate_row(elapsed_ms=2000, scenes=0, found=0, ray_hits=0, hp=0,
                              obbs=0, app_state=22)
        self.assertIsNotNone(coop.gate_transition([rows[0], empty_live], 88072))
        empty_returned = gate_row(elapsed_ms=2000, scenes=0, found=0, ray_hits=0, hp=0,
                                  obbs=0, app_state=18)
        self.assertIsNone(coop.gate_transition([rows[0], empty_returned], 88072))
        # The peer may have no Lua-authority diagnostics. The original native
        # world query is still the direct passage/collision observation.
        peer_closed = gate_row(phase_observer=0)
        peer_open = gate_row(elapsed_ms=2000, found=0, ray_hits=0, hp=0, obbs=0,
                             phase_observer=0, hide_count=0, last_action=0, last_object=0)
        self.assertIsNotNone(coop.gate_transition([peer_closed, peer_open], 88072))

        invalid = (
            list(reversed(rows)),
            [rows[0], gate_row(elapsed_ms=2000, game_mode=88073, found=0, ray_hits=0,
                               hp=0, obbs=0, hide_count=1, last_action=1, last_object=7001)],
            [gate_row(query_valid=0), rows[1]],
            [rows[0], gate_row(elapsed_ms=2000, found=0, ray_hits=0, hp=0, obbs=0,
                               hide_count=1, last_action=0, last_object=7001, query_valid=0)],
        )
        for candidate in invalid:
            with self.subTest(candidate=candidate):
                self.assertIsNone(coop.gate_transition(candidate, 88072))

    def test_server_phase_requires_exact_two_human_mode5_roster(self):
        row = {
            "role": "server", "event": "battle_active", "host_pilot_id": 101,
            "mode": 5, "humans_team_1": 1, "humans_team_2": 1,
            "ai_team_1": 0, "ai_team_2": 0, "active_slots": 2,
            "slots": [
                {"pilot_id": 101, "is_ai": False},
                {"pilot_id": 102, "is_ai": False},
            ],
        }
        self.assertIs(coop.server_battle_active([row], 101, 102), row)
        for key, value in (("mode", 1), ("ai_team_2", 1), ("active_slots", 3)):
            changed = dict(row); changed[key] = value
            with self.subTest(key=key):
                self.assertIsNone(coop.server_battle_active([changed], 101, 102))

    def test_pair_validation_rejects_mismatched_or_unbounded_runs(self):
        class Actor:
            pass
        def actor(run_id, root, **changes):
            value = Actor()
            plan = {
                "friends_only": True, "mission_mode": 5, "mission_game_mode_id": 88072,
                "mission_ai_count": 0, "target_host": "127.222.0.1", "login_port": 5001,
                "target_server": "1", "target_channel": "1",
            }
            plan.update(changes)
            value.metadata = {"run_id": run_id, "client": {"root": root}, "plan": plan}
            return value
        host = actor("host", "host-root")
        peer = actor("peer", "peer-root")
        self.assertEqual(coop.validate_pair(host, peer), 88072)
        for changed in (
            actor("peer", "peer-root", mission_ai_count=3),
            actor("peer", "peer-root", mission_game_mode_id=88073),
            actor("peer", "peer-root", friends_only=False),
        ):
            with self.assertRaises(coop.game_test.GameTestError):
                coop.validate_pair(host, changed)


if __name__ == "__main__":
    unittest.main()
