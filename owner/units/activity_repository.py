"""Guarded native ItemMachine registration for reviewed imported units.

This module uses the original ItemMachine tables and stored procedures.  It
does not replace the draw path, call the native reset procedure, or touch any
player table.  The only mutable scope is one authenticated machine ID plus its
owner ledger row.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
from typing import Any, Mapping, Sequence

from .activity_pool import compile_pool
from .repository import SQL_SCHEMAS, _connect, database_target_key


SCHEMA_VERSION = 1
OWNER_KEY = "sdgo-unit-activity"
OWNER_PROPERTY = "unit_import/activity/v1"
APP_LOCK = "SDGO:unit-import-activity"
MACHINE_ID_FIRST = 1_990_001
MACHINE_ID_LAST = 1_990_999
POOL_SIZE = 10_000
PRIME_COUNT = 704
_HASH = re.compile(r"[0-9a-f]{64}\Z")
_PROJECT = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z")

PRICE_COLUMNS = ("MachineID", "Type", "Price", "Disable")
LIST_COLUMNS = ("MachineID", "Idx", "CategoryID", "Amount", "used", "ItemName")
CONDITION_COLUMNS = (
    "MachineID",
    "CurIdx1",
    "CurOutCnt1",
    "CurPrime1",
    "TableSize1",
    "SeedX1",
    "SeedP1",
    "OriginPrime1",
)
PRIME_COLUMNS = ("MachineID", "Idx", "PNum")
SHARE_COLUMNS = ("MachineID", "PNum", "CurPNum")
STATE_TABLES = (
    ("tbl_ItemMachinePrice", PRICE_COLUMNS),
    ("tbl_ItemMachineList", LIST_COLUMNS),
    ("tbl_ItemMachineConditionList", CONDITION_COLUMNS),
    ("tbl_ItemMachinePrime", PRIME_COLUMNS),
    ("tbl_ItemMachineShareIdxPrime", SHARE_COLUMNS),
)
DEFINITION_TABLES = (
    ("tbl_ItemMachinePrice", PRICE_COLUMNS),
    ("tbl_ItemMachineList", LIST_COLUMNS),
    ("tbl_ItemMachineConditionList", CONDITION_COLUMNS),
    ("tbl_ItemMachinePrime", PRIME_COLUMNS),
    ("tbl_ItemMachineShareIdxPrime", ("MachineID", "PNum")),
)
PROCEDURES = ("sp_GetNextMachineItem", "sp_ResetUserItemMachineInfo")

# These five layouts were captured from the isolated native database.  They are
# definition tables consumed by the original procedures, so writes are
# admitted only against the exact reviewed layout.
_NATIVE_SCHEMA = {
    "tbl_ItemMachinePrice": (
        ("MachineID", "int", 4, False),
        ("Type", "int", 4, False),
        ("Price", "int", 4, False),
        ("Disable", "int", 4, False),
    ),
    "tbl_ItemMachineList": (
        ("MachineID", "int", 4, False),
        ("Idx", "int", 4, False),
        ("CategoryID", "int", 4, False),
        ("Amount", "int", 4, False),
        ("used", "tinyint", 1, False),
        ("ItemName", "varchar", 50, True),
    ),
    "tbl_ItemMachineConditionList": tuple(
        (name, "int", 4, False) for name in CONDITION_COLUMNS
    ),
    "tbl_ItemMachinePrime": tuple(
        (name, "int", 4, False) for name in PRIME_COLUMNS
    ),
    "tbl_ItemMachineShareIdxPrime": tuple(
        (name, "int", 4, False) for name in SHARE_COLUMNS
    ),
}
_LOOKUP_COLUMNS = {
    "tbl_ItemCategory": ("CatalogueID", "Name", "RefIndex", "TimeLimit"),
    "tbl_Units": ("UnitNumber",),
}
LEDGER_COLUMNS = (
    "MachineID",
    "OwnerKey",
    "ProjectKey",
    "UnitID",
    "Revision",
    "TemplateMachineID",
    "TemplateHash",
    "InstalledHash",
    "PoolSize",
    "ConfigJson",
    "SourceJson",
)
LEDGER_SCHEMA = (
    ("MachineID", "int", 4, False),
    ("OwnerKey", "nvarchar", 256, False),
    ("ProjectKey", "nvarchar", 256, False),
    ("UnitID", "int", 4, False),
    ("Revision", "char", 64, False),
    ("TemplateMachineID", "int", 4, False),
    ("TemplateHash", "char", 64, False),
    ("InstalledHash", "char", 64, False),
    ("PoolSize", "int", 4, False),
    ("ConfigJson", "nvarchar", -1, False),
    ("SourceJson", "nvarchar", -1, False),
    ("UpdatedAt", "datetime2", 6, False),
)
_CONFIG_FIELDS = {
    "schema_version",
    "id",
    "unit_id",
    "project_key",
    "revision",
    "currency_type",
    "price",
    "template_machine_id",
    "pool",
}
_POOL_FIELDS = {"category_id", "amount", "entries"}


def _canonical_json(value: Any) -> str:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    )


def _digest(value: Any) -> str:
    return hashlib.sha256(_canonical_json(value).encode("utf-8")).hexdigest()


def _integer(value: Any, label: str, minimum: int, maximum: int) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{label} 必须是 {minimum}..{maximum} 的整数")
    return value


def normalize_config(value: Any) -> dict[str, Any]:
    """Validate the complete reviewed activity declaration."""
    if not isinstance(value, Mapping) or set(value) != _CONFIG_FIELDS:
        raise ValueError("活动机台配置字段与 schema v1 不一致")
    if value["schema_version"] != SCHEMA_VERSION:
        raise ValueError("不支持的活动机台配置版本")
    machine_id = _integer(
        value["id"], "活动机台编号", MACHINE_ID_FIRST, MACHINE_ID_LAST
    )
    unit_id = _integer(value["unit_id"], "活动机体编号", 1, 9_999_999)
    project_key = value["project_key"]
    if not isinstance(project_key, str) or _PROJECT.fullmatch(project_key) is None:
        raise ValueError("活动机台 project_key 无效")
    revision = value["revision"]
    if not isinstance(revision, str) or _HASH.fullmatch(revision) is None:
        raise ValueError("活动机台 revision 必须是小写 SHA-256")
    currency_type = value["currency_type"]
    if type(currency_type) is not int or currency_type not in (0, 2):
        raise ValueError("活动机台货币类型只支持 0 或 2")
    price = _integer(value["price"], "活动机台价格", 1, 2_147_483_647)
    template = _integer(
        value["template_machine_id"], "活动机台模板编号", 1, 2_147_483_647
    )
    if template == machine_id:
        raise ValueError("活动机台不能以自身作为原生模板")
    supplied_pool = value["pool"]
    if (
        not isinstance(supplied_pool, Sequence)
        or isinstance(supplied_pool, (str, bytes, bytearray))
        or not 2 <= len(supplied_pool) <= 128
    ):
        raise ValueError("活动机台奖池必须含 2..128 个条目")
    pool = []
    rewards = set()
    category_totals = {}
    total = 0
    for index, supplied in enumerate(supplied_pool):
        if not isinstance(supplied, Mapping) or set(supplied) != _POOL_FIELDS:
            raise ValueError(f"活动机台奖池第 {index + 1} 项字段无效")
        category_id = _integer(
            supplied["category_id"], "奖池 CategoryID", 1, 2_147_483_647
        )
        amount = _integer(supplied["amount"], "奖池数量", 1, 2_147_483_647)
        entries = _integer(supplied["entries"], "奖池概率份数", 1, POOL_SIZE)
        if (category_id, amount) in rewards:
            raise ValueError(f"活动机台奖池类别与数量重复：{category_id}")
        rewards.add((category_id, amount))
        category_totals[category_id] = category_totals.get(category_id, 0) + entries
        total += entries
        pool.append(
            {
                "category_id": category_id,
                "amount": amount,
                "entries": entries,
            }
        )
    if total != POOL_SIZE:
        raise ValueError(f"活动机台概率份数必须精确合计 {POOL_SIZE}")
    if max(category_totals.values()) * 2 > POOL_SIZE:
        raise ValueError(
            "活动机台单项超过 50%，无法兼容原生连续重复奖品规避逻辑"
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "id": machine_id,
        "unit_id": unit_id,
        "project_key": project_key,
        "revision": revision,
        "currency_type": currency_type,
        "price": price,
        "template_machine_id": template,
        "pool": pool,
    }


def _ordered_state(
    state: Mapping[str, Sequence[Mapping[str, Any]]],
    *,
    include_runtime: bool,
) -> dict[str, list]:
    result: dict[str, list] = {}
    tables = STATE_TABLES if include_runtime else DEFINITION_TABLES
    for table, columns in tables:
        rows = state.get(table)
        if (
            not isinstance(rows, Sequence)
            or isinstance(rows, (str, bytes, bytearray))
        ):
            raise ValueError(f"{table} 状态缺失")
        normalized = []
        for row in rows:
            if not isinstance(row, Mapping):
                raise ValueError(f"{table} 状态行无效")
            try:
                normalized.append({column: row[column] for column in columns})
            except KeyError as error:
                raise ValueError(f"{table} 状态缺少列 {error.args[0]}") from None
        if "Idx" in columns:
            normalized.sort(key=lambda row: int(row["Idx"]))
        else:
            normalized.sort(key=lambda row: int(row["MachineID"]))
        result[table] = normalized
    return result


def canonical_state_hash(state: Mapping[str, Sequence[Mapping[str, Any]]]) -> str:
    """Hash every protected definition field.

    CurPNum is intentionally excluded: the reviewed original reset procedure
    advances that one field during normal play.  Its range is still validated
    and reported as a separate runtime observation.
    """
    return _digest(_ordered_state(state, include_runtime=False))


def _canonical_full_state_hash(
    state: Mapping[str, Sequence[Mapping[str, Any]]],
) -> str:
    return _digest(_ordered_state(state, include_runtime=True))


def _table_hashes(state: Mapping[str, Sequence[Mapping[str, Any]]]) -> dict[str, str]:
    ordered = _ordered_state(state, include_runtime=True)
    return {table: _digest(ordered[table]) for table, _ in STATE_TABLES}


def _as_native_int(value: Any, label: str) -> int:
    if type(value) is not int:
        raise ValueError(f"{label} 不是原生整数")
    return value


def _validate_template(
    state: Mapping[str, Sequence[Mapping[str, Any]]],
    *,
    expected_machine_id: int | None = None,
) -> None:
    ordered = _ordered_state(state, include_runtime=True)
    counts = {table: len(ordered[table]) for table, _ in STATE_TABLES}
    expected = {
        "tbl_ItemMachinePrice": 1,
        "tbl_ItemMachineList": POOL_SIZE,
        "tbl_ItemMachineConditionList": 1,
        "tbl_ItemMachinePrime": PRIME_COUNT,
        "tbl_ItemMachineShareIdxPrime": 1,
    }
    if counts != expected:
        raise ValueError("所选原生活动机台模板不完整")
    machine_ids = {
        _as_native_int(row["MachineID"], f"{table}.MachineID")
        for table, _ in STATE_TABLES
        for row in ordered[table]
    }
    if len(machine_ids) != 1:
        raise ValueError("原生活动机台五表 MachineID 不一致")
    machine_id = next(iter(machine_ids))
    if expected_machine_id is not None and machine_id != expected_machine_id:
        raise ValueError("原生活动机台读回了错误的 MachineID")
    price = ordered["tbl_ItemMachinePrice"][0]
    if (
        _as_native_int(price["Type"], "tbl_ItemMachinePrice.Type") not in (0, 2)
        or _as_native_int(price["Price"], "tbl_ItemMachinePrice.Price") <= 0
        or _as_native_int(price["Disable"], "tbl_ItemMachinePrice.Disable")
        not in (0, 1)
    ):
        raise ValueError("原生活动机台模板价格记录无效")
    list_rows = ordered["tbl_ItemMachineList"]
    list_indices = [
        _as_native_int(row["Idx"], "tbl_ItemMachineList.Idx")
        for row in list_rows
    ]
    if list_indices != list(range(1, POOL_SIZE + 1)):
        raise ValueError("原生活动机台模板的 10000 项索引不连续")
    for row in list_rows:
        if (
            _as_native_int(row["CategoryID"], "tbl_ItemMachineList.CategoryID")
            <= 0
            or _as_native_int(row["Amount"], "tbl_ItemMachineList.Amount") <= 0
            or _as_native_int(row["used"], "tbl_ItemMachineList.used")
            not in (0, 1)
            or row["ItemName"] is not None
            and not isinstance(row["ItemName"], str)
        ):
            raise ValueError("原生活动机台模板奖池记录无效")
    prime_rows = ordered["tbl_ItemMachinePrime"]
    prime_indices = [
        _as_native_int(row["Idx"], "tbl_ItemMachinePrime.Idx")
        for row in prime_rows
    ]
    if prime_indices != list(range(1, PRIME_COUNT + 1)):
        raise ValueError("原生活动机台模板的 Prime 索引不连续")
    if any(
        _as_native_int(row["PNum"], "tbl_ItemMachinePrime.PNum") <= 0
        for row in prime_rows
    ):
        raise ValueError("原生活动机台模板含无效 Prime")
    condition = ordered["tbl_ItemMachineConditionList"][0]
    if _as_native_int(condition["TableSize1"], "TableSize1") != POOL_SIZE:
        raise ValueError("原生活动机台模板 TableSize1 不是 10000")
    for column in CONDITION_COLUMNS[1:]:
        _as_native_int(condition[column], f"tbl_ItemMachineConditionList.{column}")
    share = ordered["tbl_ItemMachineShareIdxPrime"][0]
    if _as_native_int(share["PNum"], "tbl_ItemMachineShareIdxPrime.PNum") <= 0:
        raise ValueError("原生活动机台模板 Share PNum 无效")
    cur_pnum = _as_native_int(
        share["CurPNum"], "tbl_ItemMachineShareIdxPrime.CurPNum"
    )
    if not 0 <= cur_pnum < POOL_SIZE:
        raise ValueError("原生活动机台 CurPNum 超出 TableSize1")


def _state_presence(state: Mapping[str, Sequence[Mapping[str, Any]]]) -> dict[str, int]:
    return {table: len(state[table]) for table, _ in STATE_TABLES}


def _validate_owned_state(
    state: Mapping[str, Sequence[Mapping[str, Any]]],
    *,
    machine_id: int,
) -> None:
    _validate_template(state, expected_machine_id=machine_id)


def _runtime_observation(
    state: Mapping[str, Sequence[Mapping[str, Any]]],
) -> dict[str, int]:
    condition = state["tbl_ItemMachineConditionList"][0]
    share = state["tbl_ItemMachineShareIdxPrime"][0]
    table_size = _as_native_int(condition["TableSize1"], "TableSize1")
    cur_pnum = _as_native_int(share["CurPNum"], "CurPNum")
    if not 0 <= cur_pnum < table_size:
        raise ValueError("活动机台 CurPNum 超出原生序列表范围")
    return {
        "share_cur_pnum": cur_pnum,
        "table_size": table_size,
    }


def _normalize_schema_row(row: Sequence[Any]) -> tuple[str, str, int, bool]:
    return str(row[0]), str(row[1]).lower(), int(row[2]), bool(row[3])


def _validate_native_schema(cursor) -> None:
    for table, expected in _NATIVE_SCHEMA.items():
        rows = cursor.execute(
            """
