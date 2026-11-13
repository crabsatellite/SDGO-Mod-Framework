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
CALLBACK_SCHEMA = 'sdgo.unit-native-programs/2'
CALLBACK_ROLES = {'ranged': (1, 1569), 'special_ranged': (2, 1669), 'initializer': (3, 1771)}
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


def _inspect_chunk(raw):
    """Match the native SDK's Lua 5.0 x86 closure-only registration profile."""
    if not isinstance(raw, bytes) or not 22 <= len(raw) <= MAX_PROGRAM_BYTES:
        raise ValueError('native Lua chunk byte budget')
    if raw[:14] != _HEADER or raw[14:22] != struct.pack('<d', 3.14159265358979323846e7):
        raise ValueError('native Lua 5.0 x86/double header required')
    position, items, prototypes = 22, 0, 0
    resource_references = []

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

    def prototype(depth, path=()):
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
        for index in range(count()):
            tag = take(1)[0]
            if tag == 3: take(8); value = None
            elif tag == 4: value = string(True)
            elif tag == 0: value = None
            else: raise ValueError('unsupported native Lua constant tag')
            constants.append(value)
            if value is not None and re.match(rb'(?i)^(?:gdrs|mdrs|anrs|txrs|fxrs|sprs|sdrs|scp)[\\/]',value):
                # Directory prefixes used to construct a path are not complete
                # resource literals. They still require a separate dependency contract.
                if not re.search(rb'\.[A-Za-z0-9]{1,8}$',value):continue
                from .assets import _resource_path
                try:
                    literal=value.decode('ascii')
                    normalized=_resource_path(literal)
                except (UnicodeError,ValueError) as error:
                    raise ValueError('native program resource literal is invalid') from error
                resource_references.append({'prototype':list(path),'constant':index,'literal':literal,'path':normalized})
                if len(resource_references)>65536:raise ValueError('native program resource reference budget')
        children = [prototype(depth + 1, path + (index,)) for index in range(count())]
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
        return {'exports':sorted(name.decode('ascii') for name in names),'resource_references':resource_references}
    except UnicodeError as error:
        raise ValueError('native Lua export names must be ASCII') from error


def inspect_chunk(raw):
    return _inspect_chunk(raw)['exports']


def literal_resource_references(raw):
    """Read exact serialized Lua constants, excluding source/debug strings.

    This inventory does not infer computed paths, native fixed resources or
    model/animation semantics. Those dependencies need separate declarations.
    """
    return _inspect_chunk(raw)['resource_references']


def validate_resource_dependencies(bundle, weapons, read_resource, *, catalog_sha256=None):
    """Check every admitted program's literal paths against the actual target.

    The resolver supplies published overlay bytes or compatible original bytes;
    it must never fetch content, execute a script, or invent a missing resource.
    Returns bounded hash witnesses, not actor/gameplay admission.
    """
    decoded=decode_bundle(bundle)
    if catalog_sha256 is not None and decoded['catalog_sha256']!=catalog_sha256:
        raise ValueError('native program resource check uses a different catalog SHA-256')
    rebuilt=compile_bundle(decoded['manifests'],decoded['catalog_sha256'],decoded['files'],weapons)
    if rebuilt!=bundle:raise ValueError('native program sidecar is not canonical')
    resources={};references=[];missing=[];total=0
    for manifest in decoded['manifests']:
        for program in manifest['programs']:
            for reference in literal_resource_references(decoded['files'][program['member']]):
                path=reference['path']
                if len(references)>=65536:raise ValueError('native program resource reference budget')
                references.append({'owner':manifest['owner'],'program':program['resource'],
                                   'program_sha256':program['sha256'],**reference})
                if path in resources:continue
                if len(resources)>=4096:raise ValueError('native program resource dependency budget')
                try:raw=read_resource(path)
                except FileNotFoundError:raw=None
                if raw is None:
                    resources[path]=None;missing.append(path);continue
                if not isinstance(raw,bytes) or not 0<len(raw)<=64*1024*1024:
                    raise ValueError('native program resource exceeds its byte budget: '+path)
                total+=len(raw)
                if total>512*1024*1024:
                    raise ValueError('native program resource dependency budget')
                resources[path]={'sha256':_hash(raw),'bytes':len(raw)}
    if missing:
        raise ValueError('native program resource is missing: '+', '.join(sorted(missing)[:8])+
                         '; provide its complete dependencies before publication')
    return {'programs':sum(len(row['programs'])for row in decoded['manifests']),
            'resources':resources,'references':references,'actor_accepted':False}


