from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import copy,hashlib,io,json,os,subprocess,sys,tempfile,unittest,zipfile
from manager import client_runtime as c,framework_source as f,storage,updates
from tests.test_manager_updates import bundle
from tools.publish_framework_runtime import publish


class DualRuntime(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name).resolve()
        self.game=self.root/'game';self.game.mkdir();self.repo=self.root/'content';self.repo.mkdir()
        self.framework_binding={'schema_version':1,'kind':'sdgo-framework-runtime','source_id':'b'*32,'repository':updates.REPOSITORY,
                                'channel':'stable','feeds':['git+ssh://git@example.invalid/framework.git#runtime/stable']}
        self.binding={'schema_version':2,'kind':'sdgo-client-runtime','source_id':'a'*32,'label':'Owner content',
                      'feeds':['git+ssh://git@example.invalid/content.git#main'],'framework':self.framework_binding}
        self.host={'id':'client','role':'client','name':'Fixture','runtime_mode':'player','path':str(self.game)}
        self.workspace=SimpleNamespace(root=self.root/'workspace',settings={'hosts':[self.host]})
        self.controller=c.ClientRuntime(self.workspace)
        raw,asset,_=bundle(release='1.7.10');directory=self.root/'framework';manifest=updates.unpack(raw,asset,directory)
        self.framework={'source_id':'b'*32,'version':'1.7.10','channel':'stable','commit':'c'*40,'mirror':1,'sha256':updates.digest(raw),'bundle':str(directory),'manifest':manifest}
        archive=io.BytesIO();self.base=b'fixture native archive'
        with zipfile.ZipFile(archive,'w')as z:z.writestr('GPackData.zpk',self.base)
        part=archive.getvalue();self.write('GPackData.zpk.part1.zip',part)
        self.git('init','-b','main');self.git('config','user.name','Fixture');self.git('config','user.email','fixture@example.invalid');self.commit_all()
        self.contract={'schema_version':1,'source_id':'a'*32,'root_extensions':['.exe','.dll','.ini','.zpk'],'entrypoints':[],
            'trees':['sdgo/mods','sprs'],'remove_paths':['retired.cmd'],'base':{'path':'GPackData.zpk','commit':self.git('rev-parse','HEAD'),
            'bytes':len(self.base),'sha256':updates.digest(self.base),'parts':[{'path':'GPackData.zpk.part1.zip','entry':'GPackData.zpk',
            'bytes':len(part),'sha256':updates.digest(part),'unpacked_bytes':len(self.base)}]},
            'layers':{'schema_version':1,'minimum_framework':'1.7.10','required_mods':{'example':'2.0.0'},
                      'shared_files':['sdgo/sdgo.ini','sdgo/mods/example/config.ini']}}
        self.write('player-package.json',storage.json_bytes({'runtime':self.contract}));self.write(c.SOURCE,storage.json_bytes(self.binding))
        self.write('GOnline.exe',b'original fixture');self.write('sprs/new-unit.spr',b'new custom unit')
        self.write('sdgo/sdgo.ini',b'[mods]\nexample=1\n');self.write('sdgo/mods/example/config.ini',b'[settings]\nvalue=owner revision 2\n')
        self.write('sdgo/shared-settings.json',storage.json_bytes({'schema_version':1,'patches':{'sdgo/mods/example/mod.ini':[['settings','Amount','8']]},
            'personal_settings':[{'file':'sdgo/mods/example/mod.ini','section':'settings','key':'Personal'}]}));self.commit_all()
        p=self.game/c.SOURCE;p.parent.mkdir(parents=True);p.write_bytes(storage.json_bytes(self.binding))
        (self.game/'config.xml').write_bytes(b'personal display preferences')
        (self.game/'retired.cmd').write_bytes(b'retired entry')
        folder=self.game/'sdgo/mods/example';folder.mkdir(parents=True)
        (folder/'mod.ini').write_bytes(b'[mod]\nid=example\nversion=1.0.0\nenabled=1\n[settings]\nAmount=99\nPersonal=37\n')
        (folder/'config.ini').write_bytes(b'[settings]\nvalue=old owner configuration\n')
        self.controller.repository=lambda b:(self.repo,self.git('rev-parse','HEAD'),{'runtime':self.contract},self.contract,1)
        def local(repo,args,input=None,output=None):
            if args[0]=='fetch':return b''
            raw=subprocess.check_output(['git','-C',str(repo),*args],input=input,stderr=subprocess.DEVNULL)
            if output:Path(output).write_bytes(raw);return None
            return raw
        self.addCleanup(patch.stopall)
        patch.object(self.controller,'git',side_effect=local).start();patch.object(f,'load',return_value=self.framework).start()
        patch.object(updates,'running',return_value=False).start()

    def git(self,*args):return subprocess.check_output(['git','-C',str(self.repo),*args],stderr=subprocess.DEVNULL).decode().strip()
    def write(self,name,raw):
        p=self.repo/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(raw)
    def commit_all(self):self.git('add','.');self.git('commit','-m','Fixture content')

    def test_two_sources_shared_settings_personal_state_and_receipts(self):
        plan=self.controller.plan(self.host);self.assertEqual(plan['framework_version'],'1.7.10')
        result=self.controller.install(self.host,plan['digest']);self.assertEqual(result['status'],'installed')
        cfg=updates.read_ini((self.game/'sdgo/mods/example/mod.ini').read_bytes())
        self.assertEqual(cfg['mod']['version'],'2.0.0');self.assertEqual(cfg['settings']['Amount'],'8');self.assertEqual(cfg['settings']['Personal'],'37')
        self.assertIn(b'owner revision 2',(self.game/'sdgo/mods/example/config.ini').read_bytes())
        self.assertEqual((self.game/'config.xml').read_bytes(),b'personal display preferences')
        self.assertEqual((self.game/'sprs/new-unit.spr').read_bytes(),b'new custom unit');self.assertFalse((self.game/'retired.cmd').exists())
        receipt=storage.read_json(self.game/c.RECEIPT);self.assertEqual(receipt['framework']['commit'],'c'*40)
        self.assertEqual(receipt['content_commit'],self.git('rev-parse','HEAD'));self.assertFalse(receipt['data_migrations'])
        self.assertEqual(self.controller.plan(self.host)['status'],'current')

    def test_content_cannot_replace_program_dll_or_mod_identity(self):
        self.write('sdgo/core.dll',b'owner program override');self.commit_all()
        with self.assertRaisesRegex(ValueError,'不能替换框架'):self.controller.plan(self.host)

    def test_incompatible_framework_or_missing_provider_is_refused(self):
        self.contract['layers']['minimum_framework']='1.7.11'
        with self.assertRaisesRegex(ValueError,'兼容要求'):self.controller.plan(self.host)
        self.contract['layers']['minimum_framework']='1.7.10';self.contract['layers']['required_mods']['missing']='1.0.0'
        with self.assertRaisesRegex(ValueError,'missing'):self.controller.plan(self.host)
        self.assertFalse((self.game/'d3d9.dll').exists())

    def test_forged_shared_metadata_is_refused(self):
        self.write('sdgo/shared-settings.json',storage.json_bytes({'schema_version':1,'patches':{'sdgo/mods/example/mod.ini':[['mod','dll','foreign.dll']]}}));self.commit_all()
        with self.assertRaisesRegex(ValueError,'身份或接口'):self.controller.plan(self.host)

    def test_failure_rolls_back_both_sources(self):
        before={p.relative_to(self.game).as_posix():p.read_bytes()for p in self.game.rglob('*')if p.is_file()}
        plan=self.controller.plan(self.host);replace=self.controller.move_image
        def fail(src,target,run):
            if target.name==updates.MANAGER:raise OSError('fixture commit failure')
            return replace(src,target,run)
        with patch.object(self.controller,'move_image',side_effect=fail):
            with self.assertRaises(OSError):self.controller.install(self.host,plan['digest'])
        after={p.relative_to(self.game).as_posix():p.read_bytes()for p in self.game.rglob('*')if p.is_file()}
        self.assertEqual(before,after)


