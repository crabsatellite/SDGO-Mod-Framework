"""Strict decoders for the verified client-side SDGO unit record layouts.

Only offsets that have been checked against the current client and catalog
exports are named here.  Every caller also receives the original bytes so that
unexplained fields are never discarded or re-encoded by this module.
"""
from __future__ import annotations

import math
import re
import struct
from typing import Any


UNIT_SIZE = 4976
WEAPON_SIZE = 1871
MAX_NATIVE_ID = 9_999_999

RESOURCE_ROOTS = ("gdrs", "mdrs", "anrs", "txrs", "fxrs", "sprs", "sdrs", "scp")
_RESOURCE_ROOT_PATTERN = "|".join(RESOURCE_ROOTS)
_RESOURCE_PATH_RE = re.compile(
    rf"(?i)(?<![a-z0-9_.-])"
    rf"((?:{_RESOURCE_ROOT_PATTERN})[\\/]"
    rf"(?:[a-z0-9_.-]+[\\/])*[a-z0-9_.-]+\.[a-z0-9]{{1,8}})"
)
_BARE_TEXTURE_RE = re.compile(r"(?i)(?<![a-z0-9_.-])([a-z0-9_-]{2,64}\.txr)(?![a-z0-9_.-])")
_VIRTUAL_PATH_RE = re.compile(
    rf"(?i)^(?:{_RESOURCE_ROOT_PATTERN})/"
    r"(?:[a-z0-9][a-z0-9_.-]*/)*[a-z0-9][a-z0-9_.-]*$"
)

# Public field descriptions for the authoring UI.  Portrait path offsets are
# verified; the exact game-screen purpose labels still need runtime acceptance.
UNIT_FIELDS: dict[str, dict[str, Any]] = {
    "UnitNumber": {"offset": 0, "type": "u32", "verified": True},
    "security": {"offset": 4, "type": "u8", "verified": True, "required_value": 0},
    "name": {"offset": 5, "type": "utf16le", "bytes": 100, "verified": True},
    "UnitGradeNumber": {"offset": 308, "type": "u8", "verified": True},
    "UnitGradeNumber2": {"offset": 309, "type": "u8", "verified": True},
    "MasteredWeaponType": {"offset": 310, "type": "u8", "verified": True},
    "BattleCost": {"offset": 311, "type": "u16", "verified": True},
    "Star": {"offset": 313, "type": "u8", "verified": True},
    "UnitNumber_Transform": {"offset": 316, "type": "u32", "verified": True},
    "HP": {"offset": 321, "type": "u32", "verified": True},
    "Shield": {"offset": 325, "type": "u32", "verified": True},
    "ShieldType": {"offset": 329, "type": "u8", "verified": True},
    "ShieldRate": {"offset": 330, "type": "u32", "verified": True},
    "MoveLevel": {"offset": 540, "type": "u8", "verified": True},
    "DashType": {"offset": 541, "type": "u8", "verified": True},
    "DashLevel": {"offset": 542, "type": "u8", "verified": True},
    "QuickStepLevel": {"offset": 543, "type": "u8", "verified": True},
    "MotivePower": {"offset": 545, "type": "u8", "verified": True},
    "BoosterIncrease": {"offset": 546, "type": "u16", "verified": True},
    "BoosterDecrease": {"offset": 548, "type": "u16", "verified": True},
    "StrikingPower": {"offset": 556, "type": "u16", "verified": True},
    "DefensivePower": {"offset": 558, "type": "u16", "verified": True},
    "AdaptEnvironType": {"offset": 562, "type": "u8", "verified": True},
    "AdaptEnvironSpace": {"offset": 563, "type": "i16", "verified": True},
    "AdaptEnvironGround": {"offset": 565, "type": "i16", "verified": True},
    "AdaptEnvironUnderwater": {"offset": 567, "type": "i16", "verified": True},
    "AdaptEnvironDesert": {"offset": 569, "type": "i16", "verified": True},
    "AdaptEnvironSky": {"offset": 571, "type": "i16", "verified": True},
    "Sight": {"offset": 573, "type": "u16", "verified": True},
    "SkillNumber_1": {"offset": 575, "type": "u32", "verified": True},
    "SkillNumber_2": {"offset": 579, "type": "u32", "verified": True},
    # The native client stores this shared catalog value in one byte even
    # though the BattleServer prefix reserves a uint32 for it.
    "MasteredWeaponIX": {"offset": 584, "type": "u8", "verified": True},
    "WeaponNumber_1": {"offset": 585, "type": "u32", "verified": True},
    "WeaponNumber_2": {"offset": 589, "type": "u32", "verified": True},
    "WeaponNumber_3": {"offset": 593, "type": "u32", "verified": True},
    "WeaponNumber_FatalAttack": {"offset": 597, "type": "u32", "verified": True},
    "model_path": {"offset": 612, "type": "utf16le", "bytes": 60, "verified": True},
    "model_override": {"offset": 738, "type": "utf16le", "bytes": 60, "verified": True},
    "portrait_shop": {
        "offset": 4654,
        "type": "utf16le",
        "bytes": 100,
        "verified": True,
        "purpose_verified_in_game": False,
    },
    "portrait_capsule": {
        "offset": 4754,
        "type": "utf16le",
        "bytes": 100,
        "verified": True,
        "purpose_verified_in_game": False,
    },
    "portrait_detail": {
        "offset": 4854,
        "type": "utf16le",
        "bytes": 100,
        "verified": True,
        "purpose_verified_in_game": False,
    },
}

