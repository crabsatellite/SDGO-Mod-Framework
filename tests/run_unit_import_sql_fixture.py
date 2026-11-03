"""Opt-in destructive-to-scratch SQL fixture for the unit import repository.

The script creates one uniquely named database, installs only fixed definition
tables, exercises Repository.apply against it, and drops only the database that
it marked as its own.  It never copies or queries player tables.
"""
from __future__ import annotations

import argparse
import copy
import datetime as dt
import json
from pathlib import Path
import re
import struct
import sys
import uuid
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.units import repository as repository_module
from owner.units.repository import (
    GRADE_COLUMNS,
    ITEM_COLUMNS,
    LEVEL_COLUMNS,
    OWNER_KEY,
    Repository,
    SKILL_COLUMNS,
    SQL_SCHEMAS,
    UNIT_COLUMNS,
    WEAPON_COLUMNS,
    _read_ini,
    _readback_matches,
)
from tests.test_unit_import_repository import (
    item_row,
    name_row,
    plan as base_plan,
    unit_row,
    weapon_row,
)


DATABASE_NAME = re.compile(r"SDGO_UnitImportTest_[0-9a-f]{12}\Z")
COLLATION_NAME = re.compile(r"[A-Za-z0-9_]+\Z")
MARKER_NAME = "sdgo_unit_import_fixture_owner"


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def sanitized_error(error: BaseException) -> str:
    args = getattr(error, "args", ())
    if error.__class__.__module__ == "pyodbc":
        state = args[0] if args else "unknown"
        return f"{error.__class__.__name__}: SQLSTATE={state}"
    text = str(error)
    if len(text) > 500:
        text = text[:500] + "..."
    return f"{error.__class__.__name__}: {text}"


class SqlFactory:
    def __init__(self, server_ini: Path):
        import pyodbc

        self.pyodbc = pyodbc
        parser = _read_ini(server_ini)
        section = parser["DB Game"]
        required = ("DBServerName", "DBName", "ID", "PWD")
        if any(not section.get(key) for key in required):
            raise ValueError("服务端 [DB Game] 配置不完整")
        drivers = [name for name in pyodbc.drivers() if "SQL Server" in name]
        if not drivers:
            raise ValueError("本机缺少 SQL Server ODBC 驱动")
        self.driver = drivers[-1]
        self.server = section["DBServerName"]
        self.source_database = section["DBName"]
        self.user = section["ID"]
        self.password = section["PWD"]

    @staticmethod
    def _escape(value: str) -> str:
        return "{" + str(value).replace("}", "}}") + "}"

    def connect(self, database: str, *, autocommit: bool = False):
        connection_string = (
            f"DRIVER={self._escape(self.driver)};"
            f"SERVER={self._escape(self.server)};"
            f"DATABASE={self._escape(database)};"
            f"UID={self._escape(self.user)};"
            f"PWD={self._escape(self.password)};"
        )
        connection = self.pyodbc.connect(
            connection_string, timeout=10, autocommit=autocommit
        )
        connection.timeout = 30
        return connection


def sql_type(kind: str, size: int) -> str:
    if kind in ("tinyint", "smallint", "int", "real"):
        return kind
    if kind == "varchar":
        return f"varchar({size})"
    if kind == "nvarchar":
        if size == -1:
            return "nvarchar(max)"
        return f"nvarchar({size // 2})"
    raise AssertionError(f"unsupported fixture SQL type {kind}")


