import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, MagicMock

from manager.engines.capsules import capsule_presets as presets
from manager.engines.capsules.capsule_catalog import digest, read_json, write_json
from manager.engines.capsules.capsule_ui import CatalogStore, CapsuleApplication, ConflictError


def native_catalog():
    """Target metadata fixture without any embedded native archives or images."""
    data = presets.template()
    units = [{**u, 'name': str(u['id']), 'eligible': True,
              'rank': {'C': 1, 'B': 2, 'A': 3}[u['grade'][0]],
              'subgrade': 4 if u['grade'].endswith('R') else 2,
              'transform_to': 0, 'capsule_sprites': []} for u in data['units']]
    by_unit = {u['id']: u for u in units}
    by_machine = {m['template_id']: m for m in data['fixed_machines']}
    fixed = []
    for number in range(1, 79):
        model = by_machine.get(number * 100 + 1, data['fixed_machines'][0])
        fixed.append({'id': number * 100 + 1, 'name': str(number), 'enabled': True,
                      'owner': 'native', 'currency': 'GP', 'draw_policy': 'native_sequence',
                      'price': model['price'], 'slots': [
                          {**s, 'sprites': []} for s in model['slots']]})
    return {'schema_version': 1, 'source_identity': {'target': 'fixture'},
            'units': units, 'fixed_machines': fixed, 'custom_machines': []}