WEAPON_FIELDS: dict[str, dict[str, Any]] = {
    "WeaponNo": {"offset": 0, "type": "u32", "verified": True},
    "security": {"offset": 4, "type": "u8", "verified": True, "required_value": 0},
    "WeaponName": {"offset": 5, "type": "utf16le", "bytes": 100, "verified": True},
    "AttackStyle": {"offset": 105, "type": "u8", "verified": True},
    "RangeType": {"offset": 106, "type": "u8", "verified": True},
    "HitType": {"offset": 107, "type": "u8", "verified": True},
    "DamageSize": {"offset": 108, "type": "u32", "verified": True},
    "SpeedRate": {"offset": 112, "type": "u16", "verified": True},
    "RangeSize": {"offset": 114, "type": "u32", "verified": True},
    "AttackSpeed": {"offset": 118, "type": "f32", "verified": True},
    "ExtraAttackType": {"offset": 122, "type": "u8", "verified": True},
    "BulletRemain": {"offset": 124, "type": "u16", "verified": True},
    "ReloadTime": {"offset": 126, "type": "u16", "verified": True},
    "PushDist": {"offset": 128, "type": "u16", "verified": True},
    "CoolTime": {"offset": 130, "type": "u16", "verified": True},
    "SpecialFeature": {"offset": 132, "type": "u32", "verified": True},
    "PushType": {"offset": 136, "type": "u8", "verified": True},
    "StunBase": {"offset": 137, "type": "u16", "verified": True},
    "StunFinal": {"offset": 139, "type": "u16", "verified": True},
    "Pierce": {"offset": 141, "type": "u8", "verified": True},
    "PierceRate": {"offset": 142, "type": "u8", "verified": True},
    "MovingShot": {"offset": 143, "type": "u8", "verified": True},
    "MaxCombo": {"offset": 612, "type": "u8", "verified": True},
    "FunnelID": {"offset": 1565, "type": "u32", "verified": True},
}


def _identity(raw: bytes, label: str) -> int:
    value = struct.unpack_from("<I", raw, 0)[0]
    if not 1 <= value <= MAX_NATIVE_ID:
        raise ValueError(f"{label}: invalid native id {value}")
    return value


def _utf16(raw: bytes, offset: int, length: int, label: str, *, required: bool = False) -> str:
    field = raw[offset : offset + length]
    if len(field) != length or length % 2:
        raise ValueError(f"{label}: record is truncated")
    end = length
    terminated = False
    for index in range(0, length, 2):
        if field[index : index + 2] == b"\0\0":
            end = index
            terminated = True
            break
    if not terminated:
        raise ValueError(f"{label}: fixed-width string is not NUL terminated")
    try:
        value = field[:end].decode("utf-16-le", errors="strict")
    except UnicodeDecodeError:
        raise ValueError(f"{label}: invalid UTF-16LE") from None
    if required and not value.strip():
        raise ValueError(f"{label}: value is empty")
    if any(ord(character) < 32 and character not in "\t\r\n" for character in value):
        raise ValueError(f"{label}: contains control characters")
    return value


def _virtual_path(value: str, label: str) -> str:
    if not value:
        return ""
    normalized = value.replace("\\", "/").lower()
    if (
        len(normalized) > 240
        or not _VIRTUAL_PATH_RE.fullmatch(normalized)
        or any(part in ("", ".", "..") for part in normalized.split("/"))
    ):
        raise ValueError(f"{label}: invalid resource path")
    return normalized


