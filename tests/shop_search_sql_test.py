"""Explicit opt-in SQL Server 2014 smoke test for the shop search installer.

Creates and drops only a uniquely named SDGO_ShopSearch_Test_* database with
synthetic rows. Production game databases are never read or written.
"""
import argparse
import base64
import configparser
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import tempfile


def ident(value):
    return "[" + value.replace("]", "]]") + "]"


def load_config(path):
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = raw.decode("utf-16")
    else:
        text = raw.decode("utf-8-sig")
    cfg = configparser.ConfigParser(interpolation=None, strict=False)
    cfg.read_string(text)
    return cfg


def connection(cfg, *, database):
    import pyodbc
    section = cfg["DB StoreBilling"]

    def esc(value):
        return "{" + str(value).replace("}", "}}") + "}"

    text = (
        "DRIVER={SQL Server};SERVER=" + esc(section["DBServerName"])
        + ";DATABASE=" + esc(database)
        + ";UID=" + esc(section["ID"])
        + ";PWD=" + esc(section["PWD"]) + ";"
    )
    cn = pyodbc.connect(text, timeout=5, autocommit=True)
    cn.timeout = 30
    return cn


def all_results(cursor):
    result = []
    while True:
        if cursor.description:
            names = [column[0] for column in cursor.description]
            result.append([dict(zip(names, row)) for row in cursor.fetchall()])
        if not cursor.nextset():
            return result


def query_shop(cursor, depth, page, size, rank=-1, sub=-1, payment=-1,
               term=""):
    return query_procedure(
        cursor, "sp_StoreItemsList", depth, page, size, rank, sub, payment,
        term,
    )


def query_sub(cursor, depth, page, size, rank=-1, sub=-1, payment=-1,
              term=""):
    return query_procedure(
        cursor, "sp_StoreSubItemsList", depth, page, size, rank, sub,
        payment, term,
    )


def query_procedure(cursor, procedure, depth, page, size, rank=-1, sub=-1,
                    payment=-1, term=""):
    if procedure not in {"sp_StoreItemsList", "sp_StoreSubItemsList"}:
        raise AssertionError("unsafe procedure fixture")
    cursor.execute(
        f"""
        DECLARE @total INT;
        EXEC dbo.{procedure} ?,?,?,?,?,?,?,@total OUTPUT;
        SELECT @total AS total;
        """,
        depth, page, size, rank, sub, payment, term,
    )
    sets = all_results(cursor)
    if len(sets) != 2:
        raise AssertionError("unexpected procedure result-set count")
    return sets[0], sets[1][0]["total"]


def cn_wire(value):
    payload = base64.b64encode(value.encode("utf-16le")).decode("ascii")
    return "~CN:" + payload


def ordered_product_ids(rows):
    result = []
    for row in rows:
        product = row["f_productid"]
        if not result or result[-1] != product:
            result.append(product)
    return result


def emit(builder, expected_main, expected_sub, output, revision):
    with expected_main.open("w", encoding="utf-8", newline="") as stream:
        stream.write(expected_main.definition)
    with expected_sub.open("w", encoding="utf-8", newline="") as stream:
        stream.write(expected_sub.definition)
    result = subprocess.run(
        [
            str(builder), "--emit", str(expected_main), str(expected_sub),
            str(output), revision,
        ],
        capture_output=True, text=True, encoding="utf-8", timeout=30,
    )
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


