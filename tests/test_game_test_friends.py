from pathlib import Path
import ctypes
import concurrent.futures
import importlib.util
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
spec=importlib.util.spec_from_file_location('game_test_friends',ROOT/'tools/game_test_friends.py')
friends=importlib.util.module_from_spec(spec);spec.loader.exec_module(friends)


class FriendCampaignContracts(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.run_id='01234567-89ab-4cde-8fab-0123456789ab'
        self.metadata={'run_id':self.run_id,'plan':{'friends_only':True},
                       'client':{'root':str(self.root),'events':str(self.root/'events.jsonl')}}
        patch=mock.patch.object(friends.game_test,'validate_metadata',return_value=self.metadata)
        patch.start();self.addCleanup(patch.stop)
        (self.root/'sdgo/gametest').mkdir(parents=True)
    def test_non_ascii_target_survives_real_windows_ini_read(self):
        if os.name!='nt':self.skipTest('requires the same Windows INI reader as the native Mod')
        actor=friends.FriendActor(self.root)
        actor.wait=lambda predicate,timeout: {'written':True}
        actor.command('add',target_name='测试好友')
        buffer=ctypes.create_unicode_buffer(80)
        api=ctypes.WinDLL('kernel32',use_last_error=True).GetPrivateProfileStringW
        api.argtypes=[ctypes.c_wchar_p,ctypes.c_wchar_p,ctypes.c_wchar_p,ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_wchar_p]
        api.restype=ctypes.c_uint32
        self.assertEqual(api('command','target_name','',buffer,80,str(actor.directory/'friend-command.ini')),4)
        self.assertEqual(buffer.value,'测试好友')
    def test_foreign_native_receipt_cannot_be_used_for_a_command(self):
        actor=friends.FriendActor(self.root)
        (actor.directory/'friend-result.ini').write_text('[run]\nid=foreign\n',encoding='utf-8')
        with self.assertRaises(friends.game_test.GameTestError):actor.state()
    def test_native_error_requires_fresh_same_session_exact_original_path(self):
        actor=friends.FriendActor(self.root);path=Path(self.metadata['client']['events'])
        row={'run_id':self.run_id,'case':'friends.session','event':'observation',
             'message':'native error code=0818 name=ERR_MAKE_FRIEND_MSN_NOT_EXIST'}
        path.write_text(json.dumps(row)+'\n',encoding='utf-8')
        baseline=len(actor.native_events())
        self.assertFalse(actor.native_error_after(baseline,0x0818,'ERR_MAKE_FRIEND_MSN_NOT_EXIST'))
        with path.open('a',encoding='utf-8') as out:out.write(json.dumps(row)+'\n')
        self.assertTrue(actor.native_error_after(baseline,0x0818,'ERR_MAKE_FRIEND_MSN_NOT_EXIST'))
        self.assertFalse(actor.native_error_after(baseline,0x1406,'OTHER_ERROR'))
        row['run_id']='foreign';path.write_text(json.dumps(row)+'\n',encoding='utf-8')
        with self.assertRaises(friends.game_test.GameTestError):actor.native_events()
    def test_exited_native_process_fails_ready_wait_without_waiting_for_missing_run_end(self):
        completion=concurrent.futures.Future();completion.set_result({'status':'fail'})
        actor=friends.FriendActor(self.root,completion)
        with self.assertRaisesRegex(friends.game_test.GameTestError,'native actor exited'):
            actor.wait(lambda state:state.get('run',{}).get('ready')=='1',timeout=30)
    def test_command_delimiters_are_rejected_before_publication(self):
        actor=friends.FriendActor(self.root)
        with self.assertRaises(friends.game_test.GameTestError):actor.command('send',body='line1\n[run]\nid=other')
        self.assertFalse((actor.directory/'friend-command.ini').exists())

    def test_final_receipt_published_between_poll_and_run_end_is_consumed(self):
        actor=friends.FriendActor(self.root)
        Path(self.metadata['client']['events']).touch()
        final={'command':{'sequence':'4','done':'1'}}
        with mock.patch.object(actor,'state',side_effect=[None,final]), \
             mock.patch.object(friends.game_test,'has_run_end',return_value=True):
            self.assertEqual(actor.wait(lambda s:s['command']['sequence']=='4',1),final)
    def test_command_publication_waits_for_native_reader_without_delete_sharing(self):
        if os.name!='nt':self.skipTest('requires Windows file sharing semantics')
        actor=friends.FriendActor(self.root)
        actor.wait=lambda predicate,timeout:{'written':True}
        path=actor.directory/'friend-command.ini'
        path.write_bytes(b'\xff\xfe'+('[command]\nsequence=0\n').encode('utf-16-le'))
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
        kernel.CreateFileW.restype=ctypes.c_void_p
        kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        handle=kernel.CreateFileW(str(path),0x80000000,3,None,3,0,None)
        self.assertNotEqual(handle,ctypes.c_void_p(-1).value)
        released=threading.Timer(.1,kernel.CloseHandle,args=(handle,));released.start()
        try:
            started=time.monotonic();actor.command('snapshot')
            self.assertGreaterEqual(time.monotonic()-started,.075)
            self.assertEqual(actor.sequence,1)
            self.assertIn('sequence=1',path.read_text(encoding='utf-16'))
            self.assertFalse(path.with_name(path.name+'.tmp').exists())
        finally:released.join()
    def test_native_atomic_replace_permission_race_retries_but_malformed_receipt_fails(self):
        actor=friends.FriendActor(self.root)
        path=actor.directory/'friend-result.ini';path.write_text('[run]\nid='+self.run_id+'\n',encoding='utf-8')
        transient=friends.game_test.GameTestError('native receipt temporarily delete-pending')
        transient.__cause__=PermissionError(13,'sharing conflict')
        with mock.patch.object(friends.game_test,'read_ini',side_effect=transient):
            self.assertIsNone(actor.state())
        malformed=friends.game_test.GameTestError('malformed receipt')
        malformed.__cause__=ValueError('invalid section')
        with mock.patch.object(friends.game_test,'read_ini',side_effect=malformed):
            with self.assertRaises(friends.game_test.GameTestError):actor.state()
    def test_unsupported_adapter_result_cannot_count_as_transport_success(self):
        for result in (0x100,0x105,0x106,0x108,0x109,0x10B,0x10C):
            with self.subTest(result=result),self.assertRaises(friends.game_test.GameTestError):
                friends.assert_receipt({'command':{'done':'1','result':str(result),'request_sent':'1'}})
    def test_generic_failure_cannot_count_as_native_validation(self):
        self.assertFalse(friends.native_rejection({'command':{'result':str(0x108),'reply_status':'0','reply_consumed':'0'}}))
        self.assertTrue(friends.native_rejection({'command':{'result':str(0x10A),'reply_status':'5127','reply_consumed':'1'}}))
    def test_campaign_uses_only_declared_existing_pagination_profiles(self):
        profiles=self.root/'profiles';profiles.mkdir()
        for label in ('a','b','page-peer-0','page-peer-2','page-peer-not-an-actor'):
            (profiles/(label+'.ini')).write_text('private fixture profile',encoding='utf-8')
        campaign=friends.Campaign(self.root,self.root,self.root,'127.0.0.2')
        self.addCleanup(campaign.pool.shutdown,wait=False)
        self.assertEqual(campaign.peer_labels,('page-peer-0','page-peer-2'))
        with mock.patch.object(campaign,'start') as start:
            campaign.peer_labels=()
            with self.assertRaisesRegex(friends.game_test.GameTestError,'existing fixture'):
                campaign.pagination()
            start.assert_not_called()
    def test_stop_keeps_actor_owned_until_its_finish_command_succeeds(self):
        campaign=friends.Campaign(self.root,self.root,self.root,'127.0.0.2')
        self.addCleanup(campaign.pool.shutdown,wait=False)
        actor=mock.Mock();actor.command.side_effect=PermissionError('native command file occupied')
        campaign.running['a']=(actor,None,None)
        with self.assertRaises(PermissionError):campaign.stop('a')
        self.assertIn('a',campaign.running)
    def test_persistence_resume_rejects_a_foreign_pass_receipt(self):
        campaign=friends.Campaign(self.root,self.root,self.root,'127.0.0.2')
        self.addCleanup(campaign.pool.shutdown,wait=False)
        report=self.root/'b-01/receipts/report.json';report.parent.mkdir(parents=True)
        report.write_text(json.dumps({'status':'pass','run_id':'foreign'}),encoding='utf-8')
        with mock.patch.object(campaign,'start') as start:
            with self.assertRaisesRegex(friends.game_test.GameTestError,'passed native peer'):
                campaign.persistence()
            start.assert_not_called()


if __name__=='__main__':unittest.main()
