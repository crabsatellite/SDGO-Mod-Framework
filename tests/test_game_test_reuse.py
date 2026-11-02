"""Owned client reuse: no native processes, server, credentials or desktop."""
from pathlib import Path
import json
import os
import sys
import types
import unittest
from unittest import mock
import uuid

import test_game_test as fixtures
game_test=fixtures.game_test


class ReuseOwnedClientTests(unittest.TestCase):
    def setUp(self):
        self.fixture=fixtures.GameTestRunnerTests('runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.addCleanup(self.fixture.tearDown)
        self.base=self.fixture.root
        self.bin=self.base/'fake-recycle-bin';self.bin.mkdir()
        self.recycled=[]
        def recycle(value):
            path=Path(value);self.recycled.append(path)
            path.rename(self.bin/(uuid.uuid4().hex+'-'+path.name))
        patches=[mock.patch.object(game_test,'OWNED_BUILD_ROOT',self.base),
                 mock.patch.object(game_test,'process_snapshot',return_value=[]),
                 mock.patch.dict(sys.modules,{'send2trash':types.SimpleNamespace(send2trash=recycle)})]
        for patch in patches:patch.start();self.addCleanup(patch.stop)

    def test_fresh_capsule_moves_same_file_and_archives_prior_evidence(self):
        run=self.fixture.prepare('current');old=self.fixture.metadata(run)
        game=run/'client/GOnline.exe';identity=game.stat().st_ino
        event=Path(old['client']['events']);event.parent.mkdir(parents=True,exist_ok=True)
        event.write_text('old native evidence\n',encoding='utf-8')
        # A same-file move is essential; copytree is forbidden in this path.
        original_copytree=game_test.shutil.copytree
        def no_full_client_copy(source,destination,*args,**kwargs):
            self.assertNotEqual(Path(source).resolve(),self.fixture.template.resolve())
            return original_copytree(source,destination,*args,**kwargs)
        with mock.patch.object(game_test.shutil,'copytree',side_effect=no_full_client_copy):
            new=game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run,mode_sequence='3')
        self.assertNotEqual(old['run_id'],new['run_id'])
        self.assertEqual(identity,game.stat().st_ino)
        self.assertEqual('3',new['plan']['mode_sequence'])
        self.assertEqual(old['run_id'],new['reuse']['previous_run_id'])
        self.assertIsNone(new['execution'])
        self.assertFalse(event.exists())
        self.assertEqual('old native evidence\n',game_test.read_json(self.base/'gametest-current/previous-run.json')['events'])
        self.assertTrue(any(p.name=='gametest' for p in self.recycled))
        self.assertFalse((run/'.run.lock').exists())
        self.assertFalse(game_test.account_profile_lock_path(self.fixture.account_profile).exists())
        game_test.verify_hashes(game_test.validate_metadata(run))

    def test_running_client_refuses_before_any_move(self):
        run=self.fixture.prepare('active');before=(run/'run.json').read_bytes()
        row={'pid':42,'image_path':str(run/'client/GOnline.exe')}
        with mock.patch.object(game_test,'process_snapshot',return_value=[row]):
            with self.assertRaisesRegex(game_test.GameTestError,'running'):
                game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertEqual(before,(run/'run.json').read_bytes());self.assertFalse(self.recycled)

    def test_referenced_archive_baseline_is_protected(self):
        run=self.fixture.prepare('baseline');other=self.base/'another';other.mkdir()
        (other/'run.json').write_text(json.dumps({'assets':{'baseline':str(run)}}))
        with self.assertRaisesRegex(game_test.GameTestError,'baseline'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertTrue((run/'client/GOnline.exe').exists());self.assertFalse(self.recycled)

    def test_tampered_payload_and_player_directory_are_rejected(self):
        with self.assertRaisesRegex(game_test.GameTestError,'owned build'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,self.base.parent/'player')
        run=self.fixture.prepare('changed');(run/'client/GOnline.exe').write_bytes(b'changed')
        with self.assertRaisesRegex(game_test.GameTestError,'changed'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertFalse(self.recycled)

    def test_invalid_stage_restores_complete_previous_capsule(self):
        run=self.fixture.prepare('recoverable');old=(run/'run.json').read_bytes()
        with self.assertRaises(game_test.GameTestError):
            game_test.reuse_prepared(self.fixture.template,self.base/'missing-stage',run)
        self.assertEqual(old,(run/'run.json').read_bytes())
        game_test.verify_hashes(game_test.validate_metadata(run))
        self.assertFalse((run/'.run.lock').exists())

    def test_shared_mutable_file_is_never_overwritten(self):
        run=self.fixture.prepare('shared');original=self.base/'original-core.dll'
        os.link(run/'client/sdgo/core.dll',original)
        with self.assertRaisesRegex(game_test.GameTestError,'shared mutable'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertEqual(original.read_bytes(),(run/'client/sdgo/core.dll').read_bytes())
        self.assertFalse(self.recycled)

    def test_stale_stage_symbols_cannot_consume_the_existing_client_or_evidence(self):
        run=self.fixture.prepare('stale-stage');old=(run/'run.json').read_bytes()
        event=run/'client/sdgo/gametest/events.jsonl';event.write_text('keep original evidence\n')
        identity=(run/'client/GOnline.exe').stat().st_ino
        symbols=self.fixture.stage/f'client/sdgo/symbols/{game_test.HOST_KEY}.ini'
        ini=game_test.read_ini(symbols);ini.remove_section('test.client_update');game_test.write_ini(symbols,ini)
        with self.assertRaisesRegex(game_test.GameTestError,'stage host symbols lack'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertEqual(old,(run/'run.json').read_bytes())
        self.assertEqual(identity,(run/'client/GOnline.exe').stat().st_ino)
        self.assertEqual(event.read_text(),'keep original evidence\n')
        self.assertFalse(self.recycled)
        self.assertFalse(list(self.base.glob('.gametest-*')))
        game_test.verify_hashes(game_test.validate_metadata(run))

    def test_linked_player_directory_cannot_be_validated_or_reused(self):
        run=self.fixture.prepare('linked');client=run/'client';saved=run/'owned-original'
        client.rename(saved)
        try:client.symlink_to(saved,target_is_directory=True)
        except OSError as error:self.skipTest(str(error))
        with self.assertRaisesRegex(game_test.GameTestError,'linked'):
            game_test.validate_metadata(run)
        with self.assertRaisesRegex(game_test.GameTestError,'linked'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertTrue((saved/'GOnline.exe').exists());self.assertFalse(self.recycled)


if __name__=='__main__':unittest.main()
