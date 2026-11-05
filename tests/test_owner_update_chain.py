"""Real publish/install transactions on owned fixtures, with no real account data."""
import copy
import io
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
import urllib.error
from urllib.parse import urlsplit
import zipfile
from manager import owner_channel as o, updates as u, storage
from manager.workspace import Workspace
from test_manager_updates import fake_exe, bundle, KEY

ROOT=Path(__file__).resolve().parents[1]


class OwnerChainTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve();self.output=self.root/'publication'
        self.workspace=Workspace(self.root/'state',ROOT/'mods')
        self.workspace.settings['services']={o.SERVICE:{'enabled':False}}
        self.addCleanup(self.workspace.close)
        running=patch.object(u,'running',return_value=False);running.start();self.addCleanup(running.stop)
        self.requests=[];self.unavailable=set()
        def fetch(url,limit,stop):
            self.requests.append(url)
            if urlsplit(url).hostname in self.unavailable:raise urllib.error.URLError('fixture offline')
            relative=urlsplit(url).path.removeprefix('/packs/')
            raw=(self.output/relative).read_bytes()
            if len(raw)>limit:raise ValueError('fixture download budget')
            return raw
        self.controller=o.OwnerChannels(self.workspace,fetch)
        self.addCleanup(self.controller.stop)
        self.owner=self.host('owner');self.player=self.host('player')
        self.mod(self.owner,'example')
        self.source_cfg={'directory':str(self.output),'label':'Example server',
                         'feeds':['https://primary.example/packs/owner-feed.json'],
                         'extras':[],'git_remote':'','branch':'main'}
        self.controller.configure(self.owner['id'],{'mode':'owner','publication':self.source_cfg})

    def host(self,name):
        root=self.root/name;fake_exe(root/'GOnline.exe')
        (root/'GPackData.zpk').write_bytes(b'compatible native archive fixture')
        (root/'config.xml').write_bytes(b'private screen/login fixture')
        self.workspace.add_host(root)
        return self.workspace.settings['hosts'][-1]

    def mod(self,host,identity,scope='public'):
        root=Path(host['path']);base=root/'sdgo/mods'/identity;base.mkdir(parents=True)
        (root/'d3d9.dll').write_bytes(b'owner loader');(root/'sdgo/core.dll').write_bytes(b'owner core')
        (root/'sdgo/sdgo.ini').write_bytes(b'[mods]\nexample=1\n')
        symbols=root/'sdgo/symbols';symbols.mkdir(exist_ok=True)
        (symbols/(KEY+'.ini')).write_bytes(b'[host]\nimage=GOnline.exe\n')
        (base/'mod.ini').write_text('[mod]\ndistribution='+scope+'\nid='+identity+'\ndll='+identity+'.dll\nversion=1.0.0\ncategory=gameplay\nfiles=script.lua\nconfig_files=config.ini\n',encoding='utf-8')
        (base/'manager.json').write_bytes(storage.json_bytes({'schema_version':1,'id':identity,'distribution':scope,'title':'Example','hosts':['client'],'settings':[]}))
        (base/(identity+'.dll')).write_bytes(b'owner mod code')
        (base/'script.lua').write_bytes(b'-- owner authored script')
        (base/'config.ini').write_bytes(b'[settings]\nAmount=42\n')
        (root/'sdgo/catalog.json').write_bytes(storage.json_bytes({'schema_version':1,'mods':[{'id':'example'}],'features':{},'categories':{}}))
        return base

    def publish(self):
        plan=self.controller.preview(self.owner['id'])
        result=self.controller.publish(self.owner['id'],plan['digest'])
        self.assertEqual(result['status'],'published');return result

    def bind(self,automatic=False):
        source=o.binding(self.owner['publication'])
        self.controller.configure(self.player['id'],{'mode':'player','source':source,'automatic':automatic})
        return source

    def test_owner_publish_player_update_and_independent_second_revision(self):
        self.publish();source=self.bind()
        personal=(Path(self.player['path'])/'config.xml').read_bytes()
        first=self.controller.check(self.player['id'],True)
        self.assertEqual(first['revision'],1)
        target=Path(self.player['path'])/'sdgo/mods/example'
        self.assertEqual((target/'config.ini').read_bytes(),b'[settings]\nAmount=42\n')
        (Path(self.owner['path'])/'sdgo/mods/example/config.ini').write_bytes(b'[settings]\nAmount=75\n')
        self.publish();second=self.controller.check(self.player['id'],True)
        self.assertEqual(second['revision'],2)
        self.assertEqual((target/'config.ini').read_bytes(),b'[settings]\nAmount=75\n')
        self.assertEqual((Path(self.player['path'])/'config.xml').read_bytes(),personal)
        self.assertEqual(o.installed_binding(self.player['path']),source)
        self.assertNotIn('GOnline.exe',json.loads((Path(self.player['path'])/o.RECEIPT).read_text())['files'])

    def test_diy_can_protect_immediately_without_publishing_configuration(self):
        h=self.host('diy-alone');self.controller.configure(h['id'],{'mode':'owner'})
        self.assertEqual(o.mode(h),'owner')
        with self.assertRaisesRegex(ValueError,'先配置'):self.controller.preview(h['id'])

    def test_official_automatic_update_cannot_overwrite_diy_or_player_binding(self):
        self.publish();self.bind();self.controller.check(self.player['id'],True)
        raw,asset,_=bundle();upstream=self.root/'upstream';manifest=u.unpack(raw,asset,upstream)
        controller=u.Updates(self.workspace,ROOT);controller.bundle=upstream;controller.manifest=manifest
        controller.apply()
        self.assertEqual([r['status'] for r in controller.state()['hosts']],['owner','player'])
        for h in (self.owner,self.player):self.assertEqual((Path(h['path'])/'sdgo/mods/example/example.dll').read_bytes(),b'owner mod code')
        with self.assertRaises(ValueError):controller.runtime_plan(self.player['id'])

    def test_manual_adoption_requires_current_plan_and_retains_independent_config(self):
        raw,asset,_=bundle();upstream=self.root/'upstream';manifest=u.unpack(raw,asset,upstream)
        controller=u.Updates(self.workspace,ROOT);controller.bundle=upstream;controller.manifest=manifest
        plan=controller.runtime_plan(self.owner['id'])
        path=Path(self.owner['path'])/'sdgo/mods/example/config.ini';saved=path.read_bytes()
        (Path(self.owner['path'])/'sdgo/mods/example/example.dll').write_bytes(b'changed after review')
        with self.assertRaises(storage.Conflict):controller.adopt(self.owner['id'],['example'],plan['digest'])
        plan=controller.runtime_plan(self.owner['id']);controller.adopt(self.owner['id'],['example'],plan['digest'])
        self.assertEqual(path.read_bytes(),saved)
        self.assertEqual((Path(self.owner['path'])/'d3d9.dll').read_bytes(),b'owner loader')
        self.assertEqual((Path(self.owner['path'])/'sdgo/mods/example/example.dll').read_bytes(),b'new mod')

    def test_public_readback_is_required_and_can_retry_without_new_revision(self):
        self.unavailable.add('primary.example');plan=self.controller.preview(self.owner['id'])
        with self.assertRaises(ValueError):self.controller.publish(self.owner['id'],plan['digest'])
        self.assertNotIn('published_revision',self.owner)
        self.unavailable.clear();result=self.controller.verify_publish(self.owner['id'])
        self.assertEqual(result['revision'],1)

    def test_owner_change_after_review_cannot_publish_stale_preview(self):
        plan=self.controller.preview(self.owner['id'])
        (Path(self.owner['path'])/'sdgo/mods/example/script.lua').write_bytes(b'new DIY')
        with self.assertRaises(storage.Conflict):self.controller.publish(self.owner['id'],plan['digest'])
        self.assertFalse((self.output/'owner-feed.json').exists())

    def test_generated_resources_are_hash_bound_without_exporting_install_receipts(self):
        folder=Path(self.owner['path'])/'sdgo/mods/example';asset=folder/'data.bin';asset.write_bytes(b'generated data')
        (folder/'manifest.json').write_bytes(storage.json_bytes({'files':{'data.bin':u.digest(asset.read_bytes())}}))
        self.publish();self.bind();self.controller.check(self.player['id'],True)
        target=Path(self.player['path'])/'sdgo/mods/example';self.assertEqual((target/'data.bin').read_bytes(),b'generated data')
        asset.write_bytes(b'tampered generated data')
        with self.assertRaises(ValueError):self.controller.preview(self.owner['id'])

    def test_private_mod_names_and_data_do_not_enter_published_manifest(self):
        private=self.mod(self.owner,'personal_addon','local_only')
        (private/'personal_addon.dll').write_bytes(distribution_marker:=__import__('manager.distribution',fromlist=['']).NONREDISTRIBUTABLE_MARKER)
        result=self.publish();feed=json.loads((self.output/'owner-feed.json').read_text())
        with zipfile.ZipFile(self.output/feed['asset'])as z:
            self.assertFalse(any('personal_addon' in n for n in z.namelist()))
            self.assertNotIn(b'personal_addon',z.read('owner-release.json'))
            self.assertNotIn(distribution_marker,z.read('sdgo/mods/example/example.dll'))

    def test_credentials_are_refused_before_publication(self):
        (Path(self.owner['path'])/'sdgo/mods/example/config.ini').write_bytes(b'[settings]\nPassword=never publish\n')
        with self.assertRaises(ValueError):self.controller.preview(self.owner['id'])
        self.assertFalse(self.output.exists())

    def test_player_binding_survives_a_new_workspace_and_unavailable_owner_never_uses_official(self):
        self.publish();self.bind();self.controller.check(self.player['id'],True)
        other=Workspace(self.root/'other',ROOT/'mods');self.addCleanup(other.close);other.add_host(self.player['path'])
        self.assertEqual(o.mode(other.settings['hosts'][0]),'player')
        self.requests.clear();self.unavailable.add('primary.example')
        with self.assertRaises(ValueError):self.controller.check(self.player['id'],True)
        self.assertTrue(self.requests and all('primary.example'in x for x in self.requests))

    def test_mirror_fallback_is_bound_to_same_publisher(self):
        cfg=copy.deepcopy(self.source_cfg);cfg['feeds'].append('https://backup.example/packs/owner-feed.json')
        self.controller.configure(self.owner['id'],{'mode':'owner','publication':cfg});self.publish();self.bind()
        self.unavailable.add('primary.example');self.controller.check(self.player['id'],True)
        self.assertTrue(any('backup.example'in x for x in self.requests))

    def test_wrong_publisher_and_revision_reuse_are_refused(self):
        self.publish();self.bind();self.controller.check(self.player['id'],True)
        feed=json.loads((self.output/'owner-feed.json').read_text());bad={**feed,'publisher_id':'0'*32}
        (self.output/'owner-feed.json').write_bytes(storage.json_bytes(bad))
        with self.assertRaises(ValueError):self.controller.check(self.player['id'],True)
        (self.output/'owner-feed.json').write_bytes(storage.json_bytes(feed))
        cache,m=self.controller.downloads[self.player['id']];m=copy.deepcopy(m)
        path=cache/'sdgo/mods/example/script.lua';path.write_bytes(b'different same revision')
        m['files']['sdgo/mods/example/script.lua']={'bytes':path.stat().st_size,'sha256':u.digest(path.read_bytes())}
        with self.assertRaises(ValueError):o.install(self.player,cache,m,self.controller.cache)

    def test_corrupt_cache_and_incompatible_base_cannot_mutate_player(self):
        self.publish();self.bind();self.controller.check(self.player['id'])
        cache,m=self.controller.downloads[self.player['id']]
        dll=cache/'sdgo/mods/example/example.dll';dll.write_bytes(b'corrupt')
        with self.assertRaises(ValueError):o.install(self.player,cache,m,self.controller.cache)
        self.assertFalse((Path(self.player['path'])/'d3d9.dll').exists())
        self.controller.check(self.player['id'])
        (Path(self.player['path'])/'GPackData.zpk').write_bytes(b'incompatible')
        with self.assertRaises(ValueError):o.install(self.player,cache,m,self.controller.cache)
        self.assertFalse((Path(self.player['path'])/'d3d9.dll').exists())

    def test_open_game_waits_and_write_failure_rolls_back_all_runtime_changes(self):
        self.publish();self.bind();self.controller.check(self.player['id'])
        cache,m=self.controller.downloads[self.player['id']]
        self.assertEqual(o.install(self.player,cache,m,self.controller.cache,lambda _:True)['status'],'waiting')
        actual=storage.atomic;root=Path(self.player['path'])
        def fail(path,raw):
            if path==root/'sdgo/core.dll':raise OSError('fixture write denied')
            actual(path,raw)
        with patch.object(storage,'atomic',side_effect=fail),self.assertRaises(OSError):o.install(self.player,cache,m,self.controller.cache)
        self.assertFalse((root/'d3d9.dll').exists());self.assertFalse((root/o.RECEIPT).exists())
        self.assertTrue((root/o.BINDING).exists())

    def test_retirement_cannot_delete_unmanaged_or_locally_changed_files(self):
        self.publish();self.bind();self.controller.check(self.player['id'],True)
        root=Path(self.player['path']);custom=root/'sdgo/mods/unrelated/extra.bin';custom.parent.mkdir(parents=True);custom.write_bytes(b'keep')
        held=root/'sdgo/mods/example/script.lua';held.write_bytes(b'local edit')
        cfg=Path(self.owner['path'])/'sdgo/mods/example/mod.ini';cfg.write_text(cfg.read_text().replace('files=script.lua','files='))
        self.publish()
        with self.assertRaises(storage.Conflict):self.controller.check(self.player['id'],True)
        self.assertEqual(custom.read_bytes(),b'keep');self.assertEqual(held.read_bytes(),b'local edit')

    def test_invalid_configuration_keeps_old_binding_and_explicit_unbind_preserves_runtime(self):
        self.publish();self.bind();self.controller.check(self.player['id'],True)
        root=Path(self.player['path']);before=(root/o.BINDING).read_bytes()
        with self.assertRaises(ValueError):self.controller.configure(self.player['id'],{'mode':'owner','publication':{'directory':'relative'}})
        self.assertEqual((root/o.BINDING).read_bytes(),before)
        self.controller.configure(self.player['id'],{'mode':'owner'})
        self.assertEqual(o.mode(self.player),'owner');self.assertFalse((root/o.BINDING).exists())
        self.assertEqual((root/'sdgo/mods/example/example.dll').read_bytes(),b'owner mod code')

    def test_broken_binding_blocks_official_updates_but_can_be_explicitly_removed(self):
        root=Path(self.player['path']);(root/'sdgo').mkdir();(root/o.BINDING).write_bytes(b'broken JSON')
        self.assertEqual(o.mode(self.player),'blocked')
        self.controller.configure(self.player['id'],{'mode':'owner'})
        self.assertEqual(o.mode(self.player),'owner')

    def test_custom_github_upstream_does_not_accept_a_different_repository(self):
        raw,asset,api=bundle();api=copy.deepcopy(api)
        api['assets'][0]['browser_download_url']=api['assets'][0]['browser_download_url'].replace(u.REPOSITORY,'example/custom')
        self.assertEqual(u.release_asset(api,'stable','example/custom')['url'],api['assets'][0]['browser_download_url'])
        with self.assertRaises(ValueError):u.release_asset(api)
        with self.assertRaises(ValueError):u.unpack(raw,asset,self.root/'foreign','example/custom')
        c=u.Updates(self.workspace,ROOT);c.source('example/custom')
        self.assertEqual(c.state()['repository'],'example/custom');self.assertEqual(o.mode(self.owner),'owner')

    def test_failed_workspace_save_restores_binding_receipt_and_role(self):
        self.publish();self.bind();self.controller.check(self.player['id'],True)
        root=Path(self.player['path']);before={n:(root/n).read_bytes()for n in (o.BINDING,o.RECEIPT)}
        with patch.object(self.workspace,'save_workspace',side_effect=OSError('fixture save denied')),self.assertRaises(OSError):
            self.controller.configure(self.player['id'],{'mode':'owner'})
        self.assertEqual(o.mode(self.player),'player')
        for name,raw in before.items():self.assertEqual((root/name).read_bytes(),raw)

    def test_server_diy_boundary_does_not_require_player_publication(self):
        server=self.root/'battle';fake_exe(server/'BattleServer.exe');self.workspace.add_host(server)
        host=self.workspace.settings['hosts'][-1]
        self.controller.configure(host['id'],{'mode':'owner'});self.assertEqual(o.mode(host),'owner')
        with self.assertRaises(ValueError):self.controller.configure(host['id'],{'mode':'player'})

    def test_protected_hosts_require_a_protocol_aware_manager_program(self):
        raw,asset,_=bundle();folder=self.root/'old-upstream';manifest=u.unpack(raw,asset,folder)
        exe=self.root/'manager.exe';exe.write_bytes(b'installed manager')
        c=u.Updates(self.workspace,ROOT,manager_exe=exe);c.current='1.0.0';c.bundle=folder;c.manifest=manifest
        with self.assertRaises(ValueError):c.apply()
        self.assertEqual(exe.read_bytes(),b'installed manager')

    def test_newer_manager_can_review_older_upstream_components_without_downgrading_program(self):
        raw,asset,api=bundle()
        def fetch(url,limit,stop):return json.dumps(api).encode() if url==u.LATEST else raw
        exe=self.root/'manager.exe';exe.write_bytes(b'newer installed manager')
        c=u.Updates(self.workspace,ROOT,manager_exe=exe,fetcher=fetch);c.current='2.0.0'
        c.cycle(False);self.assertIsNotNone(c.manifest)
        self.assertTrue(c.runtime_plan(self.owner['id'])['components'])
        c.apply();self.assertFalse(c.state()['manager_restart'])
        self.assertEqual(exe.read_bytes(),b'newer installed manager')

    def test_private_global_toggles_stay_local_while_shared_values_follow_owner(self):
        original=Path(self.owner['path'])/o.GLOBAL
        original.write_bytes(b'[mods]\nexample=1\npersonal_addon=1\n[personal]\npassword=private setting\n')
        self.publish();source=self.bind();root=Path(self.player['path'])
        (root/o.GLOBAL).write_bytes(b'[mods]\nexample=0\npersonal_addon=0\n[personal]\nNick=keep\n')
        self.controller.check(self.player['id'],True)
        cfg=u.read_ini((root/o.GLOBAL).read_bytes())
        self.assertEqual(cfg['mods']['example'],'1');self.assertEqual(cfg['mods']['personal_addon'],'0');self.assertEqual(cfg['personal']['Nick'],'keep')
        self.assertEqual(self.controller.check(self.player['id'],audit_only=True)['status'],'current')
        feed=json.loads((self.output/'owner-feed.json').read_text())
        with zipfile.ZipFile(self.output/feed['asset'])as z:
            public=z.read(o.GLOBAL);self.assertNotIn(b'personal_addon',public);self.assertNotIn(b'private setting',public)

    def test_diy_maintainer_marker_survives_new_workspace_and_blocks_player_shortcut(self):
        root=Path(self.owner['path']);(root/o.BINDING).write_bytes(storage.json_bytes(o.binding(self.owner['publication'])))
        self.assertEqual(o.mode({'path':str(root)}),'owner')
        with self.assertRaises(ValueError):o.sync_client(root)

    def test_automatic_binding_runs_in_registered_lifecycle_and_can_be_disabled_per_client(self):
        self.publish();self.bind(automatic=True)
        receipt=Path(self.player['path'])/o.RECEIPT;end=time.monotonic()+6
        while not receipt.exists() and time.monotonic()<end:time.sleep(.05)
        self.assertTrue(receipt.exists());self.assertTrue(self.controller.state()['running'])
        self.controller.automatic(self.player['id'],False)
        row=next(h for h in self.controller.state()['hosts']if h['id']==self.player['id']);self.assertFalse(row['automatic'])


if __name__=='__main__':unittest.main()
