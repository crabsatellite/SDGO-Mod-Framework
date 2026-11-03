from pathlib import Path
import os,tempfile,unittest
from unittest.mock import patch
from manager.windows import product_name


@unittest.skipUnless(os.name=='nt','Windows version-resource API')
class ProductNameTests(unittest.TestCase):
    def test_plain_file_has_no_product_name_without_spawning_a_shell(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'unversioned.dll';path.write_bytes(b'not a PE version resource')
            with patch('subprocess.run',side_effect=AssertionError('must not start a shell')):
                self.assertIsNone(product_name(path))

    def test_actual_framework_version_resource(self):
        path=Path(__file__).resolve().parents[1]/'build/Release/unit_catalog.dll'
        if not path.is_file():self.skipTest('Build the Win32 unit catalog first')
        with patch('subprocess.run',side_effect=AssertionError('must not execute the target')):
            self.assertEqual(product_name(path),'SDGO mod framework')
