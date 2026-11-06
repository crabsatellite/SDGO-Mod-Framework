"""Roster publisher helpers: server-side name folding, shared weapon rows and config adaptations."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.gunion.publisher.assemble import _ascii_name, _server_weapon
from tools.gunion.publisher.common import Config
from tools.gunion.publisher.resources import NATIVE_SUFFIX, UNSUPPORTED


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
