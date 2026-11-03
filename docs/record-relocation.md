# Snapshot-bound record ID requests

`owner.units.relocation.plan` is a pure planning step for an importer transaction.
It does not discover a server, read an INI, connect to a database, reserve an ID,
publish resources, or write a native tree. The caller supplies exact request and
snapshot bytes, both expected SHA-256 values, and the authenticated source blobs.

The native tree has one global numeric key space. A key occupied by another
record kind is still occupied. Input snapshots therefore list `occupiedIds`
across all observed kinds, alongside optional typed record fingerprints and
explicit coverage/evidence sources. Skipped table codecs, unobserved server
tables and other-owner reservations remain incomplete coverage; an unobserved
key must not be described as an allocated or proven-free key.

Requests use `sdgo:record-relocation-request/1` and snapshots use
`sdgo:record-key-snapshot/1`. Each source record has a stable key, native kind,
source ID, extent and SHA-256. A caller-selected allocation policy supplies
explicit inclusive ranges and whether to retain source IDs not observed in the
snapshot. The planner sorts source keys, retains eligible source IDs before
assigning range IDs, skips all occupied/requested keys and refuses exhaustion.
It never borrows an existing baseline record because the bytes look similar.

References name an exact source record key and verified uint32 field offset,
source value and target kind. Owned references name an exact target record key;
ambiguous duplicate source IDs are not implicit target selection. External
references retain their source ID and separately report typed identity/hash
observations. They cannot silently become a guessed substitute. Overlapping
fields, stale hashes, changed source bytes, wrong kinds and malformed contracts
are rejected before any generated output is returned.

The planner returns a JSON plan plus generated record copies. Only the identity
word and declared reference fields can change; every unknown byte is preserved.
Original blobs remain untouched. The plan binds both exact input hashes, source
and requested record hashes, every declared reference and all mutation spans.
`referenceInventoryComplete` and `unresolved` must honestly cover additional
script literals, ID-derived resources and fields not yet represented by the
adapter. A typed offset profile is evidence; scanning for matching numbers is not.

Every result unconditionally sets `idsReserved`, `allocationAuthorityIssued`,
`nativeRecordRegistrationAllowed` and `activationAllowed` to false. Even a
complete snapshot is not a live lock or reservation. The installation transaction
must acquire the authoritative owner/server lock, recheck/CAS the exact target
revision and conflicts, reserve the requested IDs, materialize all remaining
references and read back actual owner registrations/resources. Only that later
trusted state may implement a runtime resource-binding `Authority`.

Use `python -m unittest discover -s tests -p test_unit_relocation_plan.py -v`
for the neutral policy/reference/hash/conflict fixtures. They do not operate on
game assets, credentials, player records or a running native process.
