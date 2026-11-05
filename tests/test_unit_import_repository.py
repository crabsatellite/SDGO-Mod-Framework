from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.units import repository as unit_repository
from owner.units.repository import (
    GRADE_COLUMNS,
    FUNNEL_COLUMNS,
    ITEM_COLUMNS,
    LEDGER_SCHEMA,
    LEVEL_COLUMNS,
    NAME_COLUMNS,
    Repository,
    SKILL_COLUMNS,
    SQL_SCHEMAS,
    UNIT_COLUMNS,
    UNIT_GRAPH_COLUMNS,
    WEAPON_COLUMNS,
)


def unit_row(identity=25060, *, transform=0, root=True):
    row = {}
    for name, kind, _, _ in SQL_SCHEMAS["tbl_Units"]:
        row[name] = 1.0 if kind == "real" else 1
    row.update({
        "UnitNumber": identity,
        "UnitModelName": f"Imported Unit {identity}",
        "UnitGradeNumber": 4,
        "UnitGradeNumber2": 4,
        "UnitNumber_Transform": transform,
        "HP": 14500,
        "Shield": 2000,
        "SkillNumber_1": 70001,
        "SkillNumber_2": 70002,
        "SkillNumber_3": 70003,
        "WeaponNumber_1": 61165,
        "WeaponNumber_2": 45082,
        "WeaponNumber_3": 61166,
        "WeaponNumber_FatalAttack": 43286,
        "UnitGrowIndex": 1,
        "UnitGraph": 1010001,
        "SallyReady": 1 if root else 0,
        "CollectionUnit": 1 if root else 0,
    })
    return row


def weapon_row(identity):
    row = {}
    for name, kind, _, _ in SQL_SCHEMAS["tbl_WeaponsRev"]:
        row[name] = 1.0 if kind == "real" else 1
    row.update({
        "WeaponNo": identity, "WeaponName": f"Weapon {identity}",
        "FunnelID": 0,
    })
    return row


def item_row(identity=1119000, ref=25060):
    return {
        "CatalogueID": identity,
        "Name": "ImportedUnit",
        "RefIndex": ref,
        "TimeLimit": 1,
        "Option1": 2,
        "Option2": 2,
    }


def name_row(identity=25060):
    return {"UnitNumber": identity, "NameCN": "导入机体", "NameEN": "Imported Unit"}


def plan():
    return {
        "family_key": "fixture-imported-family",
        "revision": "fixture-v1",
        "root_unit_id": 25060,
        "item_template_id": 1110008,
        "units": [unit_row()],
        "weapons": [weapon_row(61165), weapon_row(61166)],
        "item": item_row(),
        "names": [name_row()],
    }


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


def make_snapshot(root: Path, *, skills=True):
    units = [unit_row(10001)]
    units[0].update({
        "UnitModelName": "Native",
        "UnitGradeNumber": 4,
        "UnitGradeNumber2": 4,
        "SkillNumber_1": 70001,
        "SkillNumber_2": 70002,
        "SkillNumber_3": 70003,
        "WeaponNumber_1": 45082,
        "WeaponNumber_2": 43286,
        "WeaponNumber_3": 45082,
        "WeaponNumber_FatalAttack": 43286,
        "UnitGrowIndex": 1,
    })
    write_json(root / "dbo.tbl_Units.part1.json", units)
    write_json(root / "dbo.tbl_Units.part2.json", [])
    write_json(root / "dbo.tbl_WeaponsRev.part1.json", [
        weapon_row(45082), weapon_row(43286)
    ])
    for part in (2, 3, 4):
        write_json(root / f"dbo.tbl_WeaponsRev.part{part}.json", [])
    write_json(root / "dbo.tbl_ItemCategory.json", [{
        "CatalogueID": 1110008, "Name": "NativeTemplate", "RefIndex": 10001,
        "TimeLimit": 1, "Option1": 2, "Option2": 2,
    }])
    write_json(root / "dbo.tbl_UnitGrades.json", [{
        key: (
            4 if key in ("UnitGradeNumber", "UnitGradeNumber2")
            else "SR" if key == "UnitGradeName"
            else 0.0 if key in ("MixValUp", "MixValKeep")
            else 0
        )
        for key in GRADE_COLUMNS
    }])
    write_json(root / "dbo.tbl_UnitLevels.json", [{
        "UnitLevelIndex": 1, "UnitLevelNumber": 1, "UnitLevelName": "Level 1",
        "UnitGrowWeighted": 30, "LapUnitExp": 300,
    }])
    write_json(root / "dbo.tbl_UnitNames.json", [name_row(10001)])
    if skills:
        write_json(root / "dbo.tbl_SkillRev.json", [
            {key: (
                identity if key == "SkillID"
                else f"Skill {identity}" if key == "SkillName"
                else 1.0 if key in {
                    "BoostIncreaseRate", "Scale", "AutoBulletChargeRate",
                    "BulletChargeRate", "AtkSpdRate",
                }
                else 0
            ) for key in SKILL_COLUMNS}
            for identity in (70001, 70002, 70003)
        ])
    write_json(root / "dbo.tbl_Funnel.json", [{
        "FunnelID": 198001, "FunnelProp": 1, "AniTime": 1,
        "AttackDelay": 1, "AttackCount": 3, "Stealable": 1,
    }])
    write_json(root / "dbo.tbl_UnitGraph.json", [{
        "GraphNumber": 1010001, "Power": 54, "Armor": 51,
        "Mobility": 54, "Control": 44, "PosNumber": 1,
        "DefaultSkill_1": 30500, "DefaultSkill_2": 30504,
    }])
    # A deliberately malformed player export proves the loader never globs it.
    (root / "dbo.tbl_UserUnits.part1.json").write_text("{not-json", encoding="utf-8")


