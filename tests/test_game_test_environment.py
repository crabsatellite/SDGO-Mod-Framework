from pathlib import Path
import dataclasses
import json
import multiprocessing
import os
import tempfile
import threading
import unittest
from unittest import mock

from tools.game_test_environment import Registry, ManagedEnvironment, process_identity


class Driver:
    def __init__(self, fail=False):
        self.fail = fail
        self.calls = []

    def cleanup(self, lease, resources):
        self.calls.append(('cleanup', lease.id, list(resources)))
        return {'status': 'attention' if self.fail else 'clean', 'resources_released': len(resources)}

    def archive(self, lease, archive):
        self.calls.append(('archive', lease.id))
        (archive / 'test-evidence.json').write_text('{"native_game_claim":false}', encoding='utf-8')


def allocate_worker(registry, parent, archive, ready, release):
    r = Registry(Path(registry))
    lease = r.allocate(Path(parent), 'fixture-template', probe=False)
    ready.put(lease.public())
    release.wait(20)
    ManagedEnvironment(r, lease, Driver(), Path(archive)).close()


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.registry = Registry(self.base / 'registry')
        self.parent = self.base / 'runtime'
        self.archive = self.base / 'evidence'

    def tearDown(self):
        self.temp.cleanup()

    def lease(self):
        return self.registry.allocate(self.parent, 'fixture-template', probe=False)

    def test_controller_identity_includes_birth_time(self):
        self.assertIsNotNone(process_identity(os.getpid()))
        self.assertIsNone(process_identity(2147483647))

    def test_separate_environments_do_not_share_addresses_or_port_pools(self):
        a, b = self.lease(), self.lease()
        self.assertNotEqual(a.ip, b.ip)
        self.assertNotEqual(a.udp_first, b.udp_first)
        self.assertNotEqual(a.root, b.root)
        self.assertNotEqual(a.token, b.token)
        self.assertNotIn('token', a.public())

    def test_actual_parallel_processes_hold_independent_leases(self):
        ctx = multiprocessing.get_context('spawn')
        ready, release = ctx.Queue(), ctx.Event()
        children = [ctx.Process(target=allocate_worker, args=(str(self.registry.directory), str(self.parent),
                    str(self.archive), ready, release)) for _ in range(2)]
        try:
            for child in children:
                child.start()
            rows = [ready.get(timeout=20) for _ in children]
            self.assertEqual(len({x['ip'] for x in rows}), 2)
            self.assertEqual(len({x['udp_first'] for x in rows}), 2)
            self.assertEqual(len([x for x in self.registry.list() if x['state'] != 'closed']), 2)
        finally:
            release.set()
            for child in children:
                child.join(timeout=20)
                if child.is_alive():
                    child.kill(); child.join()
        self.assertTrue(all(child.exitcode == 0 for child in children))
        self.assertTrue(all(x['state'] == 'closed' for x in self.registry.list()))

    def test_normal_exit_releases_runtime_and_keeps_compact_evidence(self):
        lease, driver = self.lease(), Driver()
        self.registry.record(lease, {'kind': 'test_resource', 'id': 1})
        (lease.root / 'disposable').write_text('temporary')
        with ManagedEnvironment(self.registry, lease, driver, self.archive):
            pass
        self.assertFalse(lease.root.exists())
        self.assertTrue((self.archive / lease.id / 'test-evidence.json').is_file())
        self.assertEqual(self.registry.list()[0]['state'], 'closed')
        self.assertEqual(driver.calls[0][2], [{'kind': 'test_resource', 'id': 1}])

    def test_failure_also_cleans_up(self):
        lease = self.lease()
        with self.assertRaisesRegex(ValueError, 'fixture failed'):
            with ManagedEnvironment(self.registry, lease, Driver(), self.archive):
                raise ValueError('fixture failed')
        self.assertFalse(lease.root.exists())
        self.assertEqual(self.registry.list()[0]['state'], 'closed')

    def test_success_deletes_test_records_after_runtime_cleanup(self):
        lease=self.lease();driver=Driver()
        with ManagedEnvironment(self.registry,lease,driver,self.archive) as managed:
            (lease.root/'result.json').write_text('{"status":"pass"}',encoding='utf-8')
        self.assertTrue(managed.evidence_deleted)
        self.assertFalse((self.archive/lease.id).exists())
        self.assertFalse(lease.root.exists())
        self.assertEqual(self.registry.list()[0]['state'],'closed')

    def test_live_foreign_controller_cannot_be_recovered(self):
        lease, driver = self.lease(), Driver()
        with self.assertRaisesRegex(RuntimeError, 'live controller'):
            ManagedEnvironment(self.registry, lease, driver, self.archive).close(recovering=True)
        self.assertEqual(driver.calls, [])
        self.assertTrue(lease.root.exists())

    def test_wrong_ownership_token_never_calls_cleanup(self):
        lease, driver = self.lease(), Driver()
        with self.assertRaisesRegex(RuntimeError, 'ownership'):
            ManagedEnvironment(self.registry, dataclasses.replace(lease, token='foreign'), driver, self.archive).close()
        self.assertEqual(driver.calls, [])
        self.assertTrue(lease.root.exists())

    def test_cleanup_failure_retains_lease_and_gives_agent_an_action(self):
        lease = self.lease()
        with self.assertRaises(RuntimeError):
            ManagedEnvironment(self.registry, lease, Driver(fail=True), self.archive).close()
        self.assertTrue(lease.root.exists())
        self.assertEqual(self.registry.list()[0]['state'], 'cleanup_required')
        action = json.loads((self.archive / lease.id / 'agent-action.json').read_text())
        self.assertEqual(action['action'][-1], 'recover')

    def test_resource_journal_does_not_lose_concurrent_transport_children(self):
        lease = self.lease()
        threads = [threading.Thread(target=lambda n=n: self.registry.record(lease, {'kind': 'child', 'id': n})) for n in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual({x['id'] for x in self.registry.validate(lease)['resources']}, set(range(12)))

    def test_template_directory_is_outside_disposable_runtime(self):
        template = self.base / 'template'
        template.mkdir();(template / 'accounts').write_text('five reusable fixtures')
        lease = self.lease()
        ManagedEnvironment(self.registry, lease, Driver(), self.archive).close()
        self.assertEqual((template / 'accounts').read_text(), 'five reusable fixtures')

    def test_abandoned_recovery_is_claimed_by_one_controller(self):
        lease=self.lease()
        with self.registry.connection() as cn:
            cn.execute('UPDATE environments SET owner_pid=2147483647 WHERE id=?',(lease.id,))
        adopted=self.registry.claim_recovery(lease.id)
        self.assertEqual(adopted.owner_pid,os.getpid())
        with self.assertRaisesRegex(RuntimeError,'live controller'):
            self.registry.claim_recovery(lease.id)
        ManagedEnvironment(self.registry,adopted,Driver(),self.archive).close()

    def test_recovery_finishes_crash_between_deletion_and_registry_update(self):
        import shutil
        lease=self.lease()
        self.registry.update(lease,'cleaning')
        shutil.rmtree(lease.root)
        ManagedEnvironment(self.registry,lease,Driver(),self.archive).close()
        self.assertEqual(self.registry.list()[0]['state'],'closed')

    def test_failed_marker_write_does_not_leave_unregistered_runtime(self):
        with mock.patch('tools.game_test_environment.atomic_json',side_effect=OSError('disk')):
            with self.assertRaises(OSError):self.lease()
        self.assertEqual(list(self.parent.iterdir()),[])
        self.assertEqual(self.registry.list(),[])


class NativeTemplateTests(unittest.TestCase):
    def test_fleet_selection_obeys_native_costs_for_large_ma_and_four_unit_cases(self):
        from tools.game_test_fixtures import choose_fleet
        ships=[(99,270005,0,15)]
        primary={'uid':1,'unit_id':24788}
        others=[{'uid':i+2,'unit_id':100+i} for i in range(4)]
        stars={24788:14,**{100+i:3 for i in range(4)}}
        ship,deck=choose_fleet(ships,primary,others,stars)
        self.assertEqual(ship[0],99);self.assertEqual(len(deck),1)
        stars[24788]=4
        _,deck=choose_fleet(ships,primary,others,stars)
        self.assertEqual(len(deck),4)
        self.assertEqual(deck[0],primary)
        stars[24788]=16
        self.assertIsNone(choose_fleet(ships,primary,others,stars))

    def test_native_database_key_spelling_and_unrelated_values_survive_rebase(self):
        from tools.game_test_native_environment import rebase_configuration
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);folder=root/'Server/StoreServer';folder.mkdir(parents=True)
            ini=folder/'StoreServer.ini'
            ini.write_bytes(b'[DB Member]\r\nDBServerName=127.0.0.1,12345\r\nDBName=GDCommon\r\nID=old\r\nPWD=old-secret\r\nUnknownMixedCase=0015\r\n')
            rebase_configuration(root,root,root,'127.84.2.1',{'port':54321,'app_user':'native','app_password':'new-secret'},old_ip='127.91.0.1',old_sql_port=12345,old_password='old-secret')
            text=ini.read_text(encoding='cp936')
            self.assertIn('DBServerName=127.0.0.1,54321',text)
            self.assertIn('DBName=GDCommon',text)
            self.assertIn('UnknownMixedCase=0015',text)
            self.assertNotIn('dbservername=',text)

    def test_five_account_roles_have_their_primaries_in_the_shared_roster(self):
        from tools.game_test_fixtures import ROSTER,PRIMARIES,ROLES,build_accounts
        self.assertEqual(len(PRIMARIES),5)
        self.assertEqual(len(set(ROLES)),5)
        self.assertTrue(set(PRIMARIES).issubset(ROSTER))
        with self.assertRaisesRegex(RuntimeError,'isolated template'):
            build_accounts(None,Path('unused'),'unused')

    def test_configuration_rebase_covers_chinese_and_escaped_paths_and_alias_ips(self):
        from tools.game_test_native_environment import rebase_configuration
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'Server').mkdir()
            old=root/'旧模板';new=root/'新副本'
            forms=(str(old),old.as_posix(),json.dumps(str(old))[1:-1])
            for encoding in ('utf-8','utf-16-le','cp936'):
                path=root/(encoding+'.cfg')
                path.write_bytes(('\n'.join(forms)+'\n127.91.0.1 127.91.0.3 127.0.0.1,12345 old-secret').encode(encoding))
            rebase_configuration(root,old,new,'127.84.2.1',{'port':54321,'app_user':'native','app_password':'new-secret'},
                                 old_ip='127.91.0.1',old_sql_port=12345,old_password='old-secret')
            for encoding in ('utf-8','utf-16-le','cp936'):
                text=(root/(encoding+'.cfg')).read_bytes().decode(encoding)
                self.assertNotIn(str(old),text)
                self.assertIn(str(new),text)
                self.assertIn('127.84.2.3',text)
                self.assertIn('127.0.0.1,54321',text)
                self.assertNotIn('old-secret',text)


