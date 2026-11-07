from pathlib import Path
import shutil,tempfile,unittest
from tools.game_test import copy_ignore


class ClientCopyScope(unittest.TestCase):
    def test_manager_state_and_locks_are_not_cloned_into_native_clients(self):
        with tempfile.TemporaryDirectory()as temporary:
            root=Path(temporary);source=root/'source';target=root/'target'
            state=source/'sdgo/local-state/manager';state.mkdir(parents=True)
            (state/'manager-instance.lock').write_bytes(b'lock')
            (state/'private-settings.json').write_bytes(b'private')
            content=source/'sdgo/mods/example';content.mkdir(parents=True)
            (content/'profile.bin').write_bytes(b'installed gameplay data')
            shutil.copytree(source,target,ignore=copy_ignore)
            self.assertFalse((target/'sdgo/local-state').exists())
            self.assertEqual((target/'sdgo/mods/example/profile.bin').read_bytes(),b'installed gameplay data')
