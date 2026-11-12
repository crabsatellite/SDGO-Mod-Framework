"""The shop player ZIP is a read-only, exact client overlay."""
import base64
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

from manager import packages, storage
from manager.shop import (PACKAGE_FILES, Shop, _blueprint_category_map,
                          _shop_acquisition_sources, _shop_category_maps, _symbol_fragment)
from manager.workspace import Workspace


ROOT = Path(__file__).resolve().parents[1]
HOST = 'GOnline-54BE1568-00C97000'
REVISION = '7' * 64
PUBLISHED_REVISION = '8' * 64
UNIT_NAME_DISPLAY_SYMBOLS = {
    'shop_localization.unit_name_display.unit_name_5',
    'shop_localization.unit_name_display.unit_name_105',
    'shop_localization.unit_name_display.unit_name_205',
    'shop_localization.unit_name_display.call.0083A5A4',
    'shop_localization.unit_name_display.call.00970764',
    'shop_localization.unit_name_display.call.00812654',
    'shop_localization.unit_name_display.call.0089751A',
    'shop_localization.unit_name_display.call.008C051C',
    'shop_localization.unit_name_display.call.009CA9D4',
    'shop_localization.unit_name_display.call.005F6BFD',
    'shop_localization.unit_name_display.call.00620E58',
    'shop_localization.unit_name_display.assign_call.006EF1F4',
    'shop_localization.unit_name_display.assign_call.006EF31D',
    'shop_localization.unit_name_display.assign_call.006EF404',
}


class ShopClientPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.client = self.root / 'owner-client'
        self.client.mkdir()
        (self.client / 'GOnline.exe').write_bytes(b'fixture')
        localization = self.client / 'sdgo/mods/shop_localization'
        labels = self.client / 'sdgo/mods/shop_labels'
        localization.mkdir(parents=True)
        labels.mkdir(parents=True)
        (localization / 'shop_localization.dll').write_bytes(b'MZ localization fixture')
        (localization / 'mod.ini').write_text(
            '[mod]\nid=shop_localization\nversion=0.2.0\ndll=shop_localization.dll\n'
            'hosts=' + HOST + '\nrequires=shop_localization.assign,shop_localization.copy\n',
            encoding='ascii')
        (localization / 'manager.json').write_text(
            json.dumps({'schema_version': 1, 'id': 'shop_localization', 'title': '商城汉化',
                        'category': 'gameplay', 'hosts': ['client'], 'settings': []}),
            encoding='utf-8')
        (localization / 'zh-CN.ini').write_text(
            '[entry:382534f4a1acf9259dc5c63c]\nKind=name\nSource=Gundam\nText=高达\n',
            encoding='utf-8')
        fragment = ROOT / 'mods/shop_localization/symbols.fragment.ini'
        (localization / 'symbols.fragment.ini').write_bytes(fragment.read_bytes())
        (labels / 'shop_labels.dll').write_bytes(b'MZ labels fixture')
        (labels / 'mod.ini').write_text(
            '[mod]\nid=shop_labels\nversion=0.3.0\ndll=shop_labels.dll\nhosts=' + HOST + '\n'
            'requires=shop.buy_options.populate,text.ansi_to_wide\n',
            encoding='ascii')
        (labels / 'manager.json').write_text(
            json.dumps({'schema_version': 1, 'id': 'shop_labels', 'title': '商城显示',
                        'category': 'gameplay', 'hosts': ['client'], 'settings': []}),
            encoding='utf-8')
        (labels / 'display.ini').write_bytes(
            b'\xff\xfe' + ('[shop]\r\nRevision=' + PUBLISHED_REVISION + '\r\n'
                           '[option:3806]\r\nCategory=1110549\r\nValidity=7\r\nPayment=2\r\n'
                           'Price=9900\r\nText=租赁 7 天\r\n'
                           '[product:1019001]\r\nName=测试高达\r\nTemplateId=1010001\r\n'
                           'Category=1110549\r\nValidity=7\r\nPayment=2\r\nPrice=9900\r\n').encode('utf-16-le'))
        symbols = self.client / ('sdgo/symbols/' + HOST + '.ini')
        symbols.parent.mkdir()
        current = storage.ini_values(fragment.read_bytes())
        current['shop.buy_options.populate'] = {'kind': 'code', 'rva': '0x201', 'expect': 'DD'}
        current['text.ansi_to_wide'] = {'kind': 'code', 'rva': '0x202', 'expect': 'EE'}
        current['unrelated.private'] = {'kind': 'data', 'rva': '0x999'}
        text = io.StringIO()
        current.write(text, space_around_delimiters=False)
        symbols.write_text(text.getvalue(), encoding='ascii')
        self.workspace = Workspace(self.root / 'state', self.root / 'contracts')
        self.workspace.settings['modules']['shop'] = {
            'workspace': str(self.root / 'shop-workspace'),
            'client': str(self.client),
        }
        self.active_products = []
        self.bridge = Mock()
        def bridge_call(request):
            self.assertEqual('read-effective', request.get('action'))
            return {
                'active_products': list(self.active_products),
                'active_products_source': 'store_billing_visible_readback',
            }
        self.bridge.call.side_effect = bridge_call
        self.workspace.services.resource = Mock(return_value=self.bridge)
        self.provider = Shop(self.workspace)

    def tearDown(self):
        self.workspace.services.close()
        self.temp.cleanup()

    def export(self):
        result = self.provider.request('GET', '/api/shop/client-package', {}, {'X-Shop-Revision': REVISION})
        self.assertEqual(result.mime, 'application/zip')
        return result.data

    @staticmethod
    def snapshot(root):
        return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in root.rglob('*') if path.is_file()}

    def test_exact_allowlist_hashes_and_static_dictionary_bytes(self):
        before = self.snapshot(self.client)
        raw = self.export()
        expected = {'symbols.fragment.ini', 'manifest.json',
                    'client/sdgo/mods/shop_localization/acquisition-sources.json',
                    'client/sdgo/mods/shop_localization/acquisition-sources.bin'}
        for identity, names in PACKAGE_FILES.items():
            expected.update('client/sdgo/mods/' + identity + '/' + name for name in names)
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            self.assertEqual(set(archive.namelist()), expected)
            self.assertIsNone(archive.testzip())
            dictionary = self.client / 'sdgo/mods/shop_localization/zh-CN.ini'
            self.assertEqual(archive.read('client/sdgo/mods/shop_localization/zh-CN.ini'),
                             dictionary.read_bytes())
            manifest = json.loads(archive.read('manifest.json'))
            self.assertEqual(manifest['shop_revision'], REVISION)
            self.assertEqual(manifest['published_revision'], PUBLISHED_REVISION)
            self.assertEqual('store_billing_visible_readback',
                             manifest['acquisition_provenance']['source'])
            self.assertEqual(0, manifest['acquisition_provenance']['active_product_options'])
            self.assertEqual(manifest['mods'], {'shop_localization': '0.2.0', 'shop_labels': '0.3.0'})
            self.assertEqual(b'SDGOAS1\0', archive.read(
                'client/sdgo/mods/shop_localization/acquisition-sources.bin')[:8])
            for name, digest in manifest['files'].items():
                self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)
        self.assertEqual(self.snapshot(self.client), before)

    def test_blueprint_sources_join_stock_and_enabled_imported_item_refs(self):
        unit = self.client / 'sdgo/mods/unit_catalog'
        unit.mkdir(parents=True)
        (unit / 'definitions.json').write_text(json.dumps({
            'protocol': 'sdgo.unit-family', 'schema_version': 1, 'registrations': [{
                'category_id': 1199000, 'unit_id': 90001, 'enabled': True,
                'blueprints': [
                    {'id': 209000, 'item_category_id': 1599000, 'enabled': True},
                    {'id': 209001, 'item_category_id': 1599001, 'enabled': False},
                ],
            }],
        }), encoding='utf-8')
        active = [
            {'ProductId': 1019001, 'OptionId': 3801, 'Category': 1599000, 'Validity': 1},
            # The disabled imported recipe is not in the category map even if a
            # stale stock row still exists in StoreBilling.
            {'ProductId': 1019002, 'OptionId': 3802, 'Category': 1599001, 'Validity': 1},
        ]
        value = json.loads(_shop_acquisition_sources(
            self.client, active, 'store_billing_visible_readback'))
        self.assertEqual({'209000': [{
            'key': 'shop:blueprint:209000', 'text': '商城购买',
        }]}, value['blueprints'])
        self.assertEqual({}, value['units'])

        stock = bytearray(614)
        stock[105:109] = (200001).to_bytes(4, 'little')
        stock[109] = 0
        fake = Mock()
        fake.records.return_value = {1520001: bytes(stock)}
        with patch('manager.shop.native_assets.Assets', return_value=fake):
            mapping = _blueprint_category_map(self.client)
            units, blueprints = _shop_category_maps(self.client)
            stock_source = json.loads(_shop_acquisition_sources(self.client, [{
                'ProductId': 1000001, 'OptionId': 3001,
                'Category': 1520001, 'Validity': 1,
            }], 'store_billing_visible_readback'))
        self.assertEqual({'200001': [{
            'key': 'shop:blueprint:200001', 'text': '商城购买',
        }]}, stock_source['blueprints'])
        self.assertEqual(200001, mapping[1520001])
        self.assertEqual(209000, mapping[1599000])
        self.assertNotIn(1599001, mapping)
        self.assertEqual(90001, units[1199000])
        self.assertEqual(mapping, blueprints)


    def test_unit_sources_use_item_time_limit_for_one_day_rental_and_purchase(self):
        first = bytearray(614)
        first[105:109] = (10001).to_bytes(4, 'little')
        second = bytearray(614)
        second[105:109] = (10002).to_bytes(4, 'little')
        second[109] = 1
        fake = Mock()
        fake.records.return_value = {1110001: bytes(first), 1110002: bytes(second)}
        with patch('manager.shop.native_assets.Assets', return_value=fake):
            purchase = json.loads(_shop_acquisition_sources(
                self.client, [
                    {'ProductId': 1, 'OptionId': 11, 'Category': 1110001, 'Validity': 5},
                    {'ProductId': 2, 'OptionId': 12, 'Category': 1110002, 'Validity': 1},
                ], 'store_billing_visible_readback'))
            self.assertEqual([{
                'key': 'shop:unit:10001:purchase', 'text': '商城购买',
            }], purchase['units']['10001'])
            self.assertEqual([{
                'key': 'shop:unit:10002:rental', 'text': '商城租用',
            }], purchase['units']['10002'])

            retargeted = json.loads(_shop_acquisition_sources(
                self.client, [
                    {'ProductId': 1, 'OptionId': 11, 'Category': 1110002, 'Validity': 30},
                ], 'store_billing_visible_readback'))
            self.assertNotIn('10001', retargeted['units'])
            self.assertEqual([{
                'key': 'shop:unit:10002:rental', 'text': '商城租用',
            }], retargeted['units']['10002'])

            cleared = json.loads(_shop_acquisition_sources(
                self.client, [], 'store_billing_visible_readback'))
            self.assertEqual({}, cleared['units'])

    def test_complete_localization_callsites_and_label_requirements_use_current_symbols(self):
        with zipfile.ZipFile(io.BytesIO(self.export())) as archive:
            fragment = storage.ini_values(archive.read('symbols.fragment.ini'))
        self.assertIn('shop_localization.call.009C038C', fragment)
        self.assertEqual(fragment['shop_localization.call.009C038C']['rva'], '0x005C038C')
        self.assertTrue(UNIT_NAME_DISPLAY_SYMBOLS.issubset(fragment.sections()))
        self.assertIn('shop.buy_options.populate', fragment)
        self.assertIn('text.ansi_to_wide', fragment)
        self.assertNotIn('unrelated.private', fragment)

    def test_export_uses_readback_and_never_leaks_neighbor_files_or_credentials(self):
        private = self.client / 'private/account.ini'
        private.parent.mkdir()
        private.write_text('password=never-export', encoding='utf-8')
        raw = self.export()
        self.bridge.call.assert_called()
        self.assertNotIn(b'never-export', raw)
        self.assertNotIn(str(self.client).encode(), raw)
        display = self.client / 'sdgo/mods/shop_labels/display.ini'
        display.write_bytes(b'\xff\xfe' + '[shop]\r\nRevision=x\r\n[database]\r\nPassword=secret\r\n'.encode('utf-16-le'))
        with self.assertRaisesRegex(ValueError, '未登记内容'):
            self.export()
        display.write_bytes(
            b'\xff\xfe' + ('[shop]\r\nRevision=' + PUBLISHED_REVISION +
                           '\r\n[product:1019001]\r\nName=English Gundam\r\nTemplateId=1010001\r\n'
                           'Category=1110549\r\nValidity=7\r\nPayment=2\r\nPrice=9900\r\n').encode('utf-16-le'))
        with self.assertRaisesRegex(ValueError, '必须包含汉字'):
            self.export()

    def test_rejects_strict_display_and_symbol_boundary_failures(self):
        display = self.client / 'sdgo/mods/shop_labels/display.ini'
        fragment = self.client / 'sdgo/mods/shop_localization/symbols.fragment.ini'
        symbols = self.client / ('sdgo/symbols/' + HOST + '.ini')
        originals = {path: path.read_bytes() for path in (display, fragment, symbols)}
        profile = originals[display].decode('utf-16')
        cases = {
            'duplicate product section': (
                display,
                originals[display] + (
                    '\r\n[product:1019001]\r\nName=重复商品\r\nTemplateId=1010001\r\n'
                    'Category=1110549\r\nValidity=7\r\nPayment=2\r\nPrice=9900\r\n'
                ).encode('utf-16-le'),
                '重复或无效'),
            'missing product field': (
                display,
                b'\xff\xfe' + profile.replace('TemplateId=1010001\r\n', '').encode('utf-16-le'),
                '字段不完整'),
            'duplicate product key': (
                display,
                b'\xff\xfe' + profile.replace(
                    'Name=测试高达\r\n', 'Name=测试高达\r\nName=重复高达\r\n', 1).encode('utf-16-le'),
                '重复或无效'),
            'invalid product number': (
                display,
                b'\xff\xfe' + profile.replace(
                    'TemplateId=1010001\r\n', 'TemplateId=not-a-number\r\n', 1).encode('utf-16-le'),
                '数字字段'),
            'missing option field': (
                display,
                b'\xff\xfe' + profile.replace('Text=租赁 7 天\r\n', '').encode('utf-16-le'),
                '字段不完整'),
            'invalid option number': (
                display,
                b'\xff\xfe' + profile.replace('Validity=7\r\nPayment=2\r\nPrice=9900\r\nText=',
                                               'Validity=0\r\nPayment=9\r\nPrice=free\r\nText=', 1).encode('utf-16-le'),
                '数字字段'),
        }
        for name, (path, raw, message) in cases.items():
            with self.subTest(name=name):
                for target, original in originals.items():
                    target.write_bytes(original)
                path.write_bytes(raw)
                with self.assertRaisesRegex(ValueError, message):
                    self.export()
        for target, original in originals.items():
            target.write_bytes(original)
        parsed = storage.ini_values(originals[fragment])
        parsed.remove_section('shop_localization.call.009E584D')
        text = io.StringIO()
        parsed.write(text, space_around_delimiters=False)
        fragment.write_text(text.getvalue(), encoding='ascii')
        with self.assertRaisesRegex(ValueError, '不完整或版本不一致'):
            self.export()
        fragment.write_bytes(originals[fragment])
        symbols.write_bytes(
            originals[symbols] +
            b'\n[shop_localization.removed_old_version]\nkind=code\nrva=0x1\n')
        with self.assertRaisesRegex(ValueError, '不完整或版本不一致'):
            self.export()

    def test_package_imports_through_standard_manager_without_scripts(self):
        raw = self.export()
        target = self.root / 'player-client'
        target.mkdir()
        (target / 'GOnline.exe').write_bytes(b'fixture')
        player = Workspace(self.root / 'player-state', self.root / 'player-contracts')
        try:
            player.add_host(target)
            host = player.settings['hosts'][0]['id']
            existing_symbols = target / ('sdgo/symbols/' + HOST + '.ini')
            existing_symbols.parent.mkdir(parents=True)
            existing_symbols.write_text(
                '[host]\nimage=GOnline.exe\ntimestamp=0x54BE1568\n'
                'size_of_image=0x00C97000\n[other.mod]\nkind=data\nrva=0x7654\n',
                encoding='ascii')
            with patch.object(packages, '_running', return_value=False):
                result = packages.import_client(player, host, base64.b64encode(raw).decode('ascii'))
            self.assertEqual(result['installed_files'], 12)
            self.assertEqual((target / 'sdgo/mods/shop_localization/zh-CN.ini').read_bytes(),
                             (self.client / 'sdgo/mods/shop_localization/zh-CN.ini').read_bytes())
            self.assertEqual((target / 'sdgo/mods/shop_localization/symbols.fragment.ini').read_bytes(),
                             (self.client / 'sdgo/mods/shop_localization/symbols.fragment.ini').read_bytes())
            installed_symbols = storage.ini_values((target / ('sdgo/symbols/' + HOST + '.ini')).read_bytes())
            self.assertIn('shop_localization.call.009C038C', installed_symbols)
            self.assertTrue(UNIT_NAME_DISPLAY_SYMBOLS.issubset(installed_symbols.sections()))
            self.assertEqual(installed_symbols['other.mod']['rva'], '0x7654')
            self.assertFalse(any(path.suffix.lower() in {'.exe', '.ps1', '.py'}
                                 for path in (target / 'sdgo').rglob('*')))
            player.settings['modules']['shop'] = {
                'workspace': str(self.root / 'player-shop-workspace'),
                'client': str(target),
            }
            readback = Mock()
            readback.call.return_value = {
                'active_products': list(self.active_products),
                'active_products_source': 'store_billing_visible_readback',
            }
            player.services.resource = Mock(return_value=readback)
            second = Shop(player).request(
                'GET', '/api/shop/client-package', {}, {'X-Shop-Revision': REVISION}).data
            self.assertEqual(second, raw)
            with zipfile.ZipFile(io.BytesIO(second)) as archive:
                second_manifest = json.loads(archive.read('manifest.json'))
                self.assertEqual(second_manifest['published_revision'], PUBLISHED_REVISION)
                self.assertIn('client/sdgo/mods/shop_localization/symbols.fragment.ini',
                              archive.namelist())
                second_symbols = storage.ini_values(archive.read('symbols.fragment.ini'))
                self.assertIn('shop_localization.call.009C038C', second_symbols)
                self.assertTrue(UNIT_NAME_DISPLAY_SYMBOLS.issubset(second_symbols.sections()))
        finally:
            player.services.close()

    def test_apply_reports_server_success_when_client_publication_fails(self):
        self.bridge.call.side_effect = lambda request: {
            'active_products': [],
            'active_products_source': 'store_billing_visible_readback',
            'items': [], 'catalog': {'products': []}, 'revision': REVISION,
        }
        with patch.object(self.provider, '_install_applied_acquisition_sources',
                          side_effect=ValueError('fixture publication failure')) as publish:
            with self.assertRaisesRegex(ValueError, '商城服务器应用已完成，但客户端获得方式发布失败'):
                self.provider.request('POST', '/api/shop/apply', {},
                                      {'X-Shop-Revision': REVISION})
        self.assertEqual('apply', self.bridge.call.call_args.args[0]['action'])
        publish.assert_called_once()

    def test_full_stock_readback_is_used_by_source_callable_export_and_apply(self):
        stock = bytearray(614)
        stock[105:109] = (200001).to_bytes(4, 'little')
        fake = Mock()
        fake.records.return_value = {1520001: bytes(stock)}
        self.active_products = [{
            'ProductId': 1000001, 'OptionId': 3001,
            'Category': 1520001, 'Validity': 1,
        }]
        with patch('manager.shop.native_assets.Assets', return_value=fake):
            source = self.provider.applied_acquisition_sources()
            raw = self.export()
        self.assertEqual('store_billing_visible_readback', source['provenance']['source'])
        self.assertEqual({'200001': [{
            'key': 'shop:blueprint:200001', 'text': '商城购买',
        }]}, source['blueprints'])
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            exported = json.loads(archive.read(
                'client/sdgo/mods/shop_localization/acquisition-sources.json'))
        self.assertEqual(source, exported)
        # This stock product is absent from the managed display.ini fixture; it
        # is still published because current StoreBilling readback is authority.
        display = (self.client / 'sdgo/mods/shop_labels/display.ini').read_text(
            encoding='utf-16')
        self.assertNotIn('1000001', display)

        def apply_call(request):
            self.assertEqual('apply', request.get('action'))
            return {
                'active_products': list(self.active_products),
                'active_products_source': 'store_billing_visible_readback',
                'items': [], 'catalog': {'products': []}, 'revision': REVISION,
            }
        self.bridge.call.side_effect = apply_call
        with patch('manager.shop.native_assets.Assets', return_value=fake):
            applied = self.provider.request(
                'POST', '/api/shop/apply', {}, {'X-Shop-Revision': REVISION})
        sidecar = self.client / 'sdgo/mods/shop_localization/acquisition-sources.json'
        self.assertEqual(source, json.loads(sidecar.read_text(encoding='utf-8')))
        self.assertTrue(applied['acquisition_sources']['restart_required'])
        self.assertEqual(b'SDGOAS1\0', (sidecar.with_suffix('.bin')).read_bytes()[:8])

        # A subsequent successful readback with the product down clears the
        # client claim instead of retaining the former draft or display label.
        self.active_products = []
        with patch('manager.shop.native_assets.Assets', return_value=fake):
            cleared = self.provider.request(
                'POST', '/api/shop/apply', {}, {'X-Shop-Revision': REVISION})
        self.assertEqual({}, json.loads(sidecar.read_text(encoding='utf-8'))['blueprints'])
        self.assertEqual(0, cleared['acquisition_sources']['provenance'][
            'active_product_options'])

    def test_missing_files_symbols_client_and_revision_fail_before_bridge(self):
        self.workspace.services.resource = Mock(side_effect=AssertionError('bridge must not start'))
        self.provider.config.pop('client')
        with self.assertRaisesRegex(ValueError, '选择'):
            self.provider.request('GET', '/api/shop/client-package', {}, {'X-Shop-Revision': REVISION})
        self.provider.config['client'] = str(self.client)
        with self.assertRaisesRegex(ValueError, '版本'):
            self.provider.request('GET', '/api/shop/client-package', {}, {'X-Shop-Revision': ''})
        symbol = self.client / ('sdgo/symbols/' + HOST + '.ini')
        symbol.write_text('[host]\ntimestamp=0x54BE1568\nsize_of_image=0x00C97000\n', encoding='ascii')
        with self.assertRaisesRegex(ValueError, '不完整或版本不一致'):
            self.export()


class RepositoryShopSymbolContractTests(unittest.TestCase):
    def test_shipped_host_and_module_fragments_satisfy_the_actual_export_consumer(self):
        import shutil
        with tempfile.TemporaryDirectory() as temporary:
            client = Path(temporary)
            symbols = client/'sdgo/symbols'
            symbols.mkdir(parents=True)
            shutil.copy2(ROOT/'symbols'/(HOST+'.ini'), symbols/(HOST+'.ini'))
            manifests = {}
            for identity in ('shop_labels', 'shop_localization'):
                destination = client/'sdgo/mods'/identity
                destination.mkdir(parents=True)
                source = ROOT/'mods'/identity
                manifests[identity] = storage.ini_values((source/'mod.ini').read_bytes())
                if (source/'symbols.fragment.ini').is_file():
                    shutil.copy2(source/'symbols.fragment.ini', destination/'symbols.fragment.ini')
            self.assertTrue(_symbol_fragment(client, manifests, HOST))


if __name__ == '__main__':
    unittest.main()