def create_fixed_tables(connection) -> None:
    cursor = connection.cursor()
    primary_keys = {
        "tbl_Units": ("UnitNumber",),
        "tbl_WeaponsRev": ("WeaponNo",),
        "tbl_ItemCategory": ("CatalogueID",),
    }
    for table, schema in SQL_SCHEMAS.items():
        if table == "tbl_UnitNames":
            # Repository.apply must create and validate this support table.
            continue
        definitions = [
            f"[{name}] {sql_type(kind, size)} {'NULL' if nullable else 'NOT NULL'}"
            for name, kind, size, nullable in schema
        ]
        key = primary_keys[table]
        definitions.append(
            "CONSTRAINT [PK_%s] PRIMARY KEY(%s)"
            % (table, ",".join(f"[{name}]" for name in key))
        )
        cursor.execute(
            f"CREATE TABLE dbo.[{table}]({','.join(definitions)})"
        )
    cursor.execute(
        """
CREATE TABLE dbo.tbl_UnitGrades(
    UnitGradeNumber tinyint NOT NULL,
    UnitGradeNumber2 tinyint NOT NULL,
    UnitGradeName varchar(2) NOT NULL,
    UnitGradeCalled tinyint NOT NULL,
    GetSpAttack int NOT NULL,
    GetSpDemage int NOT NULL,
    GetSpKilled int NOT NULL,
    MixValUp real NOT NULL,
    MixValKeep real NOT NULL,
    Parts2Prob int NOT NULL,
    Parts3Prob int NOT NULL,
    Parts4Prob int NOT NULL,
    PartsOffset tinyint NOT NULL,
    CONSTRAINT PK_tbl_UnitGrades PRIMARY KEY(UnitGradeNumber,UnitGradeNumber2)
);
CREATE TABLE dbo.tbl_UnitLevels(
    UnitLevelIndex int NOT NULL,
    UnitLevelNumber tinyint NOT NULL,
    UnitLevelName varchar(40) NOT NULL,
    UnitGrowWeighted int NULL,
    LapUnitExp int NOT NULL,
    CONSTRAINT PK_tbl_UnitLevels PRIMARY KEY(UnitLevelIndex,UnitLevelNumber)
);
CREATE TABLE dbo.tbl_SkillRev(
    SkillID int NOT NULL PRIMARY KEY,
    SkillName varchar(50) NOT NULL,
    Activate tinyint NOT NULL,
    ActivateValue smallint NOT NULL,
    MoveLv smallint NOT NULL,
    DashLv smallint NOT NULL,
    QuickLv smallint NOT NULL,
    MotiveLv smallint NOT NULL,
    BoostIncreaseRate real NOT NULL,
    BoostPower smallint NOT NULL,
    Attack smallint NOT NULL,
    Defense smallint NOT NULL,
    RadarRadius smallint NOT NULL,
    SpIncreaseRate smallint NOT NULL,
    Scale real NOT NULL,
    AutoBulletChargeRate real NOT NULL,
    BulletChargeRate real NOT NULL,
    AtkSpdRate real NOT NULL,
    ActiveResult smallint NOT NULL,
    Feature tinyint NOT NULL,
    UseTarget tinyint NOT NULL,
    BattleMode tinyint NOT NULL,
    PartSpLevel tinyint NOT NULL
);
CREATE TABLE dbo.tbl_Funnel(
    FunnelID int NOT NULL PRIMARY KEY,
    FunnelProp int NOT NULL,
    AniTime int NOT NULL,
    AttackDelay int NOT NULL,
    AttackCount tinyint NOT NULL,
    Stealable tinyint NOT NULL
);
CREATE TABLE dbo.tbl_UnitGraph(
    GraphNumber int NOT NULL PRIMARY KEY,
    Power smallint NOT NULL,
    Armor smallint NOT NULL,
    Mobility smallint NOT NULL,
    Control smallint NOT NULL,
    PosNumber tinyint NOT NULL,
    DefaultSkill_1 smallint NOT NULL,
    DefaultSkill_2 smallint NOT NULL
)
"""
    )
    connection.commit()


def insert_row(connection, table: str, columns: tuple[str, ...], row: dict) -> None:
    connection.cursor().execute(
        f"INSERT dbo.[{table}]({','.join(f'[{name}]' for name in columns)}) "
        f"VALUES({','.join('?' for _ in columns)})",
        *(row[name] for name in columns),
    )


