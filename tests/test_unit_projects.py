import io
import json
import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
import zipfile

from owner.units.project import normalize_metadata,matches,export_project
from owner.units.workspace import Workspace
from unit_import_support import Baseline,Repository,publisher,png,archive,complete
from test_unit_blueprint_templates import bound_fixture,declaration
from test_unit_blueprint_resources import closure_fixture


class UnitProjects(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);self.engines=[]
    def tearDown(self):
        for engine in self.engines:engine.close()
        self.temp.cleanup()
    def engine(self,name):
        engine=Workspace(self.root/name,baseline=Baseline(),repository=Repository(),publisher=publisher)
        self.engines.append(engine);return engine

    def test_metadata_and_poster_round_trip_without_runtime_records_or_host_paths(self):
        one=self.engine('one');state=one.create_project({'name':'Example',
            'metadata':{'description':'Original owner description','aliases':['Example Alias'],'search_examples':['Alias']}},one.public()['revision'])
        identity=state['draft']['id']
        state=one.upload_asset(identity,'poster.png',png(),'activity_poster',state['revision'])
        raw=export_project(one,state['draft'])
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            manifest=json.loads(z.read('project.json'))
            self.assertFalse(manifest['runtime_installable'])
            self.assertFalse(any(n.endswith(('.exe','.dll','.ini')) for n in z.namelist()))
            self.assertNotIn(str(self.root),json.dumps(manifest))
            self.assertIn('assets/'+state['draft']['media']['activity_poster']['asset_sha']+'.png',z.namelist())
        two=self.engine('two');imported=two.inspect('project.zip',raw,two.public()['revision'])
        self.assertEqual('metadata-only',imported['draft']['project_kind'])
        self.assertEqual('Original owner description',imported['draft']['metadata']['description'])
        self.assertTrue(matches(imported['draft'],'example alias'))
        self.assertEqual(state['draft']['media'],imported['draft']['media'])
        self.assertEqual(state['draft']['family_key'],imported['draft']['family_key'])
        self.assertFalse(imported['draft']['can_confirm'])
        self.assertEqual([],two.repository.applied)

    def test_missing_or_modified_asset_rejected_before_import(self):
        one=self.engine('one');state=one.create_project({'name':'Example'},one.public()['revision'])
        state=one.upload_asset(state['draft']['id'],'poster.png',png(),'activity_poster',state['revision'])
        raw=export_project(one,state['draft'])
        for replace in (None,b'not the declared PNG'):
            out=io.BytesIO()
            with zipfile.ZipFile(io.BytesIO(raw)) as source,zipfile.ZipFile(out,'w') as target:
                for name in source.namelist():
                    if name.startswith('assets/'):
                        if replace is not None:target.writestr(name,replace)
                    else:target.writestr(name,source.read(name))
            two=self.engine('two'+str(replace is None))
            with self.assertRaisesRegex(ValueError,'清单|修改'):two.inspect('bad.zip',out.getvalue(),two.public()['revision'])

    def test_foreign_media_reference_and_unknown_metadata_rejected(self):
        engine=self.engine('one');state=engine.create_project({'name':'Example'},engine.public()['revision'])
        with self.assertRaises(ValueError):engine.update(state['draft']['id'],{'media':{'activity_poster':{'asset_sha':'a'*64}}},state['revision'])
        with self.assertRaises(ValueError):normalize_metadata({'server_password':'not metadata'})
        with self.assertRaises(ValueError):normalize_metadata({'visibility':'public'})
        with self.assertRaises(ValueError):normalize_metadata({'aliases':['Alias','ＡＬＩＡＳ']})

    def test_examples_do_not_invent_search_matches(self):
        draft={'name':'Example','metadata':{'aliases':['Known'],'search_examples':['Unmatched']}}
        self.assertTrue(matches(draft,'known'))
        self.assertFalse(matches(draft,'unmatched'))

    def test_legacy_record_package_still_imports(self):
        engine=self.engine('one');state=engine.inspect('legacy.zip',archive(),engine.public()['revision'])
        self.assertEqual(1,len(state['draft']['forms']))

    def test_native_programs_round_trip_but_cannot_apply_before_actor_admission(self):
        from test_unit_native_programs import fixture
        from unit_import_support import weapon
        one=self.engine('one');state=complete(one,one.inspect('unit.zip',archive(),one.public()['revision']))
        declaration,program_files,_=fixture(identity=90010)
        record=bytearray(weapon(90010));name='example_Init'.encode('utf-16-le')
        record[1771:1771+len(name)]=name
        declaration['weapon_initializers'][0]['weapon_sha256']=hashlib.sha256(record).hexdigest()
        manifest_path='native-programs/manifest.json'
        program_manifest=json.dumps(declaration).encode()
        binding={'manifest_path':manifest_path,'manifest_sha256':hashlib.sha256(program_manifest).hexdigest()}
        with zipfile.ZipFile(io.BytesIO(export_project(one,state['draft']))) as source:
            files={name:source.read(name) for name in source.namelist()}
        project=json.loads(files.pop('project.json'))
        files['gdrs/first.bin']=bytes(record);files.update(program_files);files[manifest_path]=program_manifest
        project['native_programs']=[binding]
        project['files']={name:hashlib.sha256(raw).hexdigest() for name,raw in files.items()}
        files['project.json']=json.dumps(project).encode()
        output=io.BytesIO()
        with zipfile.ZipFile(output,'w') as target:
            for name,raw in files.items():target.writestr(name,raw)
        two=self.engine('two');imported=two.inspect('program-project.zip',output.getvalue(),two.public()['revision'])
        self.assertEqual([binding],imported['draft']['native_programs'])
        self.assertEqual(1,imported['draft']['summary']['native_program_groups'])
        self.assertFalse(imported['draft']['can_confirm'])
        self.assertTrue(any(row['code']=='native_program_actor_adapter' for row in imported['draft']['requirements']))
        self.assertFalse(imported['capabilities']['native_programs']['native_publication'])
        self.assertEqual([],two.repository.applied)
        with zipfile.ZipFile(io.BytesIO(export_project(two,imported['draft']))) as result:
            self.assertEqual([binding],json.loads(result.read('project.json'))['native_programs'])
            self.assertEqual(program_manifest,result.read(manifest_path))
            self.assertEqual(program_files['example.scp'],result.read('example.scp'))

    def test_full_record_project_keeps_definitions_and_allocates_local_item_identity(self):
        one=self.engine('one');state=complete(one,one.inspect('unit.zip',archive(),one.public()['revision']))
        state=one.update(state['draft']['id'],{'metadata':{'description':'Owner description'}},state['revision'])
        raw=export_project(one,state['draft']);two=self.engine('two')
        imported=two.inspect('project.zip',raw,two.public()['revision'])
        self.assertEqual(90001,imported['draft']['root_unit_id'])
        self.assertEqual(1110001,imported['draft']['item_template_id'])
        self.assertIsNotNone(imported['draft']['category_id'])
        self.assertTrue(imported['draft']['can_confirm'])

    def test_blueprint_client_templates_round_trip_without_untyped_bin_decode(self):
        one=self.engine('one')
        state=complete(one,one.inspect('unit.zip',archive(),one.public()['revision']))
        draft=copy.deepcopy(state['draft'])
        binding,files=bound_fixture()
        for path,data in files.items():
            digest=one._store(data)
            draft['files'].append({'path':path,'sha':digest,'bytes':len(data)})
        recipe=declaration(binding)
        recipe['output_unit_id']=draft['root_unit_id']
        draft['blueprints']=[recipe]
        raw=export_project(one,draft)
        two=self.engine('two')
        imported=two.inspect('project.zip',raw,two.public()['revision'])
        restored=imported['draft']['blueprints'][0]
        self.assertEqual(binding,restored['client_templates'])
        paths={entry['path'] for entry in imported['draft']['files']}
        self.assertTrue(set(files)<=paths)
        self.assertTrue(any(row['code']=='blueprint_template_semantic_closure'
                            for row in imported['draft']['requirements']))
        self.assertFalse(any(row['code']=='blueprint_template_binding'
                             for row in imported['draft']['requirements']))

    def test_closed_blueprint_resources_round_trip_without_opening_native_admission(self):
        one=self.engine('closed-one')
        state=complete(one,one.inspect('unit.zip',archive(),one.public()['revision']))
        draft=copy.deepcopy(state['draft'])
        manifest,files=closure_fixture()
        encoded=json.dumps(manifest).encode();path='blueprints/closed.json';files[path]=encoded
        binding={'manifest_path':path,'manifest_sha256':__import__('hashlib').sha256(encoded).hexdigest()}
        for member,data in files.items():
            draft['files'].append({'path':member,'sha':one._store(data),'bytes':len(data)})
        recipe=declaration(binding);recipe['output_unit_id']=draft['root_unit_id'];draft['blueprints']=[recipe]
        exported=export_project(one,draft)
        two=self.engine('closed-two');imported=two.inspect('closed.zip',exported,two.public()['revision'])
        restored=imported['draft'];requirements=restored['requirements']
        self.assertEqual(binding,restored['blueprints'][0]['client_templates'])
        self.assertIn('dependencies/a.txr',{entry['path'] for entry in restored['files']})
        self.assertFalse(any(row['code']=='blueprint_template_semantic_closure' for row in requirements))
        self.assertFalse(any(row['code']=='blueprint_template_native_adapter' for row in requirements))
        self.assertTrue(any(row['code']=='blueprint_template_native_ui' for row in requirements))
        self.assertFalse(restored['can_confirm'])

    def test_project_rejects_rehashed_manifest_with_stale_declaration_binding(self):
        one=self.engine('one')
        state=complete(one,one.inspect('unit.zip',archive(),one.public()['revision']))
        draft=copy.deepcopy(state['draft'])
        binding,files=bound_fixture()
        for path,data in files.items():
            digest=one._store(data)
            draft['files'].append({'path':path,'sha':digest,'bytes':len(data)})
        recipe=declaration(binding);recipe['output_unit_id']=draft['root_unit_id']
        draft['blueprints']=[recipe]
        raw=export_project(one,draft)
        output=io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(raw)) as source:
            project=json.loads(source.read('project.json'))
            manifest_path=binding['manifest_path']
            changed=source.read(manifest_path)+b' '
            project['files'][manifest_path]=__import__('hashlib').sha256(changed).hexdigest()
            with zipfile.ZipFile(output,'w') as target:
                for name in source.namelist():
                    if name=='project.json':
                        target.writestr(name,json.dumps(project,ensure_ascii=False))
                    elif name==manifest_path:
                        target.writestr(name,changed)
                    else:
                        target.writestr(name,source.read(name))
        two=self.engine('two')
        with self.assertRaisesRegex(ValueError,'SHA-256 mismatch'):
            two.inspect('bad-project.zip',output.getvalue(),two.public()['revision'])

    def test_manager_can_create_metadata_first_without_a_client_or_host(self):
        from manager.workspace import Workspace as ManagerWorkspace
        from manager.providers import attach
        root=self.root/'manager';manager=ManagerWorkspace(root,Path(__file__).resolve().parents[1]/'mods')
        try:
            attach(manager);units=manager.providers['units']
            current=units.request('GET','/api/units',{}, {})
            created=units.request('POST','/api/units/projects',{'name':'Metadata first','metadata':{'aliases':['Search Alias']}},
                                  {'X-Catalog-Revision':current['revision']})
            self.assertFalse(created['draft']['can_confirm'])
            library=manager.providers['capsules'].request('GET','/api/projects',{}, {})
            self.assertEqual('Metadata first',library['items'][0]['name'])
            self.assertTrue(library['items'][0]['usable_for_authoring'])
            self.assertFalse(library['items'][0]['native_ready'])
        finally:manager.close()


if __name__=='__main__':unittest.main()
