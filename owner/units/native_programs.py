"""Source-bound Lua closure declarations and catalog-bound runtime transport.

This validates programs without executing them. The native consumer repeats
structural validation and checks the original root/VM/thread boundary. Neither
the manifest nor this container admits new skills, server definitions or actors.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct

SCHEMA = 'sdgo.unit-native-programs/1'
NATIVE_PUBLICATION_SUPPORTED = False
MAGIC = b'SDGOPR1\0'
MAX_BYTES = 128 * 1024 * 1024
MAX_PROGRAM_BYTES = 32 * 1024 * 1024
_HEADER = bytes([27, 76, 117, 97, 80, 1, 4, 4, 4, 6, 8, 9, 9, 8])


def _hash(raw):
    return hashlib.sha256(raw).hexdigest()


def _digest(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('native program SHA-256 must be lowercase hexadecimal')
    return value


def _fields(value, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ValueError('native program declaration fields differ')


def inspect_chunk(raw):
    """Match the native SDK's Lua 5.0 x86 closure-only registration profile."""
    if not isinstance(raw, bytes) or not 22 <= len(raw) <= MAX_PROGRAM_BYTES:
        raise ValueError('native Lua chunk byte budget')
    if raw[:14] != _HEADER or raw[14:22] != struct.pack('<d', 3.14159265358979323846e7):
        raise ValueError('native Lua 5.0 x86/double header required')
    position, items, prototypes = 22, 0, 0

    def take(size):
        nonlocal position
        if size < 0 or size > len(raw) - position:
            raise ValueError('truncated native Lua chunk')
        result = raw[position:position + size]
        position += size
        return result

    def word():
        return struct.unpack('<I', take(4))[0]

    def count():
        nonlocal items
        value = word()
        items += value
        if value > 1_000_000 or items > 5_000_000:
            raise ValueError('native Lua vector budget')
        return value

    def string(constant=False):
        size = word()
        if size == 0:
            if constant: raise ValueError('null Lua string constant')
            return b''
        value = take(size)
        if value[-1] != 0:
            raise ValueError('Lua string lacks terminator')
        return value[:-1]

    def prototype(depth):
        nonlocal prototypes
        prototypes += 1
        if depth > 128 or prototypes > 100_000:
            raise ValueError('native Lua prototype budget')
        string(); word()
        upvalues, parameters, vararg, stack = take(4)
        if not stack or parameters > stack:
            raise ValueError('invalid native Lua register frame')
        take(count() * 4)
        for _ in range(count()): string(); word(); word()
        for _ in range(count()): string()
        constants = []
        for _ in range(count()):
            tag = take(1)[0]
            if tag == 3: take(8); value = None
            elif tag == 4: value = string(True)
            elif tag == 0: value = None
            else: raise ValueError('unsupported native Lua constant tag')
            constants.append(value)
        children = [prototype(depth + 1) for _ in range(count())]
        code = [word() for _ in range(count())]
        if any((instruction & 63) > 34 for instruction in code):
            raise ValueError('unknown native Lua opcode')
        return upvalues, parameters, stack, constants, children, code

    upvalues, parameters, stack, constants, children, code = prototype(0)
    if position != len(raw) or upvalues or parameters:
        raise ValueError('native Lua root extent/parameters')
    pc, names, used = 0, set(), set()
    while pc + 1 < len(code):
        closure, publication = code[pc:pc + 2]
        if closure & 63 == 27: break
        if closure & 63 != 34 or publication & 63 != 7:
            raise ValueError('native Lua registration root may only publish closures')
        child, key = (closure >> 6) & 0x3ffff, (publication >> 6) & 0x3ffff
        if closure >> 24 >= stack or closure >> 24 != publication >> 24:
            raise ValueError('native Lua closure register differs')
        if child >= len(children) or children[child][0] or child in used:
            raise ValueError('captured/duplicate native Lua registration closure')
        if key >= len(constants) or constants[key] is None or constants[key] in names:
            raise ValueError('native Lua publication key differs')
        used.add(child); names.add(constants[key]); pc += 2
    if (pc + 1 != len(code) or code[pc] & 63 != 27 or
            (code[pc] >> 15) & 511 != 1 or (code[pc] >> 6) & 511 != 0 or code[pc] >> 24 >= stack or
            not names or len(used) != len(children)):
        raise ValueError('native Lua registration return/closure inventory differs')
    try:
        return sorted(name.decode('ascii') for name in names)
    except UnicodeError as error:
        raise ValueError('native Lua export names must be ASCII') from error