class CandidateAdmissionTests(unittest.TestCase):
    def setUp(self):
        from tests.test_game_test import GameTestRunnerTests
        self.fixture=GameTestRunnerTests(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.addCleanup(self.fixture.tearDown)
        from tests.test_game_test import game_test
        self.gt=game_test

    def test_explicit_mod_resources_and_symbols_are_sealed_before_execution(self):
        f=self.fixture;source=f.stage/'client/sdgo/mods/candidate_fix'
        (source/'resources').mkdir(parents=True)
        (source/'mod.ini').write_text('[mod]\nid=candidate_fix\ndll=candidate_fix.dll\nhosts='+self.gt.HOST_KEY+'\nrequires=candidate.native\n',encoding='utf-8')
        (source/'candidate_fix.dll').write_bytes(b'candidate')
        (source/'resources/data.bin').write_bytes(b'fixture-resource')
        symbols=f.stage/('client/sdgo/symbols/'+self.gt.HOST_KEY+'.ini')
        with symbols.open('a',encoding='utf-8') as stream:stream.write('\n[candidate.native]\nkind=code\nrva=candidate\n')
        run=f.prepare('candidate',extra_mods=('candidate_fix',))
        metadata=f.metadata(run)
        self.assertIn('sdgo/mods/candidate_fix/resources/data.bin',metadata['client']['immutable_hashes'])
        self.assertTrue(self.gt.read_ini(run/('client/sdgo/symbols/'+self.gt.HOST_KEY+'.ini')).has_section('candidate.native'))
        self.gt.verify_hashes(metadata)
        (run/'client/sdgo/mods/candidate_fix/resources/data.bin').write_bytes(b'changed')
        with self.assertRaises(self.gt.GameTestError):self.gt.verify_hashes(metadata)

    def test_roundtrip_scenario_is_explicit_and_manifest_hash_bound(self):
        run=self.fixture.prepare('roundtrip',play_unit_uid=17,apsalus_roundtrip=True)
        path=run/'client/sdgo/mods/game_test_client/mod.ini'
        self.assertEqual(self.gt.read_ini(path).get('settings','ApsalusRoundtrip'),'1')
        metadata=self.fixture.metadata(run)
        self.assertIn('sdgo/mods/game_test_client/mod.ini',metadata['client']['immutable_hashes'])
        with self.assertRaises(self.gt.GameTestError):
            self.fixture.prepare('incomplete-roundtrip',apsalus_roundtrip=True,bootstrap_only=True)

    def test_disabled_retained_diagnostic_does_not_require_its_historical_aliases(self):
        root=self.fixture.template/'sdgo/mods/old_probe'
        root.mkdir()
        manifest=root/'mod.ini'
        manifest.write_text('[mod]\nid=old_probe\nenabled=0\nrequires=old.unavailable\n',encoding='utf-8')
        (root/'old_probe.dll').write_bytes(b'old-inactive-probe')
        self.fixture.prepare('disabled-probe')
        manifest.write_text('[mod]\nid=old_probe\nenabled=1\nrequires=old.unavailable\n',encoding='utf-8')
        with self.assertRaisesRegex(self.gt.GameTestError,'retained Mod symbols'):
            self.fixture.prepare('active-probe')


if __name__ == '__main__':
    unittest.main()
