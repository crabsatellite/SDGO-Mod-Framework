"""Fixed-catalog validation and transactional installation for imported units.

Only the server's non-player definition tables are read or written here.  Shop
listings, prices, gacha machines, inventories, and stored procedures are
outside this repository.
"""
from __future__ import annotations

import configparser
import hashlib
import json
import math
from pathlib import Path
import re
import struct
from typing import Any, Iterable


INT_MAX = 2_147_483_647
FAMILY_KEY = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z")
MANAGED_ITEM_FIRST = 1_119_000
MANAGED_ITEM_LAST = 1_119_999
OWNER_KEY = "sdgo-unit-catalog"
APP_LOCK = "SDGO:unit-import-catalog"

UNIT_COLUMNS = (
    "UnitNumber", "Influence1", "Influence2", "UnitModelName",
    "UnitGradeNumber", "UnitGradeNumber2", "MasteredWeaponType",
    "BattleCost", "Star", "UnitNumber_Transform", "HP", "Shield",
    "ShieldType", "ShieldRate", "DashType", "MoveLevel", "DashLevel",
    "QuickStepLevel", "MotivePower", "BoosterIncrease", "BoosterDecrease",
    "FatalAttackIncrease", "AdaptEnvironType", "AdaptEnvironSpace",
    "AdaptEnvironGround", "AdaptEnvironUnderwater", "AdaptEnvironDesert",
    "AdaptEnvironSky", "Sight", "SkillNumber_1", "SkillNumber_2",
    "SkillNumber_3", "StrikingPower", "DefensivePower",
    "MasteredWeaponIX", "WeaponNumber_1", "WeaponNumber_2",
    "WeaponNumber_3", "WeaponNumber_FatalAttack", "ViewOrder",
    "MaxEnergyAmount", "MaxEnergyCost", "EnergyDecAmount", "UnitGrowIndex",
    "DailyLimit", "MonthLimit", "UserGradeLimit", "SallyReady",
    "CollectionUnit", "UnitGraph",
)
WEAPON_COLUMNS = (
    "WeaponNo", "WeaponName", "AttackStyle", "RangeType", "HitType",
    "DamageSize", "SpeedRate", "RangeSize", "AttackSpeed",
    "ExtraAttackType", "BulletRemain", "ReloadTime", "PushDist", "CoolTime",
    "SpecialFeature", "PushType", "StunBase", "StunFinal", "Pierce",
    "PierceRate", "MovingShot", "MaxCombo", "FunnelID",
)
ITEM_COLUMNS = ("CatalogueID", "Name", "RefIndex", "TimeLimit", "Option1", "Option2")
NAME_COLUMNS = ("UnitNumber", "NameCN", "NameEN")
GRADE_COLUMNS = (
    "UnitGradeNumber", "UnitGradeNumber2", "UnitGradeName",
    "UnitGradeCalled", "GetSpAttack", "GetSpDemage", "GetSpKilled",
    "MixValUp", "MixValKeep", "Parts2Prob", "Parts3Prob", "Parts4Prob",
    "PartsOffset",
)
LEVEL_COLUMNS = (
    "UnitLevelIndex", "UnitLevelNumber", "UnitLevelName",
    "UnitGrowWeighted", "LapUnitExp",
)
SKILL_COLUMNS = (
    "SkillID", "SkillName", "Activate", "ActivateValue", "MoveLv", "DashLv",
    "QuickLv", "MotiveLv", "BoostIncreaseRate", "BoostPower", "Attack",
    "Defense", "RadarRadius", "SpIncreaseRate", "Scale",
    "AutoBulletChargeRate", "BulletChargeRate", "AtkSpdRate", "ActiveResult",
    "Feature", "UseTarget", "BattleMode", "PartSpLevel",
)
FUNNEL_COLUMNS = (
    "FunnelID", "FunnelProp", "AniTime", "AttackDelay", "AttackCount",
    "Stealable",
)
UNIT_GRAPH_COLUMNS = (
    "GraphNumber", "Power", "Armor", "Mobility", "Control", "PosNumber",
    "DefaultSkill_1", "DefaultSkill_2",
)

TABLES = {
    "unit": ("tbl_Units", UNIT_COLUMNS, "UnitNumber"),
    "weapon": ("tbl_WeaponsRev", WEAPON_COLUMNS, "WeaponNo"),
    "item": ("tbl_ItemCategory", ITEM_COLUMNS, "CatalogueID"),
    "name": ("tbl_UnitNames", NAME_COLUMNS, "UnitNumber"),
}
from .blueprint_repository import RECIPE_COLUMNS,INFO_COLUMNS
TABLES.update({'blueprint':('tbl_MixTreeFile',RECIPE_COLUMNS,'Idx'),
               'blueprint_info':('tbl_MixTreeInfo',INFO_COLUMNS,'ItemID')})

