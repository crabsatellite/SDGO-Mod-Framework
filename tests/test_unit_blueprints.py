import copy
import unittest

from owner.units import blueprints as b


def declaration():
    return {'schema_version':1,'id':209000,'name':'示例设计图','internal_name':'Example Plan','enabled':True,
        'source':{'kind':'authored','note':'Explicit owner choice'},'recipe_template_id':200001,
        'item_template_id':1520001,'item_category_id':1599000,'output_unit_id':90001,
        'materials':[{'unit_id':13003,'minimum_level':7},{'unit_id':16002,'minimum_level':1},
                     {'unit_id':15004,'minimum_level':1}],
        'minimum_player_level':0,'repay_point':600}


class UnitBlueprints(unittest.TestCase):
    def test_exact_baseline_hash_uses_native_arithmetic(self):
        self.assertEqual(1090352029,b.native_material_hash([13003,16002,15004]))
        with self.assertRaisesRegex(ValueError,'INT'):b.native_material_hash([46341])

    def test_material_order_and_levels_are_preserved(self):
        source=declaration();original=copy.deepcopy(source)
        rows=b.server_rows(source,root_unit_id=90001)
        self.assertEqual(13003,rows['recipe']['UnitID1'])
        self.assertEqual(7,rows['recipe']['UnitLevel1'])
        self.assertEqual(0,rows['recipe']['UnitID7'])
        self.assertEqual(1090352029,rows['info']['Hash'])
        self.assertEqual(source,original)

    def test_missing_values_stay_missing_and_block_compilation(self):
        value={'schema_version':1,'source':{'kind':'unknown'},'materials':[]}
        normalized=b.normalize(value,root_unit_id=90001)
        self.assertIsNone(normalized['repay_point'])
        issues=b.requirements(normalized,root_unit_id=90001)
        self.assertTrue(any(i['code']=='blueprint_source' for i in issues))
        with self.assertRaises(ValueError):b.server_rows(normalized,root_unit_id=90001)

    def test_source_recovered_requires_row_identity_and_hash(self):
        value=declaration();value['source']={'kind':'source-recovered'}
        self.assertEqual(3,sum(i['code']=='blueprint_source' for i in b.requirements(value,root_unit_id=90001)))
        value['source'].update(domain='source-domain',record_id=200001,record_sha256='a'*64)
        self.assertEqual([],b.requirements(value,root_unit_id=90001))

    def test_unknown_fields_duplicates_self_reference_and_wrong_output_rejected(self):
        changes=[{'unexpected':True},{'output_unit_id':10001},
            {'materials':[{'unit_id':90001,'minimum_level':1}]},
            {'materials':[{'unit_id':1,'minimum_level':1}]*2},
            {'materials':[{'unit_id':1,'minimum_level':1}]*8}]
        for change in changes:
            value=declaration();value.update(change)
            with self.subTest(change=change),self.assertRaises(ValueError):b.normalize(value,root_unit_id=90001)

    def test_host_dependency_and_level_domains(self):
        value=declaration()
        self.assertEqual(2,sum(i['code']=='blueprint_material' for i in b.requirements(value,root_unit_id=90001,unit_ids={13003})))
        for bad in (True,-1,256,1.5):
            value=declaration();value['minimum_player_level']=bad
            with self.subTest(value=bad),self.assertRaises(ValueError):b.normalize(value,root_unit_id=90001)

    def test_primary_recipe_is_not_inferred_from_id_order(self):
        first=declaration();second=declaration();second['id']=209001
        with self.assertRaises(ValueError):b.primary_id([first,second])
        first['encyclopedia_primary']=True
        self.assertEqual(209000,b.primary_id([second,first]))
        second['encyclopedia_primary']=True
        with self.assertRaises(ValueError):b.primary_id([first,second])
        self.assertEqual(209000,b.primary_id([first]))


if __name__=='__main__':unittest.main()
