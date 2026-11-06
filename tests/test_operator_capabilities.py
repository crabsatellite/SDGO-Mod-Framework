import copy
import unittest
from pathlib import Path
from unittest.mock import patch

from manager.engines.operators import capabilities, model, server


def bonus(gp=0, unit=0, energy=0):
    return {
        "gp_percent": gp,
        "unit_exp_percent": unit,
        "energy_save_percent": energy,
    }


def abilities(
    *,
    mode="custom",
    general=None,
    victory=None,
    defeat=None,
    timed=None,
    pilot=0,
    mission=None,
    extensions=None,
):
    return {
        "schema_version": 1,
        "mode": mode,
        "general": general or bonus(),
        "victory": victory or bonus(),
        "defeat": defeat or bonus(),
        "timed": timed or [],
        "pilot_exp_percent": pilot,
        "mission_parts": mission,
        "extensions": extensions or {},
    }


def skill(identity, gp, unit, energy, kind=1):
    return {
        "SkillNo": identity,
        "SkillType": kind,
        "PointAdd": gp,
        "UnitExpAdd": unit,
        "EnergySave": energy,
    }


def timed(identity, add_on, operator_skill, start=2000, end=2200):
    return {
        "TimeID": identity,
        "AddOnID": add_on,
        "OperatorSkill": operator_skill,
        "StartTime": start,
        "CloseTime": end,
    }


def operator(ability):
    return {
        "id": 60003,
        "managed": True,
        "enabled": True,
        "abilities": ability,
    }


def catalog():
    return {
        "schema_version": 1,
        "native_operator_ids": [60003],
        "client_index_sha256": "fixture",
        "uploaded_assets": {},
        "native_server_data": {},
        "native_skills": [],
        "native_operator_times": [],
        "items": [
            {"id": 1210002, "name": "operator", "ref_index": 60003, "time_limit": 0},
        ],
        "operators": [{
            "id": 60003,
            "category_id": 1210002,
            "template_id": 60003,
            "name": "副官",
            "managed": False,
            "enabled": True,
            "delivery": "product",
            "avatar": {"kind": "native", "operator_id": 60003},
            "drops": [],
        }],
    }


class AbilitySchemaTests(unittest.TestCase):
    def test_missing_abilities_remains_backward_compatible(self):
        value = catalog()
        model.validate(value)
        self.assertNotIn("abilities", value["operators"][0])

    def test_percentages_are_strict_integers_and_reject_bool(self):
        for invalid in (-1, 101, 1.5, "20", True):
            value = abilities(general=bonus(gp=invalid))
            with self.assertRaises(ValueError):
                capabilities.normalize(value)

    def test_timed_slots_are_strict_ordered_and_do_not_cross_midnight(self):
        valid = abilities(timed=[
            {"start": "08:00", "end": "10:00", **bonus(gp=10)},
            {"start": "08:30", "end": "09:00", **bonus(unit=20)},
        ])
        self.assertEqual(len(capabilities.normalize(valid)["timed"]), 2)
        for invalid in [
            abilities(timed=[{"start": "8:00", "end": "10:00", **bonus()}]),
            abilities(timed=[{"start": "23:00", "end": "01:00", **bonus()}]),
            abilities(timed=[
                {"start": "09:00", "end": "10:00", **bonus()},
                {"start": "08:00", "end": "09:00", **bonus()},
            ]),
            abilities(timed=[
                {"start": f"0{i}:00", "end": f"0{i}:30", **bonus()}
                for i in range(5)
            ]),
        ]:
            with self.assertRaises(ValueError):
                capabilities.normalize(invalid)

    def test_inherit_cannot_hide_dormant_custom_values(self):
        self.assertEqual(
            capabilities.normalize(abilities(mode="inherit")),
            abilities(mode="inherit"),
        )
        with self.assertRaises(ValueError):
            capabilities.normalize(abilities(mode="inherit", mission=True))

    def test_pilot_exp_is_first_class_but_unknown_extensions_fail_closed(self):
        value = capabilities.normalize(abilities(pilot=25))
        self.assertEqual(capabilities.required_native_capabilities(value), {"pilot_exp_percent"})
        self.assertEqual(capabilities.required_extensions(value), set())
        with self.assertRaises(ValueError):
            capabilities.normalize(abilities(extensions={"future.effect": {"value": 1}}))
        known = capabilities.normalize(
            abilities(extensions={"future.effect": {"value": 1}}),
            {"future.effect": 1},
        )
        self.assertEqual(capabilities.required_extensions(known), {"future.effect"})

    def test_model_update_is_atomic_and_exports_runtime_requirements(self):
        original = catalog()
        configured = model.update_operator(original, 60003, {
            "abilities": abilities(pilot=30),
        })
        self.assertNotIn("abilities", original["operators"][0])
        self.assertEqual(
            model.registrations(configured)["required_native_capabilities"],
            ["pilot_exp_percent"],
        )
        bad = abilities()
        bad["general"]["gp_percent"] = True
        with self.assertRaises(ValueError):
            model.update_operator(original, 60003, {"abilities": bad})
        self.assertEqual(original, catalog())


