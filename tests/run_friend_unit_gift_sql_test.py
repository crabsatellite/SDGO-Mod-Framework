#!/usr/bin/env python3
"""SQL Server 2014 fixture test for friend_unit_gift_server.

The fixture is synthetic and lives in one task-owned database. It never writes
GOnlineGame, GDCommon, the shared native backup, or any production database.
"""

from __future__ import annotations

import argparse
import configparser
import concurrent.futures
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys
import traceback
import uuid

import pyodbc


DATABASE = "SDGOFriendUnitGiftPolicyFixture"
EXPECTED_SQL_PID = 29848
EXPECTED_SQL_PORT = 61633
OWNER_PROPERTY = "sdgo_friend_unit_gift_fixture_owner"

UNIT_COLUMNS = [
    ("UserUnitNumber", "bigint", False, True),
    ("AccountNumber", "int", False, False),
    ("UserUnitSlotNumber", "int", False, False),
    ("UnitNumber", "int", False, False),
    ("UnitLevelNumber", "tinyint", False, False),
    ("UnitKill", "int", False, False),
    ("StateSaleIntention", "char(1) COLLATE Thai_CI_AS", False, False),
    ("DateGet", "datetime", False, False),
    ("TermValidityUse", "smallint", False, False),
    ("UnitColorNumber_1", "smallint", False, False),
    ("UnitColorNumber_2", "smallint", False, False),
    ("UnitColorNumber_3", "smallint", False, False),
    ("UnitColorNumber_4", "smallint", False, False),
    ("UnitColorNumber_5", "smallint", False, False),
    ("UnitColorNumber_6", "smallint", False, False),
    ("UnitDecalNumber_1", "int", False, False),
    ("UnitDecalNumber_2", "int", False, False),
    ("UnitDecalNumber_3", "int", False, False),
    ("UnitDead", "int", False, False),
    ("UnitBattle", "int", False, False),
    ("UnitWin", "int", False, False),
    ("UnitLost", "int", False, False),
    ("UnitTie", "int", False, False),
    ("BuyType", "int", False, False),
    ("CurEnergy", "real", False, False),
    ("DateEnd", "datetime", True, False),
    ("CategoryNumber", "int", False, False),
    ("ShopUnit", "tinyint", False, False),
    ("RunOut", "tinyint", False, False),
    ("Forbidden", "tinyint", False, False),
    ("TimeLimit", "tinyint", False, False),
    ("ShopItemId", "varchar(6) COLLATE Thai_CI_AS", True, False),
    ("ShopItemtblidx", "int", True, False),
    ("SerialNum", "bigint", True, False),
    ("CubeMapLevel", "smallint", False, False),
    ("SkillPartID", "int", False, False),
    ("BattleShipCount", "smallint", False, False),
    ("LapUnitExp", "int", False, False),
    ("Normal_MaxLv", "tinyint", False, False),
    ("LockState", "tinyint", False, False),
    ("UnitSkill_1", "smallint", True, False),
    ("UnitSkill_2", "smallint", True, False),
    ("UnitSkill_3", "smallint", True, False),
    ("UnitSkill_4", "smallint", True, False),
    ("ResetCnt", "smallint", True, False),
    ("UnitClass", "tinyint", False, False),
    ("UnitClassMark", "tinyint", False, False),
    ("ComposeExp", "tinyint", False, False),
    ("ComposeRepair", "tinyint", False, False),
    ("ComposeSkillReset", "tinyint", False, False),
]

SENDER = 101
RECIPIENT = 102
OTHER = 103


def quote_identifier(value: str) -> str:
    if not value.replace("_", "a").isalnum() or not value:
        raise ValueError("unsafe identifier")
    return f"[{value}]"


def sql_literal(value: str) -> str:
    return "N'" + value.replace("'", "''") + "'"


def connection_string(config: dict, database: str) -> str:
    if int(config["sql_port"]) != EXPECTED_SQL_PORT:
        raise RuntimeError("unexpected SQL port")
    return (
        "DRIVER={SQL Server};"
        f"SERVER={config['sql_host']},{config['sql_port']};"
        f"UID={config['app_user']};PWD={config['app_password']};"
        f"DATABASE={database};"
    )


def load_owner_config(path: Path) -> dict:
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.optionxform = str
    with path.open("r", encoding="utf-8-sig", errors="strict") as source:
        parser.read_file(source)
    if not parser.has_section("DB Member"):
        raise RuntimeError("owner credential source has no DB Member section")
    section = parser["DB Member"]
    user = section.get("id") or section.get("ID")
    password = section.get("pwd") or section.get("PWD")
    if user is None or password is None:
        raise RuntimeError("owner credential source is incomplete")
    if user.lower() != "sa" or len(password) != 6:
        raise RuntimeError("owner credential source is not the verified native owner")
    return {
        "sql_host": "127.0.0.1",
        "sql_port": EXPECTED_SQL_PORT,
        "app_user": user,
        "app_password": password,
    }


def connect(config: dict, database: str, *, autocommit: bool = True):
    return pyodbc.connect(
        connection_string(config, database), timeout=10, autocommit=autocommit
    )