def _visible_strings(raw: bytes) -> set[str]:
    """Extract bounded ASCII and ASCII-in-UTF16 strings from opaque resources."""
    values: set[str] = set()
    for match in re.finditer(rb"[\x20-\x7e]{4,512}", raw):
        values.add(match.group().decode("ascii"))
    for match in re.finditer(rb"(?:(?:[\x20-\x7e])\x00){4,512}", raw):
        values.add(match.group()[::2].decode("ascii"))
    return values


def scan_resource_references(raw: bytes, extension: str = "") -> list[str]:
    """Return visible direct resource references in an opaque record/resource.

    This is deliberately a conservative string scan.  It does not prove
    semantic validity or a transitive dependency closure.
    """
    if not isinstance(raw, bytes):
        raise TypeError("raw must be bytes")
    if len(raw) > 128 * 1024 * 1024:
        raise ValueError("resource exceeds scan limit")
    if extension and (not extension.startswith(".") or not extension[1:].isalnum()):
        raise ValueError("invalid resource extension")
    references: set[str] = set()
    for text in _visible_strings(raw):
        for match in _RESOURCE_PATH_RE.finditer(text):
            references.add(match.group(1).replace("\\", "/").lower())
        # The supplied ZFX files demonstrate bare texture references.  Mapping
        # other bare names without format evidence would create false paths.
        for match in _BARE_TEXTURE_RE.finditer(text):
            before = text[max(0, match.start(1) - 1) : match.start(1)]
            if before not in ("/", "\\"):
                references.add("txrs/" + match.group(1).lower())
    return sorted(references)


def _validate_common(raw: bytes, size: int, label: str) -> int:
    if not isinstance(raw, bytes) or len(raw) != size:
        actual = len(raw) if isinstance(raw, (bytes, bytearray)) else "non-bytes"
        raise ValueError(f"{label}: expected {size} bytes, got {actual}")
    identity = _identity(raw, label)
    if raw[4] != 0:
        raise ValueError(f"{label} {identity}: security/release byte must be zero")
    return identity


def model_override(raw: bytes) -> str:
    """The target model constructor prefers this optional 30-wide path.

    Numeric/opaque bytes from another layout are not a target model filename.
    Never erase them here: their source intent must be resolved by the author.
    """
    identity=_validate_common(raw,UNIT_SIZE,'unit record')
    label=f'unit {identity} model override @738'
    value=_virtual_path(_utf16(raw,738,60,label),label)
    if value and (not value.startswith('mdrs/') or not value.endswith('.mod')):
        raise ValueError(label+': expected an empty field or mdrs/*.mod path')
    return value


def decode_unit(raw: bytes) -> dict[str, Any]:
    identity = _validate_common(raw, UNIT_SIZE, "unit record")
    name = _utf16(raw, 5, 100, f"unit {identity} name", required=True)
    model = _virtual_path(_utf16(raw, 612, 60, f"unit {identity} model"), f"unit {identity} model")
    override=model_override(raw)
    portraits = {
        key: _virtual_path(
            _utf16(raw, int(UNIT_FIELDS[key]["offset"]), 100, f"unit {identity} {key}"),
            f"unit {identity} {key}",
        )
        for key in ("portrait_shop", "portrait_capsule", "portrait_detail")
    }
    client: dict[str, Any] = {
        "UnitNumber": identity,
        "name": name,
        "UnitGradeNumber": raw[308],
        "UnitGradeNumber2": raw[309],
        "MasteredWeaponType": raw[310],
        "BattleCost": struct.unpack_from("<H", raw, 311)[0],
        "Star": raw[313],
        "UnitNumber_Transform": struct.unpack_from("<I", raw, 316)[0],
        "HP": struct.unpack_from("<I", raw, 321)[0],
        "Shield": struct.unpack_from("<I", raw, 325)[0],
        "ShieldType": raw[329],
        "ShieldRate": struct.unpack_from("<I", raw, 330)[0],
        "MoveLevel": raw[540],
        "DashType": raw[541],
        "DashLevel": raw[542],
        "QuickStepLevel": raw[543],
        "MotivePower": raw[545],
        "BoosterIncrease": struct.unpack_from("<H", raw, 546)[0],
        "BoosterDecrease": struct.unpack_from("<H", raw, 548)[0],
        "StrikingPower": struct.unpack_from("<H", raw, 556)[0],
        "DefensivePower": struct.unpack_from("<H", raw, 558)[0],
        "AdaptEnvironType": raw[562],
        "AdaptEnvironSpace": struct.unpack_from("<h", raw, 563)[0],
        "AdaptEnvironGround": struct.unpack_from("<h", raw, 565)[0],
        "AdaptEnvironUnderwater": struct.unpack_from("<h", raw, 567)[0],
        "AdaptEnvironDesert": struct.unpack_from("<h", raw, 569)[0],
        "AdaptEnvironSky": struct.unpack_from("<h", raw, 571)[0],
        "Sight": struct.unpack_from("<H", raw, 573)[0],
        "SkillNumber_1": struct.unpack_from("<I", raw, 575)[0],
        "SkillNumber_2": struct.unpack_from("<I", raw, 579)[0],
        "MasteredWeaponIX": raw[584],
        "WeaponNumber_1": struct.unpack_from("<I", raw, 585)[0],
        "WeaponNumber_2": struct.unpack_from("<I", raw, 589)[0],
        "WeaponNumber_3": struct.unpack_from("<I", raw, 593)[0],
        "WeaponNumber_FatalAttack": struct.unpack_from("<I", raw, 597)[0],
        # Server UnitModelName is the catalog/display definition name.  The
        # client field at 612 is a model resource path and must stay distinct.
        "UnitModelName": name,
        "model_path": model,
        **portraits,
    }
    weapons = [
        client["WeaponNumber_1"],
        client["WeaponNumber_2"],
        client["WeaponNumber_3"],
        client["WeaponNumber_FatalAttack"],
    ]
    references = set(scan_resource_references(raw, ".bin"))
    references.update(path for path in (model, override, *portraits.values()) if path)
    client['model_override']=override
    return {
        "kind": "unit",
        "id": identity,
        "name": name,
        "client": client,
        "raw": raw,
        "model": model,
        "model_override": override,
        "transform_target": client["UnitNumber_Transform"],
        "weapons": weapons,
        "resource_refs": sorted(references),
    }


