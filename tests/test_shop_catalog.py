import configparser
import copy
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import shop_catalog as shop


def snapshot():
    return {"products": [{"f_productid": 1010001, "f_itemid": "110001", "f_package": "0", "f_depth1": 1, "f_display": "1"}],
            "options": [{"f_itemid": "110001", "f_categoryNumber": 1110549}],
            "categories": [{"CatalogueID": 1110549, "TimeLimit": 1, "Name": "租赁机体"},
                           {"CatalogueID": 1120022, "TimeLimit": 0, "Name": "永久机体"}]}


def catalog():
    return {"schema_version": 1, "catalog_id": "host-shop", "products": []}


def add(value=None, data=None, **kw):
    args = {"key": "zaku", "category_id": 1120022, "price": 500, "currency": "GB"}
    args.update(kw)
    return shop.create(value or catalog(), data or snapshot(), **args)


class ShopConfiguration(unittest.TestCase):
    def test_large_catalog_round_trips_without_shared_active_slot_quota(self):
        value = add()
        base = value['products'][0]
        value['products'] = [{**base, 'key': f'large-{i}', 'product_id': 1011000+i,
                              'item_id': str(130000+i)} for i in range(5000)]
        shop.validate(value, snapshot())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'catalog.ini'
            shop.compile_catalog(value, output, snapshot())
            parsed = configparser.ConfigParser(interpolation=None)
            parsed.read(output, encoding='utf-16')
            self.assertEqual(len(parsed.sections()), 5001)

    def test_legacy_tail_exhaustion_uses_only_free_ids_in_same_native_pools(self):
        data = snapshot()
        data['reserved_product_ids'] = list(range(1019001, 1020000)) + [1010002]
        data['reserved_skus'] = [f'1{i:05d}' for i in range(90001, 100000)] + ['100001']
        p = add(data=data)['products'][0]
        self.assertEqual((p['product_id'], p['item_id']), (1010003, '100002'))
        self.assertEqual(data['products'][0]['f_productid'], 1010001)

    def test_full_tab_does_not_consume_other_tab_or_recycle_retired_identity(self):
        data = snapshot()
        data['reserved_product_ids'] = list(range(1010001, 1020000))
        with self.assertRaisesRegex(ValueError, 'other tabs remain available'):
            add(data=data)
        data['categories'].append({'CatalogueID': 1210001, 'TimeLimit': 0, 'Name': '副官'})
        data['products'].append({'f_productid': 1030001, 'f_itemid': '300001', 'f_package': '0', 'f_depth1': 3, 'f_display': '1'})
        data['options'].append({'f_itemid': '300001', 'f_categoryNumber': 1210001})
        p = add(data=data, category_id=1210001)['products'][0]
        self.assertEqual((p['product_id'], p['item_id']), (1039001, '390001'))
        retired = shop.retire(add(), 'zaku')
        p = add(value=retired, key='next')['products'][-1]
        self.assertEqual((p['product_id'], p['item_id']), (1019002, '190002'))

    def test_full_sku_prefix_does_not_spill_into_another_prefix(self):
        data = snapshot()
        data['reserved_skus'] = [f'1{i:05d}' for i in range(1, 100000)]
        with self.assertRaisesRegex(ValueError, 'other prefixes remain available'):
            add(data=data)

    def test_oversized_input_rejected_before_json_read(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'catalog.json'
            with path.open('wb') as stream:
                stream.truncate(shop.MAX_CATALOG_BYTES + 1)
            with self.assertRaisesRegex(ValueError, '32 MiB'):
                shop.load(path)

    def test_create_respects_native_tab_and_sku_kind(self):
        p = add()["products"][0]
        self.assertEqual(p["product_id"], 1019001)
        self.assertEqual(p["item_id"], "190001")
        self.assertEqual(p["label"], "永久")

    def test_rental_duration_remains_explicit(self):
        p = add(category_id=1110549, validity=7, currency="MB")["products"][0]
        self.assertEqual(p["label"], "租赁 7 天")
        self.assertEqual(p["validity"], 7)

    def test_native_id_collision_avoided(self):
        data = snapshot()
        data["products"].append({**data["products"][0], "f_productid": 1019001, "f_itemid": "190001"})
        p = add(data=data)["products"][0]
        self.assertEqual((p["product_id"], p["item_id"]), (1019002, "190002"))

    def test_update_delete_and_identity_stay_separate(self):
        original = add()
        changed = shop.update(original, "zaku", {"price": 900, "currency": "MB"})
        retired = shop.retire(changed, "zaku")
        self.assertTrue(retired["products"][0]["retired"])
        self.assertFalse(retired["products"][0]["enabled"])
        self.assertEqual(original["products"][0]["price"], 500)
        self.assertEqual(changed["products"][0]["product_id"], original["products"][0]["product_id"])
        with self.assertRaises(ValueError): shop.update(original, "zaku", {"product_id": 1010001})

    def test_retired_ids_are_not_reused(self):
        retired = shop.retire(add(), "zaku")
        p = add(retired, key="new")["products"][-1]
        self.assertEqual(p["product_id"], 1019002)

    def test_reject_unusable_free_price_and_bad_currency(self):
        for price in [0, -1, 1.2, True]:
            with self.assertRaises(ValueError): add(price=price)
        with self.assertRaises(ValueError): add(currency="GP")

    def test_permanent_unit_is_not_a_seven_day_sale(self):
        with self.assertRaises(ValueError): add(validity=7)

    def test_unknown_category_and_native_kind_rejected(self):
        with self.assertRaises(ValueError): add(category_id=9999999)
        data = snapshot()
        data["categories"].append({"CatalogueID": 1540001, "Name": "设计图", "TimeLimit": 0})
        with self.assertRaises(ValueError): add(data=data, category_id=1540001)

    def test_ini_injection_and_wrong_unicode_length_rejected(self):
        with self.assertRaises(ValueError): add(name="中文\n[shop]\nSchema=2")
        with self.assertRaises(ValueError): add(name="机"*51)
        with self.assertRaises(ValueError): add(key="a]\n[evil")

    def test_compile_is_utf16_and_portable(self):
        value = add(name="O'Hara; 中文商品")
        root = Path(__file__).resolve().parents[1] / "build" / "shop_config_test"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="different host ", dir=root) as directory:
            path = Path(directory).resolve()
            self.assertTrue(path.is_relative_to(root.resolve()))
            ini = path / "catalog.ini"
            revision = shop.compile_catalog(value, ini, snapshot())
            self.assertTrue(ini.read_bytes().startswith(b"\xff\xfe"))
            parser = configparser.ConfigParser(interpolation=None)
            parser.read(ini, encoding="utf-16")
            self.assertEqual(parser["product:zaku"]["Name"], "O'Hara; 中文商品")
            self.assertEqual(parser["product:zaku"]["Currency"], "GB")
            self.assertEqual(parser["shop"]["Revision"], revision)
            first = ini.read_bytes()
            shop.compile_catalog(value, ini, snapshot())
            self.assertEqual(first, ini.read_bytes())
            self.assertNotIn(directory, ini.read_text(encoding="utf-16"))

    def test_compile_retired_entry_retires_in_runtime_by_omission(self):
        retired = shop.retire(add(), "zaku")
        root = Path(__file__).resolve().parents[1] / "build" / "shop_config_test"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            self.assertTrue(Path(directory).resolve().is_relative_to(root.resolve()))
            path = Path(directory) / "catalog.ini"
            shop.compile_catalog(retired, path)
            self.assertNotIn("[product:", path.read_text(encoding="utf-16"))


if __name__ == "__main__": unittest.main()
