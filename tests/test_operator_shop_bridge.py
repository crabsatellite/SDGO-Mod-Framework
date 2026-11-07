"""Offline coverage for operator registrations entering the existing shop draft API."""
import copy
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from manager.shop import Shop


def operator_catalog(*, enabled=True, delivery='product', managed=True, storefront=None):
    operator = {
        'id': 60047,
        'category_id': 1219047,
        'template_id': 60003,
        'name': '补给副官',
        'managed': managed,
        'enabled': enabled,
        'delivery': delivery,
        'avatar': {'kind': 'native', 'operator_id': 60003},
        'drops': [],
    }
    if storefront is not None:
        operator['storefront'] = storefront
    return {
        'schema_version': 1,
        'native_operator_ids': [60003],
        'client_index_sha256': 'fixture',
        'uploaded_assets': {},
        'native_server_data': {},
        'items': [{'id': 1219047, 'name': 'operator', 'ref_index': 60047, 'time_limit': 0}],
        'operators': [operator],
    }


class OperatorEngine:
    def __init__(self, catalog):
        self.catalog = catalog

    def read(self):
        return copy.deepcopy(self.catalog)


class NativeIniTargetTests(unittest.TestCase):
    def test_duplicate_native_credential_case_does_not_break_target_comparison(self):
        from owner.units.repository import database_target_key
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);a=root/'a.ini';b=root/'b.ini'
            a.write_text('[DB Game]\nDBServerName=localhost\nDBName=fixture\nid=old\npwd=old\nID=current\nPWD=current\n',encoding='utf-8')
            b.write_text('[DB Game]\nDBServerName=LOCALHOST\nDBName=FIXTURE\nID=current\nPWD=current\n',encoding='utf-8')
            original=a.read_bytes()
            self.assertEqual(database_target_key(a),database_target_key(b))
            self.assertEqual(a.read_bytes(),original)


class FakeBridge:
    def __init__(self, *, ref_index=60047, fail_actions=()):
        self.revision = 0
        self.actions = []
        self.fail_actions = set(fail_actions)
        self.state = {
            'catalog': {'schema_version': 1, 'catalog_id': 'host-shop', 'products': []},
            'items': [{'Id': 1219047, 'Name': 'native operator', 'RefIndex': ref_index,
                       'TimeLimit': 0, 'Kind': 12}],
            'products': [],
            'demo': True,
        }

    def _result(self):
        value = copy.deepcopy(self.state)
        value['revision'] = 'r' + str(self.revision)
        value['products'] = [
            {'product': copy.deepcopy(product), 'status': '未应用'}
            for product in value['catalog']['products']
        ]
        return value

    def call(self, request):
        action = request['action']
        self.actions.append((action, copy.deepcopy(request.get('body', {}))))
        if action == 'read':
            return self._result()
        if request.get('revision') != 'r' + str(self.revision):
            raise ValueError('stale shop revision')
        if action in self.fail_actions:
            raise ValueError('forced ' + action + ' failure')
        body = request['body']
        if action == 'create':
            product = {
                'key': 'p-' + str(len(self.state['catalog']['products']) + 1),
                'product_id': 1019001 + len(self.state['catalog']['products']),
                'item_id': '190001',
                'template_id': 1010001,
                'category_id': body['category_id'],
                'name': body['name'],
                'label': '数量 1',
                'price': 500,
                'currency': 'GB',
                'validity': 1,
                'enabled': True,
                'retired': False,
            }
            self.state['catalog']['products'].append(product)
        elif action == 'update':
            product = next(p for p in self.state['catalog']['products'] if p['key'] == body['key'])
            old_validity = product['validity']
            product.update(body['values'])
            if product['validity'] != old_validity:
                product['label'] = '数量 ' + str(product['validity'])
        elif action == 'delete':
            product = next(p for p in self.state['catalog']['products'] if p['key'] == body['key'])
            product.update(retired=True, enabled=False)
        elif action == 'apply':
            pass
        else:
            raise AssertionError('unexpected action ' + action)
        self.revision += 1
        return self._result()


class Services:
    def __init__(self, bridge):
        self.bridge = bridge

    def resource(self, identity, factory):
        assert identity == 'shop.bridge'
        return self.bridge


class Workspace:
    mode = 'standard'

    def __init__(self, catalog, bridge):
        self.settings = {'modules': {'shop': {'workspace': 'fixture', 'demo': '1'}}}
        self.services = Services(bridge)
        operator = SimpleNamespace(engine=OperatorEngine(catalog), config={})
        self.providers = {'operators': operator}


class Units:
    def __init__(self, registrations):
        self.registrations = registrations

    def for_server(self, _):
        return copy.deepcopy(self.registrations)