def decode_weapon(raw: bytes) -> dict[str, Any]:
    identity = _validate_common(raw, WEAPON_SIZE, "weapon record")
    name = _utf16(raw, 5, 100, f"weapon {identity} name", required=True)
    attack_speed = struct.unpack_from("<f", raw, 118)[0]
    if not math.isfinite(attack_speed) or attack_speed < 0:
        raise ValueError(f"weapon {identity}: invalid AttackSpeed")
    client = {
        "WeaponNo": identity,
        "WeaponName": name,
        "AttackStyle": raw[105],
        "RangeType": raw[106],
        "HitType": raw[107],
        "DamageSize": struct.unpack_from("<I", raw, 108)[0],
        "SpeedRate": struct.unpack_from("<H", raw, 112)[0],
        "RangeSize": struct.unpack_from("<I", raw, 114)[0],
        "AttackSpeed": attack_speed,
        "ExtraAttackType": raw[122],
        "BulletRemain": struct.unpack_from("<H", raw, 124)[0],
        "ReloadTime": struct.unpack_from("<H", raw, 126)[0],
        "PushDist": struct.unpack_from("<H", raw, 128)[0],
        "CoolTime": struct.unpack_from("<H", raw, 130)[0],
        "SpecialFeature": struct.unpack_from("<I", raw, 132)[0],
        "PushType": raw[136],
        "StunBase": struct.unpack_from("<H", raw, 137)[0],
        "StunFinal": struct.unpack_from("<H", raw, 139)[0],
        "Pierce": raw[141],
        "PierceRate": raw[142],
        "MovingShot": raw[143],
        "MaxCombo": raw[612],
        "FunnelID": struct.unpack_from("<I", raw, 1565)[0],
    }
    return {
        "kind": "weapon",
        "id": identity,
        "name": name,
        "client": client,
        "raw": raw,
        "resource_refs": scan_resource_references(raw, ".bin"),
    }


def decode_record(raw: bytes) -> dict[str, Any]:
    """Decode a single raw client row by verified byte size and internal ID."""
    if not isinstance(raw, bytes):
        raise ValueError("record payload must be bytes")
    if len(raw) == UNIT_SIZE:
        return decode_unit(raw)
    if len(raw) == WEAPON_SIZE:
        return decode_weapon(raw)
    if len(raw)==1197:
        from .blueprint_codec import decode
        decoded=decode(raw)
        return {'kind':'blueprint','id':decoded['id'],'name':decoded['name'],'raw':raw,
                'resource_refs':decoded['resource_refs'],'blueprint':decoded}
    raise ValueError(f"unknown client BIN layout ({len(raw)} bytes)")


