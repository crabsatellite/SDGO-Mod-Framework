import copy
import unittest
from owner.units import model


class ModelTests(unittest.TestCase):
    def record(self,identity=25061):
        return {'id':identity,'name':'机体','client':{'WeaponNumber_1':61165},'server':{'UnitNumber':identity,'HP':14500},
                'sources':{},'transform_target':0,'template_id':None}

    def draft(self):
        return {'id':'a','name':'机体','root_unit_id':None,'behavior_profile':None,'forms':[self.record()],
                'weapons':[],'status':'draft','presentation':{'shop':{},'capsule':{}},'item_template_id':None}

    def test_missing_values_are_not_turned_into_zero(self):
        r=model.new_record({'id':25061,'name':'示例机体','raw':b'raw','client':{'UnitNumber':25061,'HP':14500}},'unit')
        self.assertNotIn('MoveLevel',r['server']);self.assertEqual(r['server']['HP'],14500)

    def test_template_fills_only_missing_fields(self):
        r=model.update_record(self.record(),{'id':25061,'template_id':16027},'unit',{16027:{'HP':100,'MoveLevel':77}})
        self.assertEqual(r['server']['HP'],14500);self.assertEqual(r['server']['MoveLevel'],77)
        self.assertEqual(r['sources']['MoveLevel'],'template:16027')

    def test_partial_drafts_can_be_saved(self):
        d=model.update(self.draft(),{'name':'新机体','root_unit_id':None,'item_template_id':None,'behavior_profile':None,
                       'presentation':{'shop':{'asset_sha':None,'caption':''}}},{})
        self.assertIsNone(d['root_unit_id']);self.assertIsNone(d['presentation']['shop']['asset_sha'])

    def test_zero_preserved_and_boolean_numbers_rejected(self):
        r=model.update_record(self.record(),{'id':25061,'server':{'Shield':0}},'unit',{})
        self.assertEqual(r['server']['Shield'],0)
        with self.assertRaises(ValueError):model.field_value('Shield',False)

    def test_roundtrip_form_does_not_erase_package_provenance(self):
        r=self.record();r['sources']['HP']='package'
        result=model.update_record(r,{'id':25061,'server':{'HP':14500}},'unit',{})
        self.assertEqual(result['sources']['HP'],'package')

    def test_nonfinite_and_out_of_bounds_rejected(self):
        for value in (float('nan'),float('inf'),-1):
            with self.assertRaises(ValueError):model.field_value('MaxEnergyAmount',value)
        with self.assertRaises(ValueError):model.field_value('MoveLevel',256)

    def test_record_identity_and_reference_changes_rejected(self):
        with self.assertRaises(ValueError):model.update_record(self.record(),{'id':25061,'server':{'UnitNumber':123}},'unit',{})
        with self.assertRaises(ValueError):model.update(self.draft(),{'forms':[{'id':25061,'server':{'WeaponNumber_1':999}}]}, {})
        with self.assertRaises(ValueError):model.update(self.draft(),{'forms':[{'id':25061,'server':{'UnitNumber_Transform':999}}]}, {})

    def test_root_controls_sortie_readiness(self):
        d=self.draft();d['forms'].append(self.record(25060))
        r=model.update(d,{'root_unit_id':25061},{})
        self.assertEqual([f['server']['SallyReady'] for f in r['forms']],[1,0])

    def test_root_change_wins_over_stale_readonly_form_values(self):
        d=self.draft();d['forms'].append(self.record(25060))
        r=model.update(d,{'root_unit_id':25061,'forms':[{'id':25061,'server':{'SallyReady':None}},
                      {'id':25060,'server':{'SallyReady':1}}]}, {})
        self.assertEqual([f['server']['SallyReady'] for f in r['forms']],[1,0])

    def test_published_copy_does_not_change_preview_revision(self):
        a=self.draft();b=copy.deepcopy(a);b['published']=copy.deepcopy(a);b['status']='confirmed'
        self.assertEqual(model.content_revision(a),model.content_revision(b))
        b['presentation']['shop']['asset_sha']='f'*64
        self.assertNotEqual(model.content_revision(a),model.content_revision(b))

    def test_published_inventory_identity_cannot_be_reassigned(self):
        d=self.draft();d['forms'].append(self.record(25060));d['root_unit_id']=25061
        d['published']=copy.deepcopy(d)
        with self.assertRaisesRegex(ValueError,'根形态'):model.update(d,{'root_unit_id':25060},{})

    def test_every_edit_invalidates_confirmation(self):
        d=self.draft();d['status']='confirmed';d['confirmation']={'shop':True,'capsule':True}
        r=model.update(d,{'name':'新名字'},{})
        self.assertEqual(r['status'],'draft');self.assertNotIn('confirmation',r)

    def test_unknown_fields_and_definitions_are_rejected(self):
        with self.assertRaises(ValueError):model.update(self.draft(),{'execute_sql':'x'},{})
        with self.assertRaises(ValueError):model.definitions({'schema_version':1,'sql':'x'})


if __name__=='__main__':unittest.main()
