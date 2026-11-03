from pathlib import Path
import json
import os
import tempfile
import unittest
from manager import storage,updates
from manager.update_restart import pending
from manager.workspace import Workspace

ROOT=Path(__file__).resolve().parents[1]


class Controller:
    def __init__(self):self.running=False;self.handoffs=0
    def state(self):return {'running':self.running,'healthy':self.running,'error':''}
    def start(self):self.running=True
    def stop(self):self.running=False
    def prepare_restart(self):self.handoffs+=1


class RestartHandoffTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name);self.workspace=Workspace(self.root/'workspace',ROOT/'mods')
        self.controller=Controller()
        self.workspace.services.register('fixture.wallet','Fixture',self.controller,self.controller.start)
        self.workspace.services.start('fixture.wallet')
        self.source=self.workspace.root/'updates/releases/fixture'/updates.MANAGER
        self.source.parent.mkdir(parents=True);self.source.write_bytes(b'new verified Manager')
        self.target=self.root/updates.MANAGER;self.target.write_bytes(b'current Manager')
        self.value={'schema_version':1,'source':str(self.source),'target':str(self.target),
                    'workspace':str(self.workspace.root),'parent_pid':os.getpid(),
                    'source_sha256':updates.digest(self.source.read_bytes()),
                    'previous_sha256':updates.digest(self.target.read_bytes())}
        self.job=self.workspace.root/'updates/restart.json'
        self.addCleanup(self.workspace.close)

    def write(self,value=None):storage.atomic(self.job,storage.json_bytes(self.value if value is None else value))

    def test_only_verified_update_shutdown_requests_a_service_handoff(self):
        self.write();self.assertTrue(pending(self.workspace.root))
        self.workspace.close()
        self.assertEqual(self.controller.handoffs,1)
        self.assertFalse(self.controller.running)
        self.assertTrue(self.workspace.services.desired('fixture.wallet'))

    def test_regular_exit_does_not_preserve_a_wallet_readiness_lease(self):
        self.workspace.close()
        self.assertEqual(self.controller.handoffs,0)

    def test_tampered_or_foreign_jobs_never_enable_handoff(self):
        for key,value in [('parent_pid',os.getpid()+1),('source_sha256','f'*64),
                          ('previous_sha256','a'*64),('workspace',str(self.root))]:
            with self.subTest(key=key):
                self.write({**self.value,key:value})
                self.assertFalse(pending(self.workspace.root))
        self.write([]);self.assertFalse(pending(self.workspace.root))
        self.write();self.source.write_bytes(b'tampered')
        self.workspace.close();self.assertEqual(self.controller.handoffs,0)


if __name__=='__main__':unittest.main()
