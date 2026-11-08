import copy
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from owner.units.native_programs import compile_bundle, decode_bundle, inspect_chunk, validate_manifest, validate_project_bindings, CALLBACK_SCHEMA, CALLBACK_ROLES


def chunk(name='example_Init'):
    word = lambda value: struct.pack('<I', value)
    string = lambda value: word(len(value) + 1) + value + b'\0'
    def prototype(constants, children, code):
        return (word(0) + word(0) + bytes([0, 0, 0, 2]) + word(0) * 3 + word(len(constants)) +
                b''.join(b'\4' + string(item) for item in constants) + word(len(children)) + b''.join(children) +
                word(len(code)) + b''.join(word(item) for item in code))
    child = prototype([], [], [27 | (1 << 15)])
    root = prototype([name.encode()], [child], [34, 7, 27 | (1 << 15)])
    return bytes([27, 76, 117, 97, 80, 1, 4, 4, 4, 6, 8, 9, 9, 8]) + struct.pack('<d', 3.14159265358979323846e7) + root


def fixture(owner='example', identity=90001):
    name = owner + '_Init'
    program = chunk(name)
    weapon = bytearray(1871)
    struct.pack_into('<I', weapon, 0, identity)
    encoded = (name + '\0').encode('utf-16-le')
    weapon[1771:1771 + len(encoded)] = encoded
    manifest = {'schema': 'sdgo.unit-native-programs/1', 'owner': owner, 'revision': '1' * 64,
        'vm_abi': 'lua50-x86-double64', 'phase': 'after-original-root-loader',
        'root_profile': 'closure-publications-only-v1',
        'programs': [{'resource': 'scp/' + owner + '_init.scp', 'member': owner + '.scp',
                      'sha256': hashlib.sha256(program).hexdigest(), 'exports': [name]}],
        'weapon_initializers': [{'weapon_id': identity, 'weapon_sha256': hashlib.sha256(weapon).hexdigest(), 'export': name}]}
    return manifest, {owner + '.scp': program}, {identity: bytes(weapon)}


def callback_fixture(owner='example', identity=90001):
    manifest,files,weapons=fixture(owner,identity)
    weapon=bytearray(weapons[identity])
    names={'ranged':owner+'_Ranged','special_ranged':owner+'_Special','initializer':owner+'_Init'}
    for role in ('ranged','special_ranged'):
        name=names[role];member=owner+'_'+role+'.scp';program=chunk(name);files[member]=program
        manifest['programs'].append({'resource':'scp/'+member,'member':member,
            'sha256':hashlib.sha256(program).hexdigest(),'exports':[name]})
        offset=CALLBACK_ROLES[role][1];encoded=(name+'\0').encode('utf-16-le')
        weapon[offset:offset+len(encoded)]=encoded
    manifest['schema']=CALLBACK_SCHEMA;manifest.pop('weapon_initializers')
    manifest['weapon_callbacks']=[{'weapon_id':identity,'weapon_sha256':hashlib.sha256(weapon).hexdigest(),
                                  'role':role,'export':names[role]} for role in CALLBACK_ROLES]
    return manifest,files,{identity:bytes(weapon)}


