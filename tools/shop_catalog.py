"""Portable host shop configuration API/CLI. Runtime application is native C++.

No database writes are performed by this tool. Player clients do not need Python.
The native helper/DLL reads catalog.ini and applies owned definitions atomically.
"""
from __future__ import annotations

import argparse
import configparser
import copy
import datetime
import hashlib
import json
from itertools import chain
from pathlib import Path
import re

KEY = re.compile(r"[A-Za-z0-9_.-]{1,64}\Z")
CURRENCIES = {"GB": 1, "MB": 2}
MAX_CATALOG_BYTES = 32 * 1024 * 1024


def load(path):
    if Path(path).stat().st_size > MAX_CATALOG_BYTES:
        raise ValueError("Shop configuration exceeds the 32 MiB workspace budget")
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def save(path, value):
    path = Path(path)
    raw = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if len(raw) > MAX_CATALOG_BYTES:
        raise ValueError("Shop configuration exceeds the 32 MiB workspace budget")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        previous = path.read_bytes()
        if previous == raw:
            return
        backup = path.with_name(path.name + "." + hashlib.sha256(previous).hexdigest()[:12] + ".bak")
        if not backup.exists():
            backup.write_bytes(previous)
    temporary = path.with_name(path.name + ".pending")
    with temporary.open("xb") as f:
        f.write(raw)
    temporary.replace(path)


def integer(value, name, minimum=0, maximum=2147483647):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in {minimum}..{maximum}")


def text(value, name):
    if (not isinstance(value, str) or not value or len(value.encode("utf-16-le")) > 100
            or any(c in value for c in "\r\n\0")):
        raise ValueError(f"{name} must contain 1..50 UTF-16 units on one line")
    if value.startswith('"') or value.endswith('"'):
        raise ValueError(f"{name} must not start/end with INI quote delimiters")


