import copy
import hashlib
import io
import json
import struct
import unittest
from unittest.mock import patch

from PIL import Image
from owner.units import blueprint_resources as resources
from owner.units import blueprint_templates as templates
from test_unit_blueprint_templates import fixture,sha


def texture(fmt='PNG'):
    out=io.BytesIO();Image.new('RGBA',(8,8),(20,90,160,170)).save(out,fmt)
    return out.getvalue()


def sprite(version=11000,paths=('txrs/a.txr',),annotation=b'',opacity=255):
    stride={10000:164,11000:172}[version]
    raw=bytearray(8+len(paths)*stride+4);struct.pack_into('<II',raw,0,version,len(paths))
    for index,path in enumerate(paths):
        offset=8+index*stride
        struct.pack_into('<III4f2I',raw,offset,20,4,8,0,0,1,1,0x3f800000,0)
        carrier=offset+36
        if version==11000:
            struct.pack_into('<II',raw,carrier,0xff123456,opacity);carrier+=8
        encoded=path.replace('/','\\').encode('utf-16-le')
        raw[carrier:carrier+64]=encoded.ljust(64,b'\0')
        raw[carrier+64:carrier+128]=annotation.ljust(64,b'\0')
    struct.pack_into('<f',raw,len(raw)-4,1)
    return bytes(raw)


def closure_fixture():
    manifest,files=fixture()
    manifest['schema']=templates.RESOURCE_CLOSURE_SCHEMA
    # The source sprites remain intentionally opaque source evidence; only
    # the prepared sprites participate in the runtime resource graph.
    prepared={'sprs/owned-a.spr','sprs/owned-b.spr','sprs/i1.spr'}
    for entry in manifest['resources']:
        if entry['path'] in prepared:
            raw=sprite();files[entry['member']]=raw;entry['sha256']=sha(raw)
    payload=texture()
    files['dependencies/a.txr']=payload
    manifest['resource_dependencies']=[{'path':'txrs/a.txr','member':'dependencies/a.txr','sha256':sha(payload)}]
    return manifest,files


