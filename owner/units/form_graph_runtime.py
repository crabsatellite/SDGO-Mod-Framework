"""Bind directed unit records to independent installed server ownership.

Requests contain complete-record hashes and inventory entry declarations, never
permission flags. The controller supplies consistent SQL/ledger readback and
independently established baseline witnesses. No SQL, inventory or runtime write
is performed here; compiled host authority must stay outside content packages.
"""
from __future__ import annotations

import hashlib
import datetime
import json
import re
import struct

from . import repository as repo
from .codec import UNIT_FIELDS, patch_unit

SCHEMA = 'sdgo.unit-form-graphs/1'
HOST_SHA256 = 'f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4'
MAX_BYTES = 8 * 1024 * 1024
MAX_OWNERS = 512
MAX_NODES = 4096
MAX_DECLARATIONS = 16384


def _exact(value, fields, label):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError(label + ' fields differ')


def _hash(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('unit form SHA-256 is invalid')
    return value


def _id(value, *, zero=False):
    if type(value) is not int or not ((zero and value == 0) or repo.UNIT_ID_FIRST <= value <= repo.UNIT_ID_LAST):
        raise ValueError('unit form ID is outside the native server domain')
    return value


def _sha(raw):
    return hashlib.sha256(raw).hexdigest()


def _revision(value):
    if (not isinstance(value, str) or not 1 <= len(value) <= 128 or
            not value.strip() or any(ord(c) < 32 for c in value)):
        raise ValueError('unit form revision is invalid')
    return _sha(value.encode('utf-8'))


def _record(raw, identity):
    if (not isinstance(raw, bytes) or len(raw) != 4976 or raw[4] != 0 or
            struct.unpack_from('<I', raw)[0] != identity):
        raise ValueError('unit form complete client record differs')
    return raw


def _snapshot_row(row, table, columns):
    _exact(row, columns, 'unit form server row')
    for name, kind, size, nullable in repo.SQL_SCHEMAS[table]:
        value = row[name]
        # Existing read-only catalogs can contain legacy NUL padding. Preserve
        # those bytes; importing a newly owned row still uses the strict writer
        # validator below. Numeric types/ranges are checked for every row.
        if kind in ('varchar', 'nvarchar') and isinstance(value, str):
            if len(value) > (size//2 if kind == 'nvarchar' else size):
                raise ValueError('unit form snapshot text extent differs')
        else:repo._validate_scalar(table+'.'+name, value, kind, size, nullable)


def _ledger(row, kind, identity):
    _exact(row, {'record_kind','record_id','owner_key','family','record_key','revision',
                 'original_json','installed_hash','updated_at'}, 'unit form complete ownership ledger')
    if (row['record_kind'] != kind or type(row['record_id']) is not int or row['record_id'] != identity or
            not isinstance(row['family'], str) or repo.FAMILY_KEY.fullmatch(row['family']) is None or
            row['record_key'] != f"{row['family']}/{kind}/{identity}" or
            not isinstance(row['owner_key'], str) or not 1 <= len(row['owner_key']) <= 128 or '\0' in row['owner_key']):
        raise ValueError('unit form ledger record identity differs')
    _revision(row['revision']);_hash(row['installed_hash'])
    try:
        if not isinstance(row['updated_at'], str):raise ValueError()
        datetime.datetime.fromisoformat(row['updated_at'])
        if not isinstance(row['original_json'], str) or len(row['original_json']) > 65536:raise ValueError()
        def pairs(values):
            result = {}
            for key, value in values:
                if key in result:raise ValueError()
                result[key] = value
            return result
        original = json.loads(row['original_json'], object_pairs_hook=pairs)
        if original is not None:
            table, columns, key = repo.TABLES[kind]
            _snapshot_row(original, table, columns)
            if original[key] != identity:raise ValueError()
    except (ValueError, TypeError, KeyError, OverflowError):
        raise ValueError('unit form ledger rollback/time metadata is invalid') from None
    # UpdatedAt is audit time, not content. Every other ledger field, including
    # exact OriginalJson text and RecordKey, belongs to the snapshot fingerprint.
    return {key: value for key, value in row.items() if key != 'updated_at'}


def prepare(requests, client_records, *, server_rows, ownership, originals, item_rows, item_ownership):
    """Readback maps come from the trusted controller, not from a project.

    ownership maps unit IDs to owner_key/family/revision/installed_hash read from
    sdgo_UnitImportDefinitions. originals maps baseline unit IDs to independently
    verified client_sha256/server_sha256 witnesses. Baseline nodes can be shared
    read-only. item_rows and item_ownership are actual item/ledger readback;
    ownership of a form alone never makes it an inventory entry root.
    """
    if not isinstance(requests, list) or not 1 <= len(requests) <= MAX_OWNERS:
        raise ValueError('unit form owner count budget')
    if not isinstance(client_records, dict):raise ValueError('unit form client record map differs')
    if not isinstance(server_rows, dict) or not 1 <= len(server_rows) <= 100000:
        raise ValueError('unit form server snapshot budget')
    rows = {}
    for identity, row in server_rows.items():
        _id(identity)
        _snapshot_row(row, 'tbl_Units', repo.UNIT_COLUMNS)
        if row['UnitNumber'] != identity:raise ValueError('unit form server snapshot key differs')
        rows[identity] = dict(row)
    if not isinstance(ownership, dict) or set(ownership) - set(rows):
        raise ValueError('unit form owned rows are absent from the server snapshot')
    if not isinstance(originals, dict) or set(originals) - set(rows) or set(originals) & set(ownership):
        raise ValueError('unit form original witnesses overlap ownership or missing rows')
    if not isinstance(item_rows, dict) or len(item_rows) > 100000:raise ValueError('unit form item snapshot differs')
    for identity, item in item_rows.items():
        _snapshot_row(item, 'tbl_ItemCategory', repo.ITEM_COLUMNS)
        if type(identity) is not int or not 1 <= identity <= repo.INT_MAX or item['CatalogueID'] != identity:
            raise ValueError('unit form item snapshot key differs')
    if not isinstance(item_ownership, dict) or set(item_ownership)-set(item_rows):
        raise ValueError('unit form item ledger snapshot differs')
    semantic_units = {identity: _ledger(row, 'unit', identity) for identity, row in ownership.items()}
    semantic_items = {}
    for identity, row in item_ownership.items():
        if type(identity) is not int or not 1 <= identity <= repo.INT_MAX:raise ValueError('unit form item ledger ID differs')
        semantic_items[identity] = _ledger(row, 'item', identity)
    for identity, witness in originals.items():
        _exact(witness, {'client_sha256', 'server_sha256'}, 'unit form original witness')
        _hash(witness['client_sha256']);_hash(witness['server_sha256'])
    owners, claimed, distinct, declarations, output = set(), set(), set(), 0, []
    entries = {}
    for request in requests:
        _exact(request, {'schema', 'owner', 'revision', 'roots', 'records'}, 'unit form request')
        owner = request['owner'];revision = _revision(request['revision'])
        if (request['schema'] != SCHEMA or not isinstance(owner, str) or
                repo.FAMILY_KEY.fullmatch(owner) is None or owner in owners):
            raise ValueError('unit form schema/owner is invalid or repeated')
        owners.add(owner)
        roots = request['roots'];records = request['records']
        if (not isinstance(roots, list) or not 1 <= len(roots) <= MAX_NODES or
                not isinstance(records, list) or not 1 <= len(records) <= MAX_NODES):
            raise ValueError('unit form root/record count budget')
        if any(_id(n) != n for n in roots) or len(set(roots)) != len(roots):
            raise ValueError('unit form entry roots repeat')
        nodes = {}
        for declaration in records:
            _exact(declaration, {'id', 'client_sha256'}, 'unit form record declaration')
            identity = _id(declaration['id']);digest = _hash(declaration['client_sha256'])
            if identity in nodes:raise ValueError('unit form record is repeated')
            raw = _record(client_records.get(identity), identity)
            if _sha(raw) != digest:raise ValueError('unit form client SHA-256 differs')
            installed = rows.get(identity)
            if installed is None:raise ValueError('unit form server row is absent')
            server_hash = repo._row_hash(installed, repo.UNIT_COLUMNS)
            # Re-encoding is a comparison only. Every audited shared scalar,
            # including transform/skills/weapons, must agree with SQL readback.
            shared = {key: value for key, value in installed.items()
                      if key in UNIT_FIELDS and UNIT_FIELDS[key]['type'] != 'utf16le'}
            if patch_unit(raw, shared) != raw:
                raise ValueError('unit form client/server shared fields differ')
            ledger = ownership.get(identity)
            if ledger is not None:
                if (ledger['owner_key'] != repo.OWNER_KEY or ledger['family'] != owner or
                        ledger['revision'] != request['revision'] or identity in claimed):
                    raise ValueError('unit form installed ownership/revision differs')
                repo._validate_complete_row(installed, 'tbl_Units', repo.UNIT_COLUMNS)
                if _hash(ledger['installed_hash']) != server_hash:
                    raise ValueError('unit form server row changed outside its ledger')
                origin = 'owned';claimed.add(identity)
            else:
                witness = originals.get(identity)
                if witness != {'client_sha256': digest, 'server_sha256': server_hash}:
                    raise ValueError('unit form original record lacks independent witnesses')
                origin = 'original'
            target = _id(struct.unpack_from('<I', raw, 316)[0], zero=True)
            nodes[identity] = {'id': identity, 'target': target, 'origin': origin,
                               'client_sha256': digest, 'server_sha256': server_hash}
            distinct.add(identity);declarations += 1
            if len(distinct) > MAX_NODES or declarations > MAX_DECLARATIONS:
                raise ValueError('unit form aggregate record budget')
        if any(root not in nodes or nodes[root]['origin'] != 'owned' for root in roots):
            raise ValueError('unit form inventory root must be an owned supplied node')
        for root in roots:
            matches = [(item, item_ownership[identity]) for identity, item in item_rows.items()
                if item['RefIndex'] == root and identity//100000 == 11 and identity in item_ownership and
                item_ownership[identity]['family'] == owner and item_ownership[identity]['revision'] == request['revision']]
            if len(matches) != 1:raise ValueError('unit form owned inventory entry is absent or ambiguous')
            item, ledger = matches[0]
            repo._validate_complete_row(item, 'tbl_ItemCategory', repo.ITEM_COLUMNS)
            if (item['CatalogueID'] // 100000 != 11 or item['RefIndex'] != root or
                    ledger['owner_key'] != repo.OWNER_KEY or ledger['family'] != owner or
                    ledger['revision'] != request['revision'] or
                    _hash(ledger['installed_hash']) != repo._row_hash(item, repo.ITEM_COLUMNS)):
                raise ValueError('unit form installed inventory entry differs')
            entries[root] = item
        if any(node['target'] and node['target'] not in nodes for node in nodes.values()):
            raise ValueError('unit form directed record references are not closed')
        # Keep auxiliary/incoming forms as explicit nodes, without turning them
        # into inventory roots or inventing a reverse transform edge.
        output.append({'owner': owner, 'revision_sha256': revision, 'roots': sorted(roots),
                       'nodes': [nodes[n] for n in sorted(nodes)]})
    fingerprint = _sha(repo._canonical_json({'rows': [rows[n] for n in sorted(rows)],
        'ownership': [[n, semantic_units[n]] for n in sorted(semantic_units)],
        'item_ownership': [[n, semantic_items[n]] for n in sorted(semantic_items)],
        'originals': [[n, originals[n]] for n in sorted(originals)],
        'entries': [[n, entries[n], semantic_items[entries[n]['CatalogueID']]] for n in sorted(entries)]}).encode('utf-8'))
    return {'owners': sorted(output, key=lambda row: row['owner']), 'server_fingerprint': fingerprint,
            'native_actor_accepted': False, 'ordinary_publication_allowed': False}


def compile_files(prepared):
    """Compile paired candidate/host bytes; this does not install either file."""
    _exact(prepared, {'owners', 'server_fingerprint', 'native_actor_accepted',
                     'ordinary_publication_allowed'}, 'prepared unit form graph')
    if prepared['native_actor_accepted'] is not False or prepared['ordinary_publication_allowed'] is not False:
        raise ValueError('unit form compilation cannot claim actor/publication acceptance')
    owners = prepared['owners']
    if not isinstance(owners, list) or not 1 <= len(owners) <= MAX_OWNERS:
        raise ValueError('unit form runtime owner budget')
    payload = bytearray();names = set();owned = set();all_ids = set();total = 0;identities = {}
    for owner in owners:
        _exact(owner, {'owner', 'revision_sha256', 'roots', 'nodes'}, 'prepared unit form owner')
        name = owner['owner']
        if not isinstance(name, str) or repo.FAMILY_KEY.fullmatch(name) is None or name in names:
            raise ValueError('unit form runtime owner is invalid or repeated')
        names.add(name);raw = name.encode('ascii');payload.extend(struct.pack('<I', len(raw)));payload.extend(raw)
        payload.extend(bytes.fromhex(_hash(owner['revision_sha256'])))
        roots, nodes = owner['roots'], owner['nodes']
        if not isinstance(roots, list) or not 1 <= len(roots) <= MAX_NODES or roots != sorted(set(roots)):
            raise ValueError('unit form runtime root order/budget')
        payload.extend(struct.pack('<I', len(roots)))
        for root in roots:payload.extend(struct.pack('<I', _id(root)))
        if not isinstance(nodes, list) or not 1 <= len(nodes) <= MAX_NODES:
            raise ValueError('unit form runtime node budget')
        payload.extend(struct.pack('<I', len(nodes)));ids = {};previous = 0
        for node in nodes:
            _exact(node, {'id', 'target', 'origin', 'client_sha256', 'server_sha256'}, 'prepared unit form node')
            identity, target = _id(node['id']), _id(node['target'], zero=True)
            if identity <= previous or node['origin'] not in ('owned', 'original'):
                raise ValueError('unit form runtime node order/origin')
            if node['origin'] == 'owned':
                if identity in owned:raise ValueError('unit form runtime owned node crosses owners')
                owned.add(identity)
            signature = (target, node['origin'], _hash(node['client_sha256']), _hash(node['server_sha256']))
            if identity in identities and identities[identity] != signature:
                raise ValueError('unit form runtime shared identity differs')
            identities[identity] = signature
            previous = identity;ids[identity] = node;all_ids.add(identity);total += 1
            payload.extend(struct.pack('<III', identity, target, 1 if node['origin'] == 'owned' else 2))
            payload.extend(bytes.fromhex(_hash(node['client_sha256'])))
            payload.extend(bytes.fromhex(_hash(node['server_sha256'])))
        if any(root not in ids or ids[root]['origin'] != 'owned' for root in roots) or any(
                node['target'] and node['target'] not in ids for node in nodes):
            raise ValueError('unit form runtime root/reference closure differs')
    if len(all_ids) > MAX_NODES or total > MAX_DECLARATIONS or len(payload) > MAX_BYTES:
        raise ValueError('unit form runtime aggregate budget')
    candidate = (struct.pack('<8s4I', b'SDGOUF1\0', 1, 64, len(owners), len(payload)) +
                 bytes.fromhex(_sha(payload)) + bytes(8) + payload)
    authority = (struct.pack('<8s4I', b'SDGOUA1\0', 1, 128, len(owners), len(payload)) +
                 bytes.fromhex(_sha(candidate)) + bytes.fromhex(_hash(prepared['server_fingerprint'])) +
                 bytes.fromhex(HOST_SHA256) + bytes(8) + payload)
    return bytes(candidate), bytes(authority)


def prepare_from_cursor(cursor, requests, client_records, *, originals):
    """Read a consistent installed graph/entry snapshot; never write rows."""
    from .server_dependencies import _schema
    transaction = cursor.execute('SELECT @@TRANCOUNT,XACT_STATE()').fetchone()
    if transaction is None or int(transaction[0]) < 1 or int(transaction[1]) != 1:
        raise ValueError('unit form authority requires an active committable transaction')
    lock = cursor.execute("DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,"
        "@LockMode=N'Shared',@LockOwner=N'Transaction',@LockTimeout=5000; SELECT @r;", repo.APP_LOCK).fetchone()
    if lock is None or int(lock[0]) < 0:raise ValueError('another unit catalog transaction is active')
    for table in ('tbl_Units', 'tbl_ItemCategory'):_schema(cursor, table, repo.SQL_SCHEMAS[table])
    _schema(cursor, 'sdgo_UnitImportDefinitions', repo.LEDGER_SCHEMA)
    rows = {int(row[0]): dict(zip(repo.UNIT_COLUMNS, row)) for row in cursor.execute(
        'SELECT '+','.join('['+column+']' for column in repo.UNIT_COLUMNS)+' FROM dbo.tbl_Units WITH (HOLDLOCK)')}
    items = {int(row[0]): dict(zip(repo.ITEM_COLUMNS, row)) for row in cursor.execute(
        'SELECT '+','.join('['+column+']' for column in repo.ITEM_COLUMNS)+' FROM dbo.tbl_ItemCategory WITH (HOLDLOCK)')}
    ledgers = {(str(row[0]), int(row[1])): dict(zip(('record_kind','record_id','owner_key','family',
        'record_key','revision','original_json','installed_hash','updated_at'),
        (*row[:8], row[8].isoformat() if isinstance(row[8], datetime.datetime) else row[8])))
        for row in cursor.execute("SELECT RecordKind,RecordID,OwnerKey,FamilyKey,RecordKey,Revision,OriginalJson,InstalledHash,UpdatedAt "
            "FROM dbo.sdgo_UnitImportDefinitions WITH (HOLDLOCK) WHERE RecordKind IN ('unit','item')")}
    ownership = {identity: ledger for (kind, identity), ledger in ledgers.items() if kind == 'unit'}
    return prepare(requests, client_records, server_rows=rows, ownership=ownership, originals=originals, item_rows=items,
                   item_ownership={identity: ledger for (kind, identity), ledger in ledgers.items() if kind == 'item'})
