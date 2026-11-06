"""Exact Union grid fields and source-derived core runtime table rows.

This is a compiler input, not an admission shortcut. All field spans retain
their carrier bytes. In particular the byte consumed as ShieldRate is distinct
from the historical exporter's overlapping uint32 carrier, and the Union weapon
insertion at A0 is retained without assigning it a guessed meaning.
"""
from __future__ import annotations

import hashlib
import math
import struct
from typing import Any, Mapping, Sequence


LAYOUT = "union-5.4.6-client-grid-v1"
UNIT_SIZE = 4976
WEAPON_SIZE = 1871

# Names identify literal fields; they do not establish Union server policy.
UNIT_NUMBERS = {
    "unitId": (0, "I"), "securityRaw": (4, "B"),
    "transformIndexRaw": (0x131, "B"), "rankRaw": (0x134, "B"),
    "gradeVariantRaw": (0x135, "B"), "typeRaw": (0x136, "B"),
    "battleCostRaw": (0x137, "H"), "starRaw": (0x139, "B"),
    "transformTarget": (0x13C, "I"), "transformRequestConditionRaw": (0x140, "B"),
    "hpRaw": (0x141, "I"), "shieldRaw": (0x145, "I"),
    "shieldTypeRaw": (0x149, "B"), "shieldRateByteRaw": (0x14A, "B"),
    "shieldRateCarrierU32Raw": (0x14A, "I"),
    "moveLevelRaw": (0x21C, "B"), "dashTypeRaw": (0x21D, "B"),
    "dashLevelRaw": (0x21E, "B"), "quickStepLevelRaw": (0x21F, "B"),
    "additionalRateLevelRaw220": (0x220, "B"), "motivePowerRaw": (0x221, "B"),
    "boosterIncreaseRaw": (0x222, "h"), "boosterDecreaseRaw": (0x224, "h"),
    "boostGaugeMaximumRaw": (0x226, "h"), "attackRaw": (0x22C, "H"),
    "defenseRaw": (0x22E, "H"), "sightRaw": (0x23D, "H"),
    "skill1": (0x23F, "I"), "skill2": (0x243, "I"),
    "defaultWeaponIndexRaw": (0x248, "B"),
    **{f"weapon{slot}": (0x249 + slot * 4, "I") for slot in range(4)},
    "unitScaleRaw25B": (0x25B, "f"), "stunScaleClassRaw": (0x25F, "B"),
    "unitExtentRaw31E": (0x31E, "f"), "unitCollisionCenterYRaw322": (0x322, "f"),
    "swordWidthRaw": (0xF42, "H"), "swordCenterColorArgb": (0xF44, "I"),
    "swordEdgeColorArgb": (0xF48, "I"), "swordAlphaDecayPerRender": (0xFB0, "f"),
    "swordBlendModeRaw": (0xFB4, "B"), "leaderSpeedThresholdRawFb5": (0xFB5, "f"),
    "partitionFlagRaw": (0xFC1, "B"),
}
UNIT_TEXT = {
    "name": (5, 100), "englishName": (105, 100), "shieldId": (339, 40),
    "modelPath": (612, 60),
    **{f"weaponDisplayGroup{slot}": (0x326 + slot * 300, 300) for slot in range(4)},
    **{f"weaponAttachment{slot}": (0x902 + slot * 200, 200) for slot in range(4)},
    "swordTexturePath": (0xF4C, 100), "partitionNames": (0xFC2, 200),
    "partitionAdditionalNode": (0x108A, 100),
    "selectionHideNodes": (0xE7A, 300),
}
WEAPON_NUMBERS = {
    "weaponId": (0, "I"), "securityRaw": (4, "B"),
    "attackStyleRaw": (105, "B"), "rangeTypeRaw": (106, "B"), "hitTypeRaw": (107, "B"),
    "damageRaw": (108, "i"), "speedRateRaw": (112, "H"), "rangeRaw": (114, "I"),
    "attackSpeedRate": (118, "f"), "extraAttackTypeRaw": (122, "B"),
    "reloadModeRaw": (123, "B"), "ammo": (124, "H"),
    "reloadThresholdWireU16Raw": (126, "H"), "reloadThresholdRaw": (126, "h"),
    "knockbackRaw": (128, "H"), "cooldownRaw": (130, "H"),
    "specialFeatureId": (132, "I"), "hurtTypeRaw": (136, "B"),
    "stunBaseRaw": (137, "H"), "stunFinalRaw": (139, "H"),
    "pierce": (141, "B"), "pierceRate": (142, "B"), "movingShot": (143, "B"),
    "collisionWidthRaw": (148, "f"), "collisionHeightRaw": (152, "f"),
    "collisionRadiusRaw": (156, "f"), "unionInsertedU32RawA0": (160, "I"),
    "projectileScaleRaw": (284, "f"), "maxCombo": (612, "B"),
    "projectileVisualFlag42Raw": (1353, "B"), "projectileVisualFlag43Raw": (1354, "B"),
    "projectileVisualScaleRaw": (1455, "f"), "projectileVisualLifetimeMsRaw": (1459, "I"),
    "projectileTailFlag42Raw": (1463, "B"), "projectileTailFlag43Raw": (1464, "B"),
    "funnelIdRaw": (1565, "I"), "projectileInitRaw": (1769, "H"),
}
WEAPON_TEXT = {
    "name": (5, 100), "muzzleEffectPath": (164, 60), "impactEffectPath": (224, 60),
    "soundPath": (288, 60), "projectileModelPath": (348, 60),
    "jumpActionName": (412, 100), "dashActionName": (512, 100),
    **{f"attackAction{slot}": (613 + slot * 100, 100) for slot in range(5)},
    "meleeCollisionNodeNames": (1113, 100),
    "weaponHudTexturePath": (1233, 60), "weaponHudSelectedTexturePath": (1293, 60),
    "projectileTexturePath": (1355, 60), "secondarySoundPath": (1465, 100),
    "rangedScriptFunction": (1569, 100), "specialRangedScriptFunction": (1669, 100),
    "projectileInitFunction": (1771, 100),
}


