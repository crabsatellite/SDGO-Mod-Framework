"""Resolve neutral recipes against an owner's shop snapshot; never apply SQL.

Generated JSON/ZIP files are owner authoring output, not framework defaults or
player installers. Existing catalog entries remain unchanged, all added offers
are disabled, and price/currency/validity come from a visible target option.
"""
from __future__ import annotations

import argparse
import copy
from decimal import Decimal, InvalidOperation
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import shop_catalog as shop


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n').encode('utf-8')


def source_integer(value, field):
    if isinstance(value, bool):
        raise ValueError('Target ' + field + ' must be an exact integer')
    try:
        number = Decimal(str(value))
        if not number.is_finite() or number != number.to_integral_value():
            raise ValueError('Target ' + field + ' must be an exact integer')
        return int(number)
    except InvalidOperation:
        raise ValueError('Target ' + field + ' must be an exact integer') from None


def recipes(value):
    if not isinstance(value, dict) or set(value) != {'schema', 'recipes'} or value['schema'] != 'sdgo.shop-example-recipes/1':
        raise ValueError('Unsupported shop example recipe document')
    if not isinstance(value['recipes'], list) or not 1 <= len(value['recipes']) <= 16:
        raise ValueError('Provide 1..16 example recipes')
    seen = set()
    for row in value['recipes']:
        if not isinstance(row, dict) or set(row) != {'key', 'name', 'category_kind', 'time_limit'}:
            raise ValueError('Example recipe fields differ')
        key = row['key']
        if not isinstance(key, str) or not re.fullmatch('[a-z][a-z0-9-]{0,39}', key) or key in seen:
            raise ValueError('Example recipe key is invalid or duplicated')
        seen.add(key)
        shop.text(row['name'], 'example name')
        if type(row['category_kind']) is not int or row['category_kind'] not in (11, 12, 15, 22):
            raise ValueError('Unsupported example item kind')
        if row['time_limit'] is not None and (type(row['time_limit']) is not int or row['time_limit'] not in (0, 1, 2)):
            raise ValueError('Example time limit must be null, 0, 1 or 2')
    return copy.deepcopy(value['recipes'])


def visible_options(snapshot, recipe):
    categories = {int(row['CatalogueID']): row for row in snapshot['categories']}
    units = {int(row['UnitNumber']): row for row in snapshot.get('units', [])}
    products = {}
    for row in snapshot['products']:
        if (str(row.get('f_display', '')).strip() != '1' or int(row.get('f_quantity', 1)) <= 0
                or str(row.get('f_package', '0')).strip() != '0' or int(row['f_depth1']) == 20):
            continue
        products.setdefault(str(row['f_itemid']).strip(), []).append(row)
    candidates = []
    for option in snapshot['options']:
        category = categories.get(int(option['f_categoryNumber']))
        if category is None or int(category['CatalogueID']) // 100000 != recipe['category_kind']:
            continue
        if int(category['TimeLimit']) not in (0, 1, 2):
            continue
        if recipe['time_limit'] is not None and int(category['TimeLimit']) != recipe['time_limit']:
            continue
        if 'f_display' in option and str(option['f_display']).strip() != '1':
            continue
        payment = str(option['f_payment']).strip()
        price = source_integer(option['f_price'], 'price')
        validity = source_integer(option['f_validity'], 'validity')
        if payment not in ('1', '2') or not 1 <= price <= 2147483647 or not 1 <= validity <= 1000000:
            continue
        if recipe['category_kind'] == 11:
            unit = units.get(int(category.get('RefIndex', 0)))
            if unit is None or int(unit.get('SallyReady', 0)) != 1:
                continue
            if int(category['TimeLimit']) == 0 and validity != 1:
                continue
        for product in products.get(str(option['f_itemid']).strip(), []):
            candidates.append({
                'product_id': int(product['f_productid']),
                'option_id': int(option['f_itemtblidx']),
                'category_id': int(category['CatalogueID']),
                'price': price, 'currency': 'GB' if payment == '1' else 'MB', 'validity': validity,
            })
    return sorted(candidates, key=lambda row: (row['product_id'], row['option_id'], row['category_id']))


