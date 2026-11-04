from __future__ import annotations

import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from owner.units import activity_repository
from owner.units.activity_repository import (
    ActivityRepository,
    CONDITION_COLUMNS,
    LEDGER_COLUMNS,
    LEDGER_SCHEMA,
    LIST_COLUMNS,
    OWNER_KEY,
    OWNER_PROPERTY,
    POOL_SIZE,
    PRICE_COLUMNS,
    PRIME_COLUMNS,
    SHARE_COLUMNS,
    STATE_TABLES,
    _NATIVE_SCHEMA,
    canonical_state_hash,
    expand_pool,
    normalize_config,
)
from owner.units.repository import SQL_SCHEMAS


ROOT_UNIT = 90001
MACHINE_ID = 1_990_001
TEMPLATE_ID = 9301
ROOT_CATEGORY = 1_190_001
OTHER_CATEGORY = 1_190_002
BLUEPRINT_CATEGORY = 1_590_001


def config(*, revision: str = "a" * 64, price: int = 99) -> dict:
    return {
        "schema_version": 1,
        "id": MACHINE_ID,
        "unit_id": ROOT_UNIT,
        "project_key": "fixture.imported-s",
        "revision": revision,
        "currency_type": 2,
        "price": price,
        "template_machine_id": TEMPLATE_ID,
        "pool": [
            {"category_id": ROOT_CATEGORY, "amount": 1, "entries": 1000},
            {"category_id": OTHER_CATEGORY, "amount": 1, "entries": 4000},
            {
                "category_id": BLUEPRINT_CATEGORY,
                "amount": 1,
                "entries": 5000,
            },
        ],
    }


def template_state(machine_id: int = TEMPLATE_ID) -> dict[str, list[dict]]:
    return {
        "tbl_ItemMachinePrice": [
            {"MachineID": machine_id, "Type": 2, "Price": 99, "Disable": 0}
        ],
        "tbl_ItemMachineList": [
            {
                "MachineID": machine_id,
                "Idx": index,
                "CategoryID": ROOT_CATEGORY,
                "Amount": 1,
                "used": 1,
                "ItemName": "Native template",
            }
            for index in range(1, POOL_SIZE + 1)
        ],
        "tbl_ItemMachineConditionList": [
            {
                "MachineID": machine_id,
                "CurIdx1": 0,
                "CurOutCnt1": 0,
                "CurPrime1": 0,
                "TableSize1": POOL_SIZE,
                "SeedX1": 1_337_755,
                "SeedP1": 1_350_880,
                "OriginPrime1": 7121,
            }
        ],
        "tbl_ItemMachinePrime": [
            {"MachineID": machine_id, "Idx": index, "PNum": 2001 + index * 2}
            for index in range(1, 705)
        ],
        "tbl_ItemMachineShareIdxPrime": [
            {"MachineID": machine_id, "PNum": 2003, "CurPNum": 4036}
        ],
    }


class Result:
    def __init__(self, rows=()):
        self.rows = list(rows)
        self.offset = 0

    def fetchone(self):
        if self.offset >= len(self.rows):
            return None
        row = self.rows[self.offset]
        self.offset += 1
        return row

    def fetchall(self):
        rows = self.rows[self.offset :]
        self.offset = len(self.rows)
        return rows