def decode_record(raw: bytes, kind: str, source_row: int, *,
                  expected_sha256: str | None = None) -> dict[str, Any]:
    """Decode exact spans; nonfinite floats retain their bits, never a default."""
    if kind not in {"unit", "weapon"}:
        raise ValueError("unit or weapon kind required")
    size = UNIT_SIZE if kind == "unit" else WEAPON_SIZE
    if not isinstance(raw, bytes) or len(raw) != size:
        raise ValueError(f"{kind} raw record size mismatch")
    if type(source_row) is not int or source_row < 0:
        raise ValueError("nonnegative source row required")
    digest = hashlib.sha256(raw).hexdigest()
    if expected_sha256 is not None and digest != expected_sha256:
        raise ValueError("raw record SHA-256 mismatch")
    numbers, strings = ((UNIT_NUMBERS, UNIT_TEXT) if kind == "unit"
                        else (WEAPON_NUMBERS, WEAPON_TEXT))
    fields = {}
    for name, (offset, primitive) in numbers.items():
        carrier = raw[offset:offset + struct.calcsize("<" + primitive)]
        value = struct.unpack("<" + primitive, carrier)[0]
        finite = primitive != "f" or math.isfinite(value)
        fields[name] = {"offset": offset, "size": len(carrier), "primitive": primitive,
                        "rawHex": carrier.hex(), "value": value if finite else None}
        if not finite:
            fields[name]["unresolved"] = "nonfinite-source-float"
    for name, (offset, size) in strings.items():
        carrier = raw[offset:offset + size]
        end = next((i for i in range(0, size, 2) if carrier[i:i + 2] == b"\0\0"), size)
        fields[name] = {"offset": offset, "size": size, "primitive": "utf16le",
                        "rawHex": carrier.hex(), "terminatorOffset": offset + end if end < size else None,
                        "value": carrier[:end].decode("utf-16le", errors="strict")}
    if kind == "unit":
        fields["shieldDirectionModesRaw"] = {
            "offset": 333, "size": 6, "primitive": "uint8[6]",
            "rawHex": raw[333:339].hex(), "value": list(raw[333:339]),
        }
    return {"schema": "gunion-minecraft-grid-fields/1", "layout": LAYOUT, "kind": kind,
            "sourceRow": source_row, "recordSha256": digest, "recordSize": len(raw), "fields": fields}


def known_unit_fields(decoded: Mapping[str, Any]) -> dict[str, Any]:
    """Fields whose unit conversion needs no missing action/movement adapter.

    Guard routing, runtime hardpoint fallback and sword interpolation constants
    remain outside this source literal projection.
    """
    if decoded["kind"] != "unit":
        raise ValueError("unit fields required")
    f = {name: cell["value"] for name, cell in decoded["fields"].items()}
    return {
        "stats": {"hp": round(f["hpRaw"] * 0.04, 6),
                  "attack": f["attackRaw"], "defense": f["defenseRaw"]},
        "sourceCombat": {"stunScaleClassRaw": f["stunScaleClassRaw"],
                         "shieldDirectionModesRaw": f["shieldDirectionModesRaw"],
                         "shieldId": f["shieldId"], "shieldRateByteRaw": f["shieldRateByteRaw"]},
        "vfxRuntime": {
            "sourceWeaponAttachmentNodes": [f[f"weaponAttachment{i}"] for i in range(4)],
            "swordLightProfile": {"widthRaw": f["swordWidthRaw"],
                                  "centerColorArgb": f["swordCenterColorArgb"],
                                  "edgeColorArgb": f["swordEdgeColorArgb"],
                                  "texturePath": f["swordTexturePath"],
                                  "alphaDecayPerRender": f["swordAlphaDecayPerRender"],
                                  "blendModeRaw": f["swordBlendModeRaw"]},
        },
    }