class SnapshotAndValidationTests(unittest.TestCase):
    def test_only_whitelisted_catalog_files_are_loaded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root)
            repository = Repository(root, None)
            self.assertEqual(set(repository.catalog), {
                "units", "weapons", "items", "names", "grades",
            "level_indices", "skill_ids", "pending_skill_ids", "funnel_ids", "pending_funnel_ids",
                "unit_graph_ids",
            })
            self.assertEqual(set(repository.catalog["units"]), {10001})
            self.assertEqual(repository.catalog["skill_ids"], {70001, 70002, 70003})
            info = repository.public_info()
            self.assertNotIn(str(root), json.dumps(info))
            self.assertFalse(info["server_configured"])

    def test_complete_plan_passes_and_dependencies_are_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root)
            repository = Repository(root, None)
            result = repository.check_plan(plan())
            self.assertTrue(result["valid"])
            self.assertEqual(result["item_catalogue_id"], 1119000)

            invalid = plan()
            invalid["units"][0]["WeaponNumber_3"] = 999999
            with self.assertRaisesRegex(ValueError, "WeaponNumber_3"):
                repository.check_plan(invalid)

    def test_unknown_missing_and_foreign_rows_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root)
            repository = Repository(root, None)
            unknown = plan()
            unknown["units"][0]["invented"] = 0
            with self.assertRaisesRegex(ValueError, "未知字段"):
                repository.check_plan(unknown)

            collision = plan()
            collision["units"][0] = unit_row(10001)
            collision["root_unit_id"] = 10001
            collision["item"]["RefIndex"] = 10001
            collision["names"] = [name_row(10001)]
            with self.assertRaisesRegex(ValueError, "占用"):
                repository.check_plan(collision)

    def test_missing_skill_export_is_pending_not_missing_skill_spam(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root, skills=False)
            repository = Repository(root, None)
            with self.assertRaisesRegex(ValueError, "pending"):
                repository.check_plan(plan())

    def test_zero_weapon_and_skill_refs_are_empty_not_missing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root, skills=False)
            candidate = plan()
            candidate["units"][0].update({
                "WeaponNumber_2": 0,
                "WeaponNumber_3": 0,
                "SkillNumber_1": 0,
                "SkillNumber_2": 0,
                "SkillNumber_3": 0,
            })
            result = Repository(root, None).check_plan(candidate)
            self.assertTrue(result["valid"])

    def test_nonzero_funnel_and_unit_graph_dependencies_are_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root)
            repository = Repository(root, None)
            candidate = plan()
            candidate["weapons"][0]["FunnelID"] = 999999
            with self.assertRaisesRegex(ValueError, "FunnelID 999999"):
                repository.check_plan(candidate)
            candidate = plan()
            candidate["units"][0]["UnitGraph"] = 999999
            with self.assertRaisesRegex(ValueError, "UnitGraph 999999"):
                repository.check_plan(candidate)

    def test_unused_optional_dependency_metadata_does_not_block(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root)
            (root / "dbo.tbl_Funnel.json").unlink()
            (root / "dbo.tbl_UnitGraph.json").unlink()
            candidate = plan()
            candidate["units"][0]["UnitGraph"] = 0
            result = Repository(root, None).check_plan(candidate)
            self.assertTrue(result["valid"])

            candidate["weapons"][0]["FunnelID"] = 198001
            with self.assertRaisesRegex(ValueError, "FunnelID.*pending"):
                Repository(root, None).check_plan(candidate)

    def test_apply_requires_server_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_snapshot(root)
            with self.assertRaisesRegex(ValueError, "离线模式"):
                Repository(root, None).apply(plan())