class Backend:
    def __init__(self):
        self.nocount=False
        self.tables = {
            table: {} for table, _ in STATE_TABLES
        }
        source = template_state()
        for table, _ in STATE_TABLES:
            self.tables[table][TEMPLATE_ID] = copy.deepcopy(source[table])
        self.tables["tbl_ItemMachinePrice"][990001] = [
            {"MachineID": 990001, "Type": 0, "Price": 100000, "Disable": 0}
        ]
        self.units = {ROOT_UNIT}
        self.categories = {
            ROOT_CATEGORY: {
                "CatalogueID": ROOT_CATEGORY,
                "Name": "Imported S unit",
                "RefIndex": ROOT_UNIT,
                "TimeLimit": 0,
            },
            OTHER_CATEGORY: {
                "CatalogueID": OTHER_CATEGORY,
                "Name": "Companion unit",
                "RefIndex": 10002,
                "TimeLimit": 0,
            },
            BLUEPRINT_CATEGORY: {
                "CatalogueID": BLUEPRINT_CATEGORY,
                "Name": "Imported blueprint",
                "RefIndex": 1_800_001,
                "TimeLimit": 0,
            },
        }
        self.ledger_exists = False
        self.ledger_owner = None
        self.ledger: dict[int, dict] = {}
        self.procedures = {
            "sp_GetNextMachineItem": "CREATE PROC sp_GetNextMachineItem AS SELECT 1",
            "sp_ResetUserItemMachineInfo": (
                "CREATE PROC sp_ResetUserItemMachineInfo AS "
                "UPDATE tbl_ItemMachineShareIdxPrime SET CurPNum=CurPNum"
            ),
        }
        self.native_schema = copy.deepcopy(_NATIVE_SCHEMA)
        self.lookup_schema = {
            table: copy.deepcopy(SQL_SCHEMAS[table])
            for table in ("tbl_ItemCategory", "tbl_Units")
        }
        self.statements: list[tuple[str, tuple]] = []
        self.executemany_batches: list[tuple[str, int]] = []
        self.lossy_list_name = False
        self.force_ledger_cas_failure = False

    def snapshot(self):
        return copy.deepcopy(
            (
                self.tables,
                self.ledger_exists,
                self.ledger_owner,
                self.ledger,
            )
        )

    def restore(self, value):
        (
            self.tables,
            self.ledger_exists,
            self.ledger_owner,
            self.ledger,
        ) = copy.deepcopy(value)


