from collections import Counter
from fractions import Fraction
import unittest

from owner.units.activity_pool import compile_pool


class ActivityPool(unittest.TestCase):
    def test_native_sequence_exact_counts_spread_and_reproducibility(self):
        rows=[{'category_id':1119000,'amount':1,'weight':{'numerator':31,'denominator':350},'role':'featured'},
              {'category_id':1120001,'amount':1,'weight':{'numerator':319,'denominator':350}}]
        result=compile_pool(rows)
        counts=Counter(r['CategoryID'] for r in result['sequence'])
        self.assertEqual(10000,len(result['sequence']));self.assertEqual(886,counts[1119000])
        self.assertEqual(result,compile_pool(rows))
        self.assertEqual({'numerator':31,'denominator':350},result['rewards'][0]['intended_frequency'])
        self.assertLess(abs(result['rewards'][0]['quantization_error']),1/10000)

    def test_equal_probability_tie_is_stable_and_mixed_units_keep_original_categories(self):
        rewards=[{'category_id':r,'amount':1,'weight':1} for r in (1120002,1520102,1120001)]
        result=compile_pool(rewards)
        self.assertEqual({1120001:3334,1120002:3333,1520102:3333},dict(Counter(r['CategoryID'] for r in result['sequence'])))
        self.assertEqual(list(range(1,10001)),[r['Idx'] for r in result['sequence']])
        self.assertEqual(0,result['adjacent_category_repeats'])

    def test_thematic_mix_avoids_repeat_rejection_without_changing_any_prize_count(self):
        counts=[886,200,120,120,160,300,300,250,400,350,700,600,700,700,700,700,400,400,400,414,300,300,200,200,200]
        rewards=[{'category_id':1000000+i,'amount':1,'weight':count} for i,count in enumerate(counts)]
        result=compile_pool(rewards)
        self.assertEqual(0,result['adjacent_category_repeats'])
        actual=Counter(r['CategoryID'] for r in result['sequence'])
        self.assertEqual(counts,[actual[r['category_id']] for r in rewards])

    def test_invalid_rewards_or_size_are_rejected_without_implicit_loss(self):
        valid=[{'category_id':1,'amount':1,'weight':1},{'category_id':2,'amount':1,'weight':1}]
        for rows in ([],[valid[0]],[valid[0],valid[0]],
            [{'category_id':1,'amount':True,'weight':1},valid[1]],
            [{'category_id':1,'amount':1,'weight':1},{'category_id':2,'amount':1,'weight':1000000000}]):
            with self.subTest(rows=rows),self.assertRaises(ValueError):compile_pool(rows)
        with self.assertRaises(ValueError):compile_pool(valid,size=999)


if __name__=='__main__':unittest.main()