# kind, size in bytes, nullability.  The values are checked against the live
# SQL metadata before any catalog row is changed.
SQL_SCHEMAS = {
    "tbl_Units": (
        ("UnitNumber", "int", 4, False), ("Influence1", "tinyint", 1, False),
        ("Influence2", "tinyint", 1, False), ("UnitModelName", "varchar", 100, False),
        ("UnitGradeNumber", "tinyint", 1, False), ("UnitGradeNumber2", "tinyint", 1, False),
        ("MasteredWeaponType", "tinyint", 1, False), ("BattleCost", "smallint", 2, False),
        ("Star", "tinyint", 1, False), ("UnitNumber_Transform", "int", 4, False),
        ("HP", "int", 4, False), ("Shield", "int", 4, True),
        ("ShieldType", "tinyint", 1, False), ("ShieldRate", "int", 4, True),
        ("DashType", "tinyint", 1, False), ("MoveLevel", "tinyint", 1, False),
        ("DashLevel", "tinyint", 1, False), ("QuickStepLevel", "tinyint", 1, False),
        ("MotivePower", "tinyint", 1, False), ("BoosterIncrease", "smallint", 2, False),
        ("BoosterDecrease", "smallint", 2, False),
        ("FatalAttackIncrease", "smallint", 2, False),
        ("AdaptEnvironType", "tinyint", 1, False),
        ("AdaptEnvironSpace", "smallint", 2, False),
        ("AdaptEnvironGround", "smallint", 2, False),
        ("AdaptEnvironUnderwater", "smallint", 2, False),
        ("AdaptEnvironDesert", "smallint", 2, False),
        ("AdaptEnvironSky", "smallint", 2, False), ("Sight", "smallint", 2, False),
        ("SkillNumber_1", "int", 4, False), ("SkillNumber_2", "int", 4, False),
        ("SkillNumber_3", "int", 4, False), ("StrikingPower", "smallint", 2, False),
        ("DefensivePower", "smallint", 2, False), ("MasteredWeaponIX", "int", 4, False),
        ("WeaponNumber_1", "int", 4, True), ("WeaponNumber_2", "int", 4, True),
        ("WeaponNumber_3", "int", 4, True),
        ("WeaponNumber_FatalAttack", "int", 4, True), ("ViewOrder", "int", 4, False),
        ("MaxEnergyAmount", "real", 4, True), ("MaxEnergyCost", "real", 4, True),
        ("EnergyDecAmount", "tinyint", 1, True), ("UnitGrowIndex", "int", 4, True),
        ("DailyLimit", "int", 4, True), ("MonthLimit", "int", 4, True),
        ("UserGradeLimit", "int", 4, True), ("SallyReady", "tinyint", 1, True),
        ("CollectionUnit", "tinyint", 1, True), ("UnitGraph", "int", 4, True),
    ),
    "tbl_WeaponsRev": (
        ("WeaponNo", "int", 4, False), ("WeaponName", "varchar", 50, False),
        ("AttackStyle", "tinyint", 1, False), ("RangeType", "tinyint", 1, False),
        ("HitType", "tinyint", 1, False), ("DamageSize", "int", 4, False),
        ("SpeedRate", "smallint", 2, False), ("RangeSize", "int", 4, False),
        ("AttackSpeed", "real", 4, False), ("ExtraAttackType", "tinyint", 1, False),
        ("BulletRemain", "smallint", 2, False), ("ReloadTime", "smallint", 2, False),
        ("PushDist", "smallint", 2, False), ("CoolTime", "smallint", 2, False),
        ("SpecialFeature", "int", 4, False), ("PushType", "tinyint", 1, False),
        ("StunBase", "smallint", 2, False), ("StunFinal", "smallint", 2, False),
        ("Pierce", "tinyint", 1, False), ("PierceRate", "tinyint", 1, False),
        ("MovingShot", "tinyint", 1, False), ("MaxCombo", "tinyint", 1, False),
        ("FunnelID", "int", 4, True),
    ),
    "tbl_ItemCategory": (
        ("CatalogueID", "int", 4, False), ("Name", "varchar", 50, True),
        ("RefIndex", "int", 4, False), ("TimeLimit", "int", 4, False),
        ("Option1", "int", 4, False), ("Option2", "int", 4, False),
    ),
    "tbl_UnitNames": (
        ("UnitNumber", "int", 4, False), ("NameCN", "nvarchar", 160, True),
        ("NameEN", "varchar", 120, True),
    ),
}
LEDGER_SCHEMA = (
    ("RecordKind", "varchar", 16, False),
    ("RecordID", "int", 4, False),
    ("OwnerKey", "nvarchar", 256, False),
    ("FamilyKey", "nvarchar", 256, False),
    ("RecordKey", "nvarchar", 600, False),
    ("Revision", "nvarchar", 256, False),
    ("OriginalJson", "nvarchar", -1, False),
    ("InstalledHash", "char", 64, False),
    # SQL Server stores datetime2(0) in six bytes (precision 0-2).
    ("UpdatedAt", "datetime2", 6, False),
)

_INTEGER_KINDS = {
    "tinyint": (0, 255),
    "smallint": (-32_768, 32_767),
    "int": (-2_147_483_648, INT_MAX),
    "bigint": (-9_223_372_036_854_775_808,9_223_372_036_854_775_807),
}


def _read_ini(path: Path) -> configparser.ConfigParser:
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = raw.decode("utf-16")
    else:
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            text = raw.decode("mbcs")
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(text)
    if "DB Game" not in parser:
        raise ValueError("服务端配置缺少 [DB Game]")
    return parser


def database_target_key(server_ini: Path) -> str:
    db = _read_ini(Path(server_ini))['DB Game']
    identity = [db['DBServerName'].strip().casefold(), db['DBName'].strip().casefold()]
    return hashlib.sha256(json.dumps(identity, ensure_ascii=True).encode()).hexdigest()


def _connect(server_ini: Path):
    try:
        import pyodbc
    except ImportError:
        raise ValueError("本机缺少 pyodbc，无法读取服务端固定目录") from None
    parser = _read_ini(server_ini)
    db = parser["DB Game"]
    required = ("DBServerName", "DBName", "ID", "PWD")
    if any(not db.get(key) for key in required):
        raise ValueError("服务端 [DB Game] 配置不完整")
    drivers = [name for name in pyodbc.drivers() if "SQL Server" in name]
    if not drivers:
        raise ValueError("本机缺少 SQL Server ODBC 驱动")

    def escaped(value: str) -> str:
        return "{" + str(value).replace("}", "}}") + "}"

    connection_string = (
        f"DRIVER={escaped(drivers[-1])};"
        f"SERVER={escaped(db['DBServerName'])};"
        f"DATABASE={escaped(db['DBName'])};"
        f"UID={escaped(db['ID'])};PWD={escaped(db['PWD'])};"
    )
    try:
        connection = pyodbc.connect(
            connection_string, timeout=5, autocommit=False
        )
        connection.timeout = 30
        return connection
    except pyodbc.Error as error:
        state = error.args[0] if error.args else "unknown"
        raise ValueError(f"服务端连接失败，SQLSTATE={state}") from None