class Result:
    def __init__(self, rows=()):
        self.rows = list(rows)
        self.index = 0

    def fetchone(self):
        if self.index >= len(self.rows):
            return None
        row = self.rows[self.index]
        self.index += 1
        return row

    def fetchall(self):
        rows = self.rows[self.index:]
        self.index = len(self.rows)
        return rows


class FakeCursor:
    def __init__(self, connection):
        self.connection = connection
        self.result = Result()

    def fetchone(self):
        return self.result.fetchone()

    def fetchall(self):
        return self.result.fetchall()

    def execute(self, sql, *params):
        compact = " ".join(sql.split())
        self.connection.statements.append((compact, params))
        if compact.startswith("SELECT SCHEMA_ID"):
            self.result = Result([(getattr(self.connection, 'collection_schema', None),)])
        elif compact.startswith("SELECT OBJECT_ID"):
            table = str(params[0]).split(".")[-1]
            self.result = Result([(1 if table in self.connection.tables else None,)])
        elif "sys.sp_getapplock" in compact:
            self.result = Result([(0,)])
        elif "FROM sys.columns" in compact:
            table = str(params[0]).split(".")[-1]
            self.result = Result([
                (name, kind, size, nullable)
                for name, kind, size, nullable in (
                    LEDGER_SCHEMA if table == "sdgo_UnitImportDefinitions"
                    else SQL_SCHEMAS[table]
                )
            ])
        elif compact.startswith("SELECT RecordKind,RecordID,FamilyKey,InstalledHash"):
            self.result = Result([
                (kind, identity, row["FamilyKey"], row["InstalledHash"])
                for (kind, identity), row in self.connection.ledger.items()
            ])
        elif compact.startswith("SELECT OwnerKey,FamilyKey,OriginalJson,InstalledHash"):
            key = (params[0], params[1])
            row = self.connection.ledger.get(key)
            self.result = Result([] if row is None else [(
                row["OwnerKey"], row["FamilyKey"], row["OriginalJson"],
                row["InstalledHash"],
            )])
        elif compact.startswith("SELECT ") and " FROM dbo." in compact:
            before, after = compact.split(" FROM dbo.", 1)
            columns = tuple(part.strip() for part in before[7:].split(","))
            table = after.split()[0]
            rows = self.connection.tables[table]
            if " WHERE " in compact:
                key_name = compact.split(" WHERE ", 1)[1].split("=", 1)[0]
                row = rows.get(params[0])
                self.result = Result([] if row is None else [
                    tuple(row[column] for column in columns)
                ])
            else:
                self.result = Result([
                    tuple(row[column] for column in columns)
                    for row in rows.values()
                ])
        elif compact.startswith("INSERT dbo.sdgo_UnitImportDefinitions"):
            kind, identity, owner, family, record_key, revision, original, digest = params
            self.connection.ledger[(kind, identity)] = {
                "OwnerKey": owner, "FamilyKey": family, "RecordKey": record_key,
                "Revision": revision, "OriginalJson": original,
                "InstalledHash": digest,
            }
            self.result = Result()
        elif compact.startswith("UPDATE dbo.sdgo_UnitImportDefinitions"):
            record_key, revision, digest, kind, identity = params
            row = self.connection.ledger[(kind, identity)]
            row.update({
                "RecordKey": record_key, "Revision": revision,
                "InstalledHash": digest,
            })
            self.result = Result()
        elif compact.startswith("INSERT dbo."):
            table = compact.split("INSERT dbo.", 1)[1].split("(", 1)[0]
            columns_text = compact.split("(", 1)[1].split(")", 1)[0]
            columns = tuple(columns_text.split(","))
            row = dict(zip(columns, params))
            if self.connection.lossy_table == table:
                text_column = {
                    "tbl_Units": "UnitModelName",
                    "tbl_WeaponsRev": "WeaponName",
                    "tbl_ItemCategory": "Name",
                    "tbl_UnitNames": "NameCN",
                }[table]
                row[text_column] = "?"
            key = {
                "tbl_Units": "UnitNumber", "tbl_WeaponsRev": "WeaponNo",
                "tbl_ItemCategory": "CatalogueID", "tbl_UnitNames": "UnitNumber",
            }[table]
            self.connection.tables[table][row[key]] = row
            self.result = Result()
        elif compact.startswith("UPDATE dbo."):
            table = compact.split("UPDATE dbo.", 1)[1].split()[0]
            assignments = compact.split(" SET ", 1)[1].split(" WHERE ", 1)[0]
            columns = [part.split("=", 1)[0] for part in assignments.split(",")]
            identity = params[-1]
            row = self.connection.tables[table][identity]
            row.update(dict(zip(columns, params[:-1])))
            self.result = Result()
        else:
            if "CREATE TABLE dbo.tbl_UnitNames" in compact:
                self.connection.tables.setdefault("tbl_UnitNames", {})
            if "CREATE TABLE dbo.sdgo_UnitImportDefinitions" in compact:
                self.connection.tables.setdefault("sdgo_UnitImportDefinitions", {})
            self.result = Result()
        return self


