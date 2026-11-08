"""Owner exclusion list for the collection draw.

Listed unit forms keep every drawable family that contains them out of the
collection machine and its catalog explanation. Unit definitions, inventories,
gift boxes and catalog unlocks are never changed here. The pool is always read
back from the installed ``sdgo_collection_capsule.EligibleRoots`` procedure,
which shares its pool fragment with the draw itself.
"""
from __future__ import annotations

import contextlib

MAX_ENTRIES = 5000
MAX_NOTE = 100
OWNER = "collection_capsule/v1"


def normalize(entries) -> list[dict]:
    """Validate an authored list; order and duplicates are not significant."""
    if not isinstance(entries, list) or len(entries) > MAX_ENTRIES:
        raise ValueError(f"除外名单须为列表，最多 {MAX_ENTRIES} 项")
    result = {}
    for entry in entries:
        if type(entry) is int:
            entry = {"unit_id": entry, "note": ""}
        if not isinstance(entry, dict) or set(entry) - {"unit_id", "note"}:
            raise ValueError("除外名单每项须为机体 ID 和可选备注")
        unit, note = entry.get("unit_id"), entry.get("note", "")
        if type(unit) is not int or not 0 < unit <= 2147483647:
            raise ValueError("除外名单含无效的机体 ID")
        if not isinstance(note, str) or len(note) > MAX_NOTE or any(ord(c) < 32 for c in note):
            raise ValueError(f"机体 {unit} 的备注须为不超过 {MAX_NOTE} 字的单行文字")
        if unit in result and result[unit] != note.strip():
            raise ValueError(f"机体 {unit} 在除外名单中重复且备注不同")
        result[unit] = note.strip()
    return [{"unit_id": unit, "note": note} for unit, note in sorted(result.items())]


def _require_owned(cursor):
    marker = cursor.execute("""SELECT CONVERT(nvarchar(128),value) FROM sys.extended_properties
WHERE class=3 AND major_id=SCHEMA_ID(N'sdgo_collection_capsule') AND name=N'sdgo_owner'""").fetchone()
    if not marker or marker[0] != OWNER:
        raise ValueError("目标数据库没有安装收藏扭蛋")
    row = cursor.execute("SELECT OBJECT_ID(N'sdgo_collection_capsule.ExcludedUnits',N'U'),"
                         "OBJECT_ID(N'sdgo_collection_capsule.EligibleRoots',N'P')").fetchone()
    if row[0] is None or row[1] is None:
        raise ValueError("请先在收藏扭蛋页执行「更新为图鉴规则」，再编辑除外名单")


def _pool(cursor) -> dict:
    cursor.execute("EXEC sdgo_collection_capsule.EligibleRoots")
    summary = cursor.fetchone()
    if not cursor.nextset():
        raise ValueError("收藏扭蛋候选读回不完整")
    rows = cursor.fetchall()
    bases = [{"unit_id": int(r[0]), "rank": int(r[1]), "excluded": bool(r[2]), "deliverable": bool(r[3])}
             for r in rows]
    return {"pool_error": bool(summary[0]), "bases": int(summary[1]), "eligible": int(summary[2]),
            "excluded_forms": int(summary[3]), "roots": bases}


def _names(cursor, ids) -> dict[int, str]:
    names = {}
    ids = sorted(set(ids))
    for offset in range(0, len(ids), 900):
        chunk = ids[offset:offset + 900]
        marks = ",".join("?" for _ in chunk)
        for unit, name in cursor.execute(
                "SELECT UnitNumber,UnitModelName FROM dbo.tbl_Units WHERE UnitNumber IN (" + marks + ")", *chunk):
            names[int(unit)] = str(name or "").strip()
    return names


def _listed(cursor) -> list[dict]:
    return [{"unit_id": int(r[0]), "note": str(r[1]), "updated_utc": r[2].isoformat(timespec="seconds") + "Z"}
            for r in cursor.execute("SELECT UnitID,Note,UpdatedUtc FROM sdgo_collection_capsule.ExcludedUnits ORDER BY UnitID")]


def describe(entries, pool, names) -> dict:
    """Summarize the effect without returning player data."""
    removed = [r["unit_id"] for r in pool["roots"] if r["excluded"]]
    return {
        "entries": [{**e, "name": names.get(e["unit_id"], ""), "exists": e["unit_id"] in names} for e in entries],
        "excluded_roots": [{"unit_id": u, "name": names.get(u, "")} for u in removed],
        "eligible": pool["eligible"], "bases": pool["bases"], "pool_error": pool["pool_error"],
        "undeliverable": [r["unit_id"] for r in pool["roots"] if not r["excluded"] and not r["deliverable"]],
    }


