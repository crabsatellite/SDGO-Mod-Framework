import hashlib
import struct
import unittest

from owner.units.codec import UNIT_SIZE
from owner.units.model_integrity import bind_model_digest, model_digest, rebind_model_digest


class ModelIntegrity(unittest.TestCase):
    def setUp(self):
        self.source = b'\0' * 4 + b'ZOA\0' + b'original source model'
        self.prepared = self.source + b'owner resource adaptation'
        record = bytearray(UNIT_SIZE)
        struct.pack_into('<I', record, 0, 9800101)
        record[672:736] = model_digest(self.source)
        self.record = bytes(record)

    def test_four_word_digest_and_exact_carrier_rebind(self):
        digest = hashlib.md5(self.source, usedforsecurity=False).digest()
        expected = ''.join(digest[i:i+4][::-1].hex().upper() for i in range(0, 16, 4))
        self.assertEqual(model_digest(self.source).decode('utf-16-le'), expected)
        output = rebind_model_digest(self.record, self.source, self.prepared)
        self.assertEqual(output[:672], self.record[:672])
        self.assertEqual(output[736:], self.record[736:])
        self.assertEqual(output[672:736], model_digest(self.prepared))
        self.assertEqual(bind_model_digest(self.record,self.prepared),output)
        self.assertEqual(rebind_model_digest(self.record, self.source, self.source), self.record)

    def test_wrong_source_unknown_model_and_invalid_record_refuse(self):
        with self.assertRaisesRegex(ValueError, 'authenticated source'):
            rebind_model_digest(self.record, self.prepared, self.source)
        for invalid in (b'', b'unsupported' * 10):
            with self.assertRaises(ValueError):
                rebind_model_digest(self.record, self.source, invalid)
        for invalid in (self.record[:-1], self.record[:4] + b'\1' + self.record[5:]):
            with self.assertRaises(ValueError):
                rebind_model_digest(invalid, self.source, self.prepared)


if __name__ == '__main__':
    unittest.main()