def execute_install(config: dict, install_path: Path) -> tuple[bool, str]:
    script = r"""
$ErrorActionPreference='Stop'
$sql=Get-Content -LiteralPath $env:SDGO_GIFT_SQL_FILE -Raw -Encoding UTF8
$builder=New-Object System.Data.SqlClient.SqlConnectionStringBuilder
$builder['Data Source']='127.0.0.1,61633'
$builder['Initial Catalog']='SDGOFriendUnitGiftPolicyFixture'
$builder['User ID']=$env:SDGO_GIFT_SQL_USER
$builder['Password']=$env:SDGO_GIFT_SQL_PASSWORD
$builder['Integrated Security']=$false
$builder['Connect Timeout']=10
$builder['Encrypt']=$false
$connection=New-Object System.Data.SqlClient.SqlConnection $builder.ConnectionString
try {
  $connection.Open()
  $command=$connection.CreateCommand()
  $command.CommandTimeout=120
  $command.CommandText=$sql
  [void]$command.ExecuteNonQuery()
} finally {
  if($null -ne $connection){$connection.Dispose()}
}
"""
    environment = dict(
        os.environ,
        SDGO_GIFT_SQL_FILE=str(install_path.resolve()),
        SDGO_GIFT_SQL_USER=config["app_user"],
        SDGO_GIFT_SQL_PASSWORD=config["app_password"],
    )
    result = subprocess.run(
        [
            "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
            "-Command", script,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=150,
        env=environment,
    )
    detail = (result.stdout + result.stderr).replace(
        config["app_password"], "<redacted>"
    )
    return result.returncode == 0, detail[:4000]


def ensure_local_fixture_paths(directory: Path) -> tuple[Path, Path]:
    directory.mkdir(parents=True, exist_ok=True)
    absolute = directory.resolve(strict=True)
    if os.path.normcase(os.path.realpath(directory)) != os.path.normcase(str(absolute)):
        raise RuntimeError("fixture directory resolves through a junction")
    attrs = getattr(os.lstat(absolute), "st_file_attributes", 0)
    if attrs & 0x400:
        raise RuntimeError("fixture directory is a reparse point")
    data = absolute / f"{DATABASE}.mdf"
    log = absolute / f"{DATABASE}_log.ldf"
    if data.exists() or log.exists():
        raise RuntimeError("fixture database files already exist")
    return data, log


def create_database(config: dict, data: Path, log: Path, owner: str) -> None:
    with connect(config, "master") as connection:
        row = connection.cursor().execute(
            "SELECT CONVERT(int,SERVERPROPERTY('ProcessID')),"
            "CONVERT(nvarchar(128),SERVERPROPERTY('ProductVersion')),"
            "DB_ID(?)",
            DATABASE,
        ).fetchone()
        if int(row[0]) != EXPECTED_SQL_PID:
            raise RuntimeError("unexpected SQL process")
        if not str(row[1]).startswith("12.0."):
            raise RuntimeError("fixture requires SQL Server 2014")
        if row[2] is not None:
            raise RuntimeError("fixture database already exists")
        create = (
            f"CREATE DATABASE {quote_identifier(DATABASE)} ON PRIMARY "
            f"(NAME={sql_literal(DATABASE)},FILENAME={sql_literal(str(data))}) "
            f"LOG ON (NAME={sql_literal(DATABASE + '_log')},"
            f"FILENAME={sql_literal(str(log))})"
        )
        connection.cursor().execute(create)
    with connect(config, DATABASE) as connection:
        connection.cursor().execute(
            "EXEC sys.sp_addextendedproperty @name=?,@value=?", OWNER_PROPERTY, owner
        )


def guarded_drop_database(
    config: dict, data: Path, log: Path, owner: str
) -> None:
    with connect(config, "master") as connection:
        row = connection.cursor().execute(
            "SELECT DB_ID(?)", DATABASE
        ).fetchone()
        if row[0] is None:
            return
    with connect(config, DATABASE) as connection:
        row = connection.cursor().execute(
            "SELECT CONVERT(nvarchar(128),value) FROM sys.extended_properties "
            "WHERE class=0 AND name=?",
            OWNER_PROPERTY,
        ).fetchone()
        files = connection.cursor().execute(
            "SELECT physical_name FROM sys.database_files ORDER BY type"
        ).fetchall()
        actual = {os.path.normcase(os.path.abspath(str(item[0]))) for item in files}
        expected = {
            os.path.normcase(os.path.abspath(str(data))),
            os.path.normcase(os.path.abspath(str(log))),
        }
        if not row or row[0] != owner or actual != expected:
            raise RuntimeError("fixture cleanup ownership guard failed")
    with connect(config, "master") as connection:
        cursor = connection.cursor()
        cursor.execute(
            f"ALTER DATABASE {quote_identifier(DATABASE)} "
            "SET SINGLE_USER WITH ROLLBACK IMMEDIATE"
        )
        cursor.execute(f"DROP DATABASE {quote_identifier(DATABASE)}")
    if data.exists() or log.exists():
        raise RuntimeError("SQL Server left fixture files after guarded drop")


def create_native_schema(connection) -> None:
    unit_defs = []
    for name, sql_type, nullable, identity in UNIT_COLUMNS:
        suffix = " IDENTITY(1,1)" if identity else ""
        null_sql = " NULL" if nullable else " NOT NULL"
        unit_defs.append(f"[{name}] {sql_type}{suffix}{null_sql}")
    sql = f"""
CREATE TABLE dbo.tbl_Users(
 AccountNumber int NOT NULL CONSTRAINT PK_fixture_users PRIMARY KEY,
 UserBattle int NOT NULL,UserKill int NOT NULL,ExpandSlotCount int NOT NULL,
 GoUserUnitNum bigint NOT NULL,AssistUserUnitNum bigint NOT NULL,
 UserUnitKindCnt real NOT NULL,UserUnitTotalCnt smallint NOT NULL);
CREATE TABLE dbo.tbl_Units(
 UnitNumber int NOT NULL CONSTRAINT PK_fixture_units PRIMARY KEY,
 UnitGradeNumber tinyint NOT NULL,UnitGradeNumber2 tinyint NOT NULL);
CREATE TABLE dbo.tbl_UserUnits(
 {",".join(unit_defs)},
 CONSTRAINT PK_tbl_UserUnits PRIMARY KEY(AccountNumber,UserUnitNumber));
CREATE UNIQUE INDEX UK_tbl_UserUnits_AccountNumber_UserUnitSlotNumber
 ON dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber);
CREATE TABLE dbo.tbl_UserBattleShips(
 AccountNumber int NOT NULL CONSTRAINT PK_fixture_ships PRIMARY KEY,
 SelectPointUnit bigint NULL,UserUnitNumber_1 bigint NULL,
 UserUnitNumber_2 bigint NULL,UserUnitNumber_3 bigint NULL,
 UserUnitNumber_4 bigint NULL);
CREATE TABLE dbo.tbl_UserUnitSlotNumberList(
 UserUnitSlotNumber int NOT NULL CONSTRAINT PK_fixture_slots PRIMARY KEY);
CREATE TABLE dbo.tbl_Friend(
 f_idx int NOT NULL,f_idx2 int NOT NULL,f_relation int NOT NULL,
 f_cutoff char(1) COLLATE Thai_CI_AS NULL,
 CONSTRAINT UQ_fixture_friend UNIQUE(f_idx,f_idx2));
CREATE TABLE dbo.tbl_Account_Pilot(
 f_idx int NOT NULL CONSTRAINT PK_fixture_pilot PRIMARY KEY,
 f_pilot nvarchar(20) COLLATE Chinese_PRC_CS_AI NOT NULL);
"""
    connection.cursor().execute(sql)
    cursor = connection.cursor()
    cursor.fast_executemany = True
    cursor.executemany(
        "INSERT dbo.tbl_UserUnitSlotNumberList(UserUnitSlotNumber) VALUES(?)",
        [(value,) for value in range(1, 1201)],
    )


def default_unit(account: int, slot: int, unit_number: int) -> dict:
    values = {
        "AccountNumber": account,
        "UserUnitSlotNumber": slot,
        "UnitNumber": unit_number,
        "UnitLevelNumber": 1,
        "UnitKill": 0,
        "StateSaleIntention": "0",
        "DateGet": dt.datetime(2020, 1, 2, 3, 4, 5),
        "TermValidityUse": 0,
        "UnitDead": 0,
        "UnitBattle": 0,
        "UnitWin": 0,
        "UnitLost": 0,
        "UnitTie": 0,
        "BuyType": 0,
        "CurEnergy": 100.0,
        "DateEnd": None,
        "CategoryNumber": 0,
        "ShopUnit": 0,
        "RunOut": 0,
        "Forbidden": 0,
        "TimeLimit": 0,
        "ShopItemId": None,
        "ShopItemtblidx": None,
        "SerialNum": None,
        "CubeMapLevel": 0,
        "SkillPartID": 0,
        "BattleShipCount": 0,
        "LapUnitExp": 0,
        "Normal_MaxLv": 1,
        "LockState": 0,
        "UnitSkill_1": None,
        "UnitSkill_2": None,
        "UnitSkill_3": None,
        "UnitSkill_4": None,
        "ResetCnt": None,
        "UnitClass": 0,
        "UnitClassMark": 0,
        "ComposeExp": 0,
        "ComposeRepair": 0,
        "ComposeSkillReset": 0,
    }
    for index in range(1, 7):
        values[f"UnitColorNumber_{index}"] = 0
    for index in range(1, 4):
        values[f"UnitDecalNumber_{index}"] = 0
    return values


def insert_unit(connection, values: dict) -> int:
    names = [name for name, _, _, identity in UNIT_COLUMNS if not identity]
    cursor = connection.cursor()
    cursor.execute(
        f"SET NOCOUNT ON; INSERT dbo.tbl_UserUnits({','.join('[' + n + ']' for n in names)}) "
        f"VALUES({','.join('?' for _ in names)});"
        "SELECT CONVERT(bigint,SCOPE_IDENTITY())",
        [values[name] for name in names],
    )
    return int(cursor.fetchone()[0])


def seed_fixture(connection) -> dict:
    cursor = connection.cursor()
    cursor.executemany(
        "INSERT dbo.tbl_Users VALUES(?,?,?,?,?,?,?,?)",
        [
            (SENDER, 115, 549, 0, 0, 0, 0.0, 0),
            (RECIPIENT, 10, 10, 0, 0, 0, 0.0, 0),
            (OTHER, 10, 10, 0, 0, 0, 0.0, 0),
        ],
    )
    cursor.executemany(
        "INSERT dbo.tbl_UserBattleShips VALUES(?,?,?,?,?,?)",
        [(SENDER, None, None, None, None, None),
         (RECIPIENT, None, None, None, None, None),
         (OTHER, None, None, None, None, None)],
    )
    cursor.executemany(
        "INSERT dbo.tbl_Friend VALUES(?,?,7,'0')",
        [(SENDER, RECIPIENT), (RECIPIENT, SENDER)],
    )
    cursor.executemany(
        "INSERT dbo.tbl_Account_Pilot VALUES(?,?)",
        [(SENDER, "Sender Fixture"), (RECIPIENT, "Recipient Fixture")],
    )
    catalog = []
    number = 1000
    for grade in range(1, 5):
        for subtype in range(2, 5):
            for _ in range(2):
                catalog.append((number, grade, subtype))
                number += 1
    catalog.extend([(9990, 1, 2), (9991, 1, 2), (9992, 1, 2), (9993, 1, 2)])
    cursor.executemany("INSERT dbo.tbl_Units VALUES(?,?,?)", catalog)

    sender_uids = []
    for slot, (unit_number, _, _) in enumerate(catalog[:24], start=1):
        sender_uids.append(insert_unit(connection, default_unit(SENDER, slot, unit_number)))
    special = default_unit(SENDER, 25, 9990)
    special.update(
        UnitLevelNumber=12, UnitKill=33, TermValidityUse=7,
        UnitColorNumber_1=11, UnitColorNumber_2=12, UnitColorNumber_3=13,
        UnitColorNumber_4=14, UnitColorNumber_5=15, UnitColorNumber_6=16,
        UnitDecalNumber_1=21, UnitDecalNumber_2=22, UnitDecalNumber_3=23,
        UnitDead=4, UnitBattle=15, UnitWin=9, UnitLost=5, UnitTie=1,
        BuyType=3, CurEnergy=77.25, CategoryNumber=2, ShopUnit=1,
        ShopItemId="A12345", ShopItemtblidx=456, SerialNum=987654321,
        CubeMapLevel=2, SkillPartID=789, LapUnitExp=3456, Normal_MaxLv=15,
        UnitSkill_1=101, UnitSkill_2=102, UnitSkill_3=103, UnitSkill_4=104,
        ResetCnt=2, UnitClass=3, UnitClassMark=4,
        ComposeExp=5, ComposeRepair=6, ComposeSkillReset=7,
        DateGet=dt.datetime(2018, 7, 8, 9, 10, 11),
    )
    gift_uid = insert_unit(connection, special)
    recipient_one = insert_unit(connection, default_unit(RECIPIENT, 1, 9991))
    recipient_three = insert_unit(connection, default_unit(RECIPIENT, 3, 9992))
    refresh_counts(connection, SENDER)
    refresh_counts(connection, RECIPIENT)
    return {
        "gift_uid": gift_uid,
        "recipient_uids": [recipient_one, recipient_three],
    }


def refresh_counts(connection, account: int) -> None:
    connection.cursor().execute(
        "UPDATE u SET UserUnitKindCnt=x.k,UserUnitTotalCnt=x.n "
        "FROM dbo.tbl_Users u CROSS APPLY("
        "SELECT CONVERT(real,COUNT(DISTINCT UnitNumber)) k,"
        "CONVERT(smallint,COUNT(*)) n FROM dbo.tbl_UserUnits "
        "WHERE AccountNumber=?) x WHERE u.AccountNumber=?",
        account,
        account,
    )


def add_friendship(connection) -> None:
    connection.cursor().execute(
        "IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Friend WHERE f_idx=? AND f_idx2=?) "
        "INSERT dbo.tbl_Friend VALUES(?,?,7,'0');"
        "IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Friend WHERE f_idx=? AND f_idx2=?) "
        "INSERT dbo.tbl_Friend VALUES(?,?,7,'0')",
        SENDER, RECIPIENT, SENDER, RECIPIENT,
        RECIPIENT, SENDER, RECIPIENT, SENDER,
    )


def exec_result(connection, proc: str, params: list):
    placeholders = ",".join("?" for _ in params)
    row = connection.cursor().execute(
        f"EXEC [sdgo_friend_unit_gift].[{proc}] {placeholders}", params
    ).fetchone()
    return tuple(row)


def row_payload(connection, account: int, uid: int):
    columns = ",".join(f"[{item[0]}]" for item in UNIT_COLUMNS)
    return tuple(connection.cursor().execute(
        f"SELECT {columns} FROM dbo.tbl_UserUnits "
        "WHERE AccountNumber=? AND UserUnitNumber=?",
        account, uid,
    ).fetchone())


def assert_payload_equal(before: tuple, after: tuple, *, account: int, slot: int):
    assert len(before) == len(after) == 50
    for index, (name, _, _, _) in enumerate(UNIT_COLUMNS):
        expected = before[index]
        if name == "AccountNumber":
            expected = account
        elif name == "UserUnitSlotNumber":
            expected = slot
        if isinstance(expected, float):
            assert abs(float(after[index]) - expected) < 0.0001, name
        else:
            assert after[index] == expected, name


def set_policy(connection, enabled: bool) -> None:
    connection.cursor().execute(
        "EXEC [sdgo_friend_unit_gift].[SetPolicy] ?,20,1,12,2",
        int(enabled),
    )


def offer(
    connection, uid: int, note: str = "保留原样", request_id: str | None = None
):
    return exec_result(
        connection, "Offer",
        [SENDER, request_id or str(uuid.uuid4()), RECIPIENT, uid, note],
    )


def test_policy(connection, config: dict, original: tuple) -> dict:
    results: dict[str, object] = {}
    gift_uid = int(original[0])
    malformed_request = str(uuid.uuid4())
    assert exec_result(
        connection, "Offer",
        [None, malformed_request, RECIPIENT, gift_uid, "invalid"],
    )[0] == 2
    assert exec_result(
        connection, "Offer",
        [SENDER, malformed_request, None, gift_uid, "invalid"],
    )[0] == 2
    assert exec_result(
        connection, "Offer",
        [SENDER, malformed_request, RECIPIENT, None, "invalid"],
    )[0] == 2
    assert exec_result(
        connection, "Accept", [None, malformed_request, 1]
    )[0] == 2
    assert exec_result(
        connection, "Accept", [RECIPIENT, malformed_request, None]
    )[0] == 2
    embedded_null = tuple(connection.cursor().execute(
        "DECLARE @BadNote nvarchar(3)=N'a'+NCHAR(0)+N'b';"
        "EXEC [sdgo_friend_unit_gift].[Offer] "
        "@ActorAccount=?,@RequestId=?,@RecipientAccount=?,"
        "@UserUnitNumber=?,@Note=@BadNote",
        SENDER, str(uuid.uuid4()), RECIPIENT, gift_uid,
    ).fetchone())
    assert embedded_null[0] == 2
    assert connection.cursor().execute(
        "SELECT COUNT(*) FROM [sdgo_friend_unit_gift].[CommandReceipt] "
        "WHERE RequestId=?", malformed_request,
    ).fetchone()[0] == 0
    results["malformed_inputs_rejected_without_receipt"] = True

    with connect(config, DATABASE) as implicit_connection:
        implicit_cursor = implicit_connection.cursor()
        implicit_cursor.execute("SET IMPLICIT_TRANSACTIONS ON")
        implicit_rejected = False
        try:
            exec_result(
                implicit_connection, "Offer",
                [SENDER, str(uuid.uuid4()), RECIPIENT, gift_uid, "implicit"],
            )
        except pyodbc.Error:
            implicit_rejected = True
        assert implicit_rejected
        assert int(implicit_cursor.execute("SELECT @@TRANCOUNT").fetchone()[0]) == 0
        implicit_cursor.execute("SET IMPLICIT_TRANSACTIONS OFF")

    with connect(config, DATABASE, autocommit=False) as ambient_connection:
        ambient_cursor = ambient_connection.cursor()
        ambient_cursor.execute(
            "SELECT COUNT(*) FROM dbo.tbl_Users WITH(UPDLOCK)"
        ).fetchone()
        assert int(ambient_cursor.execute("SELECT @@TRANCOUNT").fetchone()[0]) == 1
        ambient_rejected = False
        try:
            exec_result(
                ambient_connection, "Offer",
                [SENDER, str(uuid.uuid4()), RECIPIENT, gift_uid, "ambient"],
            )
        except pyodbc.Error:
            ambient_rejected = True
        assert ambient_rejected
        assert int(ambient_cursor.execute("SELECT @@TRANCOUNT").fetchone()[0]) == 1
        ambient_connection.rollback()
    results["caller_transaction_modes_rejected_without_leak"] = True

    preview = tuple(connection.cursor().execute(
        "EXEC [sdgo_friend_unit_gift].[PreviewOffer] ?,?,?",
        SENDER, RECIPIENT, gift_uid,
    ).fetchone())
    assert preview[0] == 0
    assert int(preview[4]) == gift_uid
    assert int(preview[9]) == RECIPIENT
    assert preview[10] == "Recipient Fixture"

    offer_request = str(uuid.uuid4())
    accepted = offer(connection, gift_uid, request_id=offer_request)
    assert accepted[0] == 0 and accepted[4] == gift_uid
    gift_id = int(accepted[1])
    pending_detail = tuple(connection.cursor().execute(
        "EXEC [sdgo_friend_unit_gift].[QueryGift] ?,?",
        RECIPIENT, gift_id,
    ).fetchone())
    assert int(pending_detail[0]) == gift_id
    assert int(pending_detail[1]) == gift_uid
    assert pending_detail[5] == "Sender Fixture"
    assert pending_detail[6] == "保留原样"
    assert int(pending_detail[12]) == 0
    assert offer(connection, gift_uid, request_id=offer_request) == accepted
    assert offer(
        connection, gift_uid, "different", request_id=offer_request
    )[0] == 12
    assert offer(
        connection, gift_uid, "保留原样 ", request_id=offer_request
    )[0] == 12
    assert exec_result(
        connection, "Offer",
        [SENDER, offer_request, OTHER, gift_uid, "保留原样"],
    )[0] == 12
    assert exec_result(
        connection, "Offer",
        [SENDER, offer_request, RECIPIENT, gift_uid + 1, "保留原样"],
    )[0] == 12
    set_policy(connection, False)
    connection.cursor().execute("DELETE dbo.tbl_Friend")
    accept_request = str(uuid.uuid4())
    accepted_result = exec_result(
        connection, "Accept", [RECIPIENT, accept_request, gift_id]
    )
    assert accepted_result[0] == 0 and accepted_result[2] == 1
    assert accepted_result[3] == 2
    after = row_payload(connection, RECIPIENT, gift_uid)
    assert_payload_equal(original, after, account=RECIPIENT, slot=2)
    accepted_detail = tuple(connection.cursor().execute(
        "EXEC [sdgo_friend_unit_gift].[QueryGift] ?,?",
        SENDER, gift_id,
    ).fetchone())
    assert int(accepted_detail[3]) == 1
    assert int(accepted_detail[4]) == RECIPIENT
    assert accepted_detail[5] == "Recipient Fixture"
    assert int(accepted_detail[8]) > 0
    assert int(accepted_detail[12]) == 1
    replay = exec_result(connection, "Accept", [RECIPIENT, accept_request, gift_id])
    assert replay == accepted_result
    receipt = exec_result(
        connection, "QueryReceipt", [RECIPIENT, accept_request]
    )
    assert receipt == accepted_result
    assert exec_result(
        connection, "Accept", [RECIPIENT, accept_request, gift_id + 9999]
    )[0] == 12
    conflict = exec_result(connection, "Return", [RECIPIENT, accept_request, gift_id])
    assert conflict[0] == 12
    blocked = offer(connection, int(
        connection.cursor().execute(
            "SELECT TOP 1 UserUnitNumber FROM dbo.tbl_UserUnits "
            "WHERE AccountNumber=? ORDER BY UserUnitNumber", SENDER
        ).fetchone()[0]
    ))
    assert blocked[0] == 1
    provenance = connection.cursor().execute(
        "EXEC [sdgo_friend_unit_gift].[QueryProvenance] ?,?",
        RECIPIENT, gift_uid,
    ).fetchone()
    assert provenance and provenance[6] == "保留原样"
    counters = connection.cursor().execute(
        "SELECT UserUnitKindCnt,UserUnitTotalCnt,"
        "(SELECT COUNT(DISTINCT UnitNumber) FROM dbo.tbl_UserUnits WHERE AccountNumber=?),"
        "(SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber=?) "
        "FROM dbo.tbl_Users WHERE AccountNumber=?",
        RECIPIENT, RECIPIENT, RECIPIENT,
    ).fetchone()
    assert int(counters[0]) == counters[2]
    assert counters[1] == counters[3]
    results["accept_preserved_all_50_columns"] = True
    results["preview_detail_receipt_provenance_contracts"] = True
    results["committed_mutation_receipt_recovery"] = True
    results["disabled_existing_accept"] = True
    results["unfriended_existing_accept"] = True
    results["idempotent_and_conflict_bound"] = True

    set_policy(connection, True)
    add_friendship(connection)
    connection.cursor().execute(
        "UPDATE dbo.tbl_Users SET ExpandSlotCount=1 WHERE AccountNumber=?",
        SENDER,
    )
    second_values = default_unit(SENDER, 25, 9990)
    second_values["DateGet"] = dt.datetime(2017, 2, 3, 4, 5, 6)
    second_uid = insert_unit(connection, second_values)
    refresh_counts(connection, SENDER)
    second_original = row_payload(connection, SENDER, second_uid)
    second_offer = offer(connection, second_uid, "return")
    assert second_offer[0] == 0
    connection.cursor().execute("DELETE dbo.tbl_Friend")
    set_policy(connection, False)
    return_request = str(uuid.uuid4())
    returned = exec_result(
        connection, "Return",
        [RECIPIENT, return_request, int(second_offer[1])],
    )
    assert returned[0] == 0 and returned[2] == 3 and returned[3] == 25
    assert exec_result(
        connection, "Return",
        [RECIPIENT, return_request, int(second_offer[1]) + 9999],
    )[0] == 12
    assert_payload_equal(
        second_original, row_payload(connection, SENDER, second_uid),
        account=SENDER, slot=25,
    )
    results["disabled_unfriended_return"] = True

    set_policy(connection, True)
    add_friendship(connection)
    connection.cursor().execute(
        "DELETE dbo.tbl_UserUnits WHERE AccountNumber=? AND UserUnitNumber=?",
        SENDER, second_uid,
    )
    refresh_counts(connection, SENDER)
    third_values = default_unit(SENDER, 25, 9990)
    third_values["DateGet"] = dt.datetime(2016, 3, 4, 5, 6, 7)
    third_uid = insert_unit(connection, third_values)
    refresh_counts(connection, SENDER)
    third_original = row_payload(connection, SENDER, third_uid)
    third_offer = offer(connection, third_uid, "wait")
    assert third_offer[0] == 0
    filler = default_unit(SENDER, 25, 9993)
    filler_uid = insert_unit(connection, filler)
    refresh_counts(connection, SENDER)
    set_policy(connection, False)
    waiting = exec_result(
        connection, "Return",
        [RECIPIENT, str(uuid.uuid4()), int(third_offer[1])],
    )
    assert waiting[0] == 11 and waiting[2] == 2
    assert connection.cursor().execute(
        "SELECT COUNT(*) FROM [sdgo_friend_unit_gift].[EscrowUnit] "
        "WHERE GiftId=?", int(third_offer[1])
    ).fetchone()[0] == 1
    returned_inbox = connection.cursor().execute(
        "EXEC [sdgo_friend_unit_gift].[QueryReturned] ?", SENDER
    ).fetchall()
    assert any(int(row[0]) == int(third_offer[1]) for row in returned_inbox)
    connection.cursor().execute(
        "DELETE dbo.tbl_UserUnits WHERE AccountNumber=? AND UserUnitNumber=?",
        SENDER, filler_uid,
    )
    refresh_counts(connection, SENDER)
    claim_request = str(uuid.uuid4())
    claimed = exec_result(
        connection, "ClaimReturned",
        [SENDER, claim_request, int(third_offer[1])],
    )
    assert claimed[0] == 0 and claimed[2] == 3
    assert exec_result(
        connection, "ClaimReturned",
        [SENDER, claim_request, int(third_offer[1]) + 9999],
    )[0] == 12
    assert_payload_equal(
        third_original, row_payload(connection, SENDER, third_uid),
        account=SENDER, slot=25,
    )
    results["return_waits_for_capacity_then_claims"] = True

    set_policy(connection, True)
    add_friendship(connection)
    candidate = int(connection.cursor().execute(
        "SELECT TOP 1 UserUnitNumber FROM dbo.tbl_UserUnits "
        "WHERE AccountNumber=? AND UserUnitSlotNumber=24", SENDER
    ).fetchone()[0])
    invariant_cases = [
        ("LockState", "UPDATE dbo.tbl_UserUnits SET LockState=1 "
                      "WHERE AccountNumber=? AND UserUnitNumber=?",
         "UPDATE dbo.tbl_UserUnits SET LockState=0 "
         "WHERE AccountNumber=? AND UserUnitNumber=?"),
        ("BattleShipCount", "UPDATE dbo.tbl_UserUnits SET BattleShipCount=1 "
                            "WHERE AccountNumber=? AND UserUnitNumber=?",
         "UPDATE dbo.tbl_UserUnits SET BattleShipCount=0 "
         "WHERE AccountNumber=? AND UserUnitNumber=?"),
        ("TimeLimit", "UPDATE dbo.tbl_UserUnits SET TimeLimit=1 "
                      "WHERE AccountNumber=? AND UserUnitNumber=?",
         "UPDATE dbo.tbl_UserUnits SET TimeLimit=0 "
         "WHERE AccountNumber=? AND UserUnitNumber=?"),
        ("RunOut", "UPDATE dbo.tbl_UserUnits SET RunOut=1 "
                   "WHERE AccountNumber=? AND UserUnitNumber=?",
         "UPDATE dbo.tbl_UserUnits SET RunOut=0 "
         "WHERE AccountNumber=? AND UserUnitNumber=?"),
        ("Forbidden", "UPDATE dbo.tbl_UserUnits SET Forbidden=1 "
                      "WHERE AccountNumber=? AND UserUnitNumber=?",
         "UPDATE dbo.tbl_UserUnits SET Forbidden=0 "
         "WHERE AccountNumber=? AND UserUnitNumber=?"),
        ("DateEnd", "UPDATE dbo.tbl_UserUnits SET DateEnd=GETDATE() "
                    "WHERE AccountNumber=? AND UserUnitNumber=?",
         "UPDATE dbo.tbl_UserUnits SET DateEnd=NULL "
         "WHERE AccountNumber=? AND UserUnitNumber=?"),
    ]
    for name, apply_sql, clear_sql in invariant_cases:
        connection.cursor().execute(apply_sql, SENDER, candidate)
        assert offer(connection, candidate, name)[0] == 7
        connection.cursor().execute(clear_sql, SENDER, candidate)
    connection.cursor().execute(
        "UPDATE dbo.tbl_Users SET GoUserUnitNum=? WHERE AccountNumber=?",
        candidate, SENDER,
    )
    assert offer(connection, candidate, "active")[0] == 7
    connection.cursor().execute(
        "UPDATE dbo.tbl_Users SET GoUserUnitNum=0,AssistUserUnitNum=? "
        "WHERE AccountNumber=?", candidate, SENDER
    )
    assert offer(connection, candidate, "assist")[0] == 7
    connection.cursor().execute(
        "UPDATE dbo.tbl_Users SET AssistUserUnitNum=0 WHERE AccountNumber=?",
        SENDER,
    )
    connection.cursor().execute(
        "UPDATE dbo.tbl_UserBattleShips SET UserUnitNumber_1=? "
        "WHERE AccountNumber=?", candidate, SENDER
    )
    assert offer(connection, candidate, "ship")[0] == 7
    connection.cursor().execute(
        "UPDATE dbo.tbl_UserBattleShips SET UserUnitNumber_1=NULL "
        "WHERE AccountNumber=?", SENDER
    )
    results["all_fixed_unit_invariants_rejected"] = True

    connection.cursor().execute(
        "EXEC [sdgo_friend_unit_gift].[SetPolicy] 1,20,1,0,1"
    )
    connection.cursor().execute(
        "UPDATE dbo.tbl_Users SET ExpandSlotCount=20 WHERE AccountNumber=?",
        SENDER,
    )

    # The global Mod transaction lock makes these races deterministic and safe.
    race_slot = int(connection.cursor().execute(
        "SELECT MIN(slots.UserUnitSlotNumber) "
        "FROM dbo.tbl_UserUnitSlotNumberList slots "
        "WHERE NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned "
        "WHERE owned.AccountNumber=? "
        "AND owned.UserUnitSlotNumber=slots.UserUnitSlotNumber)",
        SENDER,
    ).fetchone()[0])
    race_uid = insert_unit(connection, default_unit(SENDER, race_slot, 9990))
    refresh_counts(connection, SENDER)
    race_offer = offer(connection, race_uid, "race")
    assert race_offer[0] == 0
    race_gift = int(race_offer[1])

    def race_call(proc: str):
        with connect(config, DATABASE) as race_connection:
            return exec_result(
                race_connection, proc,
                [RECIPIENT, str(uuid.uuid4()), race_gift],
            )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        race_results = list(executor.map(race_call, ["Accept", "Return"]))
    assert sorted(result[0] for result in race_results) in ([0, 10], [10, 0])
    final_state = int(connection.cursor().execute(
        "SELECT State FROM [sdgo_friend_unit_gift].[GiftOffer] WHERE GiftId=?",
        race_gift,
    ).fetchone()[0])
    assert final_state in (1, 3)
    assert connection.cursor().execute(
        "SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE UserUnitNumber=?",
        race_uid,
    ).fetchone()[0] == 1
    results["accept_return_race_single_resolution"] = True

    double_accept_slot = int(connection.cursor().execute(
        "SELECT MIN(slots.UserUnitSlotNumber) "
        "FROM dbo.tbl_UserUnitSlotNumberList slots "
        "WHERE NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned "
        "WHERE owned.AccountNumber=? "
        "AND owned.UserUnitSlotNumber=slots.UserUnitSlotNumber)",
        SENDER,
    ).fetchone()[0])
    double_accept_uid = insert_unit(
        connection, default_unit(SENDER, double_accept_slot, 9990)
    )
    refresh_counts(connection, SENDER)
    double_accept_offer = offer(connection, double_accept_uid, "double accept")
    assert double_accept_offer[0] == 0
    double_accept_gift = int(double_accept_offer[1])

    def accept_once(_):
        with connect(config, DATABASE) as race_connection:
            return exec_result(
                race_connection, "Accept",
                [RECIPIENT, str(uuid.uuid4()), double_accept_gift],
            )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        double_accept_results = list(executor.map(accept_once, range(2)))
    assert sorted(result[0] for result in double_accept_results) == [0, 10]
    assert connection.cursor().execute(
        "SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE UserUnitNumber=?",
        double_accept_uid,
    ).fetchone()[0] == 1
    results["double_accept_single_resolution"] = True

    double_offer_slot = int(connection.cursor().execute(
        "SELECT MIN(slots.UserUnitSlotNumber) "
        "FROM dbo.tbl_UserUnitSlotNumberList slots "
        "WHERE NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned "
        "WHERE owned.AccountNumber=? "
        "AND owned.UserUnitSlotNumber=slots.UserUnitSlotNumber)",
        SENDER,
    ).fetchone()[0])
    double_offer_uid = insert_unit(
        connection, default_unit(SENDER, double_offer_slot, 9990)
    )
    refresh_counts(connection, SENDER)

    def offer_once(_):
        with connect(config, DATABASE) as race_connection:
            return exec_result(
                race_connection, "Offer",
                [SENDER, str(uuid.uuid4()), RECIPIENT, double_offer_uid, "double"],
            )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        double_offer_results = list(executor.map(offer_once, range(2)))
    assert sorted(result[0] for result in double_offer_results) == [0, 6]
    assert connection.cursor().execute(
        "SELECT COUNT(*) FROM [sdgo_friend_unit_gift].[EscrowUnit] "
        "WHERE UserUnitNumber=?",
        double_offer_uid,
    ).fetchone()[0] == 1
    results["double_offer_same_uid_single_escrow"] = True

    capacity_uids = []
    for unit_number in (9990, 9993):
        slot = int(connection.cursor().execute(
            "SELECT MIN(slots.UserUnitSlotNumber) "
            "FROM dbo.tbl_UserUnitSlotNumberList slots "
            "WHERE NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned "
            "WHERE owned.AccountNumber=? "
            "AND owned.UserUnitSlotNumber=slots.UserUnitSlotNumber)",
            SENDER,
        ).fetchone()[0])
        uid = insert_unit(connection, default_unit(SENDER, slot, unit_number))
        refresh_counts(connection, SENDER)
        offered = offer(connection, uid, "capacity")
        assert offered[0] == 0
        capacity_uids.append((uid, int(offered[1])))
    connection.cursor().execute(
        "UPDATE dbo.tbl_Users SET ExpandSlotCount=0 WHERE AccountNumber=?",
        RECIPIENT,
    )
    occupied = {
        int(row[0]) for row in connection.cursor().execute(
            "SELECT UserUnitSlotNumber FROM dbo.tbl_UserUnits "
            "WHERE AccountNumber=?", RECIPIENT
        ).fetchall()
    }
    while len(occupied) < 23:
        slot = next(
            value for value in range(1, 25) if value not in occupied
        )
        insert_unit(connection, default_unit(RECIPIENT, slot, 9991))
        occupied.add(slot)
    refresh_counts(connection, RECIPIENT)

    def capacity_accept(item):
        with connect(config, DATABASE) as race_connection:
            return exec_result(
                race_connection, "Accept",
                [RECIPIENT, str(uuid.uuid4()), item[1]],
            )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        capacity_results = list(executor.map(capacity_accept, capacity_uids))
    assert sorted(result[0] for result in capacity_results) == [0, 8]
    assert connection.cursor().execute(
        "SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber=?",
        RECIPIENT,
    ).fetchone()[0] == 24
    results["capacity_race_one_accept_one_full"] = True
    return results


def verify_install_guards(
    connection, config: dict, install_path: Path
) -> dict:
    succeeded, detail = execute_install(config, install_path)
    assert succeeded, detail
    connection.cursor().execute(
        "ALTER PROCEDURE [sdgo_friend_unit_gift].[QueryInbox] @ActorAccount int "
        "AS BEGIN SET NOCOUNT ON; SELECT 1 AS edited; END"
    )
    edited_succeeded, _ = execute_install(config, install_path)
    edited_refused = not edited_succeeded
    assert edited_refused
    definition = connection.cursor().execute(
        "SELECT Definition FROM [sdgo_friend_unit_gift].[ModuleState] "
        "WHERE ObjectName=N'QueryInbox'"
    ).fetchone()[0]
    connection.cursor().execute("ALTER" + definition[6:])
    connection.cursor().execute(
        "CREATE TABLE [sdgo_friend_unit_gift].[ForeignObject](value int NULL)"
    )
    foreign_succeeded, _ = execute_install(config, install_path)
    foreign_refused = not foreign_succeeded
    assert foreign_refused
    connection.cursor().execute(
        "DROP TABLE [sdgo_friend_unit_gift].[ForeignObject]"
    )
    connection.cursor().execute(
        "CREATE TRIGGER [sdgo_friend_unit_gift].[ForeignTrigger] "
        "ON [sdgo_friend_unit_gift].[Policy] AFTER UPDATE AS "
        "BEGIN SET NOCOUNT ON; END"
    )
    trigger_succeeded, _ = execute_install(config, install_path)
    trigger_refused = not trigger_succeeded
    assert trigger_refused
    connection.cursor().execute(
        "DROP TRIGGER [sdgo_friend_unit_gift].[ForeignTrigger]"
    )
    connection.cursor().execute(
        "EXEC sys.sp_updateextendedproperty @name=N'sdgo_owner',"
        "@value=N'foreign',@level0type=N'SCHEMA',"
        "@level0name=N'sdgo_friend_unit_gift'"
    )
    owner_succeeded, _ = execute_install(config, install_path)
    owner_refused = not owner_succeeded
    assert owner_refused
    connection.cursor().execute(
        "EXEC sys.sp_updateextendedproperty @name=N'sdgo_owner',"
        "@value=N'friend_unit_gift_server/v1',@level0type=N'SCHEMA',"
        "@level0name=N'sdgo_friend_unit_gift'"
    )
    succeeded, detail = execute_install(config, install_path)
    assert succeeded, detail
    return {
        "edited_module_refused": True,
        "foreign_schema_object_refused": True,
        "foreign_trigger_refused": True,
        "owner_marker_drift_refused": True,
        "exact_reinstall_succeeds": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--owner-ini", required=True, type=Path)
    parser.add_argument("--install-sql", required=True, type=Path)
    parser.add_argument("--fixture-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    public_config = json.loads(args.config.read_text(encoding="utf-8"))
    if int(public_config["sql_port"]) != EXPECTED_SQL_PORT:
        raise RuntimeError("private native configuration points at another port")
    config = load_owner_config(args.owner_ini)
    args.install_sql.read_text(encoding="utf-8")
    data, log = ensure_local_fixture_paths(args.fixture_dir)
    owner = str(uuid.uuid4())
    report = {
        "database": DATABASE,
        "sql_pid": EXPECTED_SQL_PID,
        "sql_port": EXPECTED_SQL_PORT,
        "passed": False,
        "checks": {},
        "cleanup": False,
    }
    created = False
    failure: BaseException | None = None
    try:
        report["stage"] = "create_database"
        create_database(config, data, log, owner)
        created = True
        with connect(config, DATABASE) as connection:
            report["stage"] = "create_native_schema"
            create_native_schema(connection)
            report["stage"] = "seed_fixture"
            seeded = seed_fixture(connection)
            original = row_payload(connection, SENDER, seeded["gift_uid"])
            report["stage"] = "install_policy"
            succeeded, detail = execute_install(config, args.install_sql)
            if not succeeded:
                raise RuntimeError("install execution failed: " + detail)
            report["stage"] = "policy_cases"
            report["checks"].update(test_policy(connection, config, original))
            report["stage"] = "install_guards"
            report["checks"].update(
                verify_install_guards(connection, config, args.install_sql)
            )
        report["stage"] = "complete"
        report["passed"] = True
    except BaseException as error:
        failure = error
        report["error_type"] = type(error).__name__
        report["error"] = str(error)[:1000]
        report["traceback"] = traceback.format_exc(limit=4)[-3000:]
    finally:
        if created:
            try:
                guarded_drop_database(config, data, log, owner)
                report["cleanup"] = True
            except BaseException as cleanup_error:
                report["cleanup_error_type"] = type(cleanup_error).__name__
                report["cleanup_error"] = str(cleanup_error)[:1000]
                if failure is None:
                    failure = cleanup_error
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    if failure is not None:
        print(f"friend unit gift SQL fixture failed: {type(failure).__name__}")
        return 1
    print("friend_unit_gift_sql_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
