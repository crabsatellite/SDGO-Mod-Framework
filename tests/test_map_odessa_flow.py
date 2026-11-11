import json
import unittest
from pathlib import Path


MISSION = (
    Path(__file__).resolve().parents[1]
    / "examples"
    / "maps"
    / "twin-valley-fortress"
    / "missions"
    / "odessa.json"
)


class OdessaFlowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.spec = json.loads(MISSION.read_text(encoding="utf-8"))
        cls.phases = cls.spec["phases"]

    def test_named_base_target_precedes_remaining_force_cleanup(self):
        self.assertEqual(8, len(self.phases))
        self.assertEqual(
            ["outer", "mopup", "missiles", "guntank", "tristars", "core", "core_mopup", "nuke"],
            [phase["key"] for phase in self.phases],
        )

        target, cleanup = self.phases[5:7]
        self.assertEqual("north_valley", target["enter"]["sector"])
        self.assertEqual(["l11", "l12", "l12a"], target["enter"]["lines"])
        self.assertEqual(
            {"kill": [{"npc": "commander", "count": 2}]},
            target["goal"],
        )
        self.assertEqual({"kill_any": 12}, cleanup["goal"])
        self.assertNotIn("enter", cleanup)
        self.assertIn("commander", {spawn["npc"] for spawn in target["spawns"]})
        self.assertNotIn("commander", {spawn["npc"] for spawn in cleanup["spawns"]})
        self.assertTrue(
            {"dom", "zaku_cannon", "dom_desert", "zaku_bazooka"}
            <= {spawn["npc"] for spawn in cleanup["spawns"]}
        )

    def test_each_phase_has_an_immediate_first_active_wave(self):
        for phase in self.phases:
            by_side = {}
            for spawn in phase.get("spawns", []):
                by_side.setdefault(spawn.get("side", "enemy"), []).append(spawn)
            with self.subTest(phase=phase["key"], side="enemy"):
                self.assertEqual(0, min(spawn.get("at", 0) for spawn in by_side["enemy"]))
            for side, spawns in by_side.items():
                with self.subTest(phase=phase["key"], side=side):
                    self.assertEqual(0, min(spawn.get("at", 0) for spawn in spawns))

    def test_only_authored_cutscene_sections_declare_scene_waits(self):
        declared = {
            phase["key"]: phase.get("enter", {}).get("cutscene")
            for phase in self.phases
            if phase.get("enter", {}).get("cutscene")
        }
        self.assertEqual({"tristars": "tristars", "nuke": "nuke"}, declared)


if __name__ == "__main__":
    unittest.main()