class DefinitionPath:
    def __init__(self, path, definition):
        self.path = path
        self.definition = definition

    def open(self, *args, **kwargs):
        return self.path.open(*args, **kwargs)

    def __str__(self):
        return str(self.path)

    def __fspath__(self):
        return str(self.path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ini", type=Path, required=True)
    parser.add_argument("--builder", type=Path, required=True)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)

    cfg = load_config(args.server_ini)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    database = "SDGO_ShopSearch_Test_" + stamp + "_" + secrets.token_hex(3)
    if not re.fullmatch(r"SDGO_ShopSearch_Test_[A-Za-z0-9_]+", database):
        raise AssertionError("unsafe database identifier")

    receipt = {
        "database": database,
        "scope": "unique synthetic test database only",
        "checks": [],
    }
    source = connection(cfg, database=cfg["DB StoreBilling"]["DBName"])
    source_cursor = source.cursor()
    source_cursor.execute(
        "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList')),"
        "OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreSubItemsList'))"
    )
    native_main, native_sub = source_cursor.fetchone()
    if hashlib.sha256(native_main.encode("utf-8")).hexdigest() != (
        "595974c93de41196927d7cc665d6154a9932713182d0c1824246d57d579a43b6"
    ):
        source_cursor.execute(
            "SELECT OriginalDefinition FROM dbo.sdgo_ShopSearchBackup "
            "WHERE ObjectName=N'dbo.sp_StoreItemsList'"
        )
        native_main = source_cursor.fetchone()[0]
    source.close()
    if (
        hashlib.sha256(native_main.encode("utf-8")).hexdigest()
        != "595974c93de41196927d7cc665d6154a9932713182d0c1824246d57d579a43b6"
        or hashlib.sha256(native_sub.encode("utf-8")).hexdigest()
        != "8857d2f6e806b7fc394369bb73fd5f71b5bf442ed56b2f827d76a0fd0027a0dc"
    ):
        raise AssertionError("native search definitions do not match helper allowlist")
    admin = connection(cfg, database="master")
    target = None
    helper_ini = None

    def check(condition, name):
        if not condition:
            raise AssertionError(name)
        receipt["checks"].append(name)
        print("PASS", name, flush=True)

    try:
        admin.cursor().execute("CREATE DATABASE " + ident(database))
        target = connection(cfg, database=database)
        cursor = target.cursor()
        cursor.execute(
            """
            CREATE TABLE dbo.tbls_StoreItems(
                f_productid INT NOT NULL PRIMARY KEY,
                f_itemid VARCHAR(16) NOT NULL,
                f_subitemCnt SMALLINT NOT NULL,
                f_depth3 SMALLINT NOT NULL,
                f_price INT NOT NULL,
                f_payment SMALLINT NOT NULL,
                f_limitedAdition SMALLINT NOT NULL,
                f_quantity INT NOT NULL,
                f_package SMALLINT NOT NULL,
                f_limitedGift SMALLINT NOT NULL,
                f_display SMALLINT NOT NULL,
                f_depth0 SMALLINT NOT NULL,
                f_depth1 SMALLINT NOT NULL,
                f_search1 SMALLINT NOT NULL,
                f_search2 SMALLINT NOT NULL,
                f_itemNm VARCHAR(50) COLLATE Thai_CI_AS NOT NULL,
                f_regdate DATETIME NOT NULL
            );
            CREATE TABLE dbo.tbls_SubItems(
                f_itemtblidx INT NOT NULL PRIMARY KEY,
                f_itemid VARCHAR(16) NOT NULL,
                f_price INT NOT NULL,
                f_itemNm NVARCHAR(50) NULL,
                f_validity INT NOT NULL,
                f_payment CHAR(1) NOT NULL,
                f_categoryNumber INT NULL,
                f_index CHAR(1) NULL
            );
            INSERT dbo.tbls_StoreItems VALUES
            (1010101,'A1',2,1,100,1,0,1,1,0,1,2,1,3,7,'alpha gundam','20260108'),
            (1010102,'A2',1,2,200,2,0,1,2,0,1,2,1,3,8,'Beta O''Hara','20260107'),
            (1010103,'A3',1,3,300,1,0,1,3,0,1,2,1,2,7,
             'Gamma Quote ''; DROP TABLE dbo.tbls_StoreItems;--','20260106'),
            (1010104,'A4',1,2,400,1,0,1,4,0,1,2,1,2,8,'Delta Unit','20260105'),
            (1010105,'A5',1,1,500,2,0,1,5,0,1,2,1,1,7,'Echo Unit','20260104'),
            (1010106,'A6',1,3,600,1,0,1,6,0,1,2,1,1,8,'Foxtrot Unit','20260103'),
            (1010107,'A7',1,2,700,2,0,1,7,0,1,2,20,3,7,'Event Unit','20260102'),
            (2010101,'B1',1,1,800,1,0,1,8,0,1,2,2,3,7,'Other Category','20260101');
            INSERT dbo.tbls_SubItems VALUES
            (1101,'A1',100,N'Alpha permanent',1,'1',1120022,'2'),
            (1102,'A1',90,N'Alpha rental',7,'1',1110549,'1'),
            (1201,'A2',200,N'Beta',1,'2',1120022,'1'),
            (1301,'A3',300,N'Gamma',1,'1',1120022,'1'),
            (1401,'A4',400,N'Delta',1,'1',1120022,'1'),
            (1501,'A5',500,N'Echo',1,'2',1120022,'1'),
            (1601,'A6',600,N'Foxtrot',1,'1',1120022,'1'),
            (1701,'A7',700,N'Event',1,'2',1120022,'1'),
            (2101,'B1',800,N'Other',1,'1',1120022,'1');

            EXEC(N'CREATE PROCEDURE dbo.sp_StoreItemsList
                @depth1 SMALLINT,@PageNm INT,@PageSize INT,
                @search1 SMALLINT,@search2 SMALLINT,@search3 SMALLINT,
                @search4 VARCHAR(100),@TotalCnt INT OUTPUT
            AS
            BEGIN
                SET NOCOUNT ON;
                SET @TotalCnt=0;
                SELECT TOP (0) f_productid,f_itemid,f_subitemCnt,f_depth3,
                    f_price,f_payment,f_limitedAdition,f_quantity,f_package,
                    f_limitedGift
                FROM dbo.tbls_StoreItems;
            END');
            EXEC(N'CREATE PROCEDURE dbo.sp_StoreSubItemsList
                @depth1 SMALLINT,@PageNm INT,@PageSize INT,
                @search1 SMALLINT,@search2 SMALLINT,@search3 SMALLINT,
                @search4 VARCHAR(50),@TotalCnt INT OUTPUT
            AS
            BEGIN
                SET NOCOUNT ON;
                SET @TotalCnt=0;
                SELECT TOP (0)
                    S.f_productid,B.f_itemtblidx,B.f_price,B.f_itemNm,
                    B.f_validity,B.f_payment,B.f_categoryNumber,S.f_package
                FROM dbo.tbls_StoreItems S
                JOIN dbo.tbls_SubItems B ON B.f_itemid=S.f_itemid;
            END');
            """
        )
        cursor.execute(
            "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList'))"
        )
        baseline_main = cursor.fetchone()[0]
        cursor.execute(
            "SELECT OBJECT_DEFINITION("
            "OBJECT_ID(N'dbo.sp_StoreSubItemsList'))"
        )
        baseline_sub = cursor.fetchone()[0]
        cursor.execute(
            "SELECT * FROM dbo.tbls_StoreItems ORDER BY f_productid"
        )
        product_before = hashlib.sha256(
            repr(cursor.fetchall()).encode("utf-8")
        ).hexdigest()
        cursor.execute(
            "SELECT * FROM dbo.tbls_SubItems ORDER BY f_itemtblidx"
        )
        subitem_before = hashlib.sha256(
            repr(cursor.fetchall()).encode("utf-8")
        ).hexdigest()

        expected_main_file = args.output / "expected-main.sql"
        expected_sub_file = args.output / "expected-sub.sql"
        install_file = args.output / "install.sql"
        expected_main = DefinitionPath(expected_main_file, baseline_main)
        expected_sub = DefinitionPath(expected_sub_file, baseline_sub)
        emit(
            args.builder.resolve(), expected_main, expected_sub,
            install_file, "smoke-1",
        )
        cursor.execute(install_file.read_text(encoding="utf-8"))

        cursor.execute("SELECT COUNT(*) FROM dbo.sdgo_ShopSearchAliases")
        check(cursor.fetchone()[0] == 4, "canonical_alias_rows_installed")
        cursor.execute(
            "SELECT COUNT(*) FROM dbo.sdgo_ShopSearchState "
            "WHERE ObjectName IN("
            "N'dbo.sp_StoreItemsList',N'dbo.sp_StoreSubItemsList')"
        )
        check(cursor.fetchone()[0] == 2, "both_procedure_states_installed")
        cursor.execute(
            "SELECT COUNT(*) FROM dbo.sdgo_ShopSearchBackup "
            "WHERE ObjectName IN("
            "N'dbo.sp_StoreItemsList',N'dbo.sp_StoreSubItemsList')"
        )
        check(cursor.fetchone()[0] == 2, "both_native_definitions_backed_up")
        cursor.execute(
            "SELECT COUNT(*) FROM sys.extended_properties "
            "WHERE class=1 AND minor_id=0 AND name=N'sdgo_owner' "
            "AND CONVERT(NVARCHAR(128),value)=N'shop_search/v2' "
            "AND major_id IN("
            "OBJECT_ID(N'dbo.sdgo_ShopSearchAliases'),"
            "OBJECT_ID(N'dbo.sdgo_ShopSearchState'),"
            "OBJECT_ID(N'dbo.sdgo_ShopSearchBackup'))"
        )
        check(cursor.fetchone()[0] == 3, "fresh_install_owns_all_tables_as_v2")

        rows, total = query_shop(cursor, 1, 1, 10, term=cn_wire("阿尔"))
        children, child_total = query_sub(
            cursor, 1, 1, 10, term=cn_wire("阿尔"),
        )
        check(
            total == child_total == 1
            and ordered_product_ids(rows) == ordered_product_ids(children)
            == [1010101],
            "Chinese_alias_parent_and_child_selection_match",
        )
        rows, total = query_shop(cursor, 1, 1, 10, term="ALPHA")
        children, child_total = query_sub(
            cursor, 1, 1, 10, term="ALPHA",
        )
        check(
            total == child_total == 1
            and ordered_product_ids(rows) == ordered_product_ids(children)
            == [1010101],
            "English_parent_and_child_selection_match",
        )
        rows, total = query_shop(cursor, 0, 1, 10, rank=3)
        children, child_total = query_sub(cursor, 0, 1, 10, rank=3)
        check(
            total == child_total == 3
            and ordered_product_ids(rows) == ordered_product_ids(children)
            and 1010107 not in set(ordered_product_ids(children)),
            "global_rank_parent_and_child_selection_match",
        )
        rows, total = query_shop(cursor, 0, 1, 10, payment=2)
        children, child_total = query_sub(cursor, 0, 1, 10, payment=2)
        check(
            total == child_total == 2
            and ordered_product_ids(rows) == ordered_product_ids(children),
            "global_currency_parent_and_child_selection_match",
        )
        hot, hot_total = query_shop(cursor, 99, 1, 20, rank=3)
        hot_children, hot_child_total = query_sub(
            cursor, 99, 1, 20, rank=3,
        )
        check(
            hot_total == hot_child_total == 2
            and ordered_product_ids(hot) == ordered_product_ids(hot_children)
            and 1010107 in set(ordered_product_ids(hot_children)),
            "hot_parent_and_child_selection_match",
        )

        first, total1 = query_shop(cursor, 0, 1, 2)
        second, total2 = query_shop(cursor, 0, 2, 2)
        first_children, child_total1 = query_sub(cursor, 0, 1, 2)
        second_children, child_total2 = query_sub(cursor, 0, 2, 2)
        first_ids = ordered_product_ids(first)
        second_ids = ordered_product_ids(second)
        check(
            total1 == total2 == child_total1 == child_total2 == 7
            and first_ids == ordered_product_ids(first_children)
            and second_ids == ordered_product_ids(second_children)
            and not set(first_ids).intersection(second_ids),
            "one_based_pages_match_between_parent_and_child_procedures",
        )
        check(
            list(first_children[0].keys()) == [
                "f_productid", "f_itemtblidx", "f_price", "f_itemNm",
                "f_validity", "f_payment", "f_categoryNumber", "f_package",
            ]
            and first_children[0]["f_package"] == 1
            and [
                row["f_itemtblidx"] for row in first_children
                if row["f_productid"] == 1010101
            ] == [1101, 1102],
            "sub_result_contract_package_and_child_order_preserved",
        )

        cursor.execute(
            "SELECT ObjectName,OriginalDefinition "
            "FROM dbo.sdgo_ShopSearchBackup "
            "ORDER BY ObjectName"
        )
        backups = dict(cursor.fetchall())
        cursor.execute(
            "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList'))"
        )
        installed_main = cursor.fetchone()[0]
        cursor.execute(
            "SELECT OBJECT_DEFINITION("
            "OBJECT_ID(N'dbo.sp_StoreSubItemsList'))"
        )
        installed_sub = cursor.fetchone()[0]

        for table in [
            "sdgo_ShopSearchAliases",
            "sdgo_ShopSearchState",
            "sdgo_ShopSearchBackup",
        ]:
            cursor.execute(
                "EXEC sys.sp_updateextendedproperty "
                "@name=N'sdgo_owner',@value=N'shop_search/v1',"
                "@level0type=N'SCHEMA',@level0name=N'dbo',"
                "@level1type=N'TABLE',@level1name=?",
                table,
            )
        cursor.execute(
            "DELETE FROM dbo.sdgo_ShopSearchState "
            "WHERE ObjectName=N'dbo.sp_StoreSubItemsList';"
            "DELETE FROM dbo.sdgo_ShopSearchBackup "
            "WHERE ObjectName=N'dbo.sp_StoreSubItemsList';"
        )
        cursor.execute(
            re.sub(
                r"^\s*CREATE\s+(?:PROC|PROCEDURE)\s+",
                "ALTER PROCEDURE ",
                baseline_sub,
                count=1,
                flags=re.IGNORECASE,
            )
        )
        expected_main.definition = installed_main
        expected_sub.definition = baseline_sub
        emit(
            args.builder.resolve(), expected_main, expected_sub,
            install_file, "smoke-v1-migration",
        )
        cursor.execute(install_file.read_text(encoding="utf-8"))
        cursor.execute(
            "SELECT COUNT(*) FROM dbo.sdgo_ShopSearchState "
            "WHERE Revision=N'smoke-v1-migration' AND ObjectName IN("
            "N'dbo.sp_StoreItemsList',N'dbo.sp_StoreSubItemsList')"
        )
        check(cursor.fetchone()[0] == 2, "main_only_v1_install_migrates_both_states")
        cursor.execute(
            "SELECT COUNT(*) FROM sys.extended_properties "
            "WHERE class=1 AND minor_id=0 AND name=N'sdgo_owner' "
            "AND CONVERT(NVARCHAR(128),value)=N'shop_search/v2' "
            "AND major_id IN("
            "OBJECT_ID(N'dbo.sdgo_ShopSearchAliases'),"
            "OBJECT_ID(N'dbo.sdgo_ShopSearchState'),"
            "OBJECT_ID(N'dbo.sdgo_ShopSearchBackup'))"
        )
        check(cursor.fetchone()[0] == 3, "v1_migration_upgrades_all_owners_to_v2")
        cursor.execute(
            "SELECT ObjectName,OriginalDefinition "
            "FROM dbo.sdgo_ShopSearchBackup ORDER BY ObjectName"
        )
        migrated_backups = dict(cursor.fetchall())
        check(
            migrated_backups["dbo.sp_StoreItemsList"]
            == backups["dbo.sp_StoreItemsList"] == baseline_main
            and migrated_backups["dbo.sp_StoreSubItemsList"] == baseline_sub,
            "first_native_backups_preserved_across_v1_migration",
        )

        cursor.execute(
            "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList'))"
        )
        installed_main = cursor.fetchone()[0]
        cursor.execute(
            "SELECT OBJECT_DEFINITION("
            "OBJECT_ID(N'dbo.sp_StoreSubItemsList'))"
        )
        installed_sub = cursor.fetchone()[0]
        expected_main.definition = installed_main
        expected_sub.definition = installed_sub
        emit(
            args.builder.resolve(), expected_main, expected_sub,
            install_file, "smoke-idempotent",
        )
        cursor.execute(install_file.read_text(encoding="utf-8"))
        cursor.execute(
            "SELECT COUNT(*) FROM dbo.sdgo_ShopSearchState "
            "WHERE Revision=N'smoke-idempotent'"
        )
        check(cursor.fetchone()[0] == 2,
              "idempotent_reinstall_updates_both_revisions")
        cursor.execute(
            "SELECT ObjectName,OriginalDefinition "
            "FROM dbo.sdgo_ShopSearchBackup ORDER BY ObjectName"
        )
        check(
            dict(cursor.fetchall()) == migrated_backups,
            "idempotent_reinstall_preserves_both_native_backups",
        )

        cursor.execute(
            "SELECT Revision,InstalledDefinition,UpdatedAt "
            "FROM dbo.sdgo_ShopSearchState "
            "WHERE ObjectName=N'dbo.sp_StoreSubItemsList'"
        )
        saved_sub_state = cursor.fetchone()
        cursor.execute(
            "DELETE FROM dbo.sdgo_ShopSearchState "
            "WHERE ObjectName=N'dbo.sp_StoreSubItemsList'"
        )
        refused = False
        try:
            cursor.execute(install_file.read_text(encoding="utf-8"))
        except Exception:
            refused = True
        check(refused, "v2_missing_sub_state_refused")
        cursor.execute(
            "INSERT dbo.sdgo_ShopSearchState("
            "ObjectName,Revision,InstalledDefinition,UpdatedAt)"
            "VALUES(N'dbo.sp_StoreSubItemsList',?,?,?)",
            saved_sub_state[0], saved_sub_state[1], saved_sub_state[2],
        )

        cursor.execute(
            """
            DECLARE @refused BIT=0;
            BEGIN TRY
                BEGIN TRANSACTION;
                DECLARE @owner NVARCHAR(128);
                SELECT @owner=CONVERT(NVARCHAR(128),value)
                FROM sys.extended_properties
                WHERE class=1 AND minor_id=0 AND name=N'sdgo_owner'
                  AND major_id=OBJECT_ID(N'dbo.sdgo_ShopSearchAliases');
                IF ISNULL(@owner,N'')<>N'shop_search/v1'
                    THROW 51007,'old v1 helper ownership gate',1;
                ROLLBACK TRANSACTION;
            END TRY
            BEGIN CATCH
                IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;
                SET @refused=1;
            END CATCH;
            SELECT @refused;
            """
        )
        check(cursor.fetchone()[0] == 1, "old_v1_helper_refuses_v2_owners")

        cursor.execute(
            "SELECT ProductId,OriginalName,Name "
            "FROM dbo.sdgo_ShopSearchAliases "
            "ORDER BY ProductId,OriginalName"
        )
        aliases_before_external_edit = cursor.fetchall()
        cursor.execute(
            "ALTER PROCEDURE dbo.sp_StoreSubItemsList "
            "@depth1 SMALLINT,@PageNm INT,@PageSize INT,@search1 SMALLINT,"
            "@search2 SMALLINT,@search3 SMALLINT,@search4 VARCHAR(100),"
            "@TotalCnt INT OUTPUT AS BEGIN SET NOCOUNT ON; "
            "SET @TotalCnt=123; END"
        )
        cursor.execute(
            "SELECT OBJECT_DEFINITION("
            "OBJECT_ID(N'dbo.sp_StoreSubItemsList'))"
        )
        external_sub = cursor.fetchone()[0]
        expected_sub.definition = external_sub
        emit(
            args.builder.resolve(), expected_main, expected_sub,
            install_file, "smoke-external-sub",
        )
        refused = False
        try:
            cursor.execute(install_file.read_text(encoding="utf-8"))
        except Exception:
            refused = True
        check(refused, "owned_external_sub_procedure_edit_refused")
        cursor.execute(
            "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList'))"
        )
        main_after_refusal = cursor.fetchone()[0]
        cursor.execute(
            "SELECT ProductId,OriginalName,Name "
            "FROM dbo.sdgo_ShopSearchAliases "
            "ORDER BY ProductId,OriginalName"
        )
        check(
            main_after_refusal == installed_main
            and cursor.fetchall() == aliases_before_external_edit,
            "external_sub_edit_rolls_back_alias_and_main_changes",
        )

        if args.helper:
            cursor.execute(
                "DROP PROCEDURE dbo.sp_StoreSubItemsList;"
                "DROP PROCEDURE dbo.sp_StoreItemsList;"
                "DROP TABLE dbo.sdgo_ShopSearchAliases;"
                "DROP TABLE dbo.sdgo_ShopSearchState;"
                "DROP TABLE dbo.sdgo_ShopSearchBackup;"
            )
            cursor.execute(native_main)
            cursor.execute(native_sub)
            private = configparser.ConfigParser(interpolation=None)
            private["DB StoreBilling"] = dict(cfg["DB StoreBilling"])
            private["DB StoreBilling"]["DBName"] = database
            handle = tempfile.NamedTemporaryFile(
                prefix="sdgo-shop-search-", suffix=".ini", delete=False,
            )
            helper_ini = Path(handle.name)
            handle.close()
            with helper_ini.open("w", encoding="utf-16") as stream:
                private.write(stream)
            helper_output = args.output / "helper"
            helper_output.mkdir()
            dictionary = helper_output / "zh-CN.ini"
            dictionary.write_text(
                "[entry:a]\nKind=name\n"
                "Source=@product:1010101:alpha gundam\n"
                "Text=高达阿尔法\n"
                "[entry:b]\nKind=name\n"
                "Source=@product:1010102:Beta O'Hara\n"
                "Text=测试机乙\n",
                encoding="utf-8",
            )
            applied = subprocess.run(
                [
                    str(args.helper.resolve()),
                    "--host-ini", str(helper_ini),
                    "--search-index", str(dictionary.resolve()),
                    "--output", str(helper_output.resolve()),
                ],
                capture_output=True, text=True, encoding="utf-8", timeout=45,
            )
            if applied.returncode:
                raise AssertionError(applied.stdout + applied.stderr)
            secret = cfg["DB StoreBilling"]["PWD"]
            check(
                secret not in applied.stdout + applied.stderr,
                "runtime_helper_does_not_log_credentials",
            )
            helper_receipt = json.loads(
                (helper_output / "search-index-result.json").read_text(
                    encoding="utf-8",
                )
            )
            cursor.execute(
                "SELECT COUNT(*) FROM dbo.sdgo_ShopSearchState S "
                "WHERE S.ObjectName IN("
                "N'dbo.sp_StoreItemsList',N'dbo.sp_StoreSubItemsList') "
                "AND S.InstalledDefinition=OBJECT_DEFINITION("
                "OBJECT_ID(S.ObjectName,N'P'))"
            )
            check(
                helper_receipt["installed"] is True
                and helper_receipt["name_count"] == 2
                and cursor.fetchone()[0] == 2,
                "runtime_helper_accepts_native_pair_and_reads_back_both",
            )
            rows, total = query_shop(cursor, 0, 1, 2)
            children, child_total = query_sub(cursor, 0, 1, 2)
            check(
                total == child_total == 7
                and ordered_product_ids(rows)
                == ordered_product_ids(children),
                "runtime_helper_installed_pair_returns_matching_global_page",
            )

        cursor.execute(
            "SELECT * FROM dbo.tbls_StoreItems ORDER BY f_productid"
        )
        product_after = hashlib.sha256(
            repr(cursor.fetchall()).encode("utf-8")
        ).hexdigest()
        cursor.execute(
            "SELECT * FROM dbo.tbls_SubItems ORDER BY f_itemtblidx"
        )
        subitem_after = hashlib.sha256(
            repr(cursor.fetchall()).encode("utf-8")
        ).hexdigest()
        check(
            product_after == product_before
            and subitem_after == subitem_before,
            "native_product_and_child_rows_unchanged",
        )
        receipt["result"] = "PASS"
        (args.output / "result.json").write_text(
            json.dumps(receipt, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        print("ISOLATED SHOP SEARCH SQL SMOKE PASSED", flush=True)
    finally:
        if helper_ini is not None:
            helper_ini.unlink(missing_ok=True)
        if target is not None:
            target.close()
        if re.fullmatch(r"SDGO_ShopSearch_Test_[A-Za-z0-9_]+", database):
            drop = admin.cursor()
            drop.execute(
                "IF DB_ID(?) IS NOT NULL BEGIN "
                "ALTER DATABASE " + ident(database)
                + " SET SINGLE_USER WITH ROLLBACK IMMEDIATE;"
                "DROP DATABASE " + ident(database) + ";END",
                database,
            )
        admin.close()


if __name__ == "__main__":
    main()