class FakeConnection:
    def __init__(self):
        native = unit_row(10001)
        native.update({
            "UnitModelName": "Native",
            "SkillNumber_1": 70001, "SkillNumber_2": 70002,
            "SkillNumber_3": 70003, "WeaponNumber_1": 45082,
            "WeaponNumber_2": 43286, "WeaponNumber_3": 45082,
            "WeaponNumber_FatalAttack": 43286,
        })
        self.tables = {
            "tbl_Units": {10001: native},
            "tbl_WeaponsRev": {
                45082: weapon_row(45082), 43286: weapon_row(43286),
            },
            "tbl_ItemCategory": {1110008: {
                "CatalogueID": 1110008, "Name": "NativeTemplate",
                "RefIndex": 10001, "TimeLimit": 1, "Option1": 2, "Option2": 2,
            }},
            "tbl_UnitGrades": {1: {
                key: (
                    4 if key in ("UnitGradeNumber", "UnitGradeNumber2")
                    else "SR" if key == "UnitGradeName"
                    else 0.0 if key in ("MixValUp", "MixValKeep")
                    else 0
                ) for key in GRADE_COLUMNS
            }},
            "tbl_UnitLevels": {1: {
                "UnitLevelIndex": 1, "UnitLevelNumber": 1,
                "UnitLevelName": "Level 1", "UnitGrowWeighted": 30,
                "LapUnitExp": 300,
            }},
            "tbl_SkillRev": {
                identity: {
                    key: (
                        identity if key == "SkillID"
                        else f"Skill {identity}" if key == "SkillName"
                        else 1.0 if key in {
                            "BoostIncreaseRate", "Scale",
                            "AutoBulletChargeRate", "BulletChargeRate",
                            "AtkSpdRate",
                        }
                        else 0
                    ) for key in SKILL_COLUMNS
                } for identity in (70001, 70002, 70003)
            },
            "tbl_Funnel": {198001: {
                "FunnelID": 198001, "FunnelProp": 1, "AniTime": 1,
                "AttackDelay": 1, "AttackCount": 3, "Stealable": 1,
            }},
            "tbl_UnitGraph": {1010001: {
                "GraphNumber": 1010001, "Power": 54, "Armor": 51,
                "Mobility": 54, "Control": 44, "PosNumber": 1,
                "DefaultSkill_1": 30500, "DefaultSkill_2": 30504,
            }},
            "tbl_UnitNames": {10001: name_row(10001)},
            "sdgo_UnitImportDefinitions": {},
        }
        self.ledger = {}
        self.statements = []
        self.committed = False
        self.rolled_back = False
        self.closed = False
        self.lossy_table = None

    def cursor(self):
        return FakeCursor(self)

    def commit(self):
        self.committed = True

    def rollback(self):
        self.rolled_back = True

    def close(self):
        self.closed = True