class FakeCursor:
    def __init__(self, connection):
        self.connection = connection
        self.backend = connection.backend
        self.result = Result()
        self.rowcount = -1
        self.fast_executemany = False
        self.description=None

    def fetchone(self):
        return self.result.fetchone()

    def fetchall(self):
        return self.result.fetchall()

    @staticmethod
    def _compact(sql: str) -> str:
        return " ".join(sql.split())

    def execute(self, sql, *params):
        compact = self._compact(sql)
        self.backend.statements.append((compact, tuple(params)))
        self.result = Result()
        self.rowcount = -1
        self.description=None
        if "sys.sp_getapplock" in compact:
            self.result = Result([(0,)])
        elif compact.startswith("SELECT OBJECT_ID(N'dbo.sdgo_UnitActivityMachines'"):
            self.result = Result(
                [(1 if self.backend.ledger_exists else None,)]
            )
        elif "FROM sys.extended_properties" in compact:
            self.result = Result(
                []
                if self.backend.ledger_owner is None
                else [(self.backend.ledger_owner,)]
            )
        elif "FROM sys.columns" in compact:
            if params:
                table = str(params[0]).split(".")[-1]
                rows = (
                    self.backend.native_schema[table]
                    if table in self.backend.native_schema
                    else self.backend.lookup_schema[table]
                )
            else:
                rows = LEDGER_SCHEMA if self.backend.ledger_exists else ()
            self.result = Result(rows)
        elif compact.startswith(
            "SELECT CONVERT(nvarchar(max),OBJECT_DEFINITION"
        ):
            name = str(params[0]).split(".")[-1]
            text = self.backend.procedures.get(name)
            self.result = Result([] if text is None else [(text,)])
        elif compact.startswith("SELECT UnitNumber FROM dbo.tbl_Units"):
            self.result = Result(
                [(params[0],)] if params[0] in self.backend.units else []
            )
        elif compact.startswith(
            "SELECT CatalogueID,Name,RefIndex,TimeLimit "
            "FROM dbo.tbl_ItemCategory"
        ):
            self.result = Result(
                [
                    (
                        self.backend.categories[identity]["CatalogueID"],
                        self.backend.categories[identity]["Name"],
                        self.backend.categories[identity]["RefIndex"],
                        self.backend.categories[identity]["TimeLimit"],
                    )
                    for identity in params
                    if identity in self.backend.categories
                ]
            )
        elif compact.startswith(
            "SELECT MachineID,OwnerKey,ProjectKey,UnitID "
            "FROM dbo.sdgo_UnitActivityMachines"
        ):
            project_key, unit_id, machine_id = params
            self.result = Result(
                [
                    (
                        identity,
                        row["OwnerKey"],
                        row["ProjectKey"],
                        row["UnitID"],
                    )
                    for identity, row in self.backend.ledger.items()
                    if identity != machine_id
                    and (
                        row["ProjectKey"] == project_key
                        or row["UnitID"] == unit_id
                    )
                ]
            )
        elif compact.startswith(
            "SELECT MachineID,OwnerKey,ProjectKey,UnitID,Revision,"
            "TemplateMachineID,TemplateHash,InstalledHash,PoolSize,"
            "ConfigJson,SourceJson FROM dbo.sdgo_UnitActivityMachines"
        ):
            row = self.backend.ledger.get(params[0])
            self.result = Result(
                []
                if row is None
                else [tuple(row[column] for column in LEDGER_COLUMNS)]
            )
        elif compact.startswith("SELECT ") and " FROM dbo.tbl_ItemMachine" in compact:
            selected, tail = compact.split(" FROM dbo.", 1)
            columns = tuple(selected[7:].split(","))
            table = tail.split()[0]
            machine_id = params[0]
            rows = copy.deepcopy(
                self.backend.tables[table].get(machine_id, [])
            )
            if " ORDER BY Idx" in compact:
                rows.sort(key=lambda row: row["Idx"])
            self.result = Result(
                [tuple(row[column] for column in columns) for row in rows]
            )
        elif compact.startswith("CREATE TABLE dbo.sdgo_UnitActivityMachines"):
            self.backend.ledger_exists = True
        elif compact.startswith("EXEC sys.sp_addextendedproperty"):
            self.backend.ledger_owner = params[0]
        elif compact.startswith("INSERT dbo.tbl_ItemMachinePrice"):
            row = dict(zip(PRICE_COLUMNS, params))
            self.backend.tables["tbl_ItemMachinePrice"][row["MachineID"]] = [
                row
            ]
            self.rowcount = 1
        elif compact.startswith("INSERT dbo.tbl_ItemMachineConditionList"):
            row = dict(zip(CONDITION_COLUMNS, params))
            self.backend.tables["tbl_ItemMachineConditionList"][
                row["MachineID"]
            ] = [row]
            self.rowcount = 1
        elif compact.startswith("INSERT dbo.tbl_ItemMachineShareIdxPrime"):
            row = dict(zip(SHARE_COLUMNS, params))
            self.backend.tables["tbl_ItemMachineShareIdxPrime"][
                row["MachineID"]
            ] = [row]
            self.rowcount = 1
        elif compact.startswith("UPDATE dbo.tbl_ItemMachinePrice"):
            type_, price, disable, machine_id = params
            rows = self.backend.tables["tbl_ItemMachinePrice"].get(
                machine_id, []
            )
            if rows:
                rows[0].update(Type=type_, Price=price, Disable=disable)
                self.rowcount = 1
            else:
                self.rowcount = 0
        elif compact.startswith("DELETE FROM dbo.tbl_ItemMachineList"):
            self.backend.tables["tbl_ItemMachineList"].pop(params[0], None)
            self.rowcount = 1
        elif compact.startswith("INSERT dbo.sdgo_UnitActivityMachines"):
            (
                machine_id,
                owner,
                project,
                unit,
                revision,
                template,
                template_hash,
                installed_hash,
                pool_size,
                config_json,
                source_json,
            ) = params
            self.backend.ledger[machine_id] = {
                "MachineID": machine_id,
                "OwnerKey": owner,
                "ProjectKey": project,
                "UnitID": unit,
                "Revision": revision,
                "TemplateMachineID": template,
                "TemplateHash": template_hash,
                "InstalledHash": installed_hash,
                "PoolSize": pool_size,
                "ConfigJson": config_json,
                "SourceJson": source_json,
            }
            self.rowcount = 1
        elif (compact.startswith("UPDATE dbo.sdgo_UnitActivityMachines SET ConfigJson=ConfigJson+")
              or compact.startswith("UPDATE dbo.sdgo_UnitActivityMachines SET SourceJson=SourceJson+")):
            machine_id, installed_hash, revision = params[-3:]
            row = self.backend.ledger.get(machine_id)
            if row and row['InstalledHash'] == installed_hash and row['Revision'] == revision:
                field = 'ConfigJson' if 'SET ConfigJson=' in compact else 'SourceJson'
                row[field] += ''.join(params[:-3])
                self.rowcount = 1
            else:
                self.rowcount = 0
        elif compact.startswith("UPDATE dbo.sdgo_UnitActivityMachines"):
            (
                owner,
                project,
                unit,
                revision,
                template,
                template_hash,
                installed_hash,
                pool_size,
                config_json,
                source_json,
                machine_id,
                previous_hash,
            ) = params
            row = self.backend.ledger.get(machine_id)
            if (
                row is not None
                and row["InstalledHash"] == previous_hash
                and not self.backend.force_ledger_cas_failure
            ):
                row.update(
                    OwnerKey=owner,
                    ProjectKey=project,
                    UnitID=unit,
                    Revision=revision,
                    TemplateMachineID=template,
                    TemplateHash=template_hash,
                    InstalledHash=installed_hash,
                    PoolSize=pool_size,
                    ConfigJson=config_json,
                    SourceJson=source_json,
                )
                self.rowcount = 1
            else:
                self.rowcount = 0
        if 'SELECT @@ROWCOUNT AS AffectedRows' in compact:
            self.result=Result([(self.rowcount,)])
            self.description=[('AffectedRows',)]
        if self.backend.nocount:self.rowcount=-1
        return self

    def executemany(self, sql, rows):
        compact = self._compact(sql)
        values = list(rows)
        self.backend.statements.append((compact, ("executemany", len(values))))
        self.backend.executemany_batches.append((compact, len(values)))
        if compact.startswith("INSERT dbo.tbl_ItemMachinePrime"):
            columns = PRIME_COLUMNS
            table = "tbl_ItemMachinePrime"
        elif compact.startswith("INSERT dbo.tbl_ItemMachineList"):
            columns = LIST_COLUMNS
            table = "tbl_ItemMachineList"
        else:
            raise AssertionError(compact)
        for values_row in values:
            row = dict(zip(columns, values_row))
            if table == "tbl_ItemMachineList" and self.backend.lossy_list_name:
                row["ItemName"] = "?"
            self.backend.tables[table].setdefault(row["MachineID"], []).append(
                row
            )
        self.rowcount = len(values)
        return self


