from fractions import Fraction
import unittest

from owner.units.activity_balance import recommend


class ActivityBalance(unittest.TestCase):
    def fixture(self):
        units=[{'UnitNumber':1,'UnitGradeNumber':4,'UnitGradeNumber2':4},
               {'UnitNumber':2,'UnitGradeNumber':4,'UnitGradeNumber2':4},
               {'UnitNumber':3,'UnitGradeNumber':4,'UnitGradeNumber2':2}]
        categories=[{'CatalogueID':1100000+i,'RefIndex':i,'Name':str(i)} for i in (1,2,3)]
        categories.append({'CatalogueID':1300001,'RefIndex':1,'Name':'Native filler'})
        prices=[{'MachineID':mid,'Disable':0,'Type':kind,'Price':price} for mid,kind,price in [(101,2,100),(301,2,200),(501,0,999)]]
        pool=[{'MachineID':mid,'CategoryID':cat,'Amount':amount,'ExpandedEntries':count}
              for mid,cat,amount,count in [(101,1100001,1,10),(101,1300001,5,90),
                  (301,1100002,1,30),(301,1300001,5,70),
                  (501,1100003,1,99),(501,1300001,5,1)]]
        return dict(units=units,categories=categories,prices=prices,pool=pool,listed_machines={101,301,501})

    def test_exact_grade_and_currency_mean_and_filler_amount_are_retained(self):
        result=recommend({'unit_id':100,'rank':4,'subgrade':4},**self.fixture())
        self.assertEqual(2,result['currency']);self.assertEqual([101,301],result['source_machine_ids'])
        self.assertEqual(150,result['suggested_price'])
        self.assertEqual(.2,result['rare_configuration_frequency']['value'])
        self.assertEqual(.8,result['filler_rewards'][0]['configuration_frequency']['value'])
        self.assertEqual(5,result['filler_rewards'][0]['amount'])
        self.assertFalse(result['changes_applied'])

    def test_disabled_and_unlisted_machines_do_not_change_average(self):
        data=self.fixture();data['prices'][1]['Disable']=1
        result=recommend({'unit_id':100,'rank':4,'subgrade':4},**data)
        self.assertEqual([101],result['source_machine_ids'])
        data=self.fixture();data['listed_machines']={301}
        self.assertEqual([301],recommend({'unit_id':100,'rank':4,'subgrade':4},**data)['source_machine_ids'])

    def test_unknown_currency_or_tier_stays_unavailable(self):
        self.assertFalse(recommend({'unit_id':100,'rank':4,'subgrade':4},currency=0,**self.fixture())['available'])
        self.assertFalse(recommend({'unit_id':100,'rank':4,'subgrade':5},**self.fixture())['available'])

    def test_all_mean_shares_sum_to_one(self):
        result=recommend({'unit_id':100,'rank':4,'subgrade':4},**self.fixture())
        parts=[result['rare_configuration_frequency'],*[v['configuration_frequency'] for v in result['filler_rewards']]]
        self.assertEqual(Fraction(1),sum((Fraction(v['numerator'],v['denominator']) for v in parts),Fraction()))


if __name__=='__main__':unittest.main()
