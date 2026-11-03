from pathlib import Path
import os
import tempfile
import unittest
from unittest.mock import patch
from tools import game_test


class AtomicWriteTests(unittest.TestCase):
    def test_transient_windows_reader_does_not_abort_a_native_run(self):
        with tempfile.TemporaryDirectory() as directory:
            file=Path(directory)/'quiet-runtime.json'
            file.write_text('old',encoding='utf-8')
            original=os.replace
            attempts=[]
            def replace(source,target):
                attempts.append(source)
                if len(attempts)<3:raise PermissionError('reader sharing lock')
                original(source,target)
            with patch.object(game_test.os,'replace',side_effect=replace),patch.object(game_test.time,'sleep'):
                game_test.atomic_write(file,'new')
            self.assertEqual(file.read_text(encoding='utf-8'),'new')
            self.assertEqual(len(attempts),3)
            self.assertEqual(list(Path(directory).glob('*.tmp')),[])

    def test_permanent_denial_keeps_the_prior_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            file=Path(directory)/'quiet-runtime.json';file.write_text('old',encoding='utf-8')
            with patch.object(game_test.os,'replace',side_effect=PermissionError()),patch.object(game_test.time,'sleep'):
                with self.assertRaises(PermissionError):game_test.atomic_write(file,'new')
            self.assertEqual(file.read_text(encoding='utf-8'),'old')
            self.assertEqual(list(Path(directory).glob('*.tmp')),[])