class OperatorShopBridgeTests(unittest.TestCase):
    def provider(self, catalog=None, bridge=None):
        bridge = bridge or FakeBridge()
        return Shop(Workspace(catalog or operator_catalog(), bridge)), bridge

    def test_registration_is_previewed_and_create_becomes_unlisted_draft(self):
        provider, bridge = self.provider()
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        registration = state['operator_imports']['1219047']
        self.assertEqual((registration['operator_id'], registration['category_id']), (60047, 1219047))
        self.assertEqual(state['items'][0]['Name'], '补给副官')
        self.assertFalse(registration['draft_defaults']['enabled'])
        state = provider.request('POST', '/api/shop/create',
                                 {'category_id': 1219047, 'name': 'ignored'},
                                 {'X-Shop-Revision': state['revision']})
        product = state['catalog']['products'][0]
        self.assertEqual((product['name'], product['price'], product['currency'], product['validity']),
                         ('补给副官', 500, 'GB', 1))
        self.assertFalse(product['enabled'])
        self.assertEqual([action for action, _ in bridge.actions], ['read', 'read', 'create', 'update'])
        self.assertNotIn('apply', [action for action, _ in bridge.actions])

    def test_storefront_prefill_is_applied_but_cannot_publish(self):
        catalog = operator_catalog(storefront={
            'name': '精选副官',
            'label': '永久副官',
            'price': 888,
            'currency': 'MB',
            'validity': 3,
        })
        provider, _ = self.provider(catalog)
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        state = provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                                 {'X-Shop-Revision': state['revision']})
        product = state['catalog']['products'][0]
        self.assertEqual(
            {key: product[key] for key in ('name', 'label', 'price', 'currency', 'validity', 'enabled')},
            {'name': '精选副官', 'label': '永久副官', 'price': 888,
             'currency': 'MB', 'validity': 3, 'enabled': False},
        )
        self.assertEqual(
            [action for action, _ in provider.workspace.services.bridge.actions],
            ['read', 'read', 'create', 'update', 'update'],
        )

    def test_only_managed_enabled_product_registrations_are_visible(self):
        for changes in (
                {'managed': False},
                {'enabled': False},
                {'delivery': 'all_players'}):
            provider, _ = self.provider(operator_catalog(**changes))
            state = provider.request('GET', '/api/shop/catalog', {}, {})
            self.assertEqual(state['operator_imports'], {})
            self.assertEqual(state['items'][0]['Name'], 'native operator')

    def test_identity_and_existing_product_conflicts_fail_before_create(self):
        provider, bridge = self.provider(bridge=FakeBridge(ref_index=60048))
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        self.assertIn('原生副官编号冲突', state['operator_imports']['1219047']['draft_error'])
        with self.assertRaisesRegex(ValueError, '原生副官编号冲突'):
            provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                             {'X-Shop-Revision': state['revision']})
        self.assertEqual([action for action, _ in bridge.actions], ['read', 'read'])

        provider, bridge = self.provider()
        bridge.state['catalog']['products'].append({
            'key': 'existing', 'category_id': 1219047, 'retired': False,
        })
        with self.assertRaisesRegex(ValueError, '已经存在'):
            provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                             {'X-Shop-Revision': 'r0'})
        self.assertEqual([action for action, _ in bridge.actions], ['read'])

        provider, bridge = self.provider()
        with self.assertRaisesRegex(ValueError, '必须使用整数编号'):
            provider.request('POST', '/api/shop/create', {'category_id': '1219047'},
                             {'X-Shop-Revision': 'r0'})
        self.assertEqual(bridge.actions, [])

    def test_missing_native_category_is_previewed_but_cannot_create(self):
        provider, bridge = self.provider()
        bridge.state['items'] = []
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        registration = state['operator_imports']['1219047']
        self.assertFalse(registration['available'])
        self.assertIn('尚未应用', registration['draft_error'])
        with self.assertRaisesRegex(ValueError, '尚未应用'):
            provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                             {'X-Shop-Revision': state['revision']})
        self.assertEqual(bridge.state['catalog']['products'], [])
        self.assertEqual([action for action, _ in bridge.actions], ['read', 'read'])

    def test_invalid_or_free_storefront_defaults_are_rejected(self):
        for storefront in (
                {'price': 0},
                {'currency': 'GP'},
                {'enabled': True},
                {'name': 'English only'}):
            provider, bridge = self.provider(operator_catalog(storefront=storefront))
            state = provider.request('GET', '/api/shop/catalog', {}, {})
            self.assertIn('draft_error', state['operator_imports']['1219047'])
            with self.assertRaises(ValueError):
                provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                                 {'X-Shop-Revision': state['revision']})
            self.assertFalse(
                {'create', 'update', 'apply'} & {action for action, _ in bridge.actions}
            )

    def test_formal_apply_remains_an_explicit_shop_action(self):
        provider, bridge = self.provider()
        state = provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                                 {'X-Shop-Revision': 'r0'})
        self.assertNotIn('apply', [action for action, _ in bridge.actions])
        applied = provider.request('POST', '/api/shop/apply', {},
                                   {'X-Shop-Revision': state['revision']})
        self.assertEqual(applied['revision'], 'r3')
        self.assertEqual([action for action, _ in bridge.actions],
                         ['read', 'create', 'update', 'apply'])

    def test_stale_revision_cannot_leave_a_linked_draft(self):
        provider, bridge = self.provider()
        bridge.revision = 1
        with self.assertRaisesRegex(ValueError, 'stale'):
            provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                             {'X-Shop-Revision': 'r0'})
        self.assertEqual(bridge.state['catalog']['products'], [])
        self.assertEqual([action for action, _ in bridge.actions], ['read', 'create'])

    def test_prefill_failure_retires_draft_and_reports_cleanup_failure(self):
        provider, bridge = self.provider(bridge=FakeBridge(fail_actions={'update'}))
        with self.assertRaisesRegex(ValueError, 'forced update failure'):
            provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                             {'X-Shop-Revision': 'r0'})
        self.assertTrue(bridge.state['catalog']['products'][0]['retired'])
        self.assertFalse(bridge.state['catalog']['products'][0]['enabled'])
        self.assertEqual([action for action, _ in bridge.actions],
                         ['read', 'create', 'update', 'delete'])

        provider, bridge = self.provider(
            bridge=FakeBridge(fail_actions={'update', 'delete'}))
        with self.assertRaisesRegex(ValueError, '自动删除未完成'):
            provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                             {'X-Shop-Revision': 'r0'})
        self.assertTrue(bridge.state['catalog']['products'][0]['enabled'])
        self.assertEqual([action for action, _ in bridge.actions],
                         ['read', 'create', 'update', 'delete'])

    def test_duplicate_registrations_are_rejected_before_shop_access(self):
        catalog = operator_catalog()
        catalog['operators'].append(copy.deepcopy(catalog['operators'][0]))
        provider, bridge = self.provider(catalog)
        with self.assertRaisesRegex(ValueError, '登记重复'):
            provider.request('GET', '/api/shop/catalog', {}, {})
        self.assertEqual(bridge.actions, [])

    def test_missing_operator_configuration_keeps_normal_shop_available(self):
        provider, bridge = self.provider()
        provider.workspace.providers = {}
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        self.assertEqual(state['operator_imports'], {})
        self.assertEqual(state['items'][0]['Name'], 'native operator')

        provider, _ = self.provider()
        provider.workspace.providers['operators'] = SimpleNamespace(engine=None, config={})
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        self.assertEqual(state['operator_imports'], {})

    def test_unit_registration_coexists_and_collision_is_fail_closed(self):
        provider, bridge = self.provider()
        provider.workspace.settings['modules']['shop']['world'] = 'fixture-world'
        provider.workspace.providers['units'] = Units([{
            'category_id': 1119001, 'display_name': '测试机体',
        }])
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        self.assertIn('1119001', state['unit_imports'])
        self.assertIn('draft_defaults', state['operator_imports']['1219047'])

        provider, bridge = self.provider()
        provider.workspace.settings['modules']['shop']['world'] = 'fixture-world'
        provider.workspace.providers['units'] = Units([{
            'category_id': 1219047, 'display_name': '冲突机体',
        }])
        state = provider.request('GET', '/api/shop/catalog', {}, {})
        registration = state['operator_imports']['1219047']
        self.assertIn('同一商城物品类别', registration['draft_error'])
        self.assertEqual(state['items'][0]['Name'], '冲突机体')
        with self.assertRaisesRegex(ValueError, '同一商城物品类别'):
            provider.request('POST', '/api/shop/create', {'category_id': 1219047},
                             {'X-Shop-Revision': state['revision']})
        self.assertEqual([action for action, _ in bridge.actions], ['read'])

    def test_operator_and_shop_targets_must_match(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            operator_ini = root / 'operator.ini'
            shop_world = root / 'shop'
            shop_world.mkdir()
            operator_ini.write_text(
                '[DB Game]\nDBServerName=fixture\nDBName=operators\n',
                encoding='utf-8',
            )
            (shop_world / 'WorldServer.ini').write_text(
                '[DB Game]\nDBServerName=fixture\nDBName=shop\n',
                encoding='utf-8',
            )
            provider, bridge = self.provider()
            provider.workspace.providers['operators'].config['server_ini'] = str(operator_ini)
            provider.workspace.settings['modules']['shop']['world'] = str(shop_world)
            with self.assertRaisesRegex(ValueError, '同一宿主数据库'):
                provider.request('GET', '/api/shop/catalog', {}, {})
            self.assertEqual(bridge.actions, [])


if __name__ == '__main__':
    unittest.main()
