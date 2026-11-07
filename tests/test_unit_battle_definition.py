import copy
import hashlib
import math
import struct
import unittest

from owner.units import native_battle_definition as profile
from owner.units import repository as repo

def row(identity=15001):
    result={name:0 for name in repo.UNIT_COLUMNS}
    result.update(UnitNumber=identity,UnitModelName='Example\0',UnitNumber_Transform=15002,
        UnitGrowIndex=1,MasteredWeaponIX=2,HP=12345,Shield=2000,
        SkillNumber_1=70111,WeaponNumber_1=40001,ViewOrder=61234,
        MaxEnergyAmount=1.25,MaxEnergyCost=0.125,MonthLimit=123,
        UserGradeLimit=0x01000000,SallyReady=1,CollectionUnit=1,UnitGraph=3)
    return result

def grade():return {'UnitGradeNumber':0,'UnitGradeNumber2':0,'LevelRating':-3}
GRADES={(0,0):grade()}

class NativeBattleDefinition(unittest.TestCase):
    def test_native_consumer_offsets_and_complete_little_endian_payload(self):
        source=row();raw=profile.expected_payload(source,grade())
        self.assertEqual(114,len(raw));self.assertEqual((15001,),struct.unpack_from('<I',raw))
        self.assertEqual(15002,struct.unpack_from('<I',raw,0xc)[0])
        self.assertEqual(70111,struct.unpack_from('<I',raw,0x35)[0])
        self.assertEqual(2,struct.unpack_from('<I',raw,0x45)[0])
        self.assertEqual(40001,struct.unpack_from('<I',raw,0x49)[0])
        self.assertEqual(61234,struct.unpack_from('<I',raw,0x59)[0])
        self.assertEqual(1.25,struct.unpack_from('<f',raw,0x5d)[0])
        self.assertEqual(0x01000000,struct.unpack_from('<I',raw,0x6a)[0])
        self.assertEqual(-3,struct.unpack_from('<i',raw,0x6e)[0])
        decoded=profile.decode_payload(raw)
        self.assertEqual({name:source.get(name,-3) for name,_ in profile.FIELDS},decoded)

    def test_sql_only_fields_still_change_complete_row_binding(self):
        first=row();second=copy.deepcopy(first)
        second.update(DailyLimit=99,MonthLimit=88,SallyReady=0,CollectionUnit=0,UnitGraph=4,UnitModelName='Another')
        a,ra=profile.compile_expectations({15001:first},GRADES);b,rb=profile.compile_expectations({15001:second},GRADES)
        self.assertEqual(a,b);self.assertNotEqual(ra['records'][0]['server_row_sha256'],rb['records'][0]['server_row_sha256'])
        self.assertEqual({'UnitModelName','DailyLimit','MonthLimit','SallyReady','CollectionUnit','UnitGraph'},set(ra['sql_only_fields']))
        for flag in ('native_readback_accepted','native_actor_accepted','ordinary_publication_allowed'):
            self.assertIs(ra[flag],False)

    def test_malformed_incomplete_nonfinite_and_out_of_domain_inputs_are_refused(self):
        for name,value in [('HP',True),('SkillNumber_1',1.5),('MasteredWeaponIX',2**31),
                           ('Influence1',256),('BattleCost',-32769),('MaxEnergyAmount',math.nan),
                           ('UnitNumber',30001),('UnitNumber_Transform',35000)]:
            source=row();source[name]=value
            with self.subTest(field=name),self.assertRaises(ValueError):profile.expected_payload(source,grade())
        source=row();del source['SallyReady']
        with self.assertRaises(ValueError):profile.expected_payload(source,grade())
        for raw in (b'',bytes(115),bytearray(profile.expected_payload(row(),grade()))):
            with self.assertRaises(ValueError):profile.decode_payload(raw)
        raw=bytearray(profile.expected_payload(row(),grade()));struct.pack_into('<f',raw,0x5d,math.inf)
        with self.assertRaises(ValueError):profile.decode_payload(bytes(raw))
        with self.assertRaises(ValueError):profile.compile_expectations({15002:row()},GRADES)

    def test_sorted_hash_bound_expectations_and_field_differences(self):
        data,receipt=profile.compile_expectations({15002:row(15002),15001:row()},GRADES)
        self.assertEqual((profile.MAGIC,1,114,2,0),profile.HEADER.unpack_from(data))
        self.assertEqual([15001,15002],[r['id'] for r in receipt['records']])
        self.assertEqual(hashlib.sha256(data).hexdigest(),receipt['expected_sha256'])
        first=profile.expected_payload(row(),grade());other=row();other['MasteredWeaponIX']=3;other['SkillNumber_1']=70112
        changes=profile.compare_payload(first,profile.expected_payload(other,grade()))
        self.assertEqual([('SkillNumber_1',0x35),('MasteredWeaponIX',0x45)],[(r['field'],r['offset']) for r in changes])

    def test_grade_join_is_mandatory_typed_and_hash_bound(self):
        with self.assertRaises(ValueError):profile.compile_expectations({15001:row()}, {})
        with self.assertRaises(ValueError):profile.expected_payload(row(),{'UnitGradeNumber':1,'UnitGradeNumber2':0,'LevelRating':-3})
        for value in (True,1.5,2**31):
            with self.subTest(value=value),self.assertRaises(ValueError):profile.expected_payload(row(),dict(grade(),LevelRating=value))
        old,receipt=profile.compile_expectations({15001:row()},GRADES)
        new,new_receipt=profile.compile_expectations({15001:row()},{(0,0):dict(grade(),LevelRating=-2)})
        self.assertNotEqual(old,new);self.assertNotEqual(receipt['grade_snapshot_sha256'],new_receipt['grade_snapshot_sha256'])

if __name__=='__main__':unittest.main()
