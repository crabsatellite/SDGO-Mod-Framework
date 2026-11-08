"""Batch roster registration for the capsule, activity and shop editors.

The publication, archive and database are fixtures: the SQL layer is the repository tests'
in-memory fake and every file lives in a temporary folder.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import io
from pathlib import Path
import sys
import threading
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image

from manager import storage
from manager.units import Units, store_activity_poster, _put_poster
from manager.unit_activity import resolve_family
from owner.native_archive import build_archive_file
from owner.units import activity_poster, batch_registry, batch_server
from owner.units.assets import sprite_binary
from tests.test_unit_batch_server import REVISION, ServerGroup, ordered_families, write_publication


def png(size=(64, 48), colour=(200, 40, 40, 255)):
    stream = io.BytesIO()
    Image.new('RGBA', size, colour).save(stream, 'PNG')
    return stream.getvalue()


def add_package(folder: Path, families) -> None:
    """registrations.json plus a one-sprite-per-family overlay archive, as the batch publisher writes them."""
    package = folder / batch_registry.PACKAGE
    package.mkdir(parents=True, exist_ok=True)
    resources, items = {}, []
    for value in families:
        root = value['root_unit_id']
        texture, sprite = f'txrs/fixture{root}.txr', f'sprs/fixture{root}d.spr'
        resources[texture] = png()
        resources[sprite] = sprite_binary(texture.replace('/', '\\'), 64, 48)
        items.append({'key': f'unit:{root}', 'provider': 'unit_catalog', 'kind': 'unit', 'unit_id': root,
                      'category_id': value['item']['CatalogueID'], 'family_key': value['family_key'],
                      'display_name': f'批量机体{root}', 'forms': [root], 'shop_sprite': sprite,
                      'secret_store_sprite': sprite, 'capsule_sprite': sprite, 'unit_primary_sprite': sprite,
                      'custom_candidate_sprite': sprite})
    items.append({'key': 'unit:25061', 'family_key': 'unit-import.carried', 'unit_id': 25061})  # carried, not owned
    index, _ = build_archive_file(resources, lambda name: True, package / 'resources.zpk')
    (package / 'resources.zdx').write_bytes(index)
    (package / 'resource-overlay.ini').write_text('[runtime]\nschema=1\nindex=resources.zdx\narchive=resources.zpk\n', 'utf-8')
    (folder / 'registrations.json').write_bytes(storage.json_bytes({'schema_version': 1, 'items': items}))


class RegistryTests(ServerGroup):
    def setUp(self):
        super().setUp()
        self.families = ordered_families()
        self.folder = write_publication(self.root, self.families)
        add_package(self.folder, self.families)
        self.workspace = self.root / 'units-workspace'

    def apply(self):
        with patch.object(batch_server, 'running_services', return_value=[]):
            batch_server.apply(self.folder, self.ini)

    def test_unapplied_publication_is_refused(self):
        with self.assertRaisesRegex(ValueError, '尚未完整应用'):
            batch_registry.register(self.workspace, self.folder, self.ini)
        self.assertEqual(batch_registry.load(self.workspace), [])

    def test_applied_publication_registers_and_offers_rows(self):
        self.apply()
        entry = batch_registry.register(self.workspace, self.folder, self.ini)
        self.assertEqual((entry['families'], entry['revision']), (3, REVISION))
        rows = batch_registry.rows(self.workspace, self.ini)
        self.assertEqual(sorted(row['unit_id'] for row in rows), [25070, 25080, 25090])
        row = next(row for row in rows if row['unit_id'] == 25080)
        self.assertEqual((row['rank'], row['subgrade'], row['category_id'], row['revision']), (4, 4, 1119002, REVISION))
        self.assertTrue(row['server_applied'])
        self.assertEqual(row['activation']['source'], 'batch')
        with Image.open(io.BytesIO(batch_registry.sprite_png(row))) as image:
            self.assertEqual(image.size, (64, 48))

    def test_baseline_identical_sprite_is_read_from_the_client_archive(self):
        # The publisher does not ship sprites that are byte-identical to the baseline client; the game reads them
        # from its own archive, and the Manager must render them from there too.
        self.apply()
        batch_registry.register(self.workspace, self.folder, self.ini)
        row = next(row for row in batch_registry.rows(self.workspace, self.ini) if row['unit_id'] == 25080)
        row = dict(row, capsule_sprite='sprs/baseline.spr')
        baseline_files = {'sprs/baseline.spr': sprite_binary('txrs\\baseline.txr', 40, 30),
                          'txrs/baseline.txr': png((40, 30))}

        class FakeBaseline:
            def __init__(self, root):
                self.root, self._loose_roots = root, [root]

            def exists(self, path):
                return path in baseline_files

            def read(self, path):
                return baseline_files[path]

        with self.assertRaisesRegex(ValueError, 'missing'):
            batch_registry.sprite_png(row)
        with patch('owner.units.assets.Baseline', FakeBaseline):
            with Image.open(io.BytesIO(batch_registry.sprite_png(row, baseline_client=self.root))) as image:
                self.assertEqual(image.size, (40, 30))
            row = dict(row, capsule_sprite='sprs/nowhere.spr')
            with self.assertRaisesRegex(ValueError, 'missing'):
                batch_registry.sprite_png(row, baseline_client=self.root)

    def test_reregistering_replaces_the_owner_entry(self):
        self.apply()
        batch_registry.register(self.workspace, self.folder, self.ini)
        batch_registry.register(self.workspace, self.folder, self.ini)
        self.assertEqual(len(batch_registry.load(self.workspace)), 1)
        self.assertTrue(batch_registry.unregister(self.workspace, 'fixture', batch_registry.load(self.workspace)[0]['server_target']))
        self.assertEqual(batch_registry.rows(self.workspace, self.ini), [])

    def test_changed_publication_is_not_offered_silently(self):
        self.apply()
        batch_registry.register(self.workspace, self.folder, self.ini)
        plan = storage.read_json(self.folder / 'server-plan.json')
        plan['revision'] = 'd' * 64
        (self.folder / 'server-plan.json').write_bytes(storage.json_bytes(plan))
        with self.assertRaisesRegex(ValueError, '已改变'):
            batch_registry.rows(self.workspace, self.ini)

    def test_other_database_sees_no_rows(self):
        self.apply()
        batch_registry.register(self.workspace, self.folder, self.ini)
        other = self.root / 'Other/WorldServer.ini'
        other.parent.mkdir(parents=True)
        other.write_text('[DB Game]\nDBServerName=10.0.0.9,7\nDBName=other\nID=x\nPWD=x\n')
        self.assertEqual(batch_registry.rows(self.workspace, other), [])

    def test_manager_rows_include_batch_families_without_shadowing_projects(self):
        self.apply()
        batch_registry.register(self.workspace, self.folder, self.ini)
        workspace = types.SimpleNamespace(root=self.root / 'manager',
                                          settings={'modules': {'units': {'workspace': str(self.workspace), 'server_ini': str(self.ini)}}})
        units = Units(workspace)
        engine = types.SimpleNamespace(lock=threading.RLock(), read=lambda: {'drafts': {}})
        units.engine = engine
        rows = units.for_server(self.ini)
        self.assertEqual(sorted(row['unit_id'] for row in rows), [25070, 25080, 25090])
        self.assertTrue(all(row['shop_image_url'].startswith('/api/providers/units/api/units/batch/image?') for row in rows))
        image = units.request('GET', rows[0]['shop_image_url'].replace('/api/providers/units', ''), None, {})
        self.assertEqual(image.mime, 'image/png')
        self.assertTrue(image.data.startswith(bytes([0x89]) + b"PNG"))
        family = resolve_family(units, 'fixture.batch.b', 25080, self.ini)
        self.assertEqual((family['category_id'], family['rank'], family['revision']), (1119002, 4, REVISION))
        with self.assertRaisesRegex(ValueError, '尚未登记'):
            resolve_family(units, 'fixture.batch.b', 25090, self.ini)


class PosterTests(unittest.TestCase):
    def test_placeholder_is_native_size(self):
        with Image.open(io.BytesIO(activity_poster.placeholder(png((120, 120)), '测试机体'))) as image:
            self.assertEqual(image.size, activity_poster.SIZE)

    def test_uploaded_poster_replaces_the_asset_and_repoints_users(self):
        key = 'fixture.batch.b:activity_poster'
        old = {'key': key, 'project_key': 'fixture.batch.b', 'project_revision': REVISION, 'name': 'B', 'role': 'activity_poster',
               'unit_id': 25080, 'asset_sha256': 'e' * 64, 'source_asset_sha256': 'e' * 64, 'original_sha256': 'e' * 64,
               'png_base64': '', 'metadata': {}, 'placeholder': True}
        catalog = {'project_assets': [old],
                   'activity_drafts': [{'id': 1201, 'project_key': 'fixture.batch.b', 'unit_id': 25080, 'artwork_key': key,
                                        'artwork_sha256': 'e' * 64, 'name': 'B活动扭蛋机'}],
                   'activity_machines': [{'id': 1201, 'project_key': 'fixture.batch.b', 'unit_id': 25080, 'artwork_key': key,
                                          'artwork_sha256': 'e' * 64, 'name': 'B', 'revision': 'f' * 64}]}
        saved = {}

        class Store:
            lock = threading.RLock()

            def read(self):
                return copy.deepcopy(catalog), b'raw'

            def _expect(self, value, revision):
                if revision != 'rev':
                    raise ValueError('stale')

            def _save(self, value, raw):
                saved['catalog'] = value

        provider = types.SimpleNamespace(get_engine=lambda: types.SimpleNamespace(store=Store()))
        poster = png((722, 644), (10, 120, 220, 255))
        with patch('manager.engines.capsules.capsule_catalog.validate'):
            store_activity_poster(provider, {'expected_revision': 'rev', 'id': 1201, 'filename': 'poster.png',
                                             'data': base64.b64encode(poster).decode('ascii')})
        result = saved['catalog']
        asset = result['project_assets'][0]
        self.assertNotIn('placeholder', asset)
        self.assertEqual(asset['original_sha256'], hashlib.sha256(poster).hexdigest())
        with Image.open(io.BytesIO(base64.b64decode(asset['png_base64']))) as image:
            self.assertEqual(image.size, (361, 322))
        self.assertEqual(result['activity_drafts'][0]['artwork_sha256'], asset['asset_sha256'])
        self.assertEqual(result['activity_machines'][0]['artwork_sha256'], asset['asset_sha256'])
        self.assertNotEqual(result['activity_machines'][0]['revision'], 'f' * 64)

    def test_poster_requests_with_extra_fields_are_refused(self):
        provider = types.SimpleNamespace(get_engine=lambda: None)
        with self.assertRaisesRegex(ValueError, '无效'):
            store_activity_poster(provider, {'expected_revision': 'rev', 'id': 1, 'filename': 'a.png', 'data': '', 'pool': []})


if __name__ == '__main__':
    unittest.main()
