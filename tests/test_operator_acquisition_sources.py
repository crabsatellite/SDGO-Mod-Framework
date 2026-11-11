import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

from manager import storage
from manager.engines.operators import acquisition, install_package, model
from manager.engines.operators.workspace import Workspace
from owner.units import acquisition_sources as acquisition_protocol


def catalog():
    return {
        'schema_version': 1,
        'native_operator_ids': [60003],
        'client_index_sha256': 'fixture',
        'uploaded_assets': {},
        'native_server_data': {},
        'default_pack_version': 1,
        'items': [
            {'id': 1210002, 'name': 'operator', 'ref_index': 60003, 'time_limit': 0},
            {'id': 1520001, 'name': 'plan A', 'ref_index': 200001, 'time_limit': 0},
            {'id': 1520002, 'name': 'plan A alias', 'ref_index': 200001, 'time_limit': 0},
            {'id': 1520003, 'name': 'rental plan', 'ref_index': 200002, 'time_limit': 1},
        ],
        'drop_pools': {
            'blueprints': {
                'label': '随机有效设计图',
                'category_ids': [1520001, 1520002, 1520003],
            },
        },
        'operators': [{
            'id': 60003,
            'category_id': 1210002,
            'template_id': 60003,
            'name': '设计图补给官·希美',
            'managed': True,
            'enabled': True,
            'delivery': 'all_players',
            'avatar': {'kind': 'native', 'operator_id': 60003},
            'drops': [{
                'category_id': 1520001,
                'pool': 'blueprints',
                'chance_bps': 10000,
                'min_amount': 1,
                'max_amount': 1,
            }],
        }],
    }


