import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from owner.collection_capsule import acquisition
from owner.collection_capsule.rules import defaults
from test_collection_client import make_client,make_cover,build_registry
from test_capsule_client_runtime import write_bundle,harness
import subprocess

class CollectionAcquisitionTests(unittest.TestCase):
    def test_roots_and_rules_are_bound_without_alternate_form_guesses(self):
        rules={**defaults(),'price_gp':70000,'limit_period':'day','draw_limit':3}
        raw,meta=acquisition.encode([{'unit_id':22150},{'unit_id':24733}],[],rules)
        self.assertEqual(struct.unpack_from('<8sIII',raw)[:2],(b'SDGOACQ1',2))
        self.assertEqual(meta['unit_ids'],[22150,24733])
        self.assertIn('70,000 GB',meta['text']);self.assertIn('每日3次',meta['text'])
        self.assertEqual(meta['source_text_offset'],214)
        rules.update(limit_period='none',draw_limit=0)
        self.assertIn('不限次',acquisition.encode([22150],[],rules)[1]['text'])

    def test_disable_removes_own_explanations_and_keeps_other_registry_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);game=make_client(root/'game');cover=root/'cover.png';make_cover(cover)
            build_registry(game,cover,root/'enabled')
            original=json.loads((root/'enabled/manifest.json').read_text(encoding='utf-8'))
            self.assertEqual(original['collection_acquisition']['root_count'],6)
            self.assertTrue((root/'enabled'/acquisition.FILE).is_file())
            build_registry(game,cover,root/'disabled',current=root/'enabled',enabled=False)
            self.assertFalse((root/'disabled'/acquisition.FILE).exists())
            self.assertNotIn('[acquisition]',(root/'disabled/runtime.ini').read_text())
            self.assertIsNone(json.loads((root/'disabled/manifest.json').read_text())['collection_acquisition'])

    def test_actual_native_decoder_and_text_writer_preserve_other_record_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);write_bundle(root)
            raw,_=acquisition.encode([22150],[],defaults())
            (root/acquisition.FILE).write_bytes(raw)
            with (root/'runtime.ini').open('a') as f:f.write('\n[acquisition]\nfile='+acquisition.FILE+'\nsha256='+hashlib.sha256(raw).hexdigest()+'\n')
            result=subprocess.run([str(harness()),str(root)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('ACQUISITION 1',result.stdout)
            (root/acquisition.FILE).write_bytes(raw+b'changed')
            failed=subprocess.run([str(harness()),str(root)],capture_output=True,text=True)
            self.assertNotEqual(failed.returncode,0)
            self.assertIn('SHA-256 mismatch',failed.stdout)

if __name__=='__main__':unittest.main()