def excluded_roots(pool: dict, excluded) -> set[int]:
    """Offline equivalent for an exported snapshot: bases whose chain lists an excluded form."""
    excluded = set(excluded)
    return ({unit["unit_id"] for unit in pool["units"] if unit["unit_id"] in excluded}
            | {alias["base_unit_id"] for alias in pool.get("form_aliases", []) if alias["unit_id"] in excluded})


def eligible_roots(db) -> list[int]:
    """Drawable family roots after exclusions, as the draw itself computes them."""
    cursor = db.cursor()
    try:
        _require_owned(cursor)
        return [r["unit_id"] for r in _pool(cursor)["roots"] if not r["excluded"]]
    finally:
        cursor.close()


def read(db) -> dict:
    cursor = db.cursor()
    try:
        _require_owned(cursor)
        entries = _listed(cursor)
        pool = _pool(cursor)
        names = _names(cursor, [e["unit_id"] for e in entries] + [r["unit_id"] for r in pool["roots"] if r["excluded"]])
        return {**describe(entries, pool, names), "eligible_roots": [r["unit_id"] for r in pool["roots"] if not r["excluded"]]}
    finally:
        cursor.close()


def replace(db, entries, *, dry_run: bool = False) -> dict:
    """Atomically replace the list; a dry run reports the same readback and rolls back."""
    entries = normalize(entries)
    cursor = db.cursor()
    began = False
    try:
        if cursor.execute("SELECT @@TRANCOUNT").fetchone()[0]:
            raise ValueError("除外名单写入需要独立事务")
        cursor.execute("SET XACT_ABORT ON;BEGIN TRANSACTION;")
        began = True
        lock = cursor.execute("""DECLARE @r int;EXEC @r=sys.sp_getapplock @Resource=N'sdgo.collection_capsule.install.v1',
@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000;SELECT @r;""").fetchone()[0]
        if lock < 0:
            raise ValueError("收藏扭蛋正在安装或更新，请稍后再试")
        _require_owned(cursor)
        before = _listed(cursor)
        names = _names(cursor, [e["unit_id"] for e in entries])
        missing = [e["unit_id"] for e in entries if e["unit_id"] not in names]
        if missing:
            raise ValueError("这些机体 ID 不在目标数据库中：" + "、".join(map(str, missing[:20])))
        kept = {e["unit_id"]: e["note"] for e in before}
        wanted = {e["unit_id"]: e["note"] for e in entries}
        for unit in sorted(set(kept) - set(wanted)):
            cursor.execute("DELETE sdgo_collection_capsule.ExcludedUnits WHERE UnitID=?", unit)
        for unit, note in sorted(wanted.items()):
            if unit not in kept:
                cursor.execute("INSERT sdgo_collection_capsule.ExcludedUnits(UnitID,Note) VALUES(?,?)", unit, note)
            elif kept[unit] != note:
                cursor.execute("UPDATE sdgo_collection_capsule.ExcludedUnits SET Note=?,UpdatedUtc=GETUTCDATE() "
                               "WHERE UnitID=?", note, unit)
        after = _listed(cursor)
        stored = [(e["unit_id"], e["note"]) for e in after]
        if stored != [(e["unit_id"], e["note"]) for e in entries]:
            raise RuntimeError("除外名单写入读回不一致")
        pool = _pool(cursor)
        names.update(_names(cursor, [r["unit_id"] for r in pool["roots"] if r["excluded"]]))
        if pool["pool_error"]:
            raise ValueError("收藏扭蛋候选有未登记初始形态的互变机体；先登记或把它加入除外名单")
        if pool["eligible"] == 0:
            raise ValueError("除外后没有可抽取的机体；收藏扭蛋至少要保留一个候选")
        cursor.execute("COMMIT TRANSACTION;" if not dry_run else "ROLLBACK TRANSACTION;")
        began = False
        return {**describe(after, pool, names),
                "eligible_roots": [r["unit_id"] for r in pool["roots"] if not r["excluded"]],
                "changed": [(e["unit_id"], e["note"]) for e in before] != [(e["unit_id"], e["note"]) for e in entries],
                "applied": not dry_run, "world_restart_required": False}
    except Exception:
        if began:
            with contextlib.suppress(Exception):
                cursor.execute("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;")
        raise
    finally:
        cursor.close()

