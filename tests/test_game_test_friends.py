from pathlib import Path
import ctypes
import importlib.util
import json
import os
import sys
import tempfile
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
    def test_command_delimiters_are_rejected_before_publication(self):
        actor=friends.FriendActor(self.root)
        with self.assertRaises(friends.game_test.GameTestError):actor.command('send',body='line1\n[run]\nid=other')
        self.assertFalse((actor.directory/'friend-command.ini').exists())
    def test_unsupported_adapter_result_cannot_count_as_transport_success(self):
        for result in (0x100,0x105,0x106,0x108,0x109,0x10B,0x10C):
            with self.subTest(result=result),self.assertRaises(friends.game_test.GameTestError):
                friends.assert_receipt({'command':{'done':'1','result':str(result),'request_sent':'1'}})
    def test_generic_failure_cannot_count_as_native_validation(self):
        self.assertFalse(friends.native_rejection({'command':{'result':str(0x108),'reply_status':'0','reply_consumed':'0'}}))
        self.assertTrue(friends.native_rejection({'command':{'result':str(0x10A),'reply_status':'5127','reply_consumed':'1'}}))


if __name__=='__main__':unittest.main()