class DefaultPresetTests(unittest.TestCase):
    def test_default_35_and_prices_are_shipped_without_artwork_or_custom_content(self):
        data = presets.template()
        self.assertEqual([m['id'] for m in data['fixed_machines']], list(range(7901, 11302, 100)))
        self.assertEqual(len(data['fixed_machines']), 35)
        self.assertEqual(sum(m['price'] == 1200 for m in data['fixed_machines']), 8)
        self.assertEqual(sum(m['price'] == 1600 for m in data['fixed_machines']), 18)
        self.assertEqual(sum(m['price'] == 7500 for m in data['fixed_machines']), 2)
        self.assertEqual(sum(m['price'] == 14800 for m in data['fixed_machines']), 5)
        self.assertNotIn('custom_machines', data)
        self.assertNotIn('activity_machines', data)
        self.assertNotIn('source_identity', data)
        self.assertNotIn('png_base64', presets.RESOURCE.read_text(encoding='utf-8'))

    def test_initial_install_has_113_fixed_and_does_not_modify_native_pools(self):
        original = native_catalog()
        result, receipt = presets.apply(original)
        self.assertEqual(receipt['status'], 'initialized')
        self.assertEqual(len(result['fixed_machines']), 113)
        self.assertEqual(result['fixed_machines'][:78], original['fixed_machines'])
        self.assertEqual(result['custom_machines'], original['custom_machines'])
        self.assertEqual(result['units'], original['units'])
        self.assertEqual(len(original['fixed_machines']), 78)
        for installed, source in zip(result['fixed_machines'][78:], presets.template()['fixed_machines']):
            self.assertEqual(installed['price'], source['price'])
            self.assertEqual([(s['unit_id'], s['weight'], s['native_draw_group']) for s in installed['slots']],
                             [(s['unit_id'], s['weight'], s['native_draw_group']) for s in source['slots']])

    def test_upgrades_preserve_edits_retirement_and_other_owner_content(self):
        catalog, _ = presets.apply(native_catalog())
        catalog['fixed_machines'][78]['price'] = 3210
        catalog['fixed_machines'][79].update(enabled=False, retired=True)
        catalog['fixed_machines'].append({**copy.deepcopy(catalog['fixed_machines'][80]),
                                         'id': 20001, 'owner': 'mod', 'name': 'authored'})
        again, _ = presets.apply(catalog)
        self.assertEqual(again, catalog)
        catalog.pop('default_fixed_preset')
        again, _ = presets.apply(catalog)
        self.assertEqual(again, catalog, 'older owner configurations must also survive')

    def test_incompatible_unit_fails_before_mutation(self):
        catalog = native_catalog()
        catalog['units'][0]['eligible'] = False
        before = copy.deepcopy(catalog)
        with self.assertRaisesRegex(ValueError, '机体'):
            presets.apply(catalog)
        self.assertEqual(catalog, before)

    def test_restore_rejects_an_unrelated_machine_occupying_a_default_id(self):
        catalog = native_catalog()
        catalog['fixed_machines'].append({**copy.deepcopy(catalog['fixed_machines'][0]),
                                         'id': 7901, 'owner': 'mod'})
        with self.assertRaisesRegex(ValueError, '占用'):
            presets.apply(catalog, restore=True)

    def test_explicit_restore_can_reenable_a_retired_default(self):
        catalog, _ = presets.apply(native_catalog())
        catalog['fixed_machines'][78].update(enabled=False, retired=True)
        result, _ = presets.apply(catalog, restore=True)
        self.assertTrue(result['fixed_machines'][78]['enabled'])
        self.assertFalse(result['fixed_machines'][78].get('retired', False))

    def test_store_seeds_once_and_restore_uses_revision_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'catalog.json'
            write_json(path, native_catalog())
            store = CatalogStore(path)
            seeded = store.read()[0]
            self.assertEqual(len(seeded['fixed_machines']), 113)
            app = CapsuleApplication(store)
            self.assertEqual(app.bootstrap()['default_preset']['total_fixed_machines'], 113)
            edited = copy.deepcopy(seeded)
            edited['fixed_machines'][78]['price'] = 1
            write_json(path, edited)
            reopened = CatalogStore(path)
            self.assertEqual(read_json(path), edited)
            with self.assertRaises(ConflictError):
                reopened.restore_defaults(digest(seeded))
            restored = reopened.restore_defaults(digest(edited))
            self.assertEqual(restored['default_preset']['status'], 'restored')
            self.assertEqual(read_json(path)['fixed_machines'][78]['price'], 7500)

    def test_provider_first_target_configuration_applies_both_sides(self):
        from manager.workspace import Workspace
        from manager.providers import Capsules
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workspace = Workspace(root/'workspace', Path(__file__).resolve().parents[1]/'mods')
            provider = Capsules(workspace)
            engine = MagicMock()
            engine.store.read.return_value = (native_catalog(), b'fixture')
            engine.store.apply.return_value = {'applied': True, 'server_applied': True}
            with patch('manager.engines.capsules.capsule_setup.prepare', return_value={
                    'catalog': str(root/'catalog.json'), 'install_client': str(root/'client')} ) as setup, \
                    patch.object(provider, 'get_engine', return_value=engine):
                result = provider.configure({'client': str(root/'client'), 'server_ini': str(root/'server.ini')})
                setup.assert_called_once_with(workspace.root, str(root/'client'), str(root/'server.ini'))
                engine.store.apply.assert_called_once_with(digest(native_catalog()))
                self.assertTrue(result['installation']['server_applied'])
                provider.configure({'catalog': str(root/'authored.json')})
                self.assertEqual(setup.call_count, 1, 'existing owner configuration is not reinstalled')
            workspace.close()

    def test_fresh_setup_refuses_existing_server_config_before_export(self):
        from manager.engines.capsules.capsule_setup import prepare
        connection = MagicMock()
        connection.cursor.return_value.execute.return_value.fetchval.return_value = 1
        with tempfile.TemporaryDirectory() as temporary, \
                patch('manager.engines.capsules.capsule_deployment._assert_client_stopped'), \
                patch('owner.collection_capsule.deployment.connection', return_value=connection), \
                patch('tools.export_capsule_snapshot.export_snapshot') as exporter:
            with self.assertRaisesRegex(ValueError, '已有扭蛋配置'):
                prepare(temporary, Path(temporary)/'client', Path(temporary)/'server.ini')
            exporter.assert_not_called()
            connection.close.assert_called_once()

    def test_native_ini_allows_identical_case_variants_but_not_conflicting_credentials(self):
        from manager.engines.capsules.capsule_deployment import _read_server_settings
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'server.ini'
            text = ('[Other]\nID=ignored\nid=also ignored\n[DB Game]\n'
                    'DBServerName=127.0.0.1\nDBName=Fixture\nID=test\nid=test\nPWD=fixture\n')
            path.write_text(text, encoding='utf-16')
            self.assertEqual(_read_server_settings(path)['ID'], 'test')
            path.write_text(text.replace('id=test', 'id=conflict'), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'conflicting'):
                _read_server_settings(path)


if __name__ == '__main__':
    unittest.main()