def skill_row(identity: int) -> dict:
    return {
        name: (
            identity if name == "SkillID"
            else f"Skill {identity}" if name == "SkillName"
            else 1.0 if name in {
                "BoostIncreaseRate", "Scale", "AutoBulletChargeRate",
                "BulletChargeRate", "AtkSpdRate",
            }
            else 0
        )
        for name in SKILL_COLUMNS
    }


def grade_row() -> dict:
    return {
        name: (
            4 if name in ("UnitGradeNumber", "UnitGradeNumber2")
            else "SR" if name == "UnitGradeName"
            else 0.0 if name in ("MixValUp", "MixValKeep")
            else 0
        )
        for name in GRADE_COLUMNS
    }


def install_minimum_catalog(connection) -> None:
    native = unit_row(10001)
    native.update({
        "UnitModelName": "Native Unit",
        "WeaponNumber_1": 45082,
        "WeaponNumber_2": 43286,
        "WeaponNumber_3": 0,
        "WeaponNumber_FatalAttack": 43286,
    })
    insert_row(connection, "tbl_Units", UNIT_COLUMNS, native)
    for identity in (45082, 43286):
        insert_row(connection, "tbl_WeaponsRev", WEAPON_COLUMNS, weapon_row(identity))
    insert_row(connection, "tbl_ItemCategory", ITEM_COLUMNS, {
        "CatalogueID": 1110008,
        "Name": "NativeTemplate",
        "RefIndex": 10001,
        "TimeLimit": 1,
        "Option1": 2,
        "Option2": 2,
    })
    insert_row(connection, "tbl_UnitGrades", GRADE_COLUMNS, grade_row())
    insert_row(connection, "tbl_UnitLevels", LEVEL_COLUMNS, {
        "UnitLevelIndex": 1,
        "UnitLevelNumber": 1,
        "UnitLevelName": "Level 1",
        "UnitGrowWeighted": 30,
        "LapUnitExp": 300,
    })
    for identity in (70001, 70002, 70003):
        insert_row(connection, "tbl_SkillRev", SKILL_COLUMNS, skill_row(identity))
    insert_row(
        connection, "tbl_Funnel",
        ("FunnelID", "FunnelProp", "AniTime", "AttackDelay", "AttackCount", "Stealable"),
        {
            "FunnelID": 198001, "FunnelProp": 1, "AniTime": 1,
            "AttackDelay": 1, "AttackCount": 3, "Stealable": 1,
        },
    )
    insert_row(
        connection, "tbl_UnitGraph",
        (
            "GraphNumber", "Power", "Armor", "Mobility", "Control",
            "PosNumber", "DefaultSkill_1", "DefaultSkill_2",
        ),
        {
            "GraphNumber": 1010001, "Power": 54, "Armor": 51,
            "Mobility": 54, "Control": 44, "PosNumber": 1,
            "DefaultSkill_1": 30500, "DefaultSkill_2": 30504,
        },
    )
    connection.commit()


def variant(
    *,
    family: str,
    root: int,
    weapon_first: int,
    weapon_second: int,
    category: int,
) -> dict:
    value = base_plan()
    value["family_key"] = family
    value["root_unit_id"] = root
    value["units"] = [unit_row(root)]
    value["units"][0].update({
        "WeaponNumber_1": weapon_first,
        "WeaponNumber_2": 45082,
        "WeaponNumber_3": weapon_second,
        "WeaponNumber_FatalAttack": 43286,
    })
    value["weapons"] = [weapon_row(weapon_first), weapon_row(weapon_second)]
    value["weapons"][0]["FunnelID"] = 198001
    value["item"] = item_row(category, root)
    value["names"] = [name_row(root)]
    return value


def fetch_row(connection, table: str, columns: tuple[str, ...], key: str, identity: int):
    row = connection.cursor().execute(
        f"SELECT {','.join(f'[{name}]' for name in columns)} "
        f"FROM dbo.[{table}] WHERE [{key}]=?",
        identity,
    ).fetchone()
    return None if row is None else dict(zip(columns, row))


