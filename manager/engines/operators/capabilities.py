"""Strict operator-ability schema and deterministic native row compiler.

The native skill table's PointAdd field is the GB/G-coin percentage.  Pilot
experience is deliberately kept out of PointAdd and requires its own verified
runtime capability.
"""
import copy
import json
import re


ABILITY_KEYS = {
    "schema_version", "mode", "general", "victory", "defeat", "timed",
    "pilot_exp_percent", "mission_parts", "extensions",
}
BONUS_KEYS = {"gp_percent", "unit_exp_percent", "energy_save_percent"}
TIME_KEYS = {"start", "end", *BONUS_KEYS}
PROTECTED_SKILL_MAX = 27
EXTENSION_NAME = re.compile(r"[a-z][a-z0-9_.-]{0,63}\Z")


def _percent(value, label):
    if type(value) is not int or not 0 <= value <= 100:
        raise ValueError(f"{label} 必须是 0–100 的整数")
    return value


def _bonus(value, label):
    if not isinstance(value, dict) or set(value) != BONUS_KEYS:
        raise ValueError(f"{label} 奖励配置字段不完整")
    return {
        "gp_percent": _percent(value["gp_percent"], label + " GB"),
        "unit_exp_percent": _percent(value["unit_exp_percent"], label + " 机体经验"),
        "energy_save_percent": _percent(value["energy_save_percent"], label + " 能量节省"),
    }


def _clock(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"(?:[01][0-9]|2[0-3]):[0-5][0-9]", value):
        raise ValueError(f"{label} 必须使用 HH:MM")
    hour, minute = (int(part) for part in value.split(":"))
    return hour * 60 + minute


def registered_names(value):
    if value is None:
        return set()
    if isinstance(value, dict):
        names = {name for name, enabled in value.items() if enabled}
    elif isinstance(value, (list, tuple, set, frozenset)):
        names = set(value)
    else:
        raise ValueError("副官能力扩展登记格式无效")
    if any(not isinstance(name, str) or not EXTENSION_NAME.fullmatch(name) for name in names):
        raise ValueError("副官能力扩展名称无效")
    return names


def normalize(value, registered_extensions=()):
    """Validate and return a detached canonical abilities object.

    ``None`` means full template inheritance and remains absent from old
    catalogs.  An explicit inherit object is accepted only when it carries no
    dormant custom behavior.
    """
    if value is None:
        return None
    if not isinstance(value, dict) or set(value) != ABILITY_KEYS:
        raise ValueError("副官能力配置字段不完整或包含未知字段")
    if value["schema_version"] != 1:
        raise ValueError("不支持的副官能力配置版本")
    if value["mode"] not in ("inherit", "custom"):
        raise ValueError("副官能力模式只能是 inherit 或 custom")

    result = {
        "schema_version": 1,
        "mode": value["mode"],
        "general": _bonus(value["general"], "常规"),
        "victory": _bonus(value["victory"], "胜利"),
        "defeat": _bonus(value["defeat"], "失败"),
        "timed": [],
        "pilot_exp_percent": _percent(value["pilot_exp_percent"], "驾驶员经验"),
        "mission_parts": value["mission_parts"],
        "extensions": copy.deepcopy(value["extensions"]),
    }
    if result["mission_parts"] is not None and type(result["mission_parts"]) is not bool:
        raise ValueError("任务部件能力必须是布尔值或 null")
    if not isinstance(result["extensions"], dict):
        raise ValueError("副官能力扩展必须是对象")

    registered = registered_names(registered_extensions)
    for name in result["extensions"]:
        if not isinstance(name, str) or not EXTENSION_NAME.fullmatch(name):
            raise ValueError("副官能力扩展名称无效")
        if name not in registered:
            raise ValueError("副官能力需要未登记扩展：" + name)
    timed = value["timed"]
    if not isinstance(timed, list) or len(timed) > 4:
        raise ValueError("定时能力最多配置 4 个连续槽位")
    previous_start = -1
    for index, entry in enumerate(timed):
        if not isinstance(entry, dict) or set(entry) != TIME_KEYS:
            raise ValueError("定时能力字段不完整或包含未知字段")
        start = _clock(entry["start"], f"定时能力 {index + 1} 开始时间")
        end = _clock(entry["end"], f"定时能力 {index + 1} 结束时间")
        if start > end:
            raise ValueError("定时能力暂不支持跨午夜时间段")
        if start < previous_start:
            raise ValueError("定时能力槽位必须按开始时间排序")
        previous_start = start
        bonus = _bonus({key: entry[key] for key in BONUS_KEYS}, f"定时能力 {index + 1}")
        result["timed"].append({
            "start": entry["start"], "end": entry["end"], **bonus,
        })

    if result["mode"] == "inherit":
        dormant = (
            any(result[name][key] for name in ("general", "victory", "defeat") for key in BONUS_KEYS)
            or result["timed"]
            or result["pilot_exp_percent"]
            or result["mission_parts"] is not None
            or result["extensions"]
        )
        if dormant:
            raise ValueError("inherit 模式不能携带自定义能力")
    # Ensure extension payloads are serializable and finite under the same
    # JSON contract used by catalog revision hashes.
    try:
        json.dumps(result["extensions"], allow_nan=False)
    except (TypeError, ValueError):
        raise ValueError("副官能力扩展数据不是有效 JSON") from None
    return result


