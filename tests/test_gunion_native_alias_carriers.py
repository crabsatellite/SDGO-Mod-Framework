import struct
import unittest
from dataclasses import replace

from tools.gunion.native_alias_carriers import field,model_materials,rewrite_prefixes,fixed_records


class NativeAliasCarrierTest(unittest.TestCase):
    def test_family_graph_skill_definition_label_and_unknown_kinds_are_explicit(self):
        raw=bytearray(882)
        value='Fxrs\\sample.zfx'.encode('utf-16-le')+b'\0\0'
        raw[308:308+len(value)]=value
        carriers=fixed_records(bytes(raw),'skill-definition')
        self.assertEqual(1,len(carriers))
        self.assertEqual((308,60,'fxrs/sample.zfx'),(carriers[0].offset,carriers[0].capacity,carriers[0].path))
        with self.assertRaisesRegex(ValueError,'unsupported'):
            fixed_records(bytes(raw),'unrecognized-record')

    def test_only_typed_prefix_changes_and_invalid_post_nul_padding_survives(self):
        raw=bytearray(b'\xa5'*100)
        value='Txrs\\old.txr'.encode('utf-16-le')+b'\0\0'
        raw[7:7+len(value)]=value
        raw[7+len(value):9+len(value)]=b'\0\xd8'
        c=field(bytes(raw),7,64,'utf-16-le','texture','fixture')
        out,spans=rewrite_prefixes(bytes(raw),[c,c],{'txrs/old.txr':'txrs/new.txr'})
        self.assertEqual(1,len(spans))
        self.assertEqual(bytes(raw)[:7],out[:7])
        self.assertEqual(bytes(raw)[7+len(value):],out[7+len(value):])
        self.assertEqual('Txrs\\old.txr',c.literal)
        self.assertEqual('txrs/new.txr',field(out,7,64,'utf-16-le','texture','fixture').path)

    def test_stale_overflow_and_resource_type_changes_refuse(self):
        raw=b'old.txr\0'+b'\xa5'*24
        c=field(raw,0,32,'ascii','texture','fixture','txrs')
        for alias in ('mdrs/new.mod','txrs/'+'x'*40+'.txr'):
            with self.assertRaises(ValueError):rewrite_prefixes(raw,[c],{'txrs/old.txr':alias})
        with self.assertRaisesRegex(ValueError,'stale'):
            rewrite_prefixes(b'x'+raw[1:],[c],{'txrs/old.txr':'txrs/new.txr'})

    def test_header_counted_materials_and_six_fang_fields_keep_other_bytes(self):
        model=bytearray(25+256+580)
        model[4:8]=b'ZOA\0';struct.pack_into('<iiii',model,9,1,1,0,0)
        text='Txrs\\sample.txr'.encode('utf-16-le')+b'\0\0'
        model[25+256+68:25+256+68+len(text)]=text
        carriers=model_materials(bytes(model));self.assertEqual(1,len(carriers))
        out,spans=rewrite_prefixes(bytes(model),carriers,{'txrs/sample.txr':'txrs/scoped.txr'})
        self.assertEqual(model[:349],out[:349])
        self.assertEqual(349,spans[0]['offset'])
        fang=bytearray(732)
        for i in range(6):
            value=f'mdrs/{i}.mod'.encode('utf-16-le')+b'\0\0';fang[6+100*i:6+100*i+len(value)]=value
        self.assertEqual(6,len(fixed_records(bytes(fang),'source-funnel-record')))
