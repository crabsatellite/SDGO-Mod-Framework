"""Source-bound kind-11 transport for the native 732-byte record layout.

Only the identity word may be relocated. Unknown payload fields remain opaque;
this profile grants neither native registration nor actor/runtime acceptance.
"""
from __future__ import annotations

import hashlib
import re
import struct

PROFILE_ID = 'native-type11-732-v1'
RECORD_KIND = 11
RECORD_SIZE = 732
MAX_ID = 9_999_999
HOST_SHA256 = 'e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92'


def _identity(value, label, minimum=1):
    if type(value) is not int or not minimum <= value <= MAX_ID:
        raise ValueError(f'{label} must be in {minimum}..{MAX_ID}')
    return value


def _record(raw, label):
    if not isinstance(raw, bytes) or len(raw) != RECORD_SIZE:
        raise ValueError(f'{label} must retain a complete 732-byte kind-11 record')
    identity = _identity(struct.unpack_from('<I', raw)[0], label+' identity')
    if raw[4] != 0:
        raise ValueError(f'{label} security byte must be zero')
    return identity


def _hash(raw, expected, label):
    if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected):
        raise ValueError(label+' SHA-256 must be lowercase hex')
    if hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError(label+' SHA-256 mismatch')


def prepare_record(source: bytes, *, target_id: int) -> bytes:
    """Copy the exact source payload, changing only its verified ID word."""
    _record(source, 'source')
    _identity(target_id, 'target_id')
    return struct.pack('<I', target_id) + source[4:]


def validate_record(*, source: bytes, target: bytes, source_id: int,
                    target_id: int, source_row: int, source_sha256: str,
                    target_sha256: str) -> dict:
    """Return transport IR; registration remains a separate consumer gate."""
    actual_source = _record(source, 'source')
    actual_target = _record(target, 'target')
    _identity(source_id, 'source_id')
    _identity(target_id, 'target_id')
    _identity(source_row, 'source_row', minimum=0)
    if actual_source != source_id or actual_target != target_id:
        raise ValueError('kind-11 source/target identity mismatch')
    _hash(source, source_sha256, 'source')
    _hash(target, target_sha256, 'target')
    if target != prepare_record(source, target_id=target_id):
        raise ValueError('kind-11 relocation changed bytes outside the identity word')
    recovered = prepare_record(target, target_id=source_id)
    if recovered != source:
        raise ValueError('kind-11 inverse relocation differs from source')
    return {
        'schema': 'sdgo.native-type11-record/1',
        'profile_id': PROFILE_ID, 'record_kind': RECORD_KIND,
        'host_sha256': HOST_SHA256, 'source_row': source_row,
        'source': {'id': source_id, 'size': RECORD_SIZE, 'sha256': source_sha256,
                   'retention': 'complete-record-required'},
        'target': {'id': target_id, 'size': RECORD_SIZE, 'sha256': target_sha256},
        'allowed_mutation_spans': [{'offset': 0, 'bytes': 4}],
        'unknown_payload_preserved': True,
        'source_recoverable': True,
        'admission': {
            'native_registration_blocked': True,
            'native_runtime_accepted': False,
            'blockers': [
                'native consumer kind-11 capability is not enabled',
                'successful native registration and destruction is unresolved',
                'weapon, resource, server-field and actor lifecycle closure is unresolved',
            ],
        },
    }