class NativeCompileTests(unittest.TestCase):
    def test_exact_existing_type1_skills_are_reused(self):
        native = [
            skill(1, 20, 0, 0),
            skill(3, 0, 20, 0),
            skill(12, 0, 0, 0, kind=2),
        ]
        value = capabilities.normalize(abilities(
            general=bonus(gp=20),
            victory=bonus(unit=20),
            defeat=bonus(gp=20),
        ))
        plan = capabilities.compile_plan([operator(value)], native, [])
        self.assertEqual(plan["owned_skills"], [])
        self.assertEqual(plan["operators"][60003]["BattleResult"], 1)
        self.assertEqual(plan["operators"][60003]["TeamWin"], 3)
        self.assertEqual(plan["operators"][60003]["TeamLose"], 1)

    def test_type2_zero_row_is_not_reused_and_new_ids_are_gated(self):
        value = capabilities.normalize(abilities())
        with self.assertRaisesRegex(ValueError, "SkillNo"):
            capabilities.compile_plan(
                [operator(value)], [skill(12, 0, 0, 0, kind=2)], []
            )

    def test_new_skills_are_low_deterministic_and_never_touch_1_to_27(self):
        value = capabilities.normalize(abilities(
            general=bonus(gp=5),
            victory=bonus(unit=7),
            defeat=bonus(energy=9),
        ))
        first = capabilities.compile_plan(
            [operator(value)], [skill(1, 20, 0, 0)], [],
            allow_new_skill_ids=True,
        )
        second = capabilities.compile_plan(
            [operator(value)], [skill(1, 20, 0, 0)], [],
            allow_new_skill_ids=True,
        )
        self.assertEqual(first, second)
        self.assertEqual(
            [row["SkillNo"] for row in first["owned_skills"]],
            [28, 29, 30],
        )
        self.assertTrue(all(row["SkillNo"] > 27 for row in first["owned_skills"]))

    def test_timed_first_slot_order_and_exact_native_row_reuse(self):
        native = [
            skill(1, 20, 0, 0),
            skill(3, 0, 20, 0),
            skill(17, 60, 20, 0),
        ]
        native_times = [timed(66001, 1, 17)]
        value = capabilities.normalize(abilities(
            general=bonus(gp=20),
            victory=bonus(unit=20),
            defeat=bonus(gp=20),
            timed=[{"start": "20:00", "end": "22:00", **bonus(gp=60, unit=20)}],
        ))
        plan = capabilities.compile_plan([operator(value)], native, native_times)
        compiled = plan["operators"][60003]
        self.assertEqual(
            [compiled[f"AddOn{i}"] for i in range(1, 5)],
            [1, 0, 0, 0],
        )
        self.assertEqual(plan["owned_times"], [])

    def test_new_time_uses_low_id_free_in_both_native_namespaces(self):
        native = [skill(1, 20, 0, 0)]
        native_times = [timed(66001, 1, 1), timed(66002, 2, 1)]
        value = capabilities.normalize(abilities(
            general=bonus(gp=20),
            victory=bonus(gp=20),
            defeat=bonus(gp=20),
            timed=[{"start": "08:00", "end": "09:00", **bonus(gp=20)}],
        ))
        with self.assertRaisesRegex(ValueError, "TimeID"):
            capabilities.compile_plan([operator(value)], native, native_times)
        plan = capabilities.compile_plan(
            [operator(value)], native, native_times,
            allow_new_time_ids=True,
        )
        self.assertEqual(plan["owned_times"][0]["TimeID"], 3)
        self.assertEqual(plan["owned_times"][0]["AddOnID"], 3)
        self.assertEqual(plan["operators"][60003]["AddOn1"], 3)

    def test_owned_row_collision_fails_before_a_plan_is_returned(self):
        value = capabilities.normalize(abilities(
            general=bonus(gp=5),
            victory=bonus(gp=5),
            defeat=bonus(gp=5),
        ))
        owned = [skill(28, 5, 0, 0)]
        changed = [skill(28, 6, 0, 0)]
        with self.assertRaisesRegex(ValueError, "其他内容改变"):
            capabilities.compile_plan(
                [operator(value)], changed, [], owned_skills=owned,
                allow_new_skill_ids=True,
            )

    def test_exact_owned_id_is_reserved_before_allocating_changed_tuples(self):
        value = capabilities.normalize(abilities(
            general=bonus(unit=5),
            victory=bonus(gp=10),
            defeat=bonus(gp=10),
        ))
        owned = [skill(28, 10, 0, 0)]
        plan = capabilities.compile_plan(
            [operator(value)], owned, [], owned_skills=owned,
            allow_new_skill_ids=True,
        )
        rows = {row["SkillNo"]: row for row in plan["owned_skills"]}
        self.assertEqual((rows[28]["PointAdd"], rows[28]["UnitExpAdd"]), (10, 0))
        self.assertEqual((rows[29]["PointAdd"], rows[29]["UnitExpAdd"]), (0, 5))


