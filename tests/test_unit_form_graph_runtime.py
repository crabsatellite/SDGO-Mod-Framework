import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from owner.units import repository as repo
from owner.units.codec import decode_unit
from owner.units.form_graph_runtime import SCHEMA, prepare, compile_files
from unit_import_support import unit


def ledger(kind, identity, row, columns):
    return {'record_kind':kind, 'record_id':identity,'owner_key':repo.OWNER_KEY,'family':'example.family',
            'record_key':f'example.family/{kind}/{identity}','revision':'source-r1','original_json':'null',
            'installed_hash':repo._row_hash(row,columns),'updated_at':'2026-01-01T00:00:00'}


def fixture(edges=None):
    edges = edges or {15001: 15002, 15002: 15003, 15003: 15002}
    records = {identity: unit(identity, target) for identity, target in edges.items()}
    rows, ownership = {}, {}
    for identity, raw in records.items():
        row = {key: 0 for key in repo.UNIT_COLUMNS}
        row.update({key: value for key, value in decode_unit(raw)['client'].items() if key in row})
        row['UnitGrowIndex'] = 1;rows[identity] = row
        ownership[identity] = ledger('unit',identity,row,repo.UNIT_COLUMNS)
    request = {'schema': SCHEMA, 'owner': 'example.family', 'revision': 'source-r1',
               'roots': [15001], 'records': [{'id': identity, 'client_sha256': hashlib.sha256(raw).hexdigest()}
                                           for identity, raw in records.items()]}
    item = dict(zip(repo.ITEM_COLUMNS, (1119000, 'Example unit', 15001, 0, 0, 0)))
    return [request], records, rows, ownership, {}, {1119000:item}, {1119000:ledger('item',1119000,item,repo.ITEM_COLUMNS)}


def prepared(value=None):
    requests, records, rows, ownership, originals, items, item_ownership = value or fixture()
    return prepare(requests, records, server_rows=rows, ownership=ownership, originals=originals,
                   item_rows=items, item_ownership=item_ownership)


