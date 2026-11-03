"""Fixture tests for cropping/lineage only; these do not execute a native model."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from PIL import Image
from tools import game_test_portraits as portraits


class PortraitExportTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        scope=patch.object(portraits,'ROOT',self.root);scope.start();self.addCleanup(scope.stop)
        self.run=self.root/'build/run';self.visual=self.run/'client/sdgo/gametest/visual';self.visual.mkdir(parents=True)
        self.output=self.root/'build/native-unit-portraits/unit'
        self.meta={'run_id':'fixture-run','source':{'commit':'fixture'},'client':{'root':str(self.run/'client'),
            'immutable_hashes':{'GOnline.exe':'a'*64,'mdrs/example.mod':'b'*64}}}
        (self.run/'run.json').write_text(json.dumps(self.meta))
        case_names=('framework.bootstrap','flow.login','setup.active_unit','pages.unit_index.pose_wait','pages.unit_index.pose_ready')
        self.report={'status':'pass','layers':{'source':{'status':'bound'},'native':{'cases':{
            name:{'status':'pass'} for name in case_names}}}}
        (self.run/'receipts').mkdir();self.save_report()
        for name,count in (('pages.unit_index.pose_wait',2),('pages.unit_index.pose_ready',3)):
            description={'schema':1,'unit_id':12345,'model':'mdrs/example.mod','pose_handle':7,
                'animation':'choice_01' if name.endswith('ready') else 'wait_01',
                'viewport':[2,1,5,6],'renderer':'original-unit-detail-backbuffer'}
            (self.visual/(name+'.json')).write_text(json.dumps(description))
            for index in range(count):
                image=Image.new('RGB',(10,8),(255,0,0))
                for y in range(1,7):
                    for x in range(2,7):image.putpixel((x,y),(index*20+1,x,y))
                image.save(self.visual/(name+'-'+str(index)+'.bmp'))

    def save_report(self):
        (self.run/'receipts/report.json').write_text(json.dumps(self.report))

    def test_exact_native_viewport_crop_retains_only_requested_pixels_and_lineage(self):
        result=portraits.export_portraits(self.run,self.output,12345)
        self.assertEqual(result['pictures'],5)
        manifest=json.loads((self.output/'manifest.json').read_text())
        self.assertFalse(manifest['ai_generated'])
        self.assertFalse(manifest['public_tool_default_content'])
        for picture in manifest['pictures']:
            path=self.output/picture['path']
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),picture['sha256'])
            with Image.open(path) as image:
                self.assertEqual(image.size,(5,6));self.assertEqual(image.getpixel((0,0))[1:],(2,1))
                self.assertNotIn((255,0,0),set(image.getdata()))
            self.assertEqual(picture['model_sha256'],'b'*64)

    def test_wrong_unit_and_missing_native_pose_proof_are_rejected(self):
        with self.assertRaisesRegex(ValueError,'requested model'):portraits.export_portraits(self.run,self.output,99999)
        self.assertFalse(self.output.exists())
        self.report['layers']['native']['cases']['pages.unit_index.pose_ready']['status']='fail';self.save_report()
        with self.assertRaisesRegex(ValueError,'have not passed'):portraits.export_portraits(self.run,self.output,12345)

    def test_output_cannot_overwrite_existing_content_or_escape_the_authoring_cache(self):
        self.output.mkdir(parents=True);(self.output/'keep.txt').write_text('keep')
        with self.assertRaises(ValueError):portraits.export_portraits(self.run,self.output,12345)
        self.assertEqual((self.output/'keep.txt').read_text(),'keep')
        with self.assertRaises(ValueError):portraits.export_portraits(self.run,self.root/'outside',12345)


if __name__=='__main__':unittest.main()
