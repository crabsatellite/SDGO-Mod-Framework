import copy
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import threading
import time
import unittest
from unittest import mock
import urllib.request
import urllib.error
import zipfile

from manager import updates as u, storage
from manager.workspace import Workspace
from manager.server import make_server
from manager.update_restart import finish

ROOT=Path(__file__).resolve().parents[1]
KEY='GOnline-54BE1568-00C97000'


def fake_exe(path):
    data=bytearray(256);struct.pack_into('<I',data,0x3c,0x40)
    data[0x40:0x44]=b'PE\0\0';struct.pack_into('<I',data,0x48,0x54be1568)
    struct.pack_into('<I',data,0x40+24+56,0xc97000)
    path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)


def bundle(channel='stable',release='1.1.7',extra=None):
    mod={'id':'example','feature':'example','target':'client','category':'fixes',
         'hosts':[KEY],'version':'2.0.0','channel':channel,'files':['manager.json','config.ini'],
         'config_files':['config.ini'],'runtime_files':[]}
    catalog={'schema_version':1,'mods':[mod],'features':{'example':{'category':'fixes','mods':['example']}},'categories':{}}
    payload={u.MANAGER:b'new manager', 'client/d3d9.dll':b'new loader','client/sdgo/core.dll':b'new core',
             'client/sdgo/sdgo.ini':b'[core]\nlog=info\n',
             'client/sdgo/symbols/'+KEY+'.ini':b'[host]\nname=GOnline\n[new.alias]\nrva=7\n',
             'client/sdgo/catalog.json':storage.json_bytes(catalog),'server/sdgo/catalog.json':storage.json_bytes(catalog),
             'server/winmm.dll':b'server loader','server/sdgo/core.dll':b'server core','server/sdgo/sdgo.ini':b'[core]\nlog=info\n',
             'client/sdgo/mods/example/example.dll':b'new mod',
             'client/sdgo/mods/example/mod.ini':b'[mod]\nid=example\nversion=2.0.0\nenabled=1\n[settings]\nAmount=2\nNewOption=7\n',
             'client/sdgo/mods/example/manager.json':b'{}',
             'client/sdgo/mods/example/config.ini':b'[settings]\nvalue=default\n'}
    payload.update(extra or {})
    manifest={'schema_version':1,'repository':u.REPOSITORY,'version':release,'channel':channel,'source_commit':'a'*40,
              'files':{n:{'bytes':len(b),'sha256':u.digest(b)} for n,b in payload.items()}}
    output=io.BytesIO()
    with zipfile.ZipFile(output,'w') as z:
        z.writestr('release.json',storage.json_bytes(manifest))
        for n,b in payload.items():z.writestr(n,b)
    raw=output.getvalue();name='SDGO-ModFramework-'+release+'.zip'
    asset={'version':release,'channel':channel,'bytes':len(raw),'sha256':u.digest(raw),
           'url':'https://github.com/'+u.REPOSITORY+'/releases/download/v'+release+'/'+name,
           'page':'https://github.com/'+u.REPOSITORY+'/releases/tag/v'+release,'notes':'Test release'}
    api={'tag_name':'v'+release,'draft':False,'prerelease':channel=='beta','body':'Test release',
         'assets':[{'name':name,'state':'uploaded','size':len(raw),'digest':'sha256:'+u.digest(raw),'browser_download_url':asset['url']}]}
    return raw,asset,api


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.workspace=Workspace(self.root/'workspace',ROOT/'mods')
        self.host=self.root/'client';fake_exe(self.host/'GOnline.exe')
        self.workspace.add_host(self.host);self.record=self.workspace.settings['hosts'][0]
        self.raw,self.asset,self.api=bundle()
        self.bundle=self.root/'bundle';self.manifest=u.unpack(self.raw,self.asset,self.bundle)

    def existing(self):
        base=self.host/'sdgo/mods/example';base.mkdir(parents=True)
        (self.host/'d3d9.dll').write_bytes(b'old loader');(self.host/'sdgo/core.dll').write_bytes(b'old core')
        (base/'mod.ini').write_bytes(b'[mod]\nid=example\nversion=1.0.0\nenabled=0\n[settings]\nAmount=91\nCustom=keep\n')
        (base/'example.dll').write_bytes(b'old mod');(base/'config.ini').write_bytes(b'private config')
        (self.host/'sdgo/sdgo.ini').write_bytes(b'[mods]\ncustom=1\n')
        (self.host/'config.xml').write_bytes(b'personal screen settings')
        symbols=self.host/'sdgo/symbols';symbols.mkdir()
        (symbols/(KEY+'.ini')).write_bytes(b'[custom.alias]\nrva=99\n')
        (self.host/'sdgo/catalog.json').write_bytes(storage.json_bytes({'mods':[{'id':'private_mod'}],'features':{},'categories':{}}))
        return base

    def controller(self,release_api=None,archive=None):
        def fetch(url,limit,stop):return json.dumps(release_api or self.api).encode() if url in (u.LATEST,u.RELEASES) else (archive or self.raw)
        c=u.Updates(self.workspace,ROOT,fetcher=fetch)
        self.addCleanup(c.stop);return c

    def test_stable_is_default_and_rejects_prerelease(self):
        c=self.controller();self.assertEqual(c.state()['channel'],'stable')
        _,_,api=bundle('beta','1.1.7-beta.1')
        with self.assertRaises(ValueError):u.release_asset(api)
        self.assertEqual(u.release_asset(api,'beta')['channel'],'beta')
        self.assertGreater(u.version('1.1.7'),u.version('1.1.7-beta.90'))
        self.assertGreater(u.version('1.1.8-beta.1'),u.version('1.1.7'))

    def test_beta_selects_newest_valid_release_including_stable(self):
        raw,asset,api=bundle('beta','1.1.8-beta.3')
        c=self.controller([self.api,dict(api,draft=True),api],raw)
        self.workspace.settings['updates']={'channel':'beta'}
        c.cycle(False)
        self.assertEqual(c.state()['latest_version'],'1.1.8-beta.3')
        self.assertFalse((self.host/'sdgo').exists())

    def test_beta_prefers_extra_features_at_same_core_but_newer_stable_wins(self):
        raw,_,beta=bundle('beta','1.1.7-beta.2')
        c=self.controller([self.api,beta],raw)
        self.workspace.settings['updates']={'channel':'beta'}
        c.cycle(False)
        self.assertEqual(c.state()['latest_version'],'1.1.7-beta.2')
        raw,_,stable=bundle('stable','1.1.8')
        c=self.controller([beta,stable],raw)
        c.cycle(False)
        self.assertEqual(c.state()['latest_version'],'1.1.8')

    def test_foreign_release_url_and_missing_digest_are_rejected(self):
        for field,value in [('browser_download_url','https://example.org/evil.zip'),('digest','')]:
            api=copy.deepcopy(self.api);api['assets'][0][field]=value
            with self.assertRaises(ValueError):u.release_asset(api)

    def test_corrupt_download_is_rejected_before_extraction(self):
        target=self.root/'bad'
        with self.assertRaises(ValueError):u.unpack(self.raw+b'x',self.asset,target)
        self.assertFalse(target.exists())

    def test_traversal_case_duplicates_and_game_originals_are_rejected(self):
        for name in ('client/../escape.dll','client/SDGO/core.dll','client/GOnline.exe','server/data.mdf'):
            raw,asset,_=bundle(extra={name:b'invalid'})
            with self.subTest(name=name),self.assertRaises(ValueError):u.unpack(raw,asset,self.root/'bad')

    def test_stable_payload_cannot_sneak_in_beta(self):
        raw,asset,_=bundle('beta','1.1.7-beta.1');asset['channel']='stable'
        with self.assertRaises(ValueError):u.unpack(raw,asset,self.root/'bad')

    def test_install_preserves_player_settings_custom_mods_and_symbols(self):
        base=self.existing()
        result=u.install_host(self.record,self.bundle,self.manifest,self.root/'cache',lambda _:False)
        cfg=u.read_ini((base/'mod.ini').read_bytes())
        self.assertEqual(cfg['mod']['version'],'2.0.0');self.assertEqual(cfg['mod']['enabled'],'0')
        self.assertEqual(dict(cfg['settings']),{'Amount':'91','NewOption':'7','Custom':'keep'})
        self.assertEqual((base/'config.ini').read_bytes(),b'private config')
        self.assertEqual((self.host/'config.xml').read_bytes(),b'personal screen settings')
        self.assertEqual((self.host/'sdgo/sdgo.ini').read_bytes(),b'[mods]\ncustom=1\n')
        symbols=u.read_ini((self.host/'sdgo/symbols'/(KEY+'.ini')).read_bytes())
        self.assertIn('custom.alias',symbols);self.assertIn('new.alias',symbols)
        self.assertIn('private_mod',[m['id'] for m in json.loads((self.host/'sdgo/catalog.json').read_text())['mods']])
        self.assertEqual(result['status'],'installed')
        again=u.install_host(self.record,self.bundle,self.manifest,self.root/'cache',lambda _:False)
        self.assertEqual(again['changed_files'],0)

    def test_running_game_defers_then_installs_without_stopping_it(self):
        before=(self.host/'GOnline.exe').read_bytes()
        with mock.patch.object(u,'running',return_value=True):
            c=self.controller();c.cycle(True)
        self.assertEqual(c.state()['phase'],'waiting');self.assertFalse((self.host/'sdgo').exists())
        with mock.patch.object(u,'running',return_value=False):c.apply()
        self.assertEqual(c.state()['phase'],'current');self.assertTrue((self.host/'sdgo/release.json').exists())
        self.assertEqual((self.host/'GOnline.exe').read_bytes(),before)

    def test_failed_write_rolls_back_already_written_runtime(self):
        self.existing();original={p:p.read_bytes() for p in self.host.rglob('*') if p.is_file()}
        real=storage.atomic;failed=False
        def fail_once(path,raw):
            nonlocal failed
            if path==self.host/'sdgo/core.dll' and not failed:failed=True;raise PermissionError('locked')
            return real(path,raw)
        with mock.patch.object(storage,'atomic',side_effect=fail_once),self.assertRaises(PermissionError):
            u.install_host(self.record,self.bundle,self.manifest,self.root/'cache',lambda _:False)
        for p,raw in original.items():self.assertEqual(p.read_bytes(),raw,p)
        self.assertFalse((self.host/'sdgo/release.json').exists())

    def test_unknown_game_version_and_external_loader_are_preserved(self):
        (self.host/'d3d9.dll').write_bytes(b'other framework')
        with self.assertRaises(ValueError):u.install_host(self.record,self.bundle,self.manifest,self.root/'cache',lambda _:False)
        self.assertEqual((self.host/'d3d9.dll').read_bytes(),b'other framework')

    def test_cache_tampering_is_rejected_before_host_changes(self):
        c=self.controller();c.cycle(False)
        (c.bundle/'client/d3d9.dll').write_bytes(b'changed')
        with self.assertRaises(ValueError):c.apply()
        self.assertFalse((self.host/'sdgo').exists())

    def test_self_update_swap_preserves_backup_and_checks_previous_binary(self):
        workspace=self.workspace.root;source=workspace/'updates/releases/fixture'/u.MANAGER
        source.parent.mkdir(parents=True);source.write_bytes(b'new exe')
        target=self.root/'manager'/u.MANAGER;target.parent.mkdir();target.write_bytes(b'old exe')
        backup=workspace/'updates/backups/manager/old'/u.MANAGER
        job=workspace/'updates/restart.json'
        value={'schema_version':1,'workspace':str(workspace),'source':str(source),'target':str(target),
               'source_sha256':u.digest(source.read_bytes()),'previous_sha256':u.digest(target.read_bytes()),
               'parent_pid':0,'backup':str(backup)}
        storage.atomic(job,storage.json_bytes(value))
        finish(job,launch=False)
        self.assertEqual(target.read_bytes(),b'new exe');self.assertEqual(backup.read_bytes(),b'old exe');self.assertFalse(job.exists())
        storage.atomic(job,storage.json_bytes(value))
        with self.assertRaises(ValueError):finish(job,launch=False)

    def test_http_update_actions_require_the_manager_session(self):
        self.workspace.updates=self.controller()
        server=make_server(self.workspace);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
        base='http://127.0.0.1:'+str(server.server_port)
        with urllib.request.urlopen(base+'/api/state') as r:token=json.load(r)['session_token']
        for origin in ('https://foreign.invalid',base):
            req=urllib.request.Request(base+'/api/updates',data=b'{"action":"install"}',
                headers={'Content-Type':'application/json','Origin':origin,'X-ModManager-Token':'bad'})
            with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(req)
            self.assertEqual(error.exception.code,403)
            error.exception.close()
        with urllib.request.urlopen(base+'/api/updates') as r:self.assertEqual(json.load(r)['channel'],'stable')


if __name__=='__main__':unittest.main()
