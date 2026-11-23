"""Read-only Manager diagnosis, with neutral synthetic content only."""
from pathlib import Path
import hashlib
import shutil
import struct
import tempfile
import unittest

from manager import registry, storage
from manager.localization_check import check, dictionary
from manager.workspace import Workspace
from owner.units.compiler import fnv, native_catalog

ROOT = Path(__file__).resolve().parents[1]
WORDS = b'[entry:generic]\nKind=name\nSource=GENERIC UNIT\nText=Generic translated unit\n'


class LocalizationCheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.client = Path(self.temporary.name)
        self.workspace = object.__new__(Workspace)
        self.workspace.mode = 'standard'
        self.workspace.contracts = registry.load(ROOT / 'mods')
        self.workspace.settings = {'hosts': [{'id': 'fixture', 'role': 'client', 'path': str(self.client)}]}
        for name in ('shop_localization', 'unit_catalog'):
            folder = self.client / 'sdgo/mods' / name
            folder.mkdir(parents=True)
            for leaf in ('mod.ini', 'manager.json'):
                shutil.copyfile(ROOT / 'mods' / name / leaf, folder / leaf)
            path = folder / 'mod.ini'
            path.write_bytes(storage.patch_ini(path.read_bytes(), [('mod', 'enabled', '1')]))
        (self.client / 'sdgo/mods/shop_localization/zh-CN.ini').write_bytes(WORDS)

    def catalog(self, *, malformed=False, preset=False):
        raw = bytearray(4976)
        struct.pack_into('<I', raw, 0, 9000001)
        raw[5:5 + len('Generic unit') * 2] = 'Generic unit'.encode('utf-16-le')
        raw[612:612 + len('mdrs/generic.mod') * 2] = 'mdrs/generic.mod'.encode('utf-16-le')
        if malformed:
            raw[105:205] = b'X\0' * 50
            payload = struct.pack('<6I', 2, 9000001, 4976, fnv(raw), 0, 0) + raw
            content = struct.pack('<8s6I', b'SDGOUN1\0', 1, 32, 1, len(payload), fnv(payload), 0) + payload
        else:
            content = native_catalog([(2, 9000001, bytes(raw))])
        folder = self.client / 'sdgo/mods/unit_catalog'
        if preset:
            folder /= 'presets'
            folder.mkdir()
        (folder / 'catalog.bin').write_bytes(content)

    def test_invalid_legacy_record_reports_exact_location_and_preserves_all_files(self):
        self.catalog(malformed=True)
        before = {path: hashlib.sha256(path.read_bytes()).digest() for path in self.client.rglob('*') if path.is_file()}
        result = check(self.workspace, 'fixture')
        self.assertEqual('blocked', result['status'])
        self.assertFalse(result['changes_applied'])
        self.assertFalse(result['runtime_verified'])
        self.assertEqual((2, 9000001, 105, 50), tuple(result['issues'][0][k] for k in ('kind', 'id', 'offset', 'capacity')))
        self.assertNotIn('XXXXXXXX', result['issues'][0]['message'])
        self.assertEqual(before, {path: hashlib.sha256(path.read_bytes()).digest() for path in before})

    def test_valid_and_disabled_states_are_distinct_from_runtime_acceptance(self):
        self.catalog()
        result = check(self.workspace, 'fixture')
        self.assertEqual('ready', result['status'])
        self.assertEqual(1, result['dictionary']['entries'])
        path = self.client / 'sdgo/mods/shop_localization/mod.ini'
        path.write_bytes(storage.patch_ini(path.read_bytes(), [('settings', 'TranslateNames', '0')]))
        self.assertEqual('warning', check(self.workspace, 'fixture')['status'])

    def test_ai_handoff_carries_diagnosis_and_public_bug_summary_omits_content_identity(self):
        from manager.ai_context import build
        self.catalog(malformed=True)
        arguments = {'host_id': 'fixture', 'mod_id': 'shop_localization'}
        local = build(self.workspace, 'http://127.0.0.1:12345', **arguments)['text']
        public = build(self.workspace, 'http://127.0.0.1:12345', kind='bug', **arguments)['text']
        self.assertIn('9000001', local)
        self.assertIn('unterminated_display_text', local)
        self.assertIn('unterminated_display_text', public)
        self.assertNotIn('9000001', public)
        self.assertNotIn('XXXXXXXX', local + public)

    def test_both_scopes_and_container_checksums_are_verified(self):
        self.catalog()
        self.catalog(preset=True)
        self.assertEqual('invalid_catalog', check(self.workspace, 'fixture')['issues'][0]['code'])
        path = self.client / 'sdgo/mods/unit_catalog/presets/catalog.bin'
        path.write_bytes(path.read_bytes()[:-1])
        self.assertEqual('blocked', check(self.workspace, 'fixture')['status'])

    def test_dictionary_rejects_duplicates_and_reports_entry_line(self):
        self.assertEqual({'GENERIC UNIT': ('name', 'Generic translated unit')}, dictionary(WORDS))
        for bad in (WORDS + WORDS.replace(b'generic]', b'other]'),
                    WORDS.replace(b'Kind=name', b'Kind=unknown'),
                    WORDS.replace(b'Text=Generic translated unit', b'Text=bad\\q'),
                    WORDS.replace(b'Source=GENERIC UNIT', b'Source=GB'),
                    WORDS.replace(b'Text=Generic translated unit', b'Text=one\x00two')):
            with self.subTest(data=bad), self.assertRaisesRegex(ValueError, 'line=.*section='):
                dictionary(bad)
        values = dictionary(WORDS.replace(b'GENERIC UNIT', b'GENERIC\\sUNIT').replace(b'Text=Generic translated unit', b'Text=Line1\\nLine2'))
        self.assertEqual(('name', 'Line1\nLine2'), values['GENERIC UNIT'])

    def test_shipped_dictionary_obeys_the_same_format(self):
        self.assertGreater(len(dictionary((ROOT / 'mods/shop_localization/zh-CN.ini').read_bytes())), 18000)

    def test_blank_legacy_port_does_not_break_unrelated_dependency_checks_or_write_config(self):
        folder = self.client / 'sdgo/mods/net_route'
        folder.mkdir()
        for leaf in ('mod.ini', 'manager.json'):
            shutil.copyfile(ROOT / 'mods/net_route' / leaf, folder / leaf)
        before = (folder / 'mod.ini').read_bytes()
        for value in (None, '[settings]\nLoginPort=\n', '[settings]\nLoginPort=26000\n'):
            if value is not None:
                (folder / 'config.ini').write_text(value)
            node = next(row for row in self.workspace.dependency_nodes() if row['id'] == 'net_route')
            self.assertEqual(26000 if value and '26000' in value else 15001, node['settings']['login_port'])
            from manager.dependencies import enforce_install
            enforce_install(self.client, {}, 'client')
        self.assertEqual(before, (folder / 'mod.ini').read_bytes())


if __name__ == '__main__':
    unittest.main()
