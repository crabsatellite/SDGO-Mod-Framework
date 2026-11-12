import struct
import unittest
from owner.units.indexes import Allocator,FIRST,catalog_records
from owner.units.compiler import native_catalog

def index(identity,unit):
    raw=bytearray(614);struct.pack_into('<I',raw,0,identity);raw[5]=1;struct.pack_into('<I',raw,6,unit)
    return bytes(raw)

class IndexScopes(unittest.TestCase):
    def test_owner_additions_preserve_preset_and_existing_owner_ids(self):
        original={FIRST:index(FIRST,10001)}
        scopes={'owner':{FIRST+1:index(FIRST+1,29001)},'presets':{FIRST+2:index(FIRST+2,29002)}}
        a=Allocator(original,scopes,'owner')
        self.assertEqual(FIRST+1,a.allocate(29001));self.assertEqual(FIRST+3,a.allocate(29003))
        b=Allocator(original,scopes,'presets')
        self.assertEqual(FIRST+2,b.allocate(29002));self.assertEqual(FIRST+3,b.allocate(29004))
    def test_gaps_and_cross_scope_collisions_refuse_before_allocation(self):
        original={FIRST:index(FIRST,10001)}
        for scopes in [{'owner':{FIRST:index(FIRST,29001)}}, {'presets':{FIRST+2:index(FIRST+2,29001)}}]:
            with self.subTest(scopes=scopes):
                with self.assertRaises(ValueError):Allocator(original,scopes,'owner')
    def test_catalog_parser_checks_complete_payload_and_record_identity(self):
        raw=native_catalog([(85,FIRST,index(FIRST,10001))]);self.assertEqual((85,index(FIRST,10001)),catalog_records(raw)[FIRST])
        for offset in (0,8,20,24,32,56):
            mutated=bytearray(raw);mutated[offset]^=1
            with self.subTest(offset=offset):
                with self.assertRaises(ValueError):catalog_records(bytes(mutated))
