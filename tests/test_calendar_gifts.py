from pathlib import Path
import datetime as dt
import json
import tempfile
import unittest
from owner.calendar_gifts import model
from owner.calendar_gifts.workspace import Workspace
from manager import storage


class CalendarGiftContracts(unittest.TestCase):
    def test_public_defaults_have_seven_festivals_and_twelve_birthday_grades(self):
        policy=model.validate(model.defaults())
        self.assertFalse(policy['enabled']);self.assertEqual(len(policy['events']),7)
        self.assertEqual(len(policy['birthday']['rewards']['grades']),12)
        self.assertEqual(model.private_defaults(),{'schema':1,'birthdays':[],'groups':{}})

    def test_authoritative_festival_dates_include_lunar_and_qingming(self):
        events={e['id']:e for e in model.defaults()['events']}
        expected={'spring_festival':'2026-02-17','qingming':'2026-04-05','dragon_boat':'2026-06-19','mid_autumn':'2026-09-25'}
        for key,value in expected.items():self.assertEqual(model.event_date(events[key],2026).isoformat(),value)
        with self.assertRaisesRegex(ValueError,'缺少'):model.event_date(events['spring_festival'],2041)

    def test_same_year_identity_survives_birthday_date_edits(self):
        p=model.defaults();people=model.private_defaults();people['birthdays']=[{'account_id':71,'month_day':'11-03','enabled':True}]
        first=model.occurrences(p,people,dt.date(2026,11,3),dt.date(2026,11,3))[0]
        people['birthdays'][0]['month_day']='11-04'
        later=model.occurrences(p,people,dt.date(2026,11,4),dt.date(2026,11,4))[0]
        self.assertEqual((first['key'],first['account_id']),(later['key'],later['account_id']))

    def test_leap_birthdays_and_cross_year_windows(self):
        self.assertEqual(model.birthday_date('02-29',2025,'feb28'),dt.date(2025,2,28))
        self.assertEqual(model.birthday_date('02-29',2025,'mar01'),dt.date(2025,3,1))
        self.assertIsNone(model.birthday_date('02-29',2025,'leap_only'))
        self.assertEqual(model.birthday_date('02-29',2028,'feb28'),dt.date(2028,2,29))
        found=model.occurrences(model.defaults(),model.private_defaults(),dt.date(2025,12,31),dt.date(2026,1,2))
        self.assertEqual([e['key'] for e in found],['new_year:2026'])

    def test_legacy_fallback_stays_within_base_grade_and_never_duplicates(self):
        rule=model.default_rewards(True);rule['grades']=[[1,3],[1,4],[2,3]]
        units=[{'unit_id':1,'grade':(1,4)},{'unit_id':2,'grade':(1,2)},{'unit_id':3,'grade':(4,3)}]
        chosen=model.choose_missing(rule,units,set(),lambda pool:pool[0])
        self.assertEqual([r['unit_id'] for r in chosen],[1,2])
        self.assertEqual(model.choose_missing(rule,units,{1,2},lambda pool:pool[0]),[])

    def test_pending_native_units_are_treated_as_owned(self):
        units=[{'unit_id':1,'grade':(4,3)},{'unit_id':2,'grade':(4,4)}]
        self.assertEqual(model.choose_missing(model.default_rewards(),units,{1},lambda pool:pool[0]),[units[1]])

    def test_public_export_excludes_private_birthdays_groups_and_targeted_dates(self):
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);public=w.public()
            private={'schema':1,'birthdays':[{'account_id':987654,'month_day':'11-03','enabled':True}],'groups':{'private_group':[987654]}}
            w.save('private',private,public['private_revision'])
            p=w.policy();p['events'][0]['group']='private_group';p['enabled']=True
            w.save('policy',p,public['policy_revision']);data=w.export();exported=json.loads(data)
            self.assertNotIn(b'987654',data);self.assertNotIn(b'11-03',data);self.assertNotIn(b'private_group',data)
            self.assertFalse(exported['enabled']);self.assertEqual(len(exported['events']),6)
            self.assertTrue(w.people()['birthdays'])

    def test_revisions_preserve_other_edits_and_first_enable_does_not_backfill(self):
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);initial=w.public();p=w.policy();p['enabled']=True
            w.save('policy',p,initial['policy_revision'])
            with self.assertRaises(storage.Conflict):w.save('policy',p,initial['policy_revision'])
            self.assertEqual(w.window(p),(w.today(p),w.today(p)))

    def test_invalid_private_data_and_birthday_event_key_cannot_enter_public_rules(self):
        p=model.defaults();p['events'][0]['id']='birthday'
        with self.assertRaises(ValueError):model.validate(p)
        p=model.defaults();p['birthdays']=[{'account_id':1,'month_day':'01-01'}]
        with self.assertRaises(ValueError):model.validate(p)
        with self.assertRaises(ValueError):model.validate_private({'schema':1,'birthdays':[{'account_id':True,'month_day':'01-01','enabled':True}],'groups':{}})

    def test_legacy_migration_accepts_completed_dates_and_excludes_pending_or_unrelated_state(self):
        value={'birthday_done':{'7':'2026-03-04','8':'pending','9':'invalid','bogus':'2026-01-01'},'private_email':'never imported'}
        self.assertEqual(model.legacy_birthdays(value,dt.date(2026,11,3)),[{'account_id':7,'date':'2026-03-04'}])
        with self.assertRaises(ValueError):model.legacy_birthdays({'birthday_done':{'7':'2027-03-04'}},dt.date(2026,11,3))
        with tempfile.TemporaryDirectory() as root:
            source=Path(root)/'processor_state.json';source.write_text(json.dumps(value),encoding='utf-8')
            w=Workspace(Path(root)/'calendar');self.assertEqual(w.import_legacy(source),1)
            self.assertEqual(w.legacy(),[{'account_id':7,'date':'2026-03-04'}])
            self.assertNotIn('private_email',w.legacy_file.read_text(encoding='utf-8'))
            self.assertNotIn(b'2026-03-04',w.export())


if __name__=='__main__':unittest.main()