def core_table_rows(units: Sequence[Mapping[str, Any]], weapons: Sequence[Mapping[str, Any]], *,
                    unit_evidence: Sequence[str], weapon_evidence: Sequence[str],
                    baseline_tables: Mapping[str, Mapping[str, Any]] | None = None) -> dict[str, Any]:
    """Compile partitions, banks and reload rows; never overwrite another domain.

    baseline_tables maps table names to their existing profiles. Equal entries
    need no new row; conflicts stay explicit and are not published.
    """
    if not unit_evidence or not weapon_evidence:
        raise ValueError("source and native-consumer evidence required for both domains")
    baseline_tables = baseline_tables or {}
    rows, blocked, unchanged = [], [], []
    seen = set()

    def emit(table, key, value, evidence, record):
        identity = (table, key)
        if identity in seen:
            raise ValueError("duplicate source runtime row")
        seen.add(identity)
        existing = baseline_tables.get(table, {}).get(key)
        provenance = {"recordSha256": record["recordSha256"], "sourceRow": record["sourceRow"]}
        if existing is not None:
            item = {"table": table, "key": key, **provenance}
            if existing == value:
                unchanged.append(item)
            else:
                blocked.append({**item, "reason": "existing-domain-value-conflict", "sourceValue": value,
                                "existingValue": existing})
            return
        rows.append({"table": table, "path": ["profiles"], "key": key, "value": value,
                     "inputEvidence": list(evidence), "sourceRecord": provenance})

    for record in units:
        if record.get("kind") != "unit" or record.get("layout") != LAYOUT:
            raise ValueError("exact Union unit layout required")
        f = {k: v["value"] for k, v in record["fields"].items()}
        key = str(f["unitId"])
        names = f["partitionNames"]
        roots = names.split(";") if names else ["Bipcore", "Bip03A", "Bip03"]
        emit("runtime/animation_partitions.json", key,
             {"enabled": f["partitionFlagRaw"] == 1 and len(roots) == 3,
              "roots": roots if len(roots) == 3 else [], "additionalNode": f["partitionAdditionalNode"],
              "sourceFlag": f["partitionFlagRaw"], "sourceNames": names, "sourceTable": "0237",
              "recordSha256": record["recordSha256"]}, unit_evidence, record)
        default = f["defaultWeaponIndexRaw"] - 1
        if default not in range(4):
            blocked.append({"table": "runtime/weapon_banks.json", "key": key,
                            "reason": "invalid-source-default-slot", "raw": f["defaultWeaponIndexRaw"]})
        else:
            emit("runtime/weapon_banks.json", key,
                 {"defaultSlot": default, "alternateFormId": str(f["transformTarget"]) if f["transformTarget"] else ""},
                 unit_evidence, record)
    weapon_groups = {}
    for record in weapons:
        weapon_groups.setdefault(record["fields"]["weaponId"]["value"], []).append(record)
    source_variants = []
    for group in weapon_groups.values():
        if len(group) > 1:
            projections = {(r["fields"]["reloadModeRaw"]["value"],
                            r["fields"]["reloadThresholdRaw"]["value"]) for r in group}
            source_variants.append({"weaponId": group[0]["fields"]["weaponId"]["value"],
                                    "sourceRows": [r["sourceRow"] for r in group],
                                    "recordSha256": [r["recordSha256"] for r in group],
                                    "reloadProjectionIdentical": len(projections) == 1})
            if len(projections) != 1:
                blocked.append({"table": "runtime/weapon_reload.json", "key": str(source_variants[-1]["weaponId"]),
                                "reason": "source-row-reload-value-conflict", "sourceRows": source_variants[-1]["sourceRows"]})
                continue
        record = group[0]
        if record.get("kind") != "weapon" or record.get("layout") != LAYOUT:
            raise ValueError("exact Union weapon layout required")
        f = {k: v["value"] for k, v in record["fields"].items()}
        key = str(f["weaponId"])
        if f["reloadModeRaw"] not in (0, 1) or f["reloadThresholdRaw"] < 0:
            blocked.append({"table": "runtime/weapon_reload.json", "key": key,
                            "reason": "unsupported-source-reload-policy", "modeRaw": f["reloadModeRaw"],
                            "thresholdRaw": f["reloadThresholdRaw"]})
        else:
            emit("runtime/weapon_reload.json", key,
                 {"modeRaw": f["reloadModeRaw"], "thresholdRaw": f["reloadThresholdRaw"]},
                 weapon_evidence, record)
    return {"schema": "gunion-minecraft-core-tables/1", "rows": rows, "blocked": blocked,
            "unchangedExisting": unchanged, "sourceRowVariants": source_variants, "playable": False}
