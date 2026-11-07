import struct
import unittest

from tools.gunion.lua50_compile import parse_chunk
from tools.gunion.lua50_scope import Rewrite,encode_chunk,lower


def integer(value):return struct.pack('<I',value)
def string(value):return integer(len(value)+1)+value+b'\0'
def instruction(op,a=0,b=0,c=0):return op|(a<<24)|(b<<15)|(c<<6)
def abx(op,a,bx):return op|(a<<24)|(bx<<6)
def prototype(constants,children,code):
    return (string(b'export')+integer(17)+bytes((0,1,0,4))+integer(1)+integer(17)+integer(1)+string(b'export')+integer(0)+integer(len(code))
            +integer(0)+integer(len(constants))+b''.join(constants)+integer(len(children))+b''.join(children)+integer(len(code))+b''.join(integer(word)for word in code))
def fixture(mixed=False):
    header=b'\x1bLua'+bytes((0x50,1,4,4,4,6,8,9,9,8))+struct.pack('<d',3.14159265358979323846e7)
    child=prototype([b'\4'+string(b'state'),b'\4'+string(b'fxrs\\a.zfx'),b'\4'+string(b'unused'),b'\3'+bytes.fromhex('0000000000000080')],[],
                    [abx(5,0,0),abx(7,0,0),abx(1,1,1),abx(1,2,0)]if mixed else[abx(5,0,0),abx(7,0,0),abx(1,1,1),instruction(27,0,1)])
    return header+prototype([b'\4'+string(b'export')],[child],[abx(34,0,0),abx(7,0,0),instruction(27,0,1)])


class Lua50ScopeTest(unittest.TestCase):
    def test_callback_name_must_feed_literal_special_script_setter(self):
        header=fixture()[:22]
        constants=[b'\4'+string(b'helper'),b'\4'+string(b'SpecialScriptFuncStr')]
        code=[instruction(11,0,0,251),abx(1,2,0),instruction(25,0,3,1),instruction(27,0,1)]
        child=prototype(constants,[],code)
        raw=header+prototype([b'\4'+string(b'export')],[child],[abx(34,0,0),abx(7,0,0),instruction(27,0,1)])
        declaration=Rewrite((0,),0,b'helper',b'owner_helper','callback-name','native named callback buffer and literal setter argument',b'SpecialScriptFuncStr')
        target,proof=lower(raw,[declaration]);self.assertEqual(b'owner_helper',parse_chunk(target).root.children[0].constants[0].value)
        self.assertEqual('callback-name',proof['carriers'][0]['role'])
        for api in [None,b'FakeSetter']:
            with self.assertRaises(ValueError):lower(raw,[Rewrite((0,),0,b'helper',b'owner_helper','callback-name','review',api)])
        invalid=raw.replace(integer(instruction(25,0,3,1)),integer(instruction(25,0,4,1)))
        with self.assertRaises(ValueError):lower(invalid,[declaration])

    def test_complete_roundtrip_and_only_selected_typed_strings_change(self):
        raw=fixture();before=parse_chunk(raw);self.assertEqual(raw,encode_chunk(before))
        target,proof=lower(raw,[Rewrite((),0,b'export',b'owner_export','global-export','root closure binding'),
                              Rewrite((0,),0,b'state',b'owner_state','mutable-global','source GETGLOBAL/SETGLOBAL'),
                              Rewrite((0,),1,b'fxrs\\a.zfx',b'fxrs\\owner_a.zfx','resource-path','reviewed native effect argument')])
        after=parse_chunk(target);self.assertEqual(b'export',after.root.source);self.assertEqual(b'export',after.root.locals[0].name)
        self.assertEqual([i.raw for i in before.root.children[0].code],[i.raw for i in after.root.children[0].code])
        self.assertEqual('0000000000000080',after.root.children[0].constants[3].raw_hex)
        self.assertEqual(b'unused',after.root.children[0].constants[2].value)
        self.assertEqual(3,len(proof['carriers']));self.assertFalse(proof['nativeRootOrGameplayAccepted'])

    def test_global_symbol_shared_with_non_global_constant_use_refuses(self):
        with self.assertRaisesRegex(ValueError,'non-global'):
            lower(fixture(True),[Rewrite((0,),0,b'state',b'owner_state','mutable-global','review')])

    def test_wrong_carrier_source_duplicate_and_unused_constant_refuse(self):
        for declaration in [Rewrite((1,),0,b'export',b'owner_export','global-export','review'),
                            Rewrite((),0,b'wrong',b'owner_export','global-export','review'),
                            Rewrite((0,),2,b'unused',b'owner_unused','resource-path','review'),
                            Rewrite((0,),0,b'state',b'owner_state','resource-path','review')]:
            with self.assertRaises(ValueError):lower(fixture(),[declaration])
        row=Rewrite((),0,b'export',b'owner_export','global-export','review')
        with self.assertRaisesRegex(ValueError,'duplicate'):lower(fixture(),[row,row])

    def test_metadata_bytes_cannot_establish_a_constant_carrier(self):
        with self.assertRaises(ValueError):lower(fixture(),[Rewrite((0,),3,b'export',b'owner_export','resource-path','review')])
        with self.assertRaises(ValueError):lower(fixture(),[Rewrite((),0,b'export',b'owner_export','untyped-scanner-match','review')])

    def test_export_requires_direct_closure_and_authority(self):
        chunk=parse_chunk(fixture());raw=fixture();root=chunk.root
        # A register copy cannot become an allowed root closure publication.
        old=integer(root.code[0].raw);new=integer(instruction(0,0,1));offset=len(raw)-len(root.code)*4
        changed=raw[:offset]+raw[offset:].replace(old,new,1)
        row=Rewrite((),0,b'export',b'owner_export','global-export','review')
        with self.assertRaisesRegex(ValueError,'closure'):lower(changed,[row])
        with self.assertRaises(ValueError):lower(raw,[Rewrite((),0,b'export',b'owner_export','global-export','')])


if __name__=='__main__':unittest.main()
