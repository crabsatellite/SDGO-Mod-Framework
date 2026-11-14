import copy
import hashlib
import json
import io
import struct
import zipfile
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from owner.units import model
from owner.units.workspace import Workspace
from unit_import_support import Baseline,Repository,archive,png,publisher,complete


class WorkspaceTests(unittest.TestCase):
    def directed(self,targets,profile='native-three-stage-forms-v1'):
        from unit_import_support import unit
        with zipfile.ZipFile(io.BytesIO(archive())) as source:
            files={n:source.read(n) for n in source.namelist() if n!='gdrs/unit.bin'}
        for identity,target in targets.items():files[f'gdrs/form-{identity}.bin']=unit(identity,target)
        output=io.BytesIO()
        with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as z:
            for name,raw in files.items():z.writestr(name,raw)
        state=self.workspace.inspect('directed.zip',output.getvalue(),self.workspace.public()['revision'])
        return self.workspace.update(state['draft']['id'],{
            'root_unit_id':min(targets),'behavior_profile':profile},state['revision'])

    def test_general_directed_profile_keeps_cycles_and_auxiliary_incoming_nodes(self):
        for targets in ({91001:91002,91002:91003,91003:91002},
                        {91001:91002,91002:91003,91003:91004,91004:91001},
                        {91001:91002,91002:91003,91003:0,91004:91002}):
            with self.subTest(targets=targets):
                state=complete(self.workspace,self.directed(targets,'native-directed-forms-v1'))
                draft=state['draft'];self.assertTrue(draft['can_confirm'],draft['requirements'])
                self.assertEqual(targets,{node['id']:node['transform_target'] for node in draft['form_graph']['nodes']})
                self.assertEqual([91001],[row['id'] for row in draft['forms'] if row['server']['SallyReady']])
                from owner.units.project import export_project
                with tempfile.TemporaryDirectory() as directory:
                    other=Workspace(directory,baseline=Baseline(),repository=Repository(),publisher=publisher)
                    try:
                        imported=other.inspect('directed-project.zip',export_project(self.workspace,draft),other.public()['revision'])['draft']
                        self.assertEqual('native-directed-forms-v1',imported['behavior_profile'])
                        self.assertEqual(draft['form_graph'],imported['form_graph'])
                    finally:other.close()

    def test_general_directed_profile_refuses_unrelated_forms_and_does_not_skip_server_binding(self):
        state=self.directed({91001:91002,91002:0,91003:0},'native-directed-forms-v1')
        self.assertTrue(any(r['code']=='disconnected_form' for r in state['draft']['requirements']))
        state=complete(self.workspace,self.directed({92001:92002,92002:92003,92003:0},'native-directed-forms-v1'))
        self.repo.configured=True;draft=state['draft']
        with self.assertRaisesRegex(ValueError,'形态图计划'):
            self.workspace.confirm(draft['id'],{'preview_revision':draft['content_revision'],'shop':True,'capsule':True},state['revision'])
        self.assertEqual([],self.repo.applied)
        self.assertEqual('draft',self.workspace.public(draft['id'])['draft']['status'])

    def test_directed_profile_preserves_three_form_chain_and_project_roundtrip(self):
        state=complete(self.workspace,self.directed({91001:91002,91002:91003,91003:0}))
        draft=state['draft'];self.assertTrue(draft['can_confirm'],draft['requirements'])
        self.assertEqual('native-three-stage-forms-v1',self.workspace.plan(draft)['behavior_profile'])
        self.assertEqual([91001,91002,91003],draft['form_graph']['root_routes'][0]['path'])
        self.assertEqual('no-target',draft['form_graph']['root_routes'][0]['end'])
        self.assertEqual([1,0,0],[r['server']['SallyReady'] for r in draft['forms']])
        self.assertTrue(any(r['code']=='form_graph_runtime' and r['level']=='warning' for r in draft['requirements']))
        from owner.units.project import export_project
        other=Workspace(Path(self.tmp.name)/'directed-receiver',baseline=Baseline(),repository=Repository(),publisher=publisher)
        try:
            imported=other.inspect('project.zip',export_project(self.workspace,draft),other.public()['revision'])['draft']
            self.assertEqual('native-three-stage-forms-v1',imported['behavior_profile'])
            self.assertEqual(draft['form_graph'],imported['form_graph'])
            self.assertTrue(imported['can_confirm'],imported['requirements'])
        finally:other.close()

    def test_directed_profile_rejects_external_unreachable_and_out_of_budget_graphs(self):
        cases=[({91001:91002,91002:0},'unsupported_forms'),
               ({91001:91002,91002:0,91003:91002},'unreachable_form'),
               ({91001:91002,91002:91003,91003:91001},'behavior_mismatch'),
               ({i:i+1 if i<91017 else 0 for i in range(91001,91018)},'unsupported_forms')]
        for targets,code in cases:
            with self.subTest(code=code,count=len(targets)):
                state=self.directed(targets)
                self.assertFalse(state['draft']['can_confirm'])
                self.assertTrue(any(r['code']==code and r['level']=='error' for r in state['draft']['requirements']))
        with self.assertRaisesRegex(ValueError,'transform target is missing'):
            self.directed({91001:91002,91002:91003,91003:99999})

    def test_three_form_authoring_shows_directed_graph_without_dropping_entry_or_opening_runtime(self):
        from unit_import_support import unit
        with zipfile.ZipFile(io.BytesIO(archive())) as source:
            files={name:source.read(name) for name in source.namelist()}
        files['gdrs/unit.bin']=unit(90001,90002)
        files['gdrs/alternate.bin']=unit(90002,90003)
        files['gdrs/third.bin']=unit(90003,90002)
        out=io.BytesIO()
        with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as target:
            for name,raw in files.items():target.writestr(name,raw)
        state=self.workspace.inspect('three.zip',out.getvalue(),self.workspace.public()['revision'])
        identity=state['draft']['id']
        state=self.workspace.update(identity,{'root_unit_id':90001,'behavior_profile':'native-linked-forms-v1'},state['revision'])
        graph=state['draft']['form_graph']
        self.assertEqual([90001,90002,90003],graph['root_routes'][0]['path'])
        self.assertEqual([90002,90003],graph['root_routes'][0]['cycle'])
        self.assertEqual(3,len(state['draft']['forms']));self.assertFalse(state['draft']['can_confirm'])
        self.assertTrue(any(row['code']=='unsupported_forms' for row in state['draft']['requirements']))
        from owner.units.project import export_project
        other=Workspace(Path(self.tmp.name)/'receiver',baseline=Baseline(),repository=Repository(),publisher=publisher)
        try:
            imported=other.inspect('project.zip',export_project(self.workspace,state['draft']),other.public()['revision'])
            self.assertEqual(graph,imported['draft']['form_graph'])
        finally:other.close()

    def test_user_grade_limit_keeps_sql_int_width_and_byte_fields_stay_bounded(self):
        self.assertEqual(model.field_value('UserGradeLimit',0x01000000),0x01000000)
        with self.assertRaises(ValueError):model.field_value('UserGradeLimit',0x80000000)
        with self.assertRaises(ValueError):model.field_value('SallyReady',256)

    def test_old_project_override_dependency_is_recomputed_from_complete_record(self):
        from unit_import_support import wchar
        state=complete(self.workspace,self.inspect());draft=copy.deepcopy(state['draft'])
        raw=bytearray(self.workspace.blob(draft['forms'][0]['source_sha']))
        wchar(raw,738,'mdrs/extra.mod')
        draft['forms'][0]['source_sha']=self.workspace._store(bytes(raw))
        # Old projects did not know that this field was a native model path.
        draft['forms'][0]['resource_refs']=[]
        issues=self.workspace.requirements(draft)
        self.assertTrue(any(row['code']=='resource' and row['field']=='mdrs/extra.mod' for row in issues))
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.repo=Repository()
        self.workspace=Workspace(Path(self.tmp.name),baseline=Baseline(),repository=self.repo,publisher=publisher)
    def tearDown(self):self.workspace.close();self.tmp.cleanup()
    def inspect(self):
        return self.workspace.inspect('unit.zip',archive(),self.workspace.public()['revision'])
    def test_single_form_auto_detected_but_missing_fields_block(self):
        state=self.inspect();d=state['draft'];self.assertEqual(d['root_unit_id'],90001)
        self.assertEqual(d['behavior_profile'],'native-single-form-v1');self.assertFalse(d['can_confirm'])
        self.assertTrue(any(r['code']=='field' for r in d['requirements']))
    def test_complete_then_two_preview_approvals_required(self):
        state=complete(self.workspace,self.inspect());d=state['draft'];self.assertTrue(d['can_confirm'],d['requirements'])
        with self.assertRaises(ValueError):self.workspace.confirm(d['id'],{'preview_revision':d['content_revision'],'shop':True},state['revision'])
        state=self.workspace.confirm(d['id'],{'preview_revision':d['content_revision'],'shop':True,'capsule':True},state['revision'])
        self.assertEqual(state['draft']['status'],'confirmed');self.assertFalse(state['draft']['server_result']['applied'])
        self.assertTrue(self.workspace.export(d['id']).startswith(b'PK'))
    def test_stale_revision_and_old_preview_never_apply(self):
        state=complete(self.workspace,self.inspect());d=state['draft'];old=copy.deepcopy(state)
        state=self.workspace.update(d['id'],{'name':'改名'},state['revision'])
        with self.assertRaises(model.RevisionConflict):self.workspace.update(d['id'],{'name':'覆盖'},old['revision'])
        with self.assertRaises(ValueError):self.workspace.confirm(d['id'],{'preview_revision':old['draft']['content_revision'],'shop':True,'capsule':True},state['revision'])
    def test_image_change_invalidates_published_state(self):
        state=complete(self.workspace,self.inspect());d=state['draft']
        state=self.workspace.confirm(d['id'],{'preview_revision':d['content_revision'],'shop':True,'capsule':True},state['revision'])
        state=self.workspace.upload_asset(d['id'],'new.png',png('green'),'shop',state['revision'])
        self.assertEqual(state['draft']['status'],'draft');self.assertNotIn('confirmation',state['draft'])
        self.assertIn('published',self.workspace.read()['drafts'][d['id']])
        with self.assertRaises(ValueError):self.workspace.export(d['id'])
    def test_sql_failure_does_not_publish_draft(self):
        self.repo.configured=True;self.repo.fail=True
        state=complete(self.workspace,self.inspect());d=state['draft']
        with self.assertRaises(ValueError):self.workspace.confirm(d['id'],{'preview_revision':d['content_revision'],'shop':True,'capsule':True},state['revision'])
        self.assertEqual(self.workspace.public()['draft']['status'],'draft');self.assertEqual(self.repo.applied,[])
    def test_corrupt_archive_not_saved_as_draft(self):
        with self.assertRaises(ValueError):self.workspace.inspect('x.zip',b'bad',self.workspace.public()['revision'])
        self.assertEqual(self.workspace.public()['drafts'],[])
    def test_blob_tampering_blocks_confirmation(self):
        state=complete(self.workspace,self.inspect());d=state['draft'];sha=d['presentation']['shop']['asset_sha']
        (self.workspace.root/'objects'/sha).write_bytes(b'changed')
        self.assertFalse(self.workspace.public()['draft']['can_confirm'])
    def test_workspace_lock_prevents_two_writers(self):
        with self.assertRaises(ValueError):Workspace(self.workspace.root,baseline=Baseline(),repository=Repository(),publisher=publisher)

    def test_committed_apply_recovers_after_local_publication_failure(self):
        self.repo.configured=True
        state=complete(self.workspace,self.inspect());d=state['draft']
        with patch.object(self.workspace,'_commit',side_effect=OSError('simulated final file write failure')):
            with self.assertRaises(OSError):self.workspace.confirm(d['id'],{'preview_revision':d['content_revision'],'shop':True,'capsule':True},state['revision'])
        self.assertEqual(len(self.repo.applied),1)
        self.workspace.close()
        self.workspace=Workspace(Path(self.tmp.name),baseline=Baseline(),repository=self.repo,publisher=publisher)
        self.assertEqual(self.workspace.public(d['id'])['draft']['status'],'confirmed')
    def test_disabled_preserves_existing_content(self):
        state=self.inspect();identity=state['draft']['id'];state=self.workspace.disable(identity,state['revision'])
        self.assertEqual(state['draft']['status'],'disabled');self.assertTrue(state['draft']['files'])

    def test_explicit_missing_skill_removal_updates_both_forms_and_native_package(self):
        from unit_import_support import unit,weapon
        source=io.BytesIO()
        with zipfile.ZipFile(source,'w') as z:
            for identity,target in ((90001,90002),(90002,90001)):
                raw=bytearray(unit(identity,target));struct.pack_into('<II',raw,575,70331,123)
                z.writestr(f'gdrs/{identity}.bin',raw)
            for identity in (90010,90011):z.writestr(f'gdrs/{identity}.bin',weapon(identity))
            z.writestr('mdrs/test.mod',b'\xe9\xc6\0\0ZOA\0\x05'+bytes(55))
        self.repo.catalog['skill_ids']={123}
        state=self.workspace.inspect('linked.zip',source.getvalue(),self.workspace.public()['revision'])
        identity=state['draft']['id']
        state=self.workspace.update(identity,{'root_unit_id':90001,'behavior_profile':'native-linked-forms-v1'},state['revision'])
        state=complete(self.workspace,state);before=copy.deepcopy(state['draft'])
        self.assertFalse(before['can_confirm']);self.assertEqual(len(before['removable_missing_skills'][0]['references']),2)
        state=self.workspace.drop_missing_skills(identity,[70331],state['revision']);draft=state['draft']
        self.assertTrue(draft['can_confirm']);self.assertNotEqual(before['content_revision'],draft['content_revision'])
        for form in draft['forms']:
            self.assertEqual(form['server']['SkillNumber_1'],0)
            self.assertEqual(form['server']['SkillNumber_2'],123)
            self.assertEqual(struct.unpack_from('<I',self.workspace.blob(form['source_sha']),575)[0],70331)
        self.assertEqual(len([r for r in draft['requirements'] if r['code']=='skill_omitted']),2)
        self.assertTrue(all(r['SkillNumber_1']==0 for r in self.workspace.plan(draft)['units']))
        with self.assertRaises(ValueError):self.workspace.confirm(identity,{'preview_revision':before['content_revision'],'shop':True,'capsule':True},state['revision'])
        binary=self.workspace.root/'fixture.dll';raw=bytearray(512);raw[:2]=b'MZ'
        struct.pack_into('<I',raw,0x3c,0x80);raw[0x80:0x84]=b'PE\0\0';struct.pack_into('<H',raw,0x84,0x14c);binary.write_bytes(raw)
        self.workspace.module_binary=binary;self.workspace.publisher=None
        with patch('owner.units.presentation.capsule_card',return_value=png()):
            state=self.workspace.confirm(identity,{'preview_revision':draft['content_revision'],'shop':True,'capsule':True},state['revision'])
        with zipfile.ZipFile(io.BytesIO(self.workspace.export(identity))) as z:
            catalog=z.read('client/sdgo/mods/unit_catalog/catalog.bin')
            published_files={name:z.read(name) for name in z.namelist()}
        # Exercise the actual archive consumer: placing a loose model beside
        # the game is insufficient for its native archive-only resource path.
        from owner.collection_capsule.client import ArchiveReader
        import configparser
        prefix='client/sdgo/mods/unit_catalog/'
        runtime=configparser.ConfigParser(interpolation=None)
        runtime.read_string(published_files[prefix+'resource-overlay.ini'].decode('ascii'))
        for key,suffix in (('index','zdx'),('archive','zpk')):
            payload=published_files[prefix+'resources.'+suffix]
            self.assertEqual(hashlib.sha256(payload).hexdigest(),runtime['runtime'][key+'_sha256'])
            (self.workspace.root/('resources.'+suffix)).write_bytes(payload)
        reader=ArchiveReader(self.workspace.root/'resources.zdx',self.workspace.root/'resources.zpk')
        manifest=json.loads(published_files['manifest.json'])
        self.assertEqual(manifest['resource_overlay']['paths'],manifest['owned_paths'])
        for path in manifest['owned_paths']:
            self.assertEqual(reader.read(path),published_files['client/'+path])
        self.assertTrue(reader.has('mdrs/test.mod'))
        offset=32;checked=[];index_entries=[]
        while offset<len(catalog):
            kind,number,size,*_=struct.unpack_from('<6I',catalog,offset);offset+=24
            if kind==2:
                self.assertEqual(struct.unpack_from('<II',catalog,offset+575),(0,123));checked.append(number)
                if number==90001:
                    from PIL import Image
                    paths=[catalog[offset+position:offset+position+100].decode('utf-16-le').split('\0')[0].replace('\\','/') for position in (4654,4754,4854)]
                    self.assertEqual(len(set(paths)),3)
                    sizes=[]
                    for path in paths:
                        spr=published_files['client/'+path]
                        from manager.engines.capsules.capsule_assets import sprite_frame
                        frame=sprite_frame(spr);texture=published_files['client/'+frame['texture'].replace('\\','/')]
                        sizes.append(Image.open(io.BytesIO(texture)).size)
                    self.assertEqual(sizes,[(256,256),(256,256),(92,92)])
            if kind==85:
                self.assertEqual(number,0x375BE2)
                self.assertEqual(catalog[offset+5],1)
                self.assertEqual(struct.unpack_from('<I',catalog,offset+6)[0],90001)
                self.assertEqual(size,614)
                index_entries.append(number)
                for position in (14,114):
                    path=catalog[offset+position:offset+position+100].decode('utf-16-le').split('\0')[0].replace('\\','/')
                    spr=published_files['client/'+path]
                    from manager.engines.capsules.capsule_assets import sprite_frame
                    frame=sprite_frame(spr)
                    self.assertEqual(Image.open(io.BytesIO(published_files['client/'+frame['texture'].replace('\\','/')])).size,(122,122))
            offset+=size
        self.assertEqual(checked,[90001,90002])
        self.assertEqual(index_entries,[0x375BE2])

    def test_skill_removal_rejects_known_unverified_and_stale_requests(self):
        state=complete(self.workspace,self.inspect());identity=state['draft']['id']
        state=self.workspace.update(identity,{'forms':[{'id':90001,'server':{'SkillNumber_1':123,'SkillNumber_3':70331}}]},state['revision'])
        self.repo.catalog['skill_ids']={123};before=self.workspace.path.read_bytes()
        for ids in ([123],[70331],[],[True],[123,123]):
            with self.assertRaises(ValueError):self.workspace.drop_missing_skills(identity,ids,state['revision'])
        self.assertEqual(before,self.workspace.path.read_bytes())
        with self.assertRaises(model.RevisionConflict):self.workspace.drop_missing_skills(identity,[70331],'stale')


if __name__=='__main__':unittest.main()
