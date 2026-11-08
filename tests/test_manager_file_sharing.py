"""Actual Windows sharing violations on owned temporary metadata files."""
from pathlib import Path
import ctypes as c
from ctypes import wintypes as w
import os
import tempfile
import threading
import unittest
from manager import storage


@unittest.skipUnless(os.name=='nt','Windows file sharing')
class FileSharing(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.path=Path(temp.name)/'result.json';storage.atomic(self.path,storage.json_bytes({'status':'working'}))
        self.k=c.WinDLL('kernel32',use_last_error=True)
        self.k.CreateFileW.argtypes=(w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE);self.k.CreateFileW.restype=w.HANDLE
        self.k.CloseHandle.argtypes=(w.HANDLE,)

    def lock(self,share):
        handle=self.k.CreateFileW(str(self.path),0x80000000,share,None,3,0x80,None)
        self.assertNotEqual(handle,c.c_void_p(-1).value)
        return handle

    def release_later(self,handle):
        def release():
            threading.Event().wait(.15);self.k.CloseHandle(handle)
        thread=threading.Thread(target=release);thread.start();self.addCleanup(thread.join)

    def test_reader_waits_for_a_short_exclusive_writer(self):
        self.release_later(self.lock(0))
        self.assertEqual(storage.read_json(self.path),{'status':'working'})

    def test_atomic_replace_waits_for_reader_then_installs_complete_json(self):
        self.release_later(self.lock(1))
        storage.atomic(self.path,storage.json_bytes({'status':'launched','pid':17}))
        self.assertEqual(storage.read_json(self.path),{'status':'launched','pid':17})
        self.assertEqual(list(self.path.parent.glob('*.tmp')),[])

    def test_persistent_lock_still_fails_without_changing_existing_json(self):
        handle=self.lock(1)
        try:
            with self.assertRaises(PermissionError):storage.atomic(self.path,storage.json_bytes({'status':'launched'}))
            self.assertEqual(storage.read_json(self.path),{'status':'working'})
        finally:self.k.CloseHandle(handle)


if __name__=='__main__':unittest.main()
