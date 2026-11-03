"""Wallet ownership/restart regressions; no real accounts or production SQL."""
from pathlib import Path
import configparser
import os
import tempfile
import time
import unittest
import uuid
from unittest import mock
from manager.services import WalletService


class Ledger:
    def __init__(self):self.scope=uuid.uuid4();self.probes=0;self.fail_next=False
    def scope_id(self):
        self.probes+=1
        if self.fail_next:
            self.fail_next=False
            error=type('OperationalError',(Exception,),{})
            raise error('08S01','private connection details')
        return self.scope


@unittest.skipUnless(os.name=='nt','Manager wallet uses a native Windows ownership lock')
class WalletLifecycleTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name);self.ini=self.root/'server.ini';self.ini.write_text('[DB StoreBilling]\n')
        self.runtime=self.root/'runtime';self.ledger=Ledger();self.connections=[];self.services=[]
        def connect(*args,**kwargs):
            connection=mock.Mock();self.connections.append(connection);return connection
        patch=mock.patch('manager.database.connect',side_effect=connect);self.connect=patch.start();self.addCleanup(patch.stop)
        patch=mock.patch('owner.rewards.sql_repository.RewardWalletRepository',return_value=self.ledger)
        patch.start();self.addCleanup(patch.stop)
        self.addCleanup(lambda:[s.stop() for s in self.services])

    def service(self):
        service=WalletService();self.services.append(service);return service

    def status(self):
        c=configparser.ConfigParser();c.read(self.runtime/'wallet-status.ini');return dict(c['wallet'])

    def wait(self,predicate,timeout=5):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if predicate():return
            time.sleep(.025)
        self.fail('wallet did not reach its expected bounded recovery state')

    def test_live_owner_excludes_second_service(self):
        first=self.service();first.start(self.ini,self.runtime)
        self.assertTrue(first.state()['healthy'])
        with self.assertRaises(ValueError):self.service().start(self.ini,self.runtime)
        self.assertEqual(self.connect.call_count,1)

    def test_explicit_stop_marks_worker_unhealthy(self):
        service=self.service();service.start(self.ini,self.runtime);service.stop()
        self.assertEqual(self.status()['healthy'],'0')
        self.assertFalse(service.state()['running'])

    def test_planned_restart_retains_only_the_last_verified_lease_and_successor_claims_it(self):
        first=self.service();first.start(self.ini,self.runtime)
        before=(self.runtime/'wallet-status.ini').read_bytes()
        first.prepare_restart();first.stop()
        self.assertEqual((self.runtime/'wallet-status.ini').read_bytes(),before,
                         'handoff must neither publish a false outage nor forge a later heartbeat')
        started=time.monotonic();second=self.service();second.start(self.ini,self.runtime)
        self.assertTrue(second.state()['healthy'])
        self.assertLess(time.monotonic()-started,2,
                        'a released ownership lock must not be blocked by the preceding fresh heartbeat')

    def test_failed_initial_connection_recovers_without_another_start_request(self):
        successful=self.connect.side_effect
        attempts=0
        def transient(*args,**kwargs):
            nonlocal attempts
            attempts+=1
            if attempts==1:raise ValueError('SQLSTATE 08001; private password must not escape')
            return successful(*args,**kwargs)
        self.connect.side_effect=transient
        service=self.service();service.start(self.ini,self.runtime)
        self.assertTrue(service.state()['running'])
        self.assertNotIn('private',service.state()['error'])
        self.wait(lambda:service.state()['healthy'])
        self.assertGreaterEqual(attempts,2)

    def test_broken_connection_is_reopened_with_the_same_wallet_scope(self):
        service=self.service();service.start(self.ini,self.runtime)
        self.ledger.fail_next=True
        self.wait(lambda:self.connect.call_count>=2 and service.state()['healthy'])
        self.assertTrue(self.connections[0].close.called)
        self.assertEqual(self.status()['scopeid'],str(self.ledger.scope))


if __name__=='__main__':unittest.main()
