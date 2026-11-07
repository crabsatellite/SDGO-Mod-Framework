"""Batch roster publisher for recovered G Union families.

The stages here turn a neutral recovery delivery plus a snapshot-bound record
relocation plan into one native unit catalog publication: family set, resource
closure with owner aliases, scoped native programs, catalog/overlay assembly
and server plan. Every stage writes JSON receipts; none of them reserves IDs,
installs into a client, writes SQL or claims native gameplay acceptance on
its own. Private delivery paths are supplied by the caller's configuration
file and never embedded here.
"""
