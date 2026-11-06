import copy
import unittest
from owner.units.transform_graph import inspect
from unit_import_support import unit


class TransformGraph(unittest.TestCase):
    def test_three_forms_preserve_one_way_entry_into_two_form_cycle(self):
        records={90001:unit(90001,90002),90002:unit(90002,90003),90003:unit(90003,90002)}
        before=copy.deepcopy(records);graph=inspect(records,[90001])
        self.assertEqual([90001,90002,90003],graph['root_routes'][0]['path'])
        self.assertEqual([90002,90003],graph['root_routes'][0]['cycle'])
        self.assertEqual([[90002,90003]],graph['directed_cycles'])
        self.assertEqual(before,records);self.assertTrue(graph['record_references_closed'])
        self.assertFalse(graph['native_behavior_inferred']);self.assertFalse(graph['native_registration_authorized'])

    def test_shared_later_forms_do_not_discard_other_entries_or_invent_reverse_edges(self):
        records={90001:unit(90001,90004),90002:unit(90002,90004),90003:unit(90003,90004),90004:unit(90004)}
        graph=inspect(records,[90001,90003])
        self.assertEqual([90001,90004],graph['root_routes'][0]['path'])
        self.assertEqual([90002,90003],graph['root_routes'][0]['other_component_ids'])
        self.assertEqual([90003,90004],graph['root_routes'][1]['path'])
        self.assertEqual(0,graph['nodes'][3]['transform_target'])
        self.assertEqual(1,len(graph['components']));self.assertFalse(graph['player_inventory_inferred'])

    def test_external_target_and_disconnected_variants_remain_explicit(self):
        graph=inspect({90001:unit(90001,90002),90003:unit(90003)},[90001])
        self.assertEqual([{'source':90001,'target':90002}],graph['external_targets'])
        self.assertFalse(graph['record_references_closed']);self.assertEqual('external',graph['root_routes'][0]['end'])
        self.assertEqual(2,len(graph['components']));self.assertEqual([],graph['root_routes'][0]['other_component_ids'])

    def test_complete_records_and_explicit_roots_are_required(self):
        for records,roots in (({90001:unit(90002)},[]),({90001:b'partial'},[]),
                              ({90001:unit()},[90002]),({90001:unit()},[90001,90001]),({True:unit()},[])):
            with self.subTest(roots=roots),self.assertRaises(ValueError):inspect(records,roots)
        self.assertEqual([],inspect({})['nodes'])


if __name__=='__main__':unittest.main()