class OperatorAcquisitionSourcesTests(unittest.TestCase):
    def test_enabled_default_policy_expands_pool_and_deduplicates_recipe_aliases(self):
        value = acquisition.compile_sources(catalog())
        self.assertEqual(['200001'], list(value['blueprints']))
        self.assertEqual([{
            'key': 'operator:60003:blueprint:200001',
            'text': '副官掉落：设计图补给官·希美',
        }], value['blueprints']['200001'])
        self.assertNotIn('200002', value['blueprints'])

    def test_disabled_operator_and_zero_probability_clear_claims(self):
        for change in ({'enabled': False}, {'drops': [{
                'category_id': 1520001, 'pool': 'blueprints', 'chance_bps': 0,
                'min_amount': 1, 'max_amount': 1,
        }]}):
            value = catalog()
            value['operators'][0].update(change)
            self.assertEqual({}, acquisition.compile_sources(value)['blueprints'])

    def test_snapshot_rejects_tampered_claims(self):
        value = acquisition.snapshot(catalog())
        value['blueprints']['200001'][0]['text'] = '伪造来源'
        with self.assertRaisesRegex(ValueError, '不一致'):
            acquisition.public_snapshot(value)

    def test_apply_writes_snapshot_only_after_success(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = Workspace.__new__(Workspace)
            workspace.root = Path(directory)
            workspace.server_ini = Path(directory) / 'WorldServer.ini'
            workspace.server_root = None
            workspace.package_root = None
            client = Path(directory) / 'client'
            module = client / 'sdgo/mods/operator_catalog'
            module.mkdir(parents=True)
            workspace.assets = Mock()
            workspace.assets.root = client
            current = catalog()
            workspace.check = lambda expected: current
            with patch('manager.engines.operators.workspace.server.apply_catalog', return_value={'revision': model.revision(current)}):
                result = Workspace.apply(workspace, model.revision(current))
            self.assertEqual(model.revision(current), result['acquisition_source_revision'])
            self.assertEqual(acquisition.compile_sources(current), acquisition.read(workspace.root / acquisition.SNAPSHOT_NAME))
            self.assertEqual(acquisition.compile_sources(current), json.loads(
                (module / 'acquisition-sources.json').read_text(encoding='utf-8')))
            self.assertEqual(acquisition_protocol.SOURCE_MAGIC,
                             (module / 'acquisition-sources.bin').read_bytes()[:8])
            self.assertTrue(result['acquisition_sources_client_installed'])
            self.assertTrue(result['acquisition_sources_restart_required'])

    def test_apply_refreshes_fresh_package_ownership_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = root / 'client'
            module = client / 'sdgo/mods/operator_catalog'
            initial = catalog()
            workspace = Workspace.__new__(Workspace)
            workspace.root = root
            workspace.server_ini = root / 'WorldServer.ini'
            workspace.server_root = None
            workspace.package_root = None
            workspace.module_binary = root / 'operator_catalog.dll'
            workspace.module_binary.write_bytes(b'MZ fixture')
            workspace.assets = Mock()
            workspace.assets.root = client
            workspace.assets.records.return_value = {}
            workspace.read = lambda: initial
            status = {
                'database_applied': True,
                'applied_revision': model.revision(initial),
            }
            with patch('manager.engines.operators.workspace.server.catalog_status',
                       return_value=status):
                package = Workspace.package(workspace)
            with zipfile.ZipFile(io.BytesIO(package)) as archive:
                for name in archive.namelist():
                    prefix = 'client/sdgo/mods/operator_catalog/'
                    if name.startswith(prefix):
                        target = module / name[len(prefix):]
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(archive.read(name))

            manifest_path = module / 'manifest.json'
            before = install_package._inner_manifest(manifest_path.read_bytes())
            updated = copy.deepcopy(initial)
            updated['operators'][0]['name'] = '设计图补给官·希美（更新）'
            workspace.check = lambda expected: updated
            with patch('manager.engines.operators.workspace.server.apply_catalog',
                       return_value={'revision': model.revision(updated)}):
                Workspace.apply(workspace, model.revision(updated))

            after = install_package._inner_manifest(manifest_path.read_bytes())
            self.assertEqual(before['revision'], after['revision'])
            for name, metadata in before['files'].items():
                if name not in ('acquisition-sources.json', 'acquisition-sources.bin'):
                    self.assertEqual(metadata, after['files'][name])
            for name, metadata in after['files'].items():
                raw = (module / name).read_bytes()
                self.assertEqual(len(raw), metadata['bytes'])
                self.assertEqual(hashlib.sha256(raw).hexdigest(), metadata['sha256'])
            source = json.loads((module / 'acquisition-sources.json').read_text(
                encoding='utf-8'))
            self.assertEqual('副官掉落：设计图补给官·希美（更新）',
                             source['blueprints']['200001'][0]['text'])

    def test_malformed_existing_ownership_manifest_fails_closed_after_database_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workspace = Workspace.__new__(Workspace)
            workspace.root = root
            workspace.server_ini = root / 'WorldServer.ini'
            workspace.server_root = None
            workspace.package_root = None
            client = root / 'client'
            module = client / 'sdgo/mods/operator_catalog'
            module.mkdir(parents=True)
            workspace.assets = Mock()
            workspace.assets.root = client
            current = catalog()
            workspace.check = lambda expected: current
            old_json = b'{"old":true}\n'
            old_bin = b'old-binary'
            malformed = b'{"schema_version":1,"files":{}}\n'
            (module / 'acquisition-sources.json').write_bytes(old_json)
            (module / 'acquisition-sources.bin').write_bytes(old_bin)
            (module / 'manifest.json').write_bytes(malformed)
            with patch('manager.engines.operators.workspace.server.apply_catalog',
                       return_value={'revision': model.revision(current)}) as applied:
                with self.assertRaisesRegex(
                        ValueError, '数据库应用已完成.*资源所有权清单'):
                    Workspace.apply(workspace, model.revision(current))
            applied.assert_called_once()
            self.assertFalse((root / acquisition.SNAPSHOT_NAME).exists())
            self.assertEqual(old_json, (module / 'acquisition-sources.json').read_bytes())
            self.assertEqual(old_bin, (module / 'acquisition-sources.bin').read_bytes())
            self.assertEqual(malformed, (module / 'manifest.json').read_bytes())

    def test_manifest_write_failure_rolls_back_snapshot_sidecars_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workspace = Workspace.__new__(Workspace)
            workspace.root = root
            workspace.server_ini = root / 'WorldServer.ini'
            workspace.server_root = None
            workspace.package_root = None
            client = root / 'client'
            module = client / 'sdgo/mods/operator_catalog'
            module.mkdir(parents=True)
            workspace.assets = Mock()
            workspace.assets.root = client
            current = catalog()
            workspace.check = lambda expected: current
            snapshot_path = root / acquisition.SNAPSHOT_NAME
            manifest_path = module / 'manifest.json'
            old_snapshot = b'old-applied-snapshot'
            old_json = b'{"schema_version":1,"blueprints":{}}\n'
            old_bin = b'old-binary'
            snapshot_path.write_bytes(old_snapshot)
            (module / 'acquisition-sources.json').write_bytes(old_json)
            (module / 'acquisition-sources.bin').write_bytes(old_bin)
            manifest_value = {
                'schema_version': 1,
                'module': 'operator_catalog',
                'kind': 'generated-client-resources',
                'revision': '1' * 64,
                'files': {
                    'acquisition-sources.json': {
                        'bytes': len(old_json),
                        'sha256': hashlib.sha256(old_json).hexdigest(),
                    },
                    'acquisition-sources.bin': {
                        'bytes': len(old_bin),
                        'sha256': hashlib.sha256(old_bin).hexdigest(),
                    },
                },
            }
            old_manifest = storage.json_bytes(manifest_value)
            manifest_path.write_bytes(old_manifest)
            real_atomic = storage.atomic
            failed = {'value': False}

            def fail_manifest_once(path, raw):
                if Path(path) == manifest_path and not failed['value']:
                    failed['value'] = True
                    raise OSError('injected manifest write failure')
                return real_atomic(path, raw)

            with patch('manager.engines.operators.workspace.server.apply_catalog',
                       return_value={'revision': model.revision(current)}), patch(
                           'manager.engines.operators.workspace.storage.atomic',
                           side_effect=fail_manifest_once):
                with self.assertRaisesRegex(
                        ValueError, '数据库应用已完成.*injected manifest write failure'):
                    Workspace.apply(workspace, model.revision(current))
            self.assertTrue(failed['value'])
            self.assertEqual(old_snapshot, snapshot_path.read_bytes())
            self.assertEqual(old_json, (module / 'acquisition-sources.json').read_bytes())
            self.assertEqual(old_bin, (module / 'acquisition-sources.bin').read_bytes())
            self.assertEqual(old_manifest, manifest_path.read_bytes())

    def test_failed_database_apply_does_not_publish_draft_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workspace = Workspace.__new__(Workspace)
            workspace.root = root
            workspace.server_ini = root / 'WorldServer.ini'
            workspace.server_root = None
            workspace.package_root = None
            client = root / 'client'
            module = client / 'sdgo/mods/operator_catalog'
            module.mkdir(parents=True)
            workspace.assets = Mock()
            workspace.assets.root = client
            current = catalog()
            workspace.check = lambda expected: current
            old_json = b'{"old":true}\n'
            old_bin = b'old-binary'
            (module / 'acquisition-sources.json').write_bytes(old_json)
            (module / 'acquisition-sources.bin').write_bytes(old_bin)
            with patch('manager.engines.operators.workspace.server.apply_catalog',
                       side_effect=ValueError('database apply failed')):
                with self.assertRaisesRegex(ValueError, 'database apply failed'):
                    Workspace.apply(workspace, model.revision(current))
            self.assertFalse((root / acquisition.SNAPSHOT_NAME).exists())
            self.assertEqual(old_json, (module / 'acquisition-sources.json').read_bytes())
            self.assertEqual(old_bin, (module / 'acquisition-sources.bin').read_bytes())

    def test_package_uses_only_database_verified_applied_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current = catalog()
            workspace = Workspace.__new__(Workspace)
            workspace.root = root
            workspace.server_ini = root / 'WorldServer.ini'
            workspace.module_binary = root / 'operator_catalog.dll'
            workspace.module_binary.write_bytes(b'MZ fixture')
            workspace.assets = Mock()
            workspace.assets.records.return_value = {}
            workspace.read = lambda: current
            acquisition.write(root / acquisition.SNAPSHOT_NAME, current)
            status = {'database_applied': True, 'applied_revision': model.revision(current)}
            with patch('manager.engines.operators.workspace.server.catalog_status', return_value=status):
                package = Workspace.package(workspace)
            with zipfile.ZipFile(io.BytesIO(package)) as archive:
                published = json.loads(archive.read('client/sdgo/mods/operator_catalog/acquisition-sources.json'))
                binary = archive.read('client/sdgo/mods/operator_catalog/acquisition-sources.bin')
            self.assertEqual(acquisition.compile_sources(current), published)
            self.assertEqual(acquisition_protocol.SOURCE_MAGIC, binary[:8])
            status['database_applied'] = False
            with patch('manager.engines.operators.workspace.server.catalog_status', return_value=status):
                package = Workspace.package(workspace)
            with zipfile.ZipFile(io.BytesIO(package)) as archive:
                cleared = json.loads(archive.read('client/sdgo/mods/operator_catalog/acquisition-sources.json'))
            self.assertEqual({}, cleared['blueprints'])


    def test_upgrade_without_snapshot_uses_only_exact_current_applied_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current = catalog()
            workspace = Workspace.__new__(Workspace)
            workspace.root = root
            workspace.server_ini = root / 'WorldServer.ini'
            workspace.module_binary = root / 'operator_catalog.dll'
            workspace.module_binary.write_bytes(b'MZ fixture')
            workspace.assets = Mock()
            workspace.assets.records.return_value = {}
            workspace.read = lambda: current
            self.assertFalse((root / acquisition.SNAPSHOT_NAME).exists())
            status = {
                'database_applied': True,
                'applied_revision': model.revision(current),
            }
            with patch('manager.engines.operators.workspace.server.catalog_status',
                       return_value=status):
                package = Workspace.package(workspace)
            with zipfile.ZipFile(io.BytesIO(package)) as archive:
                published = json.loads(archive.read(
                    'client/sdgo/mods/operator_catalog/acquisition-sources.json'))
            self.assertEqual(acquisition.compile_sources(current), published)
            self.assertFalse((root / acquisition.SNAPSHOT_NAME).exists())

            status['applied_revision'] = 'different-draft-or-database-revision'
            with patch('manager.engines.operators.workspace.server.catalog_status',
                       return_value=status):
                package = Workspace.package(workspace)
            with zipfile.ZipFile(io.BytesIO(package)) as archive:
                cleared = json.loads(archive.read(
                    'client/sdgo/mods/operator_catalog/acquisition-sources.json'))
            self.assertEqual({}, cleared['blueprints'])


    def test_applied_upgrade_sources_do_not_require_client_programme_package(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current = catalog()
            workspace = Workspace.__new__(Workspace)
            workspace.root = root
            workspace.server_ini = root / 'WorldServer.ini'
            workspace.read = lambda: current
            status = {
                'database_applied': True,
                'applied_revision': model.revision(current),
            }
            with patch('manager.engines.operators.workspace.server.catalog_status',
                       return_value=status):
                published = Workspace.applied_acquisition_sources(workspace)
            self.assertEqual(acquisition.compile_sources(current), published)
            self.assertFalse(hasattr(workspace, 'module_binary'))

            status['applied_revision'] = 'stale'
            with patch('manager.engines.operators.workspace.server.catalog_status',
                       return_value=status):
                cleared = Workspace.applied_acquisition_sources(workspace)
            self.assertEqual({}, cleared['blueprints'])


if __name__ == '__main__':
    unittest.main()
