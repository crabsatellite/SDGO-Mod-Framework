from pathlib import Path
import configparser
import tempfile
import unittest
import uuid
from owner.rewards.wallet_binding import publish


class BindingTests(unittest.TestCase):
    def test_verified_scope_is_persistent_and_does_not_depend_on_a_heartbeat(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);scope=uuid.uuid4();binding=publish(root,scope)
            first=binding.read_bytes()
            self.assertIn(str(scope).encode(),first)
            self.assertFalse((root/'wallet-status.ini').exists())
            publish(root,scope)
            self.assertEqual(binding.read_bytes(),first)
            with self.assertRaises(ValueError):publish(root,uuid.uuid4())
            self.assertEqual(binding.read_bytes(),first)

    def test_invalid_or_foreign_binding_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);binding=root/'wallet-binding.ini'
            for value in [b'invalid',b'[binding]\nSchemaVersion=1\nProtocol=other\nScopeId=none\n']:
                binding.write_bytes(value)
                with self.assertRaises((ValueError,configparser.Error)):publish(root,uuid.uuid4())
                self.assertEqual(binding.read_bytes(),value)


if __name__=='__main__':unittest.main()
