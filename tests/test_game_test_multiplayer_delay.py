from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import game_test
import game_test_multiplayer as cohort


class MissingProcess(Exception):pass


class Process:
    pid=123
    def __init__(self):self.image=str(Path('owned-client/GOnline.exe').resolve());self.created=7;self.suspended=False;self.resumes=0
    def exe(self):return self.image
    def create_time(self):return self.created
    def suspend(self):self.suspended=True
    def resume(self):self.suspended=False;self.resumes+=1


class Actor:
    metadata={'client':{'game':str(Path('owned-client/GOnline.exe').resolve())}}
    def native_events(self):return [{'pid':123}]


class DelayOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.process=Process()
        self.psutil=types.SimpleNamespace(Process=lambda _:self.process,NoSuchProcess=MissingProcess)
        self.mock=patch.dict(sys.modules,{'psutil':self.psutil});self.mock.start();self.addCleanup(self.mock.stop)

    def test_foreign_image_is_never_suspended(self):
        self.process.image=str(Path('another-client/GOnline.exe').resolve())
        with self.assertRaises(game_test.GameTestError):
            with cohort.pause_owned_actor(Actor()):pass
        self.assertFalse(self.process.suspended)
        self.assertEqual(self.process.resumes,0)

    def test_host_failure_releases_the_owned_guest(self):
        with self.assertRaisesRegex(RuntimeError,'host failed'):
            with cohort.pause_owned_actor(Actor()):
                self.assertTrue(self.process.suspended)
                raise RuntimeError('host failed')
        self.assertFalse(self.process.suspended)
        self.assertEqual(self.process.resumes,1)

    def test_pid_reuse_does_not_resume_a_different_process(self):
        with self.assertRaises(game_test.GameTestError):
            with cohort.pause_owned_actor(Actor()):self.process.created=8
        self.assertEqual(self.process.resumes,0)

    def test_unbound_or_multiple_native_pids_are_rejected(self):
        actor=Actor()
        for values in ([],[{'pid':123},{'pid':124}]):
            actor.native_events=lambda:values
            with self.assertRaises(game_test.GameTestError):cohort.owned_actor_process(actor)
        self.assertFalse(self.process.suspended)


if __name__=='__main__':unittest.main()
