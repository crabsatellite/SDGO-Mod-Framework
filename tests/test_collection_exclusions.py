"""Collection exclusion list, catalog explanation refresh and their Manager entry points."""
import configparser
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch

from manager import storage
from manager.providers import attach
from manager.workspace import Workspace
from owner.collection_capsule import exclusions, hints
from owner.collection_capsule.acquisition import FILE, encode
from owner.collection_capsule.pool import build_pool
from owner.collection_capsule.sql import module_body

ROOT = Path(__file__).resolve().parents[1]
RULES = {'price_gp': 100000, 'limit_period': 'week', 'draw_limit': 15, 'utc_offset_minutes': 480}


def unit(uid, target=0, ready=1, rank=1):
    return dict(UnitNumber=uid, UnitNumber_Transform=target, SallyReady=ready, UnitGradeNumber=rank,
                UnitGradeNumber2=2, UnitGraph=1000000 + uid, UnitModelName=f'Fixture {uid}')


def registry(client, roots):
    """A minimal installed shared capsule registry carrying a catalog explanation."""
    root = Path(client) / hints.REGISTRY
    root.mkdir(parents=True)
    raw, metadata = encode(roots, [], RULES)
    (root / FILE).write_bytes(raw)
    (root / 'data').mkdir()
    (root / 'data/0099.gdt').write_bytes(b'table')
    digest = hashlib.sha256(raw).hexdigest()
    (root / 'runtime.ini').write_text(
        '[runtime]\nschema=1\noverlay_zdx_sha256=' + 'a' * 64 + '\n\n[gdt]\n0099=data\\0099.gdt\n'
        '0099_sha256=' + hashlib.sha256(b'table').hexdigest() + '\n\n[acquisition]\nfile=' + FILE +
        '\nsha256=' + digest + '\n\n[counts]\nfixed=1\n', encoding='ascii')
    (root / 'manifest.json').write_text(json.dumps({'schema_version': 1, 'collection_acquisition': metadata,
                                                    'files': {FILE: digest, 'data/0099.gdt': 'x'}}), encoding='utf-8')
    return root


class ExclusionListTests(unittest.TestCase):
    def test_entries_are_normalized_sorted_and_validated(self):
        self.assertEqual(exclusions.normalize([26187, {'unit_id': 15171, 'note': ' 测试机 '}, 26187]),
                         [{'unit_id': 15171, 'note': '测试机'}, {'unit_id': 26187, 'note': ''}])
        for bad in ({'unit_id': 1}, [0], [-5], ['26187'], [{'unit_id': 1, 'note': 'a\nb'}],
                    [{'unit_id': 1, 'extra': 2}], [{'unit_id': 1, 'note': 'x' * 101}],
                    [{'unit_id': 1, 'note': 'a'}, {'unit_id': 1, 'note': 'b'}], list(range(1, 5002))):
            with self.assertRaises(ValueError, msg=repr(bad)[:60]):
                exclusions.normalize(bad)

    def test_offline_snapshot_removes_every_base_whose_chain_lists_a_form(self):
        rows = [unit(100, 101), unit(101, 0, ready=0), unit(102), unit(103, 104), unit(104)]
        pool = build_pool(rows)
        bases = {u['unit_id'] for u in pool['units']}
        self.assertEqual(bases, {100, 102, 103})
        self.assertEqual(exclusions.excluded_roots(pool, {101}), {100})      # a transformed form
        self.assertEqual(exclusions.excluded_roots(pool, {104}), {103})      # a reachable second form
        self.assertEqual(exclusions.excluded_roots(pool, {102, 999}), {102})  # unknown IDs are inert

    def test_draw_and_pool_view_share_one_pool_fragment(self):
        fragment = (ROOT / 'owner/collection_capsule/sql/pool.sql').read_text(encoding='utf-8').strip('\n')
        for name in ('draw.sql', 'eligible_roots.sql'):
            body = module_body(name)
            self.assertIn(fragment, body)
            self.assertNotIn('SDGO_COLLECTION_POOL', body)
        draw = module_body('draw.sql')
        # Account choices, the catalog-complete phase and delivery checks all use the excluded pool.
        tail = draw[draw.index('IF @poolError=1 RETURN 3008;'):]
        self.assertNotIn('@bases', tail)
        self.assertEqual(tail.count('FROM @eligible u'), 2)


class CatalogExplanationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.client = Path(self.temp.name) / 'client'

    def tearDown(self):
        self.temp.cleanup()

    def test_refresh_rewrites_list_declaration_and_manifest_only(self):
        root = registry(self.client, [100, 102])
        table = (root / 'data/0099.gdt').read_bytes()
        result = hints.sync(self.client, [100, 103, 110], RULES)
        self.assertEqual((result['status'], result['added'], result['removed']), ('updated', [103, 110], [102]))
        raw = (root / FILE).read_bytes()
        self.assertEqual(hints.decode(raw), [100, 103, 110])
        ini = configparser.ConfigParser(interpolation=None)
        ini.read(root / 'runtime.ini', encoding='utf-8')
        self.assertEqual(ini['acquisition']['sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(ini['runtime']['overlay_zdx_sha256'], 'a' * 64)
        manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['files'][FILE], hashlib.sha256(raw).hexdigest())
        self.assertEqual(manifest['files']['data/0099.gdt'], 'x')
        self.assertEqual(manifest['collection_acquisition']['unit_ids'], [100, 103, 110])
        self.assertEqual((root / 'data/0099.gdt').read_bytes(), table)
        self.assertFalse(list(root.glob('*.sdgo-pending')))
        self.assertEqual(hints.sync(self.client, [110, 103, 100], RULES)['status'], 'current')

    def test_running_client_keeps_its_files_and_reports_the_pending_change(self):
        root = registry(self.client, [100])
        before = {p.name: p.read_bytes() for p in root.iterdir() if p.is_file()}
        def running(_):
            raise RuntimeError('GOnline.exe is running')
        result = hints.sync(self.client, [100, 102], RULES, client_running=running)
        self.assertEqual((result['status'], result['added']), ('client_running', [102]))
        self.assertEqual({p.name: p.read_bytes() for p in root.iterdir() if p.is_file()}, before)

    def test_interrupted_refresh_is_repaired_from_the_same_pool(self):
        root = registry(self.client, [100])
        raw, _ = encode([100, 102], [], RULES)
        (root / FILE).write_bytes(raw)  # list written, declaration not yet
        self.assertFalse(hints.installed(self.client)['consistent'])
        self.assertEqual(hints.sync(self.client, [100, 102], RULES)['status'], 'updated')
        self.assertTrue(hints.installed(self.client)['consistent'])

    def test_clients_without_the_collection_registry_are_left_alone(self):
        self.assertEqual(hints.sync(self.client, [100], RULES)['status'], 'not_installed')
        self.assertFalse(self.client.exists())
        with self.assertRaises(ValueError):
            hints.sync(self.client, [], RULES)

    def test_live_rules_come_from_the_installed_machine(self):
        db = Mock()
        db.cursor.return_value.execute.return_value.fetchone.return_value = (123456, 'day ', 3, 480)
        self.assertEqual(hints.live_rules(db), {'price_gp': 123456, 'limit_period': 'day',
                                                'draw_limit': 3, 'utc_offset_minutes': 480})


class ManagerEntryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.workspace = Workspace(self.root / 'workspace', ROOT / 'mods')
        attach(self.workspace)
        self.provider = self.workspace.providers['collection_capsule']
        self.provider.configure({'client': str(self.root / 'client'),
                                 'server_ini': str(self.root / 'world/WorldServer.ini')})

    def tearDown(self):
        self.temp.cleanup()

    def post(self, path, **values):
        return self.provider.request('POST', path, {'revision': self.provider.revision(), **values}, {})

    def test_saving_exclusions_writes_the_selected_world_then_refreshes_the_client(self):
        database = Mock()
        written = {'entries': [], 'excluded_roots': [], 'eligible': 3, 'bases': 4, 'pool_error': False,
                   'undeliverable': [], 'eligible_roots': [100, 102, 103], 'changed': True, 'applied': True,
                   'world_restart_required': False}
        with patch('owner.collection_capsule.deployment.connection', return_value=database), \
             patch('owner.collection_capsule.exclusions.replace', return_value=dict(written)) as replace, \
             patch.object(self.provider, 'sync_hints', return_value={'status': 'updated'}) as refresh:
            result = self.post('/api/exclusions', entries=[{'unit_id': 26187, 'note': '已下架'}])
        replace.assert_called_once_with(database, [{'unit_id': 26187, 'note': '已下架'}], dry_run=False)
        refresh.assert_called_once_with()
        self.assertEqual(result['hints'], {'status': 'updated'})
        self.assertNotIn('eligible_roots', result)
        database.close.assert_called_once()

    def test_preview_rolls_back_and_does_not_touch_the_client(self):
        with patch('owner.collection_capsule.deployment.connection', return_value=Mock()), \
             patch('owner.collection_capsule.exclusions.replace', return_value={'eligible_roots': []}) as replace, \
             patch.object(self.provider, 'sync_hints') as refresh:
            self.post('/api/exclusions', entries=[1], dry_run=True)
        self.assertTrue(replace.call_args.kwargs['dry_run'])
        refresh.assert_not_called()

    def test_stale_page_cannot_save_exclusions(self):
        with self.assertRaises(storage.Conflict):
            self.provider.request('POST', '/api/exclusions', {'revision': 'stale', 'entries': []}, {})

    def test_capsule_compile_refreshes_the_explanation_before_its_fingerprint(self):
        import threading
        from manager.engines.capsules import capsule_ui
        order = []
        store = capsule_ui.CatalogStore.__new__(capsule_ui.CatalogStore)
        store.lock = threading.RLock()
        store.client_path = self.root / 'client'
        store.lookup_path = self.root / 'lookup.json'
        store.bundle_path = self.root / 'bundle'
        store.read = lambda: ({}, None)
        store._expect = lambda catalog, expected: 'f' * 64
        store.before_compile = lambda client: order.append(('hints', client)) or {'status': 'updated'}

        class Stop(Exception):
            pass

        def fingerprint(client):
            order.append(('fingerprint', client))
            raise Stop
        with patch.object(capsule_ui.CatalogStore, 'can_compile', True, create=True),              patch('manager.engines.capsules.capsule_client_compile.extension_fingerprint', fingerprint):
            with self.assertRaises(Stop):
                store.compile('f' * 64)
        self.assertEqual(order, [('hints', store.client_path), ('fingerprint', store.client_path)])
        store.before_compile = Mock(side_effect=ValueError('World offline'))
        order.clear()
        with patch.object(capsule_ui.CatalogStore, 'can_compile', True, create=True),              patch('manager.engines.capsules.capsule_client_compile.extension_fingerprint', fingerprint):
            with self.assertRaises(Stop):  # a failed refresh never blocks the ordinary compile
                store.compile('f' * 64)
        self.assertEqual(order, [('fingerprint', store.client_path)])

    def test_capsules_provider_wires_the_refresh_to_the_collection_world(self):
        capsules = self.workspace.providers['capsules']
        (self.root / 'catalog.json').write_text('{}', encoding='utf-8')
        capsules.configure({'catalog': str(self.root / 'catalog.json')})
        with patch('manager.engines.capsules.capsule_ui.CatalogStore') as store_type,              patch('manager.engines.capsules.capsule_ui.CapsuleApplication'):
            capsules.get_engine()
        hook = store_type.call_args.kwargs['before_compile']
        with patch.object(self.provider, 'sync_hints', return_value={'status': 'updated'}) as refresh:
            self.assertEqual(hook('target-client'), {'status': 'updated'})
        refresh.assert_called_once_with('target-client')

    def test_applied_unit_imports_refresh_the_explanation(self):
        units = self.workspace.providers['units']
        with patch.object(self.provider, 'sync_hints', return_value={'status': 'current'}) as refresh:
            self.assertEqual(units._sync_collection_hints(), {'status': 'current'})
        refresh.assert_called_once_with()
        with patch.object(self.provider, 'sync_hints', side_effect=ValueError('请先更新')):
            self.assertEqual(units._sync_collection_hints()['status'], 'failed')

if __name__ == '__main__':
    unittest.main()
