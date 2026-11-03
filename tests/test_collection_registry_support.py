"""Collection authoring works from kits and an installed frozen Manager."""
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from owner.collection_capsule import client

class RegistrySupportTests(unittest.TestCase):
    def test_owner_kit_provider_precedes_frozen_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);kit=root/'kit';provider=kit/'support/capsule-registry/capsule_catalog.dll'
            provider.parent.mkdir(parents=True);provider.write_bytes(b'owner-provider')
            embedded=root/'frozen/collection-support/capsule_catalog.dll'
            embedded.parent.mkdir(parents=True);embedded.write_bytes(b'embedded-provider')
            with mock.patch.object(client.sys,'frozen',True,create=True),mock.patch.object(client.sys,'_MEIPASS',str(root/'frozen'),create=True):
                self.assertEqual(client.registry_support(kit),provider)
                self.assertEqual(client.registry_support(root/'fresh-release'),embedded)

    def test_source_tools_require_an_actual_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(client.sys,'frozen',False,create=True):
                with self.assertRaisesRegex(ValueError,'missing the shared'):
                    client.registry_support(Path(directory))

if __name__=='__main__':unittest.main()