def validate_manifest(value, files, weapons):
    version = value.get('schema') if isinstance(value, dict) else None
    binding_key = 'weapon_callbacks' if version == CALLBACK_SCHEMA else 'weapon_initializers'
    _fields(value, {'schema', 'owner', 'revision', 'vm_abi', 'phase', 'root_profile', 'programs', binding_key})
    if (version not in (SCHEMA, CALLBACK_SCHEMA) or value['vm_abi'] != 'lua50-x86-double64' or
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
    bindings = value[binding_key]
    if not isinstance(bindings, list) or not 1 <= len(bindings) <= 4096:
        raise ValueError('native program callback count budget')
    seen = set()
    callbacks = []
    for row in bindings:
        _fields(row, {'weapon_id', 'weapon_sha256', 'export', *({'role'} if version == CALLBACK_SCHEMA else set())})
        role = row['role'] if version == CALLBACK_SCHEMA else 'initializer'
        if role not in CALLBACK_ROLES: raise ValueError('unsupported native weapon callback role')
        identity = row['weapon_id']
        if type(identity) is not int or not 1 <= identity <= 9_999_999 or (identity, role) in seen:
            raise ValueError('native program weapon identity/callback repeats')
        seen.add((identity, role))
        record = weapons.get(identity)
        if not isinstance(record, bytes) or len(record) != 1871 or record[4] != 0 or struct.unpack_from('<I', record)[0] != identity:
            raise ValueError('native program must bind a complete imported weapon')
        if _hash(record) != _digest(row['weapon_sha256']):
            raise ValueError('native program weapon SHA-256 differs')
        if not isinstance(row['export'], str) or row['export'] not in names or len(row['export']) > 49:
            raise ValueError('native callback must be a declared bounded export')
        _, offset = CALLBACK_ROLES[role]
        field = record[offset:offset + 100]
        terminator = next((offset for offset in range(0, 100, 2) if field[offset:offset + 2] == b'\0\0'), None)
        if terminator is None or field[:terminator].decode('utf-16-le') != row['export']:
            raise ValueError('native weapon ' + role + ' field does not match export')
        callbacks.append({**row, 'role': role})
    return {'owner': owner, 'revision': value['revision'], 'programs': programs,
            'weapon_callbacks': callbacks,
            'weapon_initializers': [{key: row[key] for key in ('weapon_id','weapon_sha256','export')}
                                    for row in callbacks if row['role']=='initializer']}


def compile_bundle(manifests, catalog_sha256, files, weapons):
    """Encode the optional runtime sidecar against the exact emitted catalog."""
    if not isinstance(manifests, list) or not 1 <= len(manifests) <= 128:
        raise ValueError('native program owner count budget')
    owners, exports, bound, weapon_owners = set(), set(), set(), {}
    version = 2 if any(isinstance(value, dict) and value.get('schema') == CALLBACK_SCHEMA for value in manifests) else 1
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
        word(len(candidate['weapon_callbacks']))
        for binding in candidate['weapon_callbacks']:
            identity, role = binding['weapon_id'], binding['role']
            if identity in weapon_owners and weapon_owners[identity] != candidate['owner']:
                raise ValueError('native program owners share a weapon')
            weapon_owners[identity] = candidate['owner']
            if (identity,role) in bound: raise ValueError('native weapon callback repeats')
            bound.add((identity,role)); word(identity)
            if version == 2: word(CALLBACK_ROLES[role][0])
            payload.extend(bytes.fromhex(binding['weapon_sha256'])); text(binding['export'])
        if len(payload) > MAX_BYTES: raise ValueError('native program bundle byte budget')
    # A declared owner function appearing in a weapon field must have its
    # matching role approved. Merely publishing that global is insufficient.
    for identity,record in weapons.items():
        if not isinstance(record,bytes)or len(record)!=1871:continue
        for role,(_,offset)in CALLBACK_ROLES.items():
            field=record[offset:offset+100]
            end=next((i for i in range(0,100,2)if field[i:i+2]==b'\0\0'),None)
            if end is None:raise ValueError('native weapon callback lacks terminator')
            name=field[:end].decode('utf-16-le')
            if (name in exports or any(name.startswith(owner+'_')for owner in owners))and(identity,role)not in bound:
                raise ValueError('native weapon '+str(identity)+' '+role+' callback lacks its owner binding')
    return (struct.pack('<8s4I', MAGIC, version, 64, len(manifests), len(payload)) +
            bytes.fromhex(_digest(catalog_sha256)) + bytes(8) + payload)


def decode_bundle(raw):
    """Read a compiled sidecar back into owner declarations (the inverse of compile_bundle).

    The container does not record archive member names, so each program gets a synthetic
    ``installed/<owner>/<n>.scp`` member. A v2 container decodes as callback-schema declarations
    (initializer-only owners keep the ``initializer`` role). Recompiling the returned manifests and
    files against the same catalog and weapon records reproduces ``raw`` exactly.
    """
    if not isinstance(raw, bytes) or not 64 <= len(raw) <= 64 + MAX_BYTES:
        raise ValueError('native program bundle byte extent')
    magic, version, header, count, extent = struct.unpack_from('<8s4I', raw)
    if (magic != MAGIC or version not in (1, 2) or header != 64 or not 1 <= count <= 128 or
            extent != len(raw) - 64 or raw[56:64] != bytes(8)):
        raise ValueError('native program bundle header')
    roles = {number: name for name, (number, _) in CALLBACK_ROLES.items()}
    position = 64

    def take(size):
        nonlocal position
        if size > len(raw) - position:
            raise ValueError('truncated native program bundle')
        position += size
        return raw[position - size:position]

    def word():
        return struct.unpack('<I', take(4))[0]

    def text(maximum):
        size = word()
        if not 1 <= size <= maximum:
            raise ValueError('native program bundle text extent')
        try:
            return take(size).decode('ascii')
        except UnicodeError as error:
            raise ValueError('native program bundle text encoding') from error

    manifests, files = [], {}
    for _ in range(count):
        owner, revision = text(48), text(128)
        programs = []
        for index in range(word()):
            resource, digest = text(240), take(32).hex()
            exports = [text(255) for _ in range(word())]
            member = 'installed/%s/%04d.scp' % (owner, index + 1)
            if member in files:
                raise ValueError('duplicate native program owner')
            files[member] = take(word())
            programs.append({'resource': resource, 'member': member, 'sha256': digest, 'exports': exports})
        bindings = []
        for _ in range(word()):
            identity = word()
            role = roles.get(word()) if version == 2 else 'initializer'
            if role is None:
                raise ValueError('unsupported native weapon callback role')
            bindings.append({'weapon_id': identity, 'weapon_sha256': take(32).hex(), 'role': role, 'export': text(49)})
        manifest = {'schema': CALLBACK_SCHEMA if version == 2 else SCHEMA, 'owner': owner, 'revision': revision,
                    'vm_abi': 'lua50-x86-double64', 'phase': 'after-original-root-loader',
                    'root_profile': 'closure-publications-only-v1', 'programs': programs}
        if version == 2:
            manifest['weapon_callbacks'] = bindings
        else:
            manifest['weapon_initializers'] = [{key: row[key] for key in ('weapon_id', 'weapon_sha256', 'export')}
                                               for row in bindings]
        manifests.append(manifest)
    if position != len(raw):
        raise ValueError('native program bundle trailing bytes')
    return {'catalog_sha256': raw[24:56].hex(), 'version': version, 'manifests': manifests, 'files': files}


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


def validate_project_bindings(value, *, files, excluded_members=()):
    """Resolve portable authoring bindings; no runtime authority is issued."""
    bindings = normalize_bindings(value)
    if not bindings: return []
    weapons = {}
    for member, raw in files.items():
        if member in excluded_members:continue
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
