"""Sealed configuration for one original ordinary-shop operator flow."""
from __future__ import annotations

import hashlib
import ipaddress
import json
from pathlib import Path
import re


SHOP_INPUT = "inputs/operator-shop.ini"
DISPLAY_INPUT = "inputs/operator-shop-display.ini"
SHOP_RELATIVE = Path("sdgo/gametest") / SHOP_INPUT
DISPLAY_RELATIVE = Path("sdgo/gametest") / DISPLAY_INPUT
CASE = "setup.operator_flow"

VALUE_FIELDS = (
    "operator_id",
    "product_id",
    "template_id",
    "option_id",
    "item_id",
    "category_id",
    "price",
    "validity",
    "payment",
    "name",
    "label",
    "nickname",
    "purchase",
)
RUN_VALUE_FIELDS = tuple("operator_flow_" + field for field in VALUE_FIELDS)
PATH_FIELDS = (
    "operator_shop_path",
    "operator_shop_sha256",
    "operator_shop_display_path",
    "operator_shop_display_sha256",
)
RUN_FIELDS = RUN_VALUE_FIELDS + PATH_FIELDS
REQUIRED_VALUE_FIELDS = set(VALUE_FIELDS) - {"purchase"}


def load(path):
    lexical = Path(path).expanduser()
    if lexical.is_symlink():
        raise ValueError("operator flow JSON is missing, linked, empty, or exceeds 64 KiB")
    path = lexical.resolve()
    if not path.is_file() or not 0 < path.stat().st_size <= 64 * 1024:
        raise ValueError("operator flow JSON is missing, linked, empty, or exceeds 64 KiB")

    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("operator flow JSON contains duplicate fields")
            result[key] = value
        return result

    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=object_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid operator flow JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("operator flow JSON must contain one object")
    return value


