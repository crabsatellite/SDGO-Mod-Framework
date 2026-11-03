import tempfile
import unittest
from pathlib import Path

from owner.native_archive import build_archive
from owner.collection_capsule.client import ArchiveReader,build_archive as capsule_archive,ClientRegistryError


class NativeArchiveTests(unittest.TestCase):
    def test_native_reader_resolves_provider_resources_and_exact_bytes(self):
        resources={'mdrs/family/body.mod':b'ZOA-model-data'*100,'anrs/ready.ani':b'animation',
                   'txrs/body.txr':b'texture','scp/attack.scp':b'script'}
        index,archive,manifest=build_archive(resources,lambda p:p.split('/')[0] in ('mdrs','anrs','txrs','scp'))
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'r.zdx').write_bytes(index);(root/'r.zpk').write_bytes(archive)
            consumer=ArchiveReader(root/'r.zdx',root/'r.zpk')
            for path,raw in resources.items():self.assertEqual(consumer.read(path),raw)
            self.assertFalse(consumer.has('mdrs/absent.mod'))
        self.assertEqual({e['path'] for e in manifest['entries']},set(resources))

    def test_provider_policies_remain_independent(self):
        with self.assertRaises(ClientRegistryError):capsule_archive({'mdrs/unit.mod':b'model'})
        capsule_archive({'sprs/60000.spr':b'owned-sprite'})
        with self.assertRaises(ValueError):build_archive({'mdrs/unit.mod':b'model'},lambda p:p.startswith('txrs/'))

    def test_ambiguous_and_escaping_paths_are_rejected(self):
        for resources in ({'mdrs/../body.mod':b'x'},{'mdrs/body.mod':b'x','MDRS\\BODY.MOD':b'y'},
                          {'mdrs/body.mod.':b'x'},{'C:/body.mod':b'x'},{'mdrs/body\x00.mod':b'x'}):
            with self.subTest(paths=list(resources)),self.assertRaises(ValueError):build_archive(resources,lambda _:True)


if __name__=='__main__':unittest.main()
