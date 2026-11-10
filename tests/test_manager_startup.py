import os
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch,Mock
from manager.instance import Lease
from tools import mod_manager


class StartupWorkspace(unittest.TestCase):
    @unittest.skipUnless(os.name=='nt','Windows directory junction')
    def test_redirected_manager_workspace_remains_the_same_client_workspace(self):
        with tempfile.TemporaryDirectory()as temporary:
            root=Path(temporary);game=root/'client';game.mkdir();(game/'GOnline.exe').write_bytes(b'fixture')
            actual=root/'state';actual.mkdir();linked=game/'sdgo/local-state/manager';linked.parent.mkdir(parents=True)
            subprocess.run(['cmd','/d','/c','mklink','/J',str(linked),str(actual)],check=True,capture_output=True)
            self.assertEqual(linked.resolve(),actual.resolve())
            args=SimpleNamespace(workspace=linked,startup_client=game,no_browser=True,port=0)
            server=Mock(server_port=12345)
            with patch.object(mod_manager,'attach'),patch('manager.updates.attach'),patch.object(mod_manager,'make_server',return_value=server),patch.object(mod_manager,'announce'):
                mod_manager.serve(args)
            server.serve_forever.assert_called_once();server.server_close.assert_called_once()
            lease=Lease(actual)
            try:self.assertIsNotNone(lease.file,'startup must release the workspace lease')
            finally:lease.close()

    def test_another_workspace_is_rejected_and_its_lease_is_released(self):
        with tempfile.TemporaryDirectory()as temporary:
            root=Path(temporary);game=root/'client';game.mkdir();(game/'GOnline.exe').write_bytes(b'fixture')
            state=root/'other';args=SimpleNamespace(workspace=state,startup_client=game,no_browser=True,port=0)
            with self.assertRaisesRegex(ValueError,'启动工作区不属于'):mod_manager.serve(args)
            lease=Lease(state)
            try:self.assertIsNotNone(lease.file)
            finally:lease.close()