SELECT c.name,t.name,c.max_length,c.is_nullable
FROM sys.columns c
JOIN sys.types t
  ON c.system_type_id=t.system_type_id
 AND t.user_type_id=t.system_type_id
WHERE c.object_id=OBJECT_ID(?)
ORDER BY c.column_id
""",
            "dbo." + table,
        ).fetchall()
        actual = tuple(_normalize_schema_row(row) for row in rows)
        if actual != expected:
            raise ValueError(f"服务端 dbo.{table} 列布局与支持版本不一致")
    for table, required_names in _LOOKUP_COLUMNS.items():
        rows = cursor.execute(
            """
SELECT c.name,t.name,c.max_length,c.is_nullable
FROM sys.columns c
JOIN sys.types t
  ON c.system_type_id=t.system_type_id
 AND t.user_type_id=t.system_type_id
WHERE c.object_id=OBJECT_ID(?)
ORDER BY c.column_id
""",
            "dbo." + table,
        ).fetchall()
        actual = {
            row[0]: row
            for row in (_normalize_schema_row(value) for value in rows)
        }
        expected = {
            row[0]: row
            for row in SQL_SCHEMAS[table]
            if row[0] in required_names
        }
        if any(actual.get(name) != expected[name] for name in required_names):
            raise ValueError(f"服务端 dbo.{table} 查询列布局与支持版本不一致")


def _ledger_exists(cursor) -> bool:
    row = cursor.execute(
        "SELECT OBJECT_ID(N'dbo.sdgo_UnitActivityMachines',N'U')"
    ).fetchone()
    return row is not None and row[0] is not None


def _ledger_owner(cursor) -> str | None:
    row = cursor.execute(
        """