def required_extensions(value):
    """Return extension names required by a normalized ability object."""
    if value is None:
        return set()
    return set(value["extensions"])


def required_native_capabilities(value):
    if value is not None and value["pilot_exp_percent"]:
        return {"pilot_exp_percent"}
    return set()


def skill_tuple(value):
    """Return native (PointAdd, UnitExpAdd, EnergySave) fields."""
    return (
        value["gp_percent"],
        value["unit_exp_percent"],
        value["energy_save_percent"],
    )


def hhmm(value):
    minutes = _clock(value, "能力时间")
    return (minutes // 60) * 100 + minutes % 60


def _skill_row(row):
    return {
        "SkillNo": int(row["SkillNo"]),
        "SkillType": int(row["SkillType"]),
        "PointAdd": int(row["PointAdd"]),
        "UnitExpAdd": int(row["UnitExpAdd"]),
        "EnergySave": int(row["EnergySave"]),
    }


def _time_row(row):
    return {
        "TimeID": int(row["TimeID"]),
        "AddOnID": int(row["AddOnID"]),
        "OperatorSkill": int(row["OperatorSkill"]),
        "StartTime": int(row["StartTime"]),
        "CloseTime": int(row["CloseTime"]),
    }


def _next_free(occupied, start):
    value = start
    while value in occupied:
        value += 1
    occupied.add(value)
    return value


def compile_plan(
    operators,
    native_skills,
    native_times,
    *,
    owned_skills=(),
    owned_times=(),
    allow_new_skill_ids=False,
    allow_new_time_ids=False,
):
    """Compile enabled custom abilities without mutating any input.

    Existing exact type-1 skills are reused.  Module-owned rows may be
    deterministically reassigned; all unowned rows are treated as occupied.
    New skill IDs start at 28 so original skills 1–27 are never modified.
    Custom timed rows use one low free positive integer for both TimeID and
    AddOnID, avoiding either native namespace.
    """
    skills = [_skill_row(row) for row in native_skills]
    times = [_time_row(row) for row in native_times]
    owned_skill_rows = {int(row["SkillNo"]): _skill_row(row) for row in owned_skills}
    owned_time_rows = {int(row["TimeID"]): _time_row(row) for row in owned_times}

    skill_by_id = {row["SkillNo"]: row for row in skills}
    if len(skill_by_id) != len(skills):
        raise ValueError("服务端副官技能编号重复")
    for identity, expected in owned_skill_rows.items():
        if identity <= PROTECTED_SKILL_MAX:
            raise ValueError("自定义副官技能登记侵入原版 1–27")
        if skill_by_id.get(identity) != expected:
            raise ValueError(f"自定义副官技能 {identity} 已被其他内容改变")

    time_by_id = {row["TimeID"]: row for row in times}
    if len(time_by_id) != len(times):
        raise ValueError("服务端副官时间编号重复")
    add_on_ids = [row["AddOnID"] for row in times]
    if len(set(add_on_ids)) != len(add_on_ids):
        raise ValueError("服务端副官时间 AddOnID 重复")
    for identity, expected in owned_time_rows.items():
        if time_by_id.get(identity) != expected:
            raise ValueError(f"自定义副官时间 {identity} 已被其他内容改变")

    custom = []
    required_skill_tuples = set()
    required_native = set()
    required_extension_names = set()
    for op in sorted(operators, key=lambda item: int(item["id"])):
        abilities = op.get("abilities")
        if not op.get("managed") or not op.get("enabled") or abilities is None or abilities["mode"] != "custom":
            continue
        entry = {"id": int(op["id"]), "abilities": abilities}
        custom.append(entry)
        required_native.update(required_native_capabilities(abilities))
        required_extension_names.update(required_extensions(abilities))
        required_skill_tuples.update(skill_tuple(abilities[name]) for name in ("general", "victory", "defeat"))
        required_skill_tuples.update(skill_tuple(slot) for slot in abilities["timed"])

    exact_skills = {}
    for row in sorted(skills, key=lambda item: item["SkillNo"]):
        if row["SkillType"] == 1:
            exact_skills.setdefault(
                (row["PointAdd"], row["UnitExpAdd"], row["EnergySave"]),
                row["SkillNo"],
            )

    # Owned IDs are reclaimable for a changed plan, but exact existing owned
    # rows remain the first choice and therefore stay stable.
    unowned_skill_ids = set(skill_by_id) - set(owned_skill_rows)
    reserved_exact_skill_ids = {
        exact_skills[values] for values in required_skill_tuples
        if values in exact_skills
    }
    occupied_skill_ids = set(unowned_skill_ids) | reserved_exact_skill_ids
    compiled_skills = []
    tuple_to_skill = {}
    for values in sorted(required_skill_tuples):
        identity = exact_skills.get(values)
        owned = identity in owned_skill_rows if identity is not None else False
        if identity is None:
            if not allow_new_skill_ids:
                raise ValueError(
                    "能力需要新的原生 SkillNo；隔离 SQL 分页/加载/实战验收前不可用"
                )
            identity = _next_free(occupied_skill_ids, PROTECTED_SKILL_MAX + 1)
            owned = True
        tuple_to_skill[values] = identity
        if owned:
            compiled_skills.append({
                "SkillNo": identity, "SkillType": 1,
                "PointAdd": values[0], "UnitExpAdd": values[1], "EnergySave": values[2],
            })

    existing_time_by_signature = {}
    for row in sorted(times, key=lambda item: (item["TimeID"], item["AddOnID"])):
        signature = (row["OperatorSkill"], row["StartTime"], row["CloseTime"])
        existing_time_by_signature.setdefault(signature, row)

    required_times = set()
    for entry in custom:
        for slot in entry["abilities"]["timed"]:
            required_times.add((
                tuple_to_skill[skill_tuple(slot)], hhmm(slot["start"]), hhmm(slot["end"])
            ))

    unowned_times = [row for row in times if row["TimeID"] not in owned_time_rows]
    occupied_time_ids = {row["TimeID"] for row in unowned_times}
    occupied_add_on_ids = {row["AddOnID"] for row in unowned_times}
    reserved_exact_times = [
        existing_time_by_signature[signature] for signature in required_times
        if signature in existing_time_by_signature
    ]
    occupied_joint_ids = (
        occupied_time_ids | occupied_add_on_ids |
        {row["TimeID"] for row in reserved_exact_times} |
        {row["AddOnID"] for row in reserved_exact_times}
    )
    signature_to_add_on = {}
    compiled_times = []
    for signature in sorted(required_times):
        row = existing_time_by_signature.get(signature)
        owned = row is not None and row["TimeID"] in owned_time_rows
        if row is None:
            if not allow_new_time_ids:
                raise ValueError(
                    "能力需要新的原生 TimeID；隔离 SQL 分页/加载/实战验收前不可用"
                )
            identity = _next_free(occupied_joint_ids, 1)
            row = {
                "TimeID": identity, "AddOnID": identity,
                "OperatorSkill": signature[0],
                "StartTime": signature[1], "CloseTime": signature[2],
            }
            owned = True
        signature_to_add_on[signature] = row["AddOnID"]
        if owned:
            compiled_times.append(copy.deepcopy(row))

    operator_plan = {}
    for entry in custom:
        abilities = entry["abilities"]
        slots = []
        for slot in abilities["timed"]:
            signature = (
                tuple_to_skill[skill_tuple(slot)], hhmm(slot["start"]), hhmm(slot["end"])
            )
            slots.append(signature_to_add_on[signature])
        slots.extend([0] * (4 - len(slots)))
        operator_plan[entry["id"]] = {
            "BattleResult": tuple_to_skill[skill_tuple(abilities["general"])],
            "TeamWin": tuple_to_skill[skill_tuple(abilities["victory"])],
            "TeamLose": tuple_to_skill[skill_tuple(abilities["defeat"])],
            "AddOn1": slots[0], "AddOn2": slots[1],
            "AddOn3": slots[2], "AddOn4": slots[3],
            "mission_parts": abilities["mission_parts"],
            "pilot_exp_percent": abilities["pilot_exp_percent"],
            "extensions": copy.deepcopy(abilities["extensions"]),
        }
    return {
        "operators": operator_plan,
        "owned_skills": sorted(compiled_skills, key=lambda row: row["SkillNo"]),
        "owned_times": sorted(compiled_times, key=lambda row: row["TimeID"]),
        "required_native_capabilities": sorted(required_native),
        "required_extensions": sorted(required_extension_names),
    }
