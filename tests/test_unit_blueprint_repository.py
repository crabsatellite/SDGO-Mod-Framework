import copy
import unittest

from owner.units import blueprint_repository as r
from owner.units.repository import _row_hash,ITEM_COLUMNS
from test_unit_blueprints import declaration


class BlueprintRepository(unittest.TestCase):
    def catalog(self):
        recipe=declaration();recipe['id']=200001
        return {'blueprints':{200001:{}},'blueprint_infos':{},'blueprint_materials':{},
                'items':{1520001:{'CatalogueID':1520001,'Name':'Original Plan',
                    'RefIndex':200001,'TimeLimit':0,'Option1':0,'Option2':0}}}

    def validate(self,recipe=None,catalog=None,ownership=None):
        return r.validate([recipe or declaration()],root=90001,unit_ids={13003,16002,15004,90001},
            catalog=catalog or self.catalog(),ownership=ownership or {},family='owner.family')

    def test_exact_native_definition_and_hash_groups(self):
        entry=self.validate()[0]
        self.assertEqual(209000,entry['recipe']['Idx'])
        self.assertEqual(1090352029,entry['info']['Hash'])
        self.assertEqual(3,len(entry['materials']))
        self.assertEqual('Example Plan',entry['item']['Name'])
        self.assertEqual(1599000,entry['item']['CatalogueID'])
        self.assertEqual(209000,entry['item']['RefIndex'])

    def test_foreign_record_or_stale_owner_is_rejected(self):
        catalog=self.catalog();entry=self.validate()[0]
        catalog['blueprints'][209000]=entry['recipe']
        with self.assertRaisesRegex(ValueError,'占用'):self.validate(catalog=catalog)
        ownership={('blueprint',209000):('owner.family',_row_hash(entry['recipe'],r.RECIPE_COLUMNS))}
        self.assertEqual(209000,self.validate(catalog=catalog,ownership=ownership)[0]['recipe']['Idx'])
        catalog['blueprints'][209000]['RepayPoint']=42
        with self.assertRaisesRegex(ValueError,'修改'):self.validate(catalog=catalog,ownership=ownership)

    def test_material_list_hash_is_order_independent_but_level_sensitive(self):
        entry=self.validate()[0];materials=entry['materials']
        self.assertEqual(r.material_hash(materials),r.material_hash(list(reversed(materials))))
        updated=copy.deepcopy(materials);updated[0]['UnitLv']=1
        self.assertNotEqual(r.material_hash(materials),r.material_hash(updated))

    def test_missing_or_foreign_material_list_not_silently_rebuilt(self):
        catalog=self.catalog();catalog['blueprint_materials'][209000]=self.validate()[0]['materials']
        with self.assertRaisesRegex(ValueError,'材料索引'):self.validate(catalog=catalog)
        catalog['blueprint_materials']={}
        with self.assertRaisesRegex(ValueError,'账本'):
            self.validate(catalog=catalog,ownership={('blueprint_list',209000):('owner.family','a'*64)})

    def test_reserved_ids_and_typed_item_template_are_enforced(self):
        for field,value in [('id',200001),('item_category_id',1520001)]:
            recipe=declaration();recipe[field]=value
            with self.subTest(field=field),self.assertRaisesRegex(ValueError,'保留范围'):self.validate(recipe)
        catalog=self.catalog();catalog['items'][1520001]['RefIndex']=999
        with self.assertRaisesRegex(ValueError,'对应配方'):self.validate(catalog=catalog)

    def test_missing_fields_of_a_disabled_definition_remain_blocking(self):
        recipe=declaration();recipe['enabled']=False;recipe['materials']=[]
        with self.assertRaisesRegex(ValueError,'3至5'):self.validate(recipe)


if __name__=='__main__':unittest.main()
