import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from manager.owned_process import run


class OwnedCommandTests(unittest.TestCase):
    def test_success_returns_original_output(self):
        r=run([sys.executable,'-c','print("owned command")'],cwd=Path.cwd(),env=dict(os.environ),timeout=5)
        self.assertEqual(r.returncode,0);self.assertIn(b'owned command',r.stdout)

    def test_timeout_closes_the_descendant_tree_and_its_pipe_handles(self):
        with tempfile.TemporaryDirectory()as d:
            root=Path(d);beat=root/'heartbeat'
            child=root/'child.py';child.write_text('import time\nfrom pathlib import Path\np=Path("heartbeat")\nwhile True:\n p.write_text(str(time.monotonic()))\n time.sleep(.03)\n')
            code='import subprocess,sys,time; subprocess.Popen([sys.executable,"child.py"]); time.sleep(30)'
            start=time.monotonic()
            with self.assertRaises(subprocess.TimeoutExpired):run([sys.executable,'-c',code],cwd=root,env=dict(os.environ),timeout=1)
            self.assertLess(time.monotonic()-start,4);self.assertTrue(beat.exists())
            before=beat.read_bytes();time.sleep(.2);self.assertEqual(beat.read_bytes(),before)

    def test_manager_stop_cancels_owned_command(self):
        stop=threading.Event();timer=threading.Timer(.5,stop.set);timer.start()
        try:
            start=time.monotonic()
            with self.assertRaises(InterruptedError):run([sys.executable,'-c','import time;time.sleep(30)'],cwd=Path.cwd(),env=dict(os.environ),timeout=20,stop=stop)
            self.assertLess(time.monotonic()-start,4)
        finally:timer.cancel()


if __name__=='__main__':unittest.main()