def _empty_catalog() -> dict[str, Any]:
    return {
        "units": {},
        "weapons": {},
        "items": {},
        "names": {},
        "grades": set(),
        "level_indices": set(),
        "skill_ids": set(),
        "funnel_ids": set(),
        "unit_graph_ids": set(),
    }


def _json_rows(path: Path) -> list[dict[str, Any]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        raise ValueError(f"固定目录快照无法读取：{path.name}") from None
    if not isinstance(value, list) or any(not isinstance(row, dict) for row in value):
        raise ValueError(f"固定目录快照格式错误：{path.name}")
    return value


def _index(rows: Iterable[dict[str, Any]], key: str, label: str) -> dict[int, dict[str, Any]]:
    result: dict[int, dict[str, Any]] = {}
    for row in rows:
        value = row.get(key)
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"{label} 含无效主键")
        if value in result:
            raise ValueError(f"{label} 含重复主键 {value}")
        result[value] = dict(row)
    return result


def _load_snapshot(path: Path) -> tuple[dict[str, Any], dict[str, bool]]:
    if not path.is_dir():
        raise ValueError("固定目录快照必须是数据库 JSON 导出目录")
    catalog = _empty_catalog()
    metadata = {
        "units": True, "weapons": True, "items": True, "names": True,
        "grades": True, "levels": True, "skills": True,
        "funnels": True, "unit_graphs": True,
    }

    def parts(names: tuple[str, ...], label: str) -> list[dict[str, Any]]:
        rows: list[dict[str, Any]] = []
        for name in names:
            file = path / name
            if not file.is_file():
                raise ValueError(f"固定目录快照缺少 {label}：{name}")
            rows.extend(_json_rows(file))
        return rows

    units = parts(
        ("dbo.tbl_Units.part1.json", "dbo.tbl_Units.part2.json"), "机体表"
    )
    weapons = parts(
        (
            "dbo.tbl_WeaponsRev.part1.json",
            "dbo.tbl_WeaponsRev.part2.json",
            "dbo.tbl_WeaponsRev.part3.json",
            "dbo.tbl_WeaponsRev.part4.json",
        ),
        "武器表",
    )
    items = parts(("dbo.tbl_ItemCategory.json",), "物品类别表")
    grades = parts(("dbo.tbl_UnitGrades.json",), "等级表")
    levels = parts(("dbo.tbl_UnitLevels.json",), "成长表")

    catalog["units"] = _index(units, "UnitNumber", "tbl_Units")
    catalog["weapons"] = _index(weapons, "WeaponNo", "tbl_WeaponsRev")
    catalog["items"] = _index(items, "CatalogueID", "tbl_ItemCategory")
    catalog["grades"] = {
        (row.get("UnitGradeNumber"), row.get("UnitGradeNumber2"))
        for row in grades
    }
    catalog["level_indices"] = {row.get("UnitLevelIndex") for row in levels}

    names_file = path / "dbo.tbl_UnitNames.json"
    if names_file.is_file():
        catalog["names"] = _index(_json_rows(names_file), "UnitNumber", "tbl_UnitNames")
    else:
        metadata["names"] = False

    # The shipped database export calls the native skill table tbl_SkillRev.
    # A missing file is kept as an explicit pending dependency, never treated
    # as proof that all skill ids are absent.
    skill_file = path / "dbo.tbl_SkillRev.json"
    if skill_file.is_file():
        skill_rows = _json_rows(skill_file)
        catalog["skill_ids"] = set(_index(skill_rows, "SkillID", "tbl_SkillRev"))
    else:
        metadata["skills"] = False
    funnel_file = path / "dbo.tbl_Funnel.json"
    if funnel_file.is_file():
        funnel_rows = _json_rows(funnel_file)
        catalog["funnel_ids"] = set(_index(funnel_rows, "FunnelID", "tbl_Funnel"))
    else:
        metadata["funnels"] = False
    graph_file = path / "dbo.tbl_UnitGraph.json"
    if graph_file.is_file():
        graph_rows = _json_rows(graph_file)
        catalog["unit_graph_ids"] = set(
            _index(graph_rows, "GraphNumber", "tbl_UnitGraph")
        )
    else:
        metadata["unit_graphs"] = False
    files={name:path/('dbo.'+name+'.json') for name in ('tbl_MixTreeFile','tbl_MixTreeInfo','tbl_MixTreeList')}
    if all(file.is_file() for file in files.values()):
        catalog['blueprints']=_index(_json_rows(files['tbl_MixTreeFile']),'Idx','tbl_MixTreeFile')
        catalog['blueprint_infos']=_index(_json_rows(files['tbl_MixTreeInfo']),'ItemID','tbl_MixTreeInfo')
        catalog['blueprint_materials']={}
        for row in _json_rows(files['tbl_MixTreeList']):catalog['blueprint_materials'].setdefault(row['ItemID'],[]).append(row)
    return catalog, metadata


def _object_exists(cursor, table: str) -> bool:
    row = cursor.execute("SELECT OBJECT_ID(?, N'U')", "dbo." + table).fetchone()
    return row is not None and row[0] is not None


def _select_rows(cursor, table: str, columns: tuple[str, ...]) -> list[dict[str, Any]]:
    cursor.execute(f"SELECT {','.join(columns)} FROM dbo.{table}")
    return [dict(zip(columns, row)) for row in cursor.fetchall()]