def plan_examples(snapshot, catalog, recipe_document):
    shop.validate(catalog, snapshot)
    current = copy.deepcopy(catalog)
    planned = []
    for recipe in recipes(recipe_document):
        key = 'example-' + recipe['key']
        if any(row['key'] == key for row in current['products']):
            raise ValueError('Example key already exists; preserve it and choose a different recipe key: ' + key)
        options = visible_options(snapshot, recipe)
        if not options:
            raise ValueError('No visible, priced, compatible target option for ' + recipe['key'])
        selected = options[0]
        current = shop.create(current, snapshot, key=key, name=recipe['name'],
                              category_id=selected['category_id'], price=selected['price'],
                              currency=selected['currency'], validity=selected['validity'])
        current = shop.update(current, key, {'enabled': False})
        # Each file preserves the prior catalog and adds exactly one disabled
        # offer; later files also retain examples from earlier steps.
        planned.append({'key': recipe['key'], 'selection': selected, 'catalog': copy.deepcopy(current)})
    assert current['products'][:len(catalog['products'])] == catalog['products']
    assert all(not row['enabled'] for row in current['products'][len(catalog['products']):])
    return planned


def build(snapshot_path, catalog_path, output, recipe_path=None):
    snapshot_path, catalog_path = Path(snapshot_path), Path(catalog_path)
    recipe_path = Path(recipe_path) if recipe_path else Path(__file__).with_name('templates.json')
    snapshot_raw, catalog_raw, recipe_raw = snapshot_path.read_bytes(), catalog_path.read_bytes(), recipe_path.read_bytes()
    snapshot = json.loads(snapshot_raw)
    planned = plan_examples(snapshot, json.loads(catalog_raw), json.loads(recipe_raw))
    output = Path(output).resolve()
    if output.exists():
        raise ValueError('Choose a new output directory; existing authored output is never overwritten')
    output.mkdir(parents=True)
    entries = []
    for number, item in enumerate(planned, 1):
        leaf = f'{number:02d}-' + item['key']
        folder = output / leaf
        folder.mkdir()
        (folder / 'shop.json').write_bytes(encoded(item['catalog']))
        shop.compile_catalog(item['catalog'], folder / 'catalog.ini', snapshot)
        (folder / 'selection.json').write_bytes(encoded(item['selection']))
        readme = (
            '# Owner shop authoring example / 商城作者模板\n\n'
            'Import shop.json in the same Shop workspace; review before applying. All new offers are disabled.\n'
            '在同一商城工作区导入 shop.json；新增商品默认未启用，审阅后才可明确上架并应用。\n'
            'Price, currency and validity are inherited from the selected visible target option. No SQL is executed here.\n'
            '价格、币种及期限/数量沿用目标现货；生成过程不执行 SQL。后续编号目录保留前面步骤的商品。\n'
            'This ZIP is authoring material, NOT a player runtime package or the normal Import configuration package input.\n'
            '本 ZIP 不是玩家安装包，不能使用普通“导入配置包”；给玩家的显示更新由 Manager 在应用后另行导出。\n'
            'Resolved target IDs and existing owner offers are private owner output; do not add them to framework source/releases.\n'
            '解析后的目标编号和已有商品属于服主输出，不作为通用框架示例或默认内容发布。\n'
        )
        (folder / 'README.md').write_text(readme, encoding='utf-8')
        members = {p.name: sha(p.read_bytes()) for p in folder.iterdir() if p.is_file()}
        manifest = {'schema': 'sdgo.shop-import-example/1', 'key': item['key'],
                    'catalog_id': item['catalog']['catalog_id'], 'applies_sql': False,
                    'new_offers_enabled': False, 'files': members,
                    'inputs': {'snapshot_sha256': sha(snapshot_raw), 'catalog_sha256': sha(catalog_raw),
                               'recipes_sha256': sha(recipe_raw)}}
        (folder / 'shop-example.json').write_bytes(encoded(manifest))
        archive = output / (leaf + '.shop-authoring.zip')
        with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as zipped:
            for member in sorted(folder.iterdir()):
                zipped.writestr(member.name, member.read_bytes())
        entries.append({'key': item['key'], 'directory': leaf, 'archive': archive.name,
                        'archive_sha256': sha(archive.read_bytes()), 'source_option': item['selection'],
                        'new_offer': item['catalog']['products'][-1]})
    result = {'schema': 'sdgo.shop-import-examples/1', 'catalog_id': planned[0]['catalog']['catalog_id'],
              'applied': False, 'examples': entries}
    (output / 'examples.json').write_bytes(encoded(result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--recipes', type=Path)
    args = parser.parse_args()
    result = build(args.snapshot, args.catalog, args.output, args.recipes)
    print(json.dumps({'examples': len(result['examples']), 'applied': False, 'new_offers_enabled': False}))


if __name__ == '__main__':
    main()
