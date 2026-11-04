"""Independent Mod symbols must remain independent during native preparation."""
from pathlib import Path
import tempfile
import unittest
from tools import game_test

HOST='[host]\nimage=WorldServer.exe\ntimestamp=0x548A86E3\nsize_of_image=0x0010C000\n'

class ModSymbolPreparationTests(unittest.TestCase):
    def test_client_preflight_accepts_only_its_own_matching_sidecar(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);client=root/'client';stage=root/'stage'
            host=HOST.replace('WorldServer.exe','GOnline.exe')
            host=host.replace('0x548A86E3','0x54BE1568').replace('0x0010C000','0x00C97000')
            for base in (client,stage/'client'):
                (base/'sdgo/symbols').mkdir(parents=True)
                (base/'sdgo/symbols'/f'{game_test.HOST_KEY}.ini').write_text(host)
            observer=stage/'client/sdgo/mods/game_test_client';observer.mkdir(parents=True)
            (observer/'mod.ini').write_text('[mod]\nid=game_test_client\nrequires=observer.local\n')
            (observer/'game_test_client.dll').write_bytes(b'fixture observer')
            fragment=observer/'symbols.fragment.ini'
            fragment.write_text(host+'[observer.local]\nkind=code\nrva=0x1000\n')
            (stage/'client/sdgo/core.dll').write_bytes(b'fixture core')
            target=client/'sdgo/mods/match_ai';target.mkdir(parents=True)
            (target/'mod.ini').write_text('[mod]\nid=match_ai\n')
            (target/'match_ai.dll').write_bytes(b'fixture target')
            game_test.validate_observer_stage(client,stage)
            fragment.write_text(host.replace('0x54BE1568','0x54BE1569')+'[observer.local]\nkind=code\nrva=0x1000\n')
            with self.assertRaisesRegex(game_test.GameTestError,'does not match'):
                game_test.validate_observer_stage(client,stage)

    def test_complete_backend_map_supplies_dynamic_host_aliases_and_retains_local_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);stage=root/'stage.ini';copied=root/'copied.ini'
            stage.write_text(HOST+'[gift.world_route]\nkind=code\nrva=0x1000\n')
            copied.write_text(HOST+'[owner.existing]\nkind=code\nrva=0x2000\n')
            game_test.merge_required_symbols(stage,copied,[],include_all=True)
            self.assertEqual(set(game_test.read_ini(copied).sections()),{'host','gift.world_route','owner.existing'})

    def test_shipped_shared_maps_have_no_duplicate_symbol_sections(self):
        root=Path(__file__).resolve().parents[1]
        for path in (root/'symbols').glob('*.ini'):
            with self.subTest(map=path.name):
                self.assertIn('host',game_test.read_ini(path))

    def test_matching_sidecar_satisfies_only_its_own_mod_and_stays_local(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);stage=root/'stage.ini';copied=root/'copied.ini'
            stage.write_text(HOST)
            mod=root/'login';mod.mkdir();manifest=mod/'mod.ini'
            manifest.write_text('[mod]\nrequires=login.request\n')
            (mod/'symbols.fragment.ini').write_text(HOST+'[login.request]\nkind=code\nrva=0x00059EE0\nexpect=55 8B EC\n')
            game_test.merge_required_symbols(stage,copied,[manifest])
            self.assertEqual(set(game_test.read_ini(copied).sections()),{'host'})
            other=root/'other';other.mkdir();other_manifest=other/'mod.ini'
            other_manifest.write_text('[mod]\nrequires=login.request\n')
            with self.assertRaisesRegex(game_test.GameTestError,'lack required sections'):
                game_test.merge_required_symbols(stage,copied,[manifest,other_manifest])

    def test_another_host_or_missing_local_alias_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);stage=root/'stage.ini';copied=root/'copied.ini';manifest=root/'mod.ini'
            stage.write_text(HOST);manifest.write_text('[mod]\nrequires=login.request\n')
            fragment=root/'symbols.fragment.ini'
            fragment.write_text(HOST.replace('0x548A86E3','0x548A86E4')+'[login.request]\nkind=code\nrva=0x00059EE0\n')
            with self.assertRaisesRegex(game_test.GameTestError,'does not match'):
                game_test.merge_required_symbols(stage,copied,[manifest])
            fragment.write_text(HOST+'[different.alias]\nkind=code\nrva=0x00059EE0\n')
            with self.assertRaisesRegex(game_test.GameTestError,'lack required sections'):
                game_test.merge_required_symbols(stage,copied,[manifest])

if __name__=='__main__':unittest.main()
