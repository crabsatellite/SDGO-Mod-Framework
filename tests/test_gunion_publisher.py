"""Roster publisher helpers: server-side name folding, shared weapon rows and config adaptations."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.gunion.publisher.assemble import _ascii_name, _server_weapon
from tools.gunion.publisher.common import Config
from tools.gunion.publisher.families import ENTRY, LINKED, ONE_WAY, SELF, SINGLE, _classify
from tools.gunion.publisher.resources import NATIVE_SUFFIX, UNSUPPORTED


class OverlayCarryTests(unittest.TestCase):
    def test_installed_overlay_entries_read_back_byte_identical(self):
        from owner.native_archive import build_archive, read_archive
        resources = {'mdrs/body.mod': bytes(range(256)) * 9, 'anrs/walk.ani': b'native animation',
                     'txrs/skin.txr': bytes(i % 251 for i in range(5000))}
        index, archive, _ = build_archive(resources, lambda _: True)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'resources.zpk'
            path.write_bytes(archive)
            self.assertEqual(resources, read_archive(index, path))
            path.write_bytes(archive[:-1])
            with self.assertRaises(ValueError):
                read_archive(index, path)

    def test_split_archive_parts_stay_below_the_part_size_and_read_back(self):
        from owner.native_archive import build_archive, build_archive_file, read_archive
        def noise(seed, size):  # incompressible bytes, so each entry spans several chunks
            return b''.join(hashlib.sha256(f'{seed}:{i}'.encode()).digest() for i in range(size // 32))
        resources = {'mdrs/a.mod': noise('a', 2400), 'anrs/b.ani': noise('b', 1600),
                     'txrs/c.txr': noise('c', 3200), 'sprs/d.spr': b'sprite'}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'resources.9.zpk').write_bytes(b'stale part')
            index, inventory = build_archive_file(resources, lambda _: True, root / 'resources.zpk', part_chunks=8)
            parts = inventory['parts']
            self.assertGreater(len(parts), 1)
            self.assertFalse((root / 'resources.9.zpk').exists())
            self.assertTrue(all(row['bytes'] == 8 * 520 for row in parts[:-1]))
            self.assertLessEqual(parts[-1]['bytes'], 8 * 520)
            joined = b''.join((root / row['name']).read_bytes() for row in parts)
            self.assertEqual(hashlib.sha256(joined).hexdigest(), inventory['archive_sha256'])
            for entry in inventory['entries']:
                self.assertEqual(entry['start_chunk'] // 8, (entry['start_chunk'] + entry['chunks'] - 1) // 8)
            self.assertEqual(resources, read_archive(index, root / 'resources.zpk', len(parts), 8))
            with self.assertRaises(ValueError):
                read_archive(index, root / 'resources.zpk')
            # Without parts the stream is byte-identical to the single-archive encoder.
            single_index, single = build_archive_file(resources, lambda _: True, root / 'single.zpk')
            self.assertNotIn('parts', single)
            expected_index, expected_archive, _ = build_archive(resources, lambda _: True)
            self.assertEqual((expected_index, expected_archive), (single_index, (root / 'single.zpk').read_bytes()))
            with self.assertRaises(ValueError):
                build_archive_file({'mdrs/big.mod': noise('big', 4000)}, lambda _: True, root / 'big.zpk', part_chunks=2)


class FamilyShapeTests(unittest.TestCase):
    def test_single_reciprocal_and_one_way_contexts(self):
        self.assertEqual((SINGLE, [11102], []), self.shape([11102], 11102, {11102: 0}))
        self.assertEqual((LINKED, [15261, 15262], []), self.shape([15261, 15262], 15261, {15261: 15262, 15262: 15261}))
        self.assertEqual((ONE_WAY, [15207, 27000], []), self.shape([15207, 27000], 15207, {15207: 27000, 27000: 0}))

    def test_self_transform_loop_is_a_native_profile(self):
        shape = _classify([25066], 25066, {25066: 25066})
        self.assertEqual((SELF, [25066], []), (shape['profile'], shape['forms'], shape['entries']))

    def test_entry_variants_carry_the_profile_of_their_target(self):
        linked = _classify([11307, 27099, 27100], 27099, {11307: 27099, 27099: 27100, 27100: 27099})
        self.assertEqual((LINKED, [27099, 27100], [11307], ENTRY),
                         (linked['profile'], linked['forms'], linked['entries'], linked['entry_profile']))
        one_way = _classify([26101, 26106, 26097, 26098], 26101, {26101: 26106, 26106: 0, 26097: 26106, 26098: 26106})
        self.assertEqual((ONE_WAY, [26101, 26106], [26097, 26098], ONE_WAY),
                         (one_way['profile'], one_way['forms'], one_way['entries'], one_way['entry_profile']))
        with self.assertRaises(ValueError):
            _classify([1, 2, 3], 1, {1: 2, 2: 3, 3: 1})

    @staticmethod
    def shape(members, root, transforms):
        value = _classify(members, root, transforms)
        return value['profile'], value['forms'], value['entries']


class ServerNameTests(unittest.TestCase):
    def test_varchar_names_are_folded_to_ascii_and_recorded(self):
        renamed = {}
        cases = {'LeonardoWon（AWAKEN）': 'LeonardoWon(AWAKEN)', 'HI-νGUNDAM(HWS)': 'HI-nuGUNDAM(HWS)',
                 "Gelgoog Char’s Custom[SS]": "Gelgoog Char's Custom[SS]", 'Sword Impulse Gundam SpecⅡ': 'Sword Impulse Gundam SpecII',
                 '00敢达Raiser(Burst Mode)': '00 Raiser(Burst Mode)'}
        for text, expected in cases.items():
            self.assertEqual(expected, _ascii_name(text, 'FALLBACK', renamed, text))
        self.assertEqual(set(cases), set(renamed))
        self.assertEqual('UNION_U_26001', _ascii_name('光束军刀', 'UNION_U_26001'))
        untouched = {}
        self.assertEqual('00 QAN[T] FULL SABER', _ascii_name('00 QAN[T] FULL SABER', 'x', untouched, 'k'))
        self.assertEqual({}, untouched)

    def test_server_weapon_rows_keep_ascii_names_and_rename_the_rest(self):
        renamed = {}
        kept = _server_weapon({'id': 9000001, 'values': {'WeaponNo': 9000001, 'WeaponName': 'UNION_W_9000001', 'DamageSize': 1}}, renamed)
        self.assertEqual('UNION_W_9000001', kept['WeaponName'])
        folded = _server_weapon({'id': 29908, 'values': {'WeaponNo': 29908, 'WeaponName': '光束军刀'}}, renamed)
        self.assertEqual('UNION_W_29908', folded['WeaponName'])
        self.assertEqual({29908: '光束军刀'}, renamed)


class ConfigTests(unittest.TestCase):
    def test_reviewed_adaptations_are_read_from_the_private_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('delivery/native', 'plan', 'client', 'out'):
                (root / name).mkdir(parents=True)
            config = root / 'config.json'
            config.write_text(json.dumps({
                'delivery_root': str(root / 'delivery'), 'relocation_plan_dir': str(root / 'plan'),
                'baseline_client': str(root / 'client'), 'output_root': str(root / 'out'),
                'item_templates_by_grade': {'4': 1120443},
                'script_adaptations': {'2647.scp': [{'find': 'for i = 1, #attack_params do',
                                                     'replace': 'for i = 1, table.getn(attack_params) do', 'count': 1}]},
                'callback_substitutions': {'Class3HomingMisaileInit0': 'Class3HomingMisaileInit', 'chuangsini': ''}}),
                encoding='utf-8')
            value = Config(config)
            self.assertEqual({4: 1120443}, value.item_templates)
            self.assertEqual(['2647.scp'], list(value.script_adaptations))
            self.assertEqual({'Class3HomingMisaileInit0': 'Class3HomingMisaileInit', 'chuangsini': ''}, value.callback_substitutions)

    def test_native_suffix_table_covers_every_overlay_root(self):
        self.assertEqual({'mdrs': '.mod', 'anrs': '.ani', 'txrs': '.txr', 'fxrs': '.zfx', 'sprs': '.spr', 'sdrs': '.zsd'}, NATIVE_SUFFIX)
        self.assertEqual('unsupported-native-type', UNSUPPORTED)


if __name__ == '__main__':
    unittest.main()