def readback_differences(expected: dict, actual: dict, table: str) -> list[str]:
    result = []
    for name, kind, _, _ in SQL_SCHEMAS[table]:
        left, right = expected[name], actual[name]
        if kind == "real" and left is not None:
            left = struct.unpack("<f", struct.pack("<f", float(left)))[0]
        if left != right:
            result.append(f"{name}:{left!r}!={right!r}")
    return result


def assert_absent(connection, value: dict) -> None:
    checks = (
        ("tbl_Units", "UnitNumber", [row["UnitNumber"] for row in value["units"]]),
        ("tbl_WeaponsRev", "WeaponNo", [row["WeaponNo"] for row in value["weapons"]]),
        ("tbl_ItemCategory", "CatalogueID", [value["item"]["CatalogueID"]]),
        ("tbl_UnitNames", "UnitNumber", [row["UnitNumber"] for row in value["names"]]),
    )
    cursor = connection.cursor()
    for table, key, identities in checks:
        for identity in identities:
            count = cursor.execute(
                f"SELECT COUNT(*) FROM dbo.[{table}] WHERE [{key}]=?",
                identity,
            ).fetchone()[0]
            if count != 0:
                raise AssertionError(f"rollback left {table} {identity}")
    identities = [
        *(row["UnitNumber"] for row in value["units"]),
        *(row["WeaponNo"] for row in value["weapons"]),
        value["item"]["CatalogueID"],
    ]
    for identity in identities:
        count = cursor.execute(
            "SELECT COUNT(*) FROM dbo.sdgo_UnitImportDefinitions WHERE RecordID=?",
            identity,
        ).fetchone()[0]
        if count != 0:
            raise AssertionError(f"rollback left ownership {identity}")