def _load_cursor_catalog(cursor) -> tuple[dict[str, Any], dict[str, bool]]:
    catalog = _empty_catalog()
    metadata = {
        "units": True, "weapons": True, "items": True, "names": True,
        "grades": True, "levels": True, "skills": True,
        "funnels": True, "unit_graphs": True,
    }
    required = (
        ("tbl_Units", UNIT_COLUMNS), ("tbl_WeaponsRev", WEAPON_COLUMNS),
        ("tbl_ItemCategory", ITEM_COLUMNS), ("tbl_UnitGrades", GRADE_COLUMNS),
        ("tbl_UnitLevels", LEVEL_COLUMNS), ("tbl_SkillRev", SKILL_COLUMNS),
    )
    for table, _ in required:
        if not _object_exists(cursor, table):
            if table == "tbl_SkillRev":
                metadata["skills"] = False
                continue
            raise ValueError(f"服务端缺少固定目录表 dbo.{table}")

    catalog["units"] = _index(
        _select_rows(cursor, "tbl_Units", UNIT_COLUMNS), "UnitNumber", "tbl_Units"
    )
    catalog["weapons"] = _index(
        _select_rows(cursor, "tbl_WeaponsRev", WEAPON_COLUMNS),
        "WeaponNo", "tbl_WeaponsRev",
    )
    catalog["items"] = _index(
        _select_rows(cursor, "tbl_ItemCategory", ITEM_COLUMNS),
        "CatalogueID", "tbl_ItemCategory",
    )
    grades = _select_rows(cursor, "tbl_UnitGrades", GRADE_COLUMNS)
    levels = _select_rows(cursor, "tbl_UnitLevels", LEVEL_COLUMNS)
    catalog["grades"] = {
        (row["UnitGradeNumber"], row["UnitGradeNumber2"]) for row in grades
    }
    catalog["level_indices"] = {row["UnitLevelIndex"] for row in levels}
    if metadata["skills"]:
        catalog["skill_ids"] = {
            row["SkillID"]
            for row in _select_rows(cursor, "tbl_SkillRev", SKILL_COLUMNS)
        }
    if _object_exists(cursor, "tbl_Funnel"):
        catalog["funnel_ids"] = {
            row["FunnelID"]
            for row in _select_rows(cursor, "tbl_Funnel", FUNNEL_COLUMNS)
        }
    else:
        metadata["funnels"] = False
    if _object_exists(cursor, "tbl_UnitGraph"):
        catalog["unit_graph_ids"] = {
            row["GraphNumber"]
            for row in _select_rows(cursor, "tbl_UnitGraph", UNIT_GRAPH_COLUMNS)
        }
    else:
        metadata["unit_graphs"] = False
    if _object_exists(cursor, "tbl_UnitNames"):
        catalog["names"] = _index(
            _select_rows(cursor, "tbl_UnitNames", NAME_COLUMNS),
            "UnitNumber", "tbl_UnitNames",
        )
    from .blueprint_repository import load
    catalog.update(load(cursor))
    return catalog, metadata


def _canonical_json(value: Any) -> str:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
        allow_nan=False,
    )


def _row_hash(row: dict[str, Any], columns: tuple[str, ...]) -> str:
    ordered = {column: row[column] for column in columns}
    return hashlib.sha256(_canonical_json(ordered).encode("utf-8")).hexdigest()


def _readback_matches(
    expected: dict[str, Any], actual: dict[str, Any], table: str
) -> bool:
    from .blueprint_repository import schema as blueprint_schema
    specs=SQL_SCHEMAS.get(table,blueprint_schema().get(table))
    for name, kind, _, _ in specs:
        left, right = expected[name], actual[name]
        if kind == "real" and left is not None:
            left = struct.unpack("<f", struct.pack("<f", float(left)))[0]
        if left != right:
            return False
    return True


def _read_ownership(cursor) -> dict[tuple[str, int], tuple[str, str]]:
    if not _object_exists(cursor, "sdgo_UnitImportDefinitions"):
        return {}
    rows = cursor.execute(
        "SELECT RecordKind,RecordID,FamilyKey,InstalledHash "
        "FROM dbo.sdgo_UnitImportDefinitions"
    ).fetchall()
    return {
        (str(kind), int(identity)): (str(family), str(digest).lower())
        for kind, identity, family, digest in rows
    }


def _load_server(server_ini: Path) -> tuple[
    dict[str, Any], dict[str, bool], dict[tuple[str, int], tuple[str, str]]
]:
    connection = _connect(server_ini)
    try:
        cursor = connection.cursor()
        catalog, metadata = _load_cursor_catalog(cursor)
        ownership = _read_ownership(cursor)
        connection.rollback()
        return catalog, metadata, ownership
    except ValueError:
        connection.rollback()
        raise
    except Exception:
        connection.rollback()
        raise ValueError("读取服务端固定目录失败，未进行任何写入") from None
    finally:
        connection.close()


def _validate_scalar(name: str, value: Any, sql_kind: str, size: int, nullable: bool) -> None:
    if value is None:
        if nullable:
            return
        raise ValueError(f"{name} 不能为空")
    if sql_kind in _INTEGER_KINDS:
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"{name} 必须是整数")
        low, high = _INTEGER_KINDS[sql_kind]
        if not low <= value <= high:
            raise ValueError(f"{name} 超出 SQL {sql_kind} 范围")
        return
    if sql_kind == "real":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError(f"{name} 必须是有限数字")
        if not math.isfinite(float(value)) or abs(float(value)) > 3.4e38:
            raise ValueError(f"{name} 超出 SQL real 范围")
        return
    if sql_kind in ("varchar", "nvarchar"):
        if not isinstance(value, str) or "\0" in value:
            raise ValueError(f"{name} 必须是不含 NUL 的文本")
        character_limit = size // 2 if sql_kind == "nvarchar" else size
        if len(value) > character_limit:
            raise ValueError(f"{name} 超过数据库长度 {character_limit}")
        return
    raise AssertionError(f"unsupported SQL kind {sql_kind}")


