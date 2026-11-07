"""Server-side operator adapter. Store listings and prices are never changed."""
import hashlib
import json
from pathlib import Path
import re
from ... import storage
from .model import registrations, revision, validate
from . import capabilities, runtime


def validate_pools(cur, catalog):
    """Recheck typed references against the live host, without name matching."""
    clauses={
        'blueprints':("C.CatalogueID/100000=15 AND EXISTS(SELECT 1 FROM dbo.tbl_MixTreeFile M "
                      "JOIN dbo.tbl_Units U ON U.UnitNumber=M.OutUnitID WHERE M.Idx=C.RefIndex)"),
        'stickers':"C.CatalogueID/100000=17 AND EXISTS(SELECT 1 FROM dbo.tbl_StickerItem S WHERE S.StickerID=C.RefIndex)",
        'paints':"C.CatalogueID/100000=16 AND EXISTS(SELECT 1 FROM dbo.tbl_PaintItem P WHERE P.PaintID=C.RefIndex AND P.Type IN (0,1))",
        'polish':"C.CatalogueID/100000=16 AND EXISTS(SELECT 1 FROM dbo.tbl_PaintItem P WHERE P.PaintID=C.RefIndex AND P.Type=2)",
    }
    used={r['pool'] for o in catalog['operators'] if o['managed'] and o['enabled'] for r in o['drops'] if 'pool' in r}
    for key in used:
        pool=catalog['drop_pools'][key]
        clause=clauses.get(key,'1=1')
        valid={r[0] for r in cur.execute('SELECT C.CatalogueID FROM dbo.tbl_ItemCategory C WHERE '+clause)}
        if not set(pool['category_ids'])<=valid:
            raise ValueError('随机物品池与当前服务端类型/配方不一致：'+pool['label'])


def migrate_default_aliases(cur, catalog):
    if catalog.get('default_pack_version') != 1:return 0
    from .presets import LEGACY_ALIASES
    changed=0
    enabled={o['id'] for o in catalog['operators'] if o['managed'] and o['enabled']}
    for identity,alias in LEGACY_ALIASES.items():
        if identity not in enabled:continue
        # Only exact aliases written by the historical script are migrated.
        # The following fixed-name migration replaces these historical aliases
        # with an ASCII token and a completed-name flag in the same transaction.
        cur.execute('''INSERT dbo.sdgo_OperatorLegacyNameBackup
            SELECT U.UserOperatorNumber,U.AccountNumber,U.OperatorNickname,U.IsSetName
            FROM dbo.tbl_UserOperators U WHERE U.OperatorNumber=?
            AND U.OperatorNickname COLLATE Latin1_General_100_BIN2=?
            AND NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorLegacyNameBackup B WHERE B.UserOperatorNumber=U.UserOperatorNumber)''',identity,alias)
        row=cur.execute('''SELECT COUNT(*) FROM dbo.tbl_UserOperators
            WHERE OperatorNumber=? AND OperatorNickname COLLATE Latin1_General_100_BIN2=?''',identity,alias).fetchone()
        changed+=row[0]
    return changed


def migrate_fixed_default_names(cur,catalog):
    from .presets import fixed_name_ids
    changed=0
    for identity in sorted(fixed_name_ids(catalog)):
        token='@SDGO:'+str(identity)
        cur.execute('''INSERT dbo.sdgo_OperatorFixedNameBackup
            SELECT U.UserOperatorNumber,U.AccountNumber,U.OperatorNumber,U.OperatorNickname,U.IsSetName
            FROM dbo.tbl_UserOperators U WHERE U.OperatorNumber=? AND ISNULL(U.Forbidden,0)=0
            AND (U.IsSetName<>1 OR U.OperatorNickname COLLATE Latin1_General_100_BIN2<>?)
            AND NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorFixedNameBackup B WHERE B.UserOperatorNumber=U.UserOperatorNumber)''',identity,token)
        cur.execute('''UPDATE dbo.tbl_UserOperators SET OperatorNickname=?,IsSetName=1
            WHERE OperatorNumber=? AND ISNULL(Forbidden,0)=0
            AND (IsSetName<>1 OR OperatorNickname COLLATE Latin1_General_100_BIN2<>?)''',token,identity,token)
        changed+=max(0,cur.rowcount)
    return changed


def install_fixed_name_guard(cur):
    name='dbo.sdgo_OperatorFixedNames'
    row=cur.execute('SELECT definition FROM sys.sql_modules WHERE object_id=OBJECT_ID(?)',name).fetchone()
    text=(MODULE_DIR/'server/naming.sql').read_text(encoding='utf-8')
    if row:
        if '-- SDGO_OPERATOR_FIXED_NAMES_V1' not in row[0]:
            raise ValueError('固定副官命名触发器已被其他内容占用，未覆盖')
        text=re.sub(r'^CREATE TRIGGER','ALTER TRIGGER',text,count=1)
    cur.execute(text)


