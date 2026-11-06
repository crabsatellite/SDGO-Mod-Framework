"""Safety gates operate before any player-data writer; fixtures own all files."""
from pathlib import Path
import copy
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from manager import dependencies as d, configuration as c, operations as op, storage, runtime_state as runtime
from manager.change_review import ChangeReviews,ReviewRequired
from manager.target_locks import lease
from manager.workspace import Workspace

ROOT=Path(__file__).resolve().parents[1]


def node(identity,enabled=True,**contract):
    return {'id':identity,'version':'1.2.3','enabled':enabled,'host':'client','role':'client','contract':{'id':identity,'title':identity,**contract},'settings':{}}


class Dependencies(unittest.TestCase):
    def test_required_optional_conflict_and_versioned_capability(self):
        consumer=node('consumer',dependencies={'required':[{'id':'provider','minimum':'1.2.0','capability':'example.bridge','capability_version':2}]})
        provider=node('provider',provides={'example.bridge':2})
        self.assertFalse(d.issues([consumer,provider]))
        after=copy.deepcopy([consumer,provider]);after[1]['enabled']=False
        with self.assertRaises(ValueError):d.enforce_transition([consumer,provider],after)
        consumer['contract']['dependencies']={'optional':[{'id':'provider','capability':'example.bridge','capability_version':3}]}
        self.assertFalse(d.issues([consumer]));self.assertTrue(d.issues([consumer,provider]))
        consumer['contract']['dependencies']={'conflicts':[{'id':'provider'}]};self.assertTrue(d.issues([consumer,provider]))
        self.assertGreater(d.version('1.2.3-beta.10'),d.version('1.2.3-beta.2'))
        self.assertGreater(d.version('1.2.3'),d.version('1.2.3-beta.999'))

    def test_cycles_and_different_abi_major_are_refused(self):
        first=node('first',dependencies={'required':[{'id':'second'}]})
        second=node('second',dependencies={'optional':[{'id':'first'}]})
        self.assertTrue(any(p['kind']=='cycle'for p in d.issues([first,second])))
        first['contract']['dependencies']={'required':[{'id':'second','capability':'bridge','capability_version':1}]}
        second['contract']={'id':'second','provides':{'bridge':2}}
        self.assertTrue(d.issues([first,second]))

    def test_condition_and_existing_unrelated_problem(self):
        consumer=node('consumer',dependencies={'required':[{'id':'provider','when':{'setting':'extra','equals':True}}]})
        consumer['settings']={'extra':False};self.assertFalse(d.issues([consumer]))
        after=copy.deepcopy(consumer);after['settings']['extra']=True
        with self.assertRaises(ValueError):d.enforce_transition([consumer],[after])
        d.enforce_transition([after],[copy.deepcopy(after)])

    def test_invalid_or_ambiguous_contract_is_refused(self):
        for rules in ({'required':[{'id':'x'},{'id':'x'}]}, {'required':[{'id':'x'}],'conflicts':[{'id':'x'}]}, {'optional':[{'id':'../x'}]}):
            with self.assertRaises(ValueError):d.validate({'id':'consumer','dependencies':rules})

    def test_render_preserves_case_and_tracks_all_declared_inputs(self):
        contract={'id':'consumer','settings':[{'id':'range','file':'mod.ini','key':'EnemyAggroRange'}],'configuration_files':['script.lua']}
        raw=b'[mod]\nid=consumer\nconfig_files=config.ini\n[settings]\nEnemyAggroRange=2500\n'
        rendered=c.render(raw,contract);self.assertIn(b'EnemyAggroRange=2500',rendered)
        self.assertEqual(c.render(rendered,contract),rendered)
        with tempfile.TemporaryDirectory()as temp:
            root=Path(temp)/'client/sdgo/mods/consumer';root.mkdir(parents=True)
            (root/'mod.ini').write_bytes(rendered);(root/'script.lua').write_bytes(b'-- one')
            names=c.files(storage.ini_values(rendered),contract);before=c.fingerprint(root,names)
            (root/'config.ini').write_bytes(b'[settings]\nValue=2\n')
            self.assertNotEqual(before,c.fingerprint(root,names))
            for name in('../secret','root:private.ini','a\\b','/absolute'):
                with self.assertRaises(ValueError):c.relative(name)


