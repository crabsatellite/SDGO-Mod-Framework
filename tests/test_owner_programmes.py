from pathlib import Path
import copy
import json
import unittest
from unittest.mock import patch
from manager import distribution, owner_programmes as p, server_profiles, storage, updates
from tests import test_server_profiles as fixtures


class OwnerProgrammes(unittest.TestCase):
    git = fixtures.Profiles.git
    write = fixtures.Profiles.write
    commit_all = fixtures.Profiles.commit_all

    def setUp(self):
        fixtures.Profiles.setUp(self)
        self.framework['manifest'].setdefault('capabilities', {})['owner_programmes'] = 1
        self.owner = self.game/'sdgo/owner-mods/owner_fix'
        self.owner.mkdir(parents=True)
        (self.owner/'owner_fix.dll').write_bytes(b'fixture owner DLL ' + distribution.OWNER_MARKER)
        (self.owner/'mod.ini').write_bytes(b'[mod]\nid=owner_fix\nname=Owner fix\nversion=0.1.0\ndll=owner_fix.dll\ndistribution=owner\nenabled=1\n')
        with(self.owner/'mod.ini').open('ab')as stream:stream.write(('hosts='+self.profile['base']['host_key']+'\n').encode())
        (self.owner/'manager.json').write_bytes(storage.json_bytes({'schema_version': 1, 'id': 'owner_fix',
            'title': 'Owner fix', 'hosts': ['client'], 'distribution': 'owner', 'settings': []}))
        self.output = self.root/'owner-programme'
        self.lock = copy.deepcopy(self.profile['framework'])
        self.published = p.publish(self.host, {'source_id': self.source['source_id'], 'channel': 'stable',
            'feeds': ['local+'+(self.output/p.MANIFEST).as_uri()]}, self.lock, self.output, ['owner_fix'])
        self.profile.update(schema_version=2,channel='stable',owner_programme=self.published)
        self.save_profile()
        for leaf in self.owner.iterdir(): leaf.unlink()
        self.owner.rmdir()

    def save_profile(self):
        storage.atomic(self.repo/server_profiles.MANIFEST, storage.json_bytes(self.profile))

    def install(self):
        plan = self.controller.plan(self.host)
        self.assertEqual(self.controller.install(self.host, plan['digest'])['status'], 'installed')
        return plan

    def test_programme_framework_and_content_install_in_one_transaction(self):
        plan = self.install()
        self.assertEqual(plan['owner_programme_revision'], 1)
        self.assertTrue((self.owner/'owner_fix.dll').is_file())
        self.assertEqual((self.game/'sprs/new-unit.spr').read_bytes(), b'new custom unit')
        receipt = storage.read_json(self.game/'sdgo/local-state/client-runtime.json')
        self.assertEqual(receipt['owner_programme']['sha256'], self.published['sha256'])
        self.assertEqual(self.controller.plan(self.host)['status'], 'current')

    def test_single_url_binding_preserves_owner_policy_and_refuses_another_channel(self):
        source={**self.source,'manager_policy':'owner','channel':'stable'}
        marker=self.repo/'profile-source.json';storage.atomic(marker,storage.json_bytes(source))
        self.workspace.host=lambda identity:self.host;self.workspace.save_workspace=lambda:None
        with patch.object(self.controller,'start'):
            self.controller.bind_profile(self.host['id'],self.source['feeds'])
        self.assertEqual(storage.read_json(self.game/'sdgo/client-source.json'),source)
        source['channel']='beta';storage.atomic(marker,storage.json_bytes(source))
        with self.assertRaisesRegex(ValueError,'通道'):
            self.controller.bind_profile(self.host['id'],self.source['feeds'])
        self.assertEqual(storage.read_json(self.game/'sdgo/client-source.json')['channel'],'stable')

    def test_content_revision_keeps_the_same_programme_lock(self):
        self.install()
        self.profile['revision'] += 1
        self.save_profile()
        plan = self.install()
        self.assertFalse(any(n.endswith('.dll') for n in plan['files']))
        self.assertEqual(self.controller.plans[self.host['id']]['owner_programme']['source'], self.published)

    def test_owner_profile_receipt_keeps_its_digest_and_rejects_same_revision_rewrites(self):
        self.install()
        receipt=storage.read_json(self.game/'sdgo/local-state/client-runtime.json')
        expected=updates.digest(storage.json_bytes(self.profile))
        self.assertEqual(receipt['profile']['sha256'],expected)
        self.assertEqual(receipt['content_commit'],expected)
        self.profile['label']='Changed bytes without a new revision';self.save_profile()
        with self.assertRaisesRegex(ValueError,'同一修订号'):
            self.controller.plan(self.host)

    def test_foreign_publisher_wrong_framework_and_tampering_fail_before_install(self):
        for key, value in [('source_id', 'f'*32), ('sha256', '0'*64)]:
            with self.subTest(key=key):
                self.profile['owner_programme'] = {**self.published, key: value}
                self.save_profile()
                with self.assertRaises(ValueError): self.controller.plan(self.host)
                self.assertFalse((self.game/'d3d9.dll').exists())
        self.profile['owner_programme'] = self.published
        self.profile['framework'] = {**self.lock, 'version': '9.9.9'}
        self.save_profile()
        with self.assertRaises(ValueError): self.controller.plan(self.host)

    def test_local_only_bytes_never_enter_owner_or_public_distribution(self):
        with self.assertRaises(ValueError):
            distribution.require_owner_payload('owner.dll', distribution.OWNER_MARKER+distribution.NONREDISTRIBUTABLE_MARKER)
        with self.assertRaises(ValueError):
            distribution.require_redistributable('owner.dll', distribution.OWNER_MARKER)
        with self.assertRaises(ValueError):
            distribution.require_owner_payload('owner.dll', b'unmarked DLL')

    def test_content_still_rejects_programmes_and_owner_metadata(self):
        for leaf in ('owner_fix.dll', 'mod.ini', 'manager.json'):
            with self.assertRaises(ValueError): server_profiles.content_name(p.ROOT+'owner_fix/'+leaf)

    def test_old_profile_format_cannot_silently_ignore_owner_programmes(self):
        self.profile['schema_version'] = 1
        self.save_profile()
        with self.assertRaisesRegex(ValueError, '旧 Manager'): self.controller.plan(self.host)

    def test_programme_requires_a_capable_official_core(self):
        del self.framework['manifest']['capabilities']['owner_programmes']
        with self.assertRaisesRegex(ValueError, '官方基线'): self.controller.plan(self.host)

    def test_unknown_loadable_mods_are_preserved_in_recovery_and_stop_loading(self):
        extra = self.game/'sdgo/local-mods/unknown'
        extra.mkdir(parents=True)
        (extra/'mod.ini').write_bytes(b'[mod]\nid=unknown\ndll=unknown.dll\n')
        (extra/'unknown.dll').write_bytes(b'unknown original bytes')
        (self.game/'notes.txt').write_text('personal note')
        plan = self.install()
        self.assertIn('sdgo/local-mods/unknown/mod.ini', plan['quarantined'])
        self.assertFalse((extra/'mod.ini').exists())
        receipt = storage.read_json(self.game/'sdgo/local-state/client-runtime.json')
        saved = Path(receipt['backup'])/'sdgo/local-mods/unknown/unknown.dll'
        self.assertEqual(saved.read_bytes(), b'unknown original bytes')
        self.assertEqual((self.game/'notes.txt').read_text(), 'personal note')

    def test_failed_commit_restores_programmes_content_and_unknown_mod(self):
        (self.game/'sdgo/local-mods/unknown').mkdir(parents=True)
        (self.game/'sdgo/local-mods/unknown/mod.ini').write_bytes(b'[mod]\nid=unknown\n')
        plan = self.controller.plan(self.host)
        before = {f.relative_to(self.game).as_posix(): f.read_bytes() for f in self.game.rglob('*') if f.is_file()}
        original = __import__('shutil').copyfile
        def copy(src, dst, *args, **kwargs):
            if Path(dst).name.startswith('owner_fix.dll.'): raise OSError('fixture owner commit failure')
            return original(src, dst, *args, **kwargs)
        with patch('manager.client_runtime.shutil.copyfile', side_effect=copy), self.assertRaises(OSError):
            self.controller.install(self.host, plan['digest'])
        after = {f.relative_to(self.game).as_posix(): f.read_bytes() for f in self.game.rglob('*') if f.is_file()}
        self.assertEqual(before, after)

    def test_no_duplicate_official_mod_identity(self):
        manifest = json.loads((self.output/p.MANIFEST).read_bytes())
        manifest['mods'] = {'example': '0.1.0'}
        source = {**self.published, 'mods': manifest['mods']}
        with self.assertRaises(ValueError): p.validate(manifest, source, self.lock)

    def test_advancing_offline_channel_preserves_old_pinned_revision(self):
        value=json.loads((self.output/p.MANIFEST).read_bytes())
        self.owner.mkdir(parents=True)
        for path in value['files']:
            (self.owner/Path(path).name).write_bytes((self.output/p.PREFIX/path).read_bytes())
        raw=(self.owner/'mod.ini').read_bytes().replace(b'version=0.1.0',b'version=0.1.1')
        (self.owner/'mod.ini').write_bytes(raw)
        latest=p.publish(self.host,{'source_id':self.source['source_id'],'channel':'stable',
            'feeds':['local+'+(self.output/p.MANIFEST).as_uri()]},self.lock,self.output,['owner_fix'])
        self.assertEqual(latest['revision'],2)
        self.assertNotEqual(latest['feeds'],self.published['feeds'])
        old=p.load(self.controller,self.published,self.lock)
        self.assertEqual(old['manifest']['mods']['owner_fix'],'0.1.0')

    def test_public_archive_rejects_owner_directory_with_stripped_markers(self):
        import io,zipfile
        stream=io.BytesIO()
        with zipfile.ZipFile(stream,'w')as archive:archive.writestr('sdgo/owner-mods/x/x.dll',b'unmarked bytes')
        with self.assertRaises(ValueError):distribution.require_redistributable_archive(stream.getvalue())

    def test_retiring_an_owner_mod_keeps_the_official_baseline_and_content(self):
        self.install()
        latest=p.publish(self.host,{'source_id':self.source['source_id'],'channel':'stable',
            'feeds':['local+'+(self.output/p.MANIFEST).as_uri()]},self.lock,self.output,[])
        self.profile.update(revision=2,owner_programme=latest);self.save_profile()
        self.install()
        self.assertFalse((self.owner/'mod.ini').exists())
        self.assertTrue((self.game/'sdgo/mods/example/example.dll').is_file())
        self.assertTrue((self.game/'sprs/new-unit.spr').is_file())

    def test_owner_programmes_cannot_smuggle_credentials_in_settings(self):
        self.owner.mkdir(parents=True)
        for leaf in ('mod.ini','manager.json','owner_fix.dll'):
            (self.owner/leaf).write_bytes((self.output/p.PREFIX/p.ROOT/'owner_fix'/leaf).read_bytes())
        with (self.owner/'mod.ini').open('ab')as stream:stream.write(b'[settings]\nApiKey=private-fixture-secret\n')
        with self.assertRaisesRegex(ValueError,'私人'):
            p.publish(self.host,{'source_id':self.source['source_id'],'channel':'stable',
                'feeds':['local+'+(self.output/p.MANIFEST).as_uri()]},self.lock,self.output,['owner_fix'])

    def test_selected_content_channel_rejects_a_mixed_mirror(self):
        source={**self.source,'channel':'beta','manager_policy':'owner'}
        storage.atomic(self.game/'sdgo/client-source.json',storage.json_bytes(source))
        with self.assertRaisesRegex(ValueError,'通道'):self.controller.plan(self.host)

    def test_broken_unknown_metadata_is_quarantined_instead_of_blocking_update(self):
        folder=self.game/'sdgo/local-mods/broken';folder.mkdir(parents=True)
        (folder/'mod.ini').write_bytes(b'[mod]\nid=broken\n')
        (folder/'manager.json').write_bytes(b'not valid JSON')
        self.install()
        self.assertFalse((folder/'mod.ini').exists())
        self.assertTrue((self.owner/'owner_fix.dll').exists())

    def test_private_mod_ai_report_stays_with_owner_and_uses_actual_mod_root(self):
        from types import SimpleNamespace
        from manager.ai_context import build
        self.install()
        workspace=SimpleNamespace(host=lambda identity:self.host,contracts={},_mod_directory=lambda host,identity:self.owner,
            _installed=lambda host:[{'id':'owner_fix','title':'Owner fix','version':'0.1.0','enabled':True,'distribution':'owner'}])
        result=build(workspace,'http://127.0.0.1:1',host_id='client',mod_id='owner_fix',kind='bug',problem='fixture bug')
        self.assertIsNone(result['repository']);self.assertNotIn('gh issue create',result['text'])
        self.assertNotIn(str(self.root),result['text'])

    def test_explicit_owner_import_preserves_old_local_scope_outside_loading_root(self):
        import base64,io,zipfile
        from types import SimpleNamespace
        from manager.packages import import_client
        old=self.game/'sdgo/local-mods/owner_fix';old.mkdir(parents=True)
        (old/'mod.ini').write_bytes(b'[mod]\nid=owner_fix\ndistribution=local_only\n')
        (old/'private.note').write_bytes(b'old personal module data')
        archive=io.BytesIO()
        with zipfile.ZipFile(archive,'w')as z:
            for path in json.loads((self.output/p.MANIFEST).read_bytes())['files']:
                z.writestr(path,(self.output/p.PREFIX/path).read_bytes())
        workspace=SimpleNamespace(host=lambda identity:self.host,public=lambda:{})
        result=import_client(workspace,'client',base64.b64encode(archive.getvalue()).decode())
        self.assertTrue((self.owner/'owner_fix.dll').is_file());self.assertFalse(old.exists())
        self.assertEqual((Path(result['scope_recovery'])/'local-mods/owner_fix/private.note').read_bytes(),b'old personal module data')

    def test_publisher_state_with_installed_owner_mods_is_json_serializable(self):
        from manager.workspace import Workspace
        from manager.owner_channel import OwnerChannels
        self.install()
        workspace=Workspace(self.root/'publisher-state',Path(__file__).resolve().parents[1]/'mods')
        self.addCleanup(workspace.close);workspace.settings['hosts']=[self.host]
        controller=OwnerChannels(workspace)
        state=controller.state();storage.json_bytes(state)
        self.assertEqual(state['hosts'][0]['owner_mods'][0]['version'],'0.1.0')
        self.assertIsInstance(state['hosts'][0]['publication'],dict)

    def test_programme_publication_keeps_the_approved_feed_commit(self):
        from types import SimpleNamespace
        self.host.update(runtime_mode='owner',runtime_mode_source='explicit',profile_publication={'source_id':self.source['source_id']})
        self.workspace.host=lambda identity:self.host
        self.workspace.save_workspace=lambda:None
        self.workspace.settings['updates']={'git_source':self.source['framework']}
        self.workspace.updates=SimpleNamespace(latest={k:self.framework[k]for k in('version','sha256','commit')},source_repository=lambda:updates.REPOSITORY)
        directory=self.root/'approved-programmes'
        with patch('manager.framework_source.load',return_value=self.framework)as load:
            self.controller.publish_programmes('client',str(directory),['local+'+(directory/p.MANIFEST).as_uri()],[],channel='stable')
        self.assertEqual(load.call_args.args[2]['locked_framework']['commit'],self.framework['commit'])


if __name__ == '__main__': unittest.main()
