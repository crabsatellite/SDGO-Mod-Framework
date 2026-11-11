from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock,patch
from tools import mod_manager


class InvalidGuiStdout:
    def write(self,*_):raise OSError(22,'Invalid argument')
    def flush(self):raise OSError(22,'Invalid argument')


class ManagerGuiStartupContracts(unittest.TestCase):
    def test_invalid_redirected_gui_stdout_does_not_block_http_loop_or_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);workspace=SimpleNamespace(root=root,services=SimpleNamespace(restore=Mock()),close=Mock())
            lease=SimpleNamespace(file=object(),close=Mock());server=SimpleNamespace(server_port=12345,serve_forever=Mock(),server_close=Mock())
            with patch.object(mod_manager,'Lease',return_value=lease),patch.object(mod_manager,'Workspace',return_value=workspace),\
                 patch.object(mod_manager,'attach'),patch('manager.updates.attach'),\
                 patch.object(mod_manager,'make_server',return_value=server),patch.object(mod_manager.sys,'stdout',InvalidGuiStdout()):
                mod_manager.main(['--workspace',str(root),'--no-browser'])
            server.serve_forever.assert_called_once();server.server_close.assert_called_once();workspace.close.assert_called_once();lease.close.assert_called_once()
            self.assertTrue((root/'manager-session.json').is_file())

    def test_existing_instance_can_reopen_with_invalid_gui_stdout(self):
        lease=SimpleNamespace(file=None,existing_url=lambda:'http://127.0.0.1:12345/',close=Mock())
        with patch.object(mod_manager,'Lease',return_value=lease),patch.object(mod_manager.sys,'stdout',InvalidGuiStdout()):
            mod_manager.main(['--no-browser'])
        lease.close.assert_called_once()


if __name__=='__main__':unittest.main()
