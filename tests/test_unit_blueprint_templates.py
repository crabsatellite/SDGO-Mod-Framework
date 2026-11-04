import copy
import hashlib
import json
import struct
import unittest

from owner.units import blueprint_templates as t


def native_resource(path, size):
    raw=path.replace('/','\\').encode('utf-16-le')
    return raw.ljust(size,b'\0')


def recipe_record(identity=200001, first='sprs/source-a.spr',
                  second='sprs/source-b.spr'):
    raw=bytearray(1197)
    struct.pack_into('<I',raw,0,identity)
    name='Generic Recipe'.encode('utf-16-le');raw[5:5+len(name)]=name
    struct.pack_into('<I',raw,105,3)
    raw[109:169]=native_resource(first,60)
    raw[169:229]=native_resource(second,60)
    struct.pack_into('<I',raw,229,16003)
    struct.pack_into('<7I',raw,233,13003,16002,15004,0,0,0,0)
    struct.pack_into('<7I',raw,261,7,1,1,0,0,0,0)
    struct.pack_into('<II',raw,1189,0,600)
    return bytes(raw)


def item_record(identity=1520001, recipe=200001,
                sprite='sprs/i0.spr', name='Separate Item Name'):
    raw=bytearray(614)
    struct.pack_into('<I',raw,0,identity)
    encoded=name.encode('utf-16-le');raw[5:5+len(encoded)]=encoded
    struct.pack_into('<I',raw,105,recipe)
    raw[114:142]=native_resource(sprite,28)
    return bytes(raw)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def fixture():
    recipe_source=recipe_record()
    recipe_prepared=bytearray(recipe_source)
    recipe_prepared[109:169]=native_resource('sprs/owned-a.spr',60)
    recipe_prepared[169:229]=native_resource('sprs/owned-b.spr',60)
    recipe_prepared=bytes(recipe_prepared)
    item_source=item_record()
    item_prepared=bytearray(item_source)
    item_prepared[114:142]=native_resource('sprs/i1.spr',28)
    item_prepared=bytes(item_prepared)
    resources={
        'sprs/source-a.spr':b'source-a',
        'sprs/source-b.spr':b'source-b',
        'sprs/owned-a.spr':b'owned-a',
        'sprs/owned-b.spr':b'owned-b',
        'sprs/i0.spr':b'item-source',
        'sprs/i1.spr':b'item-owned',
    }
    files={
        'templates/source/recipe.bin':recipe_source,
        'templates/prepared/recipe.bin':recipe_prepared,
        'templates/source/item.bin':item_source,
        'templates/prepared/item.bin':item_prepared,
    }
    declared=[]
    for path,raw in resources.items():
        member='resources/'+path
        files[member]=raw
        declared.append({'path':path,'member':member,'sha256':sha(raw)})
    manifest={
        'schema':t.SCHEMA,
        'owner':'generic-test-owner',
        'templates':[
            {'role':t.RECIPE_ROLE,'kind':33,'bytes':1197,
             'source_record_id':200001,'source_sha256':sha(recipe_source),
             'prepared_sha256':sha(recipe_prepared),
             'source_path':'templates/source/recipe.bin',
             'prepared_path':'templates/prepared/recipe.bin',
             'rewrite_spans':[
                 {'offset':109,'bytes':60,
                  'source_value':'sprs/source-a.spr',
                  'prepared_value':'sprs/owned-a.spr'},
                 {'offset':169,'bytes':60,
                  'source_value':'sprs/source-b.spr',
                  'prepared_value':'sprs/owned-b.spr'}],
             'preserve_outside_rewrite_spans':True},
            {'role':t.ITEM_ROLE,'kind':34,'bytes':614,
             'source_record_id':1520001,'source_sha256':sha(item_source),
             'prepared_sha256':sha(item_prepared),
             'source_path':'templates/source/item.bin',
             'prepared_path':'templates/prepared/item.bin',
             'rewrite_spans':[
                 {'offset':114,'bytes':28,
                  'source_value':'sprs/i0.spr',
                  'prepared_value':'sprs/i1.spr'}],
             'preserve_outside_rewrite_spans':True,
             'display_name_mode':'preserve-template'}],
        'resources':declared,
    }
    return manifest,files


