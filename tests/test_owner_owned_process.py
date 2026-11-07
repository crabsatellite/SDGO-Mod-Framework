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
    def test_quiet_transfer_can_be_observed_and_cancelled_without_stderr(self):
        stop=threading.Event();observed=[]
        def poll():observed.append(True);stop.set()
        with self.assertRaises(InterruptedError):
            run([sys.executable,'-c','import time;time.sleep(30)'],cwd=Path.cwd(),env=dict(os.environ),timeout=None,stop=stop,progress=lambda _:None,poll=poll)
        self.assertTrue(observed)
    def test_transfer_without_deadline_streams_progress_and_remains_cancellable(self):
        stop=threading.Event();seen=[]
        def progress(raw):
            if raw:seen.append(raw);stop.set()
        timer=threading.Timer(15,stop.set);timer.start()
        try:
            code='import sys,time;sys.stderr.buffer.write(b"receiving chunk");sys.stderr.flush();time.sleep(30)'
            with self.assertRaises(InterruptedError):run([sys.executable,'-c',code],cwd=Path.cwd(),env=dict(os.environ),timeout=None,stop=stop,progress=progress)
            self.assertIn(b'receiving chunk',b''.join(seen))
        finally:timer.cancel()
    def test_streamed_binary_output_and_stdin_survive_communicate_timeouts(self):
        with tempfile.TemporaryDirectory()as d:
            root=Path(d);output=root/'raw.blob';data=bytes(range(256))*8192
            code='import sys,time;data=sys.stdin.buffer.read();time.sleep(.5);sys.stdout.buffer.write(data);sys.stderr.buffer.write(b"done")'
            r=run([sys.executable,'-c',code],cwd=root,env=dict(os.environ),timeout=10,input=data,output=output)
            self.assertEqual(r.returncode,0);self.assertIsNone(r.stdout)
            self.assertEqual(r.stderr,b'done');self.assertEqual(output.read_bytes(),data)

    def test_success_returns_original_output(self):
        r=run([sys.executable,'-c','print("owned command")'],cwd=Path.cwd(),env=dict(os.environ),timeout=5)
        self.assertEqual(r.returncode,0);self.assertIn(b'owned command',r.stdout)

    def test_timeout_closes_the_descendant_tree_and_its_pipe_handles(self):
        with tempfile.TemporaryDirectory()as d:
            root=Path(d);beat=root/'heartbeat'
            child=root/'child.py';child.write_text('import time\nfrom pathlib import Path\np=Path("heartbeat")\nwhile True:\n p.write_text(str(time.monotonic()))\n time.sleep(.03)\n')
            code='import subprocess,sys,time; subprocess.Popen([sys.executable,"child.py"]); time.sleep(30)'
            start=time.monotonic()
            # Two real interpreters must start before this can exercise tree
            # cleanup. Keep a separate, strict bound on the cleanup overhead.
            timeout=5
            with self.assertRaises(subprocess.TimeoutExpired):run([sys.executable,'-c',code],cwd=root,env=dict(os.environ),timeout=timeout)
            self.assertLess(time.monotonic()-start,timeout+3);self.assertTrue(beat.exists())
            before=beat.read_bytes();time.sleep(.2);self.assertEqual(beat.read_bytes(),before)

    def test_manager_stop_cancels_owned_command(self):
        stop=threading.Event();timer=threading.Timer(.5,stop.set);timer.start()
        try:
            start=time.monotonic()
            with self.assertRaises(InterruptedError):run([sys.executable,'-c','import time;time.sleep(30)'],cwd=Path.cwd(),env=dict(os.environ),timeout=20,stop=stop)
            self.assertLess(time.monotonic()-start,4)
        finally:timer.cancel()


if __name__=='__main__':unittest.main()