class BlueprintResourceClosure(unittest.TestCase):
    def test_all_frame_edges_are_hash_bound_and_annotation_stays_opaque(self):
        raw=sprite(paths=('txrs/a.txr','txrs/b.txr'),annotation='txrs/unused.txr'.encode('utf-16-le'))
        data={'s.spr':raw,'a.txr':texture(),'b.txr':texture('TGA')}
        bindings={p:{'member':m,'sha256':sha(data[m])} for p,m in
                  [('sprs/s.spr','s.spr'),('txrs/a.txr','a.txr'),('txrs/b.txr','b.txr')]}
        result=resources.inspect_closure(['sprs/s.spr'],bindings,data.__getitem__)
        self.assertEqual(2,len(result['edges']))
        self.assertEqual({'PNG','TGA'},{r['format'] for r in result['textures']})
        self.assertTrue(result['resource_closure_verified'])
        self.assertFalse(result['native_registration_authorized'])
        self.assertFalse(result['resource_bytes_changed'])
        self.assertEqual('0000803f',result['sprites'][0]['playback_bits'])
        self.assertEqual(raw[8+28:8+36].hex(),result['sprites'][0]['frames'][0]['opaque_frame_fields'])

    def test_missing_altered_unused_or_undecodable_texture_fails(self):
        raw=sprite();data={'s.spr':raw,'a.txr':texture()}
        bindings={'sprs/s.spr':{'member':'s.spr','sha256':sha(raw)},
                  'txrs/a.txr':{'member':'a.txr','sha256':sha(data['a.txr'])}}
        with self.assertRaisesRegex(ValueError,'missing.*dependency'):
            resources.inspect_closure(['sprs/s.spr'],{'sprs/s.spr':bindings['sprs/s.spr']},data.__getitem__)
        bad=copy.deepcopy(bindings);bad['txrs/a.txr']['sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'SHA-256'):
            resources.inspect_closure(['sprs/s.spr'],bad,data.__getitem__)
        bad=copy.deepcopy(bindings);bad['txrs/unused.txr']=dict(bindings['txrs/a.txr'])
        with self.assertRaisesRegex(ValueError,'unreferenced'):
            resources.inspect_closure(['sprs/s.spr'],bad,data.__getitem__)
        data['a.txr']=b'not an image';bindings['txrs/a.txr']['sha256']=sha(data['a.txr'])
        with self.assertRaisesRegex(ValueError,'invalid or truncated'):
            resources.inspect_closure(['sprs/s.spr'],bindings,data.__getitem__)

    def test_native_frame_extents_uv_and_path_budgets_are_enforced(self):
        raw=sprite(10000)
        self.assertEqual(10000,resources.sprite_frames(raw)['version'])
        invalids=[raw[:-1],raw+b'\0',struct.pack('<II',9999,1)+raw[8:]]
        nan=bytearray(raw);struct.pack_into('<f',nan,20,float('nan'));invalids.append(bytes(nan))
        no_term=bytearray(raw);no_term[44:108]=b'x\0'*32;invalids.append(bytes(no_term))
        for invalid in invalids:
            with self.assertRaises(ValueError):resources.sprite_frames(invalid)
        with self.assertRaisesRegex(ValueError,'opacity'):resources.sprite_frames(sprite(opacity=256))

    def test_v2_resolves_prepared_resources_without_promoting_source_evidence(self):
        manifest,files=closure_fixture()
        result=templates.validate_manifest(manifest,files=files)
        self.assertTrue(result['admission']['resource_closure_verified'])
        self.assertFalse(result['admission']['native_registration_authorized'])
        self.assertEqual({'sprs/owned-a.spr','sprs/owned-b.spr','sprs/i1.spr','txrs/a.txr'},
                         {r['path'] for r in result['runtime_resources']})
        self.assertEqual(6,len(result['resources']))
        self.assertEqual(b'source-a',files['resources/sprs/source-a.spr'])
        raw=json.dumps(manifest).encode();files['blueprint.json']=raw
        binding=templates.validate_binding({'manifest_path':'blueprint.json','manifest_sha256':sha(raw)},files=files)
        self.assertIn('dependencies/a.txr',binding['resource_members'])
        self.assertIn('dependencies/a.txr',binding['members'])

    def test_v1_keeps_transport_only_semantics_and_rejects_new_untyped_fields(self):
        manifest,files=fixture()
        result=templates.validate_manifest(manifest,files=files)
        self.assertFalse(result['admission']['resource_closure_verified'])
        self.assertEqual(3,len(result['runtime_resources']))
        manifest['resource_dependencies']=[]
        with self.assertRaisesRegex(ValueError,'unknown fields'):templates.validate_manifest(manifest,files=files)

    def test_native_compilation_accepts_only_validated_v2_and_preserves_runtime_gate(self):
        old,old_files=fixture()
        with self.assertRaisesRegex(ValueError,'版本1仅用于传输'):
            templates.require_native_admission(templates.validate_manifest(old,files=old_files))
        manifest,files=closure_fixture()
        validated=templates.validate_manifest(manifest,files=files)
        templates.require_native_admission(validated)
        self.assertFalse(validated['admission']['native_registration_authorized'])
        self.assertFalse(validated['admission']['native_runtime_accepted'])
        with patch('owner.units.blueprints.RAW_TEMPLATE_NATIVE_REGISTRATION_SUPPORTED',False):
            with self.assertRaisesRegex(ValueError,'尚未开放'):
                templates.require_native_admission(validated)

    def test_resource_and_pixel_budgets_apply_before_any_runtime_admission(self):
        raw=sprite();data={'s':raw,'t':texture()}
        bindings={'sprs/s.spr':{'member':'s','sha256':sha(raw)},
                  'txrs/a.txr':{'member':'t','sha256':sha(data['t'])}}
        with patch.object(resources,'MAX_TOTAL_BYTES',1):
            with self.assertRaisesRegex(ValueError,'byte budget'):resources.inspect_closure(['sprs/s.spr'],bindings,data.__getitem__)
        with patch.object(resources,'MAX_TOTAL_PIXELS',1):
            with self.assertRaisesRegex(ValueError,'pixel budget'):resources.inspect_closure(['sprs/s.spr'],bindings,data.__getitem__)
        with patch.object(resources,'MAX_TEXTURE_PIXELS',1):
            with self.assertRaisesRegex(ValueError,'pixel budget'):resources.inspect_closure(['sprs/s.spr'],bindings,data.__getitem__)

    def test_v2_cannot_claim_closure_without_complete_unique_dependency_members(self):
        manifest,files=closure_fixture()
        bad=copy.deepcopy(manifest);bad['resource_dependencies']=[]
        with self.assertRaisesRegex(ValueError,'missing.*dependency'):templates.validate_manifest(bad,files=files)
        bad=copy.deepcopy(manifest);bad['resource_dependencies']*=2
        with self.assertRaisesRegex(ValueError,'duplicated or reused'):templates.validate_manifest(bad,files=files)
        bad=copy.deepcopy(manifest);bad['resource_dependencies'][0]['member']='../a.txr'
        with self.assertRaisesRegex(ValueError,'archive member'):templates.validate_manifest(bad,files=files)


if __name__=='__main__':unittest.main()
