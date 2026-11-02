from pathlib import Path
import tempfile
import stat
import unittest
from unittest import mock

from tools import game_test as gt
from tools import game_test_native_environment as native


class ServerCandidateTests(unittest.TestCase):
    def test_snapshot_upload_streams_exact_bytes_and_rejects_a_container_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            source=Path(temporary)/'snapshot.bak'
            source.write_bytes(b'\x00SQL snapshot\xff\n'*1024)
            expected=gt.sha256(source)
            uploaded=[]
            def transfer(*args,**kwargs):
                uploaded.append(kwargs['stdin'].read())
                self.assertEqual(kwargs['timeout'],600)
                return mock.Mock(returncode=0)
            with mock.patch.object(native.subprocess,'run',side_effect=transfer), mock.patch.object(native,'docker',return_value=expected+'  snapshot.bak'):
                native.upload_snapshot('owned-fixture',source,'/owned/snapshot.bak',expected)
            self.assertEqual(uploaded,[source.read_bytes()])
            with mock.patch.object(native.subprocess,'run',side_effect=transfer), mock.patch.object(native,'docker',return_value='0'*64+'  snapshot.bak'):
                with self.assertRaisesRegex(RuntimeError,'snapshot bytes differ'):
                    native.upload_snapshot('owned-fixture',source,'/owned/snapshot.bak',expected)

    def test_runtime_copy_is_writable_while_the_template_stays_read_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); source=root/'template'; source.mkdir()
            original=source/'symbols.ini'; original.write_text('sealed input',encoding='utf-8');original.chmod(stat.S_IRUSR)
            try:
                native.copy_tree(source,root/'owned-copy')
                self.assertFalse(original.stat().st_mode & stat.S_IWUSR)
                copied=root/'owned-copy/symbols.ini'
                self.assertTrue(copied.stat().st_mode & stat.S_IWUSR)
                self.assertEqual(copied.read_bytes(),original.read_bytes())
            finally:original.chmod(stat.S_IRUSR|stat.S_IWUSR)

    def test_overlay_preserves_private_configuration_and_unrelated_hosts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); stage=root/'stage'; instance=root/'instance'
            source=stage/'server/sdgo/mods/candidate_fix'; source.mkdir(parents=True)
            (source/'mod.ini').write_text('[mod]\nid=candidate_fix\nversion=2\nhosts=BattleServer-fixture\ncategory=gameplay\nfeature=room-titles\nenabled=0\nrequires=candidate.hook\nconfig_files=config.ini\n',encoding='utf-8')
            (source/'candidate_fix.dll').write_bytes(b'candidate DLL fixture')
            (source/'config.ini').write_text('package default',encoding='utf-8')
            symbols=stage/'server/sdgo/symbols'; symbols.mkdir()
            (symbols/'BattleServer-fixture.ini').write_text('[host]\ntimestamp=1\n[candidate.hook]\nkind=code\nrva=1\n',encoding='utf-8')
            for folder,exe in (('BattleServer2','BattleServer.exe'),('WorldServer','WorldServer.exe')):
                host=instance/'Server'/folder; (host/'sdgo/symbols').mkdir(parents=True)
                (host/exe).write_bytes(b'original native fixture; never executed')
                (host/'sdgo/sdgo.ini').write_text('[mods]\nunrelated=1\n',encoding='utf-8')
            destination=instance/'Server/BattleServer2/sdgo/mods/candidate_fix'; destination.mkdir(parents=True)
            (destination/'mod.ini').write_text('[mod]\nenabled=0\n[settings]\nKeep=owned value\n',encoding='utf-8')
            (destination/'config.ini').write_text('owned configuration',encoding='utf-8')
            with mock.patch('tools.verify_symbols.read_pe',return_value=None), mock.patch('tools.verify_symbols.host_key',side_effect=lambda name,_:Path(name).stem+'-fixture'):
                records=native.overlay_server_candidates(instance,stage,['candidate_fix'])
            self.assertEqual([r['host'] for r in records],['BattleServer2'])
            self.assertEqual((destination/'candidate_fix.dll').read_bytes(),b'candidate DLL fixture')
            self.assertEqual((destination/'config.ini').read_text(),'owned configuration')
            self.assertEqual(gt.read_ini(destination/'mod.ini').get('settings','Keep'),'owned value')
            self.assertEqual(gt.read_ini(destination/'mod.ini').get('mod','enabled'),'1')
            self.assertEqual(gt.read_ini(instance/'Server/BattleServer2/sdgo/sdgo.ini').get('mods','unrelated'),'1')
            self.assertTrue(gt.read_ini(instance/'Server/BattleServer2/sdgo/symbols/BattleServer-fixture.ini').has_section('candidate.hook'))
            self.assertFalse((instance/'Server/WorldServer/sdgo/mods/candidate_fix').exists())
            self.assertEqual((instance/'Server/BattleServer2/BattleServer.exe').read_bytes(),b'original native fixture; never executed')
            self.assertEqual(records[0]['files']['candidate_fix.dll'],gt.sha256(destination/'candidate_fix.dll'))


if __name__=='__main__':unittest.main()
