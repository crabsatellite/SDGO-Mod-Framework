"""Central service ownership, persistence and restart recovery without game data."""
from pathlib import Path
import tempfile
import time
import unittest
from manager.workspace import Workspace

ROOT=Path(__file__).resolve().parents[1]


class Controller:
    def __init__(self):self.running=False;self.starts=0;self.stops=0
    def state(self):return {'running':self.running,'healthy':self.running,'error':''}
    def start(self):self.starts+=1;self.running=True
    def stop(self):self.stops+=1;self.running=False


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.workspace=Workspace(self.root,ROOT/'mods');self.controller=Controller()
        self.workspace.services.register('fixture.worker','Fixture worker',self.controller,self.controller.start)

    def tearDown(self):self.workspace.services.close();self.temp.cleanup()

    def test_one_owner_and_repeated_start_is_idempotent(self):
        registry=self.workspace.services
        registry.start('fixture.worker');registry.start('fixture.worker')
        self.assertEqual(self.controller.starts,1)
        state=registry.describe('fixture.worker');self.assertTrue(state['desired']);self.assertTrue(state['healthy']);self.assertEqual(state['owner'],'manager')

    def test_shutdown_preserves_desired_state_and_next_manager_restores(self):
        self.workspace.services.start('fixture.worker');self.workspace.services.close()
        self.assertFalse(self.controller.running)
        next_workspace=Workspace(self.root,ROOT/'mods');next_controller=Controller()
        next_workspace.services.register('fixture.worker','Fixture worker',next_controller,next_controller.start)
        try:
            next_workspace.services.restore()
            for _ in range(30):
                if next_controller.running:break
                time.sleep(.05)
            self.assertTrue(next_controller.running);self.assertEqual(next_controller.starts,1)
        finally:next_workspace.services.close()

    def test_explicit_stop_disables_restore(self):
        registry=self.workspace.services;registry.start('fixture.worker');registry.stop('fixture.worker');registry.restore();time.sleep(.1)
        self.assertFalse(self.controller.running);self.assertFalse(registry.desired('fixture.worker'))

    def test_crashed_enabled_service_is_restarted_by_manager(self):
        registry=self.workspace.services;registry.start('fixture.worker');self.controller.running=False;registry.restore()
        for _ in range(30):
            if self.controller.running:break
            time.sleep(.05)
        self.assertTrue(self.controller.running);self.assertEqual(self.controller.starts,2)

    def test_unknown_or_duplicate_registration_is_refused(self):
        with self.assertRaises(ValueError):self.workspace.services.start('missing')
        with self.assertRaises(ValueError):self.workspace.services.register('fixture.worker','Other',self.controller,self.controller.start)

    def test_bad_display_state_does_not_prevent_shutdown_or_other_resources(self):
        class Bad(Controller):
            def state(self):raise ValueError('malformed addon metadata')
        class Resource:
            closed=False
            def close(self):self.closed=True
        bad=Bad();resource=Resource();registry=self.workspace.services
        registry.register('fixture.bad','Bad display',bad,bad.start)
        registry.resource('fixture.resource',lambda:resource)
        self.assertTrue(registry.describe('fixture.bad')['state_error'])
        registry.restore();time.sleep(.1)
        self.assertTrue(registry.thread.is_alive())
        registry.close()
        self.assertGreaterEqual(bad.stops,1)
        self.assertGreaterEqual(self.controller.stops,1)
        self.assertTrue(resource.closed)

    def test_failed_stop_is_recorded_after_other_services_are_closed(self):
        class Bad(Controller):
            def stop(self):raise OSError('fixture close failure')
        bad=Bad();self.workspace.services.register('fixture.bad','Bad close',bad,bad.start)
        self.workspace.services.close()
        self.assertGreaterEqual(self.controller.stops,1)
        self.assertTrue(self.workspace.services.close_errors)
        self.assertIn('fixture.bad',(self.root/'manager-errors.log').read_text(encoding='utf-8'))


if __name__=='__main__':unittest.main()
