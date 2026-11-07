from pathlib import Path
from unittest.mock import patch
import copy,json,unittest
from manager import server_profiles as p,storage,updates
from tests.test_runtime_layers import DualRuntime
from tests.test_manager_updates import fake_exe
from manager import profile_publish,transfers


def fixture_download(url,target,entry,stop=None,progress=None):
    raw=updates.fetch(url,entry['bytes']+1,stop)
    if len(raw)!=entry['bytes']or updates.digest(raw)!=entry['sha256']:raise ValueError('校验失败')
    storage.atomic(target,raw)
    if progress:progress(len(raw))
    return target


class Profiles(unittest.TestCase):
    git=DualRuntime.git;write=DualRuntime.write;commit_all=DualRuntime.commit_all
    def setUp(self):
        DualRuntime.setUp(self);fake_exe(self.game/'GOnline.exe')
        patch.object(transfers,'download',side_effect=fixture_download).start()
        self.framework['manifest'].setdefault('capabilities',{})['runtime_profiles']=1
        self.profile={'schema_version':1,'kind':p.KIND,'source_id':'a'*32,'label':'Fixture owner','revision':1,
            'base':{'executable':'GOnline.exe','host_key':updates.host_key(self.game/'GOnline.exe')},
            'framework':{k:self.framework[k]for k in('version','sha256','commit')},'mods':{'example':'2.0.0'},
            'shared_files':['sdgo/sdgo.ini','sdgo/mods/example/config.ini'],'files':{}}
        names=['sdgo/sdgo.ini','sdgo/mods/example/config.ini','sprs/new-unit.spr',p.SHARED]
        for name in names:
            raw=(self.repo/name).read_bytes();target=self.repo/p.CONTENT_PREFIX/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
            self.profile['files'][name]={'bytes':len(raw),'sha256':updates.digest(raw)}
        (self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        self.source={**self.binding,'schema_version':3,'feeds':['local+'+(self.repo/p.MANIFEST).as_uri()]}
        (self.game/'sdgo/client-source.json').write_bytes(storage.json_bytes(self.source))

    def test_offline_profile_combines_selected_mods_and_content_without_game_archive(self):
        original=(self.game/'GOnline.exe').read_bytes();plan=self.controller.plan(self.host)
        self.assertNotIn(updates.MANAGER,plan['files'])
        self.assertNotIn('GPackData.zpk',plan['files']);self.assertNotIn('GOnline.exe',plan['files'])
        result=self.controller.install(self.host,plan['digest']);self.assertEqual(result['status'],'installed')
        self.assertEqual((self.game/'GOnline.exe').read_bytes(),original)
        self.assertEqual((self.game/'sprs/new-unit.spr').read_bytes(),b'new custom unit')
        cfg=updates.read_ini((self.game/'sdgo/mods/example/mod.ini').read_bytes());self.assertEqual(cfg['settings']['Amount'],'8');self.assertEqual(cfg['settings']['Personal'],'37')
        self.assertEqual(self.controller.plan(self.host)['status'],'current')
        self.assertEqual(storage.read_json(self.game/'sdgo/local-state/client-runtime.json')['profile']['revision'],1)

    def test_https_manifest_and_individual_content_use_the_same_composer(self):
        source={**self.source,'feeds':['https://owner.invalid/server-profile.json']}
        (self.game/'sdgo/client-source.json').write_bytes(storage.json_bytes(source))
        def fetch(url,limit,stop=None):
            if url.endswith('/server-profile.json'):return storage.json_bytes(self.profile)
            name=url.split('/content/',1)[1];return(self.repo/'content'/name).read_bytes()
        with patch.object(updates,'fetch',side_effect=fetch):
            plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        self.assertEqual(storage.read_json(self.game/'sdgo/local-state/client-runtime.json')['profile']['transport'],'https')

    def test_wrong_base_is_refused_before_any_install(self):
        self.profile['base']['host_key']='GOnline-00000000-00000001';(self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        with self.assertRaisesRegex(ValueError,'匹配版本'):self.controller.plan(self.host)
        self.assertFalse((self.game/'d3d9.dll').exists())

    def test_payload_failure_uses_matching_backup_without_changing_publisher(self):
        source={**self.source,'feeds':['https://first.invalid/server-profile.json','https://backup.invalid/server-profile.json']}
        (self.game/'sdgo/client-source.json').write_bytes(storage.json_bytes(source))
        def fetch(url,limit,stop=None):
            if url.endswith('/server-profile.json'):return storage.json_bytes(self.profile)
            if 'first.invalid'in url:raise OSError('first payload unavailable')
            return(self.repo/'content'/url.split('/content/',1)[1]).read_bytes()
        with patch.object(updates,'fetch',side_effect=fetch):
            plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        receipt=storage.read_json(self.game/'sdgo/local-state/client-runtime.json')
        self.assertEqual(receipt['source_id'],source['source_id']);self.assertEqual(receipt['mirror'],2)

    def test_imported_programme_mirrors_are_used_with_the_owners_channel_and_lock(self):
        self.profile['framework_source']=copy.deepcopy(self.source['framework'])
        self.profile['framework_source']['channel']='beta'
        self.profile['framework']['version']='1.7.10-beta.1'
        self.source['framework']['feeds']=['git+https://selected.invalid/framework.git#runtime/stable']
        (self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        (self.game/'sdgo/client-source.json').write_bytes(storage.json_bytes(self.source))
        chosen={**self.framework,'version':'1.7.10-beta.1','channel':'beta'}
        with patch('manager.framework_source.load',return_value=chosen)as load:
            plan=self.controller.plan(self.host)
        self.assertEqual(load.call_args.args[1]['feeds'],self.source['framework']['feeds'])
        self.assertEqual(load.call_args.args[1]['channel'],'beta')
        self.assertEqual(load.call_args.kwargs['constraints']['locked_framework'],self.profile['framework'])
        self.assertEqual(plan['framework_version'],'1.7.10-beta.1')

    def test_different_revision_is_not_accepted_as_a_payload_backup(self):
        source={**self.source,'feeds':['https://first.invalid/server-profile.json','https://backup.invalid/server-profile.json']}
        (self.game/'sdgo/client-source.json').write_bytes(storage.json_bytes(source))
        def fetch(url,limit,stop=None):
            if url.endswith('/server-profile.json'):
                profile=copy.deepcopy(self.profile)
                if 'backup.invalid'in url:profile['revision']+=1
                return storage.json_bytes(profile)
            raise OSError('unavailable')
        pinned={'type':'https','feed':source['feeds'][0]}
        with patch.object(p,'resolve',return_value=(self.profile,pinned,1)),patch.object(updates,'fetch',side_effect=fetch),self.assertRaisesRegex(ValueError,'保留当前安装'):self.controller.plan(self.host)
        self.assertFalse((self.game/'d3d9.dll').exists())

    def test_lagging_mirror_does_not_hide_a_newer_revision_before_locking(self):
        source={**self.source,'feeds':['https://first.invalid/server-profile.json','https://newer.invalid/server-profile.json']}
        def fetch(url,limit,stop=None):
            profile=copy.deepcopy(self.profile)
            if 'newer.invalid'in url:profile['revision']=2
            return storage.json_bytes(profile)
        with patch.object(updates,'fetch',side_effect=fetch):profile,_,mirror=p.resolve(self.controller,source)
        self.assertEqual(profile['revision'],2);self.assertEqual(mirror,2)

    def test_same_revision_with_different_manifests_is_refused(self):
        source={**self.source,'feeds':['https://first.invalid/server-profile.json','https://other.invalid/server-profile.json']}
        def fetch(url,limit,stop=None):
            profile=copy.deepcopy(self.profile)
            if 'other.invalid'in url:profile['label']='Different same-revision bytes'
            return storage.json_bytes(profile)
        with patch.object(updates,'fetch',side_effect=fetch),self.assertRaisesRegex(ValueError,'不同清单'):p.resolve(self.controller,source)

    def test_failed_download_keeps_verified_files_and_new_revision_reuses_installed_content(self):
        source={**self.source,'feeds':['https://owner.invalid/server-profile.json']}
        (self.game/'sdgo/client-source.json').write_bytes(storage.json_bytes(source))
        calls=[];fail=True
        def fetch(url,limit,stop=None):
            nonlocal fail
            if url.endswith('/server-profile.json'):return storage.json_bytes(self.profile)
            name=url.split('/content/',1)[1];calls.append(name)
            if name=='sprs/new-unit.spr'and fail:raise OSError('connection interrupted')
            return(self.repo/'content'/name).read_bytes()
        with patch.object(updates,'fetch',side_effect=fetch):
            with self.assertRaises(ValueError):self.controller.plan(self.host)
            self.assertFalse((self.game/'d3d9.dll').exists());fail=False
            plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
            self.assertEqual(calls.count('sdgo/sdgo.ini'),1)
            self.profile['revision']=2;raw=b'new content revision';name='sprs/new-unit.spr'
            (self.repo/'content'/name).write_bytes(raw);self.profile['files'][name]={'bytes':len(raw),'sha256':updates.digest(raw)}
            calls.clear();plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
            self.assertEqual(calls,[name]);self.assertEqual((self.game/name).read_bytes(),raw)

    def test_program_sql_database_and_private_rosters_cannot_be_client_content(self):
        for name in('GOnline.exe','sdgo/core.dll','sprs/data.sql','mdrs/data.mdf','sdgo/mods/example/manager.json'):
            with self.subTest(name=name),self.assertRaises(ValueError):p.content_name(name)
        with self.assertRaises(ValueError):p.check_secrets({'birthdays':[{'name':'private'}]})

    def test_content_tampering_and_publisher_changes_are_refused(self):
        (self.repo/'content/sprs/new-unit.spr').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'校验失败'):self.controller.plan(self.host)
        self.profile['source_id']='b'*32;(self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        with self.assertRaisesRegex(ValueError,'身份'):self.controller.plan(self.host)

    def test_owner_export_contains_recipe_and_content_without_programs_or_originals(self):
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        host={**self.host,'runtime_mode':'owner'};directory=self.root/'profile-export'
        result=profile_publish.publish(host,self.source,self.framework,directory,extras=['sprs/new-unit.spr'])
        exported=__import__('json').loads((directory/p.MANIFEST).read_text())
        self.assertEqual(result['revision'],1);self.assertEqual(exported['mods'],{'example':'2.0.0'})
        self.assertNotIn('GOnline.exe',exported['files']);self.assertNotIn('sdgo/core.dll',exported['files'])
        self.assertNotIn('sdgo/mods/example/example.dll',exported['files']);self.assertIn('sprs/new-unit.spr',exported['files'])
        self.assertEqual(profile_publish.publish(host,self.source,self.framework,directory,extras=['sprs/new-unit.spr'])['revision'],1)

    def test_owner_program_drift_is_refused_even_when_version_number_matches(self):
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        (self.game/'sdgo/mods/example/example.dll').write_bytes(b'unpublished program with the same version')
        with self.assertRaisesRegex(ValueError,'程序与所选发布包不同'):
            profile_publish.snapshot({**self.host,'runtime_mode':'owner'},self.source,self.framework)

    def test_owner_api_keeps_existing_identity_and_returns_saved_publication(self):
        from types import SimpleNamespace
        from manager.owner_channel import OwnerChannels
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        self.host['runtime_mode']='owner';self.host['publication']={'publisher_id':self.source['source_id']}
        self.workspace.host=lambda identity:self.host;self.workspace.save_workspace=lambda:None
        self.workspace.services=SimpleNamespace(desired=lambda _:False)
        self.workspace.updates=SimpleNamespace(latest={'version':self.framework['version'],'sha256':self.framework['sha256'],'channel':'stable'},source_repository=lambda:updates.REPOSITORY)
        destination=self.root/'api-export'
        result=self.controller.publish_profile(self.host['id'],str(destination),self.source['feeds'],['sprs/new-unit.spr'])
        self.assertEqual(result['revision'],1)
        self.assertEqual(storage.read_json(destination/p.MANIFEST)['source_id'],self.source['source_id'])
        state=OwnerChannels(self.workspace).state()['hosts'][0]['profile_publication']
        self.assertEqual(state['directory'],str(destination.resolve()));self.assertEqual(state['feeds'],self.source['feeds'])

    def test_owner_content_export_keeps_programme_lock_after_restart_or_new_upstream(self):
        from types import SimpleNamespace
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        self.host['runtime_mode']='owner';self.host['publication']={'publisher_id':self.source['source_id']}
        self.workspace.host=lambda _:self.host;self.workspace.save_workspace=lambda:None
        self.workspace.updates=SimpleNamespace(latest={'version':self.framework['version'],'sha256':self.framework['sha256'],'channel':'stable'},source_repository=lambda:updates.REPOSITORY)
        directory=self.root/'retained-lock'
        self.controller.publish_profile(self.host['id'],str(directory),self.source['feeds'],['sprs/new-unit.spr'])
        accepted=storage.read_json(directory/p.MANIFEST)['framework']
        for latest in(None,{'version':'99.0.0','sha256':'f'*64,'channel':'stable'}):
            with self.subTest(latest=latest):
                self.workspace.updates.latest=latest
                (self.game/'sdgo/mods/example/config.ini').write_bytes(('[settings]\nvalue=changed '+str(latest is None)+'\n').encode())
                with patch('manager.framework_source.load',return_value=self.framework)as load:
                    result=self.controller.publish_profile(self.host['id'],str(directory),self.source['feeds'],['sprs/new-unit.spr'])
                self.assertEqual(load.call_args.args[2],{'minimum_framework':accepted['version'],'locked_framework':accepted})
                self.assertEqual(storage.read_json(directory/p.MANIFEST)['framework'],accepted)
                self.assertGreater(result['revision'],1)
        storage.atomic(self.game/'sdgo/release.json',storage.json_bytes({'version':'99.0.0'}))
        self.workspace.updates.latest=None
        with self.assertRaisesRegex(ValueError,'先检查并选择'):
            self.controller.publish_profile(self.host['id'],str(directory),self.source['feeds'])

    def test_owner_content_export_refuses_unpublished_framework_bytes(self):
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        (self.game/'sdgo/core.dll').write_bytes(b'unpublished core with unchanged metadata')
        with self.assertRaisesRegex(ValueError,'框架程序与所选发布包不同'):
            profile_publish.snapshot({**self.host,'runtime_mode':'owner'},self.source,self.framework)

    def test_publication_network_commands_do_not_have_total_transfer_deadlines(self):
        from types import SimpleNamespace
        from manager.client_runtime import ClientRuntime
        for args in(['fetch','origin'],['push','origin','HEAD:content'],['ls-remote','origin']):
            with self.subTest(command=args[0]),patch('manager.owned_process.run',return_value=SimpleNamespace(returncode=0,stdout=b'',stderr=b''))as run:
                ClientRuntime.git(self.controller,self.repo,args)
                self.assertIsNone(run.call_args.kwargs['timeout'])
                self.assertIs(run.call_args.kwargs['stop'],self.controller.operation_stop)

    def test_git_publication_preserves_crlf_unicode_and_binary_bytes(self):
        import subprocess
        from types import SimpleNamespace
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        raw='[settings]\r\nlabel=机体资源\r\n'.encode('utf-8')
        (self.game/'sdgo/mods/example/config.ini').write_bytes(raw)
        directory=self.root/'git-profile';directory.mkdir()
        def git(*args):return subprocess.check_output(['git','-C',str(directory),*args],stderr=subprocess.DEVNULL)
        git('init','-b','content');git('config','core.autocrlf','true')
        profile_publish.publish({**self.host,'runtime_mode':'owner'},self.source,self.framework,directory,extras=['sprs/new-unit.spr'])
        manifest=storage.read_json(directory/p.MANIFEST)
        adapter=SimpleNamespace(git=lambda path,args:subprocess.check_output(['git','-C',str(path),*args],stderr=subprocess.DEVNULL))
        profile_publish.git_stage(adapter,directory,manifest)
        self.assertEqual(git('show',':content/sdgo/mods/example/config.ini'),raw)
        self.assertEqual(git('show',':content/sprs/new-unit.spr'),(self.game/'sprs/new-unit.spr').read_bytes())
        git('config','user.name','Fixture');git('config','user.email','fixture@example.invalid');git('commit','-m','Fixture profile')
        commit=git('rev-parse','HEAD').decode().strip()
        import shutil
        uri='git+ssh://git@example.invalid/profile.git#content'
        cache=self.controller.cache/'git'/updates.digest(uri.encode())
        shutil.copytree(directory/'.git',cache)
        subprocess.check_call(['git','-C',str(cache),'config','core.bare','true'],stdout=subprocess.DEVNULL)
        subprocess.check_call(['git','-C',str(cache),'remote','add','origin','ssh://git@example.invalid/profile.git'],stdout=subprocess.DEVNULL)
        (cache/'FETCH_HEAD').write_text(commit+'\n',encoding='ascii')
        raw_manifest,resolved=p.read(self.controller,uri,p.MANIFEST,16*1024*1024)
        self.assertEqual(json.loads(raw_manifest),manifest);self.assertEqual(resolved['commit'],commit)
        fresh=self.root/'fresh';fake_exe(fresh/'GOnline.exe')
        (fresh/'sdgo').mkdir();(fresh/'sdgo/client-source.json').write_bytes(storage.json_bytes(self.source))
        host={**self.host,'id':'fresh','path':str(fresh)}
        transport={'type':'git','repo':str(directory),'commit':commit,'feed':self.source['feeds'][0]}
        with patch.object(p,'resolve',return_value=(manifest,transport,1)):
            plan=self.controller.plan(host);self.controller.install(host,plan['digest'])
        self.assertEqual((fresh/'sdgo/mods/example/config.ini').read_bytes(),raw)
        self.assertEqual((fresh/'sprs/new-unit.spr').read_bytes(),(self.game/'sprs/new-unit.spr').read_bytes())

    def test_configuration_revision_updates_keep_original_and_personal_settings(self):
        original=(self.game/'GOnline.exe').read_bytes()
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        self.profile['revision']=2;raw=b'updated custom unit';name='sprs/new-unit.spr'
        (self.repo/'content'/name).write_bytes(raw);self.profile['files'][name]={'bytes':len(raw),'sha256':updates.digest(raw)}
        (self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        plan=self.controller.plan(self.host);self.assertEqual(plan['files'],[name]);self.controller.install(self.host,plan['digest'])
        self.assertEqual((self.game/name).read_bytes(),raw);self.assertEqual((self.game/'GOnline.exe').read_bytes(),original)
        self.profile['revision']=1;(self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        with self.assertRaisesRegex(ValueError,'回退'):self.controller.plan(self.host)

    def test_imported_shared_settings_reject_credentials_in_patch_rows(self):
        name=p.SHARED;raw=storage.json_bytes({'schema_version':1,'patches':{'sdgo/mods/example/mod.ini':[['settings','Password','private']]}})
        (self.repo/'content'/name).write_bytes(raw);self.profile['files'][name]={'bytes':len(raw),'sha256':updates.digest(raw)}
        (self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        with self.assertRaisesRegex(ValueError,'私人'):self.controller.plan(self.host)

    def test_mod_versions_are_exact_when_profile_is_locked(self):
        self.profile['mods']['example']='1.0.0';(self.repo/p.MANIFEST).write_bytes(storage.json_bytes(self.profile))
        with self.assertRaisesRegex(ValueError,'版本不一致'):self.controller.plan(self.host)

    def test_binding_configuration_file_can_be_imported_directly(self):
        marker=self.repo/'profile-source.json';marker.write_bytes(storage.json_bytes(self.source))
        self.workspace.host=lambda identity:self.host;self.workspace.save_workspace=lambda:None
        with patch.object(self.controller,'start'):
            self.controller.bind_profile(self.host['id'],[],str(marker))
        imported=storage.read_json(self.game/'sdgo/client-source.json')
        self.assertEqual(imported['feeds'],self.source['feeds']);self.assertEqual(imported['source_id'],self.source['source_id'])

    def test_framework_program_selection_does_not_install_unselected_mods(self):
        catalog_path=Path(self.framework['bundle'])/'client/sdgo/catalog.json';catalog=storage.read_json(catalog_path)
        other={**catalog['mods'][0],'id':'unused'};catalog['mods'].append(other);catalog_path.write_bytes(storage.json_bytes(catalog))
        raw=catalog_path.read_bytes();self.framework['manifest']['files']['client/sdgo/catalog.json']={'bytes':len(raw),'sha256':updates.digest(raw)}
        # This is composition coverage; verified transport separately protects
        # the original catalog bytes before the layer is accepted.
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        ids=[m['id']for m in storage.read_json(self.game/'sdgo/catalog.json')['mods']]
        self.assertEqual(ids,['example']);self.assertFalse((self.game/'sdgo/mods/unused').exists())

    def test_one_click_binding_queues_one_combined_install(self):
        self.workspace.host=lambda identity:self.host
        self.workspace.save_workspace=lambda:None
        with patch.object(self.controller,'start'):
            result=self.controller.bind_profile(self.host['id'],self.source['feeds'])
        self.assertEqual(result['hosts'][0]['result']['status'],'queued')
        self.assertEqual(self.controller.jobs,[(self.host['id'],'sync',None)])
        self.assertEqual(storage.read_json(self.game/'sdgo/client-source.json')['schema_version'],3)

    def test_initial_manifest_resolution_reports_progress_and_can_be_cancelled(self):
        import threading,time
        self.workspace.host=lambda identity:self.host;self.workspace.save_workspace=lambda:None
        entered=threading.Event();outcome=[]
        def resolve(*_):
            entered.set()
            while not self.controller.operation_stop.is_set():time.sleep(.01)
            raise InterruptedError('cancelled')
        with patch.object(self.controller,'_bind_profile',side_effect=resolve),patch.object(self.controller,'start')as start:
            thread=threading.Thread(target=lambda:outcome.append(self.controller.bind_profile(self.host['id'],self.source['feeds'])))
            thread.start();self.assertTrue(entered.wait(10))
            row=self.controller.state()['hosts'][0]
            self.assertEqual(row['result']['progress']['phase'],'manifest');self.assertEqual(row['pending']['feeds'],self.source['feeds'])
            self.controller.cancel(self.host['id']);thread.join(10)
            self.assertFalse(thread.is_alive());self.assertFalse(self.controller.stop_event.is_set());self.assertTrue(self.host['client_updates_paused'])
            self.assertEqual(outcome[0]['hosts'][0]['result']['status'],'cancelled');start.assert_not_called();self.assertEqual(self.controller.jobs,[])

    def test_profile_does_not_downgrade_the_independently_updated_manager(self):
        from manager.workspace import Workspace
        workspace=Workspace(self.root/'frontend',self.root/'addons');self.addCleanup(workspace.close)
        workspace.settings['hosts']=[self.host]
        executable=self.game/updates.MANAGER;executable.write_bytes(b'newer Manager with download progress')
        frontend=updates.Updates(workspace,self.root/'distribution',executable)
        self.assertFalse(frontend.managed_by_client())
        plan=self.controller.plan(self.host);self.controller.install(self.host,plan['digest'])
        self.assertEqual(executable.read_bytes(),b'newer Manager with download progress')


class ReleaseTransport(unittest.TestCase):
    def test_exact_release_lock_uses_public_release_without_git(self):
        import tempfile,threading
        from types import SimpleNamespace
        from manager import framework_source
        from tests.test_manager_updates import bundle
        raw,asset,api=bundle(release='1.8.4')
        source={'schema_version':1,'kind':'sdgo-framework-runtime','source_id':'b'*32,'repository':updates.REPOSITORY,'channel':'stable','transport':'github-releases'}
        with tempfile.TemporaryDirectory()as temporary:
            controller=SimpleNamespace(cache=Path(temporary),stop_event=threading.Event())
            calls=[]
            def fetch(url,limit,stop=None):
                calls.append(url);return storage.json_bytes(api)if 'api.github.com'in url else raw
            constraints={'minimum_framework':'1.8.4','locked_framework':{'version':'1.8.4','sha256':asset['sha256']}}
            with patch.object(updates,'fetch',side_effect=fetch),patch.object(transfers,'download',side_effect=fixture_download):loaded=framework_source.load(controller,source,constraints)
            self.assertEqual(loaded['version'],'1.8.4');self.assertTrue(calls[0].endswith('/releases/tags/v1.8.4'))
            constraints['locked_framework']['sha256']='0'*64
            with patch.object(updates,'fetch',side_effect=fetch),self.assertRaisesRegex(ValueError,'锁定'):framework_source.load(controller,source,constraints)


if __name__=='__main__':unittest.main()