def _validate_complete_row(
    row: Any, table: str, columns: tuple[str, ...]
) -> dict[str, Any]:
    if not isinstance(row, dict):
        raise ValueError(f"{table} 行必须是对象")
    unknown = set(row) - set(columns)
    missing = set(columns) - set(row)
    if unknown:
        raise ValueError(f"{table} 含未知字段：{','.join(sorted(unknown))}")
    if missing:
        raise ValueError(f"{table} 缺少必填字段：{','.join(sorted(missing))}")
    specs = SQL_SCHEMAS[table]
    for name, kind, size, nullable in specs:
        _validate_scalar(f"{table}.{name}", row[name], kind, size, nullable)
    return dict(row)


def _validate_plan(
    plan: Any,
    catalog: dict[str, Any],
    metadata: dict[str, bool],
    ownership: dict[tuple[str, int], tuple[str, str]],
) -> dict[str, Any]:
    fields = {
        "family_key", "revision", "root_unit_id", "item_template_id",
        "units", "weapons", "item", "names",
    }
    if not isinstance(plan, dict):
        raise ValueError("导入计划必须是对象")
    if set(plan)-{'blueprints'} != fields:
        unknown = set(plan) - fields-{'blueprints'}
        missing = fields - set(plan)
        details = []
        if unknown:
            details.append("未知字段 " + ",".join(sorted(unknown)))
        if missing:
            details.append("缺少字段 " + ",".join(sorted(missing)))
        raise ValueError("导入计划结构错误：" + "；".join(details))
    family = plan["family_key"]
    revision = plan["revision"]
    if not isinstance(family, str) or FAMILY_KEY.fullmatch(family) is None:
        raise ValueError("family_key 必须是 1-128 位 ASCII 标识")
    if (
        not isinstance(revision, str) or not revision.strip()
        or len(revision) > 128 or any(ord(ch) < 32 for ch in revision)
    ):
        raise ValueError("revision 必须是 1-128 位可显示文本")
    for key in ("root_unit_id", "item_template_id"):
        value = plan[key]
        if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= INT_MAX:
            raise ValueError(f"{key} 必须是正整数")
    if not isinstance(plan["units"], list) or not plan["units"]:
        raise ValueError("units 至少需要一条完整机体定义")
    if not isinstance(plan["weapons"], list):
        raise ValueError("weapons 必须是数组")
    if not isinstance(plan["names"], list) or not plan["names"]:
        raise ValueError("names 至少需要一条名称定义")

    units = [
        _validate_complete_row(row, "tbl_Units", UNIT_COLUMNS)
        for row in plan["units"]
    ]
    weapons = [
        _validate_complete_row(row, "tbl_WeaponsRev", WEAPON_COLUMNS)
        for row in plan["weapons"]
    ]
    item = _validate_complete_row(plan["item"], "tbl_ItemCategory", ITEM_COLUMNS)
    names = [
        _validate_complete_row(row, "tbl_UnitNames", NAME_COLUMNS)
        for row in plan["names"]
    ]

    def unique(rows: list[dict[str, Any]], key: str, label: str) -> dict[int, dict[str, Any]]:
        result: dict[int, dict[str, Any]] = {}
        for row in rows:
            identity = row[key]
            if identity <= 0:
                raise ValueError(f"{label} 主键必须是正整数")
            if identity in result:
                raise ValueError(f"{label} 主键重复：{identity}")
            result[identity] = row
        return result

    unit_map = unique(units, "UnitNumber", "机体")
    weapon_map = unique(weapons, "WeaponNo", "武器")
    name_map = unique(names, "UnitNumber", "名称")
    for identity, row in unit_map.items():
        if not row["UnitModelName"].strip():
            raise ValueError(f"机体 {identity} 的 UnitModelName 不能为空")
        if row["HP"] <= 0:
            raise ValueError(f"机体 {identity} 的 HP 必须大于零")
        if row["UnitNumber_Transform"] == identity:
            raise ValueError(f"机体 {identity} 不能把自身作为变形目标")
    for identity, row in weapon_map.items():
        if not row["WeaponName"].strip():
            raise ValueError(f"武器 {identity} 的 WeaponName 不能为空")
    for identity, row in name_map.items():
        if not isinstance(row["NameCN"], str) or not row["NameCN"].strip():
            raise ValueError(f"机体 {identity} 的 NameCN 不能为空")
    if not isinstance(item["Name"], str) or not item["Name"].strip():
        raise ValueError("物品类别 Name 不能为空")
    root = plan["root_unit_id"]
    if root not in unit_map:
        raise ValueError("root_unit_id 必须指向本次导入的机体")
    if set(name_map) != set(unit_map):
        raise ValueError("每个导入机体必须且只能有一条名称定义")
    if unit_map[root]["SallyReady"] != 1:
        raise ValueError("根机体必须明确设置 SallyReady=1")
    for identity, row in unit_map.items():
        if identity != root and row["SallyReady"] != 0:
            raise ValueError(f"非根形态 {identity} 必须设置 SallyReady=0")

    if not all(metadata.get(key, False) for key in ("units", "weapons", "items", "grades", "levels")):
        raise ValueError("固定目录元数据不完整，依赖检查仍为 pending")
    existing_units = catalog["units"]
    existing_weapons = catalog["weapons"]
    all_units = set(existing_units) | set(unit_map)
    all_weapons = set(existing_weapons) | set(weapon_map)
    for identity, row in unit_map.items():
        transform = row["UnitNumber_Transform"]
        if transform and transform not in all_units:
            raise ValueError(f"机体 {identity} 的变形目标 {transform} 不存在")
        for field in (
            "WeaponNumber_1", "WeaponNumber_2", "WeaponNumber_3",
            "WeaponNumber_FatalAttack",
        ):
            weapon = row[field]
            if weapon and weapon not in all_weapons:
                raise ValueError(f"机体 {identity} 的 {field}={weapon} 不存在")
        grade = (row["UnitGradeNumber"], row["UnitGradeNumber2"])
        if grade not in catalog["grades"]:
            raise ValueError(f"机体 {identity} 的等级组合 {grade} 不存在")
        if row["UnitGrowIndex"] not in catalog["level_indices"]:
            raise ValueError(
                f"机体 {identity} 的成长索引 {row['UnitGrowIndex']} 不存在"
            )
        skills = [
            row[field]
            for field in ("SkillNumber_1", "SkillNumber_2", "SkillNumber_3")
            if row[field]
        ]
        if skills and not metadata.get("skills", False):
            raise ValueError("服务端技能目录元数据缺失，技能依赖检查仍为 pending")
        for skill in skills:
            if skill not in catalog["skill_ids"]:
                raise ValueError(f"机体 {identity} 的技能 {skill} 不存在")
        graph = row["UnitGraph"]
        if graph and not metadata.get("unit_graphs", False):
            raise ValueError("服务端属性图目录元数据缺失，UnitGraph 检查仍为 pending")
        if graph and graph not in catalog["unit_graph_ids"]:
            raise ValueError(f"机体 {identity} 的 UnitGraph {graph} 不存在")

    used_funnels = {
        row["FunnelID"] for row in weapon_map.values() if row["FunnelID"]
    }
    if used_funnels and not metadata.get("funnels", False):
        raise ValueError("服务端浮游炮目录元数据缺失，FunnelID 检查仍为 pending")
    for funnel in sorted(used_funnels):
        if funnel not in catalog["funnel_ids"]:
            raise ValueError(f"武器引用的 FunnelID {funnel} 不存在")

    template = catalog["items"].get(plan["item_template_id"])
    if (
        template is None
        or plan["item_template_id"] // 100_000 != 11
        or MANAGED_ITEM_FIRST <= plan["item_template_id"] <= MANAGED_ITEM_LAST
        or ("item", plan["item_template_id"]) in ownership
    ):
        raise ValueError("item_template_id 必须是现有 type 11 机体类别")
    if item["CatalogueID"] == plan["item_template_id"]:
        raise ValueError("新物品类别不能覆盖模板")
    if not MANAGED_ITEM_FIRST <= item["CatalogueID"] <= MANAGED_ITEM_LAST:
        raise ValueError("CatalogueID 必须位于模块保留范围 1119000..1119999")
    if item["CatalogueID"] // 100_000 != 11:
        raise ValueError("CatalogueID 必须保持 type 11 机体类别")
    if item["RefIndex"] != root:
        raise ValueError("物品类别 RefIndex 必须指向 root_unit_id")
    for field in ("TimeLimit", "Option1", "Option2"):
        if item[field] != template[field]:
            raise ValueError(f"物品类别 {field} 必须沿用所选模板")

    def check_collision(
        kind: str, identity: int, row: dict[str, Any],
        existing: dict[int, dict[str, Any]], columns: tuple[str, ...],
    ) -> None:
        if identity not in existing:
            if (kind, identity) in ownership:
                raise ValueError(f"{kind} {identity} 的所有权账本存在但目录行缺失")
            return
        owned = ownership.get((kind, identity))
        if owned is None:
            raise ValueError(f"{kind} {identity} 已被原生或其他内容占用")
        owned_family, installed_hash = owned
        if owned_family != family:
            raise ValueError(f"{kind} {identity} 属于其他导入 family")
        if _row_hash(existing[identity], columns) != installed_hash:
            raise ValueError(f"{kind} {identity} 已在账本外被修改，拒绝覆盖")

    for identity, row in unit_map.items():
        check_collision("unit", identity, row, existing_units, UNIT_COLUMNS)
    for identity, row in weapon_map.items():
        check_collision("weapon", identity, row, existing_weapons, WEAPON_COLUMNS)
    check_collision(
        "item", item["CatalogueID"], item, catalog["items"], ITEM_COLUMNS
    )
    for identity, row in name_map.items():
        check_collision("name", identity, row, catalog["names"], NAME_COLUMNS)

    result = {
        "family_key": family,
        "revision": revision,
        "root_unit_id": root,
        "item_template_id": plan["item_template_id"],
        "units": units,
        "weapons": weapons,
        "item": item,
        "names": names,
    }
    if plan.get('blueprints'):
        from .blueprint_repository import validate
        result['blueprints']=validate(plan['blueprints'],root=root,unit_ids=all_units,
            catalog=catalog,ownership=ownership,family=family)
    return result


