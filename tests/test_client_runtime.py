"""Complete-runtime updates use synthetic Git blobs and never game/user data."""
from pathlib import Path
import hashlib,io,json,os,subprocess,tempfile,unittest,zipfile
from types import SimpleNamespace
from unittest.mock import patch
from manager import client_runtime as c,git_source,storage,updates


class CompleteRuntime(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.game=self.root/'game';self.game.mkdir();self.repo=self.root/'repo';self.repo.mkdir()
        self.binding={'schema_version':1,'kind':'sdgo-client-runtime','source_id':'a'*32,'label':'Example pack',
                      'feeds':['git+ssh://git@example.invalid/pack.git#main','git+https://example.invalid/pack.git#main']}
        self.host={'id':'test','name':'Example','path':str(self.game),'role':'client','runtime_mode':'starter'}
        self.workspace=SimpleNamespace(root=self.root/'manager',settings={'hosts':[self.host]})
        self.controller=c.ClientRuntime(self.workspace)
        self.environment=patch.dict(os.environ,{'LOCALAPPDATA':str(self.root/'local')});self.environment.start();self.addCleanup(self.environment.stop)
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
        plan=self.controller.plan(self.host);replace=os.replace
        def fail(src,dest):
            if Path(dest)==self.game/'GOnline.exe':raise OSError('fixture failure')
            return replace(src,dest)
        with patch.object(os,'replace',side_effect=fail),self.assertRaises(OSError):self.controller.install(self.host,plan['digest'])
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
        storage.atomic(run/'transaction.json',storage.json_bytes(journal))
        with self.assertRaises(ValueError):self.controller.plan(self.host)
        self.assertEqual(self.controller.recover(self.host)['status'],'recovered')
        self.assertEqual((self.game/'retired.cmd').read_bytes(),b'old updater')
        self.assertFalse(self.controller.unresolved(self.game))
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


if __name__=='__main__':unittest.main()