def validate(catalog, snapshot=None):
    if catalog.get("schema_version") != 1 or not KEY.fullmatch(catalog.get("catalog_id", "")):
        raise ValueError("Invalid catalog schema or identity")
    if not isinstance(catalog.get("products"), list):
        raise ValueError("Invalid product list")
    ids, skus, keys = set(), set(), set()
    categories = {r['CatalogueID']: r for r in snapshot['categories']} if snapshot is not None else {}
    templates = {r['f_productid']: r for r in snapshot['products']} if snapshot is not None else {}
    kinds = {}
    if snapshot is not None:
        for row in snapshot['options']:
            kinds.setdefault(row['f_itemid'].strip(), set()).add(int(row['f_categoryNumber']) // 100000)
    for p in catalog["products"]:
        if not KEY.fullmatch(p.get("key", "")) or p["key"] in keys:
            raise ValueError("Invalid or duplicate product key")
        keys.add(p["key"])
        integer(p["product_id"], "product_id", 1)
        integer(p["template_id"], "template_id", 1)
        integer(p["category_id"], "category_id", 1)
        integer(p["price"], "price", 1)
        integer(p["validity"], "validity", 1, 1000000)
        if p["product_id"] == p["template_id"] or p["product_id"] in ids:
            raise ValueError("Product id collides with template or another product")
        ids.add(p["product_id"])
        if not re.fullmatch(r"[0-9]{6}", p.get("item_id", "")) or p["item_id"] in skus:
            raise ValueError("Invalid or duplicate six-digit native SKU")
        skus.add(p["item_id"])
        if p.get("currency") not in CURRENCIES:
            raise ValueError("Currency must be GB or MB")
        if type(p.get("enabled")) is not bool or type(p.get("retired", False)) is not bool:
            raise ValueError("Product enabled/retired must be booleans")
        if p.get("retired") and p["enabled"]:
            raise ValueError("A retired product cannot remain enabled")
        text(p["name"], "name")
        text(p["label"], "label")
        if not p.get("retired"):
            if snapshot is not None:
                category = categories.get(p['category_id'])
                template = templates.get(p['template_id'])
                if category is None or template is None or str(template.get("f_package", "0")) != "0":
                    raise ValueError("Unknown category or native single-item template")
                tab = int(template["f_depth1"])
                if tab == 20 or not (100 + tab) * 10000 < p["product_id"] < (101 + tab) * 10000:
                    raise ValueError("Product id is outside its native tab range")
                if p["item_id"][0] != template["f_itemid"].strip()[0]:
                    raise ValueError("SKU kind differs from template")
                if int(category['CatalogueID']) // 100000 not in kinds.get(template['f_itemid'].strip(), ()):
                    raise ValueError("Template and category have different native item kinds")
                if category["CatalogueID"] // 100000 == 11 and int(category["TimeLimit"]) == 0 and p["validity"] != 1:
                    raise ValueError("Permanent unit validity is one item, not a lease duration")
                if int(category["TimeLimit"]) == 1 and p["validity"] > 36500:
                    raise ValueError("Rental duration exceeds supported range")
    if len((json.dumps(catalog, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")) > MAX_CATALOG_BYTES:
        raise ValueError("Shop configuration exceeds the 32 MiB workspace budget")


def create(catalog, snapshot, *, key, category_id, price, currency, validity=1, name=None, label=None):
    result = copy.deepcopy(catalog)
    category = next((r for r in snapshot["categories"] if int(r["CatalogueID"]) == category_id), None)
    if category is None:
        raise ValueError("Unknown native item category")
    kind = category_id // 100000
    options = {r["f_itemid"].strip() for r in snapshot["options"] if int(r["f_categoryNumber"]) // 100000 == kind}
    templates = [r for r in snapshot["products"] if r["f_itemid"].strip() in options
                 and str(r.get("f_package", "0")) == "0" and int(r["f_depth1"]) != 20]
    if not templates:
        raise ValueError("No supported native single-item template for this category kind")
    template = min(templates, key=lambda r: (str(r["f_display"]) != "1", int(r["f_productid"])))
    used_products = ({int(r["f_productid"]) for r in snapshot["products"]}
                     | set(snapshot.get("reserved_product_ids", ())) | {p["product_id"] for p in result["products"]})
    used_skus = ({r["f_itemid"].strip() for r in snapshot["products"] + snapshot["options"]}
                 | set(snapshot.get("reserved_skus", ())) | {p["item_id"] for p in result["products"]})
    tab = int(template["f_depth1"])
    if not 0 <= tab < 100 or tab == 20:
        raise ValueError("Unsupported native shop tab")
    product_id = next(((100 + tab) * 10000 + i for i in chain(range(9001, 10000), range(1, 9001))
                       if (100 + tab) * 10000 + i not in used_products), None)
    prefix = template["f_itemid"].strip()[0]
    sku = next((prefix + f"{i:05d}" for i in chain(range(90001, 100000), range(1, 90001))
                if prefix + f"{i:05d}" not in used_skus), None)
    if product_id is None:
        raise ValueError("This native shop tab is full; other tabs remain available. Retired identities stay reserved")
    if sku is None:
        raise ValueError("This native SKU prefix is full; other prefixes remain available. Retired identities stay reserved")
    if label is None:
        if int(category["TimeLimit"]) == 1:
            label = f"租赁 {validity} 天" if kind == 11 else f"有效期 {validity} 天"
        elif kind == 11:
            label = "永久"
        else:
            label = f"数量 {validity}"
    result["products"].append({"key": key, "product_id": product_id, "item_id": sku,
        "template_id": int(template["f_productid"]), "category_id": category_id,
        "name": name or category["Name"].split("\0")[0], "label": label,
        "price": price, "currency": currency, "validity": validity, "enabled": True, "retired": False})
    validate(result, snapshot)
    return result


def update(catalog, key, changes):
    result = copy.deepcopy(catalog)
    product = next((p for p in result["products"] if p["key"] == key), None)
    if product is None:
        raise ValueError("Product not found")
    if set(changes) - {"category_id", "name", "label", "price", "currency", "validity", "enabled", "retired"}:
        raise ValueError("Product identity is immutable; use a new product key")
    product.update(changes)
    validate(result)
    return result


def retire(catalog, key):
    return update(catalog, key, {"enabled": False, "retired": True})


def compile_catalog(catalog, path, snapshot=None):
    validate(catalog, snapshot)
    revision = hashlib.sha256(json.dumps(catalog, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    lines = ["; Host-owned shop definitions. Contains no credentials.", "[shop]", "Schema=1",
             f"CatalogId={catalog['catalog_id']}", f"Revision={revision}", ""]
    fields = {"ProductId": "product_id", "ItemId": "item_id", "TemplateId": "template_id",
              "Category": "category_id", "Price": "price", "Currency": "currency",
              "Validity": "validity", "Name": "name", "Label": "label"}
    for product in catalog["products"]:
        if product.get("retired"):
            continue
        lines.append(f"[product:{product['key']}]")
        for target, source in fields.items():
            lines.append(f"{target}={product[source]}")
        lines.extend([f"Enabled={int(product['enabled'])}", ""])
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    data = ("\r\n".join(lines) + "\r\n").encode("utf-16")
    if len(data) > MAX_CATALOG_BYTES:
        raise ValueError("Compiled shop configuration exceeds the 32 MiB workspace budget")
    if out.exists() and out.read_bytes() == data:
        return revision
    pending = out.with_name(out.name + ".pending")
    with pending.open("xb") as f:
        f.write(data)
    pending.replace(out)
    return revision


def snapshot(server_ini, output, client=None):
    import pyodbc
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(server_ini, encoding="utf-8-sig")
    def escape(value):
        return "{" + str(value).replace("}", "}}") + "}"
    data = {"schema_version": 1, "exported_at": datetime.datetime.now(datetime.timezone.utc).isoformat()}
    for role, tables in [("DB Game", [("tbl_ItemCategory", "categories"), ("tbl_Units", "units")]),
                         ("DB StoreBilling", [("tbls_StoreItems", "products"), ("tbls_SubItems", "options")])]:
        db = cfg[role]
        connect = ("DRIVER={SQL Server};SERVER=" + escape(db["DBServerName"]) + ";DATABASE=" + escape(db["DBName"])
                   + ";UID=" + escape(db["ID"]) + ";PWD=" + escape(db["PWD"]) + ";")
        try:
            cn = pyodbc.connect(connect, timeout=5, autocommit=True)
        except pyodbc.Error as error:
            raise ValueError(f"Read-only snapshot connection failed, SQLSTATE={error.args[0]}") from None
        try:
            cn.timeout = 20
            cur = cn.cursor()
            for name, target in tables:
                cur.execute(f"SELECT * FROM dbo.[{name}]")
                keys = [c[0] for c in cur.description]
                data[target] = [dict(zip(keys, row)) for row in cur.fetchall()]
            if role == "DB StoreBilling":
                cur.execute("SELECT f_productid FROM dbo.tbls_StoreItemsPlus")
                data["reserved_product_ids"] = [int(row[0]) for row in cur.fetchall()]
                cur.execute("IF OBJECT_ID(N'dbo.sdgo_ShopCatalog',N'U') IS NOT NULL SELECT ProductId,ItemId FROM dbo.sdgo_ShopCatalog ELSE SELECT CAST(NULL AS int),CAST(NULL AS char(6)) WHERE 1=0")
                reserved = cur.fetchall()
                data["reserved_product_ids"] = sorted(set(data["reserved_product_ids"]) | {int(row[0]) for row in reserved})
                data["reserved_skus"] = sorted({str(row[1]).strip() for row in reserved})
        finally:
            cn.close()
    if client is not None:
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        from manager.shop import _native_product_ids
        data['reserved_product_ids'] = sorted(set(data['reserved_product_ids']) | set(_native_product_ids(Path(client))))
    # Store configuration only; no account, balance, inventory or history tables.
    data = json.loads(json.dumps(data, ensure_ascii=False, default=str))
    save(output, data)
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    cmd = sub.add_parser("snapshot"); cmd.add_argument("--server-ini", required=True); cmd.add_argument("--output", required=True);cmd.add_argument("--client", help="Matching destination client; reserves original product IDs absent from SQL")
    cmd = sub.add_parser("init"); cmd.add_argument("--id", required=True); cmd.add_argument("--file", required=True)
    cmd = sub.add_parser("items"); cmd.add_argument("--snapshot", required=True); cmd.add_argument("--search", default="")
    cmd = sub.add_parser("add")
    cmd.add_argument("--file", required=True); cmd.add_argument("--snapshot", required=True); cmd.add_argument("--key", required=True)
    cmd.add_argument("--category", type=int, required=True); cmd.add_argument("--price", type=int, required=True)
    cmd.add_argument("--currency", choices=CURRENCIES, required=True); cmd.add_argument("--validity", type=int, default=1)
    cmd.add_argument("--name"); cmd.add_argument("--label")
    for name in ["list", "show", "update", "delete", "validate", "compile"]:
        cmd = sub.add_parser(name); cmd.add_argument("--file", required=True)
        if name in ["show", "update", "delete"]: cmd.add_argument("--key", required=True)
        if name == "update": cmd.add_argument("--patch", required=True)
        if name in ["validate", "compile"]: cmd.add_argument("--snapshot")
        if name == "compile": cmd.add_argument("--output", required=True)
    args = p.parse_args()
    if args.command == "snapshot":
        result = snapshot(args.server_ini, args.output, args.client)
        print(json.dumps({k: len(result[k]) for k in ["products", "options", "categories", "units"]}))
        return
    if args.command == "init":
        if Path(args.file).exists(): raise ValueError("Catalog already exists")
        result = {"schema_version": 1, "catalog_id": args.id, "products": []}; validate(result); save(args.file, result); return
    if args.command == "items":
        rows = load(args.snapshot)["categories"]
        print(json.dumps([r for r in rows if args.search.lower() in (str(r["CatalogueID"]) + " " + r["Name"]).lower()], ensure_ascii=False, indent=2)); return
    catalog = load(args.file)
    validate(catalog)
    if args.command == "add":
        result = create(catalog, load(args.snapshot), key=args.key, category_id=args.category,
                        price=args.price, currency=args.currency, validity=args.validity, name=args.name, label=args.label)
        save(args.file, result)
    elif args.command == "update": save(args.file, update(catalog, args.key, load(args.patch)))
    elif args.command == "delete": save(args.file, retire(catalog, args.key))
    elif args.command == "list": print(json.dumps(catalog["products"], ensure_ascii=False, indent=2))
    elif args.command == "show":
        product = next((r for r in catalog["products"] if r["key"] == args.key), None)
        if product is None: raise ValueError("Product not found")
        print(json.dumps(product, ensure_ascii=False, indent=2))
    elif args.command == "validate": validate(catalog, load(args.snapshot) if args.snapshot else None); print("VALID")
    elif args.command == "compile":
        print(compile_catalog(catalog, args.output, load(args.snapshot) if args.snapshot else None))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit(str(error))