def bound_fixture():
    manifest,files=fixture()
    raw=json.dumps(manifest,ensure_ascii=False,sort_keys=True).encode()
    path='blueprints/209000/client-templates.json'
    files[path]=raw
    return {
        'manifest_path':path,
        'manifest_sha256':sha(raw),
    },files


def declaration(binding, *, enabled=True):
    return {
        'schema_version':1,
        'id':209000,
        'enabled':enabled,
        'name':'Compiled Recipe',
        'internal_name':'compiled_recipe',
        'source':{'kind':'authored'},
        'recipe_template_id':200001,
        'item_template_id':1520001,
        'item_category_id':1599000,
        'output_unit_id':16003,
        'materials':[
            {'unit_id':13003,'minimum_level':7},
            {'unit_id':16002,'minimum_level':2},
            {'unit_id':15004,'minimum_level':3},
        ],
        'minimum_player_level':4,
        'repay_point':700,
        'encyclopedia_primary':True,
        'recipe_sprite':'sprs/owned-b.spr',
        'client_templates':binding,
    }


class BlueprintClientTemplates(unittest.TestCase):
    def test_complete_typed_binding_preserves_item_template_name(self):
        manifest,files=fixture()
        result=t.validate_manifest(manifest,files=files)
        self.assertEqual({t.RECIPE_ROLE:33,t.ITEM_ROLE:34},
                         result['admission']['typed_record_kinds'])
        item=result['templates'][t.ITEM_ROLE]
        self.assertEqual('Separate Item Name',item['display_name'])
        self.assertEqual('preserve-template',item['display_name_mode'])
        self.assertTrue(
            result['admission']['direct_resource_refs_verified'])
        self.assertFalse(result['admission']['resource_closure_verified'])
        self.assertTrue(
            result['admission']['semantic_resource_closure_required'])
        self.assertFalse(result['admission']['server_definition_eligible'])
        self.assertFalse(result['admission']['native_registration_authorized'])

    def test_source_prepared_records_and_resources_are_sha_bound(self):
        manifest,files=fixture()
        for mutation in ('missing-source','source-hash','missing-resource',
                         'resource-hash','empty-resource'):
            value=copy.deepcopy(manifest);blobs=dict(files)
            if mutation=='missing-source':
                blobs.pop('templates/source/recipe.bin')
            elif mutation=='source-hash':
                value['templates'][0]['source_sha256']='0'*64
            elif mutation=='missing-resource':
                blobs.pop('resources/sprs/owned-a.spr')
            elif mutation=='resource-hash':
                row=next(r for r in value['resources']
                         if r['path']=='sprs/owned-a.spr')
                row['sha256']='0'*64
            else:
                blobs['resources/sprs/owned-a.spr']=b''
                row=next(r for r in value['resources']
                         if r['path']=='sprs/owned-a.spr')
                row['sha256']=sha(b'')
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):
                t.validate_manifest(value,files=blobs)

    def test_changes_outside_reviewed_spans_fail_closed(self):
        manifest,files=fixture()
        prepared=bytearray(files['templates/prepared/recipe.bin'])
        prepared[400]^=1
        files['templates/prepared/recipe.bin']=bytes(prepared)
        manifest['templates'][0]['prepared_sha256']=sha(bytes(prepared))
        with self.assertRaisesRegex(ValueError,'outside approved'):
            t.validate_manifest(manifest,files=files)

    def test_only_exact_native_sprite_carriers_can_be_rewritten(self):
        manifest,files=fixture()
        manifest['templates'][0]['rewrite_spans'][0]['offset']=229
        with self.assertRaisesRegex(ValueError,'not approved'):
            t.validate_manifest(manifest,files=files)
        manifest,files=fixture()
        manifest['templates'][0]['rewrite_spans'].append(
            copy.deepcopy(manifest['templates'][0]['rewrite_spans'][0]))
        with self.assertRaisesRegex(ValueError,'overlap'):
            t.validate_manifest(manifest,files=files)

    def test_declared_span_values_must_match_both_records(self):
        manifest,files=fixture()
        manifest['templates'][0]['rewrite_spans'][0][
            'prepared_value']='sprs/wrong.spr'
        with self.assertRaisesRegex(ValueError,'prepared bytes mismatch'):
            t.validate_manifest(manifest,files=files)

    def test_kind34_is_typed_and_cannot_be_a_kind85_sized_record(self):
        manifest,files=fixture()
        manifest['templates'][1]['kind']=85
        with self.assertRaisesRegex(ValueError,'wrong typed record kind'):
            t.validate_manifest(manifest,files=files)
        manifest,files=fixture()
        source=bytearray(files['templates/source/item.bin'])
        struct.pack_into('<I',source,0,3650001)
        files['templates/source/item.bin']=bytes(source)
        manifest['templates'][1]['source_record_id']=3650001
        manifest['templates'][1]['source_sha256']=sha(bytes(source))
        with self.assertRaisesRegex(ValueError,'class-15'):
            t.validate_manifest(manifest,files=files)

    def test_item_template_must_reference_the_bound_recipe(self):
        manifest,files=fixture()
        for member in ('templates/source/item.bin',
                       'templates/prepared/item.bin'):
            raw=bytearray(files[member]);struct.pack_into('<I',raw,105,200002)
            files[member]=bytes(raw)
        manifest['templates'][1]['source_sha256']=sha(
            files['templates/source/item.bin'])
        manifest['templates'][1]['prepared_sha256']=sha(
            files['templates/prepared/item.bin'])
        with self.assertRaisesRegex(ValueError,'does not reference'):
            t.validate_manifest(manifest,files=files)

    def test_preserve_template_and_explicit_name_modes_are_distinct(self):
        manifest,files=fixture()
        manifest['templates'][1]['display_name']='Wrong Override'
        with self.assertRaisesRegex(ValueError,'cannot supply'):
            t.validate_manifest(manifest,files=files)
        manifest,files=fixture()
        manifest['templates'][1]['display_name_mode']='explicit'
        manifest['templates'][1]['display_name']='Reviewed Item Name'
        result=t.validate_manifest(manifest,files=files)
        self.assertEqual('Reviewed Item Name',
                         result['templates'][t.ITEM_ROLE]['display_name'])
        self.assertFalse(result['admission']['item_template_name_preserved'])

    def test_resource_manifest_is_exact_and_not_payload_storage(self):
        manifest,files=fixture()
        manifest['resources'].pop()
        with self.assertRaisesRegex(ValueError,'resources are missing'):
            t.validate_manifest(manifest,files=files)
        manifest,files=fixture()
        files['resources/sprs/unused.spr']=b'unused'
        manifest['resources'].append(
            {'path':'sprs/unused.spr',
             'member':'resources/sprs/unused.spr',
             'sha256':sha(b'unused')})
        with self.assertRaisesRegex(ValueError,'unreferenced'):
            t.validate_manifest(manifest,files=files)

    def test_server_donor_fields_and_unknown_schema_are_rejected(self):
        manifest,files=fixture()
        manifest['templates'][0]['server_template_id']=200123
        with self.assertRaisesRegex(ValueError,'unknown fields'):
            t.validate_manifest(manifest,files=files)
        manifest,files=fixture();manifest['schema']='other'
        with self.assertRaisesRegex(ValueError,'unsupported'):
            t.validate_manifest(manifest,files=files)

    def test_binding_is_sha_bound_and_resolves_all_project_members(self):
        binding,files=bound_fixture()
        result=t.validate_binding(binding,files=files)
        self.assertEqual(binding,result['binding'])
        self.assertIn(binding['manifest_path'],result['members'])
        self.assertEqual(4,len(result['template_members']))
        self.assertEqual(6,len(result['resource_members']))
        bad=copy.deepcopy(binding);bad['manifest_sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'SHA-256 mismatch'):
            t.validate_binding(bad,files=files)

    def test_malformed_manifest_and_unsafe_member_paths_fail_closed(self):
        binding,files=bound_fixture()
        unsafe=copy.deepcopy(binding);unsafe['manifest_path']='../manifest.json'
        with self.assertRaisesRegex(ValueError,'archive member'):
            t.validate_binding(unsafe,files=files)
        manifest,files=fixture()
        manifest['templates'][0]['source_path']='../recipe.bin'
        with self.assertRaisesRegex(ValueError,'archive member'):
            t.validate_manifest(manifest,files=files)
        manifest,files=fixture()
        manifest['resources'][0]['member']='../owned.spr'
        with self.assertRaisesRegex(ValueError,'archive member'):
            t.validate_manifest(manifest,files=files)
        invalid=b'{not-json'
        files={'blueprints/client-templates.json':invalid}
        with self.assertRaisesRegex(ValueError,'valid UTF-8 JSON'):
            t.validate_binding({
                'manifest_path':'blueprints/client-templates.json',
                'manifest_sha256':sha(invalid),
            },files=files)

    def test_project_bindings_reject_typed_record_reuse(self):
        binding,files=bound_fixture()
        definitions={'blueprints':[
            {'id':209000,'client_templates':binding},
            {'id':209001,'client_templates':binding},
        ]}
        with self.assertRaisesRegex(ValueError,'reuse'):
            t.validate_project_bindings(definitions,files=files)

    def test_compiler_preserves_prepared_unknown_bytes_and_item_name(self):
        binding,files=bound_fixture()
        prepared_recipe=files['templates/prepared/recipe.bin']
        prepared_item=files['templates/prepared/item.bin']
        result=t.compile_binding(
            declaration(binding,enabled=False),
            root_unit_id=16003,
            files=files,
        )
        recipe=result['recipe'];item=result['item']
        self.assertEqual(209000,struct.unpack_from('<I',recipe,0)[0])
        self.assertEqual(
            'Compiled Recipe',
            recipe[5:105].decode('utf-16-le').split('\0')[0],
        )
        self.assertEqual(16003,struct.unpack_from('<I',recipe,229)[0])
        self.assertEqual((7,2,3),struct.unpack_from('<3I',recipe,261))
        self.assertEqual(prepared_recipe[400:1189],recipe[400:1189])
        self.assertEqual(
            'Separate Item Name',
            item[5:105].decode('utf-16-le').split('\0')[0],
        )
        self.assertEqual(1599000,struct.unpack_from('<I',item,0)[0])
        self.assertEqual(209000,struct.unpack_from('<I',item,105)[0])
        self.assertEqual(prepared_item[109:],item[109:])
        receipt=result['receipt']
        self.assertFalse(receipt['resource_closure_verified'])
        self.assertFalse(receipt['native_registration_authorized'])
        self.assertEqual(
            sha(recipe),
            receipt['records'][0]['compiled_sha256'],
        )
        self.assertTrue(receipt['records'][0]['compiled_changes'])
        self.assertTrue(
            all(row['unknown_bytes_preserved']
                for row in receipt['records'])
        )

    def test_explicit_item_name_is_the_only_name_override(self):
        manifest,files=fixture()
        manifest['templates'][1]['display_name_mode']='explicit'
        manifest['templates'][1]['display_name']='Reviewed Item Name'
        raw=json.dumps(manifest,ensure_ascii=False,sort_keys=True).encode()
        path='blueprints/209000/client-templates.json'
        files[path]=raw
        result=t.compile_binding(
            declaration({'manifest_path':path,'manifest_sha256':sha(raw)}),
            root_unit_id=16003,
            files=files,
        )
        self.assertEqual(
            'Reviewed Item Name',
            result['item'][5:105].decode('utf-16-le').split('\0')[0],
        )


if __name__=='__main__':unittest.main()