def _ensure_support_schema(cursor) -> None:
    cursor.execute(
        """
IF OBJECT_ID(N'dbo.tbl_UnitNames', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.tbl_UnitNames(
        UnitNumber int NOT NULL PRIMARY KEY,
        NameCN nvarchar(80) NULL,
        NameEN varchar(120) NULL
    );
END;
IF OBJECT_ID(N'dbo.sdgo_UnitImportDefinitions', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.sdgo_UnitImportDefinitions(
        RecordKind varchar(16) NOT NULL,
        RecordID int NOT NULL,
        OwnerKey nvarchar(128) NOT NULL,
        FamilyKey nvarchar(128) NOT NULL,
        RecordKey nvarchar(300) NOT NULL,
        Revision nvarchar(128) NOT NULL,
        OriginalJson nvarchar(max) NOT NULL,
        InstalledHash char(64) NOT NULL,
        UpdatedAt datetime2(0) NOT NULL
            CONSTRAINT DF_sdgo_UnitImportDefinitions_UpdatedAt DEFAULT SYSUTCDATETIME(),
        CONSTRAINT PK_sdgo_UnitImportDefinitions
            PRIMARY KEY(RecordKind, RecordID)
    );
END;
"""
    )


def _validate_live_schema(cursor) -> None:
    for table, expected in {**SQL_SCHEMAS, "sdgo_UnitImportDefinitions": LEDGER_SCHEMA}.items():
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
        actual = tuple(
            (str(name), str(kind).lower(), int(size), bool(nullable))
            for name, kind, size, nullable in rows
        )
        if actual != expected:
            raise ValueError(f"服务端 dbo.{table} 结构与支持版本不一致")