class Maintenance(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.host=self.root/'WorldServer';self.host.mkdir()
        self.exe=self.host/'WorldServer.exe';self.exe.write_bytes(b'fixture executable')
        self.ini=self.host/'WorldServer.ini';self.ini.write_text('[DB Game]\nDBServerName=127.0.0.1,7\nDBName=game\n')
        (self.host/'sdgo').mkdir();(self.host/'sdgo/core.dll').write_bytes(op.CORE_CONTRACT)
        self.environment=patch.dict(os.environ,{'LOCALAPPDATA':str(self.root/'local')});self.environment.start();self.addCleanup(self.environment.stop)

    def test_busy_target_never_enters_mutation_body(self):
        writes=[]
        from contextlib import contextmanager
        @contextmanager
        def busy(_):raise ValueError('native running');yield
        with patch.object(op,'unmapped_image',busy),self.assertRaises(ValueError):
            with op.server_change(self.ini,'example'):writes.append('SQL')
        self.assertFalse(writes)
        self.assertFalse((self.host/'sdgo/local-state').exists())

    def test_only_explicit_server_layout_groups_siblings(self):
        unrelated=self.root/'unrelated';unrelated.mkdir();(unrelated/'BattleServer.exe').write_bytes(b'other fixture')
        group,nodes,_=op.topology(self.ini)
        self.assertEqual(group,self.host);self.assertEqual(nodes,[self.exe])
        server=self.root/'Server';world=server/'WorldServer';battle=server/'BattleServer2'
        world.mkdir(parents=True);battle.mkdir();(world/'WorldServer.exe').write_bytes(b'fixture');(battle/'BattleServer.exe').write_bytes(b'fixture')
        ini=world/'WorldServer.ini';shutil.copy2(self.ini,ini)
        group,nodes,_=op.topology(ini);self.assertEqual(group,server);self.assertEqual(len(nodes),2)

    def test_committed_data_is_not_misreported_as_rolled_back_on_later_failure(self):
        identity=None
        with self.assertRaises(OSError):
            with op.server_change(self.ini,'example')as change:
                identity=change.id;result=change.result({'revision':'r1'})
                self.assertEqual(result['activation'],'pending_restart');raise OSError('workspace disk full')
        receipt=storage.read_json(self.host/'sdgo/local-state/changes'/(identity+'.json'))
        self.assertEqual(receipt['status'],'recovery_required');self.assertTrue(receipt['data_committed'])

    def test_history_backup_is_local_and_budget_failure_has_no_database_writes(self):
        class Cursor:
            description=[('AccountNumber',),('Value',)]
            def execute(self,sql,*args):
                self.sql=sql;return self
            def fetchone(self):return(1,)
            def __iter__(self):return iter([(101,'retained history')])
            def close(self):pass
        class Database:
            def __init__(self):self.calls=[]
            def cursor(self):
                cursor=Cursor();execute=cursor.execute
                def record(sql,*args):self.calls.append(sql);return execute(sql,*args)
                cursor.execute=record;return cursor
        db=Database()
        with op.server_change(self.ini,'history',('schema_migration',))as change:
            op.backup_tables(db,change,['sdgo_collection_capsule.Seen'])
            backup=change.directory/'backups'/change.id
            manifest=storage.read_json(backup/'manifest.json')
            self.assertEqual(manifest['tables'][0]['rows'],1)
            self.assertEqual(json.loads((backup/manifest['tables'][0]['file']).read_text()),[101,'retained history'])
        self.assertTrue(all(sql.startswith('SELECT')for sql in db.calls))
        with self.assertRaises(ValueError):
            with op.server_change(self.ini,'history-budget',('schema_migration',))as change:
                op.backup_tables(db,change,['sdgo_collection_capsule.Seen'],max_bytes=1)
        self.assertTrue(all(sql.startswith('SELECT')for sql in db.calls))

    def test_framework_upgrade_preserves_settings_inventory_resources_and_login(self):
        from test_manager_updates import fake_exe,bundle
        from manager import updates
        workspace=Workspace(self.root/'upgrade-state',ROOT/'mods');self.addCleanup(workspace.close)
        client=self.root/'old-client';fake_exe(client/'GOnline.exe');workspace.add_host(client)
        mod=client/'sdgo/mods/example';mod.mkdir(parents=True)
        (mod/'mod.ini').write_bytes(b'[mod]\nid=example\nversion=1.0.0\nenabled=0\n[settings]\nAmount=91\nCustom=keep\n')
        (mod/'config.ini').write_bytes(b'authored independent config')
        (client/'sdgo/core.dll').write_bytes(b'legacy core');(client/'d3d9.dll').write_bytes(b'old loader')
        protected={'config.xml':b'personal login and display','GPackData.zpk':b'unchanged original archive','sdgo/mods/unit_catalog/catalog.bin':b'already installed definitions','mdrs/existing.mdr':b'authored resource'}
        for name,raw in protected.items():path=client/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(raw)
        raw=c.render(b'[mod]\nid=example\ndistribution=public\nversion=2.0.0\nenabled=1\n[settings]\nAmount=2\n',{'id':'example'})
        blob,asset,_=bundle(extra={'client/sdgo/mods/example/mod.ini':raw});stage=self.root/'upgrade-package';manifest=updates.unpack(blob,asset,stage)
        with patch('manager.database.connect',side_effect=AssertionError('upgrade must not write SQL')):
            result=updates.install_host(workspace.settings['hosts'][0],stage,manifest,self.root/'upgrade-cache',is_running=lambda _:False)
        self.assertEqual(result['status'],'installed')
        for name,raw in protected.items():self.assertEqual((client/name).read_bytes(),raw)
        self.assertEqual((mod/'config.ini').read_bytes(),b'authored independent config')
        values=storage.ini_values((mod/'mod.ini').read_bytes())
        self.assertEqual(values.get('settings','amount'),'91');self.assertEqual(values.get('settings','custom'),'keep')
        self.assertEqual(values.get('mod','enabled'),'0')

    def test_review_binds_exact_payload_and_is_one_use(self):
        workspace=Workspace(self.root/'state',ROOT/'mods');self.addCleanup(workspace.close)
        provider=type('P',(),{'id':'operators','config':{'server_ini':str(self.ini)}})()
        controller=ChangeReviews(workspace);headers={'X-Catalog-Revision':'r1'};body={'amount':2}
        with self.assertRaises(ReviewRequired)as required:
            with controller.execute(provider,'POST','/api/operators/apply',body,headers):self.fail('writer entered')
        token=required.exception.plan['review'];self.assertTrue(required.exception.plan['changes_player_assets'])
        with self.assertRaises(ReviewRequired):
            with controller.execute(provider,'POST','/api/operators/apply',{'amount':3},{**headers,'X-Change-Review':token}):self.fail('stale review entered')
        with controller.execute(provider,'POST','/api/operators/apply',body,{**headers,'X-Change-Review':token}):pass
        with self.assertRaises(ReviewRequired):
            with controller.execute(provider,'POST','/api/operators/apply',body,{**headers,'X-Change-Review':token}):self.fail('review replayed')

    def test_content_cannot_be_disabled_through_settings_or_category(self):
        workspace=Workspace(self.root/'content-state',ROOT/'mods');self.addCleanup(workspace.close)
        client=self.root/'client';client.mkdir();(client/'GOnline.exe').write_bytes(b'owned fixture')
        mod=client/'sdgo/mods/unit_catalog';mod.mkdir(parents=True)
        shutil.copy2(ROOT/'mods/unit_catalog/manager.json',mod/'manager.json')
        raw=b'[mod]\nid=unit_catalog\nenabled=1\ncategory=gameplay\n[settings]\nEnabled=1\n'
        (mod/'mod.ini').write_bytes(raw);(mod/'definitions.json').write_bytes(b'{"units":[1]}')
        workspace.add_host(client);host=workspace.settings['hosts'][0]['id']
        details=workspace.details(host,'unit_catalog')
        with self.assertRaises(ValueError):workspace.configure(host,'unit_catalog',{'enabled':False},details['revisions'])
        state=workspace.public()['hosts'][0];entry=next(m for m in state['mods']if m['id']=='unit_catalog')
        with self.assertRaises(ValueError):workspace.toggle(host,'unit_catalog',False,{'global':state['global_revision'],'manifest':entry['revision']})
        with self.assertRaises(ValueError):workspace.category(host,'gameplay',False,state['global_revision'])
        self.assertEqual((mod/'mod.ini').read_bytes(),raw)

    def test_remote_or_old_core_is_refused_without_entering_writer(self):
        self.ini.write_text('[DB Game]\nDBServerName=remote.invalid\nDBName=fixture\n')
        with self.assertRaises(ValueError):
            with op.server_change(self.ini,'example'):self.fail('remote writer entered')
        self.ini.write_text('[DB Game]\nDBServerName=127.0.0.1,7\nDBName=fixture\n')
        (self.host/'sdgo/core.dll').write_bytes(b'legacy core')
        with self.assertRaises(ValueError):
            with op.server_change(self.ini,'example'):self.fail('unverifiable activation allowed')

    def test_legacy_activation_never_queries_a_database_or_mutates_existing_files(self):
        before={p:p.read_bytes()for p in self.host.rglob('*')if p.is_file()}
        with patch('manager.database.connect',side_effect=AssertionError('unexpected SQL')):
            self.assertEqual(op.activation(None,None)['status'],'legacy')
            workspace=Workspace(self.root/'state',ROOT/'mods');workspace.add_host(self.host)
            workspace.public();workspace.close()
        self.assertEqual(before,{p:p.read_bytes()for p in self.host.rglob('*')if p.is_file()})

    def test_partial_restart_and_stale_log_cannot_mark_change_active(self):
        battle=self.root/'BattleServer2';battle.mkdir();exe=battle/'BattleServer.exe';exe.write_bytes(b'fixture')
        log=battle/'LogData/start.txt';log.parent.mkdir();marker=b'Table Download OK.. Time Procedure Ready..';log.write_bytes(marker)
        change={'committed_ns':100,'log_offsets':{str(log):len(marker)}}
        process={'pid':1,'path':str(exe),'started_ns':101,'creation_ticks':'101','listening':True}
        with patch.object(runtime,'process_state',return_value=[process]),patch.object(runtime,'receipt',return_value={'mods':{}}):
            self.assertFalse(runtime.native_activation(exe,change)['ready'])
            with log.open('ab')as stream:stream.write(b'\n'+marker)
            self.assertTrue(runtime.native_activation(exe,change)['ready'])
            process['started_ns']=99;self.assertFalse(runtime.native_activation(exe,change)['ready'])

    def test_cross_process_target_lease(self):
        identity=str(self.root/'target');stop=self.root/'stop-child'
        program="from manager.target_locks import lease;from pathlib import Path;import sys,time;\nwith lease(sys.argv[1]):\n print('ready',flush=True)\n while not Path(sys.argv[2]).exists():time.sleep(.02)"
        process=subprocess.Popen([sys.executable,'-B','-c',program,identity,str(stop)],cwd=ROOT,stdout=subprocess.PIPE,text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(),'ready')
            with self.assertRaises(ValueError):
                with lease(identity):self.fail('second workspace acquired target')
        finally:
            stop.touch()
            try:process.wait(timeout=30)
            except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=30)
            process.stdout.close()
        with lease(identity):pass

    @unittest.skipUnless(os.name=='nt','Windows image mapping barrier')
    def test_real_image_mapping_blocks_writes_and_start_is_blocked_during_commit(self):
        import ctypes
        from ctypes import wintypes as w
        shutil.copy2(Path(os.environ['SystemRoot'])/'System32/ping.exe',self.exe)
        original=hashlib.sha256(self.exe.read_bytes()).hexdigest()
        k=ctypes.WinDLL('kernel32',use_last_error=True)
        k.CreateFileW.argtypes=(w.LPCWSTR,w.DWORD,w.DWORD,w.LPVOID,w.DWORD,w.DWORD,w.HANDLE);k.CreateFileW.restype=w.HANDLE
        k.CreateFileMappingW.argtypes=(w.HANDLE,w.LPVOID,w.DWORD,w.DWORD,w.DWORD,w.LPCWSTR);k.CreateFileMappingW.restype=w.HANDLE
        k.MapViewOfFile.argtypes=(w.HANDLE,w.DWORD,w.DWORD,w.DWORD,ctypes.c_size_t);k.MapViewOfFile.restype=w.LPVOID
        k.UnmapViewOfFile.argtypes=(w.LPCVOID,);k.CloseHandle.argtypes=(w.HANDLE,)
        file=k.CreateFileW(str(self.exe),0x80000000,7,None,3,0x80,None);self.assertNotEqual(file,ctypes.c_void_p(-1).value)
        section=k.CreateFileMappingW(file,None,0x1000002,0,0,None);self.assertTrue(section)
        view=k.MapViewOfFile(section,4,0,0,0);self.assertTrue(view)
        try:
            with self.assertRaises(ValueError):
                with op.server_change(self.ini,'example'):self.fail('running image accepted')
        finally:k.UnmapViewOfFile(view);k.CloseHandle(section);k.CloseHandle(file)
        with op.server_change(self.ini,'example'):
            self.assertEqual(hashlib.sha256(self.exe.read_bytes()).hexdigest(),original)
            with self.assertRaises(OSError):subprocess.Popen([str(self.exe),'-n','1','127.0.0.1'],creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(hashlib.sha256(self.exe.read_bytes()).hexdigest(),original)


if __name__=='__main__':unittest.main()
