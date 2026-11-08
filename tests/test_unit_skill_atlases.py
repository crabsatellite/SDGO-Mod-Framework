import copy
import hashlib
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from PIL import Image

from owner.units.assets import sprite_binary
from owner.units.skill_atlases import carried_manifests,compile_bundle,decode_bundle,validate_manifest
from owner.units.compiler import fnv
from owner.native_archive import build_archive


def fixture():
    output=io.BytesIO();Image.new('RGBA',(32,32),'orange').save(output,'PNG');texture=output.getvalue()
    sprite=sprite_binary('txrs/owner_atlas.txr',32,32)
    record=bytearray(1132);struct.pack_into('<I',record,0,9800123)
    name='Neutral skill'.encode('utf-16-le');record[5:5+len(name)]=name
    manifest={'schema':'sdgo.unit-skill-atlases/1','owner':'example','revision':'a'*64,
        'sprites':[{'path':'sprs/owner_atlas.spr','sha256':hashlib.sha256(sprite).hexdigest(),
            'textures':[{'path':'txrs/owner_atlas.txr','sha256':hashlib.sha256(texture).hexdigest()}]}],
        'skills':[{'id':9800123,'record_sha256':hashlib.sha256(record).hexdigest(),'sprite':'sprs/owner_atlas.spr'}]}
    return manifest,{9800123:bytes(record)},{'sprs/owner_atlas.spr':sprite,'txrs/owner_atlas.txr':texture}


class SkillAtlases(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('SDGO_SKILL_ATLAS_VALIDATOR'),'set the built native atlas validator for cross-consumer checking')
    def test_python_sidecar_is_consumed_by_the_native_catalog_and_archive_reader(self):
        manifest,records,resources=fixture()
        with tempfile.TemporaryDirectory(prefix='sdgo-skill-atlas-format-') as directory:
            root=Path(directory);record=records[9800123]
            payload=struct.pack('<6I',13,9800123,len(record),fnv(record),0,0)+record
            catalog=struct.pack('<8s6I',b'SDGOUN1\0',1,32,1,len(payload),fnv(payload),0)+payload
            zdx,zpk,_=build_archive(resources,lambda _:True)
            sha=lambda raw:hashlib.sha256(raw).hexdigest()
            (root/'catalog.bin').write_bytes(catalog);(root/'resources.zdx').write_bytes(zdx);(root/'resources.zpk').write_bytes(zpk)
            (root/'resource-overlay.ini').write_text('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256='+sha(zdx)+'\narchive=resources.zpk\narchive_sha256='+sha(zpk)+'\n',encoding='ascii')
            (root/'skill-atlases.bin').write_bytes(compile_bundle([manifest],sha(catalog),sha(zdx),sha(zpk),records,resources))
            result=subprocess.run([os.environ['SDGO_SKILL_ATLAS_VALIDATOR'],str(root/'skill-atlases.bin'),str(root/'catalog.bin'),str(root)],
                                  capture_output=True,text=True,timeout=30)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)

    def test_catalog_archive_binding_and_exact_frame_texture_inventory(self):
        manifest,records,resources=fixture()
        rows=validate_manifest(manifest,records,resources)
        self.assertEqual((0,1),(rows[0]['frame'],rows[0]['frame_count']))
        binary=compile_bundle([manifest],'b'*64,'c'*64,'d'*64,records,resources)
        self.assertEqual((b'SDGOAT1\0',1,128,1,len(binary)-128),struct.unpack_from('<8s4I',binary))
        self.assertEqual(bytes.fromhex('b'*64+'c'*64+'d'*64),binary[24:120])

    def test_installed_sidecar_decodes_back_to_manifests_that_recompile_byte_identically(self):
        manifest,records,resources=fixture()
        binary=compile_bundle([manifest],'b'*64,'c'*64,'d'*64,records,resources)
        decoded=decode_bundle(binary)
        self.assertEqual(('b'*64,'c'*64,'d'*64),(decoded['catalog_sha256'],decoded['index_sha256'],decoded['archive_sha256']))
        self.assertEqual([(9800123,'example','a'*64,'sprs/owner_atlas.spr',1)],
                         [(row['id'],row['owner'],row['revision'],row['sprite'],row['frame_count']) for row in decoded['entries']])
        carried=carried_manifests(decoded,resources)
        self.assertEqual([manifest],carried)
        self.assertEqual(binary,compile_bundle(carried,'b'*64,'c'*64,'d'*64,records,resources))
        missing=dict(resources);missing.pop('txrs/owner_atlas.txr')
        with self.assertRaisesRegex(ValueError,'texture is absent'):carried_manifests(decoded,missing)
        with self.assertRaisesRegex(ValueError,'sprite is absent'):carried_manifests(decoded,{})
        for bad in (binary+b'\0',binary[:-1],binary[:120]+b'\1'+binary[121:]):
            with self.subTest(size=len(bad)),self.assertRaises(ValueError):decode_bundle(bad)

    def test_owner_atlas_does_not_reuse_the_global_sprite_name(self):
        manifest,records,resources=fixture();sprite=resources['sprs/owner_atlas.spr']
        manifest['sprites'][0]['path']='sprs/0594.spr';resources['sprs/0594.spr']=sprite
        with self.assertRaisesRegex(ValueError,'independent'):validate_manifest(manifest,records,resources)

    def test_every_typed_frame_texture_must_be_present_and_hash_bound(self):
        manifest,records,resources=fixture()
        bad=copy.deepcopy(manifest);bad['sprites'][0]['textures'][0]['path']='txrs/unrelated.txr'
        modified=dict(resources);modified['txrs/unrelated.txr']=modified['txrs/owner_atlas.txr']
        with self.assertRaisesRegex(ValueError,'inventory'):validate_manifest(bad,records,modified)
        for name in resources:
            modified=dict(resources);modified[name]=b'changed'
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'bytes differ'):
                validate_manifest(manifest,records,modified)

    def test_a_frame_index_cannot_silently_clamp_to_another_icon(self):
        manifest,records,resources=fixture();raw=bytearray(records[9800123]);struct.pack_into('<H',raw,426,1)
        manifest['skills'][0]['record_sha256']=hashlib.sha256(raw).hexdigest()
        with self.assertRaisesRegex(ValueError,'frame exceeds'):validate_manifest(manifest,{9800123:bytes(raw)},resources)

    def test_records_and_owner_claims_cannot_change_the_verified_bindings(self):
        manifest,records,resources=fixture()
        bad=copy.deepcopy(manifest);bad['skills'][0]['record_sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'record identity'):validate_manifest(bad,records,resources)
        bad=copy.deepcopy(manifest);bad['native_registration_authorized']=True
        with self.assertRaisesRegex(ValueError,'fields differ'):validate_manifest(bad,records,resources)
        with self.assertRaisesRegex(ValueError,'owner repeats'):
            compile_bundle([manifest,manifest],'b'*64,'c'*64,'d'*64,records,resources)


if __name__=='__main__':unittest.main()
