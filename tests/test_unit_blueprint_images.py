import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock,patch

from PIL import Image
from owner.units.presentation import (
    BLUEPRINT_FRAME_SAMPLE_COUNT,
    BLUEPRINT_PREVIEW_SIZE,
    _clear_native_blueprint_frame_cache,
    _native_blueprint_frame,
    blueprint_images,
)


class BlueprintImages(unittest.TestCase):
    def tearDown(self):
        _clear_native_blueprint_frame_cache()

    def test_reuses_capsule_crop_inside_actual_native_blueprint_frame(self):
        image=Image.new('RGBA',(64,64))
        image.paste((220,160,30,255),(10,10,50,50))
        stream=io.BytesIO();image.save(stream,'PNG');raw=stream.getvalue()
        import hashlib
        sha=hashlib.sha256(raw).hexdigest()
        engine=SimpleNamespace(baseline=SimpleNamespace(root=Path('.')),blob=Mock(return_value=raw))
        family={'root_unit_id':90001,'name':'Example','forms':[{'id':90001,
            'server':{'UnitGradeNumber':1,'UnitGradeNumber2':1}}],
            'presentation':{'capsule':{'asset_sha':sha},'shop':{'asset_sha':'unrelated-shop'}}}
        frame=Image.new('RGBA',BLUEPRINT_PREVIEW_SIZE)
        for x in range(BLUEPRINT_PREVIEW_SIZE[0]):
            frame.putpixel((x,0),(75,81,85,255))
            frame.putpixel((x,BLUEPRINT_PREVIEW_SIZE[1]-1),(75,81,85,255))
        with patch('manager.engines.capsules.capsule_assets.Assets') as assets, \
                patch('owner.units.presentation._native_blueprint_frame',return_value=frame):
            assets.return_value._head_crop.return_value=Image.new('RGBA',(163,64),(220,160,30,255))
            output=blueprint_images(engine,family)
            assets.return_value._head_crop.assert_called_once_with(90001,0)
        engine.blob.assert_called_once_with(sha)
        for key,size in (('blueprint_preview',BLUEPRINT_PREVIEW_SIZE),('blueprint_card',(183,87))):
            result=Image.open(io.BytesIO(output[key])).convert('RGBA')
            self.assertEqual(size,result.size)
            self.assertEqual((220,160,30,255),result.getpixel((size[0]//2,size[1]//2)))
        preview=Image.open(io.BytesIO(output['blueprint_preview'])).convert('RGBA')
        for y in range(frame.height):
            for x in range(frame.width):
                pixel=frame.getpixel((x,y))
                if pixel[3]:self.assertEqual(pixel,preview.getpixel((x,y)))
        self.assertNotIn((9,200,20,255),preview.getdata())

    @staticmethod
    def _native_rows():
        rows=[];images={}
        width,height=BLUEPRINT_PREVIEW_SIZE
        for index in range(BLUEPRINT_FRAME_SAMPLE_COUNT):
            path=f'sprs/frame_sample_{index}.spr'
            row=bytearray(1197);row[4]=0
            encoded=(path.replace('/','\\')+'\0').encode('utf-16-le')
            row[289:289+len(encoded)]=encoded
            rows.append(bytes(row))
            image=Image.new('RGBA',(width,height))
            color=(75,81,85,255)
            for x in range(width):
                image.putpixel((x,0),color);image.putpixel((x,height-1),color)
            for y in range(height):
                image.putpixel((0,y),color)
            for step in range(8):
                image.putpixel((width-1-step,step),color)
            image.putpixel((20+index,40),(index+1,120,180,255))
            images[path]=image
        return rows,images

    def test_recovers_only_fixed_native_border_without_donor_unit_pixels(self):
        rows,images=self._native_rows()
        reader=Mock()
        reader.read_resource.return_value=b'native-gdt'
        with patch('manager.engines.capsules.capsule_catalog.ClientData',return_value=reader), \
                patch('manager.engines.capsules.capsule_catalog.gdt_records',
                      return_value=(1197,rows)), \
                patch('manager.engines.capsules.capsule_assets.sprite_frame',
                      return_value={'width':BLUEPRINT_PREVIEW_SIZE[0],
                                    'height':BLUEPRINT_PREVIEW_SIZE[1],
                                    'texture':'txrs/native-frame.txr'}), \
                patch('manager.engines.capsules.capsule_assets.Assets.sprite',
                      autospec=True,side_effect=lambda _renderer,path:images[path]):
            frame=_native_blueprint_frame('native-client')
        self.assertEqual(BLUEPRINT_PREVIEW_SIZE,frame.size)
        expected=Image.new('RGBA',BLUEPRINT_PREVIEW_SIZE)
        color=(75,81,85,255);width,height=BLUEPRINT_PREVIEW_SIZE
        for x in range(width):
            expected.putpixel((x,0),color);expected.putpixel((x,height-1),color)
        for y in range(height):expected.putpixel((0,y),color)
        for step in range(8):expected.putpixel((width-1-step,step),color)
        self.assertEqual(expected.tobytes(),frame.tobytes())

    def test_rejects_fixed_pixels_disconnected_from_native_border(self):
        rows,images=self._native_rows()
        for image in images.values():image.putpixel((126,50),(0,0,0,255))
        reader=Mock()
        reader.read_resource.return_value=b'native-gdt'
        with patch('manager.engines.capsules.capsule_catalog.ClientData',return_value=reader), \
                patch('manager.engines.capsules.capsule_catalog.gdt_records',
                      return_value=(1197,rows)), \
                patch('manager.engines.capsules.capsule_assets.sprite_frame',
                      return_value={'width':BLUEPRINT_PREVIEW_SIZE[0],
                                    'height':BLUEPRINT_PREVIEW_SIZE[1],
                                    'texture':'txrs/native-frame.txr'}), \
                patch('manager.engines.capsules.capsule_assets.Assets.sprite',
                      autospec=True,side_effect=lambda _renderer,path:images[path]):
            with self.assertRaisesRegex(ValueError,'模板机体'):
                _native_blueprint_frame('ambiguous-client')

    def test_cache_key_tracks_native_gdt_and_source_resource_bytes(self):
        rows,first=self._native_rows()
        second={path:image.copy() for path,image in first.items()}
        for image in second.values():
            for x in range(BLUEPRINT_PREVIEW_SIZE[0]):
                image.putpixel((x,0),(25,120,160,255))
        readers=[]
        for marker in (b'first',b'second'):
            reader=Mock()
            reader.read_resource.side_effect=lambda path,m=marker: (
                b'gdt-'+m if path=='gdrs/0029.gdt'
                else m+path.encode('ascii'))
            readers.append(reader)
        calls=0
        def sprite(_renderer,path):
            nonlocal calls
            images=first if calls<BLUEPRINT_FRAME_SAMPLE_COUNT else second
            calls+=1
            return images[path]
        with patch('manager.engines.capsules.capsule_catalog.ClientData',
                   side_effect=readers), \
                patch('manager.engines.capsules.capsule_catalog.gdt_records',
                      return_value=(1197,rows)), \
                patch('manager.engines.capsules.capsule_assets.sprite_frame',
                      return_value={'width':BLUEPRINT_PREVIEW_SIZE[0],
                                    'height':BLUEPRINT_PREVIEW_SIZE[1],
                                    'texture':'txrs/native-frame.txr'}), \
                patch('manager.engines.capsules.capsule_assets.Assets.sprite',
                      autospec=True,side_effect=sprite):
            old=_native_blueprint_frame('same-client')
            new=_native_blueprint_frame('same-client')
        self.assertEqual((75,81,85,255),old.getpixel((120,0)))
        self.assertEqual((25,120,160,255),new.getpixel((120,0)))
        self.assertEqual(BLUEPRINT_FRAME_SAMPLE_COUNT*2,calls)


if __name__=='__main__':unittest.main()
