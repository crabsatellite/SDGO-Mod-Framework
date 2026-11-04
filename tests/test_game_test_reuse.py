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

    def existing_peer(self,state='existing_account'):
        path=self.base/'private/peer.ini';path.parent.mkdir(exist_ok=True)
        config=game_test.read_ini(self.fixture.account_profile)
        config['account']['username']='gtPeer0001'
        if not config.has_section('registration'):config.add_section('registration')
        config['registration']['state']=state
        game_test.write_ini(path,config)
        return path

    def test_explicit_rebind_reuses_same_client_and_preserves_both_account_profiles(self):
        run=self.fixture.prepare('account-rebind');peer=self.existing_peer()
        old_profile=self.fixture.account_profile.read_bytes();peer_profile=peer.read_bytes()
        identity=(run/'client/GOnline.exe').stat().st_ino
        with mock.patch.object(game_test,'create_account_profile',side_effect=AssertionError('must not register')):
            result=game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run,
                account_profile=peer,rebind_existing_account=True)
        self.assertEqual(identity,(run/'client/GOnline.exe').stat().st_ino)
        self.assertTrue(result['reuse']['rebound_existing_account'])
        self.assertEqual(game_test.validate_private_account(run/'client',result['plan']),game_test.account_profile_lock_path(peer))
        self.assertEqual(old_profile,self.fixture.account_profile.read_bytes())
        self.assertEqual(peer_profile,peer.read_bytes())
        for profile in (self.fixture.account_profile,peer):self.assertFalse(game_test.account_profile_lock_path(profile).exists())
        game_test.verify_hashes(game_test.validate_metadata(run))

    @unittest.skipUnless(os.name == 'nt', 'Windows short-name aliases')
    def test_short_and_long_profile_names_share_one_exclusive_account_lock(self):
        import ctypes
        from ctypes import wintypes
        peer=self.existing_peer()
        native=ctypes.WinDLL('kernel32',use_last_error=True)
        native.GetShortPathNameW.argtypes=(wintypes.LPCWSTR,wintypes.LPWSTR,wintypes.DWORD)
        native.GetShortPathNameW.restype=wintypes.DWORD
        buffer=ctypes.create_unicode_buffer(32768)
        size=native.GetShortPathNameW(str(peer),buffer,len(buffer))
        self.assertTrue(size and size<len(buffer))
        alias=Path(buffer.value)
        if str(alias).casefold()==str(peer.resolve()).casefold():
            self.skipTest('volume does not expose a distinct short path')
        self.assertEqual(peer.resolve(),alias.resolve())
        long_lock=game_test.account_profile_lock_path(peer.resolve())
        self.assertEqual(long_lock,game_test.account_profile_lock_path(alias))
        lock,token=game_test.acquire_exclusive_lock(long_lock,'long account path')
        try:
            with self.assertRaises(game_test.GameTestError):
                game_test.acquire_exclusive_lock(game_test.account_profile_lock_path(alias),'short account alias')
        finally:
            game_test.release_owned_lock(lock,token)

    def test_profile_parent_alias_returns_canonical_exclusive_lock(self):
        peer=self.existing_peer()
        alias=peer.parent/'..'/peer.parent.name/peer.name
        expected=peer.resolve().with_name(peer.name+'.run.lock')
        self.assertEqual(expected,game_test.account_profile_lock_path(alias))
        lock,token=game_test.acquire_exclusive_lock(expected,'canonical account')
        try:
            with self.assertRaises(game_test.GameTestError):
                game_test.acquire_exclusive_lock(game_test.account_profile_lock_path(alias),'parent alias')
        finally:
            game_test.release_owned_lock(lock,token)

    def test_another_profile_still_requires_an_explicit_rebind(self):
        run=self.fixture.prepare('no-implicit-switch');old=(run/'run.json').read_bytes()
        with self.assertRaisesRegex(game_test.GameTestError,'explicit existing-account'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run,account_profile=self.existing_peer())
        self.assertEqual(old,(run/'run.json').read_bytes());self.assertFalse(self.recycled)

    def test_rebinding_cannot_create_an_account_or_use_registration_retries(self):
        for state,options in (('pending_first_login',{}),('existing_account',{'retry_count':2}),
                              ('existing_account',{'expect_first_fail':True})):
            with self.subTest(state=state,options=options):
                run=self.fixture.prepare('no-registration-'+str(len(list(self.base.glob('no-registration-*')))))
                old=(run/'run.json').read_bytes()
                with self.assertRaisesRegex(game_test.GameTestError,'already registered'):
                    game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run,
                        account_profile=self.existing_peer(state),rebind_existing_account=True,**options)
                self.assertEqual(old,(run/'run.json').read_bytes());self.assertFalse(self.recycled)

    def test_busy_rebind_target_leaves_previous_client_intact_and_releases_its_lock(self):
        run=self.fixture.prepare('busy-peer');peer=self.existing_peer();old=(run/'run.json').read_bytes()
        held,token=game_test.acquire_exclusive_lock(game_test.account_profile_lock_path(peer),'existing peer')
        try:
            with self.assertRaises(game_test.GameTestError):
                game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run,
                    account_profile=peer,rebind_existing_account=True)
            self.assertEqual(old,(run/'run.json').read_bytes());self.assertFalse(self.recycled)
            self.assertFalse((run/'.run.lock').exists())
            self.assertFalse(game_test.account_profile_lock_path(self.fixture.account_profile).exists())
            self.assertTrue(held.exists())
        finally:game_test.release_owned_lock(held,token)

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
        self.assertEqual((run/'client/d3d9.dll').read_bytes(),b'normal-loader')
        self.assertEqual('old native evidence\n',game_test.read_json(self.base/'gametest-current/previous-run.json')['events'])
        self.assertTrue(any(p.name=='gametest' for p in self.recycled))
        self.assertFalse((run/'.run.lock').exists())
        self.assertFalse(game_test.account_profile_lock_path(self.fixture.account_profile).exists())
        game_test.verify_hashes(game_test.validate_metadata(run))

    def test_explicit_staged_loader_binding_replaces_only_the_owned_copy(self):
        run=self.fixture.prepare('staged-loader')
        source_loader=(self.fixture.template/'d3d9.dll').read_bytes()
        stage_loader=(self.fixture.stage/'client/d3d9.dll').read_bytes()
        self.assertNotEqual(source_loader,stage_loader)

        metadata=game_test.reuse_prepared(
            self.fixture.template,
            self.fixture.stage,
            run,
            bind_staged_loader=True,
        )

        self.assertEqual((run/'client/d3d9.dll').read_bytes(),stage_loader)
        self.assertEqual((self.fixture.template/'d3d9.dll').read_bytes(),source_loader)
        self.assertEqual(
            metadata['client']['immutable_hashes']['d3d9.dll'],
            game_test.sha256(self.fixture.stage/'client/d3d9.dll'),
        )
        game_test.verify_hashes(game_test.validate_metadata(run))

    def test_running_client_refuses_before_any_move(self):
        run=self.fixture.prepare('active');before=(run/'run.json').read_bytes()
        row={'pid':42,'image_path':str(run/'client/GOnline.exe')}
        with mock.patch.object(game_test,'process_snapshot',return_value=[row]):
            with self.assertRaisesRegex(game_test.GameTestError,'running'):
                game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertEqual(before,(run/'run.json').read_bytes());self.assertFalse(self.recycled)

    def test_retired_case_contract_is_reused_but_cannot_run_as_a_current_contract(self):
        current=game_test.required_flow_cases('1',room_titles_only=True)
        historical=current[:-3]
        with mock.patch.object(game_test,'required_flow_cases',return_value=historical):
            run=self.fixture.prepare('historical',room_titles_only=True)
        old=self.fixture.metadata(run)
        game=run/'client/GOnline.exe';identity=game.stat().st_ino
        with self.assertRaisesRegex(game_test.GameTestError,'required normal client flow'):
            game_test.validate_metadata(run)
        new=game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run,room_titles_only=True)
        self.assertEqual(game.stat().st_ino,identity)
        self.assertEqual(new['reuse']['previous_run_id'],old['run_id'])
        self.assertEqual(game_test.read_ini(run/'client/sdgo/gametest/run.ini')['run']['required_cases'],','.join(current))
        archived=game_test.read_json(self.base/'gametest-current/previous-run.json')
        self.assertEqual(archived['required_cases'],list(historical))
        game_test.verify_hashes(game_test.validate_metadata(run))

    def test_referenced_archive_baseline_is_protected(self):
        run=self.fixture.prepare('baseline');other=self.base/'another';other.mkdir()
        (other/'run.json').write_text(json.dumps({'assets':{'baseline':str(run)}}))
        with self.assertRaisesRegex(game_test.GameTestError,'baseline'):
            game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        self.assertTrue((run/'client/GOnline.exe').exists());self.assertFalse(self.recycled)

    def test_nested_friend_campaign_also_protects_its_archive_baseline(self):
        run=self.fixture.prepare('baseline')
        actor=self.base/'friends/campaign/a-01';actor.mkdir(parents=True)
        (actor/'run.json').write_text(json.dumps({'assets':{'baseline':str(run)}}))
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

    def test_reuse_updates_the_matching_sidecar_without_exporting_its_aliases(self):
        run=self.fixture.prepare('sidecar-update')
        staged=self.fixture.stage/'client/sdgo/mods/match_ai'
        staged.mkdir()
        (staged/'match_ai.dll').write_bytes(b'updated-match-fixture')
        (staged/'mod.ini').write_text(fixtures.MATCH_MANIFEST)
        manifest=game_test.read_ini(staged/'mod.ini')
        manifest['mod']['requires']='game.app,match.local_update'
        game_test.write_ini(staged/'mod.ini',manifest)
        shared=self.fixture.stage/f'client/sdgo/symbols/{game_test.HOST_KEY}.ini'
        symbols=game_test.read_ini(shared)
        symbols['host']['timestamp']='0x54BE1568'
        symbols['host']['size_of_image']='0x00C97000'
        game_test.write_ini(shared,symbols)
        fragment=game_test.ini_parser();fragment['host']=dict(symbols['host'])
        fragment['match.local_update']={'kind':'code','rva':'0x1234','expect':'55 8B EC'}
        game_test.write_ini(staged/'symbols.fragment.ini',fragment)
        game_test.reuse_prepared(self.fixture.template,self.fixture.stage,run)
        installed=run/'client/sdgo/mods/match_ai/symbols.fragment.ini'
        self.assertEqual(installed.read_bytes(),(staged/'symbols.fragment.ini').read_bytes())
        copied=game_test.read_ini(run/f'client/sdgo/symbols/{game_test.HOST_KEY}.ini')
        self.assertNotIn('match.local_update',copied)
        game_test.verify_hashes(game_test.validate_metadata(run))
        other=self.fixture.stage/'client/sdgo/mods/other_mod';other.mkdir(exist_ok=True)
        (other/'mod.ini').write_text(fixtures.OTHER_MANIFEST+'requires=match.local_update\n')
        (other/'other_mod.dll').write_bytes(b'updated-other-fixture')
        with self.assertRaisesRegex(game_test.GameTestError,'stage host symbols lack.*match.local_update'):
            game_test.validate_observer_stage(run/'client',self.fixture.stage)

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
