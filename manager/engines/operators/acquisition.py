"""Applied operator blueprint acquisition sources for the optional client contributor."""
from __future__ import annotations

import copy
from pathlib import Path

from . import model

SCHEMA_VERSION = 1
PROVIDER = "operator_catalog"
SNAPSHOT_NAME = "applied-acquisition.json"


def _public(revision, blueprints):
    return {
        "schema_version": SCHEMA_VERSION,
        "provider": PROVIDER,
        "revision": revision,
        "blueprints": blueprints,
    }


def compile_sources(catalog):
    """Compile source claims from the exact catalog that was applied."""
    model.validate(catalog)
    items = {int(item["id"]): item for item in catalog["items"]}
    pools = catalog.get("drop_pools", {})
    claims = {}
    for operator in catalog["operators"]:
        if not operator["managed"] or not operator["enabled"]:
            continue
        for rule in operator["drops"]:
            if rule["chance_bps"] <= 0 or rule["max_amount"] <= 0:
                continue
            categories = [rule["category_id"]]
            if "pool" in rule:
                categories = pools[rule["pool"]]["category_ids"]
            for category_id in categories:
                item = items.get(int(category_id))
                if not item or int(category_id) // 100000 != 15:
                    continue
                if int(item.get("time_limit", -1)) != 0:
                    continue
                recipe_id = item.get("ref_index")
                if type(recipe_id) is not int or recipe_id <= 0:
                    continue
                by_operator = claims.setdefault(recipe_id, {})
                by_operator.setdefault(operator["id"], {
                    "key": f"operator:{operator['id']}:blueprint:{recipe_id}",
                    "text": "副官掉落：" + operator["name"],
                })
    return _public(model.revision(catalog), {
        str(recipe_id): [by_operator[key] for key in sorted(by_operator)]
        for recipe_id, by_operator in sorted(claims.items())
    })


def snapshot(catalog):
    value = compile_sources(catalog)
    value["catalog"] = copy.deepcopy(catalog)
    return value


def public_snapshot(value):
    if not isinstance(value, dict) or value.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("副官获得方式快照版本无效")
    if value.get("provider") != PROVIDER or not isinstance(value.get("catalog"), dict):
        raise ValueError("副官获得方式快照身份无效")
    model.validate(value["catalog"])
    expected = compile_sources(value["catalog"])
    if value.get("revision") != expected["revision"] or value.get("blueprints") != expected["blueprints"]:
        raise ValueError("副官获得方式快照与已应用配置不一致")
    return expected


def empty(revision=""):
    return _public(revision if isinstance(revision, str) else "", {})


def write(path, catalog):
    model.save(Path(path), snapshot(catalog))


def read(path):
    return public_snapshot(model.load(Path(path)))


def read_with_catalog(path):
    value = model.load(Path(path))
    public_snapshot(value)
    return value