class ServerSchemaTests(unittest.TestCase):
    def test_purchased_disabled_new_operator_uses_template_baseline_idempotently(self):
        template = dict(zip(server.OPERATOR_COLUMNS, [
            60003, "template", 1, 3, 4, 77, 24, 1, 2, 0, 0, 1, 9,
        ]))
        current = dict(template)
        current.update(
            OperatorID=61000, OperatorName="SDGOop61000",
            TeamWin=28, TeamLose=29, BattleResult=30,
            AddOn1=4, AddOn2=5, GParts=1,
        )
        planned = server._retained_operator_plan(
            61000, True, template, current,
        )
        self.assertEqual(planned["OperatorID"], 61000)
        self.assertEqual(planned["OperatorName"], "SDGOop61000")
        self.assertEqual(planned["DropType"], 0)
        self.assertEqual(
            tuple(planned[name] for name in (
                "TeamWin", "TeamLose", "BattleResult", "ChangeSp",
                "ServiceType", "AddOn1", "AddOn2", "AddOn3", "AddOn4",
                "GParts",
            )),
            tuple(template[name] for name in (
                "TeamWin", "TeamLose", "BattleResult", "ChangeSp",
                "ServiceType", "AddOn1", "AddOn2", "AddOn3", "AddOn4",
                "GParts",
            )),
        )
        self.assertEqual(
            server._retained_operator_plan(61000, True, template, planned),
            planned,
        )
        self.assertIsNone(
            server._retained_operator_plan(61000, False, template, current)
        )

    def test_template_sync_preserves_opaque_fields_and_target_name(self):
        template = dict(zip(server.OPERATOR_COLUMNS, [
            60003, "template", 1, 3, 4, 77, 24, 1, 2, 0, 0, 0, 9,
        ]))
        target = dict(template)
        target.update(OperatorID=60004, OperatorName="existing target")
        managed = [{
            "id": 60004, "template_id": 60003, "managed": True, "enabled": True,
        }]
        compiled = {
            "operators": {60004: {
                "TeamWin": 11, "TeamLose": 10, "BattleResult": 9,
                "AddOn1": 3, "AddOn2": 4, "AddOn3": 0, "AddOn4": 0,
                "mission_parts": False,
            }},
        }
        rows = {60003: template, 60004: target}
        with patch.object(server, "_operator_row", side_effect=lambda cur, identity: copy.deepcopy(rows.get(identity))):
            planned = server._operator_data_plan(
                object(), managed, {60004: (True, 0)}, compiled,
            )[60004]["desired"]
        self.assertEqual(planned["OperatorName"], "existing target")
        self.assertEqual((planned["ChangeSp"], planned["ServiceType"]), (77, 24))
        self.assertEqual(
            (planned["TeamWin"], planned["TeamLose"], planned["BattleResult"]),
            (11, 10, 9),
        )
        self.assertEqual(
            [planned[f"AddOn{i}"] for i in range(1, 5)],
            [3, 4, 0, 0],
        )
        self.assertEqual(planned["GParts"], 0)

    def test_full_native_operator_row_is_backed_up(self):
        self.assertEqual(server.OPERATOR_COLUMNS, [
            "OperatorID", "OperatorName", "TeamWin", "TeamLose", "BattleResult",
            "ChangeSp", "ServiceType", "AddOn1", "AddOn2", "AddOn3", "AddOn4",
            "DropType", "GParts",
        ])
        schema = (
            Path(server.MODULE_DIR) / "server" / "schema.sql"
        ).read_text(encoding="utf-8")
        for field in server.BACKUP_COLUMNS:
            self.assertIn(field, schema)
        self.assertIn("BaselineTemplateID", schema)
        self.assertIn("custom_skill_ids", schema)
        self.assertIn("custom_time_ids", schema)
        self.assertIn("pilot_exp_percent", schema)


if __name__ == "__main__":
    unittest.main()
