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


def ident(value):
    return "[" + value.replace("]", "]]") + "]"


def load_config(path):
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = raw.decode("utf-16")
    else:
        text = raw.decode("utf-8-sig")
    cfg = configparser.ConfigParser(interpolation=None)
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
    cursor.execute(
        """
        DECLARE @total INT;
        EXEC dbo.sp_StoreItemsList ?,?,?,?,?,?,?,@total OUTPUT;
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


def emit(builder, expected, output, revision):
    with expected.open("w", encoding="utf-8", newline="") as stream:
        stream.write(expected.definition)
    result = subprocess.run(
        [str(builder), "--emit", str(expected), str(output), revision],
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
    admin = connection(cfg, database="master")
    target = None

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
            INSERT dbo.tbls_StoreItems VALUES
            (1010101,'A1',1,1,100,1,0,1,0,0,1,2,1,3,7,'alpha gundam','20260108'),
            (1010102,'A2',1,2,200,2,0,1,0,0,1,2,1,3,8,'Beta O''Hara','20260107'),
            (1010103,'A3',1,3,300,1,0,1,0,0,1,2,1,2,7,
             'Gamma Quote ''; DROP TABLE dbo.tbls_StoreItems;--','20260106'),
            (1010104,'A4',1,2,400,1,0,1,0,0,1,2,1,2,8,'Delta Unit','20260105'),
            (1010105,'A5',1,1,500,2,0,1,0,0,1,2,1,1,7,'Echo Unit','20260104'),
            (1010106,'A6',1,3,600,1,0,1,0,0,1,2,1,1,8,'Foxtrot Unit','20260103'),
            (1010107,'A7',1,2,700,2,0,1,0,0,1,2,20,3,7,'Event Unit','20260102'),
            (2010101,'B1',1,1,800,1,0,1,0,0,1,2,2,3,7,'Other Category','20260101');

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
            """
        )
        cursor.execute(
            "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList'))"
        )
        baseline = cursor.fetchone()[0]
        cursor.execute(
            "SELECT * FROM dbo.tbls_StoreItems ORDER BY f_productid"
        )
        product_before = hashlib.sha256(
            repr(cursor.fetchall()).encode("utf-8")
        ).hexdigest()

        expected_file = args.output / "expected.sql"
        install_file = args.output / "install.sql"
        expected = DefinitionPath(expected_file, baseline)
        emit(args.builder.resolve(), expected, install_file, "smoke-1")
        cursor.execute(install_file.read_text(encoding="utf-8"))

        cursor.execute("SELECT COUNT(*) FROM dbo.sdgo_ShopSearchAliases")
        check(cursor.fetchone()[0] == 4, "canonical_alias_rows_installed")

        rows, total = query_shop(cursor, 1, 1, 10, term=cn_wire("阿尔"))
        check(total == 1 and rows[0]["f_productid"] == 1010101,
              "Chinese_alias_partial_search")
        rows, total = query_shop(cursor, 1, 1, 10, term="ALPHA")
        check(total == 1 and rows[0]["f_productid"] == 1010101,
              "English_case_insensitive_search")
        rows, total = query_shop(cursor, 0, 1, 10, rank=3)
        check(total == 3 and 1010107 not in {r["f_productid"] for r in rows},
              "global_rank_filter_and_event_exclusion")
        hot, hot_total = query_shop(cursor, 99, 1, 20, rank=3)
        check(hot_total == 2 and 1010107 in {r["f_productid"] for r in hot},
              "hot_scope_uses_same_rank_filter")

        first, total1 = query_shop(cursor, 0, 1, 2)
        second, total2 = query_shop(cursor, 0, 2, 2)
        first_ids = {row["f_productid"] for row in first}
        second_ids = {row["f_productid"] for row in second}
        check(total1 == total2 == 7 and not first_ids.intersection(second_ids),
              "one_based_pagination_and_consistent_total")

        cursor.execute(
            "SELECT OriginalDefinition FROM dbo.sdgo_ShopSearchBackup "
            "WHERE ObjectName=N'dbo.sp_StoreItemsList'"
        )
        backup = cursor.fetchone()[0]
        cursor.execute(
            "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList'))"
        )
        installed = cursor.fetchone()[0]
        expected.definition = installed
        emit(args.builder.resolve(), expected, install_file, "smoke-2")
        cursor.execute(install_file.read_text(encoding="utf-8"))
        cursor.execute(
            "SELECT Revision FROM dbo.sdgo_ShopSearchState "
            "WHERE ObjectName=N'dbo.sp_StoreItemsList'"
        )
        check(cursor.fetchone()[0] == "smoke-2",
              "idempotent_reinstall_updates_revision")
        cursor.execute(
            "SELECT OriginalDefinition FROM dbo.sdgo_ShopSearchBackup "
            "WHERE ObjectName=N'dbo.sp_StoreItemsList'"
        )
        check(cursor.fetchone()[0] == backup == baseline,
              "first_native_backup_preserved")

        cursor.execute(
            "ALTER PROCEDURE dbo.sp_StoreItemsList "
            "@depth1 SMALLINT,@PageNm INT,@PageSize INT,@search1 SMALLINT,"
            "@search2 SMALLINT,@search3 SMALLINT,@search4 VARCHAR(100),"
            "@TotalCnt INT OUTPUT AS BEGIN SET NOCOUNT ON; "
            "SET @TotalCnt=123; END"
        )
        refused = False
        try:
            cursor.execute(install_file.read_text(encoding="utf-8"))
        except Exception:
            refused = True
        check(refused, "owned_external_procedure_edit_refused")

        cursor.execute(
            "SELECT * FROM dbo.tbls_StoreItems ORDER BY f_productid"
        )
        product_after = hashlib.sha256(
            repr(cursor.fetchall()).encode("utf-8")
        ).hexdigest()
        check(product_after == product_before, "native_product_rows_unchanged")
        receipt["result"] = "PASS"
        (args.output / "result.json").write_text(
            json.dumps(receipt, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        print("ISOLATED SHOP SEARCH SQL SMOKE PASSED", flush=True)
    finally:
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
