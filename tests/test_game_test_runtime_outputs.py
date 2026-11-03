from pathlib import Path
import tempfile
import unittest
from tools import game_test


class RuntimeOutputTests(unittest.TestCase):
    def test_announcement_page_is_a_sealed_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            page='sdgo/mods/announcements/pages/notice_123.html'
            for name in ('GOnline.exe','sdgo/core.dll','sdgo/sdgo.ini','sdgo/gametest/run.ini',page):
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'initial')
            (root/'sdgo/symbols').mkdir()
            hashes,_=game_test.collect_hashes(root)
            self.assertIn(page,hashes)
            metadata={'client':{'root':str(root),'immutable_hashes':hashes},'plan':{}}
            (root/page).write_bytes(b'changed announcement content')
            with self.assertRaises(game_test.GameTestError):game_test.verify_hashes(metadata)

    def test_capsule_receipt_is_output_but_runtime_config_stays_sealed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for name in ('GOnline.exe','sdgo/core.dll','sdgo/sdgo.ini','sdgo/gametest/run.ini',
                         'sdgo/mods/capsule_catalog/runtime.ini','sdgo/mods/capsule_catalog/runtime-receipt.ini'):
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'initial')
            (root/'sdgo/symbols').mkdir()
            hashes,_=game_test.collect_hashes(root)
            receipt='sdgo/mods/capsule_catalog/runtime-receipt.ini'
            config='sdgo/mods/capsule_catalog/runtime.ini'
            self.assertNotIn(receipt,hashes)
            self.assertIn(config,hashes)
            metadata={'client':{'root':str(root),'immutable_hashes':hashes},'plan':{}}
            (root/receipt).write_bytes(b'new native runtime observation')
            game_test.verify_hashes(metadata)
            (root/config).write_bytes(b'changed real input')
            with self.assertRaises(game_test.GameTestError):game_test.verify_hashes(metadata)
