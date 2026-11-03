from pathlib import Path
import os,sys,unittest
from unittest.mock import patch
from manager.windows import process_image_paths


@unittest.skipUnless(os.name=='nt','Windows process-image API')
class ProcessImageTests(unittest.TestCase):
    def test_current_process_is_found_without_spawning_a_shell(self):
        with patch('subprocess.run',side_effect=AssertionError('must not spawn a shell')):
            paths=process_image_paths(Path(sys.executable).name)
        self.assertIn(Path(sys.executable).resolve(),[Path(p).resolve() for p in paths])

    def test_missing_image_and_invalid_name(self):
        self.assertEqual(process_image_paths('sdgo-unit-fixture-not-running.exe'),[])
        with self.assertRaises(ValueError):process_image_paths('../GOnline.exe')
