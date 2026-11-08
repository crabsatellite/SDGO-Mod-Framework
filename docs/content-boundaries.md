# Framework, owner content and local state

[简体中文](content-boundaries.zh-CN.md) · [Back to the feature index](../README.md) · [Manager](mod-manager.md) · [Owner publication](owner-updates.md)

## Design principles

Manager composes a portable content contract rather than requiring a particular complete-client repository or branch layout. Players obtain a compatible original client, import an owner profile and install its locked framework programs and owner content through one verified transaction.

The official catalog lists available Mods. An owner profile records the selected programs, exact versions, shared parameters and content digests. Players resolve that profile rather than independently selecting whatever version is newest. Initial installation and updates share compatibility checks, staging, recovery and commit semantics.

Manager updates itself from its own program upstream. Owners lock the game framework and Mod runtime files, so an older content package cannot downgrade a newer Manager. Shared content never overwrites Manager and does not affect its existing source bindings or download recovery.

| Ownership | Contents | Distribution |
| --- | --- | --- |
| Framework | Manager, core, Mod programs, interfaces, generic defaults, configuration contracts and SQL templates | Framework source and the appropriate program channel |
| Owner shared content | Mod selection/version lock, shared settings, added units, models, textures and generated catalogs | Owner-selected private Git, HTTPS or offline directory/package |
| Owner server definitions | Concrete unit, shop and reward records and generated server apply plans | Server maintenance workflow, never executed by a player profile |
| Private/runtime data | Credentials, tokens, birthday rosters, real accounts, inventory, currency, databases, logs and recovery state | Appropriate local/server private workspace, never player distribution |
| Original base and complete backup | Original programs/archives or a fully assembled snapshot | Obtained separately; saving a complete backup is optional |

A private distribution endpoint is not a credential or player-database export. The owner authoring workspace and its player-shareable subset are separate.

## SQL ownership

`distribution/artifact-ownership.json` declares every framework SQL file. Generic SQL may define tables, procedures and triggers or run parameterized queries and maintenance operations against the selected server. It is implementation code, not a dump of that server's records.

For example, a generic collection initialization query builds a local index from that server's existing inventory. The query is distributable framework code; its resulting account, unlock and inventory records are not framework or player content. The `owner/` source directory implements generic owner tools and is not a place for one server's private rows.

Concrete INSERT/UPDATE plans generated from owner unit, shop or reward edits are owner server definitions. They keep the server preview, maintenance, transaction and activation gates. Importing a player profile does not execute SQL or migrate databases. Real account, inventory and currency records are runtime data and never definition exports.

## Sources and installation

A profile records base-client compatibility, an exact framework version/digest, selected Mods, shared configuration and content hashes. Git, HTTPS manifests/resources and offline directories use the same profile contract and installer.

Framework identities, DLLs and interface declarations belong to the program source. Owner content changes declared settings and assets only. Login, display/FPS and explicitly declared personal preferences remain local; shared gameplay settings follow the owner. Source, digest or dependency failures preserve the existing installation.

An optional complete-client snapshot can be assembled from the same lockfile for backup or initial distribution. It is an output, not a separately maintained source of configuration or a requirement imposed on other owners.