class FakeConnection:
    def __init__(self, backend: Backend):
        self.backend = backend
        self._snapshot = backend.snapshot()
        self.committed = False
        self.rolled_back = False
        self.closed = False

    def cursor(self):
        return FakeCursor(self)

    def commit(self):
        self.committed = True
        self._snapshot = self.backend.snapshot()

    def rollback(self):
        self.rolled_back = True
        self.backend.restore(self._snapshot)

    def close(self):
        self.closed = True


class RepositoryFixture:
    def __init__(self, backend: Backend):
        self.backend = backend
        self.connections: list[FakeConnection] = []
        self.before_connect = None

    def connect(self, _server_ini):
        if self.before_connect is not None:
            self.before_connect(len(self.connections) + 1, self.backend)
        connection = FakeConnection(self.backend)
        self.connections.append(connection)
        return connection


class ValidationTests(unittest.TestCase):
    def test_normalize_requires_exact_10000_and_reserved_machine_range(self):
        self.assertEqual(normalize_config(config())["id"], MACHINE_ID)
        candidate = config()
        candidate["pool"][0]["entries"] -= 1
        with self.assertRaisesRegex(ValueError, "10000"):
            normalize_config(candidate)
        candidate = config()
        candidate["id"] = 990001
        with self.assertRaisesRegex(ValueError, "1990001"):
            normalize_config(candidate)

    def test_hash_is_order_independent_and_only_excludes_native_cur_pnum(self):
        state = template_state(MACHINE_ID)
        shuffled = copy.deepcopy(state)
        shuffled["tbl_ItemMachineList"].reverse()
        shuffled["tbl_ItemMachinePrime"].reverse()
        self.assertEqual(
            canonical_state_hash(state), canonical_state_hash(shuffled)
        )
        runtime = copy.deepcopy(state)
        runtime["tbl_ItemMachineShareIdxPrime"][0]["CurPNum"] = 7777
        self.assertEqual(
            canonical_state_hash(state), canonical_state_hash(runtime)
        )
        tampered = copy.deepcopy(state)
        tampered["tbl_ItemMachineShareIdxPrime"][0]["PNum"] += 1
        self.assertNotEqual(
            canonical_state_hash(state), canonical_state_hash(tampered)
        )

    def test_expand_pool_uses_reviewed_integer_entries(self):
        rows = expand_pool(config(), Backend().categories)
        self.assertEqual(len(rows), POOL_SIZE)
        self.assertEqual([row["Idx"] for row in rows], list(range(1, 10001)))
        counts = {
            identity: sum(row["CategoryID"] == identity for row in rows)
            for identity in (ROOT_CATEGORY, OTHER_CATEGORY, BLUEPRINT_CATEGORY)
        }
        self.assertEqual(
            counts,
            {
                ROOT_CATEGORY: 1000,
                OTHER_CATEGORY: 4000,
                BLUEPRINT_CATEGORY: 5000,
            },
        )
        categories = [row["CategoryID"] for row in rows]
        self.assertTrue(
            all(
                categories[index] != categories[(index + 1) % len(categories)]
                for index in range(len(categories))
            )
        )

    def test_pool_over_half_is_rejected_for_native_repeat_avoidance(self):
        candidate = config()
        candidate["pool"][0]["entries"] = 5001
        candidate["pool"][1]["entries"] = 3999
        candidate["pool"][2]["entries"] = 1000
        with self.assertRaisesRegex(ValueError, "连续重复"):
            normalize_config(candidate)


