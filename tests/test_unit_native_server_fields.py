import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from owner.units import model
from owner.units.native_content import validate_project_bindings
from owner.units.native_server_fields import stock_templates
from owner.units.project import export_project
from owner.units.repository import Repository as CatalogRepository
from owner.units.server_dependencies import validate_manifest,prepare
from owner.units.workspace import Workspace
from test_unit_native_content import fixture,attachment
from test_unit_import_repository import make_snapshot,write_json
from unit_import_support import Baseline,Repository,archive,publisher,complete


def sha(raw):return hashlib.sha256(raw).hexdigest()


class NativeServerFields(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.repository=Repository()
        self.engine=Workspace(self.root/'units',baseline=Baseline(),repository=self.repository,publisher=publisher)

    def tearDown(self):
        self.engine.close();self.temp.cleanup()

    def import_group(self,*,missing=False,dispatch=0,identities=False):
        manifest,files=fixture(dispatch=dispatch)
        self.original=copy.deepcopy((manifest,files))
        skill=json.loads(files[manifest['server_skills']['manifest_path']])
        row=copy.deepcopy(skill['skills'][0]['definition']['values']);row['SkillID']=70116
        self.repository.catalog['skills']={70116:row}
        if identities:
            template=stock_templates(self.repository.catalog,'skill')[0]
            skill['skills'][0]['definition']={'kind':'template','id':template['id'],'sha256':template['sha256']}
            raw=json.dumps(skill).encode();files[manifest['server_skills']['manifest_path']]=raw
            manifest['server_skills']['manifest_sha256']=sha(raw)
            data={'schema':'sdgo.unit-skill-identities/1','owner':'example','revision':skill['revision'],
                  'bindings':[{'target':9800123,'source':70116,'client_sha256':skill['skills'][0]['client_sha256']}]}
            member='dependencies/identities.json';files[member]=json.dumps(data).encode()
            manifest['skill_identities']={'manifest_path':member,'manifest_sha256':sha(files[member])}
        if missing:del manifest['server_skills']
        state=complete(self.engine,self.engine.inspect('unit.zip',archive(),self.engine.public()['revision']))
        self.identity=state['draft']['id']
        state=self.engine.confirm(self.identity,{'preview_revision':state['draft']['content_revision'],'shop':True,'capsule':True},state['revision'])
        self.previous=copy.deepcopy(state['draft'])
        return self.engine.upload_native_content(self.identity,'dependencies.zip',attachment(manifest,files),state['revision'])

    def edit(self,state,definition,*,kind='skill',identity=9800123):
        return self.engine.edit_native_server_definition(self.identity,{
            'manifest_path':state['draft']['native_content'][0]['manifest_path'],'kind':kind,'id':identity,'definition':definition},state['revision'])

    def resolved(self,state):
        files={entry['path']:self.engine.blob(entry['sha']) for entry in state['draft']['files']}
        return validate_project_bindings(state['draft']['native_content'],files=files)[0],files

    def test_partial_values_stay_missing_and_roundtrip_without_battle_mode_guess(self):
        state=self.import_group(missing=True,dispatch=2)
        entry=state['draft']['native_content_groups'][0]['server_definitions'][0]
        fields={row['name']:row for row in entry['fields']}
        self.assertIsNone(fields['BattleMode']['value']);self.assertIn('BattleMode',entry['missing_fields'])
        self.assertTrue(fields['Attack']['readonly']);self.assertEqual(0,fields['Attack']['value'])
        state=self.edit(state,{'kind':'draft','origin':'authored','values':{'SkillName':'Owner supplied name'}})
        group,files=self.resolved(state);server=group['components']['server_skills']
        definition=server['skills'][0]['definition']
        self.assertEqual('draft',definition['kind']);self.assertNotIn('BattleMode',definition['values'])
        self.assertEqual(9800123,definition['values']['SkillID'])
        self.assertTrue(any(row['code']=='native_server_fields' for row in group['requirements']))
        self.assertFalse(state['draft']['can_confirm']);self.assertFalse(group['runtime_authority_issued'])
        records={row['id']:files[row['member']] for row in group['records'] if row['kind']==13}
        with self.assertRaises(ValueError):validate_manifest(server,records)
        with self.assertRaises(ValueError):prepare(server,records,templates={},current={},ownership={},available_ids=set())
        other=Workspace(self.root/'receiver',baseline=Baseline(),repository=Repository(),publisher=publisher)
        try:
            imported=other.inspect('project.zip',export_project(self.engine,state['draft']),other.public()['revision'])
            actual=imported['draft']['native_content_groups'][0]['server_definitions'][0]
            self.assertEqual(definition,actual['definition']);self.assertIn('BattleMode',actual['missing_fields'])
        finally:other.close()
        self.assertEqual(self.previous['forms'],state['draft']['forms'])
        self.assertEqual(self.previous['published'],state['draft']['published'])
        self.assertNotIn('confirmation',state['draft'])

    def test_complete_edit_is_explicit_and_preserves_original_evidence_and_record_bytes(self):
        state=self.import_group(dispatch=2);before_group,before_files=self.resolved(state)
        source_reference=before_group['manifest']['server_skills']
        original=before_files[source_reference['manifest_path']]
        values=copy.deepcopy(before_group['components']['server_skills']['skills'][0]['definition']['values'])
        values.update(SkillName='Changed by owner',BattleMode=0,Scale=1.25)
        state=self.edit(state,{'kind':'draft','origin':'authored','values':values})
        group,files=self.resolved(state);definition=group['components']['server_skills']['skills'][0]['definition']
        self.assertEqual('explicit',definition['kind']);self.assertEqual(values,definition['values'])
        self.assertEqual(original,files[source_reference['manifest_path']])
        self.assertIn({'member':source_reference['manifest_path'],'sha256':sha(original)},group['manifest']['evidence'])
        self.assertEqual(before_group['records'],group['records'])
        server=group['components']['server_skills'];records={9800123:files[group['records'][0]['member']]}
        self.assertEqual(0,prepare(server,records,templates={},current={},ownership={},available_ids=set())[0]['row']['BattleMode'])
        count=len(files)
        values['Scale']=1.5
        state=self.edit(state,{'kind':'draft','origin':'authored','values':values})
        group,files=self.resolved(state)
        self.assertEqual(count,len(files));self.assertEqual(original,files[source_reference['manifest_path']])
        self.assertFalse(state['draft']['can_confirm'])

    def test_bad_fields_stale_revisions_and_template_changes_are_atomic(self):
        state=self.import_group();before=self.engine.read()
        for values in ({'Attack':8},{'Scale':float('nan')},{'Scale':float('inf')},{'Scale':3.5e38},
                       {'PartSpLevel':256},{'BattleMode':True},{'unexpected':1},{'SkillID':11}):
            with self.subTest(values=values),self.assertRaises(ValueError):
                self.edit(state,{'kind':'draft','origin':'authored','values':values})
        with self.assertRaises(model.RevisionConflict):
            self.engine.edit_native_server_definition(self.identity,{'manifest_path':state['draft']['native_content'][0]['manifest_path'],
                'kind':'skill','id':9800123,'definition':{'kind':'draft','origin':'authored','values':{}}},'stale')
        template=state['templates']['skills'][0]
        with self.assertRaisesRegex(ValueError,'已改变'):
            self.edit(state,{'kind':'template','id':template['id'],'sha256':'0'*64})
        row=self.repository.catalog['skills'][70116];row['Attack']=12
        changed=stock_templates(self.repository.catalog,'skill')[0]
        with self.assertRaisesRegex(ValueError,'共享字段'):
            self.edit(state,{'kind':'template','id':changed['id'],'sha256':changed['sha256']})
        self.assertEqual(before,self.engine.read())

    def test_template_save_keeps_behavior_binding_and_moves_revisions_together(self):
        state=self.import_group(identities=True)
        template=state['templates']['skills'][0]
        state=self.edit(state,{'kind':'template','id':template['id'],'sha256':template['sha256']})
        group,files=self.resolved(state);server=group['components']['server_skills'];request=group['components']['skill_identities']
        self.assertEqual(server['revision'],request['revision']);self.assertEqual(70116,request['bindings'][0]['source'])
        before=self.engine.read()
        with self.assertRaisesRegex(ValueError,'行为绑定'):
            self.edit(state,{'kind':'draft','origin':'authored','values':{}})
        self.assertEqual(before,self.engine.read());self.assertFalse(group['runtime_authority_issued'])

    def test_funnel_fields_cannot_truncate_and_unknown_dispatch_stays_blocked(self):
        state=self.import_group(dispatch=165)
        state=self.edit(state,{'kind':'draft','origin':'authored','values':{}},kind='funnel',identity=9800124)
        group,files=self.resolved(state);server=group['components']['server_funnels']
        self.assertEqual('explicit',server['funnels'][0]['definition']['kind'])
        self.assertTrue(any(row['code']=='native_dispatch' for row in group['requirements']))
        self.assertEqual(165,files[group['records'][0]['member']][1131])
        with self.assertRaises(ValueError):self.edit(state,{'kind':'draft','origin':'authored','values':{'AttackCount':300}},kind='funnel',identity=9800124)

    def test_snapshot_keeps_full_template_rows_and_excludes_owned_or_invalid_rows(self):
        root=self.root/'catalog';root.mkdir();make_snapshot(root)
        repository=CatalogRepository(snapshot=root)
        self.assertEqual({70001,70002,70003},set(repository.catalog['skills']))
        self.assertEqual({198001},set(repository.catalog['funnels']))
        self.assertEqual(3,len(stock_templates(repository.catalog,'skill')))
        write_json(root/'dbo.sdgo_UnitImportDependencies.json',[{'RecordKind':'skill','RecordID':70002}])
        repository=CatalogRepository(snapshot=root);repository.catalog['skills'][70003]['SkillName']='bad\0name'
        self.assertEqual([70001],[row['id'] for row in stock_templates(repository.catalog,'skill')])


if __name__=='__main__':unittest.main()
