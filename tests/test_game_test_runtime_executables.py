from pathlib import Path
import tempfile
import unittest
from unittest import mock
from tools import game_test


class DirectRuntimeExecutableTests(unittest.TestCase):
    def test_direct_mode_protects_game_and_leaves_unused_desktop_tools_out(self):
        self.assertFalse(game_test.unused_direct_executable(Path('GOnline.exe')))
        self.assertFalse(game_test.unused_direct_executable(Path('d3d9.dll')))
        self.assertTrue(game_test.unused_direct_executable(Path('unused-tool.exe')))
        self.assertFalse(game_test.unused_direct_executable(Path('unused-tool.exe'),'selected.exe'))

    def test_reuse_does_not_read_unselected_helper_but_still_rejects_game_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);game=root/'GOnline.exe';helper=root/'unused-tool.exe'
            game.write_bytes(b'game');helper.write_bytes(b'helper')
            metadata={'client':{'root':str(root),'immutable_hashes':
                {'GOnline.exe':game_test.sha256(game),'unused-tool.exe':game_test.sha256(helper)}},'plan':{'launcher_path':''}}
            original=game_test.sha256
            def protected(path):
                if path==helper:raise PermissionError('quarantined unrelated desktop helper')
                return original(path)
            with mock.patch.object(game_test,'sha256',side_effect=protected):
                game_test.verify_hashes(metadata,for_reuse=True)
                with self.assertRaises(PermissionError):game_test.verify_hashes(metadata)
                metadata['plan']['launcher_path']='unused-tool.exe'
                with self.assertRaises(PermissionError):game_test.verify_hashes(metadata,for_reuse=True)
            metadata['plan']['launcher_path']='';game.write_bytes(b'changed game')
            with self.assertRaises(game_test.GameTestError):game_test.verify_hashes(metadata,for_reuse=True)


if __name__=='__main__':unittest.main()
