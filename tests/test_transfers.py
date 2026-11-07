from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import hashlib,socket,tempfile,threading,time,unittest
from manager.transfers import download


class Transfers(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.target=Path(self.temp.name)/'content.bin';self.data=bytes(range(256))*4096
        self.entry={'bytes':len(self.data),'sha256':hashlib.sha256(self.data).hexdigest()};self.ranges=[]
        case=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*_):pass
            def do_GET(self):
                header=self.headers.get('Range');case.ranges.append((self.path,header))
                start=int(header[6:-1])if header else 0
                self.send_response(206 if start else 200)
                if start:self.send_header('Content-Range',f'bytes {start}-{len(case.data)-1}/{len(case.data)}')
                self.send_header('Content-Length',str(len(case.data)-start));self.end_headers()
                raw=case.data[start:]
                if self.path=='/corrupt':raw=b'X'+raw[1:]
                if self.path=='/broken':
                    self.wfile.write(raw[:16384]);self.wfile.flush();self.close_connection=True;return
                try:
                    for index in range(0,len(raw),16384):
                        self.wfile.write(raw[index:index+16384]);self.wfile.flush()
                        if self.path=='/slow':time.sleep(.01)
                except(ConnectionError,OSError):pass
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.addCleanup(self.server.server_close);self.addCleanup(self.server.shutdown)
        threading.Thread(target=self.server.serve_forever,daemon=True).start()
        self.url='http://127.0.0.1:'+str(self.server.server_port)

    def test_failed_source_resumes_identical_file_from_backup_with_range(self):
        with self.assertRaises(OSError):download(self.url+'/broken',self.target,self.entry)
        self.assertFalse(self.target.exists());partial=self.target.with_name('content.bin.part')
        self.assertEqual(partial.stat().st_size,16384)
        download(self.url+'/good',self.target,self.entry)
        self.assertEqual(self.ranges[-1],('/good','bytes=16384-'))
        self.assertEqual(self.target.read_bytes(),self.data);self.assertFalse(partial.exists())

    def test_slow_progress_is_reported_and_cancelled_download_continues(self):
        stop=threading.Event();seen=[]
        def progress(size):
            seen.append(size)
            if size>=32768:stop.set()
        with self.assertRaises(InterruptedError):download(self.url+'/slow',self.target,self.entry,stop,progress)
        self.assertGreater(len(set(seen)),1);self.assertFalse(self.target.exists())
        offset=self.target.with_name('content.bin.part').stat().st_size
        download(self.url+'/good',self.target,self.entry)
        self.assertEqual(self.ranges[-1][1],f'bytes={offset}-');self.assertEqual(self.target.read_bytes(),self.data)

    def test_wrong_bytes_never_become_an_installable_file(self):
        with self.assertRaisesRegex(ValueError,'摘要'):download(self.url+'/corrupt',self.target,self.entry)
        self.assertFalse(self.target.exists());self.assertFalse(self.target.with_name('content.bin.part').exists())


if __name__=='__main__':unittest.main()