def _fetch_record(cursor, table: str, columns: tuple[str, ...], key: str, identity: int):
    row = cursor.execute(
        f"SELECT {','.join(columns)} FROM dbo.{table} WHERE {key}=?",
        identity,
    ).fetchone()
    return None if row is None else dict(zip(columns, row))


def _write_record(
    cursor,
    *,
    kind: str,
    row: dict[str, Any],
    family: str,
    revision: str,
) -> tuple[str, str]:
    table, columns, key = TABLES[kind]
    identity = row[key]
    current = _fetch_record(cursor, table, columns, key, identity)
    ledger = cursor.execute(
        """
SELECT OwnerKey,FamilyKey,OriginalJson,InstalledHash
FROM dbo.sdgo_UnitImportDefinitions WITH (UPDLOCK,HOLDLOCK)
WHERE RecordKind=? AND RecordID=?
""",
        kind, identity,
    ).fetchone()
    if current is None and ledger is not None:
        raise ValueError(f"{kind} {identity} 的目录行缺失，拒绝重建")
    if current is not None:
        if ledger is None:
            raise ValueError(f"{kind} {identity} 已被原生或其他内容占用")
        owner, old_family, original_json, installed_hash = ledger
        if str(owner) != OWNER_KEY or str(old_family) != family:
            raise ValueError(f"{kind} {identity} 不属于当前 family")
        if _row_hash(current, columns) != str(installed_hash).lower():
            raise ValueError(f"{kind} {identity} 已在账本外被修改")
        assignments = ",".join(f"{column}=?" for column in columns if column != key)
        values = [row[column] for column in columns if column != key]
        cursor.execute(
            f"UPDATE dbo.{table} SET {assignments} WHERE {key}=?",
            *values, identity,
        )
        action = "updated"
    else:
        original_json = "null"
        cursor.execute(
            f"INSERT dbo.{table}({','.join(columns)}) "
            f"VALUES({','.join('?' for _ in columns)})",
            *(row[column] for column in columns),
        )
        action = "inserted"

    installed = _fetch_record(cursor, table, columns, key, identity)
    if installed is None:
        raise ValueError(f"{kind} {identity} 写入后无法读回")
    if not _readback_matches(row, installed, table):
        raise ValueError(f"{kind} {identity} 数据库读回与请求不一致，拒绝有损写入")
    installed_hash = _row_hash(installed, columns)
    record_key = f"{family}/{kind}/{identity}"
    if ledger is None:
        cursor.execute(
            """
INSERT dbo.sdgo_UnitImportDefinitions(
    RecordKind,RecordID,OwnerKey,FamilyKey,RecordKey,Revision,
    OriginalJson,InstalledHash,UpdatedAt
) VALUES(?,?,?,?,?,?,?,?,SYSUTCDATETIME())
""",
            kind, identity, OWNER_KEY, family, record_key, revision,
            original_json, installed_hash,
        )
    else:
        cursor.execute(
            """
UPDATE dbo.sdgo_UnitImportDefinitions
SET RecordKey=?,Revision=?,InstalledHash=?,UpdatedAt=SYSUTCDATETIME()
WHERE RecordKind=? AND RecordID=?
""",
            record_key, revision, installed_hash, kind, identity,
        )
    return action, installed_hash


