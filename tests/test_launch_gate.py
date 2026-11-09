"""Startup uses the real profile planner/installer; only process edges are fake."""
from pathlib import Path
import copy
import hashlib
import os
import subprocess
import sys
import tempfile
import threading
import urllib.request
import json
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from manager import launch,storage,client_runtime
from tests import test_server_profiles as profile_fixtures


class LaunchGate(unittest.TestCase):
    git=profile_fixtures.Profiles.git;write=profile_fixtures.Profiles.write;commit_all=profile_fixtures.Profiles.commit_all

    def setUp(self):
        profile_fixtures.Profiles.setUp(self)
        self.token='c'*32
        # The fixture programme declares the actual loader protocol export.
        name='client/d3d9.dll';raw=b'fixture SdgoLaunchGateProtocol'
        (Path(self.framework['bundle'])/name).write_bytes(raw)
        self.framework['manifest']['files'][name]={'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
        self.request()

    def request(self,command=None):
        root=str(self.game).encode('utf-16-le');command=(command or('"'+str(self.game/'GOnline.exe')+'" --example "中文 参数"')).encode('utf-16-le')
        storage.atomic(launch.item(self.game,self.token,'request'),launch.REQUEST.pack(b'SDGOLQ01',123,0,456,len(root)//2,len(command)//2)+root+command)

    def spawn(self,command,**kwargs):
        self.assertEqual(launch.read_request(self.game,self.token)['command'],command)
        self.assertEqual(kwargs['executable'],str(self.game/'GOnline.exe'))
        self.assertEqual(kwargs['env']['SDGO_LAUNCH_PERMIT'],self.token)
        self.assertIn('RunAsInvoker',kwargs['env']['__COMPAT_LAYER'].split())
        raw=launch.item(self.game,self.token,'permit').read_bytes();magic,expiry,receipt,source,n=launch.PERMIT.unpack_from(raw)
        self.assertEqual(magic,b'SDGOLP01');self.assertEqual(receipt,hashlib.sha256((self.game/client_runtime.RECEIPT).read_bytes()).digest())
        self.assertEqual(source,hashlib.sha256((self.game/client_runtime.SOURCE).read_bytes()).digest())
        self.assertEqual(raw[launch.PERMIT.size:].decode('utf-16-le'),str(self.game))
        launch.item(self.game,self.token,'permit').unlink()
        storage.atomic(launch.item(self.game,self.token,'ack'),launch.PROTOCOL)
        return SimpleNamespace(pid=789,poll=lambda:None)

    def run_launch(self):
        with patch.object(launch,'creation',return_value=None),patch.object(launch.subprocess,'Popen',side_effect=self.spawn)as spawned:
            result=launch.run(self.controller,self.host,self.token)
        self.assertEqual(result['status'],'launched');self.assertEqual(spawned.call_count,1)
        self.assertFalse(launch.item(self.game,self.token,'request').exists())
        return result

    def test_profile_install_remote_recheck_and_unchanged_arguments(self):
        with patch('manager.server_profiles.resolve',wraps=__import__('manager.server_profiles',fromlist=['resolve']).resolve)as resolve:
            self.run_launch();self.assertGreaterEqual(resolve.call_count,2)
        self.assertEqual((self.game/'sprs/new-unit.spr').read_bytes(),b'new custom unit')

    def test_original_handoff_keeps_graphics_compatibility_without_requesting_elevation(self):
        environment={'__COMPAT_LAYER':'HIGHDPIAWARE RunAsAdmin Win7RTM','SDGO_LAUNCH_PERMIT':self.token}
        original=environment.copy()
        with patch.object(launch.subprocess,'Popen',return_value=object())as spawned:
            launch.start_original('original arguments',self.game,environment)
        self.assertEqual(environment,original)
        self.assertEqual(spawned.call_args.kwargs['env']['__COMPAT_LAYER'],'HIGHDPIAWARE Win7RTM RunAsInvoker')
        self.assertEqual(spawned.call_args.kwargs['env']['SDGO_LAUNCH_PERMIT'],self.token)

    def test_correct_snapshot_without_receipt_gets_verified_receipt_before_launch(self):
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        (self.game/client_runtime.RECEIPT).unlink()
        self.run_launch();self.assertTrue((self.game/client_runtime.RECEIPT).is_file())

    def test_remote_changes_during_install_are_followed_before_launch(self):
        real_install=self.controller.install;calls=[]
        def install(*args):
            result=real_install(*args);calls.append(result)
            if len(calls)==1:
                self.profile['revision']=2
                (self.repo/'server-profile.json').write_bytes(storage.json_bytes(self.profile))
            return result
        with patch.object(self.controller,'install',side_effect=install):self.run_launch()
        self.assertEqual(len(calls),2)
        self.assertEqual(storage.read_json(self.game/client_runtime.RECEIPT)['profile']['revision'],2)

    def test_failure_and_cancel_never_start_game(self):
        with patch.object(launch,'creation',return_value=None),patch.object(launch.subprocess,'Popen')as spawned:
            with patch.object(self.controller,'plan',side_effect=OSError('network failed')):
                with self.assertRaises(OSError):launch.run(self.controller,self.host,self.token)
            self.controller.cancel_event.set()
            with self.assertRaises(InterruptedError):launch.run(self.controller,self.host,self.token)
            spawned.assert_not_called()

    def test_source_changed_during_final_recheck_blocks_launch(self):
        from manager import server_profiles
        real=server_profiles.resolve;calls=[]
        def resolve(*args):
            result=real(*args);calls.append(1)
            if len(calls)==2:
                changed=copy.deepcopy(self.source);changed['label']='Changed binding'
                (self.game/client_runtime.SOURCE).write_bytes(storage.json_bytes(changed))
            return result
        with patch.object(launch,'creation',return_value=None),patch.object(launch.subprocess,'Popen')as spawned,patch.object(server_profiles,'resolve',side_effect=resolve):
            with self.assertRaisesRegex(ValueError,'更新源'):launch.run(self.controller,self.host,self.token)
            spawned.assert_not_called()

    def test_mapped_game_is_waited_for_without_termination(self):
        states=iter([True,True,False,False,False,False])
        with patch('manager.updates.running',side_effect=lambda *args:next(states,False)):
            self.run_launch()

    def test_unknown_loader_cannot_silently_lose_future_checks(self):
        name='client/d3d9.dll';raw=b'old loader'
        (Path(self.framework['bundle'])/name).write_bytes(raw);self.framework['manifest']['files'][name]={'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
        with patch.object(launch,'creation',return_value=None),patch.object(launch.subprocess,'Popen')as spawned:
            with self.assertRaisesRegex(ValueError,'尚不支持启动前校验'):launch.run(self.controller,self.host,self.token)
            spawned.assert_not_called()

    def test_request_root_command_and_process_identity_are_checked(self):
        with patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')):
            launch.accept(self.game,self.token)
        self.request('"'+str(self.game/'GOnline.exe')+'" --changed')
        with self.assertRaisesRegex(ValueError,'被修改'):launch.accept(self.game,self.token)
        with self.assertRaises(ValueError):launch.item(self.game,'../bad','request')
        self.request('"C:\\Windows\\notepad.exe"')
        if os.name=='nt':
            with self.assertRaisesRegex(ValueError,'原版游戏入口'):launch.read_request(self.game,self.token)

    def test_repeated_requests_coalesce_and_idle_cancel_finishes_ui(self):
        self.workspace.host=lambda identity:self.host
        self.workspace.save_workspace=lambda:None
        with patch.object(self.controller,'start'),patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')):
            first=self.controller.launch(self.game,self.token)
            self.token='d'*32;self.request()
            second=self.controller.launch(self.game,self.token)
            self.assertEqual(first['token'],second['token']);self.assertEqual(len(self.controller.jobs),1)
            with self.assertRaisesRegex(ValueError,'正在准备启动游戏'):self.controller.request(self.host['id'],'check')
            self.controller.cancel(self.host['id'])
            state=self.controller.launch_state(self.game,first['token'])
            self.assertEqual(state['status'],'cancelled');self.assertEqual(state['token'],first['token'])


class LaunchSession(unittest.TestCase):
    def test_unexpected_worker_failure_is_persisted_before_waiting_for_retry(self):
        from manager.launch_ui import Session
        import time
        with tempfile.TemporaryDirectory()as temporary:
            session=Session(Path(temporary),'a'*32);session.url='http://127.0.0.1:1/'
            with patch.object(session,'api',side_effect=RuntimeError('cannot submit launch')):
                worker=threading.Thread(target=session.work);worker.start()
                try:
                    receipt=launch.item(session.root,session.token,'session.json');deadline=time.monotonic()+3
                    while storage.read_json(receipt,{}).get('status')!='error'and time.monotonic()<deadline:time.sleep(.01)
                    result=storage.read_json(receipt)
                    self.assertEqual(result['stage'],'submit');self.assertEqual(result['error_type'],'RuntimeError')
                    self.assertEqual(result['message'],'cannot submit launch');self.assertEqual(session.result['status'],'error')
                    self.assertTrue(worker.is_alive(),'the error must leave retry/cancel available')
                    # The UI receipt must never clobber an independent worker result.
                    launch.save(session.root,session.token,status='launched',pid=123)
                    worker.join(2);self.assertFalse(worker.is_alive())
                    self.assertEqual(launch.state(session.root,session.token)['status'],'launched')
                finally:session.stop.set();worker.join(3)

    def test_quoted_original_command_with_trailing_space_is_preserved(self):
        command='"D:\\Game Folder\\GOnline.exe" '
        with patch.object(launch.subprocess,'Popen')as spawned:
            launch.start_original(command,Path('D:/Game Folder'),{})
        self.assertEqual(spawned.call_args.args[0],command)

    def test_local_launch_api_and_session_discovery_ignore_system_proxies(self):
        from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
        from manager.launch_ui import Session
        from manager.instance import Lease
        from manager.server import make_server
        intercepted=[]
        class Proxy(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                intercepted.append(self.path);self.send_error(502)
            do_POST=do_GET
        with tempfile.TemporaryDirectory()as temp:
            root=Path(temp);session=Session(root,'a'*32)
            workspace=SimpleNamespace(root=session.workspace,client_runtime=SimpleNamespace(
                launch=lambda root,token:{'status':'queued','token':token}))
            server=make_server(workspace);proxy=ThreadingHTTPServer(('127.0.0.1',0),Proxy)
            threads=[threading.Thread(target=s.serve_forever,daemon=True)for s in(server,proxy)]
            for thread in threads:thread.start()
            session.url=f'http://127.0.0.1:{server.server_port}/'
            storage.atomic(session.workspace/'manager-session.json',storage.json_bytes({'url':session.url}))
            proxy_url=f'http://127.0.0.1:{proxy.server_port}'
            try:
                with patch.dict(os.environ,{'http_proxy':proxy_url,'HTTP_PROXY':proxy_url,'https_proxy':proxy_url,'HTTPS_PROXY':proxy_url,'no_proxy':'','NO_PROXY':''}),patch.object(urllib.request,'_opener',None):
                    lease=Lease(session.workspace)
                    try:self.assertEqual(lease.existing_url(),session.url)
                    finally:lease.close()
                    identity=session.api('api/identity');session.secret=identity['session_token']
                    self.assertEqual(session.api('api/launch',{'root':str(root),'token':session.token})['status'],'queued')
                    self.assertEqual(intercepted,[],'local launch requests must never reach a proxy')
                    with self.assertRaises(urllib.error.HTTPError)as error:urllib.request.urlopen('http://download.invalid/programme.zip',timeout=3)
                    error.exception.close()
                self.assertEqual(intercepted,['http://download.invalid/programme.zip'],'remote downloads retain the configured proxy')
            finally:
                for s in(server,proxy):s.shutdown();s.server_close()
                for thread in threads:thread.join(3)

    def test_a_late_terminal_receipt_finishes_ui_after_a_transient_session_error(self):
        from manager.launch_ui import Session
        import time
        with tempfile.TemporaryDirectory()as temporary:
            session=Session(Path(temporary),'a'*32);session.url='http://127.0.0.1:1/'
            failed=threading.Event()
            def api(*args):failed.set();raise ValueError('temporary restart transport error')
            with patch.object(session,'api',side_effect=api):
                worker=threading.Thread(target=session.work);worker.start()
                try:
                    self.assertTrue(failed.wait(3))
                    deadline=time.monotonic()+3
                    while session.result.get('status')!='error'and time.monotonic()<deadline:time.sleep(.01)
                    self.assertEqual(session.result.get('status'),'error')
                    launch.save(session.root,session.token,status='launched',pid=123)
                    worker.join(2)
                    self.assertFalse(worker.is_alive(),'completed launch must not wait for a retry click')
                    self.assertEqual(session.result['status'],'launched')
                finally:session.stop.set();worker.join(3)

    @unittest.skipUnless(os.name=='nt','Windows helper copy mutex')
    def test_concurrent_helper_copies_share_one_complete_file(self):
        import time
        with tempfile.TemporaryDirectory()as temp:
            root=Path(temp);source=root/'source.exe';source.write_bytes(os.urandom(1024*1024));target=root/'cache/Manager.exe'
            script='from pathlib import Path;import sys,time;from manager.launch_ui import cache_manager;r=Path(sys.argv[1]);(r/(sys.argv[2]+".ready")).touch();\nwhile not(r/"start").exists():time.sleep(.01)\ncache_manager(r/"cache/Manager.exe",(r/"source.exe").read_bytes())'
            processes=[subprocess.Popen([sys.executable,'-c',script,str(root),str(i)])for i in range(4)]
            try:
                deadline=time.monotonic()+15
                while len(list(root.glob('*.ready')))<4 and time.monotonic()<deadline:time.sleep(.02)
                self.assertEqual(len(list(root.glob('*.ready'))),4);(root/'start').touch()
                self.assertEqual([p.wait(timeout=15)for p in processes],[0]*4)
                self.assertEqual(source.read_bytes(),target.read_bytes())
            finally:
                for p in processes:
                    if p.poll()is None:p.kill();p.wait(timeout=10)

    @unittest.skipUnless(os.name=='nt','Windows process identity')
    def test_exited_process_with_a_held_handle_is_not_an_origin(self):
        process=subprocess.Popen([sys.executable,'-c','pass']);process.wait(timeout=10)
        self.assertIsNone(launch.creation(process.pid))

    def test_startup_identity_does_not_depend_on_parsing_installed_mods(self):
        from manager.server import make_server
        with tempfile.TemporaryDirectory()as temp:
            workspace=SimpleNamespace(root=Path(temp),public=lambda:(_ for _ in()).throw(ValueError('broken local registration')))
            server=make_server(workspace);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{server.server_port}/api/identity')as response:identity=json.load(response)
                self.assertEqual(identity['launch_gate'],1);self.assertTrue(identity['session_token'])
            finally:server.shutdown();thread.join(timeout=5);server.server_close()


if __name__=='__main__':unittest.main()
