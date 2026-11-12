"""Batch roster server apply: ordering, cycles, idempotence and the stopped-server gate.

Every file and database row here belongs to the fixture: the SQL layer is the
repository tests' in-memory fake, and the server group is a temporary folder.
"""
from __future__ import annotations

from contextlib import contextmanager
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from manager import operations, storage
from manager.change_review import ChangeReviews, ReviewRequired, operation
from manager.units import Units
from owner.units import batch_server, repository
from owner.units.compiler import native_catalog
from tests.test_unit_import_repository import FakeConnection, FakeCursor, Result, name_row, unit_row, weapon_row
from tests.unit_import_support import unit as unit_record, weapon as weapon_record

REVISION = 'a' * 64


class BatchCursor(FakeCursor):
    """Adds the counting queries the batch readback uses to the repository fake."""

    def execute(self, sql, *params):
        compact = ' '.join(sql.split())
        if compact.startswith('SELECT COUNT(*) FROM dbo.['):
            self.connection.statements.append((compact, params))
            table = compact.split('dbo.[', 1)[1].split(']', 1)[0]
            rows = self.connection.tables.get(table, {})
            self.result = Result([(sum(1 for value in params if value in rows),)])
            return self
        if compact.startswith('SELECT DISTINCT FamilyKey,Revision FROM dbo.sdgo_UnitImportDefinitions'):
            self.connection.statements.append((compact, params))
            wanted = set(params)
            self.result = Result(sorted({(row['FamilyKey'], row['Revision']) for (kind, _), row in self.connection.ledger.items()
                                         if kind == 'unit' and row['FamilyKey'] in wanted}))
            return self
        return super().execute(sql, *params)


class BatchConnection(FakeConnection):
    def cursor(self):
        return BatchCursor(self)


def family(key, root, item, *, weapons, own_weapons, transform=0):
    unit = unit_row(root, transform=transform)
    unit.update(dict(zip(batch_server.WEAPON_FIELDS, weapons)))
    return {'family_key': key, 'root_unit_id': root, 'item_template_id': 1110008, 'units': [unit],
            'weapons': [weapon_row(identity) for identity in own_weapons],
            'item': {'CatalogueID': item, 'RefIndex': root, 'Name': 'Batch %d' % root},
            'names': [name_row(root)], 'shared_form_ids': {}, 'shared_weapon_ids': {},
            'behavior_profile': 'native-single-form-v1'}


def ordered_families():
    # A (first in publication order) uses a weapon owned by B, so B must go first.
    return [family('fixture.batch.a', 25070, 1119001, weapons=(61201, 45082, 61200, 43286), own_weapons=(61200,)),
            family('fixture.batch.b', 25080, 1119002, weapons=(61201, 45082, 61201, 43286), own_weapons=(61201,)),
            family('fixture.batch.c', 25090, 1119003, weapons=(61202, 45082, 61202, 43286), own_weapons=(61202,))]


def cyclic_families():
    # A transforms into B's form while B uses A's weapon: neither can go first.
    return [family('fixture.batch.a', 25070, 1119001, weapons=(61200, 45082, 61200, 43286), own_weapons=(61200,), transform=25080),
            family('fixture.batch.b', 25080, 1119002, weapons=(61200, 45082, 61201, 43286), own_weapons=(61201,))]


def write_publication(root: Path, families, *, graphs=None) -> Path:
    folder = root / 'publication'
    records = [(2, row['UnitNumber'], unit_record(row['UnitNumber'])) for value in families for row in value['units']]
    records += [(10, row['WeaponNo'], weapon_record(row['WeaponNo'])) for value in families for row in value['weapons']]
    catalog = folder / batch_server.CATALOG
    catalog.parent.mkdir(parents=True)
    catalog.write_bytes(native_catalog(list({(kind, identity): (kind, identity, raw) for kind, identity, raw in records}.values())))
    (folder / 'package/manifest.json').write_bytes(storage.json_bytes(
        {'files': {'client/sdgo/mods/unit_catalog/catalog.bin': batch_server._sha(catalog.read_bytes())}}))
    plan = {'schema': batch_server.PLAN_SCHEMA, 'owner': 'fixture', 'revision': REVISION, 'families': families,
            'skills': {'schema': 'sdgo.unit-server-dependencies/1', 'owner': 'fixture', 'revision': 'b' * 64, 'skills': []},
            'funnels': {'schema': 'sdgo.unit-server-funnels/1', 'owner': 'fixture', 'revision': 'c' * 64, 'funnels': []},
            'affiliations': []}
    (folder / 'server-plan.json').write_bytes(storage.json_bytes(plan))
    if graphs is not None:
        (folder / 'form-graph-requests.json').write_bytes(storage.json_bytes(
            {'schema': batch_server.GRAPH_SCHEMA, 'owner': 'fixture', 'revision': REVISION, 'requests': graphs}))
    return folder


