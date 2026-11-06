"""Keep typed ray fields distinct from LuaJIT colon-call receiver copies."""
import unittest
from tests.test_gunion_luajit import dump, instruction, prototype, uleb
from tools.gunion.luajit import parse_dump, KGC_STRING
from tools.gunion.luajit_lower import lower_prototype, LoweringError

def string(value):
    raw=value.encode('utf8')
    return uleb(KGC_STRING+len(raw))+raw

def program(keys):
    payload=prototype(parameters=1,frame_size=3,
        instructions=[instruction('TGETS',a=1,b=0,c=0),instruction('TGETS',a=2,b=0,c=1),instruction('RET1',a=1,d=2)],
        gc_constants=[string(keys[1]),string(keys[0])])
    return parse_dump(dump(payload))

class TypedRayFieldLoweringTests(unittest.TestCase):
    def test_ray_position_and_direction_are_reads_not_self_receiver_writes(self):
        source=program(('pos','dir'));out=lower_prototype(source,source.prototypes[0])
        self.assertEqual([word&63 for word in out.code_words],[6,6,27])
        self.assertEqual([((word>>24)&255,(word>>15)&511) for word in out.code_words[:2]],[(1,0),(2,0)])
        self.assertEqual([out.constant_pool[((word>>6)&511)-250] for word in out.code_words[:2]],['pos','dir'])
        self.assertEqual(out.source_pc_to_derived_pc,(0,1,2))

    def test_unknown_nonmethod_field_is_still_refused(self):
        source=program(('unreviewed','dir'))
        with self.assertRaisesRegex(LoweringError,'unproven colon-call/field'):
            lower_prototype(source,source.prototypes[0])

    def test_existing_vector_component_reads_keep_their_spine(self):
        source=program(('x','y'));out=lower_prototype(source,source.prototypes[0])
        self.assertEqual([word&63 for word in out.code_words],[6,6,27])

if __name__=='__main__':unittest.main()
