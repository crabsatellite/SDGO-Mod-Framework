import copy
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from owner.units.repository import SKILL_COLUMNS
from owner.units.server_dependencies import canonical, row_hash
from owner.units.skill_identity_runtime import prepare, compile_files


def fixture():
    source = {key: 0 for key in SKILL_COLUMNS}
    source.update(SkillID=70207, SkillName='Example flag skill', Scale=1.0)
    target = dict(source, SkillID=9800123)
    raw = bytearray(1132); struct.pack_into('<I', raw, 0, 9800123)
    record = bytes(raw); digest = hashlib.sha256(record).hexdigest()
    request = {'schema': 'sdgo.unit-skill-identities/1', 'owner': 'example', 'revision': 'a'*64,
               'bindings': [{'target': 9800123, 'source': 70207, 'client_sha256': digest}]}
    ledger = {'owner': 'example', 'revision': 'a'*64, 'client_hash': digest,
              'installed_hash': row_hash(target),
              'definition_json': canonical({'kind': 'template', 'id': 70207, 'sha256': row_hash(source)})}
    return [request], {9800123: record}, {70207: source, 9800123: target}, {9800123: ledger}


def prepared(value=None, **changes):
    requests, records, rows, ownership = value or fixture()
    return prepare(changes.get('requests', requests), changes.get('records', records),
                   server_rows=changes.get('rows', rows), ownership=changes.get('ownership', ownership))


class SkillIdentityRuntime(unittest.TestCase):
    def test_complete_installed_definition_and_independent_original_ids_are_bound(self):
        result = prepared(); self.assertEqual([70207], result['original_ids'])
        skill = result['owners'][0]['skills'][0]
        self.assertEqual((9800123, 70207), (skill['target'], skill['source']))
        candidate, authority = compile_files(result)
        self.assertEqual((b'SDGOSI1\0', 1, 64, 1, len(candidate)-64), struct.unpack_from('<8s4I', candidate))
        self.assertEqual((b'SDGOSA1\0', 1, 128, 1, 1, len(authority)-128), struct.unpack_from('<8s5I', authority))
        self.assertEqual(hashlib.sha256(candidate).digest(), authority[28:60])
        self.assertEqual(candidate[64:], authority[132:])
        self.assertFalse(result['ordinary_publication_allowed'])

    def test_owner_flags_and_revision_claims_never_replace_actual_ownership(self):
        requests, records, rows, ownership = fixture()
        for field, value in [('owner', 'another'), ('revision', 'b'*64)]:
            changed=copy.deepcopy(requests);changed[0][field]=value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'ownership/revision'):
                prepared((changed, records, rows, ownership))
        changed=copy.deepcopy(requests);changed[0]['native_registration_allowed']=True
        with self.assertRaisesRegex(ValueError, 'fields differ'):prepared((changed, records, rows, ownership))
        with self.assertRaisesRegex(ValueError, 'allocation/original'):
            prepared((requests, records, rows, {}))

    def test_source_template_hash_and_current_server_row_require_exact_readback(self):
        value=fixture();value[2][9800123]['Attack']=1
        with self.assertRaisesRegex(ValueError, 'outside its ownership'):prepared(value)
        value=fixture();value[2][70207]['Attack']=1
        with self.assertRaisesRegex(ValueError, 'installed complete stock template'):prepared(value)

    def test_unselected_original_text_is_preserved_without_weakening_selected_rows(self):
        value=fixture();baseline=dict(value[2][70207],SkillID=79999,SkillName='Legacy\0padding')
        value[2][79999]=baseline
        result=prepared(value)
        self.assertEqual([70207,79999],result['original_ids'])
        self.assertEqual('Legacy\0padding',value[2][79999]['SkillName'])
        value[2][70207]['SkillName']='Invalid\0selected template'
        with self.assertRaises(ValueError):prepared(value)
        value=fixture();value[3][9800123]['definition_json']=canonical({'kind':'explicit','origin':'authored','values':value[2][9800123]})
        with self.assertRaisesRegex(ValueError, 'installed complete stock template'):prepared(value)

    def test_a_default_unknown_or_owned_template_is_not_an_original_selector(self):
        for source in (70205, 71097, 9800123):
            value=fixture();value[0][0]['bindings'][0]['source']=source
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, 'allocation/original'):prepared(value)

    def test_client_records_cannot_change_id_projection_dispatch_or_complete_hash(self):
        for offset in (0, 105, 1131):
            value=fixture();record=bytearray(value[1][9800123]);record[offset]=165
            value[1][9800123]=bytes(record)
            with self.subTest(offset=offset), self.assertRaises(ValueError):prepared(value)
        value=fixture();record=bytearray(value[1][9800123]);record[1131]=165
        digest=hashlib.sha256(record).hexdigest();value[1][9800123]=bytes(record)
        value[0][0]['bindings'][0]['client_sha256']=digest;value[3][9800123]['client_hash']=digest
        with self.assertRaisesRegex(ValueError, 'unknown client dispatch'):prepared(value)

    def test_installed_hashes_do_not_hide_a_client_server_field_mismatch(self):
        value=fixture();record=bytearray(value[1][9800123]);record[155]=1
        digest=hashlib.sha256(record).hexdigest();value[1][9800123]=bytes(record)
        value[0][0]['bindings'][0]['client_sha256']=digest;value[3][9800123]['client_hash']=digest
        with self.assertRaisesRegex(ValueError,'shared skill field'):prepared(value)

    @unittest.skipUnless(os.environ.get('SDGO_SKILL_IDENTITY_VALIDATOR'), 'set the built native identity validator')
    def test_producer_files_are_consumed_by_the_actual_native_parser_and_policy(self):
        candidate, authority=compile_files(prepared())
        with tempfile.TemporaryDirectory(prefix='sdgo-identity-format-') as name:
            root=Path(name);(root/'candidate.bin').write_bytes(candidate);(root/'authority.bin').write_bytes(authority)
            result=subprocess.run([os.environ['SDGO_SKILL_IDENTITY_VALIDATOR'],str(root/'candidate.bin'),str(root/'authority.bin')],
                                  capture_output=True,text=True,timeout=30)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)


if __name__ == '__main__':unittest.main()