SELECT CONVERT(nvarchar(128),value)
FROM sys.extended_properties
WHERE class=1
  AND major_id=OBJECT_ID(N'dbo.sdgo_UnitActivityMachines')
  AND minor_id=0
  AND name=N'sdgo_owner'
"""
    ).fetchone()
    return None if row is None else str(row[0])


def _validate_ledger_schema(cursor) -> None:
    rows = cursor.execute(
        """
SELECT c.name,t.name,c.max_length,c.is_nullable
FROM sys.columns c
JOIN sys.types t
  ON c.system_type_id=t.system_type_id
 AND t.user_type_id=t.system_type_id
WHERE c.object_id=OBJECT_ID(N'dbo.sdgo_UnitActivityMachines')
ORDER BY c.column_id
"""
    ).fetchall()
    actual = tuple(_normalize_schema_row(row) for row in rows)
    if actual != LEDGER_SCHEMA:
        raise ValueError("活动机台所有权账本结构与支持版本不一致")
    if _ledger_owner(cursor) != OWNER_PROPERTY:
        raise ValueError("活动机台所有权账本属于其他组件或缺少 owner 标记")


def _ensure_ledger(cursor) -> None:
    if not _ledger_exists(cursor):
        cursor.execute(
            """
CREATE TABLE dbo.sdgo_UnitActivityMachines(
    MachineID int NOT NULL
        CONSTRAINT PK_sdgo_UnitActivityMachines PRIMARY KEY,
    OwnerKey nvarchar(128) NOT NULL,
    ProjectKey nvarchar(128) NOT NULL
        CONSTRAINT UQ_sdgo_UnitActivityMachines_ProjectKey UNIQUE,
    UnitID int NOT NULL
        CONSTRAINT UQ_sdgo_UnitActivityMachines_UnitID UNIQUE,
    Revision char(64) NOT NULL,
    TemplateMachineID int NOT NULL,
    TemplateHash char(64) NOT NULL,
    InstalledHash char(64) NOT NULL,
    PoolSize int NOT NULL,
    ConfigJson nvarchar(max) NOT NULL,
    SourceJson nvarchar(max) NOT NULL,
    UpdatedAt datetime2(0) NOT NULL
        CONSTRAINT DF_sdgo_UnitActivityMachines_UpdatedAt
        DEFAULT SYSUTCDATETIME()
)
"""
        )
        cursor.execute(
            """
