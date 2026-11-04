import struct
import unittest

from owner.units import blueprint_codec as c
from test_unit_blueprints import declaration


def donor():
    raw=bytearray(b'Z'*1197)
    struct.pack_into('<I',raw,0,200001);raw[4]=0;raw[5:105]=bytes(100)
    text='Original'.encode('utf-16-le');raw[5:5+len(text)]=text
    raw[169:229]=bytes(60)
    sprite='sprs/recipe.spr'.encode('utf-16-le');raw[169:169+len(sprite)]=sprite
    raw[289:349]=bytes(60)
    preview='sprs/preview-old.spr'.encode('utf-16-le');raw[289:289+len(preview)]=preview
    struct.pack_into('<II',raw,105,3,12345)
    struct.pack_into('<I',raw,229,16003)
    struct.pack_into('<7I',raw,233,13003,16002,15004,0,0,0,0)
    struct.pack_into('<7I',raw,261,7,1,1,0,0,0,0)
    struct.pack_into('<II',raw,1189,0,600)
    return bytes(raw)


class BlueprintCodec(unittest.TestCase):
    def test_dword_layout_and_known_patch_spans_preserve_other_bytes(self):
        raw=donor();value=declaration()
        output=c.patch(raw,value,root_unit_id=90001)
        decoded=c.decode(output)
        self.assertEqual(209000,decoded['id']);self.assertEqual(90001,decoded['output_unit_id'])
        self.assertEqual(value['materials'],decoded['materials'])
        spans=[(0,4),(5,105),(105,109),(229,289),(1189,1197)]
        for offset,(before,after) in enumerate(zip(raw,output)):
            if not any(begin<=offset<end for begin,end in spans):self.assertEqual(before,after,offset)

    def test_count_security_extent_and_sparse_slots_rejected(self):
        for offset,value in [(105,2),(105,8),(245,10001)]:
            raw=bytearray(donor());struct.pack_into('<I',raw,offset,value)
            with self.subTest(offset=offset,value=value),self.assertRaises(ValueError):c.decode(bytes(raw))
        raw=bytearray(donor());raw[4]=1
        with self.assertRaises(ValueError):c.decode(bytes(raw))
        with self.assertRaises(ValueError):c.decode(donor()[:-1])

    def test_template_identity_and_native_sprite_capacity(self):
        value=declaration();value['recipe_template_id']=200002
        with self.assertRaises(ValueError):c.patch(donor(),value,root_unit_id=90001)
        value=declaration()
        with self.assertRaises(ValueError):c.patch(donor(),value,root_unit_id=90001,recipe_sprite='sprs/'+'x'*40+'.spr')

    def test_inactive_level_dwords_survive_donor_preserving_patch(self):
        raw=bytearray(donor());struct.pack_into('<I',raw,277,1)
        output=c.patch(bytes(raw),declaration(),root_unit_id=90001)
        self.assertEqual(1,struct.unpack_from('<I',output,277)[0])
        self.assertEqual([0,1,0,0],c.decode(output)['unused_slot_levels'])

    def test_both_output_pictures_can_be_rebound_without_changing_material_sprites(self):
        raw=donor()
        output=c.patch(raw,declaration(),root_unit_id=90001,
            recipe_sprite='sprs/unit-card.spr',preview_sprite='sprs/unit-preview.spr')
        self.assertEqual(raw[109:169],output[109:169])
        self.assertEqual('sprs\\unit-card.spr',output[169:229].decode('utf-16-le').split('\0')[0])
        self.assertEqual('sprs\\unit-preview.spr',output[289:349].decode('utf-16-le').split('\0')[0])
        self.assertEqual(raw[349:1189],output[349:1189])


if __name__=='__main__':unittest.main()