class GitFrameworkTransport(unittest.TestCase):
    def test_published_artifact_is_loaded_from_git_and_versions_are_immutable(self):
        with tempfile.TemporaryDirectory()as temporary:
            root=Path(temporary).resolve();source=root/'source';source.mkdir();remote=root/'remote.git'
            def git(path,*args):return subprocess.check_output(['git','-C',str(path),*args],stderr=subprocess.DEVNULL)
            subprocess.run(['git','init','--bare',str(remote)],capture_output=True,check=True)
            git(source,'init');git(source,'config','user.name','Fixture');git(source,'config','user.email','fixture@example.invalid');git(source,'remote','add','origin',str(remote))
            raw,asset,_=bundle(release='1.7.10');archive=root/'SDGO-ModFramework-1.7.10.zip';archive.write_bytes(raw)
            feed,commit=publish(archive,source,push=True);self.assertEqual(publish(archive,source,push=True)[1],commit)
            self.assertEqual(git(source,'log','-1','--format=%an <%ae>|%cn <%ce>',commit).decode().strip(),
                             'Fixture <fixture@example.invalid>|Fixture <fixture@example.invalid>')
            controller=c.ClientRuntime(SimpleNamespace(root=root/'workspace'));original=controller.git
            def local(repo,args,input=None,output=None):
                if args[:1]==['fetch']:
                    args=list(args);args[args.index('origin')]=str(remote)
                return original(repo,args,input=input,output=output)
            binding={'schema_version':1,'kind':'sdgo-framework-runtime','source_id':feed['source_id'],'repository':updates.REPOSITORY,
                     'channel':'stable','feeds':['git+ssh://git@example.invalid/framework.git#runtime/stable']}
            with patch.object(controller,'git',side_effect=local):
                loaded=f.load(controller,binding)
            self.assertEqual(loaded['version'],'1.7.10');self.assertEqual(loaded['commit'],commit)
            self.assertEqual((Path(loaded['bundle'])/updates.MANAGER).read_bytes(),b'new manager')
            changed,_,_=bundle(release='1.7.10',extra={'client/sdgo/sdgo.ini':b'[core]\nlog=debug\n'})
            archive.write_bytes(changed)
            with self.assertRaisesRegex(ValueError,'immutable'):publish(archive,source,push=True)

    def test_runner_without_git_identity_publishes_as_actions_bot(self):
        # Hosted CI runners have no user.name/user.email and no usable host email to guess from.
        with tempfile.TemporaryDirectory()as temporary:
            root=Path(temporary).resolve();source=root/'source';source.mkdir();remote=root/'remote.git';config=root/'global.gitconfig'
            config.write_text('[user]\n\tuseConfigOnly = true\n',encoding='utf-8')
            with patch.dict('os.environ',{'GIT_CONFIG_GLOBAL':str(config),'GIT_CONFIG_NOSYSTEM':'1'}):
                for name in('GIT_AUTHOR_NAME','GIT_AUTHOR_EMAIL','GIT_COMMITTER_NAME','GIT_COMMITTER_EMAIL','EMAIL'):os.environ.pop(name,None)
                def git(path,*args):return subprocess.run(['git','-C',str(path),*args],capture_output=True)
                subprocess.run(['git','init','--bare',str(remote)],capture_output=True,check=True)
                git(source,'init');git(source,'remote','add','origin',str(remote))
                self.assertNotEqual(git(source,'var','GIT_COMMITTER_IDENT').returncode,0)
                raw,_,_=bundle(release='1.7.10');archive=root/'SDGO-ModFramework-1.7.10.zip';archive.write_bytes(raw)
                feed,commit=publish(archive,source,push=True)
                self.assertEqual(git(remote,'rev-parse','refs/heads/runtime/stable').stdout.decode().strip(),commit)
                bot='github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>'
                self.assertEqual(git(source,'log','-1','--format=%an <%ae>|%cn <%ce>',commit).stdout.decode().strip(),bot+'|'+bot)
                self.assertNotEqual(git(source,'var','GIT_COMMITTER_IDENT').returncode,0)
                self.assertEqual(publish(archive,source,push=True)[1],commit)
                older,_,_=bundle(release='1.7.9');stale=root/'SDGO-ModFramework-1.7.9.zip';stale.write_bytes(older)
                with self.assertRaisesRegex(ValueError,'newer'):publish(stale,source,push=True)
                changed,_,_=bundle(release='1.7.10',extra={'client/sdgo/sdgo.ini':b'[core]\nlog=debug\n'});archive.write_bytes(changed)
                with self.assertRaisesRegex(ValueError,'immutable'):publish(archive,source,push=True)
                self.assertEqual(git(remote,'rev-parse','refs/heads/runtime/stable').stdout.decode().strip(),commit)

    def test_rejected_mirror_ref_is_reported_privately_without_blocking_runtime_refs(self):
        with tempfile.TemporaryDirectory()as temporary:
            root=Path(temporary).resolve();mirrors=root/'mirrors';work=root/'work';placeholder=root/'placeholder';origin=root/'origin.git';checkout=root/'checkout'
            def git(path,*args):return subprocess.run(['git','-C',str(path),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid',*args],capture_output=True,check=True).stdout.decode().strip()
            for bare in(origin,mirrors/'framework.git',mirrors/'client.git',mirrors/'locked.git'):subprocess.run(['git','init','-q','--bare',str(bare)],check=True)
            # A hosted mirror that refuses pushes explains why in its own language.
            hook=mirrors/'locked.git/hooks/pre-receive';hook.write_bytes('#!/bin/sh\necho "镜像仓库不允许直接推送" >&2\nexit 1\n'.encode());hook.chmod(0o755)
            for path in(work,placeholder):path.mkdir();git(path,'init','-q')
            git(work,'commit','-q','--allow-empty','-m','Source');git(work,'push','-q',str(origin),'HEAD:refs/heads/master')
            git(work,'checkout','-q','--orphan','runtime');git(work,'commit','-q','--allow-empty','-m','Runtime');git(work,'push','-q',str(origin),'HEAD:refs/heads/runtime/beta')
            # A hosted mirror created with its own initial commit shares no history with master.
            git(placeholder,'commit','-q','--allow-empty','-m','Initial commit');git(placeholder,'push','-q',str(mirrors/'framework.git'),'HEAD:refs/heads/master')
            subprocess.run(['git','clone','-q',str(origin),str(checkout)],capture_output=True,check=True)
            runtime=git(origin,'rev-parse','refs/heads/runtime/beta');initial=git(mirrors/'framework.git','rev-parse','refs/heads/master')
            def mirror(targets):
                env={**os.environ,'GIT_CONFIG_COUNT':'1','GIT_CONFIG_KEY_0':'url.'+mirrors.as_posix()+'/.insteadOf','GIT_CONFIG_VALUE_0':'https://mirror.invalid/',
                     'RUNTIME_GIT_MIRROR_URL':'https://mirror.invalid/framework.git','RUNTIME_GIT_MIRROR_TOKEN':'mirror-secret-value','RUNTIME_GIT_MIRROR_TARGETS':json.dumps(targets),
                     'PYTHONIOENCODING':'cp1252'}
                return subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/sync_runtime_mirror.py')],cwd=checkout,env=env,capture_output=True,encoding='utf-8',errors='replace')
            framework='https://mirror.invalid/framework.git';client='https://mirror.invalid/client.git'
            result=mirror([{'url':framework,'branches':['master','runtime/stable','runtime/beta']},{'url':client,'branches':['runtime/beta']},
                           {'url':'https://mirror.invalid/locked.git','branches':['runtime/beta']}])
            self.assertEqual(result.returncode,1,result.stdout+result.stderr);self.assertNotIn('Traceback',result.stderr)
            self.assertIn('Verified mirrored refs: 0:runtime/beta, 1:runtime/beta',result.stdout)
            errors=[l for l in result.stdout.splitlines()if l.startswith('::error')]
            self.assertEqual(len(errors),2,result.stdout);self.assertRegex(errors[0],r'target 0 master: push exited 1: .*(fetch first|rejected)')
            self.assertRegex(errors[1],r'target 2 runtime/beta: push exited 1: .*remote: 镜像仓库不允许直接推送.*pre-receive hook declined')
            for leak in('mirror.invalid',mirrors.as_posix(),str(mirrors),'mirror-secret-value'):self.assertNotIn(leak,result.stdout+result.stderr)
            self.assertEqual(git(mirrors/'framework.git','rev-parse','refs/heads/runtime/beta'),runtime)
            self.assertEqual(git(mirrors/'client.git','rev-parse','refs/heads/runtime/beta'),runtime)
            self.assertEqual(git(mirrors/'framework.git','rev-parse','refs/heads/master'),initial)
            result=mirror([{'url':framework,'branches':['runtime/stable','runtime/beta']},{'url':client,'branches':['runtime/beta']}])
            self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('Verified mirrored refs: 0:runtime/beta, 1:runtime/beta',result.stdout)

    def test_mirror_diagnostics_drop_addresses_and_tokens(self):
        from tools.sync_runtime_mirror import reason
        stderr=b"To https://codeup.example/org/x.git\n ! [remote rejected] abc -> runtime/beta (pre-receive hook declined)\nremote: see https://codeup.example/help\nfatal: Authentication failed for 'https://git:tok123@codeup.example/x.git/'\n"
        self.assertEqual(reason(stderr,{'RUNTIME_GIT_MIRROR_TOKEN':'tok123'}),
                         "! [remote rejected] abc -> runtime/beta (pre-receive hook declined) | remote: see <mirror> | fatal: Authentication failed for '<mirror>'")

    def test_source_identity_and_asset_convention_are_validated(self):
        for value in ({},{'schema_version':1,'kind':'sdgo-framework-runtime','source_id':'a'*32,'repository':updates.REPOSITORY,
                         'channel':'stable','feeds':['git+ssh://private:secret@example.invalid/repo.git']}):
            with self.assertRaises(ValueError):f.binding(value)


if __name__=='__main__':unittest.main()
