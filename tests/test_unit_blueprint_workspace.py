import copy
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from owner.units.workspace import Workspace
from owner.units import model
from unit_import_support import Baseline,Repository,archive,publisher


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
        self.assertIsNone(recipe['recipe_template_id'])
        self.assertEqual(bytes(raw),self.engine.blob(recipe['source']['record_sha256']))
        self.assertFalse(state['draft']['can_confirm'])


if __name__=='__main__':unittest.main()
