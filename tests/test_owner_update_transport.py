"""Actual TLS downloads and Git publication on temporary, task-owned endpoints."""
import functools
import http.server
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import threading
import unittest
import urllib.request
from unittest.mock import patch
from manager import owner_channel as o, updates as u
import test_owner_update_chain as fixtures


def openssl():
    candidate=shutil.which('openssl')
    if candidate:return candidate
    candidate=Path(r'C:\Program Files\Git\usr\bin\openssl.exe')
    return str(candidate) if candidate.is_file() else None


def certificate(root):
    key=root/'tls-key.pem';cert=root/'tls-cert.pem'
    subprocess.run([openssl(),'req','-x509','-newkey','rsa:2048','-nodes','-days','1',
        '-subj','/CN=localhost','-addext','subjectAltName=IP:127.0.0.1,DNS:localhost',
        '-keyout',str(key),'-out',str(cert)],check=True,capture_output=True,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    return cert,key


@unittest.skipUnless(openssl() and shutil.which('git'),'TLS/Git transport fixture requires OpenSSL and Git')
class TransportTests(unittest.TestCase):
    def setUp(self):
        self.fixture=fixtures.OwnerChainTests();self.fixture.setUp();self.addCleanup(self.fixture.doCleanups)
        self.root=self.fixture.root;self.workspace=self.fixture.workspace
        self.remote=self.root/'bare.git';subprocess.run(['git','init','--bare',str(self.remote)],check=True,capture_output=True)
        remote=self.remote
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*_):pass
            def do_GET(self):
                name=self.path.removeprefix('/packs/')
                if self.path.startswith('/packs/') and '..' not in name and '?'not in name:
                    p=subprocess.run(['git','--git-dir',str(remote),'show','main:'+name],capture_output=True)
                    if p.returncode==0:
                        self.send_response(200);self.send_header('Content-Length',str(len(p.stdout)));self.end_headers();self.wfile.write(p.stdout);return
                self.send_error(404)
        cert,key=certificate(self.root)
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
        self.server.socket=context.wrap_socket(self.server.socket,server_side=True)
        thread=threading.Thread(target=self.server.serve_forever,daemon=True);thread.start()
        self.addCleanup(lambda:(self.server.shutdown(),self.server.server_close(),thread.join()))
        remote_url='https://owner-git.example/runtime.git'
        self.env={'SSL_CERT_FILE':str(cert),'GIT_CONFIG_COUNT':'4',
            'GIT_CONFIG_KEY_0':'url.'+self.remote.as_uri()+'.insteadOf','GIT_CONFIG_VALUE_0':remote_url,
            'GIT_CONFIG_KEY_1':'user.name','GIT_CONFIG_VALUE_1':'Fixture Owner',
            'GIT_CONFIG_KEY_2':'user.email','GIT_CONFIG_VALUE_2':'fixture@example.invalid',
            'GIT_CONFIG_KEY_3':'protocol.file.allow','GIT_CONFIG_VALUE_3':'always'}
        env=patch.dict(os.environ,self.env);env.start();self.addCleanup(env.stop)
        opener=patch.object(urllib.request,'_opener',None);opener.start();self.addCleanup(opener.stop)
        self.controller=o.OwnerChannels(self.workspace)
        cfg={**self.fixture.source_cfg,'git_remote':remote_url,'feeds':[f'https://127.0.0.1:{self.server.server_port}/packs/owner-feed.json']}
        self.controller.configure(self.fixture.owner['id'],{'mode':'owner','publication':cfg})

    def test_git_push_public_tls_readback_and_headless_player_sync(self):
        owner=self.fixture.owner;player=self.fixture.player
        p=self.controller.preview(owner['id']);result=self.controller.publish(owner['id'],p['digest'])
        self.assertEqual(result['status'],'published')
        pushed=json.loads(subprocess.check_output(['git','--git-dir',str(self.remote),'show','main:owner-feed.json']))
        self.assertEqual(result['sha256'],pushed['sha256'])
        self.controller.configure(player['id'],{'mode':'player','source':o.binding(owner['publication']),'automatic':False})
        from manager import storage
        other=self.root/'other-maintenance';subprocess.run(['git','clone','--branch','main',str(self.remote),str(other)],check=True,capture_output=True)
        (other/'maintenance.txt').write_bytes(b'unrelated concurrent maintenance')
        for command in (['add','maintenance.txt'],['commit','-m','Unrelated maintenance'],['push','origin','main']):
            subprocess.run(['git','-C',str(other),*command],check=True,capture_output=True)
        changed=Path(owner['path'])/'sdgo/mods/example/config.ini';changed.write_bytes(b'[settings]\nAmount=81\n')
        p=self.controller.preview(owner['id']);self.controller.publish(owner['id'],p['digest'])
        process=subprocess.run([__import__('sys').executable,'tools/mod_manager.py','--sync-owner-pack',player['path']],
            cwd=Path(__file__).resolve().parents[1],capture_output=True,timeout=30)
        self.assertEqual(process.returncode,0,process.stderr.decode(errors='replace'))
        self.assertEqual((Path(player['path'])/'sdgo/mods/example/config.ini').read_bytes(),b'[settings]\nAmount=81\n')
        self.assertEqual(storage.read_json(Path(player['path'])/o.RECEIPT)['revision'],2)
        self.assertEqual(subprocess.check_output(['git','--git-dir',str(self.remote),'show','main:maintenance.txt']),b'unrelated concurrent maintenance')
        self.assertEqual(storage.read_json(Path(player['path'])/'sdgo/local-state/owner-updater/result.json')['status'],'installed')

    def test_public_identity_mismatch_never_becomes_a_successful_publish(self):
        p=self.controller.preview(self.fixture.owner['id'])
        result=self.controller.publish(self.fixture.owner['id'],p['digest'])
        self.assertEqual(result['revision'],1)
        self.fixture.owner['publication']['publisher_id']='1'*32
        with self.assertRaises(ValueError):self.controller.verify_publish(self.fixture.owner['id'])


if __name__=='__main__':unittest.main()
