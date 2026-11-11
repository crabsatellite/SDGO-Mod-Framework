"""Deterministic client sidecars for optional acquisition-source providers."""
from __future__ import annotations

import re
import struct

SOURCE_MAGIC = b"SDGOAS1\0"
AVAILABILITY_MAGIC = b"SDGOBP1\0"
VERSION = 1
HEADER_BYTES = 32
BLUEPRINT_SUBJECT = 2
SHOP_PROVIDER = 1
OPERATOR_PROVIDER = 2
MAX_ROWS = 65535
MAX_PAYLOAD_BYTES = 32 * 1024 * 1024
_KEY = re.compile(r"[A-Za-z0-9:._-]{1,127}\Z")


def fnv1a32(raw):
    value = 2166136261
    for byte in raw:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def encode_sources(document, provider_kind):
    if provider_kind not in (SHOP_PROVIDER, OPERATOR_PROVIDER):
        raise ValueError("获得方式 provider 类型无效")
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise ValueError("获得方式文档版本无效")
    unique = {}
    for subject, field, label in (
            (1, "units", "机体"),
            (BLUEPRINT_SUBJECT, "blueprints", "设计图")):
        entries_by_id = document.get(field, {})
        if not isinstance(entries_by_id, dict):
            raise ValueError("获得方式文档的" + label + "来源无效")
        if provider_kind == OPERATOR_PROVIDER and subject != BLUEPRINT_SUBJECT and entries_by_id:
            raise ValueError("副官获得方式只能发布设计图来源")
        for raw_id, entries in entries_by_id.items():
            if not isinstance(raw_id, str) or not raw_id.isascii() or not raw_id.isdigit():
                raise ValueError("获得方式" + label + "编号无效")
            stable_id = int(raw_id)
            if not 0 < stable_id <= 0xffffffff or not isinstance(entries, list):
                raise ValueError("获得方式" + label + "条目无效")
            for entry in entries:
                if not isinstance(entry, dict) or set(entry) != {"key", "text"}:
                    raise ValueError("获得方式来源条目无效")
                key, text = entry["key"], entry["text"]
                if not isinstance(key, str) or not _KEY.fullmatch(key):
                    raise ValueError("获得方式稳定键无效")
                if not isinstance(text, str) or not text or "\0" in text:
                    raise ValueError("获得方式文字无效")
                try:
                    encoded_text = text.encode("utf-16-le")
                    encoded_key = key.encode("ascii")
                except UnicodeEncodeError:
                    raise ValueError("获得方式文字编码无效") from None
                units = len(encoded_text) // 2
                if not 1 <= units <= 2048:
                    raise ValueError("获得方式文字超过容量")
                identity = (subject, stable_id, key)
                row = (encoded_key, encoded_text, units)
                if identity in unique and unique[identity] != row:
                    raise ValueError("获得方式稳定键存在冲突")
                unique[identity] = row
    if len(unique) > MAX_ROWS:
        raise ValueError("获得方式来源数量超过容量")
    parts = []
    for (subject, recipe_id, _), (key, text, units) in sorted(unique.items()):
        parts.append(struct.pack("<4I", subject, recipe_id, len(key), units) + key + text)
    payload = b"".join(parts)
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise ValueError("获得方式来源内容超过容量")
    return struct.pack(
        "<8s6I", SOURCE_MAGIC, VERSION, HEADER_BYTES, len(parts), len(payload),
        fnv1a32(payload), provider_kind,
    ) + payload


def encode_availability(document, catalog_payload_fnv):
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise ValueError("设计图可用性文档版本无效")
    if type(catalog_payload_fnv) is not int or not 0 <= catalog_payload_fnv <= 0xffffffff:
        raise ValueError("机体目录 payload 校验值无效")
    blueprints = document.get("blueprints")
    if not isinstance(blueprints, dict):
        raise ValueError("设计图可用性文档缺少条目")
    rows = []
    for raw_id, value in blueprints.items():
        if not isinstance(raw_id, str) or not raw_id.isascii() or not raw_id.isdigit() or not isinstance(value, dict):
            raise ValueError("设计图可用性条目无效")
        recipe_id = int(raw_id)
        unit_id = value.get("unit_id")
        state = value.get("state")
        if not 0 < recipe_id <= 0xffffffff or type(unit_id) is not int or not 0 < unit_id <= 0xffffffff:
            raise ValueError("设计图可用性编号无效")
        if state not in (1, 2):
            raise ValueError("设计图可用性状态无效")
        rows.append((recipe_id, unit_id, 1 if state == 1 else 0))
    rows.sort()
    if len(rows) > MAX_ROWS or len({row[0] for row in rows}) != len(rows):
        raise ValueError("设计图可用性条目重复或超过容量")
    payload = b"".join(struct.pack("<3I", *row) for row in rows)
    return struct.pack(
        "<8s6I", AVAILABILITY_MAGIC, VERSION, HEADER_BYTES, len(rows), len(payload),
        fnv1a32(payload), catalog_payload_fnv,
    ) + payload


