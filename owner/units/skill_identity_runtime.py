"""Compile native selector requests against independent installed SQL ownership.

The caller supplies a consistent server snapshot. Content requests never carry
an admission flag, and the generated host authority is not a project member.
It must be installed by the trusted deployment controller after readback.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct

from .profiles.skill_target_1132 import recover_source_record
from .server_dependencies import normalize_row, row_hash, canonical, prepare as prepare_dependency

SCHEMA = 'sdgo.unit-skill-identities/1'
HOST_SHA256 = 'f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4'
SUPPORTED_SOURCES = frozenset({70116, 70138, 70207, 70234})
MAX_BYTES = 8 * 1024 * 1024


def _exact(value, keys, label):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ValueError(label + ' fields differ')


def _id(value):
    if type(value) is not int or not 1 <= value <= 9_999_999:
        raise ValueError('skill identity ID is invalid')
    return value


def _hash(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('skill identity SHA-256 is invalid')
    return value


def _sha(value):
    return hashlib.sha256(value).hexdigest()


def prepare(requests, client_records, *, server_rows, ownership):
    """Bind requests to complete installed rows, ledger entries and templates.

    ownership contains *all* owned skill rows, not just the selected owners.
    The controller must read these maps itself; they are not request metadata.
    No row, account, inventory or native actor is changed here.
    """
    if not isinstance(requests, list) or not 1 <= len(requests) <= 128:
        raise ValueError('skill identity owner count budget')
    # Unrelated baseline text can contain legacy padding/NUL bytes. Retain it
    # in the snapshot fingerprint; only the selected source/destination rows
    # are normalized and admitted. Never clean or rewrite original rows.
    from .repository import SKILL_COLUMNS
    if any(not isinstance(row, dict) or set(row) != set(SKILL_COLUMNS) for row in server_rows.values()):
        raise ValueError('server skill snapshot columns differ')
    rows = {identity: dict(row) for identity, row in server_rows.items()}
    if any(_id(identity) != row['SkillID'] for identity, row in rows.items()):
        raise ValueError('server skill snapshot key differs')
    if set(ownership) - set(rows):
        raise ValueError('owned skill rows are missing from the server snapshot')
    originals = set(rows) - set(ownership)
    if not originals or len(originals) > 100_000:
        raise ValueError('original skill snapshot budget')
    owners, destinations, result = set(), set(), []
    for request in requests:
        _exact(request, {'schema', 'owner', 'revision', 'bindings'}, 'skill identity request')
        owner, revision = request['owner'], _hash(request['revision'])
        if (request['schema'] != SCHEMA or not isinstance(owner, str) or
                not re.fullmatch('[a-z0-9][a-z0-9._-]{0,63}', owner) or owner in owners):
            raise ValueError('skill identity schema/owner is invalid or repeated')
        owners.add(owner)
        bindings = request['bindings']
        if not isinstance(bindings, list) or not 1 <= len(bindings) <= 4096:
            raise ValueError('skill identity binding count budget')
        prepared = []
        for entry in bindings:
            _exact(entry, {'target', 'source', 'client_sha256'}, 'skill identity binding')
            target, source = _id(entry['target']), _id(entry['source'])
            if (target in destinations or target in originals or target == source or
                    source not in originals or source not in SUPPORTED_SOURCES):
                raise ValueError('skill identity allocation/original selector differs')
            destinations.add(target)
            record = client_records.get(target)
            digest = _hash(entry['client_sha256'])
            if (not isinstance(record, bytes) or len(record) != 1132 or record[4] or
                    struct.unpack_from('<I', record)[0] != target or _sha(record) != digest):
                raise ValueError('skill identity complete client record differs')
            recover_source_record(record)
            if record[1131] > 3:
                raise ValueError('skill identity unknown client dispatch remains blocked')
            installed, ledger = rows.get(target), ownership.get(target)
            if (installed is None or ledger is None or ledger.get('owner') != owner or
                    ledger.get('revision') != revision or ledger.get('client_hash') != digest):
                raise ValueError('skill identity installed ownership/revision differs')
            installed = normalize_row(installed)
            template = normalize_row(rows[source])
            server_digest, template_digest = row_hash(installed), row_hash(template)
            if server_digest != ledger.get('installed_hash'):
                raise ValueError('skill identity server row changed outside its ownership ledger')
            try:
                definition = json.loads(ledger['definition_json'])
            except (KeyError, TypeError, ValueError):
                raise ValueError('skill identity installed template declaration is invalid') from None
            if definition != {'kind': 'template', 'id': source, 'sha256': template_digest}:
                raise ValueError('skill identity source is not the installed complete stock template')
            expected = dict(template, SkillID=target)
            if row_hash(expected) != server_digest:
                raise ValueError('skill identity template clone differs from the installed server row')
            prepare_dependency({'schema':'sdgo.unit-server-dependencies/1','owner':owner,'revision':revision,
                'skills':[{'id':target,'client_sha256':digest,'definition':definition}]}, {target:record},
                templates={source:template},current={target:installed},ownership={target:ledger},available_ids=set(rows))
            prepared.append({'target': target, 'source': source, 'client_sha256': digest,
                             'server_sha256': server_digest, 'template_sha256': template_digest})
        result.append({'owner': owner, 'revision': revision, 'skills': prepared})
    if len(destinations) > 4096:
        raise ValueError('skill identity aggregate binding count budget')
    fingerprint = _sha(canonical({'rows': [rows[key] for key in sorted(rows)],
        'ownership': [[key, ownership[key]] for key in sorted(ownership)]}).encode('utf-8'))
    return {'owners': result, 'original_ids': sorted(originals), 'server_fingerprint': fingerprint,
            'native_actor_accepted': False, 'ordinary_publication_allowed': False}


def compile_files(prepared):
    payload = bytearray()
    for owner in prepared['owners']:
        for name in (owner['owner'], owner['revision']):
            raw = name.encode('ascii'); payload.extend(struct.pack('<I', len(raw))); payload.extend(raw)
        payload.extend(struct.pack('<I', len(owner['skills'])))
        for skill in owner['skills']:
            payload.extend(struct.pack('<II', skill['target'], skill['source']))
            for name in ('client_sha256', 'server_sha256', 'template_sha256'):
                payload.extend(bytes.fromhex(_hash(skill[name])))
    if len(payload) > MAX_BYTES:
        raise ValueError('skill identity runtime byte budget')
    count = len(prepared['owners'])
    candidate = (struct.pack('<8s4I', b'SDGOSI1\0', 1, 64, count, len(payload)) +
                 bytes.fromhex(_sha(payload)) + bytes(8) + payload)
    originals = prepared['original_ids']
    authority_payload = b''.join(struct.pack('<I', identity) for identity in originals) + payload
    authority = (struct.pack('<8s5I', b'SDGOSA1\0', 1, 128, len(originals), count, len(authority_payload)) +
                 bytes.fromhex(_sha(candidate)) + bytes.fromhex(_hash(prepared['server_fingerprint'])) +
                 bytes.fromhex(HOST_SHA256) + bytes(4) + authority_payload)
    return bytes(candidate), bytes(authority)


def prepare_from_cursor(cursor, requests, client_records):
    """Read the authority from the selected SQL transaction, without writes."""
    from .repository import SKILL_COLUMNS, APP_LOCK
    from .server_dependencies import _schema, SQL_SCHEMA, LEDGER, LEDGER_SCHEMA
    transaction = cursor.execute('SELECT @@TRANCOUNT,XACT_STATE()').fetchone()
    if transaction is None or int(transaction[0]) < 1 or int(transaction[1]) != 1:
        raise ValueError('skill identity authority requires an active committable transaction')
    lock = cursor.execute("DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,"
        "@LockMode=N'Shared',@LockOwner=N'Transaction',@LockTimeout=5000; SELECT @r;", APP_LOCK).fetchone()
    if lock is None or int(lock[0]) < 0:
        raise ValueError('another unit catalog transaction is active')
    _schema(cursor, 'tbl_SkillRev', SQL_SCHEMA); _schema(cursor, LEDGER, LEDGER_SCHEMA)
    rows = {int(row[0]): dict(zip(SKILL_COLUMNS, row)) for row in cursor.execute(
        'SELECT ' + ','.join('[' + name + ']' for name in SKILL_COLUMNS) + ' FROM dbo.tbl_SkillRev WITH (HOLDLOCK)')}
    ownership = {int(row[0]): dict(zip(('owner', 'revision', 'client_hash', 'definition_json', 'installed_hash'), row[1:]))
        for row in cursor.execute("SELECT RecordID,OwnerKey,Revision,ClientHash,DefinitionJson,InstalledHash "
            "FROM dbo.sdgo_UnitImportDependencies WITH (HOLDLOCK) WHERE RecordKind='skill'")}
    return prepare(requests, client_records, server_rows=rows, ownership=ownership)
