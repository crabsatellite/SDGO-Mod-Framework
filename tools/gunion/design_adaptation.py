"""Explicit design adaptations for missing server values, separate from source facts.

This module has no database, file, installation or activation operations. Callers
provide a hash-pinned definition snapshot and opt into the owner's design policy.
Recovered values, including zero, always win. Neither local templates nor the
resulting values represent the missing original server database.
"""
from __future__ import annotations

from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from fractions import Fraction
import hashlib
import json
import math
from types import MappingProxyType
from typing import Any


AUTHORIZATION = "user-approved-missing-server-design-adaptation"
SCHEMA = "gunion-design-adaptation/1"
REFERENCE_SCHEMA = "local-design-definition-snapshot/1"
UNIT_FIELDS = frozenset({"FatalAttackIncrease"})
WEAPON_FIELDS = frozenset({"AttackSpeed"})
FEATURE_WEIGHTS = {"HP": 4, "StrikingPower": 3, "DefensivePower": 3,
                   "MoveLevel": 1, "DashLevel": 1, "MotivePower": 1}


@dataclass(frozen=True)
class DefinitionReference:
    sha256: str
    units: tuple[Mapping[str, Any], ...]
    weapons: tuple[Mapping[str, Any], ...]
    skill_ids: frozenset[int]


def decode_reference(raw: bytes, *, expected_sha256: str) -> DefinitionReference:
    if not isinstance(raw, bytes) or hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError("definition reference SHA-256 mismatch")
    document = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    if document.get("schema") != REFERENCE_SCHEMA:
        raise ValueError("unsupported definition reference schema")
    tables = document.get("tables", {})
    units = _rows(tables, "tbl_Units", "UnitNumber")
    weapons = _rows(tables, "tbl_WeaponsRev", "WeaponNo")
    skills = _rows(tables, "tbl_SkillRev", "SkillID")
    for row in units:
        _integer(row.get("FatalAttackIncrease"), 0, 32767, "reference FatalAttackIncrease")
        for key in ("UnitGradeNumber", "MasteredWeaponType", "UnitGradeNumber2"):
            _integer(row.get(key), 0, 255, "reference " + key)
        for key in FEATURE_WEIGHTS:
            _finite(row.get(key), "reference " + key)
    return DefinitionReference(expected_sha256, units, weapons,
                               frozenset(row["SkillID"] for row in skills))