def run_fixture(factory: SqlFactory, database: str, receipt: dict) -> None:
    token = uuid.uuid4().hex
    connection = factory.connect(database)
    try:
        actual = connection.cursor().execute("SELECT DB_NAME()").fetchone()[0]
        if actual != database:
            raise AssertionError("fixture DB_NAME() verification failed")
        connection.cursor().execute(
            "EXEC sys.sp_addextendedproperty @name=?,@value=?",
            MARKER_NAME, token,
        )
        connection.commit()
        receipt["database_verified"] = True
        receipt["ownership_marker"] = token
        create_fixed_tables(connection)
        install_minimum_catalog(connection)
    finally:
        connection.close()

    def fixture_connect(_server_ini):
        result = factory.connect(database)
        actual = result.cursor().execute("SELECT DB_NAME()").fetchone()[0]
        if actual != database:
            result.close()
            raise AssertionError("repository connected outside fixture database")
        return result

    tests: list[dict[str, str]] = receipt["tests"]
    with patch.object(repository_module, "_connect", side_effect=fixture_connect), \
         patch.object(repository_module, "database_target_key", return_value=database):
        repository = Repository(None, Path("fixture-credentials-not-exported.ini"))
        first = variant(
            family="fixture-family", root=25060, weapon_first=61165,
            weapon_second=61166, category=1119000,
        )
        result = repository.apply(first)
        if result["inserted"] != {
            "weapons": 2, "units": 1, "names": 1, "items": 1
        }:
            raise AssertionError("first apply inserted unexpected counts")
        tests.append({"name": "new_family_apply", "status": "passed"})

        verify = factory.connect(database)
        try:
            for table, columns, key, rows in (
                ("tbl_Units", UNIT_COLUMNS, "UnitNumber", first["units"]),
                ("tbl_WeaponsRev", WEAPON_COLUMNS, "WeaponNo", first["weapons"]),
                ("tbl_ItemCategory", ITEM_COLUMNS, "CatalogueID", [first["item"]]),
                ("tbl_UnitNames", ("UnitNumber", "NameCN", "NameEN"), "UnitNumber", first["names"]),
            ):
                for expected in rows:
                    actual = fetch_row(verify, table, columns, key, expected[key])
                    if actual is None or not _readback_matches(expected, actual, table):
                        if actual is None:
                            present = [
                                int(row[0]) for row in verify.cursor().execute(
                                    f"SELECT [{key}] FROM dbo.[{table}] ORDER BY [{key}]"
                                ).fetchall()
                            ]
                            differences = [f"missing {expected[key]!r}; present={present}"]
                        else:
                            differences = readback_differences(
                                expected, actual, table
                            )
                        raise AssertionError(
                            f"{table} full readback differs: {','.join(differences)}"
                        )
            ledger = verify.cursor().execute(
                """
SELECT RecordKind,RecordID,OwnerKey,FamilyKey,RecordKey,Revision,
       OriginalJson,InstalledHash
FROM dbo.sdgo_UnitImportDefinitions
ORDER BY RecordKind,RecordID
"""
            ).fetchall()
            if len(ledger) != 5:
                raise AssertionError("ownership row count differs")
            for row in ledger:
                if (
                    row[2] != OWNER_KEY or row[3] != first["family_key"]
                    or row[6] != "null"
                    or not re.fullmatch(r"[0-9a-f]{64}", str(row[7]))
                ):
                    raise AssertionError("ownership row content differs")
            verify.rollback()
        finally:
            verify.close()
        tests.append({"name": "full_readback_and_ownership", "status": "passed"})

        upgrade = copy.deepcopy(first)
        upgrade["revision"] = "fixture-revision-2"
        upgrade["units"][0]["HP"] = 15000
        result = repository.apply(upgrade)
        if result["updated"]["units"] != 1:
            raise AssertionError("same-owner update was not recorded")
        verify = factory.connect(database)
        try:
            hp = verify.cursor().execute(
                "SELECT HP FROM dbo.tbl_Units WHERE UnitNumber=25060"
            ).fetchone()[0]
            if hp != 15000:
                raise AssertionError("same-owner update readback differs")
            verify.rollback()
        finally:
            verify.close()
        tests.append({"name": "same_owner_upgrade", "status": "passed"})

        external = factory.connect(database)
        try:
            external.cursor().execute(
                "UPDATE dbo.tbl_Units SET HP=1 WHERE UnitNumber=25060"
            )
            external.commit()
        finally:
            external.close()
        try:
            repository.apply(upgrade)
        except ValueError as error:
            if "账本外" not in str(error):
                raise
        else:
            raise AssertionError("external modification was accepted")
        tests.append({"name": "external_modification_rejected", "status": "passed"})

        missing = variant(
            family="missing-reference", root=25070, weapon_first=61175,
            weapon_second=61176, category=1119001,
        )
        before_delete = Repository(None, Path("fixture-credentials-not-exported.ini"))
        edit = factory.connect(database)
        try:
            edit.cursor().execute(
                "DELETE FROM dbo.tbl_WeaponsRev WHERE WeaponNo=45082"
            )
            edit.commit()
        finally:
            edit.close()
        try:
            before_delete.apply(missing)
        except ValueError as error:
            if "WeaponNumber_2=45082" not in str(error):
                raise
        else:
            raise AssertionError("missing live reference was accepted")
        verify = factory.connect(database)
        try:
            assert_absent(verify, missing)
            insert_row(verify, "tbl_WeaponsRev", WEAPON_COLUMNS, weapon_row(45082))
            verify.commit()
        finally:
            verify.close()
        tests.append({"name": "missing_reference_transaction_rollback", "status": "passed"})

        lossy = variant(
            family="lossy-varchar", root=25080, weapon_first=61185,
            weapon_second=61186, category=1119002,
        )
        lossy["units"][0]["UnitModelName"] = "命运高达"
        clean_repository = Repository(
            None, Path("fixture-credentials-not-exported.ini")
        )
        try:
            clean_repository.apply(lossy)
        except ValueError as error:
            if "有损写入" not in str(error):
                raise
        else:
            raise AssertionError("lossy varchar write was accepted")
        verify = factory.connect(database)
        try:
            assert_absent(verify, lossy)
            verify.rollback()
        finally:
            verify.close()
        tests.append({"name": "lossy_varchar_transaction_rollback", "status": "passed"})

    receipt["ownership_marker"] = token


