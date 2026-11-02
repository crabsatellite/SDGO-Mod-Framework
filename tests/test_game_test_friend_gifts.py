from pathlib import Path
import datetime as dt
import importlib.util
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
spec=importlib.util.spec_from_file_location('game_test_friend_gifts',ROOT/'tools/game_test_friend_gifts.py')
gifts=importlib.util.module_from_spec(spec);spec.loader.exec_module(gifts)


class GiftNativeProofContracts(unittest.TestCase):
    def test_empty_or_foreign_sql_metadata_cannot_prove_owned_files(self):
        root=ROOT/'build/native-proof-contract'
        self.assertFalse(gifts.native_paths_owned(root,[]))
        self.assertFalse(gifts.native_paths_owned(root,[root.parent/'other-copy/Game.mdf']))
        self.assertTrue(gifts.native_paths_owned(root,[root/'data/Game.mdf',root/'data/Game.ldf']))

    def test_source_time_is_utc_and_does_not_shift_naive_sql_datetime(self):
        sql_time=dt.datetime(2026,11,3,0,1,59,999000)
        self.assertEqual(gifts.provenance_timestamp(sql_time),'2026-11-03 00:01 UTC')
        tokyo=sql_time.replace(hour=9,tzinfo=dt.timezone(dt.timedelta(hours=9)))
        self.assertEqual(gifts.provenance_timestamp(tokyo),'2026-11-03 00:01 UTC')

    def test_unicode_multiline_and_surrogate_pairs_round_trip_without_ini_delimiters(self):
        note='赠礼\n[unit_gift]\n寄语=出击顺利！🎁'
        encoded=gifts.text_hex(note)
        self.assertFalse(any(c in encoded for c in '\r\n=[]'))
        self.assertEqual(gifts.from_text_hex(encoded),note)

    def test_native_uid_is_never_converted_through_a_float(self):
        uid=0x1122334455667788
        row={'UserUnitNumber':uid,'AccountNumber':1,'UserUnitSlotNumber':2,'DateGet':'original','UnitExp':314159}
        received={**row,'AccountNumber':3,'UserUnitSlotNumber':11}
        self.assertTrue(gifts.same_payload(row,received,allow_owner_slot=True))
        self.assertFalse(gifts.same_payload(row,{**received,'UserUnitNumber':uid+1},allow_owner_slot=True))

    def test_accept_only_allows_owner_and_slot_changes(self):
        before={'AccountNumber':1,'UserUnitSlotNumber':2,'DateGet':'original','UnitExp':99,'Color':b'\x00\xff'}
        after={**before,'AccountNumber':3,'UserUnitSlotNumber':5}
        self.assertFalse(gifts.same_payload(before,after))
        for field,value in (('DateGet','reset'),('UnitExp',0),('Color',b'\0')):
            with self.subTest(field=field):
                self.assertFalse(gifts.same_payload(before,{**after,field:value},allow_owner_slot=True))
        self.assertFalse(gifts.same_payload(before,None,allow_owner_slot=True))


if __name__=='__main__':unittest.main()