class TransactionTests(unittest.TestCase):
    def setUp(self):
        target = patch.object(unit_repository, 'database_target_key', return_value='fixture-database')
        target.start();self.addCleanup(target.stop)

    def test_apply_uses_lock_parameterized_writes_and_no_player_tables(self):
        fake = FakeConnection()
        repository = object.__new__(Repository)
        repository.snapshot = None
        repository.server_ini = Path("private.ini")
        repository.catalog, repository._metadata = unit_repository._load_cursor_catalog(
            fake.cursor()
        )
        repository._ownership = {}
        repository._source = "server"

        with patch.object(unit_repository, "_connect", return_value=fake):
            result = repository.apply(plan())

        self.assertTrue(fake.committed)
        self.assertFalse(fake.rolled_back)
        self.assertTrue(fake.closed)
        self.assertTrue(result["cache_restart_required"])
        self.assertFalse(result["services_restarted"])
        self.assertEqual(result["inserted"]["units"], 1)
        sql = "\n".join(statement for statement, _ in fake.statements)
        self.assertIn("sys.sp_getapplock", sql)
        self.assertIn("SET XACT_ABORT ON", sql)
        self.assertIn("COMMIT TRANSACTION", sql)
        self.assertNotIn("tbl_User", sql)
        self.assertNotIn("gacha", sql.lower())
        self.assertNotIn("shop", sql.lower())
        insert = next(
            (statement, params) for statement, params in fake.statements
            if statement.startswith("INSERT dbo.tbl_Units")
        )
        self.assertEqual(insert[0].count("?"), len(UNIT_COLUMNS))
        self.assertEqual(len(insert[1]), len(UNIT_COLUMNS))
        self.assertEqual(len(fake.ledger), 5)

    def test_same_family_upgrade_requires_untampered_installed_hash(self):
        fake = FakeConnection()
        repository = object.__new__(Repository)
        repository.snapshot = None
        repository.server_ini = Path("private.ini")
        repository.catalog, repository._metadata = unit_repository._load_cursor_catalog(
            fake.cursor()
        )
        repository._ownership = {}
        repository._source = "server"
        with patch.object(unit_repository, "_connect", return_value=fake):
            repository.apply(plan())
            revision = plan()
            revision["revision"] = "5.32"
            revision["units"][0]["HP"] = 15000
            result = repository.apply(revision)
        self.assertEqual(result["updated"]["units"], 1)
        self.assertEqual(fake.tables["tbl_Units"][25060]["HP"], 15000)
        fake.tables["tbl_Units"][25060]["HP"] = 1
        with patch.object(unit_repository, "_connect", return_value=fake):
            with self.assertRaisesRegex(ValueError, "账本外"):
                repository.apply(revision)

    def test_lossy_varchar_or_unicode_readback_rolls_back(self):
        fake = FakeConnection()
        fake.lossy_table = "tbl_UnitNames"
        repository = object.__new__(Repository)
        repository.snapshot = None
        repository.server_ini = Path("private.ini")
        repository.catalog, repository._metadata = unit_repository._load_cursor_catalog(
            fake.cursor()
        )
        repository._ownership = {}
        repository._source = "server"
        with patch.object(unit_repository, "_connect", return_value=fake):
            with self.assertRaisesRegex(ValueError, "有损写入"):
                repository.apply(plan())
        self.assertTrue(fake.rolled_back)
        self.assertFalse(fake.committed)

    def test_collection_root_registration_is_inside_the_import_transaction(self):
        fake = FakeConnection()
        fake.collection_schema = 7
        repository = object.__new__(Repository)
        repository.snapshot = None
        repository.server_ini = Path('private.ini')
        repository.catalog, repository._metadata = unit_repository._load_cursor_catalog(fake.cursor())
        repository._ownership = {}
        repository._source = 'server'
        def register(connection, declaration, *, in_transaction):
            self.assertIs(connection, fake)
            self.assertTrue(in_transaction)
            self.assertFalse(fake.committed)
            self.assertEqual(declaration['family']['root_form'], 25060)
            self.assertEqual(declaration['family']['forms'], [
                {'id': 25060, 'key': '25060', 'transform_target': 0}])
            self.assertIn(25060, fake.tables['tbl_Units'])
        with patch.object(unit_repository, '_connect', return_value=fake), patch(
                'owner.collection_capsule.deployment.register_unit_family', side_effect=register) as callback:
            response = repository.apply(plan())
        callback.assert_called_once()
        self.assertTrue(response['collection_family_registered'])
        self.assertTrue(fake.committed)

    def test_rejected_collection_root_rolls_back_the_whole_import(self):
        fake = FakeConnection()
        fake.collection_schema = 7
        repository = object.__new__(Repository)
        repository.snapshot = None
        repository.server_ini = Path('private.ini')
        repository.catalog, repository._metadata = unit_repository._load_cursor_catalog(fake.cursor())
        repository._ownership = {}
        repository._source = 'server'
        with patch.object(unit_repository, '_connect', return_value=fake), patch(
                'owner.collection_capsule.deployment.register_unit_family', side_effect=ValueError('root conflict')):
            with self.assertRaisesRegex(ValueError, 'root conflict'):
                repository.apply(plan())
        self.assertTrue(fake.rolled_back)
        self.assertFalse(fake.committed)


if __name__ == "__main__":
    unittest.main()
