import unittest

from owner.units.activity_patterns import build_pattern
from owner.units.activity_repository import normalize_config


class ActivityPatterns(unittest.TestCase):
    def fixture(self):
        units=[{'UnitNumber':uid,'UnitGradeNumber':4,'UnitGradeNumber2':grade}
               for uid,grade in [(1,4),(2,2),(3,4),(4,2)]]
        categories=[{'CatalogueID':1100000+uid,'Name':str(uid),'RefIndex':uid,'TimeLimit':0}
                    for uid in range(1,5)]
        categories.append({'CatalogueID':2220001,'Name':'Native experience pack','RefIndex':3240001,'TimeLimit':0})
        prices=[{'MachineID':mid,'Disable':0,'Type':kind,'Price':price}
                for mid,kind,price in [(101,0,10000),(201,2,3),(9301,2,99)]]
        pool=[]
        for mid in (101,201):
            for cid,amount,count in [(1100002,1,10),(1100004,1,10),(2220001,1,4980),(2220001,3,5000)]:
                pool.append({'MachineID':mid,'CategoryID':cid,'Amount':amount,'ExpandedEntries':count})
        for cid,count in [(1100001,1000),(1100003,1000),(1100002,2500),(1100004,2500),(2220001,3000)]:
            pool.append({'MachineID':9301,'CategoryID':cid,'Amount':1,'ExpandedEntries':count})
        return dict(units=units,categories=categories,prices=prices,pool=pool,listed_machines={101,201,9301})

    def build(self,pattern,preferences=None):
        data=self.fixture()
        # The ordinary experience category is split across amounts, but its
        # category total must still stay below the native repeat limit.
        data['categories'].append({'CatalogueID':2220002,'Name':'Native second pack','RefIndex':3240002,'TimeLimit':0})
        for row in data['pool']:
            if row['MachineID'] in (101,201) and row['Amount']==3:row['CategoryID']=2220002
        return build_pattern(pattern,{'unit_id':90001,'rank':4,'subgrade':4},1119000,preferences=preferences,**data)

    def test_owner_can_configure_private_gp_and_small_token_target_defaults(self):
        preferences={'schema_version':1,'patterns':{'gp_rare':{'featured_entries':20},'token_rare':{'featured_entries':20}}}
        for pattern,currency,price in [('gp_rare',0,10000),('token_rare',2,3)]:
            result=self.build(pattern,preferences)
            self.assertEqual((currency,price,20),(result['currency_type'],result['price'],result['target_entries']))
            self.assertEqual(10000,sum(row['entries'] for row in result['pool']))
            root=next(r for r in result['pool'] if r['category_id']==1119000)
            self.assertEqual(20,root['entries'])
            self.assertTrue(any(r['category_id']==1100004 for r in result['pool']))

    def test_framework_without_owner_settings_uses_host_reference_frequency(self):
        result=self.build('gp_rare')
        self.assertEqual(10,result['target_entries'])

    def test_99_token_pool_is_only_selected_explicitly(self):
        result=self.build('token_99')
        self.assertEqual((2,99,1000),(result['currency_type'],result['price'],result['target_entries']))
        self.assertEqual(9301,result['source_machine_id'])

    def test_unsupported_pattern_never_falls_back(self):
        with self.assertRaises(ValueError):self.build('unknown')

    def test_same_item_different_amounts_are_supported_but_category_limit_is_combined(self):
        config={'schema_version':1,'id':1990001,'unit_id':90001,'project_key':'fixture',
                'revision':'a'*64,'currency_type':0,'price':10000,'template_machine_id':101,
                'pool':[{'category_id':1119000,'amount':1,'entries':20},
                        {'category_id':2220001,'amount':1,'entries':2000},
                        {'category_id':2220001,'amount':3,'entries':3000},
                        {'category_id':2220002,'amount':1,'entries':4980}]}
        self.assertEqual(config,normalize_config(config))
        config['pool'][1]['entries']+=1;config['pool'][3]['entries']-=1
        with self.assertRaisesRegex(ValueError,'50%'):normalize_config(config)


if __name__=='__main__':unittest.main()
