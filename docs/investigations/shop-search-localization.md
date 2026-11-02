# Shop search, filters and shared localization

The shop_localization feature supplies display translations, a bounded Chinese query transport and optional
localization for cooperating Mods. Static native analysis targets GOnline SHA-256 e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92
and current WorldServer SHA-256 82d3c4cf09679a431b9bfa639795b148f932642d8774ff66021785115dc53d46.
Saved decompilation was corroborated against current-file instructions; unavailable deep REA providers
were not presented as runtime traces.

## Search and filtering findings

GOnline search entry 0x009CD9E0 reads rank Combo +0xAC, payment +0xB0, keyword +0xB4 and button +0xB8.
The rank order All/S/A/B/C maps -1/3/2/1/0; payment All/GB/MB maps -1/1/2. Its CP874 conversion
at 0x009CDB79 destroys Chinese before sending native opcode 0x72D. Pagination conversion at
0x009BE804 uses the same query state. World copies at most 100 keyword bytes unchanged, binds
adChar size 101, and calls dbo.sp_StoreItemsList.

The original procedure applies rank/payment filtering to ordinary unit categories.
Global depth1=0 ignores rank, so S/A requests can return the same rows containing both ranks.
Hot depth1=99 ignores all optional filters. This is a
server predicate defect beyond display localization. Combo selection itself does not submit: the shop
callback ignores the Combo event 0x201 and the magnifier applies the selected values. No broad event hook
is introduced, because open/cancel/reset also emits that event.

The client transports Chinese/mixed keywords as the reserved ASCII ~CN: plus UTF-16LE Base64,
with a 36-unit/100-byte bound. Only the two verified CP874 shop keyword callers use this conversion;
ordinary English/Thai, other pages and numeric filter fields retain native behavior. The server-owned
search procedure decodes through SQL2014 XML, matches Unicode substrings against published names and
product-bound aliases, applies rank/subfilter/payment consistently and computes page results and total
from the same set. Native category/hot membership, event exclusion, 10 output columns and sort order are preserved.

Manager owns an independent Update Chinese search action. The helper accepts only exact product/search-scoped
word-bank entries, retains displayed model-code aliases, canonicalizes at SQL ProductID/original-name boundaries,
backs up the known native procedure and validates owned state before subsequent updates. Definitions
are checked with binary collation plus byte length under a transaction lock; unknown or externally
modified definitions refuse. Empty/mismatched aliases cannot silently commit an empty installation.
The helper never invokes catalog Apply or changes product rows, pricing, inventory or wallets.

## Text and cooperating Mods

Key coverage alone does not establish translation quality: mixed English prose can
remain despite source keys being present. The authored vocabulary revises 334 descriptions and seven name values,
adds Korean Jamo checks, and generates 17,265 entries including stable ID keys. Thai/Hangul/Jamo and
replacement-character targets are absent; legitimate technical/model identifiers remain explicit.
All 1071 hangar rule UnitIDs are mapped from 1073 available unit IDs. Six plain borrowed names are
localized, while seven actual model codes remain unchanged.

The optional SdgoLocalizationLookupV1 C ABI supplies UnitName, ItemName and fixed UiText to cooperating
Mods. Consumers discover the already-loaded provider, never load or require it, use caller-owned buffers,
and preserve source text when disabled/missing/unknown. Hangar uses the service only while formatting
preview names and rewards, leaving rules/identity/packets intact. Room-title integration translates only
fixed notices/caption; player titles, drafts and chat never enter the lookup. Name and interface scopes
remain separately configurable in the same Manager feature.

## Evidence boundaries

Actual DLL tests cover display callers, switches, dictionary parsing and C ABI output bounds; consumer
tests cover provider absence and later discovery. An independent SQL2014 temporary database checks
Unicode partial/English searches, global/hot filters, pagination, idempotence, backups, external-edit
refusal and unchanged product rows. The actual native helper is separately exercised using the exact
known native definition in a unique synthetic database. Every test database is removed. Private original
SQL, native records, credentials and raw local receipts remain outside source control.

These fixtures do not render actual game search results or execute player purchases/exchanges. Final
verification must distinguish installed hashes, server procedure readback, client load logs and manual
search/filter/confirmation visuals.

Billing names can differ from native display names through English/Thai rental prefixes, whitespace, legacy
spelling, model-code formatting or festival-key word ordering. Explicit @search:ProductID:Original
entries bind these current billing names to the already rendered titles; they never replace native display
records or product identities. English/model queries also consult the same guarded aliases.
