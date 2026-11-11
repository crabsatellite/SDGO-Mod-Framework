import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from manager.engines.capsules import capsule_acquisition as hints
from owner.units.compiler import native_catalog
from test_capsule_client_runtime import write_bundle, harness


def item(category, unit, disabled=False):
    row = bytearray(614)
    struct.pack_into('<I', row, 0, category)
    row[4] = int(disabled)
    struct.pack_into('<I', row, 105, unit)
    return bytes(row)


def activity(mid=15401, category=1119001, **changes):
    return {'id': mid, 'name': 'Owner event',
            'pool': [{'category_id': category, 'amount': 1, 'entries': 20}], **changes}


class CapsuleAcquisitionTests(unittest.TestCase):
    def test_native_visible_numbers_are_not_database_or_grid_keys(self):
        self.assertEqual(hints.display_number(15401, 'activity'), 154)
        self.assertEqual(hints.display_number(9301, 'activity'), 93)
        self.assertEqual(hints.display_number(101, 'fixed'), 1)
        self.assertEqual(hints.display_number(2, 'custom'), 2)
        for mid in (990001, 1990001, 1990999, 0, True):
            self.assertIsNone(hints.display_number(mid, 'activity'))

    def test_activity_pool_includes_all_unit_rewards_not_only_featured_root(self):
        machine = activity()
        machine['pool'] += [{'category_id': 1119002, 'amount': 1, 'entries': 5},
                            {'category_id': 2220001, 'amount': 3, 'entries': 100},
                            {'category_id': 1119003, 'amount': 1, 'entries': 0}]
        catalog = {'activity_machines': [machine], 'activity_drafts': [activity(15501, 1119003)]}
        _, meta = hints.encode(catalog, [item(1119001, 30001), item(1119002, 30002), item(1119003, 30003)])
        self.assertEqual([r['unit_id'] for r in meta['entries']], [30001, 30002])
        self.assertEqual(meta['entries'][0]['text'], '活动扭蛋机154号')
        self.assertNotIn('15401', meta['entries'][0]['text'])
        disabled = copy.deepcopy(catalog)
        disabled['activity_machines'][0]['enabled'] = False
        self.assertEqual(hints.encode(disabled, [item(1119001, 30001)])[1]['unit_count'], 0)

    def test_add_remove_and_renumber_rebuild_sources_without_reimport(self):
        rows = [item(1119001, 30001)]
        catalog = {'activity_machines': [activity(), activity(15501)]}
        raw, meta = hints.encode(catalog, rows)
        self.assertEqual(meta['entries'][0]['text'], '活动扭蛋机154–155号')
        self.assertEqual(hints.encode({'activity_machines': list(reversed(catalog['activity_machines']))}, rows)[0], raw)
        catalog['activity_machines'] = [activity(16001)]
        _, meta = hints.encode(catalog, rows)
        self.assertEqual(meta['entries'][0]['text'], '活动扭蛋机160号')
        self.assertEqual(hints.encode({'activity_machines': []}, rows)[1]['entries'], [])

    def test_native_ordinary_sources_are_not_duplicated_and_custom_numbers_are_explicit(self):
        catalog = {'activity_machines': [activity(), activity(1990001, name='Season event')],
                   'fixed_machines': [
                       {'id': 101, 'enabled': True, 'currency': 'GP', 'slots': [{'unit_id': 30001, 'weight': 1000}]},
                       {'id': 201, 'enabled': True, 'currency': 'TOKEN', 'slots': [{'unit_id': 30001, 'weight': 1000}]},
                       {'id': 301, 'enabled': False, 'currency': 'GP', 'slots': [{'unit_id': 30001}]}],
                   'custom_machines': [{'id': 2, 'enabled': True,
                       'candidates': [{'unit_id': 30001, 'basic_weight': 1}]}]}
        _, meta = hints.encode(catalog, [item(1119001, 30001)])
        entry = meta['entries'][0]
        for expected in ('活动扭蛋机154号', 'Season event', '自定义扭蛋机2号'):
            self.assertIn(expected, entry['text'])
        self.assertNotIn('1990001', entry['text'])
        for label in ('普通扭蛋机', '代币扭蛋机'):
            self.assertNotIn(label, entry['text'])
        custom = next(source for source in entry['sources'] if source['kind'] == 'custom')
        self.assertEqual((custom['machine_id'], custom['display_number']), (2, 2))
        self.assertNotIn('自定义扭蛋机', entry['legacy_text'])

    def test_custom_add_remove_disable_and_renumber_rebuild_sources(self):
        def custom(mid, *units, **changes):
            return {'id': mid, 'name': f'Custom {mid}', 'enabled': True,
                    'candidates': [{'unit_id': unit, 'basic_weight': 1} for unit in units],
                    **changes}

        catalog = {'custom_machines': [custom(1, 30001), custom(2, 30001, 30002)]}
        raw, meta = hints.encode(catalog, [])
        self.assertEqual(meta['entries'][0]['text'], '自定义扭蛋机1–2号')
        self.assertEqual(meta['entries'][1]['text'], '自定义扭蛋机2号')
        self.assertEqual([entry['legacy_text'] for entry in meta['entries']], ['', ''])
        self.assertEqual(hints.encode({'custom_machines': list(reversed(catalog['custom_machines']))}, [])[0], raw)
        _, changed = hints.encode({'custom_machines': [custom(7, 30001)]}, [])
        self.assertEqual(changed['entries'][0]['text'], '自定义扭蛋机7号')
        self.assertEqual(hints.encode({'custom_machines': [custom(7, 30001, enabled=False)]}, [])[1]['entries'], [])
        self.assertEqual(hints.encode({'custom_machines': [custom(7, 30001, retired=True)]}, [])[1]['entries'], [])

    def test_real_runtime_custom_only_source_uses_shared_display_without_legacy_patch(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_bundle(root)
            catalog = {'custom_machines': [{'id': 7, 'enabled': True,
                'candidates': [{'unit_id': 30001, 'basic_weight': 1}]}]}
            raw, meta = hints.encode(catalog, [])
            self.assertEqual(meta['entries'][0]['text'], '自定义扭蛋机7号')
            self.assertEqual(meta['entries'][0]['legacy_text'], '')
            (root / hints.FILE).write_bytes(raw)
            with (root / 'runtime.ini').open('a') as stream:
                stream.write('\n[capsule_acquisition]\nfile=' + hints.FILE +
                             '\nsha256=' + hashlib.sha256(raw).hexdigest() + '\n')
            result = subprocess.run([str(harness()), str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('CAPSULE_ACQUISITION 1', result.stdout)
            self.assertIn('ACQUISITION_SOURCES 1', result.stdout)

    def test_complete_display_text_is_retained_beyond_the_legacy_record_limit(self):
        machines = [activity(1990000 + index, name=f'Season source {index:02d}')
                    for index in range(1, 21)]
        raw, meta = hints.encode({'activity_machines': machines}, [item(1119001, 30001)])
        entry = meta['entries'][0]
        self.assertGreater(hints._characters(entry['text']), 100)
        self.assertLess(hints._characters(entry['brief']), 100)
        display_chars, full_chars, brief_chars = struct.unpack_from('<III', raw, 20)
        self.assertEqual(display_chars, hints._characters(entry['text']))
        self.assertEqual(full_chars, hints._characters(entry['legacy_text']))
        self.assertEqual(brief_chars, hints._characters(entry['brief']))
        self.assertLess(full_chars, 100)

    def test_imported_item_protocol_is_verified_and_supplies_unit_rewards(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root / 'sdgo/mods/unit_catalog/catalog.bin'
            path.parent.mkdir(parents=True)
            raw = native_catalog([(34, 1119001, item(1119001, 30001))])
            path.write_bytes(raw)
            rows, sha = hints.imported_item_rows(root)
            self.assertEqual(sha, hashlib.sha256(raw).hexdigest())
            self.assertEqual(hints.encode({'activity_machines': [activity()]}, rows)[1]['entries'][0]['unit_id'], 30001)
            path.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
            with self.assertRaisesRegex(ValueError, 'checksum'):
                hints.imported_item_rows(root)

    def test_real_runtime_decoder_applies_hints_and_rejects_modified_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_bundle(root)
            raw, _ = hints.encode({'activity_machines': [activity()]}, [item(1119001, 30001)])
            (root / hints.FILE).write_bytes(raw)
            with (root / 'runtime.ini').open('a') as stream:
                stream.write('\n[capsule_acquisition]\nfile=' + hints.FILE + '\nsha256=' + hashlib.sha256(raw).hexdigest() + '\n')
            result = subprocess.run([str(harness()), str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('CAPSULE_ACQUISITION 1', result.stdout)
            (root / hints.FILE).write_bytes(raw + b'changed')
            result = subprocess.run([str(harness()), str(root)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SHA-256 mismatch', result.stdout)


    def test_real_runtime_decoder_exposes_long_text_but_uses_the_legacy_brief_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_bundle(root)
            machines = [activity(1990000 + index, name=f'Season source {index:02d}')
                        for index in range(1, 21)]
            raw, meta = hints.encode({'activity_machines': machines}, [item(1119001, 30001)])
            self.assertGreater(hints._characters(meta['entries'][0]['text']), 100)
            (root / hints.FILE).write_bytes(raw)
            with (root / 'runtime.ini').open('a') as stream:
                stream.write('\n[capsule_acquisition]\nfile=' + hints.FILE + '\nsha256=' + hashlib.sha256(raw).hexdigest() + '\n')
            result = subprocess.run([str(harness()), str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('CAPSULE_ACQUISITION 1', result.stdout)
            self.assertIn('ACQUISITION_SOURCES 1', result.stdout)


if __name__ == '__main__':
    unittest.main()