def _positive_native_id(value, label):
    if type(value) is not int or not 0 < value <= 0xffffffff:
        raise ValueError(label + "编号无效")
    return value


def _verified_catalog_records(raw):
    """Return canonical owned catalog records and its exact payload FNV."""
    if not isinstance(raw, (bytes, bytearray, memoryview)):
        raise ValueError("机体目录不是二进制数据")
    raw = bytes(raw)
    if len(raw) < HEADER_BYTES:
        raise ValueError("机体目录容器截断")
    magic, version, header, count, payload_bytes, payload_fnv, reserved = struct.unpack_from(
        "<8s6I", raw)
    if (magic != b"SDGOUN1\0" or version != 1 or header != HEADER_BYTES or
            reserved != 0):
        raise ValueError("机体目录容器版本无效")
    # Keep this parser bound to the same limits as unit_catalog_format.h and
    # compiler.native_catalog before walking attacker-controlled extents.
    if count > 8192 or payload_bytes > 64 * 1024 * 1024:
        raise ValueError("机体目录超过容量")
    if len(raw) != HEADER_BYTES + payload_bytes:
        raise ValueError("机体目录 payload 长度不匹配")
    if fnv1a32(raw[HEADER_BYTES:]) != payload_fnv:
        raise ValueError("机体目录 payload 校验失败")
    cursor = HEADER_BYTES
    records = []
    for _ in range(count):
        if cursor + 24 > len(raw):
            raise ValueError("机体目录记录头截断")
        kind, identity, extent, digest, flags_a, flags_b = struct.unpack_from(
            "<6I", raw, cursor)
        cursor += 24
        if cursor + extent > len(raw):
            raise ValueError("机体目录记录截断")
        record = raw[cursor:cursor + extent]
        cursor += extent
        if fnv1a32(record) != digest or flags_a or flags_b:
            raise ValueError("机体目录记录校验失败")
        records.append((kind, identity, record))
    if cursor != len(raw):
        raise ValueError("机体目录存在尾随数据")
    # Rebuilding verifies supported kinds and sizes, identities, publication
    # bytes, canonical order, uniqueness, and the complete blueprint codec.
    from .compiler import native_catalog
    if native_catalog(records) != raw:
        raise ValueError("机体目录不是规范的 owner 发布目录")
    return records, payload_fnv


