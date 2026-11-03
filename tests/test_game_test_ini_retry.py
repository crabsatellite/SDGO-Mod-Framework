from pathlib import Path
import importlib.util
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('game_test',Path(__file__).resolve().parents[1]/'tools/game_test.py')
game_test=importlib.util.module_from_spec(spec);spec.loader.exec_module(game_test)

class IniReplacementRetry(unittest.TestCase):
    def error(self,code):
        error=PermissionError('native Windows file lock');error.winerror=code;return error

    def test_transient_windows_lock_retries_the_same_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'settings.tmp';target=Path(directory)/'settings.ini'
            source.write_text('new');target.write_text('old');replace=game_test.os.replace
            calls=[]
            def locked_then_ready(a,b):
                calls.append((a,b))
                if len(calls)==1:raise self.error(5)
                return replace(a,b)
            with patch.object(game_test.os,'replace',side_effect=locked_then_ready),patch.object(game_test.time,'sleep'):
                game_test.replace_ini(source,target)
            self.assertEqual(calls,[(source,target),(source,target)])
            self.assertEqual(target.read_text(),'new')

    def test_deadline_preserves_original_and_propagates_the_real_error(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'settings.tmp';target=Path(directory)/'settings.ini'
            source.write_text('new');target.write_text('old')
            with patch.object(game_test.os,'replace',side_effect=self.error(32)):
                with self.assertRaises(PermissionError):game_test.replace_ini(source,target,timeout=0)
            self.assertEqual(target.read_text(),'old');self.assertEqual(source.read_text(),'new')

    def test_other_permission_failures_are_not_retried(self):
        with patch.object(game_test.os,'replace',side_effect=self.error(13)) as replace:
            with self.assertRaises(PermissionError):game_test.replace_ini(Path('a'),Path('b'))
            self.assertEqual(replace.call_count,1)

if __name__=='__main__':unittest.main()