def _integer(value, label, minimum=1, maximum=2**31 - 1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{label} must be an integer from {minimum} through {maximum}")
    return value


def _display_text(value, label):
    if type(value) is not str or not value or any(character in "\r\n\0" for character in value):
        raise ValueError(f"{label} must be nonempty bounded display text")
    try:
        encoded = value.encode("utf-16-le")
    except UnicodeEncodeError as error:
        raise ValueError(f"{label} must be valid bounded display text") from error
    if len(encoded) > 100:
        raise ValueError(f"{label} must be nonempty bounded display text")
    return value


def _item_id(value):
    if type(value) is not str or re.fullmatch(r"[0-9]{6}", value) is None:
        raise ValueError("item_id must be exactly six decimal digits")
    return value


def _validate_scope(plan):
    if "unit_scenario" in plan:
        raise ValueError("operator flow cannot be combined with a native unit scenario")
    if any(key.startswith("shop_") for key in plan):
        raise ValueError("operator flow cannot be combined with StockAssertions")
    forbidden = (
        "fps_cap",
        "mission_mode",
        "bootstrap_only",
        "battleship_only",
        "gift_requests_only",
        "deck_only",
        "friends_only",
        "room_titles_only",
        "room_style_preview",
        "collection_draw",
        "page_group",
        "random_loadout",
        "same_room_rematch",
        "ai_pool_level",
        "ai_difficulty",
        "ai_layout",
    )
    if any(plan.get(field) is not None and plan.get(field) is not False for field in forbidden):
        raise ValueError("operator flow requires read-only pages or an ordinary PvP scenario")
    if plan.get("retry_count") != 1 or plan.get("expect_first_fail") is not False:
        raise ValueError("operator flow requires one login attempt with a reused account")
    if plan.get("network_policy") not in ("direct", "routed_loopback"):
        raise ValueError("operator flow requires a sealed loopback network policy")
    try:
        target = ipaddress.IPv4Address(plan.get("target_host", ""))
    except ipaddress.AddressValueError as error:
        raise ValueError("operator flow requires an explicit IPv4 loopback endpoint") from error
    if not target.is_loopback:
        raise ValueError("operator flow requires an IPv4 127/8 endpoint")


def validate(value, plan):
    if not isinstance(value, dict):
        raise ValueError("operator_flow must be a JSON object")
    unknown = set(value) - set(VALUE_FIELDS)
    missing = REQUIRED_VALUE_FIELDS - set(value)
    if unknown or missing:
        raise ValueError("operator_flow must contain exactly the sealed operator purchase fields")
    _validate_scope(plan)
    result = {
        "operator_id": _integer(value["operator_id"], "operator_id", 60047, 60999),
        "product_id": _integer(value["product_id"], "product_id"),
        "template_id": _integer(value["template_id"], "template_id"),
        "option_id": _integer(value["option_id"], "option_id"),
        "item_id": _item_id(value["item_id"]),
        "category_id": _integer(value["category_id"], "category_id"),
        "price": _integer(value["price"], "price"),
        "validity": _integer(value["validity"], "validity", 1, 1_000_000),
        "payment": _integer(value["payment"], "payment", 1, 2),
        "name": _display_text(value["name"], "name"),
        "label": _display_text(value["label"], "label"),
    }
    if result["category_id"] != 1219000 + result["operator_id"] - 60000:
        raise ValueError("category_id does not match the imported operator identity")
    if result["product_id"] == result["template_id"]:
        raise ValueError("operator owner product_id must differ from template_id")
    nickname = value["nickname"]
    if type(nickname) is not str or re.fullmatch(r"[A-Za-z0-9_]{1,16}", nickname) is None:
        raise ValueError("nickname must contain one to sixteen ASCII letters, digits, or underscore")
    result["nickname"] = nickname
    purchase = value.get("purchase", False)
    if type(purchase) is not bool:
        raise ValueError("purchase must be a JSON boolean")
    result["purchase"] = purchase
    return result


def shop_bytes(value):
    currency = "GB" if value["payment"] == 1 else "MB"
    text = (
        "[shop]\n"
        "Schema=1\n"
        "Scope=OwnerCatalog\n"
        "CatalogId=operator-flow\n"
        "Revision=operator-flow-v1\n"
        "\n"
        f"[product:operator-{value['operator_id']}]\n"
        f"ProductId={value['product_id']}\n"
        f"ItemId={value['item_id']}\n"
        f"TemplateId={value['template_id']}\n"
        f"Category={value['category_id']}\n"
        f"Price={value['price']}\n"
        f"Currency={currency}\n"
        f"Validity={value['validity']}\n"
        "Enabled=1\n"
        f"Name={value['name']}\n"
        f"Label={value['label']}\n"
    )
    return text.encode("utf-8")


def display_bytes(value):
    text = (
        "[shop]\n"
        "Revision=operator-flow-v1\n"
        "\n"
        f"[option:{value['option_id']}]\n"
        f"Category={value['category_id']}\n"
        f"Validity={value['validity']}\n"
        f"Payment={value['payment']}\n"
        f"Price={value['price']}\n"
        f"Text={value['label']}\n"
    )
    return text.encode("utf-8")


def bind(value, plan):
    value = validate(value, plan)
    shop = shop_bytes(value)
    display = display_bytes(value)
    return {
        "operator_flow": value,
        "operator_shop_path": SHOP_INPUT,
        "operator_shop_sha256": hashlib.sha256(shop).hexdigest(),
        "operator_shop_display_path": DISPLAY_INPUT,
        "operator_shop_display_sha256": hashlib.sha256(display).hexdigest(),
    }


def run_fields(plan):
    value = plan["operator_flow"]
    result = {
        "operator_flow_" + field: (
            "1" if value[field] is True else "0" if value[field] is False else str(value[field])
        )
        for field in VALUE_FIELDS
    }
    result.update({field: str(plan[field]) for field in PATH_FIELDS})
    return result


def cases(base):
    base = tuple(base)
    try:
        index = base.index("flow.onboarding") + 1
    except ValueError as error:
        raise ValueError("operator flow requires the normal login and onboarding cases") from error
    return base[:index] + (CASE,) + base[index:]


def install(root, binding, atomic_write):
    root = Path(root).resolve()
    value = binding["operator_flow"]
    canonical = bind(value, binding)
    for field, expected in canonical.items():
        if type(binding.get(field)) is not type(expected) or binding.get(field) != expected:
            raise ValueError(f"operator flow plan disagrees with its canonical {field}")
    targets = (
        ((root / SHOP_RELATIVE).resolve(), shop_bytes(value)),
        ((root / DISPLAY_RELATIVE).resolve(), display_bytes(value)),
    )
    for target, payload in targets:
        if not target.is_relative_to(root):
            raise ValueError("operator shop input path escapes the copied client")
        if target.exists():
            raise ValueError("fresh copied client already contains operator shop inputs")
        target.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(target, payload.decode("utf-8"))
    return canonical


def _canonical_decimal(raw, label, minimum=1, maximum=2**31 - 1):
    if re.fullmatch(r"[1-9][0-9]*", raw or "") is None:
        raise ValueError(f"{label} must be canonical positive decimal")
    value = int(raw)
    return _integer(value, label, minimum, maximum)


def read(root, run, plan):
    present = {field for field in RUN_FIELDS if field in run}
    unknown = {field for field in run if field.startswith("operator_")} - set(RUN_FIELDS)
    if unknown:
        raise ValueError("unknown operator flow run.ini fields are forbidden")
    if not present:
        return {}
    if present != set(RUN_FIELDS):
        raise ValueError("operator flow run.ini must contain every sealed field")
    raw = {
        field: run["operator_flow_" + field]
        for field in VALUE_FIELDS
    }
    for field, minimum, maximum in (
        ("operator_id", 60047, 60999),
        ("product_id", 1, 2**31 - 1),
        ("template_id", 1, 2**31 - 1),
        ("option_id", 1, 2**31 - 1),
        ("category_id", 1, 2**31 - 1),
        ("price", 1, 2**31 - 1),
        ("validity", 1, 1_000_000),
        ("payment", 1, 2),
    ):
        raw[field] = _canonical_decimal(raw[field], field, minimum, maximum)
    raw["item_id"] = _item_id(raw["item_id"])
    if raw["purchase"] not in ("0", "1"):
        raise ValueError("operator flow purchase must be 0 or 1")
    raw["purchase"] = raw["purchase"] == "1"
    value = validate(raw, plan)
    if run["operator_shop_path"] != SHOP_INPUT or run["operator_shop_display_path"] != DISPLAY_INPUT:
        raise ValueError("operator shop inputs must use their fixed copied-client paths")
    for field in ("operator_shop_sha256", "operator_shop_display_sha256"):
        if re.fullmatch(r"[0-9a-f]{64}", run[field]) is None:
            raise ValueError("invalid operator shop input digest")
    root = Path(root).resolve()
    expected = (
        (root / SHOP_RELATIVE, shop_bytes(value), run["operator_shop_sha256"]),
        (root / DISPLAY_RELATIVE, display_bytes(value), run["operator_shop_display_sha256"]),
    )
    for lexical, canonical, digest in expected:
        if (
            lexical.is_symlink()
            or not lexical.is_file()
            or lexical.stat().st_nlink != 1
            or not lexical.resolve().is_relative_to(root)
            or lexical.stat().st_size > 64 * 1024
            or any(
                parent.is_symlink()
                or (hasattr(parent, "is_junction") and parent.is_junction())
                for parent in lexical.parents
                if parent != root and parent.is_relative_to(root)
            )
        ):
            raise ValueError("operator shop input is missing, linked, or escapes its copied client")
        raw_bytes = lexical.read_bytes()
        if hashlib.sha256(raw_bytes).hexdigest() != digest or raw_bytes != canonical:
            raise ValueError("operator shop input differs from its sealed canonical configuration")
    return {
        "operator_flow": value,
        **{field: run[field] for field in PATH_FIELDS},
    }