class UnitFormGraphRuntime(unittest.TestCase):
    def test_directed_edges_are_derived_from_complete_records_with_no_reverse_or_inventory_inference(self):
        result = prepared(fixture({15001: 15002, 15002: 15003, 15003: 15002, 15004: 15003}))
        owner = result['owners'][0]
        self.assertEqual([15001], owner['roots'])
        self.assertEqual([(15001, 15002), (15002, 15003), (15003, 15002), (15004, 15003)],
                         [(node['id'], node['target']) for node in owner['nodes']])
        self.assertFalse(result['native_actor_accepted']);self.assertFalse(result['ordinary_publication_allowed'])
        candidate, authority = compile_files(result)
        self.assertEqual((b'SDGOUF1\0', 1, 64, 1, len(candidate)-64), struct.unpack_from('<8s4I', candidate))
        self.assertEqual((b'SDGOUA1\0', 1, 128, 1, len(authority)-128), struct.unpack_from('<8s4I', authority))
        self.assertEqual(hashlib.sha256(candidate).digest(), authority[24:56])
        self.assertEqual(candidate[64:], authority[128:])

    def test_request_flags_do_not_replace_ledger_ownership_revision_or_readback(self):
        for field, value in [('owner', 'another'), ('revision', 'other'), ('native_activation_allowed', True)]:
            data = fixture();data[0][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):prepared(data)
        data = fixture();data[2][15002]['HP'] += 1
        with self.assertRaises(ValueError):prepared(data)
        data = fixture();data[3][15002]['owner_key'] = 'another-framework'
        with self.assertRaisesRegex(ValueError, 'ownership/revision'):prepared(data)
        data = fixture();data[3][15002]['installed_hash'] = 'a'*64
        with self.assertRaisesRegex(ValueError, 'outside its ledger'):prepared(data)

    def test_client_hash_and_all_audited_shared_fields_are_bound(self):
        for offset in (0, 316, 321, 575, 585):
            data = fixture();raw = bytearray(data[1][15002]);raw[offset] ^= 1;data[1][15002] = bytes(raw)
            with self.subTest(offset=offset), self.assertRaises(ValueError):prepared(data)
        data = fixture();raw = bytearray(data[1][15002]);raw[321] ^= 1;data[1][15002] = bytes(raw)
        data[0][0]['records'][1]['client_sha256'] = hashlib.sha256(raw).hexdigest()
        with self.assertRaisesRegex(ValueError, 'client/server shared fields'):prepared(data)

    def test_only_independently_witnessed_baseline_can_be_reused_without_ownership(self):
        data = fixture();del data[3][15003]
        with self.assertRaisesRegex(ValueError, 'independent witnesses'):prepared(data)
        data[4][15003] = {'client_sha256': hashlib.sha256(data[1][15003]).hexdigest(),
                          'server_sha256': repo._row_hash(data[2][15003], repo.UNIT_COLUMNS)}
        result = prepared(data);self.assertEqual('original', result['owners'][0]['nodes'][-1]['origin'])
        data[0][0]['roots'].append(15003)
        with self.assertRaisesRegex(ValueError, 'root must be an owned'):prepared(data)
        data[0][0]['roots'].pop();data[2][15003]['UnitGraph'] = 1
        with self.assertRaisesRegex(ValueError, 'independent witnesses'):prepared(data)

    def test_partial_foreign_duplicate_or_out_of_domain_graphs_are_refused(self):
        data = fixture();data[0][0]['records'].pop()
        with self.assertRaisesRegex(ValueError, 'references are not closed'):prepared(data)
        data = fixture();data[0][0]['records'].append(copy.deepcopy(data[0][0]['records'][0]))
        with self.assertRaisesRegex(ValueError, 'repeated'):prepared(data)
        data = fixture();data[0][0]['roots'] = [35000]
        with self.assertRaisesRegex(ValueError, 'server domain'):prepared(data)
        data = fixture();data[3][15003]['family'] = 'another-owner';data[3][15003]['record_key'] = 'another-owner/unit/15003'
        with self.assertRaisesRegex(ValueError, 'ownership/revision'):prepared(data)
        data = fixture();data[3][15004] = dict(data[3][15003])
        with self.assertRaisesRegex(ValueError, 'absent from the server snapshot'):prepared(data)

    def test_self_loop_is_retained_as_data_without_claiming_its_native_behavior(self):
        result = prepared(fixture({15001: 15001}))
        self.assertEqual(15001, result['owners'][0]['nodes'][0]['target'])
        self.assertFalse(result['ordinary_publication_allowed'])

    def test_owning_a_form_does_not_authorize_it_as_an_inventory_entry(self):
        data = fixture();data[0][0]['roots'].append(15002)
        with self.assertRaisesRegex(ValueError, 'inventory entry is absent'):prepared(data)
        for field, value in [('RefIndex', 15002), ('Option1', 99), ('CatalogueID', 1519000)]:
            data = fixture();data[5][1119000][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):prepared(data)
        data = fixture();data[6][1119000]['family'] = 'other';data[6][1119000]['record_key'] = 'other/item/1119000'
        with self.assertRaisesRegex(ValueError, 'inventory entry is absent'):prepared(data)

    def test_complete_ledger_identity_and_rollback_data_are_bound_except_audit_time(self):
        original = prepared()['server_fingerprint']
        data = fixture();data[3][15002]['record_key'] = 'example.family/unit/15003'
        with self.assertRaisesRegex(ValueError, 'record identity'):prepared(data)
        data = fixture();data[3][15002]['original_json'] = '{}'
        with self.assertRaisesRegex(ValueError, 'rollback/time'):prepared(data)
        data = fixture();data[3][15002]['original_json'] = json.dumps(data[2][15002])
        self.assertNotEqual(original, prepared(data)['server_fingerprint'])
        data = fixture();data[3][15002]['updated_at'] = '2026-02-01T01:02:03'
        self.assertEqual(original, prepared(data)['server_fingerprint'])
        data = fixture();item = dict(data[5][1119000],CatalogueID=1119001,RefIndex=15004)
        data[5][1119001] = item;data[6][1119001] = ledger('item',1119001,item,repo.ITEM_COLUMNS)
        with_extra = prepared(data)['server_fingerprint'];self.assertNotEqual(original, with_extra)
        data[6][1119001]['original_json'] = json.dumps(item)
        self.assertNotEqual(with_extra, prepared(data)['server_fingerprint'])

    def test_every_snapshot_scalar_is_checked_without_rewriting_readonly_legacy_text(self):
        data = fixture();row = dict(data[2][15003],UnitNumber=16000,UnitModelName='Legacy\0padding')
        data[2][16000] = row;prepared(data);self.assertEqual('Legacy\0padding',data[2][16000]['UnitModelName'])
        data[2][16000]['HP'] = 'invalid'
        with self.assertRaises(ValueError):prepared(data)

    def test_compiler_refuses_tampered_prepared_roots_hashes_and_shared_origins(self):
        data = prepared();data['ordinary_publication_allowed'] = True
        with self.assertRaises(ValueError):compile_files(data)
        data = prepared();data['owners'][0]['roots'] = [15002, 15001]
        with self.assertRaises(ValueError):compile_files(data)
        data = prepared();data['owners'][0]['nodes'][0]['client_sha256'] = 'bad'
        with self.assertRaises(ValueError):compile_files(data)
        data = prepared();other = copy.deepcopy(data['owners'][0]);other['owner'] = 'another'
        for node in other['nodes']:node['origin'] = 'original'
        other['nodes'][0]['origin'] = 'owned';data['owners'].append(other)
        with self.assertRaises(ValueError):compile_files(data)

    @unittest.skipUnless(os.environ.get('SDGO_FORM_GRAPH_VALIDATOR'), 'set the built form graph validator')
    def test_producer_bytes_are_consumed_by_actual_native_parser_and_policy(self):
        candidate, authority = compile_files(prepared())
        with tempfile.TemporaryDirectory(prefix='sdgo-form-graph-') as directory:
            root = Path(directory);(root/'candidate.bin').write_bytes(candidate);(root/'authority.bin').write_bytes(authority)
            result = subprocess.run([os.environ['SDGO_FORM_GRAPH_VALIDATOR'], str(root/'candidate.bin'), str(root/'authority.bin')],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(0, result.returncode, result.stdout+result.stderr)

    @unittest.skipUnless(os.environ.get('SDGO_FORM_GRAPH_VALIDATOR'), 'set the built form graph validator')
    def test_native_parser_refuses_independent_authority_drift_and_resealed_invalid_policy(self):
        candidate, authority = compile_files(prepared())
        owner_size = struct.unpack_from('<I', candidate, 64)[0]
        root_count_at = 64+4+owner_size+32
        roots_at = root_count_at+4
        node_at = roots_at+4+4

        def reseal(raw, host, replace_authority_payload):
            raw[24:56] = hashlib.sha256(raw[64:]).digest()
            if replace_authority_payload:
                host[128:] = raw[64:];host[24:56] = hashlib.sha256(raw).digest()
            return bytes(raw), bytes(host)

        cases = []
        for offset, value, label in [(node_at+4, 15009, 'open graph'), (node_at+8, 0, 'unknown origin'),
                                     (node_at+8, 2, 'original root'), (node_at+76, 15001, 'repeated node'),
                                     (roots_at, 35000, 'invalid root domain')]:
            raw, host = bytearray(candidate), bytearray(authority)
            struct.pack_into('<I', raw, offset, value);raw, host = reseal(raw, host, True)
            cases.append((raw, host, label))
        raw = bytearray(candidate);struct.pack_into('<I', raw, node_at+4, 15003)
        raw, host = reseal(raw, bytearray(authority), False)
        cases.append((raw, host, 'content cannot replace installed authority'))
        host = bytearray(authority);host[88] ^= 1;cases.append((candidate, bytes(host), 'wrong host'))
        host = bytearray(authority);host[120] = 1;cases.append((candidate, bytes(host), 'authority reserved'))
        raw = bytearray(candidate);raw[56] = 1;cases.append((bytes(raw), authority, 'candidate reserved'))
        raw = bytearray(candidate);struct.pack_into('<I', raw, 64, 0xffffffff)
        raw, host = reseal(raw, bytearray(authority), True);cases.append((raw, host, 'owner text extent'))
        cases.extend([(candidate[:-1], authority, 'truncated candidate'),
                      (candidate, authority+b'\0', 'authority trailing bytes')])
        with tempfile.TemporaryDirectory(prefix='sdgo-form-graph-negative-') as directory:
            root = Path(directory)
            for raw, host, label in cases:
                with self.subTest(case=label):
                    (root/'candidate.bin').write_bytes(raw);(root/'authority.bin').write_bytes(host)
                    result = subprocess.run([os.environ['SDGO_FORM_GRAPH_VALIDATOR'], str(root/'candidate.bin'), str(root/'authority.bin')],
                        capture_output=True, text=True, timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                    self.assertEqual(1, result.returncode, label+': '+result.stdout+result.stderr)


if __name__ == '__main__':unittest.main()
