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
from contextlib import contextmanager
import unittest
from unittest.mock import patch,Mock
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

    def test_replayed_completed_intent_does_not_need_deleted_request_or_spawn_again(self):
        first=self.run_launch()
        with patch.object(launch.subprocess,'Popen')as spawned:
            result=launch.run(self.controller,self.host,self.token)
        self.assertEqual(result['pid'],first['pid']);spawned.assert_not_called()

    def test_completion_from_another_backend_is_rechecked_before_grant(self):
        @contextmanager
        def another_backend_finished(key):
            launch.save(self.game,self.token,status='launched',pid=654)
            yield
        with patch.object(launch,'creation',return_value=None),patch.object(launch,'lease',another_backend_finished),patch.object(launch.subprocess,'Popen')as spawned:
            result=launch.run(self.controller,self.host,self.token)
        self.assertEqual(result['pid'],654);spawned.assert_not_called()

    def test_consumer_failure_is_returned_to_accepted_launch(self):
        import time
        self.workspace.host=lambda identity:self.host;self.workspace.services=SimpleNamespace(desired=lambda identity:False)
        broken=SimpleNamespace(acquire=Mock(side_effect=RuntimeError('consumer fixture failure')),locked=lambda:False)
        with patch.object(self.controller,'busy',broken),patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')):
            try:
                self.controller.launch(self.game,self.token);deadline=time.monotonic()+3
                while launch.state(self.game,self.token).get('status')!='error'and time.monotonic()<deadline:time.sleep(.01)
                result=launch.state(self.game,self.token)
                self.assertEqual(result['status'],'error');self.assertIn('consumer fixture failure',result['message'])
                self.assertEqual(self.controller.worker_error['error_type'],'RuntimeError')
            finally:self.controller.stop()

    def test_periodic_sync_yields_after_git_releases_its_atomic_files(self):
        completed=[];self.controller.thread=threading.current_thread();self.controller.yield_requested=True
        def command(*args,**kwargs):
            self.assertFalse(kwargs['stop'].is_set(),'periodic yield must not kill Git with shallow/index locks held')
            completed.append(True);return SimpleNamespace(returncode=0,stdout=b'',stderr=b'')
        try:
            with patch('manager.owned_process.run',side_effect=command):
                with self.assertRaises(InterruptedError):client_runtime.ClientRuntime.git(self.controller,self.game.parent/'git-fixture',['fetch','origin'])
            self.assertEqual(completed,[True]);self.assertFalse(self.controller.operation_context.in_git)
        finally:self.controller.thread=None;self.controller.yield_requested=False

    def test_startup_failure_keeps_a_token_bound_backend_error(self):
        from tools import mod_manager
        with patch.object(mod_manager,'serve',side_effect=ValueError('startup fixture failed')),patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')):
            code=mod_manager.main(['--workspace',str(self.game/'sdgo/local-state/manager'),'--no-browser',
                '--startup-client',str(self.game),'--startup-request',self.token])
        self.assertEqual(code,1)
        error=storage.read_json(launch.item(self.game,self.token,'server-error.json'))
        self.assertEqual(error['token'],self.token);self.assertEqual(error['stage'],'manager_startup')
        self.assertEqual(error['message'],'startup fixture failed')
        self.assertFalse(launch.item(self.game,self.token,'result.json').exists())

    def test_launch_arriving_while_consumer_stops_is_consumed_by_successor(self):
        import time
        self.workspace.host=lambda identity:self.host;self.workspace.save_workspace=lambda:None
        self.workspace.services=SimpleNamespace(desired=lambda identity:False)
        waiting=threading.Event();exiting=threading.Event();resume=threading.Event();original_wait=self.controller.stop_event.wait
        def paused_wait(timeout):
            waiting.set();value=original_wait(timeout)
            if self.controller.stop_event.is_set():exiting.set();self.assertTrue(resume.wait(5))
            return value
        with patch.object(self.controller.stop_event,'wait',side_effect=paused_wait),patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')),patch.object(launch,'run',return_value={'status':'launched','pid':789}):
            stopper=None
            try:
                self.controller.start();self.assertTrue(waiting.wait(3))
                stopper=threading.Thread(target=self.controller.stop);stopper.start();self.assertTrue(exiting.wait(3))
                response=self.controller.launch(self.game,self.token);self.assertEqual(response['status'],'queued')
                resume.set();stopper.join(3);deadline=time.monotonic()+3
                while launch.state(self.game,self.token).get('status')!='launched'and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(launch.state(self.game,self.token).get('status'),'launched','accepted launch must survive the exiting consumer')
            finally:
                resume.set()
                if stopper:stopper.join(3)
                self.controller.stop()

    def test_other_client_background_sync_cannot_hold_an_accepted_launch(self):
        import time
        other_root=self.game.parent/'other-client';other_root.mkdir()
        storage.atomic(other_root/'sdgo/client-source.json',storage.json_bytes(self.source))
        other={'id':'other','path':str(other_root),'role':'client','name':'other'}
        self.workspace.settings['hosts'].insert(0,other)
        self.workspace.host=lambda identity:next(h for h in self.workspace.settings['hosts']if h['id']==identity)
        self.workspace.save_workspace=lambda:None;self.workspace.services=SimpleNamespace(desired=lambda identity:True)
        entered=threading.Event();resume=threading.Event()
        def plan(host):
            if host['id']=='other':
                entered.set()
                while not self.controller.operation_stop.is_set()and not resume.wait(.01):pass
                if self.controller.operation_stop.is_set():raise InterruptedError('background sync yielded')
            return {'status':'current'}
        with patch.object(self.controller,'plan',side_effect=plan),patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')),patch.object(launch,'run',return_value={'status':'launched','pid':789}):
            try:
                self.controller.start();self.assertTrue(entered.wait(3))
                self.controller.launch(self.game,self.token);deadline=time.monotonic()+3
                while launch.state(self.game,self.token).get('status')!='launched'and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(launch.state(self.game,self.token).get('status'),'launched','another directory periodic sync must yield without changing its source')
            finally:resume.set();self.controller.stop()

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

    def test_next_launch_is_not_lost_while_a_background_check_is_working(self):
        import time
        self.workspace.host=lambda identity:self.host
        self.workspace.save_workspace=lambda:None
        self.workspace.services=SimpleNamespace(desired=lambda identity:False)
        checking=threading.Event();resume=threading.Event()
        def plan(host):
            checking.set();self.assertTrue(resume.wait(5));return {'status':'current'}
        with patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')),patch.object(launch,'run',return_value={'status':'launched','pid':789}),patch.object(self.controller,'plan',side_effect=plan):
            try:
                self.controller.launch(self.game,self.token)
                deadline=time.monotonic()+3
                while launch.state(self.game,self.token).get('status')!='launched'and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(launch.state(self.game,self.token)['status'],'launched')
                self.controller.request(self.host['id'],'check');self.assertTrue(checking.wait(3))
                self.token='d'*32;self.request()
                response=self.controller.launch(self.game,self.token)
                self.assertEqual(response['token'],self.token,'a new click must not reuse a finished launch token')
                resume.set();deadline=time.monotonic()+3
                while launch.state(self.game,self.token).get('status')!='launched'and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(launch.state(self.game,self.token)['status'],'launched')
            finally:resume.set();self.controller.stop()

    def test_cancelling_launch_queued_behind_background_check_is_acknowledged(self):
        import time
        self.workspace.host=lambda identity:self.host;self.workspace.save_workspace=lambda:None
        self.workspace.services=SimpleNamespace(desired=lambda identity:False)
        checking=threading.Event();resume=threading.Event()
        def plan(host):checking.set();self.assertTrue(resume.wait(5));return {'status':'current'}
        with patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')),patch.object(launch,'run')as run,patch.object(self.controller,'plan',side_effect=plan):
            try:
                self.controller.request(self.host['id'],'check');self.assertTrue(checking.wait(3))
                self.controller.launch(self.game,self.token);self.controller.cancel(self.host['id']);resume.set()
                deadline=time.monotonic()+3
                while launch.state(self.game,self.token).get('status')!='cancelled'and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(launch.state(self.game,self.token)['status'],'cancelled');run.assert_not_called()
            finally:resume.set();self.controller.stop()

    def test_failed_launch_can_be_cancelled_after_a_later_background_check(self):
        import time
        self.workspace.host=lambda identity:self.host;self.workspace.save_workspace=lambda:None
        self.workspace.services=SimpleNamespace(desired=lambda identity:False)
        with patch.object(launch,'creation',return_value=(456,self.game/'GOnline.exe')),patch.object(launch,'run',side_effect=ValueError('fixture start error')),patch.object(self.controller,'plan',return_value={'status':'current'}):
            try:
                self.controller.launch(self.game,self.token);deadline=time.monotonic()+3
                while launch.state(self.game,self.token).get('status')!='error'and time.monotonic()<deadline:time.sleep(.01)
                self.controller.request(self.host['id'],'check');deadline=time.monotonic()+3
                while self.controller.results[self.host['id']]['status']!='current'and time.monotonic()<deadline:time.sleep(.01)
                self.controller.cancel(self.host['id'],self.token)
                self.assertEqual(launch.state(self.game,self.token)['status'],'cancelled')
            finally:self.controller.stop()


