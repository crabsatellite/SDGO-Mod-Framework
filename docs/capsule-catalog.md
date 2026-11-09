# Capsule catalog editor

[简体中文](capsule-catalog.zh-CN.md) · [Back to the feature index](../README.md)

Change a machine’s name, price and pool weights, then save the configuration.

![Capsule editor: save machine names, prices and weights](capsule-editor-demo.webp)

Owners manage fixed and custom capsule machines through the capsule panel in SDGO Mod Manager.
Create, inspect, edit, retire, restore and reorder machines; choose native six-slot
templates; set prices and weights; add or remove custom candidates; preview unit
portraits; export and compile configuration; apply matching client and server data.
Players keep the game's native capsule screens and normal launcher.

Publishing a catalog also derives the encyclopedia's activity acquisition sources
from its active pools, including every unit reward in an activity machine.
Recompile and publish after adding, retiring or renumbering a machine; units do not
need to be imported again, and no extra linking Mod is required. Player-facing
numbers follow the native UI: internal ID `15401` is activity machine 154. Special
machines retain their names. Existing sources and blueprint fields are preserved;
unapplied activity drafts are not advertised as available sources.

`capsule_catalog` ships disabled until the owner supplies a complete generated
runtime. No game assets, player data, database credentials or machine-specific
paths are checked into this repository. Owners provide their own supported client
and server configuration. The packaged Manager includes its owner-tool dependencies. Source development uses
Python 3.10+, Pillow and pyodbc; SQL operations require an installed SQL Server ODBC
driver. Player installation does not require Python.

See the [owner walkthrough](capsule-authoring.zh-CN.md) for commands, source
inspection and initialization. All configuration uses the existing Manager session,
provider API and workspace; there is no separate capsule server or launcher.

The configuration is shared by two consumers:

| Consumer | Output and behavior |
|---|---|
| Client runtime | Verified overlays for six shared capsule GDTs, card sprites/textures and the exact capsule list price UI resource |
| Server adapter | Configuration SQL and Mod-owned prize selection; native charging, delivery and refund routes remain in use |

The same `catalog_revision` must reach both sides. An exported JSON alone is not
an installation. Applying checks the target, creates backups, installs client
files and commits server configuration; failures restore client files and roll
back SQL. Existing Mods and player history are preserved. Close the target game
before installation. The tool does not restart unrelated servers.
Applying a complete capsule package enables this feature's own switches, including
a disabled stub from an earlier framework installation; other Mod switches are retained.
The package also carries the framework loader, core and capsule programme so that a
client without the framework can run the catalog. A client that already runs the same
or a newer framework version keeps its own programmes (and the capsule symbol
definitions that belong to them) and receives only the catalog data; the apply result
and the backup receipt list them under `kept_programmes`. Older programmes are updated
as before.

For an A/B/C portrait, upload a finished PNG, drag the fixed-aspect crop frame,
resize its corners or use the zoom slider, and preview the native card. Save
either pose separately or share one image across both. Reset restores a pose's
original artwork. PNG bytes, hashes and crop coordinates travel inside the
catalog, so they do not reference a path on the owner's computer. The default
catalog has no uploaded overrides and uses original local client artwork. No
restored game artwork, game archives or AI models are published by this feature.

Give players the complete generated `player` directory, including its manifest
and `Install-Client.cmd`. Every catalog change requires matching generated client
data; updating only the framework DLL does not update prize pictures or slots.
If you ship players a complete client through your own updater, include that data
and its updated file manifest.
Read back the published payload, installed client `runtime.ini` and server
`sdgo_mod_CapsuleRuntimeState.CatalogRevision`: all must have the same
`catalog_revision`. Keep the owner workspace on that catalog too. Check first,
then install:

```powershell
.\Install-Client.cmd -GameDir "D:\Game" -CheckOnly
.\Install-Client.cmd -GameDir "D:\Game"
```

New fixed pools use independent weighted selection with six positive integer
weights totaling 1000. Existing unedited pools retain native sequence behavior.
Native templates carry both their grade order and price; an all-A machine is not
priced as a cheap mixed pool merely because it was newly generated.
Independent selection has no early-draw penalty, pity counter or consecutive
duplicate suppression. Native sequence behavior includes duplicate suppression;
copying its weights does not reproduce its draw order. See the
[distribution investigation](investigations/capsule-weighted-distribution.zh-CN.md).

Custom candidates preserve the source-bound native whitelist. Additional high-rank
candidates may be SR, or SS with no blueprint recipe record. Ordinary S and
recipe-bearing SS are excluded. The six base slots and two extension slots keep
their native selection constraints; adding candidates does not add SR slots.

Other Mods should use the [integration contract](capsule-integration.md). Native
card, list-position and price-display details are in the
[developer notes (Chinese)](capsule-display-fix.zh-CN.md).

New dedicated activity drafts follow the displayed numbers of existing ordinary
activity machines. Pinned numbers from 9900 upward are excluded from automatic
allocation. After saving an activity configuration, choose a displayed number
from 1 to 9899 in the Manager and save it, then compile and apply both sides.
Renumbering preserves the poster, price, rewards and native draw sequence;
it does not create a second machine or reset player progress. A machine with
existing player draw state cannot be renumbered, so choose its number before
making it available to players.
When the collection capsule is installed, its own resource adapter keeps that
machine pinned above authenticated imported activities. Unknown-provider rows
are preserved; compilation refuses to move a pinned row across them.
