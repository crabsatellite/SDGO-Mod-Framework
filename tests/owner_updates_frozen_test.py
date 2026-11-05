"""Standalone Manager API -> real TLS publication -> standalone player updater."""
import argparse
import functools
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT));sys.path.insert(0,str(ROOT/'tests'))
from test_owner_update_chain import OwnerChainTests
from test_owner_update_transport import certificate
from manager import owner_channel as o, storage


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--exe',type=Path,required=True);args=parser.parse_args()
    f=OwnerChainTests();f.setUp();process=None;https_server=None;thread=None;base=None;token=''
    try:
        cert,key=certificate(f.root)
        class Handler(http.server.SimpleHTTPRequestHandler):
            def log_message(self,*_):pass
        https_server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Handler,directory=str(f.output)))
        tls=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);tls.load_cert_chain(cert,key);https_server.socket=tls.wrap_socket(https_server.socket,server_side=True)
        thread=threading.Thread(target=https_server.serve_forever,daemon=True);thread.start()
        feed=f'https://127.0.0.1:{https_server.server_port}/owner-feed.json'
        f.workspace.settings['services']['manager.updates']={'enabled':False};f.workspace.save_workspace()
        environment=dict(os.environ,SSL_CERT_FILE=str(cert));environment.pop('PYTHONPATH',None)
        process=subprocess.Popen([str(args.exe.resolve()),'--workspace',str(f.workspace.root),'--no-browser'],cwd=f.root,
            env=environment,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        deadline=time.monotonic()+40
        while time.monotonic()<deadline:
            p=f.workspace.root/'manager-session.json'
            if p.exists():
                try:base=json.loads(p.read_text())['url'].rstrip('/');break
                except (OSError,ValueError):pass
            if process.poll()is not None:raise RuntimeError('Standalone Manager exited early')
            time.sleep(.1)
        if not base:raise RuntimeError('Standalone Manager did not expose its session')
        def call(path,body=None):
            req=urllib.request.Request(base+path,None if body is None else json.dumps(body).encode(),
                headers={'Content-Type':'application/json','Origin':base,'X-ModManager-Token':token})
            with urllib.request.urlopen(req,timeout=30)as response:return json.load(response)
        token=call('/api/state')['session_token']
        source_cfg={**f.source_cfg,'feeds':[feed]}
        call('/api/owner-updates',{'action':'configure','host':f.owner['id'],'mode':'owner','publication':source_cfg})
        assert (Path(f.owner['path'])/o.MAINTAINER).is_file()
        with urllib.request.urlopen(base+'/owner_updates.js',timeout=10)as r:assert b'mountOwnerUpdates'in r.read()
        plan=call('/api/owner-updates',{'action':'preview','host':f.owner['id']});assert 'global_scope'in plan
        call('/api/owner-updates',{'action':'publish','host':f.owner['id'],'digest':plan['digest']})
        def wait_publish():
            end=time.monotonic()+30
            while time.monotonic()<end:
                data=call('/api/owner-updates');row=next(h for h in data['hosts']if h['id']==f.owner['id']);result=row['result']
                if result.get('status')=='error':raise RuntimeError(result['message'])
                if result.get('status')=='published':return result
                time.sleep(.1)
            raise RuntimeError('Public TLS readback did not finish')
        assert wait_publish()['revision']==1
        resolved=call('/api/owner-updates',{'action':'resolve','feeds':[feed]})
        call('/api/owner-updates',{'action':'configure','host':f.player['id'],'mode':'player','source':resolved['source'],'automatic':False})
        player=Path(f.player['path']);personal=(player/'config.xml').read_bytes()
        def cli(flag,expected=0):
            p=subprocess.run([str(args.exe.resolve()),flag,str(player)],env=environment,cwd=f.root,
                capture_output=True,timeout=40,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            assert p.returncode==expected,(flag,p.returncode,p.stderr)
        cli('--check-owner-pack',3);assert not (player/'d3d9.dll').exists()
        cli('--sync-owner-pack');assert storage.read_json(player/o.RECEIPT)['revision']==1
        cli('--check-owner-pack');assert (player/'config.xml').read_bytes()==personal
        (Path(f.owner['path'])/'sdgo/mods/example/config.ini').write_bytes(b'[settings]\nAmount=98\n')
        plan=call('/api/owner-updates',{'action':'preview','host':f.owner['id']})
        call('/api/owner-updates',{'action':'publish','host':f.owner['id'],'digest':plan['digest']})
        assert wait_publish()['revision']==2
        cli('--sync-owner-pack');assert (player/'sdgo/mods/example/config.ini').read_bytes()==b'[settings]\nAmount=98\n'
        cli('--check-owner-pack');assert (player/'config.xml').read_bytes()==personal
        print('PASS standalone owner publication, HTTPS identity pinning, two player revisions, read-only audit and personal-state preservation')
        call('/api/exit',{});process.wait(timeout=30)
    finally:
        if process and process.poll()is None:
            if base:
                try:
                    req=urllib.request.Request(base+'/api/exit',data=b'{}',headers={'Content-Type':'application/json','Origin':base,'X-ModManager-Token':token})
                    urllib.request.urlopen(req,timeout=5).close();process.wait(timeout=30)
                except Exception:process.terminate();process.wait(timeout=10)
            else:process.terminate();process.wait(timeout=10)
        if https_server:https_server.shutdown();https_server.server_close()
        if thread:thread.join()
        f.doCleanups()


if __name__=='__main__':main()