class LaunchSession(unittest.TestCase):
    def test_manager_session_replacement_resubmits_the_same_intent(self):
        from manager.launch_ui import Session
        with tempfile.TemporaryDirectory()as temporary:
            session=Session(Path(temporary),'a'*32);session.url='http://127.0.0.1:1111/';session.backend_pid=111
            old=session.url;new='http://127.0.0.1:2222/';posted=[]
            storage.atomic(session.workspace/'manager-session.json',storage.json_bytes({'url':old,'pid':111}))
            def connect():session.url=new;session.backend_pid=222
            def api(path,body=None):
                if body is not None:
                    posted.append((session.url,body['token']));return {'status':'queued','host':'client','token':session.token}
                if session.url==old:
                    storage.atomic(session.workspace/'manager-session.json',storage.json_bytes({'url':new,'pid':222}))
                    return {'status':'queued','host':'client','token':session.token}
                return {'status':'launched','pid':789}
            with patch.object(session,'api',side_effect=api),patch.object(session,'connect',side_effect=connect):
                worker=threading.Thread(target=session.work);worker.start();worker.join(3)
                try:
                    self.assertFalse(worker.is_alive());self.assertEqual(posted,[(old,session.token),(new,session.token)])
                    self.assertEqual(session.result['status'],'launched')
                    receipt=storage.read_json(launch.item(session.root,session.token,'session.json'))
                    self.assertEqual(receipt['backend_pid'],222);self.assertEqual(receipt['backend_url'],new)
                finally:session.stop.set();worker.join(3)

    def test_unknown_delivery_is_retried_once_without_a_new_token(self):
        from manager.launch_ui import Session
        with tempfile.TemporaryDirectory()as temporary:
            session=Session(Path(temporary),'a'*32);session.url='http://127.0.0.1:1/';posts=[]
            def api(path,body=None):
                if body is not None:posts.append(body['token']);return {'status':'queued','host':'client','token':session.token}
                return {'status':'unknown'}if len(posts)<2 else{'status':'launched','pid':789}
            with patch.object(session,'api',side_effect=api):
                worker=threading.Thread(target=session.work);worker.start();worker.join(3)
                try:self.assertFalse(worker.is_alive());self.assertEqual(posts,[session.token,session.token]);self.assertEqual(session.result['status'],'launched')
                finally:session.stop.set();worker.join(3)

    def test_later_replacement_session_recovers_after_transport_failure(self):
        from manager.launch_ui import Session
        import time
        with tempfile.TemporaryDirectory()as temporary:
            session=Session(Path(temporary),'a'*32);session.url='http://127.0.0.1:1111/';session.backend_pid=111
            old=session.url;new='http://127.0.0.1:2222/';posts=[]
            storage.atomic(session.workspace/'manager-session.json',storage.json_bytes({'url':old,'pid':111}))
            def connect():session.url=new;session.backend_pid=222
            def api(path,body=None):
                if body is not None:posts.append(session.url);return {'status':'queued','token':session.token,'host':'client'}
                if session.url==old:raise OSError('old backend exited')
                return {'status':'launched','pid':789}
            with patch.object(session,'connect',side_effect=connect),patch.object(session,'api',side_effect=api):
                worker=threading.Thread(target=session.work);worker.start()
                try:
                    deadline=time.monotonic()+3
                    while session.result.get('status')!='error'and time.monotonic()<deadline:time.sleep(.01)
                    self.assertEqual(session.result['status'],'error')
                    storage.atomic(session.workspace/'manager-session.json',storage.json_bytes({'url':new,'pid':222}))
                    worker.join(3);self.assertFalse(worker.is_alive());self.assertEqual(posts,[old,new])
                    self.assertEqual(session.result['status'],'launched')
                finally:session.stop.set();worker.join(3)
    def test_a_new_executable_does_not_silently_reuse_an_old_backend(self):
        from manager.launch_ui import Session,IncompatibleSession
        with tempfile.TemporaryDirectory()as temporary:
            session=Session(Path(temporary),'a'*32)
            lease=SimpleNamespace(file=None,close=lambda:None,existing_url=lambda:'http://127.0.0.1:1/')
            with patch('manager.launch_ui.Lease',return_value=lease),patch.object(session,'api',return_value={'launch_gate':1,'session_token':'fixture'}):
                with self.assertRaisesRegex(IncompatibleSession,'后台尚未更新'):session.connect()

    def test_backend_startup_error_is_reported_without_waiting_for_session_timeout(self):
        from manager.launch_ui import Session,ManagerStartupError
        session=None
        with tempfile.TemporaryDirectory()as temporary:
            session=Session(Path(temporary),'a'*32)
            class FakeLease:
                file=True
                def __init__(self,*args):pass
                def close(self):self.file=None
                def existing_url(self):raise ValueError('no session')
            def spawn(*args,**kwargs):
                self.assertIn('--startup-request',args[0])
                storage.atomic(launch.item(session.root,session.token,'server-error.json'),storage.json_bytes({
                    'token':session.token,'manager_pid':789,'message':'invalid workspace'}))
                return SimpleNamespace(pid=789,poll=lambda:1)
            with patch('manager.launch_ui.Lease',FakeLease),patch('manager.launch_ui.subprocess.Popen',side_effect=spawn):
                with self.assertRaisesRegex(ManagerStartupError,'invalid workspace'):session.connect()

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
                self.assertEqual(identity['launch_service_revision'],launch.SERVICE_REVISION)
            finally:server.shutdown();thread.join(timeout=5);server.server_close()


if __name__=='__main__':unittest.main()
