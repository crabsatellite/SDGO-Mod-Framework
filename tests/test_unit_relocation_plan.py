import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from owner.units.relocation import RelocationError, plan


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


class RelocationPlanTests(unittest.TestCase):
    def setUp(self):
        unit = bytearray(range(32)); struct.pack_into('<I', unit, 0, 100); struct.pack_into('<I', unit, 8, 200)
        weapon = bytearray(range(32)); struct.pack_into('<I', weapon, 0, 200)
        self.blobs = {'unit:100:1': bytes(unit), 'weapon:200:2': bytes(weapon)}
        self.request = {'schema': 'sdgo:record-relocation-request/1', 'owner': 'example', 'revision': 'fixture-v1',
            'allocation': {'ranges': [{'first': 1000, 'last': 1100}], 'preserveUnobservedSourceIds': True},
            'records': [dict(key=k, kind=kind, id=identity, bytes=len(self.blobs[k]), sha256=sha(self.blobs[k])) for k,kind,identity in [('unit:100:1',2,100),('weapon:200:2',10,200)]],
            'references': [{'record':'unit:100:1','offset':8,'sourceId':200,'targetKind':10,'target':'weapon:200:2','role':'ordinary-weapon'}],
            'referenceInventoryComplete': False, 'unresolved':[{'record':'weapon:200:2','reason':'other typed dependencies are not part of this fixture'}]}
        self.snapshot = {'schema':'sdgo:record-key-snapshot/1','revision':'target-v1','occupiedIds':[100,200,1000],
            'typedRecords':[{'kind':85,'id':200,'sha256':'a'*64}],
            'coverage':{'nativeAllTables':False,'serverAllKinds':False,'otherOwnerReservations':False},
            'sources':[{'role':'neutral-fixture-catalog','sha256':'b'*64}]}

    def run_plan(self, request=None, snapshot=None, blobs=None):
        a,b=encoded(self.request if request is None else request),encoded(self.snapshot if snapshot is None else snapshot)
        return plan(a,b,self.blobs if blobs is None else blobs,request_sha256=sha(a),snapshot_sha256=sha(b))

    def test_global_collision_rewrites_identity_and_reference_only(self):
        result, raw = self.run_plan(); requests={r['key']:r for r in result['requests']}
        self.assertEqual(requests['unit:100:1']['requestedTargetId'],1001)
        self.assertEqual(requests['weapon:200:2']['requestedTargetId'],1002)
        self.assertEqual(struct.unpack_from('<I',raw['unit:100:1'],8)[0],1002)
        self.assertEqual(raw['unit:100:1'][4:8],self.blobs['unit:100:1'][4:8])
        self.assertEqual(raw['unit:100:1'][12:],self.blobs['unit:100:1'][12:])
        self.assertEqual(raw['weapon:200:2'][4:],self.blobs['weapon:200:2'][4:])
        self.assertEqual(result['requests'][1]['sourceSha256'],sha(self.blobs['weapon:200:2']))

    def test_source_unobserved_ids_are_preserved_before_range_assignment(self):
        self.snapshot['occupiedIds']=[100]; self.snapshot['typedRecords']=[]
        self.request['allocation']['ranges']=[{'first':200,'last':202}]
        result,_=self.run_plan(); self.assertEqual({r['key']:r['requestedTargetId'] for r in result['requests']},{'unit:100:1':201,'weapon:200:2':200})
        self.assertIn('not-observed',result['requests'][1]['disposition'])

    def test_record_and_reference_order_do_not_choose_different_ids(self):
        a,rawa=self.run_plan(); self.request['records'].reverse(); self.request['references'].reverse(); b,rawb=self.run_plan()
        self.assertEqual(a['requests'],b['requests']); self.assertEqual(a['references'],b['references']); self.assertEqual(rawa,rawb)

    def test_same_numeric_source_id_is_not_an_implicit_target_choice(self):
        extra=bytearray(self.blobs['weapon:200:2']); extra[20]^=1; self.blobs['weapon:200:3']=bytes(extra)
        self.request['records'].append(dict(key='weapon:200:3',kind=10,id=200,bytes=32,sha256=sha(bytes(extra))))
        result,raw=self.run_plan(); ids={r['key']:r['requestedTargetId'] for r in result['requests']}
        self.assertNotEqual(ids['weapon:200:2'],ids['weapon:200:3']); self.assertEqual(struct.unpack_from('<I',raw['unit:100:1'],8)[0],ids['weapon:200:2'])

    def test_complete_snapshot_still_never_issues_authority(self):
        self.snapshot['coverage']={k:True for k in self.snapshot['coverage']}; self.request['referenceInventoryComplete']=True
        result,_=self.run_plan()
        for field in ['allocationAuthorityIssued','idsReserved','nativeRecordRegistrationAllowed','activationAllowed']:self.assertFalse(result[field])
        self.assertTrue(result['requiresInstallerLockAndCAS']);self.assertTrue(result['requiresOwnerReservationAndRuntimeReadback'])

    def test_policy_can_request_new_ids_without_changing_source_values(self):
        self.snapshot['occupiedIds']=[];self.snapshot['typedRecords']=[];self.request['allocation']['preserveUnobservedSourceIds']=False
        result,_=self.run_plan();self.assertEqual([r['requestedTargetId'] for r in result['requests']],[1000,1001]);self.assertEqual([r['sourceId'] for r in result['requests']],[100,200])

    def test_external_ref_retains_id_and_requires_declared_kind_hash(self):
        self.request['references']=[dict(record='unit:100:1',offset=8,sourceId=200,targetKind=85,externalId=200,expectedExternalSha256='a'*64)]
        result,raw=self.run_plan(); self.assertEqual(struct.unpack_from('<I',raw['unit:100:1'],8)[0],200);self.assertTrue(result['references'][0]['identityObserved'])
        self.request['references'][0]['expectedExternalSha256']='c'*64
        with self.assertRaises(RelocationError):self.run_plan()

    def test_stale_snapshot_or_request_bytes_are_rejected(self):
        a,b=encoded(self.request),encoded(self.snapshot)
        for kwargs in [dict(request_sha256='0'*64,snapshot_sha256=sha(b)),dict(request_sha256=sha(a),snapshot_sha256='0'*64)]:
            with self.assertRaises(RelocationError):plan(a,b,self.blobs,**kwargs)

    def test_corrupt_actual_source_blob_is_rejected(self):
        blobs=dict(self.blobs);blobs['weapon:200:2']=b'\0'*32
        with self.assertRaises(RelocationError):self.run_plan(blobs=blobs)

    def test_reference_overlap_and_identity_overlap_are_rejected(self):
        for offset in [0,7,29]:
            request=copy.deepcopy(self.request); request['references'][0]['offset']=offset
            with self.assertRaises(RelocationError):self.run_plan(request=request)
        self.request['references']*=2
        with self.assertRaises(RelocationError):self.run_plan()

    def test_typed_ref_cannot_point_to_wrong_kind(self):
        self.request['references'][0]['targetKind']=2
        with self.assertRaises(RelocationError):self.run_plan()

    def test_invented_reference_value_is_rejected(self):
        self.request['references'][0]['sourceId']=201
        with self.assertRaises(RelocationError):self.run_plan()

    def test_exhausted_or_overlapping_range_is_rejected(self):
        self.request['allocation']['ranges']=[{'first':1000,'last':1001}]
        with self.assertRaises(RelocationError):self.run_plan()
        self.request['allocation']['ranges']=[{'first':1000,'last':1002},{'first':1002,'last':1003}]
        with self.assertRaises(RelocationError):self.run_plan()

    def test_unknown_elevation_fields_and_malformed_refs_are_rejected(self):
        request=copy.deepcopy(self.request);request['activationAllowed']=True
        with self.assertRaises(RelocationError):self.run_plan(request=request)
        for reference in [None,5,{'record':'unit:100:1','offset':'eight'}]:
            request=copy.deepcopy(self.request);request['references']=[reference]
            with self.assertRaises(RelocationError):self.run_plan(request=request)

    def test_duplicate_json_key_and_nonfinite_metadata_are_rejected(self):
        raw=encoded(self.request); bad=raw[:-1]+b',"owner":"other"}'
        with self.assertRaises(RelocationError):plan(bad,encoded(self.snapshot),self.blobs,request_sha256=sha(bad),snapshot_sha256=sha(encoded(self.snapshot)))
        bad=raw[:-1]+b',"evidence":{"bad":NaN}}'
        with self.assertRaises(RelocationError):plan(bad,encoded(self.snapshot),self.blobs,request_sha256=sha(bad),snapshot_sha256=sha(encoded(self.snapshot)))


    def kind_policy(self, first=40, last=90, maximum=90):
        self.request['schema']='sdgo:record-relocation-request/2'
        self.request['allocation']['kindPolicies']=[dict(kind=2,ranges=[dict(first=first,last=last)],maxTargetId=maximum)]

    def test_kind_limit_changes_unit_range_and_all_declared_references_only(self):
        self.kind_policy()
        result,raw=self.run_plan()
        rows={r['key']:r for r in result['requests']}
        self.assertEqual(rows['unit:100:1']['requestedTargetId'],40)
        self.assertEqual(rows['weapon:200:2']['requestedTargetId'],1001)
        self.assertEqual(struct.unpack_from('<I',raw['unit:100:1'],8)[0],1001)
        self.assertEqual(raw['unit:100:1'][12:],self.blobs['unit:100:1'][12:])
        self.assertEqual(result['schema'],'sdgo:record-relocation-plan/2')
        self.assertEqual(result['allocationPolicy'],self.request['allocation'])
        self.assertFalse(result['idsReserved'])

    def test_kind_limit_also_applies_to_unobserved_source_id_preservation(self):
        self.kind_policy();self.snapshot['occupiedIds']=[];self.snapshot['typedRecords']=[]
        result,_=self.run_plan()
        self.assertEqual([r['requestedTargetId'] for r in result['requests']],[40,200])

    def test_kind_ranges_still_share_one_global_collision_space(self):
        self.kind_policy(first=1001,last=1002,maximum=1002)
        result,_=self.run_plan()
        self.assertEqual([r['requestedTargetId'] for r in result['requests']],[1001,1002])

    def test_invalid_or_legacy_kind_policy_is_refused(self):
        self.kind_policy()
        for change in ('legacy','over-limit','duplicate','empty','exhausted'):
            request=copy.deepcopy(self.request)
            if change=='legacy':request['schema']='sdgo:record-relocation-request/1'
            elif change=='over-limit':request['allocation']['kindPolicies'][0]['maxTargetId']=39
            elif change=='duplicate':request['allocation']['kindPolicies']*=2
            elif change=='empty':request['allocation']['kindPolicies']=[]
            else:request['allocation']['kindPolicies'][0].update(ranges=[dict(first=100,last=100)],maxTargetId=100)
            with self.subTest(change=change),self.assertRaises(RelocationError):self.run_plan(request=request)

    def test_kind_policy_is_deterministic_under_source_record_reordering(self):
        self.kind_policy();first,raw1=self.run_plan()
        self.request['records'].reverse();second,raw2=self.run_plan()
        self.assertEqual(first['requests'],second['requests']);self.assertEqual(raw1,raw2)

    def test_host_domain_prevents_package_ranges_from_widening_the_target(self):
        self.kind_policy(first=35000,last=46340,maximum=46340)
        a,b=encoded(self.request),encoded(self.snapshot)
        with self.assertRaisesRegex(RelocationError,'host target domain'):
            plan(a,b,self.blobs,request_sha256=sha(a),snapshot_sha256=sha(b),target_domains={2:(10001,30000)})
        self.kind_policy(first=27000,last=35000,maximum=46340)
        a=encoded(self.request)
        result,_=plan(a,b,self.blobs,request_sha256=sha(a),snapshot_sha256=sha(b),target_domains={2:(10001,30000)})
        self.assertEqual(27000,result['requests'][0]['requestedTargetId'])
        self.assertEqual([{'kind':2,'first':10001,'last':30000}],result['targetDomains'])
        self.assertFalse(result['allocationAuthorityIssued'])

    def test_host_lower_bound_also_applies_when_source_identity_is_unobserved(self):
        self.snapshot.update(occupiedIds=[],typedRecords=[])
        self.kind_policy(first=27000,last=30000,maximum=30000)
        a,b=encoded(self.request),encoded(self.snapshot)
        result,_=plan(a,b,self.blobs,request_sha256=sha(a),snapshot_sha256=sha(b),target_domains={2:(10001,30000)})
        self.assertEqual([27000,200],[row['requestedTargetId'] for row in result['requests']])

    def test_external_unit_reference_cannot_bypass_host_domain(self):
        self.kind_policy(first=27000,last=30000,maximum=30000)
        self.request['references']=[{'record':'unit:100:1','offset':8,'sourceId':200,'targetKind':2,'externalId':200}]
        a,b=encoded(self.request),encoded(self.snapshot)
        with self.assertRaisesRegex(RelocationError,'external reference'):
            plan(a,b,self.blobs,request_sha256=sha(a),snapshot_sha256=sha(b),target_domains={2:(10001,30000)})


if __name__=='__main__':unittest.main()
