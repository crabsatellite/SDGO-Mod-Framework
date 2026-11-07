"""Complete-runtime updates use synthetic Git blobs and never game/user data."""
from pathlib import Path
import copy,hashlib,io,json,os,subprocess,tempfile,unittest,zipfile
from types import SimpleNamespace
from unittest.mock import patch
from manager import client_runtime as c,git_source,storage,updates


class CompleteRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._runtime_repository_temp=tempfile.TemporaryDirectory(prefix="runtime-repository-fixture-")
        cls.addClassCleanup(cls._runtime_repository_temp.cleanup)
        cls._repo_data=None

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name).resolve()
        self.game=self.root/'game';self.game.mkdir();self.repo=Path(type(self)._runtime_repository_temp.name)/'repo'
        self.binding={'schema_version':1,'kind':'sdgo-client-runtime','source_id':'a'*32,'label':'Example pack',
                      'feeds':['git+ssh://git@example.invalid/pack.git#main','git+https://example.invalid/pack.git#main']}
        self.host={'id':'test','name':'Example','path':str(self.game),'role':'client','runtime_mode':'starter'}
        self.workspace=SimpleNamespace(root=self.root/'manager',settings={'hosts':[self.host]})
        self.controller=c.ClientRuntime(self.workspace)
        self.environment=patch.dict(os.environ,{'LOCALAPPDATA':str(self.root/'local')});self.environment.start();self.addCleanup(self.environment.stop)
        if type(self)._repo_data is None:
            self.repo.mkdir(exist_ok=True)
            self.git('init','-b','main');self.git('config','user.name','Fixture');self.git('config','user.email','fixture@example.invalid')
            archive=io.BytesIO();base=b'synthetic original archive'
            with zipfile.ZipFile(archive,'w')as z:z.writestr('GPackData.zpk',base)
            raw=archive.getvalue();(self.repo/'GPackData.zpk.part1.zip').write_bytes(raw)
            self.git('add','.');self.git('commit','-m','Archive fixture');base_commit=self.git('rev-parse','HEAD').strip()
            self.contract={'schema_version':1,'source_id':'a'*32,'root_extensions':['.exe','.dll','.ini','.zpk'],
                           'entrypoints':[],'trees':['sdgo/mods','sdgo/symbols','sprs'],
                           'remove_paths':['retired.cmd','retired-helper.ps1'],
                           'base':{'path':'GPackData.zpk','commit':base_commit,'bytes':len(base),'sha256':hashlib.sha256(base).hexdigest(),
                                   'parts':[{'path':'GPackData.zpk.part1.zip','bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'entry':'GPackData.zpk','unpacked_bytes':len(base)}]}}
            self.write('GOnline.exe',b'synthetic original executable');self.write(updates.MANAGER,b'synthetic Manager')
            self.write('sdgo/core.dll',b'core fixture');self.write(c.SOURCE,storage.json_bytes(self.binding))
            self.write('sdgo/mods/example/mod.ini',b'[mod]\nid=example\nconfig_files=config.ini\nenabled=1\n[settings]\nValue=1\n')
            self.write('sdgo/mods/example/config.ini',b'[settings]\nPrivateChoice=original\n')
            self.write('sdgo/sdgo.ini',b'[mods]\nexample=1\n');self.write('sprs/example.spr',b'synthetic resource')
            self.write('player-package.json',storage.json_bytes({'runtime':self.contract}));self.git('add','.');self.git('commit','-m','Runtime fixture')
            self.commit=self.git('rev-parse','HEAD').strip()
            type(self)._repo_data=(self.commit,copy.deepcopy(self.contract))
        else:
            self.commit,self.contract=type(self)._repo_data[0],copy.deepcopy(type(self)._repo_data[1])
        target=self.game/c.SOURCE;target.parent.mkdir(parents=True);target.write_bytes(storage.json_bytes(self.binding))
        (self.game/'config.xml').write_bytes(b'personal login and graphics settings')
        (self.game/'retired.cmd').write_bytes(b'old updater');(self.game/'retired-helper.ps1').write_bytes(b'old updater implementation')
        self.controller.repository=lambda binding:(self.repo,self.commit,{'runtime':self.contract},self.contract,1)
        self.fetch=patch.object(self.controller,'git',side_effect=self.local_git);self.fetch.start();self.addCleanup(self.fetch.stop)
        self.running=patch.object(updates,'running',return_value=False);self.running.start();self.addCleanup(self.running.stop)

    def git(self,*args):
        return subprocess.check_output(['git','-C',str(self.repo),*args],stderr=subprocess.DEVNULL).decode().strip()
    def local_git(self,repo,args,input=None,output=None):
        if args[:1]==['fetch']:return b''
        result=subprocess.check_output(['git','-C',str(repo),*args],stderr=subprocess.DEVNULL,input=input)
        if output is not None:Path(output).write_bytes(result);return None
        return result
    def write(self,name,raw):
        p=self.repo/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(raw)

    def test_complete_install_archive_readback_and_script_retirement(self):
        plan=self.controller.plan(self.host);self.assertIn('GPackData.zpk',plan['files'])
        result=self.controller.install(self.host,plan['digest']);self.assertEqual(result['status'],'installed')
        self.assertEqual((self.game/'GPackData.zpk').read_bytes(),b'synthetic original archive')
        self.assertFalse((self.game/'retired.cmd').exists());self.assertFalse((self.game/'retired-helper.ps1').exists())
        self.assertEqual((self.game/'config.xml').read_bytes(),b'personal login and graphics settings')
        receipt=storage.read_json(self.game/c.RECEIPT);self.assertFalse(receipt['data_migrations'])
        self.assertTrue((Path(receipt['backup'])/'retired.cmd').is_file())
        self.assertEqual(self.controller.plan(self.host)['status'],'current')

    def test_partial_inventory_never_requests_sizes_of_unrelated_archives(self):
        self.controller.plan(self.host)
        calls=self.controller.git.call_args_list
        listings=[call.args[1]for call in calls if call.args[1][:1]==['ls-tree']]
        self.assertTrue(listings);self.assertTrue(all('-l'not in args for args in listings))
        archive_oid=self.git('rev-parse','HEAD:GPackData.zpk.part1.zip').encode()
        requests=b''.join(call.kwargs.get('input')or b''for call in calls if call.args[1]==['cat-file','--batch-check'])
        self.assertNotIn(archive_oid,requests)

    def test_cached_blobs_are_not_downloaded_again(self):
        self.controller.plan(self.host)
        self.assertFalse(any(call.args[1][:1]==['fetch']for call in self.controller.git.call_args_list))

    def test_initial_install_gets_default_config_but_existing_preferences_are_preserved(self):
        self.write('config.xml',b'distributor starter configuration');self.git('add','config.xml');self.git('commit','-m','Starter config fixture')
        commit=self.git('rev-parse','HEAD')
        self.controller.repository=lambda binding:(self.repo,commit,{'runtime':self.contract},self.contract,1)
        plan=self.controller.plan(self.host);self.assertNotIn('config.xml',plan['files'])
        self.assertEqual((self.game/'config.xml').read_bytes(),b'personal login and graphics settings')
        (self.game/'config.xml').unlink();plan=self.controller.plan(self.host)
        self.assertIn('config.xml',plan['files']);self.controller.install(self.host,plan['digest'])
        self.assertEqual((self.game/'config.xml').read_bytes(),b'distributor starter configuration')

    def test_polling_working_install_does_not_read_the_live_journal(self):
        self.controller.results[self.host['id']]={'status':'working'}
        self.controller.plans[self.host['id']]={'binding':self.binding}
        with patch.object(self.controller,'unresolved',side_effect=PermissionError('journal being replaced')),patch.object(c,'installed_source',side_effect=PermissionError('descriptor being replaced')):
            state=self.controller.state()['hosts'][0]
            self.assertEqual(state['result']['status'],'working');self.assertFalse(state['recovery_required']);self.assertEqual(state['source'],self.binding)

    def test_existing_authored_settings_and_unknown_custom_content_survive(self):
        p=self.game/'sdgo/mods/example';p.mkdir(parents=True)
        (p/'mod.ini').write_bytes(b'[mod]\nid=example\nconfig_files=config.ini\nenabled=0\n[settings]\nValue=77\n')
        (p/'config.ini').write_bytes(b'[settings]\nPrivateChoice=keep\n')
        custom=self.game/'sprs/custom.spr';custom.parent.mkdir();custom.write_bytes(b'authored extra')
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        self.assertIn(b'Value=77',(p/'mod.ini').read_bytes());self.assertIn(b'enabled=0',(p/'mod.ini').read_bytes())
        self.assertEqual((p/'config.ini').read_bytes(),b'[settings]\nPrivateChoice=keep\n');self.assertEqual(custom.read_bytes(),b'authored extra')
        self.assertEqual(self.controller.plan(self.host)['status'],'current')

    def test_changed_local_files_are_refused_before_any_commit(self):
        plan=self.controller.plan(self.host);(self.game/'retired.cmd').write_bytes(b'changed since preview')
        with self.assertRaises(storage.Conflict):self.controller.install(self.host,plan['digest'])
        self.assertEqual((self.game/'retired.cmd').read_bytes(),b'changed since preview');self.assertFalse((self.game/'GOnline.exe').exists())

    def test_failed_commit_rolls_back_existing_files(self):
        plan=self.controller.plan(self.host);replace=self.controller.move_image
        def fail(src,dest,run):
            if Path(dest)==self.game/'GOnline.exe':raise OSError('fixture failure')
            return replace(src,dest,run)
        with patch.object(self.controller,'move_image',side_effect=fail),self.assertRaises(OSError):self.controller.install(self.host,plan['digest'])
        self.assertEqual((self.game/'retired.cmd').read_bytes(),b'old updater');self.assertFalse((self.game/'GPackData.zpk').exists())
        self.assertFalse((self.game/c.RECEIPT).exists())

    def test_diy_and_running_game_are_preserved(self):
        self.host['runtime_mode']='owner'
        with self.assertRaises(ValueError):self.controller.plan(self.host)
        self.host['runtime_mode']='starter';plan=self.controller.plan(self.host)
        with patch.object(updates,'running',return_value=True):self.assertEqual(self.controller.install(self.host,plan['digest'])['status'],'waiting')
        self.assertTrue((self.game/'retired.cmd').exists())

    def test_mismatched_binding_and_protected_retirement_are_refused(self):
        c.validate_contract({'runtime':self.contract},self.binding)
        self.contract['source_id']='b'*32
        with self.assertRaises(ValueError):c.validate_contract({'runtime':self.contract},self.binding)
        self.contract['source_id']='a'*32;self.contract['remove_paths'].append('config.xml')
        with self.assertRaises(ValueError):c.validate_contract({'runtime':self.contract},self.binding)

    def test_ssh_and_https_reuse_local_identity_without_embedding_credentials(self):
        self.assertEqual(git_source.parse(self.binding['feeds'][0]),('ssh://git@example.invalid/pack.git','main'))
        for url in('git+https://user@example.invalid/x.git','git+ssh://git:secret@example.invalid/x.git','git+ssh://git@example.invalid/x.git#../bad'):
            with self.assertRaises(ValueError):git_source.parse(url)
        self.assertIn('权限',git_source.failure(b'remote: Repository not found.'))

    def test_descriptor_can_register_an_empty_client_without_reclassifying_diy(self):
        from manager.workspace import Workspace
        from manager.owner_channel import OwnerChannels,mode
        workspace=Workspace(self.root/'empty-install',self.root/'addons');self.addCleanup(workspace.close)
        state=workspace.add_host(self.game)
        self.assertEqual(state['hosts'][0]['role'],'client')
        self.assertEqual(state['hosts'][0]['runtime_mode'],'player')
        self.assertTrue(state['hosts'][0]['complete_runtime'])
        self.assertFalse((self.game/'GOnline.exe').exists())
        self.assertEqual(mode(state['hosts'][0]),'player')
        state['hosts'][0]['runtime_mode']='player'
        channels=OwnerChannels(workspace);host=channels.state()['hosts'][0]
        self.assertTrue(host['complete_runtime']);self.assertIsNone(host['source'])
        with self.assertRaises(ValueError):channels.check(host['id'])

    def test_unavailable_ssh_falls_back_only_to_a_matching_configured_source(self):
        controller=c.ClientRuntime(self.workspace);remotes={}
        def git(repo,args,input=None,output=None):
            if args==['init','--bare']:(repo/'HEAD').write_bytes(b'fixture');return b''
            if args[:2]==['remote','add']:remotes[str(repo)]=args[-1];return b''
            if args[:2]==['config','--get']:return remotes[str(repo)].encode()
            if args[0]=='fetch':
                if remotes[str(repo)].startswith('ssh:'):raise ValueError('SSH identity unavailable')
                return b''
            if args[0]=='rev-parse':return self.commit.encode()
            if args[0]=='show':return storage.json_bytes(self.binding if args[1].endswith(':'+c.SOURCE)else{'runtime':self.contract})
            raise AssertionError(args)
        with patch.object(controller,'git',side_effect=git):
            result=controller.repository(self.binding)
            self.assertEqual(result[1],self.commit);self.assertEqual(result[-1],2)
            self.contract['source_id']='b'*32
            with self.assertRaises(ValueError):controller.repository(self.binding)

    def test_bound_player_manager_has_one_program_update_authority(self):
        from manager.workspace import Workspace
        workspace=Workspace(self.root/'single-updater',self.root/'addons');self.addCleanup(workspace.close)
        workspace.settings['hosts']=[self.host]
        (self.game/updates.MANAGER).write_bytes(b'bootstrap Manager')
        fetcher=unittest.mock.Mock(side_effect=AssertionError('independent program update must not run'))
        controller=updates.Updates(workspace,self.root/'distribution',self.game/updates.MANAGER,fetcher)
        self.assertTrue(controller.state()['program_managed_by_client'])
        controller.cycle(install=True);fetcher.assert_not_called()
        self.assertEqual(controller.state()['phase'],'managed');self.assertFalse(controller.state()['manager_restart'])
        self.host['runtime_mode']='owner';self.assertFalse(controller.managed_by_client())

    def test_blob_fetch_failure_and_timeout_use_the_next_bound_mirror(self):
        from manager import owned_process
        for failure in (subprocess.CompletedProcess([],1,b'',b'network transfer failed'),subprocess.TimeoutExpired('git',120)):
            controller=c.ClientRuntime(self.workspace);key=str(self.repo.resolve())
            controller.fetch_mirrors[key]=[(2,'https://example.invalid/fallback.git')]
            with self.subTest(failure=type(failure).__name__),patch.object(owned_process,'run',side_effect=[failure,subprocess.CompletedProcess([],0,b'objects',b'')])as run:
                result=controller.git(self.repo,['fetch','--stdin','origin'],input=b'a'*40+b'\n')
                self.assertEqual(result,b'objects');self.assertEqual(controller.fetch_mirror[key],2)
                self.assertEqual(run.call_args_list[1].args[0][-1],'https://example.invalid/fallback.git')
                self.assertEqual(run.call_args_list[0].kwargs['input'],run.call_args_list[1].kwargs['input'])
                self.assertIn('http.lowSpeedLimit=0',run.call_args_list[0].args[0])
                self.assertIsNone(run.call_args_list[0].kwargs['timeout'])

    def test_network_transfers_have_no_total_deadline_and_keep_cancellation(self):
        from manager import owned_process
        controller=c.ClientRuntime(self.workspace)
        with patch.object(owned_process,'run',return_value=subprocess.CompletedProcess([],0,b'objects',b''))as run:
            controller.git(self.repo,['fetch','--stdin','origin'],input=b'1'*40+b'\n')
            self.assertIsNone(run.call_args.kwargs['timeout'])
            self.assertIs(run.call_args.kwargs['stop'],controller.operation_stop)
            controller.git(self.repo,['fetch','origin','refs/heads/main'])
            self.assertIsNone(run.call_args.kwargs['timeout'])

    def test_cancelled_blob_fetch_does_not_start_another_mirror(self):
        from manager import owned_process
        controller=c.ClientRuntime(self.workspace)
        controller.fetch_mirrors[str(self.repo.resolve())]=[(2,'https://example.invalid/fallback.git')]
        with patch.object(owned_process,'run',side_effect=InterruptedError('cancelled'))as run:
            with self.assertRaises(InterruptedError):controller.git(self.repo,['fetch','--stdin','origin'],input=b'a'*40+b'\n')
            self.assertEqual(run.call_count,1)

    def test_core_safety_cannot_be_downgraded_by_a_complete_pack(self):
        from manager.operations import CORE_CONTRACT
        (self.game/'sdgo/core.dll').write_bytes(CORE_CONTRACT)
        plan=self.controller.plan(self.host)
        with self.assertRaises(ValueError):self.controller.install(self.host,plan['digest'])
        self.assertEqual((self.game/'sdgo/core.dll').read_bytes(),CORE_CONTRACT)
        self.assertTrue((self.game/'retired.cmd').is_file())

    def test_interrupted_transaction_restores_only_unchanged_owned_files(self):
        run=self.controller.cache/'runs'/('c'*32);saved=run/'recovery/retired.cmd';saved.parent.mkdir(parents=True)
        saved.write_bytes(b'old updater');old=c.file_hash(saved)
        (self.game/'retired.cmd').write_bytes(b'partially installed');current=c.file_hash(self.game/'retired.cmd')
        journal={'schema_version':1,'host':str(self.game.resolve()),'status':'working','changes':[{'path':'retired.cmd','previous':old,'installed':current}]}
        marker=self.game/c.RECEIPT;storage.atomic(marker,storage.json_bytes({'interrupted_new_receipt':True}))
        journal['changes'].append({'path':c.RECEIPT,'previous':None,'installed':c.file_hash(marker)})
        storage.atomic(run/'transaction.json',storage.json_bytes(journal))
        with self.assertRaises(ValueError):self.controller.plan(self.host)
        self.assertEqual(self.controller.recover(self.host)['status'],'recovered')
        self.assertEqual((self.game/'retired.cmd').read_bytes(),b'old updater')
        self.assertFalse(self.controller.unresolved(self.game))
        self.assertFalse(marker.exists())
        journal['status']='working';storage.atomic(run/'transaction.json',storage.json_bytes(journal))
        (self.game/'retired.cmd').write_bytes(b'external authored change')
        with self.assertRaises(storage.Conflict):self.controller.recover(self.host)
        self.assertEqual((self.game/'retired.cmd').read_bytes(),b'external authored change')

    def test_running_manager_handoff_installs_all_files_after_exit(self):
        from manager.workspace import Workspace
        workspace=Workspace(self.root/'manager',self.root/'addons')
        self.addCleanup(workspace.close);workspace.settings['hosts']=[self.host]
        controller=c.ClientRuntime(workspace);controller.repository=self.controller.repository
        controller.git=self.local_git
        (self.game/updates.MANAGER).write_bytes(b'old running Manager')
        workspace.updates=SimpleNamespace(manager_exe=self.game/updates.MANAGER)
        plan=controller.plan(self.host);result=controller.install(self.host,plan['digest'])
        self.assertEqual(result['status'],'restart_required')
        self.assertTrue((self.game/'retired.cmd').exists());self.assertFalse((self.game/'GPackData.zpk').exists())
        value=c.validate_handoff(result['handoff']);value['parent_pid']=0
        storage.atomic(Path(result['handoff']),storage.json_bytes(value))
        result=c.finish(result['handoff'],launch=False)
        self.assertEqual(result['status'],'installed');self.assertEqual((self.game/updates.MANAGER).read_bytes(),b'synthetic Manager')

    @unittest.skipUnless(os.name=='nt','Windows image sections')
    def test_new_executable_cannot_start_until_the_complete_commit_finishes(self):
        def image(path):
            try:
                process=subprocess.Popen([str(path),'/d','/c','exit 0'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
            except OSError as error:return False,error.winerror
            process.wait(timeout=10);return True,0
        system=Path(os.environ['SystemRoot'])/'System32'
        old=(system/'ping.exe').read_bytes();new=(system/'cmd.exe').read_bytes()
        executable=self.game/'GOnline.exe';executable.write_bytes(old)
        run=self.controller.cache/'runs'/('d'*32);stage=run/'payload';stage.mkdir(parents=True);(stage/'GOnline.exe').write_bytes(new)
        entry={'bytes':len(new),'sha256':hashlib.sha256(new).hexdigest()}
        plan={'binding':self.binding,'snapshot':{'GOnline.exe':c.file_hash(executable)},'needed':['GOnline.exe'],'retired':[],
              'effective':{'GOnline.exe':entry},'entries':{'GOnline.exe':entry},'source_id':'a'*32,'commit':self.commit,'mirror':1}
        atomic=storage.atomic;observed=[]
        def check(path,raw):
            if Path(path).name=='transaction.json'and executable.read_bytes()==new:
                mapped,error=image(executable);self.assertFalse(mapped);self.assertIn(error,(5,32,1224));observed.append(error)
            return atomic(path,raw)
        self.assertTrue(image(executable)[0])
        with patch.object(storage,'atomic',side_effect=check):self.controller.commit(self.host,plan,run,stage)
        self.assertTrue(observed);self.assertTrue(image(executable)[0])


if __name__=='__main__':unittest.main()
