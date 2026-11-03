"""Three native consumers retain their own images; a single source cannot alias all."""
import hashlib
import io
from pathlib import Path
import struct
from types import SimpleNamespace
import unittest

from PIL import Image
from owner.units.assets import sprite_binary
from owner.units.presentation import unit_images,shop_image,capsule_source
from unit_import_support import unit,wchar,png


class PresentationTests(unittest.TestCase):
    def fixture(self):
        blobs={};files=[];assets=[]
        def blob(raw):
            sha=hashlib.sha256(raw).hexdigest();blobs[sha]=raw;return sha
        raw=bytearray(unit())
        for offset,name,size,color in ((4654,'shop',(256,256),'red'),(4754,'active',(256,256),'green'),(4854,'custom',(92,92),'blue')):
            texture='txrs/'+name+'.txr';sprite='sprs/'+name+'.spr'
            out=io.BytesIO();Image.new('RGBA',size,color).save(out,'PNG')
            for path,data in ((texture,out.getvalue()),(sprite,sprite_binary(texture.replace('/','\\'),*size))):
                files.append({'path':path,'sha':blob(data)})
            wchar(raw,offset,sprite.replace('/','\\'))
        selected=blob(png('orange'));assets.append({'sha':selected,'source':'package-sprite'})
        family={'root_unit_id':90001,'name':'Fixture','files':files,'assets':assets,
                'forms':[{'id':90001,'source_sha':blob(bytes(raw))}],
                'presentation':{role:{'asset_sha':selected,'source_mode':'native'} for role in ('shop','capsule')}}
        engine=SimpleNamespace(blob=lambda sha:blobs[sha],baseline=SimpleNamespace(read=lambda path:None))
        return engine,family

    def test_native_product_states_and_custom_portrait_remain_distinct(self):
        engine,family=self.fixture();images=unit_images(engine,family)
        expected={'unit_primary':((256,256),(255,0,0,255)),'shop':((256,256),(0,128,0,255)),
                  'custom':((92,92),(0,0,255,255))}
        for key,(size,color) in expected.items():
            image=Image.open(io.BytesIO(images[key]));self.assertEqual(image.size,size);self.assertEqual(image.getpixel((20,20)),color)
        self.assertEqual(Image.open(io.BytesIO(shop_image(engine,family))).size,(122,122))
        self.assertEqual(Image.open(io.BytesIO(shop_image(engine,family))).getpixel((20,20)),(255,0,0,255))
        self.assertEqual(shop_image(engine,family,1),images['unit_primary'])
        self.assertEqual(capsule_source(engine,family),images['custom'])

    def test_selected_upload_uses_its_channel_and_native_canvas(self):
        engine,family=self.fixture();family['presentation']['shop']['source_mode']='selected'
        images=unit_images(engine,family)
        self.assertEqual(Image.open(io.BytesIO(images['shop'])).size,(256,256))
        self.assertEqual(Image.open(io.BytesIO(images['shop'])).getpixel((128,128)),(255,165,0,255))
        self.assertEqual(Image.open(io.BytesIO(shop_image(engine,family))).size,(122,122))
        self.assertEqual(Image.open(io.BytesIO(shop_image(engine,family))).getpixel((61,61)),(255,165,0,255))
        self.assertEqual(Image.open(io.BytesIO(images['custom'])).getpixel((20,20)),(0,0,255,255))


if __name__=='__main__':unittest.main()