EXEC sys.sp_addextendedproperty
    @name=N'sdgo_owner',@value=?,
    @level0type=N'SCHEMA',@level0name=N'dbo',
    @level1type=N'TABLE',@level1name=N'sdgo_UnitActivityMachines'
""",
            OWNER_PROPERTY,
        )
    _validate_ledger_schema(cursor)


def _procedure_hashes(cursor) -> dict[str, str]:
    result = {}
    for procedure in PROCEDURES:
        row = cursor.execute(
            "SELECT CONVERT(nvarchar(max),OBJECT_DEFINITION(OBJECT_ID(?)))",
            "dbo." + procedure,
        ).fetchone()
        if row is None or not isinstance(row[0], str) or not row[0].strip():
            raise ValueError(f"服务端缺少原生过程 dbo.{procedure}")
        result[procedure] = hashlib.sha256(row[0].encode("utf-8")).hexdigest()
    return result


def _fetch_state(cursor, machine_id: int, *, locked: bool) -> dict[str, list[dict]]:
    hint = " WITH (UPDLOCK,HOLDLOCK)" if locked else ""
    result = {}
    for table, columns in STATE_TABLES:
        order = " ORDER BY Idx" if "Idx" in columns else ""
        rows = cursor.execute(
            f"SELECT {','.join(columns)} FROM dbo.{table}{hint} "
            f"WHERE MachineID=?{order}",
            machine_id,
        ).fetchall()
        result[table] = [
            {column: value for column, value in zip(columns, row)}
            for row in rows
        ]
    return result


def _fetch_ledger(cursor, machine_id: int, *, locked: bool) -> dict | None:
    if not _ledger_exists(cursor):
        return None
    _validate_ledger_schema(cursor)
    hint = " WITH (UPDLOCK,HOLDLOCK)" if locked else ""
    row = cursor.execute(
        f"SELECT {','.join(LEDGER_COLUMNS)} "
        f"FROM dbo.sdgo_UnitActivityMachines{hint} WHERE MachineID=?",
        machine_id,
    ).fetchone()
    if row is None:
        return None
    result = {column: value for column, value in zip(LEDGER_COLUMNS, row)}
    for field in ("MachineID", "UnitID", "TemplateMachineID", "PoolSize"):
        result[field] = int(result[field])
    for field in (
        "OwnerKey",
        "ProjectKey",
        "Revision",
        "TemplateHash",
        "InstalledHash",
        "ConfigJson",
        "SourceJson",
    ):
        result[field] = str(result[field])
    return result


def _fetch_lookups(cursor, config: Mapping[str, Any], *, locked: bool) -> dict[int, dict]:
    hint = " WITH (UPDLOCK,HOLDLOCK)" if locked else ""
    unit = cursor.execute(
        f"SELECT UnitNumber FROM dbo.tbl_Units{hint} WHERE UnitNumber=?",
        config["unit_id"],
    ).fetchone()
    if unit is None:
        raise ValueError("活动机台根机体尚未登记到 tbl_Units")
    category_ids = [row["category_id"] for row in config["pool"]]
    placeholders = ",".join("?" for _ in category_ids)
    rows = cursor.execute(
        "SELECT CatalogueID,Name,RefIndex,TimeLimit "
        f"FROM dbo.tbl_ItemCategory{hint} "
        f"WHERE CatalogueID IN ({placeholders})",
        *category_ids,
    ).fetchall()
    categories = {
        int(row[0]): {
            "CatalogueID": int(row[0]),
            "Name": row[1],
            "RefIndex": int(row[2]),
            "TimeLimit": int(row[3]),
        }
        for row in rows
    }
    if set(categories) != set(category_ids):
        missing = sorted(set(category_ids) - set(categories))
        raise ValueError(f"活动机台奖池缺少物品类别：{missing[0]}")
    root_category = False
    for category_id, row in categories.items():
        category_class = category_id // 100_000
        if category_class not in (11, 15, 22):
            raise ValueError(
                f"活动机台只接受原生机体、图纸或消耗品类别：{category_id}"
            )
        if row["TimeLimit"] != 0:
            raise ValueError(f"活动机台奖品必须是永久物品：{category_id}")
        name = row["Name"]
        if (
            not isinstance(name, str)
            or not name
            or len(name) > 50
            or "\0" in name
        ):
            raise ValueError(f"活动机台奖品名称无法写入原生 ItemName：{category_id}")
        if category_class == 11 and row["RefIndex"] == config["unit_id"]:
            root_category = True
    if not root_category:
        raise ValueError("活动机台奖池必须包含当前根机体的永久 11 类物品")
    return categories


def expand_pool(
    config: Mapping[str, Any],
    category_lookup: Mapping[int, Mapping[str, Any]],
) -> list[dict[str, Any]]:
    """Expand reviewed integer weights into the exact native 10000-row list."""
    normalized = normalize_config(config)
    rewards = []
    for entry in normalized["pool"]:
        category = category_lookup.get(entry["category_id"])
        if category is None:
            raise ValueError(f"活动机台奖池缺少物品类别：{entry['category_id']}")
        name = category.get("Name")
        if not isinstance(name, str) or not name or len(name) > 50 or "\0" in name:
            raise ValueError("活动机台 ItemName 无效")
        rewards.append(
            {
                "category_id": entry["category_id"],
                "amount": entry["amount"],
                "weight": entry["entries"],
                "name": name,
                "role": (
                    "root"
                    if category.get("RefIndex") == normalized["unit_id"]
                    else "companion"
                ),
                "source": "reviewed_activity_config",
            }
        )
    compiled = compile_pool(rewards)
    names = {
        entry["category_id"]: category_lookup[entry["category_id"]]["Name"]
        for entry in normalized["pool"]
    }
    rows = [
        {
            "MachineID": normalized["id"],
            **row,
            "ItemName": names[row["CategoryID"]],
        }
        for row in compiled["sequence"]
    ]
    if len(rows) != POOL_SIZE:
        raise ValueError("活动机台奖池展开后不是 10000 项")
    categories = [row["CategoryID"] for row in rows]
    if any(
        categories[index] == categories[(index + 1) % len(categories)]
        for index in range(len(categories))
    ):
        raise ValueError("活动机台奖池序列无法兼容原生连续重复奖品规避逻辑")
    return rows


def _target_state(
    config: Mapping[str, Any],
    template: Mapping[str, Sequence[Mapping[str, Any]]],
    categories: Mapping[int, Mapping[str, Any]],
    *,
    current: Mapping[str, Sequence[Mapping[str, Any]]] | None,
) -> dict[str, list[dict]]:
    _validate_template(
        template, expected_machine_id=config["template_machine_id"]
    )
    machine_id = config["id"]
    if current is None:
        condition = copy.deepcopy(template["tbl_ItemMachineConditionList"])
        prime = copy.deepcopy(template["tbl_ItemMachinePrime"])
        share = copy.deepcopy(template["tbl_ItemMachineShareIdxPrime"])
        for rows in (condition, prime, share):
            for row in rows:
                row["MachineID"] = machine_id
    else:
        _validate_owned_state(current, machine_id=machine_id)
        condition = copy.deepcopy(current["tbl_ItemMachineConditionList"])
        prime = copy.deepcopy(current["tbl_ItemMachinePrime"])
        share = copy.deepcopy(current["tbl_ItemMachineShareIdxPrime"])
    return {
        "tbl_ItemMachinePrice": [
            {
                "MachineID": machine_id,
                "Type": config["currency_type"],
                "Price": config["price"],
                "Disable": 0,
            }
        ],
        "tbl_ItemMachineList": expand_pool(config, categories),
        "tbl_ItemMachineConditionList": condition,
        "tbl_ItemMachinePrime": prime,
        "tbl_ItemMachineShareIdxPrime": share,
    }


def _source_receipt(
    config: Mapping[str, Any],
    template: Mapping[str, Sequence[Mapping[str, Any]]],
    procedures: Mapping[str, str],
) -> dict[str, Any]:
    ordered = _ordered_state(template, include_runtime=True)
    return {
        "schema_version": SCHEMA_VERSION,
        "template_machine_id": config["template_machine_id"],
        "template_hash": _canonical_full_state_hash(template),
        "template_definition_hash": canonical_state_hash(template),
        "table_hashes": _table_hashes(template),
        "row_counts": _state_presence(template),
        "template_state": ordered,
        "runtime_observation": _runtime_observation(template),
        "procedure_sha256": dict(sorted(procedures.items())),
        "procedure_fingerprints_are_advisory": True,
    }


def _stored_source(ledger: Mapping[str, Any]) -> dict[str, Any]:
    try:
        value = json.loads(ledger["SourceJson"])
    except (TypeError, json.JSONDecodeError):
        raise ValueError("活动机台账本的模板来源收据损坏") from None
    if (
        not isinstance(value, dict)
        or value.get("schema_version") != SCHEMA_VERSION
        or value.get("template_machine_id") != ledger["TemplateMachineID"]
        or value.get("template_hash") != ledger["TemplateHash"].lower()
        or not isinstance(value.get("template_state"), dict)
    ):
        raise ValueError("活动机台账本的模板来源收据不一致")
    template = value["template_state"]
    _validate_template(
        template, expected_machine_id=ledger["TemplateMachineID"]
    )
    if _canonical_full_state_hash(template) != value["template_hash"]:
        raise ValueError("活动机台账本的模板快照哈希不一致")
    return value


def _validate_ownership(
    config: Mapping[str, Any],
    current: Mapping[str, Sequence[Mapping[str, Any]]],
    ledger: Mapping[str, Any] | None,
) -> str | None:
    presence = _state_presence(current)
    has_rows = any(presence.values())
    if ledger is None:
        if has_rows:
            raise ValueError("目标 MachineID 已被原生或其他内容占用")
        return None
    if ledger["OwnerKey"] != OWNER_KEY:
        raise ValueError("目标 MachineID 属于其他活动机台提供者")
    if (
        ledger["ProjectKey"] != config["project_key"]
        or ledger["UnitID"] != config["unit_id"]
        or ledger["TemplateMachineID"] != config["template_machine_id"]
    ):
        raise ValueError("活动机台不能更换 project、根机体或原生模板身份")
    if ledger["PoolSize"] != POOL_SIZE:
        raise ValueError("既有活动机台奖池大小不是 10000，拒绝重置玩家状态")
    if not has_rows:
        raise ValueError("活动机台账本存在但原生五表记录缺失")
    _validate_owned_state(current, machine_id=config["id"])
    digest = canonical_state_hash(current)
    if digest != ledger["InstalledHash"].lower():
        raise ValueError("活动机台原生五表已在所有权账本外修改")
    _stored_source(ledger)
    return digest


def _validate_identity_ledger(
    cursor,
    config: Mapping[str, Any],
    *,
    locked: bool,
) -> None:
    if not _ledger_exists(cursor):
        return
    _validate_ledger_schema(cursor)
    hint = " WITH (UPDLOCK,HOLDLOCK)" if locked else ""
    rows = cursor.execute(
        "SELECT MachineID,OwnerKey,ProjectKey,UnitID "
        f"FROM dbo.sdgo_UnitActivityMachines{hint} "
        "WHERE (ProjectKey=? OR UnitID=?) AND MachineID<>?",
        config["project_key"],
        config["unit_id"],
        config["id"],
    ).fetchall()
    if rows:
        raise ValueError("活动机台 project_key 或根机体已绑定其他 MachineID")


def _inspect(cursor, config: Mapping[str, Any], *, locked: bool) -> dict[str, Any]:
    _validate_native_schema(cursor)
    procedures = _procedure_hashes(cursor)
    template = _fetch_state(
        cursor, config["template_machine_id"], locked=locked
    )
    _validate_template(
        template, expected_machine_id=config["template_machine_id"]
    )
    categories = _fetch_lookups(cursor, config, locked=locked)
    current = _fetch_state(cursor, config["id"], locked=locked)
    ledger = _fetch_ledger(cursor, config["id"], locked=locked)
    _validate_identity_ledger(cursor, config, locked=locked)
    current_hash = _validate_ownership(config, current, ledger)
    target = _target_state(
        config,
        template,
        categories,
        current=None if ledger is None else current,
    )
    admission_source = _source_receipt(config, template, procedures)
    source = (
        admission_source
        if ledger is None
        else _stored_source(ledger)
    )
    target_hash = canonical_state_hash(target)
    config_json = _canonical_json(config)
    source_json = _canonical_json(source)
    if ledger is None:
        action = "inserted"
    elif (
        current_hash == target_hash
        and ledger["Revision"] == config["revision"]
        and ledger["ConfigJson"] == config_json
        and ledger["SourceJson"] == source_json
    ):
        action = "unchanged"
    else:
        action = "updated"
    return {
        "config": config,
        "template": template,
        "target": target,
        "current": current,
        "ledger": ledger,
        "source": source,
        "admission_source": admission_source,
        # A normal native reset advances the template's shared cursor.  Keep
        # the full observation in SourceJson, but never use that runtime value
        # as a configuration CAS token.
        "source_hash": _digest({
            "template_machine_id": config["template_machine_id"],
            "template_definition_hash": admission_source["template_definition_hash"],
            "procedure_sha256": procedures,
        }),
        "target_hash": target_hash,
        "runtime_observation": _runtime_observation(
            target if ledger is None else current
        ),
        "config_json": config_json,
        "source_json": source_json,
        "action": action,
    }


def _execute_chunks(cursor, sql: str, rows: Sequence[Sequence[Any]], size: int = 500):
    try:
        cursor.fast_executemany = True
    except (AttributeError, TypeError):
        pass
    for start in range(0, len(rows), size):
        cursor.executemany(sql, rows[start : start + size])


def _write_state(cursor, state: Mapping[str, Sequence[Mapping[str, Any]]], *, exists: bool):
    machine_id = int(state["tbl_ItemMachinePrice"][0]["MachineID"])
    price = state["tbl_ItemMachinePrice"][0]
    if exists:
        cursor.execute(
            "UPDATE dbo.tbl_ItemMachinePrice "
            "SET Type=?,Price=?,Disable=? WHERE MachineID=?",
            price["Type"],
            price["Price"],
            price["Disable"],
            machine_id,
        )
        if getattr(cursor, "rowcount", 1) != 1:
            raise ValueError("活动机台价格记录在事务中消失")
        cursor.execute(
            "DELETE FROM dbo.tbl_ItemMachineList WHERE MachineID=?",
            machine_id,
        )
    else:
        cursor.execute(
            "INSERT dbo.tbl_ItemMachinePrice(MachineID,Type,Price,Disable) "
            "VALUES(?,?,?,?)",
            *(price[column] for column in PRICE_COLUMNS),
        )
        condition = state["tbl_ItemMachineConditionList"][0]
        cursor.execute(
            "INSERT dbo.tbl_ItemMachineConditionList"
            f"({','.join(CONDITION_COLUMNS)}) "
            f"VALUES({','.join('?' for _ in CONDITION_COLUMNS)})",
            *(condition[column] for column in CONDITION_COLUMNS),
        )
        share = state["tbl_ItemMachineShareIdxPrime"][0]
        cursor.execute(
            "INSERT dbo.tbl_ItemMachineShareIdxPrime"
            f"({','.join(SHARE_COLUMNS)}) "
            f"VALUES({','.join('?' for _ in SHARE_COLUMNS)})",
            *(share[column] for column in SHARE_COLUMNS),
        )
        _execute_chunks(
            cursor,
            "INSERT dbo.tbl_ItemMachinePrime(MachineID,Idx,PNum) VALUES(?,?,?)",
            [
                tuple(row[column] for column in PRIME_COLUMNS)
                for row in state["tbl_ItemMachinePrime"]
            ],
        )
    _execute_chunks(
        cursor,
        "INSERT dbo.tbl_ItemMachineList"
        "(MachineID,Idx,CategoryID,Amount,used,ItemName) VALUES(?,?,?,?,?,?)",
        [
            tuple(row[column] for column in LIST_COLUMNS)
            for row in state["tbl_ItemMachineList"]
        ],
    )


def _append_ledger_text(cursor, config, installed_hash, field, text):
    # The legacy SQL Server ODBC driver uses SQLPutData for long nvarchar
    # parameters and fails on this host with error 7125. Each bound string is
    # kept below 4000 UTF-16 code units; concatenation remains nvarchar(max).
    chunks = [text[start:start + 2000] for start in range(2000, len(text), 2000)]
    for start in range(0, len(chunks), 100):
        batch = chunks[start:start + 100]
        expression = "+".join("CAST(? AS nvarchar(max))" for _ in batch)
        cursor.execute(
            f"UPDATE dbo.sdgo_UnitActivityMachines SET {field}={field}+{expression} "
            "WHERE MachineID=? AND InstalledHash=? AND Revision=?",
            *batch, config["id"], installed_hash, config["revision"],
        )
        if getattr(cursor, "rowcount", 1) != 1:
            raise ValueError("活动机台来源收据分段写入 CAS 失败")


def _upsert_ledger(cursor, inspection: Mapping[str, Any], installed_hash: str) -> None:
    config = inspection["config"]
    values = (
        OWNER_KEY,
        config["project_key"],
        config["unit_id"],
        config["revision"],
        config["template_machine_id"],
        inspection["source"]["template_hash"],
        installed_hash,
        POOL_SIZE,
        inspection["config_json"][:2000],
        inspection["source_json"][:2000],
    )
    if inspection["ledger"] is None:
        cursor.execute(
            """
