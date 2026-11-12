import hashlib
import struct
import unittest
from owner.units import compiler,stages

class StageRules(unittest.TestCase):
    def setUp(self):
        a=bytearray(4976);b=bytearray(4976);s=bytearray(1132)
        struct.pack_into('<I',a,0,29001);struct.pack_into('<I',b,0,29002);struct.pack_into('<I',s,0,74001)
        struct.pack_into('<I',a,316,29002);struct.pack_into('<I',a,575,74001)
        s[155]=2;struct.pack_into('<h',s,156,60)
        self.records={(2,29001):bytes(a),(2,29002):bytes(b),(13,74001):bytes(s)}
        self.catalog=compiler.native_catalog([(k,i,r) for (k,i),r in self.records.items()])
        self.rules=[{'unit_id':29001,'target_unit_id':29002,'skill_id':74001,'max_hp_percent':60}]
    def test_seals_complete_catalog_and_exact_native_edge(self):
        raw=stages.encode(self.rules,self.catalog,self.records)
        magic,version,header,count,size,digest,checksum,reserved=struct.unpack_from('<8s4I32sI20s',raw)
        self.assertEqual((b'SDGOST1\0',1,80,1,16),(magic,version,header,count,size))
        self.assertEqual(hashlib.sha256(self.catalog).digest(),digest)
        self.assertEqual(compiler.fnv(raw[80:]),checksum);self.assertEqual(bytes(20),reserved)
        self.assertEqual((29001,29002,74001,60),struct.unpack_from('<4I',raw,80))
    def test_rejects_changed_edge_skill_threshold_dispatch_and_special_transform_condition(self):
        for key,offset,raw_value in [((2,29001),316,bytes(4)),((2,29001),575,bytes(4)),
                                    ((2,29002),320,b'\x03'),((13,74001),155,b'\x00'),
                                    ((13,74001),156,b'\x3b\x00'),((13,74001),1131,b'\x01')]:
            with self.subTest(key=key,offset=offset):
                records=dict(self.records);raw=bytearray(records[key]);raw[offset:offset+len(raw_value)]=raw_value;records[key]=bytes(raw)
                with self.assertRaises(ValueError):stages.encode(self.rules,self.catalog,records)
        with self.assertRaises(ValueError):stages.encode(self.rules,self.catalog,{k:v for k,v in self.records.items() if k!=(2,29002)})
    def test_rejects_duplicate_sources_and_invalid_scalar_types(self):
        with self.assertRaises(ValueError):stages.normalize(self.rules*2)
        for field,value in [('max_hp_percent',True),('max_hp_percent',100),('skill_id',False),('target_unit_id',29001)]:
            with self.subTest(field=field,value=value):
                row={**self.rules[0],field:value}
                with self.assertRaises(ValueError):stages.normalize([row])
        with self.assertRaisesRegex(ValueError,'额外技能槽'):
            stages.normalize([{**self.rules[0],'skill_id':79950}])

    def test_native_skill_category_boundaries_are_open_as_in_the_consumer(self):
        from owner.units.skill_slots import ordinary,ORDINARY_RANGES
        for first,last in ORDINARY_RANGES:
            self.assertTrue(ordinary(first));self.assertTrue(ordinary(last))
            self.assertFalse(ordinary(first-1));self.assertFalse(ordinary(last+1))
        self.assertFalse(ordinary(True));self.assertFalse(ordinary(9901001))
