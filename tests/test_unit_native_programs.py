import copy
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from owner.units.native_programs import compile_bundle, inspect_chunk, validate_manifest, validate_project_bindings


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


class NativePrograms(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('SDGO_UNIT_PROGRAM_VALIDATOR'), 'set the built native validator path for the cross-consumer check')
    def test_python_output_is_consumed_and_executed_by_the_native_sdk(self):
        manifest, files, weapons = fixture()
        with tempfile.TemporaryDirectory(prefix='sdgo-native-program-contract-') as directory:
            bundle = Path(directory) / 'native-programs.bin'
            bundle.write_bytes(compile_bundle([manifest], 'a' * 64, files, weapons))
            program = Path(directory) / 'program.scp'
            program.write_bytes(files['example.scp'])
            executable = os.environ['SDGO_UNIT_PROGRAM_VALIDATOR']
            for arguments in (['--unit-program-bundle', str(bundle)],
                              ['--private-chunk', str(program), 'example', 'example_Init']):
                with self.subTest(arguments=arguments):
                    result = subprocess.run([executable, *arguments], capture_output=True, text=True, timeout=30)
                    self.assertEqual(0, result.returncode, result.stdout + result.stderr)

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
