"""Neutral shop examples: preserve balances by construction, without SQL/game access."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('shop_import_examples', ROOT / 'examples/shop/build_examples.py')
examples = importlib.util.module_from_spec(spec)
spec.loader.exec_module(examples)


def fixture():
    categories, products, options, units = [], [], [], []
    for index, (kind, time) in enumerate(((11, 0), (11, 1), (12, 1), (15, 0), (22, 2)), 1):
        category = kind * 100000 + 900 + index
        categories.append({'CatalogueID': category, 'TimeLimit': time, 'Name': 'Synthetic target', 'RefIndex': 29000 + index})
        products.append({'f_productid': 1010000 + index, 'f_itemid': '11000' + str(index),
                         'f_package': '0', 'f_depth1': 1, 'f_display': '1', 'f_quantity': 1})
        options.append({'f_itemtblidx': index, 'f_itemid': '11000' + str(index), 'f_categoryNumber': category,
                        'f_validity': 7 if time == 1 else 1, 'f_price': index * 100, 'f_payment': '1' if index % 2 else '2'})
        if kind == 11:
            units.append({'UnitNumber': 29000 + index, 'SallyReady': 1})
    return {'categories': categories, 'products': products, 'options': options, 'units': units}


class ImportExamples(unittest.TestCase):
    def setUp(self):
        self.snapshot = fixture()
        self.catalog = {'schema_version': 1, 'catalog_id': 'fictional-shop', 'products': []}
        self.recipes = json.loads((ROOT / 'examples/shop/templates.json').read_bytes())

    def test_each_step_adds_one_disabled_offer_and_inherits_exact_target_terms(self):
        original = copy.deepcopy(self.snapshot)
        planned = examples.plan_examples(self.snapshot, self.catalog, self.recipes)
        self.assertEqual(5, len(planned))
        keys, products, skus = set(), set(), set()
        for number, item in enumerate(planned, 1):
            self.assertEqual(number, len(item['catalog']['products']))
            added = item['catalog']['products'][-1]
            self.assertFalse(added['enabled'])
            for field in ('price', 'currency', 'validity', 'category_id'):
                self.assertEqual(item['selection'][field], added[field])
            self.assertNotIn(added['product_id'], products)
            self.assertNotIn(added['item_id'], skus)
            self.assertNotIn(added['key'], keys)
            products.add(added['product_id']); skus.add(added['item_id']); keys.add(added['key'])
        self.assertEqual(original, self.snapshot)
        self.assertEqual([], self.catalog['products'])

    def test_existing_offers_and_identity_are_preserved_without_implicit_retirement(self):
        base = examples.plan_examples(self.snapshot, self.catalog, self.recipes)[0]['catalog']
        base['products'][0]['key'] = 'authored-existing'
        base['products'][0]['enabled'] = True
        before = copy.deepcopy(base)
        planned = examples.plan_examples(self.snapshot, base, self.recipes)
        for item in planned:
            self.assertEqual(before['catalog_id'], item['catalog']['catalog_id'])
            self.assertEqual(before['products'], item['catalog']['products'][:1])
        self.assertEqual(before, base)

    def test_each_published_recipe_can_be_resolved_independently(self):
        paths = sorted((ROOT / 'examples/shop/recipes').glob('*.json'))
        self.assertEqual(5, len(paths))
        for path in paths:
            with self.subTest(recipe=path.name):
                planned = examples.plan_examples(self.snapshot, self.catalog, json.loads(path.read_bytes()))
                self.assertEqual(1, len(planned))
                self.assertEqual(path.stem, planned[0]['key'])
                self.assertFalse(planned[0]['catalog']['products'][0]['enabled'])

    def test_free_hidden_unregistered_and_wrong_kind_options_cannot_become_examples(self):
        for change in ('hidden', 'zero-price', 'unregistered-unit', 'wrong-kind'):
            data = copy.deepcopy(self.snapshot)
            if change == 'hidden': data['products'][0]['f_display'] = '0'
            elif change == 'zero-price': data['options'][0]['f_price'] = 0
            elif change == 'unregistered-unit': data['units'][0]['SallyReady'] = 0
            else: data['categories'][0]['CatalogueID'] = 9900001
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'No visible'):
                examples.plan_examples(data, self.catalog, self.recipes)

    def test_fractional_price_is_not_silently_truncated(self):
        self.snapshot['options'][0]['f_price'] = '100.25'
        with self.assertRaisesRegex(ValueError, 'exact integer'):
            examples.plan_examples(self.snapshot, self.catalog, self.recipes)

    def test_repeated_keys_and_executable_recipe_fields_are_refused(self):
        planned = examples.plan_examples(self.snapshot, self.catalog, self.recipes)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            examples.plan_examples(self.snapshot, planned[-1]['catalog'], self.recipes)
        self.recipes['recipes'][0]['command'] = 'not executable'
        with self.assertRaisesRegex(ValueError, 'fields differ'):
            examples.plan_examples(self.snapshot, self.catalog, self.recipes)

    def test_packages_are_authoring_only_hash_bound_and_do_not_overwrite_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot, catalog = root / 'snapshot.json', root / 'shop.json'
            snapshot.write_bytes(examples.encoded(self.snapshot)); catalog.write_bytes(examples.encoded(self.catalog))
            output = root / 'examples'
            result = examples.build(snapshot, catalog, output)
            self.assertFalse(result['applied'])
            for item in result['examples']:
                with zipfile.ZipFile(output / item['archive']) as archive:
                    self.assertNotIn('manifest.json', archive.namelist())
                    manifest = json.loads(archive.read('shop-example.json'))
                    self.assertFalse(manifest['applies_sql'])
                    self.assertFalse(manifest['new_offers_enabled'])
                    for name, digest in manifest['files'].items():
                        self.assertEqual(digest, examples.sha(archive.read(name)))
            with self.assertRaisesRegex(ValueError, 'new output directory'):
                examples.build(snapshot, catalog, output)

    def test_missing_type_fails_before_creating_any_partial_package(self):
        self.snapshot['options'].pop()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); snapshot, catalog = root / 'snapshot.json', root / 'shop.json'
            snapshot.write_bytes(examples.encoded(self.snapshot)); catalog.write_bytes(examples.encoded(self.catalog))
            with self.assertRaisesRegex(ValueError, 'No visible'):
                examples.build(snapshot, catalog, root / 'out')
            self.assertFalse((root / 'out').exists())


if __name__ == '__main__':
    unittest.main()
