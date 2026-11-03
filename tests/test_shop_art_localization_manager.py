"""The artwork patch uses the existing installed-contract Manager workflow."""
import base64
import io
from pathlib import Path
import tempfile
import unittest
import zipfile

from manager import packages
from manager.workspace import Workspace

ROOT = Path(__file__).resolve().parents[1]


class ArtworkManagerTests(unittest.TestCase):
    def test_existing_manager_discovers_and_toggles_installed_artwork_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = root / 'client'
            client.mkdir()
            (client / 'GOnline.exe').write_bytes(b'fixture')
            contracts = root / 'older-manager-contracts'
            contracts.mkdir()
            workspace = Workspace(root / 'state', contracts)
            try:
                workspace.add_host(client)
                host_id = workspace.settings['hosts'][0]['id']
                data = io.BytesIO()
                with zipfile.ZipFile(data, 'w') as archive:
                    for name in ('mod.ini', 'manager.json', 'symbols.fragment.ini'):
                        archive.writestr('client/sdgo/mods/shop_art_localization/' + name,
                                         (ROOT / 'mods/shop_art_localization' / name).read_bytes())
                    archive.writestr('client/sdgo/mods/shop_art_localization/shop_art_localization.dll', b'fixture')
                    archive.writestr('symbols.fragment.ini', (ROOT / 'mods/shop_art_localization/symbols.fragment.ini').read_bytes())
                packages.import_client(workspace, host_id, base64.b64encode(data.getvalue()).decode('ascii'))
                host = workspace.public()['hosts'][0]
                mod = next(m for m in host['mods'] if m['id'] == 'shop_art_localization')
                self.assertTrue(mod['registered'])
                self.assertTrue(mod['manifest_enabled'])
                details = workspace.details(host_id, mod['id'])
                self.assertEqual(details['contract']['title'], '商城图片文字汉化')
                workspace.toggle(host_id, mod['id'], False,
                                 {'global': host['global_revision'], 'manifest': mod['revision']})
                self.assertFalse(next(m for m in workspace.public()['hosts'][0]['mods'] if m['id'] == mod['id'])['manifest_enabled'])
            finally:
                workspace.services.close()
