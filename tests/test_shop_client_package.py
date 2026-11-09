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
from manager.shop import PACKAGE_FILES, Shop
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
        expected = {'symbols.fragment.ini', 'manifest.json'}
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
            self.assertEqual(manifest['mods'], {'shop_localization': '0.2.0', 'shop_labels': '0.3.0'})
            for name, digest in manifest['files'].items():
                self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)
        self.assertEqual(self.snapshot(self.client), before)

    def test_complete_localization_callsites_and_label_requirements_use_current_symbols(self):
        with zipfile.ZipFile(io.BytesIO(self.export())) as archive:
            fragment = storage.ini_values(archive.read('symbols.fragment.ini'))
        self.assertIn('shop_localization.call.009C038C', fragment)
        self.assertEqual(fragment['shop_localization.call.009C038C']['rva'], '0x005C038C')
        self.assertTrue(UNIT_NAME_DISPLAY_SYMBOLS.issubset(fragment.sections()))
        self.assertIn('shop.buy_options.populate', fragment)
        self.assertIn('text.ansi_to_wide', fragment)
        self.assertNotIn('unrelated.private', fragment)

    def test_export_never_starts_bridge_or_leaks_neighbor_files_and_rejects_credentials(self):
        private = self.client / 'private/account.ini'
        private.parent.mkdir()
        private.write_text('password=never-export', encoding='utf-8')
        self.workspace.services.resource = Mock(side_effect=AssertionError('bridge must not start'))
        raw = self.export()
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
            self.assertEqual(result['installed_files'], 10)
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


if __name__ == '__main__':
    unittest.main()
