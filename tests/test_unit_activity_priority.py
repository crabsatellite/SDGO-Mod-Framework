import copy
import hashlib
import struct
import unittest

from owner.collection_capsule import client
from owner.units.activity_client import compile_activity_client
from manager.engines.capsules.capsule_catalog import gdt_records
from manager.engines.capsules.capsule_client_compile import coordinate_activity_priority
from test_unit_activity_client import activity_row,machine,asset,baseline


class ActivityPriority(unittest.TestCase):
    def test_only_recognized_collection_and_owned_grid_ids_move(self):
        ordinary=activity_row(0,9301,'sprs/10000.spr')
        collection=client._make_activity_row(bytes(179),145002,990001,100000)
        imported=activity_row(2,9401,'sprs/60000.spr')
        rows,receipt=client.pin_collection_activity_rows([ordinary,collection,imported],movable_machine_ids={9401})
        self.assertEqual(ordinary,rows[0])
        self.assertEqual([9301,9401,990001],[struct.unpack_from('<I',row,5)[0] for row in rows])
        self.assertEqual(imported[4:],rows[1][4:]);self.assertEqual(collection[4:],rows[2][4:])
        self.assertEqual('pinned',receipt['action'])
        again,second=client.pin_collection_activity_rows(rows,movable_machine_ids={9401})
        self.assertEqual(rows,again);self.assertEqual('unchanged',second['action'])

    def test_foreign_or_modified_collection_row_is_not_reordered(self):
        collection=client._make_activity_row(bytes(179),145001,990001,100000)
        foreign=activity_row(1,9401,'sprs/10000.spr')
        with self.assertRaises(client.ClientRegistryError):
            client.pin_collection_activity_rows([collection,foreign],movable_machine_ids=())
        modified=bytearray(collection);modified[13]=1
        with self.assertRaises(client.ClientRegistryError):
            client.pin_collection_activity_rows([bytes(modified),foreign],movable_machine_ids={9401})

    def test_coordinator_rebinds_receipts_and_preserves_all_non_table_resources(self):
        gdt,resources,original=baseline();art=asset();definition=machine(9401,artwork=art)
        result=compile_activity_client(gdt,resources,[definition],[art])
        before=copy.deepcopy(result)
        result=coordinate_activity_priority(result)
        _,rows=gdt_records(result['gdt'])
        self.assertEqual(original[:2],rows[:2])
        self.assertEqual([9000,9001,9401,990001],[struct.unpack_from('<I',row,5)[0] for row in rows])
        for path,raw in before['resources'].items():
            if path!='gdrs/0099.gdt':self.assertEqual(raw,result['resources'][path],path)
        owned=result['ownership'][0]
        self.assertEqual(145003,owned['grid_id'])
        self.assertEqual(hashlib.sha256(rows[2]).hexdigest(),owned['row_sha256'])
        self.assertEqual(2,result['placements'][0]['native_position'])
        self.assertTrue(result['placements'][0]['native_position_predicted'])
        again=coordinate_activity_priority(compile_activity_client(result['gdt'],result['resources'],[definition],[art],ownership=result['ownership']))
        self.assertEqual(result['gdt'],again['gdt'])
        self.assertEqual(result['ownership'],again['ownership'])

    def test_pinning_preserves_existing_owned_tail_order(self):
        collection=client._make_activity_row(bytes(179),145001,990001,100000)
        tail=[activity_row(1,9601,'sprs/60000.spr'),activity_row(2,9401,'sprs/60001.spr')]
        rows,_=client.pin_collection_activity_rows([collection,*tail],movable_machine_ids={9601,9401})
        self.assertEqual([9601,9401,990001],[struct.unpack_from('<I',row,5)[0] for row in rows])
        self.assertEqual([row[4:] for row in tail],[row[4:] for row in rows[:2]])


if __name__=='__main__':unittest.main()