def validate_manifest(value, files, weapons):
    _fields(value, {'schema', 'owner', 'revision', 'vm_abi', 'phase', 'root_profile', 'programs', 'weapon_initializers'})
    if (value['schema'] != SCHEMA or value['vm_abi'] != 'lua50-x86-double64' or
            value['phase'] != 'after-original-root-loader' or value['root_profile'] != 'closure-publications-only-v1'):
        raise ValueError('unsupported native program profile')
    owner = value['owner']
    if not isinstance(owner, str) or not re.fullmatch('[a-z_][a-z0-9_]{0,47}', owner):
        raise ValueError('native program owner namespace')
    _digest(value['revision'])
    if not isinstance(value['programs'], list) or not 1 <= len(value['programs']) <= 512:
        raise ValueError('native program count budget')
    programs, names, resources, members, total = [], set(), set(), set(), 0
    for row in value['programs']:
        _fields(row, {'resource', 'member', 'sha256', 'exports'})
        resource, member = row['resource'], row['member']
        if (not isinstance(resource, str) or len(resource) > 240 or
                not re.fullmatch('scp/' + re.escape(owner) + r'_[a-z0-9_.-]+\.scp', resource) or '..' in resource):
            raise ValueError('native program resource namespace')
        if (not isinstance(member, str) or not re.fullmatch('[A-Za-z0-9_][A-Za-z0-9_./-]{0,239}', member) or
                any(part in ('', '.', '..') for part in member.split('/'))):
            raise ValueError('native program archive member')
        if member in members or resource in resources:
            raise ValueError('duplicate native program member/resource')
        members.add(member); resources.add(resource)
        if member not in files:
            raise ValueError('native program archive member is missing')
        raw = files[member]
        if _hash(raw) != _digest(row['sha256']):
            raise ValueError('native program member SHA-256 differs')
        actual = inspect_chunk(raw)
        exports = row['exports']
        if (not isinstance(exports, list) or any(not isinstance(name, str) for name in exports) or
                sorted(exports) != actual or len(exports) != len(set(exports))):
            raise ValueError('native program declared exports differ from actual closures')
        for name in actual:
            if len(name) > 255 or not re.fullmatch(re.escape(owner) + '_[A-Za-z0-9_]+', name) or name in names:
                raise ValueError('native program export namespace/collision')
            names.add(name)
        total += len(raw)
        if total > MAX_BYTES:
            raise ValueError('native program total byte budget')
        programs.append({**row, 'raw': raw})
    bindings = value['weapon_initializers']
    if not isinstance(bindings, list) or not 1 <= len(bindings) <= 4096:
        raise ValueError('native program initializer count budget')
    seen = set()
    for row in bindings:
        _fields(row, {'weapon_id', 'weapon_sha256', 'export'})
        identity = row['weapon_id']
        if type(identity) is not int or not 1 <= identity <= 9_999_999 or identity in seen:
            raise ValueError('native program weapon identity')
        seen.add(identity)
        record = weapons.get(identity)
        if not isinstance(record, bytes) or len(record) != 1871 or record[4] != 0 or struct.unpack_from('<I', record)[0] != identity:
            raise ValueError('native program must bind a complete imported weapon')
        if _hash(record) != _digest(row['weapon_sha256']):
            raise ValueError('native program weapon SHA-256 differs')
        if not isinstance(row['export'], str) or row['export'] not in names or len(row['export']) > 49:
            raise ValueError('native initializer must be a declared bounded export')
        field = record[1771:1871]
        terminator = next((offset for offset in range(0, 100, 2) if field[offset:offset + 2] == b'\0\0'), None)
        if terminator is None or field[:terminator].decode('utf-16-le') != row['export']:
            raise ValueError('native weapon initializer field does not match export')
    return {'owner': owner, 'revision': value['revision'], 'programs': programs,
            'weapon_initializers': [dict(row) for row in bindings]}