def write_receipt(path: Path, receipt: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(receipt, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ini", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args(argv)
    database = "SDGO_UnitImportTest_" + uuid.uuid4().hex[:12]
    if DATABASE_NAME.fullmatch(database) is None:
        raise AssertionError("generated fixture database name is invalid")
    receipt = {
        "schema_version": 1,
        "mode": "isolated_sql_fixture",
        "opt_in": True,
        "database_name": database,
        "started_at": now(),
        "status": "running",
        "source_database_written": False,
        "player_tables_created_or_read": False,
        "services_restarted": False,
        "database_created": False,
        "database_verified": False,
        "database_dropped": False,
        "tests": [],
    }
    created = False
    marker = None
    exit_code = 1
    try:
        factory = SqlFactory(args.server_ini)
        source = factory.connect(factory.source_database)
        try:
            cursor = source.cursor()
            receipt["sql_product_version"] = str(
                cursor.execute(
                    "SELECT CONVERT(varchar(128),SERVERPROPERTY('ProductVersion'))"
                ).fetchone()[0]
            )
            collation = str(
                cursor.execute(
                    "SELECT CONVERT(varchar(128),DATABASEPROPERTYEX(DB_NAME(),'Collation'))"
                ).fetchone()[0]
            )
            if COLLATION_NAME.fullmatch(collation) is None:
                raise ValueError("源数据库排序规则名称不安全")
            receipt["fixture_collation"] = collation
            source.rollback()
        finally:
            source.close()

        admin = factory.connect("master", autocommit=True)
        try:
            if admin.cursor().execute(
                "SELECT DB_ID(?)", database
            ).fetchone()[0] is not None:
                raise AssertionError("generated fixture database already exists")
            try:
                admin.cursor().execute(
                    f"CREATE DATABASE [{database}] COLLATE {collation}"
                )
            except factory.pyodbc.Error as error:
                receipt["status"] = "not_run_create_database_failed"
                receipt["error"] = sanitized_error(error)
                exit_code = 2
            else:
                created = True
                receipt["database_created"] = True
        finally:
            admin.close()

        if created:
            run_fixture(factory, database, receipt)
            receipt["status"] = "passed"
            exit_code = 0
    except Exception as error:
        receipt["status"] = "failed"
        receipt["error"] = sanitized_error(error)
    finally:
        receipt["finished_at"] = now()
        if created:
            try:
                marker = receipt.get("ownership_marker")
                if DATABASE_NAME.fullmatch(database) is None:
                    raise AssertionError("refusing to drop unexpected database name")
                verify = factory.connect(database)
                try:
                    actual_name = verify.cursor().execute(
                        "SELECT DB_NAME()"
                    ).fetchone()[0]
                    row = verify.cursor().execute(
                        """
SELECT CONVERT(nvarchar(128),value)
FROM sys.extended_properties
WHERE class=0 AND name=?
""",
                        MARKER_NAME,
                    ).fetchone()
                    actual_marker = None if row is None else str(row[0])
                    verify.rollback()
                finally:
                    verify.close()
                if actual_name != database or not marker or actual_marker != marker:
                    raise AssertionError(
                        "fixture ownership could not be reverified; database retained"
                    )
                admin = factory.connect("master", autocommit=True)
                try:
                    admin.cursor().execute(
                        f"ALTER DATABASE [{database}] "
                        "SET SINGLE_USER WITH ROLLBACK IMMEDIATE"
                    )
                    admin.cursor().execute(f"DROP DATABASE [{database}]")
                    receipt["database_dropped"] = True
                finally:
                    admin.close()
            except Exception as cleanup_error:
                receipt["cleanup_error"] = sanitized_error(cleanup_error)
                if receipt["status"] == "passed":
                    receipt["status"] = "failed_cleanup"
                    exit_code = 1
        receipt.pop("ownership_marker", None)
        write_receipt(args.receipt, receipt)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