def apply_default_mission_parts(cur,catalog,prior_values=None):
    from .presets import fixed_name_ids
    explicit={
        op["id"] for op in catalog["operators"]
        if op["managed"] and op["enabled"]
        and op.get("abilities", {}).get("mode") == "custom"
        and op["abilities"].get("mission_parts") is not None
    }
    enabled=fixed_name_ids(catalog)-explicit
    backups={r[0]:r[1] for r in cur.execute('SELECT OperatorID,OriginalGParts FROM dbo.sdgo_OperatorMissionPartsBackup')}
    module_created={
        int(r[0]) for r in cur.execute(
            "SELECT OperatorID FROM dbo.sdgo_OperatorDataBackup WHERE WasPresent=0"
        )
    }
    for identity,original in backups.items():
        if identity not in enabled and identity not in explicit and identity not in module_created:
            cur.execute('UPDATE dbo.tbl_OperatorData SET GParts=? WHERE OperatorID=?',original,identity)
    changed=0
    for identity in sorted(enabled):
        row=cur.execute('SELECT GParts FROM dbo.tbl_OperatorData WHERE OperatorID=?',identity).fetchone()
        if row is None:raise ValueError('默认副官任务部件登记不存在：'+str(identity))
        if identity not in backups:
            cur.execute('INSERT dbo.sdgo_OperatorMissionPartsBackup VALUES(?,?)',identity,row[0])
        if row[0]!=1:
            cur.execute('UPDATE dbo.tbl_OperatorData SET GParts=1 WHERE OperatorID=?',identity)
        before=(prior_values or {}).get(identity,row[0])
        if before!=1:changed+=1
    return changed


def migrate_default_entitlements(cur, catalog):
    """Replace old default-officer entitlements in place; equipped IDs stay valid."""
    if catalog.get('default_pack_version') != 1:return 0
    from .presets import NAMES
    changed=0
    enabled={o['id'] for o in catalog['operators'] if o['managed'] and o['enabled'] and o['delivery']=='all_players'}
    for identity in NAMES:
        if identity not in enabled:continue
        cur.execute('''INSERT dbo.sdgo_OperatorLegacyEntitlementBackup
            SELECT U.UserOperatorNumber,U.AccountNumber,U.OperatorNumber,U.OPType,U.RunOut,U.DateEnd
            FROM dbo.tbl_UserOperators U WHERE U.OperatorNumber=? AND ISNULL(U.Forbidden,0)=0
            AND (U.OPType<>0 OR U.RunOut<>0 OR U.DateEnd IS NOT NULL)
            AND NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorLegacyEntitlementBackup B WHERE B.UserOperatorNumber=U.UserOperatorNumber)''',identity)
        cur.execute('''UPDATE dbo.tbl_UserOperators SET OPType=0,RunOut=0,DateEnd=NULL
            WHERE OperatorNumber=? AND ISNULL(Forbidden,0)=0
            AND (OPType<>0 OR RunOut<>0 OR DateEnd IS NOT NULL)''',identity)
        changed+=max(0,cur.rowcount)
    return changed

_ROOT = Path(__file__).resolve().parents[3]
MODULE_DIR = Path(__file__).with_name('resources')
WRAPPED = ("sp_OperatorDrop", "sp_GetUserOperatorPage", "sp_GetPilotOperator")
MARKER = "-- SDGO_OPERATOR_CATALOG_V1"
OPERATOR_COLUMNS = [
    "OperatorID", "OperatorName", "TeamWin", "TeamLose", "BattleResult",
    "ChangeSp", "ServiceType", "AddOn1", "AddOn2", "AddOn3", "AddOn4",
    "DropType", "GParts",
]
BACKUP_COLUMNS = [
    "OriginalOperatorName", "OriginalTeamWin", "OriginalTeamLose",
    "OriginalBattleResult", "OriginalChangeSp", "OriginalServiceType",
    "OriginalAddOn1", "OriginalAddOn2", "OriginalAddOn3", "OriginalAddOn4",
    "OriginalGParts",
]


def read_ini(server_ini):
    return storage.ini_values(Path(server_ini).read_bytes())


def connect(server_ini):
    import pyodbc
    parser=read_ini(server_ini)
    db = parser["DB Game"]
    drivers = [d for d in pyodbc.drivers() if "SQL Server" in d]
    if not drivers:
        raise ValueError("本机缺少 SQL Server ODBC 驱动")
    def escape(value):
        return "{" + str(value).replace("}", "}}") + "}"
    connection = (f"DRIVER={escape(drivers[-1])};SERVER={escape(db['DBServerName'])};"
                  f"DATABASE={escape(db['DBName'])};UID={escape(db['ID'])};PWD={escape(db['PWD'])};")
    try:
        result = pyodbc.connect(connection, timeout=5, autocommit=True)
        result.timeout = 30
        return result
    except pyodbc.Error as error:
        raise ValueError(f"服务端连接失败，SQLSTATE={error.args[0]}") from None