def compile_bundle(manifests, catalog_sha256, files, weapons):
    """Encode the optional runtime sidecar against the exact emitted catalog."""
    if not isinstance(manifests, list) or not 1 <= len(manifests) <= 128:
        raise ValueError('native program owner count budget')
    owners, exports, bound = set(), set(), set()
    program_count = 0
    payload = bytearray()

    def word(value): payload.extend(struct.pack('<I', value))
    def text(value):
        raw = value.encode('ascii'); word(len(raw)); payload.extend(raw)

    for manifest in manifests:
        candidate = validate_manifest(manifest, files, weapons)
        if candidate['owner'] in owners: raise ValueError('duplicate native program owner')
        owners.add(candidate['owner'])
        text(candidate['owner']); text(candidate['revision']); word(len(candidate['programs']))
        for program in candidate['programs']:
            program_count += 1
            if program_count > 4096: raise ValueError('native program batch count budget')
            text(program['resource']); payload.extend(bytes.fromhex(program['sha256'])); word(len(program['exports']))
            for export in program['exports']:
                if export in exports: raise ValueError('native program owners share an export')
                exports.add(export); text(export)
            word(len(program['raw'])); payload.extend(program['raw'])
        word(len(candidate['weapon_initializers']))
        for binding in candidate['weapon_initializers']:
            if binding['weapon_id'] in bound: raise ValueError('native program owners share a weapon')
            bound.add(binding['weapon_id']); word(binding['weapon_id'])
            payload.extend(bytes.fromhex(binding['weapon_sha256'])); text(binding['export'])
        if len(payload) > MAX_BYTES: raise ValueError('native program bundle byte budget')
    return (struct.pack('<8s4I', MAGIC, 1, 64, len(manifests), len(payload)) +
            bytes.fromhex(_digest(catalog_sha256)) + bytes(8) + payload)


def normalize_bindings(value):
    if not isinstance(value, list) or len(value) > 128:
        raise ValueError('native program binding count budget')
    result, seen = [], set()
    for row in value:
        _fields(row, {'manifest_path', 'manifest_sha256'})
        member = row['manifest_path']
        if (not isinstance(member, str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_./-]{0,234}\.json', member) or
                any(part in ('', '.', '..') for part in member.split('/')) or member in seen):
            raise ValueError('native program manifest member is invalid or repeated')
        seen.add(member)
        result.append({'manifest_path': member, 'manifest_sha256': _digest(row['manifest_sha256'])})
    return result


def validate_project_bindings(value, *, files):
    """Resolve portable authoring bindings; no runtime authority is issued."""
    bindings = normalize_bindings(value)
    if not bindings: return []
    weapons = {}
    for member, raw in files.items():
        if member.lower().endswith('.bin') and len(raw) == 1871:
            identity = struct.unpack_from('<I', raw)[0]
            if identity in weapons: raise ValueError('native program project has duplicate weapon records')
            weapons[identity] = raw
    result, owners = [], set()
    def pairs(values):
        row = {}
        for key, item in values:
            if key in row: raise ValueError('duplicate native program JSON field')
            row[key] = item
        return row
    for binding in bindings:
        member = binding['manifest_path']
        if member not in files: raise ValueError('native program manifest is missing')
        raw = files[member]
        if len(raw) > 1024 * 1024 or _hash(raw) != binding['manifest_sha256']:
            raise ValueError('native program manifest size/SHA-256 differs')
        try: manifest = json.loads(raw.decode('utf-8'), object_pairs_hook=pairs)
        except (UnicodeError, json.JSONDecodeError) as error:
            raise ValueError('invalid native program manifest JSON') from error
        normalized = validate_manifest(manifest, files, weapons)
        if normalized['owner'] in owners: raise ValueError('duplicate native program project owner')
        owners.add(normalized['owner'])
        result.append({'binding': binding, 'manifest': manifest,
                       'members': {member, *(row['member'] for row in manifest['programs'])},
                       'native_registration_authorized': False})
    return result
