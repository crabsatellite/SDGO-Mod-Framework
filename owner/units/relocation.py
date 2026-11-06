"""Deterministic, snapshot-bound ID requests; never an allocation authority.

This module does no filesystem, database, network or native-runtime I/O. The
caller supplies authenticated snapshot/request bytes and exact source records.
Installation still needs its own lock/CAS/reservation and runtime tree checks.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
from typing import Mapping

MAX_TARGET_ID = 0x7FFFFFFF


class RelocationError(ValueError):
    pass


def _sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise RelocationError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def _load(raw: bytes, expected: str, maximum: int, label: str):
    if not isinstance(raw, bytes) or not raw or len(raw) > maximum or _sha(raw) != expected:
        raise RelocationError(label + ' bytes/hash mismatch')
    try:
        def constant(value):
            raise RelocationError('non-finite JSON constant: ' + value)
        result = json.loads(raw.decode('utf-8-sig'), object_pairs_hook=_object, parse_constant=constant)
    except (UnicodeError, json.JSONDecodeError) as e:
        raise RelocationError(label + ' JSON invalid') from e
    if not isinstance(result, dict):
        raise RelocationError(label + ' must be an object')
    return result


def _int(value, label, maximum=MAX_TARGET_ID):
    if type(value) is not int or not 1 <= value <= maximum:
        raise RelocationError(label + ' integer invalid')
    return value


def _hash(value, label):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise RelocationError(label + ' hash invalid')
    return value


def _key(value):
    if not isinstance(value, str) or not re.fullmatch('[a-zA-Z0-9_.:-]{1,128}', value):
        raise RelocationError('source record key invalid')
    return value


def _fields(value, required, optional=()):
    if not isinstance(value, dict) or set(value) - set(required) - set(optional) or set(required) - set(value):
        raise RelocationError('missing or unknown contract fields')


def _ranges(value, maximum=MAX_TARGET_ID):
    if not isinstance(value, list) or not value:
        raise RelocationError('explicit allocation ranges required')
    result = []
    for interval in value:
        _fields(interval, ('first', 'last'))
        first = _int(interval['first'], 'first target ID', maximum)
        last = _int(interval['last'], 'last target ID', maximum)
        if last < first:
            raise RelocationError('allocation interval inverted')
        result.append((first, last))
    result.sort()
    if any(right[0] <= left[1] for left, right in zip(result, result[1:])):
        raise RelocationError('allocation intervals overlap')
    return result


def plan(request_bytes: bytes, snapshot_bytes: bytes, source_records: Mapping[str, bytes], *,
         request_sha256: str, snapshot_sha256: str, target_domains=None):
    """Return ``(JSON plan, generated record copies)`` with no external writes.

    Record keys, not ambiguous numeric IDs, identify reference targets. Only the
    identity word and explicitly declared, verified uint32 reference fields are
    rewritten. Every other source byte is retained, including unknown fields.
    """
    request = _load(request_bytes, request_sha256, 16 * 1024 * 1024, 'request')
    snapshot = _load(snapshot_bytes, snapshot_sha256, 64 * 1024 * 1024, 'snapshot')
    _fields(request, ('schema', 'owner', 'revision', 'allocation', 'records', 'references',
                      'referenceInventoryComplete'), ('unresolved', 'evidence'))
    _fields(snapshot, ('schema', 'revision', 'occupiedIds', 'typedRecords', 'coverage', 'sources'))
    version = {'sdgo:record-relocation-request/1': 1, 'sdgo:record-relocation-request/2': 2}.get(request['schema'])
    if version is None or snapshot['schema'] != 'sdgo:record-key-snapshot/1':
        raise RelocationError('unsupported relocation contract')
    if not isinstance(request['owner'], str) or not re.fullmatch('[a-z_][a-z0-9_]{0,47}', request['owner']):
        raise RelocationError('owner invalid')
    for value in (request['revision'], snapshot['revision']):
        if not isinstance(value, str) or not 1 <= len(value) <= 128:
            raise RelocationError('revision invalid')
    if type(request['referenceInventoryComplete']) is not bool:
        raise RelocationError('reference coverage must be explicit')
    if not isinstance(snapshot['coverage'], dict) or not snapshot['coverage'] or any(type(v) is not bool for v in snapshot['coverage'].values()):
        raise RelocationError('snapshot coverage invalid')
    if not isinstance(snapshot['sources'], list) or not snapshot['sources']:
        raise RelocationError('snapshot evidence sources missing')
    for source in snapshot['sources']:
        _fields(source, ('role', 'sha256'), ('identity', 'coverage', 'member'))
        if not isinstance(source['role'], str) or not source['role']:
            raise RelocationError('snapshot evidence role missing')
        _hash(source['sha256'], 'snapshot source')
    if not isinstance(snapshot['occupiedIds'], list) or len(snapshot['occupiedIds']) > 5_000_000:
        raise RelocationError('occupied ID inventory invalid')
    occupied = {_int(n, 'occupied ID', 0xFFFFFFFF) for n in snapshot['occupiedIds']}
    if len(occupied) != len(snapshot['occupiedIds']):
        raise RelocationError('occupied ID inventory contains duplicates')
    typed = {}
    if not isinstance(snapshot['typedRecords'], list):
        raise RelocationError('typed record inventory invalid')
    for record in snapshot['typedRecords']:
        _fields(record, ('kind', 'id', 'sha256'), ('owner', 'origin'))
        identity = (_int(record['kind'], 'record kind', 0xFFFFFFFF), _int(record['id'], 'typed ID', 0xFFFFFFFF))
        _hash(record['sha256'], 'typed record')
        if identity[1] not in occupied or identity in typed:
            raise RelocationError('typed record identity is absent or ambiguous')
        typed[identity] = record['sha256']
    policy = request['allocation']
    _fields(policy, ('ranges', 'preserveUnobservedSourceIds'), ('kindPolicies',) if version == 2 else ())
    if type(policy['preserveUnobservedSourceIds']) is not bool or not isinstance(policy['ranges'], list) or not policy['ranges']:
        raise RelocationError('explicit allocation policy required')
    ranges = _ranges(policy['ranges'])
    kind_policies = {}
    if 'kindPolicies' in policy:
        if not isinstance(policy['kindPolicies'], list) or not policy['kindPolicies']:
            raise RelocationError('kind allocation policies must be a nonempty list')
        for entry in policy['kindPolicies']:
            _fields(entry, ('kind','ranges','maxTargetId'))
            kind = _int(entry['kind'], 'policy kind', 0xFFFFFFFF)
            if kind in kind_policies:
                raise RelocationError('duplicate kind allocation policy')
            maximum = _int(entry['maxTargetId'], 'kind maximum target ID')
            kind_policies[kind] = (_ranges(entry['ranges'], maximum), maximum)
    domains={}
    if target_domains is not None:
        if not isinstance(target_domains,dict) or not target_domains or len(target_domains)>255:
            raise RelocationError('host target domains must be a nonempty bounded map')
        for kind,bounds in target_domains.items():
            _int(kind,'host target kind',0xFFFFFFFF)
            if not isinstance(bounds,(list,tuple)) or len(bounds)!=2:
                raise RelocationError('host target domain bounds invalid')
            first,last=(_int(value,'host target domain bound') for value in bounds)
            if first>last:raise RelocationError('host target domain inverted')
            domains[kind]=(first,last)
    def allowed(kind):
        selected,maximum=kind_policies.get(kind,(ranges,MAX_TARGET_ID))
        if kind not in domains:return selected
        first,last=domains[kind];last=min(last,maximum)
        result=[(max(a,first),min(b,last)) for a,b in selected if max(a,first)<=min(b,last)]
        if not result:raise RelocationError('allocation ranges are outside the host target domain for kind '+str(kind))
        return result
    if not isinstance(request['records'], list) or not 1 <= len(request['records']) <= 100000:
        raise RelocationError('source record count invalid')
    records, raws = {}, {}
    for record in request['records']:
        _fields(record, ('key', 'kind', 'id', 'bytes', 'sha256'), ('member',))
        key = _key(record['key']); _int(record['kind'], 'source kind', 0xFFFFFFFF); _int(record['id'], 'source ID')
        if key in records or type(record['bytes']) is not int or not 4 <= record['bytes'] <= 1024 * 1024:
            raise RelocationError('source record key/extent invalid')
        _hash(record['sha256'], 'source record')
        raw = source_records.get(key)
        if not isinstance(raw, bytes) or len(raw) != record['bytes'] or _sha(raw) != record['sha256'] or struct.unpack_from('<I', raw)[0] != record['id']:
            raise RelocationError('actual source record differs: ' + key)
        records[key], raws[key] = record, raw
    if set(source_records) != set(records):
        raise RelocationError('source blob inventory differs from request')

    keys = sorted(records)
    target, disposition = {}, {}
    used = set(occupied)
    if policy['preserveUnobservedSourceIds']:
        for key in keys:
            value = records[key]['id']
            maximum = kind_policies.get(records[key]['kind'], (None, MAX_TARGET_ID))[1]
            first,last=domains.get(records[key]['kind'],(1,maximum))
            if value not in used and first<=value<=min(last,maximum):
                target[key] = value; used.add(value); disposition[key] = 'source-id-not-observed-in-snapshot'
    cursors = {}
    for key in keys:
        if key in target:
            continue
        policy_key = records[key]['kind'] if records[key]['kind'] in kind_policies or records[key]['kind'] in domains else None
        allowed_ranges = allowed(records[key]['kind'])
        cursor, next_id = cursors.get(policy_key, (0, allowed_ranges[0][0]))
        while cursor < len(allowed_ranges):
            if next_id > allowed_ranges[cursor][1]:
                cursor += 1
                if cursor < len(allowed_ranges): next_id = allowed_ranges[cursor][0]
                continue
            if next_id not in used:
                break
            next_id += 1
        if cursor == len(allowed_ranges):
            raise RelocationError('requested target ID range exhausted')
        target[key] = next_id; used.add(next_id); next_id += 1
        cursors[policy_key] = (cursor, next_id)
        disposition[key] = 'new-id-requested-for-source-collision' if records[key]['id'] in occupied else 'new-id-requested-by-policy-or-source-duplicate'

    if not isinstance(request['references'], list) or len(request['references']) > 1_000_000:
        raise RelocationError('reference inventory invalid')
    for ref in request['references']:
        _fields(ref, ('record', 'offset', 'sourceId', 'targetKind'), ('target', 'externalId', 'expectedExternalSha256', 'role'))
        _key(ref['record'])
        if type(ref['offset']) is not int:
            raise RelocationError('reference offset invalid')
    generated = {key: bytearray(raw) for key, raw in raws.items()}
    spans = {key: [(0, 4)] for key in keys}
    for key in keys: struct.pack_into('<I', generated[key], 0, target[key])
    edges = []
    for ref in sorted(request['references'], key=lambda r: (r['record'], r['offset'])):
        _fields(ref, ('record', 'offset', 'sourceId', 'targetKind'), ('target', 'externalId', 'expectedExternalSha256', 'role'))
        key = _key(ref['record'])
        if key not in records or type(ref['offset']) is not int or not 4 <= ref['offset'] <= len(raws[key]) - 4:
            raise RelocationError('reference extent invalid')
        offset = ref['offset']; source_id = _int(ref['sourceId'], 'reference source ID'); kind = _int(ref['targetKind'], 'reference kind', 0xFFFFFFFF)
        if struct.unpack_from('<I', raws[key], offset)[0] != source_id or any(offset < end and offset + 4 > first for first, end in spans[key]):
            raise RelocationError('reference source value or overlap invalid')
        spans[key].append((offset, offset + 4))
        if ('target' in ref) == ('externalId' in ref):
            raise RelocationError('reference must name one exact record key or external ID')
        item = dict(record=key, offset=offset, width=4, sourceId=source_id, targetKind=kind, role=ref.get('role'))
        if 'target' in ref:
            dest = _key(ref['target'])
            if dest not in records or records[dest]['id'] != source_id or records[dest]['kind'] != kind or 'expectedExternalSha256' in ref:
                raise RelocationError('owned reference target identity/kind invalid')
            value = target[dest]; item.update(target=dest, requestedTargetId=value, mode='owned-reference-rewritten')
        else:
            value = _int(ref['externalId'], 'external ID')
            if kind in domains and not domains[kind][0]<=value<=domains[kind][1]:
                raise RelocationError('external reference exceeds its host target domain')
            if value != source_id:
                raise RelocationError('external reference cannot silently change its source ID')
            observed = typed.get((kind, value)); expected = ref.get('expectedExternalSha256')
            if expected is not None and observed != _hash(expected, 'external record'):
                raise RelocationError('external record fingerprint missing or changed')
            item.update(requestedTargetId=value, mode='external-id-retained', identityObserved=observed is not None, recordSha256=observed)
        struct.pack_into('<I', generated[key], offset, value); edges.append(item)

    output = {}; requests = []
    for key in keys:
        original, rewritten = raws[key], bytes(generated[key]); allowed = set()
        for first, end in spans[key]: allowed.update(range(first, end))
        if any(a != b and i not in allowed for i, (a, b) in enumerate(zip(original, rewritten))):
            raise RelocationError('undeclared source mutation')
        output[key] = rewritten
        requests.append(dict(key=key, owner=request['owner'], kind=records[key]['kind'], sourceId=records[key]['id'], requestedTargetId=target[key], sourceSha256=records[key]['sha256'], requestedRecordSha256=_sha(rewritten), bytes=len(rewritten), disposition=disposition[key], allowedMutationSpans=[dict(offset=a, bytes=b-a) for a, b in sorted(spans[key])]))
    unresolved = request.get('unresolved', [])
    if not isinstance(unresolved, list) or any(not isinstance(item, dict) for item in unresolved):
        raise RelocationError('unresolved reference inventory invalid')
    result = dict(schema='sdgo:record-relocation-plan/1', owner=request['owner'], sourceRevision=request['revision'], requestSha256=request_sha256, snapshotSha256=snapshot_sha256, snapshotRevision=snapshot['revision'], snapshotCoverage=snapshot['coverage'], sourceEvidence=snapshot['sources'], requests=requests, references=edges, unresolved=unresolved, referenceInventoryComplete=request['referenceInventoryComplete'], originalSourceRecordsChanged=False, generatedCopiesOnly=True, unknownBytesPreserved=True, requiresInstallerLockAndCAS=True, requiresOwnerReservationAndRuntimeReadback=True, allocationAuthorityIssued=False, idsReserved=False, nativeRecordRegistrationAllowed=False, activationAllowed=False)
    if version == 2:
        result.update(schema='sdgo:record-relocation-plan/2', allocationPolicy=policy)
    if domains:
        result['targetDomains']=[{'kind':kind,'first':first,'last':last} for kind,(first,last) in sorted(domains.items())]
    return result, output
