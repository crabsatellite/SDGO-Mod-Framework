import copy
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from owner.units.workspace import Workspace
from owner.units import model
from unit_import_support import Baseline,Repository,archive,publisher
from test_unit_blueprint_templates import (
    bound_fixture,
    declaration as template_declaration,
    fixture as template_fixture,
    sha,
)


def draft_recipe():
    return {'schema_version':1,'name':'作者设计图','source':{'kind':'authored'},'materials':[]}


class BlueprintWorkspace(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.engine=Workspace(Path(self.temp.name),baseline=Baseline(),repository=Repository(),publisher=publisher)
        self.state=self.engine.inspect('unit.zip',archive(),self.engine.public()['revision'])
        self.id=self.state['draft']['id']

    def tearDown(self):self.engine.close();self.temp.cleanup()

    def save(self,recipes):
        self.state=self.engine.update(self.id,{'blueprints':recipes},self.state['revision'])
        return self.state['draft']

    def draft_with_template_files(self,files):
        draft=copy.deepcopy(self.state['draft'])
        for path,data in files.items():
            digest=self.engine._store(data)
            draft['files'].append({'path':path,'sha':digest,'bytes':len(data)})
        return draft

    def test_author_can_create_edit_remove_unpublished_recipe_and_ids_are_not_reused(self):
        d=self.save([draft_recipe()]);first=copy.deepcopy(d['blueprints'][0])
        self.assertEqual(209000,first['id']);self.assertEqual(1599000,first['item_category_id'])
        first['name']='修改后设计图';d=self.save([first])
        self.assertEqual('修改后设计图',d['blueprints'][0]['name'])
        self.assertEqual(209000,d['blueprints'][0]['id'])
        self.assertEqual([],self.save([])['blueprints'])
        d=self.save([draft_recipe()])
        self.assertEqual(209001,d['blueprints'][0]['id'])
        self.assertEqual(1599001,d['blueprints'][0]['item_category_id'])

    def test_pending_native_adapter_is_explicitly_blocked_not_ignored(self):
        d=self.save([draft_recipe()])
        self.assertIn('blueprint_native_adapter',[r['code'] for r in d['requirements']])
        self.assertFalse(d['can_confirm'])

    def test_disabled_retained_recipe_still_needs_compilable_definition(self):
        recipe=draft_recipe();recipe['enabled']=False
        d=self.save([recipe])
        codes=[row['code'] for row in d['requirements']]
        self.assertIn('blueprint_field',codes)
        self.assertIn('blueprint_native_adapter',codes)
        self.assertFalse(d['can_confirm'])

    def test_definition_patch_accepts_optional_blueprint_declaration(self):
        patch=model.definitions({'schema_version':1,'blueprints':[draft_recipe()]})
        self.assertEqual([draft_recipe()],patch['blueprints'])

    def test_published_recipe_is_retired_by_disable_not_hard_removal(self):
        d=self.save([draft_recipe()]);value=copy.deepcopy(d)
        value['published']={'blueprints':copy.deepcopy(value['blueprints']),'root_unit_id':value['root_unit_id']}
        with self.assertRaisesRegex(ValueError,'已发布设计图'):model.update(value,{'blueprints':[]},self.engine.repository.catalog)
        value['blueprints'][0]['enabled']=False
        changed=model.update(value,{'blueprints':value['blueprints']},self.engine.repository.catalog)
        self.assertFalse(changed['blueprints'][0]['enabled'])

    def test_raw_recipe_in_source_archive_is_retained_with_provenance_and_missing_target_fields(self):
        from test_unit_blueprint_codec import donor
        raw=bytearray(donor());struct.pack_into('<I',raw,229,90001)
        output=io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(archive())) as source,zipfile.ZipFile(output,'w') as target:
            for name in source.namelist():target.writestr(name,source.read(name))
            target.writestr('records/recipe.bin',bytes(raw))
        state=self.engine.inspect('with-recipe.zip',output.getvalue(),self.state['revision'])
        recipe=state['draft']['blueprints'][0]
        self.assertEqual(209000,recipe['id'])
        self.assertEqual(200001,recipe['source']['record_id'])
        self.assertEqual('source-recovered',recipe['source']['kind'])
        self.assertEqual('sprs/recipe.spr',recipe['recipe_sprite'])
        self.assertTrue(any(r['field']=='sprs/recipe.spr' for r in state['draft']['requirements']))
        self.assertIsNone(recipe['recipe_template_id'])
        self.assertEqual(bytes(raw),self.engine.blob(recipe['source']['record_sha256']))
        self.assertFalse(state['draft']['can_confirm'])

    def test_capacity_counts_two_records_for_disabled_and_published_blueprints(self):
        d=self.save([draft_recipe()])
        d['blueprints'][0]['enabled']=False
        state={'drafts':{
            self.id:d,
            'other':{
                'id':'other','status':'draft','forms':[],'weapons':[],
                'published':{
                    'forms':[{} for _ in range(4088)],
                    'weapons':[],
                    'blueprints':[{'enabled':False}],
                },
            },
        }}
        issues=self.engine.requirements(d,state)
        self.assertIn('catalog_capacity',[row['code'] for row in issues])

    def test_workspace_rejects_cross_declaration_template_reuse(self):
        binding,files=bound_fixture()
        draft=self.draft_with_template_files(files)
        first=template_declaration(binding)
        first['output_unit_id']=draft['root_unit_id']
        second=copy.deepcopy(first)
        second.update(id=209001,item_category_id=1599001,
                      encyclopedia_primary=False)
        draft['blueprints']=[first,second]
        issues=self.engine.requirements(
            draft,{'drafts':{draft['id']:draft}})
        matches=[row for row in issues
                 if row['code']=='blueprint_template_project_binding']
        self.assertEqual(1,len(matches))
        self.assertIn('reuse',matches[0]['message'])

    def test_workspace_rejects_conflicting_virtual_resource_witnesses(self):
        first_binding,first_files=bound_fixture()
        second_manifest,raw_second_files=template_fixture()
        second_files={}
        for template in second_manifest['templates']:
            for key in ('source_path','prepared_path'):
                old=template[key];new='second/'+old
                template[key]=new;second_files[new]=raw_second_files[old]
        for resource in second_manifest['resources']:
            old=resource['member'];new='second/'+old
            resource['member']=new;second_files[new]=raw_second_files[old]
        second_manifest['owner']='generic-test-owner-2'
        manifest_path='blueprints/209001/client-templates.json'
        raw=json.dumps(second_manifest,ensure_ascii=False,
                       sort_keys=True).encode()
        second_files[manifest_path]=raw
        second_binding={'manifest_path':manifest_path,
                        'manifest_sha256':sha(raw)}
        combined={**first_files,**second_files}
        draft=self.draft_with_template_files(combined)
        first=template_declaration(first_binding)
        first['output_unit_id']=draft['root_unit_id']
        second=template_declaration(second_binding)
        second.update(id=209001,item_category_id=1599001,
                      output_unit_id=draft['root_unit_id'],
                      encyclopedia_primary=False)
        draft['blueprints']=[first,second]
        issues=self.engine.requirements(
            draft,{'drafts':{draft['id']:draft}})
        matches=[row for row in issues
                 if row['code']=='blueprint_template_project_binding']
        self.assertEqual(1,len(matches))
        self.assertIn('disagree about resource',matches[0]['message'])


if __name__=='__main__':unittest.main()