INSERT dbo.sdgo_UnitActivityMachines(
    MachineID,OwnerKey,ProjectKey,UnitID,Revision,TemplateMachineID,
    TemplateHash,InstalledHash,PoolSize,ConfigJson,SourceJson,UpdatedAt
) VALUES(?,?,?,?,?,?,?,?,?,?,?,SYSUTCDATETIME())
""",
            config["id"],
            *values,
        )
    else:
        cursor.execute(
            """
UPDATE dbo.sdgo_UnitActivityMachines
SET OwnerKey=?,ProjectKey=?,UnitID=?,Revision=?,TemplateMachineID=?,
    TemplateHash=?,InstalledHash=?,PoolSize=?,ConfigJson=?,SourceJson=?,
    UpdatedAt=SYSUTCDATETIME()
WHERE MachineID=? AND InstalledHash=?
""",
            *values,
            config["id"],
            inspection["ledger"]["InstalledHash"],
        )
        if getattr(cursor, "rowcount", 1) != 1:
            raise ValueError("活动机台账本 CAS 更新失败")
    for field, key in (("ConfigJson", "config_json"), ("SourceJson", "source_json")):
        _append_ledger_text(cursor, config, installed_hash, field, inspection[key])


class ActivityRepository:
    """Read, install, and verify one owner-scoped native activity machine."""

    def __init__(self, server_ini):
        self.server_ini = Path(server_ini) if server_ini is not None else None

    def _connection(self):
        if self.server_ini is None:
            raise ValueError("未配置服务端 INI，不能访问原生活动机台")
        return _connect(self.server_ini)

    def preflight(self, config: Any) -> dict[str, Any]:
        """Read non-player configuration only; perform no writes."""
        normalized = normalize_config(config)
        connection = self._connection()
        try:
            cursor = connection.cursor()
            cursor.execute("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE")
            inspection = _inspect(cursor, normalized, locked=False)
            connection.rollback()
            return {
                "schema_version": SCHEMA_VERSION,
                "machine_id": normalized["id"],
                "unit_id": normalized["unit_id"],
                "project_key": normalized["project_key"],
                "revision": normalized["revision"],
                "template_machine_id": normalized["template_machine_id"],
                "template_hash": inspection["source"]["template_hash"],
                "current_template_hash": inspection["admission_source"][
                    "template_hash"
                ],
                "source_hash": inspection["source_hash"],
                "target_hash": inspection["target_hash"],
                "action": inspection["action"],
                "runtime_observation": inspection["runtime_observation"],
                "pool_size": POOL_SIZE,
                "prime_rows": PRIME_COUNT,
                "procedure_sha256": inspection["admission_source"][
                    "procedure_sha256"
                ],
                "procedure_fingerprints_are_advisory": True,
                "valid": True,
                "writes_performed": False,
                "player_tables_read": False,
            }
        except ValueError:
            try:
                connection.rollback()
            except Exception:
                pass
            raise
        except Exception:
            try:
                connection.rollback()
            except Exception:
                pass
            raise ValueError("读取原生活动机台配置失败，未进行任何写入") from None
        finally:
            connection.close()

    def _apply_on_connection(
        self,
        connection,
        normalized: Mapping[str, Any],
        reviewed: Mapping[str, Any],
        *,
        owns_transaction: bool,
        target_key: str,
    ) -> dict[str, Any]:
        cursor = connection.cursor()
        if owns_transaction:
            cursor.execute("SET XACT_ABORT ON")
            cursor.execute("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE")
            cursor.execute("BEGIN TRANSACTION")
        cursor.execute(
            """