class ServerGroup(unittest.TestCase):
    """A stopped native server group: WorldServer plus two battle hosts."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        server = self.root / 'Server'
        self.hosts = []
        for name, exe in (('WorldServer', 'WorldServer.exe'), ('BattleServer2', 'BattleServer.exe'), ('BattleServer3', 'BattleServer.exe')):
            host = server / name
            (host / 'sdgo').mkdir(parents=True)
            (host / exe).write_bytes(b'stopped fixture executable')
            (host / 'sdgo/core.dll').write_bytes(operations.CORE_CONTRACT)
            if exe == 'BattleServer.exe':
                mod = host / 'sdgo/mods' / batch_server.GRAPH_MOD
                mod.mkdir(parents=True)
                (mod / (batch_server.GRAPH_MOD + '.dll')).write_bytes(b'fixture module')
                (mod / 'mod.ini').write_bytes(b'[mod]\r\nid=unit_catalog_server\r\nenabled=0\r\n\r\n[settings]\r\nEnabled=1\r\n')
                self.hosts.append(host)
        self.ini = server / 'WorldServer/WorldServer.ini'
        self.ini.write_text('[DB Game]\nDBServerName=127.0.0.1,7\nDBName=fixture\nID=fixture\nPWD=fixture\n')
        for patcher in (patch.dict(os.environ, {'LOCALAPPDATA': str(self.root / 'local')}),
                        patch.object(operations, 'channel_ports', return_value=[9302, 9303])):
            patcher.start()
            self.addCleanup(patcher.stop)
        self.fake = BatchConnection()
        connect = patch.object(repository, '_connect', side_effect=lambda ini: self.fake)
        connect.start()
        self.addCleanup(connect.stop)

    def receipts(self):
        return [storage.read_json(path) for path in (self.root / 'Server/sdgo/local-state/changes').glob('*.json')]

    def writes(self):
        return [sql for sql, _ in self.fake.statements if sql.startswith(('INSERT', 'UPDATE', 'CREATE'))]


class OrderingTests(unittest.TestCase):
    def test_cross_family_references_go_after_their_owner(self):
        order = [row['family_key'] for row in batch_server.family_order(ordered_families())]
        self.assertEqual(order, ['fixture.batch.b', 'fixture.batch.c', 'fixture.batch.a'])

    def test_transform_target_and_declared_shared_owner_are_dependencies(self):
        families = ordered_families()
        families[1]['units'][0]['UnitNumber_Transform'] = 25090  # b transforms into c's form
        self.assertEqual([row['family_key'] for row in batch_server.family_order(families)],
                         ['fixture.batch.c', 'fixture.batch.b', 'fixture.batch.a'])
        families[2]['shared_weapon_ids'] = {'61200': 'fixture.batch.a'}  # c declares a as an owner
        with self.assertRaisesRegex(ValueError, '循环依赖'):
            # c waits for a (declared owner), a waits for b (weapon), b waits for c (transform).
            batch_server.family_order(families)

    def test_cycle_is_refused_with_its_members(self):
        with self.assertRaisesRegex(ValueError, 'fixture.batch.a → fixture.batch.b → fixture.batch.a'):
            batch_server.family_order(cyclic_families())

    def test_independent_families_keep_publication_order(self):
        families = [family('fixture.batch.%d' % index, 25100 + index, 1119100 + index,
                           weapons=(45082, 45082, 45082, 43286), own_weapons=()) for index in range(5)]
        self.assertEqual([row['root_unit_id'] for row in batch_server.family_order(families)], [25100, 25101, 25102, 25103, 25104])


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_roster_root_or_publication_folder_is_accepted(self):
        folder = write_publication(self.root, ordered_families())
        for path in (folder, self.root):
            publication = batch_server.load_publication(path)
            self.assertEqual(publication.summary()['families'], 3)
            self.assertEqual(publication.unit_ids(), [25070, 25080, 25090])

    def test_publications_with_the_earlier_schema_identifiers_still_load(self):
        folder = write_publication(self.root, ordered_families())
        plan = storage.read_json(folder / 'server-plan.json')
        for schema, loads in (('sdgo.gunion-roster-server-plan/1', True), ('sdgo.other-server-plan/1', False)):
            plan['schema'] = schema
            (folder / 'server-plan.json').write_bytes(storage.json_bytes(plan))
            if loads:
                self.assertEqual(batch_server.load_publication(folder).summary()['families'], 3)
            else:
                with self.assertRaisesRegex(ValueError, '不支持的批量服务端计划版本'):
                    batch_server.load_publication(folder)

    def test_tampered_catalog_or_duplicate_ownership_is_refused(self):
        folder = write_publication(self.root / 'tampered', ordered_families())
        catalog = folder / batch_server.CATALOG
        raw = bytearray(catalog.read_bytes())
        raw[-1] ^= 1
        catalog.write_bytes(bytes(raw))
        with self.assertRaisesRegex(ValueError, '哈希不一致'):
            batch_server.load_publication(folder)
        families = ordered_families()
        families[2]['weapons'].append(weapon_row(61200))
        write_publication(self.root, families)
        with self.assertRaisesRegex(ValueError, '同时由'):
            batch_server.load_publication(self.root)

    def test_graph_requests_must_match_the_plan_and_catalog(self):
        families = ordered_families()
        request = {'schema': 'sdgo.unit-form-graphs/1', 'owner': 'fixture.batch.a', 'revision': REVISION,
                   'families': ['fixture.batch.a'], 'roots': [25070], 'records': [{'id': 25070, 'client_sha256': '0' * 64}]}
        write_publication(self.root, families, graphs=[request])
        with self.assertRaisesRegex(ValueError, '客户端机体目录不一致'):
            batch_server.load_publication(self.root)


class ApplyTests(ServerGroup):
    def test_read_only_restart_verification_accepts_running_services_but_refuses_edited_rows(self):
        folder=write_publication(self.root,ordered_families())
        batch_server.apply(folder,self.ini)
        before=len(self.writes())
        with patch.object(batch_server,'running_services',return_value=['active fixture service']):
            self.assertTrue(batch_server.verify_installed(folder,self.ini,types.SimpleNamespace(root=self.root/'client')))
            self.fake.tables['tbl_Units'][25070]['HP']+=1
            self.assertFalse(batch_server.verify_installed(folder,self.ini,types.SimpleNamespace(root=self.root/'client')))
        self.assertEqual(before,len(self.writes()),'restart verification must not write fixed or player data')

    def test_dependency_order_applies_where_plan_order_would_fail(self):
        folder = write_publication(self.root, ordered_families())
        events = []
        result = batch_server.apply(folder, self.ini, events.append)
        units = [params[0] for sql, params in self.fake.statements if sql.startswith('INSERT dbo.tbl_Units')]
        self.assertEqual(units, [25080, 25090, 25070])
        self.assertTrue(result['applied'])
        self.assertEqual(result['activation'], 'pending_restart')
        self.assertTrue(result['readback']['complete'])
        self.assertEqual(result['readback']['ledger_families_at_revision'], 3)
        self.assertEqual([event['detail'] for event in events if event['stage'] == 'families' and event['detail']],
                         ['fixture.batch.b', 'fixture.batch.c', 'fixture.batch.a'])
        self.assertEqual(events[0]['stage'], 'gate')
        sql = '\n'.join(statement for statement, _ in self.fake.statements)
        for player_table in ('tbl_User', 'tbl_Inven', 'tbl_Shop', 'Machine'):
            self.assertNotIn(player_table, sql)
        self.assertFalse(result['grants_units'])
        receipt, = self.receipts()
        self.assertEqual(receipt['action'], batch_server.ACTION)
        self.assertEqual(receipt['status'], 'pending_restart')
        self.assertEqual(receipt['content'], {'revision': REVISION})
        # The ordinary per-family path in plan order refuses the first family.
        plain = repository.Repository(server_ini=self.ini)
        fresh = BatchConnection()
        with patch.object(repository, '_connect', return_value=fresh):
            plain = repository.Repository(server_ini=self.ini)
            with self.assertRaisesRegex(ValueError, 'WeaponNumber_1=61201 不存在'):
                plain.check_plan(batch_server.family_plan(ordered_families()[0], REVISION, plain.catalog['items']))

    def test_rerun_is_idempotent_through_the_ownership_ledger(self):
        folder = write_publication(self.root, ordered_families())
        first = batch_server.apply(folder, self.ini)
        tables = copy.deepcopy({name: self.fake.tables[name] for name in ('tbl_Units', 'tbl_WeaponsRev', 'tbl_UnitNames', 'tbl_ItemCategory')})
        ledger = copy.deepcopy(self.fake.ledger)
        second = batch_server.apply(folder, self.ini)
        self.assertEqual(first['rows_inserted'], 3 * 4)
        self.assertEqual(second['rows_inserted'], 0)
        self.assertEqual(second['rows_updated'], 3 * 4)
        self.assertEqual(tables, {name: self.fake.tables[name] for name in tables})
        self.assertEqual(set(ledger), set(self.fake.ledger))
        self.assertEqual({key: row['InstalledHash'] for key, row in ledger.items()},
                         {key: row['InstalledHash'] for key, row in self.fake.ledger.items()})
        self.assertTrue(second['readback']['complete'])
        statuses = sorted(receipt['status'] for receipt in self.receipts())
        self.assertEqual(statuses, ['pending_restart', 'superseded'])
        preflight = batch_server.preflight(folder, self.ini)
        self.assertEqual((preflight['families_installed'], preflight['families_new']), (3, 0))
        self.assertTrue(preflight['ready'])

    def test_cycle_is_refused_before_any_write(self):
        folder = write_publication(self.root, cyclic_families())
        with self.assertRaisesRegex(ValueError, '循环依赖'):
            batch_server.apply(folder, self.ini)
        self.assertFalse(self.fake.statements)
        self.assertFalse(self.receipts())
        preflight = batch_server.preflight(folder, self.ini)
        self.assertFalse(preflight['ready'])
        self.assertIn('循环依赖', preflight['problems'][0]['error'])
        self.assertFalse(self.writes())

    def test_running_services_block_apply_without_entering_the_writer(self):
        folder = write_publication(self.root, ordered_families())

        @contextmanager
        def running(path, replace=False):
            if path.name == 'BattleServer.exe' and path.parent.name == 'BattleServer3':
                raise ValueError('请先停止维护范围内的原生服务，并确认程序文件可访问：BattleServer3/BattleServer.exe')
            yield

        with patch.object(operations, 'unmapped_image', running):
            preflight = batch_server.preflight(folder, self.ini)
            self.assertEqual(preflight['running_services'], ['BattleServer3/BattleServer.exe'])
            self.assertFalse(preflight['servers_stopped'])
            self.assertFalse(preflight['ready'])
            self.assertFalse(preflight['problems'])
            before = len(self.fake.statements)
            with self.assertRaisesRegex(ValueError, '请先停止'):
                batch_server.apply(folder, self.ini)
        self.assertFalse(self.writes())
        self.assertEqual(len(self.fake.statements), before)
        self.assertFalse(self.receipts())

    def test_refused_family_stops_the_batch_before_its_first_write(self):
        families = ordered_families()
        families[2]['units'][0]['UnitGrowIndex'] = 99
        folder = write_publication(self.root, families)
        with self.assertRaisesRegex(ValueError, 'fixture.batch.c.*成长索引 99.*未写入任何内容'):
            batch_server.apply(folder, self.ini)
        self.assertFalse(self.writes())
        receipt, = self.receipts()
        self.assertEqual(receipt['status'], 'failed')

    def test_form_graphs_are_installed_on_every_battle_host_with_backup(self):
        families = ordered_families()
        request = {'schema': 'sdgo.unit-form-graphs/1', 'owner': 'fixture.batch.a', 'revision': REVISION,
                   'families': ['fixture.batch.a'], 'roots': [25070], 'records': []}
        folder = write_publication(self.root, families, graphs=[])
        publication = batch_server.load_publication(folder)
        request['records'] = [{'id': 25070, 'client_sha256': batch_server._sha(publication.records[2][25070])}]
        folder = write_publication(self.root / 'with-graph', families, graphs=[request])
        from test_unit_form_graph_runtime import fixture, prepared, grade_rows
        from owner.units.form_graph_runtime import compile_native_files
        previous=fixture();previous_plan=prepared(previous)
        previous_plan['owners'][0]['owner']='fixture.batch.a'
        old_candidate,old_authority=compile_native_files(previous_plan,previous[2],grade_rows(previous[2]))
        for host in self.hosts:
            (host/'sdgo/mods/unit_catalog_server/form-graphs.bin').write_bytes(old_candidate)
            authority=host/'sdgo/install-state/unit-form-authority.bin'
            authority.parent.mkdir(parents=True,exist_ok=True);authority.write_bytes(old_authority)
        compiled = []

        def compile_graphs(cursor, requests, records, *, originals):
            compiled.append((len(requests), sorted(records), originals))
            return b'candidate graph', b'host authority'

        with patch('owner.units.form_graph_runtime.prepare_native_from_cursor', compile_graphs):
            result = batch_server.apply(folder, self.ini)
        self.assertEqual(compiled, [(1, [25070, 25080, 25090], {})])
        self.assertEqual(result['graphs']['hosts'], ['BattleServer2', 'BattleServer3'])
        for host in self.hosts:
            self.assertEqual((host / 'sdgo/mods/unit_catalog_server/form-graphs.bin').read_bytes(), b'candidate graph')
            self.assertEqual((host / 'sdgo/install-state/unit-form-authority.bin').read_bytes(), b'host authority')
            values = storage.ini_values((host / 'sdgo/mods/unit_catalog_server/mod.ini').read_bytes())
            self.assertEqual(values.get('mod', 'enabled'), '1')
            self.assertEqual(values.get('settings', 'Enabled'), '1')
        receipt, = self.receipts()
        backup = self.root / 'Server/sdgo/local-state/changes/backups' / receipt['id'] / 'BattleServer2'
        self.assertEqual((backup / 'sdgo/mods/unit_catalog_server/form-graphs.bin').read_bytes(), old_candidate)
        self.assertIn(b'enabled=0', (backup / 'sdgo/mods/unit_catalog_server/mod.ini').read_bytes())
        self.assertEqual((result['readback']['graph_hosts'], result['readback']['graph_hosts_installed']), (2, 2))

    def test_missing_server_binding_is_refused_before_the_gate(self):
        families = ordered_families()
        folder = write_publication(self.root, families, graphs=[])
        publication = batch_server.load_publication(folder)
        request = {'schema': 'sdgo.unit-form-graphs/1', 'owner': 'fixture.batch.a', 'revision': REVISION,
                   'families': ['fixture.batch.a'], 'roots': [25070],
                   'records': [{'id': 25070, 'client_sha256': batch_server._sha(publication.records[2][25070])}]}
        folder = write_publication(self.root / 'with-graph', families, graphs=[request])
        (self.hosts[1] / 'sdgo/mods/unit_catalog_server/unit_catalog_server.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'BattleServer3'):
            batch_server.apply(folder, self.ini)
        self.assertFalse(self.fake.statements)
        self.assertIn('BattleServer3', batch_server.preflight(folder, self.ini)['problems'][0]['error'])


class ManagerActionTests(ServerGroup):
    def provider(self):
        workspace = types.SimpleNamespace(settings={'modules': {'units': {'server_ini': str(self.ini)}}}, root=self.root / 'state',
            services=types.SimpleNamespace(register=lambda *args:None),save_workspace=lambda:None)
        return Units(workspace)

    def test_job_reports_progress_and_completion(self):
        folder = write_publication(self.root, ordered_families())
        provider = self.provider()
        state = provider.request('POST', '/api/units/batch/apply', {'publication': str(folder)}, {})
        provider._batch_thread.join(30)
        final = provider.request('GET', '/api/units/batch', {}, {})['batch']
        self.assertIn(state['batch']['status'], ('running', 'completed'))
        self.assertEqual(final['status'], 'completed')
        self.assertEqual((final['done'], final['total']), (3, 3))
        self.assertTrue(final['result']['readback']['complete'])
        readback = provider.request('POST', '/api/units/batch/readback', {'publication': str(folder)}, {})['readback']
        self.assertEqual(readback['ledger_families_at_revision'], 3)

    def test_gate_refusal_is_returned_to_the_request(self):
        folder = write_publication(self.root, ordered_families())
        provider = self.provider()

        @contextmanager
        def running(path, replace=False):
            raise ValueError('请先停止维护范围内的原生服务')
            yield

        with patch.object(operations, 'unmapped_image', running):
            with self.assertRaisesRegex(ValueError, '请先停止'):
                provider.request('POST', '/api/units/batch/apply', {'publication': str(folder)}, {})
        self.assertEqual(provider.batch_state()['batch']['status'], 'failed')
        self.assertFalse(self.writes())

    def test_second_job_is_refused_while_one_runs(self):
        folder = write_publication(self.root, ordered_families())
        provider = self.provider()
        release = threading.Event()

        def slow(folder, ini, progress):
            progress({'stage': 'shared', 'done': 0, 'total': 3, 'detail': ''})
            release.wait(30)
            return {'readback': {}}

        with patch.object(batch_server, 'apply', slow):
            provider.start_batch(str(folder))
            with self.assertRaisesRegex(ValueError, '正在应用'):
                provider.start_batch(str(folder))
            release.set()
            provider._batch_thread.join(30)
        self.assertEqual(provider.batch_state()['batch']['status'], 'completed')

    def test_requests_without_server_or_with_extra_fields_are_refused(self):
        provider = self.provider()
        with self.assertRaisesRegex(ValueError, '请求无效'):
            provider.request('POST', '/api/units/batch/preflight', {'publication': 'x', 'force': True}, {})
        provider.workspace.settings['modules']['units'] = {}
        with self.assertRaisesRegex(ValueError, 'WorldServer INI'):
            provider.request('POST', '/api/units/batch/apply', {'publication': str(self.root)}, {})

    def test_apply_needs_a_one_use_review_and_leaves_the_gate_to_the_job(self):
        declaration = operation('units', 'POST', '/api/units/batch/apply')
        self.assertTrue(declaration['background'])
        self.assertEqual(declaration['kind'], 'fixed_catalog')
        self.assertIsNone(operation('units', 'POST', '/api/units/batch/preflight'))
        workspace = types.SimpleNamespace(providers={})
        provider = types.SimpleNamespace(id='units', config={'server_ini': str(self.ini)})
        controller = ChangeReviews(workspace)
        body = {'publication': 'roster'}
        with self.assertRaises(ReviewRequired) as required:
            with controller.execute(provider, 'POST', '/api/units/batch/apply', body, {}):
                self.fail('writer entered without review')
        token = required.exception.plan['review']
        with patch.object(operations, 'server_change', side_effect=AssertionError('request must not hold the gate')):
            with controller.execute(provider, 'POST', '/api/units/batch/apply', body, {'X-Change-Review': token}):
                pass
        with self.assertRaises(ReviewRequired):
            with controller.execute(provider, 'POST', '/api/units/batch/apply', body, {'X-Change-Review': token}):
                self.fail('review replayed')


if __name__ == '__main__':
    unittest.main()