def adapt_unit(source_values: Mapping[str, Any], *, reference: DefinitionReference,
               authorization: str, excluded_template_ids: Iterable[int] = (),
               neighbor_count: int = 5) -> dict[str, Any]:
    """Fill only missing FatalAttackIncrease from comparable local definitions.

    Candidates must match grade and weapon type. Exact rational distances compare
    relative HP, attack, defense and movement differences, with a small subtype
    penalty. The lower median of up to five closest values is an actual observed
    local value, never an extrapolation. No caller-provided narrative guess can
    replace a recovered field through this adapter.
    """
    _authorized(authorization)
    values = dict(source_values)
    _integer(values.get("UnitNumber"), 1, 2147483647, "UnitNumber")
    if "FatalAttackIncrease" in values:
        _integer(values["FatalAttackIncrease"], 0, 32767, "source FatalAttackIncrease")
        return _result(values, {}, reference)
    _integer(neighbor_count, 1, 31, "neighbor_count")
    excluded = set(excluded_template_ids)
    candidates = [row for row in reference.units
                  if row["UnitNumber"] not in excluded
                  and row["UnitGradeNumber"] == values.get("UnitGradeNumber")
                  and row["MasteredWeaponType"] == values.get("MasteredWeaponType")]
    if not candidates:
        raise ValueError("no local template matches unit grade and weapon type")
    for key in FEATURE_WEIGHTS:
        _finite(values.get(key), "source " + key)
    candidates.sort(key=lambda row: (_distance(values, row), row["UnitNumber"]))
    selected = candidates[:neighbor_count]
    chosen = sorted(row["FatalAttackIncrease"] for row in selected)[(len(selected)-1)//2]
    values["FatalAttackIncrease"] = chosen
    provenance = {"FatalAttackIncrease": _provenance(
        chosen, "grade-and-weapon-type-nearest-lower-median", selected,
        "UnitNumber", reference)}
    return _result(values, provenance, reference)


def adapt_weapon(source_values: Mapping[str, Any], *, reference: DefinitionReference,
                 authorization: str, excluded_template_ids: Iterable[int] = ()) -> dict[str, Any]:
    """Fill an absent AttackSpeed only; rejected source values require explicit omission.

    The source record bytes remain the caller's immutable provenance. This
    projection chooses an existing positive finite speed from the same native
    attack/range/hit class, prioritizing matching combo and ammunition behavior.
    """
    _authorized(authorization)
    values = dict(source_values)
    _integer(values.get("WeaponNo"), 1, 2147483647, "WeaponNo")
    if "AttackSpeed" in values:
        _finite(values["AttackSpeed"], "source AttackSpeed")
        return _result(values, {}, reference)
    excluded = set(excluded_template_ids)
    candidates = [row for row in reference.weapons
                  if row["WeaponNo"] not in excluded
                  and all(row.get(key) == values.get(key)
                          for key in ("AttackStyle", "RangeType", "HitType"))
                  and _positive_finite(row.get("AttackSpeed"))]
    if not candidates:
        raise ValueError("no local weapon template matches attack/range/hit class")
    def distance(row):
        return (abs(row.get("MaxCombo", 0)-values.get("MaxCombo", 0)),
                int(bool(row.get("BulletRemain")) != bool(values.get("BulletRemain"))),
                abs(row.get("RangeSize", 0)-values.get("RangeSize", 0)), row["WeaponNo"])
    candidates.sort(key=distance)
    selected = candidates[:5]
    chosen = sorted(row["AttackSpeed"] for row in selected)[(len(selected)-1)//2]
    values["AttackSpeed"] = chosen
    return _result(values, {"AttackSpeed": _provenance(
        chosen, "matching-weapon-class-nearest-lower-median", selected,
        "WeaponNo", reference)}, reference)


def _distance(values, template):
    result = Fraction(int(values.get("UnitGradeNumber2") != template["UnitGradeNumber2"]), 10)
    for key, weight in FEATURE_WEIGHTS.items():
        left, right = Fraction(str(values[key])), Fraction(str(template[key]))
        result += weight * abs(left-right) / max(abs(left), abs(right), 1)
    return result


def _result(values, provenance, reference):
    return {"schema": SCHEMA, "authorization": AUTHORIZATION,
            "reference_sha256": reference.sha256, "values": values,
            "design_values": {key: values[key] for key in provenance},
            "design_provenance": provenance,
            "nativeActivationAllowed": False, "playable": False}


def _provenance(value, rule, selected, key, reference):
    return {"authority": "design-adaptation", "authorization": AUTHORIZATION,
            "not_union_source": True, "rule": rule, "value": value,
            "reference_sha256": reference.sha256,
            "template_ids": [row[key] for row in selected]}


def _rows(tables, name, key):
    table = tables.get(name, {})
    rows = table.get("rows")
    if table.get("available") is not True or not isinstance(rows, list) or not rows:
        raise ValueError("missing reference table: " + name)
    seen = set()
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("invalid reference row in " + name)
        _integer(row.get(key), 0, 2147483647, name + "." + key)
        if row[key] in seen:
            raise ValueError("duplicate reference key in " + name)
        seen.add(row[key])
    canonical = json.dumps(rows, ensure_ascii=False, sort_keys=True,
                           separators=(",", ":"), allow_nan=False).encode("utf-8")
    if hashlib.sha256(canonical).hexdigest() != table.get("rows_sha256"):
        raise ValueError("reference row digest mismatch in " + name)
    return tuple(MappingProxyType(dict(row)) for row in rows)


def _authorized(value):
    if value != AUTHORIZATION:
        raise ValueError("explicit missing-server design authorization required")


def _integer(value, low, high, label):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ValueError(label + " is outside integer range")


def _finite(value, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(label + " must be finite numeric data")


def _positive_finite(value):
    return (not isinstance(value, bool) and isinstance(value, (int, float))
            and math.isfinite(value) and value > 0)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result