class RepositoryTests(unittest.TestCase):
    def setUp(self):
        self.backend = Backend()
        self.fixture = RepositoryFixture(self.backend)
        self.repository = ActivityRepository(Path("private-server.ini"))
        target = patch.object(
            activity_repository,
            "database_target_key",
            return_value="fixture-database",
        )
        target.start()
        self.addCleanup(target.stop)
        connector = patch.object(
            activity_repository, "_connect", side_effect=self.fixture.connect
        )
        connector.start()
        self.addCleanup(connector.stop)

    def apply(self, value=None):
        return self.repository.apply(config() if value is None else value)

    def test_first_install_clones_native_sequence_and_uses_parameter_batches(self):
        unrelated = copy.deepcopy(
            self.backend.tables["tbl_ItemMachinePrice"][990001]
        )
        result = self.apply()
        self.assertEqual(result["action"], "inserted")
        self.assertEqual(result["runtime_observation"]["share_cur_pnum"], 4036)
        self.assertEqual(
            self.backend.tables["tbl_ItemMachineConditionList"][MACHINE_ID][0][
                "SeedX1"
            ],
            1_337_755,
        )
        self.assertEqual(
            len(self.backend.tables["tbl_ItemMachinePrime"][MACHINE_ID]), 704
        )
        self.assertEqual(
            len(self.backend.tables["tbl_ItemMachineList"][MACHINE_ID]),
            POOL_SIZE,
        )
        self.assertEqual(
            self.backend.tables["tbl_ItemMachinePrice"][990001], unrelated
        )
        self.assertEqual(self.backend.ledger_owner, OWNER_PROPERTY)
        ledger = self.backend.ledger[MACHINE_ID]
        self.assertEqual(ledger["OwnerKey"], OWNER_KEY)
        source = json.loads(ledger["SourceJson"])
        self.assertEqual(
            len(source["template_state"]["tbl_ItemMachineList"]), POOL_SIZE
        )
        self.assertEqual(
            source["runtime_observation"]["share_cur_pnum"], 4036
        )
        self.assertTrue(
            all(size <= 500 for _, size in self.backend.executemany_batches)
        )
        self.assertEqual(
            sum(
                size
                for sql, size in self.backend.executemany_batches
                if "tbl_ItemMachinePrime" in sql
            ),
            704,
        )
        self.assertEqual(
            sum(
                size
                for sql, size in self.backend.executemany_batches
                if "tbl_ItemMachineList" in sql
            ),
            POOL_SIZE,
        )
        sql = "\n".join(statement for statement, _ in self.backend.statements)
        self.assertNotIn("tbl_User", sql)
        self.assertNotIn("EXEC sp_ResetUserItemMachineInfo", sql)
        self.assertNotIn("EXEC dbo.sp_ResetUserItemMachineInfo", sql)

    def test_repeat_is_idempotent(self):
        self.apply()
        before = len(self.backend.statements)
        result = self.apply()
        self.assertEqual(result["action"], "unchanged")
        later = "\n".join(
            sql for sql, _ in self.backend.statements[before:]
        )
        self.assertNotIn("DELETE FROM dbo.tbl_ItemMachineList", later)
        self.assertNotIn("UPDATE dbo.tbl_ItemMachinePrice SET", later)

    def test_shared_connection_keeps_commit_rollback_and_close_with_caller(self):
        connection = FakeConnection(self.backend)
        result = self.repository.apply(
            config(), connection=connection, in_transaction=True
        )
        self.assertTrue(result["transaction_owned_by_caller"])
        self.assertFalse(connection.committed)
        self.assertFalse(connection.rolled_back)
        self.assertFalse(connection.closed)
        verified = self.repository.verify(config(), connection=connection)
        self.assertTrue(verified["transaction_owned_by_caller"])
        self.assertFalse(connection.committed)
        self.assertFalse(connection.rolled_back)
        self.assertFalse(connection.closed)
        sql = "\n".join(statement for statement, _ in self.backend.statements)
        self.assertNotIn("BEGIN TRANSACTION", sql)
        self.assertNotIn("COMMIT TRANSACTION", sql)

    def test_shared_connection_requires_explicit_transaction_contract(self):
        connection = FakeConnection(self.backend)
        with self.assertRaisesRegex(ValueError, "调用方管理事务"):
            self.repository.apply(config(), connection=connection)

    def test_server_identity_is_resolved_before_any_database_write(self):
        with patch.object(
            activity_repository,
            "database_target_key",
            side_effect=ValueError("bad ini"),
        ):
            with self.assertRaisesRegex(ValueError, "bad ini"):
                self.repository.apply(config())
        self.assertEqual(self.fixture.connections, [])
        self.assertFalse(self.backend.ledger_exists)

    def test_update_preserves_condition_prime_and_share_runtime(self):
        self.apply()
        self.backend.tables["tbl_ItemMachineShareIdxPrime"][MACHINE_ID][0][
            "CurPNum"
        ] = 7777
        condition = copy.deepcopy(
            self.backend.tables["tbl_ItemMachineConditionList"][MACHINE_ID]
        )
        prime = copy.deepcopy(
            self.backend.tables["tbl_ItemMachinePrime"][MACHINE_ID]
        )
        share = copy.deepcopy(
            self.backend.tables["tbl_ItemMachineShareIdxPrime"][MACHINE_ID]
        )
        revised = config(revision="b" * 64, price=125)
        revised["pool"][0]["entries"] = 2000
        revised["pool"][1]["entries"] = 3000
        before = len(self.backend.statements)
        result = self.apply(revised)
        self.assertEqual(result["action"], "updated")
        self.assertEqual(result["runtime_observation"]["share_cur_pnum"], 7777)
        self.assertEqual(
            self.backend.tables["tbl_ItemMachineConditionList"][MACHINE_ID],
            condition,
        )
        self.assertEqual(
            self.backend.tables["tbl_ItemMachinePrime"][MACHINE_ID], prime
        )
        self.assertEqual(
            self.backend.tables["tbl_ItemMachineShareIdxPrime"][MACHINE_ID],
            share,
        )
        later = "\n".join(
            sql for sql, _ in self.backend.statements[before:]
        )
        self.assertIn("DELETE FROM dbo.tbl_ItemMachineList", later)
        self.assertNotIn("DELETE FROM dbo.tbl_ItemMachineConditionList", later)
        self.assertNotIn("DELETE FROM dbo.tbl_ItemMachinePrime", later)
        self.assertNotIn("DELETE FROM dbo.tbl_ItemMachineShareIdxPrime", later)

    def test_verify_accepts_cur_pnum_advance_but_rejects_pnum_tamper(self):
        self.apply()
        self.backend.tables["tbl_ItemMachineShareIdxPrime"][MACHINE_ID][0][
            "CurPNum"
        ] = 9999
        result = self.repository.verify(config())
        self.assertTrue(result["verified"])
        self.assertEqual(result["runtime_observation"]["share_cur_pnum"], 9999)
        self.backend.tables["tbl_ItemMachineShareIdxPrime"][MACHINE_ID][0][
            "PNum"
        ] += 1
        with self.assertRaisesRegex(ValueError, "账本外修改"):
            self.repository.verify(config())

    def test_foreign_row_in_any_native_table_blocks_registration(self):
        for table, columns in STATE_TABLES:
            with self.subTest(table=table):
                backend = Backend()
                fixture = RepositoryFixture(backend)
                if table == "tbl_ItemMachinePrice":
                    row = {
                        "MachineID": MACHINE_ID,
                        "Type": 2,
                        "Price": 1,
                        "Disable": 0,
                    }
                else:
                    row = copy.deepcopy(template_state(MACHINE_ID)[table][0])
                backend.tables[table][MACHINE_ID] = [row]
                with patch.object(
                    activity_repository, "_connect", side_effect=fixture.connect
                ):
                    with self.assertRaisesRegex(ValueError, "已被原生"):
                        ActivityRepository(Path("private.ini")).preflight(
                            config()
                        )

    def test_project_or_unit_identity_cannot_move_to_another_machine(self):
        self.apply()
        moved = config(revision="c" * 64)
        moved["id"] = MACHINE_ID + 1
        with self.assertRaisesRegex(ValueError, "已绑定其他"):
            self.repository.preflight(moved)

    def test_lookup_schema_allows_unrelated_columns_but_native_layout_is_exact(self):
        self.repository.preflight(config())
        self.backend.native_schema["tbl_ItemMachinePrice"] = (
            *_NATIVE_SCHEMA["tbl_ItemMachinePrice"],
            ("Unexpected", "int", 4, False),
        )
        with self.assertRaisesRegex(ValueError, "列布局"):
            self.repository.preflight(config())

    def test_lossy_readback_rolls_back_all_install_rows(self):
        self.backend.lossy_list_name = True
        with self.assertRaisesRegex(ValueError, "读回与请求不一致"):
            self.apply()
        for table, _ in STATE_TABLES:
            self.assertNotIn(MACHINE_ID, self.backend.tables[table])
        self.assertNotIn(MACHINE_ID, self.backend.ledger)
        self.assertFalse(self.backend.ledger_exists)
        self.assertTrue(self.fixture.connections[-1].rolled_back)

    def test_invalid_runtime_cur_pnum_is_rejected(self):
        self.apply()
        self.backend.tables["tbl_ItemMachineShareIdxPrime"][MACHINE_ID][0][
            "CurPNum"
        ] = POOL_SIZE
        with self.assertRaisesRegex(ValueError, "CurPNum"):
            self.repository.verify(config())

    def test_template_cas_change_between_review_and_write_rolls_back(self):
        def mutate_second_connection(number, backend):
            if number == 2:
                backend.tables["tbl_ItemMachineShareIdxPrime"][TEMPLATE_ID][0][
                    "PNum"
                ] = 4037

        self.fixture.before_connect = mutate_second_connection
        with self.assertRaisesRegex(ValueError, "确认后发生变化"):
            self.apply()
        for table, _ in STATE_TABLES:
            self.assertNotIn(MACHINE_ID, self.backend.tables[table])
        self.assertFalse(self.backend.ledger_exists)

    def test_normal_template_cursor_advance_during_review_is_allowed(self):
        def mutate_second_connection(number, backend):
            if number == 2:
                backend.tables["tbl_ItemMachineShareIdxPrime"][TEMPLATE_ID][0][
                    "CurPNum"
                ] = 4037

        self.fixture.before_connect = mutate_second_connection
        self.assertTrue(self.apply()["applied"])

    def test_ledger_cas_failure_rolls_back_definition_update(self):
        self.apply()
        old_price = copy.deepcopy(
            self.backend.tables["tbl_ItemMachinePrice"][MACHINE_ID]
        )
        old_list = copy.deepcopy(
            self.backend.tables["tbl_ItemMachineList"][MACHINE_ID]
        )
        old_ledger = copy.deepcopy(self.backend.ledger[MACHINE_ID])
        self.backend.force_ledger_cas_failure = True
        revised = config(revision="d" * 64, price=300)
        with self.assertRaisesRegex(ValueError, "CAS"):
            self.apply(revised)
        self.assertEqual(
            self.backend.tables["tbl_ItemMachinePrice"][MACHINE_ID],
            old_price,
        )
        self.assertEqual(
            self.backend.tables["tbl_ItemMachineList"][MACHINE_ID], old_list
        )
        self.assertEqual(self.backend.ledger[MACHINE_ID], old_ledger)

    def test_nocount_does_not_turn_a_successful_ledger_cas_into_failure(self):
        self.backend.nocount=True
        self.assertTrue(self.apply()['applied'])
        revised=config(revision='d'*64,price=300)
        self.assertTrue(self.apply(revised)['applied'])
        self.assertTrue(self.repository.verify(revised)['verified'])
        self.backend.force_ledger_cas_failure=True
        with self.assertRaisesRegex(ValueError,'CAS'):
            self.apply(config(revision='e'*64,price=301))


if __name__ == "__main__":
    unittest.main()
