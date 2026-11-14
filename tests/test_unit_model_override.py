import io
import unittest
import zipfile
from owner.units.archive import inspect_upload
from owner.units.codec import decode_unit,model_override
from owner.units.compiler import native_catalog
from unit_import_support import Baseline,unit,wchar


class ModelOverrideTests(unittest.TestCase):
    def test_optional_path_is_typed_and_preserves_the_complete_record(self):
        self.assertEqual('',model_override(unit()))
        raw=bytearray(unit());wchar(raw,738,'mdrs/alternate.mod');raw=bytes(raw)
        decoded=decode_unit(raw)
        self.assertEqual(raw,decoded['raw'])
        self.assertEqual('mdrs/alternate.mod',decoded['client']['model_override'])
        self.assertIn('mdrs/alternate.mod',decoded['resource_refs'])
        self.assertTrue(native_catalog([(2,90001,raw)]))

    def test_non_path_and_unterminated_bytes_are_not_erased_or_accepted(self):
        for text in ('invalid','sprs/portrait.spr','mdrs/../escape.mod'):
            raw=bytearray(unit());wchar(raw,738,text)
            with self.assertRaisesRegex(ValueError,'738'):decode_unit(bytes(raw))
            with self.assertRaisesRegex(ValueError,'738'):native_catalog([(2,90001,bytes(raw))])
        raw=bytearray(unit());raw[738:798]=b'A\0'*30
        with self.assertRaisesRegex(ValueError,'738'):decode_unit(bytes(raw))

    def test_missing_override_is_a_critical_archive_dependency(self):
        class CompleteBaseline(Baseline):
            def exists(self,path):return path=='mdrs/test.mod'
        raw=bytearray(unit());wchar(raw,738,'mdrs/alternate.mod')
        output=io.BytesIO()
        with zipfile.ZipFile(output,'w') as archive:archive.writestr('unit.bin',bytes(raw))
        with self.assertRaisesRegex(ValueError,'override @738 is missing'):
            inspect_upload('example.zip',output.getvalue(),CompleteBaseline())


if __name__=='__main__':unittest.main()