def compile_blueprint_availability(definitions, catalog_raw):
    """Derive a catalog-bound availability sidecar from accepted publication bytes.

    This is a pure migration helper: it neither rebuilds authored assets nor
    writes the accepted installation. The definitions and catalog must describe
    the same complete owner publication.
    """
    if (not isinstance(definitions, dict) or
            definitions.get("protocol") != "sdgo.unit-family" or
            definitions.get("schema_version") != 1):
        raise ValueError("机体目录定义协议无效")
    revision = definitions.get("revision")
    if not isinstance(revision, str) or not revision or len(revision) > 256:
        raise ValueError("机体目录定义 revision 无效")
    families = definitions.get("families")
    registrations = definitions.get("registrations")
    if not isinstance(families, list) or not isinstance(registrations, list):
        raise ValueError("机体目录定义缺少完整发布清单")

    records, payload_fnv = _verified_catalog_records(catalog_raw)
    by_kind = {}
    for kind, identity, raw in records:
        by_kind.setdefault(kind, {})[identity] = raw

    family_by_key = {}
    family_recipes = {}
    for family in families:
        if not isinstance(family, dict):
            raise ValueError("机体 family 定义无效")
        key = family.get("family_key")
        root = _positive_native_id(family.get("root_unit_id"), "机体 family")
        if not isinstance(key, str) or not key or key in family_by_key:
            raise ValueError("机体 family key 无效或重复")
        recipes = family.get("blueprints", [])
        if not isinstance(recipes, list):
            raise ValueError("机体 family 设计图清单无效")
        parsed = {}
        for recipe in recipes:
            if not isinstance(recipe, dict):
                raise ValueError("机体 family 设计图定义无效")
            recipe_id = _positive_native_id(recipe.get("id"), "设计图")
            item_id = _positive_native_id(recipe.get("item_category_id"), "设计图物品")
            enabled = recipe.get("enabled")
            if type(enabled) is not bool or recipe_id in parsed:
                raise ValueError("机体 family 设计图启用状态无效或编号重复")
            parsed[recipe_id] = (item_id, enabled)
        family_by_key[key] = (root, family)
        family_recipes[key] = parsed

    registration_by_key = {}
    registration_indexes = {}
    all_recipes = {}
    for registration in registrations:
        if not isinstance(registration, dict):
            raise ValueError("机体 registration 定义无效")
        if (registration.get("provider") != "unit_catalog" or
                registration.get("kind") != "unit"):
            raise ValueError("机体 registration 所有者无效")
        key = registration.get("family_key")
        root = _positive_native_id(registration.get("unit_id"), "机体 registration")
        index_id = _positive_native_id(
            registration.get("unit_index_id"), "机体图鉴 registration")
        family_enabled = registration.get("enabled")
        if (not isinstance(key, str) or key not in family_by_key or
                key in registration_by_key or type(family_enabled) is not bool):
            raise ValueError("机体 registration 覆盖或启用状态无效")
        if family_by_key[key][0] != root:
            raise ValueError("机体 family 与 registration 产物编号不一致")
        if index_id in registration_indexes:
            raise ValueError("机体图鉴 registration 编号重复")
        recipes = registration.get("blueprints")
        if not isinstance(recipes, list):
            raise ValueError("机体 registration 设计图清单无效")
        parsed = {}
        for recipe in recipes:
            if not isinstance(recipe, dict):
                raise ValueError("机体 registration 设计图定义无效")
            recipe_id = _positive_native_id(recipe.get("id"), "设计图")
            item_id = _positive_native_id(recipe.get("item_category_id"), "设计图物品")
            enabled = recipe.get("enabled")
            if type(enabled) is not bool or recipe_id in parsed:
                raise ValueError("机体 registration 设计图启用状态无效或编号重复")
            parsed[recipe_id] = (item_id, enabled)
        if parsed != family_recipes[key]:
            raise ValueError("机体 family 与 registration 设计图清单不一致")
        primary = registration.get("primary_blueprint_id")
        if type(primary) is not int or primary < 0 or primary > 0xffffffff:
            raise ValueError("机体 registration 主设计图编号无效")
        if primary and primary not in parsed:
            raise ValueError("机体 registration 主设计图未声明")
        registration_by_key[key] = registration
        registration_indexes[index_id] = (root, family_enabled, primary)
        for recipe_id, (item_id, recipe_enabled) in parsed.items():
            if recipe_id in all_recipes:
                raise ValueError("设计图编号跨 family 重复")
            all_recipes[recipe_id] = {
                "item_category_id": item_id,
                "unit_id": root,
                "enabled": family_enabled and recipe_enabled,
            }

    if set(registration_by_key) != set(family_by_key):
        raise ValueError("机体 family 与 registration 覆盖不完整")
    roots = {value[0] for value in family_by_key.values()}
    if not roots.issubset(by_kind.get(2, {})):
        raise ValueError("机体 root 原生记录与发布定义覆盖不一致")

    indexes = by_kind.get(85, {})
    if set(indexes) != set(registration_indexes):
        raise ValueError("机体图鉴原生记录与 registration 覆盖不一致")
    for index_id, (root, enabled, primary) in registration_indexes.items():
        raw = indexes[index_id]
        if (struct.unpack_from("<I", raw, 6)[0] != root or raw[5] not in (0, 1) or
                raw[5] != int(enabled) or struct.unpack_from("<I", raw, 10)[0] != primary):
            raise ValueError("机体图鉴原生记录与 registration 不一致")

    blueprint_records = by_kind.get(33, {})
    if set(blueprint_records) != set(all_recipes):
        raise ValueError("设计图原生记录与发布定义覆盖不一致")
    from .blueprint_codec import decode
    for recipe_id, value in all_recipes.items():
        decoded = decode(blueprint_records[recipe_id])
        if decoded["id"] != recipe_id or decoded["output_unit_id"] != value["unit_id"]:
            raise ValueError("设计图原生产物与发布定义不一致")

    document = {
        "schema_version": 1,
        "provider": "unit_catalog",
        "revision": revision,
        "blueprints": {
            str(recipe_id): {
                "state": 1 if value["enabled"] else 2,
                "item_category_id": value["item_category_id"],
                "unit_id": value["unit_id"],
            }
            for recipe_id, value in sorted(all_recipes.items())
        },
    }
    return document, encode_availability(document, payload_fnv)
