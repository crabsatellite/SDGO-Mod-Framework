import copy
import unittest

from owner.units.acquisition import route_roots


def root(uid,rank=1,subgrade=2):
    return {'unit_id':uid,'rank':rank,'subgrade':subgrade,'server_applied':True,
            'revision':'unit-r1','display_name':'Example'}


def recipe(rid,output,*materials):
    return {'Idx':rid,'OutUnitID':output,**{'UnitID'+str(i):materials[i-1] if i<=len(materials) else 0 for i in range(1,8)}}


class AcquisitionRoutes(unittest.TestCase):
    def plan(self,roots,known=None,plans=None,recipes=()):
        reach={'known':known or {},'blueprints':plans or {}}
        return route_roots(roots,reach,recipes,family_revision='catalog-r1')

    def test_every_s_family_root_gets_its_own_activity_intent(self):
        result=self.plan([root(100,4,2),root(101,4,3),root(102,4,4),root(103,4,5)])
        self.assertEqual([100,101,102,103],[r['dedicated_root'] for r in result['routes']])
        self.assertTrue(all(r['route']=='dedicated_activity' and r['artwork_state']=='required' for r in result['routes']))
        self.assertFalse(result['changes_applied'])

    def test_recipe_requires_both_blueprint_item_and_every_material(self):
        items={1:{'source':'fixed_machines'},2:{'source':'shop'}}
        r=recipe(200001,3,1,2)
        self.assertEqual('ordinary_capsule',self.plan([root(3)],items,recipes=[r])['routes'][0]['route'])
        self.assertEqual('ordinary_capsule',self.plan([root(3)],{1:items[1]},{200001:{'source':'shop'}},[r])['routes'][0]['route'])
        good=self.plan([root(3)],items,{200001:{'source':'shop'}},[r])['routes'][0]
        self.assertEqual('blueprint',good['route'])
        self.assertEqual({'1','2'},set(good['material_sources']))

    def test_cycle_without_sources_does_not_become_a_blueprint_route(self):
        recipes=[recipe(200001,1,2),recipe(200002,2,1)]
        result=self.plan([root(1),root(2)],plans={200001:{'source':'shop'},200002:{'source':'shop'}},recipes=recipes)
        self.assertEqual(['ordinary_capsule','ordinary_capsule'],[r['route'] for r in result['routes']])

    def test_transform_state_is_not_an_inventory_material(self):
        reach={'known':{1:{'source':'fixed_machines'}},'forms':[2],'blueprints':{200001:{'source':'shop'}}}
        result=route_roots([root(3)],reach,[recipe(200001,3,2)],family_revision='r1')
        self.assertEqual([2],result['routes'][0]['recipe_analysis'][0]['missing_materials'])

    def test_rare_rank_policy_wins_even_when_a_blueprint_is_obtainable(self):
        result=self.plan([root(3,4,3)],{1:{'source':'fixed_machines'}},{200001:{'source':'shop'}},[recipe(200001,3,1)])
        self.assertEqual('dedicated_activity',result['routes'][0]['route'])

    def test_existing_ordinary_capsule_is_reused_and_inputs_are_immutable(self):
        roots=[root(1)];reach={'known':{1:{'source':'fixed_machines','machine_id':101}},'blueprints':{}}
        original=copy.deepcopy((roots,reach))
        result=route_roots(roots,reach,family_revision='r1')
        self.assertTrue(result['routes'][0]['reuse_existing'])
        result['routes'][0]['current_source']['machine_id']=999
        self.assertEqual(original,(roots,reach))

    def test_unconfirmed_or_duplicate_or_unknown_rank_is_rejected(self):
        invalid=[root(1)];invalid[0]['server_applied']=False
        for roots in (invalid,[root(1),root(1)],[root(1,5)]):
            with self.subTest(roots=roots),self.assertRaises(ValueError):self.plan(roots)

    def test_existing_graph_reader_requires_typed_blueprint_category(self):
        from manager.engines.capsules.capsule_catalog import acquisition
        catalog={'units':[{'id':n,'transform_to':0} for n in range(1,5)],
            'fixed_machines':[{'id':101,'enabled':True,'slots':[{'unit_id':n} for n in (1,2,3)]}],
            'custom_machines':[]}
        r=recipe(200001,4,1,2,3)
        data={('GOnlineGame','tbl_ItemCategory'):[{'CatalogueID':1600001,'TimeLimit':0,'RefIndex':200001}],
              ('GOnlineGame','tbl_MixTreeFile'):[r]}
        read=lambda db,name:data.get((db,name),[])
        sources=[{'category_id':1600001,'witness':{'source':'configured_activity_machine','machine_id':901}}]
        result=acquisition(None,catalog,table_reader=read,source_grants=sources)
        self.assertNotIn(4,result['known'])
        data[('GOnlineGame','tbl_ItemCategory')][0]['CatalogueID']=1599001
        sources[0]['category_id']=1599001
        result=acquisition(None,catalog,table_reader=read,source_grants=sources)
        self.assertEqual('blueprint',result['known'][4]['source'])


if __name__=='__main__':unittest.main()