def _put_utf16(output: bytearray, offset: int, length: int, value: Any, label: str) -> None:
    if not isinstance(value, str):
        raise ValueError(f"{label}: expected text")
    encoded = value.encode("utf-16-le")
    if len(encoded) >= length:
        raise ValueError(f"{label}: exceeds the native field")
    output[offset : offset + length] = encoded.ljust(length, b"\0")


def _put_number(output: bytearray, field: str, value: Any, schema: dict[str, dict[str, Any]]) -> None:
    description = schema[field]
    kind = description["type"]
    offset = int(description["offset"])
    if kind == "f32":
        if type(value) not in (int, float):
            raise ValueError(f"{field}: expected a finite number")
        numeric = float(value)
        if not math.isfinite(numeric) or not (-3.4028235e38 <= numeric <= 3.4028235e38):
            raise ValueError(f"{field}: float32 is out of range")
        struct.pack_into("<f", output, offset, numeric)
        return
    limits = {"u8": (0, 0xFF), "u16": (0, 0xFFFF), "u32": (0, 0xFFFFFFFF), "i16": (-0x8000, 0x7FFF)}
    if kind not in limits or type(value) is not int or not limits[kind][0] <= value <= limits[kind][1]:
        raise ValueError(f"{field}: invalid {kind} value")
    struct.pack_into({"u8": "<B", "u16": "<H", "u32": "<I", "i16": "<h"}[kind], output, offset, value)


def patch_unit(raw: bytes, fields: dict[str, Any]) -> bytes:
    """Patch only client unit fields whose offsets are verified.

    Unknown server-only fields fail closed instead of silently creating a
    client/server combat mismatch.
    """
    decode_unit(raw)
    if not isinstance(fields, dict):
        raise ValueError("unit patch fields must be an object")
    aliases = {"UnitModelName": "name"}
    allowed = {
        "UnitNumber",
        "name",
        "model_path",
        "UnitGradeNumber",
        "UnitGradeNumber2",
        "MasteredWeaponType",
        "BattleCost",
        "Star",
        "UnitNumber_Transform",
        "HP",
        "Shield",
        "ShieldType",
        "ShieldRate",
        "MoveLevel",
        "DashType",
        "DashLevel",
        "QuickStepLevel",
        "MotivePower",
        "BoosterIncrease",
        "BoosterDecrease",
        "StrikingPower",
        "DefensivePower",
        "AdaptEnvironType",
        "AdaptEnvironSpace",
        "AdaptEnvironGround",
        "AdaptEnvironUnderwater",
        "AdaptEnvironDesert",
        "AdaptEnvironSky",
        "Sight",
        "SkillNumber_1",
        "SkillNumber_2",
        "MasteredWeaponIX",
        "WeaponNumber_1",
        "WeaponNumber_2",
        "WeaponNumber_3",
        "WeaponNumber_FatalAttack",
        "portrait_shop",
        "portrait_capsule",
        "portrait_detail",
    }
    normalized = {aliases.get(key, key): value for key, value in fields.items()}
    unknown = set(normalized) - allowed
    if unknown:
        raise ValueError("unit fields lack a verified client offset: " + ", ".join(sorted(unknown)))
    output = bytearray(raw)
    for field, value in normalized.items():
        if field == "name":
            _put_utf16(output, 5, 100, value, "unit name")
        elif field == "model_path":
            path = _virtual_path(value, "unit model").replace("/", "\\")
            _put_utf16(output, 612, 60, path, "unit model")
        elif field.startswith("portrait_"):
            path = _virtual_path(value, field).replace("/", "\\") if value else ""
            _put_utf16(output, int(UNIT_FIELDS[field]["offset"]), 100, path, field)
        else:
            _put_number(output, field, value, UNIT_FIELDS)
    return bytes(output)


def patch_weapon(raw: bytes, fields: dict[str, Any]) -> bytes:
    """Patch the 23 server-shared weapon fields verified in the client row."""
    decode_weapon(raw)
    if not isinstance(fields, dict):
        raise ValueError("weapon patch fields must be an object")
    allowed = set(WEAPON_FIELDS) - {"security"}
    unknown = set(fields) - allowed
    if unknown:
        raise ValueError("weapon fields lack a verified client offset: " + ", ".join(sorted(unknown)))
    output = bytearray(raw)
    for field, value in fields.items():
        if field == "WeaponName":
            _put_utf16(output, 5, 100, value, "weapon name")
        else:
            _put_number(output, field, value, WEAPON_FIELDS)
    return bytes(output)
