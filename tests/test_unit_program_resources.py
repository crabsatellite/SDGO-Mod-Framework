"""Script-only dependencies must be checked before catalogue mutation."""
from pathlib import Path
from types import SimpleNamespace
import configparser
import hashlib
import json
import tempfile
import unittest
from unittest.mock import patch

from owner.native_archive import build_archive
from owner.units import batch_server, compiler, native_programs, program_resources
from test_unit_native_programs import chunk, fixture


class ProgramResources(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.module=self.root/'package/client/sdgo/mods/unit_catalog'
        self.module.mkdir(parents=True)
        manifest,files,weapons=fixture()
        program=chunk(body_constants=[b'fxrs/example_beam.zfx']);files['example.scp']=program
        manifest['programs'][0]['sha256']=hashlib.sha256(program).hexdigest()
        catalog=compiler.native_catalog([(10,k,v)for k,v in weapons.items()])
        self.write('catalog.bin',catalog)
        self.write('native-programs.bin',native_programs.compile_bundle([manifest],hashlib.sha256(catalog).hexdigest(),files,weapons))
        self.publication=SimpleNamespace(root=self.root,records={10:weapons})
        self.seal()

    def write(self,name,data):
        (self.module/name).write_bytes(data)

    def seal(self):
        value={'files':{'client/sdgo/mods/unit_catalog/'+p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in self.module.iterdir()if p.is_file()}}
        (self.root/'package/manifest.json').write_text(json.dumps(value),encoding='utf-8')

    def overlay(self,resources):
        index,archive,_=build_archive(resources,lambda path:path.startswith('fxrs/'))
        self.write('resources.zdx',index);self.write('resources.zpk',archive)
        cfg=configparser.ConfigParser();cfg['runtime']={'schema':'1','index':'resources.zdx','archive':'resources.zpk',
            'index_sha256':hashlib.sha256(index).hexdigest(),'archive_sha256':hashlib.sha256(archive).hexdigest()}
        with (self.module/'resource-overlay.ini').open('w',encoding='utf-8')as stream:cfg.write(stream)
        self.seal()

    def test_missing_script_literal_refuses_without_a_target_or_shipped_resource(self):
        with self.assertRaisesRegex(ValueError,'example_beam.zfx'):
            program_resources.validate_publication(self.publication)

    def test_published_resource_is_authenticated_and_not_gameplay_approval(self):
        self.overlay({'fxrs/example_beam.zfx':b'non-executed resource fixture'})
        report=program_resources.validate_publication(self.publication)
        self.assertEqual(1,report['programs']);self.assertFalse(report['actor_accepted'])
        self.assertIn('fxrs/example_beam.zfx',report['resources'])
        with (self.module/'resources.zpk').open('r+b')as stream:stream.seek(16);stream.write(b'changed')
        with self.assertRaisesRegex(ValueError,'SHA-256 differs'):
            program_resources.validate_publication(self.publication)

    def test_compatible_original_resource_uses_the_actual_target_reader(self):
        seen=[]
        def read(path):seen.append(path);return b'original byte fixture'
        report=program_resources.validate_publication(self.publication,SimpleNamespace(read=read))
        self.assertEqual(['fxrs/example_beam.zfx'],seen)
        self.assertEqual(hashlib.sha256(b'original byte fixture').hexdigest(),report['resources'][seen[0]]['sha256'])

    def test_a_sidecar_from_another_catalog_is_rejected(self):
        changed=bytearray((self.module/'catalog.bin').read_bytes());changed[-1]^=1
        self.write('catalog.bin',bytes(changed));self.seal()
        with self.assertRaisesRegex(ValueError,'different catalog'):
            program_resources.validate_publication(self.publication,SimpleNamespace(read=lambda _:b'fixture'))

    def test_apply_refuses_before_opening_the_database_mutation_gate(self):
        with patch.object(batch_server,'load_publication',return_value=self.publication), \
             patch('manager.operations.server_change')as gate:
            with self.assertRaisesRegex(ValueError,'example_beam.zfx'):
                batch_server.apply(self.root,self.root/'WorldServer.ini')
            gate.assert_not_called()

    def test_duplicate_json_hash_inventory_is_rejected(self):
        path=self.root/'package/manifest.json'
        path.write_text('{"files":{},"files":{}}',encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'key repeats'):
            program_resources.validate_publication(self.publication)

    def test_nonstandard_json_constants_are_rejected(self):
        (self.root/'package/manifest.json').write_text('{"files":{},"extra":NaN}',encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'constant is invalid'):
            program_resources.validate_publication(self.publication)

    def test_linked_package_parent_is_rejected(self):
        import os
        import subprocess
        source=self.root/'package'
        target=self.root/'real-package';source.rename(target)
        if os.name=='nt':
            result=subprocess.run(['cmd','/c','mklink','/J',str(source),str(target)],capture_output=True)
            if result.returncode:self.skipTest('junction creation is unavailable')
            self.addCleanup(lambda:os.rmdir(source))
        else:source.symlink_to(target,target_is_directory=True)
        with self.assertRaisesRegex(ValueError,'linked path'):
            program_resources.validate_publication(self.publication)

    def test_oversized_sidecar_refused_before_decoding(self):
        with patch.object(native_programs,'MAX_BYTES',1):
            with self.assertRaisesRegex(ValueError,'byte budget'):
                program_resources.validate_publication(self.publication)


if __name__=='__main__':unittest.main()
