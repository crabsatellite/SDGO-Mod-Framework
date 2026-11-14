import hashlib
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from owner.units import compiler,batch_server,skill_parameters
from owner.units.indexes import catalog_records
from owner.units.profiles.skill_parameters_89 import validate_record


def parameter(identity=8000789):
    value=bytearray(89);struct.pack_into('<I',value,0,identity)
    struct.pack_into('<6f',value,65,1,1,1,1,1,1);return bytes(value)


class SkillParameters(unittest.TestCase):
    def publication(self,supply=True):
        value=bytearray(1132);struct.pack_into('<I',value,0,74020);struct.pack_into('<I',value,284,8000789)
        return SimpleNamespace(records={13:{74020:bytes(value)},79:{8000789:parameter()}if supply else {}})

    def test_complete_record_roundtrips_without_inventing_defaults(self):
        raw=parameter();catalog=compiler.native_catalog([(79,8000789,raw)])
        self.assertEqual({8000789:(79,raw)},catalog_records(catalog))
        self.assertEqual(hashlib.sha256(raw).hexdigest(),validate_record(raw)['sha256'])
        for invalid in (raw[:-1],raw+b'\0',bytes(4)+raw[4:],raw[:4]+b'\1'+raw[5:]):
            with self.assertRaises(ValueError):validate_record(invalid)
        bad=bytearray(raw);struct.pack_into('<f',bad,85,float('nan'))
        with self.assertRaisesRegex(ValueError,'non-finite'):validate_record(bytes(bad))

    def test_shipped_dependency_is_resolved_from_its_actual_bytes(self):
        result=skill_parameters.validate_publication(self.publication())
        self.assertTrue(result['complete']);self.assertFalse(result['actor_accepted'])
        self.assertEqual('published',result['records'][8000789]['source'])
        self.assertEqual(284,result['references'][0]['offset'])

    def test_missing_record_refuses_before_database_mutation_gate(self):
        with patch.object(batch_server,'load_publication',return_value=self.publication(False)),patch('manager.operations.server_change')as gate:
            with self.assertRaisesRegex(ValueError,'8000789'):
                batch_server.apply(Path('.'),Path('WorldServer.ini'))
            gate.assert_not_called()

    def test_original_reuse_reads_and_checks_the_typed_target(self):
        calls=[]
        def read(kind,identity):calls.append((kind,identity));return parameter(identity)
        result=skill_parameters.validate_publication(self.publication(False),SimpleNamespace(record=read))
        self.assertEqual([('skill_parameter',8000789)],calls)
        self.assertEqual('compatible-original',result['records'][8000789]['source'])
        with self.assertRaises(ValueError):
            skill_parameters.validate_publication(self.publication(False),SimpleNamespace(record=lambda *_:parameter(8000790)))

    def test_wrong_published_kind_cannot_fall_back_to_an_original(self):
        publication=self.publication(False);publication.records[12]={8000789:b'not parameters'}
        with self.assertRaisesRegex(ValueError,'another published record kind'):
            skill_parameters.validate_publication(publication,SimpleNamespace(record=lambda *_:parameter()))

    def test_changed_parameter_version_cannot_replace_the_original(self):
        changed=bytearray(parameter());struct.pack_into('<f',changed,65,2.0)
        with self.assertRaisesRegex(ValueError,'conflicts with the compatible original'):
            skill_parameters.validate_publication(self.publication(),SimpleNamespace(record=lambda *_:bytes(changed)))
        result=skill_parameters.validate_publication(self.publication(),SimpleNamespace(record=lambda *_:parameter()))
        self.assertTrue(result['complete'])

    def test_original_cross_kind_collision_refuses_before_database_mutation(self):
        baseline=SimpleNamespace(record=lambda *_:None,any_id=lambda identity:identity==8000789)
        with patch.object(batch_server,'load_publication',return_value=self.publication()),patch('manager.operations.server_change')as gate:
            with self.assertRaisesRegex(ValueError,'another compatible-original record kind'):
                batch_server.apply(Path('.'),Path('WorldServer.ini'),baseline=baseline)
            gate.assert_not_called()

    def test_batch_transport_rejects_cross_kind_global_duplicates_and_bad_order(self):
        def container(entries):
            payload=b''.join(struct.pack('<6I',kind,identity,len(raw),compiler.fnv(raw),0,0)+raw for kind,identity,raw in entries)
            return struct.pack('<8s6I',b'SDGOUN1\0',1,32,len(entries),len(payload),compiler.fnv(payload),0)+payload
        feature=bytearray(1559);struct.pack_into('<I',feature,0,8000789)
        feature[5:5+len('Training'.encode('utf-16le'))]='Training'.encode('utf-16le')
        cases=[[(12,8000789,bytes(feature)),(79,8000789,parameter())],
               [(79,8000790,parameter(8000790)),(79,8000789,parameter())]]
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);path=root/batch_server.CATALOG;path.parent.mkdir(parents=True)
            for entries in cases:
                path.write_bytes(container(entries))
                with self.assertRaisesRegex(ValueError,'记录身份'):
                    batch_server._catalog_records(root)
            valid=container([(79,8000789,parameter())]);path.write_bytes(valid[:-1]+bytes([valid[-1]^1]))
            with self.assertRaisesRegex(ValueError,'校验'):
                batch_server._catalog_records(root)


if __name__=='__main__':unittest.main()