class Repository:
    """Read fixed catalogs and atomically install a confirmed unit family."""

    def __init__(
        self,
        snapshot: Path | None = None,
        server_ini: Path | None = None,
    ):
        self.snapshot = Path(snapshot) if snapshot is not None else None
        self.server_ini = Path(server_ini) if server_ini is not None else None
        self._ownership: dict[tuple[str, int], tuple[str, str]] = {}
        if self.snapshot is not None:
            self.catalog, self._metadata = _load_snapshot(self.snapshot)
            self._source = "snapshot"
        elif self.server_ini is not None:
            self.catalog, self._metadata, self._ownership = _load_server(
                self.server_ini
            )
            self._source = "server"
        else:
            self.catalog = _empty_catalog()
            self._metadata = {
                key: False for key in (
                    "units", "weapons", "items", "names",
                    "grades", "levels", "skills",
                    "funnels", "unit_graphs",
                )
            }
            self._source = "unavailable"

    def public_info(self) -> dict[str, Any]:
        """Return capability/count information without paths or credentials."""
        return {
            "source": self._source,
            "snapshot_configured": self.snapshot is not None,
            "server_configured": self.server_ini is not None,
            "apply_available": self.server_ini is not None,
            "metadata": dict(self._metadata),
            "counts": {
                "units": len(self.catalog["units"]),
                "weapons": len(self.catalog["weapons"]),
                "items": len(self.catalog["items"]),
                "names": len(self.catalog["names"]),
                "grades": len(self.catalog["grades"]),
                "level_indices": len(self.catalog["level_indices"]),
                "skill_ids": len(self.catalog["skill_ids"]),
                "funnel_ids": len(self.catalog["funnel_ids"]),
                "unit_graph_ids": len(self.catalog["unit_graph_ids"]),
            },
            "cache_restart_required_after_apply": True,
        }

    def check_plan(self, plan: Any) -> dict[str, Any]:
        """Validate a complete plan without changing the server."""
        validated = _validate_plan(
            plan, self.catalog, self._metadata, self._ownership
        )
        return {
            "family_key": validated["family_key"],
            "revision": validated["revision"],
            "root_unit_id": validated["root_unit_id"],
            "units": len(validated["units"]),
            "weapons": len(validated["weapons"]),
            "names": len(validated["names"]),
            "item_catalogue_id": validated["item"]["CatalogueID"],
            "valid": True,
        }

    def apply(self, plan: Any) -> dict[str, Any]:
        """Apply all fixed definitions together; never restart game services."""
        if self.server_ini is None:
            raise ValueError("未配置服务端 INI，离线模式不能执行 apply")
        # Fast owner-facing diagnostics first.  The same checks are repeated
        # against live fixed tables after the transaction lock is acquired.
        self.check_plan(plan)
        target_key = database_target_key(self.server_ini)
        connection = _connect(self.server_ini)
        try:
            cursor = connection.cursor()
            cursor.execute("SET XACT_ABORT ON")
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
                raise ValueError("另一项机体目录更新正在进行，请稍后重试")

            _ensure_support_schema(cursor)
            _validate_live_schema(cursor)
            if plan.get('blueprints'):
                from .blueprint_repository import validate_schema
                validate_schema(cursor)
            live_catalog, live_metadata = _load_cursor_catalog(cursor)
            live_ownership = _read_ownership(cursor)
            validated = _validate_plan(
                plan, live_catalog, live_metadata, live_ownership
            )

            counts = {
                "inserted": {"weapons": 0, "units": 0, "names": 0, "items": 0},
                "updated": {"weapons": 0, "units": 0, "names": 0, "items": 0},
            }
            installed_hashes: dict[tuple[str, int], str] = {}
            groups = (
                ("weapon", "weapons", validated["weapons"], "WeaponNo"),
                ("unit", "units", validated["units"], "UnitNumber"),
                ("name", "names", validated["names"], "UnitNumber"),
                ("item", "items", [validated["item"]], "CatalogueID"),
            )
            for kind, plural, rows, key in groups:
                for row in rows:
                    action, digest = _write_record(
                        cursor, kind=kind, row=row,
                        family=validated["family_key"],
                        revision=validated["revision"],
                    )
                    counts[action][plural] += 1
                    installed_hashes[(kind, row[key])] = digest
            if validated.get('blueprints'):
                from .blueprint_repository import apply_materials
                counts['inserted'].update(blueprints=0,blueprint_infos=0,blueprint_lists=0)
                counts['updated'].update(blueprints=0,blueprint_infos=0,blueprint_lists=0)
                for entry in validated['blueprints']:
                    for kind,plural,row,key in (
                        ('blueprint','blueprints',entry['recipe'],'Idx'),
                        ('blueprint_info','blueprint_infos',entry['info'],'ItemID'),
                        ('item','items',entry['item'],'CatalogueID')):
                        action,digest=_write_record(cursor,kind=kind,row=row,family=validated['family_key'],revision=validated['revision'])
                        counts[action][plural]+=1;installed_hashes[(kind,row[key])]=digest
                    action,digest=apply_materials(cursor,entry['materials'],family=validated['family_key'],revision=validated['revision'])
                    counts[action]['blueprint_lists']+=1
                    installed_hashes[('blueprint_list',entry['recipe']['Idx'])]=digest
            # A linked form is not a second inventory prize.  Keep the existing
            # collection feature's live root registry in this same transaction.
            collection_registered = False
            if cursor.execute("SELECT SCHEMA_ID(N'sdgo_collection_capsule')").fetchone()[0] is not None:
                from owner.collection_capsule.deployment import register_unit_family
                declaration = {
                    'protocol': 'sdgo.unit-family', 'protocol_version': 1,
                    'installable': True, 'package_id': validated['family_key'],
                    'family': {
                        'root_form': validated['root_unit_id'],
                        'forms': [{'id': row['UnitNumber'], 'key': str(row['UnitNumber']),
                                   'transform_target': row.get('UnitNumber_Transform') or 0}
                                  for row in validated['units']],
                    },
                }
                register_unit_family(connection, declaration, in_transaction=True)
                collection_registered = True
            # pyodbc autocommit=False plus an explicit BEGIN can otherwise
            # leave a nested transaction open on older SQL Server drivers.
            # Close the SQL transaction explicitly before ending the ODBC
            # transaction so connection.close() cannot roll back a "success".
            cursor.execute("COMMIT TRANSACTION")
            connection.commit()

            # Keep this long-lived instance coherent for a later same-family
            # revision without querying credentials or player data again.
            for row in validated["weapons"]:
                self.catalog["weapons"][row["WeaponNo"]] = dict(row)
            for row in validated["units"]:
                self.catalog["units"][row["UnitNumber"]] = dict(row)
            for row in validated["names"]:
                self.catalog["names"][row["UnitNumber"]] = dict(row)
            item = validated["item"]
            self.catalog["items"][item["CatalogueID"]] = dict(item)
            for entry in validated.get('blueprints',[]):
                rid=entry['recipe']['Idx']
                self.catalog.setdefault('blueprints',{})[rid]=dict(entry['recipe'])
                self.catalog.setdefault('blueprint_infos',{})[rid]=dict(entry['info'])
                self.catalog.setdefault('blueprint_materials',{})[rid]=[dict(row) for row in entry['materials']]
                self.catalog['items'][entry['item']['CatalogueID']]=dict(entry['item'])
            for (kind, identity), digest in installed_hashes.items():
                self._ownership[(kind, identity)] = (
                    validated["family_key"], digest
                )
            self._metadata["names"] = True
            return {
                "family_key": validated["family_key"],
                "revision": validated["revision"],
                **counts,
                "cache_restart_required": True,
                "services_restarted": False,
                "player_tables_changed": False,
                "shop_or_gacha_changed": False,
                "collection_family_registered": collection_registered,
                "applied": True,
                "server_target": target_key,
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
            raise ValueError("服务端目录事务失败，全部更改已回滚") from None
        finally:
            connection.close()