DECLARE @Result int;
EXEC @Result=sys.sp_getapplock
    @Resource=?,@LockMode=N'Exclusive',@LockOwner=N'Transaction',
    @LockTimeout=5000;
SELECT @Result;
""",
            APP_LOCK,
        )
        lock = cursor.fetchone()
        if lock is None or int(lock[0]) < 0:
            raise ValueError("另一项活动机台更新正在进行，请稍后重试")
        _ensure_ledger(cursor)
        inspection = _inspect(cursor, normalized, locked=True)
        if (
            inspection["source_hash"] != reviewed["source_hash"]
            or inspection["target_hash"] != reviewed["target_hash"]
        ):
            raise ValueError("活动机台模板或奖品目录在确认后发生变化")
        if inspection["action"] != "unchanged":
            _write_state(
                cursor,
                inspection["target"],
                exists=inspection["ledger"] is not None,
            )
            installed = _fetch_state(cursor, normalized["id"], locked=True)
            _validate_owned_state(installed, machine_id=normalized["id"])
            installed_hash = canonical_state_hash(installed)
            if installed_hash != inspection["target_hash"]:
                raise ValueError("活动机台五表写入后读回与请求不一致")
            _upsert_ledger(cursor, inspection, installed_hash)
        else:
            installed_hash = inspection["target_hash"]
        ledger = _fetch_ledger(cursor, normalized["id"], locked=True)
        if (
            ledger is None
            or ledger["OwnerKey"] != OWNER_KEY
            or ledger["InstalledHash"].lower() != installed_hash
            or ledger["Revision"] != normalized["revision"]
            or ledger["ConfigJson"] != inspection["config_json"]
            or ledger["SourceJson"] != inspection["source_json"]
        ):
            raise ValueError("活动机台所有权账本写入后读回不一致")
        runtime_observation = _runtime_observation(
            _fetch_state(cursor, normalized["id"], locked=True)
        )
        if owns_transaction:
            cursor.execute("COMMIT TRANSACTION")
            connection.commit()
        return {
            "schema_version": SCHEMA_VERSION,
            "machine_id": normalized["id"],
            "unit_id": normalized["unit_id"],
            "project_key": normalized["project_key"],
            "revision": normalized["revision"],
            "action": inspection["action"],
            "installed_hash": installed_hash,
            "template_hash": inspection["source"]["template_hash"],
            "current_template_hash": inspection["admission_source"][
                "template_hash"
            ],
            "runtime_observation": runtime_observation,
            "pool_size": POOL_SIZE,
            "prime_rows": PRIME_COUNT,
            "applied": True,
            "server_target": target_key,
            "player_tables_changed": False,
            "player_state_reset": False,
            "stored_procedures_changed": False,
            "server_restart_required": False,
            "transaction_owned_by_caller": not owns_transaction,
        }

    def apply(
        self,
        config: Any,
        *,
        connection=None,
        in_transaction: bool = False,
    ) -> dict[str, Any]:
        """Install/update one machine, optionally inside a caller transaction."""
        normalized = normalize_config(config)
        target_key = database_target_key(self.server_ini)
        if connection is not None:
            if not in_transaction:
                raise ValueError("共享数据库连接必须声明由调用方管理事务")
            try:
                reviewed = _inspect(
                    connection.cursor(), normalized, locked=False
                )
                return self._apply_on_connection(
                    connection,
                    normalized,
                    reviewed,
                    owns_transaction=False,
                    target_key=target_key,
                )
            except ValueError:
                raise
            except Exception:
                raise ValueError(
                    "原生活动机台事务失败，请由调用方回滚共享事务"
                ) from None
        if in_transaction:
            raise ValueError("未提供共享数据库连接，不能使用调用方事务模式")
        reviewed = self.preflight(normalized)
        owned_connection = self._connection()
        try:
            return self._apply_on_connection(
                owned_connection,
                normalized,
                reviewed,
                owns_transaction=True,
                target_key=target_key,
            )
        except ValueError:
            try:
                owned_connection.rollback()
            except Exception:
                pass
            raise
        except Exception:
            try:
                owned_connection.rollback()
            except Exception:
                pass
            raise ValueError("原生活动机台事务失败，全部更改已回滚") from None
        finally:
            owned_connection.close()

    def verify(self, config: Any, *, connection=None) -> dict[str, Any]:
        """Read back exact owned state and its source/template receipt."""
        normalized = normalize_config(config)
        owns_connection = connection is None
        active_connection = self._connection() if owns_connection else connection
        try:
            cursor = active_connection.cursor()
            inspection = _inspect(cursor, normalized, locked=False)
            ledger = inspection["ledger"]
            if ledger is None:
                raise ValueError("活动机台尚未安装")
            if (
                inspection["target_hash"] != ledger["InstalledHash"].lower()
                or inspection["action"] != "unchanged"
            ):
                raise ValueError("活动机台已安装状态与审核配置不一致")
            if owns_connection:
                active_connection.rollback()
            return {
                "schema_version": SCHEMA_VERSION,
                "machine_id": normalized["id"],
                "installed_hash": inspection["target_hash"],
                "template_hash": inspection["source"]["template_hash"],
                "current_template_hash": inspection["admission_source"][
                    "template_hash"
                ],
                "source_hash": inspection["source_hash"],
                "runtime_observation": inspection["runtime_observation"],
                "pool_size": POOL_SIZE,
                "prime_rows": PRIME_COUNT,
                "verified": True,
                "player_tables_read": False,
                "transaction_owned_by_caller": not owns_connection,
            }
        except ValueError:
            if owns_connection:
                try:
                    active_connection.rollback()
                except Exception:
                    pass
            raise
        except Exception:
            if owns_connection:
                try:
                    active_connection.rollback()
                except Exception:
                    pass
            raise ValueError("读取活动机台安装状态失败") from None
        finally:
            if owns_connection:
                active_connection.close()