def source_hash(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def catalog_status(catalog, server_ini):
    """Read the saved catalog's exact profile revision/state from the server."""
    validate(catalog)
    expected_revision = revision(catalog)
    expected = {
        int(operator["id"]): bool(operator["enabled"])
        for operator in catalog["operators"] if operator["managed"]
    }
    cn = connect(server_ini)
    try:
        cur = cn.cursor()
        rows = list(cur.execute(
            """SELECT OperatorID,Enabled,Revision
               FROM dbo.sdgo_OperatorProfiles ORDER BY OperatorID"""
        ))
        current = {
            int(identity): {
                "enabled": bool(enabled),
                "revision": str(applied_revision),
            }
            for identity, enabled, applied_revision in rows
        }
        reasons = []
        for identity, enabled in expected.items():
            row = current.get(identity)
            if row is None:
                reasons.append(f"operator profile {identity} is absent")
            elif row["revision"] != expected_revision:
                reasons.append(
                    f"operator profile {identity} has another catalog revision"
                )
            elif row["enabled"] != enabled:
                reasons.append(
                    f"operator profile {identity} enabled state differs"
                )
        unexpected = sorted(
            identity for identity, row in current.items()
            if identity not in expected and row["enabled"]
        )
        if unexpected:
            reasons.append(
                "other operator profiles remain enabled: "
                + ",".join(map(str, unexpected))
            )
        managed_revisions = {
            current[identity]["revision"]
            for identity in expected if identity in current
        }
        applied_revision = (
            next(iter(managed_revisions))
            if len(managed_revisions) == 1 else None
        )
        return {
            "database_applied": not reasons,
            "expected_revision": expected_revision,
            "applied_revision": applied_revision,
            "managed_profiles": len(expected),
            "reasons": reasons,
        }
    finally:
        try:
            cn.rollback()
        except Exception:
            pass
        cn.close()


def alter(text):
    result, count = re.subn(r"(?im)^\s*CREATE\s+(PROC(?:EDURE)?)\b", r"ALTER \1", text, count=1)
    if count != 1:
        raise ValueError("原生过程定义的 CREATE 入口无法确认")
    return result


def wrap(name, original):
    if MARKER in original:
        raise ValueError("不能重复包装副官过程")
    if name == "sp_OperatorDrop":
        anchor = r"(?im)^\s*DECLARE\s+@v_Probability\s+INT\s*;?\s*$"
        injected = f"""
{MARKER}
IF EXISTS(SELECT 1 FROM dbo.sdgo_OperatorProfiles WHERE OperatorID=@OperatorNumber AND Enabled=1)
BEGIN
    EXEC dbo.sdgo_OperatorManagedDrop @v_AccountID,@v_UserOperatorNumber,
        @v_DropCategoryID OUTPUT,@v_Amount OUTPUT,@v_RetVal OUTPUT;
    RETURN;
END;
"""
        matches = list(re.finditer(anchor, original))
        if len(matches) != 1 or "@OperatorNumber" not in original[:matches[0].start()]:
            raise ValueError("原生副官掉落过程版本不匹配，未做改动")
        # Injection is after the native ownership/DropType checks and native
        # prelude, preserving existing bonus logic exactly once.
        result = original[:matches[0].start()] + injected + original[matches[0].start():]
    elif name in ("sp_GetUserOperatorPage", "sp_GetPilotOperator"):
        anchor = r"(?im)^[ \t]*SET[ \t]+NOCOUNT[ \t]+ON[ \t]*;?"
        matches = list(re.finditer(anchor, original))
        if len(matches) != 1 or not re.search(r"@v_AccountNum\s+INT", original, re.I):
            raise ValueError("原生副官读取过程版本不匹配，未做改动")
        point = matches[0].end()
        result = original[:point] + f"\n{MARKER}\nEXEC dbo.sdgo_EnsureFreeOperators @v_AccountNum;\n" + original[point:]
    else:
        raise ValueError("不允许包装此过程")
    return alter(result)


def definition(cur, name):
    row = cur.execute("SELECT definition FROM sys.sql_modules WHERE object_id=OBJECT_ID(?)", "dbo." + name).fetchone()
    if row is None or row[0] is None:
        raise ValueError(f"服务端缺少原生过程 {name}")
    return row[0]


def install_procedures(cur):
    for file in ("random.sql", "drop.sql", "free.sql"):
        text = (MODULE_DIR / "server" / file).read_text(encoding="utf-8")
        match = re.search(r"CREATE\s+PROCEDURE\s+dbo\.([A-Za-z0-9_]+)", text, re.I)
        name = match[1]
        if cur.execute("SELECT OBJECT_ID(?)", "dbo." + name).fetchone()[0] is not None:
            text = alter(text)
        cur.execute(text)
    for name in WRAPPED:
        current = definition(cur, name)
        backup = cur.execute("SELECT OriginalDefinition,InstalledHash FROM dbo.sdgo_OperatorProcedureBackup WHERE ProcedureName=?", name).fetchone()
        if backup:
            original, installed_hash = backup
            if source_hash(current) != installed_hash:
                raise ValueError(f"{name} 已被其他修改改变，未覆盖")
        else:
            original = current
        cur.execute(wrap(name, original))
        installed = definition(cur, name)
        if backup:
            cur.execute("UPDATE dbo.sdgo_OperatorProcedureBackup SET InstalledHash=? WHERE ProcedureName=?", source_hash(installed), name)
        else:
            cur.execute("INSERT dbo.sdgo_OperatorProcedureBackup VALUES(?,?,?)", name, original, source_hash(installed))


def _operator_row(cur, identity):
    row = cur.execute(
        "SELECT " + ",".join(OPERATOR_COLUMNS) +
        " FROM dbo.tbl_OperatorData WHERE OperatorID=?", identity
    ).fetchone()
    return dict(zip(OPERATOR_COLUMNS, row)) if row is not None else None


def _template_operator_row(template, identity, operator_name=None, drop_type=0):
    desired = dict(template)
    desired["OperatorID"] = identity
    desired["OperatorName"] = operator_name or f"SDGOop{identity}"
    desired["DropType"] = drop_type
    return desired


def _retained_operator_plan(identity, has_owners, template, current):
    """Plan a module-created row without touching any player-owned record."""
    if not has_owners:
        return None
    if template is None:
        raise ValueError(f"已持有的新增副官 {identity} 缺少原生恢复模板")
    return _template_operator_row(
        template, identity,
        current["OperatorName"] if current is not None else None,
        0,
    )


def _write_operator_row(cur, desired, current):
    if current is None:
        cur.execute(
            "INSERT dbo.tbl_OperatorData(" + ",".join(OPERATOR_COLUMNS) +
            ") VALUES(" + ",".join("?" for _ in OPERATOR_COLUMNS) + ")",
            *(desired[name] for name in OPERATOR_COLUMNS),
        )
        return
    mutable = OPERATOR_COLUMNS[1:]
    cur.execute(
        "UPDATE dbo.tbl_OperatorData SET " +
        ",".join(name + "=?" for name in mutable) +
        " WHERE OperatorID=?",
        *(desired[name] for name in mutable), desired["OperatorID"],
    )


def _complete_old_operator_backups(cur):
    """Finish the one-time migration from the historical DropType-only backup."""
    rows = list(cur.execute(
        "SELECT OperatorID,WasPresent,BackupComplete FROM dbo.sdgo_OperatorDataBackup"
    ))
    for identity, was_present, complete in rows:
        if complete:
            continue
        if not was_present:
            cur.execute(
                "UPDATE dbo.sdgo_OperatorDataBackup SET BackupComplete=1 WHERE OperatorID=?",
                identity,
            )
            continue
        original = _operator_row(cur, identity)
        if original is None:
            raise ValueError(f"副官 {identity} 的原始服务端记录已丢失，不能完成备份迁移")
        mission = cur.execute(
            "SELECT OriginalGParts FROM dbo.sdgo_OperatorMissionPartsBackup WHERE OperatorID=?",
            identity,
        ).fetchone()
        if mission is not None:
            original["GParts"] = mission[0]
        values = [
            original["OperatorName"], original["TeamWin"], original["TeamLose"],
            original["BattleResult"], original["ChangeSp"], original["ServiceType"],
            original["AddOn1"], original["AddOn2"], original["AddOn3"],
            original["AddOn4"], original["GParts"], identity,
        ]
        cur.execute(
            """UPDATE dbo.sdgo_OperatorDataBackup SET BackupComplete=1,
               OriginalOperatorName=?,OriginalTeamWin=?,OriginalTeamLose=?,
               OriginalBattleResult=?,OriginalChangeSp=?,OriginalServiceType=?,
               OriginalAddOn1=?,OriginalAddOn2=?,OriginalAddOn3=?,OriginalAddOn4=?,
               OriginalGParts=? WHERE OperatorID=?""",
            *values,
        )


def _restore_operator_backups(cur):
    _complete_old_operator_backups(cur)
    cur.execute(
        """UPDATE O SET
             OperatorName=B.OriginalOperatorName,TeamWin=B.OriginalTeamWin,
             TeamLose=B.OriginalTeamLose,BattleResult=B.OriginalBattleResult,
             ChangeSp=B.OriginalChangeSp,ServiceType=B.OriginalServiceType,
             AddOn1=B.OriginalAddOn1,AddOn2=B.OriginalAddOn2,
             AddOn3=B.OriginalAddOn3,AddOn4=B.OriginalAddOn4,
             DropType=B.OriginalDropType,GParts=B.OriginalGParts
           FROM dbo.tbl_OperatorData O JOIN dbo.sdgo_OperatorDataBackup B
             ON O.OperatorID=B.OperatorID
           WHERE B.WasPresent=1 AND B.BackupComplete=1"""
    )
    cur.execute(
        """SELECT B.OperatorID,B.BaselineTemplateID,
                  CASE WHEN EXISTS(
                    SELECT 1 FROM dbo.tbl_UserOperators U
                    WHERE U.OperatorNumber=B.OperatorID
                  ) THEN 1 ELSE 0 END
           FROM dbo.sdgo_OperatorDataBackup B
           WHERE B.WasPresent=0 AND B.BackupComplete=1"""
    )
    retained = set()
    for identity, template_id, has_owners in list(cur.fetchall()):
        current = _operator_row(cur, identity)
        if not has_owners:
            if current is not None:
                cur.execute(
                    "DELETE FROM dbo.tbl_OperatorData WHERE OperatorID=?",
                    identity,
                )
            continue
        if template_id is None:
            raise ValueError(f"已持有的新增副官 {identity} 缺少持久化原生模板")
        if int(template_id) == int(identity):
            raise ValueError(f"已持有的新增副官 {identity} 不能以自身作为恢复模板")
        template = _operator_row(cur, template_id)
        desired = _retained_operator_plan(identity, True, template, current)
        _write_operator_row(cur, desired, current)
        retained.add(int(identity))
    return retained


def _ensure_operator_backup(cur, identity, template_id):
    backup = cur.execute(
        """SELECT WasPresent,OriginalDropType,BackupComplete,BaselineTemplateID
           FROM dbo.sdgo_OperatorDataBackup WHERE OperatorID=?""",
        identity,
    ).fetchone()
    if backup:
        if not backup[2]:
            raise ValueError("副官原始记录备份未完成")
        if not backup[0] and backup[3] != template_id:
            cur.execute(
                """UPDATE dbo.sdgo_OperatorDataBackup
                   SET BaselineTemplateID=? WHERE OperatorID=?""",
                template_id, identity,
            )
        return bool(backup[0]), backup[1]
    original = _operator_row(cur, identity)
    if original is None:
        cur.execute(
            """INSERT dbo.sdgo_OperatorDataBackup(
                 OperatorID,WasPresent,OriginalDropType,BaselineTemplateID,BackupComplete,
                 OriginalOperatorName,OriginalTeamWin,OriginalTeamLose,
                 OriginalBattleResult,OriginalChangeSp,OriginalServiceType,
                 OriginalAddOn1,OriginalAddOn2,OriginalAddOn3,OriginalAddOn4,
                 OriginalGParts)
               VALUES(?,0,NULL,?,1,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL)""",
            identity, template_id,
        )
        return False, None
    cur.execute(
        """INSERT dbo.sdgo_OperatorDataBackup(
             OperatorID,WasPresent,OriginalDropType,BaselineTemplateID,BackupComplete,
             OriginalOperatorName,OriginalTeamWin,OriginalTeamLose,
             OriginalBattleResult,OriginalChangeSp,OriginalServiceType,
             OriginalAddOn1,OriginalAddOn2,OriginalAddOn3,OriginalAddOn4,
             OriginalGParts)
           VALUES(?,1,?,?,1,?,?,?,?,?,?,?,?,?,?,?)""",
        identity, original["DropType"], template_id, original["OperatorName"],
        original["TeamWin"], original["TeamLose"], original["BattleResult"],
        original["ChangeSp"], original["ServiceType"], original["AddOn1"],
        original["AddOn2"], original["AddOn3"], original["AddOn4"],
        original["GParts"],
    )
    return True, original["DropType"]


def _register_native_capabilities(cur, trusted_runtime):
    flags = runtime.trusted_capabilities(trusted_runtime)
    if flags != runtime.TRUSTED_NATIVE_CAPABILITIES:
        raise ValueError("verified native runtime capability set is incomplete")
    if (
        not isinstance(trusted_runtime.evidence, str)
        or len(trusted_runtime.evidence) > 256
    ):
        raise ValueError("verified native runtime evidence is invalid")
    for name in sorted(flags):
        cur.execute(
            """UPDATE dbo.sdgo_OperatorNativeCapabilities
               SET Verified=1,Evidence=? WHERE CapabilityName=?""",
            trusted_runtime.evidence, name,
        )


def _ability_plan(cur, catalog, *, trusted_runtime=None):
    skill_columns = ["SkillNo", "SkillType", "PointAdd", "UnitExpAdd", "EnergySave"]
    time_columns = ["TimeID", "AddOnID", "OperatorSkill", "StartTime", "CloseTime"]
    skills = [
        dict(zip(skill_columns, row))
        for row in cur.execute("SELECT " + ",".join(skill_columns) + " FROM dbo.tbl_OperatorSkill")
    ]
    times = [
        dict(zip(time_columns, row))
        for row in cur.execute("SELECT " + ",".join(time_columns) + " FROM dbo.tbl_OperatorTime")
    ]
    owned_skills = [
        dict(zip(skill_columns, row))
        for row in cur.execute("SELECT " + ",".join(skill_columns) + " FROM dbo.sdgo_OperatorSkillRegistry")
    ]
    owned_times = [
        dict(zip(time_columns, row))
        for row in cur.execute("SELECT " + ",".join(time_columns) + " FROM dbo.sdgo_OperatorTimeRegistry")
    ]
    native_flags = {
        name: name in runtime.trusted_capabilities(trusted_runtime)
        for name in runtime.TRUSTED_NATIVE_CAPABILITIES
    } if trusted_runtime is not None else {
        name: False for name in runtime.TRUSTED_NATIVE_CAPABILITIES
    }
    plan = capabilities.compile_plan(
        catalog["operators"], skills, times,
        owned_skills=owned_skills, owned_times=owned_times,
        allow_new_skill_ids=native_flags.get("custom_skill_ids", False),
        allow_new_time_ids=native_flags.get("custom_time_ids", False),
    )
    required_native = set(plan["required_native_capabilities"])
    if plan["owned_skills"]:
        required_native.add("custom_skill_ids")
    if plan["owned_times"]:
        required_native.add("custom_time_ids")
    missing_native = [
        name for name in sorted(required_native)
        if not native_flags.get(name, False)
    ]
    if missing_native:
        raise ValueError("副官能力缺少已验证的原生运行能力：" + ",".join(missing_native))

    extension_rows = {
        str(row[0]): (int(row[1]), bool(row[2]))
        for row in cur.execute(
            "SELECT ExtensionName,SchemaVersion,Enabled FROM dbo.sdgo_OperatorAbilityExtensions"
        )
    }
    metadata = catalog.get("registered_ability_extensions", {})
    for name in plan["required_extensions"]:
        installed = extension_rows.get(name)
        if installed is None or not installed[1]:
            raise ValueError("副官能力扩展执行器未安装：" + name)
        expected = metadata.get(name) if isinstance(metadata, dict) else None
        if type(expected) is int and expected != installed[0]:
            raise ValueError("副官能力扩展版本不匹配：" + name)
    return plan


def _validate_owned_ability_references(cur, managed_ids):
    owned_skills = {int(row[0]) for row in cur.execute("SELECT SkillNo FROM dbo.sdgo_OperatorSkillRegistry")}
    owned_add_ons = {int(row[0]) for row in cur.execute("SELECT AddOnID FROM dbo.sdgo_OperatorTimeRegistry")}
    owned_time_ids = {int(row[0]) for row in cur.execute("SELECT TimeID FROM dbo.sdgo_OperatorTimeRegistry")}
    if not owned_skills and not owned_add_ons:
        return
    for row in cur.execute(
        "SELECT OperatorID,TeamWin,TeamLose,BattleResult,AddOn1,AddOn2,AddOn3,AddOn4 FROM dbo.tbl_OperatorData"
    ):
        if int(row[0]) in managed_ids:
            continue
        if any(int(value or 0) in owned_skills for value in row[1:4]):
            raise ValueError(f"自定义副官技能仍被未托管副官 {row[0]} 引用")
        if any(int(value or 0) in owned_add_ons for value in row[4:8]):
            raise ValueError(f"自定义副官时间仍被未托管副官 {row[0]} 引用")
    for row in cur.execute("SELECT TimeID,OperatorSkill FROM dbo.tbl_OperatorTime"):
        if int(row[1]) in owned_skills and int(row[0]) not in owned_time_ids:
            raise ValueError(f"自定义副官技能仍被未托管时间 {row[0]} 引用")


def _install_ability_rows(cur, plan, managed_ids):
    _validate_owned_ability_references(cur, managed_ids)
    cur.execute(
        """DELETE T FROM dbo.tbl_OperatorTime T
           JOIN dbo.sdgo_OperatorTimeRegistry R ON R.TimeID=T.TimeID"""
    )
    cur.execute("DELETE FROM dbo.sdgo_OperatorTimeRegistry")
    cur.execute(
        """DELETE S FROM dbo.tbl_OperatorSkill S
           JOIN dbo.sdgo_OperatorSkillRegistry R ON R.SkillNo=S.SkillNo"""
    )
    cur.execute("DELETE FROM dbo.sdgo_OperatorSkillRegistry")
    for row in plan["owned_skills"]:
        values = tuple(row[name] for name in ("SkillNo", "SkillType", "PointAdd", "UnitExpAdd", "EnergySave"))
        cur.execute("INSERT dbo.tbl_OperatorSkill VALUES(?,?,?,?,?)", *values)
        cur.execute("INSERT dbo.sdgo_OperatorSkillRegistry VALUES(?,?,?,?,?)", *values)
    for row in plan["owned_times"]:
        values = tuple(row[name] for name in ("TimeID", "AddOnID", "OperatorSkill", "StartTime", "CloseTime"))
        cur.execute("INSERT dbo.tbl_OperatorTime VALUES(?,?,?,?,?)", *values)
        cur.execute("INSERT dbo.sdgo_OperatorTimeRegistry VALUES(?,?,?,?,?)", *values)


def _operator_data_plan(cur, managed, backups, ability_plan):
    """Compile complete enabled rows from restored native templates."""
    result = {}
    for op in managed:
        if not op["enabled"]:
            continue
        template = _operator_row(cur, op["template_id"])
        if template is None:
            raise ValueError("服务端副官技能模板不存在")
        current = _operator_row(cur, op["id"])
        desired = _template_operator_row(
            template, op["id"],
            (
                current["OperatorName"] if current is not None and backups[op["id"]][0]
                else None
            ),
            1,
        )
        compiled = ability_plan["operators"].get(op["id"])
        if compiled is not None:
            for field in (
                "TeamWin", "TeamLose", "BattleResult",
                "AddOn1", "AddOn2", "AddOn3", "AddOn4",
            ):
                desired[field] = compiled[field]
            if compiled["mission_parts"] is not None:
                desired["GParts"] = int(compiled["mission_parts"])
        result[op["id"]] = {"current": current, "desired": desired}
    return result


def apply_catalog(catalog, server_ini, *, runtime_evidence=None):
    validate(catalog)
    from manager.operations import server_change
    with server_change(server_ini,'operator_catalog.publish',('fixed_catalog','player_migration'))as change:
        return change.result(_apply_catalog(catalog,server_ini))


def _apply_catalog(catalog, server_ini):
    validate(catalog)
    cn = connect(server_ini)
    try:
        cur = cn.cursor()
        cur.execute("SET XACT_ABORT ON; BEGIN TRANSACTION")
        # Serialize only this module's changes; no unrelated configuration is reset.
        cur.execute("DECLARE @R INT; EXEC @R=sys.sp_getapplock @Resource=N'SDGO:catalog-change',@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000; SELECT @R")
        if cur.fetchone()[0] < 0:
            raise ValueError("另一项副官更新正在进行，请稍后重试")
        cur.execute((MODULE_DIR / "server" / "schema.sql").read_text(encoding="utf-8"))
        trusted_runtime = None
        if runtime_evidence is not None:
            fingerprints = {
                name: source_hash(definition(cur, name))
                for name in ("sp_GetOperatorSkill", "sp_GetOperatorAddOn")
            }
            trusted_runtime = runtime.verify_installed_runtime(
                runtime_evidence, fingerprints
            )
            if trusted_runtime.revision != revision(catalog):
                raise ValueError("installed operator runtime catalog revision differs")
            _register_native_capabilities(cur, trusted_runtime)
        validate_pools(cur,catalog)
        managed = [o for o in catalog["operators"] if o["managed"]]
        prior_gparts={int(row[0]):int(row[1]) for row in cur.execute(
            "SELECT OperatorID,GParts FROM dbo.tbl_OperatorData"
        )}
        _complete_old_operator_backups(cur)
        backups = {
            op["id"]: _ensure_operator_backup(cur, op["id"], op["template_id"])
            for op in managed
        }
        _restore_operator_backups(cur)
        ability_plan = _ability_plan(
            cur, catalog, trusted_runtime=trusted_runtime
        )
        operator_data_plan = _operator_data_plan(cur, managed, backups, ability_plan)
        _install_ability_rows(
            cur, ability_plan,
            {op["id"] for op in managed if op["enabled"]},
        )
        for op in managed:
            operator_id, category_id = op["id"], op["category_id"]
            exists = cur.execute("SELECT RefIndex FROM dbo.tbl_ItemCategory WHERE CatalogueID=?", category_id).fetchone()
            owned = cur.execute("SELECT OperatorID,OwnedCategory FROM dbo.sdgo_OperatorItemRegistry WHERE CategoryID=?", category_id).fetchone()
            native_item = next((x for x in catalog["items"] if x["id"] == category_id), None)
            if exists and (exists[0] != operator_id or (not owned and native_item is None)):
                raise ValueError(f"物品登记编号 {category_id} 被其他内容占用")
            if owned and owned[0] != operator_id:
                raise ValueError("物品登记归属不一致")
            if not exists:
                cur.execute("INSERT dbo.tbl_ItemCategory(CatalogueID,Name,RefIndex,TimeLimit,Option1,Option2) VALUES(?,?,?,0,0,0)",
                            category_id, f"SDGOop{operator_id}", operator_id)
            if not owned:
                cur.execute("INSERT dbo.sdgo_OperatorItemRegistry VALUES(?,?,?)", category_id, operator_id, 0 if exists else 1)
            if not op["enabled"]:
                continue
            current = operator_data_plan[operator_id]["current"]
            desired = operator_data_plan[operator_id]["desired"]
            _write_operator_row(cur, desired, current)
        cur.execute("UPDATE dbo.sdgo_OperatorProfiles SET Enabled=0")
        rev = revision(catalog)
        from .presets import fixed_name_ids
        fixed_names=fixed_name_ids(catalog)
        for op in managed:
            row = cur.execute("SELECT 1 FROM dbo.sdgo_OperatorProfiles WHERE OperatorID=?", op["id"]).fetchone()
            compiled = ability_plan["operators"].get(op["id"], {})
            pilot_exp = compiled.get("pilot_exp_percent", 0)
            extensions = json.dumps(
                compiled.get("extensions", {}), ensure_ascii=False,
                sort_keys=True, separators=(",", ":"), allow_nan=False,
            )
            values = (
                op["category_id"], int(op["enabled"]), op["delivery"], op["name"],
                rev, int(op['id'] in fixed_names), pilot_exp, extensions, op["id"],
            )
            if row:
                cur.execute("""UPDATE dbo.sdgo_OperatorProfiles SET
                    CategoryID=?,Enabled=?,DeliveryMode=?,DisplayName=?,Revision=?,
                    FixedName=?,PilotExpPercent=?,AbilityExtensions=?
                    WHERE OperatorID=?""", *values)
            else:
                cur.execute("""INSERT dbo.sdgo_OperatorProfiles(
                    CategoryID,Enabled,DeliveryMode,DisplayName,Revision,FixedName,
                    PilotExpPercent,AbilityExtensions,OperatorID)
                    VALUES(?,?,?,?,?,?,?,?,?)""", *values)
            cur.execute("DELETE FROM dbo.sdgo_OperatorRuleItems WHERE OperatorID=?", op["id"])
            cur.execute("DELETE FROM dbo.sdgo_OperatorRules WHERE OperatorID=?", op["id"])
            cumulative = 0
            for index, rule in enumerate(op["drops"]):
                if not cur.execute("SELECT 1 FROM dbo.tbl_ItemCategory WHERE CatalogueID=?", rule["category_id"]).fetchone():
                    raise ValueError("掉落物品在服务端不存在")
                cur.execute("INSERT dbo.sdgo_OperatorRules VALUES(?,?,?,?,?,?)", op["id"], index,
                            rule["category_id"], rule["chance_bps"], rule["min_amount"], rule["max_amount"])
                if 'pool' in rule:
                    for item_index,category_id in enumerate(catalog['drop_pools'][rule['pool']]['category_ids']):
                        cur.execute('INSERT dbo.sdgo_OperatorRuleItems VALUES(?,?,?,?)',op['id'],index,item_index,category_id)
                cumulative += rule["chance_bps"]
            if cumulative > 10000:
                raise ValueError("概率合计超过100%")
        install_procedures(cur)
        legacy_aliases=migrate_default_aliases(cur,catalog)
        entitlements=migrate_default_entitlements(cur,catalog)
        fixed_names_changed=migrate_fixed_default_names(cur,catalog)
        install_fixed_name_guard(cur)
        mission_parts_changed=apply_default_mission_parts(cur,catalog,prior_gparts)
        cur.execute("COMMIT TRANSACTION")
        return {"revision": rev, "managed_operators": len(managed), "storefront_changes": False,
                "native_capabilities": sorted(
                    runtime.trusted_capabilities(trusted_runtime)
                ) if trusted_runtime is not None else [],
                "native_capability_evidence": (
                    trusted_runtime.evidence
                    if trusted_runtime is not None else None
                ),
                "free_grant": "on normal native operator login/page reads", "player_nicknames_changed": bool(legacy_aliases or fixed_names_changed),
                "non_default_nicknames_changed":False,
                "legacy_script_aliases_cleared":legacy_aliases,
                "existing_default_entitlements_upgraded":entitlements,
                "fixed_default_names_applied":fixed_names_changed,
                "default_mission_parts_enabled":mission_parts_changed}
    except Exception:
        try:
            cn.cursor().execute("IF XACT_STATE()<>0 ROLLBACK TRANSACTION")
        except Exception:
            pass
        raise
    finally:
        cn.close()