class NativePrograms(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('SDGO_UNIT_PROGRAM_VALIDATOR'), 'set the built native validator path for the cross-consumer check')
    def test_python_output_is_consumed_and_executed_by_the_native_sdk(self):
        for factory in (fixture,callback_fixture):
            manifest,files,weapons=factory()
            with self.subTest(schema=manifest['schema']),tempfile.TemporaryDirectory(prefix='sdgo-native-program-contract-') as directory:
                bundle=Path(directory)/'native-programs.bin'
                bundle.write_bytes(compile_bundle([manifest],'a'*64,files,weapons))
                arguments=['--private-candidate',manifest['owner'],manifest['revision']]
                for program in manifest['programs']:
                    member=Path(directory)/program['member'];member.write_bytes(files[program['member']])
                    arguments.extend([str(member),program['resource'],program['sha256'],','.join(program['exports'])])
                executable=os.environ['SDGO_UNIT_PROGRAM_VALIDATOR']
                for command in (['--unit-program-bundle',str(bundle)],arguments):
                    result=subprocess.run([executable,*command],capture_output=True,text=True,timeout=30)
                    self.assertEqual(0,result.returncode,result.stdout+result.stderr)

    def test_three_typed_callback_roles_bind_distinct_fields_on_one_weapon(self):
        manifest,files,weapons=callback_fixture()
        normalized=validate_manifest(manifest,files,weapons)
        self.assertEqual(['ranged','special_ranged','initializer'],[row['role'] for row in normalized['weapon_callbacks']])
        binary=compile_bundle([manifest],'a'*64,files,weapons)
        self.assertEqual(2,struct.unpack_from('<I',binary,8)[0])
        bad=copy.deepcopy(manifest);bad['weapon_callbacks'][0]['export']='example_Special'
        with self.assertRaisesRegex(ValueError,'ranged field'):validate_manifest(bad,files,weapons)
        bad=copy.deepcopy(manifest);bad['weapon_callbacks'].append(dict(bad['weapon_callbacks'][0]))
        with self.assertRaisesRegex(ValueError,'repeats'):validate_manifest(bad,files,weapons)
        bad=copy.deepcopy(manifest);bad['weapon_callbacks'][0]['role']='arbitrary-offset'
        with self.assertRaisesRegex(ValueError,'role'):validate_manifest(bad,files,weapons)

    def test_callback_schema_does_not_mix_legacy_fields_or_split_a_weapon_across_owners(self):
        manifest,files,weapons=callback_fixture()
        bad=copy.deepcopy(manifest);bad['weapon_initializers']=[]
        with self.assertRaisesRegex(ValueError,'fields differ'):validate_manifest(bad,files,weapons)
        raw=bytearray(weapons[90001]);name='second_Special';encoded=(name+'\0').encode('utf-16-le')
        raw[1669:1769]=encoded.ljust(100,b'\0');digest=hashlib.sha256(raw).hexdigest()
        manifest['weapon_callbacks']=[row for row in manifest['weapon_callbacks'] if row['role']!='special_ranged']
        for row in manifest['weapon_callbacks']:row['weapon_sha256']=digest
        second,other_files,_=fixture('second')
        second['schema']=CALLBACK_SCHEMA;second.pop('weapon_initializers')
        program=chunk(name);other_files={'second.scp':program}
        second['programs']=[{'resource':'scp/second_special.scp','member':'second.scp','sha256':hashlib.sha256(program).hexdigest(),'exports':[name]}]
        second['weapon_callbacks']=[{'weapon_id':90001,'weapon_sha256':digest,'role':'special_ranged','export':name}]
        with self.assertRaisesRegex(ValueError,'owners share a weapon'):
            compile_bundle([manifest,second],'a'*64,{**files,**other_files},{90001:bytes(raw)})

    def test_installed_bundle_decodes_back_to_declarations_that_recompile_byte_identically(self):
        manifest, files, weapons = fixture()
        legacy = compile_bundle([manifest], 'a' * 64, files, weapons)
        decoded = decode_bundle(legacy)
        self.assertEqual(('a' * 64, 1, ['example']), (decoded['catalog_sha256'], decoded['version'],
                                                      [row['owner'] for row in decoded['manifests']]))
        self.assertEqual(legacy, compile_bundle(decoded['manifests'], 'a' * 64, decoded['files'], weapons))
        # A v2 container keeps every owner's roles, including an initializer-only owner.
        second, other_files, other_weapons = callback_fixture('second', 90002)
        mixed = compile_bundle([manifest, second], 'b' * 64, {**files, **other_files}, {**weapons, **other_weapons})
        decoded = decode_bundle(mixed)
        self.assertEqual((2, ['example', 'second']), (decoded['version'], [row['owner'] for row in decoded['manifests']]))
        self.assertEqual(['initializer'], [row['role'] for row in decoded['manifests'][0]['weapon_callbacks']])
        self.assertEqual(mixed, compile_bundle(decoded['manifests'], 'b' * 64, decoded['files'], {**weapons, **other_weapons}))
        # Recompiling for another catalog keeps the declarations and only rebinds the header.
        rebound = compile_bundle(decoded['manifests'], 'c' * 64, decoded['files'], {**weapons, **other_weapons})
        self.assertEqual((bytes.fromhex('c' * 64), mixed[56:]), (rebound[24:56], rebound[56:]))
        for bad in (mixed + b'\0', mixed[:-1], b'SDGOPR2\0' + mixed[8:], mixed[:56] + b'\1' + mixed[57:]):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                decode_bundle(bad)

    def test_complete_chunk_inspection_and_catalog_bound_container(self):
        manifest, files, weapons = fixture()
        self.assertEqual(['example_Init'], inspect_chunk(files['example.scp']))
        binary = compile_bundle([manifest], 'a' * 64, files, weapons)
        self.assertEqual((b'SDGOPR1\0', 1, 64, 1, len(binary) - 64), struct.unpack_from('<8s4I', binary))
        self.assertEqual(bytes.fromhex('a' * 64), binary[24:56])
        self.assertEqual(bytes(8), binary[56:64])

    def test_rejects_bad_hash_exports_or_undeclared_initializer(self):
        manifest, files, weapons = fixture()
        for field in ('sha256', 'exports'):
            bad = copy.deepcopy(manifest)
            bad['programs'][0][field] = '0' * 64 if field == 'sha256' else ['example_Other']
            with self.subTest(field=field), self.assertRaises(ValueError): validate_manifest(bad, files, weapons)
        for key, value in [('export', 'example_Other'), ('weapon_sha256', '0' * 64), ('weapon_id', 999)]:
            bad = copy.deepcopy(manifest); bad['weapon_initializers'][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_manifest(bad, files, weapons)

    def test_weapon_field_cannot_be_bound_by_manifest_name_alone(self):
        manifest, files, weapons = fixture()
        raw = bytearray(weapons[90001]); raw[1771:1871] = bytes(100)
        manifest['weapon_initializers'][0]['weapon_sha256'] = hashlib.sha256(raw).hexdigest()
        with self.assertRaisesRegex(ValueError, 'initializer field'):
            validate_manifest(manifest, files, {90001: bytes(raw)})

    def test_multiple_owners_have_explicit_distinct_bindings(self):
        first, files, weapons = fixture()
        second, other_files, other_weapons = fixture('second', 90002)
        binary = compile_bundle([first, second], 'a' * 64, {**files, **other_files}, {**weapons, **other_weapons})
        self.assertEqual(2, struct.unpack_from('<I', binary, 16)[0])
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            compile_bundle([first, first], 'a' * 64, files, weapons)

    def test_no_text_luajit_truncation_or_trailing_data_is_admitted(self):
        raw = chunk()
        for bad in (b'function example_Init() end', b'\x1bLJ' + raw[3:], raw[:-1], raw + b'extra'):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError): inspect_chunk(bad)

    def test_owner_namespaces_and_paths_are_checked_without_execution(self):
        manifest, files, weapons = fixture()
        bad = copy.deepcopy(manifest); bad['programs'][0]['resource'] = 'scp/another_init.scp'
        with self.assertRaises(ValueError): validate_manifest(bad, files, weapons)
        bad = copy.deepcopy(manifest); bad['programs'][0]['member'] = '../example.scp'
        with self.assertRaises(ValueError): validate_manifest(bad, files, weapons)

    def test_project_bindings_require_every_hashed_member_and_refuse_duplicate_fields(self):
        import json
        manifest, files, weapons = fixture()
        files['weapon.bin'] = weapons[90001]
        raw = json.dumps(manifest).encode()
        files['programs/manifest.json'] = raw
        binding = {'manifest_path': 'programs/manifest.json', 'manifest_sha256': hashlib.sha256(raw).hexdigest()}
        resolved = validate_project_bindings([binding], files=files)
        self.assertFalse(resolved[0]['native_registration_authorized'])
        self.assertEqual({'programs/manifest.json', 'example.scp'}, resolved[0]['members'])
        missing = dict(files); missing.pop('example.scp')
        with self.assertRaisesRegex(ValueError, 'missing'):
            validate_project_bindings([binding], files=missing)
        changed = dict(files); changed['programs/manifest.json'] += b' '
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            validate_project_bindings([binding], files=changed)
        duplicate = b'{"owner":"duplicate",' + raw[1:]
        files['programs/manifest.json'] = duplicate
        binding['manifest_sha256'] = hashlib.sha256(duplicate).hexdigest()
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            validate_project_bindings([binding], files=files)


if __name__ == '__main__': unittest.main()
