"""Localization stays a normal Manager-owned client feature, no separate UI."""
import base64
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from manager.workspace import Workspace
from manager import packages, storage

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_shop_localization', ROOT/'tools/package_shop_localization.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class LocalizationManager(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.client = self.root/'client'
        self.client.mkdir()
        (self.client/'GOnline.exe').write_bytes(b'fixture')
        # No new built-in contract: exercise the installed-contract fallback
        # used by an already frozen, older Manager EXE.
        empty = self.root/'old-contracts'
        empty.mkdir()
        self.ws = Workspace(self.root/'state', empty)
        self.ws.add_host(self.client)
        self.host = self.ws.settings['hosts'][0]['id']

    def tearDown(self):
        self.ws.services.close()
        self.temp.cleanup()

    def overlay(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as archive:
            for name in ('mod.ini', 'manager.json', 'zh-CN.ini'):
                archive.writestr('client/sdgo/mods/shop_localization/'+name, (ROOT/'mods/shop_localization'/name).read_bytes())
            archive.writestr('client/sdgo/mods/shop_localization/shop_localization.dll', b'native fixture')
            archive.writestr('symbols.fragment.ini', (ROOT/'mods/shop_localization/symbols.fragment.ini').read_bytes())
        return base64.b64encode(data.getvalue()).decode()

    def install(self):
        with patch.object(packages, '_running', return_value=False):
            return packages.import_client(self.ws, self.host, self.overlay())

    def test_installed_contract_is_visible_in_standard_view(self):
        self.install()
        state = self.ws.public()['hosts'][0]
        mod = next(m for m in state['mods'] if m['id']=='shop_localization')
        self.assertTrue(mod['registered'])
        self.assertFalse(mod['archived'])
        self.assertEqual(mod['settings_count'], 3)
        details = self.ws.details(self.host, 'shop_localization')
        self.assertNotIn('provider', details['contract'])
        self.assertEqual(set(details['values']), {'translatenames', 'translatedescriptions', 'translateoptions'})
        self.assertTrue(all(details['values'].values()))

    def test_registered_toggles_and_mod_switch_use_revision_contract(self):
        self.install()
        d = self.ws.details(self.host, 'shop_localization')
        self.ws.configure(self.host, 'shop_localization', {'translatenames': False}, d['revisions'])
        self.assertFalse(self.ws.details(self.host, 'shop_localization')['values']['translatenames'])
        host = self.ws.public()['hosts'][0]
        mod = host['mods'][0]
        self.ws.toggle(self.host, 'shop_localization', False, {'global': host['global_revision'], 'manifest': mod['revision']})
        self.assertFalse(self.ws.public()['hosts'][0]['mods'][0]['manifest_enabled'])
        with self.assertRaises(ValueError):
            self.ws.configure(self.host, 'shop_localization', {'translatenames': True}, d['revisions'])

    def test_import_preserves_unrelated_modules_and_symbols(self):
        other = self.client/'sdgo/mods/other'
        other.mkdir(parents=True)
        (other/'keep.bin').write_bytes(b'unchanged')
        symbols = self.client/'sdgo/symbols/GOnline-54BE1568-00C97000.ini'
        symbols.parent.mkdir()
        symbols.write_text('[other.symbol]\nkind=data\nrva=0x1234\n', encoding='ascii')
        self.install()
        self.assertEqual((other/'keep.bin').read_bytes(), b'unchanged')
        parsed = storage.ini_values(symbols.read_bytes())
        self.assertEqual(parsed['other.symbol']['rva'], '0x1234')
        self.assertIn('shop_localization.assign', parsed)

    def test_running_client_and_conflicting_symbols_refused_before_writes(self):
        with patch.object(packages, '_running', return_value=True), self.assertRaises(ValueError):
            packages.import_client(self.ws, self.host, self.overlay())
        self.assertFalse((self.client/'sdgo/mods/shop_localization').exists())
        symbols = self.client/'sdgo/symbols/GOnline-54BE1568-00C97000.ini'
        symbols.parent.mkdir(parents=True)
        symbols.write_text('[shop_localization.assign]\nkind=code\nrva=0x1\n', encoding='ascii')
        before = symbols.read_bytes()
        with self.assertRaises(ValueError):
            self.install()
        self.assertEqual(symbols.read_bytes(), before)
        self.assertFalse((self.client/'sdgo/mods/shop_localization').exists())

    def test_real_overlay_is_importable_and_whitelisted(self):
        binary = ROOT/'build/Release/shop_localization.dll'
        if not binary.is_file():
            self.skipTest('Build Win32 DLL for package readback')
        target = self.root/'shop-localization.zip'
        package.build_overlay(binary, target)
        with zipfile.ZipFile(target) as archive:
            manifest = json.loads(archive.read('manifest.json'))
            self.assertFalse(any(n.endswith(('.exe','.ps1','.py')) for n in archive.namelist()))
            self.assertEqual(len(manifest['files']), 8)
        with patch.object(packages, '_running', return_value=False):
            packages.import_client(self.ws, self.host, base64.b64encode(target.read_bytes()).decode())
        self.assertTrue(self.ws.details(self.host, 'shop_localization')['values']['translateoptions'])


if __name__ == '__main__':
    unittest.main()
